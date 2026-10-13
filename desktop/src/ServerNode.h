#pragma once

#include "ChatBackend.h"
#include "RoomCrypto.h"
#include "RoomSeal.h"

#include <QHash>
#include <QJsonObject>
#include <QQueue>
#include <QSet>
#include <QTcpSocket>
#include <QUrl>

#include "SocketRetirement.h"

#include <functional>

namespace TelemetrySettings {
inline const QString kReportCpu = QStringLiteral("nodes/reportCpu");
inline const QString kReportMemory = QStringLiteral("nodes/reportMemory");
inline const QString kReportDisk = QStringLiteral("nodes/reportDisk");
inline const QString kReportDiagnostics = QStringLiteral("nodes/reportDiagnostics");
} // namespace TelemetrySettings

class QTimer;

class ServerNode : public ChatBackend
{
    Q_OBJECT
public:
    ServerNode(const QString &userName, const QString &nodeName,
               const QString &ownerUser, const QString &stableNodeId,
               const QUrl &serverUrl,
               const QString &roomName,
               const QString &solanaAddress,
               const QString &roomPassphrase = QString(),
               QObject *parent = nullptr);

    bool start();

    void setEndpoints(const QList<QUrl> &endpoints);
    void setDirectRoom(bool direct);
    void setConnectionAuthorizer(std::function<bool(const QUrl &)> authorizer);
    void setNetworkAvailable(bool available) override;

#ifdef FORKMESH_SERVER_NODE_TESTS
    void setTransportLimitsForTests(int connectTimeoutMs,
                                    int reconnectBaseDelayMs,
                                    qint64 maxPendingWriteBytes);
    // Shrinks the presence beat so a test can run many roster flushes in a
    // few hundred milliseconds instead of one per minute.
    void setPresenceIntervalForTests(int presenceIntervalMs);
#endif

    QString sendChat(const QString &channel, const QString &text) override;
    QString sendChatWithId(const QString &channel, const QString &text,
                           const QString &messageId);
    static QString moderatedChatText(const QString &text);
    void sendThreadReply(const QString &channel,
                         const QString &rootMessageId,
                         const QString &text) override;
    void setAccountKind(const QString &kind) override;
    void setRoomPassphrase(const QString &passphrase) override;
    QString roomSealKeyId() const override;
    QStringList roomSealPeerKeyIds() const { return m_seal.peerKeyIds(); }
    void sendBotChat(const QString &channel, const QString &text) override;
    void sendBotThreadReply(const QString &channel,
                            const QString &rootMessageId,
                            const QString &text) override;
    void sendTeammateChat(const QString &channel, const QString &teammateId,
                          const QString &teammateName,
                          const QString &text) override;
    void sendTeammateThreadReply(const QString &channel,
                                 const QString &rootMessageId,
                                 const QString &teammateId,
                                 const QString &teammateName,
                                 const QString &text) override;
    void sendTeamNotice(const QString &channel,
                        const QString &rootMessageId,
                        const QString &text) override;
    void sendDirect(const QString &targetId, const QString &text) override;
    void sendFile(const QString &conversation, const QString &fileName,
                  const QString &mimeType, const QByteArray &data) override;
    QString sendFileWithId(const QString &channel, const QString &fileName,
                           const QString &mimeType, const QByteArray &data,
                           const QString &messageId);
    void sendReaction(const QString &conversation, const QString &messageId,
                      const QString &emoji) override;
    void editMessage(const QString &conversation, const QString &messageId,
                     const QString &newText) override;
    void deleteMessage(const QString &conversation, const QString &messageId) override;
    void sendAdminDelete(const QString &conversation, const QString &messageId,
                         qint64 ts, const QString &signature) override;
    void applyAdminDelete(const QString &conversation,
                          const QString &messageId) override;
    void setAvatar(const QByteArray &pngData) override;
    void setUserName(const QString &name) override;
    void setNodeIdentity(const QString &nodeName, const QString &ownerUser) override;
    void setUserStatus(const QJsonObject &status) override;
    void forgetMember(const QString &peerId) override;
    void sendTyping(const QString &conversation, bool active) override;
    void addChannel(const QString &channel) override;
    void removeChannel(const QString &channel) override;
    void createPrivateChannel(const QString &channel) override;
    void notifyCoveOpened(const QString &creatorKey, const QString &coveId,
                          const QString &coveName, const QString &openerKey,
                          const QString &openerName, qint64 ts,
                          const QString &signature) override;
    void notifyCoveInvited(const QString &inviteeAccount, const QString &coveId,
                           const QString &coveName, const QString &inviterName,
                           const QString &shareUrl, qint64 ts) override;
    QList<QJsonObject> networkDiagnostics() const override;
    void shutdown() override;
    QString modeName() const override { return "Mainnode"; }

signals:
    void connectionChanged(bool connected);
    void messageRecipientsKnown(const QString &messageId, int recipients,
                                const QStringList &names);
    void serverCopiesKnown(int stored);
    void roomSealPeersChanged();

private:
    struct Peer {
        QString name;
        QString nodeName;
        QString ownerUser;
        QString accountKind; // "node" or "user", from the peer's advertised accountKind
        QString solanaAddress;
        QString platform;
        QString version;
        QStringList mirrors;
        QList<MirrorAdvert> mirrorDetails; // per-repo HEAD info advertised by the peer
        qint64 memUsedBytes = 0;
        qint64 memTotalBytes = 0;
        qint64 diskUsedBytes = 0;
        qint64 diskTotalBytes = 0;
        double cpuPercent = -1.0;
        QList<NodeDiagnostics::Finding> diagnostics;
        qint64 diagnosticsMs = 0;
        qint64 lastSeenMs = 0;
        bool online = true;
        // The away flag and custom status from the peer's last frame that
        // carried one; hasUserStatus stays false for peers that never did.
        forkmesh::userstatus::UserStatus userStatus;
        bool hasUserStatus = false;
    };

