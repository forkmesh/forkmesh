#include "SavedSearchStore.h"
#include "AppPaths.h"
#include "BackgroundFileWriter.h"

#include <algorithm>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QUuid>

namespace {

QString normalizedQuery(const QString &query)
{
    return query.trimmed().toCaseFolded();
}

} // namespace

SavedSearchStore::SavedSearchStore(const QString &path) : m_path(path)
{
    load();
}

QString SavedSearchStore::defaultPath()
{
    const QString dir =
        forkmesh::paths::dataRoot();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/saved-searches.json");
}

QString SavedSearchStore::resultKey(const QString &kind, const QString &identity)
{
    const QByteArray digest = QCryptographicHash::hash(
        (kind + QChar(0x1f) + identity).toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex().left(12));
}

QString SavedSearchStore::resultFingerprint(const QStringList &keys)
{
    QStringList unique;
    QSet<QString> present;
    for (const QString &key : keys) {
        if (key.isEmpty() || present.contains(key))
            continue;
        present.insert(key);
        unique.append(key);
    }
    std::sort(unique.begin(), unique.end());
    const QByteArray digest = QCryptographicHash::hash(
        unique.join(QChar(0x1f)).toUtf8(), QCryptographicHash::Sha1);
    return QString::fromLatin1(digest.toHex().left(16));
}

QStringList SavedSearchStore::parseExcluded(const QString &text)
{
    QStringList terms;
    QSet<QString> present;
    static const QRegularExpression separators(QStringLiteral("[,;\\n]"));
    for (const QString &part : text.split(separators)) {
        const QString term = part.trimmed();
        if (term.isEmpty() || present.contains(term.toCaseFolded()))
            continue;
        present.insert(term.toCaseFolded());
        terms.append(term);
    }
    return terms;
}

bool SavedSearchStore::excludes(const QStringList &excluded, const QString &text)
{
    for (const QString &term : excluded)
        if (!term.isEmpty() && text.contains(term, Qt::CaseInsensitive))
            return true;
    return false;
}

QString SavedSearchStore::hiddenTermsPath()
{
    const QString dir = forkmesh::paths::dataRoot();
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/search-exclusions.json");
}

QStringList SavedSearchStore::loadHiddenTerms(const QString &path)
{
    if (path.isEmpty())
        return {};
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QStringList terms;
    const QJsonArray rows =
        QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("hidden")).toArray();
    for (const QJsonValue &value : rows)
        terms.append(value.toString());
    return parseExcluded(terms.join(QLatin1Char('\n')));
}

bool SavedSearchStore::saveHiddenTerms(const QString &path, const QStringList &terms)
{
    if (path.isEmpty())
        return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    file.write(QJsonDocument(QJsonObject{
                                 {QStringLiteral("version"), 1},
                                 {QStringLiteral("hidden"),
                                  QJsonArray::fromStringList(
                                      parseExcluded(terms.join(QLatin1Char('\n'))))}})
                   .toJson(QJsonDocument::Indented));
    return file.commit();
}

const SavedSearch *SavedSearchStore::find(const QString &id) const
{
    for (const SavedSearch &search : m_searches)
        if (search.id == id)
            return &search;
    return nullptr;
}

SavedSearch *SavedSearchStore::findMutable(const QString &id)
{
    for (SavedSearch &search : m_searches)
        if (search.id == id)
            return &search;
    return nullptr;
}

const SavedSearch *SavedSearchStore::findQuery(const QString &query,
                                               const QString &scope) const
{
    const QString wanted = normalizedQuery(query);
    for (const SavedSearch &search : m_searches)
        if (search.scope == scope && normalizedQuery(search.query) == wanted)
            return &search;
    return nullptr;
}

bool SavedSearchStore::hasAlerts() const
{
    for (const SavedSearch &search : m_searches)
        if (search.alerts)
            return true;
    return false;
}

bool SavedSearchStore::hasSecurityPertinent() const
{
    for (const SavedSearch &search : m_searches)
        if (search.securityPertinent)
            return true;
    return false;
}

