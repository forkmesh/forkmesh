#include "OfficeChannelMirror.h"
#include "ChatBlocks.h"
#include "ChatRecipients.h"
#include "ServerNode.h"
#include "SettingsWrite.h"
#include "StickerCatalog.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSettings>
#include <QThread>
#include <QTimer>
#include <QUrlQuery>
#include <QUuid>

#include <utility>

namespace {

constexpr int kMaxLiveRooms = 16;
constexpr int kReconnectBaseDelayMs = 2000;
constexpr int kReconnectMaxDelayMs = 5 * 60 * 1000;
constexpr int kMinRefreshIntervalMs = 10000;
constexpr int kMaxChannels = 50;
constexpr int kMaxTextChars = 16000;
constexpr int kMaxTopicChars = 200;
constexpr int kMaxChannelNameChars = 40;
constexpr int kMaxWebhookNameChars = 80;
constexpr int kMaxWebhooksPerChannel = 10;
constexpr int kMaxDisplayNameChars = 32;
constexpr int kMaxFileNameChars = 180;
constexpr int kMaxMimeChars = 100;
constexpr qsizetype kMaxFileBytes = 8ll * 1024 * 1024;
constexpr int kMaxPendingTextsPerRoom = 32;
constexpr int kSeenIdsPerRoom = 1000;
constexpr int kMaxDirectConversations = 50;
constexpr int kMaxLiveDirectRooms = 8;
constexpr int kMaxDirectRoomNameChars = 160;
constexpr int kMaxInvitees = 500;
constexpr int kMaxEveryoneMembers = 20000;

const QRegularExpression &usernamePattern()
{
    static const QRegularExpression pattern(
        QStringLiteral("\\A[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?\\z"));
    return pattern;
}

QString normalizedUsername(const QString &value)
{
    QString name = value.trimmed().toLower();
    if (name.startsWith(QLatin1Char('@')))
        name.remove(0, 1);
    return usernamePattern().match(name).hasMatch() ? name : QString();
}

QString sha256Hex(const QString &value)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(value.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

const QRegularExpression &channelIdPattern()
{
    static const QRegularExpression pattern(QStringLiteral("\\A[0-9a-f]{32}\\z"));
    return pattern;
}

QString boundedText(const QJsonObject &object, const char *key, int maxChars)
{
    return object.value(QLatin1String(key)).toString().left(maxChars);
}

} // namespace

namespace forkmesh::office {

QString conversationForChannel(const QString &channelName)
{
    const QString name = channelName.trimmed().left(kMaxDisplayNameChars);
    if (name.isEmpty())
        return QString();
    return QStringLiteral("#office/") + name;
}

bool isOfficeConversation(const QString &conversation)
{
    return conversation.startsWith(QStringLiteral("#office/"));
}

QString conversationForDirect(const QString &username)
{
    const QString name = normalizedUsername(username);
    if (name.isEmpty())
        return QString();
    return QStringLiteral("#office/@") + name;
}

bool isDirectOfficeConversation(const QString &conversation)
{
    return conversation.startsWith(QStringLiteral("#office/@"));
}

QString directPeerForConversation(const QString &conversation)
{
    if (!isDirectOfficeConversation(conversation))
        return QString();
    return conversation.mid(int(qstrlen("#office/@")));
}

QString roomForConversation(const QString &conversation)
{
    if (!isOfficeConversation(conversation) ||
        isDirectOfficeConversation(conversation))
        return QString();
    return conversation.mid(int(qstrlen("#office/")))
        .section(QLatin1Char('/'), -1);
}

QString organizationForConversation(const QString &conversation)
{
    if (!isOfficeConversation(conversation) ||
        isDirectOfficeConversation(conversation))
        return QString();
    const QString name = conversation.mid(int(qstrlen("#office/")));
    if (!name.contains(QLatin1Char('/')))
        return QString();
    return name.section(QLatin1Char('/'), -2, -2);
}

QString everyoneChannelId(const QString &name)
{
    return sha256Hex(QStringLiteral("forkmesh-everyone-channel-v1:") + name)
        .left(32);
}

bool isEveryoneChannelName(const QString &name)
{
    return name == QLatin1String("general") ||
           name == QLatin1String("random") || name == QLatin1String("welcome");
}

QStringList everyoneConversations()
{
    return {conversationForChannel(QStringLiteral("general")),
            conversationForChannel(QStringLiteral("random")),
            conversationForChannel(QStringLiteral("welcome"))};
}

QString everyoneGeneralConversation()
{
    return conversationForChannel(QStringLiteral("general"));
}

QString everyoneWelcomeConversation()
{
    return conversationForChannel(QStringLiteral("welcome"));
}

QString worldConversation()
{
    return QStringLiteral("#general");
}

QString worldLabel()
{
    return QStringLiteral("World");
}

bool isRetiredRelayChannel(const QString &channel)
{
    return channel == QLatin1String("#welcome") ||
           channel == QLatin1String("#random");
}

QByteArray channelCreateProof(const QString &account, const QString &ts,
                              const QString &name, const QString &visibility,
                              const QStringList &members)
{
    QStringList normalized;
    for (const QString &member : members)
        normalized.append(member.trimmed().toLower());
    const QString fields =
        QStringLiteral("name=%1\nvisibility=%2\nmembers=%3")
            .arg(name.trimmed().toLower(), visibility,
                 normalized.join(QLatin1Char(',')));
    return QStringLiteral("forkmesh-chat-channel-create-v1\n%1\n%2\n%3")
        .arg(account, ts, sha256Hex(fields))
        .toUtf8();
}

QByteArray channelMembersProof(const QString &account, const QString &channelId,
                               const QString &ts, const QString &method,
                               const QString &username)
{
    return QStringLiteral(
               "forkmesh-chat-channel-members-v1\n%1\n%2\n%3\n%4\n%5")
        .arg(account, channelId, ts, method,
             sha256Hex(username.trimmed().toLower()))
        .toUtf8();
}

QByteArray directListProof(const QString &account, const QString &ts)
{
    return QStringLiteral("forkmesh-chat-direct-list-v1\n%1\n%2")
        .arg(account, ts)
        .toUtf8();
}

QByteArray directStartProof(const QString &account, const QString &ts,
                            const QString &username)
{
    return QStringLiteral("forkmesh-chat-direct-start-v1\n%1\n%2\n%3")
        .arg(account, ts, sha256Hex(username.trimmed().toLower()))
        .toUtf8();
}

QByteArray directAccessProof(const QString &account,
                             const QString &conversationId, const QString &ts)
{
    return QStringLiteral("forkmesh-chat-direct-access-v1\n%1\n%2\n%3")
        .arg(account, conversationId, ts)
        .toUtf8();
}

QByteArray readMarksListProof(const QString &account, const QString &ts)
{
    return QStringLiteral("forkmesh-chat-reads-list-v1\n%1\n%2")
        .arg(account, ts)
        .toUtf8();
}

QString readMarksDigest(const QMap<QString, qint64> &marks)
{
    QStringList lines;
    for (auto it = marks.constBegin(); it != marks.constEnd(); ++it)
        lines.append(it.key() + QLatin1Char(' ') + QString::number(it.value()));
    return sha256Hex(lines.join(QLatin1Char('\n')));
}

QByteArray readMarksPostProof(const QString &account, const QString &ts,
                              const QMap<QString, qint64> &marks)
{
    return QStringLiteral("forkmesh-chat-reads-mark-v1\n%1\n%2\n%3")
        .arg(account, ts, readMarksDigest(marks))
        .toUtf8();
}

QByteArray channelListProof(const QString &account, const QString &ts)
{
    return QStringLiteral("forkmesh-chat-channels-v1\n%1\n%2")
        .arg(account, ts)
        .toUtf8();
}

QByteArray channelAccessProof(const QString &account, const QString &channelId,
                              const QString &ts)
{
    return QStringLiteral("forkmesh-chat-channel-access-v1\n%1\n%2\n%3")
        .arg(account, channelId, ts)
        .toUtf8();
}

QByteArray channelHistoryProof(const QString &account, const QString &channelId,
                               const QString &ts)
{
    return QStringLiteral("forkmesh-chat-channel-history-v1\n%1\n%2\n%3")
        .arg(account, channelId, ts)
        .toUtf8();
}

QByteArray channelTopicProof(const QString &account, const QString &channelId,
                             const QString &ts, const QString &topic)
{
    const QByteArray digest = QCryptographicHash::hash(
                                  topic.simplified().toUtf8(),
                                  QCryptographicHash::Sha256)
                                  .toHex();
    return QStringLiteral("forkmesh-chat-channel-topic-v1\n%1\n%2\n%3\n%4")
        .arg(account, channelId, ts, QString::fromLatin1(digest))
        .toUtf8();
}

QByteArray channelSettingsProof(const QString &account,
                                const QString &channelId, const QString &ts,
                                const QString &name, const QString &visibility,
                                const QString &topic)
{
    // Only named fields are signed, so no two different settings bodies can
    // share one signature. The desktop always sends all three.
    const QString fields = QStringLiteral("name=%1\nvisibility=%2\ntopic=%3")
                               .arg(name.trimmed().toLower(), visibility,
                                    topic.simplified());
    const QByteArray digest =
        QCryptographicHash::hash(fields.toUtf8(), QCryptographicHash::Sha256)
            .toHex();
    return QStringLiteral("forkmesh-chat-channel-settings-v1\n%1\n%2\n%3\n%4")
        .arg(account, channelId, ts, QString::fromLatin1(digest))
        .toUtf8();
}

QByteArray channelDeleteProof(const QString &account, const QString &channelId,
                              const QString &ts)
{
    return QStringLiteral("forkmesh-chat-channel-delete-v1\n%1\n%2\n%3")
        .arg(account, channelId, ts)
        .toUtf8();
}

QByteArray channelWebhooksProof(const QString &account,
                                const QString &channelId, const QString &ts,
                                const QString &method, const QString &name)
{
    const QByteArray digest = QCryptographicHash::hash(
                                  name.simplified().toUtf8(),
                                  QCryptographicHash::Sha256)
                                  .toHex();
    return QStringLiteral("forkmesh-chat-channel-webhooks-v1\n%1\n%2\n%3\n%4\n%5")
        .arg(account, channelId, ts, method, QString::fromLatin1(digest))
        .toUtf8();
}

QByteArray channelWebhookProof(const QString &account, const QString &channelId,
                               const QString &webhookId, const QString &ts,
                               const QString &method)
{
    return QStringLiteral("forkmesh-chat-channel-webhook-v1\n%1\n%2\n%3\n%4\n%5")
        .arg(account, channelId, webhookId, ts, method)
        .toUtf8();
}

bool chatMessageFromPlain(const QJsonObject &plain, const QString &conversation,
                          const QString &selfId, ChatMessage *out)
{
    if (!out || conversation.isEmpty())
        return false;
    const QString type = plain.value(QStringLiteral("type")).toString();
    if (type != QLatin1String("chat") && type != QLatin1String("thread-reply"))
        return false;
    const QString rootId =
        plain.value(QStringLiteral("rootId")).toString().trimmed().left(96);
    if (type == QLatin1String("thread-reply") && rootId.isEmpty())
        return false;
    const QString id = plain.value(QStringLiteral("id")).toString();
    if (id.isEmpty() || id.size() > 64)
        return false;
    // The office is an untrusted publisher like any other room member: every
    // field is bounded here, exactly as the mesh room's frames are.
    if (plain.value(QStringLiteral("text")).toString().size() > kMaxTextChars)
        return false;

    ChatMessage message;
    message.id = id;
    message.conversation = conversation;
    if (type == QLatin1String("thread-reply"))
        message.threadRootId = rootId;
    message.senderId =
        plain.value(QStringLiteral("senderId")).toString().left(64);
    message.senderName = boundedText(plain, "sender", kMaxDisplayNameChars);
    if (message.senderName.isEmpty())
        message.senderName = QStringLiteral("Office visitor");
    message.text = forkmesh::chatformat::bodySource(plain, kMaxTextChars);
    message.timestampMs = qint64(plain.value(QStringLiteral("ts")).toDouble());
    message.fileName = boundedText(plain, "fileName", kMaxFileNameChars);
    message.fileMime = boundedText(plain, "fileMime", kMaxMimeChars);
    if (!message.fileName.isEmpty()) {
        const QByteArray decoded = QByteArray::fromBase64(
            plain.value(QStringLiteral("file")).toString().toLatin1(),
            QByteArray::Base64Encoding | QByteArray::AbortOnBase64DecodingErrors);
        if (decoded.isEmpty() || decoded.size() > kMaxFileBytes) {
            message.fileName.clear();
            message.fileMime.clear();
        } else {
            message.fileData = decoded;
        }
    }
    if (forkmesh::chatblocks::senderMayFormat(message.senderId))
        message.blocks =
            forkmesh::chatblocks::sanitized(plain.value(QStringLiteral("blocks")));
    message.self = !selfId.isEmpty() && message.senderId == selfId;
    if (message.text.isEmpty() && !message.hasFile())
        return false;
    *out = message;
    return true;
}

bool roomMutationFromPlain(const QJsonObject &plain, RoomMutation *out)
{
    if (!out)
        return false;
    RoomMutation mutation;
    mutation.type = plain.value(QStringLiteral("type")).toString();
    if (mutation.type != QLatin1String("reaction") &&
        mutation.type != QLatin1String("edit") &&
        mutation.type != QLatin1String("delete"))
        return false;
    mutation.id = plain.value(QStringLiteral("id")).toString();
    mutation.target = plain.value(QStringLiteral("target")).toString();
    mutation.senderId =
        plain.value(QStringLiteral("senderId")).toString().left(64);
    if (mutation.id.isEmpty() || mutation.id.size() > 96 ||
        mutation.target.isEmpty() || mutation.target.size() > 96 ||
        mutation.senderId.isEmpty())
        return false;
    if (mutation.type == QLatin1String("reaction")) {
        const QString wireEmoji = plain.value(QStringLiteral("emoji")).toString();
        const QJsonValue added = plain.value(QStringLiteral("added"));
        if (wireEmoji.trimmed().isEmpty() || wireEmoji.size() > 32 ||
            !added.isBool())
            return false;
        mutation.emoji = stickers::valueFromWire(plain);
        mutation.added = added.toBool();
        const QString reactorId =
            plain.value(QStringLiteral("reactorId")).toString().left(64);
        if (!reactorId.isEmpty() && reactorId != mutation.senderId)
            return false;
        mutation.reactorName =
            boundedText(plain, "reactorName", kMaxDisplayNameChars).trimmed();
        if (mutation.reactorName.isEmpty())
            mutation.reactorName =
                boundedText(plain, "sender", kMaxDisplayNameChars).trimmed();
        if (mutation.reactorName.isEmpty())
            return false;
    } else if (mutation.type == QLatin1String("edit")) {
        if (plain.value(QStringLiteral("text")).toString().size() > kMaxTextChars)
            return false;
        mutation.text = ServerNode::moderatedChatText(
            forkmesh::chatformat::bodySource(plain, kMaxTextChars));
        if (mutation.text.trimmed().isEmpty())
            return false;
    }
    *out = mutation;
    return true;
}

} // namespace forkmesh::office

OfficeChannelMirror::OfficeChannelMirror(QNetworkAccessManager *network,
                                         QObject *parent)
    : QObject(parent), m_network(network)
{
    setChatDevice(forkmesh::chatv3::ChatDevice::local());
}

OfficeChannelMirror::~OfficeChannelMirror()
{
    if (m_device)
        m_device->unsubscribe(m_deviceToken);
}

void OfficeChannelMirror::setChatDevice(
    std::shared_ptr<forkmesh::chatv3::ChatDevice> device)
{
    if (!device || device == m_device)
        return;
    if (m_device)
        m_device->unsubscribe(m_deviceToken);
    m_device = std::move(device);
    m_deviceToken = m_device->subscribe(
        [this](forkmesh::chatv3::ChatDevice::Event event, const QString &scope,
               const QString &kid) { onDeviceEvent(event, scope, kid); });
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it)
        it->directSeal = RoomSeal();
}

