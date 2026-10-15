#include "ControlNode.h"
#include "CoreClient.h"
#include "CoreNodeLogs.h"
#include "ForkMeshVersion.h"
#include "LocalControlFleet.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSysInfo>
#include <QTimer>
#include <QVBoxLayout>

#include <functional>

using namespace forkmesh::ui;

namespace {

constexpr int kCoreWaitForResumeMs = 20000;
constexpr int kCoreWaitStepMs = 1000;
constexpr int kFinishedJobMemory = 64;

struct CoreHostJobsState {
    QHash<QString, qint64> channelEnd;
    QHash<QString, qint64> channelEpoch;
    QHash<QString, std::function<void(const QJsonObject &)>> waiting;
    QHash<QString, QJsonObject> finished;
    QStringList finishedOrder;
    QSet<QString> installJobs;
    QHash<QString, int> deployChannels;
    QString deployId;
    bool deployRelease = false;
    int resumeWaitedMs = 0;
    bool provisionMirrored = false;
};

CoreHostJobsState &jobsState()
{
    static CoreHostJobsState state;
    return state;
}

void rememberFinished(const QString &id, const QJsonObject &job)
{
    CoreHostJobsState &state = jobsState();
    state.finished.insert(id, job);
    state.finishedOrder.append(id);
    while (state.finishedOrder.size() > kFinishedJobMemory)
        state.finished.remove(state.finishedOrder.takeFirst());
}

void awaitCoreJob(const QString &id, std::function<void(const QJsonObject &)> callback)
{
    if (id.isEmpty() || !callback)
        return;
    CoreHostJobsState &state = jobsState();
    if (state.finished.contains(id)) {
        callback(state.finished.value(id));
        return;
    }
    state.waiting.insert(id, std::move(callback));
}

bool installKind(const QString &kind)
{
    return kind == QLatin1String("install") || kind == QLatin1String("uninstall");
}

}

bool MainWindow::coreHostJobsAttached() const
{
    return m_coreClient && m_coreClient->isConnected() &&
           m_coreClient->hasNativeRoute(QStringLiteral("core.hosts.install"));
}

bool MainWindow::coreHostInstallBusy() const
{
    return !jobsState().installJobs.isEmpty();
}

QJsonObject MainWindow::coreHostsEnvironment()
{
    QUrl nodeDelete = catalogApiUrl();
    nodeDelete.setPath(QStringLiteral("/api/world/admin/nodes/delete"));
    nodeDelete.setQuery(QString());
    QString linkUser;
    const QString signer = hostLinkSigningAccountName(&linkUser);
    QJsonArray fleetAddresses;
    for (const forkmesh::corelogs::CoreNode &node : forkmesh::corelogs::loadFleetNodes())
        fleetAddresses.append(node.address);
    const forkmesh::control::AgentCliCredentials credentials = localAgentCliCredentials();
    return QJsonObject{
        {QStringLiteral("installUrl"), installScriptUrl()},
        {QStringLiteral("uninstallUrl"), uninstallScriptUrl()},
        {QStringLiteral("version"), QStringLiteral(FORKMESH_VERSION)},
        {QStringLiteral("buildCommit"), hostInstallBuildCommit()},
        {QStringLiteral("sourceDir"), QStringLiteral(FORKMESH_SOURCE_DIR)},
        {QStringLiteral("appBinary"), QCoreApplication::applicationFilePath()},
        {QStringLiteral("kernelType"), QSysInfo::kernelType()},
        {QStringLiteral("cpuArch"), QSysInfo::currentCpuArchitecture()},
        {QStringLiteral("catalogUrl"), catalogApiUrl().toString()},
        {QStringLiteral("accountsLinkUrl"), accountsApiUrl(QStringLiteral("link-node")).toString()},
        {QStringLiteral("nodeDeleteUrl"), nodeDelete.toString()},
        {QStringLiteral("profilePublicKey"), m_profileIdentity.publicKey()},
        {QStringLiteral("linkSigner"), signer},
        {QStringLiteral("linkUser"), hostLinkUserName()},
        {QStringLiteral("accountOwner"), accountOwner()},
        {QStringLiteral("canSign"),
         m_profileIdentity.isValid() && hasOwnerSigningCapability(accountOwner())},
        {QStringLiteral("sessionToken"), accountSessionTokenForUrl(nodeDelete)},
        {QStringLiteral("admin"), m_isAdmin},
        {QStringLiteral("linkedNodes"), QJsonArray::fromStringList(m_profileLinkedNodes)},
        {QStringLiteral("fleetAddresses"), fleetAddresses},
        {QStringLiteral("mirrorRoot"), repositoryMirrorRoot()},
        {QStringLiteral("manifestDigestEnv"),
         qEnvironmentVariable("FORKMESH_EXPECTED_RELEASE_MANIFEST_SHA256")},
        {QStringLiteral("mirrorPackage"), hostInstallMirrorPackage()},
        {QStringLiteral("provisionLog"), vultrProvisionLogPath()},
        {QStringLiteral("agentCredentials"),
         !forkmesh::control::agentCliCredentialsAreEmpty(credentials)},
        {QStringLiteral("agentAccess"), forkmesh::control::describeAgentCliCredentials(credentials)},
        {QStringLiteral("formVultrKey"),
         m_vultrApiKeyEdit ? m_vultrApiKeyEdit->text().trimmed() : QString()},
        {QStringLiteral("uploadChecked"),
         m_hostUploadBinaryCheck && m_hostUploadBinaryCheck->isChecked()},
        {QStringLiteral("deployEnvPath"),
         forkmesh::control::siteDeployEnvFilePath(QStringLiteral(FORKMESH_SOURCE_DIR),
                                                 QCoreApplication::applicationDirPath())},
    };
}

