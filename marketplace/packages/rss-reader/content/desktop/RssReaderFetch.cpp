#include "RssReaderFetch.h"

#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QTimer>

#include <utility>

namespace forkmesh::rssreader {
namespace {

constexpr int kMaxRedirects = 5;
constexpr int kTransferTimeoutMs = 30000;

QByteArray feedAccept()
{
    return QByteArrayLiteral(
        "application/rss+xml, application/atom+xml, application/feed+json, "
        "application/json;q=0.9, application/rdf+xml;q=0.9, application/xml;q=0.9, "
        "text/xml;q=0.9, text/html;q=0.5, */*;q=0.1");
}

QString sizeText(qint64 bytes)
{
    return QLocale(QLocale::English).formattedDataSize(bytes, 0);
}

struct Discovery {
    FetchQueue *queue = nullptr;
    QUrl address;
    bool local = false;
    QList<QUrl> candidates;
    bool triedFallbacks = false;
    FetchResult first;
    std::function<void(const DiscoveryResult &)> done;
};

void tryNext(const std::shared_ptr<Discovery> &state);

void tryCandidates(const std::shared_ptr<Discovery> &state, const QList<QUrl> &candidates)
{
    state->candidates = candidates;
    tryNext(state);
}

void handleFirst(const std::shared_ptr<Discovery> &state, const FetchResult &fetched);

void fetchAddress(const std::shared_ptr<Discovery> &state)
{
    FetchRequest request;
    request.url = state->address;
    request.allowFile = state->local;
    state->queue->get(request, [state](const FetchResult &fetched) { handleFirst(state, fetched); });
}

void tryPreferred(const std::shared_ptr<Discovery> &state, const QUrl &feed,
                  std::function<void()> otherwise)
{
    FetchRequest request;
    request.url = feed;
    state->queue->get(request, [state, feed, otherwise](const FetchResult &fetched) {
        if (fetched.ok() && !fetched.notModified) {
            const ParsedFeed parsed = parseFeed(fetched.body, fetched.finalUrl);
            if (parsed.ok) {
                DiscoveryResult result;
                result.ok = true;
                result.feed = parsed;
                result.fetch = fetched;
                result.feedUrl = fetched.permanentRedirect ? fetched.finalUrl : feed;
                state->done(result);
                return;
            }
        }
        otherwise();
    });
}

void tryNext(const std::shared_ptr<Discovery> &state)
{
    if (state->candidates.isEmpty()) {
        if (!state->triedFallbacks) {
            state->triedFallbacks = true;
            QList<QUrl> fallbacks;
            for (const QUrl &candidate : fallbackFeedUrls(state->address)) {
                if (feedKey(candidate) != feedKey(state->address))
                    fallbacks.append(candidate);
            }
            if (!fallbacks.isEmpty()) {
                tryCandidates(state, fallbacks);
                return;
            }
        }
        DiscoveryResult result;
        result.fetch = state->first;
        result.error = QStringLiteral("No feed was found at %1.").arg(state->address.toString());
        state->done(result);
        return;
    }
    const QUrl candidate = state->candidates.takeFirst();
    FetchRequest request;
    request.url = candidate;
    request.allowFile = state->local;
    state->queue->get(request, [state, candidate](const FetchResult &fetched) {
        if (fetched.ok() && !fetched.notModified) {
            const ParsedFeed parsed = parseFeed(fetched.body, fetched.finalUrl);
            if (parsed.ok) {
                DiscoveryResult result;
                result.ok = true;
                result.feed = parsed;
                result.fetch = fetched;
                result.feedUrl = fetched.permanentRedirect ? fetched.finalUrl : candidate;
                state->done(result);
                return;
            }
        }
        tryNext(state);
    });
}

void handleFirst(const std::shared_ptr<Discovery> &state, const FetchResult &fetched)
{
    state->first = fetched;
    if (!fetched.ok() && fetched.status == 0) {
        DiscoveryResult result;
        result.fetch = fetched;
        result.error = fetched.error;
        state->done(result);
        return;
    }
    if (fetched.ok() && !fetched.notModified) {
        const ParsedFeed parsed = parseFeed(fetched.body, fetched.finalUrl);
        if (parsed.ok) {
            DiscoveryResult result;
            result.ok = true;
            result.feed = parsed;
            result.fetch = fetched;
            result.feedUrl = fetched.permanentRedirect ? fetched.finalUrl : state->address;
            state->done(result);
            return;
        }
        if (!state->local && looksLikeHtml(fetched.body, fetched.contentType)) {
            const QList<QUrl> links = discoverFeedLinks(QString::fromUtf8(fetched.body), fetched.finalUrl);
            if (!links.isEmpty()) {
                tryCandidates(state, links);
                return;
            }
        }
    }
    tryNext(state);
}

}

bool FetchResult::ok() const
{
    return error.isEmpty() && (notModified || (status >= 200 && status < 300));
}

QByteArray userAgent()
{
    return QByteArrayLiteral("ForkMesh-RSS-Reader/1.0 (+https://forkmesh.com)");
}

FetchQueue::FetchQueue(QNetworkAccessManager *network, int maxConcurrent)
    : m_network(network), m_guard(new QObject), m_maxConcurrent(qMax(1, maxConcurrent))
{
}

FetchQueue::~FetchQueue()
{
    delete m_guard;
    m_guard = nullptr;
    m_waiting.clear();
    const QList<QPointer<QNetworkReply>> replies = m_replies;
    m_replies.clear();
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply) {
            reply->disconnect();
            reply->abort();
            reply->deleteLater();
        }
    }
}

