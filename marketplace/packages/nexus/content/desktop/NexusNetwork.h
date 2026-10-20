#pragma once

#include "NexusCore.h"

#include <QElapsedTimer>
#include <QHostAddress>
#include <QList>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

class QNetworkAccessManager;
class QNetworkReply;
class QObject;
class QTcpSocket;
class QTimer;
class QUdpSocket;

namespace forkmesh::nexus {

struct ScanOptions {
    int timeoutMs = 1500;
    int concurrency = 48;
    int bannerMs = 700;
};

class TcpScanner
{
public:
    using ResultHandler = std::function<void(const PortResult &)>;
    using ProgressHandler = std::function<void(int completed, int total)>;
    using FinishedHandler = std::function<void(bool cancelled)>;

    explicit TcpScanner(QObject *context);
    ~TcpScanner();

    void start(const QList<QHostAddress> &addresses, const QList<quint16> &ports, const ScanOptions &options,
               ResultHandler onResult, ProgressHandler onProgress, FinishedHandler onFinished);
    void cancel();
    bool running() const { return m_running; }

private:
    struct Probe {
        QHostAddress address;
        quint16 port = 0;
    };

    void launchMore();
    void launch(const Probe &probe);
    void complete(QTcpSocket *socket, const PortResult &result);

    QObject *m_context = nullptr;
    QList<Probe> m_queue;
    int m_next = 0;
    int m_active = 0;
    int m_completed = 0;
    bool m_running = false;
    quint64 m_generation = 0;
    ScanOptions m_options;
    ResultHandler m_onResult;
    ProgressHandler m_onProgress;
    FinishedHandler m_onFinished;
    QList<QPointer<QTcpSocket>> m_sockets;
};

class MdnsBrowser
{
public:
    using UpdateHandler = std::function<void(const QList<DiscoveredDevice> &devices, int round, int rounds)>;
    using FinishedHandler = std::function<void(const QList<DiscoveredDevice> &devices, const QString &error)>;

    explicit MdnsBrowser(QObject *context);
    ~MdnsBrowser();

    void start(const QStringList &serviceTypes, int seconds, UpdateHandler onUpdate, FinishedHandler onFinished);
    void cancel();
    bool running() const { return m_running; }

private:
    void sendRound();
    void read();
    void finish(const QString &error);

    QObject *m_context = nullptr;
    QUdpSocket *m_socket = nullptr;
    QTimer *m_timer = nullptr;
    QStringList m_types;
    QList<DnsRecord> m_records;
    int m_round = 0;
    int m_rounds = 0;
    bool m_running = false;
    UpdateHandler m_onUpdate;
    FinishedHandler m_onFinished;
};

struct DnsAnswer {
    QString name;
    QString type;
    QString value;
    int ttl = 0;
};

struct DnsLookupResult {
    QString name;
    QString type;
    QList<DnsAnswer> answers;
    int elapsedMs = 0;
    QString error;
};

QStringList dnsRecordTypes();
void lookupDns(QObject *context, const QString &name, const QString &type,
               std::function<void(const DnsLookupResult &)> done);

struct CertificateSummary {
    QString subject;
    QString issuer;
    QDateTime notBefore;
    QDateTime notAfter;
};

struct HttpHop {
    QUrl url;
    int status = 0;
    QString reason;
    int elapsedMs = 0;
    QList<QPair<QByteArray, QByteArray>> headers;
};

struct HttpInspection {
    QList<HttpHop> hops;
    QString protocol;
    QString cipher;
    QList<CertificateSummary> chain;
    QStringList tlsErrors;
    QList<HeaderFinding> findings;
    QString error;
};

void inspectHttp(QNetworkAccessManager *network, QObject *context, const QUrl &url,
                 std::function<void(const HttpInspection &)> done);

}
