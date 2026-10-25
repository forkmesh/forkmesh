#include "ClaudeControlProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>

#include <iostream>

using namespace forkmesh::claudecontrol;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QJsonObject controlRequest(const QString &id, const QJsonObject &body)
{
    return QJsonObject{{QStringLiteral("type"), QStringLiteral("control_request")},
                       {QStringLiteral("request_id"), id},
                       {QStringLiteral("request"), body}};
}

static QJsonObject canUseTool(const QString &id, const QString &tool, const QJsonObject &input,
                              const QJsonArray &suggestions = {})
{
    QJsonObject body{{QStringLiteral("subtype"), QStringLiteral("can_use_tool")},
                     {QStringLiteral("tool_name"), tool},
                     {QStringLiteral("input"), input},
                     {QStringLiteral("tool_use_id"), QStringLiteral("toolu_") + id}};
    if (!suggestions.isEmpty())
        body.insert(QStringLiteral("permission_suggestions"), suggestions);
    return controlRequest(id, body);
}

static QStringList optionLabels(const Prompt &prompt)
{
    QStringList labels;
    const QJsonArray questions = prompt.card.value(QStringLiteral("questions")).toArray();
    for (const QJsonValue &option :
         questions.at(0).toObject().value(QStringLiteral("options")).toArray())
        labels << option.toObject().value(QStringLiteral("label")).toString();
    return labels;
}

static void checkInitialize()
{
    const QJsonObject init = initializeRequest(QStringLiteral("  Fix\nthe login\tbug  "));
    check(init.value(QStringLiteral("subtype")).toString() == QLatin1String("initialize"),
          "initialize names its subtype");
    check(init.value(QStringLiteral("agentProgressSummaries")).toBool() &&
              init.value(QStringLiteral("perTaskStopAffordance")).toBool(),
          "initialize asks for sub-session summaries and declares the stop control");
    check(init.value(QStringLiteral("title")).toString() == QLatin1String("Fix the login bug"),
          "initialize carries a cleaned session title");
    QStringList kinds;
    for (const QJsonValue &kind : init.value(QStringLiteral("supportedDialogKinds")).toArray())
        kinds << kind.toString();
    check(kinds == supportedDialogKinds() && kinds.contains(QStringLiteral("refusal_fallback_prompt")),
          "initialize declares exactly the dialog kinds the desktop renders");
    check(!initializeRequest(QString()).contains(QStringLiteral("title")),
          "no title leaves Claude Code to name the session");
}

static void checkPermission()
{
    const QJsonArray suggestions{QJsonObject{{QStringLiteral("type"), QStringLiteral("setMode")},
                                             {QStringLiteral("mode"), QStringLiteral("acceptEdits")},
                                             {QStringLiteral("destination"), QStringLiteral("session")}}};
    const QJsonObject input{{QStringLiteral("file_path"), QStringLiteral("/repo/a.txt")},
                            {QStringLiteral("content"), QStringLiteral("hi")}};
    const QJsonObject event = canUseTool(QStringLiteral("r1"), QStringLiteral("Write"), input,
                                         suggestions);
    const Prompt prompt = promptFor(event);
    check(prompt.kind == PromptKind::Permission, "a tool call that needs approval is a permission prompt");
    check(prompt.key == QLatin1String("claude-approval-r1"),
          "the permission card id marks it as an approval");
    check(optionLabels(prompt) == QStringList({kAllowOnce, kAllowAndRemember, kDeny}),
          "a call with suggestions offers allow once, remember and deny");
    const QString question = prompt.card.value(QStringLiteral("questions")).toArray().at(0)
                                 .toObject().value(QStringLiteral("question")).toString();
    check(question.contains(QStringLiteral("/repo/a.txt")), "the card names the file");

    const QJsonObject card = cardEvent(prompt);
    check(card.value(QStringLiteral("type")).toString() == QLatin1String("assistant") &&
              !card.contains(QStringLiteral("session_id")),
          "the synthesized card carries no session id that could replace the resume id");

    Answer once = answerFor(event, kAllowOnce);
    check(once.valid && once.response.value(QStringLiteral("behavior")).toString() == QLatin1String("allow") &&
              once.response.value(QStringLiteral("updatedInput")).toObject() == input &&
              once.response.value(QStringLiteral("toolUseID")).toString() == QLatin1String("toolu_r1") &&
              !once.response.contains(QStringLiteral("updatedPermissions")),
          "allow once allows the call unchanged");
    Answer remember = answerFor(event, kAllowAndRemember);
    check(remember.response.value(QStringLiteral("updatedPermissions")).toArray() == suggestions &&
              remember.permissionMode == QLatin1String("acceptEdits"),
          "allow and remember applies Claude's suggestions and reports the new mode");
    Answer denied = answerFor(event, kDeny);
    check(denied.response.value(QStringLiteral("behavior")).toString() == QLatin1String("deny") &&
              !denied.response.value(QStringLiteral("message")).toString().isEmpty(),
          "deny refuses with a message");
    Answer told = answerFor(event, QStringLiteral("use b.txt instead"));
    check(told.response.value(QStringLiteral("behavior")).toString() == QLatin1String("deny") &&
              told.response.value(QStringLiteral("message")).toString().contains(QStringLiteral("use b.txt instead")),
          "a typed answer denies and tells Claude what to do instead");

    const Prompt bare = promptFor(canUseTool(QStringLiteral("r2"), QStringLiteral("Bash"),
                                             QJsonObject{{QStringLiteral("command"),
                                                          QStringLiteral("\x1b[31mrm -r build\x1b[0m")}}));
    check(optionLabels(bare) == QStringList({kAllowOnce, kDeny}),
          "a call without suggestions offers no remember option");
    const QString bareQuestion = bare.card.value(QStringLiteral("questions")).toArray().at(0)
                                     .toObject().value(QStringLiteral("question")).toString();
    check(bareQuestion.contains(QStringLiteral("rm -r build")) && !bareQuestion.contains(QChar(0x1b)),
          "terminal escapes are stripped from the command shown");
}