void MainWindow::registerCoreHostJobHandlers()
{
    if (!m_coreClient)
        return;
    m_coreClient->handle(QStringLiteral("ui.hosts.environment"),
                         [this](const QJsonObject &, const CoreClient::Responder &respond) {
                             respond(coreHostsEnvironment(), QString());
                         });
    m_coreClient->handle(
        QStringLiteral("ui.hosts.clearReleaseRollout"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            clearReleaseRollout(params.value(QStringLiteral("owner")).toString(),
                                params.value(QStringLiteral("name")).toString(),
                                params.value(QStringLiteral("tag")).toString());
            respond({}, QString());
        });
    registerCoreMirrorPushHandlers();
    connect(m_coreClient, &CoreClient::connectedChanged, this,
            &MainWindow::onCoreHostJobsConnected);
    connect(m_coreClient, &CoreClient::coreEvent, this, &MainWindow::onCoreHostJobsEvent);
}

void MainWindow::onCoreHostJobsConnected(bool connected)
{
    CoreHostJobsState &state = jobsState();
    if (!connected) {
        const auto waiting = state.waiting;
        state.waiting.clear();
        for (auto it = waiting.cbegin(); it != waiting.cend(); ++it)
            it.value()(QJsonObject{{QStringLiteral("id"), it.key()},
                                   {QStringLiteral("ok"), false},
                                   {QStringLiteral("running"), false}});
        state.installJobs.clear();
        state.channelEnd.clear();
        state.channelEpoch.clear();
        if (m_hostInstallButton)
            m_hostInstallButton->setEnabled(true);
        if (!state.deployId.isEmpty()) {
            state.deployId.clear();
            m_hostDeployRemaining = 0;
        }
        if (state.provisionMirrored) {
            state.provisionMirrored = false;
            m_vultrProvisionActive = false;
            updateVultrEndDeploymentButtonVisibility();
        }
        detachCoreHostFeeds();
        m_hostInstallDeploying = false;
        updateNetworkDeployBlink();
        return;
    }
    if (!coreHostJobsAttached())
        return;
    m_coreClient->request(QStringLiteral("core.hosts.environment"), coreHostsEnvironment());
    m_coreClient->request(
        QStringLiteral("core.hosts.jobs"), {},
        [this](bool ok, const QJsonValue &value, const QString &) {
            if (!ok)
                return;
            const QJsonObject result = value.toObject();
            CoreHostJobsState &state = jobsState();
            for (const QJsonValue &entry : result.value(QStringLiteral("jobs")).toArray()) {
                const QJsonObject job = entry.toObject();
                if (job.value(QStringLiteral("running")).toBool() &&
                    installKind(job.value(QStringLiteral("kind")).toString()))
                    state.installJobs.insert(job.value(QStringLiteral("id")).toString());
            }
            const QJsonObject statuses = result.value(QStringLiteral("statuses")).toObject();
            if (m_hostInstallStatus && statuses.contains(QStringLiteral("install")))
                m_hostInstallStatus->setText(statuses.value(QStringLiteral("install")).toString());
            if (m_vultrStatus && statuses.contains(QStringLiteral("vultr")))
                m_vultrStatus->setText(statuses.value(QStringLiteral("vultr")).toString());
            const QJsonObject install = result.value(QStringLiteral("channels"))
                                            .toObject()
                                            .value(QStringLiteral("install"))
                                            .toObject();
            if (!install.isEmpty()) {
                const qint64 start = qint64(install.value(QStringLiteral("start")).toDouble());
                state.channelEpoch.insert(QStringLiteral("install"),
                                          qint64(install.value(QStringLiteral("epoch")).toDouble()));
                state.channelEnd.insert(QStringLiteral("install"), start);
                if (m_hostInstallLog) {
                    m_hostInstallLog->clear();
                    m_hostInstallLogCarry.clear();
                    m_hostInstallLogFg = -1;
                    m_hostInstallLogBold = false;
                }
                m_coreClient->request(
                    QStringLiteral("core.hosts.transcript"),
                    {{QStringLiteral("channel"), QStringLiteral("install")},
                     {QStringLiteral("from"), double(start)}},
                    [this](bool fetched, const QJsonValue &text, const QString &) {
                        if (!fetched)
                            return;
                        onCoreHostJobsEvent(QStringLiteral("core.hosts.output"), text.toObject());
                    });
            }
            if (m_hostInstallButton)
                m_hostInstallButton->setEnabled(state.installJobs.isEmpty());
            m_hostInstallDeploying = !state.installJobs.isEmpty();
            const QJsonObject deploy = result.value(QStringLiteral("deploy")).toObject();
            if (deploy.value(QStringLiteral("running")).toBool())
                applyCoreDeploySnapshot(deploy);
            const QJsonObject provision = result.value(QStringLiteral("provision")).toObject();
            if (!provision.isEmpty())
                applyCoreProvisionSnapshot(provision);
            updateNetworkDeployBlink();
        });
}

