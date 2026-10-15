#include "NodeEventSocket.h"
#include "CoreClient.h"
#include "ForkMeshVersion.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QPointer>
#include <QRandomGenerator>
#include <QSslSocket>
#include <QTimer>
#include <QUrl>

#ifndef FORKMESH_VERSION
#define FORKMESH_VERSION "dev"
#endif

namespace {

constexpr quint64 kMaxEventPayload = 64 * 1024;
// How long one turn may spend handing frames to their handlers before yielding
// back to the event loop. Matches the transcript and cloud-log drains.
constexpr qint64 kFrameDrainBudgetMs = 8;
// Outbound frame ceiling, matching the relay's NODE_EVENT_MAX_FRAME_BYTES: the
// largest signed write is a batched agent-status report with a task id, run
// state and provenance per live session (200 at most). Heartbeats are smaller.
// Must stay under 64 KiB — sendTextFrame writes the two-byte extended length,
// which cannot describe a longer payload.
constexpr int kMaxOutboundPayload = 60 * 1024;
constexpr qsizetype kMaxQueuedControlBytes = 16 * 1024 * 1024;
constexpr int kConnectTimeoutMs = 30000;
// Protocol pings ride the edge connection without waking the hibernated
// Durable Object. An app-level frame refreshes the DO's staleness clock; send
// a dedicated {"type":"ping"} only when no signed write already did so.
constexpr int kPingIntervalMs = 25000;
constexpr qint64 kAppPingIntervalMs = 4 * 60 * 1000;
constexpr qint64 kStaleRxMs = 70000;
constexpr int kReconnectBaseDelayMs = 2000;
constexpr qint64 kReconnectMaxDelayMs = 2 * 60 * 1000;

QByteArray randomWsKey()
{
    QByteArray key(16, Qt::Uninitialized);
    for (int i = 0; i < key.size(); ++i)
        key[i] = char(QRandomGenerator::global()->bounded(256));
    return key.toBase64();
}

QByteArray diagnosticToken(const QString &value)
{
    return QUrl::toPercentEncoding(value.trimmed().left(64), "-._~");
}

QByteArray clientUserAgent(const QString &account, const QString &node)
{
    return QByteArray("ForkMeshDesktop/") + FORKMESH_VERSION +
           " (account=" + diagnosticToken(account) +
           "; node=" + diagnosticToken(node) + ")";
}

QString wsRequestPath(const QUrl &url)
{
    QString path = url.path().isEmpty() ? QStringLiteral("/") : url.path();
    if (url.hasQuery())
        path += "?" + url.query();
    return path;
}

} // namespace

NodeEventSocket::NodeEventSocket(QObject *parent) : QObject(parent) {}

void NodeEventSocket::setUrlFactory(std::function<QUrl()> factory)
{
    m_urlFactory = std::move(factory);
}

void NodeEventSocket::setConnectionAuthorizer(
    std::function<bool(const QUrl &)> authorizer)
{
    m_connectionAuthorizer = std::move(authorizer);
}

void NodeEventSocket::setClientIdentity(const QString &account,
                                        const QString &node)
{
    m_clientAccount = account;
    m_clientNode = node;
}

void NodeEventSocket::start()
{
    m_userStopped = false;
    m_reconnectAttempts = 0;
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    if (m_core) {
        if (m_core->isConnected()) {
            enterCoreMode();
        } else {
            m_coreMode = true;
            armCoreFallback(5000);
        }
        return;
    }
    openConnection();
}

void NodeEventSocket::reconnectNow()
{
    if (m_userStopped || m_wsReady || m_attemptActive)
        return;
    if (m_coreMode) {
        m_reconnectAttempts = 0;
        if (m_reconnectTimer && m_reconnectTimer->isActive()) {
            m_reconnectTimer->stop();
            configureCore();
        } else if (m_core && m_core->isConnected()) {
            m_core->notify(QStringLiteral("core.relay.reconnect"));
        }
        return;
    }
    m_reconnectAttempts = 0;
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    openConnection();
}

