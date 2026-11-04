#include "ReticulumFeature.h"
#include "ReticulumGraphics.h"
#include "ReticulumLxmf.h"
#include "ReticulumMessenger.h"
#include "ReticulumNode.h"
#include "ReticulumProtocol.h"

#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QListWidget>
#include <QTabWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTreeWidget>

#include <functional>
#include <memory>

using namespace forkmesh::reticulum;

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition)
        qInfo("PASS: %s", qPrintable(message));
    else {
        qCritical("FAIL: %s", qPrintable(message));
        ++failures;
    }
}

bool waitFor(const std::function<bool()> &condition, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs)
            return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

QByteArray hex(const char *text)
{
    return QByteArray::fromHex(QByteArray(text));
}

const char *kPrivateKey =
    "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f202122232425262728292a2b2c2d2e2f303132333435363738393a3b3c3d3e3f";
const char *kPublicKey =
    "8f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd7";
const char *kAnnounce =
    "0100fae321c442e3c9bdcd7a3e79d850e03c008f40c5adb68f25624ae5b214ea767a6ec94d829d3d7b5e1ad1ba6f3e2138285f29acbae141bccaf0b22e1a94d34d0bc7361e526d0bfe12c89794bc9322966dd76ec60bc318e2c0f0d9080102030405006955b900b5fd2a0dce06f324f4ba40cef2dd50b2fde097047ac7a137b4feb5fe8277af2ddeb6608cb297548ddc310df93bd21fa3fc8524730d84d220e455e337fc0cbd0492c40d4669656c6420726164696f203308";
const char *kDiscovery =
    "008b00b14261636b626f6e65496e7465726661636501c3ccfec410aca31af0441d81dbec71e82da0b4b5f5ccfda3524e53ccfca5312e352e37ccffa8546573742068756203cb404a2f5c28f5c28f04cb40138f5c28f5c28f05cb402400000000000002af6875622e6578616d706c652e6f726706cd1092d83b79d7a76c6bf37b3a640a739f67f92b7c3cf9efac45e303030a5cfaf7b0cc";

void testFraming()
{
    const QByteArray payload = QByteArray("\x01\x7e\x7d\x02\xc0\xdb", 6) + QByteArray(30, 'x');
    const QByteArray framed = hdlcFrame(payload);
    check(!framed.mid(1, framed.size() - 2).contains('\x7e'), QStringLiteral("HDLC escapes flag bytes inside a frame"));
    FrameDecoder decoder;
    QList<QByteArray> frames;
    for (const char byte : framed + hdlcKeepalive() + framed)
        frames += decoder.feed(QByteArray(1, byte));
    check(frames.size() == 2 && frames.at(0) == payload && frames.at(1) == payload,
          QStringLiteral("HDLC decoder rebuilds frames fed one byte at a time and skips keepalives"));

    FrameDecoder small(FrameDecoder::Hdlc, 40);
    const QList<QByteArray> dropped = small.feed(hdlcFrame(QByteArray(100, 'a')) + hdlcFrame(QByteArray(25, 'b')));
    check(dropped.size() == 1 && dropped.first() == QByteArray(25, 'b') && small.invalidFrames() == 1,
          QStringLiteral("An oversized HDLC frame is dropped and the next one still decodes"));

    FrameDecoder kiss(FrameDecoder::Kiss);
    const QList<QByteArray> kissFrames = kiss.feed(kissFrame(payload) + kissFrame(payload));
    check(kissFrames.size() == 2 && kissFrames.first() == payload, QStringLiteral("KISS framing round trips"));
}