void MainWindow::onCoreHostJobsEvent(const QString &event, const QJsonObject &data)
{
    if (!event.startsWith(QLatin1String("core.hosts.")))
        return;
    CoreHostJobsState &state = jobsState();
    if (event == QLatin1String("core.hosts.output")) {
        const QString channel = data.value(QStringLiteral("channel")).toString();
        const qint64 epoch = qint64(data.value(QStringLiteral("epoch")).toDouble());
        qint64 offset = qint64(data.value(QStringLiteral("offset")).toDouble());
        QString text = data.value(QStringLiteral("text")).toString();
        const bool clear = data.value(QStringLiteral("clear")).toBool();
        if (state.channelEpoch.value(channel, -1) != epoch || clear) {
            state.channelEpoch.insert(channel, epoch);
            state.channelEnd.insert(channel, offset);
            if (clear && channel == QLatin1String("install") && m_hostInstallLog) {
                m_hostInstallLog->clear();
                m_hostInstallLogCarry.clear();
                m_hostInstallLogFg = -1;
                m_hostInstallLogBold = false;
            }
        }
        const qint64 expected = state.channelEnd.value(channel, offset);
        const QByteArray bytes = text.toUtf8();
        if (offset > expected && m_coreClient) {
            m_coreClient->request(
                QStringLiteral("core.hosts.transcript"),
                {{QStringLiteral("channel"), channel}, {QStringLiteral("from"), double(expected)}},
                [this](bool ok, const QJsonValue &value, const QString &) {
                    if (ok)
                        onCoreHostJobsEvent(QStringLiteral("core.hosts.output"), value.toObject());
                });
            return;
        }
        QByteArray fresh = bytes;
        if (offset < expected) {
            const qint64 skip = expected - offset;
            if (skip >= bytes.size())
                return;
            fresh = bytes.mid(int(skip));
            offset = expected;
        }
        state.channelEnd.insert(channel, offset + fresh.size());
        if (fresh.isEmpty())
            return;
        text = QString::fromUtf8(fresh);
        if (channel == QLatin1String("install")) {
            appendHostInstallLog(text);
            return;
        }
        const int index = state.deployChannels.value(channel, -1);
        if (index >= 0 && index < m_hostDeploySessions.size())
            appendHostDeployLog(m_hostDeploySessions.at(index), text);
        return;
    }
    if (event == QLatin1String("core.hosts.status")) {
        const QString target = data.value(QStringLiteral("target")).toString();
        const QString text = data.value(QStringLiteral("text")).toString();
        if (target == QLatin1String("install") && m_hostInstallStatus)
            m_hostInstallStatus->setText(text);
        else if (target == QLatin1String("vultr") && m_vultrStatus)
            m_vultrStatus->setText(text);
        else if (target == QLatin1String("nodes") && m_nodesStatus)
            m_nodesStatus->setText(text);
        return;
    }
    if (event == QLatin1String("core.hosts.job")) {
        const QString id = data.value(QStringLiteral("id")).toString();
        const QString kind = data.value(QStringLiteral("kind")).toString();
        const bool running = data.value(QStringLiteral("running")).toBool();
        if (installKind(kind)) {
            if (running)
                state.installJobs.insert(id);
            else
                state.installJobs.remove(id);
            if (m_hostInstallButton)
                m_hostInstallButton->setEnabled(state.installJobs.isEmpty());
            m_hostInstallDeploying = !state.installJobs.isEmpty();
            updateNetworkDeployBlink();
        }
        if (kind == QLatin1String("deleteNode") && running) {
            m_nodeDeleteInProgress = true;
            m_nodeDeleteTarget = data.value(QStringLiteral("host")).toString();
            refreshNodeActionButtons();
        }
        if (running)
            return;
        if (kind == QLatin1String("install"))
            m_hostInstallLastFailure = data.value(QStringLiteral("result"))
                                           .toObject()
                                           .value(QStringLiteral("lastFailure"))
                                           .toString();
        rememberFinished(id, data);
        if (auto callback = state.waiting.take(id))
            callback(data);
        return;
    }
    if (event == QLatin1String("core.hosts.saved")) {
        m_decommissionedNodesLoaded = false;
        m_deletedMeshNodesLoaded = false;
        refreshHostsTable();
        return;
    }
    if (event == QLatin1String("core.hosts.deploy")) {
        applyCoreDeploySnapshot(data);
        return;
    }
    if (event == QLatin1String("core.hosts.provision")) {
        applyCoreProvisionSnapshot(data);
        return;
    }
    if (event == QLatin1String("core.hosts.notify")) {
        addNotification(data.value(QStringLiteral("title")).toString(),
                        data.value(QStringLiteral("body")).toString(),
                        data.value(QStringLiteral("error")).toBool());
        return;
    }
    if (event == QLatin1String("core.hosts.flash")) {
        flashMessage(data.value(QStringLiteral("text")).toString(),
                     data.value(QStringLiteral("error")).toBool());
        return;
    }
    if (event == QLatin1String("core.hosts.nodeDeleted")) {
        const QString node = data.value(QStringLiteral("node")).toString();
        if (!data.value(QStringLiteral("ok")).toBool()) {
            m_nodeDeleteInProgress = false;
            m_nodeDeleteTarget.clear();
            refreshNodeActionButtons();
            return;
        }
        QSet<QString> deleted;
        for (const QJsonValue &value : data.value(QStringLiteral("deletedNames")).toArray())
            deleted.insert(value.toString().trimmed().toLower());
        m_decommissionedNodesLoaded = false;
        m_deletedMeshNodesLoaded = false;
        applyRelayConfirmedNodeDeletion(node, deleted,
                                        data.value(QStringLiteral("alreadyAbsent")).toBool(),
                                        false);
        return;
    }
    if (event == QLatin1String("core.hosts.linkPrompt")) {
        promptHostLinkCode(data.value(QStringLiteral("code")).toString());
        return;
    }
    if (event == QLatin1String("core.hosts.loginTerminal")) {
        openHostAgentLoginTerminal(data.value(QStringLiteral("node")).toString(),
                                   data.value(QStringLiteral("ip")).toString(),
                                   data.value(QStringLiteral("user")).toString());
        return;
    }
    if (event == QLatin1String("core.hosts.probeRequested")) {
        QTimer::singleShot(0, this, &MainWindow::probeSavedHosts);
        return;
    }
    if (event == QLatin1String("core.hosts.feedLines") ||
        event == QLatin1String("core.hosts.feedEnded")) {
        onCoreHostFeedEvent(event, data);
        return;
    }
}

