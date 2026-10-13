#include "ClaudeSessionScan.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>

namespace {

void readMeta(ExternalClaudeSession &s)
{
    QFile f(s.path);
    if (!f.open(QIODevice::ReadOnly))
        return;

    const qint64 size = f.size();
    const qint64 tailN = qMin<qint64>(size, 64 * 1024);
    f.seek(size - tailN);
    const QByteArray tail = f.readAll();
    int nl = tail.lastIndexOf('\n');
    while (nl >= 0) {
        const int prev = tail.lastIndexOf('\n', nl - 1);
        const QByteArray line = tail.mid(prev + 1, nl - prev - 1);
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.contains(QStringLiteral("cwd"))) {
            s.cwd = o.value(QStringLiteral("cwd")).toString();
            s.gitBranch = o.value(QStringLiteral("gitBranch")).toString();
            if (s.gitBranch == QLatin1String("HEAD"))
                s.gitBranch.clear();
            break;
        }
        nl = prev;
    }

    f.seek(0);
    const QByteArray head = f.read(96 * 1024);
    const QList<QByteArray> lines = head.split('\n');
    bool launchCwdSeen = false;
    for (const QByteArray &hl : lines) {
        const QJsonObject o = QJsonDocument::fromJson(hl).object();
        if (o.value(QStringLiteral("type")).toString() == QLatin1String("ai-title"))
            s.title = o.value(QStringLiteral("aiTitle")).toString();
        if (!launchCwdSeen && o.contains(QStringLiteral("cwd"))) {
            launchCwdSeen = true;
            const QString launchCwd = o.value(QStringLiteral("cwd")).toString();
            if (!launchCwd.isEmpty())
                s.cwd = launchCwd;
        }
    }
    if (s.title.isEmpty())
        s.title = QStringLiteral("Claude Code session");
}

bool isUnder(const QString &path, const QString &root)
{
    return path == root || path.startsWith(root + QLatin1Char('/'));
}

bool ownedByForkMesh(const QString &cwd, const QString &repo,
                     const QSet<QString> &ownCwds)
{
    for (const QString &own : ownCwds) {
        if (cwd == own)
            return true;
        if (own != repo && isUnder(cwd, own))
            return true;
    }
    return false;
}

} // namespace

namespace ClaudeSessionScan {

// code-health: allow(dir-walk-in-loop) only called from runOffThread in MainWindow::scanExternalClaudeSessions()
QList<ExternalClaudeSession> scan(const QString &repoLocalPath,
                                  const QSet<QString> &excludeCwds,
                                  qint64 activeWindowMs)
{
    QList<ExternalClaudeSession> out;
    if (repoLocalPath.isEmpty())
        return out;
    const QString repo = QDir(repoLocalPath).absolutePath();

    QDir projects(QDir::homePath() + QStringLiteral("/.claude/projects"));
    if (!projects.exists())
        return out;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const auto dirs = projects.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &di : dirs) {
        QDir d(di.absoluteFilePath());
        const auto files = d.entryInfoList(QStringList{QStringLiteral("*.jsonl")},
                                           QDir::Files, QDir::Time);
        for (const QFileInfo &fi : files) {
            if (now - fi.lastModified().toMSecsSinceEpoch() > activeWindowMs)
                break;
            ExternalClaudeSession s;
            s.uuid = fi.completeBaseName();
            s.path = fi.absoluteFilePath();
            s.lastActivityMs = fi.lastModified().toMSecsSinceEpoch();
            readMeta(s);
            if (s.cwd.isEmpty())
                continue;
            const QString cwd = QDir(s.cwd).absolutePath();
            if (!isUnder(cwd, repo))
                continue; // a different repo
            if (ownedByForkMesh(cwd, repo, excludeCwds))
                continue; // ForkMesh is driving this one itself
            if (!QFileInfo(cwd).isDir())
                continue;
            out.append(s);
        }
    }
    return out;
}

QList<QJsonObject> readEvents(const QString &path, qint64 fromOffset, qint64 *newOffset)
{
    QList<QJsonObject> out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (newOffset)
            *newOffset = fromOffset;
        return out;
    }
    if (fromOffset < 0 || fromOffset > f.size())
        fromOffset = 0; // truncated/rotated since last read — start over
    f.seek(fromOffset);
    const QByteArray buf = f.readAll();

    qint64 consumed = 0;
    int start = 0;
    while (true) {
        const int nl = buf.indexOf('\n', start);
        if (nl < 0)
            break; // leave the trailing partial line for next time
        const QByteArray line = buf.mid(start, nl - start);
        consumed = nl + 1;
        start = nl + 1;
        if (line.trimmed().isEmpty())
            continue;
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (!o.isEmpty())
            out.append(o);
    }
    if (newOffset)
        *newOffset = fromOffset + consumed;
    return out;
}

qint64 tailStartOffset(const QString &path, qint64 maxBytes)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return 0;
    const qint64 size = f.size();
    if (size <= maxBytes)
        return 0;
    qint64 off = size - maxBytes;
    f.seek(off);
    const QByteArray probe = f.read(qMin<qint64>(maxBytes, 1 << 20));
    const int nl = probe.indexOf('\n');
    if (nl >= 0)
        off += nl + 1; // start on a clean line boundary
    return off;
}

QList<qint64> findSessionPids(const QString &uuid, const QString &cwd)
{
    QList<qint64> claudePids;
#if defined(Q_OS_LINUX)
    const auto pids = QDir(QStringLiteral("/proc"))
                          .entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &name : pids) {
        bool isPid = false;
        const qint64 pid = name.toLongLong(&isPid);
        if (!isPid)
            continue;
        QFile comm(QStringLiteral("/proc/%1/comm").arg(name));
        if (!comm.open(QIODevice::ReadOnly))
            continue;
        if (comm.readAll().trimmed() == QByteArrayLiteral("claude"))
            claudePids.append(pid);
    }
#endif
    return findSessionPids(uuid, cwd, claudePids);
}

QList<qint64> findSessionPids(const QString &uuid, const QString &cwd,
                              const QList<qint64> &claudePids)
{
    QList<qint64> exact; // matched the session uuid via --resume (precise)
    QList<qint64> byCwd; // any `claude` process in the same working directory
#if defined(Q_OS_LINUX)
    const QString wantCwd = cwd.isEmpty() ? QString() : QDir(cwd).absolutePath();
    const QByteArray wantUuid = uuid.toUtf8();
    for (const qint64 pid : claudePids) {
        bool uuidMatch = false;
        if (!wantUuid.isEmpty()) {
            QFile cl(QStringLiteral("/proc/%1/cmdline").arg(pid));
            if (cl.open(QIODevice::ReadOnly))
                uuidMatch = cl.readAll().split('\0').contains(wantUuid);
        }
        if (uuidMatch) {
            exact.append(pid);
            continue;
        }
        // Fresh sessions don't expose their uuid, so fall back to the cwd — the
        // /proc/<pid>/cwd symlink resolves to the process's working directory.
        if (!wantCwd.isEmpty() &&
            QFileInfo(QStringLiteral("/proc/%1/cwd").arg(pid)).symLinkTarget() ==
                wantCwd)
            byCwd.append(pid);
    }
#else
    Q_UNUSED(uuid);
    Q_UNUSED(cwd);
    Q_UNUSED(claudePids);
#endif
    return exact.isEmpty() ? byCwd : exact;
}

} // namespace ClaudeSessionScan
