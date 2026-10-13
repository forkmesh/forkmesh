#pragma once

#include <QByteArray>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QString>
#include <QUrlQuery>
#include <QVariant>

namespace forkmesh {

inline QString networkFailureText(int httpStatus, const QString &reasonPhrase,
                                  const QString &qtErrorString)
{
    if (httpStatus <= 0)
        return qtErrorString.simplified();
    const QString reason = reasonPhrase.simplified();
    return reason.isEmpty()
               ? QString::number(httpStatus)
               : QStringLiteral("%1 %2").arg(QString::number(httpStatus), reason);
}

inline QString networkFailureText(const QNetworkReply *reply)
{
    if (!reply)
        return QString();
    const QVariant code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    return networkFailureText(
        code.isValid() ? code.toInt() : 0,
        reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString(),
        reply->errorString());
}

// A closed reply has no response body to inspect.  Calling peek() on one is
// not harmless: Qt emits "QIODevice::peek ... device not open", which used to
// bury the real, much more useful connectivity failure in the app log.  HTTP
// failures with a status can still have a body, so keep those available for
// diagnostics.
inline QByteArray networkReplyBodyPreview(QNetworkReply *reply, qint64 maxBytes)
{
    if (!reply || maxBytes <= 0 || !reply->isOpen() || !reply->isReadable())
        return {};
    const QVariant status =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    if (reply->error() != QNetworkReply::NoError &&
        (!status.isValid() || status.toInt() <= 0)) {
        return {};
    }
    return reply->peek(maxBytes);
}

inline bool networkReplyLooksOffline(const QNetworkReply *reply)
{
    if (!reply)
        return false;
    switch (reply->error()) {
    case QNetworkReply::ConnectionRefusedError:
    case QNetworkReply::RemoteHostClosedError:
    case QNetworkReply::HostNotFoundError:
    case QNetworkReply::TimeoutError:
    case QNetworkReply::TemporaryNetworkFailureError:
    case QNetworkReply::NetworkSessionFailedError:
    case QNetworkReply::UnknownNetworkError:
    case QNetworkReply::ProxyConnectionRefusedError:
    case QNetworkReply::ProxyConnectionClosedError:
    case QNetworkReply::ProxyNotFoundError:
    case QNetworkReply::ProxyTimeoutError:
        return true;
    default:
        break;
    }

    // Some platform backends surface an otherwise-unclassified transport error
    // when a device has no route or DNS cannot resolve while offline.
    const QString detail = reply->errorString().toLower();
    return detail.contains(QStringLiteral("offline")) ||
           detail.contains(QStringLiteral("network is unreachable")) ||
           detail.contains(QStringLiteral("network unreachable")) ||
           detail.contains(QStringLiteral("no route to host")) ||
           detail.contains(QStringLiteral("host not found")) ||
           detail.contains(QStringLiteral("temporary failure in name resolution")) ||
           detail.contains(QStringLiteral("name or service not known"));
}

// Offline mode answers a request locally instead of sending it, and
// BackoffNetworkAccessManager marks the reply with this attribute. The value is
// repeated here rather than included from that header (moc mis-parses the raw
// string literals below and would lose this namespace) so the failure text can
// tell "the reader switched the network off" apart from "the network failed";
// a static_assert in BackoffNetworkAccessManager.cpp holds the two together.
inline constexpr QNetworkRequest::Attribute kOfflineBlockedAttribute =
    static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 2);

inline bool networkReplyBlockedByOfflineMode(const QNetworkReply *reply)
{
    return reply && reply->attribute(kOfflineBlockedAttribute).toBool();
}

// A build older than the minimum supported release is kept off the network
// entirely (ClientVersionGate.h); BackoffNetworkAccessManager marks the
// replies it answers itself with this attribute. Repeated here for the same
// reason as the offline attribute above -- User + 3 and User + 4 are taken
// further down this file -- and held to the manager's copy by a static_assert
// in BackoffNetworkAccessManager.cpp.
inline constexpr QNetworkRequest::Attribute kOutdatedClientBlockedAttribute =
    static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 5);

