#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "LocalControlInsights.h"
#include "RepoWorkflow.h"
#include "RepoWorkflowRunner.h"
#include "TeammateStore.h"

#include <QApplication>
#include <QBoxLayout>
#include <QBuffer>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMimeData>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProcess>
#include <QSaveFile>
#include <QSettings>
#include <QStandardPaths>
#include <QTimer>
#include <QToolButton>
#include <QUrl>

using namespace forkmesh::ui;
namespace rw = forkmesh::repoworkflows;
using forkmesh::teammates::Config;

namespace {

const QString kWorkflowPinsSetting = QStringLiteral("ui/workflowPins");
const QString kWorkflowPinMimeType = QStringLiteral("application/x-forkmesh-workflow-pin");

int workflowPinSide(const QString &rail)
{
    return rail == rw::kRailBottom ? 14 : (rail == rw::kRailTop ? 20 : 28);
}

bool draggedWorkflowPin(const QMimeData *mime, QString *repo, QString *workflowId)
{
    if (!mime || !mime->hasFormat(kWorkflowPinMimeType))
        return false;
    const QStringList parts =
        QString::fromUtf8(mime->data(kWorkflowPinMimeType)).split(QLatin1Char('\n'));
    if (parts.size() != 2 || parts.at(0).isEmpty() || parts.at(1).isEmpty())
        return false;
    *repo = parts.at(0);
    *workflowId = parts.at(1);
    return true;
}
const QString kHandleSession = QStringLiteral("session");
const QString kHandleShortcut = QStringLiteral("shortcut");
const QString kHandleProcess = QStringLiteral("process");
constexpr qint64 kDesktopStepTimeoutMs = 20 * 60 * 1000;
constexpr qint64 kContinueGraceMs = 2 * 60 * 1000;
constexpr int kMaxShortcutOutputChars = 8000;

QString text(const QJsonObject &object, const char *key)
{
    return object.value(QLatin1String(key)).toString();
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

bool scriptShortcut(const QString &path)
{
    const QFileInfo info(path);
    const QString suffix = info.suffix().toLower();
    if (suffix == QLatin1String("sh") || suffix == QLatin1String("bash"))
        return true;
    return suffix.isEmpty() && info.isExecutable();
}

bool promptShortcut(const QString &path)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return suffix == QLatin1String("md") || suffix == QLatin1String("txt") ||
           suffix == QLatin1String("prompt");
}

QString readText(const QString &path, qint64 limit = 256 * 1024)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(file.read(limit));
}

QString pngBase64(const QPixmap &pixmap)
{
    QByteArray png;
    QBuffer buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    pixmap.toImage().save(&buffer, "PNG");
    return QString::fromLatin1(png.toBase64());
}

QString lastLines(const QString &output, int count, QStringList *lines = nullptr)
{
    QStringList kept;
    const QStringList all = output.split(QLatin1Char('\n'));
    for (auto it = all.crbegin(); it != all.crend() && kept.size() < count; ++it) {
        const QString line = it->trimmed();
        if (!line.isEmpty())
            kept.prepend(line.left(300));
    }
    if (lines)
        *lines = kept;
    return kept.isEmpty() ? QString() : kept.constLast();
}

QString statusText(const QString &status)
{
    if (status == rw::kStatusSuccess)
        return QStringLiteral("succeeded");
    if (status == rw::kStatusError)
        return QStringLiteral("failed");
    if (status == rw::kStatusStopped)
        return QStringLiteral("stopped");
    if (status == rw::kStatusRunning)
        return QStringLiteral("running");
    return status;
}

class WorkflowPinButton final : public QToolButton
{
public:
    WorkflowPinButton(int side, QWidget *parent = nullptr) : QToolButton(parent), m_side(side)
    {
        setObjectName(QStringLiteral("workflowPinButton"));
        setAutoRaise(true);
        setCursor(Qt::PointingHandCursor);
        setFixedSize(side + 6, side + 6);
        setFocusPolicy(Qt::TabFocus);
    }

    void setState(const QPixmap &face, const QString &status)
    {
        m_face = face;
        m_status = status;
        update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QToolButton::paintEvent(event);
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRect target((width() - m_side) / 2, (height() - m_side) / 2, m_side, m_side);
        if (!m_face.isNull())
            painter.drawPixmap(target, m_face);
        QColor ring;
        if (m_status == rw::kStatusRunning)
            ring = palette().color(QPalette::Highlight);
        else if (m_status == rw::kStatusError)
            ring = QColor(QStringLiteral("#dc2626"));
        if (ring.isValid()) {
            QPen pen(ring, 2);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(QRectF(target).adjusted(-1.5, -1.5, 1.5, 1.5));
        }
    }

private:
    int m_side = 24;
    QPixmap m_face;
    QString m_status;
};

} // namespace

rw::Runner *MainWindow::repoWorkflowRunner()
{
    if (m_repoWorkflowRunner)
        return m_repoWorkflowRunner;
    rw::RunnerHooks hooks;
    hooks.start = [this](const rw::StepRequest &request) { return startRepoWorkflowStep(request); };
    hooks.poll = [this](const QString &type, int handle, qint64 startedAtMs) {
        return pollRepoWorkflowStep(type, handle, startedAtMs);
    };
    hooks.stop = [this](const QString &type, int handle) { stopRepoWorkflowStep(type, handle); };
    hooks.changed = [this](const rw::Run &run) { repoWorkflowRunChanged(run); };
    hooks.finished = [this](const rw::Run &run, const QString &markdown) {
        return finishRepoWorkflowRun(run, markdown);
    };
    m_repoWorkflowRunner = new rw::Runner(hooks, this);
    return m_repoWorkflowRunner;
}

int MainWindow::repoIndexForKey(const QString &repoKey) const
{
    const QString wanted = repoKey.trimmed();
    for (int index = 0; index < m_repositories.size(); ++index) {
        const RepositoryRecord &repo = m_repositories.at(index);
        if (repo.previewOnly || repo.localPath.isEmpty())
            continue;
        if ((repo.owner + QLatin1Char('/') + repo.name).compare(wanted, Qt::CaseInsensitive) == 0)
            return index;
    }
    return -1;
}

QString MainWindow::repoWorkflowRunsRoot(int repoIndex) const
{
    const QString digest = teammateRepoDigest(repoIndex);
    if (digest.isEmpty())
        return {};
    return QDir(forkmesh::paths::dataPath(QStringLiteral("workflow-runs"))).filePath(digest);
}

QString MainWindow::repoWorkflowRunsFile(int repoIndex, const QString &workflowId) const
{
    const QString root = repoWorkflowRunsRoot(repoIndex);
    const QString id = forkmesh::teammates::Store::safeId(workflowId);
    return root.isEmpty() || id.isEmpty() ? QString() : QDir(root).filePath(id + QStringLiteral(".json"));
}

bool MainWindow::findRepoWorkflow(int repoIndex, const QString &workflowId, rw::Workflow *workflow) const
{
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return false;
    for (const rw::Listing &listing : rw::listRepositoryWorkflows(m_repositories.at(repoIndex).localPath)) {
        if (listing.workflow.id != workflowId || !listing.error.isEmpty())
            continue;
        if (workflow)
            *workflow = listing.workflow;
        return true;
    }
    return false;
}

QString MainWindow::startRepoWorkflow(int repoIndex, const rw::Workflow &workflow, const QString &trigger,
                                      const QString &taskType, const QString &assignment, QString *error)
{
    const QString repo = shortcutRepoKey(repoIndex);
    if (repo.isEmpty() || repoIndex < 0 || m_repositories.at(repoIndex).localPath.isEmpty()) {
        if (error)
            *error = QStringLiteral("That repository has no checkout on this desktop.");
        return {};
    }
    const QString runId = repoWorkflowRunner()->start(
        workflow, repo, repoIndex, trigger,
        taskType.isEmpty() ? forkmesh::teammates::kTaskTypeOnDemand : taskType, assignment, error);
    notifyRepoWorkflowsChanged();
    return runId;
}

int MainWindow::runTeammateWorkflow(const Config &teammate, const QString &trigger, const QString &taskType,
                                    int repoIndex, const QString &assignment, bool switchToTab)
{
    if (repoIndex < 0)
        repoIndex = activeTeammateRepoIndex();
    if (repoIndex < 0)
        return 0;
    if (taskType == forkmesh::teammates::kTaskTypeMentions || trigger == QLatin1String("Direct prompt")) {
        const Config live = teammateWithRuntime(teammate, repoIndex);
        if (ownTeammateSession(live))
            return startTeammateSession(live, trigger, taskType, repoIndex, assignment, switchToTab);
    }
    rw::Workflow workflow;
    if (!findRepoWorkflow(repoIndex, teammate.id, &workflow) || !workflow.isSpecialAgent())
        workflow = rw::fromTeammate(teammate);
    m_repoWorkflowSwitchToTab = switchToTab;
    QString error;
    const QString runId = startRepoWorkflow(repoIndex, workflow, trigger, taskType, assignment, &error);
    m_repoWorkflowSwitchToTab = false;
    if (runId.isEmpty()) {
        if (switchToTab)
            setRepoDetailNotice(error, true);
        return 0;
    }
    return repoWorkflowRunner()->sessionOf(runId);
}

