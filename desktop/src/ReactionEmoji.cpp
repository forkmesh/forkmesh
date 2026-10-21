#include "ReactionEmoji.h"

#include <QFont>
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QTransform>
#include <QtMath>

namespace reactions {
namespace {


QPen strokePen(const QColor &color, qreal width)
{
    QPen pen(color);
    pen.setWidthF(width);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    return pen;
}

const QColor kFeature(0x66, 0x45, 0x00); // classic emoji eye/mouth brown
const QColor kTearBlue(0x5d, 0xad, 0xec);

QBrush faceBrush()
{
    QRadialGradient g(QPointF(0.38, 0.30), 0.85);
    g.setColorAt(0.0, QColor(0xff, 0xe9, 0x7d));
    g.setColorAt(0.55, QColor(0xff, 0xd9, 0x3b));
    g.setColorAt(1.0, QColor(0xf5, 0xa6, 0x23));
    return QBrush(g);
}

QBrush handBrush()
{
    QLinearGradient g(QPointF(0.2, 0.1), QPointF(0.8, 0.95));
    g.setColorAt(0.0, QColor(0xff, 0xd7, 0x66));
    g.setColorAt(1.0, QColor(0xef, 0xa9, 0x2b));
    return QBrush(g);
}

void paintFaceBase(QPainter &p, const QBrush &brush = faceBrush())
{
    p.setPen(Qt::NoPen);
    p.setBrush(brush);
    p.drawEllipse(QRectF(0.03, 0.03, 0.94, 0.94));
}

void paintEye(QPainter &p, qreal cx, qreal cy, qreal w = 0.11, qreal h = 0.17)
{
    p.setPen(Qt::NoPen);
    p.setBrush(kFeature);
    p.drawEllipse(QRectF(cx - w / 2, cy - h / 2, w, h));
}

void paintClosedEye(QPainter &p, qreal cx, qreal cy, bool happy)
{
    QPainterPath path;
    if (happy) {
        path.moveTo(cx - 0.09, cy + 0.05);
        path.quadTo(cx, cy - 0.11, cx + 0.09, cy + 0.05);
    } else {
        path.moveTo(cx - 0.09, cy - 0.02);
        path.quadTo(cx, cy + 0.09, cx + 0.09, cy - 0.02);
    }
    p.setPen(strokePen(kFeature, 0.05));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void paintBrow(QPainter &p, qreal x1, qreal y1, qreal x2, qreal y2)
{
    QPainterPath path;
    path.moveTo(x1, y1);
    path.quadTo((x1 + x2) / 2, qMin(y1, y2) - 0.03, x2, y2);
    p.setPen(strokePen(kFeature, 0.045));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void paintSmileStroke(QPainter &p, qreal y = 0.62, qreal depth = 0.16,
                      qreal halfWidth = 0.21)
{
    QPainterPath path;
    path.moveTo(0.5 - halfWidth, y);
    path.quadTo(0.5, y + depth, 0.5 + halfWidth, y);
    p.setPen(strokePen(kFeature, 0.055));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void paintFrownStroke(QPainter &p, qreal y = 0.72, qreal depth = 0.13,
                      qreal halfWidth = 0.18)
{
    QPainterPath path;
    path.moveTo(0.5 - halfWidth, y);
    path.quadTo(0.5, y - depth, 0.5 + halfWidth, y);
    p.setPen(strokePen(kFeature, 0.055));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

void paintOpenSmile(QPainter &p, qreal top, qreal bottom, qreal halfWidth,
                    bool teeth)
{
    QPainterPath mouth;
    mouth.moveTo(0.5 - halfWidth, top);
    mouth.quadTo(0.5, top + 0.05, 0.5 + halfWidth, top);
    mouth.quadTo(0.5 + halfWidth * 0.9, bottom, 0.5, bottom);
    mouth.quadTo(0.5 - halfWidth * 0.9, bottom, 0.5 - halfWidth, top);
    mouth.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(kFeature);
    p.drawPath(mouth);
    if (teeth) {
        p.save();
        p.setClipPath(mouth);
        p.setBrush(Qt::white);
        p.drawRect(QRectF(0.5 - halfWidth, top, halfWidth * 2,
                          (bottom - top) * 0.34));
        p.restore();
    }
}

void paintTear(QPainter &p, qreal cx, qreal cy, qreal s)
{
    QPainterPath drop;
    drop.moveTo(cx, cy - s);
    drop.cubicTo(cx + s * 0.9, cy + s * 0.1, cx + s * 0.55, cy + s, cx,
                 cy + s);
    drop.cubicTo(cx - s * 0.55, cy + s, cx - s * 0.9, cy + s * 0.1, cx,
                 cy - s);
    p.setPen(Qt::NoPen);
    p.setBrush(kTearBlue);
    p.drawPath(drop);
}


void paintSmile(QPainter &p)
{
    paintFaceBase(p);
    paintEye(p, 0.34, 0.38);
    paintEye(p, 0.66, 0.38);
    paintOpenSmile(p, 0.56, 0.84, 0.23, true);
}

void paintLaugh(QPainter &p)
{
    paintFaceBase(p);
    paintClosedEye(p, 0.33, 0.38, true);
    paintClosedEye(p, 0.67, 0.38, true);
    paintOpenSmile(p, 0.52, 0.86, 0.24, true);
    paintTear(p, 0.145, 0.52, 0.085);
    paintTear(p, 0.855, 0.52, 0.085);
}

void paintWink(QPainter &p)
{
    paintFaceBase(p);
    paintEye(p, 0.34, 0.38);
    paintClosedEye(p, 0.66, 0.40, false);
    paintSmileStroke(p);
}

void paintCool(QPainter &p)
{
    paintFaceBase(p);
    const QColor lens(0x29, 0x2f, 0x33);
    p.setPen(Qt::NoPen);
    p.setBrush(lens);
    p.drawRoundedRect(QRectF(0.16, 0.32, 0.29, 0.21), 0.07, 0.07);
    p.drawRoundedRect(QRectF(0.55, 0.32, 0.29, 0.21), 0.07, 0.07);
    p.drawRect(QRectF(0.42, 0.35, 0.16, 0.05));
    p.drawRect(QRectF(0.05, 0.34, 0.12, 0.045));
    p.drawRect(QRectF(0.83, 0.34, 0.12, 0.045));
    p.setBrush(QColor(255, 255, 255, 70));
    p.drawEllipse(QRectF(0.20, 0.345, 0.10, 0.06));
    p.drawEllipse(QRectF(0.59, 0.345, 0.10, 0.06));
    paintSmileStroke(p, 0.66, 0.14, 0.19);
}

void paintThinking(QPainter &p)
{
    paintFaceBase(p);
    paintBrow(p, 0.25, 0.31, 0.42, 0.26); // raised left brow
    paintBrow(p, 0.58, 0.32, 0.75, 0.34);
    paintEye(p, 0.35, 0.42, 0.10, 0.14);
    paintEye(p, 0.67, 0.44, 0.10, 0.14);
    QPainterPath mouth; // small skeptical mouth, tilted down to the right
    mouth.moveTo(0.34, 0.70);
    mouth.quadTo(0.46, 0.75, 0.58, 0.67);
    p.setPen(strokePen(kFeature, 0.055));
    p.setBrush(Qt::NoBrush);
    p.drawPath(mouth);
}

void paintSurprised(QPainter &p)
{
    paintFaceBase(p);
    paintBrow(p, 0.26, 0.28, 0.42, 0.27);
    paintBrow(p, 0.58, 0.27, 0.74, 0.28);
    paintEye(p, 0.34, 0.41, 0.13, 0.15);
    paintEye(p, 0.66, 0.41, 0.13, 0.15);
    p.setPen(Qt::NoPen);
    p.setBrush(kFeature);
    p.drawEllipse(QRectF(0.41, 0.58, 0.18, 0.24));
}

void paintSad(QPainter &p)
{
    paintFaceBase(p);
    paintBrow(p, 0.26, 0.34, 0.41, 0.29); // inner ends raised
    paintBrow(p, 0.59, 0.29, 0.74, 0.34);
    paintEye(p, 0.34, 0.43);
    paintEye(p, 0.66, 0.43);
    paintFrownStroke(p);
    paintTear(p, 0.28, 0.60, 0.09);
}

void paintAngry(QPainter &p)
{
    QLinearGradient g(QPointF(0.5, 0.0), QPointF(0.5, 1.0));
    g.setColorAt(0.0, QColor(0xff, 0xd9, 0x3b));
    g.setColorAt(0.55, QColor(0xff, 0xa7, 0x26));
    g.setColorAt(1.0, QColor(0xe8, 0x66, 0x2d));
    paintFaceBase(p, QBrush(g));
    p.setPen(strokePen(kFeature, 0.05));
    p.setBrush(Qt::NoBrush);
    p.drawLine(QLineF(0.26, 0.31, 0.43, 0.40));
    p.drawLine(QLineF(0.74, 0.31, 0.57, 0.40));
    paintEye(p, 0.35, 0.48, 0.10, 0.13);
    paintEye(p, 0.65, 0.48, 0.10, 0.13);
    paintFrownStroke(p, 0.74, 0.12, 0.17);
}


void paintLike(QPainter &p)
{
    p.setPen(Qt::NoPen);
    p.setBrush(handBrush());
    p.save();
    p.translate(0.35, 0.34);
    p.rotate(-14);
    p.drawRoundedRect(QRectF(-0.08, -0.24, 0.16, 0.48), 0.08, 0.08);
    p.restore();
    p.drawRoundedRect(QRectF(0.30, 0.44, 0.48, 0.44), 0.10, 0.10);
    p.setPen(strokePen(QColor(0, 0, 0, 45), 0.018));
    for (qreal y : {0.555, 0.665, 0.775})
        p.drawLine(QLineF(0.44, y, 0.78, y));
}

void paintDislike(QPainter &p)
{
    p.save();
    p.translate(0.5, 0.5);
    p.scale(-1.0, -1.0);
    p.translate(-0.5, -0.5);
    paintLike(p);
    p.restore();
}

void paintPrayingHand(QPainter &p)
{
    p.setPen(Qt::NoPen);
    p.setBrush(handBrush());
    struct Finger { qreal x, tipY, angle; };
    const Finger fingers[] = {
        {0.545, 0.19, -4}, {0.600, 0.10, -1}, {0.655, 0.13, 2}, {0.710, 0.22, 6},
    };
    for (const Finger &f : fingers) {
        p.save();
        p.translate(f.x, 0.44);
        p.rotate(f.angle);
        p.drawRoundedRect(QRectF(-0.032, -(0.44 - f.tipY), 0.064, 0.44 - f.tipY),
                          0.032, 0.032);
        p.restore();
    }
    QPainterPath palm;
    palm.moveTo(0.503, 0.40);
    palm.cubicTo(0.50, 0.58, 0.505, 0.72, 0.535, 0.90);
    palm.lineTo(0.80, 0.90);
    palm.cubicTo(0.815, 0.72, 0.80, 0.54, 0.79, 0.42);
    palm.cubicTo(0.72, 0.37, 0.60, 0.36, 0.503, 0.40);
    palm.closeSubpath();
    p.drawPath(palm);
    p.save();
    p.translate(0.555, 0.63);
    p.rotate(46);
    p.drawRoundedRect(QRectF(-0.034, -0.01, 0.068, 0.22), 0.034, 0.034);
    p.restore();
}

void paintThanks(QPainter &p)
{
    paintPrayingHand(p);
    p.save();
    p.translate(1.0, 0.0);
    p.scale(-1.0, 1.0);
    paintPrayingHand(p);
    p.restore();
    p.setPen(strokePen(QColor(0, 0, 0, 60), 0.016));
    p.drawLine(QLineF(0.5, 0.38, 0.5, 0.90));
    p.setPen(strokePen(QColor(0xf0, 0xa9, 0x2b), 0.045));
    p.drawLine(QLineF(0.14, 0.22, 0.24, 0.31));
    p.drawLine(QLineF(0.06, 0.42, 0.19, 0.46));
    p.drawLine(QLineF(0.86, 0.22, 0.76, 0.31));
    p.drawLine(QLineF(0.94, 0.42, 0.81, 0.46));
}


void paintLove(QPainter &p)
{
    QPainterPath heart;
    heart.moveTo(0.5, 0.88);
    heart.cubicTo(0.13, 0.62, 0.03, 0.40, 0.10, 0.26);
    heart.cubicTo(0.17, 0.11, 0.39, 0.09, 0.5, 0.27);
    heart.cubicTo(0.61, 0.09, 0.83, 0.11, 0.90, 0.26);
    heart.cubicTo(0.97, 0.40, 0.87, 0.62, 0.5, 0.88);
    heart.closeSubpath();
    QLinearGradient g(QPointF(0.3, 0.12), QPointF(0.65, 0.9));
    g.setColorAt(0.0, QColor(0xff, 0x5f, 0x6d));
    g.setColorAt(1.0, QColor(0xd6, 0x1c, 0x4a));
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(g));
    p.drawPath(heart);
    p.setBrush(QColor(255, 255, 255, 90));
    p.save();
    p.translate(0.30, 0.28);
    p.rotate(-30);
    p.drawEllipse(QRectF(-0.09, -0.05, 0.18, 0.10));
    p.restore();
}

void paintHot(QPainter &p)
{
    QPainterPath flame;
    flame.moveTo(0.52, 0.04);
    flame.cubicTo(0.72, 0.22, 0.88, 0.38, 0.86, 0.60);
    flame.cubicTo(0.84, 0.81, 0.68, 0.95, 0.50, 0.95);
    flame.cubicTo(0.31, 0.95, 0.15, 0.81, 0.14, 0.60);
    flame.cubicTo(0.13, 0.47, 0.20, 0.40, 0.26, 0.30);
    flame.cubicTo(0.30, 0.36, 0.35, 0.38, 0.38, 0.34);
    flame.cubicTo(0.44, 0.26, 0.44, 0.14, 0.52, 0.04);
    flame.closeSubpath();
    QLinearGradient outer(QPointF(0.5, 0.05), QPointF(0.5, 0.95));
    outer.setColorAt(0.0, QColor(0xf4, 0x43, 0x36));
    outer.setColorAt(0.6, QColor(0xff, 0x7a, 0x1a));
    outer.setColorAt(1.0, QColor(0xff, 0x98, 0x00));
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(outer));
    p.drawPath(flame);
    QPainterPath inner;
    inner.moveTo(0.50, 0.44);
    inner.cubicTo(0.62, 0.56, 0.70, 0.64, 0.68, 0.76);
    inner.cubicTo(0.67, 0.87, 0.59, 0.93, 0.50, 0.93);
    inner.cubicTo(0.41, 0.93, 0.33, 0.87, 0.32, 0.76);
    inner.cubicTo(0.30, 0.64, 0.38, 0.56, 0.50, 0.44);
    inner.closeSubpath();
    QLinearGradient core(QPointF(0.5, 0.44), QPointF(0.5, 0.93));
    core.setColorAt(0.0, QColor(0xff, 0xc1, 0x07));
    core.setColorAt(1.0, QColor(0xff, 0xee, 0x58));
    p.setBrush(QBrush(core));
    p.drawPath(inner);
}

void paintCelebrate(QPainter &p)
{
    QPainterPath cone;
    cone.moveTo(0.10, 0.90);
    cone.lineTo(0.42, 0.34);
    cone.quadTo(0.55, 0.40, 0.62, 0.56);
    cone.closeSubpath();
    QLinearGradient g(QPointF(0.1, 0.9), QPointF(0.55, 0.45));
    g.setColorAt(0.0, QColor(0xff, 0x8f, 0x00));
    g.setColorAt(1.0, QColor(0xff, 0xc1, 0x07));
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(g));
    p.drawPath(cone);
    p.save();
    p.setClipPath(cone);
    p.setBrush(QColor(255, 255, 255, 80));
    p.save();
    p.translate(0.30, 0.66);
    p.rotate(28);
    p.drawRect(QRectF(-0.30, -0.03, 0.60, 0.06));
    p.restore();
    p.save();
    p.translate(0.44, 0.50);
    p.rotate(28);
    p.drawRect(QRectF(-0.30, -0.03, 0.60, 0.06));
    p.restore();
    p.restore();
    p.setBrush(Qt::NoBrush);
    p.setPen(strokePen(QColor(0x42, 0xa5, 0xf5), 0.035));
    QPainterPath s1;
    s1.moveTo(0.52, 0.32);
    s1.cubicTo(0.56, 0.20, 0.66, 0.16, 0.64, 0.06);
    p.drawPath(s1);
    p.setPen(strokePen(QColor(0xef, 0x53, 0x50), 0.035));
    QPainterPath s2;
    s2.moveTo(0.64, 0.46);
    s2.cubicTo(0.76, 0.42, 0.82, 0.34, 0.92, 0.34);
    p.drawPath(s2);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xef, 0x53, 0x50));
    p.drawEllipse(QRectF(0.70, 0.10, 0.075, 0.075));
    p.setBrush(QColor(0x66, 0xbb, 0x6a));
    p.drawEllipse(QRectF(0.86, 0.16, 0.07, 0.07));
    p.setBrush(QColor(0xff, 0xca, 0x28));
    p.drawEllipse(QRectF(0.80, 0.50, 0.07, 0.07));
    p.setBrush(QColor(0xab, 0x47, 0xbc));
    p.drawEllipse(QRectF(0.55, 0.14, 0.06, 0.06));
    p.setBrush(QColor(0x42, 0xa5, 0xf5));
    p.drawEllipse(QRectF(0.90, 0.44, 0.055, 0.055));
}

void paintStar(QPainter &p)
{
    QPainterPath star;
    const QPointF c(0.5, 0.53);
    const qreal outer = 0.47, inner = 0.19;
    for (int i = 0; i < 10; ++i) {
        const qreal r = (i % 2 == 0) ? outer : inner;
        const qreal angle = -M_PI / 2 + i * M_PI / 5;
        const QPointF pt(c.x() + r * qCos(angle), c.y() + r * qSin(angle));
        if (i == 0)
            star.moveTo(pt);
        else
            star.lineTo(pt);
    }
    star.closeSubpath();
    QLinearGradient g(QPointF(0.5, 0.06), QPointF(0.5, 1.0));
    g.setColorAt(0.0, QColor(0xff, 0xd5, 0x4f));
    g.setColorAt(1.0, QColor(0xff, 0xa0, 0x00));
    p.setPen(strokePen(QColor(0xe6, 0x8f, 0x00), 0.02));
    p.setBrush(QBrush(g));
    p.drawPath(star);
}

void paintRocket(QPainter &p)
{
    p.save();
    p.translate(0.5, 0.5);
    p.rotate(45);
    QLinearGradient body(QPointF(-0.13, 0), QPointF(0.13, 0));
    body.setColorAt(0.0, QColor(0xf5, 0xf7, 0xf8));
    body.setColorAt(1.0, QColor(0xb0, 0xbe, 0xc5));
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(body));
    p.drawRoundedRect(QRectF(-0.13, -0.40, 0.26, 0.64), 0.13, 0.13);
    QPainterPath nose;
    nose.moveTo(-0.128, -0.16);
    nose.quadTo(-0.11, -0.34, 0.0, -0.405);
    nose.quadTo(0.11, -0.34, 0.128, -0.16);
    nose.closeSubpath();
    p.setBrush(QColor(0xef, 0x53, 0x50));
    p.drawPath(nose);
    QPainterPath fin;
    fin.moveTo(-0.12, 0.02);
    fin.quadTo(-0.28, 0.16, -0.24, 0.30);
    fin.lineTo(-0.12, 0.22);
    fin.closeSubpath();
    p.drawPath(fin);
    p.save();
    p.scale(-1.0, 1.0);
    p.drawPath(fin);
    p.restore();
    p.setBrush(QColor(0x4f, 0xc3, 0xf7));
    p.setPen(strokePen(QColor(0x90, 0xa4, 0xae), 0.025));
    p.drawEllipse(QPointF(0.0, -0.10), 0.075, 0.075);
    QPainterPath flame;
    flame.moveTo(-0.07, 0.24);
    flame.quadTo(-0.10, 0.38, 0.0, 0.48);
    flame.quadTo(0.10, 0.38, 0.07, 0.24);
    flame.closeSubpath();
    QLinearGradient fg(QPointF(0, 0.24), QPointF(0, 0.48));
    fg.setColorAt(0.0, QColor(0xff, 0xa7, 0x26));
    fg.setColorAt(1.0, QColor(0xff, 0xee, 0x58));
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(fg));
    p.drawPath(flame);
    p.restore();
}