void MainWindow::applyCoreDeploySnapshot(const QJsonObject &deploy)
{
    CoreHostJobsState &state = jobsState();
    const QString id = deploy.value(QStringLiteral("id")).toString();
    const QJsonArray sessions = deploy.value(QStringLiteral("sessions")).toArray();
    const bool running = deploy.value(QStringLiteral("running")).toBool();
    if (id.isEmpty())
        return;
    if (id != state.deployId) {
        state.deployId = id;
        state.deployChannels.clear();
        qDeleteAll(m_hostDeploySessions);
        m_hostDeploySessions.clear();
        if (m_hostDeployGrid) {
            while (QLayoutItem *item = m_hostDeployGrid->takeAt(0)) {
                if (QWidget *w = item->widget())
                    w->deleteLater();
                delete item;
            }
        }
        int cols = 1;
        while (cols * cols < sessions.size())
            ++cols;
        for (int idx = 0; idx < sessions.size(); ++idx) {
            const QJsonObject entry = sessions.at(idx).toObject();
            QLabel *header = nullptr;
            QPlainTextEdit *log = nullptr;
            if (m_hostDeployGrid) {
                auto *card = new QFrame;
                card->setObjectName("leaderboardCard");
                card->setFrameShape(QFrame::StyledPanel);
                auto *col = new QVBoxLayout(card);
                col->setContentsMargins(10, 8, 10, 8);
                col->setSpacing(6);
                header = new QLabel(entry.value(QStringLiteral("header")).toString());
                QFont hf = header->font();
                hf.setBold(true);
                header->setFont(hf);
                header->setWordWrap(true);
                col->addWidget(header);
                log = new QPlainTextEdit;
                log->setObjectName("actionLog");
                log->setReadOnly(true);
                log->setLineWrapMode(QPlainTextEdit::NoWrap);
                log->setMinimumHeight(200);
                log->setMinimumWidth(260);
                applyLogFont(log);
                col->addWidget(log, 1);
                m_hostDeployGrid->addWidget(card, idx / cols, idx % cols);
            }
            auto *session = new HostDeploySession;
            session->node = entry.value(QStringLiteral("node")).toString();
            session->ip = entry.value(QStringLiteral("ip")).toString();
            session->user = entry.value(QStringLiteral("user")).toString();
            session->log = log;
            session->header = header;
            m_hostDeploySessions.append(session);
            const QString channel = entry.value(QStringLiteral("channel")).toString();
            state.deployChannels.insert(channel, idx);
            state.channelEnd.insert(channel, 0);
            if (m_coreClient)
                m_coreClient->request(
                    QStringLiteral("core.hosts.transcript"),
                    {{QStringLiteral("channel"), channel}, {QStringLiteral("from"), 0}},
                    [this](bool ok, const QJsonValue &value, const QString &) {
                        if (ok)
                            onCoreHostJobsEvent(QStringLiteral("core.hosts.output"), value.toObject());
                    });
        }
        if (m_hostDeployLabel) {
            m_hostDeployLabel->setText(deploy.value(QStringLiteral("label")).toString());
            m_hostDeployLabel->setVisible(true);
        }
        if (m_hostDeployPanel)
            m_hostDeployPanel->setVisible(true);
    }
    for (int idx = 0; idx < sessions.size() && idx < m_hostDeploySessions.size(); ++idx) {
        HostDeploySession *session = m_hostDeploySessions.at(idx);
        const QJsonObject entry = sessions.at(idx).toObject();
        session->finished = entry.value(QStringLiteral("finished")).toBool();
        if (session->header)
            session->header->setText(entry.value(QStringLiteral("header")).toString());
    }
    m_hostDeployRemaining = running ? deploy.value(QStringLiteral("remaining")).toInt() : 0;
    m_hostDeployFailed = deploy.value(QStringLiteral("failed")).toInt();
    for (QPushButton *button : {m_hostInstallAllButton, m_hostReinstallAllButton,
                                m_hostUpdateAllSourceButton, m_hostInstallButton})
        if (button)
            button->setEnabled(!running && (button != m_hostInstallButton ||
                                            state.installJobs.isEmpty()));
    if (!running && state.deployRelease) {
        state.deployRelease = false;
        m_hostDeployReleaseOwner.clear();
        m_hostDeployReleaseName.clear();
        m_hostDeployReleaseTag.clear();
        m_hostDeployReleaseRefsReady = true;
    }
    updateNetworkDeployBlink();
}

