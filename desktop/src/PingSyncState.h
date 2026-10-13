#pragma once

#include <QString>

// Offline reports are local-only; pending reports that survive a restart become
// failed rather than appearing delivered. MainWindow owns the network state.
namespace forkmesh {

enum class PingSync {
    LocalOnly,
    Offline,
    Pending,
    Synced,
    Failed,
};

QString pingSyncLabel(PingSync state);

QString pingSyncDescription(PingSync state);

QString pingSyncToken(PingSync state);
PingSync pingSyncFromToken(const QString &token);

bool pingSyncIsUnsynced(PingSync state);

PingSync pingSyncAfterRestart(PingSync stored);

QString pingSyncRestartReason();

} // namespace forkmesh
