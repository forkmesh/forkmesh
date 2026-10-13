#include "WorldSpeechBridge.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <QUuid>

namespace {
constexpr int kMaxHeaderBytes = 16 * 1024;
constexpr int kMaxBodyBytes = 8 * 1024;
constexpr int kSessionTtlSeconds = 10 * 60;
constexpr int kMaxRequestIds = 512;

qint64 nowMs()
{
    return QDateTime::currentDateTimeUtc().toMSecsSinceEpoch();
}

bool constantTimeEqual(const QByteArray &left, const QByteArray &right)
{
    if (left.size() != right.size())
        return false;
    unsigned char difference = 0;
    for (qsizetype i = 0; i < left.size(); ++i)
        difference |= static_cast<unsigned char>(left.at(i) ^ right.at(i));
    return difference == 0;
}

bool isAllowedDestination(const QString &destination)
{
    return destination == QStringLiteral("chat") ||
           destination == QStringLiteral("issue") ||
           destination == QStringLiteral("note");
}
} // namespace

WorldSpeechBridge::WorldSpeechBridge(QObject *parent) : QObject(parent)
{
    m_lifetime.start();
}

WorldSpeechBridge::~WorldSpeechBridge()
{
    stop();
}

bool WorldSpeechBridge::start()
{
    if (isListening())
        return true;
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this,
            &WorldSpeechBridge::onNewConnection);
    if (!m_server->listen(QHostAddress::LocalHost, 0)) {
        emit auditEvent(QStringLiteral("speech_bridge_bind_failed"));
        m_server->deleteLater();
        m_server = nullptr;
        return false;
    }
    if (!m_expiryTimer) {
        m_expiryTimer = new QTimer(this);
        m_expiryTimer->setInterval(1000);
        connect(m_expiryTimer, &QTimer::timeout, this,
                [this] { purgeExpired(); });
    }
    m_expiryTimer->start();
    emit auditEvent(QStringLiteral("speech_bridge_started"));
    return true;
}

void WorldSpeechBridge::stop()
{
    revokeAll();
    const QList<QTcpSocket *> sockets = m_buffers.keys();
    m_buffers.clear();
    for (QTcpSocket *socket : sockets) {
        socket->disconnect(this);
        socket->abort();
        socket->deleteLater();
    }
    if (m_server) {
        m_server->close();
        m_server->deleteLater();
        m_server = nullptr;
    }
    if (m_expiryTimer)
        m_expiryTimer->stop();
}

bool WorldSpeechBridge::isListening() const
{
    return m_server && m_server->isListening() &&
           m_server->serverAddress().isLoopback();
}

quint16 WorldSpeechBridge::port() const
{
    return isListening() ? m_server->serverPort() : 0;
}

QJsonObject WorldSpeechBridge::issuePairing(const QString &exactOrigin,
                                            int ttlSeconds)
{
    purgeExpired();
    const QString origin = normalizedOrigin(exactOrigin);
    if (origin.isEmpty() || ttlSeconds < 1 || ttlSeconds > 10 * 60)
        return {};
    if (!start())
        return {};

    const QString secret = randomSecret();
    const qint64 expires = nowMs() + qint64(ttlSeconds) * 1000;
    const qint64 deadline =
        m_lifetime.elapsed() + qint64(ttlSeconds) * 1000;
    m_pairings.insert(digest(secret), PairingRecord{origin, deadline});
    emit auditEvent(QStringLiteral("speech_pairing_issued origin=%1").arg(origin));
    return {
        {QStringLiteral("secret"), secret},
        {QStringLiteral("origin"), origin},
        {QStringLiteral("expiresAt"),
         QDateTime::fromMSecsSinceEpoch(expires).toUTC().toString(Qt::ISODate)},
        {QStringLiteral("port"), int(port())},
    };
}

void WorldSpeechBridge::revokeAll()
{
    const bool hadCapabilities = !m_pairings.isEmpty() || !m_sessions.isEmpty();
    QSet<QString> activeCaptures;
    for (const SessionRecord &session : m_sessions) {
        if (!session.captureId.isEmpty() &&
            (session.state == QStringLiteral("recording") ||
             session.state == QStringLiteral("transcribing")))
            activeCaptures.insert(session.captureId);
    }
    m_pairings.clear();
    m_sessions.clear();
    for (const QString &captureId : activeCaptures)
        emit cancelRequested(captureId);
    if (hadCapabilities)
        emit auditEvent(QStringLiteral("speech_capabilities_revoked"));
}

