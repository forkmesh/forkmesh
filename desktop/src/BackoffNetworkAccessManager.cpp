#include "BackoffNetworkAccessManager.h"

#include <QJsonObject>

#include <QJsonDocument>

#include <QJsonArray>

#include "BackgroundActivity.h"
#include "NetworkReplyError.h"

#include <QByteArray>
#include <QDateTime>
#include <QHostAddress>
#include <QIODevice>
#include <QNetworkReply>
#include <QSet>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <utility>

namespace {

constexpr qint64 kBaseMs = 3LL * 1000;
constexpr qint64 kCapMs = 5LL * 60 * 1000;
constexpr int kPayloadPreviewBytes = 16 * 1024;
constexpr int kPayloadPreviewChars = 8192;

QString endpointFullUrl(const QUrl &url)
{
    return url.toString(QUrl::RemoveUserInfo);
}

QString payloadPreview(const QByteArray &bytes, qint64 totalBytes = -1)
{
    if (bytes.isEmpty())
        return QStringLiteral("-");

    const qint64 knownTotal = totalBytes > 0 ? totalBytes : bytes.size();
    const QByteArray sample = bytes.left(kPayloadPreviewBytes);
    int controlBytes = 0;
    bool binary = false;
    for (char raw : sample) {
        const uchar byte = static_cast<uchar>(raw);
        if (byte == 0) {
            binary = true;
            break;
        }
        if (byte < 0x20 && byte != '\n' && byte != '\r' && byte != '\t')
            ++controlBytes;
    }
    if (binary || (sample.size() > 0 && controlBytes > sample.size() / 8))
        return QStringLiteral("<binary body: %1 bytes>").arg(knownTotal);

    QString text = QString::fromUtf8(sample.constData(), sample.size());
    text.replace(QLatin1Char('\r'), QStringLiteral("\\r"));
    text.replace(QLatin1Char('\n'), QStringLiteral("\\n"));
    text.replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    bool truncated = knownTotal > sample.size();
    if (text.size() > kPayloadPreviewChars) {
        text = text.left(kPayloadPreviewChars);
        truncated = true;
    }
    if (truncated)
        text += QStringLiteral(" ... (%1 bytes)").arg(knownTotal);
    return text.trimmed().isEmpty() ? QStringLiteral("-") : text;
}

QString requestBodyPreview(QIODevice *device)
{
    if (!device)
        return QString();
    if (!device->isReadable())
        return QStringLiteral("<unreadable request body>");
    if (device->isSequential())
        return QStringLiteral("<streamed request body>");

    const qint64 knownTotal = qMax<qint64>(0, device->size());
    QByteArray sample = device->peek(kPayloadPreviewBytes + 1);
    if (sample.isEmpty() && knownTotal > 0) {
        const qint64 pos = device->pos();
        if (pos >= 0 && device->seek(pos)) {
            sample = device->read(kPayloadPreviewBytes + 1);
            device->seek(pos);
        }
    }
    if (sample.isEmpty())
        return knownTotal > 0
                   ? QStringLiteral("<request body: %1 bytes>").arg(knownTotal)
                   : QString();
    return payloadPreview(sample, knownTotal > 0 ? knownTotal : sample.size());
}

QString requestDataPreview(QNetworkAccessManager::Operation op,
                           const QUrl &url, QIODevice *outgoingData)
{
    const QString body = requestBodyPreview(outgoingData);
    if (!body.isEmpty())
        return body;
    if (url.hasQuery())
        return QStringLiteral("query: %1").arg(url.query(QUrl::FullyDecoded));
    switch (op) {
    case QNetworkAccessManager::PostOperation:
    case QNetworkAccessManager::PutOperation:
    case QNetworkAccessManager::CustomOperation:
        return QStringLiteral("<empty request body>");
    default:
        return QStringLiteral("-");
    }
}

QString privateRequestPreview(QIODevice *outgoingData)
{
    const qint64 size = outgoingData && !outgoingData->isSequential()
                            ? qMax<qint64>(0, outgoingData->size())
                            : 0;
    return QStringLiteral("<private request body: %1 bytes>").arg(size);
}

QString responseDataPreview(QNetworkReply *reply, qint64 downloaded)
{
    if (!reply)
        return QStringLiteral("-");
    const QByteArray body =
        forkmesh::networkReplyBodyPreview(reply, kPayloadPreviewBytes + 1);
    if (!body.isEmpty())
        return payloadPreview(body, downloaded > 0 ? downloaded : body.size());
    if (reply->error() != QNetworkReply::NoError)
        return forkmesh::networkReplyFailureSummary(reply);
    if (downloaded > 0)
        return QStringLiteral("<response body consumed before diagnostics: %1 bytes>")
            .arg(downloaded);
    return QStringLiteral("-");
}

class BackoffSuppressedReply : public QNetworkReply
{
public:
    BackoffSuppressedReply(const QNetworkRequest &request,
                            QNetworkAccessManager::Operation op, QObject *parent,
                            int causeStatus)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        setError(QNetworkReply::UnknownNetworkError,
                 causeStatus >= 500
                     ? QStringLiteral("ForkMesh relay failed with HTTP %1; "
                                      "backing off")
                           .arg(causeStatus)
                     : QStringLiteral("ForkMesh relay is rate-limited (HTTP "
                                      "429); backing off"));
        setAttribute(
            BackoffNetworkAccessManager::kBackoffSuppressedAttribute, true);
        setOpenMode(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            emit errorOccurred(error());
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}
    qint64 bytesAvailable() const override { return 0; }

protected:
    qint64 readData(char *, qint64) override { return -1; }
};

class DeniedReply : public QNetworkReply
{
public:
    // Blocked: a rule or a build switch refused the destination. Offline: the
    // reader switched the network off. OutdatedClient: this build is older
    // than the minimum supported release, and `reason` is the sentence the
    // popup and the log line use too.
    enum Kind { Blocked, Offline, OutdatedClient };

    DeniedReply(const QNetworkRequest &request,
                QNetworkAccessManager::Operation op, const QString &reason,
                QObject *parent, Kind kind = Blocked)
        : QNetworkReply(parent)
    {
        setRequest(request);
        setUrl(request.url());
        setOperation(op);
        if (kind == OutdatedClient) {
            // Not a transport failure and not a relay refusal: nothing was
            // sent, and nothing this build can do will change that but an
            // update. Callers that park-and-retry a network failure must not
            // treat it as one, so it reads as an access denial.
            setError(QNetworkReply::ContentAccessDenied,
                     QStringLiteral("%1; %2 was not sent")
                         .arg(reason, request.url().toDisplayString()));
            setAttribute(
                BackoffNetworkAccessManager::kOutdatedClientBlockedAttribute,
                true);
        } else if (kind == Offline) {
            // A network-class error, not an access denial: the callers that
            // sort a failure into "this node is offline, park it and retry"
            // versus "the relay refused it, give up" have to read this as the
            // former, because that is what it is.
            setError(QNetworkReply::TemporaryNetworkFailureError,
                     QStringLiteral("ForkMesh is in offline mode; %1 was not sent")
                         .arg(request.url().toDisplayString()));
            setAttribute(
                BackoffNetworkAccessManager::kOfflineBlockedAttribute, true);
        } else {
            setError(QNetworkReply::ContentAccessDenied,
                     QStringLiteral("ForkMesh %1 blocked %2")
                         .arg(reason, request.url().toDisplayString()));
        }
        setOpenMode(QIODevice::ReadOnly);
        QTimer::singleShot(0, this, [this] {
            emit errorOccurred(error());
            setFinished(true);
            emit finished();
        });
    }