void OfficeChannelMirror::setMeshTransport(MeshTransport transport)
{
    m_mesh = std::move(transport);
}

QString OfficeChannelMirror::selfDirectAcct() const
{
    return m_account.isEmpty() ? m_selfId : m_account;
}

const RoomSeal &OfficeChannelMirror::directSealFor(Room &room)
{
    if (room.direct && !room.room.isEmpty() &&
        (!room.directSeal.isValid() || room.directSeal.roomName() != room.room ||
         room.directSeal.device() != m_device ||
         room.directSeal.acct() != selfDirectAcct() ||
         room.directSeal.node() != m_selfId))
        room.directSeal = RoomSeal(room.room, QStringLiteral("direct:") + room.id,
                                   RoomSeal::Policy::Strict, selfDirectAcct(), m_selfId,
                                   m_device);
    room.directSeal.setParticipants(room.members);
    return room.directSeal;
}

void OfficeChannelMirror::setDirectRoomName(Room &room, const QString &roomName)
{
    const QString name = roomName.trimmed().left(kMaxDirectRoomNameChars);
    if (!room.direct || name.isEmpty() || name == room.room)
        return;
    room.room = name;
    room.directSeal = RoomSeal();
    saveDirectDirectory();
}

OfficeChannelMirror::Room *OfficeChannelMirror::roomForScope(const QString &scope)
{
    if (!scope.startsWith(QLatin1String("direct:")))
        return nullptr;
    const auto it = m_rooms.find(scope.mid(7));
    return it == m_rooms.end() || !it->direct ? nullptr : &it.value();
}

void OfficeChannelMirror::onDeviceEvent(forkmesh::chatv3::ChatDevice::Event event,
                                        const QString &scope, const QString &kid)
{
    using Event = forkmesh::chatv3::ChatDevice::Event;
    Room *room = roomForScope(scope);
    if (!room)
        return;
    const QString conversation = room->conversation;
    const QString roomId = room->id;
    switch (event) {
    case Event::DevicePending:
        emit directDevicePending(conversation, kid);
        emit directRouteChanged(conversation);
        break;
    case Event::SafetyNumberChanged:
        emit directSafetyNumberChanged(conversation);
        break;
    case Event::TrustChanged:
        emit directRouteChanged(conversation);
        QMetaObject::invokeMethod(
            this,
            [this, roomId] {
                const auto it = m_rooms.find(roomId);
                if (it == m_rooms.end())
                    return;
                if (it->sender)
                    it->sender->reopenHeldFrames();
                else
                    processHeldDirect(it.value());
            },
            Qt::QueuedConnection);
        break;
    case Event::PrekeyRotated:
        break;
    }
}

void OfficeChannelMirror::setSigner(std::function<QString(const QByteArray &)> signer)
{
    m_signer = std::move(signer);
}

void OfficeChannelMirror::setIdentity(const QString &account, const QString &selfId,
                                      const QString &displayName)
{
    const QString normalized = account.trimmed().toLower();
    const bool switched = normalized != m_account;
    if (switched) {
        for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
            failPending(it.value(), QStringLiteral("chat account changed"));
            discardSender(it.value());
        }
        m_rooms.clear();
        m_socketBudgetNoted = false;
        if (!m_conversations.isEmpty()) {
            m_conversations.clear();
            emit conversationsChanged(m_conversations);
        }
    }
    m_account = normalized;
    m_selfId = selfId;
    m_displayName = displayName.trimmed().left(kMaxDisplayNameChars);
    if (switched && m_active && ready()) {
        restoreDirectDirectory();
        fetchChannels();
    }
}

void OfficeChannelMirror::setConnectionAuthorizer(
    std::function<bool(const QUrl &)> authorizer)
{
    m_connectionAuthorizer = std::move(authorizer);
}

void OfficeChannelMirror::setApiBase(const QUrl &apiBase)
{
    QUrl base = apiBase;
    base.setPath(QString());
    base.setQuery(QString());
    base.setFragment(QString());
    m_apiBase = base;
}

bool OfficeChannelMirror::ready() const
{
    return m_network && m_signer && !m_account.isEmpty() &&
           m_apiBase.isValid() && !m_apiBase.host().isEmpty();
}

void OfficeChannelMirror::start()
{
    if (!ready() || m_active)
        return;
    m_active = true;
    restoreDirectDirectory();
    fetchChannels();
}

void OfficeChannelMirror::stop()
{
    m_active = false;
    for (auto it = m_directOnly.begin(); it != m_directOnly.end(); ++it)
        if (it->timer)
            it->timer->deleteLater();
    m_directOnly.clear();
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it)
        discardSender(it.value());
    m_rooms.clear();
    if (m_conversations.isEmpty())
        return;
    m_conversations.clear();
    emit conversationsChanged(m_conversations);
}

bool OfficeChannelMirror::isActive() const
{
    return m_active;
}

void OfficeChannelMirror::refresh()
{
    if (!ready() || !m_active)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (m_lastListMs > 0 && now - m_lastListMs < kMinRefreshIntervalMs)
        return;
    fetchChannels();
}

QStringList OfficeChannelMirror::conversations() const
{
    return m_conversations;
}

QStringList OfficeChannelMirror::directConversations() const
{
    return m_directConversations;
}

QString OfficeChannelMirror::wireChannel(const Room &room) const
{
    return room.direct ? QStringLiteral("direct:") + room.id
                       : QLatin1Char('#') + room.name;
}

bool OfficeChannelMirror::createChannel(const QString &name, bool privateRoom,
                                        const QStringList &members)
{
    const QString wanted = name.trimmed().toLower();
    if (!ready() || wanted.isEmpty() || wanted.size() > kMaxChannelNameChars)
        return false;
    QStringList invitees;
    if (privateRoom) {
        for (const QString &member : members) {
            const QString username = normalizedUsername(member);
            if (username.isEmpty())
                return false;
            if (username != m_account && !invitees.contains(username))
                invitees.append(username);
        }
        if (invitees.size() > kMaxInvitees)
            return false;
    }
    const QString visibility = privateRoom ? QStringLiteral("private")
                                           : QStringLiteral("public");
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(
        QStringLiteral("/api/chat/channels"), ts,
        forkmesh::office::channelCreateProof(m_account, ts, wanted, visibility,
                                             invitees));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QByteArray body =
        QJsonDocument(QJsonObject{
                          {QStringLiteral("name"), wanted},
                          {QStringLiteral("visibility"), visibility},
                          {QStringLiteral("members"),
                           QJsonArray::fromStringList(invitees)}})
            .toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_network->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray responseBody = reply->readAll();
        const bool failed = reply->error() != QNetworkReply::NoError;
        const QString transportError = failed ? reply->errorString() : QString();
        reply->deleteLater();
        const QJsonObject payload = QJsonDocument::fromJson(responseBody).object();
        const QJsonObject channel =
            payload.value(QStringLiteral("channel")).toObject();
        const QString conversation = forkmesh::office::conversationForChannel(
            channel.value(QStringLiteral("name")).toString());
        if (failed || conversation.isEmpty() ||
            !channelIdPattern()
                 .match(channel.value(QStringLiteral("id")).toString())
                 .hasMatch()) {
            static const QHash<QString, QString> messages = {
                {QStringLiteral("invalid_channel_name"),
                 QStringLiteral("Use lowercase letters, numbers, and hyphens.")},
                {QStringLiteral("channel_name_taken"),
                 QStringLiteral("That room name is already in use.")},
                {QStringLiteral("user_not_found"),
                 QStringLiteral("One of the people you invited has no "
                                "ForkMesh account.")},
                {QStringLiteral("too_many_channels"),
                 QStringLiteral("The relay has reached its channel limit.")},
                {QStringLiteral("invalid_session"),
                 QStringLiteral("This node's account key was not accepted.")},
            };
            QString reason = messages.value(
                payload.value(QStringLiteral("error")).toString());
            if (reason.isEmpty())
                reason = transportError.isEmpty()
                             ? QStringLiteral("the relay rejected the room")
                             : transportError;
            emit channelCreateFailed(reason);
            emit sendActivity(
                QStringLiteral("Office chat: could not create the room (%1).")
                    .arg(reason));
            return;
        }
        emit channelCreated(conversation);
        emit sendActivity(QStringLiteral("Office chat: created %1.")
                              .arg(conversation));
        m_lastListMs = 0;
        if (m_listing)
            m_relistPending = true;
        else
            fetchChannels();
    });
    return true;
}

bool OfficeChannelMirror::addMember(const QString &conversation,
                                    const QString &username)
{
    const QString name = normalizedUsername(username);
    if (name.isEmpty() || !isPrivateConversation(conversation) ||
        forkmesh::office::isDirectOfficeConversation(conversation) ||
        channelIdForConversation(conversation, true).isEmpty())
        return false;
    sendMemberChange(conversation, name, QByteArrayLiteral("POST"));
    return true;
}

bool OfficeChannelMirror::removeMember(const QString &conversation,
                                       const QString &username)
{
    const QString name = normalizedUsername(username);
    if (name.isEmpty() || !isPrivateConversation(conversation) ||
        forkmesh::office::isDirectOfficeConversation(conversation) ||
        channelIdForConversation(conversation, true).isEmpty())
        return false;
    sendMemberChange(conversation, name, QByteArrayLiteral("DELETE"));
    return true;
}

void OfficeChannelMirror::sendMemberChange(const QString &conversation,
                                           const QString &username,
                                           const QByteArray &method)
{
    const QString channelId = channelIdForConversation(conversation, true);
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(
        QStringLiteral("/api/chat/channels/%1/members").arg(channelId), ts,
        forkmesh::office::channelMembersProof(m_account, channelId, ts,
                                              QString::fromLatin1(method),
                                              username));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QByteArray body =
        QJsonDocument(QJsonObject{{QStringLiteral("username"), username}})
            .toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_network->sendCustomRequest(request, method, body);
    const bool adding = method == QByteArrayLiteral("POST");
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, conversation, username, adding]() {
                const QByteArray responseBody = reply->readAll();
                const bool failed = reply->error() != QNetworkReply::NoError;
                const QString transportError =
                    failed ? reply->errorString() : QString();
                reply->deleteLater();
                const bool ok = QJsonDocument::fromJson(responseBody)
                                    .object()
                                    .value(QStringLiteral("ok"))
                                    .toBool();
                if (failed || !ok) {
                    reportSettingsFailure(
                        conversation,
                        adding ? QStringLiteral("invite @%1").arg(username)
                               : QStringLiteral("remove @%1").arg(username),
                        transportError, responseBody);
                    return;
                }
                emit sendActivity(
                    adding ? QStringLiteral("Office chat: invited @%1 to %2.")
                                 .arg(username, conversation)
                           : QStringLiteral("Office chat: removed @%1 from %2.")
                                 .arg(username, conversation));
                m_lastListMs = 0;
                if (m_listing)
                    m_relistPending = true;
                else
                    fetchChannels();
            });
}

