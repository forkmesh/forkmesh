#include "NexusCore.h"
#include "NexusNetwork.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonDocument>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimeZone>
#include <QTimer>

#include <algorithm>

using namespace forkmesh::nexus;

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

QByteArray name(const QString &value)
{
    QByteArray out;
    for (const QString &label : value.split(u'.', Qt::SkipEmptyParts)) {
        out.append(char(label.size()));
        out.append(label.toUtf8());
    }
    out.append(char(0));
    return out;
}

QByteArray u16(int value)
{
    QByteArray out;
    out.append(char((value >> 8) & 0xFF));
    out.append(char(value & 0xFF));
    return out;
}

QByteArray record(const QByteArray &owner, int type, const QByteArray &data)
{
    return owner + u16(type) + u16(0x8001) + u16(0) + u16(120) + u16(int(data.size())) + data;
}

void testPorts()
{
    const PortParse parse = parsePorts(QStringLiteral("443, 22 80,8000-8003 22"));
    check(parse.error.isEmpty(), QStringLiteral("a mixed port list parses"));
    check(parse.ports == QList<quint16>({22, 80, 443, 8000, 8001, 8002, 8003}), QStringLiteral("ports are sorted and deduplicated"));
    check(describePorts(parse.ports) == QStringLiteral("22,80,443,8000-8003"), QStringLiteral("runs of three or more collapse into a range"));
    check(describePorts({80, 81}) == QStringLiteral("80,81"), QStringLiteral("a pair stays as two ports"));
    check(parsePorts(QStringLiteral("0")).error.contains(QStringLiteral("outside")), QStringLiteral("port 0 is refused"));
    check(parsePorts(QStringLiteral("90-80")).error.contains(QStringLiteral("ends before")), QStringLiteral("a reversed range is refused"));
    check(parsePorts(QStringLiteral("ssh")).error.contains(QStringLiteral("not a port")), QStringLiteral("a name is refused"));
    check(parsePorts(QString()).error == QStringLiteral("Enter at least one port."), QStringLiteral("an empty list asks for a port"));
    check(presetPorts(PortPreset::Quick).size() == 20, QStringLiteral("the quick preset has 20 ports"));
    check(presetPorts(PortPreset::Common).size() == 100, QStringLiteral("the common preset has 100 ports"));
    check(presetPorts(PortPreset::WellKnown).size() == 1024, QStringLiteral("the well-known preset covers 1-1024"));
    check(serviceForPort(22) && serviceForPort(22)->name == QStringLiteral("ssh"), QStringLiteral("port 22 is SSH in the catalog"));
    check(serviceCatalog().size() == 129, QStringLiteral("the catalog matches the iPhone package"));
}

void testTargets()
{
    const TargetParse cidr = parseTarget(QStringLiteral("192.168.1.77/24"), 256);
    check(cidr.error.isEmpty() && cidr.addresses.size() == 254 && cidr.isRange, QStringLiteral("a /24 yields 254 hosts"));
    check(cidr.addresses.constFirst() == QHostAddress(QStringLiteral("192.168.1.1")), QStringLiteral("a CIDR with host bits starts at the first host"));
    check(parseTarget(QStringLiteral("10.0.0.0/23"), 256).error.contains(QStringLiteral("limit")), QStringLiteral("a /23 exceeds the host limit"));
    const TargetParse shortRange = parseTarget(QStringLiteral("192.168.1.10-12"), 256);
    check(shortRange.addresses.size() == 3 && shortRange.isRange, QStringLiteral("a last-octet range parses"));
    const TargetParse longRange = parseTarget(QStringLiteral("10.0.0.250-10.0.1.4"), 256);
    check(longRange.addresses.size() == 11, QStringLiteral("a full-address range crosses octets"));
    check(parseTarget(QStringLiteral("192.168.1.20-10"), 256).error.contains(QStringLiteral("ends before")), QStringLiteral("a reversed address range is refused"));
    check(parseTarget(QStringLiteral("192.168.1.20-192.168.1.x"), 256).error.contains(QStringLiteral("not a valid")), QStringLiteral("a malformed range end is refused"));
    const TargetParse single = parseTarget(QStringLiteral(" fe80::1 "), 256);
    check(single.addresses.size() == 1 && !single.isRange, QStringLiteral("a single IPv6 address parses"));
    check(parseTarget(QStringLiteral("2001:db8::/64"), 256).error.contains(QStringLiteral("IPv6")), QStringLiteral("an IPv6 network is refused"));
    const TargetParse host = parseTarget(QStringLiteral("printer.local"), 256);
    check(host.hostname == QStringLiteral("printer.local") && host.addresses.isEmpty(), QStringLiteral("a host name is kept for lookup"));
    check(!parseTarget(QStringLiteral("bad host!"), 256).error.isEmpty(), QStringLiteral("a malformed host is refused"));
    check(!parseTarget(QString(), 256).error.isEmpty(), QStringLiteral("an empty target is refused"));
}

