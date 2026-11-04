#include "ReticulumMessenger.h"

#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QVariantList>
#include <QtConcurrent/QtConcurrentRun>

#include <algorithm>

namespace forkmesh::reticulum {

namespace {

constexpr int kRequestWindow = 64;
constexpr int kMaxLinks = 64;
constexpr int kMaxIncomingResources = 8;
constexpr int kMaxStoredAttachmentBytes = 4 * 1024 * 1024;
const QString kDeliveryAspect = QStringLiteral("lxmf.delivery");

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

QByteArray signalling(int mtu)
{
    const quint32 value = quint32(mtu & 0x1FFFFF) + (quint32((1 << 5) & 0xE0) << 16);
    QByteArray out;
    out.append(char((value >> 16) & 0xff));
    out.append(char((value >> 8) & 0xff));
    out.append(char(value & 0xff));
    return out;
}

QString shortName(const QByteArray &hash)
{
    const QString hex = hexHash(hash);
    return QStringLiteral("<%1…%2>").arg(hex.left(6), hex.right(4));
}

}

QJsonObject ChatMessage::toJson() const
{
    QJsonObject object{{QStringLiteral("id"), id},
                       {QStringLiteral("peer"), hexHash(peer)},
                       {QStringLiteral("outgoing"), outgoing},
                       {QStringLiteral("content"), content},
                       {QStringLiteral("timestamp"), timestamp},
                       {QStringLiteral("state"), int(state)},
                       {QStringLiteral("verified"), verified},
                       {QStringLiteral("read"), read},
                       {QStringLiteral("attempts"), attempts}};
    if (!title.isEmpty())
        object.insert(QStringLiteral("title"), title);
    if (!method.isEmpty())
        object.insert(QStringLiteral("method"), method);
    if (!note.isEmpty())
        object.insert(QStringLiteral("note"), note);
    if (!attachments.isEmpty()) {
        QJsonArray files;
        qint64 total = 0;
        for (const LxmfAttachment &attachment : attachments) {
            QJsonObject file{{QStringLiteral("name"), attachment.name},
                             {QStringLiteral("size"), qint64(attachment.data.size())}};
            total += attachment.data.size();
            if (total <= kMaxStoredAttachmentBytes)
                file.insert(QStringLiteral("data"), QString::fromLatin1(attachment.data.toBase64()));
            files.append(file);
        }
        object.insert(QStringLiteral("attachments"), files);
    }
    return object;
}

std::optional<ChatMessage> ChatMessage::fromJson(const QJsonObject &object)
{
    ChatMessage message;
    message.id = object.value(QStringLiteral("id")).toString();
    message.peer = QByteArray::fromHex(object.value(QStringLiteral("peer")).toString().toLatin1());
    if (message.id.size() != 64 || message.peer.size() != kHashLength)
        return std::nullopt;
    message.outgoing = object.value(QStringLiteral("outgoing")).toBool();
    message.title = object.value(QStringLiteral("title")).toString();
    message.content = object.value(QStringLiteral("content")).toString();
    message.timestamp = object.value(QStringLiteral("timestamp")).toDouble();
    message.state = MessageState(qBound(0, object.value(QStringLiteral("state")).toInt(), int(MessageState::Received)));
    message.method = object.value(QStringLiteral("method")).toString();
    message.note = object.value(QStringLiteral("note")).toString();
    message.verified = object.value(QStringLiteral("verified")).toBool();
    message.read = object.value(QStringLiteral("read")).toBool(true);
    message.attempts = object.value(QStringLiteral("attempts")).toInt();
    for (const QJsonValue &value : object.value(QStringLiteral("attachments")).toArray()) {
        const QJsonObject file = value.toObject();
        message.attachments.append({file.value(QStringLiteral("name")).toString(),
                                    QByteArray::fromBase64(file.value(QStringLiteral("data")).toString().toLatin1())});
    }
    return message;
}

QString messageStateLabel(MessageState state)
{
    switch (state) {
    case MessageState::Queued:
        return QStringLiteral("Waiting to send");
    case MessageState::Stamping:
        return QStringLiteral("Computing proof of work");
    case MessageState::FindingPath:
        return QStringLiteral("Finding a path");
    case MessageState::Linking:
        return QStringLiteral("Opening a link");
    case MessageState::Sending:
        return QStringLiteral("Sending");
    case MessageState::Sent:
        return QStringLiteral("Sent");
    case MessageState::Delivered:
        return QStringLiteral("Delivered");
    case MessageState::Failed:
        return QStringLiteral("Not sent");
    case MessageState::Received:
        return QStringLiteral("Received");
    }
    return {};
}

Messenger::Messenger(Node *node, QObject *parent)
    : QObject(parent)
    , m_node(node)
    , m_timer(new QTimer(this))
    , m_saveTimer(new QTimer(this))
    , m_changeTimer(new QTimer(this))
{
    m_node->addLocalDestination(kDeliveryAspect, [this] { return lxmfAnnounceData(m_displayName, 0); });
    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(1500);
    connect(m_saveTimer, &QTimer::timeout, this, [this] {
        if (m_save)
            m_save(saveJson());
    });
    m_changeTimer->setSingleShot(true);
    m_changeTimer->setInterval(100);
    connect(m_changeTimer, &QTimer::timeout, this, [this] {
        if (m_changed)
            m_changed();
    });
}

Messenger::~Messenger()
{
    for (const auto &cancel : std::as_const(m_stampJobs))
        cancel->store(true);
    if (m_saveTimer->isActive() && m_save)
        m_save(saveJson());
    m_changed = nullptr;
    m_incoming = nullptr;
}

QByteArray Messenger::address() const
{
    return m_node->localDestinationHash(kDeliveryAspect);
}

QByteArray Messenger::signingPrivate() const
{
    return m_node->identity().privateKey().mid(32);
}

void Messenger::setDisplayName(const QString &name)
{
    m_displayName = name.trimmed().left(64);
}

void Messenger::load(const QString &json)
{
    const QJsonObject root = QJsonDocument::fromJson(json.toUtf8()).object();
    m_messages.clear();
    m_seenIds.clear();
    for (const QJsonValue &value : root.value(QStringLiteral("messages")).toArray()) {
        std::optional<ChatMessage> message = ChatMessage::fromJson(value.toObject());
        if (!message || m_seenIds.contains(message->id))
            continue;
        if (message->outgoing && int(message->state) < int(MessageState::Sent)) {
            message->state = MessageState::Failed;
            message->note = QStringLiteral("Not sent before ForkMesh closed. Retry to send it.");
        }
        m_seenIds.insert(message->id);
        m_messages.append(*message);
    }
    const QJsonObject names = root.value(QStringLiteral("names")).toObject();
    for (auto it = names.begin(); it != names.end(); ++it) {
        const QByteArray hash = QByteArray::fromHex(it.key().toLatin1());
        if (hash.size() == kHashLength)
            m_names.insert(hash, it.value().toString().left(64));
    }
    notifyChanged();
}

QString Messenger::saveJson() const
{
    QJsonArray messages;
    const int start = qMax(0, int(m_messages.size()) - kMaxMessages);
    for (int i = start; i < m_messages.size(); ++i)
        messages.append(m_messages.at(i).toJson());
    QJsonObject names;
    for (auto it = m_names.cbegin(); it != m_names.cend(); ++it)
        names.insert(hexHash(it.key()), it.value());
    return QString::fromUtf8(QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                                       {QStringLiteral("messages"), messages},
                                                       {QStringLiteral("names"), names}})
                                 .toJson(QJsonDocument::Compact));
}