bool OfficeChannelMirror::startDirectMessage(const QString &username)
{
    const QString name = normalizedUsername(username);
    if (!ready() || name.isEmpty() || name == m_account)
        return false;
    const QString existing = forkmesh::office::conversationForDirect(name);
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it->direct && it->conversation == existing) {
            emit directConversationReady(existing);
            return true;
        }
    }
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(
        QStringLiteral("/api/chat/direct-messages"), ts,
        forkmesh::office::directStartProof(m_account, ts, name));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QByteArray body =
        QJsonDocument(QJsonObject{{QStringLiteral("username"), name}})
            .toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_network->post(request, body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, name]() {
        const QByteArray responseBody = reply->readAll();
        const bool failed = reply->error() != QNetworkReply::NoError;
        const QString transportError = failed ? reply->errorString() : QString();
        reply->deleteLater();
        const QJsonObject payload = QJsonDocument::fromJson(responseBody).object();
        const QString conversation = failed
            ? QString()
            : upsertDirectRoom(
                  payload.value(QStringLiteral("conversation")).toObject(),
                  0);
        if (conversation.isEmpty()) {
            static const QHash<QString, QString> messages = {
                {QStringLiteral("user_not_found"),
                 QStringLiteral("There is no ForkMesh account with that name.")},
                {QStringLiteral("cannot_message_self"),
                 QStringLiteral("You cannot message yourself.")},
                {QStringLiteral("rate_limited"),
                 QStringLiteral("Too many new conversations this hour. Try "
                                "again later.")},
                {QStringLiteral("invalid_session"),
                 QStringLiteral("This node's account key was not accepted.")},
            };
            QString reason = messages.value(
                payload.value(QStringLiteral("error")).toString());
            if (reason.isEmpty())
                reason = transportError.isEmpty()
                             ? QStringLiteral("the relay rejected the message")
                             : transportError;
            emit directMessageFailed(name, reason);
            return;
        }
        saveDirectDirectory();
        emit directConversationsChanged(m_directConversations);
        emit directConversationReady(conversation);
    });
    return true;
}

QString OfficeChannelMirror::upsertDirectRoom(const QJsonObject &conversation,
                                              int order)
{
    const QString id = conversation.value(QStringLiteral("id")).toString();
    const QString other = normalizedUsername(
        conversation.value(QStringLiteral("otherUser")).toString());
    if (!channelIdPattern().match(id).hasMatch() || other.isEmpty() ||
        other == m_account)
        return QString();
    const QString key = forkmesh::office::conversationForDirect(other);
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
        if (it.key() != id && it->conversation == key)
            return QString();
    Room &room = m_rooms[id];
    room.id = id;
    room.direct = true;
    room.privateRoom = true;
    room.serverVisible = false;
    room.readOnly = false;
    room.canManage = false;
    room.name = other;
    room.conversation = key;
    room.members = QStringList{m_account, other};
    room.order = order;
    room.wantsSocket = order < kMaxLiveDirectRooms || room.watched ||
                       conversation.value(QStringLiteral("unreadCount")).toInt() > 0;
    if (!m_directConversations.contains(key))
        m_directConversations.append(key);
    ensureReceiver(id);
    return key;
}

void OfficeChannelMirror::refreshDirectMessages()
{
    if (!ready() || !m_active)
        return;
    if (m_listingDirect) {
        m_relistDirectPending = true;
        return;
    }
    fetchDirectMessages();
}

void OfficeChannelMirror::watchConversation(const QString &conversation)
{
    if (!m_active)
        return;
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
        if (it->conversation != conversation)
            continue;
        it->watched = true;
        it->wantsSocket = true;
        ensureReceiver(it.key());
        return;
    }
}

QString OfficeChannelMirror::readMarkKey(const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it->conversation == conversation)
            return (it->direct ? QStringLiteral("direct:")
                               : QStringLiteral("private:")) +
                   it->id;
    }
    static const QRegularExpression publicKey(
        QStringLiteral("^#[a-z0-9][a-z0-9_-]{0,63}$"));
    return publicKey.match(conversation).hasMatch() ? conversation : QString();
}

void OfficeChannelMirror::fetchReadMarks()
{
    if (!ready() || !m_active)
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(QStringLiteral("/api/chat/reads"), ts,
                               forkmesh::office::readMarksListProof(m_account,
                                                                    ts));
    QNetworkReply *reply = m_network->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        const QByteArray body = reply->readAll();
        const bool failed = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();
        if (failed)
            return;
        const QJsonObject listed = QJsonDocument::fromJson(body)
                                       .object()
                                       .value(QStringLiteral("marks"))
                                       .toObject();
        QHash<QString, QString> conversations;
        for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
            conversations.insert(readMarkKey(it->conversation),
                                 it->conversation);
        QHash<QString, qint64> marks;
        for (auto it = listed.constBegin(); it != listed.constEnd(); ++it) {
            const qint64 ts = it.value().toVariant().toLongLong();
            if (ts <= 0)
                continue;
            const QString conversation =
                conversations.value(it.key(),
                                    it.key().startsWith(QLatin1Char('#'))
                                        ? it.key()
                                        : QString());
            if (!conversation.isEmpty())
                marks.insert(conversation, ts);
        }
        if (!marks.isEmpty())
            emit readMarksFetched(marks);
    });
}

void OfficeChannelMirror::postReadMarks(const QHash<QString, qint64> &marks)
{
    if (!ready() || !m_active)
        return;
    QMap<QString, qint64> keyed;
    for (auto it = marks.constBegin(); it != marks.constEnd(); ++it) {
        const QString key = readMarkKey(it.key());
        if (!key.isEmpty() && it.value() > 0 && keyed.size() < 50)
            keyed.insert(key, qMax(keyed.value(key), it.value()));
    }
    if (keyed.isEmpty())
        return;
    QJsonObject markObject;
    for (auto it = keyed.constBegin(); it != keyed.constEnd(); ++it)
        markObject.insert(it.key(), static_cast<double>(it.value()));
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    QNetworkRequest request(
        signedUrl(QStringLiteral("/api/chat/reads"), ts,
                  forkmesh::office::readMarksPostProof(m_account, ts, keyed)));
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_network->post(
        request,
        QJsonDocument(QJsonObject{{QStringLiteral("marks"), markObject}})
            .toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
}

QStringList OfficeChannelMirror::directDeviceKeys(
    const QString &conversation) const
{
    QStringList keys;
    for (const DirectDevice &device : directDevices(conversation))
        if (!device.own && device.state == forkmesh::chatv3::TrustState::Approved)
            keys.append(device.kid);
    return keys;
}

QString OfficeChannelMirror::directConversationForChannel(const QString &channel) const
{
    if (!channel.startsWith(QLatin1String("direct:")))
        return QString();
    const auto it = m_rooms.constFind(channel.mid(7));
    return it == m_rooms.constEnd() || !it->direct ? QString() : it->conversation;
}

bool OfficeChannelMirror::hasDirectSeal(const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
        if (it->direct && it->conversation == conversation)
            return !it->room.isEmpty();
    return false;
}

QList<OfficeChannelMirror::DirectDevice>
OfficeChannelMirror::directDevices(const QString &conversation) const
{
    QList<DirectDevice> devices;
    if (!m_device)
        return devices;
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (!it->direct || it->conversation != conversation)
            continue;
        for (const forkmesh::chatv3::TrustRecord &record :
             m_device->records(QStringLiteral("direct:") + it->id)) {
            if (record.kid == m_device->kid())
                continue;
            DirectDevice device;
            device.kid = record.kid;
            device.acct = record.acct;
            device.own = record.acct == selfDirectAcct();
            device.state = record.state;
            device.verified = record.verified;
            device.firstSeen = record.firstSeen;
            devices.append(device);
        }
        break;
    }
    return devices;
}

bool OfficeChannelMirror::approveDirectDevice(const QString &conversation,
                                              const QString &kid)
{
    Room *room = findRoom(conversation);
    if (!room || !room->direct || !m_device)
        return false;
    m_device->setState(QStringLiteral("direct:") + room->id, kid,
                       forkmesh::chatv3::TrustState::Approved);
    return true;
}

bool OfficeChannelMirror::blockDirectDevice(const QString &conversation,
                                            const QString &kid)
{
    Room *room = findRoom(conversation);
    if (!room || !room->direct || !m_device)
        return false;
    m_device->setState(QStringLiteral("direct:") + room->id, kid,
                       forkmesh::chatv3::TrustState::Blocked);
    return true;
}

forkmesh::chatv3::SafetyNumber
OfficeChannelMirror::directSafetyNumber(const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
        if (it->direct && it->conversation == conversation && m_device)
            return m_device->safetyNumberFor(QStringLiteral("direct:") + it->id);
    return {};
}

bool OfficeChannelMirror::isDirectVerified(const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
        if (it->direct && it->conversation == conversation && m_device)
            return m_device->isVerified(QStringLiteral("direct:") + it->id);
    return false;
}

void OfficeChannelMirror::markDirectVerified(const QString &conversation)
{
    Room *room = findRoom(conversation);
    if (room && room->direct && m_device)
        m_device->markVerified(QStringLiteral("direct:") + room->id);
}

QJsonObject OfficeChannelMirror::directPlain(const Room &room,
                                             const PendingText &pending) const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString name = m_displayName.isEmpty() ? m_account : m_displayName;
    QJsonObject plain{{QStringLiteral("id"), pending.id},
                      {QStringLiteral("senderId"), m_selfId},
                      {QStringLiteral("sender"), name},
                      {QStringLiteral("accountKind"), QStringLiteral("user")},
                      {QStringLiteral("nodeName"), m_account},
                      {QStringLiteral("ownerUser"), m_account},
                      {QStringLiteral("ts"), double(now)}};
    const QString channel = wireChannel(room);
    if (pending.kind == QLatin1String("reaction")) {
        plain.insert(QStringLiteral("type"), QStringLiteral("reaction"));
        plain.insert(QStringLiteral("conversation"), channel);
        plain.insert(QStringLiteral("target"), pending.target);
        stickers::writeWire(plain, pending.emoji);
        plain.insert(QStringLiteral("reactorId"), m_selfId);
        plain.insert(QStringLiteral("reactorName"), name);
        plain.insert(QStringLiteral("added"), pending.added);
    } else if (pending.kind == QLatin1String("edit")) {
        plain.insert(QStringLiteral("type"), QStringLiteral("edit"));
        plain.insert(QStringLiteral("conversation"), channel);
        plain.insert(QStringLiteral("target"), pending.target);
        plain.insert(QStringLiteral("text"), pending.text);
        plain.insert(QStringLiteral("editedAt"), double(now));
    } else if (pending.kind == QLatin1String("delete")) {
        plain.insert(QStringLiteral("type"), QStringLiteral("delete"));
        plain.insert(QStringLiteral("conversation"), channel);
        plain.insert(QStringLiteral("target"), pending.target);
    } else if (pending.kind == QLatin1String("reply")) {
        plain.insert(QStringLiteral("type"), QStringLiteral("thread-reply"));
        plain.insert(QStringLiteral("channel"), channel);
        plain.insert(QStringLiteral("rootId"), pending.rootId);
        plain.insert(QStringLiteral("text"), ServerNode::moderatedChatText(pending.text));
    } else {
        plain.insert(QStringLiteral("type"), QStringLiteral("chat"));
        plain.insert(QStringLiteral("channel"), channel);
        if (pending.fileData.isEmpty()) {
            plain.insert(QStringLiteral("text"), ServerNode::moderatedChatText(pending.text));
        } else {
            plain.insert(QStringLiteral("fileName"), pending.fileName);
            plain.insert(QStringLiteral("fileMime"), pending.fileMime);
            plain.insert(QStringLiteral("file"),
                         QString::fromLatin1(pending.fileData.toBase64()));
        }
    }
    return plain;
}

int OfficeChannelMirror::deliverOverMesh(Room &room, const QJsonObject &plain,
                                         qsizetype fileBytes, bool *directOnly,
                                         QStringList *peerKids)
{
    if (directOnly)
        *directOnly = false;
    if (!room.direct || !m_mesh.linkedDevices || !m_mesh.sendDirect ||
        room.room.isEmpty())
        return 0;
    const RoomSeal &seal = directSealFor(room);
    if (!seal.isValid())
        return 0;
    const QString channel = wireChannel(room);
    const QString self = seal.keyId();
    bool peerFocused = false;
    int reached = 0;
    const QList<forkmesh::chatv3::MeshLinkedDevice> devices = m_mesh.linkedDevices();
    for (const forkmesh::chatv3::MeshLinkedDevice &device : devices) {
        if (device.kid == self || !seal.isApproved(device.kid))
            continue;
        const qsizetype limit = device.bluetooth
                                    ? forkmesh::office::kMaxBluetoothDirectFileBytes
                                    : forkmesh::office::kMaxDirectFileBytes;
        if (fileBytes > limit)
            continue;
        const QJsonObject frame = seal.sealObjectFor(plain, {device.kid});
        if (frame.isEmpty() || !m_mesh.sendDirect(device.kid, channel, frame))
            continue;
        ++reached;
        if (seal.acctForKeyId(device.kid) == selfDirectAcct())
            continue;
        if (peerKids)
            peerKids->append(device.kid);
        if (device.focus.contains(channel))
            peerFocused = true;
    }
    if (directOnly)
        *directOnly = peerFocused && m_focusedConversation == room.conversation;
    return reached;
}

void OfficeChannelMirror::sendDirectPending(Room &room, const PendingText &pending)
{
    bool directOnly = false;
    QStringList peerKids;
    const int reached = deliverOverMesh(room, directPlain(room, pending),
                                        pending.fileData.size(), &directOnly,
                                        &peerKids);
    if (reached > 0) {
        room.meshInFlight.insert(pending.id);
        emit sendActivity(QStringLiteral("Office chat: sent a message for %1 to %2 "
                                         "device(s) over the local mesh.")
                              .arg(room.conversation)
                              .arg(reached));
    }
    if (directOnly && !m_directOnly.contains(pending.id)) {
        DirectOnly waiting;
        waiting.roomId = room.id;
        waiting.pending = pending;
        waiting.kids = peerKids;
        waiting.timer = new QTimer(this);
        waiting.timer->setSingleShot(true);
        const QString id = pending.id;
        connect(waiting.timer, &QTimer::timeout, this, [this, id] { fallBackToRelay(id); });
        waiting.timer->start(forkmesh::office::kDirectOnlyAckMs);
        m_directOnly.insert(id, waiting);
        return;
    }
    room.pendingTexts.enqueue(pending);
    if (room.sender && room.senderConnected)
        flushPending(room);
    else if (!room.sender && !room.accessFetching)
        fetchRoomAccess(room.id);
}