    void openConnection();      // (re)create the socket and start connecting
    void scheduleReconnect();   // progressive backoff after a drop/failure
    void discardCurrentSocket();
    void closeCurrentSocket();  // send the WebSocket close handshake first
    void failCurrentConnection();
    void handleLinkLost();
    void advanceEndpoint();     // rotate m_url to the next configured mainnode
    void connectSocketSignals();
    void sendHandshake();
    void onSocketReadyRead();
    void onConnectedTransport();
    void processFrame(const QByteArray &payload);
    void sendTextFrame(const QByteArray &payload, const QString &type = QString(),
                       const QString &scope = QString(),
                       const QString &peer = QString());
    void sendControlFrame(int opcode, const QByteArray &payload = QByteArray());
    // Public #channels intentionally use controller-readable application
    // frames so integrations can participate. Private channels, DMs, and
    // control traffic remain recipient-sealed.
    bool isControllerReadableFrame(const QJsonObject &plain) const;
    void sendEncrypted(const QJsonObject &plain, bool showActivity = false,
                       bool resend = false);
    struct OutboxEntry {
        QString ref;
        QJsonObject plain;
        QSet<QString> keyIds;
        qint64 ts = 0;
    };
    void rememberOutbox(const QString &ref, const QJsonObject &plain,
                        const QJsonObject &sealed);
    void resealOutbox();
    void sendKeyAdvertisement(bool persist);
    void scheduleKeyAdvertisement();
    void sendHello(bool force = false, bool showActivity = false);
    void sampleSystemStats();
    void sampleDiagnostics();
    void sendPresence();
    void sendHistoryTo(const QString &peerId);
    void loadKnownPeers();
    void persistKnownPeers() const;
    QString hiddenChannelsSettingKey() const;
    void loadHiddenChannels();
    void saveHiddenChannels();
    // Report, once per sender key, that a retained frame was not addressed to
    // this device (or that an unencrypted frame was refused).
    void noteSealMismatch(const QString &keyId);
    void handlePlain(const QJsonObject &message, int envelopeRecipients = -1,
                     const QStringList &envelopeRecipientNames = {});
    void emitChat(const QJsonObject &message, int recipients = -1,
                  const QStringList &recipientNames = {});
    void emitDm(const QJsonObject &message, const QString &conversationPeer,
                int recipients = -1, const QStringList &recipientNames = {});
    void rememberRecipientRef(const QByteArray &frame, const QString &messageId);
    void handleRecipientsAck(const QJsonObject &envelope);
    void noteReceived(const QJsonObject &envelope);
    void flushReceived();
    void rememberPeer(const QString &peerId, const QString &name,
                      const QString &nodeName = QString(),
                      const QString &ownerUser = QString(),
                      const QString &accountKind = QString(),
                      const QString &solanaAddress = QString(),
                      const QString &platform = QString(),
                      const QString &version = QString(), bool online = true);
    void updateRosterAndStatus();
    void flushRosterAndStatus(); // does the actual roster build + emit
    // Single funnel for statusChanged. Every status the node publishes goes
    // through here so an unchanged line is never re-announced: the roster
    // flush runs on the 60s presence beat and would otherwise repeat the same
    // "N peer(s) online" text forever in the subscriber's network log.
    void emitStatus(const QString &status);
    void storeHistory(const QJsonObject &message);
    void indexMessage(const QString &id, const QString &conversation,
                      const QString &senderId);
    void dropMessageIndex(const QString &id);
    void reapStalePeers();
    void updateStoredMessage(const QString &messageId, const QString &text, bool deleted);
    bool messageIsAuthoredBy(const QString &messageId, const QString &senderId) const;
    void applyEdit(const QString &conversation, const QString &target,
                   const QString &senderId, const QString &text);
    void applyDelete(const QString &conversation, const QString &target,
                     const QString &senderId);
    void applyDeleteUnchecked(const QString &conversation, const QString &target);
    bool markSeen(const QString &messageId);
    QJsonObject makeMessage(const QString &type) const;