void testVectors()
{
    check(hexHash(nameHash(QStringLiteral("rnstransport.path.request"))) == QStringLiteral("7926bbe7dd7f9aba88b0"),
          QStringLiteral("Name hash matches Python RNS"));
    check(hexHash(pathRequestDestination()) == QStringLiteral("6b9f66014d9853faab220fba47d02761"),
          QStringLiteral("Path request destination matches Python RNS"));
    const std::optional<Identity> identity = Identity::fromPrivateKey(hex(kPrivateKey));
    check(identity && identity->publicKey() == hex(kPublicKey), QStringLiteral("Identity public key matches Python RNS"));
    check(identity && hexHash(identity->hash()) == QStringLiteral("aca31af0441d81dbec71e82da0b4b5f5"),
          QStringLiteral("Identity hash matches Python RNS"));
    check(identity && hexHash(destinationHashForName(QStringLiteral("lxmf.delivery"), identity->hash())) ==
                          QStringLiteral("fae321c442e3c9bdcd7a3e79d850e03c"),
          QStringLiteral("Destination hash matches Python RNS"));
    check(hexHash(hkdf(80, "abc", "salt", "ctx")) ==
              QStringLiteral("ffae909e84651ee55e1826c11f1a9e722af61742efc3d00b94d3067fce9e548268c612d3465a2ad08bac2eec009bb36d4fc86cb070ff64e60b45ef0fe2e76eab0cb34a87e3cabea1b0089d14dbc204de"),
          QStringLiteral("HKDF matches Python RNS"));

    const QByteArray raw = hex(kAnnounce);
    check(hexHash(packetHash(raw)) == QStringLiteral("498d563bd52778f53e58a33d05de475df0aa2042044fa6b087e5ce2691b9e2cb"),
          QStringLiteral("Packet hash matches Python RNS"));
    const std::optional<Packet> packet = parsePacket(raw);
    check(packet && packet->packetType == PacketAnnounce && packet->hops == 0, QStringLiteral("Announce header parses"));
    check(packet && packPacket(*packet) == raw, QStringLiteral("Packet packs back to the same bytes"));
    const std::optional<Announce> announce = packet ? parseAnnounce(*packet) : std::nullopt;
    QString error;
    check(announce && validateAnnounce(*announce, &error), QStringLiteral("Python-signed announce validates: %1").arg(error));
    check(announce && announce->emittedAt == 1767225600, QStringLiteral("Announce emission time is read from the random hash"));
    check(announce && displayNameFromAppData(QStringLiteral("peer"), announce->appData) == QStringLiteral("Field radio 3"),
          QStringLiteral("LXMF msgpack display name is decoded"));
    check(knownAspect(announce ? announce->nameHash : QByteArray()).has_value() &&
              knownAspect(announce->nameHash)->name == QStringLiteral("lxmf.delivery"),
          QStringLiteral("Aspect is recognised from its name hash"));

    QByteArray tampered = raw;
    tampered[tampered.size() - 3] = char(tampered.at(tampered.size() - 3) ^ 0x01);
    const std::optional<Packet> badPacket = parsePacket(tampered);
    const std::optional<Announce> bad = badPacket ? parseAnnounce(*badPacket) : std::nullopt;
    check(bad && !validateAnnounce(*bad), QStringLiteral("A tampered announce fails validation"));

    QByteArray tag;
    for (int i = 16; i < 32; ++i)
        tag.append(char(i));
    check(buildPathRequest(hex("fae321c442e3c9bdcd7a3e79d850e03c"), tag) ==
              hex("08006b9f66014d9853faab220fba47d0276100fae321c442e3c9bdcd7a3e79d850e03c101112131415161718191a1b1c1d1e1f"),
          QStringLiteral("Path request bytes match Python RNS"));

    const QByteArray built = buildAnnounce(*identity, QStringLiteral("lxmf.delivery"), "hello", false, 1767225600,
                                           QByteArray("\x01\x02\x03\x04\x05", 5));
    const std::optional<Packet> builtPacket = parsePacket(built);
    const std::optional<Announce> builtAnnounce = builtPacket ? parseAnnounce(*builtPacket) : std::nullopt;
    check(builtAnnounce && validateAnnounce(*builtAnnounce) &&
              builtPacket->destinationHash == hex("fae321c442e3c9bdcd7a3e79d850e03c"),
          QStringLiteral("Announces built here validate and use the RNS destination hash"));

    const Identity fresh = Identity::generate();
    check(fresh.hasPrivateKey() && Identity::fromPrivateKey(fresh.privateKey())->publicKey() == fresh.publicKey(),
          QStringLiteral("A generated identity reloads from its private key"));

    const std::optional<DiscoveredInterface> discovered = parseDiscoveryAppData(hex(kDiscovery));
    check(discovered && discovered->name == QStringLiteral("Test hub") &&
              discovered->type == QStringLiteral("BackboneInterface") &&
              discovered->reachableOn == QStringLiteral("hub.example.org") && discovered->port == 4242 &&
              discovered->transport && discovered->hasLocation,
          QStringLiteral("Interface discovery data is decoded"));
    check(discovered && discovered->stampValue == 11 && !discovered->stampValid,
          QStringLiteral("Discovery proof of work matches LXMF's stamp value (11) and is below the required 16"));

    check(!parsePacket(QByteArray(10, '\0')).has_value(), QStringLiteral("Short frames are refused"));
    QByteArray hops = raw;
    hops[1] = char(200);
    check(!parsePacket(hops).has_value(), QStringLiteral("Hop counts over 127 are refused"));
    QByteArray ifac = raw;
    ifac[0] = char(quint8(ifac.at(0)) | 0x80);
    check(!parsePacket(ifac).has_value(), QStringLiteral("Packets with an access code are refused"));
    bool ok = true;
    decodeMsgPack(QByteArray("\xdd\xff\xff\xff\xff", 5), &ok);
    check(!ok, QStringLiteral("A msgpack array claiming four billion items is refused"));
}

void testConfigImport()
{
    const QString text = QStringLiteral(
        "[reticulum]\n  share_instance = Yes\n  instance_name = lab\n\n[interfaces]\n"
        "  [[Default Interface]]\n    type = AutoInterface\n    enabled = yes\n"
        "  [[RNS Testnet Dublin]]\n    type = TCPClientInterface\n    enabled = yes\n"
        "    target_host = dublin.connect.reticulum.network\n    target_port = 4965\n"
        "  [[Private]]\n    type = TCPClientInterface\n    enabled = yes\n    target_host = 10.0.0.2\n"
        "    target_port = 4242\n    network_name = secret\n"
        "  [[LAN UDP]]\n    type = UDPInterface\n    interface_enabled = no\n    port = 4243 # both\n");
    QStringList skipped;
    const QList<InterfaceConfig> configs = parseReticulumConfig(text, &skipped);
    check(configs.size() == 4, QStringLiteral("Config import finds shared instance, two TCP and UDP (%1)").arg(configs.size()));
    if (configs.size() == 4) {
        check(configs.at(0).kind == InterfaceKind::SharedInstance && configs.at(0).instanceName == QStringLiteral("lab"),
              QStringLiteral("Shared instance name comes from [reticulum]"));
        check(configs.at(1).host == QStringLiteral("dublin.connect.reticulum.network") && configs.at(1).port == 4965 &&
                  configs.at(1).enabled,
              QStringLiteral("TCP client entry is imported"));
        check(configs.at(2).networkName == QStringLiteral("secret") && configs.at(2).usesAccessCode(),
              QStringLiteral("An entry with a network name keeps it for IFAC"));
        check(configs.at(3).kind == InterfaceKind::Udp && configs.at(3).listenPort == 4243 && !configs.at(3).enabled,
              QStringLiteral("UDP entry keeps its port and disabled state"));
    }
    check(skipped.size() == 1, QStringLiteral("AutoInterface is reported as skipped"));
    InterfaceConfig bad;
    bad.name = QStringLiteral("x");
    bad.host = QStringLiteral("bad host!");
    check(!bad.validationError().isEmpty(), QStringLiteral("An invalid host is refused"));
    const std::optional<InterfaceConfig> round = InterfaceConfig::fromJson(configs.value(1).toJson());
    check(round && round->sameTarget(configs.value(1)), QStringLiteral("Interface settings round trip through JSON"));
}

