#include "ClaudeSessionScan.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMutex>
#include <QRegularExpression>

#include <algorithm>

namespace {

qint64 timestampMs(const QJsonObject &o)
{
    const QString ts = o.value(QStringLiteral("timestamp")).toString();
    if (ts.isEmpty())
        return 0;
    const QDateTime when = QDateTime::fromString(ts, Qt::ISODateWithMs);
    return when.isValid() ? when.toMSecsSinceEpoch() : 0;
}

QString typedPrompt(const QJsonObject &o)
{
    if (o.value(QStringLiteral("type")).toString() != QLatin1String("user") ||
        o.value(QStringLiteral("isMeta")).toBool() ||
        o.value(QStringLiteral("isSidechain")).toBool())
        return QString();
    const QString origin =
        o.value(QStringLiteral("origin")).toObject().value(QStringLiteral("kind")).toString();
    if (!origin.isEmpty() && origin != QLatin1String("human"))
        return QString();
    const QJsonValue content =
        o.value(QStringLiteral("message")).toObject().value(QStringLiteral("content"));
    QString text;
    if (content.isString()) {
        text = content.toString();
    } else {
        for (const QJsonValue &block : content.toArray()) {
            const QJsonObject b = block.toObject();
            if (b.value(QStringLiteral("type")).toString() == QLatin1String("text")) {
                text = b.value(QStringLiteral("text")).toString();
                break;
            }
        }
    }
    text = text.trimmed();
    if (text.startsWith(QLatin1Char('<')) || text.startsWith(QLatin1String("Caveat:")))
        return QString();
    return text;
}

struct MetaScan {
    QString customTitle;
    QString aiTitle;
    QString model;
};

void scanLine(const QJsonObject &o, ExternalClaudeSession &s, MetaScan &m)
{
    const QString type = o.value(QStringLiteral("type")).toString();
    if (type == QLatin1String("custom-title")) {
        const QString t = o.value(QStringLiteral("customTitle")).toString().trimmed();
        if (!t.isEmpty())
            m.customTitle = t;
    } else if (type == QLatin1String("ai-title")) {
        const QString t = o.value(QStringLiteral("aiTitle")).toString().trimmed();
        if (!t.isEmpty())
            m.aiTitle = t;
    } else if (type == QLatin1String("assistant")) {
        const QString model =
            o.value(QStringLiteral("message")).toObject().value(QStringLiteral("model")).toString();
        if (!model.isEmpty() && !model.startsWith(QLatin1Char('<')))
            m.model = model;
    }
    if (s.entrypoint.isEmpty())
        s.entrypoint = o.value(QStringLiteral("entrypoint")).toString();
    if (s.createdMs == 0)
        s.createdMs = timestampMs(o);
    if (s.firstPrompt.isEmpty())
        s.firstPrompt = typedPrompt(o);
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

void applyDesktopTitle(ExternalClaudeSession &s, const QHash<QString, QString> &titles)
{
    const QString t = titles.value(s.uuid);
    if (!t.isEmpty())
        s.title = t;
}

QHash<QString, QString> cachedDesktopTitles()
{
    static QMutex mutex;
    static QHash<QString, QString> titles;
    static qint64 readAtMs = 0;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QMutexLocker lock(&mutex);
    if (readAtMs == 0 || now - readAtMs > 20 * 1000) {
        titles = ClaudeSessionScan::desktopTitles(ClaudeSessionScan::desktopConfigRoot());
        readAtMs = now;
    }
    return titles;
}

} // namespace

namespace ClaudeSessionScan {

void readMeta(ExternalClaudeSession &s)
{
    QFile f(s.path);
    if (!f.open(QIODevice::ReadOnly))
        return;

    MetaScan m;
    const qint64 size = f.size();
    s.bytes = size;
    constexpr qint64 kHead = 96 * 1024;
    const QByteArray head = f.read(kHead);
    bool launchCwdSeen = false;
    for (const QByteArray &hl : head.split('\n')) {
        const QJsonObject o = QJsonDocument::fromJson(hl).object();
        if (o.isEmpty())
            continue;
        scanLine(o, s, m);
        if (!launchCwdSeen && o.contains(QStringLiteral("cwd"))) {
            const QString launchCwd = o.value(QStringLiteral("cwd")).toString();
            if (!launchCwd.isEmpty()) {
                launchCwdSeen = true;
                s.cwd = launchCwd;
            }
        }
    }

    const qint64 tailN = qMin<qint64>(qMax<qint64>(0, size - kHead), 64 * 1024);
    QByteArray tail;
    if (tailN > 0) {
        f.seek(size - tailN);
        tail = f.readAll();
        const int firstNl = tail.indexOf('\n');
        tail = firstNl >= 0 ? tail.mid(firstNl + 1) : QByteArray();
        for (const QByteArray &tl : tail.split('\n')) {
            const QJsonObject o = QJsonDocument::fromJson(tl).object();
            if (!o.isEmpty())
                scanLine(o, s, m);
        }
    }

    const QByteArray last = tail.isEmpty() ? head : tail;
    int nl = last.lastIndexOf('\n');
    if (nl < 0)
        nl = last.size();
    while (nl > 0) {
        const int prev = last.lastIndexOf('\n', nl - 1);
        const QByteArray line = last.mid(prev + 1, nl - prev - 1);
        const QJsonObject o = QJsonDocument::fromJson(line).object();
        if (o.contains(QStringLiteral("cwd"))) {
            if (!launchCwdSeen)
                s.cwd = o.value(QStringLiteral("cwd")).toString();
            s.gitBranch = o.value(QStringLiteral("gitBranch")).toString();
            if (s.gitBranch == QLatin1String("HEAD"))
                s.gitBranch.clear();
            break;
        }
        if (prev < 0)
            break;
        nl = prev;
    }

    s.model = m.model;
    if (!m.customTitle.isEmpty())
        s.title = m.customTitle;
    else if (!m.aiTitle.isEmpty())
        s.title = m.aiTitle;
    else
        s.title = promptTitle(s.firstPrompt);
    if (s.title.isEmpty())
        s.title = QStringLiteral("Claude Code session");
}

QString promptTitle(const QString &prompt)
{
    for (const QString &line : prompt.split(QLatin1Char('\n'))) {
        QString t = line.simplified();
        if (t.isEmpty())
            continue;
        if (t.size() > 80)
            t = t.left(79).trimmed() + QChar(0x2026);
        return t;
    }
    return QString();
}

QString desktopConfigRoot()
{
#if defined(Q_OS_MACOS)
    return QDir::homePath() + QStringLiteral("/Library/Application Support/Claude");
#elif defined(Q_OS_WIN)
    const QString appData = qEnvironmentVariable("APPDATA");
    return appData.isEmpty() ? QString() : QDir(appData).filePath(QStringLiteral("Claude"));
#else
    QString config = qEnvironmentVariable("XDG_CONFIG_HOME");
    if (config.isEmpty())
        config = QDir::homePath() + QStringLiteral("/.config");
    return config + QStringLiteral("/Claude");
#endif
}

QHash<QString, QString> desktopTitles(const QString &desktopRoot)
{
    QHash<QString, QString> out;
    if (desktopRoot.isEmpty())
        return out;
    QDir root(desktopRoot + QStringLiteral("/claude-code-sessions"));
    if (!root.exists())
        return out;
    for (const QFileInfo &account : root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        for (const QFileInfo &org :
             QDir(account.absoluteFilePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            for (const QFileInfo &fi : QDir(org.absoluteFilePath())
                                           .entryInfoList({QStringLiteral("local_*.json")},
                                                          QDir::Files)) {
                QFile f(fi.absoluteFilePath());
                if (fi.size() > 1024 * 1024 || !f.open(QIODevice::ReadOnly))
                    continue;
                const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
                const QString id = o.value(QStringLiteral("cliSessionId")).toString();
                const QString title = o.value(QStringLiteral("title")).toString().trimmed();
                if (!id.isEmpty() && !title.isEmpty())
                    out.insert(id, title);
            }
        }
    }
    return out;
}

QString projectKey(const QString &cwd)
{
    QString key = QDir::cleanPath(cwd);
    static const QRegularExpression other(QStringLiteral("[^A-Za-z0-9]"));
    key.replace(other, QStringLiteral("-"));
    return key;
}

// code-health: allow(dir-walk-in-loop) only called from runOffThread in MainWindow::scanExternalClaudeSessions()
QList<ExternalClaudeSession> scan(const QString &repoLocalPath,
                                  const QSet<QString> &excludeCwds,
                                  qint64 activeWindowMs,
                                  const QStringList &ownConfigDirs)
{
    QList<ExternalClaudeSession> out;
    if (repoLocalPath.isEmpty())
        return out;
    const QString repo = QDir(repoLocalPath).absolutePath();

    QDir projects(QDir::homePath() + QStringLiteral("/.claude/projects"));
    if (!projects.exists())
        return out;

    QStringList ownRoots;
    for (const QString &dir : ownConfigDirs) {
        const QString canonical = QFileInfo(dir).canonicalFilePath();
        if (!canonical.isEmpty())
            ownRoots.append(canonical);
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QSet<QString> seen;
    const auto dirs = projects.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &di : dirs) {
        if (!ownRoots.isEmpty()) {
            const QString target = di.canonicalFilePath();
            if (std::any_of(ownRoots.cbegin(), ownRoots.cend(),
                            [&target](const QString &root) { return isUnder(target, root); }))
                continue;
        }
        QDir d(di.absoluteFilePath());
        const auto files = d.entryInfoList(QStringList{QStringLiteral("*.jsonl")},
                                           QDir::Files, QDir::Time);
        for (const QFileInfo &fi : files) {
            if (now - fi.lastModified().toMSecsSinceEpoch() > activeWindowMs)
                break;
            ExternalClaudeSession s;
            s.uuid = fi.completeBaseName();
            if (seen.contains(s.uuid))
                continue;
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
            seen.insert(s.uuid);
            out.append(s);
        }
    }
    if (!out.isEmpty()) {
        const QHash<QString, QString> titles = cachedDesktopTitles();
        for (ExternalClaudeSession &s : out)
            applyDesktopTitle(s, titles);
    }
    return out;
}

QList<ExternalClaudeSession> listAll(const QStringList &configDirs,
                                     const QString &repoLocalPath,
                                     bool anyFolder,
                                     const QStringList &skipRoots)
{
    QList<ExternalClaudeSession> out;
    const QString repo =
        repoLocalPath.isEmpty() ? QString() : QDir(repoLocalPath).absolutePath();
    if (repo.isEmpty() && !anyFolder)
        return out;
    const QString repoKey = repo.isEmpty() ? QString() : projectKey(repo);
    QStringList skip;
    QStringList skipKeys;
    for (const QString &root : skipRoots) {
        if (root.isEmpty())
            continue;
        skip << QDir(root).absolutePath();
        skipKeys << projectKey(QDir(root).absolutePath());
    }
    const auto skipped = [&skip](const QString &cwd) {
        for (const QString &root : skip)
            if (isUnder(cwd, root))
                return true;
        return false;
    };
    QHash<QString, int> byUuid;
    QStringList roots;
    for (const QString &dir : configDirs) {
        const QString clean = QDir::cleanPath(dir);
        if (!clean.isEmpty() && !roots.contains(clean))
            roots.append(clean);
    }
    for (const QString &configDir : std::as_const(roots)) {
        QDir projects(configDir + QStringLiteral("/projects"));
        if (!projects.exists())
            continue;
        for (const QFileInfo &di : projects.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            if (!anyFolder && !di.fileName().startsWith(repoKey))
                continue;
            if (std::any_of(skipKeys.cbegin(), skipKeys.cend(), [&di](const QString &key) {
                    return di.fileName().startsWith(key);
                }))
                continue;
            const auto files = QDir(di.absoluteFilePath())
                                   .entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files);
            for (const QFileInfo &fi : files) {
                ExternalClaudeSession s;
                s.uuid = fi.completeBaseName();
                s.path = fi.absoluteFilePath();
                s.lastActivityMs = fi.lastModified().toMSecsSinceEpoch();
                const auto known = byUuid.constFind(s.uuid);
                if (known != byUuid.constEnd()) {
                    ExternalClaudeSession &have = out[*known];
                    const QString home = projectKey(have.cwd);
                    const bool haveHome = QFileInfo(have.path).dir().dirName() == home;
                    const bool thisHome = di.fileName() == home;
                    if ((thisHome && !haveHome) ||
                        (thisHome == haveHome && fi.size() > have.bytes)) {
                        readMeta(s);
                        if (!s.cwd.isEmpty())
                            have = s;
                    }
                    continue;
                }
                readMeta(s);
                if (s.cwd.isEmpty())
                    continue;
                if (s.firstPrompt.isEmpty() && s.title == QLatin1String("Claude Code session"))
                    continue;
                const QString cwd = QDir(s.cwd).absolutePath();
                if (!anyFolder && !isUnder(cwd, repo))
                    continue;
                if (skipped(cwd))
                    continue;
                byUuid.insert(s.uuid, out.size());
                out.append(s);
            }
        }
    }
    const QHash<QString, QString> titles = desktopTitles(desktopConfigRoot());
    for (ExternalClaudeSession &s : out)
        applyDesktopTitle(s, titles);
    std::sort(out.begin(), out.end(),
              [](const ExternalClaudeSession &a, const ExternalClaudeSession &b) {
                  return a.lastActivityMs > b.lastActivityMs;
              });
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
