#include "NexusCore.h"

#include <QHash>
#include <QJsonArray>
#include <QMap>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#include <algorithm>

namespace forkmesh::nexus {
namespace {

QList<quint16> range(int first, int last)
{
    QList<quint16> ports;
    ports.reserve(last - first + 1);
    for (int port = first; port <= last; ++port)
        ports << quint16(port);
    return ports;
}

bool inNetwork(const QHostAddress &address, const char *network, int prefix)
{
    return address.isInSubnet(QHostAddress(QString::fromLatin1(network)), prefix);
}

QHostAddress normalized(const QHostAddress &address)
{
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    return ok ? QHostAddress(v4) : address;
}

bool isMulticastOrBroadcast(const QHostAddress &address)
{
    const QHostAddress value = normalized(address);
    if (value.isMulticast() || value.isBroadcast())
        return true;
    if (value.protocol() == QAbstractSocket::IPv4Protocol)
        return inNetwork(value, "240.0.0.0", 4);
    return false;
}

bool isUnspecified(const QHostAddress &address)
{
    const QHostAddress value = normalized(address);
    return value == QHostAddress(QHostAddress::AnyIPv4) || value == QHostAddress(QHostAddress::AnyIPv6) || value.isNull();
}

QString cleanBanner(const QByteArray &banner)
{
    QString text = QString::fromLatin1(banner.left(200));
    const qsizetype end = text.indexOf(QRegularExpression(QStringLiteral("[\\r\\n]")));
    if (end >= 0)
        text.truncate(end);
    QString printable;
    for (const QChar character : std::as_const(text))
        printable += character.isPrint() ? character : QChar(u' ');
    return printable.simplified();
}

QString headerValue(const QByteArray &banner, const QByteArray &name)
{
    const QList<QByteArray> lines = banner.split('\n');
    for (const QByteArray &line : lines) {
        const qsizetype colon = line.indexOf(':');
        if (colon > 0 && line.left(colon).trimmed().toLower() == name)
            return QString::fromLatin1(line.mid(colon + 1).trimmed());
    }
    return {};
}

bool readName(const QByteArray &message, int &offset, QString &name)
{
    QStringList labels;
    int position = offset;
    bool jumped = false;
    int hops = 0;
    while (true) {
        if (position >= message.size())
            return false;
        const quint8 length = quint8(message.at(position));
        if (length == 0) {
            ++position;
            break;
        }
        if ((length & 0xC0) == 0xC0) {
            if (position + 1 >= message.size() || ++hops > 32)
                return false;
            const int pointer = ((length & 0x3F) << 8) | quint8(message.at(position + 1));
            if (!jumped)
                offset = position + 2;
            jumped = true;
            position = pointer;
            continue;
        }
        if ((length & 0xC0) != 0 || position + 1 + length > message.size())
            return false;
        labels << QString::fromUtf8(message.mid(position + 1, length));
        position += 1 + length;
    }
    if (!jumped)
        offset = position;
    name = labels.join(u'.');
    return true;
}

quint16 read16(const QByteArray &message, int offset)
{
    return quint16((quint8(message.at(offset)) << 8) | quint8(message.at(offset + 1)));
}

void appendName(QByteArray &out, const QString &name)
{
    const QStringList labels = name.split(u'.', Qt::SkipEmptyParts);
    for (const QString &label : labels) {
        const QByteArray bytes = label.toUtf8().left(63);
        out.append(char(bytes.size()));
        out.append(bytes);
    }
    out.append(char(0));
}

void append16(QByteArray &out, quint16 value)
{
    out.append(char(value >> 8));
    out.append(char(value & 0xFF));
}

QString instanceLabel(const QString &instance, const QString &type)
{
    if (instance.endsWith(type))
        return instance.left(instance.size() - type.size()).chopped(instance.size() > type.size() ? 1 : 0);
    return instance;
}

QString serviceTypeOf(const QString &instance)
{
    static const QRegularExpression pattern(QStringLiteral("(_[^.]+\\._(?:tcp|udp)\\.local)$"));
    const QRegularExpressionMatch match = pattern.match(instance);
    return match.hasMatch() ? match.captured(1) : QString();
}

}

const QList<KnownService> &serviceCatalog()
{
    static const QList<KnownService> services = {
        {7, QStringLiteral("echo"), QStringLiteral("Echo"), true, false},
        {9, QStringLiteral("discard"), QStringLiteral("Discard"), false, false},
        {13, QStringLiteral("daytime"), QStringLiteral("Daytime"), false, false},
        {20, QStringLiteral("ftp-data"), QStringLiteral("FTP data"), true, false},
        {21, QStringLiteral("ftp"), QStringLiteral("FTP"), true, true},
        {22, QStringLiteral("ssh"), QStringLiteral("SSH"), false, true},
        {23, QStringLiteral("telnet"), QStringLiteral("Telnet"), true, true},
        {25, QStringLiteral("smtp"), QStringLiteral("SMTP"), true, false},
        {26, QStringLiteral("smtp-alt"), QStringLiteral("SMTP (alternate)"), true, false},
        {37, QStringLiteral("time"), QStringLiteral("Time"), false, false},
        {53, QStringLiteral("domain"), QStringLiteral("DNS"), false, false},
        {69, QStringLiteral("tftp"), QStringLiteral("TFTP"), true, true},
        {79, QStringLiteral("finger"), QStringLiteral("Finger"), true, false},
        {80, QStringLiteral("http"), QStringLiteral("HTTP"), true, false},
        {81, QStringLiteral("http-alt"), QStringLiteral("HTTP (alternate)"), true, false},
        {88, QStringLiteral("kerberos"), QStringLiteral("Kerberos"), false, false},
        {106, QStringLiteral("pop3pw"), QStringLiteral("POP3 password"), true, false},
        {110, QStringLiteral("pop3"), QStringLiteral("POP3"), true, false},
        {111, QStringLiteral("rpcbind"), QStringLiteral("RPC portmapper"), false, true},
        {113, QStringLiteral("ident"), QStringLiteral("Ident"), false, false},
        {119, QStringLiteral("nntp"), QStringLiteral("NNTP"), true, false},
        {123, QStringLiteral("ntp"), QStringLiteral("NTP"), false, false},
        {135, QStringLiteral("msrpc"), QStringLiteral("Microsoft RPC"), false, true},
        {137, QStringLiteral("netbios-ns"), QStringLiteral("NetBIOS name"), false, false},
        {139, QStringLiteral("netbios-ssn"), QStringLiteral("NetBIOS session"), false, true},
        {143, QStringLiteral("imap"), QStringLiteral("IMAP"), true, false},
        {144, QStringLiteral("news"), QStringLiteral("NeWS"), false, false},
        {179, QStringLiteral("bgp"), QStringLiteral("BGP"), false, false},
        {199, QStringLiteral("smux"), QStringLiteral("SNMP multiplexer"), false, false},
        {389, QStringLiteral("ldap"), QStringLiteral("LDAP"), true, true},
        {427, QStringLiteral("svrloc"), QStringLiteral("Service Location Protocol"), false, false},
        {443, QStringLiteral("https"), QStringLiteral("HTTPS"), false, false},
        {444, QStringLiteral("snpp"), QStringLiteral("SNPP"), false, false},
        {445, QStringLiteral("microsoft-ds"), QStringLiteral("SMB"), false, true},
        {465, QStringLiteral("smtps"), QStringLiteral("SMTP over TLS"), false, false},
        {513, QStringLiteral("login"), QStringLiteral("rlogin"), true, true},
        {514, QStringLiteral("shell"), QStringLiteral("rsh"), true, true},
        {515, QStringLiteral("printer"), QStringLiteral("LPD printing"), false, false},
        {543, QStringLiteral("klogin"), QStringLiteral("Kerberos login"), false, false},
        {544, QStringLiteral("kshell"), QStringLiteral("Kerberos shell"), false, false},
        {548, QStringLiteral("afp"), QStringLiteral("Apple Filing Protocol"), false, true},
        {554, QStringLiteral("rtsp"), QStringLiteral("RTSP streaming"), true, false},
        {587, QStringLiteral("submission"), QStringLiteral("Mail submission"), false, false},
        {591, QStringLiteral("http-alt"), QStringLiteral("HTTP (FileMaker)"), true, false},
        {631, QStringLiteral("ipp"), QStringLiteral("Internet Printing Protocol"), false, false},
        {646, QStringLiteral("ldp"), QStringLiteral("LDP"), false, false},
        {873, QStringLiteral("rsync"), QStringLiteral("rsync"), true, true},
        {990, QStringLiteral("ftps"), QStringLiteral("FTP over TLS"), false, false},
        {993, QStringLiteral("imaps"), QStringLiteral("IMAP over TLS"), false, false},
        {995, QStringLiteral("pop3s"), QStringLiteral("POP3 over TLS"), false, false},
        {1025, QStringLiteral("nfs-or-iis"), QStringLiteral("Dynamic RPC"), false, false},
        {1110, QStringLiteral("nfsd-status"), QStringLiteral("NFS status"), false, false},
        {1400, QStringLiteral("sonos"), QStringLiteral("Sonos control"), true, false},
        {1433, QStringLiteral("ms-sql-s"), QStringLiteral("Microsoft SQL Server"), false, true},
        {1521, QStringLiteral("oracle"), QStringLiteral("Oracle database"), false, true},
        {1720, QStringLiteral("h323q931"), QStringLiteral("H.323"), false, false},
        {1723, QStringLiteral("pptp"), QStringLiteral("PPTP VPN"), false, true},
        {1755, QStringLiteral("wms"), QStringLiteral("Windows Media"), false, false},
        {1883, QStringLiteral("mqtt"), QStringLiteral("MQTT"), true, true},
        {1900, QStringLiteral("upnp"), QStringLiteral("UPnP / SSDP"), false, false},
        {2000, QStringLiteral("cisco-sccp"), QStringLiteral("Cisco SCCP"), false, false},
        {2049, QStringLiteral("nfs"), QStringLiteral("NFS"), false, true},
        {2121, QStringLiteral("ccproxy-ftp"), QStringLiteral("FTP (alternate)"), true, false},
        {2222, QStringLiteral("ssh-alt"), QStringLiteral("SSH (alternate)"), false, true},
        {3000, QStringLiteral("http-dev"), QStringLiteral("Web app (development)"), true, false},
        {3128, QStringLiteral("squid-http"), QStringLiteral("HTTP proxy"), true, false},
        {3306, QStringLiteral("mysql"), QStringLiteral("MySQL"), false, true},
        {3389, QStringLiteral("ms-wbt-server"), QStringLiteral("Remote Desktop"), false, true},
        {3689, QStringLiteral("daap"), QStringLiteral("DAAP (iTunes sharing)"), false, false},
        {4443, QStringLiteral("https-alt"), QStringLiteral("HTTPS (alternate)"), false, false},
        {4899, QStringLiteral("radmin"), QStringLiteral("Radmin"), false, true},
        {5000, QStringLiteral("upnp-or-http"), QStringLiteral("UPnP / web app"), true, false},
        {5001, QStringLiteral("https-alt"), QStringLiteral("HTTPS (alternate)"), false, false},
        {5060, QStringLiteral("sip"), QStringLiteral("SIP"), true, false},
        {5353, QStringLiteral("mdns"), QStringLiteral("Multicast DNS"), false, false},
        {5357, QStringLiteral("wsdapi"), QStringLiteral("Web Services for Devices"), false, false},
        {5432, QStringLiteral("postgresql"), QStringLiteral("PostgreSQL"), false, true},
        {5540, QStringLiteral("matter"), QStringLiteral("Matter"), false, false},
        {5631, QStringLiteral("pcanywhere"), QStringLiteral("pcAnywhere"), false, true},
        {5666, QStringLiteral("nrpe"), QStringLiteral("Nagios NRPE"), false, false},
        {5800, QStringLiteral("vnc-http"), QStringLiteral("VNC over HTTP"), true, true},
        {5900, QStringLiteral("vnc"), QStringLiteral("VNC"), false, true},
        {5901, QStringLiteral("vnc-1"), QStringLiteral("VNC display 1"), false, true},
        {5938, QStringLiteral("teamviewer"), QStringLiteral("TeamViewer"), false, true},
        {5984, QStringLiteral("couchdb"), QStringLiteral("CouchDB"), false, true},
        {5985, QStringLiteral("wsman"), QStringLiteral("WinRM (HTTP)"), true, true},
        {5986, QStringLiteral("wsmans"), QStringLiteral("WinRM (HTTPS)"), false, true},
        {6000, QStringLiteral("x11"), QStringLiteral("X11"), true, true},
        {6053, QStringLiteral("esphome"), QStringLiteral("ESPHome API"), false, false},
        {6379, QStringLiteral("redis"), QStringLiteral("Redis"), true, true},
        {6466, QStringLiteral("androidtv-remote"), QStringLiteral("Android TV remote"), false, false},
        {6467, QStringLiteral("androidtv-pairing"), QStringLiteral("Android TV pairing"), false, false},
        {7000, QStringLiteral("airplay"), QStringLiteral("AirPlay"), false, false},
        {7070, QStringLiteral("realserver"), QStringLiteral("RealServer / AnyDesk"), false, true},
        {7100, QStringLiteral("airplay-alt"), QStringLiteral("AirPlay (alternate)"), false, false},
        {7474, QStringLiteral("neo4j"), QStringLiteral("Neo4j"), false, true},
        {8000, QStringLiteral("http-alt"), QStringLiteral("HTTP (alternate)"), true, false},
        {8008, QStringLiteral("http-cast"), QStringLiteral("HTTP / Google Cast"), true, false},
        {8009, QStringLiteral("cast"), QStringLiteral("Google Cast"), false, false},
        {8060, QStringLiteral("roku-ecp"), QStringLiteral("Roku control"), true, false},
        {8080, QStringLiteral("http-proxy"), QStringLiteral("HTTP (alternate)"), true, false},
        {8081, QStringLiteral("http-alt"), QStringLiteral("HTTP (alternate)"), true, false},
        {8086, QStringLiteral("influxdb"), QStringLiteral("InfluxDB"), false, true},
        {8088, QStringLiteral("http-alt"), QStringLiteral("HTTP (alternate)"), true, false},
        {8096, QStringLiteral("jellyfin"), QStringLiteral("Jellyfin / Emby"), true, false},
        {8123, QStringLiteral("home-assistant"), QStringLiteral("Home Assistant"), true, false},
        {8200, QStringLiteral("dlna"), QStringLiteral("DLNA media server"), false, false},
        {8291, QStringLiteral("winbox"), QStringLiteral("MikroTik Winbox"), false, true},
        {8384, QStringLiteral("syncthing-gui"), QStringLiteral("Syncthing web UI"), true, false},
        {8443, QStringLiteral("https-alt"), QStringLiteral("HTTPS (alternate)"), false, false},
        {8883, QStringLiteral("secure-mqtt"), QStringLiteral("MQTT over TLS"), false, false},
        {8888, QStringLiteral("http-alt"), QStringLiteral("HTTP (alternate)"), true, false},
        {9000, QStringLiteral("http-alt"), QStringLiteral("Web app / management"), true, false},
        {9042, QStringLiteral("cassandra"), QStringLiteral("Cassandra"), false, true},
        {9090, QStringLiteral("http-mgmt"), QStringLiteral("Management web UI"), true, false},
        {9100, QStringLiteral("jetdirect"), QStringLiteral("Raw printing (JetDirect)"), false, false},
        {9200, QStringLiteral("elasticsearch"), QStringLiteral("Elasticsearch"), true, true},
        {9443, QStringLiteral("https-alt"), QStringLiteral("HTTPS (alternate)"), false, false},
        {9999, QStringLiteral("abyss"), QStringLiteral("Management / debug"), false, false},
        {10000, QStringLiteral("webmin"), QStringLiteral("Webmin / NDMP"), false, true},
        {11211, QStringLiteral("memcached"), QStringLiteral("Memcached"), true, true},
        {21063, QStringLiteral("homekit"), QStringLiteral("HomeKit accessory"), false, false},
        {22000, QStringLiteral("syncthing"), QStringLiteral("Syncthing"), false, false},
        {27017, QStringLiteral("mongodb"), QStringLiteral("MongoDB"), false, true},
        {28015, QStringLiteral("rethinkdb"), QStringLiteral("RethinkDB"), false, true},
        {32400, QStringLiteral("plex"), QStringLiteral("Plex Media Server"), false, false},
        {49152, QStringLiteral("upnp-dynamic"), QStringLiteral("UPnP / dynamic"), false, false},
        {55443, QStringLiteral("camera-rtsp"), QStringLiteral("Camera streaming"), false, false},
        {62078, QStringLiteral("iphone-sync"), QStringLiteral("Apple device sync (lockdown)"), false, false},
    };
    return services;
}

const KnownService *serviceForPort(quint16 port)
{
    static const QHash<quint16, int> index = [] {
        QHash<quint16, int> table;
        const QList<KnownService> &services = serviceCatalog();
        for (int i = 0; i < services.size(); ++i) {
            if (!table.contains(services.at(i).port))
                table.insert(services.at(i).port, i);
        }
        return table;
    }();
    const auto found = index.constFind(port);
    return found == index.constEnd() ? nullptr : &serviceCatalog().at(*found);
}

QList<PortPreset> portPresets()
{
    return {PortPreset::Quick, PortPreset::Common, PortPreset::Web, PortPreset::RemoteAccess, PortPreset::FileSharing,
            PortPreset::Mail, PortPreset::SmartHome, PortPreset::Media, PortPreset::Databases, PortPreset::WellKnown};
}

QString presetTitle(PortPreset preset)
{
    switch (preset) {
    case PortPreset::Quick: return QStringLiteral("Quick (20)");
    case PortPreset::Common: return QStringLiteral("Common (100)");
    case PortPreset::Web: return QStringLiteral("Web");
    case PortPreset::RemoteAccess: return QStringLiteral("Remote access");
    case PortPreset::FileSharing: return QStringLiteral("File sharing");
    case PortPreset::Mail: return QStringLiteral("Mail");
    case PortPreset::SmartHome: return QStringLiteral("Smart home & IoT");
    case PortPreset::Media: return QStringLiteral("Media & casting");
    case PortPreset::Databases: return QStringLiteral("Databases");
    case PortPreset::WellKnown: return QStringLiteral("Well-known (1–1024)");
    }
    return {};
}

QList<quint16> presetPorts(PortPreset preset)
{
    switch (preset) {
    case PortPreset::Quick:
        return {21, 22, 23, 25, 53, 80, 110, 139, 143, 443, 445, 554, 3306, 3389, 5900, 8000, 8080, 8443, 9100, 62078};
    case PortPreset::Common:
        return {7, 9, 13, 21, 22, 23, 25, 26, 37, 53, 79, 80, 81, 88, 106, 110, 111, 113, 119, 135,
                139, 143, 144, 179, 199, 389, 427, 443, 444, 445, 465, 513, 514, 515, 543, 544, 548, 554, 587, 631,
                646, 873, 990, 993, 995, 1025, 1026, 1027, 1028, 1029, 1110, 1433, 1720, 1723, 1755, 1883, 1900, 2000, 2001, 2049,
                2121, 2717, 3000, 3128, 3306, 3389, 3986, 4899, 5000, 5009, 5051, 5060, 5101, 5190, 5357, 5432, 5631, 5666, 5800, 5900,
                6000, 6001, 6646, 7070, 8000, 8008, 8009, 8080, 8081, 8443, 8888, 9100, 9999, 10000, 32768, 49152, 49153, 49154, 49155, 62078};
    case PortPreset::Web:
        return {80, 81, 443, 591, 3000, 4443, 5000, 5001, 7080, 7443, 8000, 8008, 8080, 8081, 8088, 8443, 8888, 9000, 9090, 9443};
    case PortPreset::RemoteAccess:
        return {22, 23, 2222, 3389, 5900, 5901, 5938, 5985, 5986, 6568, 7070, 8291};
    case PortPreset::FileSharing:
        return {20, 21, 69, 111, 135, 137, 139, 445, 548, 873, 2049, 5005, 8384, 22000};
    case PortPreset::Mail:
        return {25, 110, 143, 465, 587, 993, 995, 4190};
    case PortPreset::SmartHome:
        return {80, 443, 554, 1400, 1883, 1900, 5353, 5540, 6053, 6466, 6467, 8008, 8009, 8060, 8123, 8883, 9000, 9123, 21063, 40000, 49152, 55443};
    case PortPreset::Media:
        return {554, 1400, 3689, 5000, 7000, 7100, 8008, 8009, 8060, 8096, 8200, 8443, 9080, 32400, 49153};
    case PortPreset::Databases:
        return {1433, 1521, 3306, 5432, 5984, 6379, 7474, 8086, 9042, 9200, 11211, 27017, 28015};
    case PortPreset::WellKnown:
        return range(1, 1024);
    }
    return {};
}

PortParse parsePorts(const QString &text)
{
    PortParse parse;
    const QStringList tokens = text.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
    if (tokens.isEmpty()) {
        parse.error = QStringLiteral("Enter at least one port.");
        return parse;
    }
    QSet<quint16> seen;
    for (const QString &token : tokens) {
        const QStringList bounds = token.split(u'-');
        if (bounds.size() > 2) {
            parse.error = QStringLiteral("“%1” is not a port or port range.").arg(token);
            return parse;
        }
        bool okFirst = false;
        bool okLast = false;
        const int first = bounds.first().toInt(&okFirst);
        const int last = bounds.size() == 2 ? bounds.last().toInt(&okLast) : first;
        if (bounds.size() == 1)
            okLast = okFirst;
        if (!okFirst || !okLast) {
            parse.error = QStringLiteral("“%1” is not a port or port range.").arg(token);
            return parse;
        }
        if (first < 1 || first > 65535 || last < 1 || last > 65535) {
            parse.error = QStringLiteral("“%1” is outside 1–65535.").arg(token);
            return parse;
        }
        if (last < first) {
            parse.error = QStringLiteral("The range “%1” ends before it starts.").arg(token);
            return parse;
        }
        for (int port = first; port <= last; ++port)
            seen.insert(quint16(port));
    }
    parse.ports = QList<quint16>(seen.cbegin(), seen.cend());
    std::sort(parse.ports.begin(), parse.ports.end());
    return parse;
}

QString describePorts(const QList<quint16> &ports)
{
    QList<quint16> sorted = ports;
    std::sort(sorted.begin(), sorted.end());
    sorted.erase(std::unique(sorted.begin(), sorted.end()), sorted.end());
    QStringList parts;
    for (int i = 0; i < sorted.size();) {
        int j = i;
        while (j + 1 < sorted.size() && sorted.at(j + 1) == sorted.at(j) + 1)
            ++j;
        parts << (j > i + 1 ? QStringLiteral("%1-%2").arg(sorted.at(i)).arg(sorted.at(j))
                            : QString::number(sorted.at(i)));
        if (j == i + 1)
            parts << QString::number(sorted.at(j));
        i = j + 1;
    }
    return parts.join(u',');
}

TargetParse parseTarget(const QString &text, int maximumHosts)
{
    TargetParse parse;
    const QString target = text.trimmed();
    if (target.isEmpty()) {
        parse.error = QStringLiteral("Enter a host, IP address, range or CIDR network.");
        return parse;
    }
    if (target.contains(u'/')) {
        const QPair<QHostAddress, int> network = QHostAddress::parseSubnet(target);
        if (network.first.isNull() || network.second < 0) {
            parse.error = QStringLiteral("“%1” is not a valid CIDR network.").arg(target);
            return parse;
        }
        parse.isRange = true;
        if (network.first.protocol() != QAbstractSocket::IPv4Protocol) {
            if (network.second < 128) {
                parse.error = QStringLiteral("IPv6 networks are too large to scan; enter single IPv6 addresses.");
                return parse;
            }
            parse.addresses << network.first;
            parse.isRange = false;
            return parse;
        }
        const int prefix = network.second;
        const quint64 size = quint64(1) << (32 - prefix);
        if (size > quint64(maximumHosts) + 2) {
            parse.error = QStringLiteral("/%1 holds %2 addresses, more than the limit of %3 hosts per scan.")
                              .arg(prefix).arg(size).arg(maximumHosts);
            return parse;
        }
        const quint32 mask = prefix == 0 ? 0u : ~quint32(0) << (32 - prefix);
        const quint32 base = network.first.toIPv4Address() & mask;
        quint64 first = base;
        quint64 last = base + size - 1;
        if (prefix <= 30) {
            ++first;
            --last;
        }
        for (quint64 value = first; value <= last; ++value)
            parse.addresses << QHostAddress(quint32(value));
        return parse;
    }
    const qsizetype dash = target.indexOf(u'-');
    if (dash > 0) {
        const QHostAddress start(target.left(dash).trimmed());
        const QString tail = target.mid(dash + 1).trimmed();
        if (!start.isNull() && start.protocol() == QAbstractSocket::IPv4Protocol) {
            QHostAddress end = tail.contains(u'.') ? QHostAddress(tail) : QHostAddress();
            if (!tail.contains(u'.')) {
                bool ok = false;
                const int octet = tail.toInt(&ok);
                if (!ok || octet < 0 || octet > 255) {
                    parse.error = QStringLiteral("“%1” is not a valid address range.").arg(target);
                    return parse;
                }
                end = QHostAddress((start.toIPv4Address() & 0xFFFFFF00u) | quint32(octet));
            }
            if (end.protocol() != QAbstractSocket::IPv4Protocol) {
                parse.error = QStringLiteral("“%1” is not a valid address range.").arg(target);
                return parse;
            }
            if (end.toIPv4Address() < start.toIPv4Address()) {
                parse.error = QStringLiteral("The range “%1” ends before it starts.").arg(target);
                return parse;
            }
            const quint64 count = quint64(end.toIPv4Address()) - start.toIPv4Address() + 1;
            if (count > quint64(maximumHosts)) {
                parse.error = QStringLiteral("%1 hosts exceeds the limit of %2 per scan.").arg(count).arg(maximumHosts);
                return parse;
            }
            parse.isRange = count > 1;
            for (quint64 value = start.toIPv4Address(); value <= end.toIPv4Address(); ++value)
                parse.addresses << QHostAddress(quint32(value));
            return parse;
        }
    }
    const QHostAddress address(target);
    if (!address.isNull()) {
        parse.addresses << address;
        return parse;
    }
    static const QRegularExpression hostname(QStringLiteral("^(?=.{1,253}$)([A-Za-z0-9_](?:[A-Za-z0-9_-]{0,61}[A-Za-z0-9])?)(\\.[A-Za-z0-9_](?:[A-Za-z0-9_-]{0,61}[A-Za-z0-9])?)*\\.?$"));
    if (!hostname.match(target).hasMatch()) {
        parse.error = QStringLiteral("“%1” is not a host name, IP address, range or CIDR network.").arg(target);
        return parse;
    }
    parse.hostname = target;
    return parse;
}

QList<Network> parseNetworks(const QString &text, QString *error)
{
    QList<Network> networks;
    const QStringList tokens = text.split(QRegularExpression(QStringLiteral("[,\\s]+")), Qt::SkipEmptyParts);
    for (const QString &token : tokens) {
        Network network = token.contains(u'/') ? QHostAddress::parseSubnet(token)
                                               : Network(QHostAddress(token), QHostAddress(token).protocol() == QAbstractSocket::IPv4Protocol ? 32 : 128);
        if (network.first.isNull() || network.second < 0) {
            if (error)
                *error = QStringLiteral("“%1” is not an address or CIDR network.").arg(token);
            continue;
        }
        networks << network;
    }
    return networks;
}

QString describeNetworks(const QList<Network> &networks)
{
    QStringList parts;
    for (const Network &network : networks)
        parts << QStringLiteral("%1/%2").arg(network.first.toString()).arg(network.second);
    return parts.join(QStringLiteral(", "));
}

TargetScope classify(const QHostAddress &input, const QList<Network> &authorized)
{
    const QHostAddress address = normalized(input);
    if (address.isLoopback())
        return TargetScope::Loopback;
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        if (inNetwork(address, "10.0.0.0", 8) || inNetwork(address, "172.16.0.0", 12) || inNetwork(address, "192.168.0.0", 16)
            || inNetwork(address, "169.254.0.0", 16) || inNetwork(address, "100.64.0.0", 10))
            return TargetScope::LocalNetwork;
    } else if (inNetwork(address, "fe80::", 10) || inNetwork(address, "fc00::", 7)) {
        return TargetScope::LocalNetwork;
    }
    for (const Network &network : authorized) {
        if (address.isInSubnet(network.first, network.second))
            return TargetScope::AuthorizedNetwork;
    }
    return TargetScope::PublicInternet;
}