struct FakeNode {
    QTcpServer server;
    QPointer<QTcpSocket> client;
    FrameDecoder decoder;
    QList<QByteArray> received;
    int connections = 0;

    FakeNode()
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            client = server.nextPendingConnection();
            ++connections;
            QObject::connect(client, &QTcpSocket::readyRead, client, [this] { received += decoder.feed(client->readAll()); });
        });
    }
    void send(const QByteArray &raw)
    {
        if (client)
            client->write(hdlcFrame(raw));
    }
    std::optional<Packet> find(int type, quint8 context)
    {
        for (const QByteArray &frame : received) {
            const std::optional<Packet> packet = parsePacket(frame);
            if (packet && packet->packetType == type && packet->context == context)
                return packet;
        }
        return std::nullopt;
    }
};

void testLiveSession()
{
    FakeNode fake;
    Node node;
    node.setIdentity(Identity::generate());
    node.setAnnounceName(QStringLiteral("Test desktop"));
    QList<QUrl> asked;
    node.setAuthorizer([&asked](const QString &, const QUrl &url) {
        asked.append(url);
        return true;
    });
    InterfaceConfig config;
    config.id = QStringLiteral("fake");
    config.name = QStringLiteral("Fake node");
    config.host = QStringLiteral("127.0.0.1");
    config.port = fake.server.serverPort();
    node.setInterfaces({config});
    node.start();
    check(waitFor([&] { return node.onlineCount() == 1 && fake.client; }), QStringLiteral("TCP interface comes online"));
    check(asked.size() == 1 && asked.first().scheme() == QStringLiteral("tcp") && asked.first().port() == config.port,
          QStringLiteral("The firewall is asked once for tcp://host:port"));

    fake.send(hex(kAnnounce));
    const QByteArray lxmf = hex("fae321c442e3c9bdcd7a3e79d850e03c");
    check(waitFor([&] { return node.destinations().contains(lxmf); }), QStringLiteral("Announce from the network is recorded"));
    const DestinationRecord record = node.destinations().value(lxmf);
    check(record.displayName == QStringLiteral("Field radio 3") && record.hops == 1 && record.kind == QStringLiteral("peer") &&
              record.interfaceId == QStringLiteral("fake"),
          QStringLiteral("Recorded destination has name, kind, one hop and its interface"));
    fake.send(hex(kAnnounce));
    check(waitFor([&] { return node.counters().duplicates == 1; }), QStringLiteral("A repeated announce counts as a duplicate"));
    check(node.status(QStringLiteral("fake")).rxBytes > 0 && node.status(QStringLiteral("fake")).announces == 2,
          QStringLiteral("Interface counts bytes and announces"));

    check(node.announce(), QStringLiteral("Announcing this desktop sends a packet"));
    check(waitFor([&] { return fake.find(PacketAnnounce, ContextNone).has_value(); }), QStringLiteral("Fake node receives our announce"));
    if (const std::optional<Packet> ours = fake.find(PacketAnnounce, ContextNone)) {
        const std::optional<Announce> parsed = parseAnnounce(*ours);
        check(parsed && validateAnnounce(*parsed) && parsed->destinationHash == node.destinationHash() &&
                  parsed->appData == QByteArray("Test desktop") &&
                  parsed->nameHash == nameHash(QStringLiteral("forkmesh.node")),
              QStringLiteral("Our announce validates and carries the name"));
    }

    const Identity remote = Identity::generate();
    const QByteArray remoteHash = destinationHashForName(QStringLiteral("nomadnetwork.node"), remote.hash());
    check(!node.requestPath(remoteHash).isEmpty(), QStringLiteral("Path request is sent"));
    check(waitFor([&] { return fake.find(PacketData, ContextNone).has_value(); }), QStringLiteral("Fake node receives the path request"));
    QByteArray requested;
    QByteArray tag;
    if (const std::optional<Packet> request = fake.find(PacketData, ContextNone))
        check(parsePathRequest(*request, &requested, &tag) && requested == remoteHash && tag.size() == 16,
              QStringLiteral("Path request names the destination and carries a tag"));
    QByteArray response = buildAnnounce(remote, QStringLiteral("nomadnetwork.node"), "Hill node", true,
                                        QDateTime::currentSecsSinceEpoch());
    response[1] = char(2);
    fake.send(response);
    check(waitFor([&] { return node.lookups().value(remoteHash).answered.isValid(); }), QStringLiteral("Path response answers the lookup"));
    check(node.lookups().value(remoteHash).hops == 3 && node.destinations().value(remoteHash).displayName == QStringLiteral("Hill node"),
          QStringLiteral("Lookup records three hops and the node name"));

    fake.received.clear();
    fake.send(buildPathRequest(node.destinationHash(), QByteArray(16, 'q')));
    check(waitFor([&] { return fake.find(PacketAnnounce, ContextPathResponse).has_value(); }),
          QStringLiteral("A path request for this desktop is answered with a path response"));

    fake.client->disconnectFromHost();
    check(waitFor([&] { return node.status(QStringLiteral("fake")).state == LinkState::Waiting; }),
          QStringLiteral("A dropped connection waits to retry"));
    node.stop();
    check(node.status(QStringLiteral("fake")).state == LinkState::Disabled, QStringLiteral("Stopping the node closes every interface"));
}

