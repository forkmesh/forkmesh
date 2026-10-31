#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::pullmergeall {

constexpr int kMaxTitleChars = 160;
constexpr int kMaxTargets = 200;

struct Target
{
    int number = 0;
    QString title;
    QString authorName;
    QString branch;
    QString base;
};

struct Launch
{
    int sessionId = 0;
    int count = 0;
    QStringList skipped;
    QString error;
};

struct Run
{
    QString owner;
    QString name;
    QList<Target> targets;
};

struct Draft
{
    Run run;
    QString prompt;
    QStringList skipped;
};

QString promptTitle(const QString &title);
bool safeRefName(const QString &ref);
QString agentPrompt(const QString &owner, const QString &name,
                    const QString &checkout, const QList<Target> &targets);
QString runTitle(int count);
QStringList trackedPullBranchRefs(int number);
QStringList servedPullBranchRefs(const QList<int> &numbers);
QHash<int, QString> servedPullBranchTips(const QByteArray &lsRemote,
                                         const QList<int> &numbers);
QString missingCodeReason(bool branchBacked, const QString &head,
                          const QString &base);
QString errorMessage(const QString &code);
QString launchMessage(const Launch &launch);
QString draftMessage(int count, const QStringList &skipped);
bool draftMatches(const Draft &draft, const QString &owner, const QString &name,
                  const QString &prompt);

QJsonObject runToJson(const Run &run);
bool runFromJson(const QJsonObject &object, Run *run);
QByteArray encodeRuns(const QHash<int, Run> &runs);
QHash<int, Run> decodeRuns(const QByteArray &bytes);

}
