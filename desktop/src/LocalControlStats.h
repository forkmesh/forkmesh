#pragma once

#include "LocalControlHosts.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QString>

namespace forkmesh::localcontrol::stats {

QString familyPrefix();

constexpr int kMaxTimelineDays = 30;
constexpr int kMaxQualityFiles = 200;
constexpr int kMaxFindingsPerFile = 10;
constexpr int kMaxFindingChars = 200;
constexpr int kMaxPathChars = 300;
constexpr int kMaxSentryIssues = 100;
constexpr int kMaxSentryTitleChars = 200;
constexpr int kMaxSentryProjectChars = 80;

QString severityForBadge(const QString &badge);
int clampDays(const QJsonValue &value);
qint64 localDayStartMs(qint64 ms);
QJsonObject timeline(const QMap<qint64, QHash<QString, int>> &buckets, qint64 nowMs,
                     int days);

bool parseRepoSlug(const QString &slug, QString *owner, QString *name);

struct QualityFinding {
    QString path;
    int severity = 0;
    QString title;
};
QString repositoryFileLabel();
QString relativeFindingPath(const QString &path, const QString &root);
int findingPenalty(int severity);
int signalPenalty(int severity);
QString gradeFor(int score);
QString severityLabel(int severity);
QJsonObject qualityReport(const QList<QualityFinding> &findings,
                          const QList<int> &signalSeverities, qint64 computedAtMs);

struct RepoFacts {
    qint64 commits = 0;
    qint64 files = 0;
    qint64 lines = 0;
    qint64 bytes = 0;
    QString day;
    bool ratchetEnabled = false;
    qint64 ceilingBytes = 0;
    QString ceilingDay;
};
QString byteText(qint64 bytes);
QString ratchetNote(const RepoFacts &facts);
QJsonObject repoStats(const RepoFacts &facts);
QString ratchetSummary(const QString &repo, bool enabled);
QString ratchetAction(bool enabled);

class RatchetConfirmations
{
public:
    struct Issued {
        QString token;
        qint64 expiresAtMs = 0;
    };
    Issued issue(const QString &holder, const QString &repo, bool enabled, qint64 nowMs);
    hosts::ConfirmationStore::Result consume(const QString &token, const QString &holder,
                                             qint64 nowMs, QString *repo, bool *enabled);
    int outstanding() const { return m_intents.size(); }

private:
    struct Intent {
        QString repo;
        bool enabled = false;
        qint64 expiresAtMs = 0;
    };
    hosts::ConfirmationStore m_store;
    QHash<QString, Intent> m_intents;
};

bool validSentryLevel(const QString &level);
QJsonArray sentryIssues(const QJsonArray &raw, int max = kMaxSentryIssues);

constexpr qint64 kCodemagicCacheMs = 5 * 60 * 1000;
QString codemagicTokenVariable();
qint64 nextUtcMonthStartMs(qint64 nowMs);
QJsonObject codemagicUsage(const QJsonObject &userReply, qint64 nowMs);
QString codemagicError(int httpStatus);

} // namespace forkmesh::localcontrol::stats
