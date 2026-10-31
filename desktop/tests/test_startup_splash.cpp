#include "DesktopDisplayReadiness.h"
#include "StartupSplash.h"

#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QPalette>
#include <QSettings>
#include <QStringList>
#include <QTemporaryDir>
#include <QWidget>

#include <cmath>
#include <cstring>

#ifdef Q_OS_UNIX
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

int failures = 0;

void check(bool condition, const QString &what)
{
    if (condition) {
        qInfo("PASS: %s", qPrintable(what));
    } else {
        qCritical("FAIL: %s", qPrintable(what));
        ++failures;
    }
}

QtMessageHandler g_previousHandler = nullptr;
QStringList g_warnings;

void recordWarnings(QtMsgType type, const QMessageLogContext &context,
                    const QString &message)
{
    if (type == QtWarningMsg)
        g_warnings.append(message);
    if (g_previousHandler)
        g_previousHandler(type, context, message);
}

class FirstPaintHost : public QWidget
{
public:
    forkmesh::ui::StartupSplash *splash = nullptr;
    bool finishedSplash = false;

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QWidget::paintEvent(event);
        if (finishedSplash || !splash)
            return;
        finishedSplash = true;
        splash->noteHostPainted(QStringLiteral("Main window painted"));
        splash->finish(QStringLiteral("Workspace open"));
    }
};

bool colorsClose(const QColor &actual, const QColor &expected, int tolerance = 2)
{
    return std::abs(actual.red() - expected.red()) <= tolerance &&
           std::abs(actual.green() - expected.green()) <= tolerance &&
           std::abs(actual.blue() - expected.blue()) <= tolerance &&
           std::abs(actual.alpha() - expected.alpha()) <= tolerance;
}

void runDisplayReadinessTests()
{
    QProcessEnvironment environment;
    auto status = forkmesh::inspectDesktopDisplay(
        environment, [](const QString &) { return false; });
    check(status.state == forkmesh::DesktopDisplayState::Unavailable,
          QStringLiteral("startup rejects an unconfigured desktop display"));

    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("offscreen"));
    status = forkmesh::inspectDesktopDisplay(
        environment, [](const QString &) { return false; });
    check(status.state == forkmesh::DesktopDisplayState::Ready,
          QStringLiteral("display-independent Qt platforms bypass socket waiting"));

    environment = QProcessEnvironment();
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"),
                       QStringLiteral("wayland-0"));
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"),
                       QStringLiteral("/run/user/4242"));
    QString probedPath;
    status = forkmesh::inspectDesktopDisplay(
        environment, [&probedPath](const QString &path) {
            probedPath = path;
            return false;
        });
    check(status.state == forkmesh::DesktopDisplayState::WaitingForEndpoint &&
              probedPath == QLatin1String("/run/user/4242/wayland-0"),
          QStringLiteral("a missing Wayland socket delays QApplication construction"));

    int probes = 0;
    int sleptMs = 0;
    status = forkmesh::waitForDesktopDisplay(
        environment, 500, 100,
        [&probes](const QString &) { return ++probes >= 3; },
        [&sleptMs](int milliseconds) { sleptMs += milliseconds; });
    check(status.state == forkmesh::DesktopDisplayState::Ready &&
              status.waitedMs == 200 && sleptMs == 200,
          QStringLiteral("startup waits until the Wayland socket becomes ready"));

    // A systemd user service can inherit DISPLAY while its explicitly selected
    // Wayland endpoint is still being created. The stale X11 socket is not a
    // usable fallback yet: Xwayland has not published its authorization cookie.
    // Starting QApplication in that window makes both Qt plugins fail and abort.
    environment.insert(QStringLiteral("DISPLAY"), QStringLiteral(":0"));
    QStringList probedPaths;
    status = forkmesh::inspectDesktopDisplay(
        environment, [&probedPaths](const QString &path) {
            probedPaths.append(path);
            return path == QLatin1String("/tmp/.X11-unix/X0");
        });
    check(status.state == forkmesh::DesktopDisplayState::WaitingForEndpoint &&
              probedPaths == QStringList{QStringLiteral("/run/user/4242/wayland-0")},
          QStringLiteral("a stale X11 socket cannot bypass the selected Wayland wait"));

    int waylandProbes = 0;
    int x11Probes = 0;
    sleptMs = 0;
    status = forkmesh::waitForDesktopDisplay(
        environment, 500, 100,
        [&waylandProbes, &x11Probes](const QString &path) {
            if (path == QLatin1String("/run/user/4242/wayland-0"))
                return ++waylandProbes >= 3;
            ++x11Probes;
            return true;
        },
        [&sleptMs](int milliseconds) { sleptMs += milliseconds; });
    check(status.state == forkmesh::DesktopDisplayState::Ready &&
              status.waitedMs == 200 && sleptMs == 200 && x11Probes == 0,
          QStringLiteral("login startup waits for Wayland instead of stale X11"));

    environment = QProcessEnvironment();
    environment.insert(QStringLiteral("QT_QPA_PLATFORM"),
                       QStringLiteral("xcb"));
    environment.insert(QStringLiteral("DISPLAY"), QStringLiteral(":7.0"));
    probedPath.clear();
    status = forkmesh::inspectDesktopDisplay(
        environment, [&probedPath](const QString &path) {
            probedPath = path;
            return true;
        });
    check(status.state == forkmesh::DesktopDisplayState::Ready &&
              probedPath == QLatin1String("/tmp/.X11-unix/X7"),
          QStringLiteral("a local X11 display is mapped to its server socket"));
}

