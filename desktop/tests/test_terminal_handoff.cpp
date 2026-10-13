#include "ClaudePeerMessaging.h"
#include "LocalControl.h"
#include "TerminalHandoff.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtEndian>

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <cstring>
#include <functional>
#include <iostream>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(Q_OS_MACOS)
#include <util.h>
#else
#include <pty.h>
#endif

#ifndef FORKMESH_TERM_BINARY
#error "FORKMESH_TERM_BINARY must name the built forkmesh-term"
#endif

using namespace forkmesh::localcontrol;

static int failures = 0;
static int checks = 0;

static void check(bool condition, const QString &what)
{
    ++checks;
    if (condition)
        return;
    std::cerr << "FAIL: " << what.toStdString() << "\n";
    ++failures;
}

static void same(const QString &got, const QString &want, const QString &what)
{
    ++checks;
    if (got == want)
        return;
    std::cerr << "FAIL: " << what.toStdString() << "\n  got:  " << got.toStdString()
              << "\n  want: " << want.toStdString() << "\n";
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
        EVP_PKEY_get_raw_public_key(key, reinterpret_cast<uchar *>(pub.data()), &length);
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
                       reinterpret_cast<const uchar *>(data.constData()), size_t(data.size()));
        EVP_MD_CTX_free(ctx);
        return toBase64Url(sig);
    }
};

static QByteArray lengthFrame(const QByteArray &payload)
{
    QByteArray out(4, '\0');
    qToBigEndian(quint32(payload.size()), reinterpret_cast<uchar *>(out.data()));
    return out + payload;
}

struct Phone {
    const SigningKey &identity;
    QString name;
    QTcpSocket socket;
    QByteArray buffer;
    SessionKeys keys;
    quint64 sendCounter = 0;
    quint64 receiveCounter = 0;
    bool welcomed = false;
    bool forgeAuth = false;
    QList<QJsonObject> messages;

    Phone(const SigningKey &key, const QString &label) : identity(key), name(label) {}

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
        const QJsonObject hello{{"t", helloTag()}, {"v", kProtocolVersion},
                                {"key", identity.publicKey}, {"eph", ephB64},
                                {"nonce", nonceB64}, {"name", name},
                                {"user", "jett"}, {"platform", "ios"}};
        socket.write(lengthFrame(QJsonDocument(hello).toJson(QJsonDocument::Compact)));
        QObject::connect(&socket, &QTcpSocket::readyRead, &socket, [this, eph, ephB64, nonceB64] {
            buffer.append(socket.readAll());
            while (buffer.size() >= 4) {
                const quint32 length =
                    qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer.constData()));
                if (buffer.size() < qsizetype(length) + 4)
                    return;
                const QByteArray payload = buffer.mid(4, length);
                buffer.remove(0, qsizetype(length) + 4);
                if (!welcomed) {
                    const QJsonObject w = QJsonDocument::fromJson(payload).object();
                    if (w.value("t").toString() != welcomeTag())
                        continue;
                    const QString serverKey = w.value("key").toString();
                    const QString serverEph = w.value("eph").toString();
                    const QByteArray t =
                        transcript(identity.publicKey, ephB64, nonceB64, serverKey, serverEph);
                    if (!verifyEd25519(serverKey, w.value("sig").toString(), serverSignedBytes(t)))
                        return;
                    keys = deriveKeys(x25519(eph.privateKey, fromBase64Url(serverEph, 32)), t);
                    welcomed = true;
                    const QString sig =
                        identity.sign(forgeAuth ? QByteArray("forged") : clientSignedBytes(t));
                    socket.write(lengthFrame(
                        QJsonDocument(QJsonObject{{"t", authTag()}, {"sig", sig}})
                            .toJson(QJsonDocument::Compact)));
                    continue;
                }
                messages.append(QJsonDocument::fromJson(
                                    openFrame(keys.serverToClient, receiveCounter++, payload))
                                    .object());
            }
        });
        return true;
    }

    void send(const QJsonObject &message)
    {
        socket.write(lengthFrame(sealFrame(keys.clientToServer, sendCounter++,
                                           QJsonDocument(message).toJson(QJsonDocument::Compact))));
    }

    QJsonObject response(int id) const
    {
        for (const QJsonObject &m : messages)
            if (m.value("id").toInt() == id)
                return m;
        return {};
    }

    bool paired() const
    {
        for (const QJsonObject &m : messages)
            if (m.value("event").toString() == "session" &&
                m.value("data").toObject().value("paired").toBool())
                return true;
        return false;
    }
};

