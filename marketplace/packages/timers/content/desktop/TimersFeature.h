#pragma once

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::timers {

// The three things the top-bar clock can hold. A timer counts up from when it
// was started, a countdown counts down a length ("25m standup"), and an alarm
// rings at a time of day ("7:30 standup") and shows that time on its chip.
enum class TimerKind { Timer, Countdown, Alarm };

// One entry on the clock strip. Deadlines and start instants are absolute
// wall-clock milliseconds rather than a tick, so a countdown keeps running
// while the app is closed and comes back already expired instead of restarting
// at its full length. A paused entry keeps its frozen figure instead.
struct Timer {
    QString id;
    QString label;
    TimerKind kind = TimerKind::Countdown;
    qint64 durationMs = 0;  // countdown: what Restart goes back to
    qint64 endsAtMs = 0;    // countdown and alarm: when it fires, while running
    qint64 remainingMs = 0; // countdown: frozen remainder, while paused
    qint64 startedAtMs = 0; // timer: the instant it would read zero, while running
    qint64 elapsedMs = 0;   // timer: frozen reading, while paused
    bool paused = false;
    bool announced = false; // its expiry has already been said once
};

// The model behind the top-bar clock. Everything here is pure: the caller
// supplies "now", so the same call is testable and the desktop never has two
// clocks disagreeing inside one repaint.
class TimersFeature {
public:
    // How many entries the top bar will carry. The strip has to stay a strip;
    // past this the reader is asked to finish or drop one first.
    static constexpr int kMaximumTimers = 8;
    // A countdown is a working-day tool, not a calendar. Anything longer is
    // rejected rather than silently truncated.
    static constexpr qint64 kMaximumDurationMs = qint64(7) * 24 * 60 * 60 * 1000;
    static constexpr int kMaximumLabelLength = 40;

    // "timer", "countdown", "alarm" - the stored and scripted name of a kind.
    static QString kindName(TimerKind kind);
    static bool kindFromName(const QString &name, TimerKind *kind);

    // "25m", "1h30m", "90s", "2h 5m 10s", "1d", or a bare number of minutes.
    // Returns -1 for anything else, which is a different answer from zero.
    static qint64 parseDurationMs(const QString &text);

    // A time of day - "17:00", "5:30pm", "5pm" - as the milliseconds from
    // `nowMs` until its next occurrence. A time that has already passed today
    // means tomorrow. Returns -1 when the text is not a clock time.
    static qint64 parseClockTargetMs(const QString &text, qint64 nowMs);

    // One line of reader input to one entry of `kind`. A countdown reads
    // "25m standup", "coffee in 4m" or "17:00 demo"; an alarm reads
    // "7:30 standup", "demo at 5pm" or "in 20m"; a timer's whole line is its
    // name. Returns false with a usable `error` when a countdown or alarm has
    // no length or time in it at all.
    static bool parseRequest(const QString &text, qint64 nowMs, Timer *timer,
                             QString *error = nullptr,
                             TimerKind kind = TimerKind::Countdown);

    // What a countdown or alarm has left, negative once it has run out (a
    // finished one keeps counting so the reader can see how long ago it
    // fired). A timer has no end and always reports zero here.
    static qint64 remainingMs(const Timer &timer, qint64 nowMs);
    // What a timer reads. Zero for the other kinds.
    static qint64 elapsedMs(const Timer &timer, qint64 nowMs);
    // A countdown or alarm that is not paused and has reached zero.
    static bool isFinished(const Timer &timer, qint64 nowMs);

    // The strip's own text for a remainder: "24:58", "1:04:32", "0:00", and
    // "+0:07" for a timer that finished seven seconds ago.
    static QString remainingText(qint64 remainingMs);

    // The figure on an entry's chip: a countdown's remainder, a timer's
    // reading, or the local time of day an alarm rings at.
    static QString figureText(const Timer &timer, qint64 nowMs);

    // The same state in words, for the tooltip and the accessible name.
    static QString spokenStatus(const Timer &timer, qint64 nowMs);

    // When a countdown finishes, in local wall-clock time, for the chip's
    // menu: "Done at 3:45 PM", "Done tomorrow at 9:05 AM", "Finished at
    // 3:45 PM" once it has run out, and "Done at 3:45 PM if resumed now"
    // while paused. Empty for a timer or an alarm, whose chip already reads
    // a time of day or has no end.
    static QString finishText(const Timer &timer, qint64 nowMs);

    bool add(const Timer &timer);
    // Parse and add in one step. Returns false, with `error` set, for
    // unparseable input or a full strip.
    bool addFromText(const QString &text, qint64 nowMs, QString *error = nullptr,
                     TimerKind kind = TimerKind::Countdown);
    bool remove(const QString &id);
    void clear();

    // Pause freezes a countdown's remainder or a timer's reading; resuming
    // re-hangs it off the current time. An alarm rings at a time of day, so it
    // cannot be paused, and resuming a finished countdown is Reset's job.
    bool togglePause(const QString &id, qint64 nowMs);
    // A countdown goes back to its full length, a timer back to zero. An
    // alarm has nothing to go back to.
    bool reset(const QString &id, qint64 nowMs);
    // Add time to a countdown ("+5 min") or snooze an alarm. A finished one
    // that is extended becomes live again and can announce itself once more.
    bool extend(const QString &id, qint64 extraMs, qint64 nowMs);

    int count() const { return int(m_timers.size()); }
    bool isEmpty() const { return m_timers.isEmpty(); }
    const Timer *timer(const QString &id) const;

    // The strip's reading order. Finished entries lead (they need attention),
    // then countdowns and alarms soonest first, then running timers longest
    // first, and paused entries sink to the end (they need nothing).
    QList<Timer> ordered(qint64 nowMs) const;

    // How many entries are still moving, which is what decides whether the
    // strip needs a per-second repaint at all.
    int runningCount(qint64 nowMs) const;

    // The countdowns and alarms that have run out since the last call, in the
    // order they fired. Each one is reported once; extending or resetting
    // makes it eligible again.
    QList<Timer> takeNewlyExpired(qint64 nowMs);

    // This machine's copy of the strip, kept in the package's own settings so
    // uninstalling the package takes the entries with it. Parsing applies the
    // same bounds as reader input: a hand-edited store is untrusted.
    QByteArray serialize() const;
    bool load(const QByteArray &stored);

private:
    Timer *find(const QString &id);

    QList<Timer> m_timers;
};

} // namespace forkmesh::timers
