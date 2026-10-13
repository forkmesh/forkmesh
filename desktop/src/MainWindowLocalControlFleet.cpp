// The "fleet." local control family (WS16): host operations and the Vultr fleet beyond hosts.*.
// Registered by registerLocalControlFamilies() (MainWindowLocalControlFamilies.cpp),
// which has already checked that the device belongs to this desktop's owner.
// Gate 2 answers "unsupported" for every method; WS16 fills it in.

#include "ActionStore.h"
#include "ControlNode.h"
#include "LocalControl.h"
#include "LocalControlFleet.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"
#include "MirrorGatewayHealth.h"

#include <QCheckBox>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QSpinBox>
#include <QTableWidget>
#include <QTimer>
#include <QUuid>

using namespace forkmesh::ui;
namespace fleetctl = forkmesh::localcontrol::fleet;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

constexpr int kProvisionLogLines = 80;
constexpr qint64 kProvisionLogTailBytes = 48 * 1024;
constexpr int kDiskUsageTimeoutMs = 60000;
constexpr int kMaxRepos = 300;

QString lowerName(const QString &name)
{
    return name.trimmed().toLower();
}

QByteArray fileTail(const QString &path, qint64 maxBytes)
{
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return {};
    const qint64 size = file.size();
    if (size > maxBytes)
        file.seek(size - maxBytes);
    return file.readAll();
}

}

QJsonObject MainWindow::localControlProvisionStatus()
{
    const bool resumable = m_vultrResumeRequested && !m_vultrProvisionNode.isEmpty() &&
                           !m_vultrProvisionActive;
    QJsonObject status = fleetctl::provisionStatus(
        m_vultrProvisionActive, m_vultrProvisionNode, m_vultrProvisionStage,
        m_vultrProvisionState, m_vultrProvisionDetail, m_vultrProvisionMessage,
        m_vultrPublicationGap, m_vultrStageDetailText, resumable);
    status.insert(QStringLiteral("attempts"), m_vultrInstallAttempts);
    return status;
}

QJsonObject MainWindow::localControlFleetSummary()
{
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject fleet{
        {QStringLiteral("available"), false},
        {QStringLiteral("enabled"), false},
        {QStringLiteral("desired"), 0},
        {QStringLiteral("status"), QString()},
        {QStringLiteral("nextCheckInMs"), -1},
        {QStringLiteral("reconciling"), false},
    };
    const bool vultrKey =
        (m_vultrApiKeyEdit && !m_vultrApiKeyEdit->text().trimmed().isEmpty()) ||
        !forkmesh::control::vultrApiKeyFromVariables(ActionStore::variables()).isEmpty();
    const bool busy =
        m_vultrProvisionActive ||
        (m_hostInstallProcess && m_hostInstallProcess->state() != QProcess::NotRunning) ||
        m_hostDeployRemaining > 0;
    QJsonObject mirrorClient{
        {QStringLiteral("status"),
         m_controlNodeStatus ? fleetctl::plainText(m_controlNodeStatus->text()) : QString()},
        {QStringLiteral("health"),
         m_controlNodeHealth ? fleetctl::plainText(m_controlNodeHealth->text()) : QString()},
    };
    QJsonObject result{
        {QStringLiteral("costs"), fleetctl::costSummary(hosts)},
        {QStringLiteral("fleet"), fleet},
        {QStringLiteral("provision"), localControlProvisionStatus()},
        {QStringLiteral("gateways"),
         fleetctl::gatewayRows(m_sshMirrorHealth.persistedState(), now,
                               MirrorGatewayHealth::kRetiredAfterMs)},
        {QStringLiteral("mirrorClient"), mirrorClient},
        {QStringLiteral("mirrorPush"),
         QJsonObject{{QStringLiteral("pushing"), int(m_sshMirrorPushing.size())},
                     {QStringLiteral("pending"), int(m_sshMirrorPushPending.size())}}},
        {QStringLiteral("canCreate"), vultrKey},
        {QStringLiteral("busy"), busy},
        {QStringLiteral("deploying"), m_hostDeployRemaining},
        {QStringLiteral("admin"), m_isAdmin},
        {QStringLiteral("nodeDelete"),
         QJsonObject{{QStringLiteral("inProgress"), m_nodeDeleteInProgress},
                     {QStringLiteral("target"), m_nodeDeleteTarget}}},
        {QStringLiteral("lastResult"),
         m_hostInstallStatus ? fleetctl::plainText(m_hostInstallStatus->text(), 600)
                             : QString()},
        {QStringLiteral("running"), QJsonArray::fromStringList(m_fleetControlRunning.values())},
    };
    return result;
}

