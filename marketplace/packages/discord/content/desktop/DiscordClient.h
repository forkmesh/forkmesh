#pragma once

#include "ChatBackend.h"

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>
#include <utility>

class QNetworkAccessManager;
class QNetworkReply;
class QTcpSocket;
class QTimer;

namespace forkmesh::discord {

inline const QString kConversationPrefix = QStringLiteral("#discord/");
// The Market package this runtime ships in, and its bundled entry point.
inline const QString kPackageId = QStringLiteral("discord");
inline const QString kDesktopEntrypoint = QStringLiteral("builtin:discord");
// The bot token is package-owned data: it lives under the Market registry's
// data prefix for this package, so removing the package wipes it along with
// everything else the package kept.
inline const QString kBotTokenSetting =
    QStringLiteral("market/packages/discord/data/botToken");
// Where builds before the package split stored the token. It is migrated to
// kBotTokenSetting on the first start and removed.
inline const QString kLegacyBotTokenSetting =
    QStringLiteral("integrations/discord/botToken");
inline constexpr int kGatewayIntents =
    (1 << 0) |  // GUILDS
    (1 << 9) |  // GUILD_MESSAGES
    (1 << 10) | // GUILD_MESSAGE_REACTIONS
    (1 << 11) | // GUILD_MESSAGE_TYPING
    (1 << 12) | // DIRECT_MESSAGES
    (1 << 13) | // DIRECT_MESSAGE_REACTIONS
    (1 << 14) | // DIRECT_MESSAGE_TYPING
    (1 << 15);  // MESSAGE_CONTENT (must be enabled in the Developer Portal)

QString normalizeBotToken(const QString &token);
QString conversationForChannel(const QString &channelId);
bool isDiscordConversation(const QString &conversation);
QString channelIdFromConversation(const QString &conversation);
QString messageIdFromChatId(const QString &messageId);

// Sidebar naming. Every channel of one server shares a group named after the
// server ("Discord · Forkmesh"); under that header a channel only needs its
// short label ("#general", or "#help / question-thread" for a thread). The
// full display name is the two joined with " / ".
QString guildGroupName(const QString &guildName);
QString directMessagesGroupName();
// True for any group name this package generates (server or DM group).
bool isDiscordGroupName(const QString &group);
QString channelShortName(const QString &channelName,
                         const QString &parentName = QString());

// The REST path that adds or removes this node's reaction, or an empty string
// when the reaction cannot be addressed on Discord at all. Chat rows carry
// ForkMesh reaction slugs ("like"), which Discord rejects with "Unknown Emoji";
// the slug is translated to its literal emoji here, and a slug this build has
// no emoji for is refused rather than sent to fail.
QString reactionEndpoint(const QString &channelId, const QString &messageId,
                         const QString &value);

// Pure payload conversion used by both REST history and Gateway dispatches.
// Attachments are described on the ChatMessage; DiscordClient fills fileData
// asynchronously before delivering the row.
bool chatMessageFromPayload(const QJsonObject &payload,
                            const QString &conversation,
                            const QString &selfId,
                            ChatMessage *out);

} // namespace forkmesh::discord

// A policy-compliant Discord bot connection embedded into ForkMesh chat.
// Normal Discord user tokens are intentionally unsupported: automating them is
// a prohibited self-bot. A dedicated bot token can read and interact only in
// channels where server administrators installed it and granted permissions.
class DiscordClient final : public QObject
{
    Q_OBJECT
public:
    explicit DiscordClient(QNetworkAccessManager *network,
                           QObject *parent = nullptr);

    void setBotToken(const QString &token);
    void setConnectionAuthorizer(std::function<bool(const QUrl &)> authorizer)
    {
        m_connectionAuthorizer = std::move(authorizer);
    }
    QString botToken() const { return m_token; }
    bool isConfigured() const { return !m_token.isEmpty(); }
    bool isConnected() const { return m_gatewayReady; }
    QString botName() const { return m_botName; }
    QString botId() const { return m_botId; }

    void start();
    void stop();
    void refresh();

