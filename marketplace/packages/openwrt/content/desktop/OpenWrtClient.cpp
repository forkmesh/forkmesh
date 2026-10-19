#include "OpenWrtClient.h"

#include "NetworkReplyError.h"

#include <QCryptographicHash>
#include <QHttpMultiPart>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QUrlQuery>

#if QT_CONFIG(ssl)
#include <QSslCertificate>
#include <QSslConfiguration>
#include <QSslError>
#endif

namespace forkmesh::openwrt {
namespace {

const QString kLoginObject = QStringLiteral("session");
const QString kLoginMethod = QStringLiteral("login");

int httpStatus(QNetworkReply *reply)
{
    const QVariant code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    return code.isValid() ? code.toInt() : 0;
}

QString redirectProblem(QNetworkReply *reply)
{
    const QUrl location = reply->header(QNetworkRequest::LocationHeader).toUrl();
    if (location.scheme() == QStringLiteral("https"))
        return QStringLiteral("The router only answers over HTTPS. Change the address to https://%1.").arg(reply->url().host());
    return QStringLiteral("The router redirected the request to %1. Use that address instead.").arg(location.toString());
}

}

Client::Client(QNetworkAccessManager *network)
    : m_network(network), m_guard(new QObject)
{
}

Client::~Client()
{
    for (const QPointer<QNetworkReply> &reply : std::as_const(m_replies)) {
        if (!reply)
            continue;
        QObject::disconnect(reply, nullptr, m_guard.get(), nullptr);
        reply->abort();
        reply->deleteLater();
    }
}

void Client::setBase(const QUrl &base)
{
    if (base != m_base) {
        m_session.clear();
        m_password.clear();
    }
    m_base = base;
}

void Client::setTrustedCertificate(const QString &fingerprint)
{
    m_trustedCertificate = fingerprint;
}

QNetworkRequest Client::request(const QString &path, int timeoutMs) const
{
    QUrl url = m_base;
    url.setPath(path);
    QNetworkRequest request(url);
    request.setTransferTimeout(timeoutMs);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::AlwaysNetwork);
    forkmesh::markPrivatePayload(request);
    return request;
}

void Client::watchCertificate(QNetworkReply *reply, const std::shared_ptr<QString> &untrusted)
{
    m_replies.removeAll(nullptr);
    m_replies << reply;
#if QT_CONFIG(ssl)
    const QString trusted = m_trustedCertificate;
    QObject::connect(reply, &QNetworkReply::sslErrors, reply, [reply, trusted, untrusted](const QList<QSslError> &errors) {
        QSslCertificate certificate = reply->sslConfiguration().peerCertificate();
        if (certificate.isNull() && !errors.isEmpty())
            certificate = errors.first().certificate();
        if (certificate.isNull())
            return;
        const QString fingerprint = certificateFingerprint(certificate.toDer());
        if (!trusted.isEmpty() && fingerprint == trusted)
            reply->ignoreSslErrors(errors);
        else
            *untrusted = fingerprint;
    });
#else
    Q_UNUSED(untrusted);
#endif
}

RpcResult Client::transportFailure(QNetworkReply *reply, const QString &untrusted) const
{
    RpcResult result;
    if (!untrusted.isEmpty()) {
        result.certificate = untrusted;
        result.error = QStringLiteral("The router's HTTPS certificate is not trusted yet. Compare its fingerprint with the one the router shows, then choose Trust.");
        return result;
    }
    result.unreachable = true;
    if (forkmesh::networkReplyBlockedByOfflineMode(reply)) {
        result.error = QStringLiteral("ForkMesh is in offline mode, so the router cannot be reached.");
        return result;
    }
    const QString reason = reply->errorString().trimmed();
    result.error = QStringLiteral("Could not reach the router at %1: %2").arg(m_base.host(), reason.isEmpty() ? QStringLiteral("no answer") : reason);
    return result;
}