void testPolicy()
{
    check(classify(QHostAddress(QStringLiteral("127.0.0.1"))) == TargetScope::Loopback, QStringLiteral("127.0.0.1 is this computer"));
    check(classify(QHostAddress(QStringLiteral("::ffff:192.168.0.4"))) == TargetScope::LocalNetwork, QStringLiteral("a mapped private address is local"));
    check(classify(QHostAddress(QStringLiteral("100.64.3.1"))) == TargetScope::LocalNetwork, QStringLiteral("shared address space is local"));
    check(classify(QHostAddress(QStringLiteral("fd00::5"))) == TargetScope::LocalNetwork, QStringLiteral("an IPv6 ULA is local"));
    check(classify(QHostAddress(QStringLiteral("8.8.8.8"))) == TargetScope::PublicInternet, QStringLiteral("8.8.8.8 is the internet"));
    const QList<Network> authorized = parseNetworks(QStringLiteral("203.0.113.0/28, 198.51.100.7"));
    check(authorized.size() == 2 && authorized.at(1).second == 32, QStringLiteral("authorized networks parse, a bare address as /32"));
    check(classify(QHostAddress(QStringLiteral("203.0.113.9")), authorized) == TargetScope::AuthorizedNetwork, QStringLiteral("an address in an authorized network is authorized"));
    QString error;
    parseNetworks(QStringLiteral("nonsense"), &error);
    check(!error.isEmpty(), QStringLiteral("a bad authorized network is reported"));

    ScanPolicy policy;
    const QList<QHostAddress> local{QHostAddress(QStringLiteral("192.168.1.5"))};
    const QList<QHostAddress> internet{QHostAddress(QStringLiteral("8.8.8.8"))};
    check(evaluate(policy, local, 100, false, false).kind == DecisionKind::Allowed, QStringLiteral("a local host is allowed"));
    check(evaluate(policy, internet, 10, false, true).kind == DecisionKind::Denied, QStringLiteral("internet targets are off by default"));
    check(evaluate(policy, {QHostAddress(QStringLiteral("224.0.0.251"))}, 1, false, true).kind == DecisionKind::Denied, QStringLiteral("multicast is refused"));
    check(evaluate(policy, {QHostAddress(QStringLiteral("255.255.255.255"))}, 1, false, true).kind == DecisionKind::Denied, QStringLiteral("broadcast is refused"));
    check(evaluate(policy, {QHostAddress(QStringLiteral("0.0.0.0"))}, 1, false, true).kind == DecisionKind::Denied, QStringLiteral("the unspecified address is refused"));
    check(evaluate(policy, local, 70001, false, false).kind == DecisionKind::Denied, QStringLiteral("the probe limit applies"));
    policy.allowPublicTargets = true;
    check(evaluate(policy, internet, 10, false, false).kind == DecisionKind::NeedsAttestation, QStringLiteral("an internet host needs confirmation"));
    check(evaluate(policy, internet, 10, false, true).kind == DecisionKind::Allowed, QStringLiteral("a confirmed internet host is allowed"));
    check(evaluate(policy, internet, 10, true, true).kind == DecisionKind::Denied, QStringLiteral("an internet range is refused"));
    QList<QHostAddress> five;
    for (int i = 1; i <= 5; ++i)
        five << QHostAddress(QStringLiteral("8.8.4.%1").arg(i));
    check(evaluate(policy, five, 1, false, true).kind == DecisionKind::Denied, QStringLiteral("more than four internet hosts is refused"));
    policy.authorizedNetworks = authorized;
    check(evaluate(policy, {QHostAddress(QStringLiteral("203.0.113.1")), QHostAddress(QStringLiteral("203.0.113.2"))}, 5, true, false).kind == DecisionKind::Allowed,
          QStringLiteral("a range inside an authorized network needs no confirmation"));
    check(suggestedLocalNetwork({Network(QHostAddress(QStringLiteral("127.0.0.1")), 8), Network(QHostAddress(QStringLiteral("192.168.7.33")), 16)})
              == QStringLiteral("192.168.7.0/24"),
          QStringLiteral("the suggested network skips loopback and narrows to /24"));
    check(suggestedLocalNetwork({Network(QHostAddress(QStringLiteral("8.8.8.8")), 24)}).isEmpty(), QStringLiteral("a public interface suggests nothing"));
}