struct Wrapped {
    pid_t pid = -1;
    int master = -1;
    QByteArray out;

    void pump(int ms = 20)
    {
        pollfd p{master, POLLIN, 0};
        if (::poll(&p, 1, ms) <= 0)
            return;
        char buf[4096];
        for (;;) {
            const ssize_t n = ::read(master, buf, sizeof buf);
            if (n <= 0)
                break;
            out.append(buf, n);
        }
    }

    bool until(const QByteArray &needle, int ms = 5000)
    {
        QElapsedTimer timer;
        timer.start();
        while (!out.contains(needle) && timer.elapsed() < ms)
            pump(50);
        return out.contains(needle);
    }

    QByteArray seen()
    {
        const qsizetype a = out.indexOf("BYTES:");
        const qsizetype b = out.indexOf(":END");
        if (a < 0 || b < a)
            return "(none)";
        QByteArray bytes = out.mid(a + 6, b - a - 6);
        for (qsizetype open = bytes.indexOf("\x1b" "7"); open >= 0; open = bytes.indexOf("\x1b" "7")) {
            const qsizetype close = bytes.indexOf("\x1b" "8", open);
            if (close < 0)
                break;
            bytes.remove(open, close + 2 - open);
        }
        return bytes;
    }

    void stop()
    {
        if (pid <= 0)
            return;
        ::kill(pid, SIGTERM);
        int status = 0;
        for (int i = 0; i < 100 && ::waitpid(pid, &status, WNOHANG) == 0; ++i)
            pump(20);
        ::kill(pid, SIGKILL);
        ::waitpid(pid, &status, WNOHANG);
        ::close(master);
        pid = -1;
    }
};

static Wrapped wrap(const QString &dir, const QByteArrayList &args)
{
    Wrapped w;
    w.pid = ::forkpty(&w.master, nullptr, nullptr, nullptr);
    if (w.pid == 0) {
        ::setenv("FORKMESH_TERM_DIR", QFile::encodeName(dir).constData(), 1);
        std::vector<char *> argv{const_cast<char *>(FORKMESH_TERM_BINARY)};
        for (const QByteArray &arg : args)
            argv.push_back(const_cast<char *>(arg.constData()));
        argv.push_back(nullptr);
        ::execv(FORKMESH_TERM_BINARY, argv.data());
        ::_exit(126);
    }
    ::fcntl(w.master, F_SETFL, ::fcntl(w.master, F_GETFL) | O_NONBLOCK);
    return w;
}

static QByteArray reader(int count, bool bracketed)
{
    return QByteArray(bracketed ? "printf '\\033[?2004h'; " : "") +
           "stty raw -echo; printf 'READY'; printf 'BYTES:'; dd bs=1 count=" +
           QByteArray::number(count) + " 2>/dev/null | od -An -tx1 | tr -d ' \\n'; printf ':END'";
}

static const TerminalHandoffSession *sessionFor(const QList<TerminalHandoffSession> &live, pid_t pid)
{
    for (const TerminalHandoffSession &s : live)
        if (s.pid == pid)
            return &s;
    return nullptr;
}

static bool writeFile(const QString &path, const QByteArray &data, QFile::Permissions perms)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    f.write(data);
    f.close();
    return f.setPermissions(perms);
}

static int bindSocket(const QString &path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    const QByteArray p = QFile::encodeName(path);
    std::memcpy(addr.sun_path, p.constData(), size_t(p.size()));
    ::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr);
    ::chmod(p.constData(), 0600);
    ::listen(fd, 1);
    return fd;
}

