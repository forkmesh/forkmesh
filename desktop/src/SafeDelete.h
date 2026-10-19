#pragma once

#include "AppPaths.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QtGlobal>

#if defined(Q_OS_UNIX)
#include <pwd.h>
#include <unistd.h>
#endif

namespace forkmesh::safedelete {

enum class Allow {
    Tree,
    OwnWorktree,
};

enum class RootKind {
    Projects,
    Project,
    Strict,
    Owned,
    Frozen,
};

namespace detail {

inline QString clean(const QString &path)
{
    if (path.trimmed().isEmpty())
        return QString();
    QString absolute = QDir::cleanPath(QDir(path).absolutePath());
    if (absolute == QLatin1String("/"))
        return absolute;
    const QFileInfo info(absolute);
    const QString parent = QFileInfo(info.absolutePath()).canonicalFilePath();
    if (parent.isEmpty())
        return absolute;
    return QDir::cleanPath(parent + QLatin1Char('/') + info.fileName());
}

inline bool inside(const QString &child, const QString &parent)
{
    if (child == parent || parent.isEmpty())
        return false;
    const QString prefix =
        parent.endsWith(QLatin1Char('/')) ? parent : parent + QLatin1Char('/');
    return child.startsWith(prefix);
}

inline bool within(const QString &child, const QString &parent)
{
    return child == parent || inside(child, parent);
}

inline bool disposable(const QString &name)
{
    static const QStringList names{
        QStringLiteral(".cache"),    QStringLiteral(".pytest_cache"),
        QStringLiteral(".tmp"),      QStringLiteral(".worktrees"),
        QStringLiteral("__pycache__"), QStringLiteral("build"),
        QStringLiteral("builds"),    QStringLiteral("cache"),
        QStringLiteral("caches"),    QStringLiteral("dist"),
        QStringLiteral("node_modules"), QStringLiteral("out"),
        QStringLiteral("temp"),      QStringLiteral("tmp"),
        QStringLiteral("worktrees"),
    };
    return names.contains(name) || name.startsWith(QLatin1String("build-")) ||
           name.startsWith(QLatin1String("build_")) ||
           name.startsWith(QLatin1String("tmp-")) ||
           name.startsWith(QLatin1String("tmp."));
}

inline bool sharedProjectFolder(const QString &name)
{
    static const QStringList names{
        QStringLiteral("bin"),       QStringLiteral("builds"),
        QStringLiteral("cache"),     QStringLiteral("release"),
        QStringLiteral("tmp"),       QStringLiteral("toolchain"),
        QStringLiteral("toolchain-windows"), QStringLiteral("worktrees"),
    };
    return names.contains(name) || name.startsWith(QLatin1String("toolchain-"));
}

inline QString accountHome()
{
#if defined(Q_OS_UNIX)
    if (const passwd *entry = ::getpwuid(::getuid()))
        return QString::fromLocal8Bit(entry->pw_dir);
#endif
    return QString();
}

inline QStringList homes()
{
    QStringList result;
    for (const QString &candidate :
         {qEnvironmentVariable("HOME"), QDir::homePath(), accountHome()}) {
        const QString cleaned = clean(candidate);
        if (!cleaned.isEmpty() && cleaned != QLatin1String("/") &&
            !result.contains(cleaned))
            result << cleaned;
    }
    return result;
}

inline QVector<QPair<QString, RootKind>> protectedRoots()
{
    QVector<QPair<QString, RootKind>> roots;
    auto add = [&roots](const QString &path, RootKind kind) {
        const QString cleaned = clean(path);
        if (cleaned.isEmpty() || cleaned == QLatin1String("/"))
            return;
        for (const auto &root : roots) {
            if (root.first == cleaned && root.second == kind)
                return;
        }
        roots.append({cleaned, kind});
    };
    static const QStringList projectRoots{
        QStringLiteral("src"),   QStringLiteral("data"), QStringLiteral("config"),
        QStringLiteral("ssh"),   QStringLiteral("state"), QStringLiteral("logs"),
    };
    auto addProject = [&](const QString &project) {
        if (QFileInfo(project).fileName() == QLatin1String("forkmesh-backup")) {
            add(project, RootKind::Frozen);
            return;
        }
        add(project, RootKind::Project);
        for (const QString &sub : projectRoots) {
            const bool strict =
                sub == QLatin1String("src") || sub == QLatin1String("ssh");
            add(project + QLatin1Char('/') + sub,
                strict ? RootKind::Strict : RootKind::Owned);
        }
    };
    QStringList projectsDirs;
    for (const QString &home : homes()) {
        projectsDirs << home + QStringLiteral("/projects");
        add(home + QStringLiteral("/.ssh"), RootKind::Strict);
        add(home + QStringLiteral("/.forkmesh"), RootKind::Owned);
        add(home + QStringLiteral("/.config/ForkMesh"), RootKind::Owned);
        add(home + QStringLiteral("/.local/share/ForkMesh"), RootKind::Owned);
        add(home + QStringLiteral("/.local/share/forkmesh"), RootKind::Owned);
    }
    for (const char *variable : {"XDG_DATA_HOME", "XDG_CONFIG_HOME"}) {
        const QString value = qEnvironmentVariable(variable).trimmed();
        if (!value.isEmpty())
            add(value + QStringLiteral("/ForkMesh"), RootKind::Owned);
    }
    const QString projectsEnv = qEnvironmentVariable("FORKMESH_PROJECTS_DIR").trimmed();
    if (!projectsEnv.isEmpty())
        projectsDirs << projectsEnv;
    for (const char *variable : {"FORKMESH_HOME", "FORKMESH_PROJECT_DIR"}) {
        const QString value = qEnvironmentVariable(variable).trimmed();
        if (!value.isEmpty())
            addProject(value);
    }
    for (const char *variable : {"FORKMESH_DATA_DIR", "FORKMESH_CONFIG_DIR"}) {
        const QString value = qEnvironmentVariable(variable).trimmed();
        if (!value.isEmpty())
            add(value, RootKind::Owned);
    }
    if (paths::isActive()) {
        const paths::Layout &layout = paths::layout();
        projectsDirs << layout.projectsRoot;
        addProject(layout.homeDir);
        addProject(layout.projectDir);
        add(layout.homeDir + QStringLiteral("/src"), RootKind::Strict);
        for (const QString &owned : {layout.data, layout.config, layout.state, layout.logs})
            add(owned, RootKind::Owned);
        add(layout.ssh, RootKind::Strict);
    }
    for (const QString &projects : projectsDirs) {
        if (projects.trimmed().isEmpty())
            continue;
        add(projects, RootKind::Projects);
        const QString cleaned = clean(projects);
        QStringList children = QDir(cleaned).entryList(QDir::Dirs | QDir::Hidden |
                                                       QDir::NoDotAndDotDot);
        for (const QString &fixed :
             {QStringLiteral("forkmesh"), QStringLiteral("forkmesh-backup")}) {
            if (!children.contains(fixed))
                children << fixed;
        }
        for (const QString &child : children)
            addProject(cleaned + QLatin1Char('/') + child);
    }
    return roots;
}

inline QString gitEntry(const QString &path)
{
    const QFileInfo marker(path + QStringLiteral("/.git"));
    if (marker.isSymLink() || marker.isFile())
        return QStringLiteral("file");
    if (marker.isDir())
        return QStringLiteral("dir");
    return QString();
}

inline bool gitHistory(const QStringList &parts)
{
    if (parts.isEmpty())
        return false;
    if (parts.last() == QLatin1String(".git"))
        return true;
    const qsizetype index = parts.indexOf(QStringLiteral(".git"));
    if (index < 0 || index == parts.size() - 1)
        return false;
    static const QStringList history{
        QStringLiteral("objects"), QStringLiteral("refs"), QStringLiteral("logs"),
        QStringLiteral("HEAD"),    QStringLiteral("packed-refs"),
    };
    return history.contains(parts.at(index + 1));
}

} // namespace detail

inline QString refusal(const QString &target, const QString &expectedParent,
                       Allow allow = Allow::Tree)
{
    if (target.trimmed().isEmpty())
        return QStringLiteral("the path is empty");
    const QString path = detail::clean(target);
    if (path == QLatin1String("/"))
        return QStringLiteral("the path is /");
    for (const QString &home : detail::homes()) {
        if (detail::within(home, path))
            return QStringLiteral("%1 is HOME (%2) or contains it").arg(path, home);
    }
    if (expectedParent.trimmed().isEmpty())
        return QStringLiteral("no expected parent folder was given");
    const QString base = detail::clean(expectedParent);
    if (base == QLatin1String("/"))
        return QStringLiteral("the expected parent folder is /");
    if (!detail::inside(path, base))
        return QStringLiteral("%1 is not inside the expected parent %2").arg(path, base);
    if (detail::gitHistory(path.split(QLatin1Char('/'))))
        return QStringLiteral("%1 is git history (.git)").arg(path);
    const QString entry = detail::gitEntry(path);
    if (entry == QLatin1String("dir"))
        return QStringLiteral("%1 is a git checkout (it has a .git folder)").arg(path);
    if (entry == QLatin1String("file") && allow != Allow::OwnWorktree)
        return QStringLiteral("%1 is a git worktree; only the caller that created it "
                              "may remove it")
            .arg(path);
    const auto roots = detail::protectedRoots();
    for (const auto &root : roots) {
        if (detail::within(root.first, path))
            return QStringLiteral("%1 is or contains protected ForkMesh folder %2")
                .arg(path, root.first);
    }
    const QPair<QString, RootKind> *innermost = nullptr;
    for (const auto &root : roots) {
        if (detail::inside(path, root.first) &&
            (!innermost || root.first.size() > innermost->first.size()))
            innermost = &root;
    }
    if (!innermost)
        return QString();
    const QString &root = innermost->first;
    const QStringList rel = path.mid(root.size() + 1).split(QLatin1Char('/'));
    bool disposable = false;
    for (const QString &part : rel)
        disposable = disposable || detail::disposable(part);
    disposable = disposable || rel.mid(0, rel.size() - 1).contains(QStringLiteral(".forkmesh"));
    switch (innermost->second) {
    case RootKind::Frozen:
        return QStringLiteral("%1 is inside %2, which is never erased").arg(path, root);
    case RootKind::Project:
        if (!detail::sharedProjectFolder(rel.first()) && !disposable)
            return QStringLiteral("%1 is inside project folder %2 but not in a cache, "
                                  "build or tmp folder")
                .arg(path, root);
        break;
    case RootKind::Strict:
        if (!disposable)
            return QStringLiteral("%1 is inside protected folder %2 but not in a cache, "
                                  "build or tmp folder")
                .arg(path, root);
        break;
    case RootKind::Owned:
        if (rel.size() < 2 && !disposable)
            return QStringLiteral("%1 is a top-level folder of protected ForkMesh "
                                  "folder %2")
                .arg(path, root);
        break;
    case RootKind::Projects:
        if (!disposable)
            return QStringLiteral("%1 is inside the projects root %2").arg(path, root);
        break;
    }
    return QString();
}

inline bool removeTree(const QString &target, const QString &expectedParent,
                       QString *why = nullptr, Allow allow = Allow::Tree)
{
    const QString reason = refusal(target, expectedParent, allow);
    if (!reason.isEmpty()) {
        qWarning("Refusing to delete: %s", qUtf8Printable(reason));
        if (why)
            *why = reason;
        return false;
    }
    const QString path = detail::clean(target);
    const QFileInfo info(path);
    if (info.isSymLink() || info.isFile())
        return QFile::remove(path);
    if (!info.exists())
        return true;
    const bool removed = QDir(path).removeRecursively();
    if (!removed && why)
        *why = QStringLiteral("could not remove everything in %1").arg(path);
    return removed;
}

inline QString moveAside(const QString &target, const QString &label = QStringLiteral("replaced"))
{
    const QString path = detail::clean(target);
    if (path.isEmpty() || path == QLatin1String("/"))
        return QString();
    const QFileInfo info(path);
    const QString stamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString candidate = QStringLiteral("%1/%2.%3-%4")
                            .arg(info.absolutePath(), info.fileName(), label, stamp);
    for (int counter = 2; QFileInfo::exists(candidate) || QFileInfo(candidate).isSymLink();
         ++counter)
        candidate = QStringLiteral("%1/%2.%3-%4-%5")
                        .arg(info.absolutePath(), info.fileName(), label, stamp)
                        .arg(counter);
    return QDir().rename(path, candidate) ? candidate : QString();
}

} // namespace forkmesh::safedelete
