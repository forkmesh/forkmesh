#include "AppPaths.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>

#if defined(Q_OS_UNIX)
#include <pwd.h>
#include <unistd.h>
#endif

namespace forkmesh::paths {

namespace {

QString clean(const QString &path)
{
    return path.isEmpty() ? QString() : QDir::cleanPath(path);
}

QString join(const QString &base, const QString &child)
{
    return clean(base + QLatin1Char('/') + child);
}

QString envString(const char *name)
{
    return QString::fromLocal8Bit(qgetenv(name)).trimmed();
}

// XDG defaults computed by hand: this runs before QCoreApplication exists,
// and QStandardPaths' app-specific locations need its names.
QString xdgBase(const char *variable, const QString &home, const QString &fallback,
                bool *overridden = nullptr)
{
    const QString value = envString(variable);
    if (overridden && !value.isEmpty() && clean(value) != join(home, fallback))
        *overridden = true;
    return value.isEmpty() ? join(home, fallback) : clean(value);
}

// A project folder that is itself a git checkout must not receive application
// data: the app commits its own checkout, and `git add -A` would sweep up the
// identity key and gigabytes of mirrors.
bool isGitCheckout(const QString &dir)
{
    return QFileInfo::exists(join(dir, QStringLiteral(".git")));
}

bool existsOrLink(const QString &path)
{
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

// New location when it exists, or when there is nothing legacy to preserve.
// `legacyProbe` is what proves legacy content exists; it differs from `legacy`
// where the legacy root is a shared folder such as ~/.config.
//
// `modernProbe` is what proves the new location is populated, where the folder
// alone does not: config/ may already hold tool files before the settings
// folder has moved in.
QString pick(const QString &modern, const QString &legacy, bool rerootAllowed,
             const QString &legacyProbe = QString(),
             const QString &modernProbe = QString())
{
    if (!rerootAllowed)
        return legacy;
    if (QFileInfo::exists(modernProbe.isEmpty() ? modern : modernProbe))
        return modern;
    const QString probe = legacyProbe.isEmpty() ? legacy : legacyProbe;
    if (!legacy.isEmpty() && QFileInfo::exists(probe) && !QFileInfo(probe).isSymLink())
        return legacy;
    return modern;
}

bool renamePath(const QString &from, const QString &to, bool *crossDevice,
                QString *detail)
{
    *crossDevice = false;
#if defined(Q_OS_UNIX)
    if (std::rename(QFile::encodeName(from).constData(),
                    QFile::encodeName(to).constData()) == 0)
        return true;
    const int error = errno;
    *crossDevice = error == EXDEV;
    *detail = QString::fromLocal8Bit(std::strerror(error));
    return false;
#else
    if (QDir().rename(from, to))
        return true;
    *detail = QStringLiteral("the folder is in use or on another drive");
    return false;
#endif
}

QString valueAfter(const QStringList &arguments, const QString &flag)
{
    for (int i = 1; i < arguments.size(); ++i) {
        const QString &argument = arguments.at(i);
        if (argument == flag && i + 1 < arguments.size())
            return arguments.at(i + 1).trimmed();
        if (argument.startsWith(flag + QLatin1Char('=')))
            return argument.mid(flag.size() + 1).trimmed();
    }
    return QString();
}

} // namespace

Environment Environment::fromProcess(const QStringList &arguments)
{
    Environment environment;
    environment.userHome = clean(QDir::homePath());
    environment.projectsDirEnv = envString("FORKMESH_PROJECTS_DIR");
    environment.homeEnv = envString("FORKMESH_HOME");
    environment.projectEnv = envString("FORKMESH_PROJECT");
    environment.projectArg = valueAfter(arguments, QStringLiteral("--project"));
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    const QString &home = environment.userHome;
    environment.legacyData =
        join(xdgBase("XDG_DATA_HOME", home, QStringLiteral(".local/share"),
                     &environment.xdgSandbox),
             QStringLiteral("ForkMesh/ForkMesh"));
    environment.legacyConfig =
        join(xdgBase("XDG_CONFIG_HOME", home, QStringLiteral(".config"),
                     &environment.xdgSandbox),
             QStringLiteral("ForkMesh"));
    environment.legacyCache =
        join(xdgBase("XDG_CACHE_HOME", home, QStringLiteral(".cache"),
                     &environment.xdgSandbox),
             QStringLiteral("ForkMesh/ForkMesh"));
#else
    // Windows and macOS keep platform locations for the legacy roots; they are
    // read once QCoreApplication carries the application names.
    if (QCoreApplication::instance()) {
        environment.legacyData = clean(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
        environment.legacyCache = clean(
            QStandardPaths::writableLocation(QStandardPaths::CacheLocation));
        // Empty for the registry / shared macOS Preferences: the home project
        // keeps its native store there and there is no folder to migrate.
        const QString settingsFolder = settingsDir();
        environment.legacyConfig =
            settingsFolder.isEmpty() ? QString() : clean(settingsFolder);
    }
#endif
    environment.legacyState = join(environment.userHome, QStringLiteral(".forkmesh"));
    environment.userSshDir = join(environment.userHome, QStringLiteral(".ssh"));
    return environment;
}

QString slugify(const QString &name)
{
    QString slug;
    slug.reserve(name.size());
    bool pendingDash = false;
    for (const QChar raw : name.trimmed().toLower()) {
        const bool keep = (raw >= QLatin1Char('a') && raw <= QLatin1Char('z')) ||
                          (raw >= QLatin1Char('0') && raw <= QLatin1Char('9'));
        if (keep) {
            if (pendingDash && !slug.isEmpty())
                slug += QLatin1Char('-');
            pendingDash = false;
            slug += raw;
        } else {
            pendingDash = true;
        }
    }
    return slug.left(48);
}

bool isValidSlug(const QString &slug)
{
    static const QRegularExpression pattern(
        QStringLiteral("^[a-z0-9](?:[a-z0-9-]{0,46}[a-z0-9])?$"));
    return pattern.match(slug).hasMatch();
}

QString Layout::root(Root which) const
{
    switch (which) {
    case Root::Data:
        return data;
    case Root::Config:
        return config;
    case Root::Cache:
        return cache;
    case Root::State:
        return state;
    case Root::Logs:
        return logs;
    case Root::Ssh:
        return ssh;
    }
    return QString();
}

QString Layout::diagnosticsDir() const { return join(logs, QStringLiteral("diagnostics")); }
QString Layout::sourceDir() const { return join(homeDir, QStringLiteral("src")); }
QString Layout::binDir() const { return join(homeDir, QStringLiteral("bin")); }
QString Layout::toolchainDir() const { return join(homeDir, QStringLiteral("toolchain")); }
QString Layout::buildsDir() const { return join(homeDir, QStringLiteral("builds")); }
QString Layout::worktreesDir() const { return join(homeDir, QStringLiteral("worktrees")); }
QString Layout::releaseDir() const { return join(homeDir, QStringLiteral("release")); }

Layout resolve(const Environment &environment)
{
    Layout layout;
    layout.projectsRoot = environment.projectsDirEnv.isEmpty()
                              ? join(environment.userHome, QStringLiteral("projects"))
                              : clean(environment.projectsDirEnv);
    layout.homeDir = environment.homeEnv.isEmpty()
                         ? join(layout.projectsRoot, QLatin1String(kHomeProjectSlug))
                         : clean(environment.homeEnv);

    QString slug = environment.projectArg.isEmpty() ? environment.projectEnv
                                                    : environment.projectArg;
    slug = slug.trimmed().toLower();
    if (!isValidSlug(slug))
        slug = QLatin1String(kHomeProjectSlug);
    layout.projectSlug = slug;
    layout.projectDir = layout.isHomeProject() ? layout.homeDir
                                               : join(layout.projectsRoot, slug);

    if (isGitCheckout(layout.projectDir)) {
        layout.rerootAllowed = false;
        layout.rerootBlockedReason =
            QStringLiteral("%1 is still a git checkout. Move the checkout into "
                           "%1/src (tools/forkmesh_layout.py) before application "
                           "data can live beside it.")
                .arg(layout.projectDir);
    } else if (layout.isHomeProject() && environment.xdgSandbox &&
               environment.homeEnv.isEmpty() && environment.projectsDirEnv.isEmpty()) {
        layout.rerootAllowed = false;
        layout.rerootBlockedReason =
            QStringLiteral("XDG_DATA_HOME, XDG_CONFIG_HOME or XDG_CACHE_HOME points "
                           "away from its default, so this run keeps its files there. "
                           "Set FORKMESH_HOME to place the projects layout explicitly.");
    } else {
        layout.rerootAllowed = true;
    }

    // Only the home project inherits the legacy locations. Any other project
    // is born inside the layout and must never fall back to another project's
    // identity or settings.
    const bool home = layout.isHomeProject();
    const bool allowed = layout.rerootAllowed || !home;
    const QString &dir = layout.projectDir;
    const QString none;
    layout.data = pick(join(dir, QStringLiteral("data")),
                       home ? environment.legacyData : none, allowed);
    // QSettings appends ForkMesh/ForkMesh.conf to its root, so the legacy root
    // is the shared ~/.config and only its ForkMesh folder proves content.
    layout.config = pick(join(dir, QStringLiteral("config")),
                         home && !environment.legacyConfig.isEmpty()
                             ? QFileInfo(environment.legacyConfig).absolutePath()
                             : none,
                         allowed, home ? environment.legacyConfig : none,
                         join(dir, QStringLiteral("config/ForkMesh")));
    layout.cache = pick(join(dir, QStringLiteral("cache")),
                        home ? environment.legacyCache : none, allowed);
    // The lock file is what makes the legacy state root live: it cannot move
    // while held, so it is adopted separately (adoptProjectStateRoot) and its
    // absence is what lets the state root resolve into the project.
    layout.state = pick(join(dir, QStringLiteral("state")),
                        home ? environment.legacyState : none, allowed,
                        home ? join(environment.legacyState,
                                    QStringLiteral("forkmesh.lock"))
                             : none);
    // Legacy logs lived directly in ~/.forkmesh (diagnostics/ beneath it).
    layout.logs = pick(join(dir, QStringLiteral("logs")),
                       home ? environment.legacyState : none, allowed,
                       home ? join(environment.legacyState, QStringLiteral("diagnostics"))
                            : none);
    layout.ssh = pick(join(dir, QStringLiteral("ssh")),
                      home ? join(environment.legacyData, QStringLiteral("ssh"))
                           : none,
                      allowed);
    return layout;
}

QVector<Move> planMigration(const Environment &environment, const Layout &layout)
{
    QVector<Move> plan;
    if (!layout.isHomeProject() || !layout.rerootAllowed)
        return plan;
    const QString &dir = layout.projectDir;
    auto directory = [&plan](const QString &label, const QString &from,
                             const QString &to, bool link = true) {
        if (!from.isEmpty() && clean(from) != clean(to))
            plan.append({Move::Kind::Directory, label, clean(from), clean(to), link});
    };
    auto file = [&plan](const QString &label, const QString &from,
                        const QString &to, bool link = true) {
        if (!from.isEmpty() && clean(from) != clean(to))
            plan.append({Move::Kind::File, label, clean(from), clean(to), link});
    };

    // Order matters: data first, then the pieces that are carved out of it.
    directory(QStringLiteral("Application data"), environment.legacyData,
              join(dir, QStringLiteral("data")));
    directory(QStringLiteral("SSH keys for hosts and mirrors"),
              join(dir, QStringLiteral("data/ssh")), join(dir, QStringLiteral("ssh")));
    directory(QStringLiteral("Preferences"), environment.legacyConfig,
              join(dir, QStringLiteral("config/ForkMesh")));
    directory(QStringLiteral("Cache"), environment.legacyCache,
              join(dir, QStringLiteral("cache")));

    const QString &state = environment.legacyState;
    directory(QStringLiteral("Diagnostics journals"),
              join(state, QStringLiteral("diagnostics")),
              join(dir, QStringLiteral("logs/diagnostics")));
    directory(QStringLiteral("Tool logs"), join(state, QStringLiteral("logs")),
              join(dir, QStringLiteral("logs/tools")));
    directory(QStringLiteral("Build toolchains"),
              join(state, QStringLiteral("toolchain")),
              join(dir, QStringLiteral("toolchain")));
    directory(QStringLiteral("Editor bridge drop-box"),
              join(state, QStringLiteral("ide")),
              join(dir, QStringLiteral("state/ide")));
    file(QStringLiteral("Social signal seed"),
         join(state, QStringLiteral("social.seed")),
         join(dir, QStringLiteral("state/social.seed")));

    // ForkMesh-named keys the user keeps in ~/.ssh. Anything not named for
    // ForkMesh stays put: those keys are not ours to move.
    const QDir userSsh(environment.userSshDir);
    const QStringList owned = userSsh.entryList(
        {QStringLiteral("forkmesh_*"), QStringLiteral("id_*_forkmesh"),
         QStringLiteral("id_*_forkmesh.pub")},
        QDir::Files | QDir::Hidden, QDir::Name);
    for (const QString &name : owned) {
        file(QStringLiteral("SSH key %1").arg(name), userSsh.filePath(name),
             join(dir, QStringLiteral("ssh/user/") + name),
             /*link=*/false);
    }
    return plan;
}

QVector<MoveResult> applyMigration(const QVector<Move> &plan, bool dryRun)
{
    QVector<MoveResult> results;
    results.reserve(plan.size());
    // A dry run has to see through the moves it only pretended to make: data/ssh
    // does not exist yet when data itself has not really moved.
    QVector<QPair<QString, QString>> pretended;
    for (const Move &move : plan) {
        MoveResult result;
        result.move = move;
        QString source = move.from;
        if (dryRun && !existsOrLink(source)) {
            for (const auto &earlier : pretended) {
                if (source.startsWith(earlier.second + QLatin1Char('/')))
                    source = earlier.first + source.mid(earlier.second.size());
            }
        }
        const QFileInfo from(source);
        if (from.isSymLink()) {
            result.outcome = MoveResult::Outcome::AlreadyDone;
            result.detail = QStringLiteral("already a link");
            results.append(result);
            continue;
        }
        if (!from.exists()) {
            result.outcome = MoveResult::Outcome::AlreadyDone;
            result.detail = QStringLiteral("nothing to move");
            results.append(result);
            continue;
        }
        if (existsOrLink(move.to)) {
            // Both exist: never merge or overwrite. Whoever resolves paths
            // keeps using the new location; the old one shows up in the
            // storage map for the user to reconcile.
            result.outcome = MoveResult::Outcome::Skipped;
            result.detail = QStringLiteral("destination already exists");
            results.append(result);
            continue;
        }
        if (dryRun) {
            pretended.append({move.from, move.to});
            result.outcome = MoveResult::Outcome::Moved;
            result.detail = QStringLiteral("dry run");
            results.append(result);
            continue;
        }
        const QString parent = QFileInfo(move.to).absolutePath();
        const bool parentIsNew = !QFileInfo::exists(parent);
        if (!QDir().mkpath(parent)) {
            result.outcome = MoveResult::Outcome::Failed;
            result.detail = QStringLiteral("cannot create the destination folder");
            results.append(result);
            continue;
        }
        bool crossDevice = false;
        QString renameError;
        if (!renamePath(move.from, move.to, &crossDevice, &renameError)) {
            result.outcome = crossDevice ? MoveResult::Outcome::Skipped
                                         : MoveResult::Outcome::Failed;
            result.detail = crossDevice
                                ? QStringLiteral("on a different disk; left in place")
                                : renameError;
            results.append(result);
            continue;
        }
        // A folder made to hold key material is the owner's alone.
        if (parentIsNew && (parent.endsWith(QLatin1String("/ssh")) ||
                            parent.contains(QLatin1String("/ssh/")))) {
            QFile::setPermissions(parent, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                              QFileDevice::ExeOwner);
        }
        result.outcome = MoveResult::Outcome::Moved;
        if (move.leaveLink && !QFile::link(move.to, move.from))
            result.detail = QStringLiteral("moved; compatibility link not created");
        results.append(result);
    }
    return results;
}

QString rewriteSshConfig(const QString &text,
                         const QVector<QPair<QString, QString>> &relocations)
{
    if (relocations.isEmpty())
        return text;
    static const QRegularExpression directive(
        QStringLiteral("^(\\s*(?:IdentityFile|UserKnownHostsFile|CertificateFile)"
                       "(?:\\s*=\\s*|\\s+))(\"?)([^\"\\n]*?)(\"?)(\\s*)$"),
        QRegularExpression::CaseInsensitiveOption);
    QStringList lines = text.split(QLatin1Char('\n'));
    for (QString &line : lines) {
        const QRegularExpressionMatch match = directive.match(line);
        if (!match.hasMatch())
            continue;
        const QString value = match.captured(3);
        for (const auto &relocation : relocations) {
            const QString &from = relocation.first;
            const bool whole = value == from;
            const bool beneath = value.startsWith(from + QLatin1Char('/'));
            if (!whole && !beneath)
                continue;
            line = match.captured(1) + match.captured(2) + relocation.second +
                   value.mid(from.size()) + match.captured(4) + match.captured(5);
            break;
        }
    }
    return lines.join(QLatin1Char('\n'));
}

// ~/.ssh/config keeps working after keys move: rewrite in place, keep a
// sibling backup, and never create the file when it does not exist.
bool updateUserSshConfig(const Environment &environment,
                         const QVector<MoveResult> &results)
{
    QVector<QPair<QString, QString>> relocations;
    const QString home = environment.userHome;
    for (const MoveResult &result : results) {
        if (result.outcome != MoveResult::Outcome::Moved || result.move.leaveLink)
            continue;
        relocations.append({result.move.from, result.move.to});
        if (result.move.from.startsWith(home + QLatin1Char('/'))) {
            relocations.append(
                {QStringLiteral("~") + result.move.from.mid(home.size()),
                 result.move.to});
        }
    }
    if (relocations.isEmpty())
        return false;
    const QString configPath = join(environment.userSshDir, QStringLiteral("config"));
    QFile config(configPath);
    if (!config.open(QIODevice::ReadOnly))
        return false;
    const QString before = QString::fromUtf8(config.readAll());
    config.close();
    const QString after = rewriteSshConfig(before, relocations);
    if (after == before)
        return false;
    const QString backup = configPath + QStringLiteral(".before-forkmesh-projects");
    if (!QFileInfo::exists(backup))
        QFile::copy(configPath, backup);
    QFile temporary(configPath + QStringLiteral(".forkmesh-new"));
    if (!temporary.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    temporary.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    temporary.write(after.toUtf8());
    temporary.close();
    bool crossDevice = false;
    QString detail;
    if (!renamePath(temporary.fileName(), configPath, &crossDevice, &detail)) {
        QFile::remove(temporary.fileName());
        return false;
    }
    return true;
}

namespace {


// Owner-only when we create it; an existing folder keeps the permissions its
// owner chose.
void ensurePrivateDir(const QString &path)
{
    if (path.isEmpty() || QFileInfo::exists(path))
        return;
    if (QDir().mkpath(path)) {
        QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                        QFileDevice::ExeOwner);
    }
}

} // namespace

QString repointText(const QString &text,
                    const QVector<QPair<QString, QString>> &relocations)
{
    QString out = text;
    // Longest root first, so data/ssh is not rewritten as a child of data.
    QVector<QPair<QString, QString>> ordered = relocations;
    std::sort(ordered.begin(), ordered.end(), [](const auto &a, const auto &b) {
        return a.first.size() > b.first.size();
    });
    for (const auto &relocation : ordered) {
        if (relocation.first.isEmpty() || relocation.first == relocation.second)
            continue;
        const QRegularExpression pattern(
            QRegularExpression::escape(relocation.first) +
            QStringLiteral("(?=[/\"'\\\\,;\\s]|$)"),
            QRegularExpression::MultilineOption);
        QString replaced;
        replaced.reserve(out.size());
        int last = 0;
        QRegularExpressionMatchIterator matches = pattern.globalMatch(out);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            // Never re-root a path that already sits inside the new root.
            replaced += out.mid(last, match.capturedStart() - last);
            replaced += relocation.second;
            last = match.capturedEnd();
        }
        replaced += out.mid(last);
        out = replaced;
    }
    return out;
}

namespace {

Environment g_environment;

// True when `legacy` no longer holds anything of its own: it is a link into
// the layout, or it is gone while the new location exists.
bool hasMovedTo(const QString &legacy, const QString &modern)
{
    if (legacy.isEmpty() || !QFileInfo::exists(modern))
        return false;
    const QFileInfo info(legacy);
    if (info.isSymLink())
        return clean(info.symLinkTarget()) == clean(modern);
    return !info.exists();
}

} // namespace

int repointSettingsFile()
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    if (!detail::active || !detail::current.isHomeProject() || !detail::current.rerootAllowed)
        return 0;
    const Layout &layout = detail::current;
    const Environment &environment = g_environment;
    const QString &dir = layout.projectDir;
    QVector<QPair<QString, QString>> relocations;
    auto consider = [&relocations](const QString &legacy, const QString &modern) {
        if (hasMovedTo(legacy, modern))
            relocations.append({clean(legacy), clean(modern)});
    };
    consider(environment.legacyData, join(dir, QStringLiteral("data")));
    consider(environment.legacyCache, join(dir, QStringLiteral("cache")));
    consider(environment.legacyConfig, join(dir, QStringLiteral("config/ForkMesh")));
    consider(join(environment.legacyState, QStringLiteral("diagnostics")),
             join(dir, QStringLiteral("logs/diagnostics")));
    consider(join(environment.legacyState, QStringLiteral("toolchain")),
             join(dir, QStringLiteral("toolchain")));
    // data/ssh was carved out into ssh/; both spellings of the old place move.
    if (QFileInfo(join(dir, QStringLiteral("data/ssh"))).isSymLink()) {
        relocations.append({join(environment.legacyData, QStringLiteral("ssh")),
                            join(dir, QStringLiteral("ssh"))});
        relocations.append({join(dir, QStringLiteral("data/ssh")),
                            join(dir, QStringLiteral("ssh"))});
    }
    if (relocations.isEmpty())
        return 0;

