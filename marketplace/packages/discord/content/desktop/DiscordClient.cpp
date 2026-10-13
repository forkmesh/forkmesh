#include "DiscordClient.h"
#include "ForkMeshVersion.h"
#include "ReactionCatalog.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonParseError>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSslSocket>
#include <QSysInfo>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>
#include <climits>
#include <utility>

#ifndef FORKMESH_VERSION
#define FORKMESH_VERSION "dev"
#endif

namespace {

constexpr int kDiscordTextLimit = 2000;
constexpr qint64 kMaxUploadBytes = 10ll * 1024 * 1024;
constexpr qint64 kMaxDownloadBytes = 25ll * 1024 * 1024;
constexpr qint64 kMaxApiResponseBytes = 32ll * 1024 * 1024;
constexpr int kMaxChannels = 1000;
constexpr int kMaxSeenMessages = 5000;
constexpr int kMaxGatewayPayload = 16 * 1024 * 1024;
constexpr int kGatewayConnectTimeoutMs = 30000;
constexpr int kGatewayReconnectBaseMs = 1000;
constexpr int kGatewayReconnectMaxMs = 5 * 60 * 1000;

bool isSnowflake(const QString &value)
{
    if (value.isEmpty() || value.size() > 20)
        return false;
    for (const QChar character : value) {
        if (!character.isDigit())
            return false;
    }
    return true;
}

bool isAsciiOnly(const QString &value)
{
    for (const QChar character : value) {
        if (character.unicode() > 0x7F)
            return false;
    }
    return true;
}

// A server's own emoji is addressed as "name:id" rather than by unicode.
bool isCustomDiscordEmoji(const QString &value)
{
    const qsizetype colon = value.lastIndexOf(QLatin1Char(':'));
    return colon > 0 && isSnowflake(value.mid(colon + 1));
}

QString discordDisplayName(const QJsonObject &payload)
{
    const QJsonObject member = payload.value(QStringLiteral("member")).toObject();
    const QString nick = member.value(QStringLiteral("nick")).toString().trimmed();
    if (!nick.isEmpty())
        return nick.left(80);
    QJsonObject author = payload.value(QStringLiteral("author")).toObject();
    if (author.isEmpty())
        author = member.value(QStringLiteral("user")).toObject();
    const QString global =
        author.value(QStringLiteral("global_name")).toString().trimmed();
    if (!global.isEmpty())
        return global.left(80);
    const QString username =
        author.value(QStringLiteral("username")).toString().trimmed();
    return username.isEmpty() ? QStringLiteral("Discord user")
                              : username.left(80);
}

qint64 discordTimestamp(const QJsonObject &payload)
{
    const QString timestamp =
        payload.value(QStringLiteral("timestamp")).toString();
    QDateTime parsed = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
    if (!parsed.isValid())
        parsed = QDateTime::fromString(timestamp, Qt::ISODate);
    if (parsed.isValid())
        return parsed.toMSecsSinceEpoch();

    bool ok = false;
    const quint64 snowflake =
        payload.value(QStringLiteral("id")).toString().toULongLong(&ok);
    if (ok)
        return qint64((snowflake >> 22) + 1420070400000ull);
    return QDateTime::currentMSecsSinceEpoch();
}

QString discordEmoji(const QJsonObject &emoji)
{
    const QString name = emoji.value(QStringLiteral("name")).toString();
    const QString id = emoji.value(QStringLiteral("id")).toString();
    if (!id.isEmpty())
        return name + QLatin1Char(':') + id; // a server's custom emoji
    // Unicode reactions come back spelled the way Discord stores them, so fold
    // them onto the slug the picker emits: a reaction this node added is then
    // recognized as its own (making the chip a real toggle) and renders as the
    // same drawing the user clicked.
    return reactions::valueForUnicode(name);
}

bool isDiscordCdn(const QUrl &url)
{
    if (url.scheme() != QLatin1String("https"))
        return false;
    const QString host = url.host().toLower();
    return host == QLatin1String("cdn.discordapp.com") ||
           host == QLatin1String("media.discordapp.net") ||
           host.endsWith(QLatin1String(".discordapp.com")) ||
           host.endsWith(QLatin1String(".discordapp.net"));
}

bool isDiscordGateway(const QUrl &url)
{
    const QString host = url.host().toLower();
    if (url.scheme() == QLatin1String("wss") &&
        (host == QLatin1String("discord.gg") ||
         host.endsWith(QLatin1String(".discord.gg")))) {
        return url.userInfo().isEmpty();
    }
#ifdef FORKMESH_DISCORD_TESTS
    return url.scheme() == QLatin1String("ws") &&
           (host == QLatin1String("127.0.0.1") ||
            host == QLatin1String("localhost") || host == QLatin1String("::1"));
#else
    return false;
#endif
}

QByteArray randomWebSocketKey()
{
    QByteArray bytes(16, Qt::Uninitialized);
    for (int i = 0; i < bytes.size(); ++i)
        bytes[i] = char(QRandomGenerator::global()->bounded(256));
    return bytes.toBase64();
}

QString webSocketRequestTarget(const QUrl &url)
{
    QString target = url.path().isEmpty() ? QStringLiteral("/") : url.path();
    if (url.hasQuery())
        target += QLatin1Char('?') + url.query(QUrl::FullyEncoded);
    return target;
}

QString safeUploadFileName(const QString &name)
{
    QString safe = name.trimmed().left(180);
    safe.remove(QLatin1Char('\r'));
    safe.remove(QLatin1Char('\n'));
    safe.replace(QLatin1Char('"'), QLatin1Char('_'));
    safe.replace(QLatin1Char('\\'), QLatin1Char('_'));
    safe.replace(QLatin1Char('/'), QLatin1Char('_'));
    return safe.isEmpty() ? QStringLiteral("attachment.bin") : safe;
}

} // namespace

namespace forkmesh::discord {

QString normalizeBotToken(const QString &token)
{
    QString normalized = token.trimmed();
    if (normalized.startsWith(QStringLiteral("Bot "), Qt::CaseInsensitive))
        normalized = normalized.mid(4).trimmed();
    if (normalized.isEmpty() || normalized.size() > 512 ||
        normalized.contains(QLatin1Char('\r')) ||
        normalized.contains(QLatin1Char('\n')) ||
        normalized.contains(QLatin1Char(' ')) ||
        normalized.contains(QLatin1Char('\t'))) {
        return QString();
    }
    return normalized;
}

QString conversationForChannel(const QString &channelId)
{
    const QString id = channelId.trimmed();
    return isSnowflake(id) ? kConversationPrefix + id : QString();
}

bool isDiscordConversation(const QString &conversation)
{
    return !channelIdFromConversation(conversation).isEmpty();
}

QString channelIdFromConversation(const QString &conversation)
{
    if (!conversation.startsWith(kConversationPrefix))
        return QString();
    const QString id = conversation.mid(kConversationPrefix.size());
    return isSnowflake(id) ? id : QString();
}

QString messageIdFromChatId(const QString &messageId)
{
    static const QString prefix = QStringLiteral("discord-message:");
    const QString id = messageId.startsWith(prefix)
                           ? messageId.mid(prefix.size())
                           : messageId;
    return isSnowflake(id) ? id : QString();
}

QString reactionEndpoint(const QString &channelId, const QString &messageId,
                         const QString &value)
{
    const QString trimmed = value.trimmed().left(100);
    if (!isSnowflake(channelId) || !isSnowflake(messageId) || trimmed.isEmpty())
        return {};
    QString emoji = trimmed;
    if (reactions::hasUnicode(trimmed)) {
        emoji = reactions::unicodeForValue(trimmed);
    } else if (!isCustomDiscordEmoji(trimmed) && isAsciiOnly(trimmed)) {
        // A slug with no emoji in the catalog: Discord can only answer
        // "Unknown Emoji", so decline instead of burning the request.
        return {};
    }
    return QStringLiteral("/channels/%1/messages/%2/reactions/%3/@me")
        .arg(channelId, messageId,
             QString::fromLatin1(QUrl::toPercentEncoding(emoji)));
}

bool chatMessageFromPayload(const QJsonObject &payload,
                            const QString &conversation,
                            const QString &selfId,
                            ChatMessage *out)
{
    if (!out || !isDiscordConversation(conversation))
        return false;
    const QString id = payload.value(QStringLiteral("id")).toString();
    const QString channelId =
        payload.value(QStringLiteral("channel_id")).toString();
    if (!isSnowflake(id) || !isSnowflake(channelId) ||
        channelIdFromConversation(conversation) != channelId) {
        return false;
    }

    const QJsonObject author =
        payload.value(QStringLiteral("author")).toObject();
    const QString authorId = author.value(QStringLiteral("id")).toString();
    if (!isSnowflake(authorId))
        return false;

    QString text = payload.value(QStringLiteral("content")).toString();
    for (const QJsonValue &value :
         payload.value(QStringLiteral("mentions")).toArray()) {
        const QJsonObject mention = value.toObject();
        const QString mentionId = mention.value(QStringLiteral("id")).toString();
        QString mentionName =
            mention.value(QStringLiteral("global_name")).toString().trimmed();
        if (mentionName.isEmpty())
            mentionName =
                mention.value(QStringLiteral("username")).toString().trimmed();
        if (isSnowflake(mentionId) && !mentionName.isEmpty()) {
            text.replace(QStringLiteral("<@%1>").arg(mentionId),
                         QLatin1Char('@') + mentionName);
            text.replace(QStringLiteral("<@!%1>").arg(mentionId),
                         QLatin1Char('@') + mentionName);
        }
    }

    QStringList embedLines;
    for (const QJsonValue &value :
         payload.value(QStringLiteral("embeds")).toArray()) {
        const QJsonObject embed = value.toObject();
        const QString title =
            embed.value(QStringLiteral("title")).toString().trimmed();
        const QString description =
            embed.value(QStringLiteral("description")).toString().trimmed();
        const QString url =
            embed.value(QStringLiteral("url")).toString().trimmed();
        const QString imageUrl =
            embed.value(QStringLiteral("image"))
                .toObject()
                .value(QStringLiteral("url"))
                .toString()
                .trimmed();
        QString line;
        if (!title.isEmpty())
            line += QStringLiteral("**%1**").arg(title);
        if (!description.isEmpty()) {
            if (!line.isEmpty())
                line += QLatin1Char('\n');
            line += description;
        }
        if (!url.isEmpty() && !line.contains(url)) {
            if (!line.isEmpty())
                line += QLatin1Char('\n');
            line += url;
        }
        if (!imageUrl.isEmpty() && !line.contains(imageUrl)) {
            if (!line.isEmpty())
                line += QLatin1Char('\n');
            line += imageUrl;
        }
        if (!line.isEmpty())
            embedLines.append(line);
    }
    for (const QJsonValue &value :
         payload.value(QStringLiteral("sticker_items")).toArray()) {
        const QString name =
            value.toObject().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty())
            embedLines.append(QStringLiteral("Sticker: %1").arg(name));
    }
    const QJsonObject poll = payload.value(QStringLiteral("poll")).toObject();
    const QString pollQuestion =
        poll.value(QStringLiteral("question"))
            .toObject()
            .value(QStringLiteral("text"))
            .toString()
            .trimmed();
    if (!pollQuestion.isEmpty()) {
        QStringList pollLines{QStringLiteral("Poll: %1").arg(pollQuestion)};
        for (const QJsonValue &value :
             poll.value(QStringLiteral("answers")).toArray()) {
            const QString answer =
                value.toObject()
                    .value(QStringLiteral("poll_media"))
                    .toObject()
                    .value(QStringLiteral("text"))
                    .toString()
                    .trimmed();
            if (!answer.isEmpty())
                pollLines.append(QStringLiteral("• %1").arg(answer));
        }
        embedLines.append(pollLines.join(QLatin1Char('\n')));
    }
    if (!embedLines.isEmpty()) {
        if (!text.isEmpty())
            text += QStringLiteral("\n\n");
        text += embedLines.join(QStringLiteral("\n\n"));
    }

