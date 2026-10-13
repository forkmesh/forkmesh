#pragma once

// An HTTP/3 GET for Qt. QtNetwork has no HTTP/3 (QNetworkAccessManager stops
// at HTTP/2), so this drives OpenSSL's QUIC client and nghttp3's HTTP/3 framing
// from the Qt event loop: a QSocketNotifier on the UDP socket and a timer on
// OpenSSL's next QUIC deadline. One object is one request; the response body
// is handed over as it arrives, which is what a long-lived event stream needs.
//
// Built only with FORKMESH_HAVE_HTTP3 (OpenSSL >= 3.2 and nghttp3 found at
// configure time). Without it http3Available() is false and start() refuses.
//
// Not a Q_OBJECT on purpose: callbacks instead of signals keep it out of the
// moc lists. Callbacks run on the owning thread from the event loop, never
// from inside start(); `finished` fires exactly once, after which the object
// is inert and may be deleted (deleteLater() from within the callback).

#include <QByteArray>
#include <QList>
#include <QObject>
#include <QPair>
#include <QString>
#include <QUrl>

#include <functional>
#include <memory>

namespace forkmesh::net {

bool http3Available();

class Http3Get final : public QObject
{
public:
    using Header = QPair<QByteArray, QByteArray>;

    // Response status, once the response headers are complete.
    std::function<void(int status)> onHeaders;
    // A slice of the response body, in order.
    std::function<void(const QByteArray &chunk)> onData;
    // The request is over: an empty error means the server ended the response
    // cleanly (any status); otherwise it says what failed.
    std::function<void(const QString &error)> onFinished;

    explicit Http3Get(QObject *parent = nullptr);
    ~Http3Get() override;

    // https URLs only. Header names are sent lowercased, as HTTP/3 requires.
    // Returns false (and fires nothing) when the request cannot even start.
    bool start(const QUrl &url, const QList<Header> &headers = {},
               QString *error = nullptr);
    // Drops the connection without a callback.
    void abort();

    int connectTimeoutMs = 15000;

    struct State;

private:
    std::unique_ptr<State> d;
};

} // namespace forkmesh::net
