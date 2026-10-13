#include "PingSyncState.h"

namespace forkmesh {

QString pingSyncLabel(PingSync state)
{
    switch (state) {
    case PingSync::LocalOnly:
        return QStringLiteral("Local only");
    case PingSync::Offline:
        return QStringLiteral("Offline");
    case PingSync::Pending:
        return QString::fromUtf8("Syncing\xE2\x80\xA6"); // …
    case PingSync::Synced:
        return QStringLiteral("Synced");
    case PingSync::Failed:
        return QStringLiteral("Not synced");
    }
    return QStringLiteral("Local only");
}

QString pingSyncDescription(PingSync state)
{
    switch (state) {
    case PingSync::LocalOnly:
        return QStringLiteral(
            "This event stays on this machine — it has no cloud destination.");
    case PingSync::Offline:
        return QStringLiteral(
            "Raised while this node was offline, so nothing was sent to the "
            "cloud. This page is the only record of it.");
    case PingSync::Pending:
        return QStringLiteral(
            "Queued for the relay; waiting for it to be acknowledged.");
    case PingSync::Synced:
        return QStringLiteral("The relay recorded this one.");
    case PingSync::Failed:
        return QStringLiteral(
            "This node was online but the relay never took it. This page is "
            "the only record of it.");
    }
    return {};
}

QString pingSyncToken(PingSync state)
{
    switch (state) {
    case PingSync::LocalOnly:
        return QStringLiteral("local");
    case PingSync::Offline:
        return QStringLiteral("offline");
    case PingSync::Pending:
        return QStringLiteral("pending");
    case PingSync::Synced:
        return QStringLiteral("synced");
    case PingSync::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("local");
}

PingSync pingSyncFromToken(const QString &token)
{
    const QString key = token.trimmed().toLower();
    if (key == QLatin1String("offline"))
        return PingSync::Offline;
    if (key == QLatin1String("pending"))
        return PingSync::Pending;
    if (key == QLatin1String("synced"))
        return PingSync::Synced;
    if (key == QLatin1String("failed"))
        return PingSync::Failed;
    return PingSync::LocalOnly;
}

bool pingSyncIsUnsynced(PingSync state)
{
    return state == PingSync::Offline || state == PingSync::Pending
           || state == PingSync::Failed;
}

PingSync pingSyncAfterRestart(PingSync stored)
{
    return stored == PingSync::Pending ? PingSync::Failed : stored;
}

QString pingSyncRestartReason()
{
    return QStringLiteral(
        "the app closed before the relay acknowledged it");
}

} // namespace forkmesh