QString scopeTitle(TargetScope scope)
{
    switch (scope) {
    case TargetScope::Loopback: return QStringLiteral("This computer");
    case TargetScope::LocalNetwork: return QStringLiteral("Local network");
    case TargetScope::AuthorizedNetwork: return QStringLiteral("Authorized network");
    case TargetScope::PublicInternet: return QStringLiteral("Internet");
    }
    return {};
}

Decision evaluate(const ScanPolicy &policy, const QList<QHostAddress> &addresses, int portCount, bool isRange, bool attested)
{
    if (addresses.isEmpty())
        return {DecisionKind::Denied, QStringLiteral("No addresses to scan.")};
    for (const QHostAddress &address : addresses) {
        if (isMulticastOrBroadcast(address) || isUnspecified(address))
            return {DecisionKind::Denied, QStringLiteral("%1 is a multicast, broadcast or unspecified address and cannot be scanned.").arg(address.toString())};
    }
    if (addresses.size() > policy.maximumHosts)
        return {DecisionKind::Denied, QStringLiteral("%1 hosts exceeds the limit of %2 per scan.").arg(addresses.size()).arg(policy.maximumHosts)};
    const qint64 probes = qint64(addresses.size()) * portCount;
    if (probes > policy.maximumProbes)
        return {DecisionKind::Denied, QStringLiteral("%1 connection attempts exceeds the limit of %2 per scan.").arg(probes).arg(policy.maximumProbes)};
    int publicCount = 0;
    for (const QHostAddress &address : addresses) {
        if (classify(address, policy.authorizedNetworks) == TargetScope::PublicInternet)
            ++publicCount;
    }
    if (publicCount == 0)
        return {DecisionKind::Allowed, {}};
    if (!policy.allowPublicTargets)
        return {DecisionKind::Denied, QStringLiteral("Internet addresses are off. Turn on “Allow internet targets” in Settings, or add the network to Authorized networks.")};
    if (isRange)
        return {DecisionKind::Denied, QStringLiteral("Ranges of internet addresses can only be scanned when the network is listed under Authorized networks.")};
    if (publicCount > policy.maximumPublicHosts)
        return {DecisionKind::Denied, QStringLiteral("%1 internet hosts exceeds the limit of %2 per scan.").arg(publicCount).arg(policy.maximumPublicHosts)};
    if (!attested)
        return {DecisionKind::NeedsAttestation, QStringLiteral("Confirm you own or are authorized to test the internet hosts in this scan.")};
    return {DecisionKind::Allowed, {}};
}