void NodeEventSocket::stop()
{
    m_userStopped = true;
    if (m_coreMode && m_core && m_core->isConnected())
        m_core->notify(QStringLiteral("core.relay.stop"));
    m_coreMode = false;
    m_coreBacklog = 0;
    ++m_coreGeneration;
    if (m_coreFallbackTimer)
        m_coreFallbackTimer->stop();
    if (m_reconnectTimer)
        m_reconnectTimer->stop();
    if (m_connectTimeoutTimer)
        m_connectTimeoutTimer->stop();
    if (m_pingTimer)
        m_pingTimer->stop();
    const bool wasConnected = m_wsReady;
    closeCurrentSocket();
    m_wsReady = false;
    resetControlQueue();
    m_accepts.clear();
    m_attemptActive = false;
    if (wasConnected)
        emit connectedChanged(false);
}

void NodeEventSocket::discardCurrentSocket()
{
    QTcpSocket *socket = m_socket;
    m_socket = nullptr;
    if (!socket)
        return;
    socket->disconnect(this);
    socket->abort();
    m_socketRetirement.retire(socket);
}

void NodeEventSocket::closeCurrentSocket()
{
    QTcpSocket *socket = m_socket;
    if (!socket)
        return;

    if (m_wsReady && socket->state() == QAbstractSocket::ConnectedState) {
        QByteArray payload;
        payload.append(char(0x03));
        payload.append(char(0xE8)); // normal closure (1000), network byte order
        sendControlFrame(0x8, payload);
        socket->flush();
    }

    m_socket = nullptr;
    socket->disconnect(this);
    if (socket->state() == QAbstractSocket::ConnectedState)
        socket->disconnectFromHost();
    else
        socket->abort();
    m_socketRetirement.retire(socket);
}

void NodeEventSocket::failCurrentConnection()
{
    discardCurrentSocket();
    handleLinkLost();
}

void NodeEventSocket::openConnection()
{
    discardCurrentSocket();
    m_attemptActive = false;
    m_wsReady = false;
    m_accepts.clear();
    m_relaysTerminal = false;
    m_relaysControl = false;
    resetControlQueue();
    m_readBuffer.clear();

    if (m_userStopped)
        return;

    // A fresh signed URL for every attempt: the ts in the drain token must be
    // within the relay's clock-skew window when the upgrade lands.
    m_url = m_urlFactory ? m_urlFactory() : QUrl();
    if (!m_url.isValid() || m_url.host().isEmpty()) {
        scheduleReconnect(); // e.g. not signed in yet; retry later
        return;
    }
    if (m_connectionAuthorizer && !m_connectionAuthorizer(m_url)) {
        emit systemMessage(
            QStringLiteral("Firewall blocked the relay event socket to %1.")
                .arg(m_url.host()));
        scheduleReconnect();
        return;
    }

    if (!m_connectTimeoutTimer) {
        m_connectTimeoutTimer = new QTimer(this);
        m_connectTimeoutTimer->setSingleShot(true);
        connect(m_connectTimeoutTimer, &QTimer::timeout, this, [this] {
            if (m_wsReady || m_userStopped || !m_attemptActive)
                return;
            failCurrentConnection();
        });
    }
    m_connectTimeoutTimer->start(kConnectTimeoutMs);

    m_socket = m_url.scheme() == QLatin1String("wss") ? new QSslSocket(this)
                                                      : new QTcpSocket(this);
    m_attemptActive = true;
    connectSocketSignals();
    const int port =
        m_url.port(m_url.scheme() == QLatin1String("wss") ? 443 : 80);
    if (auto *ssl = qobject_cast<QSslSocket *>(m_socket))
        ssl->connectToHostEncrypted(m_url.host(), port);
    else
        m_socket->connectToHost(m_url.host(), port);
}

void NodeEventSocket::scheduleReconnect()
{
    if (m_connectTimeoutTimer)
        m_connectTimeoutTimer->stop();
    if (m_userStopped)
        return;
    if (m_reconnectTimer && m_reconnectTimer->isActive())
        return;
    if (!m_reconnectTimer) {
        m_reconnectTimer = new QTimer(this);
        m_reconnectTimer->setSingleShot(true);
        connect(m_reconnectTimer, &QTimer::timeout, this, [this] {
            if (m_userStopped)
                return;
            if (m_coreMode)
                configureCore();
            else
                openConnection();
        });
    }
    const int shift = qMin(m_reconnectAttempts, 9);
    ++m_reconnectAttempts;
    int delay = int(qMin<qint64>(qint64(kReconnectBaseDelayMs) << shift,
                                 kReconnectMaxDelayMs));
    delay += int(QRandomGenerator::global()->bounded(delay / 4 + 250));
    m_reconnectTimer->start(delay);
}

