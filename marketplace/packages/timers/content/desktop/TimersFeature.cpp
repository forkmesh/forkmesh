#include "TimersFeature.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLocale>
#include <QRegularExpression>
#include <QUuid>

#include <algorithm>

namespace forkmesh::timers {
namespace {

constexpr qint64 kSecond = 1000;
constexpr qint64 kMinute = 60 * kSecond;
constexpr qint64 kHour = 60 * kMinute;
constexpr qint64 kDay = 24 * kHour;
// A stored id is only ever compared and used as a map key, so the bound is on
// how much of a hand-edited store this will carry rather than on its shape.
constexpr int kMaximumIdLength = 64;

// "1 hour 30 minutes" and "1h30m" are the same request. Words are folded to
// their unit letters before the grammar below sees them, longest first so
// "minutes" never gets eaten by the rule for "min".
QString foldUnitWords(QString text)
{
    static const std::pair<QLatin1String, QLatin1String> kWords[] = {
        {QLatin1String("days"), QLatin1String("d")},
        {QLatin1String("day"), QLatin1String("d")},
        {QLatin1String("hours"), QLatin1String("h")},
        {QLatin1String("hour"), QLatin1String("h")},
        {QLatin1String("hrs"), QLatin1String("h")},
        {QLatin1String("hr"), QLatin1String("h")},
        {QLatin1String("minutes"), QLatin1String("m")},
        {QLatin1String("minute"), QLatin1String("m")},
        {QLatin1String("mins"), QLatin1String("m")},
        {QLatin1String("min"), QLatin1String("m")},
        {QLatin1String("seconds"), QLatin1String("s")},
        {QLatin1String("second"), QLatin1String("s")},
        {QLatin1String("secs"), QLatin1String("s")},
        {QLatin1String("sec"), QLatin1String("s")},
    };
    for (const auto &[word, unit] : kWords)
        text.replace(word, unit);
    return text;
}

QString normalizedSpec(const QString &text)
{
    QString spec = text.simplified().toLower();
    spec.remove(QLatin1Char(' '));
    return foldUnitWords(spec);
}

QString defaultLabel(TimerKind kind)
{
    switch (kind) {
    case TimerKind::Timer:
        return QStringLiteral("Timer");
    case TimerKind::Alarm:
        return QStringLiteral("Alarm");
    case TimerKind::Countdown:
        break;
    }
    return QStringLiteral("Countdown");
}

QString cleanLabel(const QString &text, TimerKind kind)
{
    // "25m - standup" and "25m: standup" name the same entry as "25m standup".
    static const QString separators =
        QString::fromUtf8("-:,\xE2\x80\x93\xE2\x80\x94\xC2\xB7");
    QString label = text.simplified();
    while (!label.isEmpty() && separators.contains(label.at(0)))
        label.remove(0, 1);
    label = label.simplified();
    if (label.isEmpty())
        return defaultLabel(kind);
    return label.left(TimersFeature::kMaximumLabelLength);
}

bool boundedTimer(Timer *timer)
{
    if (!timer || timer->id.isEmpty() ||
        timer->id.size() > kMaximumIdLength)
        return false;
    if (timer->kind == TimerKind::Timer) {
        // A timer has no length; what it carries is a start and a reading.
        timer->durationMs = 0;
        timer->elapsedMs = qBound<qint64>(0, timer->elapsedMs,
                                   TimersFeature::kMaximumDurationMs);
    } else if (timer->durationMs <= 0 ||
               timer->durationMs > TimersFeature::kMaximumDurationMs) {
        return false;
    }
    if (timer->kind == TimerKind::Alarm)
        timer->paused = false;
    timer->label = cleanLabel(timer->label, timer->kind);
    return true;
}

// Whole seconds as "m:ss" or "h:mm:ss".
QString clockText(qint64 seconds)
{
    const qint64 hours = seconds / 3600;
    seconds -= hours * 3600;
    const qint64 minutes = seconds / 60;
    seconds -= minutes * 60;
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QLatin1Char('0'))
            .arg(seconds, 2, 10, QLatin1Char('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10,
                                                      QLatin1Char('0'));
}

QString spokenLength(qint64 milliseconds, bool roundUp)
{
    const qint64 absolute = qAbs(milliseconds);
    const qint64 seconds = roundUp ? (absolute + 999) / 1000 : absolute / 1000;
    const auto unit = [](qint64 n, const char *name) {
        return QStringLiteral("%1 %2%3")
            .arg(n)
            .arg(QLatin1String(name))
            .arg(n == 1 ? QString() : QStringLiteral("s"));
    };
    if (seconds < 60)
        return unit(seconds, "second");
    const qint64 minutes = seconds / 60;
    if (minutes < 60) {
        QString text = unit(minutes, "minute");
        if (seconds % 60 > 0)
            text += QLatin1Char(' ') + unit(seconds % 60, "second");
        return text;
    }
    QString text = unit(minutes / 60, "hour");
    if (minutes % 60 > 0)
        text += QLatin1Char(' ') + unit(minutes % 60, "minute");
    return text;
}

QString alarmTimeText(qint64 atMs)
{
    return QLocale::system().toString(QDateTime::fromMSecsSinceEpoch(atMs).time(),
                                      QLocale::ShortFormat);
}

} // namespace

QString TimersFeature::kindName(TimerKind kind)
{
    switch (kind) {
    case TimerKind::Timer:
        return QStringLiteral("timer");
    case TimerKind::Alarm:
        return QStringLiteral("alarm");
    case TimerKind::Countdown:
        break;
    }
    return QStringLiteral("countdown");
}

bool TimersFeature::kindFromName(const QString &name, TimerKind *kind)
{
    for (TimerKind candidate :
         {TimerKind::Timer, TimerKind::Countdown, TimerKind::Alarm}) {
        if (name == kindName(candidate)) {
            if (kind)
                *kind = candidate;
            return true;
        }
    }
    return false;
}

qint64 TimersFeature::parseDurationMs(const QString &text)
{
    const QString spec = normalizedSpec(text);
    if (spec.isEmpty())
        return -1;

    // A bare number is the unit people say out loud: minutes.
    static const QRegularExpression bare(QStringLiteral("^([0-9]{1,5})$"));
    const QRegularExpressionMatch bareMatch = bare.match(spec);
    if (bareMatch.hasMatch()) {
        const qint64 total = bareMatch.captured(1).toLongLong() * kMinute;
        return (total > 0 && total <= kMaximumDurationMs) ? total : -1;
    }

    static const QRegularExpression units(
        QStringLiteral("^(?:([0-9]{1,5})d)?(?:([0-9]{1,5})h)?"
                       "(?:([0-9]{1,5})m)?(?:([0-9]{1,5})s)?$"));
    const QRegularExpressionMatch match = units.match(spec);
    if (!match.hasMatch())
        return -1;
    qint64 total = 0;
    bool any = false;
    const qint64 scale[4] = {kDay, kHour, kMinute, kSecond};
    for (int i = 0; i < 4; ++i) {
        const QString captured = match.captured(i + 1);
        if (captured.isEmpty())
            continue;
        any = true;
        total += captured.toLongLong() * scale[i];
        if (total > kMaximumDurationMs)
            return -1;
    }
    if (!any || total <= 0)
        return -1;
    return total;
}

qint64 TimersFeature::parseClockTargetMs(const QString &text, qint64 nowMs)
{
    QString spec = text.simplified().toLower();
    spec.remove(QLatin1Char(' '));
    spec.remove(QLatin1Char('.')); // "5 p.m."
    static const QRegularExpression clock(
        QStringLiteral("^([0-9]{1,2})(?::([0-9]{2}))?(am|pm)?$"));
    const QRegularExpressionMatch match = clock.match(spec);
    if (!match.hasMatch())
        return -1;
    const QString minuteText = match.captured(2);
    const QString meridiem = match.captured(3);
    // A plain "5" is five minutes, not five o'clock. Only a colon or an
    // am/pm makes the number a time of day.
    if (minuteText.isEmpty() && meridiem.isEmpty())
        return -1;

    int hour = match.captured(1).toInt();
    const int minute = minuteText.isEmpty() ? 0 : minuteText.toInt();
    if (minute > 59)
        return -1;
    if (meridiem.isEmpty()) {
        if (hour > 23)
            return -1;
    } else {
        if (hour < 1 || hour > 12)
            return -1;
        hour = hour % 12;
        if (meridiem == QLatin1String("pm"))
            hour += 12;
    }

    const QDateTime now = QDateTime::fromMSecsSinceEpoch(nowMs);
    QDateTime target = now;
    target.setTime(QTime(hour, minute));
    if (!target.isValid())
        return -1;
    if (target <= now)
        target = target.addDays(1);
    const qint64 remaining = target.toMSecsSinceEpoch() - nowMs;
    return remaining > 0 ? remaining : -1;
}

bool TimersFeature::parseRequest(const QString &text, qint64 nowMs,
                                 Timer *timer, QString *error, TimerKind kind)
{
    const auto fail = [error](const QString &message) {
        if (error)
            *error = message;
        return false;
    };
    if (!timer)
        return fail(QStringLiteral("No timer to fill in."));
    const QString input = text.simplified();

    Timer parsed;
    parsed.id = QUuid::createUuid().toString(QUuid::WithoutBraces).left(12);
    parsed.kind = kind;

    if (kind == TimerKind::Timer) {
        // A timer starts from now; the whole line is only its name.
        parsed.label = cleanLabel(input, kind);
        parsed.startedAtMs = nowMs;
        *timer = parsed;
        if (error)
            error->clear();
        return true;
    }

    const QString hint =
        kind == TimerKind::Alarm
            ? QStringLiteral("Say when the alarm rings, like \"7:30 standup\" "
                             "or \"demo at 5pm\".")
            : QStringLiteral("Say how long the countdown runs, like \"25m "
                             "standup\" or \"17:00 demo\".");
    if (input.isEmpty())
        return fail(hint);

    const QStringList tokens =
        input.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    const auto lengthOf = [nowMs, kind](const QString &spec) {
        if (kind == TimerKind::Alarm) {
            // An alarm is a time of day first. A length still works ("in
            // 20m"), but only with a unit: "alarm 7" is not seven minutes.
            const qint64 clock = parseClockTargetMs(spec, nowMs);
            if (clock > 0)
                return clock;
            if (!spec.contains(QRegularExpression(QStringLiteral("[a-zA-Z]"))))
                return qint64(-1);
            return parseDurationMs(spec);
        }
        const qint64 duration = parseDurationMs(spec);
        return duration > 0 ? duration : parseClockTargetMs(spec, nowMs);
    };

    // "in 25m standup" and "at 5pm demo" lead with the length; so does the
    // shorthand "25m standup". Take the longest leading run that is a length.
    int start = 0;
    const QString first = tokens.first().toLower();
    if (tokens.size() > 1 &&
        (first == QLatin1String("in") || first == QLatin1String("at")))
        start = 1;
    qint64 length = -1;
    QString label;
    for (int n = qMin(4, int(tokens.size()) - start); n >= 1; --n) {
        const qint64 candidate =
            lengthOf(tokens.mid(start, n).join(QLatin1Char(' ')));
        if (candidate > 0) {
            length = candidate;
            label = tokens.mid(start + n).join(QLatin1Char(' '));
            break;
        }
    }

    // Otherwise the reader wrote it the other way round: "coffee in 4m",
    // "release freeze at 5pm". The last "in"/"at" wins, so a label may
    // contain one.
    if (length <= 0) {
        for (int i = int(tokens.size()) - 2; i >= 1; --i) {
            const QString word = tokens.at(i).toLower();
            if (word != QLatin1String("in") && word != QLatin1String("at"))
                continue;
            const qint64 candidate =
                lengthOf(tokens.mid(i + 1).join(QLatin1Char(' ')));
            if (candidate > 0) {
                length = candidate;
                label = tokens.mid(0, i).join(QLatin1Char(' '));
                break;
            }
        }
    }

    if (length <= 0)
        return fail(hint);

    parsed.label = cleanLabel(label, kind);
    parsed.durationMs = length;
    parsed.endsAtMs = nowMs + length;
    parsed.remainingMs = length;
    *timer = parsed;
    if (error)
        error->clear();
    return true;
}

qint64 TimersFeature::remainingMs(const Timer &timer, qint64 nowMs)
{
    if (timer.kind == TimerKind::Timer)
        return 0;
    return timer.paused ? timer.remainingMs : timer.endsAtMs - nowMs;
}

qint64 TimersFeature::elapsedMs(const Timer &timer, qint64 nowMs)
{
    if (timer.kind != TimerKind::Timer)
        return 0;
    return timer.paused ? timer.elapsedMs
                        : qMax<qint64>(0, nowMs - timer.startedAtMs);
}

bool TimersFeature::isFinished(const Timer &timer, qint64 nowMs)
{
    return timer.kind != TimerKind::Timer && !timer.paused &&
           remainingMs(timer, nowMs) <= 0;
}

QString TimersFeature::remainingText(qint64 remainingMs)
{
    // Both directions round away from zero, the way a countdown is read: a
    // countdown started at 25m shows "25:00" rather than "24:59" on its first
    // repaint, "0:01" through its last whole second, "0:00" only at zero, and
    // "+0:01" from the first second after it fired.
    const QString text = clockText((qAbs(remainingMs) + 999) / 1000);
    return remainingMs < 0 ? QLatin1Char('+') + text : text;
}

QString TimersFeature::figureText(const Timer &timer, qint64 nowMs)
{
    switch (timer.kind) {
    case TimerKind::Timer:
        // A timer is read like a stopwatch: the second ticks over once it has
        // actually passed.
        return clockText(elapsedMs(timer, nowMs) / 1000);
    case TimerKind::Alarm:
        return alarmTimeText(timer.endsAtMs);
    case TimerKind::Countdown:
        break;
    }
    return remainingText(remainingMs(timer, nowMs));
}

QString TimersFeature::spokenStatus(const Timer &timer, qint64 nowMs)
{
    if (timer.kind == TimerKind::Timer) {
        const QString reading = spokenLength(elapsedMs(timer, nowMs), false);
        return timer.paused
                   ? QStringLiteral("%1 - timer paused at %2").arg(timer.label, reading)
                   : QStringLiteral("%1 - timer running for %2")
                         .arg(timer.label, reading);
    }
    const qint64 left = remainingMs(timer, nowMs);
    if (timer.kind == TimerKind::Alarm) {
        const QString at = alarmTimeText(timer.endsAtMs);
        if (left <= 0)
            return QStringLiteral("%1 - alarm rang at %2, %3 ago")
                .arg(timer.label, at, spokenLength(left, true));
        return QStringLiteral("%1 - alarm at %2, in %3")
            .arg(timer.label, at, spokenLength(left, true));
    }
    if (timer.paused) {
        return QStringLiteral("%1 - paused with %2 left")
            .arg(timer.label, spokenLength(qMax<qint64>(left, 0), true));
    }
    if (left <= 0)
        return QStringLiteral("%1 - finished %2 ago")
            .arg(timer.label, spokenLength(left, true));
    return QStringLiteral("%1 - %2 left").arg(timer.label, spokenLength(left, true));
}

QString TimersFeature::finishText(const Timer &timer, qint64 nowMs)
{
    if (timer.kind != TimerKind::Countdown)
        return QString();
    // A paused countdown has no deadline of its own; the useful answer is when
    // it would end if the reader resumed it right now.
    const qint64 endsAtMs =
        timer.paused ? nowMs + qMax<qint64>(timer.remainingMs, 0) : timer.endsAtMs;
    const QDateTime end = QDateTime::fromMSecsSinceEpoch(endsAtMs);
    const QString at = alarmTimeText(endsAtMs);
    // A countdown runs for up to a week, so a finish on another day names it.
    const qint64 days =
        QDateTime::fromMSecsSinceEpoch(nowMs).date().daysTo(end.date());
    QString when;
    if (days == 0)
        when = QStringLiteral("at %1").arg(at);
    else if (days == 1)
        when = QStringLiteral("tomorrow at %1").arg(at);
    else if (days == -1)
        when = QStringLiteral("yesterday at %1").arg(at);
    else
        when = QStringLiteral("%1 at %2")
                   .arg(QLocale::system().dayName(end.date().dayOfWeek(),
                                                  QLocale::ShortFormat),
                        at);
    if (timer.paused)
        return QStringLiteral("Done %1 if resumed now").arg(when);
    if (isFinished(timer, nowMs))
        return QStringLiteral("Finished %1").arg(when);
    return QStringLiteral("Done %1").arg(when);
}

bool TimersFeature::add(const Timer &timer)
{
    if (m_timers.size() >= kMaximumTimers)
        return false;
    Timer copy = timer;
    if (!boundedTimer(&copy))
        return false;
    if (find(copy.id))
        return false;
    m_timers.append(copy);
    return true;
}

bool TimersFeature::addFromText(const QString &text, qint64 nowMs,
                                QString *error, TimerKind kind)
{
    if (m_timers.size() >= kMaximumTimers) {
        if (error)
            *error = QStringLiteral("The top bar holds %1 timers, countdowns "
                                    "and alarms. Remove one first.")
                         .arg(kMaximumTimers);
        return false;
    }
    Timer parsed;
    if (!parseRequest(text, nowMs, &parsed, error, kind))
        return false;
    if (!add(parsed)) {
        if (error)
            *error = QStringLiteral("That could not be added to the clock.");
        return false;
    }
    return true;
}

bool TimersFeature::remove(const QString &id)
{
    const auto it = std::find_if(m_timers.begin(), m_timers.end(),
                                 [&id](const Timer &t) { return t.id == id; });
    if (it == m_timers.end())
        return false;
    m_timers.erase(it);
    return true;
}

void TimersFeature::clear()
{
    m_timers.clear();
}

Timer *TimersFeature::find(const QString &id)
{
    for (Timer &timer : m_timers) {
        if (timer.id == id)
            return &timer;
    }
    return nullptr;
}

const Timer *TimersFeature::timer(const QString &id) const
{
    return const_cast<TimersFeature *>(this)->find(id);
}

bool TimersFeature::togglePause(const QString &id, qint64 nowMs)
{
    Timer *timer = find(id);
    if (!timer || timer->kind == TimerKind::Alarm)
        return false;
    if (timer->kind == TimerKind::Timer) {
        if (timer->paused) {
            timer->paused = false;
            timer->startedAtMs = nowMs - timer->elapsedMs;
        } else {
            timer->elapsedMs = elapsedMs(*timer, nowMs);
            timer->paused = true;
        }
        return true;
    }
    if (timer->paused) {
        timer->paused = false;
        timer->endsAtMs = nowMs + timer->remainingMs;
    } else {
        timer->remainingMs = timer->endsAtMs - nowMs;
        timer->paused = true;
    }
    return true;
}

bool TimersFeature::reset(const QString &id, qint64 nowMs)
{
    Timer *timer = find(id);
    if (!timer || timer->kind == TimerKind::Alarm)
        return false;
    if (timer->kind == TimerKind::Timer) {
        // Back to zero, and still running or still paused as it was.
        timer->elapsedMs = 0;
        timer->startedAtMs = nowMs;
        return true;
    }
    timer->paused = false;
    timer->remainingMs = timer->durationMs;
    timer->endsAtMs = nowMs + timer->durationMs;
    timer->announced = false;
    return true;
}

bool TimersFeature::extend(const QString &id, qint64 extraMs, qint64 nowMs)
{
    Timer *timer = find(id);
    if (!timer || extraMs <= 0 || timer->kind == TimerKind::Timer)
        return false;
    // A countdown that ran out twenty minutes ago gets five more minutes from
    // now, not five minutes measured from a deadline nobody was watching. A
    // snoozed alarm works the same way.
    const qint64 base =
        timer->paused ? qMax<qint64>(timer->remainingMs, 0)
                      : qMax<qint64>(timer->endsAtMs - nowMs, 0);
    const qint64 left = qMin(base + extraMs, kMaximumDurationMs);
    if (timer->paused)
        timer->remainingMs = left;
    else
        timer->endsAtMs = nowMs + left;
    // durationMs is the length the countdown was started with, so Restart
    // still means "the same twenty-five minutes again" after a few top-ups.
    if (left > 0)
        timer->announced = false;
    return true;
}

QList<Timer> TimersFeature::ordered(qint64 nowMs) const
{
    const auto rank = [nowMs](const Timer &timer) {
        if (isFinished(timer, nowMs))
            return 0;
        if (timer.paused)
            return 3;
        return timer.kind == TimerKind::Timer ? 2 : 1;
    };
    QList<Timer> sorted = m_timers;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [&rank, nowMs](const Timer &left, const Timer &right) {
                         const int leftRank = rank(left);
                         const int rightRank = rank(right);
                         if (leftRank != rightRank)
                             return leftRank < rightRank;
                         if (leftRank == 2)
                             return elapsedMs(left, nowMs) >
                                    elapsedMs(right, nowMs);
                         if (leftRank == 3)
                             return false;
                         return remainingMs(left, nowMs) <
                                remainingMs(right, nowMs);
                     });
    return sorted;
}

