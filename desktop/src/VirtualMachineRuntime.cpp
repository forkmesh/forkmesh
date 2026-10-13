#include "VirtualMachineRuntime.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

#include <utility>

namespace forkmesh::vm {
namespace {

struct RuntimeState {
    bool enabled = false;
    QString workspace;
    QString worktrees;
    QString name;
    QString limactl;
    QStringList mounts;
    QString boundaryError;
    bool limaVersionChecked = false;
    QString limaVersionError;
};

RuntimeState &state()
{
    static RuntimeState value;
    return value;
}

QString resolvedDirectory(const QString &path)
{
    if (path.trimmed().isEmpty())
        return {};
    const QFileInfo info(QDir::cleanPath(QDir(path).absolutePath()));
    const QString canonical = info.canonicalFilePath();
    return QDir::cleanPath(canonical.isEmpty() ? info.absoluteFilePath()
                                                : canonical);
}

bool isWithin(const QString &candidate, const QString &root)
{
    if (candidate.isEmpty() || root.isEmpty())
        return false;
    if (candidate == root)
        return true;
    return candidate.startsWith(root + QDir::separator());
}

void appendMount(QStringList &mounts, const QString &path)
{
    const QString resolved = resolvedDirectory(path);
    if (resolved.isEmpty())
        return;
    for (const QString &existing : std::as_const(mounts)) {
        if (isWithin(resolved, existing))
            return;
    }
    for (int i = mounts.size() - 1; i >= 0; --i) {
        if (isWithin(mounts.at(i), resolved))
            mounts.removeAt(i);
    }
    mounts.append(resolved);
}

QString linkedGitCommonDirectory(const QString &workspace)
{
    const QFileInfo marker(QDir(workspace).filePath(QStringLiteral(".git")));
    if (!marker.isFile())
        return {};

    QFile file(marker.absoluteFilePath());
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QString line = QString::fromUtf8(file.readLine()).trimmed();
    if (!line.startsWith(QLatin1String("gitdir:"), Qt::CaseInsensitive))
        return {};

    QString gitDir = line.mid(7).trimmed();
    if (QDir::isRelativePath(gitDir))
        gitDir = QDir(marker.absolutePath()).absoluteFilePath(gitDir);
    gitDir = resolvedDirectory(gitDir);
    if (gitDir.isEmpty())
        return {};

    QFile commonFile(QDir(gitDir).filePath(QStringLiteral("commondir")));
    if (!commonFile.open(QIODevice::ReadOnly | QIODevice::Text))
        return gitDir;
    QString common = QString::fromUtf8(commonFile.readLine()).trimmed();
    if (common.isEmpty())
        return gitDir;
    if (QDir::isRelativePath(common))
        common = QDir(gitDir).absoluteFilePath(common);
    const QString resolved = resolvedDirectory(common);
    return resolved.isEmpty() ? gitDir : resolved;
}

QString shellQuote(const QString &value)
{
    QString quoted = value;
    quoted.replace(QLatin1Char('\''), QStringLiteral("'\\''"));
    return QStringLiteral("'") + quoted + QStringLiteral("'");
}

bool isBroadWritableRoot(const QString &path)
{
    const QString resolved = resolvedDirectory(path);
    const QString home = resolvedDirectory(QDir::homePath());
    const QString temp = resolvedDirectory(QDir::tempPath());
    const QString root = resolvedDirectory(QDir::rootPath());
    return resolved == root || resolved == temp || resolved == home ||
           isWithin(home, resolved);
}

QString broadWorkspaceError()
{
    const QString workspace = state().workspace;
    if (isBroadWritableRoot(workspace)) {
        return QStringLiteral(
                   "%1 is too broad to expose as a writable VM workspace. "
                   "Start ForkMesh from a project or workspace subdirectory, "
                   "then enable KVM isolation.")
            .arg(QDir::toNativeSeparators(workspace));
    }
    return {};
}

QString checkLimaVersion(const QString &program)
{
    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(program, {QStringLiteral("--version")});
    if (!process.waitForStarted(2000) || !process.waitForFinished(2000)) {
        process.kill();
        process.waitForFinished(1000);
        return QStringLiteral(
            "ForkMesh could not read the limactl version. Lima 2.0 or newer "
            "is required for the restricted environment bridge.");
    }
    const QString output = QString::fromUtf8(process.readAll()).trimmed();
    static const QRegularExpression versionPattern(
        QStringLiteral("(?:^|\\s)v?(\\d+)\\.(\\d+)(?:\\.\\d+)?"));
    const QRegularExpressionMatch match = versionPattern.match(output);
    if (!match.hasMatch()) {
        return QStringLiteral(
                   "ForkMesh could not recognize the limactl version (%1). "
                   "Lima 2.0 or newer is required.")
            .arg(output.left(120));
    }
    if (match.captured(1).toInt() < 2) {
        return QStringLiteral(
                   "ForkMesh found %1. Lima 2.0 or newer is required so only "
                   "explicitly allowed environment variables reach the guest.")
            .arg(output.left(120));
    }
    return {};
}

bool guestEnvironmentKey(const QString &key)
{
    return key.startsWith(QStringLiteral("ANTHROPIC_")) ||
           key == QLatin1String("CLAUDE_CODE_OAUTH_TOKEN") ||
           key.startsWith(QStringLiteral("OPENAI_")) ||
           key.startsWith(QStringLiteral("CODEX_")) ||
           key.startsWith(QStringLiteral("FORKMESH_AGENT_")) ||
           key == QLatin1String("MAX_THINKING_TOKENS");
}

bool limactlHostEnvironmentKey(const QString &key)
{
    return key == QLatin1String("HOME") || key == QLatin1String("USER") ||
           key == QLatin1String("LOGNAME") || key == QLatin1String("PATH") ||
           key == QLatin1String("TMPDIR") ||
           key == QLatin1String("XDG_CONFIG_HOME") ||
           key == QLatin1String("XDG_DATA_HOME") ||
           key == QLatin1String("XDG_RUNTIME_DIR") ||
           key == QLatin1String("LIMA_HOME") ||
           key == QLatin1String("LIMA_SSH_OVER_VSOCK");
}

} // namespace

QString launchDirectory()
{
    const QString recorded =
        qEnvironmentVariable(kLaunchDirectoryEnvironment).trimmed();
    const QString candidate = recorded.isEmpty() ? QDir::currentPath() : recorded;
    const QString resolved = resolvedDirectory(candidate);
    if (!resolved.isEmpty() && QFileInfo(resolved).isDir())
        return resolved;
    return resolvedDirectory(QDir::currentPath());
}

void initialize(bool enabled, const QString &directory,
                const QString &limactlOverride)
{
    RuntimeState fresh;
    fresh.enabled = enabled;
    fresh.workspace = resolvedDirectory(directory.isEmpty() ? launchDirectory()
                                                             : directory);
    if (fresh.workspace.isEmpty())
        fresh.workspace = resolvedDirectory(QDir::currentPath());

    QString linkedGit = linkedGitCommonDirectory(fresh.workspace);
    if (!linkedGit.isEmpty() && isBroadWritableRoot(linkedGit)) {
        fresh.boundaryError =
            QStringLiteral(
                "The linked Git metadata for %1 resolves to %2, which is too "
                "broad to mount writable. Repair the checkout's .git pointer "
                "before enabling KVM isolation.")
                .arg(QDir::toNativeSeparators(fresh.workspace),
                     QDir::toNativeSeparators(linkedGit));
        linkedGit.clear();
    }
    const QByteArray identity = QCryptographicHash::hash(
        fresh.workspace.toUtf8() + '\0' + linkedGit.toUtf8(),
        QCryptographicHash::Sha256).toHex().left(16);
    fresh.name =
        QStringLiteral("forkmesh-kvm1-%1").arg(QString::fromLatin1(identity));
    fresh.worktrees = QDir::cleanPath(
        QDir::tempPath() + QStringLiteral("/forkmesh-vm-worktrees/") +
        fresh.name);
    fresh.limactl = limactlOverride.trimmed();
    if (fresh.limactl.isEmpty())
        fresh.limactl = QStandardPaths::findExecutable(QStringLiteral("limactl"));

    appendMount(fresh.mounts, fresh.workspace);
    appendMount(fresh.mounts, fresh.worktrees);
    appendMount(fresh.mounts, linkedGit);
    state() = fresh;
}

bool active()
{
    return state().enabled;
}

QString workspaceRoot()
{
    return state().workspace;
}

QString worktreeRoot()
{
    return state().worktrees;
}

QString instanceName()
{
    return state().name;
}

QString limactlProgram()
{
    return state().limactl;
}

QStringList mountedRoots()
{
    return state().mounts;
}

QString availabilityError()
{
#if !defined(Q_OS_LINUX)
    return QStringLiteral(
        "KVM isolation is available only on Linux hosts. This setting is not "
        "a container or software-emulation fallback.");
#else
    if (!state().boundaryError.isEmpty())
        return state().boundaryError;
    const QString unsafeWorkspace = broadWorkspaceError();
    if (!unsafeWorkspace.isEmpty())
        return unsafeWorkspace;
    if (state().limactl.isEmpty())
        return QStringLiteral(
            "limactl was not found in PATH. Install Lima 2.0 or newer and QEMU "
            "before enabling the KVM workspace.");
    if (!state().limaVersionChecked) {
        state().limaVersionChecked = true;
        state().limaVersionError = checkLimaVersion(state().limactl);
    }
    if (!state().limaVersionError.isEmpty())
        return state().limaVersionError;
    const QFileInfo kvm(QStringLiteral("/dev/kvm"));
    if (!kvm.exists())
        return QStringLiteral(
            "/dev/kvm is unavailable. Enable hardware virtualization and load "
            "the KVM kernel modules first.");
    QFile kvmDevice(kvm.absoluteFilePath());
    if (!kvmDevice.open(QIODevice::ReadWrite))
        return QStringLiteral(
            "ForkMesh cannot access /dev/kvm. Add this user to the kvm group "
            "and sign in again before preparing the VM.");
    return {};
#endif
}

bool pathIsShared(const QString &path)
{
    const QString resolved = resolvedDirectory(path);
    for (const QString &root : state().mounts) {
        if (isWithin(resolved, root))
            return true;
    }
    return false;
}

QString pathError(const QString &path)
{
    if (pathIsShared(path))
        return {};
    return QStringLiteral(
               "The work directory %1 is outside this KVM workspace (%2). "
               "Restart ForkMesh from the directory you want the guest to use, "
               "or turn off KVM isolation in Settings.")
        .arg(QDir::toNativeSeparators(path),
             QDir::toNativeSeparators(state().workspace));
}

QStringList listArguments()
{
    return {QStringLiteral("--tty=false"), QStringLiteral("list"),
            QStringLiteral("--quiet"), state().name};
}

QStringList startArguments()
{
    return {QStringLiteral("--tty=false"), QStringLiteral("start"),
            state().name};
}

QStringList createArguments()
{
    QStringList arguments{QStringLiteral("--tty=false"),
                          QStringLiteral("start"),
                          QStringLiteral("--name=") + state().name,
                          QStringLiteral("--vm-type=qemu"),
                          QStringLiteral("--containerd=none"),
                          QStringLiteral("--set=.ssh.forwardAgent=false"),
                          QStringLiteral("--cpus=4"),
                          QStringLiteral("--memory=4")};
    for (const QString &root : state().mounts)
        arguments << QStringLiteral("--mount-only=") + root +
                         QStringLiteral(":w");
    arguments << QStringLiteral("template:default");
    return arguments;
}

LaunchCommand isolateCommand(const QString &program,
                             const QStringList &arguments,
                             const QString &workingDirectory,
                             bool preserveEnvironment)
{
    LaunchCommand command{program, arguments, {}, false};
    if (!state().enabled)
        return command;
    command.isolated = true;
    command.error = availabilityError();
    if (command.error.isEmpty())
        command.error = pathError(workingDirectory);
    if (!command.error.isEmpty()) {
        command.program.clear();
        command.arguments.clear();
        return command;
    }

    command.program = state().limactl;
    command.arguments = {QStringLiteral("--tty=false"),
                         QStringLiteral("shell"),
                         QStringLiteral("--start")};
    if (preserveEnvironment)
        command.arguments << QStringLiteral("--preserve-env");
    command.arguments << QStringLiteral("--workdir=") +
                             resolvedDirectory(workingDirectory);
    command.arguments << state().name << program;
    command.arguments += arguments;
    return command;
}

void applyGuestEnvironmentPolicy(QProcessEnvironment &environment)
{
    if (!state().enabled)
        return;

    const QStringList scratchKeys{QStringLiteral("TMPDIR"),
                                  QStringLiteral("TMP"),
                                  QStringLiteral("TEMP"),
                                  QStringLiteral("XDG_CACHE_HOME")};
    QStringList guestScratch;
    for (const QString &key : environment.keys()) {
        const bool scratch = scratchKeys.contains(key);
        const bool sharedScratch =
            scratch && pathIsShared(environment.value(key));
        const bool sharedCodexHome =
            key == QLatin1String("CODEX_HOME") &&
            pathIsShared(environment.value(key));
        if (scratch && sharedScratch)
            guestScratch << key;
        if ((key == QLatin1String("CODEX_HOME") && !sharedCodexHome) ||
            (!guestEnvironmentKey(key) && !sharedScratch &&
             !limactlHostEnvironmentKey(key)))
            environment.remove(key);
    }
    environment.insert(QStringLiteral("LIMA_SHELLENV_BLOCK"),
                       QStringLiteral("*"));
    QStringList allowed{QStringLiteral("ANTHROPIC_*"),
                        QStringLiteral("CLAUDE_CODE_OAUTH_TOKEN"),
                        QStringLiteral("OPENAI_*"),
                        QStringLiteral("CODEX_*"),
                        QStringLiteral("FORKMESH_AGENT_*"),
                        QStringLiteral("MAX_THINKING_TOKENS")};
    allowed += guestScratch;
    environment.insert(QStringLiteral("LIMA_SHELLENV_ALLOW"),
                       allowed.join(QLatin1Char(',')));
}

QString interactiveCommand(const QString &program,
                           const QString &workingDirectory,
                           const QStringList &arguments)
{
    if (state().limactl.isEmpty())
        return {};
    QStringList words{state().limactl, QStringLiteral("shell"),
                      QStringLiteral("--start")};
    const QString workdir = workingDirectory.trimmed().isEmpty()
                                ? state().workspace
                                : resolvedDirectory(workingDirectory);
    if (!workdir.isEmpty())
        words << QStringLiteral("--workdir=") + workdir;
    words << state().name;
    if (!program.trimmed().isEmpty())
        words << program;
    words += arguments;
    QStringList quoted;
    quoted.reserve(words.size());
    for (const QString &word : std::as_const(words))
        quoted << shellQuote(word);
    return quoted.join(QLatin1Char(' '));
}

} // namespace forkmesh::vm
