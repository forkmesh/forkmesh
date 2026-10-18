#include "NetworkReplyError.h"
#include "OpenWrtClient.h"
#include "OpenWrtFeature.h"
#include "OpenWrtModel.h"

#include <QApplication>
#include <QCheckBox>
#include <QPlainTextEdit>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QTimer>
#include <QNetworkAccessManager>
#include <QPushButton>
#include <QSettings>
#include <QSplitter>
#include <QStackedWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <functional>
#include <memory>

using namespace forkmesh::openwrt;

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

class RecordingManager : public QNetworkAccessManager
{
public:
    int requests = 0;
    int privateRequests = 0;

protected:
    QNetworkReply *createRequest(Operation op, const QNetworkRequest &request, QIODevice *data) override
    {
        ++requests;
        if (forkmesh::networkRequestPayloadIsPrivate(request))
            ++privateRequests;
        return QNetworkAccessManager::createRequest(op, request, data);
    }
};

class FakeRouter : public QObject
{
public:
    QTcpServer server;
    QList<QJsonArray> calls;
    QString session;
    int logins = 0;
    bool expireOnce = false;
    bool fstabMissing = false;
    bool fstabWritten = false;
    bool ksmbdInstalled = false;
    QStringList packageCommands;

    FakeRouter()
    {
        server.listen(QHostAddress::LocalHost);
        connect(&server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *socket = server.nextPendingConnection())
                serve(socket);
        });
    }

    QJsonArray lastCall(const QString &object, const QString &method) const
    {
        for (auto it = calls.rbegin(); it != calls.rend(); ++it) {
            if (it->at(1).toString() == object && it->at(2).toString() == method)
                return *it;
        }
        return {};
    }