static void testPrimitives()
{
    same(TerminalHandoff::sanitizeText("hi\r\nthere\r\x1b[201~x\x07\u009b\n\n"), "hi\nthere\n[201~x",
         "text sanitised like the wrapper does");
    same(TerminalHandoff::sanitizeLabel("Jett's\niPhone\x01"), "Jett's iPhone", "label flattened");
    same(TerminalHandoff::sanitizeLabel(""), "a paired device", "empty label");
    same(QString::fromLatin1(TerminalHandoff::requestFrame("type", "Phone", "hello")),
         "FMTERM1 type 5 5\nPhonehello", "frame matches forkmesh-term's parser");
    same(QString::fromUtf8(TerminalHandoff::requestFrame("type", QString::fromUtf8("Jétt"), "\xe2\x8c\xa8")),
         QString::fromUtf8("FMTERM1 type 5 3\nJétt\xe2\x8c\xa8"), "lengths are UTF-8 bytes");
    const TerminalHandoffReply ok = TerminalHandoff::parseReply("ok typed paste=1");
    check(ok.ok && ok.detail == "typed paste=1", "ok reply");
    const TerminalHandoffReply err = TerminalHandoff::parseReply("err user_typing\n");
    check(!err.ok && err.code == "user_typing", "err reply");
    check(TerminalHandoff::parseReply("HTTP/1.1 200").code == "bad_reply", "foreign reply");
    same(TerminalHandoff::errorCode({false, "remote_off", {}}), "terminal_remote_off", "remote off code");
    same(TerminalHandoff::errorCode({false, "user_typing", {}}), "terminal_user_typing", "typing code");
    same(TerminalHandoff::errorCode({false, "rate_limited", {}}), "typing_rate_limited", "rate code");
    same(TerminalHandoff::errorCode({false, "too_long", {}}), "bad_task", "length code");
    same(TerminalHandoff::errorCode({false, "unreachable", {}}), "not_typeable", "gone code");
    same(TerminalHandoff::interruptKey("claude"), "esc", "claude stops on Esc");
    same(TerminalHandoff::interruptKey("codex"), "esc", "codex stops on Esc, since Ctrl-C quits an idle codex");
    same(TerminalHandoff::interruptKey("aider"), "ctrl-c", "other clis stop on Ctrl-C");
    same(TerminalHandoff::aliasLine("claude"), "alias claude='forkmesh term claude'", "alias");
    same(TerminalHandoff::launchCommand("codex"), "forkmesh term codex", "launch command");

    QHash<qint64, qint64> parents{{300, 200}, {200, 100}, {100, 1}, {50, 1}};
    const auto parentOf = [&parents](qint64 pid) { return parents.value(pid); };
    QList<TerminalHandoffSession> sessions;
    TerminalHandoffSession s;
    s.pid = 100;
    s.child = 150;
    sessions.append(s);
    check(TerminalHandoff::find(sessions, {150}, parentOf) == &sessions[0], "child pid matches");
    check(TerminalHandoff::find(sessions, {300}, parentOf) == &sessions[0],
          "a descendant of the wrapper matches");
    check(TerminalHandoff::find(sessions, {50}, parentOf) == nullptr, "unrelated pid does not");

    TerminalHandoff::TypingGate gate;
    same(TerminalHandoff::admit(gate, false, "k", 5, 0), "not_paired", "unpaired device refused");
    same(TerminalHandoff::admit(gate, true, QString(), 5, 0), "not_paired", "no device refused");
    same(TerminalHandoff::admit(gate, true, "k", TerminalHandoff::kMaxTextChars + 1, 0), "bad_task",
         "size cap");
    same(TerminalHandoff::admit(gate, true, "k", 5, 0), "", "first prompt admitted");
    same(TerminalHandoff::admit(gate, true, "k", 5, 500), "typing_rate_limited", "one per second");
    same(TerminalHandoff::admit(gate, true, "other", 5, 500), "", "limits are per device");
    int admitted = 1;
    for (int i = 1; i <= TerminalHandoff::kMaxTypedPerWindow + 3; ++i)
        if (TerminalHandoff::admit(gate, true, "k", 5, qint64(i) * 1500).isEmpty())
            ++admitted;
    same(QString::number(admitted), QString::number(TerminalHandoff::kMaxTypedPerWindow),
         "per-minute cap");
    same(TerminalHandoff::admit(gate, true, "k", 5, 200000), "", "window slides");
}