int TimersFeature::runningCount(qint64 nowMs) const
{
    int running = 0;
    for (const Timer &timer : m_timers) {
        if (timer.paused)
            continue;
        if (timer.kind == TimerKind::Timer || remainingMs(timer, nowMs) > 0)
            ++running;
    }
    return running;
}

QList<Timer> TimersFeature::takeNewlyExpired(qint64 nowMs)
{
    QList<Timer> fired;
    for (Timer &timer : m_timers) {
        if (timer.announced || !isFinished(timer, nowMs))
            continue;
        timer.announced = true;
        fired.append(timer);
    }
    std::stable_sort(fired.begin(), fired.end(),
                     [](const Timer &left, const Timer &right) {
                         return left.endsAtMs < right.endsAtMs;
                     });
    return fired;
}

QByteArray TimersFeature::serialize() const
{
    QJsonArray array;
    for (const Timer &timer : m_timers) {
        QJsonObject object{
            {QStringLiteral("id"), timer.id},
            {QStringLiteral("kind"), kindName(timer.kind)},
            {QStringLiteral("label"), timer.label},
            {QStringLiteral("paused"), timer.paused},
        };
        if (timer.kind == TimerKind::Timer) {
            object.insert(QStringLiteral("startedAtMs"), timer.startedAtMs);
            object.insert(QStringLiteral("elapsedMs"), timer.elapsedMs);
        } else {
            object.insert(QStringLiteral("durationMs"), timer.durationMs);
            object.insert(QStringLiteral("endsAtMs"), timer.endsAtMs);
            object.insert(QStringLiteral("remainingMs"), timer.remainingMs);
            object.insert(QStringLiteral("announced"), timer.announced);
        }
        array.append(object);
    }
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

bool TimersFeature::load(const QByteArray &stored)
{
    m_timers.clear();
    if (stored.trimmed().isEmpty())
        return true;
    const QJsonDocument document = QJsonDocument::fromJson(stored);
    if (!document.isArray())
        return false;
    const QJsonArray array = document.array();
    for (const QJsonValue &value : array) {
        if (m_timers.size() >= kMaximumTimers)
            break;
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        Timer timer;
        const QJsonValue kind = object.value(QStringLiteral("kind"));
        if (!kind.isUndefined() && !kindFromName(kind.toString(), &timer.kind))
            continue;
        timer.id = object.value(QStringLiteral("id")).toString().left(
            kMaximumIdLength);
        timer.label = object.value(QStringLiteral("label")).toString();
        timer.durationMs =
            object.value(QStringLiteral("durationMs")).toInteger(0);
        timer.endsAtMs = object.value(QStringLiteral("endsAtMs")).toInteger(0);
        timer.remainingMs =
            object.value(QStringLiteral("remainingMs")).toInteger(0);
        timer.startedAtMs =
            object.value(QStringLiteral("startedAtMs")).toInteger(0);
        timer.elapsedMs = object.value(QStringLiteral("elapsedMs")).toInteger(0);
        timer.paused = object.value(QStringLiteral("paused")).toBool();
        timer.announced = object.value(QStringLiteral("announced")).toBool();
        if (!boundedTimer(&timer))
            continue;
        if (timer.remainingMs > kMaximumDurationMs)
            timer.remainingMs = kMaximumDurationMs;
        if (find(timer.id))
            continue;
        m_timers.append(timer);
    }
    return true;
}

} // namespace forkmesh::timers