void OfficeChannelMirror::fallBackToRelay(const QString &messageId)
{
    const auto waiting = m_directOnly.find(messageId);
    if (waiting == m_directOnly.end())
        return;
    const DirectOnly entry = waiting.value();
    m_directOnly.erase(waiting);
    if (entry.timer)
        entry.timer->deleteLater();
    const auto it = m_rooms.find(entry.roomId);
    if (it == m_rooms.end())
        return;
    Room &room = it.value();
    room.pendingTexts.enqueue(entry.pending);
    emit sendActivity(QStringLiteral("Office chat: no direct answer for %1, sending "
                                     "through the relay.")
                          .arg(room.conversation));
    if (room.sender && room.senderConnected)
        flushPending(room);
    else if (!room.sender && !room.accessFetching)
        fetchRoomAccess(room.id);
}

QString OfficeChannelMirror::acceptMeshDirect(const QString &linkKid,
                                              const QString &channel,
                                              const QJsonObject &frame)
{
    if (!m_active || !channel.startsWith(QLatin1String("direct:")) ||
        linkKid.isEmpty() ||
        frame.value(QStringLiteral("kid")).toString() != linkKid)
        return QString();
    const auto it = m_rooms.find(channel.mid(7));
    if (it == m_rooms.end() || !it->direct || it->room.isEmpty())
        return QString();
    Room &room = it.value();
    const RoomSeal &seal = directSealFor(room);
    const RoomSeal::Opened opened = seal.open(frame);
    if (opened.status == RoomSeal::Status::NoSession && room.sender)
        room.sender->recoverSession(opened.senderKid, opened.ctr);
    if (opened.status != RoomSeal::Status::Opened || opened.self)
        return QString();
    if (RoomSeal::isResetPlain(opened.plain)) {
        if (room.sender)
            room.sender->resealOutboxTo(opened.senderKid);
        return QString();
    }
    QJsonObject plain = opened.plain;
    if (!plain.contains(QStringLiteral("senderId")))
        plain.insert(QStringLiteral("senderId"), opened.senderNode);
    RoomSeal::useSignedTime(plain, opened);
    const QString type = plain.value(QStringLiteral("type")).toString();
    const QString wire = wireChannel(room);
    const bool message = type == QLatin1String("chat") ||
                         type == QLatin1String("thread-reply");
    const QString bound = plain.value(message ? QStringLiteral("channel")
                                              : QStringLiteral("conversation"))
                              .toString();
    if (bound != wire)
        return QString();
    const QString id = plain.value(QStringLiteral("id")).toString().left(96);
    dispatchDirectPlain(room, plain);
    return id;
}

void OfficeChannelMirror::dispatchDirectPlain(Room &room, const QJsonObject &plain)
{
    const QString type = plain.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("chat") || type == QLatin1String("thread-reply")) {
        ChatMessage message;
        if (!forkmesh::office::chatMessageFromPlain(plain, room.conversation, m_selfId,
                                                    &message) ||
            room.seenIds.contains(message.id))
            return;
        rememberSeen(room, message.id);
        emit messageArrived(message);
        return;
    }
    if (type == QLatin1String("reaction") || type == QLatin1String("edit") ||
        type == QLatin1String("delete")) {
        applyMutation(room, plain);
        return;
    }
    if (type == QLatin1String("typing"))
        emit typingChanged(room.conversation,
                           plain.value(QStringLiteral("senderId")).toString(),
                           boundedText(plain, "sender", kMaxDisplayNameChars),
                           plain.value(QStringLiteral("active")).toBool());
}

void OfficeChannelMirror::processHeldDirect(Room &room)
{
    if (room.room.isEmpty())
        return;
    const RoomSeal &seal = directSealFor(room);
    const QList<QJsonObject> released = seal.takeReleasedHeld();
    for (const QJsonObject &frame : released) {
        const RoomSeal::Opened opened = seal.open(frame);
        if (opened.status != RoomSeal::Status::Opened || opened.self ||
            RoomSeal::isResetPlain(opened.plain))
            continue;
        QJsonObject plain = opened.plain;
        if (!plain.contains(QStringLiteral("senderId")))
            plain.insert(QStringLiteral("senderId"), opened.senderNode);
        RoomSeal::useSignedTime(plain, opened);
        dispatchDirectPlain(room, plain);
    }
}

void OfficeChannelMirror::meshDirectAcked(const QString &kid, const QString &channel,
                                          const QString &messageId)
{
    if (!channel.startsWith(QLatin1String("direct:")) || messageId.isEmpty())
        return;
    const auto it = m_rooms.find(channel.mid(7));
    if (it == m_rooms.end() || !it->direct)
        return;
    Room &room = it.value();
    const RoomSeal &seal = directSealFor(room);
    if (!seal.isApproved(kid) || seal.acctForKeyId(kid) == selfDirectAcct())
        return;
    const auto waiting = m_directOnly.find(messageId);
    if (waiting != m_directOnly.end()) {
        if (waiting->timer)
            waiting->timer->deleteLater();
        m_directOnly.erase(waiting);
    }
    room.meshInFlight.remove(messageId);
    if (room.meshDelivered.contains(messageId))
        return;
    room.meshDelivered.insert(messageId);
    emit messagesDelivered(room.conversation, {messageId});
}

void OfficeChannelMirror::meshLinksChanged()
{
    QSet<QString> linked;
    if (m_mesh.linkedDevices)
        for (const forkmesh::chatv3::MeshLinkedDevice &device : m_mesh.linkedDevices())
            linked.insert(device.kid);
    QStringList stranded;
    for (auto it = m_directOnly.cbegin(); it != m_directOnly.cend(); ++it) {
        bool reachable = false;
        for (const QString &kid : it->kids)
            reachable = reachable || linked.contains(kid);
        if (!reachable)
            stranded.append(it.key());
    }
    for (const QString &id : std::as_const(stranded))
        fallBackToRelay(id);
    for (const QString &conversation : std::as_const(m_directConversations))
        emit directRouteChanged(conversation);
}

void OfficeChannelMirror::meshPeerFocusChanged(const QString &kid,
                                               const QString &channel, bool on)
{
    Q_UNUSED(kid);
    Q_UNUSED(on);
    const QString conversation = directConversationForChannel(channel);
    if (!conversation.isEmpty())
        emit directRouteChanged(conversation);
}

void OfficeChannelMirror::setFocusedConversation(const QString &conversation)
{
    if (conversation == m_focusedConversation)
        return;
    const QString previous = m_focusedConversation;
    m_focusedConversation = conversation;
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (!it->direct)
            continue;
        if (it->conversation == previous && m_mesh.sendFocus)
            m_mesh.sendFocus(wireChannel(it.value()), false);
        if (it->conversation == conversation && m_mesh.sendFocus)
            m_mesh.sendFocus(wireChannel(it.value()), true);
    }
    if (!previous.isEmpty())
        emit directRouteChanged(previous);
    if (!conversation.isEmpty())
        emit directRouteChanged(conversation);
}

QString OfficeChannelMirror::directRouteLabel(const QString &conversation) const
{
    if (!m_mesh.linkedDevices || !m_device)
        return QString();
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (!it->direct || it->conversation != conversation || it->room.isEmpty())
            continue;
        const QString scope = QStringLiteral("direct:") + it->id;
        const QString channel = wireChannel(it.value());
        bool lan = false;
        bool bluetooth = false;
        bool focused = false;
        for (const forkmesh::chatv3::MeshLinkedDevice &device : m_mesh.linkedDevices()) {
            const forkmesh::chatv3::TrustRecord *record =
                m_device->record(scope, device.kid);
            if (!record || record->state != forkmesh::chatv3::TrustState::Approved ||
                record->acct == selfDirectAcct())
                continue;
            (device.bluetooth ? bluetooth : lan) = true;
            focused = focused || device.focus.contains(channel);
        }
        if (focused && m_focusedConversation == conversation)
            return QStringLiteral("Direct only");
        if (lan)
            return QStringLiteral("Direct · Wi-Fi");
        if (bluetooth)
            return QStringLiteral("Direct · Bluetooth");
        return QString();
    }
    return QString();
}

void OfficeChannelMirror::saveDirectDirectory() const
{
    if (m_account.isEmpty())
        return;
    QJsonArray conversations;
    for (const QString &conversation : m_directConversations) {
        for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
            if (!it->direct || it->conversation != conversation)
                continue;
            conversations.append(QJsonObject{
                {QStringLiteral("id"), it->id},
                {QStringLiteral("otherUser"), it->name},
                {QStringLiteral("room"), it->room}});
            break;
        }
    }
    const QJsonObject root{{QStringLiteral("v"), 1},
                           {QStringLiteral("apiBase"), m_apiBase.toString()},
                           {QStringLiteral("account"), m_account},
                           {QStringLiteral("conversations"), conversations}};
    forkmesh::settings::put(QSettings(), QStringLiteral("chat/direct/meshDirectory"),
                            QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void OfficeChannelMirror::restoreDirectDirectory()
{
    if (m_account.isEmpty())
        return;
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
        if (it->direct)
            return;
    const QJsonObject root =
        QJsonDocument::fromJson(
            QSettings().value(QStringLiteral("chat/direct/meshDirectory"))
                .toByteArray())
            .object();
    if (root.value(QStringLiteral("v")).toInt() != 1 ||
        root.value(QStringLiteral("apiBase")).toString() != m_apiBase.toString() ||
        root.value(QStringLiteral("account")).toString() != m_account)
        return;
    const QJsonArray saved = root.value(QStringLiteral("conversations")).toArray();
    QStringList ordered;
    for (const QJsonValue &value : saved) {
        if (ordered.size() >= kMaxDirectConversations)
            break;
        const QJsonObject entry = value.toObject();
        const QString conversation = upsertDirectRoom(entry, int(ordered.size()));
        if (conversation.isEmpty())
            continue;
        Room &room = m_rooms[entry.value(QStringLiteral("id")).toString()];
        const QString roomName = entry.value(QStringLiteral("room")).toString().trimmed();
        if (room.room.isEmpty() && !roomName.isEmpty())
            room.room = roomName.left(kMaxDirectRoomNameChars);
        ordered.append(conversation);
    }
    if (ordered.isEmpty())
        return;
    m_directConversations = ordered;
    emit directConversationsChanged(m_directConversations);
}

void OfficeChannelMirror::fetchDirectMessages()
{
    if (m_listingDirect || !ready())
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    QUrl url = signedUrl(QStringLiteral("/api/chat/direct-messages"), ts,
                         forkmesh::office::directListProof(m_account, ts));
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("limit"),
                       QString::number(kMaxDirectConversations));
    url.setQuery(query);
    m_listingDirect = true;
    QNetworkReply *reply = m_network->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_listingDirect = false;
        if (m_relistDirectPending) {
            m_relistDirectPending = false;
            QTimer::singleShot(0, this, [this] { refreshDirectMessages(); });
        }
        const QByteArray body = reply->readAll();
        const bool failed = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();
        if (failed)
            return;
        const QJsonArray listed = QJsonDocument::fromJson(body)
                                      .object()
                                      .value(QStringLiteral("conversations"))
                                      .toArray();
        const QStringList before = m_directConversations;
        QSet<QString> live;
        QStringList ordered;
        int order = 0;
        for (const QJsonValue &value : listed) {
            if (ordered.size() >= kMaxDirectConversations)
                break;
            const QString conversation =
                upsertDirectRoom(value.toObject(), order);
            if (conversation.isEmpty())
                continue;
            live.insert(value.toObject().value(QStringLiteral("id")).toString());
            ordered.append(conversation);
            ++order;
        }
        const QStringList known = m_rooms.keys();
        for (const QString &id : known) {
            if (!m_rooms[id].direct || live.contains(id))
                continue;
            failPending(m_rooms[id],
                        QStringLiteral("conversation is no longer available"));
            discardSender(m_rooms[id]);
            m_rooms.remove(id);
        }
        m_directConversations = ordered;
        saveDirectDirectory();
        if (m_directConversations != before)
            emit directConversationsChanged(m_directConversations);
        fetchReadMarks();
    });
}

QStringList OfficeChannelMirror::membersForConversation(
    const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it->conversation == conversation)
            return it->members;
    }
    return {};
}

bool OfficeChannelMirror::isPrivateConversation(
    const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it->conversation == conversation)
            return it->privateRoom;
    }
    return false;
}

bool OfficeChannelMirror::isEveryoneConversation(
    const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it->conversation == conversation)
            return it->everyone;
    }
    return false;
}

int OfficeChannelMirror::memberCountForConversation(
    const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it->conversation == conversation)
            return it->everyone ? qMax(int(it->memberCount),
                                       int(it->members.size()))
                                : int(it->members.size());
    }
    return 0;
}

bool OfficeChannelMirror::isServerVisibleConversation(
    const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it->conversation == conversation)
            return it->serverVisible;
    }
    return false;
}

bool OfficeChannelMirror::isReadOnlyConversation(
    const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it->conversation == conversation)
            return it->readOnly;
    }
    return false;
}

QString OfficeChannelMirror::topicForConversation(
    const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
        if (it->conversation == conversation)
            return it->topic;
    return {};
}

bool OfficeChannelMirror::canManageConversation(
    const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
        if (it->conversation == conversation)
            return it->canManage;
    return false;
}