void WorldSpeechBridge::publishPartial(const QString &captureId,
                                       const QString &text)
{
    const QString clean = safeTranscript(text);
    for (SessionRecord &session : m_sessions) {
        if (session.captureId != captureId || session.revoked)
            continue;
        session.state = QStringLiteral("recording");
        session.transcript = clean;
        session.error.clear();
        ++session.revision;
        return;
    }
}

void WorldSpeechBridge::publishFinal(const QString &captureId,
                                     const QString &text)
{
    const QString clean = safeTranscript(text);
    for (SessionRecord &session : m_sessions) {
        if (session.captureId != captureId || session.revoked)
            continue;
        session.state =
            clean.isEmpty() ? QStringLiteral("no-speech")
                            : QStringLiteral("complete");
        session.transcript = clean;
        session.error.clear();
        ++session.revision;
        emit auditEvent(QStringLiteral("speech_transcription_completed"));
        return;
    }
}

void WorldSpeechBridge::publishError(const QString &captureId,
                                     const QString &safeMessage)
{
    for (SessionRecord &session : m_sessions) {
        if (session.captureId != captureId || session.revoked)
            continue;
        session.state = QStringLiteral("error");
        session.error = safeTranscript(safeMessage).left(240);
        session.transcript.clear();
        ++session.revision;
        emit auditEvent(QStringLiteral("speech_transcription_failed"));
        return;
    }
}