    void abort() override {}
    qint64 bytesAvailable() const override { return 0; }

protected:
    qint64 readData(char *, qint64) override { return -1; }
};

class EndpointTransferTracker : public QObject
{
public:
    using QObject::QObject;
    qint64 uploaded = 0;
    qint64 downloaded = 0;
};

// A URL rule pattern: [scheme://]host[:port]/path, where the host may be
// *.domain and the path may hold * wildcards. Query and fragment are dropped;
// a pattern names paths, not parameters.
struct UrlGlob {
    QString scheme = QStringLiteral("*");
    QString host;
    int port = -1;
    QString path;
};

bool parseUrlGlob(QString text, UrlGlob *out)
{
    text = text.trimmed();
    UrlGlob glob;
    const int sep = text.indexOf(QLatin1String("://"));
    if (sep >= 0) {
        glob.scheme = text.left(sep).trimmed().toLower();
        text = text.mid(sep + 3);
        if (glob.scheme.isEmpty())
            return false;
        if (glob.scheme != QLatin1String("*")) {
            for (const QChar c : std::as_const(glob.scheme)) {
                if (!(c.isLetterOrNumber() || c == QLatin1Char('+') ||
                      c == QLatin1Char('-') || c == QLatin1Char('.')))
                    return false;
            }
        }
    }
    const int slash = text.indexOf(QLatin1Char('/'));
    if (slash <= 0)
        return false;
    QString authority = text.left(slash).toLower();
    glob.path = text.mid(slash);
    for (int i = 0; i < glob.path.size(); ++i) {
        if (glob.path.at(i) == QLatin1Char('?') || glob.path.at(i) == QLatin1Char('#')) {
            glob.path.truncate(i);
            break;
        }
    }
    const int colon = authority.lastIndexOf(QLatin1Char(':'));
    if (colon >= 0) {
        bool ok = false;
        glob.port = authority.mid(colon + 1).toInt(&ok);
        if (!ok || glob.port <= 0 || glob.port > 65535)
            return false;
        authority.truncate(colon);
    }
    const QString bare = authority.startsWith(QLatin1String("*."))
                             ? authority.mid(2)
                             : authority;
    if (bare.isEmpty() || bare.startsWith(QLatin1Char('.')) ||
        bare.contains(QLatin1Char('*')) || bare.contains(QLatin1Char(':')) ||
        bare.contains(QLatin1Char('[')) || bare.contains(QLatin1Char('@')))
        return false;
    glob.host = authority;
    *out = glob;
    return true;
}

QString urlGlobText(const UrlGlob &glob, bool withAnyScheme)
{
    QString text;
    if (withAnyScheme || glob.scheme != QLatin1String("*"))
        text = glob.scheme + QStringLiteral("://");
    text += glob.host;
    if (glob.port > 0)
        text += QLatin1Char(':') + QString::number(glob.port);
    return text + glob.path;
}

// * matches any run of characters, slashes included.
bool wildcardMatch(const QString &pattern, const QString &text)
{
    int p = 0, t = 0, star = -1, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && pattern.at(p) == QLatin1Char('*')) {
            star = p++;
            mark = t;
        } else if (p < pattern.size() && pattern.at(p) == text.at(t)) {
            ++p;
            ++t;
        } else if (star >= 0) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern.at(p) == QLatin1Char('*'))
        ++p;
    return p == pattern.size();
}

QString canonicalExactUrl(const QUrl &input)
{
    QUrl url = input;
    url.setScheme(url.scheme().toLower());
    url.setHost(url.host().toLower());
    if (url.path().isEmpty())
        url.setPath(QStringLiteral("/"));
    return url.toString(QUrl::RemoveUserInfo | QUrl::FullyEncoded);
}

} // namespace

BackoffNetworkAccessManager::BackoffNetworkAccessManager(QObject *parent)
    : QNetworkAccessManager(parent)
{
}

QNetworkReply *BackoffNetworkAccessManager::createNetworkRequest(
    Operation op, const QNetworkRequest &request, QIODevice *outgoingData)
{
    return QNetworkAccessManager::createRequest(op, request, outgoingData);
}

void BackoffNetworkAccessManager::setFirewallEnabled(bool enabled)
{
    m_firewallEnabled = enabled;
}

void BackoffNetworkAccessManager::setFirewallRules(const QStringList &rules)
{
    m_firewallRules.clear();
    for (const QString &rule : rules)
        addFirewallRule(rule);
}

void BackoffNetworkAccessManager::setFirewallDenyRules(const QStringList &rules)
{
    m_firewallDenyRules.clear();
    for (const QString &rule : rules)
        addFirewallDenyRule(rule);
}

bool BackoffNetworkAccessManager::addFirewallRule(const QString &rule)
{
    const QString canonical = canonicalFirewallRule(rule);
    if (canonical.isEmpty())
        return false;
    const bool wasDenied = m_firewallDenyRules.removeAll(canonical) > 0;
    if (m_firewallRules.contains(canonical))
        return wasDenied;
    m_firewallRules.append(canonical);
    m_firewallRules.sort(Qt::CaseInsensitive);
    return true;
}

bool BackoffNetworkAccessManager::removeFirewallRule(const QString &rule)
{
    const QString canonical = canonicalFirewallRule(rule);
    return m_firewallRules.removeAll(canonical) > 0;
}

bool BackoffNetworkAccessManager::addFirewallDenyRule(const QString &rule)
{
    const QString canonical = canonicalFirewallRule(rule);
    if (canonical.isEmpty())
        return false;
    const bool wasAllowed = m_firewallRules.removeAll(canonical) > 0;
    if (m_firewallDenyRules.contains(canonical))
        return wasAllowed;
    m_firewallDenyRules.append(canonical);
    m_firewallDenyRules.sort(Qt::CaseInsensitive);
    return true;
}

bool BackoffNetworkAccessManager::removeFirewallDenyRule(const QString &rule)
{
    const QString canonical = canonicalFirewallRule(rule);
    return m_firewallDenyRules.removeAll(canonical) > 0;
}

void BackoffNetworkAccessManager::setFirewallPrompt(FirewallPrompt prompt)
{
    m_firewallPrompt = std::move(prompt);
}

void BackoffNetworkAccessManager::setOffMachineRequestsBlocked(bool blocked)
{
    m_offMachineBlocked = blocked;
}

void BackoffNetworkAccessManager::setOfflineMode(bool offline)
{
    if (m_offlineMode == offline)
        return;
    m_offlineMode = offline;
    // Whatever the pending connections were waiting on, they are not going to
    // get it: a network that just came back has new routes, and one that just
    // went away has stale sockets that would otherwise be reused first.
    clearConnectionCache();
}

void BackoffNetworkAccessManager::setClientVersion(const QString &version)
{
    m_clientVersion = version.trimmed();
    m_clientVersionBlocked = forkmesh::clientVersionTooOldForNetwork(
        m_clientVersion, m_minimumNetworkVersion);
}

void BackoffNetworkAccessManager::setMinimumNetworkVersion(
    const QString &minimum)
{
    const QString trimmed = minimum.trimmed();
    m_minimumNetworkVersion =
        trimmed.isEmpty() ? forkmesh::minimumNetworkVersion() : trimmed;
    m_clientVersionBlocked = forkmesh::clientVersionTooOldForNetwork(
        m_clientVersion, m_minimumNetworkVersion);
}

void BackoffNetworkAccessManager::markReachabilityProbe(QNetworkRequest &request)
{
    request.setAttribute(kReachabilityProbeAttribute, true);
}

bool BackoffNetworkAccessManager::isReachabilityProbe(
    const QNetworkRequest &request)
{
    return request.attribute(kReachabilityProbeAttribute).toBool();
}

