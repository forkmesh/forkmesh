#include "AppPaths.h"
#include "CoreClient.h"
#include "CoreOps.h"
#include "ForkMeshVersion.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "SafeDelete.h"
#include "SingleInstance.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>

#include <utility>

using namespace forkmesh::ui;
namespace coreops = forkmesh::coreops;

namespace {

const qint64 kCoreOpsProcessStartedMs = QDateTime::currentMSecsSinceEpoch();
constexpr int kLongCoreRequestMs = 600000;

QJsonArray jsonStrings(const QStringList &values)
{
    QJsonArray out;
    for (const QString &value : values)
        out.append(value);
    return out;
}

bool hasRoute(CoreClient *client, const QString &method)
{
    return client && client->isConnected() && client->hasNativeRoute(method);
}

} // namespace

bool MainWindow::coreOpsAttached() const
{
    return !m_closingDown && hasRoute(m_coreClient, QStringLiteral("core.update.start"));
}

bool MainWindow::coreRebuildRunning() const
{
    const coreops::State *ops = coreops::peek(this);
    return ops && ops->runActive;
}

bool MainWindow::coreBackupRunning() const
{
    const coreops::State *ops = coreops::peek(this);
    return ops && ops->backupRunning;
}

void MainWindow::registerCoreOpsHandlers()
{
    coreops::state(this);
    m_coreClient->handle(
        QStringLiteral("ui.update.agents"),
        [this](const QJsonObject &, const CoreClient::Responder &respond) {
            int running = 0;
            for (const AgentSession &session : std::as_const(m_agentSessions))
                if (isStoppableAgentSession(session.id))
                    ++running;
            respond({{QStringLiteral("running"), running},
                     {QStringLiteral("anyRunning"), anyAgentRunning()},
                     {QStringLiteral("rebuildBusy"), m_rebuildButton && !m_rebuildButton->isEnabled()}},
                    QString());
        });
    m_coreClient->handle(
        QStringLiteral("ui.update.auto"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            if ((m_rebuildButton && !m_rebuildButton->isEnabled()) || anyAgentRunning()) {
                respond({{QStringLiteral("started"), false}}, QString());
                return;
            }
            const QString clientDir = params.value(QStringLiteral("clientDir")).toString();
            const QString tag = params.value(QStringLiteral("tag")).toString();
            const QString commit = params.value(QStringLiteral("commit")).toString();
            if (clientDir.isEmpty() || tag.isEmpty() || commit.isEmpty()) {
                respond({}, QStringLiteral("bad_params"));
                return;
            }
            respond({{QStringLiteral("started"), true}}, QString());
            QTimer::singleShot(0, this, [this, clientDir, tag, commit] {
                logSystem(QStringLiteral("Auto-update: release %1 is available; updating in the "
                                         "background.")
                              .arg(tag));
                if (tryPrebuiltAutoUpdate(clientDir, tag, commit))
                    return;
                updateRebuildRestart();
            });
        });
    m_coreClient->handle(
        QStringLiteral("ui.update.start"),
        [this](const QJsonObject &, const CoreClient::Responder &respond) {
            if (restartInProgress()) {
                respond({}, QStringLiteral("already_running"));
                return;
            }
            respond({{QStringLiteral("started"), true}}, QString());
            QTimer::singleShot(0, this, [this] { startLocalControlUpdate(); });
        });
    m_coreClient->handle(
        QStringLiteral("ui.update.relaunch"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const QString appPath = params.value(QStringLiteral("appPath")).toString();
            if (params.value(QStringLiteral("mode")).toString() == QLatin1String("supervised")) {
                if (!requestSupervisingUserServiceRestart(appPath)) {
                    respond({}, QStringLiteral("not_supervised"));
                    return;
                }
                recordRestartDuration();
                logSystem(QStringLiteral("=== Restarting now (rebuild & restart) ==="));
                respond({{QStringLiteral("quitting"), true}}, QString());
                QTimer::singleShot(200, qApp, &QCoreApplication::quit);
                return;
            }
            forkmesh::releaseSingleInstance();
            respond({{QStringLiteral("released"), true}}, QString());
        });
    m_coreClient->handle(
        QStringLiteral("ui.update.quit"),
        [this](const QJsonObject &, const CoreClient::Responder &respond) {
            recordRestartDuration();
            logSystem(QStringLiteral("=== Restarting now (rebuild & restart) ==="));
            respond({}, QString());
            QTimer::singleShot(200, qApp, &QCoreApplication::quit);
        });
    connect(m_coreClient, &CoreClient::connectedChanged, this,
            [this](bool connected) { onCoreOpsConnected(connected); });
    connect(m_coreClient, &CoreClient::coreEvent, this,
            [this](const QString &event, const QJsonObject &data) {
                if (event.startsWith(QLatin1String("core.update.")) ||
                    event.startsWith(QLatin1String("core.backups.")))
                    onCoreOpsEvent(event, data);
            });
}

