#include "AgentIntake.h"
#include "AgentStore.h"
#include "ReleaseDraft.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>

#include <iostream>

using namespace forkmesh::agentintake;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static QString parseError(const QJsonObject &params)
{
    CreateRequest request;
    QString error;
    if (parseCreateRequest(params, &request, &error))
        return QString();
    return error;
}

static QJsonObject roundTrip(const QString &path, const QByteArray &line)
{
    QLocalSocket socket;
    socket.connectToServer(path);
    if (!socket.waitForConnected(2000))
        return QJsonObject{{QStringLiteral("connect"), false}};
    socket.write(line);
    socket.flush();
    QByteArray reply;
    QElapsedTimer timer;
    timer.start();
    while (!reply.contains('\n') && timer.elapsed() < 5000) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        socket.waitForReadyRead(20);
        reply += socket.readAll();
    }
    return QJsonDocument::fromJson(reply).object();
}

static void testParse()
{
    const QJsonObject minimal{{QStringLiteral("repository"), QStringLiteral("forkmesh/forkmesh")},
                              {QStringLiteral("prompt"), QStringLiteral("Fix the login redirect")}};
    CreateRequest request;
    QString error;
    check(parseCreateRequest(minimal, &request, &error), "a minimal request parses");
    check(request.owner == QLatin1String("forkmesh") && request.name == QLatin1String("forkmesh"),
          "repository splits into owner and name");
    check(!request.paused && !request.hasPriority && request.priority == 50,
          "paused defaults to false and priority to the midpoint");

    QJsonObject full = minimal;
    full.insert(QStringLiteral("title"), QStringLiteral("  Login   redirect "));
    full.insert(QStringLiteral("branch"), QStringLiteral("643-continues-forkmesh-cloud-agent"));
    full.insert(QStringLiteral("worktree"), QStringLiteral("/home/u/src/.worktrees/agent-643"));
    full.insert(QStringLiteral("paused"), true);
    full.insert(QStringLiteral("priority"), 7);
    full.insert(QStringLiteral("provider"), QStringLiteral("claude-code"));
    full.insert(QStringLiteral("model"), QStringLiteral("claude-opus-4-1[1m]"));
    check(parseCreateRequest(full, &request, &error), "a full request parses");
    check(request.paused && request.hasPriority && request.priority == 7,
          "paused and priority are carried");
    check(request.title == QLatin1String("Login redirect"), "the title is simplified");
    check(request.model == QLatin1String("claude-opus-4-1[1m]"), "a bracketed model id is kept");
    check(request.task.isEmpty() && !request.handoff, "no task is handed over by default");

    QJsonObject handed = minimal;
    handed.insert(QStringLiteral("task"), QStringLiteral(" 0123456789ABCDEF0123456789abcdef "));
    handed.insert(QStringLiteral("handoff"), true);
    check(parseCreateRequest(handed, &request, &error), "a task handoff parses");
    check(request.task == QLatin1String("0123456789abcdef0123456789abcdef"),
          "the task id is trimmed and lowered");
    check(request.handoff, "handoff is carried");

    auto with = [&minimal](const QString &key, const QJsonValue &value) {
        QJsonObject params = minimal;
        params.insert(key, value);
        return params;
    };
    check(parseError(with(QStringLiteral("repository"), QStringLiteral("forkmesh"))) ==
              QLatin1String("bad_repository"),
          "a repository without owner is refused");
    check(parseError(with(QStringLiteral("repository"), QStringLiteral("a/b/c"))) ==
              QLatin1String("bad_repository"),
          "a nested repository path is refused");
    check(parseError(with(QStringLiteral("repository"), QStringLiteral("../etc"))) ==
              QLatin1String("bad_repository"),
          "a traversal repository is refused");
    check(parseError(with(QStringLiteral("task"), QStringLiteral("42"))) ==
              QLatin1String("bad_task_id"),
          "a short task id is refused");
    check(parseError(with(QStringLiteral("task"), QString(32, QLatin1Char('g')))) ==
              QLatin1String("bad_task_id"),
          "a non-hex task id is refused");
    check(parseError(with(QStringLiteral("task"), 7)) == QLatin1String("bad_request"),
          "a numeric task id is refused");
    check(parseError(with(QStringLiteral("handoff"), QStringLiteral("yes"))) ==
              QLatin1String("bad_request"),
          "a non-boolean handoff is refused");
    check(parseError(with(QStringLiteral("prompt"), QStringLiteral("   "))) ==
              QLatin1String("bad_task"),
          "an empty prompt is refused");
    check(parseError(with(QStringLiteral("prompt"), QString(kMaxPromptChars, QLatin1Char('x')))).isEmpty(),
          "a prompt at the limit is accepted");
    check(parseError(with(QStringLiteral("prompt"), QString(kMaxPromptChars + 1, QLatin1Char('x')))) ==
              QLatin1String("bad_task"),
          "a prompt over the limit is refused");
    check(parseError(with(QStringLiteral("title"), QString(kMaxTitleChars + 1, QLatin1Char('t')))) ==
              QLatin1String("bad_title"),
          "a long title is refused");
    check(parseError(with(QStringLiteral("branch"), QStringLiteral("agent/../main"))) ==
              QLatin1String("bad_branch"),
          "a branch with .. is refused");
    check(parseError(with(QStringLiteral("branch"), QStringLiteral("-x"))) ==
              QLatin1String("bad_branch"),
          "a branch that looks like an option is refused");
    check(parseError(with(QStringLiteral("worktree"), QStringLiteral("relative/path"))) ==
              QLatin1String("bad_worktree"),
          "a relative worktree is refused");
    check(parseError(with(QStringLiteral("worktree"), QStringLiteral("/a/b/../c"))) ==
              QLatin1String("bad_worktree"),
          "an uncleaned worktree path is refused");
    check(parseError(with(QStringLiteral("provider"), QStringLiteral("Claude Code"))) ==
              QLatin1String("bad_provider"),
          "a provider with spaces is refused");
    check(parseError(with(QStringLiteral("model"), QStringLiteral("m; rm"))) ==
              QLatin1String("bad_model"),
          "a model with shell characters is refused");
    check(parseError(with(QStringLiteral("priority"), 0)) == QLatin1String("bad_priority"),
          "priority 0 is refused");
    check(parseError(with(QStringLiteral("priority"), 100)) == QLatin1String("bad_priority"),
          "priority 100 is refused");
    check(parseError(with(QStringLiteral("priority"), 2.5)) == QLatin1String("bad_priority"),
          "a fractional priority is refused");
    check(parseError(with(QStringLiteral("priority"), QStringLiteral("5"))) ==
              QLatin1String("bad_priority"),
          "a string priority is refused");
    check(parseError(with(QStringLiteral("paused"), QStringLiteral("yes"))) ==
              QLatin1String("bad_request"),
          "a non-boolean paused is refused");
    check(parseError(with(QStringLiteral("prompt"), 5)) == QLatin1String("bad_request"),
          "a non-string prompt is refused");
}