private:
    void serve(QTcpSocket *socket)
    {
        auto buffer = std::make_shared<QByteArray>();
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        connect(socket, &QTcpSocket::readyRead, socket, [this, socket, buffer] {
            *buffer += socket->readAll();
            const int end = buffer->indexOf("\r\n\r\n");
            if (end < 0)
                return;
            int length = 0;
            for (const QByteArray &line : buffer->left(end).split('\n')) {
                if (line.toLower().startsWith("content-length:"))
                    length = line.mid(15).trimmed().toInt();
            }
            if (buffer->size() < end + 4 + length)
                return;
            const QJsonObject request = QJsonDocument::fromJson(buffer->mid(end + 4, length)).object();
            buffer->clear();
            const QJsonArray params = request.value(QStringLiteral("params")).toArray();
            calls << params;
            QJsonObject reply{{QStringLiteral("jsonrpc"), QStringLiteral("2.0")}, {QStringLiteral("id"), request.value(QStringLiteral("id"))}};
            const QJsonValue answer = respond(params);
            if (answer.isObject())
                reply.insert(QStringLiteral("error"), answer);
            else
                reply.insert(QStringLiteral("result"), answer);
            const QByteArray body = QJsonDocument(reply).toJson(QJsonDocument::Compact);
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\n\r\n" + body);
            socket->disconnectFromHost();
        });
    }

    QJsonValue respond(const QJsonArray &params)
    {
        const QString sid = params.at(0).toString();
        const QString object = params.at(1).toString();
        const QString method = params.at(2).toString();
        const QJsonObject args = params.at(3).toObject();
        const QJsonObject denied{{QStringLiteral("code"), -32002}, {QStringLiteral("message"), QStringLiteral("Access denied")}};
        if (object == QStringLiteral("session") && method == QStringLiteral("login")) {
            if (sid != kAnonymousSession || args.value(QStringLiteral("password")).toString() != QStringLiteral("secret"))
                return QJsonArray{UbusPermissionDenied};
            session = QStringLiteral("sess%1").arg(++logins);
            return QJsonArray{0, QJsonObject{{QStringLiteral("ubus_rpc_session"), session}}};
        }
        if (sid != session)
            return denied;
        if (expireOnce && object == QStringLiteral("system") && method == QStringLiteral("board")) {
            expireOnce = false;
            session = QStringLiteral("expired");
            return denied;
        }
        if (object == QStringLiteral("system") && method == QStringLiteral("board"))
            return QJsonArray{0, QJsonObject{{QStringLiteral("hostname"), QStringLiteral("OpenWrt")}, {QStringLiteral("model"), QStringLiteral("Test Router")}}};
        if (object == QStringLiteral("uci") && method == QStringLiteral("get") && args.value(QStringLiteral("config")).toString() == QStringLiteral("fstab")) {
            if (fstabMissing)
                return QJsonArray{UbusNotFound};
            QJsonObject values;
            if (fstabWritten)
                values.insert(QStringLiteral("cfg01"), QJsonObject{{QStringLiteral(".name"), QStringLiteral("cfg01")}, {QStringLiteral(".type"), QStringLiteral("global")},
                                                                   {QStringLiteral(".anonymous"), true}, {QStringLiteral(".index"), 0}, {QStringLiteral("anon_mount"), QStringLiteral("0")}});
            return QJsonArray{0, QJsonObject{{QStringLiteral("values"), values}}};
        }
        if (object == QStringLiteral("file") && method == QStringLiteral("write")) {
            if (args.value(QStringLiteral("path")).toString() != QStringLiteral("/etc/config/fstab") || !args.value(QStringLiteral("data")).toString().contains(QStringLiteral("global")))
                return QJsonArray{UbusPermissionDenied};
            fstabMissing = false;
            fstabWritten = true;
            return QJsonArray{0};
        }
        if (object == QStringLiteral("file") && method == QStringLiteral("list") && args.value(QStringLiteral("path")).toString() == QStringLiteral("/sys/bus/usb/devices"))
            return QJsonArray{0, QJsonObject{{QStringLiteral("entries"), QJsonArray{QJsonObject{{QStringLiteral("name"), QStringLiteral("usb1")}},
                                                                                   QJsonObject{{QStringLiteral("name"), QStringLiteral("1-1")}},
                                                                                   QJsonObject{{QStringLiteral("name"), QStringLiteral("1-1:1.0")}}}}}};
        if (object == QStringLiteral("file") && method == QStringLiteral("read") && args.value(QStringLiteral("path")).toString() == QStringLiteral("/sys/bus/usb/devices/1-1/product"))
            return QJsonArray{0, QJsonObject{{QStringLiteral("data"), QStringLiteral("Flash Disk\n")}}};
        if (object == QStringLiteral("file") && method == QStringLiteral("read") && args.value(QStringLiteral("path")).toString() == QStringLiteral("/proc/partitions"))
            return QJsonArray{0, QJsonObject{{QStringLiteral("data"), QStringLiteral("major minor  #blocks  name\n\n  31        0       1024 mtdblock0\n   8        0   15000000 sda\n   8        1   14999000 sda1\n")}}};
        if (object == QStringLiteral("uci") && method == QStringLiteral("configs")) {
            QJsonArray configs{QStringLiteral("fstab"), QStringLiteral("network")};
            if (ksmbdInstalled)
                configs.append(QStringLiteral("ksmbd"));
            return QJsonArray{0, QJsonObject{{QStringLiteral("configs"), configs}}};
        }
        if (object == QStringLiteral("uci") && method == QStringLiteral("get") && args.value(QStringLiteral("config")).toString() == QStringLiteral("ksmbd")) {
            const QJsonObject values{{QStringLiteral("cfg02"), QJsonObject{{QStringLiteral(".name"), QStringLiteral("cfg02")}, {QStringLiteral(".type"), QStringLiteral("share")},
                                                                         {QStringLiteral(".anonymous"), true}, {QStringLiteral(".index"), 0},
                                                                         {QStringLiteral("name"), QStringLiteral("media")}, {QStringLiteral("path"), QStringLiteral("/mnt/sda1")},
                                                                         {QStringLiteral("guest_ok"), QStringLiteral("yes")}, {QStringLiteral("read_only"), QStringLiteral("no")}}}};
            return QJsonArray{0, QJsonObject{{QStringLiteral("values"), values}}};
        }
        if (object == QStringLiteral("uci") && method == QStringLiteral("get")) {
            if (args.value(QStringLiteral("config")).toString() != QStringLiteral("network"))
                return QJsonArray{0, QJsonObject{{QStringLiteral("values"), QJsonObject()}}};
            const QJsonObject values{
                {QStringLiteral("lan"), QJsonObject{{QStringLiteral(".name"), QStringLiteral("lan")}, {QStringLiteral(".type"), QStringLiteral("interface")}, {QStringLiteral(".index"), 1},
                                                    {QStringLiteral("proto"), QStringLiteral("static")}, {QStringLiteral("ipaddr"), QStringLiteral("192.168.1.1")},
                                                    {QStringLiteral("netmask"), QStringLiteral("255.255.255.0")}}},
                {QStringLiteral("wan"), QJsonObject{{QStringLiteral(".name"), QStringLiteral("wan")}, {QStringLiteral(".type"), QStringLiteral("interface")}, {QStringLiteral(".index"), 2},
                                                    {QStringLiteral("proto"), QStringLiteral("dhcp")}}},
            };
            return QJsonArray{0, QJsonObject{{QStringLiteral("values"), values}}};
        }
        if (object == QStringLiteral("uci") && method == QStringLiteral("changes"))
            return QJsonArray{0, QJsonObject{{QStringLiteral("changes"), QJsonObject()}}};
        if (object == QStringLiteral("file") && method == QStringLiteral("exec")) {
            const QString command = args.value(QStringLiteral("command")).toString();
            if (command == QStringLiteral("/bin/false"))
                return QJsonArray{0, QJsonObject{{QStringLiteral("code"), 1}, {QStringLiteral("stderr"), QStringLiteral("nope")}}};
            const QJsonArray execParams = args.value(QStringLiteral("params")).toArray();
            if (command == QStringLiteral("/bin/opkg") && !execParams.isEmpty() && execParams.first().toString() != QStringLiteral("list-installed")) {
                QStringList words;
                for (const QJsonValue &word : execParams)
                    words << word.toString();
                packageCommands << words.join(QLatin1Char(' '));
                if (words.value(1) == QStringLiteral("ksmbd-server"))
                    ksmbdInstalled = true;
                return QJsonArray{0, QJsonObject{{QStringLiteral("code"), 0}, {QStringLiteral("stdout"), QStringLiteral("Installing.\n")}}};
            }
            if (command == QStringLiteral("/bin/opkg"))
                return QJsonArray{0, QJsonObject{{QStringLiteral("code"), 0}, {QStringLiteral("stdout"), QStringLiteral("base-files - 1-r1\nluci - git-24\n")}}};
            if (command == QStringLiteral("/sbin/block") && args.value(QStringLiteral("params")).toArray().first().toString() == QStringLiteral("detect"))
                return QJsonArray{0, QJsonObject{{QStringLiteral("code"), 0}, {QStringLiteral("stdout"), QStringLiteral("config 'global'\n\toption\tanon_mount\t'0'\n")}}};
            if (command == QStringLiteral("/bin/dmesg"))
                return QJsonArray{0, QJsonObject{{QStringLiteral("code"), 0}, {QStringLiteral("stdout"), QStringLiteral("<6>[    5.1] usb 1-1: new high-speed USB device number 2\n<5>[    6.0] random: crng init done\n")}}};
            return QJsonArray{UbusPermissionDenied};
        }
        return QJsonArray{0};
    }
};

