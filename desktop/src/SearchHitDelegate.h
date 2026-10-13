#pragma once

#include "GlobalSearchMatch.h"

#include <QStyledItemDelegate>

class QAbstractItemView;
class QListWidgetItem;
class QTreeWidgetItem;

namespace forkmesh::search {

constexpr int kSearchBadgeRole = Qt::UserRole + 40;
constexpr int kSearchBadgeColorRole = Qt::UserRole + 41;

class SearchHitDelegate : public QStyledItemDelegate
{
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    void paint(QPainter *painter, const QStyleOptionViewItem &option,
               const QModelIndex &index) const override;

    static void install(QAbstractItemView *view);
    static void setQuery(QAbstractItemView *view, const QString &query);
};

StatusBadge setStatusBadge(QListWidgetItem *item, const QString &kind,
                           const QString &status);
StatusBadge setStatusBadge(QTreeWidgetItem *item, const QString &kind,
                           const QString &status);

} // namespace forkmesh::search