static void testDiscovery(const QString &root)
{
    const QString dir = root + "/disc";
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    const qint64 me = QCoreApplication::applicationPid();
    const QString start = ClaudePeerMessaging::procStart(me);
    const QString sock = QStringLiteral("%1/%2.sock").arg(dir).arg(me);
    const int fd = bindSocket(sock);
    const auto record = [&](qint64 pid, const QString &socket, const QString &st, bool remote) {
        return QJsonDocument(QJsonObject{{"v", 1}, {"pid", double(pid)}, {"start", st},
                                         {"child", double(me)}, {"cli", "claude"},
                                         {"cwd", "/w"}, {"socket", socket}, {"remote", remote},
                                         {"startedMs", 1.0}, {"tty", "/dev/pts/9"}})
            .toJson();
    };
    const QFile::Permissions owner = QFile::ReadOwner | QFile::WriteOwner;
    writeFile(QStringLiteral("%1/%2.json").arg(dir).arg(me), record(me, sock, start, true), owner);
    QList<TerminalHandoffSession> live = TerminalHandoff::liveSessions(dir);
    check(live.size() == 1 && live[0].pid == me && live[0].remote && live[0].cli == "claude" &&
              live[0].cwd == "/w",
          "a live registration is found");
    writeFile(QStringLiteral("%1/%2.json").arg(dir).arg(me), record(me, sock, start, false), owner);
    live = TerminalHandoff::liveSessions(dir);
    check(live.size() == 1 && !live[0].remote, "remote flag read");
    writeFile(QStringLiteral("%1/%2.json").arg(dir).arg(me), record(me, sock, start, true),
              owner | QFile::ReadOther);
    check(TerminalHandoff::liveSessions(dir).isEmpty(), "a registration other users can read is ignored");
    writeFile(QStringLiteral("%1/%2.json").arg(dir).arg(me), record(me, "/tmp/elsewhere.sock", start, true),
              owner);
    check(TerminalHandoff::liveSessions(dir).isEmpty(), "a socket outside the folder is ignored");
    writeFile(QStringLiteral("%1/%2.json").arg(dir).arg(me), record(me, sock, "1", true), owner);
    if (!start.isEmpty())
        check(TerminalHandoff::liveSessions(dir).isEmpty(), "a recycled pid is caught by its start time");
    writeFile(QStringLiteral("%1/%2.json").arg(dir).arg(me), record(me, sock, start, true), owner);
    writeFile(dir + "/123.json", record(me, sock, start, true), owner);
    check(TerminalHandoff::liveSessions(dir).size() == 1, "a file named for another pid is ignored");
    QFile::remove(dir + "/123.json");
    ::chmod(QFile::encodeName(sock).constData(), 0666);
    check(TerminalHandoff::liveSessions(dir).isEmpty(), "a socket others can open is ignored");
    ::chmod(QFile::encodeName(sock).constData(), 0600);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                   QFile::ReadOther | QFile::ExeOther);
    check(TerminalHandoff::liveSessions(dir).isEmpty(), "an open folder is ignored");
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    check(TerminalHandoff::liveSessions(dir).size() == 1, "and found again once private");
    const TerminalHandoffReply reply = TerminalHandoff::type(live.value(0), "p", "hi");
    check(!reply.ok, "a socket that never answers is not typeable");
    ::close(fd);
    QFile::remove(sock);
    QFile::remove(QStringLiteral("%1/%2.json").arg(dir).arg(me));
}