void FetchQueue::abortAll()
{
    m_waiting.clear();
    const QList<QPointer<QNetworkReply>> replies = m_replies;
    for (const QPointer<QNetworkReply> &reply : replies) {
        if (reply)
            reply->abort();
    }
}

void FetchQueue::get(const FetchRequest &request, Done done)
{
    auto job = std::make_shared<Job>();
    job->request = request;
    job->done = std::move(done);
    job->current = request.url;
    m_waiting.append(job);
    pump();
}

void FetchQueue::pump()
{
    while (m_active < m_maxConcurrent && !m_waiting.isEmpty()) {
        const std::shared_ptr<Job> job = m_waiting.takeFirst();
        ++m_active;
        start(job);
    }
}

void FetchQueue::finish(const std::shared_ptr<Job> &job, const FetchResult &result)
{
    --m_active;
    const Done done = job->done;
    if (done)
        done(result);
    pump();
}

void FetchQueue::readFile(const std::shared_ptr<Job> &job)
{
    FetchResult result;
    result.requestedUrl = job->request.url;
    result.finalUrl = job->current;
    const QString path = job->current.toLocalFile();
    QFile file(path);
    if (!QFileInfo(path).isFile() || !file.open(QIODevice::ReadOnly)) {
        result.error = QStringLiteral("The file %1 could not be read.").arg(path);
    } else if (file.size() > job->request.maxBytes) {
        result.error = QStringLiteral("The response is larger than %1.").arg(sizeText(job->request.maxBytes));
    } else {
        result.body = file.read(job->request.maxBytes + 1);
        if (result.body.size() > job->request.maxBytes) {
            result.body.clear();
            result.error = QStringLiteral("The response is larger than %1.").arg(sizeText(job->request.maxBytes));
        } else {
            result.status = 200;
        }
    }
    finish(job, result);
}

