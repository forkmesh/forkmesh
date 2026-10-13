#include "AgentLogText.h"
#include "AgentStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {

QString redactText(QString text, const QStringList &credentialValues)
{
    for (const QString &secret : credentialValues) {
        if (secret.size() >= 8 && text.contains(secret))
            text.replace(secret, QStringLiteral("***"));
    }
    static const QRegularExpression bearerHeader(
        QStringLiteral("(?i)(authorization\\s*:\\s*bearer\\s+)[A-Za-z0-9._~+/-]{8,}"));
    static const QRegularExpression credentialAssignment(
        QStringLiteral("(?i)([\"']?(?:accessToken|refreshToken|apiKey|"
                       "anthropicApiKey|openAiApiKey|authorization)[\"']?"
                       "\\s*[:=]\\s*[\"'])[^\"'\\r\\n]+"));
    static const QRegularExpression skToken(QStringLiteral("\\bsk-[A-Za-z0-9_-]{20,}"));
    if (text.contains(QStringLiteral("authorization"), Qt::CaseInsensitive))
        text.replace(bearerHeader, QStringLiteral("\\1***"));
    bool mayContainAssignment = false;
    for (const QString &key :
         {QStringLiteral("accessToken"), QStringLiteral("refreshToken"), QStringLiteral("apiKey"),
          QStringLiteral("anthropicApiKey"), QStringLiteral("openAiApiKey"),
          QStringLiteral("authorization")}) {
        if (text.contains(key, Qt::CaseInsensitive)) {
            mayContainAssignment = true;
            break;
        }
    }
    if (mayContainAssignment)
        text.replace(credentialAssignment, QStringLiteral("\\1***"));
    if (text.contains(QStringLiteral("sk-")))
        text.replace(skToken, QStringLiteral("sk-***"));
    return text;
}

QJsonValue redactValue(const QJsonValue &value, const QStringList &credentialValues)
{
    if (value.isString())
        return redactText(value.toString(), credentialValues);
    if (value.isArray()) {
        QJsonArray result;
        for (const QJsonValue &entry : value.toArray())
            result.append(redactValue(entry, credentialValues));
        return result;
    }
    if (!value.isObject())
        return value;
    QJsonObject result;
    const QJsonObject object = value.toObject();
    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        const QString key = it.key().toLower();
        const bool sensitive = key == QLatin1String("accesstoken") ||
                               key == QLatin1String("refreshtoken") ||
                               key == QLatin1String("apikey") ||
                               key == QLatin1String("anthropicapikey") ||
                               key == QLatin1String("openaiapikey") ||
                               key == QLatin1String("authorization") ||
                               key == QLatin1String("credentials");
        result.insert(it.key(), sensitive ? QJsonValue(QStringLiteral("***"))
                                          : redactValue(it.value(), credentialValues));
    }
    return result;
}

QString b64(const QByteArray &bytes)
{
    return QString::fromLatin1(bytes.toBase64());
}

