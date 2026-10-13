#pragma once

#include <QString>
#include <QStringList>

namespace forkmesh::agentguard {
struct Config;
}

// What a project lets its coding agents reach: which folders, and whether
// they may call out to the network. Stored in the project's project.json under
// "agentAccess", so any process can edit any project's settings and every
// process serving that project reads the same policy.
//
// A project with no saved policy follows the global rules, which default to
// locked: agents work in their worktree and the project folder only, and every
// external call waits for the user.
namespace forkmesh::projects {

struct AgentAccess {
    enum class Folders {
        Project,      // the worktree, the project folder and the extra folders
        Unrestricted, // no folder boundary beyond the repository's own
    };
    enum class Network {
        Ask,   // every external call waits for the user's approval
        Block, // external calls are refused, approved destinations excepted
        Allow, // external calls run without asking
    };

    Folders folders = Folders::Project;
    // Read-only installed toolchains (~/.local/bin, ~/.cargo/bin, ...) and
    // read-write caches and temp folders, so builds keep working when locked.
    bool sharedTools = true;
    QStringList readOnlyFolders;  // absolute
    QStringList readWriteFolders; // absolute
    Network network = Network::Ask;
    // Destinations approved for every agent in the project. "example.com"
    // matches that host; "*.example.com" also matches its subdomains.
    QStringList allowedHosts;
    bool followGlobal = false;
    QStringList blockedHosts;
    QStringList allowedDestructive;
    QStringList allowedDeletePaths;
};

struct GlobalAgentRules {
    enum class Destructive { Ask, Block, Allow };

    AgentAccess defaults;
    QStringList blockedHosts;
    bool toolAllowlist = false;
    QStringList allowedToolFamilies;
    Destructive destructive = Destructive::Ask;
};

QString foldersKey(AgentAccess::Folders folders);
QString networkKey(AgentAccess::Network network);
QString destructiveKey(GlobalAgentRules::Destructive destructive);

QString globalRulesPath(const QString &homeProjectDir);
GlobalAgentRules loadGlobalRules(const QString &path);
bool saveGlobalRules(const QString &path, const GlobalAgentRules &rules,
                     QString *error = nullptr);
AgentAccess effectiveAgentAccess(const AgentAccess &project, const GlobalAgentRules &rules);
bool gatesExternalCalls(const AgentAccess &access);
bool hostBlocked(const QStringList &blockedHosts, const QString &host);
bool webSearchAllowed(const AgentAccess &access);

// The saved policy, or one that follows the global rules when project.json has none.
AgentAccess loadAgentAccess(const QString &projectDir);
// Writes "agentAccess" into project.json, keeping every other key.
bool saveAgentAccess(const QString &projectDir, const AgentAccess &access,
                     QString *error = nullptr);

// Lowercased host with any port, userinfo and trailing dot removed; empty
// when `text` is not a usable host or host pattern.
QString normalizeHost(const QString &text);
bool hostAllowed(const QStringList &allowedHosts, const QString &host);

// The folders a locked agent may read and write inside the project: every
// top-level entry of the project folder except the project's private roots
// (data, config, cache, state, logs, ssh) and anything holding one of
// `privateRoots`. Empty when the project folder is the home folder or one of
// its parents: granting that would be no lock at all.
QStringList projectAgentFolders(const QString &projectDir,
                                const QStringList &privateRoots,
                                const QString &homeDir);

// Installed toolchains agents may read, and the caches and temp folders they
// may write, when AgentAccess::sharedTools is on.
QStringList sharedToolFolders(const QString &homeDir);
QStringList sharedScratchFolders(const QString &homeDir);

// Fills the project lock of `guardrails` from `access`. The lock needs Linux
// Landlock and runs on the host, so it is left off elsewhere and inside the
// KVM guest (`vmActive`), which is already its own boundary.
void applyAgentAccess(forkmesh::agentguard::Config &guardrails,
                      const AgentAccess &access, const QString &projectDir,
                      const QStringList &privateRoots, const QString &homeDir,
                      bool vmActive);

// Whether this platform can hold agents to the project folder.
bool folderLockSupported();

} // namespace forkmesh::projects