void MainWindow::pushCoreUpdateConfig()
{
    if (!coreOpsAttached())
        return;
    const QString self = QCoreApplication::applicationFilePath();
    m_coreClient->request(
        QStringLiteral("core.update.config"),
        {{QStringLiteral("clientDir"), workingClientDir()},
         {QStringLiteral("appPath"), self},
         {QStringLiteral("selfPath"), self},
         {QStringLiteral("args"), jsonStrings(QCoreApplication::arguments().mid(1))},
         {QStringLiteral("cwd"), QDir::currentPath()},
         {QStringLiteral("unit"), supervisingSystemdUserServiceUnit()},
         {QStringLiteral("buildType"), QStringLiteral("RelWithDebInfo")},
         {QStringLiteral("version"), QStringLiteral(FORKMESH_VERSION)},
         {QStringLiteral("startedAtMs"), double(kCoreOpsProcessStartedMs)},
         {QStringLiteral("pid"), double(QCoreApplication::applicationPid())},
         {QStringLiteral("autoUpdateDir"), runningClientDir()}},
        [this](bool ok, const QJsonValue &result, const QString &) {
            coreops::State *ops = coreops::state(this);
            ops->capabilities.clear();
            if (!ok)
                return;
            for (const QJsonValue &value :
                 result.toObject().value(QStringLiteral("capabilities")).toArray())
                ops->capabilities << value.toString();
        });
}

void MainWindow::onCoreOpsConnected(bool connected)
{
    coreops::State *ops = coreops::state(this);
    if (connected) {
        if (!coreOpsAttached())
            return;
        ops->lastDesktopState = {};
        pushCoreUpdateConfig();
        pushCoreUpdateDesktopState(restartCountdownText());
        m_coreClient->request(QStringLiteral("core.update.state"), {},
                              [this](bool ok, const QJsonValue &result, const QString &) {
                                  if (ok)
                                      reattachCoreRebuild(result.toObject());
                              });
        if (hasRoute(m_coreClient, QStringLiteral("core.backups.config")))
            startAutoBackups();
        return;
    }
    ops->capabilities.clear();
    ops->prebuilt = false;
    if (ops->runActive) {
        ops->runActive = false;
        stopRefreshSpin();
        stopRestartSpin();
        setUpdateStatus(QStringLiteral("The background engine stopped during the rebuild; this "
                                       "desktop was not replaced."),
                        true);
        if (m_buildButton)
            m_buildButton->setEnabled(true);
        if (m_rebuildButton)
            m_rebuildButton->setEnabled(true);
    }
    if (ops->unpacking || ops->exporting) {
        ops->unpacking = false;
        ops->exporting = false;
        setConfigArchiveBusy(false);
        setDataStatus(QStringLiteral("The background engine stopped while it archived the "
                                     "configuration; try again."),
                      true);
    }
    if (ops->backupRunning) {
        ops->backupRunning = false;
        if (m_backupNowButton)
            m_backupNowButton->setEnabled(true);
    }
    if (!m_closingDown)
        startAutoBackups();
}

