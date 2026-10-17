#pragma once

#include "ChatBackend.h"
#include "RoomCrypto.h"
#include "RoomSeal.h"

#include <QByteArray>
#include <QHash>
#include <QMap>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVector>

#include <functional>
#include <memory>

class QNetworkAccessManager;
class QTimer;
class ServerNode;

// Office channels are independently encrypted rooms admitted with 60-second
// WebSocket tickets. The desktop authenticates with its Ed25519 identity,
// fetches retained ciphertext at startup/reconnect, and otherwise receives
// messages only through asynchronous socket pushes.
namespace forkmesh::office {

constexpr qsizetype kMaxDirectFileBytes = 1024 * 1024;
constexpr qsizetype kMaxBluetoothDirectFileBytes = 256 * 1024;
constexpr int kDirectOnlyAckMs = 3000;

QString conversationForChannel(const QString &channelName);
bool isOfficeConversation(const QString &conversation);
QString conversationForDirect(const QString &username);
bool isDirectOfficeConversation(const QString &conversation);
QString directPeerForConversation(const QString &conversation);
QString roomForConversation(const QString &conversation);
QString organizationForConversation(const QString &conversation);

QString everyoneChannelId(const QString &name);
bool isEveryoneChannelName(const QString &name);
QStringList everyoneConversations();
QString everyoneGeneralConversation();
QString everyoneWelcomeConversation();
QString worldConversation();
QString worldLabel();
bool isRetiredRelayChannel(const QString &channel);

QByteArray channelListProof(const QString &account, const QString &ts);
QByteArray channelAccessProof(const QString &account, const QString &channelId,
                              const QString &ts);
QByteArray channelHistoryProof(const QString &account, const QString &channelId,
                               const QString &ts);
QByteArray channelTopicProof(const QString &account, const QString &channelId,
                             const QString &ts, const QString &topic);
// Settings, deletion and webhooks each bind their own action to the exact
// channel, so a proof signed for one can never authorize another.
QByteArray channelSettingsProof(const QString &account,
                                const QString &channelId, const QString &ts,
                                const QString &name, const QString &visibility,
                                const QString &topic);
QByteArray channelDeleteProof(const QString &account, const QString &channelId,
                              const QString &ts);
QByteArray channelWebhooksProof(const QString &account,
                                const QString &channelId, const QString &ts,
                                const QString &method, const QString &name);
QByteArray channelWebhookProof(const QString &account, const QString &channelId,
                               const QString &webhookId, const QString &ts,
                               const QString &method);
QByteArray channelCreateProof(const QString &account, const QString &ts,
                              const QString &name, const QString &visibility,
                              const QStringList &members);
QByteArray channelMembersProof(const QString &account, const QString &channelId,
                               const QString &ts, const QString &method,
                               const QString &username);
QByteArray directListProof(const QString &account, const QString &ts);
QByteArray directStartProof(const QString &account, const QString &ts,
                            const QString &username);
QByteArray directAccessProof(const QString &account,
                             const QString &conversationId, const QString &ts);
QByteArray readMarksListProof(const QString &account, const QString &ts);
QString readMarksDigest(const QMap<QString, qint64> &marks);
QByteArray readMarksPostProof(const QString &account, const QString &ts,
                              const QMap<QString, qint64> &marks);

// One incoming webhook of a channel, as the settings panel lists it. `url` is
// filled only for a webhook this process just created or rotated: the relay
// keeps no recoverable copy of the token.
struct ChannelWebhook {
    QString id;
    QString name;
    qint64 createdAt = 0;
    qint64 lastUsedAt = 0;
    QString url;
};

// Convert one decrypted office frame into a desktop chat message. Returns false
// for anything that isn't a displayable chat frame (presence/typing/oversized).
bool chatMessageFromPlain(const QJsonObject &plain, const QString &conversation,
                          const QString &selfId, ChatMessage *out);

struct RoomMutation {
    QString type;
    QString id;
    QString target;
    QString senderId;
    QString text;
    QString emoji;
    QString reactorName;
    bool added = false;
};

bool roomMutationFromPlain(const QJsonObject &plain, RoomMutation *out);

} // namespace forkmesh::office

