#include "Http3Get.h"

#ifdef FORKMESH_HAVE_HTTP3

#include <QHostAddress>
#include <QHostInfo>
#include <QPointer>
#include <QSocketNotifier>
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QTimer>
#include <QtEndian>

#include <nghttp3/nghttp3.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/quic.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <sys/types.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#endif

#include <algorithm>
#include <cstring>
#include <vector>

namespace forkmesh::net {

bool http3Available() { return true; }

namespace {

QString opensslError(const char *what)
{
    QString text = QString::fromLatin1(what);
    unsigned long code = 0;
    QStringList reasons;
    while ((code = ERR_get_error()) != 0) {
        char buffer[256];
        ERR_error_string_n(code, buffer, sizeof buffer);
        reasons.append(QString::fromLatin1(buffer));
    }
    if (!reasons.isEmpty())
        text += QStringLiteral(": ") + reasons.join(QStringLiteral("; "));
    return text;
}

X509_STORE *sharedSystemTrust()
{
    static X509_STORE *const shared = [] {
        X509_STORE *store = X509_STORE_new();
        if (!store)
            return store;
        X509_STORE_set_default_paths(store);
        const QList<QSslCertificate> roots =
            QSslConfiguration::systemCaCertificates();
        for (const QSslCertificate &root : roots) {
            const QByteArray der = root.toDer();
            const unsigned char *bytes =
                reinterpret_cast<const unsigned char *>(der.constData());
            X509 *cert = d2i_X509(nullptr, &bytes, der.size());
            if (!cert)
                continue;
            X509_STORE_add_cert(store, cert);
            X509_free(cert);
        }
        ERR_clear_error();
        return store;
    }();
    return shared;
}

void addSystemTrust(SSL_CTX *ctx)
{
    if (X509_STORE *store = sharedSystemTrust()) {
        SSL_CTX_set1_cert_store(ctx, store);
        return;
    }
    SSL_CTX_set_default_verify_paths(ctx);
    ERR_clear_error();
}

} // namespace

struct Http3Get::State {
    Http3Get *owner = nullptr;
    QUrl url;
    QList<Header> headers;

    SSL_CTX *ctx = nullptr;
    SSL *conn = nullptr;
    nghttp3_conn *h3 = nullptr;
    SSL *request = nullptr;
    int64_t requestId = -1;
    // Every QUIC stream nghttp3 knows about, by id: our control and QPACK
    // streams, the request, and the server's own unidirectional streams.
    std::vector<std::pair<int64_t, SSL *>> streams;
    std::vector<int64_t> blocked;

    QSocketNotifier *readNotifier = nullptr;
    QSocketNotifier *writeNotifier = nullptr;
    QTimer *quicTimer = nullptr;
    QTimer *connectTimer = nullptr;
    int lookupId = -1;

    bool connected = false;
    bool servicing = false;
    bool done = false;       // finished (or aborted); nothing more fires
    bool teardownDue = false;
    bool headersSeen = false;
    int status = 0;

    ~State() { teardown(); }

    SSL *streamFor(int64_t id) const
    {
        for (const auto &entry : streams) {
            if (entry.first == id)
                return entry.second;
        }
        return nullptr;
    }

    void teardown()
    {
        if (lookupId >= 0) {
            QHostInfo::abortHostLookup(lookupId);
            lookupId = -1;
        }
        delete readNotifier;
        readNotifier = nullptr;
        delete writeNotifier;
        writeNotifier = nullptr;
        if (quicTimer)
            quicTimer->stop();
        if (connectTimer)
            connectTimer->stop();
        if (h3) {
            nghttp3_conn_del(h3);
            h3 = nullptr;
        }
        for (auto &entry : streams)
            SSL_free(entry.second);
        streams.clear();
        request = nullptr;
        if (conn && connected) {
            // Best effort CONNECTION_CLOSE, so the server lets go of the
            // request now rather than at its idle timeout.
            (void)SSL_shutdown_ex(conn,
                                  SSL_SHUTDOWN_FLAG_RAPID |
                                      SSL_SHUTDOWN_FLAG_NO_BLOCK,
                                  nullptr, 0);
            SSL_handle_events(conn);
        }
        if (conn) {
            SSL_free(conn);
            conn = nullptr;
        }
        if (ctx) {
            SSL_CTX_free(ctx);
            ctx = nullptr;
        }
        ERR_clear_error();
    }

