#pragma once

#include "RepoWorkflow.h"

#include <QHash>
#include <QObject>
#include <QTimer>

#include <functional>

namespace forkmesh::repoworkflows {

struct StepRequest {
    QString runId;
    QString repo;
    int repoIndex = -1;
    Workflow workflow;
    QJsonObject node;
    QString trigger;
    QString taskType;
    QString assignment;
    QString input;
    QString runMarkdown;
    bool primaryAgent = false;
};

struct StepStart {
    int handle = 0;
    int sessionId = 0;
    bool finished = false;
    bool success = false;
    QString output;
    QString error;
};

struct StepPoll {
    bool done = false;
    bool success = false;
    QString output;
    QString error;
    QString caption;
    QStringList logs;
    int sessionId = 0;
};

struct RunnerHooks {
    std::function<StepStart(const StepRequest &)> start;
    std::function<StepPoll(const QString &type, int handle, qint64 startedAtMs)> poll;
    std::function<void(const QString &type, int handle)> stop;
    std::function<void(const Run &)> changed;
    std::function<QString(const Run &, const QString &markdown)> finished;
    std::function<qint64()> now;
};

class Runner : public QObject
{
public:
    explicit Runner(RunnerHooks hooks, QObject *parent = nullptr);

    QString start(const Workflow &workflow, const QString &repo, int repoIndex, const QString &trigger,
                  const QString &taskType, const QString &assignment, QString *error = nullptr);
    bool stop(const QString &repo, const QString &workflowId);
    bool isRunning(const QString &repo, const QString &workflowId) const;
    Run activeRun(const QString &repo, const QString &workflowId) const;
    QList<Run> activeRuns() const;
    int sessionOf(const QString &runId) const;
    void tick();

private:
    struct Active {
        Run run;
        Workflow workflow;
        int repoIndex = -1;
        QStringList order;
        QString runningNode;
        int handle = 0;
        bool failed = false;
    };

    static QString key(const QString &repo, const QString &workflowId);
    qint64 now() const;
    void advance(const QString &key);
    void complete(const QString &key, const QString &status);
    QString inputFor(const Active &active, const QString &nodeId) const;
    void settle(Active &active, const QString &nodeId, const StepPoll &result);

    RunnerHooks m_hooks;
    QHash<QString, Active> m_active;
    QTimer m_timer;
    int m_serial = 0;
};

} // namespace forkmesh::repoworkflows
