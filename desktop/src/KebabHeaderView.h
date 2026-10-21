#pragma once


#include <QHeaderView>
#include <QSize>

class QMouseEvent;
class QPainter;
class QRect;
class QTableWidget;

namespace forkmesh::ui {

class KebabHeaderView : public QHeaderView
{
    Q_OBJECT
public:
    explicit KebabHeaderView(Qt::Orientation orientation, QWidget *parent = nullptr);

    QSize sizeHint() const override;

    // Kept public for lightweight UI contract tests.  The menu affordance is
    // deliberately its own button-shaped target instead of looking like part
    // of the column caption or the column-drag surface.
    static constexpr int optionsButtonWidth() { return 18; }

    // A sortable header reserves a slot for the sort arrow -- a square the
    // height of the caption row -- in *every* section, even though only one
    // section is ever sorted. On a wide grid of short columns (the Network
    // page's node directory runs 32 of them) that gives away more width than
    // the captions use. Compact mode reserves the slot in the sorted section
    // alone and hands the rest back to the table.
    void setCompactSections(bool on);
    bool compactSections() const { return m_compactSections; }
    // Draw the decoration above the caption. This keeps icon-heavy data grids
    // narrow without sacrificing either the label or the column menu.
    void setStackedIconLabels(bool on);
    bool stackedIconLabels() const { return m_stackedIconLabels; }
    // The width this section's size hint gives up to the sort arrow, for tests
    // and for the hand-off when the sorted column changes.
    int sortSlotWidth(int logicalIndex) const;
    bool sectionShowsSortArrow(int logicalIndex) const;

protected:
    void paintSection(QPainter *painter, const QRect &rect,
                      int logicalIndex) const override;
    QSize sectionSizeFromContents(int logicalIndex) const override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    bool flatNext() const;
    void paintFlatNextSection(QPainter *painter, const QRect &rect,
                              int logicalIndex) const;
    QRect kebabRect(const QRect &sectionRect, int logicalIndex) const;
    void showColumnMenu(int logicalIndex, const QPoint &globalPos);
    void carrySortSlot(int section);

    int m_hoverSection = -1;
    bool m_hoverOptions = false;
    bool m_compactSections = false;
    bool m_stackedIconLabels = false;
    int m_sortSlotSection = -1;
};

// Give `table` the kebab column menu. `showSortIndicator` is for tables that
// actually sort (QTableWidget::setSortingEnabled): without it the header hides
// the arrow, so a sorted table would give the reader no clue which column it
// is ordered by.
void installColumnHeaderMenu(QTableWidget *table, bool showSortIndicator = false);

} // namespace forkmesh::ui
