#include "NexusNetwork.h"

#include <QDnsLookup>
#include <QHash>
#include <QNetworkAccessManager>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSslCertificate>
#include <QSslCipher>
#include <QSslConfiguration>
#include <QSet>
#include <QSslError>
#include <QTcpSocket>
#include <QTimer>
#include <QUdpSocket>

#include <algorithm>
#include <memory>

namespace forkmesh::nexus {
namespace {

struct ProbeState {
    PortResult result;
    QElapsedTimer clock;
    QByteArray banner;
    bool connected = false;
    bool http = false;
    bool done = false;
};

bool bannerComplete(const ProbeState &state)
{
    if (state.banner.size() >= 4096)
        return true;
    if (state.http)
        return state.banner.contains("\r\n\r\n") || state.banner.contains("\n\n");
    return state.banner.contains('\n');
}

QString protocolName(QSsl::SslProtocol protocol)
{
    switch (protocol) {
    case QSsl::TlsV1_2: return QStringLiteral("TLS 1.2");
    case QSsl::TlsV1_3: return QStringLiteral("TLS 1.3");
    default: return QStringLiteral("TLS");
    }
}

CertificateSummary summarize(const QSslCertificate &certificate)
{
    CertificateSummary summary;
    summary.subject = certificate.subjectInfo(QSslCertificate::CommonName).join(QStringLiteral(", "));
    if (summary.subject.isEmpty())
        summary.subject = certificate.subjectInfo(QSslCertificate::Organization).join(QStringLiteral(", "));
    summary.issuer = certificate.issuerInfo(QSslCertificate::CommonName).join(QStringLiteral(", "));
    if (summary.issuer.isEmpty())
        summary.issuer = certificate.issuerInfo(QSslCertificate::Organization).join(QStringLiteral(", "));
    summary.notBefore = certificate.effectiveDate();
    summary.notAfter = certificate.expiryDate();
    return summary;
}

QString reverseName(const QString &name)
{
    const QHostAddress address(name);
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 value = address.toIPv4Address();
        return QStringLiteral("%1.%2.%3.%4.in-addr.arpa").arg(value & 0xFF).arg((value >> 8) & 0xFF).arg((value >> 16) & 0xFF).arg(value >> 24);
    }
    if (address.protocol() == QAbstractSocket::IPv6Protocol) {
        const Q_IPV6ADDR bytes = address.toIPv6Address();
        QStringList nibbles;
        for (int i = 15; i >= 0; --i) {
            nibbles << QString::number(bytes[i] & 0xF, 16) << QString::number(bytes[i] >> 4, 16);
        }
        return nibbles.join(u'.') + QStringLiteral(".ip6.arpa");
    }
    return name;
}

struct HttpState {
    HttpInspection inspection;
    QNetworkAccessManager *network = nullptr;
    QPointer<QObject> context;
    std::function<void(const HttpInspection &)> done;
    bool finished = false;
};

void requestHop(const std::shared_ptr<HttpState> &state, const QUrl &url);

void finishInspection(const std::shared_ptr<HttpState> &state)
{
    if (state->finished)
        return;
    state->finished = true;
    if (state->inspection.error.isEmpty() && !state->inspection.hops.isEmpty()) {
        const HttpHop &last = state->inspection.hops.constLast();
        state->inspection.findings = headerFindings(last.headers, last.url.scheme() == QLatin1String("https"));
    }
    if (state->context)
        state->done(state->inspection);
}

}

TcpScanner::TcpScanner(QObject *context)
    : m_context(context)
{
}

TcpScanner::~TcpScanner()
{
    m_onResult = {};
    m_onProgress = {};
    m_onFinished = {};
    cancel();
}

void TcpScanner::start(const QList<QHostAddress> &addresses, const QList<quint16> &ports, const ScanOptions &options,
                       ResultHandler onResult, ProgressHandler onProgress, FinishedHandler onFinished)
{
    cancel();
    ++m_generation;
    m_queue.clear();
    for (const QHostAddress &address : addresses) {
        for (quint16 port : ports)
            m_queue << Probe{address, port};
    }
    m_options = options;
    m_options.concurrency = qBound(1, m_options.concurrency, 256);
    m_options.timeoutMs = qBound(200, m_options.timeoutMs, 30000);
    m_onResult = std::move(onResult);
    m_onProgress = std::move(onProgress);
    m_onFinished = std::move(onFinished);
    m_next = 0;
    m_active = 0;
    m_completed = 0;
    m_running = true;
    if (m_queue.isEmpty()) {
        m_running = false;
        if (m_onFinished)
            m_onFinished(false);
        return;
    }
    launchMore();
}

