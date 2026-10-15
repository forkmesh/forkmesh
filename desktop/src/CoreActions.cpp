#include "CoreActions.h"

#include "ActionRunner.h"
#include "ActionSandboxClaims.h"
#include "AgentResources.h"
#include "CoreClient.h"

#include <QHash>
#include <QJsonArray>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QTimer>

namespace {

constexpr int kCoreLostGraceMs = 20000;
constexpr int kCoreStartTimeoutMs = 30000;

bool definiteRefusal(const QString &error)
{
    return error != QLatin1String("core_timeout") && error != QLatin1String("core_gone");
}

}

class CoreActionRunnerLink final : public QObject, public ActionRunnerCoreLink
{
public:
    CoreActionRunnerLink(ActionRunner *runner, CoreClient *core)
        : m_runner(runner), m_core(core)
    {
        m_lost.setSingleShot(true);
        m_lost.setInterval(kCoreLostGraceMs);
        connect(&m_lost, &QTimer::timeout, this, [this] {
            if (m_runner->m_coreRun)
                interrupt(m_runner->m_run);
        });
        connect(core, &CoreClient::coreEvent, this,
                [this](const QString &event, const QJsonObject &data) { onEvent(event, data); });
        connect(core, &CoreClient::connectedChanged, this,
                [this](bool connected) { onConnected(connected); });
    }

    bool start(const ActionRun &run, const ActionWorkflow &workflow, const QString &mirrorPath,
               const QString &workTreePath, const QMap<QString, QString> &variables) override
    {
        if (m_declinedOnce) {
            m_declinedOnce = false;
            return false;
        }
        if (!m_core || !m_core->isConnected() ||
            !m_core->hasNativeRoute(QStringLiteral("core.actions.start")))
            return false;
        follow(run, workflow, mirrorPath, workTreePath, variables);
        QJsonObject vars;
        for (auto it = variables.cbegin(); it != variables.cend(); ++it)
            vars.insert(it.key(), it.value());
        const QJsonObject params{
            {QStringLiteral("run"), run.toJson()},
            {QStringLiteral("workflow"),
             QJsonObject{{QStringLiteral("path"), workflow.path},
                         {QStringLiteral("content"), workflow.content}}},
            {QStringLiteral("mirror"), mirrorPath},
            {QStringLiteral("worktree"), workTreePath},
            {QStringLiteral("variables"), vars},
            {QStringLiteral("slice"), forkmesh::agentresources::status().available
                                          ? forkmesh::agentresources::sliceName()
                                          : QString()},
            {QStringLiteral("root"), m_runner->m_store ? m_runner->m_store->rootDir() : QString()},
        };
        const int runId = run.id;
        QPointer<CoreActionRunnerLink> self(this);
        m_core->request(
            QStringLiteral("core.actions.start"), params,
            [self, runId, run, workflow, mirrorPath, workTreePath,
             variables](bool ok, const QJsonValue &, const QString &error) {
                if (!self || !self->follows(runId) || ok ||
                    error == QLatin1String("already_running"))
                    return;
                if (!definiteRefusal(error)) {
                    self->reconcile();
                    return;
                }
                ActionRunner *runner = self->m_runner;
                runner->m_busy = false;
                runner->m_coreRun = false;
                self->m_runId = -1;
                self->m_declinedOnce = true;
                runner->start(run, workflow, mirrorPath, workTreePath, variables);
                self->m_declinedOnce = false;
            },
            kCoreStartTimeoutMs);
        return true;
    }

    bool stop() override
    {
        m_runner->m_stopping = true;
        if (!m_core || !m_core->isConnected())
            return true;
        const int runId = m_runId;
        QPointer<CoreActionRunnerLink> self(this);
        m_core->request(QStringLiteral("core.actions.stop"), {{QStringLiteral("run"), runId}},
                        [self, runId](bool ok, const QJsonValue &result, const QString &) {
                            if (!self || !self->follows(runId))
                                return;
                            if (!ok || !result.toObject().value(QStringLiteral("stopping")).toBool())
                                self->reconcile();
                        });
        return true;
    }

    bool follows(int runId) const { return m_runner->m_coreRun && m_runId == runId; }

    bool adopt(const QJsonObject &active)
    {
        if (m_runner->m_busy || !m_core)
            return false;
        const ActionRun record =
            ActionRun::fromJson(active.value(QStringLiteral("run")).toObject());
        if (record.id <= 0)
            return false;
        const QJsonObject wf = active.value(QStringLiteral("workflow")).toObject();
        const QString path = wf.value(QStringLiteral("path")).toString(record.workflowPath);
        const QString content = wf.value(QStringLiteral("content")).toString(record.workflowContent);
        follow(record, ActionFile::parse(path, content),
               active.value(QStringLiteral("mirror")).toString(),
               active.value(QStringLiteral("worktree")).toString(), {});
        adoptRecord(record);
        return true;
    }

private:
    void follow(const ActionRun &run, const ActionWorkflow &workflow, const QString &mirrorPath,
                const QString &workTreePath, const QMap<QString, QString> &variables)
    {
        m_runner->m_busy = true;
        m_runner->m_coreRun = true;
        m_runner->m_stopping = false;
        m_runner->m_completing = false;
        m_runner->m_run = run;
        m_runner->m_workflow = workflow;
        m_runner->m_mirror = mirrorPath;
        m_runner->m_repoWorkTree = workTreePath;
        m_runner->m_variables = variables;
        m_runId = run.id;
        m_pendingStatus.clear();
        m_lost.stop();
    }

