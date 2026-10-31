#include "WorkflowsModel.h"

#include <QApplication>
#include <QStandardPaths>
#include <QDateTime>
#include <QJsonDocument>

using namespace forkmesh::workflows;

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition) {
        qInfo("PASS: %s", qPrintable(message));
    } else {
        qCritical("FAIL: %s", qPrintable(message));
        ++failures;
    }
}

QJsonObject parse(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

Catalog catalog()
{
    Catalog result;
    result.load(parse(R"json({
      "core": [
        {"type": "core.manual", "name": "Manual trigger", "group": "Triggers", "trigger": true,
         "outputs": ["main"], "description": "Start with Run.", "fields": []},
        {"type": "core.schedule", "name": "Schedule", "group": "Triggers", "trigger": true,
         "outputs": ["main"], "description": "Every few minutes.",
         "fields": [{"key": "every", "label": "Every (minutes)", "type": "number", "default": "60"}]},
        {"type": "core.http", "name": "HTTP request", "group": "Core", "outputs": ["main"],
         "description": "Call any HTTPS API.",
         "fields": [{"key": "method", "type": "select", "default": "GET"},
                    {"key": "url", "type": "string"}]},
        {"type": "core.if", "name": "If", "group": "Flow", "outputs": ["true", "false"],
         "description": "Branch.", "fields": [{"key": "operator", "type": "select", "default": "equals"}]},
        {"type": "core.noop", "name": "No operation", "group": "Flow", "outputs": ["main"],
         "description": "Pass items through unchanged.", "fields": []},
        {"type": "desktop.diskUsage", "name": "Measure the disk", "group": "Desktop", "outputs": ["main"],
         "runsOn": "desktop", "desktopStep": "usage", "description": "Reads free space.", "fields": []}
      ],
      "integrations": [
        {"key": "slack", "type": "app.slack", "name": "Slack", "category": "Communication",
         "description": "Chat.", "credential": [{"key": "token", "label": "Bot token", "secret": true}],
         "operations": [{"key": "postMessage", "name": "Send a message", "method": "POST",
                         "fields": [{"key": "channel", "type": "string", "default": "#general"}]},
                        {"key": "listChannels", "name": "List channels", "method": "GET", "fields": []}]},
        {"key": "public", "type": "app.public", "name": "Public API", "category": "Utilities",
         "description": "No sign in.", "credential": [], "operations": []}
      ],
      "operators": [{"value": "equals", "label": "is equal to"}]
    })json"));
    return result;
}

void catalogCases()
{
    const Catalog types = catalog();
    check(types.isTrigger(QStringLiteral("core.manual")) && !types.isTrigger(QStringLiteral("core.http")),
          QStringLiteral("triggers are told apart from steps"));
    check(types.triggerTypes() == QStringList{QStringLiteral("core.manual"), QStringLiteral("core.schedule")},
          QStringLiteral("trigger types keep catalog order"));
    check(types.outputs(QStringLiteral("core.if")) == QStringList{QStringLiteral("true"), QStringLiteral("false")},
          QStringLiteral("If has true and false branches"));
    check(types.outputs(QStringLiteral("app.slack")) == QStringList{QStringLiteral("main")},
          QStringLiteral("integrations have one main output"));
    check(types.isDesktop(QStringLiteral("desktop.diskUsage")) &&
              types.desktopStep(QStringLiteral("desktop.diskUsage")) == QLatin1String("usage"),
          QStringLiteral("desktop steps carry their script step"));
    check(types.fields(QStringLiteral("app.slack"), QStringLiteral("postMessage")).size() == 1 &&
              types.fields(QStringLiteral("app.slack"), QStringLiteral("listChannels")).isEmpty(),
          QStringLiteral("integration fields follow the operation"));
    check(types.integrationKeys() == QStringList{QStringLiteral("slack")},
          QStringLiteral("only integrations with credential fields can hold credentials"));
    check(types.group(QStringLiteral("app.slack")) == QLatin1String("Communication"),
          QStringLiteral("integrations are grouped by category"));
    QJsonObject http{{QStringLiteral("type"), QStringLiteral("core.http")},
                     {QStringLiteral("params"), QJsonObject{{QStringLiteral("method"), QStringLiteral("POST")},
                                                            {QStringLiteral("url"), QStringLiteral("https://x.test")}}}};
    check(types.summary(http) == QLatin1String("POST https://x.test"), QStringLiteral("HTTP summary shows method and URL"));
    http.insert(QStringLiteral("disabled"), true);
    check(types.summary(http).endsWith(QStringLiteral(" · Off")), QStringLiteral("a turned-off step says Off"));
    const QJsonObject schedule{{QStringLiteral("type"), QStringLiteral("core.schedule")},
                               {QStringLiteral("params"), QJsonObject{{QStringLiteral("every"), QStringLiteral("120")}}}};
    check(types.summary(schedule) == QLatin1String("Every 2 hours"), QStringLiteral("schedule summary reads the interval"));
}

void draftCases()
{
    const Catalog types = catalog();
    Draft draft = Draft::blank();
    check(draft.nodes.size() == 1 && draft.trigger(types).value(QStringLiteral("type")).toString() ==
                                         QLatin1String("core.manual"),
          QStringLiteral("a blank workflow holds one Manual trigger"));
    const QString http = draft.addStep(types, QStringLiteral("core.http"), QStringLiteral("trigger"));
    check(http == QLatin1String("http1"), QStringLiteral("new step ids come from the type"));
    const QJsonObject step = draft.node(http);
    check(step.value(QStringLiteral("x")).toInt() == kColumn && step.value(QStringLiteral("y")).toInt() == 0,
          QStringLiteral("a new step sits one column right of its parent"));
    check(step.value(QStringLiteral("params")).toObject().value(QStringLiteral("method")).toString() ==
              QLatin1String("GET"),
          QStringLiteral("a new step takes its field defaults"));
    check(draft.inputsText(http) == QLatin1String("After Run manually"), QStringLiteral("the new step runs after its parent"));

    const QString branch = draft.addStep(types, QStringLiteral("core.if"), http);
    const QString yes = draft.addStep(types, QStringLiteral("core.noop"), branch, QStringLiteral("true"));
    const QString no = draft.addStep(types, QStringLiteral("core.noop"), branch, QStringLiteral("false"));
    check(draft.node(no).value(QStringLiteral("name")).toString() == QLatin1String("No operation 2"),
          QStringLiteral("step names stay unique"));
    check(draft.node(no).value(QStringLiteral("y")).toInt() == kRow, QStringLiteral("a second child goes one row down"));
    check(draft.inputsText(yes) == QLatin1String("If: true"), QStringLiteral("branch inputs name the branch"));
    check(draft.ordered() == QStringList{QStringLiteral("trigger"), http, branch, yes, no},
          QStringLiteral("steps are listed in run order"));

    const QString slack = draft.addStep(types, QStringLiteral("app.slack"), yes);
    const QJsonObject params = draft.node(slack).value(QStringLiteral("params")).toObject();
    check(params.value(QStringLiteral("operation")).toString() == QLatin1String("postMessage") &&
              params.value(QStringLiteral("channel")).toString() == QLatin1String("#general"),
          QStringLiteral("an integration starts on its first operation with its defaults"));

    check(!draft.canConnect(types, slack, QStringLiteral("main"), http),
          QStringLiteral("a connection that would make a loop is refused"));
    check(!draft.canConnect(types, http, QStringLiteral("main"), QStringLiteral("trigger")),
          QStringLiteral("nothing connects into the trigger"));
    check(!draft.canConnect(types, branch, QStringLiteral("main"), no),
          QStringLiteral("a branch the step does not have is refused"));
    check(draft.connectSteps(types, no, QStringLiteral("main"), slack) && draft.inputs(slack).size() == 2,
          QStringLiteral("a step can take a second input"));
    const int links = int(draft.connections.size());
    check(draft.connectSteps(types, no, QStringLiteral("main"), slack) && draft.connections.size() == links,
          QStringLiteral("connecting twice keeps one connection"));
    draft.disconnectSteps(no, QStringLiteral("main"), slack);
    check(draft.inputs(slack).size() == 1, QStringLiteral("disconnecting removes only that input"));

    draft.remove(branch);
    check(draft.indexOf(branch) < 0 && draft.inputsText(yes) == QLatin1String("After HTTP request") &&
              draft.inputsText(no) == QLatin1String("After HTTP request"),
          QStringLiteral("removing a step joins its parents to its children"));

    draft.setTrigger(types, QStringLiteral("core.schedule"));
    const QJsonObject trigger = draft.trigger(types);
    check(trigger.value(QStringLiteral("id")).toString() == QLatin1String("trigger") &&
              trigger.value(QStringLiteral("type")).toString() == QLatin1String("core.schedule") &&
              trigger.value(QStringLiteral("params")).toObject().value(QStringLiteral("every")).toString() ==
                  QLatin1String("60"),
          QStringLiteral("changing the trigger keeps its id and connections"));
    check(draft.inputsText(http) == QLatin1String("After Schedule"), QStringLiteral("the renamed trigger still feeds its step"));

    Draft loose;
    loose.nodes.append(QJsonObject{{QStringLiteral("id"), QStringLiteral("a")},
                                   {QStringLiteral("type"), QStringLiteral("core.noop")},
                                   {QStringLiteral("name"), QStringLiteral("A")},
                                   {QStringLiteral("x"), 300},
                                   {QStringLiteral("y"), 40}});
    loose.setTrigger(types, QStringLiteral("core.manual"));
    check(loose.nodes.first().toObject().value(QStringLiteral("x")).toInt() == 300 - kColumn &&
              loose.inputsText(QStringLiteral("a")) == QLatin1String("After Manual trigger"),
          QStringLiteral("a new trigger goes before the unconnected steps and feeds them"));

    const QJsonObject body = draft.body();
    check(body.value(QStringLiteral("active")).isBool() && body.value(QStringLiteral("nodes")).toArray().size() ==
                                                               draft.nodes.size(),
          QStringLiteral("the save body carries the graph and the Active flag"));
    const Draft again = Draft::fromJson(QJsonObject{{QStringLiteral("id"), QStringLiteral("abc")},
                                                    {QStringLiteral("name"), QString()},
                                                    {QStringLiteral("nodes"), 3},
                                                    {QStringLiteral("hookPath"), QStringLiteral("/api/workflows/hook/x/y")}});
    check(again.name == QLatin1String("Untitled workflow") && again.nodes.isEmpty() &&
              again.hookPath == QLatin1String("/api/workflows/hook/x/y"),
          QStringLiteral("a summary without a graph reads as an empty draft"));
    Draft desktop = Draft::blank();
    desktop.addStep(types, QStringLiteral("desktop.diskUsage"), QStringLiteral("trigger"));
    check(desktop.hasDesktopSteps(types) && !draft.hasDesktopSteps(types), QStringLiteral("desktop steps are found"));
}

void statsCases()
{
    const qint64 now = QDateTime(QDate(2026, 10, 18), QTime(12, 0)).toMSecsSinceEpoch();
    const QJsonArray workflows{
        QJsonObject{{QStringLiteral("id"), QStringLiteral("a")}, {QStringLiteral("name"), QStringLiteral("Alpha")},
                    {QStringLiteral("active"), true}, {QStringLiteral("trigger"), QStringLiteral("core.schedule")},
                    {QStringLiteral("nodes"), 4}, {QStringLiteral("nextRunAt"), double(now + 600000)},
                    {QStringLiteral("lastRunAt"), double(now - 1000)}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("b")}, {QStringLiteral("name"), QStringLiteral("Beta")},
                    {QStringLiteral("active"), false}, {QStringLiteral("trigger"), QStringLiteral("core.webhook")},
                    {QStringLiteral("nodes"), 2}, {QStringLiteral("nextRunAt"), double(now + 60000)}},
        QJsonObject{{QStringLiteral("id"), QStringLiteral("c")}, {QStringLiteral("name"), QStringLiteral("Gamma")},
                    {QStringLiteral("active"), false}, {QStringLiteral("trigger"), QStringLiteral("core.manual")},
                    {QStringLiteral("nodes"), 7}}};
    QHash<QString, QJsonArray> runs;
    runs.insert(QStringLiteral("a"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("r1")}, {QStringLiteral("status"), QStringLiteral("success")},
                                       {QStringLiteral("startedAt"), double(now - 1000)}, {QStringLiteral("finishedAt"), double(now)}},
                           QJsonObject{{QStringLiteral("id"), QStringLiteral("r2")}, {QStringLiteral("status"), QStringLiteral("error")},
                                       {QStringLiteral("startedAt"), double(now - 86400000 * 2)},
                                       {QStringLiteral("finishedAt"), double(now - 86400000 * 2 + 3000)}}});
    runs.insert(QStringLiteral("c"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("r3")}, {QStringLiteral("status"), QStringLiteral("error")},
                                       {QStringLiteral("startedAt"), double(now - 5000)}, {QStringLiteral("finishedAt"), double(now - 3000)}}});
    QHash<QString, QString> names{{QStringLiteral("a"), QStringLiteral("Alpha")}, {QStringLiteral("c"), QStringLiteral("Gamma")}};
    const QJsonArray merged = mergeRuns(runs, names, 0);
    check(merged.size() == 3 && merged.at(0).toObject().value(QStringLiteral("id")).toString() == QLatin1String("r1") &&
              merged.at(1).toObject().value(QStringLiteral("workflowName")).toString() == QLatin1String("Gamma") &&
              merged.at(2).toObject().value(QStringLiteral("workflowId")).toString() == QLatin1String("a"),
          QStringLiteral("runs of every workflow merge newest first with their workflow"));
    check(mergeRuns(runs, names, 2).size() == 2, QStringLiteral("the merged list honours its limit"));
    const Stats stats = computeStats(workflows, merged, now);
    check(stats.workflows == 3 && stats.active == 1 && stats.scheduled == 1 && stats.webhooks == 1 && stats.manual == 1,
          QStringLiteral("workflow counts by state and trigger"));
    check(stats.steps == 13, QStringLiteral("steps add up across workflows"));
    check(stats.nextRunAt == now + 600000, QStringLiteral("the next run only counts active workflows"));
    check(stats.runs == 3 && stats.succeeded == 1 && stats.failed == 2 && stats.runsToday == 2,
          QStringLiteral("run counts by result and day"));
    check(stats.averageMs == 2000 && stats.lastRunAt == now - 1000, QStringLiteral("average duration and last run"));
    check(stats.lastFailedWorkflow == QLatin1String("Gamma"), QStringLiteral("the latest failure names its workflow"));
    check(successRateText(stats) == QLatin1String("33%") && successRateText(Stats()) == QStringLiteral("–"),
          QStringLiteral("success rate reads as a percentage"));
}

