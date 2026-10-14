#include "D1DatabaseAdmin.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>

#include <algorithm>

namespace forkmesh {

namespace {

QByteArray encodedPair(const char *key, const QString &value)
{
    return QByteArray(key) + '=' + QUrl::toPercentEncoding(value);
}

} // namespace

QList<D1DatabaseColumn> parseD1DatabaseColumns(const QJsonArray &columns)
{
    QList<D1DatabaseColumn> out;
    for (const QJsonValue &value : columns) {
        const QJsonObject object = value.toObject();
        D1DatabaseColumn column;
        column.name = object.value(QStringLiteral("name")).toString();
        if (column.name.isEmpty())
            continue;
        column.type = object.value(QStringLiteral("type")).toString();
        column.primaryKey = object.value(QStringLiteral("primaryKey")).toBool();
        column.notNull = object.value(QStringLiteral("notNull")).toBool();
        column.hasDefault = object.value(QStringLiteral("hasDefault")).toBool();
        out.append(column);
    }
    return out;
}

QList<D1DatabaseTable> parseD1DatabaseInventory(const QJsonObject &inventory)
{
    QList<D1DatabaseTable> out;
    for (const QJsonValue &value :
         inventory.value(QStringLiteral("tables")).toArray()) {
        const QJsonObject object = value.toObject();
        D1DatabaseTable table;
        table.name = object.value(QStringLiteral("name")).toString();
        if (table.name.isEmpty())
            continue;
        table.rows = static_cast<qint64>(
            object.value(QStringLiteral("rows")).toDouble());
        table.restricted = object.value(QStringLiteral("restricted")).toBool();
        table.writable = object.value(QStringLiteral("writable")).toBool();
        table.deletable =
            object.value(QStringLiteral("deletable")).toBool(table.writable);
        table.writeBlockedReason =
            object.value(QStringLiteral("writeBlockedReason")).toString();
        table.rowid = object.value(QStringLiteral("rowid")).toBool(true);
        table.storage = object.value(QStringLiteral("storage")).toString();
        table.plaintextData =
            object.value(QStringLiteral("plaintextData")).toBool();
        table.columns = parseD1DatabaseColumns(
            object.value(QStringLiteral("columns")).toArray());
        for (const QJsonValue &gap :
             object.value(QStringLiteral("gaps")).toArray())
            table.gaps.append(gap.toString());
        table.auditCategory =
            object.value(QStringLiteral("auditCategory")).toString();
        table.auditNote = object.value(QStringLiteral("auditNote")).toString();
        out.append(table);
    }
    return out;
}

QByteArray d1DatabaseReadCanonical(const QString &node, const QString &ts,
                                   const D1DatabaseReadRequest &request)
{
    const QStringList parts = {
        QStringLiteral("forkmesh-admin-database-read-v1"),
        node,
        ts,
        request.op,
        request.table,
        request.cursor,
        request.limit,
        request.filterColumn,
        request.filterMode,
        request.filterValue,
        request.order,
    };
    return parts.join(QLatin1Char('\n')).toUtf8();
}

QByteArray d1DatabaseReadQueryString(const D1DatabaseReadRequest &request,
                                     const QString &node, const QString &ts,
                                     const QString &signature, bool refresh)
{
    QList<QByteArray> pairs = {
        encodedPair("op", request.op),
        encodedPair("table", request.table),
        encodedPair("cursor", request.cursor),
        encodedPair("limit", request.limit),
        encodedPair("filterColumn", request.filterColumn),
        encodedPair("filterMode", request.filterMode),
        encodedPair("filterValue", request.filterValue),
        encodedPair("order", request.order),
        encodedPair("node", node),
        encodedPair("ts", ts),
        encodedPair("sig", signature),
    };
    if (refresh)
        pairs.append(QByteArrayLiteral("refresh=1"));
    return pairs.join('&');
}

QByteArray d1DatabaseWriteCanonical(const QString &node, const QString &ts,
                                    const QString &requestId,
                                    const QByteArray &payload)
{
    const QByteArray digest =
        QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex();
    return (QStringLiteral("forkmesh-admin-database-write-v1\n") + node +
            QLatin1Char('\n') + ts + QLatin1Char('\n') + requestId +
            QLatin1Char('\n'))
               .toUtf8() +
           digest;
}

QString newD1DatabaseRequestId()
{
    QByteArray bytes(18, Qt::Uninitialized);
    for (char &byte : bytes)
        byte = static_cast<char>(QRandomGenerator::system()->bounded(256));
    return QString::fromLatin1(bytes.toBase64(QByteArray::Base64UrlEncoding |
                                              QByteArray::OmitTrailingEquals));
}

QJsonValue d1DatabaseEditorValue(const QString &text,
                                 const QString &declaredType)
{
    // SQLite's affinity rules, in its own order: INT wins, then text types,
    // then REAL/FLOA/DOUB.
    const QString type = declaredType.toUpper();
    const QString trimmed = text.trimmed();
    if (type.contains(QLatin1String("INT"))) {
        bool ok = false;
        const qlonglong integer = trimmed.toLongLong(&ok);
        if (ok)
            return QJsonValue(static_cast<qint64>(integer));
        return QJsonValue(text);
    }
    if (type.contains(QLatin1String("CHAR")) ||
        type.contains(QLatin1String("CLOB")) ||
        type.contains(QLatin1String("TEXT")))
        return QJsonValue(text);
    if (type.contains(QLatin1String("REAL")) ||
        type.contains(QLatin1String("FLOA")) ||
        type.contains(QLatin1String("DOUB"))) {
        bool ok = false;
        const double number = trimmed.toDouble(&ok);
        if (ok)
            return QJsonValue(number);
    }
    return QJsonValue(text);
}

QString d1DatabaseCellText(const QJsonValue &value)
{
    switch (value.type()) {
    case QJsonValue::Null:
    case QJsonValue::Undefined:
        return QStringLiteral("NULL");
    case QJsonValue::Bool:
        return value.toBool() ? QStringLiteral("1") : QStringLiteral("0");
    case QJsonValue::Double: {
        const double number = value.toDouble();
        if (number == static_cast<double>(static_cast<qint64>(number)) &&
            qAbs(number) < 9007199254740992.0)
            return QString::number(static_cast<qint64>(number));
        return QString::number(number, 'g', 15);
    }
    case QJsonValue::String:
        return value.toString();
    case QJsonValue::Object: {
        const QJsonObject object = value.toObject();
        if (object.size() == 1 && object.contains(QStringLiteral("$bytes")))
            return QStringLiteral("<blob %1 bytes>")
                .arg(static_cast<qint64>(
                    object.value(QStringLiteral("$bytes")).toDouble()));
        return QString::fromUtf8(
            QJsonDocument(object).toJson(QJsonDocument::Compact));
    }
    case QJsonValue::Array:
        return QString::fromUtf8(
            QJsonDocument(value.toArray()).toJson(QJsonDocument::Compact));
    }
    return QString();
}

QString d1DatabaseAuditCategoryLabel(const QString &category)
{
    if (category == QLatin1String("drop"))
        return QStringLiteral("Drop");
    if (category == QLatin1String("drop-after-readers"))
        return QStringLiteral("Drop after readers are removed");
    if (category == QLatin1String("drop-after-export"))
        return QStringLiteral("Export, then drop");
    if (category == QLatin1String("custody"))
        return QStringLiteral("Legacy custody");
    if (category == QLatin1String("runtime-only"))
        return QStringLiteral("Needs a catch-up migration");
    if (category == QLatin1String("encryption-gap"))
        return QStringLiteral("Encryption gap");
    if (category == QLatin1String("hot"))
        return QStringLiteral("Hot path");
    if (category == QLatin1String("plugin"))
        return QStringLiteral("Plugin-owned");
    if (category == QLatin1String("keep"))
        return QStringLiteral("Keep");
    return category;
}

bool d1DatabaseAuditIsDropCandidate(const QString &category)
{
    return category.startsWith(QLatin1String("drop"));
}

QStringList d1DatabaseQueryTables(const QString &sql)
{
    static const QRegularExpression pattern(
        QStringLiteral(
            R"((?:\bfrom|\bjoin|\binto|\bupdate)\s+["`\[]?([A-Za-z_][A-Za-z0-9_]*))"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList tables;
    QSet<QString> seen;
    auto it = pattern.globalMatch(sql);
    while (it.hasNext()) {
        const QString name = it.next().captured(1).toLower();
        // "FROM (SELECT ...": the subquery's own FROM names the table.
        if (name == QLatin1String("select") || seen.contains(name))
            continue;
        seen.insert(name);
        tables.append(name);
    }
    return tables;
}

QJsonArray d1DatabaseTableQueryInsights(const QJsonObject &d1UsageReport,
                                        const QString &table)
{
    const QString wanted = table.toLower();
    QList<QJsonObject> matches;
    for (const QJsonValue &value :
         d1UsageReport.value(QStringLiteral("queryInsights")).toArray()) {
        const QJsonObject row = value.toObject();
        if (d1DatabaseQueryTables(row.value(QStringLiteral("query")).toString())
                .contains(wanted))
            matches.append(row);
    }
    std::stable_sort(matches.begin(), matches.end(),
                     [](const QJsonObject &a, const QJsonObject &b) {
                         return a.value(QStringLiteral("totalRowsRead"))
                                    .toDouble() >
                                b.value(QStringLiteral("totalRowsRead"))
                                    .toDouble();
                     });
    QJsonArray out;
    for (const QJsonObject &row : matches)
        out.append(row);
    return out;
}

QString d1DatabaseDropMigration(const QStringList &tables)
{
    QStringList sorted = tables;
    sorted.removeDuplicates();
    std::sort(sorted.begin(), sorted.end());
    QString text = QStringLiteral(
        "-- Drop tables the D1 audit (#682) found dead.\n"
        "--\n"
        "-- Ship this ALONE, in the deploy after the one that removed every\n"
        "-- reader and each table's CREATE TABLE/INDEX from app/src/schema.py;\n"
        "-- otherwise a cold isolate's ensure_schema re-creates the table.\n"
        "-- Export any rows you keep first (Database tab > Export), pick the\n"
        "-- migration number after `git log main -- app/migrations`, and add\n"
        "-- the matching down migration.\n\n");
    for (const QString &table : sorted) {
        QString quoted = table;
        quoted.replace(QLatin1Char('"'), QStringLiteral("\"\""));
        text += QStringLiteral("DROP TABLE IF EXISTS \"%1\";\n").arg(quoted);
    }
    return text;
}

} // namespace forkmesh