ChatMessage *Messenger::find(const QString &id)
{
    for (ChatMessage &message : m_messages) {
        if (message.id == id)
            return &message;
    }
    return nullptr;
}

std::optional<ChatMessage> Messenger::message(const QString &id) const
{
    for (const ChatMessage &message : m_messages) {
        if (message.id == id)
            return message;
    }
    return std::nullopt;
}

QString Messenger::send(const QByteArray &peer, const QString &content, const QString &title)
{
    if (peer.size() != kHashLength || content.trimmed().isEmpty() || content.toUtf8().size() > kMaxContentBytes ||
        !m_node->identity().hasPrivateKey() || peer == address())
        return {};
    ChatMessage message;
    message.peer = peer;
    message.outgoing = true;
    message.title = title.left(256);
    message.content = content;
    message.timestamp = double(nowMs()) / 1000.0;
    message.id = hexHash(lxmfMessageId(peer, address(), lxmfPayload(message.timestamp, message.title, message.content)));
    if (m_seenIds.contains(message.id))
        return {};
    m_seenIds.insert(message.id);
    m_messages.append(message);
    const QString name = peerName(peer);
    if (!name.startsWith(QLatin1Char('<')))
        m_names.insert(peer, name);
    scheduleSave();
    const QString id = message.id;
    if (nowMs() - m_lastOwnAnnounce > 30 * 60 * 1000 && m_node->onlineCount() > 0 &&
        m_node->announceDestination(address())) {
        m_lastOwnAnnounce = nowMs();
        QTimer::singleShot(1500, this, [this, id] { process(id); });
    } else {
        process(id);
    }
    notifyChanged();
    return id;
}

void Messenger::retry(const QString &id)
{
    ChatMessage *message = find(id);
    if (!message || !message->outgoing || message->state == MessageState::Delivered)
        return;
    message->attempts = 0;
    message->note.clear();
    setState(*message, MessageState::Queued);
    process(id);
}

void Messenger::forget(const QString &id)
{
    if (auto cancel = m_stampJobs.take(id))
        cancel->store(true);
    m_messages.erase(std::remove_if(m_messages.begin(), m_messages.end(),
                                    [&id](const ChatMessage &message) { return message.id == id; }),
                     m_messages.end());
    scheduleSave();
    notifyChanged();
}

void Messenger::clearConversation(const QByteArray &peer)
{
    m_messages.erase(std::remove_if(m_messages.begin(), m_messages.end(),
                                    [&peer](const ChatMessage &message) { return message.peer == peer; }),
                     m_messages.end());
    scheduleSave();
    notifyChanged();
}

void Messenger::markRead(const QByteArray &peer)
{
    bool changed = false;
    for (ChatMessage &message : m_messages) {
        if (message.peer == peer && !message.read) {
            message.read = true;
            changed = true;
        }
    }
    if (changed) {
        scheduleSave();
        notifyChanged();
    }
}

QList<ChatMessage> Messenger::messages(const QByteArray &peer) const
{
    QList<ChatMessage> out;
    for (const ChatMessage &message : m_messages) {
        if (message.peer == peer)
            out.append(message);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const ChatMessage &a, const ChatMessage &b) { return a.timestamp < b.timestamp; });
    return out;
}

QList<Conversation> Messenger::conversations() const
{
    QHash<QByteArray, Conversation> byPeer;
    for (const ChatMessage &message : m_messages) {
        Conversation &conversation = byPeer[message.peer];
        conversation.peer = message.peer;
        ++conversation.count;
        if (!message.read)
            ++conversation.unread;
        if (conversation.count == 1 || message.timestamp >= conversation.last.timestamp)
            conversation.last = message;
    }
    QList<Conversation> out;
    for (Conversation conversation : std::as_const(byPeer)) {
        conversation.name = peerName(conversation.peer);
        out.append(conversation);
    }
    std::sort(out.begin(), out.end(),
              [](const Conversation &a, const Conversation &b) { return a.last.timestamp > b.last.timestamp; });
    return out;
}

int Messenger::unreadCount() const
{
    return int(std::count_if(m_messages.cbegin(), m_messages.cend(), [](const ChatMessage &m) { return !m.read; }));
}

QString Messenger::peerName(const QByteArray &peer) const
{
    const auto found = m_node->destinations().constFind(peer);
    if (found != m_node->destinations().cend() && !found->displayName.isEmpty())
        return found->displayName;
    const QString stored = m_names.value(peer);
    return stored.isEmpty() ? shortName(peer) : stored;
}

int Messenger::activeLinks() const
{
    return int(std::count_if(m_links.cbegin(), m_links.cend(), [](const Link &link) { return link.active; }));
}

