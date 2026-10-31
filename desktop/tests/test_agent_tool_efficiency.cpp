// Tool catalog coverage and owner-device efficiency ledger tests. Core-only:
// no QApplication, no widgets, so the suite stays fast and headless.
#include "../src/AgentPerformanceHistory.h"
#include "../src/AgentRunEfficiencyLog.h"
#include "../src/AgentToolCatalog.h"
#include "../src/AgentToolEfficiencyLedger.h"
#include "../src/BackgroundFileWriter.h"
#include "../src/AgentToolImprovementLog.h"
#include "../src/AgentToolPolicy.h"

#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTextStream>

#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>

namespace {

int failures = 0;

void check(bool condition, const QString &what)
{
    if (condition) {
        std::printf("ok   - %s\n", qPrintable(what));
        return;
    }
    ++failures;
    std::printf("FAIL - %s\n", qPrintable(what));
}

QJsonObject toolUse(const QString &id, const QString &name,
                    const QJsonObject &input)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("assistant")},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("role"), QStringLiteral("assistant")},
                     {QStringLiteral("content"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("type"), QStringLiteral("tool_use")},
                          {QStringLiteral("id"), id},
                          {QStringLiteral("name"), name},
                          {QStringLiteral("input"), input}}}}}}};
}

QJsonObject toolResult(const QString &id, const QString &content,
                       bool isError = false)
{
    return QJsonObject{
        {QStringLiteral("type"), QStringLiteral("user")},
        {QStringLiteral("message"),
         QJsonObject{{QStringLiteral("role"), QStringLiteral("user")},
                     {QStringLiteral("content"),
                      QJsonArray{QJsonObject{
                          {QStringLiteral("type"), QStringLiteral("tool_result")},
                          {QStringLiteral("tool_use_id"), id},
                          {QStringLiteral("content"), content},
                          {QStringLiteral("is_error"), isError}}}}}}};
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(bytes) == bytes.size();
}

void testCatalogCoverage()
{
    // The complete public tool surface of the reference coding agent plus
    // its three hidden control tools. Every one must be a catalog family so
    // the efficiency board can show coverage truthfully.
    const QStringList upstream = {
        QStringLiteral("read"), QStringLiteral("write"), QStringLiteral("edit"),
        QStringLiteral("ast_edit"), QStringLiteral("ast_grep"),
        QStringLiteral("grep"), QStringLiteral("glob"), QStringLiteral("bash"),
        QStringLiteral("eval"), QStringLiteral("lsp"), QStringLiteral("debug"),
        QStringLiteral("security_scan"), QStringLiteral("task"),
        QStringLiteral("hub"), QStringLiteral("todo"), QStringLiteral("ask"),
        QStringLiteral("browser"), QStringLiteral("computer"),
        QStringLiteral("web_search"), QStringLiteral("github"),
        QStringLiteral("generate_image"), QStringLiteral("tts"),
        QStringLiteral("checkpoint"), QStringLiteral("rewind"),
        QStringLiteral("context_notes"), QStringLiteral("new_context"),
        QStringLiteral("retain"), QStringLiteral("recall"),
        QStringLiteral("reflect"), QStringLiteral("memory_edit"),
        QStringLiteral("learn"), QStringLiteral("manage_skill"),
        QStringLiteral("yield"), QStringLiteral("goal"), QStringLiteral("think")};
    const QStringList ids = agentToolFamilyIds();
    QStringList missing;
    for (const QString &id : upstream)
        if (!ids.contains(id))
            missing.append(id);
    check(missing.isEmpty(),
          QStringLiteral("every upstream tool family is in the catalog (missing: %1)")
              .arg(missing.join(QStringLiteral(", "))));
    check(ids.contains(QStringLiteral("mcp")) && ids.contains(QStringLiteral("other")),
          QStringLiteral("the catalog keeps MCP and unclassified buckets"));
    check(QSet<QString>(ids.cbegin(), ids.cend()).size() == ids.size(),
          QStringLiteral("catalog family ids are unique"));
    check(ids.size() == upstream.size() + 2,
          QStringLiteral("the catalog holds exactly upstream + two buckets"));

    int hidden = 0;
    bool complete = true;
    for (const AgentToolFamily &family : agentToolFamilies()) {
        hidden += family.hidden ? 1 : 0;
        complete = complete && !family.displayName.isEmpty() &&
                   !family.category.isEmpty() && !family.summary.isEmpty() &&
                   (family.availability == AgentToolAvailability::Native ||
                    family.availability == AgentToolAvailability::Package ||
                    family.availability == AgentToolAvailability::Planned) &&
                   (family.baseline == AgentToolBaseline::None ||
                    family.baseline == AgentToolBaseline::PartialRead ||
                    family.baseline == AgentToolBaseline::AnchoredEdit ||
                    family.baseline == AgentToolBaseline::MatchedFiles);
    }
    check(hidden == 3, QStringLiteral("exactly the three control tools are hidden"));
    check(complete, QStringLiteral("every family carries a complete descriptor"));
    check(agentToolFamily(QStringLiteral("read")) &&
              agentToolFamily(QStringLiteral("read"))->baseline ==
                  AgentToolBaseline::PartialRead &&
              !agentToolFamily(QStringLiteral("nope")),
          QStringLiteral("family lookup finds known ids and rejects unknown"));
}

void testRuntimeNameMapping()
{
    struct Case {
        const char *runtime;
        const char *family;
    };
    const Case cases[] = {
        {"Read", "read"},           {"NotebookRead", "read"},
        {"WebFetch", "read"},       {"Write", "write"},
        {"Edit", "edit"},           {"MultiEdit", "edit"},
        {"NotebookEdit", "edit"},   {"FileChange", "edit"},
        {"Grep", "grep"},           {"Glob", "glob"},
        {"Bash", "bash"},           {"BashOutput", "bash"},
        {"KillShell", "bash"},      {"Task", "task"},
        {"SendMessage", "hub"},     {"TaskOutput", "hub"},
        {"Sleep", "hub"},           {"ScheduleWakeup", "hub"},
        {"CronCreate", "hub"},      {"ToolSearch", "mcp"},
        {"TodoWrite", "todo"},
        {"ExitPlanMode", "todo"},   {"AskUserQuestion", "ask"},
        {"WebSearch", "web_search"}, {"ImageGeneration", "generate_image"},
        {"Skill", "manage_skill"},  {"mcp__github__create_issue", "github"},
        {"mcp__playwright__browser_click", "browser"},
        {"mcp__forkmesh__search_code", "mcp"},
        {"lsp::goto_definition", "lsp"},
        {"ast-grep::search", "ast_grep"},
        {"memory::recall", "recall"},
        {"security::scan_repo", "security_scan"},
        {"jupyter::execute_cell", "eval"},
        {"SomethingNew", "other"},  {"", "other"},
    };
    bool all = true;
    for (const Case &c : cases) {
        const QString got = agentToolFamilyFor(QLatin1String(c.runtime));
        if (got != QLatin1String(c.family)) {
            all = false;
            std::printf("   mapping %s -> %s (expected %s)\n", c.runtime,
                        qPrintable(got), c.family);
        }
    }
    check(all, QStringLiteral("runtime tool names fold into the expected families"));
    check(agentToolFamilyFor(QStringLiteral("gh_pr_view"),
                             QJsonObject{{QStringLiteral("namespace"),
                                          QStringLiteral("github")}}) ==
              QStringLiteral("github"),
          QStringLiteral("Codex dynamic tools classify by their namespace"));
}

