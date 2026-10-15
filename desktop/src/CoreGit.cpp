#include "CoreGit.h"

#include "BackgroundActivity.h"
#include "CoreClient.h"
#include "CoreTerminal.h"
#include "GitIndexQueue.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QProcessEnvironment>
#include <QSettings>
#include <QThread>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QMutex>
#include <QMutexLocker>
#include <QTcpSocket>

#include <chrono>
#include <functional>
#include <future>
#include <memory>

namespace forkmesh::coregit {

namespace {

constexpr int kConnectMs = 2000;
constexpr int kHelloMs = 5000;
constexpr qint64 kMaxLineBytes = 96LL * 1024 * 1024;
constexpr qint64 kReplySlackMs = 30000;
constexpr qint64 kHelloId = 1;
constexpr qint64 kCallId = 2;
constexpr qsizetype kMaxRoutedInputBytes = 32 * 1024 * 1024;

struct Link {
    QMutex mutex;
    bool attached = false;
    QString stateDirectory;
    forkmesh::core::Endpoint endpoint;
    qint64 checkedPid = 0;
    QStringList routes;
    bool reads = true;
};

Link &link()
{
    static Link state;
    return state;
}

class LineChannel
{
public:
    explicit LineChannel(const forkmesh::core::Endpoint &endpoint) : m_endpoint(endpoint) {}

    bool open()
    {
#ifndef Q_OS_WIN
        if (!m_endpoint.unixPath.isEmpty()) {
            m_local = std::make_unique<QLocalSocket>();
            m_local->connectToServer(m_endpoint.unixPath);
            return m_local->waitForConnected(kConnectMs);
        }
#endif
        const int colon = m_endpoint.tcp.lastIndexOf(QLatin1Char(':'));
        if (colon <= 0)
            return false;
        m_tcp = std::make_unique<QTcpSocket>();
        m_tcp->connectToHost(m_endpoint.tcp.left(colon),
                             quint16(m_endpoint.tcp.mid(colon + 1).toUInt()));
        return m_tcp->waitForConnected(kConnectMs);
    }

    bool send(const QJsonObject &object)
    {
        QIODevice *device = this->device();
        if (!device)
            return false;
        const QByteArray line = forkmesh::core::encodeLine(object);
        if (device->write(line) != line.size())
            return false;
        while (device->bytesToWrite() > 0)
            if (!waitWritten(10000))
                return false;
        return true;
    }

    bool receive(qint64 id, qint64 deadlineMs, QJsonObject *message, QString *error,
                 const CoreEventHandler &onEvent = {})
    {
        QIODevice *device = this->device();
        if (!device) {
            *error = QStringLiteral("core_unavailable");
            return false;
        }
        for (;;) {
            qsizetype newline = m_buffer.indexOf('\n');
            while (newline >= 0) {
                const QByteArray line = m_buffer.left(newline);
                m_buffer.remove(0, newline + 1);
                const QJsonObject object = QJsonDocument::fromJson(line).object();
                const QString event = object.value(QStringLiteral("event")).toString();
                if (!event.isEmpty()) {
                    if (onEvent)
                        onEvent(event, object.value(QStringLiteral("data")).toObject());
                } else if (qint64(object.value(QStringLiteral("id")).toDouble()) == id &&
                    object.contains(QStringLiteral("ok")) &&
                    !object.contains(QStringLiteral("method"))) {
                    *message = object;
                    return true;
                }
                newline = m_buffer.indexOf('\n');
            }
            if (m_buffer.size() > kMaxLineBytes) {
                *error = QStringLiteral("core_reply_too_large");
                return false;
            }
            const qint64 remaining = deadlineMs - QDateTime::currentMSecsSinceEpoch();
            if (remaining <= 0) {
                *error = QStringLiteral("core_timeout");
                return false;
            }
            if (!connected()) {
                *error = QStringLiteral("core_gone");
                return false;
            }
            if (!waitReadable(int(qMin<qint64>(remaining, 1000))) && !connected()) {
                *error = QStringLiteral("core_gone");
                return false;
            }
            m_buffer.append(device->readAll());
        }
    }

private:
    QIODevice *device() const
    {
        if (m_local)
            return m_local.get();
        return m_tcp.get();
    }

