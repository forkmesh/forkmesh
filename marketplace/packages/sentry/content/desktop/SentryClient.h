#pragma once

#include <QByteArray>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;
class QTimer;

namespace forkmesh::sentry {

// The Market package this runtime ships in, and its bundled entry point.
inline const QString kPackageId = QStringLiteral("sentry");
inline const QString kDesktopEntrypoint =
    QStringLiteral("content/desktop/SentryDesktopExtension.cpp");

// Every line this package writes to the log opens with this prefix. The log
// callback carries kLogBadge separately, so an issue titled "TypeError: ..."
// lights the package's lane rather than the host's own ERROR lane.
inline const QString kLogBadge = QStringLiteral("SENTRY");
inline const QString kLogPrefix = QStringLiteral("Sentry: ");

// Everything the package keeps on this device lives under the Market
// registry's data prefix for the package, so removing the package wipes the
// token along with the connection details and the cached snapshot.
inline const QString kSettingsPrefix =
    QStringLiteral("market/packages/sentry/data");
inline const QString kAuthTokenSetting =
    QStringLiteral("market/packages/sentry/data/authToken");
inline const QString kBaseUrlSetting =
    QStringLiteral("market/packages/sentry/data/baseUrl");
inline const QString kOrganizationSetting =
    QStringLiteral("market/packages/sentry/data/organization");
inline const QString kProjectSetting =
    QStringLiteral("market/packages/sentry/data/project");
inline const QString kStatsPeriodSetting =
    QStringLiteral("market/packages/sentry/data/statsPeriod");

inline const QString kDefaultBaseUrl = QStringLiteral("https://sentry.io");
inline const QString kDefaultStatsPeriod = QStringLiteral("24h");
// One bounded read a minute is the whole network footprint of the package.
inline constexpr int kPollIntervalMs = 60'000;
inline constexpr int kMaxIssues = 100;
// A poll that finds more changes than this logs the first ones and one
// trailing "... and N more" line, so a noisy deploy cannot flood the log.
inline constexpr int kMaxChangeLines = 12;
inline constexpr int kMaxTitleChars = 140;
inline constexpr qint64 kMaxReplyBytes = 2 * 1024 * 1024;

// One unresolved issue as Sentry groups it. Everything here is untrusted
// network input reduced to bounded fields; the desktop renders only what
// parseIssues() accepted.
struct Issue {
    QString id;
    QString shortId;      // "FORKMESH-12"
    QString title;
    QString culprit;
    QString level;        // fatal | error | warning | info | debug
    QString status;
    QString type;         // error | default | csp | ...
    QString projectSlug;
    QString projectName;
    QString metadataType;
    QString metadataValue;
    QUrl permalink;
    qint64 count = 0;      // events, as Sentry counted them for the request
    qint64 recentCount = 0; // sum of the stats series for the stats period
    bool recentKnown = false;
    int userCount = 0;
    qint64 firstSeenMs = 0;
    qint64 lastSeenMs = 0;
    bool unhandled = false;
};

// Where to read from and as whom. `project` is optional: empty reads the
// whole organization, a slug narrows the read to that project.
struct Connection {
    QUrl baseUrl;
    QString organization;
    QString project;
    QString statsPeriod;
    QString token;

