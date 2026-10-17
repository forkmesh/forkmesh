#pragma once

#include "ChatBackend.h"
#include "RoomCrypto.h"
#include "RoomSeal.h"

#include <QByteArray>
#include <QHash>
#include <QHostAddress>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

class QIODevice;
class QTcpServer;
class QTcpSocket;
class QTimer;
class QUdpSocket;
#ifdef FORKMESH_HAS_BLUETOOTH
class BleMeshLink;
class QBluetoothDeviceDiscoveryAgent;
class QBluetoothDeviceInfo;
class QBluetoothLocalDevice;
class QBluetoothServer;
class QBluetoothServiceDiscoveryAgent;
class QBluetoothSocket;
class QBluetoothServiceInfo;
#endif

// The local mesh room: chat between ForkMesh apps that find each other in the
// same place, with no relay, no account, and nothing sent to the internet.
//
// Two transports carry the same room, and a device may be reached over either:
//
//   * Local network. An IPv4 administratively-scoped multicast beacon with
//     TTL 1, sent only on interfaces that hold a private, link-local or
//     unique-local address; every peer address is re-checked against the same
//     rule before a socket is opened or accepted, so a routable host can
//     neither be reached nor join. Messages travel over direct TCP links,
//     accepted on the same well-known port number as the beacon so that one
//     firewall rule lets the whole room through (see MeshFirewall.h).
//   * Bluetooth (built when Qt's Bluetooth module is available). An RFCOMM
//     service under a ForkMesh-specific UUID, found by service discovery on
//     paired and nearby devices. This is the path that still works with no
//     Wi-Fi, no router and no internet at all — two laptops on a table, or a
//     laptop and a phone, reach each other directly.
//
// Frames are identical on both, so a device bridged to one peer over Wi-Fi and
// another over Bluetooth relays between them and the room stays one room.
namespace forkmesh::localmesh {

// The one conversation this transport carries. Deliberately not a "#channel"
// the relay backend knows about: it is never advertised to, or persisted by,
// anything off this device.
QString conversation();
bool isMeshConversation(const QString &conversation);

// Room name both encryption layers are scoped to. Shared by every ForkMesh
// install, which is what makes zero-configuration discovery possible: the
// place a device is — this network, or Bluetooth range — is the trust
// boundary, and no key, phrase or account is exchanged to join.
QString roomName();

// IPv4 administratively-scoped group (239.192/14) and the port both the
// beacon and its listener use. Links are accepted on the same number over
// TCP; only when another program holds it does a node fall back to a port the
// system picks, which its beacon then advertises.
QString multicastGroupV4();
int discoveryPort();

// True only for an address that cannot be routed off the local network:
// loopback, RFC 1918 private IPv4, RFC 3927 IPv4 link-local, IPv6 link-local
// (fe80::/10) and IPv6 unique-local (fc00::/7). Carrier-grade NAT space
// (100.64/10) is deliberately excluded: it belongs to an ISP, not to a LAN.
bool isLocalNetworkAddress(const QHostAddress &address);

// The beacon a node multicasts, and the reverse. parseBeacon() returns false
// for anything that is not a well-formed beacon of a version we speak.
//
// A `reply` beacon is the unicast answer to a beacon heard from a device this
// node is not linked to. It reaches a sender whose own multicast never arrives
// here — an iPhone without Apple's multicast entitlement, or a machine whose
// firewall drops unsolicited inbound datagrams but passes the answer to one it
// sent — and is never itself answered, so two nodes cannot ping-pong.
struct Beacon {
    QString kid;
    quint16 port = 0;
    qint64 timestampMs = 0;
    bool reply = false;
    bool control = false;
    QString node;
};
QJsonObject beaconObject(const Beacon &beacon);
bool parseBeacon(const QJsonObject &object, Beacon *out);

// The hosts of the subnet `ip` sits on that a unicast beacon sweep covers:
// every host of a /24 or smaller subnet, or of the /24 around `ip` on a larger
// one, minus `ip` itself. Empty for anything but a private IPv4 address, so a
// sweep never leaves the local network.
QList<QHostAddress> sweepAddresses(const QHostAddress &ip, int prefixLength);

// The RFCOMM service the Bluetooth transport advertises and looks for. Fixed
// for every ForkMesh install, which is what lets two devices find each other
// with nothing configured.
QString bluetoothServiceUuid();
QString bluetoothServiceName();

// Link framing: 4-byte big-endian length, then that many bytes of JSON.
// Frames larger than the cap are refused rather than buffered.
qsizetype maxFrameBytes();
QByteArray frameBytes(const QByteArray &payload);
// Removes one complete frame from the front of `buffer`. Returns false when
// the buffer does not hold one yet; sets *overflow when the declared length is
// impossible, which the caller must treat as a protocol error.
bool takeFrame(QByteArray &buffer, QByteArray *payload, bool *overflow);

qint64 wakeTokenExpiry(const QString &token);

} // namespace forkmesh::localmesh

