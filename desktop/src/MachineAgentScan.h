#pragma once

#include "SystemStats.h"

#include <QByteArrayList>
#include <QList>
#include <QSet>
#include <QString>

struct MachineAgent {
    qint64 pid = 0;
    QString kind;
    QString model;
    QString cwd;
    QString transcript;
    QString configDir;
    QString otherUser;
};

namespace MachineAgentScan {

QString agentKind(const QString &comm, const QByteArrayList &argv);
bool scriptStub(const QString &exePath);
bool cwdRemoved(const QString &linkTarget);
bool within(const QString &path, const QString &folder);
QString argValue(const QByteArrayList &argv, const QList<QByteArray> &names);
QString claudeProjectKey(const QString &cwd);
QString recordedClaudeSession(const QString &configDir, qint64 pid);
QString claudeTranscript(const QString &configDir, const QString &cwd,
                         const QString &sessionId, const QSet<QString> &claimed);
qint64 processUid(qint64 pid);
QString userName(qint64 uid);
bool codexRollout(const QString &path);
QString openCodexRollout(const SystemStats::ProcessTable &table, qint64 pid);
QList<qint64> outsideRoots(const SystemStats::ProcessTable &table,
                           const QHash<qint64, QString> &kinds,
                           const QSet<qint64> &ownRoots,
                           const QSet<qint64> &foreignRoots = {});
QList<MachineAgent> scan(const SystemStats::ProcessTable &table,
                         const QSet<qint64> &ownRoots,
                         const QSet<QString> &ownCwds,
                         const QSet<qint64> &foreignRoots = {});
QString label(const QString &kind);
QString provider(const QString &kind);

} // namespace MachineAgentScan
