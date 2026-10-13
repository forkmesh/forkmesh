#include "SafeDelete.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh::safedelete;

namespace {

int g_failures = 0;
int g_checks = 0;

void check(bool ok, const char *what)
{
    ++g_checks;
    if (ok) {
        std::printf("PASS %s\n", what);
    } else {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

void writeFile(const QString &path, const QByteArray &content)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        file.write(content);
}

bool refused(const QString &target, const QString &parent, Allow allow = Allow::Tree)
{
    return !refusal(target, parent, allow).isEmpty();
}

}

int main(int argc, char **argv)
{
    QTemporaryDir sandbox;
    if (!sandbox.isValid())
        return 2;
    const QString root = QFileInfo(sandbox.path()).canonicalFilePath();
    const QString home = root + QStringLiteral("/home");
    const QString work = root + QStringLiteral("/work");
    QDir().mkpath(home);
    QDir().mkpath(work);
    qputenv("HOME", home.toLocal8Bit());
    for (const char *variable : {"FORKMESH_HOME", "FORKMESH_PROJECTS_DIR", "FORKMESH_DATA_DIR",
                                 "FORKMESH_CONFIG_DIR", "FORKMESH_PROJECT_DIR",
                                 "XDG_DATA_HOME", "XDG_CONFIG_HOME"})
        qunsetenv(variable);
    QCoreApplication app(argc, argv);

    check(refused(QString(), work), "empty path refused");
    check(refused(QStringLiteral("   "), work), "blank path refused");
    check(refused(QStringLiteral("/"), QStringLiteral("/")), "root refused");
    check(refused(home, root), "HOME refused");
    check(refused(root, QFileInfo(root).absolutePath()), "ancestor of HOME refused");

    const QString scratch = work + QStringLiteral("/scratch");
    writeFile(scratch + QStringLiteral("/a/b.txt"), "x");
    check(refused(scratch, QString()), "missing expected parent refused");
    check(refused(scratch, work + QStringLiteral("/elsewhere")), "outside expected parent refused");
    check(refused(work, work), "the expected parent itself refused");
    check(refused(work + QStringLiteral("/../work/scratch/../../"), work), "dot-dot escape refused");

    const QString checkout = work + QStringLiteral("/checkout");
    QDir().mkpath(checkout + QStringLiteral("/.git/objects"));
    check(refused(checkout, work), "checkout with .git folder refused");
    check(refused(checkout, work, Allow::OwnWorktree), "primary checkout refused even as worktree");
    check(refused(checkout + QStringLiteral("/.git"), checkout), ".git folder refused");
    check(refused(checkout + QStringLiteral("/.git/objects"), checkout + QStringLiteral("/.git")),
          ".git/objects refused");

    const QString linked = work + QStringLiteral("/linked");
    writeFile(linked + QStringLiteral("/.git"), "gitdir: /elsewhere/.git/worktrees/linked\n");
    check(refused(linked, work), "linked worktree refused without OwnWorktree");
    check(!refused(linked, work, Allow::OwnWorktree), "own linked worktree allowed");

    const QString projects = home + QStringLiteral("/projects");
    const QString project = projects + QStringLiteral("/forkmesh");
    for (const char *sub : {"src", "data", "config", "ssh", "state", "logs"})
        QDir().mkpath(project + QLatin1Char('/') + QLatin1String(sub));
    QDir().mkpath(projects + QStringLiteral("/forkmesh-backup/data"));
    QDir().mkpath(projects + QStringLiteral("/other/data"));
    check(refused(projects, home), "projects root refused");
    check(refused(project, projects), "home project refused");
    check(refused(projects + QStringLiteral("/other"), projects), "other project refused");
    for (const char *sub : {"src", "data", "config", "ssh", "state", "logs"})
        check(refused(project + QLatin1Char('/') + QLatin1String(sub), project),
              qPrintable(QStringLiteral("project %1 refused").arg(QLatin1String(sub))));
    check(refused(project + QStringLiteral("/src/desktop"), project + QStringLiteral("/src")),
          "folder inside src refused");
    check(!refused(project + QStringLiteral("/src/desktop/build"),
                   project + QStringLiteral("/src/desktop")),
          "build folder inside src allowed");
    check(refused(project + QStringLiteral("/data/identity"), project + QStringLiteral("/data")),
          "top-level data folder refused");
    check(!refused(project + QStringLiteral("/data/mirrors/repo"),
                   project + QStringLiteral("/data/mirrors")),
          "nested data folder allowed");
    check(refused(project + QStringLiteral("/ssh/fleet"), project + QStringLiteral("/ssh")),
          "ssh folder refused");
    check(!refused(project + QStringLiteral("/builds/x"), project + QStringLiteral("/builds")),
          "shared builds folder allowed");
    check(refused(project + QStringLiteral("/notes"), project), "unknown project folder refused");
    check(!refused(project + QStringLiteral("/src/.forkmesh/issues/7"),
                   project + QStringLiteral("/src/.forkmesh/issues")),
          "issue metadata inside src allowed");
    check(refused(project + QStringLiteral("/src/.forkmesh"), project + QStringLiteral("/src")),
          "metadata root inside src refused");
    check(!refused(project + QStringLiteral("/toolchain-linux/x"),
                   project + QStringLiteral("/toolchain-linux")),
          "toolchain-linux allowed");
    check(refused(projects + QStringLiteral("/forkmesh-backup/data/x"),
                  projects + QStringLiteral("/forkmesh-backup/data")),
          "backup project refused");
    check(refused(home + QStringLiteral("/.forkmesh"), home), "legacy state root refused");
    check(refused(home + QStringLiteral("/.config/ForkMesh"), home + QStringLiteral("/.config")),
          "legacy config root refused");

    const QString custom = root + QStringLiteral("/custom-home");
    QDir().mkpath(custom + QStringLiteral("/data/x"));
    qputenv("FORKMESH_HOME", custom.toLocal8Bit());
    check(refused(custom + QStringLiteral("/data"), custom), "FORKMESH_HOME data refused");
    check(refused(custom, root), "FORKMESH_HOME refused");
    qunsetenv("FORKMESH_HOME");

    QString why;
    check(!removeTree(checkout, work, &why) && QFileInfo::exists(checkout) && !why.isEmpty(),
          "removeTree leaves a refused checkout in place");
    check(removeTree(scratch, work) && !QFileInfo::exists(scratch), "removeTree removes scratch");
    check(removeTree(work + QStringLiteral("/never-existed"), work), "missing target is fine");
    check(removeTree(linked, work, nullptr, Allow::OwnWorktree) && !QFileInfo::exists(linked),
          "removeTree removes an own worktree");

    const QString aside = moveAside(checkout, QStringLiteral("replaced"));
    check(!aside.isEmpty() && QFileInfo::exists(aside + QStringLiteral("/.git/objects")) &&
              !QFileInfo::exists(checkout),
          "moveAside keeps the checkout under a new name");

    std::printf("%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