void WorldSpeechBridge::onNewConnection()
{
    while (m_server && m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();
        if (!socket->peerAddress().isLoopback()) {
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_buffers.insert(socket, {});
        connect(socket, &QTcpSocket::readyRead, this,
                [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this,
                [this, socket] { onDisconnected(socket); });
    }
}

void WorldSpeechBridge::onReadyRead(QTcpSocket *socket)
{
    auto it = m_buffers.find(socket);
    if (it == m_buffers.end())
        return;
    it.value().append(socket->readAll());
    if (it.value().size() > kMaxHeaderBytes + kMaxBodyBytes) {
        respond(socket, 413, {{QStringLiteral("error"),
                              QStringLiteral("request_too_large")}});
        return;
    }

    const int headerEnd = it.value().indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        if (it.value().size() > kMaxHeaderBytes)
            respond(socket, 431, {{QStringLiteral("error"),
                                  QStringLiteral("headers_too_large")}});
        return;
    }

    int contentLength = 0;
    const QList<QByteArray> preliminary =
        it.value().left(headerEnd).split('\n');
    for (const QByteArray &raw : preliminary) {
        const QByteArray line = raw.trimmed();
        const int colon = line.indexOf(':');
        if (colon > 0 &&
            line.left(colon).trimmed().toLower() == "content-length") {
            bool ok = false;
            contentLength = line.mid(colon + 1).trimmed().toInt(&ok);
            if (!ok || contentLength < 0 || contentLength > kMaxBodyBytes) {
                respond(socket, 413, {{QStringLiteral("error"),
                                      QStringLiteral("invalid_body_size")}});
                return;
            }
        }
    }
    if (it.value().size() < headerEnd + 4 + contentLength)
        return;

    const QByteArray raw = it.value().left(headerEnd + 4 + contentLength);
    m_buffers.remove(socket); // one request per connection; no request smuggling
    Request request;
    QByteArray error;
    if (!parseRequest(raw, &request, &error)) {
        respond(socket, 400,
                {{QStringLiteral("error"), QString::fromLatin1(error)}});
        return;
    }
    dispatch(socket, request);
}

void WorldSpeechBridge::onDisconnected(QTcpSocket *socket)
{
    m_buffers.remove(socket);
    socket->deleteLater();
}

bool WorldSpeechBridge::parseRequest(const QByteArray &raw, Request *request,
                                     QByteArray *error) const
{
    const int split = raw.indexOf("\r\n\r\n");
    if (split < 0) {
        *error = "incomplete_request";
        return false;
    }
    const QList<QByteArray> lines = raw.left(split).split('\n');
    if (lines.isEmpty()) {
        *error = "missing_request_line";
        return false;
    }
    const QList<QByteArray> first = lines.first().trimmed().split(' ');
    if (first.size() != 3 || first.at(2) != "HTTP/1.1") {
        *error = "invalid_request_line";
        return false;
    }
    request->method = first.at(0);
    request->target = first.at(1);
    if (!request->target.startsWith('/') || request->target.contains('?') ||
        request->target.contains('#')) {
        *error = "query_and_fragment_forbidden";
        return false;
    }
    for (int i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        if (line.isEmpty())
            continue;
        const int colon = line.indexOf(':');
        if (colon <= 0) {
            *error = "invalid_header";
            return false;
        }
        const QByteArray key = line.left(colon).trimmed().toLower();
        if (request->headers.contains(key)) {
            *error = "duplicate_header";
            return false;
        }
        request->headers.insert(key, line.mid(colon + 1).trimmed());
    }
    if (request->headers.contains("transfer-encoding")) {
        *error = "transfer_encoding_forbidden";
        return false;
    }
    request->body = raw.mid(split + 4);
    return true;
}

void WorldSpeechBridge::dispatch(QTcpSocket *socket, const Request &request)
{
    purgeExpired();
    const QString origin =
        QString::fromUtf8(request.headers.value("origin")).trimmed();
    const bool privateNetwork =
        request.headers.value("access-control-request-private-network")
            .trimmed()
            .toLower() == "true";

    if (request.method == "OPTIONS") {
        if (!originIsKnown(origin)) {
            respond(socket, 403,
                    {{QStringLiteral("error"),
                      QStringLiteral("origin_not_allowed")}});
            return;
        }
        respondPreflight(socket, origin, privateNetwork);
        return;
    }
    if (origin.isEmpty() || !originIsKnown(origin)) {
        respond(socket, 403,
                {{QStringLiteral("error"),
                  QStringLiteral("origin_not_allowed")}});
        return;
    }

    if (request.method == "POST" && request.target == "/v1/pair") {
        if (!request.body.isEmpty()) {
            respond(socket, 400,
                    {{QStringLiteral("error"),
                      QStringLiteral("pair_body_forbidden")}},
                    origin);
            return;
        }
        const QString supplied = bearer(request);
        const QByteArray suppliedDigest = digest(supplied);
        auto match = m_pairings.end();
        for (auto it = m_pairings.begin(); it != m_pairings.end(); ++it) {
            if (constantTimeEqual(it.key(), suppliedDigest)) {
                match = it;
                break;
            }
        }
        if (supplied.isEmpty() || match == m_pairings.end() ||
            match.value().origin != origin ||
            match.value().deadlineMs <= m_lifetime.elapsed()) {
            respond(socket, 401,
                    {{QStringLiteral("error"),
                      QStringLiteral("invalid_or_expired_pairing")}},
                    origin);
            return;
        }
        m_pairings.erase(match);
        const QString sessionToken = randomSecret();
        const qint64 expires = nowMs() + qint64(kSessionTtlSeconds) * 1000;
        SessionRecord browserSession;
        browserSession.origin = origin;
        browserSession.deadlineMs =
            m_lifetime.elapsed() + qint64(kSessionTtlSeconds) * 1000;
        m_sessions.insert(digest(sessionToken), browserSession);
        emit auditEvent(QStringLiteral("speech_browser_paired origin=%1").arg(origin));
        respond(socket, 200,
                {
                    {QStringLiteral("sessionToken"), sessionToken},
                    {QStringLiteral("expiresAt"),
                     QDateTime::fromMSecsSinceEpoch(expires)
                         .toUTC()
                         .toString(Qt::ISODate)},
                    {QStringLiteral("destinations"),
                     QJsonArray{QStringLiteral("chat"),
                                QStringLiteral("issue"),
                                QStringLiteral("note")}},
                    {QStringLiteral("audioTransport"),
                     QStringLiteral("none-local-capture-only")},
                },
                origin);
        return;
    }

    const QString sessionKey = authenticatedSession(request, origin);
    if (sessionKey.isEmpty()) {
        respond(socket, 401,
                {{QStringLiteral("error"),
                  QStringLiteral("invalid_or_expired_session")}},
                origin);
        return;
    }
    SessionRecord &session = m_sessions[digest(sessionKey)];

    if (request.method == "GET" &&
        request.target == "/v1/transcription") {
        respond(socket, 200,
                {
                    {QStringLiteral("state"), session.state},
                    {QStringLiteral("captureId"), session.captureId},
                    {QStringLiteral("destination"), session.destination},
                    {QStringLiteral("transcript"), session.transcript},
                    {QStringLiteral("error"), session.error},
                    {QStringLiteral("revision"),
                     QString::number(session.revision)},
                    {QStringLiteral("audioRelayed"), false},
                },
                origin);
        return;
    }

    if (request.method != "POST" ||
        (request.target != "/v1/transcription/start" &&
         request.target != "/v1/transcription/stop" &&
         request.target != "/v1/transcription/cancel" &&
         request.target != "/v1/session/revoke")) {
        respond(socket, 404,
                {{QStringLiteral("error"), QStringLiteral("not_found")}},
                origin);
        return;
    }
    if (!consumeRequestId(&session, request)) {
        respond(socket, 409,
                {{QStringLiteral("error"),
                  QStringLiteral("missing_or_replayed_request_id")}},
                origin);
        return;
    }

    if (request.target == "/v1/session/revoke") {
        if (!request.body.isEmpty()) {
            respond(socket, 400,
                    {{QStringLiteral("error"),
                      QStringLiteral("request_body_forbidden")}},
                    origin);
            return;
        }
        session.revoked = true;
        if (!session.captureId.isEmpty() &&
            (session.state == QStringLiteral("recording") ||
             session.state == QStringLiteral("transcribing"))) {
            emit cancelRequested(session.captureId);
        }
        emit auditEvent(QStringLiteral("speech_session_revoked"));
        respond(socket, 200,
                {{QStringLiteral("state"), QStringLiteral("revoked")}},
                origin);
        return;
    }

    if (request.target == "/v1/transcription/start") {
        if (session.state == QStringLiteral("recording") ||
            session.state == QStringLiteral("transcribing")) {
            respond(socket, 409,
                    {{QStringLiteral("error"),
                      QStringLiteral("capture_already_active")}},
                    origin);
            return;
        }
        for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
            if (&it.value() == &session || it.value().revoked)
                continue;
            if (it.value().state == QStringLiteral("recording") ||
                it.value().state == QStringLiteral("transcribing")) {
                respond(socket, 409,
                        {{QStringLiteral("error"),
                          QStringLiteral("desktop_microphone_busy")}},
                        origin);
                return;
            }
        }
        QJsonParseError parseError;
        const QJsonDocument document =
            QJsonDocument::fromJson(request.body, &parseError);
        const QJsonObject object =
            document.isObject() ? document.object() : QJsonObject();
        const QString destination =
            !object.isEmpty()
                ? object.value(QStringLiteral("destination"))
                      .toString()
                      .trimmed()
                      .toLower()
                : QString();
        if (parseError.error != QJsonParseError::NoError ||
            object.size() != 1 ||
            !object.contains(QStringLiteral("destination")) ||
            request.headers.value("content-type")
                    .split(';')
                    .first()
                    .trimmed()
                    .toLower() != "application/json" ||
            !isAllowedDestination(destination)) {
            respond(socket, 400,
                    {{QStringLiteral("error"),
                      QStringLiteral("choose_chat_issue_or_note")}},
                    origin);
            return;
        }
        session.captureId =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        session.destination = destination;
        session.state = QStringLiteral("recording");
        session.transcript.clear();
        session.error.clear();
        ++session.revision;
        emit auditEvent(
            QStringLiteral("speech_capture_started destination=%1")
                .arg(destination));
        emit captureRequested(session.captureId, destination);
        respond(socket, 202,
                {{QStringLiteral("state"), session.state},
                 {QStringLiteral("captureId"), session.captureId},
                 {QStringLiteral("destination"), destination},
                 {QStringLiteral("audioRelayed"), false}},
                origin);
        return;
    }

    if (session.captureId.isEmpty() ||
        (session.state != QStringLiteral("recording") &&
         session.state != QStringLiteral("transcribing"))) {
        respond(socket, 409,
                {{QStringLiteral("error"),
                  QStringLiteral("no_active_capture")}},
                origin);
        return;
    }
    if (!request.body.isEmpty()) {
        respond(socket, 400,
                {{QStringLiteral("error"),
                  QStringLiteral("request_body_forbidden")}},
                origin);
        return;
    }
    if (request.target == "/v1/transcription/cancel") {
        const QString captureId = session.captureId;
        session.state = QStringLiteral("cancelled");
        session.transcript.clear();
        session.error.clear();
        ++session.revision;
        emit cancelRequested(captureId);
        emit auditEvent(QStringLiteral("speech_capture_cancelled"));
    } else {
        session.state = QStringLiteral("transcribing");
        ++session.revision;
        emit stopRequested(session.captureId);
        emit auditEvent(QStringLiteral("speech_capture_stopped"));
    }
    respond(socket, 202,
            {{QStringLiteral("state"), session.state},
             {QStringLiteral("captureId"), session.captureId}},
            origin);
}