void paintEyes(QPainter &p)
{
    const QColor iris(0x35, 0x40, 0x4d);
    auto paintOne = [&](qreal x) {
        p.setPen(strokePen(QColor(0, 0, 0, 40), 0.015));
        p.setBrush(Qt::white);
        p.drawEllipse(QRectF(x, 0.16, 0.40, 0.68));
        p.setPen(Qt::NoPen);
        p.setBrush(iris);
        p.drawEllipse(QRectF(x + 0.025, 0.40, 0.19, 0.24));
        p.setBrush(QColor(255, 255, 255, 200));
        p.drawEllipse(QRectF(x + 0.075, 0.45, 0.055, 0.06));
    };
    paintOne(0.075);
    paintOne(0.525);
}

void paintCheck(QPainter &p)
{
    QLinearGradient g(QPointF(0.5, 0.06), QPointF(0.5, 0.94));
    g.setColorAt(0.0, QColor(0x66, 0xbb, 0x6a));
    g.setColorAt(1.0, QColor(0x38, 0x8e, 0x3c));
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(g));
    p.drawRoundedRect(QRectF(0.06, 0.06, 0.88, 0.88), 0.20, 0.20);
    QPainterPath check;
    check.moveTo(0.28, 0.53);
    check.lineTo(0.44, 0.69);
    check.lineTo(0.73, 0.33);
    p.setPen(strokePen(Qt::white, 0.11));
    p.setBrush(Qt::NoBrush);
    p.drawPath(check);
}

