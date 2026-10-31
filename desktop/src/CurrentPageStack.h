#pragma once


#include <QEvent>
#include <QSize>
#include <QSizePolicy>
#include <QStackedWidget>
#include <QVariant>
#include <QVector>
#include <QWidget>

// A QStackedWidget that takes the size of the page it is showing.
//
// QStackedLayout deliberately reserves the largest page's geometry so that
// switching pages never resizes the stack. In a form that is a column of
// sections, that reads as dead space: a section whose read-only value is one
// line still claims the height of the editor hiding behind it. Every size
// question is answered for the current page here, and the pages that are not
// showing are given an Ignored size policy so QStackedLayout's own
// max-over-all-pages minimum (which qSmartMinSize computes without looking at
// whether a page is hidden) stops holding the stack open.
class CurrentPageStack : public QStackedWidget
{
public:
    explicit CurrentPageStack(QWidget *parent = nullptr)
        : QStackedWidget(parent)
    {
        connect(this, &QStackedWidget::currentChanged, this, [this](int) {
            syncPagePolicies();
            updateGeometry();
        });
    }

    QSize sizeHint() const override
    {
        if (QWidget *w = currentWidget())
            return w->sizeHint();
        return QStackedWidget::sizeHint();
    }

    QSize minimumSizeHint() const override
    {
        if (QWidget *w = currentWidget())
            return w->minimumSizeHint();
        return QStackedWidget::minimumSizeHint();
    }

    bool hasHeightForWidth() const override
    {
        if (QWidget *w = currentWidget())
            return w->hasHeightForWidth();
        return QStackedWidget::hasHeightForWidth();
    }

    int heightForWidth(int width) const override
    {
        QWidget *w = currentWidget();
        if (!w || !w->hasHeightForWidth())
            return -1;
        return w->heightForWidth(width);
    }

protected:
    bool event(QEvent *e) override
    {
        // Pages are added after construction, and a page's own contents can
        // change what it wants; re-apply on both.
        if (e->type() == QEvent::ChildAdded ||
            (e->type() == QEvent::LayoutRequest && !pagePoliciesInSync()))
            syncPagePolicies();
        return QStackedWidget::event(e);
    }

private:
    bool pagePoliciesInSync() const
    {
        if (currentIndex() != m_syncedCurrent || count() != m_syncedPages.size())
            return false;
        for (int i = 0; i < m_syncedPages.size(); ++i) {
            QWidget *page = widget(i);
            if (page != m_syncedPages.at(i) ||
                (page && page->sizePolicy() != m_syncedPolicies.at(i)))
                return false;
        }
        return true;
    }

    void syncPagePolicies()
    {
        static const char *kSaved = "forkmeshStackPagePolicy";
        const int current = currentIndex();
        m_syncedCurrent = current;
        m_syncedPages.resize(count());
        m_syncedPolicies.resize(count());
        for (int i = 0; i < count(); ++i) {
            QWidget *page = widget(i);
            m_syncedPages[i] = page;
            if (!page)
                continue;
            QVariant saved = page->property(kSaved);
            if (!saved.isValid()) {
                saved = QVariant::fromValue(page->sizePolicy());
                page->setProperty(kSaved, saved);
            }
            page->setSizePolicy(i == current
                                    ? saved.value<QSizePolicy>()
                                    : QSizePolicy(QSizePolicy::Ignored,
                                                  QSizePolicy::Ignored));
            m_syncedPolicies[i] = page->sizePolicy();
        }
    }

    int m_syncedCurrent = -2;
    QVector<QWidget *> m_syncedPages;
    QVector<QSizePolicy> m_syncedPolicies;
};