#ifdef Q_OS_UNIX
int bindUnixSocket(const QString &path, bool listening)
{
    const QByteArray native = QFile::encodeName(path);
    sockaddr_un address;
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, native.constData(), size_t(native.size()));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (::bind(fd, reinterpret_cast<const sockaddr *>(&address),
               sizeof(address)) != 0 ||
        (listening && ::listen(fd, 4) != 0)) {
        ::close(fd);
        return -1;
    }
    return fd;
}

void runDisplaySocketProbeTests()
{
    QTemporaryDir runtime(QDir::tempPath() + QStringLiteral("/fmwl-XXXXXX"));
    check(runtime.isValid(), QStringLiteral("socket probe runtime dir exists"));
    if (!runtime.isValid())
        return;
    const QString stalePath = runtime.filePath(QStringLiteral("wayland-0"));
    const int staleFd = bindUnixSocket(stalePath, false);
    check(staleFd >= 0, QStringLiteral("a compositor socket fixture is bound"));
    if (staleFd >= 0)
        ::close(staleFd);
    check(QFileInfo::exists(stalePath) &&
              !forkmesh::desktopEndpointAcceptsConnections(stalePath),
          QStringLiteral("a socket file left by a compositor that exited "
                         "refuses connections"));

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("XDG_SESSION_TYPE"),
                       QStringLiteral("wayland"));
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"),
                       QStringLiteral("wayland-0"));
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime.path());
    auto status = forkmesh::inspectDesktopDisplay(environment);
    check(status.state == forkmesh::DesktopDisplayState::WaitingForEndpoint,
          QStringLiteral("a stale Wayland socket after logout is not a ready "
                         "display"));

    const QString livePath = runtime.filePath(QStringLiteral("wayland-1"));
    const int liveFd = bindUnixSocket(livePath, true);
    check(liveFd >= 0, QStringLiteral("a listening compositor fixture is bound"));
    environment.insert(QStringLiteral("WAYLAND_DISPLAY"),
                       QStringLiteral("wayland-1"));
    status = forkmesh::inspectDesktopDisplay(environment);
    check(status.state == forkmesh::DesktopDisplayState::Ready,
          QStringLiteral("a listening Wayland socket is a ready display"));
    if (liveFd >= 0)
        ::close(liveFd);
}
#endif

} // namespace

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshTests"));
    QCoreApplication::setApplicationName(QStringLiteral("StartupSplash"));
    QSettings::setDefaultFormat(QSettings::IniFormat);

    QTemporaryDir settingsRoot;
    check(settingsRoot.isValid(),
          QStringLiteral("temporary settings directory is available"));
    if (!settingsRoot.isValid())
        return 1;
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsRoot.path());
    QSettings().clear();

    runDisplayReadinessTests();
