// Host management from the owner's phone, over the local control channel
// (LocalControl.h) on the LAN or carried through the relay.
//
// Every operation is the Hosts page's own code path: the installer behind the
// row's Update button, the mirror-node SSH transport the provisioning checks
// use, the Vultr create and destroy flows. Nothing here holds or sends a
// credential: the Vultr API key and the SSH keys stay on this desktop, and
// the phone only ever sees rows, results and log lines.
//
// Policy (LocalControlHosts.h):
//   - only a device approved at this desktop that names this desktop's own
//     account may use hosts.*;
//   - every change needs a one-use token from hosts.prepare, bound to that
//     device, action and host, and at most a minute old; destroy also needs
//     the host's name typed back;
//   - every change is written to the desktop log with the device and the
//     transport it came over.

#include "ActionStore.h"
#include "ControlNode.h"
#include "HostLogSources.h"
#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "MainWindow.h"
#include "MainWindowInternal.h"

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProcess>
#include <QSettings>
#include <QTimer>
#include <QUuid>

using namespace forkmesh::ui;
namespace hostctl = forkmesh::localcontrol::hosts;

namespace {

constexpr int kRecentLinesPerHost = 200;
constexpr int kMaxStreamsPerConnection = 4;
constexpr int kLogFlushIntervalMs = 400;
constexpr int kServiceCommandTimeoutMs = 45000;

QString lowerHost(const QString &host)
{
    return host.trimmed().toLower();
}

QString sourceId(int source)
{
    if (source >= 0 && source < forkmesh::hosts::kLogSourceCount)
        return QLatin1String(forkmesh::hosts::kLogSources[source].id);
    return QStringLiteral("host");
}

// The systemd verbs a phone may run, and nothing else: the unit is fixed and
// a restart leaves an in-progress clone alone, as the Hosts page's does.
QString serviceCommand(const QString &action)
{
    if (action == QLatin1String("restart"))
        return QStringLiteral(
            "sh -lc 'if ls -d /var/lib/forkmesh/repositories/.clone-* "
            ">/dev/null 2>&1; then echo \"restart: skipped, a clone is still "
            "running\"; elif systemctl restart forkmesh-mirror-node; then echo "
            "\"restart: done\"; else echo \"restart: refused\"; exit 1; fi'");
    if (action == QLatin1String("stop"))
        return QStringLiteral(
            "sh -lc 'systemctl stop forkmesh-mirror-node && echo \"stop: done\"'");
    if (action == QLatin1String("start"))
        return QStringLiteral(
            "sh -lc 'systemctl start forkmesh-mirror-node && echo \"start: done\"'");
    return {};
}

} // namespace

QString MainWindow::localControlDeviceLabel(int connectionId) const
{
    if (!m_localControl)
        return QStringLiteral("a phone");
    const LocalControlServer::Device device = m_localControl->deviceFor(connectionId);
    QString label = device.name.isEmpty() ? QStringLiteral("Phone") : device.name;
    if (!device.user.isEmpty())
        label += QStringLiteral(" (%1)").arg(device.user);
    label += m_localControl->isRelayed(connectionId)
                 ? QStringLiteral(", through the relay")
                 : QStringLiteral(", on the local network");
    return label;
}

