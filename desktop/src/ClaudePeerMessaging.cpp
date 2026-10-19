#include "ClaudePeerMessaging.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>

#if defined(Q_OS_UNIX)
#include <cerrno>
#include <csignal>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

const QString kEnvelopeTag = QStringLiteral("cross-session-message");
constexpr qint64 kMaxRecordBytes = 64 * 1024;
constexpr qint64 kMaxKeyBytes = 4096;
constexpr qint64 kModeTailBytes = 512 * 1024;
constexpr int kConnectTimeoutMs = 3000;
constexpr int kDrainTimeoutMs = 2000;

QByteArray readSmall(const QString &path, qint64 limit)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > limit)
        return {};
    return file.readAll();
}

bool envelopeWordChar(QChar c)
{
    const ushort u = c.unicode();
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') || (u >= '0' && u <= '9') ||
           u == '_' || u == '-';
}

QString attributeValue(const QString &value)
{
    QString out;
    for (const QChar c : value) {
        if (c == QLatin1Char('"') || c == QLatin1Char('<') || c == QLatin1Char('>'))
            continue;
        const QChar::Category cat = c.category();
        if (cat == QChar::Other_Control || cat == QChar::Other_Format ||
            cat == QChar::Other_Surrogate || cat == QChar::Separator_Line ||
            cat == QChar::Separator_Paragraph)
            continue;
        out.append(c);
    }
    out = out.trimmed();
    return out.size() > 64 ? out.left(64) : out;
}

bool processAlive(qint64 pid)
{
    if (pid <= 0)
        return false;
#if defined(Q_OS_UNIX)
    if (::kill(pid_t(pid), 0) == 0)
        return true;
    return errno == EPERM;
#else
    return false;
#endif
}

} // namespace

namespace ClaudePeerMessaging {

QString forkMeshSenderName()
{
    return QStringLiteral("ForkMesh (phone)");
}

QString steerLead()
{
    return QStringLiteral("Your user typed this in the ForkMesh app on their phone, on this session's own page, as their next message to you in this conversation. ForkMesh can only reach a terminal session through cross-session messaging, so it arrives that way, but it was not sent by another session.");
}

QString steerBody(const QString &text)
{
    return steerLead() + QStringLiteral("\n\n") + text;
}

QString steerText(const QString &body)
{
    const QString lead = steerLead();
    if (!body.startsWith(lead))
        return body;
    return body.mid(lead.size()).trimmed();
}

QStringList configDirs(const QString &home, const QString &accountsRoot, const QStringList &extra)
{
    QStringList dirs;
    QSet<QString> seen;
    const auto add = [&](const QString &dir) {
        if (dir.isEmpty())
            return;
        const QString clean = QDir::cleanPath(dir);
        if (seen.contains(clean))
            return;
        seen.insert(clean);
        dirs.append(clean);
    };
    if (!home.isEmpty())
        add(home + QStringLiteral("/.claude"));
    if (!accountsRoot.isEmpty()) {
        const QDir accounts(accountsRoot + QStringLiteral("/claude"));
        for (const QString &name : accounts.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name))
            add(accounts.filePath(name));
    }
    for (const QString &dir : extra)
        add(dir);
    return dirs;
}

QString procStart(qint64 pid)
{
    const QByteArray stat = readSmall(QStringLiteral("/proc/%1/stat").arg(pid), 16 * 1024);
    const int close = stat.lastIndexOf(')');
    if (close < 0)
        return {};
    const QList<QByteArray> fields = stat.mid(close + 2).split(' ');
    return fields.size() > 19 ? QString::fromLatin1(fields.at(19)) : QString();
}

bool isSocket(const QString &path)
{
#if defined(Q_OS_UNIX)
    struct stat st;
    if (::lstat(QFile::encodeName(path).constData(), &st) != 0)
        return false;
    return S_ISSOCK(st.st_mode);
#else
    Q_UNUSED(path);
    return false;
#endif
}

