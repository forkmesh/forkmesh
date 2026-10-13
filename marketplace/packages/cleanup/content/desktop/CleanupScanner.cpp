#include "CleanupScanner.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QLocale>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>

#include <algorithm>

#if defined(Q_OS_UNIX)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace forkmesh::cleanup {
namespace {

constexpr qint64 kMinuteMs = 60 * 1000;
constexpr qint64 kDayMs = 24 * 60 * kMinuteMs;

bool cancelled(const CancelFlag &cancel)
{
    return cancel && cancel->load(std::memory_order_relaxed);
}

QString clean(const QString &path)
{
    return path.isEmpty() ? QString() : QDir::cleanPath(QDir(path).absolutePath());
}

QString canonical(const QString &path)
{
    const QString resolved = QFileInfo(path).canonicalFilePath();
    return resolved.isEmpty() ? clean(path) : resolved;
}

bool samePath(const QString &a, const QString &b)
{
    if (a.trimmed().isEmpty() || b.trimmed().isEmpty())
        return false;
    return clean(a) == clean(b) || canonical(a) == canonical(b);
}

QString readLink(const QString &path)
{
#if defined(Q_OS_UNIX)
    QByteArray buffer(4096, Qt::Uninitialized);
    const QByteArray native = QFile::encodeName(path);
    const ssize_t length = ::readlink(native.constData(), buffer.data(), buffer.size() - 1);
    if (length <= 0)
        return QString();
    QString target = QFile::decodeName(QByteArray(buffer.constData(), int(length)));
    if (target.endsWith(QLatin1String(" (deleted)")))
        target.chop(10);
    return target;
#else
    return QFileInfo(path).symLinkTarget();
#endif
}

QByteArray readHead(const QString &path, qint64 limit)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return QByteArray();
    return file.read(limit);
}

QFileInfoList childEntries(const QString &dir)
{
    return QDir(dir).entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::System |
                                       QDir::NoDotAndDotDot,
                                   QDir::Name);
}

QFileInfoList childDirs(const QString &dir)
{
    QFileInfoList dirs;
    for (const QFileInfo &info : childEntries(dir)) {
        if (info.isDir() && !info.isSymLink())
            dirs.append(info);
    }
    return dirs;
}

bool ownedByUs(const QFileInfo &info, const QString &uid)
{
    if (uid.isEmpty())
        return true;
    return QString::number(info.ownerId()) == uid;
}

QString git(const QString &dir, const QStringList &args, bool *ok = nullptr)
{
    QProcess process;
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("GIT_OPTIONAL_LOCKS"), QStringLiteral("0"));
    env.insert(QStringLiteral("GIT_TERMINAL_PROMPT"), QStringLiteral("0"));
    env.insert(QStringLiteral("LC_ALL"), QStringLiteral("C"));
    process.setProcessEnvironment(env);
    process.setWorkingDirectory(dir);
    process.start(QStringLiteral("git"), QStringList{QStringLiteral("-C"), dir} + args);
    const bool finished = process.waitForStarted(5000) && process.waitForFinished(30000);
    if (!finished)
        process.kill();
    const bool good = finished && process.exitStatus() == QProcess::NormalExit &&
                      process.exitCode() == 0;
    if (ok)
        *ok = good;
    return good ? QString::fromUtf8(process.readAllStandardOutput()).trimmed() : QString();
}

struct WorktreeEntry {
    QString path;
    QString head;
    QString branch;
    bool locked = false;
    QString lockReason;
    bool prunable = false;
};

struct RepoGit {
    bool ok = false;
    QString mainBranch;
    QString base;
    QList<WorktreeEntry> entries;
};

QList<WorktreeEntry> parseWorktrees(const QString &porcelain)
{
    QList<WorktreeEntry> entries;
    WorktreeEntry current;
    bool open = false;
    const auto flush = [&] {
        if (open && !current.path.isEmpty())
            entries.append(current);
        current = WorktreeEntry();
        open = false;
    };
    for (const QString &raw : porcelain.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (line.isEmpty()) {
            flush();
        } else if (line.startsWith(QLatin1String("worktree "))) {
            flush();
            current.path = line.mid(9);
            open = true;
        } else if (line.startsWith(QLatin1String("HEAD "))) {
            current.head = line.mid(5);
        } else if (line.startsWith(QLatin1String("branch "))) {
            current.branch = line.mid(7);
            if (current.branch.startsWith(QLatin1String("refs/heads/")))
                current.branch = current.branch.mid(11);
        } else if (line == QLatin1String("locked") || line.startsWith(QLatin1String("locked "))) {
            current.locked = true;
            current.lockReason = line.mid(7).trimmed();
        } else if (line == QLatin1String("prunable") ||
                   line.startsWith(QLatin1String("prunable "))) {
            current.prunable = true;
        }
    }
    flush();
    return entries;
}

QString commonRepoFor(const QString &worktree)
{
    const QString marker =
        QString::fromUtf8(readHead(worktree + QStringLiteral("/.git"), 4096)).trimmed();
    if (!marker.startsWith(QLatin1String("gitdir:")))
        return QString();
    QString gitdir = marker.mid(7).trimmed();
    if (QFileInfo(gitdir).isRelative())
        gitdir = QDir(worktree).filePath(gitdir);
    gitdir = QDir::cleanPath(gitdir);
    QString common = gitdir;
    const QString pointer =
        QString::fromUtf8(readHead(gitdir + QStringLiteral("/commondir"), 4096)).trimmed();
    if (!pointer.isEmpty())
        common = QDir::cleanPath(QFileInfo(pointer).isRelative() ? QDir(gitdir).filePath(pointer)
                                                                 : pointer);
    if (!QFileInfo(common).isDir())
        return QString();
    if (QFileInfo(common).fileName() == QLatin1String(".git"))
        return QFileInfo(common).absolutePath();
    return common;
}

QString transcriptCwd(const QString &projectDir)
{
    QFileInfoList transcripts =
        QDir(projectDir).entryInfoList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Time);
    static const QRegularExpression cwdField(QStringLiteral("\"cwd\"\\s*:\\s*\"((?:[^\"\\\\]|\\\\.)*)\""));
    for (const QFileInfo &info : std::as_const(transcripts)) {
        const QRegularExpressionMatch match =
            cwdField.match(QString::fromUtf8(readHead(info.filePath(), 256 * 1024)));
        if (match.hasMatch()) {
            QString cwd = match.captured(1);
            cwd.replace(QLatin1String("\\/"), QLatin1String("/"));
            cwd.replace(QLatin1String("\\\\"), QLatin1String("\\"));
            return cwd;
        }
    }
    return QString();
}

bool projectMatchesCwd(const QString &dirName, const QString &cwd)
{
    const QString encoded = encodeClaudeProject(cwd);
    if (dirName == encoded)
        return true;
    return encoded.size() > 120 && dirName.size() > 120 &&
           dirName.left(120) == encoded.left(120);
}

QString baseName(const QString &path)
{
    return QFileInfo(path).fileName();
}