void testModel()
{
    const QJsonObject body = QJsonDocument::fromJson(callBody(7, {}, QStringLiteral("uci"), QStringLiteral("get"), {{QStringLiteral("config"), QStringLiteral("network")}})).object();
    const QJsonArray params = body.value(QStringLiteral("params")).toArray();
    check(body.value(QStringLiteral("method")).toString() == QStringLiteral("call") && body.value(QStringLiteral("id")).toInt() == 7
              && params.at(0).toString() == kAnonymousSession && params.at(1).toString() == QStringLiteral("uci")
              && params.at(3).toObject().value(QStringLiteral("config")).toString() == QStringLiteral("network"),
          QStringLiteral("a call body is a JSON-RPC ubus call, anonymous without a session"));

    RpcResult ok = parseReply(R"({"jsonrpc":"2.0","id":1,"result":[0,{"a":1}]})");
    check(ok.ok && ok.data.value(QStringLiteral("a")).toInt() == 1, QStringLiteral("a status 0 reply carries its data"));
    RpcResult bare = parseReply(R"({"jsonrpc":"2.0","id":1,"result":[0]})");
    check(bare.ok && bare.data.isEmpty(), QStringLiteral("a status-only reply is ok without data"));
    RpcResult denied = parseReply(R"({"jsonrpc":"2.0","id":1,"result":[6]})");
    check(!denied.ok && denied.accessDenied && denied.status == UbusPermissionDenied, QStringLiteral("ubus status 6 is access denied"));
    RpcResult expired = parseReply(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32002,"message":"Access denied"}})");
    check(!expired.ok && expired.accessDenied, QStringLiteral("JSON-RPC -32002 is access denied"));
    RpcResult missing = parseReply(R"({"jsonrpc":"2.0","id":1,"error":{"code":-32000,"message":"Object not found"}})");
    check(!missing.ok && !missing.accessDenied && missing.error.contains(QStringLiteral("not found")), QStringLiteral("a missing ubus object is explained"));
    RpcResult html = parseReply("<html>404</html>", 404);
    check(!html.ok && html.error.contains(QStringLiteral("/ubus")), QStringLiteral("a device without /ubus is named as not OpenWrt"));
    check(statusMessage(UbusTimeout).contains(QStringLiteral("too long")), QStringLiteral("ubus status codes have readable messages"));

    Address plain = parseAddress(QStringLiteral("192.168.1.1"));
    check(plain.valid() && plain.base.toString() == QStringLiteral("http://192.168.1.1") && !plain.encrypted, QStringLiteral("a bare address uses http"));
    Address tls = parseAddress(QStringLiteral("https://router.lan:8443/cgi-bin/luci"));
    check(tls.valid() && tls.encrypted && tls.base.port() == 8443 && tls.base.path().isEmpty(), QStringLiteral("an https address keeps its port and drops the path"));
    check(!parseAddress(QStringLiteral("8.8.8.8")).valid(), QStringLiteral("a public address is refused"));
    check(!parseAddress(QStringLiteral("http://root:pw@192.168.1.1")).valid(), QStringLiteral("credentials in the address are refused"));
    check(!parseAddress(QStringLiteral("ftp://192.168.1.1")).valid() && !parseAddress({}).valid(), QStringLiteral("other schemes and empty input are refused"));
    check(parseAddress(QStringLiteral("[fd00::1]")).valid(), QStringLiteral("a unique-local IPv6 router is accepted"));
    check(isLocalHostName(QStringLiteral("openwrt")) && isLocalHostName(QStringLiteral("router.lan")) && isLocalHostName(QStringLiteral("10.0.0.1"))
              && isLocalHostName(QStringLiteral("172.20.1.1")) && isLocalHostName(QStringLiteral("fe80::1")),
          QStringLiteral("local names and private addresses are local"));
    check(!isLocalHostName(QStringLiteral("example.com")) && !isLocalHostName(QStringLiteral("172.32.0.1")) && !isLocalHostName(QStringLiteral("2001:db8::1")),
          QStringLiteral("internet names and public addresses are not local"));

    const QString routes = QStringLiteral("Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
                                          "wlan0\t00000000\t0101A8C0\t0003\t0\t0\t600\t00000000\t0\t0\t0\n"
                                          "eth0\t00000000\t01000A0A\t0003\t0\t0\t100\t00000000\t0\t0\t0\n"
                                          "eth0\t0000A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n");
    check(defaultGatewayFromProcRoute(routes) == QStringLiteral("10.10.0.1"), QStringLiteral("the default gateway with the lowest metric wins"));
    check(defaultGatewayFromRouteGet(QStringLiteral("   route to: default\ndestination: default\n    gateway: 192.168.8.1\n  interface: en0\n")) == QStringLiteral("192.168.8.1"),
          QStringLiteral("the macOS route output gives the gateway"));

    const QJsonObject values{
        {QStringLiteral("cfg02"), QJsonObject{{QStringLiteral(".name"), QStringLiteral("cfg02")}, {QStringLiteral(".type"), QStringLiteral("redirect")}, {QStringLiteral(".index"), 2},
                                              {QStringLiteral(".anonymous"), true}, {QStringLiteral("name"), QStringLiteral("Game")},
                                              {QStringLiteral("proto"), QJsonArray{QStringLiteral("tcp"), QStringLiteral("udp")}}}},
        {QStringLiteral("wan"), QJsonObject{{QStringLiteral(".name"), QStringLiteral("wan")}, {QStringLiteral(".type"), QStringLiteral("zone")}, {QStringLiteral(".index"), 1},
                                            {QStringLiteral("masq"), QStringLiteral("1")}}},
    };
    const QList<UciSection> all = uciSections(values);
    check(all.size() == 2 && all.first().name == QStringLiteral("wan") && all.first().flag(QStringLiteral("masq")), QStringLiteral("sections come in UCI order with their options"));
    const QList<UciSection> redirects = uciSections(values, QStringLiteral("redirect"));
    check(redirects.size() == 1 && redirects.first().option(QStringLiteral("proto")) == QStringLiteral("tcp udp")
              && sectionLabel(redirects.first()) == QStringLiteral("redirect \"Game\"") && !redirects.first().options.contains(QStringLiteral(".type")),
          QStringLiteral("a type filter, list options and anonymous labels"));
    check(optionValue(QStringLiteral("1.1.1.1, 9.9.9.9"), true).toArray().size() == 2 && optionValue(QStringLiteral(" x "), false).toString() == QStringLiteral("x"),
          QStringLiteral("text becomes a list or a trimmed value"));
    check(validUciName(QStringLiteral("guest_wifi")) && !validUciName(QStringLiteral("a.b")) && !validUciName(QStringLiteral("x;y")), QStringLiteral("UCI names are checked"));

    const QJsonObject changes{
        {QStringLiteral("wireless"), QJsonArray{QJsonArray{QStringLiteral("set"), QStringLiteral("default_radio0"), QStringLiteral("key"), QStringLiteral("hunter22")},
                                                QJsonArray{QStringLiteral("set"), QStringLiteral("default_radio0"), QStringLiteral("ssid"), QStringLiteral("Home")}}},
        {QStringLiteral("firewall"), QJsonArray{QJsonArray{QStringLiteral("remove"), QStringLiteral("cfg02")}, QJsonArray{QStringLiteral("add"), QStringLiteral("cfg09"), QStringLiteral("redirect")}}},
    };
    const QStringList described = describeChanges(changes);
    check(pendingCount(changes) == 4 && described.size() == 4, QStringLiteral("every pending change is counted and described"));
    check(described.contains(QStringLiteral("wireless.default_radio0.ssid = Home")) && described.contains(QStringLiteral("firewall.cfg02 deleted"))
              && described.contains(QStringLiteral("firewall.cfg09: new redirect section")),
          QStringLiteral("changes read as config paths"));
    check(!described.join(QLatin1Char(' ')).contains(QStringLiteral("hunter22")) && isSecretOption(QStringLiteral("key")), QStringLiteral("Wi-Fi keys are masked in the change list"));

    const QList<Lease> leases = parseLeases(QStringLiteral("1700000600 AA:BB:CC:DD:EE:01 192.168.1.20 laptop 01:aa\n0 aa:bb:cc:dd:ee:02 192.168.1.21 * *\nbroken line\n"));
    check(leases.size() == 2 && leases.first().mac == QStringLiteral("aa:bb:cc:dd:ee:01") && leases.at(1).hostname.isEmpty(), QStringLiteral("dnsmasq leases parse"));
    check(leaseExpiry(1700000600, 1700000000) == QStringLiteral("in 10m") && leaseExpiry(0, 5) == QStringLiteral("Never") && leaseExpiry(4, 5) == QStringLiteral("Expired"),
          QStringLiteral("lease expiry reads as time left"));

    const QList<Package> opkg = parseInstalledPackages(QStringLiteral("luci - git-24.1\nbase-files - 1-r1 - The base files\n"));
    check(opkg.size() == 2 && opkg.first().name == QStringLiteral("base-files") && opkg.first().version == QStringLiteral("1-r1"), QStringLiteral("opkg list-installed parses"));
    const QList<Package> status = parseInstalledPackages(QStringLiteral("Package: dnsmasq\nVersion: 2.90-r3\nStatus: install user installed\n\nPackage: uhttpd\nVersion: 2024.01\n"));
    check(status.size() == 2 && status.first().name == QStringLiteral("dnsmasq") && status.first().version == QStringLiteral("2.90-r3"), QStringLiteral("an opkg status file parses"));
    const QList<Package> apk = parseInstalledPackages(QStringLiteral("luci-app-sqm-1.2.3-r1 noarch {luci} (GPL-2.0) [installed]\nbusybox-1.36.1-r2 x86_64 {busybox} (GPL-2.0) [installed]\n"));
    check(apk.size() == 2 && apk.first().name == QStringLiteral("busybox") && apk.at(1).name == QStringLiteral("luci-app-sqm") && apk.at(1).version == QStringLiteral("1.2.3-r1"),
          QStringLiteral("apk list --installed parses"));
    const QList<PackageTool> tools = packageTools();
    check(tools.size() == 4 && tools.first().wrapper, QStringLiteral("LuCI's package wrappers are tried before the package managers"));
    check(packageArguments(tools.at(2), QStringLiteral("install"), {QStringLiteral("nano"), QStringLiteral("htop")}) == QStringList({QStringLiteral("add"), QStringLiteral("nano"), QStringLiteral("htop")})
              && packageArguments(tools.at(3), QStringLiteral("remove"), {QStringLiteral("nano")}) == QStringList({QStringLiteral("remove"), QStringLiteral("nano")})
              && packageArguments(tools.at(0), QStringLiteral("list")) == QStringList({QStringLiteral("list-installed")}),
          QStringLiteral("each tool gets its own arguments"));
    check(validPackageName(QStringLiteral("luci-app-sqm")) && !validPackageName(QStringLiteral("-force")) && !validPackageName(QStringLiteral("a;reboot"))
              && !validPackageName(QStringLiteral("a b")),
          QStringLiteral("package names cannot smuggle options or commands"));

    const QList<Service> services = parseServices(QJsonObject{{QStringLiteral("uhttpd"), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("running"), true}}},
                                                              {QStringLiteral("dnsmasq"), QJsonObject{{QStringLiteral("enabled"), true}, {QStringLiteral("running"), false}}}});
    check(services.size() == 2 && services.first().name == QStringLiteral("dnsmasq") && !services.first().running, QStringLiteral("rc list parses sorted"));

    const QJsonObject dump = QJsonDocument::fromJson(R"({"interface":[{"interface":"wan","up":true,"uptime":3600,"proto":"dhcp","l3_device":"eth1",
        "ipv4-address":[{"address":"203.0.113.7","mask":24}],"dns-server":["203.0.113.1"],
        "route":[{"target":"0.0.0.0","mask":0,"nexthop":"203.0.113.1"}]}]})").object();
    const QList<InterfaceStatus> interfaces = parseInterfaces(dump);
    check(interfaces.size() == 1 && interfaces.first().ipv4 == QStringList{QStringLiteral("203.0.113.7/24")} && interfaces.first().gateway == QStringLiteral("203.0.113.1")
              && interfaces.first().device == QStringLiteral("eth1") && interfaces.first().up,
          QStringLiteral("interface status gives addresses and the default gateway"));

    check(formatUptime(90061) == QStringLiteral("1d 1h") && formatUptime(59) == QStringLiteral("59s"), QStringLiteral("uptime is short"));
    check(formatBytes(1536) == QStringLiteral("1.5 KB") && formatBytes(12) == QStringLiteral("12 B"), QStringLiteral("sizes are readable"));
    check(formatLoad(QJsonArray{65536, 32768}) == QStringLiteral("1.00 · 0.50"), QStringLiteral("load averages are scaled"));
    check(validIpv4(QStringLiteral("192.168.1.1")) && !validIpv4(QStringLiteral("256.1.1.1")) && !validIpv4(QStringLiteral("1.2.3")), QStringLiteral("IPv4 addresses are checked"));
    check(validNetmask(QStringLiteral("255.255.255.0")) && !validNetmask(QStringLiteral("255.0.255.0")), QStringLiteral("netmasks must be contiguous"));
    check(validMac(QStringLiteral("aa:bb:cc:dd:ee:ff")) && !validMac(QStringLiteral("aa:bb:cc:dd:ee")), QStringLiteral("MAC addresses are checked"));
    check(validPort(QStringLiteral("8080")) && validPort(QStringLiteral("6000-6010")) && !validPort(QStringLiteral("0")) && !validPort(QStringLiteral("70000"))
              && !validPort(QStringLiteral("10-5")),
          QStringLiteral("ports and ranges are checked"));
    check(ssidProblem(QStringLiteral("Home")).isEmpty() && !ssidProblem({}).isEmpty() && !ssidProblem(QString(33, QLatin1Char('x'))).isEmpty(),
          QStringLiteral("Wi-Fi names are 1 to 32 bytes"));
    check(wifiKeyProblem(QStringLiteral("psk2"), QStringLiteral("short")).contains(QStringLiteral("8")) && wifiKeyProblem(QStringLiteral("none"), {}).isEmpty()
              && wifiKeyProblem(QStringLiteral("psk2"), QString(64, QLatin1Char('a'))).isEmpty()
              && !wifiKeyProblem(QStringLiteral("psk2"), QString(64, QLatin1Char('z'))).isEmpty(),
          QStringLiteral("Wi-Fi passwords follow WPA rules"));
    check(hostnameProblem(QStringLiteral("router-1")).isEmpty() && !hostnameProblem(QStringLiteral("-bad")).isEmpty(), QStringLiteral("hostnames are checked"));
    check(!passwordProblem(QStringLiteral("abc"), QStringLiteral("abc")).isEmpty() && !passwordProblem(QStringLiteral("abcdefgh"), QStringLiteral("abcdefgx")).isEmpty()
              && passwordProblem(QStringLiteral("abcdefgh"), QStringLiteral("abcdefgh")).isEmpty(),
          QStringLiteral("a new router password is long enough and confirmed"));
    check(choiceLabel(encryptionChoices(), QStringLiteral("sae")) == QStringLiteral("WPA3 only") && choiceLabel(protocolChoices(), QStringLiteral("odd")) == QStringLiteral("odd"),
          QStringLiteral("choices have labels and unknown values show as is"));
    const QString fingerprint = certificateFingerprint("certificate");
    check(fingerprint.size() == 95 && fingerprint.count(QLatin1Char(':')) == 31 && fingerprint == fingerprint.toUpper(), QStringLiteral("fingerprints are colon-separated SHA-256"));
    check(firmwareProblem(QJsonObject{{QStringLiteral("valid"), true}}).isEmpty()
              && firmwareProblem(QJsonObject{{QStringLiteral("valid"), false}, {QStringLiteral("tests"), QJsonObject{{QStringLiteral("fwtool_device_match"), false}}}}).contains(QStringLiteral("model")),
          QStringLiteral("an image for another model is refused with the reason"));
    const QJsonObject blocks = QJsonDocument::fromJson(R"({"sda1":{"dev":"/dev/sda1","uuid":"1234-ABCD","label":"Media Disk","type":"exfat","size":64000000000,"mount":"/mnt/sda1"},
        "sdb1":{"dev":"/dev/sdb1","uuid":"aa-bb","type":"ext4","size":1000},
        "mtdblock5":{"dev":"/dev/mtdblock5","type":"squashfs","size":4000},
        "ubiblock0_1":{"dev":"/dev/ubiblock0_1","type":"squashfs"}})").object();
    const QList<BlockDevice> devices = parseBlockDevices(blocks);
    check(devices.size() == 2 && devices.first().name == QStringLiteral("sda1") && devices.first().size == 64000000000LL,
          QStringLiteral("only USB and card disks are listed, not the router's flash"));
    check(isStorageDevice(QStringLiteral("mmcblk0p1")) && isStorageDevice(QStringLiteral("nvme0n1p2")) && !isStorageDevice(QStringLiteral("zram0")) && !isStorageDevice(QStringLiteral("loop0")),
          QStringLiteral("storage device names are recognised"));
    const QList<MountPoint> mountPoints = parseMountPoints(QJsonDocument::fromJson(R"({"result":[{"device":"/dev/sda1","mount":"/mnt/sda1","type":"exfat","size":64000000000,"avail":16000000000},
        {"device":"overlayfs:/overlay","mount":"/","type":"overlay","size":10,"avail":5}]})").object());
    check(mountPoints.size() == 2 && mountPoints.first().avail == 16000000000LL, QStringLiteral("mount points carry their free space"));
    const QList<UciSection> fstab = uciSections(QJsonDocument::fromJson(R"({"cfg01":{".name":"cfg01",".type":"global",".anonymous":true,".index":0,"anon_mount":"1"},
        "cfg02":{".name":"cfg02",".type":"mount",".anonymous":true,".index":1,"uuid":"aa-bb","target":"/mnt/backup","enabled":"0"}})").object());
    const QList<StorageDisk> disks = storageDisks(devices, mountPoints, fstab);
    check(disks.size() == 2 && disks.first().mounted() && disks.first().usageKnown && disks.first().avail == 16000000000LL && disks.first().fstabSection.isEmpty(),
          QStringLiteral("a mounted disk shows its usage"));
    check(!disks.at(1).mounted() && disks.at(1).fstabSection == QStringLiteral("cfg02") && disks.at(1).bootTarget == QStringLiteral("/mnt/backup") && !disks.at(1).bootEnabled,
          QStringLiteral("a disk is matched to its fstab entry by UUID"));
    check(disks.first().title() == QStringLiteral("Media Disk (sda1)") && suggestedMountPath(devices.first()) == QStringLiteral("/mnt/Media_Disk")
              && suggestedMountPath(devices.at(1)) == QStringLiteral("/mnt/sdb1"),
          QStringLiteral("disks get readable titles and mount folders"));
    check(mountPathProblem(QStringLiteral("/mnt/usb")).isEmpty() && !mountPathProblem(QStringLiteral("/etc")).isEmpty() && !mountPathProblem(QStringLiteral("/mnt/../etc")).isEmpty()
              && !mountPathProblem(QStringLiteral("/mnt/a b")).isEmpty() && !mountPathProblem(QStringLiteral("/mnt/")).isEmpty(),
          QStringLiteral("mount folders stay under /mnt-like roots"));
    const QJsonObject mountValues = mountSectionValues(devices.first(), QStringLiteral("/mnt/media"));
    check(mountValues.value(QStringLiteral("uuid")).toString() == QStringLiteral("1234-ABCD") && mountValues.value(QStringLiteral("target")).toString() == QStringLiteral("/mnt/media")
              && mountValues.value(QStringLiteral("enabled")).toString() == QStringLiteral("1") && !mountValues.contains(QStringLiteral("device")),
          QStringLiteral("a boot mount is keyed by UUID"));
    check(networkSharingPackages() == QStringList({QStringLiteral("ksmbd-server"), QStringLiteral("luci-app-ksmbd")}),
          QStringLiteral("network sharing installs the ksmbd server and its LuCI page"));
    check(shareUrl(QStringLiteral("192.168.1.1"), QStringLiteral("My Disk")) == QStringLiteral("smb://192.168.1.1/My%20Disk/")
              && shareUrl(QStringLiteral("fd00::1"), QStringLiteral("media")) == QStringLiteral("smb://[fd00::1]/media/"),
          QStringLiteral("a share opens at an smb URL on the router"));
    check(usbStoragePackages().contains(QStringLiteral("block-mount")) && usbStoragePackages().contains(QStringLiteral("kmod-usb-storage")),
          QStringLiteral("USB storage support names the driver and mount packages"));
    check(shareConfig({QStringLiteral("network"), QStringLiteral("samba4")}) == QStringLiteral("samba4") && shareConfig({QStringLiteral("ksmbd"), QStringLiteral("samba4")}) == QStringLiteral("ksmbd")
              && shareConfig({QStringLiteral("network")}).isEmpty() && shareSectionType(QStringLiteral("samba4")) == QStringLiteral("sambashare"),
          QStringLiteral("the file server in use is found"));
    const QList<Share> shares = parseShares(uciSections(QJsonDocument::fromJson(R"({"cfg01":{".name":"cfg01",".type":"globals",".index":0},
        "cfg02":{".name":"cfg02",".type":"share",".index":1,"name":"Media","path":"/mnt/sda1","read_only":"no","guest_ok":"yes"}})").object()));
    check(shares.size() == 1 && shares.first().name == QStringLiteral("Media") && shares.first().guests && !shares.first().readOnly, QStringLiteral("shares parse"));
    check(shareNameProblem(QStringLiteral("Media")).isEmpty() && !shareNameProblem(QStringLiteral("a/b")).isEmpty(), QStringLiteral("share names are checked"));
    check(confirmationPhrase({}) == QStringLiteral("OpenWrt") && confirmationPhrase(QStringLiteral("gw")) == QStringLiteral("gw"), QStringLiteral("a reset is confirmed with the hostname"));
}