    // Ends the request: at most once, and never while service() is still
    // walking OpenSSL objects a callback might otherwise free.
    void finish(const QString &error)
    {
        if (done)
            return;
        done = true;
        if (servicing)
            teardownDue = true;
        else
            teardown();
        if (owner->onFinished)
            owner->onFinished(error);
    }

    // --- nghttp3 callbacks -------------------------------------------------

    static int onRecvHeader(nghttp3_conn *, int64_t streamId, int32_t token,
                            nghttp3_rcbuf *, nghttp3_rcbuf *value, uint8_t,
                            void *user, void *)
    {
        auto *self = static_cast<State *>(user);
        if (streamId == self->requestId &&
            token == NGHTTP3_QPACK_TOKEN__STATUS) {
            const nghttp3_vec v = nghttp3_rcbuf_get_buf(value);
            self->status = QByteArray(reinterpret_cast<const char *>(v.base),
                                      int(v.len))
                               .toInt();
        }
        return 0;
    }

    static int onEndHeaders(nghttp3_conn *, int64_t streamId, int, void *user,
                            void *)
    {
        auto *self = static_cast<State *>(user);
        if (streamId == self->requestId && !self->headersSeen) {
            self->headersSeen = true;
            if (self->owner->onHeaders && !self->done)
                self->owner->onHeaders(self->status);
        }
        return 0;
    }

    static int onRecvData(nghttp3_conn *, int64_t streamId, const uint8_t *data,
                          size_t length, void *user, void *)
    {
        auto *self = static_cast<State *>(user);
        if (streamId == self->requestId && self->owner->onData && !self->done)
            self->owner->onData(QByteArray(reinterpret_cast<const char *>(data),
                                           qsizetype(length)));
        return 0;
    }

    static int onEndStream(nghttp3_conn *, int64_t streamId, void *user, void *)
    {
        auto *self = static_cast<State *>(user);
        if (streamId == self->requestId)
            self->finish(QString());
        return 0;
    }

    static int onStreamClose(nghttp3_conn *, int64_t streamId,
                             uint64_t errorCode, void *user, void *)
    {
        auto *self = static_cast<State *>(user);
        if (streamId == self->requestId && !self->done) {
            self->finish(errorCode == NGHTTP3_H3_NO_ERROR
                             ? QString()
                             : QStringLiteral("HTTP/3 stream closed (error 0x%1)")
                                   .arg(errorCode, 0, 16));
        }
        return 0;
    }

    // --- setup ---------------------------------------------------------------

