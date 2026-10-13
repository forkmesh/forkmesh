#include "DesktopDisplayReadiness.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStringList>
#include <QThread>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace forkmesh {

bool desktopEndpointAcceptsConnections(const QString &path)
{
#ifdef Q_OS_UNIX
    const QByteArray native = QFile::encodeName(path);
    sockaddr_un address;
    std::memset(&address, 0, sizeof(address));
    if (native.isEmpty() || size_t(native.size()) >= sizeof(address.sun_path))
        return true;
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, native.constData(), size_t(native.size()));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return true;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0)
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    const int result = ::connect(fd, reinterpret_cast<const sockaddr *>(&address),
                                 sizeof(address));
    const int error = errno;
    ::close(fd);
    if (result == 0)
        return true;
    return error != ECONNREFUSED && error != ENOENT && error != ENOTSOCK;
#else
    Q_UNUSED(path);
    return true;
#endif
}

namespace {

bool defaultEndpointProbe(const QString &path)
{
    const QFileInfo endpoint(path);
    if (!endpoint.exists() || endpoint.isDir())
        return false;
    return desktopEndpointAcceptsConnections(path);
}

QString platformKey(QString value)
{
    value = value.trimmed().toLower();
    const int option = value.indexOf(QLatin1Char(':'));
    return option < 0 ? value : value.left(option);
}

bool isDisplayIndependentPlatform(const QString &platform)
{
    return platform == QLatin1String("offscreen") ||
           platform == QLatin1String("minimal") ||
           platform == QLatin1String("minimalegl") ||
           platform == QLatin1String("linuxfb") ||
           platform == QLatin1String("eglfs") ||
           platform == QLatin1String("vnc");
}

QString localX11Endpoint(const QString &display)
{
    QString local = display.trimmed();
    if (local.startsWith(QLatin1String("unix/")))
        local.remove(0, 5);
    else if (local.startsWith(QLatin1String("unix:")))
        local.remove(0, 4);

    static const QRegularExpression localDisplay(
        QStringLiteral("^:([0-9]+)(?:\\.[0-9]+)?$"));
    const QRegularExpressionMatch match = localDisplay.match(local);
    if (!match.hasMatch())
        return {};
    return QStringLiteral("/tmp/.X11-unix/X%1").arg(match.captured(1));
}

QString waylandEndpoint(const QProcessEnvironment &environment,
                        bool useDefault)
{
    QString display =
        environment.value(QStringLiteral("WAYLAND_DISPLAY")).trimmed();
    if (display.isEmpty() && useDefault)
        display = QStringLiteral("wayland-0");
    if (display.isEmpty())
        return {};
    if (QDir::isAbsolutePath(display))
        return QDir::cleanPath(display);
    const QString runtime =
        environment.value(QStringLiteral("XDG_RUNTIME_DIR")).trimmed();
    return runtime.isEmpty() ? QString() : QDir(runtime).filePath(display);
}

DesktopDisplayReadiness ready(QString detail)
{
    return {DesktopDisplayState::Ready, std::move(detail), 0};
}

} // namespace