int Messenger::peerStampCost(const QByteArray &peer) const
{
    const auto found = m_node->destinations().constFind(peer);
    if (found == m_node->destinations().cend())
        return 0;
    return lxmfStampCostFromAppData(found->appData);
}

void Messenger::destinationAnnounced(const QByteArray &hash)
{
    const auto found = m_node->destinations().constFind(hash);
    if (found != m_node->destinations().cend() && found->kind == QStringLiteral("peer") && !found->displayName.isEmpty() &&
        std::any_of(m_messages.cbegin(), m_messages.cend(), [&hash](const ChatMessage &m) { return m.peer == hash; })) {
        m_names.insert(hash, found->displayName);
        scheduleSave();
    }
    for (const ChatMessage &message : std::as_const(m_messages)) {
        if (message.peer == hash && message.state == MessageState::FindingPath) {
            const QString id = message.id;
            QTimer::singleShot(0, this, [this, id] { process(id); });
        }
    }
}

QByteArray Messenger::packedFor(ChatMessage &message)
{
    return lxmfPack(m_node->identity(), address(), message.peer, message.timestamp, message.title, message.content,
                    m_stamps.value(message.id));
}

void Messenger::setState(ChatMessage &message, MessageState state, const QString &note)
{
    message.state = state;
    if (!note.isNull())
        message.note = note;
    scheduleSave();
    notifyChanged();
    updateTicking();
}

void Messenger::fail(const QString &id, const QString &note)
{
    if (ChatMessage *message = find(id)) {
        if (message->state == MessageState::Delivered)
            return;
        setState(*message, MessageState::Failed, note);
        m_node->addEvent(QStringLiteral("message"),
                         QStringLiteral("A message to %1 was not sent: %2").arg(peerName(message->peer), note),
                         message->peer);
    }
}

void Messenger::process(const QString &id)
{
    ChatMessage *message = find(id);
    if (!message || !message->outgoing)
        return;
    switch (message->state) {
    case MessageState::Queued:
    case MessageState::FindingPath:
    case MessageState::Sent:
        break;
    default:
        return;
    }
    if (m_node->onlineCount() == 0) {
        setState(*message, MessageState::Queued, QStringLiteral("Waiting for an interface to come online"));
        return;
    }
    const std::optional<Identity> peer = m_node->identityFor(message->peer);
    if (!peer) {
        if (message->state != MessageState::FindingPath) {
            m_node->requestPath(message->peer);
            m_waitingSince.insert(id, nowMs());
            setState(*message, MessageState::FindingPath, QString(""));
        }
        return;
    }
    const int cost = peerStampCost(message->peer);
    if (cost > 0 && !m_stamps.contains(id)) {
        if (cost > kMaxStampCost) {
            fail(id, QStringLiteral("The recipient asks for more proof of work (%1 bits) than ForkMesh computes").arg(cost));
            return;
        }
        auto cancel = std::make_shared<std::atomic_bool>(false);
        m_stampJobs.insert(id, cancel);
        const QByteArray messageId = QByteArray::fromHex(id.toLatin1());
        auto *watcher = new QFutureWatcher<QByteArray>(this);
        connect(watcher, &QFutureWatcher<QByteArray>::finished, this, [this, watcher, id, cancel] {
            const QByteArray stamp = watcher->result();
            watcher->deleteLater();
            m_stampJobs.remove(id);
            if (cancel->load())
                return;
            if (stamp.isEmpty()) {
                fail(id, QStringLiteral("Could not compute the proof of work"));
                return;
            }
            m_stamps.insert(id, stamp);
            if (ChatMessage *current = find(id)) {
                setState(*current, MessageState::Queued, QString(""));
                process(id);
            }
        });
        watcher->setFuture(QtConcurrent::run([messageId, cost, cancel] {
            return generateStamp(messageId, cost, cancel.get());
        }));
        setState(*message, MessageState::Stamping, QStringLiteral("The recipient asks for %1 bits of proof of work").arg(cost));
        return;
    }
    const QByteArray packed = packedFor(*message);
    const int contentSize = int(packed.size()) - (2 * kHashLength + kSignatureLength) - 16;
    if (contentSize <= kLxmfOpportunisticMaxContent && message->attempts < 2) {
        deliverOpportunistic(*message, packed, *peer);
        return;
    }
    Link *link = linkTo(message->peer);
    if (!link)
        link = openLink(message->peer);
    if (!link) {
        fail(id, QStringLiteral("Could not open a link"));
        return;
    }
    if (link->active) {
        deliverOverLink(*message, *link);
    } else {
        if (!link->waiting.contains(id))
            link->waiting.append(id);
        m_waitingSince.insert(id, nowMs());
        setState(*message, MessageState::Linking, QString(""));
    }
}

void Messenger::deliverOpportunistic(ChatMessage &message, const QByteArray &packed, const Identity &peer)
{
    const auto record = m_node->destinations().constFind(message.peer);
    const QByteArray ratchet = record != m_node->destinations().cend() ? record->ratchetKey : QByteArray();
    Packet packet;
    packet.packetType = PacketData;
    packet.destinationType = DestinationSingle;
    packet.destinationHash = message.peer;
    packet.data = peer.encrypt(packed.mid(kHashLength), ratchet);
    if (packet.data.isEmpty()) {
        fail(message.id, QStringLiteral("Could not encrypt the message"));
        return;
    }
    const QByteArray raw = m_node->sendPacket(packet);
    const QByteArray hash = packetHash(raw);
    m_receipts.insert(hash.left(kHashLength), {message.id, hash, peer.signingPublicKey(), nowMs()});
    ++message.attempts;
    message.method = QStringLiteral("opportunistic");
    m_sentAt.insert(message.id, nowMs());
    setState(message, MessageState::Sent, QStringLiteral("Waiting for delivery confirmation"));
}