void TcpScanner::cancel()
{
    if (!m_running)
        return;
    ++m_generation;
    m_running = false;
    const QList<QPointer<QTcpSocket>> sockets = m_sockets;
    m_sockets.clear();
    for (const QPointer<QTcpSocket> &socket : sockets) {
        if (socket) {
            socket->disconnect();
            socket->abort();
            socket->deleteLater();
        }
    }
    m_active = 0;
    FinishedHandler finished = m_onFinished;
    m_onResult = {};
    m_onProgress = {};
    m_onFinished = {};
    if (finished)
        finished(true);
}

void TcpScanner::launchMore()
{
    while (m_running && m_active < m_options.concurrency && m_next < m_queue.size())
        launch(m_queue.at(m_next++));
}

void TcpScanner::launch(const Probe &probe)
{
    ++m_active;
    auto *socket = new QTcpSocket(m_context);
    m_sockets << socket;
    auto *deadline = new QTimer(socket);
    deadline->setSingleShot(true);
    auto state = std::make_shared<ProbeState>();
    state->result.address = probe.address.toString();
    state->result.port = probe.port;
    state->http = sendsHttpProbe(probe.port);
    state->clock.start();
    const quint64 generation = m_generation;

    auto finish = [this, socket, state, generation](PortState portState) {
        if (state->done || generation != m_generation)
            return;
        state->done = true;
        state->result.state = portState;
        if (portState == PortState::Open) {
            const ServiceGuess guess = identifyService(state->result.port, state->banner);
            state->result.service = guess.name;
            state->result.detail = guess.detail;
            state->result.confirmed = guess.confirmed;
        } else if (const KnownService *service = serviceForPort(state->result.port)) {
            state->result.service = service->name;
        }
        complete(socket, state->result);
    };

    QObject::connect(socket, &QTcpSocket::connected, socket, [socket, deadline, state, this] {
        state->connected = true;
        state->result.latencyMs = int(state->clock.elapsed());
        if (state->http) {
            const QByteArray host = socket->peerAddress().toString().toLatin1();
            socket->write("HEAD / HTTP/1.0\r\nHost: " + host + "\r\nUser-Agent: ForkMesh-NEXUS\r\nConnection: close\r\n\r\n");
        }
        deadline->start(m_options.bannerMs);
    });
    QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, state, finish] {
        state->banner += socket->read(4096 - state->banner.size());
        if (bannerComplete(*state))
            finish(PortState::Open);
    });
    QObject::connect(socket, &QAbstractSocket::errorOccurred, socket, [state, finish](QAbstractSocket::SocketError error) {
        if (state->connected)
            finish(PortState::Open);
        else if (error == QAbstractSocket::ConnectionRefusedError)
            finish(PortState::Closed);
        else
            finish(PortState::NoResponse);
    });
    QObject::connect(deadline, &QTimer::timeout, socket, [state, finish] {
        finish(state->connected ? PortState::Open : PortState::NoResponse);
    });
    deadline->start(m_options.timeoutMs);
    socket->connectToHost(probe.address, probe.port);
}

void TcpScanner::complete(QTcpSocket *socket, const PortResult &result)
{
    m_sockets.removeAll(socket);
    socket->disconnect();
    socket->abort();
    socket->deleteLater();
    --m_active;
    ++m_completed;
    if (m_onResult)
        m_onResult(result);
    if (!m_running)
        return;
    if (m_onProgress)
        m_onProgress(m_completed, int(m_queue.size()));
    if (m_completed >= m_queue.size()) {
        m_running = false;
        FinishedHandler finished = m_onFinished;
        m_onResult = {};
        m_onProgress = {};
        m_onFinished = {};
        if (finished)
            finished(false);
        return;
    }
    QTimer::singleShot(0, m_context, [this, generation = m_generation] {
        if (generation == m_generation)
            launchMore();
    });
}