QByteArray compact(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QJsonArray numbers()
{
    const QList<double> doubles = {
        0.0, -0.0, 1.0, -1.0, 1.5, 0.1, 0.2, 0.30000000000000004, 3.14159, 100.0, 1000.0,
        10000.0, 100000.0, 123456.0, 1000000.0, 1500000.0, 12345678.0, 123456789.0,
        1234567890123.0, 1759500000000.0, 1759512345678.0, 1700000000000.0, 1e15, 1e16, 1e17,
        1e20, 1e21, 1e22, 1.5e300, 1e100, 123e18, 9007199254740993.0, -1234.5, 0.001, 0.0001,
        0.00001, 0.000123, 1e-7, 1.23e-5, 1e-10, 4.9e-324, 1.7976931348623157e308, 12345.678,
        0.5, 2.5, 99.99, 1e5 + 0.5, 4294967296.0, 2147483647.0, 2147483648.0, -2147483649.0,
        0.012345, 0.0123456789, 1.0 / 3.0, 2.0 / 3.0, 1e-5, 5e-5, 1e-4, 1e-3, 10.0, 20.0,
        1e3, 1e4, 3e5, 25e5, 1e9, 1e10, 1e11, 1e12, 1e13, 1e14, 512.0, 4096.0, 65536.0,
        1048576.0, 8388608.0, 0.000001, 7.25e-6, 6.02214076e23, 1.602e-19};
    QJsonArray out;
    for (double d : doubles) {
        char repr[64];
        std::snprintf(repr, sizeof repr, "%.17g", d);
        out.append(QJsonObject{{QStringLiteral("in"), QString::fromLatin1(repr)},
                               {QStringLiteral("out"),
                                QString::fromUtf8(compact(QJsonObject{{QStringLiteral("v"), d}}))}});
    }
    const char *mantissas[] = {"1", "12", "123", "12345", "12345678", "123456789012",
                               "1234567890123", "123456789012345", "12345678901234567"};
    for (int e = -14; e <= 26; ++e) {
        for (const char *m : mantissas) {
            const int len = int(std::strlen(m));
            for (int shift = 0; shift <= len; shift += (len > 3 ? len / 3 : 1)) {
                char text[96];
                std::snprintf(text, sizeof text, "%.*s.%se%d", shift, m, m + shift, e);
                const double d = std::strtod(text, nullptr);
                char repr[64];
                std::snprintf(repr, sizeof repr, "%.17g", d);
                out.append(QJsonObject{
                    {QStringLiteral("in"), QString::fromLatin1(repr)},
                    {QStringLiteral("out"),
                     QString::fromUtf8(compact(QJsonObject{{QStringLiteral("v"), d}}))}});
                out.append(QJsonObject{
                    {QStringLiteral("in"), QString::fromLatin1(repr)},
                    {QStringLiteral("neg"), true},
                    {QStringLiteral("out"),
                     QString::fromUtf8(compact(QJsonObject{{QStringLiteral("v"), -d}}))}});
            }
        }
    }
    for (double d : {9007199254740994.0, 9007199254740996.0, 18014398509481984.0, 1.2345678901200000e16,
                     1.234567890123e16, 99999999999999990.0, 0.00012, 0.000012, 0.0012, 8e15, 9e15, 9.1e15,
                     9007199254740992.0, 9.2e15, 1e300, 123456789012345e-20}) {
        char repr[64];
        std::snprintf(repr, sizeof repr, "%.17g", d);
        out.append(QJsonObject{{QStringLiteral("in"), QString::fromLatin1(repr)},
                               {QStringLiteral("out"),
                                QString::fromUtf8(compact(QJsonObject{{QStringLiteral("v"), d}}))}});
    }
    const QList<qint64> ints = {0, 1, -1, 42, 100000, 1000000, 2147483647LL, 2147483648LL,
                                -2147483649LL, 9007199254740993LL, 1759500000000LL,
                                std::numeric_limits<qint64>::max(),
                                std::numeric_limits<qint64>::min()};
    for (qint64 i : ints) {
        out.append(QJsonObject{{QStringLiteral("in"), QString::number(i)},
                               {QStringLiteral("int"), true},
                               {QStringLiteral("out"),
                                QString::fromUtf8(compact(QJsonObject{{QStringLiteral("v"), i}}))}});
    }
    return out;
}

QJsonArray parses()
{
    const QList<QByteArray> lines = {
        "{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"abc\",\"tools\":[\"Bash\",\"Edit\"]}",
        "{\"b\":1,\"a\":2,\"c\":{\"z\":1,\"y\":[3,2,1]}}",
        "{\"n\":1.0,\"m\":1e5,\"k\":1E+21,\"j\":-0,\"i\":0.000001,\"h\":1e-7,\"g\":123456789012345678901234,\"f\":1.5e3}",
        "{\"s\":\"slash / back \\\\ quote \\\" tab \\t nl \\n cr \\r bs \\b ff \\f\"}",
        "{\"u\":\"\\u0000\\u0001\\u001f\\u007f\\u0080\\u00e9\\u2028\\u2029\\ud83d\\ude00\"}",
        "{\"utf8\":\"caf\xc3\xa9 \xe2\x9c\x93 \xf0\x9f\x98\x80\"}",
        "{\"dup\":1,\"dup\":2}",
        "{\"empty\":{},\"arr\":[],\"nested\":[{},[],[[]]]}",
        "  {\"ws\" : true , \"f\" : false , \"n\" : null }  ",
        "\xef\xbb\xbf{\"bom\":1}",
        "{\"bad\":\"\xff\xfe\"}",
        "[1,2,3]",
        "\"just a string\"",
        "{\"trailing\":1} x",
        "{\"unterminated\":",
        "{\"lone\":\"\\ud800\"}",
        "{\"big\":18446744073709551615,\"neg\":-9223372036854775808,\"over\":-9223372036854775809}",
        "{\"frac\":2.50,\"exp\":2E2,\"e0\":5e0,\"tiny\":5e-324,\"huge\":1e308}",
        "{\"ctl\":\"a\tb\x01c\",\"esc\":\"\\u001b\\u001B\\u00ff\\/\"}",
        "{\"nonchar\":\"\xef\xbf\xbe\"}",
        "{\"surrogate\":\"\xed\xa0\x80\"}",
        "{\"overlong\":\"\xc0\x80\"}",
        "{\"toobig\":\"\xf4\x90\x80\x80\"}",
        "{\"pair\":\"\\ud800\\u0041\\udc00x\\ud83d\"}",
        "{\"keys\":{\"\xef\xbd\x81\":1,\"\xf0\x9f\x98\x80\":2,\"Z\":3,\"a\":4,\"\":5,\"\xc3\xa9\":6}}",
        "{\"badesc\":\"\\x\"}",
        "{\"num\":01}",
        "{\"num\":-}",
        "{\"num\":1.}",
        "{\"num\":.5}",
        "{\"num\":1e}",
        "{\"num\":+1}",
        "{\"lit\":tru}",
        "{\"a\":1,}",
        "{\"a\" 1}",
        "{\"a\":[1,]}",
        "{\"a\":9.1e15,\"b\":1e16,\"c\":1.5,\"d\":-0.0,\"e\":9223372036854775807,\"f\":9223372036854775808}",
        "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"duration_ms\":12345,"
        "\"num_turns\":3,\"total_cost_usd\":0.0123456,\"usage\":{\"input_tokens\":100,"
        "\"output_tokens\":50}}",
        "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"index\":0,"
        "\"delta\":{\"type\":\"text_delta\",\"text\":\"Hello <b>&amp;\"}}}",
    };
    QJsonArray out;
    for (const QByteArray &line : lines) {
        QJsonParseError error;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &error);
        QJsonObject entry{{QStringLiteral("in"), b64(line)}, {QStringLiteral("ok"), doc.isObject()}};
        if (doc.isObject()) {
            entry.insert(QStringLiteral("out"), b64(compact(doc.object())));
            entry.insert(QStringLiteral("indented"),
                         b64(QJsonDocument(doc.object()).toJson(QJsonDocument::Indented)));
        }
        out.append(entry);
    }
    return out;
}