void heartPath(QPainterPath &heart, qreal cx, qreal top, qreal w, qreal h)
{
    // The love heart's curve, scaled into a (cx, top, w, h) box.
    auto pt = [&](qreal x, qreal y) {
        return QPointF(cx + (x - 0.5) * w / 0.87, top + (y - 0.09) * h / 0.79);
    };
    heart.moveTo(pt(0.5, 0.88));
    heart.cubicTo(pt(0.13, 0.62), pt(0.03, 0.40), pt(0.10, 0.26));
    heart.cubicTo(pt(0.17, 0.11), pt(0.39, 0.09), pt(0.5, 0.27));
    heart.cubicTo(pt(0.61, 0.09), pt(0.83, 0.11), pt(0.90, 0.26));
    heart.cubicTo(pt(0.97, 0.40), pt(0.87, 0.62), pt(0.5, 0.88));
    heart.closeSubpath();
}

void paintSparkle(QPainter &p, qreal cx, qreal cy, qreal r, const QColor &color)
{
    QPainterPath star;
    star.moveTo(cx, cy - r);
    star.quadTo(cx, cy, cx + r, cy);
    star.quadTo(cx, cy, cx, cy + r);
    star.quadTo(cx, cy, cx - r, cy);
    star.quadTo(cx, cy, cx, cy - r);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(star);
}

