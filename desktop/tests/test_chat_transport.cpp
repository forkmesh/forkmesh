#include "MainnodeRoom.h"
#include "BleMeshLink.h"
#include "ChatAccountBlocks.h"
#include "ChatBlocks.h"
#include "ChatNodeOwners.h"
#include "ChatRelayRules.h"
#include "ChatFormat.h"
#include "ChatListPreview.h"
#include "ChatRecipients.h"
#include "DiscordClient.h"
#include "ReactionCatalog.h"
#include "StickerCatalog.h"
#include "LocalMeshNode.h"
#include "MeshFirewall.h"
#include "NodeDiagnostics.h"
#include "OfficeChannelMirror.h"
#include "RoomCrypto.h"
#include "RoomSeal.h"
#include "ServerNode.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUdpSocket>
#include <QUrlQuery>
#include <QThread>
#include <QTimer>
#include <QPointer>
#include <QSettings>

#include <algorithm>
#include <cstdio>
#include <functional>

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

QByteArray queryValue(const QString &query, const QByteArray &key)
{
    for (const QString &pair : query.split(QLatin1Char('&')))
        if (pair.startsWith(QString::fromUtf8(key) + QLatin1Char('=')))
            return pair.mid(key.size() + 1).toUtf8();
    return {};
}

QByteArray testNodeSeed(const QString &name)
{
    return forkmesh::chatv3::sha256("forkmesh-test-node:" + name.toUtf8());
}

QString nodeKey(const QString &name)
{
    return forkmesh::chatv3::b64u(forkmesh::chatv3::ed25519Public(testNodeSeed(name)));
}

RoomSeal::NodeSigner nodeSigner(const QString &name)
{
    const QByteArray seed = testNodeSeed(name);
    return [seed](const QByteArray &message) {
        return forkmesh::chatv3::ed25519Sign(seed, message);
    };
}

QString meshNode(const QString &name)
{
    return forkmesh::chatv3::b64u(forkmesh::chatv3::ed25519Public(
        forkmesh::chatv3::sha256("forkmesh-mesh-node-test:" + name.toUtf8())));
}

bool waitUntil(const std::function<bool()> &predicate, int timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();
    while (!predicate() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

class WebSocketHarness
{
public:
    WebSocketHarness()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                ++connectionCount;
                sockets.append(socket);
                QObject::connect(socket, &QTcpSocket::disconnected, socket,
                                 &QObject::deleteLater);
                // The first connection deliberately blackholes the HTTP
                // upgrade. Every retry is accepted immediately.
                if (connectionCount == 1 && blackholeFirst)
                    continue;
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket] { acceptHandshake(socket); });
            }
        });
    }

    bool listen()
    {
        return server.listen(QHostAddress::LocalHost, 0);
    }

    QUrl url() const
    {
        return QUrl(QStringLiteral("ws://127.0.0.1:%1%2")
                        .arg(server.serverPort())
                        .arg(forkmesh::mainnode::kRoomPath));
    }

    void sendAbnormalClose()
    {
        if (upgraded)
            upgraded->abort();
    }

    void sendCleanClose()
    {
        if (!upgraded)
            return;
        const char frame[] = {char(0x88), char(0x02), char(0x03), char(0xE8)};
        upgraded->write(frame, sizeof(frame));
        upgraded->flush();
    }

    // Write one unmasked server->client text frame, the way the relay's room
    // Durable Object relays a peer's envelope.
    void sendText(const QByteArray &payload)
    {
        if (!upgraded)
            return;
        QByteArray frame;
        frame.append(char(0x81));
        if (payload.size() < 126) {
            frame.append(char(payload.size()));
        } else if (payload.size() <= 0xffff) {
            frame.append(char(126));
            frame.append(char((payload.size() >> 8) & 0xff));
            frame.append(char(payload.size() & 0xff));
        } else {
            frame.append(char(127));
            for (int shift = 56; shift >= 0; shift -= 8)
                frame.append(char((qint64(payload.size()) >> shift) & 0xff));
        }
        frame.append(payload);
        upgraded->write(frame);
        upgraded->flush();
    }

    void sendTextThenClose(const QByteArray &payload)
    {
        if (!upgraded || payload.size() >= 126)
            return;
        QByteArray frames;
        frames.append(char(0x81));
        frames.append(char(payload.size()));
        frames.append(payload);
        frames.append(char(0x88));
        frames.append(char(0x02));
        frames.append(char(0x03));
        frames.append(char(0xE8));
        upgraded->write(frames);
        upgraded->flush();
    }

    QTcpServer server;
    bool blackholeFirst = true;
    int connectionCount = 0;
    QList<QTcpSocket *> sockets;
    QTcpSocket *upgraded = nullptr;
    QList<QByteArray> requests;
    // Everything the node wrote, exactly as the relay would hold it.
    QList<QByteArray> textFrames;
    int receivedCloseFrames = 0;
    QByteArray lastClosePayload;

private:
    void acceptHandshake(QTcpSocket *socket)
    {
        const QByteArray request = socket->readAll();
        if (socket == upgraded) {
            clientFrameBuffer += request;
            consumeClientFrames();
            return;
        }
        if (!request.contains("\r\n\r\n"))
            return;
        requests.append(request);
        const QByteArray marker = "Sec-WebSocket-Key:";
        const int keyStart = request.indexOf(marker);
        if (keyStart < 0)
            return;
        const int valueStart = keyStart + marker.size();
        const int lineEnd = request.indexOf("\r\n", valueStart);
        const QByteArray key = request.mid(valueStart, lineEnd - valueStart).trimmed();
        const QByteArray accept = QCryptographicHash::hash(
            key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
            QCryptographicHash::Sha1).toBase64();
        socket->write(
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
        socket->flush();
        upgraded = socket;
    }

    void consumeClientFrames()
    {
        while (clientFrameBuffer.size() >= 2) {
            const uchar b0 = uchar(clientFrameBuffer.at(0));
            const uchar b1 = uchar(clientFrameBuffer.at(1));
            quint64 length = b1 & 0x7f;
            int offset = 2;
            if (length == 126) {
                if (clientFrameBuffer.size() < 4)
                    return;
                length = (uchar(clientFrameBuffer.at(2)) << 8) |
                         uchar(clientFrameBuffer.at(3));
                offset = 4;
            } else if (length == 127) {
                if (clientFrameBuffer.size() < 10)
                    return;
                length = 0;
                for (int i = 0; i < 8; ++i)
                    length = (length << 8) |
                             uchar(clientFrameBuffer.at(2 + i));
                offset = 10;
            }
            const bool masked = b1 & 0x80;
            const int maskBytes = masked ? 4 : 0;
            if (length > quint64(clientFrameBuffer.size()) ||
                quint64(offset + maskBytes) + length >
                    quint64(clientFrameBuffer.size()))
                return;
            const QByteArray mask =
                masked ? clientFrameBuffer.mid(offset, 4) : QByteArray();
            offset += maskBytes;
            QByteArray payload = clientFrameBuffer.mid(offset, int(length));
            clientFrameBuffer.remove(0, offset + int(length));
            if (masked) {
                for (int i = 0; i < payload.size(); ++i)
                    payload[i] = payload.at(i) ^ mask.at(i % 4);
            }
            if ((b0 & 0x0f) == 0x1)
                textFrames.append(payload);
            if ((b0 & 0x0f) == 0x8) {
                ++receivedCloseFrames;
                lastClosePayload = payload;
            }
        }
    }

    QByteArray clientFrameBuffer;
};

class TestPeer
{
public:
    TestPeer(const QString &room, const QString &scope, RoomSeal::Policy policy,
             const QString &nodeName, char seed, const QString &acct = QString())
        : device(forkmesh::chatv3::ChatDevice::inMemory(QByteArray(32, seed))),
          node(policy == RoomSeal::Policy::Open ? nodeKey(nodeName) : nodeName),
          seal(room, scope, policy, acct.isEmpty() ? node : acct, node, device)
    {
        if (policy == RoomSeal::Policy::Open)
            seal.setNodeSigner(nodeSigner(nodeName));
    }

    QJsonObject open(const QJsonObject &frame)
    {
        if (!RoomSeal::isSealedFrame(frame))
            return {};
        const QString sig = frame.value(QStringLiteral("sig")).toString();
        const auto cached = opened.constFind(sig);
        if (cached != opened.constEnd())
            return cached.value();
        const QJsonObject plain = seal.unsealObject(frame);
        if (!plain.isEmpty())
            opened.insert(sig, plain);
        return plain;
    }

    QString keyId() const { return seal.keyId(); }
    QJsonObject keyFrame() const { return seal.keyFrame(); }
    bool accept(const QJsonObject &bundle) const
    {
        return seal.acceptKeyFrame(bundle) != RoomSeal::BundleResult::Rejected;
    }
    QJsonObject sealObject(const QJsonObject &plain) const { return seal.sealObject(plain); }

    std::shared_ptr<forkmesh::chatv3::ChatDevice> device;
    QString node;
    RoomSeal seal;
    QHash<QString, QJsonObject> opened;
};

QString directScope(const QString &room)
{
    return QStringLiteral("direct:") + room.section(QLatin1Char(':'), 1, 1);
}

bool connected(const ServerNode &node)
{
    const QList<QJsonObject> rows = node.networkDiagnostics();
    return !rows.isEmpty() && rows.first().value("connected").toBool();
}

qint64 backpressureDrops(const ServerNode &node)
{
    const QList<QJsonObject> rows = node.networkDiagnostics();
    return rows.isEmpty()
               ? -1
               : qint64(rows.first().value("backpressureDrops").toDouble());
}

void testSavedRoomMigration()
{
    QString url =
        QStringLiteral("wss://forkmesh.com") +
        forkmesh::mainnode::kLegacyRoomPath;
    QString room = forkmesh::mainnode::kLegacyDefaultRoomName;
    check(forkmesh::mainnode::migrateSavedDefaultRoom(&url, &room),
          "legacy shared-room settings are migrated");
    check(url == forkmesh::mainnode::kDefaultServerUrl,
          "legacy canonical URL moves to world-general");
    check(room == forkmesh::mainnode::kDefaultRoomName,
          "legacy room name moves to world-general");

    url = QStringLiteral("wss://api.forkmesh.com") +
          forkmesh::mainnode::kRoomPath;
    room = forkmesh::mainnode::kDefaultRoomName;
    check(forkmesh::mainnode::migrateSavedDefaultRoom(&url, &room),
          "retired API hostname is migrated");
    check(url == forkmesh::mainnode::kDefaultServerUrl,
          "retired API hostname moves to app");

    url = QStringLiteral("wss://relay.example.test:9443") +
          forkmesh::mainnode::kLegacyRoomPath +
          QStringLiteral("?instance=community");
    room = QStringLiteral("general");
    check(forkmesh::mainnode::migrateSavedDefaultRoom(&url, &room),
          "independent relay using the standard retired path is migrated");
    check(url.startsWith(QStringLiteral("wss://relay.example.test:9443")) &&
              url.contains(forkmesh::mainnode::kRoomPath) &&
              url.endsWith(QStringLiteral("?instance=community")),
          "migration preserves custom host, port, and query");

    url = QStringLiteral("wss://relay.example.test") +
          forkmesh::mainnode::kRoomPath;
    room = forkmesh::mainnode::kDefaultRoomName;
    check(!forkmesh::mainnode::migrateSavedDefaultRoom(&url, &room) &&
              url.startsWith(
                  QStringLiteral(
                      "wss://relay.example.test")),
          "the selected independent workers.dev instance is not redirected");

    const QString privateUrl =
        QStringLiteral("wss://relay.example.test/api/repo/alice/secret/"
                       "rooms/general/ws");
    url = privateUrl;
    room = QStringLiteral("general");
    check(!forkmesh::mainnode::migrateSavedDefaultRoom(&url, &room),
          "private repository room is not treated as the shared default");
    check(url == privateUrl && room == QStringLiteral("general"),
          "private repository settings remain byte-for-byte unchanged");

    const QString customRoomUrl =
        QStringLiteral("wss://forkmesh.com") +
        forkmesh::mainnode::kLegacyRoomPath;
    url = customRoomUrl;
    room = QStringLiteral("team-private");
    check(!forkmesh::mainnode::migrateSavedDefaultRoom(&url, &room),
          "custom room metadata is preserved even on the old path");
    check(url == customRoomUrl && room == QStringLiteral("team-private"),
          "custom room URL and name remain unchanged");
}

// ---- Local mesh room -------------------------------------------------------

void testLocalMeshAddressPolicy()
{
    using namespace forkmesh::localmesh;
    check(isLocalNetworkAddress(QHostAddress(QStringLiteral("192.168.1.20"))) &&
              isLocalNetworkAddress(QHostAddress(QStringLiteral("10.4.4.4"))) &&
              isLocalNetworkAddress(QHostAddress(QStringLiteral("172.20.0.9"))) &&
              isLocalNetworkAddress(QHostAddress(QStringLiteral("169.254.7.7"))) &&
              isLocalNetworkAddress(QHostAddress::LocalHost) &&
              isLocalNetworkAddress(QHostAddress(QStringLiteral("fd12::1"))) &&
              isLocalNetworkAddress(QHostAddress(QStringLiteral("fe80::1"))) &&
              isLocalNetworkAddress(QHostAddress(QStringLiteral("::1"))),
          "every address that cannot leave the local network is a mesh peer");
    check(!isLocalNetworkAddress(QHostAddress(QStringLiteral("8.8.8.8"))) &&
              !isLocalNetworkAddress(QHostAddress(QStringLiteral("172.32.0.1"))) &&
              !isLocalNetworkAddress(QHostAddress(QStringLiteral("100.64.0.1"))) &&
              !isLocalNetworkAddress(QHostAddress(QStringLiteral("2606:4700::1"))) &&
              !isLocalNetworkAddress(QHostAddress()),
          "a routable address — including carrier NAT space — is never a peer");
    // An IPv4 peer reaching a dual-stack listener is reported mapped.
    check(isLocalNetworkAddress(QHostAddress(QStringLiteral("::ffff:192.168.5.5"))) &&
              !isLocalNetworkAddress(QHostAddress(QStringLiteral("::ffff:8.8.8.8"))),
          "the IPv4-mapped form a dual-stack accept reports is judged as IPv4");

    LocalMeshNode node(QStringLiteral("ada"), QStringLiteral("desk"),
                     QStringLiteral("ada"), QStringLiteral("mesh-policy"));
    node.setMulticastEnabledForTests(false);
    node.setBluetoothEnabledForTests(false);
    check(node.start(), "the mesh opens on a host with a local network");
    check(!node.connectToPeer(QHostAddress(QStringLiteral("93.184.216.34")), 48732),
          "the mesh refuses to dial an address off the local network");
    check(!node.connectToPeer(QHostAddress::LocalHost, 0),
          "the mesh refuses a peer with no port");
    node.stop();
}

// The mobile apps speak the same room. These are the constants every end
// hard-codes (native/ios MeshWire.swift, native/android LocalMeshRoom.kt); a
// change on one side without the other silently splits the room in two.
void testLocalMeshCrossClientConstants()
{
    using namespace forkmesh::localmesh;
    check(conversation() == QStringLiteral("#mesh") &&
              roomName() == QStringLiteral("forkmesh-lan-mesh-v1"),
          "the room the desktop joins is the one the phone joins");
    check(multicastGroupV4() == QStringLiteral("239.192.79.42") &&
              discoveryPort() == 48732,
          "both clients beacon to the same group and port");
    check(bluetoothServiceUuid() ==
              QStringLiteral("7f9e5c10-4d3b-4a21-9c6f-2b7ad1e05f11"),
          "both clients advertise the same RFCOMM service");
    check(bleServiceUuid() ==
                  QStringLiteral("7f9e5c14-4d3b-4a21-9c6f-2b7ad1e05f11") &&
              bleInboxUuid() ==
                  QStringLiteral("7f9e5c15-4d3b-4a21-9c6f-2b7ad1e05f11") &&
              bleOutboxUuid() ==
                  QStringLiteral("7f9e5c16-4d3b-4a21-9c6f-2b7ad1e05f11"),
          "the desktop looks for the BLE service the phones advertise");
    check(bleChunkBytes(-1) == 512 && bleChunkBytes(23) == 512 &&
              bleChunkBytes(185) == 182 && bleChunkBytes(517) == 512 &&
              bleMaxWriteBytes() == 512,
          "a BLE write fits the MTU, or leaves a long write to the stack");
    check(maxFrameBytes() == 24ll * 1024 * 1024,
          "both clients refuse a frame at the same size");
}

void testLocalMeshFraming()
{
    using namespace forkmesh::localmesh;
    const QByteArray payload = QByteArrayLiteral("{\"hello\":\"mesh\"}");
    QByteArray stream = frameBytes(payload) + frameBytes(payload);
    QByteArray out;
    bool overflow = false;
    check(takeFrame(stream, &out, &overflow) && out == payload && !overflow,
          "a framed payload comes back byte for byte");
    check(takeFrame(stream, &out, &overflow) && out == payload &&
              stream.isEmpty(),
          "back-to-back frames are read from one buffer");
    check(!takeFrame(stream, &out, &overflow) && !overflow,
          "an empty buffer yields no frame and no error");

    QByteArray partial = frameBytes(payload);
    partial.chop(3);
    check(!takeFrame(partial, &out, &overflow) && !overflow &&
              partial.size() == frameBytes(payload).size() - 3,
          "a half-arrived frame is left in the buffer untouched");

    QByteArray huge(4, char(0));
    const quint32 length = quint32(maxFrameBytes()) + 1;
    huge[0] = char((length >> 24) & 0xff);
    huge[1] = char((length >> 16) & 0xff);
    huge[2] = char((length >> 8) & 0xff);
    huge[3] = char(length & 0xff);
    check(!takeFrame(huge, &out, &overflow) && overflow,
          "a frame larger than the cap is refused rather than buffered");
}

void testLocalMeshBeaconIsCiphertext()
{
    using namespace forkmesh::localmesh;
    Beacon beacon;
    beacon.kid = QStringLiteral("AbCdEfGhIjKlMn01");
    beacon.port = 4242;
    beacon.timestampMs = 1750000000000ll;
    beacon.control = true;

    Beacon parsed;
    check(parseBeacon(beaconObject(beacon), &parsed) &&
              parsed.kid == beacon.kid && parsed.port == beacon.port &&
              parsed.control && parsed.timestampMs == beacon.timestampMs,
          "a beacon round-trips its id, port and control flag");
    const QJsonObject wire = beaconObject(beacon);
    QStringList keys = wire.keys();
    keys.sort();
    check(keys == QStringList({QStringLiteral("control"), QStringLiteral("id"),
                               QStringLiteral("port"), QStringLiteral("t"),
                               QStringLiteral("ts"), QStringLiteral("v")}) &&
              wire.value(QStringLiteral("v")).toInt() == 3,
          "a v3 beacon carries no name, account, platform or node key");
    QJsonObject withNode = wire;
    withNode.insert(QStringLiteral("node"), QStringLiteral("desk-node-key"));
    check(parseBeacon(withNode, &parsed) &&
              !beaconObject(parsed).contains(QStringLiteral("node")),
          "a node key in a received beacon is not kept or repeated");

    LocalMeshNode desk(QStringLiteral("desk"), QStringLiteral("workshop"),
                       QStringLiteral("desk"), QStringLiteral("mesh-beacon-desk"));
    desk.setMulticastEnabledForTests(false);
    desk.setAddressSweepEnabledForTests(false);
    desk.setBluetoothEnabledForTests(false);
    desk.setControlHandler([](QTcpSocket *, const QByteArray &, const QByteArray &) {});
    QStringList deskNotes;
    QObject::connect(&desk, &LocalMeshNode::systemMessage, &desk,
                     [&deskNotes](const QString &text) { deskNotes.append(text); });
    check(desk.start(), "the beacon test device opens the mesh");
    QString openLine;
    for (const QString &note : std::as_const(deskNotes))
        if (note.contains(QStringLiteral("is open over")) ||
            note.contains(QStringLiteral("is waiting")))
            openLine = note;
    check(openLine.contains(QStringLiteral("is open over")) &&
              openLine.contains(QStringLiteral("Anyone nearby")) &&
              !openLine.contains(QStringLiteral("end-to-end")),
          "the #mesh start line says anyone nearby can read the room");
    const QString firstId = desk.beaconId();
    check(firstId.size() == 16 && firstId != desk.sealKeyId() &&
              !desk.nodeId().contains(firstId),
          "the beacon id is random, not the device's key id or node key");
    desk.stop();
    check(desk.start() && desk.beaconId().size() == 16 && desk.beaconId() != firstId,
          "a new network session draws a new beacon id");
    desk.stop();

    QJsonObject wrongTag = beaconObject(beacon);
    wrongTag.insert(QStringLiteral("t"), QStringLiteral("something-else"));
    QJsonObject wrongVersion = beaconObject(beacon);
    wrongVersion.insert(QStringLiteral("v"), 1);
    QJsonObject noPort = beaconObject(beacon);
    noPort.insert(QStringLiteral("port"), 0);
    QJsonObject badKid = beaconObject(beacon);
    badKid.insert(QStringLiteral("id"), QStringLiteral("node-ada-1"));
    check(!parseBeacon(wrongTag, &parsed) && !parseBeacon(wrongVersion, &parsed) &&
              !parseBeacon(noPort, &parsed) && !parseBeacon(badKid, &parsed) &&
              !parseBeacon(QJsonObject(), &parsed),
          "anything that is not a v3 beacon is dropped");

    const RoomCrypto crypto(roomName());
    const QJsonObject envelope = crypto.encryptObject(beaconObject(beacon));
    const QByteArray datagram =
        QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    check(!datagram.contains("AbCdEfGhIjKlMn01") && !datagram.contains("desk-node-key") &&
              !datagram.contains("forkmesh-lan-mesh"),
          "the beacon on the wire is ciphertext, not an identity broadcast");
    check(crypto.decryptObject(envelope) == beaconObject(beacon),
          "a ForkMesh app on the same network decrypts the beacon");
}

void testLocalMeshRoom()
{
    using namespace forkmesh::localmesh;
    LocalMeshNode ada(QStringLiteral("ada"), QStringLiteral("workshop"),
                    QStringLiteral("ada"), QStringLiteral("mesh-node-ada"));
    LocalMeshNode grace(QStringLiteral("grace"), QStringLiteral("studio"),
                      QStringLiteral("grace"), QStringLiteral("mesh-node-grace"));
    // Discovery is exercised separately; these two are linked directly so the
    // room's behaviour does not depend on the host allowing multicast.
    ada.setMulticastEnabledForTests(false);
    grace.setMulticastEnabledForTests(false);
    ada.setBluetoothEnabledForTests(false);
    grace.setBluetoothEnabledForTests(false);
    check(ada.start() && grace.start(), "two devices open the mesh room");
    check(ada.listenPort() != 0 && grace.listenPort() != 0,
          "each device listens for peers on this network");

    QList<ChatMessage> adaSaw;
    QList<ChatMessage> graceSaw;
    QObject::connect(&ada, &LocalMeshNode::messageArrived, &ada,
                     [&adaSaw](const ChatMessage &m) { adaSaw.append(m); });
    QObject::connect(&grace, &LocalMeshNode::messageArrived, &grace,
                     [&graceSaw](const ChatMessage &m) { graceSaw.append(m); });

    check(ada.connectToPeer(QHostAddress::LocalHost, grace.listenPort()),
          "a device dials a peer it found on this network");
    check(waitUntil([&] { return ada.peerCount() == 1 && grace.peerCount() == 1; },
                    4000),
          "both devices end up in the same mesh room");

    const QList<MemberInfo> roster = grace.members();
    check(roster.size() == 2 && roster.first().self &&
              roster.at(1).name == QStringLiteral("ada") &&
              roster.at(1).nodeName == QStringLiteral("workshop") &&
              roster.at(1).online,
          "the room roster names the people discovered on this network");

    const QString sent = ada.sendChat(QStringLiteral("anyone here?"));
    check(!sent.isEmpty(), "a mesh message is accepted with no relay at all");
    check(waitUntil([&] { return !graceSaw.isEmpty(); }, 4000) &&
              graceSaw.first().text == QStringLiteral("anyone here?") &&
              graceSaw.first().conversation == conversation() &&
              graceSaw.first().senderName == QStringLiteral("ada") &&
              !graceSaw.first().self,
          "the message arrives at the other device over the local link");
    check(adaSaw.size() == 1 && adaSaw.first().self && adaSaw.first().id == sent,
          "the sender sees its own message once");
    check(ada.diagnosticsLine().contains(QStringLiteral("dial 1 ok 1 fail 0")) &&
              ada.diagnosticsLine().contains(QStringLiteral("links 1 keyed 1")) &&
              ada.diagnosticsLine().contains(
                  QStringLiteral("chat in 0 out 1 queued 0")),
          "the #mesh debug bar counts the dial, the link and the sent message");
    check(grace.diagnosticsLine().contains(QStringLiteral("fail 0 · in 1")) &&
              grace.diagnosticsLine().contains(QStringLiteral("chat in 1 out 0")) &&
              grace.diagnosticsDetail().contains(QStringLiteral("peer ada")),
          "the receiving side counts the inbound link and names the peer");

    QString typingPeer;
    bool typingActive = false;
    QObject::connect(&grace, &LocalMeshNode::typingChanged, &grace,
                     [&](const QString &conv, const QString &peerId,
                         const QString &, bool active) {
                         if (conv == conversation()) {
                             typingPeer = peerId;
                             typingActive = active;
                         }
                     });
    ada.sendTyping(true);
    check(waitUntil([&] { return typingActive; }, 4000) &&
              typingPeer == meshNode(QStringLiteral("mesh-node-ada")),
          "typing in the mesh room shows on the other device");

    // A third device joining later is handed the room's recent history.
    LocalMeshNode linus(QStringLiteral("linus"), QStringLiteral("laptop"),
                      QStringLiteral("linus"), QStringLiteral("mesh-node-linus"));
    linus.setMulticastEnabledForTests(false);
    linus.setBluetoothEnabledForTests(false);
    QList<ChatMessage> linusSaw;
    QObject::connect(&linus, &LocalMeshNode::messageArrived, &linus,
                     [&linusSaw](const ChatMessage &m) { linusSaw.append(m); });
    check(linus.start() &&
              linus.connectToPeer(QHostAddress::LocalHost, grace.listenPort()),
          "a third device joins the room");
    waitUntil([] { return false; }, 600);
    check(linusSaw.isEmpty(),
          "a device that joins late is not handed earlier messages re-sealed to "
          "a key nobody had seen when they were sent");

    // linus and ada never dialled each other: the room still reaches both.
    const QString relayed = ada.sendChat(QStringLiteral("second"));
    check(!relayed.isEmpty() &&
              waitUntil([&] { return linusSaw.size() == 1; }, 4000) &&
              linusSaw.at(0).text == QStringLiteral("second"),
          "a message crosses the mesh to a device with no direct link");
    check(waitUntil([&] { return graceSaw.size() == 2; }, 1000) &&
              graceSaw.at(1).text == QStringLiteral("second") &&
              adaSaw.size() == 2,
          "the relayed copy is delivered exactly once to every device");

    // grace keeps its link to linus, so only ada may disappear.
    ada.stop();
    const auto roomHasAda = [&] {
        for (const MemberInfo &member : grace.members())
            if (member.id == meshNode(QStringLiteral("mesh-node-ada")))
                return member.online;
        return false;
    };
    check(waitUntil([&] { return !roomHasAda(); }, 4000) &&
              grace.peerCount() == 1,
          "a device that leaves drops out of the room");
    grace.stop();
    linus.stop();
}

void testLocalMeshNoResealToDirectJoiner()
{
    using namespace forkmesh::localmesh;
    auto quiet = [](LocalMeshNode &node) {
        node.setMulticastEnabledForTests(false);
        node.setBluetoothEnabledForTests(false);
        node.setAddressSweepEnabledForTests(false);
    };
    LocalMeshNode ada(QStringLiteral("ada"), QString(), QString(),
                      QStringLiteral("mesh-direct-ada"));
    LocalMeshNode grace(QStringLiteral("grace"), QString(), QString(),
                        QStringLiteral("mesh-direct-grace"));
    LocalMeshNode carol(QStringLiteral("carol"), QString(), QString(),
                        QStringLiteral("mesh-direct-carol"));
    quiet(ada);
    quiet(grace);
    quiet(carol);
    QList<ChatMessage> graceSaw;
    QList<ChatMessage> carolSaw;
    QObject::connect(&grace, &LocalMeshNode::messageArrived, &grace,
                     [&graceSaw](const ChatMessage &m) { graceSaw.append(m); });
    QObject::connect(&carol, &LocalMeshNode::messageArrived, &carol,
                     [&carolSaw](const ChatMessage &m) { carolSaw.append(m); });
    check(ada.start() && grace.start() && carol.start(), "three devices open the room");
    ada.connectToPeer(QHostAddress::LocalHost, grace.listenPort());
    check(waitUntil([&] { return ada.peerCount() == 1 && grace.peerCount() == 1; }, 4000),
          "the first two devices link");
    const QString first = ada.sendChat(QStringLiteral("before carol"));
    check(waitUntil([&] { return !graceSaw.isEmpty(); }, 4000) && !ada.isQueued(first),
          "the first message is delivered to the device that was there");
    carol.connectToPeer(QHostAddress::LocalHost, ada.listenPort());
    check(waitUntil([&] { return carol.peerCount() >= 1; }, 4000),
          "a third device links straight to the sender");
    waitUntil([] { return false; }, 600);
    check(std::none_of(carolSaw.cbegin(), carolSaw.cend(),
                       [&](const ChatMessage &m) { return m.id == first; }),
          "a direct joiner is not handed earlier messages re-sealed to its key");
    const QString later = ada.sendChat(QStringLiteral("after carol"));
    check(waitUntil([&] {
              return std::any_of(carolSaw.cbegin(), carolSaw.cend(),
                                 [&](const ChatMessage &m) { return m.id == later; });
          }, 4000),
          "the joiner gets what is said once it is there");
    carol.stop();
    grace.stop();
    ada.stop();
}

void testLocalMeshQueuedUntilJoin()
{
    using namespace forkmesh::localmesh;
    QTemporaryDir dir;
    check(dir.isValid(), "a scratch directory for the mesh history");
    const QString historyFile = dir.filePath(QStringLiteral("mesh.json"));
    auto quiet = [](LocalMeshNode &node) {
        node.setMulticastEnabledForTests(false);
        node.setBluetoothEnabledForTests(false);
        node.setAddressSweepEnabledForTests(false);
    };

    QString sent;
    {
        LocalMeshNode alone(QStringLiteral("ada"), QStringLiteral("workshop"),
                            QStringLiteral("ada"), QStringLiteral("mesh-queue-ada"));
        quiet(alone);
        alone.setHistoryFile(historyFile);
        check(alone.start(), "a device alone opens the mesh room");
        bool queuedSignal = false;
        QObject::connect(&alone, &LocalMeshNode::queuedChanged, &alone,
                         [&queuedSignal](const QString &, bool queued) {
                             queuedSignal = queuedSignal || queued;
                         });
        sent = alone.sendChat(QStringLiteral("anyone out there?"));
        check(!sent.isEmpty() && alone.isQueued(sent) && queuedSignal,
              "a message sent with nobody linked is queued on this device");
        alone.stop();
    }
    check(QFileInfo::exists(historyFile),
          "the queue is written down, so quitting does not lose it");

    // The same device after a restart: the message is still there, still
    // queued, and shown as history rather than as a fresh arrival.
    LocalMeshNode ada(QStringLiteral("ada"), QStringLiteral("workshop"),
                      QStringLiteral("ada"), QStringLiteral("mesh-queue-ada"));
    quiet(ada);
    ada.setHistoryFile(historyFile);
    QList<ChatMessage> adaSaw;
    QObject::connect(&ada, &LocalMeshNode::messageArrived, &ada,
                     [&adaSaw](const ChatMessage &m) { adaSaw.append(m); });
    check(ada.start(), "the device reopens the room");
    check(adaSaw.size() == 1 && adaSaw.first().id == sent &&
              adaSaw.first().historical && adaSaw.first().self &&
              ada.isQueued(sent),
          "after a restart the queued message is back, and still queued");

    LocalMeshNode grace(QStringLiteral("grace"), QStringLiteral("studio"),
                        QStringLiteral("grace"), QStringLiteral("mesh-queue-grace"));
    quiet(grace);
    QList<ChatMessage> graceSaw;
    QObject::connect(&grace, &LocalMeshNode::messageArrived, &grace,
                     [&graceSaw](const ChatMessage &m) { graceSaw.append(m); });
    check(grace.start(), "a second device opens the room");
    ada.connectToPeer(QHostAddress::LocalHost, grace.listenPort());
    check(waitUntil([&] {
              return !ada.isQueued(sent) &&
                     std::any_of(graceSaw.cbegin(), graceSaw.cend(),
                                 [&](const ChatMessage &m) {
                                     return m.id == sent;
                                 });
          }, 4000),
          "the first device to join gets the queued message, and it is no "
          "longer queued");

    // A third device that links only to grace still gets ada's message: every
    // device hands a joiner the room's history, not just its own.
    LocalMeshNode carol(QStringLiteral("carol"), QStringLiteral("attic"),
                        QStringLiteral("carol"), QStringLiteral("mesh-queue-carol"));
    quiet(carol);
    QList<ChatMessage> carolSaw;
    QObject::connect(&carol, &LocalMeshNode::messageArrived, &carol,
                     [&carolSaw](const ChatMessage &m) { carolSaw.append(m); });
    check(carol.start(), "a third device opens the room");
    carol.connectToPeer(QHostAddress::LocalHost, grace.listenPort());
    check(waitUntil([&] { return carol.peerCount() == 1; }, 4000),
          "a later joiner links to the room");
    waitUntil([] { return false; }, 600);
    check(std::none_of(carolSaw.cbegin(), carolSaw.cend(),
                       [&](const ChatMessage &m) { return m.id == sent; }),
          "a message already delivered is not re-sealed to a later joiner's key");
    carol.stop();
    grace.stop();
    ada.stop();
}

