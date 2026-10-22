#pragma once

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>
#include <QString>

namespace forkmesh::relayrules {

inline bool isRelayDirectConversation(const QString &conversation)
{
    return conversation.startsWith(QLatin1Char('@'));
}

inline bool isDeviceLocalConversation(const QString &conversation)
{
    return conversation == QLatin1String("#team");
}

inline QSet<QString> relayPrivateChannels(const QSet<QString> &saved)
{
    QSet<QString> channels;
    for (const QString &name : saved) {
        const QString channel =
            name.startsWith(QLatin1Char('#')) ? name : QLatin1Char('#') + name;
        if (!isDeviceLocalConversation(channel) &&
            !channel.startsWith(QLatin1String("#office/")))
            channels.insert(channel);
    }
    return channels;
}

inline bool purgedConversation(const QString &conversation,
                               const QSet<QString> &relayPrivate)
{
    return isRelayDirectConversation(conversation) || relayPrivate.contains(conversation);
}

inline QJsonObject purgedChatHistory(const QJsonObject &root,
                                     const QSet<QString> &relayPrivate,
                                     int *removed = nullptr)
{
    QJsonObject out = root;
    int dropped = 0;
    QJsonObject conversations;
    const QJsonObject saved = root.value(QStringLiteral("conversations")).toObject();
    for (auto it = saved.constBegin(); it != saved.constEnd(); ++it) {
        if (purgedConversation(it.key(), relayPrivate)) {
            ++dropped;
            continue;
        }
        conversations.insert(it.key(), it.value());
    }
    out.insert(QStringLiteral("conversations"), conversations);
    QJsonObject unread;
    const QJsonObject savedUnread = root.value(QStringLiteral("unread")).toObject();
    for (auto it = savedUnread.constBegin(); it != savedUnread.constEnd(); ++it)
        if (!purgedConversation(it.key(), relayPrivate))
            unread.insert(it.key(), it.value());
    out.insert(QStringLiteral("unread"), unread);
    if (!root.value(QStringLiteral("openDms")).toArray().isEmpty() ||
        !root.value(QStringLiteral("dmNames")).toObject().isEmpty())
        ++dropped;
    out.insert(QStringLiteral("openDms"), QJsonArray());
    out.insert(QStringLiteral("dmNames"), QJsonObject());
    if (purgedConversation(root.value(QStringLiteral("current")).toString(), relayPrivate))
        out.remove(QStringLiteral("current"));
    if (removed)
        *removed = dropped;
    return out;
}

inline QString senderAccount(const QString &senderId, const QString &conversation,
                             const QString &verifiedAccount)
{
    if (conversation.startsWith(QLatin1String("#office/@")))
        return conversation.mid(9).trimmed().toLower();
    if (senderId.startsWith(QLatin1String("user:")))
        return senderId.mid(5).trimmed().toLower();
    return verifiedAccount.trimmed().toLower();
}

inline QString profileAccount(const QString &memberId, bool self, const QString &ownAccount,
                              const QString &directoryAccount,
                              const QString &verifiedAccount)
{
    if (self)
        return ownAccount.trimmed().toLower();
    if (memberId.startsWith(QLatin1String("user:")))
        return memberId.mid(5).trimmed().toLower();
    if (!directoryAccount.trimmed().isEmpty())
        return directoryAccount.trimmed().toLower();
    return verifiedAccount.trimmed().toLower();
}

inline QString guestLabel(const QString &node)
{
    const QByteArray digest =
        QCryptographicHash::hash(node.toUtf8(), QCryptographicHash::Sha256);
    quint32 value = 0;
    for (int i = 0; i < 4; ++i)
        value = (value << 8) | quint8(digest.at(i));
    return QStringLiteral("Guest %1").arg(1000 + value % 9000);
}

inline bool isNamedBot(const QString &bot)
{
    const QString name = bot.trimmed().toLower();
    return name == QLatin1String("forkbot") || name == QLatin1String("claude") ||
           name == QLatin1String("codex") || name == QLatin1String("agent");
}

inline QString botViaName(const QString &bot, const QString &verifiedAccount,
                          const QString &posterNode)
{
    const QString poster = verifiedAccount.trimmed().toLower();
    return bot.trimmed().toLower() + QStringLiteral(" · via ") +
           (poster.isEmpty() ? guestLabel(posterNode) : poster);
}

inline bool webhooksAllowed(const QString &visibility)
{
    return visibility == QLatin1String("public") || visibility == QLatin1String("private");
}

inline QString webhookNote(const QString &visibility)
{
    if (!webhooksAllowed(visibility))
        return QStringLiteral("Incoming webhooks are not available in this channel.");
    return QStringLiteral(
        "POST JSON with a text, content, or message field. A URL is shown only once. "
        "Webhook messages are posted by the server, so the server can read them; "
        "they show as the integration, marked server-visible when the server "
        "says so.");
}

enum class RoomKind { Discord, ServerVisible, Mesh, DeviceLocal, Public, Private };

struct RoomBadge {
    QString text;
    QString icon;
    QString tooltip;
};

inline RoomBadge roomBadge(RoomKind kind)
{
    switch (kind) {
    case RoomKind::Discord:
        return {QStringLiteral("Discord"), QStringLiteral("server"),
                QStringLiteral("Discord bot connection over TLS; Discord and server "
                               "moderators can read this channel")};
    case RoomKind::ServerVisible:
        return {QStringLiteral("Server-visible"), QStringLiteral("server"),
                QStringLiteral("ForkMesh writes these notices itself, so the server can "
                               "read them. Only you can see this channel")};
    case RoomKind::Mesh:
        return {QStringLiteral("Nearby"), QStringLiteral("broadcast"),
                QStringLiteral("Anyone nearby running ForkMesh can join #mesh and read "
                               "what is said here. It never goes to the internet")};
    case RoomKind::DeviceLocal:
        return {QStringLiteral("This device"), QStringLiteral("device-desktop"),
                QStringLiteral("Stays on this computer; nothing is sent to the relay")};
    case RoomKind::Public:
        return {QStringLiteral("Public"), QStringLiteral("unlock"),
                QStringLiteral("Anyone who joins this room can read it. Messages are "
                               "sealed to the room's devices, so the relay cannot read "
                               "them; integrations can only post")};
    case RoomKind::Private:
        break;
    }
    return {QStringLiteral("End-to-end"), QStringLiteral("shield-check"),
            QStringLiteral("Only the people the server admits to this conversation can "
                           "read it; each message is sealed to their devices")};
}

}
