#pragma once


#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QRect>
#include <QSet>
#include <QVariantList>

#include <algorithm>

namespace forkmesh::ui {

constexpr int kGraphLaneWidth = 12;
constexpr int kGraphMargin = 8;
constexpr int kGraphMaxTextIndent = 160;
constexpr qreal kGraphLaneMinWidth = 5.0;
constexpr qreal kGraphNodeOuter = 4.5; // outer ring radius
constexpr qreal kGraphNodeInner = 2.0; // centre-dot radius

const char kGraphLaneCountProperty[] = "forkmeshGraphLaneCount";

struct CommitGraphMetrics
{
    qreal laneWidth = kGraphLaneWidth;
    qreal nodeOuter = kGraphNodeOuter;
    qreal nodeInner = kGraphNodeInner;
    qreal penWidth = 2.0;
};

inline CommitGraphMetrics commitGraphMetrics(int laneCount)
{
    CommitGraphMetrics m;
    const qreal avail = kGraphMaxTextIndent - kGraphMargin;
    if (laneCount > 0 && laneCount * m.laneWidth > avail)
        m.laneWidth = std::max(kGraphLaneMinWidth, avail / laneCount);
    m.nodeOuter = std::max(2.0, std::min(kGraphNodeOuter, m.laneWidth * 0.375));
    m.nodeInner = std::max(1.0, m.nodeOuter * 0.45);
    m.penWidth = std::max(1.0, std::min(2.0, m.laneWidth / 6.0));
    return m;
}

inline int commitGraphTextIndent(int rowMaxLane, const CommitGraphMetrics &m)
{
    return std::min<int>(kGraphMargin + qRound((rowMaxLane + 1) * m.laneWidth),
                         kGraphMaxTextIndent);
}

inline QColor commitGraphLaneColor(int lane)
{
    static const QColor palette[] = {
        QColor("#58a6ff"), QColor("#d29922"), QColor("#db61a2"),
        QColor("#bc8cff"), QColor("#39c5cf"), QColor("#3fb950"),
    };
    constexpr int n = int(sizeof(palette) / sizeof(palette[0]));
    return palette[((lane % n) + n) % n];
}

inline void paintCommitGraphGutter(QPainter *painter, const QRect &r,
                                   const QVariantList &topLanes,
                                   const QVariantList &botLanes, int nodeLane,
                                   bool isMerge, bool isOutgoing,
                                   const CommitGraphMetrics &metrics)
{
    if (topLanes.isEmpty() && botLanes.isEmpty() && nodeLane < 0)
        return;
    const qreal yTop = r.top();
    const qreal yBot = r.top() + r.height(); // meets the next row's top edge
    const qreal yMid = r.center().y() + 0.5;
    auto laneX = [&](int lane) -> qreal {
        return r.left() + kGraphMargin + lane * metrics.laneWidth;
    };
    QSet<int> topSet;
    QSet<int> botSet;
    int maxLane = nodeLane;
    for (const QVariant &v : topLanes) {
        const int l = v.toInt();
        topSet.insert(l);
        maxLane = std::max(maxLane, l);
    }
    for (const QVariant &v : botLanes) {
        const int l = v.toInt();
        botSet.insert(l);
        maxLane = std::max(maxLane, l);
    }

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setClipRect(QRect(r.left(), r.top(), kGraphMaxTextIndent,
                               r.height()),
                         Qt::IntersectClip);

    const qreal trim = (isMerge && nodeLane >= 0) ? metrics.nodeOuter : 0.0;

    auto strokePath = [&](const QPainterPath &path, const QColor &c) {
        QPen pen(c, metrics.penWidth);
        pen.setCapStyle(Qt::RoundCap);
        pen.setJoinStyle(Qt::RoundJoin);
        painter->setPen(pen);
        painter->setBrush(Qt::NoBrush);
        painter->drawPath(path);
    };
    auto straight = [&](qreal x, qreal y0, qreal y1, const QColor &c) {
        QPainterPath path(QPointF(x, y0));
        path.lineTo(QPointF(x, y1));
        strokePath(path, c);
    };
    auto loopIn = [&](int lane, const QColor &c) {
        const qreal x0 = laneX(lane);
        const qreal x1 = laneX(nodeLane);
        const qreal rad = qMax(0.0, qMin(qAbs(x1 - x0) - trim, yMid - yTop));
        const qreal sx = (x1 > x0) ? 1.0 : -1.0;
        QPainterPath path(QPointF(x0, yTop));
        path.lineTo(QPointF(x0, yMid - rad));
        path.quadTo(QPointF(x0, yMid), QPointF(x0 + sx * rad, yMid));
        path.lineTo(QPointF(x1 - sx * trim, yMid));
        strokePath(path, c);
    };
    auto loopOut = [&](int lane, const QColor &c) {
        const qreal x0 = laneX(nodeLane);
        const qreal x1 = laneX(lane);
        const qreal rad = qMax(0.0, qMin(qAbs(x1 - x0) - trim, yBot - yMid));
        const qreal sx = (x1 > x0) ? 1.0 : -1.0;
        QPainterPath path(QPointF(x0 + sx * trim, yMid));
        path.lineTo(QPointF(x1 - sx * rad, yMid));
        path.quadTo(QPointF(x1, yMid), QPointF(x1, yMid + rad));
        path.lineTo(QPointF(x1, yBot));
        strokePath(path, c);
    };

    for (int lane = 0; lane <= maxLane; ++lane) {
        if (lane == nodeLane)
            continue;
        const bool inTop = topSet.contains(lane);
        const bool inBot = botSet.contains(lane);
        const QColor c = commitGraphLaneColor(lane);
        if (inTop && inBot)
            straight(laneX(lane), yTop, yBot, c);
        else if (inTop)
            nodeLane >= 0 ? loopIn(lane, c) : straight(laneX(lane), yTop, yMid, c);
        else if (inBot)
            nodeLane >= 0 ? loopOut(lane, c) : straight(laneX(lane), yMid, yBot, c);
    }

    if (nodeLane >= 0) {
        const QColor c = commitGraphLaneColor(nodeLane);
        const qreal nx = laneX(nodeLane);
        if (topSet.contains(nodeLane))
            straight(nx, yTop, yMid - trim, c);
        if (botSet.contains(nodeLane))
            straight(nx, yMid + trim, yBot, c);
        if (isOutgoing) {
            painter->setBrush(Qt::NoBrush);
            QPen pen(c, metrics.penWidth * 0.9, Qt::DotLine);
            pen.setCapStyle(Qt::RoundCap);
            painter->setPen(pen);
            painter->drawEllipse(QPointF(nx, yMid), metrics.nodeOuter,
                                 metrics.nodeOuter);
        } else if (isMerge) {
            painter->setBrush(Qt::NoBrush);
            painter->setPen(QPen(c, metrics.penWidth));
            painter->drawEllipse(QPointF(nx, yMid), metrics.nodeOuter,
                                 metrics.nodeOuter);
            painter->setPen(Qt::NoPen);
            painter->setBrush(c);
            painter->drawEllipse(QPointF(nx, yMid), metrics.nodeInner,
                                 metrics.nodeInner);
        } else {
            painter->setPen(Qt::NoPen);
            painter->setBrush(c);
            const qreal dot = std::max(1.5, metrics.nodeOuter - 0.7);
            painter->drawEllipse(QPointF(nx, yMid), dot, dot);
        }
    }
    painter->restore();
}

} // namespace forkmesh::ui
