#include "CoreAgentStore.h"

#include "CoreClient.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QMutexLocker>

#include <atomic>
#include <cstring>

#ifdef Q_OS_WIN
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace forkmesh::coreagentstore {

namespace {

constexpr qint64 kHelloId = 1;
constexpr int kHelloMs = 5000;
constexpr qsizetype kMaxLineBytes = 96 * 1024 * 1024;

#ifdef Q_OS_WIN
using SocketHandle = SOCKET;
const SocketHandle kNoSocket = INVALID_SOCKET;

bool startSockets()
{
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return started;
}

void closeSocket(SocketHandle fd)
{
    ::closesocket(fd);
}

bool interrupted()
{
    return WSAGetLastError() == WSAEINTR;
}

bool wouldBlock()
{
    return WSAGetLastError() == WSAEWOULDBLOCK;
}

int waitSocket(SocketHandle fd, short events, int ms)
{
    WSAPOLLFD pfd{};
    pfd.fd = fd;
    pfd.events = events;
    return ::WSAPoll(&pfd, 1, ms);
}

qint64 sendBytes(SocketHandle fd, const char *data, qsizetype size)
{
    return ::send(fd, data, int(qMin<qsizetype>(size, 1 << 30)), 0);
}

qint64 receiveBytes(SocketHandle fd, char *data, qsizetype size)
{
    return ::recv(fd, data, int(size), 0);
}
#else
using SocketHandle = int;
const SocketHandle kNoSocket = -1;

bool startSockets()
{
    return true;
}

void closeSocket(SocketHandle fd)
{
    ::close(fd);
}

bool interrupted()
{
    return errno == EINTR;
}

bool wouldBlock()
{
    return errno == EAGAIN || errno == EWOULDBLOCK;
}

int waitSocket(SocketHandle fd, short events, int ms)
{
    pollfd pfd{fd, events, 0};
    return ::poll(&pfd, 1, ms);
}

qint64 sendBytes(SocketHandle fd, const char *data, qsizetype size)
{
#ifdef MSG_NOSIGNAL
    return ::send(fd, data, size_t(size), MSG_NOSIGNAL);
#else
    return ::send(fd, data, size_t(size), 0);
#endif
}

qint64 receiveBytes(SocketHandle fd, char *data, qsizetype size)
{
    return ::recv(fd, data, size_t(size), 0);
}

SocketHandle openCloseOnExec(int domain)
{
#ifdef SOCK_CLOEXEC
    return ::socket(domain, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const SocketHandle fd = ::socket(domain, SOCK_STREAM, 0);
    if (fd >= 0)
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
#endif
}

void suppressSigpipe(SocketHandle fd)
{
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    Q_UNUSED(fd)
#endif
}

SocketHandle connectUnix(const QString &socketPath)
{
    if (socketPath.isEmpty())
        return kNoSocket;
    const QByteArray path = QFile::encodeName(socketPath);
    sockaddr_un address{};
    if (path.size() >= qsizetype(sizeof(address.sun_path)))
        return kNoSocket;
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, path.constData(), size_t(path.size()));
    const SocketHandle fd = openCloseOnExec(AF_UNIX);
    if (fd < 0)
        return kNoSocket;
    suppressSigpipe(fd);
    if (::connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        closeSocket(fd);
        return kNoSocket;
    }
    return fd;
}
#endif

SocketHandle connectLoopback(const QString &endpoint)
{
    const int colon = endpoint.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0 || !startSockets())
        return kNoSocket;
    bool portOk = false;
    const uint port = endpoint.mid(colon + 1).toUInt(&portOk);
    const QHostAddress host(endpoint.left(colon));
    bool ipv4 = false;
    const quint32 address = host.toIPv4Address(&ipv4);
    if (!portOk || port == 0 || port > 65535 || !ipv4 || !host.isLoopback())
        return kNoSocket;
    sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_port = htons(quint16(port));
    target.sin_addr.s_addr = htonl(address);
#ifdef Q_OS_WIN
    const SocketHandle fd = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd != kNoSocket)
        ::SetHandleInformation(reinterpret_cast<HANDLE>(fd), HANDLE_FLAG_INHERIT, 0);
#else
    const SocketHandle fd = openCloseOnExec(AF_INET);
#endif
    if (fd == kNoSocket)
        return kNoSocket;
#ifndef Q_OS_WIN
    suppressSigpipe(fd);
#endif
    if (::connect(fd, reinterpret_cast<sockaddr *>(&target), sizeof(target)) != 0) {
        closeSocket(fd);
        return kNoSocket;
    }
    const int on = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&on), sizeof(on));
    return fd;
}

