#pragma once

#include "AgentNetworkGate.h"
#include "ProjectAgentAccess.h"

#include <QSet>
#include <QString>
#include <QStringList>

namespace forkmesh::agentrules {

struct RunRules {
    forkmesh::projects::AgentAccess access;
    forkmesh::projects::GlobalAgentRules::Destructive destructive =
        forkmesh::projects::GlobalAgentRules::Destructive::Ask;
    bool externalBlocked = false;
    QStringList deniedPermissions;
    QString teammateName;
    QStringList allowedToolRules;
};

enum class Verdict { Run, Ask, Deny };

struct Decision {
    Verdict verdict = Verdict::Run;
    QString reason;
    QString runKey;
    bool external = false;
    QString target;
};

bool gatesCommands(const RunRules &rules);

Decision decide(const RunRules &rules, const forkmesh::agentnet::ExternalCall &call,
                const forkmesh::agentnet::CommandRisk &risk, const QSet<QString> &runApproved);
Decision decideCommand(const RunRules &rules, const QString &command, const QString &cwd,
                       const QSet<QString> &runApproved);

QStringList enforcedTeammatePermissions();
QStringList deniedTeammatePermissions(const QStringList &granted);
QStringList teammateWorkPermissions(const QStringList &granted, bool ownWorktree);
QString teammateRunMode(const QString &mode, const QStringList &granted,
                        const QString &autoLabel, const QString &editLabel,
                        const QString &planLabel);
QStringList teammateDisallowedTools(const QStringList &deniedPermissions);
QStringList teammateAllowedToolRules(const QStringList &granted);
QStringList teammateAllowedHosts(const QStringList &granted);
inline const QString kReadAppLogsGrant = QStringLiteral("read-app-logs");
QStringList appLogReadRules(const QString &logPath, const QString &archiveDir);
QString claudeSettingsWithAllowRules(const QString &settings, const QStringList &allowRules);
inline constexpr qint64 kReadGuardBytes = 32 * 1024;
QString readGuardHookScript();
QString claudeSettingsWithReadGuard(const QString &settings);

} // namespace forkmesh::agentrules