QString memberKey(const QString &name)
{
    return name.endsWith(QLatin1String(".jsonl")) ? name.left(name.size() - 6) : name;
}

QStringList claudeProjectRoots(const Context &context)
{
    QStringList roots{context.home + QStringLiteral("/.claude/projects")};
    if (!context.dataDir.isEmpty()) {
        for (const QFileInfo &provider : childDirs(context.dataDir + QStringLiteral("/agent-accounts"))) {
            for (const QFileInfo &account : childDirs(provider.filePath()))
                roots << account.filePath() + QStringLiteral("/projects");
        }
    }
    return roots;
}

class Scanner
{
public:
    Scanner(const Context &context, const CancelFlag &cancel, const Progress &progress)
        : m_context(context), m_cancel(cancel), m_progress(progress)
    {
    }

    ScanResult run()
    {
        m_live = liveClaudeSessions(m_context);
        m_claudeRunning = std::any_of(m_context.processes.processes.cbegin(),
                                      m_context.processes.processes.cend(), isClaudeProcess);
        m_codexRunning = std::any_of(m_context.processes.processes.cbegin(),
                                     m_context.processes.processes.cend(), isCodexProcess);
        const std::function<void()> steps[] = {
            [this] { stage(QStringLiteral("Agent worktrees")); scanWorktrees(); },
            [this] { stage(QStringLiteral("Claude Code")); scanClaude(); },
            [this] { stage(QStringLiteral("Codex")); scanCodex(); },
            [this] { stage(QStringLiteral("Builds and temp files")); scanBuilds(); },
            [this] { stage(QStringLiteral("Other caches")); scanOtherCaches(); },
        };
        for (const auto &step : steps) {
            if (cancelled(m_cancel))
                break;
            step();
        }
        m_result.cancelled = cancelled(m_cancel);
        return m_result;
    }

private:
    void stage(const QString &text)
    {
        if (m_progress)
            m_progress(text);
    }

    bool isLiveName(const QString &name) const
    {
        for (const QString &uuid : uuidsIn(name)) {
            if (m_live.contains(uuid))
                return true;
        }
        return false;
    }

    bool recent(qint64 newestMs) const
    {
        return newestMs > 0 && m_context.nowMs - newestMs < m_context.liveMinutes * kMinuteMs;
    }

    QString recentText() const
    {
        return QStringLiteral("changed in the last %1 minutes").arg(m_context.liveMinutes);
    }

    void addMember(Item &item, const QString &path, const Usage &usage)
    {
        item.members.append({path, usage.bytes});
        item.bytes += usage.bytes;
        item.files += usage.files;
        item.modifiedMs = std::max(item.modifiedMs, usage.newestMs);
    }

    void keep(Item &item, const Usage &usage)
    {
        item.keptBytes += usage.bytes;
    }

    Item makeItem(Category category, Kind kind, const QString &title, const QString &path,
                  const QString &parent)
    {
        Item item;
        item.category = category;
        item.kind = kind;
        item.title = title;
        item.path = clean(path);
        item.parent = clean(parent);
        item.id = QStringLiteral("%1:%2:%3")
                      .arg(int(category))
                      .arg(title, item.path);
        return item;
    }

    void dropInUseMembers(Item &item)
    {
        QList<Member> kept;
        QStringList users;
        int busy = 0;
        for (const Member &member : std::as_const(item.members)) {
            const QStringList using_ = m_context.processes.usersOf(member.path);
            if (using_.isEmpty()) {
                kept.append(member);
                continue;
            }
            ++busy;
            item.bytes -= member.bytes;
            item.keptBytes += member.bytes;
            for (const QString &user : using_) {
                if (!users.contains(user))
                    users << user;
            }
        }
        if (busy == 0)
            return;
        item.members = kept;
        if (kept.isEmpty()) {
            item.status = Status::InUse;
            item.reasons.prepend(QStringLiteral("open in %1").arg(users.mid(0, 3).join(QStringLiteral(", "))));
        } else {
            item.reasons << QStringLiteral("%1 kept: open in %2")
                                .arg(busy)
                                .arg(users.mid(0, 3).join(QStringLiteral(", ")));
        }
    }

    void push(Item item, bool keepEmpty = false)
    {
        if (item.kind == Kind::Files && item.status != Status::Protected)
            dropInUseMembers(item);
        if (!keepEmpty && item.members.isEmpty() && item.bytes <= 0 && item.keptBytes <= 0 &&
            item.kind == Kind::Files)
            return;
        if (item.kind == Kind::Files && item.members.isEmpty() &&
            (item.status == Status::Orphaned || item.status == Status::Attached))
            item.status = Status::InUse;
        m_result.items.append(item);
    }

    Item membersOf(Category category, const QString &title, const QString &dir,
                   const std::function<bool(const QFileInfo &, const Usage &, QString *)> &keepIt)
    {
        Item item = makeItem(category, Kind::Files, title, dir, dir);
        QHash<QString, int> keptWhy;
        for (const QFileInfo &entry : childEntries(dir)) {
            if (cancelled(m_cancel))
                break;
            const Usage usage = measure(entry.filePath(), m_cancel);
            QString why;
            if (keepIt && keepIt(entry, usage, &why)) {
                keep(item, usage);
                keptWhy[why] += 1;
                continue;
            }
            addMember(item, entry.filePath(), usage);
        }
        for (auto it = keptWhy.cbegin(); it != keptWhy.cend(); ++it)
            item.reasons << QStringLiteral("%1 kept: %2").arg(it.value()).arg(it.key());
        return item;
    }

    std::function<bool(const QFileInfo &, const Usage &, QString *)> liveFilter() const
    {
        return [this](const QFileInfo &entry, const Usage &usage, QString *why) {
            if (isLiveName(entry.fileName())) {
                *why = QStringLiteral("live session");
                return true;
            }
            if (recent(usage.newestMs)) {
                *why = recentText();
                return true;
            }
            return false;
        };
    }

    void scanClaude()
    {
        const QString claude = m_context.home + QStringLiteral("/.claude");
        const QString projects = claude + QStringLiteral("/projects");
        for (const QFileInfo &project : childDirs(projects)) {
            if (cancelled(m_cancel))
                return;
            scanClaudeProject(project);
        }
        for (const QString &name :
             {QStringLiteral("shell-snapshots"), QStringLiteral("todos"), QStringLiteral("debug"),
              QStringLiteral("session-env"), QStringLiteral("file-history"),
              QStringLiteral("paste-cache"), QStringLiteral("telemetry")}) {
            const QString dir = claude + QLatin1Char('/') + name;
            if (!QFileInfo(dir).isDir() || QFileInfo(dir).isSymLink())
                continue;
            Item item = membersOf(Category::Claude, QStringLiteral("Claude %1").arg(name), dir,
                                  liveFilter());
            item.reasons.prepend(QStringLiteral("per-session state of finished sessions"));
            push(item);
        }
        for (const QString &root : m_context.tmpRoots)
            scanClaudeTemp(root + QStringLiteral("/claude-") + m_context.uid);
        const QString cache = m_context.home + QStringLiteral("/.cache");
        for (const QFileInfo &entry : childEntries(cache)) {
            if (!entry.fileName().startsWith(QLatin1String("claude")) || entry.isSymLink())
                continue;
            Item item = makeItem(Category::Claude, Kind::Files, entry.fileName(), entry.filePath(),
                                 cache);
            addMember(item, entry.filePath(), measure(entry.filePath(), m_cancel));
            item.reasons << QStringLiteral("Claude Code cache; rebuilt when needed");
            if (recent(item.modifiedMs) && m_claudeRunning) {
                item.status = Status::Attached;
                item.reasons << recentText();
            }
            push(item);
        }
        scanClaudeVersions();
        scanAgentAccounts();
    }