static void testTokens()
{
    const QString token = mintToken(42);
    check(tokenSessionId(token) == 42, "a token names its session");
    check(token.size() > 40, "a token carries 256 bits of secret");
    check(mintToken(42) != token, "tokens are random");
    check(tokenMatches(token, token), "a token matches itself");
    check(!tokenMatches(token, token + QLatin1Char('x')), "a longer token does not match");
    check(!tokenMatches(token, mintToken(42)), "another token does not match");
    check(!tokenMatches(QString(), QString()), "an empty token never matches");
    check(tokenSessionId(QStringLiteral("abc")) < 0, "a token without a session is refused");
    check(tokenSessionId(QStringLiteral("0.x")) < 0, "session 0 is refused");
    check(tokenSessionId(QStringLiteral("-3.x")) < 0, "a negative session is refused");
}

static void testAuthorization()
{
    QHash<int, QString> tokens;
    const QString seven = mintToken(7);
    const QString nine = mintToken(9);
    tokens.insert(7, seven);
    tokens.insert(9, nine);
    check(authorizedSession(tokens, seven) == 7, "a launched agent's token names its session");
    check(authorizedSession(tokens, nine) == 9, "each agent has its own token");
    check(authorizedSession(tokens, QString()) < 0, "a request without a token is refused");
    check(authorizedSession(tokens, mintToken(7)) < 0, "a forged token for a live session is refused");
    check(authorizedSession(tokens, QStringLiteral("8.") + seven.mid(2)) < 0,
          "a token moved to another session is refused");
    check(authorizedSession(tokens, mintToken(11)) < 0, "a token for an unknown session is refused");
    check(authorizedSession({}, seven) < 0, "no token is valid before any agent launched");

    const QJsonObject window = unauthorizedReply(QStringLiteral("windows.show"));
    check(window.value(QStringLiteral("ok")).toBool(true) == false &&
              window.value(QStringLiteral("error")).toString() == QLatin1String("unauthorized") &&
              window.value(QStringLiteral("message")).toString().contains(QLatin1String("windows")),
          "a window refusal names windows");
    check(unauthorizedReply(ReleaseCreateMethod)
              .value(QStringLiteral("message")).toString().contains(QLatin1String("releases")),
          "a release refusal names releases");
    check(unauthorizedReply(CreateMethod)
              .value(QStringLiteral("message")).toString().contains(QLatin1String("create agents")),
          "an agent refusal names agents");

    Server server;
    check(server.listen(), "the intake socket listens for the authorization test");
    int handled = 0;
    server.setAsyncHandler([&tokens, &handled](const Request &request, Server::Reply reply) {
        if (authorizedSession(tokens, request.token) <= 0) {
            reply(unauthorizedReply(request.method));
            return;
        }
        ++handled;
        reply(QJsonObject{{QStringLiteral("ok"), true}});
    });
    for (const QByteArray &method : {QByteArrayLiteral("windows.show"),
                                     QByteArrayLiteral("windows.read"),
                                     QByteArrayLiteral("windows.close"),
                                     QByteArrayLiteral("windows.browse"),
                                     QByteArrayLiteral("windows.capture")}) {
        const QJsonObject bare = roundTrip(
            server.socketPath(),
            QByteArrayLiteral("{\"method\":\"") + method + QByteArrayLiteral("\",\"params\":{}}\n"));
        check(bare.value(QStringLiteral("error")).toString() == QLatin1String("unauthorized"),
              "a raw window request without the token is refused");
        const QJsonObject forged = roundTrip(
            server.socketPath(),
            QByteArrayLiteral("{\"token\":\"7.forged\",\"method\":\"") + method +
                QByteArrayLiteral("\",\"params\":{}}\n"));
        check(forged.value(QStringLiteral("error")).toString() == QLatin1String("unauthorized"),
              "a raw window request with a wrong token is refused");
    }
    check(handled == 0, "no unauthorized window request reaches the windows");
    const QJsonObject good = roundTrip(
        server.socketPath(),
        QByteArrayLiteral("{\"token\":\"") + seven.toUtf8() +
            QByteArrayLiteral("\",\"method\":\"windows.show\",\"params\":{}}\n"));
    check(good.value(QStringLiteral("ok")).toBool() && handled == 1,
          "a window request with the agent's token is handled");
}