    bool connected() const
    {
        if (m_local)
            return m_local->state() == QLocalSocket::ConnectedState || m_local->bytesAvailable() > 0;
        if (m_tcp)
            return m_tcp->state() == QAbstractSocket::ConnectedState || m_tcp->bytesAvailable() > 0;
        return false;
    }

    bool waitReadable(int ms)
    {
        if (m_local)
            return m_local->waitForReadyRead(ms);
        return m_tcp && m_tcp->waitForReadyRead(ms);
    }

    bool waitWritten(int ms)
    {
        if (m_local)
            return m_local->waitForBytesWritten(ms);
        return m_tcp && m_tcp->waitForBytesWritten(ms);
    }

    forkmesh::core::Endpoint m_endpoint;
    std::unique_ptr<QLocalSocket> m_local;
    std::unique_ptr<QTcpSocket> m_tcp;
    QByteArray m_buffer;
};

bool hello(LineChannel &channel, const forkmesh::core::Endpoint &endpoint, QStringList *routes)
{
    if (!channel.send({{QStringLiteral("id"), double(kHelloId)},
                       {QStringLiteral("method"), QStringLiteral("core.hello")},
                       {QStringLiteral("params"),
                        QJsonObject{{QStringLiteral("role"), QStringLiteral("cli")},
                                    {QStringLiteral("token"), endpoint.token},
                                    {QStringLiteral("name"), QStringLiteral("forkmesh-desktop-worker")},
                                    {QStringLiteral("pid"), double(QCoreApplication::applicationPid())}}}}))
        return false;
    QJsonObject reply;
    QString error;
    if (!channel.receive(kHelloId, QDateTime::currentMSecsSinceEpoch() + kHelloMs, &reply, &error) ||
        !reply.value(QStringLiteral("ok")).toBool())
        return false;
    routes->clear();
    const QJsonArray table = reply.value(QStringLiteral("result")).toObject().value(QStringLiteral("routes")).toArray();
    for (const QJsonValue &value : table) {
        const QJsonObject route = value.toObject();
        if (!route.value(QStringLiteral("family")).toBool())
            routes->append(route.value(QStringLiteral("pattern")).toString());
    }
    return true;
}

bool snapshot(const QString &method, forkmesh::core::Endpoint *endpoint)
{
    QMutexLocker lock(&link().mutex);
    if (!link().attached || !link().endpoint.valid())
        return false;
    if (link().checkedPid == link().endpoint.pid && !link().routes.contains(method))
        return false;
    *endpoint = link().endpoint;
    return true;
}

void remember(const forkmesh::core::Endpoint &endpoint, const QStringList &routes)
{
    QMutexLocker lock(&link().mutex);
    if (link().endpoint.pid == endpoint.pid) {
        link().checkedPid = endpoint.pid;
        link().routes = routes;
    }
}

bool callCoreWith(const QString &method, const QJsonObject &params, qint64 timeoutMs,
                  QJsonObject *result, QString *error, bool *sent,
                  const std::function<void()> &beforeSend, const CoreEventHandler &onEvent = {})
{
    if (sent)
        *sent = false;
    QString scratch;
    if (!error)
        error = &scratch;
    forkmesh::core::Endpoint endpoint;
    if (!snapshot(method, &endpoint)) {
        *error = QStringLiteral("core_unavailable");
        return false;
    }
    LineChannel channel(endpoint);
    QStringList routes;
    if (!channel.open() || !hello(channel, endpoint, &routes)) {
        *error = QStringLiteral("core_unavailable");
        return false;
    }
    remember(endpoint, routes);
    if (!routes.contains(method)) {
        *error = QStringLiteral("unsupported");
        return false;
    }
    if (beforeSend)
        beforeSend();
    if (!channel.send({{QStringLiteral("id"), double(kCallId)},
                       {QStringLiteral("method"), method},
                       {QStringLiteral("params"), params}})) {
        *error = QStringLiteral("core_unavailable");
        return false;
    }
    if (sent)
        *sent = true;
    QJsonObject reply;
    if (!channel.receive(kCallId, QDateTime::currentMSecsSinceEpoch() + qMax<qint64>(1, timeoutMs), &reply, error, onEvent))
        return false;
    if (!reply.value(QStringLiteral("ok")).toBool()) {
        *error = reply.value(QStringLiteral("error")).toString();
        if (error->isEmpty())
            *error = QStringLiteral("core_error");
        return false;
    }
    if (result)
        *result = reply.value(QStringLiteral("result")).toObject();
    return true;
}

QString redactedConfig(const QString &value)
{
    const qsizetype eq = value.indexOf(QLatin1Char('='));
    if (eq < 0)
        return value;
    const QString key = value.left(eq).toLower();
    for (const char *word : {"header", "auth", "token", "password", "secret"})
        if (key.contains(QLatin1String(word)))
            return value.left(eq) + QStringLiteral("=<redacted>");
    return value;
}

QString commandDetail(const GitIndexWriteRequest &request)
{
    if (!request.detail.isEmpty())
        return request.detail;
    QStringList shown{QStringLiteral("git"), QStringLiteral("-C"), request.dir};
    for (qsizetype i = 0; i < request.args.size(); ++i) {
        shown << request.args.at(i);
        if (request.args.at(i) == QLatin1String("-c") && i + 1 < request.args.size())
            shown << redactedConfig(request.args.at(++i));
    }
    return shown.join(QLatin1Char(' '));
}

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

bool callCorePumped(const QString &method, const QJsonObject &params, qint64 timeoutMs,
                    QJsonObject *result, QString *error, bool *sent,
                    const std::function<void()> &beforeSend)
{
    const QCoreApplication *app = QCoreApplication::instance();
    if (!app || QThread::currentThread() != app->thread())
        return callCoreWith(method, params, timeoutMs, result, error, sent, beforeSend);
    struct Outcome {
        bool answered = false;
        bool sent = false;
        QJsonObject result;
        QString error;
    };
    if (beforeSend)
        beforeSend();
    auto pending = std::async(std::launch::async, [method, params, timeoutMs] {
        Outcome outcome;
        outcome.answered = callCoreWith(method, params, timeoutMs, &outcome.result, &outcome.error,
                                        &outcome.sent, {});
        return outcome;
    });
    while (pending.wait_for(std::chrono::milliseconds(40)) != std::future_status::ready)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 12);
    const Outcome outcome = pending.get();
    if (sent)
        *sent = outcome.sent;
    if (result)
        *result = outcome.result;
    if (error)
        *error = outcome.error;
    return outcome.answered;
}

bool runWrite(const GitIndexWriteRequest &request, GitAttempt *out)
{
    if (request.input && request.input->size() > kMaxRoutedInputBytes)
        return false;
    QJsonArray args;
    for (const QString &arg : request.args)
        args.append(arg);
    QJsonObject params{{QStringLiteral("path"), request.dir},
                       {QStringLiteral("args"), args},
                       {QStringLiteral("write"), true},
                       {QStringLiteral("env"), desktopEnvironment()},
                       {QStringLiteral("timeoutMs"), double(qMax(0, request.timeoutMs))}};
    if (request.input)
        params.insert(QStringLiteral("inputBase64"), QString::fromLatin1(request.input->toBase64()));
    const qint64 waitMs = qint64(gitTimeoutMsForArgs(request.args, qMax(request.timeoutMs, kGitDefaultTimeoutMs))) +
                          kGitIndexQueueWaitMs + kReplySlackMs;

    BackgroundWork work;
    work.kind = QStringLiteral("git");
    work.detail = commandDetail(request) + QStringLiteral(" [core queue]");
    work.execution = ActionTelemetry::Execution::Worker;
    work.source = request.source;
    work.workingDirectory = request.dir;
    quint64 ticket = 0;
    QJsonObject result;
    QString error;
    bool sent = false;
    const bool answered = callCorePumped(QStringLiteral("core.git.run"), params, waitMs, &result, &error, &sent,
                                         [&work, &ticket] { ticket = BackgroundActivity::begin(std::move(work)); });
    if (!sent) {
        if (ticket)
            BackgroundActivity::end(ticket, QStringLiteral("cancelled"));
        return false;
    }
    if (!answered && error != QLatin1String("core_gone") && error != QLatin1String("core_timeout") &&
        error != QLatin1String("core_reply_too_large")) {
        BackgroundActivity::end(ticket, QStringLiteral("cancelled"));
        return false;
    }
    if (!answered) {
        out->ok = false;
        out->timedOut = error == QLatin1String("core_timeout");
        out->output.clear();
        out->error = QStringLiteral("forkmesh-core did not finish git %1 (%2)")
                         .arg(gitSubcommand(request.args), error);
        BackgroundActivity::end(ticket, out->timedOut ? QStringLiteral("timed out") : QStringLiteral("failed (core)"));
        return true;
    }
    out->ok = result.value(QStringLiteral("ok")).toBool();
    out->timedOut = result.value(QStringLiteral("timedOut")).toBool();
    const QString encoded = result.value(QStringLiteral("stdoutBase64")).toString();
    out->output = encoded.isEmpty() ? result.value(QStringLiteral("stdout")).toString().toUtf8()
                                    : QByteArray::fromBase64(encoded.toLatin1());
    out->error = out->ok ? QString() : result.value(QStringLiteral("error")).toString();
    const int code = result.value(QStringLiteral("code")).toInt();
    BackgroundActivity::end(ticket, out->ok ? QStringLiteral("succeeded")
                                    : out->timedOut ? QStringLiteral("timed out")
                                                    : QStringLiteral("failed (exit %1)").arg(code));
    return true;
}

}