    void scanClaudeProject(const QFileInfo &project)
    {
        const QString cwd = transcriptCwd(project.filePath());
        const bool folderExists = !cwd.isEmpty() && QFileInfo(cwd).isDir();
        const QString label = cwd.isEmpty() ? project.fileName() : cwd;
        Item old = makeItem(Category::Claude, Kind::Files,
                            QStringLiteral("Old transcripts · %1").arg(label), project.filePath(),
                            project.filePath());
        Item current = makeItem(Category::Claude, Kind::Files,
                                QStringLiteral("Transcripts · %1").arg(label), project.filePath(),
                                project.filePath());
        int liveKept = 0;
        qint64 memory = 0;
        for (const QFileInfo &entry : childEntries(project.filePath())) {
            if (cancelled(m_cancel))
                return;
            const Usage usage = measure(entry.filePath(), m_cancel);
            if (entry.fileName() == QLatin1String("memory")) {
                memory += usage.bytes;
                continue;
            }
            if (m_live.contains(memberKey(entry.fileName())) || recent(usage.newestMs)) {
                ++liveKept;
                keep(current, usage);
                continue;
            }
            const bool isOld = usage.newestMs > 0 &&
                               m_context.nowMs - usage.newestMs >
                                   m_context.oldSessionDays * kDayMs;
            addMember(folderExists && !isOld ? current : old, entry.filePath(), usage);
        }
        const QString notes = [&] {
            QStringList parts;
            if (liveKept > 0)
                parts << QStringLiteral("%1 live session%2 kept")
                             .arg(liveKept)
                             .arg(liveKept == 1 ? QString() : QStringLiteral("s"));
            if (memory > 0)
                parts << QStringLiteral("memory/ kept");
            return parts.join(QStringLiteral(" · "));
        }();
        if (folderExists) {
            current.status = Status::Attached;
            current.reasons << QStringLiteral("folder %1 still exists").arg(cwd);
            old.reasons << QStringLiteral("sessions older than %1 days").arg(m_context.oldSessionDays);
            if (!notes.isEmpty())
                current.reasons << notes;
            if (!old.members.isEmpty())
                push(old);
            if (!current.members.isEmpty() || current.keptBytes > 0)
                push(current, true);
            return;
        }
        old.title = QStringLiteral("Transcripts · %1").arg(label);
        old.reasons << (cwd.isEmpty() ? QStringLiteral("no transcript names its folder")
                                      : QStringLiteral("folder %1 is gone").arg(cwd));
        old.keptBytes = current.keptBytes;
        if (!notes.isEmpty())
            old.reasons << notes;
        if (!old.members.isEmpty() || old.keptBytes > 0)
            push(old, true);
    }

    void scanClaudeTemp(const QString &dir)
    {
        const QFileInfo info(dir);
        if (!info.isDir() || info.isSymLink())
            return;
        Item misc = makeItem(Category::Claude, Kind::Files, QStringLiteral("Claude temp files"), dir, dir);
        int kept = 0;
        for (const QFileInfo &entry : childEntries(dir)) {
            if (cancelled(m_cancel))
                return;
            if (entry.isDir() && !entry.isSymLink() && entry.fileName().startsWith(QLatin1Char('-'))) {
                Item item = membersOf(Category::Claude,
                                      QStringLiteral("Scratchpads · %1").arg(entry.fileName()),
                                      entry.filePath(), liveFilter());
                item.reasons.prepend(QStringLiteral("scratchpads and task output of finished sessions"));
                push(item);
                continue;
            }
            const Usage usage = measure(entry.filePath(), m_cancel);
            if (isLiveName(entry.fileName()) || recent(usage.newestMs) ||
                (m_claudeRunning && entry.fileName() == QLatin1String("bundled-skills"))) {
                ++kept;
                keep(misc, usage);
                continue;
            }
            addMember(misc, entry.filePath(), usage);
        }
        misc.reasons << QStringLiteral("diff and cache files from finished sessions");
        if (kept > 0)
            misc.reasons << QStringLiteral("%1 kept for running sessions").arg(kept);
        push(misc);
    }

    void scanClaudeVersions()
    {
        const QString dir = m_context.home + QStringLiteral("/.local/share/claude/versions");
        const QString current = m_context.claudeBinary.isEmpty()
                                    ? QString()
                                    : canonical(m_context.claudeBinary);
        for (const QFileInfo &entry : childEntries(dir)) {
            if (entry.isSymLink())
                continue;
            Item item = makeItem(Category::Claude, Kind::Files,
                                 QStringLiteral("Claude Code %1").arg(entry.fileName()),
                                 entry.filePath(), dir);
            addMember(item, entry.filePath(), measure(entry.filePath(), m_cancel));
            if (!current.isEmpty() && pathWithin(current, canonical(entry.filePath()))) {
                item.status = Status::Protected;
                item.reasons << QStringLiteral("the version `claude` runs");
                item.members.clear();
            } else {
                item.reasons << QStringLiteral("older Claude Code version");
            }
            push(item, true);
        }
    }

    void scanAgentAccounts()
    {
        if (m_context.dataDir.isEmpty())
            return;
        const QString root = m_context.dataDir + QStringLiteral("/agent-accounts");
        for (const QFileInfo &provider : childDirs(root)) {
            for (const QFileInfo &account : childDirs(provider.filePath())) {
                const QString projects = account.filePath() + QStringLiteral("/projects");
                if (!QFileInfo(projects).isDir())
                    continue;
                Item item = makeItem(provider.fileName() == QLatin1String("codex") ? Category::Codex
                                                                                  : Category::Claude,
                                     Kind::Files,
                                     QStringLiteral("Desktop agent transcripts · %1 %2")
                                         .arg(provider.fileName(), account.fileName().left(8)),
                                     projects, projects);
                const Usage usage = measure(projects, m_cancel);
                item.keptBytes = usage.bytes;
                item.files = usage.files;
                item.modifiedMs = usage.newestMs;
                item.status = Status::Protected;
                item.reasons << QStringLiteral("ForkMesh user data (agent history); never deleted here");
                push(item, true);
            }
        }
    }