void testPartialReadSavings()
{
    QTemporaryDir tree;
    check(tree.isValid(), QStringLiteral("temporary worktree exists"));
    const QByteArray big(40'000, 'x');
    check(writeFile(QDir(tree.path()).filePath(QStringLiteral("notes.txt")), big),
          QStringLiteral("fixture file written"));

    AgentToolEfficiencyLedger ledger;
    int notifications = 0;
    ledger.changed = [&notifications] { ++notifications; };
    AgentToolEfficiencyLedger::SessionContext context;
    context.sessionId = 7;
    context.provider = QStringLiteral("claude-code");
    context.model = QStringLiteral("claude-sonnet-5");
    context.workdir = tree.path();

    check(ledger.ingest(context,
                        toolUse(QStringLiteral("toolu_1"), QStringLiteral("Read"),
                                QJsonObject{{QStringLiteral("file_path"),
                                             QStringLiteral("notes.txt")},
                                            {QStringLiteral("limit"), 5}}),
                        1000),
          QStringLiteral("a tool_use is ingested"));
    AgentToolFamilyStats read = ledger.stats(QStringLiteral("read"));
    check(read.calls == 1 && read.inFlight == 1 && read.measuredCalls == 0 &&
              read.runtimeNames.value(QStringLiteral("Read")) == 1,
          QStringLiteral("an open call counts as in flight, not yet measured"));
    check(!ledger.ingest(context,
                         toolUse(QStringLiteral("toolu_1"), QStringLiteral("Read"),
                                 QJsonObject()),
                         1001),
          QStringLiteral("a replayed tool_use id is ignored"));

    const QString partial = QStringLiteral("1\txxxx\n2\txxxx\n3\txxxx\n");
    check(ledger.ingest(context, toolResult(QStringLiteral("toolu_1"), partial),
                        1400),
          QStringLiteral("the matching tool_result completes the call"));
    read = ledger.stats(QStringLiteral("read"));
    const qint64 baseline = AgentToolEfficiencyLedger::estimateTokens(big.size());
    const qint64 actual =
        AgentToolEfficiencyLedger::estimateTokens(partial.toUtf8().size());
    check(read.inFlight == 0 && read.measuredCalls == 1 &&
              read.baselineTokens == baseline &&
              read.savedTokens == baseline - actual && read.errors == 0,
          QStringLiteral("a partial read saves whole-file minus returned tokens"));
    check(read.latencySamples == 1 && read.latencyMsTotal == 400 &&
              read.latencyMsMax == 400,
          QStringLiteral("latency is the tool_use to tool_result gap"));
    check(read.savedUsd > 0.0 &&
              qAbs(read.savedUsd - read.savedTokens * 3.0 / 1e6) < 1e-9,
          QStringLiteral("saved USD prices input tokens for the session model"));
    check(read.recentSaved.size() == 1 && read.recentSaved.first() == read.savedTokens,
          QStringLiteral("the per-call sparkline records the saving"));
    check(notifications == 2, QStringLiteral("each change notifies exactly once"));

    // A full read of a small file saves nothing but still counts as measured.
    check(writeFile(QDir(tree.path()).filePath(QStringLiteral("small.txt")),
                    QByteArray(200, 'y')),
          QStringLiteral("small fixture written"));
    ledger.ingest(context,
                  toolUse(QStringLiteral("toolu_2"), QStringLiteral("Read"),
                          QJsonObject{{QStringLiteral("file_path"),
                                       QDir(tree.path()).filePath(
                                           QStringLiteral("small.txt"))}}),
                  2000);
    ledger.ingest(context,
                  toolResult(QStringLiteral("toolu_2"),
                             QString::fromLatin1(QByteArray(400, 'y'))),
                  2100);
    read = ledger.stats(QStringLiteral("read"));
    check(read.calls == 2 && read.measuredCalls == 2 &&
              read.savedTokens == baseline - actual,
          QStringLiteral("a read larger than its file never claims a saving"));

    // URLs and missing files are not measurable.
    ledger.ingest(context,
                  toolUse(QStringLiteral("toolu_3"), QStringLiteral("WebFetch"),
                          QJsonObject{{QStringLiteral("url"),
                                       QStringLiteral("https://example.com")}}),
                  3000);
    ledger.ingest(context, toolResult(QStringLiteral("toolu_3"), QStringLiteral("page")),
                  3100);
    ledger.ingest(context,
                  toolUse(QStringLiteral("toolu_4"), QStringLiteral("Read"),
                          QJsonObject{{QStringLiteral("file_path"),
                                       QStringLiteral("missing.txt")}}),
                  3200);
    ledger.ingest(context,
                  toolResult(QStringLiteral("toolu_4"), QStringLiteral("no such file"),
                             true),
                  3300);
    read = ledger.stats(QStringLiteral("read"));
    check(read.calls == 4 && read.measuredCalls == 2 && read.errors == 1,
          QStringLiteral("URL fetches and failed reads count without a baseline"));
}

void testAnchoredEditAndMatchedFiles()
{
    QTemporaryDir tree;
    const QDir dir(tree.path());
    dir.mkpath(QStringLiteral("src"));
    check(writeFile(dir.filePath(QStringLiteral("src/a.cpp")), QByteArray(8000, 'a')) &&
              writeFile(dir.filePath(QStringLiteral("src/b.cpp")), QByteArray(12000, 'b')),
          QStringLiteral("edit fixtures written"));

    AgentToolEfficiencyLedger ledger;
    AgentToolEfficiencyLedger::SessionContext context;
    context.sessionId = 3;
    context.provider = QStringLiteral("codex");
    context.workdir = tree.path();

    const QJsonObject editInput{
        {QStringLiteral("file_path"), QStringLiteral("src/a.cpp")},
        {QStringLiteral("old_string"), QStringLiteral("aaaa")},
        {QStringLiteral("new_string"), QStringLiteral("bbbb")}};
    ledger.ingest(context, toolUse(QStringLiteral("e1"), QStringLiteral("Edit"), editInput), 10);
    ledger.ingest(context, toolResult(QStringLiteral("e1"), QStringLiteral("ok")), 20);
    AgentToolFamilyStats edit = ledger.stats(QStringLiteral("edit"));
    const qint64 patchTokens = AgentToolEfficiencyLedger::estimateTokens(
        QJsonDocument(editInput).toJson(QJsonDocument::Compact).size());
    check(edit.measuredCalls == 1 && edit.baselineTokens == 2000 &&
              edit.savedTokens == 2000 - patchTokens && edit.inputTokens == patchTokens,
          QStringLiteral("an anchored edit saves the whole-file rewrite minus the patch"));
    check(qAbs(edit.savedUsd - edit.savedTokens * 10.0 / 1e6) < 1e-9,
          QStringLiteral("edit savings are priced as output tokens"));

    // Codex file changes name several paths.
    ledger.ingest(context,
                  toolUse(QStringLiteral("e2"), QStringLiteral("FileChange"),
                          QJsonObject{{QStringLiteral("changes"),
                                       QJsonArray{
                                           QJsonObject{{QStringLiteral("path"),
                                                        QStringLiteral("src/a.cpp")}},
                                           QJsonObject{{QStringLiteral("path"),
                                                        QStringLiteral("src/b.cpp")}},
                                           QJsonObject{{QStringLiteral("path"),
                                                        QStringLiteral("src/a.cpp")}}}}}),
                  30);
    ledger.ingest(context, toolResult(QStringLiteral("e2"), QStringLiteral("[]")), 40);
    edit = ledger.stats(QStringLiteral("edit"));
    check(edit.measuredCalls == 2 && edit.baselineTokens == 2000 + 5000,
          QStringLiteral("a multi-file change sums each path once"));

    // Grep: the naive alternative reads every file that matched.
    ledger.ingest(context,
                  toolUse(QStringLiteral("g1"), QStringLiteral("Grep"),
                          QJsonObject{{QStringLiteral("pattern"), QStringLiteral("aaaa")}}),
                  50);
    ledger.ingest(context,
                  toolResult(QStringLiteral("g1"),
                             QStringLiteral("src/a.cpp:1:aaaa\nsrc/b.cpp:7:bbbb\n"
                                            "src/a.cpp:9:aaaa\nnot/a/file:3:zzz\n")),
                  60);
    const AgentToolFamilyStats grep = ledger.stats(QStringLiteral("grep"));
    check(grep.measuredCalls == 1 && grep.baselineTokens == 5000 &&
              grep.savedTokens == 5000 - grep.resultTokens,
          QStringLiteral("grep baselines against every distinct matched file"));

    // Shell output has no baseline: consumption only.
    ledger.ingest(context,
                  toolUse(QStringLiteral("b1"), QStringLiteral("Bash"),
                          QJsonObject{{QStringLiteral("command"), QStringLiteral("ls")}}),
                  70);
    ledger.ingest(context,
                  toolResult(QStringLiteral("b1"), QString(4000, QLatin1Char('l'))),
                  90);
    const AgentToolFamilyStats bash = ledger.stats(QStringLiteral("bash"));
    check(bash.calls == 1 && bash.measuredCalls == 0 && bash.savedTokens == 0 &&
              bash.resultTokens == 1000 && bash.consumedUsd > 0.0,
          QStringLiteral("shell calls report consumption without claiming savings"));

    const AgentToolLedgerTotals totals = ledger.totals(100);
    check(totals.calls == 4 && totals.familiesObserved == 3 &&
              totals.familiesTotal == agentToolFamilies().size() &&
              totals.savedTokens == edit.savedTokens + grep.savedTokens &&
              totals.recentCalls.size() == AgentToolEfficiencyLedger::kActivityBuckets &&
              totals.recentCalls.last() == 4,
          QStringLiteral("totals aggregate calls, coverage, and activity"));
}

void testLifecycleAndPersistence()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("nested/ledger.json"));
    AgentToolEfficiencyLedger ledger(path);
    AgentToolEfficiencyLedger::SessionContext context;
    context.sessionId = 11;
    context.provider = QStringLiteral("claude-code");

    ledger.ingest(context, toolUse(QStringLiteral("t1"), QStringLiteral("Task"),
                                   QJsonObject{{QStringLiteral("prompt"), QStringLiteral("x")}}),
                  100);
    ledger.ingest(context, toolUse(QStringLiteral("t2"), QStringLiteral("Bash"),
                                   QJsonObject{{QStringLiteral("command"), QStringLiteral("y")}}),
                  110);
    check(ledger.totals(120).inFlight == 2, QStringLiteral("two calls are in flight"));
    check(!ledger.ingest(context, toolResult(QStringLiteral("unknown"), QStringLiteral("?")), 130),
          QStringLiteral("a tool_result without a tool_use is ignored"));
    ledger.abandonSession(11);
    check(ledger.totals(140).inFlight == 0 &&
              ledger.stats(QStringLiteral("task")).calls == 1,
          QStringLiteral("abandoning a session clears in-flight calls but keeps counts"));
    check(!ledger.ingest(context, toolResult(QStringLiteral("t1"), QStringLiteral("late")), 150),
          QStringLiteral("a result for an abandoned call is ignored"));

    QString error;
    check(ledger.save(&error), QStringLiteral("the ledger saves atomically: %1").arg(error));
    AgentToolEfficiencyLedger restored(path);
    check(restored.load(&error) && restored.stats(QStringLiteral("task")).calls == 1 &&
              restored.stats(QStringLiteral("bash")).calls == 1 &&
              restored.totals(200).calls == 2 &&
              restored.stats(QStringLiteral("bash")).runtimeNames.value(QStringLiteral("Bash")) == 1,
          QStringLiteral("a restart restores every family: %1").arg(error));
    check(restored.totals(200).inFlight == 0,
          QStringLiteral("in-flight calls never survive a restart"));

    // Malformed state fails closed and leaves the current ledger untouched.
    check(writeFile(path, "{\"schema\":1,\"families\":{\"bash\":{\"calls\":-4}}}"),
          QStringLiteral("corrupt ledger written"));
    check(!restored.load(&error) && restored.stats(QStringLiteral("bash")).calls == 1,
          QStringLiteral("a corrupt ledger is rejected without losing loaded stats"));
    check(writeFile(path, "not json"), QStringLiteral("garbage ledger written"));
    check(!restored.load(&error), QStringLiteral("garbage is rejected"));
    check(writeFile(path, "{\"schema\":1,\"families\":{\"zzz-unknown\":{\"calls\":1,"
                          "\"errors\":0,\"measuredCalls\":0,\"latencyMsTotal\":0,"
                          "\"latencyMsMax\":0,\"latencySamples\":0,\"inputTokens\":0,"
                          "\"resultTokens\":0,\"baselineTokens\":0,\"savedTokens\":0,"
                          "\"lastCallMs\":0}}}"),
          QStringLiteral("unknown-family ledger written"));
    check(restored.load(&error) && restored.totals(300).calls == 0,
          QStringLiteral("families this build no longer knows are dropped"));

    AgentToolEfficiencyLedger unsaved;
    check(!unsaved.save(&error) && !unsaved.load(&error),
          QStringLiteral("a ledger without a path neither saves nor loads"));
    restored.clear();
    check(!restored.hasSamples() && restored.totals(400).familiesObserved == 0,
          QStringLiteral("clear forgets every sample"));
}

void testModelUsage()
{
    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("ledger.json"));
    AgentToolEfficiencyLedger ledger(path);
    AgentToolEfficiencyLedger::SessionContext opus;
    opus.sessionId = 21;
    opus.provider = QStringLiteral("claude-code");
    opus.model = QStringLiteral("Claude-Opus-5");
    AgentToolEfficiencyLedger::SessionContext codex;
    codex.sessionId = 22;
    codex.provider = QStringLiteral("codex");
    codex.model = QStringLiteral("gpt-5.5");
    ledger.beginCall(opus, QStringLiteral("a"), QStringLiteral("Bash"),
                     QJsonObject{{QStringLiteral("command"), QStringLiteral("ls")}}, 100);
    ledger.beginCall(opus, QStringLiteral("b"), QStringLiteral("Bash"),
                     QJsonObject{{QStringLiteral("command"), QStringLiteral("false")}}, 110);
    ledger.beginCall(codex, QStringLiteral("c"), QStringLiteral("Bash"),
                     QJsonObject{{QStringLiteral("command"), QStringLiteral("ls")}}, 120);
    ledger.completeCall(21, QStringLiteral("a"), QStringLiteral("file"), false, 200);
    ledger.completeCall(21, QStringLiteral("b"), QStringLiteral("boom"), true, 300);

    const auto find = [](const AgentToolEfficiencyLedger &l, const QString &provider) {
        for (const AgentToolModelStats &m : l.modelUsage())
            if (m.provider == provider)
                return m;
        return AgentToolModelStats();
    };
    const AgentToolModelStats opusStats = find(ledger, QStringLiteral("claude-code"));
    check(ledger.modelUsage().size() == 2 && opusStats.calls == 2 &&
              opusStats.errors == 1 && opusStats.model == QStringLiteral("claude-opus-5") &&
              opusStats.latencySamples == 2 && opusStats.resultTokens > 0,
          QStringLiteral("tool calls are counted per model"));
    check(find(ledger, QStringLiteral("codex")).calls == 1,
          QStringLiteral("each model keeps its own tool counters"));

    QString error;
    AgentToolEfficiencyLedger restored(path);
    check(ledger.save(&error) && restored.load(&error) &&
              find(restored, QStringLiteral("claude-code")).errors == 1 &&
              find(restored, QStringLiteral("codex")).calls == 1,
          QStringLiteral("per-model tool counters survive a restart: %1").arg(error));
    restored.clear();
    check(restored.modelUsage().isEmpty(), QStringLiteral("clear forgets per-model counters"));
}