    const int type = payload.value(QStringLiteral("type")).toInt();
    if (text.trimmed().isEmpty()) {
        if (type == 6)
            text = QStringLiteral("Pinned a message to this channel.");
        else if (type == 7)
            text = QStringLiteral("Joined the server.");
        else if (type == 18)
            text = QStringLiteral("Started a thread.");
    }

    ChatMessage message;
    message.id = QStringLiteral("discord-message:") + id;
    message.conversation = conversation;
    message.senderId = QStringLiteral("discord-user:") + authorId;
    message.senderName = discordDisplayName(payload);
    message.text = text.left(16000);
    message.timestampMs = discordTimestamp(payload);
    message.self = !selfId.isEmpty() && authorId == selfId;
    message.edited =
        !payload.value(QStringLiteral("edited_timestamp")).isNull() &&
        !payload.value(QStringLiteral("edited_timestamp")).toString().isEmpty();

    const QString referenced =
        payload.value(QStringLiteral("message_reference"))
            .toObject()
            .value(QStringLiteral("message_id"))
            .toString();
    if (isSnowflake(referenced))
        message.threadRootId = QStringLiteral("discord-message:") + referenced;

    const QJsonArray attachments =
        payload.value(QStringLiteral("attachments")).toArray();
    if (!attachments.isEmpty()) {
        const QJsonObject attachment = attachments.first().toObject();
        message.fileName =
            attachment.value(QStringLiteral("filename")).toString().left(180);
        message.fileMime =
            attachment.value(QStringLiteral("content_type")).toString().left(100);
    }
    if (message.text.trimmed().isEmpty() && message.fileName.isEmpty())
        return false;
    *out = message;
    return true;
}

} // namespace forkmesh::discord