class LocalMeshNode : public QObject
{
    Q_OBJECT
public:
    LocalMeshNode(const QString &userName, const QString &nodeName,
                const QString &ownerUser, const QString &stableNodeId,
                QObject *parent = nullptr);
    ~LocalMeshNode() override;

    // Opens the listening socket and starts beaconing. Returns false when the
    // device has no local network to mesh over, or encryption is unavailable.
    bool start();
    void stop();
    bool isRunning() const { return m_running; }

    void setUserName(const QString &name);
    void setNodeIdentity(const QString &nodeName, const QString &ownerUser);
    void setAccountKind(const QString &kind);

    // Self first, then one entry per device currently linked on this network.
    QList<MemberInfo> members() const;
    int peerCount() const;
    QString sealKeyId() const;
    quint16 listenPort() const;

    // Where the room's recent history — and with it every message still
    // waiting for a device to take it — is kept across restarts. Set before
    // start(); empty (the default) keeps history in memory only.
    void setHistoryFile(const QString &path);

    // Returns the new message's id, or an empty string when nothing was sent.
    // A message sent while no device is linked stays queued here, and goes to
    // the first device that joins with the rest of the room's history.
    QString sendChat(const QString &text);
    QString sendFile(const QString &fileName, const QString &mimeType,
                     const QByteArray &data);
    void sendTyping(bool active);
    QList<forkmesh::chatv3::MeshLinkedDevice> linkedDevices() const;
    bool sendDirectSealed(const QString &kid, const QString &channel,
                          const QJsonObject &frame);
    void setDirectFocus(const QString &channel, bool on);
    using DirectHandler = std::function<QString(
        const QString &kid, const QString &channel, const QJsonObject &frame)>;
    void setDirectHandler(DirectHandler handler);
    void setChatDevice(std::shared_ptr<forkmesh::chatv3::ChatDevice> device);
    void setControlNode(const QString &nodeKey);
    // True while a message this device wrote has not reached any device yet.
    bool isQueued(const QString &messageId) const
    {
        return m_queued.contains(messageId);
    }
    QStringList wakeTargets() const;

    // Hands over an inbound link whose first frame opens a local control
    // channel (see LocalControl.h) instead of joining the room: the socket,
    // that frame, and whatever bytes followed it. While a handler is set the
    // beacon advertises the channel; without one such a link is dropped.
    using ControlHandler = std::function<void(QTcpSocket *socket,
                                              const QByteArray &hello,
                                              const QByteArray &rest)>;
    void setControlHandler(ControlHandler handler);

    // Direct link to a peer this device already knows how to reach, for a
    // network that filters multicast. The address must be a local one.
    bool connectToPeer(const QHostAddress &address, quint16 port);

    // Beacons and sweeps the subnet now instead of on the next beat: for the
    // moment a firewall has just been opened, or a network just joined.
    void announceNow();

    // True while a device keeps being heard but no link to it can be made in
    // either direction — what a firewall dropping inbound traffic looks like.
    bool linksBlocked() const { return m_linksBlocked; }

    // True once the Bluetooth transport is listening for peers. False when the
    // build has no Bluetooth module, the machine has no adapter, or it is off.
    bool bluetoothActive() const;