QList<AgentSession> sampleSessions()
{
    QList<AgentSession> sessions;
    AgentSession plain;
    plain.id = 7;
    plain.owner = QStringLiteral("alice");
    plain.name = QStringLiteral("demo");
    plain.provider = QStringLiteral("claude-code");
    plain.prompt = QStringLiteral("Fix the bug \"now\"\nplease \xc3\xa9");
    plain.createdAtMs = 1759500000000;
    sessions.append(plain);

    AgentSession rich;
    rich.id = 12;
    rich.priority = 3;
    rich.owner = QStringLiteral("Org Name");
    rich.name = QStringLiteral("repo.with/slash");
    rich.issueNumber = 42;
    rich.issueTitle = QStringLiteral("Title \xe2\x9c\x93");
    rich.provider = QStringLiteral("codex");
    rich.model = QStringLiteral("gpt-5");
    rich.mode = QStringLiteral("Auto");
    rich.createPr = true;
    rich.directCheckout = true;
    rich.held = true;
    rich.heldBy = QStringLiteral("phone");
    rich.vibe = true;
    rich.genie = true;
    rich.strength = QStringLiteral("high");
    rich.orgTask = true;
    rich.orgTaskId = QStringLiteral("t-1");
    rich.orgTaskAttached = true;
    rich.teammateIds = {QStringLiteral(" a "), QStringLiteral("b"), QStringLiteral("a")};
    rich.skillsUsed = {QStringLiteral("Skill"), QStringLiteral("skill"), QStringLiteral("Other")};
    rich.taskGrade = 9;
    rich.orgAgentJob = QJsonObject{{QStringLiteral("sessionId"), QStringLiteral("web-9")},
                                   {QStringLiteral("lease"), 1.5},
                                   {QStringLiteral("nested"), QJsonObject{}}};
    rich.status = AgentStatus::Running;
    rich.branchName = QStringLiteral("forkmesh/x");
    rich.merged = true;
    rich.mergedAtMs = 1759500000123;
    rich.costUsd = 1.2345;
    rich.spendBeforeUsd = 100000.0;
    rich.spendAfterUsd = 0.1;
    rich.totalTokens = 123456;
    rich.durationMs = 100000;
    rich.runningMs = 1000;
    rich.runningSinceMs = 1759500000000;
    rich.lastError = QStringLiteral("boom\ttab");
    rich.chatConversation = QStringLiteral("  conv  ");
    sessions.append(rich);

    AgentSession future;
    future.schemaVersion = 3;
    future.id = 99;
    future.owner = QStringLiteral("x");
    future.name = QStringLiteral("y");
    sessions.append(future);
    return sessions;
}

