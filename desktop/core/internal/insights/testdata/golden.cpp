#include "AgentGuardrailLedger.h"
#include "AgentToolCatalog.h"
#include "AgentToolEfficiencyLedger.h"
#include "AgentToolImprovementLog.h"
#include "AgentToolPolicy.h"
#include "LocalControlInsights.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>
#include <ctime>

namespace insightctl = forkmesh::localcontrol::insights;

namespace {

constexpr qint64 kBase = 1700000000000LL;
constexpr int kWorkdirLength = 160;

QString gWorkdir;
QString gRoot;

QString scrub(QString text)
{
    if (!gWorkdir.isEmpty())
        text.replace(gWorkdir, QStringLiteral("<W>"));
    if (!gRoot.isEmpty())
        text.replace(gRoot, QStringLiteral("<R>"));
    return text;
}

QString fill(QChar ch, int count)
{
    return QStringLiteral("@FILL:%1:%2@").arg(ch).arg(count);
}

QString expand(QString text)
{
    static const QRegularExpression pattern(QStringLiteral("@FILL:(.):(\\d+)@"));
    for (auto match = pattern.match(text); match.hasMatch(); match = pattern.match(text))
        text.replace(match.capturedStart(), match.capturedLength(),
                     QString(match.captured(2).toInt(), match.captured(1).at(0)));
    return text;
}

QJsonObject toolUseBlock(const QString &id, const QString &name, const QJsonObject &input)
{
    return {{QStringLiteral("type"), QStringLiteral("tool_use")},
            {QStringLiteral("id"), id},
            {QStringLiteral("name"), name},
            {QStringLiteral("input"), input}};
}

QJsonObject assistant(const QJsonArray &blocks)
{
    return {{QStringLiteral("type"), QStringLiteral("assistant")},
            {QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), blocks}}}};
}

QJsonObject resultBlock(const QString &id, const QJsonValue &content, bool isError)
{
    return {{QStringLiteral("type"), QStringLiteral("tool_result")},
            {QStringLiteral("tool_use_id"), id},
            {QStringLiteral("content"), content},
            {QStringLiteral("is_error"), isError}};
}

QJsonObject user(const QJsonArray &blocks)
{
    return {{QStringLiteral("type"), QStringLiteral("user")},
            {QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), blocks}}}};
}

QJsonObject op(int session, const QString &provider, const QString &model, bool workdir, qint64 now,
               const QJsonObject &event)
{
    return {{QStringLiteral("op"), QStringLiteral("event")},
            {QStringLiteral("session"), session},
            {QStringLiteral("provider"), provider},
            {QStringLiteral("model"), model},
            {QStringLiteral("workdir"), workdir},
            {QStringLiteral("now"), double(now)},
            {QStringLiteral("event"), event}};
}