bool runDetached(const DetachedRequest &request, DetachedResult *out)
{
    if (!out || request.dir.isEmpty() || request.args.isEmpty())
        return false;
    const bool write = gitArgsWriteIndex(request.args);
    {
        QMutexLocker lock(&link().mutex);
        if (!link().attached || (!write && !link().reads))
            return false;
    }
    QJsonArray args;
    for (const QString &arg : request.args)
        args.append(arg);
    const int timeoutMs = qMax(0, request.timeoutMs);
    const QJsonObject params{{QStringLiteral("path"), request.dir},
                             {QStringLiteral("args"), args},
                             {QStringLiteral("write"), write},
                             {QStringLiteral("env"), desktopEnvironment()},
                             {QStringLiteral("timeoutMs"), double(timeoutMs)}};
    const qint64 waitMs = qint64(gitTimeoutMsForArgs(request.args, qMax(timeoutMs, kGitDefaultTimeoutMs))) +
                          (write ? kGitIndexQueueWaitMs : 0) + kReplySlackMs;
    BackgroundWork work;
    work.kind = QStringLiteral("git");
    work.detail = request.detail + (write ? QStringLiteral(" [core queue]") : QStringLiteral(" [core]"));
    work.execution = ActionTelemetry::Execution::Worker;
    work.source = request.source;
    work.routine = request.routine;
    work.workingDirectory = request.dir;
    work.queuedMs = request.queuedMs;
    quint64 ticket = 0;
    QJsonObject result;
    QString error;
    bool sent = false;
    const bool answered = callCoreWith(QStringLiteral("core.git.run"), params, waitMs, &result, &error, &sent,
                                       [&work, &ticket] { ticket = BackgroundActivity::begin(std::move(work)); });
    const bool truncated = answered && result.value(QStringLiteral("truncated")).toBool();
    const bool coreFailed = !answered && (error == QLatin1String("core_gone") || error == QLatin1String("core_timeout") ||
                                          error == QLatin1String("core_reply_too_large"));
    if (!sent || (!answered && !(write && coreFailed)) || (!write && (coreFailed || truncated))) {
        if (ticket)
            BackgroundActivity::end(ticket, QStringLiteral("cancelled"));
        return false;
    }
    if (!answered) {
        out->ok = false;
        out->output = QStringLiteral("forkmesh-core did not finish git %1 (%2)")
                          .arg(gitSubcommand(request.args), error)
                          .toUtf8();
        BackgroundActivity::end(ticket, error == QLatin1String("core_timeout") ? QStringLiteral("timed out")
                                                                                : QStringLiteral("failed (core)"));
        return true;
    }
    const bool timedOut = result.value(QStringLiteral("timedOut")).toBool();
    out->ok = result.value(QStringLiteral("ok")).toBool() && !timedOut;
    const QString encoded = result.value(QStringLiteral("stdoutBase64")).toString();
    out->output = encoded.isEmpty() ? result.value(QStringLiteral("stdout")).toString().toUtf8()
                                    : QByteArray::fromBase64(encoded.toLatin1());
    if (request.mergeStderr)
        out->output += result.value(QStringLiteral("stderr")).toString().toUtf8();
    const int code = result.value(QStringLiteral("code")).toInt();
    QString outcome;
    if (timedOut)
        outcome = QStringLiteral("timed out");
    else if (out->ok)
        outcome = QStringLiteral("succeeded");
    else {
        outcome = gitProbeAnswerOutcome(QStringLiteral("git"), request.args, code);
        if (outcome.isEmpty())
            outcome = QStringLiteral("failed (exit %1)").arg(code);
    }
    BackgroundActivity::end(ticket, outcome);
    return true;
}