QString stateTitle(PortState state)
{
    switch (state) {
    case PortState::Open: return QStringLiteral("Open");
    case PortState::Closed: return QStringLiteral("Closed");
    case PortState::NoResponse: return QStringLiteral("No response");
    }
    return {};
}

QString stateName(PortState state)
{
    switch (state) {
    case PortState::Open: return QStringLiteral("open");
    case PortState::Closed: return QStringLiteral("closed");
    case PortState::NoResponse: return QStringLiteral("noResponse");
    }
    return {};
}

PortState stateFromName(const QString &name)
{
    if (name == QLatin1String("open"))
        return PortState::Open;
    if (name == QLatin1String("closed"))
        return PortState::Closed;
    return PortState::NoResponse;
}

bool sendsHttpProbe(quint16 port)
{
    const KnownService *service = serviceForPort(port);
    return service && service->cleartext && service->name.startsWith(QLatin1String("http"));
}

ServiceGuess identifyService(quint16 port, const QByteArray &banner)
{
    const QString line = cleanBanner(banner);
    if (line.startsWith(QLatin1String("SSH-")))
        return {QStringLiteral("ssh"), line, true};
    if (banner.startsWith("HTTP/")) {
        const QString server = headerValue(banner, "server");
        return {QStringLiteral("http"), server.isEmpty() ? line : QStringLiteral("%1 · %2").arg(line, server), true};
    }
    if (line.startsWith(QLatin1String("RFB ")))
        return {QStringLiteral("vnc"), line, true};
    if (line.startsWith(QLatin1String("+OK")))
        return {QStringLiteral("pop3"), line, true};
    if (line.startsWith(QLatin1String("* OK")))
        return {QStringLiteral("imap"), line, true};
    if (line.startsWith(QLatin1String("220"))) {
        const QString lower = line.toLower();
        if (lower.contains(QLatin1String("smtp")) || lower.contains(QLatin1String("mail")) || port == 25 || port == 587)
            return {QStringLiteral("smtp"), line, true};
        return {QStringLiteral("ftp"), line, true};
    }
    const KnownService *service = serviceForPort(port);
    return {service ? service->name : QStringLiteral("unknown"), line, false};
}