QJsonArray buildOps()
{
    const QString p1 = QStringLiteral("claude-code");
    const QString m1 = QStringLiteral("Claude-Sonnet-4-5 ");
    const QString p2 = QStringLiteral("codex");
    const QString m2 = QStringLiteral("gpt-5-codex");
    QJsonArray ops;
    qint64 t = kBase;
    auto use1 = [&](qint64 at, const QString &id, const QString &name, const QJsonObject &input) {
        ops.append(op(1, p1, m1, true, at, assistant({toolUseBlock(id, name, input)})));
    };
    auto result1 = [&](qint64 at, const QString &id, const QJsonValue &content, bool error) {
        ops.append(op(1, p1, m1, true, at, user({resultBlock(id, content, error)})));
    };
    use1(t, QStringLiteral("a1"), QStringLiteral("Read"), {{QStringLiteral("file_path"), QStringLiteral("src/big.txt")}});
    result1(t + 1500, QStringLiteral("a1"), fill(QLatin1Char('x'), 39000), false);
    use1(t + 2000, QStringLiteral("a2"), QStringLiteral("Read"),
         {{QStringLiteral("file_path"), QStringLiteral("@W@/src/small.txt")},
          {QStringLiteral("offset"), 10},
          {QStringLiteral("limit"), 5}});
    result1(t + 2100, QStringLiteral("a2"), fill(QLatin1Char('y'), 100), false);
    ops.append(op(1, p1, m1, true, t + 3000,
                  assistant({toolUseBlock(QStringLiteral("a3"), QStringLiteral("Grep"),
                                          {{QStringLiteral("pattern"), QStringLiteral("foo")},
                                           {QStringLiteral("path"), QStringLiteral("src")}}),
                             toolUseBlock(QStringLiteral("a4"), QStringLiteral("Glob"),
                                          {{QStringLiteral("pattern"), QStringLiteral("**/*.md")}})})));
    ops.append(op(1, p1, m1, true, t + 9000,
                  user({resultBlock(QStringLiteral("a3"),
                                    QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                           {QStringLiteral("text"),
                                                            QStringLiteral("src/big.txt:12:foo\nsrc/small.txt:3:foo\n")}},
                                               QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                                           {QStringLiteral("text"), QStringLiteral("notes.md")}}},
                                    false)})));
    ops.append(op(1, p1, m1, true, t + 9400,
                  user({resultBlock(QStringLiteral("a4"), QStringLiteral("notes.md\nmissing.md\n"), false)})));
    const QList<QPair<QString, QString>> failures = {
        {QStringLiteral("cd /tmp && timeout 60 python3 -m pytest -q tests/test_x.py"),
         QStringLiteral("Exit code 1\n=== 3 failed ===")},
        {QStringLiteral("sed -n \"$(grep -n foo x.txt | cut -d: -f1),+5p\" x.txt"),
         QStringLiteral("Exit code 1\nsed: -e expression #1, char 1: unknown command: `,'")},
        {QStringLiteral("ls /nonexistent/dir"),
         QStringLiteral("Exit code 2\nls: cannot access '/nonexistent/dir': No such file or directory")},
        {QStringLiteral("pkill -f forkmesh-core"), QStringLiteral("Exit code 144")},
        {QStringLiteral("while true; do sleep 5; done"),
         QStringLiteral("Exit code 124\nCommand timed out after 2m 0s")},
        {QStringLiteral("python3 - <<'EOF'\nimport sys\nprint(1/0)\nEOF\necho done"),
         QStringLiteral("Exit code 1\nTraceback (most recent call last):\n  File \"<stdin>\", line 2\n"
                        "ZeroDivisionError: division by zero")},
        {QStringLiteral("python3 -c \"import json; json.loads('x')\""),
         QStringLiteral("Exit code 1\nTraceback (most recent call last):\njson.decoder.JSONDecodeError: "
                        "Expecting value: line 1 column 1 (char 0)")},
        {QStringLiteral("grep -rn needle src/"), QStringLiteral("Exit code 1")},
        {QStringLiteral("sleep 30 && curl -s localhost:8080"),
         QStringLiteral("<tool_use_error>Blocked: sleep 30 followed by curl -s localhost:8080. "
                        "Run blocking commands in the background</tool_use_error>")},
        {QStringLiteral("rm -rf build && cmake -S . -B build"),
         QStringLiteral("This command requires approval: cannot be statically analyzed")},
        {QStringLiteral("cargo build --release"),
         QStringLiteral("Exit code 101\nerror[E0425]: cannot find value `x` in this scope\n --> src/main.rs:12:5")},
        {QStringLiteral("npm run lint"),
         QStringLiteral("Exit code 2\n\n> app@1.0.0 lint\n> eslint src/components/App.tsx\n\n  12:5  error  "
                        "'foo' is not defined  no-undef")},
        {QStringLiteral("git push origin main"),
         QStringLiteral("[Request interrupted by user for tool use]")},
        {QStringLiteral("make check"), QStringLiteral("Exit code 2\nmake: *** [check] Error 1")},
        {QStringLiteral("/bin/bash -lc 'python3 -m pytest -q app/tests'"),
         QStringLiteral("Exit code 1\n=== 2 failed ===")},
        {QStringLiteral("/bin/bash -lc 'git merge main'"),
         QStringLiteral("Exit code 1\nAutomatic merge failed; fix conflicts and then commit the result.")},
        {QStringLiteral("g++ -o t t.cpp && ./t"), QStringLiteral("Exit code 1\nok - a\nFAIL: b")},
        {QStringLiteral("rg -n foo /x"), QStringLiteral("This command requires approval")},
        {QStringLiteral("L=/v; rm -f $L/*.log"),
         QStringLiteral("Dangerous rm operation detected in `rm -f $L/*.log`.")},
        {QStringLiteral("tail -2 a b"),
         QStringLiteral("Exit code 1\ntail: option used in invalid context -- 2")},
        {QStringLiteral("/bin/bash -lc \"python3 -m pytest app/tests/t.py -q -k 'x'\""),
         QStringLiteral("F [100%]\nassert x == y\nExit code: 1")},
        {QStringLiteral("/bin/bash -lc 'QT_QPA_PLATFORM=offscreen /s/forkmesh-window-tests'"),
         QStringLiteral("PASS: a\nFAIL: b\nExit code: 1")},
        {QStringLiteral("git diff --check"),
         QStringLiteral("a.h:3: trailing whitespace.\n+x  \nExit code: 2")},
        {QStringLiteral("/bin/bash -lc \"sed -n '1,5p' a.swift && rg -n pull docs/x.md\""),
         QStringLiteral("public enum G {\nQString error;\n}\nExit code: 1")},
        {QStringLiteral("cd /w; grep -n foo a.h; [ -f b.cpp ] && echo yes"),
         QStringLiteral("Exit code 1\n12: foo error")},
        {QStringLiteral("git worktree list | grep -c base 2>&1 | tail -n 2"),
         QStringLiteral("Exit code 1\n0")},
        {QStringLiteral("/bin/bash -lc 'swift test --filter X'"),
         QStringLiteral("/bin/bash: line 1: swift: command not found\n\nExit code: 127")},
        {QStringLiteral("git -C /w commit -m x || git status --short | head -n 5"),
         QStringLiteral("Exit code 1\nnothing to commit")},
    };
    int index = 0;
    for (const auto &failure : failures) {
        const QString id = QStringLiteral("b%1").arg(++index);
        const qint64 at = t + 10000 + index * 1000;
        use1(at, id, QStringLiteral("Bash"), {{QStringLiteral("command"), failure.first}});
        result1(at + 200, id, failure.second, true);
    }
    use1(t + 40000, QStringLiteral("c1"), QStringLiteral("Bash"),
         {{QStringLiteral("command"), QStringLiteral("cat <<EOF > /tmp/x\nhello\nEOF")}});
    result1(t + 40100, QStringLiteral("c1"), fill(QLatin1Char('z'), 40000), false);
    use1(t + 40200, QStringLiteral("c5"), QStringLiteral("Bash"),
         {{QStringLiteral("command"),
           QStringLiteral("/bin/bash -lc 'python3 -m pytest app/tests/t.py -vv -x'")}});
    result1(t + 40300, QStringLiteral("c5"),
            fill(QLatin1Char('q'), 40000) + QStringLiteral("\nExit code: 1"), true);
    use1(t + 40400, QStringLiteral("c6"), QStringLiteral("Bash"),
         {{QStringLiteral("command"), QStringLiteral("rg -n -m 100 install .")}});
    result1(t + 40500, QStringLiteral("c6"), fill(QLatin1Char('r'), 40000), false);
    use1(t + 40600, QStringLiteral("c7"), QStringLiteral("Write"),
         {{QStringLiteral("file_path"), QStringLiteral("src/w.js")},
          {QStringLiteral("content"), fill(QLatin1Char('w'), 9000)}});
    result1(t + 40700, QStringLiteral("c7"),
            QStringLiteral("<tool_use_error>File has not been read yet. Read it first before "
                           "writing to it.</tool_use_error>"),
            true);
    use1(t + 40800, QStringLiteral("c8"), QStringLiteral("Agent"),
         {{QStringLiteral("description"), QStringLiteral("Audit routes")}});
    result1(t + 40900, QStringLiteral("c8"),
            QStringLiteral("Concurrent subagent limit reached. You can run 20 subagents at once."),
            true);
    use1(t + 41000, QStringLiteral("c2"), QStringLiteral("Bash"),
         {{QStringLiteral("command"), QStringLiteral("make -j8 test")}});
    result1(t + 41000 + 75000, QStringLiteral("c2"), QStringLiteral("ok"), false);
    use1(t + 120000, QStringLiteral("c3"), QStringLiteral("Read"),
         {{QStringLiteral("file_path"), QStringLiteral("@W@/src/small.txt")},
          {QStringLiteral("offset"), 10},
          {QStringLiteral("limit"), 5}});
    result1(t + 120100, QStringLiteral("c3"), fill(QLatin1Char('y'), 100), false);
    use1(t + 121000, QStringLiteral("c4"), QStringLiteral("Edit"),
         {{QStringLiteral("file_path"), QStringLiteral("src/edit.cpp")},
          {QStringLiteral("old_string"), fill(QLatin1Char('x'), 50)},
          {QStringLiteral("new_string"), fill(QLatin1Char('y'), 8500)}});
    result1(t + 121300, QStringLiteral("c4"), QStringLiteral("The file was updated."), false);
    use1(t + 122000, QStringLiteral("c5"), QStringLiteral("mcp__github__create_issue"),
         {{QStringLiteral("title"), QStringLiteral("x")}});
    result1(t + 122500, QStringLiteral("c5"), QStringLiteral("created #12"), false);
    use1(t + 123000, QStringLiteral("c6"), QStringLiteral("FancyNewTool"), {});
    result1(t + 123100, QStringLiteral("c6"), QStringLiteral("Error: something odd happened at /home/u/x.py:12"),
            true);
    use1(t + 123200, QStringLiteral("c6p"), QStringLiteral("Write"),
         {{QStringLiteral("file_path"), QStringLiteral("@W@/plan.json")}, {QStringLiteral("content"), QStringLiteral("{}")}});
    result1(t + 123300, QStringLiteral("c6p"),
            QStringLiteral("Cannot write to /home/u/projects/forkmesh/src/.worktrees/"
                           "a-very-long-agent-branch-name/plan.json while in plan mode."),
            true);
    use1(t + 123400, QStringLiteral("c6m"), QStringLiteral("Read"),
         {{QStringLiteral("file_path"), QStringLiteral("docs/missing.md")}});
    result1(t + 123500, QStringLiteral("c6m"),
            QStringLiteral("File does not exist. Note: your current working directory is /w."), true);
    use1(t + 124000, QStringLiteral("c7"), QStringLiteral("Bash"),
         {{QStringLiteral("command"),
           QJsonArray{QStringLiteral("bash"), QStringLiteral("-lc"), QStringLiteral("git status")}}});
    result1(t + 124100, QStringLiteral("c7"), QStringLiteral("clean"), false);
    use1(t + 125000, QStringLiteral("c8"), QStringLiteral("Task"),
         {{QStringLiteral("description"), QStringLiteral("explore the repo")}});
    use1(t + 125100, QStringLiteral("c9"), QStringLiteral("Bash"),
         {{QStringLiteral("command"), QStringLiteral("go test ./...")}});
    use1(t + 125200, QStringLiteral(""), QStringLiteral("Read"), {});
    use1(t + 125300, QStringLiteral("c9"), QStringLiteral("Bash"), {});
    ops.append(op(1, p1, m1, true, t + 200000,
                  QJsonObject{{QStringLiteral("type"), QStringLiteral("result")}}));
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("disabled")},
                           {QStringLiteral("families"),
                            QJsonArray{QStringLiteral("web_search"), QStringLiteral("bash")}}});
    use1(t + 210000, QStringLiteral("d1"), QStringLiteral("WebSearch"),
         {{QStringLiteral("query"), QStringLiteral("qt json")}});
    result1(t + 212000, QStringLiteral("d1"), QStringLiteral("results"), false);
    use1(t + 213000, QStringLiteral("a1"), QStringLiteral("Read"),
         {{QStringLiteral("file_path"), QStringLiteral("notes.md")}});
    result1(t + 213500, QStringLiteral("a1"), fill(QLatin1Char('n'), 2000), false);
    auto use2 = [&](qint64 at, const QString &id, const QString &name, const QJsonObject &input) {
        ops.append(op(2, p2, m2, false, at, assistant({toolUseBlock(id, name, input)})));
    };
    auto result2 = [&](qint64 at, const QString &id, const QJsonValue &content, bool error) {
        ops.append(op(2, p2, m2, false, at, user({resultBlock(id, content, error)})));
    };
    use2(t + 300000, QStringLiteral("x1"), QStringLiteral("commandExecution"),
         {{QStringLiteral("command"), QStringLiteral("ls")}});
    result2(t + 300500, QStringLiteral("x1"), QStringLiteral("a\nb"), false);
    use2(t + 301000, QStringLiteral("x2"), QStringLiteral("playwright::browser_click"),
         {{QStringLiteral("ref"), QStringLiteral("e12")}});
    result2(t + 301500, QStringLiteral("x2"), QStringLiteral("clicked"), false);
    use2(t + 302000, QStringLiteral("x3"), QStringLiteral("lookup"),
         {{QStringLiteral("namespace"), QStringLiteral("memory_search")}});
    result2(t + 302200, QStringLiteral("x3"), QStringLiteral("none"), false);
    use2(t + 303000, QStringLiteral("x4"), QStringLiteral("shell"),
         {{QStringLiteral("command"), QStringLiteral("rg -n TODO")}});
    ops.append(op(2, p2, m2, false, t + 2 * 3600 * 1000,
                  QJsonObject{{QStringLiteral("type"), QStringLiteral("_codex_agent_complete")}}));
    use2(t + 2 * 3600 * 1000 + 5000, QStringLiteral("x5"), QStringLiteral("apply_patch"),
         {{QStringLiteral("changes"),
           QJsonArray{QJsonObject{{QStringLiteral("path"), QStringLiteral("@W@/src/edit.cpp")}},
                      QJsonObject{{QStringLiteral("path"), QStringLiteral("@W@/src/edit.cpp")}},
                      QJsonObject{{QStringLiteral("path"), QStringLiteral("@W@/notes.md")}}}}});
    result2(t + 2 * 3600 * 1000 + 5100, QStringLiteral("x5"), QStringLiteral("applied"), false);
    return ops;
}