rw::StepStart MainWindow::startRepoWorkflowStep(const rw::StepRequest &request)
{
    rw::StepStart result;
    const QString type = text(request.node, "type");
    const QJsonObject params = request.node.value(QStringLiteral("params")).toObject();
    const int repoIndex = request.repoIndex;
    if (repoIndex < 0 || repoIndex >= m_repositories.size()) {
        result.error = QStringLiteral("The repository is no longer on this desktop.");
        return result;
    }
    const QString checkout = m_repositories.at(repoIndex).localPath;
    const auto finished = [&result](bool ok, const QString &message) {
        result.finished = true;
        result.success = ok;
        (ok ? result.output : result.error) = message;
        return result;
    };

    if (type == rw::kAgent) {
        QString problem;
        Config config = rw::agentConfig(request.workflow, text(request.node, "id"), &problem);
        if (!problem.isEmpty())
            return finished(false, problem);
        if (request.primaryAgent)
            config = teammateWithRuntime(config, repoIndex);
        if (!config.enabled)
            return finished(false, QStringLiteral("%1 is paused.").arg(config.name));
        if (!config.permissions.contains(QStringLiteral("read-repositories")))
            return finished(false, QStringLiteral("%1 needs Read repositories permission before work can start.")
                                       .arg(config.name));
        if (config.workflow == forkmesh::teammates::kWorkflowContributorPayouts &&
            request.taskType == forkmesh::teammates::kTaskTypeScheduled) {
            if (!config.permissions.contains(QStringLiteral("process-payouts")))
                return finished(false, QStringLiteral("%1 needs Process reviewed contributor payouts permission.")
                                           .arg(config.name));
            runContributorPayoutTeammate(config.id, request.trigger);
            return finished(true, QStringLiteral("Started the contributor payout workflow."));
        }
        QString assignment = request.assignment.trimmed();
        if (!request.input.trimmed().isEmpty())
            assignment += QStringLiteral("\n\nOutput of the earlier workflow steps:\n\n") + request.input.trimmed();
        const int session = startTeammateSession(config, request.trigger,
                                                 request.taskType.isEmpty() ? forkmesh::teammates::kTaskTypeOnDemand
                                                                            : request.taskType,
                                                 repoIndex, assignment.trimmed(),
                                                 request.primaryAgent && m_repoWorkflowSwitchToTab);
        if (session <= 0)
            return finished(false, QStringLiteral("%1 did not start. It may already be working, or its agent "
                                                  "account and model need attention.")
                                       .arg(config.name));
        RepoWorkflowHandle handle;
        handle.kind = kHandleSession;
        handle.id = session;
        handle.startedAtMs = nowMs();
        const int key = ++m_repoWorkflowHandleSerial;
        m_repoWorkflowHandles.insert(key, handle);
        result.handle = key;
        result.sessionId = session;
        return result;
    }

    if (type == rw::kShortcut) {
        const QString file = QFileInfo(text(params, "file")).fileName();
        const QString path = QDir(QDir(checkout).filePath(QStringLiteral(".forkmesh/shortcuts"))).filePath(file);
        if (file.isEmpty() || !QFileInfo(path).isFile())
            return finished(false, QStringLiteral("%1 is not in .forkmesh/shortcuts.").arg(file));
        const bool wait = params.value(QStringLiteral("wait")).toVariant().toString() != QLatin1String("false");
        if (scriptShortcut(path)) {
            if (shortcutProcess(path))
                return finished(false, QStringLiteral("%1 is already running.").arg(file));
            const QMap<QString, QString> environment{
                {QStringLiteral("FORKMESH_WORKFLOW"), request.workflow.id},
                {QStringLiteral("FORKMESH_WORKFLOW_RUN"), request.runId},
                {QStringLiteral("FORKMESH_WORKFLOW_INPUT"), request.input.left(rw::kMaxInputChars)}};
            const int before = m_shortcutRunCounter;
            if (!runShortcut(path, repoIndex, environment) || m_shortcutRunCounter == before)
                return finished(false, QStringLiteral("%1 did not start.").arg(file));
            if (!wait)
                return finished(true, QStringLiteral("Started %1.").arg(file));
            RepoWorkflowHandle handle;
            handle.kind = kHandleShortcut;
            handle.id = m_shortcutRunCounter;
            handle.startedAtMs = nowMs();
            const int key = ++m_repoWorkflowHandleSerial;
            m_repoWorkflowHandles.insert(key, handle);
            result.handle = key;
            return result;
        }
        if (!promptShortcut(path))
            return finished(false, QStringLiteral("%1 is neither a script nor a prompt.").arg(file));
        namespace insightctl = forkmesh::localcontrol::insights;
        const QString body = readText(path);
        const insightctl::ShortcutMeta meta = insightctl::parseShortcutMeta(file, body);
        QString prompt = insightctl::shortcutPrompt(body);
        if (prompt.trimmed().isEmpty())
            return finished(false, QStringLiteral("%1 has no prompt.").arg(file));
        prompt.replace(QStringLiteral("{{appLog}}"), QDir::toNativeSeparators(networkLogPath()));
        const QString stallLog = stallLogPath();
        prompt.replace(QStringLiteral("{{stallLog}}"), stallLog.isEmpty()
                                                           ? QStringLiteral("(disabled in this build)")
                                                           : QDir::toNativeSeparators(stallLog));
        if (!request.input.trimmed().isEmpty())
            prompt += QStringLiteral("\n\nOutput of the earlier workflow steps:\n\n") + request.input.trimmed();
        QString provider = meta.agent.isEmpty() ? defaultAgentProvider() : meta.agent;
        if (provider == QLatin1String("claude"))
            provider = QStringLiteral("claude-code");
        if (!meta.agent.isEmpty() && !agentIsCodexProvider(provider) && provider != QLatin1String("claude-code"))
            return finished(false, QStringLiteral("%1 names an agent this desktop cannot run.").arg(file));
        const QString model =
            !meta.model.isEmpty() && agentModelMatchesProvider(provider, meta.model) ? meta.model : QString();
        const bool edits = params.value(QStringLiteral("allowEdits")).toVariant().toString() == QLatin1String("true");
        const int session = startAdHocAgentForRepo(
            repoIndex, prompt, provider, false, model,
            QStringLiteral("%1 · %2").arg(request.workflow.name, meta.name), false, false, QString(),
            edits ? QStringLiteral("Edit") : QStringLiteral("Plan"));
        if (session <= 0)
            return finished(false, QStringLiteral("The agent for %1 did not start.").arg(file));
        if (!wait)
            return finished(true, QStringLiteral("Started agent #%1 for %2.").arg(session).arg(meta.name));
        RepoWorkflowHandle handle;
        handle.kind = kHandleSession;
        handle.id = session;
        handle.startedAtMs = nowMs();
        const int key = ++m_repoWorkflowHandleSerial;
        m_repoWorkflowHandles.insert(key, handle);
        result.handle = key;
        result.sessionId = session;
        return result;
    }

    if (rw::isDesktopStepType(type)) {
        QString script = checkout;
        if (!QFileInfo(QDir(script).filePath(QStringLiteral("tools/free_disk_space.py"))).isFile()) {
            script.clear();
            for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
                if (!repo.previewOnly && !repo.localPath.isEmpty() &&
                    QFileInfo(QDir(repo.localPath).filePath(QStringLiteral("tools/free_disk_space.py"))).isFile()) {
                    script = repo.localPath;
                    break;
                }
            }
        }
        if (script.isEmpty())
            return finished(false, QStringLiteral("Add a checkout of forkmesh/forkmesh: this step runs its "
                                                  "tools/free_disk_space.py."));
        const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
        if (python.isEmpty())
            return finished(false, QStringLiteral("python3 is not installed on this desktop."));
        auto *process = new QProcess(this);
        process->setWorkingDirectory(script);
        process->setProcessChannelMode(QProcess::MergedChannels);
        const int key = ++m_repoWorkflowHandleSerial;
        RepoWorkflowHandle handle;
        handle.kind = kHandleProcess;
        handle.startedAtMs = nowMs();
        handle.process = process;
        m_repoWorkflowHandles.insert(key, handle);
        auto *timeout = new QTimer(process);
        timeout->setSingleShot(true);
        connect(timeout, &QTimer::timeout, process, [process] { process->kill(); });
        connect(process, &QProcess::readyRead, this, [this, key, process] {
            auto it = m_repoWorkflowHandles.find(key);
            if (it != m_repoWorkflowHandles.end())
                it->output = (it->output + QString::fromUtf8(process->readAll())).right(64 * 1024);
        });
        connect(process, &QProcess::finished, this, [this, key, process](int code, QProcess::ExitStatus status) {
            auto it = m_repoWorkflowHandles.find(key);
            if (it != m_repoWorkflowHandles.end()) {
                it->output = (it->output + QString::fromUtf8(process->readAll())).right(64 * 1024);
                it->finished = true;
                it->ok = status == QProcess::NormalExit && code == 0;
                if (!it->ok)
                    it->error = lastLines(it->output, 1).isEmpty() ? QStringLiteral("The step failed (exit %1).").arg(code)
                                                                   : lastLines(it->output, 1);
            }
            process->deleteLater();
        });
        connect(process, &QProcess::errorOccurred, this, [this, key, process](QProcess::ProcessError failure) {
            if (failure != QProcess::FailedToStart)
                return;
            auto it = m_repoWorkflowHandles.find(key);
            if (it != m_repoWorkflowHandles.end()) {
                it->finished = true;
                it->ok = false;
                it->error = QStringLiteral("python3 could not start.");
            }
            process->deleteLater();
        });
        timeout->start(int(kDesktopStepTimeoutMs));
        process->start(python, {QStringLiteral("-u"), QStringLiteral("tools/free_disk_space.py"),
                                QStringLiteral("--execute"), QStringLiteral("--step"), rw::desktopStepName(type),
                                QStringLiteral("--repo"), script, QStringLiteral("--json")});
        result.handle = key;
        return result;
    }

    if (type == rw::kOutput) {
        const QString relative = text(params, "path").trimmed();
        if (relative.isEmpty())
            return finished(true, QStringLiteral("Kept in this run's output file."));
        const QString clean = rw::safeRepositoryPath(relative);
        if (clean.isEmpty())
            return finished(false, rw::errorText(QStringLiteral("invalid_output_path")));
        const QString target = QDir(checkout).filePath(clean);
        const QString parent = QFileInfo(target).absolutePath();
        if (!QDir().mkpath(parent))
            return finished(false, QStringLiteral("Could not create %1.").arg(QFileInfo(clean).path()));
        const QString root = QFileInfo(checkout).canonicalFilePath();
        const QString resolved = QFileInfo(parent).canonicalFilePath();
        if (root.isEmpty() || !(resolved == root || resolved.startsWith(root + QLatin1Char('/'))) ||
            QFileInfo(target).isSymLink())
            return finished(false, rw::errorText(QStringLiteral("invalid_output_path")));
        QString markdown = request.runMarkdown;
        const QString title = text(params, "title").trimmed();
        if (!title.isEmpty())
            markdown = QStringLiteral("# %1\n").arg(title) + markdown.section(QLatin1Char('\n'), 1);
        QSaveFile file(target);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(markdown.toUtf8()) < 0 ||
            !file.commit())
            return finished(false, QStringLiteral("Could not write %1.").arg(clean));
        return finished(true, QStringLiteral("Saved %1.").arg(clean));
    }

    return finished(false, QStringLiteral("This desktop cannot run %1 steps.").arg(type));
}

