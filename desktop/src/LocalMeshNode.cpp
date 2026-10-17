#include "ForkMeshVersion.h"
#include "ChatHistoryLimits.h"
#include "BleMeshLink.h"
#include "LocalMeshNode.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSaveFile>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>
#include <QUuid>

#ifdef FORKMESH_HAS_BLUETOOTH
#include <QtConcurrent/QtConcurrentRun>
#include <QFutureWatcher>
#include <QBluetoothDeviceDiscoveryAgent>
#include <QBluetoothDeviceInfo>
#include <QBluetoothLocalDevice>
#include <QBluetoothServer>
#include <QBluetoothServiceDiscoveryAgent>
#include <QBluetoothServiceInfo>
#include <QBluetoothSocket>
#include <QBluetoothUuid>
#endif

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#endif

#include <utility>

#ifndef FORKMESH_VERSION
#define FORKMESH_VERSION "dev"
#endif

namespace {

constexpr int kProtocolVersion = 3;
constexpr int kDefaultDiscoveryPort = 48732;

// The first frame of a local control channel (LocalControl.h): plain JSON,
// where every room frame is a RoomCrypto envelope.
bool isControlChannelHello(const QByteArray &payload)
{
    if (!payload.contains("\"fm-control-hello\""))
        return false;
    return QJsonDocument::fromJson(payload)
               .object()
               .value(QStringLiteral("t"))
               .toString() == QLatin1String("fm-control-hello");
}
constexpr int kBeaconIntervalMs = 15000;
constexpr int kPeerTimeoutMs = 60000;   // 4 missed beacons
constexpr int kSweepIntervalMs = 5000;
// Unicast beacons go to the whole local subnet only every few beats: a /24 is
// 253 datagrams, and a peer found that way is then beaconed on its own.
constexpr int kAddressSweepEveryBeats = 4;
// With nobody linked the sweep runs twice as often. Two machines that both
// drop unsolicited datagrams only meet when each sweeps while the other's
// sweep is still fresh in its firewall's connection table, and an unanswered
// UDP entry lives for about thirty seconds.
constexpr int kAddressSweepEveryBeatsAlone = 2;
// A sweep goes out this many hosts at a time, this far apart: a /24 in about
// eight seconds, with under a hundred datagrams ever awaiting resolution.
constexpr int kSweepBatch = 16;
constexpr int kSweepBatchGapMs = 500;
// A host the socket refuses this many times running is skipped, so one bad
// address cannot stall the rest of the sweep.
constexpr int kSweepHeadMaxTries = 4;
// A peer heard for this many beats with no link in either direction means
// something is dropping the links, not that a dial is still in flight.
constexpr int kLinksBlockedAfterBeats = 3;
// A Bluetooth inquiry is slow and power-hungry compared with a multicast
// beacon, so it runs on its own, much longer beat.
constexpr int kBluetoothRescanMs = 120000;
// How long a "no adapter here" verdict stands before it is worth another
// look, for a dongle plugged in mid-session.
constexpr qint64 kBluetoothReprobeMs = 300000;
constexpr int kBleScanMs = 15000;
constexpr qint64 kBluetoothQuietMs = 600000;
constexpr int kBluetoothRedialMs = 5000;
constexpr int kDialTimeoutMs = 8000;
constexpr int kRedialDelayMs = 1500;
constexpr qint64 kRedialMinLinkMs = 5000;
constexpr qint64 kLinkReplaceAfterMs = 10000;
constexpr int kKeepAliveIdleSec = 10;
constexpr int kKeepAliveIntervalSec = 5;
constexpr int kKeepAliveProbes = 3;
constexpr int kUnackedTimeoutMs = 30000;
constexpr int kTypingStopMs = 4000;
// History is written this long after the last change, and at stop().
constexpr int kHistorySaveDelayMs = 1000;
constexpr int kHistoryFileVersion = 1;
constexpr int kSeenCacheLimit = 4096;
constexpr int kMaxLinks = 64;
constexpr int kMaxPendingFrames = 32;
constexpr int kMaxDisplayNameChars = 32;
constexpr int kMaxVersionChars = 32;
constexpr int kMaxWakeTokenChars = 200;
constexpr int kMaxWakeTokens = 32;
constexpr int kMaxTextChars = 16000;
constexpr int kMaxFileNameChars = 180;
constexpr int kMaxMimeChars = 100;
constexpr qsizetype kMaxFileBytes = 16ll * 1024 * 1024;
constexpr qsizetype kMaxBase64FileChars = 22ll * 1024 * 1024;
constexpr qsizetype kMaxFrameBytes = 24ll * 1024 * 1024;
const char kBeaconTag[] = "forkmesh-lan-mesh";
constexpr int kMaxSessionRecipients = 63;
constexpr int kForwardedCacheLimit = 4096;
constexpr int kMaxNodeKeyChars = 200;

bool validKid(const QString &kid)
{
    static const QRegularExpression pattern(QStringLiteral("\\A[A-Za-z0-9_-]{16}\\z"));
    return pattern.match(kid).hasMatch();
}

QJsonObject parseObject(const QByteArray &payload)
{
    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(payload, &error);
    return error.error == QJsonParseError::NoError && document.isObject()
               ? document.object()
               : QJsonObject();
}

QString currentPlatform()
{
#if defined(Q_OS_ANDROID)
    return QStringLiteral("android");
#elif defined(Q_OS_IOS)
    return QStringLiteral("ios");
#elif defined(Q_OS_MACOS)
    return QStringLiteral("macos");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows");
#elif defined(Q_OS_LINUX)
    return QStringLiteral("linux");
#else
    return QStringLiteral("unknown");
#endif
}

QString boundedText(const QJsonObject &object, const char *key, int maxChars)
{
    return object.value(QLatin1String(key)).toString().left(maxChars);
}

QString safeFileName(const QString &name)
{
    QString cleaned = name;
    cleaned.replace('\\', '/');
    cleaned = cleaned.section('/', -1).trimmed();
    cleaned.remove(QChar(u'\0'));
    if (cleaned.isEmpty())
        cleaned = QStringLiteral("file");
    return cleaned.left(kMaxFileNameChars);
}

QByteArray boundedBase64(const QJsonObject &object, const char *key)
{
    const QString encoded = object.value(QLatin1String(key)).toString();
    if (encoded.isEmpty() || encoded.size() > kMaxBase64FileChars)
        return {};
    const QByteArray decoded = QByteArray::fromBase64(encoded.toLatin1());
    return decoded.size() > kMaxFileBytes ? QByteArray() : decoded;
}

// Every interface that is up, can carry multicast, and holds at least one
// address that cannot be routed off this network. A machine with a public
// address on one NIC still meshes over its private NICs, and never beacons
// out of the public one.
QList<QNetworkInterface> meshInterfaces()
{
    QList<QNetworkInterface> usable;
    const QList<QNetworkInterface> all = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &iface : all) {
        const QNetworkInterface::InterfaceFlags flags = iface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) ||
            !flags.testFlag(QNetworkInterface::IsRunning) ||
            !flags.testFlag(QNetworkInterface::CanMulticast))
            continue;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            if (forkmesh::localmesh::isLocalNetworkAddress(entry.ip())) {
                usable.append(iface);
                break;
            }
        }
    }
    return usable;
}

QString meshAddressSignature()
{
    QStringList parts;
    const QList<QNetworkInterface> interfaces = meshInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries)
            if (forkmesh::localmesh::isLocalNetworkAddress(entry.ip()))
                parts << iface.name() + QLatin1Char(' ') +
                             entry.ip().toString();
    }
    parts.sort();
    return parts.join(QLatin1Char(','));
}

void setSocketInt(qintptr fd, int level, int option, int value)
{
#ifdef _WIN32
    ::setsockopt(SOCKET(fd), level, option,
                 reinterpret_cast<const char *>(&value), int(sizeof(value)));
#else
    ::setsockopt(int(fd), level, option, &value, socklen_t(sizeof(value)));
#endif
}

void enableLinkKeepAlive(QTcpSocket *socket)
{
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);
    const qintptr fd = socket->socketDescriptor();
    if (fd == -1)
        return;
#if defined(TCP_KEEPIDLE)
    setSocketInt(fd, IPPROTO_TCP, TCP_KEEPIDLE, kKeepAliveIdleSec);
#elif defined(TCP_KEEPALIVE)
    setSocketInt(fd, IPPROTO_TCP, TCP_KEEPALIVE, kKeepAliveIdleSec);
#endif
#if defined(TCP_KEEPINTVL)
    setSocketInt(fd, IPPROTO_TCP, TCP_KEEPINTVL, kKeepAliveIntervalSec);
#endif
#if defined(TCP_KEEPCNT)
    setSocketInt(fd, IPPROTO_TCP, TCP_KEEPCNT, kKeepAliveProbes);
#endif
#if defined(TCP_USER_TIMEOUT)
    setSocketInt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT, kUnackedTimeoutMs);
#endif
}

#ifdef FORKMESH_HAS_BLUETOOTH
// Rules a machine out without touching DBus at all. The kernel registers
// every Bluetooth adapter under /sys/class/bluetooth, so an empty or missing
// directory means there is nothing to probe and the 25-second DBus timeout is
// never paid. Other platforms answer through their own non-blocking backends.
bool bluetoothAdapterPossible()
{
#if defined(Q_OS_LINUX)
    QDir adapters(QStringLiteral("/sys/class/bluetooth"));
    return adapters.exists() &&
           !adapters
                .entryList(QDir::Dirs | QDir::Files | QDir::System |
                           QDir::NoDotAndDotDot)
                .isEmpty();
#else
    return true;
#endif
}
#endif // FORKMESH_HAS_BLUETOOTH

// The interfaces worth sweeping with unicast beacons: real Ethernet and Wi-Fi
// segments where other devices live. Container and VM bridges only hold this
// machine's own guests, and a VPN's private range belongs to another network.
bool sweepableInterface(const QNetworkInterface &iface)
{
    if (iface.type() != QNetworkInterface::Ethernet &&
        iface.type() != QNetworkInterface::Wifi)
        return false;
    static const char *const kBridgePrefixes[] = {
        "docker", "br-", "veth", "virbr", "vboxnet", "vmnet", "lxcbr", "lxdbr",
        "cni", "podman", "flannel"};
    const QString name = iface.name();
    for (const char *prefix : kBridgePrefixes)
        if (name.startsWith(QLatin1String(prefix)))
            return false;
    return true;
}

QString dialKey(const QHostAddress &address, quint16 port)
{
    return address.toString() + QLatin1Char(':') + QString::number(port);
}

} // namespace

