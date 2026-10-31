#pragma once

#include <QByteArray>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QUrlQuery>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;

namespace forkmesh::mastodon {

struct Result {
    bool ok = false;
    int status = 0;
    QJsonValue json;
    QString error;
    QUrl next;
    QUrl previous;

    QJsonObject object() const { return json.toObject(); }
    QJsonArray array() const;
};

using Handler = std::function<void(const Result &)>;
using ImageHandler = std::function<void(const QImage &)>;

struct UploadPart {
    QString name;
    QByteArray data;
    QString fileName;
    QString contentType;
};

class Client
{
public:
    explicit Client(QNetworkAccessManager *network);
    ~Client();

    void setAccount(const QString &instance, const QString &token);
    QString instance() const { return m_instance; }
    bool signedIn() const { return !m_instance.isEmpty() && !m_token.isEmpty(); }
    void cancelAll();
    void setUnauthorizedHandler(std::function<void()> handler) { m_unauthorized = std::move(handler); }
    void trustImageHost(const QString &host);

    void get(const QString &path, const QUrlQuery &query, Handler done);
    void getUrl(const QUrl &url, Handler done);
    void send(const QByteArray &method, const QString &path, const QJsonObject &body, Handler done);
    void post(const QString &path, Handler done) { send("POST", path, {}, std::move(done)); }
    void remove(const QString &path, Handler done) { send("DELETE", path, {}, std::move(done)); }
    void upload(const QByteArray &method, const QString &path, const QList<UploadPart> &parts, Handler done);

    void anonymous(const QByteArray &method, const QUrl &url, const QJsonObject &body, Handler done);
    void image(const QString &url, ImageHandler done);
    QImage cachedImage(const QString &url) const { return m_images.value(url); }

private:
    QNetworkRequest request(const QUrl &url, bool authorized) const;
    void track(QNetworkReply *reply, Handler done, bool authorized);
    QUrl apiUrl(const QString &path, const QUrlQuery &query = {}) const;

    QNetworkAccessManager *m_network = nullptr;
    QObject *m_context = nullptr;
    QList<QPointer<QObject>> m_retired;
    quint64 m_epoch = 0;
    QSet<QNetworkReply *> m_replies;
    QString m_instance;
    QString m_token;
    std::function<void()> m_unauthorized;
    QHash<QString, QImage> m_images;
    QHash<QString, QList<ImageHandler>> m_imageWaiters;
    QSet<QString> m_imageHosts;
    QSet<QString> m_imageFailures;
};

QString errorText(int status, const QJsonValue &json, const QString &transport);

}