void paintSparkleHeart(QPainter &p)
{
    QPainterPath heart;
    heartPath(heart, 0.5, 0.14, 0.80, 0.74);
    QLinearGradient g(QPointF(0.3, 0.12), QPointF(0.65, 0.9));
    g.setColorAt(0.0, QColor(0xff, 0x8a, 0xc8));
    g.setColorAt(1.0, QColor(0xe0, 0x3a, 0x8c));
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(g));
    p.drawPath(heart);
    paintSparkle(p, 0.80, 0.16, 0.12, QColor(0xff, 0xd5, 0x4f));
    paintSparkle(p, 0.14, 0.70, 0.08, QColor(0xff, 0xd5, 0x4f));
    paintSparkle(p, 0.40, 0.42, 0.07, QColor(255, 255, 255, 200));
}

void paintHeartHands(QPainter &p)
{
    QPainterPath heart;
    heartPath(heart, 0.5, 0.10, 0.52, 0.48);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0xef, 0x45, 0x6a));
    p.drawPath(heart);
    p.setBrush(handBrush());
    p.setPen(strokePen(QColor(0xc9, 0x86, 0x1a), 0.025));
    QPainterPath left;
    left.moveTo(0.48, 0.62);
    left.cubicTo(0.30, 0.66, 0.14, 0.52, 0.12, 0.30);
    left.cubicTo(0.06, 0.40, 0.04, 0.62, 0.16, 0.80);
    left.cubicTo(0.26, 0.94, 0.42, 0.96, 0.48, 0.88);
    left.closeSubpath();
    p.drawPath(left);
    p.save();
    p.translate(1.0, 0.0);
    p.scale(-1.0, 1.0);
    p.drawPath(left);
    p.restore();
}