    // What the room's transports have done since start(), for the debug bar
    // in the #mesh room: one compact line, and a fuller breakdown with one
    // row per device heard, for a tooltip or a bug report.
    QString diagnosticsLine() const;
    QString diagnosticsDetail() const;

#ifdef FORKMESH_LOCAL_MESH_TESTS
    // Keeps a test's beacons and links off the real network's ports.
    void setDiscoveryPortForTests(int port);
    void setBeaconIntervalForTests(int intervalMs);
    void setPeerTimeoutForTests(int timeoutMs);
    void setLinkReplaceAfterForTests(int ageMs);
    void setMulticastEnabledForTests(bool enabled);
    // Keeps a test's beacons off the rest of the machine's subnet.
    void setAddressSweepEnabledForTests(bool enabled);
    // Keeps a test off the machine's Bluetooth stack (and off DBus).
    void setBluetoothEnabledForTests(bool enabled);
#endif

signals:
    void messageArrived(const ChatMessage &message);
    void directAcked(const QString &kid, const QString &channel,
                     const QString &messageId);
    void peerFocusChanged(const QString &kid, const QString &channel, bool on);
    void linkedDevicesChanged();
    // A message of this device's became queued (no device to take it) or was
    // handed to a device.
    void queuedChanged(const QString &messageId, bool queued);
    void wakeRequested(const QStringList &tokens);
    void typingChanged(const QString &conversation, const QString &peerId,
                       const QString &peerName, bool active);
    // The membership of the room changed: a device appeared, left, or renamed.
    void peersChanged();
    void systemMessage(const QString &text);
    // linksBlocked() turned true. Emitted once per episode; the episode ends
    // with the next link that does open.
    void linksBlockedDetected();
    // A device on the network opened a link to this one, which is proof that
    // nothing here is dropping the mesh's inbound traffic.
    void inboundLinkAccepted();

private:
    struct Link {
        enum class Stage { Hello, Auth, Open };
        QIODevice *socket = nullptr;
        QByteArray buffer;
        Stage stage = Stage::Hello;
        QString peerId;
        bool outbound = false;
        bool bluetooth = false;
        QString btKey;
        qint64 helloMs = 0;
        QByteArray ephPriv;
        QJsonObject ownHello;
        QJsonObject peerHello;
        QByteArray transcript;
        QByteArray peerIdk;
        QString peerKid;
        QByteArray sendKey;
        QByteArray recvKey;
        quint64 sendCounter = 0;
        quint64 recvCounter = 0;
        bool bundleKnown = false;
        QQueue<QJsonObject> pending;
        QSet<QString> focus;
    };

    struct Peer {
        QString nodeId;
        QString name;
        QString nodeName;
        QString ownerUser;
        QString accountKind;
        QString platform;
        QString version;
        QHostAddress address;
        quint16 port = 0;
        qint64 lastSeenMs = 0;
        // When a beacon first arrived from this peer with no link to it, or 0.
        // The higher id dials once this is a while ago: the lower id's own
        // dial is evidently not getting through.
        qint64 unlinkedSinceMs = 0;
        bool linked = false;
        bool bluetooth = false; // reached over Bluetooth rather than the LAN
    };


    // Counters behind diagnosticsLine(). Reset by start().
    struct Diagnostics {
        int multicastSent = 0;
        int broadcastSent = 0;
        int unicastSent = 0; // to peers already heard, and replies
        int sweepSent = 0;
        int sendErrors = 0;
        QString lastSendError;
        int datagramsIn = 0;
        int ownBeaconsIn = 0; // our multicast/broadcast looped back
        int offNetworkIn = 0;
        int undecryptableIn = 0; // not JSON, another room key, or not a beacon
        int peerBeaconsIn = 0;
        QString lastBeaconFrom;
        qint64 lastBeaconMs = 0;
        int dials = 0;
        int dialsOk = 0;
        int dialsFailed = 0;
        QString lastDialError;
        int inboundAccepted = 0;
        int inboundRefused = 0;
        int framesIn = 0;
        int framesUndecryptable = 0;
        int framesUnsealable = 0;
        int chatIn = 0;
        int chatOut = 0;
        int directIn = 0;
        int directOut = 0;
        QString lastDropReason;
        QString discoveryError;
        QString listenError;
        int bleSeen = 0;
        int bleDials = 0;
        int bleOpened = 0;
        QString lastBleError;
    };