void testPendingBound()
{
    AgentToolEfficiencyLedger ledger;
    AgentToolEfficiencyLedger::SessionContext context;
    context.sessionId = 1;
    for (qint64 i = 0; i < AgentToolEfficiencyLedger::kMaxPendingCalls + 5; ++i)
        ledger.beginCall(context, QStringLiteral("id-%1").arg(i), QStringLiteral("Bash"),
                         QJsonObject(), 1000 + i);
    const AgentToolFamilyStats bash = ledger.stats(QStringLiteral("bash"));
    check(bash.calls == AgentToolEfficiencyLedger::kMaxPendingCalls + 5 &&
              bash.inFlight == AgentToolEfficiencyLedger::kMaxPendingCalls,
          QStringLiteral("a runtime that never answers cannot grow the pending table"));
    check(!ledger.completeCall(1, QStringLiteral("id-0"), QString(), false, 5000) &&
              ledger.completeCall(1, QStringLiteral("id-6"), QString(), false, 5000),
          QStringLiteral("the oldest pending call is the one evicted"));
    check(!ledger.beginCall(context, QString(), QStringLiteral("Bash"), QJsonObject(), 1) &&
              !ledger.beginCall(context, QString(300, QLatin1Char('x')),
                                QStringLiteral("Bash"), QJsonObject(), 1),
          QStringLiteral("empty or oversized tool ids are rejected"));
}

void testToolPolicy()
{
    using namespace AgentToolPolicy;
    check(normalizeDisabledFamilies({QStringLiteral(" Bash "), QStringLiteral("read"),
                                     QStringLiteral("bash"), QStringLiteral("nope"),
                                     QString()}) ==
              QStringList{QStringLiteral("bash"), QStringLiteral("read")},
          QStringLiteral("disabled families normalise to known, unique, sorted ids"));

    const QJsonObject options = optionsWithDisabledFamilies(
        QJsonObject{{QStringLiteral("keep"), 1}},
        {QStringLiteral("web_search"), QStringLiteral("bash")});
    check(options.value(QStringLiteral("keep")).toInt() == 1 &&
              disabledFamiliesFromOptions(options) ==
                  QStringList{QStringLiteral("bash"), QStringLiteral("web_search")},
          QStringLiteral("launch options round-trip the disabled families"));
    check(!optionsWithDisabledFamilies(options, {}).contains(LaunchOption),
          QStringLiteral("an empty policy removes the launch option"));

    const QStringList claude =
        claudeDisallowedTools({QStringLiteral("bash"), QStringLiteral("read"),
                               QStringLiteral("checkpoint")});
    check(claude.contains(QStringLiteral("Bash")) &&
              claude.contains(QStringLiteral("BashOutput")) &&
              claude.contains(QStringLiteral("Read")) &&
              claude.contains(QStringLiteral("WebFetch")) &&
              !claude.contains(QStringLiteral("Edit")),
          QStringLiteral("Claude disallowed tools cover every runtime name of an "
                         "off family and nothing else"));
    check(claudeDisallowedTools({QStringLiteral("checkpoint")}).isEmpty(),
          QStringLiteral("families without a Claude tool add nothing"));

    const QStringList codex =
        codexConfigOverrides({QStringLiteral("web_search"), QStringLiteral("read")});
    check(codex.contains(QStringLiteral("web_search=\"disabled\"")) &&
              codex.contains(QStringLiteral("tools.web_search=false")) &&
              codex.contains(QStringLiteral("tools.view_image=false")) &&
              codexConfigOverrides({QStringLiteral("bash")}).isEmpty(),
          QStringLiteral("Codex overrides exist only for switchable families"));
    check(deniesProcessExecution({QStringLiteral("bash")}) &&
              !deniesProcessExecution({QStringLiteral("edit")}),
          QStringLiteral("only the Shell family denies process execution"));
    check(enforcementNote(QStringLiteral("bash")).contains(QStringLiteral("--disallowedTools")) &&
              enforcementNote(QStringLiteral("bash")).contains(QStringLiteral("permission broker")) &&
              enforcementNote(QStringLiteral("checkpoint")).contains(QStringLiteral("No runtime switch")),
          QStringLiteral("enforcement notes describe each runtime path"));

    // The ledger counts calls that arrive while a family is off.
    AgentToolEfficiencyLedger ledger;
    AgentToolEfficiencyLedger::SessionContext context;
    context.sessionId = 5;
    ledger.setDisabledFamilies({QStringLiteral("bash")});
    ledger.ingest(context, toolUse(QStringLiteral("o1"), QStringLiteral("Bash"),
                                   QJsonObject{{QStringLiteral("command"), QStringLiteral("ls")}}),
                  10);
    ledger.ingest(context, toolUse(QStringLiteral("o2"), QStringLiteral("Read"),
                                   QJsonObject{{QStringLiteral("file_path"), QStringLiteral("x")}}),
                  20);
    check(ledger.stats(QStringLiteral("bash")).callsWhileOff == 1 &&
              ledger.stats(QStringLiteral("bash")).calls == 1 &&
              ledger.stats(QStringLiteral("read")).callsWhileOff == 0,
          QStringLiteral("calls made while a family is off are counted separately"));
    QTemporaryDir dir;
    AgentToolEfficiencyLedger persisted(QDir(dir.path()).filePath(QStringLiteral("l.json")));
    persisted.setDisabledFamilies({QStringLiteral("bash")});
    persisted.ingest(context, toolUse(QStringLiteral("o3"), QStringLiteral("Bash"), QJsonObject()), 30);
    QString error;
    AgentToolEfficiencyLedger restored(persisted.persistPath());
    check(persisted.save(&error) && restored.load(&error) &&
              restored.stats(QStringLiteral("bash")).callsWhileOff == 1,
          QStringLiteral("calls-while-off survive a restart: %1").arg(error));
}

const AgentToolImprovement *findSignal(const QVector<AgentToolImprovement> &all,
                                       const QString &kind,
                                       const QString &family = QString())
{
    for (const AgentToolImprovement &signal : all)
        if (signal.kind == kind && (family.isEmpty() || signal.family == family))
            return &signal;
    return nullptr;
}

int countLines(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    int lines = 0;
    while (!file.atEnd())
        lines += file.readLine().trimmed().isEmpty() ? 0 : 1;
    return lines;
}