void paintHandshake(QPainter &p)
{
    p.setPen(strokePen(QColor(0xc9, 0x86, 0x1a), 0.03));
    p.setBrush(handBrush());
    QPainterPath left;
    left.moveTo(0.04, 0.40);
    left.lineTo(0.30, 0.34);
    left.cubicTo(0.46, 0.30, 0.62, 0.40, 0.70, 0.52);
    left.cubicTo(0.74, 0.60, 0.66, 0.66, 0.58, 0.62);
    left.lineTo(0.44, 0.56);
    left.lineTo(0.30, 0.70);
    left.lineTo(0.04, 0.66);
    left.closeSubpath();
    p.drawPath(left);
    p.setBrush(QColor(0xf5, 0xb9, 0x45));
    QPainterPath right;
    right.moveTo(0.96, 0.40);
    right.lineTo(0.72, 0.36);
    right.cubicTo(0.58, 0.34, 0.44, 0.44, 0.36, 0.56);
    right.cubicTo(0.30, 0.66, 0.40, 0.76, 0.52, 0.74);
    right.lineTo(0.70, 0.70);
    right.lineTo(0.96, 0.66);
    right.closeSubpath();
    p.drawPath(right);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x42, 0xa5, 0xf5));
    p.drawRect(QRectF(0.0, 0.36, 0.08, 0.34));
    p.setBrush(QColor(0x8e, 0x24, 0xaa));
    p.drawRect(QRectF(0.92, 0.36, 0.08, 0.34));
}

