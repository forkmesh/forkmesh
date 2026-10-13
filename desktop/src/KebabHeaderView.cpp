#include "KebabHeaderView.h"

#include <QAction>
#include <QEvent>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QStyle>
#include <QStyleOptionHeader>
#include <QTableWidget>
#include <QTimer>
#include <QToolTip>

namespace forkmesh::ui {

namespace {
constexpr int kOptionsInset = 2;
constexpr int kStackedIconSize = 14;
constexpr int kStackedHorizontalPadding = 3;
constexpr int kStackedVerticalPadding = 2;
constexpr int kStackedIconTextGap = 1;
} // namespace

KebabHeaderView::KebabHeaderView(Qt::Orientation orientation, QWidget *parent)
    : QHeaderView(orientation, parent)
{
    setMouseTracking(true);
    setSectionsClickable(true);
}

QSize KebabHeaderView::sizeHint() const
{
    QSize hint = QHeaderView::sizeHint();
    if (m_stackedIconLabels && count() > 0) {
        int contentHeight = 0;
        for (int section = 0; section < count(); ++section) {
            if (!isSectionHidden(section))
                contentHeight = qMax(contentHeight,
                                     sectionSizeFromContents(section).height());
        }
        hint.setHeight(qMax(hint.height(), contentHeight));
    }
    return hint;
}

int KebabHeaderView::sortSlotWidth(int logicalIndex) const
{
    // Ask the style what the arrow costs this section rather than deriving it:
    // the reserve is measured from the header's *content* height (icon, text
    // and the style's margins), so it does not follow the section's painted
    // height and cannot be recomputed by hand from the stylesheet.
    if (!isSortIndicatorShown())
        return 0;
    QStyleOptionHeader opt;
    initStyleOptionForIndex(&opt, logicalIndex);
    QFont bold = font();
    bold.setBold(true); // sectionSizeFromContents() measures captions bold
    opt.fontMetrics = QFontMetrics(bold);
    opt.sortIndicator = QStyleOptionHeader::SortDown;
    const int withArrow =
        style()->sizeFromContents(QStyle::CT_HeaderSection, &opt, QSize(), this)
            .width();
    opt.sortIndicator = QStyleOptionHeader::None;
    const int without =
        style()->sizeFromContents(QStyle::CT_HeaderSection, &opt, QSize(), this)
            .width();
    return qMax(0, withArrow - without);
}

bool KebabHeaderView::sectionShowsSortArrow(int logicalIndex) const
{
    if (!isSortIndicatorShown())
        return false;
    return !m_compactSections || logicalIndex == sortIndicatorSection();
}

void KebabHeaderView::setCompactSections(bool on)
{
    if (m_compactSections == on)
        return;
    m_compactSections = on;
    if (on) {
        m_sortSlotSection = sortIndicatorSection();
        connect(this, &QHeaderView::sortIndicatorChanged, this,
                [this](int section, Qt::SortOrder) { carrySortSlot(section); });
    }
    if (isSortIndicatorShown())
        resizeSections();
    viewport()->update();
}

void KebabHeaderView::setStackedIconLabels(bool on)
{
    if (m_stackedIconLabels == on)
        return;
    m_stackedIconLabels = on;
    resizeSections();
    updateGeometry();
    viewport()->update();
}

void KebabHeaderView::carrySortSlot(int section)
{
    // ResizeToContents sections re-measure themselves through
    // sectionSizeFromContents(), but makeColumnsResizable() freezes these
    // grids to Interactive once the first rows land, and an Interactive
    // section keeps whatever width it was last given. Hand the arrow slot from
    // the old sort column to the new one by hand, or the new one paints its
    // arrow straight over its own caption.
    if (!m_compactSections || section == m_sortSlotSection)
        return;
    const int previous = m_sortSlotSection;
    m_sortSlotSection = section;
    if (!isSortIndicatorShown())
        return; // nothing was reserved, so there is nothing to hand over
    const auto adjustable = [this](int logical) {
        return logical >= 0 && logical < count() && !isSectionHidden(logical) &&
               sectionResizeMode(logical) == QHeaderView::Interactive;
    };
    // Each side is measured from its own section, so the two moves cancel out
    // and the grid keeps the width it had.
    if (adjustable(previous))
        resizeSection(previous,
                      qMax(minimumSectionSize(),
                           sectionSize(previous) - sortSlotWidth(previous)));
    if (adjustable(section))
        resizeSection(section, sectionSize(section) + sortSlotWidth(section));
}

QRect KebabHeaderView::kebabRect(const QRect &sectionRect,
                                 int logicalIndex) const
{
    // A sortable header paints its arrow hard against the section's right
    // edge, in a square the height of the section. Step the menu button inward
    // past it so the two do not stack on each other. This is the *painted*
    // footprint, which is why it is measured from the rect rather than from
    // sortSlotWidth()'s content-based reserve.
    int right = sectionRect.right();
    if (sectionShowsSortArrow(logicalIndex)) {
        right -= m_stackedIconLabels
                     ? sortSlotWidth(logicalIndex)
                     : sectionRect.height() +
                           style()->pixelMetric(QStyle::PM_HeaderMargin, nullptr,
                                                this);
    }
    return QRect(right - optionsButtonWidth() + 1,
                 sectionRect.top() + kOptionsInset,
                 optionsButtonWidth() - kOptionsInset,
                 qMax(1, sectionRect.height() - 2 * kOptionsInset));
}

QSize KebabHeaderView::sectionSizeFromContents(int logicalIndex) const
{
    if (m_stackedIconLabels && model()) {
        const QString caption =
            model()->headerData(logicalIndex, orientation(), Qt::DisplayRole)
                .toString();
        const QIcon icon = qvariant_cast<QIcon>(
            model()->headerData(logicalIndex, orientation(), Qt::DecorationRole));
        const QFontMetrics metrics(font());
        const int labelWidth = metrics.horizontalAdvance(caption);
        const int contentWidth = qMax(labelWidth, icon.isNull() ? 0
                                                                : kStackedIconSize);
        int width = contentWidth + 2 * kStackedHorizontalPadding +
                    optionsButtonWidth();
        if (sectionShowsSortArrow(logicalIndex))
            width += sortSlotWidth(logicalIndex);
        const int iconHeight = icon.isNull() ? 0 : kStackedIconSize;
        const int gap = icon.isNull() ? 0 : kStackedIconTextGap;
        return QSize(qMax(1, width),
                     2 * kStackedVerticalPadding + iconHeight + gap +
                         metrics.height());
    }
    QSize size = QHeaderView::sectionSizeFromContents(logicalIndex);
    size.setWidth(size.width() + optionsButtonWidth());
    if (!sectionShowsSortArrow(logicalIndex))
        size.setWidth(qMax(1, size.width() - sortSlotWidth(logicalIndex)));
    return size;
}

void KebabHeaderView::paintSection(QPainter *painter, const QRect &rect,
                                   int logicalIndex) const
{
    if (!m_stackedIconLabels) {
        QHeaderView::paintSection(painter, rect, logicalIndex);
    } else {
        QStyleOptionHeader opt;
        initStyleOptionForIndex(&opt, logicalIndex);
        // QHeaderView normally supplies the section rectangle after building
        // the style option. This custom branch owns that last step; leaving the
        // default empty rect made some Qt styles paint neither the section nor
        // its contents.
        opt.rect = rect;
        if (!sectionShowsSortArrow(logicalIndex))
            opt.sortIndicator = QStyleOptionHeader::None;
        // Read the model roles directly. A few platform styles consume or omit
        // opt.icon/opt.text while initializing the option, which left a fully
        // sized but blank Mirror nodes header on those platforms.
        const QIcon icon = qvariant_cast<QIcon>(
            model()->headerData(logicalIndex, orientation(),
                                Qt::DecorationRole));
        const QString caption =
            model()->headerData(logicalIndex, orientation(), Qt::DisplayRole)
                .toString();
        opt.icon = QIcon();
        opt.text.clear();
        style()->drawControl(QStyle::CE_Header, &opt, painter, this);

        int rightReserve = optionsButtonWidth();
        if (sectionShowsSortArrow(logicalIndex))
            rightReserve += sortSlotWidth(logicalIndex);
        const QRect content = rect.adjusted(kStackedHorizontalPadding,
                                            kStackedVerticalPadding,
                                            -rightReserve,
                                            -kStackedVerticalPadding);
        painter->save();
        painter->setFont(font());
        painter->setPen(opt.palette.color((opt.state & QStyle::State_Enabled)
                                               ? QPalette::Active
                                               : QPalette::Disabled,
                                           QPalette::ButtonText));
        int textTop = content.top();
        if (!icon.isNull()) {
            const QRect iconRect(content.center().x() - kStackedIconSize / 2,
                                 content.top(), kStackedIconSize,
                                 kStackedIconSize);
            icon.paint(painter, iconRect, Qt::AlignCenter,
                       (opt.state & QStyle::State_Enabled) ? QIcon::Normal
                                                           : QIcon::Disabled);
            textTop = iconRect.bottom() + 1 + kStackedIconTextGap;
        }
        painter->drawText(QRect(content.left(), textTop, content.width(),
                                qMax(1, content.bottom() - textTop + 1)),
                          Qt::AlignHCenter | Qt::AlignTop | Qt::TextSingleLine,
                          caption);
        painter->restore();
    }
    if (!rect.isValid() || rect.width() <= optionsButtonWidth())
        return;

    const QRect zone = kebabRect(rect, logicalIndex);
    const bool hovered = logicalIndex == m_hoverSection && m_hoverOptions;
    const QColor foreground = hovered ? QColor(0xe6, 0xed, 0xf3)
                                      : QColor(0x8b, 0x94, 0x9e);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    if (hovered) {
        painter->setPen(QPen(QColor(0x58, 0xa6, 0xff, 150), 1));
        painter->setBrush(QColor(0x58, 0xa6, 0xff, 28));
        painter->drawRoundedRect(zone.adjusted(1, 1, -1, -1), 3, 3);
    }
    painter->setPen(QPen(foreground, 1.35, Qt::SolidLine, Qt::RoundCap,
                         Qt::RoundJoin));
    const int cx = zone.center().x();
    const int cy = zone.center().y();
    painter->drawLine(QPoint(cx - 3, cy - 1), QPoint(cx, cy + 2));
    painter->drawLine(QPoint(cx, cy + 2), QPoint(cx + 3, cy - 1));
    painter->restore();
}

void KebabHeaderView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        const QPoint pos = event->position().toPoint();
        const int logical = logicalIndexAt(pos);
        if (logical >= 0) {
            const QRect sect(sectionViewportPosition(logical), 0,
                             sectionSize(logical), height());
            if (kebabRect(sect, logical).contains(pos)) {
                const QRect options = kebabRect(sect, logical);
                showColumnMenu(logical,
                               mapToGlobal(QPoint(options.left(), options.bottom() + 1)));
                event->accept();
                return;
            }
        }
    }
    QHeaderView::mousePressEvent(event);
}

