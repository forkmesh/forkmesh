// Local control channel (LocalControl.h): the phone on the same network
// drives this desktop. Covers the primitives the phone apps must match
// byte for byte, then a real loopback handshake: pairing approval, a paired
// reconnect, a request routed to the app, a file upload, and a forged
// signature refused.
#include "LocalControl.h"
#include "LocalControlHosts.h"
#include "LocalControlStream.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtEndian>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <functional>
#include <memory>
#include <iostream>

using namespace forkmesh::localcontrol;

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static bool waitFor(const std::function<bool()> &done, int timeoutMs = 5000)
{
    QElapsedTimer timer;
    timer.start();
    while (!done() && timer.elapsed() < timeoutMs)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    return done();
}

// An Ed25519 identity, as ForkMeshIdentity (desktop) and Identity (phone)
// hold one.
struct SigningKey {
    EVP_PKEY *key = nullptr;
    QString publicKey;

    SigningKey()
    {
        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
        EVP_PKEY_keygen_init(ctx);
        EVP_PKEY_keygen(ctx, &key);
        EVP_PKEY_CTX_free(ctx);
        QByteArray pub(32, Qt::Uninitialized);
        size_t length = 32;
        EVP_PKEY_get_raw_public_key(key, reinterpret_cast<uchar *>(pub.data()),
                                    &length);
        publicKey = toBase64Url(pub);
    }
    ~SigningKey() { EVP_PKEY_free(key); }

    QString sign(const QByteArray &data) const
    {
        QByteArray sig(64, Qt::Uninitialized);
        size_t length = 64;
        EVP_MD_CTX *ctx = EVP_MD_CTX_new();
        EVP_DigestSignInit(ctx, nullptr, nullptr, nullptr, key);
        EVP_DigestSign(ctx, reinterpret_cast<uchar *>(sig.data()), &length,
                       reinterpret_cast<const uchar *>(data.constData()),
                       size_t(data.size()));
        EVP_MD_CTX_free(ctx);
        return toBase64Url(sig);
    }
};

static QByteArray frame(const QByteArray &payload)
{
    QByteArray out(4, '\0');
    qToBigEndian(quint32(payload.size()), reinterpret_cast<uchar *>(out.data()));
    return out + payload;
}

// The phone's half of the protocol, written from the header's description.
struct TestClient {
    const SigningKey &identity;
    QTcpSocket socket;
    QByteArray buffer;
    QByteArray transcriptBytes;
    SessionKeys keys;
    quint64 sendCounter = 0;
    quint64 receiveCounter = 0;
    QString serverKey;
    QString code;
    bool welcomed = false;
    bool forgeAuth = false;
    QList<QJsonObject> messages;

    explicit TestClient(const SigningKey &key) : identity(key) {}

    bool connectTo(quint16 port)
    {
        socket.connectToHost(QHostAddress::LocalHost, port);
        if (!socket.waitForConnected(3000))
            return false;
        Ephemeral eph = newEphemeral();
        QByteArray nonce(16, Qt::Uninitialized);
        RAND_bytes(reinterpret_cast<uchar *>(nonce.data()), 16);
        const QString ephB64 = toBase64Url(eph.publicKey);
        const QString nonceB64 = toBase64Url(nonce);
        const QJsonObject hello{
            {"t", helloTag()}, {"v", kProtocolVersion},
            {"key", identity.publicKey}, {"eph", ephB64},
            {"nonce", nonceB64}, {"name", "Test phone"},
            {"user", "alice"}, {"platform", "ios"}};
        socket.write(frame(QJsonDocument(hello).toJson(QJsonDocument::Compact)));
        QObject::connect(&socket, &QTcpSocket::readyRead, &socket,
                         [this, eph, ephB64, nonceB64] {
            buffer.append(socket.readAll());
            while (buffer.size() >= 4) {
                const quint32 length = qFromBigEndian<quint32>(
                    reinterpret_cast<const uchar *>(buffer.constData()));
                if (buffer.size() < qsizetype(length) + 4)
                    return;
                const QByteArray payload = buffer.mid(4, length);
                buffer.remove(0, qsizetype(length) + 4);
                if (!welcomed) {
                    const QJsonObject w = QJsonDocument::fromJson(payload).object();
                    if (w.value("t").toString() != welcomeTag())
                        continue; // the mesh's own key frame
                    serverKey = w.value("key").toString();
                    const QString serverEph = w.value("eph").toString();
                    transcriptBytes = transcript(identity.publicKey, ephB64,
                                                 nonceB64, serverKey, serverEph);
                    if (!verifyEd25519(serverKey, w.value("sig").toString(),
                                       serverSignedBytes(transcriptBytes)))
                        return;
                    keys = deriveKeys(
                        x25519(eph.privateKey, fromBase64Url(serverEph, 32)),
                        transcriptBytes);
                    code = pairingCode(transcriptBytes);
                    welcomed = true;
                    const QString sig =
                        identity.sign(forgeAuth ? QByteArray("forged")
                                                : clientSignedBytes(transcriptBytes));
                    socket.write(frame(QJsonDocument(QJsonObject{
                                                         {"t", authTag()},
                                                         {"sig", sig}})
                                           .toJson(QJsonDocument::Compact)));
                    continue;
                }
                const QByteArray plain =
                    openFrame(keys.serverToClient, receiveCounter++, payload);
                messages.append(QJsonDocument::fromJson(plain).object());
            }
        });
        return true;
    }

    void send(const QJsonObject &message)
    {
        socket.write(frame(sealFrame(keys.clientToServer, sendCounter++,
                                     QJsonDocument(message).toJson(
                                         QJsonDocument::Compact))));
    }

    QJsonObject find(const std::function<bool(const QJsonObject &)> &match) const
    {
        for (const QJsonObject &m : messages)
            if (match(m))
                return m;
        return {};
    }
    QJsonObject event(const QString &name) const
    {
        return find([&](const QJsonObject &m) { return m.value("event").toString() == name; });
    }
    QJsonObject response(int id) const
    {
        return find([&](const QJsonObject &m) { return m.value("id").toInt() == id; });
    }
};

static void testPrimitives()
{
    // Fixed vectors shared with the phone apps.
    const QByteArray t = transcript("CK", "CE", "CN", "SK", "SE");
    check(t == "forkmesh-local-control-v1\nCK\nCE\nCN\nSK\nSE", "transcript layout");
    const QByteArray shared(32, '\x07');
    const SessionKeys keys = deriveKeys(shared, t);
    check(keys.isValid(), "keys derived");
    check(keys.clientToServer != keys.serverToClient, "directions differ");
    check(keys.clientToServer.toHex() ==
              "6e7a4a8c0002d3118fbc2d04ddbc268e61b1b45af566060d38e3235b2bd1e239",
          "c2s vector");
    check(keys.serverToClient.toHex() ==
              "b76be7e0ae92fe18b561280e896315649f905d4ebed4685676c4dc4b1158596f",
          "s2c vector");
    check(pairingCode(t) == "796568", "pairing code vector");
    const QByteArray sealed = sealFrame(keys.clientToServer, 3, "{\"id\":1}");
    check(sealed.toHex() == "4fb9fc7fc067c05ecc5591ec3f59b2f37b2727354152adc0",
          "sealed frame vector");
    check(openFrame(keys.clientToServer, 3, sealed) == "{\"id\":1}", "seal round trip");
    check(openFrame(keys.clientToServer, 4, sealed).isEmpty(), "wrong counter refused");
    check(openFrame(keys.serverToClient, 3, sealed).isEmpty(), "wrong key refused");
    check(pairingCode(t).size() == 6, "six digit code");

    const Ephemeral a = newEphemeral();
    const Ephemeral b = newEphemeral();
    check(!x25519(a.privateKey, b.publicKey).isEmpty() &&
              x25519(a.privateKey, b.publicKey) == x25519(b.privateKey, a.publicKey),
          "x25519 agrees");

    check(safeFileName("../../etc/passwd") == "passwd", "path stripped");
    check(safeFileName("..\\evil.txt") == "evil.txt", "windows path stripped");
    check(safeFileName(".hidden") == "hidden", "no dot files");
    check(safeFileName("") == "file", "empty name");
    check(safeFileName("a/b:c?.png") == "b_c_.png", "unsafe characters");

    check(isControlHello(R"({"t":"fm-control-hello","v":1})"), "hello detected");
    check(!isControlHello(R"({"t":"fm-control-hello","v":2})"), "other version");
    check(!isControlHello(R"({"iv":"x","ct":"y"})"), "mesh envelope");
}