void textCases()
{
    check(everyText(1440) == QLatin1String("Every day") && everyText(90) == QLatin1String("Every 90 minutes") &&
              everyText(0).isEmpty(),
          QStringLiteral("schedule intervals read in words"));
    check(humanBytes(512) == QLatin1String("512 B") && humanBytes(1536) == QLatin1String("1.5 KB") &&
              humanBytes(3.0 * 1024 * 1024 * 1024) == QLatin1String("3.0 GB"),
          QStringLiteral("sizes match the server's human()"));
    check(durationText(250) == QLatin1String("250 ms") && durationText(125000) == QLatin1String("2m 5s"),
          QStringLiteral("durations read in units"));
    const qint64 now = 10'000'000'000;
    check(relativeTime(0, now) == QLatin1String("Never") && relativeTime(now - 5 * 60000, now) == QLatin1String("5 min ago") &&
              relativeTime(now + 3 * 3600000, now) == QLatin1String("In 3 h"),
          QStringLiteral("times read relative to now"));
    check(errorText(QStringLiteral("cycle"), QString()).contains(QLatin1String("loop")) &&
              errorText(QStringLiteral("something_new"), QStringLiteral("HTTP 500")) == QLatin1String("HTTP 500"),
          QStringLiteral("server error codes read as sentences, unknown ones fall back"));
    check(statusLabel(QStringLiteral("error")) == QLatin1String("Failed") && triggerLabel(QString()) == QLatin1String("No trigger"),
          QStringLiteral("status and trigger labels"));
    const QJsonObject run{{QStringLiteral("output"),
                           QJsonObject{{QStringLiteral("metrics"),
                                        QJsonArray{QJsonObject{{QStringLiteral("label"), QStringLiteral("Cleaned")},
                                                               {QStringLiteral("value"), QStringLiteral("1.0 GB")}}}}}}};
    check(runOutputText(run) == QLatin1String("Cleaned: 1.0 GB"), QStringLiteral("run output falls back to its metrics"));
}

