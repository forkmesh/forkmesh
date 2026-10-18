#include "OpenWrtModel.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QtEndian>

#include <algorithm>
#include <climits>

namespace forkmesh::openwrt {
namespace {

const QSet<QString> &secretOptions()
{
    static const QSet<QString> names = {QStringLiteral("key"), QStringLiteral("key1"), QStringLiteral("key2"),
                                        QStringLiteral("key3"), QStringLiteral("key4"), QStringLiteral("password"),
                                        QStringLiteral("sae_password"), QStringLiteral("auth_secret"),
                                        QStringLiteral("acct_secret"), QStringLiteral("private_key"),
                                        QStringLiteral("preshared_key"), QStringLiteral("psk")};
    return names;
}

bool privateIpv4(quint32 address)
{
    const auto in = [address](quint32 network, int bits) {
        const quint32 mask = bits == 0 ? 0 : 0xffffffffu << (32 - bits);
        return (address & mask) == (network & mask);
    };
    return in(0x0a000000u, 8) || in(0xac100000u, 12) || in(0xc0a80000u, 16) || in(0xa9fe0000u, 16)
        || in(0x7f000000u, 8);
}

bool localAddress(const QHostAddress &address)
{
    if (address.isNull())
        return false;
    bool ok = false;
    const quint32 v4 = address.toIPv4Address(&ok);
    if (ok)
        return privateIpv4(v4);
    return address.isLoopback() || address.isLinkLocal() || address.isUniqueLocalUnicast();
}

QString changeValue(const QString &option, const QJsonValue &value)
{
    if (isSecretOption(option))
        return QStringLiteral("••••••");
    return optionText(value);
}

}

QByteArray callBody(int id, const QString &session, const QString &object, const QString &method,
                    const QJsonObject &args)
{
    const QJsonObject body{
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), id},
        {QStringLiteral("method"), QStringLiteral("call")},
        {QStringLiteral("params"), QJsonArray{session.isEmpty() ? kAnonymousSession : session, object, method, args}},
    };
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QString statusMessage(int status)
{
    switch (status) {
    case UbusOk: return QStringLiteral("Done.");
    case UbusInvalidCommand: return QStringLiteral("The router did not recognise the command.");
    case UbusInvalidArgument: return QStringLiteral("The router rejected a value as invalid.");
    case UbusMethodNotFound: return QStringLiteral("The router does not offer this function. It may need an extra package (LuCI provides most of them).");
    case UbusNotFound: return QStringLiteral("The router could not find what was asked for.");
    case UbusNoData: return QStringLiteral("The router had nothing to return.");
    case UbusPermissionDenied: return QStringLiteral("The router refused: the session expired or this account is not allowed to do that.");
    case UbusTimeout: return QStringLiteral("The router took too long to answer.");
    case UbusNotSupported: return QStringLiteral("The router does not support this.");
    case UbusUnknownError: return QStringLiteral("The router reported an unknown error.");
    case UbusConnectionFailed: return QStringLiteral("A service on the router is not running.");
    }
    return QStringLiteral("The router answered with error %1.").arg(status);
}

RpcResult parseReply(const QByteArray &body, int httpStatus)
{
    RpcResult result;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        if (httpStatus == 404)
            result.error = QStringLiteral("This device has no /ubus endpoint. Is it an OpenWrt router with uhttpd-mod-ubus and rpcd installed?");
        else if (httpStatus == 401 || httpStatus == 403) {
            result.error = statusMessage(UbusPermissionDenied);
            result.accessDenied = true;
        } else
            result.error = QStringLiteral("The router did not answer with ubus JSON (HTTP %1). Is this an OpenWrt router?").arg(httpStatus);
        return result;
    }
    const QJsonObject root = document.object();
    if (root.contains(QStringLiteral("error"))) {
        const QJsonObject error = root.value(QStringLiteral("error")).toObject();
        const int code = error.value(QStringLiteral("code")).toInt();
        result.status = code;
        switch (code) {
        case -32002:
            result.accessDenied = true;
            result.error = statusMessage(UbusPermissionDenied);
            break;
        case -32000:
            result.error = QStringLiteral("The router does not offer this function (object not found). It may need an extra package; LuCI provides most of them.");
            break;
        case -32601:
            result.error = statusMessage(UbusMethodNotFound);
            break;
        case -32602:
            result.error = statusMessage(UbusInvalidArgument);
            break;
        default:
            result.error = QStringLiteral("The router answered: %1").arg(error.value(QStringLiteral("message")).toString(QStringLiteral("unknown error")));
        }
        return result;
    }
    const QJsonArray values = root.value(QStringLiteral("result")).toArray();
    if (values.isEmpty() || !values.at(0).isDouble()) {
        result.error = QStringLiteral("The router's answer had no result.");
        return result;
    }
    result.status = values.at(0).toInt();
    result.data = values.size() > 1 ? values.at(1).toObject() : QJsonObject();
    result.ok = result.status == UbusOk;
    if (!result.ok) {
        result.accessDenied = result.status == UbusPermissionDenied;
        result.error = statusMessage(result.status);
    }
    return result;
}