static void checkQuestion()
{
    const QJsonObject input{
        {QStringLiteral("questions"),
         QJsonArray{QJsonObject{{QStringLiteral("question"), QStringLiteral("Continue?")},
                                {QStringLiteral("options"), QJsonArray{}}},
                    QJsonObject{{QStringLiteral("question"), QStringLiteral("Which branch?")},
                                {QStringLiteral("options"), QJsonArray{}}}}}};
    const QJsonObject event = canUseTool(QStringLiteral("q1"), QStringLiteral("AskUserQuestion"), input);
    const Prompt prompt = promptFor(event);
    check(prompt.kind == PromptKind::Question && prompt.key == QLatin1String("toolu_q1") &&
              cardEvent(prompt).isEmpty(),
          "AskUserQuestion answers the transcript's own card, keyed by its tool use id");
    const Answer answer = answerFor(
        event, QStringLiteral("Continue? → Yes\nWhich branch? → main"));
    const QJsonObject answers =
        answer.response.value(QStringLiteral("updatedInput")).toObject().value(QStringLiteral("answers")).toObject();
    check(answer.valid && answers.value(QStringLiteral("Continue?")).toString() == QLatin1String("Yes") &&
              answers.value(QStringLiteral("Which branch?")).toString() == QLatin1String("main"),
          "a multi-question answer is split back into Claude's answers map");
    const QJsonObject single{
        {QStringLiteral("questions"),
         QJsonArray{QJsonObject{{QStringLiteral("question"), QStringLiteral("Proceed?")}}}}};
    const Answer one = answerFor(canUseTool(QStringLiteral("q2"), QStringLiteral("AskUserQuestion"), single),
                                 QStringLiteral("No"));
    check(one.response.value(QStringLiteral("updatedInput")).toObject().value(QStringLiteral("answers"))
                  .toObject().value(QStringLiteral("Proceed?")).toString() == QLatin1String("No"),
          "a single answer maps to its question");
}

static void checkPlan()
{
    const QJsonObject event = canUseTool(QStringLiteral("p1"), QStringLiteral("ExitPlanMode"),
                                         QJsonObject{{QStringLiteral("plan"), QStringLiteral("1. do it")}});
    const Prompt prompt = promptFor(event);
    check(prompt.kind == PromptKind::Plan &&
              optionLabels(prompt) == QStringList({kApproveAcceptEdits, kApproveAskEdits, kKeepPlanning}),
          "ExitPlanMode asks for plan approval");
    const Answer approve = answerFor(event, kApproveAcceptEdits);
    const QJsonObject mode =
        approve.response.value(QStringLiteral("updatedPermissions")).toArray().at(0).toObject();
    check(approve.response.value(QStringLiteral("behavior")).toString() == QLatin1String("allow") &&
              mode.value(QStringLiteral("type")).toString() == QLatin1String("setMode") &&
              mode.value(QStringLiteral("mode")).toString() == QLatin1String("acceptEdits") &&
              approve.permissionMode == QLatin1String("acceptEdits"),
          "approving with edits switches the session out of plan mode");
    check(answerFor(event, kApproveAskEdits).permissionMode == QLatin1String("default"),
          "approving with asks switches to the ask mode");
    const Answer keep = answerFor(event, QStringLiteral("split step 1"));
    check(keep.response.value(QStringLiteral("behavior")).toString() == QLatin1String("deny") &&
              keep.response.value(QStringLiteral("message")).toString().contains(QStringLiteral("split step 1")),
          "feedback keeps Claude planning");
}