void testLocalMeshWakesDevicesThatLeft()
{
    using namespace forkmesh::localmesh;
    check(wakeTokenExpiry(QStringLiteral("mw1.abc.42.sig")) == 42 &&
              wakeTokenExpiry(QStringLiteral("mw2.abc.42.sig")) == 0 &&
              wakeTokenExpiry(QStringLiteral("mw1.abc.42")) == 0 &&
              wakeTokenExpiry(QStringLiteral("mw1.abc.soon.sig")) == 0 &&
              wakeTokenExpiry(QStringLiteral("mw1.") + QString(200, QLatin1Char('a')) +
                              QStringLiteral(".42.sig")) == 0,
          "a wake token is read only in its own shape");
    QTemporaryDir dir;
    check(dir.isValid(), "a scratch directory for the mesh history");
    const QString historyFile = dir.filePath(QStringLiteral("mesh.json"));
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString phoneToken = QStringLiteral("mw1.%1.%2.sig")
                                   .arg(QString(64, QLatin1Char('a')))
                                   .arg(now + 3600000);
    const QString staleToken = QStringLiteral("mw1.%1.%2.sig")
                                   .arg(QString(64, QLatin1Char('c')))
                                   .arg(now - 1000);
    auto entry = [&](const QString &id, const QString &sender, const QString &wake) {
        return QJsonObject{{QStringLiteral("type"), QStringLiteral("chat")},
                           {QStringLiteral("id"), id},
                           {QStringLiteral("senderId"), sender},
                           {QStringLiteral("sender"), sender},
                           {QStringLiteral("conversation"), conversation()},
                           {QStringLiteral("ts"), double(now - 60000)},
                           {QStringLiteral("text"), QStringLiteral("hi")},
                           {QStringLiteral("wake"), wake}};
    };
    QFile file(historyFile);
    check(file.open(QIODevice::WriteOnly), "the seeded history is writable");
    file.write(QJsonDocument(QJsonObject{
                   {QStringLiteral("v"), 1},
                   {QStringLiteral("history"),
                    QJsonArray{entry(QStringLiteral("p-1"), QStringLiteral("phone"), phoneToken),
                               entry(QStringLiteral("o-1"), QStringLiteral("old"), staleToken)}},
                   {QStringLiteral("queued"), QJsonArray{}}})
                   .toJson(QJsonDocument::Compact));
    file.close();

    LocalMeshNode desk(QStringLiteral("desk"), QStringLiteral("workshop"),
                       QStringLiteral("desk"), QStringLiteral("mesh-wake-desk"));
    desk.setMulticastEnabledForTests(false);
    desk.setBluetoothEnabledForTests(false);
    desk.setAddressSweepEnabledForTests(false);
    desk.setHistoryFile(historyFile);
    QList<QStringList> woken;
    QObject::connect(&desk, &LocalMeshNode::wakeRequested, &desk,
                     [&woken](const QStringList &tokens) { woken.append(tokens); });
    check(desk.start(), "the desk opens the mesh room");
    check(desk.wakeTargets() == QStringList{phoneToken},
          "a phone that left is remembered by the token its messages carried");
    check(!desk.sendChat(QStringLiteral("are you there?")).isEmpty() &&
              woken == QList<QStringList>{QStringList{phoneToken}},
          "sending to #mesh asks to wake the phone, never an expired token");
    desk.stop();
}

void testLocalMeshReturningDeviceReplacesStaleLink()
{
    using namespace forkmesh::localmesh;
    auto quiet = [](LocalMeshNode &node) {
        node.setMulticastEnabledForTests(false);
        node.setBluetoothEnabledForTests(false);
        node.setAddressSweepEnabledForTests(false);
    };
    LocalMeshNode grace(QStringLiteral("grace"), QStringLiteral("studio"),
                        QStringLiteral("grace"), QStringLiteral("mesh-return-grace"));
    LocalMeshNode ada(QStringLiteral("ada"), QStringLiteral("workshop"),
                      QStringLiteral("ada"), QStringLiteral("mesh-return-ada"));
    quiet(grace);
    quiet(ada);
    grace.setLinkReplaceAfterForTests(200);
    check(grace.start() && ada.start(), "two devices open the room");
    grace.connectToPeer(QHostAddress::LocalHost, ada.listenPort());
    check(waitUntil([&] { return grace.peerCount() == 1 && ada.peerCount() == 1; },
                    4000),
          "the first link opens");
    waitUntil([] { return false; }, 300);

    LocalMeshNode back(QStringLiteral("ada"), QStringLiteral("workshop"),
                       QStringLiteral("ada"), QStringLiteral("mesh-return-ada"));
    quiet(back);
    QList<ChatMessage> backSaw;
    QObject::connect(&back, &LocalMeshNode::messageArrived, &back,
                     [&backSaw](const ChatMessage &m) { backSaw.append(m); });
    check(back.start() &&
              back.connectToPeer(QHostAddress::LocalHost, grace.listenPort()),
          "the device comes back and dials the room again");
    check(waitUntil([&] { return back.peerCount() == 1 && ada.peerCount() == 0; },
                    4000),
          "the room keeps the returning device's new link, not the stale one");
    ada.stop();
    const QString said = grace.sendChat(QStringLiteral("welcome back"));
    check(waitUntil([&] {
              return std::any_of(backSaw.cbegin(), backSaw.cend(),
                                 [&](const ChatMessage &m) { return m.id == said; });
          }, 4000),
          "messages reach the returning device over its new link");
    back.stop();
    grace.stop();
}

void testLocalMeshDiscovery()
{
    using namespace forkmesh::localmesh;
    LocalMeshNode first(QStringLiteral("first"), QStringLiteral("one"),
                      QStringLiteral("first"), QStringLiteral("mesh-discover-1"));
    LocalMeshNode second(QStringLiteral("second"), QStringLiteral("two"),
                       QStringLiteral("second"), QStringLiteral("mesh-discover-2"));
    // Separate discovery ports so each node reads only the beacons this case
    // hands it, instead of whatever else the host's network is carrying.
    first.setDiscoveryPortForTests(49731);
    second.setDiscoveryPortForTests(49732);
    first.setBeaconIntervalForTests(200);
    second.setBeaconIntervalForTests(200);
    first.setBluetoothEnabledForTests(false);
    second.setBluetoothEnabledForTests(false);
    first.setAddressSweepEnabledForTests(false);
    second.setAddressSweepEnabledForTests(false);
    check(first.start() && second.start(), "two devices open discovery");

    const RoomCrypto crypto(roomName());
    QUdpSocket wire;

    // Noise, and a beacon sealed for a different room, must not produce a peer.
    wire.writeDatagram(QByteArrayLiteral("not-a-beacon"), QHostAddress::LocalHost,
                       49731);
    Beacon foreign;
    foreign.kid = QString(16, QLatin1Char('o'));
    foreign.port = second.listenPort();
    foreign.timestampMs = QDateTime::currentMSecsSinceEpoch();
    const RoomCrypto otherRoom(QStringLiteral("some-other-room"));
    wire.writeDatagram(QJsonDocument(otherRoom.encryptObject(beaconObject(foreign)))
                           .toJson(QJsonDocument::Compact),
                       QHostAddress::LocalHost, 49731);
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    check(first.peerCount() == 0 && first.members().size() == 1,
          "a datagram this room cannot decrypt is not a mesh peer");

    // The real thing: second's beacon, reaching first exactly as a multicast
    // beacon would. first must decrypt it, dial the advertised port, and land
    // in the same room.
    Beacon beacon;
    beacon.kid = second.sealKeyId();
    beacon.port = second.listenPort();
    beacon.timestampMs = QDateTime::currentMSecsSinceEpoch();
    QElapsedTimer beat;
    beat.start();
    check(waitUntil([&] {
              if (beat.elapsed() >= 100) {
                  beat.restart();
                  beacon.timestampMs = QDateTime::currentMSecsSinceEpoch();
                  wire.writeDatagram(
                      QJsonDocument(crypto.encryptObject(beaconObject(beacon)))
                          .toJson(QJsonDocument::Compact),
                      QHostAddress::LocalHost, 49731);
              }
              return first.peerCount() == 1 && second.peerCount() == 1;
          }, 4000),
          "a beacon is all it takes to join the room — no configuration");
    const QList<MemberInfo> roster = first.members();
    check(roster.size() == 2 && roster.at(1).name == QStringLiteral("second"),
          "the discovered device is named in the roster");
    first.stop();
    second.stop();
}

void testLocalMeshUnicastDiscovery()
{
    using namespace forkmesh::localmesh;
    // The subnet sweep stays on the local segment and off this host.
    const QList<QHostAddress> home =
        sweepAddresses(QHostAddress(QStringLiteral("192.168.1.228")), 24);
    check(home.size() == 253 &&
              home.first() == QHostAddress(QStringLiteral("192.168.1.1")) &&
              home.last() == QHostAddress(QStringLiteral("192.168.1.254")) &&
              !home.contains(QHostAddress(QStringLiteral("192.168.1.228"))),
          "a /24 sweep covers every other host, not the network or broadcast");
    check(sweepAddresses(QHostAddress(QStringLiteral("10.4.0.9")), 8).size() == 253 &&
              sweepAddresses(QHostAddress(QStringLiteral("10.4.0.9")), -1).size() == 253,
          "a wide or unknown subnet is swept as the /24 around this host");
    check(sweepAddresses(QHostAddress(QStringLiteral("192.168.1.9")), 30) ==
              QList<QHostAddress>{QHostAddress(QStringLiteral("192.168.1.10"))},
          "a narrow subnet is swept exactly");
    check(sweepAddresses(QHostAddress(QStringLiteral("8.8.8.8")), 24).isEmpty() &&
              sweepAddresses(QHostAddress(QStringLiteral("127.0.0.1")), 8).isEmpty() &&
              sweepAddresses(QHostAddress(QStringLiteral("169.254.3.4")), 16).isEmpty() &&
              sweepAddresses(QHostAddress(QStringLiteral("192.168.1.9")), 32).isEmpty(),
          "a routable, loopback, link-local or point-to-point address is not swept");

    // A device whose dial to this node never lands — an iPhone behind no
    // multicast, dialling a machine whose firewall drops it — still links:
    // this node answers its beacon by unicast, and dials it as the higher id.
    LocalMeshNode high(QStringLiteral("high"), QStringLiteral("desk"),
                       QStringLiteral("high"), QStringLiteral("mesh-unicast-z"));
    LocalMeshNode low(QStringLiteral("low"), QStringLiteral("phone"),
                      QStringLiteral("low"), QStringLiteral("mesh-unicast-a"));
    high.setDiscoveryPortForTests(49733);
    high.setBeaconIntervalForTests(200);
    high.setAddressSweepEnabledForTests(false);
    low.setMulticastEnabledForTests(false);
    for (LocalMeshNode *node : {&high, &low})
        node->setBluetoothEnabledForTests(false);
    check(high.start() && low.start(), "both devices open");
    for (int tries = 0; tries < 32 && high.beaconId() <= low.beaconId(); ++tries) {
        high.stop();
        high.start();
    }
    check(high.beaconId() > low.beaconId(), "the beacon ids are ordered for the test");

    const RoomCrypto crypto(roomName());
    QUdpSocket wire;
    check(wire.bind(QHostAddress::LocalHost, 0), "the stand-in socket binds");
    Beacon beacon;
    beacon.kid = low.beaconId();
    beacon.port = low.listenPort();
    beacon.timestampMs = QDateTime::currentMSecsSinceEpoch();
    const auto sendBeacon = [&](bool reply) {
        beacon.reply = reply;
        wire.writeDatagram(QJsonDocument(crypto.encryptObject(beaconObject(beacon)))
                               .toJson(QJsonDocument::Compact),
                           QHostAddress::LocalHost, 49733);
    };
    // Sticky, because waitUntil() asks once more after the answer is read.
    const auto takeAnswer = [&](int timeoutMs, Beacon *out) {
        bool taken = false;
        return waitUntil([&] {
            while (!taken && wire.hasPendingDatagrams()) {
                QByteArray datagram(int(wire.pendingDatagramSize()), Qt::Uninitialized);
                datagram.truncate(int(wire.readDatagram(datagram.data(), datagram.size())));
                const QJsonObject plain =
                    crypto.decryptObject(QJsonDocument::fromJson(datagram).object());
                taken = parseBeacon(plain, out);
            }
            return taken;
        }, timeoutMs);
    };

    sendBeacon(false);
    Beacon answer;
    check(takeAnswer(2000, &answer) && answer.reply &&
              answer.kid == high.beaconId() && answer.kid != high.sealKeyId() &&
              answer.port == high.listenPort(),
          "a beacon from an unlinked device is answered by unicast");
    waitUntil([] { return false; }, 150);
    check(high.peerCount() == 0 && low.peerCount() == 0,
          "the higher id leaves the first dial to the lower id");

    sendBeacon(true);
    check(!takeAnswer(150, &answer), "a reply beacon is never answered");
    check(waitUntil([&] { return high.peerCount() == 1 && low.peerCount() == 1; },
                    4000),
          "the higher id dials a device that stays unlinked");
    high.stop();
    low.stop();
}

void testLocalMeshFirewall()
{
    using namespace forkmesh::localmesh;
    using namespace forkmesh::meshfirewall;
    // A host firewall is read from its own configuration, never by running it.
    check(ufwBlocksInbound(QStringLiteral("# ufw.conf\nENABLED=yes\nLOGLEVEL=low\n"),
                           QStringLiteral("IPV6=yes\nDEFAULT_INPUT_POLICY=\"DROP\"\n")) &&
              ufwBlocksInbound(QStringLiteral("ENABLED=yes\n"),
                               QStringLiteral("DEFAULT_INPUT_POLICY=\"REJECT\"\n")) &&
              ufwBlocksInbound(QStringLiteral("ENABLED=yes\n"), QString()),
          "an enabled ufw that does not accept inbound traffic blocks the mesh");
    check(!ufwBlocksInbound(QStringLiteral("ENABLED=no\n"),
                            QStringLiteral("DEFAULT_INPUT_POLICY=\"DROP\"\n")) &&
              !ufwBlocksInbound(QString(), QString()) &&
              !ufwBlocksInbound(QStringLiteral("#ENABLED=yes\n"), QString()) &&
              !ufwBlocksInbound(QStringLiteral("ENABLED=yes\n"),
                                QStringLiteral("DEFAULT_INPUT_POLICY=\"ACCEPT\"\n")),
          "a disabled, absent or accept-by-default ufw does not");

    // The rule is scoped to the addresses the mesh itself accepts, and names
    // no protocol so that it carries both the beacon and the link.
    const QString ufw = ufwAllowCommand(discoveryPort());
    bool scoped = true;
    for (const QString &range : localSourceRanges()) {
        scoped = scoped && ufw.contains(QStringLiteral("from %1 to any port 48732").arg(range)) &&
                 isLocalNetworkAddress(QHostAddress(range.section(QLatin1Char('/'), 0, 0)));
    }
    check(scoped && ufw.count(QStringLiteral("ufw allow")) == localSourceRanges().size() &&
              !ufw.contains(QStringLiteral("proto")) &&
              !ufw.contains(QStringLiteral("from any")) &&
              !ufw.contains(QStringLiteral("sudo")),
          "the ufw rule opens the mesh port to the local network only");
    const QString firewalld = firewalldAllowCommand(discoveryPort());
    check(firewalld.count(QStringLiteral("48732/tcp")) == 2 &&
              firewalld.count(QStringLiteral("48732/udp")) == 2 &&
              firewalld.contains(QStringLiteral("--permanent")),
          "the firewalld rule covers both protocols, now and after a restart");
    const QString windows = windowsAllowCommand(discoveryPort());
    check(windows.contains(QStringLiteral("protocol=TCP localport=48732")) &&
              windows.contains(QStringLiteral("protocol=UDP localport=48732")) &&
              windows.contains(QStringLiteral("profile=private,domain")) &&
              !windows.contains(QLatin1Char('\'')) && !windows.contains(QLatin1Char('"')),
          "the Windows rule stays off public networks and needs no quoting");

    // A firewall rule can only name a port that does not change, so links are
    // accepted on the beacon's own number; a second node on the machine finds
    // it taken and advertises whatever it got instead.
    LocalMeshNode first(QStringLiteral("first"), QStringLiteral("one"),
                        QStringLiteral("first"), QStringLiteral("mesh-firewall-1"));
    LocalMeshNode second(QStringLiteral("second"), QStringLiteral("two"),
                         QStringLiteral("second"), QStringLiteral("mesh-firewall-2"));
    for (LocalMeshNode *node : {&first, &second}) {
        node->setDiscoveryPortForTests(49734);
        node->setBeaconIntervalForTests(100);
        node->setAddressSweepEnabledForTests(false);
        node->setBluetoothEnabledForTests(false);
    }
    second.setMulticastEnabledForTests(false);
    check(first.start() && second.start(), "two devices on one machine open");
    check(first.listenPort() == 49734,
          "links are accepted on the well-known port");
    check(second.listenPort() != 0 && second.listenPort() != 49734,
          "a second node on the machine falls back to a free port");
    second.stop();

    // A device that keeps announcing itself but can never be linked is what a
    // firewall looks like from the inside; a link that opens ends the episode.
    int blockedSignals = 0;
    QObject::connect(&first, &LocalMeshNode::linksBlockedDetected, &first,
                     [&blockedSignals] { ++blockedSignals; });
    QTcpServer closed;
    check(closed.listen(QHostAddress::LocalHost, 0), "a port is reserved");
    const quint16 deadPort = closed.serverPort();
    closed.close();
    const RoomCrypto crypto(roomName());
    QUdpSocket wire;
    Beacon beacon;
    beacon.kid = QString(16, QLatin1Char('g'));
    beacon.port = deadPort;
    const auto announce = [&] {
        beacon.timestampMs = QDateTime::currentMSecsSinceEpoch();
        wire.writeDatagram(QJsonDocument(crypto.encryptObject(beaconObject(beacon)))
                               .toJson(QJsonDocument::Compact),
                           QHostAddress::LocalHost, 49734);
    };
    announce();
    waitUntil([] { return false; }, 120);
    check(!first.linksBlocked() && blockedSignals == 0,
          "one unanswered dial is not yet a firewall");
    QElapsedTimer sinceBeacon;
    sinceBeacon.start();
    check(waitUntil([&] {
              if (sinceBeacon.elapsed() >= 50) { // the ghost keeps its own beat
                  sinceBeacon.restart();
                  announce();
              }
              return first.linksBlocked();
          }, 3000) && blockedSignals == 1,
          "a device heard for several beats with no link reports blocked links");
    check(first.members().size() == 2,
          "the device that cannot be linked is still shown as nearby");
    first.stop();
}

void testNodeOwnersAndBlocks()
{
    const QString node = nodeKey(QStringLiteral("owner-node"));
    qint64 clock = 1000000;
    int calls = 0;
    QList<ChatNodeOwners::Reply> pending;
    ChatNodeOwners owners;
    owners.setClock([&clock] { return clock; });
    owners.setFetcher([&](const QString &asked, ChatNodeOwners::Reply reply) {
        ++calls;
        check(asked == node, "the lookup asks for the node key it was given");
        pending.append(reply);
    });
    check(owners.account(QStringLiteral("not-a-key")).isEmpty() && calls == 0,
          "a sender id that is not a node key is never looked up");
    check(owners.account(node).isEmpty() && owners.account(node).isEmpty() && calls == 1,
          "an unanswered node shows no account and is asked about once");
    QString resolvedAccount;
    QObject::connect(&owners, &ChatNodeOwners::resolved, &owners,
                     [&resolvedAccount](const QString &, const QString &account) {
                         resolvedAccount = account;
                     });
    pending.takeFirst()(true, QStringLiteral("Alice"));
    check(owners.account(node) == QLatin1String("alice") && resolvedAccount == "alice" &&
              owners.shownAccount(node, QStringLiteral("alice")) == QLatin1String("alice") &&
              owners.shownAccount(node, QString()) == QLatin1String("alice") &&
              owners.shownAccount(node, QStringLiteral("bob")).isEmpty(),
          "a verified account is shown only for a claim that matches it");
    clock += ChatNodeOwners::kAnswerLifeMs - 1;
    owners.account(node);
    check(calls == 1, "an answer is cached for an hour");
    clock += 2;
    check(owners.account(node) == QString() && calls == 2,
          "after an hour the node is asked about again");
    pending.takeFirst()(false, QString());
    owners.account(node);
    check(calls == 2, "a failed lookup is not retried at once");
    clock += ChatNodeOwners::kRetryAfterMs + 1;
    owners.account(node);
    check(calls == 3, "a failed lookup is retried after a minute");
    pending.takeFirst()(true, QString());
    check(owners.isAnswered(node) && owners.shownAccount(node, QStringLiteral("alice")).isEmpty(),
          "a node bound to no account shows every claim as a guest");
    QString parsed;
    check(ChatNodeOwners::parseAnswer(R"({"account":"Bob"})", &parsed) && parsed == "bob" &&
              ChatNodeOwners::parseAnswer(R"({"account":""})", &parsed) && parsed.isEmpty() &&
              !ChatNodeOwners::parseAnswer("[]", &parsed) &&
              !ChatNodeOwners::parseAnswer(R"({"account":3})", &parsed),
          "only the server's {account} answer is read");
    const QUrl lookup =
        ChatNodeOwners::lookupUrl(QUrl(QStringLiteral("https://app.example.org/x?y=1")), node);
    check(lookup.path() == QLatin1String("/api/chat/node-owner") &&
              QUrlQuery(lookup).queryItemValue(QStringLiteral("node")) == node &&
              lookup.host() == QLatin1String("app.example.org"),
          "the lookup is GET /api/chat/node-owner?node=<b64u>");

    QSettings().remove(QStringLiteral("chat/test-blocks"));
    QList<QJsonObject> pushed;
    ChatAccountBlocks blocks;
    blocks.setSettingsKey(QStringLiteral("chat/test-blocks"));
    blocks.setPusher([&pushed](const QJsonObject &body) { pushed.append(body); });
    blocks.setBlocked(QStringLiteral("@Mallory"), true);
    blocks.setBlocked(QStringLiteral("mallory"), true);
    blocks.setBlocked(QStringLiteral("not a name"), true);
    check(blocks.isBlocked(QStringLiteral("MALLORY")) && pushed.size() == 1 &&
              pushed.first() == QJsonObject{{QStringLiteral("accounts"),
                                             QJsonArray{QStringLiteral("mallory")}}},
          "a block is pushed once, lowercased, as the whole list");
    blocks.setBlocked(QStringLiteral("mallory"), false);
    check(!blocks.isBlocked(QStringLiteral("mallory")) && pushed.size() == 2 &&
              pushed.last().value(QStringLiteral("accounts")).toArray().isEmpty(),
          "an unblock pushes the shorter list");
    blocks.setBlocked(QStringLiteral("eve"), true);
    pushed.clear();
    blocks.mergeServerList(QJsonObject{
        {QStringLiteral("accounts"), QJsonArray{QStringLiteral("Trent"), QStringLiteral("eve")}}});
    check(blocks.accounts() == QStringList({QStringLiteral("eve"), QStringLiteral("trent")}) &&
              pushed.isEmpty(),
          "the server's list is read at sign-in");
    blocks.setBlocked(QStringLiteral("zed"), true);
    pushed.clear();
    blocks.mergeServerList(QJsonObject{{QStringLiteral("accounts"), QJsonArray{}}});
    check(blocks.accounts().size() == 3 && pushed.size() == 1 &&
              pushed.first().value(QStringLiteral("accounts")).toArray().size() == 3,
          "a block the server has not seen yet is pushed back, not lost");
    ChatAccountBlocks reloaded;
    reloaded.setSettingsKey(QStringLiteral("chat/test-blocks"));
    check(reloaded.isBlocked(QStringLiteral("zed")), "blocks are kept across restarts");
    QStringList many;
    for (int i = 0; i < 600; ++i)
        many.append(QStringLiteral("user%1").arg(i));
    check(ChatAccountBlocks::body(many).value(QStringLiteral("accounts")).toArray().size() ==
              ChatAccountBlocks::kMaxAccounts,
          "at most 500 accounts are sent");
    check(ChatAccountBlocks::endpoint(QUrl(QStringLiteral("https://app.example.org/a?b")))
                  .toString() == QLatin1String("https://app.example.org/api/chat/blocks"),
          "blocks live at /api/chat/blocks");
    QSettings().remove(QStringLiteral("chat/test-blocks"));
}

void testRelayRules()
{
    using namespace forkmesh::relayrules;
    const QSet<QString> relayPrivate = relayPrivateChannels(
        {QStringLiteral("#plans"), QStringLiteral("secret"), QStringLiteral("#team"),
         QStringLiteral("#office/design")});
    check(relayPrivate == QSet<QString>({QStringLiteral("#plans"), QStringLiteral("#secret")}),
          "the Team room and server channels are not relay private channels");
    const QJsonObject saved{
        {"current", QStringLiteral("@peer-node")},
        {"openDms", QJsonArray{QStringLiteral("peer-node")}},
        {"dmNames", QJsonObject{{QStringLiteral("peer-node"), QStringLiteral("Peer")}}},
        {"unread", QJsonObject{{QStringLiteral("@peer-node"), 2},
                               {QStringLiteral("#plans"), 1},
                               {QStringLiteral("#general"), 3}}},
        {"conversations",
         QJsonObject{{QStringLiteral("@peer-node"), QJsonArray{QJsonObject{{"id", "d1"}}}},
                     {QStringLiteral("#plans"),
                      QJsonArray{QJsonObject{{"id", "p1"}},
                                 QJsonObject{{"id", "p2"}, {"threadRootId", "p1"}}}},
                     {QStringLiteral("#team"), QJsonArray{QJsonObject{{"id", "t1"}}}},
                     {QStringLiteral("#mesh"), QJsonArray{QJsonObject{{"id", "m1"}}}},
                     {QStringLiteral("#office/@bob"), QJsonArray{QJsonObject{{"id", "o1"}}}},
                     {QStringLiteral("#general"), QJsonArray{QJsonObject{{"id", "g1"}}}}}}};
    int removed = 0;
    const QJsonObject purged = purgedChatHistory(saved, relayPrivate, &removed);
    const QJsonObject kept = purged.value(QStringLiteral("conversations")).toObject();
    QStringList keys = kept.keys();
    keys.sort();
    check(keys == QStringList({QStringLiteral("#general"), QStringLiteral("#mesh"),
                               QStringLiteral("#office/@bob"), QStringLiteral("#team")}) &&
              removed == 3,
          "saved history drops relay private channels and @peer conversations, threads included");
    check(!purged.contains(QStringLiteral("current")) &&
              purged.value(QStringLiteral("openDms")).toArray().isEmpty() &&
              purged.value(QStringLiteral("dmNames")).toObject().isEmpty() &&
              purged.value(QStringLiteral("unread")).toObject().keys() ==
                  QStringList{QStringLiteral("#general")},
          "their unread counts, open DMs and names go with them");
    int again = 0;
    purgedChatHistory(purged, relayPrivate, &again);
    check(again == 0, "a purged history has nothing left to drop");

    check(senderAccount(QStringLiteral("node"), QStringLiteral("#office/@Bob"), QString()) ==
                  QLatin1String("bob") &&
              senderAccount(QStringLiteral("user:Carol"), QStringLiteral("#general"),
                            QString()) == QLatin1String("carol") &&
              senderAccount(QStringLiteral("node"), QStringLiteral("#general"),
                            QStringLiteral("Dave")) == QLatin1String("dave") &&
              senderAccount(QStringLiteral("node"), QStringLiteral("#general"), QString())
                  .isEmpty(),
          "a sender's account comes from the server, never from a name it claims");
    check(profileAccount(QStringLiteral("node"), false, QStringLiteral("me"), QString(),
                         QString())
                  .isEmpty() &&
              profileAccount(QStringLiteral("node"), true, QStringLiteral("Me"), QString(),
                             QString()) == QLatin1String("me") &&
              profileAccount(QStringLiteral("user:eve"), false, QString(), QString(),
                             QString()) == QLatin1String("eve") &&
              profileAccount(QStringLiteral("node"), false, QString(), QString(),
                             QStringLiteral("frank")) == QLatin1String("frank"),
          "a guest has no account to message; a verified node does");
    check(botViaName(QStringLiteral("ForkBot"), QStringLiteral("Ada"), QStringLiteral("n")) ==
                  QStringLiteral("forkbot · via ada") &&
              botViaName(QStringLiteral("claude"), QString(), QStringLiteral("node-x")) ==
                  QStringLiteral("claude · via ") + guestLabel(QStringLiteral("node-x")) &&
              guestLabel(QStringLiteral("node-x")).size() == 10 &&
              guestLabel(QStringLiteral("node-x")) == guestLabel(QStringLiteral("node-x")) &&
              isNamedBot(QStringLiteral("Codex")) && !isNamedBot(QStringLiteral("alice")),
          "a bot line is labelled with the poster's account or guest name");
    check(webhooksAllowed(QStringLiteral("private")) &&
              webhooksAllowed(QStringLiteral("public")) &&
              !webhooksAllowed(QStringLiteral("notices")) &&
              webhookNote(QStringLiteral("private")).contains(QStringLiteral("server can read")),
          "private channels can have webhooks, with a note that the server reads them");
    check(!roomBadge(RoomKind::Public).tooltip.contains(QStringLiteral("controller")) &&
              roomBadge(RoomKind::Public).tooltip.contains(QStringLiteral("cannot read")) &&
              roomBadge(RoomKind::Mesh).tooltip.contains(QStringLiteral("Anyone nearby")) &&
              roomBadge(RoomKind::Mesh).text != QLatin1String("End-to-end") &&
              roomBadge(RoomKind::DeviceLocal).text != QLatin1String("End-to-end"),
          "neither #mesh nor the Team room is labelled end-to-end");
}

void testTeammateChatRelay()
{
    {
        ServerNode shared(QStringLiteral("Alice"), QString(), QStringLiteral("alice"),
                          nodeKey(QStringLiteral("shared-node")),
                          QUrl(QStringLiteral("ws://127.0.0.1:9/ws")),
                          forkmesh::mainnode::kDefaultRoomName, QString(),
                          QStringLiteral("test-room-passphrase"));
        QStringList privateJoined;
        QObject::connect(&shared, &ServerNode::privateChannelJoined, &shared,
                         [&privateJoined](const QString &name) { privateJoined.append(name); });
        shared.createPrivateChannel(QStringLiteral("#private"));
        shared.createPrivateChannel(QStringLiteral("#team"));
        check(shared.isSharedRoom() && privateJoined == QStringList{QStringLiteral("#team")},
              "a shared relay room makes no private channel except the local Team room");
    }
    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), QStringLiteral("stable-node-id"),
                    QUrl(QStringLiteral("ws://127.0.0.1:9/ws")),
                    forkmesh::mainnode::kDefaultRoomName, QString(),
                    QStringLiteral("test-room-passphrase"));
    node.setSharedRoom(false);
    QList<ChatMessage> messages;
    QObject::connect(&node, &ServerNode::messageArrived, &node,
                     [&messages](const ChatMessage &message) {
                         messages.append(message);
                     });
    node.sendTeammateChat(QStringLiteral(" #general "),
                          QStringLiteral("quality"), QStringLiteral("Quality"),
                          QStringLiteral("Reviewed the regression report."));
    check(messages.size() == 1 &&
              messages.first().conversation == QLatin1String("#general") &&
              messages.first().senderId == QLatin1String("teammate:quality") &&
              messages.first().senderName == QLatin1String("Quality") &&
              messages.first().text ==
                  QLatin1String("Reviewed the regression report.") &&
              !messages.first().self,
          "a teammate relay appears in a public room as the teammate actor");

    node.sendTeammateChat(QStringLiteral("@alice"), QStringLiteral("quality"),
                          QStringLiteral("Quality"), QStringLiteral("private"));
    node.createPrivateChannel(QStringLiteral("#private"));
    node.sendTeammateChat(QStringLiteral("#private"),
                          QStringLiteral("quality"), QStringLiteral("Quality"),
                          QStringLiteral("private"));
    check(messages.size() == 1,
          "a teammate relay cannot enter direct or private conversations");

    node.createPrivateChannel(QStringLiteral("#team"));
    node.sendTeammateChat(QStringLiteral("#team"),
                          QStringLiteral("quality"), QStringLiteral("Quality"),
                          QStringLiteral("Team update"));
    node.sendTeammateThreadReply(
        QStringLiteral("#team"), QStringLiteral("root-288"),
        QStringLiteral("quality"), QStringLiteral("Quality"),
        QStringLiteral("Human-facing reply"));
    node.sendTeamNotice(
        QStringLiteral("#team"), QStringLiteral("root-288"),
        QStringLiteral("Link your own account."));
    check(messages.size() == 4 &&
              messages.at(1).conversation == QLatin1String("#team") &&
              messages.at(1).senderId == QLatin1String("teammate:quality") &&
              messages.at(2).conversation == QLatin1String("#team") &&
              messages.at(2).threadRootId == QLatin1String("root-288") &&
              messages.at(2).text == QLatin1String("Human-facing reply") &&
              messages.at(3).senderId == QLatin1String("team-system") &&
              messages.at(3).threadRootId == QLatin1String("root-288") &&
              messages.at(3).text == QLatin1String("Link your own account."),
          "configured teammates and the local Team policy can reply in the private Team room");
    node.sendTeammateThreadReply(
        QStringLiteral("#private"), QStringLiteral("root-288"),
        QStringLiteral("quality"), QStringLiteral("Quality"),
        QStringLiteral("must stay out"));
    check(messages.size() == 4,
          "the private teammate exception is limited to the built-in Team room");

    node.sendBotThreadReply(QStringLiteral("#general"),
                            QStringLiteral("root-288"),
                            QStringLiteral("I can help with that."));
    check(messages.size() == 5 &&
              messages.last().conversation == QLatin1String("#general") &&
              messages.last().senderId == QLatin1String("forkbot") &&
              messages.last().threadRootId == QLatin1String("root-288") &&
              messages.last().text == QLatin1String("I can help with that."),
          "ForkBot replies stay in the public thread that mentioned it");
    node.sendBotThreadReply(QStringLiteral("#private"),
                            QStringLiteral("root-288"),
                            QStringLiteral("must stay public"));
    check(messages.size() == 5,
          "ForkBot cannot reply into a private thread");
}