    void scanCodex()
    {
        const QString codex = m_context.home + QStringLiteral("/.codex");
        if (!QFileInfo(codex).isDir())
            return;
        const QString sessions = codex + QStringLiteral("/sessions");
        if (QFileInfo(sessions).isDir()) {
            Item item = makeItem(Category::Codex, Kind::Files, QStringLiteral("Codex sessions"),
                                 sessions, sessions);
            QHash<QString, Usage> days;
            for (const QFileInfo &year : childDirs(sessions)) {
                for (const QFileInfo &month : childDirs(year.filePath())) {
                    for (const QFileInfo &day : childDirs(month.filePath()))
                        days.insert(day.filePath(), measure(day.filePath(), m_cancel));
                }
            }
            for (auto it = days.cbegin(); it != days.cend(); ++it) {
                if (recent(it.value().newestMs)) {
                    keep(item, it.value());
                    continue;
                }
                addMember(item, it.key(), it.value());
            }
            std::sort(item.members.begin(), item.members.end(),
                      [](const Member &a, const Member &b) { return a.path < b.path; });
            item.reasons << QStringLiteral("rollouts of finished Codex sessions");
            push(item);
        }
        Item logs = makeItem(Category::Codex, Kind::Files, QStringLiteral("Codex logs"), codex, codex);
        for (const QFileInfo &entry : childEntries(codex)) {
            const QString name = entry.fileName();
            if (name == QLatin1String("log") ||
                (name.startsWith(QLatin1String("logs_")) && name.contains(QLatin1String(".sqlite"))))
                addMember(logs, entry.filePath(), measure(entry.filePath(), m_cancel));
        }
        if (m_codexRunning && !logs.members.isEmpty()) {
            logs.keptBytes += logs.bytes;
            logs.bytes = 0;
            logs.members.clear();
            logs.status = Status::InUse;
            logs.reasons << QStringLiteral("Codex is running and writes these");
        } else {
            logs.reasons << QStringLiteral("Codex diagnostic logs");
        }
        push(logs, logs.keptBytes > 0);
        for (const QString &name : {QStringLiteral("cache"), QStringLiteral("tmp"),
                                    QStringLiteral("shell_snapshots")}) {
            const QString dir = codex + QLatin1Char('/') + name;
            if (!QFileInfo(dir).isDir() || QFileInfo(dir).isSymLink())
                continue;
            Item item = membersOf(Category::Codex, QStringLiteral("Codex %1").arg(name), dir,
                                  liveFilter());
            item.reasons.prepend(QStringLiteral("Codex scratch; rebuilt when needed"));
            push(item);
        }
        const QString images = codex + QStringLiteral("/generated_images");
        if (QFileInfo(images).isDir() && !QFileInfo(images).isSymLink()) {
            Item item = membersOf(Category::Codex, QStringLiteral("Codex generated images"), images, {});
            item.status = Status::Attached;
            item.confirmEach = true;
            item.reasons << QStringLiteral("images Codex made for you");
            push(item);
        }
        const QString releases = codex + QStringLiteral("/packages/standalone/releases");
        const QString currentRelease =
            canonical(codex + QStringLiteral("/packages/standalone/current"));
        for (const QFileInfo &entry : childDirs(releases)) {
            Item item = makeItem(Category::Codex, Kind::Files,
                                 QStringLiteral("Codex %1").arg(entry.fileName()), entry.filePath(),
                                 releases);
            addMember(item, entry.filePath(), measure(entry.filePath(), m_cancel));
            if (samePath(currentRelease, entry.filePath())) {
                item.status = Status::Protected;
                item.reasons << QStringLiteral("the release `codex` runs");
                item.members.clear();
            } else {
                item.reasons << QStringLiteral("older Codex release");
            }
            push(item, true);
        }
        for (const QString &root : m_context.tmpRoots) {
            Item temp = makeItem(Category::Codex, Kind::Files, QStringLiteral("Codex temp files"),
                                 root, root);
            for (const QFileInfo &entry : childEntries(root)) {
                if (!entry.fileName().startsWith(QLatin1String("codex-")) || entry.isSymLink() ||
                    !ownedByUs(entry, m_context.uid))
                    continue;
                const Usage usage = measure(entry.filePath(), m_cancel);
                if (m_codexRunning || recent(usage.newestMs)) {
                    keep(temp, usage);
                    continue;
                }
                addMember(temp, entry.filePath(), usage);
            }
            temp.reasons << QStringLiteral("sandbox mount points from earlier Codex runs");
            push(temp);
        }
    }

    void scanWorktrees()
    {
        QStringList roots;
        const auto addRoot = [&roots](const QString &root) {
            if (root.isEmpty() || !QFileInfo(root).isDir() || QFileInfo(root).isSymLink())
                return;
            const QString resolved = canonical(root);
            if (!roots.contains(resolved))
                roots << resolved;
        };
        for (const cleanupfeed::Repository &repo : m_context.repositories) {
            addRoot(repo.path + QStringLiteral("/.worktrees"));
            addRoot(repo.path + QStringLiteral("/.claude/worktrees"));
        }
        addRoot(m_context.worktreesDir);
        for (const QString &tmp : m_context.tmpRoots)
            addRoot(tmp + QStringLiteral("/forkmesh-worktrees"));
        for (const QString &root : std::as_const(roots)) {
            for (const QFileInfo &child : childDirs(root)) {
                if (cancelled(m_cancel))
                    return;
                scanWorktree(root, child);
            }
        }
    }

    RepoGit &repoGit(const QString &repoPath)
    {
        auto it = m_repos.find(repoPath);
        if (it != m_repos.end())
            return it.value();
        RepoGit info;
        bool ok = false;
        info.entries = parseWorktrees(
            git(repoPath, {QStringLiteral("worktree"), QStringLiteral("list"), QStringLiteral("--porcelain")},
                &ok));
        info.ok = ok;
        if (!info.entries.isEmpty())
            info.mainBranch = info.entries.first().branch;
        for (const QString &candidate : {QStringLiteral("main"), QStringLiteral("master")}) {
            git(repoPath, {QStringLiteral("rev-parse"), QStringLiteral("--verify"), QStringLiteral("-q"),
                           QStringLiteral("refs/heads/") + candidate},
                &ok);
            if (ok) {
                info.base = candidate;
                break;
            }
        }
        if (info.base.isEmpty())
            info.base = info.mainBranch;
        return m_repos.insert(repoPath, info).value();
    }

    QString repoName(const QString &repoPath) const
    {
        for (const cleanupfeed::Repository &repo : m_context.repositories) {
            if (samePath(repo.path, repoPath))
                return repo.name;
        }
        return QFileInfo(repoPath).fileName();
    }

    bool sameRepository(const QString &repository, const QString &repoPath) const
    {
        if (repository.isEmpty())
            return true;
        const QString name = repoName(repoPath);
        return repository.compare(name, Qt::CaseInsensitive) == 0 ||
               repository.section(QLatin1Char('/'), -1)
                       .compare(name.section(QLatin1Char('/'), -1), Qt::CaseInsensitive) == 0;
    }