namespace forkmesh::localmesh {

QString conversation()
{
    return QStringLiteral("#mesh");
}

bool isMeshConversation(const QString &candidate)
{
    return candidate == conversation();
}

QString roomName()
{
    return QStringLiteral("forkmesh-lan-mesh-v1");
}

QString multicastGroupV4()
{
    return QStringLiteral("239.192.79.42");
}

int discoveryPort()
{
    return kDefaultDiscoveryPort;
}

QString bluetoothServiceUuid()
{
    return QStringLiteral("7f9e5c10-4d3b-4a21-9c6f-2b7ad1e05f11");
}

QString bluetoothServiceName()
{
    return QStringLiteral("ForkMesh local mesh");
}

bool isLocalNetworkAddress(const QHostAddress &address)
{
    if (address.isNull())
        return false;
    QHostAddress plain(address);
    plain.setScopeId(QString());
    bool isIPv4 = false;
    // Also converts an IPv4-mapped IPv6 address, which is what a dual-stack
    // listener reports for an ordinary IPv4 peer.
    const quint32 v4 = plain.toIPv4Address(&isIPv4);
    if (isIPv4) {
        return (v4 & 0xff000000u) == 0x7f000000u || // 127/8 loopback
               (v4 & 0xff000000u) == 0x0a000000u || // 10/8
               (v4 & 0xfff00000u) == 0xac100000u || // 172.16/12
               (v4 & 0xffff0000u) == 0xc0a80000u || // 192.168/16
               (v4 & 0xffff0000u) == 0xa9fe0000u;   // 169.254/16 link-local
    }
    if (plain.protocol() != QAbstractSocket::IPv6Protocol)
        return false;
    if (plain.isLoopback())
        return true;
    const Q_IPV6ADDR raw = plain.toIPv6Address();
    if ((raw[0] & 0xfe) == 0xfc) // fc00::/7 unique-local
        return true;
    return raw[0] == 0xfe && (raw[1] & 0xc0) == 0x80; // fe80::/10 link-local
}

QJsonObject beaconObject(const Beacon &beacon)
{
    QJsonObject object{
        {QStringLiteral("t"), QLatin1String(kBeaconTag)},
        {QStringLiteral("v"), kProtocolVersion},
        {QStringLiteral("id"), beacon.kid},
        {QStringLiteral("port"), int(beacon.port)},
        {QStringLiteral("ts"), double(beacon.timestampMs)}};
    if (beacon.reply)
        object.insert(QStringLiteral("reply"), true);
    if (beacon.control) {
        object.insert(QStringLiteral("control"), true);
        if (!beacon.node.isEmpty())
            object.insert(QStringLiteral("node"), beacon.node);
    }
    return object;
}

bool parseBeacon(const QJsonObject &object, Beacon *out)
{
    if (!out)
        return false;
    if (object.value(QStringLiteral("t")).toString() !=
            QLatin1String(kBeaconTag) ||
        object.value(QStringLiteral("v")).toInt() != kProtocolVersion)
        return false;
    const QString kid = object.value(QStringLiteral("id")).toString();
    const int port = object.value(QStringLiteral("port")).toInt();
    if (!validKid(kid) || port <= 0 || port > 65535)
        return false;
    out->kid = kid;
    out->port = quint16(port);
    out->timestampMs = qint64(object.value(QStringLiteral("ts")).toDouble());
    out->reply = object.value(QStringLiteral("reply")).toBool();
    out->control = object.value(QStringLiteral("control")).toBool();
    out->node = out->control
                    ? object.value(QStringLiteral("node")).toString().left(kMaxNodeKeyChars)
                    : QString();
    return true;
}

QList<QHostAddress> sweepAddresses(const QHostAddress &ip, int prefixLength)
{
    QList<QHostAddress> hosts;
    bool isIPv4 = false;
    const quint32 self = ip.toIPv4Address(&isIPv4);
    if (!isIPv4 || prefixLength > 30 || ip.isLoopback() ||
        !isLocalNetworkAddress(ip) || (self & 0xffff0000u) == 0xa9fe0000u)
        return hosts;
    // An unknown prefix (-1) or a wide one is swept as the /24 around ip.
    const int hostBits = 32 - qBound(24, prefixLength, 30);
    const quint32 network = self & (0xffffffffu << hostBits);
    const quint32 count = (1u << hostBits) - 1; // the last is the broadcast
    hosts.reserve(int(count));
    for (quint32 host = 1; host < count; ++host)
        if (network + host != self)
            hosts.append(QHostAddress(network + host));
    return hosts;
}

qsizetype maxFrameBytes()
{
    return kMaxFrameBytes;
}

QByteArray frameBytes(const QByteArray &payload)
{
    const quint32 length = quint32(payload.size());
    QByteArray framed;
    framed.reserve(payload.size() + 4);
    framed.append(char((length >> 24) & 0xff));
    framed.append(char((length >> 16) & 0xff));
    framed.append(char((length >> 8) & 0xff));
    framed.append(char(length & 0xff));
    framed.append(payload);
    return framed;
}

bool takeFrame(QByteArray &buffer, QByteArray *payload, bool *overflow)
{
    if (overflow)
        *overflow = false;
    if (!payload || buffer.size() < 4)
        return false;
    const quint32 length = (quint32(quint8(buffer[0])) << 24) |
                           (quint32(quint8(buffer[1])) << 16) |
                           (quint32(quint8(buffer[2])) << 8) |
                           quint32(quint8(buffer[3]));
    if (length == 0 || qsizetype(length) > kMaxFrameBytes) {
        if (overflow)
            *overflow = true;
        return false;
    }
    if (buffer.size() < qsizetype(length) + 4)
        return false;
    *payload = buffer.mid(4, qsizetype(length));
    buffer.remove(0, qsizetype(length) + 4);
    return true;
}

qint64 wakeTokenExpiry(const QString &token)
{
    if (token.isEmpty() || token.size() > kMaxWakeTokenChars)
        return 0;
    const QStringList parts = token.split(QLatin1Char('.'));
    if (parts.size() != 4 || parts.at(0) != QLatin1String("mw1"))
        return 0;
    bool ok = false;
    const qint64 expires = parts.at(2).toLongLong(&ok);
    return ok && expires > 0 ? expires : 0;
}

} // namespace forkmesh::localmesh

using namespace forkmesh::localmesh;

LocalMeshNode::LocalMeshNode(const QString &userName, const QString &nodeName,
                         const QString &ownerUser,
                         const QString &stableNodeId, QObject *parent)
    : QObject(parent),
      m_userName(userName.trimmed().left(kMaxDisplayNameChars)),
      m_nodeName(nodeName.trimmed().left(kMaxDisplayNameChars)),
      m_ownerUser(ownerUser.trimmed().left(kMaxDisplayNameChars)),
      m_nodeId(stableNodeId.trimmed().isEmpty()
                   ? QUuid::createUuid().toString(QUuid::WithoutBraces)
                   : stableNodeId.trimmed().left(160)),
      m_platform(currentPlatform()),
      m_version(QStringLiteral(FORKMESH_VERSION).left(kMaxVersionChars)),
      m_crypto(roomName()),
      m_discoveryPort(kDefaultDiscoveryPort),
      m_beaconIntervalMs(kBeaconIntervalMs),
      m_peerTimeoutMs(kPeerTimeoutMs),
      m_linkReplaceAfterMs(kLinkReplaceAfterMs)
{
#ifdef FORKMESH_LOCAL_MESH_TESTS
    attachDevice(forkmesh::chatv3::ChatDevice::sharedForTests(
        forkmesh::chatv3::sha256("forkmesh-mesh-test:" + m_nodeId.toUtf8())));
#else
    attachDevice(forkmesh::chatv3::ChatDevice::local());
#endif
}

LocalMeshNode::~LocalMeshNode()
{
    stop();
    if (m_device)
        m_device->unsubscribe(m_deviceToken);
}

void LocalMeshNode::attachDevice(std::shared_ptr<forkmesh::chatv3::ChatDevice> device)
{
    if (!device || device == m_device)
        return;
    if (m_device)
        m_device->unsubscribe(m_deviceToken);
    m_device = std::move(device);
    m_seal = RoomSeal(roomName(), roomName(), RoomSeal::Policy::Open, m_nodeId, m_nodeId,
                      m_device);
    m_deviceToken = m_device->subscribe(
        [this](forkmesh::chatv3::ChatDevice::Event event, const QString &,
               const QString &) { onDeviceEvent(event); });
}

void LocalMeshNode::setChatDevice(std::shared_ptr<forkmesh::chatv3::ChatDevice> device)
{
    if (m_running)
        return;
    attachDevice(std::move(device));
}

void LocalMeshNode::onDeviceEvent(forkmesh::chatv3::ChatDevice::Event event)
{
    if (event != forkmesh::chatv3::ChatDevice::Event::PrekeyRotated)
        return;
    QMetaObject::invokeMethod(
        this,
        [this] {
            const QJsonObject bundle = m_seal.keyFrame();
            if (bundle.isEmpty())
                return;
            for (Link *link : std::as_const(m_links))
                if (link->stage == Link::Stage::Open)
                    writeLink(*link, bundle);
        },
        Qt::QueuedConnection);
}

void LocalMeshNode::setControlNode(const QString &nodeKey)
{
    m_controlNode = nodeKey.trimmed().left(kMaxNodeKeyChars);
}

void LocalMeshNode::setDirectHandler(DirectHandler handler)
{
    m_directHandler = std::move(handler);
}

bool LocalMeshNode::start()
{
    if (m_running)
        return true;
    if (!m_crypto.isValid() || !m_seal.isValid()) {
        emit systemMessage(QStringLiteral(
            "Local mesh: encryption is unavailable, so the room stayed closed."));
        return false;
    }
    loadHistory();
    m_diag = Diagnostics();
    m_startedMs = QDateTime::currentMSecsSinceEpoch();
    m_running = true;
    m_addressSignature = meshAddressSignature();
    const bool lan = openNetwork();
    const bool bluetooth = openBluetooth();
    if (!m_beaconTimer) {
        m_beaconTimer = new QTimer(this);
        connect(m_beaconTimer, &QTimer::timeout, this, [this] {
            refreshNetwork();
            sendBeacon();
            sendUnicastBeacons();
            sweepPeers(); // also notices links that stay blocked, on the beat
            // Adapters get switched on, and devices walk into range, long
            // after startup.
            if (!bluetoothActive())
                openBluetooth();
        });
    }
    m_beaconTimer->setInterval(m_beaconIntervalMs);
    m_beaconTimer->start();
    if (!m_sweepTimer) {
        m_sweepTimer = new QTimer(this);
        m_sweepTimer->setInterval(kSweepIntervalMs);
        connect(m_sweepTimer, &QTimer::timeout, this, &LocalMeshNode::sweepPeers);
    }
    m_sweepTimer->start();
    m_beaconCount = 0;
    sendBeacon();
    sendUnicastBeacons();
    QStringList carried;
    if (lan)
        carried << QStringLiteral("this network");
    if (bluetooth)
        carried << QStringLiteral("Bluetooth");
    if (carried.isEmpty())
        emit systemMessage(
            QStringLiteral("Local mesh: %1 is waiting for a local network or "
                           "Bluetooth, and opens on its own when one appears.")
                .arg(conversation()));
    else
        emit systemMessage(
            QStringLiteral("Local mesh: %1 is open over %2, end-to-end "
                           "encrypted, and never leaves the room.")
                .arg(conversation(), carried.join(QStringLiteral(" and "))));
    return true;
}

bool LocalMeshNode::openNetwork()
{
    if (!m_running || meshInterfaces().isEmpty() || !openListener())
        return false;
    if (!openDiscovery())
        emit systemMessage(QStringLiteral(
            "Local mesh: network discovery could not open; listening only."));
    return true;
}

void LocalMeshNode::refreshNetwork()
{
    if (!m_running)
        return;
    const QString signature = meshAddressSignature();
    const bool changed = signature != m_addressSignature;
    m_addressSignature = signature;
    if (!m_server) {
        if (openNetwork()) {
            m_beaconCount = 0;
            emit systemMessage(QStringLiteral(
                "Local mesh: joined this network, looking for devices."));
        }
        return;
    }
    if (!m_discovery)
        openDiscovery();
    if (!changed)
        return;
    m_beaconCount = 0;
    const QList<QHostAddress> current = QNetworkInterface::allAddresses();
    QSet<QString> addresses;
    for (const QHostAddress &address : current) {
        QHostAddress plain(address);
        plain.setScopeId(QString());
        addresses.insert(plain.toString());
    }
    QList<QIODevice *> stranded;
    for (const Link *link : std::as_const(m_links)) {
        const auto *tcp = qobject_cast<const QTcpSocket *>(link->socket);
        if (!tcp)
            continue;
        QHostAddress local(tcp->localAddress());
        local.setScopeId(QString());
        bool isIPv4 = false;
        const quint32 v4 = local.toIPv4Address(&isIPv4);
        if (isIPv4)
            local = QHostAddress(v4);
        if (!local.isNull() && !addresses.contains(local.toString()))
            stranded.append(link->socket);
    }
    for (QIODevice *socket : std::as_const(stranded))
        dropLink(socket, QStringLiteral("network changed"));
}

