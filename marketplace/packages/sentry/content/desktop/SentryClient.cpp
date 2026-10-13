#include "SentryClient.h"

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

#include <algorithm>

namespace forkmesh::sentry {
namespace {

QString jsonString(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    if (value.isString())
        return value.toString().trimmed();
    if (value.isDouble())
        return QString::number(value.toDouble(), 'f', 0);
    return {};
}

qint64 jsonCount(const QJsonValue &value)
{
    // Sentry serializes event counts as strings ("12") and user counts as
    // numbers; accept either, and refuse anything negative.
    qint64 count = 0;
    if (value.isString()) {
        bool ok = false;
        count = value.toString().trimmed().toLongLong(&ok);
        if (!ok)
            count = 0;
    } else if (value.isDouble()) {
        count = qint64(value.toDouble());
    }
    return qMax<qint64>(0, count);
}

qint64 jsonTimestampMs(const QJsonValue &value)
{
    if (!value.isString())
        return 0;
    QDateTime when =
        QDateTime::fromString(value.toString().trimmed(), Qt::ISODateWithMs);
    if (!when.isValid())
        when = QDateTime::fromString(value.toString().trimmed(), Qt::ISODate);
    return when.isValid() ? when.toMSecsSinceEpoch() : 0;
}

QUrl safeLink(const QString &text)
{
    const QUrl url(text.trimmed(), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty())
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
        return {};
    return url;
}

bool loopbackHost(const QString &host)
{
    const QString lower = host.toLower();
    return lower == QLatin1String("localhost") ||
           lower == QLatin1String("127.0.0.1") || lower == QLatin1String("::1") ||
           lower == QLatin1String("[::1]");
}

QString plural(qint64 count, const char *singular, const char *pluralForm)
{
    return QStringLiteral("%1 %2")
        .arg(count)
        .arg(count == 1 ? QLatin1String(singular) : QLatin1String(pluralForm));
}

QString statusText(int status)
{
    switch (status) {
    case 400: return QStringLiteral("400 Bad Request");
    case 401: return QStringLiteral("401 Unauthorized");
    case 403: return QStringLiteral("403 Forbidden");
    case 404: return QStringLiteral("404 Not Found");
    case 429: return QStringLiteral("429 Too Many Requests");
    case 500: return QStringLiteral("500 Internal Server Error");
    case 502: return QStringLiteral("502 Bad Gateway");
    case 503: return QStringLiteral("503 Service Unavailable");
    case 504: return QStringLiteral("504 Gateway Timeout");
    default: return QString::number(status);
    }
}

} // namespace

bool Connection::isValid() const
{
    return !token.isEmpty() && !organization.isEmpty() && baseUrl.isValid() &&
           !baseUrl.host().isEmpty();
}

QString normalizeAuthToken(const QString &token)
{
    QString trimmed = token.trimmed();
    if (trimmed.startsWith(QLatin1String("bearer "), Qt::CaseInsensitive))
        trimmed = trimmed.mid(7).trimmed();
    static const QRegularExpression pattern(
        QStringLiteral("^[A-Za-z0-9._=-]{8,512}$"));
    return pattern.match(trimmed).hasMatch() ? trimmed : QString();
}

QString normalizeSlug(const QString &slug)
{
    const QString lower = slug.trimmed().toLower();
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?$"));
    return pattern.match(lower).hasMatch() ? lower : QString();
}

QUrl normalizeBaseUrl(const QString &text)
{
    QString trimmed = text.trimmed();
    if (trimmed.isEmpty())
        return QUrl(kDefaultBaseUrl);
    if (!trimmed.contains(QLatin1String("://")))
        trimmed.prepend(QLatin1String("https://"));
    QUrl url(trimmed, QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty())
        return {};
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("http")) {
        if (!loopbackHost(url.host()))
            return {};
    } else if (scheme != QLatin1String("https")) {
        return {};
    }
    if (!url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return {};
    QString path = url.path();
    while (path.endsWith(QLatin1Char('/')))
        path.chop(1);
    if (path.endsWith(QLatin1String("/api/0")))
        path.chop(6);
    if (path.contains(QLatin1String("..")))
        return {};
    url.setScheme(scheme);
    url.setHost(url.host().toLower());
    url.setPath(path);
    return url;
}

QStringList statsPeriods()
{
    return {QStringLiteral("24h"), QStringLiteral("14d")};
}

QString normalizeStatsPeriod(const QString &period)
{
    const QString lower = period.trimmed().toLower();
    return statsPeriods().contains(lower) ? lower : kDefaultStatsPeriod;
}

QString statsPeriodLabel(const QString &period)
{
    return normalizeStatsPeriod(period) == QLatin1String("14d")
               ? QStringLiteral("last 14 days")
               : QStringLiteral("last 24 hours");
}

static QUrl apiUrl(const Connection &connection, const QString &suffix)
{
    QUrl url = connection.baseUrl;
    url.setPath(url.path() + QStringLiteral("/api/0/") + suffix);
    return url;
}

QUrl organizationUrlFor(const Connection &connection)
{
    if (connection.organization.isEmpty())
        return {};
    return apiUrl(connection, QStringLiteral("organizations/%1/")
                                  .arg(connection.organization));
}

QUrl issuesUrlFor(const Connection &connection)
{
    if (connection.organization.isEmpty())
        return {};
    QUrl url = connection.project.isEmpty()
                   ? apiUrl(connection, QStringLiteral("organizations/%1/issues/")
                                            .arg(connection.organization))
                   : apiUrl(connection, QStringLiteral("projects/%1/%2/issues/")
                                            .arg(connection.organization,
                                                 connection.project));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("query"), QStringLiteral("is:unresolved"));
    query.addQueryItem(QStringLiteral("statsPeriod"),
                       normalizeStatsPeriod(connection.statsPeriod));
    query.addQueryItem(QStringLiteral("limit"), QString::number(kMaxIssues));
    query.addQueryItem(QStringLiteral("sort"), QStringLiteral("freq"));
    url.setQuery(query);
    return url;
}