QJsonObject decryptedFrame(const RoomCrypto &relayKey, const QByteArray &raw)
{
    return relayKey.decryptObject(QJsonDocument::fromJson(raw).object());
}

void testPublicIntegrationsAndPrivateSeal()
{
    WebSocketHarness harness;
    check(harness.listen(), "sealed-room harness listens");
    if (!harness.server.isListening())
        return;

    const QString room = forkmesh::mainnode::kDefaultRoomName;
    const QString roomPassphrase = QStringLiteral("test-room-passphrase");
    const QString aliceNode = nodeKey(QStringLiteral("stable-node-id"));

    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), aliceNode,
                    harness.url(), room, QString(), roomPassphrase);
    node.setNodeSigner(nodeSigner(QStringLiteral("stable-node-id")));
    QList<ChatMessage> messages;
    QObject::connect(&node, &ServerNode::messageArrived, &node,
                     [&messages](const ChatMessage &message) {
                         messages.append(message);
                     });
    QStringList editedTargets;
    QObject::connect(&node, &ServerNode::messageEdited, &node,
                     [&editedTargets](const QString &, const QString &target,
                                      const QString &) {
                         editedTargets.append(target);
                     });
    QStringList deletedTargets;
    QObject::connect(&node, &ServerNode::messageDeleted, &node,
                     [&deletedTargets](const QString &, const QString &target) {
                         deletedTargets.append(target);
                     });
    QStringList mismatchKeyIds;
    QObject::connect(&node, &ServerNode::roomSealMismatch, &node,
                     [&mismatchKeyIds](const QString &keyId) {
                         mismatchKeyIds.append(keyId);
                     });
    node.setTransportLimitsForTests(60, 20, 1 << 20);
    check(node.start(), "a policy-aware node starts against the test endpoint");
    check(waitUntil([&] { return connected(node); }, 3000),
          "a sealed node reaches the relay");

    RoomCrypto relayKey(room, roomPassphrase);
    check(waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "the node announces an E2EE public key on connect");
    QJsonObject nodeKeyFrame;
    for (const QByteArray &raw : harness.textFrames) {
        const QJsonObject inner = decryptedFrame(relayKey, raw);
        if (RoomSeal::isKeyFrame(inner)) {
            nodeKeyFrame = inner;
            break;
        }
    }
    check(RoomSeal::isKeyFrame(nodeKeyFrame) &&
              nodeKeyFrame.value("kid").toString() == node.roomSealKeyId() &&
              nodeKeyFrame.value("node").toString() == aliceNode &&
              forkmesh::chatv3::verifyNodeBinding(nodeKeyFrame, room),
          "the bundle names the node key and is signed by it");

    TestPeer peerSeal(room, room, RoomSeal::Policy::Open,
                      QStringLiteral("peer-node"), '\x11');
    check(peerSeal.accept(nodeKeyFrame),
          "a peer learns the node's signed bundle without a shared phrase");
    QJsonObject unbound = nodeKeyFrame;
    unbound.remove(QStringLiteral("nsig"));
    TestPeer wary(room, room, RoomSeal::Policy::Open, QStringLiteral("wary-node"), '\x12');
    check(!wary.accept(unbound), "a peer drops the same bundle without its node signature");
    const int framesBeforePeer = harness.textFrames.size();
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.keyFrame()))
                         .toJson(QJsonDocument::Compact));
    check(waitUntil(
              [&] { return harness.textFrames.size() > framesBeforePeer; }, 2000),
          "the node answers a newly discovered peer with its public key");
    check(waitUntil([&] { return node.roomSealPeerKeyIds().contains(peerSeal.keyId()); },
                    2000),
          "the node admits the peer's node-bound bundle");

    harness.textFrames.clear();
    node.sendChat(QStringLiteral("#general"), QStringLiteral("for everyone"));
    check(waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "a public chat reaches the relay");
    if (harness.textFrames.isEmpty())
        return;
    const QJsonObject publicEnvelope =
        QJsonDocument::fromJson(harness.textFrames.constLast()).object();
    const QJsonObject publicView = relayKey.decryptObject(publicEnvelope);
    check(publicEnvelope.value("persist").toBool() &&
              RoomSeal::isSealedFrame(publicView) &&
              !QJsonDocument(publicView).toJson().contains("for everyone") &&
              peerSeal.open(publicView).value("text").toString() ==
                  QLatin1String("for everyone"),
          "a public-channel message is sealed: the room key alone cannot read it");

    harness.textFrames.clear();
    node.sendBotChat(QStringLiteral("#general"), QStringLiteral("bot answer"));
    check(waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "a ForkBot line relayed by this device reaches the relay");
    const QJsonObject botPlain =
        harness.textFrames.isEmpty()
            ? QJsonObject()
            : peerSeal.open(decryptedFrame(relayKey, harness.textFrames.constLast()));
    check(botPlain.value("senderId").toString() == aliceNode &&
              botPlain.value("bot").toString() == QLatin1String("forkbot") &&
              botPlain.value("text").toString() == QLatin1String("bot answer"),
          "a bot line keeps the device's own sender id and names the bot apart");

    node.createPrivateChannel(QStringLiteral("#team"));
    messages.clear();
    harness.textFrames.clear();
    node.sendChat(QStringLiteral("#team"), QStringLiteral("local assignment"));
    QElapsedTimer localOnlyWait;
    localOnlyWait.start();
    while (localOnlyWait.elapsed() < 120)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    check(harness.textFrames.isEmpty() && messages.size() == 1 &&
              messages.first().conversation == QLatin1String("#team") &&
              messages.first().text == QLatin1String("local assignment"),
          "the private Team room works locally without writing a mesh frame");

    messages.clear();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonObject controller{{"type", QStringLiteral("chat")},
                           {"id", QStringLiteral("unsealed-1")},
                           {"senderId", peerSeal.node},
                           {"sender", QStringLiteral("Peer")},
                           {"channel", QStringLiteral("#general")},
                           {"text", QStringLiteral("unsealed claim")},
                           {"ts", double(now)}};
    harness.sendText(QJsonDocument(relayKey.encryptObject(controller))
                         .toJson(QJsonDocument::Compact));
    const auto sendServer = [&](const QJsonObject &plain) {
        QJsonObject envelope = relayKey.encryptObject(plain);
        envelope.insert(QStringLiteral("srv"), true);
        harness.sendText(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    };
    QJsonObject webhook{{"type", QStringLiteral("chat")},
                        {"id", QStringLiteral("webhook-1")},
                        {"senderId", QStringLiteral("webhook:abc")},
                        {"sender", QStringLiteral("Deploys")},
                        {"accountKind", QStringLiteral("user")},
                        {"ownerUser", QStringLiteral("alice")},
                        {"channel", QStringLiteral("#general")},
                        {"text", QStringLiteral("deploy finished")},
                        {"ts", double(now)}};
    sendServer(webhook);
    QJsonObject posing = webhook;
    posing["id"] = QStringLiteral("webhook-2");
    posing["senderId"] = peerSeal.node;
    posing["text"] = QStringLiteral("posing as a person");
    sendServer(posing);
    QJsonObject srvReply = webhook;
    srvReply["id"] = QStringLiteral("webhook-3");
    srvReply["type"] = QStringLiteral("thread-reply");
    srvReply["rootId"] = QStringLiteral("webhook-1");
    sendServer(srvReply);
    check(waitUntil([&] { return messages.size() >= 2; }, 2000),
          "frames the relay marks as its own reach the reader");
    waitUntil([] { return false; }, 100);
    QStringList receivedTexts;
    QString webhookSender;
    QString posingSender;
    for (const ChatMessage &message : std::as_const(messages)) {
        receivedTexts.append(message.text);
        if (message.id == QLatin1String("webhook-1"))
            webhookSender = message.senderId;
        if (message.id == QLatin1String("webhook-2"))
            posingSender = message.senderId;
    }
    check(!receivedTexts.contains(QStringLiteral("unsealed claim")) &&
              mismatchKeyIds.contains(QString()),
          "an unsealed frame without the relay's mark is refused");
    check(webhookSender == QLatin1String("webhook:abc") &&
              posingSender == QStringLiteral("srv:") + peerSeal.node &&
              posingSender != peerSeal.node,
          "a relay frame is shown as the integration it names, never as a person");
    check(std::none_of(messages.cbegin(), messages.cend(),
                       [](const ChatMessage &m) { return m.id == QLatin1String("webhook-3"); }),
          "a relay thread reply is not applied");

    QJsonObject peerMessage{{"type", QStringLiteral("chat")},
                            {"id", QStringLiteral("peer-message-1")},
                            {"senderId", peerSeal.node},
                            {"sender", QStringLiteral("Peer")},
                            {"channel", QStringLiteral("#general")},
                            {"text", QStringLiteral("peer said")},
                            {"ts", double(now)}};
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.sealObject(peerMessage)))
                         .toJson(QJsonDocument::Compact));
    QJsonObject privateFrame = peerMessage;
    privateFrame["id"] = QStringLiteral("peer-private-1");
    privateFrame["channel"] = QStringLiteral("#private");
    privateFrame["private"] = true;
    privateFrame["text"] = QStringLiteral("relay private channel");
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.sealObject(privateFrame)))
                         .toJson(QJsonDocument::Compact));
    QJsonObject remoteTeam = peerMessage;
    remoteTeam["id"] = QStringLiteral("remote-team-1");
    remoteTeam["channel"] = QStringLiteral("#team");
    remoteTeam["text"] = QStringLiteral("remote team injection");
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.sealObject(remoteTeam)))
                         .toJson(QJsonDocument::Compact));
    check(waitUntil([&] {
              return std::any_of(messages.cbegin(), messages.cend(), [](const ChatMessage &m) {
                  return m.id == QLatin1String("peer-message-1");
              });
          }, 2000),
          "a sealed public message from a peer arrives");

    const auto botLineFrom = [&](const QString &id) {
        QJsonObject botLine = peerMessage;
        botLine["id"] = id;
        botLine["bot"] = QStringLiteral("forkbot");
        botLine["sender"] = QStringLiteral("forkbot");
        botLine["text"] = QStringLiteral("I am ForkBot");
        harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.sealObject(botLine)))
                             .toJson(QJsonDocument::Compact));
        ChatMessage shown;
        waitUntil([&] {
            for (const ChatMessage &m : std::as_const(messages))
                if (m.id == id)
                    shown = m;
            return !shown.id.isEmpty();
        }, 2000);
        return shown;
    };
    const ChatMessage guestBot = botLineFrom(QStringLiteral("bot-line-1"));
    check(guestBot.senderId == QLatin1String("forkbot") &&
              guestBot.senderName == QStringLiteral("forkbot · via ") +
                                         forkmesh::relayrules::guestLabel(peerSeal.node),
          "a bot line from an unverified device names the guest that posted it");
    ChatNodeOwners botOwners;
    botOwners.setFetcher([](const QString &, ChatNodeOwners::Reply reply) {
        reply(true, QStringLiteral("peer-account-with-a-long-name"));
    });
    botOwners.account(peerSeal.node);
    node.setOwnerVerifier(&botOwners);
    const ChatMessage accountBot = botLineFrom(QStringLiteral("bot-line-2"));
    check(accountBot.senderName == QStringLiteral("forkbot · via peer-account-with-a-long-name"),
          "a bot line from a verified device names the account that posted it");
    node.setOwnerVerifier(nullptr);

    QStringList joined;
    QObject::connect(&node, &ServerNode::newMemberJoined, &node,
                     [&joined](const QString &name) { joined.append(name); });
    harness.sendText(QByteArrayLiteral(
        "{\"kind\":\"forkmesh-new-member\",\"v\":1,\"name\":\"forged\"}"));
    harness.sendText(QByteArrayLiteral(
        "{\"kind\":\"forkmesh-new-member\",\"v\":1,\"name\":\"newbie\",\"srv\":true}"));
    check(waitUntil([&] { return !joined.isEmpty(); }, 2000) &&
              joined == QStringList{QStringLiteral("newbie")},
          "a new-member announcement counts only when the relay injected it");
    waitUntil([] { return false; }, 100);
    receivedTexts.clear();
    for (const ChatMessage &message : std::as_const(messages))
        receivedTexts.append(message.text);
    check(!receivedTexts.contains(QStringLiteral("relay private channel")) &&
              !receivedTexts.contains(QStringLiteral("remote team injection")),
          "a shared room accepts no private-channel or Team frame");

    QJsonObject unsealedEdit{{"type", QStringLiteral("edit")},
                             {"id", QStringLiteral("unsealed-edit")},
                             {"senderId", peerSeal.node},
                             {"conversation", QStringLiteral("#general")},
                             {"target", QStringLiteral("peer-message-1")},
                             {"text", QStringLiteral("rewritten")},
                             {"ts", double(now)}};
    harness.sendText(QJsonDocument(relayKey.encryptObject(unsealedEdit))
                         .toJson(QJsonDocument::Compact));
    QJsonObject srvEdit = unsealedEdit;
    srvEdit["id"] = QStringLiteral("srv-edit");
    sendServer(srvEdit);
    QJsonObject srvDelete = unsealedEdit;
    srvDelete["id"] = QStringLiteral("srv-delete");
    srvDelete["type"] = QStringLiteral("delete");
    srvDelete["target"] = QStringLiteral("webhook-1");
    sendServer(srvDelete);
    TestPeer carol(room, room, RoomSeal::Policy::Open, QStringLiteral("carol-node"), '\x13');
    carol.accept(nodeKeyFrame);
    harness.sendText(QJsonDocument(relayKey.encryptObject(carol.keyFrame()))
                         .toJson(QJsonDocument::Compact));
    waitUntil([&] { return node.roomSealPeerKeyIds().contains(carol.keyId()); }, 2000);
    QJsonObject carolEdit = unsealedEdit;
    carolEdit["id"] = QStringLiteral("carol-edit");
    carolEdit["senderId"] = carol.node;
    harness.sendText(QJsonDocument(relayKey.encryptObject(carol.sealObject(carolEdit)))
                         .toJson(QJsonDocument::Compact));
    QJsonObject carolDelete = carolEdit;
    carolDelete["id"] = QStringLiteral("carol-delete");
    carolDelete["type"] = QStringLiteral("delete");
    harness.sendText(QJsonDocument(relayKey.encryptObject(carol.sealObject(carolDelete)))
                         .toJson(QJsonDocument::Compact));
    waitUntil([] { return false; }, 200);
    check(editedTargets.isEmpty() && deletedTargets.isEmpty(),
          "unsealed, relay or other people's edits and deletes are not applied");
    QJsonObject authorEdit = unsealedEdit;
    authorEdit["id"] = QStringLiteral("author-edit");
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.sealObject(authorEdit)))
                         .toJson(QJsonDocument::Compact));
    check(waitUntil([&] { return editedTargets == QStringList{QStringLiteral("peer-message-1")}; },
                    2000),
          "the authenticated author's sealed edit is applied");

    TestPeer otherSeal(room, room, RoomSeal::Policy::Open,
                       QStringLiteral("other-node"), '\x03');
    QJsonObject stranger = peerMessage;
    stranger["id"] = QStringLiteral("stranger-1");
    stranger["senderId"] = otherSeal.node;
    stranger["text"] = QStringLiteral("not for you");
    mismatchKeyIds.clear();
    harness.sendText(
        QJsonDocument(relayKey.encryptObject(otherSeal.sealObject(stranger)))
            .toJson(QJsonDocument::Compact));
    check(waitUntil([&] { return mismatchKeyIds.contains(otherSeal.keyId()); }, 2000),
          "an unaddressed sealed frame is reported by key id");

    node.shutdown();
}

void testServerFramesInChannelRooms()
{
    const QString room = QStringLiteral("chat-channel:") + QString(32, QLatin1Char('a')) +
                         QStringLiteral(":v1");
    const QString passphrase = QStringLiteral("channel-passphrase");
    RoomCrypto relayKey(room, passphrase);
    for (const bool direct : {false, true}) {
        WebSocketHarness harness;
        check(harness.listen(), "channel-room harness listens");
        if (!harness.server.isListening())
            return;
        const QString name = direct ? QStringLiteral("dm-room-node")
                                    : QStringLiteral("channel-room-node");
        ServerNode node(QStringLiteral("Ada"), QStringLiteral("ada"), QStringLiteral("ada"),
                        nodeKey(name), harness.url(), room, QString(), passphrase);
        OfficeChannelMirror::prepareRoomSender(node, [name](const QByteArray &message) {
            return forkmesh::chatv3::b64u(nodeSigner(name)(message));
        });
        node.setChatAccount(QStringLiteral("ada"));
        if (direct) {
            node.setDirectParticipants({QStringLiteral("ada"), QStringLiteral("bob")});
            node.setDirectRoom(true);
        } else {
            node.createPrivateChannel(QStringLiteral("#design"));
        }
        QList<ChatMessage> messages;
        QObject::connect(&node, &ServerNode::messageArrived, &node,
                         [&messages](const ChatMessage &m) { messages.append(m); });
        node.setTransportLimitsForTests(60, 20, 1 << 20);
        check(node.start() && waitUntil([&] { return connected(node); }, 3000),
              "a channel-room node reaches the relay");
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        const auto webhook = [&](const QString &id, bool srv) {
            QJsonObject envelope = relayKey.encryptObject(QJsonObject{
                {"type", QStringLiteral("chat")},
                {"id", id},
                {"senderId", QStringLiteral("webhook:hook")},
                {"sender", QStringLiteral("Deploys")},
                {"channel", QStringLiteral("#design")},
                {"private", true},
                {"webhook", true},
                {"serverVisible", true},
                {"text", QStringLiteral("deployed ") + id},
                {"ts", double(now)}});
            if (srv)
                envelope.insert(QStringLiteral("srv"), true);
            harness.sendText(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
        };
        webhook(QStringLiteral("plain-hook"), false);
        webhook(QStringLiteral("srv-hook"), true);
        waitUntil([&] { return !messages.isEmpty(); }, direct ? 400 : 2000);
        if (direct) {
            check(messages.isEmpty(), "a direct-message room accepts no relay frame");
        } else {
            check(messages.size() == 1 && messages.first().id == QLatin1String("srv-hook") &&
                      messages.first().senderId == QLatin1String("webhook:hook") &&
                      messages.first().senderName ==
                          QStringLiteral("Deploys · server-visible"),
                  "a private channel shows the relay's webhook as a server-visible "
                  "integration and drops the unmarked copy");
        }
        node.shutdown();
    }
}

void testTargetedConversationSealing()
{
    WebSocketHarness harness;
    check(harness.listen(), "targeted-seal harness listens");
    if (!harness.server.isListening())
        return;

    const QString room = forkmesh::mainnode::kDefaultRoomName;
    const QString roomPassphrase = QStringLiteral("targeted-seal-passphrase");
    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), nodeKey(QStringLiteral("targeted-node")),
                    harness.url(), room, QString(), roomPassphrase);
    node.setNodeSigner(nodeSigner(QStringLiteral("targeted-node")));
    QStringList notes;
    QObject::connect(&node, &ServerNode::systemMessage, &node,
                     [&notes](const QString &text) { notes.append(text); });
    QList<ChatMessage> messages;
    QObject::connect(&node, &ServerNode::messageArrived, &node,
                     [&messages](const ChatMessage &message) {
                         messages.append(message);
                     });
    QStringList channels;
    QObject::connect(&node, &ServerNode::channelsChanged, &node,
                     [&channels](const QStringList &list) { channels = list; });
    node.setTransportLimitsForTests(60, 20, 1 << 20);
    check(node.start(), "a targeted-seal node starts");
    check(waitUntil([&] { return connected(node); }, 3000),
          "a targeted-seal node reaches the relay");

    RoomCrypto relayKey(room, roomPassphrase);
    const auto inner = [&](const QByteArray &raw) {
        return relayKey.decryptObject(QJsonDocument::fromJson(raw).object());
    };
    QJsonObject nodeKeyFrame;
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(harness.textFrames))
                  if (RoomSeal::isKeyFrame(inner(raw)))
                      nodeKeyFrame = inner(raw);
              return !nodeKeyFrame.isEmpty();
          }, 2000),
          "the targeted-seal node announces its key");

    TestPeer bob(room, room, RoomSeal::Policy::Open, QStringLiteral("bob-node"),
                 '\x21');
    TestPeer carol(room, room, RoomSeal::Policy::Open,
                   QStringLiteral("carol-node"), '\x22');
    bob.accept(nodeKeyFrame);
    carol.accept(nodeKeyFrame);
    const auto relay = [&](const QJsonObject &object) {
        harness.sendText(QJsonDocument(relayKey.encryptObject(object))
                             .toJson(QJsonDocument::Compact));
    };
    relay(bob.keyFrame());
    relay(carol.keyFrame());
    check(waitUntil([&] { return node.roomSealPeerKeyIds().size() >= 2; }, 2000),
          "the node learns both room members' keys");

    const auto lastOfType = [&](const QString &type) {
        for (qsizetype i = harness.textFrames.size() - 1; i >= 0; --i) {
            const QJsonObject view = inner(harness.textFrames.at(i));
            if (!RoomSeal::isSealedFrame(view))
                continue;
            const QJsonObject forBob = bob.open(view);
            const QJsonObject forCarol = carol.open(view);
            const QJsonObject plain = forBob.isEmpty() ? forCarol : forBob;
            if (plain.value("type").toString() == type)
                return view;
        }
        return QJsonObject();
    };
    const auto waitFor = [&](const QString &type) {
        QJsonObject view;
        waitUntil([&] { return !(view = lastOfType(type)).isEmpty(); }, 2000);
        return view;
    };

    const auto relayedTypes = [&] {
        QStringList types;
        for (const QByteArray &raw : std::as_const(harness.textFrames)) {
            const QJsonObject view = inner(raw);
            QJsonObject plain = bob.open(view);
            if (plain.isEmpty())
                plain = carol.open(view);
            types.append(plain.value("type").toString());
        }
        return types;
    };
    const auto settle = [] {
        QElapsedTimer quietTimer;
        quietTimer.start();
        while (quietTimer.elapsed() < 150)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    };
    harness.textFrames.clear();
    messages.clear();
    node.sendDirect(bob.node, QStringLiteral("only for bob"));
    node.sendFile(QStringLiteral("@") + bob.node, QStringLiteral("a.txt"),
                  QStringLiteral("text/plain"), QByteArrayLiteral("file for bob"));
    node.sendReaction(QStringLiteral("@") + bob.node, QStringLiteral("some-id"),
                      QStringLiteral("+1"));
    node.sendTyping(QStringLiteral("@") + bob.node, true);
    node.sendRoomEdit(QStringLiteral("@") + bob.node, QStringLiteral("some-id"),
                      QStringLiteral("edited"));
    node.sendRoomDelete(QStringLiteral("@") + bob.node, QStringLiteral("some-id"));
    settle();
    const QStringList directTypes = relayedTypes();
    check(messages.isEmpty() && !directTypes.contains(QStringLiteral("dm")) &&
              !directTypes.contains(QStringLiteral("reaction")) &&
              !directTypes.contains(QStringLiteral("typing")) &&
              !directTypes.contains(QStringLiteral("edit")) &&
              !directTypes.contains(QStringLiteral("delete")) && !notes.isEmpty(),
          "nothing addressed to an @peer conversation is sent on the shared room");

    const qint64 sentAt = QDateTime::currentMSecsSinceEpoch();
    messages.clear();
    relay(bob.sealObject(QJsonObject{{"type", QStringLiteral("dm")},
                                     {"id", QStringLiteral("bob-dm-1")},
                                     {"senderId", bob.node},
                                     {"sender", QStringLiteral("Bob")},
                                     {"to", nodeKeyFrame.value("node").toString()},
                                     {"text", QStringLiteral("relay dm")},
                                     {"ts", double(sentAt)}}));
    relay(bob.sealObject(QJsonObject{{"type", QStringLiteral("cove-invite")},
                                     {"id", QStringLiteral("bob-cove-1")},
                                     {"senderId", bob.node},
                                     {"sender", QStringLiteral("Bob")},
                                     {"invitee", QStringLiteral("alice")},
                                     {"coveId", QStringLiteral("cove")},
                                     {"ts", double(sentAt)}}));
    relay(bob.sealObject(QJsonObject{{"type", QStringLiteral("reaction")},
                                     {"id", QStringLiteral("bob-react-1")},
                                     {"senderId", bob.node},
                                     {"conversation", QStringLiteral("@") + bob.node},
                                     {"target", QStringLiteral("x")},
                                     {"emoji", QStringLiteral("+1")},
                                     {"added", true},
                                     {"ts", double(sentAt)}}));
    bool coveInvited = false;
    QObject::connect(&node, &ServerNode::coveInvited, &node,
                     [&coveInvited](const QString &, const QString &, const QString &,
                                    const QString &, const QString &, qint64) {
                         coveInvited = true;
                     });
    relay(bob.sealObject(QJsonObject{{"type", QStringLiteral("chat")},
                                     {"id", QStringLiteral("bob-public-1")},
                                     {"senderId", bob.node},
                                     {"sender", QStringLiteral("Bob")},
                                     {"channel", QStringLiteral("#general")},
                                     {"text", QStringLiteral("public sentinel")},
                                     {"ts", double(sentAt)}}));
    check(waitUntil([&] { return !messages.isEmpty(); }, 2000) && messages.size() == 1 &&
              messages.first().id == QLatin1String("bob-public-1") && !coveInvited,
          "relay dm, @peer and cove frames are not accepted from the shared room");

    node.createPrivateChannel(QStringLiteral("#hidden-plans"));
    harness.textFrames.clear();
    node.sendChat(QStringLiteral("#hidden-plans"), QStringLiteral("not private here"));
    settle();
    bool markedPrivate = false;
    for (const QByteArray &raw : std::as_const(harness.textFrames))
        markedPrivate = markedPrivate ||
                        bob.open(inner(raw)).value("private").toBool();
    check(!harness.textFrames.isEmpty() && !markedPrivate,
          "the shared room has no relay private channel to mark a frame for");

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QJsonArray offered{QStringLiteral("#ok"), QStringLiteral("@bob-node"),
                       QStringLiteral("direct:abcdef"), QStringLiteral("#office/x"),
                       QStringLiteral("nohash"), QStringLiteral("#a@b"),
                       QStringLiteral("#") + QString(90, QLatin1Char('x'))};
    for (int i = 0; i < 400; ++i)
        offered.append(QStringLiteral("#flood-%1").arg(i));
    relay(bob.sealObject(QJsonObject{{"type", QStringLiteral("hello")},
                                     {"id", QStringLiteral("bob-hello-1")},
                                     {"senderId", bob.node},
                                     {"sender", QStringLiteral("Bob")},
                                     {"channels", offered},
                                     {"ts", double(now)}}));
    check(waitUntil([&] { return channels.contains(QStringLiteral("#ok")); }, 2000),
          "a well-formed channel name from a hello is adopted");
    bool rejected = true;
    for (const QString &name : std::as_const(channels))
        rejected = rejected && name.startsWith(QLatin1Char('#')) &&
                   !name.startsWith(QLatin1String("#office/")) &&
                   !name.contains(QLatin1Char('@')) && name.size() <= 80;
    check(rejected && !channels.contains(QStringLiteral("direct:abcdef")),
          "hello cannot plant direct, office, @ or oversized channel names");
    check(channels.size() <= 200, "hello cannot flood the channel list");

    relay(bob.sealObject(QJsonObject{{"type", QStringLiteral("channel")},
                                     {"id", QStringLiteral("bob-channel-1")},
                                     {"senderId", bob.node},
                                     {"sender", QStringLiteral("Bob")},
                                     {"name", QStringLiteral("@carol-node")},
                                     {"ts", double(now)}}));
    relay(bob.sealObject(QJsonObject{{"type", QStringLiteral("chat")},
                                     {"id", QStringLiteral("bob-sentinel-1")},
                                     {"senderId", bob.node},
                                     {"sender", QStringLiteral("Bob")},
                                     {"channel", QStringLiteral("#ok")},
                                     {"text", QStringLiteral("public line")},
                                     {"ts", double(now)}}));
    check(waitUntil([&] {
              for (const ChatMessage &m : std::as_const(messages))
                  if (m.id == QLatin1String("bob-sentinel-1"))
                      return true;
              return false;
          }, 2000) &&
              !channels.contains(QStringLiteral("@carol-node")),
          "a channel frame cannot plant a direct conversation name");

    relay(bob.sealObject(QJsonObject{{"type", QStringLiteral("chat")},
                                     {"id", QStringLiteral("bob-private-1")},
                                     {"senderId", bob.node},
                                     {"sender", QStringLiteral("Bob")},
                                     {"channel", QStringLiteral("#ok")},
                                     {"private", true},
                                     {"text", QStringLiteral("private line")},
                                     {"ts", double(now)}}));
    relay(bob.sealObject(QJsonObject{{"type", QStringLiteral("chat")},
                                     {"id", QStringLiteral("bob-sentinel-2")},
                                     {"senderId", bob.node},
                                     {"sender", QStringLiteral("Bob")},
                                     {"channel", QStringLiteral("#ok")},
                                     {"text", QStringLiteral("after private")},
                                     {"ts", double(now)}}));
    check(waitUntil([&] {
              for (const ChatMessage &m : std::as_const(messages))
                  if (m.id == QLatin1String("bob-sentinel-2"))
                      return true;
              return false;
          }, 2000) &&
              std::none_of(messages.cbegin(), messages.cend(), [](const ChatMessage &m) {
                  return m.id == QLatin1String("bob-private-1");
              }),
          "a frame marked private is refused on the shared room");
    const QString ownLine = node.sendChat(QStringLiteral("#ok"), QStringLiteral("my own line"));

    messages.clear();
    relay(carol.sealObject(QJsonObject{
        {"type", QStringLiteral("history")},
        {"id", QStringLiteral("carol-history-1")},
        {"senderId", carol.node},
        {"to", nodeKeyFrame.value("node").toString()},
        {"entries",
         QJsonArray{QJsonObject{{"type", QStringLiteral("chat")},
                                {"id", QStringLiteral("carol-old-1")},
                                {"senderId", carol.node},
                                {"sender", QStringLiteral("Carol")},
                                {"channel", QStringLiteral("#ok")},
                                {"text", QStringLiteral("carol earlier")},
                                {"ts", double(now - 1000)}},
                    QJsonObject{{"type", QStringLiteral("chat")},
                                {"id", QStringLiteral("forged-bob-1")},
                                {"senderId", bob.node},
                                {"sender", QStringLiteral("Bob")},
                                {"channel", QStringLiteral("#ok")},
                                {"text", QStringLiteral("words for bob")},
                                {"ts", double(now - 1000)}}}},
        {"ts", double(now)}}));
    check(waitUntil([&] { return !messages.isEmpty(); }, 2000) &&
              messages.size() == 1 && messages.first().id == QLatin1String("carol-old-1"),
          "a history reply speaks only for its own sender");

    harness.textFrames.clear();
    relay(carol.sealObject(QJsonObject{{"type", QStringLiteral("hello")},
                                       {"id", QStringLiteral("carol-hello-1")},
                                       {"senderId", carol.node},
                                       {"sender", QStringLiteral("Carol")},
                                       {"channels", QJsonArray{}},
                                       {"ts", double(now)}}));
    QJsonObject history;
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(harness.textFrames)) {
                  const QJsonObject plain = carol.open(inner(raw));
                  if (plain.value("type").toString() == QLatin1String("history"))
                      history = plain;
              }
              return !history.isEmpty();
          }, 2000),
          "a newcomer receives the room history");
    QStringList historyIds;
    for (const QJsonValue &entry : history.value("entries").toArray())
        historyIds.append(entry.toObject().value("id").toString());
    check(historyIds.contains(ownLine) &&
              !historyIds.contains(QStringLiteral("bob-sentinel-1")) &&
              !historyIds.contains(QStringLiteral("bob-private-1")),
          "history replay carries only this device's own messages");
    bool bobReadHistory = false;
    for (const QByteArray &raw : std::as_const(harness.textFrames))
        bobReadHistory = bobReadHistory ||
                         bob.open(inner(raw)).value("type").toString() ==
                             QLatin1String("history");
    check(!bobReadHistory, "history addressed to one peer is sealed to that peer");

    node.shutdown();
}