void LocalMeshNode::stop()
{
    if (!m_running && !m_server && !m_discovery && !bluetoothActive())
        return;
    if (m_running && !m_links.isEmpty()) {
        // Tell the room before the sockets go, so a device that leaves
        // disappears now instead of on the next sweep.
        sendToAll(makeMessage(QStringLiteral("bye")));
        for (Link *link : std::as_const(m_links))
            if (link->socket)
                link->socket->waitForBytesWritten(50);
    }
    m_running = false;
    if (m_historySaveTimer && m_historySaveTimer->isActive()) {
        m_historySaveTimer->stop();
        saveHistory();
    }
    if (m_beaconTimer)
        m_beaconTimer->stop();
    if (m_sweepTimer)
        m_sweepTimer->stop();
    if (m_sweepPaceTimer)
        m_sweepPaceTimer->stop();
    m_sweepQueue.clear();
    m_sweepDatagram.clear();
    if (m_typingStopTimer)
        m_typingStopTimer->stop();
    const QList<Link *> links = m_links;
    m_links.clear();
    for (Link *link : links) {
        if (link->socket) {
            link->socket->disconnect(this);
            link->socket->close();
            link->socket->deleteLater();
        }
        delete link;
    }
    m_dialing.clear();
    for (auto it = m_peers.begin(); it != m_peers.end(); ++it)
        it->linked = false;
    if (m_discovery) {
        m_discovery->disconnect(this);
        m_discovery->deleteLater();
        m_discovery = nullptr;
    }
    if (m_server) {
        m_server->disconnect(this);
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    closeBluetooth();
    m_bundles.clear();
    m_bundleOrder.clear();
    m_forwarded.clear();
    m_forwardedOrder.clear();
    noteMembershipChange();
    emit linkedDevicesChanged();
}

// ---- Bluetooth transport ---------------------------------------------------
//
// The same room, carried by RFCOMM instead of TCP, for when there is no Wi-Fi
// and no internet at all. Everything above the socket — framing, the room key,
// the per-device seal, history, presence — is shared with the network
// transport, so the two are one room and a device on both bridges them.

bool LocalMeshNode::bluetoothActive() const
{
#ifdef FORKMESH_HAS_BLUETOOTH
    return m_btListening;
#else
    return false;
#endif
}

bool LocalMeshNode::openBluetooth()
{
#ifdef FORKMESH_HAS_BLUETOOTH
    if (m_btListening)
        return true;
    if (!m_running || !m_bluetoothEnabled)
        return false;
    if (m_btAdapter == Adapter::Probing)
        return false;
    if (m_btAdapter == Adapter::Absent) {
        // An adapter can be plugged in, or switched on, at any time — but
        // asking costs a DBus round trip, so ask again rarely.
        if (QDateTime::currentMSecsSinceEpoch() - m_btAdapterCheckedMs <
            kBluetoothReprobeMs)
            return false;
        m_btAdapter = Adapter::Unknown;
    }
    if (m_btAdapter == Adapter::Unknown) {
        if (!bluetoothAdapterPossible()) {
            m_btAdapter = Adapter::Absent;
            m_btAdapterCheckedMs = QDateTime::currentMSecsSinceEpoch();
            return false;
        }
        m_btAdapter = Adapter::Probing;
        // QBluetoothLocalDevice waits out the whole DBus timeout when BlueZ
        // is not answering. That must never happen on the thread drawing the
        // window, so the probe runs on a worker and the answer comes back as
        // a queued signal.
        auto *watcher = new QFutureWatcher<bool>(this);
        connect(watcher, &QFutureWatcher<bool>::finished, this,
                [this, watcher] {
                    const bool present = watcher->result();
                    watcher->deleteLater();
                    m_btAdapter = present ? Adapter::Present : Adapter::Absent;
                    m_btAdapterCheckedMs = QDateTime::currentMSecsSinceEpoch();
                    if (present)
                        openBluetooth();
                });
        watcher->setFuture(QtConcurrent::run([] {
            const QBluetoothLocalDevice adapter;
            return adapter.isValid() &&
                   adapter.hostMode() !=
                       QBluetoothLocalDevice::HostPoweredOff;
        }));
        return false;
    }

    if (!m_btServer) {
        m_btServer = new QBluetoothServer(QBluetoothServiceInfo::RfcommProtocol,
                                          this);
        connect(m_btServer, &QBluetoothServer::newConnection, this,
                &LocalMeshNode::onBluetoothConnection);
    }
    if (!m_btService) {
        // Advertises one SDP record under the ForkMesh UUID. That record is
        // the whole of discovery: a peer looks for this UUID and nothing else,
        // and the record carries no identity — names are exchanged inside the
        // encrypted hello, not in the service advertisement.
        auto *service = new QBluetoothServiceInfo(m_btServer->listen(
            QBluetoothUuid(bluetoothServiceUuid()), bluetoothServiceName()));
        if (!service->isRegistered()) {
            delete service;
            m_btAdapter = Adapter::Absent;
            m_btAdapterCheckedMs = QDateTime::currentMSecsSinceEpoch();
            return false;
        }
        m_btService = service;
    }
    m_btListening = true;
    restartBluetoothDiscovery();
    if (!m_btRescanTimer) {
        m_btRescanTimer = new QTimer(this);
        m_btRescanTimer->setInterval(kBluetoothRescanMs);
        connect(m_btRescanTimer, &QTimer::timeout, this,
                &LocalMeshNode::restartBluetoothDiscovery);
    }
    m_btRescanTimer->start();
    emit systemMessage(QStringLiteral(
        "Local mesh: Bluetooth is on and this device is visible to nearby "
        "ones, so the room keeps working with no Wi-Fi and no internet."));
    return true;
#else
    return false;
#endif
}

void LocalMeshNode::closeBluetooth()
{
#ifdef FORKMESH_HAS_BLUETOOTH
    m_btListening = false;
    m_btDialing.clear();
    if (m_btRescanTimer)
        m_btRescanTimer->stop();
    if (m_btDiscovery) {
        m_btDiscovery->stop();
        m_btDiscovery->disconnect(this);
        m_btDiscovery->deleteLater();
        m_btDiscovery = nullptr;
    }
    if (m_bleDiscovery) {
        m_bleDiscovery->stop();
        m_bleDiscovery->disconnect(this);
        m_bleDiscovery->deleteLater();
        m_bleDiscovery = nullptr;
    }
    if (m_btLocal) {
        m_btLocal->deleteLater();
        m_btLocal = nullptr;
    }
    if (m_btService) {
        m_btService->unregisterService();
        delete m_btService;
        m_btService = nullptr;
    }
    if (m_btServer) {
        m_btServer->disconnect(this);
        m_btServer->close();
        m_btServer->deleteLater();
        m_btServer = nullptr;
    }
#endif
}

void LocalMeshNode::restartBluetoothDiscovery()
{
#ifdef FORKMESH_HAS_BLUETOOTH
    if (!m_running || !m_btListening)
        return;
    keepBluetoothDiscoverable();
    if (!m_btDiscovery) {
        m_btDiscovery = new QBluetoothServiceDiscoveryAgent(this);
        // Only ForkMesh's own service: no general inventory of what else the
        // people around this device are running is collected or kept.
        m_btDiscovery->setUuidFilter(QBluetoothUuid(bluetoothServiceUuid()));
        connect(m_btDiscovery,
                &QBluetoothServiceDiscoveryAgent::serviceDiscovered, this,
                &LocalMeshNode::onBluetoothService);
        connect(m_btDiscovery, &QBluetoothServiceDiscoveryAgent::finished,
                this, &LocalMeshNode::startBleScan);
        connect(m_btDiscovery, &QBluetoothServiceDiscoveryAgent::errorOccurred,
                this, [this](QBluetoothServiceDiscoveryAgent::Error) {
                    startBleScan();
                });
    }
    if (m_btDiscovery->isActive() ||
        (m_bleDiscovery && m_bleDiscovery->isActive()))
        return;
    m_btDiscovery->start(QBluetoothServiceDiscoveryAgent::FullDiscovery);
#endif
}

#ifdef FORKMESH_HAS_BLUETOOTH
void LocalMeshNode::keepBluetoothDiscoverable()
{
    // Only ever built after the worker probe found an adapter that answers,
    // so this does not wait out a DBus timeout on the GUI thread.
    if (!m_btLocal)
        m_btLocal = new QBluetoothLocalDevice(this);
    if (m_btLocal->isValid() &&
        m_btLocal->hostMode() == QBluetoothLocalDevice::HostConnectable)
        m_btLocal->setHostMode(QBluetoothLocalDevice::HostDiscoverable);
}

void LocalMeshNode::onBluetoothConnection()
{
    while (m_btServer && m_btServer->hasPendingConnections()) {
        QBluetoothSocket *socket = m_btServer->nextPendingConnection();
        if (!socket)
            break;
        if (!m_running || m_links.size() >= kMaxLinks) {
            socket->close();
            socket->deleteLater();
            continue;
        }
        attachLink(socket, false, true);
        connect(socket, &QBluetoothSocket::disconnected, this,
                [this, socket] { dropLink(socket, QString()); });
        connect(socket, &QBluetoothSocket::errorOccurred, this,
                [this, socket](QBluetoothSocket::SocketError) {
                    dropLink(socket, socket->errorString());
                });
    }
}

void LocalMeshNode::onBluetoothService(const QBluetoothServiceInfo &service)
{
    if (!m_running || !m_btListening || m_links.size() >= kMaxLinks)
        return;
    const QString address = service.device().address().toString();
    if (address.isEmpty() || !bluetoothDialAllowed(address))
        return;
    m_btDialing.insert(address);
    auto *socket = new QBluetoothSocket(QBluetoothServiceInfo::RfcommProtocol,
                                        this);
    connect(socket, &QBluetoothSocket::connected, this,
            [this, socket, address] {
                m_btDialing.remove(address);
                attachLink(socket, true, true);
                if (Link *link = linkFor(socket))
                    link->btKey = address;
                connect(socket, &QBluetoothSocket::disconnected, this,
                        [this, socket] { dropLink(socket, QString()); });
            });
    connect(socket, &QBluetoothSocket::errorOccurred, this,
            [this, socket, address](QBluetoothSocket::SocketError) {
                m_btDialing.remove(address);
                if (linkFor(socket)) {
                    dropLink(socket, socket->errorString());
                    return;
                }
                socket->disconnect(this);
                socket->deleteLater();
            });
    socket->connectToService(service);
}

bool LocalMeshNode::bluetoothDialAllowed(const QString &key) const
{
    if (m_btDialing.contains(key))
        return false;
    const auto quiet = m_btQuietUntil.constFind(key);
    if (quiet != m_btQuietUntil.constEnd() &&
        QDateTime::currentMSecsSinceEpoch() < quiet.value())
        return false;
    for (const Link *link : m_links)
        if (link->btKey == key)
            return false;
    return true;
}

void LocalMeshNode::startBleScan()
{
    if (!m_running || !m_btListening)
        return;
    if (!m_bleDiscovery) {
        m_bleDiscovery = new QBluetoothDeviceDiscoveryAgent(this);
        m_bleDiscovery->setLowEnergyDiscoveryTimeout(kBleScanMs);
        connect(m_bleDiscovery,
                &QBluetoothDeviceDiscoveryAgent::deviceDiscovered, this,
                &LocalMeshNode::onBleDevice);
        connect(m_bleDiscovery, &QBluetoothDeviceDiscoveryAgent::errorOccurred,
                this, [this](QBluetoothDeviceDiscoveryAgent::Error) {
                    m_diag.lastBleError = m_bleDiscovery->errorString();
                });
    }
    if (m_bleDiscovery->isActive())
        return;
    m_bleDiscovery->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
}

void LocalMeshNode::onBleDevice(const QBluetoothDeviceInfo &device)
{
    if (!m_running || !m_btListening || m_links.size() >= kMaxLinks)
        return;
    if (!(device.coreConfigurations() &
          QBluetoothDeviceInfo::LowEnergyCoreConfiguration) ||
        !device.serviceUuids().contains(QBluetoothUuid(bleServiceUuid())))
        return;
    ++m_diag.bleSeen;
    const QString key = device.address().isNull()
                            ? device.deviceUuid().toString(QUuid::WithoutBraces)
                            : device.address().toString();
    if (key.isEmpty() || !bluetoothDialAllowed(key))
        return;
    m_btDialing.insert(key);
    ++m_diag.bleDials;
    auto *link = new BleMeshLink(device, key, this);
    link->onOpened = [this, link, key] {
        m_btDialing.remove(key);
        if (!m_running || m_links.size() >= kMaxLinks) {
            link->close();
            link->deleteLater();
            return;
        }
        ++m_diag.bleOpened;
        attachLink(link, true, true);
        if (Link *attached = linkFor(link))
            attached->btKey = key;
    };
    link->onClosed = [this, link, key](const QString &reason) {
        m_btDialing.remove(key);
        m_diag.lastBleError = reason;
        if (linkFor(link)) {
            dropLink(link, reason);
            return;
        }
        link->disconnect(this);
        link->deleteLater();
    };
    link->dial();
}
#endif

bool LocalMeshNode::openListener()
{
    if (m_server)
        return true;
    m_server = new QTcpServer(this);
    // Any address, because the mesh spans every local interface; every peer is
    // re-checked against isLocalNetworkAddress() on accept.
    //
    // The well-known port first. A firewall rule has to name a port, and a
    // port the system picks afresh on every launch can never be allowed
    // through one — so behind a host firewall no device could ever link in.
    // A second ForkMesh on the same machine finds it taken and takes any.
    if (!m_server->listen(QHostAddress::Any, quint16(m_discoveryPort)) &&
        !m_server->listen(QHostAddress::Any, 0)) {
        m_diag.listenError = m_server->errorString();
        emit systemMessage(
            QStringLiteral("Local mesh: could not listen for peers (%1).")
                .arg(m_server->errorString()));
        m_server->deleteLater();
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QTcpServer::newConnection, this,
            &LocalMeshNode::onNewConnection);
    return true;
}

bool LocalMeshNode::openDiscovery()
{
    if (m_discovery)
        return true;
    if (!m_multicastEnabled)
        return false;
    auto *socket = new QUdpSocket(this);
    if (!socket->bind(QHostAddress::AnyIPv4, quint16(m_discoveryPort),
                      QUdpSocket::ShareAddress |
                          QUdpSocket::ReuseAddressHint)) {
        m_diag.discoveryError = socket->errorString();
        socket->deleteLater();
        return false;
    }
    m_diag.discoveryError.clear();
    // TTL 1 is the hard stop that keeps a beacon on the local segment: no
    // router will forward it, whatever the group's scope allows.
    socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 1);
    socket->setSocketOption(QAbstractSocket::MulticastLoopbackOption, 1);
    m_discovery = socket;
    connect(socket, &QUdpSocket::readyRead, this, &LocalMeshNode::onDatagrams);
    joinMulticastGroups();
    return true;
}

