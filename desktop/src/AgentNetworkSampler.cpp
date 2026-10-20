#include "AgentNetworkSampler.h"

#include "SystemStats.h"

#include <QDir>
#include <QFile>

#if defined(Q_OS_LINUX)
#include <arpa/inet.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <linux/tcp.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#endif

namespace forkmesh::agentnet {

#if defined(Q_OS_LINUX)
namespace {

// TCP_LISTEN in the kernel's state numbering: a listening socket carries no
// traffic of its own, so it is not asked for.
constexpr int kTcpListen = 10;

void dumpFamily(int fd, int family, QList<Socket> *out)
{
    struct {
        nlmsghdr header;
        inet_diag_req_v2 request;
    } message{};
    message.header.nlmsg_len = sizeof(message);
    message.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    message.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    message.request.sdiag_family = quint8(family);
    message.request.sdiag_protocol = IPPROTO_TCP;
    message.request.idiag_states = ~0u & ~(1u << kTcpListen);
    message.request.idiag_ext = 1 << (INET_DIAG_INFO - 1);

    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    if (sendto(fd, &message, sizeof(message), 0,
               reinterpret_cast<sockaddr *>(&kernel), sizeof(kernel)) < 0)
        return;

    alignas(nlmsghdr) char buffer[65536];
    for (;;) {
        const ssize_t got = recv(fd, buffer, sizeof(buffer), 0);
        if (got <= 0)
            return;
        int remaining = int(got);
        for (auto *header = reinterpret_cast<nlmsghdr *>(buffer);
             NLMSG_OK(header, remaining); header = NLMSG_NEXT(header, remaining)) {
            if (header->nlmsg_type == NLMSG_DONE || header->nlmsg_type == NLMSG_ERROR)
                return;
            const auto *diag = static_cast<const inet_diag_msg *>(NLMSG_DATA(header));
            if (diag->idiag_inode == 0)
                continue; // a socket in TIME_WAIT: nobody holds it any more

            Socket socket;
            socket.inode = diag->idiag_inode;
            socket.port = ntohs(diag->id.idiag_dport);
            char text[INET6_ADDRSTRLEN] = {};
            if (diag->idiag_family == AF_INET)
                inet_ntop(AF_INET, diag->id.idiag_dst, text, sizeof(text));
            else
                inet_ntop(AF_INET6, diag->id.idiag_dst, text, sizeof(text));
            socket.peer = QString::fromLatin1(text);
            // An IPv4 peer on a v6 socket reads as ::ffff:1.2.3.4.
            if (socket.peer.startsWith(QLatin1String("::ffff:")))
                socket.peer = socket.peer.mid(7);

            int length = int(header->nlmsg_len) - int(NLMSG_LENGTH(sizeof(*diag)));
            for (auto *attribute = reinterpret_cast<rtattr *>(
                     const_cast<inet_diag_msg *>(diag) + 1);
                 RTA_OK(attribute, length); attribute = RTA_NEXT(attribute, length)) {
                if (attribute->rta_type != INET_DIAG_INFO)
                    continue;
                tcp_info info{};
                std::memcpy(&info, RTA_DATA(attribute),
                            qMin<size_t>(sizeof(info), RTA_PAYLOAD(attribute)));
                socket.sent = info.tcpi_bytes_acked;
                socket.received = info.tcpi_bytes_received;
            }
            out->append(socket);
        }
    }
}

} // namespace
#endif

QList<Socket> tcpSockets()
{
    QList<Socket> sockets;
#if defined(Q_OS_LINUX)
    const int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0)
        return sockets;
    dumpFamily(fd, AF_INET, &sockets);
    dumpFamily(fd, AF_INET6, &sockets);
    ::close(fd);
#endif
    return sockets;
}

QSet<quint64> socketInodes(qint64 pid)
{
    QSet<quint64> inodes;
#if defined(Q_OS_LINUX)
    const QString dir = QStringLiteral("/proc/%1/fd").arg(pid);
    const QStringList fds = QDir(dir).entryList(QDir::Files | QDir::System | QDir::NoDotAndDotDot);
    for (const QString &fd : fds) {
        // The link's own text, "socket:[12345]": nothing to resolve.
        const QByteArray path = QFile::encodeName(dir + QLatin1Char('/') + fd);
        char link[64] = {};
        const ssize_t n = ::readlink(path.constData(), link, sizeof(link) - 1);
        if (n <= 9 || std::strncmp(link, "socket:[", 8) != 0)
            continue;
        inodes.insert(QByteArray(link + 8, int(n) - 9).toULongLong());
    }
#else
    Q_UNUSED(pid);
#endif
    return inodes;
}

