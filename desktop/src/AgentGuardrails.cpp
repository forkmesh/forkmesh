#include "AgentGuardrails.h"
#include "AppPaths.h"

#include "AgentGuardScript.h"
#include "AgentResources.h"
#include "VirtualMachineRuntime.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QRegularExpression>
#include <QStandardPaths>

#include <algorithm>
#include <utility>

namespace forkmesh::agentguard {
namespace {

QString cleanRelativeDirectory(const QString &raw)
{
    QString path = QDir::fromNativeSeparators(raw.trimmed());
    if (path.isEmpty() || path.contains(QChar::Null) ||
        QDir::isAbsolutePath(path))
        return {};
    path = QDir::cleanPath(path);
    if (path == QLatin1String("..") ||
        path.startsWith(QLatin1String("../")))
        return {};
    while (path.startsWith(QLatin1String("./")))
        path.remove(0, 2);
    return path.isEmpty() ? QStringLiteral(".") : path;
}

bool within(const QString &candidate, const QString &root)
{
    if (candidate.isEmpty() || root.isEmpty())
        return false;
    return candidate == root ||
           candidate.startsWith(root + QDir::separator());
}

QString resolvedExisting(const QString &path)
{
    const QFileInfo info(path);
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                                : canonical);
}

void appendExisting(QStringList &paths, const QString &candidate)
{
    if (candidate.trimmed().isEmpty() || !QFileInfo::exists(candidate))
        return;
    const QString resolved = resolvedExisting(candidate);
    if (!resolved.isEmpty() && !paths.contains(resolved))
        paths.append(resolved);
}

QString markerGitDirectory(const QFileInfo &marker)
{
    QFile file(marker.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QString line = QString::fromUtf8(file.readLine()).trimmed();
    if (!line.startsWith(QLatin1String("gitdir:"), Qt::CaseInsensitive))
        return {};
    QString gitDir = line.mid(7).trimmed();
    if (QDir::isRelativePath(gitDir))
        gitDir = QDir(marker.absolutePath()).absoluteFilePath(gitDir);
    const QFileInfo info(gitDir);
    return info.isDir() ? info.canonicalFilePath() : QString();
}

QString linkedCommonDirectory(const QString &gitDirectory)
{
    const QFileInfo gitInfo(gitDirectory);
    const QDir worktrees(gitInfo.absolutePath());
    if (QFileInfo(worktrees.absolutePath()).fileName() !=
        QLatin1String("worktrees"))
        return {};
    const QFileInfo common(worktrees.filePath(QStringLiteral("..")));
    return common.isDir() ? common.canonicalFilePath() : QString();
}

bool linkedMetadataValid(const QFileInfo &marker, const QString &gitDirectory,
                         const QString &commonDirectory)
{
    if (gitDirectory.isEmpty() || commonDirectory.isEmpty() ||
        !within(gitDirectory, commonDirectory))
        return false;

    QFile commonFile(
        QDir(gitDirectory).filePath(QStringLiteral("commondir")));
    if (!commonFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QString declaredCommon =
        QString::fromUtf8(commonFile.readLine()).trimmed();
    if (QDir::isRelativePath(declaredCommon))
        declaredCommon =
            QDir(gitDirectory).absoluteFilePath(declaredCommon);
    if (resolvedExisting(declaredCommon) != commonDirectory)
        return false;

    QFile backlink(QDir(gitDirectory).filePath(QStringLiteral("gitdir")));
    if (!backlink.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    QString declaredMarker = QString::fromUtf8(backlink.readLine()).trimmed();
    if (QDir::isRelativePath(declaredMarker))
        declaredMarker = QDir(gitDirectory).absoluteFilePath(declaredMarker);
    return resolvedExisting(declaredMarker) ==
           resolvedExisting(marker.absoluteFilePath());
}

QString trustedCommonDirectory(const QString &repositoryRoot)
{
    const QFileInfo root(repositoryRoot);
    if (!root.isDir())
        return {};
    const QString resolvedRoot = root.canonicalFilePath();
    const QFileInfo marker(
        QDir(resolvedRoot).filePath(QStringLiteral(".git")));
    if (marker.isDir()) {
        const QString metadata = marker.canonicalFilePath();
        return !marker.isSymLink() && within(metadata, resolvedRoot)
                   ? metadata
                   : QString();
    }
    if (marker.isFile()) {
        if (marker.isSymLink())
            return {};
        const QString gitDirectory = markerGitDirectory(marker);
        const QString commonDirectory =
            linkedCommonDirectory(gitDirectory);
        return linkedMetadataValid(marker, gitDirectory, commonDirectory)
                   ? commonDirectory
                   : QString();
    }
    // RepositoryRecord can point at ForkMesh's bare mirror rather than a
    // checkout. A bare repository is itself the trusted common directory.
    if (QFileInfo(QDir(resolvedRoot).filePath(QStringLiteral("HEAD"))).isFile() &&
        QFileInfo(QDir(resolvedRoot).filePath(QStringLiteral("objects"))).isDir())
        return resolvedRoot;
    return {};
}

void appendMutableGitRoots(const QString &commonDirectory,
                           QStringList &readWrite)
{
    // Keep shared hooks and configuration read-only so an agent cannot plant
    // code for a later, unguarded Git invocation. Commits still need object,
    // reference, reflog, rerere, and LFS storage beneath the common directory.
    for (const QString &name : {QStringLiteral("objects"),
                                QStringLiteral("refs"),
                                QStringLiteral("logs"),
                                QStringLiteral("rr-cache"),
                                QStringLiteral("lfs")})
        appendExisting(readWrite, QDir(commonDirectory).filePath(name));
}

bool appendGitMetadataRoots(const QString &worktree, const Config &config,
                            QStringList &readOnly, QStringList &readWrite,
                            QString *error)
{
    const QFileInfo marker(QDir(worktree).filePath(QStringLiteral(".git")));
    if (!marker.exists())
        return true;
    if (marker.isDir()) {
        const QString metadata = marker.canonicalFilePath();
        if (marker.isSymLink() || !within(metadata, worktree)) {
            *error = QStringLiteral(
                "The guarded worktree's Git directory escapes the worktree.");
            return false;
        }
        appendExisting(readOnly, metadata);
        appendMutableGitRoots(metadata, readWrite);
        return true;
    }
    if (!marker.isFile()) {
        *error = QStringLiteral("The guarded worktree has invalid Git metadata.");
        return false;
    }
    if (marker.isSymLink()) {
        *error = QStringLiteral(
            "The guarded worktree's Git marker cannot be a symbolic link.");
        return false;
    }

    appendExisting(readOnly, marker.absoluteFilePath());
    const QString gitDirectory = markerGitDirectory(marker);
    const QString linkedCommon = linkedCommonDirectory(gitDirectory);
    const QString trustedCommon =
        trustedCommonDirectory(config.repositoryRoot);
    if (trustedCommon.isEmpty() || linkedCommon != trustedCommon ||
        !linkedMetadataValid(marker, gitDirectory, linkedCommon)) {
        *error = QStringLiteral(
            "The guarded worktree's Git metadata is outside its trusted "
            "repository.");
        return false;
    }
    appendExisting(readWrite, gitDirectory);
    appendExisting(readOnly, trustedCommon);
    appendMutableGitRoots(trustedCommon, readWrite);
    return true;
}

QString guardScriptPath(QString *error)
{
    const QString directory =
        forkmesh::vm::active()
            ? QDir(forkmesh::vm::worktreeRoot())
                  .filePath(QStringLiteral("runtime"))
            : QDir(forkmesh::paths::dataRoot())
                  .filePath(QStringLiteral("agents"));
    if (!QDir().mkpath(directory)) {
        if (error)
            *error = QStringLiteral("Could not create the agent guardrail runtime.");
        return {};
    }
    const QString path =
        QDir(directory).filePath(QStringLiteral("forkmesh_agent_guard.py"));
    const QByteArray wanted = forkmeshAgentGuardScript().toUtf8();
    QFile file(path);
    bool write = true;
    if (file.open(QIODevice::ReadOnly)) {
        write = file.readAll() != wanted;
        file.close();
    }
    if (write) {
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) ||
            file.write(wanted) != wanted.size()) {
            if (error)
                *error = QStringLiteral("Could not stage the agent guardrail helper.");
            return {};
        }
        file.close();
        QFile::setPermissions(path, QFileDevice::ReadOwner |
                                        QFileDevice::WriteOwner);
    }
    return path;
}

void appendRuntimeRoots(QStringList &readOnly, const Config &config,
                        const QProcessEnvironment &environment)
{
    // Runtime code and machine-wide configuration are readable, never
    // writable. User data is intentionally absent from this list.
    for (const QString &path : {QStringLiteral("/usr"),
                                QStringLiteral("/bin"),
                                QStringLiteral("/sbin"),
                                QStringLiteral("/lib"),
                                QStringLiteral("/lib64"),
                                QStringLiteral("/etc")})
        appendExisting(readOnly, path);
    for (const QString &path : {QStringLiteral("/proc/cpuinfo"),
                                QStringLiteral("/proc/meminfo"),
                                QStringLiteral("/proc/stat"),
                                QStringLiteral("/sys/devices/system/cpu")})
        appendExisting(readOnly, path);

    // Host user-installed executables are not necessarily present at the same
    // path in the guest. The system roots above are shared by convention and
    // contain the guest's shell, Python, Git, and provider launchers.
    if (forkmesh::vm::active())
        return;

    appendExisting(readOnly, forkmesh::agentresources::shimDirectory());

    QStringList executables{QStringLiteral("sh"), QStringLiteral("bash"),
                            QStringLiteral("git"), QStringLiteral("python3")};
    if (config.provider.contains(QLatin1String("codex"), Qt::CaseInsensitive))
        executables.append(QStringLiteral("codex"));
    if (config.provider.compare(QLatin1String("claude-code"),
                                Qt::CaseInsensitive) == 0)
        executables.append(QStringLiteral("claude"));
    for (const QString &name : std::as_const(executables))
        appendExisting(readOnly,
                       QStandardPaths::findExecutable(
                           name, environment.value(QStringLiteral("PATH"))
                                     .split(QDir::listSeparator(),
                                            Qt::SkipEmptyParts)));
}

QString absoluteToolPath(const QString &worktree, const QString &raw)
{
    QString path = raw.trimmed();
    if (path.isEmpty())
        return {};
    path = QDir::fromNativeSeparators(path);
    while (!path.isEmpty() && QStringLiteral(",;:)]}").contains(path.back()))
        path.chop(1);
    if (QDir::isRelativePath(path))
        path = QDir(worktree).absoluteFilePath(path);
    QString probe = QDir::cleanPath(path);
    QStringList missingTail;
    QFileInfo info(probe);
    while (!info.exists()) {
        const QString name = info.fileName();
        const QString parent = info.absolutePath();
        if (name.isEmpty() || parent == probe)
            break;
        missingTail.prepend(name);
        probe = parent;
        info.setFile(probe);
    }
    QString resolved = info.exists() ? info.canonicalFilePath()
                                     : info.absoluteFilePath();
    for (const QString &component : std::as_const(missingTail))
        resolved = QDir(resolved).filePath(component);
    return QDir::cleanPath(resolved);
}

QStringList pathsInToolInput(const QJsonObject &input)
{
    QStringList paths;
    const QStringList keys{QStringLiteral("file_path"),
                           QStringLiteral("path"),
                           QStringLiteral("cwd"),
                           QStringLiteral("notebook_path"),
                           QStringLiteral("directory"),
                           QStringLiteral("old_file_path"),
                           QStringLiteral("new_file_path")};
    for (const QString &key : keys) {
        const QJsonValue value = input.value(key);
        if (value.isString() && !value.toString().trimmed().isEmpty())
            paths.append(value.toString());
    }
    for (const QJsonValue &value : input.value(QStringLiteral("changes")).toArray()) {
        const QString path = value.toObject().value(QStringLiteral("path")).toString();
        if (!path.trimmed().isEmpty())
            paths.append(path);
    }
    for (const QJsonValue &value : input.value(QStringLiteral("paths")).toArray())
        if (value.isString() && !value.toString().trimmed().isEmpty())
            paths.append(value.toString());

    // Shell syntax cannot be authorized by parsing it. Landlock remains the
    // enforcement point; this scan only makes obvious absolute-path denials
    // visible in the transcript before the command reports EACCES.
    const QString command = input.value(QStringLiteral("command")).toString();
    static const QRegularExpression absolutePath(
        QStringLiteral(R"((?:^|[\s'\"])((?:/|[A-Za-z]:[\\/])[^\s'\"]+))"));
    QRegularExpressionMatchIterator matches = absolutePath.globalMatch(command);
    while (matches.hasNext())
        paths.append(matches.next().captured(1));
    return paths;
}

} // namespace

QStringList normalizeDirectories(const QStringList &entries,
                                 QStringList *rejected)
{
    if (rejected)
        rejected->clear();
    QStringList normalized;
    for (const QString &entry : entries) {
        const QString path = cleanRelativeDirectory(entry);
        if (path.isEmpty()) {
            if (rejected && !entry.trimmed().isEmpty())
                rejected->append(entry);
            continue;
        }
        if (!normalized.contains(path))
            normalized.append(path);
        if (normalized.size() == 64)
            break;
    }
    std::sort(normalized.begin(), normalized.end(), [](const QString &a,
                                                        const QString &b) {
        if (a == b)
            return false;
        if (a == QLatin1String("."))
            return true;
        if (b == QLatin1String("."))
            return false;
        return a < b;
    });
    if (normalized.contains(QStringLiteral(".")))
        return {QStringLiteral(".")};

    QStringList compact;
    for (const QString &path : std::as_const(normalized)) {
        bool covered = false;
        for (const QString &parent : std::as_const(compact)) {
            if (path.startsWith(parent + QLatin1Char('/'))) {
                covered = true;
                break;
            }
        }
        if (!covered)
            compact.append(path);
    }
    return compact;
}

bool pathAllowed(const QString &worktree, const QStringList &allowedDirectories,
                 const QString &path)
{
    const QString root = resolvedExisting(worktree);
    const QString candidate = absoluteToolPath(root, path);
    if (!within(candidate, root))
        return false;
    const QStringList allowed = normalizeDirectories(allowedDirectories);
    for (const QString &relative : allowed) {
        const QString allowedPath =
            relative == QLatin1String(".")
                ? root
                : absoluteToolPath(root, QDir(root).filePath(relative));
        if (within(candidate, allowedPath))
            return true;
    }
    return false;
}

QString approvalDirectory(const QString &worktree, const QString &path)
{
    const QString root = resolvedExisting(worktree);
    const QString candidate = absoluteToolPath(root, path);
    if (!within(candidate, root))
        return {};

    // Landlock rules can only name paths that exist when the provider starts.
    // For a file (including one the tool intends to create), approve its
    // narrowest existing parent. This never follows a symlink escape because
    // absoluteToolPath() canonicalizes the longest existing prefix first.
    QFileInfo info(candidate);
    QString directory = info.isDir() ? candidate : info.absolutePath();
    while (!QFileInfo(directory).isDir()) {
        const QString parent = QFileInfo(directory).absolutePath();
        if (parent == directory)
            return {};
        directory = parent;
    }
    const QString resolved = resolvedExisting(directory);
    if (!within(resolved, root))
        return {};
    if (resolved == root)
        return QStringLiteral(".");
    return QDir::fromNativeSeparators(QDir(root).relativeFilePath(resolved));
}

QString toolViolation(const QString &toolName, const QJsonObject &input,
                      const QString &worktree,
                      const QStringList &allowedDirectories)
{
    Q_UNUSED(toolName);
    for (const QString &path : pathsInToolInput(input))
        if (!pathAllowed(worktree, allowedDirectories, path))
            return path;
    return {};
}

QString policyDescription(const QStringList &allowedDirectories)
{
    const QStringList normalized = normalizeDirectories(allowedDirectories);
    if (normalized.isEmpty())
        return QStringLiteral("no repository directories");
    if (normalized == QStringList{QStringLiteral(".")})
        return QStringLiteral("the whole repository");
    return normalized.join(QStringLiteral(", "));
}

namespace {

struct KnownDirectory {
    const char *name;
    const char *description;
    bool recommended;
};

// Conventional top-level folder names. Recommended ones are what an agent
// usually needs to change code; the rest are labeled so an owner can see at a
// glance why they are better left unchecked.
constexpr KnownDirectory kKnownDirectories[] = {
    {"src", "Source code", true},
    {"source", "Source code", true},
    {"sources", "Source code", true},
    {"lib", "Library code", true},
    {"libs", "Library code", true},
    {"app", "Application code", true},
    {"apps", "Applications", true},
    {"packages", "Workspace packages", true},
    {"pkg", "Packages", true},
    {"cmd", "Command entry points", true},
    {"internal", "Internal packages", true},
    {"include", "Public headers", true},
    {"crates", "Rust crates", true},
    {"components", "UI components", true},
    {"test", "Tests", true},
    {"tests", "Tests", true},
    {"spec", "Tests", true},
    {"specs", "Tests", true},
    {"__tests__", "Tests", true},
    {"e2e", "End-to-end tests", true},
    {"docs", "Documentation", true},
    {"doc", "Documentation", true},
    {"examples", "Examples", true},
    {"scripts", "Scripts", true},
    {"tools", "Developer tools", true},
    {"assets", "Static assets", false},
    {"public", "Static web assets", false},
    {"static", "Static web assets", false},
    {".github", "CI workflows and repository automation", false},
    {".forkmesh", "ForkMesh issues, pulls, and workflows", false},
    {".vscode", "Editor settings", false},
    {".idea", "Editor settings", false},
    {"node_modules", "Installed dependencies", false},
    {"vendor", "Vendored dependencies", false},
    {"third_party", "Third-party code", false},
    {"build", "Build output", false},
    {"dist", "Build output", false},
    {"out", "Build output", false},
    {"target", "Build output", false},
    {".venv", "Python environment", false},
    {"venv", "Python environment", false},
    {"secrets", "Secrets (keep blocked)", false},
    {".secrets", "Secrets (keep blocked)", false},
};

const KnownDirectory *knownDirectory(const QString &path)
{
    const QString name = path.section(QLatin1Char('/'), -1);
    for (const KnownDirectory &known : kKnownDirectories)
        if (name == QLatin1String(known.name))
            return &known;
    return nullptr;
}

bool coveredBy(const QString &path, const QStringList &normalized)
{
    for (const QString &allowed : normalized)
        if (allowed == QLatin1String(".") || path == allowed ||
            path.startsWith(allowed + QLatin1Char('/')))
            return true;
    return false;
}

} // namespace

QList<DirectoryChoice> directoryChoices(const QString &worktree,
                                        const QStringList &allowedDirectories)
{
    const QStringList normalized = normalizeDirectories(allowedDirectories);
    const QString root = worktree.trimmed().isEmpty() ||
                                 !QFileInfo(worktree).isDir()
                             ? QString()
                             : resolvedExisting(worktree);

    QStringList paths;
    if (!root.isEmpty()) {
        const QStringList names = QDir(root).entryList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
            QDir::Name | QDir::IgnoreCase);
        for (const QString &name : names) {
            if (name == QLatin1String(".git"))
                continue;
            // A top-level symlink leading out of the checkout can never be
            // allowed, so it is not offered either.
            if (!pathAllowed(root, {QStringLiteral(".")}, name))
                continue;
            paths.append(name);
            if (paths.size() == 200)
                break;
        }
    }
    for (const QString &saved : normalized)
        if (saved != QLatin1String(".") && !paths.contains(saved))
            paths.append(saved);