void MainWindow::applyCoreProvisionSnapshot(const QJsonObject &snapshot)
{
    CoreHostJobsState &state = jobsState();
    state.provisionMirrored = true;
    const QString previousState = m_vultrProvisionState;
    m_vultrProvisionActive = snapshot.value(QStringLiteral("active")).toBool();
    m_vultrResumeRequested = snapshot.value(QStringLiteral("resumeRequested")).toBool();
    m_vultrProvisionState = snapshot.value(QStringLiteral("state")).toString();
    m_vultrProvisionStage = snapshot.value(QStringLiteral("stage")).toInt();
    m_vultrProvisionDetail = snapshot.value(QStringLiteral("detail")).toString();
    m_vultrProvisionMessage = snapshot.value(QStringLiteral("message")).toString();
    m_vultrProvisionNode = snapshot.value(QStringLiteral("node")).toString();
    m_vultrDnsHostname = snapshot.value(QStringLiteral("dnsHostname")).toString();
    m_vultrInstanceId = snapshot.value(QStringLiteral("instanceId")).toString();
    m_vultrInstanceIp = snapshot.value(QStringLiteral("ip")).toString();
    m_vultrIdentityFile = snapshot.value(QStringLiteral("identityFile")).toString();
    m_vultrInstallAgentClis = snapshot.value(QStringLiteral("installAgentClis")).toBool();
    m_vultrInstallUseLocalBinary = snapshot.value(QStringLiteral("installUseLocalBinary")).toBool();
    m_vultrPollCount = snapshot.value(QStringLiteral("pollCount")).toInt();
    m_vultrInstallAttempts = snapshot.value(QStringLiteral("installAttempts")).toInt();
    m_vultrHostMetadata = snapshot.value(QStringLiteral("hostMetadata")).toObject();
    m_vultrPublicationGap = snapshot.value(QStringLiteral("publicationGap")).toString();
    const QJsonArray details = snapshot.value(QStringLiteral("stageDetails")).toArray();
    for (int i = 0; i < m_vultrStageDetailText.size(); ++i)
        m_vultrStageDetailText[i] = details.at(i).toString();
    const bool failed = m_vultrProvisionState == QLatin1String("failed");
    if (m_vultrCreateButton) {
        if (m_vultrProvisionActive) {
            m_vultrCreateButton->setEnabled(false);
            m_vultrCreateButton->setText(QStringLiteral("Deployment running…"));
        } else {
            m_vultrCreateButton->setEnabled(true);
            m_vultrCreateButton->setText(
                failed ? QStringLiteral("Retry deployment")
                : m_vultrProvisionState == QLatin1String("active")
                    ? QStringLiteral("Resume deployment")
                : m_vultrProvisionState == QLatin1String("succeeded")
                    ? QStringLiteral("Create another mirror")
                    : QStringLiteral("Create Vultr mirror"));
        }
    }
    if (m_vultrNameEdit && m_vultrProvisionActive && !m_vultrProvisionNode.isEmpty())
        m_vultrNameEdit->setText(m_vultrProvisionNode);
    if (m_vultrNameEdit && m_vultrProvisionState == QLatin1String("succeeded") &&
        previousState != m_vultrProvisionState)
        m_vultrNameEdit->clear();
    if (m_vultrAgentClisCheck && m_vultrProvisionActive)
        m_vultrAgentClisCheck->setChecked(m_vultrInstallAgentClis);
    renderVultrProvisionProgress(failed);
    updateVultrEndDeploymentButtonVisibility();
}