void paintGlowStar(QPainter &p)
{
    QRadialGradient glow(QPointF(0.5, 0.52), 0.5);
    glow.setColorAt(0.0, QColor(0xff, 0xf1, 0x76, 170));
    glow.setColorAt(1.0, QColor(0xff, 0xf1, 0x76, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(glow));
    p.drawEllipse(QRectF(0.0, 0.02, 1.0, 1.0));
    p.save();
    p.translate(0.12, 0.13);
    p.scale(0.76, 0.76);
    paintStar(p);
    p.restore();
    paintSparkle(p, 0.86, 0.14, 0.09, QColor(0xff, 0xca, 0x28));
    paintSparkle(p, 0.12, 0.84, 0.07, QColor(0xff, 0xca, 0x28));
}

void paintTrophy(QPainter &p)
{
    QLinearGradient gold(QPointF(0.2, 0.1), QPointF(0.8, 0.8));
    gold.setColorAt(0.0, QColor(0xff, 0xe0, 0x82));
    gold.setColorAt(1.0, QColor(0xf5, 0xa6, 0x23));
    p.setPen(strokePen(QColor(0xd4, 0x8a, 0x00), 0.05));
    p.setBrush(Qt::NoBrush);
    p.drawArc(QRectF(0.06, 0.16, 0.28, 0.30), 90 * 16, 180 * 16);
    p.drawArc(QRectF(0.66, 0.16, 0.28, 0.30), -90 * 16, 180 * 16);
    p.setPen(Qt::NoPen);
    p.setBrush(QBrush(gold));
    QPainterPath cup;
    cup.moveTo(0.20, 0.10);
    cup.lineTo(0.80, 0.10);
    cup.cubicTo(0.80, 0.46, 0.66, 0.60, 0.5, 0.62);
    cup.cubicTo(0.34, 0.60, 0.20, 0.46, 0.20, 0.10);
    cup.closeSubpath();
    p.drawPath(cup);
    p.drawRect(QRectF(0.44, 0.60, 0.12, 0.16));
    p.setBrush(QColor(0x8d, 0x6e, 0x63));
    p.drawRoundedRect(QRectF(0.26, 0.76, 0.48, 0.16), 0.04, 0.04);
    p.setBrush(QColor(255, 255, 255, 110));
    p.drawRoundedRect(QRectF(0.30, 0.16, 0.08, 0.26), 0.04, 0.04);
}

void paintThankYouSticker(QPainter &p)
{
    // The web's thank-you.svg, reduced to what reads at chip size: a tilted
    // sky-blue balloon with an ink outline and a pink "TY".
    p.save();
    p.translate(0.5, 0.5);
    p.rotate(-8);
    p.translate(-0.5, -0.5);
    QPainterPath balloon;
    balloon.addRoundedRect(QRectF(0.08, 0.14, 0.84, 0.56), 0.12, 0.12);
    QPainterPath tail;
    tail.moveTo(0.26, 0.66);
    tail.lineTo(0.16, 0.90);
    tail.lineTo(0.42, 0.68);
    tail.closeSubpath();
    balloon = balloon.united(tail);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0x1a, 0x1a, 0x1a));
    p.drawPath(balloon.translated(0.03, 0.04));
    p.setPen(strokePen(QColor(0x11, 0x11, 0x11), 0.045));
    p.setBrush(QColor(0x79, 0xc3, 0xf2));
    p.drawPath(balloon);
    QFont font;
    font.setBold(true);
    font.setWeight(QFont::Black);
    font.setPixelSize(32);
    QPainterPath text;
    text.addText(0, 0, font, QStringLiteral("TY"));
    const QRectF box = text.boundingRect();
    const qreal scale = qMin(0.30 / box.height(), 0.62 / box.width());
    QTransform place;
    place.translate(0.5 - box.center().x() * scale, 0.43 - box.center().y() * scale);
    place.scale(scale, scale);
    const QPainterPath letters = place.map(text);
    p.setPen(strokePen(QColor(0x11, 0x11, 0x11), 0.05));
    p.setBrush(QColor(0xe8, 0x43, 0x7b));
    p.drawPath(letters);
    p.restore();
    paintSparkle(p, 0.88, 0.10, 0.08, QColor(0x11, 0x11, 0x11));
}

