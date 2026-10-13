#include "BackgroundFileWriter.h"

#include <QDeadlineTimer>
#include <QDebug>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QSaveFile>
#include <QStringList>
#include <QThreadPool>
#include <QWaitCondition>

namespace forkmesh::diskwrite {

namespace {

struct Queue {
    QMutex mutex;
    QWaitCondition changed;
    QHash<QString, QByteArray> pending;
    QStringList order;
    QString inFlight;
    bool draining = false;
};

Queue &queue()
{
    static Queue instance;
    return instance;
}

QThreadPool &pool()
{
    static QThreadPool instance;
    return instance;
}

bool commit(const QString &path, const QByteArray &bytes, QString *error)
{
    const QString directory = QFileInfo(path).absolutePath();
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QStringLiteral("cannot create %1").arg(directory);
        return false;
    }
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error)
            *error = QStringLiteral("cannot write %1").arg(path);
        return false;
    }
    file.write(bytes);
    if (!file.commit()) {
        if (error)
            *error = QStringLiteral("cannot commit %1").arg(path);
        return false;
    }
    return true;
}

void drain()
{
    Queue &q = queue();
    for (;;) {
        QString path;
        QByteArray bytes;
        {
            QMutexLocker locker(&q.mutex);
            q.inFlight.clear();
            q.changed.wakeAll();
            if (q.order.isEmpty()) {
                q.draining = false;
                q.changed.wakeAll();
                return;
            }
            path = q.order.takeFirst();
            bytes = q.pending.take(path);
            q.inFlight = path;
        }
        if (QString error; !commit(path, bytes, &error))
            qWarning() << "background file write:" << error;
    }
}

} // namespace

void writeLater(const QString &path, const QByteArray &bytes)
{
    if (path.isEmpty())
        return;
    Queue &q = queue();
    QMutexLocker locker(&q.mutex);
    if (!q.pending.contains(path))
        q.order.append(path);
    q.pending.insert(path, bytes);
    if (q.draining)
        return;
    q.draining = true;
    pool().start(drain);
}

bool writeNow(const QString &path, const QByteArray &bytes, QString *error)
{
    if (path.isEmpty()) {
        if (error)
            *error = QStringLiteral("no path");
        return false;
    }
    Queue &q = queue();
    {
        QMutexLocker locker(&q.mutex);
        if (q.pending.remove(path))
            q.order.removeOne(path);
        while (q.inFlight == path)
            q.changed.wait(&q.mutex);
    }
    return commit(path, bytes, error);
}

bool flush(int timeoutMs)
{
    Queue &q = queue();
    const QDeadlineTimer deadline =
        timeoutMs < 0 ? QDeadlineTimer(QDeadlineTimer::Forever)
                      : QDeadlineTimer(timeoutMs);
    QMutexLocker locker(&q.mutex);
    while (q.draining) {
        if (!q.changed.wait(&q.mutex, deadline))
            return false;
    }
    return true;
}

} // namespace forkmesh::diskwrite
