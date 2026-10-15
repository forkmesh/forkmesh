#include "CoreClient.h"

#include "AppPaths.h"
#include "ProcessReaper.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QPointer>
#include <QTcpSocket>
#include <QTimer>

#include <memory>
#include <utility>

#ifndef Q_OS_WIN
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace forkmesh::core {

namespace {

constexpr int kMaxLineBytes = 64 * 1024 * 1024;
constexpr qint64 kRelaunchSpacingMs = 10000;
constexpr qint64 kBinaryNewerSlackMs = 2000;

QString binaryName()
{
#ifdef Q_OS_WIN
    return QStringLiteral("forkmesh-core.exe");
#else
    return QStringLiteral("forkmesh-core");
#endif
}

} // namespace

Endpoint readEndpoint(const QString &stateDirectory)
{
    Endpoint out;
    QFile file(QDir(stateDirectory).filePath(QStringLiteral("endpoint.json")));
    if (!file.open(QIODevice::ReadOnly))
        return out;
    const QJsonObject object = QJsonDocument::fromJson(file.readAll()).object();
    out.pid = qint64(object.value(QStringLiteral("pid")).toDouble());
    out.version = object.value(QStringLiteral("version")).toString();
    out.project = object.value(QStringLiteral("project")).toString();
    out.unixPath = object.value(QStringLiteral("unix")).toString();
    out.tcp = object.value(QStringLiteral("tcp")).toString();
    out.token = object.value(QStringLiteral("token")).toString();
    out.startedMs = qint64(object.value(QStringLiteral("startedMs")).toDouble());
    out.protocol = object.value(QStringLiteral("protocol")).toInt();
    return out;
}

QString defaultStateDirectory()
{
    return forkmesh::paths::stateRoot() + QStringLiteral("/core");
}

QString defaultBinaryPath()
{
    const QString fromEnv = qEnvironmentVariable("FORKMESH_CORE_BIN").trimmed();
    if (!fromEnv.isEmpty())
        return fromEnv;
    const QString appDir = QCoreApplication::applicationDirPath();
    const QStringList candidates{
        QDir(appDir).filePath(binaryName()),
        QDir(appDir).filePath(QStringLiteral("../libexec/forkmesh/") + binaryName()),
        QDir(appDir).filePath(QStringLiteral("../Resources/") + binaryName()),
    };
    for (const QString &candidate : candidates)
        if (QFileInfo(candidate).isExecutable())
            return QDir::cleanPath(candidate);
    return QDir(appDir).filePath(binaryName());
}

bool disabledByEnvironment()
{
    const QString value = qEnvironmentVariable("FORKMESH_CORE").trimmed().toLower();
    return value == QLatin1String("0") || value == QLatin1String("off") ||
           value == QLatin1String("false");
}

QByteArray encodeLine(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

} // namespace forkmesh::core

using namespace forkmesh::core;

CoreClient::CoreClient(QObject *parent) : QObject(parent)
{
    m_retry = new QTimer(this);
    m_retry->setSingleShot(true);
    connect(m_retry, &QTimer::timeout, this, &CoreClient::tryConnect);
    m_sweep = new QTimer(this);
    m_sweep->setInterval(1000);
    connect(m_sweep, &QTimer::timeout, this, &CoreClient::sweep);
    m_stateDir = defaultStateDirectory();
    m_binary = defaultBinaryPath();
}

CoreClient::~CoreClient()
{
    m_running = false;
    if (m_device) {
        m_device->disconnect(this);
        m_device->close();
    }
}

void CoreClient::start()
{
    if (m_running || disabledByEnvironment())
        return;
    m_running = true;
    m_sweep->start();
    tryConnect();
}

void CoreClient::stop()
{
    m_running = false;
    m_retry->stop();
    m_sweep->stop();
    if (m_device) {
        m_device->disconnect(this);
        m_device->close();
        m_device->deleteLater();
        m_device = nullptr;
    }
    const bool was = m_connected;
    m_connected = false;
    failAll(QStringLiteral("core_gone"));
    if (was)
        emit connectedChanged(false);
}

bool CoreClient::hasNativeRoute(const QString &method) const
{
    if (m_exact.contains(method))
        return true;
    for (const QString &prefix : m_families)
        if (method.startsWith(prefix))
            return true;
    return false;
}

QStringList CoreClient::nativeRoutes() const
{
    QStringList out = m_exact.values();
    out += m_families;
    out.sort();
    return out;
}

void CoreClient::scheduleConnect(int delayMs)
{
    if (!m_running)
        return;
    m_retry->start(delayMs);
}

void CoreClient::tryConnect()
{
    if (!m_running || m_device)
        return;
    m_endpoint = readEndpoint(m_stateDir);
    if (!m_endpoint.valid()) {
        launch();
        ++m_attempts;
        scheduleConnect(m_attempts < 30 ? 200 : qMin(30000, 2000 * (m_attempts - 29)));
        return;
    }
    openDevice();
}

void CoreClient::launch()
{
    if (!m_autoLaunch)
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_lastLaunchMs < kRelaunchSpacingMs)
        return;
    if (!QFileInfo(m_binary).isExecutable()) {
        if (m_lastError.isEmpty() || !m_lastError.startsWith(QStringLiteral("missing")))
            emit logLine(QStringLiteral("forkmesh-core not found at %1; the desktop runs "
                                        "without it.").arg(QDir::toNativeSeparators(m_binary)));
        m_lastError = QStringLiteral("missing binary");
        return;
    }
    m_lastLaunchMs = now;
    QStringList arguments{QStringLiteral("serve"), QStringLiteral("--state-dir"), m_stateDir};
    if (!m_project.isEmpty())
        arguments << QStringLiteral("--project") << m_project;
    qint64 pid = 0;
    if (forkmesh::processreaper::startReleased(m_binary, arguments, QDir::homePath(), &pid))
        emit logLine(QStringLiteral("Started forkmesh-core (pid %1).").arg(pid));
    else
        emit logLine(QStringLiteral("Could not start forkmesh-core from %1.")
                         .arg(QDir::toNativeSeparators(m_binary)));
}

