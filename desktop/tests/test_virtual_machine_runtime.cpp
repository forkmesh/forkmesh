#include "../src/VirtualMachineRuntime.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QTemporaryDir>

namespace {

int failures = 0;

void check(bool condition, const char *what)
{
    if (condition)
        qInfo("PASS: %s", what);
    else {
        qCritical("FAIL: %s", what);
        ++failures;
    }
}

bool writeText(const QString &path, const QByteArray &contents)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
        return false;
    return file.write(contents) == contents.size();
}

} // namespace

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    check(temp.isValid(), "temporary test workspace is available");
    if (!temp.isValid())
        return 1;

    const QString workspace = temp.filePath(QStringLiteral("workspace"));
    const QString commonGit = temp.filePath(QStringLiteral("common.git"));
    const QString checkoutGit =
        QDir(commonGit).filePath(QStringLiteral("worktrees/checkout"));
    check(QDir().mkpath(workspace) && QDir().mkpath(checkoutGit),
          "linked-worktree fixture directories are created");
    check(writeText(QDir(workspace).filePath(QStringLiteral(".git")),
                    QByteArray("gitdir: ") + checkoutGit.toUtf8() + '\n') &&
              writeText(QDir(checkoutGit).filePath(QStringLiteral("commondir")),
                        QByteArray("../..\n")),
          "linked-worktree metadata fixture is written");

    const QByteArray previousLaunchDir =
        qgetenv(forkmesh::vm::kLaunchDirectoryEnvironment);
    qputenv(forkmesh::vm::kLaunchDirectoryEnvironment, workspace.toUtf8());
    check(forkmesh::vm::launchDirectory() ==
              QFileInfo(workspace).canonicalFilePath(),
          "recorded caller directory wins over the helper's current directory");

    const QString fakeLimactl = QStringLiteral("/missing/forkmesh-limactl");
    forkmesh::vm::initialize(true, workspace, fakeLimactl);
    const QString firstName = forkmesh::vm::instanceName();
    const QString testWorktreeRoot = forkmesh::vm::worktreeRoot();
    const QString canonicalWorkspace = QFileInfo(workspace).canonicalFilePath();
    const QString canonicalCommon = QFileInfo(commonGit).canonicalFilePath();
    check(forkmesh::vm::active() &&
              forkmesh::vm::workspaceRoot() == canonicalWorkspace,
          "enabled runtime snapshots its canonical workspace");
    check(firstName.startsWith(QStringLiteral("forkmesh-kvm1-")) &&
              firstName.size() == 30,
          "workspace gets a deterministic bounded instance name");
    check(forkmesh::vm::mountedRoots().contains(canonicalWorkspace) &&
              forkmesh::vm::mountedRoots().contains(canonicalCommon) &&
              forkmesh::vm::mountedRoots().contains(
                  forkmesh::vm::worktreeRoot()),
          "workspace, linked Git metadata, and temporary worktrees are mounted");

    const QString nested = QDir(workspace).filePath(QStringLiteral("src/new.cpp"));
    const QString outside = temp.filePath(QStringLiteral("outside/new.cpp"));
    check(forkmesh::vm::pathIsShared(nested) &&
              !forkmesh::vm::pathIsShared(outside) &&
              !forkmesh::vm::pathError(outside).isEmpty(),
          "working directories outside the launch boundary are rejected");

    const QStringList create = forkmesh::vm::createArguments();
    const QStringList inspect = forkmesh::vm::listArguments();
    check(inspect.contains(QStringLiteral("list")) &&
              inspect.contains(QStringLiteral("--quiet")) &&
              inspect.constLast() == firstName,
          "preparation inspects the deterministic instance without starting it");
    check(create.contains(QStringLiteral("--vm-type=qemu")) &&
              create.contains(QStringLiteral("--containerd=none")) &&
              create.contains(
                  QStringLiteral("--set=.ssh.forwardAgent=false")) &&
              create.contains(QStringLiteral("--mount-only=") +
                              canonicalWorkspace + QStringLiteral(":w")) &&
              create.contains(QStringLiteral("--mount-only=") +
                              canonicalCommon + QStringLiteral(":w")) &&
              create.constLast() == QStringLiteral("template:default"),
          "Lima creation is pinned to QEMU and only explicit writable mounts");

    forkmesh::vm::initialize(true, workspace, fakeLimactl);
    check(forkmesh::vm::instanceName() == firstName,
          "the same launch directory reuses the same VM identity");

    const QString scratch =
        QDir(forkmesh::vm::worktreeRoot()).filePath(QStringLiteral("jail/tmp"));
    QDir().mkpath(scratch);
    QProcessEnvironment environment;
    environment.insert(QStringLiteral("HOME"), QDir::homePath());
    environment.insert(QStringLiteral("PATH"), QStringLiteral("/usr/bin"));
    environment.insert(QStringLiteral("UNRELATED_HOST_SECRET"),
                       QStringLiteral("do-not-forward"));
    environment.insert(QStringLiteral("OPENAI_API_KEY"),
                       QStringLiteral("allowed-provider-value"));
    environment.insert(QStringLiteral("CODEX_HOME"), outside);
    environment.insert(QStringLiteral("TMPDIR"), scratch);
    forkmesh::vm::applyGuestEnvironmentPolicy(environment);
    check(environment.contains(QStringLiteral("HOME")) &&
              environment.contains(QStringLiteral("PATH")) &&
              !environment.contains(QStringLiteral("UNRELATED_HOST_SECRET")) &&
              environment.contains(QStringLiteral("OPENAI_API_KEY")) &&
              !environment.contains(QStringLiteral("CODEX_HOME")),
          "limactl keeps host runtime values while unrelated secrets and unsafe paths are removed");
    check(environment.value(QStringLiteral("LIMA_SHELLENV_BLOCK")) ==
                  QStringLiteral("*") &&
              environment.value(QStringLiteral("LIMA_SHELLENV_ALLOW"))
                  .contains(QStringLiteral("OPENAI_*")) &&
              environment.value(QStringLiteral("LIMA_SHELLENV_ALLOW"))
                  .contains(QStringLiteral("TMPDIR")) &&
              !environment.value(QStringLiteral("LIMA_SHELLENV_ALLOW"))
                   .contains(QStringLiteral("HOME")),
          "the guest environment bridge is deny-by-default with a narrow allowlist");

    const QString quoted = forkmesh::vm::interactiveCommand(
        QStringLiteral("bash"), workspace,
        {QStringLiteral("-lc"), QStringLiteral("printf '%s' a'b")});
    check(quoted.contains(QStringLiteral("'shell'")) &&
              quoted.contains(QStringLiteral("'--start'")) &&
              quoted.contains(firstName) &&
              quoted.contains(QStringLiteral("a'\\''b")),
          "interactive guest commands preserve argument boundaries and quotes");

    const forkmesh::vm::LaunchCommand unavailable =
        forkmesh::vm::isolateCommand(QStringLiteral("bash"),
                                     {QStringLiteral("-lc"),
                                      QStringLiteral("true")},
                                     workspace);
    check(unavailable.isolated && unavailable.program.isEmpty() &&
              !unavailable.error.isEmpty(),
          "an enabled runtime fails closed when KVM prerequisites are unavailable");