bool isLocalHostName(const QString &host)
{
    const QString name = host.trimmed().toLower();
    if (name.isEmpty())
        return false;
    QHostAddress address;
    if (address.setAddress(name))
        return localAddress(address);
    if (name == QStringLiteral("localhost") || !name.contains(QLatin1Char('.')))
        return true;
    static const QStringList suffixes = {QStringLiteral(".lan"), QStringLiteral(".local"), QStringLiteral(".home"),
                                         QStringLiteral(".home.arpa"), QStringLiteral(".internal"),
                                         QStringLiteral(".localdomain"), QStringLiteral(".router")};
    return std::any_of(suffixes.begin(), suffixes.end(), [&name](const QString &suffix) { return name.endsWith(suffix); });
}

Address parseAddress(const QString &input)
{
    Address address;
    QString text = input.trimmed();
    if (text.isEmpty()) {
        address.error = QStringLiteral("Enter the router's address, for example 192.168.1.1.");
        return address;
    }
    if (!text.contains(QStringLiteral("://")))
        text.prepend(QStringLiteral("http://"));
    QUrl url(text, QUrl::StrictMode);
    const QString scheme = url.scheme().toLower();
    if (!url.isValid() || url.host().isEmpty() || (scheme != QStringLiteral("http") && scheme != QStringLiteral("https"))) {
        address.error = QStringLiteral("\"%1\" is not a router address. Use an IP address or a name such as openwrt.lan.").arg(input.trimmed());
        return address;
    }
    if (!url.userInfo().isEmpty()) {
        address.error = QStringLiteral("Leave the user name out of the address; type it in the User field.");
        return address;
    }
    QHostAddress literal;
    if (literal.setAddress(url.host()) && !localAddress(literal)) {
        address.error = QStringLiteral("%1 is not on your local network. Only routers on your own network can be managed, so the router password never crosses the internet.").arg(url.host());
        return address;
    }
    QUrl base;
    base.setScheme(scheme);
    base.setHost(url.host());
    if (url.port() > 0)
        base.setPort(url.port());
    address.base = base;
    address.encrypted = scheme == QStringLiteral("https");
    return address;
}

QString defaultGatewayFromProcRoute(const QString &text)
{
    QString best;
    int bestMetric = INT_MAX;
    const QStringList lines = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (int i = 1; i < lines.size(); ++i) {
        const QStringList fields = lines.at(i).split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
        if (fields.size() < 8 || fields.at(1) != QStringLiteral("00000000") || fields.at(7) != QStringLiteral("00000000"))
            continue;
        bool ok = false;
        const uint flags = fields.at(3).toUInt(&ok, 16);
        if (!ok || !(flags & 0x1) || !(flags & 0x2))
            continue;
        const quint32 raw = fields.at(2).toUInt(&ok, 16);
        if (!ok || raw == 0)
            continue;
        const int metric = fields.at(6).toInt();
        if (metric >= bestMetric)
            continue;
        bestMetric = metric;
        best = QHostAddress(qFromBigEndian<quint32>(&raw)).toString();
    }
    return best;
}

QString defaultGatewayFromRouteGet(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^\\s*gateway:\\s*(\\S+)\\s*$"), QRegularExpression::MultilineOption);
    const QRegularExpressionMatch match = pattern.match(text);
    if (!match.hasMatch())
        return {};
    QHostAddress address;
    return address.setAddress(match.captured(1)) ? address.toString() : QString();
}

QString UciSection::option(const QString &key, const QString &fallback) const
{
    return options.contains(key) ? optionText(options.value(key)) : fallback;
}

bool UciSection::flag(const QString &key, bool fallback) const
{
    if (!options.contains(key))
        return fallback;
    const QString value = optionText(options.value(key)).toLower();
    return value == QStringLiteral("1") || value == QStringLiteral("on") || value == QStringLiteral("true")
        || value == QStringLiteral("yes") || value == QStringLiteral("enabled");
}