bool LocalMeshNode::writeDiscovery(const QByteArray &datagram,
                                   const QHostAddress &to, quint16 port,
                                   int &counter)
{
    if (!m_discovery)
        return false;
    if (m_discovery->writeDatagram(datagram, to, port) < 0) {
        ++m_diag.sendErrors;
        m_diag.lastSendError =
            to.toString() + QStringLiteral(": ") + m_discovery->errorString();
        return false;
    }
    ++counter;
    return true;
}

void LocalMeshNode::joinMulticastGroups()
{
    if (!m_discovery)
        return;
    const QHostAddress group(multicastGroupV4());
    const QList<QNetworkInterface> interfaces = meshInterfaces();
    for (const QNetworkInterface &iface : interfaces)
        m_discovery->joinMulticastGroup(group, iface);
    m_discovery->joinMulticastGroup(group);
}

void LocalMeshNode::sendBeacon()
{
    if (!m_running || !m_server)
        return;
    joinMulticastGroups(); // interfaces come and go; re-joining is cheap
    const QByteArray datagram = beaconDatagram(false);
    if (datagram.isEmpty() || !m_discovery)
        return;
    const QHostAddress group(multicastGroupV4());
    const QList<QNetworkInterface> interfaces = meshInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        m_discovery->setMulticastInterface(iface);
        writeDiscovery(datagram, group, quint16(m_discoveryPort),
                       m_diag.multicastSent);
    }
    if (!m_addressSweepEnabled)
        return;
    // And to the subnet's broadcast address. Many access points drop
    // multicast between Wi-Fi clients, but none drops broadcast: ARP and DHCP
    // depend on it.
    for (const QNetworkInterface &iface : interfaces) {
        if (!sweepableInterface(iface))
            continue;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol ||
                entry.broadcast().isNull() || !isLocalNetworkAddress(entry.ip()))
                continue;
            writeDiscovery(datagram, entry.broadcast(), quint16(m_discoveryPort),
                           m_diag.broadcastSent);
        }
    }
}

QByteArray LocalMeshNode::beaconDatagram(bool reply) const
{
    if (!m_server)
        return {};
    Beacon beacon;
    beacon.kid = m_seal.keyId();
    beacon.port = m_server->serverPort();
    beacon.timestampMs = QDateTime::currentMSecsSinceEpoch();
    beacon.reply = reply;
    beacon.control = bool(m_controlHandler);
    beacon.node = beacon.control ? m_controlNode : QString();
    const QJsonObject envelope = m_crypto.encryptObject(beaconObject(beacon));
    if (envelope.isEmpty())
        return {};
    return QJsonDocument(envelope).toJson(QJsonDocument::Compact);
}

void LocalMeshNode::sendUnicastBeacons()
{
    if (!m_running || !m_server || !m_discovery)
        return;
    const QByteArray datagram = beaconDatagram(false);
    if (datagram.isEmpty())
        return;
    // Multicast is not the only way onto a network: iOS withholds it from
    // apps without a special entitlement, and a host firewall drops it
    // inbound. A unicast beacon from the discovery port gets an answer from
    // the same port, which such a firewall lets back in as a reply.
    QSet<QHostAddress> sent;
    for (auto it = m_peers.cbegin(); it != m_peers.cend(); ++it) {
        if (it->address.isNull() || linkForPeer(it.key()) ||
            sent.contains(it->address))
            continue;
        sent.insert(it->address);
        writeDiscovery(datagram, it->address, quint16(m_discoveryPort),
                       m_diag.unicastSent);
    }
    const int sweepEvery = m_links.isEmpty() ? kAddressSweepEveryBeatsAlone
                                             : kAddressSweepEveryBeats;
    if (!m_addressSweepEnabled || m_beaconCount++ % sweepEvery != 0)
        return;
    // A sweep still draining when the next one is due is replaced by it.
    m_sweepQueue.clear();
    m_sweepHeadTries = 0;
    const QList<QNetworkInterface> interfaces = meshInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        if (!sweepableInterface(iface))
            continue;
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QList<QHostAddress> hosts =
                sweepAddresses(entry.ip(), entry.prefixLength());
            for (const QHostAddress &host : hosts) {
                if (sent.contains(host))
                    continue;
                sent.insert(host);
                m_sweepQueue.append(host);
            }
        }
    }
    m_sweepDatagram = datagram;
    drainSweep();
}

void LocalMeshNode::drainSweep()
{
    if (!m_running || !m_discovery || m_sweepDatagram.isEmpty()) {
        m_sweepQueue.clear();
        m_sweepDatagram.clear();
        return;
    }
    for (int sent = 0; sent < kSweepBatch && !m_sweepQueue.isEmpty(); ++sent) {
        if (!writeDiscovery(m_sweepDatagram, m_sweepQueue.first(),
                            quint16(m_discoveryPort), m_diag.sweepSent) &&
            ++m_sweepHeadTries < kSweepHeadMaxTries)
            break; // the send buffer is full; resolution frees it shortly
        m_sweepQueue.removeFirst();
        m_sweepHeadTries = 0;
    }
    if (m_sweepQueue.isEmpty()) {
        m_sweepDatagram.clear();
        return;
    }
    if (!m_sweepPaceTimer) {
        m_sweepPaceTimer = new QTimer(this);
        m_sweepPaceTimer->setSingleShot(true);
        m_sweepPaceTimer->setInterval(kSweepBatchGapMs);
        connect(m_sweepPaceTimer, &QTimer::timeout, this,
                &LocalMeshNode::drainSweep);
    }
    m_sweepPaceTimer->start();
}

void LocalMeshNode::onDatagrams()
{
    while (m_discovery && m_discovery->hasPendingDatagrams()) {
        QByteArray datagram(int(m_discovery->pendingDatagramSize()), Qt::Uninitialized);
        QHostAddress sender;
        quint16 senderPort = 0;
        const qint64 read = m_discovery->readDatagram(datagram.data(),
                                                      datagram.size(), &sender,
                                                      &senderPort);
        if (read <= 0 || !m_running)
            continue;
        datagram.truncate(int(read));
        ++m_diag.datagramsIn;
        // A beacon from outside this network is not a mesh peer, whatever it
        // claims: refuse it before any of its contents are looked at.
        if (!isLocalNetworkAddress(sender)) {
            ++m_diag.offNetworkIn;
            continue;
        }
        const QJsonDocument document = QJsonDocument::fromJson(datagram);
        const QJsonObject plain = document.isObject()
                                      ? m_crypto.decryptObject(document.object())
                                      : QJsonObject();
        Beacon beacon;
        if (plain.isEmpty() || !parseBeacon(plain, &beacon)) {
            ++m_diag.undecryptableIn;
            continue;
        }
        if (beacon.kid == m_seal.keyId()) {
            ++m_diag.ownBeaconsIn;
            continue;
        }
        ++m_diag.peerBeaconsIn;
        m_diag.lastBeaconMs = QDateTime::currentMSecsSinceEpoch();
        const QString heardName = m_peers.value(beacon.kid).name;
        m_diag.lastBeaconFrom =
            QStringLiteral("%1 %2:%3%4")
                .arg(heardName.isEmpty() ? beacon.kid.left(8) : heardName,
                     sender.toString())
                .arg(beacon.port)
                .arg(beacon.reply ? QStringLiteral(" (reply)") : QString());
        rememberPeer(beacon.kid, QJsonObject(), false, false);
        if (!m_running || !m_discovery)
            break;
        Peer &peer = m_peers[beacon.kid];
        QHostAddress reachable(sender);
        reachable.setScopeId(QString());
        peer.address = reachable;
        peer.port = beacon.port;
        if (linkForPeer(beacon.kid)) {
            peer.unlinkedSinceMs = 0;
            continue;
        }
        // Answer straight back, so a sender that never hears this node's
        // multicast still learns where to reach it.
        if (!beacon.reply) {
            const QByteArray answer = beaconDatagram(true);
            if (!answer.isEmpty())
                writeDiscovery(answer, sender, senderPort, m_diag.unicastSent);
        }
        // Both sides beacon, so let the lower id dial and the higher id
        // listen. That keeps one link per pair in the common case; the hello
        // handshake resolves the crossing case deterministically. When the
        // lower id's dial never lands — this machine's firewall drops it —
        // the higher id dials too, once the peer has stayed unlinked for half
        // a beat.
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (peer.unlinkedSinceMs == 0)
            peer.unlinkedSinceMs = now;
        if (m_seal.keyId() < beacon.kid ||
            now - peer.unlinkedSinceMs >= m_beaconIntervalMs / 2)
            connectToPeer(reachable, beacon.port);
    }
}

void LocalMeshNode::announceNow()
{
    if (!m_running)
        return;
    refreshNetwork();
    m_beaconCount = 0; // the next unicast round sweeps the subnet
    sendBeacon();
    sendUnicastBeacons();
}

bool LocalMeshNode::connectToPeer(const QHostAddress &address, quint16 port)
{
    if (!m_running || port == 0 || !isLocalNetworkAddress(address))
        return false;
    if (m_links.size() >= kMaxLinks)
        return false;
    const QString key = dialKey(address, port);
    if (m_dialing.contains(key))
        return false;
    m_dialing.insert(key);
    ++m_diag.dials;
    auto *socket = new QTcpSocket(this);
    QTimer::singleShot(kDialTimeoutMs, socket, [this, socket, key] {
        if (socket->state() == QAbstractSocket::ConnectedState)
            return;
        m_dialing.remove(key);
        ++m_diag.dialsFailed;
        m_diag.lastDialError = key + QStringLiteral(": timed out");
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    });
    connect(socket, &QTcpSocket::connected, this, [this, socket, key] {
        m_dialing.remove(key);
        ++m_diag.dialsOk;
        attachLink(socket, true, false);
        connect(socket, &QTcpSocket::disconnected, this,
                [this, socket] { dropLink(socket, QString()); });
    });
    connect(socket, &QTcpSocket::errorOccurred, this,
            [this, socket, key](QAbstractSocket::SocketError) {
                m_dialing.remove(key);
                if (linkFor(socket)) {
                    dropLink(socket, socket->errorString());
                    return;
                }
                ++m_diag.dialsFailed;
                m_diag.lastDialError = key + QStringLiteral(": ") +
                                       socket->errorString();
                socket->disconnect(this);
                socket->deleteLater();
            });
    socket->connectToHost(address, port);
    return true;
}

