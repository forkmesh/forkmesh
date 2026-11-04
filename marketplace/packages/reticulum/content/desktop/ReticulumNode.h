#pragma once

#include "ReticulumProtocol.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVector>

#include <functional>
#include <optional>

class QLocalSocket;
class QTcpSocket;
class QTimer;
class QUdpSocket;

namespace forkmesh::reticulum {

enum class InterfaceKind { TcpClient, SharedInstance, Udp };
enum class LinkState { Disabled, Connecting, Online, Waiting, Blocked, Failed };

struct InterfaceConfig {
    QString id;
    QString name;
    InterfaceKind kind = InterfaceKind::TcpClient;
    bool enabled = true;
    QString host;
    int port = 4242;
    bool kiss = false;
    QString instanceName = QStringLiteral("default");
    bool sharedOverTcp = false;
    QString listenAddress = QStringLiteral("0.0.0.0");
    int listenPort = 4242;
    QString forwardAddress = QStringLiteral("255.255.255.255");
    int forwardPort = 4242;
    QString networkName;
    QString passphrase;
    int accessCodeSize = 16;

    bool usesAccessCode() const { return !networkName.isEmpty() || !passphrase.isEmpty(); }
    QJsonObject toJson() const;
    static std::optional<InterfaceConfig> fromJson(const QJsonObject &object);
    QString endpoint() const;
    QString kindLabel() const;
    QString validationError() const;
    bool sameTarget(const InterfaceConfig &other) const;
};

struct TrafficSample {
    qint64 rx = 0;
    qint64 tx = 0;
    int announces = 0;
};

struct InterfaceStatus {
    LinkState state = LinkState::Disabled;
    QString detail;
    qint64 rxBytes = 0;
    qint64 txBytes = 0;
    qint64 rxPackets = 0;
    qint64 txPackets = 0;
    qint64 announces = 0;
    qint64 invalidFrames = 0;
    qint64 rejected = 0;
    qint64 connectMs = -1;
    int attempts = 0;
    int sessions = 0;
    QDateTime onlineSince;
    QDateTime nextAttempt;
    QVector<TrafficSample> history;
};

struct DestinationRecord {
    QByteArray hash;
    QByteArray identityHash;
    QByteArray publicKey;
    QByteArray nameHash;
    QByteArray transportId;
    QByteArray appData;
    QString aspect;
    QString label;
    QString kind;
    QString displayName;
    QString interfaceId;
    int hops = 0;
    int announces = 0;
    bool ratchet = false;
    QByteArray ratchetKey;
    bool viaSharedInstance = false;
    qint64 emittedAt = 0;
    QDateTime firstHeard;
    QDateTime lastHeard;
    std::optional<DiscoveredInterface> discovery;
};

struct NetworkEvent {
    QDateTime at;
    QString kind;
    QString text;
    QByteArray destination;
};

struct PathLookup {
    QByteArray destination;
    QByteArray tag;
    QDateTime started;
    QDateTime answered;
    int hops = -1;
    QString interfaceId;
    bool timedOut = false;
};

struct PacketCounters {
    qint64 data = 0;
    qint64 announces = 0;
    qint64 linkRequests = 0;
    qint64 proofs = 0;
    qint64 duplicates = 0;
    qint64 invalid = 0;
    qint64 rejectedAnnounces = 0;
    qint64 pathRequestsSeen = 0;
    qint64 pathRequestsAnswered = 0;
    qint64 announcesSent = 0;
};

class InterfaceRuntime;

class Node : public QObject
{
public:
    using Authorizer = std::function<bool(const QString &, const QUrl &)>;

    static constexpr int kHistorySeconds = 180;
    static constexpr int kMaxDestinations = 20000;
    static constexpr int kMaxEvents = 400;
    static constexpr int kMaxSeenPackets = 60000;
    static constexpr int kPathTimeoutSeconds = 20;

    explicit Node(QObject *parent = nullptr);
    ~Node() override;

    void setAuthorizer(Authorizer authorizer);
    void setChangedHandler(std::function<void()> handler);
    void setAnnounceHandler(std::function<void(const QByteArray &)> handler) { m_announced = std::move(handler); }
    using PacketHandler = std::function<void(const QString &, const Packet &, const QByteArray &, int)>;
    void setPacketHandler(PacketHandler handler) { m_packetHandler = std::move(handler); }
    void addLocalDestination(const QString &aspect, std::function<QByteArray()> appData);
    QByteArray localDestinationHash(const QString &aspect) const;
    bool isLocalDestination(const QByteArray &hash) const { return !localAspect(hash).isEmpty(); }
    QString localAspect(const QByteArray &hash) const;
    QByteArray sendPacket(Packet packet, const QString &onlyInterface = {});
    QByteArray sendRaw(const QByteArray &raw, const QString &onlyInterface = {});
    std::optional<Identity> identityFor(const QByteArray &destination) const;
    bool interfaceOnline(const QString &id) const;

    QList<InterfaceConfig> interfaces() const;
    void setInterfaces(const QList<InterfaceConfig> &configs);
    void upsertInterface(const InterfaceConfig &config);
    void removeInterface(const QString &id);
    void setInterfaceEnabled(const QString &id, bool enabled);
    void reconnectInterface(const QString &id);
    InterfaceStatus status(const QString &id) const;
    int onlineCount() const;

