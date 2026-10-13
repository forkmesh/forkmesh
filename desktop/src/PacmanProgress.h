#pragma once

// A tiny Pac-Man-shaped progress chart: a circle whose green body
// fills as `progress` climbs from 0 to 1, leaving a wedge "mouth" that opens to
// the right and closes shut once the value reaches 100%. It shows how much of
// the current file has scrolled past, and lives on the diff's per-file header
// row — both where that row is rendered into the diff document and where the
// sticky bar pins a copy of it. Both surfaces rasterise this one
// painter into an inline image, so the chart is identical either side of the
// hand-off. Deliberately dependency-light: a pure painter, no widget, no moc.

#include <QColor>
#include <QPainter>
#include <QRectF>

#include <algorithm>

inline void paintPacmanProgress(QPainter &p, const QRectF &box, double progress,
                                const QColor &color)
{
    p.setRenderHint(QPainter::Antialiasing);
    const qreal m = 1.5;
    const QRectF r(box.x() + m, box.y() + m, box.width() - 2 * m,
                   box.height() - 2 * m);

    p.setPen(Qt::NoPen);
    QColor track = color;
    track.setAlpha(48);
    p.setBrush(track);
    p.drawEllipse(r);

    const int full = qRound(360.0 * 16.0 * std::clamp(progress, 0.0, 1.0));
    if (full > 0) {
        const int gap = 360 * 16 - full;
        p.setBrush(color);
        p.drawPie(r, gap / 2, full);
    }
}