DiscordClient::DiscordClient(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_network(network)
{
}

void DiscordClient::setBotToken(const QString &token)
{
    const QString normalized = forkmesh::discord::normalizeBotToken(token);
    if (normalized == m_token)
        return;
    const bool restart = m_active;
    stop();
    m_token = normalized;
    m_botId.clear();
    m_botName.clear();
    m_seenMessages.clear();
    m_seenMessageOrder.clear();
    m_avatarHashes.clear();
    m_selfReactions.clear();
    m_globalRateLimitUntilMs = 0;
    if (restart && !m_token.isEmpty())
        start();
}

void DiscordClient::start()
{
    if (m_active || !m_network || m_token.isEmpty())
        return;
    m_active = true;
    m_gatewayReconnectAttempts = 0;
    m_gatewayFatalStatus.clear();
    setStatus(QStringLiteral("Discord · connecting"), false, false);
    fetchIdentity();
}

void DiscordClient::stop()
{
    // Every asynchronous REST/CDN callback captures this generation. Bumping
    // it prevents a request made with an old token from mutating a later
    // connection after Disconnect or Connect has returned.
    ++m_connectionGeneration;
    m_active = false;
    m_identityLoading = false;
    m_guildsLoading = false;
    m_refreshPending = false;
    m_guildFetchQueue.clear();
    if (m_gatewayReconnectTimer)
        m_gatewayReconnectTimer->stop();
    if (m_gatewayConnectTimer)
        m_gatewayConnectTimer->stop();
    if (m_gatewayHeartbeatTimer)
        m_gatewayHeartbeatTimer->stop();
    closeGatewaySocket();
    m_gatewayReady = false;
    m_gatewayAttemptActive = false;
    m_gatewaySessionId.clear();
    m_gatewayFatalStatus.clear();
    m_gatewayResumeUrl.clear();
    m_gatewaySequence = -1;
    m_typingGenerations.clear();
    m_lastTypingSentMs.clear();

    m_guilds.clear();
    m_channels.clear();
    if (!m_conversations.isEmpty()) {
        m_conversations.clear();
        emit conversationsChanged(m_conversations);
    }
    setStatus(m_token.isEmpty() ? QStringLiteral("Discord · not connected")
                                : QStringLiteral("Discord · disconnected"),
              false, false);
}

void DiscordClient::refresh()
{
    if (!m_active || m_token.isEmpty())
        return;
    if (m_identityLoading || m_guildsLoading || !m_guildFetchQueue.isEmpty()) {
        m_refreshPending = true;
        return;
    }
    fetchGuilds();
}

void DiscordClient::setStatus(const QString &status, bool connected, bool error)
{
    emit statusChanged(status, connected, error);
}

void DiscordClient::fetchIdentity()
{
    if (!m_active || m_identityLoading)
        return;
    m_identityLoading = true;
    apiRequest(QByteArrayLiteral("GET"), QStringLiteral("/users/@me"), {}, {},
               [this](int status, const QByteArray &body, const QString &error) {
                   m_identityLoading = false;
                   if (!m_active)
                       return;
                   QJsonParseError parseError;
                   const QJsonDocument document =
                       QJsonDocument::fromJson(body, &parseError);
                   const QJsonObject user = document.object();
                   const QString id = user.value(QStringLiteral("id")).toString();
                   if (status < 200 || status >= 300 || !isSnowflake(id) ||
                       !user.value(QStringLiteral("bot")).toBool()) {
                       const bool nonBotAccount =
                           status >= 200 && status < 300 && document.isObject() &&
                           isSnowflake(id) &&
                           !user.value(QStringLiteral("bot")).toBool();
                       const QString reason =
                           status == 401
                               ? QStringLiteral("the bot token was rejected")
                               : nonBotAccount
                                     ? QStringLiteral("the token is not a bot account")
                                     : status >= 200 && status < 300
                                           ? QStringLiteral(
                                                 "Discord returned an invalid "
                                                 "identity response")
                                           : boundedDiscordError(status, body,
                                                                 error);
                       setStatus(QStringLiteral("Discord · %1").arg(reason),
                                 false, true);
                       emit activity(QStringLiteral("Discord: %1.").arg(reason));
                       if (status == 401 || nonBotAccount) {
                           m_active = false;
                           ++m_connectionGeneration;
                       }
                       return;
                   }
                   m_botId = id;
                   m_botName =
                       user.value(QStringLiteral("global_name")).toString().trimmed();
                   if (m_botName.isEmpty())
                       m_botName =
                           user.value(QStringLiteral("username")).toString().trimmed();
                   if (m_botName.isEmpty())
                       m_botName = QStringLiteral("bot");
                   setStatus(QStringLiteral("Discord · %1 · loading channels")
                                 .arg(m_botName),
                             false, false);
                   fetchAvatar(user);
                   fetchGuilds();
                   openGateway();
               });
}

void DiscordClient::fetchGuilds()
{
    if (!m_active || m_guildsLoading || m_botId.isEmpty())
        return;
    m_guildsLoading = true;
    apiRequest(QByteArrayLiteral("GET"), QStringLiteral("/users/@me/guilds"),
               {}, {},
               [this](int status, const QByteArray &body, const QString &error) {
                   m_guildsLoading = false;
                   if (!m_active)
                       return;
                   const QJsonDocument document = QJsonDocument::fromJson(body);
                   if (status < 200 || status >= 300 || !document.isArray()) {
                       handleApiFailure(QStringLiteral("list servers"), status,
                                        boundedDiscordError(status, body, error));
                       return;
                   }

                   QHash<QString, Channel> directChannels;
                   for (auto it = m_channels.constBegin();
                        it != m_channels.constEnd(); ++it) {
                       if (it->direct)
                           directChannels.insert(it.key(), it.value());
                   }
                   m_channels = std::move(directChannels);
                   m_guilds.clear();
                   m_guildFetchQueue.clear();
                   int order = 0;
                   for (const QJsonValue &value : document.array()) {
                       if (m_guilds.size() >= 200)
                           break;
                       const QJsonObject object = value.toObject();
                       Guild guild;
                       guild.id = object.value(QStringLiteral("id")).toString();
                       guild.name =
                           object.value(QStringLiteral("name")).toString().trimmed().left(100);
                       guild.icon = object.value(QStringLiteral("icon")).toString();
                       guild.order = order++;
                       if (!isSnowflake(guild.id) || guild.name.isEmpty())
                           continue;
                       m_guilds.insert(guild.id, guild);
                       m_guildFetchQueue.enqueue(guild.id);
                   }
                   if (m_guilds.isEmpty()) {
                       rebuildConversations(true);
                       if (!m_gatewayFatalStatus.isEmpty())
                           setStatus(m_gatewayFatalStatus, false, true);
                       else
                           setStatus(QStringLiteral("Discord · %1 · no servers")
                                         .arg(m_botName),
                                     m_gatewayReady, false);
                       emit activity(QStringLiteral(
                           "Discord: the bot is not installed in any servers yet."));
                       return;
                   }
                   fetchNextGuild();
               });
}

void DiscordClient::fetchNextGuild()
{
    if (!m_active)
        return;
    if (m_guildFetchQueue.isEmpty()) {
        rebuildConversations(true);
        if (!m_gatewayFatalStatus.isEmpty()) {
            setStatus(m_gatewayFatalStatus, false, true);
        } else {
            setStatus(QStringLiteral("Discord · %1 · %2 channel%3")
                          .arg(m_botName)
                          .arg(m_conversations.size())
                          .arg(m_conversations.size() == 1
                                   ? QString()
                                   : QStringLiteral("s")),
                      m_gatewayReady, false);
        }
        if (m_refreshPending) {
            m_refreshPending = false;
            QTimer::singleShot(0, this, &DiscordClient::fetchGuilds);
        }
        return;
    }
    fetchGuildChannels(m_guildFetchQueue.head());
}

void DiscordClient::fetchGuildChannels(const QString &guildId)
{
    apiRequest(QByteArrayLiteral("GET"),
               QStringLiteral("/guilds/%1/channels").arg(guildId), {}, {},
               [this, guildId](int status, const QByteArray &body,
                               const QString &error) {
                   if (!m_active)
                       return;
                   const QJsonDocument document = QJsonDocument::fromJson(body);
                   if (status >= 200 && status < 300 && document.isArray()) {
                       applyChannels(guildId, document.array(), false);
                   } else {
                       emit activity(
                           QStringLiteral("Discord: could not list channels in %1 (%2).")
                               .arg(m_guilds.value(guildId).name,
                                    boundedDiscordError(status, body, error)));
                   }
                   fetchGuildThreads(guildId);
               });
}

void DiscordClient::fetchGuildThreads(const QString &guildId)
{
    apiRequest(QByteArrayLiteral("GET"),
               QStringLiteral("/guilds/%1/threads/active").arg(guildId), {}, {},
               [this, guildId](int status, const QByteArray &body,
                               const QString &) {
                   if (!m_active)
                       return;
                   if (status >= 200 && status < 300) {
                       const QJsonArray threads =
                           QJsonDocument::fromJson(body)
                               .object()
                               .value(QStringLiteral("threads"))
                               .toArray();
                       applyChannels(guildId, threads, true);
                   }
                   if (!m_guildFetchQueue.isEmpty() &&
                       m_guildFetchQueue.head() == guildId) {
                       m_guildFetchQueue.dequeue();
                   } else {
                       m_guildFetchQueue.removeAll(guildId);
                   }
                   // Spread a large server list over time instead of bursting
                   // through Discord's global REST limit.
                   QTimer::singleShot(75, this, &DiscordClient::fetchNextGuild);
               });
}

void DiscordClient::applyChannels(const QString &guildId,
                                  const QJsonArray &channels, bool)
{
    for (const QJsonValue &value : channels)
        applyChannel(value.toObject(), guildId);
}

bool DiscordClient::supportedChannelType(int type)
{
    return type == 0 || type == 2 || type == 5 || type == 10 || type == 11 ||
           type == 12;
}

void DiscordClient::applyChannel(const QJsonObject &payload,
                                 const QString &fallbackGuildId)
{
    const QString id = payload.value(QStringLiteral("id")).toString();
    const int type = payload.value(QStringLiteral("type")).toInt(-1);
    if (!isSnowflake(id))
        return;
    const bool existed = m_channels.contains(id);

    if (type == 1) {
        Channel channel = m_channels.value(id);
        channel.id = id;
        channel.guildId.clear();
        channel.name = QStringLiteral("direct-message");
        channel.topic.clear();
        channel.parentId.clear();
        channel.type = type;
        channel.position = 0;
        channel.direct = true;
        for (const QJsonValue &value :
             payload.value(QStringLiteral("recipients")).toArray()) {
            const QJsonObject recipient = value.toObject();
            const QString recipientId =
                recipient.value(QStringLiteral("id")).toString();
            if (!isSnowflake(recipientId) || recipientId == m_botId)
                continue;
            const QString name = discordDisplayName(
                QJsonObject{{QStringLiteral("author"), recipient}});
            channel.recipientName = name;
            channel.memberNames.insert(recipientId, name);
            fetchAvatar(recipient);
            break;
        }
        m_channels.insert(id, channel);
        if (existed)
            emit conversationMetadataChanged(conversationForChannelId(id));
        return;
    }

    if (!supportedChannelType(type) && type != 4)
        return;
    QString guildId =
        payload.value(QStringLiteral("guild_id")).toString(fallbackGuildId);
    if (!isSnowflake(guildId) || !m_guilds.contains(guildId))
        return;

    Channel channel = m_channels.value(id);
    const QString previousTopic = channel.topic;
    channel.id = id;
    channel.guildId = guildId;
    channel.name =
        payload.value(QStringLiteral("name")).toString().trimmed().left(100);
    channel.topic =
        payload.value(QStringLiteral("topic")).toString().trimmed().left(1024);
    channel.parentId =
        payload.value(QStringLiteral("parent_id")).toString().left(20);
    channel.type = type;
    channel.position = payload.value(QStringLiteral("position")).toInt();
    channel.direct = false;
    if (channel.name.isEmpty())
        channel.name = type >= 10 && type <= 12 ? QStringLiteral("thread")
                                                : QStringLiteral("channel");
    m_channels.insert(id, channel);
    if (existed) {
        emit conversationMetadataChanged(conversationForChannelId(id));
        if (type == 4) {
            for (const Channel &child : std::as_const(m_channels)) {
                if (child.parentId == id)
                    emit conversationMetadataChanged(
                        conversationForChannelId(child.id));
            }
        }
    }
    if (previousTopic != channel.topic && !previousTopic.isNull())
        emit topicChanged(conversationForChannelId(id), channel.topic);
}

void DiscordClient::removeChannel(const QString &channelId)
{
    if (!m_channels.remove(channelId))
        return;
    rebuildConversations();
}

void DiscordClient::rebuildConversations(bool forceSignal)
{
    QList<Channel> channels;
    for (const Channel &channel : std::as_const(m_channels)) {
        if (supportedChannelType(channel.type) || channel.direct)
            channels.append(channel);
    }
    std::sort(channels.begin(), channels.end(), [this](const Channel &left,
                                                        const Channel &right) {
        if (left.direct != right.direct)
            return !left.direct;
        const Guild leftGuild = m_guilds.value(left.guildId);
        const Guild rightGuild = m_guilds.value(right.guildId);
        if (leftGuild.order != rightGuild.order)
            return leftGuild.order < rightGuild.order;
        const int leftParent = m_channels.value(left.parentId).position;
        const int rightParent = m_channels.value(right.parentId).position;
        if (leftParent != rightParent)
            return leftParent < rightParent;
        if (left.position != right.position)
            return left.position < right.position;
        const QString leftName = left.direct ? left.recipientName : left.name;
        const QString rightName = right.direct ? right.recipientName : right.name;
        const int byName = leftName.compare(rightName, Qt::CaseInsensitive);
        return byName == 0 ? left.id < right.id : byName < 0;
    });

    QStringList next;
    next.reserve(qMin(kMaxChannels, channels.size()));
    for (const Channel &channel : std::as_const(channels)) {
        if (next.size() >= kMaxChannels)
            break;
        const QString conversation = conversationForChannelId(channel.id);
        if (!conversation.isEmpty())
            next.append(conversation);
    }
    if (next == m_conversations) {
        if (forceSignal)
            emit conversationsChanged(m_conversations);
        return;
    }
    m_conversations = next;
    emit conversationsChanged(m_conversations);
}

QString DiscordClient::conversationForChannelId(const QString &channelId) const
{
    return forkmesh::discord::conversationForChannel(channelId);
}

DiscordClient::Channel *DiscordClient::channelForConversation(
    const QString &conversation)
{
    const QString id = forkmesh::discord::channelIdFromConversation(conversation);
    auto it = m_channels.find(id);
    return it == m_channels.end() ? nullptr : &it.value();
}

const DiscordClient::Channel *DiscordClient::channelForConversation(
    const QString &conversation) const
{
    const QString id = forkmesh::discord::channelIdFromConversation(conversation);
    auto it = m_channels.constFind(id);
    return it == m_channels.constEnd() ? nullptr : &it.value();
}

bool DiscordClient::isGuildConversation(const QString &conversation) const
{
    const Channel *channel = channelForConversation(conversation);
    return channel && !channel->direct && !channel->guildId.isEmpty();
}

QString forkmesh::discord::guildGroupName(const QString &guildName)
{
    return QStringLiteral("Discord · %1")
        .arg(guildName.isEmpty() ? QStringLiteral("server") : guildName);
}

QString forkmesh::discord::directMessagesGroupName()
{
    return QStringLiteral("Discord · DMs");
}

bool forkmesh::discord::isDiscordGroupName(const QString &group)
{
    return group.startsWith(QStringLiteral("Discord · "));
}

QString forkmesh::discord::channelShortName(const QString &channelName,
                                            const QString &parentName)
{
    if (parentName.isEmpty())
        return QLatin1Char('#') + channelName;
    return QLatin1Char('#') + parentName + QStringLiteral(" / ") + channelName;
}

QString DiscordClient::groupName(const QString &conversation) const
{
    const Channel *channel = channelForConversation(conversation);
    if (!channel)
        return QString();
    if (channel->direct)
        return forkmesh::discord::directMessagesGroupName();
    return forkmesh::discord::guildGroupName(
        m_guilds.value(channel->guildId).name);
}

QString DiscordClient::shortName(const QString &conversation) const
{
    const Channel *channel = channelForConversation(conversation);
    if (!channel)
        return conversation;
    if (channel->direct) {
        return QStringLiteral("DM · %1")
            .arg(channel->recipientName.isEmpty()
                     ? QStringLiteral("direct message")
                     : channel->recipientName);
    }
    QString parentName;
    if (channel->type >= 10 && channel->type <= 12) {
        const auto parent = m_channels.constFind(channel->parentId);
        if (parent != m_channels.constEnd() && !parent->name.isEmpty())
            parentName = parent->name;
    }
    return forkmesh::discord::channelShortName(channel->name, parentName);
}

QString DiscordClient::displayName(const QString &conversation) const
{
    const Channel *channel = channelForConversation(conversation);
    if (!channel)
        return conversation;
    if (channel->direct) {
        return QStringLiteral("Discord DM · %1")
            .arg(channel->recipientName.isEmpty()
                     ? QStringLiteral("direct message")
                     : channel->recipientName);
    }
    return groupName(conversation) + QStringLiteral(" / ") +
           shortName(conversation);
}

QString DiscordClient::topic(const QString &conversation) const
{
    const Channel *channel = channelForConversation(conversation);
    return channel ? channel->topic : QString();
}

QStringList DiscordClient::members(const QString &conversation) const
{
    const Channel *channel = channelForConversation(conversation);
    if (!channel)
        return {};
    QStringList names = channel->memberNames.values();
    names.removeDuplicates();
    names.sort(Qt::CaseInsensitive);
    return names;
}

QStringList DiscordClient::mentionNames(const QString &conversation) const
{
    const Channel *channel = channelForConversation(conversation);
    if (!channel)
        return {};
    QStringList names;
    for (auto it = channel->memberNames.constBegin();
         it != channel->memberNames.constEnd(); ++it) {
        if (it.key() != m_botId && !names.contains(it.value(), Qt::CaseInsensitive))
            names.append(it.value());
    }
    names.sort(Qt::CaseInsensitive);
    return names;
}

QUrl DiscordClient::browserUrl(const QString &conversation) const
{
    const Channel *channel = channelForConversation(conversation);
    if (!channel)
        return {};
    return QUrl(QStringLiteral("https://discord.com/channels/%1/%2")
                    .arg(channel->direct ? QStringLiteral("@me")
                                         : channel->guildId,
                         channel->id));
}

bool DiscordClient::canSend(const QString &conversation) const
{
    return m_active && !m_token.isEmpty() &&
           channelForConversation(conversation) != nullptr;
}

void DiscordClient::openConversation(const QString &conversation, bool force)
{
    Channel *channel = channelForConversation(conversation);
    if (!channel || channel->historyLoading ||
        (channel->historyLoaded && !force)) {
        return;
    }
    channel->historyLoading = true;
    const QString channelId = channel->id;
    apiRequest(QByteArrayLiteral("GET"),
               QStringLiteral("/channels/%1/messages?limit=100").arg(channelId),
               {}, {},
               [this, channelId, conversation, force](int status,
                                                      const QByteArray &body,
                                                      const QString &error) {
                   auto it = m_channels.find(channelId);
                   if (it == m_channels.end())
                       return;
                   it->historyLoading = false;
                   const QJsonDocument document = QJsonDocument::fromJson(body);
                   if (status < 200 || status >= 300 || !document.isArray()) {
                       it->historyLoaded = false;
                       handleApiFailure(QStringLiteral("load message history"),
                                        status,
                                        boundedDiscordError(status, body, error),
                                        conversation);
                       return;
                   }
                   it->historyLoaded = true;
                   const QJsonArray messages = document.array();
                   for (qsizetype index = messages.size(); index > 0; --index)
                       deliverPayload(messages.at(index - 1).toObject(),
                                      conversation, force, true);
               });
}

QString DiscordClient::senderName(const QJsonObject &payload) const
{
    return discordDisplayName(payload);
}

void DiscordClient::rememberMember(Channel &channel,
                                   const QJsonObject &payload)
{
    QJsonObject author = payload.value(QStringLiteral("author")).toObject();
    if (author.isEmpty())
        author = payload.value(QStringLiteral("member"))
                     .toObject()
                     .value(QStringLiteral("user"))
                     .toObject();
    const QString userId = author.value(QStringLiteral("id")).toString();
    const QString name = senderName(payload);
    if (isSnowflake(userId) && !name.isEmpty() &&
        channel.memberNames.value(userId) != name) {
        channel.memberNames.insert(userId, name);
        emit membersChanged(conversationForChannelId(channel.id));
    }
}

void DiscordClient::ensureDirectChannel(const QJsonObject &messagePayload)
{
    if (!messagePayload.value(QStringLiteral("guild_id")).toString().isEmpty())
        return;
    const QString channelId =
        messagePayload.value(QStringLiteral("channel_id")).toString();
    if (!isSnowflake(channelId))
        return;
    const auto existing = m_channels.constFind(channelId);
    if (existing != m_channels.constEnd() && !existing->direct &&
        !existing->guildId.isEmpty()) {
        return;
    }
    Channel channel = m_channels.value(channelId);
    channel.id = channelId;
    channel.guildId.clear();
    channel.type = 1;
    channel.direct = true;
    channel.name = QStringLiteral("direct-message");
    const QJsonObject author =
        messagePayload.value(QStringLiteral("author")).toObject();
    const QString authorId = author.value(QStringLiteral("id")).toString();
    if (authorId != m_botId)
        channel.recipientName = senderName(messagePayload);
    m_channels.insert(channelId, channel);
    rebuildConversations();
}

void DiscordClient::deliverPayload(const QJsonObject &payload,
                                   const QString &knownConversation,
                                   bool allowExisting, bool historical,
                                   const QByteArray &knownAttachment)
{
    ensureDirectChannel(payload);
    const QString channelId =
        payload.value(QStringLiteral("channel_id")).toString();
    const QString conversation = knownConversation.isEmpty()
                                     ? conversationForChannelId(channelId)
                                     : knownConversation;
    Channel *channel = channelForConversation(conversation);
    if (!channel)
        return;
    const QString rawId = payload.value(QStringLiteral("id")).toString();
    const QString chatId = isSnowflake(rawId)
                               ? QStringLiteral("discord-message:") + rawId
                               : QString();
    if (chatId.isEmpty() ||
        (m_seenMessages.contains(chatId) && !allowExisting)) {
        return;
    }
    if (!m_seenMessages.contains(chatId)) {
        m_seenMessages.insert(chatId);
        m_seenMessageOrder.enqueue(chatId);
        while (m_seenMessageOrder.size() > kMaxSeenMessages)
            m_seenMessages.remove(m_seenMessageOrder.dequeue());
    }

    ChatMessage message;
    if (!forkmesh::discord::chatMessageFromPayload(
            payload, conversation, m_botId, &message)) {
        return;
    }
    message.historical = historical;
    rememberMember(*channel, payload);
    fetchAvatar(payload.value(QStringLiteral("author")).toObject());

    // Resolve channel mentions to the same readable labels used by the
    // sidebar. Unknown channels retain their Discord markup rather than being
    // guessed at.
    static const QRegularExpression channelMention(
        QStringLiteral("<#[0-9]{1,20}>") );
    QRegularExpressionMatchIterator matches =
        channelMention.globalMatch(message.text);
    QHash<QString, QString> replacements;
    while (matches.hasNext()) {
        const QString token = matches.next().captured();
        const QString id = token.mid(2, token.size() - 3);
        const auto candidate = m_channels.constFind(id);
        if (candidate != m_channels.constEnd())
            replacements.insert(token, QLatin1Char('#') + candidate->name);
    }
    for (auto it = replacements.constBegin(); it != replacements.constEnd(); ++it)
        message.text.replace(it.key(), it.value());

    const QJsonArray attachments =
        payload.value(QStringLiteral("attachments")).toArray();
    for (int i = 1; i < attachments.size(); ++i) {
        const QJsonObject attachment = attachments.at(i).toObject();
        const QString name =
            attachment.value(QStringLiteral("filename")).toString().left(180);
        const QString url = attachment.value(QStringLiteral("url")).toString();
        if (!url.isEmpty()) {
            if (!message.text.isEmpty())
                message.text += QLatin1Char('\n');
            message.text += QStringLiteral("%1: %2").arg(name, url);
        }
    }

    // Store the snapshot before a potentially asynchronous attachment fetch.
    // MainWindow will then already have it when the delayed row is created.
    emitReactionSnapshot(message.conversation, message.id,
                         payload.value(QStringLiteral("reactions")).toArray());

    if (attachments.isEmpty()) {
        deliverConverted(message);
        return;
    }
    const QJsonObject first = attachments.first().toObject();
    const QString url = first.value(QStringLiteral("url")).toString();
    const qint64 size = qint64(first.value(QStringLiteral("size")).toDouble());
    if (!knownAttachment.isEmpty() &&
        knownAttachment.size() <= kMaxUploadBytes) {
        message.fileData = knownAttachment;
        deliverConverted(message);
        return;
    }
    if (size <= 0 || size > kMaxDownloadBytes ||
        !isDiscordCdn(QUrl(url))) {
        message.fileName.clear();
        message.fileMime.clear();
        if (!url.isEmpty()) {
            if (!message.text.isEmpty())
                message.text += QLatin1Char('\n');
            message.text += url;
        }
        deliverConverted(message);
        return;
    }
    fetchAttachment(message, url);
}

void DiscordClient::deliverConverted(ChatMessage message)
{
    emit messageArrived(message);
}

void DiscordClient::fetchAttachment(ChatMessage message,
                                    const QString &fallbackUrl)
{
    const QUrl url(fallbackUrl);
    if (!m_network || !isDiscordCdn(url)) {
        message.fileName.clear();
        message.fileMime.clear();
        deliverConverted(message);
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    QNetworkReply *reply = m_network->get(request);
    const quint64 generation = m_connectionGeneration;
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64) {
                if (received > kMaxDownloadBytes)
                    reply->abort();
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, message = std::move(message), fallbackUrl,
             generation]() mutable {
                const QByteArray data = reply->readAll();
                const QUrl finalUrl = reply->url();
                const bool ok = reply->error() == QNetworkReply::NoError &&
                                data.size() <= kMaxDownloadBytes &&
                                isDiscordCdn(finalUrl);
                reply->deleteLater();
                if (generation != m_connectionGeneration || !m_active ||
                    !channelForConversation(message.conversation)) {
                    return;
                }
                if (ok) {
                    message.fileData = data;
                } else {
                    message.fileName.clear();
                    message.fileMime.clear();
                    if (!message.text.isEmpty())
                        message.text += QLatin1Char('\n');
                    message.text += fallbackUrl;
                }
                deliverConverted(message);
            });
}