void testDeterministicRecovery()
{
    WebSocketHarness harness;
    check(harness.listen(), "local WebSocket harness listens");
    if (!harness.server.isListening())
        return;

    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), QStringLiteral("stable-node-id"),
                    harness.url(), forkmesh::mainnode::kDefaultRoomName,
                    QString(), QStringLiteral("test-room-passphrase"));
    bool connectionSignalState = false;
    QObject::connect(&node, &ServerNode::connectionChanged, &node,
                     [&connectionSignalState](bool connectedNow) {
                         connectionSignalState = connectedNow;
                     });
    node.setTransportLimitsForTests(60, 20, 2048);
    node.setNetworkAvailable(false);
    check(node.start(), "ServerNode starts against the test endpoint");
    waitUntil([] { return false; }, 100);
    check(harness.connectionCount == 0,
          "startup does not dial while the operating system is offline");
    node.setNetworkAvailable(true);

    check(waitUntil([&] {
              return harness.connectionCount == 2 && connected(node);
          }, 1200),
          "a blackholed CONNECTING attempt times out and reconnects");
    check(connectionSignalState,
          "transport users are notified when encrypted sending is ready");
    check(!harness.requests.isEmpty() &&
              harness.requests.last().startsWith(
                  "GET " + forkmesh::mainnode::kRoomPath.toUtf8() + " "),
          "Qt requests the world-general WebSocket endpoint");
    check(harness.requests.last().contains(
              "User-Agent: ForkMeshDesktop/") &&
              harness.requests.last().contains("user=Alice") &&
              harness.requests.last().contains("node=alice-laptop") &&
              harness.requests.last().contains("owner=alice") &&
              harness.requests.last().contains("room=world-general"),
          "the upgrade identifies the user, node, owner and room in cloud diagnostics");
    waitUntil([] { return false; }, 120);
    check(harness.connectionCount == 2,
          "one stalled attempt produces exactly one successful reconnect");

    const qint64 dropsBefore = backpressureDrops(node);
    node.sendChat(QStringLiteral("#general"), QString(16000, QLatin1Char('x')));
    check(waitUntil([&] {
              return backpressureDrops(node) > dropsBefore;
          }, 200),
          "oversized pending send is dropped by the socket backpressure budget");

    harness.sendAbnormalClose();
    check(waitUntil([&] {
              return harness.connectionCount == 3 && connected(node);
          }, 800),
          "an abnormal WebSocket close reconnects");
    waitUntil([] { return false; }, 120);
    check(harness.connectionCount == 3,
          "an abnormal close schedules exactly one reconnect");

    node.setNetworkAvailable(false);
    check(waitUntil([&] { return !connected(node); }, 200),
          "network loss tears down the live socket");
    check(!connectionSignalState,
          "transport users are notified when encrypted sending stops");
    const int offlineConnections = harness.connectionCount;
    waitUntil([] { return false; }, 120);
    check(harness.connectionCount == offlineConnections,
          "no reconnect is attempted while the network is unavailable");

    node.setNetworkAvailable(true);
    check(waitUntil([&] {
              return harness.connectionCount == offlineConnections + 1 &&
                     connected(node);
          }, 500),
          "network restoration performs one immediate reconnect");
    waitUntil([] { return false; }, 120);
    check(harness.connectionCount == offlineConnections + 1,
          "network restoration does not create duplicate connections");

    const int serverCloseConnectionCount = harness.connectionCount;
    const int closeFramesBeforeServerClose = harness.receivedCloseFrames;
    harness.sendCleanClose();
    check(waitUntil([&] {
              return harness.receivedCloseFrames > closeFramesBeforeServerClose;
          }, 300),
          "a server close frame is echoed to complete the WebSocket handshake");
    check(harness.lastClosePayload == QByteArray::fromHex("03e8"),
          "the echoed close preserves the server's status code");
    check(waitUntil([&] {
              return harness.connectionCount == serverCloseConnectionCount + 1 &&
                     connected(node);
          }, 800),
          "a clean server close still reconnects the active transport");

    const int closeFramesBeforeShutdown = harness.receivedCloseFrames;
    node.shutdown();
    check(waitUntil([&] {
              return harness.receivedCloseFrames > closeFramesBeforeShutdown;
          }, 300),
          "intentional shutdown sends a WebSocket close frame");
    check(harness.lastClosePayload == QByteArray::fromHex("03e8"),
          "intentional shutdown uses normal close status 1000");
    const int stoppedConnections = harness.connectionCount;
    waitUntil([] { return false; }, 120);
    check(harness.connectionCount == stoppedConnections,
          "intentional shutdown suppresses reconnects");
}


// The presence beat re-runs the roster flush every minute so peer staleness is
// re-evaluated without traffic. That sweep must not re-announce an unchanged
// status line: subscribers log every statusChanged, and a periodic repeat
// buried the network log under identical "N peer(s) online" entries. Pin that
// status is published on change only, and that transitional connection
// statuses still invalidate the sticky roster line so it is re-announced after
// a reconnect instead of leaving the UI stuck on "Reconnecting...".
void testStatusIsEventDriven()
{
    WebSocketHarness harness;
    check(harness.listen(), "local WebSocket harness listens for status tests");
    if (!harness.server.isListening())
        return;

    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), QStringLiteral("status-node-id"),
                    harness.url(), forkmesh::mainnode::kDefaultRoomName,
                    QString(), QStringLiteral("test-room-passphrase"));
    QStringList statuses;
    QObject::connect(&node, &ServerNode::statusChanged, &node,
                     [&statuses](const QString &status) {
                         statuses.append(status);
                     });
    node.setTransportLimitsForTests(60, 20, 2048);
    node.setPresenceIntervalForTests(40);
    check(node.start(), "ServerNode starts for the status contract test");

    const QString rosterStatus = QString::fromUtf8(
        "0 peer(s) online \xC2\xB7 encrypted room world-general");
    check(waitUntil([&] { return statuses.contains(rosterStatus); }, 1500),
          "the roster status is published once the room is joined");

    // Ten presence beats with nobody joining or leaving.
    const int settled = statuses.size();
    waitUntil([] { return false; }, 400);
    check(statuses.size() == settled,
          "an unchanged roster publishes no further status over many "
          "presence beats");
    check(statuses.count(rosterStatus) == 1,
          "the roster status is announced exactly once while it holds");

    for (int i = 1; i < statuses.size(); ++i) {
        check(statuses.at(i) != statuses.at(i - 1),
              "no status line is published twice in a row");
    }

    // A real transition still gets through, and it invalidates the sticky
    // roster line so the identical text is re-announced on the way back.
    node.setNetworkAvailable(false);
    check(waitUntil([&] { return !connected(node); }, 300),
          "network loss tears down the live socket");
    check(statuses.size() > settled && statuses.last() != rosterStatus,
          "a genuine transition is still published");

    node.setNetworkAvailable(true);
    check(waitUntil([&] {
              return connected(node) && statuses.count(rosterStatus) == 2;
          }, 1500),
          "the roster status is re-announced after a reconnect");

    node.shutdown();
}

void testTeardownMidBufferLeavesSocketAlone()
{
    WebSocketHarness harness;
    check(harness.listen(), "local WebSocket harness listens for teardown test");
    if (!harness.server.isListening())
        return;

    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), QStringLiteral("teardown-node-id"),
                    harness.url(), forkmesh::mainnode::kDefaultRoomName,
                    QString(), QStringLiteral("test-room-passphrase"));
    node.setTransportLimitsForTests(60, 20, 2048);
    check(node.start(), "ServerNode starts for the teardown test");
    check(waitUntil([&] { return connected(node) && harness.upgraded; }, 1500),
          "the node joins the room before the teardown frames");

    int framesSeen = 0;
    QObject::connect(&node, &ServerNode::socketFrame, &node,
                     [&node, &framesSeen](bool outgoing, const QString &,
                                          const QString &, qint64) {
                         if (outgoing)
                             return;
                         if (++framesSeen == 1)
                             node.setNetworkAvailable(false);
                     });
    harness.sendTextThenClose(QByteArrayLiteral("{\"type\":\"noop\"}"));
    check(waitUntil([&] { return framesSeen > 0 && !connected(node); }, 1500),
          "a frame whose handler drops the link is read without crashing on "
          "the close frame queued behind it");

    node.setNetworkAvailable(true);
    check(waitUntil([&] { return connected(node); }, 3000),
          "the node reconnects after the mid-buffer teardown");
    node.shutdown();
}

void testNestedPumpInFrameHandlerKeepsDeliveringSocket()
{
    WebSocketHarness harness;
    check(harness.listen(), "local WebSocket harness listens for nested pump test");
    if (!harness.server.isListening())
        return;

    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), QStringLiteral("nested-pump-node-id"),
                    harness.url(), forkmesh::mainnode::kDefaultRoomName,
                    QString(), QStringLiteral("test-room-passphrase"));
    node.setTransportLimitsForTests(60, 20, 2048);
    check(node.start(), "ServerNode starts for the nested pump test");
    check(waitUntil([&] { return connected(node) && harness.upgraded; }, 1500),
          "the node joins the room before the nested pump frame");

    const QPointer<QTcpSocket> delivering = node.findChild<QTcpSocket *>();
    check(!delivering.isNull(), "the node owns the socket delivering frames");
    int framesSeen = 0;
    bool aliveAfterPump = false;
    QObject::connect(
        &node, &ServerNode::socketFrame, &node,
        [&node, &framesSeen, &aliveAfterPump, &delivering](
            bool outgoing, const QString &, const QString &, qint64) {
            if (outgoing || ++framesSeen != 1)
                return;
            QTimer::singleShot(0, &node,
                               [&node] { node.setNetworkAvailable(false); });
            QElapsedTimer pump;
            pump.start();
            while (pump.elapsed() < 150)
                QCoreApplication::processEvents(QEventLoop::AllEvents, 12);
            aliveAfterPump = !delivering.isNull();
        });
    harness.sendText(QByteArrayLiteral("{\"type\":\"noop\"}"));
    check(waitUntil([&] { return framesSeen > 0 && !connected(node); }, 1500),
          "a frame handler that pumps events sees the link dropped");
    check(aliveAfterPump,
          "the socket whose readyRead is still on the stack survives a nested "
          "event pump that drops the link");
    check(waitUntil([&] { return delivering.isNull(); }, 1500),
          "the dropped socket is deleted once its delivery has unwound");

    node.setNetworkAvailable(true);
    check(waitUntil([&] { return connected(node); }, 3000),
          "the node reconnects after the nested pump teardown");
    node.shutdown();
}

// Away and the custom status ride hello/presence frames (the web and phone
// clients stamp the same "userStatus" object). A live frame sets the peer's
// status on the roster; a frame without the key, or a stale one, must leave
// it alone; and setting our own status sends a presence frame at once.
void testRecipientCounts()
{
    using namespace forkmesh::recipients;
    check(label(-1).isEmpty() && tooltip(-1).isEmpty(),
          "an unknown recipient count shows nothing");
    check(label(3) == QStringLiteral("to 3") &&
              tooltip(3) == QStringLiteral("Delivered to 3 people"),
          "a count reads to N with a people tooltip");
    check(label(1) == QStringLiteral("to 1") &&
              tooltip(1) == QStringLiteral("Delivered to 1 person"),
          "one recipient is one person");
    check(label(0) == QStringLiteral("to 0") &&
              tooltip(0) == QStringLiteral("Delivered to 0 people"),
          "zero recipients is a known count");
    check(countFromValue(QJsonValue(4)) == 4 &&
              countFromValue(QJsonValue(2.5)) == -1 &&
              countFromValue(QJsonValue(-1)) == -1 &&
              countFromValue(QJsonValue(QStringLiteral("3"))) == -1 &&
              countFromValue(QJsonValue(true)) == -1 &&
              countFromValue(QJsonValue(2000000)) == -1 &&
              countFromValue(QJsonValue()) == -1,
          "only a sane non-negative integer is a recipient count");
    const QStringList pair{QStringLiteral("alice"), QStringLiteral("bob")};
    check(tooltip(2, pair) ==
              QStringLiteral("Delivered to 2 people: @alice, @bob"),
          "the tooltip names who the message was delivered to");
    check(tooltip(1, {QStringLiteral("bob")}) ==
              QStringLiteral("Delivered to 1 person: @bob"),
          "one named recipient");
    check(tooltip(30, pair) ==
              QStringLiteral("Delivered to 30 people: @alice, @bob and 28 more"),
          "names past the cap are counted, not listed");
    check(tooltip(0, pair) == QStringLiteral("Delivered to 0 people"),
          "names never outnumber the count");
    check(namesFromValue(QJsonArray{QStringLiteral("bob"),
                                    QStringLiteral("Carol"),
                                    QStringLiteral("<b>x</b>"), 7,
                                    QStringLiteral("bob"),
                                    QStringLiteral("dave")}) ==
              QStringList({QStringLiteral("bob"), QStringLiteral("dave")}),
          "only distinct lowercase handles are recipient names");

    WebSocketHarness harness;
    check(harness.listen(), "recipient-count harness listens");
    if (!harness.server.isListening())
        return;
    const QString room = forkmesh::mainnode::kDefaultRoomName;
    const QString roomPassphrase = QStringLiteral("test-room-passphrase");
    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), nodeKey(QStringLiteral("recipients-node")),
                    harness.url(), room, QString(), roomPassphrase);
    QList<ChatMessage> messages;
    QObject::connect(&node, &ServerNode::messageArrived, &node,
                     [&messages](const ChatMessage &message) {
                         messages.append(message);
                     });
    QList<QPair<QString, int>> acks;
    QList<QStringList> ackNames;
    QObject::connect(&node, &ServerNode::messageRecipientsKnown, &node,
                     [&acks, &ackNames](const QString &messageId,
                                        int recipients,
                                        const QStringList &names) {
                         acks.append(qMakePair(messageId, recipients));
                         ackNames.append(names);
                     });
    node.setNodeSigner(nodeSigner(QStringLiteral("recipients-node")));
    node.setTransportLimitsForTests(60, 20, 1 << 20);
    check(node.start(), "a node starts for the recipient-count test");
    check(waitUntil([&] { return connected(node); }, 3000),
          "the recipient-count node reaches the relay");

    RoomCrypto relayKey(room, roomPassphrase);
    check(waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "the recipient-count node announces its key");
    QJsonObject nodeKeyFrame;
    for (const QByteArray &raw : harness.textFrames) {
        const QJsonObject inner =
            relayKey.decryptObject(QJsonDocument::fromJson(raw).object());
        if (RoomSeal::isKeyFrame(inner)) {
            nodeKeyFrame = inner;
            break;
        }
    }
    TestPeer peerSeal(room, room, RoomSeal::Policy::Open, QStringLiteral("bob-node"),
                      '\x07');
    check(peerSeal.accept(nodeKeyFrame),
          "the peer learns the recipient-count node's key");
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.keyFrame()))
                         .toJson(QJsonDocument::Compact));

    const auto sendPeerChat = [&](const QString &id, const QJsonValue &outer,
                                  const QJsonValue &inner) {
        QJsonObject frame{{"type", "chat"},
                          {"id", id},
                          {"senderId", peerSeal.node},
                          {"sender", QStringLiteral("bob")},
                          {"channel", QStringLiteral("#general")},
                          {"text", QStringLiteral("hello ") + id},
                          {"ts", double(QDateTime::currentMSecsSinceEpoch())}};
        if (!inner.isUndefined())
            frame.insert(QStringLiteral("recipients"), inner);
        QJsonObject envelope =
            relayKey.encryptObject(peerSeal.sealObject(frame));
        envelope.insert(QStringLiteral("persist"), true);
        envelope.insert(QStringLiteral("notify"), true);
        if (!outer.isUndefined())
            envelope.insert(QStringLiteral("recipients"), outer);
        if (outer.isDouble())
            envelope.insert(QStringLiteral("recipientNames"),
                            QJsonArray{QStringLiteral("alice"),
                                       QStringLiteral("carol")});
        harness.sendText(QJsonDocument(envelope).toJson(QJsonDocument::Compact));
    };
    const auto namesFor = [&messages](const QString &id) {
        for (const ChatMessage &message : std::as_const(messages))
            if (message.id == id)
                return message.recipientNames;
        return QStringList();
    };
    const auto countFor = [&messages](const QString &id) {
        for (const ChatMessage &message : std::as_const(messages))
            if (message.id == id)
                return message.recipientCount;
        return -2;
    };

    sendPeerChat(QStringLiteral("rc-outer"), QJsonValue(3), QJsonValue());
    check(waitUntil([&] { return countFor(QStringLiteral("rc-outer")) != -2; },
                    2000) &&
              countFor(QStringLiteral("rc-outer")) == 3,
          "the relay's outer recipients field reaches the chat message");
    check(namesFor(QStringLiteral("rc-outer")) ==
              QStringList({QStringLiteral("alice"), QStringLiteral("carol")}),
          "the relay's outer recipient names reach the chat message");
    sendPeerChat(QStringLiteral("rc-inner"), QJsonValue(), QJsonValue(9));
    check(waitUntil([&] { return countFor(QStringLiteral("rc-inner")) != -2; },
                    2000) &&
              countFor(QStringLiteral("rc-inner")) == -1,
          "a recipients field inside the sealed plain is ignored");
    sendPeerChat(QStringLiteral("rc-both"), QJsonValue(2), QJsonValue(40));
    check(waitUntil([&] { return countFor(QStringLiteral("rc-both")) != -2; },
                    2000) &&
              countFor(QStringLiteral("rc-both")) == 2,
          "only the outer envelope speaks for the count");
    sendPeerChat(QStringLiteral("rc-none"), QJsonValue(), QJsonValue());
    check(waitUntil([&] { return countFor(QStringLiteral("rc-none")) != -2; },
                    2000) &&
              countFor(QStringLiteral("rc-none")) == -1,
          "a frame without the field has an unknown count");
    sendPeerChat(QStringLiteral("rc-bad"), QJsonValue(1.5), QJsonValue());
    check(waitUntil([&] { return countFor(QStringLiteral("rc-bad")) != -2; },
                    2000) &&
              countFor(QStringLiteral("rc-bad")) == -1,
          "a fractional count is refused");

    harness.textFrames.clear();
    const QString sentId =
        node.sendChat(QStringLiteral("#general"), QStringLiteral("to everyone"));
    check(!sentId.isEmpty() &&
              waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "an own chat reaches the relay");
    if (harness.textFrames.isEmpty())
        return;
    const QByteArray sentFrame = harness.textFrames.constLast();
    const QString ref = QString::fromLatin1(
        QCryptographicHash::hash(sentFrame, QCryptographicHash::Sha256)
            .toHex()
            .left(40));
    check(countFor(sentId) == -1,
          "an own message starts with an unknown count");
    const auto sendAck = [&](const QString &ackRef, const QJsonValue &count) {
        harness.sendText(
            QJsonDocument(QJsonObject{{"kind", "forkmesh-recipients"},
                                      {"v", 1},
                                      {"ref", ackRef},
                                      {"recipients", count},
                                      {"recipientNames",
                                       QJsonArray{QStringLiteral("bob")}}})
                .toJson(QJsonDocument::Compact));
    };
    sendAck(QString(40, QLatin1Char('0')), QJsonValue(7));
    sendAck(ref, QJsonValue(4));
    check(waitUntil([&] { return !acks.isEmpty(); }, 2000) &&
              acks.size() == 1 && acks.first().first == sentId &&
              acks.first().second == 4,
          "the recipients ack with the frame's sha256 ref names the sent message");
    check(ackNames.size() == 1 &&
              ackNames.first() == QStringList({QStringLiteral("bob")}),
          "the recipients ack carries who received the message");
    acks.clear();
    sendAck(ref, QJsonValue(5));
    waitUntil([] { return false; }, 150);
    check(acks.isEmpty(), "a ref is answered once");
    node.shutdown();
}

void testDirectRoomResealsUndeliveredMessages()
{
    WebSocketHarness harness;
    check(harness.listen(), "direct reseal harness listens");
    if (!harness.server.isListening())
        return;
    const QString room = QStringLiteral("chat-direct:%1:v1").arg(QString(32, QLatin1Char('d')));
    const QString passphrase = QStringLiteral("direct-room-passphrase");
    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice"),
                    QStringLiteral("alice"), QStringLiteral("direct-reseal-node"),
                    harness.url(), room, QString(), passphrase);
    node.setDirectRoom(true);
    node.setTransportLimitsForTests(60, 20, 1 << 20);
    check(node.start(), "a direct-room node starts");
    check(waitUntil([&] { return connected(node); }, 3000),
          "the direct-room node reaches the relay");
    RoomCrypto relayKey(room, passphrase);
    const auto inner = [&](const QByteArray &raw) {
        return relayKey.decryptObject(QJsonDocument::fromJson(raw).object());
    };
    const auto outer = [](const QByteArray &raw) {
        return QJsonDocument::fromJson(raw).object();
    };
    const auto keyFrames = [&](bool persistedOnly) {
        int count = 0;
        for (const QByteArray &raw : std::as_const(harness.textFrames))
            if (RoomSeal::isKeyFrame(inner(raw)) &&
                (!persistedOnly || outer(raw).value("persist").toBool()))
                ++count;
        return count;
    };
    check(waitUntil([&] { return keyFrames(false) > 0; }, 2000),
          "the direct-room node announces its key");
    QJsonObject nodeKey;
    for (const QByteArray &raw : std::as_const(harness.textFrames))
        if (RoomSeal::isKeyFrame(inner(raw)))
            nodeKey = inner(raw);
    harness.textFrames.clear();

    const QString direct = QStringLiteral("direct:") + QString(32, QLatin1Char('d'));
    const QString sentId = node.sendChat(direct, QStringLiteral("are you there"));
    check(waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "a DM written before the peer's key is known reaches the relay");
    if (harness.textFrames.isEmpty())
        return;
    QByteArray firstFrame = harness.textFrames.constLast();
    for (const QByteArray &raw : std::as_const(harness.textFrames))
        if (RoomSeal::isSealedFrame(inner(raw)))
            firstFrame = raw;
    const QString firstRef = QString::fromLatin1(
        QCryptographicHash::hash(firstFrame, QCryptographicHash::Sha256)
            .toHex().left(40));
    check(inner(firstFrame).value("v").toInt() == 4 &&
              inner(firstFrame).value("keys").toArray().isEmpty(),
          "with no peer key the DM is still sent, as a v4 frame with no entries");

    TestPeer stranger(room, directScope(room), RoomSeal::Policy::Strict,
                      QStringLiteral("stranger-node"), '\x05');
    QJsonObject unreadable = relayKey.encryptObject(stranger.sealObject(
        QJsonObject{{"type", "chat"}, {"id", "not-for-us"},
                    {"senderId", "stranger-node"}, {"sender", "stranger"},
                    {"channel", direct}, {"text", "hidden"},
                    {"ts", double(QDateTime::currentMSecsSinceEpoch())}}));
    unreadable.insert("persist", true);
    unreadable.insert("ref", QString(40, QLatin1Char('a')));
    const int persistedBefore = keyFrames(true);
    harness.sendText(QJsonDocument(unreadable).toJson(QJsonDocument::Compact));
    check(waitUntil([&] { return keyFrames(true) > persistedBefore; }, 2000),
          "a DM not sealed for this device makes it retain its key on the relay");
    harness.sendText(QJsonDocument(unreadable).toJson(QJsonDocument::Compact));
    waitUntil([] { return false; }, 200);
    check(keyFrames(true) == persistedBefore + 1,
          "the retained key is sent once per connection");

    TestPeer bob(room, directScope(room), RoomSeal::Policy::Strict,
                 QStringLiteral("bob-node"), '\x07');
    bob.accept(nodeKey);
    QJsonObject bobKey = relayKey.encryptObject(bob.keyFrame());
    bobKey.insert("persist", true);
    bobKey.insert("ref", QString(40, QLatin1Char('b')));
    harness.textFrames.clear();
    harness.sendText(QJsonDocument(bobKey).toJson(QJsonDocument::Compact));
    QJsonObject resent;
    QJsonObject superseded;
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(harness.textFrames)) {
                  const QJsonObject envelope = outer(raw);
                  if (envelope.value("resend").toBool())
                      resent = envelope;
                  if (envelope.value("kind").toString() == "forkmesh-superseded")
                      superseded = envelope;
              }
              return !resent.isEmpty() && !superseded.isEmpty();
          }, 2000),
          "learning the peer's key re-seals the undelivered DM and supersedes it");
    const QJsonObject reopened = bob.open(relayKey.decryptObject(resent));
    check(reopened.value("id").toString() == sentId &&
              reopened.value("text").toString() == QStringLiteral("are you there"),
          "the peer can open the re-sealed DM under its original id");
    check(resent.value("persist").toBool() && resent.value("notify").toBool(),
          "the re-sealed DM is retained and counted like the original");
    check(superseded.value("refs").toArray() == QJsonArray{firstRef},
          "the superseded frame names the unreadable copy");
    bool keyAcked = false;
    for (const QByteArray &raw : std::as_const(harness.textFrames)) {
        const QJsonObject envelope = outer(raw);
        if (envelope.value("kind").toString() == "forkmesh-received" &&
            envelope.value("refs").toArray().contains(QString(40, QLatin1Char('b'))))
            keyAcked = true;
    }
    waitUntil([] { return false; }, 250);
    for (const QByteArray &raw : std::as_const(harness.textFrames)) {
        const QJsonObject envelope = outer(raw);
        if (envelope.value("kind").toString() == "forkmesh-received" &&
            envelope.value("refs").toArray().contains(QString(40, QLatin1Char('b'))))
            keyAcked = true;
    }
    check(!keyAcked, "a key advertisement is never acknowledged, so it stays on the relay");

    harness.sendText(QJsonDocument(QJsonObject{{"kind", "forkmesh-server-copies"},
                                               {"v", 1}, {"stored", 0}})
                         .toJson(QJsonDocument::Compact));
    TestPeer bobPhone(room, directScope(room), RoomSeal::Policy::Strict,
                      QStringLiteral("bob-node"), '\x09');
    harness.textFrames.clear();
    harness.sendText(QJsonDocument(relayKey.encryptObject(bobPhone.keyFrame()))
                         .toJson(QJsonDocument::Compact));
    waitUntil([] { return false; }, 300);
    bool resentAgain = false;
    for (const QByteArray &raw : std::as_const(harness.textFrames))
        resentAgain = resentAgain || outer(raw).value("resend").toBool();
    check(!resentAgain, "nothing is re-sealed once the relay holds no copies");
    check(node.roomSeal().pendingKeyIds() == QStringList{bobPhone.keyId()} &&
              !node.roomSeal().isApproved(bobPhone.keyId()),
          "a second device of the other person is pending in a strict DM");
    node.shutdown();
}

void testDirectRoomRatchetRecovery()
{
    WebSocketHarness harness;
    check(harness.listen(), "ratchet recovery harness listens");
    if (!harness.server.isListening())
        return;
    const QString conversationId(32, QLatin1Char('f'));
    const QString room = QStringLiteral("chat-direct:%1:v1").arg(conversationId);
    const QString passphrase = QStringLiteral("ratchet-recovery-passphrase");
    const QString direct = QStringLiteral("direct:") + conversationId;
    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice"),
                    QStringLiteral("alice"), QStringLiteral("ratchet-node"),
                    harness.url(), room, QString(), passphrase);
    node.setDirectRoom(true);
    node.setTransportLimitsForTests(60, 20, 1 << 20);
    check(node.start(), "a ratchet recovery node starts");
    check(waitUntil([&] { return connected(node); }, 3000),
          "the ratchet recovery node reaches the relay");
    RoomCrypto relayKey(room, passphrase);
    const auto inner = [&](const QByteArray &raw) {
        return relayKey.decryptObject(QJsonDocument::fromJson(raw).object());
    };
    const auto outer = [](const QByteArray &raw) {
        return QJsonDocument::fromJson(raw).object();
    };
    QJsonObject nodeKey;
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(harness.textFrames))
                  if (RoomSeal::isKeyFrame(inner(raw)))
                      nodeKey = inner(raw);
              return !nodeKey.isEmpty();
          }, 2000),
          "the ratchet recovery node announces its key");
    const QString nodeKid = nodeKey.value("kid").toString();
    const QString nodeId = nodeKey.value("node").toString();
    TestPeer bob(room, directScope(room), RoomSeal::Policy::Strict,
                 QStringLiteral("bob-node"), '\x0d');
    bob.accept(nodeKey);
    harness.sendText(QJsonDocument(relayKey.encryptObject(bob.keyFrame()))
                         .toJson(QJsonDocument::Compact));
    check(waitUntil([&] { return node.roomSealPeerKeyIds().size() >= 1; }, 2000),
          "the node learns the peer's device key");
    const auto keyFrames = [&]() {
        int count = 0;
        for (const QByteArray &raw : std::as_const(harness.textFrames))
            if (RoomSeal::isKeyFrame(inner(raw)) && outer(raw).value("persist").toBool())
                ++count;
        return count;
    };
    const auto acked = [&](const QString &ref) {
        for (const QByteArray &raw : std::as_const(harness.textFrames)) {
            const QJsonObject envelope = outer(raw);
            if (envelope.value("kind").toString() == "forkmesh-received" &&
                envelope.value("refs").toArray().contains(ref))
                return true;
        }
        return false;
    };

    harness.textFrames.clear();
    const QString sentId = node.sendChat(direct, QStringLiteral("ratchet hello"));
    check(waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "the DM reaches the relay");
    if (harness.textFrames.isEmpty())
        return;
    QByteArray sentRaw = harness.textFrames.constLast();
    for (const QByteArray &raw : std::as_const(harness.textFrames))
        if (RoomSeal::isSealedFrame(inner(raw)))
            sentRaw = raw;
    const QJsonObject sentSealed = inner(sentRaw);
    QStringList sentKids;
    for (const QJsonValue &value : sentSealed.value("keys").toArray())
        sentKids.append(value.toObject().value("kid").toString());
    check(sentSealed.value("v").toInt() == 4 && sentKids == QStringList{bob.keyId()},
          "a DM is a v4 frame with an entry for the peer and none for this device");
    check(bob.open(sentSealed).value("id").toString() == sentId,
          "the peer opens the v4 DM");

    harness.textFrames.clear();
    QJsonObject echo = outer(sentRaw);
    const QString echoRef(40, QLatin1Char('1'));
    echo.insert("ref", echoRef);
    harness.sendText(QJsonDocument(echo).toJson(QJsonDocument::Compact));
    waitUntil([] { return false; }, 300);
    check(!acked(echoRef) && keyFrames() == 0,
          "an own frame coming back is ignored: no ack and no key advertisement");

    node.roomSeal().resetSessions(bob.keyId());
    harness.textFrames.clear();
    QJsonObject broken = relayKey.encryptObject(bob.sealObject(QJsonObject{
        {"type", "chat"}, {"id", "after-loss"}, {"senderId", "bob-node"},
        {"channel", direct}, {"text", "lost session"},
        {"ts", double(QDateTime::currentMSecsSinceEpoch())}}));
    const QString brokenRef(40, QLatin1Char('2'));
    broken.insert("ref", brokenRef);
    broken.insert("persist", true);
    harness.sendText(QJsonDocument(broken).toJson(QJsonDocument::Compact));
    QJsonObject resetEnvelope;
    QJsonObject resetPlain;
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(harness.textFrames)) {
                  const QJsonObject sealed = inner(raw);
                  if (!RoomSeal::isSealedFrame(sealed))
                      continue;
                  const QJsonObject plain = bob.open(sealed);
                  if (RoomSeal::isResetPlain(plain)) {
                      resetEnvelope = outer(raw);
                      resetPlain = plain;
                  }
              }
              return !resetPlain.isEmpty() && keyFrames() == 1;
          }, 2000),
          "noSession sends a reset to the peer and one persisted key advertisement");
    check(resetPlain.value("senderId").toString() == nodeId &&
              resetEnvelope.value("persist").toBool() &&
              !resetEnvelope.value("notify").toBool() && !acked(brokenRef),
          "the reset is persisted, silent, names this node, and the broken frame is not acked");
    const QJsonObject resetSealed = relayKey.decryptObject(resetEnvelope);
    check(resetSealed.value("keys").toArray().size() == 1 &&
              resetSealed.value("keys").toArray().first().toObject().value("kid").toString() ==
                  bob.keyId(),
          "the reset is sealed only to the device whose session was lost");

    const QJsonObject fresh = relayKey.encryptObject(bob.sealObject(QJsonObject{
        {"type", "chat"}, {"id", "after-reset"}, {"senderId", "bob-node"},
        {"channel", direct}, {"text", "works again"},
        {"ts", double(QDateTime::currentMSecsSinceEpoch())}}));
    QList<ChatMessage> arrived;
    QObject::connect(&node, &ChatBackend::messageArrived, &node,
                     [&arrived](const ChatMessage &message) { arrived.append(message); });
    harness.sendText(QJsonDocument(fresh).toJson(QJsonDocument::Compact));
    check(waitUntil([&] { return !arrived.isEmpty(); }, 2000) &&
              arrived.first().id == QStringLiteral("after-reset"),
          "the peer's next message on the new session opens");

    harness.textFrames.clear();
    bob.seal.resetSessions(nodeKid);
    QJsonObject peerReset = relayKey.encryptObject(bob.seal.sealObjectFor(
        QJsonObject{{"type", "e2ee-reset"}, {"senderId", "bob-node"}}, {nodeKid}));
    peerReset.insert("persist", true);
    peerReset.insert("ref", QString(40, QLatin1Char('3')));
    const qsizetype shown = arrived.size();
    harness.sendText(QJsonDocument(peerReset).toJson(QJsonDocument::Compact));
    QJsonObject resent;
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(harness.textFrames))
                  if (outer(raw).value("resend").toBool())
                      resent = outer(raw);
              return !resent.isEmpty();
          }, 2000),
          "an opened reset re-seals the outbox to that device");
    waitUntil([] { return false; }, 300);
    bool superseded = false;
    for (const QByteArray &raw : std::as_const(harness.textFrames))
        superseded = superseded ||
                     outer(raw).value("kind").toString() == "forkmesh-superseded";
    const QJsonObject resentSealed = relayKey.decryptObject(resent);
    check(resentSealed.value("keys").toArray().size() == 1 &&
              bob.open(resentSealed).value("id").toString() == sentId &&
              resent.value("persist").toBool() && arrived.size() == shown && !superseded,
          "the outbox entry is re-sealed to that device alone, the reset is not shown, "
          "and nothing is superseded");

    node.roomSeal().resetSessions(bob.keyId());
    harness.textFrames.clear();
    const QByteArray lostAgain =
        QJsonDocument(relayKey.encryptObject(bob.sealObject(QJsonObject{
                          {"type", "chat"}, {"id", "lost-again"}, {"senderId", "bob-node"},
                          {"channel", direct}, {"text", "lost again"},
                          {"ts", double(QDateTime::currentMSecsSinceEpoch())}})))
            .toJson(QJsonDocument::Compact);
    harness.sendText(lostAgain);
    waitUntil([] { return false; }, 300);
    check(harness.textFrames.isEmpty(),
          "a second noSession from the same device within 60 s sends nothing");

    const auto nodeDevice = node.roomSeal().device();
    const qint64 later = QDateTime::currentMSecsSinceEpoch() + 61 * 1000;
    nodeDevice->setClock([later] { return later; });
    const QJsonObject sessionsBefore = nodeDevice->exportRatchets(room);
    harness.textFrames.clear();
    harness.sendText(QJsonDocument(broken).toJson(QJsonDocument::Compact));
    waitUntil([] { return false; }, 300);
    check(harness.textFrames.isEmpty() && !acked(brokenRef) &&
              nodeDevice->exportRatchets(room) == sessionsBefore,
          "a replayed noSession frame older than a frame already opened from that device "
          "starts no recovery: sessions kept, no reset, no key advertisement");
    harness.sendText(lostAgain);
    resetPlain = QJsonObject();
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(harness.textFrames)) {
                  const QJsonObject sealed = inner(raw);
                  if (RoomSeal::isSealedFrame(sealed) && RoomSeal::isResetPlain(bob.open(sealed)))
                      resetPlain = bob.open(sealed);
              }
              return !resetPlain.isEmpty() && keyFrames() == 1;
          }, 2000),
          "a noSession frame newer than every opened frame still recovers after 60 s");
    nodeDevice->setClock({});
    node.shutdown();
}