SavedSearch SavedSearchStore::add(const QString &query, const QString &scope,
                                  bool alerts, qint64 nowMs)
{
    if (const SavedSearch *existing = findQuery(query, scope))
        return *existing;
    SavedSearch search;
    search.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    search.query = query.trimmed();
    search.scope = scope;
    search.alerts = alerts;
    search.createdMs = nowMs;
    m_searches.prepend(search);
    return search;
}

bool SavedSearchStore::remove(const QString &id)
{
    for (int i = 0; i < m_searches.size(); ++i) {
        if (m_searches.at(i).id == id) {
            m_searches.removeAt(i);
            return true;
        }
    }
    return false;
}

bool SavedSearchStore::setAlerts(const QString &id, bool alerts)
{
    SavedSearch *search = findMutable(id);
    if (!search || search->alerts == alerts)
        return false;
    search->alerts = alerts;
    return true;
}

bool SavedSearchStore::setSecurityPertinent(const QString &id, bool pertinent)
{
    SavedSearch *search = findMutable(id);
    if (!search || search->securityPertinent == pertinent)
        return false;
    search->securityPertinent = pertinent;
    return true;
}

bool SavedSearchStore::setExcluded(const QString &id, const QStringList &excluded)
{
    SavedSearch *search = findMutable(id);
    const QStringList terms = parseExcluded(excluded.join(QLatin1Char('\n')));
    if (!search || search->excluded == terms)
        return false;
    search->excluded = terms;
    return true;
}

bool SavedSearchStore::acknowledgeSecurityAlarm(const QString &id)
{
    SavedSearch *search = findMutable(id);
    if (!search || search->resultFingerprint.isEmpty() ||
        search->acknowledgedFingerprint == search->resultFingerprint)
        return false;
    search->acknowledgedFingerprint = search->resultFingerprint;
    return true;
}

SavedSearchRun SavedSearchStore::recordRun(const QString &id,
                                           const QStringList &keys,
                                           const QMap<QString, int> &kindCounts,
                                           qint64 nowMs, bool viewed)
{
    SavedSearchRun run;
    SavedSearch *search = findMutable(id);
    if (!search)
        return run;
    run.recorded = true;
    run.baseline = search->runs == 0;

    const QSet<QString> seen(search->seen.cbegin(), search->seen.cend());
    QStringList current;
    QSet<QString> currentSet;
    for (const QString &key : keys) {
        if (key.isEmpty() || currentSet.contains(key))
            continue;
        currentSet.insert(key);
        current.append(key);
        if (!run.baseline && !seen.contains(key))
            run.newKeys.append(key);
    }
    // What this run found leads, then older sightings, so a trimmed list
    // forgets the results that have been gone longest.
    for (const QString &key : std::as_const(search->seen))
        if (!currentSet.contains(key))
            current.append(key);
    const QString fingerprint = resultFingerprint(keys);
    const bool acknowledged =
        !search->acknowledgedFingerprint.isEmpty() &&
        search->acknowledgedFingerprint == search->resultFingerprint;
    if (acknowledged && !run.baseline && run.newKeys.isEmpty())
        search->acknowledgedFingerprint = fingerprint;
    search->resultFingerprint = fingerprint;
    if (current.size() > kMaxSeenKeys)
        current = current.mid(0, kMaxSeenKeys);
    search->seen = current;

    int total = 0;
    for (int count : kindCounts)
        total += count;
    search->previousTotal = search->runs == 0 ? total : search->lastTotal;
    search->lastTotal = total;
    search->kindCounts = kindCounts;
    search->lastRunMs = nowMs;
    search->lastNew = run.newKeys.size();
    ++search->runs;
    if (viewed) {
        search->lastViewedMs = nowMs;
        search->newSinceViewed = 0;
    } else {
        search->newSinceViewed += run.newKeys.size();
    }
    return run;
}

