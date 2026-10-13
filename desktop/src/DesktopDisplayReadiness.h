#pragma once

#include <QProcessEnvironment>
#include <QString>

#include <functional>

namespace forkmesh {

enum class DesktopDisplayState {
    Ready,
    WaitingForEndpoint,
    Unavailable,
};

struct DesktopDisplayReadiness {
    DesktopDisplayState state = DesktopDisplayState::Unavailable;
    QString detail;
    int waitedMs = 0;
};

using DesktopDisplayEndpointProbe = std::function<bool(const QString &path)>;
using DesktopDisplaySleeper = std::function<void(int milliseconds)>;

bool desktopEndpointAcceptsConnections(const QString &path);

DesktopDisplayReadiness inspectDesktopDisplay(
    const QProcessEnvironment &environment,
    DesktopDisplayEndpointProbe endpointProbe = {});

DesktopDisplayReadiness waitForDesktopDisplay(
    const QProcessEnvironment &environment, int timeoutMs, int pollIntervalMs,
    DesktopDisplayEndpointProbe endpointProbe = {},
    DesktopDisplaySleeper sleeper = {});

} // namespace forkmesh
