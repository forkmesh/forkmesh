#ifndef FORKMESH_RESTART_TIMING_H
#define FORKMESH_RESTART_TIMING_H

#include <QList>
#include <QMap>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>

namespace forkmesh::restarttiming {

// A restart that gets as far as relaunching normally takes at least a few
// seconds. Keeping only plausible completed runs means a cancelled build or a
// hand-edited setting cannot make the next estimate misleading.
inline constexpr qint64 kMinimumRecordedDurationMs = 3 * 1000;
inline constexpr qint64 kMaximumRecordedDurationMs = 45 * 60 * 1000;
inline constexpr qint64 kFallbackEstimateMs = 2 * 60 * 1000;
inline constexpr int kHistoryLimit = 5;
// Runs kept for the Stats page; only the newest kHistoryLimit drive estimates.
inline constexpr int kStoredRunLimit = 30;
// The relaunched process must reach its constructor this soon after the old
// one handed off, or the pending record belongs to some other launch.
inline constexpr qint64 kRelaunchHandoffWindowMs = 2 * 60 * 1000;

// One completed restart: click to the relaunched window's first paint, plus
// the elapsed time at which each progress milestone was first reached.
struct RestartRun {
    qint64 finishedAtMs = 0; // epoch ms; 0 for totals recorded before timestamps
    qint64 totalMs = 0;
    QMap<int, qint64> milestones; // percent -> elapsed ms
};

inline bool isPlausibleDuration(qint64 durationMs)
{
    return durationMs >= kMinimumRecordedDurationMs &&
           durationMs <= kMaximumRecordedDurationMs;
}

inline QList<qint64> normalizedHistory(const QList<qint64> &durations)
{
    QList<qint64> result;
    result.reserve(qMin(durations.size(), kHistoryLimit));
    for (qint64 duration : durations) {
        if (isPlausibleDuration(duration))
            result.append(duration);
    }
    if (result.size() > kHistoryLimit)
        result = result.mid(result.size() - kHistoryLimit);
    return result;
}

inline qint64 medianMs(QList<qint64> samples)
{
    if (samples.isEmpty())
        return 0;
    std::sort(samples.begin(), samples.end());
    const int upper = samples.size() / 2;
    const int lower = (samples.size() - 1) / 2;
    return samples.at(lower) + (samples.at(upper) - samples.at(lower)) / 2;
}

// The median ignores one unusually slow cold build without hiding a real shift
// in normal restart time. It is deliberately calculated from the newest small
// window so the estimate adapts after a toolchain or hardware change.
inline qint64 estimatedDurationMs(const QList<qint64> &durations,
                                  qint64 fallbackMs = kFallbackEstimateMs)
{
    const QList<qint64> samples = normalizedHistory(durations);
    if (samples.isEmpty())
        return qMax<qint64>(0, fallbackMs);
    return medianMs(samples);
}

// "finishedAtMs;totalMs;percent:elapsedMs,..." — a bare number is a total
// from the older duration-only history.
inline QString serializeRun(const RestartRun &run)
{
    QStringList marks;
    for (auto it = run.milestones.cbegin(); it != run.milestones.cend(); ++it)
        marks.append(QStringLiteral("%1:%2").arg(it.key()).arg(it.value()));
    return QStringLiteral("%1;%2;%3")
        .arg(run.finishedAtMs)
        .arg(run.totalMs)
        .arg(marks.join(QLatin1Char(',')));
}

inline bool parseRun(const QString &text, RestartRun *run)
{
    RestartRun parsed;
    bool ok = false;
    const QStringList fields = text.trimmed().split(QLatin1Char(';'));
    if (fields.size() == 1) {
        parsed.totalMs = fields.at(0).toLongLong(&ok);
    } else if (fields.size() == 3) {
        parsed.finishedAtMs = fields.at(0).toLongLong(&ok);
        if (ok)
            parsed.totalMs = fields.at(1).toLongLong(&ok);
        const QStringList marks =
            fields.at(2).split(QLatin1Char(','), Qt::SkipEmptyParts);
        for (const QString &mark : marks) {
            const int colon = mark.indexOf(QLatin1Char(':'));
            bool percentOk = false;
            bool elapsedOk = false;
            const int percent = mark.left(colon).toInt(&percentOk);
            const qint64 elapsed = mark.mid(colon + 1).toLongLong(&elapsedOk);
            if (colon > 0 && percentOk && elapsedOk && percent >= 0 &&
                percent <= 100 && elapsed >= 0 && elapsed <= parsed.totalMs)
                parsed.milestones.insert(percent, elapsed);
        }
    }
    if (!ok || !isPlausibleDuration(parsed.totalMs))
        return false;
    if (run)
        *run = parsed;
    return true;
}

inline QList<RestartRun> parseRuns(const QStringList &stored)
{
    QList<RestartRun> runs;
    for (const QString &entry : stored) {
        RestartRun run;
        if (parseRun(entry, &run))
            runs.append(run);
    }
    if (runs.size() > kStoredRunLimit)
        runs = runs.mid(runs.size() - kStoredRunLimit);
    return runs;
}

inline QStringList serializeRuns(QList<RestartRun> runs)
{
    if (runs.size() > kStoredRunLimit)
        runs = runs.mid(runs.size() - kStoredRunLimit);
    QStringList stored;
    stored.reserve(runs.size());
    for (const RestartRun &run : runs)
        stored.append(serializeRun(run));
    return stored;
}

// Remaining time as of the moment `percent` was first reached, `elapsedMs`
// into this restart. Progress milestones are not evenly spaced in time (the
// compile between 60% and 80% is most of a rebuild), so the estimate is the
// median of how long recent restarts really took from that same milestone to
// a painted window. Runs without a milestone profile fall back to counting
// down from their median total.
inline qint64 remainingAfterMilestoneMs(const QList<RestartRun> &runs,
                                        int percent, qint64 elapsedMs,
                                        qint64 fallbackMs = kFallbackEstimateMs)
{
    const QList<RestartRun> recent =
        runs.size() > kHistoryLimit ? runs.mid(runs.size() - kHistoryLimit) : runs;
    QList<qint64> fromMilestone;
    QList<qint64> totals;
    for (const RestartRun &run : recent) {
        totals.append(run.totalMs);
        const auto mark = run.milestones.constFind(percent);
        if (mark != run.milestones.cend())
            fromMilestone.append(run.totalMs - mark.value());
    }
    if (percent <= 0)
        fromMilestone = totals;
    if (!fromMilestone.isEmpty())
        return medianMs(fromMilestone);
    const qint64 total = totals.isEmpty() ? fallbackMs : medianMs(totals);
    return qMax<qint64>(0, total - qMax<qint64>(0, elapsedMs));
}

// The countdown runs on wall time from the latest milestone's estimate.
inline qint64 remainingNowMs(qint64 remainingAtMilestoneMs,
                             qint64 milestoneElapsedMs, qint64 elapsedMs)
{
    return qMax<qint64>(0, remainingAtMilestoneMs -
                               qMax<qint64>(0, elapsedMs - milestoneElapsedMs));
}

inline QString durationText(qint64 durationMs)
{
    const qint64 seconds = qMax<qint64>(0, (durationMs + 999) / 1000);
    if (seconds < 60)
        return QStringLiteral("%1s").arg(seconds);
    return QStringLiteral("%1m %2s").arg(seconds / 60).arg(seconds % 60, 2,
                                                    10, QLatin1Char('0'));
}

// What a build step is doing right now, read off the output `cmake --build`
// streams: Ninja's "[412/1210] Building CXX object .../Foo.cpp.o" or Make's
// "[ 34%] Linking CXX executable forkmesh". Without it a long compile shows
// nothing but "60%" for its whole run.
struct BuildProgress {
    int done = -1;    // Ninja steps finished, out of total
    int total = -1;
    int percent = -1; // Make's own percent when there is no step count
    QString action;   // "compiling", "linking" or "building"
    QString target;   // the file or binary the step works on
    QString line;     // the progress line as printed