static void checkDialogs()
{
    const QJsonObject refusal = controlRequest(
        QStringLiteral("d1"),
        QJsonObject{{QStringLiteral("subtype"), QStringLiteral("request_user_dialog")},
                    {QStringLiteral("dialog_kind"), QStringLiteral("refusal_fallback_prompt")},
                    {QStringLiteral("payload"),
                     QJsonObject{{QStringLiteral("originalModel"), QStringLiteral("opus")},
                                 {QStringLiteral("fallbackModel"), QStringLiteral("sonnet")}}}});
    const Prompt prompt = promptFor(refusal);
    check(prompt.kind == PromptKind::Dialog &&
              optionLabels(prompt) == QStringList({QStringLiteral("Retry on sonnet"),
                                                   QStringLiteral("Edit my prompt")}),
          "the refusal dialog offers the fallback model");
    const Answer retry = answerFor(refusal, QStringLiteral("Retry on sonnet"));
    check(retry.response.value(QStringLiteral("behavior")).toString() == QLatin1String("completed") &&
              retry.response.value(QStringLiteral("result")).toString() == QLatin1String("retry_fallback"),
          "retry maps to retry_fallback");
    const Answer rewrite = answerFor(refusal, QStringLiteral("summarize it instead"));
    check(rewrite.response.value(QStringLiteral("result")).toString() == QLatin1String("edit_prompt") &&
              rewrite.followUp == QLatin1String("summarize it instead"),
          "a typed answer edits the prompt and is sent as the next turn");

    const QJsonObject fallback = controlRequest(
        QStringLiteral("d2"),
        QJsonObject{{QStringLiteral("subtype"), QStringLiteral("request_user_dialog")},
                    {QStringLiteral("dialog_kind"), QStringLiteral("auto_mode_server_fallback")},
                    {QStringLiteral("payload"),
                     QJsonObject{{QStringLiteral("title"), QStringLiteral("Classifier billing")},
                                 {QStringLiteral("paragraphs"), QJsonArray{QStringLiteral("p")}},
                                 {QStringLiteral("helpUrl"), QStringLiteral("https://example.com")}}}});
    check(answerFor(fallback, QStringLiteral("Stop this turn")).response.value(QStringLiteral("result")).toString() ==
              QLatin1String("interrupt"),
          "stopping the turn maps to interrupt");

    const QJsonObject unknown = controlRequest(
        QStringLiteral("d3"),
        QJsonObject{{QStringLiteral("subtype"), QStringLiteral("request_user_dialog")},
                    {QStringLiteral("dialog_kind"), QStringLiteral("lsp_recommendation")}});
    check(promptFor(unknown).kind == PromptKind::Unanswerable && !answerFor(unknown, QStringLiteral("x")).valid,
          "a dialog kind that was never declared is left unanswered");
    const QJsonObject hook = controlRequest(
        QStringLiteral("h1"), QJsonObject{{QStringLiteral("subtype"), QStringLiteral("hook_callback")}});
    check(promptFor(hook).kind == PromptKind::None, "requests nobody renders are not prompts");
}