QJsonArray sessionsSection()
{
    QJsonArray out;
    for (const AgentSession &session : sampleSessions()) {
        const QJsonObject json = session.toJson();
        out.append(QJsonObject{
            {QStringLiteral("compact"), b64(compact(json))},
            {QStringLiteral("indented"), b64(QJsonDocument(json).toJson(QJsonDocument::Indented))},
            {QStringLiteral("repoKey"), session.repoKey()},
        });
    }
    const QList<QByteArray> raws = {
        "{\"id\":3,\"owner\":\"o\",\"name\":\"n\",\"provider\":\"claude-code\",\"extra\":1}",
        "{\"id\":4,\"schemaVersion\":1,\"priority\":0,\"taskGrade\":-3,\"held\":false,\"heldBy\":\"z\","
        "\"orgAgentJob\":{\"sessionId\":\" web \"},\"mergedAtMs\":\"17\",\"createdAtMs\":1.5e12}",
        "{\"id\":5,\"teammateIds\":[\"\",\" x \",\"x\",3],\"skillsUsed\":[\"A\",\"a\",\" b \"]}",
    };
    for (const QByteArray &raw : raws) {
        const AgentSession session =
            AgentSession::fromJson(QJsonDocument::fromJson(raw).object());
        out.append(QJsonObject{
            {QStringLiteral("raw"), b64(raw)},
            {QStringLiteral("compact"), b64(compact(session.toJson()))},
            {QStringLiteral("indented"),
             b64(QJsonDocument(session.toJson()).toJson(QJsonDocument::Indented))},
        });
    }
    return out;
}