QJsonObject MainWindow::localControlHostsPayload()
{
    QSettings settings;
    const QJsonArray hosts = forkmesh::control::loadSavedHosts(
        settings, kHostsSetting, &m_hostSessionPasswords);
    QHash<QString, const MemberInfo *> roster;
    for (const MemberInfo &member : std::as_const(m_homeRoster))
        if (!member.nodeName.isEmpty())
            roster.insert(lowerHost(member.nodeName), &member);
    QJsonArray rows;
    int online = 0;
    int attention = 0;
    for (const QJsonValue &value : hosts) {
        const QJsonObject host = value.toObject();
        const QString name = host.value(QStringLiteral("name")).toString();
        const QString probe = m_hostReachability.value(
            forkmesh::control::savedHostCredentialKey(
                name, host.value(QStringLiteral("ip")).toString(),
                host.value(QStringLiteral("user")).toString()));
        const MemberInfo *member = roster.value(lowerHost(name));
        QJsonObject row = hostctl::hostRow(host, probe,
                                           member ? member->version : QString(),
                                           member && member->online);
        const QString health = row.value(QStringLiteral("health")).toString();
        online += health == QLatin1String("online") ? 1 : 0;
        attention += health == QLatin1String("attention") ||
                             health == QLatin1String("offline")
                         ? 1
                         : 0;
        rows.append(row);
    }
    const bool vultrKey =
        (m_vultrApiKeyEdit && !m_vultrApiKeyEdit->text().trimmed().isEmpty()) ||
        !forkmesh::control::vultrApiKeyFromVariables(ActionStore::variables())
             .isEmpty();
    const bool installing =
        m_vultrProvisionActive ||
        (m_hostInstallProcess &&
         m_hostInstallProcess->state() != QProcess::NotRunning) ||
        m_hostDeployRemaining > 0;
    return QJsonObject{
        {QStringLiteral("hosts"), rows},
        {QStringLiteral("online"), online},
        {QStringLiteral("attention"), attention},
        {QStringLiteral("canCreate"), vultrKey},
        {QStringLiteral("busy"), installing},
        {QStringLiteral("provisioning"),
         m_vultrProvisionActive ? m_vultrProvisionNode : QString()},
        {QStringLiteral("desktop"), machineNodeName()},
    };
}

QJsonObject MainWindow::localControlHostDetail(const QString &host)
{
    const QJsonObject payload = localControlHostsPayload();
    QJsonObject row;
    for (const QJsonValue &value : payload.value(QStringLiteral("hosts")).toArray()) {
        if (lowerHost(value.toObject().value(QStringLiteral("name")).toString()) ==
            lowerHost(host)) {
            row = value.toObject();
            break;
        }
    }
    if (row.isEmpty())
        return {};
    const QStringList recent = m_hostRecentLogLines.value(lowerHost(host));
    row.insert(QStringLiteral("recentLog"),
               QJsonArray::fromStringList(recent.mid(qMax(0, recent.size() - 60))));
    QString installStatus;
    if (m_hostInstallStatus)
        installStatus = m_hostInstallStatus->text();
    row.insert(QStringLiteral("lastResult"), installStatus.left(600));
    return row;
}

