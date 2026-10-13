#include "ScrollJumpButtons.h"

#include "Theme.h"

#include <QAbstractScrollArea>
#include <QEvent>
#include <QPalette>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollBar>
#include <QTimer>

static constexpr int kEdgeSlack = 4;

class JumpArrowButton : public QPushButton
{
public:
    JumpArrowButton(bool up, QWidget *parent) : QPushButton(parent), m_up(up) {}

    void setArrowColor(const QColor &c)
    {
        if (m_arrow != c) {
            m_arrow = c;
            update();
        }
    }

    void setDotColor(const QColor &c)
    {
        if (m_dot != c) {
            m_dot = c;
            update();
        }
    }

    void setFresh(bool fresh)
    {
        if (m_fresh != fresh) {
            m_fresh = fresh;
            update();
        }
    }

    bool isFresh() const { return m_fresh; }

protected:
    void paintEvent(QPaintEvent *e) override
    {
        QPushButton::paintEvent(e); // styled circle + border (no text)

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QPointF c(width() / 2.0, height() / 2.0);
        const qreal hw = 5.0;  // half-width of the triangle
        const qreal hh = 3.5;  // half-height
        QPainterPath tri;
        if (m_up) {
            tri.moveTo(c.x(), c.y() - hh);
            tri.lineTo(c.x() - hw, c.y() + hh);
            tri.lineTo(c.x() + hw, c.y() + hh);
        } else {
            tri.moveTo(c.x() - hw, c.y() - hh);
            tri.lineTo(c.x() + hw, c.y() - hh);
            tri.lineTo(c.x(), c.y() + hh);
        }
        tri.closeSubpath();
        p.fillPath(tri, m_arrow);
        if (m_fresh) {
            p.setPen(Qt::NoPen);
            p.setBrush(m_dot);
            p.drawEllipse(QPointF(width() - 7.0, 7.0), 3.5, 3.5);
        }
    }

private:
    bool m_up;
    bool m_fresh = false;
    QColor m_arrow{0xe6, 0xed, 0xf3}; // sensible dark-theme default
    QColor m_dot{0x2f, 0x81, 0xf7};
};

ScrollJumpButtons::ScrollJumpButtons(QAbstractScrollArea *area)
    : QObject(area), m_area(area)
{
    auto mkBtn = [this](bool up, const QString &tip) {
        auto *b = new JumpArrowButton(up, m_area->viewport());
        b->setObjectName(QStringLiteral("scrollJump"));
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        b->setFixedSize(30, 30);
        b->hide();
        return b;
    };
    m_top = mkBtn(true, QStringLiteral("Jump to top"));
    m_bottom = mkBtn(false, QStringLiteral("Jump to latest"));
    connect(m_top, &QPushButton::clicked, this, &ScrollJumpButtons::topClicked);
    connect(m_bottom, &QPushButton::clicked, this, &ScrollJumpButtons::bottomClicked);
    setTheme(m_area->palette().color(QPalette::Base).lightness() < 128);

    m_area->viewport()->installEventFilter(this);

    QScrollBar *sb = m_area->verticalScrollBar();
    connect(sb, &QScrollBar::valueChanged, this,
            &ScrollJumpButtons::scheduleVisibilityUpdate);
    connect(sb, &QScrollBar::rangeChanged, this,
            &ScrollJumpButtons::scheduleVisibilityUpdate);
    updateVisibility();
}

void ScrollJumpButtons::setButtonStyle(const QString &css)
{
    m_top->setStyleSheet(Theme::remapSheet(css));
    m_bottom->setStyleSheet(Theme::remapSheet(css));
}

void ScrollJumpButtons::setTheme(bool dark)
{
    applyArrowColor(dark);
}

void ScrollJumpButtons::applyArrowColor(bool dark)
{
    const QColor source = dark ? QColor(0xe6, 0xed, 0xf3)
                               : QColor(0x1f, 0x23, 0x28);
    const QColor c = Theme::remapColor(source);
    m_top->setArrowColor(c);
    m_bottom->setArrowColor(c);
    m_bottom->setDotColor(Theme::remapColor(dark ? QColor(0x2f, 0x81, 0xf7)
                                                 : QColor(0x09, 0x69, 0xda)));
}

bool ScrollJumpButtons::eventFilter(QObject *obj, QEvent *e)
{
    if (obj == m_area->viewport() &&
        (e->type() == QEvent::Resize || e->type() == QEvent::Show))
        reposition();
    return QObject::eventFilter(obj, e);
}

void ScrollJumpButtons::reposition()
{
    QWidget *vp = m_area->viewport();
    const int m = 12, w = m_bottom->width(), h = m_bottom->height();
    const int y = vp->height() - h - m;
    m_bottom->move((vp->width() - w) / 2, y);
    m_top->move(vp->width() - w - m, y);
    m_top->raise();
    m_bottom->raise();
}

void ScrollJumpButtons::scheduleVisibilityUpdate()
{
    if (m_updatePending)
        return;
    m_updatePending = true;
    QTimer::singleShot(0, this, &ScrollJumpButtons::updateVisibility);
}

void ScrollJumpButtons::updateVisibility()
{
    m_updatePending = false;
    QScrollBar *sb = m_area->verticalScrollBar();
    const bool scrollable = sb->maximum() > sb->minimum();
    const bool offTail =
        scrollable && sb->value() < sb->maximum() - kEdgeSlack;
    const int extent = sb->maximum() - sb->minimum() + sb->pageStep();
    if (!offTail)
        m_seenExtent = extent;
    m_bottom->setFresh(offTail && (m_bottom->isFresh() || extent > m_seenExtent));
    m_bottom->setToolTip(m_bottom->isFresh()
                             ? QStringLiteral("New activity below. Jump to latest")
                             : QStringLiteral("Jump to latest"));
    m_top->setVisible(scrollable && sb->value() > sb->minimum() + kEdgeSlack);
    m_bottom->setVisible(offTail);
    reposition();
}