void Messenger::deliverOverLink(ChatMessage &message, Link &link)
{
    const QByteArray packed = packedFor(message);
    const int contentSize = int(packed.size()) - (2 * kHashLength + kSignatureLength) - 16;
    ++message.attempts;
    if (contentSize <= kLxmfLinkPacketMaxContent) {
        const QByteArray raw = linkPacket(link, PacketData, ContextNone, packed);
        const QByteArray hash = packetHash(raw);
        m_receipts.insert(hash.left(kHashLength), {message.id, hash, link.peerSigningPublic, nowMs()});
        message.method = QStringLiteral("link");
        m_sentAt.insert(message.id, nowMs());
        setState(message, MessageState::Sent, QStringLiteral("Waiting for delivery confirmation"));
        return;
    }
    if (packed.size() > kResourceMaxSize) {
        fail(message.id, QStringLiteral("The message is too large"));
        return;
    }
    if (std::any_of(m_outgoingResources.cbegin(), m_outgoingResources.cend(),
                    [&link](const OutgoingResource &r) { return r.linkId == link.id; })) {
        --message.attempts;
        if (!link.waiting.contains(message.id))
            link.waiting.append(message.id);
        setState(message, MessageState::Sending, QStringLiteral("Waiting for the previous transfer"));
        return;
    }
    OutgoingResource resource;
    resource.linkId = link.id;
    resource.messageId = message.id;
    resource.dataSize = packed.size();
    const QByteArray blob = tokenEncrypt(link.key, randomBytes(kResourceRandomLength) + packed);
    resource.transferSize = blob.size();
    const int sdu = link.mtu - kHeaderMaxSize - 1;
    for (int offset = 0; offset < blob.size(); offset += sdu)
        resource.parts.append(blob.mid(offset, sdu));
    for (int attempt = 0; attempt < 8; ++attempt) {
        resource.randomHash = randomBytes(kResourceRandomLength);
        resource.mapHashes.clear();
        QSet<QByteArray> unique;
        for (const QByteArray &part : std::as_const(resource.parts)) {
            const QByteArray mapHash = resourceMapHash(part, resource.randomHash);
            unique.insert(mapHash);
            resource.mapHashes.append(mapHash);
        }
        if (unique.size() == resource.mapHashes.size())
            break;
    }
    resource.hash = fullHash(packed + resource.randomHash);
    resource.expectedProof = fullHash(packed + resource.hash);
    resource.lastActivity = nowMs();
    message.method = QStringLiteral("resource");
    message.progress = 0;
    const QByteArray key = resource.hash;
    m_outgoingResources.insert(key, resource);
    advertiseResource(m_outgoingResources[key]);
    setState(message, MessageState::Sending, QStringLiteral("Offering %1 bytes").arg(packed.size()));
}

void Messenger::advertiseResource(OutgoingResource &resource)
{
    auto link = m_links.find(resource.linkId);
    if (link == m_links.end())
        return;
    ResourceAdvertisement advertisement;
    advertisement.transferSize = resource.transferSize;
    advertisement.dataSize = resource.dataSize;
    advertisement.parts = int(resource.parts.size());
    advertisement.hash = resource.hash;
    advertisement.randomHash = resource.randomHash;
    advertisement.originalHash = resource.hash;
    advertisement.flags = 0x01;
    advertisement.hashmap = resource.mapHashes.join();
    linkPacket(*link, PacketData, ContextResourceAdvertisement, packResourceAdvertisement(advertisement, 0));
    ++resource.advertisements;
    resource.lastActivity = nowMs();
}

void Messenger::startNextOnLink(const QByteArray &linkId)
{
    auto link = m_links.find(linkId);
    if (link == m_links.end() || !link->active)
        return;
    const QStringList waiting = link->waiting;
    link->waiting.clear();
    for (const QString &id : waiting) {
        ChatMessage *message = find(id);
        auto current = m_links.find(linkId);
        if (!message || current == m_links.end())
            continue;
        if (message->state == MessageState::Sending || message->state == MessageState::Linking)
            deliverOverLink(*message, *current);
    }
}

Messenger::Link *Messenger::linkTo(const QByteArray &peer)
{
    for (Link &link : m_links) {
        if (link.initiator && link.peer == peer)
            return &link;
    }
    return nullptr;
}

Messenger::Link *Messenger::openLink(const QByteArray &peer)
{
    const std::optional<Identity> identity = m_node->identityFor(peer);
    if (!identity || m_links.size() >= kMaxLinks)
        return nullptr;
    const Identity ephemeral = Identity::generate();
    Link link;
    link.initiator = true;
    link.peer = peer;
    link.ownEncryptionPrivate = ephemeral.privateKey().left(32);
    link.ownEncryptionPublic = ephemeral.encryptionPublicKey();
    link.ownSigningPrivate = ephemeral.privateKey().mid(32);
    link.peerSigningPublic = identity->signingPublicKey();
    link.requestedAt = nowMs();
    link.interfaceId = m_node->destinations().value(peer).interfaceId;
    Packet packet;
    packet.packetType = PacketLinkRequest;
    packet.destinationType = DestinationSingle;
    packet.destinationHash = peer;
    packet.data = link.ownEncryptionPublic + ephemeral.signingPublicKey();
    const QByteArray raw = m_node->sendPacket(packet);
    link.id = packetHash(raw).left(kHashLength);
    m_links.insert(link.id, link);
    m_node->addEvent(QStringLiteral("link"), QStringLiteral("Opening a link to %1").arg(peerName(peer)), peer);
    updateTicking();
    return &m_links[link.id];
}

QByteArray Messenger::linkPacket(Link &link, int type, quint8 context, const QByteArray &plaintext, bool encrypt)
{
    Packet packet;
    packet.packetType = type;
    packet.destinationType = DestinationLink;
    packet.destinationHash = link.id;
    packet.context = context;
    packet.data = encrypt ? tokenEncrypt(link.key, plaintext) : plaintext;
    const QByteArray raw = packPacket(packet);
    m_node->sendRaw(raw, link.interfaceId);
    link.lastOutbound = nowMs();
    return raw;
}

