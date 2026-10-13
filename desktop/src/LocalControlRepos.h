#pragma once

#include "LocalControlHosts.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QString>
#include <QStringList>

namespace forkmesh::localcontrol::repos {

QString familyPrefix();

constexpr int kDefaultCommits = 50;
constexpr int kMaxCommits = 200;
constexpr int kMaxBranches = 300;
constexpr int kMaxRuns = 50;
constexpr int kMaxManualBranches = 100;
constexpr qint64 kMaxLogChars = 128 * 1024;
constexpr int kMaxSubjectChars = 300;

int clampLimit(const QJsonValue &value, int fallback, int max);

bool parseRepoSlug(const QString &slug, QString *owner, QString *name);
bool validWorkflowPath(const QString &path);
bool validBranchName(const QString &branch);
QString branchFromRef(const QString &ref);

QString commitLogFormat();
QJsonArray parseCommits(const QByteArray &output, int max);

QString branchRefFormat();
void parseTrack(const QString &track, int *ahead, int *behind, bool *gone);
QJsonArray parseBranches(const QByteArray &output, int max);

struct RunFacts {
    int id = 0;
    QString workflowName;
    QString workflowPath;
    QString status;
    QString commit;
    QString ref;
    int pullNumber = 0;
    QString pullAuthor;
    qint64 createdAtMs = 0;
    qint64 startedAtMs = 0;
    qint64 finishedAtMs = 0;
    QStringList stepNames;
    QStringList stepStatuses;
    int currentStepIndex = -1;
};
QJsonObject runRow(const RunFacts &run);

struct WorkflowFacts {
    QString path;
    QString name;
    QString error;
    bool valid = false;
    QStringList triggers;
    bool manual = false;
    bool disabled = false;
    bool here = true;
    QString runsOn;
};
QJsonObject workflowRow(const WorkflowFacts &workflow);

QJsonObject actionsSummary(const QList<RunFacts> &runs);

struct LogSlice {
    QString text;
    qint64 from = 0;
    qint64 next = 0;
    bool reset = false;
};
LogSlice logSlice(const QString &log, qint64 since, qint64 maxChars = kMaxLogChars);

QStringList actions();
bool actionTargetsRun(const QString &action);
bool actionSwitchesWorkflow(const QString &action);
bool runStatusAllows(const QString &action, const QString &status);
QString actionLabel(const QString &action);

struct Intent {
    QString action;
    QString repo;
    QString path;
    QString branch;
    int run = 0;
};
QString intentSubject(const Intent &intent);
QString actionSummary(const Intent &intent, const QString &workflowName);

class ActionConfirmations
{
public:
    struct Issued {
        QString token;
        qint64 expiresAtMs = 0;
    };
    Issued issue(const QString &holder, const Intent &intent, qint64 nowMs);
    hosts::ConfirmationStore::Result consume(const QString &token, const QString &holder,
                                             qint64 nowMs, Intent *intent);
    int outstanding() const { return m_intents.size(); }

private:
    struct Entry {
        Intent intent;
        qint64 expiresAtMs = 0;
    };
    hosts::ConfirmationStore m_store;
    QHash<QString, Entry> m_intents;
};

} // namespace forkmesh::localcontrol::repos