void MainWindow::reattachCoreRebuild(const QJsonObject &state)
{
    coreops::State *ops = coreops::state(this);
    if (!state.value(QStringLiteral("job")).toBool() || ops->runActive)
        return;
    ops->runActive = true;
    ops->cancelled = false;
    ops->run = state.value(QStringLiteral("run")).toInt(-1);
    ops->phase.clear();
    ops->pass = -1;
    ops->percent = -1;
    QPushButton *button = m_navRebuildButton       ? m_navRebuildButton
                          : m_profileRebuildButton ? m_profileRebuildButton
                                                   : m_rebuildButton;
    if (button && !m_restartSpinButton)
        startRestartSpin(button, state.value(QStringLiteral("kind")).toString() ==
                                         QLatin1String("update")
                                     ? RestartKind::Update
                                     : RestartKind::Rebuild);
    m_buildButton = m_rebuildButton;
    m_buildStatusLabel = m_rebuildStatus;
    if (m_rebuildButton)
        m_rebuildButton->setEnabled(false);
    ops->echo = true;
    showUpdateLog();
    QString backlog;
    for (const QJsonValue &line : state.value(QStringLiteral("lines")).toArray())
        backlog += line.toString() + QLatin1Char('\n');
    appendUpdateLog(backlog);
    ops->echo = false;
    logSystem(QStringLiteral("A rebuild is still running in the background engine; following it."));
    onCoreOpsEvent(QStringLiteral("core.update.state"), state);
    const QString status = state.value(QStringLiteral("status")).toString();
    if (!status.isEmpty())
        applyCoreUpdateStatus(status, status == state.value(QStringLiteral("error")).toString());
}

void MainWindow::applyCoreUpdateStatus(const QString &status, bool isError)
{
    m_updateFeed.noteStatus(status, isError);
    if (isError && m_updateLogDialog) {
        m_updateLogDialog->show();
        m_updateLogDialog->raise();
        m_updateLogDialog->activateWindow();
    }
    QLabel *label = m_buildStatusLabel ? m_buildStatusLabel : m_updateStatus;
    if (!label)
        return;
    label->setStyleSheet(isError ? "color:#ff6b6b; background:transparent;"
                                 : "color:#9ca3af; background:transparent;");
    label->setText(status);
    label->show();
}

QJsonObject MainWindow::coreRelaunchRequest(const QString &kind, const QString &clientDir,
                                            const QString &relaunchPath,
                                            const QString &buildType) const
{
    const QString self = QCoreApplication::applicationFilePath();
    return {{QStringLiteral("kind"), kind},
            {QStringLiteral("label"),
             m_restartKind == RestartKind::Update ? QStringLiteral("update")
                                                  : QStringLiteral("rebuild")},
            {QStringLiteral("clientDir"), clientDir},
            {QStringLiteral("buildType"), buildType},
            {QStringLiteral("appPath"), relaunchPath.isEmpty() ? self : relaunchPath},
            {QStringLiteral("selfPath"), self},
            {QStringLiteral("args"), jsonStrings(QCoreApplication::arguments().mid(1))},
            {QStringLiteral("cwd"), QDir::currentPath()},
            {QStringLiteral("unit"), supervisingSystemdUserServiceUnit()}};
}

bool MainWindow::startCoreJob(const QJsonObject &request, std::function<void()> fallback)
{
    coreops::State *ops = coreops::state(this);
    if (ops->bypass) {
        ops->bypass = false;
        return false;
    }
    if (!coreOpsAttached())
        return false;
    ops->runActive = true;
    ops->cancelled = false;
    ops->phase.clear();
    ops->pass = -1;
    ops->percent = -1;
    m_updateAsUser = request.value(QStringLiteral("asUser")).toString();
    logRestart(QStringLiteral("handing the %1 to the background engine")
                   .arg(request.value(QStringLiteral("kind")).toString()));
    m_coreClient->request(
        QStringLiteral("core.update.start"), request,
        [this, fallback](bool ok, const QJsonValue &result, const QString &error) {
            coreops::State *ops = coreops::state(this);
            if (ok) {
                ops->run = result.toObject().value(QStringLiteral("run")).toInt(-1);
                return;
            }
            ops->runActive = false;
            if (error == QLatin1String("already_running")) {
                stopRefreshSpin();
                stopRestartSpin();
                setUpdateStatus(QStringLiteral("A rebuild is already running in the background "
                                               "engine; wait for it to finish."),
                                true);
                if (m_buildButton)
                    m_buildButton->setEnabled(true);
                if (m_rebuildButton)
                    m_rebuildButton->setEnabled(true);
                return;
            }
            logRestart(QStringLiteral("the background engine refused the job (%1); running it "
                                      "in the desktop")
                           .arg(error));
            ops->bypass = true;
            fallback();
        },
        30000);
    return true;
}