void Messenger::closeLink(const QByteArray &linkId, bool notifyPeer)
{
    auto link = m_links.find(linkId);
    if (link == m_links.end())
        return;
    if (notifyPeer && !link->key.isEmpty())
        linkPacket(*link, PacketData, ContextLinkClose, link->id);
    const QStringList waiting = link->waiting;
    const QByteArray peer = link->peer;
    m_links.erase(link);
    for (auto it = m_incomingResources.begin(); it != m_incomingResources.end();) {
        if (it->linkId == linkId)
            it = m_incomingResources.erase(it);
        else
            ++it;
    }
    for (auto it = m_outgoingResources.begin(); it != m_outgoingResources.end();) {
        if (it->linkId == linkId) {
            const QString id = it->messageId;
            it = m_outgoingResources.erase(it);
            fail(id, QStringLiteral("The link closed before the message arrived"));
        } else {
            ++it;
        }
    }
    for (const QString &id : waiting)
        fail(id, QStringLiteral("The link closed"));
    notifyChanged();
}

void Messenger::handlePacket(const QString &interfaceId, const Packet &packet, const QByteArray &raw, int)
{
    if (!m_node->identity().hasPrivateKey())
        return;
    if (packet.packetType == PacketProof) {
        if (packet.destinationType == DestinationLink && packet.context == ContextLinkRequestProof) {
            auto link = m_links.find(packet.destinationHash);
            if (link == m_links.end() || !link->initiator || link->active)
                return;
            if (packet.data.size() != 96 && packet.data.size() != 99)
                return;
            QByteArray signalBytes;
            int mtu = kMtu;
            if (packet.data.size() == 99) {
                signalBytes = packet.data.mid(96, 3);
                if ((quint8(signalBytes.at(0)) >> 5) != 1)
                    return;
                mtu = int(((quint32(quint8(signalBytes.at(0))) << 16) | (quint32(quint8(signalBytes.at(1))) << 8) |
                           quint8(signalBytes.at(2))) & 0x1FFFFF);
            }
            const QByteArray peerPublic = packet.data.mid(64, 32);
            if (!ed25519Verify(link->peerSigningPublic, packet.data.left(64),
                               link->id + peerPublic + link->peerSigningPublic + signalBytes))
                return;
            link->key = hkdf(64, x25519Exchange(link->ownEncryptionPrivate, peerPublic), link->id);
            if (link->key.isEmpty())
                return;
            link->active = true;
            link->mtu = qBound(kMtu, mtu, kStreamHardwareMtu);
            link->interfaceId = interfaceId;
            link->lastInbound = nowMs();
            const double rtt = double(nowMs() - link->requestedAt) / 1000.0;
            linkPacket(*link, PacketData, ContextLinkRtt, MsgPackWriter().real(rtt).bytes());
            m_node->addEvent(QStringLiteral("link"),
                             QStringLiteral("Link to %1 is up (%2 ms)").arg(peerName(link->peer)).arg(qint64(rtt * 1000)),
                             link->peer);
            const QStringList waiting = link->waiting;
            link->waiting.clear();
            const QByteArray id = link->id;
            for (const QString &messageId : waiting) {
                ChatMessage *message = find(messageId);
                auto current = m_links.find(id);
                if (message && current != m_links.end())
                    deliverOverLink(*message, *current);
            }
            return;
        }
        if (packet.destinationType == DestinationLink && packet.context == ContextResourceProof) {
            if (packet.data.size() != 2 * kFullHashLength)
                return;
            auto resource = m_outgoingResources.find(packet.data.left(kFullHashLength));
            if (resource == m_outgoingResources.end() || packet.data.mid(kFullHashLength) != resource->expectedProof)
                return;
            const QString id = resource->messageId;
            const QByteArray linkId = resource->linkId;
            m_outgoingResources.erase(resource);
            QTimer::singleShot(0, this, [this, linkId] { startNextOnLink(linkId); });
            if (ChatMessage *message = find(id)) {
                message->progress = 100;
                setState(*message, MessageState::Delivered, QString(""));
                m_node->addEvent(QStringLiteral("message"), QStringLiteral("Message delivered to %1").arg(peerName(message->peer)),
                                 message->peer);
            }
            return;
        }
        QByteArray key = packet.destinationHash;
        if (packet.destinationType == DestinationLink) {
            if (packet.data.size() != kFullHashLength + kSignatureLength)
                return;
            key = packet.data.left(kHashLength);
        }
        auto receipt = m_receipts.find(key);
        if (receipt == m_receipts.end() || !validateProof(receipt->signingPublic, receipt->packetHash, packet.data))
            return;
        const QString id = receipt->messageId;
        m_receipts.erase(receipt);
        if (ChatMessage *message = find(id)) {
            setState(*message, MessageState::Delivered, QString(""));
            m_node->addEvent(QStringLiteral("message"), QStringLiteral("Message delivered to %1").arg(peerName(message->peer)),
                             message->peer);
        }
        return;
    }
    if (packet.packetType == PacketLinkRequest) {
        if (packet.destinationHash == address())
            handleLinkRequest(interfaceId, packet, raw);
        return;
    }
    if (packet.packetType != PacketData)
        return;
    if (packet.destinationType == DestinationLink) {
        handleLinkData(interfaceId, packet, raw);
        return;
    }
    if (packet.destinationType == DestinationSingle && packet.destinationHash == address() &&
        packet.context == ContextNone) {
        const std::optional<QByteArray> plaintext = m_node->identity().decrypt(packet.data);
        if (!plaintext)
            return;
        const QByteArray hash = packetHash(raw);
        m_node->sendRaw(buildProof(signingPrivate(), hash, false, hash.left(kHashLength), DestinationSingle), interfaceId);
        receiveLxmf(address() + *plaintext, QStringLiteral("opportunistic"));
    }
}