static void checkElicitation()
{
    const QJsonObject event = controlRequest(
        QStringLiteral("e1"),
        QJsonObject{
            {QStringLiteral("subtype"), QStringLiteral("elicitation")},
            {QStringLiteral("mcp_server_name"), QStringLiteral("tickets")},
            {QStringLiteral("message"), QStringLiteral("Pick a project")},
            {QStringLiteral("property_order"), QJsonArray{QStringLiteral("project"), QStringLiteral("count"),
                                                          QStringLiteral("urgent")}},
            {QStringLiteral("requested_schema"),
             QJsonObject{{QStringLiteral("type"), QStringLiteral("object")},
                         {QStringLiteral("required"), QJsonArray{QStringLiteral("project")}},
                         {QStringLiteral("properties"),
                          QJsonObject{{QStringLiteral("urgent"), QJsonObject{{QStringLiteral("type"), QStringLiteral("boolean")}}},
                                      {QStringLiteral("count"), QJsonObject{{QStringLiteral("type"), QStringLiteral("integer")}}},
                                      {QStringLiteral("project"),
                                       QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                                   {QStringLiteral("enum"), QJsonArray{QStringLiteral("web"), QStringLiteral("app")}},
                                                   {QStringLiteral("enumNames"), QJsonArray{QStringLiteral("Website"), QStringLiteral("App")}}}}}}}}});
    const Prompt prompt = promptFor(event);
    check(prompt.kind == PromptKind::Elicitation && prompt.key == QLatin1String("claude-input-e1"),
          "an MCP elicitation opens its own window");
    const QList<ElicitationField> fields = elicitationFields(event);
    check(fields.size() == 3 && fields.at(0).name == QLatin1String("project") &&
              fields.at(0).required && fields.at(0).choiceLabels == QStringList({QStringLiteral("Website"), QStringLiteral("App")}),
          "fields follow the server's order with their choices");
    QString error;
    elicitationContent(fields, {}, &error);
    check(!error.isEmpty(), "a missing required field is refused");
    const QJsonObject content = elicitationContent(
        fields, {{QStringLiteral("project"), QStringLiteral("web")},
                 {QStringLiteral("count"), QStringLiteral("3")},
                 {QStringLiteral("urgent"), QStringLiteral("true")}},
        &error);
    check(error.isEmpty() && content.value(QStringLiteral("project")).toString() == QLatin1String("web") &&
              content.value(QStringLiteral("count")).toInt() == 3 &&
              content.value(QStringLiteral("urgent")).toBool(),
          "values are converted to the schema's types");
    elicitationContent(fields, {{QStringLiteral("project"), QStringLiteral("web")},
                                {QStringLiteral("count"), QStringLiteral("three")}},
                       &error);
    check(!error.isEmpty(), "a non-number for an integer field is refused");
    check(elicitationResponse(QStringLiteral("decline"), content) ==
              QJsonObject{{QStringLiteral("action"), QStringLiteral("decline")}},
          "a declined elicitation sends no content");
}

static void checkTitlesAndEnvelopes()
{
    const QJsonObject changed{{QStringLiteral("type"), QStringLiteral("system")},
                              {QStringLiteral("subtype"), QStringLiteral("session_title_changed")},
                              {QStringLiteral("title"), QString::fromUtf8("Fix​  login‮ bug")}};
    check(titleFromEvent(changed) == QLatin1String("Fix login bug"),
          "a session name loses zero-width and direction characters");
    check(titleFromEvent(QJsonObject{{QStringLiteral("type"), QStringLiteral("system")}}).isEmpty(),
          "other system events carry no name");
    check(cleanTitle(QString(300, QLatin1Char('a'))).size() == 200, "names are capped at 200 characters");
    const QJsonObject ok = success(QStringLiteral("r9"), QJsonObject{{QStringLiteral("behavior"), QStringLiteral("allow")}});
    check(ok.value(QStringLiteral("type")).toString() == QLatin1String("control_response") &&
              ok.value(QStringLiteral("response")).toObject().value(QStringLiteral("request_id")).toString() ==
                  QLatin1String("r9"),
          "a success response names the request it answers");
    check(failure(QStringLiteral("r8"), QStringLiteral("no")).value(QStringLiteral("response")).toObject()
                  .value(QStringLiteral("subtype")).toString() == QLatin1String("error"),
          "an error response is marked as one");
    check(permissionModeLabel(QStringLiteral("acceptEdits")) == QLatin1String("Edit") &&
              permissionModeLabel(QStringLiteral("default")) == QLatin1String("Ask"),
          "Claude's permission modes map to the desktop's labels");
    const QString demoted = permissionModeChangeNotice(QStringLiteral("bypassPermissions"),
                                                       QStringLiteral("acceptEdits"));
    check(demoted.contains(QStringLiteral("from Auto mode (bypassPermissions) to Edit mode (acceptEdits)")) &&
              demoted.contains(QStringLiteral("Bypass permissions mode is no longer active")) &&
              demoted.endsWith(QStringLiteral("\n\n")),
          "leaving Auto tells the agent its bypass instructions no longer hold");
    check(permissionModeChangeNotice(QStringLiteral("acceptEdits"), QStringLiteral("default"))
                  .contains(QStringLiteral("wait for the user's approval")) &&
              !permissionModeChangeNotice(QStringLiteral("acceptEdits"), QStringLiteral("default"))
                   .contains(QStringLiteral("Bypass")),
          "a switch to Ask says tool calls now wait for approval");
    check(permissionModeChangeNotice(QStringLiteral("default"), QStringLiteral("default")).isEmpty() &&
              permissionModeChangeNotice(QString(), QStringLiteral("default")).isEmpty() &&
              permissionModeChangeNotice(QStringLiteral("default"), QStringLiteral("unknown")).isEmpty(),
          "no notice without a real change to a known mode");
}

int main()
{
    checkInitialize();
    checkPermission();
    checkQuestion();
    checkPlan();
    checkDialogs();
    checkElicitation();
    checkTitlesAndEnvelopes();
    if (failures == 0)
        std::cout << "All Claude control protocol tests passed.\n";
    return failures == 0 ? 0 : 1;
}