QUrl browserUrlFor(const Connection &connection)
{
    if (connection.organization.isEmpty())
        return {};
    QUrl url = connection.baseUrl;
    url.setPath(url.path() + QStringLiteral("/organizations/%1/issues/")
                                 .arg(connection.organization));
    if (!connection.project.isEmpty()) {
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("query"),
                           QStringLiteral("is:unresolved project:%1")
                               .arg(connection.project));
        url.setQuery(query);
    }
    return url;
}

QUrl authTokenPageFor(const Connection &connection)
{
    QUrl url = connection.baseUrl.isValid() && !connection.baseUrl.host().isEmpty()
                   ? connection.baseUrl
                   : QUrl(kDefaultBaseUrl);
    url.setPath(url.path() + QStringLiteral("/settings/account/api/auth-tokens/"));
    return url;
}

QString boundedText(const QString &text, int maxChars)
{
    QString clean;
    clean.reserve(text.size());
    bool space = false;
    for (const QChar &ch : text) {
        const bool blank = ch.isSpace() || ch.category() == QChar::Other_Control;
        if (blank) {
            space = true;
            continue;
        }
        if (space && !clean.isEmpty())
            clean.append(QLatin1Char(' '));
        space = false;
        clean.append(ch);
    }
    if (maxChars > 3 && clean.size() > maxChars)
        clean = clean.left(maxChars - 3).trimmed() + QStringLiteral("...");
    return clean;
}

QString parseErrorDetail(const QByteArray &body)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject())
        return {};
    return boundedText(jsonString(document.object(), "detail"), 120);
}

QString parseOrganizationName(const QByteArray &body)
{
    const QJsonDocument document = QJsonDocument::fromJson(body);
    if (!document.isObject())
        return {};
    return boundedText(jsonString(document.object(), "name"), 80);
}

