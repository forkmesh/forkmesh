#include "AgentRunRules.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace forkmesh::agentrules {

namespace {

using Access = forkmesh::projects::AgentAccess;
using Destructive = forkmesh::projects::GlobalAgentRules::Destructive;

QString shownTarget(const QString &target)
{
    return target == QLatin1String("web-search") ? QStringLiteral("web search") : target;
}

Decision verdict(Verdict kind, const QString &reason, const QString &runKey = QString())
{
    Decision decision;
    decision.verdict = kind;
    decision.reason = reason;
    decision.runKey = runKey;
    return decision;
}

} // namespace

bool gatesCommands(const RunRules &rules)
{
    return forkmesh::projects::gatesExternalCalls(rules.access) ||
           rules.destructive != Destructive::Allow || rules.externalBlocked ||
           !rules.deniedPermissions.isEmpty();
}

Decision decide(const RunRules &rules, const forkmesh::agentnet::ExternalCall &call,
                const forkmesh::agentnet::CommandRisk &risk, const QSet<QString> &runApproved)
{
    const QString who = rules.teammateName.isEmpty() ? QStringLiteral("This run")
                                                     : rules.teammateName;
    auto finish = [&call](Decision decision) {
        decision.external = call.external;
        decision.target = call.target;
        return decision;
    };
    if (!risk.permission.isEmpty() && rules.deniedPermissions.contains(risk.permission))
        return finish(verdict(
            Verdict::Deny,
            QStringLiteral("%1 does not have the “%2” permission this command needs. "
                           "Do not try another way; leave it for the user.")
                .arg(who, risk.permission)));
    if (!risk.destructive.isEmpty()) {
        const QString key = QStringLiteral("destructive:") + risk.destructive;
        if (rules.destructive == Destructive::Block)
            return finish(verdict(
                Verdict::Deny,
                QStringLiteral("The global agent rules block destructive commands (%1). Do not "
                               "try another way; ask the user to run it themselves.")
                    .arg(risk.destructive)));
        if (rules.destructive == Destructive::Ask && !runApproved.contains(key) &&
            !rules.access.allowedDestructive.contains(risk.destructive) &&
            !(risk.onlyRemoves &&
              forkmesh::commandvars::deletesAllowed(rules.access.allowedDeletePaths,
                                                    risk.removedPaths, risk.scratch)))
            return finish(verdict(
                Verdict::Ask,
                QStringLiteral("A destructive command (%1) waits for your approval.")
                    .arg(risk.destructive),
                key));
    }
    if (!call.external)
        return finish(verdict(Verdict::Run, QString()));
    const QString shown = shownTarget(call.target);
    const QString host = forkmesh::projects::normalizeHost(call.target);
    if (!host.isEmpty() && forkmesh::projects::hostBlocked(rules.access.blockedHosts, host))
        return finish(verdict(
            Verdict::Deny,
            QStringLiteral("%1 is on the global agent rules' blocked list. Do not try another "
                           "way to reach it.")
                .arg(shown)));
    if ((!host.isEmpty() && forkmesh::projects::hostAllowed(rules.access.allowedHosts, host)) ||
        runApproved.contains(call.target))
        return finish(verdict(Verdict::Run, QString()));
    if (rules.externalBlocked)
        return finish(verdict(
            Verdict::Deny,
            QStringLiteral("%1 does not have the “external-tools” permission, so it "
                           "cannot reach %2. Do not try another way to reach it.")
                .arg(who, shown)));
    if (rules.access.network == Access::Network::Allow)
        return finish(verdict(Verdict::Run, QString()));
    if (rules.access.network == Access::Network::Block)
        return finish(verdict(
            Verdict::Deny,
            QStringLiteral("This project blocks external calls, and %1 is not one of its "
                           "approved destinations. Ask the user to approve it in the "
                           "project's settings; do not try another way to reach it.")
                .arg(shown)));
    return finish(verdict(Verdict::Ask,
                          QStringLiteral("Reaches %1; external calls wait for your approval.")
                              .arg(shown),
                          call.target));
}

Decision decideCommand(const RunRules &rules, const QString &command, const QString &cwd,
                       const QSet<QString> &runApproved)
{
    forkmesh::commandvars::Environment environment;
    environment.values.insert(QStringLiteral("HOME"), QDir::homePath());
    return decide(rules, forkmesh::agentnet::classifyCommand(command, cwd),
                  forkmesh::agentnet::classifyCommandRisk(command, environment), runApproved);
}

