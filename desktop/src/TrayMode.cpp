#include "TrayMode.h"

#include "AppPaths.h"
#include "CoreClient.h"
#include "DesktopDisplayReadiness.h"
#include "SingleInstance.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QIcon>
#include <QLockFile>
#include <QMenu>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSystemTrayIcon>
#include <QTimer>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

#ifdef Q_OS_MACOS
#include <dlfcn.h>
#endif

namespace forkmesh::tray {

namespace {

constexpr int kPollMs = 2000;
constexpr qint64 kStartupGraceMs = 60000;
constexpr qint64 kGoneGraceMs = 30000;
constexpr qint64 kTrayWaitMs = 120000;
constexpr int kDisplayWaitMs = 30000;

bool pidAlive(qint64 pid)
{
#ifdef Q_OS_WIN
    if (pid <= 0)
        return false;
    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!process)
        return ::GetLastError() == ERROR_ACCESS_DENIED;
    DWORD code = 0;
    const bool running = ::GetExitCodeProcess(process, &code) && code == STILL_ACTIVE;
    ::CloseHandle(process);
    return running;
#else
    if (pid <= 1)
        return false;
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

bool desktopWindowRunning()
{
    qint64 pid = 0;
    QString host;
    QString application;
    return QLockFile(singleInstanceLockPath()).getLockInfo(&pid, &host, &application) && pidAlive(pid);
}

QString toolTip(const core::Endpoint &endpoint, const QString &project)
{
    QString text = QStringLiteral("ForkMesh background engine %1").arg(endpoint.version).trimmed();
    if (project != QLatin1String(paths::kHomeProjectSlug))
        text += QStringLiteral(" (%1)").arg(project);
    return text;
}

void hideFromDock()
{
#ifdef Q_OS_MACOS
    using Lookup = void *(*)(const char *);
    const auto getClass = reinterpret_cast<Lookup>(dlsym(RTLD_DEFAULT, "objc_getClass"));
    const auto selector = reinterpret_cast<Lookup>(dlsym(RTLD_DEFAULT, "sel_registerName"));
    void *send = dlsym(RTLD_DEFAULT, "objc_msgSend");
    if (!getClass || !selector || !send)
        return;
    void *app = reinterpret_cast<void *(*)(void *, void *)>(send)(getClass("NSApplication"),
                                                                  selector("sharedApplication"));
    if (app)
        reinterpret_cast<void (*)(void *, void *, long)>(send)(app, selector("setActivationPolicy:"), 1);
#endif
}

void startDetached(const QString &program, const QStringList &arguments)
{
    QProcess::startDetached(program, arguments, QDir::homePath());
}

} // namespace

QStringList arguments(const QString &project)
{
    return {QStringLiteral("--tray"), QStringLiteral("--project"), project};
}

bool launch()
{
    const QString platform = QGuiApplication::platformName();
    if (platform == QLatin1String("offscreen") || platform == QLatin1String("minimal"))
        return false;
    return QProcess::startDetached(QCoreApplication::applicationFilePath(),
                                   arguments(paths::projectSlug()), QDir::homePath());
}

int run(int argc, char *argv[])
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    if (waitForDesktopDisplay(QProcessEnvironment::systemEnvironment(), kDisplayWaitMs, 500).state !=
        DesktopDisplayState::Ready)
        return 0;
#endif
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("ForkMesh"));
    app.setOrganizationName(QStringLiteral("ForkMesh"));
    app.setQuitOnLastWindowClosed(false);
    paths::activate(app.arguments());
    hideFromDock();
    const QIcon appIcon(QStringLiteral(":/app/forkmesh.png"));
    app.setWindowIcon(appIcon);
    QGuiApplication::setDesktopFileName(QStringLiteral("forkmesh"));

    const QString project = paths::projectSlug();
    const QString stateDir = core::defaultStateDirectory();
    QDir().mkpath(stateDir);
    QLockFile lock(QDir(stateDir).filePath(QStringLiteral("tray.lock")));
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0))
        return 0;

    QSystemTrayIcon icon(appIcon);
    QMenu menu;
    QAction *openAction = menu.addAction(QStringLiteral("Open ForkMesh"));
    QAction *stopAction = menu.addAction(QStringLiteral("Stop background engine"));
    menu.addSeparator();
    QAction *hideAction = menu.addAction(QStringLiteral("Hide icon"));
    icon.setContextMenu(&menu);

    const auto openDesktop = [project] {
        startDetached(QCoreApplication::applicationFilePath(), {QStringLiteral("--project"), project});
    };
    QObject::connect(openAction, &QAction::triggered, &app, openDesktop);
    QObject::connect(stopAction, &QAction::triggered, &app, [project] {
        startDetached(core::defaultBinaryPath(), {QStringLiteral("stop"), QStringLiteral("--project"), project});
    });
    QObject::connect(hideAction, &QAction::triggered, &app, &QCoreApplication::quit);
    QObject::connect(&icon, &QSystemTrayIcon::activated, &app,
                     [openDesktop](QSystemTrayIcon::ActivationReason reason) {
                         if (reason == QSystemTrayIcon::Trigger)
                             openDesktop();
                     });

    QElapsedTimer started;
    started.start();
    QElapsedTimer lastSeen;
    bool trayFound = false;
    const auto poll = [&] {
        const core::Endpoint endpoint = core::readEndpoint(stateDir);
        const bool running = endpoint.pid > 0 && pidAlive(endpoint.pid);
        trayFound = trayFound || QSystemTrayIcon::isSystemTrayAvailable();
        if (!trayFound && started.elapsed() > kTrayWaitMs) {
            app.quit();
            return;
        }
        if (running) {
            lastSeen.start();
            if (desktopWindowRunning()) {
                if (icon.isVisible())
                    icon.hide();
                return;
            }
            const QString tip = toolTip(endpoint, project);
            if (icon.toolTip() != tip)
                icon.setToolTip(tip);
            if (trayFound && !icon.isVisible())
                icon.show();
            return;
        }
        if (icon.isVisible())
            icon.hide();
        const bool seen = lastSeen.isValid();
        if ((seen ? lastSeen.elapsed() : started.elapsed()) > (seen ? kGoneGraceMs : kStartupGraceMs))
            app.quit();
    };
    QTimer timer;
    timer.setInterval(kPollMs);
    QObject::connect(&timer, &QTimer::timeout, &app, poll);
    timer.start();
    QTimer::singleShot(0, &app, poll);
    return app.exec();
}

} // namespace forkmesh::tray