void FetchQueue::start(const std::shared_ptr<Job> &job)
{
    FetchResult failed;
    failed.requestedUrl = job->request.url;
    failed.finalUrl = job->current;
    const QString scheme = job->current.scheme().toLower();
    if (scheme == QLatin1String("file") && job->request.allowFile && job->current.isLocalFile()) {
        QTimer::singleShot(0, m_guard, [this, job] { readFile(job); });
        return;
    }
    if (!m_network) {
        failed.error = QStringLiteral("The desktop network is not available.");
        finish(job, failed);
        return;
    }
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")) {
        failed.error = QStringLiteral("Only http and https addresses can be fetched.");
        finish(job, failed);
        return;
    }
    QNetworkRequest request(job->current);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::ManualRedirectPolicy);
    request.setTransferTimeout(kTransferTimeoutMs);
    request.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    request.setRawHeader("Accept", job->request.accept.isEmpty() ? feedAccept() : job->request.accept);
    if (job->hops == 0 && !job->request.etag.isEmpty())
        request.setRawHeader("If-None-Match", job->request.etag.toUtf8());
    if (job->hops == 0 && !job->request.lastModified.isEmpty())
        request.setRawHeader("If-Modified-Since", job->request.lastModified.toUtf8());
    QNetworkReply *reply = m_network->get(request);
    m_replies.append(reply);
    const qint64 limit = job->request.maxBytes;
    auto tooLarge = std::make_shared<bool>(false);
    QObject::connect(reply, &QNetworkReply::downloadProgress, m_guard,
                     [reply, limit, tooLarge](qint64 received, qint64) {
        if (received > limit && !*tooLarge) {
            *tooLarge = true;
            reply->abort();
        }
    });
    QObject::connect(reply, &QNetworkReply::finished, m_guard, [this, job, reply, limit, tooLarge] {
        m_replies.removeAll(QPointer<QNetworkReply>(reply));
        reply->deleteLater();
        FetchResult result;
        result.requestedUrl = job->request.url;
        result.finalUrl = job->current;
        result.permanentRedirect = job->hops > 0 && job->permanent;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        result.etag = QString::fromUtf8(reply->rawHeader("ETag")).trimmed();
        result.lastModified = QString::fromUtf8(reply->rawHeader("Last-Modified")).trimmed();
        if (*tooLarge) {
            result.error = QStringLiteral("The response is larger than %1.").arg(sizeText(limit));
            finish(job, result);
            return;
        }
        const int status = result.status;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            const QByteArray location = reply->rawHeader("Location");
            const QUrl next = job->current.resolved(QUrl(QString::fromUtf8(location)));
            const QString nextScheme = next.scheme().toLower();
            if (location.isEmpty() || !next.isValid()) {
                result.error = QStringLiteral("The server sent a redirect without an address.");
            } else if (job->hops >= kMaxRedirects) {
                result.error = QStringLiteral("Too many redirects.");
            } else if (nextScheme != QLatin1String("https") &&
                       !(nextScheme == QLatin1String("http") &&
                         job->current.scheme().toLower() == QLatin1String("http"))) {
                result.error = QStringLiteral("The server redirected to a less safe address (%1).")
                                   .arg(next.toString());
            } else {
                ++job->hops;
                job->permanent = job->permanent && (status == 301 || status == 308);
                job->current = next;
                start(job);
                return;
            }
            finish(job, result);
            return;
        }
        if (status == 304) {
            result.notModified = true;
            finish(job, result);
            return;
        }
        if (status == 0 || reply->error() != QNetworkReply::NoError) {
            if (status >= 400)
                result.error = QStringLiteral("The server answered HTTP %1.").arg(status);
            else
                result.error = reply->errorString().isEmpty()
                                   ? QStringLiteral("The request failed.")
                                   : reply->errorString();
            if (status >= 400 || status == 0) {
                finish(job, result);
                return;
            }
        }
        if (status < 200 || status >= 300) {
            result.error = QStringLiteral("The server answered HTTP %1.").arg(status);
            finish(job, result);
            return;
        }
        result.body = reply->readAll();
        if (result.body.size() > limit) {
            result.body.clear();
            result.error = QStringLiteral("The response is larger than %1.").arg(sizeText(limit));
        }
        finish(job, result);
    });
}

void discoverFeed(FetchQueue *queue, const QUrl &address,
                  std::function<void(const DiscoveryResult &)> done)
{
    auto state = std::make_shared<Discovery>();
    state->queue = queue;
    state->address = address;
    state->local = address.isLocalFile();
    state->done = std::move(done);
    if (state->local) {
        fetchAddress(state);
        return;
    }
    const DiscoveryRewrite rewrite = discoveryRewrite(address);
    if (rewrite.feed.isValid() && feedKey(rewrite.feed) != feedKey(address)) {
        tryPreferred(state, rewrite.feed, [state] { fetchAddress(state); });
        return;
    }
    if (rewrite.channelLookup) {
        FetchRequest request;
        request.url = address;
        request.accept = QByteArrayLiteral("text/html,application/xhtml+xml;q=0.9,*/*;q=0.5");
        queue->get(request, [state](const FetchResult &page) {
            const QString channel = page.ok() && !page.notModified
                                        ? youtubeChannelId(QString::fromUtf8(page.body))
                                        : QString();
            if (channel.isEmpty()) {
                handleFirst(state, page);
                return;
            }
            tryPreferred(state, youtubeChannelFeed(channel), [state, page] { handleFirst(state, page); });
        });
        return;
    }
    fetchAddress(state);
}

}
