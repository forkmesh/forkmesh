#include "MachineAgentScan.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPair>
#include <QRegularExpression>

#include <algorithm>

#if defined(Q_OS_LINUX)
#include <pwd.h>
#include <unistd.h>
#endif

namespace {

constexpr qint64 kMaxSessionRecordBytes = 64 * 1024;

const QStringList &knownKinds()
{
    static const QStringList kinds = {
        QStringLiteral("claude"),   QStringLiteral("codex"),
        QStringLiteral("gemini"),   QStringLiteral("opencode"),
        QStringLiteral("aider"),    QStringLiteral("goose"),
        QStringLiteral("amp"),
        QStringLiteral("crush"),    QStringLiteral("cursor-agent"),
        QStringLiteral("copilot"),
    };
    return kinds;
}

bool isInterpreter(const QString &comm)
{
    return comm == QLatin1String("node") || comm == QLatin1String("bun")
           || comm == QLatin1String("deno") || comm == QLatin1String("MainThread")
           || comm.startsWith(QLatin1String("python"));
}

QString scriptKind(const QByteArray &arg)
{
    QString name = QFileInfo(QString::fromLocal8Bit(arg)).fileName();
    for (const char *ext : {".js", ".mjs", ".cjs", ".py"}) {
        if (name.endsWith(QLatin1String(ext))) {
            name.chop(int(qstrlen(ext)));
            break;
        }
    }
    return knownKinds().contains(name) ? name : QString();
}

QByteArray readProc(qint64 pid, const char *leaf)
{
    QFile file(QStringLiteral("/proc/%1/%2").arg(pid).arg(QLatin1String(leaf)));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.readAll();
}

QByteArrayList splitNul(const QByteArray &data)
{
    QByteArrayList parts = data.split('\0');
    while (!parts.isEmpty() && parts.last().isEmpty())
        parts.removeLast();
    return parts;
}

QString environValue(const QByteArrayList &environment, const QByteArray &name)
{
    const QByteArray prefix = name + '=';
    for (const QByteArray &entry : environment) {
        if (entry.startsWith(prefix))
            return QString::fromLocal8Bit(entry.mid(prefix.size()));
    }
    return {};
}

} // namespace