void testImprovementLog()
{
    QTemporaryDir tree;
    const QDir dir(tree.path());
    check(writeFile(dir.filePath(QStringLiteral("big.txt")), QByteArray(40000, 'b')) &&
              writeFile(dir.filePath(QStringLiteral("src.cpp")), QByteArray(8000, 'c')),
          QStringLiteral("improvement fixtures written"));
    const QString logPath = dir.filePath(QStringLiteral("state/tool-improvements.jsonl"));

    AgentToolEfficiencyLedger ledger;
    AgentToolImprovementLog log(logPath);
    int changes = 0;
    log.changed = [&changes] { ++changes; };
    ledger.callObserved = [&log](const AgentToolCallObservation &call) {
        log.observe(call);
    };
    AgentToolEfficiencyLedger::SessionContext context;
    context.sessionId = 3;
    context.provider = QStringLiteral("claude-code");
    context.model = QStringLiteral("claude-sonnet-5");
    context.workdir = tree.path();
    const auto call = [&](const QString &id, const QString &name,
                          const QJsonObject &input, const QString &result,
                          qint64 startMs, qint64 endMs, bool isError = false) {
        ledger.ingest(context, toolUse(id, name, input), startMs);
        ledger.ingest(context, toolResult(id, result, isError), endMs);
    };
    const QJsonObject bigRead{{QStringLiteral("file_path"), QStringLiteral("big.txt")}};

    // A bounded read of a big file is exactly what the tool is for.
    call(QStringLiteral("p1"), QStringLiteral("Read"),
         QJsonObject{{QStringLiteral("file_path"), QStringLiteral("big.txt")},
                     {QStringLiteral("limit"), 5}},
         QStringLiteral("1\tbbbb\n"), 1000, 1010);
    check(log.signalCount() == 0 && changes == 0 && !QFile::exists(logPath),
          QStringLiteral("a bounded read logs nothing"));

    // The whole file came back: one whole-file-read signal, not also an
    // oversized-result one, appended to the JSONL log immediately.
    call(QStringLiteral("w1"), QStringLiteral("Read"), bigRead,
         QString(40000, QLatin1Char('b')), 2000, 2050);
    QVector<AgentToolImprovement> all = log.improvements();
    const AgentToolImprovement *whole =
        findSignal(all, AgentToolImprovementLog::KindWholeFileRead, QStringLiteral("read"));
    check(whole && whole->occurrences == 1 && whole->tokensAtStake == 10000 &&
              whole->detail.startsWith(QStringLiteral("big.txt")) &&
              !findSignal(all, AgentToolImprovementLog::KindOversizedResult) &&
              changes == 1 && countLines(logPath) == 1,
          QStringLiteral("a whole-file read is logged once, in realtime (lines=%1)")
              .arg(countLines(logPath)));

    // Reading it again in the same turn is also a repeat; after an edit it
    // is not, and after the turn ends it is not either.
    call(QStringLiteral("w2"), QStringLiteral("Read"), bigRead,
         QString(40000, QLatin1Char('b')), 3000, 3050);
    all = log.improvements();
    const AgentToolImprovement *repeat =
        findSignal(all, AgentToolImprovementLog::KindRepeatedCall);
    check(repeat && repeat->occurrences == 1 &&
              findSignal(all, AgentToolImprovementLog::KindWholeFileRead)->occurrences == 2,
          QStringLiteral("an identical read in the same turn is a repeated call"));
    call(QStringLiteral("e1"), QStringLiteral("Edit"),
         QJsonObject{{QStringLiteral("file_path"), QStringLiteral("src.cpp")},
                     {QStringLiteral("old_string"), QStringLiteral("a")},
                     {QStringLiteral("new_string"), QStringLiteral("b")}},
         QStringLiteral("ok"), 3100, 3110);
    call(QStringLiteral("w3"), QStringLiteral("Read"), bigRead,
         QString(40000, QLatin1Char('b')), 3200, 3250);
    check(findSignal(log.improvements(), AgentToolImprovementLog::KindRepeatedCall)
                  ->occurrences == 1,
          QStringLiteral("an edit in between makes the next read fresh"));
    log.forgetSession(3);
    call(QStringLiteral("w4"), QStringLiteral("Read"), bigRead,
         QString(40000, QLatin1Char('b')), 3300, 3350);
    check(findSignal(log.improvements(), AgentToolImprovementLog::KindRepeatedCall)
                  ->occurrences == 1,
          QStringLiteral("a new turn makes the next read fresh"));

    // Errors that differ only in numbers and paths fold into one signal.
    call(QStringLiteral("x1"), QStringLiteral("Bash"),
         QJsonObject{{QStringLiteral("command"), QStringLiteral("make")}},
         QStringLiteral("<tool_use_error>File /tmp/a.txt has 12 lines</tool_use_error>"),
         4000, 4010, true);
    call(QStringLiteral("x2"), QStringLiteral("Bash"),
         QJsonObject{{QStringLiteral("command"), QStringLiteral("make")}},
         QStringLiteral("File /home/b/c.txt has 7 lines\nmore"), 4100, 4110, true);
    const AgentToolImprovement *error =
        findSignal(log.improvements(), AgentToolImprovementLog::KindToolError);
    check(error && error->occurrences == 2 && error->family == QStringLiteral("bash") &&
              error->detail.contains(QStringLiteral("c.txt has 7 lines")) &&
              !error->detail.contains(QStringLiteral("more")),
          QStringLiteral("errors group by normalized first line: %1")
              .arg(error ? error->signature : QString()));

    // Oversized output, an unknown tool, a slow call, and an unanswered call.
    call(QStringLiteral("o1"), QStringLiteral("Bash"),
         QJsonObject{{QStringLiteral("command"), QStringLiteral("cat build.log")}},
         QString(40000, QLatin1Char('l')), 5000, 5010);
    call(QStringLiteral("u1"), QStringLiteral("FrobnicateWidget"), QJsonObject(),
         QStringLiteral("ok"), 5100, 5110);
    call(QStringLiteral("s1"), QStringLiteral("Glob"),
         QJsonObject{{QStringLiteral("pattern"), QStringLiteral("**/*.cpp")}},
         QStringLiteral("a.cpp"), 6000, 6000 + AgentToolImprovementLog::kSlowCallMs);
    ledger.ingest(context, toolUse(QStringLiteral("a1"), QStringLiteral("WebSearch"),
                                   QJsonObject{{QStringLiteral("query"), QStringLiteral("qt")}}),
                  70000);
    ledger.abandonSession(3, 70500);
    all = log.improvements();
    const AgentToolImprovement *oversized =
        findSignal(all, AgentToolImprovementLog::KindOversizedResult, QStringLiteral("bash"));
    check(oversized && oversized->tokensAtStake == 10000 &&
              oversized->detail.startsWith(QStringLiteral("cat build.log")),
          QStringLiteral("oversized shell output is logged with its command"));
    const AgentToolImprovement *unknown = findSignal(all, AgentToolImprovementLog::KindUncatalogued);
    check(unknown && unknown->runtimeName == QStringLiteral("FrobnicateWidget") &&
              unknown->family == QStringLiteral("other"),
          QStringLiteral("an uncatalogued runtime tool is logged"));
    check(findSignal(all, AgentToolImprovementLog::KindSlowCall, QStringLiteral("glob")),
          QStringLiteral("a minute-long glob is a slow call"));
    const AgentToolImprovement *abandoned =
        findSignal(all, AgentToolImprovementLog::KindAbandonedCall);
    check(abandoned && abandoned->lastMs == 70500 && ledger.totals(70500).inFlight == 0,
          QStringLiteral("a call left unanswered at turn end is logged"));
    check(all.first().kind == AgentToolImprovementLog::KindAbandonedCall,
          QStringLiteral("the newest signal is listed first"));

    // Calls while a family is off are policy gaps.
    ledger.setDisabledFamilies({QStringLiteral("glob")});
    call(QStringLiteral("f1"), QStringLiteral("Glob"),
         QJsonObject{{QStringLiteral("pattern"), QStringLiteral("*.h")}},
         QStringLiteral("a.h"), 80000, 80010);
    check(findSignal(log.improvements(), AgentToolImprovementLog::KindCalledWhileOff,
                     QStringLiteral("glob")),
          QStringLiteral("a call to a switched-off family is logged"));

    // A parallel batch is released in order: a quick call behind a slow one
    // arrives with it and is not slow itself; a later sibling still can be.
    const auto bashCommand = [](const QString &command) {
        return QJsonObject{{QStringLiteral("command"), command}};
    };
    const qint64 batchStart = 90000;
    const qint64 batchEnd = batchStart + AgentToolImprovementLog::kSlowCallMs + 1000;
    ledger.ingest(context, toolUse(QStringLiteral("b1"), QStringLiteral("Bash"),
                                   bashCommand(QStringLiteral("pytest -q"))),
                  batchStart);
    ledger.ingest(context, toolUse(QStringLiteral("b2"), QStringLiteral("Bash"),
                                   bashCommand(QStringLiteral("git log -1"))),
                  batchStart + 10);
    ledger.ingest(context, toolUse(QStringLiteral("b3"), QStringLiteral("Bash"),
                                   bashCommand(QStringLiteral("ninja"))),
                  batchStart + 20);
    ledger.ingest(context, toolResult(QStringLiteral("b1"), QStringLiteral("ok")), batchEnd);
    ledger.ingest(context, toolResult(QStringLiteral("b2"), QStringLiteral("ok")), batchEnd + 5);
    ledger.ingest(context, toolResult(QStringLiteral("b3"), QStringLiteral("ok")),
                  batchEnd + 20000);
    int slowShellCalls = 0;
    bool slowGitLog = false;
    const AgentToolImprovement *slowNinja = nullptr;
    for (const AgentToolImprovement &signal : log.improvements()) {
        if (signal.kind != AgentToolImprovementLog::KindSlowCall ||
            signal.family != QStringLiteral("bash"))
            continue;
        slowShellCalls += int(signal.occurrences);
        slowGitLog = slowGitLog || signal.topic.startsWith(QStringLiteral("git"));
        if (signal.topic == QStringLiteral("ninja"))
            slowNinja = &signal;
    }
    check(slowShellCalls == 2 && !slowGitLog && slowNinja &&
              slowNinja->occurrences == 1 &&
              slowNinja->detail.startsWith(QStringLiteral("ninja")),
          QStringLiteral("a call held behind a slow sibling is not a slow call, and "
                         "slow shell calls fold by what ran (%1 calls)")
              .arg(slowShellCalls));

    // The digest ranks by context at stake and points at the log.
    const QString digest = log.digestMarkdown();
    check(digest.startsWith(QStringLiteral("# Tool efficiency: potential improvements")) &&
              digest.indexOf(QStringLiteral("Whole-file read")) <
                  digest.indexOf(QStringLiteral("Uncatalogued tool")) &&
              digest.contains(QStringLiteral("Suggestion: ")) &&
              digest.contains(logPath),
          QStringLiteral("the digest is ready to hand back to an agent"));

    // Reloading the JSONL rebuilds the same aggregates; torn lines are skipped.
    const int signalCount = log.signalCount();
    const qint64 occurrences = log.occurrenceCount();
    check(countLines(logPath) == occurrences,
          QStringLiteral("every occurrence is one JSONL line (%1 vs %2)")
              .arg(countLines(logPath))
              .arg(occurrences));
    {
        QFile file(logPath);
        file.open(QIODevice::Append);
        file.write("{\"schema\":1,\"kind\":\"tool-error\",\"ms\":9");
    }
    AgentToolImprovementLog reloaded(logPath);
    QString error2;
    check(reloaded.load(&error2) && reloaded.signalCount() == signalCount &&
              reloaded.occurrenceCount() == occurrences &&
              findSignal(reloaded.improvements(), AgentToolImprovementLog::KindToolError)
                      ->occurrences == 2,
          QStringLiteral("the log reloads its aggregates and skips a torn line: %1")
              .arg(error2));
    reloaded.clear();
    check(reloaded.signalCount() == 0 && !QFile::exists(logPath),
          QStringLiteral("clearing forgets every signal and deletes the log"));

    // Lines a week older than the newest are aged out on load; handing the
    // signals off archives the log so the next digest starts afresh.
    {
        QFile file(logPath);
        file.open(QIODevice::WriteOnly);
        const auto line = [](qint64 ms, const char *detail) {
            return QJsonDocument(QJsonObject{
                                     {QStringLiteral("schema"), 1},
                                     {QStringLiteral("kind"), QStringLiteral("tool-error")},
                                     {QStringLiteral("family"), QStringLiteral("bash")},
                                     {QStringLiteral("tool"), QStringLiteral("Bash")},
                                     {QStringLiteral("ms"), double(ms)},
                                     {QStringLiteral("detail"), QString::fromLatin1(detail)},
                                 })
                       .toJson(QJsonDocument::Compact) +
                   '\n';
        };
        const qint64 newest = 1789000000000LL;
        file.write(line(newest - AgentToolImprovementLog::kMaxAgeMs - 1, "stale"));
        file.write(line(newest - 1000, "recent"));
        file.write(line(newest, "newest"));
    }
    AgentToolImprovementLog aged(logPath);
    check(aged.load() && aged.occurrenceCount() == 2 &&
              aged.improvements().first().detail == QStringLiteral("newest") &&
              aged.digestMarkdown().contains(QStringLiteral("last seen ")),
          QStringLiteral("occurrences a week older than the newest are aged out"));
    const QString sentPath = aged.sentLogPath();
    check(aged.digestMarkdown(25, sentPath).contains(sentPath) &&
              aged.archiveSent() && aged.signalCount() == 0 &&
              !QFile::exists(logPath) && countLines(sentPath) == 3,
          QStringLiteral("handing signals off archives the log and starts afresh"));
    AgentToolImprovementLog afterHandOff(logPath);
    check(afterHandOff.load() && afterHandOff.signalCount() == 0,
          QStringLiteral("archived signals do not come back on the next launch"));
    QFile::remove(sentPath);

    // The in-memory view stays bounded.
    AgentToolImprovementLog bounded;
    for (int i = 0; i < AgentToolImprovementLog::kMaxSignals + 30; ++i) {
        AgentToolCallObservation observation;
        observation.family = QStringLiteral("bash");
        observation.runtimeName = QStringLiteral("Bash");
        observation.isError = true;
        QString word;
        for (int n = i; ; n /= 26) {
            word += QChar(QLatin1Char('a').unicode() + n % 26);
            if (n < 26)
                break;
        }
        observation.resultText = QStringLiteral("unknown option %1").arg(word);
        observation.startedMs = 1;
        observation.completedMs = 10 + i;
        bounded.observe(observation);
    }
    check(bounded.signalCount() == AgentToolImprovementLog::kMaxSignals &&
              bounded.improvements().first().lastMs ==
                  10 + AgentToolImprovementLog::kMaxSignals + 29,
          QStringLiteral("the signal table is capped, evicting the stalest"));
}

} // namespace