QJsonArray logTextSection()
{
    const QList<QByteArray> events = {
        "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"  Hello there  \"},"
        "{\"type\":\"tool_use\",\"name\":\"Bash\",\"input\":{\"command\":\"ls -la\"}},"
        "{\"type\":\"tool_use\",\"name\":\"Edit\",\"input\":{\"file_path\":\"/a/b.cpp\"}},"
        "{\"type\":\"tool_use\",\"name\":\"Grep\",\"input\":{\"pattern\":\"foo\"}},"
        "{\"type\":\"tool_use\",\"name\":\"Glob\",\"input\":{\"path\":\"src\"}},"
        "{\"type\":\"tool_use\",\"name\":\"Todo\",\"input\":{\"todos\":[{\"a\":1}],\"z\":\"\xc3\xa9\"}},"
        "{\"type\":\"tool_use\",\"name\":\"Empty\",\"input\":{}},"
        "{\"type\":\"output_text\",\"text\":\"done\"},{\"type\":\"thinking\",\"thinking\":\"x\"}]}}",
        "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"name\":\"shell\","
        "\"input\":{\"command\":[\"bash\",\"-lc\",\"echo hi\"]}},{\"type\":\"tool_use\",\"name\":\"shell\","
        "\"input\":{\"command\":[\"git\",\"status\"]}}]}}",
        "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"name\":\"Bash\","
        "\"input\":{\"command\":\"" + QByteArray(250, 'x') + "\"}}]}}",
        "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"tool_use\",\"name\":\"Bash\","
        "\"input\":{\"command\":\"" + QByteArray(199, 'y') + "\xf0\x9f\x98\x80" + "zz\"}}]}}",
        "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"content\":\"ok\"}]}}",
        "{\"type\":\"user\",\"message\":{\"content\":\"<task-notification><status>completed</status>"
        "<summary>Build  done</summary><result>line one\\n\\nline &lt;two&gt;</result></task-notification>\"}}",
        "{\"type\":\"user\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"<task-notification>"
        "<status>Cost</status><summary></summary><event>evt</event></task-notification>\"}]}}",
        "{\"type\":\"user\",\"isSidechain\":true,\"message\":{\"content\":\"<task-notification>x"
        "</task-notification>\"}}",
        "{\"type\":\"user\",\"parent_tool_use_id\":\"t1\",\"message\":{\"content\":\"<task-notification>x"
        "</task-notification>\"}}",
        "{\"type\":\"user\",\"message\":{\"content\":\"<task-notification><status>failed</status>"
        "<summary>Long</summary><result>" + QByteArray(1600, 'r') + "</result></task-notification>\"}}",
        "{\"type\":\"user\",\"message\":{\"content\":\"<task-notification><status>ok</status>"
        "<summary>Fence</summary><result>```\\ncode\\n" + QByteArray(1490, 'c') +
            "\\nmore</result></task-notification>\"}}",
    };
    QJsonArray out;
    for (const QByteArray &line : events) {
        const QJsonObject event = QJsonDocument::fromJson(line).object();
        out.append(QJsonObject{
            {QStringLiteral("event"), b64(line)},
            {QStringLiteral("parsed"), !event.isEmpty()},
            {QStringLiteral("assistant"),
             b64(AgentLogText::assistantContent(event.value(QStringLiteral("message")).toObject())
                     .toUtf8())},
            {QStringLiteral("user"), b64(AgentLogText::userEvent(event).toUtf8())},
        });
    }
    return out;
}

