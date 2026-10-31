#include "AgentHub.h"

#include <QCoreApplication>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::agenthub;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QString suggestError(const QJsonObject &params)
{
    QString error;
    return parseSuggestRequest(params, nullptr, &error) ? QString() : error;
}

static void testSuggest()
{
    SuggestRequest request;
    QString error;
    check(parseSuggestRequest(QJsonObject{{QStringLiteral("prompt"), QStringLiteral("  Fix the login page\nIt 500s  ")},
                                          {QStringLiteral("title"), QStringLiteral("  Login   fix ")},
                                          {QStringLiteral("repository"), QStringLiteral("forkmesh/forkmesh")},
                                          {QStringLiteral("reason"), QStringLiteral(" separate  task ")}},
                              &request, &error),
          "suggest parses");
    check(request.owner == QLatin1String("forkmesh") && request.name == QLatin1String("forkmesh"),
          "suggest repository");
    check(request.prompt == QLatin1String("Fix the login page\nIt 500s"), "suggest prompt trimmed");
    check(request.title == QLatin1String("Login fix"), "suggest title simplified");
    check(request.reason == QLatin1String("separate task"), "suggest reason simplified");

    check(suggestError(QJsonObject{{QStringLiteral("prompt"), QStringLiteral("x")}}).isEmpty(),
          "repository is optional");
    check(suggestError(QJsonObject{}) == QLatin1String("bad_task"), "prompt required");
    check(suggestError(QJsonObject{{QStringLiteral("prompt"), QString(8001, QLatin1Char('x'))}}) ==
              QLatin1String("bad_task"),
          "prompt too long");
    check(suggestError(QJsonObject{{QStringLiteral("prompt"), QStringLiteral("x")},
                                   {QStringLiteral("repository"), QStringLiteral("nope")}}) ==
              QLatin1String("bad_repository"),
          "bad repository");
    check(suggestError(QJsonObject{{QStringLiteral("prompt"), QStringLiteral("x")},
                                   {QStringLiteral("reason"), QString(301, QLatin1Char('r'))}}) ==
              QLatin1String("bad_reason"),
          "reason too long");
    check(suggestError(QJsonObject{{QStringLiteral("prompt"), 5}}) == QLatin1String("bad_request"),
          "wrong type");

    SuggestRequest untitled;
    untitled.prompt = QStringLiteral("Write the release notes for 2.4\nthen post them");
    check(suggestionTitle(untitled) == QLatin1String("Write the release notes for 2.4"),
          "title from the first prompt line");
    const QString body = suggestionBody(untitled, QStringLiteral("a/b"), QStringLiteral("Agent #4"));
    check(body.contains(QLatin1String("> Write the release notes")), "body quotes the prompt");
    check(body.contains(QLatin1String("> then post them")), "every prompt line is quoted");
    check(body.contains(QLatin1String("Repository: **a/b**")), "body names the repository");
    check(body.contains(QLatin1String("Suggested by Agent #4")), "body names the agent");
    untitled.prompt = QString(5000, QLatin1Char('p'));
    check(suggestionBody(untitled, QString(), QString()).size() < 1400, "long prompts are cut");
}

static void testList()
{
    ListRequest request;
    QString error;
    check(parseListRequest(QJsonObject{}, &request, &error) && !request.includeFinished &&
              request.limit == kDefaultListed,
          "list defaults");
    check(parseListRequest(QJsonObject{{QStringLiteral("include_finished"), true},
                                       {QStringLiteral("limit"), 5}},
                           &request, &error) &&
              request.includeFinished && request.limit == 5,
          "list options");
    check(!parseListRequest(QJsonObject{{QStringLiteral("limit"), 0}}, &request, &error) &&
              error == QLatin1String("bad_limit"),
          "limit lower bound");
    check(!parseListRequest(QJsonObject{{QStringLiteral("limit"), 101}}, &request, &error),
          "limit upper bound");
    check(!parseListRequest(QJsonObject{{QStringLiteral("limit"), 2.5}}, &request, &error),
          "limit whole number");
    check(!parseListRequest(QJsonObject{{QStringLiteral("include_finished"), 1}}, &request, &error),
          "include_finished is a boolean");
}