static_assert(BackoffNetworkAccessManager::kOfflineBlockedAttribute ==
                  forkmesh::kOfflineBlockedAttribute,
              "the offline-blocked attribute is named in two headers and both "
              "have to mean the same reply attribute");

bool BackoffNetworkAccessManager::isOfflineBlocked(const QNetworkReply *reply)
{
    return forkmesh::networkReplyBlockedByOfflineMode(reply);
}

static_assert(BackoffNetworkAccessManager::kOutdatedClientBlockedAttribute ==
                  forkmesh::kOutdatedClientBlockedAttribute,
              "the outdated-client attribute is named in two headers and both "
              "have to mean the same reply attribute");

bool BackoffNetworkAccessManager::isOutdatedClientBlocked(
    const QNetworkReply *reply)
{
    return forkmesh::networkReplyBlockedByOutdatedClient(reply);
}

bool BackoffNetworkAccessManager::isOnThisMachine(const QUrl &url)
{
    const QString host = normalizedHost(url.host());
    // No host at all is a data:/file:/qrc: URL — those never reach the wire.
    if (host.isEmpty())
        return true;
    if (host == QLatin1String("localhost") ||
        host.endsWith(QLatin1String(".localhost")))
        return true;
    // A literal address decides itself (QUrl has already unwrapped an IPv6
    // host from its brackets); a name that is not one of the above would have
    // to be resolved to know, and resolving is already the network.
    const QHostAddress address(host);
    return !address.isNull() && address.isLoopback();
}

QList<BackoffNetworkAccessManager::EndpointStats>
BackoffNetworkAccessManager::endpointStats() const
{
    QList<EndpointStats> rows;
    rows.reserve(m_endpointStats.size());
    for (const EndpointStats &stats : m_endpointStats) {
        EndpointStats summary = stats;
        summary.requests.clear();
        rows.append(summary);
    }
    std::sort(rows.begin(), rows.end(), [](const EndpointStats &a,
                                           const EndpointStats &b) {
        return a.lastCalledMs > b.lastCalledMs;
    });
    return rows;
}

QList<BackoffNetworkAccessManager::DestinationStats>
BackoffNetworkAccessManager::destinationStats() const
{
    QHash<QString, DestinationStats> byHost;
    for (const EndpointStats &stats : m_endpointStats) {
        const QUrl url(stats.lastFullUrl.isEmpty() ? stats.endpoint
                                                   : stats.lastFullUrl);
        const QString host = normalizedHost(url.host());
        if (host.isEmpty())
            continue;
        DestinationStats &row = byHost[host];
        if (row.host.isEmpty() || stats.lastCalledMs > row.lastCalledMs) {
            row.sampleUrl = url;
            row.lastCalledMs = stats.lastCalledMs;
        }
        row.host = host;
        row.calls += stats.calls;
        row.allowed += stats.allowed;
        row.blocked += stats.blocked;
    }
    QList<DestinationStats> rows;
    rows.reserve(byHost.size());
    for (DestinationStats row : std::as_const(byHost)) {
        row.onThisMachine = isOnThisMachine(row.sampleUrl);
        row.match = firewallMatch(row.sampleUrl);
        rows.append(row);
    }
    std::sort(rows.begin(), rows.end(),
              [](const DestinationStats &a, const DestinationStats &b) {
                  if (a.calls != b.calls)
                      return a.calls > b.calls;
                  return a.lastCalledMs > b.lastCalledMs;
              });
    return rows;
}

QList<BackoffNetworkAccessManager::RequestRecord>
BackoffNetworkAccessManager::endpointRequests(const QString &method,
                                              const QString &endpoint) const
{
    Q_UNUSED(method);
    QList<RequestRecord> rows;
    for (const EndpointStats &stats : m_endpointStats) {
        if (stats.endpoint == endpoint) {
            rows = stats.requests;
            break;
        }
    }
    std::sort(rows.begin(), rows.end(), [](const RequestRecord &a,
                                           const RequestRecord &b) {
        return qMax(a.finishedMs, a.startedMs) >
               qMax(b.finishedMs, b.startedMs);
    });
    return rows;
}

bool BackoffNetworkAccessManager::hostInCooldown(const QString &host) const
{
    return !m_backoff.ready(normalizedHost(host),
                            QDateTime::currentMSecsSinceEpoch());
}

qint64 BackoffNetworkAccessManager::hostCooldownRemainingMs(
    const QString &host) const
{
    return m_backoff.msUntilReady(normalizedHost(host),
                                  QDateTime::currentMSecsSinceEpoch());
}

bool BackoffNetworkAccessManager::isBackoffSuppressed(const QNetworkReply *reply)
{
    return reply
           && reply->attribute(kBackoffSuppressedAttribute).toBool();
}

QString BackoffNetworkAccessManager::normalizedHost(const QString &host)
{
    return host.trimmed().toLower();
}

int BackoffNetworkAccessManager::normalizedPort(const QUrl &url)
{
    if (url.port() > 0)
        return url.port();
    const QString scheme = url.scheme().toLower();
    if (scheme == QLatin1String("https") || scheme == QLatin1String("wss"))
        return 443;
    if (scheme == QLatin1String("http") || scheme == QLatin1String("ws"))
        return 80;
    return -1;
}

QString BackoffNetworkAccessManager::canonicalFirewallRule(const QString &rule)
{
    QString text = rule.trimmed();
    if (text.isEmpty())
        return QString();
    if (text == QLatin1String("*"))
        return text;

    const QString lower = text.toLower();
    if (lower.startsWith(QStringLiteral("scheme:"))) {
        const QString scheme = lower.mid(7).trimmed();
        return scheme.isEmpty() ? QString() : QStringLiteral("scheme:") + scheme;
    }
    if (lower.startsWith(QStringLiteral("hostwild:"))) {
        const QString host = normalizedHost(text.mid(9));
        return host.isEmpty() ? QString() : QStringLiteral("hostwild:") + host;
    }
    if (lower.startsWith(QStringLiteral("hostport:"))) {
        const QString rest = text.mid(9).trimmed();
        const int colon = rest.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0)
            return QString();
        bool ok = false;
        const int port = rest.mid(colon + 1).toInt(&ok);
        const QString host = normalizedHost(rest.left(colon));
        if (!ok || port <= 0 || host.isEmpty())
            return QString();
        return QStringLiteral("hostport:%1:%2").arg(host).arg(port);
    }
    if (lower.startsWith(QStringLiteral("host:"))) {
        const QString host = normalizedHost(text.mid(5));
        return host.isEmpty() ? QString() : QStringLiteral("host:") + host;
    }
    if (lower.startsWith(QStringLiteral("urlglob:"))) {
        UrlGlob glob;
        return parseUrlGlob(text.mid(8), &glob)
                   ? QStringLiteral("urlglob:") + urlGlobText(glob, true)
                   : QString();
    }
    if (lower.startsWith(QStringLiteral("url:"))) {
        QUrl url(text.mid(4).trimmed());
        if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty())
            return QString();
        return QStringLiteral("url:") + canonicalExactUrl(url);
    }

    // A path makes it a URL rule. Without a scheme, or with a *, it is a
    // pattern (any scheme unless one is named, any query); a full URL with a
    // scheme and no * stays one exact URL.
    {
        const int sep = text.indexOf(QLatin1String("://"));
        const QString rest = sep >= 0 ? text.mid(sep + 3) : text;
        const int slash = rest.indexOf(QLatin1Char('/'));
        const bool hasPath = slash > 0 && slash < rest.size() - 1;
        if (hasPath && (sep < 0 || text.contains(QLatin1Char('*')))) {
            UrlGlob glob;
            return parseUrlGlob(text, &glob)
                       ? QStringLiteral("urlglob:") + urlGlobText(glob, true)
                       : QString();
        }
    }

    if (lower.startsWith(QStringLiteral("*."))) {
        const QString host = normalizedHost(text.mid(2));
        return host.isEmpty() ? QString() : QStringLiteral("hostwild:") + host;
    }

    QUrl asUrl = QUrl::fromUserInput(text);
    if (asUrl.isValid() && !asUrl.scheme().isEmpty() && !asUrl.host().isEmpty()) {
        if (!asUrl.path().isEmpty() && asUrl.path() != QLatin1String("/"))
            return QStringLiteral("url:") + canonicalExactUrl(asUrl);
        if (asUrl.hasQuery())
            return QStringLiteral("url:") + canonicalExactUrl(asUrl);
        if (asUrl.port() > 0)
            return QStringLiteral("hostport:%1:%2")
                .arg(normalizedHost(asUrl.host()))
                .arg(normalizedPort(asUrl));
        return QStringLiteral("host:") + normalizedHost(asUrl.host());
    }

    const int colon = text.lastIndexOf(QLatin1Char(':'));
    if (colon > 0) {
        bool ok = false;
        const int port = text.mid(colon + 1).toInt(&ok);
        const QString host = normalizedHost(text.left(colon));
        if (ok && port > 0 && !host.isEmpty())
            return QStringLiteral("hostport:%1:%2").arg(host).arg(port);
    }

    const QString host = normalizedHost(text);
    return host.isEmpty() ? QString() : QStringLiteral("host:") + host;
}