void testServices()
{
    ServiceGuess ssh = identifyService(2222, "SSH-2.0-OpenSSH_9.6p1 Debian\r\n");
    check(ssh.name == QStringLiteral("ssh") && ssh.confirmed && ssh.detail.startsWith(QStringLiteral("SSH-2.0")), QStringLiteral("an SSH banner is confirmed"));
    ServiceGuess http = identifyService(8080, "HTTP/1.1 200 OK\r\nServer: nginx/1.25\r\n\r\n");
    check(http.name == QStringLiteral("http") && http.detail.contains(QStringLiteral("nginx/1.25")), QStringLiteral("an HTTP reply names its server"));
    check(identifyService(21, "220 ProFTPD Server ready\r\n").name == QStringLiteral("ftp"), QStringLiteral("a 220 greeting on 21 is FTP"));
    check(identifyService(25, "220 mail.example ESMTP Postfix\r\n").name == QStringLiteral("smtp"), QStringLiteral("an ESMTP greeting is SMTP"));
    check(identifyService(5900, "RFB 003.008\n").name == QStringLiteral("vnc"), QStringLiteral("an RFB greeting is VNC"));
    ServiceGuess inferred = identifyService(443, {});
    check(inferred.name == QStringLiteral("https") && !inferred.confirmed, QStringLiteral("a silent port is only inferred from the catalog"));
    check(identifyService(5, {}).name == QStringLiteral("unknown"), QStringLiteral("an unlisted silent port is unknown"));
    check(sendsHttpProbe(80) && sendsHttpProbe(8080) && !sendsHttpProbe(443) && !sendsHttpProbe(22), QStringLiteral("only cleartext web ports get an HTTP probe"));
}

void testDns()
{
    const QByteArray query = mdnsQuery({QStringLiteral("_http._tcp.local"), QStringLiteral("_ssh._tcp.local")}, DnsTypePtr);
    check(query.size() == 12 + 18 + 4 + 17 + 4, QStringLiteral("a two-question query has the expected size"));
    check(quint8(query.at(5)) == 2, QStringLiteral("the query counts two questions"));

    QByteArray message = u16(0) + u16(0x8400) + u16(0) + u16(4) + u16(0) + u16(0);
    const int ptrOwner = int(message.size());
    message += record(name(QStringLiteral("_ipp._tcp.local")), DnsTypePtr, name(QStringLiteral("Office Printer._ipp._tcp.local")));
    const int instanceOffset = ptrOwner + int(name(QStringLiteral("_ipp._tcp.local")).size()) + 10;
    QByteArray pointer;
    pointer.append(char(0xC0 | (instanceOffset >> 8)));
    pointer.append(char(instanceOffset & 0xFF));
    message += record(pointer, DnsTypeSrv, u16(0) + u16(0) + u16(631) + name(QStringLiteral("printer.local")));
    QByteArray txt;
    txt.append(char(6));
    txt.append("ty=HP4");
    message += record(pointer, DnsTypeTxt, txt);
    QByteArray a;
    a.append(char(192));
    a.append(char(168));
    a.append(char(1));
    a.append(char(40));
    message += record(name(QStringLiteral("printer.local")), DnsTypeA, a);

    const QList<DnsRecord> records = parseDnsMessage(message);
    check(records.size() == 4, QStringLiteral("every answer parses"));
    check(records.value(1).name == QStringLiteral("Office Printer._ipp._tcp.local"), QStringLiteral("a compressed owner name follows its pointer"));
    check(records.value(1).port == 631 && records.value(1).target == QStringLiteral("printer.local"), QStringLiteral("SRV carries port and target"));
    check(records.value(2).text == QStringList{QStringLiteral("ty=HP4")}, QStringLiteral("TXT strings parse"));
    check(records.value(3).address == QHostAddress(QStringLiteral("192.168.1.40")), QStringLiteral("an A record parses"));
    check(parseDnsMessage(message.left(40)).size() < 4, QStringLiteral("a truncated message stops cleanly"));
    QByteArray loop = u16(0) + u16(0x8400) + u16(0) + u16(1) + u16(0) + u16(0);
    loop.append(char(0xC0));
    loop.append(char(12));
    check(parseDnsMessage(loop).isEmpty(), QStringLiteral("a self-referencing pointer is refused"));

    const QList<DiscoveredDevice> devices = devicesFromRecords(records);
    check(devices.size() == 1, QStringLiteral("the records describe one device"));
    const DiscoveredDevice device = devices.value(0);
    check(device.name == QStringLiteral("Office Printer"), QStringLiteral("the device is named after its instance"));
    check(device.addresses == QStringList{QStringLiteral("192.168.1.40")}, QStringLiteral("the device has its address"));
    check(device.services == QStringList{QStringLiteral("Printer (IPP)")}, QStringLiteral("the device lists its service"));
    check(device.ports == QList<quint16>{631}, QStringLiteral("the device lists its port"));
    const DiscoveredDevice copy = deviceFromJson(deviceToJson(device));
    check(copy.name == device.name && copy.addresses == device.addresses && copy.ports == device.ports, QStringLiteral("a device round-trips through JSON"));
}