void CoreClient::openDevice()
{
    m_buffer.clear();
    m_helloSent = false;
#ifndef Q_OS_WIN
    if (!m_endpoint.unixPath.isEmpty()) {
        auto *socket = new QLocalSocket(this);
        m_device = socket;
        connect(socket, &QLocalSocket::connected, this, &CoreClient::onDeviceConnected);
        connect(socket, &QLocalSocket::readyRead, this, &CoreClient::onReadyRead);
        connect(socket, &QLocalSocket::disconnected, this, &CoreClient::onDisconnected);
        connect(socket, &QLocalSocket::errorOccurred, this, [this, socket] {
            if (m_device == socket && socket->state() != QLocalSocket::ConnectedState)
                onDisconnected();
        });
        socket->connectToServer(m_endpoint.unixPath);
        return;
    }
#endif
    const int colon = m_endpoint.tcp.lastIndexOf(QLatin1Char(':'));
    auto *socket = new QTcpSocket(this);
    m_device = socket;
    connect(socket, &QTcpSocket::connected, this, &CoreClient::onDeviceConnected);
    connect(socket, &QTcpSocket::readyRead, this, &CoreClient::onReadyRead);
    connect(socket, &QTcpSocket::disconnected, this, &CoreClient::onDisconnected);
    connect(socket, &QTcpSocket::errorOccurred, this, [this, socket] {
        if (m_device == socket && socket->state() != QAbstractSocket::ConnectedState)
            onDisconnected();
    });
    socket->connectToHost(m_endpoint.tcp.left(colon), quint16(m_endpoint.tcp.mid(colon + 1).toUInt()));
}