    void start();
    void stop();
    bool running() const { return m_running; }

    void setIdentity(const Identity &identity);
    const Identity &identity() const { return m_identity; }
    QString aspect() const { return m_aspect; }
    QByteArray destinationHash() const;
    void setAnnounceName(const QString &name);
    QString announceName() const { return m_announceName; }
    void setAnswerPathRequests(bool answer) { m_answerPathRequests = answer; }
    bool announce(bool pathResponse = false, const QString &onlyInterface = {});
    bool announceDestination(const QByteArray &destination, bool pathResponse = false, const QString &onlyInterface = {});
    QDateTime lastAnnounce() const { return m_lastAnnounce; }

    QByteArray requestPath(const QByteArray &destination);
    const QHash<QByteArray, PathLookup> &lookups() const { return m_lookups; }

    const QHash<QByteArray, DestinationRecord> &destinations() const { return m_destinations; }
    void forgetDestination(const QByteArray &hash);
    void clearDestinations();
    const QList<NetworkEvent> &events() const { return m_events; }
    const PacketCounters &counters() const { return m_counters; }
    QVector<TrafficSample> totalHistory() const;
    QDateTime startedAt() const { return m_startedAt; }

    void ingest(const QString &interfaceId, const QByteArray &raw, bool fromSharedInstance = false);
    void addEvent(const QString &kind, const QString &text, const QByteArray &destination = {});
    void tickForTest() { tick(); }

private:
    friend class InterfaceRuntime;

    InterfaceRuntime *runtime(const QString &id) const;
    void transmit(const QByteArray &raw, const QString &onlyInterface = {});
    void notifyChanged();
    bool rememberPacket(const QByteArray &hash);
    void handleAnnounce(const QString &interfaceId, const Packet &packet, const QByteArray &raw, int hops);
    void handlePathRequest(const QString &interfaceId, const Packet &packet);
    void trimDestinations();
    void tick();
    void updateTicking();
    QString interfaceName(const QString &id) const;

    QList<InterfaceRuntime *> m_runtimes;
    Authorizer m_authorizer;
    std::function<void()> m_changed;
    std::function<void(const QByteArray &)> m_announced;
    PacketHandler m_packetHandler;
    struct LocalDestination {
        QString aspect;
        std::function<QByteArray()> appData;
    };
    QHash<QString, LocalDestination> m_local;
    QHash<QByteArray, QDateTime> m_lastPathAnswers;
    QTimer *m_changeTimer = nullptr;
    QTimer *m_tickTimer = nullptr;
    bool m_running = false;
    QDateTime m_startedAt;
    Identity m_identity;
    QString m_aspect = QStringLiteral("forkmesh.node");
    QString m_announceName;
    bool m_answerPathRequests = true;
    QDateTime m_lastAnnounce;
    QHash<QByteArray, DestinationRecord> m_destinations;
    QHash<QByteArray, PathLookup> m_lookups;
    QList<NetworkEvent> m_events;
    QSet<QByteArray> m_seen;
    QQueue<QByteArray> m_seenOrder;
    PacketCounters m_counters;
    QVector<TrafficSample> m_history;
    TrafficSample m_current;
};

class InterfaceRuntime : public QObject
{
public:
    InterfaceRuntime(Node *node, const InterfaceConfig &config);
    ~InterfaceRuntime() override;

    const InterfaceConfig &config() const { return m_config; }
    void setConfig(const InterfaceConfig &config);
    const InterfaceStatus &status() const { return m_status; }
    bool online() const { return m_status.state == LinkState::Online; }
    void start();
    void stop();
    void reconnectNow();
    bool send(const QByteArray &raw);
    void rotateHistory();

private:
    friend class Node;

    void connectTcp();
    void connectLocal();
    void bindUdp();
    void setState(LinkState state, const QString &detail = {});
    void wentOnline();
    void lostConnection(const QString &reason);
    void scheduleRetry(const QString &reason);
    void receive(const QByteArray &bytes);
    void receiveFrame(const QByteArray &frame);
    void dropSockets();
    bool authorize(const QString &method, const QUrl &url);

    Node *m_node;
    InterfaceConfig m_config;
    InterfaceStatus m_status;
    FrameDecoder m_decoder;
    InterfaceAccessCode m_accessCode;
    QPointer<QTcpSocket> m_tcp;
    QPointer<QLocalSocket> m_local;
    QPointer<QUdpSocket> m_udp;
    QTimer *m_retryTimer = nullptr;
    QTimer *m_keepaliveTimer = nullptr;
    qint64 m_connectStarted = 0;
    bool m_active = false;
    bool m_authorized = false;
    TrafficSample m_current;
};

QString linkStateLabel(LinkState state);
QList<InterfaceConfig> defaultInterfaces();
QList<InterfaceConfig> publicEntryPoints();
QList<InterfaceConfig> parseReticulumConfig(const QString &text, QStringList *skipped = nullptr);
QString reticulumConfigPath();

}