rw::StepPoll MainWindow::pollRepoWorkflowStep(const QString &type, int handleKey, qint64 startedAtMs)
{
    Q_UNUSED(type);
    Q_UNUSED(startedAtMs);
    rw::StepPoll result;
    auto it = m_repoWorkflowHandles.find(handleKey);
    if (it == m_repoWorkflowHandles.end()) {
        result.done = true;
        result.error = QStringLiteral("The step was lost when the desktop restarted.");
        return result;
    }
    RepoWorkflowHandle &handle = it.value();
    if (handle.kind == kHandleSession) {
        result.sessionId = handle.id;
        const AgentSession *session = findAgentSession(handle.id);
        if (!session) {
            m_repoWorkflowHandles.erase(it);
            result.done = true;
            result.error = QStringLiteral("Agent session #%1 is gone.").arg(result.sessionId);
            return result;
        }
        if (session->runInFlight() || session->status == AgentStatus::Queued ||
            !session->pendingSteerMessage.isEmpty() || !session->inFlightSteerMessage.isEmpty()) {
            handle.seenRunning = true;
            return result;
        }
        if (!AgentStatus::isTerminal(session->status) && session->status != AgentStatus::Cleared)
            return result;
        if (!handle.seenRunning && session->finishedAtMs < handle.startedAtMs - 1000 &&
            nowMs() - handle.startedAtMs < kContinueGraceMs)
            return result;
        const int sessionId = session->id;
        result.done = true;
        result.success = session->status == AgentStatus::Success;
        result.output = agentClosingSummary(sessionId).trimmed();
        if (result.output.isEmpty())
            result.output = teammateLatestOutput(session).trimmed();
        result.caption = forkmesh::teammates::runCaptionFromSummary(result.output);
        if (!result.success)
            result.error = session->lastError.trimmed().isEmpty()
                               ? QStringLiteral("Agent session #%1 %2.").arg(sessionId).arg(statusText(session->status))
                               : session->lastError.trimmed();
        m_repoWorkflowHandles.erase(it);
        return result;
    }
    if (handle.kind == kHandleShortcut) {
        const auto *run = shortcutRunById(handle.id);
        if (!run) {
            m_repoWorkflowHandles.erase(it);
            result.done = true;
            result.error = QStringLiteral("The shortcut run is gone.");
            return result;
        }
        if (run->running)
            return result;
        result.done = true;
        result.success = run->ok && !run->stopped;
        QStringList lines;
        lastLines(run->output, 40, &lines);
        result.logs = lines;
        result.output = run->output.trimmed().right(kMaxShortcutOutputChars);
        if (!result.success)
            result.error = run->stopped ? QStringLiteral("The shortcut was stopped.") : run->message;
        m_repoWorkflowHandles.erase(it);
        return result;
    }
    if (!handle.finished && handle.process && handle.process->state() != QProcess::NotRunning)
        return result;
    if (!handle.finished && handle.process)
        return result;
    result.done = true;
    result.success = handle.ok;
    QStringList lines;
    result.output = lastLines(handle.output, 40, &lines);
    result.logs = lines;
    result.error = handle.error;
    if (!result.success && result.error.isEmpty())
        result.error = QStringLiteral("The desktop step stopped before it finished.");
    m_repoWorkflowHandles.erase(it);
    return result;
}

void MainWindow::stopRepoWorkflowStep(const QString &type, int handleKey)
{
    Q_UNUSED(type);
    const auto it = m_repoWorkflowHandles.constFind(handleKey);
    if (it == m_repoWorkflowHandles.cend())
        return;
    const RepoWorkflowHandle handle = it.value();
    m_repoWorkflowHandles.remove(handleKey);
    if (handle.kind == kHandleSession)
        stopAgentSessionById(handle.id);
    else if (handle.kind == kHandleShortcut)
        stopShortcutRun(handle.id);
    else if (handle.process)
        handle.process->kill();
}

void MainWindow::repoWorkflowRunChanged(const rw::Run &run)
{
    const int repoIndex = repoIndexForKey(run.repo);
    const QString file = repoWorkflowRunsFile(repoIndex, run.workflowId);
    if (!file.isEmpty()) {
        QStringList dropped;
        rw::saveRun(file, run, &dropped);
        const QString outputs = QDir(repoWorkflowRunsRoot(repoIndex)).filePath(run.workflowId);
        for (const QString &runId : std::as_const(dropped)) {
            const QString stale = QDir(outputs).filePath(runId + QStringLiteral(".md"));
            if (QFileInfo(stale).isFile())
                QFile::remove(stale);
        }
    }
    notifyRepoWorkflowsChanged();
}

QString MainWindow::finishRepoWorkflowRun(const rw::Run &run, const QString &markdown)
{
    const int repoIndex = repoIndexForKey(run.repo);
    const QString root = repoWorkflowRunsRoot(repoIndex);
    QString path;
    if (!root.isEmpty()) {
        const QString folder = QDir(root).filePath(run.workflowId);
        if (QDir().mkpath(folder)) {
            path = QDir(folder).filePath(run.id + QStringLiteral(".md"));
            QSaveFile file(path);
            if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(markdown.toUtf8()) < 0 ||
                !file.commit())
                path.clear();
        }
    }
    if (run.trigger.startsWith(QLatin1String("By hand")) || run.trigger.contains(QLatin1String("rail"))) {
        if (run.status == rw::kStatusSuccess)
            flashMessage(QStringLiteral("%1 finished: %2").arg(run.workflowName, rw::runSummaryText(run)));
        else if (run.status == rw::kStatusError)
            flashMessage(QStringLiteral("%1 failed: %2").arg(run.workflowName, rw::runSummaryText(run)), true);
    }
    return path;
}