int openCount(const ScanSession &session)
{
    return int(std::count_if(session.results.cbegin(), session.results.cend(),
                             [](const PortResult &result) { return result.state == PortState::Open; }));
}

QJsonObject scanToJson(const ScanSession &session)
{
    QJsonArray results;
    for (const PortResult &result : session.results) {
        QJsonObject row{{QStringLiteral("address"), result.address},
                        {QStringLiteral("port"), int(result.port)},
                        {QStringLiteral("state"), stateName(result.state)},
                        {QStringLiteral("service"), result.service},
                        {QStringLiteral("confirmed"), result.confirmed}};
        if (!result.detail.isEmpty())
            row.insert(QStringLiteral("detail"), result.detail);
        if (result.latencyMs >= 0)
            row.insert(QStringLiteral("latencyMs"), result.latencyMs);
        results << row;
    }
    return {{QStringLiteral("id"), session.id},
            {QStringLiteral("target"), session.target},
            {QStringLiteral("ports"), session.ports},
            {QStringLiteral("startedAt"), session.startedAt.toUTC().toString(Qt::ISODate)},
            {QStringLiteral("finishedAt"), session.finishedAt.toUTC().toString(Qt::ISODate)},
            {QStringLiteral("cancelled"), session.cancelled},
            {QStringLiteral("probes"), session.probes},
            {QStringLiteral("results"), results}};
}