void MainWindow::handleLocalControlFleetRequest(int connectionId, int requestId,
                                                const QString &method,
                                                const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const LocalControlServer::Device device = server->deviceFor(connectionId);

    if (method == QLatin1String("fleet.summary")) {
        server->respond(connectionId, requestId, localControlFleetSummary());
        return;
    }
    if (method == QLatin1String("fleet.provision")) {
        QJsonObject status = localControlProvisionStatus();
        status.insert(QStringLiteral("log"),
                      QJsonArray::fromStringList(fleetctl::tailLines(
                          fileTail(vultrProvisionLogPath(), kProvisionLogTailBytes),
                          kProvisionLogLines, 400)));
        server->respond(connectionId, requestId, status);
        return;
    }
    if (method == QLatin1String("fleet.repos")) {
        QJsonArray repos;
        for (const RepositoryRecord &repo : std::as_const(m_repositories)) {
            if (repos.size() >= kMaxRepos)
                break;
            if (repo.owner.isEmpty() || repo.name.isEmpty() || repo.localPath.isEmpty())
                continue;
            const QString key = repo.owner + QLatin1Char('/') + repo.name;
            repos.append(QJsonObject{
                {QStringLiteral("repo"), key},
                {QStringLiteral("private"), repo.isPrivate},
                {QStringLiteral("published"), repo.publishToNetwork},
                {QStringLiteral("pushing"), m_sshMirrorPushing.contains(key) ||
                                                m_sshMirrorPushing.contains(key.toLower())}});
        }
        server->respond(connectionId, requestId, {{QStringLiteral("repos"), repos}});
        return;
    }
    if (method == QLatin1String("fleet.disk")) {
        const QString host = params.value(QStringLiteral("host")).toString().trimmed();
        QString path = params.value(QStringLiteral("path")).toString().trimmed();
        if (path.isEmpty())
            path = QStringLiteral("/");
        if (localControlHostDetail(host).isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_host"));
            return;
        }
        const QString command = fleetctl::diskUsageCommand(path);
        if (command.isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("bad_path"));
            return;
        }
        QPointer<LocalControlServer> guard(server);
        runMirrorNodeSsh(host, command, kDiskUsageTimeoutMs,
                         [guard, connectionId, requestId, host, path](bool ok, QString output) {
                             if (!guard)
                                 return;
                             const QJsonArray rows = fleetctl::parseDiskUsage(output, path);
                             if (!ok && rows.isEmpty()) {
                                 guard->respondError(connectionId, requestId,
                                                     QStringLiteral("host_unreachable"));
                                 return;
                             }
                             guard->respond(connectionId, requestId,
                                            {{QStringLiteral("host"), host},
                                             {QStringLiteral("path"), path},
                                             {QStringLiteral("entries"), rows}});
                         });
        return;
    }
    if (method == QLatin1String("fleet.prepare")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        QString error;
        const QString subject = fleetctl::subjectFor(action, params, &error);
        if (subject.isEmpty()) {
            server->respondError(connectionId, requestId, error);
            return;
        }
        const QString host = subject.section(QLatin1Char('|'), 0, 0);
        const bool namesSavedHost =
            action == QLatin1String("forget") || action == QLatin1String("uninstall") ||
            action == QLatin1String("agentClis") || action == QLatin1String("actions");
        if (namesSavedHost && localControlHostDetail(host).isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_host"));
            return;
        }
        if (action == QLatin1String("add") && !localControlHostDetail(host).isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("host_exists"));
            return;
        }
        if (action == QLatin1String("deleteNode") && !m_isAdmin) {
            server->respondError(connectionId, requestId, QStringLiteral("not_admin"));
            return;
        }
        const QString token = m_fleetControlConfirmations.issue(
            device.key, action, subject, QDateTime::currentMSecsSinceEpoch());
        QJsonObject result{
            {QStringLiteral("token"), token},
            {QStringLiteral("action"), action},
            {QStringLiteral("subject"), subject},
            {QStringLiteral("summary"), fleetctl::actionSummary(action, subject)},
            {QStringLiteral("expiresInMs"), double(hostctl::ConfirmationStore::kLifetimeMs)},
        };
        const QString typed = fleetctl::typedConfirmation(action, subject);
        if (!typed.isEmpty())
            result.insert(QStringLiteral("typeToConfirm"), typed);
        server->respond(connectionId, requestId, result);
        return;
    }
    if (method == QLatin1String("fleet.run")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        QString error;
        const QString subject = fleetctl::subjectFor(action, params, &error);
        const QString token = params.value(QStringLiteral("token")).toString();
        const hostctl::ConfirmationStore::Result verdict = m_fleetControlConfirmations.consume(
            token, device.key, action, subject, QDateTime::currentMSecsSinceEpoch());
        if (subject.isEmpty() || verdict != hostctl::ConfirmationStore::Result::Ok) {
            const QString why = subject.isEmpty() ? error : hostctl::confirmationError(verdict);
            logSystem(QStringLiteral("Refused a fleet %1 from %2: %3.")
                          .arg(action, localControlDeviceLabel(connectionId), why));
            server->respondError(connectionId, requestId, why);
            return;
        }
        if (!fleetctl::typedConfirmationMatches(
                params.value(QStringLiteral("confirmText")).toString(),
                fleetctl::typedConfirmation(action, subject))) {
            logSystem(QStringLiteral("Refused a fleet %1 of \"%2\" from %3: the typed "
                                     "confirmation did not match.")
                          .arg(action, subject, localControlDeviceLabel(connectionId)));
            server->respondError(connectionId, requestId, QStringLiteral("name_mismatch"));
            return;
        }
        runLocalControlFleetAction(connectionId, requestId, action, subject, params);
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}