void DiscordClient::fetchAvatar(const QJsonObject &author)
{
    const QString id = author.value(QStringLiteral("id")).toString();
    const QString hash = author.value(QStringLiteral("avatar")).toString();
    const QString actorId = QStringLiteral("discord-user:") + id;
    if (!isSnowflake(id) || hash.isEmpty() ||
        m_avatarHashes.value(actorId) == hash || !m_network) {
        return;
    }
    m_avatarHashes.insert(actorId, hash);
    const QString extension = hash.startsWith(QStringLiteral("a_"))
                                  ? QStringLiteral("gif")
                                  : QStringLiteral("png");
    const QUrl url(QStringLiteral(
                       "https://cdn.discordapp.com/avatars/%1/%2.%3?size=64")
                       .arg(id, hash, extension));
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(15000);
    QNetworkReply *reply = m_network->get(request);
    const quint64 generation = m_connectionGeneration;
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64) {
                if (received > 2 * 1024 * 1024)
                    reply->abort();
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, actorId, hash, generation]() {
                const QByteArray data = reply->readAll();
                const bool ok = reply->error() == QNetworkReply::NoError &&
                                data.size() <= 2 * 1024 * 1024 &&
                                isDiscordCdn(reply->url());
                reply->deleteLater();
                if (generation != m_connectionGeneration || !m_active) {
                    if (m_avatarHashes.value(actorId) == hash)
                        m_avatarHashes.remove(actorId);
                    return;
                }
                if (m_avatarHashes.value(actorId) != hash)
                    return;
                if (ok)
                    emit avatarChanged(actorId, data);
                else if (m_avatarHashes.value(actorId) == hash)
                    m_avatarHashes.remove(actorId);
            });
}

void DiscordClient::emitReactionSnapshot(
    const QString &conversation, const QString &messageId,
    const QJsonArray &reactions)
{
    QMap<QString, QStringList> snapshot;
    QSet<QString> self;
    for (const QJsonValue &value : reactions) {
        const QJsonObject reaction = value.toObject();
        const QString emoji =
            discordEmoji(reaction.value(QStringLiteral("emoji")).toObject());
        const int count = qBound(0, reaction.value(QStringLiteral("count")).toInt(),
                                 1000);
        if (emoji.isEmpty() || count <= 0)
            continue;
        QStringList reactors;
        const bool me = reaction.value(QStringLiteral("me")).toBool();
        if (me) {
            reactors.append(m_botName);
            self.insert(emoji);
        }
        for (int i = reactors.size(); i < count; ++i)
            reactors.append(QStringLiteral("Discord member %1").arg(i + 1));
        snapshot.insert(emoji, reactors);
    }
    m_selfReactions.insert(messageId, self);
    emit reactionsLoaded(conversation, messageId, snapshot);
}

QJsonObject DiscordClient::outgoingMessage(const QString &text,
                                           const QString &rootMessageId) const
{
    QJsonObject payload;
    payload.insert(QStringLiteral("content"), text);
    payload.insert(
        QStringLiteral("allowed_mentions"),
        QJsonObject{{QStringLiteral("parse"),
                     QJsonArray{QStringLiteral("users"),
                                QStringLiteral("roles")}},
                    {QStringLiteral("replied_user"), false}});
    const QString rawRoot =
        forkmesh::discord::messageIdFromChatId(rootMessageId);
    if (!rawRoot.isEmpty()) {
        payload.insert(
            QStringLiteral("message_reference"),
            QJsonObject{{QStringLiteral("message_id"), rawRoot},
                        {QStringLiteral("fail_if_not_exists"), false}});
    }
    return payload;
}

QString DiscordClient::encodeMentions(const Channel &channel,
                                      const QString &text) const
{
    struct Mention {
        QString id;
        QString name;
    };
    QHash<QString, Mention> unique;
    for (auto it = channel.memberNames.constBegin();
         it != channel.memberNames.constEnd(); ++it) {
        const QString name = it.value().trimmed();
        if (!isSnowflake(it.key()) || it.key() == m_botId || name.isEmpty())
            continue;
        const QString key = name.toCaseFolded();
        if (unique.contains(key) && unique.value(key).id != it.key()) {
            unique[key].id.clear();
        } else {
            unique.insert(key, Mention{it.key(), name});
        }
    }

    QList<Mention> mentions;
    for (const Mention &mention : std::as_const(unique)) {
        if (!mention.id.isEmpty())
            mentions.append(mention);
    }
    std::sort(mentions.begin(), mentions.end(), [](const Mention &left,
                                                   const Mention &right) {
        return left.name.size() > right.name.size();
    });

    QString encoded = text;
    for (const Mention &mention : std::as_const(mentions)) {
        const QRegularExpression pattern(
            QStringLiteral("(?<![\\p{L}\\p{N}_<])@%1(?=$|[\\s\\p{P}])")
                .arg(QRegularExpression::escape(mention.name)),
            QRegularExpression::CaseInsensitiveOption |
                QRegularExpression::UseUnicodePropertiesOption);
        QList<QPair<int, int>> spans;
        QRegularExpressionMatchIterator matches = pattern.globalMatch(encoded);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            spans.append({match.capturedStart(), match.capturedLength()});
        }
        for (auto it = spans.crbegin(); it != spans.crend(); ++it) {
            encoded.replace(it->first, it->second,
                            QStringLiteral("<@%1>").arg(mention.id));
        }
    }
    return encoded;
}