void NodeEventSocket::connectSocketSignals()
{
    QTcpSocket *socket = m_socket;
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        const forkmesh::DeferredSocketRetirement::Scope dispatching(
            m_socketRetirement);
        if (socket == m_socket)
            onSocketReadyRead();
    });
    connect(socket, &QTcpSocket::connected, this, [this, socket] {
        const forkmesh::DeferredSocketRetirement::Scope dispatching(
            m_socketRetirement);
        socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        if (socket == m_socket && !qobject_cast<QSslSocket *>(socket))
            sendHandshake();
    });
    if (auto *ssl = qobject_cast<QSslSocket *>(socket))
        connect(ssl, &QSslSocket::encrypted, this, [this, socket] {
            const forkmesh::DeferredSocketRetirement::Scope dispatching(
                m_socketRetirement);
            if (socket == m_socket)
                sendHandshake();
        });
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        const forkmesh::DeferredSocketRetirement::Scope dispatching(
            m_socketRetirement);
        if (socket != m_socket)
            return;
        discardCurrentSocket();
        handleLinkLost();
    });
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket] {
        const forkmesh::DeferredSocketRetirement::Scope dispatching(
            m_socketRetirement);
        if (socket != m_socket)
            return;
        failCurrentConnection();
    });
}

void NodeEventSocket::handleLinkLost()
{
    if (!m_attemptActive && !m_wsReady)
        return;
    m_attemptActive = false;
    if (m_pingTimer)
        m_pingTimer->stop();
    if (m_connectTimeoutTimer)
        m_connectTimeoutTimer->stop();
    const bool wasConnected = m_wsReady;
    m_wsReady = false;
    resetControlQueue();
    m_accepts.clear();
    if (wasConnected)
        emit connectedChanged(false);
    scheduleReconnect();
}

void NodeEventSocket::sendHandshake()
{
    m_wsKey = randomWsKey();
    QByteArray host = m_url.host().toUtf8();
    if (m_url.port() > 0)
        host += ":" + QByteArray::number(m_url.port());
    const QByteArray request =
        "GET " + wsRequestPath(m_url).toUtf8() + " HTTP/1.1\r\n"
        "Host: " + host + "\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Key: " + m_wsKey + "\r\n"
        "User-Agent: " + clientUserAgent(m_clientAccount, m_clientNode) +
        "\r\n\r\n";
    m_socket->write(request);
}

void NodeEventSocket::onPingTick()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastRxMs > 0 && now - m_lastRxMs > kStaleRxMs) {
        failCurrentConnection();
        return;
    }
    sendControlFrame(0x9);
    if (now - m_lastAppSendMs >= kAppPingIntervalMs) {
        sendTextFrame(QJsonDocument(QJsonObject{{QStringLiteral("type"),
                                                 QStringLiteral("ping")}})
                          .toJson(QJsonDocument::Compact));
    }
}

void NodeEventSocket::onSocketReadyRead()
{
    m_readBuffer += m_socket->readAll();
    if (!m_wsReady) {
        const int headerEnd = m_readBuffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray header = m_readBuffer.left(headerEnd);
        m_readBuffer.remove(0, headerEnd + 4);
        if (!header.startsWith("HTTP/1.1 101") &&
            !header.startsWith("HTTP/1.0 101")) {
            // Non-101 is almost always transient (relay redeploy, quota 429)
            // — or a 401 because the signed token aged past the skew window
            // in a slow connect. Reconnect with backoff either way; the
            // fallback /api/sync poll covers the gap.
            const int lineEnd = header.indexOf("\r\n");
            const QString statusLine = QString::fromLatin1(
                (lineEnd < 0 ? header : header.left(lineEnd)).left(80))
                                           .trimmed();
            emit systemMessage(
                QStringLiteral("Relay event socket upgrade refused (%1); "
                               "falling back to the sync poll until it "
                               "reconnects.")
                    .arg(statusLine.isEmpty() ? QStringLiteral("no status")
                                              : statusLine));
            failCurrentConnection();
            return;
        }
        m_wsReady = true;
        m_attemptActive = true;
        m_reconnectAttempts = 0;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_lastRxMs = now;
        m_lastAppSendMs = now;
        if (m_reconnectTimer)
            m_reconnectTimer->stop();
        if (m_connectTimeoutTimer)
            m_connectTimeoutTimer->stop();
        if (!m_pingTimer) {
            m_pingTimer = new QTimer(this);
            m_pingTimer->setInterval(kPingIntervalMs);
            connect(m_pingTimer, &QTimer::timeout, this,
                    &NodeEventSocket::onPingTick);
        }
        m_pingTimer->start();
        emit connectedChanged(true);
    }

    drainReadBuffer();
}

