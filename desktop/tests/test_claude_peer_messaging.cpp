#include "ClaudePeerMessaging.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QTemporaryDir>

#include <cstring>
#include <iostream>
#include <thread>

#if defined(Q_OS_UNIX)
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

static int failures = 0;

static void check(bool condition, const char *what)
{
    if (condition)
        return;
    std::cerr << "FAIL: " << what << "\n";
    ++failures;
}

static void same(const QByteArray &got, const QByteArray &want, const QString &what)
{
    if (got == want)
        return;
    std::cerr << "FAIL: " << what.toStdString() << "\n  got:  " << got.toStdString()
              << "\n  want: " << want.toStdString() << "\n";
    ++failures;
}

static ClaudePeerMessage messageFrom(const QJsonObject &o)
{
    ClaudePeerMessage m;
    m.text = o.value(QStringLiteral("text")).toString();
    m.fromName = o.value(QStringLiteral("fromName")).toString();
    m.fromMode = o.value(QStringLiteral("fromMode")).toString();
    m.from = o.value(QStringLiteral("from")).toString();
    m.msgId = o.value(QStringLiteral("msgId")).toString();
    m.priority = o.value(QStringLiteral("priority")).toString(QStringLiteral("next"));
    return m;
}

static bool writeFile(const QString &path, const QByteArray &data)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    return file.write(data) == data.size();
}

#if defined(Q_OS_UNIX)
static int listenOn(const QString &path)
{
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    const QByteArray p = QFile::encodeName(path);
    std::memcpy(addr.sun_path, p.constData(), size_t(p.size()));
    if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 || ::listen(fd, 4) != 0) {
        ::close(fd);
        return -1;
    }
    return fd;
}

static QByteArray acceptAll(int server)
{
    const int conn = ::accept(server, nullptr, nullptr);
    QByteArray data;
    if (conn < 0)
        return data;
    char buf[4096];
    for (;;) {
        const ssize_t n = ::read(conn, buf, sizeof buf);
        if (n <= 0)
            break;
        data.append(buf, int(n));
    }
    ::close(conn);
    return data;
}

#endif

