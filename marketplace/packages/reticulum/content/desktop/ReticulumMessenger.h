#pragma once

#include "ReticulumLxmf.h"
#include "ReticulumNode.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QTimer;

namespace forkmesh::reticulum {

enum class MessageState { Queued, Stamping, FindingPath, Linking, Sending, Sent, Delivered, Failed, Received };

struct ChatMessage {
    QString id;
    QByteArray peer;
    bool outgoing = false;
    QString title;
    QString content;
    double timestamp = 0;
    MessageState state = MessageState::Queued;
    QString method;
    QString note;
    bool verified = false;
    bool read = true;
    int progress = 0;
    int attempts = 0;
    QList<LxmfAttachment> attachments;

    QJsonObject toJson() const;
    static std::optional<ChatMessage> fromJson(const QJsonObject &object);
};

struct Conversation {
    QByteArray peer;
    QString name;
    ChatMessage last;
    int unread = 0;
    int count = 0;
};

QString messageStateLabel(MessageState state);

class Messenger : public QObject
{
public:
    static constexpr int kMaxMessages = 5000;
    static constexpr int kMaxContentBytes = 64 * 1024;
    static constexpr int kMaxIncomingResource = kResourceMaxSize;
    static constexpr int kPathWaitSeconds = 30;
    static constexpr int kLinkWaitSeconds = 30;
    static constexpr int kResourceStallSeconds = 15;
    static constexpr int kLinkIdleSeconds = 120;

    explicit Messenger(Node *node, QObject *parent = nullptr);
    ~Messenger() override;

    QByteArray address() const;
    void setDisplayName(const QString &name);
    QString displayName() const { return m_displayName; }
    void setChangedHandler(std::function<void()> handler) { m_changed = std::move(handler); }
    void setIncomingHandler(std::function<void(const ChatMessage &)> handler) { m_incoming = std::move(handler); }
    void setSaveHandler(std::function<void(const QString &)> handler) { m_save = std::move(handler); }
    void load(const QString &json);
    QString saveJson() const;

    QString send(const QByteArray &peer, const QString &content, const QString &title = {});
    void retry(const QString &id);
    void forget(const QString &id);
    void clearConversation(const QByteArray &peer);
    void markRead(const QByteArray &peer);
    QList<ChatMessage> messages(const QByteArray &peer) const;
    QList<Conversation> conversations() const;
    std::optional<ChatMessage> message(const QString &id) const;
    int unreadCount() const;
    QString peerName(const QByteArray &peer) const;
    int activeLinks() const;
    int peerStampCost(const QByteArray &peer) const;

    void handlePacket(const QString &interfaceId, const Packet &packet, const QByteArray &raw, int hops);
    void destinationAnnounced(const QByteArray &hash);
    void tickForTest() { tick(); }

private:
    struct Link {
        QByteArray id;
        bool initiator = false;
        bool active = false;
        QByteArray peer;
        QString interfaceId;
        QByteArray ownEncryptionPrivate;
        QByteArray ownEncryptionPublic;
        QByteArray ownSigningPrivate;
        QByteArray peerSigningPublic;
        QByteArray key;
        int mtu = kMtu;
        qint64 requestedAt = 0;
        qint64 lastInbound = 0;
        qint64 lastOutbound = 0;
        QStringList waiting;
    };
    struct Receipt {
        QString messageId;
        QByteArray packetHash;
        QByteArray signingPublic;
        qint64 sentAt = 0;
    };
    struct IncomingResource {
        QByteArray linkId;
        ResourceAdvertisement advertisement;
        QList<QByteArray> hashmap;
        QList<QByteArray> parts;
        int received = 0;
        int requested = 0;
        bool waitingForHashmap = false;
        qint64 lastActivity = 0;
        int retries = 0;
    };
    struct OutgoingResource {
        QByteArray linkId;
        QString messageId;
        QByteArray hash;
        QByteArray randomHash;
        QByteArray expectedProof;
        QList<QByteArray> parts;
        QList<QByteArray> mapHashes;
        qint64 dataSize = 0;
        qint64 transferSize = 0;
        QSet<int> sent;
        qint64 lastActivity = 0;
        int advertisements = 0;
        bool requested = false;
    };

    ChatMessage *find(const QString &id);
    void process(const QString &id);
    void deliverOpportunistic(ChatMessage &message, const QByteArray &packed, const Identity &peer);
    void deliverOverLink(ChatMessage &message, Link &link);
    void startNextOnLink(const QByteArray &linkId);
    Link *openLink(const QByteArray &peer);
    Link *linkTo(const QByteArray &peer);
    QByteArray linkPacket(Link &link, int type, quint8 context, const QByteArray &plaintext, bool encrypt = true);
    void closeLink(const QByteArray &linkId, bool notifyPeer);
    void handleLinkRequest(const QString &interfaceId, const Packet &packet, const QByteArray &raw);
    void handleLinkProof(const Packet &packet);
    void handleLinkData(const QString &interfaceId, const Packet &packet, const QByteArray &raw);
    void handleProof(const Packet &packet);
    void handleResourceAdvertisement(Link &link, const QByteArray &plaintext);
    void handleResourcePart(Link &link, const QByteArray &data);
    void handleResourceRequest(Link &link, const QByteArray &plaintext);
    void handleHashmapUpdate(Link &link, const QByteArray &plaintext);
    void requestNextParts(IncomingResource &resource);
    void assembleResource(const QByteArray &hash);
    void advertiseResource(OutgoingResource &resource);
    void receiveLxmf(const QByteArray &packed, const QString &method);
    void setState(ChatMessage &message, MessageState state, const QString &note = {});
    void fail(const QString &id, const QString &note);
    void notifyChanged();
    void scheduleSave();
    void tick();
    void updateTicking();
    QByteArray packedFor(ChatMessage &message);
    QByteArray signingPrivate() const;

    Node *m_node;
    QString m_displayName;
    QList<ChatMessage> m_messages;
    QHash<QByteArray, QString> m_names;
    QSet<QString> m_seenIds;
    QHash<QString, QByteArray> m_stamps;
    QHash<QString, QByteArray> m_payloads;
    QHash<QString, qint64> m_waitingSince;
    QHash<QString, qint64> m_sentAt;
    QHash<QByteArray, Link> m_links;
    QHash<QByteArray, Receipt> m_receipts;
    QHash<QByteArray, IncomingResource> m_incomingResources;
    QHash<QByteArray, OutgoingResource> m_outgoingResources;
    QHash<QString, std::shared_ptr<std::atomic_bool>> m_stampJobs;
    std::function<void()> m_changed;
    std::function<void(const ChatMessage &)> m_incoming;
    std::function<void(const QString &)> m_save;
    QTimer *m_timer = nullptr;
    QTimer *m_saveTimer = nullptr;
    QTimer *m_changeTimer = nullptr;
    qint64 m_lastOwnAnnounce = 0;
};

}