bool parseIssues(const QByteArray &body, QList<Issue> *issues, QString *error)
{
    if (error)
        error->clear();
    if (!issues)
        return false;
    QJsonParseError parseError;
    const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("Sentry returned something other than JSON.");
        return false;
    }
    if (!document.isArray()) {
        if (error) {
            const QString detail = parseErrorDetail(body);
            *error = detail.isEmpty()
                         ? QStringLiteral("Sentry did not return an issue list.")
                         : detail;
        }
        return false;
    }

    QList<Issue> parsed;
    const QJsonArray entries = document.array();
    for (const QJsonValue &value : entries) {
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        Issue issue;
        issue.id = boundedText(jsonString(object, "id"), 40);
        if (issue.id.isEmpty())
            continue;
        issue.shortId = boundedText(jsonString(object, "shortId"), 40);
        const QJsonObject metadata =
            object.value(QStringLiteral("metadata")).toObject();
        issue.metadataType = boundedText(jsonString(metadata, "type"), 80);
        issue.metadataValue =
            boundedText(jsonString(metadata, "value"), kMaxTitleChars);
        issue.title = boundedText(jsonString(object, "title"), kMaxTitleChars);
        if (issue.title.isEmpty()) {
            issue.title = issue.metadataType.isEmpty()
                              ? issue.metadataValue
                              : issue.metadataValue.isEmpty()
                                    ? issue.metadataType
                                    : QStringLiteral("%1: %2").arg(
                                          issue.metadataType,
                                          issue.metadataValue);
        }
        if (issue.title.isEmpty())
            issue.title = QStringLiteral("Untitled issue");
        issue.culprit = boundedText(jsonString(object, "culprit"), kMaxTitleChars);
        issue.level = boundedText(jsonString(object, "level"), 16).toLower();
        issue.status = boundedText(jsonString(object, "status"), 24).toLower();
        issue.type = boundedText(jsonString(object, "type"), 24).toLower();
        const QJsonObject project =
            object.value(QStringLiteral("project")).toObject();
        issue.projectSlug = boundedText(jsonString(project, "slug"), 64);
        issue.projectName = boundedText(jsonString(project, "name"), 80);
        issue.permalink = safeLink(jsonString(object, "permalink"));
        issue.count = jsonCount(object.value(QStringLiteral("count")));
        issue.userCount =
            int(qMin<qint64>(jsonCount(object.value(QStringLiteral("userCount"))),
                             1'000'000'000));
        issue.firstSeenMs =
            jsonTimestampMs(object.value(QStringLiteral("firstSeen")));
        issue.lastSeenMs =
            jsonTimestampMs(object.value(QStringLiteral("lastSeen")));
        issue.unhandled =
            object.value(QStringLiteral("isUnhandled")).toBool(false);
        const QJsonObject stats = object.value(QStringLiteral("stats")).toObject();
        for (auto it = stats.constBegin(); it != stats.constEnd(); ++it) {
            if (!it.value().isArray())
                continue;
            qint64 sum = 0;
            const QJsonArray series = it.value().toArray();
            for (const QJsonValue &point : series) {
                const QJsonArray pair = point.toArray();
                if (pair.size() >= 2)
                    sum += jsonCount(pair.at(1));
            }
            issue.recentCount = sum;
            issue.recentKnown = true;
            break;
        }
        parsed.append(issue);
    }

    std::stable_sort(parsed.begin(), parsed.end(),
                     [](const Issue &left, const Issue &right) {
                         if (left.count != right.count)
                             return left.count > right.count;
                         if (left.lastSeenMs != right.lastSeenMs)
                             return left.lastSeenMs > right.lastSeenMs;
                         return left.shortId < right.shortId;
                     });
    if (parsed.size() > kMaxIssues)
        parsed = parsed.mid(0, kMaxIssues);
    *issues = parsed;
    return true;
}

Aggregate aggregate(const QList<Issue> &issues)
{
    Aggregate total;
    for (const Issue &issue : issues) {
        ++total.issueCount;
        total.eventCount += issue.count;
        total.userCount += issue.userCount;
        if (issue.unhandled)
            ++total.unhandledCount;
        total.newestLastSeenMs = qMax(total.newestLastSeenMs, issue.lastSeenMs);
        const QString project = issue.projectSlug.isEmpty()
                                    ? QStringLiteral("(no project)")
                                    : issue.projectSlug;
        total.issuesByProject[project] += 1;
        total.eventsByProject[project] += issue.count;
        const QString level =
            issue.level.isEmpty() ? QStringLiteral("unknown") : issue.level;
        total.issuesByLevel[level] += 1;
        total.eventsByLevel[level] += issue.count;
    }
    return total;
}

QList<Change> diffIssues(const QList<Issue> &previous,
                         const QList<Issue> &current)
{
    QHash<QString, qint64> previousCounts;
    previousCounts.reserve(previous.size());
    for (const Issue &issue : previous)
        previousCounts.insert(issue.id, issue.count);
    QList<Change> changes;
    for (const Issue &issue : current) {
        const auto it = previousCounts.constFind(issue.id);
        if (it == previousCounts.constEnd()) {
            changes.append(Change{Change::NewIssue, issue, 0});
        } else if (issue.count > it.value()) {
            changes.append(Change{Change::MoreEvents, issue, it.value()});
        }
    }
    return changes;
}

QString issueLabel(const Issue &issue)
{
    return issue.shortId.isEmpty() ? issue.id : issue.shortId;
}

QString summaryLine(const QString &organizationLabel, const Aggregate &total,
                    const QString &statsPeriod)
{
    return kLogPrefix +
           QString::fromUtf8("%1 \xC2\xB7 %2 \xC2\xB7 %3 in the %4 \xC2\xB7 %5")
               .arg(organizationLabel,
                    plural(total.issueCount, "unresolved issue",
                           "unresolved issues"),
                    plural(total.eventCount, "event", "events"),
                    statsPeriodLabel(statsPeriod),
                    plural(total.userCount, "user affected", "users affected"));
}

QString changeLine(const Change &change)
{
    const Issue &issue = change.issue;
    const QString project = issue.projectSlug.isEmpty()
                                ? QString()
                                : QString::fromUtf8(" \xC2\xB7 ") + issue.projectSlug;
    if (change.kind == Change::NewIssue) {
        return kLogPrefix +
               QString::fromUtf8("new issue %1 \xC2\xB7 %2 \xC2\xB7 %3 \xC2\xB7 %4%5")
                   .arg(issueLabel(issue), issue.title,
                        plural(issue.count, "event", "events"),
                        plural(issue.userCount, "user", "users"), project);
    }
    return kLogPrefix +
           QString::fromUtf8("%1 +%2 (%3) \xC2\xB7 %4%5")
               .arg(issueLabel(issue),
                    plural(issue.count - change.previousCount, "event",
                           "events"),
                    plural(issue.count, "event", "events"), issue.title,
                    project);
}

QString failureLine(const QUrl &baseUrl, int status, const QByteArray &body,
                    const QString &networkError)
{
    const QString host = baseUrl.host().isEmpty() ? QStringLiteral("Sentry")
                                                  : baseUrl.host();
    QString line = kLogPrefix +
                   QStringLiteral("could not read issues from %1").arg(host);
    if (status > 0) {
        line += QStringLiteral(" (%1)").arg(statusText(status));
        const QString detail = parseErrorDetail(body);
        if (!detail.isEmpty())
            line += QStringLiteral(": %1").arg(detail);
        if (status == 401 || status == 403) {
            line += QStringLiteral(
                " - check the API key and that it has the org:read and "
                "project:read scopes");
        } else if (status == 404) {
            line += QStringLiteral(
                " - check the organization and project slugs");
        } else if (status == 429) {
            line += QStringLiteral(" - Sentry's rate limit; reading again later");
        }
    } else {
        line += networkError.trimmed().isEmpty()
                    ? QStringLiteral(" (no response)")
                    : QStringLiteral(" (%1)").arg(boundedText(networkError, 160));
    }
    return line;
}

QString relativeTimeText(qint64 ms, qint64 nowMs)
{
    if (ms <= 0)
        return QStringLiteral("never");
    const qint64 ageSeconds = qMax<qint64>(0, (nowMs - ms) / 1000);
    if (ageSeconds < 5)
        return QStringLiteral("just now");
    if (ageSeconds < 60)
        return QStringLiteral("%1 s ago").arg(ageSeconds);
    if (ageSeconds < 3600)
        return QStringLiteral("%1 min ago").arg(ageSeconds / 60);
    if (ageSeconds < 86400)
        return QStringLiteral("%1 h ago").arg(ageSeconds / 3600);
    return QStringLiteral("%1 d ago").arg(ageSeconds / 86400);
}

QString levelLabel(const QString &level)
{
    const QString lower = level.trimmed().toLower();
    if (lower.isEmpty())
        return QStringLiteral("Unknown");
    return lower.at(0).toUpper() + lower.mid(1);
}

} // namespace forkmesh::sentry