QJsonValue substitute(const QJsonValue &value, const QString &workdir)
{
    if (value.isString())
        return expand(value.toString().replace(QStringLiteral("@W@"), workdir));
    if (value.isArray()) {
        QJsonArray out;
        for (const QJsonValue &item : value.toArray())
            out.append(substitute(item, workdir));
        return out;
    }
    if (value.isObject()) {
        QJsonObject out;
        const QJsonObject object = value.toObject();
        for (auto it = object.begin(); it != object.end(); ++it)
            out.insert(it.key(), substitute(it.value(), workdir));
        return out;
    }
    return value;
}

QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.readAll());
}

QJsonObject familyRows(const AgentToolEfficiencyLedger &ledger)
{
    QJsonObject rows;
    const QStringList disabled = QStringList(ledger.disabledFamilies().cbegin(), ledger.disabledFamilies().cend());
    for (const AgentToolFamilyStats &stats : ledger.allStats()) {
        const AgentToolFamily *family = agentToolFamily(stats.family);
        rows.insert(stats.family, insightctl::familyRow(stats, family, disabled.contains(stats.family),
                                                        AgentToolPolicy::enforcementNote(stats.family)));
    }
    return rows;
}

QJsonObject ledgerScenario(const QString &root)
{
    QJsonObject out;
    const QString workdir = QDir(root).filePath(QString(kWorkdirLength - root.size() - 1, QLatin1Char('w')));
    gWorkdir = workdir;
    gRoot = root;
    QJsonArray files;
    const QList<QPair<QString, int>> sizes = {{QStringLiteral("src/big.txt"), 40000},
                                              {QStringLiteral("src/small.txt"), 1000},
                                              {QStringLiteral("src/edit.cpp"), 9000},
                                              {QStringLiteral("notes.md"), 12000}};
    for (const auto &entry : sizes) {
        const QString path = QDir(workdir).filePath(entry.first);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write(QByteArray(entry.second, 'q'));
        files.append(QJsonObject{{QStringLiteral("path"), entry.first}, {QStringLiteral("size"), entry.second}});
    }
    out.insert(QStringLiteral("files"), files);
    out.insert(QStringLiteral("workdirLength"), kWorkdirLength);
    const QJsonArray ops = buildOps();
    out.insert(QStringLiteral("ops"), ops);

    const QString ledgerPath = QDir(root).filePath(QStringLiteral("tool-efficiency.json"));
    const QString logPath = QDir(root).filePath(QStringLiteral("tool-improvements.jsonl"));
    AgentToolEfficiencyLedger ledger(ledgerPath);
    AgentToolImprovementLog log(logPath);
    ledger.callObserved = [&log](const AgentToolCallObservation &call) { log.observe(call); };
    QJsonArray changes;
    for (const QJsonValue &value : ops) {
        const QJsonObject o = value.toObject();
        if (o.value(QStringLiteral("op")).toString() == QLatin1String("disabled")) {
            QSet<QString> set;
            for (const QJsonValue &family : o.value(QStringLiteral("families")).toArray())
                set.insert(family.toString());
            ledger.setDisabledFamilies(set);
            continue;
        }
        const QJsonObject ev = substitute(o.value(QStringLiteral("event")), workdir).toObject();
        const int session = o.value(QStringLiteral("session")).toInt();
        const qint64 now = qint64(o.value(QStringLiteral("now")).toDouble());
        const QString type = ev.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("result") || type == QLatin1String("_codex_agent_complete")) {
            ledger.abandonSession(session, now);
            log.forgetSession(session);
            changes.append(QJsonValue::Null);
            continue;
        }
        AgentToolEfficiencyLedger::SessionContext context;
        context.sessionId = session;
        context.provider = o.value(QStringLiteral("provider")).toString();
        context.model = o.value(QStringLiteral("model")).toString();
        if (o.value(QStringLiteral("workdir")).toBool())
            context.workdir = workdir;
        changes.append(ledger.ingest(context, ev, now));
    }
    out.insert(QStringLiteral("changed"), changes);
    const qint64 end = kBase + 3 * 3600 * 1000;
    out.insert(QStringLiteral("end"), double(end));
    out.insert(QStringLiteral("ledger"),
               scrub(QString::fromUtf8(QJsonDocument(ledger.toJson()).toJson(QJsonDocument::Compact))));
    out.insert(QStringLiteral("totals"), insightctl::totalsRow(ledger.totals(end)));
    out.insert(QStringLiteral("families"), familyRows(ledger));
    QJsonObject usage;
    for (const AgentToolUsageStats &u : ledger.usage()) {
        QJsonObject row = insightctl::usageRow(u);
        row.insert(QStringLiteral("command"), u.command);
        usage.insert(u.key, row);
    }
    out.insert(QStringLiteral("usage"), usage);
    QJsonObject models;
    for (const AgentToolModelStats &m : ledger.modelUsage())
        models.insert(AgentToolEfficiencyLedger::modelUsageKey(m.provider, m.model),
                      QJsonObject{{QStringLiteral("calls"), double(m.calls)},
                                  {QStringLiteral("errors"), double(m.errors)},
                                  {QStringLiteral("consumedUsd"), m.consumedUsd},
                                  {QStringLiteral("savedTokens"), double(m.savedTokens)},
                                  {QStringLiteral("latencyMsTotal"), double(m.latencyMsTotal)}});
    out.insert(QStringLiteral("models"), models);
    out.insert(QStringLiteral("log"), scrub(readAll(logPath)));
    QJsonArray improvements;
    for (const AgentToolImprovement &item : log.improvements()) {
        QJsonObject row = insightctl::improvementRow(item);
        row.insert(QStringLiteral("signature"), item.signature);
        row.insert(QStringLiteral("firstMs"), double(item.firstMs));
        improvements.append(QJsonValue(scrub(QString::fromUtf8(
            QJsonDocument(row).toJson(QJsonDocument::Compact)))));
    }
    out.insert(QStringLiteral("improvements"), improvements);
    out.insert(QStringLiteral("occurrences"), double(log.occurrenceCount()));
    out.insert(QStringLiteral("digest"), scrub(log.digestMarkdown(5, QString())));
    out.insert(QStringLiteral("digestAll"), scrub(log.digestMarkdown(100, QStringLiteral("/x/sent.jsonl"))));

    AgentToolImprovementLog reloaded(logPath);
    reloaded.load();
    QJsonArray reloadedRows;
    for (const AgentToolImprovement &item : reloaded.improvements())
        reloadedRows.append(QJsonValue(scrub(QString::fromUtf8(
            QJsonDocument(insightctl::improvementRow(item)).toJson(QJsonDocument::Compact)))));
    out.insert(QStringLiteral("reloaded"), reloadedRows);

    ledger.save();
    AgentToolEfficiencyLedger loaded(ledgerPath);
    QString error;
    loaded.load(&error);
    out.insert(QStringLiteral("loaded"),
               scrub(QString::fromUtf8(QJsonDocument(loaded.toJson()).toJson(QJsonDocument::Compact))));
    gWorkdir.clear();
    gRoot.clear();
    return out;
}

