#pragma once

#include <QDir>
#include <QFileInfo>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QString>
#include <QStringList>

// Where the agent CLIs actually live, as opposed to where a desktop session's
// PATH happens to point.
//
// Claude Code and Codex are user-scoped installs: the native installer drops
// ~/.local/bin/claude, npm -g drops it in the npm prefix (~/.npm-global/bin, an
// nvm version's bin, or a Homebrew prefix), and the legacy local installer
// leaves a launcher at ~/.claude/local/claude. None of those are reliably on
// the PATH a GUI process inherits when it is started from a .desktop launcher,
// a dock icon or Finder, because no login shell ran to source the user's
// profile. So ForkMesh would announce "Claude Code is not installed on this
// device" on machines where `claude` runs fine in a terminal — while the agent
// runner started it anyway, since it widens PATH before spawning.
//
// Resolve through PATH first so a user's own override still wins, then fall
// back to the known install directories, and hand the resolved absolute path to
// whatever is about to exec it: a terminal emulator launched as
// `gnome-terminal -- claude` inherits the same truncated PATH we just worked
// around.
namespace forkmesh::agentcli {

// User-owned install prefixes. Safe to take precedence over an inherited PATH,
// which is the point: the user installed these for themselves.
inline QStringList userInstallDirs()
{
    const QString home = QDir::homePath();
    if (home.isEmpty())
        return {};
    const QDir h(home);
    return {h.filePath(QStringLiteral(".local/bin")),
            h.filePath(QStringLiteral(".claude/local")),
            h.filePath(QStringLiteral(".claude/bin")),
            h.filePath(QStringLiteral(".npm-global/bin")),
            h.filePath(QStringLiteral(".bun/bin")),
            h.filePath(QStringLiteral(".cargo/bin")),
            h.filePath(QStringLiteral("bin"))};
}

// Prefixes worth searching but not worth promoting: every nvm-managed Node
// release has its own global bin directory, and only one of them is the version
// the user actually selected. Ordering these ahead of an inherited PATH would
// swap the active `node` out from under a process.
inline QStringList fallbackInstallDirs()
{
    QStringList dirs;
    const QString home = QDir::homePath();
    if (!home.isEmpty()) {
        QString nvmDir = qEnvironmentVariable("NVM_DIR").trimmed();
        if (nvmDir.isEmpty())
            nvmDir = QDir(home).filePath(QStringLiteral(".nvm"));
        const QDir versions(nvmDir + QStringLiteral("/versions/node"));
        const QStringList releases = versions.entryList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
        for (const QString &release : releases)
            dirs << versions.filePath(release) + QStringLiteral("/bin");
    }
#if defined(Q_OS_WIN)
    const QString appData = qEnvironmentVariable("APPDATA").trimmed();
    if (!appData.isEmpty())
        dirs << QDir(appData).filePath(QStringLiteral("npm"));
#else
    dirs << QStringLiteral("/opt/homebrew/bin")
         << QStringLiteral("/usr/local/bin") << QStringLiteral("/usr/bin")
         << QStringLiteral("/bin");
#endif
    return dirs;
}

// Absolute path to `program`, or an empty string when it is genuinely absent.
inline QString executable(const QString &program)
{
    const QString value = program.trimmed();
    if (value.isEmpty())
        return {};
    if (QFileInfo(value).isAbsolute()) {
        const QFileInfo info(value);
        return info.isFile() && info.isExecutable() ? info.absoluteFilePath()
                                                    : QString();
    }
    const QString onPath = QStandardPaths::findExecutable(value);
    if (!onPath.isEmpty())
        return onPath;
    return QStandardPaths::findExecutable(value,
                                          userInstallDirs() +
                                              fallbackInstallDirs());
}

inline bool installed(const QString &program)
{
    return !executable(program).isEmpty();
}

// `path` with each existing user install directory it lacks put in front, the
// way ~/.profile does, and the entries it already had kept in their order. An
// empty PATH starts from the system default rather than losing /usr/bin.
// `added` receives the directories that were missing.
inline QString pathWithUserInstallDirs(const QString &path,
                                       QStringList *added = nullptr)
{
    const QChar separator = QDir::listSeparator();
    QStringList entries = path.split(separator, Qt::SkipEmptyParts);
#if !defined(Q_OS_WIN)
    if (entries.isEmpty())
        entries = {QStringLiteral("/usr/local/sbin"),
                   QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/sbin"),
                   QStringLiteral("/usr/bin"), QStringLiteral("/sbin"),
                   QStringLiteral("/bin")};
#endif
    QStringList missing;
    for (const QString &dir : userInstallDirs()) {
        const QString native = QDir::toNativeSeparators(dir);
        if (!entries.contains(native) && !entries.contains(dir) &&
            QFileInfo(dir).isDir())
            missing << native;
    }
    if (added)
        *added = missing;
    return (missing + entries).join(separator);
}

// Run once at the top of main(), before any thread reads the environment. A
// systemd user service, a .desktop launcher or a dock icon starts ForkMesh
// without a login shell, so its PATH lacks ~/.local/bin and the rest, and
// every agent, terminal and helper it starts inherits that. Returns the
// directories it added.
inline QStringList addUserInstallDirsToProcessPath()
{
    const QString before = qEnvironmentVariable("PATH");
    QStringList added;
    const QString after = pathWithUserInstallDirs(before, &added);
    if (after != before)
        qputenv("PATH", after.toLocal8Bit());
    return added;
}

// Widen a child's PATH to exactly the set executable() searches, so a process
// spawned from ForkMesh can always exec a CLI the UI just reported as present.
inline void widenPath(QProcessEnvironment &env)
{
    const QChar separator = QDir::listSeparator();
    QStringList path = userInstallDirs();
    path << env.value(QStringLiteral("PATH"))
                .split(separator, Qt::SkipEmptyParts);
    path << fallbackInstallDirs();
    path.removeDuplicates();
    env.insert(QStringLiteral("PATH"), path.join(separator));
}

} // namespace forkmesh::agentcli