using namespace forkmesh::sentry;

SentryClient::SentryClient(QNetworkAccessManager *network, QObject *parent)
    : QObject(parent), m_network(network)
{
    m_pollTimer = new QTimer(this);
    m_pollTimer->setSingleShot(true);
    // A once-a-minute timer costs nothing to keep exact, and the host shows
    // the time to the next read.
    m_pollTimer->setTimerType(Qt::PreciseTimer);
    connect(m_pollTimer, &QTimer::timeout, this, [this] {
        if (m_active)
            fetchIssues();
    });
    m_status = QStringLiteral("Not connected");
}

void SentryClient::setConnection(const Connection &connection)
{
    const bool wasActive = m_active;
    stop();
    m_connection = connection;
    m_connection.statsPeriod = normalizeStatsPeriod(connection.statsPeriod);
    m_issues.clear();
    m_organizationName.clear();
    m_lastError.clear();
    m_lastSyncMs = 0;
    m_firstPoll = true;
    m_authorizationFailed = false;
    emit issuesChanged();
    if (wasActive && isConfigured())
        start();
    else if (!isConfigured())
        setStatus(QStringLiteral("Not connected"), false, false);
}

QString SentryClient::organizationName() const
{
    return m_organizationName.isEmpty() ? m_connection.organization
                                        : m_organizationName;
}