    const QString path = join(layout.config, QStringLiteral("ForkMesh/ForkMesh.conf"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return 0;
    const QByteArray raw = file.readAll();
    file.close();
    // Cheap exit for every start after the first: nothing names a legacy root.
    bool mentioned = false;
    for (const auto &relocation : relocations)
        mentioned = mentioned || raw.contains(relocation.first.toUtf8());
    if (!mentioned)
        return 0;
    const QString before = QString::fromUtf8(raw);
    const QString after = repointText(before, relocations);
    if (after == before)
        return 0;
    int changed = 0;
    for (const auto &relocation : relocations)
        changed += before.count(relocation.first) - after.count(relocation.first);

    const QString backup = path + QStringLiteral(".before-path-repoint");
    if (!QFileInfo::exists(backup))
        QFile::copy(path, backup);
    QFile temporary(path + QStringLiteral(".forkmesh-new"));
    if (!temporary.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return 0;
    temporary.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    temporary.write(after.toUtf8());
    temporary.close();
    bool crossDevice = false;
    QString detail;
    if (!renamePath(temporary.fileName(), path, &crossDevice, &detail)) {
        QFile::remove(temporary.fileName());
        return 0;
    }
    return qMax(changed, 1);
#else
    return 0;
#endif
}

namespace {

bool g_createFolders = true;

void applyLayout(const Layout &layout)
{
    detail::current = layout;
    detail::active = true;

    if (layout.rerootAllowed && g_createFolders) {
        ensurePrivateDir(layout.projectDir);
        for (const QString &dir : {layout.data, layout.config, layout.cache,
                                   layout.state, layout.logs, layout.ssh})
            ensurePrivateDir(dir);
    }

    // QSettings() is default-constructed in several hundred places. Pointing
    // the native format at the config root re-homes all of them at once, and
    // with them every "folder of the settings file" derivation.
    QSettings::setPath(QSettings::NativeFormat, QSettings::UserScope, layout.config);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, layout.config);
#if !defined(Q_OS_UNIX) || defined(Q_OS_MACOS)
    // The native store (registry, plist) is per user, not per folder. Any
    // project other than home keeps its settings in a file of its own.
    if (!layout.isHomeProject())
        QSettings::setDefaultFormat(QSettings::IniFormat);
#endif

    // Children re-derive a blocked layout themselves (the checkout is still
    // there, the XDG sandbox is inherited). Exporting FORKMESH_HOME would read
    // to them as an explicit placement and defeat exactly that.
    if (layout.rerootAllowed) {
        qputenv("FORKMESH_HOME", QFile::encodeName(layout.homeDir));
        qputenv("FORKMESH_PROJECTS_DIR", QFile::encodeName(layout.projectsRoot));
    }
    qputenv("FORKMESH_PROJECT", layout.projectSlug.toUtf8());
    qputenv("FORKMESH_PROJECT_DIR", QFile::encodeName(layout.projectDir));
    qputenv("FORKMESH_DATA_DIR", QFile::encodeName(layout.data));
    qputenv("FORKMESH_CONFIG_DIR", QFile::encodeName(layout.config));
}

} // namespace

void activate(const QStringList &arguments, bool createFolders)
{
    g_createFolders = createFolders;
    g_environment = Environment::fromProcess(arguments);
    applyLayout(resolve(g_environment));
}

QVector<MoveResult> migrateNow(bool dryRun)
{
    if (!detail::active)
        return {};
    const QVector<MoveResult> results =
        applyMigration(planMigration(g_environment, detail::current), dryRun);
    if (dryRun)
        return results;
    updateUserSshConfig(g_environment, results);
    // Roots now resolve to where they moved. The state root stays legacy for
    // as long as its lock file exists; see adoptProjectStateRoot().
    applyLayout(resolve(g_environment));
    return results;
}

QString adoptProjectStateRoot()
{
    if (!detail::active || !detail::current.rerootAllowed)
        return QString();
    const QString modern = join(detail::current.projectDir, QStringLiteral("state"));
    if (clean(detail::current.state) == modern)
        return QString();
    const QString legacyLock =
        join(detail::current.state, QStringLiteral("forkmesh.lock"));
    Layout layout = detail::current;
    layout.state = modern;
    applyLayout(layout);
    return legacyLock;
}

QString configRoot()
{
    if (detail::active)
        return detail::current.config;
    const QString folder = settingsDir();
    return folder.isEmpty()
               ? QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)
               : QFileInfo(folder).absolutePath();
}

QString projectsRoot()
{
    return detail::active ? detail::current.projectsRoot
                    : resolve(Environment::fromProcess({})).projectsRoot;
}

QString homeDir()
{
    return detail::active ? detail::current.homeDir : resolve(Environment::fromProcess({})).homeDir;
}

QString projectDir() { return detail::active ? detail::current.projectDir : homeDir(); }

QString projectSlug()
{
    return detail::active ? detail::current.projectSlug : QLatin1String(kHomeProjectSlug);
}

QString earlyMainLogPath(int argc, char **argv)
{
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
    QStringList arguments;
    arguments.reserve(argc);
    for (int i = 0; i < argc; ++i)
        arguments.append(QString::fromLocal8Bit(argv[i]));
    const Layout early = resolve(Environment::fromProcess(arguments));
    return join(early.data, QStringLiteral("network_log.txt"));
#else
    Q_UNUSED(argc);
    Q_UNUSED(argv);
    return QString();
#endif
}

namespace {

// The account's home as the system knows it, not as $HOME claims: a test or
// benchmark that points HOME at a scratch folder is still the same user in
// the same socket namespace.
QString accountHome()
{
#if defined(Q_OS_UNIX)
    if (const struct passwd *entry = getpwuid(getuid())) {
        if (entry->pw_dir && *entry->pw_dir)
            return clean(QFile::decodeName(entry->pw_dir));
    }
#endif
    return clean(QDir::homePath());
}

} // namespace

QString singleInstanceServerName()
{
    // The socket namespace is per user, not per folder, so the name carries
    // everything that tells two instances apart: the project, and any state
    // folder that is not this account's default one. A sandboxed profile can
    // therefore never take over the real instance's activation socket.
    QString name = QStringLiteral("forkmesh-single-instance");
    const QString slug = projectSlug();
    if (slug != QLatin1String(kHomeProjectSlug))
        name += QLatin1Char('-') + slug;
    const QString state = clean(stateRoot());
    const QString home = accountHome();
    const bool defaultPlace =
        state == join(home, QStringLiteral(".forkmesh")) ||
        state == join(home, QStringLiteral("projects/") + slug + QStringLiteral("/state"));
    if (!defaultPlace)
        name += QLatin1Char('-') +
                QString::fromLatin1(QCryptographicHash::hash(state.toUtf8(),
                                                             QCryptographicHash::Sha256)
                                        .toHex()
                                        .left(16));
    return name;
}

// --- storage map -------------------------------------------------------------

QVector<Location> legacyLocations(const Environment &environment)
{
    const QString &home = environment.userHome;
    const QString dataHome = QFileInfo(QFileInfo(environment.legacyData).absolutePath())
                                 .absolutePath();                     // ~/.local/share
    const QString configHome = environment.legacyConfig.isEmpty()
                                   ? QString()
                                   : QFileInfo(environment.legacyConfig).absolutePath();
    const QString cacheHome = QFileInfo(QFileInfo(environment.legacyCache).absolutePath())
                                  .absolutePath();                    // ~/.cache
    const QString stateHome = join(home, QStringLiteral(".local/state"));

    QVector<Location> found;
    auto consider = [&found](const QString &id, const QString &label,
                             const QString &path, const QString &hint) {
        const QFileInfo info(path);
        if (!info.exists() && !info.isSymLink())
            return;
        Location location;
        location.id = id;
        location.label = label;
        location.path = clean(path);
        location.hint = hint;
        location.scope = Location::Scope::Legacy;
        location.isCompatLink = info.isSymLink();
        found.append(location);
    };

    consider(QStringLiteral("legacy-data"), QStringLiteral("Application data (old location)"),
             environment.legacyData,
             QStringLiteral("Where application data lived before the projects layout."));
    consider(QStringLiteral("legacy-config"), QStringLiteral("Preferences (old location)"),
             environment.legacyConfig,
             QStringLiteral("Where the settings file lived before the projects layout."));
    consider(QStringLiteral("legacy-cache"), QStringLiteral("Cache (old location)"),
             environment.legacyCache, QStringLiteral("Disposable cache."));
    consider(QStringLiteral("legacy-state"), QStringLiteral("~/.forkmesh"),
             environment.legacyState,
             QStringLiteral("Lock file, diagnostics journals and build toolchains."));

    // Everything else named for ForkMesh in the usual hidden folders. Listed
    // by pattern so a location nobody catalogued still shows up on the map.
    const struct {
        QString base;
        QStringList patterns;
        QString hint;
    } sweeps[] = {
        {dataHome, {QStringLiteral("forkmesh*"), QStringLiteral("ForkMesh?*")},
         QStringLiteral("Installer checkout, toolchains and tool data.")},
        {configHome, {QStringLiteral("forkmesh*"), QStringLiteral("ForkMesh?*")},
         QStringLiteral("Tool configuration and signing keys.")},
        {cacheHome, {QStringLiteral("forkmesh*")},
         QStringLiteral("Build trees, release scratch space and toolchains.")},
        {stateHome, {QStringLiteral("forkmesh*")},
         QStringLiteral("Tool state and observation journals.")},
        {home, {QStringLiteral(".forkmesh_*"), QStringLiteral(".forkmesh-*"),
                QStringLiteral("forkmesh-*")},
         QStringLiteral("Tokens, keys and worktrees kept directly in the home folder.")},
        {environment.userSshDir,
         {QStringLiteral("forkmesh_*"), QStringLiteral("id_*_forkmesh*")},
         QStringLiteral("SSH key or known-hosts file named for ForkMesh.")},
    };
    for (const auto &sweep : sweeps) {
        if (sweep.base.isEmpty())   // QDir("") would sweep the working directory
            continue;
        const QFileInfoList entries = QDir(sweep.base).entryInfoList(
            sweep.patterns, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
                                QDir::System,
            QDir::Name);
        for (const QFileInfo &entry : entries) {
            const QString path = clean(entry.absoluteFilePath());
            bool known = false;
            for (const Location &existing : found)
                known = known || existing.path == path;
            if (known)
                continue;
            consider(QStringLiteral("legacy:") + path, entry.fileName(), path, sweep.hint);
        }
    }
    return found;
}

QVector<Location> storageMap(const Environment &environment, const Layout &layout,
                             const QVector<Location> &extra)
{
    QVector<Location> map;
    auto add = [&map](const QString &id, const QString &label, const QString &path,
                      const QString &hint, Location::Scope scope, bool inBackup) {
        if (path.isEmpty())
            return;
        const QString absolute = clean(path);
        for (const Location &existing : map)
            if (existing.path == absolute)
                return;
        Location location;
        location.id = id;
        location.label = label;
        location.path = absolute;
        location.hint = hint;
        location.scope = scope;
        location.inBackup = inBackup;
        location.isCompatLink = QFileInfo(absolute).isSymLink();
        map.append(location);
    };
    using Scope = Location::Scope;

    add(QStringLiteral("project"), QStringLiteral("Project folder"), layout.projectDir,
        QStringLiteral("Everything this project keeps on disk lives beneath this folder."),
        Scope::Project, false);
    add(QStringLiteral("data"), QStringLiteral("Application data"), layout.data,
        QStringLiteral("Your identity key, account, chat history, agents, mirrors and "
                       "every other local store."),
        Scope::Project, true);
    add(QStringLiteral("config"), QStringLiteral("Preferences"), layout.config,
        QStringLiteral("The settings file: theme, access tokens, notification toggles "
                       "and shared action variables."),
        Scope::Project, true);
    add(QStringLiteral("ssh"), QStringLiteral("SSH keys"), layout.ssh,
        QStringLiteral("Keys and known hosts ForkMesh uses to reach hosts and mirrors."),
        Scope::Project, true);
    add(QStringLiteral("logs"), QStringLiteral("Logs and diagnostics"), layout.logs,
        QStringLiteral("Stall and action journals, plus logs written by ForkMesh tools."),
        Scope::Project, false);
    add(QStringLiteral("state"), QStringLiteral("Runtime state"), layout.state,
        QStringLiteral("The single-instance lock and editor bridge drop-box."),
        Scope::Project, false);
    add(QStringLiteral("cache"), QStringLiteral("Cache"), layout.cache,
        QStringLiteral("Regenerated automatically; always safe to delete."),
        Scope::Project, false);
    for (const Location &location : extra) {
        add(location.id, location.label, location.path, location.hint, location.scope,
            location.inBackup);
    }

    add(QStringLiteral("source"), QStringLiteral("ForkMesh source"), layout.sourceDir(),
        QStringLiteral("The checkout ForkMesh builds and updates itself from."),
        Scope::Shared, false);
    add(QStringLiteral("toolchain"), QStringLiteral("Build toolchains"),
        layout.toolchainDir(),
        QStringLiteral("Android, Qt and packaging toolchains. Re-downloadable."),
        Scope::Shared, false);
    add(QStringLiteral("builds"), QStringLiteral("Build trees"), layout.buildsDir(),
        QStringLiteral("Compiler output for agent sessions and releases. Disposable."),
        Scope::Shared, false);
    add(QStringLiteral("worktrees"), QStringLiteral("Worktrees"), layout.worktreesDir(),
        QStringLiteral("Git worktrees for agent sessions and manual branches."),
        Scope::Shared, false);
    add(QStringLiteral("release"), QStringLiteral("Release workspace"),
        layout.releaseDir(),
        QStringLiteral("Scratch space for building and signing releases."),
        Scope::Shared, false);
    add(QStringLiteral("bin"), QStringLiteral("Installed binaries"), layout.binDir(),
        QStringLiteral("The ForkMesh binary and helper programs."), Scope::Shared, false);

    // Sibling projects, so the map accounts for the whole projects root.
    const QFileInfoList siblings =
        QDir(layout.projectsRoot)
            .entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &sibling : siblings) {
        const QString path = clean(sibling.absoluteFilePath());
        if (path == clean(layout.projectDir) || path == clean(layout.homeDir))
            continue;
        if (!QFileInfo::exists(join(path, QLatin1String(kProjectFileName))))
            continue;
        add(QStringLiteral("project:") + sibling.fileName(),
            QStringLiteral("Project \xE2\x80\x9C%1\xE2\x80\x9D").arg(sibling.fileName()), path,
            QStringLiteral("Another project. Its files are isolated from this one."),
            Scope::Foreign, false);
    }
    if (!layout.isHomeProject()) {
        add(QStringLiteral("home"), QStringLiteral("ForkMesh home"), layout.homeDir,
            QStringLiteral("The ForkMesh project: source, toolchains and its own data."),
            Scope::Foreign, false);
    }

    for (const Location &legacy : legacyLocations(environment)) {
        bool listed = false;
        for (const Location &existing : map)
            listed = listed || existing.path == legacy.path;
        if (!listed)
            map.append(legacy);
    }
    return map;
}

} // namespace forkmesh::paths
