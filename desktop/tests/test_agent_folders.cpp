#include "AgentFolders.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>

static int failures = 0;

static void check(bool ok, const char *what)
{
    if (!ok) {
        ++failures;
        std::cerr << "FAIL: " << what << '\n';
    }
}

static QString describe(const QList<AgentFolderRow> &rows,
                        const QStringList &members)
{
    QStringList parts;
    for (const AgentFolderRow &row : rows)
        parts << (row.header() ? QStringLiteral("[") + row.folderId +
                                     QStringLiteral("]")
                               : members.value(row.member));
    return parts.join(QLatin1Char(' '));
}

int main(int argc, char **argv)
{
    QTemporaryDir home;
    qputenv("HOME", home.path().toUtf8());
    qputenv("XDG_CONFIG_HOME", home.path().toUtf8());
    qputenv("XDG_DATA_HOME", home.path().toUtf8());
    qputenv("XDG_CACHE_HOME", home.path().toUtf8());
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshFolderTest"));
    QCoreApplication::setApplicationName(QStringLiteral("AgentFolders"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, home.path());

    const QString repo = AgentFolders::repoKey(QStringLiteral("me"),
                                               QStringLiteral("app"));
    const QString other = AgentFolders::repoKey(QStringLiteral("me"),
                                                QStringLiteral("site"));
    AgentFolders folders;
    check(folders.create(repo, QStringLiteral("   ")).isEmpty(),
          "a blank name makes no folder");
    const QString ui = folders.create(repo, QStringLiteral(" UI "));
    const QString infra = folders.create(repo, QStringLiteral("Infra"));
    check(!ui.isEmpty() && !infra.isEmpty() && ui != infra,
          "each folder gets its own id");
    check(folders.folder(repo, ui) && folders.folder(repo, ui)->name == "UI",
          "folder names are trimmed");
    check(folders.folders(other).isEmpty(), "folders belong to one repository");

    const QStringList members{QStringLiteral("1"), QStringLiteral("2"),
                              QStringLiteral("3"), QStringLiteral("4")};
    check(folders.assign(repo, QStringLiteral("2"), infra), "assign to Infra");
    check(folders.assign(repo, QStringLiteral("4"), ui), "assign to UI");
    check(!folders.assign(repo, QStringLiteral("4"), ui),
          "assigning to the same folder is no change");
    check(!folders.assign(repo, QStringLiteral("3"), QStringLiteral("gone")),
          "an unknown folder takes no members");
    check(folders.memberCount(repo, infra, members) == 1, "Infra holds one");

    QString shape = describe(folders.layout(repo, members, false), members);
    check(shape == QStringLiteral("[%1] 4 [%2] 2 1 3").arg(ui, infra),
          "folders come first in their order, then the loose agents");

    check(folders.setCollapsed(repo, ui, true), "collapse UI");
    check(!folders.setCollapsed(repo, ui, true), "collapsing twice is no change");
    shape = describe(folders.layout(repo, members, false), members);
    check(shape == QStringLiteral("[%1] [%2] 2 1 3").arg(ui, infra),
          "a collapsed folder hides its agents");
    shape = describe(folders.layout(repo, {QStringLiteral("4")}, true),
                     {QStringLiteral("4")});
    check(shape == QStringLiteral("[%1] 4").arg(ui),
          "a search opens matching folders and hides the empty ones");

    check(folders.move(repo, infra, ui, false), "drag Infra above UI");
    check(folders.folders(repo).first().id == infra, "Infra is now first");
    check(!folders.move(repo, infra, ui, false), "the same drop is no change");
    check(folders.moveToEdge(repo, infra, true), "Infra to the bottom");
    check(folders.folders(repo).last().id == infra, "Infra is last");

    check(folders.rename(repo, infra, QStringLiteral("Platform")), "rename");
    check(!folders.rename(repo, infra, QStringLiteral(" ")),
          "a blank rename is refused");

    check(folders.setAllCollapsed(repo, false), "expand all");
    folders.save();
    AgentFolders loaded;
    loaded.load();
    check(loaded.folders(repo).size() == 2 &&
              loaded.folders(repo).first().id == ui &&
              loaded.folder(repo, infra)->name == QStringLiteral("Platform") &&
              loaded.folderOf(repo, QStringLiteral("2")) == infra &&
              loaded.folderOf(repo, QStringLiteral("4")) == ui,
          "folders, order and members survive a save and load");

    check(loaded.remove(repo, infra), "delete Platform");
    check(loaded.folderOf(repo, QStringLiteral("2")).isEmpty(),
          "a deleted folder lets its agents go back to the list");
    check(loaded.assign(repo, QStringLiteral("4"), QString()),
          "remove from folder");
    check(loaded.remove(repo, ui) && loaded.folders(repo).isEmpty(),
          "the last folder can go too");
    loaded.save();
    check(!QSettings().contains(QStringLiteral("agents/folders")),
          "no folders leaves no setting behind");

    AgentFolders named;
    check(named.findOrCreate(repo, QStringLiteral(" ")).isEmpty(),
          "a blank name finds and makes no folder");
    const QString special =
        named.findOrCreate(repo, QStringLiteral("Special Agents"));
    check(!special.isEmpty() && named.folders(repo).size() == 1,
          "the first lookup makes the folder");
    check(named.findOrCreate(repo, QStringLiteral(" special   agents ")) ==
                  special &&
              named.folders(repo).size() == 1,
          "a later lookup reuses it, whatever the case and spacing");
    check(named.findOrCreate(other, QStringLiteral("Special Agents")) !=
                  special &&
              named.folders(other).size() == 1,
          "each repository gets its own folder of that name");

    const QString external =
        named.findOrCreate(repo, QStringLiteral("External Claude Code"));
    named.assign(repo, QStringLiteral("claude:b"), external);
    named.assign(repo, QStringLiteral("claude:a"), external);
    named.assign(repo, QStringLiteral("7"), special);
    check(named.members(repo, external) ==
              QStringList{QStringLiteral("claude:a"), QStringLiteral("claude:b")},
          "members lists one folder's keys, sorted");
    check(named.members(repo, QString()).isEmpty() &&
              named.members(other, external).isEmpty(),
          "no folder or another repository lists nothing");

    if (failures == 0)
        std::cout << "agent folders: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