class OfficeChannelMirror : public QObject
{
    Q_OBJECT
public:
    explicit OfficeChannelMirror(QNetworkAccessManager *network,
                                 QObject *parent = nullptr);
    ~OfficeChannelMirror() override;

    // Signs a canonical string with this node's account identity key, in the
    // same base64url form the relay's signed endpoints expect.
    void setSigner(std::function<QString(const QByteArray &)> signer);
    void setIdentity(const QString &account, const QString &selfId,
                     const QString &displayName = QString());
    void setConnectionAuthorizer(std::function<bool(const QUrl &)> authorizer);
    void setApiBase(const QUrl &apiBase);
    struct MeshTransport {
        std::function<QList<forkmesh::chatv3::MeshLinkedDevice>()> linkedDevices;
        std::function<bool(const QString &kid, const QString &channel,
                           const QJsonObject &frame)>
            sendDirect;
        std::function<void(const QString &channel, bool on)> sendFocus;
    };
    void setMeshTransport(MeshTransport transport);
    void setChatDevice(std::shared_ptr<forkmesh::chatv3::ChatDevice> device);

    struct DirectDevice {
        QString kid;
        QString acct;
        bool own = false;
        forkmesh::chatv3::TrustState state = forkmesh::chatv3::TrustState::Pending;
        bool verified = false;
        qint64 firstSeen = 0;
    };

    void start();
    void stop();
    bool isActive() const;
    void refresh();

    QStringList conversations() const;
    QStringList membersForConversation(const QString &conversation) const;
    bool isPrivateConversation(const QString &conversation) const;
    bool isEveryoneConversation(const QString &conversation) const;
    int memberCountForConversation(const QString &conversation) const;
    bool isServerVisibleConversation(const QString &conversation) const;
    bool isReadOnlyConversation(const QString &conversation) const;
    QString topicForConversation(const QString &conversation) const;
    QString channelNameForConversation(const QString &conversation) const;
    QString channelIdFor(const QString &conversation) const;
    bool canManageConversation(const QString &conversation) const;
    bool canSend(const QString &conversation) const;
    bool sendMessage(const QString &conversation, const QString &text);
    bool canSendFile(const QString &conversation) const;
    bool sendFile(const QString &conversation, const QString &fileName,
                  const QString &mimeType, const QByteArray &data);
    bool sendBotMessage(const QString &conversation, const QString &text);
    bool sendThreadReply(const QString &conversation, const QString &rootId,
                         const QString &text);
    bool sendReaction(const QString &conversation, const QString &messageId,
                      const QString &emoji, bool added);
    bool editMessage(const QString &conversation, const QString &messageId,
                     const QString &text);
    bool deleteMessage(const QString &conversation, const QString &messageId);
    bool setTopic(const QString &conversation, const QString &topic);

    // Channel settings, all manager-only and all signed with the account key.
    bool updateChannel(const QString &conversation, const QString &name,
                       const QString &topic, bool privateRoom);
    bool deleteChannel(const QString &conversation);
    bool refreshWebhooks(const QString &conversation);
    bool createWebhook(const QString &conversation, const QString &name);
    bool rotateWebhook(const QString &conversation, const QString &webhookId);
    bool deleteWebhook(const QString &conversation, const QString &webhookId);

    bool createChannel(const QString &name, bool privateRoom,
                       const QStringList &members);
    bool addMember(const QString &conversation, const QString &username);
    bool removeMember(const QString &conversation, const QString &username);