    QList<DirectoryChoice> choices;
    DirectoryChoice whole;
    whole.path = QStringLiteral(".");
    whole.description = QStringLiteral(
        "Whole repository, including root files such as README and build files");
    whole.exists = !root.isEmpty();
    whole.allowed = normalized.contains(QStringLiteral("."));
    choices.append(whole);

    QList<DirectoryChoice> rest;
    for (const QString &path : std::as_const(paths)) {
        DirectoryChoice choice;
        choice.path = path;
        if (const KnownDirectory *known = knownDirectory(path)) {
            choice.description = QString::fromLatin1(known->description);
            choice.recommended = known->recommended;
        }
        choice.exists =
            !root.isEmpty() && QFileInfo(QDir(root).filePath(path)).isDir();
        choice.allowed = coveredBy(path, normalized);
        rest.append(choice);
    }
    std::stable_sort(rest.begin(), rest.end(),
                     [](const DirectoryChoice &a, const DirectoryChoice &b) {
                         if (a.recommended != b.recommended)
                             return a.recommended;
                         return a.path.compare(b.path, Qt::CaseInsensitive) < 0;
                     });
    choices.append(rest);
    return choices;
}

QStringList recommendedDirectories(const QString &worktree)
{
    QStringList recommended;
    for (const DirectoryChoice &choice : directoryChoices(worktree, {}))
        if (choice.recommended && choice.exists)
            recommended.append(choice.path);
    return recommended.isEmpty() ? QStringList{QStringLiteral(".")}
                                 : normalizeDirectories(recommended);
}

