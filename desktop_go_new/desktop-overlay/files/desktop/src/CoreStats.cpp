#include "CoreStats.h"

#include "CoreClient.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <utility>

namespace forkmesh::corestats {

namespace {

constexpr int kFlushDelayMs = 50;
constexpr int kMaxBatchLines = 256;

struct PendingLines {
    QPointer<CoreClient> client;
    QJsonArray lines;
    bool armed = false;
};

PendingLines &pendingLines()
{
    static PendingLines pending;
    return pending;
}

QString appendMethod()
{
    return QStringLiteral("core.log.append");
}

}

void flushLogLines()
{
    PendingLines &pending = pendingLines();
    pending.armed = false;
    const QJsonArray lines = std::exchange(pending.lines, QJsonArray());
    if (lines.isEmpty() || !pending.client || !pending.client->isConnected())
        return;
    pending.client->notify(appendMethod(), {{QStringLiteral("lines"), lines}});
}

void noteLogLine(CoreClient *client, const QString &storedLine, const QString &badge)
{
    if (!client || !client->isConnected() || !client->hasNativeRoute(appendMethod()))
        return;
    PendingLines &pending = pendingLines();
    if (pending.client != client) {
        pending.lines = QJsonArray();
        pending.client = client;
    }
    pending.lines.append(QJsonObject{{QStringLiteral("line"), storedLine},
                                     {QStringLiteral("badge"), badge}});
    if (pending.lines.size() >= kMaxBatchLines) {
        flushLogLines();
        return;
    }
    if (pending.armed)
        return;
    pending.armed = true;
    QTimer::singleShot(kFlushDelayMs, client, [] { flushLogLines(); });
}

} // namespace forkmesh::corestats