void Client::signIn(const QString &user, const QString &password, Done done)
{
    m_session.clear();
    m_user = user;
    m_password.clear();
    const QJsonObject args{{QStringLiteral("username"), user},
                           {QStringLiteral("password"), password},
                           {QStringLiteral("timeout"), kSessionTimeoutSeconds}};
    send(Pending{kLoginObject, kLoginMethod, args,
                 [this, password, done](const RpcResult &result) {
                     RpcResult reported = result;
                     if (result.ok) {
                         m_session = result.data.value(QStringLiteral("ubus_rpc_session")).toString();
                         m_password = password;
                         if (m_session.isEmpty()) {
                             reported.ok = false;
                             reported.error = QStringLiteral("The router accepted the sign-in but returned no session.");
                         }
                     } else if (result.accessDenied) {
                         reported.error = QStringLiteral("The user name or password is wrong.");
                     }
                     done(reported);
                 },
                 20000},
         false);
}

void Client::signOut()
{
    if (!m_session.isEmpty())
        send(Pending{QStringLiteral("session"), QStringLiteral("destroy"), {}, [](const RpcResult &) {}, 5000}, false);
    m_session.clear();
    m_password.clear();
    m_waiting.clear();
}

void Client::call(const QString &object, const QString &method, const QJsonObject &args, Done done, int timeoutMs)
{
    send(Pending{object, method, args, std::move(done), timeoutMs}, true);
}

void Client::exec(const QString &command, const QStringList &params, Done done, int timeoutMs)
{
    const QJsonObject args{{QStringLiteral("command"), command}, {QStringLiteral("params"), QJsonArray::fromStringList(params)}};
    call(QStringLiteral("file"), QStringLiteral("exec"), args,
         [done](const RpcResult &result) {
             RpcResult reported = result;
             if (result.ok) {
                 const int code = result.data.value(QStringLiteral("code")).toInt();
                 if (code != 0) {
                     reported.ok = false;
                     const QString errors = result.data.value(QStringLiteral("stderr")).toString().trimmed();
                     reported.error = errors.isEmpty() ? QStringLiteral("The command exited with code %1.").arg(code) : errors.right(600);
                 }
             }
             done(reported);
         },
         timeoutMs);
}

void Client::send(const Pending &pending, bool mayRenew)
{
    if (!m_network) {
        RpcResult result;
        result.error = QStringLiteral("The desktop network is not available.");
        pending.done(result);
        return;
    }
    const bool login = pending.object == kLoginObject && pending.method == kLoginMethod;
    QNetworkRequest request = this->request(QStringLiteral("/ubus"), pending.timeoutMs);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    const QByteArray body = callBody(m_nextId++, login ? kAnonymousSession : m_session, pending.object, pending.method, pending.args);
    QNetworkReply *reply = m_network->post(request, body);
    auto untrusted = std::make_shared<QString>();
    watchCertificate(reply, untrusted);
    QObject::connect(reply, &QNetworkReply::finished, m_guard.get(), [this, reply, pending, mayRenew, untrusted] {
        reply->deleteLater();
        const int status = httpStatus(reply);
        if (status >= 301 && status <= 308) {
            RpcResult result;
            result.error = redirectProblem(reply);
            pending.done(result);
            return;
        }
        if (reply->error() != QNetworkReply::NoError && status == 0) {
            pending.done(transportFailure(reply, *untrusted));
            return;
        }
        const RpcResult result = parseReply(reply->readAll(), status);
        if (result.accessDenied && mayRenew && !m_password.isEmpty()) {
            renewThen(pending);
            return;
        }
        pending.done(result);
    });
}

void Client::renewSession(Done done)
{
    if (!m_network || m_password.isEmpty()) {
        RpcResult failed;
        failed.error = QStringLiteral("Sign out and sign in again so the router shows the new settings.");
        done(failed);
        return;
    }
    renewThen(Pending{QStringLiteral("uci"), QStringLiteral("configs"), {}, std::move(done)});
}

void Client::renewThen(const Pending &pending)
{
    m_waiting << pending;
    if (m_renewing)
        return;
    m_renewing = true;
    const QJsonObject args{{QStringLiteral("username"), m_user},
                           {QStringLiteral("password"), m_password},
                           {QStringLiteral("timeout"), kSessionTimeoutSeconds}};
    send(Pending{kLoginObject, kLoginMethod, args,
                 [this](const RpcResult &result) {
                     m_renewing = false;
                     const QList<Pending> waiting = std::exchange(m_waiting, {});
                     const QString session = result.data.value(QStringLiteral("ubus_rpc_session")).toString();
                     if (result.ok && !session.isEmpty()) {
                         m_session = session;
                         if (m_sessionRenewed)
                             m_sessionRenewed();
                         for (const Pending &next : waiting)
                             send(next, false);
                         return;
                     }
                     RpcResult failed = result;
                     failed.ok = false;
                     failed.accessDenied = true;
                     failed.error = QStringLiteral("The router session ended and signing in again failed: %1").arg(result.error);
                     for (const Pending &next : waiting)
                         next.done(failed);
                 },
                 20000},
         false);
}

