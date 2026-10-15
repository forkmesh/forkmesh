#include "DesktopPingForward.h"

#include <QJsonArray>
#include <QJsonObject>

#include <iostream>

using namespace forkmesh::pingforward;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static Source agentDone()
{
    Source source;
    source.localId = 7;
    source.timestampMs = 1760000000000;
    source.node = QStringLiteral("Jett-Laptop");
    source.kind = QStringLiteral("agent-done");
    source.linkKind = QStringLiteral("agent");
    source.linkNumber = 12;
    source.repo = QStringLiteral("jett/forkmesh");
    source.title = QStringLiteral("Agent #12 is done");
    source.body = QStringLiteral("Fixed the flaky test.");
    return source;
}

static void checkWireShape()
{
    const Ping ping = pingFor(agentDone());
    check(valid(ping), "an agent-done ping is valid");
    check(ping.pingId == QLatin1String("1760000000000-7"), "the ping id is time and row");
    check(ping.node == QLatin1String("jett-laptop"), "the node is lowercased");
    check(ping.category == QLatin1String("agent-finished"), "agent-done is agent-finished");
    check(ping.sessionId == QLatin1String("12"), "an agent link carries its session");
    check(ping.repo == QLatin1String("jett/forkmesh"), "the repository travels");
    check(canonical(QStringLiteral("jett"), ping, QStringLiteral("123")) ==
              QStringLiteral("forkmesh-account-desktop-ping-v1\njett\n1760000000000-7\n"
                             "jett-laptop\nagent-done\nagent-finished\n12\n"
                             "jett/forkmesh\n0\nAgent #12 is done\n"
                             "Fixed the flaky test.\n123"),
          "the canonical matches desktop_ping_canonical in app/src/desktop_pings.py");
    const QJsonObject json = body(ping);
    check(json.value(QStringLiteral("warning")).isBool(), "warning is a JSON bool");
    check(json.value(QStringLiteral("sessionId")).toString() == QLatin1String("12"),
          "the body carries the session");
    check(path() == QLatin1String("/api/notifications/desktop"), "the route");
}

static void checkCategories()
{
    check(categoryFor(QStringLiteral("desktop"), QStringLiteral("agent"), true) ==
              QLatin1String("agent-failed"),
          "a failing agent is agent-failed");
    check(categoryFor(QStringLiteral("desktop"), QStringLiteral("agent"), false) ==
              QLatin1String("agent-finished"),
          "a waiting agent offers Continue");
    check(categoryFor(QStringLiteral("action"), QStringLiteral("repo"), true).isEmpty(),
          "other pings are plain pings");
    Source issue = agentDone();
    issue.kind = QStringLiteral("issue");
    issue.linkKind = QStringLiteral("issue");
    check(pingFor(issue).sessionId.isEmpty(), "an issue number is not a session");
}

static void checkKinds()
{
    check(forwardsKind(QStringLiteral("agent-done")), "agent-done is forwarded");
    check(forwardsKind(QStringLiteral("desktop")), "desktop pings are forwarded");
    check(forwardsKind(QStringLiteral("action")), "action pings are forwarded");
    check(!forwardsKind(QStringLiteral("chat")), "chat already reaches the phone");
    check(!forwardsKind(QStringLiteral("search")), "searches stay on the desktop");
    check(!forwardsKind(QStringLiteral("error")), "logged errors stay on the desktop");
}

static void checkText()
{
    check(fitText(QStringLiteral("a\nb\tc"), 160, true) == QLatin1String("a b c"),
          "a title is one line");
    check(fitText(QStringLiteral(" x \n\n y "), 500, false) == QLatin1String("x\ny"),
          "a body keeps its lines");
    const QString longText(600, QLatin1Char('x'));
    check(fitText(longText, kBodyMax, false).size() == kBodyMax, "a long body is cut to fit");
    const QString emoji = QString(158, QLatin1Char('x')) + QString::fromUtf8("\xF0\x9F\xA4\x96xx");
    const QString cut = fitText(emoji, kTitleMax, true);
    check(cut.size() <= kTitleMax && !cut.at(cut.size() - 2).isHighSurrogate(),
          "a cut never splits a surrogate pair");
    Source empty = agentDone();
    empty.title.clear();
    empty.body = QStringLiteral("   ");
    check(!valid(pingFor(empty)), "an empty ping is refused");
    Source odd = agentDone();
    odd.repo = QStringLiteral("not a repo");
    odd.node = QStringLiteral("Jett's Mac");
    const Ping oddPing = pingFor(odd);
    check(oddPing.repo.isEmpty() && oddPing.node == QLatin1String("jett-s-mac") &&
              valid(oddPing),
          "an unusable repo is dropped and the node is made a token");
}