void CoreClient::onDeviceConnected()
{
    m_helloSent = true;
    write({{QStringLiteral("id"), 0x7fffffff},
           {QStringLiteral("method"), QStringLiteral("core.hello")},
           {QStringLiteral("params"),
            QJsonObject{{QStringLiteral("role"), QStringLiteral("ui")},
                        {QStringLiteral("token"), m_endpoint.token},
                        {QStringLiteral("name"), QStringLiteral("forkmesh-desktop")},
                        {QStringLiteral("protocol"), 2},
                        {QStringLiteral("pid"), double(QCoreApplication::applicationPid())}}}});
}

void CoreClient::onReadyRead()
{
    if (!m_device)
        return;
    m_buffer.append(m_device->readAll());
    qsizetype start = 0;
    for (;;) {
        const qsizetype end = m_buffer.indexOf('\n', start);
        if (end < 0)
            break;
        const QByteArray line = m_buffer.mid(start, end - start);
        start = end + 1;
        if (!line.isEmpty())
            handleLine(line);
        if (!m_device)
            return;
    }
    m_buffer.remove(0, start);
    if (m_buffer.size() > kMaxLineBytes)
        onDisconnected();
}

void CoreClient::onDisconnected()
{
    if (!m_device)
        return;
    QIODevice *device = m_device;
    m_device = nullptr;
    device->disconnect(this);
    device->deleteLater();
    const bool was = m_connected;
    m_connected = false;
    failAll(QStringLiteral("core_gone"));
    if (was) {
        emit logLine(QStringLiteral("Lost the connection to forkmesh-core."));
        emit connectedChanged(false);
    }
    if (m_restarting) {
        m_restarting = false;
        m_lastLaunchMs = 0;
        m_attempts = 0;
        scheduleConnect(500);
        return;
    }
    if (!was)
        launch();
    ++m_attempts;
    scheduleConnect(m_attempts < 30 ? 300 : 5000);
}

void CoreClient::handleLine(const QByteArray &line)
{
    const QJsonObject message = QJsonDocument::fromJson(line).object();
    if (message.isEmpty())
        return;
    const int id = message.value(QStringLiteral("id")).toInt();
    const QString method = message.value(QStringLiteral("method")).toString();
    if (!method.isEmpty()) {
        const QJsonObject params = message.value(QStringLiteral("params")).toObject();
        const auto handler = m_handlers.constFind(method);
        if (handler == m_handlers.constEnd()) {
            if (id > 0)
                write({{QStringLiteral("id"), id},
                       {QStringLiteral("ok"), false},
                       {QStringLiteral("error"), QStringLiteral("unsupported")}});
            return;
        }
        QPointer<CoreClient> self(this);
        const Responder respond = [self, id](const QJsonObject &result, const QString &error) {
            if (!self || id <= 0)
                return;
            if (error.isEmpty())
                self->write({{QStringLiteral("id"), id},
                             {QStringLiteral("ok"), true},
                             {QStringLiteral("result"), result}});
            else
                self->write({{QStringLiteral("id"), id},
                             {QStringLiteral("ok"), false},
                             {QStringLiteral("error"), error}});
        };
        (*handler)(params, respond);
        return;
    }
    if (message.contains(QStringLiteral("ok")) && id > 0) {
        if (id == 0x7fffffff) {
            if (message.value(QStringLiteral("ok")).toBool())
                finishHello(message.value(QStringLiteral("result")).toObject());
            else {
                m_lastError = message.value(QStringLiteral("error")).toString();
                onDisconnected();
            }
            return;
        }
        const Pending pending = m_pending.take(id);
        if (pending.reply)
            pending.reply(message.value(QStringLiteral("ok")).toBool(),
                          message.value(QStringLiteral("result")),
                          message.value(QStringLiteral("error")).toString());
        return;
    }
    const QString event = message.value(QStringLiteral("event")).toString();
    if (event == QLatin1String("core.progress")) {
        const QJsonObject data = message.value(QStringLiteral("data")).toObject();
        const int requestId = data.value(QStringLiteral("id")).toInt();
        if (m_pending.contains(requestId))
            emit progress(requestId, data.value(QStringLiteral("progress")));
        return;
    }
    if (!event.isEmpty())
        emit coreEvent(event, message.value(QStringLiteral("data")).toObject());
}