    bool connectTo(const QHostAddress &address, QString *error)
    {
        const quint16 port = quint16(url.port(443));
        BIO_ADDR *peer = BIO_ADDR_new();
        int family = AF_INET;
        bool made = false;
        if (address.protocol() == QAbstractSocket::IPv6Protocol) {
            family = AF_INET6;
            const Q_IPV6ADDR raw = address.toIPv6Address();
            made = BIO_ADDR_rawmake(peer, AF_INET6, raw.c, sizeof raw.c,
                                    qToBigEndian<quint16>(port)) == 1;
        } else {
            const quint32 raw = qToBigEndian<quint32>(address.toIPv4Address());
            made = BIO_ADDR_rawmake(peer, AF_INET, &raw, sizeof raw,
                                    qToBigEndian<quint16>(port)) == 1;
        }
        if (!made) {
            BIO_ADDR_free(peer);
            *error = QStringLiteral("unusable address %1").arg(address.toString());
            return false;
        }

        const int fd = BIO_socket(family, SOCK_DGRAM, IPPROTO_UDP, 0);
        if (fd < 0 || !BIO_connect(fd, peer, 0) || !BIO_socket_nbio(fd, 1)) {
            if (fd >= 0)
                BIO_closesocket(fd);
            BIO_ADDR_free(peer);
            *error = opensslError("cannot open a UDP socket");
            return false;
        }
        BIO *bio = BIO_new(BIO_s_datagram());
        if (!bio) {
            BIO_closesocket(fd);
            BIO_ADDR_free(peer);
            *error = opensslError("cannot create a datagram BIO");
            return false;
        }
        BIO_set_fd(bio, fd, BIO_CLOSE);

        ctx = SSL_CTX_new(OSSL_QUIC_client_method());
        if (!ctx) {
            BIO_free(bio);
            BIO_ADDR_free(peer);
            *error = opensslError("OpenSSL has no QUIC client");
            return false;
        }
        SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
        addSystemTrust(ctx);

        conn = SSL_new(ctx);
        const QByteArray host = url.host().toUtf8();
        static const unsigned char alpn[] = {2, 'h', '3'};
        const bool configured =
            conn && SSL_set_tlsext_host_name(conn, host.constData()) == 1 &&
            SSL_set1_host(conn, host.constData()) == 1 &&
            SSL_set_alpn_protos(conn, alpn, sizeof alpn) == 0 &&
            SSL_set1_initial_peer_addr(conn, peer) == 1 &&
            SSL_set_default_stream_mode(conn, SSL_DEFAULT_STREAM_MODE_NONE) == 1 &&
            SSL_set_incoming_stream_policy(
                conn, SSL_INCOMING_STREAM_POLICY_ACCEPT, 0) == 1 &&
            SSL_set_blocking_mode(conn, 0) == 1;
        BIO_ADDR_free(peer);
        if (!configured) {
            BIO_free(bio);
            *error = opensslError("cannot configure the QUIC connection");
            return false;
        }
        SSL_set_bio(conn, bio, bio);

        readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, owner);
        QObject::connect(readNotifier, &QSocketNotifier::activated, owner,
                         [this] { service(); });
        writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, owner);
        writeNotifier->setEnabled(false);
        QObject::connect(writeNotifier, &QSocketNotifier::activated, owner,
                         [this] { service(); });
        return true;
    }

    bool startHttp3(QString *error)
    {
        nghttp3_callbacks callbacks;
        std::memset(&callbacks, 0, sizeof callbacks);
        callbacks.recv_header = &State::onRecvHeader;
        callbacks.end_headers = &State::onEndHeaders;
        callbacks.recv_data = &State::onRecvData;
        callbacks.end_stream = &State::onEndStream;
        callbacks.stream_close = &State::onStreamClose;
        nghttp3_settings settings;
        nghttp3_settings_default(&settings);
        if (nghttp3_conn_client_new(&h3, &callbacks, &settings,
                                    nghttp3_mem_default(), this) != 0) {
            *error = QStringLiteral("cannot start HTTP/3");
            return false;
        }

        const auto open = [this](uint64_t flags) -> int64_t {
            SSL *stream = SSL_new_stream(conn, flags | SSL_STREAM_FLAG_NO_BLOCK);
            if (!stream)
                return -1;
            SSL_set_blocking_mode(stream, 0);
            const int64_t id = int64_t(SSL_get_stream_id(stream));
            streams.emplace_back(id, stream);
            return id;
        };
        const int64_t control = open(SSL_STREAM_FLAG_UNI);
        const int64_t encoder = open(SSL_STREAM_FLAG_UNI);
        const int64_t decoder = open(SSL_STREAM_FLAG_UNI);
        requestId = open(0);
        if (control < 0 || encoder < 0 || decoder < 0 || requestId < 0 ||
            nghttp3_conn_bind_control_stream(h3, control) != 0 ||
            nghttp3_conn_bind_qpack_streams(h3, encoder, decoder) != 0) {
            *error = opensslError("cannot open the HTTP/3 streams");
            return false;
        }
        request = streamFor(requestId);

        QByteArray authority = url.host(QUrl::FullyEncoded).toUtf8();
        if (url.port(443) != 443)
            authority += ':' + QByteArray::number(url.port());
        QByteArray path = url.path(QUrl::FullyEncoded).toUtf8();
        if (path.isEmpty())
            path = "/";
        if (url.hasQuery())
            path += '?' + url.query(QUrl::FullyEncoded).toUtf8();
        QList<Header> fields = {
            {":method", "GET"},
            {":scheme", "https"},
            {":authority", authority},
            {":path", path},
            {"user-agent", "ForkMesh-desktop"},
        };
        for (const Header &header : std::as_const(headers))
            fields.append({header.first.toLower(), header.second});
        std::vector<nghttp3_nv> nva;
        nva.reserve(size_t(fields.size()));
        for (const Header &field : std::as_const(fields)) {
            nghttp3_nv nv;
            nv.name = reinterpret_cast<const uint8_t *>(field.first.constData());
            nv.namelen = size_t(field.first.size());
            nv.value = reinterpret_cast<const uint8_t *>(field.second.constData());
            nv.valuelen = size_t(field.second.size());
            nv.flags = NGHTTP3_NV_FLAG_NONE;
            nva.push_back(nv);
        }
        // The name/value bytes are copied into QPACK output as it is encoded,
        // which submit_request does straight away; `fields` may then go.
        if (nghttp3_conn_submit_request(h3, requestId, nva.data(), nva.size(),
                                        nullptr, nullptr) != 0) {
            *error = QStringLiteral("cannot submit the HTTP/3 request");
            return false;
        }
        return true;
    }

    // --- the pump ------------------------------------------------------------

    bool writeHttp3(QString *error)
    {
        for (int64_t id : blocked)
            nghttp3_conn_unblock_stream(h3, id);
        blocked.clear();
        for (;;) {
            int64_t id = -1;
            int fin = 0;
            nghttp3_vec vec[16];
            const nghttp3_ssize count =
                nghttp3_conn_writev_stream(h3, &id, &fin, vec, 16);
            if (count < 0) {
                *error = QStringLiteral("HTTP/3 framing failed: %1")
                             .arg(QString::fromLatin1(nghttp3_strerror(int(count))));
                return false;
            }
            if (id < 0)
                return true;
            SSL *stream = streamFor(id);
            if (!stream) {
                *error = QStringLiteral("HTTP/3 wrote to an unknown stream");
                return false;
            }
            size_t total = 0;
            bool partial = false;
            for (nghttp3_ssize i = 0; i < count && !partial; ++i) {
                size_t written = 0;
                ERR_clear_error();
                if (vec[i].len > 0 &&
                    !SSL_write_ex(stream, vec[i].base, vec[i].len, &written)) {
                    const int code = SSL_get_error(stream, 0);
                    if (code != SSL_ERROR_WANT_WRITE) {
                        *error = opensslError("QUIC stream write failed");
                        return false;
                    }
                }
                total += written;
                partial = written < vec[i].len;
            }
            // OpenSSL copies what it accepts into its own send buffer and
            // retransmits it itself, so nghttp3 can let go of it right away.
            nghttp3_conn_add_write_offset(h3, id, total);
            nghttp3_conn_add_ack_offset(h3, id, total);
            if (partial) {
                nghttp3_conn_block_stream(h3, id);
                blocked.push_back(id);
                continue;
            }
            if (fin)
                SSL_stream_conclude(stream, 0);
        }
    }

    bool readStream(int64_t id, SSL *stream, QString *error)
    {
        unsigned char buffer[16384];
        for (;;) {
            size_t length = 0;
            ERR_clear_error();
            if (SSL_read_ex(stream, buffer, sizeof buffer, &length)) {
                const nghttp3_ssize used =
                    nghttp3_conn_read_stream(h3, id, buffer, length, 0);
                if (used < 0) {
                    *error = QStringLiteral("HTTP/3 read failed: %1").arg(
                        QString::fromLatin1(nghttp3_strerror(int(used))));
                    return false;
                }
                if (done)
                    return true;
                continue;
            }
            const int code = SSL_get_error(stream, 0);
            if (code == SSL_ERROR_WANT_READ)
                return true;
            if (code == SSL_ERROR_ZERO_RETURN) {
                nghttp3_conn_read_stream(h3, id, nullptr, 0, 1);
                return true;
            }
            switch (SSL_get_stream_read_state(stream)) {
            case SSL_STREAM_STATE_FINISHED:
            case SSL_STREAM_STATE_WRONG_DIR:
                return true;
            case SSL_STREAM_STATE_RESET_REMOTE:
                if (id == requestId) {
                    *error = QStringLiteral("the server reset the request");
                    return false;
                }
                return true;
            case SSL_STREAM_STATE_CONN_CLOSED:
                *error = QStringLiteral("the QUIC connection closed");
                return false;
            default:
                *error = opensslError("QUIC stream read failed");
                return false;
            }
        }
    }

    void service()
    {
        if (done || servicing || !conn)
            return;
        servicing = true;
        QPointer<Http3Get> guard(owner);
        QString error;
        ERR_clear_error();
        SSL_handle_events(conn);
        bool ok = true;
        if (!connected) {
            ERR_clear_error();
            const int result = SSL_connect(conn);
            if (result == 1) {
                connected = true;
                connectTimer->stop();
                ok = startHttp3(&error);
            } else {
                const int code = SSL_get_error(conn, result);
                if (code != SSL_ERROR_WANT_READ && code != SSL_ERROR_WANT_WRITE) {
                    const long verify = SSL_get_verify_result(conn);
                    error = verify != X509_V_OK
                                ? QStringLiteral("certificate rejected: %1")
                                      .arg(QString::fromLatin1(
                                          X509_verify_cert_error_string(verify)))
                                : opensslError("QUIC handshake failed");
                    ok = false;
                }
            }
        }
        if (ok && connected && !done) {
            while (SSL *incoming =
                       SSL_accept_stream(conn, SSL_ACCEPT_STREAM_NO_BLOCK)) {
                SSL_set_blocking_mode(incoming, 0);
                streams.emplace_back(int64_t(SSL_get_stream_id(incoming)),
                                     incoming);
            }
            for (size_t i = 0; ok && !done && i < streams.size(); ++i)
                ok = readStream(streams[i].first, streams[i].second, &error);
            if (ok && !done)
                ok = writeHttp3(&error);
        }
        if (!guard)
            return; // a callback deleted the request outright
        servicing = false;
        if (!ok && !done)
            finish(error);
        if (teardownDue) {
            teardownDue = false;
            teardown();
            return;
        }
        if (done || !conn)
            return;
        SSL_handle_events(conn); // flush what the writes queued
        writeNotifier->setEnabled(SSL_net_write_desired(conn) == 1);
        timeval tv{};
        int infinite = 0;
        if (SSL_get_event_timeout(conn, &tv, &infinite) == 1 && !infinite) {
            const qint64 ms = qint64(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
            quicTimer->start(int(qBound(qint64(0), ms, qint64(60 * 60 * 1000))));
        } else {
            quicTimer->stop();
        }
    }
};

