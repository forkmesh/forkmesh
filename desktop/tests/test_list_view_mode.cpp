#include "ListViewMode.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryDir>

#include <cstdio>

using namespace forkmesh::ui;

namespace {

int failures = 0;

void check(bool condition, const char *message)
{
    if (condition)
        return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

}

int main(int argc, char **argv)
{
    QTemporaryDir config;
    if (!config.isValid()) {
        std::fprintf(stderr, "FAIL: no temporary config dir\n");
        return 1;
    }
    qputenv("XDG_CONFIG_HOME", config.path().toUtf8());
    qunsetenv("FORKMESH_HOME");
    qunsetenv("FORKMESH_PROJECTS_DIR");
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("ForkMeshListViewTest"));
    QCoreApplication::setApplicationName(QStringLiteral("ListViewModeTest"));

    check(nextListViewMode(ListViewMode::Compact) == ListViewMode::Zen,
          "Compact steps to Zen");
    check(nextListViewMode(ListViewMode::Zen) == ListViewMode::Full,
          "Zen steps to Full");
    check(nextListViewMode(ListViewMode::Full) == ListViewMode::Compact,
          "Full steps back to Compact");

    check(listViewModeLabel(ListViewMode::Compact).key == QLatin1String("compact") &&
              listViewModeLabel(ListViewMode::Compact).icon ==
                  QLatin1String("list-unordered") &&
              listViewModeLabel(ListViewMode::Compact).caption ==
                  QLatin1String("Compact"),
          "Compact keeps the Agents list's key, icon and caption");
    check(listViewModeLabel(ListViewMode::Zen).key == QLatin1String("zen") &&
              listViewModeLabel(ListViewMode::Zen).icon == QLatin1String("moon"),
          "Zen keeps the moon icon and its stored key");
    check(listViewModeLabel(ListViewMode::Full).key == QLatin1String("full") &&
              listViewModeLabel(ListViewMode::Full).icon == QLatin1String("eye"),
          "Full keeps the eye icon and its stored key");

    for (ListViewMode mode :
         {ListViewMode::Compact, ListViewMode::Zen, ListViewMode::Full})
        check(listViewModeFromKey(listViewModeLabel(mode).key,
                                  ListViewMode::Compact) == mode,
              "every stored key reads back as its mode");
    check(listViewModeFromKey(QStringLiteral("cards"), ListViewMode::Full) ==
              ListViewMode::Full,
          "an unknown stored key falls back to the page's default");
    check(listViewModeFromKey(QString(), ListViewMode::Compact) ==
              ListViewMode::Compact,
          "no stored key falls back to the page's default");

    check(listViewModeTooltip(ListViewMode::Zen,
                              QStringLiteral("nothing but each agent's title")) ==
              QStringLiteral("Zen view: nothing but each agent's title. Click "
                             "for Full view"),
          "the tooltip names the view and the one a click goes to");
    check(listViewModeAccessibleName(QStringLiteral("Task list"),
                                     ListViewMode::Compact) ==
              QStringLiteral("Task list view: Compact"),
          "the accessible name names the list and the view");

    const QString agentsKey = QStringLiteral("agents/listView");
    const QString tasksKey = QStringLiteral("tasks/listView");
    check(loadListViewMode(tasksKey, ListViewMode::Full) == ListViewMode::Full,
          "an unset Tasks view loads as the page default");
    check(storeListViewMode(tasksKey, ListViewMode::Zen),
          "the first store writes the setting");
    check(!storeListViewMode(tasksKey, ListViewMode::Zen),
          "storing the same view again does not rewrite settings");
    check(QSettings().value(tasksKey).toString() == QLatin1String("zen"),
          "the Tasks view is stored under tasks/listView");
    check(loadListViewMode(tasksKey, ListViewMode::Full) == ListViewMode::Zen,
          "the stored Tasks view loads back");
    check(loadListViewMode(agentsKey, ListViewMode::Compact) ==
              ListViewMode::Compact,
          "the Agents view is kept apart from the Tasks view");
    check(storeListViewMode(agentsKey, ListViewMode::Full) &&
              loadListViewMode(tasksKey, ListViewMode::Full) ==
                  ListViewMode::Zen,
          "changing the Agents view leaves the Tasks view alone");

    if (failures == 0)
        std::printf("list view mode tests passed\n");
    return failures == 0 ? 0 : 1;
}