static void testRateLimiter()
{
    RateLimiter limiter(3, 1000);
    check(limiter.allow(0) && limiter.allow(10) && limiter.allow(20), "three fit in the window");
    check(!limiter.allow(30), "the fourth inside the window is refused");
    check(limiter.used(30) == 3, "a refusal is not counted");
    check(limiter.allow(1000), "the window slides");
    check(limiter.used(1000) == 3, "the oldest entry left the window");
}

static void testHeldSession()
{
    AgentSession held;
    held.id = 7;
    held.status = AgentStatus::Queued;
    held.held = true;
    held.heldBy = QStringLiteral("Agent #3");
    const QJsonObject json = held.toJson();
    check(json.value(QStringLiteral("held")).toBool(), "held is saved");
    const AgentSession loaded = AgentSession::fromJson(json);
    check(loaded.held && loaded.heldBy == QLatin1String("Agent #3"), "held survives a restart");
    check(loaded.awaitingStart(), "a held queued session awaits its start");

    AgentSession plain;
    plain.id = 8;
    plain.status = AgentStatus::Queued;
    const QJsonObject plainJson = plain.toJson();
    check(!plainJson.contains(QStringLiteral("held")) &&
              !plainJson.contains(QStringLiteral("heldBy")),
          "an ordinary session writes no held keys");
    check(!AgentSession::fromJson(plainJson).held, "an older record loads as not held");

    AgentSession finished = loaded;
    finished.status = AgentStatus::Stopped;
    check(!finished.awaitingStart(), "only a queued session can await its start");

    AgentSession first = plain;
    first.id = 1;
    first.createdAtMs = 1;
    AgentSession pausedFirst = loaded;
    pausedFirst.createdAtMs = 0;
    AgentSession later = plain;
    later.createdAtMs = 2;
    const QList<int> queue =
        AgentStore::queuedSessionIdsOldestFirst({first, pausedFirst, later});
    check(queue == QList<int>({1, 8}), "the startup queue leaves held sessions out");
}