QJsonObject ledgerLoads(const QString &root)
{
    QJsonObject out;
    const QStringList inputs = {
        QStringLiteral(
            "{\"schema\":1,\"totalCalls\":7,\"firstCallMs\":5,\"lastCallMs\":9.7,\"families\":{"
            "\"read\":{\"calls\":4,\"errors\":1,\"measuredCalls\":2,\"latencyMsTotal\":100,\"latencyMsMax\":60,"
            "\"latencySamples\":3,\"inputTokens\":10,\"resultTokens\":20,\"baselineTokens\":300,\"savedTokens\":250,"
            "\"savedUsd\":-1,\"consumedUsd\":0.25,\"lastCallMs\":99,\"callsWhileOff\":1,"
            "\"recentSaved\":[1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26],"
            "\"runtimeNames\":{\"Read\":3,\"Zero\":0,\"Neg\":-2,\"Str\":\"4\",\"Frac\":2.7}},"
            "\"bogus\":{\"calls\":\"x\"}},"
            "\"usage\":[{\"tool\":\"Bash\",\"command\":\"git status\",\"family\":\"bash\",\"calls\":3,\"errors\":9,"
            "\"latencyMsTotal\":30,\"consumedUsd\":0.5},{\"tool\":\"\",\"calls\":3},{\"tool\":\"Read\",\"calls\":0},"
            "{\"tool\":\"Read\",\"calls\":2.9,\"lastCallMs\":-4}],"
            "\"models\":[{\"provider\":\" claude-code \",\"model\":\" Opus \",\"calls\":2,\"errors\":5},"
            "{\"provider\":\"\",\"model\":\"x\",\"calls\":1}],"
            "\"activity\":{\"bucketStartMs\":1200000,\"calls\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1,2],"
            "\"saved\":[0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,5]}}"),
        QStringLiteral("{\"schema\":2,\"families\":{}}"),
        QStringLiteral("{\"schema\":1}"),
        QStringLiteral("{\"schema\":1,\"families\":{\"read\":{\"calls\":-1}}}"),
        QStringLiteral("{\"schema\":1,\"families\":{\"read\":{\"calls\":1,\"errors\":0,\"measuredCalls\":0,"
                       "\"latencyMsTotal\":0,\"latencyMsMax\":0,\"latencySamples\":0,\"inputTokens\":0,"
                       "\"resultTokens\":0,\"baselineTokens\":0,\"savedTokens\":0,\"lastCallMs\":0,"
                       "\"callsWhileOff\":\"x\"}}}"),
        QStringLiteral("{\"schema\":1,\"families\":{},\"activity\":{\"calls\":[1],\"saved\":[1]}}"),
        QStringLiteral("not json"),
    };
    QJsonArray cases;
    int index = 0;
    for (const QString &input : inputs) {
        const QString path = QDir(root).filePath(QStringLiteral("load-%1.json").arg(++index));
        QFile file(path);
        file.open(QIODevice::WriteOnly);
        file.write(input.toUtf8());
        file.close();
        AgentToolEfficiencyLedger ledger(path);
        QString error;
        const bool ok = ledger.load(&error);
        cases.append(QJsonObject{{QStringLiteral("input"), input},
                                 {QStringLiteral("ok"), ok},
                                 {QStringLiteral("error"), error},
                                 {QStringLiteral("output"),
                                  QString::fromUtf8(QJsonDocument(ledger.toJson()).toJson(QJsonDocument::Compact))}});
    }
    out.insert(QStringLiteral("ledger"), cases);

    const QString logPath = QDir(root).filePath(QStringLiteral("load.jsonl"));
    const QStringList lines = {
        QStringLiteral("{\"schema\":1,\"ms\":1000,\"kind\":\"tool-error\",\"family\":\"bash\",\"tool\":\"Bash\","
                       "\"signature\":\"s\",\"session\":3,\"tokens\":5,\"detail\":\"old\",\"suggestion\":\"x\",\"topic\":\"t\"}"),
        QStringLiteral("garbage"),
        QStringLiteral("{\"schema\":2,\"ms\":900000000,\"kind\":\"tool-error\"}"),
        QStringLiteral("{\"schema\":1,\"ms\":\"900000000\",\"kind\":\"tool-error\"}"),
        QStringLiteral("{\"schema\":1,\"ms\":900000000,\"kind\":\"made-up\"}"),
        QStringLiteral("   {\"schema\":1,\"ms\":900000000,\"kind\":\"slow-call\",\"family\":\"bash\",\"tool\":\"Bash\","
                       "\"signature\":\"make\",\"session\":1.5,\"tokens\":-3,\"detail\":\"a\",\"suggestion\":\"b\",\"topic\":\"make\"}   "),
        QStringLiteral("{\"schema\":1,\"ms\":900000500,\"kind\":\"slow-call\",\"family\":\"bash\",\"tool\":\"Bash\","
                       "\"signature\":\"make\",\"tokens\":7,\"detail\":\"newer\",\"suggestion\":\"c\",\"topic\":\"make\"}"),
        QStringLiteral("{\"schema\":1,\"ms\":900000100,\"kind\":\"slow-call\",\"family\":\"bash\",\"tool\":\"Bash\","
                       "\"signature\":\"make\",\"tokens\":1,\"detail\":\"middle\",\"suggestion\":\"d\",\"topic\":\"make\"}"),
        QStringLiteral(""),
        QStringLiteral("{\"schema\":1,\"ms\":899000000,\"kind\":\"repeated-call\",\"family\":\"read\",\"tool\":\"Read\","
                       "\"tokens\":12.9}"),
    };
    QFile log(logPath);
    log.open(QIODevice::WriteOnly);
    log.write(lines.join(QLatin1Char('\n')).toUtf8());
    log.close();
    AgentToolImprovementLog loaded(logPath);
    loaded.load();
    QJsonArray rows;
    for (const AgentToolImprovement &item : loaded.improvements()) {
        QJsonObject row = insightctl::improvementRow(item);
        row.insert(QStringLiteral("firstMs"), double(item.firstMs));
        rows.append(row);
    }
    out.insert(QStringLiteral("logLines"), QJsonArray::fromStringList(lines));
    out.insert(QStringLiteral("logRows"), rows);
    out.insert(QStringLiteral("logOccurrences"), double(loaded.occurrenceCount()));
    return out;
}

