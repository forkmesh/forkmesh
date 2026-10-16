#include "SingleInstance.h"
#include "AppPaths.h"

#include <QDebug>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QSaveFile>
#include <QTimer>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>

namespace forkmesh {

namespace {

// One instance per project: each project has its own lock and its own socket,
// so a second project opens beside the first instead of focusing it.
QString serverName()
{
    return forkmesh::paths::singleInstanceServerName();
}

constexpr int kYieldWaitMs = 45000;
constexpr int kYieldPollMs = 150;
constexpr int kYieldExitDeadlineMs = 25000;
const QByteArray kYieldReply("yield\n");

QLockFile *g_lockFile = nullptr;
QLocalServer *g_server = nullptr;
std::function<void(const QString &)> g_activationHandler;
std::function<void()> g_yieldHandler;
bool g_background = false;
bool g_tookOverFromBackground = false;
int g_backgroundLockWaitMs = 0;
std::atomic_bool g_quitRequested{false};

bool askRunningInstance(const QString &activationTarget)
{
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(500))
        return false;
    const QByteArray target = activationTarget.toUtf8();
    socket.write(target.isEmpty() ? QByteArray("activate")
                                  : QByteArray("activate\n") + target.left(4096));
    socket.waitForBytesWritten(500);
    QByteArray reply = socket.readAll();
    while (reply.size() < kYieldReply.size() && socket.waitForReadyRead(1500))
        reply += socket.readAll();
    reply += socket.readAll();
    socket.disconnectFromServer();
    return reply.startsWith(kYieldReply);
}

bool waitForYieldedLock(const std::function<void()> &whileWaiting)
{
    QElapsedTimer waited;
    waited.start();
    while (waited.elapsed() < kYieldWaitMs) {
        if (whileWaiting)
            whileWaiting();
        if (g_lockFile->tryLock(kYieldPollMs))
            return true;
    }
    return false;
}

void writeBackgroundMarker()
{
    QSaveFile marker(backgroundInstanceMarkerPath());
    if (!marker.open(QIODevice::WriteOnly))
        return;
    marker.write(QByteArray::number(QCoreApplication::applicationPid()) + '\n');
    marker.commit();
}

} // namespace

void setBackgroundInstance()
{
    g_background = true;
}

void setBackgroundLockWait(int milliseconds)
{
    g_backgroundLockWaitMs = qMax(0, milliseconds);
}

bool singleInstanceHeld()
{
    return g_lockFile && g_lockFile->isLocked();
}

bool askBackgroundInstanceToQuit()
{
    QLocalSocket socket;
    socket.connectToServer(serverName());
    if (!socket.waitForConnected(500))
        return false;
    socket.write("quit");
    socket.waitForBytesWritten(500);
    QByteArray reply;
    while (reply.size() < kYieldReply.size() && socket.waitForReadyRead(1500))
        reply += socket.readAll();
    socket.disconnectFromServer();
    return reply.startsWith(kYieldReply);
}

bool isBackgroundInstance()
{
    return g_background;
}

bool tookOverFromBackgroundInstance()
{
    return g_tookOverFromBackground;
}

void onSingleInstanceYield(std::function<void()> handler)
{
    g_yieldHandler = std::move(handler);
    if (g_quitRequested && g_yieldHandler)
        QTimer::singleShot(0, g_yieldHandler);
}

void requestBackgroundQuit()
{
    if (!g_background || g_quitRequested.exchange(true))
        return;
    std::thread([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(kYieldExitDeadlineMs));
        std::_Exit(0);
    }).detach();
    QTimer::singleShot(0, [] {
        if (g_yieldHandler)
            g_yieldHandler();
    });
}

QString backgroundInstanceMarkerPath()
{
    return forkmesh::paths::stateRoot() + QStringLiteral("/forkmesh-background.pid");
}

qint64 backgroundInstancePid()
{
    QFile marker(backgroundInstanceMarkerPath());
    if (!marker.open(QIODevice::ReadOnly))
        return 0;
    return marker.readLine(32).trimmed().toLongLong();
}

bool acquireSingleInstance(const QString &activationTarget,
                           const std::function<void()> &whileWaiting)
{
    QDir().mkpath(forkmesh::paths::stateRoot());

    g_lockFile = new QLockFile(singleInstanceLockPath());
    if (!g_lockFile->tryLock(g_background ? g_backgroundLockWaitMs : 0)) {
        const bool yielded = !g_background && askRunningInstance(activationTarget);
        if (!yielded || !waitForYieldedLock(whileWaiting)) {
            delete g_lockFile;
            g_lockFile = nullptr;
            return false;
        }
        g_tookOverFromBackground = true;
    }
    if (g_background)
        writeBackgroundMarker();
    else if (backgroundInstancePid() > 0)
        QFile::remove(backgroundInstanceMarkerPath());

    QLocalServer::removeServer(serverName());
    g_server = new QLocalServer();
    g_server->setSocketOptions(QLocalServer::UserAccessOption);
    QObject::connect(g_server, &QLocalServer::newConnection, g_server, [] {
        while (QLocalSocket *socket = g_server->nextPendingConnection()) {
            socket->waitForReadyRead(200);
            const QByteArray message = socket->read(4128);
            if (g_background) {
                if (message.startsWith("activate") || message.startsWith("quit")) {
                    socket->write(kYieldReply);
                    socket->waitForBytesWritten(500);
                    requestBackgroundQuit();
                }
                socket->deleteLater();
                continue;
            }
            socket->deleteLater();
            if (message.startsWith("quit"))
                continue;
            if (g_activationHandler) {
                const QByteArray prefix("activate\n");
                const QString target =
                    message.startsWith(prefix)
                        ? QString::fromUtf8(message.mid(prefix.size()))
                        : QString();
                g_activationHandler(target);
            }
        }
    });
    if (!g_server->listen(serverName())) {
        qWarning().noquote() << "ForkMesh: single-instance activation socket"
                                 " unavailable:"
                              << g_server->errorString();
    }
    return true;
}

void onSingleInstanceActivation(
    std::function<void(const QString &activationTarget)> handler)
{
    g_activationHandler = std::move(handler);
}

void rehomeSingleInstanceLock()
{
    if (!g_lockFile)
        return;
    const QString legacyLock = forkmesh::paths::adoptProjectStateRoot();
    if (legacyLock.isEmpty())
        return;
    QDir().mkpath(forkmesh::paths::stateRoot());
    auto *lock = new QLockFile(singleInstanceLockPath());
    if (!lock->tryLock()) {
        // Someone else already owns the project lock; keep the one we hold.
        delete lock;
        return;
    }
    g_lockFile->unlock();
    delete g_lockFile;
    g_lockFile = lock;
    QFile::remove(legacyLock);
    // Gone only when empty: anything unknown left in ~/.forkmesh stays, and
    // shows up on the storage map.
    QDir().rmdir(QFileInfo(legacyLock).absolutePath());
}

QString singleInstanceLockPath()
{
    return forkmesh::paths::stateRoot() + QStringLiteral("/forkmesh.lock");
}

void releaseSingleInstance()
{
    if (g_background && backgroundInstancePid() == QCoreApplication::applicationPid())
        QFile::remove(backgroundInstanceMarkerPath());
    if (g_server) {
        g_server->close();
        g_server->deleteLater();
        g_server = nullptr;
    }
    if (g_lockFile) {
        g_lockFile->unlock();
        delete g_lockFile;
        g_lockFile = nullptr;
    }
}

} // namespace forkmesh
