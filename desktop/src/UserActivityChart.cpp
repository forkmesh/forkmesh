#include "UserActivityChart.h"

#include "UserActivityLog.h"

#include <QFont>
#include <QPainter>

#include <cmath>

namespace forkmesh::activity {

ActivitySummary summarizeActivity(const QVector<int> &counts)
{
    ActivitySummary summary;
    QVector<int> hours(kActivityHours, 0);
    for (int i = 0; i < qMin(int(counts.size()), kActivitySlots); ++i) {
        const int value = qMax(0, counts.at(i));
        if (value <= 0)
            continue;
        summary.total += value;
        ++summary.activeSlots;
        hours[i / kActivitySlotsPerHour] += value;
        if (value > summary.peak) {
            summary.peak = value;
            summary.peakSlot = i;
        }
    }
    for (int hour = 0; hour < kActivityHours; ++hour) {
        if (hours.at(hour) > summary.peakHourTotal) {
            summary.peakHourTotal = hours.at(hour);
            summary.peakHour = hour;
        }
    }
    return summary;
}

int activityLevel(int value, int peak)
{
    if (value <= 0 || peak <= 0)
        return 0;
    const int level = int(std::ceil(4.0 * value / peak));
    return qBound(1, level, kActivityLevels - 1);
}

QColor activityLevelColor(int level, bool dark)
{
    static const char *const kLight[kActivityLevels] = {
        "#ebedf0", "#9be9a8", "#40c463", "#30a14e", "#216e39"};
    static const char *const kDark[kActivityLevels] = {
        "#21262d", "#0e4429", "#006d32", "#26a641", "#39d353"};
    const int index = qBound(0, level, kActivityLevels - 1);
    return QColor(QLatin1String(dark ? kDark[index] : kLight[index]));
}

QSize activityHeatSize()
{
    return QSize(kActivityLabelWidth + kActivityHours * kActivityCellPitch,
                 kActivitySlotsPerHour * kActivityCellPitch +
                     kActivityAxisHeight + kActivityLegendHeight);
}

QImage activityHeatImage(const QVector<int> &counts, int nowSlot, bool dark)
{
    const QSize size = activityHeatSize();
    QImage image(size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor text(QLatin1String(dark ? "#8b949e" : "#57606a"));
    const QColor ring(QLatin1String(dark ? "#e6edf3" : "#1f2328"));
    const int peak = summarizeActivity(counts).peak;
    const int gap = kActivityCellPitch - kActivityCellSize;
    const auto cellRect = [&](int column, int row) {
        return QRectF(kActivityLabelWidth + column * kActivityCellPitch + gap / 2.0,
                      row * kActivityCellPitch + gap / 2.0, kActivityCellSize,
                      kActivityCellSize);
    };

    painter.setPen(Qt::NoPen);
    for (int hour = 0; hour < kActivityHours; ++hour) {
        for (int row = 0; row < kActivitySlotsPerHour; ++row) {
            const int slot = hour * kActivitySlotsPerHour + row;
            const int value = slot < counts.size() ? counts.at(slot) : 0;
            QColor fill = activityLevelColor(activityLevel(value, peak), dark);
            if (nowSlot >= 0 && slot > nowSlot && value <= 0)
                fill.setAlphaF(0.4);
            painter.setBrush(fill);
            painter.drawRoundedRect(cellRect(hour, row), 3, 3);
        }
    }

    if (nowSlot >= 0 && nowSlot < kActivitySlots) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(ring, 1.5));
        painter.drawRoundedRect(cellRect(nowSlot / kActivitySlotsPerHour,
                                         nowSlot % kActivitySlotsPerHour)
                                    .adjusted(-1, -1, 1, 1),
                                4, 4);
    }

    QFont label = painter.font();
    label.setPixelSize(10);
    painter.setFont(label);
    painter.setPen(text);
    for (int row = 0; row < kActivitySlotsPerHour; row += 3) {
        const QRectF cell = cellRect(0, row);
        painter.drawText(QRectF(0, cell.top() - 2, kActivityLabelWidth - 4,
                                cell.height() + 4),
                         Qt::AlignRight | Qt::AlignVCenter,
                         QStringLiteral(":%1").arg(row * kActivitySlotMinutes, 2,
                                                   10, QLatin1Char('0')));
    }
    const int gridBottom = kActivitySlotsPerHour * kActivityCellPitch;
    for (int hour = 0; hour < kActivityHours; hour += 3) {
        const QRectF cell = cellRect(hour, 0);
        painter.drawText(QRectF(cell.center().x() - 20, gridBottom, 40,
                                kActivityAxisHeight),
                         Qt::AlignHCenter | Qt::AlignVCenter,
                         QStringLiteral("%1").arg(hour, 2, 10, QLatin1Char('0')));
    }

    const int legendTop = gridBottom + kActivityAxisHeight + 6;
    const int swatch = 12;
    const int swatchPitch = swatch + 3;
    const int moreWidth = 30;
    const int legendRight = size.width() - gap / 2;
    const int swatchesLeft = legendRight - moreWidth - kActivityLevels * swatchPitch;
    painter.drawText(QRectF(swatchesLeft - 34, legendTop - 2, 30, swatch + 4),
                     Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Less"));
    painter.drawText(QRectF(legendRight - moreWidth + 4, legendTop - 2,
                            moreWidth - 4, swatch + 4),
                     Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("More"));
    painter.setPen(Qt::NoPen);
    for (int level = 0; level < kActivityLevels; ++level) {
        painter.setBrush(activityLevelColor(level, dark));
        painter.drawRoundedRect(QRectF(swatchesLeft + level * swatchPitch,
                                       legendTop, swatch, swatch),
                                2, 2);
    }
    return image;
}

}