bool DiscordClient::sendMessage(const QString &conversation,
                                const QString &text)
{
    const QString message = text.trimmed();
    const Channel *channel = channelForConversation(conversation);
    if (!channel || message.isEmpty())
        return false;
    const QString encoded = encodeMentions(*channel, message);
    if (encoded.size() > kDiscordTextLimit) {
        emit sendFailed(
            conversation, text,
            QStringLiteral("Discord messages are limited to 2,000 characters"));
        return false;
    }
    const QString channelId = channel->id;
    const QByteArray body =
        QJsonDocument(outgoingMessage(encoded)).toJson(QJsonDocument::Compact);
    apiRequest(QByteArrayLiteral("POST"),
               QStringLiteral("/channels/%1/messages").arg(channelId), body,
               QByteArrayLiteral("application/json"),
               [this, conversation, text](int status, const QByteArray &response,
                                           const QString &error) {
                   const QJsonObject object =
                       QJsonDocument::fromJson(response).object();
                   if (status < 200 || status >= 300 || object.isEmpty()) {
                       handleApiFailure(
                           QStringLiteral("send message"), status,
                           boundedDiscordError(status, response, error),
                           conversation, text);
                       return;
                   }
                   deliverPayload(object, conversation);
               });
    return true;
}

bool DiscordClient::sendReply(const QString &conversation,
                              const QString &rootMessageId,
                              const QString &text)
{
    const QString message = text.trimmed();
    const Channel *channel = channelForConversation(conversation);
    const QString rawRoot =
        forkmesh::discord::messageIdFromChatId(rootMessageId);
    if (!channel || rawRoot.isEmpty() || message.isEmpty())
        return false;
    const QString encoded = encodeMentions(*channel, message);
    if (encoded.size() > kDiscordTextLimit) {
        emit sendFailed(
            conversation, text,
            QStringLiteral("Discord messages are limited to 2,000 characters"));
        return false;
    }
    const QByteArray body =
        QJsonDocument(outgoingMessage(encoded, rootMessageId))
            .toJson(QJsonDocument::Compact);
    apiRequest(QByteArrayLiteral("POST"),
               QStringLiteral("/channels/%1/messages").arg(channel->id), body,
               QByteArrayLiteral("application/json"),
               [this, conversation, text](int status, const QByteArray &response,
                                           const QString &error) {
                   const QJsonObject object =
                       QJsonDocument::fromJson(response).object();
                   if (status < 200 || status >= 300 || object.isEmpty()) {
                       handleApiFailure(
                           QStringLiteral("send reply"), status,
                           boundedDiscordError(status, response, error),
                           conversation, text);
                       return;
                   }
                   deliverPayload(object, conversation);
               });
    return true;
}

QByteArray DiscordClient::multipartBody(const QString &boundary,
                                        const QString &fileName,
                                        const QString &mimeType,
                                        const QByteArray &data,
                                        const QJsonObject &payload) const
{
    const QByteArray marker = QByteArrayLiteral("--") + boundary.toLatin1();
    QByteArray body;
    body.reserve(data.size() + 2048);
    body += marker + QByteArrayLiteral("\r\n");
    body += QByteArrayLiteral(
        "Content-Disposition: form-data; name=\"payload_json\"\r\n");
    body += QByteArrayLiteral("Content-Type: application/json\r\n\r\n");
    body += QJsonDocument(payload).toJson(QJsonDocument::Compact);
    body += QByteArrayLiteral("\r\n");
    body += marker + QByteArrayLiteral("\r\n");
    body += QByteArrayLiteral(
        "Content-Disposition: form-data; name=\"files[0]\"; filename=\"") +
            safeUploadFileName(fileName).toUtf8() + QByteArrayLiteral("\"\r\n");
    QByteArray safeMime = mimeType.trimmed().left(100).toLatin1();
    safeMime.replace('\r', '_');
    safeMime.replace('\n', '_');
    if (safeMime.isEmpty())
        safeMime = QByteArrayLiteral("application/octet-stream");
    body += QByteArrayLiteral("Content-Type: ") + safeMime +
            QByteArrayLiteral("\r\n\r\n");
    body += data;
    body += QByteArrayLiteral("\r\n") + marker + QByteArrayLiteral("--\r\n");
    return body;
}

bool DiscordClient::sendFile(const QString &conversation,
                             const QString &fileName,
                             const QString &mimeType, const QByteArray &data)
{
    const Channel *channel = channelForConversation(conversation);
    if (!channel || data.isEmpty())
        return false;
    if (data.size() > kMaxUploadBytes) {
        emit sendFailed(
            conversation, QString(),
            QStringLiteral("Discord's default upload limit is 10 MiB"));
        return false;
    }
    const QString safeName = safeUploadFileName(fileName);
    QJsonObject payload = outgoingMessage(QString());
    payload.remove(QStringLiteral("content"));
    payload.insert(
        QStringLiteral("attachments"),
        QJsonArray{QJsonObject{{QStringLiteral("id"), 0},
                               {QStringLiteral("filename"), safeName}}});
    const QString boundary =
        QStringLiteral("ForkMeshDiscord%1")
            .arg(QRandomGenerator::global()->generate64(), 0, 16);
    const QByteArray body =
        multipartBody(boundary, safeName, mimeType, data, payload);
    apiRequest(
        QByteArrayLiteral("POST"),
        QStringLiteral("/channels/%1/messages").arg(channel->id), body,
        QByteArrayLiteral("multipart/form-data; boundary=") + boundary.toLatin1(),
        [this, conversation, safeName, data](int status,
                                             const QByteArray &response,
                                             const QString &error) {
            const QJsonObject object = QJsonDocument::fromJson(response).object();
            if (status < 200 || status >= 300 || object.isEmpty()) {
                handleApiFailure(
                    QStringLiteral("upload %1").arg(safeName), status,
                    boundedDiscordError(status, response, error), conversation);
                return;
            }
            deliverPayload(object, conversation, false, false, data);
        });
    return true;
}

void DiscordClient::toggleReaction(const QString &conversation,
                                   const QString &messageId,
                                   const QString &emoji)
{
    const Channel *channel = channelForConversation(conversation);
    const QString rawId =
        forkmesh::discord::messageIdFromChatId(messageId);
    const QString cleanEmoji = emoji.trimmed().left(100);
    if (!channel || rawId.isEmpty() || cleanEmoji.isEmpty())
        return;
    // The path carries the emoji, the bookkeeping keeps the slug: both sides of
    // the toggle are then spelled the way the chat rows are.
    const QString path = forkmesh::discord::reactionEndpoint(channel->id, rawId,
                                                             cleanEmoji);
    if (path.isEmpty())
        return;
    const bool remove = m_selfReactions.value(messageId).contains(cleanEmoji);
    apiRequest(remove ? QByteArrayLiteral("DELETE") : QByteArrayLiteral("PUT"),
               path, {}, {},
               [this, conversation, messageId, cleanEmoji, remove](
                   int status, const QByteArray &response,
                   const QString &error) {
                   if (status < 200 || status >= 300) {
                       handleApiFailure(
                           remove ? QStringLiteral("remove reaction")
                                  : QStringLiteral("add reaction"),
                           status,
                           boundedDiscordError(status, response, error),
                           conversation);
                       return;
                   }
                   if (remove)
                       m_selfReactions[messageId].remove(cleanEmoji);
                   else
                       m_selfReactions[messageId].insert(cleanEmoji);
                   emit reactionChanged(conversation, messageId, cleanEmoji,
                                        m_botName, !remove);
               });
}

void DiscordClient::editMessage(const QString &conversation,
                                const QString &messageId,
                                const QString &text)
{
    const Channel *channel = channelForConversation(conversation);
    const QString rawId =
        forkmesh::discord::messageIdFromChatId(messageId);
    const QString message = text.trimmed();
    if (!channel || rawId.isEmpty() || message.isEmpty())
        return;
    const QString encoded = encodeMentions(*channel, message);
    if (encoded.size() > kDiscordTextLimit) {
        emit sendFailed(
            conversation, text,
            QStringLiteral("Discord messages are limited to 2,000 characters"));
        return;
    }
    const QByteArray body =
        QJsonDocument(outgoingMessage(encoded)).toJson(QJsonDocument::Compact);
    apiRequest(
        QByteArrayLiteral("PATCH"),
        QStringLiteral("/channels/%1/messages/%2").arg(channel->id, rawId), body,
        QByteArrayLiteral("application/json"),
        [this, conversation, messageId, message](
            int status, const QByteArray &response, const QString &error) {
            if (status < 200 || status >= 300) {
                handleApiFailure(
                    QStringLiteral("edit message"), status,
                    boundedDiscordError(status, response, error), conversation);
                return;
            }
            emit messageEdited(conversation, messageId, message);
        });
}

void DiscordClient::deleteMessage(const QString &conversation,
                                  const QString &messageId)
{
    const Channel *channel = channelForConversation(conversation);
    const QString rawId =
        forkmesh::discord::messageIdFromChatId(messageId);
    if (!channel || rawId.isEmpty())
        return;
    apiRequest(
        QByteArrayLiteral("DELETE"),
        QStringLiteral("/channels/%1/messages/%2").arg(channel->id, rawId), {},
        {},
        [this, conversation, messageId](int status, const QByteArray &response,
                                        const QString &error) {
            if (status < 200 || status >= 300) {
                handleApiFailure(
                    QStringLiteral("delete message"), status,
                    boundedDiscordError(status, response, error), conversation);
                return;
            }
            emit messageDeleted(conversation, messageId);
        });
}

void DiscordClient::sendTyping(const QString &conversation)
{
    const Channel *channel = channelForConversation(conversation);
    if (!channel)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastTypingSentMs.value(channel->id, 0) < 8000)
        return;
    m_lastTypingSentMs.insert(channel->id, now);
    apiRequest(QByteArrayLiteral("POST"),
               QStringLiteral("/channels/%1/typing").arg(channel->id), {}, {},
               [](int, const QByteArray &, const QString &) {});
}

void DiscordClient::handleApiFailure(const QString &operation, int status,
                                     const QString &error,
                                     const QString &conversation,
                                     const QString &originalText)
{
    QString reason = error.trimmed();
    if (reason.isEmpty())
        reason = QStringLiteral("HTTP %1").arg(status);
    emit activity(QStringLiteral("Discord: could not %1 (%2).")
                      .arg(operation, reason));
    if (!conversation.isEmpty() && !originalText.isNull())
        emit sendFailed(conversation, originalText, reason);
    if (status == 401) {
        m_active = false;
        ++m_connectionGeneration;
        m_gatewayReady = false;
        m_gatewayAttemptActive = false;
        setStatus(QStringLiteral("Discord · bot token rejected"), false, true);
        if (m_gatewayReconnectTimer)
            m_gatewayReconnectTimer->stop();
        if (m_gatewayHeartbeatTimer)
            m_gatewayHeartbeatTimer->stop();
        closeGatewaySocket(4004, QByteArrayLiteral("invalid token"));
    }
}

QString DiscordClient::boundedDiscordError(int status, const QByteArray &body,
                                           const QString &networkError)
{
    const QJsonObject object = QJsonDocument::fromJson(body).object();
    QString message =
        object.value(QStringLiteral("message")).toString().simplified();
    if (message.isEmpty())
        message = networkError.simplified();
    if (message.isEmpty() && status > 0)
        message = QStringLiteral("HTTP %1").arg(status);
    return message.left(240);
}