// adhoc #2104: one readyRead used to hand every buffered frame to its handlers
// in a single turn, and those handlers are not cheap — a heartbeat result
// repaints the node list, an event frame can start a repository sync. A relay
// that had been queueing while the link was down therefore arrived as one long
// block of GUI thread (6.6 s in the worst sampled stall). Frames are drained on
// a budget now, the same way the cloud log monitor drains its records: always
// one, then only while the turn has time left.
void NodeEventSocket::drainReadBuffer()
{
    m_drainScheduled = false;
    if (!m_socket || !m_wsReady)
        return; // torn down, or a fresh handshake owns the buffer
    const QPointer<QTcpSocket> socket(m_socket);
    QElapsedTimer budget;
    budget.start();
    bool handled = false;
    while (socket && m_socket == socket && m_wsReady &&
           m_readBuffer.size() >= 2) {
        if (handled && budget.elapsed() >= kFrameDrainBudgetMs) {
            scheduleReadBufferDrain();
            return;
        }
        const uchar b0 = uchar(m_readBuffer.at(0));
        const uchar b1 = uchar(m_readBuffer.at(1));
        const int opcode = b0 & 0x0f;
        quint64 len = b1 & 0x7f;
        int pos = 2;
        if (len == 126) {
            if (m_readBuffer.size() < pos + 2)
                return;
            len = (uchar(m_readBuffer.at(pos)) << 8) |
                  uchar(m_readBuffer.at(pos + 1));
            pos += 2;
        } else if (len == 127) {
            if (m_readBuffer.size() < pos + 8)
                return;
            len = 0;
            for (int i = 0; i < 8; ++i)
                len = (len << 8) | uchar(m_readBuffer.at(pos + i));
            pos += 8;
        }
        if (len > kMaxEventPayload) {
            failCurrentConnection();
            return;
        }
        const bool masked = b1 & 0x80;
        QByteArray mask;
        if (masked) {
            if (m_readBuffer.size() < pos + 4)
                return;
            mask = m_readBuffer.mid(pos, 4);
            pos += 4;
        }
        if (m_readBuffer.size() < pos + int(len))
            return;
        QByteArray payload = m_readBuffer.mid(pos, int(len));
        m_readBuffer.remove(0, pos + int(len));
        if (masked) {
            for (int i = 0; i < payload.size(); ++i)
                payload[i] = payload.at(i) ^ mask.at(i % 4);
        }
        m_lastRxMs = QDateTime::currentMSecsSinceEpoch();
        handled = true; // this frame is off the buffer, whatever it costs below
        if (opcode == 0x9) { // server ping -> pong
            sendControlFrame(0xA, payload);
            continue;
        }
        if (opcode == 0xA) // pong: keepalive acknowledged
            continue;
        if (opcode == 0x8) {
            sendControlFrame(0x8, payload);
            if (socket && m_socket == socket)
                socket->flush();
            if (socket && m_socket == socket)
                socket->disconnectFromHost();
            return;
        }
        if (opcode == 0x1)
            processFrame(payload);
    }
}

void NodeEventSocket::scheduleReadBufferDrain()
{
    if (m_drainScheduled)
        return;
    m_drainScheduled = true;
    QTimer::singleShot(0, this, &NodeEventSocket::drainReadBuffer);
}