QList<UciSection> uciSections(const QJsonObject &values, const QString &type)
{
    QList<UciSection> sections;
    for (auto it = values.begin(); it != values.end(); ++it) {
        const QJsonObject raw = it.value().toObject();
        UciSection section;
        section.name = raw.value(QStringLiteral(".name")).toString(it.key());
        section.type = raw.value(QStringLiteral(".type")).toString();
        section.anonymous = raw.value(QStringLiteral(".anonymous")).toBool();
        section.index = raw.value(QStringLiteral(".index")).toInt();
        if (!type.isEmpty() && section.type != type)
            continue;
        for (auto option = raw.begin(); option != raw.end(); ++option) {
            if (!option.key().startsWith(QLatin1Char('.')))
                section.options.insert(option.key(), option.value());
        }
        sections << section;
    }
    std::stable_sort(sections.begin(), sections.end(), [](const UciSection &left, const UciSection &right) {
        return left.index < right.index;
    });
    return sections;
}

QString optionText(const QJsonValue &value)
{
    if (value.isArray()) {
        QStringList parts;
        for (const QJsonValue &item : value.toArray())
            parts << optionText(item);
        return parts.join(QLatin1Char(' '));
    }
    if (value.isBool())
        return value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    if (value.isDouble())
        return QString::number(value.toDouble());
    return value.toString();
}

QJsonValue optionValue(const QString &text, bool list)
{
    if (!list)
        return text.trimmed();
    QJsonArray items;
    for (const QString &part : text.split(QRegularExpression(QStringLiteral("[\\s,]+")), Qt::SkipEmptyParts))
        items.append(part);
    return items;
}

bool isSecretOption(const QString &name)
{
    return secretOptions().contains(name.toLower());
}

QString sectionLabel(const UciSection &section)
{
    const QString name = section.option(QStringLiteral("name"));
    if (!section.anonymous)
        return section.name;
    if (!name.isEmpty())
        return QStringLiteral("%1 \"%2\"").arg(section.type, name);
    return QStringLiteral("%1 #%2").arg(section.type).arg(section.index);
}

bool validUciName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_]{1,64}$"));
    return pattern.match(name).hasMatch();
}

bool validUciOptionName(const QString &name)
{
    return validUciName(name);
}

int pendingCount(const QJsonObject &changes)
{
    int count = 0;
    for (auto it = changes.begin(); it != changes.end(); ++it)
        count += it.value().toArray().size();
    return count;
}

QStringList describeChanges(const QJsonObject &changes)
{
    QStringList lines;
    for (auto it = changes.begin(); it != changes.end(); ++it) {
        const QString config = it.key();
        for (const QJsonValue &entry : it.value().toArray()) {
            const QJsonArray change = entry.toArray();
            const QString op = change.at(0).toString();
            const QString section = change.at(1).toString();
            const QString option = change.size() > 2 ? change.at(2).toString() : QString();
            const QJsonValue value = change.size() > 3 ? change.at(3) : QJsonValue();
            const QString path = QStringLiteral("%1.%2").arg(config, section);
            if (op == QStringLiteral("set") && change.size() > 3)
                lines << QStringLiteral("%1.%2 = %3").arg(path, option, changeValue(option, value));
            else if (op == QStringLiteral("set") || op == QStringLiteral("add"))
                lines << QStringLiteral("%1: new %2 section").arg(path, option.isEmpty() ? QStringLiteral("unnamed") : option);
            else if (op == QStringLiteral("remove") && !option.isEmpty())
                lines << QStringLiteral("%1.%2 removed").arg(path, option);
            else if (op == QStringLiteral("remove"))
                lines << QStringLiteral("%1 deleted").arg(path);
            else if (op == QStringLiteral("list-add"))
                lines << QStringLiteral("%1.%2 += %3").arg(path, option, changeValue(option, value));
            else if (op == QStringLiteral("list-del"))
                lines << QStringLiteral("%1.%2 -= %3").arg(path, option, changeValue(option, value));
            else if (op == QStringLiteral("rename"))
                lines << QStringLiteral("%1 renamed to %2").arg(path, option);
            else if (op == QStringLiteral("order"))
                lines << QStringLiteral("%1 moved").arg(path);
            else
                lines << QStringLiteral("%1: %2").arg(path, op);
        }
    }
    return lines;
}