void Messenger::handleLinkRequest(const QString &interfaceId, const Packet &packet, const QByteArray &raw)
{
    if (packet.data.size() != 64 && packet.data.size() != 67)
        return;
    if (m_links.size() >= kMaxLinks)
        return;
    int mtu = kMtu;
    if (packet.data.size() == 67) {
        if (((quint8(packet.data.at(64)) & 0xE0) >> 5) != 1)
            return;
        mtu = int(((quint32(quint8(packet.data.at(64))) << 16) | (quint32(quint8(packet.data.at(65))) << 8) |
                   quint8(packet.data.at(66))) & 0x1FFFFF);
        mtu = qBound(kMtu, mtu, kStreamHardwareMtu);
    }
    QByteArray hashable(1, char(quint8(raw.at(0)) & 0x0F));
    hashable += packet.headerType == Header2 ? raw.mid(kHashLength + 2) : raw.mid(2);
    if (packet.data.size() > 64)
        hashable.chop(packet.data.size() - 64);
    const QByteArray linkId = truncatedHash(hashable);
    if (m_links.contains(linkId))
        return;
    const Identity ephemeral = Identity::generate();
    Link link;
    link.id = linkId;
    link.initiator = false;
    link.interfaceId = interfaceId;
    link.ownEncryptionPrivate = ephemeral.privateKey().left(32);
    link.ownEncryptionPublic = ephemeral.encryptionPublicKey();
    link.ownSigningPrivate = signingPrivate();
    link.peerSigningPublic = packet.data.mid(32, 32);
    link.key = hkdf(64, x25519Exchange(link.ownEncryptionPrivate, packet.data.left(32)), linkId);
    link.mtu = mtu;
    link.requestedAt = nowMs();
    link.lastInbound = nowMs();
    if (link.key.isEmpty())
        return;
    const QByteArray signalBytes = signalling(mtu);
    const QByteArray signature =
        ed25519Sign(link.ownSigningPrivate, linkId + link.ownEncryptionPublic + m_node->identity().signingPublicKey() + signalBytes);
    Packet proof;
    proof.packetType = PacketProof;
    proof.destinationType = DestinationLink;
    proof.destinationHash = linkId;
    proof.context = ContextLinkRequestProof;
    proof.data = signature + link.ownEncryptionPublic + signalBytes;
    m_node->sendRaw(packPacket(proof), interfaceId);
    m_links.insert(linkId, link);
    updateTicking();
}

void Messenger::handleLinkData(const QString &, const Packet &packet, const QByteArray &raw)
{
    auto it = m_links.find(packet.destinationHash);
    if (it == m_links.end() || it->key.isEmpty())
        return;
    Link &link = *it;
    link.lastInbound = nowMs();
    switch (packet.context) {
    case ContextKeepalive:
        if (!link.initiator && packet.data == QByteArray(1, char(0xFF)))
            linkPacket(link, PacketData, ContextKeepalive, QByteArray(1, char(0xFE)), false);
        return;
    case ContextResource:
        handleResourcePart(link, packet.data);
        return;
    default:
        break;
    }
    const std::optional<QByteArray> plaintext = tokenDecrypt(link.key, packet.data);
    if (!plaintext)
        return;
    switch (packet.context) {
    case ContextLinkRtt:
        if (!link.initiator)
            link.active = true;
        break;
    case ContextLinkClose:
        if (*plaintext == link.id)
            closeLink(link.id, false);
        break;
    case ContextNone: {
        const QByteArray hash = packetHash(raw);
        Packet proof;
        proof.packetType = PacketProof;
        proof.destinationType = DestinationLink;
        proof.destinationHash = link.id;
        proof.data = hash + ed25519Sign(link.ownSigningPrivate, hash);
        m_node->sendRaw(packPacket(proof), link.interfaceId);
        if (!link.initiator)
            receiveLxmf(*plaintext, QStringLiteral("link"));
        break;
    }
    case ContextResourceAdvertisement:
        handleResourceAdvertisement(link, *plaintext);
        break;
    case ContextResourceRequest:
        handleResourceRequest(link, *plaintext);
        break;
    case ContextResourceHashmapUpdate:
        handleHashmapUpdate(link, *plaintext);
        break;
    case ContextResourceInitiatorCancel:
        m_incomingResources.remove(*plaintext);
        break;
    case ContextResourceReceiverCancel: {
        auto resource = m_outgoingResources.find(*plaintext);
        if (resource != m_outgoingResources.end()) {
            const QString id = resource->messageId;
            const QByteArray linkId = resource->linkId;
            m_outgoingResources.erase(resource);
            fail(id, QStringLiteral("The recipient refused the message"));
            QTimer::singleShot(0, this, [this, linkId] { startNextOnLink(linkId); });
        }
        break;
    }
    default:
        break;
    }
}

void Messenger::handleResourceAdvertisement(Link &link, const QByteArray &plaintext)
{
    const std::optional<ResourceAdvertisement> advertisement = unpackResourceAdvertisement(plaintext);
    if (!advertisement)
        return;
    if (m_incomingResources.contains(advertisement->hash))
        return;
    if (link.initiator || advertisement->compressed() || advertisement->segments > 1 || advertisement->isRequest() ||
        advertisement->isResponse() || advertisement->transferSize > kMaxIncomingResource + 4096 ||
        m_incomingResources.size() >= kMaxIncomingResources) {
        linkPacket(link, PacketData, ContextResourceReceiverCancel, advertisement->hash);
        return;
    }
    IncomingResource resource;
    resource.linkId = link.id;
    resource.advertisement = *advertisement;
    for (int i = 0; i < advertisement->parts; ++i) {
        resource.parts.append(QByteArray());
        resource.hashmap.append(QByteArray());
    }
    for (int i = 0; i * kResourceMapHashLength < advertisement->hashmap.size() && i < advertisement->parts; ++i)
        resource.hashmap[i] = advertisement->hashmap.mid(i * kResourceMapHashLength, kResourceMapHashLength);
    resource.lastActivity = nowMs();
    m_incomingResources.insert(advertisement->hash, resource);
    requestNextParts(m_incomingResources[advertisement->hash]);
    updateTicking();
}

void Messenger::requestNextParts(IncomingResource &resource)
{
    auto link = m_links.find(resource.linkId);
    if (link == m_links.end() || resource.waitingForHashmap)
        return;
    QByteArray hashes;
    bool exhausted = false;
    int requested = 0;
    for (int i = 0; i < resource.parts.size() && requested < kRequestWindow; ++i) {
        if (!resource.parts.at(i).isEmpty())
            continue;
        if (resource.hashmap.at(i).isEmpty()) {
            exhausted = true;
            break;
        }
        hashes += resource.hashmap.at(i);
        ++requested;
    }
    if (requested == 0 && !exhausted)
        return;
    QByteArray request(1, char(exhausted ? 0xFF : 0x00));
    if (exhausted) {
        int height = 0;
        while (height < resource.hashmap.size() && !resource.hashmap.at(height).isEmpty())
            ++height;
        if (height == 0)
            return;
        request += resource.hashmap.at(height - 1);
        resource.waitingForHashmap = true;
    }
    request += resource.advertisement.hash + hashes;
    resource.requested = requested;
    resource.lastActivity = nowMs();
    linkPacket(*link, PacketData, ContextResourceRequest, request);
}