void MainWindow::handleLocalControlHostsRequest(int connectionId, int requestId,
                                                const QString &method,
                                                const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    const LocalControlServer::Device device = server->deviceFor(connectionId);
    if (device.key.isEmpty() ||
        (!hostctl::ownerMayManageHosts(device.user, nodeOwnerDisplayName()) &&
         !hostctl::ownerMayManageHosts(device.user, accountOwner()))) {
        server->respondError(connectionId, requestId, QStringLiteral("not_owner"));
        return;
    }
    const QString host = params.value(QStringLiteral("host")).toString().trimmed();

    if (method == QLatin1String("hosts.list")) {
        server->respond(connectionId, requestId, localControlHostsPayload());
        return;
    }
    if (method == QLatin1String("hosts.detail")) {
        const QJsonObject detail = localControlHostDetail(host);
        if (detail.isEmpty())
            server->respondError(connectionId, requestId,
                                 QStringLiteral("unknown_host"));
        else
            server->respond(connectionId, requestId, detail);
        return;
    }
    if (method == QLatin1String("hosts.options")) {
        // The same plan and region rules the desktop's own create uses, as
        // choices; read with the desktop's key, which never leaves it.
        QString apiKey =
            m_vultrApiKeyEdit ? m_vultrApiKeyEdit->text().trimmed() : QString();
        if (apiKey.isEmpty())
            apiKey = forkmesh::control::vultrApiKeyFromVariables(
                ActionStore::variables());
        if (apiKey.isEmpty()) {
            server->respond(connectionId, requestId,
                            {{QStringLiteral("vultr"), false}});
            return;
        }
        QSettings settings;
        QStringList known;
        for (const QJsonValue &value : forkmesh::control::loadSavedHosts(
                 settings, kHostsSetting, &m_hostSessionPasswords))
            known.append(value.toObject().value(QStringLiteral("name")).toString().toLower());
        known += m_profileLinkedNodes;
        const QString suggested = forkmesh::control::nextMirrorNodeName(known);
        QPointer<LocalControlServer> guard(server);
        vultrApiCall(
            apiKey, QStringLiteral("/v2/plans?per_page=500"),
            QByteArrayLiteral("GET"), {},
            [guard, connectionId, requestId, suggested](QJsonObject result,
                                                        QString error) {
                if (!guard)
                    return;
                if (!error.isEmpty()) {
                    guard->respondError(connectionId, requestId,
                                        QStringLiteral("vultr_unreachable"));
                    return;
                }
                QJsonObject choices = forkmesh::control::vultrPlanChoices(
                    result.value(QStringLiteral("plans")).toArray());
                choices.insert(QStringLiteral("vultr"), true);
                choices.insert(QStringLiteral("suggestedName"), suggested);
                guard->respond(connectionId, requestId, choices);
            });
        return;
    }
    if (method == QLatin1String("hosts.prepare")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        if (!hostctl::isMutatingAction(action)) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("unsupported_action"));
            return;
        }
        // A new server may be left unnamed (the desktop picks the next free
        // mirror name); everything else names a saved host.
        if (action == QLatin1String("install")) {
            if (!host.isEmpty() && !hostctl::validHostName(host)) {
                server->respondError(connectionId, requestId,
                                     QStringLiteral("bad_host"));
                return;
            }
        } else if (localControlHostDetail(host).isEmpty()) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("unknown_host"));
            return;
        }
        const QString token = m_hostControlConfirmations.issue(
            device.key, action, host, QDateTime::currentMSecsSinceEpoch());
        QJsonObject result{
            {QStringLiteral("token"), token},
            {QStringLiteral("action"), action},
            {QStringLiteral("host"), host},
            {QStringLiteral("summary"), hostctl::actionSummary(action, host)},
            {QStringLiteral("expiresInMs"),
             double(hostctl::ConfirmationStore::kLifetimeMs)},
        };
        if (hostctl::isDestructiveAction(action))
            result.insert(QStringLiteral("typeToConfirm"), host);
        server->respond(connectionId, requestId, result);
        return;
    }
    if (method == QLatin1String("hosts.run")) {
        const QString action = params.value(QStringLiteral("action")).toString();
        const QString token = params.value(QStringLiteral("token")).toString();
        const hostctl::ConfirmationStore::Result verdict =
            m_hostControlConfirmations.consume(token, device.key, action, host,
                                               QDateTime::currentMSecsSinceEpoch());
        if (verdict != hostctl::ConfirmationStore::Result::Ok) {
            logSystem(QStringLiteral("Refused a host %1 of \"%2\" from %3: %4.")
                          .arg(action, host, localControlDeviceLabel(connectionId),
                               hostctl::confirmationError(verdict)));
            server->respondError(connectionId, requestId,
                                 hostctl::confirmationError(verdict));
            return;
        }
        if (hostctl::isDestructiveAction(action) &&
            !hostctl::typedNameMatches(
                params.value(QStringLiteral("confirmName")).toString(), host)) {
            logSystem(QStringLiteral("Refused to destroy \"%1\" from %2: the typed "
                                     "name did not match.")
                          .arg(host, localControlDeviceLabel(connectionId)));
            server->respondError(connectionId, requestId,
                                 QStringLiteral("name_mismatch"));
            return;
        }
        runLocalControlHostAction(connectionId, requestId, action, host, params);
        return;
    }
    if (method == QLatin1String("hosts.logs.start")) {
        if (localControlHostDetail(host).isEmpty()) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("unknown_host"));
            return;
        }
        int open = 0;
        for (const HostLogSubscription &sub : std::as_const(m_hostLogSubscriptions))
            open += sub.connectionId == connectionId ? 1 : 0;
        if (open >= kMaxStreamsPerConnection) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("too_many_streams"));
            return;
        }
        const QString stream = QUuid::createUuid().toString(QUuid::Id128);
        HostLogSubscription sub;
        sub.connectionId = connectionId;
        sub.host = lowerHost(host);
        m_hostLogSubscriptions.insert(stream, sub);
        if (!m_hostLogFlushTimer) {
            m_hostLogFlushTimer = new QTimer(this);
            m_hostLogFlushTimer->setInterval(kLogFlushIntervalMs);
            connect(m_hostLogFlushTimer, &QTimer::timeout, this,
                    &MainWindow::flushLocalControlHostLogs);
        }
        if (!m_hostLogFlushTimer->isActive())
            m_hostLogFlushTimer->start();
        // The recent tail first, so the viewer is never empty on open.
        const QStringList recent = m_hostRecentLogLines.value(sub.host);
        server->respond(connectionId, requestId,
                        {{QStringLiteral("stream"), stream},
                         {QStringLiteral("backlog"),
                          QJsonArray::fromStringList(
                              recent.mid(qMax(0, recent.size() - 120)))},
                         {QStringLiteral("window"), hostctl::LogStream::kWindow},
                         {QStringLiteral("maxBuffered"),
                          hostctl::LogStream::kMaxBufferedLines}});
        return;
    }
    if (method == QLatin1String("hosts.logs.ack") ||
        method == QLatin1String("hosts.logs.stop")) {
        const QString stream = params.value(QStringLiteral("stream")).toString();
        auto it = m_hostLogSubscriptions.find(stream);
        if (it == m_hostLogSubscriptions.end() || it->connectionId != connectionId) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("unknown_stream"));
            return;
        }
        if (method == QLatin1String("hosts.logs.stop")) {
            m_hostLogSubscriptions.erase(it);
        } else {
            it->stream.ack(qint64(params.value(QStringLiteral("seq")).toDouble()));
            // A window that just opened is worth filling now, not in 400 ms.
            QTimer::singleShot(0, this, &MainWindow::flushLocalControlHostLogs);
        }
        server->respond(connectionId, requestId, {});
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported"));
}