void testFirewallDenied()
{
    FakeNode fake;
    Node node;
    node.setAuthorizer([](const QString &, const QUrl &) { return false; });
    InterfaceConfig config;
    config.id = QStringLiteral("denied");
    config.name = QStringLiteral("Denied");
    config.host = QStringLiteral("127.0.0.1");
    config.port = fake.server.serverPort();
    node.setInterfaces({config});
    node.start();
    waitFor([] { return false; }, 300);
    check(node.status(QStringLiteral("denied")).state == LinkState::Blocked && fake.connections == 0,
          QStringLiteral("A connection the firewall denies is never opened"));
}

void settle()
{
    waitFor([] { return false; }, 800);
}

QByteArray syntheticAnnounce(int index, QString *aspectOut = nullptr)
{
    static QList<Identity> identities;
    while (identities.size() <= index)
        identities.append(Identity::generate());
    const QStringList aspects = {QStringLiteral("lxmf.delivery"), QStringLiteral("lxmf.delivery"), QStringLiteral("lxmf.delivery"),
                                 QStringLiteral("nomadnetwork.node"), QStringLiteral("lxmf.propagation"),
                                 QStringLiteral("rnstransport.remote.management"), QStringLiteral("example.unknown")};
    const QString aspect = aspects.at(index % aspects.size());
    if (aspectOut)
        *aspectOut = aspect;
    QByteArray appData;
    if (aspect == QStringLiteral("lxmf.delivery")) {
        const QByteArray name = QStringLiteral("Peer %1").arg(index).toUtf8();
        appData = QByteArray("\x92\xc4", 2) + char(name.size()) + name + QByteArray(1, '\x08');
    } else if (aspect == QStringLiteral("nomadnetwork.node")) {
        appData = QStringLiteral("Node %1").arg(index).toUtf8();
    }
    QByteArray raw = buildAnnounce(identities.at(index), aspect, appData, false, QDateTime::currentSecsSinceEpoch());
    raw[1] = char((index * 7) % 6);
    return raw;
}

struct Hub {
    QTcpServer server;
    QList<QPointer<QTcpSocket>> clients;
    Hub()
    {
        server.listen(QHostAddress::LocalHost);
        QObject::connect(&server, &QTcpServer::newConnection, &server, [this] {
            QTcpSocket *socket = server.nextPendingConnection();
            clients.append(socket);
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket] {
                const QByteArray bytes = socket->readAll();
                for (const QPointer<QTcpSocket> &other : std::as_const(clients)) {
                    if (other && other != socket)
                        other->write(bytes);
                }
            });
        });
    }
};

struct Peer {
    Node node;
    Messenger messenger{&node};
    Peer(const QString &name, quint16 port, const QString &networkName = {}, const QString &passphrase = {})
    {
        node.setIdentity(Identity::generate());
        messenger.setDisplayName(name);
        node.setAnnounceHandler([this](const QByteArray &hash) { messenger.destinationAnnounced(hash); });
        node.setPacketHandler([this](const QString &id, const Packet &packet, const QByteArray &raw, int hops) {
            messenger.handlePacket(id, packet, raw, hops);
        });
        InterfaceConfig config;
        config.id = QStringLiteral("hub");
        config.name = QStringLiteral("Hub");
        config.host = QStringLiteral("127.0.0.1");
        config.port = port;
        config.networkName = networkName;
        config.passphrase = passphrase;
        node.setInterfaces({config});
        node.start();
    }
};