static void testWrapper(const QString &dir)
{
    Wrapped w = wrap(dir, {"sh", "-c", reader(21, true)});
    check(w.until("READY"), "wrapped reader ready");
    const TerminalHandoffSession *s = nullptr;
    QList<TerminalHandoffSession> live;
    for (int i = 0; i < 100 && !s; ++i) {
        live = TerminalHandoff::liveSessions(dir);
        s = sessionFor(live, w.pid);
        if (!s)
            w.pump(30);
    }
    check(s != nullptr, "the desktop discovers a running forkmesh-term");
    if (!s) {
        w.stop();
        return;
    }
    check(s->remote && s->cli == "sh" && s->child > 0, "registration fields");
    check(TerminalHandoff::find(live, {s->child}) == s, "the wrapped cli's pid maps to it");
    for (int i = 0; i < 100; ++i) {
        const TerminalHandoffReply hello =
            TerminalHandoff::exchange(*s, TerminalHandoff::requestFrame("hello", {}, {}));
        if (hello.detail.contains("paste=1"))
            break;
        w.pump(30);
    }
    const TerminalHandoffReply typed = TerminalHandoff::type(*s, "Jett's iPhone", "hi\r\nthere");
    check(typed.ok, "typed through the socket: " + typed.code + " " + typed.detail);
    check(w.until(":END"), "reader finished");
    same(QString::fromLatin1(w.seen()), "1b5b3230307e68690a74686572651b5b3230317e0d",
         "desktop text arrives as one bracketed paste and Enter");
    check(w.out.contains("typed from Jett's iPhone"), "notice names the phone");
    w.stop();

    Wrapped k = wrap(dir, {"sh", "-c", reader(1, false)});
    check(k.until("READY"), "key reader ready");
    const TerminalHandoffSession *ks = nullptr;
    for (int i = 0; i < 100 && !ks; ++i) {
        live = TerminalHandoff::liveSessions(dir);
        ks = sessionFor(live, k.pid);
        if (!ks)
            k.pump(30);
    }
    check(ks != nullptr, "second session discovered");
    if (ks) {
        const TerminalHandoffReply key = TerminalHandoff::key(*ks, "Jett's iPhone", "interrupt");
        check(key.ok && key.detail == "key=ctrl-c", "interrupt answered: " + key.detail);
        check(k.until(":END"), "key reader finished");
        same(QString::fromLatin1(k.seen()), "03", "Ctrl-C reaches a non-Claude cli");
        check(!TerminalHandoff::key(*ks, "p", "rm -rf").ok, "only named keys are sent");
    }
    k.stop();
}

