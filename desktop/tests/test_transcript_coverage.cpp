#include "TranscriptCoverage.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>

#include <iostream>

using namespace forkmesh::transcriptcoverage;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QJsonObject parse(const char *json)
{
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

static void testDrawnEvents()
{
    const char *drawn[] = {
        R"({"type":"system","subtype":"init","model":"m","cwd":"/w"})",
        R"({"type":"system","subtype":"thinking_tokens","estimated_tokens":3})",
        R"({"type":"system","subtype":"status","status":null})",
        R"({"type":"system","subtype":"vcs_state_changed","kind":"push","cwd":"/w"})",
        R"({"type":"system","subtype":"vcs_state_changed"})",
        R"({"type":"system","subtype":"task_progress","task_id":"t"})",
        R"({"type":"assistant","message":{"content":[{"type":"thinking","thinking":"x"},{"type":"text","text":"hi"},{"type":"tool_use","id":"a","name":"Bash","input":{}}]}})",
        R"({"type":"assistant","message":{"content":[{"type":"web_search_tool_result","tool_use_id":"a"}]}})",
        R"({"type":"user","message":{"content":[{"type":"tool_result","tool_use_id":"a","content":"ok"}]}})",
        R"({"type":"user","message":{"content":"a prompt"}})",
        R"({"type":"stream_event","event":{"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"s"}}})",
        R"({"type":"stream_event","event":{"type":"content_block_start","index":0,"content_block":{"type":"tool_use"}}})",
        R"({"type":"stream_event","event":{"type":"message_stop"}})",
        R"({"type":"result","subtype":"success","result":"done"})",
        R"({"type":"_codex_agent_delta","item_id":"i","delta":"x"})",
        R"({"type":"_local_notice","text":"n"})",
        R"({"type":"rate_limit_event","rate_limit_info":{}})",
        R"({"type":"control_request","request_id":"r","request":{"subtype":"can_use_tool"}})",
        R"({"type":"file-history-snapshot","snapshot":{}})",
        R"({"type":"tool_progress","parent_tool_use_id":"toolu_1","heartbeat":true})",
        R"({"type":"assistant","parent_tool_use_id":"toolu_1","message":{"content":[{"type":"image"}]}})",
        R"({"type":"user","isSidechain":true,"message":{"content":[{"type":"image"}]}})",
    };
    for (const char *json : drawn) {
        if (!eventGaps(parse(json)).isEmpty())
            std::cerr << "  flagged: " << json << "\n";
        check(eventGaps(parse(json)).isEmpty(), "a drawn or deliberately unshown event is not a gap");
    }
}

static void testMissingEvents()
{
    check(eventGaps(parse(R"({"type":"command_lifecycle","state":"started"})")) ==
              QStringList{QStringLiteral("type:command_lifecycle")},
          "an event type the transcript ignores is a gap");
    check(eventGaps(parse(R"({"type":"system","subtype":"permission_denied","tool_name":"Bash"})")) ==
              QStringList{QStringLiteral("system:permission_denied")},
          "a system subtype the transcript ignores is a gap");
    check(eventGaps(parse(R"({"type":"assistant","message":{"content":[{"type":"text","text":"a"},{"type":"image"},{"type":"image"}]}})")) ==
              QStringList{QStringLiteral("assistant:image")},
          "an assistant block the transcript skips is one gap per kind");
    check(eventGaps(parse(R"({"type":"user","message":{"content":[{"type":"document"}]}})")) ==
              QStringList{QStringLiteral("user:document")},
          "a user block the transcript skips is a gap");
    check(eventGaps(parse(R"({"type":"stream_event","event":{"type":"content_block_delta","delta":{"type":"citations_delta"}}})")) ==
              QStringList{QStringLiteral("delta:citations_delta")},
          "a stream delta the transcript skips is a gap");
    check(eventGaps(parse(R"({"type":"stream_event","event":{"type":"content_block_start","content_block":{"type":"container_upload"}}})")) ==
              QStringList{QStringLiteral("assistant:container_upload")},
          "a streamed block start shares the assistant block's gap");
    check(eventGaps(parse(R"({"message":"x"})")) == QStringList{QStringLiteral("type:(none)")},
          "an untyped event is a gap");
}

