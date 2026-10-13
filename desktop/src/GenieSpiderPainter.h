#pragma once

#include "GenieLandmarks.h"
#include "GenieVitals.h"

#include <QPointF>
#include <QRectF>
#include <QString>

class QPainter;

enum class GenieMood { Idle, Greeting, Thinking, Speaking };

enum class GenieReaction { None, Wave, Cheer, Nod, Eureka, Giggle, Surprise, Dizzy, Dance };

enum class GenieDance { Disco, Twist, Jig, Ripple, Spin, JazzHands, Moonwalk, Headbang, Robot, Cancan };

constexpr int kGenieDanceCount = 10;

struct GenieSpiderPose {
    double t = 0;
    GenieMood mood = GenieMood::Idle;
    GenieReaction reaction = GenieReaction::None;
    GenieDance dance = GenieDance::Disco;
    double reactionAge = 0;
    double vitality = 0.7;
    GenieVitalLevel condition = GenieVitalLevel::Happy;
    double stridePhase = 0;
    double walking = 0;
    double facing = 0;
    double air = 0;
    double land = 0;
    double hover = 0;
    QPointF gaze;
    bool hasGaze = false;
    double point = 0;
    QPointF pointAt;
    double lean = 0;
    double sniff = 0;
    double tug = 0;
    double size = 1;
    double spin = 0;
    double twirl = 0;
    double fade = 1;
    double incense = 0;

    static double duration(GenieReaction reaction);
};

struct GenieMagicBody {
    double lift = 0;
    double fade = 1;
    double size = 1;
    double spin = 0;
    double twirl = 0;
    double incense = 0;
    double grow = 1;
    double air = 0;
};

struct GenieMagic {
    GenieInteraction kind = GenieInteraction::None;
    double age = 0;
    double t = 0;
    QRectF target;
    QPointF foot;
    QPointF from;
    QPointF hand;
    double side = 78;
    double grow = 1;
    double lift = 0;
    quint32 seed = 0;
    GenieSpiderPose pose;

    static constexpr double kJump = 0.5;
    static double length(GenieInteraction kind);
    static GenieMagicBody body(GenieInteraction kind, double age);
    QRectF bounds() const;
};

namespace GenieSpiderPainter {

void paint(QPainter &painter, const QRectF &box, const GenieSpiderPose &pose);
void paintMagic(QPainter &painter, const GenieMagic &magic);

}