static void testServer()
{
    Server server;
    QString error;
    check(server.listen(&error), "the intake socket listens");
    const QString path = server.socketPath();
    check(QFileInfo(path).permissions() & QFileDevice::ReadOwner, "the socket is the owner's");
    check(!(QFileInfo(path).permissions() & QFileDevice::ReadOther) &&
              !(QFileInfo(path).permissions() & QFileDevice::WriteOther),
          "the socket is closed to other users");

    Request seen;
    server.setHandler([&seen](const Request &request) {
        seen = request;
        return QJsonObject{{QStringLiteral("ok"), true}, {QStringLiteral("id"), 5}};
    });
    const QJsonObject reply = roundTrip(
        path, QByteArrayLiteral("{\"token\":\"3.abc\",\"method\":\"agents.create\","
                                "\"params\":{\"prompt\":\"p\"}}\n"));
    check(reply.value(QStringLiteral("ok")).toBool() &&
              reply.value(QStringLiteral("id")).toInt() == 5,
          "a request gets the handler's reply");
    check(seen.token == QLatin1String("3.abc") && seen.method == CreateMethod &&
              seen.params.value(QStringLiteral("prompt")).toString() == QLatin1String("p"),
          "the handler sees the token, method and params");

    const QJsonObject garbage = roundTrip(path, QByteArrayLiteral("not json\n"));
    check(garbage.value(QStringLiteral("error")).toString() == QLatin1String("bad_request"),
          "a non-JSON line is refused");

    QByteArray huge = QByteArrayLiteral("{\"token\":\"");
    huge += QByteArray(kMaxRequestBytes + 10, 'a');
    huge += QByteArrayLiteral("\"}\n");
    seen = Request();
    const QJsonObject tooLarge = roundTrip(path, huge);
    check(tooLarge.value(QStringLiteral("ok")).toBool() == false && seen.token.isEmpty(),
          "an oversized request never reaches the handler");
}

static QString releaseError(const QString &method, const QJsonObject &params,
                            ReleaseRequest *out = nullptr)
{
    ReleaseRequest request;
    QString error;
    if (parseReleaseRequest(method, params, &request, &error)) {
        if (out)
            *out = request;
        return QString();
    }
    return error;
}