void testFeature(const QString &screenshotDir)
{
    QHash<QString, QVariant> settings;
    settings.insert(QStringLiteral("interfaces"), QStringLiteral("[]"));
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("reticulum");
    host.setting = [&settings](const QString &key, const QVariant &fallback) { return settings.value(key, fallback); };
    host.setSetting = [&settings](const QString &key, const QVariant &value) { settings.insert(key, value); };
    host.authorizeConnection = [](const QString &, const QUrl &) { return true; };

    Hub hub;
    auto page = std::make_unique<ReticulumFeature>(host, nullptr);
    page->resize(1280, 820);
    page->show();
    check(settings.contains(QStringLiteral("identity")) &&
              QByteArray::fromHex(settings.value(QStringLiteral("identity")).toString().toLatin1()).size() == kPrivateKeyLength,
          QStringLiteral("A new identity is stored on first open"));
    page->refresh();
    InterfaceConfig a;
    a.id = QStringLiteral("a");
    a.name = QStringLiteral("RMAP Backbone 1");
    a.host = QStringLiteral("127.0.0.1");
    a.port = hub.server.serverPort();
    InterfaceConfig b = defaultInterfaces().first();
    b.id = QStringLiteral("b");
    b.enabled = false;
    page->addInterface(a);
    page->addInterface(b);
    check(QJsonDocument::fromJson(settings.value(QStringLiteral("interfaces")).toString().toUtf8()).array().size() == 2,
          QStringLiteral("Added interfaces are saved"));
    check(waitFor([&] { return page->node()->onlineCount() == 1; }), QStringLiteral("The page connects its interface"));
    for (int i = 0; i < 160; ++i)
        page->node()->ingest(i % 3 == 0 ? QStringLiteral("b") : QStringLiteral("a"), syntheticAnnounce(i));
    page->node()->ingest(QStringLiteral("a"), hex(kAnnounce));
    page->node()->tickForTest();
    page->selectDestination(hex("fae321c442e3c9bdcd7a3e79d850e03c"));
    page->updateViewsNow();
    check(page->tile(1)->value() == QStringLiteral("161"), QStringLiteral("Destinations tile counts 161 (%1)").arg(page->tile(1)->value()));
    check(page->tile(0)->value() == QStringLiteral("Online"), QStringLiteral("Connection tile shows Online"));
    check(page->networkMap()->plottedCount() == 161, QStringLiteral("Every destination is placed on the map"));
    const QPointF point = page->networkMap()->positionOf(hex("fae321c442e3c9bdcd7a3e79d850e03c"));
    check(page->networkMap()->rect().contains(point.toPoint()), QStringLiteral("Map positions fall inside the map"));
    settle();
    if (!screenshotDir.isEmpty())
        page->grab().save(QDir(screenshotDir).filePath(QStringLiteral("reticulum-network.png")));

    Peer bob(QStringLiteral("Bob on Sideband"), hub.server.serverPort());
    check(waitFor([&] { return bob.node.onlineCount() == 1 && hub.clients.size() >= 2; }), QStringLiteral("Page: a peer joins the hub"));
    page->node()->announce();
    bob.node.announce();
    check(waitFor([&] { return bob.node.destinations().contains(page->messenger()->address()) &&
                               page->node()->destinations().contains(bob.messenger.address()); }),
          QStringLiteral("Page: page and peer hear each other"));
    bob.messenger.send(page->messenger()->address(), QStringLiteral("Hello from the mesh! Are you there?"));
    check(waitFor([&] { return page->navigationBadge() == 1; }, 15000), QStringLiteral("Page: an incoming message shows an unread badge"));
    page->openConversation(bob.messenger.address());
    check(page->navigationBadge() == 0, QStringLiteral("Page: opening the conversation marks it read"));
    const QString reply = page->sendMessage(QStringLiteral("Loud and clear. Writing from the ForkMesh desktop over Reticulum."));
    check(waitFor([&] { return page->messenger()->message(reply) && page->messenger()->message(reply)->state == MessageState::Delivered; }, 15000),
          QStringLiteral("Page: a reply typed on the page is delivered"));
    check(page->hostPromptTarget().action == QStringLiteral("Send to Bob on Sideband"),
          QStringLiteral("Page: the window's prompt box becomes the message composer"));
    check(page->submitHostPrompt(QStringLiteral("Sent from the footer prompt"), {}),
          QStringLiteral("Page: the footer prompt sends a message"));
    check(waitFor([&] { return bob.messenger.messages(page->messenger()->address()).size() == 3; }),
          QStringLiteral("Page: the peer receives the reply"));
    page->updateViewsNow();
    check(page->conversationList()->count() == 1 && page->chatList()->count() == 3,
          QStringLiteral("Page: the Messages tab lists the conversation and both messages"));
    settle();
    const auto bubbleShows = [&page](const QString &text) {
        for (const QLabel *label : page->chatList()->findChildren<QLabel *>()) {
            if (label->text().contains(text))
                return true;
        }
        return false;
    };
    check(bubbleShows(QStringLiteral("Delivered")), QStringLiteral("Page: the reply bubble shows it was delivered"));
    if (!screenshotDir.isEmpty())
        page->grab().save(QDir(screenshotDir).filePath(QStringLiteral("reticulum-messages.png")));

    page->tabs()->setCurrentIndex(ReticulumFeature::DestinationsTab);
    page->updateViewsNow();
    check(page->destinationTree()->topLevelItemCount() == page->node()->destinations().size(), QStringLiteral("Destinations table lists every destination"));
    const QList<forkmesh::market::NativePackageSearchHit> hits = page->searchHits(QStringLiteral("Field radio"), 5);
    check(hits.size() == 1, QStringLiteral("Top search finds a destination by name"));
    settle();
    if (!screenshotDir.isEmpty())
        page->grab().save(QDir(screenshotDir).filePath(QStringLiteral("reticulum-destinations.png")));

    page->tabs()->setCurrentIndex(ReticulumFeature::InterfacesTab);
    page->interfaceTree()->setCurrentItem(page->interfaceTree()->topLevelItem(0));
    page->updateViewsNow();
    check(page->interfaceTree()->topLevelItemCount() == 2 &&
              page->interfaceTree()->topLevelItem(0)->text(3).contains(QStringLiteral("Online")),
          QStringLiteral("Interfaces table shows both interfaces and their state"));
    settle();
    if (!screenshotDir.isEmpty())
        page->grab().save(QDir(screenshotDir).filePath(QStringLiteral("reticulum-interfaces.png")));
    page->tabs()->setCurrentIndex(ReticulumFeature::ActivityTab);
    page->updateViewsNow();
    settle();
    if (!screenshotDir.isEmpty())
        page->grab().save(QDir(screenshotDir).filePath(QStringLiteral("reticulum-activity.png")));
    page->tabs()->setCurrentIndex(ReticulumFeature::IdentityTab);
    page->updateViewsNow();
    settle();
    if (!screenshotDir.isEmpty())
        page->grab().save(QDir(screenshotDir).filePath(QStringLiteral("reticulum-identity.png")));
}

