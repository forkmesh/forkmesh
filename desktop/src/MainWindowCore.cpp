#include "AppPaths.h"
#include "CoreClient.h"
#include "CoreGit.h"
#include "CoreSecrets.h"
#include "CoreVault.h"
#include "LocalControl.h"
#include "MainWindow.h"
#include "NodeEventSocket.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSettings>

void MainWindow::startCoreClient()
{
    forkmesh::corevault::install(this, [this](const QString &text) { logSystem(text); });
    if (m_coreClient || forkmesh::core::disabledByEnvironment() ||
        !QSettings().value(QStringLiteral("core/enabled"), true).toBool())
        return;
    m_coreClient = new CoreClient(this);
    m_coreClient->setProject(forkmesh::paths::projectSlug());
    connect(m_coreClient, &CoreClient::logLine, this,
            [this](const QString &text) { logSystem(text); });
    connect(m_coreClient, &CoreClient::connectedChanged, this,
            &MainWindow::onCoreConnectedChanged);
    registerCoreControlHandlers();
    installCoreAgentProcesses();
    forkmesh::coregit::follow(m_coreClient);
    forkmesh::corevault::follow(m_coreClient);
    if (m_nodeEventSocket)
        m_nodeEventSocket->setCoreClient(m_coreClient);
    m_coreClient->start();
}

void MainWindow::sendCoreControlInfo()
{
    if (!m_coreClient || !m_coreClient->isConnected())
        return;
    m_coreClient->request(QStringLiteral("control.info"),
                          {{QStringLiteral("name"), machineNodeName()},
                           {QStringLiteral("user"), nodeOwnerDisplayName()},
                           {QStringLiteral("node"), machineNodeName().trimmed().toLower()}});
    QJsonArray owners;
    for (const QString &name : {nodeOwnerDisplayName(), accountOwner()})
        if (!name.trimmed().isEmpty())
            owners.append(name.trimmed());
    m_coreClient->request(QStringLiteral("core.owners"), {{QStringLiteral("names"), owners}});
}

void MainWindow::onCoreConnectedChanged(bool connected)
{
    if (!ensureLocalControlServer())
        return;
    if (connected) {
        LocalControlServer::CoreBridge bridge;
        bridge.notify = [this](const QString &method, const QJsonObject &params) {
            return m_coreClient && m_coreClient->notify(method, params);
        };
        bridge.request = [this](const QString &method, const QJsonObject &params,
                                std::function<void(bool, const QJsonObject &, const QString &)> done) {
            if (!m_coreClient) {
                if (done)
                    done(false, {}, QStringLiteral("core_unavailable"));
                return;
            }
            m_coreClient->request(method, params,
                                  [done](bool ok, const QJsonValue &result, const QString &error) {
                                      if (done)
                                          done(ok, result.toObject(), error);
                                  });
        };
        sendCoreControlInfo();
        m_localControl->attachCore(std::move(bridge));
    } else {
        m_localControl->detachCore();
    }
    refreshLocalControlTab();
}