#if defined(Q_OS_LINUX)
    forkmesh::vm::initialize(true, QDir::homePath(), fakeLimactl);
    check(forkmesh::vm::availabilityError().contains(
              QStringLiteral("too broad"), Qt::CaseInsensitive),
          "home-directory launches cannot expose the whole account to the VM");

    const QString unsafeWorkspace =
        temp.filePath(QStringLiteral("unsafe-workspace"));
    QDir().mkpath(unsafeWorkspace);
    writeText(QDir(unsafeWorkspace).filePath(QStringLiteral(".git")),
              QByteArray("gitdir: ") + QDir::homePath().toUtf8() + '\n');
    forkmesh::vm::initialize(true, unsafeWorkspace, fakeLimactl);
    check(forkmesh::vm::availabilityError().contains(
              QStringLiteral("linked Git metadata"), Qt::CaseInsensitive),
          "a checkout cannot smuggle a broad writable mount through its Git pointer");
#endif

    QDir(testWorktreeRoot).removeRecursively();
    forkmesh::vm::initialize(false, workspace, fakeLimactl);
    const forkmesh::vm::LaunchCommand host =
        forkmesh::vm::isolateCommand(QStringLiteral("bash"),
                                     {QStringLiteral("-lc"),
                                      QStringLiteral("true")},
                                     workspace);
    check(!host.isolated && host.program == QStringLiteral("bash") &&
              host.arguments.size() == 2,
          "disabled runtime leaves the host command unchanged");

    if (previousLaunchDir.isNull())
        qunsetenv(forkmesh::vm::kLaunchDirectoryEnvironment);
    else
        qputenv(forkmesh::vm::kLaunchDirectoryEnvironment, previousLaunchDir);

    if (failures == 0)
        qInfo("All KVM runtime boundary tests passed.");
    else
        qCritical("%d KVM runtime boundary test(s) failed.", failures);
    return failures == 0 ? 0 : 1;
}