void LocalMeshNode::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        if (!socket)
            break;
        if (!m_running || m_links.size() >= kMaxLinks ||
            !isLocalNetworkAddress(socket->peerAddress())) {
            ++m_diag.inboundRefused;
            socket->abort();
            socket->deleteLater();
            continue;
        }
        const bool fromNetwork = !socket->peerAddress().isLoopback();
        ++m_diag.inboundAccepted;
        attachLink(socket, false, false);
        connect(socket, &QTcpSocket::disconnected, this,
                [this, socket] { dropLink(socket, QString()); });
        connect(socket, &QTcpSocket::errorOccurred, this,
                [this, socket](QAbstractSocket::SocketError) {
                    dropLink(socket, socket->errorString());
                });
        if (fromNetwork && !m_inboundLinkSeen) {
            m_inboundLinkSeen = true;
            emit inboundLinkAccepted();
        }
    }
}

void LocalMeshNode::setControlHandler(ControlHandler handler)
{
    m_controlHandler = std::move(handler);
}

void LocalMeshNode::attachLink(QIODevice *socket, bool outbound, bool bluetooth)
{
    if (!socket || linkFor(socket))
        return;
    auto *link = new Link;
    link->socket = socket;
    link->outbound = outbound;
    link->bluetooth = bluetooth;
    link->ephPriv = forkmesh::chatv3::randomBytes(32);
    link->ownHello = forkmesh::chatv3::linkHello(
        m_seal.device() ? m_seal.device()->idk() : QByteArray(),
        forkmesh::chatv3::x25519Public(link->ephPriv),
        forkmesh::chatv3::randomBytes(16));
    m_links.append(link);
    if (auto *tcp = qobject_cast<QTcpSocket *>(socket))
        enableLinkKeepAlive(tcp);
    connect(socket, &QIODevice::readyRead, this,
            [this, socket] { onLinkReadyRead(socket); });
    writeRaw(*link, link->ownHello);
}

void LocalMeshNode::onLinkReadyRead(QIODevice *socket)
{
    Link *link = linkFor(socket);
    if (!link)
        return;
    link->buffer.append(socket->readAll());
    QByteArray payload;
    bool overflow = false;
    while (takeFrame(link->buffer, &payload, &overflow)) {
        ++m_diag.framesIn;
        if (link->stage == Link::Stage::Hello && !link->outbound &&
            !link->bluetooth && isControlChannelHello(payload)) {
            auto *tcp = qobject_cast<QTcpSocket *>(socket);
            const QByteArray rest = link->buffer;
            m_links.removeOne(link);
            forkmesh::chatv3::wipe(link->ephPriv);
            delete link;
            socket->disconnect(this);
            if (!tcp || !m_controlHandler) {
                socket->close();
                socket->deleteLater();
                return;
            }
            m_controlHandler(tcp, payload, rest);
            return;
        }
        switch (link->stage) {
        case Link::Stage::Hello:
            handleLinkHello(*link, payload);
            break;
        case Link::Stage::Auth:
            handleLinkAuth(*link, payload);
            break;
        case Link::Stage::Open: {
            QByteArray clear;
            if (!forkmesh::chatv3::linkOpen(link->recvKey, link->recvCounter, payload,
                                            &clear)) {
                ++m_diag.framesUndecryptable;
                dropLink(socket, QStringLiteral("bad link frame"));
                return;
            }
            ++link->recvCounter;
            const QJsonObject frame = parseObject(clear);
            if (!frame.isEmpty())
                handleLinkFrame(*link, frame);
            break;
        }
        }
        link = linkFor(socket);
        if (!link)
            return;
    }
    if (overflow)
        dropLink(socket, QStringLiteral("oversized frame"));
}

void LocalMeshNode::handleLinkHello(Link &link, const QByteArray &payload)
{
    const QJsonObject hello = parseObject(payload);
    if (!forkmesh::chatv3::isLinkHello(hello)) {
        ++m_diag.framesUndecryptable;
        dropLink(link.socket, QStringLiteral("not a v3 link"));
        return;
    }
    const QByteArray idk =
        forkmesh::chatv3::fromB64u(hello.value(QStringLiteral("idk")).toString());
    const QString kid = forkmesh::chatv3::id16(idk);
    if (!m_device || idk == m_device->idk()) {
        dropLink(link.socket, QStringLiteral("own identity"));
        return;
    }
    if (m_device->isBlockedAnywhere(kid)) {
        dropLink(link.socket, QStringLiteral("blocked device"));
        return;
    }
    link.peerHello = hello;
    link.peerIdk = idk;
    link.peerKid = kid;
    link.transcript = link.outbound
                          ? forkmesh::chatv3::linkTranscript(link.ownHello, hello)
                          : forkmesh::chatv3::linkTranscript(hello, link.ownHello);
    const QByteArray sig = m_device->sign(forkmesh::chatv3::linkAuthMessage(
        link.outbound ? QStringLiteral("i") : QStringLiteral("r"), link.transcript));
    link.stage = Link::Stage::Auth;
    writeRaw(link, QJsonObject{{QStringLiteral("t"), QStringLiteral("fm-link-auth")},
                               {QStringLiteral("sig"), forkmesh::chatv3::b64u(sig)}});
}

void LocalMeshNode::handleLinkAuth(Link &link, const QByteArray &payload)
{
    const QJsonObject auth = parseObject(payload);
    bool ok = false;
    const QByteArray sig =
        forkmesh::chatv3::fromB64u(auth.value(QStringLiteral("sig")).toString(), &ok);
    if (auth.value(QStringLiteral("t")).toString() != QLatin1String("fm-link-auth") ||
        !ok ||
        !forkmesh::chatv3::ed25519Verify(
            link.peerIdk, sig,
            forkmesh::chatv3::linkAuthMessage(
                link.outbound ? QStringLiteral("r") : QStringLiteral("i"),
                link.transcript))) {
        ++m_diag.framesUndecryptable;
        dropLink(link.socket, QStringLiteral("bad link auth"));
        return;
    }
    QByteArray i2r;
    QByteArray r2i;
    const bool derived = forkmesh::chatv3::linkKeys(
        link.ephPriv,
        forkmesh::chatv3::fromB64u(link.peerHello.value(QStringLiteral("eph")).toString()),
        link.transcript, &i2r, &r2i);
    forkmesh::chatv3::wipe(link.ephPriv);
    if (!derived) {
        dropLink(link.socket, QStringLiteral("bad link key"));
        return;
    }
    link.sendKey = link.outbound ? i2r : r2i;
    link.recvKey = link.outbound ? r2i : i2r;
    forkmesh::chatv3::wipe(i2r);
    forkmesh::chatv3::wipe(r2i);
    link.stage = Link::Stage::Open;
    onLinkAuthenticated(link);
}

void LocalMeshNode::onLinkAuthenticated(Link &link)
{
    QIODevice *socket = link.socket;
    const QString kid = link.peerKid;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    link.helloMs = now;
    if (Link *other = linkForPeer(kid, &link)) {
        if (other->bluetooth != link.bluetooth) {
            if (link.bluetooth) {
                dropLink(socket, QStringLiteral("duplicate link"));
                return;
            }
            dropLink(other->socket, QStringLiteral("duplicate link"));
        } else if (now - other->helloMs >= m_linkReplaceAfterMs) {
            dropLink(other->socket, QStringLiteral("replaced"));
        } else {
            const QString own = m_seal.keyId();
            const QString mine = link.outbound ? own : kid;
            const QString theirs = other->outbound ? own : kid;
            if (mine < theirs) {
                dropLink(socket, QStringLiteral("duplicate link"));
                return;
            }
            dropLink(other->socket, QStringLiteral("duplicate link"));
        }
    }
    Link *live = linkFor(socket);
    if (!live)
        return;
    m_peers[kid].unlinkedSinceMs = 0;
    m_peers[kid].lastSeenMs = now;
    const QJsonObject bundle = m_seal.keyFrame();
    if (!bundle.isEmpty())
        writeLink(*live, bundle);
    for (const QString &known : std::as_const(m_bundleOrder))
        if (known != kid)
            writeLink(*live, m_bundles.value(known));
    for (const QString &channel : std::as_const(m_ownFocus))
        writeLink(*live, QJsonObject{{QStringLiteral("type"), QStringLiteral("focus")},
                                     {QStringLiteral("channel"), channel},
                                     {QStringLiteral("on"), true}});
    QJsonObject hello = makeMessage(QStringLiteral("hello"));
    if (m_server)
        hello.insert(QStringLiteral("port"), int(m_server->serverPort()));
    if (m_controlHandler)
        hello.insert(QStringLiteral("control"), true);
    live->pending.prepend(hello);
    if (live->bundleKnown)
        flushLinkPending(*live);
    emit linkedDevicesChanged();
}

void LocalMeshNode::handleLinkFrame(Link &link, const QJsonObject &frame)
{
    const QString type = frame.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("e2ee-key")) {
        handleBundle(link, frame);
        return;
    }
    if (type == QLatin1String("sealed")) {
        handleSealed(link, frame);
        return;
    }
    const QString channel = frame.value(QStringLiteral("channel")).toString();
    if (!channel.startsWith(QLatin1String("direct:")) || channel.size() > 80)
        return;
    if (type == QLatin1String("direct-sealed")) {
        const QJsonObject sealed = frame.value(QStringLiteral("frame")).toObject();
        if (sealed.value(QStringLiteral("kid")).toString() != link.peerKid) {
            m_diag.lastDropReason = QStringLiteral("direct frame from another device");
            return;
        }
        ++m_diag.directIn;
        const QString accepted =
            m_directHandler ? m_directHandler(link.peerKid, channel, sealed) : QString();
        if (!accepted.isEmpty())
            writeLink(link, QJsonObject{{QStringLiteral("type"), QStringLiteral("direct-ack")},
                                        {QStringLiteral("channel"), channel},
                                        {QStringLiteral("id"), accepted}});
        return;
    }
    if (type == QLatin1String("direct-ack")) {
        const QString id = frame.value(QStringLiteral("id")).toString().left(96);
        if (!id.isEmpty())
            emit directAcked(link.peerKid, channel, id);
        return;
    }
    if (type == QLatin1String("focus")) {
        const bool on = frame.value(QStringLiteral("on")).toBool();
        const bool changed = on ? !link.focus.contains(channel) : link.focus.contains(channel);
        if (on)
            link.focus.insert(channel);
        else
            link.focus.remove(channel);
        if (changed)
            emit peerFocusChanged(link.peerKid, channel, on);
    }
}

void LocalMeshNode::handleBundle(Link &link, const QJsonObject &bundle)
{
    const QString kid = bundle.value(QStringLiteral("kid")).toString();
    if (kid == m_seal.keyId())
        return;
    bool changed = false;
    if (m_seal.acceptKeyFrame(bundle, &changed) == RoomSeal::BundleResult::Rejected) {
        ++m_diag.framesUnsealable;
        return;
    }
    const QJsonObject known = m_bundles.value(kid);
    const bool fresh = known.isEmpty() || changed;
    if (fresh) {
        m_bundles.insert(kid, bundle);
        m_bundleOrder.removeAll(kid);
        m_bundleOrder.append(kid);
        while (m_bundleOrder.size() > kMaxSessionRecipients)
            m_bundles.remove(m_bundleOrder.takeFirst());
    }
    QIODevice *socket = link.socket;
    if (kid == link.peerKid && !link.bundleKnown) {
        link.bundleKnown = true;
        flushLinkPending(link);
        if (!linkFor(socket))
            return;
    }
    if (!fresh)
        return;
    for (Link *other : std::as_const(m_links))
        if (other != &link && other->stage == Link::Stage::Open)
            writeLink(*other, bundle);
    if (known.isEmpty() && kid != link.peerKid)
        resealOwnHistory(link, kid);
    const QList<QJsonObject> released = m_seal.takeReleasedHeld();
    for (const QJsonObject &frame : released) {
        Link *current = linkFor(socket);
        if (!current)
            return;
        handleSealed(*current, frame);
    }
}

