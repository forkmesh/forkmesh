#pragma once

// Who is talking to whom over the network, per agent, as it happens.
//
// Nothing else in the desktop can say which agent moved which bytes: the
// agents share the desktop's network namespace, so interface counters and
// /proc/<pid>/net/dev are the whole machine's. What does know is the kernel's
// socket table. sock_diag (the netlink interface `ss` reads) reports every TCP
// socket with its peer and the bytes it has sent and received; each agent's
// process tree holds a known set of those sockets in /proc/<pid>/fd. Put the
// two together, sample often, and diff: that is every agent's traffic, to
// each host, in real time. The desktop itself is sampled the same way, on its
// own pid alone (its children are the agents).
//
// Linux only; elsewhere tcpSockets() is empty and the window simply shows no
// flows. Qt Core, no clock of its own: the caller samples on a worker thread
// and diffs on the UI thread (tests/test_agent_activity.cpp covers the diff).

#include <QHash>
#include <QList>
#include <QSet>
#include <QString>
#include <QtGlobal>

#include "SystemStats.h"

namespace forkmesh::agentnet {

// One process tree to attribute sockets to.
struct Root {
    int sessionId = 0;
    qint64 pid = 0;
    // The desktop's own root counts only its own sockets; an agent's counts
    // everything it started (git, curl, an MCP server...).
    bool withDescendants = true;
};

struct Socket {
    quint64 inode = 0;
    QString peer;       // the remote address, as text
    quint16 port = 0;
    quint64 sent = 0;   // bytes the peer has acknowledged
    quint64 received = 0;
};

// Every TCP socket on the machine, v4 and v6, with its byte counters.
QList<Socket> tcpSockets();
// The socket inodes a process holds open.
QSet<quint64> socketInodes(qint64 pid);

// A process under an agent's root, found on the same walk.
struct Process {
    qint64 pid = 0;
    qint64 parentPid = 0;
    QString name;    // /proc/<pid>/stat's comm
    QString command; // /proc/<pid>/cmdline, spaces for its NULs
    qint64 cpuTicks = 0;
};

struct Tree {
    qint64 rootPid = 0;
    QList<Process> processes; // parents before their children
};

// One worker-thread look: the sockets, which session owns each, and every
// agent's process tree (the desktop's own is not walked).
struct Sample {
    QList<Socket> sockets;
    QHash<quint64, int> owner; // inode -> session id
    QHash<int, Tree> trees;    // session id -> its processes
    QHash<quint64, qint64> holder;
    QHash<qint64, QString> names;
};

// The command line of a process, up to `limit` characters.
QString commandLine(qint64 pid, int limit = 300);
QString processName(qint64 pid);
Sample sample(const QList<Root> &roots);
// The same, off one walk of /proc the caller already made: every root's tree
// comes from `table`, rather than a walk per agent.
Sample sample(const QList<Root> &roots, const SystemStats::ProcessTable &table);

// Bytes one session moved to or from one peer since the last sample.
struct Delta {
    int sessionId = 0;
    QString peer;
    quint16 port = 0;
    qint64 up = 0;
    qint64 down = 0;
    qint64 pid = 0;
    QString process;
};

// The diff between samples. The first sample is a baseline - bytes a socket
// moved before anybody was looking are not traffic now - but a socket first
// seen after that is new, and everything on it counts.
class Ledger
{
public:
    QList<Delta> update(const Sample &sample);
    void reset();

private:
    struct Seen {
        quint64 sent = 0;
        quint64 received = 0;
    };
    QHash<quint64, Seen> m_seen;
    bool m_primed = false;
};

// 127.x and ::1: a peer on this machine (an MCP server, the desktop's own
// bridge), drawn as such rather than as a host out on the network.
bool isLoopback(const QString &peer);

} // namespace forkmesh::agentnet