bool MainWindow::startCoreRebuild(const QString &clientDir, const QString &asUser,
                                  const QString &relaunchPath, const QString &buildType)
{
    if (!asUser.isEmpty() && !coreops::can(this, QStringLiteral("asUser")))
        return false;
    QJsonObject request =
        coreRelaunchRequest(QStringLiteral("rebuild"), clientDir, relaunchPath, buildType);
    if (!asUser.isEmpty())
        request.insert(QStringLiteral("asUser"), asUser);
    return startCoreJob(request, [this, clientDir, asUser, relaunchPath, buildType] {
        buildAndRelaunch(clientDir, asUser, relaunchPath, buildType);
    });
}

bool MainWindow::startCoreUpdate(const QString &installUrl, const QString &repoDir,
                                 const QString &clientDir, const QString &relaunchPath)
{
    const bool lookup = installUrl.isEmpty();
    if (lookup != coreops::can(this, QStringLiteral("installSource")))
        return false;
    const QString asUser = m_updateAsUser;
    if (!asUser.isEmpty() && !coreops::can(this, QStringLiteral("asUser")))
        return false;
    QJsonObject request = coreRelaunchRequest(QStringLiteral("update"), clientDir, relaunchPath,
                                              QStringLiteral("Release"));
    if (lookup)
        request.insert(QStringLiteral("catalogUrl"), catalogApiUrl().toString());
    else
        request.insert(QStringLiteral("installUrl"), installUrl);
    request.insert(QStringLiteral("repoDir"), repoDir);
    if (!asUser.isEmpty())
        request.insert(QStringLiteral("asUser"), asUser);
    return startCoreJob(request, [this] { updateRebuildRestart(); });
}

bool MainWindow::startCoreCleanRebuild(const QString &clientDir)
{
    if (!coreOpsAttached() || !coreops::can(this, QStringLiteral("clean")))
        return false;
    QJsonObject request = coreRelaunchRequest(QStringLiteral("rebuild"), clientDir, QString(),
                                              QStringLiteral("Release"));
    request.insert(QStringLiteral("clean"), true);
    return startCoreJob(request, [this] { rebuildAndRelaunch(); });
}

bool MainWindow::startCorePrebuilt(const QString &clientDir, const QString &tag,
                                   const QString &tagCommit)
{
    coreops::State *ops = coreops::state(this);
    if (std::exchange(ops->prebuiltBypass, false))
        return false;
    if (!coreOpsAttached() || !coreops::can(this, QStringLiteral("prebuilt")))
        return false;
#if defined(Q_OS_LINUX)
    const QString wantOs = QStringLiteral("linux");
#elif defined(Q_OS_MACOS)
    const QString wantOs = QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    const QString wantOs = QStringLiteral("windows");
#else
    return false;
#endif
    QJsonArray mirrors;
    for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
        if (repo.previewOnly || repo.mirrorPath.trimmed().isEmpty())
            continue;
        mirrors.append(QJsonObject{
            {QStringLiteral("identity"), repoSegment(repo.owner, QStringLiteral("owner")) +
                                             QLatin1Char('/') +
                                             repoSegment(repo.name, QStringLiteral("repository"))},
            {QStringLiteral("casRoot"), mirrorReleaseCasRoot(repo.mirrorPath)}});
    }
    QJsonObject request = coreRelaunchRequest(QStringLiteral("prebuilt"), clientDir,
                                              runningClientExecutable(), QStringLiteral("Release"));
    request.insert(QStringLiteral("label"), QStringLiteral("update"));
    request.insert(QStringLiteral("tag"), tag);
    request.insert(QStringLiteral("tagCommit"), tagCommit);
    request.insert(QStringLiteral("os"), wantOs);
    request.insert(QStringLiteral("arch"), QSysInfo::currentCpuArchitecture());
    request.insert(QStringLiteral("catalogUrl"), catalogApiUrl().toString());
    request.insert(QStringLiteral("mirrors"), mirrors);
    request.insert(QStringLiteral("tempDir"),
                   QStandardPaths::writableLocation(QStandardPaths::TempLocation));
    ops->runActive = true;
    ops->prebuilt = true;
    ops->cancelled = false;
    ops->phase.clear();
    ops->pass = -1;
    ops->percent = -1;
    m_coreClient->request(
        QStringLiteral("core.update.prebuilt"), request,
        [this, clientDir, tag, tagCommit](bool ok, const QJsonValue &result,
                                          const QString &error) {
            coreops::State *ops = coreops::state(this);
            const QJsonObject reply = result.toObject();
            if (ok && reply.value(QStringLiteral("started")).toBool()) {
                ops->run = reply.value(QStringLiteral("run")).toInt(-1);
                return;
            }
            ops->runActive = false;
            ops->prebuilt = false;
            if (ok) {
                if (!reply.value(QStringLiteral("handled")).toBool())
                    updateRebuildRestart();
                return;
            }
            if (error == QLatin1String("already_running"))
                return;
            logRestart(QStringLiteral("the background engine refused the prebuilt update (%1); "
                                      "checking it in the desktop")
                           .arg(error));
            ops->prebuiltBypass = true;
            if (!tryPrebuiltAutoUpdate(clientDir, tag, tagCommit))
                updateRebuildRestart();
        },
        kLongCoreRequestMs);
    return true;
}