QJsonObject guardrailScenario(const QString &root)
{
    QJsonObject out;
    const qint64 t = kBase;
    QJsonArray ops;
    auto blocked = [&](const QString &repo, const QString &tool, const QString &path, bool outside, qint64 at) {
        ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("blocked")},
                               {QStringLiteral("repository"), repo},
                               {QStringLiteral("tool"), tool},
                               {QStringLiteral("path"), path},
                               {QStringLiteral("outside"), outside},
                               {QStringLiteral("now"), double(at)}});
    };
    blocked(QStringLiteral("alice/demo"), QStringLiteral("Read"), QStringLiteral("/etc/passwd"), true, t);
    blocked(QStringLiteral("alice/demo"), QStringLiteral("  "), QStringLiteral(" src/x "), false, t + 2 * 3600 * 1000);
    for (int i = 0; i < 10; ++i)
        blocked(QStringLiteral("carol/x"), QStringLiteral("Bash"), QStringLiteral("/p/%1").arg(i % 9), false,
                t + 3 * 3600 * 1000 + i);
    blocked(QStringLiteral(""), QStringLiteral("Read"), QStringLiteral("/x"), false, t);
    blocked(QStringLiteral("Bob/zed"), QStringLiteral("Write"), fill(QLatin1Char('p'), 400), true,
            t + 30 * 3600 * 1000);
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("decision")},
                           {QStringLiteral("repository"), QStringLiteral("alice/demo")},
                           {QStringLiteral("approved"), true},
                           {QStringLiteral("now"), double(t + 5)}});
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("decision")},
                           {QStringLiteral("repository"), QStringLiteral("dave/new")},
                           {QStringLiteral("approved"), false},
                           {QStringLiteral("now"), double(t + 31 * 3600 * 1000)}});
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("stopped")},
                           {QStringLiteral("repository"), QStringLiteral("alice/demo")},
                           {QStringLiteral("count"), 2},
                           {QStringLiteral("now"), double(t + 7)}});
    ops.append(QJsonObject{{QStringLiteral("op"), QStringLiteral("stopped")},
                           {QStringLiteral("repository"), QStringLiteral("alice/demo")},
                           {QStringLiteral("count"), 0},
                           {QStringLiteral("now"), double(t + 8)}});
    out.insert(QStringLiteral("ops"), ops);
    AgentGuardrailLedger ledger(QDir(root).filePath(QStringLiteral("guardrails.json")));
    for (const QJsonValue &value : ops) {
        const QJsonObject o = value.toObject();
        const QString kind = o.value(QStringLiteral("op")).toString();
        const qint64 now = qint64(o.value(QStringLiteral("now")).toDouble());
        const QString repo = o.value(QStringLiteral("repository")).toString();
        if (kind == QLatin1String("blocked"))
            ledger.recordBlocked(repo, o.value(QStringLiteral("tool")).toString(),
                                 expand(o.value(QStringLiteral("path")).toString()),
                                 o.value(QStringLiteral("outside")).toBool(), now);
        else if (kind == QLatin1String("decision"))
            ledger.recordDecision(repo, o.value(QStringLiteral("approved")).toBool(), now);
        else
            ledger.recordAgentsStopped(repo, o.value(QStringLiteral("count")).toInt(), now);
    }
    const qint64 end = t + 32 * 3600 * 1000;
    out.insert(QStringLiteral("end"), double(end));
    out.insert(QStringLiteral("json"),
               QString::fromUtf8(QJsonDocument(ledger.toJson()).toJson(QJsonDocument::Compact)));
    out.insert(QStringLiteral("repositories"), QJsonArray::fromStringList(ledger.repositories()));
    QJsonArray rows;
    for (const QString &repo : ledger.repositories())
        rows.append(insightctl::guardrailRow(ledger.stats(repo), 5, 5));
    out.insert(QStringLiteral("rows"), rows);
    out.insert(QStringLiteral("totals"), insightctl::guardrailTotalsRow(ledger.totals(end)));
    QJsonArray recent;
    for (qint64 v : ledger.totals(end).recentBlocked)
        recent.append(double(v));
    out.insert(QStringLiteral("recentBlocked"), recent);
    const QString input = QStringLiteral(
        "{\"version\":1,\"repositories\":{\"\":{\"blocked\":3},\"x/y\":{\"blocked\":4,\"blockedOutside\":9,"
        "\"approved\":1.5,\"denied\":-2,\"agentsStopped\":\"3\",\"lastEventMs\":77,\"tools\":{\"Read\":2,"
        "\"Frac\":1.5,\"Neg\":-1},\"recentPaths\":[\"a\",2,\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]}},"
        "\"activity\":[1,2,3],\"activityBucketStartMs\":3600000}");
    AgentGuardrailLedger loaded;
    loaded.fromJson(QJsonDocument::fromJson(input.toUtf8()).object());
    out.insert(QStringLiteral("loadInput"), input);
    out.insert(QStringLiteral("loadOutput"),
               QString::fromUtf8(QJsonDocument(loaded.toJson()).toJson(QJsonDocument::Compact)));
    AgentGuardrailLedger newer;
    QString error;
    out.insert(QStringLiteral("newerOk"),
               newer.fromJson(QJsonObject{{QStringLiteral("version"), 2}}, &error));
    out.insert(QStringLiteral("newerError"), error);
    return out;
}

