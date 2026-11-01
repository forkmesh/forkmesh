#pragma once

#include <QColor>
#include <QConicalGradient>
#include <QElapsedTimer>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>

#include <functional>

class QAbstractButton;
class QObject;
class QWidget;

namespace forkmesh::ui::busyring {

constexpr int kTurnMs = 1600;
constexpr qreal kArc = 0.7;
constexpr int kFrameMs = 33;
constexpr qreal kStroke = 1.75;

inline qreal phase()
{
    static QElapsedTimer clock;
    if (!clock.isValid())
        clock.start();
    return qreal(clock.elapsed() % kTurnMs) / kTurnMs;
}

inline void paint(QPainter &p, const QRectF &bounds, const QColor &accent,
                  qreal turn, qreal radius = -1)
{
    if (bounds.isEmpty())
        return;
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF ring = bounds.adjusted(kStroke / 2, kStroke / 2, -kStroke / 2,
                                        -kStroke / 2);
    QPainterPath path;
    if (radius < 0)
        path.addEllipse(ring);
    else
        path.addRoundedRect(ring, radius, radius);
    QColor track = accent;
    track.setAlphaF(0.14);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(track, kStroke));
    p.drawPath(path);
    QConicalGradient sweep(ring.center(), -360.0 * turn);
    QColor tail = accent;
    tail.setAlphaF(0);
    sweep.setColorAt(0.0, accent);
    sweep.setColorAt(kArc, tail);
    sweep.setColorAt(1.0, tail);
    p.setPen(QPen(QBrush(sweep), kStroke, Qt::SolidLine, Qt::RoundCap));
    p.drawPath(path);
    p.restore();
}

inline QRectF aroundIcon(const QRectF &icon)
{
    return icon.adjusted(-4, -4, 4, 4);
}

QColor accent();
QRectF iconRect(const QAbstractButton *button);
void setBusy(QWidget *host, bool busy, QWidget *anchor = nullptr);
bool isBusy(const QWidget *host);
void observe(QObject *context, std::function<void()> changed);
void watchClick(QAbstractButton *button);
void installClickWatcher();

}