inline bool networkReplyBlockedByOutdatedClient(const QNetworkReply *reply)
{
    return reply && reply->attribute(kOutdatedClientBlockedAttribute).toBool();
}

inline QString networkReplyFailureSummary(const QNetworkReply *reply)
{
    // Same reading as offline mode, one step earlier: this build never sent
    // the request, and no amount of retrying or connection debugging will
    // change that. Only an update will.
    if (networkReplyBlockedByOutdatedClient(reply)) {
        return QStringLiteral(
            "This ForkMesh build is out of date - the request was not sent "
            "(update ForkMesh to reach the network again)");
    }
    // A request offline mode held back is a switch this app is holding, not a
    // reachability failure. "Connection unavailable - you may be offline" sent
    // readers to debug a network that was fine for a call the desktop
    // deliberately never made (issue #712); name the switch instead.
    if (networkReplyBlockedByOfflineMode(reply)) {
        return QStringLiteral(
            "Offline mode is on - the request was not sent (switch Offline "
            "mode off beside the footer version to resume)");
    }
    const QString detail = networkFailureText(reply);
    if (!networkReplyLooksOffline(reply))
        return detail;
    const QString unavailable =
        QStringLiteral("Connection unavailable - you may be offline");
    return detail.isEmpty() ? unavailable
                            : QStringLiteral("%1 (%2)").arg(unavailable, detail);
}

// The Worker marks a 5xx it produced ON PURPOSE for an expected condition --
// a fail-closed public-snapshot barrier during a privacy transition, an
// aborted Durable Object, an upstream dependency that is simply unavailable --
// with this header (EXPECTED_DEGRADED_HEADER in the Worker's entry.py). Its
// own error log already skips them, because counting deliberate degraded
// answers as faults buries real bugs. The desktop owes them the same reading:
// the route has no answer right now and says when to come back, which is not
// evidence that the host is overloaded or that anything is broken.
inline bool networkReplyIsExpectedDegraded(const QNetworkReply *reply)
{
    return reply &&
           !reply->rawHeader("x-forkmesh-expected-degraded").trimmed().isEmpty();
}

// Some questions are answered with an HTTP error. A Mastodon account lookup
// for a handle the instance does not know replies 404 {"error":"Record not
// found"} -- that is the answer "no such account", not a broken request. The
// caller names such statuses on its request, and the net log then reads the
// reply as EXPECTED rather than ERR, so it neither badges ERROR nor pings the
// user about a contact's stale handle. BackoffNetworkAccessManager.h owns
// User and User + 1; User + 2 is kOfflineBlockedAttribute above.
inline constexpr QNetworkRequest::Attribute kExpectedHttpStatusAttribute =
    static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 3);

inline void markExpectedHttpStatus(QNetworkRequest &request, int httpStatus)
{
    QVariantList statuses =
        request.attribute(kExpectedHttpStatusAttribute).toList();
    if (!statuses.contains(QVariant(httpStatus)))
        statuses.append(httpStatus);
    request.setAttribute(kExpectedHttpStatusAttribute, statuses);
}

inline bool networkReplyStatusIsExpected(const QNetworkReply *reply)
{
    if (!reply)
        return false;
    const QVariant code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    return code.isValid() &&
           reply->request()
               .attribute(kExpectedHttpStatusAttribute)
               .toList()
               .contains(QVariant(code.toInt()));
}

// A favicon probe is decoration with a generated letter mark to fall back on,
// so its failure is no event for the net log. The log knows one by this mark
// rather than by its address: Qt follows redirects and reports the final URL,
// and qoto.org answers every path -- /favicon.ico included -- with a 308 to
// social.cleverlibre.org, whose landing /about page 500s. Matched by path, the
// probe read "net GET ERR 500 … /about" and pinged. A redirect request is a
// copy of the original, so the mark survives every hop.
inline constexpr QNetworkRequest::Attribute kFaviconProbeAttribute =
    static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 4);

inline void markFaviconProbe(QNetworkRequest &request)
{
    request.setAttribute(kFaviconProbeAttribute, true);
}

inline bool networkReplyIsFaviconProbe(const QNetworkReply *reply)
{
    return reply &&
           reply->request().attribute(kFaviconProbeAttribute).toBool();
}