    bool openListener();
    bool openDiscovery();
    bool openNetwork();
    void refreshNetwork();
    void redialPeer(const QString &peerId);
    // Writes one discovery datagram and counts it under `counter`, or as a
    // send error with the socket's reason.
    bool writeDiscovery(const QByteArray &datagram, const QHostAddress &to,
                        quint16 port, int &counter);
    // Returns true once the Bluetooth transport is listening. Never blocks:
    // the adapter probe that can stall for a DBus timeout runs on a worker.
    bool openBluetooth();
    void closeBluetooth();
    void restartBluetoothDiscovery();
#ifdef FORKMESH_HAS_BLUETOOTH
    void onBluetoothConnection();
    void onBluetoothService(const QBluetoothServiceInfo &service);
    void startBleScan();
    void onBleDevice(const QBluetoothDeviceInfo &device);
    bool bluetoothDialAllowed(const QString &key) const;
    // An inquiry only finds an adapter that answers one, and BlueZ keeps an
    // adapter hidden unless asked — and hides it again after a few minutes.
    // Two devices that were never paired find each other only while this
    // holds. An adapter the user switched off is left off.
    void keepBluetoothDiscoverable();
#endif
    void joinMulticastGroups();
    void sendBeacon();
    QByteArray beaconDatagram(bool reply) const;
    // Unicast beacons for a network where multicast does not arrive: to every
    // unlinked peer already heard from, and every few beats to the whole
    // local subnet.
    void sendUnicastBeacons();
    // Sends the queued subnet sweep a batch at a time. A datagram to a host
    // whose hardware address is still being resolved stays charged to the
    // socket until resolution gives up, seconds later, so a whole /24 at once
    // fills the send buffer two thirds of the way through: every host above
    // about .168 — and any reply beacon sent meanwhile — was silently lost.
    void drainSweep();
    void onDatagrams();
    void onNewConnection();
    void attachLink(QIODevice *socket, bool outbound, bool bluetooth);
    void onLinkReadyRead(QIODevice *socket);
    void dropLink(QIODevice *socket, const QString &reason);
    void sweepPeers();

    QJsonObject makeMessage(const QString &type) const;
    bool writeRaw(Link &link, const QJsonObject &frame);
    bool writeLink(Link &link, const QJsonObject &frame);
    bool writeSealedTo(Link &link, const QJsonObject &plain);
    void sendToAll(const QJsonObject &plain, bool store = false);
    QStringList sessionKeyIds() const;
    void clearQueued(const QString &id);
    void handleLinkHello(Link &link, const QByteArray &payload);
    void handleLinkAuth(Link &link, const QByteArray &payload);
    void onLinkAuthenticated(Link &link);
    void handleLinkFrame(Link &link, const QJsonObject &frame);
    void handleBundle(Link &link, const QJsonObject &bundle);
    void handleSealed(Link &link, const QJsonObject &frame);
    void handleEnvelope(Link &link, const QJsonObject &plain,
                        const QString &senderKid, const QJsonObject &frame);
    void forwardFrame(Link *from, const QJsonObject &frame);
    bool rememberForwarded(const QString &sig);
    void resealOwnHistory(Link &link, const QString &kid);
    void flushLinkPending(Link &link);
    void attachDevice(std::shared_ptr<forkmesh::chatv3::ChatDevice> device);
    void onDeviceEvent(forkmesh::chatv3::ChatDevice::Event event);
    // Sends a message this device wrote, queueing it when nobody is linked.
    QString sendOwn(QJsonObject message);
    void loadHistory();
    void scheduleHistorySave();
    void saveHistory();
    void sendHistoryTo(Link &link);
    void rememberPeer(const QString &kid, const QJsonObject &plain,
                      bool linked, bool bluetooth);
    void rememberWake(const QString &senderId, const QJsonObject &plain);
    Link *linkFor(QIODevice *socket);
    Link *linkForPeer(const QString &kid, const Link *except = nullptr);
    const Link *openLinkFor(const QString &kid) const;
    void storeHistory(const QJsonObject &plain);
    bool markSeen(const QString &id);
    void emitChat(const QJsonObject &plain, bool historical = false);
    void noteMembershipChange();