void NodeEventSocket::processFrame(const QByteArray &payload)
{
    const QJsonDocument doc = QJsonDocument::fromJson(payload);
    if (!doc.isObject())
        return;
    const QJsonObject frame = doc.object();
    const QString type = frame.value(QStringLiteral("type")).toString();
    // Advisory-only by design: a frame can trigger one signed inbox sync or a
    // direct Git fetch of a locally configured repository, but it supplies no
    // repository or inbox bytes. The advertised state only avoids a no-op fetch;
    // normal Git and refs-integrity checks still decide what is accepted.
    if (type == QLatin1String("event")) {
        emit eventReceived(frame.value(QStringLiteral("topic")).toString(),
                           frame.value(QStringLiteral("repo")).toString(),
                           frame.value(QStringLiteral("state")).toString());
        return;
    }
    if (type == QLatin1String("hello")) {
        // Which signed writes this relay can take off the socket. It grants
        // nothing — every such frame still carries its own proof — so the
        // worst a lying relay achieves is that we send it a write it ignores,
        // exactly what an older relay does today.
        m_accepts.clear();
        for (const QJsonValue &value :
             frame.value(QStringLiteral("accepts")).toArray()) {
            const QString kind = value.toString().trimmed();
            if (!kind.isEmpty())
                m_accepts.insert(kind);
        }
        // Relays are frames this socket can carry between the account's own
        // peers rather than writes to relay state. An older relay names none,
        // and the node simply never sees a terminal frame from it.
        m_relaysTerminal = false;
        m_relaysControl = false;
        for (const QJsonValue &value :
             frame.value(QStringLiteral("relays")).toArray()) {
            if (value.toString().trimmed() == QLatin1String("terminal"))
                m_relaysTerminal = true;
            if (value.toString().trimmed() == QLatin1String("control"))
                m_relaysControl = true;
        }
        const QJsonObject budget = frame.value(QStringLiteral("controlBudget")).toObject();
        const qint64 windowMs = qint64(budget.value(QStringLiteral("windowMs")).toDouble());
        const int maximum = budget.value(QStringLiteral("max")).toInt();
        if (windowMs > 0 && maximum > 0)
            m_controlBudget.configure(windowMs, maximum);
        else
            m_controlBudget.configure(forkmesh::localcontrol::kDefaultRelayControlWindowMs,
                                      forkmesh::localcontrol::kDefaultRelayControlMax);
        return;
    }
    if (type == QLatin1String("control")) {
        // Only the phone half's frames; another node's answers echo nowhere.
        if (frame.value(QStringLiteral("from")).toString() ==
            QLatin1String("browser"))
            emit controlFrameReceived(frame);
        return;
    }
    if (type == QLatin1String("terminal")) {
        // The relay only forwards these between one account's own sockets, and
        // it labels the sender. A frame that did not come from the browser
        // half is another node's output echoed back.
        if (frame.value(QStringLiteral("from")).toString() ==
            QLatin1String("browser"))
            emit terminalFrameReceived(frame);
        return;
    }
    if (type == QLatin1String("agent-status-result")) {
        emit agentStatusResult(
            frame.value(QStringLiteral("ok")).toBool(),
            frame.value(QStringLiteral("results")).toArray());
        return;
    }
    if (type == QLatin1String("heartbeat-result"))
        emit heartbeatResult(
            frame.value(QStringLiteral("requestId")).toString(),
            frame.value(QStringLiteral("status")).toInt(),
            frame.value(QStringLiteral("response")).toObject());
}

bool NodeEventSocket::sendSignedFrame(const QJsonObject &frame)
{
    const QString kind = frame.value(QStringLiteral("type")).toString();
    if (m_coreMode)
        return !kind.isEmpty() && accepts(kind) &&
               sendThroughCore(QStringLiteral("signed"), frame);
    // Never speak a frame kind this relay has not advertised: an older relay
    // silently drops what it does not understand, and the caller must be free
    // to treat "false" as "use the signed HTTPS route instead".
    if (kind.isEmpty() || !accepts(kind) || !m_socket)
        return false;
    const QByteArray payload =
        QJsonDocument(frame).toJson(QJsonDocument::Compact);
    if (payload.size() > kMaxOutboundPayload)
        return false;
    sendTextFrame(payload);
    return true;
}