bool OfficeChannelMirror::canSend(const QString &conversation) const
{
    if (!ready() || !forkmesh::office::isOfficeConversation(conversation))
        return false;
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
        if (it->conversation == conversation)
            return !it->readOnly;
    return false;
}

bool OfficeChannelMirror::sendMessage(const QString &conversation,
                                      const QString &text)
{
    const QString message = text.trimmed().left(kMaxTextChars);
    if (message.isEmpty() || !canSend(conversation))
        return false;
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
        Room &room = it.value();
        if (room.conversation != conversation)
            continue;
        if (!reservePendingSlot(room))
            return false;
        const QString messageId =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        const PendingText pending{messageId, message, {}, {}, {}};
        rememberSeen(room, messageId);
        ChatMessage echo;
        echo.id = messageId;
        echo.conversation = room.conversation;
        echo.senderId = m_selfId;
        echo.senderName = m_displayName.isEmpty() ? m_account : m_displayName;
        echo.text = ServerNode::moderatedChatText(message);
        echo.timestampMs = QDateTime::currentMSecsSinceEpoch();
        echo.self = true;
        emit messageArrived(echo);
        emit sendActivity(QStringLiteral("Office chat: queued a message for %1.")
                              .arg(conversation));
        if (room.direct) {
            sendDirectPending(room, pending);
            return true;
        }
        room.pendingTexts.enqueue(pending);
        if (room.sender && room.senderConnected)
            flushPending(room);
        else if (!room.sender && !room.accessFetching)
            fetchRoomAccess(room.id);
        return true;
    }
    return false;
}

bool OfficeChannelMirror::canSendFile(const QString &conversation) const
{
    if (!canSend(conversation))
        return false;
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
        if (it->conversation == conversation)
            return it->direct;
    return false;
}

bool OfficeChannelMirror::sendFile(const QString &conversation,
                                   const QString &fileName,
                                   const QString &mimeType,
                                   const QByteArray &data)
{
    const QString name = fileName.trimmed().left(kMaxFileNameChars);
    if (name.isEmpty() || data.isEmpty() ||
        data.size() > forkmesh::office::kMaxDirectFileBytes ||
        !canSendFile(conversation))
        return false;
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
        Room &room = it.value();
        if (room.conversation != conversation)
            continue;
        if (!reservePendingSlot(room))
            return false;
        const QString messageId =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString mime = mimeType.trimmed().isEmpty()
                                 ? QStringLiteral("application/octet-stream")
                                 : mimeType.trimmed().left(kMaxMimeChars);
        const PendingText pending{messageId, QString(), name, mime, data};
        rememberSeen(room, messageId);
        ChatMessage echo;
        echo.id = messageId;
        echo.conversation = room.conversation;
        echo.senderId = m_selfId;
        echo.senderName = m_displayName.isEmpty() ? m_account : m_displayName;
        echo.fileName = name;
        echo.fileMime = mime;
        echo.fileData = data;
        echo.timestampMs = QDateTime::currentMSecsSinceEpoch();
        echo.self = true;
        emit messageArrived(echo);
        emit sendActivity(QStringLiteral("Office chat: queued %1 for %2.")
                              .arg(name, conversation));
        if (room.direct) {
            sendDirectPending(room, pending);
            return true;
        }
        room.pendingTexts.enqueue(pending);
        if (room.sender && room.senderConnected)
            flushPending(room);
        else if (!room.sender && !room.accessFetching)
            fetchRoomAccess(room.id);
        return true;
    }
    return false;
}

bool OfficeChannelMirror::reservePendingSlot(Room &room)
{
    if (room.pendingTexts.size() >= kMaxPendingTextsPerRoom) {
        for (qsizetype i = 0; i < room.pendingTexts.size(); ++i) {
            if (room.meshDelivered.remove(room.pendingTexts.at(i).id)) {
                room.pendingTexts.removeAt(i);
                break;
            }
        }
    }
    if (room.pendingTexts.size() < kMaxPendingTextsPerRoom)
        return true;
    emit sendActivity(QStringLiteral("Office chat: send queue for %1 is full.")
                          .arg(room.conversation));
    return false;
}

bool OfficeChannelMirror::sendBotMessage(const QString &conversation,
                                         const QString &text)
{
    const QString message = text.trimmed().left(kMaxTextChars);
    if (message.isEmpty())
        return false;
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
        Room &room = it.value();
        if (room.conversation != conversation || room.privateRoom ||
            !room.sender || !room.senderConnected) {
            continue;
        }
        room.sender->sendBotChat(QLatin1Char('#') + room.name, message);
        return true;
    }
    return false;
}

OfficeChannelMirror::Room *OfficeChannelMirror::findRoom(
    const QString &conversation)
{
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it)
        if (it->conversation == conversation)
            return &it.value();
    return nullptr;
}

bool OfficeChannelMirror::queueMutation(const QString &conversation,
                                        const PendingText &pending)
{
    if (!canSend(conversation))
        return false;
    Room *room = findRoom(conversation);
    if (!room || !reservePendingSlot(*room))
        return false;
    if (room->direct) {
        sendDirectPending(*room, pending);
        return true;
    }
    room->pendingTexts.enqueue(pending);
    if (room->sender && room->senderConnected)
        flushPending(*room);
    else if (!room->sender && !room->accessFetching)
        fetchRoomAccess(room->id);
    return true;
}

bool OfficeChannelMirror::sendThreadReply(const QString &conversation,
                                          const QString &rootId,
                                          const QString &text)
{
    const QString message = text.trimmed().left(kMaxTextChars);
    const QString root = rootId.trimmed().left(96);
    if (message.isEmpty() || root.isEmpty() || !canSend(conversation))
        return false;
    Room *room = findRoom(conversation);
    if (!room || !reservePendingSlot(*room))
        return false;
    PendingText pending;
    pending.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending.text = message;
    pending.kind = QStringLiteral("reply");
    pending.rootId = root;
    rememberSeen(*room, pending.id);
    ChatMessage echo;
    echo.id = pending.id;
    echo.conversation = room->conversation;
    echo.threadRootId = root;
    echo.senderId = m_selfId;
    echo.senderName = m_displayName.isEmpty() ? m_account : m_displayName;
    echo.text = ServerNode::moderatedChatText(message);
    echo.timestampMs = QDateTime::currentMSecsSinceEpoch();
    echo.self = true;
    const QString roomId = room->id;
    emit messageArrived(echo);
    if (!m_rooms.contains(roomId))
        return true;
    Room &live = m_rooms[roomId];
    if (live.direct) {
        sendDirectPending(live, pending);
        return true;
    }
    live.pendingTexts.enqueue(pending);
    if (live.sender && live.senderConnected)
        flushPending(live);
    else if (!live.sender && !live.accessFetching)
        fetchRoomAccess(roomId);
    return true;
}

bool OfficeChannelMirror::sendReaction(const QString &conversation,
                                       const QString &messageId,
                                       const QString &emoji, bool added)
{
    const QString target = messageId.trimmed();
    if (target.isEmpty() || target.size() > 96 || emoji.trimmed().isEmpty() ||
        emoji.size() > 32)
        return false;
    PendingText pending;
    pending.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending.kind = QStringLiteral("reaction");
    pending.target = target;
    pending.emoji = emoji;
    pending.added = added;
    if (!queueMutation(conversation, pending))
        return false;
    emit reactionChanged(conversation, target, emoji,
                         m_displayName.isEmpty() ? m_account : m_displayName,
                         added);
    return true;
}

bool OfficeChannelMirror::editMessage(const QString &conversation,
                                      const QString &messageId,
                                      const QString &text)
{
    const QString target = messageId.trimmed();
    const QString edited =
        ServerNode::moderatedChatText(text.trimmed().left(kMaxTextChars));
    if (target.isEmpty() || target.size() > 96 || edited.trimmed().isEmpty())
        return false;
    Room *room = canSend(conversation) ? findRoom(conversation) : nullptr;
    if (!room)
        return false;
    bool queued = false;
    for (PendingText &pending : room->pendingTexts) {
        if (pending.id == target && pending.fileData.isEmpty() &&
            (pending.kind.isEmpty() || pending.kind == QLatin1String("reply")) &&
            !room->meshDelivered.contains(target) &&
            !room->meshInFlight.contains(target)) {
            pending.text = edited;
            queued = true;
        }
    }
    if (!queued) {
        PendingText pending;
        pending.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        pending.kind = QStringLiteral("edit");
        pending.target = target;
        pending.text = edited;
        if (!queueMutation(conversation, pending))
            return false;
    }
    emit messageEdited(conversation, target, m_selfId, edited);
    return true;
}

bool OfficeChannelMirror::deleteMessage(const QString &conversation,
                                        const QString &messageId)
{
    const QString target = messageId.trimmed();
    if (target.isEmpty() || target.size() > 96)
        return false;
    Room *room = canSend(conversation) ? findRoom(conversation) : nullptr;
    if (!room)
        return false;
    bool unsent = false;
    if (!room->meshDelivered.contains(target) && !room->meshInFlight.contains(target)) {
        for (qsizetype i = room->pendingTexts.size() - 1; i >= 0; --i) {
            const PendingText &pending = room->pendingTexts.at(i);
            if (pending.id == target &&
                (pending.kind.isEmpty() ||
                 pending.kind == QLatin1String("reply"))) {
                room->pendingTexts.removeAt(i);
                unsent = true;
            }
        }
    }
    if (!unsent) {
        PendingText pending;
        pending.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        pending.kind = QStringLiteral("delete");
        pending.target = target;
        if (!queueMutation(conversation, pending))
            return false;
    }
    emit messageDeleted(conversation, target, m_selfId);
    return true;
}

bool OfficeChannelMirror::setTopic(const QString &conversation,
                                   const QString &topic)
{
    const QString normalized = topic.simplified();
    if (normalized.size() > kMaxTopicChars || !ready())
        return false;
    for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
        Room &room = it.value();
        if (room.conversation != conversation || !room.canManage)
            continue;
        const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QUrl url = signedUrl(
            QStringLiteral("/api/chat/channels/%1").arg(room.id), ts,
            forkmesh::office::channelTopicProof(
                m_account, room.id, ts, normalized));
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::ContentTypeHeader,
                          QStringLiteral("application/json"));
        const QByteArray body = QJsonDocument(
                                    QJsonObject{{QStringLiteral("topic"),
                                                 normalized}})
                                    .toJson(QJsonDocument::Compact);
        QNetworkReply *reply = m_network->sendCustomRequest(
            request, QByteArrayLiteral("PATCH"), body);
        const QString channelId = room.id;
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, channelId, conversation]() {
                    const QByteArray responseBody = reply->readAll();
                    const bool failed = reply->error() != QNetworkReply::NoError;
                    const QString reason = reply->errorString();
                    const QJsonObject payload =
                        QJsonDocument::fromJson(responseBody).object();
                    const bool validResponse =
                        payload.value(QStringLiteral("ok")).toBool() &&
                        payload.value(QStringLiteral("channel")).isObject();
                    reply->deleteLater();
                    if (failed || !validResponse || !m_rooms.contains(channelId)) {
                        emit sendActivity(
                            QStringLiteral("Office chat: could not change the "
                                           "topic for %1 (%2).")
                                .arg(conversation,
                                     failed && !reason.isEmpty()
                                         ? reason
                                         : QStringLiteral("invalid response")));
                        return;
                    }
                    const QString saved =
                        payload.value(QStringLiteral("channel"))
                            .toObject()
                            .value(QStringLiteral("topic"))
                            .toString()
                            .simplified()
                            .left(kMaxTopicChars);
                    Room &savedRoom = m_rooms[channelId];
                    savedRoom.topic = saved;
                    emit topicChanged(conversation, saved);
                    emit sendActivity(
                        saved.isEmpty()
                            ? QStringLiteral("Office chat: cleared the topic for %1.")
                                  .arg(conversation)
                            : QStringLiteral("Office chat: changed the topic for %1 "
                                             "to \"%2\".")
                                  .arg(conversation, saved));
                });
        return true;
    }
    return false;
}

QUrl OfficeChannelMirror::signedUrl(const QString &path, const QString &ts,
                                    const QByteArray &canonical) const
{
    QUrl url = m_apiBase;
    url.setPath(path);
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("node"), m_account);
    query.addQueryItem(QStringLiteral("ts"), ts);
    query.addQueryItem(QStringLiteral("sig"), m_signer(canonical));
    url.setQuery(query);
    return url;
}

QString OfficeChannelMirror::channelNameForConversation(
    const QString &conversation) const
{
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it)
        if (it->conversation == conversation)
            return it->name;
    return {};
}

QString OfficeChannelMirror::channelIdFor(const QString &conversation) const
{
    return channelIdForConversation(conversation, false);
}

QString OfficeChannelMirror::channelIdForConversation(
    const QString &conversation, bool requireManager) const
{
    if (!ready())
        return {};
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (it->conversation != conversation)
            continue;
        if (requireManager && !it->canManage)
            return {};
        return it->id;
    }
    return {};
}

