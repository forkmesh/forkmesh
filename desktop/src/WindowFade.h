#pragma once

#include <QChildEvent>
#include <QColor>
#include <QEvent>
#include <QGuiApplication>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QTimer>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace forkmesh::ui {

class WindowFade : public QWidget
{
public:
    static constexpr qreal kCornerRadius = 10.0;

    static bool platformNeedsFade()
    {
        return QGuiApplication::platformName().startsWith(
            QLatin1String("wayland"));
    }

    static void prepareHost(QWidget *host)
    {
        host->setAttribute(Qt::WA_TranslucentBackground);
        host->setAttribute(Qt::WA_NoSystemBackground, false);
    }

    explicit WindowFade(QWidget *host) : QWidget(host)
    {
        setObjectName(QStringLiteral("windowFade"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setAttribute(Qt::WA_TranslucentBackground);
        setFocusPolicy(Qt::NoFocus);
        host->installEventFilter(this);
        for (QObject *child : host->children())
            if (child != this && child->isWidgetType())
                child->installEventFilter(this);
        hide();
        sync();
    }

    void setOpacityPercent(int percent)
    {
        m_percent = std::clamp(percent, 0, 100);
        sync();
        if (!isHidden())
            raise();
    }

    int opacityPercent() const { return m_percent; }

    bool roundsCorners() const
    {
        const QWidget *host = parentWidget();
        return host && !(host->windowState() &
                         (Qt::WindowMaximized | Qt::WindowFullScreen));
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (watched == parent()) {
            switch (event->type()) {
            case QEvent::Resize:
            case QEvent::WindowStateChange:
                sync();
                break;
            case QEvent::ChildAdded: {
                QObject *child = static_cast<QChildEvent *>(event)->child();
                if (child != this && child->isWidgetType())
                    child->installEventFilter(this);
                scheduleRaise();
                break;
            }
            default:
                break;
            }
        } else if (event->type() == QEvent::ZOrderChange) {
            scheduleRaise();
        }
        return QWidget::eventFilter(watched, event);
    }

    void paintEvent(QPaintEvent *event) override
    {
        QPainter painter(this);
        painter.setClipRegion(event->region());
        if (m_percent < 100) {
            painter.setCompositionMode(QPainter::CompositionMode_DestinationIn);
            painter.fillRect(rect(), QColor(0, 0, 0, m_percent * 255 / 100));
        }
        if (roundsCorners()) {
            QPainterPath outside;
            outside.setFillRule(Qt::OddEvenFill);
            outside.addRect(QRectF(rect()));
            outside.addRoundedRect(QRectF(rect()), kCornerRadius, kCornerRadius);
            painter.setRenderHint(QPainter::Antialiasing, true);
            painter.setCompositionMode(QPainter::CompositionMode_DestinationOut);
            painter.fillPath(outside, Qt::black);
        }
    }

private:
    QRegion cornerRegion() const
    {
        const int side = static_cast<int>(std::ceil(kCornerRadius));
        const QRect bounds = rect();
        QRegion region(0, 0, side, side);
        region += QRect(bounds.right() - side + 1, 0, side, side);
        region += QRect(0, bounds.bottom() - side + 1, side, side);
        region += QRect(bounds.right() - side + 1, bounds.bottom() - side + 1,
                        side, side);
        return region;
    }

    void sync()
    {
        setGeometry(parentWidget()->rect());
        const bool fading = m_percent < 100;
        const bool corners = roundsCorners();
        if (fading || !corners)
            clearMask();
        else
            setMask(cornerRegion());
        const bool visible = fading || corners;
        const bool shown = !isHidden();
        setVisible(visible);
        if (visible && !shown)
            raise();
        update();
    }

    void scheduleRaise()
    {
        if (!isVisible() || m_raisePending)
            return;
        m_raisePending = true;
        QTimer::singleShot(0, this, [this] {
            m_raisePending = false;
            if (isVisible())
                raise();
        });
    }

    int m_percent = 100;
    bool m_raisePending = false;
};

} // namespace forkmesh::ui