QString BackoffNetworkAccessManager::firewallRuleForUrl(const QUrl &url,
                                                        bool includePort)
{
    if (url.host().isEmpty())
        return QString();
    if (includePort) {
        const int port = normalizedPort(url);
        if (port > 0)
            return QStringLiteral("hostport:%1:%2")
                .arg(normalizedHost(url.host()))
                .arg(port);
    }
    return QStringLiteral("host:") + normalizedHost(url.host());
}

QString BackoffNetworkAccessManager::firewallRuleForExactUrl(const QUrl &url)
{
    if (!url.isValid() || url.scheme().isEmpty() || url.host().isEmpty())
        return QString();
    return QStringLiteral("url:") + canonicalExactUrl(url);
}

qint64 BackoffNetworkAccessManager::firewallRuleSpecificity(const QString &rule)
{
    // Host first, then path: a rule naming the host outranks a wildcard host
    // whatever their paths, and a longer literal path outranks a shorter one.
    const auto key = [](qint64 hostScore, qint64 pathScore, bool scheme) {
        return hostScore * 1000000 + qMin<qint64>(pathScore, 99999) * 10 +
               (scheme ? 1 : 0);
    };
    if (rule.startsWith(QStringLiteral("scheme:")))
        return key(0, 0, true);
    if (rule.startsWith(QStringLiteral("hostwild:")))
        return key(100 + qMin<qint64>(rule.size() - 9, 800), 0, false);
    if (rule.startsWith(QStringLiteral("hostport:")))
        return key(1001, 0, false);
    if (rule.startsWith(QStringLiteral("host:")))
        return key(1000, 0, false);
    if (rule.startsWith(QStringLiteral("url:")))
        return key(1001, 99999, true);
    if (rule.startsWith(QStringLiteral("urlglob:"))) {
        UrlGlob glob;
        if (!parseUrlGlob(rule.mid(8), &glob))
            return 0;
        const bool wildHost = glob.host.startsWith(QLatin1String("*."));
        const qint64 hostScore =
            (wildHost ? 100 + qMin<qint64>(glob.host.size() - 2, 800) : 1000) +
            (glob.port > 0 ? 1 : 0);
        // One exact path outranks any wildcard path on the same host, however
        // long the wildcard's literal part: api/tasks is more specific than
        // api/tasks/*, which also covers api/tasks.
        const qint64 literal = glob.path.size() - glob.path.count(QLatin1Char('*'));
        const qint64 pathScore = glob.path.contains(QLatin1Char('*'))
                                     ? qMin<qint64>(literal, 89999)
                                     : 90000 + qMin<qint64>(literal, 9998);
        return key(hostScore, pathScore, glob.scheme != QLatin1String("*"));
    }
    return 0; // "*"
}

QString BackoffNetworkAccessManager::firewallRuleForPath(const QUrl &url, bool andBelow)
{
    const QString host = normalizedHost(url.host());
    if (host.isEmpty() || host.contains(QLatin1Char(':')))
        return QString();
    QString path = url.path();
    if (andBelow) {
        const int slash = path.lastIndexOf(QLatin1Char('/'));
        if (slash <= 0)
            return QString(); // top level: the host rule already says this
        path = path.left(slash + 1) + QLatin1Char('*');
    } else if (path.isEmpty() || path == QLatin1String("/")) {
        return QString();
    }
    UrlGlob glob;
    glob.host = host;
    glob.port = url.port();
    glob.path = path;
    return canonicalFirewallRule(QStringLiteral("urlglob:") + urlGlobText(glob, true));
}

QString BackoffNetworkAccessManager::firewallRuleEditText(const QString &rule)
{
    const QString canonical = canonicalFirewallRule(rule);
    if (canonical.startsWith(QStringLiteral("hostwild:")))
        return QStringLiteral("*.") + canonical.mid(9);
    if (canonical.startsWith(QStringLiteral("hostport:")))
        return canonical.mid(9);
    if (canonical.startsWith(QStringLiteral("host:")))
        return canonical.mid(5);
    if (canonical.startsWith(QStringLiteral("url:")))
        return canonical.mid(4);
    if (canonical.startsWith(QStringLiteral("urlglob:"))) {
        UrlGlob glob;
        if (parseUrlGlob(canonical.mid(8), &glob))
            return urlGlobText(glob, false);
    }
    return canonical;
}

QList<BackoffNetworkAccessManager::FirewallRuleEntry>
BackoffNetworkAccessManager::parseFirewallRuleList(const QByteArray &json,
                                                   QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (!doc.isObject()) {
        if (error)
            *error = parseError.error == QJsonParseError::NoError
                         ? QStringLiteral("the rule list is not a JSON object")
                         : parseError.errorString();
        return {};
    }
    QList<FirewallRuleEntry> entries;
    QStringList invalid;
    const QJsonArray rules = doc.object().value(QStringLiteral("rules")).toArray();
    for (const QJsonValue &value : rules) {
        const QJsonObject object = value.toObject();
        const QString text = object.value(QStringLiteral("rule")).toString();
        const QString canonical = canonicalFirewallRule(text);
        if (canonical.isEmpty()) {
            invalid.append(text.isEmpty() ? QStringLiteral("(empty)") : text);
            continue;
        }
        const bool duplicate =
            std::any_of(entries.cbegin(), entries.cend(),
                        [&canonical](const FirewallRuleEntry &entry) {
                            return entry.rule == canonical;
                        });
        if (!duplicate)
            entries.append({canonical,
                            object.value(QStringLiteral("purpose")).toString().trimmed()});
    }
    if (error)
        *error = invalid.isEmpty()
                     ? QString()
                     : QStringLiteral("not valid rules: ") +
                           invalid.join(QStringLiteral(", "));
    return entries;
}