bool SavedSearchStore::load()
{
    m_searches.clear();
    if (m_path.isEmpty())
        return false;
    QFile file(m_path);
    if (!file.open(QIODevice::ReadOnly))
        return false;
    const QJsonArray rows =
        QJsonDocument::fromJson(file.readAll()).object().value(QStringLiteral("searches")).toArray();
    for (const QJsonValue &value : rows) {
        const QJsonObject row = value.toObject();
        SavedSearch search;
        search.id = row.value(QStringLiteral("id")).toString();
        search.query = row.value(QStringLiteral("query")).toString();
        if (search.id.isEmpty() || search.query.trimmed().isEmpty())
            continue;
        search.scope = row.value(QStringLiteral("scope")).toString();
        search.alerts = row.value(QStringLiteral("alerts")).toBool();
        search.securityPertinent =
            row.value(QStringLiteral("securityPertinent")).toBool();
        for (const QJsonValue &term : row.value(QStringLiteral("excluded")).toArray())
            search.excluded.append(term.toString());
        search.excluded = parseExcluded(search.excluded.join(QLatin1Char('\n')));
        search.createdMs = qint64(row.value(QStringLiteral("createdMs")).toDouble());
        search.lastRunMs = qint64(row.value(QStringLiteral("lastRunMs")).toDouble());
        search.lastViewedMs =
            qint64(row.value(QStringLiteral("lastViewedMs")).toDouble());
        search.runs = row.value(QStringLiteral("runs")).toInt();
        search.lastTotal = row.value(QStringLiteral("lastTotal")).toInt();
        search.previousTotal = row.value(QStringLiteral("previousTotal")).toInt();
        search.lastNew = row.value(QStringLiteral("lastNew")).toInt();
        search.newSinceViewed = row.value(QStringLiteral("newSinceViewed")).toInt();
        const QJsonObject counts = row.value(QStringLiteral("kindCounts")).toObject();
        for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
            search.kindCounts.insert(it.key(), it.value().toInt());
        for (const QJsonValue &key : row.value(QStringLiteral("seen")).toArray())
            search.seen.append(key.toString());
        search.resultFingerprint =
            row.value(QStringLiteral("resultFingerprint")).toString();
        if (search.resultFingerprint.isEmpty() && search.runs > 0)
            search.resultFingerprint = QStringLiteral("run-%1-%2")
                                           .arg(search.lastRunMs)
                                           .arg(search.lastTotal);
        search.acknowledgedFingerprint =
            row.value(QStringLiteral("acknowledgedFingerprint")).toString();
        m_searches.append(search);
    }
    return true;
}

QByteArray SavedSearchStore::serialized() const
{
    QJsonArray rows;
    for (const SavedSearch &search : m_searches) {
        QJsonObject counts;
        for (auto it = search.kindCounts.constBegin();
             it != search.kindCounts.constEnd(); ++it)
            counts.insert(it.key(), it.value());
        rows.append(QJsonObject{
            {QStringLiteral("id"), search.id},
            {QStringLiteral("query"), search.query},
            {QStringLiteral("scope"), search.scope},
            {QStringLiteral("alerts"), search.alerts},
            {QStringLiteral("securityPertinent"), search.securityPertinent},
            {QStringLiteral("excluded"), QJsonArray::fromStringList(search.excluded)},
            {QStringLiteral("createdMs"), double(search.createdMs)},
            {QStringLiteral("lastRunMs"), double(search.lastRunMs)},
            {QStringLiteral("lastViewedMs"), double(search.lastViewedMs)},
            {QStringLiteral("runs"), search.runs},
            {QStringLiteral("lastTotal"), search.lastTotal},
            {QStringLiteral("previousTotal"), search.previousTotal},
            {QStringLiteral("lastNew"), search.lastNew},
            {QStringLiteral("newSinceViewed"), search.newSinceViewed},
            {QStringLiteral("kindCounts"), counts},
            {QStringLiteral("seen"), QJsonArray::fromStringList(search.seen)},
            {QStringLiteral("resultFingerprint"), search.resultFingerprint},
            {QStringLiteral("acknowledgedFingerprint"),
             search.acknowledgedFingerprint},
        });
    }
    return QJsonDocument(QJsonObject{{QStringLiteral("version"), 1},
                                     {QStringLiteral("searches"), rows}})
        .toJson(QJsonDocument::Compact);
}

bool SavedSearchStore::save() const
{
    if (m_path.isEmpty())
        return false;
    return forkmesh::diskwrite::writeNow(m_path, serialized());
}

void SavedSearchStore::saveLater() const
{
    forkmesh::diskwrite::writeLater(m_path, serialized());
}