void KebabHeaderView::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->position().toPoint();
    const int logical = logicalIndexAt(pos);
    bool options = false;
    if (logical >= 0) {
        const QRect section(sectionViewportPosition(logical), 0,
                            sectionSize(logical), height());
        options = kebabRect(section, logical).contains(pos);
    }
    if (logical != m_hoverSection || options != m_hoverOptions) {
        m_hoverSection = logical;
        m_hoverOptions = options;
        viewport()->update();
    }
    QHeaderView::mouseMoveEvent(event);
}

void KebabHeaderView::leaveEvent(QEvent *event)
{
    if (m_hoverSection != -1) {
        m_hoverSection = -1;
        m_hoverOptions = false;
        viewport()->update();
    }
    QHeaderView::leaveEvent(event);
}

bool KebabHeaderView::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int logical = logicalIndexAt(help->pos());
        if (logical >= 0) {
            const QRect section(sectionViewportPosition(logical), 0,
                                sectionSize(logical), height());
            if (kebabRect(section, logical).contains(help->pos())) {
                QToolTip::showText(help->globalPos(), tr("Column options"), this,
                                   kebabRect(section, logical));
                return true;
            }
        }
    }
    return QHeaderView::event(event);
}

void KebabHeaderView::showColumnMenu(int logicalIndex, const QPoint &globalPos)
{
    auto *view = qobject_cast<QTableView *>(parentWidget());

    int visibleCount = 0;
    for (int i = 0; i < count(); ++i)
        if (!isSectionHidden(i))
            ++visibleCount;
    bool anyHidden = visibleCount < count();
    const int visual = visualIndex(logicalIndex);

    QMenu menu(this);

    QAction *asc = menu.addAction(tr("Sort ascending"));
    QAction *desc = menu.addAction(tr("Sort descending"));
    asc->setCheckable(true);
    desc->setCheckable(true);
    if (sortIndicatorSection() == logicalIndex) {
        if (sortIndicatorOrder() == Qt::AscendingOrder)
            asc->setChecked(true);
        else
            desc->setChecked(true);
    }

    menu.addSeparator();
    QAction *select = menu.addAction(tr("Select column"));
    QAction *hide = menu.addAction(tr("Hide field"));
    hide->setEnabled(visibleCount > 1); // never hide the last visible column
    QAction *showAll = anyHidden ? menu.addAction(tr("Show all fields")) : nullptr;

    menu.addSeparator();
    QAction *left = menu.addAction(tr("Move left"));
    QAction *right = menu.addAction(tr("Move right"));
    QAction *start = menu.addAction(tr("Move to start"));
    QAction *end = menu.addAction(tr("Move to end"));
    left->setEnabled(visual > 0);
    start->setEnabled(visual > 0);
    right->setEnabled(visual < count() - 1);
    end->setEnabled(visual < count() - 1);

    QAction *chosen = menu.exec(globalPos);
    if (!chosen)
        return;

    if (chosen == asc && view)
        view->sortByColumn(logicalIndex, Qt::AscendingOrder);
    else if (chosen == desc && view)
        view->sortByColumn(logicalIndex, Qt::DescendingOrder);
    else if (chosen == select && view)
        view->selectColumn(logicalIndex);
    else if (chosen == hide)
        setSectionHidden(logicalIndex, true);
    else if (chosen == showAll) {
        for (int i = 0; i < count(); ++i)
            setSectionHidden(i, false);
    } else if (chosen == left)
        moveSection(visual, visual - 1);
    else if (chosen == right)
        moveSection(visual, visual + 1);
    else if (chosen == start)
        moveSection(visual, 0);
    else if (chosen == end)
        moveSection(visual, count() - 1);
}

void installColumnHeaderMenu(QTableWidget *table, bool showSortIndicator)
{
    if (!table)
        return;
    auto *header = new KebabHeaderView(Qt::Horizontal, table);
    header->setSectionsMovable(true);
    table->setHorizontalHeader(header);
    // Deferred either way: setSortingEnabled() and the first layout both set
    // this themselves, and whichever of them runs last would otherwise decide.
    QTimer::singleShot(0, header, [header, showSortIndicator] {
        header->setSortIndicatorShown(showSortIndicator);
    });
}

} // namespace forkmesh::ui
