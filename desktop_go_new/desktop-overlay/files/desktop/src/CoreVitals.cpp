#include "CoreVitals.h"

#include "AppPaths.h"
#include "CoreClient.h"
#include "SystemStats.h"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QJsonArray>
#include <QMutex>
#include <QMutexLocker>
#include <QPair>
#include <QPointer>
#include <QStringList>

#include <chrono>

namespace forkmesh::corevitals {

namespace {

constexpr qint64 kFreshMs = 3000;

struct State {
    bool valid = false;
    qint64 receivedMs = 0;
    qint64 memoryTotal = -1;
    qint64 memoryAvailable = -1;
    qint64 swapTotal = -1;
    qint64 swapFree = -1;
    double hostCpu = -1;
    QHash<QString, QPair<qint64, qint64>> disks;
    double appCpu = -1;
    qint64 appResident = -1;
    int threads = -1;
    int openFiles = -1;
    int openFileLimit = -1;
};

QMutex &stateMutex()
{
    static QMutex mutex;
    return mutex;
}

State &state()
{
    static State current;
    return current;
}

qint64 steadyMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

State freshState()
{
    QMutexLocker lock(&stateMutex());
    const State &current = state();
    if (!current.valid || steadyMs() - current.receivedMs > kFreshMs)
        return State();
    return current;
}

qint64 number(const QJsonObject &object, const char *key)
{
    const QJsonValue value = object.value(QLatin1String(key));
    return value.isDouble() ? qint64(value.toDouble()) : -1;
}

constexpr int kMaxExtraDisks = 6;

struct Watch {
    QPointer<CoreClient> client;
    QStringList extraDisks;
};

Watch &watch()
{
    static Watch current;
    return current;
}

void request(CoreClient *client)
{
    QJsonArray disks{QDir::homePath(), forkmesh::paths::dataRoot()};
    {
        QMutexLocker lock(&stateMutex());
        for (const QString &path : std::as_const(watch().extraDisks))
            if (!disks.contains(path))
                disks.append(path);
    }
    client->request(QStringLiteral("core.vitals"),
                    {{QStringLiteral("pid"), double(QCoreApplication::applicationPid())},
                     {QStringLiteral("watch"), true},
                     {QStringLiteral("detail"), true},
                     {QStringLiteral("disks"), disks}},
                    [](bool ok, const QJsonValue &result, const QString &) {
                        const QJsonObject detail = result.toObject().value(QStringLiteral("detail")).toObject();
                        if (ok && !detail.isEmpty())
                            apply(detail);
                    });
}

}

void apply(const QJsonObject &sample)
{
    State next;
    next.valid = true;
    next.receivedMs = steadyMs();
    next.memoryTotal = number(sample, "memoryTotal");
    next.memoryAvailable = number(sample, "memoryAvailable");
    next.swapTotal = number(sample, "swapTotal");
    next.swapFree = number(sample, "swapFree");
    next.hostCpu = sample.value(QStringLiteral("cpu")).toDouble(-1);
    for (const QJsonValue &entry : sample.value(QStringLiteral("disks")).toArray()) {
        const QJsonObject disk = entry.toObject();
        const QString path = disk.value(QStringLiteral("path")).toString();
        if (!path.isEmpty())
            next.disks.insert(path, {number(disk, "total"), number(disk, "free")});
    }
    const QJsonObject app = sample.value(QStringLiteral("app")).toObject();
    if (qint64(app.value(QStringLiteral("pid")).toDouble()) == QCoreApplication::applicationPid()) {
        next.appCpu = app.value(QStringLiteral("cpuPercent")).toDouble(-1);
        next.appResident = number(app, "resident");
        next.threads = int(number(app, "threads"));
        next.openFiles = int(number(app, "openFiles"));
        next.openFileLimit = int(number(app, "openFileLimit"));
    }
    QMutexLocker lock(&stateMutex());
    state() = next;
}

void reset()
{
    QMutexLocker lock(&stateMutex());
    state() = State();
}

bool fresh()
{
    return freshState().valid;
}

void follow(CoreClient *client)
{
    if (!client)
        return;
    {
        QMutexLocker lock(&stateMutex());
        watch().client = client;
    }
    QObject::connect(client, &CoreClient::connectedChanged, client, [client](bool connected) {
        if (connected)
            request(client);
        else
            reset();
    });
    QObject::connect(client, &CoreClient::coreEvent, client,
                     [](const QString &event, const QJsonObject &data) {
                         if (event == QLatin1String("core.vitals.sample"))
                             apply(data);
                     });
    QObject::connect(client, &QObject::destroyed, [] { reset(); });
    if (client->isConnected())
        request(client);
}

qint64 totalMemoryBytes()
{
    const State s = freshState();
    return s.memoryTotal > 0 ? s.memoryTotal : SystemStats::totalMemoryBytes();
}

qint64 availableMemoryBytes()
{
    const State s = freshState();
    return s.memoryTotal > 0 && s.memoryAvailable >= 0 ? s.memoryAvailable
                                                       : SystemStats::availableMemoryBytes();
}

qint64 totalSwapBytes()
{
    const State s = freshState();
    return s.valid && s.swapTotal >= 0 ? s.swapTotal : SystemStats::totalSwapBytes();
}

qint64 freeSwapBytes()
{
    const State s = freshState();
    return s.valid && s.swapFree >= 0 ? s.swapFree : SystemStats::freeSwapBytes();
}

static void sampleDisk(const QString &path, const State &s)
{
    if (!s.valid || path.isEmpty() || !QDir::isAbsolutePath(path) || s.disks.contains(path))
        return;
    QPointer<CoreClient> client;
    {
        QMutexLocker lock(&stateMutex());
        Watch &current = watch();
        if (current.extraDisks.contains(path) || !current.client)
            return;
        current.extraDisks.append(path);
        while (current.extraDisks.size() > kMaxExtraDisks)
            current.extraDisks.removeFirst();
        client = current.client;
    }
    QMetaObject::invokeMethod(
        client.data(),
        [client] {
            if (client && client->isConnected())
                request(client);
        },
        Qt::QueuedConnection);
}

qint64 diskTotalBytes(const QString &path)
{
    const State s = freshState();
    sampleDisk(path, s);
    const auto found = s.disks.constFind(path);
    return found != s.disks.constEnd() && found->first >= 0 ? found->first
                                                            : SystemStats::diskTotalBytes(path);
}

qint64 diskFreeBytes(const QString &path)
{
    const State s = freshState();
    sampleDisk(path, s);
    const auto found = s.disks.constFind(path);
    return found != s.disks.constEnd() && found->second >= 0 ? found->second
                                                             : SystemStats::diskFreeBytes(path);
}

double hostCpuPercent()
{
    const State s = freshState();
    return s.valid ? s.hostCpu : SystemStats::hostCpuPercent();
}

bool appCpuPercent(double *percent)
{
    const State s = freshState();
    if (!s.valid || s.appResident < 0)
        return false;
    if (percent)
        *percent = s.appCpu;
    return true;
}

qint64 residentBytes()
{
    const State s = freshState();
    return s.appResident >= 0 ? s.appResident : SystemStats::residentBytes();
}

int threadCount()
{
    const State s = freshState();
    return s.threads > 0 ? s.threads : SystemStats::threadCount();
}

int openFileCount()
{
    const State s = freshState();
    return s.openFiles >= 0 ? s.openFiles + 1 : SystemStats::openFileCount();
}

int openFileSoftLimit()
{
    const State s = freshState();
    return s.openFileLimit >= 0 ? s.openFileLimit : SystemStats::openFileSoftLimit();
}

}