bool policyNarrowed(const QStringList &before, const QStringList &after)
{
    const QStringList next = normalizeDirectories(after);
    for (const QString &path : normalizeDirectories(before))
        if (!coveredBy(path, next))
            return true;
    return false;
}

QString promptInstruction(const Config &config)
{
    if (!config.enabled && config.projectLocked)
        return QStringLiteral(
                   "The project%1 holds you to its folder, enforced by the "
                   "operating system. You may read and change your worktree "
                   "and the project folder; anything else of the user's is "
                   "blocked, apart from read-only system runtime files, "
                   "installed tools, provider login state, Git metadata, "
                   "private scratch and any folder the user granted in the "
                   "project's settings. Do not try to bypass this boundary; "
                   "if you need another folder, say so and stop.")
            .arg(config.projectName.isEmpty()
                     ? QString()
                     : QStringLiteral(" \xE2\x80\x9C%1\xE2\x80\x9D")
                           .arg(config.projectName));
    if (!config.enabled)
        return {};
    return QStringLiteral(
               "Repository guardrails are enforced by the operating system. "
               "You may read or change only these repository directories: %1. "
               "Tool calls and subprocesses that access any other repository "
               "path or unrelated host data will be blocked. Read-only system "
               "runtime files, provider login state, Git metadata, and private "
               "scratch are internal exceptions. Do not try to bypass this "
               "boundary.")
        .arg(policyDescription(config.allowedDirectories));
}

