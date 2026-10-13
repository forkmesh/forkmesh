#include "TeammateMood.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QSet>

#include <iostream>

using namespace forkmesh::teammates;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}

MoodSignals idleAgent()
{
    MoodSignals s;
    s.nowMs = 1'800'000'000'000LL;
    s.lastRunAtMs = s.nowMs - 3600LL * 1000;
    s.createdAtMs = s.nowMs - 90LL * 24 * 3600 * 1000;
    s.updatedAtMs = s.createdAtMs;
    s.scheduleMode = kScheduleDaily;
    s.scheduleActive = true;
    s.staleness = 0.1;
    s.hourOfDay = 14;
    s.weekday = 3;
    s.teamSize = 5;
    s.teamWorking = 1;
    s.permissions = 3;
    s.hasDescription = true;
    s.hasInstructions = true;
    s.sessionStatus = QStringLiteral("success");
    s.recentRuns = 4;
    s.recentSuccesses = 4;
    s.successStreak = 1;
    s.runsToday = 1;
    return s;
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);

    check(moodFactorCount() == 100, "the mood weighs exactly 100 signals");
    const QStringList names = moodFactorNames();
    check(QSet<QString>(names.cbegin(), names.cend()).size() == names.size(),
          "every mood signal has its own name");

    QSet<QString> keys;
    QSet<QString> emojis;
    for (int kind = 0; kind <= static_cast<int>(MoodKind::Sleepy); ++kind) {
        const Mood mood = moodFor(static_cast<MoodKind>(kind));
        keys.insert(mood.key);
        emojis.insert(mood.emoji);
        check(!mood.label.isEmpty() && !mood.emoji.isEmpty(),
              "every mood has a label and an emoji");
    }
    check(keys.size() == static_cast<int>(MoodKind::Sleepy) + 1 &&
              emojis.size() == keys.size(),
          "every mood has its own key and emoji");

    const Mood happy = assessMood(idleAgent());
    check(happy.kind == MoodKind::Happy && !happy.wantsAttention &&
              happy.reasons.contains(QStringLiteral("succeeded last time")),
          "an agent whose last run passed recently is happy and says why");
    check(happy.reasons.size() <= 3, "a mood names at most three reasons");

    const QJsonObject wire = moodJson(happy);
    check(wire.value(QStringLiteral("key")).toString() == happy.key &&
              wire.value(QStringLiteral("emoji")).toString() == happy.emoji &&
              wire.value(QStringLiteral("motion")).toString() ==
                  moodMotionName(happy.motion) &&
              !wire.value(QStringLiteral("attention")).toBool() &&
              wire.value(QStringLiteral("reasons")).toArray().size() ==
                  happy.reasons.size(),
          "the phone's mood object carries the key, emoji, motion and reasons");

    MoodSignals waiting = idleAgent();
    waiting.inFlight = true;
    waiting.sessionStatus = QStringLiteral("waiting");
    const Mood needsYou = assessMood(waiting);
    check(needsYou.kind == MoodKind::NeedsYou && needsYou.wantsAttention &&
              needsYou.motion == MoodMotion::Wave,
          "an agent waiting for a reply waves for attention");

    MoodSignals done = idleAgent();
    done.doneUnseen = true;
    check(assessMood(done).kind == MoodKind::Celebrating &&
              assessMood(done).motion == MoodMotion::Jump,
          "an unseen finished run celebrates");

    MoodSignals working = idleAgent();
    working.inFlight = true;
    working.sessionStatus = QStringLiteral("running");
    working.runningMs = 5 * 60 * 1000;
    working.numTurns = 8;
    working.teamWorking = 2;
    check(assessMood(working).kind == MoodKind::Focused,
          "a working agent is focused");

    MoodSignals failed = idleAgent();
    failed.sessionStatus = QStringLiteral("failed");
    failed.failureStreak = 2;
    failed.successStreak = 0;
    failed.recentFailures = 2;
    failed.lastError = QStringLiteral("tests failed");
    const Mood frustrated = assessMood(failed);
    check(frustrated.kind == MoodKind::Frustrated && frustrated.wantsAttention &&
              frustrated.motion == MoodMotion::Shake,
          "repeated failures frustrate an agent, which shakes for attention");

    MoodSignals limited = failed;
    limited.lastError = QStringLiteral("Claude AI usage limit reached|1900000000");
    check(assessMood(limited).kind == MoodKind::Stuck,
          "a usage limit leaves an agent stuck rather than merely frustrated");

    MoodSignals paused = idleAgent();
    paused.enabled = false;
    check(assessMood(paused).kind == MoodKind::Sleepy &&
              assessMood(paused).motion == MoodMotion::Doze,
          "a paused agent dozes");

    MoodSignals overdue = idleAgent();
    overdue.sessionStatus.clear();
    overdue.windowState = RunWindowState::Overdue;
    overdue.staleness = 1.0;
    overdue.lastRunAtMs = overdue.nowMs - 3LL * 24 * 3600 * 1000;
    overdue.runsToday = 0;
    const Mood missing = assessMood(overdue);
    check(missing.kind == MoodKind::MissingYou && missing.motion == MoodMotion::Wave,
          "an agent past its run reminder waves to be run");

    MoodSignals fresh = idleAgent();
    fresh.lastRunAtMs = 0;
    fresh.sessionStatus.clear();
    fresh.recentRuns = 0;
    fresh.recentSuccesses = 0;
    fresh.successStreak = 0;
    fresh.runsToday = 0;
    fresh.createdAtMs = fresh.nowMs - 3600LL * 1000;
    check(assessMood(fresh).kind == MoodKind::Curious,
          "a brand-new agent that never ran is curious");

    MoodSignals bored = idleAgent();
    bored.scheduleMode = kScheduleOnDemand;
    bored.scheduleActive = false;
    bored.sessionStatus.clear();
    bored.lastRunAtMs = bored.nowMs - 40LL * 24 * 3600 * 1000;
    bored.staleness = 1.0;
    bored.runsToday = 0;
    bored.teamWorking = 0;
    check(assessMood(bored).kind == MoodKind::Bored,
          "an on-demand agent nobody ran for a month is bored");

    for (const MoodSignals &input : {idleAgent(), waiting, done, failed, paused}) {
        const Mood mood = assessMood(input);
        check(mood.intensity >= 0.25 && mood.intensity <= 1.0,
              "mood intensity stays between a quarter and full");
    }

    if (failures == 0)
        std::cout << "teammate mood tests passed\n";
    return failures == 0 ? 0 : 1;
}