struct LinkState {
    QMutex mutex;
    bool attached = false;
    forkmesh::core::Endpoint endpoint;
    QString coreRoot;
    std::atomic<quint64> generation{1};
    quint64 checkedGeneration = 0;
    QString checkedRoot;
    bool checkedAnswer = false;
};

LinkState &link()
{
    static LinkState state;
    return state;
}

QString canonical(const QString &path)
{
    const QString clean = QDir::cleanPath(QDir(path).absolutePath());
    const QString resolved = QFileInfo(clean).canonicalFilePath();
    return resolved.isEmpty() ? clean : resolved;
}

} // namespace

class Channel
{
public:
    explicit Channel(const forkmesh::core::Endpoint &endpoint) : m_endpoint(endpoint) {}
    ~Channel() { close(); }

    bool open()
    {
#ifndef Q_OS_WIN
        m_fd = connectUnix(m_endpoint.unixPath);
#endif
        if (m_fd == kNoSocket)
            m_fd = connectLoopback(m_endpoint.tcp);
        if (m_fd == kNoSocket)
            return false;
        QJsonObject reply;
        if (!send({{QStringLiteral("id"), double(kHelloId)},
                   {QStringLiteral("method"), QStringLiteral("core.hello")},
                   {QStringLiteral("params"),
                    QJsonObject{{QStringLiteral("role"), QStringLiteral("cli")},
                                {QStringLiteral("token"), m_endpoint.token},
                                {QStringLiteral("name"), QStringLiteral("forkmesh-desktop-agentstore")},
                                {QStringLiteral("pid"), double(QCoreApplication::applicationPid())}}}}) ||
            !receive(kHelloId, kHelloMs, &reply) || !reply.value(QStringLiteral("ok")).toBool()) {
            close();
            return false;
        }
        const QJsonArray table =
            reply.value(QStringLiteral("result")).toObject().value(QStringLiteral("routes")).toArray();
        for (const QJsonValue &value : table)
            m_routes.append(value.toObject().value(QStringLiteral("pattern")).toString());
        return true;
    }

    bool hasRoute(const QString &method) const { return m_routes.contains(method); }
    bool isOpen() const { return m_fd != kNoSocket; }

    bool send(const QJsonObject &object)
    {
        if (m_fd == kNoSocket)
            return false;
        const QByteArray line = QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
        qsizetype sent = 0;
        while (sent < line.size()) {
            const qint64 n = sendBytes(m_fd, line.constData() + sent, line.size() - sent);
            if (n > 0) {
                sent += qsizetype(n);
                continue;
            }
            if (n < 0 && interrupted())
                continue;
            if (n < 0 && wouldBlock() && waitSocket(m_fd, POLLOUT, 10000) > 0)
                continue;
            close();
            return false;
        }
        return true;
    }

    bool request(const QString &method, const QJsonObject &params, int timeoutMs,
                 QJsonObject *result, QString *error)
    {
        const qint64 id = ++m_nextId;
        if (!send({{QStringLiteral("id"), double(id)},
                   {QStringLiteral("method"), method},
                   {QStringLiteral("params"), params}})) {
            if (error)
                *error = QStringLiteral("core_unavailable");
            return false;
        }
        QJsonObject reply;
        if (!receive(id, timeoutMs, &reply)) {
            if (error)
                *error = QStringLiteral("core_gone");
            close();
            return false;
        }
        if (!reply.value(QStringLiteral("ok")).toBool()) {
            if (error) {
                *error = reply.value(QStringLiteral("error")).toString();
                if (error->isEmpty())
                    *error = QStringLiteral("core_error");
            }
            return false;
        }
        if (result)
            *result = reply.value(QStringLiteral("result")).toObject();
        return true;
    }

    void close()
    {
        if (m_fd != kNoSocket)
            closeSocket(m_fd);
        m_fd = kNoSocket;
        m_buffer.clear();
    }

private:
    bool receive(qint64 id, int timeoutMs, QJsonObject *message)
    {
        const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + qMax(1, timeoutMs);
        for (;;) {
            qsizetype newline = m_buffer.indexOf('\n');
            while (newline >= 0) {
                const QByteArray line = m_buffer.left(newline);
                m_buffer.remove(0, newline + 1);
                const QJsonObject object = QJsonDocument::fromJson(line).object();
                if (object.contains(QStringLiteral("ok")) && !object.contains(QStringLiteral("method")) &&
                    qint64(object.value(QStringLiteral("id")).toDouble()) == id) {
                    *message = object;
                    return true;
                }
                newline = m_buffer.indexOf('\n');
            }
            if (m_fd == kNoSocket || m_buffer.size() > kMaxLineBytes)
                return false;
            const qint64 remaining = deadline - QDateTime::currentMSecsSinceEpoch();
            if (remaining <= 0)
                return false;
            const int ready = waitSocket(m_fd, POLLIN, int(qMin<qint64>(remaining, 1000)));
            if (ready < 0 && interrupted())
                continue;
            if (ready < 0)
                return false;
            if (ready == 0)
                continue;
            char chunk[65536];
            const qint64 n = receiveBytes(m_fd, chunk, qsizetype(sizeof(chunk)));
            if (n < 0 && interrupted())
                continue;
            if (n <= 0)
                return false;
            m_buffer.append(chunk, qsizetype(n));
        }
    }