    void adoptRecord(const ActionRun &record)
    {
        if (record.id != m_runId)
            return;
        m_runner->m_run = record;
        if (m_runner->m_store)
            m_runner->m_store->adoptRun(record);
    }

    void onEvent(const QString &event, const QJsonObject &data)
    {
        if (!event.startsWith(QLatin1String("actions.")))
            return;
        if (event == QLatin1String("actions.sandbox")) {
            forkmesh::actions::setCoreClaimedSandbox(
                data.value(QStringLiteral("name")).toString(),
                data.value(QStringLiteral("claimed")).toBool());
            return;
        }
        const int runId = data.value(QStringLiteral("run")).toInt();
        if (!follows(runId))
            return;
        const ActionRun record =
            data.contains(QStringLiteral("record"))
                ? ActionRun::fromJson(data.value(QStringLiteral("record")).toObject())
                : m_runner->m_run;
        if (event == QLatin1String("actions.log")) {
            emit m_runner->logLine(runId, data.value(QStringLiteral("text")).toString());
        } else if (event == QLatin1String("actions.step")) {
            adoptRecord(record);
            emit m_runner->stepChanged(runId, data.value(QStringLiteral("step")).toInt(),
                                       data.value(QStringLiteral("status")).toString());
        } else if (event == QLatin1String("actions.status")) {
            adoptRecord(record);
            const QString status = data.value(QStringLiteral("status")).toString();
            if (status == ActionStatus::Running || status == ActionStatus::Queued)
                emit m_runner->statusChanged(runId, status);
            else
                m_pendingStatus = status;
        } else if (event == QLatin1String("actions.release")) {
            if (data.value(QStringLiteral("phase")).toString() == QLatin1String("prepared"))
                emit m_runner->releaseMetadataPrepared(runId);
        } else if (event == QLatin1String("actions.finished")) {
            forkmesh::coreactions::noteFinished(data);
            finish(record, data.value(QStringLiteral("ok")).toBool(),
                   data.value(QStringLiteral("landed")).toBool());
        } else if (event == QLatin1String("actions.interrupted")) {
            interrupt(record);
        }
    }

    void onConnected(bool connected)
    {
        if (!m_runner->m_coreRun)
            return;
        if (!connected) {
            if (!m_lost.isActive())
                m_lost.start();
            return;
        }
        reconcile();
    }

    void reconcile()
    {
        if (!m_runner->m_coreRun)
            return;
        if (!m_core || !m_core->isConnected()) {
            if (!m_lost.isActive())
                m_lost.start();
            return;
        }
        const int runId = m_runId;
        QPointer<CoreActionRunnerLink> self(this);
        m_core->request(
            QStringLiteral("core.actions.active"), {},
            [self, runId](bool ok, const QJsonValue &result, const QString &) {
                if (!self || !self->follows(runId))
                    return;
                if (!ok) {
                    if (!self->m_lost.isActive())
                        self->m_lost.start();
                    return;
                }
                const QJsonObject reply = result.toObject();
                forkmesh::coreactions::applyClaims(reply);
                for (const QJsonValue &value : reply.value(QStringLiteral("active")).toArray()) {
                    const ActionRun record = ActionRun::fromJson(
                        value.toObject().value(QStringLiteral("run")).toObject());
                    if (record.id != runId)
                        continue;
                    self->m_lost.stop();
                    self->adoptRecord(record);
                    emit self->m_runner->stepChanged(runId, record.currentStepIndex,
                                                     record.currentStepIndex >= 0 &&
                                                             record.currentStepIndex <
                                                                 record.stepStatuses.size()
                                                         ? record.stepStatuses.at(
                                                               record.currentStepIndex)
                                                         : record.status);
                    return;
                }
                for (const QJsonValue &value : reply.value(QStringLiteral("finished")).toArray()) {
                    const QJsonObject entry = value.toObject();
                    const ActionRun record =
                        ActionRun::fromJson(entry.value(QStringLiteral("run")).toObject());
                    if (record.id != runId)
                        continue;
                    forkmesh::coreactions::noteFinished(entry);
                    self->finish(record, entry.value(QStringLiteral("ok")).toBool(),
                                 entry.value(QStringLiteral("landed")).toBool());
                    return;
                }
                self->interrupt(self->m_runner->m_run);
            });
    }