    QString m_userName;
    QString m_accountKind = QStringLiteral("node");
    QString m_nodeName;
    QString m_ownerUser;
    // setUserStatus(): stamped on hello/presence frames when non-empty.
    QJsonObject m_userStatus;
    QUrl m_url; // the mainnode we're currently pointed at (m_endpoints[m_endpointIndex])
    QList<QUrl> m_endpoints;
    int m_endpointIndex = 0; // which endpoint m_url currently points at
    QString m_roomName;
    QString m_solanaAddress;
    QString m_platform;
    QString m_version;
    qint64 m_memUsedBytes = 0;
    qint64 m_memTotalBytes = 0;
    qint64 m_diskUsedBytes = 0;
    qint64 m_diskTotalBytes = 0;
    double m_cpuPercent = -1.0;
    QList<NodeDiagnostics::Finding> m_diagnostics; // our own last self-check
    qint64 m_diagnosticsMs = 0;                    // when it last ran (0 = never)
    qint64 m_lastStatsSampleMs = 0; // throttle so chat sends don't re-sample
    QString m_nodeId;
    QString m_rosterStorageKey;
    RoomCrypto m_crypto;
    // Always initialized from a device-local X25519 private key. It seals
    // private channels, DMs, and control traffic before the relay-derived room
    // key sees them; explicitly public #channels use the room layer directly.
    RoomSeal m_seal;
    // Sender ids already reported through roomSealMismatch, so old history not
    // addressed to this device costs the UI one notice per sender.
    QSet<QString> m_reportedSealKeyIds;
    QTcpSocket *m_socket = nullptr;
    forkmesh::DeferredSocketRetirement m_socketRetirement;
    std::function<bool(const QUrl &)> m_connectionAuthorizer;
    QByteArray m_readBuffer;
    QHash<QString, QString> m_recipientRefs;
    QQueue<QString> m_recipientRefOrder;
    QStringList m_receivedRefs;
    bool m_receivedFlushPending = false;
    bool m_directRoom = false;
    bool m_mismatchAdvertised = false;
    QList<OutboxEntry> m_outbox;
    QByteArray m_wsKey;
    bool m_wsReady = false;
    QTimer *m_pingTimer = nullptr; // keeps the relay connection from idling out
    qint64 m_pingSentMs = 0; // when the last keepalive ping left (RTT sample)
    QTimer *m_presenceTimer = nullptr; // periodic presence beat + stale-peer sweep
    int m_presenceIntervalMs;          // kPresenceIntervalMs; shortened by tests
    QTimer *m_helloAdvertiseTimer = nullptr; // coalesces mirror hello updates
    QTimer *m_keyAdvertiseTimer = nullptr; // coalesces E2EE key replies
    QTimer *m_reconnectTimer = nullptr;
    int m_reconnectAttempts = 0; // consecutive failures since the last upgrade
    QTimer *m_connectTimeoutTimer = nullptr;
    bool m_userStopped = false;
    bool m_networkAvailable = true;
    // True for exactly one current TCP/TLS/WebSocket attempt (including an
    // upgraded live link). It makes duplicate error/disconnect callbacks
    // idempotent and prevents two reconnect timers from one failure.
    bool m_attemptActive = false;
    int m_connectTimeoutMs = 30000;
    int m_reconnectBaseDelayMs = 1000;
    qint64 m_maxPendingWriteBytes = 96ll * 1024 * 1024 + 14;
    bool m_reconnectJitter = true;
    qint64 m_backpressureDrops = 0;
    qint64 m_lastBackpressureNoticeMs = 0;
    QTimer *m_rosterEmitTimer = nullptr; // coalesces roster/status emissions
    QHash<QString, qint64> m_lastHelloReplyMs; // peer id -> last directed hello reply
    qint64 m_lastHelloSentMs = 0;
    QString m_lastStatusText;
    qint64 m_wsConnectedAtMs = 0;
    qint64 m_lastRxMs = 0;
    qint64 m_lastTxMs = 0;
    qint64 m_rxFrames = 0;
    qint64 m_txFrames = 0;
    qint64 m_rxBytes = 0;
    qint64 m_txBytes = 0;
    qint64 m_rxControlFrames = 0;
    qint64 m_txControlFrames = 0;
    int m_lastRxBytes = 0;
    int m_lastTxBytes = 0;
    QString m_lastRxType;
    QString m_lastRxScope;
    QString m_lastRxSender; // the peer the newest text frame came from
    QString m_lastTxType;
    QString m_lastTxScope;

    QStringList m_channels{"#general", "#welcome", "#random"};
    QSet<QString> m_privateChannels;
    QSet<QString> m_hiddenChannels;
    QHash<QString, Peer> m_peers;
    QHash<QString, QList<QJsonObject>> m_channelHistory;
    QHash<QString, qsizetype> m_channelHistoryChars;
    QHash<QString, QString> m_messageConversation;
    QHash<QString, QString> m_messageSender;
    QQueue<QString> m_messageIndexOrder;
    QHash<QString, QHash<QString, QHash<QString, QString>>> m_reactions;
    QByteArray m_avatarPng;
    QSet<QString> m_seenIds;
    QQueue<QString> m_seenOrder;
};