void MainWindow::cancelCoreRebuild()
{
    coreops::State *ops = coreops::peek(this);
    if (!ops || !ops->runActive)
        return;
    ops->runActive = false;
    ops->cancelled = true;
    if (m_coreClient && m_coreClient->isConnected())
        m_coreClient->request(QStringLiteral("core.update.cancel"));
}

void MainWindow::pushCoreUpdateDesktopState(const QString &activity)
{
    coreops::State *ops = coreops::peek(this);
    if (!ops || !coreOpsAttached())
        return;
    const bool running = restartInProgress();
    QString kind;
    if (m_restartSpinButton)
        kind = m_restartKind == RestartKind::Update ? QStringLiteral("update")
                                                    : QStringLiteral("rebuild");
    else if (running)
        kind = QStringLiteral("rebuild");
    const QJsonObject state = coreops::desktopState(running, kind, m_restartProgress,
                                                    m_restartPhase, activity);
    if (state == ops->lastDesktopState)
        return;
    ops->lastDesktopState = state;
    m_coreClient->notify(QStringLiteral("core.update.desktop"), state);
}

void MainWindow::forwardCoreUpdateLog(const QString &text)
{
    const coreops::State *ops = coreops::peek(this);
    if (!ops || ops->echo || text.isEmpty() || !coreOpsAttached())
        return;
    m_coreClient->notify(QStringLiteral("core.update.append"), {{QStringLiteral("text"), text}});
}

void MainWindow::forwardCoreUpdateStatus(const QString &status, bool isError)
{
    const coreops::State *ops = coreops::peek(this);
    if (!ops || ops->echo || !coreOpsAttached())
        return;
    m_coreClient->notify(QStringLiteral("core.update.note"),
                         {{QStringLiteral("status"), status}, {QStringLiteral("error"), isError}});
}

void MainWindow::forwardCoreUpdateReset()
{
    const coreops::State *ops = coreops::peek(this);
    if (!ops || ops->echo || !coreOpsAttached())
        return;
    m_coreClient->notify(QStringLiteral("core.update.reset"));
}