void testClient()
{
    FakeRouter router;
    RecordingManager network;
    QUrl base;
    base.setScheme(QStringLiteral("http"));
    base.setHost(QStringLiteral("127.0.0.1"));
    base.setPort(router.server.serverPort());

    Client client(&network);
    client.setBase(base);
    RpcResult wrong;
    bool done = false;
    client.signIn(QStringLiteral("root"), QStringLiteral("guess"), [&](const RpcResult &result) { wrong = result; done = true; });
    check(waitFor([&] { return done; }) && !wrong.ok && wrong.error.contains(QStringLiteral("password")) && !client.signedIn(),
          QStringLiteral("a wrong password is reported as such"));

    done = false;
    RpcResult login;
    client.signIn(QStringLiteral("root"), QStringLiteral("secret"), [&](const RpcResult &result) { login = result; done = true; });
    check(waitFor([&] { return done; }) && login.ok && client.signedIn(), QStringLiteral("the right password signs in"));

    int renewed = 0;
    client.setSessionRenewed([&] { ++renewed; });
    router.expireOnce = true;
    done = false;
    RpcResult board;
    client.call(QStringLiteral("system"), QStringLiteral("board"), {}, [&](const RpcResult &result) { board = result; done = true; });
    check(waitFor([&] { return done; }) && board.ok && board.data.value(QStringLiteral("model")).toString() == QStringLiteral("Test Router"),
          QStringLiteral("an expired session is renewed and the call retried"));
    check(renewed == 1 && router.logins == 2 && router.lastCall(QStringLiteral("system"), QStringLiteral("board")).at(0).toString() == QStringLiteral("sess2"),
          QStringLiteral("the retry uses the new session"));

    done = false;
    RpcResult failed;
    client.exec(QStringLiteral("/bin/false"), {}, [&](const RpcResult &result) { failed = result; done = true; });
    check(waitFor([&] { return done; }) && !failed.ok && failed.error == QStringLiteral("nope"), QStringLiteral("a failing command reports its stderr"));

    check(network.requests > 0 && network.privateRequests == network.requests, QStringLiteral("every router request is marked private for the network log"));

    QUrl closed = base;
    QTcpServer probe;
    probe.listen(QHostAddress::LocalHost);
    closed.setPort(probe.serverPort());
    probe.close();
    Client offline(&network);
    offline.setBase(closed);
    done = false;
    RpcResult unreachable;
    offline.signIn(QStringLiteral("root"), QStringLiteral("secret"), [&](const RpcResult &result) { unreachable = result; done = true; });
    check(waitFor([&] { return done; }) && !unreachable.ok && unreachable.unreachable && unreachable.error.contains(QStringLiteral("127.0.0.1")),
          QStringLiteral("an unreachable router is named in the error"));
}

