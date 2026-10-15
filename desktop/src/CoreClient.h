#pragma once

#include <QByteArray>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>

#include <functional>

class QIODevice;
class QTcpSocket;
class QTimer;

namespace forkmesh::core {

struct Endpoint {
    qint64 pid = 0;
    QString version;
    QString project;
    QString unixPath;
    QString tcp;
    QString token;
    qint64 startedMs = 0;
    int protocol = 0;
    bool valid() const { return !token.isEmpty() && (!unixPath.isEmpty() || !tcp.isEmpty()); }
};

Endpoint readEndpoint(const QString &stateDirectory);
QString defaultStateDirectory();
QString defaultBinaryPath();
bool disabledByEnvironment();
QByteArray encodeLine(const QJsonObject &object);

} // namespace forkmesh::core

class CoreClient : public QObject
{
    Q_OBJECT
public:
    using Reply = std::function<void(bool ok, const QJsonValue &result, const QString &error)>;
    using Responder = std::function<void(const QJsonObject &result, const QString &error)>;
    using Handler = std::function<void(const QJsonObject &params, const Responder &respond)>;

    explicit CoreClient(QObject *parent = nullptr);
    ~CoreClient() override;

    void setProject(const QString &slug) { m_project = slug; }
    void setStateDirectory(const QString &dir) { m_stateDir = dir; }
    void setBinaryPath(const QString &path) { m_binary = path; }
    void setAutoLaunch(bool launch) { m_autoLaunch = launch; }
    void setHeadless(bool headless) { m_headless = headless; }
    QString binaryPath() const { return m_binary; }
    QString stateDirectory() const { return m_stateDir; }

    void start();
    void stop();
    bool isConnected() const { return m_connected; }
    QString version() const { return m_endpoint.version; }
    qint64 pid() const { return m_endpoint.pid; }
    QString lastError() const { return m_lastError; }
    bool hasNativeRoute(const QString &method) const;
    QStringList nativeRoutes() const;

    int request(const QString &method, const QJsonObject &params = {},
                Reply reply = {}, int timeoutMs = 120000);
    bool cancel(int id);
    int protocol() const { return m_protocol; }
    bool supports(const QString &feature) const { return m_features.contains(feature); }
    bool notify(const QString &method, const QJsonObject &params = {});
    void handle(const QString &method, Handler handler);

    bool adoptControlSocket(QTcpSocket *socket, const QByteArray &hello,
                            const QByteArray &rest);

signals:
    void connectedChanged(bool connected);
    void coreEvent(const QString &event, const QJsonObject &data);
    void progress(int id, const QJsonValue &data);
    void logLine(const QString &text);

private:
    struct Pending {
        Reply reply;
        qint64 deadlineMs = 0;
    };

    void scheduleConnect(int delayMs);
    void tryConnect();
    void launch();
    void openDevice();
    void onDeviceConnected();
    void onReadyRead();
    void onDisconnected();
    void handleLine(const QByteArray &line);
    void finishHello(const QJsonObject &result);
    void failAll(const QString &error);
    void sweep();
    bool write(const QJsonObject &object);
    bool binaryNewerThanCore() const;
    static bool blocksUpgrade(const QJsonArray &busy);
    void restartForUpgrade();
    void checkDeferredUpgrade();
    bool adoptByDescriptor(QTcpSocket *socket, const QByteArray &line);
    bool adoptByPipe(QTcpSocket *socket, const QByteArray &line);

    QString m_project;
    QString m_stateDir;
    QString m_binary;
    bool m_autoLaunch = true;
    bool m_headless = false;
    bool m_running = false;
    bool m_connected = false;
    bool m_helloSent = false;
    bool m_restarting = false;
    bool m_upgradeDeferred = false;
    QTimer *m_upgradeTimer = nullptr;
    qint64 m_lastLaunchMs = 0;
    int m_attempts = 0;
    forkmesh::core::Endpoint m_endpoint;
    QIODevice *m_device = nullptr;
    QByteArray m_buffer;
    int m_nextId = 1;
    QHash<int, Pending> m_pending;
    QHash<QString, Handler> m_handlers;
    QSet<QString> m_exact;
    QStringList m_families;
    QTimer *m_retry = nullptr;
    QTimer *m_sweep = nullptr;
    QString m_lastError;
    int m_protocol = 0;
    QSet<QString> m_features;
};