QList<BackoffNetworkAccessManager::EndpointProfile>
BackoffNetworkAccessManager::parseEndpointCatalog(const QByteArray &json,
                                                  QString *error)
{
    QJsonParseError parseError;
    const QJsonDocument doc = QJsonDocument::fromJson(json, &parseError);
    if (!doc.isObject()) {
        if (error)
            *error = parseError.error == QJsonParseError::NoError
                         ? QStringLiteral("the endpoint catalog is not a JSON object")
                         : parseError.errorString();
        return {};
    }
    QList<EndpointProfile> profiles;
    QStringList invalid;
    const QJsonArray endpoints =
        doc.object().value(QStringLiteral("endpoints")).toArray();
    for (const QJsonValue &value : endpoints) {
        const QJsonObject object = value.toObject();
        EndpointProfile profile;
        const QString patternText = object.value(QStringLiteral("pattern")).toString();
        profile.pattern = canonicalFirewallRule(patternText);
        profile.method =
            object.value(QStringLiteral("method")).toString().trimmed().toUpper();
        profile.description =
            object.value(QStringLiteral("description")).toString().trimmed();
        profile.importance = object.value(QStringLiteral("importance")).toInt(-1);
        profile.trigger = object.value(QStringLiteral("trigger")).toString().trimmed();
        profile.modestPerHour =
            object.value(QStringLiteral("modestPerHour")).toDouble(-1);
        profile.alternative =
            object.value(QStringLiteral("alternative")).toString().trimmed();
        profile.requestNeeded = object.value(QStringLiteral("requestNeeded")).toInt(-1);
        for (const QJsonValue &source :
             object.value(QStringLiteral("source")).toArray()) {
            const QString text = source.toString().trimmed();
            if (!text.isEmpty())
                profile.sources.append(text);
        }
        if (profile.pattern.isEmpty() || profile.description.isEmpty() ||
            profile.importance < 1 || profile.importance > 10 ||
            profile.modestPerHour < 0 || profile.requestNeeded < 0 ||
            profile.requestNeeded > 100) {
            invalid.append(patternText.isEmpty() ? QStringLiteral("(no pattern)")
                                                 : patternText);
            continue;
        }
        profiles.append(profile);
    }
    if (error)
        *error = invalid.isEmpty()
                     ? QString()
                     : QStringLiteral("invalid endpoint entries: ") +
                           invalid.join(QStringLiteral(", "));
    return profiles;
}

int BackoffNetworkAccessManager::matchEndpointProfile(
    const QList<EndpointProfile> &profiles, const QString &method, const QUrl &url)
{
    const QString wanted = method.trimmed().toUpper();
    int best = -1;
    qint64 bestScore = -1;
    for (int i = 0; i < profiles.size(); ++i) {
        const EndpointProfile &profile = profiles.at(i);
        if (!profile.method.isEmpty() && profile.method != wanted)
            continue;
        if (!firewallRuleMatches(profile.pattern, url))
            continue;
        // Specificity leaves the lowest digit for the scheme; the method gets
        // a bit above the whole key so a named method always wins a tie.
        const qint64 score = firewallRuleSpecificity(profile.pattern) * 2 +
                             (profile.method.isEmpty() ? 0 : 1);
        if (score > bestScore) {
            bestScore = score;
            best = i;
        }
    }
    return best;
}

double BackoffNetworkAccessManager::observedCallsPerHour(const EndpointStats &stats,
                                                        qint64 nowMs)
{
    if (stats.calls <= 0 || stats.firstCalledMs <= 0)
        return 0;
    const qint64 windowMs = qMax<qint64>(nowMs - stats.firstCalledMs, 10 * 60 * 1000);
    return double(stats.calls) * 3600.0 * 1000.0 / double(windowMs);
}

QString BackoffNetworkAccessManager::firewallMethodName(Operation op)
{
    switch (op) {
    case HeadOperation:
        return QStringLiteral("HEAD");
    case GetOperation:
        return QStringLiteral("GET");
    case PutOperation:
        return QStringLiteral("PUT");
    case PostOperation:
        return QStringLiteral("POST");
    case DeleteOperation:
        return QStringLiteral("DELETE");
    case CustomOperation:
        return QStringLiteral("CUSTOM");
    default:
        return QStringLiteral("REQ");
    }
}

QString BackoffNetworkAccessManager::firewallRuleLabel(const QString &rule)
{
    const QString canonical = canonicalFirewallRule(rule);
    if (canonical == QLatin1String("*"))
        return QStringLiteral("All destinations");
    if (canonical.startsWith(QStringLiteral("scheme:")))
        return QStringLiteral("All %1 requests").arg(canonical.mid(7).toUpper());
    if (canonical.startsWith(QStringLiteral("hostwild:")))
        return QStringLiteral("Host *.%1").arg(canonical.mid(9));
    if (canonical.startsWith(QStringLiteral("hostport:")))
        return QStringLiteral("Host %1").arg(canonical.mid(9));
    if (canonical.startsWith(QStringLiteral("host:")))
        return QStringLiteral("Host %1").arg(canonical.mid(5));
    if (canonical.startsWith(QStringLiteral("urlglob:")))
        return QStringLiteral("URLs %1").arg(firewallRuleEditText(canonical));
    if (canonical.startsWith(QStringLiteral("url:")))
        return QStringLiteral("URL %1").arg(canonical.mid(4));
    return canonical;
}

QString BackoffNetworkAccessManager::suggestedFirewallWildcard(const QUrl &url)
{
    const QString host = normalizedHost(url.host());
    if (host.isEmpty() || !QHostAddress(host).isNull())
        return QString();
    const QStringList labels = host.split(QLatin1Char('.'), Qt::SkipEmptyParts);
    if (labels.size() < 2)
        return QString();
    // A two-letter country code under a generic second level (co.uk, com.au)
    // is still a public suffix, so keep one more label.
    static const QStringList genericSecondLevels = {
        QStringLiteral("co"),  QStringLiteral("com"), QStringLiteral("net"),
        QStringLiteral("org"), QStringLiteral("gov"), QStringLiteral("ac"),
        QStringLiteral("edu")};
    int keep = 2;
    if (labels.size() >= 3 && labels.last().size() == 2 &&
        genericSecondLevels.contains(labels.at(labels.size() - 2)))
        keep = 3;
    return QStringLiteral("*.") +
           labels.mid(labels.size() - keep).join(QLatin1Char('.'));
}

