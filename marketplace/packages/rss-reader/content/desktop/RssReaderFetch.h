#pragma once

#include "RssReaderModel.h"

#include <QByteArray>
#include <QList>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

class QObject;

namespace forkmesh::rssreader {

struct FetchRequest {
    QUrl url;
    QString etag;
    QString lastModified;
    qint64 maxBytes = kMaxFetchBytes;
    QByteArray accept;
    bool allowFile = false;
};

struct FetchResult {
    QUrl requestedUrl;
    QUrl finalUrl;
    bool permanentRedirect = false;
    int status = 0;
    bool notModified = false;
    QByteArray body;
    QString contentType;
    QString etag;
    QString lastModified;
    QString error;
    bool ok() const;
};

class FetchQueue
{
public:
    using Done = std::function<void(const FetchResult &)>;

    explicit FetchQueue(QNetworkAccessManager *network, int maxConcurrent = kMaxConcurrentFetches);
    ~FetchQueue();
    FetchQueue(const FetchQueue &) = delete;
    FetchQueue &operator=(const FetchQueue &) = delete;

    void get(const FetchRequest &request, Done done);
    int busy() const { return m_active + int(m_waiting.size()); }
    bool available() const { return m_network != nullptr; }
    void abortAll();

private:
    struct Job {
        FetchRequest request;
        Done done;
        QUrl current;
        int hops = 0;
        bool permanent = true;
    };
    void pump();
    void start(const std::shared_ptr<Job> &job);
    void readFile(const std::shared_ptr<Job> &job);
    void finish(const std::shared_ptr<Job> &job, const FetchResult &result);

    QPointer<QNetworkAccessManager> m_network;
    QObject *m_guard = nullptr;
    QList<std::shared_ptr<Job>> m_waiting;
    QList<QPointer<QNetworkReply>> m_replies;
    int m_active = 0;
    int m_maxConcurrent = kMaxConcurrentFetches;
};

struct DiscoveryResult {
    bool ok = false;
    QString error;
    QUrl feedUrl;
    ParsedFeed feed;
    FetchResult fetch;
};

void discoverFeed(FetchQueue *queue, const QUrl &address,
                  std::function<void(const DiscoveryResult &)> done);

QByteArray userAgent();

}
