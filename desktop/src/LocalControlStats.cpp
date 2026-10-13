#include "LocalControlStats.h"

#include <QDate>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>
#include <QTime>
#include <QTimeZone>

#include <algorithm>

namespace forkmesh::localcontrol::stats {

namespace {

QString flatText(const QString &text, int max)
{
    QString out;
    bool space = false;
    for (const QChar c : text) {
        if (c.isSpace() || c.category() == QChar::Other_Control ||
            c.category() == QChar::Other_Format) {
            space = !out.isEmpty();
            continue;
        }
        if (space)
            out.append(QLatin1Char(' '));
        space = false;
        out.append(c);
        if (out.size() >= max)
            break;
    }
    if (out.size() > max)
        out.truncate(max);
    if (!out.isEmpty() && out.back().isHighSurrogate())
        out.chop(1);
    return out.trimmed();
}

}

QString familyPrefix()
{
    return QStringLiteral("stats.");
}

QString severityForBadge(const QString &badge)
{
    const QString name = badge.trimmed().toUpper();
    if (name == QLatin1String("ERROR") || name == QLatin1String("CRASH") ||
        name == QLatin1String("FATAL"))
        return QStringLiteral("error");
    if (name == QLatin1String("STALL") || name == QLatin1String("BGBLOCK") ||
        name == QLatin1String("WARN") || name == QLatin1String("WARNING"))
        return QStringLiteral("warning");
    return QStringLiteral("info");
}

int clampDays(const QJsonValue &value)
{
    const int days = value.isDouble() ? int(value.toDouble()) : kMaxTimelineDays;
    return qBound(1, days, kMaxTimelineDays);
}

qint64 localDayStartMs(qint64 ms)
{
    const QDate day = QDateTime::fromMSecsSinceEpoch(ms).date();
    return QDateTime(day, QTime(0, 0)).toMSecsSinceEpoch();
}

QJsonObject timeline(const QMap<qint64, QHash<QString, int>> &buckets, qint64 nowMs,
                     int days)
{
    days = qBound(1, days, kMaxTimelineDays);
    const QDate today = QDateTime::fromMSecsSinceEpoch(nowMs).date();
    struct Day {
        qint64 start = 0;
        int errors = 0;
        int warnings = 0;
        int info = 0;
    };
    QList<Day> rows;
    rows.reserve(days);
    for (int i = days - 1; i >= 0; --i)
        rows.append(Day{QDateTime(today.addDays(-i), QTime(0, 0)).toMSecsSinceEpoch()});
    const qint64 windowStart = rows.first().start;
    const qint64 windowEnd = QDateTime(today.addDays(1), QTime(0, 0)).toMSecsSinceEpoch();
    for (auto bucket = buckets.lowerBound(windowStart); bucket != buckets.cend(); ++bucket) {
        if (bucket.key() >= windowEnd)
            break;
        const qint64 start = localDayStartMs(bucket.key());
        auto row = std::find_if(rows.begin(), rows.end(),
                                [start](const Day &d) { return d.start == start; });
        if (row == rows.end())
            continue;
        for (auto category = bucket->cbegin(); category != bucket->cend(); ++category) {
            const int count = qMax(0, category.value());
            const QString severity = severityForBadge(category.key());
            if (severity == QLatin1String("error"))
                row->errors += count;
            else if (severity == QLatin1String("warning"))
                row->warnings += count;
            else
                row->info += count;
        }
    }
    QJsonArray out;
    for (const Day &day : std::as_const(rows))
        out.append(QJsonObject{{QStringLiteral("dayTs"), double(day.start)},
                               {QStringLiteral("errors"), day.errors},
                               {QStringLiteral("warnings"), day.warnings},
                               {QStringLiteral("info"), day.info}});
    return QJsonObject{{QStringLiteral("days"), out}};
}

bool parseRepoSlug(const QString &slug, QString *owner, QString *name)
{
    static const QRegularExpression pattern(
        QStringLiteral("^([A-Za-z0-9_.-]{1,100})/([A-Za-z0-9_.-]{1,100})$"));
    const QRegularExpressionMatch match = pattern.match(slug);
    if (!match.hasMatch())
        return false;
    const QString o = match.captured(1);
    const QString n = match.captured(2);
    if (o == QLatin1String(".") || o == QLatin1String("..") || n == QLatin1String(".") ||
        n == QLatin1String(".."))
        return false;
    if (owner)
        *owner = o;
    if (name)
        *name = n;
    return true;
}

QString repositoryFileLabel()
{
    return QStringLiteral("(repository)");
}

QString relativeFindingPath(const QString &path, const QString &root)
{
    QString clean = path.trimmed();
    if (clean.isEmpty())
        return repositoryFileLabel();
    clean.replace(QLatin1Char('\\'), QLatin1Char('/'));
    if (QDir::isAbsolutePath(clean) || clean.startsWith(QLatin1Char('/')) ||
        clean.contains(QLatin1Char(':'))) {
        const QString base = QDir::cleanPath(root);
        const QString full = QDir::cleanPath(clean);
        if (!root.isEmpty() && full.startsWith(base + QLatin1Char('/')))
            clean = full.mid(base.size() + 1);
        else
            clean = QFileInfo(full).fileName();
    }
    clean = QDir::cleanPath(clean);
    if (clean.startsWith(QLatin1String("../")) || clean == QLatin1String(".."))
        clean = QFileInfo(clean).fileName();
    if (clean.isEmpty() || clean == QLatin1String("."))
        return repositoryFileLabel();
    return clean.left(kMaxPathChars);
}

int findingPenalty(int severity)
{
    switch (severity) {
    case 1:
        return 2;
    case 2:
        return 8;
    case 3:
        return 20;
    case 4:
        return 35;
    default:
        return 0;
    }
}

int signalPenalty(int severity)
{
    switch (severity) {
    case 1:
        return 1;
    case 2:
        return 4;
    case 3:
        return 12;
    case 4:
        return 25;
    default:
        return 0;
    }
}

QString gradeFor(int score)
{
    if (score >= 90)
        return QStringLiteral("A");
    if (score >= 80)
        return QStringLiteral("B");
    if (score >= 70)
        return QStringLiteral("C");
    if (score >= 60)
        return QStringLiteral("D");
    return QStringLiteral("F");
}

QString severityLabel(int severity)
{
    switch (severity) {
    case 1:
        return QStringLiteral("Info");
    case 2:
        return QStringLiteral("Warning");
    case 3:
        return QStringLiteral("High");
    case 4:
        return QStringLiteral("Critical");
    default:
        return QStringLiteral("Pass");
    }
}

QJsonObject qualityReport(const QList<QualityFinding> &findings,
                          const QList<int> &signalSeverities, qint64 computedAtMs)
{
    struct File {
        QString path;
        int penalty = 0;
        int worst = 0;
        QList<QPair<int, QString>> notes;
    };
    QHash<QString, File> files;
    int total = 0;
    for (const int severity : signalSeverities)
        total += signalPenalty(severity);
    for (const QualityFinding &finding : findings) {
        if (finding.severity <= 0)
            continue;
        total += signalPenalty(finding.severity);
        const QString path = finding.path.isEmpty() ? repositoryFileLabel()
                                                    : finding.path.left(kMaxPathChars);
        File &file = files[path];
        file.path = path;
        file.penalty += findingPenalty(finding.severity);
        file.worst = qMax(file.worst, finding.severity);
        file.notes.append({finding.severity,
                           flatText(severityLabel(finding.severity) + QStringLiteral(": ") +
                                        finding.title,
                                    kMaxFindingChars)});
    }
    QList<File> ordered = files.values();
    std::sort(ordered.begin(), ordered.end(), [](const File &a, const File &b) {
        if (a.penalty != b.penalty)
            return a.penalty > b.penalty;
        return a.path < b.path;
    });
    QJsonArray rows;
    for (File &file : ordered) {
        if (rows.size() >= kMaxQualityFiles)
            break;
        std::stable_sort(file.notes.begin(), file.notes.end(),
                         [](const QPair<int, QString> &a, const QPair<int, QString> &b) {
                             return a.first > b.first;
                         });
        QJsonArray notes;
        for (const auto &note : std::as_const(file.notes)) {
            if (notes.size() >= kMaxFindingsPerFile)
                break;
            notes.append(note.second);
        }
        rows.append(QJsonObject{{QStringLiteral("path"), file.path},
                                {QStringLiteral("score"), qMax(0, 100 - file.penalty)},
                                {QStringLiteral("findings"), notes}});
    }
    const int score = qBound(0, 100 - total, 100);
    return QJsonObject{{QStringLiteral("score"), score},
                       {QStringLiteral("grade"), gradeFor(score)},
                       {QStringLiteral("computedAt"), double(computedAtMs)},
                       {QStringLiteral("findingCount"), int(findings.size())},
                       {QStringLiteral("files"), rows}};
}

QString byteText(qint64 bytes)
{
    const double value = double(qMax<qint64>(0, bytes));
    if (value >= 1024.0 * 1024.0 * 1024.0)
        return QStringLiteral("%1 GB").arg(value / (1024.0 * 1024.0 * 1024.0), 0, 'f', 1);
    if (value >= 1024.0 * 1024.0)
        return QStringLiteral("%1 MB").arg(value / (1024.0 * 1024.0), 0, 'f', 1);
    if (value >= 1024.0)
        return QStringLiteral("%1 KB").arg(value / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 B").arg(qint64(value));
}

QString ratchetNote(const RepoFacts &facts)
{
    if (!facts.ratchetEnabled)
        return {};
    if (facts.ceilingBytes <= 0)
        return QStringLiteral("Commits may not grow the repository.");
    const QString since = facts.ceilingDay.isEmpty()
                              ? QString()
                              : QStringLiteral(" (set %1)").arg(facts.ceilingDay.left(10));
    return QStringLiteral("Commits may not grow the repository past %1%2.")
        .arg(byteText(facts.ceilingBytes), since);
}

QJsonObject repoStats(const RepoFacts &facts)
{
    QJsonObject ratchet{{QStringLiteral("enabled"), facts.ratchetEnabled},
                        {QStringLiteral("note"), ratchetNote(facts)}};
    if (facts.ratchetEnabled) {
        ratchet.insert(QStringLiteral("ceilingBytes"), double(qMax<qint64>(0, facts.ceilingBytes)));
        ratchet.insert(QStringLiteral("ceilingDay"), facts.ceilingDay.left(10));
    }
    return QJsonObject{{QStringLiteral("commits"), double(qMax<qint64>(0, facts.commits))},
                       {QStringLiteral("files"), double(qMax<qint64>(0, facts.files))},
                       {QStringLiteral("lines"), double(qMax<qint64>(0, facts.lines))},
                       {QStringLiteral("bytes"), double(qMax<qint64>(0, facts.bytes))},
                       {QStringLiteral("day"), facts.day.left(10)},
                       {QStringLiteral("ratchet"), ratchet}};
}

QString ratchetSummary(const QString &repo, bool enabled)
{
    return enabled ? QStringLiteral("Turn on Ratchet Mode for %1 on the desktop: commits may "
                                    "not grow the repository past the size it is now.")
                         .arg(repo)
                   : QStringLiteral("Turn off Ratchet Mode for %1 on the desktop: commits are "
                                    "no longer checked against a size ceiling.")
                         .arg(repo);
}

QString ratchetAction(bool enabled)
{
    return enabled ? QStringLiteral("ratchet.on") : QStringLiteral("ratchet.off");
}

RatchetConfirmations::Issued RatchetConfirmations::issue(const QString &holder,
                                                         const QString &repo, bool enabled,
                                                         qint64 nowMs)
{
    for (auto it = m_intents.begin(); it != m_intents.end();) {
        if (it->expiresAtMs <= nowMs)
            it = m_intents.erase(it);
        else
            ++it;
    }
    while (m_intents.size() >= hosts::ConfirmationStore::kMaxOutstanding) {
        auto oldest = std::min_element(m_intents.begin(), m_intents.end(),
                                       [](const Intent &a, const Intent &b) {
                                           return a.expiresAtMs < b.expiresAtMs;
                                       });
        m_intents.erase(oldest);
    }
    Issued issued;
    issued.token = m_store.issue(holder, ratchetAction(enabled), repo, nowMs);
    issued.expiresAtMs = nowMs + hosts::ConfirmationStore::kLifetimeMs;
    m_intents.insert(issued.token, Intent{repo, enabled, issued.expiresAtMs});
    return issued;
}

hosts::ConfirmationStore::Result RatchetConfirmations::consume(const QString &token,
                                                               const QString &holder,
                                                               qint64 nowMs, QString *repo,
                                                               bool *enabled)
{
    const Intent intent = m_intents.take(token);
    if (token.isEmpty() || intent.repo.isEmpty()) {
        m_store.consume(token, holder, QString(), QString(), nowMs);
        return hosts::ConfirmationStore::Result::Missing;
    }
    const auto result =
        m_store.consume(token, holder, ratchetAction(intent.enabled), intent.repo, nowMs);
    if (result == hosts::ConfirmationStore::Result::Ok) {
        if (repo)
            *repo = intent.repo;
        if (enabled)
            *enabled = intent.enabled;
    }
    return result;
}

bool validSentryLevel(const QString &level)
{
    static const QStringList levels{QStringLiteral("fatal"), QStringLiteral("error"),
                                    QStringLiteral("warning"), QStringLiteral("info"),
                                    QStringLiteral("debug")};
    return levels.contains(level);
}

QJsonArray sentryIssues(const QJsonArray &raw, int max)
{
    static const QRegularExpression idPattern(QStringLiteral("^[A-Za-z0-9_.:-]{1,64}$"));
    QJsonArray out;
    for (const QJsonValue &value : raw) {
        if (out.size() >= qBound(0, max, kMaxSentryIssues))
            break;
        const QJsonObject row = value.toObject();
        const QString id = row.value(QStringLiteral("id")).toString();
        if (!idPattern.match(id).hasMatch())
            continue;
        QString level = row.value(QStringLiteral("level")).toString().trimmed().toLower();
        if (!validSentryLevel(level))
            level = QStringLiteral("error");
        const QString title =
            flatText(row.value(QStringLiteral("title")).toString(), kMaxSentryTitleChars);
        out.append(QJsonObject{
            {QStringLiteral("id"), id},
            {QStringLiteral("title"), title.isEmpty() ? id : title},
            {QStringLiteral("project"),
             flatText(row.value(QStringLiteral("project")).toString(), kMaxSentryProjectChars)},
            {QStringLiteral("level"), level},
            {QStringLiteral("count"), qMax(0.0, row.value(QStringLiteral("count")).toDouble())},
            {QStringLiteral("lastSeen"),
             qMax(0.0, row.value(QStringLiteral("lastSeen")).toDouble())},
        });
    }
    return out;
}

QString codemagicTokenVariable()
{
    return QStringLiteral("CODEMAGIC_API_TOKEN");
}

qint64 nextUtcMonthStartMs(qint64 nowMs)
{
    const QDate today = QDateTime::fromMSecsSinceEpoch(nowMs, QTimeZone::utc()).date();
    const QDate first = QDate(today.year(), today.month(), 1).addMonths(1);
    return QDateTime(first, QTime(0, 0), QTimeZone::utc()).toMSecsSinceEpoch();
}

namespace {

double freeSeconds(const QJsonObject &buildTime)
{
    double total = 0;
    for (auto it = buildTime.begin(); it != buildTime.end(); ++it)
        if (it.key().endsWith(QLatin1String("_free")))
            total += qMax(0.0, it.value().toDouble());
    return total;
}

double paidSeconds(const QJsonObject &buildTime)
{
    double total = 0;
    for (auto it = buildTime.begin(); it != buildTime.end(); ++it)
        if (!it.key().endsWith(QLatin1String("_free")))
            total += qMax(0.0, it.value().toDouble());
    return total;
}

}

QJsonObject codemagicUsage(const QJsonObject &userReply, qint64 nowMs)
{
    const QJsonObject user = userReply.contains(QStringLiteral("user"))
                                 ? userReply.value(QStringLiteral("user")).toObject()
                                 : userReply;
    const QJsonObject buildTimes = user.value(QStringLiteral("buildTimes")).toObject();
    const QJsonObject usage =
        user.value(QStringLiteral("billing")).toObject().value(QStringLiteral("usage")).toObject();
    const QJsonObject current =
        usage.value(QStringLiteral("currentPeriod")).toObject().value(QStringLiteral("buildTime")).toObject();
    const QJsonObject previous =
        usage.value(QStringLiteral("previousPeriod")).toObject().value(QStringLiteral("buildTime")).toObject();
    double limit = buildTimes.value(QStringLiteral("monthlyFreeBuildTimeLimit")).toDouble();
    if (limit <= 0)
        limit = usage.value(QStringLiteral("freeLimit")).toObject().value(QStringLiteral("buildTime")).toDouble();
    if (limit <= 0)
        return {};
    const QJsonValue currentFree =
        buildTimes.value(QStringLiteral("currentPeriod")).toObject().value(QStringLiteral("free"));
    const QJsonValue previousFree =
        buildTimes.value(QStringLiteral("previousPeriod")).toObject().value(QStringLiteral("free"));
    const double used = qMax(0.0, currentFree.isDouble() ? currentFree.toDouble() : freeSeconds(current));
    const double before =
        qMax(0.0, previousFree.isDouble() ? previousFree.toDouble() : freeSeconds(previous));
    return {
        {QStringLiteral("limitSeconds"), limit},
        {QStringLiteral("usedSeconds"), used},
        {QStringLiteral("remainingSeconds"), qMax(0.0, limit - used)},
        {QStringLiteral("previousSeconds"), before},
        {QStringLiteral("paidSeconds"), paidSeconds(current)},
        {QStringLiteral("resetsAt"), double(nextUtcMonthStartMs(nowMs))},
        {QStringLiteral("fetchedAt"), double(nowMs)},
    };
}

QString codemagicError(int httpStatus)
{
    if (httpStatus == 401 || httpStatus == 403)
        return QStringLiteral("codemagic_unauthorized");
    if (httpStatus >= 200 && httpStatus < 300)
        return QStringLiteral("bad_answer");
    return QStringLiteral("codemagic_unreachable");
}

} // namespace forkmesh::localcontrol::stats
