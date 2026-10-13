#include "CoreAgentRun.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QPointer>

#include <atomic>

namespace forkmesh::coreagentrun {

namespace {

struct Subscription {
    QPointer<QObject> context;
    Handler handler;
};

struct State {
    Backend backend;
    QHash<QString, Subscription> subscriptions;
    const QJsonObject *delivering = nullptr;
    QStringList lastCredentials;
    bool credentialsSent = false;
    QHash<QString, qint64> stderrSeen;
};

State &state()
{
    static State s;
    return s;
}

bool routed(const QString &method)
{
    const Backend &b = state().backend;
    return b.hasRoute && b.hasRoute(method);
}

} // namespace

void setBackend(Backend backend)
{
    state().backend = std::move(backend);
    state().credentialsSent = false;
}

void dispatch(const QString &event, const QJsonObject &data)
{
    if (!event.startsWith(QLatin1String("core.agentrun.")))
        return;
    const QString key = data.value(QStringLiteral("key")).toString();
    if (data.contains(QStringLiteral("eo"))) {
        const qint64 end = qint64(data.value(QStringLiteral("eo")).toDouble());
        qint64 &seen = state().stderrSeen[key];
        if (end > seen)
            seen = end;
    }
    const auto it = state().subscriptions.constFind(key);
    if (it == state().subscriptions.constEnd() || !it->context)
        return;
    const Handler handler = it->handler;
    handler(event, data);
}

void connectionChanged(bool connected)
{
    state().credentialsSent = false;
    if (!connected)
        return;
    pushCredentials();
    const QStringList keys = state().subscriptions.keys();
    for (const QString &key : keys) {
        const auto it = state().subscriptions.constFind(key);
        if (it == state().subscriptions.constEnd() || !it->context)
            continue;
        const Handler handler = it->handler;
        handler(QStringLiteral("core.agentrun.reconnected"), {{QStringLiteral("key"), key}});
    }
}

bool available()
{
    const Backend &b = state().backend;
    return b.available && b.available() && routed(QStringLiteral("core.agentrun.attach"));
}

void pushCredentials()
{
    State &s = state();
    if (!s.backend.notify || !s.backend.credentials || !routed(QStringLiteral("core.agentrun.credentials")))
        return;
    const QStringList values = s.backend.credentials();
    if (s.credentialsSent && values == s.lastCredentials)
        return;
    if (s.backend.notify(QStringLiteral("core.agentrun.credentials"),
                         {{QStringLiteral("values"), QJsonArray::fromStringList(values)}})) {
        s.lastCredentials = values;
        s.credentialsSent = true;
    }
}

void subscribe(const QString &key, QObject *context, Handler handler)
{
    state().subscriptions.insert(key, Subscription{context, std::move(handler)});
}

void unsubscribe(const QString &key, QObject *context)
{
    State &s = state();
    const auto it = s.subscriptions.find(key);
    if (it != s.subscriptions.end() && (!it->context || it->context == context))
        s.subscriptions.erase(it);
}

void request(const QString &method, const QJsonObject &params, QObject *context, Done done)
{
    const Backend &b = state().backend;
    if (!b.request) {
        if (done)
            done(false, {}, QStringLiteral("core_unavailable"));
        return;
    }
    QPointer<QObject> guard(context);
    b.request(method, params, [guard, done](bool ok, const QJsonObject &result, const QString &error) {
        if (guard && done)
            done(ok, result, error);
    });
}

bool notify(const QString &method, const QJsonObject &params)
{
    const Backend &b = state().backend;
    return b.notify && b.notify(method, params);
}

void attach(const QString &key, qint64 after, QObject *context, Done done)
{
    pushCredentials();
    request(QStringLiteral("core.agentrun.attach"),
            {{QStringLiteral("key"), key},
             {QStringLiteral("after"), double(after)},
             {QStringLiteral("stderr"), true},
             {QStringLiteral("stderrAfter"), double(state().stderrSeen.value(key))}},
            context, std::move(done));
}

void detach(const QString &key)
{
    state().stderrSeen.remove(key);
    notify(QStringLiteral("core.agentrun.detach"), {{QStringLiteral("key"), key}});
}

QString transientKey(const QString &runtime)
{
    static std::atomic<int> serial{0};
    return QStringLiteral("dialog-%1-%2-%3-%4")
        .arg(runtime)
        .arg(QCoreApplication::applicationPid())
        .arg(QDateTime::currentMSecsSinceEpoch())
        .arg(++serial);
}

bool deliveringStored(const QJsonObject &event)
{
    return state().delivering == &event;
}

bool deliveringAnyStored()
{
    return state().delivering != nullptr;
}

StoredDelivery::StoredDelivery(const QJsonObject *event) : m_previous(state().delivering)
{
    state().delivering = event;
}

StoredDelivery::~StoredDelivery()
{
    state().delivering = m_previous;
}

} // namespace forkmesh::coreagentrun