// Shell failures fold by the command that ran and the kind of failure, show
// the line that says what went wrong rather than "Exit code N", carry the
// fix for that kind, and leave a failing test run out of the signals.
void testShellFailureSignals()
{
    QTemporaryDir tree;
    const QString logPath =
        QDir(tree.path()).filePath(QStringLiteral("tool-improvements.jsonl"));
    AgentToolEfficiencyLedger ledger;
    AgentToolImprovementLog log(logPath);
    ledger.callObserved = [&log](const AgentToolCallObservation &call) {
        log.observe(call);
    };
    AgentToolEfficiencyLedger::SessionContext context;
    context.sessionId = 7;
    context.provider = QStringLiteral("claude-code");
    context.workdir = tree.path();
    qint64 clock = 1000;
    const auto fail = [&](const QString &id, const QString &command,
                          const QString &result) {
        ledger.ingest(context,
                      toolUse(id, QStringLiteral("Bash"),
                              QJsonObject{{QStringLiteral("command"), command}}),
                      clock);
        ledger.ingest(context, toolResult(id, result, true), clock + 10);
        clock += 100;
    };
    const auto errorSignal = [&](const QString &topic) -> const AgentToolImprovement * {
        static QVector<AgentToolImprovement> all;
        all = log.improvements();
        for (const AgentToolImprovement &signal : std::as_const(all))
            if (signal.kind == AgentToolImprovementLog::KindToolError &&
                signal.topic == topic)
                return &signal;
        return nullptr;
    };

    // A cd in an earlier call moved the working directory: two different
    // commands, one missing-path signal per verb, with the real error line.
    fail(QStringLiteral("m1"), QStringLiteral("cd app && ls src"), QStringLiteral("ok"));
    fail(QStringLiteral("m2"), QStringLiteral("sed -n 531,935p app/src/mirrors.py"),
         QStringLiteral("Exit code 2\nsed: can't read app/src/mirrors.py: No such "
                        "file or directory\n"));
    fail(QStringLiteral("m3"), QStringLiteral("sed -n 1,80p app/tests/test_x.py"),
         QStringLiteral("Exit code 2\nsed: can't read app/tests/test_x.py: No such "
                        "file or directory\n"));
    const AgentToolImprovement *missing = errorSignal(QStringLiteral("sed · missing path"));
    check(missing && missing->occurrences == 2 &&
              missing->detail.contains(QStringLiteral("exit 2: sed: can't read")) &&
              !missing->detail.contains(QStringLiteral("Exit code")) &&
              missing->suggestion.contains(QStringLiteral("working directory persists")),
          QStringLiteral("missing paths fold per verb and show the error line: %1")
              .arg(missing ? missing->detail : QStringLiteral("(none)")));

    // pkill matched the shell running it.
    fail(QStringLiteral("k1"),
         QStringLiteral("pkill -f \"forkmesh-window-tests\"; sleep 1; pgrep -fa tests"),
         QStringLiteral("Exit code 144\n"));
    const AgentToolImprovement *selfKill =
        errorSignal(QStringLiteral("pkill · killed its own shell"));
    check(selfKill && selfKill->detail.endsWith(QStringLiteral("exit 144")) &&
              selfKill->suggestion.contains(QStringLiteral("[f]oo")),
          QStringLiteral("a self-killing pkill is its own signal"));

    // Inline scripts: the traceback's last line is the message; cd, variable
    // assignments and timeout prefixes do not change the verb.
    fail(QStringLiteral("p1"),
         QStringLiteral("T=/tmp/x && mkdir -p $T && python3 - <<EOF\nprint(1/0)\nEOF"),
         QStringLiteral("Exit code 1\nTraceback (most recent call last):\n  File "
                        "\"<stdin>\", line 1, in <module>\nZeroDivisionError: "
                        "division by zero\n"));
    fail(QStringLiteral("p2"),
         QStringLiteral("cd /tmp/work; timeout 300 python3 -c 'import nope'"),
         QStringLiteral("Exit code 1\nTraceback (most recent call last):\n"
                        "ModuleNotFoundError: No module named 'nope'\n"));
    const AgentToolImprovement *inlineScript =
        errorSignal(QStringLiteral("python3 inline script · inline script failed"));
    check(inlineScript && inlineScript->occurrences == 2 &&
              inlineScript->detail.contains(QStringLiteral("ModuleNotFoundError")) &&
              inlineScript->suggestion.contains(QStringLiteral("run it by path")),
          QStringLiteral("failing heredoc scripts fold together with the traceback's "
                         "last line: %1")
              .arg(inlineScript ? inlineScript->detail : QStringLiteral("(none)")));

    // A probe that found nothing, a blocked sleep, and a timed-out command.
    fail(QStringLiteral("g1"),
         QStringLiteral("grep -rn \"needle\" desktop/src 2>/dev/null"),
         QStringLiteral("Exit code 1\n"));
    fail(QStringLiteral("s1"), QStringLiteral("sleep 60; tail -5 /tmp/build.log"),
         QStringLiteral("Blocked: sleep 60 followed by: tail -5 /tmp/build.log. To "
                        "wait for a condition, use Monitor."));
    fail(QStringLiteral("t1"),
         QStringLiteral("cd app && timeout 500 python3 -m pytest -q tests/test_slow.py"),
         QStringLiteral("Exit code 143\n"));
    check(errorSignal(QStringLiteral("grep · probe found nothing")) &&
              errorSignal(QStringLiteral("grep · probe found nothing"))
                  ->suggestion.contains(QStringLiteral("|| true")),
          QStringLiteral("a grep with no match is a probe miss"));
    check(errorSignal(QStringLiteral("sleep · blocked by the harness")),
          QStringLiteral("a blocked foreground sleep is its own signal"));
    check(errorSignal(QStringLiteral("pytest · timed out")),
          QStringLiteral("a killed test run is a timeout, not a test failure"));

    // A test run that reports failures is the tool working: no signal.
    const int before = log.signalCount();
    const qint64 occurrencesBefore = log.occurrenceCount();
    fail(QStringLiteral("t2"),
         QStringLiteral("cd app && timeout 900 python3 -m pytest -q tests/test_a.py"),
         QStringLiteral("Exit code 1\nFAILED tests/test_a.py::test_b\n"));
    fail(QStringLiteral("t3"), QStringLiteral("go test ./..."),
         QStringLiteral("Exit code 1\n--- FAIL: TestX\n"));
    check(log.signalCount() == before && log.occurrenceCount() == occurrencesBefore,
          QStringLiteral("a failing test run is not an improvement signal"));

    // A ';' chain reports its last command: the ls that missed, not the tail
    // that printed. A script written by heredoc and then run names the run.
    // An empty grep -n line number breaks sed in a recognisable way.
    fail(QStringLiteral("c1"),
         QStringLiteral("tail -1 /tmp/build.log; ls /tmp/build.done 2>/dev/null"),
         QStringLiteral("Exit code 2\n[ 85%] Building CXX object x.cpp.o\n"));
    fail(QStringLiteral("c2"),
         QStringLiteral("cat > /tmp/render_check.js <<'EOF'\nconst x = {\nEOF\n"
                        "node /tmp/render_check.js"),
         QStringLiteral("Exit code 1\nSyntaxError: Unexpected token '{'\n"));
    fail(QStringLiteral("c3"),
         QStringLiteral("grep -n \"def safe_segment\" app/src/*.py; sed -n "
                        "\"$(grep -n 'def safe_segment' app/src/catalog.py | cut -d: "
                        "-f1),+20p\" app/src/catalog.py"),
         QStringLiteral("Exit code 1\nsed: -e expression #1, char 1: unknown "
                        "command: `,'\n"));
    check(errorSignal(QStringLiteral("ls · probe found nothing")),
          QStringLiteral("a ';' chain is named after its last command"));
    check(errorSignal(QStringLiteral("node render_check.js · exit 1")),
          QStringLiteral("a heredoc body is skipped so the command after it is named"));
    check(errorSignal(QStringLiteral("sed · empty line number")) &&
              errorSignal(QStringLiteral("sed · empty line number"))
                  ->suggestion.contains(QStringLiteral("grep -n")),
          QStringLiteral("an empty grep -n line number handed to sed is its own signal"));

    // A foreground poll loop the bound killed, and a command the harness
    // would not run; a call the user interrupted is not a signal at all.
    fail(QStringLiteral("w1"),
         QStringLiteral("timeout 115 bash -c 'until [ -f /tmp/x.done ]; do sleep 5; done'"),
         QStringLiteral("Exit code 124\n"));
    fail(QStringLiteral("r1"),
         QStringLiteral("find app/src -name '*.py' | while read f; do echo $f; done"),
         QStringLiteral("Contains shell syntax (string) that cannot be statically "
                        "analyzed"));
    check(errorSignal(QStringLiteral("bash inline script · foreground wait")) &&
              errorSignal(QStringLiteral("bash inline script · foreground wait"))
                  ->suggestion.contains(QStringLiteral("Monitor")),
          QStringLiteral("a killed poll loop is a foreground wait"));
    check(errorSignal(QStringLiteral("find · refused by the harness")),
          QStringLiteral("a command the harness could not analyse is a refusal"));
    const int beforeInterrupt = log.signalCount();
    fail(QStringLiteral("i1"), QStringLiteral("python3 -m pytest app/tests/test_a.py"),
         QStringLiteral("[Request interrupted by user for tool use]"));
    check(log.signalCount() == beforeInterrupt,
          QStringLiteral("a call the user interrupted is not an improvement signal"));

    // Errors with no known class still fold by verb and normalized message.
    fail(QStringLiteral("o1"), QStringLiteral("git -C /tmp/a rebase main"),
         QStringLiteral("Exit code 128\nfatal: not a git repository: /tmp/a\n"));
    fail(QStringLiteral("o2"), QStringLiteral("git -C /tmp/b rebase main"),
         QStringLiteral("Exit code 128\nfatal: not a git repository: /tmp/b\n"));
    const AgentToolImprovement *git = errorSignal(QStringLiteral("git · exit 128"));
    check(git && git->occurrences == 2 &&
              git->detail.contains(QStringLiteral("exit 128: fatal: not a git")),
          QStringLiteral("unclassified errors fold by verb, exit code and message"));

    const int beforeOutcomes = log.signalCount();
    fail(QStringLiteral("x1"),
         QStringLiteral("/bin/bash -lc 'python3 -m pytest app/tests/test_a.py'"),
         QStringLiteral("Exit code 1\n=== 1 failed, 3 passed ===\n"));
    fail(QStringLiteral("x2"),
         QStringLiteral("/bin/bash -lc 'git -C /w/agent-1 merge main'"),
         QStringLiteral("Exit code 1\nAuto-merging a.py\nCONFLICT (content): Merge "
                        "conflict in a.py\nAutomatic merge failed; fix conflicts and "
                        "then commit the result.\n"));
    fail(QStringLiteral("x3"),
         QStringLiteral("S=/tmp/s && g++ -o $S/t desktop/tests/t.cpp && $S/t"),
         QStringLiteral("Exit code 1\nok - values load\nFAIL: values are trimmed\n"));
    fail(QStringLiteral("x4"),
         QStringLiteral("cd /tmp/s && python3 - <<'EOF'\nimport subprocess\nEOF"),
         QStringLiteral("Exit code 1\n/tmp/s/harness.cpp:104:47: error: 'f' was not "
                        "declared in this scope\n"));
    check(log.signalCount() == beforeOutcomes,
          QStringLiteral("wrapped test runs, merge conflicts, failing checks and "
                         "compile errors are outcomes"));

    fail(QStringLiteral("a1"), QStringLiteral("rg -n foo /home/u/docs"),
         QStringLiteral("This command requires approval"));
    fail(QStringLiteral("a2"), QStringLiteral("L=/var/tmp/x; rm -f $L/*.log"),
         QStringLiteral("Dangerous rm operation detected in `rm -f $L/*.log`. The "
                        "target '$L/*.log' is a shell variable expansion"));
    fail(QStringLiteral("a3"), QStringLiteral("tail -2 /tmp/a.txt /tmp/b.txt"),
         QStringLiteral("Exit code 1\ntail: option used in invalid context -- 2\n"));
    check(errorSignal(QStringLiteral("rg · refused by the harness")),
          QStringLiteral("a command awaiting approval is a refusal"));
    check(errorSignal(QStringLiteral("rm · unguarded delete")) &&
              errorSignal(QStringLiteral("rm · unguarded delete"))
                  ->suggestion.contains(QStringLiteral("fm_safe_rm_tree")),
          QStringLiteral("an rm the harness refused points at the guarded helper"));
    check(errorSignal(QStringLiteral("tail · tail -N on several files")),
          QStringLiteral("tail -N on several files is its own signal"));
    check(agentShellCommandShape(QStringLiteral(
              "env -u FORKMESH_HOME timeout -k 5 100 python3 tools/x.py")) ==
              QStringLiteral("python3 x.py"),
          QStringLiteral("env and timeout options with arguments are skipped"));
    check(agentShellCommandShape(QStringLiteral("/bin/bash -lc 'git status --short'")) ==
              QStringLiteral("git status"),
          QStringLiteral("a bash -lc launcher is unwrapped"));

    const int beforeCodexOutcomes = log.signalCount();
    fail(QStringLiteral("c1"),
         QStringLiteral("/bin/bash -lc \"python3 -m pytest app/tests/test_forkbot.py -q -k 'x'\""),
         QStringLiteral("F [100%]\nassert response[\"data\"][\"error\"] == \"model_not_found\"\n"
                        "Exit code: 1"));
    fail(QStringLiteral("c2"),
         QStringLiteral("/bin/bash -lc 'QT_QPA_PLATFORM=offscreen /s/build/forkmesh-window-tests'"),
         QStringLiteral("PASS: banner warns\nFAIL: banner writes the marker\nExit code: 1"));
    fail(QStringLiteral("c3"),
         QStringLiteral("/bin/bash -lc 'python3 tools/sync_header.py && git diff --check'"),
         QStringLiteral("desktop/src/A.h:12: trailing whitespace.\n+int a;  \nExit code: 2"));
    check(log.signalCount() == beforeCodexOutcomes,
          QStringLiteral("Codex results end with \"Exit code: N\", so its failing test "
                         "runs, test binaries and diff checks are outcomes"));

    fail(QStringLiteral("c4"),
         QStringLiteral("/bin/bash -lc \"sed -n '336,690p' native/GenieGravity.swift && "
                        "sed -n '580,650p' docs/GENIE.md && rg -n 'pull' docs/x.md\""),
         QStringLiteral("public enum GenieGravity {\n    QString error;\n}\nExit code: 1"));
    fail(QStringLiteral("c5"),
         QStringLiteral("cd /w; grep -n \"commentAgentRequests\" desktop/src/MainWindow.h; "
                        "[ -f desktop/src/X.cpp ] && echo yes"),
         QStringLiteral("Exit code 1\n11968: QList<CommentAgentRequest> commentAgentRequests;"));
    const AgentToolImprovement *chainMiss = errorSignal(QStringLiteral("rg · probe found nothing"));
    check(chainMiss && chainMiss->suggestion.contains(QStringLiteral("|| true")),
          QStringLiteral("a read-only chain that exits 1 is a probe miss named by its "
                         "last search, even when the output it printed says error"));
    check(errorSignal(QStringLiteral("[ · probe found nothing")),
          QStringLiteral("a test bracket ending a chain is the probe that missed"));
    fail(QStringLiteral("c6"), QStringLiteral("/bin/bash -lc 'swift test --filter X'"),
         QStringLiteral("/bin/bash: line 1: swift: command not found\n\nExit code: 127"));
    check(errorSignal(QStringLiteral("swift · command not found")),
          QStringLiteral("a missing command is its own signal"));

    const auto bigResult = [&](const QString &id, const QString &command, bool isError) {
        ledger.ingest(context,
                      toolUse(id, QStringLiteral("Bash"),
                              QJsonObject{{QStringLiteral("command"), command}}),
                      clock);
        ledger.ingest(context,
                      toolResult(id, QString(40000, QLatin1Char('x')) +
                                         (isError ? QStringLiteral("\nExit code: 1")
                                                  : QString()),
                                 isError),
                      clock + 10);
        clock += 100;
    };
    const auto oversized = [&](const QString &topic) -> const AgentToolImprovement * {
        static QVector<AgentToolImprovement> all;
        all = log.improvements();
        for (const AgentToolImprovement &signal : std::as_const(all))
            if (signal.kind == AgentToolImprovementLog::KindOversizedResult &&
                signal.topic == topic)
                return &signal;
        return nullptr;
    };
    bigResult(QStringLiteral("o1"),
              QStringLiteral("/bin/bash -lc \"python3 -m pytest app/tests/test_forkbot.py -vv -x\""),
              true);
    bigResult(QStringLiteral("o2"),
              QStringLiteral("/bin/bash -lc \"rg -n -m 100 'install-instance' .\""), false);
    check(oversized(QStringLiteral("test run")) &&
              oversized(QStringLiteral("test run"))->suggestion.contains(QStringLiteral("--tb=short")),
          QStringLiteral("a failing test run that floods the context is still oversized"));
    check(oversized(QStringLiteral("search")) &&
              oversized(QStringLiteral("search"))->suggestion.contains(QStringLiteral("per file")),
          QStringLiteral("an oversized search says -m caps matches per file"));

    const auto failTool = [&](const QString &id, const QString &name,
                              const QJsonObject &input, const QString &result) {
        ledger.ingest(context, toolUse(id, name, input), clock);
        ledger.ingest(context, toolResult(id, result, true), clock + 10);
        clock += 100;
    };
    failTool(QStringLiteral("w1"), QStringLiteral("Write"),
             QJsonObject{{QStringLiteral("file_path"), QStringLiteral("/w/a.js")},
                         {QStringLiteral("content"), QString(9000, QLatin1Char('y'))}},
             QStringLiteral("<tool_use_error>File has not been read yet. Read it first "
                            "before writing to it.</tool_use_error>"));
    failTool(QStringLiteral("t1"), QStringLiteral("Agent"),
             QJsonObject{{QStringLiteral("description"), QStringLiteral("Audit routes")}},
             QStringLiteral("Concurrent subagent limit reached. You can run 20 subagents "
                            "at once. Do not retry."));
    check(errorSignal(QStringLiteral("write before read")) &&
              errorSignal(QStringLiteral("write before read"))
                  ->suggestion.contains(QStringLiteral("Edit")),
          QStringLiteral("a Write refused before a Read is its own signal"));
    check(errorSignal(QStringLiteral("subagent limit")),
          QStringLiteral("the concurrent subagent cap is its own signal"));

    // The topic survives the JSONL round trip and reaches the digest.
    AgentToolImprovementLog reloaded(logPath);
    QString error;
    bool topics = reloaded.load(&error);
    for (const AgentToolImprovement &signal : reloaded.improvements())
        topics = topics && !signal.topic.isEmpty();
    check(topics && reloaded.signalCount() == log.signalCount() &&
              reloaded.digestMarkdown().contains(
                  QStringLiteral("(`Bash`) · sed · missing path · ×2")),
          QStringLiteral("topics reload from the log and head the digest: %1").arg(error));
}