QStringList enforcedTeammatePermissions()
{
    return {QStringLiteral("create-branches"), QStringLiteral("review-prs"),
            QStringLiteral("manage-issues"), QStringLiteral("merge"),
            QStringLiteral("external-tools")};
}

QStringList deniedTeammatePermissions(const QStringList &granted)
{
    QStringList denied;
    for (const QString &permission : enforcedTeammatePermissions())
        if (!granted.contains(permission))
            denied.append(permission);
    return denied;
}

QStringList teammateWorkPermissions(const QStringList &granted, bool ownWorktree)
{
    QStringList permissions = granted;
    if (!ownWorktree)
        permissions.removeAll(QStringLiteral("create-branches"));
    return permissions;
}

QString teammateRunMode(const QString &mode, const QStringList &granted,
                        const QString &autoLabel, const QString &editLabel,
                        const QString &planLabel)
{
    const bool unattended = granted.contains(QStringLiteral("run-unattended"));
    if (!unattended && !granted.contains(QStringLiteral("modify-code")))
        return planLabel;
    const QString trimmed = mode.trimmed();
    if (trimmed.isEmpty())
        return unattended ? autoLabel : editLabel;
    if (!unattended && trimmed.compare(autoLabel, Qt::CaseInsensitive) == 0)
        return editLabel;
    return trimmed;
}

QStringList teammateDisallowedTools(const QStringList &deniedPermissions)
{
    QStringList tools;
    if (deniedPermissions.contains(QStringLiteral("manage-issues")))
        for (const char *name : {"create_issue", "comment_on_issue", "create_milestone",
                                 "update_milestone", "create_project", "update_project"})
            tools << QStringLiteral("mcp__forkmesh__") + QLatin1String(name);
    if (deniedPermissions.contains(QStringLiteral("create-branches")))
        tools << QStringLiteral("mcp__forkmesh__open_pr_from_branch");
    if (deniedPermissions.contains(QStringLiteral("external-tools")))
        tools << QStringLiteral("WebFetch") << QStringLiteral("WebSearch");
    return tools;
}

QStringList teammateAllowedToolRules(const QStringList &granted)
{
    QStringList rules;
    if (granted.contains(QStringLiteral("run-git")))
        rules << QStringLiteral("Bash(git:*)");
    if (granted.contains(QStringLiteral("run-tests")))
        rules << QStringLiteral("Bash(python3 -m pytest:*)") << QStringLiteral("Bash(pytest:*)")
              << QStringLiteral("Bash(node:*)");
    if (granted.contains(QStringLiteral("read-forkmesh-web"))) {
        for (const char *host : {"forkmesh.com", "www.forkmesh.com", "app.forkmesh.com",
                                 "world.forkmesh.com"}) {
            rules << QStringLiteral("WebFetch(domain:%1)").arg(QLatin1String(host));
            rules << QStringLiteral("Bash(curl -s https://%1/:*)").arg(QLatin1String(host));
        }
    }
    return rules;
}

QStringList teammateAllowedHosts(const QStringList &granted)
{
    if (granted.contains(QStringLiteral("read-forkmesh-web")))
        return {QStringLiteral("*.forkmesh.com")};
    return {};
}

namespace {

QString claudeAbsoluteRulePath(const QString &path)
{
    if (path.trimmed().isEmpty())
        return {};
    QString posix = QDir::cleanPath(QString(path).replace(QLatin1Char('\\'), QLatin1Char('/')));
    if (posix.size() >= 2 && posix.at(1) == QLatin1Char(':') && posix.at(0).isLetter())
        posix = QLatin1Char('/') + posix.at(0).toLower() + posix.mid(2);
    if (!posix.startsWith(QLatin1Char('/')) || posix == QLatin1String("/"))
        return {};
    return QLatin1Char('/') + posix;
}

} // namespace

QStringList appLogReadRules(const QString &logPath, const QString &archiveDir)
{
    QStringList rules;
    const QString log = claudeAbsoluteRulePath(logPath);
    if (!log.isEmpty())
        rules << QStringLiteral("Read(%1)").arg(log);
    const QString archive = claudeAbsoluteRulePath(archiveDir);
    if (!archive.isEmpty())
        rules << QStringLiteral("Read(%1/**)").arg(archive);
    return rules;
}