    void scanWorktree(const QString &root, const QFileInfo &child)
    {
        const QString path = child.filePath();
        m_worktreeNames << child.fileName();
        const QFileInfo marker(path + QStringLiteral("/.git"));
        const Usage usage = measure(path, m_cancel);
        if (marker.isDir()) {
            Item item = makeItem(Category::Worktrees, Kind::Files, child.fileName(), path, root);
            item.keptBytes = usage.bytes;
            item.files = usage.files;
            item.modifiedMs = usage.newestMs;
            item.status = Status::Protected;
            item.reasons << QStringLiteral("a full git clone with its own .git folder; never removed here");
            push(item, true);
            return;
        }
        if (!marker.exists()) {
            Item item = makeItem(Category::Worktrees, Kind::Files, child.fileName(), path, root);
            addMember(item, path, usage);
            item.reasons << QStringLiteral("leftover folder; not a git worktree");
            if (recent(usage.newestMs)) {
                item.status = Status::Attached;
                item.reasons << recentText();
            }
            push(item);
            return;
        }
        const QString repoPath = commonRepoFor(path);
        Item item = makeItem(Category::Worktrees, Kind::Worktree, child.fileName(), path, root);
        item.bytes = usage.bytes;
        item.files = usage.files;
        item.modifiedMs = usage.newestMs;
        item.repoPath = repoPath;
        const WorktreeEntry *entry = nullptr;
        if (!repoPath.isEmpty()) {
            const RepoGit &info = repoGit(repoPath);
            for (const WorktreeEntry &candidate : info.entries) {
                if (samePath(candidate.path, path)) {
                    entry = &candidate;
                    break;
                }
            }
            item.baseBranch = info.base;
        }
        if (!entry) {
            item.kind = Kind::Files;
            item.keptBytes = item.bytes;
            item.bytes = 0;
            item.status = Status::Protected;
            item.reasons << (repoPath.isEmpty()
                                 ? QStringLiteral("its .git file points at a repository that is gone; remove it by hand")
                                 : QStringLiteral("has a .git file but %1 does not list it; run git worktree prune")
                                       .arg(repoPath));
            push(item, true);
            return;
        }
        item.branch = entry->branch;
        item.locked = entry->locked;
        QStringList attached;
        QStringList inUse;
        item.reasons << (item.branch.isEmpty()
                             ? QStringLiteral("detached HEAD in %1").arg(repoName(repoPath))
                             : QStringLiteral("worktree of branch %1 in %2")
                                   .arg(item.branch, repoName(repoPath)));
        bool ok = false;
        const QString status =
            git(path, {QStringLiteral("status"), QStringLiteral("--porcelain")}, &ok);
        item.dirtyFiles = ok ? int(status.split(QLatin1Char('\n'), Qt::SkipEmptyParts).size()) : 0;
        if (item.dirtyFiles > 0)
            attached << QStringLiteral("%1 uncommitted file%2")
                            .arg(item.dirtyFiles)
                            .arg(item.dirtyFiles == 1 ? QString() : QStringLiteral("s"));
        if (!item.baseBranch.isEmpty() && !entry->head.isEmpty()) {
            const QStringList counts =
                git(repoPath, {QStringLiteral("rev-list"), QStringLiteral("--left-right"),
                               QStringLiteral("--count"),
                               QStringLiteral("refs/heads/%1...%2").arg(item.baseBranch, entry->head)},
                    &ok)
                    .split(QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);
            if (ok && counts.size() == 2) {
                item.behind = counts.at(0).toInt();
                item.ahead = counts.at(1).toInt();
            }
        }
        if (item.ahead > 0)
            attached << QStringLiteral("%1 commit%2 not in %3")
                            .arg(item.ahead)
                            .arg(item.ahead == 1 ? QString() : QStringLiteral("s"), item.baseBranch);
        else if (item.ahead == 0)
            item.reasons << QStringLiteral("merged into %1").arg(item.baseBranch);
        if (item.behind > 0)
            item.reasons << QStringLiteral("%1 behind").arg(item.behind);
        if (item.locked)
            attached << (entry->lockReason.isEmpty()
                             ? QStringLiteral("locked")
                             : QStringLiteral("locked: %1").arg(entry->lockReason));
        for (const cleanupfeed::Agent &agent : m_context.agents) {
            const bool matches =
                samePath(agent.worktree, path) ||
                (!agent.branch.isEmpty() && agent.branch == item.branch &&
                 (agent.repoPath.isEmpty() || samePath(agent.repoPath, repoPath)));
            if (!matches)
                continue;
            const QString text = QStringLiteral("agent #%1 (%2)").arg(agent.id).arg(agent.status);
            item.agents << text;
            if (agent.live)
                inUse << QStringLiteral("agent #%1 is running").arg(agent.id);
            else
                attached << QStringLiteral("used by %1").arg(text);
        }
        for (const cleanupfeed::TaskLink &task : m_context.tasks) {
            const bool matches = samePath(task.worktree, path) ||
                                 (!task.branch.isEmpty() && task.branch == item.branch &&
                                  sameRepository(task.repository, repoPath));
            if (!matches)
                continue;
            const QString text = task.title.isEmpty()
                                     ? QStringLiteral("task %1").arg(task.taskId)
                                     : QStringLiteral("task “%1”").arg(task.title);
            item.tasks << text;
            attached << QStringLiteral("work item of %1").arg(text);
        }
        const QStringList users = m_context.processes.usersOf(path);
        if (!users.isEmpty())
            inUse << QStringLiteral("open in %1").arg(users.mid(0, 3).join(QStringLiteral(", ")));
        if (!inUse.isEmpty()) {
            item.status = Status::InUse;
            item.reasons = inUse + item.reasons + attached;
        } else if (!attached.isEmpty()) {
            item.status = Status::Attached;
            item.reasons += attached;
        } else {
            item.status = Status::Orphaned;
            item.reasons << QStringLiteral("nothing references it");
        }
        push(item, true);
    }

    void scanBuilds()
    {
        scanAgentBuilds();
        for (const QString &root : m_context.tmpRoots)
            scanTmp(root);
        const QString qttest = m_context.home + QStringLiteral("/.qttest/share/ForkMeshTests");
        if (QFileInfo(qttest).isDir()) {
            Item item = membersOf(Category::BuildTemp, QStringLiteral("Qt test temp folders"), qttest,
                                  [this](const QFileInfo &entry, const Usage &usage, QString *why) {
                                      if (!entry.fileName().contains(QLatin1String("qt_temp"))) {
                                          *why = QStringLiteral("not a test temp folder");
                                          return true;
                                      }
                                      if (recent(usage.newestMs)) {
                                          *why = recentText();
                                          return true;
                                      }
                                      return false;
                                  });
            item.reasons.prepend(QStringLiteral("left by killed window-test runs"));
            push(item);
        }
        scanSandboxes();
        scanPlaywright();
        scanCcache();
    }

