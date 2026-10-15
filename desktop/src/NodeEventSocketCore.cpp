#include "NodeEventSocket.h"
#include "CoreClient.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QTimer>

namespace {

constexpr int kCoreGoneFallbackMs = 15000;
constexpr int kCoreConfigureTimeoutMs = 15000;
constexpr int kMaxCoreOutboundPayload = 60 * 1024;

} // namespace

void NodeEventSocket::setCoreClient(CoreClient *client)
{
    if (m_core == client)
        return;
    if (m_core)
        m_core->disconnect(this);
    m_core = client;
    if (!client) {
        if (m_coreMode)
            leaveCoreMode(!m_userStopped);
        return;
    }
    connect(client, &CoreClient::coreEvent, this, &NodeEventSocket::onCoreEvent);
    connect(client, &CoreClient::connectedChanged, this,
            &NodeEventSocket::onCoreConnectedChanged);
    if (!m_userStopped && client->isConnected())
        enterCoreMode();
}

void NodeEventSocket::enterCoreMode()
{
    const bool wasConnected = m_wsReady;
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    if (m_connectTimeoutTimer)
        m_connectTimeoutTimer->stop();
    if (m_pingTimer)
        m_pingTimer->stop();
    if (m_coreFallbackTimer)
        m_coreFallbackTimer->stop();
    if (!m_coreMode)
        closeCurrentSocket();
    m_attemptActive = false;
    m_wsReady = false;
    resetControlQueue();
    m_accepts.clear();
    m_relaysTerminal = false;
    m_relaysControl = false;
    m_coreMode = true;
    m_coreBacklog = 0;
    m_coreStateSeq = 0;
    m_reconnectAttempts = 0;
    if (wasConnected)
        emit connectedChanged(false);
    configureCore();
}

void NodeEventSocket::leaveCoreMode(bool openNative)
{
    const bool wasConnected = m_wsReady;
    ++m_coreGeneration;
    m_coreMode = false;
    m_coreBacklog = 0;
    m_wsReady = false;
    m_accepts.clear();
    m_relaysTerminal = false;
    m_relaysControl = false;
    if (m_coreFallbackTimer)
        m_coreFallbackTimer->stop();
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    if (wasConnected)
        emit connectedChanged(false);
    if (openNative && !m_userStopped) {
        m_reconnectAttempts = 0;
        openConnection();
    }
}

void NodeEventSocket::armCoreFallback(int delayMs)
{
    if (!m_coreFallbackTimer) {
        m_coreFallbackTimer = new QTimer(this);
        m_coreFallbackTimer->setSingleShot(true);
        connect(m_coreFallbackTimer, &QTimer::timeout, this, [this] {
            if (!m_coreMode || m_userStopped || (m_core && m_core->isConnected()))
                return;
            leaveCoreMode(true);
        });
    }
    m_coreFallbackTimer->start(delayMs);
}

void NodeEventSocket::onCoreConnectedChanged(bool connected)
{
    if (m_userStopped)
        return;
    if (connected) {
        enterCoreMode();
        return;
    }
    if (!m_coreMode)
        return;
    ++m_coreGeneration;
    const bool wasConnected = m_wsReady;
    m_wsReady = false;
    m_accepts.clear();
    m_relaysTerminal = false;
    m_relaysControl = false;
    m_coreBacklog = 0;
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    if (wasConnected)
        emit connectedChanged(false);
    armCoreFallback(kCoreGoneFallbackMs);
}

void NodeEventSocket::configureCore()
{
    if (!m_coreMode || m_userStopped || !m_core || !m_core->isConnected())
        return;
    const QUrl url = m_urlFactory ? m_urlFactory() : QUrl();
    if (!url.isValid() || url.host().isEmpty()) {
        scheduleReconnect();
        return;
    }
    if (m_connectionAuthorizer && !m_connectionAuthorizer(url)) {
        emit systemMessage(
            QStringLiteral("Firewall blocked the relay event socket to %1.")
                .arg(url.host()));
        scheduleReconnect();
        return;
    }
    m_url = url;
    const quint64 generation = ++m_coreGeneration;
    QPointer<NodeEventSocket> self(this);
    m_core->request(
        QStringLiteral("core.relay.configure"),
        {{QStringLiteral("url"), url.toString(QUrl::FullyEncoded)},
         {QStringLiteral("account"), m_clientAccount},
         {QStringLiteral("node"), m_clientNode}},
        [self, generation](bool ok, const QJsonValue &result, const QString &error) {
            if (!self || generation != self->m_coreGeneration || !self->m_coreMode ||
                self->m_userStopped)
                return;
            if (ok) {
                self->applyCoreState(result.toObject());
                return;
            }
            emit self->systemMessage(
                QStringLiteral("The core could not take over the relay event "
                               "socket (%1); the desktop holds it instead.")
                    .arg(error.isEmpty() ? QStringLiteral("no answer") : error));
            self->leaveCoreMode(true);
        },
        kCoreConfigureTimeoutMs);
}

void NodeEventSocket::applyCoreState(const QJsonObject &state)
{
    const qint64 seq = qint64(state.value(QStringLiteral("seq")).toDouble());
    if (seq > 0 && seq <= m_coreStateSeq)
        return;
    if (seq > 0)
        m_coreStateSeq = seq;
    const bool connected = state.value(QStringLiteral("running")).toBool() &&
                           state.value(QStringLiteral("connected")).toBool();
    m_accepts.clear();
    for (const QJsonValue &value : state.value(QStringLiteral("accepts")).toArray()) {
        const QString kind = value.toString().trimmed();
        if (!kind.isEmpty())
            m_accepts.insert(kind);
    }
    m_relaysTerminal = false;
    m_relaysControl = false;
    for (const QJsonValue &value : state.value(QStringLiteral("relays")).toArray()) {
        if (value.toString() == QLatin1String("terminal"))
            m_relaysTerminal = true;
        if (value.toString() == QLatin1String("control"))
            m_relaysControl = true;
    }
    m_coreBacklog = state.value(QStringLiteral("controlBacklog")).toInt();
    const bool wasConnected = m_wsReady;
    m_wsReady = connected;
    if (connected)
        m_reconnectAttempts = 0;
    if (wasConnected != connected)
        emit connectedChanged(connected);
}

void NodeEventSocket::onCoreEvent(const QString &event, const QJsonObject &data)
{
    if (!m_coreMode)
        return;
    if (event == QLatin1String("core.relay.state")) {
        applyCoreState(data);
        return;
    }
    if (event == QLatin1String("core.relay.event"))
        processFrame(QJsonDocument(data.value(QStringLiteral("frame")).toObject())
                         .toJson(QJsonDocument::Compact));
}

bool NodeEventSocket::sendThroughCore(const QString &kind, const QJsonObject &frame)
{
    if (!m_wsReady || !m_core || !m_core->isConnected())
        return false;
    if (QJsonDocument(frame).toJson(QJsonDocument::Compact).size() > kMaxCoreOutboundPayload)
        return false;
    return m_core->notify(QStringLiteral("core.relay.send"),
                          {{QStringLiteral("kind"), kind}, {QStringLiteral("frame"), frame}});
}
