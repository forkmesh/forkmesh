#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QObject>
#include <QSet>
#include <QString>

class QTcpServer;
class QTcpSocket;
class QTimer;

// A deliberately small HTTP control bridge between the hosted ForkMesh World
// and the desktop client's existing local speech-to-text pipeline.
// Security boundary:
//   * the listener binds only to an OS loopback address;
//   * a short-lived, one-use pairing capability is exchanged in an
//     Authorization header, never a URL;
//   * every capability is bound to one exact http(s) Origin;
//   * mutating calls require a one-use request id;
//   * the protocol has no request or response field capable of carrying audio.
// The browser asks the desktop client to start/stop its local microphone and
// polls for transcript text. MainWindow owns capture/transcription and publishes
// status and text through the slots below.
class WorldSpeechBridge final : public QObject
{
    Q_OBJECT
public:
    explicit WorldSpeechBridge(QObject *parent = nullptr);
    ~WorldSpeechBridge() override;

    bool start();
    void stop();
    bool isListening() const;
    quint16 port() const;

    // Returns {secret, origin, expiresAt, port}. The plaintext secret exists only
    // in this return value; the bridge retains a SHA-256 digest. Invalid origins
    // produce an empty object.
    QJsonObject issuePairing(const QString &exactOrigin, int ttlSeconds = 120);
    void revokeAll();

    void publishPartial(const QString &captureId, const QString &text);
    void publishFinal(const QString &captureId, const QString &text);
    void publishError(const QString &captureId, const QString &safeMessage);

signals:
    void captureRequested(const QString &captureId, const QString &destination);
    void stopRequested(const QString &captureId);
    void cancelRequested(const QString &captureId);
    void auditEvent(const QString &event);

private slots:
    void onNewConnection();

private:
    struct PairingRecord {
        QString origin;
        qint64 deadlineMs = 0;
    };

    struct SessionRecord {
        QString origin;
        qint64 deadlineMs = 0;
        bool revoked = false;
        QSet<QString> requestIds;
        QString captureId;
        QString destination;
        QString state = QStringLiteral("idle");
        QString transcript;
        QString error;
        quint64 revision = 0;
    };

    struct Request {
        QByteArray method;
        QByteArray target;
        QHash<QByteArray, QByteArray> headers;
        QByteArray body;
    };

    void onReadyRead(QTcpSocket *socket);
    void onDisconnected(QTcpSocket *socket);
    bool parseRequest(const QByteArray &raw, Request *request,
                      QByteArray *error) const;
    void dispatch(QTcpSocket *socket, const Request &request);
    void respond(QTcpSocket *socket, int status, const QJsonObject &body,
                 const QString &origin = QString()) const;
    void respondPreflight(QTcpSocket *socket, const QString &origin,
                          bool privateNetwork) const;

    QString authenticatedSession(const Request &request,
                                 const QString &origin) const;
    bool consumeRequestId(SessionRecord *session, const Request &request);
    bool originIsKnown(const QString &origin) const;
    void purgeExpired();
    static QString normalizedOrigin(const QString &candidate);
    static QString randomSecret();
    static QByteArray digest(const QString &secret);
    static QString bearer(const Request &request);
    static QString safeTranscript(const QString &text);
    static QString statusText(int status);

    QTcpServer *m_server = nullptr;
    QTimer *m_expiryTimer = nullptr;
    QElapsedTimer m_lifetime;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    QHash<QByteArray, PairingRecord> m_pairings;
    QHash<QByteArray, SessionRecord> m_sessions;
};