    void scanAgentBuilds()
    {
        const QString builds = m_context.buildsDir;
        if (builds.isEmpty() || !QFileInfo(builds).isDir())
            return;
        static const QRegularExpression agentName(
            QStringLiteral("^agent-([A-Za-z0-9][A-Za-z0-9_-]*?)(?:-tmp)?(?:\\.[^/]*)?$"));
        QMap<QString, Item> groups;
        for (const QFileInfo &entry : childEntries(builds)) {
            if (cancelled(m_cancel))
                return;
            if (entry.isSymLink())
                continue;
            const QRegularExpressionMatch match = agentName.match(entry.fileName());
            const QString key = match.hasMatch() ? QStringLiteral("agent-") + match.captured(1)
                                                 : entry.fileName();
            if (!groups.contains(key)) {
                Item item = makeItem(Category::BuildTemp, Kind::Files,
                                     QStringLiteral("Build · %1").arg(key),
                                     builds + QLatin1Char('/') + key, builds);
                if (match.hasMatch())
                    classifyAgentBuild(item, match.captured(1));
                else {
                    item.status = Status::Attached;
                    item.reasons << QStringLiteral("build tree that is not an agent's");
                }
                groups.insert(key, item);
            }
            addMember(groups[key], entry.filePath(), measure(entry.filePath(), m_cancel));
        }
        for (Item &item : groups) {
            if (item.status == Status::Orphaned && recent(item.modifiedMs)) {
                item.status = Status::Attached;
                item.reasons << recentText();
            }
            push(item);
        }
    }

    void classifyAgentBuild(Item &item, const QString &slug)
    {
        bool numeric = false;
        const int number = slug.toInt(&numeric);
        for (const cleanupfeed::Agent &agent : m_context.agents) {
            if (!numeric || (agent.id != number && agent.issueNumber != number))
                continue;
            if (agent.live) {
                item.status = Status::Attached;
                item.reasons << QStringLiteral("agent #%1 is running").arg(agent.id);
                return;
            }
        }
        QString flat = slug;
        flat.remove(QLatin1Char('-'));
        for (const QString &name : std::as_const(m_worktreeNames)) {
            QString flatName = name;
            flatName.remove(QLatin1Char('-'));
            const bool matches =
                name == slug || name.startsWith(slug + QLatin1Char('-')) ||
                name == QStringLiteral("agent-") + slug ||
                name.startsWith(QStringLiteral("agent-%1-").arg(slug)) ||
                (!numeric && flat.size() >= 4 && flatName.startsWith(flat));
            if (matches) {
                item.status = Status::Attached;
                item.reasons << QStringLiteral("worktree %1 still exists").arg(name);
                return;
            }
        }
        item.reasons << QStringLiteral("agent %1 build; no worktree of that name is left").arg(slug);
    }

    void scanTmp(const QString &root)
    {
        static const QRegularExpression pattern(
            QStringLiteral("^(fm\\d+[-_.].*|fm-.*|fmmcp-.*|claude-[0-9a-f]+-cwd|ActionSecurity-.*|"
                           ".*qt_temp.*|ForkMesh-.*|forkmesh-build.*|chat-history-.*)$"));
        Item item = makeItem(Category::BuildTemp, Kind::Files, QStringLiteral("Agent temp files"),
                             root, root);
        for (const QFileInfo &entry : childEntries(root)) {
            if (cancelled(m_cancel))
                return;
            if (entry.isSymLink() || !pattern.match(entry.fileName()).hasMatch() ||
                !ownedByUs(entry, m_context.uid) || (!entry.isDir() && !entry.isFile()))
                continue;
            const Usage usage = measure(entry.filePath(), m_cancel);
            if (recent(usage.newestMs)) {
                keep(item, usage);
                continue;
            }
            addMember(item, entry.filePath(), usage);
        }
        item.reasons << QStringLiteral("scratch files agents left in %1").arg(root);
        if (item.keptBytes > 0)
            item.reasons << QStringLiteral("recent ones kept");
        push(item);
    }

    void scanSandboxes()
    {
        const QString base = m_context.sandboxDir;
        if (base.isEmpty() || !QFileInfo(base).isDir())
            return;
        for (const QFileInfo &entry : childDirs(base)) {
            if (cancelled(m_cancel))
                return;
            const QString name = entry.fileName();
            if (!name.startsWith(QLatin1String("forkmesh-run-")) &&
                !name.startsWith(QLatin1String("forkmesh-discard-")))
                continue;
            Item item = makeItem(Category::BuildTemp, Kind::ActionSandbox,
                                 QStringLiteral("Actions sandbox · %1").arg(name), entry.filePath(), base);
            const Usage usage = measure(entry.filePath(), m_cancel);
            addMember(item, entry.filePath(), usage);
            const QStringList users = m_context.processes.usersOf(entry.filePath());
            if (m_context.claimedSandboxes.contains(name)) {
                item.status = Status::InUse;
                item.reasons << QStringLiteral("a running Action owns it");
            } else if (!users.isEmpty()) {
                item.status = Status::InUse;
                item.reasons << QStringLiteral("open in %1").arg(users.mid(0, 3).join(QStringLiteral(", ")));
            } else {
                item.reasons << QStringLiteral("finished run's disposable tree");
            }
            push(item, true);
        }
        const QString cache = QFileInfo(base).absolutePath() + QStringLiteral("/cache");
        if (QFileInfo(cache).isDir()) {
            Item item = makeItem(Category::BuildTemp, Kind::Report, QStringLiteral("Actions build cache"),
                                 cache, QFileInfo(cache).absolutePath());
            const Usage usage = measure(cache, m_cancel);
            item.keptBytes = usage.bytes;
            item.files = usage.files;
            item.modifiedMs = usage.newestMs;
            item.status = Status::Protected;
            item.reasons << QStringLiteral("persistent per-repository build cache; Actions caps it itself");
            push(item, true);
        }
    }

    void scanPlaywright()
    {
        const QString root = m_context.home + QStringLiteral("/.cache/ms-playwright");
        QHash<QString, QFileInfo> newest;
        const QFileInfoList dirs = childDirs(root);
        static const QRegularExpression build(QStringLiteral("^(.*)-(\\d+)$"));
        for (const QFileInfo &entry : dirs) {
            const QRegularExpressionMatch match = build.match(entry.fileName());
            if (!match.hasMatch())
                continue;
            const QString family = match.captured(1);
            if (!newest.contains(family) ||
                build.match(newest.value(family).fileName()).captured(2).toInt() < match.captured(2).toInt())
                newest.insert(family, entry);
        }
        for (const QFileInfo &entry : dirs) {
            const QRegularExpressionMatch match = build.match(entry.fileName());
            if (!match.hasMatch())
                continue;
            Item item = makeItem(Category::BuildTemp, Kind::Files,
                                 QStringLiteral("Playwright %1").arg(entry.fileName()), entry.filePath(), root);
            addMember(item, entry.filePath(), measure(entry.filePath(), m_cancel));
            if (newest.value(match.captured(1)).filePath() == entry.filePath()) {
                item.status = Status::Attached;
                item.reasons << QStringLiteral("newest %1 build; browser tests use it").arg(match.captured(1));
            } else {
                item.reasons << QStringLiteral("older %1 build").arg(match.captured(1));
            }
            push(item);
        }
    }