    bool isValid() const { return total > 0 || percent >= 0; }

    // "compiling 412/1210", "linking (99%)"
    QString text() const
    {
        if (total > 0)
            return QStringLiteral("%1 %2/%3").arg(action).arg(done).arg(total);
        if (percent >= 0)
            return QStringLiteral("%1 (%2%)").arg(action).arg(percent);
        return QString();
    }
};

// The newest progress line in `text`; invalid when it holds none.
inline BuildProgress lastBuildProgress(const QString &text)
{
    static const QRegularExpression ninja(
        QStringLiteral("^\\[(\\d+)/(\\d+)\\]\\s+(.*)$"));
    static const QRegularExpression make(
        QStringLiteral("^\\[\\s*(\\d+)%\\]\\s+(.*)$"));
    static const QRegularExpression lineBreak(QStringLiteral("[\\r\\n]"));
    const QStringList lines = text.split(lineBreak, Qt::SkipEmptyParts);
    for (auto it = lines.crbegin(); it != lines.crend(); ++it) {
        const QString line = it->trimmed();
        BuildProgress progress;
        QString description;
        if (const QRegularExpressionMatch m = ninja.match(line); m.hasMatch()) {
            progress.done = m.captured(1).toInt();
            progress.total = m.captured(2).toInt();
            description = m.captured(3);
        } else if (const QRegularExpressionMatch m = make.match(line);
                   m.hasMatch()) {
            progress.percent = m.captured(1).toInt();
            description = m.captured(2);
        }
        if (!progress.isValid())
            continue;
        progress.line = line;
        progress.action =
            description.startsWith(QLatin1String("Building"))
                ? QStringLiteral("compiling")
                : description.startsWith(QLatin1String("Linking"))
                      ? QStringLiteral("linking")
                      : QStringLiteral("building");
        QString target = description.section(QLatin1Char(' '), -1)
                             .section(QLatin1Char('/'), -1);
        for (const QLatin1String suffix : {QLatin1String(".obj"), QLatin1String(".o")}) {
            if (target.endsWith(suffix)) {
                target.chop(suffix.size());
                break;
            }
        }
        progress.target = target;
        return progress;
    }
    return {};
}

// The line beside the restart ring: progress, what the restart is doing, then
// the estimate while it holds and the real elapsed time once it has not.
inline QString countdownText(int percent, const QString &activity,
                             qint64 remainingMs, qint64 elapsedMs)
{
    if (percent >= 100)
        return QStringLiteral("100% · relaunching…");
    QString text = QStringLiteral("%1%").arg(percent);
    if (!activity.isEmpty())
        text += QStringLiteral(" · ") + activity;
    if (remainingMs > 0)
        return text + QStringLiteral(" · ~%1 left").arg(durationText(remainingMs));
    return text + QStringLiteral(" · %1 in, longer than usual…")
                      .arg(durationText(elapsedMs));
}

} // namespace forkmesh::restarttiming

#endif // FORKMESH_RESTART_TIMING_H