// One place decides what a failed settings write says, so the dialog and the
// activity log never disagree about why the relay refused.
void OfficeChannelMirror::reportSettingsFailure(const QString &conversation,
                                                const QString &action,
                                                const QString &transportError,
                                                const QByteArray &body)
{
    static const QHash<QString, QString> messages = {
        {QStringLiteral("invalid_channel_name"),
         QStringLiteral("Use lowercase letters, numbers, and hyphens.")},
        {QStringLiteral("channel_name_taken"),
         QStringLiteral("That channel name is already in use.")},
        {QStringLiteral("invalid_visibility"),
         QStringLiteral("Choose Public or Private visibility.")},
        {QStringLiteral("invalid_topic"),
         QStringLiteral("Topics must be 200 characters or fewer.")},
        {QStringLiteral("invalid_webhook_name"),
         QStringLiteral("Webhook names must be 80 characters or fewer.")},
        {QStringLiteral("too_many_webhooks"),
         QStringLiteral("This channel has reached its webhook limit.")},
        {QStringLiteral("public_channel_required"),
         QStringLiteral("Controller integrations require a Public channel.")},
        {QStringLiteral("manager_required"),
         QStringLiteral("Only the channel creator or an administrator can "
                        "manage it.")},
        {QStringLiteral("invalid_session"),
         QStringLiteral("This node's account key was not accepted.")},
        {QStringLiteral("not_found"),
         QStringLiteral("That channel no longer exists.")},
    };
    const QString code = QJsonDocument::fromJson(body)
                             .object()
                             .value(QStringLiteral("error"))
                             .toString();
    QString reason = messages.value(code);
    if (reason.isEmpty()) {
        reason = transportError.isEmpty()
                     ? QStringLiteral("the relay rejected the change")
                     : transportError;
    }
    emit channelSettingsFailed(conversation, reason);
    emit sendActivity(QStringLiteral("Office chat: could not %1 for %2 (%3).")
                          .arg(action, conversation, reason));
}

bool OfficeChannelMirror::updateChannel(const QString &conversation,
                                        const QString &name,
                                        const QString &topic, bool privateRoom)
{
    const QString channelId = channelIdForConversation(conversation, true);
    const QString wanted = name.trimmed().toLower().left(kMaxChannelNameChars);
    const QString normalizedTopic = topic.simplified().left(kMaxTopicChars);
    const QString visibility = privateRoom ? QStringLiteral("private")
                                           : QStringLiteral("public");
    if (channelId.isEmpty() || wanted.isEmpty())
        return false;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(
        QStringLiteral("/api/chat/channels/%1").arg(channelId), ts,
        forkmesh::office::channelSettingsProof(m_account, channelId, ts, wanted,
                                               visibility, normalizedTopic));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QByteArray body =
        QJsonDocument(QJsonObject{{QStringLiteral("name"), wanted},
                                  {QStringLiteral("visibility"), visibility},
                                  {QStringLiteral("topic"), normalizedTopic}})
            .toJson(QJsonDocument::Compact);
    QNetworkReply *reply =
        m_network->sendCustomRequest(request, QByteArrayLiteral("PATCH"), body);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, channelId, conversation]() {
                const QByteArray responseBody = reply->readAll();
                const bool failed = reply->error() != QNetworkReply::NoError;
                const QString transportError =
                    failed ? reply->errorString() : QString();
                const QJsonObject channel =
                    QJsonDocument::fromJson(responseBody)
                        .object()
                        .value(QStringLiteral("channel"))
                        .toObject();
                reply->deleteLater();
                if (failed || channel.isEmpty() ||
                    !m_rooms.contains(channelId)) {
                    reportSettingsFailure(conversation,
                                          QStringLiteral("save the settings"),
                                          transportError, responseBody);
                    return;
                }
                applyChannelRecord(channelId, channel);
                emit channelSettingsSaved(m_rooms[channelId].conversation);
                // A visibility change rotates the room key, so re-read the
                // list rather than keep talking to the retired room.
                m_lastListMs = 0;
                refresh();
            });
    return true;
}

// Fold one server channel record into its room, announcing a rename so the
// window can move the conversation it already holds messages under.
void OfficeChannelMirror::applyChannelRecord(const QString &channelId,
                                             const QJsonObject &channel)
{
    if (!m_rooms.contains(channelId))
        return;
    Room &room = m_rooms[channelId];
    const QString previous = room.conversation;
    const QString name = channel.value(QStringLiteral("name")).toString()
                             .left(kMaxDisplayNameChars);
    const QString conversation = forkmesh::office::conversationForChannel(name);
    const QString topic = channel.value(QStringLiteral("topic")).toString()
                              .simplified().left(kMaxTopicChars);
    const bool privateRoom =
        channel.value(QStringLiteral("visibility")).toString() ==
        QStringLiteral("private");
    const bool serverVisible =
        channel.value(QStringLiteral("serverVisible")).toBool();
    const bool privacyChanged = room.privateRoom != privateRoom ||
                                room.serverVisible != serverVisible;
    if (!conversation.isEmpty() && conversation != previous) {
        room.name = name;
        room.conversation = conversation;
        const int index = m_conversations.indexOf(previous);
        if (index >= 0)
            m_conversations[index] = conversation;
        emit conversationRenamed(previous, conversation);
        emit conversationsChanged(m_conversations);
    }
    const bool topicMoved = room.topic != topic;
    room.topic = topic;
    room.privateRoom = privateRoom;
    room.serverVisible = serverVisible;
    room.readOnly = channel.value(QStringLiteral("readOnly")).toBool();
    if (privacyChanged) {
        // ServerNode chooses the application-layer policy from its local
        // private-channel set. Recreate it when visibility changes so opening
        // a channel does not keep sealing and closing one never sends plain.
        discardSender(room);
        ensureReceiver(channelId);
    }
    if (topicMoved)
        emit topicChanged(room.conversation, topic);
}

bool OfficeChannelMirror::deleteChannel(const QString &conversation)
{
    const QString channelId = channelIdForConversation(conversation, true);
    if (channelId.isEmpty())
        return false;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(
        QStringLiteral("/api/chat/channels/%1").arg(channelId), ts,
        forkmesh::office::channelDeleteProof(m_account, channelId, ts));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_network->sendCustomRequest(
        request, QByteArrayLiteral("DELETE"), QByteArrayLiteral("{}"));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, channelId, conversation]() {
                const QByteArray responseBody = reply->readAll();
                const bool failed = reply->error() != QNetworkReply::NoError;
                const QString transportError =
                    failed ? reply->errorString() : QString();
                const bool removed = QJsonDocument::fromJson(responseBody)
                                         .object()
                                         .value(QStringLiteral("removed"))
                                         .toBool();
                reply->deleteLater();
                if (failed || !removed) {
                    reportSettingsFailure(conversation,
                                          QStringLiteral("delete the channel"),
                                          transportError, responseBody);
                    return;
                }
                if (m_rooms.contains(channelId)) {
                    failPending(m_rooms[channelId],
                                QStringLiteral("the channel was deleted"));
                    discardSender(m_rooms[channelId]);
                    m_rooms.remove(channelId);
                }
                m_webhooks.remove(channelId);
                if (m_conversations.removeAll(conversation) > 0)
                    emit conversationsChanged(m_conversations);
                emit channelDeleted(conversation);
            });
    return true;
}

bool OfficeChannelMirror::refreshWebhooks(const QString &conversation)
{
    const QString channelId = channelIdForConversation(conversation, true);
    if (channelId.isEmpty())
        return false;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(
        QStringLiteral("/api/chat/channels/%1/webhooks").arg(channelId), ts,
        forkmesh::office::channelWebhooksProof(m_account, channelId, ts,
                                               QStringLiteral("GET"),
                                               QString()));
    QNetworkReply *reply = m_network->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, channelId, conversation]() {
                const QByteArray responseBody = reply->readAll();
                const bool failed = reply->error() != QNetworkReply::NoError;
                const QString transportError =
                    failed ? reply->errorString() : QString();
                reply->deleteLater();
                if (failed) {
                    reportSettingsFailure(conversation,
                                          QStringLiteral("list the webhooks"),
                                          transportError, responseBody);
                    return;
                }
                applyWebhookList(channelId, conversation,
                                 QJsonDocument::fromJson(responseBody)
                                     .object()
                                     .value(QStringLiteral("webhooks"))
                                     .toArray());
            });
    return true;
}

// The relay stores a webhook token only as a blind index, so a URL this
// process minted is kept beside the listed record until the app exits.
void OfficeChannelMirror::applyWebhookList(const QString &channelId,
                                           const QString &conversation,
                                           const QJsonArray &records)
{
    QHash<QString, QString> knownUrls;
    for (const forkmesh::office::ChannelWebhook &webhook :
         std::as_const(m_webhooks[channelId]))
        if (!webhook.url.isEmpty())
            knownUrls.insert(webhook.id, webhook.url);
    QVector<forkmesh::office::ChannelWebhook> webhooks;
    for (const QJsonValue &value : records) {
        if (webhooks.size() >= kMaxWebhooksPerChannel)
            break;
        const QJsonObject record = value.toObject();
        forkmesh::office::ChannelWebhook webhook;
        webhook.id = record.value(QStringLiteral("id")).toString();
        if (!channelIdPattern().match(webhook.id).hasMatch())
            continue;
        webhook.name = record.value(QStringLiteral("name")).toString()
                           .left(kMaxWebhookNameChars);
        webhook.createdAt =
            qint64(record.value(QStringLiteral("createdAt")).toDouble());
        webhook.lastUsedAt =
            qint64(record.value(QStringLiteral("lastUsedAt")).toDouble());
        webhook.url = knownUrls.value(webhook.id);
        webhooks.append(webhook);
    }
    m_webhooks[channelId] = webhooks;
    emit channelWebhooksChanged(conversation, webhooks);
}

bool OfficeChannelMirror::createWebhook(const QString &conversation,
                                        const QString &name)
{
    const QString channelId = channelIdForConversation(conversation, true);
    const QString wanted = name.simplified().left(kMaxWebhookNameChars);
    if (channelId.isEmpty() || wanted.isEmpty())
        return false;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(
        QStringLiteral("/api/chat/channels/%1/webhooks").arg(channelId), ts,
        forkmesh::office::channelWebhooksProof(m_account, channelId, ts,
                                               QStringLiteral("POST"), wanted));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    const QByteArray body =
        QJsonDocument(QJsonObject{{QStringLiteral("name"), wanted}})
            .toJson(QJsonDocument::Compact);
    QNetworkReply *reply = m_network->post(request, body);
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, channelId, conversation]() {
                const QByteArray responseBody = reply->readAll();
                const bool failed = reply->error() != QNetworkReply::NoError;
                const QString transportError =
                    failed ? reply->errorString() : QString();
                const QJsonObject payload =
                    QJsonDocument::fromJson(responseBody).object();
                reply->deleteLater();
                if (failed || !payload.value(QStringLiteral("ok")).toBool()) {
                    reportSettingsFailure(conversation,
                                          QStringLiteral("add the webhook"),
                                          transportError, responseBody);
                    return;
                }
                rememberWebhookUrl(channelId,
                                   payload.value(QStringLiteral("webhook"))
                                       .toObject()
                                       .value(QStringLiteral("id"))
                                       .toString(),
                                   payload.value(QStringLiteral("url"))
                                       .toString());
                refreshWebhooks(conversation);
            });
    return true;
}

bool OfficeChannelMirror::rotateWebhook(const QString &conversation,
                                        const QString &webhookId)
{
    const QString channelId = channelIdForConversation(conversation, true);
    if (channelId.isEmpty() ||
        !channelIdPattern().match(webhookId).hasMatch())
        return false;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(
        QStringLiteral("/api/chat/channels/%1/webhooks/%2")
            .arg(channelId, webhookId),
        ts,
        forkmesh::office::channelWebhookProof(m_account, channelId, webhookId,
                                              ts, QStringLiteral("POST")));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_network->post(request, QByteArrayLiteral("{}"));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, channelId, webhookId, conversation]() {
                const QByteArray responseBody = reply->readAll();
                const bool failed = reply->error() != QNetworkReply::NoError;
                const QString transportError =
                    failed ? reply->errorString() : QString();
                const QJsonObject payload =
                    QJsonDocument::fromJson(responseBody).object();
                reply->deleteLater();
                if (failed || !payload.value(QStringLiteral("ok")).toBool()) {
                    reportSettingsFailure(
                        conversation, QStringLiteral("issue a new webhook URL"),
                        transportError, responseBody);
                    return;
                }
                rememberWebhookUrl(
                    channelId, webhookId,
                    payload.value(QStringLiteral("url")).toString());
                refreshWebhooks(conversation);
            });
    return true;
}

void OfficeChannelMirror::rememberWebhookUrl(const QString &channelId,
                                             const QString &webhookId,
                                             const QString &url)
{
    if (!channelIdPattern().match(webhookId).hasMatch() || url.isEmpty())
        return;
    QVector<forkmesh::office::ChannelWebhook> &webhooks = m_webhooks[channelId];
    for (forkmesh::office::ChannelWebhook &webhook : webhooks) {
        if (webhook.id != webhookId)
            continue;
        webhook.url = url;
        return;
    }
    forkmesh::office::ChannelWebhook webhook;
    webhook.id = webhookId;
    webhook.url = url;
    webhooks.append(webhook);
}

bool OfficeChannelMirror::deleteWebhook(const QString &conversation,
                                        const QString &webhookId)
{
    const QString channelId = channelIdForConversation(conversation, true);
    if (channelId.isEmpty() ||
        !channelIdPattern().match(webhookId).hasMatch())
        return false;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(
        QStringLiteral("/api/chat/channels/%1/webhooks/%2")
            .arg(channelId, webhookId),
        ts,
        forkmesh::office::channelWebhookProof(m_account, channelId, webhookId,
                                              ts, QStringLiteral("DELETE")));
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/json"));
    QNetworkReply *reply = m_network->sendCustomRequest(
        request, QByteArrayLiteral("DELETE"), QByteArrayLiteral("{}"));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, channelId, webhookId, conversation]() {
                const QByteArray responseBody = reply->readAll();
                const bool failed = reply->error() != QNetworkReply::NoError;
                const QString transportError =
                    failed ? reply->errorString() : QString();
                reply->deleteLater();
                if (failed) {
                    reportSettingsFailure(conversation,
                                          QStringLiteral("remove the webhook"),
                                          transportError, responseBody);
                    return;
                }
                QVector<forkmesh::office::ChannelWebhook> &webhooks =
                    m_webhooks[channelId];
                for (int index = webhooks.size() - 1; index >= 0; --index)
                    if (webhooks[index].id == webhookId)
                        webhooks.remove(index);
                refreshWebhooks(conversation);
            });
    return true;
}