bool MainWindow::runHostInstallThroughCore(bool forceUploadBinary,
                                           std::function<void(bool)> onFinished,
                                           bool reinstall, bool fromSource,
                                           bool suppressFailureStatus)
{
    if (!coreHostJobsAttached() || m_vultrProvisionActive)
        return false;
    const QString node = m_hostNameEdit ? m_hostNameEdit->text().trimmed() : QString();
    if (!m_hostRunnerToken.isEmpty() && node.toLower() == m_hostRunnerName && !reinstall &&
        !fromSource)
        return false;
    const QString banner = m_hostInstallAttemptBanner;
    m_hostInstallAttemptBanner.clear();
    if (m_hostInstallProcess && m_hostInstallProcess->state() != QProcess::NotRunning) {
        if (m_hostInstallStatus)
            m_hostInstallStatus->setText(QStringLiteral("An install is already running."));
        if (onFinished)
            onFinished(false);
        return true;
    }
    const QString ip = m_hostIpEdit ? m_hostIpEdit->text().trimmed() : QString();
    const QString user = m_hostUserEdit ? m_hostUserEdit->text().trimmed() : QString();
    const QString pass = m_hostPassEdit ? m_hostPassEdit->text() : QString();
    if (!pass.isEmpty() && !ip.isEmpty() && !user.isEmpty())
        m_hostSessionPasswords.insert(
            forkmesh::control::savedHostCredentialKey(node, ip, user), pass);
    m_coreClient->request(QStringLiteral("core.hosts.environment"), coreHostsEnvironment());
    m_coreClient->request(
        QStringLiteral("core.hosts.install"),
        {{QStringLiteral("node"), node},
         {QStringLiteral("ip"), ip},
         {QStringLiteral("user"), user},
         {QStringLiteral("password"), pass},
         {QStringLiteral("forceUpload"), forceUploadBinary},
         {QStringLiteral("uploadChecked"),
          m_hostUploadBinaryCheck && m_hostUploadBinaryCheck->isChecked()},
         {QStringLiteral("reinstall"), reinstall},
         {QStringLiteral("fromSource"), fromSource},
         {QStringLiteral("suppressFailureStatus"), suppressFailureStatus},
         {QStringLiteral("attemptBanner"), banner}},
        [onFinished](bool ok, const QJsonValue &value, const QString &) {
            const QJsonObject result = value.toObject();
            if (!ok || !result.value(QStringLiteral("started")).toBool()) {
                if (onFinished)
                    onFinished(false);
                return;
            }
            awaitCoreJob(result.value(QStringLiteral("job")).toString(),
                         [onFinished](const QJsonObject &job) {
                             if (onFinished)
                                 onFinished(job.value(QStringLiteral("ok")).toBool());
                         });
        });
    return true;
}

bool MainWindow::runHostUninstallThroughCore()
{
    if (!coreHostJobsAttached())
        return false;
    const QString ip = m_hostIpEdit ? m_hostIpEdit->text().trimmed() : QString();
    const QString user = m_hostUserEdit ? m_hostUserEdit->text().trimmed() : QString();
    const QString pass = m_hostPassEdit ? m_hostPassEdit->text() : QString();
    const QString node = m_hostNameEdit ? m_hostNameEdit->text().trimmed() : QString();
    m_coreClient->request(QStringLiteral("core.hosts.environment"), coreHostsEnvironment());
    m_coreClient->request(QStringLiteral("core.hosts.uninstall"),
                          {{QStringLiteral("node"), node},
                           {QStringLiteral("ip"), ip},
                           {QStringLiteral("user"), user},
                           {QStringLiteral("password"), pass}});
    return true;
}

