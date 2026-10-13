#include "WorldSpeechBridge.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QTimer>

#include <cstdlib>
#include <iostream>

namespace {
int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

struct HttpResponse {
    int status = 0;
    QByteArray raw;
    QJsonObject json;
};

HttpResponse request(quint16 port, const QByteArray &method,
                     const QByteArray &target, const QString &origin,
                     const QString &bearer = QString(),
                     const QString &requestId = QString(),
                     const QJsonObject &body = {})
{
    QTcpSocket socket;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop,
                     &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::readyRead, &loop, [&] {
        // The server closes every response, so disconnected normally ends the
        // loop. This guard also handles a complete response on platforms where
        // close delivery is deferred.
        if (socket.bytesAvailable() > 0 &&
            socket.peek(socket.bytesAvailable()).contains("\r\n\r\n"))
            QTimer::singleShot(20, &loop, &QEventLoop::quit);
    });
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(1000))
        return {};

    const QByteArray payload =
        body.isEmpty() ? QByteArray()
                       : QJsonDocument(body).toJson(QJsonDocument::Compact);
    QByteArray wire = method + " " + target + " HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Origin: " +
                      origin.toUtf8() + "\r\n"
                      "Connection: close\r\n";
    if (!bearer.isEmpty())
        wire += "Authorization: Bearer " + bearer.toUtf8() + "\r\n";
    if (!requestId.isEmpty())
        wire += "X-ForkMesh-Request-Id: " + requestId.toUtf8() + "\r\n";
    if (!payload.isEmpty())
        wire += "Content-Type: application/json\r\n";
    wire += "Content-Length: " + QByteArray::number(payload.size()) +
            "\r\n\r\n" + payload;
    socket.write(wire);
    socket.flush();
    timeout.start(2000);
    loop.exec();

    HttpResponse response;
    response.raw = socket.readAll();
    const QList<QByteArray> firstLine =
        response.raw.left(response.raw.indexOf("\r\n")).split(' ');
    if (firstLine.size() >= 2)
        response.status = firstLine.at(1).toInt();
    const int bodyStart = response.raw.indexOf("\r\n\r\n");
    if (bodyStart >= 0) {
        const QJsonDocument document =
            QJsonDocument::fromJson(response.raw.mid(bodyStart + 4));
        if (document.isObject())
            response.json = document.object();
    }
    return response;
}

HttpResponse preflight(quint16 port, const QString &origin)
{
    QTcpSocket socket;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    QObject::connect(&socket, &QTcpSocket::disconnected, &loop,
                     &QEventLoop::quit);
    socket.connectToHost(QHostAddress::LocalHost, port);
    if (!socket.waitForConnected(1000))
        return {};
    const QByteArray wire =
        "OPTIONS /v1/pair HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Origin: " +
        origin.toUtf8() +
        "\r\n"
        "Access-Control-Request-Method: POST\r\n"
        "Access-Control-Request-Private-Network: true\r\n"
        "Content-Length: 0\r\nConnection: close\r\n\r\n";
    socket.write(wire);
    socket.flush();
    timeout.start(2000);
    loop.exec();
    HttpResponse response;
    response.raw = socket.readAll();
    const QList<QByteArray> firstLine =
        response.raw.left(response.raw.indexOf("\r\n")).split(' ');
    if (firstLine.size() >= 2)
        response.status = firstLine.at(1).toInt();
    return response;
}