void OfficeChannelMirror::fetchChannels()
{
    if (m_listing)
        return;
    m_lastListMs = QDateTime::currentMSecsSinceEpoch();
    fetchDirectMessages();
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(QStringLiteral("/api/chat/channels"), ts,
                               forkmesh::office::channelListProof(m_account, ts));
    m_listing = true;
    QNetworkReply *reply = m_network->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        m_listing = false;
        if (m_relistPending) {
            m_relistPending = false;
            QTimer::singleShot(0, this, [this] {
                m_lastListMs = 0;
                fetchChannels();
            });
        }
        const QByteArray body = reply->readAll();
        const bool failed = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();
        if (failed)
            return;
        const QJsonArray channels =
            QJsonDocument::fromJson(body).object()
                .value(QStringLiteral("channels")).toArray();
        QStringList conversations;
        QSet<QString> live;
        bool fetchEveryone = false;
        for (const QJsonValue &value : channels) {
            if (conversations.size() >= kMaxChannels)
                break;
            const QJsonObject channel = value.toObject();
            const QString id = channel.value(QStringLiteral("id")).toString();
            if (!channelIdPattern().match(id).hasMatch())
                continue;
            const QString conversation = forkmesh::office::conversationForChannel(
                channel.value(QStringLiteral("name")).toString());
            if (conversation.isEmpty() || conversations.contains(conversation))
                continue;
            if (m_rooms.contains(id) && m_rooms[id].direct)
                continue;
            Room &room = m_rooms[id];
            const QString previousConversation = room.conversation;
            room.id = id;
            room.name = channel.value(QStringLiteral("name")).toString()
                            .left(kMaxDisplayNameChars);
            room.conversation = conversation;
            const QString topic =
                channel.value(QStringLiteral("topic")).toString()
                    .simplified().left(kMaxTopicChars);
            const bool canManage =
                channel.value(QStringLiteral("canManage")).toBool();
            const bool privateRoom =
                channel.value(QStringLiteral("visibility")).toString() ==
                QStringLiteral("private");
            QStringList members;
            if (privateRoom) {
                QSet<QString> seenMembers;
                for (const QJsonValue &memberValue :
                     channel.value(QStringLiteral("members")).toArray()) {
                    const QString member =
                        memberValue.toString().trimmed().toLower()
                            .left(kMaxDisplayNameChars);
                    if (!member.isEmpty() && !seenMembers.contains(member)) {
                        seenMembers.insert(member);
                        members.append(member);
                    }
                }
            }
            const bool everyone =
                !privateRoom &&
                channel.value(QStringLiteral("kind")).toString() ==
                    QLatin1String("everyone") &&
                forkmesh::office::isEveryoneChannelName(room.name);
            const int memberCount =
                everyone ? qBound(0,
                                  channel.value(QStringLiteral("memberCount"))
                                      .toInt(),
                                  kMaxEveryoneMembers)
                         : 0;
            if (everyone)
                members = room.members;
            const bool serverVisible =
                channel.value(QStringLiteral("serverVisible")).toBool();
            const bool privacyChanged = room.privateRoom != privateRoom ||
                                        room.serverVisible != serverVisible;
            const bool membersChanged =
                privacyChanged || room.members != members ||
                room.everyone != everyone || room.memberCount != memberCount;
            const bool topicChangedNow = room.topic != topic;
            room.privateRoom = privateRoom;
            room.serverVisible = serverVisible;
            room.readOnly = channel.value(QStringLiteral("readOnly")).toBool();
            room.canManage = canManage && !everyone;
            room.everyone = everyone;
            room.memberCount = memberCount;
            if (everyone && (room.members.isEmpty() ||
                             memberCount != m_everyoneMembersFetchedCount))
                fetchEveryone = true;
            room.topic = topic;
            room.members = members;
            room.order = int(conversations.size());
            room.wantsSocket = room.order < kMaxLiveRooms || room.watched;
            if (privacyChanged)
                discardSender(room);
            conversations.append(conversation);
            live.insert(id);
            // A rename keeps the room id, so anything filed under the old
            // conversation has to move with it -- including a rename another
            // client made while this one was only listening.
            if (!previousConversation.isEmpty() &&
                previousConversation != conversation)
                emit conversationRenamed(previousConversation, conversation);
            if (membersChanged)
                emit roomMembersChanged(conversation, members);
            if (topicChangedNow)
                emit topicChanged(conversation, topic);
        }
        if (conversations.size() > kMaxLiveRooms && !m_socketBudgetNoted) {
            m_socketBudgetNoted = true;
            emit sendActivity(
                QStringLiteral("Office chat: watching the first %1 of %2 rooms "
                               "live; the rest load when you open or send to "
                               "them.")
                    .arg(kMaxLiveRooms)
                    .arg(conversations.size()));
        }
        const QStringList known = m_rooms.keys();
        for (const QString &id : known) {
            if (m_rooms[id].direct)
                continue;
            if (!live.contains(id)) {
                failPending(m_rooms[id],
                            QStringLiteral("office room is no longer available"));
                discardSender(m_rooms[id]);
                m_rooms.remove(id);
            }
        }
        const bool changed = conversations != m_conversations;
        if (changed) {
            m_conversations = conversations;
            emit conversationsChanged(m_conversations);
        }
        for (const QString &id : std::as_const(live)) {
            fetchHistory(id);
            ensureReceiver(id);
        }
        if (fetchEveryone)
            fetchEveryoneMembers();
    });
}

void OfficeChannelMirror::fetchEveryoneMembers()
{
    if (m_everyoneMembersFetching || !ready())
        return;
    QString channelId;
    int firstOrder = 0;
    int expected = 0;
    for (auto it = m_rooms.constBegin(); it != m_rooms.constEnd(); ++it) {
        if (!it->everyone)
            continue;
        if (channelId.isEmpty() || it->order < firstOrder) {
            channelId = it.key();
            firstOrder = it->order;
        }
        expected = qMax(expected, int(it->memberCount));
    }
    if (channelId.isEmpty())
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url = signedUrl(
        QStringLiteral("/api/chat/channels/%1/members").arg(channelId), ts,
        forkmesh::office::channelMembersProof(m_account, channelId, ts,
                                              QStringLiteral("GET"),
                                              QString()));
    m_everyoneMembersFetching = true;
    QNetworkReply *reply = m_network->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, expected]() {
        m_everyoneMembersFetching = false;
        const QByteArray body = reply->readAll();
        const bool failed = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();
        if (failed)
            return;
        const QJsonObject payload = QJsonDocument::fromJson(body).object();
        if (!payload.value(QStringLiteral("everyone")).toBool())
            return;
        QStringList members;
        QSet<QString> seen;
        for (const QJsonValue &value :
             payload.value(QStringLiteral("members")).toArray()) {
            if (members.size() >= kMaxEveryoneMembers)
                break;
            const QString name = normalizedUsername(
                value.toObject().value(QStringLiteral("username")).toString());
            if (name.isEmpty() || seen.contains(name))
                continue;
            seen.insert(name);
            members.append(name);
        }
        m_everyoneMembersFetchedCount = expected;
        QStringList changed;
        for (auto it = m_rooms.begin(); it != m_rooms.end(); ++it) {
            if (!it->everyone || it->members == members)
                continue;
            it->members = members;
            changed.append(it->conversation);
        }
        for (const QString &conversation : std::as_const(changed))
            emit roomMembersChanged(conversation, members);
    });
}

void OfficeChannelMirror::ensureReceiver(const QString &channelId)
{
    if (!m_active || !m_rooms.contains(channelId))
        return;
    Room &room = m_rooms[channelId];
    if (!room.wantsSocket || room.sender || room.accessFetching ||
        room.reconnectPending)
        return;
    fetchRoomAccess(channelId);
}

void OfficeChannelMirror::scheduleReconnect(const QString &channelId)
{
    if (!m_active || !m_rooms.contains(channelId))
        return;
    Room &room = m_rooms[channelId];
    if (room.reconnectPending || room.sender || room.accessFetching)
        return;
    if (!room.wantsSocket && room.pendingTexts.isEmpty())
        return; // an out-of-budget room only connects to send
    const int shift = qMin(room.reconnectAttempts, 8);
    ++room.reconnectAttempts;
    int delay = int(qMin<qint64>(qint64(kReconnectBaseDelayMs) << shift,
                                 kReconnectMaxDelayMs));
    delay += int(QRandomGenerator::global()->bounded(delay / 4 + 250));
    room.reconnectPending = true;
    QTimer::singleShot(delay, this, [this, channelId] {
        if (!m_rooms.contains(channelId))
            return;
        Room &liveRoom = m_rooms[channelId];
        liveRoom.reconnectPending = false;
        if (!m_active || liveRoom.sender)
            return;
        if (!liveRoom.wantsSocket && liveRoom.pendingTexts.isEmpty())
            return; // an out-of-budget room only connects to send
        fetchRoomAccess(channelId);
    });
}

void OfficeChannelMirror::fetchRoomAccess(const QString &channelId)
{
    if (!m_rooms.contains(channelId))
        return;
    Room &room = m_rooms[channelId];
    if (room.accessFetching || room.sender)
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    const QUrl url =
        room.direct
            ? signedUrl(
                  QStringLiteral("/api/chat/direct-messages/%1/room-access")
                      .arg(channelId),
                  ts,
                  forkmesh::office::directAccessProof(m_account, channelId, ts))
            : signedUrl(
                  QStringLiteral("/api/chat/channels/%1/room-access")
                      .arg(channelId),
                  ts,
                  forkmesh::office::channelAccessProof(m_account, channelId,
                                                       ts));
    room.accessFetching = true;
    if (!room.pendingTexts.isEmpty())
        emit sendActivity(QStringLiteral("Office chat: requesting encrypted room access "
                                         "for %1.")
                              .arg(room.conversation));
    QNetworkReply *reply = m_network->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, channelId]() {
                const QByteArray body = reply->readAll();
                const bool failed = reply->error() != QNetworkReply::NoError;
                const QString networkError = reply->errorString();
                reply->deleteLater();
                if (!m_rooms.contains(channelId))
                    return;
                Room &room = m_rooms[channelId];
                const QJsonObject payload =
                    QJsonDocument::fromJson(body).object();
                if (failed || payload.value(QStringLiteral("room")).toString().isEmpty()) {
                    room.accessFetching = false;
                    failPending(
                        room,
                        failed ? networkError
                               : payload.value(QStringLiteral("error"))
                                     .toString(QStringLiteral("room access denied")));
                    scheduleReconnect(channelId);
                    return;
                }
                // The sender's constructor derives the room key (210k PBKDF2
                // rounds, ~0.2 s each) synchronously; with a dozen channels
                // coming up together that was seconds of frozen GUI at startup
                // and on every reconnect. Derive into the process-wide key
                // cache on a worker first — the constructor then hits it — and
                // keep the room marked fetching so nothing opens it twice.
                const QString roomName = payload.value(QStringLiteral("room")).toString();
                const QString passphrase =
                    payload.value(QStringLiteral("passphrase")).toString();
                QThread *warm = QThread::create([roomName, passphrase] {
                    RoomCrypto::warmKey(roomName, passphrase);
                });
                connect(warm, &QThread::finished, warm, &QObject::deleteLater);
                connect(warm, &QThread::finished, this, [this, channelId, payload] {
                    if (!m_rooms.contains(channelId))
                        return;
                    m_rooms[channelId].accessFetching = false;
                    openSender(channelId, payload);
                });
                warm->start();
            });
}

