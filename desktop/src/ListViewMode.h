#pragma once

#include <QString>

namespace forkmesh::ui {

enum class ListViewMode {
    Compact,
    Zen,
    Full,
};

struct ListViewModeLabel {
    QString key;
    QString icon;
    QString caption;
};

ListViewModeLabel listViewModeLabel(ListViewMode mode);
ListViewMode nextListViewMode(ListViewMode mode);
ListViewMode listViewModeFromKey(const QString &key, ListViewMode fallback);
QString listViewModeTooltip(ListViewMode mode, const QString &description);
QString listViewModeAccessibleName(const QString &listName, ListViewMode mode);
ListViewMode loadListViewMode(const QString &settingsKey, ListViewMode fallback);
bool storeListViewMode(const QString &settingsKey, ListViewMode mode);

}