void WorldSpeechBridge::respond(QTcpSocket *socket, int status,
                                const QJsonObject &body,
                                const QString &origin) const
{
    if (!socket)
        return;
    const QByteArray payload = QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray response =
        "HTTP/1.1 " + QByteArray::number(status) + " " +
        statusText(status).toUtf8() + "\r\n"
        "Content-Type: application/json; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Content-Security-Policy: default-src 'none'\r\n"
        "X-Content-Type-Options: nosniff\r\n"
        "Referrer-Policy: no-referrer\r\n"
        "Connection: close\r\n";
    if (!origin.isEmpty()) {
        response += "Access-Control-Allow-Origin: " + origin.toUtf8() + "\r\n";
        response += "Vary: Origin\r\n";
    }
    response += "Content-Length: " + QByteArray::number(payload.size()) +
                "\r\n\r\n" + payload;
    socket->write(response);
    socket->disconnectFromHost();
}

void WorldSpeechBridge::respondPreflight(QTcpSocket *socket,
                                         const QString &origin,
                                         bool privateNetwork) const
{
    QByteArray response =
        "HTTP/1.1 204 No Content\r\n"
        "Access-Control-Allow-Origin: " +
        origin.toUtf8() +
        "\r\n"
        "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
        "Access-Control-Allow-Headers: Authorization, Content-Type, "
        "X-ForkMesh-Request-Id\r\n"
        "Access-Control-Max-Age: 120\r\n"
        "Cache-Control: no-store\r\n"
        "Vary: Origin, Access-Control-Request-Private-Network\r\n";
    if (privateNetwork)
        response += "Access-Control-Allow-Private-Network: true\r\n";
    response += "Content-Length: 0\r\nConnection: close\r\n\r\n";
    socket->write(response);
    socket->disconnectFromHost();
}