    void finish(const ActionRun &record, bool ok, bool landed)
    {
        adoptRecord(record);
        const int runId = m_runId;
        forkmesh::coreactions::markFinishHandled(runId);
        m_runner->m_busy = false;
        m_runner->m_coreRun = false;
        m_runner->m_stopping = false;
        m_runId = -1;
        m_lost.stop();
        const QString status = m_pendingStatus.isEmpty() ? record.status : m_pendingStatus;
        m_pendingStatus.clear();
        if (m_core && m_core->isConnected())
            m_core->request(QStringLiteral("core.actions.ack"),
                            {{QStringLiteral("runs"), QJsonArray{runId}}});
        QPointer<ActionRunner> runner(m_runner);
        emit m_runner->statusChanged(runId, status);
        if (!runner)
            return;
        emit runner->finished(runId, ok);
        if (runner && landed)
            emit runner->releaseMetadataLanded(runId);
    }

    void interrupt(const ActionRun &record)
    {
        adoptRecord(record);
        const int runId = m_runId;
        m_runner->m_busy = false;
        m_runner->m_coreRun = false;
        m_runner->m_stopping = false;
        m_runId = -1;
        m_lost.stop();
        m_pendingStatus.clear();
        emit m_runner->interrupted(runId);
    }

    ActionRunner *m_runner;
    QPointer<CoreClient> m_core;
    QTimer m_lost;
    int m_runId = -1;
    QString m_pendingStatus;
    bool m_declinedOnce = false;
};

namespace forkmesh::coreactions {

void attach(ActionRunner *runner, CoreClient *core)
{
    if (!runner || !core || runner->coreLink())
        return;
    runner->setCoreLink(std::make_unique<CoreActionRunnerLink>(runner, core));
}

bool adopt(ActionRunner *runner, const QJsonObject &active)
{
    auto *link = runner ? dynamic_cast<CoreActionRunnerLink *>(runner->coreLink()) : nullptr;
    return link && link->adopt(active);
}

bool follows(const ActionRunner *runner, int runId)
{
    auto *link = runner ? dynamic_cast<CoreActionRunnerLink *>(runner->coreLink()) : nullptr;
    return link && link->follows(runId);
}

namespace {

struct FinishedFacts {
    QHash<int, QPair<bool, QString>> served;
    QSet<int> autoFix;
    QSet<int> direct;
    QSet<int> handled;
};

FinishedFacts &facts()
{
    static FinishedFacts state;
    return state;
}

}

QJsonObject pullChecksParams(const QString &owner, const QString &name, const QStringList &trusted,
                             bool identity, bool manual, bool force, const QList<int> &numbers)
{
    QJsonArray list;
    for (int number : numbers)
        list.append(number);
    return {
        {QStringLiteral("owner"), owner},
        {QStringLiteral("name"), name},
        {QStringLiteral("trusted"), QJsonArray::fromStringList(trusted)},
        {QStringLiteral("identity"), identity},
        {QStringLiteral("trigger"),
         manual ? QStringLiteral("pull_request_manual") : QStringLiteral("pull_request")},
        {QStringLiteral("force"), force},
        {QStringLiteral("numbers"), list},
    };
}

void noteFinished(const QJsonObject &finished)
{
    int runId = finished.value(QStringLiteral("run")).toInt();
    if (runId <= 0)
        runId = finished.value(QStringLiteral("run")).toObject().value(QStringLiteral("id")).toInt();
    if (runId <= 0)
        return;
    if (finished.value(QStringLiteral("autoFix")).toBool())
        facts().autoFix.insert(runId);
    const QJsonValue served = finished.value(QStringLiteral("served"));
    if (served.isObject())
        facts().served.insert(runId, {served.toObject().value(QStringLiteral("ok")).toBool(),
                                      served.toObject().value(QStringLiteral("error")).toString()});
}

bool takeCoreAutoFix(int runId)
{
    return facts().autoFix.remove(runId);
}

bool takeServedLanding(int runId, bool *ok, QString *error)
{
    const auto it = facts().served.constFind(runId);
    if (it == facts().served.constEnd())
        return false;
    if (ok)
        *ok = it->first;
    if (error)
        *error = it->second;
    facts().served.remove(runId);
    return true;
}

void followDirect(int runId)
{
    if (runId > 0)
        facts().direct.insert(runId);
}

bool followsDirect(int runId)
{
    return facts().direct.contains(runId);
}

void dropDirect(int runId)
{
    facts().direct.remove(runId);
}

QList<int> directRuns()
{
    return facts().direct.values();
}

void markFinishHandled(int runId)
{
    if (runId > 0)
        facts().handled.insert(runId);
}

bool finishHandled(int runId)
{
    return facts().handled.contains(runId);
}

void applyClaims(const QJsonObject &activeReply)
{
    if (!activeReply.contains(QStringLiteral("sandboxes")))
        return;
    QSet<QString> names;
    for (const QJsonValue &value : activeReply.value(QStringLiteral("sandboxes")).toArray())
        names.insert(value.toString());
    forkmesh::actions::replaceCoreClaimedSandboxes(names);
}

}