void testStorageCheck()
{
    check(parsePartitions(QStringLiteral("major minor  #blocks  name\n\n  31 0 1024 mtdblock0\n 8 0 100 sda\n 8 1 99 sda1\n 179 0 50 mmcblk0\n"))
              == QStringList({QStringLiteral("sda"), QStringLiteral("sda1"), QStringLiteral("mmcblk0")}),
          QStringLiteral("/proc/partitions gives the kernel's disks without the router's flash"));
    const QStringList entries{QStringLiteral("usb1"), QStringLiteral("usb2"), QStringLiteral("1-1"), QStringLiteral("1-1:1.0"), QStringLiteral("2-1.4")};
    check(usbControllerCount(entries) == 2 && usbDeviceEntries(entries) == QStringList({QStringLiteral("1-1"), QStringLiteral("2-1.4")}),
          QStringLiteral("USB controllers and devices are told apart from interfaces"));
    check(storageKernelLines(QStringLiteral("<6>[1.0] usb 1-1: new device\n<6>[1.1] eth0: link up\n<3>[2.0] sd 0:0:0:0: [sda] No Caching mode page found\n"))
              == QStringList({QStringLiteral("[1.0] usb 1-1: new device"), QStringLiteral("[2.0] sd 0:0:0:0: [sda] No Caching mode page found")}),
          QStringLiteral("only USB and disk kernel messages are kept, without their priority"));
    check(missingStoragePackages({QStringLiteral("block-mount"), QStringLiteral("kmod-usb-storage")}).size() == usbStoragePackages().size() - 2,
          QStringLiteral("missing storage packages are listed"));
    check(fstabFromDetect(QStringLiteral("config 'global'\n\toption\tanon_mount\t'0'")).endsWith(QStringLiteral("'0'\n"))
              && fstabFromDetect(QString()).startsWith(QStringLiteral("config global\n")),
          QStringLiteral("block detect output becomes the fstab, with a default when it prints nothing"));
    check(outputTail(QStringLiteral("a\n\nb\nc\n"), 2) == QStringList({QStringLiteral("b"), QStringLiteral("c")}), QStringLiteral("the end of a command's output is kept"));

    StorageCheck empty;
    empty.packagesKnown = true;
    empty.usbKnown = true;
    empty.usbControllers = 1;
    empty.partitionsKnown = true;
    check(storageFindings(empty).first().contains(QStringLiteral("sees nothing plugged into its USB port")), QStringLiteral("an empty port is reported"));
    StorageCheck noHost = empty;
    noHost.usbControllers = 0;
    check(storageFindings(noHost).first().contains(QStringLiteral("no USB controller driver")), QStringLiteral("a missing USB controller driver is reported"));
    StorageCheck noDriver = empty;
    noDriver.usbDevices = {QStringLiteral("Flash Disk (1-1)")};
    check(storageFindings(noDriver).first().contains(QStringLiteral("kmod-usb-storage")) && storageFindings(noDriver).first().contains(QStringLiteral("Flash Disk")),
          QStringLiteral("a plugged-in drive without the storage driver names the driver"));
    StorageCheck noBlock = noDriver;
    noBlock.installed = {QStringLiteral("kmod-usb-storage")};
    noBlock.partitions = {QStringLiteral("sda"), QStringLiteral("sda1")};
    check(storageFindings(noBlock).first().contains(QStringLiteral("block-mount is not installed")), QStringLiteral("a disk without block-mount is reported"));
    StorageCheck noFstab = noBlock;
    noFstab.blockTool = true;
    noFstab.disks = 1;
    const QStringList fstabFindings = storageFindings(noFstab);
    check(fstabFindings.join(QLatin1Char('\n')).contains(QStringLiteral("/etc/config/fstab")) && fstabFindings.last().contains(QStringLiteral("Turn on Mount any disk")),
          QStringLiteral("a missing fstab and an unmounted disk say what to do"));
    StorageCheck ready = noFstab;
    ready.fstabExists = true;
    ready.installed = usbStoragePackages();
    ready.mounted = 1;
    check(storageFindings(ready) == QStringList({QStringLiteral("The disk is mounted.")}), QStringLiteral("a working disk reports only that it is mounted"));
    check(storageReport(ready).contains(QStringLiteral("Storage packages: all installed")) && storageReport(ready).startsWith(QStringLiteral("• The disk is mounted.")),
          QStringLiteral("the report lists findings before the details"));
}