    QStringList conversations() const { return m_conversations; }
    QString displayName(const QString &conversation) const;
    // The sidebar group a conversation belongs to by default, and the label it
    // carries inside that group (see forkmesh::discord::guildGroupName).
    QString groupName(const QString &conversation) const;
    QString shortName(const QString &conversation) const;
    QString topic(const QString &conversation) const;
    QStringList members(const QString &conversation) const;
    QStringList mentionNames(const QString &conversation) const;
    QUrl browserUrl(const QString &conversation) const;
    // Only server channels and threads are public ForkBot destinations; DMs
    // remain private even though the configured Discord bot can technically
    // read them.
    bool isGuildConversation(const QString &conversation) const;
    bool canSend(const QString &conversation) const;

    void openConversation(const QString &conversation, bool force = false);
    bool sendMessage(const QString &conversation, const QString &text);
    bool sendReply(const QString &conversation, const QString &rootMessageId,
                   const QString &text);
    bool sendFile(const QString &conversation, const QString &fileName,
                  const QString &mimeType, const QByteArray &data);
    void toggleReaction(const QString &conversation, const QString &messageId,
                        const QString &emoji);
    void editMessage(const QString &conversation, const QString &messageId,
                     const QString &text);
    void deleteMessage(const QString &conversation, const QString &messageId);
    void sendTyping(const QString &conversation);

#ifdef FORKMESH_DISCORD_TESTS
    void setApiBaseForTests(const QUrl &base) { m_apiBase = base; }
    void setGatewayUrlForTests(const QUrl &url) { m_gatewayUrl = url; }
#endif

signals:
    void conversationsChanged(const QStringList &conversations);
    void messageArrived(const ChatMessage &message);
    void reactionChanged(const QString &conversation,
                         const QString &messageId, const QString &emoji,
                         const QString &reactorName, bool added);
    void reactionsLoaded(const QString &conversation,
                         const QString &messageId,
                         const QMap<QString, QStringList> &reactions);
    void messageEdited(const QString &conversation, const QString &messageId,
                       const QString &newText);
    void messageDeleted(const QString &conversation,
                        const QString &messageId);
    void typingChanged(const QString &conversation, const QString &peerId,
                       const QString &peerName, bool active);
    void avatarChanged(const QString &peerId, const QByteArray &pngData);
    void membersChanged(const QString &conversation);
    void conversationMetadataChanged(const QString &conversation);
    void topicChanged(const QString &conversation, const QString &topic);
    void statusChanged(const QString &status, bool connected, bool error);
    void activity(const QString &text);
    void sendFailed(const QString &conversation, const QString &text,
                    const QString &reason);

private:
    struct Guild {
        QString id;
        QString name;
        QString icon;
        int order = 0;
    };

    struct Channel {
        QString id;
        QString guildId;
        QString name;
        QString topic;
        QString parentId;
        QString recipientName;
        int type = -1;
        int position = 0;
        bool direct = false;
        bool historyLoaded = false;
        bool historyLoading = false;
        QHash<QString, QString> memberNames; // Discord user id -> display name
    };

    using ApiCallback =
        std::function<void(int status, const QByteArray &body,
                           const QString &error)>;

    void fetchIdentity();
    void fetchGuilds();
    void fetchNextGuild();
    void fetchGuildChannels(const QString &guildId);
    void fetchGuildThreads(const QString &guildId);
    void applyChannels(const QString &guildId, const QJsonArray &channels,
                       bool threads);
    void applyChannel(const QJsonObject &payload,
                      const QString &fallbackGuildId = QString());
    void removeChannel(const QString &channelId);
    void rebuildConversations(bool forceSignal = false);
    void ensureDirectChannel(const QJsonObject &messagePayload);

    void deliverPayload(const QJsonObject &payload,
                        const QString &conversation = QString(),
                        bool allowExisting = false,
                        bool historical = false,
                        const QByteArray &knownAttachment = QByteArray());
    void deliverConverted(ChatMessage message);
    void emitReactionSnapshot(const QString &conversation,
                              const QString &messageId,
                              const QJsonArray &reactions);
    void rememberMember(Channel &channel, const QJsonObject &payload);
    QString senderName(const QJsonObject &payload) const;
    void fetchAvatar(const QJsonObject &author);
    void fetchAttachment(ChatMessage message, const QString &fallbackUrl);