void MainWindow::checkRepoWorkflowSchedules()
{
    const QDateTime now = QDateTime::currentDateTime();
    for (int index = 0; index < m_repositories.size(); ++index) {
        const RepositoryRecord &repo = m_repositories.at(index);
        if (repo.previewOnly || repo.localPath.isEmpty())
            continue;
        const QString key = shortcutRepoKey(index);
        for (const rw::Listing &listing : rw::listRepositoryWorkflows(repo.localPath)) {
            const rw::Workflow &workflow = listing.workflow;
            if (!listing.error.isEmpty() || workflow.isSpecialAgent() || workflow.scheduleNode().isEmpty() ||
                repoWorkflowRunner()->isRunning(key, workflow.id))
                continue;
            const QList<rw::Run> runs = rw::loadRuns(repoWorkflowRunsFile(index, workflow.id));
            const qint64 last = runs.isEmpty() ? 0 : runs.first().startedAtMs;
            const Config schedule = rw::scheduleConfig(workflow, last);
            if (schedule.schedule.mode == forkmesh::teammates::kScheduleContinuous ||
                !forkmesh::teammates::scheduleIsDue(schedule, now))
                continue;
            QString error;
            startRepoWorkflow(index, workflow,
                              QStringLiteral("Scheduled · %1").arg(forkmesh::teammates::humanSchedule(schedule.schedule)),
                              forkmesh::teammates::kTaskTypeScheduled, QString(), &error);
        }
    }
    refreshWorkflowPins();
}

void MainWindow::triggerRepoWorkflowsForEvent(const QString &eventName, int repoIndex)
{
    if (repoIndex < 0)
        repoIndex = activeTeammateRepoIndex();
    if (repoIndex < 0 || repoIndex >= m_repositories.size())
        return;
    const QString key = shortcutRepoKey(repoIndex);
    for (const rw::Listing &listing : rw::listRepositoryWorkflows(m_repositories.at(repoIndex).localPath)) {
        const rw::Workflow &workflow = listing.workflow;
        if (!listing.error.isEmpty() || workflow.isSpecialAgent() || repoWorkflowRunner()->isRunning(key, workflow.id))
            continue;
        const Config schedule = rw::scheduleConfig(workflow, 0);
        if (!forkmesh::teammates::scheduleMatchesEvent(schedule, eventName) || !schedule.schedule.active)
            continue;
        QString error;
        startRepoWorkflow(repoIndex, workflow,
                          QStringLiteral("Repository event · %1").arg(forkmesh::teammates::taskTypeLabel(eventName)),
                          eventName, QString(), &error);
    }
}

QList<rw::Pin> MainWindow::workflowPins() const
{
    return rw::parsePins(QSettings().value(kWorkflowPinsSetting).toString());
}

void MainWindow::saveWorkflowPins(const QList<rw::Pin> &pins)
{
    QSettings().setValue(kWorkflowPinsSetting, rw::serializePins(pins));
    refreshWorkflowPins();
    notifyRepoWorkflowsChanged();
}

void MainWindow::pinWorkflowToRail(const QString &repo, const QString &workflowId, const QString &rail)
{
    if (!rw::isRail(rail))
        return;
    saveWorkflowPins(rw::pinWorkflow(workflowPins(), {repo, workflowId, rail}));
}

void MainWindow::unpinWorkflowFromRail(const QString &repo, const QString &workflowId)
{
    saveWorkflowPins(rw::unpinWorkflow(workflowPins(), repo, workflowId));
}

void MainWindow::addWorkflowPinActions(QMenu *menu, const QString &repo, const QString &workflowId)
{
    if (!menu || repo.isEmpty() || workflowId.isEmpty())
        return;
    const QString current = rw::pinnedRail(workflowPins(), repo, workflowId);
    QMenu *pin = menu->addMenu(current.isEmpty() ? QStringLiteral("Pin to rail")
                                                 : QStringLiteral("Pinned to the %1").arg(rw::railLabel(current).toLower()));
    for (const QString &rail : rw::rails()) {
        QAction *action = pin->addAction(rw::railLabel(rail));
        action->setCheckable(true);
        action->setChecked(rail == current);
        connect(action, &QAction::triggered, this,
                [this, repo, workflowId, rail] { pinWorkflowToRail(repo, workflowId, rail); });
    }
    if (!current.isEmpty()) {
        pin->addSeparator();
        connect(pin->addAction(QStringLiteral("Unpin")), &QAction::triggered, this,
                [this, repo, workflowId] { unpinWorkflowFromRail(repo, workflowId); });
    }
}

QPixmap MainWindow::repoWorkflowIcon(const rw::Workflow &workflow, int side) const
{
    if (workflow.isSpecialAgent()) {
        QString problem;
        const Config config = rw::agentConfig(workflow, workflow.primaryAgent(), &problem);
        if (problem.isEmpty())
            return teammateAvatar(config, side);
    }
    if (!workflow.icon.isEmpty() && !rw::isIconChoice(workflow.icon))
        return teammateAvatarPixmap(workflow.icon, defaultAgentProvider(), QString(), side);
    const QString name = workflow.icon == QLatin1String("command") || workflow.icon.isEmpty()
                             ? (workflow.icon.isEmpty() ? QStringLiteral("workflow") : QStringLiteral("terminal"))
                             : workflow.icon;
    QPixmap canvas(side * 2, side * 2);
    canvas.setDevicePixelRatio(2);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    painter.setRenderHint(QPainter::Antialiasing);
    QColor fill = palette().color(QPalette::Highlight);
    fill.setAlpha(46);
    painter.setPen(Qt::NoPen);
    painter.setBrush(fill);
    painter.drawEllipse(QRectF(0, 0, side, side));
    const int glyph = qMax(10, side * 3 / 5);
    const QPixmap icon = tintedOcticonPixmap(name, palette().color(QPalette::Highlight), glyph);
    painter.drawPixmap(QRect((side - glyph) / 2, (side - glyph) / 2, glyph, glyph), icon);
    painter.end();
    return canvas;
}

QWidget *MainWindow::buildWorkflowPinStrip(const QString &rail)
{
    auto *strip = new QWidget;
    strip->setObjectName(QStringLiteral("workflowPins-%1").arg(rail));
    strip->setProperty("workflowRail", rail);
    strip->setAccessibleName(QStringLiteral("Workflows pinned to the %1").arg(rw::railLabel(rail).toLower()));
    const bool vertical = rail == rw::kRailLeft || rail == rw::kRailRight;
    auto *layout = new QBoxLayout(vertical ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight, strip);
    if (rail == rw::kRailBottom)
        layout->setContentsMargins(2, 0, 2, 0);
    else if (vertical)
        layout->setContentsMargins(4, 6, 4, 6);
    else
        layout->setContentsMargins(4, 0, 4, 0);
    layout->setSpacing(vertical ? 6 : 4);
    if (vertical)
        layout->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    strip->setSizePolicy(vertical ? QSizePolicy::Fixed : QSizePolicy::Maximum,
                         vertical ? QSizePolicy::Maximum : QSizePolicy::Fixed);
    strip->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(strip, &QWidget::customContextMenuRequested, this, [this, strip](const QPoint &point) {
        QMenu menu(this);
        connect(menu.addAction(QStringLiteral("Open Workflows")), &QAction::triggered, this,
                [this] { openWorkflowInPage(QString(), QString()); });
        menu.exec(strip->mapToGlobal(point));
    });
    if (!m_workflowPinHook)
        m_workflowPinHook = new RailEventHook(
            [this](QObject *watched, QEvent *event) { return handleWorkflowPinEvent(watched, event); }, this);
    strip->setAcceptDrops(true);
    strip->installEventFilter(m_workflowPinHook);
    strip->hide();
    m_workflowPinStrips.insert(rail, strip);
    QTimer::singleShot(0, this, [this] { refreshWorkflowPins(); });
    return strip;
}