void testFeature()
{
    FakeRouter router;
    router.fstabMissing = true;
    RecordingManager network;
    forkmesh::market::NativePackageHost host;
    host.packageId = QStringLiteral("openwrt");
    host.network = &network;
    QString browsedUrl;
    bool browsedAsGuest = false;
    host.openNetworkShare = [&](const QString &url, bool guest) {
        browsedUrl = url;
        browsedAsGuest = guest;
    };
    OpenWrtFeature page(host);
    QLineEdit *addressField = nullptr;
    QLineEdit *passwordField = nullptr;
    for (QLineEdit *field : page.findChildren<QLineEdit *>()) {
        if (field->accessibleName() == QStringLiteral("Router address"))
            addressField = field;
        if (field->accessibleName() == QStringLiteral("Router password"))
            passwordField = field;
    }
    QPushButton *connectButton = nullptr;
    for (QPushButton *button : page.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Connect"))
            connectButton = button;
    }
    auto *pages = page.findChild<QStackedWidget *>();
    check(addressField && passwordField && connectButton && pages && pages->currentIndex() == 0, QStringLiteral("the page opens on the sign-in form"));
    if (!addressField || !passwordField || !connectButton || !pages)
        return;

    addressField->setText(QStringLiteral("8.8.8.8"));
    connectButton->click();
    const auto statusText = [&page] {
        QStringList texts;
        for (QLabel *label : page.findChildren<QLabel *>()) {
            if (label->objectName() == QStringLiteral("errorLabel") || label->objectName() == QStringLiteral("statusLine"))
                texts << label->text();
        }
        return texts.join(QLatin1Char('\n'));
    };
    check(network.requests == 0 && statusText().contains(QStringLiteral("not on your local network")), QStringLiteral("a public address is refused before any request"));

    addressField->setText(QStringLiteral("http://127.0.0.1:%1").arg(router.server.serverPort()));
    passwordField->setText(QStringLiteral("secret"));
    connectButton->click();
    auto *interfaces = page.findChild<QTreeWidget *>(QStringLiteral("openWrtInterfaces"));
    check(waitFor([&] { return pages->currentIndex() == 1 && interfaces && interfaces->topLevelItemCount() == 2; }), QStringLiteral("signing in loads the router's interfaces"));
    check(passwordField->text().isEmpty(), QStringLiteral("the password field is cleared after signing in"));
    check(network.privateRequests == network.requests, QStringLiteral("the page's requests are all private"));
    if (!interfaces || interfaces->topLevelItemCount() == 0)
        return;

    interfaces->setCurrentItem(interfaces->topLevelItem(0));
    QLineEdit *dns = nullptr;
    for (QLineEdit *field : page.findChildren<QLineEdit *>()) {
        if (field->placeholderText().startsWith(QStringLiteral("Optional, for example 1.1.1.1")))
            dns = field;
    }
    QPushButton *save = nullptr;
    auto *splitter = qobject_cast<QSplitter *>(interfaces->parentWidget());
    if (splitter) {
        for (QPushButton *button : splitter->findChildren<QPushButton *>()) {
            if (button->text() == QStringLiteral("Save"))
                save = button;
        }
    }
    check(dns && save, QStringLiteral("the interface editor has a DNS field and Save"));
    if (!dns || !save)
        return;
    dns->setText(QStringLiteral("1.1.1.1 9.9.9.9"));
    save->click();
    check(waitFor([&] { return !router.lastCall(QStringLiteral("uci"), QStringLiteral("set")).isEmpty(); }), QStringLiteral("saving stages a UCI set"));
    const QJsonObject args = router.lastCall(QStringLiteral("uci"), QStringLiteral("set")).at(3).toObject();
    check(args.value(QStringLiteral("config")).toString() == QStringLiteral("network") && args.value(QStringLiteral("section")).toString() == QStringLiteral("lan")
              && args.value(QStringLiteral("values")).toObject().value(QStringLiteral("dns")).toArray().size() == 2,
          QStringLiteral("the staged change sets the lan DNS list"));
    check(router.lastCall(QStringLiteral("uci"), QStringLiteral("apply")).isEmpty(), QStringLiteral("nothing is applied without asking"));
    check(page.searchHits(QStringLiteral("wan"), 5).size() == 1, QStringLiteral("the top search finds router interfaces"));

    QCheckBox *autoMount = nullptr;
    for (QCheckBox *box : page.findChildren<QCheckBox *>()) {
        if (box->text() == QStringLiteral("Mount any disk when it is plugged in"))
            autoMount = box;
    }
    check(autoMount && autoMount->isEnabled(), QStringLiteral("automatic mounting can be switched on while the router has no fstab"));
    if (!autoMount)
        return;
    autoMount->click();
    check(waitFor([&] {
              const QJsonObject set = router.lastCall(QStringLiteral("uci"), QStringLiteral("set")).at(3).toObject();
              return set.value(QStringLiteral("config")).toString() == QStringLiteral("fstab");
          }),
          QStringLiteral("switching on automatic mounting stages it"));
    const QJsonObject written = router.lastCall(QStringLiteral("file"), QStringLiteral("write")).at(3).toObject();
    const QJsonObject mountSet = router.lastCall(QStringLiteral("uci"), QStringLiteral("set")).at(3).toObject();
    check(written.value(QStringLiteral("path")).toString() == QStringLiteral("/etc/config/fstab") && written.value(QStringLiteral("data")).toString().startsWith(QStringLiteral("config 'global'"))
              && mountSet.value(QStringLiteral("section")).toString() == QStringLiteral("cfg01")
              && mountSet.value(QStringLiteral("values")).toObject().value(QStringLiteral("anon_mount")).toString() == QStringLiteral("1"),
          QStringLiteral("a missing fstab is created from block detect before anon_mount is set on its global section"));
    check(router.lastCall(QStringLiteral("uci"), QStringLiteral("add")).isEmpty(), QStringLiteral("no second global section is added"));

    QPlainTextEdit *log = nullptr;
    for (QPlainTextEdit *edit : page.findChildren<QPlainTextEdit *>()) {
        if (edit->accessibleName() == QStringLiteral("USB storage diagnostics"))
            log = edit;
    }
    QPushButton *checkUsb = nullptr;
    for (QPushButton *button : page.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Check USB storage"))
            checkUsb = button;
    }
    check(log && checkUsb, QStringLiteral("the Storage tab has a USB check and its log"));
    if (!log || !checkUsb)
        return;
    checkUsb->click();
    check(waitFor([&] { return log->toPlainText().contains(QStringLiteral("Recent kernel messages")); }), QStringLiteral("the USB check finishes"));
    const QString report = log->toPlainText();
    check(report.contains(QStringLiteral("USB devices: Flash Disk (1-1)")) && report.contains(QStringLiteral("Disks the kernel sees: sda, sda1"))
              && report.contains(QStringLiteral("found no filesystem it can read")) && report.contains(QStringLiteral("usb 1-1: new high-speed"))
              && !report.contains(QStringLiteral("crng")),
          QStringLiteral("the USB check reports the device, the kernel's disks, the reason and the USB kernel messages"));

    QPushButton *installSharing = nullptr;
    QPushButton *browse = nullptr;
    for (QPushButton *button : page.findChildren<QPushButton *>()) {
        if (button->text() == QStringLiteral("Install network sharing"))
            installSharing = button;
        if (button->text() == QStringLiteral("Browse files"))
            browse = button;
    }
    auto *shares = page.findChild<QTreeWidget *>(QStringLiteral("openWrtShares"));
    check(installSharing && !installSharing->isHidden() && installSharing->isEnabled() && browse && shares && shares->topLevelItemCount() == 0,
          QStringLiteral("without a file server the share note offers to install network sharing"));
    if (!installSharing || !browse || !shares)
        return;
    QTimer::singleShot(0, &page, [] {
        for (int i = 0; i < 100; ++i) {
            if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
                for (QAbstractButton *button : box->buttons()) {
                    if (button->text() == QStringLiteral("Install"))
                        button->click();
                }
                return;
            }
            QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        }
    });
    installSharing->click();
    check(waitFor([&] { return statusText().contains(QStringLiteral("Network sharing is installed")); }), QStringLiteral("installing network sharing finishes"));
    check(router.packageCommands == QStringList({QStringLiteral("update"), QStringLiteral("install ksmbd-server"), QStringLiteral("install luci-app-ksmbd")}),
          QStringLiteral("the package lists are updated, then ksmbd-server and luci-app-ksmbd are installed one at a time"));
    check(log->toPlainText().contains(QStringLiteral("luci-app-ksmbd is installed.")), QStringLiteral("the install progress goes to the Storage log"));
    check(waitFor([&] { return shares->topLevelItemCount() == 1 && installSharing->isHidden(); }),
          QStringLiteral("the shares list appears and the install button goes away"));
    if (shares->topLevelItemCount() != 1)
        return;
    shares->setCurrentItem(shares->topLevelItem(0));
    browse->click();
    check(browsedUrl == QStringLiteral("smb://127.0.0.1/media/") && browsedAsGuest, QStringLiteral("Browse files opens the share in Files as a guest when guests are allowed"));
}

}

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir settingsDir;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settingsDir.path());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshOpenWrtTests"));
    QCoreApplication::setApplicationName(QStringLiteral("openwrt-tests"));
    testModel();
    testClient();
    testStorageCheck();
    testFeature();
    return failures == 0 ? 0 : 1;
}
