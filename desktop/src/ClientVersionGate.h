#pragma once

#include <QString>
#include <QVersionNumber>

// One rule for "this build is too old to be on the network", shared by the
// HTTP chokepoint (BackoffNetworkAccessManager::createRequest) and the
// WebSocket one (MainWindow::authorizeFirewallConnection) so a retired build
// is stopped everywhere rather than only where someone remembered to check.
namespace forkmesh {

// The oldest build allowed to talk to anything off this machine. Raising it
// retires every older build at once: instead of failing one call at a time
// against a relay that no longer answers them the way they expect, they stop
// sending and tell the reader to update.
inline constexpr char kMinimumNetworkVersion[] = "1.0.0";

inline QString minimumNetworkVersion()
{
    return QString::fromLatin1(kMinimumNetworkVersion);
}

// "v1.2.2" and "1.2.2-rc1" both parse: the release workflow stamps the tag
// into the binary, and a pre-release suffix still names its release number.
inline QVersionNumber clientVersionNumber(const QString &version)
{
    QString text = version.trimmed();
    if (text.startsWith(QLatin1Char('v')) || text.startsWith(QLatin1Char('V')))
        text = text.mid(1);
    return QVersionNumber::fromString(text);
}

// True only for a build that names a release number below the minimum. A
// build with no release number -- "dev", an empty string, a branch name -- is
// not older than anything, and blocking it would take the network away from
// everyone working on this app.
inline bool clientVersionTooOldForNetwork(
    const QString &version, const QString &minimum = minimumNetworkVersion())
{
    const QVersionNumber running = clientVersionNumber(version);
    const QVersionNumber floor = clientVersionNumber(minimum);
    if (running.isNull() || floor.isNull())
        return false;
    return QVersionNumber::compare(running, floor) < 0;
}

// What the refused reply, the log line and the popup all say, so a reader who
// meets one of them recognizes the other two.
inline QString outdatedClientSummary(
    const QString &version, const QString &minimum = minimumNetworkVersion())
{
    return QStringLiteral(
               "ForkMesh %1 is older than the minimum supported version %2")
        .arg(version.trimmed().isEmpty() ? QStringLiteral("(unknown build)")
                                         : version.trimmed(),
             minimum);
}

// How to get unblocked. The rebuild path runs git and the compiler as child
// processes, so it still works with every Qt request refused.
inline QString outdatedClientUpdateAdvice()
{
    return QStringLiteral(
        "Update ForkMesh to keep using the network: press Restart in the left "
        "rail for a rebuild from source, or install the latest release.");
}

} // namespace forkmesh