QList<Lease> parseLeases(const QString &text)
{
    QList<Lease> leases;
    for (const QString &line : text.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QStringList fields = line.split(QLatin1Char(' '), Qt::SkipEmptyParts);
        if (fields.size() < 4)
            continue;
        Lease lease;
        bool ok = false;
        lease.expires = fields.at(0).toLongLong(&ok);
        if (!ok || !validMac(fields.at(1)))
            continue;
        lease.mac = fields.at(1).toLower();
        lease.ip = fields.at(2);
        lease.hostname = fields.at(3) == QStringLiteral("*") ? QString() : fields.at(3);
        leases << lease;
    }
    return leases;
}

QString leaseExpiry(qint64 expires, qint64 nowSeconds)
{
    if (expires <= 0)
        return QStringLiteral("Never");
    const qint64 left = expires - nowSeconds;
    if (left <= 0)
        return QStringLiteral("Expired");
    return QStringLiteral("in %1").arg(formatUptime(left));
}

QList<Package> parseInstalledPackages(const QString &text)
{
    static const QRegularExpression apkLine(QStringLiteral("^(\\S+?)-(\\d[^\\s-]*(?:-r\\d+)?)(?:\\s|$)"));
    QList<Package> packages;
    Package block;
    const auto flush = [&packages, &block] {
        if (!block.name.isEmpty())
            packages << block;
        block = Package();
    };
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            flush();
            continue;
        }
        if (line.startsWith(QStringLiteral("Package: "))) {
            flush();
            block.name = line.mid(9).trimmed();
            continue;
        }
        if (line.startsWith(QStringLiteral("Version: "))) {
            block.version = line.mid(9).trimmed();
            continue;
        }
        if (!block.name.isEmpty() || line.contains(QStringLiteral(": ")))
            continue;
        const int dash = line.indexOf(QStringLiteral(" - "));
        if (dash > 0) {
            const QString rest = line.mid(dash + 3);
            const int more = rest.indexOf(QStringLiteral(" - "));
            packages << Package{line.left(dash), more >= 0 ? rest.left(more) : rest};
            continue;
        }
        const QRegularExpressionMatch match = apkLine.match(line);
        if (match.hasMatch())
            packages << Package{match.captured(1), match.captured(2)};
    }
    flush();
    std::sort(packages.begin(), packages.end(), [](const Package &left, const Package &right) {
        return left.name < right.name;
    });
    return packages;
}

QList<PackageTool> packageTools()
{
    return {{QStringLiteral("/usr/libexec/package-manager-call"), true},
            {QStringLiteral("/usr/libexec/opkg-call"), true},
            {QStringLiteral("/usr/bin/apk"), false},
            {QStringLiteral("/bin/opkg"), false}};
}

QStringList packageArguments(const PackageTool &tool, const QString &action, const QString &name)
{
    const bool apk = !tool.wrapper && tool.program.endsWith(QStringLiteral("/apk"));
    if (action == QStringLiteral("list")) {
        if (tool.wrapper)
            return {QStringLiteral("list-installed")};
        return apk ? QStringList{QStringLiteral("list"), QStringLiteral("--installed")} : QStringList{QStringLiteral("list-installed")};
    }
    if (action == QStringLiteral("update"))
        return {QStringLiteral("update")};
    if (action == QStringLiteral("install"))
        return {apk ? QStringLiteral("add") : QStringLiteral("install"), name};
    if (action == QStringLiteral("remove"))
        return {apk ? QStringLiteral("del") : QStringLiteral("remove"), name};
    return {};
}

bool validPackageName(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[a-z0-9][a-z0-9+._-]{0,99}$"));
    return pattern.match(name).hasMatch();
}

QList<Service> parseServices(const QJsonObject &list)
{
    QList<Service> services;
    for (auto it = list.begin(); it != list.end(); ++it) {
        const QJsonObject raw = it.value().toObject();
        Service service;
        service.name = it.key();
        service.enabled = raw.value(QStringLiteral("enabled")).toBool();
        service.running = raw.value(QStringLiteral("running")).toBool();
        service.start = raw.value(QStringLiteral("start")).toInt();
        services << service;
    }
    std::sort(services.begin(), services.end(), [](const Service &left, const Service &right) {
        return left.name < right.name;
    });
    return services;
}

