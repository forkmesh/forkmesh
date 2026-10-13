#include "IssueBurnup.h"

#include <algorithm>

namespace {

qint64 issueCreatedAt(const Issue &issue)
{
    if (issue.createdAt > 0)
        return issue.createdAt;
    qint64 createdAt = 0;
    for (const IssueEvent &event : issue.events) {
        if (event.type != QLatin1String("open") || event.ts <= 0)
            continue;
        if (createdAt == 0 || event.ts < createdAt)
            createdAt = event.ts;
    }
    return createdAt;
}

QString issueStatusAt(const Issue &issue, qint64 timestampMs)
{
    QList<const IssueEvent *> changes;
    for (const IssueEvent &event : issue.events) {
        if (event.type == QLatin1String("status") && event.ts > 0 &&
            event.ts <= timestampMs)
            changes.append(&event);
    }
    std::sort(changes.begin(), changes.end(), [](const IssueEvent *left,
                                                  const IssueEvent *right) {
        return left->ts < right->ts;
    });

    QString status = QStringLiteral("open");
    for (const IssueEvent *change : changes) {
        if (change->status == QLatin1String("open") ||
            change->status == QLatin1String("closed"))
            status = change->status;
    }
    return status;
}

} // namespace

QList<IssueBurnupPoint> buildIssueBurnupSeries(const QList<Issue> &issues,
                                               qint64 startMs, qint64 endMs,
                                               int intervals)
{
    if (endMs < startMs)
        std::swap(startMs, endMs);
    intervals = qMax(1, intervals);

    QList<IssueBurnupPoint> series;
    series.reserve(intervals + 1);
    const qint64 duration = endMs - startMs;
    for (int i = 0; i <= intervals; ++i) {
        const qint64 timestamp =
            startMs + (duration * static_cast<qint64>(i)) / intervals;
        IssueBurnupPoint point;
        point.timestampMs = timestamp;
        for (const Issue &issue : issues) {
            const qint64 createdAt = issueCreatedAt(issue);
            if (createdAt <= 0 || createdAt > timestamp)
                continue;
            if (issueStatusAt(issue, timestamp) == QLatin1String("closed"))
                ++point.closedCount;
            else
                ++point.openCount;
        }
        series.append(point);
    }
    return series;
}

qint64 firstIssueHistoryTimestamp(const QList<Issue> &issues, qint64 fallbackMs)
{
    qint64 first = 0;
    for (const Issue &issue : issues) {
        const qint64 createdAt = issueCreatedAt(issue);
        if (createdAt > 0 && (first == 0 || createdAt < first))
            first = createdAt;
    }
    return first > 0 ? first : fallbackMs;
}