void testReceiptMarkStates()
{
    using namespace forkmesh::recipients;
    check(receiptMark(false, 3, Receipt::Read, true) == ReceiptMark::None,
          "someone else's message carries no receipt mark");
    check(receiptMark(true, -1, Receipt::None, false) == ReceiptMark::Sending,
          "an own message the relay has not acknowledged is sending");
    check(receiptMark(true, 0, Receipt::None, false) == ReceiptMark::Sent &&
              receiptMark(true, -1, Receipt::None, true) == ReceiptMark::Sent,
          "an acknowledged or already settled own message is sent");
    check(receiptMark(true, -1, Receipt::Delivered, false) ==
                  ReceiptMark::Delivered &&
              receiptMark(true, 1, Receipt::Read, false) == ReceiptMark::Read,
          "delivered and read outrank the relay acknowledgement");
    check(receiptLabel(ReceiptMark::Sending) == QStringLiteral("Sending") &&
              receiptLabel(ReceiptMark::Sent) == QStringLiteral("Sent") &&
              receiptLabel(ReceiptMark::Delivered) == QStringLiteral("Delivered") &&
              receiptLabel(ReceiptMark::Read) == QStringLiteral("Read") &&
              receiptLabel(ReceiptMark::None).isEmpty(),
          "every mark has the accessible name the other clients use");
    check(receiptFromName(receiptName(Receipt::Delivered)) == Receipt::Delivered &&
              receiptFromName(receiptName(Receipt::Read)) == Receipt::Read &&
              receiptFromName(QStringLiteral("bogus")) == Receipt::None &&
              receiptName(Receipt::None).isEmpty(),
          "the saved receipt state round-trips through the chat cache");
    check(!showsRecipientCount(QStringLiteral("#office/@bob")) &&
              showsRecipientCount(QStringLiteral("#office/design")) &&
              showsRecipientCount(QStringLiteral("#general")),
          "a direct message drops the to-N label, channels keep it");
    const QString ref(40, QLatin1Char('a'));
    const QJsonObject delivered{
        {"kind", "forkmesh-delivered"},
        {"v", 1},
        {"refs", QJsonArray{ref, ref, QStringLiteral("not-a-ref"), 7,
                            QString(40, QLatin1Char('b'))}}};
    check(isDeliveredFrame(delivered) &&
              !isReadFrame(delivered) &&
              isReadFrame(QJsonObject{{"kind", "forkmesh-read"}, {"v", 1}}) &&
              refsFromEnvelope(delivered) ==
                  QStringList({ref, QString(40, QLatin1Char('b'))}),
          "a delivered frame yields its distinct well-formed refs");
}

void testDirectReceiptsAndRoomMutations()
{
    WebSocketHarness harness;
    check(harness.listen(), "direct receipts harness listens");
    if (!harness.server.isListening())
        return;
    const QString conversationId(32, QLatin1Char('c'));
    const QString room = QStringLiteral("chat-direct:%1:v1").arg(conversationId);
    const QString passphrase = QStringLiteral("direct-receipts-passphrase");
    const QString direct = QStringLiteral("direct:") + conversationId;
    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice"),
                    QStringLiteral("alice"), QStringLiteral("receipts-node"),
                    harness.url(), room, QString(), passphrase);
    node.setDirectRoom(true);
    node.setTransportLimitsForTests(60, 20, 1 << 20);
    QList<QStringList> delivered;
    int reads = 0;
    QList<QJsonObject> mutations;
    QObject::connect(&node, &ServerNode::messagesDelivered, &node,
                     [&delivered](const QStringList &ids) { delivered.append(ids); });
    QObject::connect(&node, &ServerNode::conversationRead, &node,
                     [&reads] { ++reads; });
    QObject::connect(&node, &ServerNode::roomMutationArrived, &node,
                     [&mutations](const QJsonObject &plain) { mutations.append(plain); });
    check(node.start(), "a direct receipts node starts");
    check(waitUntil([&] { return connected(node); }, 3000),
          "the direct receipts node reaches the relay");
    RoomCrypto relayKey(room, passphrase);
    const auto inner = [&](const QByteArray &raw) {
        return relayKey.decryptObject(QJsonDocument::fromJson(raw).object());
    };
    QJsonObject nodeKey;
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(harness.textFrames))
                  if (RoomSeal::isKeyFrame(inner(raw)))
                      nodeKey = inner(raw);
              return !nodeKey.isEmpty();
          }, 2000),
          "the direct receipts node announces its key");
    TestPeer bob(room, directScope(room), RoomSeal::Policy::Strict,
                 QStringLiteral("bob-node"), '\x0b');
    bob.accept(nodeKey);
    harness.sendText(QJsonDocument(relayKey.encryptObject(bob.keyFrame()))
                         .toJson(QJsonDocument::Compact));
    check(waitUntil([&] { return node.roomSealPeerKeyIds().size() >= 1; }, 2000),
          "the node learns the other person's device key");
    const auto opened = [&](const QByteArray &raw) { return bob.open(inner(raw)); };
    const auto findFrame = [&](const QString &type, QByteArray *raw) {
        for (const QByteArray &frame : std::as_const(harness.textFrames)) {
            const QJsonObject plain = opened(frame);
            if (plain.value("type").toString() == type) {
                if (raw)
                    *raw = frame;
                return plain;
            }
        }
        return QJsonObject();
    };
    const auto relayFrame = [&](const QJsonObject &object) {
        harness.sendText(QJsonDocument(object).toJson(QJsonDocument::Compact));
    };

    harness.textFrames.clear();
    const QString sentId = node.sendChat(direct, QStringLiteral("see you soon"));
    QByteArray sentFrame;
    check(waitUntil([&] { return !findFrame("chat", &sentFrame).isEmpty(); },
                    2000),
          "an own direct message reaches the relay");
    const QString sentRef = forkmesh::recipients::frameRef(sentFrame);
    check(!sentId.isEmpty() && node.relayRefForMessage(sentId) == sentRef,
          "the node remembers the relay ref of its own message");

    relayFrame(QJsonObject{{"kind", "forkmesh-delivered"},
                           {"v", 1},
                           {"refs", QJsonArray{QString(40, QLatin1Char('0')),
                                               sentRef}}});
    check(waitUntil([&] { return !delivered.isEmpty(); }, 2000) &&
              delivered.first() == QStringList{sentId},
          "a delivered frame naming the ref marks that message delivered");
    relayFrame(QJsonObject{{"kind", "forkmesh-delivered"},
                           {"v", 1},
                           {"refs", QJsonArray{QString(40, QLatin1Char('9'))}}});
    relayFrame(QJsonObject{{"kind", "forkmesh-read"}, {"v", 1}});
    check(waitUntil([&] { return reads == 1; }, 2000) && delivered.size() == 1,
          "a read frame reaches the window and unknown refs are ignored");

    const QJsonObject ownReplay{
        {"type", "chat"},
        {"id", "own-from-phone"},
        {"senderId", "receipts-phone"},
        {"sender", "Alice"},
        {"channel", direct},
        {"text", "sent from my other device"},
        {"ts", double(QDateTime::currentMSecsSinceEpoch())}};
    TestPeer alicePhone(room, directScope(room), RoomSeal::Policy::Strict,
                        QStringLiteral("receipts-phone"), '\x0c',
                        QStringLiteral("receipts-node"));
    alicePhone.accept(nodeKey);
    alicePhone.seal.approve(nodeKey.value("kid").toString());
    harness.sendText(QJsonDocument(relayKey.encryptObject(alicePhone.keyFrame()))
                         .toJson(QJsonDocument::Compact));
    check(waitUntil([&] {
              return node.roomSeal().pendingKeyIds().contains(alicePhone.keyId());
          }, 2000),
          "a new device of this account is pending until approved");
    QJsonObject forged = ownReplay;
    forged.insert("id", QStringLiteral("forged-own"));
    forged.insert("senderId", QStringLiteral("receipts-node"));
    relayFrame(relayKey.encryptObject(bob.sealObject(forged)));
    node.approveSealDevice(alicePhone.keyId());
    QJsonObject ownEnvelope = relayKey.encryptObject(alicePhone.sealObject(ownReplay));
    const QString ownRef(40, QLatin1Char('e'));
    ownEnvelope.insert("ref", ownRef);
    ownEnvelope.insert("persist", true);
    relayFrame(ownEnvelope);
    check(waitUntil([&] {
              return node.relayRefForMessage(QStringLiteral("own-from-phone")) ==
                     ownRef;
          }, 2000),
          "an own relayed frame from an approved device remembers its ref");
    check(node.relayRefForMessage(QStringLiteral("forged-own")).isEmpty(),
          "another account's device cannot write as this account");
    relayFrame(QJsonObject{{"kind", "forkmesh-delivered"},
                           {"v", 1},
                           {"refs", QJsonArray{ownRef}}});
    check(waitUntil([&] { return delivered.size() == 2; }, 2000) &&
              delivered.last() == QStringList{QStringLiteral("own-from-phone")},
          "a delivered frame for a replayed own frame maps to its message");

    harness.textFrames.clear();
    const QString replyId = node.sendThreadReplyWithId(
        direct, sentId, QStringLiteral("a reply"), QStringLiteral("reply-1"));
    node.sendRoomReaction(direct, sentId, QStringLiteral("👍"), true);
    node.sendRoomEdit(direct, sentId, QStringLiteral("see you later"));
    const QString liveRef = node.relayRefForMessage(sentId);
    node.sendRoomDelete(direct, sentId);
    QJsonObject superseded;
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(harness.textFrames)) {
                  const QJsonObject envelope =
                      QJsonDocument::fromJson(raw).object();
                  if (envelope.value("kind").toString() == "forkmesh-superseded")
                      superseded = envelope;
              }
              return !superseded.isEmpty() &&
                     !findFrame("delete", nullptr).isEmpty();
          }, 2000),
          "deleting an own direct message sends the delete and withdraws the original");
    const QJsonObject reply = findFrame("thread-reply", nullptr);
    const QJsonObject reaction = findFrame("reaction", nullptr);
    const QJsonObject edit = findFrame("edit", nullptr);
    const QJsonObject deletion = findFrame("delete", nullptr);
    check(replyId == QStringLiteral("reply-1") &&
              reply.value("id").toString() == replyId &&
              reply.value("rootId").toString() == sentId &&
              reply.value("channel").toString() == direct &&
              reply.value("text").toString() == QStringLiteral("a reply"),
          "a reply is a sealed thread-reply frame under the root message");
    check(reaction.value("conversation").toString() == direct &&
              reaction.value("target").toString() == sentId &&
              reaction.value("emoji").toString() == QStringLiteral("👍") &&
              reaction.value("added").toBool() &&
              reaction.value("reactorId").toString() == QStringLiteral("receipts-node"),
          "a reaction is a sealed reaction frame naming its target");
    check(edit.value("conversation").toString() == direct &&
              edit.value("target").toString() == sentId &&
              edit.value("text").toString() == QStringLiteral("see you later") &&
              edit.value("editedAt").toDouble() > 0,
          "an edit is a sealed edit frame with its new text");
    check(deletion.value("conversation").toString() == direct &&
              deletion.value("target").toString() == sentId &&
              !liveRef.isEmpty() &&
              superseded.value("refs").toArray() == QJsonArray{liveRef},
          "the superseded frame names the deleted message's relay ref");

    for (const QString &type : {QStringLiteral("reaction"), QStringLiteral("edit"),
                                QStringLiteral("delete")}) {
        QJsonObject frame{{"type", type},
                          {"id", QStringLiteral("bob-") + type},
                          {"senderId", "bob-node"},
                          {"sender", "bob"},
                          {"conversation", direct},
                          {"target", sentId},
                          {"ts", double(QDateTime::currentMSecsSinceEpoch())}};
        if (type == QLatin1String("reaction")) {
            frame.insert("emoji", QStringLiteral("🎉"));
            frame.insert("reactorId", "bob-node");
            frame.insert("reactorName", "bob");
            frame.insert("added", true);
        }
        if (type == QLatin1String("edit"))
            frame.insert("text", QStringLiteral("forged"));
        QJsonObject envelope = relayKey.encryptObject(bob.sealObject(frame));
        envelope.insert("persist", true);
        relayFrame(envelope);
        relayFrame(envelope);
    }
    check(waitUntil([&] { return mutations.size() >= 3; }, 2000),
          "the other person's reaction, edit and delete frames reach the mirror");
    waitUntil([] { return false; }, 150);
    check(mutations.size() == 3 &&
              mutations.at(0).value("type").toString() == QStringLiteral("reaction") &&
              mutations.at(1).value("type").toString() == QStringLiteral("edit") &&
              mutations.at(2).value("type").toString() == QStringLiteral("delete") &&
              mutations.at(1).value("senderId").toString() == QStringLiteral("bob-node"),
          "each mutation is handed on once, with the author it claims");
    node.shutdown();
}

void testUserStatusOnPresenceFrames()
{
    WebSocketHarness harness;
    check(harness.listen(), "user-status harness listens");
    if (!harness.server.isListening())
        return;

    const QString room = forkmesh::mainnode::kDefaultRoomName;
    const QString roomPassphrase = QStringLiteral("test-room-passphrase");
    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), nodeKey(QStringLiteral("status-frame-node")),
                    harness.url(), room, QString(), roomPassphrase);
    QList<MemberInfo> roster;
    QObject::connect(&node, &ServerNode::rosterChanged, &node,
                     [&roster](const QList<MemberInfo> &members) {
                         roster = members;
                     });
    node.setNodeSigner(nodeSigner(QStringLiteral("status-frame-node")));
    node.setTransportLimitsForTests(60, 20, 1 << 20);
    NodeDiagnostics::hostCollector().setDataDir(QDir::tempPath());
    NodeDiagnostics::hostCollector().run();
    check(node.start(), "a node starts for the user-status frame test");
    check(waitUntil([&] { return connected(node); }, 3000),
          "the user-status node reaches the relay");

    RoomCrypto relayKey(room, roomPassphrase);
    check(waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "the node announces its key");
    QJsonObject nodeKeyFrame;
    for (const QByteArray &raw : harness.textFrames) {
        const QJsonObject inner =
            relayKey.decryptObject(QJsonDocument::fromJson(raw).object());
        if (RoomSeal::isKeyFrame(inner)) {
            nodeKeyFrame = inner;
            break;
        }
    }
    TestPeer peerSeal(room, room, RoomSeal::Policy::Open, QStringLiteral("bob-node"),
                      '\x15');
    check(peerSeal.accept(nodeKeyFrame), "the peer learns the node's public key");
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.keyFrame()))
                         .toJson(QJsonDocument::Compact));

    const auto sendPeerFrame = [&](const QString &id, const QString &type,
                                   qint64 ts, const QJsonValue &status) {
        QJsonObject frame{{"type", type},
                          {"id", id},
                          {"senderId", peerSeal.node},
                          {"sender", QStringLiteral("bob")},
                          {"ownerUser", QStringLiteral("bob")},
                          {"ts", double(ts)}};
        if (!status.isUndefined())
            frame.insert(QStringLiteral("userStatus"), status);
        harness.sendText(
            QJsonDocument(relayKey.encryptObject(peerSeal.sealObject(frame)))
                .toJson(QJsonDocument::Compact));
    };
    const QString bobNode = peerSeal.node;
    const auto bob = [&roster, bobNode]() -> MemberInfo {
        for (const MemberInfo &member : roster)
            if (member.id == bobNode)
                return member;
        return MemberInfo();
    };
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QJsonObject palm{{"away", true},
                           {"emoji", QStringLiteral("🌴")},
                           {"text", QStringLiteral("Vacationing")},
                           {"expiresAt", 0}};

    sendPeerFrame(QStringLiteral("bob-presence-1"), QStringLiteral("presence"),
                  now, palm);
    check(waitUntil([&] { return bob().hasUserStatus; }, 2000),
          "a live presence frame puts the peer's status on the roster");
    const MemberInfo withStatus = bob();
    check(withStatus.userStatus.away &&
              withStatus.userStatus.emoji == QStringLiteral("🌴") &&
              withStatus.userStatus.text == QStringLiteral("Vacationing"),
          "the roster carries the peer's away flag, emoji and text");

    // An older client's presence frame says nothing about the status.
    sendPeerFrame(QStringLiteral("bob-presence-2"), QStringLiteral("presence"),
                  now, QJsonValue(QJsonValue::Undefined));
    // A stale frame carrying a different status must not undo the live one.
    sendPeerFrame(QStringLiteral("bob-presence-3"), QStringLiteral("presence"),
                  now - 10 * 60 * 1000,
                  QJsonObject{{"away", false}, {"emoji", QStringLiteral("🚌")},
                              {"text", QStringLiteral("old")}, {"expiresAt", 0}});
    // Nor may a non-presence frame speak for it.
    sendPeerFrame(QStringLiteral("bob-typing-1"), QStringLiteral("typing"), now,
                  QJsonObject{{"away", false}, {"emoji", QStringLiteral("🎉")},
                              {"text", QStringLiteral("typed")}, {"expiresAt", 0}});
    waitUntil([] { return false; }, 400);
    check(bob().hasUserStatus && bob().userStatus.emoji == QStringLiteral("🌴") &&
              bob().userStatus.away,
          "frames without the key, stale frames and non-presence frames keep "
          "the status");

    sendPeerFrame(QStringLiteral("bob-hello-1"), QStringLiteral("hello"), now,
                  QJsonObject{{"away", false}, {"emoji", QString()},
                              {"text", QString()}, {"expiresAt", 0}});
    check(waitUntil([&] { return !bob().userStatus.away &&
                                 bob().userStatus.emoji.isEmpty(); }, 2000),
          "a live hello that clears the status clears it on the roster");

    // Our own status leaves at once, sealed to the peer, on a presence frame.
    harness.textFrames.clear();
    node.setUserStatus(QJsonObject{{"away", true},
                                   {"emoji", QStringLiteral("📅")},
                                   {"text", QStringLiteral("In a meeting")},
                                   {"expiresAt", double(now + 3600000)}});
    QJsonObject sentPresence;
    check(waitUntil(
              [&] {
                  for (const QByteArray &raw : harness.textFrames) {
                      const QJsonObject inner = peerSeal.open(
                          relayKey.decryptObject(
                              QJsonDocument::fromJson(raw).object()));
                      if (inner.value("type").toString() ==
                          QLatin1String("presence")) {
                          sentPresence = inner;
                          return true;
                      }
                  }
                  return false;
              },
              2000),
          "setting our status sends a presence frame immediately");
    const QJsonObject sentStatus =
        sentPresence.value(QStringLiteral("userStatus")).toObject();
    check(sentStatus.value("away").toBool() &&
              sentStatus.value("emoji").toString() == QStringLiteral("📅") &&
              sentStatus.value("text").toString() ==
                  QStringLiteral("In a meeting") &&
              qint64(sentStatus.value("expiresAt").toDouble()) == now + 3600000,
          "the presence frame carries our userStatus");
    check(NodeDiagnostics::hostCollector().lastRunMs() > 0 &&
              !sentPresence.contains(QStringLiteral("dg")) &&
              !sentPresence.contains(QStringLiteral("solana")),
          "a shared room's presence carries no diagnostics and no tip address");
    harness.textFrames.clear();
    node.setTipAddress(QStringLiteral("So1anaTipAddress111111111111111111111111111"));
    QJsonObject tipHello;
    waitUntil([&] {
        for (const QByteArray &raw : harness.textFrames) {
            const QJsonObject inner = peerSeal.open(
                relayKey.decryptObject(QJsonDocument::fromJson(raw).object()));
            if (inner.value("type").toString() == QLatin1String("hello"))
                tipHello = inner;
        }
        return !tipHello.isEmpty();
    }, 2000);
    check(tipHello.value(QStringLiteral("solana")).toString() ==
              QStringLiteral("So1anaTipAddress111111111111111111111111111"),
          "a tip address the user chose to share goes out with the next hello");

    check(bob().ownerUser.isEmpty() && bob().accountKind == QLatin1String("guest"),
          "an account claim nobody verified is shown as a guest");
    QStringList asked;
    ChatNodeOwners owners;
    owners.setFetcher([&asked](const QString &node, ChatNodeOwners::Reply reply) {
        asked.append(node);
        QTimer::singleShot(0, [reply] { reply(true, QStringLiteral("bob")); });
    });
    node.setOwnerVerifier(&owners);
    check(waitUntil([&] { return bob().ownerUser == QLatin1String("bob"); }, 2000) &&
              asked == QStringList{bobNode},
          "the server's answer for the node key lets the roster show the account");
    ChatNodeOwners liars;
    liars.setFetcher([](const QString &, ChatNodeOwners::Reply reply) {
        reply(true, QStringLiteral("mallory"));
    });
    node.setOwnerVerifier(&liars);
    check(waitUntil([&] { return bob().ownerUser.isEmpty() &&
                                 bob().accountKind == QLatin1String("guest"); }, 2000),
          "a claim that does not match the node's account is shown as a guest");
    node.setOwnerVerifier(nullptr);
    check(waitUntil(
              [&] {
                  return !roster.isEmpty() && roster.first().self &&
                         roster.first().hasUserStatus &&
                         roster.first().userStatus.away;
              },
              2000),
          "our own roster row carries the status we set");

    // Signing out forgets the status: the next presence frame omits the key
    // entirely rather than advertising an empty status peers would trust.
    harness.textFrames.clear();
    node.setUserStatus(QJsonObject());
    QJsonObject unknownPresence;
    check(waitUntil(
              [&] {
                  for (const QByteArray &raw : harness.textFrames) {
                      const QJsonObject inner = peerSeal.open(
                          relayKey.decryptObject(
                              QJsonDocument::fromJson(raw).object()));
                      if (inner.value("type").toString() ==
                          QLatin1String("presence")) {
                          unknownPresence = inner;
                          return true;
                      }
                  }
                  return false;
              },
              2000),
          "forgetting our status sends a presence frame");
    check(!unknownPresence.isEmpty() &&
              !unknownPresence.contains(QStringLiteral("userStatus")),
          "a node without a known status omits userStatus");

    node.shutdown();
}