void testSlowCallsAndRefusedEdits()
{
    QTemporaryDir tree;
    AgentToolEfficiencyLedger ledger;
    AgentToolImprovementLog log(QDir(tree.path()).filePath(QStringLiteral("t.jsonl")));
    ledger.callObserved = [&log](const AgentToolCallObservation &call) {
        log.observe(call);
    };
    AgentToolEfficiencyLedger::SessionContext context;
    context.sessionId = 9;
    context.provider = QStringLiteral("claude-code");
    context.workdir = tree.path();
    qint64 clock = 1000;
    const auto call = [&](const QString &id, const QString &name, const QJsonObject &input,
                          const QString &result, qint64 latency, bool isError) {
        ledger.ingest(context, toolUse(id, name, input), clock);
        ledger.ingest(context, toolResult(id, result, isError), clock + latency);
        clock += latency + 1000;
    };
    const auto slowShell = [&](const QString &id, const QString &command) {
        call(id, QStringLiteral("Bash"), QJsonObject{{QStringLiteral("command"), command}},
             QStringLiteral("ok"), 100000, false);
    };
    const auto find = [&](const QString &kind, const QString &signature)
        -> std::optional<AgentToolImprovement> {
        for (const AgentToolImprovement &signal : log.improvements())
            if (signal.kind == kind && signal.signature == signature)
                return signal;
        return std::nullopt;
    };

    slowShell(QStringLiteral("s1"),
              QStringLiteral("timeout 100 bash -c 'until grep -q \"^exit\" /var/tmp/a/go.log; "
                             "do sleep 3; done'; systemctl --user is-active fm-go; tail -3 "
                             "/var/tmp/a/go.log"));
    slowShell(QStringLiteral("s2"),
              QStringLiteral("until [ -s /tmp/x.txt ]; do sleep 10; done; cat /tmp/x.txt"));
    const auto wait = find(AgentToolImprovementLog::KindSlowCall,
                           QStringLiteral("foreground wait"));
    check(wait && wait->occurrences == 2 &&
              wait->suggestion.contains(QStringLiteral("run_in_background")),
          QStringLiteral("slow foreground waits fold under one signal"));

    slowShell(QStringLiteral("s3"),
              QStringLiteral("git log --oneline -S needle main | head; grep -rn needle docs"));
    slowShell(QStringLiteral("s4"), QStringLiteral("python3 - <<'EOF'\nimport time\nEOF\n"
                                                   "echo done | head -3"));
    check(find(AgentToolImprovementLog::KindSlowCall, QStringLiteral("git log")) &&
              find(AgentToolImprovementLog::KindSlowCall,
                   QStringLiteral("python3 inline script")),
          QStringLiteral("a slow chain is named after its first heavy command"));

    call(QStringLiteral("e1"), QStringLiteral("Edit"),
         QJsonObject{{QStringLiteral("file_path"), QStringLiteral("/w/a.cpp")},
                     {QStringLiteral("old_string"), QStringLiteral("a")},
                     {QStringLiteral("new_string"), QStringLiteral("b")}},
         QStringLiteral("Cannot write to /w/a.cpp while in plan mode."), 10, true);
    call(QStringLiteral("e2"), QStringLiteral("Write"),
         QJsonObject{{QStringLiteral("file_path"), QStringLiteral("/w/b.json")},
                     {QStringLiteral("content"), QStringLiteral("{}")}},
         QStringLiteral("Cannot write to /w/b.json while in plan mode."), 10, true);
    const QString deep = QStringLiteral("/home/u/projects/forkmesh/src/.worktrees/") + QString(200, QLatin1Char('d')) +
                         QStringLiteral("/c.json");
    call(QStringLiteral("e3"), QStringLiteral("Write"),
         QJsonObject{{QStringLiteral("file_path"), deep}, {QStringLiteral("content"), QStringLiteral("{}")}},
         QStringLiteral("Cannot write to %1 while in plan mode.").arg(deep), 10, true);
    call(QStringLiteral("r1"), QStringLiteral("Read"),
         QJsonObject{{QStringLiteral("file_path"), QStringLiteral("docs/coo.md")}},
         QStringLiteral("File does not exist. Note: your current working directory is /w."),
         10, true);
    const auto plan = find(AgentToolImprovementLog::KindToolError,
                           QStringLiteral("refused in plan mode"));
    qint64 refused = 0;
    for (const AgentToolImprovement &signal : log.improvements())
        if (signal.signature == QStringLiteral("refused in plan mode"))
            refused += signal.occurrences;
    check(plan && plan->topic == QStringLiteral("refused in plan mode") && refused == 3 &&
              plan->suggestion.contains(QStringLiteral("ExitPlanMode")),
          QStringLiteral("edits refused in plan mode name plan mode"));
    const auto guessed = find(AgentToolImprovementLog::KindToolError,
                              QStringLiteral("guessed path"));
    check(guessed && guessed->suggestion.contains(QStringLiteral("Glob")),
          QStringLiteral("a Read of a missing file is a guessed path"));
}