ScanSession scanFromJson(const QJsonObject &object)
{
    ScanSession session;
    session.id = object.value(QStringLiteral("id")).toString();
    if (session.id.isEmpty())
        session.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    session.target = object.value(QStringLiteral("target")).toString();
    session.ports = object.value(QStringLiteral("ports")).toString();
    session.startedAt = QDateTime::fromString(object.value(QStringLiteral("startedAt")).toString(), Qt::ISODate);
    session.finishedAt = QDateTime::fromString(object.value(QStringLiteral("finishedAt")).toString(), Qt::ISODate);
    session.cancelled = object.value(QStringLiteral("cancelled")).toBool();
    session.probes = object.value(QStringLiteral("probes")).toInt();
    const QJsonArray results = object.value(QStringLiteral("results")).toArray();
    for (const QJsonValue &value : results) {
        const QJsonObject row = value.toObject();
        PortResult result;
        result.address = row.value(QStringLiteral("address")).toString();
        result.port = quint16(row.value(QStringLiteral("port")).toInt());
        result.state = stateFromName(row.value(QStringLiteral("state")).toString());
        result.service = row.value(QStringLiteral("service")).toString();
        result.detail = row.value(QStringLiteral("detail")).toString();
        result.confirmed = row.value(QStringLiteral("confirmed")).toBool();
        result.latencyMs = row.value(QStringLiteral("latencyMs")).toInt(-1);
        if (result.port != 0)
            session.results << result;
    }
    return session;
}