QList<InterfaceStatus> parseInterfaces(const QJsonObject &dump)
{
    QList<InterfaceStatus> interfaces;
    for (const QJsonValue &value : dump.value(QStringLiteral("interface")).toArray()) {
        const QJsonObject raw = value.toObject();
        InterfaceStatus status;
        status.name = raw.value(QStringLiteral("interface")).toString();
        status.proto = raw.value(QStringLiteral("proto")).toString();
        status.device = raw.value(QStringLiteral("l3_device")).toString(raw.value(QStringLiteral("device")).toString());
        status.up = raw.value(QStringLiteral("up")).toBool();
        status.uptime = raw.value(QStringLiteral("uptime")).toInteger();
        for (const QJsonValue &address : raw.value(QStringLiteral("ipv4-address")).toArray()) {
            const QJsonObject entry = address.toObject();
            status.ipv4 << QStringLiteral("%1/%2").arg(entry.value(QStringLiteral("address")).toString()).arg(entry.value(QStringLiteral("mask")).toInt());
        }
        for (const QJsonValue &address : raw.value(QStringLiteral("ipv6-address")).toArray()) {
            const QJsonObject entry = address.toObject();
            status.ipv6 << QStringLiteral("%1/%2").arg(entry.value(QStringLiteral("address")).toString()).arg(entry.value(QStringLiteral("mask")).toInt());
        }
        for (const QJsonValue &server : raw.value(QStringLiteral("dns-server")).toArray())
            status.dns << server.toString();
        for (const QJsonValue &route : raw.value(QStringLiteral("route")).toArray()) {
            const QJsonObject entry = route.toObject();
            if (entry.value(QStringLiteral("target")).toString() == QStringLiteral("0.0.0.0") && entry.value(QStringLiteral("mask")).toInt() == 0) {
                status.gateway = entry.value(QStringLiteral("nexthop")).toString();
                break;
            }
        }
        if (!status.name.isEmpty())
            interfaces << status;
    }
    return interfaces;
}

QString formatUptime(qint64 seconds)
{
    if (seconds < 0)
        seconds = 0;
    const qint64 days = seconds / 86400;
    const qint64 hours = (seconds % 86400) / 3600;
    const qint64 minutes = (seconds % 3600) / 60;
    if (days > 0)
        return QStringLiteral("%1d %2h").arg(days).arg(hours);
    if (hours > 0)
        return QStringLiteral("%1h %2m").arg(hours).arg(minutes);
    if (minutes > 0)
        return QStringLiteral("%1m").arg(minutes);
    return QStringLiteral("%1s").arg(seconds);
}

QString formatBytes(qint64 bytes)
{
    static const QStringList units = {QStringLiteral("B"), QStringLiteral("KB"), QStringLiteral("MB"), QStringLiteral("GB"), QStringLiteral("TB")};
    double value = double(qMax<qint64>(0, bytes));
    int unit = 0;
    while (value >= 1024.0 && unit < units.size() - 1) {
        value /= 1024.0;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(qint64(value))
                     : QStringLiteral("%1 %2").arg(QString::number(value, 'f', value < 10 ? 1 : 0), units.at(unit));
}

QString formatLoad(const QJsonArray &load)
{
    QStringList parts;
    for (const QJsonValue &value : load)
        parts << QString::number(value.toDouble() / 65536.0, 'f', 2);
    return parts.join(QStringLiteral(" · "));
}

bool validIpv4(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)(\\.(25[0-5]|2[0-4]\\d|1\\d\\d|[1-9]?\\d)){3}$"));
    return pattern.match(text.trimmed()).hasMatch();
}

bool validNetmask(const QString &text)
{
    if (!validIpv4(text))
        return false;
    const quint32 mask = QHostAddress(text.trimmed()).toIPv4Address();
    const quint32 inverted = ~mask;
    return (inverted & (inverted + 1)) == 0;
}

bool validMac(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^[0-9A-Fa-f]{2}(:[0-9A-Fa-f]{2}){5}$"));
    return pattern.match(text.trimmed()).hasMatch();
}

bool validPort(const QString &text)
{
    static const QRegularExpression pattern(QStringLiteral("^(\\d{1,5})(?:[-:](\\d{1,5}))?$"));
    const QRegularExpressionMatch match = pattern.match(text.trimmed());
    if (!match.hasMatch())
        return false;
    const int first = match.captured(1).toInt();
    const int last = match.captured(2).isEmpty() ? first : match.captured(2).toInt();
    return first >= 1 && last <= 65535 && first <= last;
}

QString ssidProblem(const QString &ssid)
{
    const int length = ssid.toUtf8().size();
    if (length == 0)
        return QStringLiteral("Give the Wi-Fi network a name.");
    if (length > 32)
        return QStringLiteral("A Wi-Fi name can be at most 32 bytes long.");
    return {};
}