void testHistory()
{
    ScanSession session;
    session.id = QStringLiteral("a");
    session.target = QStringLiteral("192.168.1.0/24");
    session.ports = QStringLiteral("22,23");
    session.startedAt = QDateTime(QDate(2026, 10, 19), QTime(9, 0), QTimeZone::utc());
    session.results << PortResult{QStringLiteral("192.168.1.2"), 22, PortState::Open, QStringLiteral("ssh"), QStringLiteral("SSH-2.0"), true, 4}
                    << PortResult{QStringLiteral("192.168.1.2"), 23, PortState::Open, QStringLiteral("telnet"), QString(), false, 5}
                    << PortResult{QStringLiteral("192.168.1.3"), 22, PortState::Closed, QStringLiteral("ssh"), QString(), false, -1};
    const ScanSession copy = scanFromJson(scanToJson(session));
    check(copy.results.size() == 3 && copy.results.at(2).state == PortState::Closed && copy.results.at(0).latencyMs == 4,
          QStringLiteral("a scan round-trips through JSON"));
    check(openCount(session) == 2, QStringLiteral("open ports are counted"));
    const QByteArray csv = scanCsv(session);
    check(csv.startsWith("address,port,state") && csv.count('\n') == 4, QStringLiteral("CSV has a header and one row per result"));
    ScanSession formula = session;
    formula.results = {PortResult{QStringLiteral("=cmd"), 1, PortState::Open, {}, {}, false, -1}};
    check(scanCsv(formula).contains("\"'=cmd\""), QStringLiteral("CSV defuses spreadsheet formulas"));

    ScanSession older = session;
    older.id = QStringLiteral("b");
    older.results = {PortResult{QStringLiteral("192.168.1.2"), 22, PortState::Open, QStringLiteral("ssh"), {}, true, 3},
                     PortResult{QStringLiteral("192.168.1.9"), 80, PortState::Open, QStringLiteral("http"), {}, true, 3}};
    const QStringList changes = portChanges(session, &older);
    check(changes.contains(QStringLiteral("Newly open since the last scan: 192.168.1.2:23")), QStringLiteral("a newly open port is noted"));
    check(changes.contains(QStringLiteral("No longer open: 192.168.1.9:80")), QStringLiteral("a closed port is noted"));
    check(changes.contains(QStringLiteral("192.168.1.2:23 accepts Telnet, which sends data unencrypted")), QStringLiteral("cleartext sensitive services are flagged"));

    NexusArchive archive;
    archive.settings.timeoutMs = 900;
    archive.settings.allowPublicTargets = true;
    archive.settings.authorizedNetworks = QStringLiteral("203.0.113.0/28");
    for (int i = 0; i < MaximumStoredScans + 5; ++i)
        archive.scans << session;
    const NexusArchive restored = archiveFromJson(QJsonDocument::fromJson(QJsonDocument(archiveToJson(archive)).toJson()).object());
    check(restored.scans.size() == MaximumStoredScans, QStringLiteral("history keeps the newest 50 scans"));
    check(restored.settings.timeoutMs == 900 && restored.settings.allowPublicTargets, QStringLiteral("settings round-trip"));
    check(policyFor(restored.settings).authorizedNetworks.size() == 1, QStringLiteral("authorized networks feed the policy"));
    check(archiveFromJson({}).settings.concurrency == 48, QStringLiteral("an empty archive has default settings"));
}