static void testReleaseRequests()
{
    const QJsonObject repo{{QStringLiteral("repository"), QStringLiteral("forkmesh/forkmesh")}};
    ReleaseRequest request;
    check(releaseError(ReleaseCreateMethod, repo, &request).isEmpty() &&
              request.owner == QLatin1String("forkmesh") && request.tag.isEmpty() &&
              !request.hasRollout && request.count == 10,
          "a create with only a repository is valid and leaves every choice to the desktop");
    QJsonObject full = repo;
    full.insert(QStringLiteral("tag"), QStringLiteral(" v1.2.7 "));
    full.insert(QStringLiteral("target"), QStringLiteral("main"));
    full.insert(QStringLiteral("title"), QStringLiteral("  ForkMesh   v1.2.7 "));
    full.insert(QStringLiteral("notes"), QStringLiteral("\n* fix\n"));
    full.insert(QStringLiteral("rollout"), false);
    check(releaseError(ReleaseCreateMethod, full, &request).isEmpty() &&
              request.tag == QLatin1String("v1.2.7") &&
              request.target == QLatin1String("main") &&
              request.title == QLatin1String("ForkMesh v1.2.7") &&
              request.notes == QLatin1String("* fix") && request.hasRollout &&
              !request.rollout,
          "create fields are trimmed and an explicit rollout choice is kept");
    const auto with = [&repo](const QString &key, const QJsonValue &value) {
        QJsonObject params = repo;
        params.insert(key, value);
        return params;
    };
    check(releaseError(ReleaseCreateMethod, with(QStringLiteral("tag"),
                                                 QStringLiteral("../v1"))) ==
              QLatin1String("bad_tag") &&
              releaseError(ReleaseCreateMethod, with(QStringLiteral("tag"),
                                                     QStringLiteral("v1..2"))) ==
                  QLatin1String("bad_tag") &&
              releaseError(ReleaseCreateMethod, with(QStringLiteral("tag"),
                                                     QStringLiteral("v1.lock"))) ==
                  QLatin1String("bad_tag") &&
              releaseError(ReleaseCreateMethod, with(QStringLiteral("target"),
                                                     QStringLiteral("a/../main"))) ==
                  QLatin1String("bad_target") &&
              releaseError(ReleaseCreateMethod,
                           with(QStringLiteral("title"), QString(201, QLatin1Char('t')))) ==
                  QLatin1String("bad_title") &&
              releaseError(ReleaseCreateMethod,
                           with(QStringLiteral("notes"), QString(20001, QLatin1Char('n')))) ==
                  QLatin1String("bad_notes") &&
              releaseError(ReleaseCreateMethod,
                           with(QStringLiteral("rollout"), QStringLiteral("yes"))) ==
                  QLatin1String("bad_request") &&
              releaseError(ReleaseCreateMethod, with(QStringLiteral("tag"), 7)) ==
                  QLatin1String("bad_request"),
          "malformed release fields are refused with their own codes");
    check(releaseError(ReleaseCreateMethod,
                       QJsonObject{{QStringLiteral("repository"), QStringLiteral("x")}}) ==
              QLatin1String("bad_repository"),
          "a release request names owner/name");
    check(releaseError(ReleasePublishMethod, repo) == QLatin1String("bad_tag") &&
              releaseError(ReleasePublishMethod, with(QStringLiteral("tag"),
                                                      QStringLiteral("v1.2.7")))
                  .isEmpty(),
          "publishing needs the tag of an existing release");
    check(releaseError(ReleaseListMethod, with(QStringLiteral("count"), 0)) ==
              QLatin1String("bad_count") &&
              releaseError(ReleaseListMethod, with(QStringLiteral("count"), 51)) ==
                  QLatin1String("bad_count") &&
              releaseError(ReleaseListMethod, with(QStringLiteral("count"), 2.5)) ==
                  QLatin1String("bad_count") &&
              releaseError(ReleaseListMethod, with(QStringLiteral("count"), 5), &request)
                  .isEmpty() &&
              request.count == 5,
          "list count is a whole number from 1 to 50");
    check(releaseError(QStringLiteral("releases.delete"), repo) ==
              QLatin1String("unknown_method"),
          "no release method beyond list, create and publish is accepted");
}