bool CoreClient::binaryNewerThanCore() const
{
    const QFileInfo info(m_binary);
    if (!info.exists() || m_endpoint.startedMs <= 0)
        return false;
    return info.lastModified().toMSecsSinceEpoch() > m_endpoint.startedMs + kBinaryNewerSlackMs;
}

bool CoreClient::blocksUpgrade(const QJsonArray &busy)
{
    static const QSet<QString> survives{QStringLiteral("agentproc"), QStringLiteral("control"),
                                        QStringLiteral("mesh"), QStringLiteral("relay")};
    for (const QJsonValue &value : busy)
        if (!survives.contains(value.toString()))
            return true;
    return false;
}

void CoreClient::restartForUpgrade()
{
    m_upgradeDeferred = false;
    if (m_upgradeTimer)
        m_upgradeTimer->stop();
    m_restarting = true;
    write({{QStringLiteral("id"), m_nextId++},
           {QStringLiteral("method"), QStringLiteral("core.shutdown")},
           {QStringLiteral("params"), QJsonObject{}}});
}

void CoreClient::checkDeferredUpgrade()
{
    if (!m_upgradeDeferred || !m_connected || !binaryNewerThanCore()) {
        if (m_upgradeTimer)
            m_upgradeTimer->stop();
        m_upgradeDeferred = false;
        return;
    }
    request(QStringLiteral("core.status"), {},
            [this](bool ok, const QJsonValue &result, const QString &) {
                if (!ok || !m_upgradeDeferred)
                    return;
                if (blocksUpgrade(result.toObject().value(QStringLiteral("busy")).toArray()))
                    return;
                emit logLine(QStringLiteral("forkmesh-core's work finished; restarting it on the "
                                            "newer binary."));
                restartForUpgrade();
            });
}

void CoreClient::finishHello(const QJsonObject &result)
{
    m_endpoint.version = result.value(QStringLiteral("version")).toString(m_endpoint.version);
    m_endpoint.pid = qint64(result.value(QStringLiteral("pid")).toDouble(double(m_endpoint.pid)));
    if (m_autoLaunch && binaryNewerThanCore()) {
        const QJsonArray busy = result.value(QStringLiteral("busy")).toArray();
        if (!blocksUpgrade(busy)) {
            emit logLine(QStringLiteral("forkmesh-core %1 is older than the binary on disk; "
                                        "restarting it.").arg(m_endpoint.version));
            restartForUpgrade();
            return;
        }
        if (!m_upgradeDeferred)
            emit logLine(QStringLiteral("forkmesh-core %1 is older than the binary on disk; "
                                        "the restart waits until its running work finishes.")
                             .arg(m_endpoint.version));
        m_upgradeDeferred = true;
        if (!m_upgradeTimer) {
            m_upgradeTimer = new QTimer(this);
            m_upgradeTimer->setInterval(60000);
            connect(m_upgradeTimer, &QTimer::timeout, this, &CoreClient::checkDeferredUpgrade);
        }
        m_upgradeTimer->start();
    }
    m_protocol = result.value(QStringLiteral("protocol")).toInt(1);
    m_features.clear();
    for (const QJsonValue &feature : result.value(QStringLiteral("features")).toArray())
        m_features.insert(feature.toString());
    m_exact.clear();
    m_families.clear();
    const QJsonArray routes = result.value(QStringLiteral("routes")).toArray();
    for (const QJsonValue &value : routes) {
        const QJsonObject route = value.toObject();
        const QString pattern = route.value(QStringLiteral("pattern")).toString();
        if (route.value(QStringLiteral("family")).toBool())
            m_families << pattern;
        else
            m_exact.insert(pattern);
    }
    m_attempts = 0;
    m_connected = true;
    m_lastError.clear();
    emit logLine(QStringLiteral("Connected to forkmesh-core %1 (pid %2, %3 native methods).")
                     .arg(m_endpoint.version)
                     .arg(m_endpoint.pid)
                     .arg(m_exact.size() + m_families.size()));
    emit connectedChanged(true);
}