static void testRawLines()
{
    check(rawLineGaps(QStringLiteral(R"({"method":"item/agentMessage/delta","params":{}})")).isEmpty(),
          "a handled Codex notification is drawn");
    check(rawLineGaps(QStringLiteral(R"({"method":"item/started","params":{"item":{"type":"commandExecution"}}})")).isEmpty(),
          "a Codex tool item is drawn");
    check(rawLineGaps(QStringLiteral(R"({"method":"item/completed","params":{"item":{"type":"hookPrompt"}}})")) ==
              QStringList{QStringLiteral("codex-item:hookPrompt")},
          "a Codex item the session drops is a gap");
    check(rawLineGaps(QStringLiteral(R"({"method":"model/rerouted","params":{}})")) ==
              QStringList{QStringLiteral("codex:model/rerouted")},
          "a Codex notification the session drops is a gap");
    check(rawLineGaps(QStringLiteral(R"({"method":"account/updated","params":{}})")).isEmpty() &&
              rawLineGaps(QStringLiteral(R"({"method":"codex/event/task_started","params":{}})")).isEmpty(),
          "Codex bookkeeping is deliberately unshown");
    check(rawLineGaps(QStringLiteral(R"({"id":3,"result":{}})")).isEmpty(),
          "a Codex response is not a transcript entry");
    check(rawLineGaps(QStringLiteral(R"({"type":"assistant","method":"x"})")).isEmpty(),
          "a Claude line in the raw log is judged as an event, not here");
    check(rawLineGaps(QStringLiteral("[stderr] \"method\" warning")).isEmpty() &&
              rawLineGaps(QStringLiteral("{\"method\":\"tru")).isEmpty(),
          "stderr and cut lines are not judged");
}

static void testCodexStatusMessages()
{
    const char *messages[] = {
        R"({"method":"remoteControl/status/changed","params":{"environmentId":null,"installationId":"installation","serverName":"a","status":"disabled"}})",
        R"({"method":"mcpServer/startupStatus/updated","params":{"error":null,"failureReason":null,"name":"codex_apps","status":"starting","threadId":"t"}})",
        R"({"method":"mcpServer/startupStatus/updated","params":{"error":{"message":"connection refused"},"failureReason":"transport","name":"codex_apps","status":"failed","threadId":"t"}})",
        R"({"method":"thread/settings/updated","params":{"threadId":"t","threadSettings":{"model":"gpt-test","effort":"high","approvalPolicy":"never","sandboxPolicy":{"type":"dangerFullAccess"},"collaborationMode":{"mode":"default","settings":{"developer_instructions":"untrusted instructions"}}}}})",
        R"({"method":"turn/moderationMetadata","params":{"threadId":"t","turnId":"turn","metadata":{"generation":{},"prompt":{"omnimod":{"outputs":[{"is_blocked":false,"results":[{"category_flags":{"A1":false}}]}]}},"tool_call":{},"tool_response":{}}}})",
    };
    for (const char *json : messages) {
        const QString line = QString::fromUtf8(json);
        check(rawLineGaps(line).isEmpty(),
              "Codex status notices and internal moderation metadata are covered");
        Tracker tracker;
        check(!tracker.noteEvent(droppedLineEvent(QStringLiteral("codex"), line)) &&
                  tracker.isEmpty(),
              "replaying previously dropped Codex status messages clears their gaps");
    }
    check(rawLineGaps(QStringLiteral(R"({"method":"turn/moderationMetadata/new","params":{}})")) ==
              QStringList{QStringLiteral("codex:turn/moderationMetadata/new")},
          "the moderation exclusion only covers the known method");
}