QList<ClaudePeerSession> readSessions(const QString &configDir)
{
    static const QRegularExpression recordName(QStringLiteral("^(\\d+)\\.json$"));
    QList<ClaudePeerSession> out;
    const QDir dir(configDir + QStringLiteral("/sessions"));
    for (const QString &name : dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        const QRegularExpressionMatch match = recordName.match(name);
        if (!match.hasMatch())
            continue;
        const QJsonObject record =
            QJsonDocument::fromJson(readSmall(dir.filePath(name), kMaxRecordBytes)).object();
        const qint64 pid = qint64(record.value(QStringLiteral("pid")).toDouble());
        if (pid <= 0 || pid != match.captured(1).toLongLong())
            continue;
        if (record.value(QStringLiteral("peerProtocol")).toInt() != kPeerProtocol)
            continue;
        const QString socketPath = record.value(QStringLiteral("messagingSocketPath")).toString();
        if (socketPath.isEmpty() || !QDir::isAbsolutePath(socketPath))
            continue;
        if (!processAlive(pid))
            continue;
        const QString recorded = record.value(QStringLiteral("procStart")).toString();
        const QString actual = procStart(pid);
        if (!recorded.isEmpty() && !actual.isEmpty() && recorded != actual)
            continue;
        if (!isSocket(socketPath))
            continue;
        ClaudePeerSession session;
        session.pid = pid;
        session.sessionId = record.value(QStringLiteral("sessionId")).toString();
        session.cwd = record.value(QStringLiteral("cwd")).toString();
        session.name = record.value(QStringLiteral("name")).toString();
        session.status = record.value(QStringLiteral("status")).toString();
        session.entrypoint = record.value(QStringLiteral("entrypoint")).toString();
        session.socketPath = socketPath;
        session.configDir = QDir::cleanPath(configDir);
        out.append(session);
    }
    return out;
}

QList<ClaudePeerSession> liveSessions(const QStringList &configDirs)
{
    QList<ClaudePeerSession> out;
    QSet<qint64> seen;
    for (const QString &dir : configDirs) {
        for (const ClaudePeerSession &session : readSessions(dir)) {
            if (seen.contains(session.pid))
                continue;
            seen.insert(session.pid);
            out.append(session);
        }
    }
    return out;
}

const ClaudePeerSession *find(const QList<ClaudePeerSession> &sessions,
                              const QList<qint64> &pids, const QString &sessionId)
{
    for (const ClaudePeerSession &session : sessions) {
        if (pids.contains(session.pid))
            return &session;
    }
    if (sessionId.isEmpty())
        return nullptr;
    const ClaudePeerSession *found = nullptr;
    for (const ClaudePeerSession &session : sessions) {
        if (session.sessionId != sessionId)
            continue;
        if (found)
            return nullptr;
        found = &session;
    }
    return found;
}

QString keyPath(const QString &configDir, qint64 pid, const QString &socketPath)
{
    const QByteArray hash =
        QCryptographicHash::hash(socketPath.toUtf8(), QCryptographicHash::Sha256).toHex();
    return configDir + QStringLiteral("/sessions/%1.%2.key").arg(pid).arg(QString::fromLatin1(hash));
}

QString peerToken(const QString &keyPath)
{
    static const QRegularExpression tokenShape(QStringLiteral("^[0-9a-f]{32}$"));
    const QJsonObject key = QJsonDocument::fromJson(readSmall(keyPath, kMaxKeyBytes)).object();
    const QString token = key.value(QStringLiteral("peerToken")).toString();
    return tokenShape.match(token).hasMatch() ? token : QString();
}

QString permissionClass(const QByteArrayList &argv, const QString &transcriptPath)
{
    static const QRegularExpression modeField(
        QStringLiteral("\"permissionMode\"\\s*:\\s*\"([A-Za-z]+)\""));
    if (!transcriptPath.isEmpty()) {
        QFile file(transcriptPath);
        if (file.open(QIODevice::ReadOnly)) {
            const qint64 size = file.size();
            if (size > kModeTailBytes)
                file.seek(size - kModeTailBytes);
            const QString tail = QString::fromUtf8(file.readAll());
            QString last;
            QRegularExpressionMatchIterator it = modeField.globalMatch(tail);
            while (it.hasNext())
                last = it.next().captured(1);
            if (!last.isEmpty())
                return last == QLatin1String("bypassPermissions") ? QStringLiteral("bypass")
                                                                  : QStringLiteral("prompting");
        }
    }
    for (int i = 1; i < argv.size(); ++i) {
        const QByteArray &arg = argv.at(i);
        if (arg == "--dangerously-skip-permissions" ||
            arg == "--permission-mode=bypassPermissions")
            return QStringLiteral("bypass");
        if (arg == "--permission-mode" && i + 1 < argv.size() &&
            argv.at(i + 1) == "bypassPermissions")
            return QStringLiteral("bypass");
    }
    return QStringLiteral("prompting");
}