DesktopDisplayReadiness inspectDesktopDisplay(
    const QProcessEnvironment &environment,
    DesktopDisplayEndpointProbe endpointProbe)
{
    if (!endpointProbe)
        endpointProbe = defaultEndpointProbe;

    const QString configuredPlatforms =
        environment.value(QStringLiteral("QT_QPA_PLATFORM")).trimmed();
    bool wantsWayland = false;
    bool wantsX11 = false;
    bool hasCustomPlatform = false;
    if (!configuredPlatforms.isEmpty()) {
        const QStringList candidates =
            configuredPlatforms.split(QLatin1Char(';'), Qt::SkipEmptyParts);
        for (const QString &candidate : candidates) {
            const QString platform = platformKey(candidate);
            if (isDisplayIndependentPlatform(platform))
                return ready(QStringLiteral("Qt platform %1 does not need a desktop socket")
                                 .arg(platform));
            if (platform.startsWith(QLatin1String("wayland")))
                wantsWayland = true;
            else if (platform == QLatin1String("xcb"))
                wantsX11 = true;
            else
                hasCustomPlatform = true;
        }
        // A project-specific QPA plugin may not use either standard socket.
        // Let Qt validate it instead of rejecting a configuration we cannot
        // accurately preflight.
        if (hasCustomPlatform)
            return ready(QStringLiteral("custom Qt platform selected"));
    } else {
        const QString sessionType =
            environment.value(QStringLiteral("XDG_SESSION_TYPE")).trimmed();
        const bool waylandSession =
            sessionType.compare(QStringLiteral("wayland"),
                                Qt::CaseInsensitive) == 0;
        const bool x11Session =
            sessionType.compare(QStringLiteral("x11"), Qt::CaseInsensitive) == 0;
        const bool hasWaylandDisplay =
            !environment.value(QStringLiteral("WAYLAND_DISPLAY")).trimmed().isEmpty();
        const bool hasX11Display =
            !environment.value(QStringLiteral("DISPLAY")).trimmed().isEmpty();

        // Qt tries the session's preferred QPA plugin first. Do not declare a
        // login ready merely because a fallback socket already exists: during
        // graphical-session startup X11's well-known socket can predate both
        // its authorization cookie and the preferred Wayland endpoint. Qt
        // then rejects both plugins and aborts in QApplication construction.
        wantsWayland = waylandSession || (!x11Session && hasWaylandDisplay);
        wantsX11 = !wantsWayland && hasX11Display;
    }

    QStringList endpoints;
    if (wantsWayland) {
        const QString endpoint = waylandEndpoint(
            environment, !configuredPlatforms.isEmpty() ||
                             environment.value(QStringLiteral("XDG_SESSION_TYPE"))
                                     .compare(QStringLiteral("wayland"),
                                              Qt::CaseInsensitive) == 0);
        if (!endpoint.isEmpty()) {
            if (endpointProbe(endpoint))
                return ready(QStringLiteral("Wayland endpoint is ready"));
            endpoints.append(endpoint);
        }
    }

    if (wantsX11) {
        const QString display =
            environment.value(QStringLiteral("DISPLAY")).trimmed();
        const QString endpoint = localX11Endpoint(display);
        if (endpoint.isEmpty()) {
            if (!display.isEmpty())
                return ready(QStringLiteral("non-local X11 display will be validated by Qt"));
        } else {
            if (endpointProbe(endpoint))
                return ready(QStringLiteral("X11 endpoint is ready"));
            endpoints.append(endpoint);
        }
    }

    if (!endpoints.isEmpty()) {
        return {DesktopDisplayState::WaitingForEndpoint,
                QStringLiteral("preferred desktop socket not ready: %1")
                    .arg(endpoints.join(QStringLiteral(", "))),
                0};
    }

    QString reason;
    if (wantsWayland)
        reason = QStringLiteral("Wayland was selected without a usable runtime endpoint");
    else if (wantsX11)
        reason = QStringLiteral("X11 was selected but DISPLAY is empty");
    else
        reason = QStringLiteral(
            "neither a desktop display nor a display-independent Qt platform "
            "is configured");
    return {DesktopDisplayState::Unavailable, reason, 0};
}

DesktopDisplayReadiness waitForDesktopDisplay(
    const QProcessEnvironment &environment, int timeoutMs, int pollIntervalMs,
    DesktopDisplayEndpointProbe endpointProbe, DesktopDisplaySleeper sleeper)
{
    timeoutMs = std::max(0, timeoutMs);
    pollIntervalMs = std::max(1, pollIntervalMs);
    if (!sleeper) {
        sleeper = [](int milliseconds) {
            QThread::msleep(static_cast<unsigned long>(milliseconds));
        };
    }

    DesktopDisplayReadiness status =
        inspectDesktopDisplay(environment, endpointProbe);
    while (status.state == DesktopDisplayState::WaitingForEndpoint &&
           status.waitedMs < timeoutMs) {
        const int delay = std::min(pollIntervalMs, timeoutMs - status.waitedMs);
        sleeper(delay);
        const int waitedMs = status.waitedMs + delay;
        status = inspectDesktopDisplay(environment, endpointProbe);
        status.waitedMs = waitedMs;
    }
    return status;
}

} // namespace forkmesh