void Client::upload(const QString &path, const QByteArray &data, Done done)
{
    RpcResult failed;
    if (!m_network || m_session.isEmpty()) {
        failed.error = QStringLiteral("Sign in to the router first.");
        done(failed);
        return;
    }
    auto *multipart = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    const auto field = [multipart](const QString &name, const QByteArray &value) {
        QHttpPart part;
        part.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"%1\"").arg(name));
        part.setBody(value);
        multipart->append(part);
    };
    field(QStringLiteral("sessionid"), m_session.toUtf8());
    field(QStringLiteral("filename"), path.toUtf8());
    QHttpPart file;
    file.setHeader(QNetworkRequest::ContentDispositionHeader, QStringLiteral("form-data; name=\"filedata\"; filename=\"upload.bin\""));
    file.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/octet-stream"));
    file.setBody(data);
    multipart->append(file);
    QNetworkReply *reply = m_network->post(request(QStringLiteral("/cgi-bin/cgi-upload"), 600000), multipart);
    multipart->setParent(reply);
    auto untrusted = std::make_shared<QString>();
    watchCertificate(reply, untrusted);
    const QString expected = QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
    QObject::connect(reply, &QNetworkReply::finished, m_guard.get(), [this, reply, untrusted, expected, done] {
        reply->deleteLater();
        const int status = httpStatus(reply);
        if (reply->error() != QNetworkReply::NoError && status == 0) {
            done(transportFailure(reply, *untrusted));
            return;
        }
        RpcResult result;
        const QByteArray body = reply->readAll();
        const QJsonObject answer = QJsonDocument::fromJson(body).object();
        if (status == 403 || body.contains("denied")) {
            result.accessDenied = true;
            result.error = QStringLiteral("The router refused the upload. Its access list needs LuCI's system pages (luci-mod-system).");
        } else if (answer.isEmpty()) {
            result.error = QStringLiteral("The router did not accept the file: %1").arg(QString::fromUtf8(body.left(200)).trimmed());
        } else if (answer.value(QStringLiteral("sha256sum")).toString().compare(expected, Qt::CaseInsensitive) != 0) {
            result.error = QStringLiteral("The file arrived damaged (checksum mismatch). Try again.");
        } else {
            result.ok = true;
            result.status = UbusOk;
            result.data = answer;
        }
        done(result);
    });
}

void Client::downloadBackup(BytesDone done)
{
    if (!m_network || m_session.isEmpty()) {
        done({}, QStringLiteral("Sign in to the router first."));
        return;
    }
    QNetworkRequest request = this->request(QStringLiteral("/cgi-bin/cgi-backup"), 120000);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/x-www-form-urlencoded"));
    QUrlQuery form;
    form.addQueryItem(QStringLiteral("sessionid"), m_session);
    QNetworkReply *reply = m_network->post(request, form.toString(QUrl::FullyEncoded).toUtf8());
    auto untrusted = std::make_shared<QString>();
    watchCertificate(reply, untrusted);
    QObject::connect(reply, &QNetworkReply::finished, m_guard.get(), [this, reply, untrusted, done] {
        reply->deleteLater();
        const int status = httpStatus(reply);
        if (reply->error() != QNetworkReply::NoError && status == 0) {
            done({}, transportFailure(reply, *untrusted).error);
            return;
        }
        const QByteArray body = reply->readAll();
        if (body.size() < 2 || uchar(body.at(0)) != 0x1f || uchar(body.at(1)) != 0x8b) {
            done({}, status == 403 || body.contains("denied")
                         ? QStringLiteral("The router refused to make a backup. Its access list needs LuCI's system pages (luci-mod-system).")
                         : QStringLiteral("The router did not send a backup archive: %1").arg(QString::fromUtf8(body.left(200)).trimmed()));
            return;
        }
        done(body, {});
    });
}

}