bool runRead(const ReadRequest &request, GitAttempt *out)
{
    if (!out || request.dir.isEmpty() || request.args.isEmpty() || gitArgsWriteIndex(request.args))
        return false;
    if (request.input && request.input->size() > kMaxRoutedInputBytes)
        return false;
    const QCoreApplication *app = QCoreApplication::instance();
    if (!app || QThread::currentThread() == app->thread())
        return false;
    {
        QMutexLocker lock(&link().mutex);
        if (!link().attached || !link().reads)
            return false;
    }
    QJsonArray args;
    for (const QString &arg : request.args)
        args.append(arg);
    const int timeoutMs = qMax(0, request.timeoutMs);
    QJsonObject params{{QStringLiteral("path"), request.dir},
                       {QStringLiteral("args"), args},
                       {QStringLiteral("write"), false},
                       {QStringLiteral("env"), desktopEnvironment()},
                       {QStringLiteral("timeoutMs"), double(timeoutMs)}};
    if (request.input)
        params.insert(QStringLiteral("inputBase64"), QString::fromLatin1(request.input->toBase64()));
    const qint64 waitMs = qint64(gitTimeoutMsForArgs(request.args, qMax(timeoutMs, kGitDefaultTimeoutMs))) +
                          kReplySlackMs;
    BackgroundWork work;
    work.kind = QStringLiteral("git");
    work.detail = request.detail + QStringLiteral(" [core]");
    work.execution = ActionTelemetry::Execution::Worker;
    work.source = request.source;
    work.routine = BackgroundRoutine::current();
    work.workingDirectory = request.dir;
    quint64 ticket = 0;
    QJsonObject result;
    QString error;
    bool sent = false;
    const bool answered = callCoreWith(QStringLiteral("core.git.run"), params, waitMs, &result, &error, &sent,
                                       [&work, &ticket] { ticket = BackgroundActivity::begin(std::move(work)); });
    if (!answered || result.value(QStringLiteral("truncated")).toBool()) {
        if (ticket)
            BackgroundActivity::end(ticket, QStringLiteral("cancelled"));
        return false;
    }
    out->timedOut = result.value(QStringLiteral("timedOut")).toBool();
    out->ok = result.value(QStringLiteral("ok")).toBool() && !out->timedOut;
    const QString encoded = result.value(QStringLiteral("stdoutBase64")).toString();
    out->output = encoded.isEmpty() ? result.value(QStringLiteral("stdout")).toString().toUtf8()
                                    : QByteArray::fromBase64(encoded.toLatin1());
    out->error = out->ok ? QString() : result.value(QStringLiteral("error")).toString();
    const int code = result.value(QStringLiteral("code")).toInt();
    QString outcome;
    if (out->timedOut)
        outcome = QStringLiteral("timed out");
    else if (out->ok)
        outcome = QStringLiteral("succeeded");
    else {
        outcome = gitProbeAnswerOutcome(QStringLiteral("git"), request.args, code);
        if (outcome.isEmpty())
            outcome = QStringLiteral("failed (exit %1)").arg(code);
    }
    BackgroundActivity::end(ticket, outcome);
    return true;
}