void MainWindow::onCoreOpsEvent(const QString &event, const QJsonObject &data)
{
    coreops::State *ops = coreops::state(this);
    if (event == QLatin1String("core.update.started")) {
        if (!ops->runActive)
            reattachCoreRebuild(QJsonObject{{QStringLiteral("job"), true},
                                            {QStringLiteral("run"), data.value(QStringLiteral("run"))},
                                            {QStringLiteral("kind"), data.value(QStringLiteral("kind"))}});
        ops->run = data.value(QStringLiteral("run")).toInt(ops->run);
        return;
    }
    if (event == QLatin1String("core.update.output")) {
        if (!ops->runActive || ops->cancelled)
            return;
        const QString text = data.value(QStringLiteral("text")).toString();
        ops->echo = true;
        appendUpdateLog(text);
        ops->echo = false;
        noteRestartStepOutput(text);
        return;
    }
    if (event == QLatin1String("core.update.log")) {
        if (ops->runActive && !ops->cancelled)
            logRestart(data.value(QStringLiteral("text")).toString());
        return;
    }
    if (event == QLatin1String("core.update.state")) {
        if (!ops->runActive || ops->cancelled)
            return;
        const QString phase = data.value(QStringLiteral("phase")).toString();
        const int pass = data.value(QStringLiteral("pass")).toInt();
        if (phase != ops->phase || pass != ops->pass) {
            ops->phase = phase;
            ops->pass = pass;
            if (!phase.isEmpty())
                setRestartPhase(phase, pass);
            if (ops->prebuilt && phase == QLatin1String("installing"))
                startRestartCautionFlash();
        }
        const int percent = data.value(QStringLiteral("percent")).toInt();
        if (percent != ops->percent) {
            ops->percent = percent;
            if (percent > 0)
                setRestartSpinProgress(percent);
        }
        if (data.value(QStringLiteral("statusChanged")).toBool())
            applyCoreUpdateStatus(data.value(QStringLiteral("statusText")).toString(),
                                  data.value(QStringLiteral("statusError")).toBool());
        return;
    }
    if (event == QLatin1String("core.update.finished")) {
        const bool wasActive = ops->runActive;
        ops->runActive = false;
        ops->prebuilt = false;
        if (ops->cancelled || data.value(QStringLiteral("cancelled")).toBool()) {
            ops->cancelled = false;
            return;
        }
        if (data.value(QStringLiteral("fallback")).toString() == QLatin1String("source")) {
            if (!m_closingDown)
                QTimer::singleShot(0, this, [this] { updateRebuildRestart(); });
            return;
        }
        if (!wasActive || data.value(QStringLiteral("ok")).toBool())
            return;
        if (const QString flash = data.value(QStringLiteral("flash")).toString(); !flash.isEmpty())
            flashMessage(flash, true);
        m_updateAsUser.clear();
        stopRefreshSpin();
        stopRestartSpin();
        if (m_buildButton)
            m_buildButton->setEnabled(true);
        if (m_rebuildButton)
            m_rebuildButton->setEnabled(true);
        return;
    }
    if (event == QLatin1String("core.backups.status")) {
        setBackupStatus(data.value(QStringLiteral("text")).toString(),
                        data.value(QStringLiteral("error")).toBool());
        return;
    }
    if (event == QLatin1String("core.backups.changed")) {
        const bool running = data.value(QStringLiteral("running")).toBool();
        ops->backupRunning = running;
        if (m_backupNowButton)
            m_backupNowButton->setEnabled(!running);
        const qint64 failureAt = qint64(data.value(QStringLiteral("failureAtMs")).toDouble());
        if (failureAt != ops->failureAtMs && failureAt > 0) {
            ops->failureAtMs = failureAt;
            m_lastBackupFailure = data.value(QStringLiteral("failure")).toString();
            m_lastBackupResultMs = failureAt;
        }
        if (!running)
            refreshBackupTable();
        return;
    }
    if (event == QLatin1String("core.backups.unpacked")) {
        if (!ops->unpacking)
            return;
        ops->unpacking = false;
        setConfigArchiveBusy(false);
        if (data.value(QStringLiteral("ok")).toBool()) {
            finishConfigRestore(data.value(QStringLiteral("workRoot")).toString(),
                                data.value(QStringLiteral("stamp")).toString());
            return;
        }
        const QString why = data.value(QStringLiteral("error")).toString();
        setDataStatus("Restore failed to unpack the archive" + (why.isEmpty() ? QString() : ": " + why),
                      true);
        return;
    }
    if (event == QLatin1String("core.backups.exported")) {
        if (!ops->exporting)
            return;
        ops->exporting = false;
        setConfigArchiveBusy(false);
        const QString out = data.value(QStringLiteral("out")).toString();
        if (data.value(QStringLiteral("ok")).toBool()) {
            setDataStatus("Exported configuration to " + out + " (" +
                          QLocale().formattedDataSize(
                              qint64(data.value(QStringLiteral("bytes")).toDouble())) +
                          ").");
            return;
        }
        const QString why = data.value(QStringLiteral("error")).toString();
        setDataStatus("Export failed" + (why.isEmpty() ? QString() : ": " + why), true);
    }
}

QJsonObject MainWindow::coreBackupConfig(bool arm) const
{
    return {{QStringLiteral("enabled"), autoBackupEnabled()},
            {QStringLiteral("keep"), backupKeepCount()},
            {QStringLiteral("root"), backupRoot()},
            {QStringLiteral("dataRoot"), forkmesh::paths::dataRoot()},
            {QStringLiteral("settingsFile"), QSettings().fileName()},
            {QStringLiteral("excludes"),
             jsonStrings({repositoryMirrorRoot(), repositoryPreviewRoot(), backupRoot()})},
            {QStringLiteral("arm"), arm}};
}

