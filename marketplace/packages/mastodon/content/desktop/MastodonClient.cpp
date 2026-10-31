#include "MastodonClient.h"
#include "MastodonCore.h"

#include "NetworkReplyError.h"

#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QTimer>

namespace forkmesh::mastodon {

namespace {

constexpr int kMaxCachedImages = 800;
constexpr qint64 kMaxImageBytes = 12 * 1024 * 1024;

}

QJsonArray Result::array() const
{
    return json.toArray();
}

QString errorText(int status, const QJsonValue &json, const QString &transport)
{
    const QJsonObject object = json.toObject();
    QString message = object.value(QStringLiteral("error_description")).toString();
    if (message.isEmpty())
        message = object.value(QStringLiteral("error")).toString();
    if (!message.isEmpty())
        return message;
    if (status == 401)
        return QStringLiteral("Your Mastodon sign-in has expired or was revoked.");
    if (status == 403)
        return QStringLiteral("Mastodon refused that action for this account.");
    if (status == 404)
        return QStringLiteral("Mastodon could not find that.");
    if (status == 422)
        return QStringLiteral("Mastodon could not accept that.");
    if (status == 429)
        return QStringLiteral("Mastodon is limiting requests for a moment. Try again shortly.");
    if (status >= 500)
        return QStringLiteral("The Mastodon server had a problem (%1).").arg(status);
    if (!transport.isEmpty())
        return transport;
    return QStringLiteral("Mastodon did not answer.");
}

Client::Client(QNetworkAccessManager *network)
    : m_network(network), m_context(new QObject)
{
}

Client::~Client()
{
    cancelAll();
    for (const QPointer<QObject> &retired : std::as_const(m_retired))
        delete retired.data();
    delete m_context;
}

void Client::setAccount(const QString &instance, const QString &token)
{
    if (instance != m_instance || token != m_token)
        cancelAll();
    m_instance = instance;
    m_token = token;
    m_imageHosts.clear();
    if (!instance.isEmpty())
        m_imageHosts.insert(instanceHost(instance));
}

void Client::cancelAll()
{
    ++m_epoch;
    m_retired.removeIf([](const QPointer<QObject> &retired) { return retired.isNull(); });
    QObject *old = m_context;
    m_context = new QObject;
    m_retired.append(old);
    old->deleteLater();
    const QSet<QNetworkReply *> replies = m_replies;
    m_replies.clear();
    for (QNetworkReply *reply : replies) {
        reply->disconnect();
        reply->abort();
        reply->deleteLater();
    }
    m_imageWaiters.clear();
}

void Client::trustImageHost(const QString &host)
{
    if (!host.isEmpty())
        m_imageHosts.insert(host.toLower());
}

QUrl Client::apiUrl(const QString &path, const QUrlQuery &query) const
{
    QUrl url(m_instance + path);
    if (!query.isEmpty())
        url.setQuery(query);
    return url;
}

QNetworkRequest Client::request(const QUrl &url, bool authorized) const
{
    QNetworkRequest request(url);
    request.setRawHeader("Accept", "application/json");
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ForkMesh-Desktop-Mastodon/1.1"));
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(30000);
    if (authorized && !m_token.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + m_token.toUtf8());
    return request;
}

void Client::track(QNetworkReply *reply, Handler done, bool authorized)
{
    m_replies.insert(reply);
    const quint64 epoch = m_epoch;
    QObject::connect(reply, &QNetworkReply::finished, m_context, [this, reply, done = std::move(done), authorized, epoch] {
        if (epoch != m_epoch)
            return;
        m_replies.remove(reply);
        reply->deleteLater();
        Result result;
        result.status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();
        QJsonParseError parseError{};
        const QJsonDocument document = QJsonDocument::fromJson(body, &parseError);
        if (document.isArray())
            result.json = document.array();
        else if (document.isObject())
            result.json = document.object();
        result.ok = reply->error() == QNetworkReply::NoError && result.status >= 200 && result.status < 300;
        const QByteArray link = reply->rawHeader("Link");
        result.next = linkTarget(link, QStringLiteral("next"));
        result.previous = linkTarget(link, QStringLiteral("prev"));
        if (!result.ok) {
            const QString transport = reply->error() == QNetworkReply::OperationCanceledError
                                          ? QStringLiteral("The request to Mastodon timed out.")
                                          : reply->errorString();
            result.error = errorText(result.status, result.json, result.status ? QString() : transport);
        }
        if (result.status == 401 && authorized && m_unauthorized) {
            QTimer::singleShot(0, m_context, [this, epoch] {
                if (epoch == m_epoch && m_unauthorized)
                    m_unauthorized();
            });
        }
        if (done)
            done(result);
    });
}

void Client::get(const QString &path, const QUrlQuery &query, Handler done)
{
    getUrl(apiUrl(path, query), std::move(done));
}

void Client::getUrl(const QUrl &url, Handler done)
{
    if (!m_network || !signedIn() || QUrl(m_instance).host() != url.host()) {
        Result result;
        result.error = QStringLiteral("Not signed in to Mastodon.");
        QTimer::singleShot(0, m_context, [done, result] { if (done) done(result); });
        return;
    }
    track(m_network->get(request(url, true)), std::move(done), true);
}

void Client::send(const QByteArray &method, const QString &path, const QJsonObject &body, Handler done)
{
    if (!m_network || !signedIn()) {
        Result result;
        result.error = QStringLiteral("Not signed in to Mastodon.");
        QTimer::singleShot(0, m_context, [done, result] { if (done) done(result); });
        return;
    }
    QNetworkRequest req = request(apiUrl(path), true);
    QByteArray payload;
    if (method != "DELETE" || !body.isEmpty()) {
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    }
    track(m_network->sendCustomRequest(req, method, payload), std::move(done), true);
}

void Client::upload(const QByteArray &method, const QString &path, const QList<UploadPart> &parts, Handler done)
{
    if (!m_network || !signedIn()) {
        Result result;
        result.error = QStringLiteral("Not signed in to Mastodon.");
        QTimer::singleShot(0, m_context, [done, result] { if (done) done(result); });
        return;
    }
    auto *multi = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    for (const UploadPart &part : parts) {
        QHttpPart field;
        QString disposition = QStringLiteral("form-data; name=\"%1\"").arg(part.name);
        if (!part.fileName.isEmpty()) {
            QString safeName = part.fileName;
            safeName.remove(QLatin1Char('"')).remove(QLatin1Char('\r')).remove(QLatin1Char('\n'));
            disposition += QStringLiteral("; filename=\"%1\"").arg(safeName);
        }
        field.setHeader(QNetworkRequest::ContentDispositionHeader, disposition);
        if (!part.contentType.isEmpty())
            field.setHeader(QNetworkRequest::ContentTypeHeader, part.contentType);
        field.setBody(part.data);
        multi->append(field);
    }
    QNetworkReply *reply = m_network->sendCustomRequest(request(apiUrl(path), true), method, multi);
    multi->setParent(reply);
    track(reply, std::move(done), true);
}

void Client::anonymous(const QByteArray &method, const QUrl &url, const QJsonObject &body, Handler done)
{
    if (!m_network) {
        Result result;
        result.error = QStringLiteral("The network is not available.");
        QTimer::singleShot(0, m_context, [done, result] { if (done) done(result); });
        return;
    }
    QNetworkRequest req = request(url, false);
    QByteArray payload;
    if (method != "GET") {
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
        payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    }
    track(m_network->sendCustomRequest(req, method, payload), std::move(done), false);
}

void Client::image(const QString &url, ImageHandler done)
{
    const auto cached = m_images.constFind(url);
    if (cached != m_images.constEnd()) {
        if (done)
            done(cached.value());
        return;
    }
    const QUrl target(url);
    if (!m_network || target.scheme() != QLatin1String("https") || target.host().isEmpty() ||
        m_imageFailures.contains(url))
        return;
    auto waiting = m_imageWaiters.find(url);
    if (waiting != m_imageWaiters.end()) {
        if (done)
            waiting->append(std::move(done));
        return;
    }
    m_imageWaiters.insert(url, done ? QList<ImageHandler>{std::move(done)} : QList<ImageHandler>{});
    QNetworkRequest req(target);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ForkMesh-Desktop-Mastodon/1.1"));
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setTransferTimeout(30000);
    if (!m_imageHosts.contains(target.host().toLower()))
        markFaviconProbe(req);
    QNetworkReply *reply = m_network->get(req);
    m_replies.insert(reply);
    QObject::connect(reply, &QNetworkReply::downloadProgress, m_context, [reply](qint64 received, qint64) {
        if (received > kMaxImageBytes)
            reply->abort();
    });
    const quint64 epoch = m_epoch;
    QObject::connect(reply, &QNetworkReply::finished, m_context, [this, reply, url, epoch] {
        if (epoch != m_epoch)
            return;
        m_replies.remove(reply);
        reply->deleteLater();
        QImage image;
        if (reply->error() == QNetworkReply::NoError)
            image.loadFromData(reply->readAll());
        const QList<ImageHandler> waiters = m_imageWaiters.take(url);
        if (image.isNull()) {
            m_imageFailures.insert(url);
            return;
        }
        if (image.width() > 1600 || image.height() > 1600)
            image = image.scaled(1600, 1600, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (m_images.size() >= kMaxCachedImages)
            m_images.clear();
        m_images.insert(url, image);
        for (const ImageHandler &waiter : waiters) {
            if (waiter)
                waiter(image);
        }
    });
}

}
