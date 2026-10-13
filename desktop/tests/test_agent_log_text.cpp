#include "AgentLogText.h"
#include "ClaudePeerMessaging.h"

#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

#include <iostream>

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void same(const QString &got, const QString &want, const char *what)
{
    if (got == want)
        return;
    std::cerr << "FAIL: " << what << "\n  got:  " << got.toStdString()
              << "\n  want: " << want.toStdString() << "\n";
    ++failures;
}

static QJsonObject json(const char *text)
{
    return QJsonDocument::fromJson(QByteArray(text)).object();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    using namespace AgentLogText;

    same(claudeEntry(json(R"({"type":"user","message":{"role":"user","content":"  fix the build "}})")),
         QStringLiteral("\n> fix the build\n"), "a typed prompt is a > line");
    same(claudeEntry(json(R"({"type":"user","message":{"content":[{"type":"text","text":"look"},{"type":"image"}]}})")),
         QStringLiteral("\n> look\n"), "a prompt with an image keeps its text");
    same(claudeEntry(json(R"({"type":"user","message":{"content":[{"type":"tool_result","content":"ok"}]}})")),
         QString(), "tool results are not prompts");
    same(claudeEntry(json(R"({"type":"user","isMeta":true,"message":{"content":"Caveat: local"}})")),
         QString(), "meta entries are skipped");
    same(claudeEntry(json(R"({"type":"user","message":{"content":"<command-name>/clear</command-name>"}})")),
         QString(), "slash command echoes are skipped");
    same(claudeEntry(json(R"({"type":"assistant","isSidechain":true,"message":{"content":[{"type":"text","text":"sub"}]}})")),
         QString(), "subagent chatter is skipped");
    same(claudeEntry(json(R"({"type":"assistant","message":{"content":[{"type":"thinking","thinking":"hm"},{"type":"text","text":"Done."},{"type":"tool_use","name":"Bash","input":{"command":"make"}}]}})")),
         QStringLiteral("\nDone.\n\n[Bash make]\n"), "replies and tool calls");
    same(claudeEntry(json(R"({"type":"assistant","message":{"content":[{"type":"tool_use","name":"Read","input":{"file_path":"/a/b.cpp"}}]}})")),
         QStringLiteral("\n[Read /a/b.cpp]\n"), "a file tool names its file");
    same(claudeEntry(json(R"({"type":"system","subtype":"compact_boundary"})")),
         QStringLiteral("\n==> Conversation compacted\n"), "compaction is a status line");
    same(claudeEntry(json(R"({"type":"summary","summary":"x"})")), QString(), "summaries are skipped");

    same(codexEntry(json(R"({"type":"event_msg","payload":{"type":"user_message","message":"add tests"}})")),
         QStringLiteral("\n> add tests\n"), "a codex prompt");
    same(codexEntry(json(R"({"type":"response_item","payload":{"type":"message","role":"user","content":[{"type":"input_text","text":"add tests"}]}})")),
         QString(), "the codex prompt is not repeated from its response item");
    same(codexEntry(json(R"({"type":"event_msg","payload":{"type":"agent_message","message":"On it."}})")),
         QString(), "the codex reply is not repeated from its event");
    same(codexEntry(json(R"({"type":"response_item","payload":{"type":"message","role":"assistant","content":[{"type":"output_text","text":"On it."}]}})")),
         QStringLiteral("\nOn it.\n"), "a codex reply");
    same(codexEntry(json(R"({"type":"response_item","payload":{"type":"function_call","name":"shell","arguments":"{\"command\":[\"bash\",\"-lc\",\"ls src\"]}"}})")),
         QStringLiteral("\n[shell ls src]\n"), "a codex shell call");
    same(codexEntry(json(R"({"type":"response_item","payload":{"type":"custom_tool_call","name":"apply_patch","input":"*** Begin Patch\n*** Update File: src/a.rs\n@@\n"}})")),
         QStringLiteral("\n[apply_patch Update File: src/a.rs]\n"), "a codex patch names its file");
    same(codexEntry(json(R"({"type":"message","role":"user","content":[{"type":"input_text","text":"<environment_context>x</environment_context>"}]})")),
         QString(), "an older rollout's context message is skipped");
    same(codexEntry(json(R"({"type":"message","role":"user","content":[{"type":"input_text","text":"go"}]})")),
         QStringLiteral("\n> go\n"), "an older rollout's prompt");

    same(outsideEntry(QStringLiteral("gemini"), json(R"({"type":"user","message":{"content":"x"}})")),
         QString(), "other CLIs are not read");
    check(readsTranscript(QStringLiteral("claude")) && readsTranscript(QStringLiteral("codex")) &&
              !readsTranscript(QStringLiteral("aider")),
          "claude and codex transcripts are read");

    same(claudeEntry(json(R"j({"type":"user","isMeta":true,"message":{"role":"user","content":"Another Claude session sent a message:\n<cross-session-message from-name=\"ForkMesh (phone)\" from-mode=\"bypass\">\nAlso run the tests\n</cross-session-message>\n\nThis came from another Claude session"},"origin":{"kind":"peer","from":"unknown","name":"ForkMesh (phone)","fromMode":"bypass","body":"Also run the tests"}})j")),
         QStringLiteral("\n> ") + QString::fromUtf8("\xE2\x9F\xB5") + QStringLiteral(" ForkMesh (phone): Also run the tests\n"),
         "a phone steer reads as a prompt that names the phone");
    same(claudeEntry(json(R"j({"type":"user","isMeta":true,"message":{"role":"user","content":"Another Claude session sent a message:\n<cross-session-message from-name=\"ForkMesh (phone)\">\nfrom the envelope\n</cross-session-message>\n\nThis came"},"origin":{"kind":"peer","from":"unknown","name":"ForkMesh (phone)"}})j")),
         QStringLiteral("\n> ") + QString::fromUtf8("\xE2\x9F\xB5") + QStringLiteral(" ForkMesh (phone): from the envelope\n"),
         "a steer without origin.body is read from its envelope");
    {
        QJsonObject origin{{QStringLiteral("kind"), QStringLiteral("peer")},
                           {QStringLiteral("name"), QStringLiteral("ForkMesh (phone)")},
                           {QStringLiteral("body"), ClaudePeerMessaging::steerBody(QStringLiteral("Yes"))}};
        QJsonObject entry{{QStringLiteral("type"), QStringLiteral("user")},
                          {QStringLiteral("isMeta"), true},
                          {QStringLiteral("message"), QJsonObject{{QStringLiteral("content"), QStringLiteral("x")}}},
                          {QStringLiteral("origin"), origin}};
        same(claudeEntry(entry),
             QStringLiteral("\n> ") + QString::fromUtf8("\xE2\x9F\xB5") + QStringLiteral(" ForkMesh (phone): Yes\n"),
             "a steer's lead is not shown");
    }
    same(claudeEntry(json(R"({"type":"user","isMeta":true,"message":{"content":"x"},"origin":{"kind":"peer","from":"uds:/run/user/1001/cc-socks/2157150.sock","name":"a-13","body":"ping from a"}})")),
         QStringLiteral("\n> ") + QString::fromUtf8("\xE2\x9F\xB5") + QStringLiteral(" a-13: ping from a\n"),
         "another session's message names its sender");
    same(claudeEntry(json(R"({"type":"user","isMeta":true,"message":{"content":"x"},"origin":{"kind":"peer","body":"no name"}})")),
         QStringLiteral("\n> no name\n"), "a peer message without a name is a plain prompt");
    same(claudeEntry(json(R"({"type":"user","isSidechain":true,"isMeta":true,"message":{"content":"x"},"origin":{"kind":"peer","body":"sub"}})")),
         QString(), "a subagent's peer message is skipped");
    same(claudeEntry(json(R"j({"type":"user","origin":{"kind":"task-notification"},"message":{"role":"user","content":"<task-notification>\n<task-id>b1</task-id>\n<status>completed</status>\n<summary>Agent \"Fix &amp; ship\" finished</summary>\n<note>internal</note>\n<result>Done.\n\n| a | b |\n|---|---|\n| 1 &lt; 2 | **x** |</result>\n<usage><tool_uses>3</tool_uses></usage>\n</task-notification>"}})j")),
         QStringLiteral("\n==> Task completed: Agent \"Fix & ship\" finished\n    Done.\n\n    | a | b |\n    |---|---|\n    | 1 < 2 | **x** |\n"),
         "a task notification is a Task status line with its result indented");
    same(claudeEntry(json(R"j({"type":"user","message":{"content":"<task-notification>\n<task-id>b2</task-id>\n<status>failed</status>\n<summary>Background command \"make\" failed with exit code 2</summary>\n</task-notification>"}})j")),
         QStringLiteral("\n==> Task failed: Background command \"make\" failed with exit code 2\n"),
         "a failed command without a result is one line");
    same(claudeEntry(json(R"j({"type":"user","message":{"content":"<task-notification>\n<task-id>m1</task-id>\n<summary>Monitor event: \"build\"</summary>\n<event>EXIT 0</event>\n</task-notification>"}})j")),
         QStringLiteral("\n==> Task update: Monitor event: \"build\"\n    EXIT 0\n"),
         "a monitor event has no status and shows its event");
    same(claudeEntry(json(R"j({"type":"user","message":{"content":"<task-notification>\n<status>Weird Status!</status>\n<summary>x</summary>\n</task-notification>"}})j")),
         QStringLiteral("\n==> Task update: x\n"), "an unexpected status reads as an update");
    same(claudeEntry(json(R"j({"type":"user","message":{"content":"<task-notification>\n<status>cost</status>\n<summary>y</summary>\n</task-notification>"}})j")),
         QStringLiteral("\n==> Task update: y\n"), "a status never collides with the Task cost line");
    same(claudeEntry(json(R"j({"type":"user","message":{"content":[{"type":"text","text":"<task-notification>\n<status>killed</status>\n<summary>z</summary>\n</task-notification>"}]}})j")),
         QStringLiteral("\n==> Task killed: z\n"), "a notification in a text block");
    {
        QString result;
        for (int i = 0; i < 40; ++i)
            result += QStringLiteral("line %1 of the agent's long report, with enough words\n").arg(i);
        const QString xml = QStringLiteral("<task-notification><status>completed</status><summary>long</summary><result>") +
                            QStringLiteral("```\n") + result + QStringLiteral("```\n") +
                            QStringLiteral("</result></task-notification>");
        const QString out = taskNotification(xml);
        check(out.startsWith(QStringLiteral("\n==> Task completed: long\n    ```\n    line 0 ")), "a long result keeps its start");
        check(out.size() < 1800, "a long result is clipped near 1500 characters");
        check(out.contains(QStringLiteral("\n    ```\n\n    ") + QString::fromUtf8("\xE2\x80\xA6") + QStringLiteral("more (")),
              "a clipped result closes its open fence and ends with a more marker");
        check(out.endsWith(QStringLiteral(" characters not shown)\n")), "the marker counts what was left out");
        check(!out.contains(QStringLiteral("line 39")), "the clipped tail is left out");
    }
    same(userEvent(json(R"j({"type":"user","message":{"role":"user","content":"<task-notification>\n<status>completed</status>\n<summary>Background command \"tests\" completed (exit code 0)</summary>\n</task-notification>"},"parent_tool_use_id":null,"session_id":"s"})j")),
         QStringLiteral("\n==> Task completed: Background command \"tests\" completed (exit code 0)\n"),
         "a ForkMesh run's stream shows task notifications");
    same(userEvent(json(R"({"type":"user","message":{"content":[{"type":"tool_result","content":"ok"}]}})")),
         QString(), "a ForkMesh run's tool results stay out");
    same(userEvent(json(R"j({"type":"user","parent_tool_use_id":"toolu_1","message":{"content":"<task-notification><status>completed</status><summary>sub</summary></task-notification>"}})j")),
         QString(), "a subagent's notification stays out");
    same(claudeEntry(json(R"({"type":"assistant","message":{"content":[{"type":"text","text":"## Plan\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n- **one**\n  - *two*\n\n```json\n{\"k\": 1}\n```"}]}})")),
         QStringLiteral("\n## Plan\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n- **one**\n  - *two*\n\n```json\n{\"k\": 1}\n```\n"),
         "a reply's markdown is kept as written");
    same(claudeEntry(json(R"({"type":"user","message":{"content":"<system-reminder>\nx\n</system-reminder>"}})")),
         QString(), "system reminders stay out");
    {
        QFile session(QStringLiteral(FORKMESH_AGENT_TRANSCRIPT_FIXTURE "/session.jsonl"));
        QFile expected(QStringLiteral(FORKMESH_AGENT_TRANSCRIPT_FIXTURE "/transcript.txt"));
        check(session.open(QIODevice::ReadOnly) && expected.open(QIODevice::ReadOnly),
              "the shared transcript fixture is readable");
        QString log;
        for (const QByteArray &line : session.readAll().split('\n')) {
            if (!line.trimmed().isEmpty())
                log += claudeEntry(QJsonDocument::fromJson(line).object());
        }
        same(log, QString::fromUtf8(expected.readAll()), "the shared session renders to the shared transcript");
    }
    QJsonObject big;
    big.insert(QStringLiteral("command"), QString(300, QLatin1Char('x')));
    check(toolLine(QStringLiteral("Bash"), big).size() < 220, "long tool arguments are clipped");

    if (failures == 0)
        std::cout << "agent log text tests passed\n";
    return failures == 0 ? 0 : 1;
}
