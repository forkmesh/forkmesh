#pragma once

#include <QColor>
#include <QImage>
#include <QVector>

namespace forkmesh::activity {

inline constexpr int kActivityLevels = 5;
inline constexpr int kActivityCellPitch = 20;
inline constexpr int kActivityCellSize = 16;
inline constexpr int kActivityLabelWidth = 30;
inline constexpr int kActivityAxisHeight = 18;
inline constexpr int kActivityLegendHeight = 22;

struct ActivitySummary {
    int total = 0;
    int peak = 0;
    int peakSlot = -1;
    int peakHour = -1;
    int peakHourTotal = 0;
    int activeSlots = 0;
};

ActivitySummary summarizeActivity(const QVector<int> &counts);
int activityLevel(int value, int peak);
QColor activityLevelColor(int level, bool dark);
QSize activityHeatSize();
QImage activityHeatImage(const QVector<int> &counts, int nowSlot, bool dark);

}