void Messenger::handleResourcePart(Link &link, const QByteArray &data)
{
    for (auto it = m_incomingResources.begin(); it != m_incomingResources.end(); ++it) {
        IncomingResource &resource = *it;
        if (resource.linkId != link.id)
            continue;
        const QByteArray mapHash = resourceMapHash(data, resource.advertisement.randomHash);
        for (int i = 0; i < resource.parts.size(); ++i) {
            if (resource.parts.at(i).isEmpty() && resource.hashmap.at(i) == mapHash) {
                resource.parts[i] = data;
                ++resource.received;
                --resource.requested;
                resource.lastActivity = nowMs();
                resource.retries = 0;
                const QByteArray hash = it.key();
                if (resource.received == resource.parts.size())
                    assembleResource(hash);
                else if (resource.requested <= 0)
                    requestNextParts(resource);
                return;
            }
        }
    }
}

void Messenger::handleHashmapUpdate(Link &, const QByteArray &plaintext)
{
    if (plaintext.size() <= kFullHashLength)
        return;
    auto it = m_incomingResources.find(plaintext.left(kFullHashLength));
    if (it == m_incomingResources.end())
        return;
    const QVariantList update = decodeMsgPack(plaintext.mid(kFullHashLength)).toList();
    if (update.size() < 2)
        return;
    const int segment = update.at(0).toInt();
    const QByteArray hashmap = update.at(1).toByteArray();
    for (int i = 0; i * kResourceMapHashLength < hashmap.size(); ++i) {
        const int index = segment * kResourceHashmapMaxLength + i;
        if (index >= 0 && index < it->hashmap.size())
            it->hashmap[index] = hashmap.mid(i * kResourceMapHashLength, kResourceMapHashLength);
    }
    it->waitingForHashmap = false;
    it->lastActivity = nowMs();
    requestNextParts(*it);
}

void Messenger::assembleResource(const QByteArray &hash)
{
    auto it = m_incomingResources.find(hash);
    if (it == m_incomingResources.end())
        return;
    const IncomingResource resource = *it;
    m_incomingResources.erase(it);
    auto link = m_links.find(resource.linkId);
    if (link == m_links.end())
        return;
    const std::optional<QByteArray> plain = tokenDecrypt(link->key, resource.parts.join());
    if (!plain || plain->size() < kResourceRandomLength)
        return;
    const QByteArray data = plain->mid(kResourceRandomLength);
    if (fullHash(data + resource.advertisement.randomHash) != hash)
        return;
    linkPacket(*link, PacketProof, ContextResourceProof, hash + fullHash(data + hash), false);
    QByteArray payload = data;
    if (resource.advertisement.hasMetadata() && payload.size() >= 3) {
        const int size = (quint8(payload.at(0)) << 16) | (quint8(payload.at(1)) << 8) | quint8(payload.at(2));
        payload = payload.mid(3 + size);
    }
    receiveLxmf(payload, QStringLiteral("resource"));
}

void Messenger::handleResourceRequest(Link &link, const QByteArray &plaintext)
{
    if (plaintext.isEmpty())
        return;
    const bool exhausted = quint8(plaintext.at(0)) == 0xFF;
    const int pad = exhausted ? 1 + kResourceMapHashLength : 1;
    if (plaintext.size() < pad + kFullHashLength)
        return;
    auto it = m_outgoingResources.find(plaintext.mid(pad, kFullHashLength));
    if (it == m_outgoingResources.end() || it->linkId != link.id)
        return;
    OutgoingResource &resource = *it;
    resource.requested = true;
    resource.lastActivity = nowMs();
    const QByteArray requested = plaintext.mid(pad + kFullHashLength);
    for (int offset = 0; offset + kResourceMapHashLength <= requested.size(); offset += kResourceMapHashLength) {
        const int index = int(resource.mapHashes.indexOf(requested.mid(offset, kResourceMapHashLength)));
        if (index < 0)
            continue;
        linkPacket(link, PacketData, ContextResource, resource.parts.at(index), false);
        resource.sent.insert(index);
    }
    if (exhausted) {
        const int index = int(resource.mapHashes.indexOf(plaintext.mid(1, kResourceMapHashLength)));
        const int partIndex = index + 1;
        if (index >= 0 && partIndex % kResourceHashmapMaxLength == 0) {
            const int segment = partIndex / kResourceHashmapMaxLength;
            const QByteArray hashmap = resource.mapHashes.mid(segment * kResourceHashmapMaxLength, kResourceHashmapMaxLength).join();
            if (!hashmap.isEmpty())
                linkPacket(link, PacketData, ContextResourceHashmapUpdate,
                           resource.hash + MsgPackWriter().array(2).integer(segment).bin(hashmap).bytes());
        }
    }
    if (ChatMessage *message = find(resource.messageId)) {
        message->progress = int(100 * resource.sent.size() / qMax<qsizetype>(1, resource.parts.size()));
        setState(*message, MessageState::Sending, QStringLiteral("%1% sent").arg(message->progress));
    }
}

