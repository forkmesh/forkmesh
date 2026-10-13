// The Timers package's model: what a typed request means for a timer, a
// countdown or an alarm, what a chip reads, which entry announces itself, and
// what survives a restart. Everything is driven off an injected "now", so the
// interesting cases are the ones a wall clock could never reproduce on demand:
// a countdown that expired while the app was closed, a paused entry that must
// not drift, and a stored strip that has been edited by hand.

#include "TimersFeature.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QLocale>

#include <cstdio>

using forkmesh::timers::Timer;
using forkmesh::timers::TimerKind;
using forkmesh::timers::TimersFeature;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition) {
        std::printf("PASS: %s\n", message);
    } else {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

constexpr qint64 kSecond = 1000;
constexpr qint64 kMinute = 60 * kSecond;
constexpr qint64 kHour = 60 * kMinute;
constexpr qint64 kNow = 1750000000000LL; // a fixed instant, in local time

void testDurationParsing()
{
    check(TimersFeature::parseDurationMs(QStringLiteral("25m")) == 25 * kMinute,
          "\"25m\" is twenty-five minutes");
    check(TimersFeature::parseDurationMs(QStringLiteral("25")) == 25 * kMinute,
          "a bare number is minutes, the unit people say out loud");
    check(TimersFeature::parseDurationMs(QStringLiteral("90s")) == 90 * kSecond,
          "\"90s\" is ninety seconds");
    check(TimersFeature::parseDurationMs(QStringLiteral("1h30m")) ==
              kHour + 30 * kMinute,
          "\"1h30m\" adds its parts");
    check(TimersFeature::parseDurationMs(QStringLiteral("2 hours 5 min")) ==
              2 * kHour + 5 * kMinute,
          "spelled-out units mean the same as their letters");
    check(TimersFeature::parseDurationMs(QStringLiteral("1d")) == 24 * kHour,
          "a day is a length");
    check(TimersFeature::parseDurationMs(QStringLiteral("0m")) == -1 &&
              TimersFeature::parseDurationMs(QStringLiteral("0")) == -1,
          "a zero-length countdown is refused rather than fired instantly");
    check(TimersFeature::parseDurationMs(QStringLiteral("8d")) == -1,
          "a countdown longer than a week is refused");
    check(TimersFeature::parseDurationMs(QStringLiteral("standup")) == -1 &&
              TimersFeature::parseDurationMs(QString()) == -1 &&
              TimersFeature::parseDurationMs(QStringLiteral("m")) == -1,
          "text with no figure in it is not a length");
}

void testClockParsing()
{
    const QDateTime now = QDateTime::fromMSecsSinceEpoch(kNow);
    const auto targetOf = [&now](const QString &text) {
        const qint64 remaining = TimersFeature::parseClockTargetMs(text, kNow);
        return remaining < 0 ? QDateTime()
                             : now.addMSecs(remaining);
    };

    const QDateTime seventeen = targetOf(QStringLiteral("17:00"));
    check(seventeen.isValid() && seventeen.time().hour() == 17 &&
              seventeen.time().minute() == 0 && seventeen > now,
          "\"17:00\" is the next 5pm, not a length");
    const QDateTime fivePm = targetOf(QStringLiteral("5pm"));
    check(fivePm.isValid() && fivePm.time().hour() == 17 &&
              fivePm.time().minute() == 0,
          "\"5pm\" reaches the same hour as \"17:00\"");
    const QDateTime halfSix = targetOf(QStringLiteral("6:30 am"));
    check(halfSix.isValid() && halfSix.time().hour() == 6 &&
              halfSix.time().minute() == 30,
          "\"6:30 am\" keeps its minutes");
    check(TimersFeature::parseClockTargetMs(QStringLiteral("5"), kNow) == -1,
          "a plain \"5\" is five minutes, never five o'clock");
    check(TimersFeature::parseClockTargetMs(QStringLiteral("25:00"), kNow) ==
                  -1 &&
              TimersFeature::parseClockTargetMs(QStringLiteral("17:75"), kNow) ==
                  -1,
          "an hour or minute that does not exist is not a time");

    // A time that has already gone today is tomorrow's, so the countdown is
    // always positive and never fires the instant it is created.
    const QDateTime justPassed = now.addSecs(-60);
    const QString spec = QStringLiteral("%1:%2")
                             .arg(justPassed.time().hour(), 2, 10, QLatin1Char('0'))
                             .arg(justPassed.time().minute(), 2, 10,
                                  QLatin1Char('0'));
    const qint64 remaining = TimersFeature::parseClockTargetMs(spec, kNow);
    check(remaining > 23 * kHour && remaining <= 24 * kHour,
          "a time of day that has passed means tomorrow");
}

void testRequestParsing()
{
    Timer timer;
    QString error;
    check(TimersFeature::parseRequest(QStringLiteral("25m standup"), kNow,
                                      &timer, &error) &&
              timer.kind == TimerKind::Countdown &&
              timer.label == QStringLiteral("standup") &&
              timer.durationMs == 25 * kMinute &&
              timer.endsAtMs == kNow + 25 * kMinute,
          "\"25m standup\" is a 25 minute countdown called standup");
    check(TimersFeature::parseRequest(QStringLiteral("1h 30m deploy freeze"),
                                      kNow, &timer, &error) &&
              timer.label == QStringLiteral("deploy freeze") &&
              timer.durationMs == kHour + 30 * kMinute,
          "a multi-word length keeps the rest of the line as the label");
    check(TimersFeature::parseRequest(QStringLiteral("coffee in 4m"), kNow,
                                      &timer, &error) &&
              timer.label == QStringLiteral("coffee") &&
              timer.durationMs == 4 * kMinute,
          "the length may come last: \"coffee in 4m\"");
    check(TimersFeature::parseRequest(QStringLiteral("in 10m"), kNow, &timer,
                                      &error) &&
              timer.label == QStringLiteral("Countdown"),
          "a countdown with nothing said about it is still called something");
    check(TimersFeature::parseRequest(QStringLiteral("25m - standup"), kNow,
                                      &timer, &error) &&
              timer.label == QStringLiteral("standup"),
          "a separator between the length and the name is not part of the name");
    check(TimersFeature::parseRequest(QStringLiteral("at 17:00 demo"), kNow,
                                      &timer, &error) &&
              timer.label == QStringLiteral("demo") && timer.durationMs > 0,
          "\"at 17:00 demo\" counts down to a time of day");

    error.clear();
    check(!TimersFeature::parseRequest(QStringLiteral("do the thing"), kNow,
                                       &timer, &error) &&
              !error.isEmpty(),
          "a request with no length in it is refused with something to read");
    check(!TimersFeature::parseRequest(QString(), kNow, &timer, &error),
          "an empty request is refused");
}

void testTimerAndAlarmParsing()
{
    Timer timer;
    QString error;
    check(TimersFeature::parseRequest(QStringLiteral("deep work"), kNow, &timer,
                                      &error, TimerKind::Timer) &&
              timer.kind == TimerKind::Timer &&
              timer.label == QStringLiteral("deep work") &&
              timer.startedAtMs == kNow &&
              TimersFeature::elapsedMs(timer, kNow + 90 * kSecond) ==
                  90 * kSecond,
          "a timer's whole line is its name, and it counts up from now");
    check(TimersFeature::parseRequest(QString(), kNow, &timer, &error,
                                      TimerKind::Timer) &&
              timer.label == QStringLiteral("Timer"),
          "a timer needs no name at all");

    check(TimersFeature::parseRequest(QStringLiteral("7:30 standup"), kNow,
                                      &timer, &error, TimerKind::Alarm) &&
              timer.kind == TimerKind::Alarm &&
              timer.label == QStringLiteral("standup") &&
              QDateTime::fromMSecsSinceEpoch(timer.endsAtMs).time() ==
                  QTime(7, 30),
          "\"7:30 standup\" is an alarm that rings at half past seven");
    check(TimersFeature::parseRequest(QStringLiteral("demo at 5pm"), kNow,
                                      &timer, &error, TimerKind::Alarm) &&
              timer.label == QStringLiteral("demo") &&
              QDateTime::fromMSecsSinceEpoch(timer.endsAtMs).time() ==
                  QTime(17, 0),
          "an alarm's time may come last: \"demo at 5pm\"");
    check(TimersFeature::parseRequest(QStringLiteral("in 20m stretch"), kNow,
                                      &timer, &error, TimerKind::Alarm) &&
              timer.endsAtMs == kNow + 20 * kMinute,
          "an alarm can be set a length from now");
    error.clear();
    check(!TimersFeature::parseRequest(QStringLiteral("7"), kNow, &timer,
                                       &error, TimerKind::Alarm) &&
              error.contains(QStringLiteral("alarm")),
          "a bare number is not an alarm time, and the refusal says how to "
          "write one");
    check(TimersFeature::figureText(timer, kNow) ==
              QLocale::system().toString(
                  QDateTime::fromMSecsSinceEpoch(timer.endsAtMs).time(),
                  QLocale::ShortFormat),
          "an alarm's chip shows the time it rings, not what is left");
}

void testRemainingText()
{
    check(TimersFeature::remainingText(25 * kMinute) == QLatin1String("25:00"),
          "a countdown under an hour reads as minutes and seconds");
    check(TimersFeature::remainingText(kHour + 4 * kMinute + 32 * kSecond) ==
              QLatin1String("1:04:32"),
          "an hour or more reads as hours, minutes and seconds");
    check(TimersFeature::remainingText(0) == QLatin1String("0:00"),
          "zero reads as zero");
    check(TimersFeature::remainingText(-7 * kSecond) == QLatin1String("+0:07"),
          "a finished countdown counts up from the moment it fired");
    check(TimersFeature::remainingText(999) == QLatin1String("0:01") &&
              TimersFeature::remainingText(-1) == QLatin1String("+0:01"),
          "the last whole second still reads as a second, and zero is reached "
          "only at zero");

    Timer timer;
    TimersFeature::parseRequest(QString(), kNow, &timer, nullptr,
                                TimerKind::Timer);
    check(TimersFeature::figureText(timer, kNow + 999) == QLatin1String("0:00") &&
              TimersFeature::figureText(timer, kNow + kHour + 5 * kSecond) ==
                  QLatin1String("1:00:05"),
          "a timer reads like a stopwatch: a second counts once it has passed");
}

void testStripLifecycle()
{
    TimersFeature timers;
    QString error;
    check(timers.addFromText(QStringLiteral("25m standup"), kNow, &error) &&
              timers.addFromText(QStringLiteral("5m coffee"), kNow, &error) &&
              timers.count() == 2,
          "two countdowns can run at once");

    QList<Timer> ordered = timers.ordered(kNow);
    check(ordered.size() == 2 && ordered.first().label == QLatin1String("coffee"),
          "the strip reads soonest first");

    const QString coffee = ordered.first().id;
    check(timers.togglePause(coffee, kNow) &&
              timers.ordered(kNow).last().label == QLatin1String("coffee"),
          "a paused countdown sinks behind the ones still counting");
    // A paused countdown must not lose time while it is paused.
    check(TimersFeature::remainingMs(*timers.timer(coffee),
                                     kNow + 10 * kMinute) == 5 * kMinute,
          "a paused countdown keeps its remainder no matter how long it sits");
    check(timers.togglePause(coffee, kNow + 10 * kMinute) &&
              TimersFeature::remainingMs(*timers.timer(coffee),
                                         kNow + 10 * kMinute) == 5 * kMinute,
          "resuming re-hangs the remainder off the moment it resumed");
    check(timers.takeNewlyExpired(kNow + 12 * kMinute).isEmpty(),
          "a countdown that is still counting has nothing to announce");

    const QList<Timer> fired = timers.takeNewlyExpired(kNow + 16 * kMinute);
    check(fired.size() == 1 && fired.first().label == QLatin1String("coffee"),
          "the countdown that ran out announces itself");
    check(timers.takeNewlyExpired(kNow + 17 * kMinute).isEmpty(),
          "it announces itself exactly once, not on every repaint");
    check(timers.ordered(kNow + 16 * kMinute).first().label ==
              QLatin1String("coffee"),
          "a finished countdown sits at the head of the strip, where it is seen");

    check(timers.extend(coffee, 5 * kMinute, kNow + 16 * kMinute) &&
              TimersFeature::remainingMs(*timers.timer(coffee),
                                         kNow + 16 * kMinute) == 5 * kMinute,
          "adding five minutes to a finished countdown measures them from now");
    check(timers.takeNewlyExpired(kNow + 22 * kMinute).size() == 1,
          "an extended countdown announces itself again when it runs out again");

    check(timers.reset(coffee, kNow + 30 * kMinute) &&
              TimersFeature::remainingMs(*timers.timer(coffee),
                                         kNow + 30 * kMinute) == 5 * kMinute,
          "restarting a countdown goes back to the length it was started with");

    check(timers.runningCount(kNow + 30 * kMinute) == 1,
          "only the restarted countdown is still live; the 25 minute one has "
          "run out");
    check(timers.remove(coffee) && timers.count() == 1 &&
              !timers.timer(coffee),
          "a removed countdown is gone from the strip");

    for (int i = 0; i < TimersFeature::kMaximumTimers; ++i)
        timers.addFromText(QStringLiteral("10m t%1").arg(i), kNow, &error);
    error.clear();
    check(timers.count() == TimersFeature::kMaximumTimers &&
              !timers.addFromText(QStringLiteral("focus"), kNow, &error,
                                  TimerKind::Timer) &&
              !error.isEmpty(),
          "the strip stays a strip: the limit covers every kind and is refused "
          "with an explanation");
}

void testTimersAndAlarmsOnTheStrip()
{
    TimersFeature timers;
    QString error;
    timers.addFromText(QStringLiteral("focus"), kNow, &error, TimerKind::Timer);
    timers.addFromText(QStringLiteral("in 10m stretch"), kNow, &error,
                       TimerKind::Alarm);
    timers.addFromText(QStringLiteral("30m tea"), kNow, &error);
    const QString focus = timers.ordered(kNow).last().id;

    QList<Timer> ordered = timers.ordered(kNow + kMinute);
    check(ordered.size() == 3 && ordered.at(0).label == QLatin1String("stretch") &&
              ordered.at(1).label == QLatin1String("tea") &&
              ordered.at(2).label == QLatin1String("focus"),
          "alarms and countdowns read soonest first, and running timers come "
          "after them");

    check(timers.togglePause(focus, kNow + 2 * kMinute) &&
              TimersFeature::elapsedMs(*timers.timer(focus), kNow + kHour) ==
                  2 * kMinute &&
              timers.togglePause(focus, kNow + kHour) &&
              TimersFeature::elapsedMs(*timers.timer(focus),
                                       kNow + kHour + kMinute) == 3 * kMinute,
          "a paused timer holds its reading and carries on from it");
    check(timers.reset(focus, kNow + kHour) &&
              TimersFeature::elapsedMs(*timers.timer(focus), kNow + kHour) == 0,
          "resetting a timer takes it back to zero");
    check(!timers.extend(focus, kMinute, kNow),
          "a timer has no end to push back");

    const QString stretch = ordered.at(0).id;
    check(!timers.togglePause(stretch, kNow) && !timers.reset(stretch, kNow),
          "an alarm rings at a time of day, so it cannot be paused or "
          "restarted");

    const QList<Timer> fired = timers.takeNewlyExpired(kNow + 11 * kMinute);
    check(fired.size() == 1 && fired.first().kind == TimerKind::Alarm &&
              TimersFeature::isFinished(*timers.timer(stretch),
                                        kNow + 11 * kMinute),
          "an alarm announces itself when its time comes; a timer never does");
    check(timers.extend(stretch, 5 * kMinute, kNow + 11 * kMinute) &&
              timers.timer(stretch)->endsAtMs == kNow + 16 * kMinute &&
              timers.takeNewlyExpired(kNow + 16 * kMinute).size() == 1,
          "snoozing an alarm rings it again five minutes later");
    check(timers.runningCount(kNow + 20 * kMinute) == 2,
          "a running timer and a live countdown both keep the second hand "
          "going");
}

void testPersistence()
{
    TimersFeature timers;
    QString error;
    timers.addFromText(QStringLiteral("25m standup"), kNow, &error);
    timers.addFromText(QStringLiteral("5m coffee"), kNow, &error);
    timers.togglePause(timers.ordered(kNow).first().id, kNow);
    timers.addFromText(QStringLiteral("focus"), kNow, &error, TimerKind::Timer);
    timers.addFromText(QStringLiteral("in 2h review"), kNow, &error,
                       TimerKind::Alarm);
    const QByteArray stored = timers.serialize();

    TimersFeature restored;
    check(restored.load(stored) && restored.count() == 4,
          "the strip comes back after a restart");
    Timer coffee, standup, focus, review;
    for (const Timer &timer : restored.ordered(kNow)) {
        if (timer.label == QLatin1String("coffee"))
            coffee = timer;
        else if (timer.label == QLatin1String("standup"))
            standup = timer;
        else if (timer.label == QLatin1String("focus"))
            focus = timer;
        else if (timer.label == QLatin1String("review"))
            review = timer;
    }
    check(coffee.paused &&
              TimersFeature::remainingMs(coffee, kNow + kHour) == 5 * kMinute,
          "a paused countdown comes back paused, with its remainder intact");
    check(focus.kind == TimerKind::Timer &&
              TimersFeature::elapsedMs(focus, kNow + kHour) == kHour,
          "a timer comes back still counting from when it was started");
    check(review.kind == TimerKind::Alarm && review.endsAtMs == kNow + 2 * kHour,
          "an alarm comes back ringing at the same time");
    // Deadlines are absolute, so a countdown that ran out while the app was
    // closed comes back finished rather than starting over.
    check(TimersFeature::remainingMs(standup, kNow + kHour) < 0 &&
              restored.takeNewlyExpired(kNow + kHour).size() == 1,
          "a countdown that expired while the app was closed says so");

    TimersFeature damaged;
    check(!damaged.load(QByteArray("{\"not\":\"an array\"}")) &&
              damaged.isEmpty(),
          "a stored strip that is not a list of entries loads as nothing");
    check(damaged.load(QByteArray()) && damaged.isEmpty(),
          "an absent store is an empty strip, not an error");
    check(damaged.load(QByteArray(
              "[{\"id\":\"a\",\"label\":\"x\",\"durationMs\":0,"
              "\"endsAtMs\":1},"
              "{\"id\":\"c\",\"kind\":\"sundial\",\"label\":\"x\","
              "\"durationMs\":60000,\"endsAtMs\":2},"
              "{\"id\":\"b\",\"label\":\"ok\",\"durationMs\":60000,"
              "\"endsAtMs\":2}]")) &&
              damaged.count() == 1 &&
              damaged.ordered(kNow).first().label == QLatin1String("ok"),
          "a hand-edited store keeps its usable entries and drops the rest");

    QByteArray overlong = "[";
    for (int i = 0; i < TimersFeature::kMaximumTimers + 5; ++i) {
        overlong += QByteArray("{\"id\":\"i") + QByteArray::number(i) +
                    "\",\"label\":\"t\",\"durationMs\":60000,\"endsAtMs\":2},";
    }
    overlong.chop(1);
    overlong += "]";
    TimersFeature bounded;
    bounded.load(overlong);
    check(bounded.count() == TimersFeature::kMaximumTimers,
          "a store with more entries than the strip holds is bounded on load");
}

void testSpokenStatus()
{
    Timer timer;
    QString error;
    TimersFeature::parseRequest(QStringLiteral("25m standup"), kNow, &timer,
                                &error);
    check(TimersFeature::spokenStatus(timer, kNow) ==
              QLatin1String("standup - 25 minutes left"),
          "the tooltip says what is left in words");
    check(TimersFeature::spokenStatus(timer, kNow + 26 * kMinute) ==
              QLatin1String("standup - finished 1 minute ago"),
          "a finished countdown says how long ago it fired");
    timer.paused = true;
    timer.remainingMs = 90 * kSecond;
    check(TimersFeature::spokenStatus(timer, kNow + kHour) ==
              QLatin1String("standup - paused with 1 minute 30 seconds left"),
          "a paused countdown says it is paused, and how much it is holding");

    Timer focus;
    TimersFeature::parseRequest(QStringLiteral("focus"), kNow, &focus, &error,
                                TimerKind::Timer);
    check(TimersFeature::spokenStatus(focus, kNow + 5 * kMinute) ==
              QLatin1String("focus - timer running for 5 minutes"),
          "a timer says how long it has been running");
    Timer alarm;
    TimersFeature::parseRequest(QStringLiteral("in 2h review"), kNow, &alarm,
                                &error, TimerKind::Alarm);
    check(TimersFeature::spokenStatus(alarm, kNow).startsWith(
              QLatin1String("review - alarm at ")) &&
              TimersFeature::spokenStatus(alarm, kNow).endsWith(
                  QLatin1String(", in 2 hours")),
          "an alarm says when it rings and how far off that is");
}

void testFinishText()
{
    const QString at = QLocale::system().toString(
        QDateTime::fromMSecsSinceEpoch(kNow + 25 * kMinute).time(),
        QLocale::ShortFormat);
    const bool sameDay = QDateTime::fromMSecsSinceEpoch(kNow).date() ==
                         QDateTime::fromMSecsSinceEpoch(kNow + 25 * kMinute).date();
    Timer timer;
    QString error;
    TimersFeature::parseRequest(QStringLiteral("25m standup"), kNow, &timer,
                                &error);
    if (sameDay) {
        check(TimersFeature::finishText(timer, kNow) ==
                  QStringLiteral("Done at %1").arg(at),
              "a running countdown says the time of day it will finish");
        check(TimersFeature::finishText(timer, kNow + 26 * kMinute) ==
                  QStringLiteral("Finished at %1").arg(at),
              "a finished countdown says the time it finished");
    }
    Timer paused = timer;
    paused.paused = true;
    paused.remainingMs = 25 * kMinute;
    check(TimersFeature::finishText(paused, kNow) ==
              TimersFeature::finishText(timer, kNow) +
                  QLatin1String(" if resumed now"),
          "a paused countdown says when it would finish if resumed now");

    Timer tomorrow;
    TimersFeature::parseRequest(QStringLiteral("1d"), kNow, &tomorrow, &error);
    check(TimersFeature::finishText(tomorrow, kNow)
              .startsWith(QLatin1String("Done tomorrow at ")),
          "a countdown ending on another day names the day");

    Timer focus;
    TimersFeature::parseRequest(QStringLiteral("focus"), kNow, &focus, &error,
                                TimerKind::Timer);
    check(TimersFeature::finishText(focus, kNow).isEmpty(),
          "a timer has no finish time");
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testDurationParsing();
    testClockParsing();
    testRequestParsing();
    testTimerAndAlarmParsing();
    testRemainingText();
    testStripLifecycle();
    testTimersAndAlarmsOnTheStrip();
    testPersistence();
    testSpokenStatus();
    testFinishText();
    if (failures == 0)
        std::printf("\nAll timer model checks passed.\n");
    else
        std::printf("\n%d timer model check(s) failed.\n", failures);
    return failures == 0 ? 0 : 1;
}
