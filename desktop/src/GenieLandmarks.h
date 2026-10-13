#pragma once

#include <QList>
#include <QRandomGenerator>
#include <QRectF>
#include <QString>

#include <functional>

class QWidget;

enum class GenieMarkKind { Button, Tab, Heading, Label, Picture, Input, Row, Progress };

struct GenieMark {
    GenieMarkKind kind = GenieMarkKind::Button;
    QRectF rect;
    int group = -1;
};

struct GenieScene {
    QList<GenieMark> marks;
    QList<QRectF> blocks;
    bool quiet = false;
};

struct GenieStage {
    double left = 0;
    double right = 0;
    double side = 78;
    std::function<double(double)> floor;
    QList<QRectF> blocks;
};

enum class GenieInteraction {
    None,
    Peek,
    Read,
    Wave,
    Point,
    Sniff,
    Cheer,
    TabNod,
    Silk,
    Hop,
    Eureka,
    Startle,
    Giggle,
    Count,
    Compare,
    Poof,
    Levitate,
    Lamp,
    Wishes,
    SmokeRings,
    Shimmer,
    Stardust,
    Carpet,
    Vortex,
    Crystal,
    Runes,
    Halo,
    Fireflies,
    Whirl,
    Grow,
    Bottle,
    Rainbow,
    Fireworks,
    Bubbles,
    Glitter,
    JewelBeam,
    Ripple,
    Moons,
    Constellation,
    Crescent,
    Mirage,
    Coins,
    Flourish,
    Incense,
    Whirlwind
};

constexpr int kGenieInteractionCount = 44;
constexpr int kGenieMagicCount = 30;

struct GeniePlan {
    GenieInteraction kind = GenieInteraction::None;
    QList<QRectF> marks;
    double spot = -1;
    bool valid() const { return kind != GenieInteraction::None && !marks.isEmpty(); }
};

namespace GenieKeepClear {

QRectF body(double x, double floor, double side);
double coverage(const QRectF &body, const QList<QRectF> &blocks);
double coverageAt(const GenieStage &stage, double x, const QList<QRectF> &extra = {});
double clearSpot(const GenieStage &stage, double wanted, const QList<QRectF> &extra = {});

}

namespace GeniePlay {

QString name(GenieInteraction kind);
QList<GenieInteraction> all();
bool magic(GenieInteraction kind);
bool fits(GenieInteraction kind, GenieMarkKind mark);
GeniePlan plan(GenieInteraction kind, const QList<GenieMark> &marks, const GenieStage &stage, double from,
               quint32 roll);

}

class GenieInteractionBag
{
public:
    explicit GenieInteractionBag(quint32 seed);

    GenieInteraction take(const std::function<bool(GenieInteraction)> &possible);
    int left() const { return int(m_bag.size()); }

private:
    QList<GenieInteraction> shuffled();

    QList<GenieInteraction> m_bag;
    QRandomGenerator m_random;
};

namespace GenieLandmarks {

GenieScene scan(QWidget *host, const QList<const QWidget *> &skip);

}
