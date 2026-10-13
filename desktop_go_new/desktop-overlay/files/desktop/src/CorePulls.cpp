#include "CorePulls.h"

#include "CoreGit.h"
#include "PullStoreCore.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonArray>
#include <QProcessEnvironment>
#include <QThread>

#include <chrono>
#include <future>

namespace forkmesh::corepulls {

namespace {

constexpr qint64 kWriteTimeoutMs = 15 * 60 * 1000;
constexpr qint64 kReadTimeoutMs = 5 * 60 * 1000;

bool validEnvName(const QString &name)
{
    if (name.isEmpty())
        return false;
    for (qsizetype i = 0; i < name.size(); ++i) {
        const QChar c = name.at(i);
        const bool letter = (c >= QLatin1Char('A') && c <= QLatin1Char('Z')) ||
                            (c >= QLatin1Char('a') && c <= QLatin1Char('z')) || c == QLatin1Char('_');
        const bool digit = c >= QLatin1Char('0') && c <= QLatin1Char('9');
        if (!letter && !(digit && i > 0))
            return false;
    }
    return true;
}

const QJsonArray &desktopEnvironment()
{
    static const QJsonArray env = [] {
        const QProcessEnvironment system = QProcessEnvironment::systemEnvironment();
        QJsonArray out;
        for (const QString &name : system.keys()) {
            if (name == QLatin1String("FORKMESH_DESKTOP_INSTANCE") || !validEnvName(name))
                continue;
            const QString value = system.value(name);
            if (value.contains(QChar(0)))
                continue;
            out.append(name + QLatin1Char('=') + value);
        }
        for (const char *name : {"LC_ALL", "LANGUAGE"})
            if (!system.contains(QLatin1String(name)))
                out.append(QLatin1String(name) + QLatin1Char('='));
        return out;
    }();
    return env;
}

bool declined(const QString &error)
{
    return error == QLatin1String("identity_mismatch") || error == QLatin1String("identity_unavailable") ||
           error == QLatin1String("forbidden") || error == QLatin1String("bad_params") ||
           error == QLatin1String("unsupported") || error == QLatin1String("core_unavailable");
}

struct Outcome {
    bool answered = false;
    bool sent = false;
    QJsonObject result;
    QString error;
};

Outcome callOnce(const QString &method, const QJsonObject &params, qint64 timeoutMs)
{
    Outcome outcome;
    outcome.answered = forkmesh::coregit::callCore(method, params, timeoutMs, &outcome.result, &outcome.error,
                                                   &outcome.sent);
    return outcome;
}

Outcome callPumped(const QString &method, const QJsonObject &params, qint64 timeoutMs)
{
    const QCoreApplication *app = QCoreApplication::instance();
    if (!app || QThread::currentThread() != app->thread())
        return callOnce(method, params, timeoutMs);
    auto pending = std::async(std::launch::async, [method, params, timeoutMs] {
        return callOnce(method, params, timeoutMs);
    });
    while (pending.wait_for(std::chrono::milliseconds(40)) != std::future_status::ready)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 12);
    return pending.get();
}

class Backend final : public forkmesh::pullstorecore::Backend
{
public:
    bool available() const override { return forkmesh::coregit::attached(); }

    forkmesh::pullstorecore::Reply call(const QString &method, const QJsonObject &params, bool write) override
    {
        QJsonObject request = params;
        QJsonObject store = request.value(QStringLiteral("store")).toObject();
        store.insert(QStringLiteral("env"), desktopEnvironment());
        request.insert(QStringLiteral("store"), store);
        const Outcome outcome = callPumped(method, request, write ? kWriteTimeoutMs : kReadTimeoutMs);
        forkmesh::pullstorecore::Reply reply;
        if (outcome.answered) {
            reply.answered = true;
            reply.result = outcome.result;
            return reply;
        }
        reply.error = outcome.error;
        reply.failed = outcome.sent && !declined(outcome.error);
        return reply;
    }
};

}

void install()
{
    static Backend backend;
    forkmesh::pullstorecore::setBackend(&backend);
}

}