    QString m_userName;
    QString m_nodeName;
    QString m_ownerUser;
    QString m_accountKind = QStringLiteral("node");
    QString m_nodeId;
    QString m_platform;
    QString m_version;

    RoomCrypto m_crypto;
    RoomSeal m_seal;
    std::shared_ptr<forkmesh::chatv3::ChatDevice> m_device;
    int m_deviceToken = 0;
    QHash<QString, QJsonObject> m_bundles;
    QStringList m_bundleOrder;
    QSet<QString> m_forwarded;
    QQueue<QString> m_forwardedOrder;
    QHash<QString, QJsonObject> m_historyFrames;
    QSet<QString> m_ownFocus;
    DirectHandler m_directHandler;
    QString m_controlNode;

    QTcpServer *m_server = nullptr;
    QUdpSocket *m_discovery = nullptr;
#ifdef FORKMESH_HAS_BLUETOOTH
    // Asking Qt for the local adapter costs a full DBus timeout — 25 seconds
    // on a machine with no BlueZ — so the answer is found once, off the GUI
    // thread, and remembered.
    enum class Adapter { Unknown, Probing, Absent, Present };
    QBluetoothLocalDevice *m_btLocal = nullptr;
    QBluetoothServer *m_btServer = nullptr;
    QBluetoothServiceInfo *m_btService = nullptr;
    QBluetoothServiceDiscoveryAgent *m_btDiscovery = nullptr;
    QTimer *m_btRescanTimer = nullptr;
    QSet<QString> m_btDialing; // device addresses with an attempt in flight
    QBluetoothDeviceDiscoveryAgent *m_bleDiscovery = nullptr;
    Adapter m_btAdapter = Adapter::Unknown;
    qint64 m_btAdapterCheckedMs = 0; // when Absent was last concluded
    bool m_btListening = false;
#endif
    bool m_bluetoothEnabled = true;
    QHash<QString, qint64> m_btQuietUntil;
    QTimer *m_beaconTimer = nullptr;
    QTimer *m_sweepTimer = nullptr;
    QTimer *m_sweepPaceTimer = nullptr;
    QTimer *m_typingStopTimer = nullptr;

    QList<Link *> m_links;
    QHash<QString, Peer> m_peers;
    QHash<QString, QString> m_wakeTokens;
    QStringList m_wakeOrder;
    QList<QJsonObject> m_history; // recent mesh frames, replayed to joiners
    qsizetype m_historyChars = 0;
    QSet<QString> m_queued; // own message ids no device has taken yet
    QString m_historyPath;
    bool m_historyLoaded = false;
    QTimer *m_historySaveTimer = nullptr;
    QSet<QString> m_seenIds;
    QQueue<QString> m_seenOrder;
    QSet<QString> m_dialing; // "address:port" of in-flight outbound attempts
    QList<QHostAddress> m_sweepQueue; // subnet hosts still to be beaconed
    QByteArray m_sweepDatagram;       // the beacon the queued sweep sends
    int m_sweepHeadTries = 0;         // failed sends to m_sweepQueue.first()
    QString m_addressSignature;

    int m_discoveryPort;
    int m_beaconIntervalMs;
    int m_peerTimeoutMs;
    qint64 m_linkReplaceAfterMs;
    bool m_multicastEnabled = true;
    bool m_addressSweepEnabled = true;
    int m_beaconCount = 0;
    bool m_linksBlocked = false;
    bool m_inboundLinkSeen = false;
    bool m_running = false;
    bool m_typingActive = false;
    Diagnostics m_diag;
    qint64 m_startedMs = 0;
    ControlHandler m_controlHandler;
};