// The World office's channel rooms reach the desktop over signed history and
// room-access endpoints. Pin the proof strings the account key signs (the
// Worker verifies the same bytes), the conversation naming, and decryption.
void testOfficeChannelMirror()
{
    using namespace forkmesh::office;

    check(conversationForChannel(QStringLiteral("design")) ==
              QStringLiteral("#office/design"),
          "office rooms are namespaced so they can't merge with a mesh room");
    check(conversationForChannel(QStringLiteral("  ")).isEmpty(),
          "a nameless office room has no conversation");
    check(isOfficeConversation(QStringLiteral("#office/design")) &&
              !isOfficeConversation(QStringLiteral("#general")),
          "only office mirrors are recognized as office conversations");

    check(channelListProof(QStringLiteral("ada"), QStringLiteral("17")) ==
              QByteArray("forkmesh-chat-channels-v1\nada\n17"),
          "channel-list proof matches CHAT_CHANNEL_LIST_PROOF");
    check(channelHistoryProof(QStringLiteral("ada"), QString(32, 'a'),
                              QStringLiteral("17")) ==
              QByteArray("forkmesh-chat-channel-history-v1\nada\n") +
                  QByteArray(32, 'a') + QByteArray("\n17"),
          "channel-history proof matches CHAT_CHANNEL_HISTORY_PROOF");
    check(channelAccessProof(QStringLiteral("ada"), QString(32, 'a'),
                             QStringLiteral("17")) ==
              QByteArray("forkmesh-chat-channel-access-v1\nada\n") +
                  QByteArray(32, 'a') + QByteArray("\n17"),
          "channel-access proof matches CHAT_CHANNEL_ACCESS_PROOF");
    check(channelTopicProof(QStringLiteral("ada"), QString(32, 'a'),
                            QStringLiteral("17"),
                            QStringLiteral("  release   planning ")) ==
              QByteArray("forkmesh-chat-channel-topic-v1\nada\n") +
                  QByteArray(32, 'a') +
                  QByteArray("\n17\n02cdc4895c29323ff287e20b069cdd33fe8f38cb82ddcd30205c5c94e0be5eb0"),
          "channel-topic proof binds normalized content to the exact channel");
    // Each settings write signs its own action, so a proof for one can never
    // be replayed as another. The Worker recomputes these exact bytes.
    check(channelSettingsProof(QStringLiteral("ada"), QString(32, 'a'),
                               QStringLiteral("17"),
                               QStringLiteral("  Shipping  "),
                               QStringLiteral("private"),
                               QStringLiteral("  release   planning ")) ==
              QByteArray("forkmesh-chat-channel-settings-v1\nada\n") +
                  QByteArray(32, 'a') +
                  QByteArray("\n17\n0b62f098453457605adbb8b28b1f666bcb1adee749ef31dd9d9fb3f961ff8745"),
          "channel-settings proof names every field it normalizes");
    check(channelDeleteProof(QStringLiteral("ada"), QString(32, 'a'),
                             QStringLiteral("17")) ==
              QByteArray("forkmesh-chat-channel-delete-v1\nada\n") +
                  QByteArray(32, 'a') + QByteArray("\n17"),
          "channel-delete proof binds only the channel it retires");
    check(channelWebhooksProof(QStringLiteral("ada"), QString(32, 'a'),
                               QStringLiteral("17"), QStringLiteral("GET"),
                               QString()) ==
              QByteArray("forkmesh-chat-channel-webhooks-v1\nada\n") +
                  QByteArray(32, 'a') +
                  QByteArray("\n17\nGET\ne3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
          "listing webhooks signs the digest of an empty name");
    check(channelWebhooksProof(QStringLiteral("ada"), QString(32, 'a'),
                               QStringLiteral("17"), QStringLiteral("POST"),
                               QStringLiteral(" Deploy  bot ")) ==
              QByteArray("forkmesh-chat-channel-webhooks-v1\nada\n") +
                  QByteArray(32, 'a') +
                  QByteArray("\n17\nPOST\n81327111302a048b726b26953c2027414e5aec9cfa17c6eb78dfa5cbdc47ec63"),
          "creating a webhook binds the normalized name it will carry");
    check(channelWebhookProof(QStringLiteral("ada"), QString(32, 'a'),
                              QString(32, 'c'), QStringLiteral("17"),
                              QStringLiteral("DELETE")) ==
              QByteArray("forkmesh-chat-channel-webhook-v1\nada\n") +
                  QByteArray(32, 'a') + QByteArray("\n") + QByteArray(32, 'c') +
                  QByteArray("\n17\nDELETE"),
          "a webhook proof names the webhook and the method it authorizes");
    check(channelWebhookProof(QStringLiteral("ada"), QString(32, 'a'),
                              QString(32, 'c'), QStringLiteral("17"),
                              QStringLiteral("POST")) !=
              channelWebhookProof(QStringLiteral("ada"), QString(32, 'a'),
                                  QString(32, 'c'), QStringLiteral("17"),
                                  QStringLiteral("DELETE")),
          "rotating and removing a webhook are not the same capability");

    const QString room = QStringLiteral("chat-channel:") + QString(32, 'b') +
                         QStringLiteral(":v1");
    const RoomCrypto crypto(room, QStringLiteral("room-passphrase"));
    check(crypto.isValid(), "office room key derives from room + passphrase");

    RoomCrypto::resetDerivationCountForTests();
    ServerNode officeSender(
        QStringLiteral("Ada"), QStringLiteral("ada"), QStringLiteral("ada"),
        QStringLiteral("office-node"), QUrl(QStringLiteral("wss://relay.test/ws")),
        room, QString(), QStringLiteral("room-passphrase"));
    check(RoomCrypto::derivationCountForTests() == 1,
          "an office sender derives only the relay key, not a discarded fallback");

    QJsonObject spoken{{"type", "chat"},
                       {"id", "office-message-1"},
                       {"senderId", "office-participant"},
                       {"sender", "ada"},
                       {"accountKind", "user"},
                       {"channel", "#design"},
                       {"text", "standing by the whiteboard"},
                       {"ts", double(QDateTime::currentMSecsSinceEpoch())}};
    spoken.insert(QStringLiteral("meetingProof"),
                  QJsonObject{{"participantId", "office-participant"}});
    const QJsonObject envelope = crypto.encryptObject(spoken);
    const QJsonObject plain = crypto.decryptObject(envelope);

    ChatMessage message;
    check(chatMessageFromPlain(plain, QStringLiteral("#office/design"),
                               QStringLiteral("this-node"), &message),
          "an office chat frame becomes a desktop chat message");
    check(message.text == QStringLiteral("standing by the whiteboard") &&
              message.senderName == QStringLiteral("ada") &&
              message.conversation == QStringLiteral("#office/design") &&
              !message.self,
          "the mirrored row keeps the office author, text, and room");

    ChatMessage self;
    check(chatMessageFromPlain(plain, QStringLiteral("#office/design"),
                               QStringLiteral("office-participant"), &self) &&
              self.self,
          "our own office message is marked as ours");

    ChatMessage ignored;
    QJsonObject presence = spoken;
    presence.insert(QStringLiteral("type"), QStringLiteral("hello"));
    check(!chatMessageFromPlain(presence, QStringLiteral("#office/design"),
                                QString(), &ignored),
          "office presence frames are not chat rows");
    QJsonObject empty = spoken;
    empty.insert(QStringLiteral("text"), QString());
    check(!chatMessageFromPlain(empty, QStringLiteral("#office/design"),
                                QString(), &ignored),
          "an empty office frame with no attachment is dropped");
    check(!chatMessageFromPlain(QJsonObject(), QStringLiteral("#office/design"),
                                QString(), &ignored),
          "a frame that failed to decrypt is dropped");

    QJsonObject notice = spoken;
    notice.insert(QStringLiteral("senderId"), QStringLiteral("system:mail-log"));
    notice.insert(
        QStringLiteral("blocks"),
        QJsonArray{
            QJsonObject{{"type", "section"},
                        {"text", QStringLiteral("🪪 Verification · ✅ accepted "
                                                "· 2026-09-28 13:00:00 UTC")},
                        {"fields", QJsonArray{"📨 To: jo@example.org",
                                              "📝 Subject: <b>Welcome</b>"}}},
            QJsonObject{{"type", "actions"}, {"elements", QJsonArray{}}},
            QJsonObject{{"type", "context"}, {"text", "2 older notes dropped"}}});
    ChatMessage formatted;
    check(chatMessageFromPlain(notice, QStringLiteral("#office/email-log"),
                               QString(), &formatted) &&
              formatted.blocks.size() == 2 &&
              formatted.blocks.at(0).toObject().value("fields").toArray().size() == 2,
          "a server notice keeps its section and context blocks and drops the rest");
    const QString cards = forkmesh::chatblocks::html(formatted.blocks, true);
    check(cards.contains(QStringLiteral(">🪪 Verification<")) &&
              cards.contains(QStringLiteral("color:#3fb950; font-weight:600'>✅ accepted<")) &&
              cards.contains(QStringLiteral(">📨 To</td>")) &&
              cards.contains(QStringLiteral("jo@example.org")) &&
              cards.contains(QStringLiteral("&lt;b&gt;Welcome&lt;/b&gt;")) &&
              cards.contains(QStringLiteral("2 older notes dropped")),
          "an emoji headline and emoji field labels keep their colour and label grid");
    const QJsonArray tones{QJsonObject{
        {"type", "section"},
        {"text", QStringLiteral("🗂️ Task activity · 👀 opened · 1970-01-01")},
        {"fields", QJsonArray{"🆔 Message ID: m-1"}}}};
    const QString toned = forkmesh::chatblocks::html(tones, true);
    check(toned.contains(QStringLiteral("color:#58a6ff; font-weight:600'>👀 opened<")) &&
              toned.contains(QStringLiteral(">🆔 Message ID</td>")),
          "an open, click or view reads as an informational blue");
    QJsonObject borrowed = notice;
    borrowed.insert(QStringLiteral("senderId"), QStringLiteral("office-participant"));
    ChatMessage member;
    check(chatMessageFromPlain(borrowed, QStringLiteral("#office/design"),
                               QString(), &member) &&
              member.blocks.isEmpty(),
          "a member's frame cannot borrow the notice layout");
}

void testChatListPreview()
{
    using forkmesh::chatlist::preview;
    using forkmesh::chatlist::timeLabel;
    const auto message = [](const QString &id, const QString &sender,
                            const QString &text, qint64 ts) {
        ChatMessage m;
        m.id = id;
        m.conversation = QStringLiteral("#general");
        m.senderId = sender + QStringLiteral("-key");
        m.senderName = sender;
        m.text = text;
        m.timestampMs = ts;
        return m;
    };
    QList<ChatMessage> history;
    check(preview(history, false, QStringLiteral("ada")).isEmpty(),
          "a conversation with no messages has no last line");
    history << message("1", "bob", "*Release* is at\nnoon `today`", 1000);
    check(preview(history, false, QStringLiteral("ada")).text ==
                  QStringLiteral("bob: Release is at noon today") &&
              preview(history, false, QStringLiteral("ada")).timestampMs == 1000,
          "a channel line names the sender, drops formatting and newlines");
    check(preview(history, true, QStringLiteral("ada")).text ==
              QStringLiteral("Release is at noon today"),
          "a direct message line is just the text");
    ChatMessage own = message("2", "Ada", "on my way", 2000);
    history << own;
    check(preview(history, false, QStringLiteral("ada")).text ==
                  QStringLiteral("You: on my way") &&
              preview(history, true, QString()).text ==
                  QStringLiteral("on my way"),
          "an own message reads You: by name, and needs a name or the self flag");
    own.senderName = QStringLiteral("Ada's laptop");
    own.self = true;
    history.last() = own;
    check(preview(history, true, QString()).text == QStringLiteral("You: on my way"),
          "the self flag marks an own message");
    ChatMessage reply = message("3", "cat", "a thread reply", 3000);
    reply.threadRootId = QStringLiteral("1");
    ChatMessage gone = message("4", "dee", "", 4000);
    gone.deleted = true;
    ChatMessage empty = message("5", "eve", "   ", 5000);
    history << reply << gone << empty;
    check(preview(history, false, QStringLiteral("ada")).text ==
              QStringLiteral("You: on my way"),
          "thread replies, deleted and empty frames never become the last line");
    ChatMessage file = message("6", "finn", "", 6000);
    file.fileName = QStringLiteral("plan.pdf");
    history << file;
    check(preview(history, false, QStringLiteral("ada")).text ==
              QString::fromUtf8("finn: \xF0\x9F\x93\x8E plan.pdf"),
          "a file with no text shows the paperclip and the file name");
    ChatMessage discordReply = message("7", "gus", "on discord", 7000);
    discordReply.conversation = QStringLiteral("#discord/1/2");
    discordReply.threadRootId = QStringLiteral("6");
    history << discordReply;
    check(preview(history, false, QStringLiteral("ada")).text ==
              QStringLiteral("gus: on discord"),
          "a Discord reply sits in the transcript, so it counts");
    history << message("8", "hal", QString(400, QLatin1Char('x')), 8000);
    check(preview(history, false, QString()).text.size() ==
              forkmesh::chatlist::kMaxPreviewChars + 5,
          "a long message is cut before it reaches the row");

    const QDateTime now(QDate(2026, 10, 13), QTime(15, 30));
    const auto at = [](const QDate &day, const QTime &time) {
        return QDateTime(day, time).toMSecsSinceEpoch();
    };
    check(timeLabel(at(QDate(2026, 10, 13), QTime(14, 2)), now) ==
                  QStringLiteral("2:02 PM") &&
              timeLabel(at(QDate(2026, 10, 11), QTime(9, 0)), now) ==
                  QStringLiteral("Sun") &&
              timeLabel(at(QDate(2026, 10, 3), QTime(9, 0)), now) ==
                  QStringLiteral("Oct 3") &&
              timeLabel(at(QDate(2025, 12, 24), QTime(9, 0)), now) ==
                  QStringLiteral("12/24/25") &&
              timeLabel(0, now).isEmpty(),
          "the row time is the clock today, the weekday this week, then the date");
}

void testChatFormat()
{
    using namespace forkmesh::chatformat;

    check(toHtml(QStringLiteral("*bold* _italic_ ~strike~ `code`")) ==
              QStringLiteral("<b>bold</b> <i>italic</i> <s>strike</s> "
                             "<code style=\"background-color:#161b22\">&nbsp;code&nbsp;</code>"),
          "Slack-style markers render as bold, italic, strike and code");
    check(toHtml(QStringLiteral("**bold** ~~gone~~ ++under++")) ==
              QStringLiteral("<b>bold</b> <s>gone</s> <u>under</u>"),
          "the web composer's doubled markers still render");
    check(toHtml(QStringLiteral("snake_case_name 2*3*4")) ==
              QStringLiteral("snake_case_name 2*3*4"),
          "markers inside a word are left alone");
    check(toHtml(QStringLiteral("<https://forkmesh.com|ForkMesh> [docs](javascript:x)"))
              .startsWith(QStringLiteral("<a href=\"https://forkmesh.com\"")) &&
              !toHtml(QStringLiteral("[docs](javascript:x)")).contains(QStringLiteral("<a")),
          "Slack links resolve and unsafe targets stay text");
    check(toHtml(QStringLiteral("<b>hi</b> & co")) ==
              QStringLiteral("&lt;b&gt;hi&lt;/b&gt; &amp; co"),
          "raw HTML in a message stays escaped");
    const QString blocks =
        toHtml(QStringLiteral("- one\n- *two*\n> quoted\n```\na *b*\n```"));
    check(blocks.contains(QStringLiteral("<ul")) &&
              blocks.contains(QStringLiteral("<li><b>two</b></li>")) &&
              blocks.contains(QStringLiteral(">quoted</td>")) &&
              blocks.contains(QStringLiteral("<pre style=\"margin:0\">a *b*</pre>")),
          "lists, quotes and code blocks render as blocks");
    check(toPlainText(QStringLiteral("*Deploy* done: <https://x.test|logs>\n- a\n> q")) ==
              QStringLiteral("Deploy done: logs\n\u2022 a\n\u201cq\u201d"),
          "plain text drops every marker for notifications");
    check(notificationText(QStringLiteral("1\n2\n\n3\n4\n5")) ==
              QStringLiteral("1\n2\n3\n4 \u2026"),
          "a notification keeps four non-empty lines");
    check(notificationText(QString(200, QLatin1Char('x'))).size() == 180,
          "a notification body is capped");
    QJsonObject rich{{"text", "plain"},
                     {"richText", QJsonObject{{"v", 1}, {"source", "*rich*"}}}};
    check(bodySource(rich, 100) == QStringLiteral("*rich*") &&
              bodySource(QJsonObject{{"text", "_only_"}}, 100) == QStringLiteral("_only_"),
          "the rich source is the body the desktop formats");
    const QJsonArray notice{QJsonObject{
        {"type", "section"},
        {"text", "Verification \u00b7 accepted"},
        {"fields", QJsonArray{"To: jo@example.org", "Subject: *Hi*"}}}};
    check(forkmesh::chatblocks::plainSummary(notice) ==
              QStringLiteral("Verification \u00b7 accepted\nTo: jo@example.org\nSubject: Hi"),
          "a notice summarises its blocks for a notification");
}

void testDiscordClientContract()
{
    using namespace forkmesh::discord;

    check(normalizeBotToken(QStringLiteral("  Bot abc.def.ghi  ")) ==
              QStringLiteral("abc.def.ghi"),
          "Discord bot authorization prefixes are normalized once");
    check(normalizeBotToken(QStringLiteral("abc def")).isEmpty() &&
              normalizeBotToken(QStringLiteral("abc\ndef")).isEmpty(),
          "Discord credentials with header-breaking whitespace are rejected");

    const QString channelId = QStringLiteral("123456789012345678");
    const QString conversation = conversationForChannel(channelId);
    check(conversation ==
              QStringLiteral("#discord/123456789012345678") &&
              isDiscordConversation(conversation) &&
              channelIdFromConversation(conversation) == channelId,
          "Discord channels have namespaced chat conversation keys");
    check(!isDiscordConversation(QStringLiteral("#discord/not-a-snowflake")) &&
              conversationForChannel(QStringLiteral("12/34")).isEmpty(),
          "only bounded Discord snowflakes become conversations");
    check(guildGroupName(QStringLiteral("Forkmesh")) ==
                  QString::fromUtf8("Discord \xC2\xB7 Forkmesh") &&
              guildGroupName(QString()) ==
                  QString::fromUtf8("Discord \xC2\xB7 server") &&
              directMessagesGroupName() ==
                  QString::fromUtf8("Discord \xC2\xB7 DMs") &&
              channelShortName(QStringLiteral("general")) ==
                  QStringLiteral("#general") &&
              channelShortName(QStringLiteral("question"),
                               QStringLiteral("help")) ==
                  QStringLiteral("#help / question") &&
              guildGroupName(QStringLiteral("Forkmesh")) + QStringLiteral(" / ") +
                      channelShortName(QStringLiteral("general")) ==
                  QString::fromUtf8("Discord \xC2\xB7 Forkmesh / #general"),
          "Discord channels group under their server and keep short labels inside it");
    check(messageIdFromChatId(
              QStringLiteral("discord-message:987654321098765432")) ==
              QStringLiteral("987654321098765432") &&
              messageIdFromChatId(QStringLiteral("mesh-message")).isEmpty(),
          "Discord UI message ids round-trip only to Discord API ids");

    // Chat rows carry reaction slugs, Discord only understands the literal
    // emoji: sending the slug is what made every reaction fail with "Unknown
    // Emoji", so the translation is pinned in both directions.
    const QString messageId = QStringLiteral("987654321098765432");
    const QString thumbsUp = QString::fromUcs4(U"\U0001F44D");
    const QString endpoint =
        reactionEndpoint(channelId, messageId, QStringLiteral("like"));
    check(endpoint ==
              QStringLiteral("/channels/123456789012345678/messages/"
                             "987654321098765432/reactions/"
                             "%F0%9F%91%8D/@me"),
          "a reaction slug reaches Discord as its percent-encoded emoji");
    check(reactionEndpoint(channelId, messageId, thumbsUp) == endpoint,
          "a literal emoji addresses the same reaction as its slug");
    check(reactionEndpoint(channelId, messageId,
                           QStringLiteral("partyblob:222222222222222222")) ==
              QStringLiteral("/channels/123456789012345678/messages/"
                             "987654321098765432/reactions/"
                             "partyblob%3A222222222222222222/@me"),
          "a server's custom emoji keeps its name:id form");
    check(reactionEndpoint(channelId, messageId,
                           QStringLiteral("nosuchslug")).isEmpty() &&
              reactionEndpoint(channelId, messageId, QString()).isEmpty(),
          "a reaction Discord cannot resolve is declined, not sent to fail");
    check(reactionEndpoint(QStringLiteral("../../users"), messageId,
                           QStringLiteral("like")).isEmpty() &&
              reactionEndpoint(channelId, QStringLiteral("../@me"),
                               QStringLiteral("like")).isEmpty(),
          "reaction routes are built only from Discord snowflakes");

    // Qt owns the last step: setPath() re-parses the route, and a percent
    // sequence it decoded but failed to re-encode would reach Discord as raw
    // bytes.
    QUrl apiUrl(QStringLiteral("https://discord.com/api/v10"));
    apiUrl.setPath(apiUrl.path() + endpoint, QUrl::TolerantMode);
    check(apiUrl.toEncoded().endsWith(
              QByteArrayLiteral("/reactions/%F0%9F%91%8D/@me")),
          "the encoded emoji survives Discord URL assembly");

    check(reactions::valueForUnicode(thumbsUp) == QStringLiteral("like") &&
              reactions::valueForUnicode(QString::fromUcs4(U"\u2764\uFE0F")) ==
                  QStringLiteral("love") &&
              reactions::valueForUnicode(QString::fromUcs4(U"\u2764")) ==
                  QStringLiteral("love"),
          "Discord reactions fold back onto chat slugs with or without a "
          "variation selector");
    check(reactions::valueForUnicode(QString::fromUcs4(U"\U0001F984")) ==
              QString::fromUcs4(U"\U0001F984"),
          "an emoji this build cannot draw keeps its own spelling");

    bool everySlugTranslates = !reactions::catalog().isEmpty();
    for (const reactions::CatalogEntry &entry : reactions::catalog()) {
        everySlugTranslates =
            everySlugTranslates && !entry.emoji.isEmpty() &&
            reactions::unicodeForValue(entry.value) == entry.emoji &&
            reactions::valueForUnicode(entry.emoji) == entry.value &&
            !reactionEndpoint(channelId, messageId, entry.value).isEmpty();
    }
    check(everySlugTranslates,
          "every reaction the picker offers can be spelled to Discord");

    QJsonObject payload{
        {QStringLiteral("id"), QStringLiteral("987654321098765432")},
        {QStringLiteral("channel_id"), channelId},
        {QStringLiteral("content"),
         QStringLiteral("Hello <@111111111111111111>")},
        {QStringLiteral("timestamp"),
         QStringLiteral("2026-08-18T14:23:45.123Z")},
        {QStringLiteral("edited_timestamp"),
         QStringLiteral("2026-08-18T14:24:00.000Z")},
        {QStringLiteral("author"),
         QJsonObject{{QStringLiteral("id"),
                      QStringLiteral("222222222222222222")},
                     {QStringLiteral("username"), QStringLiteral("ada")},
                     {QStringLiteral("global_name"),
                      QStringLiteral("Ada Lovelace")}}},
        {QStringLiteral("member"),
         QJsonObject{{QStringLiteral("nick"), QStringLiteral("Ada")}}},
        {QStringLiteral("mentions"),
         QJsonArray{QJsonObject{
             {QStringLiteral("id"), QStringLiteral("111111111111111111")},
             {QStringLiteral("username"), QStringLiteral("grace")},
             {QStringLiteral("global_name"), QStringLiteral("Grace Hopper")}}}},
        {QStringLiteral("message_reference"),
         QJsonObject{{QStringLiteral("message_id"),
                      QStringLiteral("333333333333333333")}}},
        {QStringLiteral("attachments"),
         QJsonArray{QJsonObject{
             {QStringLiteral("filename"), QStringLiteral("design.png")},
             {QStringLiteral("content_type"), QStringLiteral("image/png")},
             {QStringLiteral("size"), 42},
             {QStringLiteral("url"),
              QStringLiteral("https://cdn.discordapp.com/attachments/a/b/"
                             "design.png")}}}}};

    ChatMessage message;
    check(chatMessageFromPayload(payload, conversation,
                                 QStringLiteral("222222222222222222"),
                                 &message),
          "a Discord message payload becomes a native chat row");
    check(message.id ==
                  QStringLiteral("discord-message:987654321098765432") &&
              message.senderId ==
                  QStringLiteral("discord-user:222222222222222222") &&
              message.senderName == QStringLiteral("Ada") &&
              message.text == QStringLiteral("Hello @Grace Hopper") &&
              message.self && message.edited &&
              message.threadRootId ==
                  QStringLiteral("discord-message:333333333333333333") &&
              message.fileName == QStringLiteral("design.png") &&
              message.fileMime == QStringLiteral("image/png") &&
              message.timestampMs == 1787063025123ll,
          "Discord conversion preserves identity, reply, edit, mention, file, "
          "and timestamp semantics");

    QJsonObject malformed = payload;
    malformed.insert(QStringLiteral("channel_id"),
                     QStringLiteral("999999999999999999"));
    check(!chatMessageFromPayload(malformed, conversation, QString(), &message),
          "a payload cannot escape into a different Discord conversation");

    check((kGatewayIntents & (1 << 15)) != 0 &&
              (kGatewayIntents & (1 << 1)) == 0,
          "the Gateway asks for message content without privileged member data");
}

} // namespace

// A one-connection-at-a-time HTTP stand-in for the relay. It records every
// request the mirror makes and answers from a canned table, which is enough to
// pin the method, path, and signed proof of each channel-settings write.
class RelayHarness
{
public:
    struct Call {
        QByteArray method;
        QString path;
        QString query;
        QByteArray body;
    };

    RelayHarness()
    {
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            while (server.hasPendingConnections()) {
                QTcpSocket *socket = server.nextPendingConnection();
                QObject::connect(socket, &QTcpSocket::disconnected, socket,
                                 &QObject::deleteLater);
                QObject::connect(socket, &QTcpSocket::readyRead, socket,
                                 [this, socket] { serve(socket); });
            }
        });
    }

    bool listen() { return server.listen(QHostAddress::LocalHost, 0); }

    QUrl apiBase() const
    {
        return QUrl(QStringLiteral("http://127.0.0.1:%1")
                        .arg(server.serverPort()));
    }

    // path -> JSON body answered for it, newest registration wins.
    QHash<QString, QByteArray> responses;
    QList<Call> calls;
    QTcpServer server;

    const Call *lastCall(const QByteArray &method, const QString &path) const
    {
        for (int index = calls.size() - 1; index >= 0; --index)
            if (calls[index].method == method && calls[index].path == path)
                return &calls[index];
        return nullptr;
    }

private:
    void serve(QTcpSocket *socket)
    {
        buffers[socket] += socket->readAll();
        QByteArray &buffer = buffers[socket];
        const int headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0)
            return;
        const QByteArray header = buffer.left(headerEnd);
        int contentLength = 0;
        for (const QByteArray &line : header.split('\n')) {
            const QByteArray trimmed = line.trimmed().toLower();
            if (trimmed.startsWith("content-length:"))
                contentLength = trimmed.mid(15).trimmed().toInt();
        }
        const QByteArray body = buffer.mid(headerEnd + 4);
        if (body.size() < contentLength)
            return;
        buffer.clear();
        const QList<QByteArray> requestLine = header.split('\n')
                                                  .first()
                                                  .trimmed()
                                                  .split(' ');
        Call call;
        call.method = requestLine.value(0);
        const QUrl target(QString::fromUtf8(requestLine.value(1)));
        call.path = target.path();
        call.query = target.query();
        call.body = body;
        calls.append(call);
        const QByteArray payload =
            responses.value(call.path, QByteArrayLiteral("{\"ok\":true}"));
        socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\n"
                      "Content-Length: " +
                      QByteArray::number(payload.size()) +
                      "\r\nConnection: close\r\n\r\n" + payload);
        socket->flush();
        socket->disconnectFromHost();
    }

    QHash<QTcpSocket *, QByteArray> buffers;
};