    void scanCcache()
    {
        QString active = m_context.ccacheDir;
        if (active.isEmpty() && m_context.runCcache) {
            QProcess process;
            process.start(QStringLiteral("ccache"), {QStringLiteral("-k"), QStringLiteral("cache_dir")});
            if (process.waitForStarted(2000) && process.waitForFinished(5000) &&
                process.exitCode() == 0)
                active = QString::fromUtf8(process.readAllStandardOutput()).trimmed();
            else
                process.kill();
        }
        QStringList candidates;
        if (!active.isEmpty())
            candidates << clean(active);
        for (const QString &path : {m_context.home + QStringLiteral("/.ccache"),
                                    m_context.home + QStringLiteral("/.cache/ccache")}) {
            if (!candidates.contains(clean(path)))
                candidates << clean(path);
        }
        for (const QString &path : std::as_const(candidates)) {
            const QFileInfo info(path);
            if (!info.isDir() || info.isSymLink())
                continue;
            const bool isActive = !active.isEmpty() && samePath(path, active);
            Item item = makeItem(Category::BuildTemp, isActive ? Kind::Report : Kind::Files,
                                 QStringLiteral("ccache · %1").arg(path), path, info.absolutePath());
            const Usage usage = measure(path, m_cancel);
            if (isActive) {
                item.keptBytes = usage.bytes;
                item.files = usage.files;
                item.modifiedMs = usage.newestMs;
                item.status = Status::Protected;
                item.reasons << QStringLiteral("the active compiler cache; report only, use Trim ccache");
            } else {
                addMember(item, path, usage);
                item.status = Status::Attached;
                item.confirmEach = true;
                item.reasons << QStringLiteral("not the active ccache folder");
            }
            push(item, true);
        }
    }

    void scanOtherCaches()
    {
        const QString cache = m_context.home + QStringLiteral("/.cache");
        static const QRegularExpression pattern(QStringLiteral("^(fm-.*|fm\\d.*|forkmesh-.*)$"));
        for (const QFileInfo &entry : childEntries(cache)) {
            if (cancelled(m_cancel))
                return;
            if (entry.isSymLink() || !pattern.match(entry.fileName()).hasMatch())
                continue;
            Item item = makeItem(Category::OtherCaches, Kind::Files, entry.fileName(), entry.filePath(),
                                 cache);
            const Usage usage = measure(entry.filePath(), m_cancel);
            if (entry.fileName().startsWith(QLatin1String("forkmesh-release-")) ||
                QFileInfo(entry.filePath() + QStringLiteral("/.git")).exists()) {
                item.keptBytes = usage.bytes;
                item.files = usage.files;
                item.modifiedMs = usage.newestMs;
                item.status = Status::Protected;
                item.reasons << (entry.fileName().startsWith(QLatin1String("forkmesh-release-"))
                                     ? QStringLiteral("release build folder; a release timer uses it")
                                     : QStringLiteral("a git checkout or worktree"));
                push(item, true);
                continue;
            }
            addMember(item, entry.filePath(), usage);
            item.confirmEach = true;
            item.reasons << QStringLiteral("operator cache from an agent run; check before deleting");
            if (recent(usage.newestMs)) {
                item.status = Status::Attached;
                item.reasons << recentText();
            }
            push(item);
        }
    }

    const Context &m_context;
    CancelFlag m_cancel;
    Progress m_progress;
    ScanResult m_result;
    QSet<QString> m_live;
    QHash<QString, RepoGit> m_repos;
    QStringList m_worktreeNames;
    bool m_claudeRunning = false;
    bool m_codexRunning = false;
};