QJsonArray shapes()
{
    const QStringList commands = {
        QStringLiteral("git status"),
        QStringLiteral("cd /repo && git -C x log --oneline"),
        QStringLiteral("timeout 60 go test ./..."),
        QStringLiteral("timeout -k 5 60 npm test"),
        QStringLiteral("nice -n 10 make -j8"),
        QStringLiteral("nice -n5 ninja -C build"),
        QStringLiteral("env FOO=1 BAR=2 python3 script.py --x"),
        QStringLiteral("FOO=1 python -m pytest -q"),
        QStringLiteral("python3 -m http.server"),
        QStringLiteral("python3 -c 'print(1)'"),
        QStringLiteral("python3"),
        QStringLiteral("node --eval \"x\""),
        QStringLiteral("node scripts/build.js"),
        QStringLiteral("/usr/bin/git diff"),
        QStringLiteral("./gradlew test"),
        QStringLiteral("for f in *.txt; do echo $f; done"),
        QStringLiteral("while read l; do echo \"$l\"; done < file"),
        QStringLiteral("cat <<'EOF' > a.txt\nline; with; semis\nEOF\nwc -l a.txt"),
        QStringLiteral("cat <<EOF\nx\nEOFX\nEOF"),
        QStringLiteral("bash <<-END\n\techo hi\n\tEND\nls"),
        QStringLiteral("cat << \"QUOTED\"\nbody\nQUOTED"),
        QStringLiteral("cat <<'NOEND'\nnever closed"),
        QStringLiteral("echo hi"),
        QStringLiteral("cd /x"),
        QStringLiteral("(cd sub && make)"),
        QStringLiteral("{ echo a; echo b; } | sort"),
        QStringLiteral("set -e; export A=1; cargo build --release"),
        QStringLiteral("sudo systemctl restart forkmesh"),
        QStringLiteral("exec docker compose up"),
        QStringLiteral("npx playwright test"),
        QStringLiteral("gh pr view 12"),
        QStringLiteral("git"),
        QStringLiteral("git -C repo status"),
        QStringLiteral("git ./x"),
        QStringLiteral("a && b || c ; d"),
        QStringLiteral("x=$(ls; pwd) && echo $x"),
        QStringLiteral("echo \"a;b\" ; rg foo"),
        QStringLiteral("if true; then echo x; fi"),
        QStringLiteral("time ./run.sh"),
        QStringLiteral("command -v jq"),
        QStringLiteral(""),
        QStringLiteral("   "),
        QStringLiteral("perl -e 'print 1'"),
        QStringLiteral("dart run build_runner"),
        QStringLiteral("zsh ./x.sh"),
        QStringLiteral("cd a; cd b; ls -la"),
        QStringLiteral("mkdir -p out && cp a b && wrangler deploy"),
        QStringLiteral("echo \\; ls"),
        QStringLiteral("python3.11 -m unittest discover"),
        QStringLiteral("ruby -"),
        QStringLiteral("/bin/bash -lc 'git status --short'"),
        QStringLiteral("bash -lc \"echo \\\"a\\\"; rg x\""),
        QStringLiteral("sh -c 'a' && b 'c'"),
        QStringLiteral("bash -lc 'it'\\''s; ls'"),
        QStringLiteral("env -u FORKMESH_HOME timeout -k 5 100 python3 tools/x.py"),
        QStringLiteral("sudo -u www ls"),
    };
    QJsonArray out;
    for (const QString &command : commands)
        out.append(QJsonObject{{QStringLiteral("command"), command},
                               {QStringLiteral("shape"), agentShellCommandShape(command)}});
    return out;
}