bool MainWindow::runHostDeployAllThroughCore(int mode, const QJsonObject &release)
{
    if (!coreHostJobsAttached())
        return false;
    QJsonObject passwords;
    for (auto it = m_hostSessionPasswords.cbegin(); it != m_hostSessionPasswords.cend(); ++it)
        passwords.insert(it.key(), it.value());
    QJsonObject payload = release;
    if (!release.isEmpty()) {
        payload.insert(QStringLiteral("owner"), m_hostDeployReleaseOwner);
        payload.insert(QStringLiteral("name"), m_hostDeployReleaseName);
        payload.insert(QStringLiteral("refsReady"), m_hostDeployReleaseRefsReady);
        jobsState().deployRelease = true;
    }
    const QString modeName = mode == int(FleetDeployMode::Reinstall) ? QStringLiteral("reinstall")
                             : mode == int(FleetDeployMode::UpdateSource)
                                 ? QStringLiteral("source")
                                 : QStringLiteral("install");
    m_coreClient->request(QStringLiteral("core.hosts.environment"), coreHostsEnvironment());
    m_coreClient->request(QStringLiteral("core.hosts.deployAll"),
                          {{QStringLiteral("mode"), modeName},
                           {QStringLiteral("release"), payload},
                           {QStringLiteral("passwords"), passwords}},
                          [](bool ok, const QJsonValue &value, const QString &) {
                              if (!ok || !value.toObject().value(QStringLiteral("started")).toBool())
                                  jobsState().deployRelease = false;
                          });
    return true;
}

bool MainWindow::runAgentCliInstallThroughCore(const QString &node, const QString &ip,
                                               const QString &user, const QString &sshPassword,
                                               const QString &identityFile, bool copyCredentials,
                                               std::function<void(bool, QString)> onFinished)
{
    if (!coreHostJobsAttached() || m_vultrProvisionActive)
        return false;
    m_coreClient->request(
        QStringLiteral("core.hosts.agentClis"),
        {{QStringLiteral("node"), node},
         {QStringLiteral("ip"), ip},
         {QStringLiteral("user"), user},
         {QStringLiteral("password"), sshPassword},
         {QStringLiteral("identityFile"), identityFile},
         {QStringLiteral("copyCredentials"), copyCredentials}},
        [onFinished](bool ok, const QJsonValue &value, const QString &error) {
            const QJsonObject result = value.toObject();
            if (!ok || !result.value(QStringLiteral("started")).toBool()) {
                if (onFinished)
                    onFinished(false, ok ? result.value(QStringLiteral("message")).toString() : error);
                return;
            }
            awaitCoreJob(result.value(QStringLiteral("job")).toString(),
                         [onFinished](const QJsonObject &job) {
                             if (onFinished)
                                 onFinished(job.value(QStringLiteral("ok")).toBool(),
                                            job.value(QStringLiteral("message")).toString());
                         });
        });
    return true;
}

bool MainWindow::runHostActionsConfigurationThroughCore(
    forkmesh::control::MirrorActionsConfigurationRequest &request, const QString &sshPassword)
{
    if (!coreHostJobsAttached())
        return false;
    QJsonObject variables;
    for (auto it = request.variables.cbegin(); it != request.variables.cend(); ++it)
        variables.insert(it.key(), it.value());
    m_coreClient->request(QStringLiteral("core.hosts.actionsConfig"),
                          {{QStringLiteral("requestId"), request.requestId},
                           {QStringLiteral("host"), request.host},
                           {QStringLiteral("sshUser"), request.sshUser},
                           {QStringLiteral("nodeName"), request.nodeName},
                           {QStringLiteral("actionsEnabled"), request.actionsEnabled},
                           {QStringLiteral("replaceVariables"), request.replaceVariables},
                           {QStringLiteral("variables"), variables},
                           {QStringLiteral("password"), sshPassword}});
    for (QString &value : request.variables)
        value.fill(QChar::Null);
    request.variables.clear();
    return true;
}

bool MainWindow::submitHostLinkCodeThroughCore(const QString &code)
{
    if (!coreHostJobsAttached())
        return false;
    m_coreClient->request(QStringLiteral("core.hosts.environment"), coreHostsEnvironment());
    m_coreClient->request(QStringLiteral("core.hosts.linkCode"), {{QStringLiteral("code"), code}});
    return true;
}

bool MainWindow::destroyVultrHostThroughCore(const QString &name,
                                             std::function<void(bool, QString)> onDone)
{
    if (!coreHostJobsAttached() || !m_coreClient->hasNativeRoute(QStringLiteral("core.hosts.destroy")))
        return false;
    m_coreClient->request(
        QStringLiteral("core.hosts.destroy"),
        {{QStringLiteral("name"), name},
         {QStringLiteral("apiKey"), m_vultrApiKeyEdit ? m_vultrApiKeyEdit->text().trimmed() : QString()}},
        [onDone](bool ok, const QJsonValue &value, const QString &error) {
            const QJsonObject result = value.toObject();
            if (!ok || !result.value(QStringLiteral("started")).toBool()) {
                if (onDone)
                    onDone(false, ok ? result.value(QStringLiteral("message")).toString() : error);
                return;
            }
            awaitCoreJob(result.value(QStringLiteral("job")).toString(),
                         [onDone](const QJsonObject &job) {
                             if (onDone)
                                 onDone(job.value(QStringLiteral("ok")).toBool(),
                                        job.value(QStringLiteral("message")).toString());
                         });
        });
    return true;
}