void MainWindow::runLocalControlHostAction(int connectionId, int requestId,
                                           const QString &action,
                                           const QString &host,
                                           const QJsonObject &params)
{
    LocalControlServer *server = m_localControl;
    if (!server)
        return;
    if (action == QLatin1String("terminal")) {
        if (!localControlHostDetail(host)
                 .value(QStringLiteral("actions"))
                 .toArray()
                 .contains(action)) {
            server->respondError(connectionId, requestId, QStringLiteral("unsupported_action"));
            return;
        }
        startLocalControlHostTerminal(connectionId, requestId, host, params);
        return;
    }
    const QString who = localControlDeviceLabel(connectionId);
    const QString key = action + QLatin1Char(':') + lowerHost(host);
    if (m_hostControlRunning.contains(key)) {
        server->respondError(connectionId, requestId, QStringLiteral("already_running"));
        return;
    }
    const auto started = [this, server, connectionId, requestId, key, action,
                          host, who](const QString &message) {
        m_hostControlRunning.insert(key, connectionId);
        logSystem(QStringLiteral("Host %1 of \"%2\" requested by %3.")
                      .arg(action, host.isEmpty() ? QStringLiteral("(new)") : host,
                           who));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("started"), true},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("host"), host},
                         {QStringLiteral("message"), message}});
        server->broadcast(QStringLiteral("hosts.changed"));
    };
    const bool installBusy =
        m_vultrProvisionActive ||
        (m_hostInstallProcess &&
         m_hostInstallProcess->state() != QProcess::NotRunning) ||
        m_hostDeployRemaining > 0;

    if (action == QLatin1String("update") || action == QLatin1String("installBinary")) {
        if (installBusy) {
            server->respondError(connectionId, requestId, QStringLiteral("busy"));
            return;
        }
        QSettings settings;
        const QJsonArray hosts = forkmesh::control::loadSavedHosts(
            settings, kHostsSetting, &m_hostSessionPasswords);
        int row = -1;
        for (int i = 0; i < hosts.size(); ++i)
            if (lowerHost(hosts.at(i).toObject().value(QStringLiteral("name"))
                              .toString()) == lowerHost(host))
                row = i;
        if (row < 0 || !m_hostsTable || row >= m_hostsTable->rowCount()) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("unknown_host"));
            return;
        }
        const bool upload = action == QLatin1String("installBinary");
        started(upload ? QStringLiteral("Uploading this desktop's ForkMesh binary.")
                       : QStringLiteral("Installing the latest ForkMesh."));
        loadHostIntoForm(row, 0);
        runHostInstall(upload, [this, action, host](bool ok) {
            finishLocalControlHostAction(
                action, host, ok,
                ok ? QStringLiteral("ForkMesh was updated and restarted.")
                   : (m_hostInstallStatus ? m_hostInstallStatus->text()
                                          : QStringLiteral("The update failed.")));
        });
        return;
    }
    if (action == QLatin1String("restart") || action == QLatin1String("stop") ||
        action == QLatin1String("start")) {
        const QJsonObject row = localControlHostDetail(host);
        if (!row.value(QStringLiteral("actions")).toArray().contains(action)) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("not_a_mirror_node"));
            return;
        }
        started(QStringLiteral("Asking the host's service manager."));
        runMirrorNodeSsh(host, serviceCommand(action), kServiceCommandTimeoutMs,
                         [this, action, host](bool ok, QString output) {
                             finishLocalControlHostAction(
                                 action, host, ok,
                                 output.isEmpty()
                                     ? (ok ? QStringLiteral("Done.")
                                           : QStringLiteral(
                                                 "The host could not be reached."))
                                     : output);
                         });
        return;
    }
    if (action == QLatin1String("install")) {
        if (installBusy) {
            server->respondError(connectionId, requestId, QStringLiteral("busy"));
            return;
        }
        if (!m_vultrNameEdit) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("hosts_page_unavailable"));
            return;
        }
        // The Hosts page's own create, with the phone's pick of the same
        // plan and region choices; an empty name lets the desktop pick one.
        m_vultrNameEdit->setText(host);
        m_vultrRequestedPlan = params.value(QStringLiteral("plan")).toString().trimmed();
        m_vultrRequestedRegion =
            params.value(QStringLiteral("region")).toString().trimmed().toLower();
        createVultrMirrorFromForm();
        if (!m_vultrProvisionActive) {
            m_vultrRequestedPlan.clear();
            m_vultrRequestedRegion.clear();
            const QString why = m_vultrStatus ? m_vultrStatus->text().trimmed()
                                              : QString();
            logSystem(QStringLiteral("A new host requested by %1 did not start: %2")
                          .arg(who, why));
            server->respondError(connectionId, requestId,
                                 why.isEmpty() ? QStringLiteral("not_started") : why);
            return;
        }
        // Keyed by the name the create settled on, which finishVultrProvision
        // reports back.
        m_hostControlRunning.insert(QStringLiteral("install:") +
                                        lowerHost(m_vultrProvisionNode),
                                    connectionId);
        logSystem(QStringLiteral("New Vultr host \"%1\" (%2, %3) requested by %4.")
                      .arg(m_vultrProvisionNode,
                           m_vultrRequestedPlan.isEmpty() ? QStringLiteral("default plan")
                                                          : m_vultrRequestedPlan,
                           m_vultrRequestedRegion.isEmpty()
                               ? QStringLiteral("default region")
                               : m_vultrRequestedRegion,
                           who));
        server->respond(connectionId, requestId,
                        {{QStringLiteral("started"), true},
                         {QStringLiteral("action"), action},
                         {QStringLiteral("host"), m_vultrProvisionNode},
                         {QStringLiteral("message"),
                          QStringLiteral("Creating the server. This takes a few "
                                         "minutes.")}});
        server->broadcast(QStringLiteral("hosts.changed"));
        return;
    }
    if (action == QLatin1String("destroy")) {
        const QJsonObject row = localControlHostDetail(host);
        if (!row.value(QStringLiteral("actions")).toArray().contains(action)) {
            server->respondError(connectionId, requestId,
                                 QStringLiteral("not_a_vultr_host"));
            return;
        }
        started(QStringLiteral("Destroying the server."));
        destroyVultrHostNamed(host, [this, host](bool ok, QString message) {
            finishLocalControlHostAction(QStringLiteral("destroy"), host, ok,
                                         message);
        });
        return;
    }
    server->respondError(connectionId, requestId, QStringLiteral("unsupported_action"));
}

