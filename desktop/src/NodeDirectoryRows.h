#pragma once

#include "CoreNodeLogs.h"
#include "RunnerLogin.h"

#include <QHash>
#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <algorithm>

namespace forkmesh::nodedir {

inline constexpr qint64 kRunnerFreshMs = 2 * 60 * 1000;

enum class RowKind { Node, Relay, Runner };

inline QString rowKindKey(RowKind kind)
{
    switch (kind) {
    case RowKind::Relay:
        return QStringLiteral("relay");
    case RowKind::Runner:
        return QStringLiteral("runner");
    case RowKind::Node:
        break;
    }
    return QStringLiteral("node");
}

struct RunnerRow {
    QString name;
    QString feedName;
    QString label;
    QString address;
    QString scope;
    QString status;
    QString registeredBy;
    qint64 lastSeenAtMs = 0;
    int activeLeases = -1;
    bool registered = false;
    bool inFleet = false;
};

inline QString runnerKey(const QString &name)
{
    const QString key = name.trimmed().toLower();
    static const QRegularExpression shortName(
        QStringLiteral("^(?:fmnext-)?(r|b)([0-9]{1,4})$"));
    static const QRegularExpression label(
        QStringLiteral("^fmnext-(run|runner|burst)-([0-9]{1,4})$"));
    QRegularExpressionMatch match = shortName.match(key);
    if (!match.hasMatch())
        match = label.match(key);
    if (!match.hasMatch())
        return key;
    const QString prefix = match.captured(1).startsWith(QLatin1Char('b'))
                               ? QStringLiteral("b")
                               : QStringLiteral("r");
    return QStringLiteral("fmnext-") + prefix +
           QString::number(match.captured(2).toInt());
}

inline QString runnerFeedName(const QString &name)
{
    const QString key = runnerKey(name);
    static const QRegularExpression fleet(
        QStringLiteral("^fmnext-([rb][0-9]{1,4})$"));
    const QRegularExpressionMatch match = fleet.match(key);
    return match.hasMatch() ? match.captured(1) : QString();
}

inline QList<RunnerRow>
mergeRunnerRows(const QList<runners::RunnerInfo> &registered,
                const QList<corelogs::CoreNode> &fleet)
{
    QList<RunnerRow> rows;
    QHash<QString, int> byKey;
    for (const corelogs::CoreNode &node : fleet) {
        if (node.role != QLatin1String("runner"))
            continue;
        const QString key = runnerKey(node.label.isEmpty()
                                          ? node.name
                                          : node.label);
        if (key.isEmpty() || byKey.contains(key))
            continue;
        RunnerRow row;
        row.name = key;
        row.feedName = node.name;
        row.label = node.label;
        row.address = node.address;
        row.scope = QStringLiteral("pool");
        row.inFleet = true;
        byKey.insert(key, rows.size());
        rows.append(row);
    }
    QList<runners::RunnerInfo> sorted = registered;
    std::sort(sorted.begin(), sorted.end(),
              [](const runners::RunnerInfo &a, const runners::RunnerInfo &b) {
                  return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
              });
    for (const runners::RunnerInfo &info : std::as_const(sorted)) {
        const QString key = runnerKey(info.name);
        if (key.isEmpty())
            continue;
        const auto found = byKey.constFind(key);
        if (found == byKey.constEnd()) {
            byKey.insert(key, rows.size());
            rows.append(RunnerRow());
        }
        RunnerRow &row = rows[byKey.value(key)];
        row.name = info.name.trimmed();
        if (row.feedName.isEmpty())
            row.feedName = runnerFeedName(info.name);
        if (!info.scope.trimmed().isEmpty())
            row.scope = info.scope.trimmed().toLower();
        row.status = info.status.trimmed().toLower();
        row.registeredBy = info.registeredBy.trimmed();
        row.lastSeenAtMs = info.lastSeenAtMs;
        row.activeLeases = info.activeLeases;
        row.registered = true;
    }
    return rows;
}

inline bool runnerOnline(const RunnerRow &row, qint64 nowMs)
{
    if (!row.registered || row.status != QLatin1String("active"))
        return false;
    return row.lastSeenAtMs > 0 && nowMs - row.lastSeenAtMs <= kRunnerFreshMs;
}

inline QString runnerStatusText(const RunnerRow &row, qint64 nowMs)
{
    if (!row.registered)
        return QStringLiteral("Not registered");
    if (row.status != QLatin1String("active"))
        return row.status.isEmpty() ? QStringLiteral("Unknown")
                                    : row.status.left(1).toUpper() +
                                          row.status.mid(1);
    return runnerOnline(row, nowMs) ? QStringLiteral("Online")
                                    : QStringLiteral("Offline");
}

inline QString runnerActivityText(const RunnerRow &row)
{
    if (row.activeLeases < 0)
        return QString();
    if (row.activeLeases == 0)
        return QStringLiteral("idle");
    return row.activeLeases == 1
               ? QStringLiteral("1 job lease")
               : QStringLiteral("%1 job leases").arg(row.activeLeases);
}

struct RelayProbe {
    QString state;
    int latencyMs = -1;
    QString version;
    QString failure;
    qint64 updatedMs = 0;
};

inline QString relayVersionText(const QString &version, const QString &rev)
{
    const QString revText =
        rev.trimmed().isEmpty() ? QStringLiteral("dev") : rev.trimmed().left(12);
    if (version.trimmed().isEmpty())
        return revText;
    return version.trimmed() + QString::fromUtf8(" \xC2\xB7 ") + revText;
}

inline QString relaySummaryText(const QList<RelayProbe> &probes,
                                const QString &updatedAt)
{
    if (probes.isEmpty())
        return QStringLiteral("no relays configured");
    int online = 0;
    int checking = 0;
    for (const RelayProbe &probe : probes) {
        if (probe.state == QLatin1String("Online"))
            ++online;
        else if (probe.state.isEmpty() ||
                 probe.state.startsWith(QLatin1String("Checking")))
            ++checking;
    }
    if (checking > 0)
        return QString::fromUtf8("probing %1 relay(s)\xE2\x80\xA6")
            .arg(probes.size());
    QString text = QStringLiteral("%1 of %2 relay(s) online")
                       .arg(online)
                       .arg(probes.size());
    if (!updatedAt.isEmpty())
        text += QString::fromUtf8(" \xC2\xB7 updated ") + updatedAt;
    return text;
}

} // namespace forkmesh::nodedir