QString SentryClient::organizationLabel() const
{
    if (m_organizationName.isEmpty() ||
        m_organizationName.compare(m_connection.organization,
                                   Qt::CaseInsensitive) == 0)
        return m_connection.organization;
    return QStringLiteral("%1 (%2)").arg(m_organizationName,
                                         m_connection.organization);
}

Aggregate SentryClient::aggregate() const
{
    return forkmesh::sentry::aggregate(m_issues);
}

int SentryClient::nextPollInMs() const
{
    return m_pollTimer && m_pollTimer->isActive() ? m_pollTimer->remainingTime()
                                                  : -1;
}

void SentryClient::start()
{
    if (!isConfigured()) {
        setStatus(QStringLiteral("Not connected"), false, false);
        return;
    }
    if (m_active)
        return;
    m_active = true;
    m_authorizationFailed = false;
    ++m_generation;
    m_requestInFlight = false;
    m_refreshPending = false;
    setStatus(QStringLiteral("Connecting to %1 as %2")
                  .arg(m_connection.baseUrl.host(), m_connection.organization),
              false, false);
    emit activity(kLogPrefix +
                  QStringLiteral("connecting to %1 for organization %2")
                      .arg(m_connection.baseUrl.host(),
                           m_connection.organization));
    fetchOrganization();
}

void SentryClient::stop()
{
    ++m_generation; // replies still in flight belong to the old run
    m_pollTimer->stop();
    m_active = false;
    m_connected = false;
    m_requestInFlight = false;
    m_refreshPending = false;
    setStatus(isConfigured() ? QStringLiteral("Stopped")
                             : QStringLiteral("Not connected"),
              false, false);
}

void SentryClient::refresh()
{
    if (!isConfigured())
        return;
    if (!m_active) {
        start();
        return;
    }
    if (m_requestInFlight) {
        m_refreshPending = true;
        return;
    }
    m_pollTimer->stop();
    if (m_authorizationFailed) {
        // The token may have been fixed: go back through the organization
        // read so a still-refused token is reported once, not every minute.
        m_authorizationFailed = false;
        fetchOrganization();
        return;
    }
    fetchIssues();
}

QNetworkReply *SentryClient::startRequest(const QUrl &url)
{
    if (!m_network || !url.isValid())
        return nullptr;
    QNetworkRequest request(url);
    request.setRawHeader("Authorization",
                         "Bearer " + m_connection.token.toUtf8());
    request.setRawHeader("Accept", "application/json");
    request.setRawHeader("Cache-Control", "no-store");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(15000);
    return m_network->get(request);
}

void SentryClient::fetchOrganization()
{
    if (!m_active || m_requestInFlight)
        return;
    QNetworkReply *reply = startRequest(organizationUrlFor(m_connection));
    if (!reply) {
        handleOrganizationReply(0, {}, QStringLiteral("no network access"));
        return;
    }
    m_requestInFlight = true;
    const quint64 generation = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        m_requestInFlight = false;
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        handleOrganizationReply(status, reply->read(kMaxReplyBytes),
                                status == 0 ? reply->errorString() : QString());
    });
}

void SentryClient::fetchIssues()
{
    if (!m_active || m_requestInFlight)
        return;
    m_pollTimer->stop();
    QNetworkReply *reply = startRequest(issuesUrlFor(m_connection));
    if (!reply) {
        handleIssuesReply(0, {}, QStringLiteral("no network access"));
        return;
    }
    m_requestInFlight = true;
    const quint64 generation = m_generation;
    connect(reply, &QNetworkReply::finished, this, [this, reply, generation] {
        reply->deleteLater();
        if (generation != m_generation)
            return;
        m_requestInFlight = false;
        const int status =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        m_pendingRetryAfterMs =
            retryDelayFor(status, reply->rawHeader("Retry-After"));
        handleIssuesReply(status, reply->read(kMaxReplyBytes),
                          status == 0 ? reply->errorString() : QString());
    });
}

