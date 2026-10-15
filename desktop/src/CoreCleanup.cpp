#include "CoreCleanup.h"

#include "CoreClient.h"

#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QUuid>

#include <memory>

namespace forkmesh::corecleanup {
namespace {

namespace cf = forkmesh::cleanupfeed;

bool attached(CoreClient *client, const QString &method)
{
    return client && client->isConnected() && client->hasNativeRoute(method);
}

bool carries(const QJsonObject &data, const QString &token)
{
    for (const QJsonValue &value : data.value(QStringLiteral("tokens")).toArray()) {
        if (value.toString() == token)
            return true;
    }
    return false;
}

struct Watch {
    QPointer<QObject> guard;
    cf::CoreReply done;
    bool settled = false;

    void settle(const QJsonObject &result)
    {
        if (settled)
            return;
        settled = true;
        if (guard)
            guard->deleteLater();
        if (done)
            done(result);
    }
};

std::shared_ptr<Watch> follow(CoreClient *client, QObject *owner, const QString &token,
                              const QString &progressEvent, const QString &doneEvent,
                              cf::CoreProgress progress, cf::CoreReply done)
{
    auto watch = std::make_shared<Watch>();
    auto *guard = new QObject(owner);
    watch->guard = guard;
    watch->done = std::move(done);
    QObject::connect(client, &CoreClient::coreEvent, guard,
                     [watch, token, progressEvent, doneEvent,
                      progress](const QString &event, const QJsonObject &data) {
                         if (watch->settled || !carries(data, token))
                             return;
                         if (event == progressEvent && progress)
                             progress(data.value(QStringLiteral("stage")).toString());
                         else if (event == doneEvent)
                             watch->settle(data);
                     });
    QObject::connect(client, &CoreClient::connectedChanged, guard, [watch](bool connected) {
        if (!connected)
            watch->settle({{QStringLiteral("error"), QStringLiteral("core_disconnected")}});
    });
    QObject::connect(client, &QObject::destroyed, guard, [watch] {
        watch->settle({{QStringLiteral("error"), QStringLiteral("core_disconnected")}});
    });
    return watch;
}

bool startJob(const std::function<CoreClient *()> &client, QObject *owner, const QString &method,
              QJsonObject params, const QString &progressEvent, const QString &doneEvent,
              cf::CoreProgress progress, cf::CoreReply done)
{
    CoreClient *core = client ? client() : nullptr;
    if (!attached(core, method))
        return false;
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const std::shared_ptr<Watch> watch =
        follow(core, owner, token, progressEvent, doneEvent, std::move(progress), std::move(done));
    params.insert(QStringLiteral("token"), token);
    core->request(method, params,
                  [watch](bool ok, const QJsonValue &, const QString &error) {
                      if (!ok)
                          watch->settle({{QStringLiteral("error"),
                                          error.isEmpty() ? QStringLiteral("core_failed") : error}});
                  },
                  30000);
    return true;
}

} // namespace

void install(cf::Host &host, QObject *owner, std::function<CoreClient *()> client)
{
    host.coreScan = [owner, client](const QJsonObject &context, cf::CoreProgress progress,
                                    cf::CoreReply done) {
        return startJob(client, owner, QStringLiteral("core.cleanup.scan"),
                        {{QStringLiteral("context"), context}},
                        QStringLiteral("core.cleanup.progress"),
                        QStringLiteral("core.cleanup.scanned"), std::move(progress),
                        std::move(done));
    };
    host.coreCancelScan = [client] {
        CoreClient *core = client ? client() : nullptr;
        if (attached(core, QStringLiteral("core.cleanup.cancel")))
            core->request(QStringLiteral("core.cleanup.cancel"));
    };
    host.coreDeleteFiles = [owner, client](const QJsonObject &request, cf::CoreReply done) {
        return startJob(client, owner, QStringLiteral("core.cleanup.deleteFiles"), request,
                        QString(), QStringLiteral("core.cleanup.deleted"), {}, std::move(done));
    };
    host.coreAppendLog = [client](const QJsonObject &request, cf::CoreReply done) {
        CoreClient *core = client ? client() : nullptr;
        if (!attached(core, QStringLiteral("core.cleanup.appendLog")))
            return false;
        core->request(QStringLiteral("core.cleanup.appendLog"), request,
                      [done](bool ok, const QJsonValue &result, const QString &error) {
                          if (!done)
                              return;
                          done(ok ? result.toObject()
                                  : QJsonObject{{QStringLiteral("error"), error}});
                      },
                      30000);
        return true;
    };
}

} // namespace forkmesh::corecleanup