void testToolUsageRows()
{
    AgentToolEfficiencyLedger ledger;
    AgentToolEfficiencyLedger::SessionContext context;
    context.sessionId = 3;
    const auto bash = [](const QString &command) {
        return QJsonObject{{QStringLiteral("command"), command}};
    };
    ledger.ingest(context, toolUse(QStringLiteral("u1"), QStringLiteral("Bash"),
                                   bash(QStringLiteral("cd /x && git status"))),
                  1000);
    ledger.ingest(context, toolResult(QStringLiteral("u1"), QStringLiteral("clean")),
                  1100);
    ledger.ingest(context, toolUse(QStringLiteral("u2"), QStringLiteral("Bash"),
                                   bash(QStringLiteral("git status --short"))),
                  2000);
    ledger.ingest(context,
                  toolResult(QStringLiteral("u2"), QStringLiteral("fatal"), true),
                  2300);
    ledger.ingest(context, toolUse(QStringLiteral("u3"), QStringLiteral("Read"),
                                   QJsonObject{{QStringLiteral("file_path"),
                                                QStringLiteral("a.txt")}}),
                  3000);
    ledger.ingest(context, toolResult(QStringLiteral("u3"), QStringLiteral("text")),
                  3050);

    QHash<QString, AgentToolUsageStats> rows;
    for (const AgentToolUsageStats &row : ledger.usage())
        rows.insert(row.key, row);
    const AgentToolUsageStats git = rows.value(QStringLiteral("Bash · git status"));
    check(rows.size() == 2 && git.calls == 2 && git.errors == 1 &&
              git.command == QStringLiteral("git status") &&
              git.family == QStringLiteral("bash"),
          QStringLiteral("shell calls group by tool and command shape"));
    check(qFuzzyCompare(git.averageLatencyMs(), 200.0) &&
              qFuzzyCompare(git.errorRate(), 0.5) && git.latencyMsMax == 300,
          QStringLiteral("a usage row keeps latency and error rate"));
    check(rows.value(QStringLiteral("Read")).calls == 1 &&
              rows.value(QStringLiteral("Read")).command.isEmpty(),
          QStringLiteral("other tools group by runtime name"));

    AgentToolEfficiencyLedger restored;
    check(restored.fromJson(ledger.toJson()) && restored.usage().size() == 2,
          QStringLiteral("usage rows survive a save and load"));
    QJsonObject old = ledger.toJson();
    old.remove(QStringLiteral("usage"));
    check(restored.fromJson(old) && restored.usage().isEmpty(),
          QStringLiteral("a ledger without usage rows still loads"));
    ledger.clear();
    check(ledger.usage().isEmpty(), QStringLiteral("clear forgets usage rows"));
}

void testBackgroundSave()
{
    QTemporaryDir tree;
    check(tree.isValid(), QStringLiteral("temporary directory for background saves"));
    const QString path = QDir(tree.path()).filePath(QStringLiteral("nested/ledger.json"));
    AgentToolEfficiencyLedger ledger(path);
    const AgentToolEfficiencyLedger::SessionContext context{
        1, QStringLiteral("claude"), QStringLiteral("claude-sonnet-5"), tree.path()};
    ledger.beginCall(context, QStringLiteral("toolu_bg"), QStringLiteral("Bash"),
                     QJsonObject{{QStringLiteral("command"), QStringLiteral("ls")}},
                     1000);
    ledger.completeCall(1, QStringLiteral("toolu_bg"), QStringLiteral("ok"), false,
                        1200);
    ledger.saveLater();
    check(forkmesh::diskwrite::flush(10000),
          QStringLiteral("a background save drains"));
    AgentToolEfficiencyLedger reloaded(path);
    check(reloaded.load() && reloaded.hasSamples(),
          QStringLiteral("a background save lands on disk with its samples"));

    const QString other = QDir(tree.path()).filePath(QStringLiteral("order.txt"));
    for (int i = 0; i < 50; ++i)
        forkmesh::diskwrite::writeLater(other, QByteArray::number(i));
    check(forkmesh::diskwrite::flush(10000),
          QStringLiteral("queued writes drain"));
    QFile last(other);
    check(last.open(QIODevice::ReadOnly) && last.readAll() == "49",
          QStringLiteral("the newest queued write wins"));
    last.close();

    forkmesh::diskwrite::writeLater(other, QByteArrayLiteral("stale"));
    check(forkmesh::diskwrite::writeNow(other, QByteArrayLiteral("fresh")) &&
              forkmesh::diskwrite::flush(10000),
          QStringLiteral("a direct write succeeds while a queued one exists"));
    QFile fresh(other);
    check(fresh.open(QIODevice::ReadOnly) && fresh.readAll() == "fresh",
          QStringLiteral("a direct write is not overwritten by an older queued one"));
}

void testRunEfficiencyLog()
{
    QTemporaryDir tree;
    const QString path = QDir(tree.path()).filePath(QStringLiteral("agent-runs.jsonl"));
    AgentToolEfficiencyLedger ledger;
    AgentToolImprovementLog improvements;
    AgentRunEfficiencyLog runs(path);
    ledger.callObserved = [&](const AgentToolCallObservation &call) {
        runs.observeCall(call);
        improvements.observe(call);
    };
    improvements.occurred = [&](int sessionId, const QString &, qint64 tokens) {
        runs.noteSignal(sessionId, tokens);
    };
    const auto feed = [&](const AgentRunEfficiencyLog::RunContext &run, const QJsonObject &event,
                          qint64 at) {
        const QString type = event.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("result"))
            ledger.abandonSession(run.sessionId, at);
        runs.ingest(run, event, at);
        AgentToolEfficiencyLedger::SessionContext context;
        context.sessionId = run.sessionId;
        context.provider = run.provider;
        context.model = run.model;
        context.workdir = tree.path();
        ledger.ingest(context, event, at);
    };
    const auto assistantUsage = [](const QString &id, int input, int cacheRead,
                                   int cacheWrite, int output) {
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("assistant")},
            {QStringLiteral("message"),
             QJsonObject{{QStringLiteral("id"), id},
                         {QStringLiteral("role"), QStringLiteral("assistant")},
                         {QStringLiteral("content"), QJsonArray{}},
                         {QStringLiteral("usage"),
                          QJsonObject{{QStringLiteral("input_tokens"), input},
                                      {QStringLiteral("cache_read_input_tokens"), cacheRead},
                                      {QStringLiteral("cache_creation_input_tokens"), cacheWrite},
                                      {QStringLiteral("output_tokens"), output}}}}}};
    };
    const auto result = [](const QString &subtype, int durationMs) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                           {QStringLiteral("subtype"), subtype},
                           {QStringLiteral("is_error"), subtype != QLatin1String("success")},
                           {QStringLiteral("duration_ms"), durationMs}};
    };

    AgentRunEfficiencyLog::RunContext claude{4, QStringLiteral("Fix login"),
                                             QStringLiteral("claude-code"),
                                             QStringLiteral("claude-opus-5-5"), {}};
    feed(claude,
         QJsonObject{{QStringLiteral("type"), QStringLiteral("system")},
                     {QStringLiteral("subtype"), QStringLiteral("init")}},
         1000);
    feed(claude, assistantUsage(QStringLiteral("msg_1"), 100, 1000, 200, 50), 1100);
    feed(claude,
         toolUse(QStringLiteral("r1"), QStringLiteral("Bash"),
                 QJsonObject{{QStringLiteral("command"), QStringLiteral("git status --short")}}),
         1150);
    feed(claude, assistantUsage(QStringLiteral("msg_1"), 100, 1000, 200, 80), 1160);
    feed(claude, toolResult(QStringLiteral("r1"), QStringLiteral(" M a.cpp")), 1300);
    feed(claude, assistantUsage(QStringLiteral("msg_2"), 10, 1300, 0, 40), 1400);
    feed(claude, result(QStringLiteral("success"), 4000), 5000);
    check(runs.runs().size() == 1 && !runs.hasOpenRun(4),
          QStringLiteral("a result closes the run"));
    const AgentRunRecord first = runs.runs().value(0);
    check(first.modelCalls == 2 && first.inputTokens == 110 &&
              first.cachedInputTokens == 2300 && first.cacheWriteTokens == 200 &&
              first.outputTokens == 120,
          QStringLiteral("model usage sums per message id, keeping each message's largest count"));
    check(first.toolCalls == 1 && first.toolErrors == 0 && first.durationMs == 4000 &&
              first.outcome == QLatin1String("success") && first.estimatedUsd > 0.0 &&
              first.configLabel() == QLatin1String("All tools on"),
          QStringLiteral("a run records its tool calls, time, outcome and tool configuration"));

    AgentRunEfficiencyLog::RunContext offRead = claude;
    offRead.disabledFamilies = {QStringLiteral("read")};
    feed(offRead, assistantUsage(QStringLiteral("msg_3"), 20, 900, 0, 30), 6000);
    feed(offRead,
         toolUse(QStringLiteral("r2"), QStringLiteral("Bash"),
                 QJsonObject{{QStringLiteral("command"), QStringLiteral("cat big.log")}}),
         6100);
    feed(offRead, toolResult(QStringLiteral("r2"), QString(40000, QLatin1Char('x'))), 6200);
    feed(offRead,
         toolUse(QStringLiteral("r3"), QStringLiteral("Bash"),
                 QJsonObject{{QStringLiteral("command"), QStringLiteral("sleep 600")}}),
         6300);
    feed(offRead, result(QStringLiteral("error_max_turns"), 0), 9000);
    const AgentRunRecord second = runs.runs().value(1);
    check(second.toolCalls == 2 && second.signalCount >= 2 && second.tokensAtStake >= 10000 &&
              second.efficiency() < 0.5 && second.failed() &&
              second.durationMs == 3000 &&
              second.configLabel() == QLatin1String("Off: read"),
          QStringLiteral("an oversized result and an abandoned call count against the run: "
                         "%1 signals, %2 at stake")
              .arg(second.signalCount)
              .arg(second.tokensAtStake));

    AgentRunEfficiencyLog::RunContext codex{9, QStringLiteral("Port"), QStringLiteral("codex"),
                                            QStringLiteral("gpt-5.5"), {QStringLiteral("grep")}};
    const auto usage = [](int input, int cached, int output, int lastInput, int lastCached,
                          int lastOutput) {
        return QJsonObject{
            {QStringLiteral("type"), QStringLiteral("_codex_usage")},
            {QStringLiteral("tokenUsage"),
             QJsonObject{{QStringLiteral("total"),
                          QJsonObject{{QStringLiteral("inputTokens"), input},
                                      {QStringLiteral("cachedInputTokens"), cached},
                                      {QStringLiteral("outputTokens"), output}}},
                         {QStringLiteral("last"),
                          QJsonObject{{QStringLiteral("inputTokens"), lastInput},
                                      {QStringLiteral("cachedInputTokens"), lastCached},
                                      {QStringLiteral("outputTokens"), lastOutput}}}}}};
    };
    feed(codex, usage(5000, 2000, 300, 1000, 400, 100), 10000);
    feed(codex, usage(6500, 3000, 500, 1500, 1000, 200), 11000);
    feed(codex, result(QStringLiteral("success"), 2500), 12000);
    feed(codex, usage(8000, 3600, 650, 1500, 600, 150), 13000);
    feed(codex, result(QStringLiteral("success"), 1000), 14000);
    const AgentRunRecord codexFirst = runs.runs().value(2);
    const AgentRunRecord codexSecond = runs.runs().value(3);
    check(codexFirst.modelCalls == 2 && codexFirst.cachedInputTokens == 1400 &&
              codexFirst.inputTokens == 1100 && codexFirst.outputTokens == 300,
          QStringLiteral("a Codex run counts from the total before its first response"));
    check(codexSecond.modelCalls == 1 && codexSecond.cachedInputTokens == 600 &&
              codexSecond.inputTokens == 900 && codexSecond.outputTokens == 150,
          QStringLiteral("the next Codex run counts from where the last one ended"));

    const QVector<AgentRunComparison> groups = runs.comparison();
    check(groups.size() == 3 && groups.at(0).provider == QLatin1String("claude-code") &&
              groups.at(0).allToolsOn() && groups.at(1).config == QLatin1String("Off: read") &&
              groups.at(1).failedRuns == 1 && groups.at(2).runs == 2,
          QStringLiteral("runs compare per runtime and tool configuration, all tools on first"));
    check(runs.comparisonMarkdown().contains(QStringLiteral("| codex | Off: grep | 2 |")),
          QStringLiteral("the comparison renders as a markdown table"));

    AgentRunEfficiencyLog reloaded(path);
    QString error;
    check(reloaded.load(&error) && reloaded.runs().size() == 4 &&
              reloaded.runs().at(1).tokensAtStake == second.tokensAtStake &&
              reloaded.runs().at(1).disabledFamilies == QStringList{QStringLiteral("read")} &&
              reloaded.runs().at(3).familyCalls.isEmpty() &&
              reloaded.runs().at(0).familyCalls.value(QStringLiteral("bash")) == 1,
          QStringLiteral("runs reload from agent-runs.jsonl: %1").arg(error));

    feed(claude, assistantUsage(QStringLiteral("msg_9"), 5, 5, 0, 5), 20000);
    feed(claude,
         QJsonObject{{QStringLiteral("type"), QStringLiteral("system")},
                     {QStringLiteral("subtype"), QStringLiteral("init")}},
         21000);
    check(runs.runs().size() == 5 && runs.runs().last().outcome == QLatin1String("no result") &&
              runs.hasOpenRun(4),
          QStringLiteral("a run cut off without a result closes when the next one starts"));
    runs.clear();
    check(runs.runs().isEmpty() && !QFile::exists(path),
          QStringLiteral("clear forgets runs and deletes the log"));
}