MdnsBrowser::MdnsBrowser(QObject *context)
    : m_context(context)
{
}

MdnsBrowser::~MdnsBrowser()
{
    m_onUpdate = {};
    m_onFinished = {};
    cancel();
}

void MdnsBrowser::start(const QStringList &serviceTypes, int seconds, UpdateHandler onUpdate, FinishedHandler onFinished)
{
    cancel();
    m_types = serviceTypes;
    m_records.clear();
    m_round = 0;
    m_rounds = qBound(1, seconds, 30);
    m_onUpdate = std::move(onUpdate);
    m_onFinished = std::move(onFinished);
    m_socket = new QUdpSocket(m_context);
    if (!m_socket->bind(QHostAddress(QHostAddress::AnyIPv4), 0)) {
        m_running = true;
        finish(QStringLiteral("Multicast DNS could not open a socket: %1").arg(m_socket->errorString()));
        return;
    }
    m_socket->setSocketOption(QAbstractSocket::MulticastTtlOption, 255);
    QObject::connect(m_socket, &QUdpSocket::readyRead, m_socket, [this] { read(); });
    m_timer = new QTimer(m_context);
    m_timer->setInterval(1000);
    QObject::connect(m_timer, &QTimer::timeout, m_timer, [this] {
        if (m_round >= m_rounds) {
            finish({});
            return;
        }
        sendRound();
    });
    m_running = true;
    sendRound();
    m_timer->start();
}

void MdnsBrowser::cancel()
{
    if (!m_running)
        return;
    finish({});
}

namespace {

QList<QNetworkInterface> multicastInterfaces()
{
    QList<QNetworkInterface> found;
    for (const QNetworkInterface &interface : QNetworkInterface::allInterfaces()) {
        const auto flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || !flags.testFlag(QNetworkInterface::CanMulticast) || flags.testFlag(QNetworkInterface::IsLoopBack))
            continue;
        const QList<QNetworkAddressEntry> entries = interface.addressEntries();
        if (std::any_of(entries.cbegin(), entries.cend(), [](const QNetworkAddressEntry &entry) {
                return entry.ip().protocol() == QAbstractSocket::IPv4Protocol;
            }))
            found << interface;
    }
    return found;
}

}

void MdnsBrowser::sendRound()
{
    ++m_round;
    QList<QByteArray> packets;
    QStringList ptr{QStringLiteral("_services._dns-sd._udp.local")};
    ptr << m_types;
    for (qsizetype i = 0; i < ptr.size(); i += 12)
        packets << mdnsQuery(ptr.mid(i, 12), DnsTypePtr);
    QStringList unresolved;
    QStringList hosts;
    QSet<QString> haveSrv;
    QSet<QString> haveAddress;
    for (const DnsRecord &record : std::as_const(m_records)) {
        if (record.type == DnsTypeSrv) {
            haveSrv.insert(record.name);
            hosts << record.target;
        } else if (record.type == DnsTypeA || record.type == DnsTypeAaaa) {
            haveAddress.insert(record.name.toLower());
        }
    }
    for (const DnsRecord &record : std::as_const(m_records)) {
        if (record.type == DnsTypePtr && !record.name.startsWith(QLatin1String("_services.")) && !haveSrv.contains(record.target)
            && !unresolved.contains(record.target))
            unresolved << record.target;
    }
    for (qsizetype i = 0; i < unresolved.size(); i += 12)
        packets << mdnsQuery(unresolved.mid(i, 12), DnsTypeSrv);
    QStringList missing;
    for (const QString &host : std::as_const(hosts)) {
        if (!haveAddress.contains(host.toLower()) && !missing.contains(host))
            missing << host;
    }
    for (qsizetype i = 0; i < missing.size(); i += 12)
        packets << mdnsQuery(missing.mid(i, 12), DnsTypeA);
    const QHostAddress group(QStringLiteral("224.0.0.251"));
    QList<QNetworkInterface> interfaces = multicastInterfaces();
    if (interfaces.isEmpty())
        interfaces << QNetworkInterface();
    bool sent = false;
    for (const QNetworkInterface &interface : std::as_const(interfaces)) {
        if (interface.isValid())
            m_socket->setMulticastInterface(interface);
        for (const QByteArray &packet : std::as_const(packets))
            sent = m_socket->writeDatagram(packet, group, 5353) >= 0 || sent;
    }
    if (!sent && m_round == 1) {
        finish(QStringLiteral("Multicast DNS queries could not be sent: %1").arg(m_socket->errorString()));
        return;
    }
    if (m_onUpdate)
        m_onUpdate(devicesFromRecords(m_records), m_round, m_rounds);
}

