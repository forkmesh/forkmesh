#pragma once

#include <QApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QObject>
#include <QPointer>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QWidget>
#include <QWindow>

namespace forkmesh::ui {

inline QRect fitGeometryToScreen(QRect geometry, const QRect &available)
{
    if (!available.isValid() || !geometry.isValid())
        return geometry;
    geometry.setSize(geometry.size().boundedTo(available.size()));
    if (geometry.right() > available.right())
        geometry.moveRight(available.right());
    if (geometry.bottom() > available.bottom())
        geometry.moveBottom(available.bottom());
    if (geometry.left() < available.left())
        geometry.moveLeft(available.left());
    if (geometry.top() < available.top())
        geometry.moveTop(available.top());
    return geometry;
}

inline QRect resizedGeometry(const QRect &start, Qt::Edges edges, const QPoint &delta,
                             const QSize &minimum, const QSize &maximum)
{
    QRect geometry = start;
    if (edges.testFlag(Qt::LeftEdge)) {
        const int left = qBound(start.right() - maximum.width() + 1,
                                start.left() + delta.x(),
                                start.right() - minimum.width() + 1);
        geometry.setLeft(left);
    } else if (edges.testFlag(Qt::RightEdge)) {
        geometry.setWidth(
            qBound(minimum.width(), start.width() + delta.x(), maximum.width()));
    }
    if (edges.testFlag(Qt::TopEdge)) {
        const int top = qBound(start.bottom() - maximum.height() + 1,
                               start.top() + delta.y(),
                               start.bottom() - minimum.height() + 1);
        geometry.setTop(top);
    } else if (edges.testFlag(Qt::BottomEdge)) {
        geometry.setHeight(
            qBound(minimum.height(), start.height() + delta.y(), maximum.height()));
    }
    return geometry;
}

class WindowFrameControl : public QObject
{
public:
    static constexpr int kResizeBorder = 8;

    explicit WindowFrameControl(QWidget *window)
        : QObject(window), m_window(window)
    {
        setObjectName(QStringLiteral("windowFrameControl"));
        qApp->installEventFilter(this);
        QTimer::singleShot(0, this, [this] { watchScreen(); });
    }

    bool resizing() const { return m_edges != Qt::Edges(); }
    bool moving() const { return m_moving; }

    void fitToScreen()
    {
        if (!m_window || !m_window->isVisible() || m_window->isMaximized() ||
            m_window->isFullScreen() || resizing() || moving())
            return;
        const QScreen *screen = m_window->screen();
        if (!screen)
            return;
        const QRect current = m_window->geometry();
        const QRect fitted = fitGeometryToScreen(current, screen->availableGeometry());
        if (fitted != current)
            m_window->setGeometry(fitted);
    }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override
    {
        if (!m_window)
            return false;
        switch (event->type()) {
        case QEvent::MouseButtonPress:
            return beginResize(watched, static_cast<QMouseEvent *>(event)) ||
                   beginMove(watched, static_cast<QMouseEvent *>(event));
        case QEvent::MouseMove: {
            const QPoint delta =
                static_cast<QMouseEvent *>(event)->globalPosition().toPoint() -
                m_startPos;
            if (resizing()) {
                m_window->setGeometry(
                    resizedGeometry(m_startGeometry, m_edges, delta,
                                    m_window->minimumSize(), m_window->maximumSize()));
                return true;
            }
            if (moving()) {
                m_window->move(m_startGeometry.topLeft() + delta);
                return true;
            }
            return false;
        }
        case QEvent::MouseButtonRelease:
            if (!resizing() && !moving())
                return false;
            endResize();
            return true;
        case QEvent::Show:
            if (watched == m_window) {
                QTimer::singleShot(0, this, [this] {
                    watchScreen();
                    fitToScreen();
                });
            }
            return false;
        case QEvent::WindowDeactivate:
        case QEvent::Hide:
            if (watched == m_window && (resizing() || moving()))
                endResize();
            return false;
        default:
            return false;
        }
    }

private:
    Qt::Edges edgesAt(const QPoint &globalPos) const
    {
        if (!m_window->isVisible() || m_window->isMaximized() ||
            m_window->isFullScreen())
            return {};
        const QPoint pos = m_window->mapFromGlobal(globalPos);
        const QRect area = m_window->rect();
        if (!area.adjusted(-1, -1, 1, 1).contains(pos))
            return {};
        Qt::Edges edges;
        if (pos.x() <= area.left() + kResizeBorder)
            edges |= Qt::LeftEdge;
        if (pos.x() >= area.right() - kResizeBorder)
            edges |= Qt::RightEdge;
        if (pos.y() <= area.top() + kResizeBorder)
            edges |= Qt::TopEdge;
        if (pos.y() >= area.bottom() - kResizeBorder)
            edges |= Qt::BottomEdge;
        return edges;
    }

    bool beginResize(QObject *watched, QMouseEvent *mouse)
    {
        if (resizing() || mouse->button() != Qt::LeftButton ||
            !watched->isWidgetType() ||
            static_cast<QWidget *>(watched)->window() != m_window)
            return false;
        const QPoint globalPos = mouse->globalPosition().toPoint();
        const Qt::Edges edges = edgesAt(globalPos);
        if (!edges)
            return false;
        mouse->accept();
        QWindow *handle = m_window->windowHandle();
        if (handle && handle->startSystemResize(edges))
            return true;
        m_edges = edges;
        m_startGeometry = m_window->geometry();
        m_startPos = globalPos;
        m_window->grabMouse();
        return true;
    }

    bool beginMove(QObject *watched, QMouseEvent *mouse)
    {
        if (moving() || mouse->button() != Qt::LeftButton ||
            watched->objectName() != QLatin1String("windowChromeBar") ||
            !watched->isWidgetType() ||
            static_cast<QWidget *>(watched)->window() != m_window ||
            m_window->isMaximized() || m_window->isFullScreen())
            return false;
        QWindow *handle = m_window->windowHandle();
        if (handle && handle->startSystemMove()) {
            mouse->accept();
            return true;
        }
        mouse->accept();
        m_moving = true;
        m_startGeometry = m_window->geometry();
        m_startPos = mouse->globalPosition().toPoint();
        m_window->grabMouse();
        return true;
    }

    void endResize()
    {
        m_edges = {};
        m_moving = false;
        if (m_window)
            m_window->releaseMouse();
    }

    void watchScreen()
    {
        QWindow *handle = m_window ? m_window->windowHandle() : nullptr;
        if (!handle || m_watched == handle)
            return;
        m_watched = handle;
        connect(handle, &QWindow::screenChanged, this, [this] {
            QTimer::singleShot(0, this, [this] {
                if (m_watched)
                    watchGeometry(m_watched->screen());
                fitToScreen();
            });
        });
        watchGeometry(handle->screen());
    }

    void watchGeometry(QScreen *screen)
    {
        if (!screen || m_screens.contains(screen) ||
            !QGuiApplication::screens().contains(screen))
            return;
        m_screens.insert(screen);
        connect(screen, &QScreen::availableGeometryChanged, this,
                [this](const QRect &) { fitToScreen(); });
        connect(screen, &QObject::destroyed, this,
                [this, screen] { m_screens.remove(screen); });
    }

    QPointer<QWidget> m_window;
    QPointer<QWindow> m_watched;
    QSet<QScreen *> m_screens;
    Qt::Edges m_edges;
    bool m_moving = false;
    QRect m_startGeometry;
    QPoint m_startPos;
};

}