// The settings panel's writes leave the desktop as signed, resource-bound
// requests. Prove each one uses the method, path, and proof the relay checks.
void testOfficeChannelSettingsRequests()
{
    using namespace forkmesh::office;

    const QString channelId(32, 'a');
    const QString webhookId(32, 'c');
    RelayHarness relay;
    check(relay.listen(), "the relay stand-in binds a loopback port");
    relay.responses.insert(
        QStringLiteral("/api/chat/channels"),
        QByteArrayLiteral("{\"channels\":[{\"id\":\"") + channelId.toUtf8() +
            QByteArrayLiteral("\",\"name\":\"release-team\",\"visibility\":"
                              "\"private\",\"canManage\":true,\"topic\":\"\","
                              "\"keyVersion\":1}]}"));
    relay.responses.insert(
        QStringLiteral("/api/chat/channels/") + channelId,
        QByteArrayLiteral("{\"ok\":true,\"channel\":{\"id\":\"") +
            channelId.toUtf8() +
            QByteArrayLiteral("\",\"name\":\"shipping\",\"visibility\":"
                              "\"public\",\"canManage\":true,\"topic\":"
                              "\"release planning\"}}"));
    relay.responses.insert(
        QStringLiteral("/api/chat/channels/") + channelId +
            QStringLiteral("/webhooks"),
        QByteArrayLiteral("{\"ok\":true,\"webhooks\":[{\"id\":\"") +
            webhookId.toUtf8() +
            QByteArrayLiteral("\",\"name\":\"Deploy bot\",\"createdAt\":1,"
                              "\"lastUsedAt\":0}],\"webhook\":{\"id\":\"") +
            webhookId.toUtf8() +
            QByteArrayLiteral("\"},\"url\":\"https://forkmesh.test/api/chat/"
                              "webhooks/token\"}"));
    relay.responses.insert(
        QStringLiteral("/api/chat/channels/") + channelId +
            QStringLiteral("/webhooks/") + webhookId,
        QByteArrayLiteral("{\"ok\":true,\"url\":\"https://forkmesh.test/api/"
                          "chat/webhooks/rotated\"}"));

    QNetworkAccessManager network;
    OfficeChannelMirror mirror(&network);
    // The signature is the canonical itself, so every assertion below reads
    // the exact bytes this node signed.
    mirror.setSigner([](const QByteArray &canonical) {
        return QString::fromUtf8(canonical.toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    });
    mirror.setConnectionAuthorizer([](const QUrl &) { return false; });
    mirror.setApiBase(relay.apiBase());
    mirror.setIdentity(QStringLiteral("ada"), QStringLiteral("self-key"),
                       QStringLiteral("Ada"));
    mirror.start();

    const QString conversation = QStringLiteral("#office/release-team");
    check(waitUntil([&] { return mirror.conversations().contains(conversation); },
                    4000),
          "the mirror lists a manageable office channel");

    auto signedProof = [&relay](const QByteArray &method, const QString &path) {
        const RelayHarness::Call *call = relay.lastCall(method, path);
        if (!call)
            return QByteArray();
        for (const QString &pair : call->query.split(QLatin1Char('&')))
            if (pair.startsWith(QStringLiteral("sig=")))
                return QByteArray::fromBase64(
                    pair.mid(4).toUtf8(),
                    QByteArray::Base64UrlEncoding);
        return QByteArray();
    };
    const QString channelPath = QStringLiteral("/api/chat/channels/") + channelId;
    const QString webhooksPath = channelPath + QStringLiteral("/webhooks");
    const QString webhookPath = webhooksPath + QStringLiteral("/") + webhookId;

    check(mirror.refreshWebhooks(conversation), "a manager can list webhooks");
    check(waitUntil([&] { return relay.lastCall("GET", webhooksPath); }, 4000) &&
              signedProof("GET", webhooksPath)
                  .startsWith("forkmesh-chat-channel-webhooks-v1\nada\n" +
                              channelId.toUtf8() + "\n"),
          "listing webhooks is signed with the webhooks proof");

    // The relay now lists the renamed channel, as it would after the PATCH:
    // the mirror re-reads the list to pick up the rotated key.
    relay.responses.insert(
        QStringLiteral("/api/chat/channels"),
        QByteArrayLiteral("{\"channels\":[{\"id\":\"") + channelId.toUtf8() +
            QByteArrayLiteral("\",\"name\":\"shipping\",\"visibility\":"
                              "\"public\",\"canManage\":true,\"topic\":"
                              "\"release planning\",\"keyVersion\":2}]}"));
    check(mirror.updateChannel(conversation, QStringLiteral(" Shipping "),
                               QStringLiteral("  release   planning "), false),
          "a manager can save channel settings");
    check(waitUntil([&] { return relay.lastCall("PATCH", channelPath); }, 4000),
          "settings go out as a PATCH to the channel itself");
    const RelayHarness::Call *patch = relay.lastCall("PATCH", channelPath);
    check(patch && patch->body.contains("\"name\":\"shipping\"") &&
              patch->body.contains("\"visibility\":\"public\"") &&
              patch->body.contains("\"topic\":\"release planning\""),
          "the PATCH body carries the normalized name, visibility and topic");
    check(signedProof("PATCH", channelPath) ==
              channelSettingsProof(QStringLiteral("ada"), channelId,
                                   QString::fromUtf8(patch ? queryValue(
                                                                 patch->query,
                                                                 "ts")
                                                           : QByteArray()),
                                   QStringLiteral("shipping"),
                                   QStringLiteral("public"),
                                   QStringLiteral("release planning")),
          "the settings proof signs exactly the body that was sent");
    check(waitUntil([&] { return mirror.conversations().contains(
                              QStringLiteral("#office/shipping")); },
                    4000),
          "a saved rename moves the conversation the window follows");

    check(mirror.createWebhook(QStringLiteral("#office/shipping"),
                               QStringLiteral(" Deploy  bot ")),
          "a manager can add a webhook");
    check(waitUntil([&] { return relay.lastCall("POST", webhooksPath); }, 4000) &&
              signedProof("POST", webhooksPath)
                  .endsWith(QCryptographicHash::hash(
                                QByteArrayLiteral("Deploy bot"),
                                QCryptographicHash::Sha256)
                                .toHex()),
          "creating a webhook signs the name the relay will store");

    check(mirror.rotateWebhook(QStringLiteral("#office/shipping"), webhookId),
          "a manager can issue a new webhook URL");
    check(waitUntil([&] { return relay.lastCall("POST", webhookPath); }, 4000) &&
              signedProof("POST", webhookPath).endsWith("\nPOST"),
          "rotating a webhook signs the POST it authorizes");

    check(mirror.deleteWebhook(QStringLiteral("#office/shipping"), webhookId),
          "a manager can remove a webhook");
    check(waitUntil([&] { return relay.lastCall("DELETE", webhookPath); }, 4000) &&
              signedProof("DELETE", webhookPath).endsWith("\nDELETE"),
          "removing a webhook signs the DELETE it authorizes");

    QString deleted;
    QObject::connect(&mirror, &OfficeChannelMirror::channelDeleted,
                     &mirror, [&deleted](const QString &conversation) {
                         deleted = conversation;
                     });
    relay.responses.insert(channelPath,
                           QByteArrayLiteral("{\"ok\":true,\"removed\":true}"));
    check(mirror.deleteChannel(QStringLiteral("#office/shipping")),
          "a manager can delete the channel");
    check(waitUntil([&] { return relay.lastCall("DELETE", channelPath); }, 4000) &&
              signedProof("DELETE", channelPath) ==
                  channelDeleteProof(
                      QStringLiteral("ada"), channelId,
                      QString::fromUtf8(queryValue(
                          relay.lastCall("DELETE", channelPath)->query, "ts"))),
          "deleting the channel signs the delete proof for that channel");
    check(waitUntil([&] { return deleted == QStringLiteral("#office/shipping"); },
                    4000) &&
              !mirror.conversations().contains(QStringLiteral("#office/shipping")),
          "a deleted channel leaves the sidebar immediately");

    // A member without manage rights signs nothing at all.
    check(!mirror.updateChannel(QStringLiteral("#office/absent"),
                                QStringLiteral("nope"), QString(), true) &&
              !mirror.deleteChannel(QStringLiteral("#office/absent")) &&
              !mirror.refreshWebhooks(QStringLiteral("#office/absent")),
          "settings writes refuse a channel this account cannot manage");
    mirror.stop();
}

void testOfficePrivateRoomsAndDirectMessages()
{
    using namespace forkmesh::office;

    check(conversationForDirect(QStringLiteral(" @Bob ")) ==
                  QStringLiteral("#office/@bob") &&
              isDirectOfficeConversation(QStringLiteral("#office/@bob")) &&
              isOfficeConversation(QStringLiteral("#office/@bob")) &&
              !isDirectOfficeConversation(QStringLiteral("#office/design")) &&
              directPeerForConversation(QStringLiteral("#office/@bob")) ==
                  QStringLiteral("bob") &&
              conversationForDirect(QStringLiteral("not a name")).isEmpty(),
          "a direct message lives under its own #office/@ conversation");

    check(roomForConversation(QStringLiteral("#office/email-log")) ==
                  QStringLiteral("email-log") &&
              organizationForConversation(
                  QStringLiteral("#office/email-log")).isEmpty() &&
              roomForConversation(
                  QStringLiteral("#office/office/forkmesh/coffee")) ==
                  QStringLiteral("coffee") &&
              organizationForConversation(
                  QStringLiteral("#office/office/forkmesh/coffee")) ==
                  QStringLiteral("forkmesh") &&
              roomForConversation(QStringLiteral("#office/@bob")).isEmpty() &&
              roomForConversation(QStringLiteral("#general")).isEmpty(),
          "an office channel is labelled by its room name alone");

    const QString ts = QStringLiteral("1700000000000");
    check(channelCreateProof(QStringLiteral("alice"), ts,
                             QStringLiteral(" Not-Very-Secret "),
                             QStringLiteral("private"),
                             {QStringLiteral("Bob"), QStringLiteral("carol")}) ==
              QByteArrayLiteral(
                  "forkmesh-chat-channel-create-v1\nalice\n1700000000000\n"
                  "cb57d4a3c1ff43d0506249ea83dbc35e5700b9755652b84d25beddf57bb3eff1"),
          "the create proof matches the digest the worker test pins");
    const QByteArray bobDigest =
        QCryptographicHash::hash("bob", QCryptographicHash::Sha256).toHex();
    const QString channelId(32, 'a');
    check(channelMembersProof(QStringLiteral("alice"), channelId, ts,
                              QStringLiteral("POST"), QStringLiteral(" Bob ")) ==
              "forkmesh-chat-channel-members-v1\nalice\n" + channelId.toUtf8() +
                  "\n1700000000000\nPOST\n" + bobDigest,
          "an invitation proof names the room, the action and the person");
    check(directStartProof(QStringLiteral("alice"), ts, QStringLiteral("Bob")) ==
                  "forkmesh-chat-direct-start-v1\nalice\n1700000000000\n" +
                      bobDigest &&
              directListProof(QStringLiteral("alice"), ts) ==
                  QByteArrayLiteral(
                      "forkmesh-chat-direct-list-v1\nalice\n1700000000000"),
          "direct-message proofs bind the account and the person messaged");
    QMap<QString, qint64> readMarks;
    readMarks.insert(QStringLiteral("direct:") + QString(32, 'c'), 7);
    readMarks.insert(QStringLiteral("#general"), 5);
    check(readMarksDigest(readMarks) ==
                  QStringLiteral("1e4339410c94c4a213a9ddbc275db2315d0ca4fe1140"
                                 "08638fd3ae1a7e0142b5") &&
              readMarksPostProof(QStringLiteral("alice"), ts, readMarks) ==
                  "forkmesh-chat-reads-mark-v1\nalice\n1700000000000\n" +
                      readMarksDigest(readMarks).toUtf8() &&
              readMarksListProof(QStringLiteral("alice"), ts) ==
                  QByteArrayLiteral(
                      "forkmesh-chat-reads-list-v1\nalice\n1700000000000"),
          "read-mark proofs bind the account and every mark");

    const QString carolId(32, 'd');
    const QString bobId(32, 'e');
    RelayHarness relay;
    check(relay.listen(), "the relay stand-in binds a loopback port");
    relay.responses.insert(
        QStringLiteral("/api/chat/channels"),
        QByteArrayLiteral("{\"channels\":[{\"id\":\"") + channelId.toUtf8() +
            QByteArrayLiteral("\",\"name\":\"not-very-secret\",\"visibility\":"
                              "\"private\",\"canManage\":true,\"topic\":\"\","
                              "\"keyVersion\":1,\"members\":[\"ada\",\"bob\"]}],"
                              "\"channel\":{\"id\":\"") +
            channelId.toUtf8() +
            QByteArrayLiteral("\",\"name\":\"not-very-secret\",\"visibility\":"
                              "\"private\"}}"));
    relay.responses.insert(
        QStringLiteral("/api/chat/direct-messages"),
        QByteArrayLiteral("{\"conversations\":[{\"id\":\"") + carolId.toUtf8() +
            QByteArrayLiteral("\",\"otherUser\":\"carol\"}],\"conversation\":"
                              "{\"id\":\"") +
            bobId.toUtf8() + QByteArrayLiteral("\",\"otherUser\":\"bob\"}}"));

    QNetworkAccessManager network;
    OfficeChannelMirror mirror(&network);
    mirror.setSigner([](const QByteArray &canonical) {
        return QString::fromUtf8(canonical.toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    });
    mirror.setConnectionAuthorizer([](const QUrl &) { return false; });
    mirror.setApiBase(relay.apiBase());
    mirror.setIdentity(QStringLiteral("ada"), QStringLiteral("self-key"),
                       QStringLiteral("Ada"));
    auto signedProof = [&relay](const QByteArray &method, const QString &path) {
        const RelayHarness::Call *call = relay.lastCall(method, path);
        if (!call)
            return QByteArray();
        return QByteArray::fromBase64(queryValue(call->query, "sig"),
                                      QByteArray::Base64UrlEncoding);
    };
    QString created;
    QString ready;
    QObject::connect(&mirror, &OfficeChannelMirror::channelCreated, &mirror,
                     [&created](const QString &value) { created = value; });
    QObject::connect(&mirror, &OfficeChannelMirror::directConversationReady,
                     &mirror, [&ready](const QString &value) { ready = value; });
    mirror.start();

    const QString room = QStringLiteral("#office/not-very-secret");
    const QString carol = QStringLiteral("#office/@carol");
    check(waitUntil([&] { return mirror.conversations().contains(room) &&
                                 mirror.directConversations().contains(carol); },
                    4000),
          "the mirror lists private rooms and direct messages separately");
    check(!mirror.conversations().contains(carol) &&
              mirror.isPrivateConversation(carol) &&
              !mirror.canManageConversation(carol) &&
              mirror.membersForConversation(carol) ==
                  QStringList({QStringLiteral("ada"), QStringLiteral("carol")}),
          "a direct message is a private two-person room, not a channel");
    check(mirror.membersForConversation(room) ==
              QStringList({QStringLiteral("ada"), QStringLiteral("bob")}),
          "a private room's member list is exactly what the relay granted");
    check(signedProof("GET", QStringLiteral("/api/chat/direct-messages"))
              .startsWith("forkmesh-chat-direct-list-v1\nada\n"),
          "listing direct messages is signed with the direct list proof");
    const QString carolAccess =
        QStringLiteral("/api/chat/direct-messages/%1/room-access").arg(carolId);
    check(waitUntil([&] { return relay.lastCall("GET", carolAccess); }, 4000) &&
              signedProof("GET", carolAccess)
                  .startsWith("forkmesh-chat-direct-access-v1\nada\n" +
                              carolId.toUtf8() + "\n"),
          "a direct room asks for access on the participant-only endpoint");

    check(!mirror.createChannel(QStringLiteral("room"), true,
                                {QStringLiteral("not a name")}),
          "an invitation to an invalid username is refused before signing");
    check(mirror.createChannel(QStringLiteral(" Not-Very-Secret "), true,
                               {QStringLiteral("Bob"), QStringLiteral("ada"),
                                QStringLiteral("bob")}),
          "a private room can be created with explicit invitations");
    check(waitUntil([&] { return relay.lastCall("POST",
                                                QStringLiteral("/api/chat/channels")); },
                    4000),
          "creating a room posts to the channels collection");
    const RelayHarness::Call *createCall =
        relay.lastCall("POST", QStringLiteral("/api/chat/channels"));
    const QJsonObject createBody =
        createCall ? QJsonDocument::fromJson(createCall->body).object()
                   : QJsonObject();
    check(createBody.value(QStringLiteral("visibility")).toString() ==
                  QStringLiteral("private") &&
              createBody.value(QStringLiteral("members")).toArray() ==
                  QJsonArray({QStringLiteral("bob")}),
          "the room is private and invites exactly the chosen people");
    check(createCall &&
              signedProof("POST", QStringLiteral("/api/chat/channels")) ==
                  channelCreateProof(
                      QStringLiteral("ada"),
                      QString::fromUtf8(queryValue(createCall->query, "ts")),
                      QStringLiteral("not-very-secret"),
                      QStringLiteral("private"), {QStringLiteral("bob")}),
          "the create proof signs exactly the body that was sent");
    check(waitUntil([&] { return created == room; }, 4000),
          "the window learns which conversation the new room is");

    const QString membersPath =
        QStringLiteral("/api/chat/channels/%1/members").arg(channelId);
    check(mirror.addMember(room, QStringLiteral(" Carol ")),
          "the creator can invite another person");
    check(waitUntil([&] { return relay.lastCall("POST", membersPath); }, 4000) &&
              relay.lastCall("POST", membersPath)->body.contains("\"carol\"") &&
              signedProof("POST", membersPath)
                  .endsWith("\nPOST\n" +
                            QCryptographicHash::hash(
                                "carol", QCryptographicHash::Sha256)
                                .toHex()),
          "an invitation is an explicit signed grant for one person");
    check(mirror.removeMember(room, QStringLiteral("bob")) &&
              waitUntil([&] { return relay.lastCall("DELETE", membersPath); },
                        4000),
          "the creator can remove a person again");
    check(!mirror.addMember(carol, QStringLiteral("bob")),
          "nobody can be added to a direct message");

    const int startsBefore = int(std::count_if(
        relay.calls.cbegin(), relay.calls.cend(), [](const RelayHarness::Call &c) {
            return c.method == "POST" &&
                   c.path == QLatin1String("/api/chat/direct-messages");
        }));
    ready.clear();
    check(mirror.startDirectMessage(QStringLiteral("@Carol")) &&
              ready == carol,
          "messaging someone reopens the existing conversation at once");
    check(!mirror.startDirectMessage(QStringLiteral("ada")),
          "a person cannot message themselves");
    check(mirror.startDirectMessage(QStringLiteral("bob")),
          "messaging a new person starts a conversation");
    check(waitUntil([&] { return ready == QStringLiteral("#office/@bob"); }, 4000) &&
              mirror.directConversations().contains(
                  QStringLiteral("#office/@bob")),
          "the new conversation opens and joins the direct-message list");
    const int startsAfter = int(std::count_if(
        relay.calls.cbegin(), relay.calls.cend(), [](const RelayHarness::Call &c) {
            return c.method == "POST" &&
                   c.path == QLatin1String("/api/chat/direct-messages");
        }));
    check(startsAfter == startsBefore + 1 &&
              signedProof("POST", QStringLiteral("/api/chat/direct-messages")) ==
                  directStartProof(
                      QStringLiteral("ada"),
                      QString::fromUtf8(queryValue(
                          relay.lastCall("POST",
                                         QStringLiteral("/api/chat/direct-messages"))
                              ->query,
                          "ts")),
                      QStringLiteral("bob")),
          "only the new person costs a request, signed for that person");
    ready.clear();
    check(mirror.startDirectMessage(QStringLiteral("Bob")) &&
              ready == QStringLiteral("#office/@bob"),
          "a second Message click reuses the conversation it just opened");
    {
        QList<ChatMessage> invites;
        QObject::connect(&mirror, &OfficeChannelMirror::messageArrived, &mirror,
                         [&invites](const ChatMessage &message) { invites.append(message); });
        ready.clear();
        check(mirror.sendDirectText(QStringLiteral("@Carol"),
                                    QStringLiteral("Join my cove: https://example.org/c")) &&
                  invites.size() == 1 && invites.first().conversation == carol &&
                  invites.first().self &&
                  invites.first().text == QStringLiteral("Join my cove: https://example.org/c") &&
                  ready.isEmpty(),
              "a cove invite goes out as a direct message without opening the conversation");
        {
        const QString officeNode = nodeKey(QStringLiteral("office-room-node"));
        ServerNode roomSender(QStringLiteral("Ada"), QStringLiteral("ada"), QStringLiteral("ada"),
                              officeNode, QUrl(QStringLiteral("ws://127.0.0.1:9/ws")),
                              QStringLiteral("chat-channel:") + QString(32, QLatin1Char('f')) +
                                  QStringLiteral(":v1"),
                              QString(), QStringLiteral("room-passphrase"));
        const RoomSeal::NodeSigner sign = nodeSigner(QStringLiteral("office-room-node"));
        OfficeChannelMirror::prepareRoomSender(roomSender, [sign](const QByteArray &message) {
            return forkmesh::chatv3::b64u(sign(message));
        });
        roomSender.setChatAccount(QStringLiteral("ada"));
        roomSender.createPrivateChannel(QStringLiteral("#design"));
        const QJsonObject bundle = roomSender.roomSeal().keyFrame();
        check(!roomSender.isSharedRoom() &&
                  forkmesh::chatv3::verifyNodeBinding(bundle, roomSender.roomSeal().roomName()) &&
                  bundle.value(QStringLiteral("node")).toString() == officeNode,
              "an office room's node is not the shared room and signs its bundle with the node key");
    }
    check(!mirror.sendDirectText(QStringLiteral("ada"), QStringLiteral("self")),
              "nobody sends a direct invite to themselves");
        QObject::disconnect(&mirror, &OfficeChannelMirror::messageArrived, &mirror, nullptr);
    }

    QList<ChatMessage> echoed;
    QObject::connect(&mirror, &OfficeChannelMirror::messageArrived, &mirror,
                     [&echoed](const ChatMessage &message) {
                         echoed.append(message);
                     });
    QString failedId;
    QString failedText;
    QObject::connect(&mirror, &OfficeChannelMirror::messageSendFailed, &mirror,
                     [&failedId, &failedText](const QString &, const QString &id,
                                              const QString &text,
                                              const QString &) {
                         failedId = id;
                         failedText = text;
                     });
    check(mirror.sendMessage(carol, QStringLiteral(" hi carol ")) &&
              echoed.size() == 1 && echoed.first().conversation == carol &&
              echoed.first().self &&
              echoed.first().senderId == QStringLiteral("self-key") &&
              echoed.first().text == QStringLiteral("hi carol") &&
              !echoed.first().id.isEmpty(),
          "a typed direct message shows at once, before the room socket is up");
    check(waitUntil([&] { return !failedId.isEmpty(); }, 4000) &&
              failedId == echoed.first().id &&
              failedText == QStringLiteral("hi carol"),
          "a send that never reached the relay names the message it showed");

    echoed.clear();
    const QByteArray image("\x89PNG\r\n\x1a\npixels", 14);
    check(mirror.canSendFile(carol) &&
              mirror.sendFile(carol, QStringLiteral("pasted-image.png"),
                              QStringLiteral("image/png"), image) &&
              echoed.size() == 1 && echoed.first().conversation == carol &&
              echoed.first().self && echoed.first().text.isEmpty() &&
              echoed.first().fileName == QStringLiteral("pasted-image.png") &&
              echoed.first().fileMime == QStringLiteral("image/png") &&
              echoed.first().fileData == image,
          "an image shared in a direct message shows at once");
    check(!mirror.sendFile(carol, QStringLiteral("large.png"),
                           QStringLiteral("image/png"),
                           QByteArray(forkmesh::office::kMaxDirectFileBytes + 1,
                                      'x')) &&
              echoed.size() == 1,
          "a direct-message file over the web client's 1 MiB limit is refused");
    check(!mirror.canSendFile(room) &&
              !mirror.sendFile(room, QStringLiteral("pasted-image.png"),
                               QStringLiteral("image/png"), image),
          "a channel still takes text only from the desktop");
    mirror.stop();
}

void testOfficeNoticesChannelShowsServerFrames()
{
    const QString noticesId(32, 'd');
    const QString privateId(32, 'e');
    RelayHarness relay;
    check(relay.listen(), "the relay stand-in binds a loopback port");
    relay.responses.insert(
        QStringLiteral("/api/chat/channels"),
        QByteArrayLiteral("{\"channels\":[{\"id\":\"") + noticesId.toUtf8() +
            QByteArrayLiteral("\",\"name\":\"email-log\",\"visibility\":"
                              "\"private\",\"kind\":\"notices\","
                              "\"serverVisible\":true,\"readOnly\":true,"
                              "\"canManage\":false,\"topic\":\"Server-visible "
                              "notices\",\"keyVersion\":1},{\"id\":\"") +
            privateId.toUtf8() +
            QByteArrayLiteral("\",\"name\":\"design\",\"visibility\":"
                              "\"private\",\"canManage\":true,\"topic\":\"\","
                              "\"keyVersion\":1}]}"));
    auto history = [](const QString &channelId,
                      const QList<QPair<QString, bool>> &entries) {
        const QString room = QStringLiteral("chat-channel:") + channelId +
                             QStringLiteral(":v1");
        const RoomCrypto crypto(room, QStringLiteral("room-passphrase"));
        QJsonArray messages;
        for (const auto &[messageId, srv] : entries) {
        const QJsonObject notice{
            {"type", "chat"},
            {"id", messageId},
            {"senderId", "system:mail-log"},
            {"sender", "Mail log (ForkMesh)"},
            {"accountKind", "user"},
            {"channel", "#email-log"},
            {"text", "2026-09-28 13:00:00Z · Verification · a***@example.org · "
                     "accepted"},
            {"ts", 1790600000000.0},
            {"webhook", true},
            {"serverVisible", true},
            {"private", true},
            {"notice", true},
            {"recipients", 42}};
        QJsonObject envelope = crypto.encryptObject(notice);
        envelope.insert(QStringLiteral("persist"), true);
        envelope.insert(QStringLiteral("recipients"), 5);
        if (srv)
            envelope.insert(QStringLiteral("srv"), true);
        messages.append(QJsonObject{
            {"ts", 1790600000000.0},
            {"body", QString::fromUtf8(QJsonDocument(envelope).toJson(QJsonDocument::Compact))}});
        }
        const QJsonObject payload{
            {"room", room},
            {"passphrase", "room-passphrase"},
            {"messages", messages}};
        return QJsonDocument(payload).toJson(QJsonDocument::Compact);
    };
    relay.responses.insert(QStringLiteral("/api/chat/channels/") + noticesId +
                               QStringLiteral("/history"),
                           history(noticesId, {{QStringLiteral("notice-1"), true},
                                               {QStringLiteral("notice-forged"), false}}));
    relay.responses.insert(QStringLiteral("/api/chat/channels/") + privateId +
                               QStringLiteral("/history"),
                           history(privateId, {{QStringLiteral("unsealed-1"), false},
                                               {QStringLiteral("private-srv-1"), true}}));

    QNetworkAccessManager network;
    OfficeChannelMirror mirror(&network);
    mirror.setSigner([](const QByteArray &) { return QStringLiteral("sig"); });
    mirror.setConnectionAuthorizer([](const QUrl &) { return false; });
    mirror.setApiBase(relay.apiBase());
    mirror.setIdentity(QStringLiteral("jett"), QStringLiteral("self-key"),
                       QStringLiteral("Jett"));
    QList<ChatMessage> arrived;
    QObject::connect(&mirror, &OfficeChannelMirror::messageArrived, &mirror,
                     [&arrived](const ChatMessage &message) {
                         arrived.append(message);
                     });
    mirror.start();

    const QString notices = QStringLiteral("#office/email-log");
    const QString design = QStringLiteral("#office/design");
    check(waitUntil([&] { return mirror.conversations().contains(notices) &&
                                 mirror.conversations().contains(design); },
                    4000),
          "the mirror lists the notices channel beside a private one");
    check(mirror.isServerVisibleConversation(notices) &&
              mirror.isReadOnlyConversation(notices) &&
              !mirror.isServerVisibleConversation(design),
          "only the notices channel is flagged server-visible and read-only");
    check(!mirror.canSend(notices) && !mirror.sendMessage(notices,
                                                          QStringLiteral("hi")),
          "the desktop never tries to post into a read-only notices channel");
    ChatMessage note;
    check(waitUntil([&] {
              for (const ChatMessage &message : std::as_const(arrived))
                  if (message.id == QLatin1String("notice-1"))
                      note = message;
              return !note.id.isEmpty();
          }, 4000) &&
              note.conversation == notices &&
              note.senderName == QStringLiteral("Mail log (ForkMesh) · server-visible") &&
              note.text.contains(QStringLiteral("accepted")),
          "a server-written email-log note is shown in the notices channel");
    check(note.recipientCount == 5,
          "history reads the recipient count from the outer envelope only");
    check(waitUntil([&] { return relay.lastCall(
                              "GET", QStringLiteral("/api/chat/channels/") +
                                         privateId +
                                         QStringLiteral("/history")); },
                    4000),
          "the private channel's history is read too");
    waitUntil([] { return false; }, 500);
    bool unsealedShown = false;
    bool privateSrvShown = false;
    for (const ChatMessage &message : std::as_const(arrived)) {
        unsealedShown = unsealedShown || message.id == QLatin1String("unsealed-1") ||
                        message.id == QLatin1String("notice-forged");
        privateSrvShown = privateSrvShown ||
                          (message.id == QLatin1String("private-srv-1") &&
                           message.conversation == design &&
                           message.senderId == QLatin1String("system:mail-log"));
    }
    check(!unsealedShown,
          "an unsealed frame the relay did not mark as its own is refused");
    check(privateSrvShown, "a relay frame in a private channel's history is shown");
    mirror.stop();
}

void testOfficeEveryoneChannels()
{
    using namespace forkmesh::office;

    check(everyoneChannelId(QStringLiteral("general")) ==
                  QString::fromLatin1(
                      QCryptographicHash::hash(
                          "forkmesh-everyone-channel-v1:general",
                          QCryptographicHash::Sha256)
                          .toHex()
                          .left(32)) &&
              everyoneChannelId(QStringLiteral("welcome")).size() == 32,
          "everyone channel ids match the relay's fixed derivation");
    check(everyoneConversations() ==
                  QStringList{QStringLiteral("#office/general"),
                              QStringLiteral("#office/random"),
                              QStringLiteral("#office/welcome")} &&
              everyoneGeneralConversation() == QStringLiteral("#office/general") &&
              everyoneWelcomeConversation() == QStringLiteral("#office/welcome"),
          "the everyone channels are ordinary office conversations, in order");
    check(worldConversation() == QStringLiteral("#general") &&
              worldLabel() == QStringLiteral("World") &&
              isRetiredRelayChannel(QStringLiteral("#welcome")) &&
              isRetiredRelayChannel(QStringLiteral("#random")) &&
              !isRetiredRelayChannel(QStringLiteral("#general")),
          "the relay keeps only its #general room, shown as World");

    const QString generalId = everyoneChannelId(QStringLiteral("general"));
    const QString randomId = everyoneChannelId(QStringLiteral("random"));
    const QString welcomeId = everyoneChannelId(QStringLiteral("welcome"));
    const QString designId(32, 'f');
    RelayHarness relay;
    check(relay.listen(), "the relay stand-in binds a loopback port");
    auto everyoneChannel = [](const QString &id, const char *name) {
        return QByteArrayLiteral("{\"id\":\"") + id.toUtf8() +
               QByteArrayLiteral("\",\"name\":\"") + QByteArray(name) +
               QByteArrayLiteral("\",\"kind\":\"everyone\",\"visibility\":"
                                 "\"public\",\"canManage\":false,"
                                 "\"memberCount\":3,\"topic\":\"\","
                                 "\"keyVersion\":1}");
    };
    relay.responses.insert(
        QStringLiteral("/api/chat/channels"),
        QByteArrayLiteral("{\"channels\":[") +
            everyoneChannel(generalId, "general") + ',' +
            everyoneChannel(randomId, "random") + ',' +
            everyoneChannel(welcomeId, "welcome") +
            QByteArrayLiteral(",{\"id\":\"") + designId.toUtf8() +
            QByteArrayLiteral("\",\"name\":\"design\",\"visibility\":"
                              "\"public\",\"canManage\":true,\"topic\":\"\","
                              "\"keyVersion\":1}]}"));
    relay.responses.insert(
        QStringLiteral("/api/chat/channels/") + generalId +
            QStringLiteral("/members"),
        QByteArrayLiteral("{\"everyone\":true,\"members\":["
                          "{\"username\":\"ada\",\"joinedAt\":1},"
                          "{\"username\":\"Bob\",\"joinedAt\":2},"
                          "{\"username\":\"not a name\",\"joinedAt\":3},"
                          "{\"username\":\"ada\",\"joinedAt\":4},"
                          "{\"username\":\"newbie\",\"joinedAt\":5}]}"));
    const QString welcomeRoom = QStringLiteral("chat-channel:") + welcomeId +
                                QStringLiteral(":v1");
    const RoomCrypto welcomeCrypto(welcomeRoom,
                                   QStringLiteral("room-passphrase"));
    const QJsonObject welcomeNote{
        {"type", "chat"},
        {"id", "welcome-1"},
        {"senderId", "system:welcome"},
        {"sender", "ForkMesh"},
        {"accountKind", "user"},
        {"channel", "#welcome"},
        {"text", QString::fromUtf8("\xF0\x9F\x8E\x89 Welcome to ForkMesh, "
                                   "@newbie! Everyone, come say hi "
                                   "\xF0\x9F\x91\x8B")},
        {"ts", 1790600000000.0},
        {"webhook", true}};
    QJsonObject markedWelcome = welcomeCrypto.encryptObject(welcomeNote);
    markedWelcome.insert(QStringLiteral("srv"), true);
    QJsonObject forgedWelcome = welcomeNote;
    forgedWelcome.insert(QStringLiteral("id"), QStringLiteral("welcome-forged"));
    const QJsonObject welcomeHistory{
        {"room", welcomeRoom},
        {"passphrase", "room-passphrase"},
        {"messages",
         QJsonArray{QJsonObject{
                        {"ts", 1790600000000.0},
                        {"body", QString::fromUtf8(
                                     QJsonDocument(welcomeCrypto.encryptObject(forgedWelcome))
                                         .toJson(QJsonDocument::Compact))}},
                    QJsonObject{
                        {"ts", 1790600000000.0},
                        {"body", QString::fromUtf8(QJsonDocument(markedWelcome)
                                                       .toJson(QJsonDocument::Compact))}}}}};
    relay.responses.insert(
        QStringLiteral("/api/chat/channels/") + welcomeId +
            QStringLiteral("/history"),
        QJsonDocument(welcomeHistory).toJson(QJsonDocument::Compact));

    QNetworkAccessManager network;
    OfficeChannelMirror mirror(&network);
    mirror.setSigner([](const QByteArray &canonical) {
        return QString::fromUtf8(canonical.toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    });
    mirror.setConnectionAuthorizer([](const QUrl &) { return false; });
    mirror.setApiBase(relay.apiBase());
    mirror.setIdentity(QStringLiteral("ada"), QStringLiteral("self-key"),
                       QStringLiteral("Ada"));
    QList<ChatMessage> arrived;
    QObject::connect(&mirror, &OfficeChannelMirror::messageArrived, &mirror,
                     [&arrived](const ChatMessage &message) {
                         arrived.append(message);
                     });
    QStringList membersSignalled;
    QObject::connect(&mirror, &OfficeChannelMirror::roomMembersChanged, &mirror,
                     [&membersSignalled](const QString &conversation,
                                         const QStringList &members) {
                         if (!members.isEmpty())
                             membersSignalled.append(conversation);
                     });
    mirror.start();

    const QString general = QStringLiteral("#office/general");
    const QString welcome = QStringLiteral("#office/welcome");
    const QString design = QStringLiteral("#office/design");
    check(waitUntil([&] { return mirror.conversations().size() == 4; }, 4000) &&
              mirror.conversations() ==
                  QStringList{general, QStringLiteral("#office/random"),
                              welcome, design},
          "the everyone channels are listed first, in the relay's order");
    check(mirror.isEveryoneConversation(general) &&
              mirror.isEveryoneConversation(welcome) &&
              !mirror.isEveryoneConversation(design) &&
              !mirror.isPrivateConversation(general),
          "only kind:\"everyone\" channels are flagged as everyone channels");
    check(!mirror.canManageConversation(general) &&
              mirror.canManageConversation(design) &&
              !mirror.updateChannel(general, QStringLiteral("general"),
                                    QStringLiteral("x"), false) &&
              !mirror.deleteChannel(general) &&
              !mirror.addMember(general, QStringLiteral("bob")),
          "an everyone channel offers no manage, delete or invite action");
    check(mirror.canSend(general), "every member can post in #general");

    const QString membersPath = QStringLiteral("/api/chat/channels/") +
                                generalId + QStringLiteral("/members");
    check(waitUntil([&] { return mirror.membersForConversation(welcome).size() ==
                                 3; },
                    4000),
          "the member list comes from the everyone members endpoint");
    check(mirror.membersForConversation(general) ==
                  QStringList{QStringLiteral("ada"), QStringLiteral("bob"),
                              QStringLiteral("newbie")} &&
              mirror.memberCountForConversation(general) == 3 &&
              mirror.membersForConversation(design).isEmpty(),
          "members are normalized, deduplicated and shared by all three");
    const RelayHarness::Call *membersCall = relay.lastCall("GET", membersPath);
    check(membersCall &&
              QByteArray::fromBase64(
                  queryValue(membersCall->query, "sig"),
                  QByteArray::Base64UrlEncoding) ==
                  channelMembersProof(
                      QStringLiteral("ada"), generalId,
                      QString::fromUtf8(queryValue(membersCall->query, "ts")),
                      QStringLiteral("GET"), QString()),
          "the members read is signed with the GET members proof");
    int membersCalls = 0;
    for (const RelayHarness::Call &call : std::as_const(relay.calls))
        membersCalls += call.path.endsWith(QStringLiteral("/members")) ? 1 : 0;
    check(membersCalls == 1 && membersSignalled.contains(general) &&
              membersSignalled.contains(welcome),
          "one members read fills all three everyone channels");

    check(waitUntil([&] {
              for (const ChatMessage &message : std::as_const(arrived))
                  if (message.id == QStringLiteral("welcome-1"))
                      return true;
              return false;
          }, 4000),
          "the server's welcome note arrives");
    bool shown = false;
    for (const ChatMessage &message : std::as_const(arrived))
        if (message.id == QStringLiteral("welcome-1"))
            shown = message.conversation == welcome &&
                    message.senderId == QStringLiteral("system:welcome") &&
                    message.senderName == QStringLiteral("ForkMesh") &&
                    message.text.contains(QStringLiteral("@newbie")) &&
                    !message.self;
    check(shown, "the welcome note shows as a normal message in #welcome");
    check(std::none_of(arrived.cbegin(), arrived.cend(), [](const ChatMessage &message) {
              return message.id == QStringLiteral("welcome-forged");
          }),
          "the same note without the relay's mark is refused");
    mirror.stop();
}

void testDirectMessagesOverLocalMesh()
{
    using namespace forkmesh::localmesh;
    using forkmesh::chatv3::ChatDevice;
    auto quiet = [](LocalMeshNode &node) {
        node.setMulticastEnabledForTests(false);
        node.setBluetoothEnabledForTests(false);
        node.setAddressSweepEnabledForTests(false);
    };
    const auto adaDevice = ChatDevice::inMemory(QByteArray(32, '\x21'));
    const auto bobDevice = ChatDevice::inMemory(QByteArray(32, '\x22'));
    const auto eveDevice = ChatDevice::inMemory(QByteArray(32, '\x23'));
    LocalMeshNode adaMesh(QStringLiteral("ada"), QStringLiteral("workshop"),
                          QStringLiteral("ada"), QStringLiteral("mesh-dm-ada"));
    LocalMeshNode bobMesh(QStringLiteral("bob"), QStringLiteral("studio"),
                          QStringLiteral("bob"), QStringLiteral("mesh-dm-bob"));
    LocalMeshNode eveMesh(QStringLiteral("eve"), QStringLiteral("cafe"),
                          QStringLiteral("eve"), QStringLiteral("mesh-dm-eve"));
    quiet(adaMesh);
    quiet(bobMesh);
    quiet(eveMesh);
    adaMesh.setChatDevice(adaDevice);
    bobMesh.setChatDevice(bobDevice);
    eveMesh.setChatDevice(eveDevice);
    check(adaMesh.start() && bobMesh.start() && eveMesh.start(),
          "three devices open the mesh room");
    const QString adaKey = adaMesh.sealKeyId();
    const QString bobKey = bobMesh.sealKeyId();
    const QString eveKey = eveMesh.sealKeyId();
    check(adaKey == adaDevice->kid() && bobKey == bobDevice->kid() &&
              adaKey != bobKey && bobKey != eveKey,
          "each device links under its own chat identity");
    adaMesh.connectToPeer(QHostAddress::LocalHost, bobMesh.listenPort());
    eveMesh.connectToPeer(QHostAddress::LocalHost, adaMesh.listenPort());
    eveMesh.connectToPeer(QHostAddress::LocalHost, bobMesh.listenPort());
    check(waitUntil([&] { return adaMesh.peerCount() == 2 &&
                                 bobMesh.peerCount() == 2 &&
                                 eveMesh.peerCount() == 2; },
                    6000),
          "the three devices are linked");
    waitUntil([] { return false; }, 300);
    check(adaMesh.linkedDevices().size() == 2,
          "each authenticated link is one linked device");

    const QString conversationId(32, 'f');
    const QString channel = QStringLiteral("direct:") + conversationId;
    const QString dmRoom = QStringLiteral("chat-direct:%1:v1").arg(conversationId);
    RoomSeal adaDm(dmRoom, channel, RoomSeal::Policy::Strict, QStringLiteral("ada"),
                   QStringLiteral("ada-self"), adaDevice);
    RoomSeal bobDm(dmRoom, channel, RoomSeal::Policy::Strict, QStringLiteral("bob"),
                   QStringLiteral("bob-self"), bobDevice);
    RoomSeal eveDm(dmRoom, channel, RoomSeal::Policy::Strict, QStringLiteral("eve"),
                   QStringLiteral("eve-self"), eveDevice);
    adaDm.acceptKeyFrame(bobDm.keyFrame());
    bobDm.acceptKeyFrame(adaDm.keyFrame());
    eveDm.acceptKeyFrame(bobDm.keyFrame());

    QList<ChatMessage> roomSaw;
    for (LocalMeshNode *node : {&adaMesh, &bobMesh, &eveMesh})
        QObject::connect(node, &LocalMeshNode::messageArrived, node,
                         [&roomSaw](const ChatMessage &m) { roomSaw.append(m); });
    QList<QPair<QString, QJsonObject>> bobSaw;
    bobMesh.setDirectHandler([&](const QString &kid, const QString &,
                                 const QJsonObject &frame) {
        bobSaw.append({kid, frame});
        return bobDm.unsealObject(frame).value(QStringLiteral("id")).toString();
    });
    int eveSaw = 0;
    eveMesh.setDirectHandler([&eveSaw](const QString &, const QString &,
                                       const QJsonObject &) {
        ++eveSaw;
        return QString();
    });
    QStringList adaAcks;
    QObject::connect(&adaMesh, &LocalMeshNode::directAcked, &adaMesh,
                     [&adaAcks](const QString &, const QString &, const QString &id) {
                         adaAcks.append(id);
                     });

    const QJsonObject probe{{QStringLiteral("type"), QStringLiteral("chat")},
                            {QStringLiteral("id"), QStringLiteral("dm-probe")},
                            {QStringLiteral("senderId"), QStringLiteral("ada-self")},
                            {QStringLiteral("channel"), channel},
                            {QStringLiteral("text"), QStringLiteral("only bob")}};
    const QJsonObject probeFrame = adaDm.sealObjectFor(probe, {bobKey});
    check(adaMesh.sendDirectSealed(bobKey, channel, probeFrame),
          "a direct-sealed frame goes to the one linked device");
    check(waitUntil([&] { return !bobSaw.isEmpty(); }, 4000) &&
              bobSaw.first().first == adaKey && bobSaw.first().second == probeFrame,
          "the recipient gets the sealed frame with the link's authenticated device");
    check(waitUntil([&] { return adaAcks == QStringList{QStringLiteral("dm-probe")}; },
                    4000),
          "the recipient answers with a direct-ack naming the message");
    waitUntil([] { return false; }, 200);
    check(eveSaw == 0 && roomSaw.isEmpty(),
          "nobody else on the mesh sees it, and it never lands in #mesh");
    check(!adaMesh.sendDirectSealed(QStringLiteral("not-a-linked-key"), channel,
                                    probeFrame),
          "a direct frame with no linked recipient device is not sent");
    check(!eveMesh.sendDirectSealed(bobKey, channel, probeFrame),
          "a device cannot pass another device's frame off as its own");

    RelayHarness adaRelay;
    RelayHarness bobRelay;
    check(adaRelay.listen() && bobRelay.listen(),
          "two relay stand-ins bind loopback ports");
    auto listing = [&conversationId](const char *other) {
        return QByteArrayLiteral("{\"conversations\":[{\"id\":\"") +
               conversationId.toUtf8() + QByteArrayLiteral("\",\"otherUser\":\"") +
               QByteArray(other) + QByteArrayLiteral("\"}]}");
    };
    adaRelay.responses.insert(QStringLiteral("/api/chat/direct-messages"),
                              listing("bob"));
    bobRelay.responses.insert(QStringLiteral("/api/chat/direct-messages"),
                              listing("ada"));
    auto remember = [&](const QUrl &apiBase, const QString &account,
                        const QString &other) {
        const QJsonObject root{
            {QStringLiteral("v"), 1},
            {QStringLiteral("apiBase"), apiBase.toString()},
            {QStringLiteral("account"), account},
            {QStringLiteral("conversations"),
             QJsonArray{QJsonObject{{QStringLiteral("id"), conversationId},
                                    {QStringLiteral("otherUser"), other},
                                    {QStringLiteral("room"), dmRoom}}}}};
        QSettings().setValue(QStringLiteral("chat/direct/meshDirectory"),
                             QJsonDocument(root).toJson(QJsonDocument::Compact));
    };
    auto prepare = [](OfficeChannelMirror &mirror, const QUrl &apiBase,
                      const QString &account) {
        mirror.setSigner([](const QByteArray &canonical) {
            return QString::fromUtf8(canonical.toBase64(
                QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
        });
        mirror.setConnectionAuthorizer([](const QUrl &) { return false; });
        mirror.setApiBase(apiBase);
        mirror.setIdentity(account, account + QStringLiteral("-self"), account);
    };
    auto wire = [](OfficeChannelMirror &mirror, LocalMeshNode &mesh) {
        OfficeChannelMirror::MeshTransport transport;
        transport.linkedDevices = [&mesh] { return mesh.linkedDevices(); };
        transport.sendDirect = [&mesh](const QString &kid, const QString &ch,
                                       const QJsonObject &frame) {
            return mesh.sendDirectSealed(kid, ch, frame);
        };
        transport.sendFocus = [&mesh](const QString &ch, bool on) {
            mesh.setDirectFocus(ch, on);
        };
        mirror.setMeshTransport(transport);
        mesh.setDirectHandler([&mirror](const QString &kid, const QString &ch,
                                        const QJsonObject &frame) {
            return mirror.acceptMeshDirect(kid, ch, frame);
        });
        QObject::connect(&mesh, &LocalMeshNode::directAcked, &mirror,
                         &OfficeChannelMirror::meshDirectAcked);
        QObject::connect(&mesh, &LocalMeshNode::linkedDevicesChanged, &mirror,
                         &OfficeChannelMirror::meshLinksChanged);
        QObject::connect(&mesh, &LocalMeshNode::peerFocusChanged, &mirror,
                         &OfficeChannelMirror::meshPeerFocusChanged);
    };

    QNetworkAccessManager network;
    OfficeChannelMirror adaMirror(&network);
    OfficeChannelMirror bobMirror(&network);
    adaMirror.setChatDevice(adaDevice);
    bobMirror.setChatDevice(bobDevice);
    prepare(adaMirror, adaRelay.apiBase(), QStringLiteral("ada"));
    prepare(bobMirror, bobRelay.apiBase(), QStringLiteral("bob"));
    wire(adaMirror, adaMesh);
    wire(bobMirror, bobMesh);
    QList<ChatMessage> bobShown;
    QObject::connect(&bobMirror, &OfficeChannelMirror::messageArrived, &bobMirror,
                     [&bobShown](const ChatMessage &m) { bobShown.append(m); });
    QStringList adaFailed;
    QObject::connect(&adaMirror, &OfficeChannelMirror::messageSendFailed,
                     &adaMirror,
                     [&adaFailed](const QString &, const QString &id,
                                  const QString &, const QString &) {
                         adaFailed.append(id);
                     });
    QStringList adaDelivered;
    QObject::connect(&adaMirror, &OfficeChannelMirror::messagesDelivered, &adaMirror,
                     [&adaDelivered](const QString &, const QStringList &ids) {
                         adaDelivered.append(ids);
                     });
    QStringList adaActivity;
    QObject::connect(&adaMirror, &OfficeChannelMirror::sendActivity, &adaMirror,
                     [&adaActivity](const QString &text) { adaActivity.append(text); });
    QList<ChatMessage> adaShown;
    QObject::connect(&adaMirror, &OfficeChannelMirror::messageArrived, &adaMirror,
                     [&adaShown](const ChatMessage &m) { adaShown.append(m); });

    remember(adaRelay.apiBase(), QStringLiteral("ada"), QStringLiteral("bob"));
    adaMirror.start();
    remember(bobRelay.apiBase(), QStringLiteral("bob"), QStringLiteral("ada"));
    bobMirror.start();
    const QString toBob = QStringLiteral("#office/@bob");
    const QString toAda = QStringLiteral("#office/@ada");
    check(adaMirror.directConversations() == QStringList{toBob} &&
              adaMirror.directDeviceKeys(toBob) == QStringList{bobKey} &&
              bobMirror.directConversations() == QStringList{toAda},
          "direct messages and their approved devices are known with no relay reply");
    check(adaMirror.directRouteLabel(toBob) == QStringLiteral("Direct · Wi-Fi"),
          "a linked approved device of the other person shows the direct route");

    check(adaMirror.sendMessage(toBob, QStringLiteral("the router is down")),
          "a direct message is accepted while the relay cannot be reached");
    const QString firstId = adaShown.isEmpty() ? QString() : adaShown.last().id;
    check(waitUntil([&] { return !bobShown.isEmpty(); }, 4000) &&
              bobShown.first().conversation == toAda &&
              bobShown.first().text == QStringLiteral("the router is down") &&
              bobShown.first().senderName == QStringLiteral("ada") &&
              !bobShown.first().self,
          "the other person's desktop shows it in their direct conversation");
    check(waitUntil([&] { return adaDelivered.contains(firstId); }, 4000),
          "the direct-ack marks the message delivered");
    const QString accessPath =
        QStringLiteral("/api/chat/direct-messages/%1/room-access")
            .arg(conversationId);
    check(waitUntil([&] { return adaRelay.lastCall("GET", accessPath); }, 4000),
          "without focus the relay copy is still sent");
    waitUntil([] { return false; }, 300);
    check(adaFailed.isEmpty(),
          "a message the mesh delivered is not reported as failed when the "
          "relay is unreachable");

    QJsonObject relabelled = adaDm.sealObjectFor(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("chat")},
                    {QStringLiteral("id"), QStringLiteral("forged-1")},
                    {QStringLiteral("senderId"), QStringLiteral("ada-self")},
                    {QStringLiteral("channel"), channel},
                    {QStringLiteral("text"), QStringLiteral("relabelled")}},
        {bobKey});
    check(bobMirror.acceptMeshDirect(eveKey, channel, relabelled).isEmpty(),
          "a frame is accepted only from the device the link authenticated");
    const QJsonObject eveFrame = eveDm.sealObjectFor(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("chat")},
                    {QStringLiteral("id"), QStringLiteral("forged-2")},
                    {QStringLiteral("senderId"), QStringLiteral("eve-self")},
                    {QStringLiteral("channel"), channel},
                    {QStringLiteral("text"), QStringLiteral("it is ada, honest")}},
        {bobKey});
    check(eveMesh.sendDirectSealed(bobKey, channel, eveFrame),
          "another device on the mesh can address bob");
    waitUntil([] { return false; }, 500);
    bool forgedShown = false;
    for (const ChatMessage &message : std::as_const(bobShown))
        forgedShown = forgedShown || message.text.contains(QStringLiteral("honest")) ||
                      message.text.contains(QStringLiteral("relabelled"));
    check(!forgedShown && bobShown.size() == 1,
          "but bob's desktop holds it: that device was never approved in the DM");

    adaMirror.setFocusedConversation(toBob);
    bobMirror.setFocusedConversation(toAda);
    check(waitUntil([&] {
              return adaMirror.directRouteLabel(toBob) == QStringLiteral("Direct only");
          }, 4000),
          "while both people look at the chat the route is direct only");
    adaActivity.clear();
    check(adaMirror.sendMessage(toBob, QStringLiteral("while we both look")),
          "a focused message is accepted");
    const QString focusedId = adaShown.isEmpty() ? QString() : adaShown.last().id;
    check(waitUntil([&] { return adaDelivered.contains(focusedId); }, 4000) &&
              bobShown.size() == 2,
          "a direct-only message is delivered and acknowledged over the mesh");
    waitUntil([] { return false; }, forkmesh::office::kDirectOnlyAckMs + 300);
    bool relayed = false;
    for (const QString &line : std::as_const(adaActivity))
        relayed = relayed || line.contains(QStringLiteral("through the relay"));
    check(!relayed, "an acknowledged direct-only message never goes to the relay");

    bobMesh.setDirectHandler([](const QString &, const QString &, const QJsonObject &) {
        return QString();
    });
    adaActivity.clear();
    check(adaMirror.sendMessage(toBob, QStringLiteral("nobody answers")),
          "a direct-only message is accepted when the peer stops answering");
    check(waitUntil([&] {
              for (const QString &line : std::as_const(adaActivity))
                  if (line.contains(QStringLiteral("through the relay")))
                      return true;
              return false;
          }, forkmesh::office::kDirectOnlyAckMs + 2000),
          "with no direct-ack within 3 s the relay copy is sent");

    bobMirror.setFocusedConversation(QString());
    check(waitUntil([&] {
              return adaMirror.directRouteLabel(toBob) == QStringLiteral("Direct · Wi-Fi");
          }, 4000),
          "when the other person looks away the route is no longer direct only");

    adaMirror.stop();
    bobMirror.stop();
    adaMesh.stop();
    bobMesh.stop();
    eveMesh.stop();
}

void testLocalMeshLinkV3()
{
    using namespace forkmesh::localmesh;
    using namespace forkmesh::chatv3;
    LocalMeshNode node(QStringLiteral("desk"), QStringLiteral("workshop"),
                       QStringLiteral("desk"), QStringLiteral("mesh-v3-node"));
    node.setMulticastEnabledForTests(false);
    node.setBluetoothEnabledForTests(false);
    node.setAddressSweepEnabledForTests(false);
    QList<QByteArray> controlHellos;
    node.setControlHandler([&controlHellos](QTcpSocket *socket, const QByteArray &hello,
                                            const QByteArray &) {
        controlHellos.append(hello);
        socket->close();
        socket->deleteLater();
    });
    QList<ChatMessage> nodeSaw;
    QObject::connect(&node, &LocalMeshNode::messageArrived, &node,
                     [&nodeSaw](const ChatMessage &m) { nodeSaw.append(m); });
    check(node.start(), "the node opens for the v3 link test");

    struct Client {
        QTcpSocket socket;
        QByteArray buffer;
        QList<QByteArray> frames;
        bool take(int timeoutMs, QByteArray *out)
        {
            const bool ok = waitUntil([&] {
                buffer += socket.readAll();
                QByteArray payload;
                bool overflow = false;
                while (takeFrame(buffer, &payload, &overflow))
                    frames.append(payload);
                return !frames.isEmpty();
            }, timeoutMs);
            if (ok && out)
                *out = frames.takeFirst();
            return ok;
        }
        void send(const QByteArray &payload) { socket.write(frameBytes(payload)); }
    };
    const auto compact = [](const QJsonObject &object) {
        return QJsonDocument(object).toJson(QJsonDocument::Compact);
    };
    const auto connectClient = [&](Client &client) {
        client.socket.connectToHost(QHostAddress::LocalHost, node.listenPort());
        return client.socket.waitForConnected(2000);
    };

    const auto phone = ChatDevice::inMemory(QByteArray(32, '\x31'));
    Client client;
    check(connectClient(client), "a phone dials the desktop's mesh port");
    const QByteArray eph = randomBytes(32);
    const QJsonObject hello = linkHello(phone->idk(), x25519Public(eph), randomBytes(16));
    client.send(compact(hello));
    QByteArray payload;
    check(client.take(3000, &payload), "the desktop answers with its own hello");
    const QJsonObject nodeHello = QJsonDocument::fromJson(payload).object();
    check(isLinkHello(nodeHello) &&
              id16(fromB64u(nodeHello.value(QStringLiteral("idk")).toString())) ==
                  node.sealKeyId(),
          "the desktop's link hello names its chat identity key in plaintext");
    const QByteArray transcript = linkTranscript(hello, nodeHello);
    check(client.take(3000, &payload), "the desktop sends its link auth");
    const QJsonObject nodeAuth = QJsonDocument::fromJson(payload).object();
    check(nodeAuth.value(QStringLiteral("t")).toString() == QStringLiteral("fm-link-auth") &&
              ed25519Verify(fromB64u(nodeHello.value(QStringLiteral("idk")).toString()),
                            fromB64u(nodeAuth.value(QStringLiteral("sig")).toString()),
                            linkAuthMessage(QStringLiteral("r"), transcript)),
          "the responder signs the transcript in the responder role");
    client.send(compact(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("fm-link-auth")},
        {QStringLiteral("sig"),
         b64u(phone->sign(linkAuthMessage(QStringLiteral("i"), transcript)))}}));
    QByteArray i2r;
    QByteArray r2i;
    check(linkKeys(eph, fromB64u(nodeHello.value(QStringLiteral("eph")).toString()),
                   transcript, &i2r, &r2i),
          "the phone derives the link keys");
    quint64 inCounter = 0;
    quint64 outCounter = 0;
    const auto nextFrame = [&](QJsonObject *out) {
        QByteArray data;
        if (!client.take(3000, &data))
            return false;
        QByteArray clear;
        if (!linkOpen(r2i, inCounter++, data, &clear))
            return false;
        *out = QJsonDocument::fromJson(clear).object();
        return true;
    };
    QJsonObject first;
    check(nextFrame(&first) && first.value(QStringLiteral("type")).toString() ==
                                   QStringLiteral("e2ee-key") &&
              verifyBundle(first, roomName(), QDateTime::currentMSecsSinceEpoch()) &&
              verifyNodeBinding(first, roomName()) &&
              first.value(QStringLiteral("node")).toString() ==
                  meshNode(QStringLiteral("mesh-v3-node")) &&
              first.value(QStringLiteral("acct")).toString() ==
                  first.value(QStringLiteral("node")).toString(),
          "the first encrypted frame is the desktop's #mesh bundle, bound to its node key");
    const QString phoneNode = nodeKey(QStringLiteral("phone-node"));
    RoomSeal phoneSeal(roomName(), roomName(), RoomSeal::Policy::Open, phoneNode,
                       phoneNode, phone);
    phoneSeal.setNodeSigner(nodeSigner(QStringLiteral("phone-node")));
    phoneSeal.acceptKeyFrame(first);
    const auto sendLink = [&](const QJsonObject &frame) {
        client.send(linkSeal(i2r, outCounter++, compact(frame)));
    };
    sendLink(phoneSeal.keyFrame());
    sendLink(phoneSeal.sealObject(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("hello")},
        {QStringLiteral("id"), QStringLiteral("phone-hello-1")},
        {QStringLiteral("senderId"), phoneNode},
        {QStringLiteral("ownerUser"), QStringLiteral("someone-else")},
        {QStringLiteral("sender"), QStringLiteral("Phone")},
        {QStringLiteral("platform"), QStringLiteral("ios")},
        {QStringLiteral("conversation"), conversation()}}));
    check(waitUntil([&] { return node.peerCount() == 1; }, 3000),
          "the desktop admits the authenticated phone to #mesh");
    bool named = false;
    for (const MemberInfo &member : node.members())
        named = named || (member.id == phoneNode &&
                          member.name == QStringLiteral("Phone") &&
                          member.ownerUser.isEmpty());
    check(named, "names arrive only inside the encrypted link, and no account claim is kept");
    const QString said = node.sendChat(QStringLiteral("over v3"));
    bool heard = false;
    bool ownerSent = false;
    int helloSeen = 0;
    QJsonObject frame;
    for (int i = 0; i < 8 && !heard && nextFrame(&frame); ++i) {
        const QJsonObject plain = phoneSeal.unsealObject(frame);
        ownerSent = ownerSent || plain.contains(QStringLiteral("ownerUser"));
        if (plain.value(QStringLiteral("type")).toString() == QStringLiteral("hello"))
            ++helloSeen;
        heard = plain.value(QStringLiteral("id")).toString() == said &&
                plain.value(QStringLiteral("text")).toString() == QStringLiteral("over v3");
    }
    check(heard, "#mesh chat reaches the phone as a v3 sealed frame inside the link");
    check(helloSeen == 1 && !ownerSent, "#mesh frames carry no ownerUser");
    sendLink(phoneSeal.sealObject(QJsonObject{
        {QStringLiteral("type"), QStringLiteral("chat")},
        {QStringLiteral("id"), QStringLiteral("phone-chat-1")},
        {QStringLiteral("senderId"), phoneNode},
        {QStringLiteral("sender"), QStringLiteral("Phone")},
        {QStringLiteral("conversation"), conversation()},
        {QStringLiteral("text"), QStringLiteral("from the phone")}}));
    check(waitUntil([&] {
              return std::any_of(nodeSaw.cbegin(), nodeSaw.cend(), [](const ChatMessage &m) {
                  return m.id == QStringLiteral("phone-chat-1") &&
                         m.text == QStringLiteral("from the phone");
              });
          }, 3000),
          "the desktop opens the phone's sealed #mesh chat");
    client.send(QByteArray(40, 'x'));
    check(waitUntil([&] { return client.socket.state() != QAbstractSocket::ConnectedState; },
                    3000),
          "a link frame that fails to open closes the link");

    Client forger;
    check(connectClient(forger), "a second device dials");
    const QByteArray forgerEph = randomBytes(32);
    const auto forgerDevice = ChatDevice::inMemory(QByteArray(32, '\x32'));
    const QJsonObject forgerHello =
        linkHello(forgerDevice->idk(), x25519Public(forgerEph), randomBytes(16));
    forger.send(compact(forgerHello));
    forger.take(3000, nullptr);
    forger.take(3000, nullptr);
    forger.send(compact(QJsonObject{
        {QStringLiteral("t"), QStringLiteral("fm-link-auth")},
        {QStringLiteral("sig"), b64u(forgerDevice->sign("not the transcript"))}}));
    check(waitUntil([&] { return forger.socket.state() != QAbstractSocket::ConnectedState; },
                    3000),
          "a bad link signature closes the link");

    Client mirror;
    check(connectClient(mirror), "a device claiming the desktop's own key dials");
    const auto nodeDevice =
        ChatDevice::sharedForTests(sha256("forkmesh-mesh-test:mesh-v3-node"));
    mirror.send(compact(linkHello(nodeDevice->idk(), x25519Public(randomBytes(32)),
                                  randomBytes(16))));
    check(waitUntil([&] { return mirror.socket.state() != QAbstractSocket::ConnectedState; },
                    3000),
          "a hello with the desktop's own identity key closes the link");

    Client legacy;
    check(connectClient(legacy), "a v2 device dials");
    legacy.send(compact(RoomCrypto(roomName()).encryptObject(
        QJsonObject{{QStringLiteral("type"), QStringLiteral("e2ee-key")},
                    {QStringLiteral("v"), 2}})));
    check(waitUntil([&] { return legacy.socket.state() != QAbstractSocket::ConnectedState; },
                    3000),
          "a v2 link is refused");

    Client control;
    check(connectClient(control), "a phone opens a control channel on the same port");
    control.send(QByteArrayLiteral("{\"t\":\"fm-control-hello\",\"v\":1}"));
    check(waitUntil([&] { return controlHellos.size() == 1; }, 3000) &&
              controlHellos.first().contains("fm-control-hello"),
          "fm-control-hello still starts local control, not a mesh link");
    node.stop();
}