QList<BackoffNetworkAccessManager::FirewallRuleSuggestion>
BackoffNetworkAccessManager::firewallRuleSuggestions(const QUrl &url,
                                                     const QString &catalogPattern)
{
    QList<FirewallRuleSuggestion> suggestions;
    const QString host = normalizedHost(url.host());
    if (!url.isValid() || host.isEmpty())
        return suggestions;
    const QString catalogRule = canonicalFirewallRule(catalogPattern);
    const auto offer = [&](const QString &rule, const QString &scope,
                           const QString &explanation) {
        const QString canonical = canonicalFirewallRule(rule);
        if (canonical.isEmpty() || !firewallRuleMatches(canonical, url))
            return;
        for (const FirewallRuleSuggestion &existing : std::as_const(suggestions)) {
            if (existing.rule == canonical)
                return;
        }
        suggestions.append({canonical, scope, explanation});
    };
    // Path rules on one host sort by specificity, so the catalog pattern
    // lands between the folders it is narrower and broader than.
    const auto offerSorted = [&](QList<FirewallRuleSuggestion> group) {
        std::stable_sort(group.begin(), group.end(),
                         [](const FirewallRuleSuggestion &a,
                            const FirewallRuleSuggestion &b) {
                             return firewallRuleSpecificity(a.rule) >
                                    firewallRuleSpecificity(b.rule);
                         });
        for (const FirewallRuleSuggestion &suggestion : std::as_const(group))
            offer(suggestion.rule, suggestion.scope, suggestion.explanation);
    };
    const auto pathGlob = [&url](const QString &globHost, const QString &path) {
        UrlGlob glob;
        glob.host = globHost;
        glob.port = url.port();
        glob.path = path;
        return canonicalFirewallRule(QStringLiteral("urlglob:") + urlGlobText(glob, true));
    };
    const QString catalogScope = QStringLiteral("This kind of call");
    const QString catalogExplanation = QStringLiteral(
        "The endpoint catalog's pattern for this call, so every variant of it is "
        "covered and nothing else.");
    const bool catalogIsPath = catalogRule.startsWith(QLatin1String("urlglob:"));
    const bool catalogWildHost =
        catalogIsPath && catalogRule.contains(QLatin1String("://*."));

    offer(firewallRuleForExactUrl(url), QStringLiteral("This exact URL"),
          QStringLiteral("Only this URL, query string included. The same path with a "
                         "different query asks again."));

    // The path, then every folder above it: /api/repo/acme/*, /api/repo/*, /api/*.
    const QString path = url.path();
    QList<FirewallRuleSuggestion> hostPaths;
    if (!path.isEmpty() && path != QLatin1String("/"))
        hostPaths.append({pathGlob(host, path), QStringLiteral("This path"),
                          QStringLiteral("%1 on %2 with any query string, over any "
                                         "scheme.")
                              .arg(path, host)});
    const QStringList segments = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (qsizetype depth = segments.size() - 1; depth >= 1; --depth) {
        const QString folder =
            QLatin1Char('/') + segments.mid(0, depth).join(QLatin1Char('/'));
        hostPaths.append({pathGlob(host, folder + QStringLiteral("/*")),
                          QStringLiteral("Everything under %1/").arg(folder),
                          QStringLiteral("Every path below %1/ on %2, and %1 "
                                         "itself.")
                              .arg(folder, host)});
    }
    if (catalogIsPath && !catalogWildHost)
        hostPaths.append({catalogRule, catalogScope, catalogExplanation});
    offerSorted(hostPaths);

    const int port = normalizedPort(url);
    if (url.port() > 0)
        offer(firewallRuleForUrl(url, true), QStringLiteral("This host and port"),
              QStringLiteral("Any path on %1, only on port %2.").arg(host).arg(port));
    offer(firewallRuleForUrl(url), QStringLiteral("This host"),
          QStringLiteral("Any path on %1, on any port or scheme. Other hosts on "
                         "the same domain still ask.")
              .arg(host));

    // The parent domains, deepest first, ending at the registrable domain.
    const QString registrable = suggestedFirewallWildcard(url);
    if (!registrable.isEmpty()) {
        const QString domain = registrable.mid(2);
        QList<FirewallRuleSuggestion> wildPaths;
        if (segments.size() >= 2)
            wildPaths.append(
                {pathGlob(registrable, QLatin1Char('/') + segments.first() +
                                           QStringLiteral("/*")),
                 QStringLiteral("/%1/ on every %2 host").arg(segments.first(), domain),
                 QStringLiteral("Every path below /%1/ on %2 and all of its "
                                "subdomains; other paths still ask.")
                     .arg(segments.first(), domain)});
        if (catalogWildHost)
            wildPaths.append({catalogRule, catalogScope, catalogExplanation});
        offerSorted(wildPaths);

        const QStringList labels = host.split(QLatin1Char('.'), Qt::SkipEmptyParts);
        const qsizetype keep = domain.count(QLatin1Char('.')) + 1;
        // A bare domain (forkmesh.com) is its own registrable domain.
        for (qsizetype drop = qMin<qsizetype>(1, labels.size() - keep);
             labels.size() - drop >= keep; ++drop) {
            const QString parent = labels.mid(drop).join(QLatin1Char('.'));
            const bool whole = labels.size() - drop == keep;
            offer(QStringLiteral("*.") + parent,
                  whole ? QStringLiteral("The whole %1 domain").arg(parent)
                        : QStringLiteral("Every host under %1").arg(parent),
                  QStringLiteral("%1 and every subdomain of it, any path.%2")
                      .arg(parent,
                           whole ? QStringLiteral(" Broad: any service on this "
                                                  "domain gets the same answer.")
                                 : QString()));
        }
    }
    if (!catalogIsPath && !catalogRule.isEmpty())
        offer(catalogRule, catalogScope, catalogExplanation);

    const QString scheme = url.scheme().toLower();
    if (!scheme.isEmpty())
        offer(QStringLiteral("scheme:") + scheme,
              QStringLiteral("Every %1 request").arg(scheme.toUpper()),
              QStringLiteral("Any %1 destination at all. Very broad: allowing it "
                             "stops the firewall asking about %1.")
                  .arg(scheme.toUpper()));

    // A catalog pattern equal to a suggestion already offered marks that one.
    for (FirewallRuleSuggestion &suggestion : suggestions) {
        if (!catalogRule.isEmpty() && suggestion.rule == catalogRule &&
            suggestion.scope != catalogScope)
            suggestion.explanation +=
                QStringLiteral(" This is the endpoint catalog's pattern for this call.");
    }
    return suggestions;
}

QString BackoffNetworkAccessManager::endpointStatsUrl(const QUrl &url)
{
    QUrl grouped = url;
    grouped.setQuery(QString());
    grouped.setFragment(QString());
    return endpointFullUrl(grouped);
}

QString BackoffNetworkAccessManager::endpointStatsKey(Operation op, const QUrl &url)
{
    Q_UNUSED(op);
    return endpointStatsUrl(url);
}

BackoffNetworkAccessManager::EndpointStats &
BackoffNetworkAccessManager::endpointStatsFor(Operation op, const QUrl &url)
{
    EndpointStats &stats = m_endpointStats[endpointStatsKey(op, url)];
    const QString method = firewallMethodName(op);
    if (stats.method.isEmpty()) {
        stats.method = method;
    } else {
        const QStringList methods =
            stats.method.split(QStringLiteral(", "), Qt::SkipEmptyParts);
        if (!methods.contains(method))
            stats.method += QStringLiteral(", ") + method;
    }
    stats.endpoint = endpointStatsUrl(url);
    return stats;
}

int BackoffNetworkAccessManager::noteEndpointAttempt(Operation op,
                                                     const QUrl &url,
                                                     const QString &requestData)
{
    EndpointStats &stats = endpointStatsFor(op, url);
    ++stats.calls;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (stats.firstCalledMs <= 0)
        stats.firstCalledMs = nowMs;
    stats.lastFullUrl = endpointFullUrl(url);
    stats.lastRequestData = requestData.isEmpty() ? QStringLiteral("-")
                                                  : requestData;
    stats.lastResponseData = QStringLiteral("Pending");
    stats.lastCalledMs = nowMs;

    RequestRecord record;
    record.method = firewallMethodName(op);
    record.url = stats.lastFullUrl;
    record.requestData = stats.lastRequestData;
    record.responseData = stats.lastResponseData;
    record.startedMs = nowMs;
    stats.requests.append(record);
    emit endpointStatsChanged();
    return stats.requests.size() - 1;
}

void BackoffNetworkAccessManager::noteEndpointAllowed(Operation op, const QUrl &url)
{
    EndpointStats &stats = endpointStatsFor(op, url);
    ++stats.allowed;
    stats.firewallDecision = QStringLiteral("Allowed");
    stats.lastCalledMs = QDateTime::currentMSecsSinceEpoch();
    emit endpointStatsChanged();
}

void BackoffNetworkAccessManager::noteEndpointBlocked(Operation op, const QUrl &url,
                                                      const QString &reason,
                                                      int requestIndex)
{
    EndpointStats &stats = endpointStatsFor(op, url);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    ++stats.blocked;
    ++stats.failures;
    stats.firewallDecision = QStringLiteral("Blocked");
    stats.lastError = reason;
    stats.lastResponseData = reason;
    stats.lastStatus = 0;
    stats.lastCalledMs = nowMs;
    if (requestIndex >= 0 && requestIndex < stats.requests.size()) {
        RequestRecord &record = stats.requests[requestIndex];
        record.responseData = reason;
        record.error = reason;
        record.blocked = true;
        record.finishedMs = nowMs;
    }
    emit endpointStatsChanged();
}

