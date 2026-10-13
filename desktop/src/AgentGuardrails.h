#pragma once

#include <QList>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>

class QJsonObject;

namespace forkmesh::agentguard {

// Repository-owned policy copied onto an agent launch. Directory entries are
// repository-relative and apply to both reads and writes in the disposable
// agent worktree. Read-only system runtime files, provider configuration,
// private scratch, and Git metadata are added as internal exceptions by
// restrictedCommand().
struct Config {
    bool enabled = false;
    QStringList allowedDirectories;
    QString provider;
    // Trusted source checkout (or bare mirror) used to validate a linked
    // agent worktree's external Git metadata. It is not itself granted access.
    QString repositoryRoot;

    // Project lock (Project settings > Agents), independent of the
    // repository policy above: the whole worktree when the repository has no
    // policy of its own, plus these absolute folders, and nothing else of the
    // user's. See forkmesh::projects::applyAgentAccess().
    bool projectLocked = false;
    QString projectName;
    QStringList projectReadWrite;
    QStringList projectReadOnly;

    // Whether restrictedCommand() wraps the launch in the Landlock helper.
    bool enforced() const { return enabled || projectLocked; }
};

struct LaunchCommand {
    QString program;
    QStringList arguments;
    QString error;
    bool restricted = false;
};

QStringList normalizeDirectories(const QStringList &entries,
                                 QStringList *rejected = nullptr);
bool pathAllowed(const QString &worktree, const QStringList &allowedDirectories,
                 const QString &path);
// Return the narrowest existing repository-relative directory that can cover a
// denied path. Empty means the path resolves outside the repository and cannot
// be approved by this repository's policy.
QString approvalDirectory(const QString &worktree, const QString &path);
QString toolViolation(const QString &toolName, const QJsonObject &input,
                      const QString &worktree,
                      const QStringList &allowedDirectories);
QString policyDescription(const QStringList &allowedDirectories);
QString promptInstruction(const Config &config);

// One row of the repository Settings allowlist: the whole repository first,
// then every top-level directory of the checkout plus any saved entry, with
// common source/test/documentation folders marked recommended and listed
// ahead of dependencies, build output, CI configuration, and secrets.
struct DirectoryChoice {
    QString path; // repository-relative; "." is the whole repository
    QString description;
    bool recommended = false;
    bool exists = false;
    bool allowed = false; // listed, or covered by a listed parent
};
QList<DirectoryChoice> directoryChoices(const QString &worktree,
                                        const QStringList &allowedDirectories);
// The recommended existing directories of a checkout, or "." when it has none.
QStringList recommendedDirectories(const QString &worktree);
// True when `after` no longer covers something `before` allowed, i.e. running
// agents must restart to pick up the tighter boundary.
bool policyNarrowed(const QStringList &before, const QStringList &after);

// Wrap a provider command in the bundled fail-closed Landlock helper. The
// wrapper is inside the VM when KVM isolation is active and on the host
// otherwise. An enabled policy is never silently downgraded: invalid/missing
// roots produce LaunchCommand::error and an unavailable Landlock kernel makes
// the helper exit before the provider starts.
LaunchCommand restrictedCommand(
    const QString &program, const QStringList &arguments,
    const QString &worktree, const Config &config,
    const QProcessEnvironment &environment,
    const QStringList &additionalReadOnly = {});

} // namespace forkmesh::agentguard