QByteArray scanCsv(const ScanSession &session)
{
    auto quote = [](QString value) {
        if (!value.isEmpty() && QStringLiteral("=+-@").contains(value.front()))
            value.prepend(u'\'');
        return QStringLiteral("\"%1\"").arg(value.replace(u'"', QStringLiteral("\"\"")));
    };
    QByteArray csv = "address,port,state,service,confirmed,latency_ms,detail\n";
    for (const PortResult &result : session.results) {
        const QStringList fields{quote(result.address), QString::number(result.port), stateName(result.state),
                                 quote(result.service), result.confirmed ? QStringLiteral("true") : QStringLiteral("false"),
                                 result.latencyMs >= 0 ? QString::number(result.latencyMs) : QString(), quote(result.detail)};
        csv += fields.join(u',').toUtf8() + '\n';
    }
    return csv;
}

QStringList portChanges(const ScanSession &current, const ScanSession *previous)
{
    QStringList changes;
    QSet<QString> before;
    if (previous) {
        if (previous->target != current.target)
            previous = nullptr;
        else {
            for (const PortResult &result : previous->results) {
                if (result.state == PortState::Open)
                    before.insert(QStringLiteral("%1:%2").arg(result.address).arg(result.port));
            }
        }
    }
    QSet<QString> now;
    for (const PortResult &result : current.results) {
        if (result.state != PortState::Open)
            continue;
        const QString key = QStringLiteral("%1:%2").arg(result.address).arg(result.port);
        now.insert(key);
        const KnownService *service = serviceForPort(result.port);
        if (previous && !before.contains(key))
            changes << QStringLiteral("Newly open since the last scan: %1").arg(key);
        if (service && service->cleartext && service->sensitive)
            changes << QStringLiteral("%1 accepts %2, which sends data unencrypted").arg(key, service->title);
    }
    if (previous) {
        for (const QString &key : std::as_const(before)) {
            if (!now.contains(key))
                changes << QStringLiteral("No longer open: %1").arg(key);
        }
    }
    return changes;
}

QByteArray mdnsQuery(const QStringList &names, quint16 type, quint16 id)
{
    QByteArray out;
    append16(out, id);
    append16(out, 0);
    append16(out, quint16(names.size()));
    append16(out, 0);
    append16(out, 0);
    append16(out, 0);
    for (const QString &name : names) {
        appendName(out, name);
        append16(out, type);
        append16(out, 1);
    }
    return out;
}