    forkmesh::core::Endpoint m_endpoint;
    SocketHandle m_fd = kNoSocket;
    qint64 m_nextId = kHelloId;
    QByteArray m_buffer;
    QStringList m_routes;
};

namespace {

struct MetaChannel {
    QMutex mutex;
    std::unique_ptr<Channel> channel;
    quint64 generation = 0;
};

MetaChannel &metaChannel()
{
    static MetaChannel state;
    return state;
}

bool snapshot(forkmesh::core::Endpoint *endpoint, quint64 *generation)
{
    QMutexLocker lock(&link().mutex);
    if (!link().attached)
        return false;
    *endpoint = link().endpoint;
    *generation = link().generation.load();
    return true;
}

} // namespace

void attach(const forkmesh::core::Endpoint &endpoint)
{
    Channel probe(endpoint);
    QString root;
    if (endpoint.valid() && probe.open() &&
        probe.hasRoute(QStringLiteral("core.agentstore.appendEvent"))) {
        QJsonObject result;
        if (probe.request(QStringLiteral("core.agentstore.root"), {}, 5000, &result, nullptr))
            root = result.value(QStringLiteral("root")).toString();
    }
    QMutexLocker lock(&link().mutex);
    link().attached = !root.isEmpty();
    link().endpoint = endpoint;
    link().coreRoot = root.isEmpty() ? QString() : canonical(root);
    link().generation.fetch_add(1);
}

void detach()
{
    QMutexLocker lock(&link().mutex);
    link().attached = false;
    link().coreRoot.clear();
    link().generation.fetch_add(1);
}

bool attached()
{
    QMutexLocker lock(&link().mutex);
    return link().attached;
}

quint64 generation()
{
    return link().generation.load();
}

bool routes(const QString &storeRoot)
{
    QMutexLocker lock(&link().mutex);
    if (!link().attached || link().coreRoot.isEmpty())
        return false;
    if (link().checkedGeneration == link().generation.load() && link().checkedRoot == storeRoot)
        return link().checkedAnswer;
    link().checkedGeneration = link().generation.load();
    link().checkedRoot = storeRoot;
    link().checkedAnswer = canonical(storeRoot) == link().coreRoot;
    return link().checkedAnswer;
}

bool call(const QString &method, const QJsonObject &params, QJsonObject *result, QString *error,
          int timeoutMs)
{
    forkmesh::core::Endpoint endpoint;
    quint64 gen = 0;
    if (!snapshot(&endpoint, &gen)) {
        if (error)
            *error = QStringLiteral("core_unavailable");
        return false;
    }
    MetaChannel &meta = metaChannel();
    QMutexLocker lock(&meta.mutex);
    if (!meta.channel || meta.generation != gen || !meta.channel->isOpen()) {
        meta.channel = std::make_unique<Channel>(endpoint);
        meta.generation = gen;
        if (!meta.channel->open()) {
            meta.channel.reset();
            if (error)
                *error = QStringLiteral("core_unavailable");
            return false;
        }
    }
    return meta.channel->request(method, params, timeoutMs, result, error);
}

Lane::Lane() = default;

Lane::~Lane() = default;

bool Lane::ensure()
{
    forkmesh::core::Endpoint endpoint;
    quint64 gen = 0;
    if (!snapshot(&endpoint, &gen))
        return false;
    if (m_channel && m_generation == gen && m_channel->isOpen())
        return true;
    m_channel = std::make_unique<Channel>(endpoint);
    m_generation = gen;
    m_dirty = false;
    if (!m_channel->open()) {
        m_channel.reset();
        return false;
    }
    return true;
}

bool Lane::notify(const QString &method, const QJsonObject &params)
{
    if (!ensure())
        return false;
    if (!m_channel->send({{QStringLiteral("method"), method}, {QStringLiteral("params"), params}})) {
        m_channel.reset();
        return false;
    }
    m_dirty = true;
    return true;
}

bool Lane::sync(int timeoutMs)
{
    if (!m_dirty)
        return true;
    m_dirty = false;
    if (!m_channel || !m_channel->isOpen())
        return false;
    if (!m_channel->request(QStringLiteral("core.agentstore.sync"), {}, timeoutMs, nullptr, nullptr)) {
        m_channel.reset();
        return false;
    }
    return true;
}

} // namespace forkmesh::coreagentstore