void MainWindow::refreshWorkflowPins()
{
    if (m_workflowPinStrips.isEmpty() || m_workflowPinDragging)
        return;
    const QList<rw::Pin> pins = workflowPins();
    QHash<QString, QList<rw::Listing>> listings;
    for (auto stripIt = m_workflowPinStrips.cbegin(); stripIt != m_workflowPinStrips.cend(); ++stripIt) {
        QWidget *strip = stripIt.value();
        if (!strip)
            continue;
        const QString rail = stripIt.key();
        auto *layout = qobject_cast<QBoxLayout *>(strip->layout());
        if (!layout)
            continue;
        while (QLayoutItem *item = layout->takeAt(0)) {
            if (QWidget *widget = item->widget())
                widget->deleteLater();
            delete item;
        }
        const int side = workflowPinSide(rail);
        int shown = 0;
        for (const rw::Pin &pin : pins) {
            if (pin.rail != rail)
                continue;
            const int repoIndex = repoIndexForKey(pin.repo);
            if (repoIndex < 0)
                continue;
            if (!listings.contains(pin.repo))
                listings.insert(pin.repo, rw::listRepositoryWorkflows(m_repositories.at(repoIndex).localPath));
            rw::Listing found;
            for (const rw::Listing &listing : listings.value(pin.repo))
                if (listing.workflow.id == pin.workflowId)
                    found = listing;
            if (found.workflow.id.isEmpty())
                continue;
            const rw::Workflow workflow = found.workflow;
            auto *button = new WorkflowPinButton(side, strip);
            button->setProperty("workflowRepo", pin.repo);
            button->setProperty("workflowId", pin.workflowId);
            const bool running = repoWorkflowRunner()->isRunning(pin.repo, pin.workflowId);
            const QList<rw::Run> runs = rw::loadRuns(repoWorkflowRunsFile(repoIndex, pin.workflowId));
            QString status = running ? rw::kStatusRunning : (runs.isEmpty() ? QString() : runs.first().status);
            QStringList tip{QStringLiteral("%1 · %2").arg(workflow.name, pin.repo), workflow.triggerSummary()};
            if (running) {
                const rw::Run active = repoWorkflowRunner()->activeRun(pin.repo, pin.workflowId);
                for (const rw::StepRun &step : active.steps)
                    if (step.status == rw::kStatusRunning)
                        tip << QStringLiteral("Running: %1").arg(step.name);
                tip << QStringLiteral("Click to open it in Workflows");
            } else {
                if (!runs.isEmpty())
                    tip << QStringLiteral("Last run %1 %2: %3")
                               .arg(statusText(runs.first().status),
                                    forkmesh::teammates::humanLastActive(runs.first().startedAtMs),
                                    rw::runSummaryText(runs.first()));
                tip << QStringLiteral("Click to run · right-click for more");
            }
            if (!found.error.isEmpty()) {
                status = rw::kStatusError;
                tip << found.error;
            }
            button->setToolTip(tip.join(QLatin1Char('\n')));
            button->setAccessibleName(QStringLiteral("Run %1").arg(workflow.name));
            button->setState(repoWorkflowIcon(workflow, side), status);
            const QString repo = pin.repo;
            const QString id = pin.workflowId;
            connect(button, &QToolButton::clicked, this, [this, repo, id, rail] { runPinnedWorkflow(repo, id, rail); });
            button->setContextMenuPolicy(Qt::CustomContextMenu);
            connect(button, &QWidget::customContextMenuRequested, this, [this, button, repo, id](const QPoint &point) {
                showWorkflowPinMenu(repo, id, button->mapToGlobal(point));
            });
            button->installEventFilter(m_workflowPinHook);
            layout->addWidget(button, 0, Qt::AlignCenter);
            ++shown;
        }
        if (rail == rw::kRailLeft || rail == rw::kRailRight)
            layout->addStretch(1);
        strip->setVisible(shown > 0);
    }
}

bool MainWindow::handleWorkflowPinEvent(QObject *watched, QEvent *event)
{
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget)
        return false;
    const QEvent::Type type = event->type();
    const QString rail = widget->property("workflowRail").toString();
    if (rail.isEmpty()) {
        if (widget->objectName() != QLatin1String("workflowPinButton"))
            return false;
        if (type == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            m_workflowPinDragSource.clear();
            if (mouse->button() == Qt::LeftButton) {
                m_workflowPinDragSource = widget;
                m_workflowPinDragStart = mouse->position().toPoint();
            }
        } else if (type == QEvent::MouseButtonRelease) {
            m_workflowPinDragSource.clear();
        } else if (type == QEvent::MouseMove) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (m_workflowPinDragSource != widget || !(mouse->buttons() & Qt::LeftButton) ||
                (mouse->position().toPoint() - m_workflowPinDragStart).manhattanLength() <
                    QApplication::startDragDistance())
                return false;
            m_workflowPinDragSource.clear();
            startWorkflowPinDrag(widget, m_workflowPinDragStart);
            return true;
        }
        return false;
    }
    if (type == QEvent::Resize) {
        if (QWidget *outline = widget->findChild<QWidget *>(QStringLiteral("workflowPinDropZone"),
                                                            Qt::FindDirectChildrenOnly))
            outline->setGeometry(widget->rect());
        return false;
    }
    if (type == QEvent::DragLeave) {
        hideWorkflowPinDropIndicator();
        return false;
    }
    if (type != QEvent::DragEnter && type != QEvent::DragMove && type != QEvent::Drop)
        return false;
    auto *drop = static_cast<QDropEvent *>(event);
    QString repo;
    QString workflowId;
    if (!draggedWorkflowPin(drop->mimeData(), &repo, &workflowId))
        return false;
    const QPoint at = drop->position().toPoint();
    const int slot = workflowPinSlotAt(widget, at, repo, workflowId);
    drop->setDropAction(Qt::MoveAction);
    drop->accept();
    if (type != QEvent::Drop) {
        showWorkflowPinDropIndicator(widget, at);
        return true;
    }
    hideWorkflowPinDropIndicator();
    const QList<rw::Pin> pins = rw::movePin(workflowPins(), {repo, workflowId, rail}, slot);
    QSettings().setValue(kWorkflowPinsSetting, rw::serializePins(pins));
    refreshWorkflowPins();
    notifyRepoWorkflowsChanged();
    return true;
}

void MainWindow::startWorkflowPinDrag(QWidget *button, const QPoint &hotSpot)
{
    if (auto *tool = qobject_cast<QAbstractButton *>(button))
        tool->setDown(false);
    auto *mime = new QMimeData;
    mime->setData(kWorkflowPinMimeType, QStringLiteral("%1\n%2")
                                            .arg(button->property("workflowRepo").toString(),
                                                 button->property("workflowId").toString())
                                            .toUtf8());
    auto *drag = new QDrag(this);
    drag->setMimeData(mime);
    drag->setPixmap(button->grab());
    drag->setHotSpot(hotSpot);
    setWorkflowPinStripsOpen(true);
    drag->exec(Qt::MoveAction);
    setWorkflowPinStripsOpen(false);
}

void MainWindow::setWorkflowPinStripsOpen(bool open)
{
    m_workflowPinDragging = open;
    for (auto it = m_workflowPinStrips.cbegin(); it != m_workflowPinStrips.cend(); ++it) {
        QWidget *strip = it.value();
        if (!strip)
            continue;
        QWidget *outline =
            strip->findChild<QWidget *>(QStringLiteral("workflowPinDropZone"), Qt::FindDirectChildrenOnly);
        if (!open) {
            strip->setMinimumSize(0, 0);
            delete outline;
            continue;
        }
        const QString rail = it.key();
        const int button = workflowPinSide(rail) + 6;
        const bool vertical = rail == rw::kRailLeft || rail == rw::kRailRight;
        strip->setMinimumSize(vertical ? button + 8 : button * 3, vertical ? button * 3 : button);
        if (!outline) {
            auto *frame = new QFrame(strip);
            frame->setObjectName(QStringLiteral("workflowPinDropZone"));
            frame->setAttribute(Qt::WA_TransparentForMouseEvents);
            frame->setStyleSheet(Theme::remapSheet(
                QStringLiteral("QFrame#workflowPinDropZone { background: transparent;"
                               " border: 1px dashed #2ea043; border-radius: 6px; }")));
            outline = frame;
        }
        outline->setGeometry(strip->rect());
        outline->show();
        strip->show();
    }
    if (!open) {
        hideWorkflowPinDropIndicator();
        refreshWorkflowPins();
    }
}

int MainWindow::workflowPinSlotAt(QWidget *strip, const QPoint &at, const QString &repo,
                                  const QString &workflowId) const
{
    auto *layout = strip ? qobject_cast<QBoxLayout *>(strip->layout()) : nullptr;
    if (!layout)
        return 0;
    const bool vertical = layout->direction() == QBoxLayout::TopToBottom;
    int slot = 0;
    for (int i = 0; i < layout->count(); ++i) {
        QWidget *button = layout->itemAt(i)->widget();
        if (!button || button->isHidden() || button->objectName() != QLatin1String("workflowPinButton"))
            continue;
        if (button->property("workflowId").toString() == workflowId &&
            button->property("workflowRepo").toString().compare(repo, Qt::CaseInsensitive) == 0)
            continue;
        const QPoint centre = button->geometry().center();
        if ((vertical ? centre.y() : centre.x()) < (vertical ? at.y() : at.x()))
            ++slot;
    }
    return slot;
}

