#include "StallRepairRouting.h"

#include <QCoreApplication>

#include <iostream>

using namespace forkmesh::stallrepair;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

Candidate session(int id, bool active, const QString &title,
                  qint64 createdAtMs = 1)
{
    Candidate candidate;
    candidate.id = id;
    candidate.active = active;
    candidate.sameRepository = true;
    candidate.title = title;
    candidate.createdAtMs = createdAtMs;
    return candidate;
}

}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);

    check(looksLikeStallWork(QStringLiteral("Quality · UI stall repair"),
                             QString()),
          "a special agent's stall run is stall work");
    check(looksLikeStallWork(QString(),
                             QStringLiteral("Please fix these UI stalls. ...")),
          "an automatic stall prompt is stall work");
    check(looksLikeStallWork(QString(),
                             QStringLiteral("ForkMesh's GUI thread stalled 3 time(s)")),
          "a manual stall prompt is stall work");
    check(!looksLikeStallWork(QStringLiteral("Fix the login page"),
                              QStringLiteral("Make the button blue")),
          "unrelated work is not stall work");

    check(pickSession({}, 0) == 0, "no sessions, nothing to add to");

    const Candidate running =
        session(7, true, QStringLiteral("Quality · UI stall repair"), 10);
    check(pickSession({running}, 0) == 7,
          "a running stall session is found without a remembered id");

    Candidate finished = session(9, false, QStringLiteral("anything"), 20);
    finished.succeeded = true;
    check(pickSession({running, finished}, 9) == 9,
          "the remembered session keeps taking stalls after a successful turn");

    Candidate merged = finished;
    merged.merged = true;
    check(pickSession({running, merged}, 9) == 7,
          "a merged remembered session falls back to the running stall session");
    check(pickSession({merged}, 9) == 0,
          "a merged session never takes more stalls");

    Candidate stopped = session(11, false, QStringLiteral("anything"));
    check(pickSession({stopped}, 11) == 0,
          "a stopped or failed session is not resumed for stalls");

    Candidate foreign = running;
    foreign.foreign = true;
    check(pickSession({foreign}, 7) == 0,
          "an external or cloud session is never steered with stalls");

    Candidate otherRepo = running;
    otherRepo.sameRepository = false;
    check(pickSession({otherRepo}, 7) == 0,
          "a session in another repository does not take stalls");

    const Candidate unrelated =
        session(12, true, QStringLiteral("Fix the login page"), 30);
    check(pickSession({unrelated}, 0) == 0,
          "a running session on other work is not picked by scanning");
    check(pickSession({unrelated}, 12) == 12,
          "a remembered running session is used even with a custom title");

    const Candidate newer =
        session(13, true, QStringLiteral("Quality · UI stall repair"), 40);
    check(pickSession({running, newer}, 0) == 13,
          "the newest running stall session wins");

    check(followUpDelayMs(1000, 0) == kFollowUpSettleMs,
          "the first follow-up waits for the burst to settle");
    check(followUpDelayMs(kFollowUpMinIntervalMs, 1) ==
              qMax(kFollowUpSettleMs, qint64(1)),
          "a follow-up right after the interval only settles");
    check(followUpDelayMs(2 * 60 * 1000, 60 * 1000) ==
              kFollowUpMinIntervalMs - 60 * 1000,
          "follow-ups are spaced by the minimum interval");

    const QString prompt = followUpPrompt(
        {QStringLiteral("[10:00:00] old"), QStringLiteral("[10:00:05] new")},
        52, QStringLiteral("Stall log: /x/stalls.log\n"));
    check(prompt.contains(QStringLiteral("2 new report(s), 52 this session")),
          "the follow-up counts new and total stalls");
    check(prompt.indexOf(QStringLiteral("[10:00:05] new")) <
              prompt.indexOf(QStringLiteral("[10:00:00] old")),
          "reports are listed newest first");
    check(prompt.contains(QStringLiteral("Stall log: /x/stalls.log")),
          "the follow-up names the log locations");

    QStringList many;
    for (int i = 0; i < 200; ++i)
        many.append(QStringLiteral("[t] report %1 ").arg(i) + QString(400, QLatin1Char('x')));
    const QString bounded = followUpPrompt(many, 200, QString(), 4000);
    check(bounded.size() <= 4000, "the follow-up stays within its bound");
    check(bounded.contains(QStringLiteral("report 199")),
          "the newest report survives the bound");
    check(bounded.contains(QStringLiteral("older reports omitted")),
          "a truncated follow-up says older reports were omitted");

    if (failures == 0)
        std::cout << "stall repair routing tests passed\n";
    return failures == 0 ? 0 : 1;
}
