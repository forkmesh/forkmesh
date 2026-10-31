#pragma once

#include "PullAiReview.h"
#include "PullMergeAll.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::pullreviewall {

constexpr int kMaxFindingsPerPull = 25;

struct Review
{
    int number = 0;
    QString summary;
    QList<AiReviewFinding> findings;
};

struct Report
{
    QString model;
    QList<Review> reviews;
};

struct Run
{
    QString owner;
    QString name;
    QString reportPath;
    QList<pullmergeall::Target> targets;
};

QString agentPrompt(const QString &owner, const QString &name, const QString &checkout,
                    const QList<pullmergeall::Target> &targets,
                    const QString &reportPath);
QString runTitle(int count);
bool parseReport(const QByteArray &bytes, Report *report, QString *error = nullptr);
QString reviewState(const Review &review);
QString reviewerLabel(const QString &providerName, const QString &modelLabel,
                      const QString &reportedModel);
QString threadBody(const AiReviewFinding &finding);
QString summaryBody(const Review &review, const QStringList &threadIds,
                    const QString &reviewer);
QString draftMessage(int count, const QStringList &skipped);

QByteArray encodeRuns(const QHash<int, Run> &runs);
QHash<int, Run> decodeRuns(const QByteArray &bytes);

}