QJsonArray familiesFor()
{
    const QList<QPair<QString, QJsonObject>> names = {
        {QStringLiteral("Read"), {}},
        {QStringLiteral("  bash "), {}},
        {QStringLiteral("mcp__github__list_prs"), {}},
        {QStringLiteral("mcp__playwright__browser_navigate"), {}},
        {QStringLiteral("mcp__memory__search_memory"), {}},
        {QStringLiteral("mcp__x"), {}},
        {QStringLiteral("mcp__"), {}},
        {QStringLiteral("srv::kernel_run"), {}},
        {QStringLiteral("ToolSearch"), {}},
        {QStringLiteral("dynamicToolCall"), {}},
        {QStringLiteral("whatever"), {{QStringLiteral("namespace"), QStringLiteral("semgrep")}}},
        {QStringLiteral("whatever"), {}},
        {QStringLiteral(""), {}},
        {QStringLiteral("mcp__fetcher__get"), {}},
        {QStringLiteral("mcp__srv__skill_list"), {}},
        {QStringLiteral("mcp__a__b__c"), {}},
    };
    QJsonArray out;
    for (const auto &entry : names)
        out.append(QJsonObject{{QStringLiteral("name"), entry.first},
                               {QStringLiteral("input"), entry.second},
                               {QStringLiteral("family"), agentToolFamilyFor(entry.first, entry.second)}});
    return out;
}

QJsonObject policy()
{
    QJsonObject out;
    QJsonObject notes;
    for (const QString &id : agentToolFamilyIds())
        notes.insert(id, AgentToolPolicy::enforcementNote(id));
    out.insert(QStringLiteral("notes"), notes);
    const QStringList raw = {QStringLiteral(" Bash "), QStringLiteral("read"), QStringLiteral("bogus"),
                             QStringLiteral("bash"), QStringLiteral("WEB_SEARCH"), QString()};
    out.insert(QStringLiteral("raw"), QJsonArray::fromStringList(raw));
    out.insert(QStringLiteral("normalized"),
               QJsonArray::fromStringList(AgentToolPolicy::normalizeDisabledFamilies(raw)));
    out.insert(QStringLiteral("claude"),
               QJsonArray::fromStringList(AgentToolPolicy::claudeDisallowedTools(raw)));
    QJsonArray catalog;
    for (const AgentToolFamily &family : agentToolFamilies())
        catalog.append(QJsonObject{{QStringLiteral("id"), family.id},
                                   {QStringLiteral("name"), family.displayName},
                                   {QStringLiteral("category"), family.category},
                                   {QStringLiteral("summary"), family.summary},
                                   {QStringLiteral("availability"), family.availability},
                                   {QStringLiteral("note"), family.availabilityNote},
                                   {QStringLiteral("baseline"), family.baseline},
                                   {QStringLiteral("expected"), family.expectedSavings},
                                   {QStringLiteral("hidden"), family.hidden}});
    out.insert(QStringLiteral("catalog"), catalog);
    return out;
}

QString cleanRelativeDirectory(const QString &raw)
{
    QString path = QDir::fromNativeSeparators(raw.trimmed());
    if (path.isEmpty() || path.contains(QChar::Null) || QDir::isAbsolutePath(path))
        return {};
    path = QDir::cleanPath(path);
    if (path == QLatin1String("..") || path.startsWith(QLatin1String("../")))
        return {};
    while (path.startsWith(QLatin1String("./")))
        path.remove(0, 2);
    return path.isEmpty() ? QStringLiteral(".") : path;
}