bool MainWindow::createVultrMirrorThroughCore()
{
    if (!coreHostJobsAttached() ||
        !m_coreClient->hasNativeRoute(QStringLiteral("core.hosts.provision.create")))
        return false;
    if ((m_hostInstallProcess && m_hostInstallProcess->state() != QProcess::NotRunning) ||
        (m_hostDeployRemaining > 0 && jobsState().deployId.isEmpty())) {
        if (m_vultrStatus)
            m_vultrStatus->setText(QStringLiteral("Wait for the running install to finish first."));
        return true;
    }
    m_coreClient->request(QStringLiteral("core.hosts.environment"), coreHostsEnvironment());
    m_coreClient->request(
        QStringLiteral("core.hosts.provision.create"),
        {{QStringLiteral("node"), m_vultrNameEdit ? m_vultrNameEdit->text().trimmed() : QString()},
         {QStringLiteral("apiKey"), m_vultrApiKeyEdit ? m_vultrApiKeyEdit->text().trimmed() : QString()},
         {QStringLiteral("installAgentClis"), m_vultrAgentClisCheck && m_vultrAgentClisCheck->isChecked()},
         {QStringLiteral("plan"), std::exchange(m_vultrRequestedPlan, QString())},
         {QStringLiteral("region"), std::exchange(m_vultrRequestedRegion, QString())}});
    return true;
}

bool MainWindow::resumeVultrProvisionThroughCore()
{
    CoreHostJobsState &state = jobsState();
    if (!coreHostJobsAttached()) {
        if (m_coreClient && !m_coreClient->isConnected() &&
            state.resumeWaitedMs < kCoreWaitForResumeMs) {
            state.resumeWaitedMs += kCoreWaitStepMs;
            QTimer::singleShot(kCoreWaitStepMs, this, &MainWindow::resumeVultrProvision);
            return true;
        }
        return false;
    }
    if (!m_coreClient->hasNativeRoute(QStringLiteral("core.hosts.provision.resume")))
        return false;
    const QString apiKey = m_vultrApiKeyEdit ? m_vultrApiKeyEdit->text().trimmed() : QString();
    m_coreClient->request(QStringLiteral("core.hosts.environment"), coreHostsEnvironment());
    m_coreClient->request(
        QStringLiteral("core.hosts.provision.state"), {},
        [this, apiKey](bool ok, const QJsonValue &value, const QString &) {
            if (!ok || !m_coreClient)
                return;
            const QJsonObject snapshot = value.toObject();
            applyCoreProvisionSnapshot(snapshot);
            if (snapshot.value(QStringLiteral("active")).toBool())
                return;
            m_coreClient->request(QStringLiteral("core.hosts.provision.resume"),
                                  {{QStringLiteral("apiKey"), apiKey}});
        });
    return true;
}

bool MainWindow::endVultrProvisionThroughCore()
{
    if (!coreHostJobsAttached() ||
        !m_coreClient->hasNativeRoute(QStringLiteral("core.hosts.provision.end")))
        return false;
    m_coreClient->request(QStringLiteral("core.hosts.provision.end"));
    return true;
}

bool MainWindow::deleteMeshNodeThroughCore(const QString &target, const QString &nodeId)
{
    if (!coreHostJobsAttached() ||
        !m_coreClient->hasNativeRoute(QStringLiteral("core.hosts.deleteNode")))
        return false;
    m_nodeDeleteInProgress = true;
    m_nodeDeleteTarget = target;
    refreshNodeActionButtons();
    flashMessage(QStringLiteral("Deleting node \"%1\". Waiting for the relay to confirm it.")
                     .arg(target));
    m_coreClient->request(QStringLiteral("core.hosts.environment"), coreHostsEnvironment());
    m_coreClient->request(
        QStringLiteral("core.hosts.deleteNode"),
        {{QStringLiteral("node"), target},
         {QStringLiteral("nodeId"), nodeId},
         {QStringLiteral("origin"), QStringLiteral("desktop")}},
        [this](bool ok, const QJsonValue &value, const QString &error) {
            const QJsonObject result = value.toObject();
            if (ok && result.value(QStringLiteral("started")).toBool())
                return;
            m_nodeDeleteInProgress = false;
            m_nodeDeleteTarget.clear();
            refreshNodeActionButtons();
            const QString message = ok ? result.value(QStringLiteral("message")).toString() : error;
            if (!message.isEmpty())
                flashMessage(message, true);
        });
    return true;
}

