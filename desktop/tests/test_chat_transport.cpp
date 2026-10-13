#include "MainnodeRoom.h"
#include "BleMeshLink.h"
#include "ChatBlocks.h"
#include "ChatFormat.h"
#include "ChatRecipients.h"
#include "DiscordClient.h"
#include "ReactionCatalog.h"
#include "LocalMeshNode.h"
#include "MeshFirewall.h"
#include "OfficeChannelMirror.h"
#include "RoomCrypto.h"
#include "RoomSeal.h"
#include "ServerNode.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
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
                if (connectionCount == 1)
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
    beacon.nodeId = QStringLiteral("node-ada-1");
    beacon.name = QStringLiteral("ada");
    beacon.nodeName = QStringLiteral("workshop");
    beacon.ownerUser = QStringLiteral("ada");
    beacon.platform = QStringLiteral("linux");
    beacon.version = QStringLiteral("9.9.9");
    beacon.port = 4242;
    beacon.timestampMs = 1750000000000ll;

    Beacon parsed;
    check(parseBeacon(beaconObject(beacon), &parsed) &&
              parsed.nodeId == beacon.nodeId && parsed.port == beacon.port &&
              parsed.name == beacon.name && parsed.nodeName == beacon.nodeName &&
              parsed.ownerUser == beacon.ownerUser &&
              parsed.version == beacon.version,
          "a beacon round-trips every field a roster entry needs");

    QJsonObject wrongTag = beaconObject(beacon);
    wrongTag.insert(QStringLiteral("t"), QStringLiteral("something-else"));
    QJsonObject wrongVersion = beaconObject(beacon);
    wrongVersion.insert(QStringLiteral("v"), 99);
    QJsonObject noPort = beaconObject(beacon);
    noPort.insert(QStringLiteral("port"), 0);
    check(!parseBeacon(wrongTag, &parsed) && !parseBeacon(wrongVersion, &parsed) &&
              !parseBeacon(noPort, &parsed) && !parseBeacon(QJsonObject(), &parsed),
          "anything that is not a beacon of a version we speak is dropped");

    // The datagram that actually goes out must not carry the advertisement in
    // the clear: another program on the same network sees ciphertext only.
    const RoomCrypto crypto(roomName());
    const QJsonObject envelope = crypto.encryptObject(beaconObject(beacon));
    const QByteArray datagram =
        QJsonDocument(envelope).toJson(QJsonDocument::Compact);
    check(!datagram.contains("node-ada-1") && !datagram.contains("workshop") &&
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
              typingPeer == QStringLiteral("mesh-node-ada"),
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
    check(waitUntil([&] { return !linusSaw.isEmpty(); }, 4000) &&
              linusSaw.first().text == QStringLiteral("anyone here?"),
          "a device that joins late is backfilled by the peers already here");

    // linus and ada never dialled each other: the room still reaches both.
    const QString relayed = ada.sendChat(QStringLiteral("second"));
    check(!relayed.isEmpty() &&
              waitUntil([&] { return linusSaw.size() == 2; }, 4000) &&
              linusSaw.at(1).text == QStringLiteral("second"),
          "a message crosses the mesh to a device with no direct link");
    check(waitUntil([&] { return graceSaw.size() == 2; }, 1000) &&
              graceSaw.at(1).text == QStringLiteral("second") &&
              adaSaw.size() == 2,
          "the relayed copy is delivered exactly once to every device");

    // grace keeps its link to linus, so only ada may disappear.
    ada.stop();
    const auto roomHasAda = [&] {
        for (const MemberInfo &member : grace.members())
            if (member.id == QStringLiteral("mesh-node-ada"))
                return member.online;
        return false;
    };
    check(waitUntil([&] { return !roomHasAda(); }, 4000) &&
              grace.peerCount() == 1,
          "a device that leaves drops out of the room");
    grace.stop();
    linus.stop();
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
    check(waitUntil([&] {
              return std::any_of(carolSaw.cbegin(), carolSaw.cend(),
                                 [&](const ChatMessage &m) {
                                     return m.id == sent &&
                                            m.text == QStringLiteral(
                                                          "anyone out there?");
                                 });
          }, 4000),
          "a later joiner gets the chat from whichever device it links to");
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
    foreign.nodeId = QStringLiteral("outsider");
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
    beacon.nodeId = QStringLiteral("mesh-discover-2");
    beacon.name = QStringLiteral("second");
    beacon.nodeName = QStringLiteral("two");
    beacon.port = second.listenPort();
    beacon.platform = QStringLiteral("linux");
    beacon.timestampMs = QDateTime::currentMSecsSinceEpoch();
    wire.writeDatagram(QJsonDocument(crypto.encryptObject(beaconObject(beacon)))
                           .toJson(QJsonDocument::Compact),
                       QHostAddress::LocalHost, 49731);
    check(waitUntil([&] {
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

    const RoomCrypto crypto(roomName());
    QUdpSocket wire;
    check(wire.bind(QHostAddress::LocalHost, 0), "the stand-in socket binds");
    Beacon beacon;
    beacon.nodeId = QStringLiteral("mesh-unicast-a");
    beacon.name = QStringLiteral("low");
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
              answer.nodeId == QStringLiteral("mesh-unicast-z") &&
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
    beacon.nodeId = QStringLiteral("mesh-firewall-ghost");
    beacon.name = QStringLiteral("ghost");
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

void testTeammateChatRelay()
{
    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), QStringLiteral("stable-node-id"),
                    QUrl(QStringLiteral("ws://127.0.0.1:9/ws")),
                    forkmesh::mainnode::kDefaultRoomName, QString(),
                    QStringLiteral("test-room-passphrase"));
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

// Public #channels admit controller integrations while private channels keep
// the automatic recipient seal, all on the same mainnode connection.
void testPublicIntegrationsAndPrivateSeal()
{
    WebSocketHarness harness;
    check(harness.listen(), "sealed-room harness listens");
    if (!harness.server.isListening())
        return;

    const QString room = forkmesh::mainnode::kDefaultRoomName;
    const QString roomPassphrase = QStringLiteral("test-room-passphrase");

    ServerNode node(QStringLiteral("Alice"), QStringLiteral("alice-laptop"),
                    QStringLiteral("alice"), QStringLiteral("stable-node-id"),
                    harness.url(), room, QString(), roomPassphrase);
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
    QStringList mismatchKeyIds;
    QObject::connect(&node, &ServerNode::roomSealMismatch, &node,
                     [&mismatchKeyIds](const QString &keyId) {
                         mismatchKeyIds.append(keyId);
                     });
    node.setTransportLimitsForTests(60, 20, 1 << 20);
    check(node.start(), "a policy-aware node starts against the test endpoint");
    check(waitUntil([&] { return connected(node); }, 3000),
          "a sealed node reaches the relay");

    check(!node.roomSealKeyId().isEmpty(),
          "the node creates its device encryption identity automatically");

    // Learn the node's public key exactly as another room device does. The
    // relay can decrypt this announcement, but it contains no private key.
    RoomCrypto relayKey(room, roomPassphrase);
    check(waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "the node announces an E2EE public key on connect");
    QJsonObject nodeKeyFrame;
    for (const QByteArray &raw : harness.textFrames) {
        const QJsonObject outer = QJsonDocument::fromJson(raw).object();
        const QJsonObject inner = relayKey.decryptObject(outer);
        if (RoomSeal::isKeyFrame(inner)) {
            nodeKeyFrame = inner;
            break;
        }
    }
    check(RoomSeal::isKeyFrame(nodeKeyFrame) &&
              nodeKeyFrame.value("kid").toString() == node.roomSealKeyId(),
          "the announcement identifies only the node's public key");

    const QByteArray peerPrivate = QByteArray::fromBase64(
        QByteArrayLiteral("cJMcO0LKgE1B6DRDn8z2mCcgt-cKyuWEtafYL2hFjHk"),
        QByteArray::Base64UrlEncoding);
    RoomSeal peerSeal(room, peerPrivate);
    check(peerSeal.acceptKeyFrame(nodeKeyFrame),
          "a peer learns the node public key without a shared phrase");
    const int framesBeforePeer = harness.textFrames.size();
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.keyFrame()))
                         .toJson(QJsonDocument::Compact));
    check(waitUntil(
              [&] { return harness.textFrames.size() > framesBeforePeer; }, 2000),
          "the node answers a newly discovered peer with its public key");
    bool answeredPeer = false;
    for (int index = framesBeforePeer; index < harness.textFrames.size(); ++index) {
        const QJsonObject reply = relayKey.decryptObject(
            QJsonDocument::fromJson(harness.textFrames.at(index)).object());
        answeredPeer = answeredPeer ||
                       (RoomSeal::isKeyFrame(reply) &&
                        reply.value("kid").toString() == node.roomSealKeyId());
    }
    check(answeredPeer, "the discovery reply carries the same public key");

    // A public message is deliberately readable under the room key so MCP,
    // ForkBot, and webhooks can participate.
    harness.textFrames.clear();
    node.sendChat(QStringLiteral("#general"), QStringLiteral("for MCP"));
    check(waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "a public chat reaches the relay");
    if (harness.textFrames.isEmpty())
        return;
    const QJsonObject publicEnvelope =
        QJsonDocument::fromJson(harness.textFrames.constLast()).object();
    const QJsonObject publicFrame = relayKey.decryptObject(publicEnvelope);
    check(publicEnvelope.value("persist").toBool() &&
              publicFrame.value("type").toString() == QLatin1String("chat") &&
              publicFrame.value("channel").toString() == QLatin1String("#general") &&
              publicFrame.value("text").toString() == QLatin1String("for MCP"),
          "the controller can read a durable public-channel frame");

    // Team coordination is an internal desktop-to-Agent bridge, not a room on
    // the shared mesh. Its local row exists, but no ciphertext leaves the host.
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

    // The same connection seals a channel as soon as it is marked private.
    node.createPrivateChannel(QStringLiteral("#private"));
    harness.textFrames.clear();
    node.sendChat(QStringLiteral("#private"),
                  QStringLiteral("beyond the relay"));
    check(waitUntil([&] { return !harness.textFrames.isEmpty(); }, 2000),
          "a private chat still reaches the relay");
    if (harness.textFrames.isEmpty())
        return;
    const QJsonObject privateEnvelope =
        QJsonDocument::fromJson(harness.textFrames.constLast()).object();
    const QJsonObject relayView = relayKey.decryptObject(privateEnvelope);
    check(privateEnvelope.value("persist").toBool() &&
              RoomSeal::isSealedFrame(relayView),
          "the controller's room key opens only a private recipient envelope");
    check(!QJsonDocument(relayView).toJson(QJsonDocument::Compact)
               .contains("beyond the relay") &&
              peerSeal.unsealObject(relayView).value("text").toString() ==
                  QLatin1String("beyond the relay"),
          "a discovered peer, but not the controller, reads private chat");

    // Controller-authored public chat is accepted. The identical attempt in a
    // private channel is refused, while a recipient-sealed peer frame arrives.
    messages.clear();
    QJsonObject publicBot{{"type", QStringLiteral("chat")},
                          {"id", QStringLiteral("public-bot-1")},
                          {"senderId", QStringLiteral("forkbot")},
                          {"sender", QStringLiteral("forkbot")},
                          {"channel", QStringLiteral("#general")},
                          {"text", QStringLiteral("public integration")},
                          {"ts", double(QDateTime::currentMSecsSinceEpoch())}};
    harness.sendText(
        QJsonDocument(relayKey.encryptObject(publicBot))
            .toJson(QJsonDocument::Compact));
    QJsonObject refusedPrivate = publicBot;
    refusedPrivate["id"] = QStringLiteral("private-controller-1");
    refusedPrivate["channel"] = QStringLiteral("#private");
    refusedPrivate["private"] = true;
    refusedPrivate["text"] = QStringLiteral("operator plaintext");
    harness.sendText(
        QJsonDocument(relayKey.encryptObject(refusedPrivate))
            .toJson(QJsonDocument::Compact));

    QJsonObject refusedRemoteTeam = refusedPrivate;
    refusedRemoteTeam["id"] = QStringLiteral("remote-team-1");
    refusedRemoteTeam["channel"] = QStringLiteral("#team");
    refusedRemoteTeam["text"] = QStringLiteral("remote team injection");
    harness.sendText(
        QJsonDocument(relayKey.encryptObject(
                          peerSeal.sealObject(refusedRemoteTeam)))
            .toJson(QJsonDocument::Compact));

    QJsonObject peer{{"type", QStringLiteral("chat")},
                     {"id", QStringLiteral("private-peer-1")},
                     {"senderId", QStringLiteral("peer-node")},
                     {"sender", QStringLiteral("Peer")},
                     {"channel", QStringLiteral("#private")},
                     {"private", true},
                     {"text", QStringLiteral("read me")},
                     {"ts", double(QDateTime::currentMSecsSinceEpoch())}};
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.sealObject(peer)))
                         .toJson(QJsonDocument::Compact));
    check(waitUntil([&] { return messages.size() >= 2; }, 2000),
          "public integration and sealed private peer frames reach the reader");
    QStringList receivedTexts;
    for (const ChatMessage &message : std::as_const(messages))
        receivedTexts.append(message.text);
    check(receivedTexts.contains(QStringLiteral("public integration")) &&
              receivedTexts.contains(QStringLiteral("read me")) &&
              !receivedTexts.contains(QStringLiteral("operator plaintext")) &&
              !receivedTexts.contains(QStringLiteral("remote team injection")),
          "public controller traffic and invited private peers cannot enter Team");

    // The controller cannot use its public-channel authority to mutate a
    // private message by copying (or guessing) that message's opaque id.
    QJsonObject crossBoundaryEdit{
        {"type", QStringLiteral("edit")},
        {"id", QStringLiteral("public-cross-edit")},
        {"senderId", QStringLiteral("peer-node")},
        {"sender", QStringLiteral("Peer")},
        {"conversation", QStringLiteral("#general")},
        {"target", QStringLiteral("private-peer-1")},
        {"text", QStringLiteral("controller rewrite")},
        {"ts", double(QDateTime::currentMSecsSinceEpoch())}};
    harness.sendText(
        QJsonDocument(relayKey.encryptObject(crossBoundaryEdit))
            .toJson(QJsonDocument::Compact));
    QJsonObject publicSentinel = publicBot;
    publicSentinel["id"] = QStringLiteral("public-sentinel");
    publicSentinel["text"] = QStringLiteral("after rejected mutation");
    harness.sendText(
        QJsonDocument(relayKey.encryptObject(publicSentinel))
            .toJson(QJsonDocument::Compact));
    check(waitUntil([&] { return messages.size() >= 3; }, 2000) &&
              editedTargets.isEmpty(),
          "public controller authority cannot cross into a private target");

    // A frame not addressed to this installation is reported once by sender
    // key id; this is how pre-discovery history looks on a new device.
    RoomSeal otherSeal(room, QByteArray(32, '\x03'));
    QJsonObject stranger = peer;
    stranger["id"] = QStringLiteral("stranger-1");
    stranger["text"] = QStringLiteral("not for you");
    harness.sendText(
        QJsonDocument(relayKey.encryptObject(otherSeal.sealObject(stranger)))
            .toJson(QJsonDocument::Compact));
    check(waitUntil([&] { return mismatchKeyIds.size() >= 2; }, 2000),
          "an unaddressed sealed frame is reported rather than dropped in silence");
    // The empty id is the refused private controller frame; the other is
    // retained ciphertext that predates this device's discovery.
    check(mismatchKeyIds == QStringList{QString(), otherSeal.keyId()} &&
              messages.size() == 3,
          "both ways a private channel falls out of step are reported by key id");

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
                    QStringLiteral("alice"), QStringLiteral("recipients-node"),
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
    RoomSeal peerSeal(room, QByteArray(32, '\x07'));
    check(peerSeal.acceptKeyFrame(nodeKeyFrame),
          "the peer learns the recipient-count node's key");
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.keyFrame()))
                         .toJson(QJsonDocument::Compact));

    const auto sendPeerChat = [&](const QString &id, const QJsonValue &outer,
                                  const QJsonValue &inner) {
        QJsonObject frame{{"type", "chat"},
                          {"id", id},
                          {"senderId", QStringLiteral("bob-node")},
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
    const QByteArray firstFrame = harness.textFrames.constLast();
    const QString firstRef = QString::fromLatin1(
        QCryptographicHash::hash(firstFrame, QCryptographicHash::Sha256)
            .toHex().left(40));
    check(inner(firstFrame).value("keys").toArray().size() == 1,
          "with no peer key the DM is sealed to this device alone");

    RoomSeal stranger(room, QByteArray(32, '\x05'));
    QJsonObject unreadable = relayKey.encryptObject(stranger.sealObject(
        QJsonObject{{"type", "chat"}, {"id", "not-for-us"},
                    {"senderId", "bob-node"}, {"sender", "bob"},
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

    RoomSeal bob(room, QByteArray(32, '\x07'));
    bob.acceptKeyFrame(nodeKey);
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
    const QJsonObject reopened =
        bob.unsealObject(relayKey.decryptObject(resent));
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
    RoomSeal bobPhone(room, QByteArray(32, '\x09'));
    harness.textFrames.clear();
    harness.sendText(QJsonDocument(relayKey.encryptObject(bobPhone.keyFrame()))
                         .toJson(QJsonDocument::Compact));
    waitUntil([] { return false; }, 300);
    bool resentAgain = false;
    for (const QByteArray &raw : std::as_const(harness.textFrames))
        resentAgain = resentAgain || outer(raw).value("resend").toBool();
    check(!resentAgain, "nothing is re-sealed once the relay holds no copies");
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
                    QStringLiteral("alice"), QStringLiteral("status-frame-node"),
                    harness.url(), room, QString(), roomPassphrase);
    QList<MemberInfo> roster;
    QObject::connect(&node, &ServerNode::rosterChanged, &node,
                     [&roster](const QList<MemberInfo> &members) {
                         roster = members;
                     });
    node.setTransportLimitsForTests(60, 20, 1 << 20);
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
    RoomSeal peerSeal(room, QByteArray(32, '\x05'));
    check(peerSeal.acceptKeyFrame(nodeKeyFrame),
          "the peer learns the node's public key");
    harness.sendText(QJsonDocument(relayKey.encryptObject(peerSeal.keyFrame()))
                         .toJson(QJsonDocument::Compact));

    const auto sendPeerFrame = [&](const QString &id, const QString &type,
                                   qint64 ts, const QJsonValue &status) {
        QJsonObject frame{{"type", type},
                          {"id", id},
                          {"senderId", QStringLiteral("bob-node")},
                          {"sender", QStringLiteral("bob")},
                          {"ownerUser", QStringLiteral("bob")},
                          {"ts", double(ts)}};
        if (!status.isUndefined())
            frame.insert(QStringLiteral("userStatus"), status);
        harness.sendText(
            QJsonDocument(relayKey.encryptObject(peerSeal.sealObject(frame)))
                .toJson(QJsonDocument::Compact));
    };
    const auto bob = [&roster]() -> MemberInfo {
        for (const MemberInfo &member : roster)
            if (member.id == QLatin1String("bob-node"))
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
                      const QJsonObject inner = peerSeal.unsealObject(
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
                      const QJsonObject inner = peerSeal.unsealObject(
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
    auto history = [](const QString &channelId, const QString &messageId) {
        const QString room = QStringLiteral("chat-channel:") + channelId +
                             QStringLiteral(":v1");
        const RoomCrypto crypto(room, QStringLiteral("room-passphrase"));
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
            {"notice", true},
            {"recipients", 42}};
        QJsonObject envelope = crypto.encryptObject(notice);
        envelope.insert(QStringLiteral("persist"), true);
        envelope.insert(QStringLiteral("recipients"), 5);
        const QJsonObject payload{
            {"room", room},
            {"passphrase", "room-passphrase"},
            {"messages",
             QJsonArray{QJsonObject{
                 {"ts", 1790600000000.0},
                 {"body", QString::fromUtf8(
                              QJsonDocument(envelope)
                                  .toJson(QJsonDocument::Compact))}}}}};
        return QJsonDocument(payload).toJson(QJsonDocument::Compact);
    };
    relay.responses.insert(QStringLiteral("/api/chat/channels/") + noticesId +
                               QStringLiteral("/history"),
                           history(noticesId, QStringLiteral("notice-1")));
    relay.responses.insert(QStringLiteral("/api/chat/channels/") + privateId +
                               QStringLiteral("/history"),
                           history(privateId, QStringLiteral("unsealed-1")));

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
    check(waitUntil([&] { return !arrived.isEmpty(); }, 4000) &&
              arrived.first().conversation == notices &&
              arrived.first().senderName == QStringLiteral("Mail log (ForkMesh)") &&
              arrived.first().text.contains(QStringLiteral("accepted")),
          "a server-written email-log note is shown in the notices channel");
    check(!arrived.isEmpty() && arrived.first().recipientCount == 5,
          "history reads the recipient count from the outer envelope only");
    check(waitUntil([&] { return relay.lastCall(
                              "GET", QStringLiteral("/api/chat/channels/") +
                                         privateId +
                                         QStringLiteral("/history")); },
                    4000),
          "the private channel's history is read too");
    waitUntil([] { return false; }, 500);
    bool unsealedShown = false;
    for (const ChatMessage &message : std::as_const(arrived))
        unsealedShown = unsealedShown || message.conversation == design;
    check(!unsealedShown,
          "an ordinary private channel still refuses an unsealed frame");
    mirror.stop();
}

void testDirectMessagesOverLocalMesh()
{
    using namespace forkmesh::localmesh;
    auto quiet = [](LocalMeshNode &node, char key) {
        node.setMulticastEnabledForTests(false);
        node.setBluetoothEnabledForTests(false);
        node.setAddressSweepEnabledForTests(false);
        node.setSealKeyForTests(QByteArray(32, key));
    };
    LocalMeshNode adaMesh(QStringLiteral("ada"), QStringLiteral("workshop"),
                          QStringLiteral("ada"), QStringLiteral("mesh-dm-ada"));
    LocalMeshNode bobMesh(QStringLiteral("bob"), QStringLiteral("studio"),
                          QStringLiteral("bob"), QStringLiteral("mesh-dm-bob"));
    LocalMeshNode eveMesh(QStringLiteral("eve"), QStringLiteral("cafe"),
                          QStringLiteral("eve"), QStringLiteral("mesh-dm-eve"));
    quiet(adaMesh, 1);
    quiet(bobMesh, 2);
    quiet(eveMesh, 3);
    check(adaMesh.start() && bobMesh.start() && eveMesh.start(),
          "three devices open the mesh room");
    const QString adaKey = adaMesh.sealKeyId();
    const QString bobKey = bobMesh.sealKeyId();
    const QString eveKey = eveMesh.sealKeyId();
    check(!adaKey.isEmpty() && adaKey != bobKey && bobKey != eveKey,
          "each device has its own sealing key");
    adaMesh.connectToPeer(QHostAddress::LocalHost, bobMesh.listenPort());
    eveMesh.connectToPeer(QHostAddress::LocalHost, adaMesh.listenPort());
    eveMesh.connectToPeer(QHostAddress::LocalHost, bobMesh.listenPort());
    check(waitUntil([&] { return adaMesh.peerCount() == 2 &&
                                 bobMesh.peerCount() == 2 &&
                                 eveMesh.peerCount() == 2; },
                    4000),
          "the three devices are linked");
    waitUntil([] { return false; }, 300);

    QList<ChatMessage> roomSaw;
    for (LocalMeshNode *node : {&adaMesh, &bobMesh, &eveMesh})
        QObject::connect(node, &LocalMeshNode::messageArrived, node,
                         [&roomSaw](const ChatMessage &m) { roomSaw.append(m); });
    QList<QJsonObject> eveSaw;
    QObject::connect(&eveMesh, &LocalMeshNode::directMessageArrived, &eveMesh,
                     [&eveSaw](const QString &, const QString &,
                               const QJsonObject &plain) { eveSaw.append(plain); });
    QList<QPair<QString, QJsonObject>> bobSaw;
    QObject::connect(&bobMesh, &LocalMeshNode::directMessageArrived, &bobMesh,
                     [&bobSaw](const QString &, const QString &keyId,
                               const QJsonObject &plain) {
                         bobSaw.append({keyId, plain});
                     });

    const QString conversationId(32, 'f');
    const QString channel = QStringLiteral("direct:") + conversationId;
    check(adaMesh.sendDirect(channel, QStringLiteral("dm-probe"),
                             QStringLiteral("only bob"), {bobKey}) == 1,
          "a direct message goes to the one linked device holding a room key");
    check(waitUntil([&] { return !bobSaw.isEmpty(); }, 4000) &&
              bobSaw.first().first == adaKey &&
              bobSaw.first().second.value(QStringLiteral("channel")).toString() ==
                  channel &&
              bobSaw.first().second.value(QStringLiteral("text")).toString() ==
                  QStringLiteral("only bob"),
          "the recipient opens it and learns which device key sealed it");
    waitUntil([] { return false; }, 300);
    check(eveSaw.isEmpty() && roomSaw.isEmpty(),
          "nobody else on the mesh sees it, and it never lands in #mesh");
    check(adaMesh.sendDirect(channel, QStringLiteral("dm-nobody"),
                             QStringLiteral("hello?"),
                             {QStringLiteral("not-a-linked-key")}) == 0,
          "a direct message with no linked recipient device is not sent");

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
    auto remember = [&conversationId](const QUrl &apiBase, const QString &account,
                                      const QString &other, const QString &key) {
        const QJsonObject root{
            {QStringLiteral("v"), 1},
            {QStringLiteral("apiBase"), apiBase.toString()},
            {QStringLiteral("account"), account},
            {QStringLiteral("conversations"),
             QJsonArray{QJsonObject{{QStringLiteral("id"), conversationId},
                                    {QStringLiteral("otherUser"), other},
                                    {QStringLiteral("keys"), QJsonArray{key}}}}}};
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

    QNetworkAccessManager network;
    OfficeChannelMirror adaMirror(&network);
    OfficeChannelMirror bobMirror(&network);
    prepare(adaMirror, adaRelay.apiBase(), QStringLiteral("ada"));
    prepare(bobMirror, bobRelay.apiBase(), QStringLiteral("bob"));
    adaMirror.setMeshDirectSender(
        [&adaMesh](const QString &wire, const QString &id, const QString &text,
                   const QStringList &keys) {
            return adaMesh.sendDirect(wire, id, text, keys);
        });
    QObject::connect(&bobMesh, &LocalMeshNode::directMessageArrived, &bobMirror,
                     [&bobMirror](const QString &wire, const QString &keyId,
                                  const QJsonObject &plain) {
                         bobMirror.acceptMeshDirect(wire, keyId, plain);
                     });
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

    remember(adaRelay.apiBase(), QStringLiteral("ada"), QStringLiteral("bob"),
             bobKey);
    adaMirror.start();
    remember(bobRelay.apiBase(), QStringLiteral("bob"), QStringLiteral("ada"),
             adaKey);
    bobMirror.start();
    const QString toBob = QStringLiteral("#office/@bob");
    const QString toAda = QStringLiteral("#office/@ada");
    check(adaMirror.directConversations() == QStringList{toBob} &&
              adaMirror.directDeviceKeys(toBob) == QStringList{bobKey} &&
              bobMirror.directConversations() == QStringList{toAda},
          "direct messages and their device keys are listed with no relay reply");

    check(adaMirror.sendMessage(toBob, QStringLiteral("the router is down")),
          "a direct message is accepted while the relay cannot be reached");
    check(waitUntil([&] { return !bobShown.isEmpty(); }, 4000) &&
              bobShown.first().conversation == toAda &&
              bobShown.first().text == QStringLiteral("the router is down") &&
              bobShown.first().senderName == QStringLiteral("ada") &&
              !bobShown.first().self,
          "the other person's desktop shows it in their direct conversation");
    const QString accessPath =
        QStringLiteral("/api/chat/direct-messages/%1/room-access")
            .arg(conversationId);
    check(waitUntil([&] { return adaRelay.lastCall("GET", accessPath); }, 4000),
          "the sender still asks the relay for the room");
    waitUntil([] { return false; }, 300);
    check(adaFailed.isEmpty(),
          "a message the mesh delivered is not reported as failed when the "
          "relay is unreachable");

    QJsonObject replay = bobSaw.last().second;
    check(!bobMirror.acceptMeshDirect(channel, adaKey, replay),
          "the same message is shown once");
    replay.insert(QStringLiteral("id"), QStringLiteral("forged-1"));
    check(!bobMirror.acceptMeshDirect(channel, eveKey, replay),
          "a device key the direct room never saw cannot post into it");
    check(eveMesh.sendDirect(channel, QStringLiteral("forged-2"),
                             QStringLiteral("it is ada, honest"), {bobKey}) == 1,
          "another device on the mesh can address bob's key");
    waitUntil([] { return false; }, 500);
    bool forgedShown = false;
    for (const ChatMessage &message : std::as_const(bobShown))
        forgedShown = forgedShown || message.text.contains(QStringLiteral("honest"));
    check(!forgedShown && bobShown.size() == 1,
          "but bob's desktop refuses it: eve holds no key of that conversation");

    adaMirror.stop();
    bobMirror.stop();
    adaMesh.stop();
    bobMesh.stop();
    eveMesh.stop();
}

int main(int argc, char **argv)
{
    QStandardPaths::setTestModeEnabled(true);
    QCoreApplication app(argc, argv);
    app.setOrganizationName(QStringLiteral("ForkMeshTests"));
    app.setApplicationName(QStringLiteral("chat-transport"));

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
        testLocalMeshQueuedUntilJoin();
        testLocalMeshWakesDevicesThatLeft();
        testLocalMeshReturningDeviceReplacesStaleLink();
        testLocalMeshDiscovery();
        testLocalMeshUnicastDiscovery();
        testLocalMeshFirewall();
        if (failures == 0)
            std::puts("All local mesh tests passed.");
        return failures == 0 ? 0 : 1;
    }
    testTeammateChatRelay();
    if (!discordOnly) {
        testSavedRoomMigration();
        testPublicIntegrationsAndPrivateSeal();
        testDeterministicRecovery();
        testStatusIsEventDriven();
        testTeardownMidBufferLeavesSocketAlone();
        testNestedPumpInFrameHandlerKeepsDeliveringSocket();
        testUserStatusOnPresenceFrames();
        testRecipientCounts();
        testDirectRoomResealsUndeliveredMessages();
        testOfficeChannelMirror();
        testOfficeChannelSettingsRequests();
        testOfficeNoticesChannelShowsServerFrames();
        testOfficePrivateRoomsAndDirectMessages();
        testDirectMessagesOverLocalMesh();
        testLocalMeshAddressPolicy();
        testLocalMeshCrossClientConstants();
        testLocalMeshFraming();
        testLocalMeshBeaconIsCiphertext();
        testLocalMeshRoom();
        testLocalMeshQueuedUntilJoin();
        testLocalMeshWakesDevicesThatLeft();
        testLocalMeshReturningDeviceReplacesStaleLink();
        testLocalMeshDiscovery();
        testLocalMeshUnicastDiscovery();
        testLocalMeshFirewall();
    }
    testDiscordClientContract();
    testChatFormat();
    if (failures == 0)
        std::puts("All chat transport tests passed.");
    return failures == 0 ? 0 : 1;
}