void testPerformanceHistory()
{
    const qint64 hourMs = 60 * 60 * 1000LL;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    AgentPerformanceHistory history;
    int changes = 0;
    history.changed = [&changes] { ++changes; };

    auto call = [](qint64 completedMs, qint64 latencyMs, bool isError, qint64 tokens) {
        AgentToolCallObservation observation;
        observation.family = QStringLiteral("bash");
        observation.startedMs = completedMs - latencyMs;
        observation.completedMs = completedMs;
        observation.isError = isError;
        observation.inputTokens = tokens / 4;
        observation.resultTokens = tokens - tokens / 4;
        return observation;
    };
    history.recordCall(call(now - 5 * 60 * 1000, 400, false, 1000));
    history.recordCall(call(now - 4 * 60 * 1000, 800, true, 3000));
    AgentToolCallObservation held = call(now - 3 * 60 * 1000, 9000, false, 0);
    held.heldBehindSibling = true;
    history.recordCall(held);
    history.recordCall(call(now - 90 * 60 * 1000, 1000, false, 2000));
    history.recordSignal(1000, now - 4 * 60 * 1000);

    const AgentPerformanceSeries calls =
        history.series(QStringLiteral("toolCalls"), AgentPerformanceHistory::Hour, now);
    check(calls.values.size() == 60 && calls.hasSummary && calls.summary == 3.0 &&
              calls.hasPreviousSummary && calls.previousSummary == 1.0 &&
              qAbs(calls.change() - 2.0) < 1e-9,
          QStringLiteral("tool calls sum per hour against the hour before"));
    const AgentPerformanceSeries errors =
        history.series(QStringLiteral("toolErrors"), AgentPerformanceHistory::Hour, now);
    check(qAbs(errors.summary - 100.0 / 3.0) < 1e-6 &&
              errors.previousSummary == 0.0 && std::isnan(errors.change()),
          QStringLiteral("the error rate is errors over calls, with no change against zero"));
    const AgentPerformanceSeries latency =
        history.series(QStringLiteral("toolLatency"), AgentPerformanceHistory::Hour, now);
    check(latency.summary == 600.0,
          QStringLiteral("a call held behind its sibling does not count toward call time"));
    const AgentPerformanceSeries efficiency =
        history.series(QStringLiteral("efficiency"), AgentPerformanceHistory::Hour, now);
    check(efficiency.summary == 75.0 && efficiency.previousSummary == 100.0,
          QStringLiteral("efficiency is one minus tokens at stake over tool tokens"));
    const AgentPerformanceSeries gauge =
        history.series(QStringLiteral("agents"), AgentPerformanceHistory::Hour, now);
    check(!gauge.hasSummary && std::isnan(gauge.values.last()),
          QStringLiteral("a gauge never sampled stays empty rather than zero"));
    const AgentPerformanceSeries weekCalls =
        history.series(QStringLiteral("toolCalls"), AgentPerformanceHistory::Week, now);
    check(weekCalls.values.size() == 168 && weekCalls.summary == 4.0 &&
              history.series(QStringLiteral("toolCalls"), AgentPerformanceHistory::Month, now)
                      .summary == 4.0,
          QStringLiteral("every call lands in each period's buckets"));

    AgentRunRecord run;
    run.outcome = QStringLiteral("error");
    run.endedMs = now - 2 * 60 * 1000;
    run.durationMs = 120000;
    run.estimatedUsd = 0.5;
    history.recordRun(run);
    run.outcome = QStringLiteral("success");
    run.durationMs = 60000;
    history.recordRun(run);
    const AgentPerformanceSeries runs =
        history.series(QStringLiteral("runs"), AgentPerformanceHistory::Day, now);
    check(runs.summary == 2.0 && runs.summaryNote == QLatin1String("2 runs, 1 failed") &&
              history.series(QStringLiteral("runTime"), AgentPerformanceHistory::Day, now)
                      .summary == 90000.0 &&
              history.series(QStringLiteral("cost"), AgentPerformanceHistory::Day, now)
                      .summary == 1.0,
          QStringLiteral("runs count with their failures, average time and cost"));

    AgentPerformanceHistory::SystemSample sample;
    sample.runningAgents = 2;
    sample.agentLimit = 4;
    sample.memoryPercent = 50.0;
    sample.cpuPercent = 20.0;
    sample.memoryTotalBytes = 16LL * 1024 * 1024 * 1024;
    sample.cpuCount = 8;
    sample.build = QStringLiteral("1.0");
    history.recordSample(sample, now - 2 * 60 * 1000);
    sample.runningAgents = 6;
    sample.agentLimit = 8;
    sample.memoryPercent = 30.0;
    sample.memoryTotalBytes = 32LL * 1024 * 1024 * 1024;
    sample.build = QStringLiteral("1.1");
    history.recordSample(sample, now - 60 * 1000);
    const AgentPerformanceSeries agents =
        history.series(QStringLiteral("agents"), AgentPerformanceHistory::Day, now);
    check(agents.summary == 4.0 && agents.summaryNote == QLatin1String("peak 6"),
          QStringLiteral("agents running averages samples and keeps the peak"));
    QStringList labels;
    for (const AgentPerformanceMarker &marker :
         history.markers(AgentPerformanceHistory::Hour, now))
        labels << marker.label;
    check(labels == QStringList{QStringLiteral("Memory 16 → 32 GB"),
                                QStringLiteral("Agent limit 4 → 8"),
                                QStringLiteral("Build 1.1")},
          QStringLiteral("memory, agent-limit and build changes become chart markers: %1")
              .arg(labels.join(QStringLiteral(", "))));
    check(history.summaryMarkdown(AgentPerformanceHistory::Hour, now)
                  .contains(QStringLiteral("| Tool calls | 3 | 1 | +200% |")),
          QStringLiteral("the summary renders as a markdown table"));
    check(changes > 0, QStringLiteral("every record notifies the owner"));

    QTemporaryDir dir;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("performance-history.json"));
    AgentPerformanceHistory saved(path);
    QString error;
    check(saved.fromJson(history.toJson(), &error) && saved.saveNow(&error),
          QStringLiteral("history saves: %1").arg(error));
    AgentPerformanceHistory reloaded(path);
    check(reloaded.load(&error) && reloaded.loadedFromDisk() &&
              reloaded.series(QStringLiteral("toolCalls"), AgentPerformanceHistory::Hour, now)
                      .summary == 3.0 &&
              reloaded.markers(AgentPerformanceHistory::Hour, now).size() == 3,
          QStringLiteral("history reloads its buckets and markers: %1").arg(error));
    sample.memoryTotalBytes = 32LL * 1024 * 1024 * 1024;
    reloaded.recordSample(sample, now);
    check(reloaded.markers(AgentPerformanceHistory::Hour, now).size() == 3,
          QStringLiteral("an unchanged environment after a reload adds no marker"));

    AgentPerformanceHistory seeded;
    AgentRunRecord old;
    old.outcome = QStringLiteral("success");
    old.endedMs = now - 3 * 24 * hourMs;
    old.durationMs = 1000;
    old.toolCalls = 10;
    old.toolErrors = 1;
    old.toolInputTokens = 500;
    old.toolResultTokens = 1500;
    old.tokensAtStake = 200;
    AgentRunRecord ancient = old;
    ancient.endedMs = now - 90 * 24 * hourMs;
    check(seeded.seedFromRuns({old, ancient}) &&
              seeded.series(QStringLiteral("toolCalls"), AgentPerformanceHistory::Week, now)
                      .summary == 10.0 &&
              seeded.series(QStringLiteral("efficiency"), AgentPerformanceHistory::Week, now)
                      .summary == 90.0 &&
              seeded.series(QStringLiteral("toolCalls"), AgentPerformanceHistory::Month, now)
                      .summary == 10.0 &&
              !seeded.seedFromRuns({old}),
          QStringLiteral("a new history backfills once from the run log, inside 60 days"));
}

int main()
{
    testToolPolicy();
    testCatalogCoverage();
    testRuntimeNameMapping();
    testPartialReadSavings();
    testAnchoredEditAndMatchedFiles();
    testLifecycleAndPersistence();
    testModelUsage();
    testPendingBound();
    testImprovementLog();
    testShellFailureSignals();
    testSlowCallsAndRefusedEdits();
    testToolUsageRows();
    testBackgroundSave();
    testRunEfficiencyLog();
    testPerformanceHistory();
    std::printf("%d failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