QJsonArray redactSection()
{
    const QStringList credentials = {QStringLiteral("supersecretvalue1"), QStringLiteral("short"),
                                     QStringLiteral("another-secret-22")};
    const QList<QByteArray> events = {
        "{\"type\":\"assistant\",\"message\":{\"content\":[{\"type\":\"text\",\"text\":\"key "
        "supersecretvalue1 and sk-abcdefghijklmnopqrstuvwxyz012 and short\"}]}}",
        "{\"apiKey\":\"x\",\"ApiKey\":{\"a\":1},\"credentials\":[1],\"nested\":{\"Authorization\":\"Bearer abc\"}}",
        "{\"text\":\"Authorization: Bearer abcdefghij12345 rest\",\"t2\":\"authorization:bearer short\"}",
        "{\"text\":\"\\\"accessToken\\\": \\\"tok-value\\\", 'apiKey'='v2' refreshToken=\\\"zz\\\"\"}",
        "{\"text\":\"xsk-abcdefghijklmnopqrstuvwxyz sk-short sk-ABCDEFGHIJKLMNOPQRSTUVWXYZ_-\"}",
        "{\"n\":1.5,\"b\":true,\"z\":null,\"arr\":[\"another-secret-22\",2]}",
    };
    QJsonArray out;
    for (const QByteArray &line : events) {
        const QJsonObject event = QJsonDocument::fromJson(line).object();
        out.append(QJsonObject{
            {QStringLiteral("in"), b64(line)},
            {QStringLiteral("out"), b64(compact(redactValue(QJsonValue(event), credentials).toObject()))},
        });
    }
    return QJsonArray{QJsonObject{{QStringLiteral("credentials"), QJsonArray::fromStringList(credentials)},
                                  {QStringLiteral("cases"), out}}};
}

QJsonObject streamSection()
{
    const QStringList credentials = {QStringLiteral("supersecretvalue1")};
    const QString account = QStringLiteral("acct-1");
    const QString key = QStringLiteral("agent-5-1759500000000");
    const double ts = 1759512345678.0;
    QByteArray stream;
    stream += "{\"type\":\"system\",\"subtype\":\"init\",\"session_id\":\"s-1\",\"tools\":[],\"model\":\"m\"}\n";
    stream += "\n   \t\r\n";
    stream += "not json at all\n";
    stream += "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"delta\":{\"text\":\"He\"}},\"session_id\":\"s-1\"}\r\n";
    stream += "{\"type\":\"assistant\",\"session_id\":\"s-1\",\"message\":{\"id\":\"m1\",\"content\":[{\"type\":\"text\",\"text\":\"Key supersecretvalue1 sk-abcdefghijklmnopqrstuvwxyz \u00e9 \\ud83d\\ude00\"},{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"Bash\",\"input\":{\"command\":\"ls\",\"apiKey\":\"x\"}}],\"usage\":{\"output_tokens\":7,\"input_tokens\":3}}}\n";
    stream += "{\"type\":\"user\",\"session_id\":\"s-1\",\"message\":{\"content\":[{\"type\":\"tool_result\",\"tool_use_id\":\"t1\",\"content\":\"ok 1.0 1e-7\"}]},\"n\":1.0,\"e\":1e-7}\n";
    stream += "{\"type\":\"user\",\"message\":{\"content\":\"<task-notification><status>completed</status><summary>Build</summary><result>done</result></task-notification>\"}}\n";
    stream += "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"duration_ms\":1200,\"num_turns\":2,\"total_cost_usd\":0.0123,\"session_id\":\"s-1\"}\n";
    stream += "{\"type\":\"partial\"";
    QByteArray events;
    QByteArray log;
    qint64 offset = 0;
    while (true) {
        const qsizetype newline = stream.indexOf('\n', offset);
        if (newline < 0)
            break;
        QByteArray line = stream.mid(offset, newline - offset);
        offset = newline + 1;
        if (line.endsWith('\r'))
            line.chop(1);
        bool blank = true;
        for (char c : line)
            if (c != ' ' && c != '\t' && c != '\r')
                blank = false;
        if (line.isEmpty() || blank)
            continue;
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject())
            continue;
        QJsonObject event = doc.object();
        event.insert(QStringLiteral("_so"), double(offset));
        event.insert(QStringLiteral("_sk"), key);
        QJsonObject ev = redactValue(QJsonValue(event), credentials).toObject();
        if (!ev.value(QStringLiteral("session_id")).toString().isEmpty() ||
            !ev.value(QStringLiteral("thread_id")).toString().isEmpty())
            ev.insert(QStringLiteral("_forkmesh_account"), account);
        const QString type = ev.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("assistant") && !ev.contains(QStringLiteral("_ts")))
            ev.insert(QStringLiteral("_ts"), ts);
        events += compact(ev) + "\n";
        QString text;
        if (type == QLatin1String("assistant"))
            text = AgentLogText::assistantContent(ev.value(QStringLiteral("message")).toObject());
        else if (type == QLatin1String("user"))
            text = AgentLogText::userEvent(ev);
        if (!text.isEmpty()) {
            log += text.toUtf8();
            if (!text.endsWith(QLatin1Char('\n')))
                log += "\n";
        }
    }
    return QJsonObject{{QStringLiteral("stream"), b64(stream)},
                       {QStringLiteral("credentials"), QJsonArray::fromStringList(credentials)},
                       {QStringLiteral("account"), account},
                       {QStringLiteral("key"), key},
                       {QStringLiteral("ts"), ts},
                       {QStringLiteral("events"), b64(events)},
                       {QStringLiteral("transcript"), b64(log)}};
}

