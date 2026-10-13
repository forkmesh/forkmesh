#pragma once

#include <QLayout>
#include <QLayoutItem>
#include <QList>
#include <QMargins>
#include <QRect>
#include <QSize>
#include <QWidget>

// Left-to-right layout that wraps onto a new line when the row runs out of
// width — chips, pills and small buttons that must not be forced into one
// column by a QVBoxLayout. A child widget with the "flowFill" property set
// stretches to the end of its line.
class FlowLayout : public QLayout
{
public:
    explicit FlowLayout(QWidget *parent, int margin = 0, int hSpacing = 6,
                        int vSpacing = 6)
        : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing)
    {
        setContentsMargins(margin, margin, margin, margin);
    }
    ~FlowLayout() override
    {
        QLayoutItem *item;
        while ((item = takeAt(0)))
            delete item;
    }
    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return m_items.size(); }
    QLayoutItem *itemAt(int i) const override { return m_items.value(i); }
    QLayoutItem *takeAt(int i) override
    {
        return (i >= 0 && i < m_items.size()) ? m_items.takeAt(i) : nullptr;
    }
    Qt::Orientations expandingDirections() const override { return {}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int width) const override
    {
        return doLayout(QRect(0, 0, width, 0), true);
    }
    void setGeometry(const QRect &rect) override
    {
        QLayout::setGeometry(rect);
        doLayout(rect, false);
    }
    QSize sizeHint() const override { return minimumSize(); }
    QSize minimumSize() const override
    {
        QSize size;
        for (QLayoutItem *item : m_items)
            size = size.expandedTo(item->minimumSize());
        const QMargins m = contentsMargins();
        return size + QSize(m.left() + m.right(), m.top() + m.bottom());
    }

private:
    int doLayout(const QRect &rect, bool testOnly) const
    {
        const QMargins m = contentsMargins();
        const QRect eff =
            rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
        int x = eff.x();
        int y = eff.y();
        int lineHeight = 0;
        for (QLayoutItem *item : m_items) {
            if (item->isEmpty())
                continue; // a hidden child must not claim a slot or its spacing
            const QSize hint = item->sizeHint();
            int nextX = x + hint.width() + m_hSpace;
            if (nextX - m_hSpace > eff.right() + 1 && lineHeight > 0) {
                x = eff.x();
                y = y + lineHeight + m_vSpace;
                nextX = x + hint.width() + m_hSpace;
                lineHeight = 0;
            }
            QSize size = hint;
            QWidget *widget = item->widget();
            if (widget && widget->property("flowFill").toBool()) {
                size.setWidth(qMax(hint.width(), eff.right() + 1 - x));
                nextX = x + size.width() + m_hSpace;
            }
            if (!testOnly)
                item->setGeometry(QRect(QPoint(x, y), size));
            x = nextX;
            lineHeight = qMax(lineHeight, size.height());
        }
        return y + lineHeight - rect.y() + m.bottom();
    }
    QList<QLayoutItem *> m_items;
    int m_hSpace;
    int m_vSpace;
};