QStringList normalizeDirectories(const QStringList &entries)
{
    QStringList normalized;
    for (const QString &entry : entries) {
        const QString path = cleanRelativeDirectory(entry);
        if (path.isEmpty())
            continue;
        if (!normalized.contains(path))
            normalized.append(path);
        if (normalized.size() == 64)
            break;
    }
    std::sort(normalized.begin(), normalized.end(), [](const QString &a, const QString &b) {
        if (a == b)
            return false;
        if (a == QLatin1String("."))
            return true;
        if (b == QLatin1String("."))
            return false;
        return a < b;
    });
    if (normalized.contains(QStringLiteral(".")))
        return {QStringLiteral(".")};
    QStringList compact;
    for (const QString &path : std::as_const(normalized)) {
        bool covered = false;
        for (const QString &parent : std::as_const(compact)) {
            if (path.startsWith(parent + QLatin1Char('/'))) {
                covered = true;
                break;
            }
        }
        if (!covered)
            compact.append(path);
    }
    return compact;
}

QJsonArray directories()
{
    const QList<QStringList> cases = {
        {QStringLiteral("src"), QStringLiteral("src/lib"), QStringLiteral("./docs/"), QStringLiteral("a/../b")},
        {QStringLiteral("x"), QStringLiteral("."), QStringLiteral("y")},
        {QStringLiteral("/abs"), QStringLiteral("../up"), QStringLiteral(".."), QStringLiteral(" "),
         QStringLiteral("z/./w"), QStringLiteral("z")},
        {QStringLiteral("./"), QStringLiteral("b")},
        {QStringLiteral("srcx"), QStringLiteral("src"), QStringLiteral("src/a")},
    };
    QJsonArray out;
    for (const QStringList &input : cases)
        out.append(QJsonObject{{QStringLiteral("input"), QJsonArray::fromStringList(input)},
                               {QStringLiteral("output"), QJsonArray::fromStringList(normalizeDirectories(input))}});
    return out;
}

QJsonObject helpers()
{
    QJsonObject out;
    QJsonArray subjects;
    const QList<QJsonObject> params = {
        {{QStringLiteral("action"), QStringLiteral("toggleFamily")}, {QStringLiteral("family"), QStringLiteral(" bash ")},
         {QStringLiteral("enabled"), false}},
        {{QStringLiteral("action"), QStringLiteral("toggleFamily")}, {QStringLiteral("family"), QStringLiteral("Bash")},
         {QStringLiteral("enabled"), true}},
        {{QStringLiteral("action"), QStringLiteral("toggleFamily")}, {QStringLiteral("family"), QStringLiteral("bash")},
         {QStringLiteral("enabled"), 1}},
        {{QStringLiteral("action"), QStringLiteral("runShortcut")}, {QStringLiteral("repo"), QStringLiteral(" alice/demo ")},
         {QStringLiteral("scope"), QStringLiteral("shortcut")}, {QStringLiteral("file"), QStringLiteral("run tests.sh")}},
        {{QStringLiteral("action"), QStringLiteral("runShortcut")}, {QStringLiteral("repo"), QStringLiteral("alice/demo")},
         {QStringLiteral("scope"), QStringLiteral("bogus")}, {QStringLiteral("file"), QStringLiteral("a.sh")}},
        {{QStringLiteral("action"), QStringLiteral("runShortcut")}, {QStringLiteral("repo"), QStringLiteral("alice/../x")},
         {QStringLiteral("scope"), QStringLiteral("app")}, {QStringLiteral("file"), QStringLiteral("a.md")}},
        {{QStringLiteral("action"), QStringLiteral("runShortcut")}, {QStringLiteral("repo"), QStringLiteral("alice/demo")},
         {QStringLiteral("scope"), QStringLiteral("app")}, {QStringLiteral("file"), QStringLiteral("../a.md")}},
        {{QStringLiteral("action"), QStringLiteral("explode")}},
    };
    for (const QJsonObject &p : params) {
        QString error;
        const QString action = p.value(QStringLiteral("action")).toString();
        const QString subject = insightctl::subjectFor(action, p, &error);
        subjects.append(QJsonObject{{QStringLiteral("params"), p},
                                    {QStringLiteral("subject"), subject},
                                    {QStringLiteral("error"), subject.isEmpty() ? error : QString()},
                                    {QStringLiteral("summary"), insightctl::actionSummary(action, subject, QString())},
                                    {QStringLiteral("summaryLabel"),
                                     insightctl::actionSummary(action, subject, QStringLiteral("Label"), true)}});
    }
    out.insert(QStringLiteral("subjects"), subjects);
    QList<insightctl::PeerTraffic> traffic = {
        {7, QStringLiteral("140.82.112.3"), 443, 100, 2000},
        {7, QStringLiteral("140.82.112.3"), 22, 5, 5},
        {7, QStringLiteral("127.0.0.1"), 8080, 999, 999},
        {7, QStringLiteral("::1"), 9000, 1, 1},
        {7, QStringLiteral("2606:4700::6810:84e5"), 443, 50, 50},
        {3, QStringLiteral("10.0.0.2"), 0, 1, 2},
        {3, QStringLiteral("10.0.0.2"), 5432, 3, 4},
        {0, QStringLiteral("8.8.8.8"), 53, 1, 1},
        {3, QString(), 1, 1, 1},
    };
    QJsonArray trafficJson;
    for (const auto &item : traffic)
        trafficJson.append(QJsonObject{{QStringLiteral("session"), item.session},
                                       {QStringLiteral("peer"), item.peer},
                                       {QStringLiteral("port"), int(item.port)},
                                       {QStringLiteral("sent"), double(item.sent)},
                                       {QStringLiteral("received"), double(item.received)}});
    QHash<int, QString> labels{{7, QStringLiteral("Fix the build")}, {3, QString(200, QLatin1Char('L'))}};
    out.insert(QStringLiteral("traffic"), trafficJson);
    out.insert(QStringLiteral("network"), insightctl::networkRows(traffic, labels, {}, 2));
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    qputenv("TZ", "UTC");
    tzset();
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: golden <out.json>\n");
        return 2;
    }
    QTemporaryDir root;
    QJsonObject out;
    out.insert(QStringLiteral("ledger"), ledgerScenario(root.path()));
    out.insert(QStringLiteral("loads"), ledgerLoads(root.path()));
    out.insert(QStringLiteral("guardrails"), guardrailScenario(root.path()));
    out.insert(QStringLiteral("shapes"), shapes());
    out.insert(QStringLiteral("familyFor"), familiesFor());
    out.insert(QStringLiteral("policy"), policy());
    out.insert(QStringLiteral("directories"), directories());
    out.insert(QStringLiteral("helpers"), helpers());
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::WriteOnly))
        return 1;
    file.write(QJsonDocument(out).toJson(QJsonDocument::Compact));
    return 0;
}