bool runCommand(const CommandRequest &request, CommandResult *out)
{
    if (!out || request.dir.isEmpty() || request.args.isEmpty())
        return false;
    const QCoreApplication *app = QCoreApplication::instance();
    if (!app || QThread::currentThread() == app->thread())
        return false;
    {
        QMutexLocker lock(&link().mutex);
        if (!link().attached)
            return false;
    }
    QJsonArray args;
    for (const QString &arg : request.args)
        args.append(arg);
    QJsonArray env;
    for (const QString &entry : request.env)
        env.append(entry);
    const bool write = request.write || gitArgsWriteIndex(request.args);
    const int timeoutMs = qMax(0, request.timeoutMs);
    const QJsonObject params{{QStringLiteral("path"), request.dir},
                             {QStringLiteral("args"), args},
                             {QStringLiteral("write"), write},
                             {QStringLiteral("env"), env},
                             {QStringLiteral("timeoutMs"), double(timeoutMs)}};
    const qint64 waitMs = qint64(qMax(timeoutMs, kGitDefaultTimeoutMs)) + (write ? kGitIndexQueueWaitMs : 0) +
                          kReplySlackMs;
    BackgroundWork work;
    work.kind = QStringLiteral("git");
    work.detail = request.detail + (write ? QStringLiteral(" [core queue]") : QStringLiteral(" [core]"));
    work.execution = ActionTelemetry::Execution::Worker;
    work.source = request.source;
    work.workingDirectory = request.dir;
    quint64 ticket = 0;
    QJsonObject result;
    QString error;
    bool sent = false;
    const bool answered = callCoreWith(QStringLiteral("core.git.run"), params, waitMs, &result, &error, &sent,
                                       [&work, &ticket] { ticket = BackgroundActivity::begin(std::move(work)); });
    if (!sent) {
        if (ticket)
            BackgroundActivity::end(ticket, QStringLiteral("cancelled"));
        return false;
    }
    if (!answered) {
        if (error != QLatin1String("core_gone") && error != QLatin1String("core_timeout") &&
            error != QLatin1String("core_reply_too_large")) {
            BackgroundActivity::end(ticket, QStringLiteral("cancelled"));
            return false;
        }
        out->ok = false;
        out->timedOut = error == QLatin1String("core_timeout");
        out->error = QStringLiteral("forkmesh-core did not finish git %1 (%2)").arg(gitSubcommand(request.args), error);
        BackgroundActivity::end(ticket, out->timedOut ? QStringLiteral("timed out") : QStringLiteral("failed (core)"));
        return true;
    }
    out->timedOut = result.value(QStringLiteral("timedOut")).toBool();
    out->ok = result.value(QStringLiteral("ok")).toBool() && !out->timedOut;
    const QString encoded = result.value(QStringLiteral("stdoutBase64")).toString();
    out->output = encoded.isEmpty() ? result.value(QStringLiteral("stdout")).toString().toUtf8()
                                    : QByteArray::fromBase64(encoded.toLatin1());
    const QByteArray stderrText = result.value(QStringLiteral("stderr")).toString().toUtf8();
    if (!stderrText.isEmpty() && !out->output.isEmpty() && !out->output.endsWith('\n'))
        out->output += '\n';
    out->output += stderrText;
    out->error = out->ok ? QString() : result.value(QStringLiteral("error")).toString();
    const int code = result.value(QStringLiteral("code")).toInt();
    BackgroundActivity::end(ticket, out->timedOut ? QStringLiteral("timed out")
                                    : out->ok    ? QStringLiteral("succeeded")
                                                 : QStringLiteral("failed (exit %1)").arg(code));
    return true;
}

