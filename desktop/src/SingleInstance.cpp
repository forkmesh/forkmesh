#include "SingleInstance.h"
#include "AppPaths.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>

namespace forkmesh {

namespace {

// One instance per project: each project has its own lock and its own socket,
// so a second project opens beside the first instead of focusing it.
QString serverName()
{
    return forkmesh::paths::singleInstanceServerName();
}

QLockFile *g_lockFile = nullptr;
QLocalServer *g_server = nullptr;
std::function<void(const QString &)> g_activationHandler;

} // namespace

bool acquireSingleInstance(const QString &activationTarget)
{
    const QString dir = forkmesh::paths::stateRoot();
    QDir().mkpath(dir);

    g_lockFile = new QLockFile(dir + QStringLiteral("/forkmesh.lock"));
    if (!g_lockFile->tryLock()) {
        QLocalSocket socket;
        socket.connectToServer(serverName());
        if (socket.waitForConnected(500)) {
            const QByteArray target = activationTarget.toUtf8();
            socket.write(target.isEmpty() ? QByteArray("activate")
                                          : QByteArray("activate\n") +
                                                target.left(4096));
            socket.waitForBytesWritten(500);
            socket.disconnectFromServer();
        }
        delete g_lockFile;
        g_lockFile = nullptr;
        return false;
    }

    QLocalServer::removeServer(serverName());
    g_server = new QLocalServer();
    g_server->setSocketOptions(QLocalServer::UserAccessOption);
    QObject::connect(g_server, &QLocalServer::newConnection, g_server, [] {
        while (QLocalSocket *socket = g_server->nextPendingConnection()) {
            socket->waitForReadyRead(200);
            const QByteArray message = socket->read(4128);
            socket->deleteLater();
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
    const QString dir = forkmesh::paths::stateRoot();
    QDir().mkpath(dir);
    auto *lock = new QLockFile(dir + QStringLiteral("/forkmesh.lock"));
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

void releaseSingleInstance()
{
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
