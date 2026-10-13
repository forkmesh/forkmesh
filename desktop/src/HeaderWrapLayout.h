#pragma once

#include <QLayout>
#include <QLayoutItem>
#include <QList>
#include <QMargins>
#include <QRect>
#include <QSize>
#include <QWidget>

// A pane header that survives being dragged narrow. It has three slots:
//
//   lead     a small fixed widget owning the top-left corner (a close button,
//            a section caption)
//   title    the flexible middle, which takes whatever the first line has left
//   actions  a trailing cluster, held against the right edge the way a
//            QHBoxLayout stretch would hold it
//
// While the title's minimum still fits beside the actions the result is
// indistinguishable from `[lead][title]<stretch>[actions]`. Once it stops
// fitting, the title moves onto a line of its own below, and if the actions
// alone still overflow they wrap across further lines as well. So a pane headed
// by this layout is only ever as wide as its widest single action, instead of
// the whole row added up -- which is what lets the splitter beside it be
// dragged much further in either direction.
class HeaderWrapLayout : public QLayout
{
public:
    explicit HeaderWrapLayout(QWidget *parent, int hSpacing = 6,
                              int vSpacing = 4)
        : QLayout(parent), m_hSpace(hSpacing), m_vSpace(vSpacing)
    {
        setContentsMargins(0, 0, 0, 0);
    }
    ~HeaderWrapLayout() override
    {
        QLayoutItem *item;
        while ((item = takeAt(0)))
            delete item;
    }

    void setLead(QWidget *widget)
    {
        if (!widget)
            return;
        addWidget(widget);
        m_lead = m_items.last();
    }
    void setTitle(QWidget *widget)
    {
        if (!widget)
            return;
        addWidget(widget);
        m_title = m_items.last();
    }
    void addAction(QWidget *widget)
    {
        if (!widget)
            return;
        addWidget(widget);
        m_actions.append({m_items.last(), m_pendingGap});
        m_pendingGap = 0;
    }
    // Extra room before the next action, as QBoxLayout::addSpacing gives. It is
    // dropped when that action ends up starting a line.
    void addActionSpacing(int px) { m_pendingGap += px; }