    void apiRequest(const QByteArray &method, const QString &path,
                    const QByteArray &body, const QByteArray &contentType,
                    ApiCallback callback, int attempt = 0);
    void handleApiFailure(const QString &operation, int status,
                          const QString &error,
                          const QString &conversation = QString(),
                          const QString &originalText = QString());
    QNetworkReply *startApiRequest(const QByteArray &method,
                                   const QString &path,
                                   const QByteArray &body,
                                   const QByteArray &contentType);
    QByteArray multipartBody(const QString &boundary, const QString &fileName,
                             const QString &mimeType, const QByteArray &data,
                             const QJsonObject &payload) const;
    QString encodeMentions(const Channel &channel, const QString &text) const;
    QJsonObject outgoingMessage(const QString &text,
                                const QString &rootMessageId = QString()) const;

    void openGateway();
    void scheduleGatewayReconnect(bool resetSession = false,
                                  int minimumDelayMs = 0);
    void discardGatewaySocket();
    void closeGatewaySocket(quint16 code = 1000,
                            const QByteArray &reason = QByteArray());
    void connectGatewaySignals();
    void sendGatewayHandshake();
    void readGateway();
    void consumeGatewayFrames();
    void processGatewayPayload(const QByteArray &payload);
    void processGatewayDispatch(const QString &type,
                                const QJsonObject &payload);
    void sendGatewayJson(const QJsonObject &payload);
    void sendGatewayFrame(int opcode, const QByteArray &payload);
    void sendGatewayHeartbeat();
    void sendGatewayHeartbeatNow(bool requirePreviousAck);
    void identifyGateway();
    void resumeGateway();
    void gatewayLost(const QString &reason = QString());
    void setStatus(const QString &status, bool connected, bool error);

    QString conversationForChannelId(const QString &channelId) const;
    Channel *channelForConversation(const QString &conversation);
    const Channel *channelForConversation(const QString &conversation) const;
    static bool supportedChannelType(int type);
    static QString boundedDiscordError(int status, const QByteArray &body,
                                       const QString &networkError);

    QNetworkAccessManager *m_network = nullptr;
    std::function<bool(const QUrl &)> m_connectionAuthorizer;
    QString m_token;
    QString m_botId;
    QString m_botName;
    QUrl m_apiBase{QStringLiteral("https://discord.com/api/v10")};
    QUrl m_gatewayUrl{
        QStringLiteral("wss://gateway.discord.gg/?v=10&encoding=json")};
    QUrl m_gatewayCurrentUrl;
    bool m_active = false;
    bool m_identityLoading = false;
    bool m_guildsLoading = false;
    bool m_refreshPending = false;
    quint64 m_connectionGeneration = 0;
    qint64 m_globalRateLimitUntilMs = 0;
    QHash<QString, Guild> m_guilds;
    QHash<QString, Channel> m_channels;
    QStringList m_conversations;
    QQueue<QString> m_guildFetchQueue;
    QSet<QString> m_seenMessages;
    QQueue<QString> m_seenMessageOrder;
    QHash<QString, QString> m_avatarHashes; // actor id -> requested avatar hash
    QHash<QString, QSet<QString>> m_selfReactions; // chat id -> emoji set
    QHash<QString, qint64> m_lastTypingSentMs;     // channel id -> timestamp
    QHash<QString, qint64> m_typingGenerations;    // channel:user -> timestamp

    QTcpSocket *m_gatewaySocket = nullptr;
    QByteArray m_gatewayReadBuffer;
    QByteArray m_gatewayMessageBuffer;
    QByteArray m_gatewayKey;
    int m_gatewayFragmentOpcode = 0;
    bool m_gatewayUpgraded = false;
    bool m_gatewayReady = false;
    bool m_gatewayAttemptActive = false;
    bool m_gatewayHeartbeatAcked = true;
    int m_gatewayHeartbeatIntervalMs = 0;
    int m_gatewayReconnectAttempts = 0;
    qint64 m_gatewaySequence = -1;
    QString m_gatewaySessionId;
    QString m_gatewayFatalStatus;
    QUrl m_gatewayResumeUrl;
    QTimer *m_gatewayHeartbeatTimer = nullptr;
    QTimer *m_gatewayReconnectTimer = nullptr;
    QTimer *m_gatewayConnectTimer = nullptr;
};
