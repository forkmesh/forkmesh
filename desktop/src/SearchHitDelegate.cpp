#include "SearchHitDelegate.h"
#include "Theme.h"

#include <QAbstractItemView>
#include <QApplication>
#include <QListWidgetItem>
#include <QPainter>
#include <QPainterPath>
#include <QTreeWidgetItem>

namespace forkmesh::search {

namespace {

constexpr const char *kQueryProperty = "forkmeshSearchQuery";

QRect drawBadge(QPainter *painter, const QFont &base, const QString &label,
                const QColor &color, int x, int centerY)
{
    QFont font = base;
    font.setBold(true);
    font.setPointSizeF(qMax(6.0, base.pointSizeF() - 1.5));
    const QFontMetrics fm(font);
    const int height = fm.height() + 2;
    const QRect rect(x, centerY - height / 2,
                     fm.horizontalAdvance(label) + 12, height);
    QColor fill = color;
    fill.setAlpha(40);
    QPainterPath path;
    path.addRoundedRect(QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5),
                        height / 2.0, height / 2.0);
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->fillPath(path, fill);
    painter->setPen(color);
    painter->drawPath(path);
    painter->setFont(font);
    painter->drawText(rect, Qt::AlignCenter, label);
    return rect;
}

int drawRuns(QPainter *painter, const QFont &base, const QString &line,
             const QString &query, const QColor &color, int x, int right,
             int top, int height)
{
    QFont bold = base;
    bold.setBold(true);
    const QColor mark(187, 128, 9, 110);
    for (const HighlightRun &run : highlightRuns(line, query)) {
        if (x >= right)
            break;
        const QFont &font = run.match ? bold : base;
        const QFontMetrics fm(font);
        QString text = run.text;
        int width = fm.horizontalAdvance(text);
        if (x + width > right) {
            text = fm.elidedText(text, Qt::ElideRight, right - x);
            width = fm.horizontalAdvance(text);
        }
        const QRect box(x, top, width, height);
        if (run.match)
            painter->fillRect(box.adjusted(-1, 1, 1, -1), mark);
        painter->setFont(font);
        painter->setPen(color);
        painter->drawText(box, Qt::AlignLeft | Qt::AlignVCenter, text);
        x += width;
    }
    return x;
}

} // namespace

void SearchHitDelegate::install(QAbstractItemView *view)
{
    view->setItemDelegate(new SearchHitDelegate(view));
}

void SearchHitDelegate::setQuery(QAbstractItemView *view, const QString &query)
{
    if (!view)
        return;
    view->setProperty(kQueryProperty, query.trimmed());
    view->viewport()->update();
}

void SearchHitDelegate::paint(QPainter *painter,
                              const QStyleOptionViewItem &option,
                              const QModelIndex &index) const
{
    QStyleOptionViewItem opt(option);
    initStyleOption(&opt, index);
    const QWidget *widget = option.widget;
    const QString query =
        widget ? widget->property(kQueryProperty).toString() : QString();
    const QString badge = index.data(kSearchBadgeRole).toString();
    const bool selectable = index.flags() & Qt::ItemIsSelectable;
    if (!selectable || opt.text.isEmpty() ||
        (badge.isEmpty() && !containsFold(opt.text, query))) {
        QStyledItemDelegate::paint(painter, option, index);
        return;
    }

    QStyle *style = widget ? widget->style() : QApplication::style();
    const QRect textRect =
        style->subElementRect(QStyle::SE_ItemViewItemText, &opt, widget);
    const QString text = opt.text;
    opt.text.clear();
    style->drawControl(QStyle::CE_ItemViewItem, &opt, painter, widget);

    const bool selected = opt.state & QStyle::State_Selected;
    const QPalette::ColorGroup group =
        opt.state & QStyle::State_Enabled ? QPalette::Normal
                                          : QPalette::Disabled;
    const QColor primary = opt.palette.color(
        group, selected ? QPalette::HighlightedText : QPalette::Text);
    const QColor dim = selected ? primary : Theme::remapColor(QColor("#8b949e"));

    const QStringList lines = text.split(QLatin1Char('\n'));
    const QFontMetrics fm(opt.font);
    const int lineHeight = fm.height();
    int top = textRect.center().y() - lineHeight * int(lines.size()) / 2 + 1;
    const int left = textRect.left() + 2;
    const int right = textRect.right() - 2;

    painter->save();
    painter->setClipRect(textRect);
    for (int i = 0; i < lines.size(); ++i) {
        int x = left;
        if (i == 0 && !badge.isEmpty()) {
            const QColor color = index.data(kSearchBadgeColorRole).value<QColor>();
            x = drawBadge(painter, opt.font, badge, color, x,
                          top + lineHeight / 2)
                    .right() +
                7;
            painter->setRenderHint(QPainter::Antialiasing, false);
        }
        drawRuns(painter, opt.font, lines.at(i), query, i == 0 ? primary : dim,
                 x, right, top, lineHeight);
        top += lineHeight;
    }
    painter->restore();
}

StatusBadge setStatusBadge(QListWidgetItem *item, const QString &kind,
                           const QString &status)
{
    const StatusBadge badge = statusBadge(kind, status);
    if (item && !badge.label.isEmpty()) {
        item->setData(kSearchBadgeRole, badge.label);
        item->setData(kSearchBadgeColorRole, QColor(badge.color));
    }
    return badge;
}

StatusBadge setStatusBadge(QTreeWidgetItem *item, const QString &kind,
                           const QString &status)
{
    const StatusBadge badge = statusBadge(kind, status);
    if (item && !badge.label.isEmpty()) {
        item->setData(0, kSearchBadgeRole, badge.label);
        item->setData(0, kSearchBadgeColorRole, QColor(badge.color));
    }
    return badge;
}

} // namespace forkmesh::search
