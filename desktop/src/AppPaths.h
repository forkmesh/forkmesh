#pragma once

#include <QDir>
#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QVector>

// Every on-disk location ForkMesh owns, resolved in one place.
//
// Layout (the "projects" layout):
//
//   ~/projects/                      projects root  (FORKMESH_PROJECTS_DIR)
//     forkmesh/                      the home project (FORKMESH_HOME)
//       src/                         source checkout the app builds/updates from
//       bin/ toolchain/ builds/ worktrees/ release/    shared by every project
//       data/                                      this project's own files
//         core/                      identity, settings (config/), cache/, state/, logs/, ssh/
//         <plugin>/                  one folder per plugin that keeps data
//       project.json  logo.png
//     <project>/                     any other project, fully isolated
//       data/core/ data/<plugin>/
//       src/                         optional: the project's repositories
//       project.json  logo.png
//
// A project folder is never itself a git checkout: its checkout lives in src/.
// That rule is what keeps application data out of `git add -A`, and it is why
// a home folder that still *is* the checkout stays on the legacy locations
// until tools/forkmesh_layout.py has moved the checkout into src/.
//
// Each root resolves independently to the new location when it exists (or
// when there is nothing legacy to preserve), and to the legacy location
// otherwise, so a half-finished or refused migration never starts the app on
// an empty identity.
//
// Nothing here is active until activate() runs. Tests and helper binaries
// that never call it keep the QStandardPaths behaviour they isolate with
// QStandardPaths::setTestModeEnabled().
namespace forkmesh::paths {

inline constexpr char kHomeProjectSlug[] = "forkmesh";
inline constexpr char kProjectFileName[] = "project.json";
inline constexpr char kCoreFolder[] = "core";

inline const QVector<QPair<QString, QString>> &pluginDataEntries()
{
    static const QVector<QPair<QString, QString>> entries{
        {QStringLiteral("actions"), QStringLiteral("actions")},
        {QStringLiteral("agents"), QStringLiteral("agents")},
        {QStringLiteral("agent-accounts"), QStringLiteral("agents/accounts")},
        {QStringLiteral("agent-images"), QStringLiteral("agents/images")},
        {QStringLiteral("agent-runtime"), QStringLiteral("agents/runtime")},
        {QStringLiteral("agent-screenshots"), QStringLiteral("agents/screenshots")},
        {QStringLiteral("prompt"), QStringLiteral("agents/prompt")},
        {QStringLiteral("teammate-workspaces"), QStringLiteral("agents/teammate-workspaces")},
        {QStringLiteral("chat_history"), QStringLiteral("chat/history")},
        {QStringLiteral("avatars"), QStringLiteral("chat/avatars")},
        {QStringLiteral("local_mesh"), QStringLiteral("chat/local-mesh")},
        {QStringLiteral("notes"), QStringLiteral("notes")},
        {QStringLiteral("vault"), QStringLiteral("vault")},
        {QStringLiteral("backups"), QStringLiteral("backups")},
        {QStringLiteral("pull-meta"), QStringLiteral("pull-requests/meta")},
        {QStringLiteral("pull-review"), QStringLiteral("pull-requests/review")},
        {QStringLiteral("usage-reminders"), QStringLiteral("usage/reminders")},
        {QStringLiteral("rss-reader"), QStringLiteral("rss-reader")},
        {QStringLiteral("secure-drive"), QStringLiteral("secure-drive")},
        {QStringLiteral("fleet_logs"), QStringLiteral("logs-hosts/fleet")},
        {QStringLiteral("whisper.cpp"), QStringLiteral("speech/whisper.cpp")},
        {QStringLiteral("parakeet"), QStringLiteral("speech/parakeet")},
    };
    return entries;
}

inline QString organizedDataEntry(const QString &legacyName)
{
    for (const auto &entry : pluginDataEntries()) {
        if (entry.first == legacyName)
            return entry.second;
    }
    return QLatin1String(kCoreFolder) + QLatin1Char('/') + legacyName;
}

inline QString pluginOfDataEntry(const QString &organized)
{
    const QString first = organized.section(QLatin1Char('/'), 0, 0);
    return first == QLatin1String(kCoreFolder) ? QString() : first;
}

// Inputs of the resolver, injected so the logic is testable without touching
// the real home directory.
struct Environment {
    QString userHome;        // QDir::homePath()
    QString projectsDirEnv;  // FORKMESH_PROJECTS_DIR
    QString homeEnv;         // FORKMESH_HOME
    QString projectEnv;      // FORKMESH_PROJECT
    QString projectArg;      // --project <slug>
    QString legacyData;      // ~/.local/share/ForkMesh/ForkMesh
    QString legacyConfig;    // ~/.config/ForkMesh
    QString legacyCache;     // ~/.cache/ForkMesh/ForkMesh
    QString legacyState;     // ~/.forkmesh
    QString userSshDir;      // ~/.ssh
    // An XDG_*_HOME pointing away from its default is a deliberate sandbox
    // (benchmarks, tests, a throwaway profile). The home project then stays on
    // the XDG locations and nothing is migrated, unless FORKMESH_HOME or
    // FORKMESH_PROJECTS_DIR says where the layout should live instead.
    bool xdgSandbox = false;