void LocalMeshNode::handleSealed(Link &link, const QJsonObject &frame)
{
    const QString sig = frame.value(QStringLiteral("sig")).toString();
    const RoomSeal::Opened opened = m_seal.open(frame);
    bool forward = false;
    switch (opened.status) {
    case RoomSeal::Status::Opened: {
        const QString type = opened.plain.value(QStringLiteral("type")).toString();
        forward = type == QLatin1String("chat");
        if (forward && !rememberForwarded(sig))
            forward = false;
        QIODevice *socket = link.socket;
        QJsonObject plain = opened.plain;
        RoomSeal::useSignedTime(plain, opened);
        handleEnvelope(link, plain, opened.senderKid, frame);
        Link *current = linkFor(socket);
        if (forward)
            forwardFrame(current, frame);
        return;
    }
    case RoomSeal::Status::NotForMe:
    case RoomSeal::Status::Held: {
        if (!rememberForwarded(sig))
            return;
        QSet<QString> recipients;
        for (const QJsonValue &value : frame.value(QStringLiteral("keys")).toArray())
            recipients.insert(value.toObject().value(QStringLiteral("kid")).toString());
        for (Link *other : std::as_const(m_links))
            if (other != &link && other->stage == Link::Stage::Open &&
                recipients.contains(other->peerKid))
                writeLink(*other, frame);
        return;
    }
    case RoomSeal::Status::Replay:
        break;
    case RoomSeal::Status::Invalid:
        ++m_diag.framesUnsealable;
        break;
    }
    if (forward)
        forwardFrame(&link, frame);
}

void LocalMeshNode::forwardFrame(Link *from, const QJsonObject &frame)
{
    for (Link *other : std::as_const(m_links))
        if (other != from && other->stage == Link::Stage::Open)
            writeLink(*other, frame);
}

bool LocalMeshNode::rememberForwarded(const QString &sig)
{
    if (sig.isEmpty() || m_forwarded.contains(sig))
        return false;
    m_forwarded.insert(sig);
    m_forwardedOrder.enqueue(sig);
    while (m_forwardedOrder.size() > kForwardedCacheLimit)
        m_forwarded.remove(m_forwardedOrder.dequeue());
    return true;
}

void LocalMeshNode::resealOwnHistory(Link &link, const QString &kid)
{
    QIODevice *socket = link.socket;
    for (const QJsonObject &entry : std::as_const(m_history)) {
        if (entry.value(QStringLiteral("senderId")).toString() != m_nodeId)
            continue;
        const QJsonObject frame = m_seal.sealObjectFor(entry, {kid});
        Link *current = linkFor(socket);
        if (frame.isEmpty() || !current)
            return;
        rememberForwarded(frame.value(QStringLiteral("sig")).toString());
        if (writeLink(*current, frame))
            clearQueued(entry.value(QStringLiteral("id")).toString());
    }
}

void LocalMeshNode::flushLinkPending(Link &link)
{
    QIODevice *socket = link.socket;
    while (!link.pending.isEmpty()) {
        const QJsonObject plain = link.pending.dequeue();
        writeSealedTo(link, plain);
        if (!linkFor(socket))
            return;
    }
}

void LocalMeshNode::dropLink(QIODevice *socket, const QString &reason)
{
    for (qsizetype i = 0; i < m_links.size(); ++i) {
        Link *link = m_links.at(i);
        if (link->socket != socket)
            continue;
        m_diag.lastDropReason =
            (link->peerKid.isEmpty() ? QStringLiteral("unnamed link")
                                     : link->peerKid.left(8)) +
            QStringLiteral(": ") +
            (reason.isEmpty() ? QStringLiteral("closed") : reason);
        m_links.removeAt(i);
        const QString peerId = link->peerKid;
        const bool wasOpen = link->stage == Link::Stage::Open;
        const bool bluetooth = link->bluetooth;
        const bool settled =
            link->helloMs > 0 &&
            QDateTime::currentMSecsSinceEpoch() - link->helloMs >=
                kRedialMinLinkMs;
        if (!link->btKey.isEmpty() &&
            reason == QLatin1String("duplicate link"))
            m_btQuietUntil.insert(link->btKey,
                                  QDateTime::currentMSecsSinceEpoch() +
                                      kBluetoothQuietMs);
        forkmesh::chatv3::wipe(link->ephPriv);
        forkmesh::chatv3::wipe(link->sendKey);
        forkmesh::chatv3::wipe(link->recvKey);
        delete link;
        socket->disconnect(this);
        socket->close();
        socket->deleteLater();
        if (wasOpen)
            emit linkedDevicesChanged();
        const bool superseded = reason == QLatin1String("duplicate link") ||
                                reason == QLatin1String("replaced") ||
                                reason == QLatin1String("identity changed");
        if (!peerId.isEmpty() && !linkForPeer(peerId)) {
            const auto peer = m_peers.find(peerId);
            if (peer != m_peers.end()) {
                peer->linked = false;
                noteMembershipChange();
                if (!superseded && settled && m_running &&
                    !peer->address.isNull())
                    QTimer::singleShot(kRedialDelayMs, this, [this, peerId] {
                        redialPeer(peerId);
                    });
            }
        }
        if (!bluetooth && !superseded)
            m_btQuietUntil.clear();
        if (!superseded && settled && m_running)
            QTimer::singleShot(kBluetoothRedialMs, this,
                               &LocalMeshNode::restartBluetoothDiscovery);
        return;
    }
}

void LocalMeshNode::redialPeer(const QString &peerId)
{
    if (!m_running || linkForPeer(peerId))
        return;
    const auto peer = m_peers.constFind(peerId);
    if (peer == m_peers.constEnd() || peer->address.isNull() || !peer->port)
        return;
    connectToPeer(peer->address, peer->port);
}

void LocalMeshNode::sweepPeers()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    bool blocked = false;
    for (auto it = m_peers.begin(); it != m_peers.end();) {
        if (linkForPeer(it.key())) {
            ++it;
            continue;
        }
        if (now - it->lastSeenMs > m_peerTimeoutMs) {
            it = m_peers.erase(it);
            changed = true;
            continue;
        }
        // Still beaconing, long since dialled from both ends, and no link:
        // the beacons get through and the links do not.
        if (!it->bluetooth && it->unlinkedSinceMs > 0 &&
            now - it->unlinkedSinceMs >=
                qint64(m_beaconIntervalMs) * kLinksBlockedAfterBeats &&
            now - it->lastSeenMs <= qint64(m_beaconIntervalMs) * 2)
            blocked = true;
        ++it;
    }
    if (blocked != m_linksBlocked) {
        m_linksBlocked = blocked;
        if (blocked)
            emit linksBlockedDetected();
    }
    if (changed)
        noteMembershipChange();
}

QJsonObject LocalMeshNode::makeMessage(const QString &type) const
{
    QJsonObject message{
        {QStringLiteral("type"), type},
        {QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)},
        {QStringLiteral("senderId"), m_nodeId},
        {QStringLiteral("sender"), m_userName},
        {QStringLiteral("conversation"), conversation()},
        {QStringLiteral("ts"), double(QDateTime::currentMSecsSinceEpoch())}};
    if (!m_nodeName.isEmpty())
        message.insert(QStringLiteral("nodeName"), m_nodeName);
    if (!m_ownerUser.isEmpty())
        message.insert(QStringLiteral("ownerUser"), m_ownerUser);
    if (!m_accountKind.isEmpty())
        message.insert(QStringLiteral("accountKind"), m_accountKind);
    if (!m_platform.isEmpty())
        message.insert(QStringLiteral("platform"), m_platform);
    if (!m_version.isEmpty())
        message.insert(QStringLiteral("version"), m_version);
    return message;
}

bool LocalMeshNode::writeRaw(Link &link, const QJsonObject &frame)
{
    if (!link.socket || !link.socket->isWritable())
        return false;
    const QByteArray payload = QJsonDocument(frame).toJson(QJsonDocument::Compact);
    return link.socket->write(frameBytes(payload)) == payload.size() + 4;
}

bool LocalMeshNode::writeLink(Link &link, const QJsonObject &frame)
{
    if (link.stage != Link::Stage::Open || frame.isEmpty() || !link.socket ||
        !link.socket->isWritable())
        return false;
    const QByteArray plain = QJsonDocument(frame).toJson(QJsonDocument::Compact);
    if (plain.size() + 16 > maxFrameBytes())
        return false;
    const QByteArray data =
        forkmesh::chatv3::linkSeal(link.sendKey, link.sendCounter, plain);
    if (data.isEmpty())
        return false;
    ++link.sendCounter;
    return link.socket->write(frameBytes(data)) == data.size() + 4;
}

QStringList LocalMeshNode::sessionKeyIds() const
{
    return m_bundleOrder;
}

void LocalMeshNode::clearQueued(const QString &id)
{
    if (m_queued.isEmpty() || !m_queued.remove(id))
        return;
    scheduleHistorySave();
    emit queuedChanged(id, false);
}

bool LocalMeshNode::writeSealedTo(Link &link, const QJsonObject &plain)
{
    if (link.stage != Link::Stage::Open || link.peerKid.isEmpty())
        return false;
    const QJsonObject frame = m_seal.sealObjectFor(plain, {link.peerKid});
    if (frame.isEmpty())
        return false;
    rememberForwarded(frame.value(QStringLiteral("sig")).toString());
    if (!writeLink(link, frame))
        return false;
    clearQueued(plain.value(QStringLiteral("id")).toString());
    return true;
}

void LocalMeshNode::sendToAll(const QJsonObject &plain, bool store)
{
    QJsonObject frame;
    bool sealed = false;
    const QString id = plain.value(QStringLiteral("id")).toString();
    const QList<Link *> links = m_links;
    for (Link *link : links) {
        if (!m_links.contains(link))
            continue;
        if (link->stage != Link::Stage::Open || !link->bundleKnown) {
            if (link->pending.size() < kMaxPendingFrames)
                link->pending.enqueue(plain);
            continue;
        }
        if (!sealed) {
            sealed = true;
            frame = m_seal.sealObjectFor(plain, sessionKeyIds());
            if (!frame.isEmpty())
                rememberForwarded(frame.value(QStringLiteral("sig")).toString());
        }
        if (frame.isEmpty())
            break;
        if (writeLink(*link, frame))
            clearQueued(id);
    }
    if (store && !frame.isEmpty() && !id.isEmpty())
        m_historyFrames.insert(id, frame);
}

void LocalMeshNode::sendHistoryTo(Link &link)
{
    QIODevice *socket = link.socket;
    const QList<QJsonObject> history = m_history;
    for (const QJsonObject &entry : history) {
        Link *current = linkFor(socket);
        if (!current)
            return;
        const QString id = entry.value(QStringLiteral("id")).toString();
        if (entry.value(QStringLiteral("senderId")).toString() == m_nodeId) {
            writeSealedTo(*current, entry);
            continue;
        }
        const auto frame = m_historyFrames.constFind(id);
        if (frame != m_historyFrames.constEnd())
            writeLink(*current, frame.value());
    }
}

QList<forkmesh::chatv3::MeshLinkedDevice> LocalMeshNode::linkedDevices() const
{
    QList<forkmesh::chatv3::MeshLinkedDevice> devices;
    QHash<QString, int> index;
    for (const Link *link : m_links) {
        if (link->stage != Link::Stage::Open || link->peerKid.isEmpty())
            continue;
        const auto found = index.constFind(link->peerKid);
        if (found != index.constEnd()) {
            forkmesh::chatv3::MeshLinkedDevice &device = devices[found.value()];
            device.bluetooth = device.bluetooth && link->bluetooth;
            device.focus.unite(link->focus);
            continue;
        }
        forkmesh::chatv3::MeshLinkedDevice device;
        device.kid = link->peerKid;
        device.bluetooth = link->bluetooth;
        device.focus = link->focus;
        index.insert(link->peerKid, int(devices.size()));
        devices.append(device);
    }
    return devices;
}