void OfficeChannelMirror::openSender(const QString &channelId,
                                     const QJsonObject &payload)
{
    if (!m_rooms.contains(channelId))
        return;
    Room &room = m_rooms[channelId];
    const QString roomName = payload.value(QStringLiteral("room")).toString();
    const QString passphrase =
        payload.value(QStringLiteral("passphrase")).toString();
    QUrl endpoint =
        m_apiBase.resolved(QUrl(payload.value(QStringLiteral("webSocketUrl")).toString()));
    if (endpoint.scheme() == QLatin1String("https"))
        endpoint.setScheme(QStringLiteral("wss"));
    else if (endpoint.scheme() == QLatin1String("http"))
        endpoint.setScheme(QStringLiteral("ws"));
    if (roomName.isEmpty() || passphrase.isEmpty() || !endpoint.isValid() ||
        (endpoint.scheme() != QLatin1String("ws") &&
         endpoint.scheme() != QLatin1String("wss"))) {
        failPending(room, QStringLiteral("relay returned invalid room access"));
        scheduleReconnect(channelId);
        return;
    }

    discardSender(room);
    if (room.direct)
        setDirectRoomName(room, roomName);
    auto *sender = new ServerNode(
        m_displayName.isEmpty() ? m_account : m_displayName, m_account, m_account,
        m_selfId, endpoint, roomName, QString(), passphrase, this);
    sender->setChatDevice(m_device);
    sender->setChatAccount(selfDirectAcct());
    if (room.direct)
        sender->setDirectParticipants(room.members);
    sender->setAccountKind(QStringLiteral("user"));
    sender->setDirectRoom(room.direct);
    if (!room.direct) {
        if (room.privateRoom && !room.serverVisible)
            sender->createPrivateChannel(QLatin1Char('#') + room.name);
        else
            sender->addChannel(QLatin1Char('#') + room.name);
    }
    if (m_connectionAuthorizer)
        sender->setConnectionAuthorizer(m_connectionAuthorizer);
    room.sender = sender;
    room.senderConnected = false;

    connect(sender, &ChatBackend::messageArrived, this,
            [this, channelId, sender](ChatMessage message) {
                if (!m_rooms.contains(channelId) ||
                    m_rooms[channelId].sender != sender)
                    return;
                Room &liveRoom = m_rooms[channelId];
                message.conversation = liveRoom.conversation;
                if (liveRoom.seenIds.contains(message.id)) {
                    if (message.recipientCount >= 0)
                        emit messageRecipientsKnown(liveRoom.conversation,
                                                    message.id,
                                                    message.recipientCount,
                                                    message.recipientNames);
                    return;
                }
                rememberSeen(liveRoom, message.id);
                emit messageArrived(message);
            });
    connect(sender, &ServerNode::messageRecipientsKnown, this,
            [this, channelId, sender](const QString &messageId, int recipients,
                                      const QStringList &names) {
                if (!m_rooms.contains(channelId) ||
                    m_rooms[channelId].sender != sender)
                    return;
                emit messageRecipientsKnown(m_rooms[channelId].conversation,
                                            messageId, recipients, names);
            });
    connect(sender, &ServerNode::roomMutationArrived, this,
            [this, channelId, sender](const QJsonObject &plain) {
                if (!m_rooms.contains(channelId) ||
                    m_rooms[channelId].sender != sender)
                    return;
                applyMutation(m_rooms[channelId], plain);
            });
    if (room.direct) {
        connect(sender, &ServerNode::messagesDelivered, this,
                [this, channelId, sender](const QStringList &messageIds) {
                    if (!m_rooms.contains(channelId) ||
                        m_rooms[channelId].sender != sender)
                        return;
                    emit messagesDelivered(m_rooms[channelId].conversation,
                                           messageIds);
                });
        connect(sender, &ServerNode::conversationRead, this,
                [this, channelId, sender] {
                    if (!m_rooms.contains(channelId) ||
                        m_rooms[channelId].sender != sender)
                        return;
                    emit conversationRead(m_rooms[channelId].conversation);
                });
    }
    if (room.direct) {
        connect(sender, &ServerNode::roomSealPeersChanged, this,
                [this, channelId, sender] {
                    if (!m_rooms.contains(channelId) ||
                        m_rooms[channelId].sender != sender)
                        return;
                    emit directRouteChanged(m_rooms[channelId].conversation);
                });
        connect(sender, &ChatBackend::typingChanged, this,
                [this, channelId, sender](const QString &, const QString &peerId,
                                          const QString &peerName, bool active) {
                    if (!m_rooms.contains(channelId) ||
                        m_rooms[channelId].sender != sender)
                        return;
                    emit typingChanged(m_rooms[channelId].conversation, peerId,
                                       peerName, active);
                });
    }
    if (room.direct)
        connect(sender, &ServerNode::serverCopiesKnown, this,
                [this, channelId, sender](int stored) {
                    if (!m_rooms.contains(channelId) ||
                        m_rooms[channelId].sender != sender)
                        return;
                    emit directServerCopiesKnown(
                        m_rooms[channelId].conversation, stored);
                });
    connect(sender, &ServerNode::connectionChanged, this,
            [this, channelId, sender](bool connected) {
                if (!m_rooms.contains(channelId) ||
                    m_rooms[channelId].sender != sender)
                    return;
                Room &liveRoom = m_rooms[channelId];
                liveRoom.senderConnected = connected;
                if (connected) {
                    liveRoom.reconnectAttempts = 0;
                    if (!liveRoom.pendingTexts.isEmpty())
                        emit sendActivity(
                            QStringLiteral("Office chat: encrypted socket ready "
                                           "for %1.")
                                .arg(liveRoom.conversation));
                    flushPending(liveRoom);
                    fetchHistory(channelId);
                    return;
                }
                liveRoom.sender = nullptr;
                sender->shutdown();
                sender->deleteLater();
                scheduleReconnect(channelId);
            });
    connect(sender, &ChatBackend::systemMessage, this,
            [this, channelId](const QString &message) {
                if (message.contains(QStringLiteral("error"), Qt::CaseInsensitive) ||
                    message.contains(QStringLiteral("failed"), Qt::CaseInsensitive) ||
                    message.contains(QStringLiteral("blocked"), Qt::CaseInsensitive))
                    emit sendActivity(QStringLiteral("Office chat: %1").arg(message));
            });

    if (!sender->start()) {
        room.sender = nullptr;
        sender->deleteLater();
        failPending(room, QStringLiteral("could not start encrypted room socket"));
        scheduleReconnect(channelId);
        return;
    }

    QTimer::singleShot(20000, this, [this, channelId, sender] {
        if (!m_rooms.contains(channelId))
            return;
        Room &liveRoom = m_rooms[channelId];
        if (liveRoom.sender != sender || liveRoom.senderConnected)
            return;
        liveRoom.sender = nullptr;
        sender->shutdown();
        sender->deleteLater();
        failPending(liveRoom, QStringLiteral("encrypted room connection timed out"));
        scheduleReconnect(channelId);
    });
}

void OfficeChannelMirror::flushPending(Room &room)
{
    if (!room.sender || !room.senderConnected)
        return;
    while (!room.pendingTexts.isEmpty()) {
        const PendingText pending = room.pendingTexts.dequeue();
        room.meshDelivered.remove(pending.id);
        room.meshInFlight.remove(pending.id);
        if (pending.kind == QLatin1String("reply"))
            room.sender->sendThreadReplyWithId(wireChannel(room), pending.rootId,
                                               pending.text, pending.id);
        else if (pending.kind == QLatin1String("reaction"))
            room.sender->sendRoomReaction(wireChannel(room), pending.target,
                                          pending.emoji, pending.added, pending.id);
        else if (pending.kind == QLatin1String("edit"))
            room.sender->sendRoomEdit(wireChannel(room), pending.target,
                                      pending.text, pending.id);
        else if (pending.kind == QLatin1String("delete"))
            room.sender->sendRoomDelete(wireChannel(room), pending.target, pending.id);
        else if (pending.fileData.isEmpty())
            room.sender->sendChatWithId(wireChannel(room), pending.text, pending.id);
        else
            room.sender->sendFileWithId(wireChannel(room), pending.fileName,
                                        pending.fileMime, pending.fileData,
                                        pending.id);
        emit sendActivity(QStringLiteral("Office chat: sent an encrypted message "
                                         "to %1.")
                              .arg(room.conversation));
    }
}

void OfficeChannelMirror::failPending(Room &room, const QString &reason)
{
    if (room.pendingTexts.isEmpty())
        return;
    const QString failure =
        reason.trimmed().isEmpty() ? QStringLiteral("send failed") : reason.trimmed();
    QQueue<PendingText> kept;
    int failed = 0;
    while (!room.pendingTexts.isEmpty()) {
        const PendingText pending = room.pendingTexts.dequeue();
        if (room.meshDelivered.contains(pending.id) ||
            room.meshInFlight.contains(pending.id) ||
            pending.kind == QLatin1String("reaction") ||
            pending.kind == QLatin1String("edit") ||
            pending.kind == QLatin1String("delete")) {
            kept.enqueue(pending);
            continue;
        }
        ++failed;
        emit messageSendFailed(room.conversation, pending.id, pending.text,
                               failure);
    }
    room.pendingTexts = kept;
    if (failed == 0)
        return;
    emit sendActivity(QStringLiteral("Office chat: send to %1 failed (%2).")
                          .arg(room.conversation, failure));
}

void OfficeChannelMirror::rememberSeen(Room &room, const QString &messageId)
{
    if (messageId.isEmpty() || room.seenIds.contains(messageId))
        return;
    room.seenIds.insert(messageId);
    room.seenOrder.enqueue(messageId);
    while (room.seenOrder.size() > kSeenIdsPerRoom)
        room.seenIds.remove(room.seenOrder.dequeue());
}

void OfficeChannelMirror::discardSender(Room &room)
{
    ServerNode *sender = room.sender.data();
    room.sender = nullptr;
    room.senderConnected = false;
    if (!sender)
        return;
    sender->shutdown();
    sender->deleteLater();
}

void OfficeChannelMirror::fetchHistory(const QString &channelId)
{
    if (!m_rooms.contains(channelId))
        return;
    Room &room = m_rooms[channelId];
    if (room.fetching || room.direct)
        return;
    const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
    QUrl url = signedUrl(
        QStringLiteral("/api/chat/channels/%1/history").arg(channelId), ts,
        forkmesh::office::channelHistoryProof(m_account, channelId, ts));
    QUrlQuery query(url);
    query.addQueryItem(QStringLiteral("since"), QString::number(room.lastTs));
    url.setQuery(query);
    room.fetching = true;
    QNetworkReply *reply = m_network->get(QNetworkRequest(url));
    connect(reply, &QNetworkReply::finished, this, [this, reply, channelId]() {
        const QByteArray body = reply->readAll();
        const bool failed = reply->error() != QNetworkReply::NoError;
        reply->deleteLater();
        if (!m_rooms.contains(channelId))
            return;
        Room &room = m_rooms[channelId];
        if (failed) {
            room.fetching = false;
            return;
        }
        const QJsonObject payload = QJsonDocument::fromJson(body).object();
        const QString roomName = payload.value(QStringLiteral("room")).toString();
        const QString passphrase =
            payload.value(QStringLiteral("passphrase")).toString();
        if (!roomName.isEmpty() && !passphrase.isEmpty() &&
            (roomName != room.room || !room.crypto.isValid())) {
            QThread *warm = QThread::create([roomName, passphrase] {
                RoomCrypto::warmKey(roomName, passphrase);
            });
            connect(warm, &QThread::finished, warm, &QObject::deleteLater);
            connect(warm, &QThread::finished, this, [this, channelId, payload] {
                if (!m_rooms.contains(channelId))
                    return;
                m_rooms[channelId].fetching = false;
                applyHistory(m_rooms[channelId], payload);
            });
            warm->start();
            return;
        }
        room.fetching = false;
        applyHistory(room, payload);
    });
}

void OfficeChannelMirror::applyHistory(Room &room, const QJsonObject &payload)
{
    const QString roomName = payload.value(QStringLiteral("room")).toString();
    const QString passphrase =
        payload.value(QStringLiteral("passphrase")).toString();
    if (roomName.isEmpty() || passphrase.isEmpty())
        return;
    if (roomName != room.room || !room.crypto.isValid()) {
        room.room = roomName;
        room.crypto = RoomCrypto(roomName, passphrase);
        room.seal = RoomSeal(roomName, roomName, RoomSeal::Policy::Open, selfDirectAcct(),
                             m_selfId, m_device);
        if (!room.crypto.isValid())
            return;
    }

    const QJsonArray messages =
        payload.value(QStringLiteral("messages")).toArray();
    QList<QJsonObject> mutations;
    for (const QJsonValue &value : messages) {
        const QJsonObject entry = value.toObject();
        const qint64 ts = qint64(entry.value(QStringLiteral("ts")).toDouble());
        room.lastTs = qMax(room.lastTs, ts);
        const QJsonObject envelope =
            QJsonDocument::fromJson(
                entry.value(QStringLiteral("body")).toString().toUtf8())
                .object();
        if (envelope.isEmpty())
            continue;
        QJsonObject plain = room.crypto.decryptObject(envelope);
        if (RoomSeal::isLegacyFrame(plain))
            continue;
        if (RoomSeal::isKeyFrame(plain)) {
            room.seal.acceptKeyFrame(plain);
            continue;
        }
        if (RoomSeal::isSealedFrame(plain)) {
            const RoomSeal::Opened opened = room.seal.open(plain, true);
            if (opened.status != RoomSeal::Status::Opened)
                continue;
            plain = opened.plain;
            if (!plain.contains(QStringLiteral("senderId")))
                plain.insert(QStringLiteral("senderId"), opened.senderNode);
            RoomSeal::useSignedTime(plain, opened);
        } else if (room.privateRoom && !room.serverVisible) {
            // The history endpoint is controlled by the relay too. Apply the
            // same fail-closed policy as the live socket for private rooms.
            continue;
        }
        const QString type = plain.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("reaction") || type == QLatin1String("edit") ||
            type == QLatin1String("delete")) {
            mutations.append(plain);
            continue;
        }
        ChatMessage message;
        if (!forkmesh::office::chatMessageFromPlain(plain, room.conversation,
                                                    m_selfId, &message))
            continue;
        message.recipientCount =
            forkmesh::recipients::countFromEnvelope(envelope);
        message.recipientNames =
            forkmesh::recipients::namesFromEnvelope(envelope);
        if (room.seenIds.contains(message.id)) {
            if (message.recipientCount >= 0)
                emit messageRecipientsKnown(room.conversation, message.id,
                                            message.recipientCount,
                                            message.recipientNames);
            continue;
        }
        rememberSeen(room, message.id);
        emit messageArrived(message);
    }
    const QString roomId = room.id;
    for (const QJsonObject &plain : std::as_const(mutations)) {
        if (!m_rooms.contains(roomId))
            return;
        applyMutation(m_rooms[roomId], plain);
    }
}

void OfficeChannelMirror::applyMutation(Room &room, const QJsonObject &plain)
{
    forkmesh::office::RoomMutation mutation;
    if (!forkmesh::office::roomMutationFromPlain(plain, &mutation) ||
        room.seenIds.contains(mutation.id))
        return;
    rememberSeen(room, mutation.id);
    if (mutation.type == QLatin1String("reaction"))
        emit reactionChanged(room.conversation, mutation.target, mutation.emoji,
                             mutation.reactorName, mutation.added);
    else if (mutation.type == QLatin1String("edit"))
        emit messageEdited(room.conversation, mutation.target,
                           mutation.senderId, mutation.text);
    else
        emit messageDeleted(room.conversation, mutation.target,
                            mutation.senderId);
}