QString WorldSpeechBridge::authenticatedSession(const Request &request,
                                                const QString &origin) const
{
    const QString supplied = bearer(request);
    if (supplied.isEmpty())
        return {};
    const QByteArray suppliedDigest = digest(supplied);
    for (auto it = m_sessions.cbegin(); it != m_sessions.cend(); ++it) {
        if (constantTimeEqual(it.key(), suppliedDigest) &&
            !it.value().revoked && it.value().origin == origin &&
            it.value().deadlineMs > m_lifetime.elapsed())
            return supplied;
    }
    return {};
}

bool WorldSpeechBridge::consumeRequestId(SessionRecord *session,
                                         const Request &request)
{
    const QString id =
        QString::fromUtf8(request.headers.value("x-forkmesh-request-id"))
            .trimmed();
    if (!session || id.size() < 16 || id.size() > 128 ||
        session->requestIds.contains(id) ||
        !QRegularExpression(QStringLiteral("^[A-Za-z0-9._:-]+$"))
             .match(id)
             .hasMatch())
        return false;
    // Never discard old nonces while a capability is live: clearing the set
    // would make an early mutation replayable. A tab that somehow performs this
    // many mutations during a ten-minute session must pair again.
    if (session->requestIds.size() >= kMaxRequestIds)
        return false;
    session->requestIds.insert(id);
    return true;
}