    QStringList directConversations() const;
    bool startDirectMessage(const QString &username);
    void refreshDirectMessages();
    void watchConversation(const QString &conversation);
    QStringList directDeviceKeys(const QString &conversation) const;
    QString acceptMeshDirect(const QString &linkKid, const QString &channel,
                             const QJsonObject &frame);
    void meshDirectAcked(const QString &kid, const QString &channel,
                         const QString &messageId);
    void meshLinksChanged();
    void meshPeerFocusChanged(const QString &kid, const QString &channel, bool on);
    void setFocusedConversation(const QString &conversation);
    QString focusedConversation() const { return m_focusedConversation; }
    QString directRouteLabel(const QString &conversation) const;
    bool hasDirectSeal(const QString &conversation) const;
    QList<DirectDevice> directDevices(const QString &conversation) const;
    bool approveDirectDevice(const QString &conversation, const QString &kid);
    bool blockDirectDevice(const QString &conversation, const QString &kid);
    forkmesh::chatv3::SafetyNumber directSafetyNumber(const QString &conversation) const;
    bool isDirectVerified(const QString &conversation) const;
    void markDirectVerified(const QString &conversation);
    QString directConversationForChannel(const QString &channel) const;
    QString readMarkKey(const QString &conversation) const;
    void fetchReadMarks();
    void postReadMarks(const QHash<QString, qint64> &marks);

signals:
    void conversationsChanged(const QStringList &conversations);
    void roomMembersChanged(const QString &conversation,
                            const QStringList &members);
    void topicChanged(const QString &conversation, const QString &topic);
    // A rename keeps the same room; only the name it is filed under changes.
    void conversationRenamed(const QString &oldConversation,
                             const QString &newConversation);
    void channelSettingsSaved(const QString &conversation);
    void channelDeleted(const QString &conversation);
    void channelWebhooksChanged(const QString &conversation,
                                const QVector<forkmesh::office::ChannelWebhook>
                                    &webhooks);
    void channelSettingsFailed(const QString &conversation,
                               const QString &reason);
    void messageArrived(const ChatMessage &message);
    void messageRecipientsKnown(const QString &conversation,
                                const QString &messageId, int recipients,
                                const QStringList &names);
    void directServerCopiesKnown(const QString &conversation, int stored);
    void messagesDelivered(const QString &conversation,
                           const QStringList &messageIds);
    void conversationRead(const QString &conversation);
    void reactionChanged(const QString &conversation, const QString &messageId,
                         const QString &emoji, const QString &reactorName,
                         bool added);
    void messageEdited(const QString &conversation, const QString &messageId,
                       const QString &senderId, const QString &newText);
    void messageDeleted(const QString &conversation, const QString &messageId,
                        const QString &senderId);
    void sendActivity(const QString &text);
    void messageSendFailed(const QString &conversation, const QString &messageId,
                           const QString &text, const QString &reason);
    void channelCreated(const QString &conversation);
    void channelCreateFailed(const QString &reason);
    void directConversationsChanged(const QStringList &conversations);
    void directConversationReady(const QString &conversation);
    void directMessageFailed(const QString &username, const QString &reason);
    void readMarksFetched(const QHash<QString, qint64> &marks);
    void directDevicePending(const QString &conversation, const QString &kid);
    void directSafetyNumberChanged(const QString &conversation);
    void directRouteChanged(const QString &conversation);
    void typingChanged(const QString &conversation, const QString &peerId,
                       const QString &peerName, bool active);

private:
    struct PendingText {
        QString id;
        QString text;
        QString fileName;
        QString fileMime;
        QByteArray fileData;
        QString kind;
        QString rootId;
        QString target;
        QString emoji;
        bool added = false;
    };

    struct Room {
        QString id;
        QString name;
        QString conversation;
        bool privateRoom = false;
        bool direct = false;
        bool serverVisible = false;
        bool readOnly = false;
        bool canManage = false;
        bool everyone = false;
        int memberCount = 0;
        QString topic;
        QStringList members;
        qint64 lastTs = 0;
        bool fetching = false;
        QString room;
        RoomCrypto crypto;
        RoomSeal seal;
        QSet<QString> seenIds;
        QQueue<QString> seenOrder;
        QQueue<PendingText> pendingTexts;
        RoomSeal directSeal;
        QSet<QString> meshInFlight;
        QSet<QString> meshDelivered;
        QPointer<ServerNode> sender;
        bool accessFetching = false;
        bool senderConnected = false;
        int order = 0;
        bool wantsSocket = false;
        bool watched = false;
        int reconnectAttempts = 0;
        bool reconnectPending = false;
    };

