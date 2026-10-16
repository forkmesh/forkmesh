#pragma once

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>

#include "LocalControlStream.h"
#include "SocketRetirement.h"

#include <functional>

class QTcpSocket;
class QTimer;

// RFC 6455 control channel for payload-free sync notifications and bounded,
// signed node exchanges. It never carries repository bytes; every write keeps
// its node/ts/sig proof and runs through the HTTPS handler's authorization, so
// the socket is transport rather than authority.
class NodeEventSocket : public QObject
{
    Q_OBJECT
public:
    explicit NodeEventSocket(QObject *parent = nullptr);

    // Called before every connection attempt so each upgrade carries a
    // freshly-signed forkmesh-issues-pull-v1 token (owner/ts/sig query params
    // — the same auth as GET /api/sync). Returning an invalid URL skips the
    // attempt; the reconnect timer retries later (e.g. after login).
    void setUrlFactory(std::function<QUrl()> factory);
    void setConnectionAuthorizer(std::function<bool(const QUrl &)> authorizer);
    // Public account/node labels are sent only as a diagnostic User-Agent on
    // the authenticated upgrade. They let the privileged Worker tail say
    // which desktop held a socket without changing socket authorization.
    void setClientIdentity(const QString &account, const QString &node);

    void start();
    void stop();
    void reconnectNow();
    bool isConnected() const { return m_wsReady; }
    bool accepts(const QString &kind) const
    {
        return m_wsReady && m_accepts.contains(kind);
    }
    // Send one signed write. `frame` needs "type" plus whatever that frame
    // kind proves itself with; returns false when the socket cannot take it,
    // so the caller can fall back to the signed HTTPS route.
    bool sendSignedFrame(const QJsonObject &frame);
    // Send one terminal frame back to the account owner's browser. Unlike a
    // signed write this is not a request to the relay: it is one leg of a
    // stream between two of the account's own peers, and there is no HTTPS
    // fallback — false simply means those bytes are gone, which is the right
    // answer for a screen the browser is about to have redrawn anyway.
    bool sendTerminalFrame(const QJsonObject &frame);
    bool relaysTerminals() const { return m_wsReady && m_relaysTerminal; }
    // One chunk of the owner's phone control channel, sealed end to end by
    // the two devices (LocalControl.h); false when the relay does not carry
    // control frames or the socket is down.
    bool sendOwnerControlFrame(const QJsonObject &frame);
    int controlBacklog() const { return int(m_controlQueue.size()); }

signals:
    void eventReceived(const QString &topic, const QString &repo,
                       const QString &state);
    void agentStatusResult(bool ok, const QJsonArray &results);
    // One relayed sign-in frame from the owner's browser. It is addressed by
    // node name — the relay broadcasts to every node socket on the account —
    // so the receiver checks the name against its own before acting.
    void terminalFrameReceived(const QJsonObject &frame);
    // One relayed control chunk from the owner's phone (addressed by node
    // name, like the terminal; the receiver checks the name).
    void controlFrameReceived(const QJsonObject &frame);
    // Result of a signed account heartbeat carried on this connection. The
    // signed timestamp is also its request id, letting the caller ignore a
    // late reply after recovery through HTTPS.
    void heartbeatResult(const QString &requestId, int status,
                         const QJsonObject &response);
    void connectedChanged(bool connected);
    void systemMessage(const QString &text);

private:
    void openConnection();
    void scheduleReconnect();
    void discardCurrentSocket();
    void closeCurrentSocket();
    void failCurrentConnection();
    void handleLinkLost();
    void connectSocketSignals();
    void sendHandshake();
    void onSocketReadyRead();
    void drainReadBuffer();
    void scheduleReadBufferDrain();
    void processFrame(const QByteArray &payload);
    void sendTextFrame(const QByteArray &payload);
    void sendControlFrame(int opcode, const QByteArray &payload = QByteArray());
    void onPingTick();
    void pumpControlQueue();
    void resetControlQueue();

    std::function<QUrl()> m_urlFactory;
    std::function<bool(const QUrl &)> m_connectionAuthorizer;
    QString m_clientAccount;
    QString m_clientNode;
    QUrl m_url; // the endpoint of the attempt in flight (for messages)
    QTcpSocket *m_socket = nullptr;
    forkmesh::DeferredSocketRetirement m_socketRetirement;
    QByteArray m_readBuffer;
    QByteArray m_wsKey;
    QSet<QString> m_accepts;
    bool m_relaysTerminal = false;
    bool m_relaysControl = false;
    forkmesh::localcontrol::SendBudget m_controlBudget;
    QList<QByteArray> m_controlQueue;
    qsizetype m_controlQueuedBytes = 0;
    QTimer *m_controlPaceTimer = nullptr;
    bool m_wsReady = false;
    bool m_drainScheduled = false; // a budgeted frame drain owes another turn
    bool m_attemptActive = false;
    bool m_userStopped = true; // inert until start()
    QTimer *m_pingTimer = nullptr;      // keepalive + stale-rx watchdog
    QTimer *m_reconnectTimer = nullptr; // exponential backoff between attempts
    QTimer *m_connectTimeoutTimer = nullptr; // bounds connect+TLS+upgrade
    int m_reconnectAttempts = 0;
    qint64 m_lastRxMs = 0;
    // Any text frame refreshes the relay's socket lease. Signed heartbeats and
    // status writes therefore replace, rather than duplicate, the keepalive.
    qint64 m_lastAppSendMs = 0;
};