void testOfficeRoomMutationFrames()
{
    using namespace forkmesh::office;
    const QJsonObject reply{{"type", "thread-reply"},
                            {"id", "reply-1"},
                            {"senderId", "bob-node"},
                            {"sender", "bob"},
                            {"rootId", "root-1"},
                            {"channel", "direct:abc"},
                            {"text", "replying"},
                            {"ts", 1000.0}};
    ChatMessage message;
    check(chatMessageFromPlain(reply, QStringLiteral("#office/@bob"),
                               QStringLiteral("self"), &message) &&
              message.threadRootId == QStringLiteral("root-1") &&
              message.text == QStringLiteral("replying"),
          "an office thread reply becomes a reply under its root");
    QJsonObject rootless = reply;
    rootless.remove("rootId");
    check(!chatMessageFromPlain(rootless, QStringLiteral("#office/@bob"),
                                QStringLiteral("self"), &message),
          "a thread reply without a root is dropped");

    RoomMutation mutation;
    const QJsonObject reaction{{"type", "reaction"},
                               {"id", "r-1"},
                               {"senderId", "bob-node"},
                               {"sender", "bob"},
                               {"target", "m-1"},
                               {"emoji", "👍"},
                               {"reactorId", "bob-node"},
                               {"reactorName", "bob"},
                               {"added", false}};
    check(roomMutationFromPlain(reaction, &mutation) &&
              mutation.type == QStringLiteral("reaction") &&
              mutation.target == QStringLiteral("m-1") &&
              mutation.emoji == QStringLiteral("👍") &&
              mutation.reactorName == QStringLiteral("bob") && !mutation.added,
          "a reaction frame is read with its emoji, reactor and direction");
    QJsonObject spoofed = reaction;
    spoofed.insert("reactorId", "carol-node");
    check(!roomMutationFromPlain(spoofed, &mutation),
          "a reaction cannot claim another member as the reactor");
    QJsonObject noDirection = reaction;
    noDirection.remove("added");
    check(!roomMutationFromPlain(noDirection, &mutation),
          "a reaction must say whether it adds or removes");
    QJsonObject stuck = reaction;
    stuck.insert("emoji", QString::fromUcs4(U"\u2705"));
    stuck.insert("sticker", "lgtm");
    stuck.insert("added", true);
    check(roomMutationFromPlain(stuck, &mutation) &&
              mutation.emoji == stickers::valueForSlug(QStringLiteral("lgtm")) &&
              stickers::isSticker(mutation.emoji) && mutation.added,
          "a reaction frame naming a sticker is read as that sticker");
    QJsonObject unknownSticker = stuck;
    unknownSticker.insert("sticker", "from-a-newer-build");
    check(roomMutationFromPlain(unknownSticker, &mutation) &&
              mutation.emoji == QString::fromUcs4(U"\u2705"),
          "an unknown sticker falls back to its emoji reaction");
    QJsonObject longSticker = stuck;
    longSticker.insert("emoji", QString(40, QLatin1Char('x')));
    check(!roomMutationFromPlain(longSticker, &mutation),
          "a sticker frame keeps the reaction emoji length limit");

    QJsonObject wire;
    stickers::writeWire(wire, stickers::valueForSlug(QStringLiteral("love-it")));
    check(wire.value("sticker").toString() == QStringLiteral("love-it") &&
              wire.value("emoji").toString() == QString::fromUcs4(U"\u2764\uFE0F") &&
              stickers::valueFromWire(wire) ==
                  stickers::valueForSlug(QStringLiteral("love-it")),
          "a sticker goes out with its fallback emoji and comes back as itself");
    QJsonObject plainReaction;
    stickers::writeWire(plainReaction, QStringLiteral("like"));
    check(plainReaction.value("emoji").toString() == QStringLiteral("like") &&
              !plainReaction.contains("sticker") &&
              stickers::valueFromWire(plainReaction) == QStringLiteral("like"),
          "an ordinary reaction carries no sticker field");
    bool everyStickerFallsBack = stickers::catalog().size() == 8;
    for (const stickers::CatalogEntry &entry : stickers::catalog())
        everyStickerFallsBack = everyStickerFallsBack &&
                                reactions::valueForUnicode(entry.fallback) !=
                                    entry.fallback &&
                                stickers::valueForSlug(entry.slug).size() <= 32;
    check(everyStickerFallsBack,
          "every sticker falls back to a reaction older builds can draw");
    const QJsonObject edit{{"type", "edit"},
                           {"id", "e-1"},
                           {"senderId", "bob-node"},
                           {"target", "m-1"},
                           {"text", "fixed"},
                           {"editedAt", 2000.0}};
    check(roomMutationFromPlain(edit, &mutation) &&
              mutation.type == QStringLiteral("edit") &&
              mutation.senderId == QStringLiteral("bob-node") &&
              mutation.text == QStringLiteral("fixed"),
          "an edit frame keeps its author so the window can check it");
    QJsonObject emptyEdit = edit;
    emptyEdit.insert("text", QStringLiteral("  "));
    check(!roomMutationFromPlain(emptyEdit, &mutation),
          "an edit to nothing is refused");
    const QJsonObject deletion{{"type", "delete"},
                               {"id", "d-1"},
                               {"senderId", "bob-node"},
                               {"target", "m-1"}};
    check(roomMutationFromPlain(deletion, &mutation) &&
              mutation.type == QStringLiteral("delete"),
          "a delete frame is read");
    QJsonObject anonymous = deletion;
    anonymous.remove("senderId");
    check(!roomMutationFromPlain(anonymous, &mutation) &&
              !roomMutationFromPlain(QJsonObject{{"type", "chat"}}, &mutation),
          "a mutation needs an author, and a chat is not a mutation");
}

void testOfficeDirectMessageRoundTrip()
{
    const QString conversationId(32, QLatin1Char('a'));
    const QString room = QStringLiteral("chat-direct:%1:v1").arg(conversationId);
    const QString passphrase = QStringLiteral("office-dm-passphrase");
    const QString wire = QStringLiteral("direct:") + conversationId;
    const QString conversation = QStringLiteral("#office/@bob");
    WebSocketHarness socket;
    socket.blackholeFirst = false;
    RelayHarness relay;
    check(socket.listen() && relay.listen(),
          "the office DM round trip binds its relay stand-ins");
    if (!socket.server.isListening() || !relay.server.isListening())
        return;
    QSettings().remove(QStringLiteral("chat/direct/meshDirectory"));
    relay.responses.insert(
        QStringLiteral("/api/chat/direct-messages"),
        QByteArrayLiteral("{\"conversations\":[{\"id\":\"") +
            conversationId.toUtf8() +
            QByteArrayLiteral("\",\"otherUser\":\"bob\"}]}"));
    relay.responses.insert(
        QStringLiteral("/api/chat/direct-messages/%1/room-access")
            .arg(conversationId),
        QJsonDocument(QJsonObject{{"room", room},
                                  {"passphrase", passphrase},
                                  {"webSocketUrl", socket.url().toString()}})
            .toJson(QJsonDocument::Compact));

    QNetworkAccessManager network;
    OfficeChannelMirror mirror(&network);
    mirror.setSigner([](const QByteArray &canonical) {
        return QString::fromUtf8(canonical.toBase64(
            QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    });
    mirror.setConnectionAuthorizer([](const QUrl &) { return true; });
    mirror.setApiBase(relay.apiBase());
    mirror.setIdentity(QStringLiteral("ada"), QStringLiteral("ada-key"),
                       QStringLiteral("Ada"));
    QList<ChatMessage> arrived;
    QList<QPair<QString, int>> acks;
    QList<QPair<QString, QStringList>> delivered;
    QStringList reads;
    QList<QStringList> reactions;
    QList<QStringList> edits;
    QList<QStringList> deletes;
    QObject::connect(&mirror, &OfficeChannelMirror::messageArrived, &mirror,
                     [&arrived](const ChatMessage &m) { arrived.append(m); });
    QObject::connect(&mirror, &OfficeChannelMirror::messageRecipientsKnown,
                     &mirror,
                     [&acks](const QString &, const QString &id, int count,
                             const QStringList &) { acks.append({id, count}); });
    QObject::connect(&mirror, &OfficeChannelMirror::messagesDelivered, &mirror,
                     [&delivered](const QString &c, const QStringList &ids) {
                         delivered.append({c, ids});
                     });
    QObject::connect(&mirror, &OfficeChannelMirror::conversationRead, &mirror,
                     [&reads](const QString &c) { reads.append(c); });
    QObject::connect(&mirror, &OfficeChannelMirror::reactionChanged, &mirror,
                     [&reactions](const QString &c, const QString &id,
                                  const QString &emoji, const QString &who,
                                  bool added) {
                         reactions.append({c, id, emoji, who,
                                           added ? QStringLiteral("+")
                                                 : QStringLiteral("-")});
                     });
    QObject::connect(&mirror, &OfficeChannelMirror::messageEdited, &mirror,
                     [&edits](const QString &c, const QString &id,
                              const QString &sender, const QString &text) {
                         edits.append({c, id, sender, text});
                     });
    QObject::connect(&mirror, &OfficeChannelMirror::messageDeleted, &mirror,
                     [&deletes](const QString &c, const QString &id,
                                const QString &sender) {
                         deletes.append({c, id, sender});
                     });
    mirror.start();

    RoomCrypto relayKey(room, passphrase);
    const auto inner = [&](const QByteArray &raw) {
        return relayKey.decryptObject(QJsonDocument::fromJson(raw).object());
    };
    QJsonObject adaKey;
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(socket.textFrames))
                  if (RoomSeal::isKeyFrame(inner(raw)))
                      adaKey = inner(raw);
              return !adaKey.isEmpty();
          }, 8000),
          "the office DM opens its encrypted room socket");
    if (adaKey.isEmpty()) {
        mirror.stop();
        return;
    }
    TestPeer bob(room, directScope(room), RoomSeal::Policy::Strict,
                 QStringLiteral("bob-node"), '\x0d', QStringLiteral("bob"));
    TestPeer outsider(room, directScope(room), RoomSeal::Policy::Strict,
                      QStringLiteral("mallory-node"), '\x0e', QStringLiteral("mallory"));
    outsider.accept(adaKey);
    socket.sendText(QJsonDocument(relayKey.encryptObject(outsider.keyFrame()))
                        .toJson(QJsonDocument::Compact));
    bob.accept(adaKey);
    socket.sendText(QJsonDocument(relayKey.encryptObject(bob.keyFrame()))
                        .toJson(QJsonDocument::Compact));
    check(waitUntil([&] { return mirror.directDeviceKeys(conversation).size() == 1; },
                    2000),
          "the mirror learns bob's device key");
    waitUntil([] { return false; }, 150);
    check(mirror.directDeviceKeys(conversation) == QStringList{bob.keyId()},
          "a device of someone outside the DM is dropped, not trusted");
    const auto findFrame = [&](const QString &type, QByteArray *raw) {
        for (const QByteArray &frame : std::as_const(socket.textFrames)) {
            const QJsonObject opened = bob.open(inner(frame));
            if (opened.value("type").toString() == type) {
                if (raw)
                    *raw = frame;
                return opened;
            }
        }
        return QJsonObject();
    };
    const auto relayFrame = [&](const QJsonObject &object) {
        socket.sendText(QJsonDocument(object).toJson(QJsonDocument::Compact));
    };
    const auto fromBob = [&](QJsonObject frame) {
        frame.insert("senderId", "bob-node");
        frame.insert("sender", "bob");
        frame.insert("ts", double(QDateTime::currentMSecsSinceEpoch()));
        QJsonObject envelope = relayKey.encryptObject(bob.sealObject(frame));
        envelope.insert("persist", true);
        relayFrame(envelope);
    };

    socket.textFrames.clear();
    check(mirror.sendMessage(conversation, QStringLiteral("hello bob")) &&
              !arrived.isEmpty() && arrived.last().self,
          "an office DM shows at once as the sending echo");
    const QString sentId = arrived.isEmpty() ? QString() : arrived.last().id;
    QByteArray sentFrame;
    check(waitUntil([&] { return !findFrame("chat", &sentFrame).isEmpty(); },
                    2000) &&
              findFrame("chat", nullptr).value("id").toString() == sentId,
          "the office DM is sealed to bob under the echoed id");
    const QString sentRef = forkmesh::recipients::frameRef(sentFrame);
    relayFrame(QJsonObject{{"kind", "forkmesh-recipients"},
                           {"v", 1},
                           {"ref", sentRef},
                           {"recipients", 1}});
    relayFrame(QJsonObject{{"kind", "forkmesh-delivered"},
                           {"v", 1},
                           {"refs", QJsonArray{sentRef}}});
    relayFrame(QJsonObject{{"kind", "forkmesh-read"}, {"v", 1}});
    check(waitUntil([&] { return !reads.isEmpty(); }, 2000) &&
              acks.size() == 1 && acks.first().first == sentId &&
              delivered.size() == 1 && delivered.first().first == conversation &&
              delivered.first().second == QStringList{sentId} &&
              reads == QStringList{conversation},
          "the relay's sent, delivered and read frames reach the conversation");

    const int arrivedBefore = int(arrived.size());
    check(mirror.sendThreadReply(conversation, sentId, QStringLiteral("p.s.")) &&
              arrived.size() == arrivedBefore + 1 &&
              arrived.last().threadRootId == sentId && arrived.last().self,
          "a reply in an office DM shows at once under its root");
    const QString ownReplyId = arrived.isEmpty() ? QString() : arrived.last().id;
    check(mirror.sendReaction(conversation, sentId, QStringLiteral("👍"), true) &&
              reactions.size() == 1 &&
              reactions.first() == QStringList({conversation, sentId,
                                                QStringLiteral("👍"),
                                                QStringLiteral("Ada"),
                                                QStringLiteral("+")}),
          "an own reaction applies locally at once");
    check(mirror.editMessage(conversation, sentId, QStringLiteral("hello again")) &&
              edits.size() == 1 &&
              edits.first() == QStringList({conversation, sentId,
                                            QStringLiteral("ada-key"),
                                            QStringLiteral("hello again")}),
          "an own edit applies locally with this account as its author");
    check(mirror.deleteMessage(conversation, sentId) && deletes.size() == 1 &&
              deletes.first() == QStringList({conversation, sentId,
                                              QStringLiteral("ada-key")}),
          "an own delete for everyone applies locally at once");
    QJsonObject superseded;
    check(waitUntil([&] {
              for (const QByteArray &raw : std::as_const(socket.textFrames)) {
                  const QJsonObject envelope =
                      QJsonDocument::fromJson(raw).object();
                  if (envelope.value("kind").toString() == "forkmesh-superseded")
                      superseded = envelope;
              }
              return !superseded.isEmpty();
          }, 2000),
          "deleting the office DM withdraws the relay's copy");
    check(findFrame("thread-reply", nullptr).value("rootId").toString() == sentId &&
              findFrame("thread-reply", nullptr).value("channel").toString() == wire &&
              findFrame("reaction", nullptr).value("target").toString() == sentId &&
              findFrame("reaction", nullptr).value("conversation").toString() == wire &&
              findFrame("edit", nullptr).value("text").toString() ==
                  QStringLiteral("hello again") &&
              findFrame("delete", nullptr).value("target").toString() == sentId &&
              superseded.value("refs").toArray() == QJsonArray{sentRef},
          "reply, reaction, edit, delete and superseded all reach the relay sealed");
    waitUntil([] { return false; }, 150);
    check(!ownReplyId.isEmpty() &&
              std::count_if(arrived.cbegin(), arrived.cend(),
                            [&ownReplyId](const ChatMessage &m) {
                                return m.id == ownReplyId;
                            }) == 1,
          "the relay's echo of an own office DM reply does not show it twice");

    fromBob(QJsonObject{{"type", "chat"},
                        {"id", "bob-1"},
                        {"channel", wire},
                        {"text", "hi ada"}});
    fromBob(QJsonObject{{"type", "thread-reply"},
                        {"id", "bob-2"},
                        {"channel", wire},
                        {"rootId", "bob-1"},
                        {"text", "and another thing"}});
    const QJsonObject bobReaction{{"type", "reaction"},
                                  {"id", "bob-3"},
                                  {"conversation", wire},
                                  {"target", "bob-1"},
                                  {"emoji", "🎉"},
                                  {"reactorId", "bob-node"},
                                  {"reactorName", "bob"},
                                  {"added", true}};
    fromBob(bobReaction);
    fromBob(bobReaction);
    fromBob(QJsonObject{{"type", "edit"},
                        {"id", "bob-4"},
                        {"conversation", wire},
                        {"target", "bob-1"},
                        {"text", "hi there ada"}});
    fromBob(QJsonObject{{"type", "delete"},
                        {"id", "bob-5"},
                        {"conversation", wire},
                        {"target", "bob-1"}});
    check(waitUntil([&] { return deletes.size() == 2; }, 3000),
          "bob's reply, reaction, edit and delete reach the office DM");
    waitUntil([] { return false; }, 150);
    const auto shown = [&](const QString &id) {
        for (const ChatMessage &m : std::as_const(arrived))
            if (m.id == id)
                return m;
        return ChatMessage();
    };
    check(shown("bob-1").conversation == conversation &&
              shown("bob-2").threadRootId == QStringLiteral("bob-1") &&
              !shown("bob-2").self,
          "bob's message and his threaded reply land in the conversation");
    check(reactions.size() == 2 &&
              reactions.last() == QStringList({conversation, QStringLiteral("bob-1"),
                                               QStringLiteral("🎉"),
                                               QStringLiteral("bob"),
                                               QStringLiteral("+")}),
          "bob's reaction is applied once even when the relay repeats it");
    check(edits.size() == 2 &&
              edits.last() == QStringList({conversation, QStringLiteral("bob-1"),
                                           QStringLiteral("bob-node"),
                                           QStringLiteral("hi there ada")}) &&
              deletes.last() == QStringList({conversation, QStringLiteral("bob-1"),
                                             QStringLiteral("bob-node")}),
          "bob's edit and delete carry his id for the author check");
    mirror.stop();
}

int main(int argc, char **argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ForkMeshTests"));
    app.setApplicationName(QStringLiteral("chat-transport"));
    forkmesh::chatv3::clearStoredState();

    const bool discordOnly =
        app.arguments().contains(QStringLiteral("--discord-only"));
    // The local mesh needs no relay at all, so it can be exercised on its own
    // — useful on a loaded machine, where the relay harness cases are timing
    // sensitive.
    if (app.arguments().contains(QStringLiteral("--mesh-only"))) {
        testLocalMeshAddressPolicy();
        testLocalMeshCrossClientConstants();
        testLocalMeshFraming();
        testLocalMeshBeaconIsCiphertext();
        testLocalMeshRoom();
        testLocalMeshNoResealToDirectJoiner();
        testLocalMeshQueuedUntilJoin();
        testLocalMeshWakesDevicesThatLeft();
        testLocalMeshReturningDeviceReplacesStaleLink();
        testLocalMeshDiscovery();
        testLocalMeshUnicastDiscovery();
        testLocalMeshFirewall();
        testLocalMeshLinkV3();
        if (failures == 0)
            std::puts("All local mesh tests passed.");
        return failures == 0 ? 0 : 1;
    }
    testNodeOwnersAndBlocks();
    testRelayRules();
    testTeammateChatRelay();
    if (!discordOnly) {
        testSavedRoomMigration();
        testPublicIntegrationsAndPrivateSeal();
        testServerFramesInChannelRooms();
        testTargetedConversationSealing();
        testDeterministicRecovery();
        testStatusIsEventDriven();
        testTeardownMidBufferLeavesSocketAlone();
        testNestedPumpInFrameHandlerKeepsDeliveringSocket();
        testUserStatusOnPresenceFrames();
        testRecipientCounts();
        testDirectRoomResealsUndeliveredMessages();
        testDirectRoomRatchetRecovery();
        testReceiptMarkStates();
        testDirectReceiptsAndRoomMutations();
        testOfficeChannelMirror();
        testOfficeChannelSettingsRequests();
        testOfficeNoticesChannelShowsServerFrames();
        testOfficeEveryoneChannels();
        testOfficePrivateRoomsAndDirectMessages();
        testDirectMessagesOverLocalMesh();
        testLocalMeshLinkV3();
        testOfficeRoomMutationFrames();
        testOfficeDirectMessageRoundTrip();
        testLocalMeshAddressPolicy();
        testLocalMeshCrossClientConstants();
        testLocalMeshFraming();
        testLocalMeshBeaconIsCiphertext();
        testLocalMeshRoom();
        testLocalMeshNoResealToDirectJoiner();
        testLocalMeshQueuedUntilJoin();
        testLocalMeshWakesDevicesThatLeft();
        testLocalMeshReturningDeviceReplacesStaleLink();
        testLocalMeshDiscovery();
        testLocalMeshUnicastDiscovery();
        testLocalMeshFirewall();
    }
    testDiscordClientContract();
    testChatFormat();
    testChatListPreview();
    if (failures == 0)
        std::puts("All chat transport tests passed.");
    return failures == 0 ? 0 : 1;
}