static void checkPhones()
{
    const qint64 now = 1760000000000;
    check(!phoneLinked({}, QStringLiteral("jett"), now), "no phone, nothing forwarded");
    check(phoneLinked({{QStringLiteral("jett"), QStringLiteral("ios"), now - 1000, false}},
                      QStringLiteral("Jett"), now),
          "a phone seen recently links");
    check(phoneLinked({{QString(), QStringLiteral("android"), 0, true}},
                      QStringLiteral("jett"), now),
          "a connected phone links");
    check(!phoneLinked({{QStringLiteral("jett"), QStringLiteral("ios"),
                         now - kPhoneRecentMs - 1, false}},
                       QStringLiteral("jett"), now),
          "a phone gone for a month does not");
    check(!phoneLinked({{QStringLiteral("bob"), QStringLiteral("ios"), now, true}},
                       QStringLiteral("jett"), now),
          "another account's phone does not");
    check(!phoneLinked({{QStringLiteral("jett"), QStringLiteral("web"), now, true}},
                       QStringLiteral("jett"), now),
          "a browser tab does not");
}

static void checkGate()
{
    const qint64 now = 1760000000000;
    Gate gate;
    Source flash = agentDone();
    flash.kind = QStringLiteral("desktop");
    flash.title = QStringLiteral("Bob filed a new issue on jett/forkmesh");
    flash.body = QStringLiteral("Crash on start");
    flash.linkKind.clear();
    Source filed = flash;
    filed.localId = 8;
    filed.title = QStringLiteral("New issue");
    filed.body = QStringLiteral("Bob filed a new issue on jett/forkmesh: Crash on start");
    check(gate.admit(pingFor(flash), now), "the first ping goes");
    check(!gate.admit(pingFor(filed), now + 100), "the same event filed twice goes once");
    Source other = flash;
    other.title = QStringLiteral("Workflow build failed");
    other.body = QStringLiteral("Run #4 on main");
    check(gate.admit(pingFor(other), now + 200), "a different ping goes");
    check(gate.admit(pingFor(filed), now + kDuplicateWindowMs + 1000),
          "the same text later is news again");

    Gate sessions;
    Source done = agentDone();
    Source osCard = agentDone();
    osCard.kind = QStringLiteral("desktop");
    osCard.title = QStringLiteral("ForkMesh - Agent #12 is done!");
    osCard.body = QStringLiteral("jett/forkmesh - 3m\nFixed the...");
    check(sessions.admit(pingFor(done), now), "agent done goes");
    check(!sessions.admit(pingFor(osCard), now + 50), "its OS card does not go again");

    Gate burst;
    int admitted = 0;
    for (int i = 0; i < kBurstLimit + 5; ++i) {
        Source s = other;
        s.localId = i;
        s.title = QStringLiteral("Distinct ping number %1 about something").arg(i * 7919);
        s.body = QString::number(i);
        if (burst.admit(pingFor(s), now + i))
            ++admitted;
    }
    check(admitted == kBurstLimit, "a storm is capped");
}

static void checkEcho()
{
    const QJsonObject own{
        {QStringLiteral("kind"), QStringLiteral("desktop_ping")},
        {QStringLiteral("meta"), QJsonObject{{QStringLiteral("node"),
                                              QStringLiteral("jett-laptop")}}}};
    check(isOwnEcho(own, QStringLiteral("Jett-Laptop")), "this desktop's ping is an echo");
    check(!isOwnEcho(own, QStringLiteral("jett-tower")), "another desktop's ping is not");
    QJsonObject other = own;
    other.insert(QStringLiteral("kind"), QStringLiteral("mention"));
    check(!isOwnEcho(other, QStringLiteral("jett-laptop")), "other kinds are not");
}