#ifdef Q_OS_UNIX
    runDisplaySocketProbeTests();
#endif

    const QColor hostColor(180, 120, 60);
    QWidget host;
    QPalette hostPalette = host.palette();
    hostPalette.setColor(QPalette::Window, hostColor);
    host.setPalette(hostPalette);
    host.setAutoFillBackground(true);
    host.resize(900, 650);
    host.show();
    QApplication::processEvents();

    auto *splash = new forkmesh::ui::StartupSplash(
        true, QStringLiteral("test"), QStringLiteral("unknown"));
    splash->show();
    QApplication::processEvents();

    check(splash->isWindow(),
          QStringLiteral("splash starts as a top-level window"));
    check(splash->testAttribute(Qt::WA_TranslucentBackground),
          QStringLiteral("top-level splash requests compositor translucency"));

    splash->attachTo(&host);
    QApplication::processEvents();

    check(splash->parentWidget() == &host,
          QStringLiteral("splash is reparented into the main window"));
    check(!splash->isWindow(),
          QStringLiteral("attached splash is an ordinary child widget"));
    check(splash->geometry() == host.rect(),
          QStringLiteral("attached splash covers the host"));
    check(!splash->testAttribute(Qt::WA_TranslucentBackground),
          QStringLiteral("attached splash drops top-level translucency"));
    check(!splash->testAttribute(Qt::WA_NoSystemBackground),
          QStringLiteral("attached splash restores parent-content propagation"));

    QImage rendered(host.size(), QImage::Format_ARGB32_Premultiplied);
    rendered.fill(Qt::transparent);
    host.render(&rendered);

    constexpr int scrimAlpha = 130;
    const auto dimmed = [](int component) {
        return qRound(component * (255 - scrimAlpha) / 255.0);
    };
    const QColor expectedCorner(dimmed(hostColor.red()),
                                dimmed(hostColor.green()),
                                dimmed(hostColor.blue()), 255);
    const QColor actualCorner = rendered.pixelColor(16, 16);
    check(colorsClose(actualCorner, expectedCorner),
          QStringLiteral("child scrim composites over the host backing store"));

    delete splash;

    {
        FirstPaintHost firstPaint;
        firstPaint.resize(640, 480);
        auto *paintSplash = new forkmesh::ui::StartupSplash(
            true, QStringLiteral("test"), QStringLiteral("unknown"));
        paintSplash->show();
        QApplication::processEvents();
        paintSplash->attachTo(&firstPaint);
        firstPaint.splash = paintSplash;
        g_warnings.clear();
        g_previousHandler = qInstallMessageHandler(recordWarnings);
        firstPaint.show();
        for (int i = 0; i < 5 && !firstPaint.finishedSplash; ++i)
            QApplication::processEvents();
        firstPaint.repaint();
        QApplication::processEvents();
        qInstallMessageHandler(g_previousHandler);
        check(firstPaint.finishedSplash,
              QStringLiteral("the host's first paint finishes the splash"));
        check(!g_warnings.join(QLatin1Char('\n'))
                   .contains(QStringLiteral("Recursive repaint")),
              QStringLiteral("finishing the splash inside the host's first "
                             "paint queues its frame instead of repainting "
                             "recursively"));
        delete paintSplash;
    }
    return failures == 0 ? 0 : 1;
}