void desktopCases()
{
    const QStringList args = freeDiskSpaceArguments(QStringLiteral("/repo"), QStringLiteral("agents"));
    check(args == QStringList{QStringLiteral("-u"), QStringLiteral("/repo/tools/free_disk_space.py"),
                              QStringLiteral("--execute"), QStringLiteral("--step"), QStringLiteral("agents"),
                              QStringLiteral("--repo"), QStringLiteral("/repo"), QStringLiteral("--json")},
          QStringLiteral("a desktop step runs the same command as the desktop's own runner"));
    const QByteArray output = "removing x\n::result::{\"step\":\"agents\",\"freedBytes\":2048,\"items\":3,"
                              "\"freeBytes\":10,\"totalBytes\":100}\ndone\n";
    const QJsonObject reply = freeDiskSpaceReply(QStringLiteral("agents"), output, 1500);
    check(reply.value(QStringLiteral("freedBytes")).toDouble() == 2048 && reply.value(QStringLiteral("items")).toDouble() == 3 &&
              reply.value(QStringLiteral("lines")).toArray().size() == 2 && reply.value(QStringLiteral("ms")).toDouble() == 1500,
          QStringLiteral("the result line and the log lines are read"));
    check(freeDiskSpaceReply(QStringLiteral("tests"), output, 1).isEmpty(),
          QStringLiteral("a result for another step is refused"));
    check(freeDiskSpaceFailure("a\nPermission denied\n") == QLatin1String("Permission denied") &&
              freeDiskSpaceFailure("").startsWith(QLatin1String("The cleanup step")),
          QStringLiteral("a failure reports the last line printed"));
}