void Messenger::receiveLxmf(const QByteArray &packed, const QString &method)
{
    QString error;
    const std::optional<LxmfMessage> lxmf = lxmfUnpack(packed, &error);
    if (!lxmf || lxmf->destinationHash != address())
        return;
    const QString id = hexHash(lxmf->hash);
    if (m_seenIds.contains(id))
        return;
    ChatMessage message;
    message.id = id;
    message.peer = lxmf->sourceHash;
    message.outgoing = false;
    message.title = lxmf->title;
    message.content = lxmf->content;
    message.timestamp = lxmf->timestamp > 0 ? lxmf->timestamp : double(nowMs()) / 1000.0;
    message.state = MessageState::Received;
    message.method = method;
    message.read = false;
    message.attachments = lxmf->attachments;
    if (const std::optional<Identity> source = m_node->identityFor(lxmf->sourceHash)) {
        message.verified = lxmfVerify(*lxmf, source->publicKey());
        if (!message.verified)
            message.note = QStringLiteral("The signature does not match the sender's announced key");
    } else {
        message.note = QStringLiteral("Sender not yet heard on the network, so the signature is unchecked");
    }
    m_seenIds.insert(id);
    m_messages.append(message);
    if (m_messages.size() > kMaxMessages + 500)
        m_messages.erase(m_messages.begin(), m_messages.begin() + (m_messages.size() - kMaxMessages));
    const QString name = peerName(message.peer);
    if (!name.startsWith(QLatin1Char('<')))
        m_names.insert(message.peer, name);
    m_node->addEvent(QStringLiteral("message"), QStringLiteral("Message from %1").arg(name), message.peer);
    scheduleSave();
    notifyChanged();
    if (m_incoming)
        m_incoming(message);
}

void Messenger::notifyChanged()
{
    if (!m_changeTimer->isActive())
        m_changeTimer->start();
}

void Messenger::scheduleSave()
{
    if (!m_saveTimer->isActive())
        m_saveTimer->start();
}

void Messenger::updateTicking()
{
    const bool pending = !m_links.isEmpty() || !m_incomingResources.isEmpty() || !m_outgoingResources.isEmpty() ||
                         std::any_of(m_messages.cbegin(), m_messages.cend(), [](const ChatMessage &m) {
                             return m.outgoing && (int(m.state) < int(MessageState::Sent) ||
                                                   (m.state == MessageState::Sent && m.attempts < 3));
                         });
    if (pending && !m_timer->isActive())
        m_timer->start();
    else if (!pending && m_timer->isActive())
        m_timer->stop();
}

void Messenger::tick()
{
    const qint64 now = nowMs();
    QStringList toProcess;
    QStringList toFail;
    for (ChatMessage &message : m_messages) {
        if (!message.outgoing)
            continue;
        if (message.state == MessageState::Queued && m_node->onlineCount() > 0) {
            toProcess.append(message.id);
        } else if (message.state == MessageState::FindingPath) {
            if (m_node->identityFor(message.peer))
                toProcess.append(message.id);
            else if (now - m_waitingSince.value(message.id, now) > kPathWaitSeconds * 1000)
                toFail.append(message.id);
        } else if (message.state == MessageState::Sent && message.method == QStringLiteral("opportunistic") &&
                   now - m_sentAt.value(message.id, now) > 15000 && message.attempts < 3) {
            toProcess.append(message.id);
        } else if (message.state == MessageState::Linking &&
                   now - m_waitingSince.value(message.id, now) > kLinkWaitSeconds * 1000) {
            toFail.append(message.id);
        }
    }
    for (const QString &id : toFail) {
        ChatMessage *message = find(id);
        if (!message)
            continue;
        if (message->state == MessageState::FindingPath)
            fail(id, QStringLiteral("No path to this contact. They may be offline; retry later."));
        else
            fail(id, QStringLiteral("Could not open a link to this contact"));
    }
    for (const QString &id : toProcess)
        process(id);
    QList<QByteArray> stale;
    for (auto it = m_links.begin(); it != m_links.end(); ++it) {
        Link &link = *it;
        if (!link.active && link.initiator && now - link.requestedAt > kLinkWaitSeconds * 1000) {
            stale.append(link.id);
            continue;
        }
        const bool busy = !link.waiting.isEmpty() ||
                          std::any_of(m_outgoingResources.cbegin(), m_outgoingResources.cend(),
                                      [&link](const OutgoingResource &r) { return r.linkId == link.id; }) ||
                          std::any_of(m_incomingResources.cbegin(), m_incomingResources.cend(),
                                      [&link](const IncomingResource &r) { return r.linkId == link.id; });
        const qint64 idle = now - qMax(link.lastInbound, link.lastOutbound);
        if (!busy && link.initiator && link.active && idle > kLinkIdleSeconds * 1000)
            stale.append(link.id);
        else if (!link.initiator && !link.active && now - link.requestedAt > 60 * 1000)
            stale.append(link.id);
        else if (!link.initiator && now - link.lastInbound > 15 * 60 * 1000)
            stale.append(link.id);
    }
    for (const QByteArray &id : stale)
        closeLink(id, true);
    QList<QByteArray> dropped;
    for (auto it = m_incomingResources.begin(); it != m_incomingResources.end(); ++it) {
        if (now - it->lastActivity < kResourceStallSeconds * 1000)
            continue;
        if (++it->retries > 5) {
            dropped.append(it.key());
            continue;
        }
        it->waitingForHashmap = false;
        requestNextParts(*it);
    }
    for (const QByteArray &hash : dropped)
        m_incomingResources.remove(hash);
    QList<QByteArray> failed;
    for (auto it = m_outgoingResources.begin(); it != m_outgoingResources.end(); ++it) {
        if (!it->requested && now - it->lastActivity > 10000) {
            if (it->advertisements >= 4)
                failed.append(it.key());
            else
                advertiseResource(*it);
        } else if (it->requested && now - it->lastActivity > 60000) {
            failed.append(it.key());
        }
    }
    for (const QByteArray &hash : failed) {
        const OutgoingResource resource = m_outgoingResources.take(hash);
        fail(resource.messageId, QStringLiteral("The transfer stalled"));
        startNextOnLink(resource.linkId);
    }
    for (auto it = m_receipts.begin(); it != m_receipts.end();) {
        if (now - it->sentAt > 10 * 60 * 1000)
            it = m_receipts.erase(it);
        else
            ++it;
    }
    for (ChatMessage &message : m_messages) {
        if (message.outgoing && message.state == MessageState::Sent && message.attempts >= 3 &&
            message.note == QStringLiteral("Waiting for delivery confirmation")) {
            message.note = QStringLiteral("No delivery confirmation yet");
            notifyChanged();
        }
    }
    updateTicking();
}

}