static void checkCelebration()
{
    const qint64 now = 1800000000000LL;
    const QJsonObject fresh{
        {QStringLiteral("id"), QStringLiteral("abc")},
        {QStringLiteral("ts"), double(now - 30000)},
        {QStringLiteral("readAt"), 0},
        {QStringLiteral("meta"), QJsonObject{{QStringLiteral("celebrate"),
                                              QStringLiteral("crm-email-tracked")}}}};
    check(isFreshCelebration(fresh, now), "a new tracked email parties");
    QJsonObject read = fresh;
    read.insert(QStringLiteral("readAt"), double(now));
    check(!isFreshCelebration(read, now), "a read ping does not");
    QJsonObject stale = fresh;
    stale.insert(QStringLiteral("ts"), double(now - kCelebrationFreshMs - 1));
    check(!isFreshCelebration(stale, now), "an old ping does not");
    QJsonObject plain = fresh;
    plain.insert(QStringLiteral("meta"), QJsonObject());
    check(!isFreshCelebration(plain, now), "an ordinary ping does not");
    QJsonObject nameless = fresh;
    nameless.remove(QStringLiteral("id"));
    check(!isFreshCelebration(nameless, now), "a ping without an id does not");
}

static void checkOnline()
{
    const qint64 now = 1760000000000;
    check(cameOnline(0, now), "the first beat after launch is coming online");
    check(!cameOnline(now - 60000, now), "a steady beat is not");
    check(!cameOnline(now - kOnlineGapMs + 1, now), "a short gap is not");
    check(cameOnline(now - kOnlineGapMs, now), "a long gap is");

    check(offlineSpan(0) == QLatin1String("1 min"), "under a minute reads as 1 min");
    check(offlineSpan(12 * 60000) == QLatin1String("12 min"), "minutes");
    check(offlineSpan(120 * 60000) == QLatin1String("2 h"), "whole hours");
    check(offlineSpan(125 * 60000) == QLatin1String("2 h 5 min"), "hours and minutes");
    check(offlineSpan(26LL * 3600000) == QLatin1String("1 d 2 h"), "days and hours");

    const Ping started = pingFor(onlineSource(QStringLiteral("Jett-Laptop"), 0, now));
    check(valid(started), "a started ping is valid");
    check(forwardsKind(onlineKind()), "the online kind is forwarded");
    check(started.kind == QLatin1String("desktop-online"), "the online kind rides the wire");
    check(started.title == QLatin1String("Jett-Laptop is online"), "the title names the desktop");
    check(started.body.contains(QLatin1String("started")), "a launch says it started");
    check(started.category.isEmpty() && started.sessionId.isEmpty() && !started.warning,
          "an online ping opens no agent");
    check(started.pingId == QLatin1String("1760000000000-0"), "the online ping id is its time");

    const Ping back =
        pingFor(onlineSource(QStringLiteral("jett-laptop"), now - 3 * 3600000, now));
    check(back.body == QLatin1String("Back online after 3 h offline."),
          "a reconnect says how long it was offline");
    check(pingFor(onlineSource(QString(), 0, now)).title ==
              QLatin1String("Your desktop is online"),
          "a nameless desktop still reads");
    check(coreOnlineFeature() == QLatin1String("online-ping"),
          "the window defers to a core that advertises the online ping");
}

static void checkDelete()
{
    const QStringList ids{QStringLiteral("1760000000000-7"), QStringLiteral("1760000000500-8")};
    check(deleteCanonical(QStringLiteral("jett"), QStringLiteral("jett-laptop"), ids,
                          QStringLiteral("123")) ==
              QStringLiteral("forkmesh-account-desktop-ping-delete-v1\njett\njett-laptop\n"
                             "1760000000000-7\n1760000000500-8\n123"),
          "the delete canonical matches desktop_ping_delete_canonical");
    const QJsonObject json = deleteBody(QStringLiteral("jett-laptop"), ids);
    check(json.value(QStringLiteral("node")).toString() == QLatin1String("jett-laptop"),
          "the delete names the node");
    check(json.value(QStringLiteral("pingIds")).toArray().size() == 2,
          "the delete lists every ping id");
    check(kDeleteBatch == 50, "a delete batch matches DESKTOP_PING_DELETE_MAX");
}

int main()
{
    checkWireShape();
    checkCategories();
    checkKinds();
    checkText();
    checkPhones();
    checkGate();
    checkEcho();
    checkCelebration();
    checkOnline();
    checkDelete();
    if (failures == 0)
        std::cout << "desktop ping forward tests passed\n";
    return failures == 0 ? 0 : 1;
}