void MainWindow::registerCoreControlHandlers()
{
    const auto forward = [this](void (LocalControlServer::*member)(const QJsonObject &)) {
        return [this, member](const QJsonObject &params, const CoreClient::Responder &respond) {
            if (ensureLocalControlServer())
                (m_localControl->*member)(params);
            respond({}, QString());
        };
    };
    m_coreClient->handle(QStringLiteral("ui.control.opened"), forward(&LocalControlServer::coreOpened));
    m_coreClient->handle(QStringLiteral("ui.control.closed"), forward(&LocalControlServer::coreClosed));
    m_coreClient->handle(QStringLiteral("ui.control.request"), forward(&LocalControlServer::coreRequest));
    m_coreClient->handle(QStringLiteral("ui.control.pairing"), forward(&LocalControlServer::corePairing));
    m_coreClient->handle(QStringLiteral("ui.control.pairingClosed"),
                         forward(&LocalControlServer::corePairingClosed));
    m_coreClient->handle(QStringLiteral("ui.control.devices"), forward(&LocalControlServer::coreDevices));
    m_coreClient->handle(QStringLiteral("ui.control.fileReceived"),
                         forward(&LocalControlServer::coreFileReceived));
    m_coreClient->handle(
        QStringLiteral("ui.control.accountCheck"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            if (!ensureLocalControlServer()) {
                respond({{QStringLiteral("verdict"), QStringLiteral("unreachable")}}, QString());
                return;
            }
            m_localControl->coreAccountCheck(params, [respond](const QString &verdict) {
                respond({{QStringLiteral("verdict"), verdict}}, QString());
            });
        });
    m_coreClient->handle(
        QStringLiteral("ui.settings.write"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            QSettings settings;
            int written = 0;
            for (const QJsonValue &entry : params.value(QStringLiteral("writes")).toArray()) {
                const QJsonObject write = entry.toObject();
                const QString key = write.value(QStringLiteral("key")).toString();
                if (key.isEmpty() || key.startsWith(QLatin1Char('/')))
                    continue;
                if (write.value(QStringLiteral("remove")).toBool()) {
                    settings.remove(key);
                    ++written;
                    continue;
                }
                const QString type = write.value(QStringLiteral("type")).toString();
                const QJsonValue value = write.value(QStringLiteral("value"));
                if (type == QLatin1String("bool"))
                    settings.setValue(key, value.toBool());
                else if (type == QLatin1String("int"))
                    settings.setValue(key, qint64(value.toDouble()));
                else if (type == QLatin1String("double"))
                    settings.setValue(key, value.toDouble());
                else if (type == QLatin1String("stringList")) {
                    QStringList list;
                    for (const QJsonValue &item : value.toArray())
                        list << item.toString();
                    settings.setValue(key, list);
                } else if (type == QLatin1String("bytes"))
                    settings.setValue(key, QByteArray::fromBase64(value.toString().toLatin1()));
                else if (type == QLatin1String("json"))
                    settings.setValue(key, value.isObject()
                                               ? QJsonDocument(value.toObject()).toJson(QJsonDocument::Compact)
                                               : QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
                else if (type == QLatin1String("string"))
                    settings.setValue(key, value.toString());
                else {
                    respond({}, QStringLiteral("bad_type"));
                    return;
                }
                ++written;
            }
            settings.sync();
            respond({{QStringLiteral("written"), written}},
                    settings.status() == QSettings::NoError ? QString() : QStringLiteral("write_failed"));
        });
    m_coreClient->handle(QStringLiteral("ui.log"),
                         [this](const QJsonObject &params, const CoreClient::Responder &respond) {
                             const QString text = params.value(QStringLiteral("text")).toString();
                             if (!text.isEmpty())
                                 logSystem(text);
                             respond({}, QString());
                         });
    m_coreClient->handle(
        QStringLiteral("ui.repos.locate"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            QString error;
            const QJsonObject result = localControlRepoLocate(params, &error);
            respond(result, error);
        });
    m_coreClient->handle(
        QStringLiteral("ui.relay.send"),
        [this](const QJsonObject &params, const CoreClient::Responder &respond) {
            const bool sent = m_localControl &&
                              m_localControl->coreRelaySend(
                                  params.value(QStringLiteral("frame")).toObject());
            respond({{QStringLiteral("sent"), sent}}, QString());
        });
    forkmesh::coresecrets::registerHandlers(m_coreClient, [this] { reloadVariablesList(); });
    m_coreClient->handle(
        QStringLiteral("ui.insights.shortcutState"),
        [this](const QJsonObject &, const CoreClient::Responder &respond) {
            const forkmesh::localcontrol::insights::ShortcutRun *shown = nullptr;
            for (const auto &run : std::as_const(m_shortcutRuns))
                if (!shown || run.running || !shown->running)
                    shown = &run;
            if (!shown) {
                respond({{QStringLiteral("active"), false}}, QString());
                return;
            }
            respond({{QStringLiteral("active"), !m_shortcutProcesses.isEmpty()},
                     {QStringLiteral("running"), shown->running},
                     {QStringLiteral("repo"), shown->repo},
                     {QStringLiteral("file"), shown->file},
                     {QStringLiteral("lastOutput"),
                      forkmesh::localcontrol::insights::lastOutputLine(shown->output)}},
                    QString());
        });
    registerCoreHostsHandlers();
    registerCoreOpsHandlers();
    registerCoreActionsHandlers();
    registerCoreDiagHandlers();
    registerCoreMergeQueueHandlers();
    registerCoreInsightsHandlers();
    registerCoreAgentModelHandlers();
}