QString claudeSettingsWithAllowRules(const QString &settings, const QStringList &allowRules)
{
    static const QRegularExpression safeRule(
        QStringLiteral("^[A-Za-z][A-Za-z0-9_]*(\\([A-Za-z0-9 ._:/*=-]+\\))?$"));
    QStringList rules;
    for (const QString &rule : allowRules)
        if (safeRule.match(rule).hasMatch() && !rules.contains(rule))
            rules << rule;
    if (rules.isEmpty())
        return settings;
    QJsonObject object = QJsonDocument::fromJson(settings.toUtf8()).object();
    QJsonObject permissions = object.value(QStringLiteral("permissions")).toObject();
    QJsonArray allow = permissions.value(QStringLiteral("allow")).toArray();
    for (const QString &rule : rules)
        if (!allow.contains(rule))
            allow.append(rule);
    permissions.insert(QStringLiteral("allow"), allow);
    object.insert(QStringLiteral("permissions"), permissions);
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

QString readGuardHookScript()
{
    return QStringLiteral(R"PY(import json, os, sys

LIMIT = %1
SKIP = (".png", ".jpg", ".jpeg", ".gif", ".webp", ".bmp", ".ico", ".pdf", ".ipynb")

def main():
    try:
        event = json.load(sys.stdin)
    except Exception:
        return 0
    data = event.get("tool_input")
    if not isinstance(data, dict):
        return 0
    if data.get("offset") or data.get("limit") or data.get("pages"):
        return 0
    path = str(data.get("file_path") or "")
    if not path or path.lower().endswith(SKIP):
        return 0
    if not os.path.isabs(path):
        path = os.path.join(str(event.get("cwd") or ""), path)
    try:
        size = os.path.getsize(path)
    except OSError:
        return 0
    if size <= LIMIT:
        return 0
    lines = 0
    try:
        with open(path, "rb") as handle:
            head = handle.read(8192)
            if b"\0" in head:
                return 0
            lines = head.count(b"\n")
            while True:
                chunk = handle.read(1 << 20)
                if not chunk:
                    break
                lines += chunk.count(b"\n")
    except OSError:
        return 0
    lines += 1
    reason = ("%s is %d lines (about %dK tokens), too large to read whole. "
              "Grep -n the symbol or its headings first, then Read about 300 lines "
              "around the match with offset and limit. If you need every line, "
              "Read it with offset 1 and limit %d."
              % (os.path.basename(path), lines, max(1, size // 4000), lines))
    print(json.dumps({"hookSpecificOutput": {
        "hookEventName": "PreToolUse",
        "permissionDecision": "deny",
        "permissionDecisionReason": reason}}))
    return 0

sys.exit(main())
)PY")
        .arg(kReadGuardBytes);
}

QString claudeSettingsWithReadGuard(const QString &settings)
{
    const QString encoded = QString::fromLatin1(readGuardHookScript().toUtf8().toBase64());
    const QString command =
        QStringLiteral("python3 -c \"import base64,sys;exec(base64.b64decode(sys.argv[1]))\" %1")
            .arg(encoded);
    QJsonObject object = QJsonDocument::fromJson(settings.toUtf8()).object();
    QJsonObject hooks = object.value(QStringLiteral("hooks")).toObject();
    QJsonArray pre = hooks.value(QStringLiteral("PreToolUse")).toArray();
    for (const QJsonValue &entry : std::as_const(pre)) {
        const QJsonArray commands = entry.toObject().value(QStringLiteral("hooks")).toArray();
        for (const QJsonValue &hook : commands)
            if (hook.toObject().value(QStringLiteral("command")).toString() == command)
                return settings;
    }
    pre.append(QJsonObject{
        {QStringLiteral("matcher"), QStringLiteral("Read")},
        {QStringLiteral("hooks"),
         QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("command")},
                                {QStringLiteral("command"), command},
                                {QStringLiteral("timeout"), 10}}}}});
    hooks.insert(QStringLiteral("PreToolUse"), pre);
    object.insert(QStringLiteral("hooks"), hooks);
    return QString::fromUtf8(QJsonDocument(object).toJson(QJsonDocument::Compact));
}

} // namespace forkmesh::agentrules
