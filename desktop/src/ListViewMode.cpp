#include "ListViewMode.h"

#include "SettingsWrite.h"

#include <QSettings>

namespace forkmesh::ui {

ListViewModeLabel listViewModeLabel(ListViewMode mode)
{
    switch (mode) {
    case ListViewMode::Zen:
        return {QStringLiteral("zen"), QStringLiteral("moon"),
                QStringLiteral("Zen")};
    case ListViewMode::Full:
        return {QStringLiteral("full"), QStringLiteral("eye"),
                QStringLiteral("Full")};
    case ListViewMode::Compact:
        break;
    }
    return {QStringLiteral("compact"), QStringLiteral("list-unordered"),
            QStringLiteral("Compact")};
}

ListViewMode nextListViewMode(ListViewMode mode)
{
    switch (mode) {
    case ListViewMode::Compact:
        return ListViewMode::Zen;
    case ListViewMode::Zen:
        return ListViewMode::Full;
    case ListViewMode::Full:
        break;
    }
    return ListViewMode::Compact;
}

ListViewMode listViewModeFromKey(const QString &key, ListViewMode fallback)
{
    for (ListViewMode mode :
         {ListViewMode::Compact, ListViewMode::Zen, ListViewMode::Full}) {
        if (key == listViewModeLabel(mode).key)
            return mode;
    }
    return fallback;
}

QString listViewModeTooltip(ListViewMode mode, const QString &description)
{
    return QStringLiteral("%1 view: %2. Click for %3 view")
        .arg(listViewModeLabel(mode).caption, description,
             listViewModeLabel(nextListViewMode(mode)).caption);
}

QString listViewModeAccessibleName(const QString &listName, ListViewMode mode)
{
    return QStringLiteral("%1 view: %2")
        .arg(listName, listViewModeLabel(mode).caption);
}

ListViewMode loadListViewMode(const QString &settingsKey, ListViewMode fallback)
{
    return listViewModeFromKey(QSettings().value(settingsKey).toString(),
                               fallback);
}

bool storeListViewMode(const QString &settingsKey, ListViewMode mode)
{
    return forkmesh::settings::put(QSettings(), settingsKey,
                                   listViewModeLabel(mode).key);
}

}