using PaintFn = void (*)(QPainter &);

const QHash<QString, PaintFn> &painters()
{
    static const QHash<QString, PaintFn> kPainters = {
        {QStringLiteral("like"), paintLike},
        {QStringLiteral("love"), paintLove},
        {QStringLiteral("laugh"), paintLaugh},
        {QStringLiteral("celebrate"), paintCelebrate},
        {QStringLiteral("hot"), paintHot},
        {QStringLiteral("thanks"), paintThanks},
        {QStringLiteral("smile"), paintSmile},
        {QStringLiteral("wink"), paintWink},
        {QStringLiteral("cool"), paintCool},
        {QStringLiteral("thinking"), paintThinking},
        {QStringLiteral("surprised"), paintSurprised},
        {QStringLiteral("sad"), paintSad},
        {QStringLiteral("angry"), paintAngry},
        {QStringLiteral("dislike"), paintDislike},
        {QStringLiteral("eyes"), paintEyes},
        {QStringLiteral("star"), paintStar},
        {QStringLiteral("rocket"), paintRocket},
        {QStringLiteral("check"), paintCheck},
        {QStringLiteral("hearthands"), paintHeartHands},
        {QStringLiteral("sparkleheart"), paintSparkleHeart},
        {QStringLiteral("handshake"), paintHandshake},
        {QStringLiteral("glowstar"), paintGlowStar},
        {QStringLiteral("trophy"), paintTrophy},
        {QStringLiteral("thankyou"), paintThankYouSticker},
    };
    return kPainters;
}

} // namespace