QNetworkReply *DiscordClient::startApiRequest(const QByteArray &method,
                                              const QString &path,
                                              const QByteArray &body,
                                              const QByteArray &contentType)
{
    QUrl url = m_apiBase;
    const int queryAt = path.indexOf(QLatin1Char('?'));
    const QString route = queryAt < 0 ? path : path.left(queryAt);
    const QString query = queryAt < 0 ? QString() : path.mid(queryAt + 1);
    QString basePath = url.path();
    if (basePath.endsWith(QLatin1Char('/')))
        basePath.chop(1);
    url.setPath(basePath + (route.startsWith(QLatin1Char('/'))
                                ? route
                                : QLatin1Char('/') + route),
                QUrl::TolerantMode);
    if (!query.isEmpty())
        url.setQuery(query, QUrl::TolerantMode);

    QNetworkRequest request(url);
    request.setRawHeader(QByteArrayLiteral("Authorization"),
                         QByteArrayLiteral("Bot ") + m_token.toUtf8());
    request.setRawHeader(
        QByteArrayLiteral("User-Agent"),
        QByteArrayLiteral("DiscordBot (https://forkmesh.com, ") +
            QByteArrayLiteral(FORKMESH_VERSION) + QByteArrayLiteral(")"));
    request.setRawHeader(QByteArrayLiteral("Accept"),
                         QByteArrayLiteral("application/json"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(30000);
    if (!contentType.isEmpty())
        request.setRawHeader(QByteArrayLiteral("Content-Type"), contentType);

    if (method == QByteArrayLiteral("GET"))
        return m_network->get(request);
    if (method == QByteArrayLiteral("POST"))
        return m_network->post(request, body);
    if (method == QByteArrayLiteral("PUT"))
        return m_network->put(request, body);
    if (method == QByteArrayLiteral("DELETE") && body.isEmpty())
        return m_network->deleteResource(request);
    return m_network->sendCustomRequest(request, method, body);
}

void DiscordClient::apiRequest(const QByteArray &method, const QString &path,
                               const QByteArray &body,
                               const QByteArray &contentType,
                               ApiCallback callback, int attempt)
{
    if (!m_active || !m_network || m_token.isEmpty()) {
        if (callback)
            callback(0, {}, QStringLiteral("not connected"));
        return;
    }
    const quint64 generation = m_connectionGeneration;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now < m_globalRateLimitUntilMs) {
        const int delay = int(qMin<qint64>(m_globalRateLimitUntilMs - now,
                                          5 * 60 * 1000));
        QTimer::singleShot(
            qMax(1, delay), this,
            [this, method, path, body, contentType,
             callback = std::move(callback), attempt, generation]() mutable {
                if (generation != m_connectionGeneration)
                    return;
                apiRequest(method, path, body, contentType,
                           std::move(callback), attempt);
            });
        return;
    }

    QNetworkReply *reply = startApiRequest(method, path, body, contentType);
    connect(reply, &QNetworkReply::downloadProgress, reply,
            [reply](qint64 received, qint64) {
                if (received > kMaxApiResponseBytes)
                    reply->abort();
            });
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, method, path, body, contentType,
             callback = std::move(callback), attempt, generation]() mutable {
                const int status =
                    reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                        .toInt();
                const QByteArray response = reply->readAll();
                const QNetworkReply::NetworkError replyError = reply->error();
                const QString networkError =
                    replyError == QNetworkReply::NoError
                        ? QString()
                        : reply->errorString();
                const QByteArray retryHeader =
                    reply->rawHeader(QByteArrayLiteral("Retry-After"));
                reply->deleteLater();
                if (generation != m_connectionGeneration)
                    return;

                if (status == 429 && attempt < 3 && m_active) {
                    const QJsonObject object =
                        QJsonDocument::fromJson(response).object();
                    double retrySeconds =
                        object.value(QStringLiteral("retry_after")).toDouble();
                    if (retrySeconds <= 0) {
                        bool ok = false;
                        retrySeconds = retryHeader.toDouble(&ok);
                        if (!ok)
                            retrySeconds = 1.0;
                    }
                    const int delay = qBound(
                        250, int(retrySeconds * 1000.0 + 100.0),
                        5 * 60 * 1000);
                    if (object.value(QStringLiteral("global")).toBool())
                        m_globalRateLimitUntilMs =
                            QDateTime::currentMSecsSinceEpoch() + delay;
                    QTimer::singleShot(
                        delay, this,
                        [this, method, path, body, contentType,
                         callback = std::move(callback), attempt,
                         generation]() mutable {
                            if (generation != m_connectionGeneration)
                                return;
                            apiRequest(method, path, body, contentType,
                                       std::move(callback), attempt + 1);
                        });
                    return;
                }

                const bool idempotent =
                    method == QByteArrayLiteral("GET") ||
                    method == QByteArrayLiteral("PUT") ||
                    method == QByteArrayLiteral("DELETE") ||
                    method == QByteArrayLiteral("PATCH");
                if ((status >= 500 || status == 0) && attempt == 0 && m_active &&
                    idempotent &&
                    replyError != QNetworkReply::ContentAccessDenied) {
                    QTimer::singleShot(
                        1000, this,
                        [this, method, path, body, contentType,
                         callback = std::move(callback), generation]() mutable {
                            if (generation != m_connectionGeneration)
                                return;
                            apiRequest(method, path, body, contentType,
                                       std::move(callback), 1);
                        });
                    return;
                }
                if (callback)
                    callback(status, response, networkError);
            });
}

void DiscordClient::openGateway()
{
    if (!m_active || m_token.isEmpty() || m_gatewayAttemptActive)
        return;
    discardGatewaySocket();
    m_gatewayReadBuffer.clear();
    m_gatewayMessageBuffer.clear();
    m_gatewayFragmentOpcode = 0;
    m_gatewayUpgraded = false;
    m_gatewayReady = false;

    QUrl url = !m_gatewaySessionId.isEmpty() && m_gatewayResumeUrl.isValid()
                   ? m_gatewayResumeUrl
                   : m_gatewayUrl;
    if (!url.hasQuery()) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("v"), QStringLiteral("10"));
        query.addQueryItem(QStringLiteral("encoding"), QStringLiteral("json"));
        url.setQuery(query);
    }
    if (!url.isValid() || !isDiscordGateway(url)) {
        m_gatewayFatalStatus =
            QStringLiteral("Discord · invalid Gateway endpoint");
        setStatus(m_gatewayFatalStatus, false, true);
        emit activity(QStringLiteral(
            "Discord: refused an untrusted Gateway endpoint."));
        return;
    }
    if (m_connectionAuthorizer && !m_connectionAuthorizer(url)) {
        emit activity(QStringLiteral(
            "Discord: the live Gateway connection was blocked by the "
            "desktop firewall policy."));
        m_gatewayFatalStatus =
            QStringLiteral("Discord · Gateway blocked by firewall");
        setStatus(m_gatewayFatalStatus, false, true);
        return;
    }
    m_gatewayFatalStatus.clear();
    m_gatewayCurrentUrl = url;

    if (!m_gatewayConnectTimer) {
        m_gatewayConnectTimer = new QTimer(this);
        m_gatewayConnectTimer->setSingleShot(true);
        connect(m_gatewayConnectTimer, &QTimer::timeout, this, [this] {
            if (m_gatewayAttemptActive && !m_gatewayReady)
                gatewayLost(QStringLiteral("connection timed out"));
        });
    }
    m_gatewayConnectTimer->start(kGatewayConnectTimeoutMs);

    m_gatewaySocket = url.scheme() == QLatin1String("wss")
                          ? static_cast<QTcpSocket *>(new QSslSocket(this))
                          : new QTcpSocket(this);
    m_gatewayAttemptActive = true;
    connectGatewaySignals();
    const int port = url.port(url.scheme() == QLatin1String("wss") ? 443 : 80);
    if (auto *ssl = qobject_cast<QSslSocket *>(m_gatewaySocket))
        ssl->connectToHostEncrypted(url.host(), port);
    else
        m_gatewaySocket->connectToHost(url.host(), port);
}

void DiscordClient::connectGatewaySignals()
{
    QTcpSocket *socket = m_gatewaySocket;
    connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
        if (socket == m_gatewaySocket)
            readGateway();
    });
    connect(socket, &QTcpSocket::connected, this, [this, socket] {
        if (socket == m_gatewaySocket && !qobject_cast<QSslSocket *>(socket))
            sendGatewayHandshake();
    });
    if (auto *ssl = qobject_cast<QSslSocket *>(socket)) {
        connect(ssl, &QSslSocket::encrypted, this, [this, socket] {
            if (socket == m_gatewaySocket)
                sendGatewayHandshake();
        });
    }
    connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
        if (socket == m_gatewaySocket)
            gatewayLost(QStringLiteral("connection closed"));
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, socket](QAbstractSocket::SocketError) {
                if (socket == m_gatewaySocket)
                    gatewayLost(socket->errorString());
            });
}

void DiscordClient::sendGatewayHandshake()
{
    if (!m_gatewaySocket)
        return;
    m_gatewayKey = randomWebSocketKey();
    QByteArray host = m_gatewayCurrentUrl.host().toUtf8();
    if (m_gatewayCurrentUrl.port() > 0)
        host += QByteArrayLiteral(":") +
                QByteArray::number(m_gatewayCurrentUrl.port());
    const QByteArray request =
        QByteArrayLiteral("GET ") +
        webSocketRequestTarget(m_gatewayCurrentUrl).toUtf8() +
        QByteArrayLiteral(" HTTP/1.1\r\nHost: ") + host +
        QByteArrayLiteral(
            "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: ") +
        m_gatewayKey + QByteArrayLiteral("\r\nUser-Agent: DiscordBot "
                                         "(https://forkmesh.com, ") +
        QByteArrayLiteral(FORKMESH_VERSION) + QByteArrayLiteral(")\r\n\r\n");
    m_gatewaySocket->write(request);
}

void DiscordClient::readGateway()
{
    if (!m_gatewaySocket)
        return;
    m_gatewayReadBuffer += m_gatewaySocket->readAll();
    if (!m_gatewayUpgraded) {
        const int headerEnd = m_gatewayReadBuffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            if (m_gatewayReadBuffer.size() > 32 * 1024)
                gatewayLost(QStringLiteral("invalid upgrade response"));
            return;
        }
        const QByteArray header = m_gatewayReadBuffer.left(headerEnd);
        m_gatewayReadBuffer.remove(0, headerEnd + 4);
        const QByteArray expected = QCryptographicHash::hash(
                                        m_gatewayKey +
                                            QByteArrayLiteral(
                                                "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"),
                                        QCryptographicHash::Sha1)
                                        .toBase64();
        const bool statusOk = header.startsWith("HTTP/1.1 101") ||
                              header.startsWith("HTTP/1.0 101");
        bool acceptOk = false;
        for (const QByteArray &line : header.split('\n')) {
            if (!line.trimmed().toLower().startsWith(
                    QByteArrayLiteral("sec-websocket-accept:"))) {
                continue;
            }
            acceptOk = line.mid(line.indexOf(':') + 1).trimmed() == expected;
            break;
        }
        if (!statusOk || !acceptOk) {
            const int lineEnd = header.indexOf("\r\n");
            const QString status = QString::fromLatin1(
                (lineEnd < 0 ? header : header.left(lineEnd)).left(120));
            gatewayLost(status.isEmpty() ? QStringLiteral("upgrade rejected")
                                         : status);
            return;
        }
        m_gatewayUpgraded = true;
    }
    consumeGatewayFrames();
}