void BackoffNetworkAccessManager::noteEndpointFinished(Operation op, const QUrl &url,
                                                       qint64 uploaded,
                                                       qint64 downloaded,
                                                       int status,
                                                       const QString &error,
                                                       int requestIndex,
                                                       const QString &responseData)
{
    EndpointStats &stats = endpointStatsFor(op, url);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    stats.uploadBytes += qMax<qint64>(0, uploaded);
    stats.downloadBytes += qMax<qint64>(0, downloaded);
    stats.lastStatus = status;
    stats.lastError = error;
    stats.lastResponseData = responseData.isEmpty()
                                 ? (error.isEmpty() ? QStringLiteral("-")
                                                    : error)
                                 : responseData;
    stats.lastCalledMs = nowMs;
    if (status >= 400 || !error.isEmpty())
        ++stats.failures;
    if (requestIndex >= 0 && requestIndex < stats.requests.size()) {
        RequestRecord &record = stats.requests[requestIndex];
        record.uploadBytes = qMax<qint64>(0, uploaded);
        record.downloadBytes = qMax<qint64>(0, downloaded);
        record.status = status;
        record.error = error;
        record.responseData = stats.lastResponseData;
        record.finishedMs = nowMs;
    }
    emit endpointStatsChanged();
}

bool BackoffNetworkAccessManager::firewallRuleMatches(const QString &rule,
                                                      const QUrl &url)
{
    const QString scheme = url.scheme().toLower();
    const QString host = normalizedHost(url.host());
    const int port = normalizedPort(url);
    if (rule == QLatin1String("*") || rule == firewallRuleForUrl(url) ||
        rule == firewallRuleForExactUrl(url))
        return true;
    if (rule == QStringLiteral("scheme:") + scheme)
        return true;
    if (rule.startsWith(QStringLiteral("urlglob:"))) {
        UrlGlob glob;
        if (!parseUrlGlob(rule.mid(8), &glob))
            return false;
        if (glob.scheme != QLatin1String("*") && glob.scheme != scheme)
            return false;
        if (glob.port > 0 && glob.port != port)
            return false;
        if (glob.host.startsWith(QLatin1String("*."))) {
            const QString suffix = glob.host.mid(2);
            if (host != suffix && !host.endsWith(QLatin1Char('.') + suffix))
                return false;
        } else if (host != glob.host) {
            return false;
        }
        const QString path = url.path().isEmpty() ? QStringLiteral("/") : url.path();
        // "dir/*" also covers "dir" itself.
        return wildcardMatch(glob.path, path) ||
               (glob.path.endsWith(QLatin1String("/*")) && path == glob.path.chopped(2));
    }
    if (rule.startsWith(QStringLiteral("hostwild:"))) {
        const QString suffix = rule.mid(9);
        if (host == suffix || host.endsWith(QStringLiteral(".") + suffix))
            return true;
    }
    if (rule.startsWith(QStringLiteral("hostport:")) && port > 0) {
        const QString rest = rule.mid(9);
        const int colon = rest.lastIndexOf(QLatin1Char(':'));
        if (colon > 0 && rest.left(colon) == host &&
            rest.mid(colon + 1).toInt() == port)
            return true;
    }
    return false;
}

BackoffNetworkAccessManager::FirewallMatch
BackoffNetworkAccessManager::firewallMatch(const QUrl &url) const
{
    return firewallDecision(url, nullptr);
}

BackoffNetworkAccessManager::FirewallMatch
BackoffNetworkAccessManager::firewallDecision(const QUrl &url,
                                              QString *ruleOut) const
{
    if (ruleOut)
        ruleOut->clear();
    // Nothing that stays on this machine is an external destination: data:,
    // file: and qrc: URLs never reach the wire, and loopback is the local
    // relay, the IDE bridge and the agent transports.
    if (!url.isValid() || isOnThisMachine(url))
        return FirewallAllowed;
    // The most specific matching rule decides; a tie goes to deny.
    QString denyRule;
    qint64 bestDeny = -1;
    for (const QString &rule : m_firewallDenyRules) {
        if (!firewallRuleMatches(rule, url))
            continue;
        const qint64 specificity = firewallRuleSpecificity(rule);
        if (specificity > bestDeny) {
            bestDeny = specificity;
            denyRule = rule;
        }
    }
    QString allowRule;
    qint64 bestAllow = -1;
    for (const QString &rule : m_firewallRules) {
        if (!firewallRuleMatches(rule, url))
            continue;
        const qint64 specificity = firewallRuleSpecificity(rule);
        if (specificity > bestAllow) {
            bestAllow = specificity;
            allowRule = rule;
        }
    }
    if (bestDeny < 0 && bestAllow < 0)
        return FirewallUnlisted;
    const bool denied = bestDeny >= bestAllow;
    if (ruleOut)
        *ruleOut = denied ? denyRule : allowRule;
    return denied ? FirewallDenied : FirewallAllowed;
}

BackoffNetworkAccessManager::FirewallMatch
BackoffNetworkAccessManager::recordFirewallMatch(const QUrl &url)
{
    QString rule;
    const FirewallMatch match = firewallDecision(url, &rule);
    if (!rule.isEmpty()) {
        FirewallRuleHits &hits = m_firewallRuleHits[rule];
        ++hits.requests;
        if (match == FirewallDenied)
            ++hits.blocked;
        emit firewallRuleHitsChanged();
    }
    return match;
}

bool BackoffNetworkAccessManager::firewallAllows(const QUrl &url) const
{
    return !m_firewallEnabled || firewallMatch(url) == FirewallAllowed;
}