inline constexpr QNetworkRequest::Attribute kExpectedTransportFailureAttribute =
    static_cast<QNetworkRequest::Attribute>(QNetworkRequest::User + 6);

inline void markExpectedTransportFailure(QNetworkRequest &request)
{
    request.setAttribute(kExpectedTransportFailureAttribute, true);
}

inline bool networkReplyTransportFailureIsExpected(const QNetworkReply *reply)
{
    if (!reply || reply->error() == QNetworkReply::NoError ||
        networkReplyBlockedByOfflineMode(reply) ||
        networkReplyBlockedByOutdatedClient(reply) ||
        !reply->request().attribute(kExpectedTransportFailureAttribute).toBool())
        return false;
    const QVariant code =
        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute);
    return !code.isValid() || code.toInt() <= 0;
}

inline bool networkLogQueryKeyIsSensitive(const QString &key)
{
    QString normalized = key.trimmed().toLower();
    normalized.replace(QLatin1Char('-'), QLatin1Char('_'));
    return normalized == QLatin1String("sig") ||
           normalized.contains(QStringLiteral("signature")) ||
           normalized.contains(QStringLiteral("ticket")) ||
           normalized.contains(QStringLiteral("token")) ||
           normalized.contains(QStringLiteral("secret")) ||
           normalized.contains(QStringLiteral("password")) ||
           normalized.contains(QStringLiteral("passphrase")) ||
           normalized.contains(QStringLiteral("credential")) ||
           normalized == QLatin1String("authorization") ||
           normalized == QLatin1String("auth") ||
           normalized == QLatin1String("session") ||
           normalized == QLatin1String("state") ||
           normalized == QLatin1String("code") ||
           normalized == QLatin1String("key") ||
           normalized.endsWith(QStringLiteral("_key"));
}

// A request URL is useful in diagnostics, but signed room-access and OAuth
// URLs carry bearer-equivalent values in their query. Keep the route and
// ordinary filters while making the log safe to share or archive.
inline QString networkLogUrl(const QUrl &url)
{
    QUrl safe = url;
    safe.setUserInfo(QString());
    safe.setFragment(QString());
    if (!url.hasQuery())
        return safe.toString(QUrl::FullyEncoded);

    const QUrlQuery original(url);
    QUrlQuery redacted;
    for (const auto &item : original.queryItems(QUrl::FullyDecoded)) {
        redacted.addQueryItem(
            item.first,
            networkLogQueryKeyIsSensitive(item.first)
                ? QStringLiteral("REDACTED")
                : item.second);
    }
    safe.setQuery(redacted);
    return safe.toString(QUrl::FullyEncoded);
}

inline QString networkResponseSnippet(const QByteArray &body,
                                      int maxChars = 200)
{
    QString text = QString::fromUtf8(body).simplified();
    // Error payloads are normally tiny JSON objects. If one ever contains
    // credentials, redact complete JSON string values before it reaches the
    // append-only log. This also protects truncated body previews.
    static const QRegularExpression jsonCredential(
        QStringLiteral(
            R"regex(("(?:passphrase|password|passwd|secret|token|session[_-]?token|access[_-]?token|refresh[_-]?token|api[_-]?key|private[_-]?key|authorization|signature|sig|ticket|url|redirect[_-]?uri|websocketurl|meetingwebsocketurl)"\s*:\s*")(?:\\.|[^"\\])*("))regex"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(jsonCredential, QStringLiteral("\\1<redacted>\\2"));
    static const QRegularExpression plainCredential(
        QStringLiteral(
            R"regex(\b(?:authorization|bearer|password|passwd|passphrase|secret|token|api[_-]?key|signature|ticket)\b\s*[:=]\s*[^\s,;&]+)regex"),
        QRegularExpression::CaseInsensitiveOption);
    text.replace(plainCredential, QStringLiteral("<credential>"));
    if (maxChars > 0 && text.size() > maxChars)
        return text.left(maxChars) + QString::fromUtf8("\xE2\x80\xA6");
    return text;
}

} // namespace forkmesh