void testHeaders()
{
    const QList<HeaderFinding> plain = headerFindings({{"Server", "Apache/2.4.1"}}, false);
    check(std::any_of(plain.cbegin(), plain.cend(), [](const HeaderFinding &f) { return f.severity == QStringLiteral("warning"); }),
          QStringLiteral("plain HTTP is a warning"));
    check(std::any_of(plain.cbegin(), plain.cend(), [](const HeaderFinding &f) { return f.text.contains(QStringLiteral("Apache/2.4.1")); }),
          QStringLiteral("a versioned Server header is noted"));
    const QList<HeaderFinding> good = headerFindings({{"Strict-Transport-Security", "max-age=63072000"},
                                                      {"Content-Security-Policy", "default-src 'self'; frame-ancestors 'none'"},
                                                      {"X-Content-Type-Options", "nosniff"},
                                                      {"Referrer-Policy", "no-referrer"}},
                                                     true);
    check(good.isEmpty(), QStringLiteral("a well-configured site has no findings"));
}

void testScanner(QCoreApplication &)
{
    QTcpServer server;
    check(server.listen(QHostAddress::LocalHost, 0), QStringLiteral("a loopback listener starts"));
    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            socket->write("SSH-2.0-NexusTest\r\n");
            socket->flush();
        }
    });
    QTcpServer closed;
    closed.listen(QHostAddress::LocalHost, 0);
    const quint16 closedPort = closed.serverPort();
    closed.close();

    QObject context;
    TcpScanner scanner(&context);
    QList<PortResult> results;
    QList<int> progress;
    bool finished = false;
    bool wasCancelled = true;
    QEventLoop loop;
    QTimer::singleShot(10000, &loop, &QEventLoop::quit);
    ScanOptions options;
    options.timeoutMs = 2000;
    options.bannerMs = 1000;
    options.concurrency = 1;
    scanner.start({QHostAddress(QHostAddress::LocalHost)}, {server.serverPort(), closedPort}, options,
                  [&results](const PortResult &result) { results << result; },
                  [&progress](int completed, int) { progress << completed; },
                  [&](bool cancelled) {
                      finished = true;
                      wasCancelled = cancelled;
                      loop.quit();
                  });
    loop.exec();
    check(finished && !wasCancelled, QStringLiteral("the scan finishes on its own"));
    check(results.size() == 2 && progress.size() == 2, QStringLiteral("each probe reports once"));
    auto byPort = [&results](quint16 port) {
        for (const PortResult &result : results) {
            if (result.port == port)
                return result;
        }
        return PortResult{};
    };
    const PortResult open = byPort(server.serverPort());
    check(open.state == PortState::Open && open.confirmed && open.service == QStringLiteral("ssh"), QStringLiteral("the listening port is open and identified by its banner"));
    check(open.latencyMs >= 0, QStringLiteral("an open port records its connect time"));
    check(byPort(closedPort).state == PortState::Closed, QStringLiteral("a refused port is closed"));

    QList<quint16> many;
    for (int i = 0; i < 40; ++i)
        many << closedPort;
    bool cancelledFlag = false;
    int reported = 0;
    scanner.start({QHostAddress(QHostAddress::LocalHost)}, many, options,
                  [&reported](const PortResult &) { ++reported; }, {}, [&cancelledFlag](bool cancelled) { cancelledFlag = cancelled; });
    scanner.cancel();
    check(cancelledFlag && !scanner.running(), QStringLiteral("cancelling stops the scan and reports it"));
    QEventLoop settle;
    QTimer::singleShot(200, &settle, &QEventLoop::quit);
    settle.exec();
    check(reported == 0, QStringLiteral("no results arrive after cancelling"));
}

}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    testPorts();
    testTargets();
    testPolicy();
    testServices();
    testDns();
    testHistory();
    testHeaders();
    testScanner(app);
    return failures == 0 ? 0 : 1;
}