static QByteArray record(qint64 pid, const QString &procStart, const QString &socketPath,
                         const QJsonValue &protocol, const QString &sessionId)
{
    QJsonObject o{{QStringLiteral("pid"), double(pid)},
                  {QStringLiteral("sessionId"), sessionId},
                  {QStringLiteral("cwd"), QStringLiteral("/tmp/work")},
                  {QStringLiteral("name"), QStringLiteral("work-1a")},
                  {QStringLiteral("status"), QStringLiteral("busy")},
                  {QStringLiteral("entrypoint"), QStringLiteral("cli")},
                  {QStringLiteral("messagingSocketPath"), socketPath}};
    if (!procStart.isEmpty())
        o.insert(QStringLiteral("procStart"), procStart);
    if (!protocol.isUndefined())
        o.insert(QStringLiteral("peerProtocol"), protocol);
    return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

static void fixtureTests()
{
    using namespace ClaudePeerMessaging;
    QFile file(QStringLiteral(FORKMESH_CLAUDE_PEER_FIXTURE));
    check(file.open(QIODevice::ReadOnly), "fixture opens");
    const QJsonObject fixture = QJsonDocument::fromJson(file.readAll()).object();
    check(fixture.value(QStringLiteral("peerProtocol")).toInt() == kPeerProtocol, "peer protocol version");
    check(fixture.value(QStringLiteral("senderName")).toString() == forkMeshSenderName(), "sender name");
    check(fixture.value(QStringLiteral("steerLead")).toString() == steerLead(), "steer lead");
    check(steerBody(QStringLiteral("Yes")) == steerLead() + QStringLiteral("\n\nYes"),
          "a steer opens with the lead");
    check(steerText(steerBody(QStringLiteral("Yes"))) == QStringLiteral("Yes"),
          "the lead is dropped for display");
    check(steerText(QStringLiteral("ping from a")) == QStringLiteral("ping from a"),
          "another session's message keeps its text");

    const QJsonObject recorded = fixture.value(QStringLiteral("recorded")).toObject();
    same(frame(recorded.value(QStringLiteral("token")).toString(),
               messageFrom(recorded.value(QStringLiteral("message")).toObject())),
         recorded.value(QStringLiteral("frame")).toString().toUtf8(),
         QStringLiteral("rebuilds the frame recorded from Claude Code's SendMessage"));

    for (const QJsonValue &v : fixture.value(QStringLiteral("frames")).toArray()) {
        const QJsonObject c = v.toObject();
        same(frame(c.value(QStringLiteral("token")).toString(),
                   messageFrom(c.value(QStringLiteral("message")).toObject())),
             c.value(QStringLiteral("frame")).toString().toUtf8(),
             QStringLiteral("frame ") + c.value(QStringLiteral("id")).toString());
    }
    for (const QJsonValue &v : fixture.value(QStringLiteral("escapes")).toArray()) {
        const QJsonObject c = v.toObject();
        same(escapeBody(c.value(QStringLiteral("text")).toString()).toUtf8(),
             c.value(QStringLiteral("body")).toString().toUtf8(),
             QStringLiteral("escape ") + c.value(QStringLiteral("text")).toString());
    }
    for (const QJsonValue &v : fixture.value(QStringLiteral("keys")).toArray()) {
        const QJsonObject c = v.toObject();
        same(keyPath(c.value(QStringLiteral("configDir")).toString(),
                     qint64(c.value(QStringLiteral("pid")).toDouble()),
                     c.value(QStringLiteral("socketPath")).toString())
                 .toUtf8(),
             c.value(QStringLiteral("keyPath")).toString().toUtf8(),
             QStringLiteral("key path for pid ") +
                 QString::number(c.value(QStringLiteral("pid")).toInt()));
    }
    for (const QJsonValue &v : fixture.value(QStringLiteral("inbound")).toArray()) {
        const QJsonObject c = v.toObject();
        const QJsonObject in = c.value(QStringLiteral("settings")).toObject();
        const ClaudeInboundPolicy got = inboundPolicy(
            in.value(QStringLiteral("managed")).toString(), in.value(QStringLiteral("flag")).toString(),
            in.value(QStringLiteral("user")).toString(), in.value(QStringLiteral("local")).toString(),
            in.value(QStringLiteral("project")).toString(),
            c.value(QStringLiteral("receiverClass")).toString());
        same((got.policy + QLatin1Char('/') + got.cause).toUtf8(),
             (c.value(QStringLiteral("policy")).toString() + QLatin1Char('/') +
              c.value(QStringLiteral("cause")).toString())
                 .toUtf8(),
             QStringLiteral("inbound ") + c.value(QStringLiteral("id")).toString());
    }
    for (const QJsonValue &v : fixture.value(QStringLiteral("settingsFiles")).toArray()) {
        const QJsonObject c = v.toObject();
        same(inboundSetting(c.value(QStringLiteral("json")).toString().toUtf8()).toUtf8(),
             c.value(QStringLiteral("value")).toString().toUtf8(),
             QStringLiteral("setting ") + c.value(QStringLiteral("json")).toString());
    }
    QTemporaryDir dir;
    for (const QJsonValue &v : fixture.value(QStringLiteral("modes")).toArray()) {
        const QJsonObject c = v.toObject();
        QByteArrayList argv;
        for (const QJsonValue &a : c.value(QStringLiteral("argv")).toArray())
            argv.append(a.toString().toUtf8());
        QString transcript;
        const QByteArray body = c.value(QStringLiteral("transcript")).toString().toUtf8();
        if (!body.isEmpty()) {
            transcript = dir.filePath(c.value(QStringLiteral("id")).toString() + QStringLiteral(".jsonl"));
            writeFile(transcript, body);
        }
        same(permissionClass(argv, transcript).toUtf8(),
             c.value(QStringLiteral("expect")).toString().toUtf8(),
             QStringLiteral("mode ") + c.value(QStringLiteral("id")).toString());
    }
}

#if defined(Q_OS_UNIX)
static void discoveryTests()
{
    using namespace ClaudePeerMessaging;
    QTemporaryDir root(QStringLiteral("/tmp/fmpeer-XXXXXX"));
    check(root.isValid(), "temp dir");
    const QString config = root.filePath(QStringLiteral("claude"));
    QDir().mkpath(config + QStringLiteral("/sessions"));
    const QString liveSock = root.filePath(QStringLiteral("live.sock"));
    const int server = listenOn(liveSock);
    check(server >= 0, "test socket listens");
    const qint64 self = ::getpid();
    const qint64 parent = ::getppid();
    const QString sessions = config + QStringLiteral("/sessions/");

    writeFile(sessions + QStringLiteral("%1.json").arg(self),
              record(self, procStart(self), liveSock, 1, QStringLiteral("sid-live")));
    writeFile(sessions + QStringLiteral("%1.json").arg(parent),
              record(parent, procStart(parent), root.filePath(QStringLiteral("gone.sock")), 1,
                     QStringLiteral("sid-no-socket")));
    writeFile(sessions + QStringLiteral("4194305.json"),
              record(4194305, QStringLiteral("123"), liveSock, 1, QStringLiteral("sid-stale")));
    writeFile(sessions + QStringLiteral("1.json"),
              record(1, QString(), liveSock, QJsonValue(), QStringLiteral("sid-old-protocol")));
    writeFile(sessions + QStringLiteral("notes.json"), QByteArrayLiteral("{}"));
    writeFile(sessions + QStringLiteral("77.json"), QByteArrayLiteral("not json"));
    const QString token = QStringLiteral("0123456789abcdef0123456789abcdef");
    writeFile(keyPath(config, self, liveSock),
              QJsonDocument(QJsonObject{{QStringLiteral("peerToken"), token}}).toJson());

    const QList<ClaudePeerSession> found = readSessions(config);
    check(found.size() == 1, "only the live session with a socket is found");
    check(!found.isEmpty() && found.first().pid == self && found.first().sessionId == QLatin1String("sid-live"),
          "the live record is read");
    check(!found.isEmpty() && found.first().configDir == QDir::cleanPath(config), "config dir kept");

    {
        const QString recycled = root.filePath(QStringLiteral("recycled"));
        QDir().mkpath(recycled + QStringLiteral("/sessions"));
        writeFile(recycled + QStringLiteral("/sessions/%1.json").arg(self),
                  record(self, QStringLiteral("1"), liveSock, 1, QStringLiteral("sid-recycled")));
        check(readSessions(recycled).isEmpty(), "a recycled pid (other start time) is skipped");
        const QList<ClaudePeerSession> merged = liveSessions({config, recycled, config});
        check(merged.size() == 1, "live sessions are merged by pid");
    }

    const QStringList dirs = configDirs(QStringLiteral("/home/u"), root.filePath(QStringLiteral("agent-accounts")),
                                        {config, QStringLiteral("/home/u/.claude/")});
    check(dirs.first() == QLatin1String("/home/u/.claude"), "home config dir first");
    check(dirs.count(QStringLiteral("/home/u/.claude")) == 1, "config dirs are unique");

    QList<ClaudePeerSession> two = found;
    ClaudePeerSession other = found.value(0);
    other.pid = 99;
    two.append(other);
    check(find(found, {self}, QString()) != nullptr, "find by pid");
    check(find(found, {}, QStringLiteral("sid-live")) != nullptr, "find by session id");
    check(find(found, {}, QStringLiteral("sid-none")) == nullptr, "unknown session id");
    check(find(two, {}, QStringLiteral("sid-live")) == nullptr, "an ambiguous session id is refused");

    check(peerToken(keyPath(config, self, liveSock)) == token, "peer token read from the key file");
    writeFile(root.filePath(QStringLiteral("bad.key")), QByteArrayLiteral("{\"peerToken\":\"XYZ\"}"));
    check(peerToken(root.filePath(QStringLiteral("bad.key"))).isEmpty(), "a malformed token is refused");

    if (!found.isEmpty()) {
        QByteArray received;
        std::thread reader([&] { received = acceptAll(server); });
        QString error;
        bool ok = deliver(found.first(), QStringLiteral("Also run the tests"), &error);
        reader.join();
        check(ok, "delivered over the socket");
        const QList<QByteArray> lines = received.split('\n');
        check(lines.size() == 3 && lines.at(2).isEmpty(), "two newline-terminated lines");
        same(lines.value(0), authLine(token), QStringLiteral("auth line first"));
        const QJsonObject user = QJsonDocument::fromJson(lines.value(1)).object();
        check(user.value(QStringLiteral("type")).toString() == QLatin1String("user"), "user frame");
        check(user.value(QStringLiteral("priority")).toString() == QLatin1String("next"), "next priority");
        check(!user.contains(QStringLiteral("from")), "no reply address");
        check(QRegularExpression(QStringLiteral("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$"))
                  .match(user.value(QStringLiteral("msg_id")).toString())
                  .hasMatch(),
              "msg_id is a v4 uuid");
        same(user.value(QStringLiteral("message")).toObject().value(QStringLiteral("content")).toString().toUtf8(),
             QByteArrayLiteral("<cross-session-message from-name=\"ForkMesh (phone)\">\n") +
                 steerLead().toUtf8() +
                 QByteArrayLiteral("\n\nAlso run the tests\n</cross-session-message>"),
             QStringLiteral("envelope names ForkMesh and claims no permission mode"));

        std::thread closer([&] { acceptAll(server); });
        error.clear();
        check(!send(liveSock, self + 1, QByteArrayLiteral("x\n"), &error), "a different endpoint pid is refused");
        closer.join();
        check(error.contains(QLatin1String("not")), "the refusal says why");

    }
    ::close(server);

    {
        ClaudePeerSession target;
        target.configDir = root.filePath(QStringLiteral("cfg"));
        target.cwd = root.filePath(QStringLiteral("repo/sub"));
        QDir().mkpath(target.configDir);
        QDir().mkpath(target.cwd + QStringLiteral("/.claude"));
        QDir().mkpath(root.filePath(QStringLiteral("repo/.git")));
        QDir().mkpath(root.filePath(QStringLiteral("repo/.claude")));
        const QString managed = root.filePath(QStringLiteral("managed-settings.json"));
        const QByteArrayList plain{QByteArrayLiteral("claude")};
        const auto policy = [&](const QByteArrayList &argv, const QString &cls) {
            const ClaudeInboundPolicy p = predictInbound(target, argv, cls, managed);
            return p.policy + QLatin1Char('/') + p.cause;
        };
        check(policy(plain, QStringLiteral("bypass")) == QLatin1String("hold/no-mode-asserted"),
              "a bypass session holds an unclaimed message");
        check(policy(plain, QStringLiteral("prompting")) == QLatin1String("accept/mode-parity"),
              "a prompting session takes an unclaimed message");
        writeFile(target.configDir + QStringLiteral("/settings.json"),
                  QByteArrayLiteral("{\"model\":\"x\",\"crossSessionInbound\":\"accept\"}"));
        check(policy(plain, QStringLiteral("bypass")) == QLatin1String("accept/explicit-setting"),
              "the user's accept lets a bypass session take it");
        writeFile(root.filePath(QStringLiteral("repo/.claude/settings.local.json")),
                  QByteArrayLiteral("{\"crossSessionInbound\":\"hold\"}"));
        check(policy(plain, QStringLiteral("bypass")) == QLatin1String("hold/repo-setting"),
              "a repository's hold at the git root tightens it");
        writeFile(target.cwd + QStringLiteral("/.claude/settings.json"),
                  QByteArrayLiteral("{\"crossSessionInbound\":\"refuse\"}"));
        check(policy(plain, QStringLiteral("prompting")) == QLatin1String("refuse/repo-setting"),
              "a project refuse wins");
        writeFile(managed, QByteArrayLiteral("{\"crossSessionInbound\":\"accept\"}"));
        check(policy(plain, QStringLiteral("bypass")) == QLatin1String("refuse/repo-setting"),
              "a repository can still tighten a managed accept");
        QFile::remove(target.cwd + QStringLiteral("/.claude/settings.json"));
        QFile::remove(root.filePath(QStringLiteral("repo/.claude/settings.local.json")));
        QFile::remove(managed);
        QFile::remove(target.configDir + QStringLiteral("/settings.json"));
        const QByteArrayList flagged{QByteArrayLiteral("claude"), QByteArrayLiteral("--settings"),
                                     QByteArrayLiteral("{\"crossSessionInbound\":\"accept\"}")};
        check(policy(flagged, QStringLiteral("bypass")) == QLatin1String("accept/explicit-setting"),
              "--settings JSON counts as the user's choice");
    }
    check(!ClaudePeerMessaging::send(root.filePath(QStringLiteral("gone.sock")), 0, "x\n", nullptr),
          "no socket, no send");
}

#endif

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    fixtureTests();
#if defined(Q_OS_UNIX)
    discoveryTests();
#endif
    if (failures == 0)
        std::cout << "claude peer messaging tests passed\n";
    return failures == 0 ? 0 : 1;
}