int SentryClient::retryDelayFor(int status, const QByteArray &retryAfter) const
{
    if (status != 429)
        return 0;
    bool ok = false;
    const int seconds = retryAfter.trimmed().toInt(&ok);
    // Sentry answers a rate limit with the seconds to wait; honour it within
    // a window that neither hammers the API nor leaves the tab stale for an
    // hour because of one bad header.
    return ok ? qBound(60, seconds, 600) * 1000 : 5 * 60 * 1000;
}

void SentryClient::handleOrganizationReply(int status, const QByteArray &body,
                                           const QString &networkError)
{
    if (!m_active)
        return;
    if (status >= 200 && status < 300) {
        const QString name = parseOrganizationName(body);
        if (!name.isEmpty())
            m_organizationName = name;
    } else if (status == 401 || status == 403) {
        noteFailure(failureLine(m_connection.baseUrl, status, body,
                                networkError),
                    true, 0);
        return;
    }
    // Any other answer is not fatal for the read itself: the issue list is
    // what matters, and it reports its own failure if it has one.
    fetchIssues();
}

void SentryClient::handleIssuesReply(int status, const QByteArray &body,
                                     const QString &networkError)
{
    if (!m_active)
        return;
    const int retryAfterMs = m_pendingRetryAfterMs;
    m_pendingRetryAfterMs = 0;
    if (status < 200 || status >= 300) {
        noteFailure(failureLine(m_connection.baseUrl, status, body,
                                networkError),
                    status == 401 || status == 403, retryAfterMs);
        return;
    }
    QList<Issue> parsed;
    QString error;
    if (!parseIssues(body, &parsed, &error)) {
        noteFailure(kLogPrefix + error, false, 0);
        return;
    }

    const bool recovered = !m_lastError.isEmpty();
    m_lastError.clear();
    m_authorizationFailed = false;
    const QList<Issue> previous = m_issues;
    m_issues = parsed;
    m_lastSyncMs = QDateTime::currentMSecsSinceEpoch();
    m_connected = true;
    const Aggregate total = forkmesh::sentry::aggregate(m_issues);

    if (m_firstPoll || recovered) {
        // The first read of a run (and the first good read after a failure)
        // is a standing summary, not a flood of "new issue" lines for
        // everything that was already open.
        m_firstPoll = false;
        emit activity(summaryLine(organizationLabel(), total,
                                  m_connection.statsPeriod));
    } else {
        const QList<Change> changes = diffIssues(previous, m_issues);
        const int shown = qMin(int(changes.size()), kMaxChangeLines);
        for (int i = 0; i < shown; ++i)
            emit activity(changeLine(changes.at(i)));
        if (changes.size() > shown) {
            emit activity(kLogPrefix +
                          QStringLiteral("... and %1 more changed issues")
                              .arg(changes.size() - shown));
        }
    }

    setStatus(QStringLiteral("Connected to %1").arg(organizationLabel()), true,
              false);
    emit issuesChanged();
    if (m_refreshPending) {
        m_refreshPending = false;
        fetchIssues();
        return;
    }
    schedulePoll(kPollIntervalMs);
}

void SentryClient::noteFailure(const QString &line, bool authorization,
                               int retryAfterMs)
{
    m_connected = false;
    m_refreshPending = false;
    const bool changed = line != m_lastError;
    m_lastError = line;
    m_authorizationFailed = authorization;
    // A failure is logged when it starts, not every minute it persists; the
    // status line keeps showing it until a read succeeds.
    if (changed)
        emit activity(line);
    setStatus(line.mid(kLogPrefix.size()), false, true);
    emit issuesChanged();
    if (authorization) {
        m_pollTimer->stop(); // wait for a fixed token; see refresh()
        return;
    }
    schedulePoll(retryAfterMs > 0 ? retryAfterMs : kPollIntervalMs);
}

void SentryClient::schedulePoll(int delayMs)
{
    if (!m_active)
        return;
    m_pollTimer->start(qMax(1000, delayMs));
}

void SentryClient::setStatus(const QString &status, bool connected, bool error)
{
    m_status = status;
    m_connected = connected;
    emit statusChanged(status, connected, error);
}