void testListenLive()
{
    const QString tcp = qEnvironmentVariable("RETICULUM_LISTEN_TCP");
    if (tcp.isEmpty())
        return;
    Node node;
    node.setIdentity(Identity::generate());
    node.setAnswerPathRequests(false);
    InterfaceConfig config;
    config.id = QStringLiteral("live");
    config.name = QStringLiteral("Live");
    config.host = tcp.section(QLatin1Char(':'), 0, 0);
    config.port = tcp.section(QLatin1Char(':'), 1, 1).toInt();
    node.setInterfaces({config});
    node.start();
    check(waitFor([&] { return node.onlineCount() == 1; }, 15000), QStringLiteral("Live: connected to %1").arg(tcp));
    waitFor([] { return false; }, qEnvironmentVariableIntValue("RETICULUM_LISTEN_SECONDS") * 1000 + 1000);
    QHash<QString, int> kinds;
    int named = 0;
    int discovered = 0;
    for (const DestinationRecord &record : node.destinations()) {
        ++kinds[record.label];
        named += record.displayName.isEmpty() ? 0 : 1;
        discovered += record.discovery ? 1 : 0;
    }
    for (auto it = kinds.cbegin(); it != kinds.cend(); ++it)
        qInfo("LIVE kind %s: %d", qPrintable(it.key()), it.value());
    qInfo("LIVE destinations=%lld named=%d discovered=%d announces=%lld duplicates=%lld rejected=%lld invalid=%lld rx=%lld tx=%lld",
          qint64(node.destinations().size()), named, discovered, node.counters().announces, node.counters().duplicates,
          node.counters().rejectedAnnounces, node.counters().invalid, node.status(config.id).rxBytes,
          node.status(config.id).txBytes);
    check(node.status(config.id).txBytes == 0, QStringLiteral("Live: listening sent nothing"));
    node.stop();
}

void testLxmfInterop()
{
    const QString tcp = qEnvironmentVariable("RETICULUM_LXMF_TCP");
    const QByteArray peer = parseHash(qEnvironmentVariable("RETICULUM_LXMF_PEER"));
    const QString out = qEnvironmentVariable("RETICULUM_LXMF_OUT");
    if (tcp.isEmpty() || peer.isEmpty())
        return;
    Node node;
    node.setIdentity(Identity::generate());
    Messenger messenger(&node);
    messenger.setDisplayName(QStringLiteral("ForkMesh desktop"));
    node.setAnnounceHandler([&messenger](const QByteArray &hash) { messenger.destinationAnnounced(hash); });
    node.setPacketHandler([&messenger](const QString &id, const Packet &packet, const QByteArray &raw, int hops) {
        messenger.handlePacket(id, packet, raw, hops);
    });
    InterfaceConfig config;
    config.id = QStringLiteral("lxmf");
    config.name = QStringLiteral("Python LXMF");
    config.host = tcp.section(QLatin1Char(':'), 0, 0);
    config.port = tcp.section(QLatin1Char(':'), 1, 1).toInt();
    config.networkName = qEnvironmentVariable("RETICULUM_LXMF_NETNAME");
    config.passphrase = qEnvironmentVariable("RETICULUM_LXMF_PASSPHRASE");
    node.setInterfaces({config});
    node.start();
    check(waitFor([&] { return node.onlineCount() == 1; }, 10000), QStringLiteral("LXMF: connected"));
    check(waitFor([&] { return node.destinations().contains(peer); }, 20000), QStringLiteral("LXMF: Python peer announce heard"));
    node.announce();
    QFile file(out);
    if (file.open(QIODevice::WriteOnly))
        file.write(hexHash(messenger.address()).toLatin1());
    file.close();
    const QString shortText = QStringLiteral("Hello from ForkMesh");
    const QString mediumText = QString(300, QLatin1Char('m'));
    QString longText;
    for (int i = 0; i < 400; ++i)
        longText += QStringLiteral("line %1 of a long message from ForkMesh\n").arg(i);
    const QString a = messenger.send(peer, shortText, QStringLiteral("short"));
    const QString b = messenger.send(peer, mediumText, QStringLiteral("medium"));
    const QString c = messenger.send(peer, longText, QStringLiteral("long"));
    const auto delivered = [&](const QString &id) {
        const std::optional<ChatMessage> message = messenger.message(id);
        return message && message->state == MessageState::Delivered;
    };
    waitFor([&] { return delivered(a) && delivered(b) && delivered(c); }, 90000);
    for (const QString &id : {a, b, c}) {
        const std::optional<ChatMessage> message = messenger.message(id);
        check(message && message->state == MessageState::Delivered,
              QStringLiteral("LXMF: %1 message delivered by %2 (%3 %4)")
                  .arg(message ? message->title : QString(), message ? message->method : QString(),
                       message ? messageStateLabel(message->state) : QString(), message ? message->note : QString()));
    }
    const auto received = [&](const QString &title) -> std::optional<ChatMessage> {
        for (const ChatMessage &message : messenger.messages(peer)) {
            if (!message.outgoing && message.title == title)
                return message;
        }
        return std::nullopt;
    };
    waitFor([&] { return received(QStringLiteral("py-short")) && received(QStringLiteral("py-medium")) &&
                         received(QStringLiteral("py-long")); }, 90000);
    for (const QString &title : {QStringLiteral("py-short"), QStringLiteral("py-medium"), QStringLiteral("py-long")}) {
        const std::optional<ChatMessage> message = received(title);
        check(message && message->verified && message->content.size() > 0,
              QStringLiteral("LXMF: received %1 by %2, %3 characters, signature %4")
                  .arg(title, message ? message->method : QStringLiteral("-"))
                  .arg(message ? message->content.size() : 0)
                  .arg(message && message->verified ? QStringLiteral("verified") : QStringLiteral("NOT verified")));
    }
    waitFor([] { return false; }, 3000);
    node.stop();
}