const LocalMeshNode::Link *LocalMeshNode::openLinkFor(const QString &kid) const
{
    const Link *best = nullptr;
    for (const Link *link : m_links) {
        if (link->stage != Link::Stage::Open || link->peerKid != kid)
            continue;
        if (!best || (best->bluetooth && !link->bluetooth))
            best = link;
    }
    return best;
}

bool LocalMeshNode::sendDirectSealed(const QString &kid, const QString &channel,
                                     const QJsonObject &frame)
{
    if (!m_running || !channel.startsWith(QLatin1String("direct:")) ||
        frame.value(QStringLiteral("kid")).toString() != m_seal.keyId())
        return false;
    Link *link = const_cast<Link *>(openLinkFor(kid));
    if (!link)
        return false;
    const bool written =
        writeLink(*link, QJsonObject{{QStringLiteral("type"), QStringLiteral("direct-sealed")},
                                     {QStringLiteral("channel"), channel},
                                     {QStringLiteral("frame"), frame}});
    if (written)
        ++m_diag.directOut;
    return written;
}

void LocalMeshNode::setDirectFocus(const QString &channel, bool on)
{
    if (!channel.startsWith(QLatin1String("direct:")))
        return;
    if (on == m_ownFocus.contains(channel))
        return;
    if (on)
        m_ownFocus.insert(channel);
    else
        m_ownFocus.remove(channel);
    const QJsonObject frame{{QStringLiteral("type"), QStringLiteral("focus")},
                            {QStringLiteral("channel"), channel},
                            {QStringLiteral("on"), on}};
    for (Link *link : std::as_const(m_links))
        if (link->stage == Link::Stage::Open)
            writeLink(*link, frame);
}

void LocalMeshNode::handleEnvelope(Link &link, const QJsonObject &plain,
                                   const QString &senderKid, const QJsonObject &frame)
{
    const QString type = plain.value(QStringLiteral("type")).toString();
    const QString senderId =
        plain.value(QStringLiteral("senderId")).toString().left(160);
    if (senderId.isEmpty() || senderId == m_nodeId || senderKid == m_seal.keyId())
        return;
    if (plain.value(QStringLiteral("conversation")).toString() != conversation())
        return;
    const bool fromLink = senderKid == link.peerKid;
    if ((type == QLatin1String("hello") || type == QLatin1String("typing") ||
         type == QLatin1String("bye")) &&
        !fromLink)
        return;
    if (type == QLatin1String("hello"))
        link.peerId = senderId;

    rememberPeer(senderKid, plain, fromLink && link.peerId == senderId, link.bluetooth);
    rememberWake(senderId, plain);

    if (type == QLatin1String("typing")) {
        const bool active = plain.value(QStringLiteral("active")).toBool();
        emit typingChanged(conversation(), senderId,
                           boundedText(plain, "sender", kMaxDisplayNameChars),
                           active);
        return;
    }
    const QString id = plain.value(QStringLiteral("id")).toString().left(160);
    if (id.isEmpty() || !markSeen(id))
        return;

    if (type == QLatin1String("hello")) {
        sendHistoryTo(link);
        return;
    }
    if (type == QLatin1String("bye")) {
        if (m_peers.remove(senderKid) > 0)
            noteMembershipChange();
        return;
    }
    if (type != QLatin1String("chat"))
        return;
    ++m_diag.chatIn;
    storeHistory(plain);
    m_historyFrames.insert(id, frame);
    emitChat(plain);
}

void LocalMeshNode::rememberPeer(const QString &kid, const QJsonObject &plain,
                                 bool linked, bool bluetooth)
{
    if (kid.isEmpty() || kid == m_seal.keyId())
        return;
    const bool isNew = !m_peers.contains(kid);
    Peer &peer = m_peers[kid];
    const QString nodeId =
        plain.value(QStringLiteral("senderId")).toString().left(160);
    const QString name = boundedText(plain, "sender", kMaxDisplayNameChars);
    const QString nodeName = boundedText(plain, "nodeName", kMaxDisplayNameChars);
    const QString ownerUser = boundedText(plain, "ownerUser", kMaxDisplayNameChars);
    const QString accountKind = boundedText(plain, "accountKind", 16);
    const QString platform = boundedText(plain, "platform", 16);
    const QString version = boundedText(plain, "version", kMaxVersionChars);
    const bool changed = isNew || (!nodeId.isEmpty() && nodeId != peer.nodeId) ||
                         (!name.isEmpty() && name != peer.name) ||
                         (!nodeName.isEmpty() && nodeName != peer.nodeName) ||
                         (!ownerUser.isEmpty() && ownerUser != peer.ownerUser) ||
                         (linked && !peer.linked);
    if (!nodeId.isEmpty())
        peer.nodeId = nodeId;
    if (!name.isEmpty())
        peer.name = name;
    if (!nodeName.isEmpty())
        peer.nodeName = nodeName;
    if (!ownerUser.isEmpty())
        peer.ownerUser = ownerUser;
    if (!accountKind.isEmpty())
        peer.accountKind = accountKind;
    if (!platform.isEmpty())
        peer.platform = platform;
    if (!version.isEmpty())
        peer.version = version;
    if (linked) {
        peer.linked = true;
        peer.bluetooth = bluetooth;
        peer.unlinkedSinceMs = 0;
    }
    peer.lastSeenMs = QDateTime::currentMSecsSinceEpoch();
    if (changed)
        noteMembershipChange();
}

LocalMeshNode::Link *LocalMeshNode::linkFor(QIODevice *socket)
{
    for (Link *link : std::as_const(m_links))
        if (link->socket == socket)
            return link;
    return nullptr;
}

LocalMeshNode::Link *LocalMeshNode::linkForPeer(const QString &kid, const Link *except)
{
    if (kid.isEmpty())
        return nullptr;
    for (Link *link : std::as_const(m_links))
        if (link != except && link->stage == Link::Stage::Open && link->peerKid == kid)
            return link;
    return nullptr;
}

void LocalMeshNode::storeHistory(const QJsonObject &plain)
{
    const QStringList evicted =
        ChatHistoryLimits::appendBounded(m_history, plain, &m_historyChars);
    for (const QString &id : evicted)
        m_queued.remove(id);
    scheduleHistorySave();
}

void LocalMeshNode::setHistoryFile(const QString &path)
{
    m_historyPath = path;
    m_historyLoaded = false;
}

void LocalMeshNode::loadHistory()
{
    if (m_historyLoaded || m_historyPath.isEmpty())
        return;
    m_historyLoaded = true;
    QFile file(m_historyPath);
    if (!file.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    if (root.value(QStringLiteral("v")).toInt() != kHistoryFileVersion)
        return;
    QSet<QString> queued;
    const QJsonArray queuedIds = root.value(QStringLiteral("queued")).toArray();
    for (const QJsonValue &id : queuedIds)
        queued.insert(id.toString());
    const QJsonArray entries = root.value(QStringLiteral("history")).toArray();
    for (const QJsonValue &value : entries) {
        const QJsonObject entry = value.toObject();
        const QString id = entry.value(QStringLiteral("id")).toString().left(160);
        if (id.isEmpty() ||
            entry.value(QStringLiteral("type")).toString() !=
                QLatin1String("chat") ||
            entry.value(QStringLiteral("conversation")).toString() !=
                conversation() ||
            !markSeen(id))
            continue;
        ChatHistoryLimits::appendBounded(m_history, entry, &m_historyChars);
        rememberWake(entry.value(QStringLiteral("senderId")).toString().left(160),
                     entry);
        if (queued.contains(id) &&
            entry.value(QStringLiteral("senderId")).toString() == m_nodeId)
            m_queued.insert(id);
        emitChat(entry, true);
    }
}

void LocalMeshNode::scheduleHistorySave()
{
    if (m_historyPath.isEmpty())
        return;
    if (!m_historySaveTimer) {
        m_historySaveTimer = new QTimer(this);
        m_historySaveTimer->setSingleShot(true);
        m_historySaveTimer->setInterval(kHistorySaveDelayMs);
        connect(m_historySaveTimer, &QTimer::timeout, this,
                &LocalMeshNode::saveHistory);
    }
    m_historySaveTimer->start();
}

void LocalMeshNode::saveHistory()
{
    if (m_historyPath.isEmpty())
        return;
    QJsonArray entries;
    for (const QJsonObject &entry : std::as_const(m_history))
        entries.append(entry);
    QJsonArray queued;
    for (const QString &id : std::as_const(m_queued))
        queued.append(id);
    const QJsonObject root{{QStringLiteral("v"), kHistoryFileVersion},
                           {QStringLiteral("history"), entries},
                           {QStringLiteral("queued"), queued}};
    QDir().mkpath(QFileInfo(m_historyPath).absolutePath());
    QSaveFile file(m_historyPath);
    if (!file.open(QIODevice::WriteOnly))
        return;
    // The room's plaintext: readable by this user only.
    file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    file.commit();
}

QString LocalMeshNode::sendOwn(QJsonObject message)
{
    const QString id = message.value(QStringLiteral("id")).toString();
    markSeen(id);
    storeHistory(message);
    // Queued until a write to some device clears it — now, when a linked
    // device's key lands, or when a device joins and gets the history.
    m_queued.insert(id);
    ++m_diag.chatOut;
    sendToAll(message, true);
    const QStringList wake = wakeTargets();
    if (!wake.isEmpty())
        emit wakeRequested(wake);
    emitChat(message);
    if (m_queued.contains(id))
        emit queuedChanged(id, true);
    return id;
}

void LocalMeshNode::rememberWake(const QString &senderId, const QJsonObject &plain)
{
    if (senderId.isEmpty() || senderId == m_nodeId)
        return;
    const QString token = plain.value(QStringLiteral("wake")).toString();
    const qint64 expires = wakeTokenExpiry(token);
    if (expires <= QDateTime::currentMSecsSinceEpoch())
        return;
    const auto known = m_wakeTokens.constFind(senderId);
    if (known != m_wakeTokens.constEnd() && wakeTokenExpiry(known.value()) >= expires)
        return;
    m_wakeTokens.insert(senderId, token);
    m_wakeOrder.removeAll(senderId);
    m_wakeOrder.append(senderId);
    while (m_wakeOrder.size() > kMaxWakeTokens)
        m_wakeTokens.remove(m_wakeOrder.takeFirst());
}

QStringList LocalMeshNode::wakeTargets() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QStringList tokens;
    for (auto it = m_wakeOrder.crbegin(); it != m_wakeOrder.crend(); ++it) {
        const QString token = m_wakeTokens.value(*it);
        if (wakeTokenExpiry(token) > now && !tokens.contains(token))
            tokens.append(token);
        if (tokens.size() >= kMaxWakeTokens)
            break;
    }
    return tokens;
}

bool LocalMeshNode::markSeen(const QString &id)
{
    if (m_seenIds.contains(id))
        return false;
    m_seenIds.insert(id);
    m_seenOrder.enqueue(id);
    while (m_seenOrder.size() > kSeenCacheLimit)
        m_seenIds.remove(m_seenOrder.dequeue());
    return true;
}

void LocalMeshNode::emitChat(const QJsonObject &plain, bool historical)
{
    ChatMessage out;
    out.historical = historical;
    out.id = plain.value(QStringLiteral("id")).toString().left(160);
    out.conversation = conversation();
    out.senderId = plain.value(QStringLiteral("senderId")).toString().left(160);
    out.senderName = boundedText(plain, "sender", kMaxDisplayNameChars);
    out.text = boundedText(plain, "text", kMaxTextChars);
    out.timestampMs = qint64(plain.value(QStringLiteral("ts")).toDouble());
    if (plain.contains(QStringLiteral("fileName"))) {
        out.fileName = safeFileName(plain.value(QStringLiteral("fileName")).toString());
        out.fileMime = boundedText(plain, "fileMime", kMaxMimeChars);
        out.fileData = boundedBase64(plain, "file");
        if (out.fileData.isEmpty())
            out.fileName.clear();
    }
    out.self = out.senderId == m_nodeId;
    emit messageArrived(out);
}

void LocalMeshNode::noteMembershipChange()
{
    emit peersChanged();
}