QString wifiKeyProblem(const QString &encryption, const QString &key)
{
    if (encryption == QStringLiteral("none") || encryption == QStringLiteral("owe") || encryption.isEmpty())
        return {};
    static const QRegularExpression hex(QStringLiteral("^[0-9A-Fa-f]{64}$"));
    if (encryption.startsWith(QStringLiteral("psk")) && hex.match(key).hasMatch())
        return {};
    if (key.size() < 8)
        return QStringLiteral("The Wi-Fi password needs at least 8 characters.");
    if (encryption.startsWith(QStringLiteral("psk")) || encryption == QStringLiteral("sae-mixed")) {
        if (key.size() > 63)
            return QStringLiteral("A WPA2 password can be at most 63 characters.");
        for (const QChar c : key) {
            if (c.unicode() < 0x20 || c.unicode() > 0x7e)
                return QStringLiteral("A WPA2 password can only use printable ASCII characters.");
        }
    }
    return {};
}

QString hostnameProblem(const QString &hostname)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?$"));
    if (!pattern.match(hostname).hasMatch())
        return QStringLiteral("A hostname uses letters, digits and hyphens, at most 63 characters, and cannot start or end with a hyphen.");
    return {};
}

QString passwordProblem(const QString &password, const QString &confirm)
{
    if (password.size() < 8)
        return QStringLiteral("Use at least 8 characters for the router password.");
    if (password != confirm)
        return QStringLiteral("The two passwords do not match.");
    return {};
}

QList<Choice> encryptionChoices()
{
    return {{QStringLiteral("sae-mixed"), QStringLiteral("WPA2/WPA3 (recommended)")},
            {QStringLiteral("sae"), QStringLiteral("WPA3 only")},
            {QStringLiteral("psk2"), QStringLiteral("WPA2")},
            {QStringLiteral("psk-mixed"), QStringLiteral("WPA/WPA2 (older devices)")},
            {QStringLiteral("owe"), QStringLiteral("Open, encrypted (OWE)")},
            {QStringLiteral("none"), QStringLiteral("Open, no password")}};
}

QList<Choice> protocolChoices()
{
    return {{QStringLiteral("static"), QStringLiteral("Static address")},
            {QStringLiteral("dhcp"), QStringLiteral("DHCP client")},
            {QStringLiteral("dhcpv6"), QStringLiteral("DHCPv6 client")},
            {QStringLiteral("pppoe"), QStringLiteral("PPPoE")},
            {QStringLiteral("none"), QStringLiteral("Unmanaged")}};
}

QList<Choice> forwardProtocolChoices()
{
    return {{QStringLiteral("tcp udp"), QStringLiteral("TCP and UDP")},
            {QStringLiteral("tcp"), QStringLiteral("TCP")},
            {QStringLiteral("udp"), QStringLiteral("UDP")}};
}

QString choiceLabel(const QList<Choice> &choices, const QString &value)
{
    for (const Choice &choice : choices) {
        if (choice.value == value)
            return choice.label;
    }
    return value;
}

QString certificateFingerprint(const QByteArray &der)
{
    const QByteArray hex = QCryptographicHash::hash(der, QCryptographicHash::Sha256).toHex().toUpper();
    QStringList pairs;
    for (int i = 0; i < hex.size(); i += 2)
        pairs << QString::fromLatin1(hex.mid(i, 2));
    return pairs.join(QLatin1Char(':'));
}

QString firmwareProblem(const QJsonObject &validation)
{
    if (validation.value(QStringLiteral("valid")).toBool())
        return {};
    QStringList failed;
    const QJsonObject tests = validation.value(QStringLiteral("tests")).toObject();
    for (auto it = tests.begin(); it != tests.end(); ++it) {
        if (!it.value().toBool())
            failed << it.key();
    }
    if (failed.contains(QStringLiteral("fwtool_device_match")))
        return QStringLiteral("This image is not for this router model. Flashing it could make the router unusable.");
    if (!failed.isEmpty())
        return QStringLiteral("The router rejected the image (%1 failed).").arg(failed.join(QStringLiteral(", ")));
    return QStringLiteral("The router says this is not a valid firmware image for it.");
}

QString confirmationPhrase(const QString &hostname)
{
    const QString name = hostname.trimmed();
    return name.isEmpty() ? QStringLiteral("OpenWrt") : name;
}

}