QString displayName(const QString &value)
{
    const QString canonical = valueForUnicode(value);
    for (const CatalogEntry &entry : catalog()) {
        if (canonical == entry.value)
            return entry.label;
    }
    // A web custom reaction ("custom:party-parrot") has no drawing here; its
    // chip falls back to the name.
    if (value.startsWith(QLatin1String("custom:")))
        return QLatin1Char(':') + value.mid(7) + QLatin1Char(':');
    return value;
}

bool hasEmoji(const QString &value)
{
    return painters().contains(valueForUnicode(value));
}

QPixmap emojiPixmap(const QString &value, int size, qreal dpr)
{
    const QString canonical = valueForUnicode(value);
    const PaintFn fn = painters().value(canonical);
    if (!fn)
        return QPixmap();
    if (dpr <= 0)
        dpr = 1.0;
    // Vector-painted, and a chat transcript rebuild asks for one per reaction
    // chip per row; the artwork is a pure function of (emoji, size, dpr).
    static QHash<QString, QPixmap> cache;
    const QString key = canonical + QLatin1Char('|') + QString::number(size) +
                        QLatin1Char('|') + QString::number(dpr);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();
    const int px = qMax(1, qRound(size * dpr));
    QPixmap pixmap(px, px);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size, size);
    fn(p);
    p.end();
    if (cache.size() > 512)
        cache.clear();
    cache.insert(key, pixmap);
    return pixmap;
}

QPixmap addGlyph(int size, qreal dpr, const QColor &color)
{
    if (dpr <= 0)
        dpr = 1.0;
    static QHash<QString, QPixmap> cache;
    const QString key = QString::number(size) + QLatin1Char('|') +
                        QString::number(dpr) + QLatin1Char('|') +
                        QString::number(color.rgba(), 16);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();
    const int px = qMax(1, qRound(size * dpr));
    QPixmap pixmap(px, px);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.scale(size, size);
    p.setPen(strokePen(color, 0.075));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(QPointF(0.44, 0.56), 0.36, 0.36);
    QPainterPath smile;
    smile.moveTo(0.28, 0.62);
    smile.quadTo(0.44, 0.76, 0.60, 0.62);
    p.drawPath(smile);
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawEllipse(QPointF(0.32, 0.47), 0.045, 0.055);
    p.drawEllipse(QPointF(0.56, 0.47), 0.045, 0.055);
    p.setPen(strokePen(color, 0.085));
    p.drawLine(QLineF(0.84, 0.08, 0.84, 0.32));
    p.drawLine(QLineF(0.72, 0.20, 0.96, 0.20));
    p.end();
    if (cache.size() > 64)
        cache.clear();
    cache.insert(key, pixmap);
    return pixmap;
}

} // namespace reactions