void DiscordClient::consumeGatewayFrames()
{
    while (m_gatewayReadBuffer.size() >= 2) {
        const uchar first = uchar(m_gatewayReadBuffer.at(0));
        const uchar second = uchar(m_gatewayReadBuffer.at(1));
        const bool final = (first & 0x80) != 0;
        const int opcode = first & 0x0f;
        if ((first & 0x70) != 0) {
            gatewayLost(QStringLiteral("unsupported Gateway WebSocket extension"));
            return;
        }
        quint64 length = second & 0x7f;
        int offset = 2;
        if (length == 126) {
            if (m_gatewayReadBuffer.size() < 4)
                return;
            length = (quint64(uchar(m_gatewayReadBuffer.at(2))) << 8) |
                     quint64(uchar(m_gatewayReadBuffer.at(3)));
            offset = 4;
        } else if (length == 127) {
            if (m_gatewayReadBuffer.size() < 10)
                return;
            length = 0;
            for (int i = 0; i < 8; ++i)
                length = (length << 8) |
                         quint64(uchar(m_gatewayReadBuffer.at(2 + i)));
            offset = 10;
        }
        if (length > quint64(kMaxGatewayPayload) ||
            quint64(offset) + length > quint64(INT_MAX)) {
            gatewayLost(QStringLiteral("Gateway payload exceeded the safety limit"));
            return;
        }
        const bool masked = (second & 0x80) != 0;
        if (masked) {
            gatewayLost(QStringLiteral("masked Gateway server frame"));
            return;
        }
        if (opcode >= 0x8 && (!final || length > 125)) {
            gatewayLost(QStringLiteral("invalid Gateway control frame"));
            return;
        }
        if (m_gatewayReadBuffer.size() < offset + int(length))
            return;
        QByteArray payload = m_gatewayReadBuffer.mid(offset, int(length));
        m_gatewayReadBuffer.remove(0, offset + int(length));

        if (opcode == 0x8) {
            quint16 code = 1000;
            if (payload.size() >= 2) {
                code = (quint16(uchar(payload.at(0))) << 8) |
                       quint16(uchar(payload.at(1)));
            }
            if (m_gatewaySocket && m_gatewayUpgraded)
                sendGatewayFrame(0x8, payload.left(125));
            discardGatewaySocket();
            m_gatewayAttemptActive = false;
            m_gatewayReady = false;
            m_gatewayUpgraded = false;
            if (m_gatewayHeartbeatTimer)
                m_gatewayHeartbeatTimer->stop();

            if (code == 4004) {
                m_active = false;
                ++m_connectionGeneration;
                setStatus(QStringLiteral("Discord · bot token rejected"), false,
                          true);
                emit activity(QStringLiteral(
                    "Discord: the Gateway rejected the bot token."));
                return;
            }
            if (code == 4013 || code == 4014) {
                m_gatewayFatalStatus = QStringLiteral(
                    "Discord · enable Message Content Intent");
                setStatus(m_gatewayFatalStatus, false, true);
                emit activity(QStringLiteral(
                    "Discord: enable Message Content Intent on the bot's "
                    "Developer Portal page, then reconnect."));
                return;
            }
            const bool reset = code == 4007 || code == 4009;
            setStatus(QStringLiteral("Discord · reconnecting"), false, false);
            scheduleGatewayReconnect(reset);
            return;
        }
        if (opcode == 0x9) {
            sendGatewayFrame(0xA, payload.left(125));
            continue;
        }
        if (opcode == 0xA)
            continue;

        if (opcode == 0x1 || opcode == 0x2) {
            if (m_gatewayFragmentOpcode != 0) {
                gatewayLost(QStringLiteral("invalid fragmented Gateway frame"));
                return;
            }
            if (final) {
                if (opcode == 0x1)
                    processGatewayPayload(payload);
            } else {
                m_gatewayFragmentOpcode = opcode;
                m_gatewayMessageBuffer = payload;
            }
            continue;
        }
        if (opcode == 0x0) {
            if (m_gatewayFragmentOpcode == 0) {
                gatewayLost(QStringLiteral("unexpected Gateway continuation"));
                return;
            }
            if (m_gatewayMessageBuffer.size() + payload.size() >
                kMaxGatewayPayload) {
                gatewayLost(QStringLiteral("Gateway message exceeded the safety limit"));
                return;
            }
            m_gatewayMessageBuffer += payload;
            if (final) {
                const int originalOpcode = m_gatewayFragmentOpcode;
                m_gatewayFragmentOpcode = 0;
                if (originalOpcode == 0x1)
                    processGatewayPayload(m_gatewayMessageBuffer);
                m_gatewayMessageBuffer.clear();
            }
        }
    }
}

void DiscordClient::processGatewayPayload(const QByteArray &payload)
{
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject())
        return;
    const QJsonObject frame = document.object();
    const int opcode = frame.value(QStringLiteral("op")).toInt(-1);
    if (frame.value(QStringLiteral("s")).isDouble())
        m_gatewaySequence =
            qint64(frame.value(QStringLiteral("s")).toDouble());

    if (opcode == 0) {
        processGatewayDispatch(frame.value(QStringLiteral("t")).toString(),
                               frame.value(QStringLiteral("d")).toObject());
        return;
    }
    if (opcode == 1) {
        // Discord can request an immediate heartbeat independently of the
        // interval. It does not acknowledge an earlier heartbeat.
        sendGatewayHeartbeatNow(false);
        return;
    }
    if (opcode == 7) {
        closeGatewaySocket(4000, QByteArrayLiteral("server reconnect"));
        m_gatewayAttemptActive = false;
        m_gatewayReady = false;
        setStatus(QStringLiteral("Discord · reconnecting"), false, false);
        scheduleGatewayReconnect(false, 250);
        return;
    }
    if (opcode == 9) {
        const bool resumable = frame.value(QStringLiteral("d")).toBool();
        closeGatewaySocket(4000, QByteArrayLiteral("invalid session"));
        m_gatewayAttemptActive = false;
        m_gatewayReady = false;
        setStatus(QStringLiteral("Discord · reconnecting"), false, false);
        scheduleGatewayReconnect(!resumable,
                                 int(QRandomGenerator::global()->bounded(1000,
                                                                         5001)));
        return;
    }
    if (opcode == 10) {
        const int interval =
            frame.value(QStringLiteral("d"))
                .toObject()
                .value(QStringLiteral("heartbeat_interval"))
                .toInt();
        if (interval < 1000 || interval > 5 * 60 * 1000) {
            gatewayLost(QStringLiteral("invalid heartbeat interval"));
            return;
        }
        m_gatewayHeartbeatIntervalMs = interval;
        m_gatewayHeartbeatAcked = true;
        if (!m_gatewayHeartbeatTimer) {
            m_gatewayHeartbeatTimer = new QTimer(this);
            m_gatewayHeartbeatTimer->setSingleShot(true);
            connect(m_gatewayHeartbeatTimer, &QTimer::timeout, this,
                    &DiscordClient::sendGatewayHeartbeat);
        }
        const int initial = int(QRandomGenerator::global()->bounded(
            quint32(qMax(1, interval))));
        m_gatewayHeartbeatTimer->start(initial);
        if (m_gatewaySessionId.isEmpty())
            identifyGateway();
        else
            resumeGateway();
        return;
    }
    if (opcode == 11)
        m_gatewayHeartbeatAcked = true;
}