LaunchCommand restrictedCommand(
    const QString &program, const QStringList &arguments,
    const QString &worktree, const Config &config,
    const QProcessEnvironment &environment,
    const QStringList &additionalReadOnly)
{
    LaunchCommand launch{program, arguments, {}, false};
    if (!config.enforced())
        return launch;

    // A project lock without a repository policy covers the whole worktree.
    const QStringList normalized =
        config.enabled ? normalizeDirectories(config.allowedDirectories)
                       : QStringList{QStringLiteral(".")};
    if (normalized.isEmpty()) {
        launch.error = QStringLiteral(
            "Repository agent guardrails are enabled, but no allowed "
            "directories are configured.");
        return launch;
    }
    const QString root = resolvedExisting(worktree);
    if (root.isEmpty() || !QFileInfo(root).isDir()) {
        launch.error = QStringLiteral("The guarded agent worktree is unavailable.");
        return launch;
    }

    QStringList readOnly;
    QStringList readWrite;
    for (const QString &relative : normalized) {
        const QString candidate =
            relative == QLatin1String(".")
                ? root
                : QDir(root).filePath(relative);
        const QFileInfo info(candidate);
        const QString resolved = info.canonicalFilePath();
        if (!info.isDir() || resolved.isEmpty() || !within(resolved, root)) {
            launch.error = QStringLiteral(
                               "Allowed agent directory '%1' is missing or "
                               "escapes the repository.")
                               .arg(relative);
            return launch;
        }
        appendExisting(readWrite, resolved);
    }
    if (!appendGitMetadataRoots(root, config, readOnly, readWrite,
                                &launch.error))
        return launch;

    const QStringList scratchKeys{QStringLiteral("FORKMESH_AGENT_SCRATCH"),
                                  QStringLiteral("TMPDIR"),
                                  QStringLiteral("TMP"),
                                  QStringLiteral("TEMP"),
                                  QStringLiteral("XDG_CACHE_HOME")};
    for (const QString &key : scratchKeys)
        appendExisting(readWrite, environment.value(key));
    for (const QString &device : {QStringLiteral("/dev/null"),
                                  QStringLiteral("/dev/zero"),
                                  QStringLiteral("/dev/random"),
                                  QStringLiteral("/dev/urandom"),
                                  QStringLiteral("/dev/tty")})
        appendExisting(readWrite, device);
    appendRuntimeRoots(readOnly, config, environment);
    for (const QString &path : additionalReadOnly)
        appendExisting(readOnly, path);
    if (config.projectLocked) {
        for (const QString &path : config.projectReadWrite)
            appendExisting(readWrite, path);
        for (const QString &path : config.projectReadOnly)
            if (!readWrite.contains(resolvedExisting(path)))
                appendExisting(readOnly, path);
    }

    readOnly.removeDuplicates();
    readWrite.removeDuplicates();
    QJsonObject policy{
        {QStringLiteral("version"), 1},
        {QStringLiteral("provider"), config.provider.toLower()},
        {QStringLiteral("readOnly"), QJsonArray::fromStringList(readOnly)},
        {QStringLiteral("readWrite"), QJsonArray::fromStringList(readWrite)}};
    const QByteArray encoded =
        QJsonDocument(policy)
            .toJson(QJsonDocument::Compact)
            .toBase64(QByteArray::Base64UrlEncoding |
                      QByteArray::OmitTrailingEquals);

    QString stagingError;
    const QString script = guardScriptPath(&stagingError);
    if (script.isEmpty()) {
        launch.error = stagingError;
        return launch;
    }
    // Every launcher asks for a login shell (`bash -lc 'exec claude …'`). Under
    // the lock that still runs /etc/profile, which on Debian resets PATH to
    // the system default, while ~/.profile -- the file that puts ~/.local/bin,
    // home of the claude launcher, back on PATH -- sits outside the policy
    // and cannot be read. Every locked launch then exited 127 before a
    // session started. A plain -c shell keeps the environment it is given;
    // the launchers widen that PATH to the user install directories first
    // (agentcli::widenPath), since the desktop's own PATH lacks them when it
    // runs as a systemd user service.
    QStringList shellArguments = arguments;
    const QString shell = QFileInfo(program).fileName();
    if ((shell == QLatin1String("bash") || shell == QLatin1String("sh") ||
         shell == QLatin1String("dash") || shell == QLatin1String("zsh")) &&
        !shellArguments.isEmpty() &&
        shellArguments.first() == QLatin1String("-lc"))
        shellArguments.first() = QStringLiteral("-c");
    launch.program = QStringLiteral("python3");
    launch.arguments = {script, QStringLiteral("--policy"),
                        QString::fromLatin1(encoded), QStringLiteral("--"),
                        program};
    launch.arguments += shellArguments;
    launch.restricted = true;
    return launch;
}

} // namespace forkmesh::agentguard