static void testMessage()
{
    MessageRequest request;
    QString error;
    check(parseMessageRequest(QJsonObject{{QStringLiteral("agent_id"), 12},
                                          {QStringLiteral("message"), QStringLiteral("  hi  ")}},
                              &request, &error) &&
              request.target == 12 && request.text == QLatin1String("hi"),
          "message parses");
    check(!parseMessageRequest(QJsonObject{{QStringLiteral("agent_id"), QStringLiteral("12")},
                                           {QStringLiteral("message"), QStringLiteral("hi")}},
                               &request, &error) &&
              error == QLatin1String("bad_agent"),
          "agent id is a number");
    check(!parseMessageRequest(QJsonObject{{QStringLiteral("agent_id"), -3},
                                           {QStringLiteral("message"), QStringLiteral("hi")}},
                               &request, &error),
          "negative ids refused");
    check(!parseMessageRequest(QJsonObject{{QStringLiteral("agent_id"), 3},
                                           {QStringLiteral("message"), QStringLiteral("  ")}},
                               &request, &error) &&
              error == QLatin1String("bad_message"),
          "empty message refused");
    check(!parseMessageRequest(QJsonObject{{QStringLiteral("agent_id"), 3},
                                           {QStringLiteral("message"), QString(8001, QLatin1Char('m'))}},
                               &request, &error),
          "long message refused");

    const QString envelope = messageEnvelope(7, QStringLiteral("Docs pass"), QStringLiteral("Done."));
    check(envelope.startsWith(QString::fromUtf8("Message from ForkMesh agent #7 “Docs pass” "
                                                "(another agent, not the user):")),
          "envelope names the sender");
    check(envelope.contains(QLatin1String("\n\nDone.\n\n")), "envelope carries the text");
    check(envelope.contains(QLatin1String("agent_id 7")), "envelope says how to reply");
    check(messageLead(7, QString()).contains(QLatin1String("#7 (another agent")),
          "lead without a title");
}

static void testPermissions()
{
    check(permissionRank(QStringLiteral("Plan"), 9) == 0, "plan rank");
    check(permissionRank(QStringLiteral("Ask"), 9) == 1, "ask rank");
    check(permissionRank(QStringLiteral("Ask before edits"), 9) == 1, "old ask label");
    check(permissionRank(QStringLiteral("Edit"), 9) == 2, "edit rank");
    check(permissionRank(QStringLiteral("Auto mode"), 9) == 3, "old auto label");
    check(permissionRank(QStringLiteral("something"), 9) == 9, "unknown rank");
    check(messageAllowed(QStringLiteral("Auto"), QStringLiteral("Auto")), "auto to auto");
    check(messageAllowed(QStringLiteral("Edit"), QStringLiteral("Plan")), "edit to plan");
    check(messageAllowed(QStringLiteral("Ask"), QStringLiteral("Ask")), "same mode");
    check(!messageAllowed(QStringLiteral("Plan"), QStringLiteral("Auto")), "plan to auto refused");
    check(!messageAllowed(QStringLiteral("Ask"), QStringLiteral("Edit")), "ask to edit refused");
    check(!messageAllowed(QStringLiteral("Edit"), QString()), "unknown target counts as auto");
    check(messageAllowed(QStringLiteral("Auto"), QString()), "auto may message an unknown mode");
    check(!messageAllowed(QString(), QStringLiteral("Ask")), "unknown sender counts as plan");
}

static void testNotify()
{
    NotifyRequest request;
    QString error;
    check(parseNotifyRequest(QJsonObject{{QStringLiteral("title"), QStringLiteral(" Build  done ")},
                                         {QStringLiteral("body"), QStringLiteral(" all green ")}},
                             &request, &error) &&
              request.title == QLatin1String("Build done") &&
              request.body == QLatin1String("all green"),
          "notify parses");
    check(!parseNotifyRequest(QJsonObject{{QStringLiteral("body"), QStringLiteral("x")}}, &request,
                              &error) &&
              error == QLatin1String("bad_title"),
          "notify needs a title");
    check(!parseNotifyRequest(QJsonObject{{QStringLiteral("title"), QStringLiteral("x")},
                                          {QStringLiteral("body"), QString(1001, QLatin1Char('b'))}},
                              &request, &error) &&
              error == QLatin1String("bad_body"),
          "notify body limit");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testSuggest();
    testList();
    testMessage();
    testPermissions();
    testNotify();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "agent hub tests passed\n";
    return 0;
}
