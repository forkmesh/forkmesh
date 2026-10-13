#pragma once

#include "IssueStore.h"

#include <QList>

struct IssueBurnupPoint {
    qint64 timestampMs = 0;
    int openCount = 0;
    int closedCount = 0;
};

QList<IssueBurnupPoint> buildIssueBurnupSeries(const QList<Issue> &issues,
                                               qint64 startMs, qint64 endMs,
                                               int intervals);

qint64 firstIssueHistoryTimestamp(const QList<Issue> &issues, qint64 fallbackMs);
