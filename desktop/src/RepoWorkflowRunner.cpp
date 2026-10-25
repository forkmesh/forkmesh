#include "RepoWorkflowRunner.h"

#include <QDateTime>

namespace forkmesh::repoworkflows {
namespace {

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

bool passesThrough(const QString &type)
{
    return type == kNote;
}

} // namespace

Runner::Runner(RunnerHooks hooks, QObject *parent) : QObject(parent), m_hooks(std::move(hooks))
{
    m_timer.setInterval(1000);
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

QString Runner::key(const QString &repo, const QString &workflowId)
{
    return repo.toLower() + QLatin1Char('|') + workflowId;
}

qint64 Runner::now() const
{
    return m_hooks.now ? m_hooks.now() : QDateTime::currentMSecsSinceEpoch();
}

QString Runner::start(const Workflow &workflow, const QString &repo, int repoIndex, const QString &trigger,
                      const QString &taskType, const QString &assignment, QString *error)
{
    const auto refuse = [error](const QString &message) {
        if (error)
            *error = message;
        return QString();
    };
    const QString problem = validate(workflow);
    if (!problem.isEmpty())
        return refuse(errorText(problem));
    if (!workflow.enabled)
        return refuse(QStringLiteral("%1 is turned off. Turn it on to run it.").arg(workflow.name));
    const QString runKey = key(repo, workflow.id);
    if (m_active.contains(runKey))
        tick();
    if (m_active.contains(runKey))
        return refuse(QStringLiteral("%1 is already running.").arg(workflow.name));
    Active active;
    active.workflow = workflow;
    active.repoIndex = repoIndex;
    active.order = workflow.ordered();
    const qint64 started = now();
    active.run.id = QStringLiteral("r%1-%2").arg(started, 0, 36).arg(++m_serial);
    active.run.workflowId = workflow.id;
    active.run.workflowName = workflow.name;
    active.run.repo = repo;
    active.run.trigger = trigger;
    active.run.taskType = taskType;
    active.run.assignment = assignment;
    active.run.startedAtMs = started;
    active.run.status = kStatusRunning;
    for (const QString &id : std::as_const(active.order)) {
        const QJsonObject node = workflow.node(id);
        StepRun step;
        step.nodeId = id;
        step.name = text(node, "name");
        step.type = text(node, "type");
        active.run.steps.append(step);
    }
    m_active.insert(runKey, active);
    const QString runId = active.run.id;
    advance(runKey);
    if (error)
        error->clear();
    return runId;
}

bool Runner::stop(const QString &repo, const QString &workflowId)
{
    const QString runKey = key(repo, workflowId);
    auto it = m_active.find(runKey);
    if (it == m_active.end())
        return false;
    Active &active = it.value();
    if (!active.runningNode.isEmpty()) {
        if (m_hooks.stop)
            m_hooks.stop(text(active.workflow.node(active.runningNode), "type"), active.handle);
        if (StepRun *step = active.run.step(active.runningNode)) {
            step->status = kStatusStopped;
            step->finishedAtMs = now();
        }
        active.runningNode.clear();
        active.handle = 0;
    }
    complete(runKey, kStatusStopped);
    return true;
}

bool Runner::isRunning(const QString &repo, const QString &workflowId) const
{
    return m_active.contains(key(repo, workflowId));
}

Run Runner::activeRun(const QString &repo, const QString &workflowId) const
{
    return m_active.value(key(repo, workflowId)).run;
}

QList<Run> Runner::activeRuns() const
{
    QList<Run> runs;
    for (const Active &active : m_active)
        runs.append(active.run);
    return runs;
}

int Runner::sessionOf(const QString &runId) const
{
    for (const Active &active : m_active)
        if (active.run.id == runId)
            return active.run.sessionId();
    return 0;
}

QString Runner::inputFor(const Active &active, const QString &nodeId) const
{
    QStringList parts;
    for (const QString &input : active.workflow.inputsOf(nodeId)) {
        const StepRun *step = active.run.step(input);
        if (!step || isTriggerType(step->type))
            continue;
        const QString body = step->output.trimmed().isEmpty() ? step->error.trimmed() : step->output.trimmed();
        if (!body.isEmpty())
            parts.append(QStringLiteral("## %1\n\n%2").arg(step->name, body));
    }
    const QString joined = parts.join(QStringLiteral("\n\n"));
    return joined.size() > kMaxInputChars ? joined.right(kMaxInputChars) : joined;
}

void Runner::advance(const QString &runKey)
{
    while (true) {
        auto it = m_active.find(runKey);
        if (it == m_active.end())
            return;
        Active &active = it.value();
        if (!active.runningNode.isEmpty())
            return;
        QString next;
        for (const QString &id : std::as_const(active.order)) {
            const StepRun *step = active.run.step(id);
            if (step && step->status == kStatusQueued) {
                next = id;
                break;
            }
        }
        if (next.isEmpty()) {
            complete(runKey, active.failed ? kStatusError : kStatusSuccess);
            return;
        }
        const QJsonObject node = active.workflow.node(next);
        const QString type = text(node, "type");
        StepRun *step = active.run.step(next);
        step->startedAtMs = now();
        if (isTriggerType(type)) {
            step->status = kStatusSuccess;
            step->finishedAtMs = step->startedAtMs;
            continue;
        }
        const QStringList inputs = active.workflow.inputsOf(next);
        bool fed = inputs.isEmpty();
        for (const QString &input : inputs) {
            const StepRun *from = active.run.step(input);
            const bool tolerated = from && from->status == kStatusError &&
                                   active.workflow.node(input).value(QStringLiteral("continueOnFail")).toBool();
            fed = fed || (from && (from->status == kStatusSuccess || tolerated));
        }
        if (active.failed || !fed) {
            step->status = kStatusSkipped;
            step->startedAtMs = 0;
            continue;
        }
        const QString input = inputFor(active, next);
        if (node.value(QStringLiteral("disabled")).toBool() || passesThrough(type)) {
            step->status = kStatusSuccess;
            step->output = input;
            step->finishedAtMs = now();
            continue;
        }
        StepRequest request;
        request.runId = active.run.id;
        request.repo = active.run.repo;
        request.repoIndex = active.repoIndex;
        request.workflow = active.workflow;
        request.node = node;
        request.trigger = active.run.trigger;
        request.taskType = active.run.taskType;
        request.assignment = active.run.assignment;
        request.input = input;
        request.runMarkdown = runMarkdown(active.run);
        request.primaryAgent = next == active.workflow.primaryAgent();
        step->status = kStatusRunning;
        if (m_hooks.changed)
            m_hooks.changed(active.run);
        const StepStart started = m_hooks.start ? m_hooks.start(request) : StepStart{};
        it = m_active.find(runKey);
        if (it == m_active.end())
            return;
        Active &current = it.value();
        StepRun *entry = current.run.step(next);
        if (started.finished || started.handle <= 0) {
            StepPoll result;
            result.done = true;
            result.success = started.finished && started.success;
            result.output = started.output;
            result.sessionId = started.sessionId;
            result.error = started.finished ? started.error
                                            : (started.error.isEmpty()
                                                   ? QStringLiteral("The step did not start.")
                                                   : started.error);
            settle(current, next, result);
            continue;
        }
        entry->sessionId = started.sessionId;
        current.runningNode = next;
        current.handle = started.handle;
        if (m_hooks.changed)
            m_hooks.changed(current.run);
        if (!m_timer.isActive())
            m_timer.start();
        return;
    }
}

void Runner::settle(Active &active, const QString &nodeId, const StepPoll &result)
{
    StepRun *step = active.run.step(nodeId);
    if (!step)
        return;
    step->status = result.success ? kStatusSuccess : kStatusError;
    step->finishedAtMs = now();
    step->output = result.output.left(kMaxStepOutputChars);
    step->error = result.success ? QString() : result.error.left(2000);
    if (!result.logs.isEmpty())
        step->logs = result.logs;
    if (result.sessionId > 0)
        step->sessionId = result.sessionId;
    if (!result.caption.trimmed().isEmpty())
        active.run.caption = result.caption.trimmed().left(200);
    if (!result.success && !active.workflow.node(nodeId).value(QStringLiteral("continueOnFail")).toBool()) {
        active.failed = true;
        if (active.run.error.isEmpty())
            active.run.error = QStringLiteral("%1: %2").arg(step->name, step->error);
    }
}

void Runner::tick()
{
    const QStringList keys = m_active.keys();
    for (const QString &runKey : keys) {
        auto it = m_active.find(runKey);
        if (it == m_active.end() || it->runningNode.isEmpty())
            continue;
        const QString nodeId = it->runningNode;
        const QString type = text(it->workflow.node(nodeId), "type");
        const StepRun *step = it->run.step(nodeId);
        const StepPoll result =
            m_hooks.poll ? m_hooks.poll(type, it->handle, step ? step->startedAtMs : 0) : StepPoll{true, false};
        it = m_active.find(runKey);
        if (it == m_active.end())
            continue;
        if (!result.done) {
            if (StepRun *running = it->run.step(nodeId)) {
                if (result.sessionId > 0)
                    running->sessionId = result.sessionId;
                if (!result.output.isEmpty())
                    running->output = result.output.left(kMaxStepOutputChars);
                if (!result.logs.isEmpty())
                    running->logs = result.logs;
            }
            continue;
        }
        settle(it.value(), nodeId, result);
        it->runningNode.clear();
        it->handle = 0;
        if (m_hooks.changed)
            m_hooks.changed(it->run);
        advance(runKey);
    }
    bool waiting = false;
    for (const Active &active : std::as_const(m_active))
        waiting = waiting || !active.runningNode.isEmpty();
    if (!waiting)
        m_timer.stop();
}

void Runner::complete(const QString &runKey, const QString &status)
{
    auto it = m_active.find(runKey);
    if (it == m_active.end())
        return;
    Run run = it->run;
    run.status = status;
    run.finishedAtMs = now();
    for (StepRun &step : run.steps)
        if (step.status == kStatusQueued || step.status == kStatusRunning)
            step.status = kStatusSkipped;
    for (auto step = run.steps.crbegin(); step != run.steps.crend(); ++step) {
        if (step->status == kStatusSuccess && !isTriggerType(step->type) && step->type != kOutput &&
            !step->output.trimmed().isEmpty()) {
            run.output = step->output;
            break;
        }
    }
    if (status == kStatusError && run.output.isEmpty())
        run.output = run.error;
    m_active.erase(it);
    if (m_hooks.finished)
        run.outputFile = m_hooks.finished(run, runMarkdown(run));
    if (m_hooks.changed)
        m_hooks.changed(run);
    if (m_active.isEmpty())
        m_timer.stop();
}

} // namespace forkmesh::repoworkflows