bool WorldSpeechBridge::originIsKnown(const QString &origin) const
{
    if (origin.isEmpty() || normalizedOrigin(origin) != origin)
        return false;
    for (const PairingRecord &pairing : m_pairings)
        if (pairing.origin == origin &&
            pairing.deadlineMs > m_lifetime.elapsed())
            return true;
    for (const SessionRecord &session : m_sessions)
        if (session.origin == origin && !session.revoked &&
            session.deadlineMs > m_lifetime.elapsed())
            return true;
    return false;
}

void WorldSpeechBridge::purgeExpired()
{
    const qint64 now = m_lifetime.elapsed();
    for (auto it = m_pairings.begin(); it != m_pairings.end();) {
        if (it.value().deadlineMs <= now)
            it = m_pairings.erase(it);
        else
            ++it;
    }
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
        if (it.value().deadlineMs <= now || it.value().revoked) {
            if (!it.value().captureId.isEmpty() &&
                (it.value().state == QStringLiteral("recording") ||
                 it.value().state == QStringLiteral("transcribing")))
                emit cancelRequested(it.value().captureId);
            it = m_sessions.erase(it);
        } else {
            ++it;
        }
    }
}

QString WorldSpeechBridge::normalizedOrigin(const QString &candidate)
{
    const QUrl url(candidate, QUrl::StrictMode);
    if (!url.isValid() ||
        (url.scheme() != QStringLiteral("https") &&
         url.scheme() != QStringLiteral("http")) ||
        url.host().isEmpty() || !url.userInfo().isEmpty() ||
        !url.query().isEmpty() || !url.fragment().isEmpty() ||
        (!url.path().isEmpty() && url.path() != QStringLiteral("/")))
        return {};
    QUrl origin;
    origin.setScheme(url.scheme().toLower());
    origin.setHost(url.host().toLower());
    const int candidatePort = url.port();
    const bool defaultPort =
        (origin.scheme() == QStringLiteral("https") &&
         candidatePort == 443) ||
        (origin.scheme() == QStringLiteral("http") &&
         candidatePort == 80);
    if (candidatePort >= 0 && !defaultPort)
        origin.setPort(candidatePort);
    return origin.toString(QUrl::FullyEncoded | QUrl::RemovePath |
                           QUrl::StripTrailingSlash);
}

QString WorldSpeechBridge::randomSecret()
{
    QByteArray bytes(32, Qt::Uninitialized);
    for (qsizetype offset = 0; offset < bytes.size(); offset += 4) {
        const quint32 value = QRandomGenerator::system()->generate();
        const qsizetype count = qMin<qsizetype>(4, bytes.size() - offset);
        for (qsizetype index = 0; index < count; ++index)
            bytes[offset + index] =
                char((value >> (8 * index)) & 0xff);
    }
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding |
                       QByteArray::OmitTrailingEquals));
}

QByteArray WorldSpeechBridge::digest(const QString &secret)
{
    return QCryptographicHash::hash(secret.toUtf8(),
                                    QCryptographicHash::Sha256);
}

QString WorldSpeechBridge::bearer(const Request &request)
{
    const QByteArray value = request.headers.value("authorization").trimmed();
    constexpr char prefix[] = "Bearer ";
    if (!value.startsWith(prefix))
        return {};
    return QString::fromUtf8(value.mid(sizeof(prefix) - 1)).trimmed();
}

QString WorldSpeechBridge::safeTranscript(const QString &text)
{
    QString clean = text;
    clean.remove(QChar::Null);
    clean.replace(QRegularExpression(QStringLiteral("[\\r\\n\\t]+")),
                  QStringLiteral(" "));
    return clean.simplified().left(8000);
}

QString WorldSpeechBridge::statusText(int status)
{
    switch (status) {
    case 200:
        return QStringLiteral("OK");
    case 202:
        return QStringLiteral("Accepted");
    case 204:
        return QStringLiteral("No Content");
    case 400:
        return QStringLiteral("Bad Request");
    case 401:
        return QStringLiteral("Unauthorized");
    case 403:
        return QStringLiteral("Forbidden");
    case 404:
        return QStringLiteral("Not Found");
    case 409:
        return QStringLiteral("Conflict");
    case 413:
        return QStringLiteral("Payload Too Large");
    case 431:
        return QStringLiteral("Request Header Fields Too Large");
    default:
        return QStringLiteral("Error");
    }
}