void CoreClient::failAll(const QString &error)
{
    const QHash<int, Pending> pending = std::exchange(m_pending, {});
    for (const Pending &entry : pending)
        if (entry.reply)
            entry.reply(false, QJsonValue(), error);
}

void CoreClient::sweep()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<int> expired;
    for (auto it = m_pending.cbegin(); it != m_pending.cend(); ++it)
        if (it->deadlineMs > 0 && it->deadlineMs <= now)
            expired.append(it.key());
    for (int id : std::as_const(expired)) {
        const Pending pending = m_pending.take(id);
        if (pending.reply)
            pending.reply(false, QJsonValue(), QStringLiteral("core_timeout"));
    }
}

bool CoreClient::write(const QJsonObject &object)
{
    if (!m_device || !m_helloSent)
        return false;
    return m_device->write(encodeLine(object)) > 0;
}

int CoreClient::request(const QString &method, const QJsonObject &params, Reply reply,
                        int timeoutMs)
{
    if (!m_connected) {
        if (reply)
            reply(false, QJsonValue(), QStringLiteral("core_unavailable"));
        return 0;
    }
    const int id = m_nextId++;
    if (m_nextId >= 0x7ffffff0)
        m_nextId = 1;
    Pending pending;
    pending.reply = std::move(reply);
    pending.deadlineMs = timeoutMs > 0 ? QDateTime::currentMSecsSinceEpoch() + timeoutMs : 0;
    m_pending.insert(id, pending);
    if (!write({{QStringLiteral("id"), id},
                {QStringLiteral("method"), method},
                {QStringLiteral("params"), params}})) {
        const Pending failed = m_pending.take(id);
        if (failed.reply)
            failed.reply(false, QJsonValue(), QStringLiteral("core_unavailable"));
        return 0;
    }
    return id;
}

bool CoreClient::cancel(int id)
{
    if (id <= 0 || !m_pending.contains(id))
        return false;
    const Pending pending = m_pending.take(id);
    if (m_connected && supports(QStringLiteral("cancel")))
        write({{QStringLiteral("method"), QStringLiteral("core.cancel")},
               {QStringLiteral("params"), QJsonObject{{QStringLiteral("id"), id}}}});
    if (pending.reply)
        pending.reply(false, QJsonValue(), QStringLiteral("cancelled"));
    return true;
}

bool CoreClient::notify(const QString &method, const QJsonObject &params)
{
    if (!m_connected)
        return false;
    return write({{QStringLiteral("method"), method}, {QStringLiteral("params"), params}});
}

void CoreClient::handle(const QString &method, Handler handler)
{
    if (handler)
        m_handlers.insert(method, std::move(handler));
    else
        m_handlers.remove(method);
}

