#include "IpWhois.h"

#include <QHostAddress>
#include <QPair>
#include <QRegularExpression>

namespace forkmesh {
namespace ipwhois {

namespace {

QString cleanValue(QString value)
{
    static const QRegularExpression unprintable(
        QStringLiteral("[\\p{Cc}\\p{Cf}\\s]+"));
    value.replace(unprintable, QStringLiteral(" "));
    value.replace(QChar(0x00B7), QLatin1Char('-'));
    value.replace(QLatin1Char(';'), QLatin1Char(','));
    value = value.trimmed();
    if (value.size() > 80)
        value = value.left(79) + QChar(0x2026);
    return value;
}

bool isRegistry(const QString &owner)
{
    static const char *const registries[] = {
        "RIPE Network Coordination Centre",
        "Asia Pacific Network Information Centre",
        "Latin American and Caribbean IP address Regional Registry",
        "African Network Information Center",
        "American Registry for Internet Numbers",
    };
    for (const char *name : registries) {
        if (owner.startsWith(QLatin1String(name), Qt::CaseInsensitive))
            return true;
    }
    return false;
}

QString asNumber(const QString &value)
{
    static const QRegularExpression number(QStringLiteral("(?i)\\bAS\\s*([0-9]{1,10})\\b|^([0-9]{1,10})$"));
    const QRegularExpressionMatch match = number.match(value);
    if (!match.hasMatch())
        return {};
    const QString digits =
        match.captured(1).isEmpty() ? match.captured(2) : match.captured(1);
    return QStringLiteral("AS") + digits;
}

} // namespace

bool Info::isEmpty() const
{
    return network.isEmpty() && range.isEmpty() && cidr.isEmpty() &&
           owner.isEmpty() && about.isEmpty() && country.isEmpty() &&
           asn.isEmpty() && abuse.isEmpty();
}

Info parse(const QString &text)
{
    Info info;
    const QList<QPair<QStringList, QString Info::*>> fields = {
        {{QStringLiteral("netname")}, &Info::network},
        {{QStringLiteral("netrange"), QStringLiteral("inetnum"),
          QStringLiteral("inet6num")},
         &Info::range},
        {{QStringLiteral("cidr"), QStringLiteral("route"), QStringLiteral("route6")},
         &Info::cidr},
        {{QStringLiteral("orgname"), QStringLiteral("org-name"),
          QStringLiteral("organization"), QStringLiteral("owner")},
         &Info::owner},
        {{QStringLiteral("descr")}, &Info::about},
        {{QStringLiteral("country")}, &Info::country},
        {{QStringLiteral("originas"), QStringLiteral("origin"),
          QStringLiteral("aut-num")},
         &Info::asn},
        {{QStringLiteral("orgabuseemail"), QStringLiteral("abuse-mailbox")},
         &Info::abuse},
        {{QStringLiteral("regdate"), QStringLiteral("created")}, &Info::registered},
        {{QStringLiteral("updated"), QStringLiteral("last-modified"),
          QStringLiteral("changed")},
         &Info::updated},
    };
    QString lastKey;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            lastKey.clear();
            continue;
        }
        if (line.startsWith(QLatin1Char('%')) || line.startsWith(QLatin1Char('#')))
            continue;
        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon <= 0)
            continue;
        const QString key = line.left(colon).trimmed().toLower();
        const QString value = cleanValue(line.mid(colon + 1));
        const bool continued = key == lastKey;
        lastKey = key;
        if (value.isEmpty() || (continued && key == QLatin1String("descr")))
            continue;
        for (const auto &field : fields) {
            if (!field.first.contains(key))
                continue;
            if (field.second == &Info::asn) {
                const QString number = asNumber(value);
                if (!number.isEmpty())
                    info.asn = number;
            } else if (field.second == &Info::country) {
                if (value.size() == 2)
                    info.country = value.toUpper();
            } else {
                info.*field.second = value;
            }
            break;
        }
    }
    return info;
}

bool worthLookingUp(const QString &address)
{
    const QHostAddress host(address);
    if (host.isNull() || host.isLoopback() || host.isMulticast() ||
        host.isLinkLocal())
        return false;
    static const QList<QPair<QHostAddress, int>> reserved = [] {
        QList<QPair<QHostAddress, int>> ranges;
        for (const char *cidr :
             {"0.0.0.0/8", "10.0.0.0/8", "100.64.0.0/10", "127.0.0.0/8",
              "169.254.0.0/16", "172.16.0.0/12", "192.0.0.0/24",
              "192.0.2.0/24", "192.168.0.0/16", "198.18.0.0/15",
              "198.51.100.0/24", "203.0.113.0/24", "224.0.0.0/3", "::/8",
              "64:ff9b::/96", "100::/64", "2001:db8::/32", "fc00::/7",
              "fe80::/10", "ff00::/8"})
            ranges.append(QHostAddress::parseSubnet(QString::fromLatin1(cidr)));
        return ranges;
    }();
    QHostAddress plain = host;
    bool mapped = false;
    const quint32 v4 = host.toIPv4Address(&mapped);
    if (mapped)
        plain = QHostAddress(v4);
    for (const auto &range : reserved) {
        if (plain.isInSubnet(range))
            return false;
    }
    return true;
}

QString cacheKey(const QString &address)
{
    const QHostAddress host(address);
    if (host.isNull())
        return address;
    bool mapped = false;
    const quint32 v4 = host.toIPv4Address(&mapped);
    if (mapped)
        return QHostAddress(v4 & 0xFFFFFF00u).toString() + QStringLiteral("/24");
    Q_IPV6ADDR bytes = host.toIPv6Address();
    for (int i = 6; i < 16; ++i)
        bytes[i] = 0;
    return QHostAddress(bytes).toString() + QStringLiteral("/48");
}

QString ownerLabel(const Info &info)
{
    QString name = info.owner;
    if (name.isEmpty() || isRegistry(name))
        name = info.about.isEmpty() ? info.network : info.about;
    if (name.isEmpty())
        name = info.owner;
    if (info.asn.isEmpty())
        return name;
    return name.isEmpty() ? info.asn : name + QStringLiteral(", ") + info.asn;
}

QString describe(const QString &address, const Info &info)
{
    QStringList parts;
    const auto add = [&parts](const char *label, const QString &value) {
        if (!value.isEmpty())
            parts << QString::fromLatin1(label) + QStringLiteral(": ") + value;
    };
    add("owner", info.owner);
    if (info.about != info.owner)
        add("about", info.about);
    add("network", info.network);
    add("range", info.range);
    add("cidr", info.cidr);
    add("country", info.country);
    add("AS", info.asn);
    add("abuse", info.abuse);
    add("registered", info.registered);
    add("updated", info.updated);
    if (parts.isEmpty())
        parts << QStringLiteral("no registration found");
    return QStringLiteral("whois %1 \u00b7 ").arg(address) +
           parts.join(QStringLiteral(" · "));
}

} // namespace ipwhois
} // namespace forkmesh