static void testReleaseDraft()
{
    namespace rd = forkmesh::releasedraft;
    const rd::Latest latest = rd::parseLatest("v1.2.6\tForkMesh v1.2.6\n");
    check(latest.tag == QLatin1String("v1.2.6") &&
              latest.title == QLatin1String("ForkMesh v1.2.6"),
          "the newest tag line splits into tag and subject");
    check(rd::nextTag(QStringLiteral("v1.2.6")) == QLatin1String("v1.2.7") &&
              rd::nextTag(QStringLiteral("v1.2.9")) == QLatin1String("v1.2.10") &&
              rd::nextTag(QStringLiteral("release-41-rc")) == QLatin1String("release-42-rc") &&
              rd::nextTag(QStringLiteral("stable")).isEmpty(),
          "the next tag counts up the last number in the previous one");
    check(rd::nextTitle(QStringLiteral("v1.2.6"), QStringLiteral("ForkMesh v1.2.6"),
                        QStringLiteral("v1.2.7")) == QLatin1String("ForkMesh v1.2.7") &&
              rd::nextTitle(QStringLiteral("v1.2.6"), QString(), QStringLiteral("v1.2.7")) ==
                  QLatin1String("v1.2.7") &&
              rd::nextTitle(QStringLiteral("v1.2.6"), QStringLiteral("Spring"),
                            QStringLiteral("v1.2.7")) == QLatin1String("Spring"),
          "the next title follows the previous one");
    check(rd::deployableTag(QStringLiteral("v1.2.7")) &&
              rd::deployableTag(QStringLiteral("1.2.7")) &&
              !rd::deployableTag(QStringLiteral("v1.2")) &&
              !rd::deployableTag(QStringLiteral("v1.2.7-rc1")),
          "only plain version tags can roll out");
    check(rd::logArgs(QStringLiteral("v1.2.6"), QStringLiteral("main")).last() ==
              QLatin1String("v1.2.6..main") &&
              rd::logArgs(QString(), QStringLiteral("main")).contains(
                  QStringLiteral("--max-count=250")),
          "the notes log spans the previous tag to the target");
    const QString notes = rd::notesFromLog(
        "Fix login\x1f" "abc1234\x1f" "Ada\n\nAdd tags\x1f" "def5678\x1f\n",
        QStringLiteral("v1.2.6"), QStringLiteral("v1.2.7"), QStringLiteral("main"));
    check(notes == QLatin1String("## What's Changed\n* Fix login (`abc1234`) by Ada\n"
                                 "* Add tags (`def5678`)\n\n"
                                 "**Full Changelog**: v1.2.6...v1.2.7"),
          "generated notes list each commit and link the changelog");
    check(rd::notesFromLog(QByteArray(), QString(), QStringLiteral("v1.0.0"),
                           QStringLiteral("main")) ==
              QLatin1String("## What's Changed\n* No commits yet.\n"),
          "a first release with no commits says so");
    check(rd::tagMessage(QString(), QStringLiteral("notes"), QStringLiteral("v1")) ==
              QLatin1String("v1\n\nnotes") &&
              rd::tagMessage(QStringLiteral("T"), QString(), QStringLiteral("v1")) ==
                  QLatin1String("T"),
          "the tag message is the title, then the notes");
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testParse();
    testTokens();
    testAuthorization();
    testRateLimiter();
    testHeldSession();
    testServer();
    testReleaseRequests();
    testReleaseDraft();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "agent intake tests passed\n";
    return 0;
}