QString escapeBody(const QString &text)
{
    QString out;
    out.reserve(text.size());
    for (int i = 0; i < text.size(); ++i) {
        const QChar c = text.at(i);
        out.append(c);
        if (c != QLatin1Char('<'))
            continue;
        if (i + 1 < text.size() && text.at(i + 1) == QLatin1Char('\\'))
            continue;
        for (int j = i + 1; j < text.size(); ++j) {
            const QChar next = text.at(j);
            if (envelopeWordChar(next) || next == QLatin1Char('<') || next == QLatin1Char('>'))
                break;
            if (next == QLatin1Char('/') || next.unicode() > 0x7f) {
                out.append(QLatin1Char('\\'));
                break;
            }
        }
    }
    return out;
}

QString envelope(const ClaudePeerMessage &message)
{
    static const QRegularExpression address(QStringLiteral("^[A-Za-z0-9%:_/.\\\\-]{1,300}$"));
    QString attributes;
    if (address.match(message.from).hasMatch())
        attributes += QStringLiteral(" from=\"%1\"").arg(message.from);
    const QString name = attributeValue(message.fromName);
    if (!name.isEmpty())
        attributes += QStringLiteral(" from-name=\"%1\"").arg(name);
    if (message.fromMode == QLatin1String("bypass") || message.fromMode == QLatin1String("prompting"))
        attributes += QStringLiteral(" from-mode=\"%1\"").arg(message.fromMode);
    return QStringLiteral("<%1%2>\n%3\n</%1>").arg(kEnvelopeTag, attributes, escapeBody(message.text));
}

QByteArray jsonString(const QString &value)
{
    static const char hex[] = "0123456789abcdef";
    QByteArray out("\"");
    const auto escaped = [&](ushort u) {
        out += "\\u";
        out += hex[(u >> 12) & 0xf];
        out += hex[(u >> 8) & 0xf];
        out += hex[(u >> 4) & 0xf];
        out += hex[u & 0xf];
    };
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        const ushort u = c.unicode();
        switch (u) {
        case '"': out += "\\\""; continue;
        case '\\': out += "\\\\"; continue;
        case '\b': out += "\\b"; continue;
        case '\f': out += "\\f"; continue;
        case '\n': out += "\\n"; continue;
        case '\r': out += "\\r"; continue;
        case '\t': out += "\\t"; continue;
        default: break;
        }
        if (u < 0x20) {
            escaped(u);
            continue;
        }
        if (c.isHighSurrogate()) {
            if (i + 1 < value.size() && value.at(i + 1).isLowSurrogate()) {
                out += QString(value.constData() + i, 2).toUtf8();
                ++i;
            } else {
                escaped(u);
            }
            continue;
        }
        if (c.isLowSurrogate()) {
            escaped(u);
            continue;
        }
        out += QString(c).toUtf8();
    }
    out += '"';
    return out;
}

QByteArray authLine(const QString &token)
{
    return QByteArrayLiteral("{\"type\":\"auth\",\"token\":") + jsonString(token) + '}';
}

QByteArray userLine(const ClaudePeerMessage &message)
{
    QByteArray line = QByteArrayLiteral("{\"msgV\":1,\"msg_id\":") + jsonString(message.msgId) +
                      QByteArrayLiteral(",\"type\":\"user\",\"message\":{\"role\":\"user\",\"content\":") +
                      jsonString(envelope(message)) + QByteArrayLiteral("},\"priority\":") +
                      jsonString(message.priority);
    if (!message.from.isEmpty())
        line += QByteArrayLiteral(",\"from\":") + jsonString(message.from);
    line += '}';
    return line;
}