bool MainWindow::coreConfigureBackups(bool arm)
{
    if (m_closingDown || !hasRoute(m_coreClient, QStringLiteral("core.backups.config")))
        return false;
    m_coreClient->request(QStringLiteral("core.backups.config"), coreBackupConfig(arm));
    return true;
}

bool MainWindow::coreTakeBackup(bool automatic)
{
    if (m_closingDown || !hasRoute(m_coreClient, QStringLiteral("core.backups.now")))
        return false;
    if (automatic)
        return true;
    m_coreClient->request(QStringLiteral("core.backups.now"),
                          {{QStringLiteral("automatic"), false},
                           {QStringLiteral("config"), coreBackupConfig(false)}},
                          [this](bool ok, const QJsonValue &, const QString &error) {
                              if (!ok)
                                  setBackupStatus(QStringLiteral("Backup failed: ") + error, true);
                          });
    return true;
}

bool MainWindow::corePruneBackups()
{
    if (m_closingDown || !hasRoute(m_coreClient, QStringLiteral("core.backups.prune")))
        return false;
    m_coreClient->request(QStringLiteral("core.backups.config"), coreBackupConfig(false));
    m_coreClient->request(QStringLiteral("core.backups.prune"),
                          {{QStringLiteral("keep"), backupKeepCount()}},
                          [this](bool ok, const QJsonValue &, const QString &) {
                              if (ok)
                                  refreshBackupTable();
                          });
    return true;
}

void MainWindow::cancelCoreBackups()
{
    if (!hasRoute(m_coreClient, QStringLiteral("core.backups.cancel")))
        return;
    QJsonObject config = coreBackupConfig(true);
    config.insert(QStringLiteral("enabled"), false);
    m_coreClient->request(QStringLiteral("core.backups.config"), config);
    m_coreClient->request(QStringLiteral("core.backups.cancel"));
}

bool MainWindow::coreUnpackArchive(const QString &archive, const QString &workRoot,
                                   const QString &stamp)
{
    if (m_closingDown || !hasRoute(m_coreClient, QStringLiteral("core.backups.unpack")))
        return false;
    coreops::State *ops = coreops::state(this);
    ops->unpacking = true;
    setConfigArchiveBusy(true);
    setDataStatus("Unpacking the backup\xE2\x80\xA6 this can take a few minutes.");
    m_coreClient->request(
        QStringLiteral("core.backups.unpack"),
        {{QStringLiteral("archive"), archive},
         {QStringLiteral("workRoot"), workRoot},
         {QStringLiteral("stamp"), stamp}},
        [this, workRoot](bool ok, const QJsonValue &, const QString &error) {
            if (ok)
                return;
            coreops::State *ops = coreops::state(this);
            ops->unpacking = false;
            setConfigArchiveBusy(false);
            forkmesh::safedelete::removeTree(workRoot, QFileInfo(workRoot).absolutePath());
            setDataStatus("Restore failed to unpack the archive: " + error, true);
        },
        kLongCoreRequestMs);
    return true;
}

bool MainWindow::coreExportArchive(const QString &out)
{
    if (m_closingDown || !hasRoute(m_coreClient, QStringLiteral("core.backups.export")))
        return false;
    coreops::State *ops = coreops::state(this);
    ops->exporting = true;
    setConfigArchiveBusy(true);
    setDataStatus("Exporting configuration to " + out +
                  "\xE2\x80\xA6 a large database can take a few minutes; "
                  "ForkMesh stays usable while it runs.");
    m_coreClient->request(
        QStringLiteral("core.backups.export"),
        {{QStringLiteral("out"), out}, {QStringLiteral("config"), coreBackupConfig(false)}},
        [this](bool ok, const QJsonValue &, const QString &error) {
            if (ok)
                return;
            coreops::State *ops = coreops::state(this);
            ops->exporting = false;
            setConfigArchiveBusy(false);
            setDataStatus("Export failed: " + error, true);
        },
        kLongCoreRequestMs);
    return true;
}