void MainWindow::finishLocalControlHostAction(const QString &action,
                                              const QString &host, bool ok,
                                              const QString &message)
{
    const QString key = action + QLatin1Char(':') + lowerHost(host);
    if (!m_hostControlRunning.contains(key))
        return; // not one a phone started
    m_hostControlRunning.remove(key);
    logSystem(QStringLiteral("Host %1 of \"%2\" from the phone %3: %4")
                  .arg(action, host, ok ? QStringLiteral("finished")
                                        : QStringLiteral("failed"),
                       message.left(300)));
    if (!m_localControl)
        return;
    m_localControl->broadcast(QStringLiteral("hosts.action"),
                              {{QStringLiteral("action"), action},
                               {QStringLiteral("host"), host},
                               {QStringLiteral("ok"), ok},
                               {QStringLiteral("message"), message.left(2000)}});
    m_localControl->broadcast(QStringLiteral("hosts.changed"));
}

void MainWindow::tapHostLogLineForPhones(const QString &node, int source,
                                         const QString &text)
{
    if (text.trimmed().isEmpty())
        return;
    // The live view and the background feed both replay a log's tail when
    // they connect; keep each line once, as the main log does.
    static forkmesh::hosts::ForwardedHostLines seen(2048);
    if (!seen.firstSighting(node, QString::number(source) + text))
        return;
    const QString host = lowerHost(node);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString src = sourceId(source);
    forwardHostLogLineToCore(host, src, text);
    QStringList &recent = m_hostRecentLogLines[host];
    recent.append(QStringLiteral("[%1] %2").arg(src, text.left(
                                                     hostctl::LogStream::kMaxLineChars)));
    if (recent.size() > kRecentLinesPerHost)
        recent.remove(0, recent.size() - kRecentLinesPerHost);
    for (HostLogSubscription &sub : m_hostLogSubscriptions)
        if (sub.host == host)
            sub.stream.push(now, src, text);
}