    void fetchChannels();
    void fetchDirectMessages();
    QString upsertDirectRoom(const QJsonObject &conversation, int order);
    void sendMemberChange(const QString &conversation, const QString &username,
                          const QByteArray &method);
    void fetchEveryoneMembers();
    QString wireChannel(const Room &room) const;
    void ensureReceiver(const QString &channelId);
    void scheduleReconnect(const QString &channelId);
    void fetchHistory(const QString &channelId);
    void fetchRoomAccess(const QString &channelId);
    void openSender(const QString &channelId, const QJsonObject &payload);
    bool reservePendingSlot(Room &room);
    Room *findRoom(const QString &conversation);
    bool queueMutation(const QString &conversation, const PendingText &pending);
    void applyMutation(Room &room, const QJsonObject &plain);
    void flushPending(Room &room);
    void failPending(Room &room, const QString &reason);
    void rememberSeen(Room &room, const QString &messageId);
    struct DirectOnly {
        QString roomId;
        PendingText pending;
        QStringList kids;
        QTimer *timer = nullptr;
    };
    const RoomSeal &directSealFor(Room &room);
    void setDirectRoomName(Room &room, const QString &roomName);
    QJsonObject directPlain(const Room &room, const PendingText &pending) const;
    void sendDirectPending(Room &room, const PendingText &pending);
    int deliverOverMesh(Room &room, const QJsonObject &plain, qsizetype fileBytes,
                        bool *directOnly, QStringList *peerKids);
    void fallBackToRelay(const QString &messageId);
    void dispatchDirectPlain(Room &room, const QJsonObject &plain);
    void processHeldDirect(Room &room);
    void onDeviceEvent(forkmesh::chatv3::ChatDevice::Event event,
                       const QString &scope, const QString &kid);
    Room *roomForScope(const QString &scope);
    QString selfDirectAcct() const;
    void saveDirectDirectory() const;
    void restoreDirectDirectory();
    void discardSender(Room &room);
    void applyHistory(Room &room, const QJsonObject &payload);
    void applyChannelRecord(const QString &channelId,
                            const QJsonObject &channel);
    void applyWebhookList(const QString &channelId, const QString &conversation,
                          const QJsonArray &records);
    void rememberWebhookUrl(const QString &channelId, const QString &webhookId,
                            const QString &url);
    QUrl signedUrl(const QString &path, const QString &ts,
                   const QByteArray &canonical) const;
    bool ready() const;
    QString channelIdForConversation(const QString &conversation,
                                     bool requireManager) const;
    void reportSettingsFailure(const QString &conversation,
                               const QString &action,
                               const QString &transportError,
                               const QByteArray &body);

    QNetworkAccessManager *m_network = nullptr;
    bool m_active = false;
    bool m_socketBudgetNoted = false;
    qint64 m_lastListMs = 0;
    std::function<QString(const QByteArray &)> m_signer;
    std::function<bool(const QUrl &)> m_connectionAuthorizer;
    MeshTransport m_mesh;
    std::shared_ptr<forkmesh::chatv3::ChatDevice> m_device;
    int m_deviceToken = 0;
    QHash<QString, DirectOnly> m_directOnly;
    QString m_focusedConversation;
    QString m_account;
    QString m_selfId;
    QString m_displayName;
    QUrl m_apiBase;
    bool m_listing = false;
    bool m_relistPending = false;
    bool m_listingDirect = false;
    bool m_relistDirectPending = false;
    QStringList m_directConversations;
    bool m_everyoneMembersFetching = false;
    int m_everyoneMembersFetchedCount = -1;
    QHash<QString, Room> m_rooms; // channel id -> room
    QStringList m_conversations;  // stable display order
    // channel id -> webhooks last listed, with a URL kept only for the ones
    // this process created or rotated (the relay cannot re-issue it).
    QHash<QString, QVector<forkmesh::office::ChannelWebhook>> m_webhooks;
};
