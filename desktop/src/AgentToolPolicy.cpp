#include "AgentToolPolicy.h"

#include "AgentToolCatalog.h"

#include <QHash>
#include <QJsonArray>
#include <QRegularExpression>

#include <algorithm>

namespace {

// Claude Code tool names per family. Only names the CLI recognises appear
// here; families with no Claude tool are simply absent.
const QHash<QString, QStringList> &claudeToolNames()
{
    static const QHash<QString, QStringList> names = {
        {QStringLiteral("read"),
         {QStringLiteral("Read"), QStringLiteral("NotebookRead"),
          QStringLiteral("WebFetch")}},
        {QStringLiteral("write"), {QStringLiteral("Write")}},
        {QStringLiteral("edit"),
         {QStringLiteral("Edit"), QStringLiteral("MultiEdit"),
          QStringLiteral("NotebookEdit")}},
        {QStringLiteral("grep"), {QStringLiteral("Grep")}},
        {QStringLiteral("glob"), {QStringLiteral("Glob")}},
        {QStringLiteral("bash"),
         {QStringLiteral("Bash"), QStringLiteral("BashOutput"),
          QStringLiteral("KillShell"), QStringLiteral("KillBash")}},
        {QStringLiteral("task"), {QStringLiteral("Task"), QStringLiteral("Agent")}},
        {QStringLiteral("hub"),
         {QStringLiteral("SendMessage"), QStringLiteral("ListAgents"),
          QStringLiteral("TaskOutput"), QStringLiteral("TaskStop"),
          QStringLiteral("Monitor"), QStringLiteral("ScheduleWakeup"),
          QStringLiteral("CronCreate"), QStringLiteral("CronDelete"),
          QStringLiteral("CronList")}},
        {QStringLiteral("todo"),
         {QStringLiteral("TodoWrite"), QStringLiteral("TodoRead")}},
        {QStringLiteral("ask"), {QStringLiteral("AskUserQuestion")}},
        {QStringLiteral("web_search"), {QStringLiteral("WebSearch")}},
        {QStringLiteral("manage_skill"), {QStringLiteral("Skill")}},
        {QStringLiteral("computer"), {QStringLiteral("Computer")}},
    };
    return names;
}

// Codex config overrides per family. Both the older and newer spellings of
// the web-search switch are sent; Codex ignores keys it does not know.
const QHash<QString, QStringList> &codexOverrides()
{
    static const QHash<QString, QStringList> overrides = {
        {QStringLiteral("web_search"),
         {QStringLiteral("web_search=\"disabled\""),
          QStringLiteral("tools.web_search=false")}},
        {QStringLiteral("read"), {QStringLiteral("tools.view_image=false")}},
    };
    return overrides;
}

} // namespace

namespace AgentToolPolicy {

QStringList normalizeDisabledFamilies(const QStringList &raw)
{
    QStringList out;
    for (const QString &value : raw) {
        const QString id = value.trimmed().toLower();
        if (id.isEmpty() || !agentToolFamily(id) || out.contains(id))
            continue;
        out.append(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

QStringList disabledFamiliesFromOptions(const QJsonObject &options)
{
    QStringList raw;
    for (const QJsonValue &value : options.value(LaunchOption).toArray())
        if (value.isString())
            raw.append(value.toString());
    return normalizeDisabledFamilies(raw);
}

QJsonObject optionsWithDisabledFamilies(QJsonObject options,
                                        const QStringList &disabled)
{
    const QStringList normalized = normalizeDisabledFamilies(disabled);
    if (normalized.isEmpty())
        options.remove(LaunchOption);
    else
        options.insert(LaunchOption, QJsonArray::fromStringList(normalized));
    return options;
}

QStringList allowedFamiliesFromOptions(const QJsonObject &options, bool *active)
{
    const QJsonValue value = options.value(AllowlistOption);
    if (active)
        *active = value.isArray();
    QStringList raw;
    for (const QJsonValue &entry : value.toArray())
        if (entry.isString())
            raw.append(entry.toString());
    return normalizeDisabledFamilies(raw);
}

QJsonObject optionsWithAllowedFamilies(QJsonObject options, bool active,
                                       const QStringList &allowed)
{
    if (!active)
        options.remove(AllowlistOption);
    else
        options.insert(AllowlistOption,
                       QJsonArray::fromStringList(normalizeDisabledFamilies(allowed)));
    return options;
}

QStringList familiesOutsideAllowlist(const QStringList &allowedFamilies)
{
    const QStringList allowed = normalizeDisabledFamilies(allowedFamilies);
    QStringList outside;
    for (const QString &id : agentToolFamilyIds())
        if (!allowed.contains(id))
            outside.append(id);
    return normalizeDisabledFamilies(outside);
}

QStringList claudeAvailableTools(const QStringList &allowedFamilies,
                                 const QStringList &disabledFamilies)
{
    const QStringList disabled = normalizeDisabledFamilies(disabledFamilies);
    QStringList tools{QStringLiteral("ToolSearch"), QStringLiteral("EnterPlanMode"),
                      QStringLiteral("ExitPlanMode")};
    for (const QString &family : normalizeDisabledFamilies(allowedFamilies)) {
        if (disabled.contains(family))
            continue;
        for (const QString &name : claudeToolNames().value(family))
            if (!tools.contains(name))
                tools.append(name);
    }
    return tools;
}

QStringList claudeDisallowedTools(const QStringList &disabledFamilies)
{
    static const QRegularExpression safeName(
        QStringLiteral("^[A-Za-z][A-Za-z0-9_]*$"));
    QStringList tools;
    for (const QString &family : normalizeDisabledFamilies(disabledFamilies)) {
        for (const QString &name : claudeToolNames().value(family)) {
            if (safeName.match(name).hasMatch() && !tools.contains(name))
                tools.append(name);
        }
    }
    return tools;
}

QStringList codexConfigOverrides(const QStringList &disabledFamilies)
{
    QStringList overrides;
    for (const QString &family : normalizeDisabledFamilies(disabledFamilies)) {
        for (const QString &entry : codexOverrides().value(family))
            if (!overrides.contains(entry))
                overrides.append(entry);
    }
    return overrides;
}

bool deniesProcessExecution(const QStringList &disabledFamilies)
{
    return normalizeDisabledFamilies(disabledFamilies)
        .contains(QStringLiteral("bash"));
}

QString enforcementNote(const QString &family)
{
    QStringList parts;
    const QStringList claude = claudeToolNames().value(family);
    if (!claude.isEmpty())
        parts << QStringLiteral("Claude Code launches with --disallowedTools %1")
                     .arg(claude.join(QStringLiteral(", ")));
    const QStringList codex = codexOverrides().value(family);
    if (!codex.isEmpty())
        parts << QStringLiteral("Codex launches with -c %1")
                     .arg(codex.join(QStringLiteral(" -c ")));
    if (family == QLatin1String("bash"))
        parts << QStringLiteral("Codex shell approvals are denied by the "
                                "permission broker");
    if (parts.isEmpty())
        return QStringLiteral(
            "No runtime switch exists for this family yet; calls made while "
            "it is off are counted so the gap stays visible.");
    return parts.join(QStringLiteral("; ")) +
           QStringLiteral(". Takes effect on the next session launch or "
                          "resume.");
}

} // namespace AgentToolPolicy
