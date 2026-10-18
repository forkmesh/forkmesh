#include "FakeSmbServer.h"
#include "NetworkShares.h"
#include "SmbClient.h"
#include "SmbProtocol.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>

using namespace forkmesh::smb;

namespace {

int failures = 0;

void check(bool condition, const QString &message)
{
    if (condition) {
        qInfo("PASS: %s", qPrintable(message));
    } else {
        qCritical("FAIL: %s", qPrintable(message));
        ++failures;
    }
}

void testCrypto()
{
    check(md4("abc").toHex() == "a448017aaf21d8525fc10ae87aa6729d", QStringLiteral("MD4 matches RFC 1320"));
    const QByteArray ntowf = ntowfv2(QStringLiteral("Password"), QStringLiteral("User"), QStringLiteral("Domain"));
    check(ntowf.toHex() == "0c868a403bfd7a93a3001ef22ef02e3f", QStringLiteral("NTOWFv2 matches MS-NLMP 4.2.4.1.1"));
    const QByteArray targetInfo = QByteArray::fromHex("02000c0044006f006d00610069006e0001000c0053006500720076006500720000000000");
    const NtlmResponse response = ntlmv2(ntowf, QByteArray::fromHex("0123456789abcdef"), QByteArray::fromHex("aaaaaaaaaaaaaaaa"),
                                         QByteArray(8, '\0'), targetInfo);
    check(response.ntProof.toHex() == "68cd0ab851e51c96aabc927bebef6a1c", QStringLiteral("NTProofStr matches MS-NLMP 4.2.4.2.2"));
    check(response.sessionKey.toHex() == "8de40ccadbc14a82f15cb0ad0de95ca3", QStringLiteral("the session base key matches MS-NLMP 4.2.4.1.2"));
    check(response.response.mid(16, 8).toHex() == "0101000000000000" && response.response.endsWith(QByteArray(4, '\0')),
          QStringLiteral("the NTLMv2 blob starts with its version and ends with four zero bytes"));

    const QByteArray negotiate = ntlmNegotiate(false);
    check(negotiate.startsWith(QByteArray("NTLMSSP\0", 8)) && readU32(negotiate, 8) == 1 && !(readU32(negotiate, 12) & NtlmAnonymous),
          QStringLiteral("an NTLM negotiate message is type 1 without the anonymous flag"));
    check(readU32(ntlmNegotiate(true), 12) & NtlmAnonymous, QStringLiteral("an anonymous negotiate sets the anonymous flag"));
    const QByteArray wrapped = spnegoInit(negotiate);
    check(quint8(wrapped[0]) == 0x60 && findNtlmToken(wrapped) == negotiate, QStringLiteral("SPNEGO wraps the NTLM token and it can be found again"));
    check(quint8(spnegoResponse(negotiate)[0]) == 0xa1, QStringLiteral("a SPNEGO response is a NegTokenResp"));

    QString user;
    QString domain;
    splitUser(QStringLiteral("WORK\\bob"), &user, &domain);
    check(user == QStringLiteral("bob") && domain == QStringLiteral("WORK"), QStringLiteral("DOMAIN\\user splits into user and domain"));
    splitUser(QStringLiteral("bob"), &user, &domain);
    check(user == QStringLiteral("bob") && domain.isEmpty(), QStringLiteral("a bare user has no domain"));
}

void testWire()
{
    Header header;
    header.command = Create;
    header.messageId = 9;
    header.sessionId = 0x55;
    header.treeId = 3;
    const QByteArray raw = packet(header, createRequest(QStringLiteral("/a/b.txt"), 1, 0, 7, DispositionOpen, 0));
    const Message parsed = parseMessage(raw);
    check(parsed.valid && parsed.header.command == Create && parsed.header.messageId == 9 && parsed.header.treeId == 3
              && parsed.header.sessionId == 0x55,
          QStringLiteral("an SMB2 header round-trips"));
    check(fromUtf16(raw.mid(readU16(parsed.body, 44), readU16(parsed.body, 46))) == QStringLiteral("a\\b.txt"),
          QStringLiteral("paths go on the wire with backslashes and no leading separator"));
    check(readU16(createRequest({}, 1, 0, 7, DispositionOpen, OptionDirectory), 46) == 0
              && createRequest({}, 1, 0, 7, DispositionOpen, OptionDirectory).size() == 57,
          QStringLiteral("the share root opens with an empty name and a one-byte buffer"));
    check(frame(QByteArray(3, 'x')).toHex() == "00000003787878", QStringLiteral("direct TCP framing is a zero byte and a 24-bit length"));
    const QByteArray key = QByteArray(16, '\x11');
    const QByteArray signedPacket = sign(raw, key);
    check((readU32(signedPacket, 16) & FlagSigned) && signedPacket.mid(48, 16) != QByteArray(16, '\0')
              && sign(signedPacket, key) == signedPacket,
          QStringLiteral("signing sets the flag and an HMAC-SHA256 signature that verifies"));
    check(fromFileTime(toFileTime(QDateTime(QDate(2026, 10, 18), QTime(8, 30), QTimeZone::UTC)))
              == QDateTime(QDate(2026, 10, 18), QTime(8, 30), QTimeZone::UTC),
          QStringLiteral("FILETIME converts both ways"));
    check(rpcBind(1).size() == 72 && readU16(rpcBind(1), 8) == 72, QStringLiteral("the srvsvc bind PDU carries its own length"));
}

void testNames()
{
    const Location location = parseUrl(QStringLiteral("smb://bob@192.168.1.1/media/Movies/a%20b.mkv"));
    check(location.host == QStringLiteral("192.168.1.1") && location.share == QStringLiteral("media")
              && location.path == QStringLiteral("Movies/a b.mkv") && location.user == QStringLiteral("bob")
              && location.port == kDefaultPort,
          QStringLiteral("an smb URL parses into host, share, path and user"));
    const Location unc = parseUrl(QStringLiteral("\\\\router.lan:4455\\Disk\\Photos"));
    check(unc.host == QStringLiteral("router.lan") && unc.port == 4455 && unc.share == QStringLiteral("Disk")
              && unc.path == QStringLiteral("Photos"),
          QStringLiteral("a UNC path with a port parses"));
    check(formatUrl(location) == QStringLiteral("smb://192.168.1.1/media/Movies/a b.mkv"), QStringLiteral("a location formats back to an smb URL"));
    check(formatUrl(parseUrl(QStringLiteral("smb://[fd00::1]:8445/s"))) == QStringLiteral("smb://[fd00::1]:8445/s/"),
          QStringLiteral("IPv6 hosts keep their brackets and port"));
    QString host;
    quint16 port = 0;
    QString error;
    check(!parseAddress(QStringLiteral("router:99999"), &host, &port, &error) && error.contains(QStringLiteral("port")),
          QStringLiteral("a port out of range is refused"));
    check(!parseAddress(QStringLiteral("bad host"), &host, &port, &error), QStringLiteral("an address with a space is refused"));
    check(isLocalName(QStringLiteral("192.168.1.1")) && isLocalName(QStringLiteral("nas")) && isLocalName(QStringLiteral("router.lan"))
              && isLocalName(QStringLiteral("fd00::5")),
          QStringLiteral("private addresses and local names count as local"));
    check(!isLocalName(QStringLiteral("8.8.8.8")) && !isLocalName(QStringLiteral("files.example.com"))
              && !isLocalAddress(QHostAddress(QStringLiteral("2001:db8::1"))),
          QStringLiteral("public addresses and internet names do not"));
    check(joinPath(QStringLiteral("/a/"), QStringLiteral("b")) == QStringLiteral("a/b") && joinPath({}, QStringLiteral("b")) == QStringLiteral("b")
              && parentPath(QStringLiteral("a/b/c")) == QStringLiteral("a/b") && parentPath(QStringLiteral("a")).isEmpty()
              && leafName(QStringLiteral("a/b/c")) == QStringLiteral("c"),
          QStringLiteral("share paths join and split"));
    check(statusText(StatusLogonFailure) == QStringLiteral("The user name or password is wrong.")
              && statusText(StatusAccessDenied) == QStringLiteral("Access denied.")
              && statusText(StatusBadNetworkName, QStringLiteral("nas"), QStringLiteral("x")) == QStringLiteral("There is no share named x on nas.")
              && statusText(StatusObjectNameNotFound) == QStringLiteral("That file or folder no longer exists.")
              && statusText(StatusObjectPathNotFound) == QStringLiteral("That file or folder no longer exists.")
              && statusText(StatusObjectNameCollision) == QStringLiteral("Something with that name is already there.")
              && statusText(StatusDirectoryNotEmpty) == QStringLiteral("The folder is not empty.")
              && statusText(StatusSharingViolation) == QStringLiteral("The file is open somewhere else.")
              && statusText(StatusDiskFull) == QStringLiteral("The disk is full.")
              && statusText(StatusMediaWriteProtected) == QStringLiteral("The share is read only.")
              && statusText(0xC0000999) == QStringLiteral("The file server answered 0xC0000999."),
          QStringLiteral("status codes read as the shared messages"));
    check(unreachableText(QStringLiteral("nas")) == QStringLiteral("Could not reach nas. Is file sharing on?")
              && dialectText(QStringLiteral("nas")) == QStringLiteral("nas does not speak SMB 2.1.")
              && notLocalText() == QStringLiteral("Network shares must be on your local network."),
          QStringLiteral("connection problems read as the shared messages"));
    check(formatSize(512) == QStringLiteral("512 B") && formatSize(1536) == QStringLiteral("1.5 KB")
              && formatSize(5LL * 1024 * 1024 * 1024) == QStringLiteral("5.0 GB"),
          QStringLiteral("sizes are short and readable"));
}

void testClient()
{
    auto server = std::make_shared<fakesmb::Server>();
    server->files.insert(QStringLiteral("Movies"), fakesmb::Node{true, {}});
    server->files.insert(QStringLiteral("Movies/clip.txt"), fakesmb::Node{false, "hello from the router"});
    server->files.insert(QStringLiteral(".hidden"), fakesmb::Node{false, "x"});
    QByteArray big(200000, '\0');
    for (int i = 0; i < big.size(); ++i)
        big[i] = char(i % 251);
    server->files.insert(QStringLiteral("big.bin"), fakesmb::Node{false, big});

    Client wrong(fakesmb::factory(server));
    check(!wrong.open(QStringLiteral("nas"), kDefaultPort, {QStringLiteral("alice"), QStringLiteral("nope"), false}, QStringLiteral("media"))
              && wrong.error() == QStringLiteral("The user name or password is wrong."),
          QStringLiteral("a wrong password is reported as such"));
    Client missing(fakesmb::factory(server));
    check(!missing.open(QStringLiteral("nas"), kDefaultPort, {QStringLiteral("alice"), QStringLiteral("secret"), false}, QStringLiteral("films"))
              && missing.error() == QStringLiteral("There is no share named films on nas."),
          QStringLiteral("an unknown share is named"));
    Client nowhere(fakesmb::factory(server));
    check(!nowhere.open(QStringLiteral("unreachable"), kDefaultPort, {}, QStringLiteral("media"))
              && nowhere.error() == unreachableText(QStringLiteral("unreachable")),
          QStringLiteral("an unreachable host is reported"));

    Client client(fakesmb::factory(server));
    check(client.open(QStringLiteral("nas"), kDefaultPort, {QStringLiteral("alice"), QStringLiteral("secret"), false}, QStringLiteral("media"))
              && client.dialect() == 0x0210 && !client.guestSession(),
          QStringLiteral("signing in with NTLMv2 opens the share over SMB 2.1"));
    QList<Entry> entries;
    check(client.list({}, &entries) && entries.size() == 3, QStringLiteral("the share root lists its entries without . and .."));
    const auto find = [&entries](const QString &name) {
        for (const Entry &entry : std::as_const(entries)) {
            if (entry.name == name)
                return entry;
        }
        return Entry();
    };
    check(find(QStringLiteral("Movies")).folder && find(QStringLiteral("big.bin")).size == big.size() && find(QStringLiteral(".hidden")).hidden
              && find(QStringLiteral("big.bin")).modified == QDateTime(QDate(2026, 10, 1), QTime(12, 0), QTimeZone::UTC),
          QStringLiteral("entries carry folder, size, hidden and modified time"));

    QBuffer sink;
    sink.open(QIODevice::WriteOnly);
    QList<qint64> seen;
    check(client.readFile(QStringLiteral("big.bin"), &sink, [&seen](qint64 done, qint64) { seen << done; return true; })
              && sink.data() == big && seen.last() == big.size() && seen.size() >= 4,
          QStringLiteral("a large file reads in 64 KiB chunks with progress"));
    QBuffer limited;
    limited.open(QIODevice::WriteOnly);
    check(!client.readFile(QStringLiteral("big.bin"), &limited, {}, 1000) && client.tooLarge(), QStringLiteral("a read over the limit is refused"));
    QBuffer cancelled;
    cancelled.open(QIODevice::WriteOnly);
    check(!client.readFile(QStringLiteral("big.bin"), &cancelled, [](qint64 done, qint64) { return done < 70000; }) && client.cancelled(),
          QStringLiteral("a download can be cancelled"));

    server->pendingOnce = true;
    QBuffer pending;
    pending.open(QIODevice::WriteOnly);
    check(client.readFile(QStringLiteral("Movies/clip.txt"), &pending) && pending.data() == "hello from the router",
          QStringLiteral("an interim STATUS_PENDING reply is skipped"));

    QByteArray upload(150000, 'u');
    QBuffer source(&upload);
    source.open(QIODevice::ReadOnly);
    check(client.writeFile(QStringLiteral("Movies/new.txt"), &source, upload.size()) && server->read(QStringLiteral("Movies/new.txt")) == upload,
          QStringLiteral("a file uploads in chunks"));
    check(client.makeFolder(QStringLiteral("Photos")) && server->files.value(QStringLiteral("Photos")).folder, QStringLiteral("a folder is created"));
    check(!client.makeFolder(QStringLiteral("Photos")) && client.error() == QStringLiteral("Something with that name is already there."),
          QStringLiteral("creating an existing folder is refused"));
    check(client.createFile(QStringLiteral("Photos/notes.txt")) && server->exists(QStringLiteral("Photos/notes.txt")),
          QStringLiteral("an empty file is created"));
    check(!client.createFile(QStringLiteral("Photos/notes.txt")), QStringLiteral("a new file does not overwrite one that exists"));
    check(client.rename(QStringLiteral("Photos"), QStringLiteral("Movies/Photos")) && server->exists(QStringLiteral("Movies/Photos/notes.txt"))
              && !server->exists(QStringLiteral("Photos")),
          QStringLiteral("a folder moves with what is inside it"));
    Entry info;
    check(client.stat(QStringLiteral("Movies/Photos"), &info) && info.folder && info.name == QStringLiteral("Photos"),
          QStringLiteral("stat reports a folder"));
    check(!client.remove(QStringLiteral("Movies"), true) && client.error() == QStringLiteral("The folder is not empty."),
          QStringLiteral("a plain delete of a full folder is refused"));
    check(client.removeEntry(QStringLiteral("Movies")) && !server->exists(QStringLiteral("Movies"))
              && !server->exists(QStringLiteral("Movies/Photos/notes.txt")),
          QStringLiteral("deleting a folder deletes everything in it"));
    check(!client.stat(QStringLiteral("Movies"), &info) && client.error() == QStringLiteral("That file or folder no longer exists."),
          QStringLiteral("a missing path is reported"));

    bool listed = false;
    const QList<ShareInfo> shares = client.shares(&listed);
    check(listed && shares.size() == 2 && shares.at(0).name == QStringLiteral("backup") && shares.at(1).name == QStringLiteral("media")
              && shares.at(1).remark == QStringLiteral("Disk media"),
          QStringLiteral("srvsvc lists the disk shares across two fragments and hides IPC$ and print$"));
}

void testGuestAndSigning()
{
    auto server = std::make_shared<fakesmb::Server>();
    Client refused(fakesmb::factory(server));
    check(!refused.open(QStringLiteral("nas"), kDefaultPort, {{}, {}, true}, QStringLiteral("media"))
              && refused.error() == QStringLiteral("The user name or password is wrong."),
          QStringLiteral("guest sign-in fails when the server allows no guests"));
    server->allowGuest = true;
    const int before = server->sessionSetups;
    Client guest(fakesmb::factory(server));
    check(guest.open(QStringLiteral("nas"), kDefaultPort, {{}, {}, true}, QStringLiteral("media")) && guest.guestSession()
              && server->sessionSetups - before == 4,
          QStringLiteral("guest sign-in tries the guest user, then an anonymous session"));
    server->allowNamedGuest = true;
    Client named(fakesmb::factory(server));
    const int namedBefore = server->sessionSetups;
    check(named.open(QStringLiteral("nas"), kDefaultPort, {{}, {}, true}, QStringLiteral("media")) && named.guestSession()
              && server->sessionSetups - namedBefore == 2,
          QStringLiteral("a server that maps unknown users to guest accepts the guest user at once"));

    server->signingRequired = true;
    Client signedClient(fakesmb::factory(server));
    QList<Entry> entries;
    check(signedClient.open(QStringLiteral("nas"), kDefaultPort, {QStringLiteral("alice"), QStringLiteral("secret"), false}, QStringLiteral("media"))
              && signedClient.signing() && signedClient.list({}, &entries) && server->signedRequests > 0 && server->badSignatures == 0,
          QStringLiteral("a server that requires signing gets correctly signed requests"));

    auto old = std::make_shared<fakesmb::Server>();
    old->refuseDialects = true;
    Client legacy(fakesmb::factory(old));
    check(!legacy.open(QStringLiteral("nas"), kDefaultPort, {}, QStringLiteral("media")) && legacy.error() == dialectText(QStringLiteral("nas")),
          QStringLiteral("a server without SMB 2.1 is named"));
}

void testTcp()
{
    auto server = std::make_shared<fakesmb::Server>();
    server->files.insert(QStringLiteral("a.txt"), fakesmb::Node{false, "over tcp"});
    fakesmb::TcpHost host(server);
    Client client;
    QBuffer sink;
    sink.open(QIODevice::WriteOnly);
    check(client.open(QStringLiteral("127.0.0.1"), host.port(), {QStringLiteral("alice"), QStringLiteral("secret"), false}, QStringLiteral("media"))
              && client.readFile(QStringLiteral("a.txt"), &sink) && sink.data() == "over tcp",
          QStringLiteral("the TCP transport frames requests and replies"));
    Client remote;
    check(!remote.open(QStringLiteral("8.8.8.8"), kDefaultPort, {}, QStringLiteral("media")) && remote.error() == notLocalText(),
          QStringLiteral("a public address is refused before connecting"));
}

void testStore(const QString &dir)
{
    ShareStore store(dir + QStringLiteral("/network-share-passwords.json"));
    SavedShare share;
    share.host = QStringLiteral("192.168.1.1");
    share.share = QStringLiteral("media");
    share.user = QStringLiteral("alice");
    share.remember = true;
    share = store.save(share);
    check(!share.id.isEmpty() && store.shares().size() == 1 && store.share(share.id).user == QStringLiteral("alice"),
          QStringLiteral("a network share is saved"));
    check(share.title() == QStringLiteral("media on 192.168.1.1") && share.url(QStringLiteral("a/b")) == QStringLiteral("smb://192.168.1.1/media/a/b"),
          QStringLiteral("a saved share has a title and URLs"));
    check(store.setPassword(share.id, QStringLiteral("secret")) && store.password(share.id) == QStringLiteral("secret"),
          QStringLiteral("a remembered password is stored"));
    QFile secrets(store.secretsPath());
    check(secrets.permissions() == (QFile::ReadOwner | QFile::WriteOwner | QFile::ReadUser | QFile::WriteUser)
              || secrets.permissions() == (QFile::ReadOwner | QFile::WriteOwner),
          QStringLiteral("the password file is readable only by its owner"));
    check(!QSettings().value(QStringLiteral("files/networkShares")).toString().contains(QStringLiteral("secret")),
          QStringLiteral("the password never goes into the settings"));
    store.remove(share.id);
    check(store.shares().isEmpty() && store.password(share.id).isEmpty(), QStringLiteral("removing a share forgets its password"));
}

}

int main(int argc, char **argv)
{
    QTemporaryDir dir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, dir.path());
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshSmbTests"));
    QCoreApplication::setApplicationName(QStringLiteral("smb-tests"));
    testCrypto();
    testWire();
    testNames();
    testClient();
    testGuestAndSigning();
    testTcp();
    testStore(dir.path());
    return failures == 0 ? 0 : 1;
}