QList<DnsRecord> parseDnsMessage(const QByteArray &message)
{
    QList<DnsRecord> records;
    if (message.size() < 12)
        return records;
    const int questions = read16(message, 4);
    const int answers = read16(message, 6) + read16(message, 8) + read16(message, 10);
    int offset = 12;
    for (int i = 0; i < questions; ++i) {
        QString name;
        if (!readName(message, offset, name) || offset + 4 > message.size())
            return records;
        offset += 4;
    }
    for (int i = 0; i < answers; ++i) {
        DnsRecord record;
        if (!readName(message, offset, record.name) || offset + 10 > message.size())
            return records;
        record.type = read16(message, offset);
        const int length = read16(message, offset + 8);
        offset += 10;
        if (offset + length > message.size())
            return records;
        const int end = offset + length;
        switch (record.type) {
        case DnsTypeA:
            if (length == 4)
                record.address = QHostAddress(quint32((quint8(message.at(offset)) << 24) | (quint8(message.at(offset + 1)) << 16)
                                                      | (quint8(message.at(offset + 2)) << 8) | quint8(message.at(offset + 3))));
            break;
        case DnsTypeAaaa:
            if (length == 16)
                record.address = QHostAddress(reinterpret_cast<const quint8 *>(message.constData() + offset));
            break;
        case DnsTypePtr: {
            int cursor = offset;
            readName(message, cursor, record.target);
            break;
        }
        case DnsTypeSrv:
            if (length >= 7) {
                record.port = read16(message, offset + 4);
                int cursor = offset + 6;
                readName(message, cursor, record.target);
            }
            break;
        case DnsTypeTxt: {
            int cursor = offset;
            while (cursor < end) {
                const int size = quint8(message.at(cursor));
                if (cursor + 1 + size > end)
                    break;
                if (size > 0)
                    record.text << QString::fromUtf8(message.mid(cursor + 1, size));
                cursor += 1 + size;
            }
            break;
        }
        default:
            break;
        }
        offset = end;
        records << record;
    }
    return records;
}

QStringList defaultServiceTypes()
{
    return {QStringLiteral("_http._tcp.local"), QStringLiteral("_https._tcp.local"), QStringLiteral("_ssh._tcp.local"),
            QStringLiteral("_smb._tcp.local"), QStringLiteral("_airplay._tcp.local"), QStringLiteral("_raop._tcp.local"),
            QStringLiteral("_ipp._tcp.local"), QStringLiteral("_printer._tcp.local"), QStringLiteral("_homekit._tcp.local"),
            QStringLiteral("_googlecast._tcp.local"), QStringLiteral("_spotify-connect._tcp.local"), QStringLiteral("_sftp-ssh._tcp.local"),
            QStringLiteral("_workstation._tcp.local"), QStringLiteral("_device-info._tcp.local"), QStringLiteral("_hap._tcp.local"),
            QStringLiteral("_matter._tcp.local")};
}

QString serviceTypeTitle(const QString &type)
{
    static const QMap<QString, QString> titles{
        {QStringLiteral("_http._tcp.local"), QStringLiteral("Web")},
        {QStringLiteral("_https._tcp.local"), QStringLiteral("Secure web")},
        {QStringLiteral("_ssh._tcp.local"), QStringLiteral("SSH")},
        {QStringLiteral("_sftp-ssh._tcp.local"), QStringLiteral("SFTP")},
        {QStringLiteral("_smb._tcp.local"), QStringLiteral("File sharing (SMB)")},
        {QStringLiteral("_airplay._tcp.local"), QStringLiteral("AirPlay")},
        {QStringLiteral("_raop._tcp.local"), QStringLiteral("AirPlay audio")},
        {QStringLiteral("_ipp._tcp.local"), QStringLiteral("Printer (IPP)")},
        {QStringLiteral("_printer._tcp.local"), QStringLiteral("Printer (LPD)")},
        {QStringLiteral("_homekit._tcp.local"), QStringLiteral("HomeKit")},
        {QStringLiteral("_hap._tcp.local"), QStringLiteral("HomeKit accessory")},
        {QStringLiteral("_googlecast._tcp.local"), QStringLiteral("Google Cast")},
        {QStringLiteral("_spotify-connect._tcp.local"), QStringLiteral("Spotify Connect")},
        {QStringLiteral("_workstation._tcp.local"), QStringLiteral("Workstation")},
        {QStringLiteral("_device-info._tcp.local"), QStringLiteral("Device info")},
        {QStringLiteral("_matter._tcp.local"), QStringLiteral("Matter")},
    };
    const QString title = titles.value(type);
    if (!title.isEmpty())
        return title;
    QString name = type;
    name.remove(QStringLiteral(".local"));
    return name;
}

QList<DiscoveredDevice> devicesFromRecords(const QList<DnsRecord> &records)
{
    QHash<QString, QList<QHostAddress>> addressesByHost;
    QHash<QString, QPair<QString, quint16>> srvByInstance;
    QSet<QString> instances;
    for (const DnsRecord &record : records) {
        if ((record.type == DnsTypeA || record.type == DnsTypeAaaa) && !record.address.isNull()) {
            QList<QHostAddress> &list = addressesByHost[record.name.toLower()];
            if (!list.contains(record.address))
                list << record.address;
        } else if (record.type == DnsTypeSrv && !record.target.isEmpty()) {
            srvByInstance.insert(record.name, {record.target.toLower(), record.port});
            instances.insert(record.name);
        } else if (record.type == DnsTypePtr && !serviceTypeOf(record.target).isEmpty()
                   && !record.name.startsWith(QLatin1String("_services._dns-sd"))) {
            instances.insert(record.target);
        }
    }
    QMap<QString, DiscoveredDevice> devices;
    for (const QString &instance : std::as_const(instances)) {
        const QString type = serviceTypeOf(instance);
        const auto srv = srvByInstance.constFind(instance);
        const QString host = srv == srvByInstance.constEnd() ? QString() : srv->first;
        const QString key = host.isEmpty() ? instanceLabel(instance, type).toLower() : host;
        DiscoveredDevice &device = devices[key];
        if (device.name.isEmpty())
            device.name = instanceLabel(instance, type);
        if (!host.isEmpty() && !device.hostnames.contains(host))
            device.hostnames << host;
        const QString title = serviceTypeTitle(type);
        if (!type.isEmpty() && !device.services.contains(title))
            device.services << title;
        if (srv != srvByInstance.constEnd() && srv->second != 0 && !device.ports.contains(srv->second))
            device.ports << srv->second;
        for (const QHostAddress &address : addressesByHost.value(host)) {
            const QString text = address.toString();
            if (!device.addresses.contains(text))
                device.addresses << text;
        }
        device.lastSeen = QDateTime::currentDateTimeUtc();
    }
    QList<DiscoveredDevice> list = devices.values();
    for (DiscoveredDevice &device : list) {
        std::sort(device.ports.begin(), device.ports.end());
        std::sort(device.services.begin(), device.services.end());
    }
    std::sort(list.begin(), list.end(), [](const DiscoveredDevice &a, const DiscoveredDevice &b) {
        return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
    });
    return list;
}