void MainWindow::showWorkflowPinDropIndicator(QWidget *strip, const QPoint &at)
{
    auto *layout = strip ? qobject_cast<QBoxLayout *>(strip->layout()) : nullptr;
    if (!layout)
        return;
    if (!m_workflowPinDropIndicator) {
        auto *line = new QFrame(strip);
        line->setObjectName(QStringLiteral("workflowPinDropIndicator"));
        line->setAttribute(Qt::WA_TransparentForMouseEvents);
        line->setStyleSheet(Theme::remapSheet(QStringLiteral("background: #2ea043; border: none;")));
        m_workflowPinDropIndicator = line;
    }
    if (m_workflowPinDropIndicator->parentWidget() != strip)
        m_workflowPinDropIndicator->setParent(strip);
    const bool vertical = layout->direction() == QBoxLayout::TopToBottom;
    int edge = vertical ? layout->contentsMargins().top() : layout->contentsMargins().left();
    for (int i = 0; i < layout->count(); ++i) {
        QWidget *button = layout->itemAt(i)->widget();
        if (!button || button->isHidden() || button->objectName() != QLatin1String("workflowPinButton"))
            continue;
        const QRect box = button->geometry();
        if ((vertical ? box.center().y() : box.center().x()) >= (vertical ? at.y() : at.x())) {
            edge = (vertical ? box.top() : box.left()) - 2;
            break;
        }
        edge = (vertical ? box.bottom() : box.right()) + 2;
    }
    if (vertical)
        m_workflowPinDropIndicator->setGeometry(2, qMax(0, edge - 1), qMax(8, strip->width() - 4), 2);
    else
        m_workflowPinDropIndicator->setGeometry(qMax(0, edge - 1), 2, 2, qMax(8, strip->height() - 4));
    m_workflowPinDropIndicator->raise();
    m_workflowPinDropIndicator->show();
}

void MainWindow::hideWorkflowPinDropIndicator()
{
    if (m_workflowPinDropIndicator)
        m_workflowPinDropIndicator->hide();
}

void MainWindow::runPinnedWorkflow(const QString &repo, const QString &workflowId, const QString &rail)
{
    if (repoWorkflowRunner()->isRunning(repo, workflowId)) {
        openWorkflowInPage(repo, workflowId);
        return;
    }
    const int repoIndex = repoIndexForKey(repo);
    rw::Workflow workflow;
    if (!findRepoWorkflow(repoIndex, workflowId, &workflow)) {
        flashMessage(QStringLiteral("That workflow is no longer in %1.").arg(repo), true);
        return;
    }
    const QString trigger = QStringLiteral("By hand · %1").arg(rw::railLabel(rail).toLower());
    if (workflow.isSpecialAgent()) {
        const int session = runTeammate(workflowId, trigger, forkmesh::teammates::kTaskTypeOnDemand, repoIndex);
        if (session > 0 || repoWorkflowRunner()->isRunning(repo, workflowId))
            flashMessage(QStringLiteral("%1 is running.").arg(workflow.name));
        else
            flashMessage(QStringLiteral("%1 did not start. Open it in Workflows to see why.").arg(workflow.name), true);
        refreshWorkflowPins();
        return;
    }
    QString error;
    if (startRepoWorkflow(repoIndex, workflow, trigger, forkmesh::teammates::kTaskTypeOnDemand, QString(), &error)
            .isEmpty())
        flashMessage(error, true);
    else
        flashMessage(QStringLiteral("%1 is running.").arg(workflow.name));
    refreshWorkflowPins();
}

void MainWindow::showWorkflowPinMenu(const QString &repo, const QString &workflowId, const QPoint &globalPos)
{
    QMenu menu(this);
    const bool running = repoWorkflowRunner()->isRunning(repo, workflowId);
    const int repoIndex = repoIndexForKey(repo);
    rw::Workflow workflow;
    const bool found = findRepoWorkflow(repoIndex, workflowId, &workflow);
    if (running) {
        connect(menu.addAction(QStringLiteral("Stop")), &QAction::triggered, this, [this, repo, workflowId] {
            repoWorkflowRunner()->stop(repo, workflowId);
            refreshWorkflowPins();
        });
        const int session = repoWorkflowRunner()->activeRun(repo, workflowId).sessionId();
        if (session > 0)
            connect(menu.addAction(QStringLiteral("Show the agent at work")), &QAction::triggered, this,
                    [this, session] { switchToAgentsTab(session); });
    } else if (found) {
        connect(menu.addAction(QStringLiteral("Run now")), &QAction::triggered, this,
                [this, repo, workflowId] { runPinnedWorkflow(repo, workflowId, rw::pinnedRail(workflowPins(), repo, workflowId)); });
    }
    connect(menu.addAction(QStringLiteral("Open in Workflows")), &QAction::triggered, this,
            [this, repo, workflowId] { openWorkflowInPage(repo, workflowId); });
    if (found && workflow.isSpecialAgent())
        connect(menu.addAction(QStringLiteral("Open the agent's home")), &QAction::triggered, this,
                [this, repoIndex, workflowId] {
                    setActiveTeammateRepoIndex(repoIndex);
                    showTeammateHome(workflowId);
                });
    const QList<rw::Run> runs = rw::loadRuns(repoWorkflowRunsFile(repoIndex, workflowId));
    if (!runs.isEmpty() && !runs.first().outputFile.isEmpty() && QFileInfo(runs.first().outputFile).isFile()) {
        const QString output = runs.first().outputFile;
        connect(menu.addAction(QStringLiteral("Open the last output")), &QAction::triggered, this,
                [output] { QDesktopServices::openUrl(QUrl::fromLocalFile(output)); });
    }
    menu.addSeparator();
    addWorkflowPinActions(&menu, repo, workflowId);
    menu.exec(globalPos);
}

void MainWindow::notifyRepoWorkflowsChanged()
{
    if (!m_repoWorkflowNotifyTimer) {
        m_repoWorkflowNotifyTimer = new QTimer(this);
        m_repoWorkflowNotifyTimer->setSingleShot(true);
        m_repoWorkflowNotifyTimer->setInterval(250);
        connect(m_repoWorkflowNotifyTimer, &QTimer::timeout, this, [this] {
            refreshWorkflowPins();
            for (const NativePackagePage &page : std::as_const(m_nativePackagePages))
                if (page.widget)
                    page.widget->repositoryWorkflowsChanged();
        });
    }
    m_repoWorkflowNotifyTimer->start();
}

void MainWindow::openWorkflowInPage(const QString &repo, const QString &workflowId)
{
    for (const NativePackagePage &page : std::as_const(m_nativePackagePages)) {
        if (page.id != QLatin1String("workflows") || !page.widget)
            continue;
        showNativePackage(page.id);
        if (!repo.isEmpty() && !workflowId.isEmpty())
            page.widget->activateSearchHit(QStringLiteral("repo:%1|%2").arg(repo, workflowId));
        return;
    }
    flashMessage(QStringLiteral("Install the Workflows plug-in from the Market to edit workflows."), true);
}