Http3Get::Http3Get(QObject *parent)
    : QObject(parent), d(std::make_unique<State>())
{
    d->owner = this;
    d->quicTimer = new QTimer(this);
    d->quicTimer->setSingleShot(true);
    connect(d->quicTimer, &QTimer::timeout, this, [this] { d->service(); });
    d->connectTimer = new QTimer(this);
    d->connectTimer->setSingleShot(true);
    connect(d->connectTimer, &QTimer::timeout, this, [this] {
        d->finish(QStringLiteral("QUIC connection timed out after %1 s")
                      .arg(connectTimeoutMs / 1000));
    });
}

Http3Get::~Http3Get()
{
    d->done = true; // no callbacks from here on
}

bool Http3Get::start(const QUrl &url, const QList<Header> &headers,
                     QString *error)
{
    const auto refuse = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    if (d->ctx || d->lookupId >= 0 || d->done)
        return refuse(QStringLiteral("request already started"));
    if (url.scheme() != QLatin1String("https") || url.host().isEmpty())
        return refuse(QStringLiteral("HTTP/3 needs an https URL"));
    d->url = url;
    d->headers = headers;
    d->connectTimer->start(connectTimeoutMs);

    const auto begin = [this](const QHostAddress &address) {
        QString why;
        if (!d->connectTo(address, &why)) {
            d->finish(why);
            return;
        }
        d->service();
    };
    QHostAddress literal;
    if (literal.setAddress(url.host())) {
        QTimer::singleShot(0, this, [begin, literal] { begin(literal); });
        return true;
    }
    d->lookupId = QHostInfo::lookupHost(
        url.host(), this, [this, begin](const QHostInfo &info) {
            d->lookupId = -1;
            if (d->done)
                return;
            // IPv4 first: UDP over IPv6 is the path most often broken on
            // otherwise working networks.
            QList<QHostAddress> addresses = info.addresses();
            std::stable_partition(addresses.begin(), addresses.end(),
                                  [](const QHostAddress &a) {
                                      return a.protocol() ==
                                             QAbstractSocket::IPv4Protocol;
                                  });
            if (info.error() != QHostInfo::NoError || addresses.isEmpty()) {
                d->finish(QStringLiteral("cannot resolve %1: %2")
                              .arg(d->url.host(), info.errorString()));
                return;
            }
            begin(addresses.first());
        });
    return true;
}

void Http3Get::abort()
{
    if (d->done)
        return;
    d->done = true;
    if (d->servicing)
        d->teardownDue = true;
    else
        d->teardown();
}

} // namespace forkmesh::net

#else // !FORKMESH_HAVE_HTTP3

namespace forkmesh::net {

bool http3Available() { return false; }

struct Http3Get::State {};

Http3Get::Http3Get(QObject *parent) : QObject(parent) {}
Http3Get::~Http3Get() = default;

bool Http3Get::start(const QUrl &, const QList<Header> &, QString *error)
{
    if (error)
        *error = QStringLiteral("this build has no HTTP/3 support");
    return false;
}

void Http3Get::abort() {}

} // namespace forkmesh::net

#endif