    static Environment fromProcess(const QStringList &arguments);
};

enum class Root {
    Data,
    Config,
    Cache,
    State,
    Logs,
    Ssh,
};

struct Layout {
    QString projectsRoot;
    QString homeDir;
    QString projectSlug;
    QString projectDir;
    // False while projectDir is still a git checkout; every root is legacy.
    bool rerootAllowed = false;
    QString rerootBlockedReason;

    QString dataDir;
    bool organized = false;
    QString data;
    QString config;   // directory QSettings is pointed at (holds ForkMesh/ForkMesh.conf)
    QString cache;
    QString state;
    QString logs;
    QString ssh;

    bool isHomeProject() const { return projectSlug == QLatin1String(kHomeProjectSlug); }
    QString root(Root which) const;
    QString diagnosticsDir() const;
    // Shared, home-project-only locations.
    QString sourceDir() const;
    QString binDir() const;
    QString toolchainDir() const;
    QString buildsDir() const;
    QString worktreesDir() const;
    QString releaseDir() const;
    QString coreDir() const;
    QString pluginDir(const QString &plugin) const;
};

// "My Project!" -> "my-project". Empty when nothing usable remains; never
// returns a name that escapes the projects root.
QString slugify(const QString &name);
bool isValidSlug(const QString &slug);

// Pure: where things *would* live. Touches the disk only to test existence.
Layout resolve(const Environment &environment);

// One planned relocation of a legacy location into the layout.
struct Move {
    enum class Kind {
        Directory,
        File,
    };
    Kind kind = Kind::Directory;
    QString label;
    QString from;
    QString to;
    bool leaveLink = true;  // leave `from` as a symlink to `to`
};

struct MoveResult {
    Move move;
    enum class Outcome {
        Moved,
        AlreadyDone,   // nothing at `from`, or `from` already links to `to`
        Skipped,       // destination occupied, cross-device, in use...
        Failed,
    };
    Outcome outcome = Outcome::AlreadyDone;
    QString detail;
};

// The app-owned relocations for the home project. Empty for other projects
// (they are born in the layout) and while re-rooting is blocked.
QVector<Move> planMigration(const Environment &environment, const Layout &layout);

// Applies a plan with rename(2) only: instant, atomic per entry, and never a
// copy. A cross-device or occupied destination is reported, not forced.
QVector<MoveResult> applyMigration(const QVector<Move> &plan, bool dryRun);

// Rewrites IdentityFile/UserKnownHostsFile/CertificateFile values in an
// ssh_config text so they follow relocated files. Pure; returns the input
// unchanged when no path matches.
QString rewriteSshConfig(const QString &text,
                         const QVector<QPair<QString, QString>> &relocations);

// Applies rewriteSshConfig() to ~/.ssh/config for the keys a migration moved
// without leaving a link. Keeps config.before-forkmesh-projects beside it.
// Returns true when the file changed.
bool updateUserSshConfig(const Environment &environment,
                         const QVector<MoveResult> &results);

// Saved settings hold absolute paths (mirror folders, model folders, agent
// account homes). After a migration they still name the old locations and
// work only through the links left there. repointText() rewrites a relocated
// root wherever it appears as a whole path or a path prefix; it is pure and
// leaves look-alike siblings (…/ForkMesh-other) alone.
QString repointText(const QString &text,
                    const QVector<QPair<QString, QString>> &relocations);

// Applies repointText() to this project's settings file for every legacy root
// that has really moved (the old path is a link into the layout, or is gone
// while the new one exists). Keeps ForkMesh.conf.before-path-repoint. Returns
// how many references changed; 0 when there is nothing to do. Call while
// holding the single-instance lock, then sync QSettings.
int repointSettingsFile();

// --- process-wide state -----------------------------------------------------

// Resolves the layout for this process, points QSettings at the config root
// and exports FORKMESH_HOME, FORKMESH_PROJECTS_DIR, FORKMESH_PROJECT,
// FORKMESH_PROJECT_DIR, FORKMESH_DATA_DIR and FORKMESH_CONFIG_DIR for child
// tools. Never moves anything. Call before the first QSettings. A preview
// passes createFolders=false so that looking changes nothing on disk.
void activate(const QStringList &arguments, bool createFolders = true);

void retireStateRoot(const QString &oldStateDir);

// Moves the legacy locations into the layout, then re-resolves. Only call
// while holding the single-instance lock: nothing else may have these folders
// open by their old names. A dry run reports the plan and changes nothing.
QVector<MoveResult> migrateNow(bool dryRun = false);

// The lock file is the one thing that cannot move while it is held. Once the
// legacy locations are migrated, this switches the state and logs roots into
// the project and returns the legacy lock path the caller must release and
// delete after taking the new lock. Empty when there is nothing to adopt.
QString adoptProjectStateRoot();

// Process state lives in the header so the accessors below are usable from
// any translation unit without linking AppPaths.cpp: most stores are also
// compiled into standalone test binaries, which never activate the layout.
namespace detail {
inline bool active = false;
inline Layout current;
} // namespace detail

inline bool isActive() { return detail::active; }
inline const Layout &layout() { return detail::current; }

// Accessors every call site uses instead of QStandardPaths / QDir::homePath().
// Before activate() they answer exactly what the call sites used to compute.
inline QString dataRoot()
{
    return detail::active
               ? detail::current.data
               : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

// Machine-local data. Identical to dataRoot() inside the layout; on a legacy
// Windows profile it stays the non-roaming folder it always was.
inline QString localDataRoot()
{
    if (detail::active &&
        detail::current.data.startsWith(detail::current.projectDir + QLatin1Char('/')))
        return detail::current.data;
    return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
}

inline QString dataDir()
{
    return detail::active
               ? detail::current.dataDir
               : QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
}

inline QString dataPath(const QString &legacyName)
{
    if (detail::active && detail::current.organized)
        return detail::current.dataDir + QLatin1Char('/') + organizedDataEntry(legacyName);
    return dataRoot() + QLatin1Char('/') + legacyName;
}

inline QString pluginDataDir(const QString &plugin)
{
    if (detail::active && detail::current.organized)
        return detail::current.dataDir + QLatin1Char('/') + plugin;
    return dataRoot() + QLatin1Char('/') + plugin;
}

inline QString cacheRoot()
{
    return detail::active
               ? detail::current.cache
               : QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
}

// QSettings::fileName() is not always a file ForkMesh owns (issue #727):
//  - Windows native format is the registry; fileName() is a key name such as
//    "\\HKEY_CURRENT_USER\\Software\\ForkMesh\\ForkMesh", which QFileInfo calls
//    rooted -- its "folder" would be <drive>:\\HKEY_CURRENT_USER\\Software\\ForkMesh.
//  - macOS native format is a plist inside the shared ~/Library/Preferences,
//    a folder every other application uses too.
// Derive filesystem paths from settings only through these helpers.
inline bool isRegistrySettingsName(const QString &fileName)
{
    static const QRegularExpression registryKey(
        QStringLiteral("^[\\\\/]+HKEY_[A-Z_]+(?:[\\\\/]|$)"),
        QRegularExpression::CaseInsensitiveOption);
    return registryKey.match(fileName).hasMatch();
}

// The settings file itself, or empty when the store is not a file (registry).
inline QString settingsFilePathFor(const QString &fileName, QSettings::Format format)
{
    if (fileName.isEmpty() || isRegistrySettingsName(fileName))
        return QString();
#if defined(Q_OS_WIN)
    if (format == QSettings::NativeFormat)
        return QString();
#else
    Q_UNUSED(format);
#endif
    const QFileInfo info(fileName);
    return info.isAbsolute() ? info.absoluteFilePath() : QString();
}

// The folder holding the settings file when ForkMesh owns that folder; empty
// for the registry and for the shared macOS Preferences folder.
inline QString settingsFolderFor(const QString &fileName, QSettings::Format format)
{
    const QString file = settingsFilePathFor(fileName, format);
    if (file.isEmpty())
        return QString();
#if defined(Q_OS_MACOS)
    if (format == QSettings::NativeFormat)
        return QString();
#endif
    return QFileInfo(file).absolutePath();
}

inline QString settingsFilePath(const QSettings &settings)
{
    return settingsFilePathFor(settings.fileName(), settings.format());
}

inline QString settingsFolder(const QSettings &settings)
{
    return settingsFolderFor(settings.fileName(), settings.format());
}

// Folder holding the settings file (ForkMesh.conf) itself; empty when the
// settings live in the registry or the shared macOS Preferences folder.
inline QString settingsDir()
{
    return settingsFolder(QSettings());
}

inline QString stateRoot()
{
    return detail::active ? detail::current.state
                          : QDir::homePath() + QStringLiteral("/.forkmesh");
}

inline QString logsDir()
{
    return detail::active ? detail::current.logs
                          : QDir::homePath() + QStringLiteral("/.forkmesh");
}

inline QString diagnosticsDir() { return logsDir() + QStringLiteral("/diagnostics"); }

inline QString sshDir()
{
    return detail::active ? detail::current.ssh : dataRoot() + QStringLiteral("/ssh");
}

QString configRoot();
QString projectsRoot();
QString homeDir();
QString projectDir();
QString projectSlug();

// Pre-QApplication resolution of the main log, for the crash handler.
QString earlyMainLogPath(int argc, char **argv);

// Local-socket name of the single-instance server; one per project.
QString singleInstanceServerName();
bool isDefaultStateRoot();

// --- storage map -------------------------------------------------------------

struct Location {
    QString id;
    QString label;
    QString path;
    QString hint;
    enum class Scope {
        Project,   // belongs to the open project
        Shared,    // home project, used by every project
        Legacy,    // outside the projects root: still to migrate, or a link
        Foreign,   // another project's folder
    };
    Scope scope = Scope::Project;
    bool inBackup = false;
    bool isCompatLink = false;  // symlink left behind by the migration
};

// Every location that exists or is expected, including what is still outside
// the projects root. `extra` lets callers add settings-driven roots (mirror
// root, preview cache, backups) that may have been pointed elsewhere.
QVector<Location> storageMap(const Environment &environment, const Layout &layout,
                             const QVector<Location> &extra = {});

// Known ForkMesh locations outside the projects root (hidden folders in the
// home directory, ~/.ssh keys, stray worktrees), whether or not the app owns
// their migration.
QVector<Location> legacyLocations(const Environment &environment);

} // namespace forkmesh::paths
