#pragma once

#include "CurrentPageStack.h"

#include <QResizeEvent>
#include <QScrollBar>
#include <QWheelEvent>

namespace forkmesh::ui {

class SectionViewport : public CurrentPageStack
{
public:
    static constexpr int kMinimumWidth = 520;

    explicit SectionViewport(QWidget *parent = nullptr)
        : CurrentPageStack(parent)
    {
        m_bar = new QScrollBar(Qt::Horizontal, this);
        m_bar->setObjectName(QStringLiteral("sectionScrollBar"));
        m_bar->setSingleStep(40);
        m_bar->hide();
        connect(m_bar, &QScrollBar::valueChanged, this, [this](int) { place(); });
        connect(this, &QStackedWidget::currentChanged, this, [this](int) {
            m_bar->setValue(0);
            place();
        });
    }

    QSize minimumSizeHint() const override
    {
        const QSize page = CurrentPageStack::minimumSizeHint();
        return QSize(qMin(page.width(), kMinimumWidth), page.height());
    }

    int overflow() const { return m_bar->isHidden() ? 0 : m_bar->maximum(); }

protected:
    bool event(QEvent *e) override
    {
        const bool handled = CurrentPageStack::event(e);
        if (e->type() == QEvent::LayoutRequest)
            place();
        return handled;
    }

    void resizeEvent(QResizeEvent *e) override
    {
        CurrentPageStack::resizeEvent(e);
        place();
    }

    void wheelEvent(QWheelEvent *e) override
    {
        const QPoint delta = e->angleDelta();
        const bool sideways = qAbs(delta.x()) > qAbs(delta.y()) ||
                              e->modifiers().testFlag(Qt::ShiftModifier);
        if (m_bar->isHidden() || !sideways) {
            CurrentPageStack::wheelEvent(e);
            return;
        }
        const int step = delta.x() != 0 ? delta.x() : delta.y();
        m_bar->setValue(m_bar->value() - step);
        e->accept();
    }

private:
    void place()
    {
        if (m_placing)
            return;
        m_placing = true;
        QWidget *page = currentWidget();
        const int wanted =
            page ? qMax(page->minimumSizeHint().width(), page->minimumWidth()) : 0;
        const int extra = wanted - width();
        if (!page || extra <= 0) {
            m_bar->hide();
            m_bar->setRange(0, 0);
            if (page && page->geometry() != rect())
                page->setGeometry(rect());
        } else {
            const int barHeight = m_bar->sizeHint().height();
            m_bar->setRange(0, extra);
            m_bar->setPageStep(width());
            m_bar->setGeometry(0, height() - barHeight, width(), barHeight);
            m_bar->show();
            m_bar->raise();
            const QRect wide(-m_bar->value(), 0, wanted,
                             qMax(0, height() - barHeight));
            if (page->geometry() != wide)
                page->setGeometry(wide);
        }
        m_placing = false;
    }

    QScrollBar *m_bar = nullptr;
    bool m_placing = false;
};

}