void testMessaging()
{
    {
        const QByteArray payload = lxmfPayload(1767225600.5, QStringLiteral("t"), QStringLiteral("hello"));
        const Identity sender = Identity::generate();
        const QByteArray source = destinationHashForName(QStringLiteral("lxmf.delivery"), sender.hash());
        const QByteArray destination = QByteArray(16, 'd');
        LxmfMessage packedMessage;
        const QByteArray stamped = lxmfPack(sender, source, destination, 1767225600.5, QStringLiteral("t"),
                                            QStringLiteral("hello"), QByteArray(32, 's'), &packedMessage);
        const std::optional<LxmfMessage> unpacked = lxmfUnpack(stamped);
        check(unpacked && unpacked->content == QStringLiteral("hello") && unpacked->stamp == QByteArray(32, 's') &&
                  unpacked->hash == packedMessage.hash && lxmfVerify(*unpacked, sender.publicKey()),
              QStringLiteral("LXMF messages with a stamp round trip and verify"));
        check(payload.at(0) == char(0x94) && payload.at(1) == char(0xcb), QStringLiteral("LXMF payload is a four-item array starting with a float"));
        const QByteArray key = randomBytes(64);
        const std::optional<QByteArray> plain = tokenDecrypt(key, tokenEncrypt(key, "secret"));
        check(plain && *plain == "secret", QStringLiteral("Token encryption round trips"));
        QByteArray forged = tokenEncrypt(key, "secret");
        forged[20] = char(forged.at(20) ^ 1);
        check(!tokenDecrypt(key, forged), QStringLiteral("A tampered token is refused"));
        const Identity recipient = Identity::generate();
        const std::optional<QByteArray> opened = recipient.decrypt(recipient.encrypt("to you"));
        check(opened && *opened == "to you" && !Identity::generate().decrypt(recipient.encrypt("to you")),
              QStringLiteral("Identity encryption opens only for the recipient"));
        const InterfaceAccessCode code(QStringLiteral("net"), QStringLiteral("pass"));
        const QByteArray announce = hex(kAnnounce);
        const QByteArray masked = code.apply(announce);
        check((quint8(masked.at(0)) & 0x80) && code.remove(masked) == announce &&
                  !InterfaceAccessCode(QStringLiteral("net"), QStringLiteral("wrong")).remove(masked),
              QStringLiteral("Interface access codes apply, verify and reject the wrong passphrase"));
        int value = 0;
        const QByteArray id = fullHash("message");
        const QByteArray stamp = generateStamp(id, 6, nullptr, &value);
        check(stamp.size() == 32 && value >= 6 && validateStamp(id, stamp, 6), QStringLiteral("Stamps are generated and validated"));
    }

    for (const bool ifac : {false, true}) {
        Hub hub;
        const QString net = ifac ? QStringLiteral("testnet") : QString();
        const QString pass = ifac ? QStringLiteral("secret") : QString();
        Peer alice(QStringLiteral("Alice"), hub.server.serverPort(), net, pass);
        Peer bob(QStringLiteral("Bob"), hub.server.serverPort(), net, pass);
        std::unique_ptr<Peer> stranger;
        if (ifac)
            stranger = std::make_unique<Peer>(QStringLiteral("Stranger"), hub.server.serverPort(), net, QStringLiteral("wrong"));
        const QString label = ifac ? QStringLiteral(" (IFAC)") : QString();
        check(waitFor([&] { return alice.node.onlineCount() == 1 && bob.node.onlineCount() == 1 && hub.clients.size() >= (ifac ? 3 : 2); }),
              QStringLiteral("Messaging peers connect to the hub") + label);
        alice.node.announce();
        bob.node.announce();
        check(waitFor([&] { return bob.node.destinations().contains(alice.messenger.address()) &&
                                   alice.node.destinations().contains(bob.messenger.address()); }),
              QStringLiteral("Peers hear each other's LXMF announces") + label);
        if (stranger)
            check(stranger->node.destinations().isEmpty() && stranger->node.status(QStringLiteral("hub")).invalidFrames > 0,
                  QStringLiteral("A node with the wrong passphrase hears nothing"));
        QString longText;
        for (int i = 0; i < 300; ++i)
            longText += QStringLiteral("long line %1\n").arg(i);
        const QString shortId = bob.messenger.send(alice.messenger.address(), QStringLiteral("hi alice"));
        const QString mediumId = bob.messenger.send(alice.messenger.address(), QString(300, QLatin1Char('x')));
        const QString longId = bob.messenger.send(alice.messenger.address(), longText);
        const auto state = [&bob](const QString &id) {
            const std::optional<ChatMessage> message = bob.messenger.message(id);
            return message ? message->state : MessageState::Failed;
        };
        check(waitFor([&] { return state(shortId) == MessageState::Delivered && state(mediumId) == MessageState::Delivered &&
                                   state(longId) == MessageState::Delivered; }, 30000),
              QStringLiteral("Short, medium and long messages are delivered") + label);
        check(bob.messenger.message(shortId)->method == QStringLiteral("opportunistic") &&
                  bob.messenger.message(mediumId)->method == QStringLiteral("link") &&
                  bob.messenger.message(longId)->method == QStringLiteral("resource"),
              QStringLiteral("Each size uses its delivery method") + label);
        const QString secondLongId = bob.messenger.send(alice.messenger.address(), longText + QStringLiteral("again"));
        const QString thirdLongId = bob.messenger.send(alice.messenger.address(), longText + QStringLiteral("once more"));
        check(waitFor([&] { return state(secondLongId) == MessageState::Delivered && state(thirdLongId) == MessageState::Delivered; }, 30000),
              QStringLiteral("Two large messages to one peer are sent one after the other and both arrive") + label);
        const QList<ChatMessage> all = alice.messenger.messages(bob.messenger.address());
        QList<ChatMessage> got;
        for (const ChatMessage &m : all) {
            if (!m.content.endsWith(QStringLiteral("again")) && !m.content.endsWith(QStringLiteral("once more")))
                got.append(m);
        }
        check(got.size() == 3 && std::all_of(got.cbegin(), got.cend(), [](const ChatMessage &m) { return m.verified && !m.read; }) &&
                  std::any_of(got.cbegin(), got.cend(), [&longText](const ChatMessage &m) { return m.content == longText; }),
              QStringLiteral("Alice receives all three, signed, unread") + label);
        check(alice.messenger.unreadCount() == 5 && alice.messenger.conversations().value(0).name == QStringLiteral("Bob"),
              QStringLiteral("Conversations list Bob by name with 5 unread") + label);
        Messenger restored(&alice.node);
        restored.load(alice.messenger.saveJson());
        check(restored.messages(bob.messenger.address()).size() == 5, QStringLiteral("Messages survive a save and load") + label);
    }
}