bool callCore(const QString &method, const QJsonObject &params, qint64 timeoutMs,
              QJsonObject *result, QString *error, bool *sent, const CoreEventHandler &onEvent)
{
    return callCoreWith(method, params, timeoutMs, result, error, sent, {}, onEvent);
}

void follow(CoreClient *client)
{
    if (!client)
        return;
    forkmesh::coreterminal::setClient(client);
    QObject::connect(client, &CoreClient::connectedChanged, client, [client](bool connected) {
        if (connected) {
            attach(client->stateDirectory());
            forkmesh::coreterminal::sweepOrphans(client);
        } else {
            detach();
        }
    });
    QObject::connect(client, &QObject::destroyed, [] { detach(); });
    if (client->isConnected())
        attach(client->stateDirectory());
}

void attach(const QString &stateDirectory)
{
    const forkmesh::core::Endpoint endpoint = forkmesh::core::readEndpoint(stateDirectory);
    {
        QMutexLocker lock(&link().mutex);
        link().attached = endpoint.valid();
        link().stateDirectory = stateDirectory;
        if (link().endpoint.pid != endpoint.pid) {
            link().checkedPid = 0;
            link().routes.clear();
        }
        link().endpoint = endpoint;
        link().reads = QSettings().value(QStringLiteral("core/gitReads"), true).toBool();
    }
    setGitIndexWriteRoute(endpoint.valid() ? GitIndexWriteRoute(runWrite) : GitIndexWriteRoute());
}

void detach()
{
    setGitIndexWriteRoute({});
    QMutexLocker lock(&link().mutex);
    link().attached = false;
}

bool attached()
{
    QMutexLocker lock(&link().mutex);
    return link().attached;
}

}
