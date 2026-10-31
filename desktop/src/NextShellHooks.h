#pragma once

#include <QColor>
#include <QSize>
#include <QString>

#include <functional>

class QPainter;
class QRect;
class QWidget;

namespace forkmesh::ui {

constexpr int kRailExpandedItemWidth = 196;
constexpr int kRailExpandedMinWidth = 160;
constexpr int kRailExpandedMaxWidth = 380;
constexpr int kRailRowHeight = 30;

struct RailButtonPaintState {
    QString iconName;
    QString label;
    QString stateGlyph;
    QColor stateGlyphDark;
    QColor stateGlyphLight;
    int badge = 0;
    int pendingSync = 0;
    int spinAngle = 0;
    bool badgeUrgent = false;
    bool alert = false;
    bool danger = false;
    bool accent = false;
    bool syncing = false;
    bool compact = false;
    bool badgeBeside = false;
    bool captionAlwaysVisible = true;
    bool captionVisible = false;
    bool blink = false;
    bool busy = false;
    bool desktopOffline = false;
    double desktopOfflineLevel = 1.0;
    std::function<void(QPainter &, const QRect &, const QColor &)> iconPainter;
};

struct TabButtonPaintState {
    int form = 0;
    qint64 badge = 0;
    qint64 alertBadge = 0;
    bool badgeAbove = false;
    bool attentionLight = false;
    bool attentionLightOn = false;
    bool inRail = false;
    QString iconName;
    QColor accent;
};

bool railExpanded();
void setRailExpanded(bool expanded);
int railExpandedItemWidth();
void setRailExpandedItemWidth(int width);
int railCompactItemWidth();

bool railButtonRowLayout(const QWidget *button, bool captionAlwaysVisible);
QSize railButtonSizeHook(const QWidget *button, const RailButtonPaintState &state);
bool paintRailButtonHook(QWidget *button, QPainter &painter,
                         const RailButtonPaintState &state);

QSize tabButtonSizeHook(const QWidget *button, const TabButtonPaintState &state);
bool paintTabButtonHook(QWidget *button, QPainter &painter,
                        const TabButtonPaintState &state);

bool paintRailFolderPopoutHook(QWidget *popout, QPainter &painter,
                               bool highlighted);
QColor toggleSwitchTrackHook(bool checked, const QColor &classic);
QColor nextHoverFill(const QColor &classic);
QColor nextAccentTint(const QColor &classic);
QColor nextSelectedFill(const QColor &classic);
void sectionCardHook(QWidget *card);

struct NextAccess;

}
