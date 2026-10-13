#include "IpCountry.h"

#include <QHostAddress>

#include <algorithm>

namespace forkmesh {
namespace ipcountry {

namespace {

bool countryCode(const QByteArray &field, char out[2])
{
    if (field.size() != 2)
        return false;
    for (int i = 0; i < 2; ++i) {
        const char c = field.at(i);
        if (c < 'A' || c > 'Z')
            return false;
        out[i] = c;
    }
    return !(out[0] == 'Z' && out[1] == 'Z');
}

quint64 upper64(const Q_IPV6ADDR &address)
{
    quint64 value = 0;
    for (int i = 0; i < 8; ++i)
        value = (value << 8) | quint8(address[i]);
    return value;
}

bool parseLine(Table &table, const QByteArray &line)
{
    const QList<QByteArray> fields = line.trimmed().split(',');
    if (fields.size() != 3)
        return false;
    char code[2];
    if (!countryCode(fields.at(2).trimmed(), code))
        return false;
    const QHostAddress first(QString::fromLatin1(fields.at(0).trimmed()));
    const QHostAddress last(QString::fromLatin1(fields.at(1).trimmed()));
    if (first.isNull() || last.isNull() || first.protocol() != last.protocol())
        return false;
    if (first.protocol() == QAbstractSocket::IPv4Protocol) {
        Range4 range;
        range.first = first.toIPv4Address();
        range.last = last.toIPv4Address();
        if (range.last < range.first)
            return false;
        range.code[0] = code[0];
        range.code[1] = code[1];
        table.v4.append(range);
        return true;
    }
    Range6 range;
    range.first = upper64(first.toIPv6Address());
    range.last = upper64(last.toIPv6Address());
    if (range.last < range.first)
        return false;
    range.code[0] = code[0];
    range.code[1] = code[1];
    table.v6.append(range);
    return true;
}

template <typename Range, typename Key>
QString find(const QVector<Range> &ranges, Key key)
{
    auto it = std::upper_bound(
        ranges.cbegin(), ranges.cend(), key,
        [](Key value, const Range &range) { return value < range.first; });
    if (it == ranges.cbegin())
        return QString();
    --it;
    if (key > it->last)
        return QString();
    return QString::fromLatin1(it->code, 2);
}

} // namespace

int appendCsv(Table &table, const QByteArray &csv)
{
    int added = 0;
    qsizetype start = 0;
    while (start < csv.size()) {
        qsizetype end = csv.indexOf('\n', start);
        if (end < 0)
            end = csv.size();
        if (end > start && parseLine(table, csv.mid(start, end - start)))
            ++added;
        start = end + 1;
    }
    return added;
}

void finish(Table &table)
{
    std::sort(table.v4.begin(), table.v4.end(),
              [](const Range4 &a, const Range4 &b) { return a.first < b.first; });
    std::sort(table.v6.begin(), table.v6.end(),
              [](const Range6 &a, const Range6 &b) { return a.first < b.first; });
    table.v4.squeeze();
    table.v6.squeeze();
}

QString lookup(const Table &table, const QString &address)
{
    QHostAddress host(address.trimmed());
    if (host.isNull())
        return QString();
    bool ok = false;
    const quint32 v4 = host.toIPv4Address(&ok);
    if (ok)
        return find(table.v4, v4);
    if (host.protocol() != QAbstractSocket::IPv6Protocol)
        return QString();
    return find(table.v6, upper64(host.toIPv6Address()));
}

QString csvUrl(bool ipv6)
{
    return QStringLiteral(
               "https://cdn.jsdelivr.net/npm/@ip-location-db/dbip-country/%1")
        .arg(csvFileName(ipv6));
}

QString csvFileName(bool ipv6)
{
    return ipv6 ? QStringLiteral("dbip-country-ipv6.csv")
                : QStringLiteral("dbip-country-ipv4.csv");
}

bool csvLooksValid(const QByteArray &csv)
{
    const qsizetype end = csv.indexOf('\n');
    if (end <= 0)
        return false;
    Table probe;
    return parseLine(probe, csv.left(end));
}

} // namespace ipcountry
} // namespace forkmesh