    void addItem(QLayoutItem *item) override { m_items.append(item); }
    int count() const override { return m_items.size(); }
    QLayoutItem *itemAt(int i) const override { return m_items.value(i); }
    QLayoutItem *takeAt(int i) override
    {
        if (i < 0 || i >= m_items.size())
            return nullptr;
        QLayoutItem *item = m_items.takeAt(i);
        if (item == m_lead)
            m_lead = nullptr;
        if (item == m_title)
            m_title = nullptr;
        for (int a = 0; a < m_actions.size(); ++a) {
            if (m_actions.at(a).item == item) {
                m_actions.removeAt(a);
                break;
            }
        }
        return item;
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
    QSize sizeHint() const override
    {
        const QMargins m = contentsMargins();
        int width = 0;
        int height = 0;
        if (occupied(m_lead)) {
            width = m_lead->sizeHint().width();
            height = m_lead->sizeHint().height();
        }
        if (occupied(m_title)) {
            width += (width ? m_hSpace : 0) + m_title->sizeHint().width();
            height = qMax(height, m_title->sizeHint().height());
        }
        if (const int actions = actionsWidth()) {
            width += (width ? m_hSpace : 0) + actions;
            height = qMax(height, actionsHeight());
        }
        return QSize(width + m.left() + m.right(),
                     height + m.top() + m.bottom());
    }
    QSize minimumSize() const override
    {
        const QMargins m = contentsMargins();
        int width = widestAction();
        if (occupied(m_lead))
            width += (width ? m_hSpace : 0) + m_lead->sizeHint().width();
        // Past this point a long title gives -- by wrapping, or by letting its
        // tail clip -- rather than holding the whole pane open.
        if (occupied(m_title))
            width = qMax(width,
                         qMin(m_title->minimumSize().width(), kTitleMinCap));
        width += m.left() + m.right();
        return QSize(width, doLayout(QRect(0, 0, width, 0), true));
    }

private:
    struct Action
    {
        QLayoutItem *item;
        int gapBefore;
    };

    static bool occupied(QLayoutItem *item) { return item && !item->isEmpty(); }

    int actionsWidth() const // the whole cluster on one line
    {
        int width = 0;
        for (const Action &action : m_actions) {
            if (!occupied(action.item))
                continue;
            if (width)
                width += m_hSpace + action.gapBefore;
            width += action.item->sizeHint().width();
        }
        return width;
    }
    int actionsHeight() const
    {
        int height = 0;
        for (const Action &action : m_actions)
            if (occupied(action.item))
                height = qMax(height, action.item->sizeHint().height());
        return height;
    }
    int widestAction() const
    {
        int width = 0;
        for (const Action &action : m_actions)
            if (occupied(action.item))
                width = qMax(width, action.item->sizeHint().width());
        return width;
    }

    // Places the cluster inside [left, right], wrapping when it must, and
    // reports the height it used. A cluster that fits on one line is pushed to
    // the right edge, which is where a stretch ahead of it would have left it.
    int layoutActions(int left, int right, int top, int rowHeight,
                      bool testOnly) const
    {
        const int natural = actionsWidth();
        if (natural <= 0)
            return 0;
        int x = natural <= right - left + 1 ? right + 1 - natural : left;
        int y = top;
        bool first = true;
        for (const Action &action : m_actions) {
            if (!occupied(action.item))
                continue;
            const QSize hint = action.item->sizeHint();
            if (!first) {
                const int gap = m_hSpace + action.gapBefore;
                if (x + gap + hint.width() > right + 1 && x > left) {
                    x = left;
                    y += rowHeight + m_vSpace;
                } else {
                    x += gap;
                }
            }
            if (!testOnly)
                action.item->setGeometry(
                    QRect(x, y + (rowHeight - hint.height()) / 2, hint.width(),
                          hint.height()));
            x += hint.width();
            first = false;
        }
        return y + rowHeight - top;
    }

    int doLayout(const QRect &rect, bool testOnly) const
    {
        const QMargins m = contentsMargins();
        const QRect eff =
            rect.adjusted(m.left(), m.top(), -m.right(), -m.bottom());
        const QSize leadHint =
            occupied(m_lead) ? m_lead->sizeHint() : QSize(0, 0);
        const int titleHeight =
            occupied(m_title) ? m_title->sizeHint().height() : 0;
        const int actions = actionsWidth();
        const int actionsLeft =
            eff.x() + (leadHint.width() ? leadHint.width() + m_hSpace : 0);
        const int actionsRoom = actions ? actions + m_hSpace : 0;
        const bool titleInline =
            occupied(m_title) && m_title->minimumSize().width() + actionsRoom <=
                                     eff.right() + 1 - actionsLeft;

        int rowHeight = qMax(leadHint.height(), actionsHeight());
        if (titleInline)
            rowHeight = qMax(rowHeight, titleHeight);
        if (occupied(m_lead) && !testOnly)
            m_lead->setGeometry(QRect(eff.x(),
                                      eff.y() + (rowHeight -
                                                 leadHint.height()) / 2,
                                      leadHint.width(), leadHint.height()));
        int used = qMax(rowHeight, layoutActions(actionsLeft, eff.right(),
                                                 eff.y(), rowHeight, testOnly));
        if (titleInline) {
            const int width =
                qMax(0, eff.right() + 1 - actionsLeft - actionsRoom);
            if (!testOnly)
                m_title->setGeometry(
                    QRect(actionsLeft, eff.y() + (rowHeight - titleHeight) / 2,
                          width, titleHeight));
        } else if (occupied(m_title)) {
            const int width = eff.width();
            const int height = m_title->hasHeightForWidth()
                                   ? m_title->heightForWidth(width)
                                   : titleHeight;
            if (!testOnly)
                m_title->setGeometry(
                    QRect(eff.x(), eff.y() + used + m_vSpace, width, height));
            used += m_vSpace + height;
        }
        return used + m.top() + m.bottom();
    }

    static constexpr int kTitleMinCap = 72; // still shows a few characters

    QList<QLayoutItem *> m_items;
    QList<Action> m_actions;
    QLayoutItem *m_lead = nullptr;
    QLayoutItem *m_title = nullptr;
    int m_pendingGap = 0;
    int m_hSpace;
    int m_vSpace;
};
