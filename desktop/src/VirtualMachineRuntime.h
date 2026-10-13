#pragma once

#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

namespace forkmesh::vm {

inline constexpr auto kEnabledSetting = "runtime/kvmEnabled";
inline constexpr auto kLaunchDirectoryEnvironment =
    "FORKMESH_START_DIRECTORY";

struct LaunchCommand {
    QString program;
    QStringList arguments;
    QString error;
    bool isolated = false;
};

QString launchDirectory();

void initialize(bool enabled, const QString &directory = QString(),
                const QString &limactlOverride = QString());

bool active();
QString workspaceRoot();
QString worktreeRoot();
QString instanceName();
QString limactlProgram();
QStringList mountedRoots();

QString availabilityError();

bool pathIsShared(const QString &path);
QString pathError(const QString &path);

QStringList listArguments();
QStringList startArguments();
QStringList createArguments();

// Wrap a provider process with `limactl shell`. stdin/stdout remain pipes, so
// the Claude stream-json and Codex app-server protocols work unchanged.
LaunchCommand isolateCommand(const QString &program,
                             const QStringList &arguments,
                             const QString &workingDirectory,
                             bool preserveEnvironment = true);

// Limit Lima's --preserve-env bridge to the credentials and agent controls
// ForkMesh deliberately put in the provider environment. Host PATH, HOME,
// desktop sockets, SSH agents, and unrelated secrets stay outside the guest.
void applyGuestEnvironmentPolicy(QProcessEnvironment &environment);

QString interactiveCommand(const QString &program = QString(),
                           const QString &workingDirectory = QString(),
                           const QStringList &arguments = QStringList());

} // namespace forkmesh::vm