bool NodeEventSocket::sendTerminalFrame(const QJsonObject &frame)
{
    if (m_coreMode)
        return sendThroughCore(QStringLiteral("terminal"), frame);
    if (!m_wsReady || !m_socket)
        return false;
    const QByteArray payload =
        QJsonDocument(frame).toJson(QJsonDocument::Compact);
    if (payload.size() > kMaxOutboundPayload)
        return false;
    sendTextFrame(payload);
    return true;
}

bool NodeEventSocket::sendOwnerControlFrame(const QJsonObject &frame)
{
    if (m_coreMode)
        return m_relaysControl && sendThroughCore(QStringLiteral("control"), frame);
    if (!m_wsReady || !m_socket || !m_relaysControl)
        return false;
    const QByteArray payload =
        QJsonDocument(frame).toJson(QJsonDocument::Compact);
    if (payload.size() > kMaxOutboundPayload)
        return false;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_controlQueue.isEmpty() && m_controlBudget.waitMs(now) == 0) {
        m_controlBudget.spend(now);
        sendTextFrame(payload);
        return true;
    }
    if (m_controlQueuedBytes + payload.size() > kMaxQueuedControlBytes)
        return false;
    m_controlQueue.append(payload);
    m_controlQueuedBytes += payload.size();
    pumpControlQueue();
    return true;
}

void NodeEventSocket::pumpControlQueue()
{
    while (!m_controlQueue.isEmpty()) {
        if (!m_wsReady || !m_socket) {
            resetControlQueue();
            return;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const qint64 wait = m_controlBudget.waitMs(now);
        if (wait > 0) {
            if (!m_controlPaceTimer) {
                m_controlPaceTimer = new QTimer(this);
                m_controlPaceTimer->setSingleShot(true);
                connect(m_controlPaceTimer, &QTimer::timeout, this,
                        &NodeEventSocket::pumpControlQueue);
            }
            if (!m_controlPaceTimer->isActive())
                m_controlPaceTimer->start(int(qMin<qint64>(wait, 60000)));
            return;
        }
        m_controlBudget.spend(now);
        const QByteArray payload = m_controlQueue.takeFirst();
        m_controlQueuedBytes -= payload.size();
        sendTextFrame(payload);
    }
}

void NodeEventSocket::resetControlQueue()
{
    m_controlQueue.clear();
    m_controlQueuedBytes = 0;
    m_controlBudget.reset();
    if (m_controlPaceTimer)
        m_controlPaceTimer->stop();
}

void NodeEventSocket::sendTextFrame(const QByteArray &payload)
{
    if (!m_wsReady || !m_socket || payload.size() > kMaxOutboundPayload)
        return;
    QByteArray frame;
    frame.append(char(0x81));
    if (payload.size() < 126) {
        frame.append(char(0x80 | payload.size()));
    } else {
        frame.append(char(0x80 | 126));
        frame.append(char((payload.size() >> 8) & 0xff));
        frame.append(char(payload.size() & 0xff));
    }
    QByteArray mask(4, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i)
        mask[i] = char(QRandomGenerator::global()->bounded(256));
    frame.append(mask);
    QByteArray masked = payload;
    for (int i = 0; i < masked.size(); ++i)
        masked[i] = masked.at(i) ^ mask.at(i % 4);
    frame.append(masked);
    if (m_socket->write(frame) >= 0)
        m_lastAppSendMs = QDateTime::currentMSecsSinceEpoch();
}

void NodeEventSocket::sendControlFrame(int opcode, const QByteArray &payload)
{
    // Ping/pong control frames (payload <= 125 bytes), client-masked per RFC
    // 6455.
    if (!m_socket || !m_wsReady || payload.size() > 125)
        return;
    QByteArray frame;
    frame.append(char(0x80 | (opcode & 0x0f)));
    frame.append(char(0x80 | payload.size()));
    QByteArray mask(4, Qt::Uninitialized);
    for (int i = 0; i < 4; ++i)
        mask[i] = char(QRandomGenerator::global()->bounded(256));
    frame.append(mask);
    QByteArray masked = payload;
    for (int i = 0; i < masked.size(); ++i)
        masked[i] = masked.at(i) ^ mask.at(i % 4);
    frame.append(masked);
    m_socket->write(frame);
}