void MdnsBrowser::read()
{
    bool changed = false;
    while (m_socket && m_socket->hasPendingDatagrams()) {
        const QNetworkDatagram datagram = m_socket->receiveDatagram(9000);
        const QList<DnsRecord> records = parseDnsMessage(datagram.data());
        for (const DnsRecord &record : records) {
            if (record.type == DnsTypePtr && record.name == QLatin1String("_services._dns-sd._udp.local")
                && record.target.endsWith(QLatin1String(".local")) && !m_types.contains(record.target) && m_types.size() < 64) {
                m_types << record.target;
            }
            const bool known = std::any_of(m_records.cbegin(), m_records.cend(), [&record](const DnsRecord &existing) {
                return existing.type == record.type && existing.name == record.name && existing.target == record.target
                    && existing.address == record.address && existing.port == record.port;
            });
            if (!known && m_records.size() < 4000) {
                m_records << record;
                changed = true;
            }
        }
    }
    if (changed && m_onUpdate)
        m_onUpdate(devicesFromRecords(m_records), m_round, m_rounds);
}

void MdnsBrowser::finish(const QString &error)
{
    if (!m_running)
        return;
    m_running = false;
    if (m_timer) {
        m_timer->stop();
        m_timer->deleteLater();
        m_timer = nullptr;
    }
    if (m_socket) {
        m_socket->disconnect();
        m_socket->close();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    FinishedHandler finished = m_onFinished;
    m_onUpdate = {};
    m_onFinished = {};
    if (finished)
        finished(devicesFromRecords(m_records), error);
}

QStringList dnsRecordTypes()
{
    return {QStringLiteral("A"), QStringLiteral("AAAA"), QStringLiteral("CNAME"), QStringLiteral("MX"),
            QStringLiteral("NS"), QStringLiteral("TXT"), QStringLiteral("PTR"), QStringLiteral("SRV")};
}

void lookupDns(QObject *context, const QString &name, const QString &type, std::function<void(const DnsLookupResult &)> done)
{
    static const QHash<QString, QDnsLookup::Type> types{
        {QStringLiteral("A"), QDnsLookup::A}, {QStringLiteral("AAAA"), QDnsLookup::AAAA},
        {QStringLiteral("CNAME"), QDnsLookup::CNAME}, {QStringLiteral("MX"), QDnsLookup::MX},
        {QStringLiteral("NS"), QDnsLookup::NS}, {QStringLiteral("TXT"), QDnsLookup::TXT},
        {QStringLiteral("PTR"), QDnsLookup::PTR}, {QStringLiteral("SRV"), QDnsLookup::SRV}};
    const QString query = type == QLatin1String("PTR") ? reverseName(name.trimmed()) : name.trimmed();
    auto *lookup = new QDnsLookup(types.value(type, QDnsLookup::A), query, context);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    QObject::connect(lookup, &QDnsLookup::finished, context, [lookup, clock, query, type, done] {
        DnsLookupResult result;
        result.name = query;
        result.type = type;
        result.elapsedMs = int(clock->elapsed());
        if (lookup->error() != QDnsLookup::NoError) {
            result.error = lookup->errorString();
        } else {
            for (const QDnsHostAddressRecord &record : lookup->hostAddressRecords())
                result.answers << DnsAnswer{record.name(), record.value().protocol() == QAbstractSocket::IPv6Protocol ? QStringLiteral("AAAA") : QStringLiteral("A"), record.value().toString(), int(record.timeToLive())};
            for (const QDnsDomainNameRecord &record : lookup->canonicalNameRecords())
                result.answers << DnsAnswer{record.name(), QStringLiteral("CNAME"), record.value(), int(record.timeToLive())};
            for (const QDnsMailExchangeRecord &record : lookup->mailExchangeRecords())
                result.answers << DnsAnswer{record.name(), QStringLiteral("MX"), QStringLiteral("%1 %2").arg(record.preference()).arg(record.exchange()), int(record.timeToLive())};
            for (const QDnsDomainNameRecord &record : lookup->nameServerRecords())
                result.answers << DnsAnswer{record.name(), QStringLiteral("NS"), record.value(), int(record.timeToLive())};
            for (const QDnsDomainNameRecord &record : lookup->pointerRecords())
                result.answers << DnsAnswer{record.name(), QStringLiteral("PTR"), record.value(), int(record.timeToLive())};
            for (const QDnsServiceRecord &record : lookup->serviceRecords())
                result.answers << DnsAnswer{record.name(), QStringLiteral("SRV"), QStringLiteral("%1 %2 %3 %4").arg(record.priority()).arg(record.weight()).arg(record.port()).arg(record.target()), int(record.timeToLive())};
            for (const QDnsTextRecord &record : lookup->textRecords()) {
                QStringList values;
                for (const QByteArray &value : record.values())
                    values << QString::fromUtf8(value);
                result.answers << DnsAnswer{record.name(), QStringLiteral("TXT"), values.join(QStringLiteral(" ")), int(record.timeToLive())};
            }
        }
        lookup->deleteLater();
        done(result);
    });
    lookup->lookup();
}

namespace {

void requestHop(const std::shared_ptr<HttpState> &state, const QUrl &url)
{
    if (!state->context) {
        state->finished = true;
        return;
    }
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("ForkMesh-NEXUS"));
    request.setTransferTimeout(15000);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    QNetworkReply *reply = state->network->get(request);
    auto captured = std::make_shared<bool>(false);
    QObject::connect(reply, &QNetworkReply::encrypted, reply, [state, reply] {
        const QSslConfiguration configuration = reply->sslConfiguration();
        state->inspection.protocol = protocolName(configuration.sessionProtocol());
        state->inspection.cipher = configuration.sessionCipher().name();
        state->inspection.chain.clear();
        for (const QSslCertificate &certificate : configuration.peerCertificateChain())
            state->inspection.chain << summarize(certificate);
    });
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [state](const QList<QSslError> &errors) {
        for (const QSslError &error : errors) {
            if (!state->inspection.tlsErrors.contains(error.errorString()))
                state->inspection.tlsErrors << error.errorString();
        }
    });
    auto capture = [state, reply, url, clock, captured] {
        if (*captured)
            return;
        const QVariant status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
        if (!status.isValid())
            return;
        *captured = true;
        HttpHop hop;
        hop.url = url;
        hop.status = status.toInt();
        hop.reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
        hop.elapsedMs = int(clock->elapsed());
        hop.headers = reply->rawHeaderPairs();
        state->inspection.hops << hop;
    };
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, [reply, capture, captured] {
        capture();
        if (*captured)
            reply->abort();
    });
    QObject::connect(reply, &QNetworkReply::finished, reply, [state, reply, url, capture, captured] {
        capture();
        reply->deleteLater();
        if (!*captured) {
            state->inspection.error = reply->errorString();
            finishInspection(state);
            return;
        }
        const HttpHop &hop = state->inspection.hops.constLast();
        const QByteArray location = reply->rawHeader("Location");
        if (hop.status >= 300 && hop.status < 400 && !location.isEmpty() && state->inspection.hops.size() < 6) {
            const QUrl next = url.resolved(QUrl::fromEncoded(location));
            if (next.isValid() && (next.scheme() == QLatin1String("http") || next.scheme() == QLatin1String("https"))) {
                requestHop(state, next);
                return;
            }
        }
        finishInspection(state);
    });
}

}

void inspectHttp(QNetworkAccessManager *network, QObject *context, const QUrl &url, std::function<void(const HttpInspection &)> done)
{
    auto state = std::make_shared<HttpState>();
    state->network = network;
    state->context = context;
    state->done = std::move(done);
    if (!network) {
        state->inspection.error = QStringLiteral("The desktop network is not available.");
        finishInspection(state);
        return;
    }
    if (!url.isValid() || (url.scheme() != QLatin1String("http") && url.scheme() != QLatin1String("https")) || url.host().isEmpty()) {
        state->inspection.error = QStringLiteral("Enter a full http:// or https:// address.");
        finishInspection(state);
        return;
    }
    requestHop(state, url);
}

}