static void testLocalControlAuth(const QString &root, const QString &dir)
{
    SigningKey desktopKey;
    SigningKey pairedKey;
    SigningKey strangerKey;
    LocalControlServer server(desktopKey.publicKey,
                              [&desktopKey](const QByteArray &data) { return desktopKey.sign(data); });
    server.setDesktopInfo("desk", "jett");
    server.setDeviceStorePath(root + "/paired-devices.json");
    server.setReceiveDirectory(root + "/inbox");
    int pairing = 0;
    server.onPairingRequested = [&](int id, const LocalControlServer::Device &, const QString &) {
        pairing = id;
    };
    TerminalHandoff::TypingGate gate;
    int routed = 0;
    TerminalHandoffSession target;
    server.onRequest = [&](int conn, const LocalControlServer::Device &device, int id,
                           const QString &method, const QJsonObject &params) {
        ++routed;
        if (method != "agents.prompt") {
            server.respondError(conn, id, "unknown_method");
            return;
        }
        const QString text = params.value("text").toString();
        const QString refused = TerminalHandoff::admit(gate, server.isPaired(device.key), device.key,
                                                       int(text.size()), QDateTime::currentMSecsSinceEpoch());
        if (!refused.isEmpty()) {
            server.respondError(conn, id, refused);
            return;
        }
        const TerminalHandoffReply reply = TerminalHandoff::type(target, device.name, text);
        if (!reply.ok) {
            server.respondError(conn, id, TerminalHandoff::errorCode(reply));
            return;
        }
        server.respond(conn, id, {{"status", "typed"}, {"via", "terminal"}});
    };
    QTcpServer listener;
    check(listener.listen(QHostAddress::LocalHost, 0), "listener");
    QObject::connect(&listener, &QTcpServer::newConnection, [&] {
        QTcpSocket *socket = listener.nextPendingConnection();
        auto *buffer = new QByteArray;
        QObject::connect(socket, &QTcpSocket::readyRead, socket, [=, &server] {
            buffer->append(socket->readAll());
            if (buffer->size() < 4)
                return;
            const quint32 length =
                qFromBigEndian<quint32>(reinterpret_cast<const uchar *>(buffer->constData()));
            if (buffer->size() < qsizetype(length) + 4)
                return;
            const QByteArray first = buffer->mid(4, length);
            const QByteArray rest = buffer->mid(4 + length);
            delete buffer;
            socket->disconnect();
            server.adopt(socket, first, rest);
        });
    });

    Wrapped w = wrap(dir, {"sh", "-c", reader(11, false)});
    check(w.until("READY"), "reader ready");
    for (int i = 0; i < 100; ++i) {
        const QList<TerminalHandoffSession> live = TerminalHandoff::liveSessions(dir);
        if (const TerminalHandoffSession *s = sessionFor(live, w.pid)) {
            target = *s;
            break;
        }
        w.pump(30);
    }
    check(target.pid == w.pid, "target session found");

    {
        Phone stranger(strangerKey, "Stranger phone");
        check(stranger.connectTo(listener.serverPort()), "stranger connects");
        check(waitFor([&] { return pairing != 0; }), "stranger waits for approval");
        stranger.send({{"id", 1}, {"method", "agents.prompt"}, {"params", QJsonObject{{"id", -5}, {"text", "rm -rf ~"}}}});
        check(waitFor([&] { return !stranger.response(1).isEmpty(); }), "stranger answered");
        same(stranger.response(1).value("error").toString(), "not_paired", "an unpaired phone is refused");
        check(routed == 0, "the unpaired request never reached the handler");
        server.denyPairing(pairing);
        pairing = 0;
    }
    {
        Phone forger(pairedKey, "Forged phone");
        forger.forgeAuth = true;
        check(forger.connectTo(listener.serverPort()), "forger connects");
        check(waitFor([&] { return forger.socket.state() == QAbstractSocket::UnconnectedState; }),
              "a bad signature closes the channel");
        check(routed == 0 && pairing == 0, "and nothing was asked or typed");
    }
    {
        Phone phone(pairedKey, "Jett's iPhone");
        check(phone.connectTo(listener.serverPort()), "phone connects");
        check(waitFor([&] { return pairing != 0; }), "pairing requested");
        server.approvePairing(pairing);
        check(waitFor([&] { return phone.paired(); }), "paired");
        phone.send({{"id", 2}, {"method", "agents.prompt"}, {"params", QJsonObject{{"id", -5}, {"text", "from phone"}}}});
        check(waitFor([&] { return !phone.response(2).isEmpty(); }), "prompt answered");
        same(phone.response(2).value("result").toObject().value("status").toString(), "typed",
             "a paired phone types into the terminal");
        phone.send({{"id", 3}, {"method", "agents.prompt"}, {"params", QJsonObject{{"id", -5}, {"text", "again"}}}});
        check(waitFor([&] { return !phone.response(3).isEmpty(); }), "second answered");
        same(phone.response(3).value("error").toString(), "typing_rate_limited",
             "a second prompt within a second is refused");
        check(w.until(":END"), "reader finished");
        same(QString::fromLatin1(w.seen()), "66726f6d2070686f6e650d", "exactly the paired phone's text arrived");
        check(w.out.contains("typed from Jett's iPhone"), "the terminal names the paired phone");
        check(!w.out.contains("rm -rf"), "the stranger's text never arrived");
    }
    w.stop();
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    ::signal(SIGPIPE, SIG_IGN);
    QTemporaryDir tmp;
    const QString dir = tmp.path() + "/rt";
    QDir().mkpath(dir);
    QFile::setPermissions(dir, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    testPrimitives();
    testDiscovery(tmp.path());
    testWrapper(dir);
    testLocalControlAuth(tmp.path(), dir);
    std::cout << checks << " checks, " << failures << " failed\n";
    return failures == 0 ? 0 : 1;
}