void localCases()
{
    Catalog local;
    local.load(QJsonDocument::fromJson(R"json({
      "core": [
        {"type": "workflow.manual", "name": "Run by hand", "trigger": true, "outputs": ["main"], "fields": []},
        {"type": "workflow.schedule", "name": "Schedule", "trigger": true, "outputs": ["main"],
         "fields": [{"key": "mode", "label": "How often", "type": "select", "default": "daily"}]},
        {"type": "workflow.monitor", "name": "Log monitor", "trigger": true, "outputs": ["main"],
         "fields": [{"key": "badge", "label": "Badge", "type": "string"}]},
        {"type": "agent.run", "name": "Special agent", "outputs": ["main"],
         "fields": [{"key": "specialty", "label": "Specialty", "type": "string"}]}
      ]
    })json").object());
    Draft draft = Draft::fromLocal(QJsonDocument::fromJson(R"json({
      "kind": "forkmesh.workflow", "id": "guard", "name": "Guard", "icon": "sentinel", "enabled": false,
      "description": "Watches", "createdAtMs": 5,
      "nodes": [{"id": "schedule", "type": "workflow.schedule", "name": "Schedule", "x": 0, "y": 0, "params": {}},
                {"id": "agent", "type": "agent.run", "name": "Guard", "x": 288, "y": 0,
                 "params": {"specialty": "Security"}}],
      "connections": [{"from": "schedule", "output": "main", "to": "agent"}]
    })json").object(), QStringLiteral("forkmesh/forkmesh"));
    check(draft.isLocal() && !draft.active && draft.icon == QLatin1String("sentinel") && draft.hasAgentStep(),
          QStringLiteral("a desktop workflow opens with its repository, state, icon and agent step"));
    const QString monitor = draft.addTrigger(local, QStringLiteral("workflow.monitor"));
    check(!monitor.isEmpty() && draft.nodes.size() == 3 &&
              draft.inputs(QStringLiteral("agent")).size() == 2,
          QStringLiteral("a desktop workflow takes another trigger beside its schedule"));
    draft.retype(local, monitor, QStringLiteral("workflow.manual"));
    check(draft.node(monitor).value(QStringLiteral("type")).toString() == QLatin1String("workflow.manual") &&
              draft.node(QStringLiteral("schedule")).value(QStringLiteral("type")).toString() ==
                  QLatin1String("workflow.schedule"),
          QStringLiteral("changing one trigger's kind leaves the others"));
    const QJsonObject saved = draft.localJson();
    check(saved.value(QStringLiteral("kind")).toString() == QLatin1String("forkmesh.workflow") &&
              saved.value(QStringLiteral("enabled")).toBool() == false &&
              saved.value(QStringLiteral("description")).toString() == QLatin1String("Watches") &&
              saved.value(QStringLiteral("createdAtMs")).toDouble() == 5,
          QStringLiteral("a desktop workflow saves in the repository file format"));
    check(localWorkflowId(QStringLiteral("Deploy & Ship it!")) == QLatin1String("deploy-ship-it") &&
              localWorkflowId(QStringLiteral("  ")) == QLatin1String("workflow"),
          QStringLiteral("a new desktop workflow gets a file id from its name"));
    check(statusLabel(QStringLiteral("stopped")) == QLatin1String("Stopped"),
          QStringLiteral("a stopped run says so"));
}