AgentSession sessionFrom(const QJsonObject &object)
{
    return AgentSession::fromJson(object);
}

QJsonObject storeSection()
{
    QTemporaryDir temp;
    const QString root = temp.path() + QStringLiteral("/agents");
    AgentStore store(root);
    QJsonArray ops;
    QHash<int, AgentSession> live;
    const auto record = [&ops](QJsonObject op) { ops.append(op); };

    for (const AgentSession &sample : sampleSessions()) {
        if (!sample.schemaSupported())
            continue;
        AgentSession input = sample;
        input.id = 0;
        input.createdAtMs = 0;
        AgentSession created = store.createSession(input);
        record({{QStringLiteral("op"), QStringLiteral("create")},
                {QStringLiteral("session"), QString::fromUtf8(compact(input.toJson()))},
                {QStringLiteral("id"), created.id}});
        created.createdAtMs = 1759500000000 + created.id;
        store.saveSession(created);
        record({{QStringLiteral("op"), QStringLiteral("save")},
                {QStringLiteral("session"), QString::fromUtf8(compact(created.toJson()))}});
        live.insert(created.id, created);
    }
    const QList<int> ids = live.keys();
    const int first = *std::min_element(ids.begin(), ids.end());
    const int second = *std::max_element(ids.begin(), ids.end());
    const auto appendLog = [&](int id, const QString &text) {
        store.appendLog(live.value(id), text);
        record({{QStringLiteral("op"), QStringLiteral("appendLog")}, {QStringLiteral("id"), id},
                {QStringLiteral("text"), text}});
    };
    const auto appendEvent = [&](int id, const QJsonObject &event) {
        store.appendEvent(live.value(id), event);
        record({{QStringLiteral("op"), QStringLiteral("appendEvent")}, {QStringLiteral("id"), id},
                {QStringLiteral("event"), QString::fromUtf8(compact(event))}});
    };
    appendLog(first, QStringLiteral("\n==> Started\n"));
    appendLog(first, QStringLiteral("no newline"));
    appendLog(first, QStringLiteral(""));
    appendLog(first, QStringLiteral("caf\xc3\xa9\n"));
    appendEvent(first, QJsonObject{{QStringLiteral("type"), QStringLiteral("_local_user")},
                                   {QStringLiteral("text"), QStringLiteral("hi")},
                                   {QStringLiteral("_ts"), 1759512345678.0}});
    appendEvent(first, QJsonObject{{QStringLiteral("type"), QStringLiteral("result")},
                                   {QStringLiteral("_so"), 100000.0},
                                   {QStringLiteral("_sk"), QStringLiteral("agent-1-2")}});
    store.writePatch(live.value(first), QStringLiteral("diff --git a b\n+x\n"));
    record({{QStringLiteral("op"), QStringLiteral("writePatch")}, {QStringLiteral("id"), first},
            {QStringLiteral("patch"), QStringLiteral("diff --git a b\n+x\n")}});
    appendLog(second, QStringLiteral("second log\n"));
    appendEvent(second, QJsonObject{{QStringLiteral("type"), QStringLiteral("x")}});
    store.clearEvents(live.value(second));
    record({{QStringLiteral("op"), QStringLiteral("clearEvents")}, {QStringLiteral("id"), second}});
    appendEvent(second, QJsonObject{{QStringLiteral("type"), QStringLiteral("y")}});
    AgentStore::flushTranscriptWrites();

    store.recycleSession(live.value(second));
    record({{QStringLiteral("op"), QStringLiteral("recycle")}, {QStringLiteral("id"), second}});
    appendLog(second, QStringLiteral("after recycle must not land\n"));
    store.restoreRecycledSession(second);
    record({{QStringLiteral("op"), QStringLiteral("restore")}, {QStringLiteral("id"), second}});
    appendLog(second, QStringLiteral("after restore lands\n"));
    AgentStore::flushTranscriptWrites();
    store.deleteSession(live.value(first));
    record({{QStringLiteral("op"), QStringLiteral("delete")}, {QStringLiteral("id"), first}});
    store.saveSession(live.value(first));
    record({{QStringLiteral("op"), QStringLiteral("save")},
            {QStringLiteral("session"), QString::fromUtf8(compact(live.value(first).toJson()))}});
    AgentSession again = live.value(first);
    again.id = 0;
    again.createdAtMs = 0;
    const AgentSession reused = store.createSession(again);
    record({{QStringLiteral("op"), QStringLiteral("create")},
            {QStringLiteral("session"), QString::fromUtf8(compact(again.toJson()))},
            {QStringLiteral("id"), reused.id}});
    AgentSession reusedFixed = reused;
    reusedFixed.createdAtMs = 1759600000000;
    store.saveSession(reusedFixed);
    record({{QStringLiteral("op"), QStringLiteral("save")},
            {QStringLiteral("session"), QString::fromUtf8(compact(reusedFixed.toJson()))}});
    store.recycleSession(reusedFixed);
    record({{QStringLiteral("op"), QStringLiteral("recycle")}, {QStringLiteral("id"), reused.id}});
    AgentStore::flushTranscriptWrites();

    QJsonObject files;
    QJsonObject modes;
    QDirIterator it(root, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const QString rel = QDir(root).relativeFilePath(path);
        QFile file(path);
        file.open(QIODevice::ReadOnly);
        QByteArray bytes = file.readAll();
        if (rel.endsWith(QLatin1String("deleted-at.json")))
            bytes = "STAMP";
        files.insert(rel, b64(bytes));
        modes.insert(rel, QString::number(int(QFile::permissions(path)), 16));
    }
    QJsonArray dirs;
    QDirIterator dit(root, QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (dit.hasNext())
        dirs.append(QDir(root).relativeFilePath(dit.next()));
    return QJsonObject{{QStringLiteral("ops"), ops}, {QStringLiteral("files"), files},
                       {QStringLiteral("modes"), modes},
                       {QStringLiteral("dirs"), dirs}};
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        std::fprintf(stderr, "usage: golden <out.json>\n");
        return 2;
    }
    const QJsonObject golden{
        {QStringLiteral("qt"), QString::fromLatin1(qVersion())},
        {QStringLiteral("numbers"), numbers()},
        {QStringLiteral("parse"), parses()},
        {QStringLiteral("sessions"), sessionsSection()},
        {QStringLiteral("logtext"), logTextSection()},
        {QStringLiteral("redact"), redactSection()},
        {QStringLiteral("store"), storeSection()},
        {QStringLiteral("claudeStream"), streamSection()},
    };
    QFile out(QString::fromLocal8Bit(argv[1]));
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 1;
    out.write(QJsonDocument(golden).toJson(QJsonDocument::Indented));
    return 0;
}