QString commandLine(qint64 pid, int limit)
{
    QFile file(QStringLiteral("/proc/%1/cmdline").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    QByteArray bytes = file.read(limit * 2);
    bytes.replace('\0', ' ');
    return QString::fromLocal8Bit(bytes).trimmed().left(limit);
}

QString processName(qint64 pid)
{
    QFile file(QStringLiteral("/proc/%1/comm").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QString::fromLocal8Bit(file.read(64)).trimmed();
}

Sample sample(const QList<Root> &roots)
{
    bool walks = false;
    for (const Root &root : roots)
        walks = walks || (root.pid > 0 && root.withDescendants);
    return sample(roots, walks ? SystemStats::processTable() : SystemStats::ProcessTable());
}

Sample sample(const QList<Root> &roots, const SystemStats::ProcessTable &table)
{
    Sample result;
    for (const Root &root : roots) {
        if (root.pid <= 0)
            continue;
        QList<qint64> pids{root.pid};
        if (root.withDescendants) {
            Tree &tree = result.trees[root.sessionId];
            tree.rootPid = root.pid;
            for (const SystemStats::DescendantProcess &child :
                 SystemStats::descendantProcesses(root.pid, table)) {
                pids.append(child.pid);
                tree.processes.append({child.pid, child.parentPid, child.command,
                                       commandLine(child.pid), child.cpuTicks});
            }
        }
        for (const qint64 pid : std::as_const(pids)) {
            bool holds = false;
            for (const quint64 inode : socketInodes(pid)) {
                result.owner.insert(inode, root.sessionId);
                result.holder.insert(inode, pid);
                holds = true;
            }
            if (!holds || result.names.contains(pid))
                continue;
            const auto entry = table.entries.constFind(pid);
            result.names.insert(pid, entry != table.entries.constEnd() ? entry->command
                                                                       : processName(pid));
        }
    }
    if (!result.owner.isEmpty())
        result.sockets = tcpSockets();
    return result;
}

QList<Delta> Ledger::update(const Sample &sample)
{
    QHash<quint64, Seen> next;
    QHash<QString, Delta> merged; // "session|peer"
    for (const Socket &socket : sample.sockets) {
        const auto owner = sample.owner.constFind(socket.inode);
        if (owner == sample.owner.constEnd())
            continue;
        next.insert(socket.inode, Seen{socket.sent, socket.received});
        const auto before = m_seen.constFind(socket.inode);
        qint64 up = 0;
        qint64 down = 0;
        if (before != m_seen.constEnd()) {
            // Counters only climb; a smaller one is a reused inode.
            up = socket.sent >= before->sent ? qint64(socket.sent - before->sent)
                                             : qint64(socket.sent);
            down = socket.received >= before->received
                       ? qint64(socket.received - before->received)
                       : qint64(socket.received);
        } else if (m_primed) {
            up = qint64(socket.sent);
            down = qint64(socket.received);
        }
        if (up == 0 && down == 0)
            continue;
        // Per port too: two services on one address are two connections.
        const qint64 pid = sample.holder.value(socket.inode);
        const QString key = QString::number(owner.value()) + QLatin1Char('|') + socket.peer
                            + QLatin1Char('|') + QString::number(socket.port)
                            + QLatin1Char('|') + QString::number(pid);
        Delta &delta = merged[key];
        delta.sessionId = owner.value();
        delta.peer = socket.peer;
        delta.port = socket.port;
        delta.pid = pid;
        delta.process = sample.names.value(pid);
        delta.up += up;
        delta.down += down;
    }
    m_seen = next;
    m_primed = true;
    return merged.values();
}

void Ledger::reset()
{
    m_seen.clear();
    m_primed = false;
}

bool isLoopback(const QString &peer)
{
    return peer.startsWith(QLatin1String("127.")) || peer == QLatin1String("::1");
}

} // namespace forkmesh::agentnet