void MainWindow::runLocalControlFleetAction(int connectionId, int requestId,
                                            const QString &action, const QString &subject,
                                            const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const QString who = localControlDeviceLabel(connectionId);
    const QString key = action + QLatin1Char(':') + subject;
    const QString host = subject.section(QLatin1Char('|'), 0, 0);
    if (m_fleetControlRunning.contains(key)) {
        server->respondError(connectionId, requestId, QStringLiteral("already_running"));
        return;
    }
    const bool installBusy =
        m_vultrProvisionActive ||
        (m_hostInstallProcess && m_hostInstallProcess->state() != QProcess::NotRunning) ||
        m_hostDeployRemaining > 0;
    const auto respondStarted = [this, server, connectionId, requestId, action, subject,
                                 who](const QString &message, bool tracked) {
        if (tracked)
            m_fleetControlRunning.insert(action + QLatin1Char(':') + subject);
        logSystem(QStringLiteral("Fleet %1 (%2) requested by %3.").arg(action, subject, who));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("started"), true},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), subject},
                         {QStringLiteral("message"), message}});
        server->broadcast(QStringLiteral("fleet.changed"));
        server->broadcast(QStringLiteral("hosts.changed"));
    };
    const auto savedHost = [this](const QString &name, int *row) {
        QSettings settings;
        const QJsonArray hosts = forkmesh::control::loadSavedHosts(
            settings, kHostsSetting, &m_hostSessionPasswords);
        for (int i = 0; i < hosts.size(); ++i) {
            const QJsonObject host = hosts.at(i).toObject();
            if (lowerName(host.value(QStringLiteral("name")).toString()) == lowerName(name)) {
                if (row)
                    *row = i;
                return host;
            }
        }
        if (row)
            *row = -1;
        return QJsonObject();
    };

    if (action == QLatin1String("add")) {
        const QString name = params.value(QStringLiteral("host")).toString().trimmed();
        const QString address = params.value(QStringLiteral("address")).toString().trimmed();
        const QString user = params.value(QStringLiteral("user")).toString().trimmed();
        rememberHost(name, address, user, QString(), QStringLiteral("added"));
        refreshHostsTable();
        probeSavedHosts();
        respondStarted(QStringLiteral("Saved \"%1\". The desktop is checking it now.").arg(name),
                       false);
        return;
    }
    if (action == QLatin1String("forget")) {
        const QJsonObject saved = savedHost(host, nullptr);
        forgetSavedHostNamed(saved.value(QStringLiteral("name")).toString(host));
        respondStarted(QStringLiteral("Removed \"%1\" from the saved hosts.").arg(host), false);
        return;
    }
    if (action == QLatin1String("deployAll")) {
        if (installBusy) {
            server->respondError(connectionId, requestId, QStringLiteral("busy"));
            return;
        }
        const FleetDeployMode mode = subject == QLatin1String("reinstall")
                                         ? FleetDeployMode::Reinstall
                                     : subject == QLatin1String("source")
                                         ? FleetDeployMode::UpdateSource
                                         : FleetDeployMode::InstallBinary;
        runHostDeployAllParallel(mode);
        if (m_hostDeployRemaining <= 0) {
            const QString why = m_hostInstallStatus
                                    ? fleetctl::plainText(m_hostInstallStatus->text())
                                    : QString();
            server->respondError(connectionId, requestId,
                                 why.isEmpty() ? QStringLiteral("not_started") : why);
            return;
        }
        respondStarted(QStringLiteral("Deploying to %1 host(s) in parallel.")
                           .arg(m_hostDeployRemaining),
                       false);
        return;
    }
    if (action == QLatin1String("uninstall")) {
        if (installBusy) {
            server->respondError(connectionId, requestId, QStringLiteral("busy"));
            return;
        }
        int row = -1;
        savedHost(host, &row);
        if (row < 0 || !m_hostsTable || row >= m_hostsTable->rowCount()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_host"));
            return;
        }
        loadHostIntoForm(row, 0);
        runHostUninstall();
        if (!m_hostInstallProcess || m_hostInstallProcess->state() == QProcess::NotRunning) {
            const QString why = m_hostInstallStatus
                                    ? fleetctl::plainText(m_hostInstallStatus->text())
                                    : QString();
            server->respondError(connectionId, requestId,
                                 why.isEmpty() ? QStringLiteral("not_started") : why);
            return;
        }
        respondStarted(QStringLiteral("Uninstalling ForkMesh from \"%1\".").arg(host), false);
        return;
    }
    if (action == QLatin1String("agentClis")) {
        if (m_hostAgentInstallProcess &&
            m_hostAgentInstallProcess->state() != QProcess::NotRunning) {
            server->respondError(connectionId, requestId, QStringLiteral("busy"));
            return;
        }
        const QJsonObject saved = savedHost(host, nullptr);
        const QString name = saved.value(QStringLiteral("name")).toString().trimmed();
        const QString ip = saved.value(QStringLiteral("ip")).toString().trimmed();
        const QString user = saved.value(QStringLiteral("user")).toString().trimmed();
        if (name.isEmpty() || ip.isEmpty() || user.isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_host"));
            return;
        }
        const QString password = m_hostSessionPasswords.value(
            forkmesh::control::savedHostCredentialKey(name, ip, user));
        respondStarted(QStringLiteral("Installing Claude Code and Codex on \"%1\".").arg(name),
                       true);
        runAgentCliInstall(name, ip, user, password, savedHostIdentityFile(name, ip, user),
                           false, [this, action, subject](bool ok, QString message) {
                               if (m_hostInstallStatus)
                                   m_hostInstallStatus->setText(message);
                               finishLocalControlFleetAction(action, subject, ok, message);
                           });
        return;
    }
    if (action == QLatin1String("actions")) {
        if (m_hostActionsProcess && m_hostActionsProcess->state() != QProcess::NotRunning) {
            server->respondError(connectionId, requestId, QStringLiteral("busy"));
            return;
        }
        const QJsonObject saved = savedHost(host, nullptr);
        forkmesh::control::MirrorActionsConfigurationRequest request;
        request.requestId = QUuid::createUuid().toString(QUuid::Id128).toLower();
        request.host = saved.value(QStringLiteral("ip")).toString().trimmed();
        request.sshUser = saved.value(QStringLiteral("user")).toString().trimmed();
        request.nodeName = saved.value(QStringLiteral("name")).toString().trimmed();
        request.actionsEnabled = subject.endsWith(QLatin1String("|on"));
        request.replaceVariables = false;
        if (request.host.isEmpty() || request.sshUser.isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_host"));
            return;
        }
        QString password = m_hostSessionPasswords.value(forkmesh::control::savedHostCredentialKey(
            request.nodeName, request.host, request.sshUser));
        runHostActionsConfiguration(std::move(request), password);
        password.fill(QChar::Null);
        respondStarted(subject.endsWith(QLatin1String("|on"))
                           ? QStringLiteral("Turning Actions on for \"%1\".").arg(host)
                           : QStringLiteral("Turning Actions off for \"%1\".").arg(host),
                       false);
        return;
    }
    if (action == QLatin1String("linkCode")) {
        if (hostLinkSigningAccountName().isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("cannot_sign"));
            return;
        }
        submitHostLinkCode(subject);
        respondStarted(QStringLiteral("Sent the link code. The result shows on the Hosts page."),
                       true);
        QTimer::singleShot(5000, this, [this, action, subject] {
            const QString outcome =
                m_hostInstallStatus ? fleetctl::plainText(m_hostInstallStatus->text()) : QString();
            finishLocalControlFleetAction(action, subject,
                                          !outcome.contains(QStringLiteral("Could not")),
                                          outcome);
        });
        return;
    }
    if (action == QLatin1String("resume")) {
        if (m_vultrProvisionActive) {
            server->respondError(connectionId, requestId, QStringLiteral("already_running"));
            return;
        }
        if (!m_vultrResumeRequested || m_vultrProvisionNode.isEmpty()) {
            server->respondError(connectionId, requestId, QStringLiteral("nothing_to_resume"));
            return;
        }
        resumeVultrProvision();
        if (!m_vultrProvisionActive) {
            const QString why = m_vultrStatus ? fleetctl::plainText(m_vultrStatus->text())
                                              : QString();
            server->respondError(connectionId, requestId,
                                 why.isEmpty() ? QStringLiteral("not_started") : why);
            return;
        }
        respondStarted(QStringLiteral("Resumed the deployment of \"%1\".").arg(m_vultrProvisionNode),
                       false);
        return;
    }
    if (action == QLatin1String("fleetTarget") ||
        action == QLatin1String("reconcile")) {
        server->respondError(connectionId, requestId,
                             QStringLiteral("hosts_page_unavailable"));
        return;
    }
    if (action == QLatin1String("mirrorPush")) {
        int index = -1;
        for (int i = 0; i < m_repositories.size(); ++i) {
            const RepositoryRecord &repo = m_repositories.at(i);
            if ((repo.owner + QLatin1Char('/') + repo.name).toLower() == subject) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            server->respondError(connectionId, requestId, QStringLiteral("unknown_repo"));
            return;
        }
        QString idleReason;
        const int remotes = pushToSshMirrorRemotes(index, true, QString(), &idleReason);
        m_fleetControlRunning.insert(key);
        const int managed = pushRefsToManagedHosts(
            index, true, QString(), QStringList(),
            [this, action, subject](int succeeded, int failed) {
                finishLocalControlFleetAction(
                    action, subject, failed == 0,
                    QStringLiteral("Managed hosts: %1 updated, %2 failed.")
                        .arg(succeeded)
                        .arg(failed));
            });
        if (managed <= 0)
            m_fleetControlRunning.remove(key);
        if (remotes <= 0 && managed <= 0) {
            server->respondError(connectionId, requestId,
                                 idleReason.isEmpty() ? QStringLiteral("no_mirror_targets")
                                                      : fleetctl::plainText(idleReason));
            return;
        }
        logSystem(QStringLiteral("Mirror push of %1 requested by %2.").arg(subject, who));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("started"), true},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("subject"), subject},
                         {QStringLiteral("remotes"), remotes},
                         {QStringLiteral("managedHosts"), qMax(0, managed)},
                         {QStringLiteral("message"),
                          QStringLiteral("Pushing to %1 SSH remote(s) and %2 managed host(s).")
                              .arg(qMax(0, remotes))
                              .arg(qMax(0, managed))}});
        server->broadcast(QStringLiteral("fleet.changed"));
        return;
    }
    if (action == QLatin1String("deleteNode")) {
        if (!m_isAdmin) {
            server->respondError(connectionId, requestId, QStringLiteral("not_admin"));
            return;
        }
        if (m_nodeDeleteInProgress) {
            server->respondError(connectionId, requestId, QStringLiteral("busy"));
            return;
        }
        const bool canSign = m_profileIdentity.isValid() &&
                             hasOwnerSigningCapability(accountOwner());
        if (activeAccountSessionToken().isEmpty() && !canSign) {
            server->respondError(connectionId, requestId, QStringLiteral("cannot_sign"));
            return;
        }
        const QString target = host;
        m_nodeDeleteInProgress = true;
        m_nodeDeleteTarget = target;
        refreshNodeActionButtons();
        setNodeDeleteStatus(QStringLiteral("1/3: destroying the server for \"%1\" (from %2)")
                                .arg(target, who));
        respondStarted(QStringLiteral("Deleting \"%1\": server, DNS, then the mesh.").arg(target),
                       false);
        destroyVultrServerForNode(target, [this, target](QString outcome) {
            if (!outcome.isEmpty())
                setNodeDeleteStatus(outcome);
            setNodeDeleteStatus(QStringLiteral("2/3: removing DNS for \"%1\"").arg(target));
            removeVultrMirrorDns(target, [this, target](QString dnsOutcome) {
                if (!dnsOutcome.isEmpty())
                    setNodeDeleteStatus(dnsOutcome);
                setNodeDeleteStatus(
                    QStringLiteral("3/3: removing \"%1\" from the mesh").arg(target));
                sendMeshNodeDeleteRequest(target, QString());
                if (m_localControl)
                    m_localControl->broadcast(QStringLiteral("fleet.changed"));
            });
        });
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported_action"));
}

void MainWindow::finishLocalControlFleetAction(const QString &action, const QString &subject,
                                               bool ok, const QString &message)
{
    const QString key = action + QLatin1Char(':') + subject;
    if (!m_fleetControlRunning.remove(key))
        return;
    logSystem(QStringLiteral("Fleet %1 (%2) from the phone %3: %4")
                  .arg(action, subject, ok ? QStringLiteral("finished") : QStringLiteral("failed"),
                       message.left(300)));
    if (!m_localControl)
        return;
    m_localControl->broadcast(QStringLiteral("fleet.action"),
                              {{QStringLiteral("action"), action},
                               {QStringLiteral("subject"), subject},
                               {QStringLiteral("ok"), ok},
                               {QStringLiteral("message"), fleetctl::plainText(message, 2000)}});
    m_localControl->broadcast(QStringLiteral("fleet.changed"));
}