void LocalMeshNode::setUserName(const QString &name)
{
    const QString trimmed = name.trimmed().left(kMaxDisplayNameChars);
    if (trimmed.isEmpty() || trimmed == m_userName)
        return;
    m_userName = trimmed;
    if (m_running) {
        sendBeacon();
        noteMembershipChange();
    }
}

void LocalMeshNode::setNodeIdentity(const QString &nodeName,
                                  const QString &ownerUser)
{
    const QString node = nodeName.trimmed().left(kMaxDisplayNameChars);
    const QString owner = ownerUser.trimmed().left(kMaxDisplayNameChars);
    if (node == m_nodeName && owner == m_ownerUser)
        return;
    if (!node.isEmpty())
        m_nodeName = node;
    if (!owner.isEmpty())
        m_ownerUser = owner;
    if (m_running) {
        sendBeacon();
        noteMembershipChange();
    }
}

void LocalMeshNode::setAccountKind(const QString &kind)
{
    const QString trimmed = kind.trimmed().left(16);
    if (trimmed.isEmpty() || trimmed == m_accountKind)
        return;
    m_accountKind = trimmed;
    if (m_running)
        sendBeacon();
}

QList<MemberInfo> LocalMeshNode::members() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<MemberInfo> roster;
    MemberInfo self;
    self.id = m_nodeId;
    self.name = m_userName;
    self.nodeName = m_nodeName;
    self.ownerUser = m_ownerUser;
    self.accountKind = m_accountKind;
    self.platform = m_platform;
    self.version = m_version;
    self.self = true;
    self.online = m_running;
    roster.append(self);
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        MemberInfo member;
        member.id = it->nodeId.isEmpty() ? it.key() : it->nodeId;
        member.name = it->name.isEmpty() ? member.id : it->name;
        member.nodeName = it->nodeName;
        member.ownerUser = it->ownerUser;
        member.accountKind = it->accountKind;
        member.platform = it->platform;
        member.version = it->version;
        // Present either way: a device this one has no direct link to is
        // still in the room, reached through the peers that do.
        member.online = it->linked || (now - it->lastSeenMs) <= m_peerTimeoutMs;
        member.note = it->bluetooth ? QStringLiteral("Over Bluetooth")
                                    : QStringLiteral("On this network");
        roster.append(member);
    }
    return roster;
}

int LocalMeshNode::peerCount() const
{
    int linked = 0;
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it)
        if (it->linked)
            ++linked;
    return linked;
}

QString LocalMeshNode::sealKeyId() const
{
    return m_seal.keyId();
}

quint16 LocalMeshNode::listenPort() const
{
    return m_server ? m_server->serverPort() : 0;
}

namespace {

QString ageText(qint64 now, qint64 thenMs)
{
    if (thenMs <= 0)
        return QStringLiteral("never");
    const qint64 seconds = qMax<qint64>(0, (now - thenMs) / 1000);
    if (seconds < 120)
        return QStringLiteral("%1s ago").arg(seconds);
    if (seconds < 7200)
        return QStringLiteral("%1m ago").arg(seconds / 60);
    return QStringLiteral("%1h ago").arg(seconds / 3600);
}

// "wlan0 192.168.1.5/24" for every interface the mesh beacons on; a trailing
// "*" marks the ones the unicast sweep also covers.
QString meshInterfaceText()
{
    QStringList parts;
    const QList<QNetworkInterface> interfaces = meshInterfaces();
    for (const QNetworkInterface &iface : interfaces) {
        const QList<QNetworkAddressEntry> entries = iface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            if (entry.ip().protocol() != QAbstractSocket::IPv4Protocol ||
                !forkmesh::localmesh::isLocalNetworkAddress(entry.ip()) ||
                entry.ip().isLoopback())
                continue;
            parts << QStringLiteral("%1 %2/%3%4")
                         .arg(iface.name(), entry.ip().toString())
                         .arg(entry.prefixLength())
                         .arg(sweepableInterface(iface) ? QStringLiteral("*")
                                                        : QString());
        }
    }
    return parts.isEmpty() ? QStringLiteral("no LAN interface")
                           : parts.join(QStringLiteral(", "));
}

} // namespace

QString LocalMeshNode::diagnosticsLine() const
{
    if (!m_running)
        return QStringLiteral("mesh stopped");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const Diagnostics &d = m_diag;
    int keyed = 0;
    for (const Link *link : m_links)
        if (link->bundleKnown)
            ++keyed;
    QStringList parts;
    parts << QStringLiteral("udp %1 %2")
                 .arg(m_discoveryPort)
                 .arg(m_discovery ? QStringLiteral("bound")
                                  : QStringLiteral("CLOSED"));
    parts << QStringLiteral("tcp %1").arg(
        m_server ? QString::number(m_server->serverPort())
                 : QStringLiteral("CLOSED"));
    parts << meshInterfaceText();
    parts << QStringLiteral("tx mc %1 bc %2 uc %3 sweep %4%5")
                 .arg(d.multicastSent)
                 .arg(d.broadcastSent)
                 .arg(d.unicastSent)
                 .arg(d.sweepSent)
                 .arg(d.sendErrors ? QStringLiteral(" ERR %1").arg(d.sendErrors)
                                   : QString());
    parts << QStringLiteral("rx %1: own %2 peer %3 bad %4 off-net %5")
                 .arg(d.datagramsIn)
                 .arg(d.ownBeaconsIn)
                 .arg(d.peerBeaconsIn)
                 .arg(d.undecryptableIn)
                 .arg(d.offNetworkIn);
    parts << (d.lastBeaconMs
                  ? QStringLiteral("heard %1 %2")
                        .arg(d.lastBeaconFrom, ageText(now, d.lastBeaconMs))
                  : QStringLiteral("heard nobody"));
    parts << QStringLiteral("dial %1 ok %2 fail %3 · in %4")
                 .arg(d.dials)
                 .arg(d.dialsOk)
                 .arg(d.dialsFailed)
                 .arg(d.inboundAccepted);
    parts << QStringLiteral("links %1 keyed %2 · peers %3")
                 .arg(m_links.size())
                 .arg(keyed)
                 .arg(m_peers.size());
    parts << QStringLiteral("frames %1 bad %2 · chat in %3 out %4 queued %5 · dm in %6 out %7")
                 .arg(d.framesIn)
                 .arg(d.framesUndecryptable + d.framesUnsealable)
                 .arg(d.chatIn)
                 .arg(d.chatOut)
                 .arg(m_queued.size())
                 .arg(d.directIn)
                 .arg(d.directOut);
    parts << (bluetoothActive() ? QStringLiteral("bt on")
                                : QStringLiteral("bt off"));
    if (m_linksBlocked)
        parts << QStringLiteral("LINKS BLOCKED");
    return parts.join(QStringLiteral(" · "));
}

QString LocalMeshNode::diagnosticsDetail() const
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const Diagnostics &d = m_diag;
    QStringList lines;
    lines << QStringLiteral("node %1 · %2 %3 · up %4")
                 .arg(m_nodeId.left(12), m_platform, m_version,
                      ageText(now, m_startedMs));
    lines << QStringLiteral("interfaces: %1").arg(meshInterfaceText());
    lines << QStringLiteral("udp %1: %2").arg(m_discoveryPort).arg(
        m_discovery ? QStringLiteral("bound")
                    : QStringLiteral("closed %1").arg(d.discoveryError));
    lines << QStringLiteral("tcp: %1").arg(
        m_server ? QStringLiteral("listening on %1").arg(m_server->serverPort())
                 : QStringLiteral("closed %1").arg(d.listenError));
    if (!d.lastSendError.isEmpty())
        lines << QStringLiteral("last send error: %1").arg(d.lastSendError);
    if (!d.lastDialError.isEmpty())
        lines << QStringLiteral("last dial error: %1").arg(d.lastDialError);
    if (!d.lastDropReason.isEmpty())
        lines << QStringLiteral("last link drop: %1").arg(d.lastDropReason);
    if (d.inboundRefused)
        lines << QStringLiteral("inbound refused: %1").arg(d.inboundRefused);
    lines << QStringLiteral("frames undecryptable %1 · unsealable %2")
                 .arg(d.framesUndecryptable)
                 .arg(d.framesUnsealable);
    lines << QStringLiteral("sweep queue %1").arg(m_sweepQueue.size());
    if (bluetoothActive())
        lines << QStringLiteral("ble: seen %1 · dial %2 · open %3%4")
                     .arg(d.bleSeen)
                     .arg(d.bleDials)
                     .arg(d.bleOpened)
                     .arg(d.lastBleError.isEmpty()
                              ? QString()
                              : QStringLiteral(" · last %1")
                                    .arg(d.lastBleError));
    if (m_peers.isEmpty())
        lines << QStringLiteral("peers: none");
    for (auto it = m_peers.constBegin(); it != m_peers.constEnd(); ++it) {
        QString state;
        if (it->linked)
            state = it->bluetooth ? QStringLiteral("linked (bt)")
                                  : QStringLiteral("linked");
        else if (it->unlinkedSinceMs > 0)
            state = QStringLiteral("unlinked since %1")
                        .arg(ageText(now, it->unlinkedSinceMs));
        else
            state = QStringLiteral("unlinked");
        lines << QStringLiteral("peer %1 (%2) %3:%4 %5 %6 · %7 · seen %8")
                     .arg(it->name.isEmpty() ? QStringLiteral("?") : it->name,
                          it.key().left(8), it->address.toString())
                     .arg(it->port)
                     .arg(it->platform, it->version, state,
                          ageText(now, it->lastSeenMs));
    }
    return lines.join(QLatin1Char('\n'));
}

QString LocalMeshNode::sendChat(const QString &text)
{
    const QString trimmed = text.trimmed().left(kMaxTextChars);
    if (!m_running || trimmed.isEmpty())
        return QString();
    QJsonObject message = makeMessage(QStringLiteral("chat"));
    message.insert(QStringLiteral("text"), trimmed);
    return sendOwn(message);
}

QString LocalMeshNode::sendFile(const QString &fileName, const QString &mimeType,
                              const QByteArray &data)
{
    if (!m_running || data.isEmpty() || data.size() > kMaxFileBytes)
        return QString();
    QJsonObject message = makeMessage(QStringLiteral("chat"));
    message.insert(QStringLiteral("fileName"), safeFileName(fileName));
    message.insert(QStringLiteral("fileMime"), mimeType.left(kMaxMimeChars));
    message.insert(QStringLiteral("file"),
                   QString::fromLatin1(data.toBase64()));
    return sendOwn(message);
}

void LocalMeshNode::sendTyping(bool active)
{
    if (!m_running || (!active && !m_typingActive))
        return;
    m_typingActive = active;
    QJsonObject message = makeMessage(QStringLiteral("typing"));
    message.insert(QStringLiteral("active"), active);
    sendToAll(message);
    if (!m_typingStopTimer) {
        m_typingStopTimer = new QTimer(this);
        m_typingStopTimer->setSingleShot(true);
        m_typingStopTimer->setInterval(kTypingStopMs);
        connect(m_typingStopTimer, &QTimer::timeout, this,
                [this] { sendTyping(false); });
    }
    if (active)
        m_typingStopTimer->start();
    else
        m_typingStopTimer->stop();
}

#ifdef FORKMESH_LOCAL_MESH_TESTS
void LocalMeshNode::setDiscoveryPortForTests(int port)
{
    m_discoveryPort = port;
}

void LocalMeshNode::setBeaconIntervalForTests(int intervalMs)
{
    m_beaconIntervalMs = qMax(50, intervalMs);
    if (m_beaconTimer)
        m_beaconTimer->setInterval(m_beaconIntervalMs);
}

void LocalMeshNode::setPeerTimeoutForTests(int timeoutMs)
{
    m_peerTimeoutMs = qMax(50, timeoutMs);
}

void LocalMeshNode::setLinkReplaceAfterForTests(int ageMs)
{
    m_linkReplaceAfterMs = ageMs;
}

void LocalMeshNode::setMulticastEnabledForTests(bool enabled)
{
    m_multicastEnabled = enabled;
}

void LocalMeshNode::setAddressSweepEnabledForTests(bool enabled)
{
    m_addressSweepEnabled = enabled;
}

void LocalMeshNode::setBluetoothEnabledForTests(bool enabled)
{
    m_bluetoothEnabled = enabled;
}
#endif