    bool isValid() const;
};

// What a page of issues adds up to, in the figures an error tracker is read
// for. Users are summed per issue, the way Sentry counts them, so one person
// hitting three issues counts three times.
struct Aggregate {
    int issueCount = 0;
    qint64 eventCount = 0;
    qint64 userCount = 0;
    int unhandledCount = 0;
    qint64 newestLastSeenMs = 0;
    QMap<QString, int> issuesByProject;
    QMap<QString, qint64> eventsByProject;
    QMap<QString, int> issuesByLevel;
    QMap<QString, qint64> eventsByLevel;
};

// What changed between two polls: an issue that was not there before, or one
// whose event count grew. Issues that disappeared (resolved, ignored) are not
// activity worth a log line.
struct Change {
    enum Kind { NewIssue, MoreEvents };
    Kind kind = NewIssue;
    Issue issue;
    qint64 previousCount = 0;
};

// A token without surrounding whitespace or a "Bearer " prefix, or an empty
// string for anything that cannot be an auth token.
QString normalizeAuthToken(const QString &token);
// A Sentry organization or project slug, or an empty string.
QString normalizeSlug(const QString &slug);
// The Sentry host to read from: https only (plain http only to this
// machine, for tests), no credentials, query or fragment, and no trailing
// "/api/0". Empty text is the hosted sentry.io; invalid text is an empty URL.
QUrl normalizeBaseUrl(const QString &text);
// One of statsPeriods(), defaulting to 24h.
QString normalizeStatsPeriod(const QString &period);
QStringList statsPeriods();
QString statsPeriodLabel(const QString &period); // "last 24 hours"

QUrl organizationUrlFor(const Connection &connection);
QUrl issuesUrlFor(const Connection &connection);
// The organization's issue list in the browser, and where an auth token is
// created.
QUrl browserUrlFor(const Connection &connection);
QUrl authTokenPageFor(const Connection &connection);

// Parse an issue-list response body. Returns false, with `error` set, for
// anything that is not a JSON array of issues. Accepted issues are sorted by
// event count, newest activity first among equals, and bounded to kMaxIssues.
bool parseIssues(const QByteArray &body, QList<Issue> *issues,
                 QString *error = nullptr);
// The organization's display name from a GET /api/0/organizations/<slug>/
// body, or an empty string.
QString parseOrganizationName(const QByteArray &body);
// Sentry's own explanation of a refusal ({"detail": "..."}), bounded.
QString parseErrorDetail(const QByteArray &body);

Aggregate aggregate(const QList<Issue> &issues);
QList<Change> diffIssues(const QList<Issue> &previous,
                         const QList<Issue> &current);

// The log lines. Every one opens with kLogPrefix.
QString summaryLine(const QString &organizationLabel,
                    const Aggregate &aggregate, const QString &statsPeriod);
QString changeLine(const Change &change);
QString failureLine(const QUrl &baseUrl, int status, const QByteArray &body,
                    const QString &networkError);

// Reader-facing helpers shared by the log lines and the Sentry tab.
QString boundedText(const QString &text, int maxChars);
QString relativeTimeText(qint64 ms, qint64 nowMs);
QString levelLabel(const QString &level);
QString issueLabel(const Issue &issue); // shortId, else id

} // namespace forkmesh::sentry

// The Sentry package runtime: one bounded poll a minute of the connected
// organization's unresolved issues through the host's network manager, with
// the differences between polls reported as log activity. It never writes to
// Sentry and never sends the token anywhere but the configured host.
class SentryClient final : public QObject
{
    Q_OBJECT
public:
    explicit SentryClient(QNetworkAccessManager *network,
                          QObject *parent = nullptr);

    void setConnection(const forkmesh::sentry::Connection &connection);
    forkmesh::sentry::Connection connection() const { return m_connection; }
    bool isConfigured() const { return m_connection.isValid(); }
    bool isActive() const { return m_active; }
    bool isConnected() const { return m_connected; }
    // A read is on the wire; refresh() waits for it rather than doubling it.
    bool isReading() const { return m_requestInFlight; }
    bool hasError() const { return !m_lastError.isEmpty(); }
    QString lastError() const { return m_lastError; }
    QString status() const { return m_status; }
    // The organization's display name once it has been read, else its slug.
    QString organizationName() const;
    QString organizationLabel() const; // "Name (slug)"
    qint64 lastSyncMs() const { return m_lastSyncMs; }
    bool hasSynced() const { return m_lastSyncMs > 0; }
    // Milliseconds until the next scheduled read, or -1 when none is
    // scheduled (stopped, unconfigured, or waiting for a fixed token).
    int nextPollInMs() const;
    const QList<forkmesh::sentry::Issue> &issues() const { return m_issues; }
    forkmesh::sentry::Aggregate aggregate() const;

    void start();
    void stop();
    // Read again now. Also the way out of an authorization failure once the
    // token has been fixed: polling stops on 401/403 rather than retrying a
    // refused token every minute.
    void refresh();

    // The reply handlers. Public so the host and the tests can feed a reply
    // without a network; a status of 0 carries a transport error instead.
    void handleOrganizationReply(int status, const QByteArray &body,
                                 const QString &networkError);
    void handleIssuesReply(int status, const QByteArray &body,
                           const QString &networkError);

signals:
    void issuesChanged();
    void statusChanged(const QString &status, bool connected, bool error);
    void activity(const QString &text);

private:
    void fetchOrganization();
    void fetchIssues();
    QNetworkReply *startRequest(const QUrl &url);
    void schedulePoll(int delayMs);
    void setStatus(const QString &status, bool connected, bool error);
    void noteFailure(const QString &line, bool authorization,
                     int retryAfterMs);
    int retryDelayFor(int status, const QByteArray &retryAfter) const;

    QNetworkAccessManager *m_network = nullptr;
    forkmesh::sentry::Connection m_connection;
    QTimer *m_pollTimer = nullptr;
    QList<forkmesh::sentry::Issue> m_issues;
    QString m_organizationName;
    QString m_lastError;
    QString m_status;
    quint64 m_generation = 0;
    qint64 m_lastSyncMs = 0;
    bool m_active = false;
    bool m_connected = false;
    bool m_firstPoll = true;
    bool m_requestInFlight = false;
    bool m_refreshPending = false;
    bool m_authorizationFailed = false;
    int m_pendingRetryAfterMs = 0;
};