void unifiedCases()
{
    const QJsonArray account{QJsonObject{{QStringLiteral("id"), QStringLiteral("a1")},
                                         {QStringLiteral("name"), QStringLiteral("Morning report")},
                                         {QStringLiteral("active"), true},
                                         {QStringLiteral("trigger"), QStringLiteral("core.schedule")},
                                         {QStringLiteral("scheduleMinutes"), 60},
                                         {QStringLiteral("nodes"), 3},
                                         {QStringLiteral("nextRunAt"), 9000.0},
                                         {QStringLiteral("lastRunAt"), 100.0},
                                         {QStringLiteral("lastStatus"), QStringLiteral("success")}}};
    QHash<QString, QJsonArray> runs;
    runs.insert(QStringLiteral("a1"),
                QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("r1")},
                                       {QStringLiteral("status"), QStringLiteral("success")},
                                       {QStringLiteral("output"), QJsonObject{{QStringLiteral("text"), QStringLiteral("3 items")}}}},
                           QJsonObject{{QStringLiteral("id"), QStringLiteral("r0")},
                                       {QStringLiteral("status"), QStringLiteral("error")}}});
    const QJsonArray local{QJsonObject{{QStringLiteral("repo"), QStringLiteral("forkmesh/forkmesh")},
                                       {QStringLiteral("id"), QStringLiteral("sentinel")},
                                       {QStringLiteral("name"), QStringLiteral("Sentinel")},
                                       {QStringLiteral("specialAgent"), true},
                                       {QStringLiteral("enabled"), true},
                                       {QStringLiteral("hasSchedule"), true},
                                       {QStringLiteral("scheduleActive"), true},
                                       {QStringLiteral("steps"), 2},
                                       {QStringLiteral("lastRunAt"), 500.0},
                                       {QStringLiteral("lastStatus"), QStringLiteral("error")},
                                       {QStringLiteral("lastSummary"), QStringLiteral("Tests failed")},
                                       {QStringLiteral("recentRuns"),
                                        QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("lr1")},
                                                               {QStringLiteral("outputFile"), QStringLiteral("/runs/lr1.md")}}}}},
                           QJsonObject{{QStringLiteral("repo"), QStringLiteral("forkmesh/forkmesh")},
                                       {QStringLiteral("id"), QStringLiteral("clean")},
                                       {QStringLiteral("name"), QStringLiteral("Clean up")},
                                       {QStringLiteral("enabled"), false},
                                       {QStringLiteral("pinned"), QStringLiteral("bottom")}}};
    const QJsonArray pins{QJsonObject{{QStringLiteral("repo"), QString::fromLatin1(kAccountRepo)},
                                      {QStringLiteral("workflow"), QStringLiteral("a1")},
                                      {QStringLiteral("rail"), QStringLiteral("top")}}};
    QJsonArray entries = unifiedWorkflows(account, runs, local, pins);
    check(entries.size() == 3, QStringLiteral("special agents, desktop and account workflows share one list"));
    QHash<QString, QJsonObject> byKey;
    for (const QJsonValue &value : entries)
        byKey.insert(value.toObject().value(QStringLiteral("key")).toString(), value.toObject());
    const QJsonObject agent = byKey.value(QStringLiteral("local:forkmesh/forkmesh|sentinel"));
    const QJsonObject report = byKey.value(QStringLiteral("account:a1"));
    check(agent.value(QStringLiteral("kindLabel")).toString() == QLatin1String("Special agent") &&
              agent.value(QStringLiteral("lastOutput")).toString() == QLatin1String("Tests failed") &&
              agent.value(QStringLiteral("lastOutputFile")).toString() == QLatin1String("/runs/lr1.md") &&
              !agent.contains(QStringLiteral("recentRuns")),
          QStringLiteral("a special agent keeps its kind, latest output and output file"));
    check(report.value(QStringLiteral("trigger")).toString() == QLatin1String("Every hour") &&
              report.value(QStringLiteral("lastOutput")).toString() == QLatin1String("3 items") &&
              report.value(QStringLiteral("lastRunId")).toString() == QLatin1String("r1") &&
              report.value(QStringLiteral("runs")).toInt() == 2 && report.value(QStringLiteral("failedRuns")).toInt() == 1 &&
              report.value(QStringLiteral("pinned")).toString() == QLatin1String("top") &&
              report.value(QStringLiteral("repo")).toString() == QLatin1String(kAccountRepo),
          QStringLiteral("an account workflow carries its schedule, latest output, runs and rail like a special agent"));
    check(workflowMatches(agent, QString(), QStringLiteral("agent")) &&
              !workflowMatches(report, QString(), QStringLiteral("agent")) &&
              workflowMatches(report, QString(), QStringLiteral("account")) &&
              workflowMatches(agent, QString(), QStringLiteral("failing")) &&
              workflowMatches(report, QString(), QStringLiteral("pinned")) &&
              workflowMatches(agent, QStringLiteral("tests fail"), QString()) &&
              !workflowMatches(agent, QStringLiteral("nothing like it"), QString()),
          QStringLiteral("the list filters by kind, attention, rail and words in the name or output"));
    sortWorkflows(entries, QStringLiteral("name"));
    check(entries.at(0).toObject().value(QStringLiteral("name")).toString() == QLatin1String("Clean up") &&
              entries.at(2).toObject().value(QStringLiteral("name")).toString() == QLatin1String("Sentinel"),
          QStringLiteral("the list sorts by name"));
    sortWorkflows(entries, QStringLiteral("recent"));
    check(entries.at(0).toObject().value(QStringLiteral("name")).toString() == QLatin1String("Sentinel"),
          QStringLiteral("the list sorts by last run"));
    sortWorkflows(entries, QStringLiteral("next"));
    check(entries.at(0).toObject().value(QStringLiteral("name")).toString() == QLatin1String("Morning report"),
          QStringLiteral("the list sorts by next run, scheduled ones first"));
    const Stats stats = computeStats(statsWorkflows(entries), QJsonArray(), 1000);
    check(stats.workflows == 3 && stats.active == 2 && stats.scheduled == 2 && stats.steps == 5,
          QStringLiteral("the stats count every workflow in the list"));
    check(workflowStateText(agent) == QLatin1String("Failed") &&
              workflowStateText(byKey.value(QStringLiteral("local:forkmesh/forkmesh|clean"))) == QLatin1String("Off"),
          QStringLiteral("each card says its state in words"));
}

} // namespace

void workflowsCheck(bool condition, const QString &message)
{
    check(condition, message);
}

void workflowsFeatureChecks();

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    catalogCases();
    draftCases();
    statsCases();
    textCases();
    desktopCases();
    localCases();
    unifiedCases();
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshWorkflowsTest"));
    QStandardPaths::setTestModeEnabled(true);
    workflowsFeatureChecks();
    if (failures)
        qCritical("%d workflows model checks failed", failures);
    return failures ? 1 : 0;
}