void DiscordClient::processGatewayDispatch(const QString &type,
                                           const QJsonObject &payload)
{
    if (type == QLatin1String("READY")) {
        m_gatewaySessionId =
            payload.value(QStringLiteral("session_id")).toString();
        QUrl resume(payload.value(QStringLiteral("resume_gateway_url")).toString());
        if (resume.isValid() && isDiscordGateway(resume)) {
            QUrlQuery query;
            query.addQueryItem(QStringLiteral("v"), QStringLiteral("10"));
            query.addQueryItem(QStringLiteral("encoding"), QStringLiteral("json"));
            resume.setQuery(query);
            m_gatewayResumeUrl = resume;
        }
        m_gatewayReady = true;
        m_gatewayReconnectAttempts = 0;
        m_gatewayFatalStatus.clear();
        if (m_gatewayConnectTimer)
            m_gatewayConnectTimer->stop();
        setStatus(QStringLiteral("Discord · %1 · live").arg(m_botName), true,
                  false);
        emit activity(QStringLiteral("Discord: connected as %1.").arg(m_botName));
        return;
    }
    if (type == QLatin1String("RESUMED")) {
        m_gatewayReady = true;
        m_gatewayReconnectAttempts = 0;
        m_gatewayFatalStatus.clear();
        if (m_gatewayConnectTimer)
            m_gatewayConnectTimer->stop();
        setStatus(QStringLiteral("Discord · %1 · live").arg(m_botName), true,
                  false);
        return;
    }
    if (type == QLatin1String("GUILD_CREATE")) {
        Guild guild = m_guilds.value(
            payload.value(QStringLiteral("id")).toString());
        guild.id = payload.value(QStringLiteral("id")).toString();
        guild.name =
            payload.value(QStringLiteral("name")).toString().trimmed().left(100);
        guild.icon = payload.value(QStringLiteral("icon")).toString();
        if (isSnowflake(guild.id) && !guild.name.isEmpty()) {
            if (!m_guilds.contains(guild.id))
                guild.order = m_guilds.size();
            m_guilds.insert(guild.id, guild);
            applyChannels(guild.id,
                          payload.value(QStringLiteral("channels")).toArray(),
                          false);
            applyChannels(guild.id,
                          payload.value(QStringLiteral("threads")).toArray(), true);
            rebuildConversations();
        }
        return;
    }
    if (type == QLatin1String("GUILD_UPDATE")) {
        const QString id = payload.value(QStringLiteral("id")).toString();
        if (m_guilds.contains(id)) {
            m_guilds[id].name =
                payload.value(QStringLiteral("name")).toString().trimmed().left(100);
            m_guilds[id].icon =
                payload.value(QStringLiteral("icon")).toString();
            rebuildConversations();
            for (const Channel &channel : std::as_const(m_channels)) {
                if (channel.guildId == id)
                    emit conversationMetadataChanged(
                        conversationForChannelId(channel.id));
            }
        }
        return;
    }
    if (type == QLatin1String("GUILD_DELETE")) {
        if (payload.value(QStringLiteral("unavailable")).toBool())
            return;
        const QString id = payload.value(QStringLiteral("id")).toString();
        m_guilds.remove(id);
        for (auto it = m_channels.begin(); it != m_channels.end();) {
            if (it->guildId == id)
                it = m_channels.erase(it);
            else
                ++it;
        }
        rebuildConversations();
        return;
    }
    if (type == QLatin1String("CHANNEL_CREATE") ||
        type == QLatin1String("CHANNEL_UPDATE") ||
        type == QLatin1String("THREAD_CREATE") ||
        type == QLatin1String("THREAD_UPDATE")) {
        applyChannel(payload);
        rebuildConversations();
        return;
    }
    if (type == QLatin1String("CHANNEL_DELETE") ||
        type == QLatin1String("THREAD_DELETE")) {
        removeChannel(payload.value(QStringLiteral("id")).toString());
        return;
    }
    if (type == QLatin1String("MESSAGE_CREATE")) {
        deliverPayload(payload);
        return;
    }
    if (type == QLatin1String("MESSAGE_UPDATE")) {
        const QString conversation = conversationForChannelId(
            payload.value(QStringLiteral("channel_id")).toString());
        const QString id = forkmesh::discord::messageIdFromChatId(
            payload.value(QStringLiteral("id")).toString());
        if (!conversation.isEmpty() && !id.isEmpty() &&
            payload.value(QStringLiteral("content")).isString()) {
            QString text =
                payload.value(QStringLiteral("content")).toString().left(16000);
            ChatMessage converted;
            if (forkmesh::discord::chatMessageFromPayload(
                    payload, conversation, m_botId, &converted)) {
                text = converted.text;
            }
            emit messageEdited(
                conversation, QStringLiteral("discord-message:") + id,
                text);
        }
        return;
    }
    if (type == QLatin1String("MESSAGE_DELETE")) {
        const QString conversation = conversationForChannelId(
            payload.value(QStringLiteral("channel_id")).toString());
        const QString id = forkmesh::discord::messageIdFromChatId(
            payload.value(QStringLiteral("id")).toString());
        if (!conversation.isEmpty() && !id.isEmpty())
            emit messageDeleted(conversation,
                                QStringLiteral("discord-message:") + id);
        return;
    }
    if (type == QLatin1String("MESSAGE_DELETE_BULK")) {
        const QString conversation = conversationForChannelId(
            payload.value(QStringLiteral("channel_id")).toString());
        for (const QJsonValue &value :
             payload.value(QStringLiteral("ids")).toArray()) {
            const QString id =
                forkmesh::discord::messageIdFromChatId(value.toString());
            if (!conversation.isEmpty() && !id.isEmpty())
                emit messageDeleted(conversation,
                                    QStringLiteral("discord-message:") + id);
        }
        return;
    }
    if (type == QLatin1String("MESSAGE_REACTION_ADD") ||
        type == QLatin1String("MESSAGE_REACTION_REMOVE")) {
        const bool added = type == QLatin1String("MESSAGE_REACTION_ADD");
        const QString conversation = conversationForChannelId(
            payload.value(QStringLiteral("channel_id")).toString());
        const QString rawMessage = forkmesh::discord::messageIdFromChatId(
            payload.value(QStringLiteral("message_id")).toString());
        const QString userId =
            payload.value(QStringLiteral("user_id")).toString();
        const QString emoji =
            discordEmoji(payload.value(QStringLiteral("emoji")).toObject());
        if (conversation.isEmpty() || rawMessage.isEmpty() || emoji.isEmpty())
            return;
        const QString chatId = QStringLiteral("discord-message:") + rawMessage;
        if (userId == m_botId) {
            if (added)
                m_selfReactions[chatId].insert(emoji);
            else
                m_selfReactions[chatId].remove(emoji);
        }
        QString name = discordDisplayName(payload);
        if (name == QLatin1String("Discord user") && isSnowflake(userId))
            name = userId == m_botId
                       ? m_botName
                       : QStringLiteral("Discord member %1").arg(userId.right(4));
        emit reactionChanged(conversation, chatId, emoji, name, added);
        return;
    }
    if (type == QLatin1String("MESSAGE_REACTION_REMOVE_ALL")) {
        const QString conversation = conversationForChannelId(
            payload.value(QStringLiteral("channel_id")).toString());
        const QString rawMessage = forkmesh::discord::messageIdFromChatId(
            payload.value(QStringLiteral("message_id")).toString());
        if (!conversation.isEmpty() && !rawMessage.isEmpty()) {
            const QString chatId = QStringLiteral("discord-message:") + rawMessage;
            m_selfReactions.remove(chatId);
            emit reactionsLoaded(conversation, chatId, {});
        }
        return;
    }
    if (type == QLatin1String("MESSAGE_REACTION_REMOVE_EMOJI")) {
        const QString channelId =
            payload.value(QStringLiteral("channel_id")).toString();
        const QString conversation = conversationForChannelId(channelId);
        const QString rawMessage = forkmesh::discord::messageIdFromChatId(
            payload.value(QStringLiteral("message_id")).toString());
        if (conversation.isEmpty() || rawMessage.isEmpty())
            return;
        apiRequest(
            QByteArrayLiteral("GET"),
            QStringLiteral("/channels/%1/messages/%2")
                .arg(channelId, rawMessage),
            {}, {},
            [this, conversation](int status, const QByteArray &body,
                                 const QString &) {
                if (status < 200 || status >= 300)
                    return;
                const QJsonObject message = QJsonDocument::fromJson(body).object();
                const QString id = forkmesh::discord::messageIdFromChatId(
                    message.value(QStringLiteral("id")).toString());
                if (id.isEmpty())
                    return;
                emitReactionSnapshot(
                    conversation, QStringLiteral("discord-message:") + id,
                    message.value(QStringLiteral("reactions")).toArray());
            });
        return;
    }
    if (type == QLatin1String("TYPING_START")) {
        const QString channelId =
            payload.value(QStringLiteral("channel_id")).toString();
        const QString conversation = conversationForChannelId(channelId);
        const QString userId =
            payload.value(QStringLiteral("user_id")).toString();
        if (conversation.isEmpty() || !isSnowflake(userId) || userId == m_botId)
            return;
        Channel *channel = channelForConversation(conversation);
        if (channel)
            rememberMember(*channel, payload);
        QString name = discordDisplayName(payload);
        if (name == QLatin1String("Discord user") && channel &&
            !channel->recipientName.isEmpty()) {
            name = channel->recipientName;
            if (channel->memberNames.value(userId) != name) {
                channel->memberNames.insert(userId, name);
                emit membersChanged(conversation);
            }
        }
        const QString peerId = QStringLiteral("discord-user:") + userId;
        const QString generationKey = channelId + QLatin1Char(':') + userId;
        const qint64 generation = QDateTime::currentMSecsSinceEpoch();
        m_typingGenerations.insert(generationKey, generation);
        emit typingChanged(conversation, peerId, name, true);
        QTimer::singleShot(11000, this,
                           [this, generationKey, generation, conversation,
                            peerId, name] {
                               if (m_typingGenerations.value(generationKey) !=
                                   generation)
                                   return;
                               m_typingGenerations.remove(generationKey);
                               emit typingChanged(conversation, peerId, name,
                                                  false);
                           });
    }
}

void DiscordClient::identifyGateway()
{
    QJsonObject properties;
    properties.insert(QStringLiteral("os"), QSysInfo::productType());
    properties.insert(QStringLiteral("browser"), QStringLiteral("forkmesh"));
    properties.insert(QStringLiteral("device"), QStringLiteral("forkmesh"));
    QJsonObject data;
    data.insert(QStringLiteral("token"), m_token);
    data.insert(QStringLiteral("intents"), forkmesh::discord::kGatewayIntents);
    data.insert(QStringLiteral("properties"), properties);
    sendGatewayJson(QJsonObject{{QStringLiteral("op"), 2},
                                {QStringLiteral("d"), data}});
}

void DiscordClient::resumeGateway()
{
    QJsonObject data;
    data.insert(QStringLiteral("token"), m_token);
    data.insert(QStringLiteral("session_id"), m_gatewaySessionId);
    data.insert(QStringLiteral("seq"), double(m_gatewaySequence));
    sendGatewayJson(QJsonObject{{QStringLiteral("op"), 6},
                                {QStringLiteral("d"), data}});
}

void DiscordClient::sendGatewayHeartbeat()
{
    sendGatewayHeartbeatNow(true);
}

void DiscordClient::sendGatewayHeartbeatNow(bool requirePreviousAck)
{
    if (!m_active || !m_gatewayUpgraded)
        return;
    if (requirePreviousAck && !m_gatewayHeartbeatAcked) {
        gatewayLost(QStringLiteral("heartbeat was not acknowledged"));
        return;
    }
    const QJsonValue sequence = m_gatewaySequence < 0
                                    ? QJsonValue(QJsonValue::Null)
                                    : QJsonValue(double(m_gatewaySequence));
    sendGatewayJson(QJsonObject{{QStringLiteral("op"), 1},
                                {QStringLiteral("d"), sequence}});
    m_gatewayHeartbeatAcked = false;
    if (m_gatewayHeartbeatTimer && m_gatewayHeartbeatIntervalMs > 0)
        m_gatewayHeartbeatTimer->start(m_gatewayHeartbeatIntervalMs);
}

void DiscordClient::sendGatewayJson(const QJsonObject &payload)
{
    const QByteArray data = QJsonDocument(payload).toJson(QJsonDocument::Compact);
    if (data.size() <= 4096)
        sendGatewayFrame(0x1, data);
}

void DiscordClient::sendGatewayFrame(int opcode, const QByteArray &payload)
{
    if (!m_gatewaySocket || !m_gatewayUpgraded || payload.size() > 65535)
        return;
    QByteArray frame;
    frame.reserve(payload.size() + 16);
    frame.append(char(0x80 | (opcode & 0x0f)));
    if (payload.size() < 126) {
        frame.append(char(0x80 | payload.size()));
    } else {
        frame.append(char(0x80 | 126));
        frame.append(char((payload.size() >> 8) & 0xff));
        frame.append(char(payload.size() & 0xff));
    }
    QByteArray mask(4, Qt::Uninitialized);
    for (int i = 0; i < mask.size(); ++i)
        mask[i] = char(QRandomGenerator::global()->bounded(256));
    frame += mask;
    QByteArray masked = payload;
    for (int i = 0; i < masked.size(); ++i)
        masked[i] = masked.at(i) ^ mask.at(i % 4);
    frame += masked;
    m_gatewaySocket->write(frame);
}

void DiscordClient::discardGatewaySocket()
{
    QTcpSocket *socket = m_gatewaySocket;
    m_gatewaySocket = nullptr;
    if (!socket)
        return;
    socket->disconnect(this);
    socket->abort();
    socket->deleteLater();
}

void DiscordClient::closeGatewaySocket(quint16 code, const QByteArray &reason)
{
    QTcpSocket *socket = m_gatewaySocket;
    if (!socket)
        return;
    if (m_gatewayUpgraded && socket->state() == QAbstractSocket::ConnectedState) {
        QByteArray payload;
        payload.append(char((code >> 8) & 0xff));
        payload.append(char(code & 0xff));
        payload += reason.left(123);
        sendGatewayFrame(0x8, payload);
        socket->flush();
    }
    m_gatewaySocket = nullptr;
    socket->disconnect(this);
    if (socket->state() == QAbstractSocket::ConnectedState)
        socket->disconnectFromHost();
    else
        socket->abort();
    socket->deleteLater();
    m_gatewayUpgraded = false;
}

void DiscordClient::gatewayLost(const QString &reason)
{
    if (!m_gatewayAttemptActive && !m_gatewayUpgraded)
        return;
    const bool wasReady = m_gatewayReady;
    m_gatewayAttemptActive = false;
    m_gatewayReady = false;
    m_gatewayUpgraded = false;
    if (m_gatewayConnectTimer)
        m_gatewayConnectTimer->stop();
    if (m_gatewayHeartbeatTimer)
        m_gatewayHeartbeatTimer->stop();
    discardGatewaySocket();
    if (wasReady && !reason.isEmpty())
        emit activity(QStringLiteral("Discord: live connection lost (%1).")
                          .arg(reason.left(160)));
    if (m_active) {
        m_gatewayFatalStatus.clear();
        setStatus(QStringLiteral("Discord · reconnecting"), false, false);
        scheduleGatewayReconnect();
    }
}

void DiscordClient::scheduleGatewayReconnect(bool resetSession,
                                             int minimumDelayMs)
{
    if (resetSession) {
        m_gatewaySessionId.clear();
        m_gatewayResumeUrl.clear();
        m_gatewaySequence = -1;
    }
    if (!m_active)
        return;
    if (!m_gatewayReconnectTimer) {
        m_gatewayReconnectTimer = new QTimer(this);
        m_gatewayReconnectTimer->setSingleShot(true);
        connect(m_gatewayReconnectTimer, &QTimer::timeout, this,
                &DiscordClient::openGateway);
    }
    if (m_gatewayReconnectTimer->isActive())
        return;
    const int shift = qMin(m_gatewayReconnectAttempts++, 8);
    const int exponential = qMin(kGatewayReconnectBaseMs << shift,
                                 kGatewayReconnectMaxMs);
    const int jitter = int(QRandomGenerator::global()->bounded(
        quint32(exponential / 4 + 1)));
    m_gatewayReconnectTimer->start(
        qMax(minimumDelayMs, exponential + jitter));
}