#if defined(Q_OS_UNIX)
void walk(int fd, Usage &usage, QSet<QPair<quint64, quint64>> &seen, const CancelFlag &cancel,
          int depth)
{
    DIR *dir = ::fdopendir(fd);
    if (!dir) {
        ::close(fd);
        return;
    }
    const int dirFd = ::dirfd(dir);
    while (dirent *entry = ::readdir(dir)) {
        if (cancelled(cancel))
            break;
        const char *name = entry->d_name;
        if (name[0] == '.' && (name[1] == 0 || (name[1] == '.' && name[2] == 0)))
            continue;
        struct stat st;
        if (::fstatat(dirFd, name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            continue;
        const bool isDir = S_ISDIR(st.st_mode);
        if (!isDir && st.st_nlink > 1) {
            const QPair<quint64, quint64> key{quint64(st.st_dev), quint64(st.st_ino)};
            if (seen.contains(key))
                continue;
            seen.insert(key);
        }
        usage.bytes += qint64(st.st_blocks) * 512;
        if (!isDir)
            ++usage.files;
        usage.newestMs = std::max<qint64>(usage.newestMs, qint64(st.st_mtime) * 1000);
        if (isDir && depth < 200) {
            const int child = ::openat(dirFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child >= 0)
                walk(child, usage, seen, cancel, depth + 1);
        }
    }
    ::closedir(dir);
}
#endif

} // namespace

QString categoryName(Category category)
{
    switch (category) {
    case Category::Claude:
        return QStringLiteral("Claude Code");
    case Category::Codex:
        return QStringLiteral("Codex");
    case Category::Worktrees:
        return QStringLiteral("Agent worktrees");
    case Category::BuildTemp:
        return QStringLiteral("Builds and temp files");
    case Category::OtherCaches:
        return QStringLiteral("Other caches");
    }
    return QString();
}

QString statusName(Status status)
{
    switch (status) {
    case Status::InUse:
        return QStringLiteral("In use");
    case Status::Attached:
        return QStringLiteral("Attached");
    case Status::Orphaned:
        return QStringLiteral("Orphaned");
    case Status::Protected:
        return QStringLiteral("Protected");
    }
    return QString();
}

QList<Category> categories()
{
    return {Category::Claude, Category::Codex, Category::Worktrees, Category::BuildTemp,
            Category::OtherCaches};
}

QString Item::reasonText() const
{
    return reasons.join(QStringLiteral(" · "));
}

bool Item::deletable() const
{
    if (status != Status::Orphaned && status != Status::Attached)
        return false;
    if (kind == Kind::Report)
        return false;
    return kind == Kind::Worktree || !members.isEmpty();
}

bool Item::needsConfirm() const
{
    return status == Status::Attached || confirmEach;
}

QString Process::label() const
{
    QString name = comm;
    if (name.isEmpty())
        name = QFileInfo(exe).fileName();
    if (name.isEmpty() && !cmdline.isEmpty())
        name = QFileInfo(cmdline.first()).fileName();
    return QStringLiteral("%1 (PID %2)").arg(name.isEmpty() ? QStringLiteral("process") : name).arg(pid);
}

QStringList ProcessSnapshot::usersOf(const QString &path) const
{
    QStringList users;
    const QString target = clean(path);
    const QString resolved = canonical(path);
    const auto hit = [&](const QString &candidate) {
        return !candidate.isEmpty() &&
               (pathWithin(candidate, target) || pathWithin(candidate, resolved));
    };
    for (const Process &process : processes) {
        bool uses = hit(process.cwd) || hit(process.exe);
        for (qsizetype i = 0; !uses && i < process.openPaths.size(); ++i)
            uses = hit(process.openPaths.at(i));
        if (uses)
            users << process.label();
    }
    return users;
}

QList<Process> ProcessSnapshot::named(const QStringList &names) const
{
    if (names.isEmpty())
        return processes;
    QList<Process> found;
    for (const Process &process : processes) {
        if (names.contains(process.comm) || names.contains(QFileInfo(process.exe).fileName()))
            found << process;
    }
    return found;
}

ProcessSnapshot readProcesses(const QString &procRoot)
{
    ProcessSnapshot snapshot;
    const QDir proc(procRoot);
    if (!proc.exists())
        return snapshot;
    for (const QString &name : proc.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool numeric = false;
        const qint64 pid = name.toLongLong(&numeric);
        if (!numeric)
            continue;
        const QString base = proc.filePath(name);
        Process process;
        process.pid = pid;
        process.cwd = readLink(base + QStringLiteral("/cwd"));
        process.exe = readLink(base + QStringLiteral("/exe"));
        process.comm = QString::fromUtf8(readHead(base + QStringLiteral("/comm"), 256)).trimmed();
        const QByteArray cmdline = readHead(base + QStringLiteral("/cmdline"), 64 * 1024);
        for (const QByteArray &part : cmdline.split('\0')) {
            if (!part.isEmpty())
                process.cmdline << QString::fromLocal8Bit(part);
        }
        const QDir fds(base + QStringLiteral("/fd"));
        for (const QString &fd : fds.entryList(QDir::AllEntries | QDir::System | QDir::NoDotAndDotDot)) {
            const QString target = readLink(fds.filePath(fd));
            if (target.startsWith(QLatin1Char('/')))
                process.openPaths << target;
        }
        if (process.cwd.isEmpty() && process.exe.isEmpty() && process.openPaths.isEmpty() &&
            process.cmdline.isEmpty())
            continue;
        snapshot.processes << process;
    }
    snapshot.available = !snapshot.processes.isEmpty();
    return snapshot;
}

bool isClaudeProcess(const Process &process)
{
    if (process.exe.contains(QLatin1String("/claude/versions/")) || process.comm == QLatin1String("claude") ||
        QFileInfo(process.exe).fileName() == QLatin1String("claude"))
        return true;
    return !process.cmdline.isEmpty() &&
           QFileInfo(process.cmdline.first()).fileName() == QLatin1String("claude");
}

bool isCodexProcess(const Process &process)
{
    if (process.comm == QLatin1String("codex") || QFileInfo(process.exe).fileName() == QLatin1String("codex"))
        return true;
    for (qsizetype i = 0; i < std::min<qsizetype>(2, process.cmdline.size()); ++i) {
        const QString name = QFileInfo(process.cmdline.at(i)).fileName();
        if (name == QLatin1String("codex") || name == QLatin1String("codex.js"))
            return true;
    }
    return false;
}

Usage measure(const QString &path, const CancelFlag &cancel)
{
    Usage usage;
#if defined(Q_OS_UNIX)
    struct stat st;
    const QByteArray native = QFile::encodeName(path);
    if (::lstat(native.constData(), &st) != 0)
        return usage;
    usage.bytes = qint64(st.st_blocks) * 512;
    usage.newestMs = qint64(st.st_mtime) * 1000;
    if (!S_ISDIR(st.st_mode)) {
        usage.files = 1;
        return usage;
    }
    const int fd = ::open(native.constData(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0)
        return usage;
    QSet<QPair<quint64, quint64>> seen;
    walk(fd, usage, seen, cancel, 0);
#else
    const QFileInfo root(path);
    if (root.isSymLink() || root.isFile()) {
        usage.bytes = root.size();
        usage.files = 1;
        usage.newestMs = root.lastModified().toMSecsSinceEpoch();
        return usage;
    }
    usage.newestMs = root.lastModified().toMSecsSinceEpoch();
    QDirIterator it(path, QDir::AllEntries | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot,
                    QDirIterator::Subdirectories);
    while (it.hasNext() && !cancelled(cancel)) {
        it.next();
        const QFileInfo info = it.fileInfo();
        if (!info.isDir()) {
            usage.bytes += info.size();
            ++usage.files;
        }
        usage.newestMs = std::max(usage.newestMs, info.lastModified().toMSecsSinceEpoch());
    }
#endif
    return usage;
}

QString encodeClaudeProject(const QString &cwd)
{
    QString encoded = cwd;
    for (QChar &c : encoded) {
        const ushort u = c.unicode();
        const bool keep = (u >= '0' && u <= '9') || (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z');
        if (!keep)
            c = QLatin1Char('-');
    }
    return encoded;
}

QSet<QString> uuidsIn(const QString &text)
{
    static const QRegularExpression uuid(
        QStringLiteral("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}"));
    QSet<QString> found;
    auto it = uuid.globalMatch(text);
    while (it.hasNext())
        found.insert(it.next().captured(0));
    return found;
}

QSet<QString> liveClaudeSessions(const Context &context)
{
    QSet<QString> live;
    QHash<QString, int> cwds;
    for (const Process &process : context.processes.processes) {
        if (!isClaudeProcess(process))
            continue;
        live.unite(uuidsIn(process.cmdline.join(QLatin1Char(' '))));
        if (!process.cwd.isEmpty())
            cwds[process.cwd] += 1;
    }
    for (const QString &root : claudeProjectRoots(context)) {
        for (const QFileInfo &project : childDirs(root)) {
            const QFileInfoList transcripts = QDir(project.filePath())
                                                  .entryInfoList({QStringLiteral("*.jsonl")},
                                                                 QDir::Files, QDir::Time);
            for (const QFileInfo &transcript : transcripts) {
                if (context.nowMs - transcript.lastModified().toMSecsSinceEpoch() <
                    context.liveMinutes * kMinuteMs)
                    live.insert(transcript.completeBaseName());
            }
            for (auto it = cwds.cbegin(); it != cwds.cend(); ++it) {
                if (!projectMatchesCwd(project.fileName(), it.key()))
                    continue;
                for (qsizetype i = 0; i < std::min<qsizetype>(it.value(), transcripts.size()); ++i)
                    live.insert(transcripts.at(i).completeBaseName());
            }
        }
    }
    return live;
}

bool pathWithin(const QString &path, const QString &root)
{
    if (path.isEmpty() || root.isEmpty())
        return false;
    if (path == root)
        return true;
    const QString prefix = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return path.startsWith(prefix);
}

QString sizeText(qint64 bytes)
{
    return QLocale().formattedDataSize(std::max<qint64>(0, bytes), 1);
}

QString ageText(qint64 modifiedMs, qint64 nowMs)
{
    if (modifiedMs <= 0)
        return QStringLiteral("-");
    const qint64 minutes = std::max<qint64>(0, (nowMs - modifiedMs) / kMinuteMs);
    if (minutes < 60)
        return QStringLiteral("%1 min").arg(minutes);
    if (minutes < 48 * 60)
        return QStringLiteral("%1 h").arg(minutes / 60);
    return QStringLiteral("%1 d").arg(minutes / (24 * 60));
}

ScanResult scan(const Context &context, const CancelFlag &cancel, const Progress &progress)
{
    return Scanner(context, cancel, progress).run();
}

} // namespace forkmesh::cleanup