QJsonObject deviceToJson(const DiscoveredDevice &device)
{
    QJsonArray ports;
    for (quint16 port : device.ports)
        ports << int(port);
    return {{QStringLiteral("name"), device.name},
            {QStringLiteral("hostnames"), QJsonArray::fromStringList(device.hostnames)},
            {QStringLiteral("addresses"), QJsonArray::fromStringList(device.addresses)},
            {QStringLiteral("services"), QJsonArray::fromStringList(device.services)},
            {QStringLiteral("ports"), ports},
            {QStringLiteral("lastSeen"), device.lastSeen.toUTC().toString(Qt::ISODate)}};
}

DiscoveredDevice deviceFromJson(const QJsonObject &object)
{
    DiscoveredDevice device;
    device.name = object.value(QStringLiteral("name")).toString();
    auto strings = [&object](const QString &key) {
        QStringList list;
        for (const QJsonValue &value : object.value(key).toArray())
            list << value.toString();
        return list;
    };
    device.hostnames = strings(QStringLiteral("hostnames"));
    device.addresses = strings(QStringLiteral("addresses"));
    device.services = strings(QStringLiteral("services"));
    for (const QJsonValue &value : object.value(QStringLiteral("ports")).toArray())
        device.ports << quint16(value.toInt());
    device.lastSeen = QDateTime::fromString(object.value(QStringLiteral("lastSeen")).toString(), Qt::ISODate);
    return device;
}

QList<HeaderFinding> headerFindings(const QList<QPair<QByteArray, QByteArray>> &headers, bool https)
{
    QHash<QByteArray, QByteArray> values;
    for (const auto &header : headers)
        values.insert(header.first.toLower(), header.second);
    QList<HeaderFinding> findings;
    if (!https)
        findings << HeaderFinding{QStringLiteral("warning"), QStringLiteral("The page was served over plain HTTP, so it can be read or changed in transit.")};
    if (https && !values.contains("strict-transport-security"))
        findings << HeaderFinding{QStringLiteral("review"), QStringLiteral("No Strict-Transport-Security header: browsers may still try plain HTTP first.")};
    if (!values.contains("content-security-policy"))
        findings << HeaderFinding{QStringLiteral("review"), QStringLiteral("No Content-Security-Policy header.")};
    if (values.value("x-content-type-options").toLower() != "nosniff")
        findings << HeaderFinding{QStringLiteral("review"), QStringLiteral("X-Content-Type-Options is not set to nosniff.")};
    if (!values.contains("x-frame-options") && !values.value("content-security-policy").contains("frame-ancestors"))
        findings << HeaderFinding{QStringLiteral("review"), QStringLiteral("Neither X-Frame-Options nor a CSP frame-ancestors rule limits framing.")};
    if (!values.contains("referrer-policy"))
        findings << HeaderFinding{QStringLiteral("info"), QStringLiteral("No Referrer-Policy header; the browser default applies.")};
    const QByteArray server = values.value("server");
    if (!server.isEmpty() && QRegularExpression(QStringLiteral("\\d")).match(QString::fromLatin1(server)).hasMatch())
        findings << HeaderFinding{QStringLiteral("info"), QStringLiteral("The Server header names a version: %1").arg(QString::fromLatin1(server))};
    if (values.contains("x-powered-by"))
        findings << HeaderFinding{QStringLiteral("info"), QStringLiteral("X-Powered-By names the platform: %1").arg(QString::fromLatin1(values.value("x-powered-by")))};
    return findings;
}

QJsonObject archiveToJson(const NexusArchive &archive)
{
    QJsonArray scans;
    for (const ScanSession &session : archive.scans.mid(0, MaximumStoredScans))
        scans << scanToJson(session);
    QJsonArray devices;
    for (const DiscoveredDevice &device : archive.devices)
        devices << deviceToJson(device);
    return {{QStringLiteral("version"), 1},
            {QStringLiteral("settings"), QJsonObject{{QStringLiteral("timeoutMs"), archive.settings.timeoutMs},
                                                     {QStringLiteral("concurrency"), archive.settings.concurrency},
                                                     {QStringLiteral("allowPublicTargets"), archive.settings.allowPublicTargets},
                                                     {QStringLiteral("authorizedNetworks"), archive.settings.authorizedNetworks}}},
            {QStringLiteral("scans"), scans},
            {QStringLiteral("devices"), devices}};
}

NexusArchive archiveFromJson(const QJsonObject &object)
{
    NexusArchive archive;
    const QJsonObject settings = object.value(QStringLiteral("settings")).toObject();
    archive.settings.timeoutMs = qBound(200, settings.value(QStringLiteral("timeoutMs")).toInt(1500), 30000);
    archive.settings.concurrency = qBound(1, settings.value(QStringLiteral("concurrency")).toInt(48), 256);
    archive.settings.allowPublicTargets = settings.value(QStringLiteral("allowPublicTargets")).toBool();
    archive.settings.authorizedNetworks = settings.value(QStringLiteral("authorizedNetworks")).toString();
    for (const QJsonValue &value : object.value(QStringLiteral("scans")).toArray()) {
        if (archive.scans.size() >= MaximumStoredScans)
            break;
        archive.scans << scanFromJson(value.toObject());
    }
    for (const QJsonValue &value : object.value(QStringLiteral("devices")).toArray())
        archive.devices << deviceFromJson(value.toObject());
    return archive;
}

ScanPolicy policyFor(const NexusSettings &settings)
{
    ScanPolicy policy;
    policy.allowPublicTargets = settings.allowPublicTargets;
    policy.authorizedNetworks = parseNetworks(settings.authorizedNetworks);
    return policy;
}

QString suggestedLocalNetwork(const QList<Network> &interfaceAddresses)
{
    for (const Network &entry : interfaceAddresses) {
        const QHostAddress address = normalized(entry.first);
        if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLoopback()
            || classify(address) != TargetScope::LocalNetwork || inNetwork(address, "169.254.0.0", 16))
            continue;
        const int prefix = qBound(24, entry.second, 32);
        const quint32 mask = ~quint32(0) << (32 - prefix);
        return QStringLiteral("%1/%2").arg(QHostAddress(address.toIPv4Address() & mask).toString()).arg(prefix);
    }
    return {};
}

}