static void testDroppedClaudeLines()
{
    check(claudeLineGaps(QStringLiteral("Warning: something on stdout")) ==
              QStringList{QStringLiteral("claude-line:(plain text)")},
          "a plain stdout line is a gap");
    check(claudeLineGaps(QStringLiteral(R"({"type":"control_request","request_id":"r","request":{"subtype":"request_user_dialog","dialog_kind":"new_kind"}})")) ==
              QStringList{QStringLiteral("claude-line:control_request/request_user_dialog/new_kind")},
          "a dialog ForkMesh cannot answer is a gap named for its kind");
    check(claudeLineGaps(QStringLiteral(R"({"type":"control_request","request_id":"r","request":{"subtype":"mcp_message"}})")) ==
              QStringList{QStringLiteral("claude-line:control_request/mcp_message")},
          "a control request ForkMesh refuses is a gap");
    check(claudeLineGaps(QStringLiteral(R"({"type":"control_response","response":{"request_id":"r"}})")).isEmpty() &&
              claudeLineGaps(QStringLiteral(R"({"type":"control_cancel_request","request_id":"r"})")).isEmpty() &&
              claudeLineGaps(QStringLiteral("   ")).isEmpty(),
          "answers to ForkMesh's own requests and stale cancels are bookkeeping");
    const QJsonObject dropped = droppedLineEvent(QStringLiteral("claude"),
                                                 QStringLiteral("plain words"));
    check(dropped.value(QStringLiteral("type")).toString() == QLatin1String("_local_raw_line") &&
              eventGaps(dropped) == QStringList{QStringLiteral("claude-line:(plain text)")} &&
              gapLabel(QStringLiteral("claude-line:(plain text)")) ==
                  QLatin1String("Claude Code `(plain text)` output line"),
          "a recorded Claude line is judged again when the transcript is drawn");
    const QString huge = QStringLiteral(R"({"type":"control_request","request_id":"r","request":{"subtype":"request_user_dialog","dialog_kind":"big","payload":{"text":")") +
                         QString(40000, QLatin1Char('x')) + QStringLiteral(R"("}}})");
    const QJsonObject clipped = droppedLineEvent(QStringLiteral("claude"), huge);
    check(clipped.value(QStringLiteral("line")).toString().size() < 1000 &&
              eventGaps(clipped) ==
                  QStringList{QStringLiteral("claude-line:control_request/request_user_dialog/big")},
          "a huge dropped line is stored as the fields that name its kind");
}

static void testTracker()
{
    Tracker tracker;
    check(tracker.isEmpty(), "a new tracker has no gaps");
    check(!tracker.noteEvent(parse(R"({"type":"result"})")), "a drawn event adds nothing");
    check(tracker.noteEvent(parse(R"({"type":"command_lifecycle","state":"started"})")),
          "the first event of a missing kind is reported");
    check(!tracker.noteEvent(parse(R"({"type":"command_lifecycle","state":"done"})")),
          "a repeat of a known gap is counted, not reported again");
    check(tracker.kindCount() == 1 && tracker.total() == 2 &&
              tracker.gaps().first().count == 2,
          "the tracker counts each gap kind");
    check(tracker.gaps().first().sample.contains(QLatin1String("started")),
          "the sample is the first event of its kind");
    check(!tracker.noteEvent(droppedLineEvent(
              QStringLiteral("codex"), QStringLiteral(R"({"method":"turn/started","params":{}})"))),
          "a Codex message the session handles is no gap even when recorded");
    check(tracker.noteEvent(droppedLineEvent(
              QStringLiteral("codex"), QStringLiteral(R"({"method":"model/rerouted","params":{}})"))) &&
              tracker.kindCount() == 2,
          "a Codex message the session dropped is a gap");
    check(tracker.gaps().at(1).sample == QLatin1String(R"({"method":"model/rerouted","params":{}})"),
          "a dropped line's sample is the line itself");
    check(tracker.gaps().at(1).label == QLatin1String("Codex `model/rerouted` message"),
          "a gap carries a readable label");
    const QString text = summary(tracker.gaps());
    check(text.contains(QStringLiteral("2 × command_lifecycle event")) &&
              text.contains(QStringLiteral("1 × Codex model/rerouted message")),
          "the summary lists counts and labels");
    const QString prompt = agentPrompt(tracker.gaps(), QStringLiteral("agent #7"));
    check(prompt.contains(QLatin1String("agent #7")) &&
              prompt.contains(QLatin1String("`command_lifecycle` event, 2 time(s)")) &&
              prompt.contains(QLatin1String("TranscriptCoverage.cpp")) &&
              prompt.contains(QLatin1String("untrusted")),
          "the prompt names the session, each gap and where to fix it");
    tracker.clear();
    check(tracker.isEmpty() && tracker.total() == 0, "clear forgets every gap");
}

static void testLongSample()
{
    Tracker tracker;
    const QString big(5000, QLatin1Char('x'));
    tracker.noteEvent(QJsonObject{{QStringLiteral("type"), QStringLiteral("huge")},
                                  {QStringLiteral("blob"), big}});
    check(tracker.gaps().first().sample.size() < 1300, "samples are clipped");
}

static void testCodexToolItems()
{
    check(isCodexToolItem(QStringLiteral("commandExecution")) &&
              isCodexToolItem(QStringLiteral("fileChange")) &&
              !isCodexToolItem(QStringLiteral("agentMessage")),
          "Codex tool items are the ones drawn as tool cards");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testDrawnEvents();
    testMissingEvents();
    testRawLines();
    testCodexStatusMessages();
    testDroppedClaudeLines();
    testTracker();
    testLongSample();
    testCodexToolItems();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "transcript coverage tests passed\n";
    return 0;
}