QNetworkReply *BackoffNetworkAccessManager::createRequest(
    Operation op, const QNetworkRequest &request, QIODevice *outgoingData)
{
    const QUrl url = request.url();
    const bool privatePayload =
        forkmesh::networkRequestPayloadIsPrivate(request);
    const QString requestData =
        privatePayload ? privateRequestPreview(outgoingData)
                       : requestDataPreview(op, url, outgoingData);
    const int requestIndex = noteEndpointAttempt(op, url, requestData);
    if (m_clientVersionBlocked && !isOnThisMachine(url)) {
        // Ahead of offline mode, the firewall and the backoff: a retired build
        // has nothing to say to the network whatever the other switches read,
        // and the reachability probe is no exception -- there is no state it
        // could discover that would let this build back out.
        noteEndpointBlocked(op, url, QStringLiteral("Update required"),
                            requestIndex);
        emit outdatedClientBlockedRequest(firewallMethodName(op), url);
        return new DeniedReply(
            request, op,
            forkmesh::outdatedClientSummary(m_clientVersion,
                                            m_minimumNetworkVersion),
            this, DeniedReply::OutdatedClient);
    }
    if (m_offlineMode && !isOnThisMachine(url) &&
        !isReachabilityProbe(request)) {
        noteEndpointBlocked(op, url, QStringLiteral("Offline mode"),
                            requestIndex);
        return new DeniedReply(request, op, QStringLiteral("offline mode"),
                               this, DeniedReply::Offline);
    }
    if (m_offMachineBlocked && !isOnThisMachine(url)) {
        noteEndpointBlocked(op, url, QStringLiteral("Off-machine blocked"),
                            requestIndex);
        return new DeniedReply(request, op, QStringLiteral("test build"), this);
    }
    const FirewallMatch match =
        m_firewallEnabled ? recordFirewallMatch(url) : FirewallAllowed;
    if (match == FirewallDenied) {
        // Already decided: a remembered denial blocks without asking again.
        noteEndpointBlocked(op, url, QStringLiteral("Firewall denied"),
                            requestIndex);
        return new DeniedReply(request, op, QStringLiteral("firewall"), this);
    }
    if (match == FirewallUnlisted) {
        // No rule yet, so the request waits for a decision. With no prompt
        // installed there is nobody to ask, and unlisted means blocked.
        QString rule = firewallRuleForUrl(url);
        const QString method = firewallMethodName(op);
        // A favicon is decoration with a letter mark to fall back on, fetched
        // for whatever host a log line names -- every Mastodon instance that
        // hits the relay, for one. Asking about it put a modal prompt inside
        // MainWindow's constructor (the footer paints log lines while the
        // window is built), so on 2026-09-21 the window never appeared until
        // a "wants to connect" dialog was answered. Skip it this time instead;
        // nothing is remembered, so a rule added later still applies.
        const bool decoration =
            request.attribute(forkmesh::kFaviconProbeAttribute).toBool();
        const FirewallVerdict verdict =
            m_firewallPrompt && !decoration
                ? m_firewallPrompt(method, url, &rule)
                : FirewallDenyOnce;
        const QString canonical = canonicalFirewallRule(rule);
        if (!canonical.isEmpty()) {
            if (verdict == FirewallAllow)
                addFirewallRule(canonical);
            else if (verdict == FirewallDeny)
                addFirewallDenyRule(canonical);
        }
        const bool allowed = verdict == FirewallAllow;
        if (!decoration)
            emit firewallRequestDecided(method, url, canonical, allowed);
        if (!allowed) {
            noteEndpointBlocked(op, url, QStringLiteral("Firewall blocked"),
                                requestIndex);
            return new DeniedReply(request, op, QStringLiteral("firewall"),
                                   this);
        }
    }
    noteEndpointAllowed(op, url);

    auto createTrackedRequest = [&]() {
        const qint64 outgoingKnown =
            (outgoingData && !outgoingData->isSequential())
                ? qMax<qint64>(0, outgoingData->size())
                : 0;
        QNetworkReply *reply = createNetworkRequest(op, request, outgoingData);
        auto *tracker = new EndpointTransferTracker(reply);
        tracker->uploaded = outgoingKnown;
        const quint64 activity = forkmesh::BackgroundActivity::begin(
            QStringLiteral("net"),
            firewallMethodName(op) + QLatin1Char(' ') + endpointStatsUrl(url));
        connect(reply, &QNetworkReply::finished, this, [reply, activity] {
            forkmesh::BackgroundActivity::end(
                activity,
                reply->error() == QNetworkReply::NoError
                    ? QStringLiteral("succeeded")
                    : QStringLiteral("failed"));
        });
        connect(reply, &QObject::destroyed, this,
                [activity] {
                    forkmesh::BackgroundActivity::end(
                        activity, QStringLiteral("cancelled"));
                });
        connect(reply, &QNetworkReply::uploadProgress, this,
                [tracker](qint64 sent, qint64) {
                    tracker->uploaded = qMax(tracker->uploaded, sent);
                });
        connect(reply, &QNetworkReply::downloadProgress, this,
                [tracker](qint64 received, qint64) {
                    tracker->downloaded = qMax(tracker->downloaded, received);
                });
        connect(reply, &QNetworkReply::finished, this,
                [this, reply, tracker, op, url, requestIndex,
                 privatePayload] {
                    const QVariant code =
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
                    const int status = code.isValid() ? code.toInt() : 0;
                    const QString error =
                        reply->error() == QNetworkReply::NoError
                            ? QString()
                            : forkmesh::networkReplyFailureSummary(reply);
                    const qint64 downloaded =
                        qMax(tracker->downloaded, reply->bytesAvailable());
                    noteEndpointFinished(
                        op, url, tracker->uploaded, downloaded, status, error,
                        requestIndex,
                        privatePayload
                            ? QStringLiteral("<private response body: %1 bytes>")
                                  .arg(downloaded)
                            : responseDataPreview(reply, downloaded));
                });
        return reply;
    };

    // A Cloudflare 429 applies to the host, not just to the API route that
    // happened to exhaust the budget.  In particular, the desktop edge probe
    // also requests "/"; allowing that request through during an API cooldown
    // kept touching the rate-limited zone once a minute and made the shared
    // gate look (and behave) as though it were not backing off.
    const QString channel = normalizedHost(url.host());
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!channel.isEmpty() && !m_backoff.ready(channel, nowMs)) {
        noteEndpointFinished(op, url, 0, 0, 0,
                             QStringLiteral("Rate-limit backoff suppressed"),
                             requestIndex,
                             QStringLiteral("Rate-limit backoff suppressed"));
        return new BackoffSuppressedReply(request, op, this,
                                          m_backoff.lastFailureStatus(channel));
    }

    const bool apiRequest = url.path().startsWith(QStringLiteral("/api/"));
    const QString lastSeg = url.path().section(QLatin1Char('/'), -1);
    static const QSet<QString> kTunnelOps = {
        QStringLiteral("tree"), QStringLiteral("blob"),
        QStringLiteral("blobs"), QStringLiteral("raw"),
        QStringLiteral("history"), QStringLiteral("commit"),
        QStringLiteral("branches"), QStringLiteral("search")};
    const bool tunnelContent =
        kTunnelOps.contains(lastSeg)
        || url.path().contains(QStringLiteral("/releases/blob/"));

    QNetworkReply *reply = createTrackedRequest();
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, channel, apiRequest, tunnelContent] {
        const int code =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        // 429 is zone/host-wide even when a page route returned it.  Preserve
        // the narrower API-only handling for transient 5xx responses: a broken
        // website asset must not stall otherwise healthy relay operations.
        //
        // A 5xx the Worker marked expected-degraded is narrower still: it is a
        // deliberate answer about that one route, and it carries its own
        // Retry-After.  Deleting a node, for instance, fail-closes the public
        // catalog snapshots for a couple of minutes, so /api/repositories and
        // /api/repo/forkmesh/forkmesh/mirrors answer 503 by design -- and that
        // used to open a host-wide cooldown that silenced relay sync, chat and
        // pings along with them, doubling with every poll that met the same
        // barrier.  Leave the host gate alone: the route's own caller backs
        // off, and an unrelated request has no reason to be suppressed.
        const bool expectedDegraded =
            forkmesh::networkReplyIsExpectedDegraded(reply);
        const bool overloaded =
            (code == 429 && !expectedDegraded) ||
            (apiRequest && code >= 500 && code <= 599 && !tunnelContent &&
             !expectedDegraded);
        if (overloaded) {
            const QByteArray retryAfter = reply->rawHeader("Retry-After");
            bool retryAfterOk = false;
            const qint64 retryAfterMs =
                QString::fromLatin1(retryAfter).trimmed().toLongLong(&retryAfterOk) * 1000;
            const qint64 hintMs =
                retryAfterOk && retryAfterMs > 0 ? retryAfterMs : 0;
            const qint64 failedMs = QDateTime::currentMSecsSinceEpoch();
            if (m_backoff.ready(channel, failedMs) ||
                hintMs > m_backoff.msUntilReady(channel, failedMs))
                m_backoff.noteFailure(channel, failedMs, kBaseMs, kCapMs,
                                      hintMs, code);
        } else if (reply->error() == QNetworkReply::NoError) {
            const qint64 finishedMs = QDateTime::currentMSecsSinceEpoch();
            // Several requests may already be in flight when the first 429
            // arrives.  A late success from that older batch must not erase
            // the cooldown the 429 just established.  A successful probe
            // sent after the cooldown expires still resets the failure run.
            if (m_backoff.ready(channel, finishedMs))
                m_backoff.noteSuccess(channel);
        }
    });
    return reply;
}