void testInterop()
{
    const QString tcp = qEnvironmentVariable("RETICULUM_INTEROP_TCP");
    const QString shared = qEnvironmentVariable("RETICULUM_INTEROP_SHARED");
    const QByteArray target = parseHash(qEnvironmentVariable("RETICULUM_INTEROP_TARGET"));
    const QString out = qEnvironmentVariable("RETICULUM_INTEROP_OUT");
    if ((tcp.isEmpty() && shared.isEmpty()) || target.isEmpty())
        return;
    Node node;
    node.setIdentity(Identity::generate());
    node.setAnnounceName(QStringLiteral("ForkMesh interop"));
    InterfaceConfig config;
    config.id = QStringLiteral("interop");
    config.name = QStringLiteral("Python RNS");
    if (!shared.isEmpty()) {
        config.kind = InterfaceKind::SharedInstance;
        config.instanceName = shared;
    } else {
        config.host = tcp.section(QLatin1Char(':'), 0, 0);
        config.port = tcp.section(QLatin1Char(':'), 1, 1).toInt();
    }
    node.setInterfaces({config});
    node.start();
    check(waitFor([&] { return node.onlineCount() == 1; }, 10000), QStringLiteral("Interop: connected to Python RNS"));
    check(waitFor([&] {
              for (const DestinationRecord &record : node.destinations()) {
                  if (record.displayName == QStringLiteral("Python peer") && record.kind == QStringLiteral("peer"))
                      return true;
              }
              return false;
          }, 20000),
          QStringLiteral("Interop: Python RNS announce received, validated and named"));
    node.forgetDestination(target);
    check(!node.requestPath(target).isEmpty(), QStringLiteral("Interop: path request sent"));
    check(waitFor([&] { return node.lookups().value(target).answered.isValid(); }, 20000),
          QStringLiteral("Interop: Python RNS answered the path request"));
    check(node.announce(), QStringLiteral("Interop: announced this node"));
    QFile file(out);
    if (file.open(QIODevice::WriteOnly))
        file.write(hexHash(node.destinationHash()).toLatin1());
    file.close();
    waitFor([&] { return node.counters().pathRequestsAnswered > 0; }, 20000);
    check(node.counters().pathRequestsAnswered > 0, QStringLiteral("Interop: answered Python's path request for this node"));
    waitFor([] { return false; }, 1500);
    qInfo("INTEROP rx=%lld tx=%lld announces=%lld invalid=%lld rejected=%lld", node.status(config.id).rxBytes,
          node.status(config.id).txBytes, node.counters().announces, node.counters().invalid,
          node.counters().rejectedAnnounces);
    node.stop();
}

}

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    const QString screenshots = qEnvironmentVariable("RETICULUM_SCREENSHOT_DIR");
    testFraming();
    testVectors();
    testConfigImport();
    testLiveSession();
    testFirewallDenied();
    testMessaging();
    testFeature(screenshots);
    testInterop();
    testListenLive();
    testLxmfInterop();
    if (failures)
        qCritical("%d check(s) failed", failures);
    return failures == 0 ? 0 : 1;
}
