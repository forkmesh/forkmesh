#include "TerminalHandoff.h"

#include "ClaudePeerMessaging.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>

#include <cerrno>
#include <cstring>

#if defined(Q_OS_UNIX)
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif
#if defined(Q_OS_MACOS)
#include <sys/sysctl.h>
#endif

namespace TerminalHandoff {

namespace {

constexpr int kConnectTimeoutMs = 2000;
constexpr int kReplyTimeoutMs = 3000;
constexpr qint64 kMaxRegistrationBytes = 16 * 1024;

bool dropped(char32_t cp)
{
    return (cp < 0x20 && cp != u'\n' && cp != u'\t') || cp == 0x7f || (cp >= 0x80 && cp <= 0x9f);
}

#if defined(Q_OS_UNIX)
bool ownedPrivately(const QString &path, bool socket)
{
    struct stat st;
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
        return false;
    if (socket ? !S_ISSOCK(st.st_mode) : !S_ISREG(st.st_mode))
        return false;
    return st.st_uid == ::getuid() && (st.st_mode & 077) == 0;
}

bool alive(qint64 pid)
{
    return pid > 0 && ::kill(pid_t(pid), 0) == 0;
}
#endif

TerminalHandoffReply failure(const QString &code, const QString &detail)
{
    return TerminalHandoffReply{false, code, detail};
}

} // namespace

QString runtimeDir()
{
    const QByteArray forced = qgetenv("FORKMESH_TERM_DIR");
    if (!forced.isEmpty())
        return QFile::decodeName(forced);
#if defined(Q_OS_MACOS)
    char buf[1024];
    const size_t n = ::confstr(_CS_DARWIN_USER_TEMP_DIR, buf, sizeof buf);
    if (n > 0 && n <= sizeof buf) {
        QString dir = QFile::decodeName(buf);
        if (!dir.endsWith(QLatin1Char('/')))
            dir += QLatin1Char('/');
        return dir + QStringLiteral("forkmesh-term");
    }
#elif defined(Q_OS_UNIX)
    const QByteArray xdg = qgetenv("XDG_RUNTIME_DIR");
    if (xdg.startsWith('/')) {
        struct stat st;
        if (::stat(xdg.constData(), &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == ::getuid())
            return QFile::decodeName(xdg) + QStringLiteral("/forkmesh-term");
    }
#endif
#if defined(Q_OS_UNIX)
    return QStringLiteral("/tmp/forkmesh-term-%1").arg(::getuid());
#else
    return {};
#endif
}

QList<TerminalHandoffSession> readSessions(const QString &dir)
{
    QList<TerminalHandoffSession> sessions;
#if defined(Q_OS_UNIX)
    if (dir.isEmpty())
        return sessions;
    struct stat st;
    if (::lstat(QFile::encodeName(dir).constData(), &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != ::getuid() || (st.st_mode & 077) != 0)
        return sessions;
    const QDir folder(dir);
    const QString root = QDir::cleanPath(folder.absolutePath()) + QLatin1Char('/');
    for (const QString &name : folder.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        const QString path = folder.filePath(name);
        if (!ownedPrivately(path, false))
            continue;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly))
            continue;
        const QJsonObject o = QJsonDocument::fromJson(file.read(kMaxRegistrationBytes)).object();
        if (o.value(QStringLiteral("v")).toInt() != kProtocolVersion)
            continue;
        TerminalHandoffSession s;
        s.pid = qint64(o.value(QStringLiteral("pid")).toDouble());
        s.start = o.value(QStringLiteral("start")).toString();
        s.child = qint64(o.value(QStringLiteral("child")).toDouble());
        s.cli = o.value(QStringLiteral("cli")).toString();
        s.cwd = o.value(QStringLiteral("cwd")).toString();
        s.socketPath = QDir::cleanPath(o.value(QStringLiteral("socket")).toString());
        s.remote = o.value(QStringLiteral("remote")).toBool();
        s.startedMs = qint64(o.value(QStringLiteral("startedMs")).toDouble());
        s.tty = o.value(QStringLiteral("tty")).toString();
        if (s.pid <= 0 || s.child <= 0 || name != QStringLiteral("%1.json").arg(s.pid) ||
            !s.socketPath.startsWith(root) ||
            s.socketPath.mid(root.size()).contains(QLatin1Char('/')))
            continue;
        sessions.append(s);
    }
#else
    Q_UNUSED(dir);
#endif
    return sessions;
}

QList<TerminalHandoffSession> liveSessions(const QString &dir)
{
    QList<TerminalHandoffSession> live;
#if defined(Q_OS_UNIX)
    for (const TerminalHandoffSession &s : readSessions(dir)) {
        if (!alive(s.pid) || !alive(s.child))
            continue;
        const QString actual = ClaudePeerMessaging::procStart(s.pid);
        if (!s.start.isEmpty() && !actual.isEmpty() && s.start != actual)
            continue;
        if (!ownedPrivately(s.socketPath, true))
            continue;
        live.append(s);
    }
#else
    Q_UNUSED(dir);
#endif
    return live;
}

qint64 parentPid(qint64 pid)
{
#if defined(Q_OS_LINUX)
    QFile stat(QStringLiteral("/proc/%1/stat").arg(pid));
    if (!stat.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray line = stat.read(16 * 1024);
    const int close = line.lastIndexOf(')');
    if (close < 0)
        return 0;
    const QList<QByteArray> fields = line.mid(close + 2).split(' ');
    return fields.size() > 1 ? fields.at(1).toLongLong() : 0;
#elif defined(Q_OS_MACOS)
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, int(pid)};
    struct kinfo_proc info;
    size_t size = sizeof info;
    if (::sysctl(mib, 4, &info, &size, nullptr, 0) != 0 || size == 0)
        return 0;
    return info.kp_eproc.e_ppid;
#else
    Q_UNUSED(pid);
    return 0;
#endif
}

const TerminalHandoffSession *find(const QList<TerminalHandoffSession> &sessions,
                                   const QList<qint64> &pids,
                                   const std::function<qint64(qint64)> &parentOf)
{
    for (const qint64 pid : pids) {
        qint64 current = pid;
        for (int depth = 0; depth < 6 && current > 1; ++depth) {
            for (const TerminalHandoffSession &s : sessions) {
                if (current == s.child || current == s.pid)
                    return &s;
            }
            current = parentOf ? parentOf(current) : 0;
        }
    }
    return nullptr;
}

QString sanitizeText(const QString &text)
{
    QString out;
    out.reserve(text.size());
    const QList<uint> points = text.toUcs4();
    for (int i = 0; i < points.size(); ++i) {
        char32_t cp = points.at(i);
        if (cp == u'\r') {
            out += QLatin1Char('\n');
            if (i + 1 < points.size() && points.at(i + 1) == u'\n')
                ++i;
            continue;
        }
        if (dropped(cp) || cp == 0xfffd)
            continue;
        out += QString::fromUcs4(&cp, 1);
    }
    while (!out.isEmpty() && (out.back() == QLatin1Char('\n') || out.back() == QLatin1Char(' ') ||
                              out.back() == QLatin1Char('\t')))
        out.chop(1);
    return out;
}

QString sanitizeLabel(const QString &label)
{
    QString out;
    int count = 0;
    for (char32_t cp : label.toUcs4()) {
        if (count >= 64)
            break;
        if (cp == u'\n' || cp == u'\r' || cp == u'\t')
            cp = u' ';
        if (dropped(cp))
            continue;
        if (cp == u' ' && (out.isEmpty() || out.back() == QLatin1Char(' ')))
            continue;
        out += QString::fromUcs4(&cp, 1);
        ++count;
    }
    out = out.trimmed();
    return out.isEmpty() ? QStringLiteral("a paired device") : out;
}

QString interruptKey(const QString &cli)
{
    return cli == QLatin1String("claude") || cli == QLatin1String("codex") ? QStringLiteral("esc")
                                                                           : QStringLiteral("ctrl-c");
}

QByteArray requestFrame(const QString &verb, const QString &from, const QByteArray &payload)
{
    const QByteArray label = from.toUtf8();
    return QByteArrayLiteral("FMTERM") + QByteArray::number(kProtocolVersion) + ' ' +
           verb.toLatin1() + ' ' + QByteArray::number(label.size()) + ' ' +
           QByteArray::number(payload.size()) + '\n' + label + payload;
}

TerminalHandoffReply parseReply(const QByteArray &line)
{
    const QByteArray trimmed = line.trimmed();
    const int space = trimmed.indexOf(' ');
    const QByteArray head = space < 0 ? trimmed : trimmed.left(space);
    const QString rest = space < 0 ? QString() : QString::fromUtf8(trimmed.mid(space + 1));
    if (head == "ok")
        return TerminalHandoffReply{true, {}, rest};
    if (head == "err" && !rest.isEmpty())
        return TerminalHandoffReply{false, rest.section(QLatin1Char(' '), 0, 0), rest};
    return failure(QStringLiteral("bad_reply"), QString::fromUtf8(trimmed.left(80)));
}

TerminalHandoffReply exchange(const TerminalHandoffSession &session, const QByteArray &frame)
{
#if defined(Q_OS_UNIX)
    const QByteArray path = QFile::encodeName(session.socketPath);
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.isEmpty() || size_t(path.size()) >= sizeof addr.sun_path)
        return failure(QStringLiteral("unreachable"), QStringLiteral("socket path too long"));
    std::memcpy(addr.sun_path, path.constData(), size_t(path.size()));
    if (!ownedPrivately(session.socketPath, true))
        return failure(QStringLiteral("unreachable"), QStringLiteral("no private socket"));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return failure(QStringLiteral("unreachable"), QString::fromLocal8Bit(std::strerror(errno)));
    struct Closer {
        int fd;
        ~Closer() { ::close(fd); }
    } closer{fd};
    timeval timeout{kConnectTimeoutMs / 1000, (kConnectTimeoutMs % 1000) * 1000};
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
#if defined(SO_NOSIGPIPE)
    const int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0)
        return failure(QStringLiteral("unreachable"),
                       QStringLiteral("connect: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
#if defined(Q_OS_LINUX)
    ucred cred;
    socklen_t len = sizeof cred;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0 || cred.uid != ::getuid())
        return failure(QStringLiteral("unreachable"), QStringLiteral("peer owned by another user"));
    if (qint64(cred.pid) != session.pid)
        return failure(QStringLiteral("unreachable"),
                       QStringLiteral("peer is pid %1, not %2").arg(cred.pid).arg(session.pid));
#else
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0 || uid != ::getuid())
        return failure(QStringLiteral("unreachable"), QStringLiteral("peer owned by another user"));
