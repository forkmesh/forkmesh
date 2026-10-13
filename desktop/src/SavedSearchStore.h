#pragma once

#include <QMap>
#include <QString>
#include <QStringList>
#include <QVector>

// Searches the user kept from the search results page, with what the last run
// found. A saved search remembers the repository it ran against ("owner/name",
// empty when none was open) because the results page scopes most of its
// categories to the open repository: the same words searched elsewhere are a
// different search.
//
// New occurrences are detected by result identity, not by count. Every run
// hands over one short key per result (resultKey) and the store remembers the
// keys it has already seen, so a result that moves, or one that drops out and
// comes back, is not reported twice.
//
// The store is its own JSON file under AppDataLocation rather than QSettings:
// the seen-key lists grow with the result counts, and every QSettings write
// rewrites the whole ini on the GUI thread.
struct SavedSearch {
    QString id;
    QString query;
    QString scope; // "owner/name", or empty when no repository was open
    bool alerts = false;
    bool securityPertinent = false;
    QStringList excluded;
    qint64 createdMs = 0;
    qint64 lastRunMs = 0;    // 0 until the first completed run
    qint64 lastViewedMs = 0; // last run the user watched on the results page
    int runs = 0;
    int lastTotal = 0;
    int previousTotal = 0;
    int lastNew = 0;        // results the last run saw for the first time
    int newSinceViewed = 0; // first-seen results since the user last looked
    QMap<QString, int> kindCounts; // result kind -> matches in the last run
    QStringList seen;              // result keys already reported
    QString resultFingerprint;
    QString acknowledgedFingerprint;
};

struct SavedSearchRun {
    bool recorded = false; // false when the id is unknown
    bool baseline = false; // first run: everything is seen, nothing is new
    QStringList newKeys;
};

class SavedSearchStore
{
public:
    static constexpr int kMaxSeenKeys = 6000;

    explicit SavedSearchStore(const QString &path = defaultPath());

    static QString defaultPath();
    // Short stable key for one result. `identity` is whatever stays the same
    // for that result across runs (issue number, commit hash, path + text).
    static QString resultKey(const QString &kind, const QString &identity);
    static QString resultFingerprint(const QStringList &keys);
    static QStringList parseExcluded(const QString &text);
    static bool excludes(const QStringList &excluded, const QString &text);
    static QString hiddenTermsPath();
    static QStringList loadHiddenTerms(const QString &path);
    static bool saveHiddenTerms(const QString &path, const QStringList &terms);

    QString path() const { return m_path; }
    const QVector<SavedSearch> &searches() const { return m_searches; }
    const SavedSearch *find(const QString &id) const;
    // Queries compare case-insensitively after trimming, like the search.
    const SavedSearch *findQuery(const QString &query, const QString &scope) const;
    bool hasAlerts() const;
    bool hasSecurityPertinent() const;

    // Newest first. Saving a search that is already saved returns that one.
    SavedSearch add(const QString &query, const QString &scope, bool alerts,
                    qint64 nowMs);
    bool remove(const QString &id);
    bool setAlerts(const QString &id, bool alerts);
    bool setSecurityPertinent(const QString &id, bool pertinent);
    bool setExcluded(const QString &id, const QStringList &excluded);
    bool acknowledgeSecurityAlarm(const QString &id);
    SavedSearchRun recordRun(const QString &id, const QStringList &keys,
                             const QMap<QString, int> &kindCounts, qint64 nowMs,
                             bool viewed);

    bool load();
    bool save() const;

private:
    SavedSearch *findMutable(const QString &id);

    QString m_path;
    QVector<SavedSearch> m_searches;
};