QByteArray frame(const QString &token, const ClaudePeerMessage &message)
{
    QByteArray data;
    if (!token.isEmpty())
        data += authLine(token) + '\n';
    data += userLine(message) + '\n';
    return data;
}

bool send(const QString &socketPath, qint64 expectPid, const QByteArray &data, QString *error)
{
#if defined(Q_OS_UNIX)
    const auto fail = [error](const QString &why) {
        if (error)
            *error = why;
        return false;
    };
    const QByteArray path = QFile::encodeName(socketPath);
    sockaddr_un addr;
    std::memset(&addr, 0, sizeof addr);
    addr.sun_family = AF_UNIX;
    if (path.isEmpty() || size_t(path.size()) >= sizeof addr.sun_path)
        return fail(QStringLiteral("socket path too long"));
    std::memcpy(addr.sun_path, path.constData(), size_t(path.size()));
    if (!isSocket(socketPath))
        return fail(QStringLiteral("no socket"));
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return fail(QStringLiteral("socket: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
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
        return fail(QStringLiteral("connect: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
#if defined(Q_OS_LINUX)
    ucred cred;
    socklen_t len = sizeof cred;
    if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0)
        return fail(QStringLiteral("peer unverifiable"));
    if (cred.uid != ::getuid())
        return fail(QStringLiteral("peer owned by another user"));
    if (expectPid > 0 && qint64(cred.pid) != expectPid)
        return fail(QStringLiteral("peer is pid %1, not %2").arg(cred.pid).arg(expectPid));
#else
    uid_t uid = 0;
    gid_t gid = 0;
    if (::getpeereid(fd, &uid, &gid) != 0 || uid != ::getuid())
        return fail(QStringLiteral("peer owned by another user"));
    Q_UNUSED(expectPid);
#endif
    qint64 written = 0;
    while (written < data.size()) {
        const ssize_t n = ::send(fd, data.constData() + written, size_t(data.size() - written),
#if defined(MSG_NOSIGNAL)
                                 MSG_NOSIGNAL
#else
                                 0
#endif
        );
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return fail(QStringLiteral("write: %1").arg(QString::fromLocal8Bit(std::strerror(errno))));
        }
        written += n;
    }
    ::shutdown(fd, SHUT_WR);
    pollfd pfd{fd, POLLIN, 0};
    char sink[256];
    while (::poll(&pfd, 1, kDrainTimeoutMs) > 0) {
        const ssize_t n = ::read(fd, sink, sizeof sink);
        if (n <= 0)
            break;
    }
    return true;
#else
    Q_UNUSED(socketPath);
    Q_UNUSED(expectPid);
    Q_UNUSED(data);
    if (error)
        *error = QStringLiteral("unsupported platform");
    return false;
#endif
}

QString inboundSetting(const QByteArray &settingsJson)
{
    const QJsonObject settings = QJsonDocument::fromJson(settingsJson).object();
    if (!settings.contains(QStringLiteral("crossSessionInbound")))
        return {};
    const QString value = settings.value(QStringLiteral("crossSessionInbound")).toString();
    if (value == QLatin1String("accept") || value == QLatin1String("hold") ||
        value == QLatin1String("refuse"))
        return value;
    return QStringLiteral("invalid");
}

QString inboundSettingFile(const QString &path)
{
    if (path.isEmpty())
        return {};
    return inboundSetting(readSmall(path, 4 * 1024 * 1024));
}

QString flagInboundSetting(const QByteArrayList &argv)
{
    QByteArray value;
    for (int i = 1; i < argv.size(); ++i) {
        const QByteArray &arg = argv.at(i);
        if (arg == "--settings" && i + 1 < argv.size())
            value = argv.at(i + 1);
        else if (arg.startsWith("--settings="))
            value = arg.mid(11);
    }
    const QByteArray trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return {};
    if (trimmed.startsWith('{'))
        return inboundSetting(trimmed);
    return inboundSettingFile(QFile::decodeName(trimmed));
}

QString managedSettingsPath()
{
#if defined(Q_OS_MACOS)
    return QStringLiteral("/Library/Application Support/ClaudeCode/managed-settings.json");
#else
    return QStringLiteral("/etc/claude-code/managed-settings.json");
#endif
}

QString gitRoot(const QString &cwd)
{
    QDir dir(cwd);
    if (cwd.isEmpty() || !dir.exists())
        return {};
    for (;;) {
        if (QFileInfo::exists(dir.filePath(QStringLiteral(".git"))))
            return dir.absolutePath();
        if (!dir.cdUp())
            return {};
    }
}

ClaudeInboundPolicy inboundPolicy(const QString &managed, const QString &flag, const QString &user,
                                  const QString &local, const QString &project,
                                  const QString &receiverClass)
{
    const auto rank = [](const QString &value) {
        if (value == QLatin1String("refuse"))
            return 2;
        if (value == QLatin1String("hold"))
            return 1;
        return 0;
    };
    const auto valid = [](const QString &value) {
        return value == QLatin1String("accept") || value == QLatin1String("hold") ||
               value == QLatin1String("refuse");
    };
    QString value;
    QString decidedBy;
    if (managed == QLatin1String("invalid")) {
        value = QStringLiteral("refuse");
        decidedBy = QStringLiteral("managed-setting");
    } else if (valid(managed)) {
        value = managed;
        decidedBy = QStringLiteral("managed-setting");
    } else if (valid(flag)) {
        value = flag;
        decidedBy = QStringLiteral("explicit-setting");
    } else if (valid(user)) {
        value = user;
        decidedBy = QStringLiteral("explicit-setting");
    }
    for (const QString &repo : {local, project}) {
        if (!valid(repo))
            continue;
        if (rank(repo) > rank(value.isEmpty() ? QStringLiteral("accept") : value)) {
            value = repo;
            decidedBy = QStringLiteral("repo-setting");
        } else if (repo != QLatin1String("accept") && !value.isEmpty() &&
                   rank(repo) == rank(value) && decidedBy != QLatin1String("managed-setting")) {
            decidedBy = QStringLiteral("repo-setting");
        }
    }
    const bool invalid = flag == QLatin1String("invalid") || user == QLatin1String("invalid") ||
                         local == QLatin1String("invalid") || project == QLatin1String("invalid");
    if (invalid && rank(value.isEmpty() ? QStringLiteral("accept") : value) < 1) {
        value = QStringLiteral("hold");
        decidedBy = QStringLiteral("invalid-setting");
    }
    if (!value.isEmpty())
        return {value, decidedBy};
    if (receiverClass == QLatin1String("bypass"))
        return {QStringLiteral("hold"), QStringLiteral("no-mode-asserted")};
    return {QStringLiteral("accept"), QStringLiteral("mode-parity")};
}

ClaudeInboundPolicy predictInbound(const ClaudePeerSession &session, const QByteArrayList &argv,
                                   const QString &receiverClass, const QString &managedPath)
{
    QString local;
    QString project;
    if (!session.cwd.isEmpty()) {
        local = inboundSettingFile(session.cwd + QStringLiteral("/.claude/settings.local.json"));
        project = inboundSettingFile(session.cwd + QStringLiteral("/.claude/settings.json"));
        const QString root = gitRoot(session.cwd);
        if (!root.isEmpty() && QDir::cleanPath(root) != QDir::cleanPath(session.cwd)) {
            const QString rootLocal =
                inboundSettingFile(root + QStringLiteral("/.claude/settings.local.json"));
            if (!rootLocal.isEmpty() && rootLocal != QLatin1String("accept") &&
                (local.isEmpty() || local == QLatin1String("accept") ||
                 rootLocal == QLatin1String("refuse") || rootLocal == QLatin1String("invalid")))
                local = rootLocal;
        }
    }
    return inboundPolicy(inboundSettingFile(managedPath), flagInboundSetting(argv),
                         inboundSettingFile(session.configDir + QStringLiteral("/settings.json")),
                         local, project, receiverClass);
}

bool deliver(const ClaudePeerSession &session, const QString &text, QString *error)
{
    ClaudePeerMessage message;
    message.text = steerBody(text);
    message.fromName = forkMeshSenderName();
    message.msgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString token = peerToken(keyPath(session.configDir, session.pid, session.socketPath));
    return send(session.socketPath, session.pid, frame(token, message), error);
}

} // namespace ClaudePeerMessaging