QJsonObject MainWindow::nativePackageRepositoryWorkflows()
{
    static QHash<QString, QString> faces;
    const auto face = [this](const QString &cacheKey, const std::function<QPixmap()> &draw) {
        auto found = faces.constFind(cacheKey);
        if (found == faces.constEnd())
            found = faces.insert(cacheKey, pngBase64(draw()));
        return found.value();
    };
    QJsonArray avatars;
    for (const auto &choice : rw::avatarChoices()) {
        const QString key = choice.first;
        avatars.append(QJsonObject{{QStringLiteral("value"), key},
                                   {QStringLiteral("label"), choice.second},
                                   {QStringLiteral("face"), face(QStringLiteral("avatar:") + key, [this, key] {
                                        return teammateAvatarPixmap(key, defaultAgentProvider(), QString(), 28);
                                    })}});
    }
    QJsonArray icons;
    for (const auto &choice : rw::iconChoices()) {
        const QString key = choice.first;
        rw::Workflow sample;
        sample.icon = key;
        icons.append(QJsonObject{{QStringLiteral("value"), key},
                                 {QStringLiteral("label"), choice.second},
                                 {QStringLiteral("face"), face(QStringLiteral("icon:") + key, [this, sample] {
                                      return repoWorkflowIcon(sample, 28);
                                  })}});
    }
    const QList<rw::Pin> pins = workflowPins();
    QJsonArray pinRows;
    for (const rw::Pin &pin : pins)
        pinRows.append(QJsonObject{{QStringLiteral("repo"), pin.repo},
                                   {QStringLiteral("workflow"), pin.workflowId},
                                   {QStringLiteral("rail"), pin.rail}});
    QJsonArray repositories;
    QJsonArray workflows;
    const QDateTime now = QDateTime::currentDateTime();
    for (int index = 0; index < m_repositories.size(); ++index) {
        const RepositoryRecord &repo = m_repositories.at(index);
        if (repo.previewOnly || repo.localPath.isEmpty())
            continue;
        const QString key = shortcutRepoKey(index);
        QJsonArray shortcuts;
        const QFileInfoList files = QDir(QDir(repo.localPath).filePath(QStringLiteral(".forkmesh/shortcuts")))
                                        .entryInfoList(QDir::Files | QDir::Readable, QDir::Name);
        for (const QFileInfo &info : files) {
            const bool script = scriptShortcut(info.absoluteFilePath());
            if (!script && !promptShortcut(info.absoluteFilePath()))
                continue;
            const forkmesh::localcontrol::insights::ShortcutMeta meta =
                forkmesh::localcontrol::insights::parseShortcutMeta(info.fileName(),
                                                                    readText(info.absoluteFilePath(), 8192));
            shortcuts.append(QJsonObject{{QStringLiteral("file"), info.fileName()},
                                         {QStringLiteral("name"), meta.name},
                                         {QStringLiteral("description"), meta.description},
                                         {QStringLiteral("kind"), script ? QStringLiteral("script")
                                                                         : QStringLiteral("prompt")}});
        }
        repositories.append(QJsonObject{{QStringLiteral("key"), key},
                                        {QStringLiteral("title"), key},
                                        {QStringLiteral("path"), repo.localPath},
                                        {QStringLiteral("shortcuts"), shortcuts}});
        for (const rw::Listing &listing : rw::listRepositoryWorkflows(repo.localPath)) {
            const rw::Workflow &workflow = listing.workflow;
            const QList<rw::Run> runs = rw::loadRuns(repoWorkflowRunsFile(index, workflow.id));
            const bool running = repoWorkflowRunner()->isRunning(key, workflow.id);
            QJsonObject row{{QStringLiteral("repo"), key},
                            {QStringLiteral("id"), workflow.id},
                            {QStringLiteral("name"), workflow.name},
                            {QStringLiteral("description"), workflow.description},
                            {QStringLiteral("icon"), workflow.icon},
                            {QStringLiteral("enabled"), workflow.enabled},
                            {QStringLiteral("specialAgent"), workflow.isSpecialAgent()},
                            {QStringLiteral("legacy"), listing.legacy},
                            {QStringLiteral("path"), listing.path},
                            {QStringLiteral("error"), listing.error},
                            {QStringLiteral("steps"), int(workflow.nodes.size())},
                            {QStringLiteral("trigger"), workflow.triggerSummary()},
                            {QStringLiteral("running"), running},
                            {QStringLiteral("pinned"), rw::pinnedRail(pins, key, workflow.id)}};
            const QJsonObject schedule = workflow.scheduleNode();
            row.insert(QStringLiteral("hasSchedule"), !schedule.isEmpty());
            const Config scheduleOf = rw::scheduleConfig(workflow, runs.isEmpty() ? 0 : runs.first().startedAtMs);
            row.insert(QStringLiteral("scheduleActive"), scheduleOf.schedule.active);
            Config timing = scheduleOf;
            if (workflow.isSpecialAgent()) {
                QString problem;
                timing = teammateWithRuntime(rw::agentConfig(workflow, workflow.primaryAgent(), &problem), index);
                if (!runs.isEmpty())
                    timing.lastRunAtMs = qMax(timing.lastRunAtMs, runs.first().startedAtMs);
                const AgentSession *own = ownTeammateSession(timing);
                row.insert(QStringLiteral("agentWorking"),
                           own && (own->status == AgentStatus::Running || own->status == AgentStatus::Queued ||
                                   own->status == AgentStatus::Waiting));
            }
            const QDateTime next = forkmesh::teammates::nextScheduledInvocation(timing, now);
            row.insert(QStringLiteral("nextRunAt"), next.isValid() ? double(next.toMSecsSinceEpoch()) : 0.0);
            if (!runs.isEmpty()) {
                const rw::Run &last = runs.first();
                row.insert(QStringLiteral("lastRunAt"), double(last.startedAtMs));
                row.insert(QStringLiteral("lastStatus"), last.status);
                row.insert(QStringLiteral("lastSummary"), rw::runSummaryText(last));
                row.insert(QStringLiteral("runs"), int(runs.size()));
                int failed = 0;
                for (const rw::Run &run : runs)
                    failed += run.status == rw::kStatusError ? 1 : 0;
                row.insert(QStringLiteral("failedRuns"), failed);
                QJsonArray recent;
                for (int position = 0; position < runs.size() && position < 10; ++position) {
                    rw::Run compact = runs.at(position);
                    compact.output = compact.output.left(600);
                    for (rw::StepRun &step : compact.steps) {
                        step.output = step.output.left(600);
                        step.logs = step.logs.mid(qMax(0, int(step.logs.size()) - 5));
                    }
                    recent.append(compact.toJson());
                }
                row.insert(QStringLiteral("recentRuns"), recent);
            } else if (timing.lastRunAtMs > 0) {
                row.insert(QStringLiteral("lastRunAt"), double(timing.lastRunAtMs));
            }
            if (running) {
                const rw::Run active = repoWorkflowRunner()->activeRun(key, workflow.id);
                for (const rw::StepRun &step : active.steps)
                    if (step.status == rw::kStatusRunning)
                        row.insert(QStringLiteral("runningStep"), step.name);
                row.insert(QStringLiteral("runningSince"), double(active.startedAtMs));
            }
            const QString iconKey = QStringLiteral("workflow:%1|%2|%3")
                                        .arg(workflow.isSpecialAgent() ? QStringLiteral("agent") : QStringLiteral("plain"),
                                             workflow.icon,
                                             workflow.isSpecialAgent()
                                                 ? text(workflow.node(workflow.primaryAgent())
                                                            .value(QStringLiteral("params"))
                                                            .toObject(),
                                                        "avatar") +
                                                       (workflow.node(workflow.primaryAgent())
                                                                .value(QStringLiteral("params"))
                                                                .toObject()
                                                                .value(QStringLiteral("lowCostModel"))
                                                                .toBool()
                                                            ? QStringLiteral("|low")
                                                            : QString())
                                                 : QString());
            row.insert(QStringLiteral("face"),
                       face(iconKey, [this, workflow] { return repoWorkflowIcon(workflow, 64); }));
            workflows.append(row);
        }
    }
    return {{QStringLiteral("catalog"), rw::catalog()},
            {QStringLiteral("avatars"), avatars},
            {QStringLiteral("icons"), icons},
            {QStringLiteral("repositories"), repositories},
            {QStringLiteral("workflows"), workflows},
            {QStringLiteral("pins"), pinRows}};
}