void MainWindow::flushLocalControlHostLogs()
{
    if (!m_localControl || m_hostLogSubscriptions.isEmpty()) {
        if (m_hostLogFlushTimer)
            m_hostLogFlushTimer->stop();
        return;
    }
    for (auto it = m_hostLogSubscriptions.begin();
         it != m_hostLogSubscriptions.end(); ++it) {
        // One batch per stream per tick keeps a chatty host inside the relay's
        // frame budget; the window keeps a slow phone from being flooded.
        const QJsonObject batch = it->stream.takeBatch();
        if (batch.isEmpty())
            continue;
        QJsonObject data = batch;
        data.insert(QStringLiteral("stream"), it.key());
        data.insert(QStringLiteral("host"), it->host);
        m_localControl->sendEventTo(it->connectionId, QStringLiteral("hosts.log"),
                                    data);
    }
}

void MainWindow::closeLocalControlStreams(int connectionId)
{
    m_localControlImages.dropConnection(connectionId);
    m_updateWatchers.remove(connectionId);
    dropLocalControlFollows(connectionId);
    dropLocalControlTerminalViewer(connectionId);
    for (auto it = m_hostLogSubscriptions.begin();
         it != m_hostLogSubscriptions.end();) {
        if (it->connectionId == connectionId)
            it = m_hostLogSubscriptions.erase(it);
        else
            ++it;
    }
    for (auto it = m_hostControlRunning.begin(); it != m_hostControlRunning.end();
         ++it)
        if (it.value() == connectionId)
            it.value() = 0; // the result is still broadcast to whoever is back
}