namespace MachineAgentScan {

QString agentKind(const QString &comm, const QByteArrayList &argv)
{
    if (knownKinds().contains(comm))
        return comm;
    if (!argv.isEmpty()) {
        const QString first = scriptKind(argv.first());
        if (!first.isEmpty())
            return first;
    }
    if (!isInterpreter(comm))
        return {};
    for (int i = 1; i < argv.size(); ++i) {
        const QByteArray &arg = argv.at(i);
        if (arg.startsWith('-'))
            continue;
        return scriptKind(arg);
    }
    return {};
}

bool scriptStub(const QString &exePath)
{
    // A shell script named `claude` (the fakes Go tests put on PATH) gets the
    // script's name as its comm, but its executable is the shell. Real agents
    // are a native binary or an interpreter we match by script argument.
    static const QStringList shells = {
        QStringLiteral("sh"),   QStringLiteral("bash"), QStringLiteral("dash"),
        QStringLiteral("zsh"),  QStringLiteral("ash"),  QStringLiteral("ksh"),
        QStringLiteral("fish"), QStringLiteral("busybox"),
    };
    return shells.contains(QFileInfo(exePath).fileName());
}

bool cwdRemoved(const QString &linkTarget)
{
    return linkTarget.endsWith(QLatin1String(" (deleted)")) && !QFileInfo::exists(linkTarget);
}

bool within(const QString &path, const QString &folder)
{
    if (path.isEmpty() || folder.trimmed().isEmpty())
        return false;
    const QString root = QDir::cleanPath(QDir(folder).absolutePath());
    const QString target = QDir::cleanPath(QDir(path).absolutePath());
    return target == root || target.startsWith(root == QLatin1String("/") ? root : root + QLatin1Char('/'));
}

QString argValue(const QByteArrayList &argv, const QList<QByteArray> &names)
{
    for (int i = 1; i < argv.size(); ++i) {
        const QByteArray &arg = argv.at(i);
        for (const QByteArray &name : names) {
            if (arg == name && i + 1 < argv.size() && !argv.at(i + 1).startsWith('-'))
                return QString::fromLocal8Bit(argv.at(i + 1));
            if (arg.startsWith(name + '='))
                return QString::fromLocal8Bit(arg.mid(name.size() + 1));
        }
    }
    return {};
}

QString claudeProjectKey(const QString &cwd)
{
    QString key = cwd;
    for (QChar &c : key) {
        const ushort u = c.unicode();
        const bool keep = (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z')
                          || (u >= '0' && u <= '9');
        if (!keep)
            c = QLatin1Char('-');
    }
    return key;
}

QString recordedClaudeSession(const QString &configDir, qint64 pid)
{
    if (configDir.isEmpty() || pid <= 0)
        return {};
    QFile file(configDir + QStringLiteral("/sessions/%1.json").arg(pid));
    if (file.size() > kMaxSessionRecordBytes || !file.open(QIODevice::ReadOnly))
        return {};
    const QJsonObject record = QJsonDocument::fromJson(file.readAll()).object();
    if (qint64(record.value(QStringLiteral("pid")).toDouble()) != pid)
        return {};
    const QString sessionId = record.value(QStringLiteral("sessionId")).toString();
    static const QRegularExpression safe(QStringLiteral("^[A-Za-z0-9-]+$"));
    return safe.match(sessionId).hasMatch() ? sessionId : QString();
}

QString claudeTranscript(const QString &configDir, const QString &cwd,
                         const QString &sessionId, const QSet<QString> &claimed)
{
    if (configDir.isEmpty() || cwd.isEmpty())
        return {};
    const QDir dir(configDir + QStringLiteral("/projects/") + claudeProjectKey(cwd));
    if (!sessionId.isEmpty()) {
        const QString exact = dir.filePath(sessionId + QStringLiteral(".jsonl"));
        if (QFileInfo(exact).isFile())
            return exact;
    }
    const QFileInfoList files = dir.entryInfoList(QStringList{QStringLiteral("*.jsonl")},
                                                  QDir::Files, QDir::Time);
    for (const QFileInfo &file : files) {
        const QString path = file.absoluteFilePath();
        if (!claimed.contains(path))
            return path;
    }
    return {};
}

bool codexRollout(const QString &path)
{
    const QString name = QFileInfo(path).fileName();
    return path.contains(QLatin1String("/sessions/")) && name.startsWith(QLatin1String("rollout-"))
           && name.endsWith(QLatin1String(".jsonl"));
}

QString openCodexRollout(const SystemStats::ProcessTable &table, qint64 pid)
{
    QList<qint64> pending{pid};
    QSet<qint64> seen{pid};
    while (!pending.isEmpty()) {
        const qint64 current = pending.takeFirst();
        const QDir fds(QStringLiteral("/proc/%1/fd").arg(current));
        for (const QString &fd : fds.entryList(QDir::NoDotAndDotDot | QDir::AllEntries | QDir::System)) {
            const QString target = QFileInfo(fds.filePath(fd)).symLinkTarget();
            if (codexRollout(target))
                return target;
        }
        for (auto it = table.entries.constBegin(); it != table.entries.constEnd(); ++it) {
            if (it->parentPid == current && !seen.contains(it.key())) {
                seen.insert(it.key());
                pending.append(it.key());
            }
        }
    }
    return {};
}

qint64 processUid(qint64 pid)
{
    for (const QByteArray &line : readProc(pid, "status").split('\n')) {
        if (!line.startsWith("Uid:"))
            continue;
        bool ok = false;
        const qint64 uid = line.mid(4).simplified().split(' ').value(0).toLongLong(&ok);
        return ok ? uid : -1;
    }
    return -1;
}

QString userName(qint64 uid)
{
#if defined(Q_OS_LINUX)
    struct passwd entry;
    struct passwd *found = nullptr;
    char buffer[4096];
    if (::getpwuid_r(uid_t(uid), &entry, buffer, sizeof buffer, &found) == 0 && found &&
        found->pw_name && *found->pw_name)
        return QString::fromLocal8Bit(found->pw_name);
#endif
    return QStringLiteral("user %1").arg(uid);
}

QList<qint64> outsideRoots(const SystemStats::ProcessTable &table,
                           const QHash<qint64, QString> &kinds,
                           const QSet<qint64> &ownRoots,
                           const QSet<qint64> &foreignRoots)
{
    QList<qint64> roots;
    for (auto it = kinds.constBegin(); it != kinds.constEnd(); ++it) {
        const qint64 pid = it.key();
        if (ownRoots.contains(pid))
            continue;
        bool nested = false;
        QSet<qint64> seen{pid};
        qint64 parent = foreignRoots.contains(pid) ? 0 : table.entries.value(pid).parentPid;
        while (parent > 1 && !seen.contains(parent)) {
            if (foreignRoots.contains(parent))
                break;
            if (ownRoots.contains(parent) || kinds.contains(parent)) {
                nested = true;
                break;
            }
            seen.insert(parent);
            const auto entry = table.entries.constFind(parent);
            if (entry == table.entries.constEnd())
                break;
            parent = entry->parentPid;
        }
        if (!nested)
            roots.append(pid);
    }
    std::sort(roots.begin(), roots.end());
    return roots;
}

QList<MachineAgent> scan(const SystemStats::ProcessTable &table,
                         const QSet<qint64> &ownRoots,
                         const QSet<QString> &ownCwds,
                         const QSet<qint64> &foreignRoots)
{
    QList<MachineAgent> out;
#if defined(Q_OS_LINUX)
    QHash<qint64, QString> kinds;
    QHash<qint64, QByteArrayList> argvs;
    QHash<qint64, qint64> uids;
    for (auto it = table.entries.constBegin(); it != table.entries.constEnd(); ++it) {
        if (it->zombie)
            continue;
        const bool maybe = knownKinds().contains(it->command) || isInterpreter(it->command);
        if (!maybe)
            continue;
        const qint64 uid = processUid(it.key());
        if (uid < 0)
            continue;
        const QByteArrayList argv = splitNul(readProc(it.key(), "cmdline"));
        const QString kind = agentKind(it->command, argv);
        if (kind.isEmpty())
            continue;
        if (it->command == kind
            && scriptStub(QFileInfo(QStringLiteral("/proc/%1/exe").arg(it.key())).symLinkTarget()))
            continue;
        kinds.insert(it.key(), kind);
        argvs.insert(it.key(), argv);
        uids.insert(it.key(), uid);
    }
    QSet<QString> claimed;
    QList<QPair<MachineAgent, QString>> guessed;
    for (const qint64 pid : outsideRoots(table, kinds, ownRoots, foreignRoots)) {
        MachineAgent agent;
        agent.pid = pid;
        agent.kind = kinds.value(pid);
        const QByteArrayList argv = argvs.value(pid);
        agent.model = argValue(argv, {QByteArrayLiteral("--model"), QByteArrayLiteral("-m")});
        if (const qint64 uid = uids.value(pid); uid != qint64(::getuid())) {
            agent.otherUser = userName(uid);
            out.append(agent);
            continue;
        }
        agent.cwd = QFileInfo(QStringLiteral("/proc/%1/cwd").arg(pid)).symLinkTarget();
        if (cwdRemoved(agent.cwd))
            continue;
        if (agent.kind == QLatin1String("claude") && !agent.cwd.isEmpty()) {
            const QByteArrayList processEnvironment = splitNul(readProc(pid, "environ"));
            QString configDir = environValue(processEnvironment, QByteArrayLiteral("CLAUDE_CONFIG_DIR"));
            if (configDir.isEmpty()) {
                QString home = environValue(processEnvironment, QByteArrayLiteral("HOME"));
                if (home.isEmpty())
                    home = QDir::homePath();
                configDir = home + QStringLiteral("/.claude");
            }
            agent.configDir = configDir;
            const QString recorded = recordedClaudeSession(configDir, pid);
            const QString sessionId =
                !recorded.isEmpty()
                    ? recorded
                    : argValue(argv, {QByteArrayLiteral("--resume"), QByteArrayLiteral("-r"),
                                      QByteArrayLiteral("--session-id")});
            if (!recorded.isEmpty()) {
                const QString exact = QDir(configDir + QStringLiteral("/projects/")
                                           + claudeProjectKey(agent.cwd))
                                          .filePath(recorded + QStringLiteral(".jsonl"));
                if (QFileInfo(exact).isFile()) {
                    agent.transcript = exact;
                    claimed.insert(exact);
                }
            } else if (!sessionId.isEmpty()) {
                agent.transcript = claudeTranscript(configDir, agent.cwd, sessionId, claimed);
                if (!agent.transcript.isEmpty())
                    claimed.insert(agent.transcript);
            } else if (!ownCwds.contains(QDir(agent.cwd).absolutePath())) {
                guessed.append({agent, configDir});
                continue;
            }
        } else if (agent.kind == QLatin1String("codex")) {
            agent.transcript = openCodexRollout(table, pid);
        }
        out.append(agent);
    }
    for (auto &[agent, configDir] : guessed) {
        agent.transcript = claudeTranscript(configDir, agent.cwd, QString(), claimed);
        if (!agent.transcript.isEmpty())
            claimed.insert(agent.transcript);
        out.append(agent);
    }
    std::sort(out.begin(), out.end(),
              [](const MachineAgent &a, const MachineAgent &b) { return a.pid < b.pid; });
#else
    Q_UNUSED(table);
    Q_UNUSED(ownRoots);
    Q_UNUSED(ownCwds);
    Q_UNUSED(foreignRoots);
#endif
    return out;
}

QString label(const QString &kind)
{
    static const QHash<QString, QString> labels = {
        {QStringLiteral("claude"), QStringLiteral("Claude Code")},
        {QStringLiteral("codex"), QStringLiteral("Codex")},
        {QStringLiteral("gemini"), QStringLiteral("Gemini CLI")},
        {QStringLiteral("opencode"), QStringLiteral("opencode")},
        {QStringLiteral("aider"), QStringLiteral("Aider")},
        {QStringLiteral("goose"), QStringLiteral("Goose")},
        {QStringLiteral("amp"), QStringLiteral("Amp")},
        {QStringLiteral("crush"), QStringLiteral("Crush")},
        {QStringLiteral("cursor-agent"), QStringLiteral("Cursor Agent")},
        {QStringLiteral("copilot"), QStringLiteral("Copilot CLI")},
    };
    return labels.value(kind, kind);
}

QString provider(const QString &kind)
{
    if (kind == QLatin1String("claude"))
        return QStringLiteral("claude-code");
    return kind;
}

} // namespace MachineAgentScan