#endif
    qint64 written = 0;
    while (written < frame.size()) {
        const ssize_t n = ::send(fd, frame.constData() + written, size_t(frame.size() - written),
#if defined(MSG_NOSIGNAL)
                                 MSG_NOSIGNAL
#else
                                 0
#endif
        );
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return failure(QStringLiteral("unreachable"),
                           QStringLiteral("write: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        }
        written += n;
    }
    ::shutdown(fd, SHUT_WR);
    QByteArray reply;
    pollfd pfd{fd, POLLIN, 0};
    while (reply.size() < 1024 && !reply.contains('\n') && ::poll(&pfd, 1, kReplyTimeoutMs) > 0) {
        char buf[256];
        const ssize_t n = ::read(fd, buf, sizeof buf);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        reply.append(buf, n);
    }
    if (!reply.contains('\n'))
        return failure(QStringLiteral("unreachable"), QStringLiteral("no answer"));
    return parseReply(reply.left(reply.indexOf('\n')));
#else
    Q_UNUSED(session);
    Q_UNUSED(frame);
    return failure(QStringLiteral("unreachable"), QStringLiteral("unsupported platform"));
#endif
}

TerminalHandoffReply type(const TerminalHandoffSession &session, const QString &from,
                          const QString &text)
{
    const QString clean = sanitizeText(text);
    if (clean.isEmpty() || clean.size() > kMaxTextChars)
        return failure(QStringLiteral("empty"), QStringLiteral("nothing to type"));
    return exchange(session,
                    requestFrame(QStringLiteral("type"), sanitizeLabel(from), clean.toUtf8()));
}

TerminalHandoffReply key(const TerminalHandoffSession &session, const QString &from,
                         const QString &key)
{
    if (key != QLatin1String("esc") && key != QLatin1String("ctrl-c") &&
        key != QLatin1String("interrupt"))
        return failure(QStringLiteral("bad_key"), key);
    return exchange(session, requestFrame(QStringLiteral("key"), sanitizeLabel(from), key.toLatin1()));
}

QString errorCode(const TerminalHandoffReply &reply)
{
    if (reply.ok)
        return {};
    if (reply.code == QLatin1String("remote_off"))
        return QStringLiteral("terminal_remote_off");
    if (reply.code == QLatin1String("user_typing"))
        return QStringLiteral("terminal_user_typing");
    if (reply.code == QLatin1String("rate_limited"))
        return QStringLiteral("typing_rate_limited");
    if (reply.code == QLatin1String("too_long") || reply.code == QLatin1String("empty"))
        return QStringLiteral("bad_task");
    return QStringLiteral("not_typeable");
}

QString launchCommand(const QString &cli)
{
    return QStringLiteral("forkmesh term ") + cli;
}

QString aliasLine(const QString &cli)
{
    return QStringLiteral("alias %1='forkmesh term %1'").arg(cli);
}

QString TypingGate::check(const QString &deviceKey, int chars, qint64 nowMs)
{
    if (chars > kMaxTextChars)
        return QStringLiteral("bad_task");
    std::deque<qint64> &times = m_times[deviceKey];
    while (!times.empty() && nowMs - times.front() >= kRateWindowMs)
        times.pop_front();
    if (!times.empty() && nowMs - times.back() < kMinGapMs)
        return QStringLiteral("typing_rate_limited");
    if (int(times.size()) >= kMaxTypedPerWindow)
        return QStringLiteral("typing_rate_limited");
    times.push_back(nowMs);
    return {};
}

QString admit(TypingGate &gate, bool paired, const QString &deviceKey, int chars, qint64 nowMs)
{
    if (!paired || deviceKey.isEmpty())
        return QStringLiteral("not_paired");
    return gate.check(deviceKey, chars, nowMs);
}

} // namespace TerminalHandoff