void MainWindow::runNativePackageRepositoryWorkflowAction(const QString &repo, const QString &workflowId,
                                                          const QString &action, const QJsonObject &payload,
                                                          forkmesh::market::NativePackageReplyHandler done)
{
    QTimer::singleShot(0, this, [this, repo, workflowId, action, payload, done = std::move(done)] {
        const auto reply = [&done](bool ok, const QJsonObject &data, const QString &message) {
            if (done)
                done(ok, data, message);
        };
        const int repoIndex = repoIndexForKey(repo);
        if (repoIndex < 0) {
            reply(false, {}, QStringLiteral("%1 has no checkout on this desktop.").arg(repo));
            return;
        }
        const QString checkout = m_repositories.at(repoIndex).localPath;
        const QList<rw::Listing> listings = rw::listRepositoryWorkflows(checkout);
        const rw::Listing *listing = nullptr;
        for (const rw::Listing &entry : listings)
            if (entry.workflow.id == workflowId)
                listing = &entry;
        const auto saved = [this, &reply, &repo](const rw::Workflow &workflow, const QString &path) {
            refreshTeammateRail();
            if (m_sectionStack && m_sectionStack->currentIndex() == kTeamSectionIndex)
                loadTeammatesPanel();
            notifyRepoWorkflowsChanged();
            reply(true,
                  {{QStringLiteral("workflow"), workflow.toJson()},
                   {QStringLiteral("repo"), repo},
                   {QStringLiteral("path"), path}},
                  QStringLiteral("Saved %1.").arg(workflow.name));
        };

        if (action == QLatin1String("save")) {
            QString problem;
            rw::Workflow workflow =
                rw::Workflow::fromJson(payload.value(QStringLiteral("workflow")).toObject(), &problem);
            if (!problem.isEmpty()) {
                reply(false, {}, rw::errorText(problem));
                return;
            }
            if (workflowId.isEmpty()) {
                for (const rw::Listing &entry : listings) {
                    if (entry.workflow.id == workflow.id) {
                        reply(false, {}, QStringLiteral("%1 already has a workflow with the id %2.")
                                             .arg(repo, workflow.id));
                        return;
                    }
                }
            } else if (workflow.id != workflowId) {
                reply(false, {}, QStringLiteral("A saved workflow keeps its id. Duplicate it to make a new one."));
                return;
            } else if (listing) {
                workflow.createdAtMs = listing->workflow.createdAtMs;
            }
            QString path;
            const QString error = rw::writeWorkflow(checkout, workflow, nowMs(), &path);
            if (!error.isEmpty()) {
                reply(false, {}, error);
                return;
            }
            saved(workflow, path);
            return;
        }
        if (!listing) {
            reply(false, {}, QStringLiteral("That workflow is no longer in %1.").arg(repo));
            return;
        }
        const rw::Workflow workflow = listing->workflow;
        if (action == QLatin1String("get")) {
            QJsonArray runs;
            for (const rw::Run &run : rw::loadRuns(repoWorkflowRunsFile(repoIndex, workflowId)))
                runs.append(run.toJson());
            QJsonObject data{{QStringLiteral("workflow"), workflow.toJson()},
                             {QStringLiteral("repo"), repo},
                             {QStringLiteral("legacy"), listing->legacy},
                             {QStringLiteral("path"), listing->path},
                             {QStringLiteral("error"), listing->error},
                             {QStringLiteral("runs"), runs}};
            if (repoWorkflowRunner()->isRunning(repo, workflowId))
                data.insert(QStringLiteral("active"), repoWorkflowRunner()->activeRun(repo, workflowId).toJson());
            reply(true, data, QString());
            return;
        }
        if (action == QLatin1String("run")) {
            if (!listing->error.isEmpty()) {
                reply(false, {}, listing->error);
                return;
            }
            const QString trigger = text(payload, "trigger").isEmpty() ? QStringLiteral("By hand · Workflows page")
                                                                       : text(payload, "trigger").left(160);
            const QString assignment = text(payload, "assignment").left(8000);
            if (workflow.isSpecialAgent()) {
                const int session =
                    runTeammate(workflowId, trigger, forkmesh::teammates::kTaskTypeOnDemand, repoIndex, assignment);
                if (repoWorkflowRunner()->isRunning(repo, workflowId)) {
                    reply(true, {{QStringLiteral("sessionId"), session}},
                          QStringLiteral("%1 is running.").arg(workflow.name));
                    return;
                }
                const QList<rw::Run> runs = rw::loadRuns(repoWorkflowRunsFile(repoIndex, workflowId));
                const bool recent = !runs.isEmpty() && nowMs() - runs.first().startedAtMs < 5000;
                reply(recent && runs.first().status == rw::kStatusSuccess, {},
                      recent ? rw::runSummaryText(runs.first())
                             : QStringLiteral("%1 did not start. Check that it is turned on, may read repositories "
                                              "and is not already working.")
                                   .arg(workflow.name));
                return;
            }
            QString error;
            const QString runId = startRepoWorkflow(repoIndex, workflow, trigger,
                                                    forkmesh::teammates::kTaskTypeOnDemand, assignment, &error);
            reply(!runId.isEmpty(), {{QStringLiteral("runId"), runId}},
                  runId.isEmpty() ? error : QStringLiteral("%1 is running.").arg(workflow.name));
            return;
        }
        if (action == QLatin1String("stop")) {
            bool stopped = repoWorkflowRunner()->stop(repo, workflowId);
            if (!stopped && workflow.isSpecialAgent()) {
                setActiveTeammateRepoIndex(repoIndex);
                stopTeammate(workflowId);
                stopped = true;
            }
            reply(stopped, {}, stopped ? QStringLiteral("Stopped %1.").arg(workflow.name)
                                       : QStringLiteral("%1 is not running.").arg(workflow.name));
            notifyRepoWorkflowsChanged();
            return;
        }
        if (action == QLatin1String("delete")) {
            repoWorkflowRunner()->stop(repo, workflowId);
            QString error;
            if (!rw::removeWorkflowFiles(checkout, workflowId, &error)) {
                reply(false, {}, error);
                return;
            }
            if (workflow.isSpecialAgent())
                clearTeammateRuntime(workflowId, repoIndex);
            saveWorkflowPins(rw::unpinWorkflow(workflowPins(), repo, workflowId));
            refreshTeammateRail();
            reply(true, {}, QStringLiteral("Deleted %1.").arg(workflow.name));
            return;
        }
        if (action == QLatin1String("pin")) {
            const QString rail = text(payload, "rail");
            if (!rw::isRail(rail)) {
                reply(false, {}, QStringLiteral("Pick the top, left, right or bottom rail."));
                return;
            }
            pinWorkflowToRail(repo, workflowId, rail);
            reply(true, {}, QStringLiteral("Pinned %1 to the %2.").arg(workflow.name, rw::railLabel(rail).toLower()));
            return;
        }
        if (action == QLatin1String("unpin")) {
            unpinWorkflowFromRail(repo, workflowId);
            reply(true, {}, QStringLiteral("Unpinned %1.").arg(workflow.name));
            return;
        }
        if (action == QLatin1String("setEnabled") || action == QLatin1String("setSchedule")) {
            if (listing->legacy && workflow.isSpecialAgent()) {
                Config config;
                rw::toTeammate(workflow, &config);
                if (action == QLatin1String("setEnabled"))
                    config.enabled = payload.value(QStringLiteral("enabled")).toBool();
                else
                    config.schedule.active = payload.value(QStringLiteral("active")).toBool();
                QString error;
                if (!forkmesh::teammates::Store(teammatesDirPath(repoIndex)).save(&config, &error)) {
                    reply(false, {}, error);
                    return;
                }
                rw::Workflow moved;
                findRepoWorkflow(repoIndex, workflowId, &moved);
                saved(moved, QDir(checkout).filePath(rw::kFolder + QLatin1Char('/') + workflowId + QStringLiteral(".json")));
                return;
            }
            rw::Workflow changed = workflow;
            if (action == QLatin1String("setEnabled")) {
                changed.enabled = payload.value(QStringLiteral("enabled")).toBool();
            } else {
                const QJsonObject schedule = changed.scheduleNode();
                if (schedule.isEmpty()) {
                    reply(false, {}, QStringLiteral("%1 has no schedule. Add a Schedule trigger first.").arg(workflow.name));
                    return;
                }
                QJsonObject node = schedule;
                QJsonObject params = node.value(QStringLiteral("params")).toObject();
                params.insert(QStringLiteral("active"), payload.value(QStringLiteral("active")).toBool());
                node.insert(QStringLiteral("params"), params);
                changed.nodes.replace(changed.indexOf(text(schedule, "id")), node);
            }
            QString path;
            const QString error = rw::writeWorkflow(checkout, changed, nowMs(), &path);
            if (!error.isEmpty()) {
                reply(false, {}, error);
                return;
            }
            saved(changed, path);
            return;
        }
        if (action == QLatin1String("openAgent")) {
            if (!workflow.isSpecialAgent()) {
                reply(false, {}, QStringLiteral("%1 has no special agent step.").arg(workflow.name));
                return;
            }
            setActiveTeammateRepoIndex(repoIndex);
            showTeammateHome(workflowId);
            reply(true, {}, QString());
            return;
        }
        if (action == QLatin1String("openSession")) {
            const int session = payload.value(QStringLiteral("sessionId")).toInt();
            if (session <= 0 || !findAgentSession(session)) {
                reply(false, {}, QStringLiteral("That agent session is no longer on this desktop."));
                return;
            }
            switchToAgentsTab(session);
            reply(true, {}, QString());
            return;
        }
        if (action == QLatin1String("openOutput")) {
            const QString file = QFileInfo(text(payload, "path")).absoluteFilePath();
            const QString root = QFileInfo(repoWorkflowRunsRoot(repoIndex)).absoluteFilePath();
            if (root.isEmpty() || !file.startsWith(root + QLatin1Char('/')) || !QFileInfo(file).isFile()) {
                reply(false, {}, QStringLiteral("That output file is gone."));
                return;
            }
            QDesktopServices::openUrl(QUrl::fromLocalFile(file));
            reply(true, {}, QString());
            return;
        }
        if (action == QLatin1String("readOutput")) {
            const QString file = QFileInfo(text(payload, "path")).absoluteFilePath();
            const QString root = QFileInfo(repoWorkflowRunsRoot(repoIndex)).absoluteFilePath();
            if (root.isEmpty() || !file.startsWith(root + QLatin1Char('/')) || !QFileInfo(file).isFile()) {
                reply(false, {}, QStringLiteral("That output file is gone."));
                return;
            }
            reply(true, {{QStringLiteral("text"), readText(file, 512 * 1024)}}, QString());
            return;
        }
        if (action == QLatin1String("openFile")) {
            openNativePackageRepositoryFile({repo, repo, checkout, QString()},
                                            QDir(checkout).relativeFilePath(listing->path), 1);
            reply(true, {}, QString());
            return;
        }
        reply(false, {}, QStringLiteral("Workflows cannot do \"%1\".").arg(action));
    });
}