static void testLoopback()
{
    QTemporaryDir dir;
    SigningKey desktopKey;
    SigningKey phoneKey;
    const QString store = dir.filePath("paired.json");
    const QString inbox = dir.filePath("inbox");

    auto *server = new LocalControlServer(
        desktopKey.publicKey,
        [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server->setDesktopInfo("desk", "alice");
    server->setDeviceStorePath(store);
    server->setReceiveDirectory(inbox);

    int pairingConnection = 0;
    QString pairingCodeShown;
    server->onPairingRequested = [&](int id, const LocalControlServer::Device &device,
                                     const QString &code) {
        pairingConnection = id;
        pairingCodeShown = code;
        check(device.name == "Test phone", "device name carried");
        check(device.user == "alice", "device user carried");
    };
    QString requested;
    server->onRequest = [&](int conn, const LocalControlServer::Device &, int id,
                            const QString &method, const QJsonObject &params) {
        requested = method;
        server->respond(conn, id, {{"echo", params.value("x")}});
    };
    QString received;
    server->onFileReceived = [&](const LocalControlServer::Device &,
                                 const QString &path) { received = path; };

    // Stands in for LocalMeshNode: the first frame decides, the rest is
    // handed over with the socket.
    QTcpServer listener;
    check(listener.listen(QHostAddress::LocalHost, 0), "listener");
    QObject::connect(&listener, &QTcpServer::newConnection, [&] {
        QTcpSocket *socket = listener.nextPendingConnection();
        // What the mesh writes before it knows: its own key frame.
        socket->write(frame(R"({"iv":"mesh","ct":"key"})"));
        auto *buffer = new QByteArray;
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [=] {
            buffer->append(socket->readAll());
            if (buffer->size() < 4)
                return;
            const quint32 length = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar *>(buffer->constData()));
            if (buffer->size() < qsizetype(length) + 4)
                return;
            const QByteArray first = buffer->mid(4, length);
            const QByteArray rest = buffer->mid(4 + length);
            delete buffer;
            socket->disconnect();
            check(isControlHello(first), "first frame is a hello");
            server->adopt(socket, first, rest);
        });
    });

    {
        TestClient phone(phoneKey);
        check(phone.connectTo(listener.serverPort()), "phone connects");
        check(waitFor([&] { return !phone.event("session").isEmpty(); }),
              "session event");
        check(phone.serverKey == desktopKey.publicKey, "desktop key pinned");
        const QJsonObject pending = phone.event("session").value("data").toObject();
        check(!pending.value("paired").toBool(), "unknown phone not paired");
        check(pending.value("code").toString() == phone.code, "codes match");
        check(pairingCodeShown == phone.code, "desktop shows the same code");

        phone.send({{"id", 1}, {"method", "agents.list"}});
        check(waitFor([&] { return !phone.response(1).isEmpty(); }), "pending answer");
        check(phone.response(1).value("error").toString() == "not_paired",
              "requests refused before approval");
        check(requested.isEmpty(), "app never saw it");

        server->approvePairing(pairingConnection);
        check(waitFor([&] {
                  return phone.messages.size() >= 3 &&
                         phone.messages.last().value("data").toObject()
                             .value("paired").toBool();
              }),
              "approval reaches the phone");
        check(server->isPaired(phoneKey.publicKey), "remembered");
        check(server->isConnected(phoneKey.publicKey), "connected");

        phone.send({{"id", 2}, {"method", "agents.list"}, {"params", QJsonObject{{"x", 5}}}});
        check(waitFor([&] { return !phone.response(2).isEmpty(); }), "request answered");
        check(requested == "agents.list", "routed to the app");
        check(phone.response(2).value("result").toObject().value("echo").toInt() == 5,
              "result returned");

        const QByteArray content = QByteArray(300000, 'x') + "tail";
        phone.send({{"id", 3}, {"method", "files.begin"},
                    {"params", QJsonObject{{"name", "../photo.jpg"},
                                           {"size", double(content.size())}}}});
        check(waitFor([&] { return !phone.response(3).isEmpty(); }), "begin answered");
        const QString transfer =
            phone.response(3).value("result").toObject().value("transfer").toString();
        check(!transfer.isEmpty(), "transfer id");
        const int chunk = fileChunkBytes();
        int nextId = 4;
        for (qsizetype off = 0; off < content.size(); off += chunk)
            phone.send({{"id", nextId++}, {"method", "files.chunk"},
                        {"params", QJsonObject{{"transfer", transfer},
                                               {"data", QString::fromLatin1(
                                                    content.mid(off, chunk).toBase64())}}}});
        const int endId = nextId;
        phone.send({{"id", endId}, {"method", "files.end"},
                    {"params", QJsonObject{{"transfer", transfer}}}});
        check(waitFor([&] { return !phone.response(endId).isEmpty(); }), "end answered");
        check(phone.response(endId).value("ok").toBool(), "upload ok");
        check(received == QDir(inbox).filePath("photo.jpg"), "saved in the inbox");
        QFile saved(received);
        check(saved.open(QIODevice::ReadOnly) && saved.readAll() == content,
              "content intact");

        // Desktop to phone.
        QFile out(dir.filePath("report.txt"));
        out.open(QIODevice::WriteOnly);
        out.write("hello phone");
        out.close();
        check(server->sendFile(phoneKey.publicKey, out.fileName()), "send file");
        check(waitFor([&] { return !phone.event("file.end").isEmpty(); }), "file end");
        check(QByteArray::fromBase64(phone.event("file.chunk").value("data").toObject()
                                         .value("data").toString().toLatin1()) ==
                  "hello phone",
              "file bytes arrive");
        check(phone.event("file.begin").value("data").toObject().value("name").toString() ==
                  "report.txt",
              "file name arrives");

        server->broadcast("agents.changed");
        check(waitFor([&] { return !phone.event("agents.changed").isEmpty(); }),
              "broadcast");
    }
    check(waitFor([&] { return !server->isConnected(phoneKey.publicKey); }),
          "disconnect noticed");

    // A paired phone reconnects straight into control; approvals survive a
    // restart of the desktop.
    delete server;
    server = new LocalControlServer(
        desktopKey.publicKey,
        [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server->setDeviceStorePath(store);
    check(server->isPaired(phoneKey.publicKey), "approval persisted");
    pairingConnection = 0;
    server->onPairingRequested = [&](int id, const LocalControlServer::Device &,
                                     const QString &) { pairingConnection = id; };
    {
        TestClient phone(phoneKey);
        check(phone.connectTo(listener.serverPort()), "reconnects");
        check(waitFor([&] { return !phone.event("session").isEmpty(); }),
              "reconnect session");
        check(phone.event("session").value("data").toObject().value("paired").toBool(),
              "paired on reconnect");
        check(pairingConnection == 0, "no second prompt");
    }

    // A phone that cannot sign for the key it claims is cut off.
    {
        SigningKey other;
        TestClient impostor(other);
        impostor.forgeAuth = true;
        check(impostor.connectTo(listener.serverPort()), "impostor connects");
        check(waitFor([&] {
                  return impostor.socket.state() == QAbstractSocket::UnconnectedState;
              }),
              "forged auth closed");
        check(impostor.messages.isEmpty(), "impostor got nothing");
        check(pairingConnection == 0, "no prompt for a forgery");
    }

    // Forgetting a phone closes its channel and drops the approval.
    server->forget(phoneKey.publicKey);
    check(!server->isPaired(phoneKey.publicKey), "forgotten");
    delete server;
}

// The phone's half of the relayed transport: the same handshake and sealed
// frames, carried as {"type":"control"} chunks the relay object forwards.
struct RelayTestClient {
    const SigningKey &identity;
    QString session = QStringLiteral("relay-session-0123456789");
    QString node = QStringLiteral("desk");
    QString platform = QStringLiteral("ios");
    QList<QJsonObject> fromDesktop; // raw relay frames the desktop sent
    QByteArray pending;
    quint32 sendSeq = 0;
    quint32 receiveSeq = 0;
    Ephemeral eph = newEphemeral();
    QString ephB64;
    QString nonceB64;
    SessionKeys keys;
    quint64 sendCounter = 0;
    quint64 receiveCounter = 0;
    bool welcomed = false;
    bool closed = false;
    QString closeReason;
    QList<QJsonObject> messages;
    LocalControlServer *server = nullptr;

    explicit RelayTestClient(const SigningKey &key) : identity(key) {}

    QJsonObject chunkFrame(const QString &action, const QByteArray &bytes,
                           bool more = false)
    {
        return {{"type", "control"}, {"action", action}, {"session", session},
                {"node", node}, {"from", "browser"},
                {"seq", qint64(sendSeq++)},
                {"data", QString::fromLatin1(bytes.toBase64())}, {"more", more}};
    }

    void open()
    {
        QByteArray nonce(16, Qt::Uninitialized);
        RAND_bytes(reinterpret_cast<uchar *>(nonce.data()), 16);
        ephB64 = toBase64Url(eph.publicKey);
        nonceB64 = toBase64Url(nonce);
        const QByteArray hello = QJsonDocument(QJsonObject{
            {"t", helloTag()}, {"v", kProtocolVersion},
            {"key", identity.publicKey}, {"eph", ephB64},
            {"nonce", nonceB64}, {"name", "Away phone"},
            {"user", "alice"}, {"platform", platform}}).toJson(QJsonDocument::Compact);
        // Split the hello to exercise reassembly of an "open".
        server->relayFrame(chunkFrame("open", hello.left(40), true));
        server->relayFrame(chunkFrame("open", hello.mid(40)));
        pump();
    }

    // Reassemble what the desktop sent and run the phone's side.
    void pump()
    {
        while (!fromDesktop.isEmpty()) {
            const QJsonObject f = fromDesktop.takeFirst();
            if (f.value("session").toString() != session)
                continue;
            const QString action = f.value("action").toString();
            if (action == "close" || action == "error") {
                closed = true;
                closeReason = f.value("reason").toString();
                continue;
            }
            check(quint32(f.value("seq").toInt()) == receiveSeq++,
                  "desktop chunks arrive in sequence");
            pending.append(QByteArray::fromBase64(f.value("data").toString().toLatin1()));
            if (f.value("more").toBool())
                continue;
            const QByteArray payload = pending;
            pending.clear();
            if (!welcomed) {
                const QJsonObject w = QJsonDocument::fromJson(payload).object();
                const QString serverKey = w.value("key").toString();
                const QString serverEph = w.value("eph").toString();
                const QByteArray t = transcript(identity.publicKey, ephB64,
                                                nonceB64, serverKey, serverEph);
                check(verifyEd25519(serverKey, w.value("sig").toString(),
                                    serverSignedBytes(t)),
                      "relayed welcome is signed by the desktop");
                keys = deriveKeys(x25519(eph.privateKey, fromBase64Url(serverEph, 32)), t);
                welcomed = true;
                server->relayFrame(chunkFrame(
                    "data", QJsonDocument(QJsonObject{
                                {"t", authTag()},
                                {"sig", identity.sign(clientSignedBytes(t))}})
                                .toJson(QJsonDocument::Compact)));
                continue;
            }
            messages.append(QJsonDocument::fromJson(
                                openFrame(keys.serverToClient, receiveCounter++, payload))
                                .object());
        }
    }

    void send(const QJsonObject &message)
    {
        const QByteArray sealed = sealFrame(keys.clientToServer, sendCounter++,
                                            QJsonDocument(message).toJson(
                                                QJsonDocument::Compact));
        server->relayFrame(chunkFrame("data", sealed));
        pump();
    }

    QJsonObject find(const std::function<bool(const QJsonObject &)> &match) const
    {
        for (const QJsonObject &m : messages)
            if (match(m))
                return m;
        return {};
    }
    QJsonObject event(const QString &name) const
    {
        return find([&](const QJsonObject &m) { return m.value("event").toString() == name; });
    }
    QJsonObject response(int id) const
    {
        return find([&](const QJsonObject &m) { return m.value("id").toInt() == id; });
    }
};

static void testRelay()
{
    QTemporaryDir dir;
    SigningKey desktopKey;
    SigningKey pairedPhone;
    SigningKey strangerPhone;
    // Approved earlier, in person, on the desktop's own network.
    const QString store = dir.filePath("paired.json");
    {
        QFile file(store);
        file.open(QIODevice::WriteOnly);
        file.write(QJsonDocument(QJsonObject{
            {"devices", QJsonArray{QJsonObject{
                {"key", pairedPhone.publicKey}, {"name", "Away phone"},
                {"user", "alice"}, {"pairedAtMs", 1.0}}}}}).toJson());
    }
    QList<QJsonObject> outbox;
    LocalControlServer server(
        desktopKey.publicKey,
        [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server.setDesktopInfo("desk", "alice");
    server.setDeviceStorePath(store);
    server.setRelaySink([&](const QJsonObject &f) { outbox.append(f); return true; },
                        "Desk");
    bool prompted = false;
    server.onPairingRequested = [&](int, const LocalControlServer::Device &,
                                    const QString &) { prompted = true; };
    int lastConnection = 0;
    server.onRequest = [&](int conn, const LocalControlServer::Device &device, int id,
                           const QString &method, const QJsonObject &) {
        lastConnection = conn;
        check(device.key == pairedPhone.publicKey, "request carries the device");
        if (method == "big")
            server.respond(conn, id, {{"blob", QString(30000, QLatin1Char('b'))}});
        else
            server.respond(conn, id, {{"method", method}});
    };
    QList<int> closedConnections;
    server.onConnectionClosed = [&](int id) { closedConnections.append(id); };

    const auto deliver = [&](RelayTestClient &phone) {
        phone.fromDesktop.append(outbox);
        outbox.clear();
        phone.pump();
        phone.fromDesktop.append(outbox);
        outbox.clear();
        phone.pump();
    };

    // A phone the desktop never approved is refused, with no prompt: nobody
    // is there to compare the code.
    {
        RelayTestClient stranger(strangerPhone);
        stranger.server = &server;
        stranger.open();
        deliver(stranger);
        check(stranger.event("pairing-denied").value("data").toObject()
                      .value("reason").toString() == "pair_on_lan",
              "unpaired relay phone told to pair on the LAN");
        check(stranger.closed, "unpaired relay channel closed");
        check(!prompted, "no pairing prompt over the relay");
        check(!server.isConnected(strangerPhone.publicKey), "stranger not connected");
    }

    RelayTestClient phone(pairedPhone);
    phone.session = QStringLiteral("relay-session-paired-0001");
    phone.server = &server;
    phone.open();
    deliver(phone);
    const QJsonObject session = phone.event("session").value("data").toObject();
    check(session.value("paired").toBool(), "paired phone controls over the relay");
    check(session.value("transport").toString() == "relay", "transport reported");
    check(server.isConnected(pairedPhone.publicKey), "relay connection counted");

    phone.send({{"id", 1}, {"method", "agents.list"}});
    deliver(phone);
    check(phone.response(1).value("result").toObject().value("method").toString() ==
              "agents.list",
          "request routed over the relay");
    check(server.isRelayed(lastConnection), "connection is relayed");
    check(server.deviceFor(lastConnection).user == "alice", "device user known");

    // A response larger than one chunk is split and joined back.
    phone.send({{"id", 2}, {"method", "big"}});
    deliver(phone);
    check(phone.response(2).value("result").toObject().value("blob").toString().size() ==
              30000,
          "large response reassembled");

    phone.send({{"id", 3}, {"method", "files.begin"},
                {"params", QJsonObject{{"name", "a.txt"}, {"size", 3}}}});
    deliver(phone);
    check(phone.response(3).value("error").toString() == "lan_only",
          "files stay on the LAN");

    // Gate 2 families: routed by the longest prefix, before onRequest and
    // before the built-in files.* methods, and never for a bare prefix.
    QStringList familyCalls;
    check(!server.registerFamily("nodot", [](int, const LocalControlServer::Device &, int,
                                             const QString &, const QJsonObject &) {}),
          "a family prefix must end with a dot");
    check(server.registerFamily("files.browse.",
                                [&](int conn, const LocalControlServer::Device &, int id,
                                    const QString &method, const QJsonObject &) {
                                    familyCalls.append(method);
                                    server.respond(conn, id, {{"family", "files.browse."}});
                                }),
          "files.browse. registered");
    check(server.familyFor("files.browse.list") == "files.browse.", "longest prefix");
    check(server.familyFor("files.begin").isEmpty(), "files.* stays built in");
    check(server.familyFor("files.browse.").isEmpty(), "bare prefix is no method");
    phone.send({{"id", 5}, {"method", "files.browse.list"}});
    deliver(phone);
    check(phone.response(5).value("result").toObject().value("family").toString() ==
              "files.browse.",
          "family answered over the relay");
    check(familyCalls == QStringList{"files.browse.list"}, "family saw its method");

    // Frames for another machine, or echoed from a node, are ignored.
    const int before = phone.messages.size();
    QJsonObject other = phone.chunkFrame("data", "x");
    other["node"] = "someone-else";
    server.relayFrame(other);
    QJsonObject echoed = phone.chunkFrame("data", "x");
    echoed["from"] = "node";
    server.relayFrame(echoed);
    phone.sendSeq -= 2; // neither reached the channel
    deliver(phone);
    check(phone.messages.size() == before && !phone.closed,
          "misaddressed frames ignored");

    // Every chunk moves the frame counter; a gap cannot be recovered.
    phone.sendSeq += 1;
    phone.send({{"id", 6}, {"method", "agents.list"}});
    deliver(phone);
    check(phone.closed, "out-of-order chunk closes the channel");
    check(!server.isConnected(pairedPhone.publicKey), "connection gone");
    check(closedConnections.contains(lastConnection), "close reported to the app");
}

static void testAccountDevices()
{
    QTemporaryDir dir;
    SigningKey desktopKey;
    SigningKey accountPhone;
    SigningKey otherPhone;
    SigningKey accountRelayPhone;
    SigningKey otherRelayPhone;
    QTcpServer listener;
    QList<QJsonObject> outbox;

    LocalControlServer server(
        desktopKey.publicKey,
        [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server.setDesktopInfo("desk", "alice");
    server.setDeviceStorePath(dir.filePath("paired.json"));
    server.setRelaySink([&](const QJsonObject &f) { outbox.append(f); return true; },
                        "desk");
    const QSet<QString> accountKeys{accountPhone.publicKey,
                                    accountRelayPhone.publicKey};
    QStringList asked;
    using Verdict = LocalControlServer::AccountCheck;
    server.accountDeviceCheck = [&](const LocalControlServer::Device &device,
                                    std::function<void(Verdict)> done) {
        asked.append(device.key);
        const Verdict verdict = accountKeys.contains(device.key)
                                    ? Verdict::Owned
                                    : Verdict::NotOwned;
        if (device.key == otherRelayPhone.publicKey)
            done(verdict);
        else
            QTimer::singleShot(30, [done, verdict] { done(verdict); });
    };
    QStringList prompted;
    server.onPairingRequested = [&](int, const LocalControlServer::Device &device,
                                    const QString &) { prompted.append(device.key); };

    check(listener.listen(QHostAddress::LocalHost, 0), "account listener");
    QObject::connect(&listener, &QTcpServer::newConnection, [&] {
        QTcpSocket *socket = listener.nextPendingConnection();
        auto *buffer = new QByteArray;
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [=, &server] {
            buffer->append(socket->readAll());
            if (buffer->size() < 4)
                return;
            const quint32 length = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar *>(buffer->constData()));
            if (buffer->size() < qsizetype(length) + 4)
                return;
            const QByteArray first = buffer->mid(4, length);
            const QByteArray rest = buffer->mid(4 + length);
            delete buffer;
            socket->disconnect();
            server.adopt(socket, first, rest);
        });
    });

    {
        TestClient phone(accountPhone);
        check(phone.connectTo(listener.serverPort()), "account phone connects");
        check(waitFor([&] { return !phone.event("session").isEmpty(); }),
              "account phone gets a session");
        const QJsonObject session = phone.event("session").value("data").toObject();
        check(session.value("paired").toBool(), "same-account phone paired without a code");
        check(!session.contains("code"), "no code shown to a same-account phone");
        check(prompted.isEmpty(), "no prompt for a same-account phone");
        check(server.isPaired(accountPhone.publicKey), "same-account phone remembered");
    }

    {
        TestClient phone(otherPhone);
        check(phone.connectTo(listener.serverPort()), "other phone connects");
        check(waitFor([&] { return !phone.event("session").isEmpty(); }),
              "other phone gets a session");
        const QJsonObject session = phone.event("session").value("data").toObject();
        check(!session.value("paired").toBool(), "other-account phone still pending");
        check(session.value("code").toString() == phone.code, "other phone shown the code");
        check(prompted == QStringList{otherPhone.publicKey}, "desktop prompts for it");
        check(!server.isPaired(otherPhone.publicKey), "other phone not remembered");
    }

    asked.clear();
    {
        TestClient phone(accountPhone);
        check(phone.connectTo(listener.serverPort()), "account phone reconnects");
        check(waitFor([&] { return !phone.event("session").isEmpty(); }),
              "reconnect session");
        check(phone.event("session").value("data").toObject().value("paired").toBool(),
              "reconnect paired");
        check(asked == QStringList{accountPhone.publicKey},
              "a phone approved through the account is checked again");
    }

    const auto deliver = [&](RelayTestClient &phone) {
        for (int i = 0; i < 3; ++i) {
            phone.fromDesktop.append(outbox);
            outbox.clear();
            phone.pump();
        }
    };
    {
        RelayTestClient phone(accountRelayPhone);
        phone.session = QStringLiteral("relay-session-account-0001");
        phone.server = &server;
        phone.open();
        check(waitFor([&] {
                  deliver(phone);
                  return !phone.event("session").isEmpty();
              }),
              "relay account phone gets a session");
        check(phone.event("session").value("data").toObject().value("paired").toBool(),
              "same-account phone paired over the relay");
        check(server.isConnected(accountRelayPhone.publicKey), "relay phone connected");
    }
    {
        RelayTestClient phone(otherRelayPhone);
        phone.session = QStringLiteral("relay-session-other-0001");
        phone.server = &server;
        phone.open();
        deliver(phone);
        check(phone.event("pairing-denied").value("data").toObject()
                      .value("reason").toString() == "pair_on_lan",
              "other-account relay phone told to pair on the LAN");
        check(phone.closed, "other-account relay channel closed");
        check(!server.isPaired(otherRelayPhone.publicKey), "other relay phone not remembered");
    }
    check(prompted == QStringList{otherPhone.publicKey}, "no prompt over the relay");

    SigningKey browserKey;
    QSet<QString> browserAccountKeys = accountKeys;
    browserAccountKeys.insert(browserKey.publicKey);
    server.accountDeviceCheck = [&](const LocalControlServer::Device &device,
                                    std::function<void(Verdict)> done) {
        asked.append(device.key);
        done(browserAccountKeys.contains(device.key) ? Verdict::Owned
                                                     : Verdict::NotOwned);
    };
    for (int round = 0; round < 2; ++round) {
        asked.clear();
        RelayTestClient browser(browserKey);
        browser.platform = QStringLiteral("web");
        browser.session = QStringLiteral("relay-session-browser-000%1").arg(round);
        browser.server = &server;
        browser.open();
        check(waitFor([&] {
                  deliver(browser);
                  return !browser.event("session").isEmpty();
              }),
              "account browser gets a session");
        check(browser.event("session").value("data").toObject().value("paired").toBool(),
              "same-account browser paired over the relay");
        check(asked == QStringList{browserKey.publicKey},
              "a browser is checked against the account on every connect");
        check(!server.isPaired(browserKey.publicKey), "a browser is never remembered");
        server.relayFrame({{"type", "control"}, {"action", "close"},
                           {"session", browser.session}, {"node", "desk"},
                           {"from", "browser"}});
    }
    browserAccountKeys.remove(browserKey.publicKey);
    {
        RelayTestClient browser(browserKey);
        browser.platform = QStringLiteral("web");
        browser.session = QStringLiteral("relay-session-browser-revoked");
        browser.server = &server;
        browser.open();
        deliver(browser);
        check(browser.closed, "a browser revoked from the account is refused");
    }
}


static void writeStore(const QString &path, const QJsonArray &devices)
{
    QFile file(path);
    file.open(QIODevice::WriteOnly);
    file.write(QJsonDocument(QJsonObject{{"devices", devices}}).toJson());
}

static QJsonObject storedRecord(const QString &path, const QString &key)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    for (const QJsonValue &value :
         QJsonDocument::fromJson(file.readAll()).object().value("devices").toArray())
        if (value.toObject().value("key").toString() == key)
            return value.toObject();
    return {};
}

static void testCodePairedUserStays()
{
    QTemporaryDir dir;
    SigningKey desktopKey;
    SigningKey phoneKey;
    const QString store = dir.filePath("paired.json");
    writeStore(store, QJsonArray{QJsonObject{
        {"key", phoneKey.publicKey}, {"name", "Guest phone"},
        {"user", "bob"}, {"pairedAtMs", 1.0}, {"approval", "code"}}});
    QList<QJsonObject> outbox;
    LocalControlServer server(
        desktopKey.publicKey,
        [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server.setDesktopInfo("desk", "alice");
    server.setDeviceStorePath(store);
    server.setRelaySink(
        [&](const QJsonObject &f) {
            outbox.append(f);
            return true;
        },
        "desk");
    RelayTestClient phone(phoneKey);
    phone.session = QStringLiteral("relay-session-owner-claim-1");
    phone.server = &server;
    phone.open();
    for (int i = 0; i < 3; ++i) {
        phone.fromDesktop.append(outbox);
        outbox.clear();
        phone.pump();
    }
    check(server.isConnected(phoneKey.publicKey), "a code-paired phone reconnects");
    check(storedRecord(store, phoneKey.publicKey).value("user").toString() == "bob",
          "a code-paired phone cannot rename its user to the owner");
}

static void testCodePairedGuestSignsIn()
{
    QTemporaryDir dir;
    SigningKey desktopKey;
    SigningKey phoneKey;
    const QString store = dir.filePath("paired.json");
    writeStore(store, QJsonArray{QJsonObject{
        {"key", phoneKey.publicKey}, {"name", "Guest 2307"},
        {"user", ""}, {"pairedAtMs", 1.0}, {"approval", "code"}}});
    QList<QJsonObject> outbox;
    LocalControlServer server(
        desktopKey.publicKey,
        [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server.setDesktopInfo("desk", "alice");
    server.setDeviceStorePath(store);
    server.setRelaySink([&](const QJsonObject &f) { outbox.append(f); return true; },
                        "desk");
    using Verdict = LocalControlServer::AccountCheck;
    Verdict verdict = Verdict::NotOwned;
    QStringList asked;
    server.accountDeviceCheck = [&](const LocalControlServer::Device &device,
                                    std::function<void(Verdict)> done) {
        asked.append(device.key);
        done(verdict);
    };
    const auto dial = [&](const QString &session) {
        auto *phone = new RelayTestClient(phoneKey);
        phone->session = session;
        phone->server = &server;
        phone->open();
        for (int i = 0; i < 3; ++i) {
            phone->fromDesktop.append(outbox);
            outbox.clear();
            phone->pump();
        }
        return phone;
    };
    const auto connectedUser = [&]() {
        for (const LocalControlServer::Device &device : server.connectedDevices())
            if (device.key == phoneKey.publicKey)
                return device.user;
        return QString();
    };

    std::unique_ptr<RelayTestClient> phone(dial("relay-session-guest-signin-1"));
    check(asked == QStringList{phoneKey.publicKey},
          "a code-paired phone naming a new account is checked with the account");
    check(server.isConnected(phoneKey.publicKey),
          "and still connects when the account does not list it");
    check(storedRecord(store, phoneKey.publicKey).value("user").toString().isEmpty(),
          "an unconfirmed account name is not recorded");
    check(!forkmesh::localcontrol::hosts::ownerMayManageHosts(connectedUser(), "alice"),
          "so it cannot manage the desktop's hosts");

    verdict = Verdict::Unreachable;
    phone.reset(dial("relay-session-guest-signin-2"));
    check(server.isConnected(phoneKey.publicKey) &&
              storedRecord(store, phoneKey.publicKey).value("user").toString().isEmpty(),
          "an account that cannot be asked leaves the phone as it was");

    verdict = Verdict::Owned;
    phone.reset(dial("relay-session-guest-signin-3"));
    check(server.isConnected(phoneKey.publicKey),
          "a code-paired phone the account lists connects");
    check(storedRecord(store, phoneKey.publicKey).value("user").toString() == "alice",
          "and its signed-in account is recorded");
    check(storedRecord(store, phoneKey.publicKey).value("approval").toString() == "code",
          "while it stays approved in person");
    check(forkmesh::localcontrol::hosts::ownerMayManageHosts(connectedUser(), "alice"),
          "so the signed-in owner can manage the desktop's hosts");

    asked.clear();
    phone.reset(dial("relay-session-guest-signin-4"));
    check(server.isConnected(phoneKey.publicKey) && asked.isEmpty(),
          "a confirmed account is not asked about again");
}

static void testRelayResilience()
{
    QTemporaryDir dir;
    SigningKey desktopKey;
    SigningKey phoneKey;
    const QString store = dir.filePath("paired.json");
    writeStore(store, QJsonArray{QJsonObject{
        {"key", phoneKey.publicKey}, {"name", "Away phone"},
        {"user", "alice"}, {"pairedAtMs", 1.0}, {"approval", "code"}}});
    QList<QJsonObject> outbox;
    bool relayUp = true;
    LocalControlServer server(
        desktopKey.publicKey,
        [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server.setDesktopInfo("desk", "alice");
    server.setDeviceStorePath(store);
    server.setRelaySink(
        [&](const QJsonObject &f) {
            if (!relayUp)
                return false;
            outbox.append(f);
            return true;
        },
        "desk");
    server.onRequest = [&](int conn, const LocalControlServer::Device &, int id,
                           const QString &method, const QJsonObject &) {
        server.respond(conn, id, {{"method", method}});
    };
    const auto deliver = [&](RelayTestClient &phone) {
        for (int i = 0; i < 3; ++i) {
            phone.fromDesktop.append(outbox);
            outbox.clear();
            phone.pump();
        }
    };
    const auto closesFor = [&](const QString &session) {
        QStringList reasons;
        for (const QJsonObject &f : std::as_const(outbox))
            if (f.value("session").toString() == session &&
                f.value("action").toString() == "close")
                reasons.append(f.value("reason").toString());
        return reasons;
    };

    {
        RelayTestClient stale(phoneKey);
        stale.session = QStringLiteral("relay-session-stale-00001");
        stale.server = &server;
        server.relayFrame(stale.chunkFrame("data", "x"));
        check(closesFor(stale.session) == QStringList{"channel_gone"},
              "a frame for a channel the desktop no longer has is answered");
        server.relayFrame(stale.chunkFrame("data", "y"));
        check(closesFor(stale.session).size() == 1,
              "a dead channel is told only once");
        outbox.clear();
    }

    RelayTestClient phone(phoneKey);
    phone.session = QStringLiteral("relay-session-resilient-01");
    phone.server = &server;
    phone.open();
    deliver(phone);
    check(phone.event("session").value("data").toObject().value("paired").toBool(),
          "code-paired phone over the relay");
    check(server.isConnected(phoneKey.publicKey), "relay phone connected");

    relayUp = false;
    server.broadcast("agents.changed");
    QCoreApplication::processEvents();
    check(!server.isConnected(phoneKey.publicKey),
          "a frame the relay could not take ends the channel");
    relayUp = true;
    check(closesFor(phone.session).isEmpty(), "no close while the relay is down");
    server.relayRestored();
    check(closesFor(phone.session) == QStringList{"relay_reconnected"},
          "the phone is told its channel ended once the relay is back");
    server.relayRestored();
    check(closesFor(phone.session).size() == 1, "told once");
    outbox.clear();

    RelayTestClient idle(phoneKey);
    idle.session = QStringLiteral("relay-session-idle-000001");
    idle.server = &server;
    idle.open();
    deliver(idle);
    check(server.isConnected(phoneKey.publicKey), "idle phone connected");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    server.maintain(now + 60 * 1000);
    check(server.isConnected(phoneKey.publicKey), "a quiet minute keeps the channel");
    server.maintain(now + 16 * 60 * 1000);
    check(!server.isConnected(phoneKey.publicKey),
          "a relayed channel silent for 15 minutes is closed");
    check(closesFor(idle.session) == QStringList{"idle"}, "the phone is told why");
    outbox.clear();

    for (int i = 0; i < 16; ++i) {
        RelayTestClient opener(phoneKey);
        opener.session = QStringLiteral("relay-session-half-open-%1").arg(i, 2, 10, QChar('0'));
        opener.server = &server;
        server.relayFrame(opener.chunkFrame("open", "{", true));
    }
    check(outbox.isEmpty(), "half-open channels wait for the rest of the hello");
    RelayTestClient extra(phoneKey);
    extra.session = QStringLiteral("relay-session-half-open-xx");
    extra.server = &server;
    server.relayFrame(extra.chunkFrame("open", "{", true));
    check(closesFor(extra.session).isEmpty() && !outbox.isEmpty() &&
              outbox.last().value("action").toString() == "error",
          "too many half-open channels are refused");
    outbox.clear();
    server.maintain(QDateTime::currentMSecsSinceEpoch() + 31 * 1000);
    RelayTestClient after(phoneKey);
    after.session = QStringLiteral("relay-session-after-expiry");
    after.server = &server;
    after.open();
    deliver(after);
    check(server.isConnected(phoneKey.publicKey),
          "half-open channels expire and make room");
}

static void testAccountRevocation()
{
    QTemporaryDir dir;
    SigningKey desktopKey;
    SigningKey accountPhone;
    SigningKey legacyPhone;
    SigningKey codePhone;
    const QString store = dir.filePath("paired.json");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    writeStore(store, QJsonArray{
        QJsonObject{{"key", accountPhone.publicKey}, {"name", "Account phone"},
                    {"pairedAtMs", 1.0}, {"approval", "account"},
                    {"accountCheckedMs", double(now - 60 * 60 * 1000)}},
        QJsonObject{{"key", legacyPhone.publicKey}, {"name", "Legacy phone"},
                    {"pairedAtMs", 2.0}},
        QJsonObject{{"key", codePhone.publicKey}, {"name", "Code phone"},
                    {"user", "alice"}, {"pairedAtMs", 3.0}, {"approval", "code"}}});
    QList<QJsonObject> outbox;
    LocalControlServer server(
        desktopKey.publicKey,
        [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server.setDesktopInfo("desk", "alice");
    server.setDeviceStorePath(store);
    server.setRelaySink([&](const QJsonObject &f) { outbox.append(f); return true; },
                        "desk");
    using Verdict = LocalControlServer::AccountCheck;
    QHash<QString, Verdict> verdicts;
    QStringList asked;
    server.accountDeviceCheck = [&](const LocalControlServer::Device &device,
                                    std::function<void(Verdict)> done) {
        asked.append(device.key);
        done(verdicts.value(device.key, Verdict::NotOwned));
    };
    const auto deliver = [&](RelayTestClient &phone) {
        for (int i = 0; i < 3; ++i) {
            phone.fromDesktop.append(outbox);
            outbox.clear();
            phone.pump();
        }
    };
    const auto dial = [&](const SigningKey &key, const QString &session) {
        auto *phone = new RelayTestClient(key);
        phone->session = session;
        phone->server = &server;
        phone->open();
        deliver(*phone);
        return phone;
    };
    const auto deniedReason = [](const RelayTestClient &phone) {
        return phone.event("pairing-denied").value("data").toObject()
            .value("reason").toString();
    };

    verdicts.insert(accountPhone.publicKey, Verdict::Unreachable);
    std::unique_ptr<RelayTestClient> phone(
        dial(accountPhone, "relay-session-grace-000001"));
    check(server.isConnected(accountPhone.publicKey),
          "an account check that cannot be made admits a phone confirmed today");

    verdicts.insert(legacyPhone.publicKey, Verdict::Unreachable);
    std::unique_ptr<RelayTestClient> legacy(
        dial(legacyPhone, "relay-session-legacy-00001"));
    check(deniedReason(*legacy) == "account_check_failed" && legacy->closed,
          "a never-confirmed phone is refused over the relay when the account "
          "cannot be asked");
    check(server.isPaired(legacyPhone.publicKey), "and is not forgotten");

    verdicts.insert(legacyPhone.publicKey, Verdict::Owned);
    legacy.reset(dial(legacyPhone, "relay-session-legacy-00002"));
    check(server.isConnected(legacyPhone.publicKey), "a confirmed legacy phone connects");
    check(storedRecord(store, legacyPhone.publicKey).value("approval").toString() ==
              "account",
          "and is now held by the account");

    asked.clear();
    std::unique_ptr<RelayTestClient> code(dial(codePhone, "relay-session-code-0000001"));
    check(server.isConnected(codePhone.publicKey) && asked.isEmpty(),
          "a phone approved in person is not asked about");

    verdicts.insert(accountPhone.publicKey, Verdict::Owned);
    verdicts.insert(legacyPhone.publicKey, Verdict::NotOwned);
    asked.clear();
    server.maintain(QDateTime::currentMSecsSinceEpoch());
    check(asked == QStringList{accountPhone.publicKey},
          "a phone admitted on grace is rechecked at the next sweep, a confirmed "
          "one only after ten minutes");
    asked.clear();
    phone->send({{"id", 1}, {"method", "ping"}});
    legacy->send({{"id", 1}, {"method", "ping"}});
    code->send({{"id", 1}, {"method", "ping"}});
    server.maintain(QDateTime::currentMSecsSinceEpoch() + 11 * 60 * 1000);
    check(asked.contains(accountPhone.publicKey) &&
              asked.contains(legacyPhone.publicKey) &&
              !asked.contains(codePhone.publicKey),
          "account-held channels are rechecked every ten minutes");
    check(server.isConnected(accountPhone.publicKey), "a phone still on the account stays");
    check(!server.isConnected(legacyPhone.publicKey),
          "a phone removed from the account is cut off");
    check(!server.isPaired(legacyPhone.publicKey), "and forgotten");
    bool revokedTold = false;
    for (const QJsonObject &f : std::as_const(outbox))
        if (f.value("session").toString() == legacy->session &&
            f.value("reason").toString() == "revoked")
            revokedTold = true;
    check(revokedTold, "the phone is told it was revoked");
    check(server.isConnected(codePhone.publicKey), "the in-person phone is untouched");
    outbox.clear();

    verdicts.insert(accountPhone.publicKey, Verdict::NotOwned);
    std::unique_ptr<RelayTestClient> again(
        dial(accountPhone, "relay-session-revoked-0001"));
    check(deniedReason(*again) == "pair_on_lan" && again->closed,
          "a revoked phone cannot reconnect over the relay");
    check(!server.isPaired(accountPhone.publicKey), "the revoked phone is forgotten");
}

static void testLanAdmitsBeforeAccountCheck()
{
    QTemporaryDir dir;
    SigningKey desktopKey;
    SigningKey freshPhone;
    SigningKey stalePhone;
    const QString store = dir.filePath("paired.json");
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    writeStore(store, QJsonArray{
        QJsonObject{{"key", freshPhone.publicKey}, {"name", "Fresh phone"},
                    {"pairedAtMs", 1.0}, {"approval", "account"},
                    {"accountCheckedMs", double(now - 60 * 60 * 1000)}},
        QJsonObject{{"key", stalePhone.publicKey}, {"name", "Stale phone"},
                    {"pairedAtMs", 2.0}, {"approval", "account"},
                    {"accountCheckedMs", double(now - 25ll * 60 * 60 * 1000)}}});
    LocalControlServer server(
        desktopKey.publicKey,
        [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server.setDesktopInfo("desk", "alice");
    server.setDeviceStorePath(store);
    using Verdict = LocalControlServer::AccountCheck;
    QHash<QString, std::function<void(Verdict)>> pending;
    server.accountDeviceCheck = [&](const LocalControlServer::Device &device,
                                    std::function<void(Verdict)> done) {
        pending.insert(device.key, done);
    };

    QTcpServer listener;
    check(listener.listen(QHostAddress::LocalHost, 0), "fast-admit listener");
    QObject::connect(&listener, &QTcpServer::newConnection, [&] {
        QTcpSocket *socket = listener.nextPendingConnection();
        auto *buffer = new QByteArray;
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [=, &server] {
            buffer->append(socket->readAll());
            if (buffer->size() < 4)
                return;
            const quint32 length = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar *>(buffer->constData()));
            if (buffer->size() < qsizetype(length) + 4)
                return;
            const QByteArray first = buffer->mid(4, length);
            const QByteArray rest = buffer->mid(4 + length);
            delete buffer;
            socket->disconnect();
            server.adopt(socket, first, rest);
        });
    });

    {
        TestClient phone(freshPhone);
        check(phone.connectTo(listener.serverPort()), "fresh phone connects");
        check(waitFor([&] { return !phone.event("session").isEmpty(); }),
              "fresh phone gets a session while the account is still being asked");
        check(phone.event("session").value("data").toObject().value("paired").toBool(),
              "a phone the account confirmed today is admitted on the LAN at once");
        check(pending.contains(freshPhone.publicKey),
              "and the account is still asked in the background");
        if (pending.contains(freshPhone.publicKey))
            pending.take(freshPhone.publicKey)(Verdict::NotOwned);
        check(waitFor([&] { return !server.isConnected(freshPhone.publicKey); }),
              "a phone the account no longer lists is cut off when the answer lands");
        check(!server.isPaired(freshPhone.publicKey), "and forgotten");
    }

    {
        TestClient phone(stalePhone);
        check(phone.connectTo(listener.serverPort()), "stale phone connects");
        check(waitFor([&] { return pending.contains(stalePhone.publicKey); }),
              "a phone not confirmed for a day is asked about first");
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
        check(phone.event("session").isEmpty(), "and waits for the answer");
        if (pending.contains(stalePhone.publicKey))
            pending.take(stalePhone.publicKey)(Verdict::Owned);
        check(waitFor([&] { return !phone.event("session").isEmpty(); }),
              "stale phone admitted once confirmed");
        check(phone.event("session").value("data").toObject().value("paired").toBool(),
              "stale phone paired after the check");
    }
}

static void testHostPolicy()
{
    namespace hc = forkmesh::localcontrol::hosts;
    // Owner only: an approved phone that names this desktop's account.
    check(hc::ownerMayManageHosts("Alice", "alice"), "owner matches, any case");
    check(!hc::ownerMayManageHosts("bob", "alice"), "another account refused");
    check(!hc::ownerMayManageHosts("", "alice"), "anonymous phone refused");
    check(!hc::ownerMayManageHosts("alice", ""), "desktop without account refuses");

    check(hc::isMutatingAction("update") && hc::isMutatingAction("destroy") &&
              hc::isMutatingAction("install") && hc::isMutatingAction("stop"),
          "changes need confirmation");
    check(!hc::isMutatingAction("list") && !hc::isMutatingAction("rm -rf"),
          "reads and unknown verbs are not actions");
    check(hc::isDestructiveAction("destroy") && !hc::isDestructiveAction("restart"),
          "only destroy is destructive");
    check(hc::actionSummary("destroy", "mirror35").contains("cannot be undone"),
          "destroy summary warns");
    check(hc::typedNameMatches(" mirror35 ", "mirror35"), "typed name trims");
    check(!hc::typedNameMatches("MIRROR35", "mirror35"), "typed name is exact");
    check(!hc::typedNameMatches("", ""), "empty never confirms");
    check(hc::validHostName("mirror-35.example") && !hc::validHostName("a b") &&
              !hc::validHostName("-x") && !hc::validHostName(QString(64, 'a')),
          "host name alphabet");

    // Confirmation tokens: one use, bound, a minute long.
    hc::ConfirmationStore store;
    const qint64 t0 = 1000000;
    const QString token = store.issue("dev", "restart", "mirror35", t0);
    check(store.consume(token, "dev", "restart", "MIRROR35", t0 + 10) ==
              hc::ConfirmationStore::Result::Ok,
          "token confirms its own action");
    check(store.consume(token, "dev", "restart", "mirror35", t0 + 20) ==
              hc::ConfirmationStore::Result::Missing,
          "token is single use");
    const QString other = store.issue("dev", "restart", "mirror35", t0);
    check(store.consume(other, "dev", "destroy", "mirror35", t0) ==
              hc::ConfirmationStore::Result::Mismatch,
          "token bound to the action");
    check(store.consume(other, "dev", "restart", "mirror35", t0) ==
              hc::ConfirmationStore::Result::Missing,
          "a wrong guess spends the token");
    const QString stolen = store.issue("dev", "stop", "mirror35", t0);
    check(store.consume(stolen, "other-dev", "stop", "mirror35", t0) ==
              hc::ConfirmationStore::Result::Mismatch,
          "token bound to the device");
    const QString stale = store.issue("dev", "stop", "mirror35", t0);
    check(store.consume(stale, "dev", "stop", "mirror35",
                        t0 + hc::ConfirmationStore::kLifetimeMs) ==
              hc::ConfirmationStore::Result::Expired,
          "token expires");
    for (int i = 0; i < 100; ++i)
        store.issue("dev", "update", QStringLiteral("h%1").arg(i), t0);
    check(store.outstanding() <= hc::ConfirmationStore::kMaxOutstanding,
          "outstanding tokens bounded");
    check(hc::confirmationError(hc::ConfirmationStore::Result::Expired) ==
              "confirmation_expired",
          "error code");

    // Health from the Hosts page's own probe labels.
    check(hc::healthFromProbeLabel(QString::fromUtf8("\xE2\x97\x8F Online \xC2\xB7 Mirror node")) ==
              "online",
          "online");
    check(hc::healthFromProbeLabel(QString::fromUtf8("\xE2\x97\x8F Attention \xC2\xB7 Mirror node stopped")) ==
              "attention",
          "attention");
    check(hc::healthFromProbeLabel(QString::fromUtf8("\xE2\x97\x8F Online \xC2\xB7 ForkMesh missing")) ==
              "attention",
          "missing binary needs attention");
    check(hc::healthFromProbeLabel(QString::fromUtf8("\xE2\x97\x8F Offline \xC2\xB7 unreachable")) ==
              "offline",
          "offline");
    check(hc::healthFromProbeLabel(QString::fromUtf8("\xE2\x97\x8C Provisioning \xC2\xB7 waiting for SSH")) ==
              "provisioning",
          "provisioning");
    check(hc::healthFromProbeLabel("") == "unknown", "unprobed");

    const QJsonObject vultr = hc::hostRow(
        QJsonObject{{"name", "mirror35"}, {"ip", "203.0.113.5"}, {"user", "root"},
                    {"status", "installed"}, {"provider", "Vultr"},
                    {"region", "ewr"}, {"planType", "vc2-1c-1gb"},
                    {"monthlyCost", 5}},
        "", "0.8.6", true);
    check(vultr.value("health").toString() == "online", "roster fills an unprobed host");
    check(vultr.value("version").toString() == "0.8.6", "version carried");
    check(vultr.value("actions").toArray().contains("destroy") &&
              vultr.value("actions").toArray().contains("restart"),
          "Vultr mirror offers the service verbs and destroy");
    check(!vultr.contains("password") && !vultr.contains("identityFile"),
          "no credential in a row");
    const QJsonObject plain = hc::hostRow(
        QJsonObject{{"name", "box"}, {"ip", "10.0.0.2"}, {"user", "pi"},
                    {"status", "installing"}, {"identityFile", "/secret"}},
        QString::fromUtf8("\xE2\x97\x8F Online"), "", false);
    check(plain.value("health").toString() == "provisioning", "installing wins");
#if defined(Q_OS_WIN)
    check(plain.value("actions").toArray() == QJsonArray({"update", "installBinary"}),
          "a plain SSH host gets the installer only");
#else
    check(plain.value("actions").toArray() == QJsonArray({"update", "installBinary", "terminal"}),
          "a plain SSH host gets the installer and a shell, no service verbs");
    check(vultr.value("actions").toArray().contains("terminal"),
          "a mirror node offers a shell");
#endif
    check(hc::isMutatingAction("terminal") && !hc::isDestructiveAction("terminal"),
          "a shell needs a fresh confirmation but no typed name");
    check(hc::actionSummary("terminal", "box").contains("SSH key stays on the desktop"),
          "the shell summary says where the key is");
    check(hc::isMutatingAction("installBinary") &&
              hc::actionSummary("installBinary", "box").contains("binary"),
          "uploading the desktop's binary is a confirmed change");
    check(!plain.contains("identityFile"), "identity file never listed");

    // The log stream: batched, windowed, bounded.
    hc::LogStream stream;
    check(stream.takeBatch().isEmpty(), "nothing to send");
    for (int i = 0; i < 450; ++i)
        stream.push(i, "mirror", QStringLiteral("line %1").arg(i));
    const QJsonObject first = stream.takeBatch();
    check(first.value("seq").toInt() == 1 &&
              first.value("lines").toArray().size() == hc::LogStream::kMaxBatchLines,
          "first batch capped");
    check(first.value("lines").toArray().first().toObject().value("src").toString() ==
              "mirror",
          "source carried");
    stream.takeBatch();
    stream.takeBatch();
    check(stream.inFlight() == 3 && !stream.hasPending(), "all sent");
    for (int i = 0; i < 10; ++i)
        stream.push(i, "node", "x");
    stream.takeBatch();
    check(!stream.canSend() && stream.takeBatch().isEmpty(), "window full waits for acks");
    stream.ack(2);
    check(stream.inFlight() == 2 && stream.canSend(), "ack opens the window");
    stream.ack(1);
    check(stream.inFlight() == 2, "stale ack ignored");
    stream.ack(99);
    check(stream.inFlight() == 2, "ack beyond what was sent ignored");
    // A phone that stops acknowledging keeps only the newest lines.
    stream.ack(4);
    for (int i = 0; i < hc::LogStream::kMaxBufferedLines + 500; ++i)
        stream.push(i, "system", QString(3000, 'y'));
    check(stream.buffered() == hc::LogStream::kMaxBufferedLines, "buffer capped");
    const QJsonObject lagged = stream.takeBatch();
    check(lagged.value("dropped").toInt() == 500, "dropped lines counted");
    check(lagged.value("lines").toArray().first().toObject().value("text").toString().size() ==
              hc::LogStream::kMaxLineChars,
          "long lines cut");
    check(stream.takeBatch().value("dropped").toInt() == 0, "dropped reported once");
}

static QString readWholeLog(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(file.readAll());
}

static void appendBytes(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    if (file.open(QIODevice::Append))
        file.write(bytes);
}

static void testStream()
{
    QTemporaryDir dir;
    const QString path = dir.filePath(QStringLiteral("transcript.txt"));
    LogTail tail;
    bool rewritten = true;
    check(readLogGrowth(path, tail, &rewritten).isEmpty() && !rewritten && tail.bytes == 0,
          "a missing log reads empty");

    appendBytes(path, QByteArray("hello\r\nwor") + char(0xC3));
    QString streamed = readLogGrowth(path, tail, &rewritten);
    check(streamed == QStringLiteral("hello\nwor"), "growth drops \\r and waits for a split character");
    check(tail.bytes == 10 && tail.chars == 9, "tail stops before the split character");

    appendBytes(path, QByteArray(1, char(0xA9)) + "ld \xF0\x9F\x98\x80\n");
    streamed += readLogGrowth(path, tail, &rewritten);
    check(!rewritten, "an appended log is not rewritten");
    check(streamed == readWholeLog(path), "streamed growth equals a whole read");
    check(tail.chars == readWholeLog(path).size(), "the char cursor matches a whole read");
    check(readLogGrowth(path, tail, &rewritten).isEmpty(), "no growth reads empty");

    QFile rewrite(path);
    rewrite.open(QIODevice::WriteOnly | QIODevice::Truncate);
    rewrite.write("new\n");
    rewrite.close();
    const QString again = readLogGrowth(path, tail, &rewritten);
    check(rewritten && again == QStringLiteral("new\n") && tail.chars == 4,
          "a shorter log is read again from the start");

    SendBudget budget;
    check(budget.limit() == 180, "the default budget keeps a tenth of 200 spare");
    for (int i = 0; i < 180; ++i) {
        check(budget.waitMs(1000 + i) == 0, "frames inside the budget go at once");
        budget.spend(1000 + i);
    }
    check(budget.waitMs(1200) == 9800, "a full budget waits for the oldest frame to age out");
    check(budget.waitMs(11000) == 0, "the window slides");
    budget.configure(10000, 1000);
    check(budget.limit() == 900, "the relay's advertised budget is honoured");
    budget.reset();
    check(budget.waitMs(0) == 0, "reset empties the window");
    check(followIntervalMs(false) < followIntervalMs(true), "the LAN streams more often than the relay");
}

static bool waitingDevice(const LocalControlServer &server, const QString &key)
{
    for (const LocalControlServer::Device &device : server.connectedDevices())
        if (device.key == key)
            return true;
    return false;
}

static void testPendingPairingExpires()
{
    QTemporaryDir dir;
    SigningKey desktopKey;
    SigningKey idlePhone;
    SigningKey approvedPhone;
    const QString store = dir.filePath("paired.json");
    LocalControlServer server(
        desktopKey.publicKey,
        [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server.setDesktopInfo("desk", "alice");
    server.setDeviceStorePath(store);
    int pairingConnection = 0;
    QList<int> closedPrompts;
    server.onPairingRequested = [&](int id, const LocalControlServer::Device &,
                                    const QString &) { pairingConnection = id; };
    server.onPairingClosed = [&](int id) { closedPrompts.append(id); };

    QTcpServer listener;
    check(listener.listen(QHostAddress::LocalHost, 0), "expiry listener");
    QObject::connect(&listener, &QTcpServer::newConnection, [&] {
        QTcpSocket *socket = listener.nextPendingConnection();
        auto *buffer = new QByteArray;
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [=, &server] {
            buffer->append(socket->readAll());
            if (buffer->size() < 4)
                return;
            const quint32 length = qFromBigEndian<quint32>(
                reinterpret_cast<const uchar *>(buffer->constData()));
            if (buffer->size() < qsizetype(length) + 4)
                return;
            const QByteArray first = buffer->mid(4, length);
            const QByteArray rest = buffer->mid(4 + length);
            delete buffer;
            socket->disconnect();
            server.adopt(socket, first, rest);
        });
    });

    {
        TestClient phone(idlePhone);
        check(phone.connectTo(listener.serverPort()), "idle phone connects");
        check(waitFor([&] { return pairingConnection != 0; }), "idle phone prompts");
        const int idleConnection = pairingConnection;
        server.maintain(QDateTime::currentMSecsSinceEpoch() + 60 * 1000);
        check(waitingDevice(server, idlePhone.publicKey) &&
                  !phone.event("session").isEmpty() &&
                  phone.event("pairing-denied").isEmpty(),
              "a pairing prompt stays open for its first minute");
        server.maintain(QDateTime::currentMSecsSinceEpoch() + 2 * 60 * 1000 + 1000);
        check(waitFor([&] { return !phone.event("pairing-denied").isEmpty(); }),
              "an unanswered pairing is refused");
        check(phone.event("pairing-denied").value("data").toObject()
                      .value("reason").toString() == "expired",
              "the phone is told the pairing expired");
        check(closedPrompts.contains(idleConnection),
              "the desktop prompt closes when the pairing expires");
        check(!waitingDevice(server, idlePhone.publicKey) &&
                  !server.isPaired(idlePhone.publicKey),
              "an expired pairing frees its slot and pairs nothing");
        server.approvePairing(idleConnection);
        check(!server.isPaired(idlePhone.publicKey),
              "a late Allow on an expired prompt pairs nothing");
    }

    pairingConnection = 0;
    {
        TestClient phone(approvedPhone);
        check(phone.connectTo(listener.serverPort()), "second phone connects");
        check(waitFor([&] { return pairingConnection != 0; }), "second phone prompts");
        server.approvePairing(pairingConnection);
        check(server.isPaired(approvedPhone.publicKey), "second phone paired");
    }
#ifndef Q_OS_WIN
    const QFileDevice::Permissions shared =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ReadOther |
        QFileDevice::WriteOther;
    check(QFileInfo::exists(store) && !(QFileInfo(store).permissions() & shared),
          "the paired devices file is readable by its owner only");
#endif
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testPrimitives();
    testLoopback();
    testPendingPairingExpires();
    testRelay();
    testAccountDevices();
    testRelayResilience();
    testCodePairedUserStays();
    testCodePairedGuestSignsIn();
    testAccountRevocation();
    testLanAdmitsBeforeAccountCheck();
    testHostPolicy();
    testStream();
    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "local control tests passed\n";
    return 0;
}