bool CoreClient::adoptControlSocket(QTcpSocket *socket, const QByteArray &hello,
                                    const QByteArray &rest)
{
    if (!socket || !m_connected)
        return false;
    const QString peer = socket->peerAddress().toString() + QLatin1Char(':') +
                         QString::number(socket->peerPort());
    QJsonObject params{{QStringLiteral("token"), m_endpoint.token},
                       {QStringLiteral("peer"), peer},
                       {QStringLiteral("hello"), QString::fromLatin1(hello.toBase64())},
                       {QStringLiteral("rest"), QString::fromLatin1(rest.toBase64())}};
#ifndef Q_OS_WIN
    if (!m_endpoint.unixPath.isEmpty()) {
        params.insert(QStringLiteral("pass"), QStringLiteral("fd"));
        if (adoptByDescriptor(socket, encodeLine({{QStringLiteral("id"), 1},
                                                  {QStringLiteral("method"), QStringLiteral("control.adopt")},
                                                  {QStringLiteral("params"), params}})))
            return true;
    }
#endif
    params.insert(QStringLiteral("pass"), QStringLiteral("pipe"));
    return adoptByPipe(socket, encodeLine({{QStringLiteral("id"), 1},
                                           {QStringLiteral("method"), QStringLiteral("control.adopt")},
                                           {QStringLiteral("params"), params}}));
}

bool CoreClient::adoptByDescriptor(QTcpSocket *socket, const QByteArray &line)
{
#ifdef Q_OS_WIN
    Q_UNUSED(socket);
    Q_UNUSED(line);
    return false;
#else
    const QByteArray path = QFile::encodeName(m_endpoint.unixPath);
    sockaddr_un address{};
    if (path.size() >= qsizetype(sizeof(address.sun_path)))
        return false;
    const int tcpFd = int(socket->socketDescriptor());
    if (tcpFd < 0)
        return false;
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return false;
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.constData(), size_t(path.size()));
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        ::close(fd);
        return false;
    }
    iovec vector{};
    vector.iov_base = const_cast<char *>(line.constData());
    vector.iov_len = size_t(line.size());
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = &vector;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    cmsghdr *header = CMSG_FIRSTHDR(&message);
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    std::memcpy(CMSG_DATA(header), &tcpFd, sizeof(int));
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    ssize_t sent = ::sendmsg(fd, &message, flags);
    if (sent <= 0) {
        ::close(fd);
        return false;
    }
    while (sent < line.size()) {
        const ssize_t more = ::send(fd, line.constData() + sent, size_t(line.size() - sent), flags);
        if (more <= 0) {
            if (more < 0 && errno == EINTR)
                continue;
            break;
        }
        sent += more;
    }
    ::close(fd);
    socket->disconnect();
    socket->abort();
    socket->deleteLater();
    return true;
#endif
}

bool CoreClient::adoptByPipe(QTcpSocket *phone, const QByteArray &line)
{
    if (m_endpoint.tcp.isEmpty())
        return false;
    const int colon = m_endpoint.tcp.lastIndexOf(QLatin1Char(':'));
    auto *core = new QTcpSocket(this);
    phone->setParent(core);
    phone->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    auto skipping = std::make_shared<bool>(true);
    auto tearDown = [core, phone] {
        phone->disconnect();
        core->disconnect();
        phone->abort();
        core->abort();
        core->deleteLater();
    };
    connect(core, &QTcpSocket::connected, core, [core, phone, line] {
        core->setSocketOption(QAbstractSocket::LowDelayOption, 1);
        core->write(line);
        if (phone->bytesAvailable() > 0)
            core->write(phone->readAll());
    });
    connect(core, &QTcpSocket::readyRead, core, [core, phone, skipping] {
        QByteArray data = core->readAll();
        if (*skipping) {
            const qsizetype newline = data.indexOf('\n');
            if (newline < 0)
                return;
            data.remove(0, newline + 1);
            *skipping = false;
        }
        if (!data.isEmpty())
            phone->write(data);
    });
    connect(phone, &QTcpSocket::readyRead, core, [core, phone] {
        if (core->state() == QAbstractSocket::ConnectedState)
            core->write(phone->readAll());
    });
    connect(core, &QTcpSocket::disconnected, core, tearDown);
    connect(phone, &QTcpSocket::disconnected, core, tearDown);
    connect(core, &QTcpSocket::errorOccurred, core, tearDown);
    core->connectToHost(m_endpoint.tcp.left(colon), quint16(m_endpoint.tcp.mid(colon + 1).toUInt()));
    return true;
}