void waitMs(int milliseconds)
{
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}
} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    WorldSpeechBridge bridge;
    check(bridge.start(), "bridge starts");
    check(bridge.isListening(), "bridge is loopback-listening");
    check(bridge.port() != 0, "bridge uses an ephemeral port");

    const QString origin = QStringLiteral("https://world.forkmesh.example");
    check(bridge.issuePairing(QStringLiteral("*")).isEmpty(),
          "wildcard origin is rejected");
    check(bridge.issuePairing(origin + QStringLiteral("/private")).isEmpty(),
          "origin paths are rejected");
    check(bridge.issuePairing(origin + QStringLiteral("?token=no")).isEmpty(),
          "origin queries are rejected");

    const QJsonObject pairing = bridge.issuePairing(origin, 120);
    const QString pairSecret =
        pairing.value(QStringLiteral("secret")).toString();
    check(pairSecret.size() >= 40, "pairing secret has sufficient entropy");
    check(pairing.value(QStringLiteral("port")).toInt() == bridge.port(),
          "pairing advertises the bound port");

    HttpResponse cors = preflight(bridge.port(), origin);
    check(cors.status == 204, "known exact origin receives CORS preflight");
    check(cors.raw.contains("Access-Control-Allow-Origin: " + origin.toUtf8()),
          "CORS echoes the exact known origin");
    check(cors.raw.contains("Access-Control-Allow-Private-Network: true"),
          "private-network preflight is explicit");
    check(preflight(bridge.port(),
                    QStringLiteral("https://evil.example"))
              .status == 403,
          "unknown origin preflight is rejected");

    check(request(bridge.port(), "POST", "/v1/pair",
                  QStringLiteral("https://evil.example"), pairSecret)
              .status == 403,
          "pairing is exact-origin bound");

    HttpResponse paired =
        request(bridge.port(), "POST", "/v1/pair", origin, pairSecret);
    check(paired.status == 200, "valid pairing succeeds");
    const QString session =
        paired.json.value(QStringLiteral("sessionToken")).toString();
    check(session.size() >= 40, "session capability has sufficient entropy");
    check(paired.json.value(QStringLiteral("audioTransport")).toString() ==
              QStringLiteral("none-local-capture-only"),
          "pair response promises no audio transport");
    check(request(bridge.port(), "POST", "/v1/pair", origin, pairSecret)
              .status == 401,
          "pairing capability cannot be replayed");

    QString requestedCapture;
    QString requestedDestination;
    QString stoppedCapture;
    QString cancelledCapture;
    QObject::connect(&bridge, &WorldSpeechBridge::captureRequested, &bridge,
                     [&](const QString &id, const QString &destination) {
                         requestedCapture = id;
                         requestedDestination = destination;
                     });
    QObject::connect(&bridge, &WorldSpeechBridge::stopRequested, &bridge,
                     [&](const QString &id) { stoppedCapture = id; });
    QObject::connect(&bridge, &WorldSpeechBridge::cancelRequested, &bridge,
                     [&](const QString &id) { cancelledCapture = id; });

    check(request(bridge.port(), "POST", "/v1/transcription/start", origin,
                  session, QString(),
                  {{QStringLiteral("destination"), QStringLiteral("chat")}})
              .status == 409,
          "mutation without request id is rejected");
    check(request(bridge.port(), "POST", "/v1/transcription/start", origin,
                  session, QStringLiteral("start-0000000001"),
                  {{QStringLiteral("destination"),
                    QStringLiteral("undecided")}})
              .status == 400,
          "capture requires a user-selected composer");

    HttpResponse started =
        request(bridge.port(), "POST", "/v1/transcription/start", origin,
                session, QStringLiteral("start-0000000002"),
                {{QStringLiteral("destination"), QStringLiteral("issue")}});
    check(started.status == 202, "capture starts");
    check(!requestedCapture.isEmpty() &&
              requestedCapture ==
                  started.json.value(QStringLiteral("captureId")).toString(),
          "local capture signal carries the opaque id");
    check(requestedDestination == QStringLiteral("issue"),
          "capture signal carries the chosen destination");
    check(!started.json.value(QStringLiteral("audioRelayed")).toBool(),
          "start confirms audio is not relayed");

    check(request(bridge.port(), "POST", "/v1/transcription/stop", origin,
                  session, QStringLiteral("start-0000000002"))
              .status == 409,
          "mutation request ids cannot be replayed");
    check(request(bridge.port(), "POST", "/v1/transcription/stop", origin,
                  session, QStringLiteral("stop-body-0000001"),
                  {{QStringLiteral("audio"), QStringLiteral("forbidden")}})
              .status == 400,
          "stop endpoint rejects every request body including audio-shaped data");
    check(request(bridge.port(), "POST", "/v1/transcription/stop", origin,
                  session, QStringLiteral("stop-00000000001"))
              .status == 202,
          "explicit stop begins transcription");
    check(stoppedCapture == requestedCapture,
          "stop signal references the active local capture");

    bridge.publishPartial(requestedCapture, QStringLiteral("draft words"));
    bridge.publishFinal(requestedCapture, QStringLiteral("final words"));
    HttpResponse transcript =
        request(bridge.port(), "GET", "/v1/transcription", origin, session);
    check(transcript.status == 200, "authenticated status poll succeeds");
    check(transcript.json.value(QStringLiteral("state")).toString() ==
              QStringLiteral("complete"),
          "final transcript reaches complete state");
    check(transcript.json.value(QStringLiteral("transcript")).toString() ==
              QStringLiteral("final words"),
          "only transcript text returns to the browser");
    check(!transcript.json.value(QStringLiteral("audioRelayed")).toBool(),
          "status confirms no audio relay");

    HttpResponse restarted =
        request(bridge.port(), "POST", "/v1/transcription/start", origin,
                session, QStringLiteral("start-0000000003"),
                {{QStringLiteral("destination"), QStringLiteral("note")}});
    check(restarted.status == 202, "a completed session can capture again");
    const QString secondCapture =
        restarted.json.value(QStringLiteral("captureId")).toString();
    check(request(bridge.port(), "POST", "/v1/transcription/cancel", origin,
                  session, QStringLiteral("cancel-000000001"))
              .status == 202,
          "capture can be cancelled explicitly");
    check(cancelledCapture == secondCapture,
          "cancel signal references the active local capture");
    transcript =
        request(bridge.port(), "GET", "/v1/transcription", origin, session);
    check(transcript.json.value(QStringLiteral("state")).toString() ==
              QStringLiteral("cancelled"),
          "cancelled state is visible");
    check(transcript.json.value(QStringLiteral("transcript")).toString().isEmpty(),
          "cancellation does not return partial text");

    check(request(bridge.port(), "POST", "/v1/audio", origin, session,
                  QStringLiteral("audio-0000000001"))
              .status == 404,
          "there is no audio transport endpoint");
    check(request(bridge.port(), "GET", "/v1/transcription?secret=no",
                  origin, session)
              .status == 400,
          "capabilities and state cannot be addressed through query strings");

    check(request(bridge.port(), "POST", "/v1/session/revoke", origin,
                  session, QStringLiteral("revoke-000000001"))
              .status == 200,
          "session can be revoked");
    check(request(bridge.port(), "GET", "/v1/transcription", origin, session)
              .status == 403,
          "revoked origin is no longer CORS-authorized");

    const QJsonObject expiring = bridge.issuePairing(origin, 1);
    waitMs(1100);
    check(request(bridge.port(), "POST", "/v1/pair", origin,
                  expiring.value(QStringLiteral("secret")).toString())
              .status == 403,
          "expired pairing also removes origin authorization");

    bridge.stop();
    check(!bridge.isListening(), "stop closes the listener");
    if (failures == 0)
        std::cout << "World speech bridge tests passed\n";
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
