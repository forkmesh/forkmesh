#include "GenieSpiderPainter.h"

#include <QColor>
#include <QConicalGradient>
#include <QImage>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <QRadialGradient>
#include <QRectF>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace {

const QColor kGlow(64, 242, 219);
const QColor kGlowSoft(107, 219, 240);
const QColor kGlowDeep(13, 140, 143);
const QColor kLilac(143, 102, 209);
const QColor kViolet(102, 61, 168);
const QColor kPlum(54, 33, 97);
const QColor kShell(26, 26, 51);
const QColor kShellLight(56, 56, 92);
const QColor kPitch(8, 8, 20);
const QColor kDeepSea(26, 77, 102);
const QColor kSilver(204, 209, 224);
const QColor kSilverDark(117, 122, 143);
const QColor kSickly(107, 158, 92);

const double kGround = 96.2;
const QPointF kShoulders[4] = {{35, 76}, {30, 71}, {28, 65}, {29, 59}};
const QPointF kKnees[4] = {{27, 79}, {20, 72}, {15, 61}, {13.5, 51}};
const QPointF kFeet[4] = {{31, kGround}, {17.5, kGround}, {8.5, kGround}, {2.5, kGround}};
const QRectF kCrown(12, 16, 76, 74);

struct CrownCache {
    QImage image;
    double scale = 0;
    bool sick = false;
    int glow = -1;
    int gem = -1;
};

CrownCache &crownCache()
{
    static CrownCache cache;
    return cache;
}

enum class Joint { Shoulder, Knee, Foot };

struct JointSprite {
    QImage image;
    double scale = 0;
    int key = -1;
};

JointSprite &jointSprite(Joint joint, bool back)
{
    static JointSprite sprites[6];
    return sprites[int(joint) * 2 + (back ? 1 : 0)];
}

JointSprite &socketSprite(int eye)
{
    static JointSprite sprites[6];
    return sprites[eye];
}

double clamp01(double v)
{
    return std::min(std::max(v, 0.0), 1.0);
}

QColor alpha(QColor color, double opacity)
{
    color.setAlphaF(float(clamp01(color.alphaF() * opacity)));
    return color;
}

QPointF mix(const QPointF &a, const QPointF &b, double k)
{
    return a + (b - a) * k;
}

QRectF oval(double x, double y, double rx, double ry)
{
    return QRectF(x - rx, y - ry, rx * 2, ry * 2);
}

QPainterPath ovalPath(double x, double y, double rx, double ry)
{
    QPainterPath path;
    path.addEllipse(oval(x, y, rx, ry));
    return path;
}

QPainterPath arc(const QPointF &c, double r, double fromDeg, double toDeg)
{
    QPainterPath path;
    const int steps = 24;
    for (int k = 0; k <= steps; ++k) {
        const double a = qDegreesToRadians(fromDeg + (toDeg - fromDeg) * k / steps);
        const QPointF p(c.x() + std::cos(a) * r, c.y() + std::sin(a) * r);
        if (k == 0)
            path.moveTo(p);
        else
            path.lineTo(p);
    }
    return path;
}

QPen pen(const QColor &color, double width)
{
    return QPen(color, width, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin);
}

void stroke(QPainter &p, const QPainterPath &path, const QColor &color, double width)
{
    p.strokePath(path, pen(color, width));
}

QRadialGradient radial(const QPointF &center, double radius, std::initializer_list<QColor> colors)
{
    QRadialGradient g(center, radius);
    const int n = int(colors.size());
    int i = 0;
    for (const QColor &c : colors)
        g.setColorAt(n > 1 ? double(i++) / (n - 1) : 0, c);
    return g;
}

QLinearGradient linear(const QPointF &from, const QPointF &to, std::initializer_list<QColor> colors)
{
    QLinearGradient g(from, to);
    const int n = int(colors.size());
    int i = 0;
    for (const QColor &c : colors)
        g.setColorAt(n > 1 ? double(i++) / (n - 1) : 0, c);
    return g;
}

void glow(QPainter &p, double x, double y, double r, const QColor &color)
{
    if (r <= 0)
        return;
    p.fillPath(ovalPath(x, y, r, r), radial(QPointF(x, y), r, {color, alpha(color, 0)}));
}

struct Leg {
    QPointF shoulder;
    QPointF knee;
    QPointF foot;
};

class Spider
{
public:
    Spider(const GenieSpiderPose &pose, double scale)
        : s(pose)
        , m_scale(scale)
        , m_bob(bob())
        , m_sway(sway())
    {
        const double a = qDegreesToRadians(m_sway);
        m_cos = std::cos(a);
        m_sin = std::sin(a);
    }

    void paint(QPainter &p)
    {
        p.save();
        if (std::abs(s.size - 1) > 1e-3 || std::abs(s.twirl) > 1e-3) {
            p.translate(50, 45);
            p.rotate(s.twirl);
            p.scale(std::max(s.size, 0.01), std::max(s.size, 0.01));
            p.translate(-50, -45);
        }
        aura(p);
        p.save();
        if (fright() > 0.02) {
            p.translate(50, 92);
            p.scale(1 + fright() * 0.06, 1 - fright() * 0.12);
            p.translate(-50, -92);
        }
        if (landing() > 0.01) {
            p.translate(50, 93);
            p.scale(1 + landing() * 0.1, 1 - landing() * 0.14);
            p.translate(-50, -93);
        }
        const double glide = danceGlide();
        if (std::abs(glide) > 1e-3)
            p.translate(glide, 0);
        shadow(p);
        double spin = danceSpin();
        if (std::abs(s.spin) > 1e-3) {
            const double turn = std::cos(s.spin);
            spin *= std::copysign(0.25 + std::abs(turn) * 0.75, turn);
        }
        if (std::abs(spin - 1) > 1e-3) {
            p.translate(50, 0);
            p.scale(spin, 1);
            p.translate(-50, 0);
        }
        p.translate(50, 90);
        p.rotate(m_sway);
        p.translate(-50, -90 + m_bob);
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        tail(p);
        legs(p, false);
        feather(p);
        crown(p);
        eyes(p);
        mouth(p);
        legs(p, true);
        burst(p);
        if (s.condition == GenieVitalLevel::Sick)
            sweat(p);
        p.restore();
        notes(p);
        puffs(p);
        p.restore();
    }

private:
    const GenieSpiderPose &s;
    double m_scale = 1;
    double m_bob = 0;
    double m_sway = 0;
    double m_cos = 1;
    double m_sin = 0;

    QPointF toBody(const QPointF &ground) const
    {
        const QPointF d = ground - QPointF(50, 90);
        return QPointF(d.x() * m_cos + d.y() * m_sin + 50, -d.x() * m_sin + d.y() * m_cos + 90 - m_bob);
    }

    bool is(GenieReaction r) const { return s.reaction == r; }

    double envelope() const
    {
        if (is(GenieReaction::None))
            return 0;
        return std::sin(M_PI * clamp01(s.reactionAge / GenieSpiderPose::duration(s.reaction)));
    }

    double rise() const
    {
        if (is(GenieReaction::None))
            return 0;
        const double up = clamp01(s.reactionAge / 0.22);
        const double down = clamp01((GenieSpiderPose::duration(s.reaction) - s.reactionAge) / 0.26);
        return up * down;
    }

    double walk() const { return clamp01(s.walking); }
    double air() const { return clamp01(s.air); }
    double landing() const { return clamp01(s.land); }
    double vitality() const { return clamp01(s.vitality); }
    double hover() const { return clamp01(s.hover) * (1 - air()); }
    double energy() const { return 0.45 + 0.55 * vitality(); }
    double fright() const { return 0; }
    double surprise() const { return std::max(is(GenieReaction::Surprise) ? envelope() : 0.0, air() * 0.7); }
    double dizzy() const { return is(GenieReaction::Dizzy) ? envelope() : 0; }
    double eureka() const { return is(GenieReaction::Eureka) ? rise() : 0; }
    double giggle() const { return is(GenieReaction::Giggle) ? rise() : 0; }
    double cheer() const { return is(GenieReaction::Cheer) ? rise() : 0; }
    double dancing() const { return is(GenieReaction::Dance) ? rise() : 0; }
    bool dancing(GenieDance dance) const { return s.dance == dance && dancing() > 0; }
    double beat() const { return s.reactionAge * M_PI * 4; }
    int robotStep() const { return int(s.reactionAge * 4); }

    double spinAngle() const
    {
        const double u = clamp01((s.reactionAge - 0.15) / (GenieSpiderPose::duration(GenieReaction::Dance) - 0.45));
        return u * u * (3 - 2 * u) * M_PI * 4;
    }

    double danceSpin() const
    {
        if (!dancing(GenieDance::Spin))
            return 1;
        const double turn = std::cos(spinAngle());
        return 1 + (std::copysign(0.3 + std::abs(turn) * 0.7, turn) - 1) * dancing();
    }

    double danceGlide() const
    {
        if (!dancing(GenieDance::Moonwalk))
            return 0;
        const double u = s.reactionAge / GenieSpiderPose::duration(GenieReaction::Dance);
        return -std::sin(u * M_PI * 2) * 6 * dancing();
    }

    double danceBob() const
    {
        const double d = dancing();
        if (d <= 0)
            return 0;
        const double w = beat();
        switch (s.dance) {
        case GenieDance::Disco:
            return std::abs(std::sin(w)) * 1.6 * d;
        case GenieDance::Twist:
            return (2.6 + std::sin(w * 2) * 0.9) * d;
        case GenieDance::Jig:
            return -std::abs(std::sin(w)) * 10 * d;
        case GenieDance::Ripple:
            return std::sin(w) * 1.2 * d;
        case GenieDance::Spin:
            return -2.2 * d;
        case GenieDance::JazzHands:
            return std::abs(std::sin(w)) * 2.2 * d;
        case GenieDance::Moonwalk:
            return std::abs(std::sin(w)) * 1.1 * d;
        case GenieDance::Headbang:
            return std::pow(std::abs(std::sin(w)), 3) * 3.6 * d;
        case GenieDance::Robot:
            return (robotStep() % 2 ? 1.6 : -0.6) * d;
        case GenieDance::Cancan:
            return std::abs(std::sin(w)) * 1.4 * d;
        }
        return 0;
    }

    double danceSway() const
    {
        const double d = dancing();
        if (d <= 0)
            return 0;
        const double w = beat();
        switch (s.dance) {
        case GenieDance::Disco:
            return std::tanh(std::sin(w / 2) * 3) * 7 * d;
        case GenieDance::Twist:
            return std::sin(w) * 11 * d;
        case GenieDance::Jig:
        case GenieDance::Ripple:
            return std::sin(w / 2) * 4 * d;
        case GenieDance::Spin:
            return 0;
        case GenieDance::JazzHands:
        case GenieDance::Headbang:
            return std::sin(w / 2) * 3 * d;
        case GenieDance::Moonwalk:
            return (-5 + std::sin(w) * 1.2) * d;
        case GenieDance::Robot: {
            const double tilt[4] = {0, 9, 0, -9};
            return tilt[robotStep() % 4] * d;
        }
        case GenieDance::Cancan:
            return -std::tanh(std::sin(w / 2) * 3) * 5 * d;
        }
        return 0;
    }

    QPointF danceLook() const
    {
        const double d = dancing();
        if (d <= 0)
            return QPointF();
        const double w = beat();
        switch (s.dance) {
        case GenieDance::Disco:
            return QPointF(std::tanh(std::sin(w / 2) * 3) * 1.3 * d, -0.6 * d);
        case GenieDance::Twist:
            return QPointF(std::sin(w) * d, 0);
        case GenieDance::Spin:
            return QPointF(-std::sin(spinAngle()) * 1.6 * d, 0);
        case GenieDance::Moonwalk:
            return QPointF(1.2 * d, 0);
        case GenieDance::Robot:
            return QPointF((robotStep() % 4 < 2 ? 1.4 : -1.4) * d, 0);
        default:
            return QPointF();
        }
    }

    double danceLid() const
    {
        const double d = dancing();
        switch (s.dance) {
        case GenieDance::Headbang:
            return 0.75 * d;
        case GenieDance::Twist:
        case GenieDance::Jig:
        case GenieDance::JazzHands:
        case GenieDance::Cancan:
            return 0.3 * d;
        default:
            return 0;
        }
    }

    double danceFeather() const
    {
        const double d = dancing();
        if (d <= 0)
            return 0;
        return std::sin(beat()) * (dancing(GenieDance::Headbang) ? 24 : 10) * d;
    }

    QPointF danceStep(int index, double side) const
    {
        const double d = dancing();
        if (d <= 0)
            return QPointF();
        const double w = beat();
        switch (s.dance) {
        case GenieDance::Twist:
            return QPointF(-std::sin(w) * 2 * d, 0);
        case GenieDance::Jig: {
            const double up = std::abs(std::sin(w));
            return QPointF(side * up * 3 * d, -up * 9 * d);
        }
        case GenieDance::Ripple: {
            const double lift = std::max(0.0, std::sin(w * 0.75 - index * 0.85 - (side > 0 ? M_PI : 0))) * 13 * d;
            return QPointF(side * lift * 0.35, -lift);
        }
        case GenieDance::Spin:
            return QPointF(-side * 3 * d, -1.5 * d);
        case GenieDance::Moonwalk: {
            const double group = ((index % 2 == 0) == (side < 0)) ? 0 : M_PI;
            return QPointF(-std::cos(w + group) * 4 * d, -std::max(0.0, std::sin(w + group)) * 4 * d);
        }
        case GenieDance::Robot:
            if ((index == 1 || index == 2) && (robotStep() % 2 == 0) == (side < 0))
                return QPointF(0, -3 * d);
            return QPointF();
        case GenieDance::Cancan: {
            if (index != 1 && index != 2)
                return QPointF();
            const double kick = std::pow(std::max(0.0, -side * std::sin(w / 2)), 1.5) * d;
            return index == 1 ? QPointF(side * kick * 7, -kick * 22) : QPointF(side * kick * 4, -kick * 11);
        }
        default:
            return QPointF();
        }
    }

    double danceArm(int index, double side, QPointF *knee, QPointF *foot) const
    {
        const double d = dancing();
        if (d <= 0)
            return 0;
        auto at = [side](double x, double y) { return QPointF(side < 0 ? x : 100 - x, y); };
        const double w = beat();
        switch (s.dance) {
        case GenieDance::Disco:
            if (index != 0)
                return 0;
            if (side > 0) {
                const double point = 0.5 + 0.5 * std::tanh(std::sin(w / 2) * 3);
                *knee = mix(at(26, 86), at(18, 52), point);
                *foot = mix(at(40, 92), at(6, 20), point);
            } else {
                *knee = at(22, 68);
                *foot = at(33, 84);
            }
            return d;
        case GenieDance::Twist:
            if (index != 0)
                return 0;
            *knee = at(20, 68) + QPointF(std::sin(w) * 3, 0);
            *foot = at(26, 82) + QPointF(std::sin(w) * 6, 0);
            return d;
        case GenieDance::Spin:
            if (index != 0)
                return 0;
            *knee = at(14, 50);
            *foot = at(34, 16);
            return d;
        case GenieDance::JazzHands: {
            const double wiggle = std::sin(w * 2 + (side < 0 ? 0 : 1.4)) * 4;
            if (index == 0) {
                *knee = at(21, 52);
                *foot = at(8 + wiggle, 28);
                return d;
            }
            if (index == 1) {
                *knee = at(14, 62);
                *foot = at(2, 52 + wiggle);
                return d;
            }
            return 0;
        }
        case GenieDance::Moonwalk:
            if (index != 0)
                return 0;
            if (side > 0) {
                *knee = at(22, 54);
                *foot = at(30, 34);
            } else {
                *knee = at(22, 72);
                *foot = at(16 + std::sin(w) * 3, 84);
            }
            return d;
        case GenieDance::Headbang:
            if (index != 0)
                return 0;
            if (side < 0) {
                *knee = at(22, 70);
                *foot = at(10, 64);
            } else {
                *knee = at(28, 82);
                *foot = at(40, 86 + std::sin(w * 2) * 3);
            }
            return d;
        case GenieDance::Robot: {
            if (index != 0)
                return 0;
            const bool up = (robotStep() % 2 == 0) == (side < 0);
            *knee = at(13, 76);
            *foot = at(13, up ? 54 : 94);
            return d;
        }
        case GenieDance::Cancan:
            if (index != 0)
                return 0;
            *knee = at(22, 70);
            *foot = at(34, 86);
            return d;
        default:
            return 0;
        }
    }

    double nod() const
    {
        if (!is(GenieReaction::Nod))
            return 0;
        return std::sin(s.reactionAge / GenieSpiderPose::duration(GenieReaction::Nod) * M_PI * 2) * 1.8;
    }

    double waving() const
    {
        if (s.mood == GenieMood::Greeting)
            return 1;
        if (is(GenieReaction::Wave))
            return rise();
        return eureka();
    }

    double bob() const
    {
        const double idle = std::sin(s.t * 1.6 * energy()) * 1.4;
        const double step = std::abs(std::sin(s.stridePhase)) * 2.2 * walk();
        const double hop = giggle() * std::abs(std::sin(s.reactionAge * 17)) * 2.2;
        const double perk = hover() * (2.4 + std::sin(s.t * 5.5) * 0.5);
        const double sniff = clamp01(s.sniff) * std::sin(s.t * 28) * 0.7;
        return idle - step - hop - perk + nod() + danceBob() + clamp01(s.tug) * 3.5 + sniff;
    }

    double sway() const
    {
        const double idle = std::sin(s.t * 0.8) * 1.2;
        const double wobble = dizzy() * std::sin(s.reactionAge * 9) * 9;
        const double heading = s.facing * 7 * walk();
        return idle + wobble + heading + danceSway() + std::clamp(s.lean, -1.0, 1.0) * 9;
    }

    QPointF look() const
    {
        double x = s.facing * 1.1 * walk();
        double y = 0;
        if (s.hasGaze) {
            x = x * 0.3 + std::clamp(s.gaze.x(), -1.0, 1.0) * 1.5;
            y = std::clamp(s.gaze.y(), -1.0, 1.0) * 1.3;
        }
        if (s.mood == GenieMood::Thinking) {
            x += std::sin(s.t * 1.3) * 0.8;
            y -= 1.1;
        } else if (s.mood == GenieMood::Idle && walk() < 0.1 && !s.hasGaze) {
            x += std::sin(s.t * 0.37) * std::sin(s.t * 0.91) * 0.9;
            y += std::sin(s.t * 0.53) * 0.4;
        }
        return QPointF(x, y) + danceLook();
    }

    double lid() const
    {
        const double cycle = std::fmod(s.t, 4.3);
        const double blink = cycle < 0.14 ? std::sin(cycle / 0.14 * M_PI) : 0;
        double tired = 0;
        switch (s.condition) {
        case GenieVitalLevel::Sick:
            tired = 0.45;
            break;
        case GenieVitalLevel::Hungry:
            tired = 0.28;
            break;
        case GenieVitalLevel::Peckish:
            tired = 0.12;
            break;
        default:
            break;
        }
        return std::min(std::max({blink, tired - surprise() * 0.5, giggle() * 0.6, danceLid()}), 0.95);
    }

    QColor tint() const { return s.condition == GenieVitalLevel::Sick ? kSickly : kGlow; }

    double glowStrength() const
    {
        const double base = 0.35 + 0.65 * vitality();
        const double pulse = s.mood == GenieMood::Thinking ? 0.2 * (0.5 + 0.5 * std::sin(s.t * 6)) : 0;
        return std::min(base + pulse + eureka() * 0.4 + hover() * 0.25, 1.0);
    }

    void aura(QPainter &p) const
    {
        const double pulse = 0.5 + 0.5 * std::sin(s.t * 2.1);
        const double radius = 38 + pulse * 3 + hover() * 3;
        QColor color = s.mood == GenieMood::Thinking ? kViolet : tint();
        const double incense = clamp01(s.incense);
        if (incense > 0.001)
            color = QColor::fromRgbF(color.redF() + (kLilac.redF() - color.redF()) * incense,
                                     color.greenF() + (kLilac.greenF() - color.greenF()) * incense,
                                     color.blueF() + (kLilac.blueF() - color.blueF()) * incense);
        glow(p, 50, 60, radius + incense * 6, alpha(color, (0.2 + hover() * 0.1 + incense * 0.22) * glowStrength()));
    }

    void shadow(QPainter &p) const
    {
        const double grounded = 1 - air();
        if (grounded <= 0.02)
            return;
        const double width = 36 - hover() * 3;
        QRadialGradient fill(QPointF(50, kGround + 0.6), width);
        fill.setColorAt(0, alpha(kPitch, 0.42 * grounded));
        fill.setColorAt(0.55, alpha(kPitch, 0.18 * grounded));
        fill.setColorAt(1, alpha(kPitch, 0));
        p.save();
        p.translate(50, kGround + 0.6);
        p.scale(1, 0.11);
        p.translate(-50, -(kGround + 0.6));
        p.fillPath(ovalPath(50, kGround + 0.6, width, width), fill);
        p.restore();
    }

    void tail(QPainter &p) const
    {
        const double swish = std::sin(s.t * 1.9) * 3 + s.facing * -4 * walk();
        QPainterPath path;
        path.moveTo(45, 79);
        path.cubicTo(39, 84, 49 + swish, 87, 42 + swish, 92);
        path.cubicTo(35 + swish, 97.5, 47 + swish, 101, 53 + swish * 1.3, 98.5);
        path.cubicTo(49.5 + swish, 97, 44.5 + swish, 96.2, 45.8 + swish, 93);
        path.cubicTo(58.5 + swish, 87, 55, 82, 56, 79);
        path.closeSubpath();
        p.fillPath(path, linear(QPointF(42, 80), QPointF(52, 99), {kShellLight, kShell, kPitch}));
        stroke(p, path, alpha(tint(), 0.32 * glowStrength()), 0.45);
    }

    void legs(QPainter &p, bool front) const
    {
        const int order[2] = {front ? 1 : 3, front ? 0 : 2};
        for (int index : order)
            for (double side : {-1.0, 1.0})
                drawLeg(p, pose(index, side), !front);
    }

    Leg pose(int index, double side) const
    {
        auto m = [side](const QPointF &q) { return side < 0 ? q : QPointF(100 - q.x(), q.y()); };
        const QPointF shoulder = m(kShoulders[index]);
        QPointF knee = m(kKnees[index]);
        QPointF foot = m(kFeet[index]);
        const double group = ((index % 2 == 0) == (side < 0)) ? 0 : M_PI;
        const double swing = s.stridePhase + group;
        const double lift = std::max(0.0, std::sin(swing)) * 6 * walk();
        const double reach = std::cos(swing) * 3 * walk() * (s.facing == 0 ? 1 : s.facing);
        const double breathe = std::sin(s.t * 1.6 * energy() + index) * 0.5;
        const double tap = index == 0 ? std::max(0.0, std::sin(s.t * 9 + (side < 0 ? 0 : M_PI))) * 3.2 * hover() : 0;
        const QPointF step = danceStep(index, side);
        foot.ry() -= lift + tap;
        foot.rx() += reach;
        foot = toBody(foot + step);
        knee.ry() -= lift * 0.55 + tap * 0.5 + breathe;
        knee.rx() += reach * 0.5;
        knee += QPointF(step.x() * 0.5, step.y() * 0.6);
        knee.ry() += (1 - vitality()) * 3;
        if (air() > 0.01) {
            const double dx = shoulder.x() - 50;
            const double dy = shoulder.y() - 62;
            const double length = std::max(std::hypot(dx, dy), 0.01);
            const double flail = std::sin(s.stridePhase + index * 1.3 + (side < 0 ? 0 : 0.7));
            const QPointF outKnee(shoulder.x() + dx / length * 13, shoulder.y() + dy / length * 13 - 4 + flail * 2);
            const QPointF outFoot(shoulder.x() + dx / length * 27, shoulder.y() + dy / length * 27 - 3 + flail * 6);
            knee = mix(knee, outKnee, air());
            foot = mix(foot, outFoot, air());
        }
        if (index == 0) {
            const double raised = side < 0 ? std::max(waving(), cheer()) : cheer();
            if (raised > 0.01) {
                const double wiggle = std::sin(s.reactionAge * 13 + (side < 0 ? 0 : 1.4)) * 6 * raised;
                foot = mix(foot, m(QPointF(10 + wiggle * (side < 0 ? 1 : -1), 38)), raised);
                knee = mix(knee, m(QPointF(21, 56)), raised);
            }
        }
        const double point = clamp01(s.point);
        if (index == 0 && point > 0.01 && (s.pointAt.x() >= 0) == (side > 0)) {
            const double length = std::max(std::hypot(s.pointAt.x(), s.pointAt.y()), 0.01);
            const QPointF screen(s.pointAt.x() / length, s.pointAt.y() / length);
            const QPointF dir(screen.x() * m_cos + screen.y() * m_sin, -screen.x() * m_sin + screen.y() * m_cos);
            const QPointF reach = shoulder + dir * 38;
            const QPointF tip(std::clamp(reach.x(), 3.0, 97.0), std::clamp(reach.y(), 3.0, 97.0));
            knee = mix(knee, shoulder + dir * 17 + QPointF(0, -2), point);
            foot = mix(foot, tip, point);
        }
        QPointF armKnee;
        QPointF armFoot;
        const double arm = danceArm(index, side, &armKnee, &armFoot);
        if (arm > 0.01) {
            knee = mix(knee, armKnee, arm);
            foot = mix(foot, armFoot, arm);
        }
        return Leg{shoulder, knee, foot};
    }

    void drawLeg(QPainter &p, const Leg &leg, bool back) const
    {
        const double shade = back ? 0.85 : 1;
        const double g = glowStrength();
        const QColor t = tint();
        const double outward = leg.foot.x() < 50 ? -1 : 1;
        joint(p, Joint::Shoulder, back, leg.shoulder);
        QPainterPath upper;
        upper.moveTo(leg.shoulder);
        upper.lineTo(leg.knee);
        stroke(p, upper, alpha(kPitch, shade), 6.2);
        stroke(p, upper, alpha(kShell, shade), 4.6);
        QPainterPath sheen;
        sheen.moveTo(leg.shoulder - QPointF(0, 1.1));
        sheen.lineTo(leg.knee - QPointF(0, 1.1));
        stroke(p, sheen, alpha(kShellLight, 0.75 * shade), 1.1);
        const double dx = leg.foot.x() - leg.knee.x();
        const double dy = leg.foot.y() - leg.knee.y();
        const double length = std::max(std::hypot(dx, dy), 0.01);
        double nx = -dy / length;
        double ny = dx / length;
        if (nx * outward < 0 || (std::abs(nx) < 0.05 && ny > 0)) {
            nx = -nx;
            ny = -ny;
        }
        const QPointF n(nx, ny);
        const QPointF mid(leg.knee.x() + dx * 0.42, leg.knee.y() + dy * 0.42);
        const double bulge = std::min(length * 0.24, 5.5);
        QPainterPath claw;
        claw.moveTo(leg.knee + n * 3.7);
        claw.quadTo(mid + n * (bulge + 3), leg.foot);
        claw.quadTo(mid + n * (bulge * 0.5 - 0.6), leg.knee - n * 3.1);
        claw.closeSubpath();
        QLinearGradient fill(leg.knee, leg.foot);
        fill.setColorAt(0, alpha(kShellLight, shade));
        fill.setColorAt(0.35, alpha(kShell, shade));
        fill.setColorAt(0.72, alpha(kPitch, shade));
        fill.setColorAt(1, alpha(t, g * shade));
        p.fillPath(claw, fill);
        QPainterPath rim;
        rim.moveTo(leg.knee + n * 2.4);
        rim.quadTo(mid + n * (bulge + 1.7), mix(leg.knee, leg.foot, 0.82));
        stroke(p, rim, alpha(kSilver, 0.28 * shade), 0.7);
        QPainterPath vein;
        vein.moveTo(mix(leg.knee, leg.foot, 0.12));
        vein.quadTo(mid + n * bulge * 0.8, mix(leg.knee, leg.foot, 0.62));
        stroke(p, vein, alpha(t, 0.7 * g * shade), 0.75);
        joint(p, Joint::Foot, back, leg.foot);
        joint(p, Joint::Knee, back, leg.knee);
    }

    void joint(QPainter &p, Joint kind, bool back, const QPointF &at) const
    {
        const double reach = kind == Joint::Knee ? 6.2 : kind == Joint::Foot ? 3.7 : 3.2;
        JointSprite &sprite = jointSprite(kind, back);
        const int key = int(std::lround(glowStrength() * 48)) * 2 + (s.condition == GenieVitalLevel::Sick ? 1 : 0);
        const double scale = std::max(m_scale, 0.1) * 2;
        if (sprite.image.isNull() || std::abs(sprite.scale - scale) > 1e-3 || sprite.key != key) {
            const int edge = int(std::ceil(reach * 2 * scale));
            sprite.image = QImage(edge, edge, QImage::Format_ARGB32_Premultiplied);
            sprite.image.fill(Qt::transparent);
            sprite.scale = scale;
            sprite.key = key;
            QPainter c(&sprite.image);
            c.setRenderHint(QPainter::Antialiasing, true);
            c.scale(edge / (reach * 2), edge / (reach * 2));
            c.translate(reach, reach);
            const double shade = back ? 0.85 : 1;
            const double g = glowStrength();
            const QColor t = tint();
            const QPointF o(0, 0);
            if (kind == Joint::Shoulder) {
                c.fillPath(ovalPath(0, 0, 3, 3),
                           radial(o - QPointF(0.8, 0.9), 3.4, {alpha(kShellLight, shade), alpha(kPitch, shade)}));
            } else if (kind == Joint::Foot) {
                glow(c, 0, 0, 3.6, alpha(t, 0.6 * g * shade));
            } else {
                glow(c, 0, 0, 6, alpha(t, 0.42 * g * shade));
                c.fillPath(ovalPath(0, 0, 3.9, 3.9),
                           radial(o - QPointF(1, 1.2), 4.4, {alpha(kShellLight, shade), alpha(kPitch, shade)}));
                c.fillPath(ovalPath(0, 0, 2.2, 2.2),
                           radial(o - QPointF(0.5, 0.5), 2.4,
                                  {alpha(Qt::white, 0.9 * shade), alpha(t, g * shade), alpha(kGlowDeep, shade)}));
                stroke(c, ovalPath(0, 0, 3.9, 3.9), alpha(kSilverDark, 0.5 * shade), 0.35);
            }
        }
        p.drawImage(QRectF(at.x() - reach, at.y() - reach, reach * 2, reach * 2), sprite.image);
    }

    void head(QPainter &p) const
    {
        const QPainterPath face = ovalPath(50, 61, 25.5, 25);
        p.fillPath(face, radial(QPointF(41, 54), 34, {kShellLight, kShell, kPitch}));
        if (s.condition == GenieVitalLevel::Sick)
            p.fillPath(face, alpha(kSickly, 0.16));
        stroke(p, arc(QPointF(50, 61), 24.6, 15, 80), alpha(tint(), 0.4 * glowStrength()), 0.8);
        p.save();
        p.setClipPath(face, Qt::IntersectClip);
        p.fillPath(ovalPath(37, 77, 7, 4), alpha(Qt::white, 0.05));
        p.restore();
        p.fillPath(ovalPath(38, 73.5, 4, 2.2), alpha(kViolet, 0.24));
        p.fillPath(ovalPath(62, 73.5, 4, 2.2), alpha(kViolet, 0.24));
    }

    void feather(QPainter &p) const
    {
        const double hunger = 1 - vitality();
        p.save();
        p.translate(50, 27);
        p.rotate(std::sin(s.t * 1.1) * 4 + hunger * 38 + danceFeather());
        p.translate(-50, -27);
        const QPointF base(50, 28);
        const QPointF bend(44, 13);
        const QPointF tip(55, 0.5);
        auto spine = [&](double u) {
            return base * ((1 - u) * (1 - u)) + bend * (2 * (1 - u) * u) + tip * (u * u);
        };
        auto normal = [&](double u) {
            const QPointF d = (bend - base) * (2 * (1 - u)) + (tip - bend) * (2 * u);
            const double length = std::max(std::hypot(d.x(), d.y()), 0.01);
            return QPointF(-d.y() / length, d.x() / length);
        };
        const int steps = 14;
        QList<QPointF> left;
        QList<QPointF> right;
        for (int k = 0; k <= steps; ++k) {
            const double u = double(k) / steps;
            const double notch = k % 2 == 1 ? 0.74 : 1;
            const double width = 7.4 * std::pow(std::sin(M_PI * std::min(u * 1.05, 1.0)), 0.75) * (1 - u * 0.3) * notch;
            const QPointF at = spine(u);
            const QPointF n = normal(u);
            left << at + n * width;
            right << at - n * (width * 0.92);
        }
        QPainterPath leaf;
        leaf.moveTo(base);
        for (const QPointF &q : left)
            leaf.lineTo(q);
        for (auto it = right.crbegin(); it != right.crend(); ++it)
            leaf.lineTo(*it);
        leaf.closeSubpath();
        p.fillPath(leaf, linear(QPointF(42, 8), QPointF(58, 26), {kShellLight, kShell, kPitch}));
        stroke(p, leaf, alpha(kPitch, 0.6), 0.35);
        for (int k = 2; k < steps - 1; k += 2) {
            QPainterPath barb;
            barb.moveTo(spine(double(k) / steps - 0.03));
            barb.lineTo(left.at(k + 1));
            barb.moveTo(spine(double(k) / steps - 0.03));
            barb.lineTo(right.at(k + 1));
            stroke(p, barb, alpha(kPitch, 0.55), 0.4);
        }
        QPainterPath vein;
        vein.moveTo(base);
        vein.quadTo(bend, spine(0.94));
        stroke(p, vein, alpha(tint(), 0.3 * glowStrength()), 2.4);
        stroke(p, vein, alpha(tint(), 0.95 * glowStrength()), 0.8);
        p.restore();
    }

    void silver(QPainter &p, const QPainterPath &path, double width) const
    {
        stroke(p, path, alpha(kPitch, 0.55), width + 1);
        stroke(p, path, kSilverDark, width);
        stroke(p, path, kSilver, width * 0.62);
        p.save();
        p.translate(0, -width * 0.18);
        stroke(p, path, alpha(Qt::white, 0.7), width * 0.18);
        p.restore();
    }

    void turban(QPainter &p) const
    {
        QPainterPath dome;
        dome.moveTo(21.5, 60);
        dome.cubicTo(15, 39, 29, 23, 50, 23);
        dome.cubicTo(71, 23, 85, 39, 78.5, 60);
        dome.cubicTo(75.5, 51, 64, 44.5, 50, 44.5);
        dome.cubicTo(36, 44.5, 24.5, 51, 21.5, 60);
        dome.closeSubpath();
        p.fillPath(dome, linear(QPointF(36, 22), QPointF(58, 64), {kLilac, kViolet, kPlum, kShell}));
        const QColor t = tint();
        const double g = glowStrength();
        p.save();
        p.setClipPath(dome, Qt::IntersectClip);
        for (int k = 0; k < 4; ++k) {
            const double y = 58 - k * 8.5;
            const double peak = 31 - k * 6;
            QPainterPath fold;
            fold.moveTo(18, y);
            fold.cubicTo(26, peak, 70, peak - 4, 82, y - 3);
            stroke(p, fold, alpha(kPitch, 0.42), 2.2);
            stroke(p, fold.translated(0, 1.5), alpha(kLilac, 0.4), 0.9);
        }
        p.fillPath(ovalPath(37, 31, 10, 6), alpha(Qt::white, 0.1));
        QPainterPath trim;
        trim.moveTo(23, 55);
        trim.quadTo(20.5, 40, 32, 32);
        trim.moveTo(77, 55);
        trim.quadTo(79.5, 40, 68, 32);
        stroke(p, trim, alpha(t, 0.32 * g), 2);
        stroke(p, trim, alpha(t, 0.85 * g), 0.6);
        p.restore();
        QPainterPath archPath;
        archPath.moveTo(22.4, 59.6);
        archPath.cubicTo(25.4, 51, 36, 44.5, 50, 44.5);
        archPath.cubicTo(64, 44.5, 74.6, 51, 77.6, 59.6);
        silver(p, archPath, 3.4);
        QPainterPath chevron;
        chevron.moveTo(33.5, 28.5);
        chevron.quadTo(43, 31, 50, 40);
        chevron.quadTo(57, 31, 66.5, 28.5);
        silver(p, chevron, 2.6);
        const QPointF gem(50, 40.5);
        glow(p, gem.x(), gem.y(), 8 + eureka() * 5, alpha(t, 0.5 * g));
        p.fillPath(ovalPath(gem.x(), gem.y(), 5.4, 5.4),
                   radial(gem - QPointF(1.8, 2), 7, {Qt::white, kSilver, kSilverDark}));
        p.fillPath(ovalPath(gem.x(), gem.y(), 3.9, 3.9), kPitch);
        p.fillPath(ovalPath(gem.x(), gem.y(), 3, 3), radial(gem - QPointF(0.8, 0.8), 3.4, {Qt::white, t, kGlowDeep}));
        p.fillPath(ovalPath(gem.x() - 1, gem.y() - 1.1, 0.8, 0.7), alpha(Qt::white, 0.95));
        QPainterPath diamond;
        diamond.moveTo(50, 46.3);
        diamond.lineTo(52.6, 49.8);
        diamond.lineTo(50, 54);
        diamond.lineTo(47.4, 49.8);
        diamond.closeSubpath();
        p.fillPath(diamond, linear(QPointF(48, 46), QPointF(52, 54), {Qt::white, kSilver, kSilverDark}));
        stroke(p, diamond, alpha(kPitch, 0.5), 0.3);
        QPainterPath inner;
        inner.moveTo(50, 48.2);
        inner.lineTo(51.2, 49.9);
        inner.lineTo(50, 51.8);
        inner.lineTo(48.8, 49.9);
        inner.closeSubpath();
        p.fillPath(inner, alpha(t, g));
    }

    void crown(QPainter &p) const
    {
        CrownCache &cache = crownCache();
        const bool sick = s.condition == GenieVitalLevel::Sick;
        const int glowKey = int(std::lround(glowStrength() * 48));
        const int gemKey = int(std::lround(eureka() * 24));
        const double scale = std::max(m_scale, 0.1) * 2;
        if (cache.image.isNull() || std::abs(cache.scale - scale) > 1e-3 || cache.sick != sick ||
            cache.glow != glowKey || cache.gem != gemKey) {
            const QSize size(int(std::ceil(kCrown.width() * scale)), int(std::ceil(kCrown.height() * scale)));
            cache.image = QImage(size, QImage::Format_ARGB32_Premultiplied);
            cache.image.fill(Qt::transparent);
            cache.scale = scale;
            cache.sick = sick;
            cache.glow = glowKey;
            cache.gem = gemKey;
            QPainter c(&cache.image);
            c.setRenderHint(QPainter::Antialiasing, true);
            c.scale(size.width() / kCrown.width(), size.height() / kCrown.height());
            c.translate(-kCrown.topLeft());
            head(c);
            turban(c);
        }
        p.save();
        p.setRenderHint(QPainter::SmoothPixmapTransform, true);
        p.drawImage(kCrown, cache.image);
        p.restore();
        if (s.condition == GenieVitalLevel::Thriving) {
            const double cycle = std::fmod(s.t, 3.4);
            if (cycle < 0.6)
                glint(p, QPointF(52.6, 38), 2.8 * std::sin(cycle / 0.6 * M_PI));
        }
    }

    void glint(QPainter &p, const QPointF &at, double r) const
    {
        if (r <= 0.05)
            return;
        QPainterPath cross;
        cross.moveTo(at.x() - r, at.y());
        cross.lineTo(at.x() + r, at.y());
        cross.moveTo(at.x(), at.y() - r);
        cross.lineTo(at.x(), at.y() + r);
        stroke(p, cross, alpha(Qt::white, 0.9), 0.45);
    }

    void eyes(QPainter &p) const
    {
        const double wide = 1 + surprise() * 0.2 + hover() * 0.07;
        const double spots[6][3] = {{43.6, 66.8, 6},   {56.4, 66.8, 6},   {33.4, 60.4, 4.4},
                                    {66.6, 60.4, 4.4}, {44.2, 55.6, 3.1}, {55.8, 55.6, 3.1}};
        for (int i = 0; i < 6; ++i)
            eye(p, i, QPointF(spots[i][0], spots[i][1]), spots[i][2] * wide, dizzy() > 0.2 && i < 2);
    }

    void socket(QPainter &p, int index, const QPointF &c, double r) const
    {
        const double reach = r * 1.56;
        JointSprite &sprite = socketSprite(index);
        const int key = (int(std::lround(r * 40)) * 64 + int(std::lround(glowStrength() * 48))) * 2 +
                        (s.condition == GenieVitalLevel::Sick ? 1 : 0);
        const double scale = std::max(m_scale, 0.1) * 2;
        if (sprite.image.isNull() || std::abs(sprite.scale - scale) > 1e-3 || sprite.key != key) {
            const int edge = int(std::ceil(reach * 2 * scale));
            sprite.image = QImage(edge, edge, QImage::Format_ARGB32_Premultiplied);
            sprite.image.fill(Qt::transparent);
            sprite.scale = scale;
            sprite.key = key;
            QPainter e(&sprite.image);
            e.setRenderHint(QPainter::Antialiasing, true);
            e.scale(edge / (reach * 2), edge / (reach * 2));
            e.translate(reach, reach);
            const QColor t = tint();
            const double g = glowStrength();
            glow(e, 0, 0, r * 1.55, alpha(t, 0.3 * g));
            e.fillPath(ovalPath(0, 0, r * 1.13, r * 1.13), kPitch);
            stroke(e, ovalPath(0, 0, r, r), alpha(t, 0.85 * g + 0.15), std::max(r * 0.11, 0.35));
        }
        p.drawImage(QRectF(c.x() - reach, c.y() - reach, reach * 2, reach * 2), sprite.image);
    }

    void eye(QPainter &p, int index, const QPointF &c, double r, bool spiral) const
    {
        const QColor t = tint();
        const double g = glowStrength();
        socket(p, index, c, r);
        if (spiral) {
            p.save();
            p.setClipPath(ovalPath(c.x(), c.y(), r * 0.93, r * 0.93), Qt::IntersectClip);
            p.fillPath(ovalPath(c.x(), c.y(), r, r), kPitch);
            QPainterPath swirl;
            const int turns = 18;
            for (int k = 0; k <= turns; ++k) {
                const double u = double(k) / turns;
                const double a = u * M_PI * 4 + s.reactionAge * 8;
                const QPointF q(c.x() + std::cos(a) * r * 0.8 * u, c.y() + std::sin(a) * r * 0.8 * u);
                if (k == 0)
                    swirl.moveTo(q);
                else
                    swirl.lineTo(q);
            }
            p.strokePath(swirl, QPen(t, r * 0.14));
            p.restore();
        } else {
            QPointF l = look() * 0.16;
            const double reach = std::hypot(l.x(), l.y());
            if (reach > 0.2)
                l *= 0.2 / reach;
            const QPointF shift = c + l * r;
            QRadialGradient iris(QPointF(shift.x(), shift.y() + r * 0.45), r * 1.05);
            iris.setColorAt(0, kDeepSea);
            iris.setColorAt(1, kPitch);
            p.fillPath(ovalPath(c.x(), c.y(), r * 0.93, r * 0.93), iris);
            p.fillPath(ovalPath(shift.x(), shift.y() + r * 0.05, r * 0.58, r * 0.58), alpha(kPitch, 0.9));
            const double ring = std::min(0.72, 0.86 - std::min(reach, 0.2)) * r;
            stroke(p, arc(shift, ring, 25, 155), alpha(t, 0.55 * g + 0.1), r * 0.13);
            p.fillPath(ovalPath(shift.x() - r * 0.3, shift.y() - r * 0.33, r * 0.27, r * 0.23), alpha(Qt::white, 0.96));
            p.fillPath(ovalPath(shift.x() + r * 0.32, shift.y() + r * 0.22, r * 0.11, r * 0.11), alpha(Qt::white, 0.75));
        }
        const double closed = lid();
        if (closed > 0.02) {
            const double edge = r * 1.02;
            const double y = c.y() - r + r * 2 * closed;
            const double fall = std::clamp((c.y() - y) / edge, -1.0, 1.0);
            const double sweep = qRadiansToDegrees(std::acos(fall));
            QPainterPath cover = arc(c, edge, -90 - sweep, -90 + sweep);
            cover.closeSubpath();
            p.fillPath(cover, kShell);
            const double half = std::sqrt(std::max(0.0, edge * edge - (y - c.y()) * (y - c.y())));
            QPainterPath line;
            line.moveTo(c.x() - std::min(half, r), y);
            line.lineTo(c.x() + std::min(half, r), y);
            p.strokePath(line, QPen(alpha(t, 0.4 * g), std::max(r * 0.1, 0.3)));
        }
    }

    void mouth(QPainter &p) const
    {
        const QPointF c(50, 76.4);
        const QColor t = tint();
        if (s.mood == GenieMood::Speaking || s.mood == GenieMood::Greeting) {
            const double open = (0.5 + 0.5 * std::sin(s.t * 17)) * (0.65 + 0.35 * std::sin(s.t * 6.1));
            const QPainterPath hole = ovalPath(c.x(), c.y() + 0.3, 2.6, 0.5 + 1.9 * open);
            p.fillPath(hole, kPitch);
            p.strokePath(hole, QPen(alpha(t, 0.8), 0.45));
            return;
        }
        if (surprise() > 0.05) {
            p.strokePath(ovalPath(c.x(), c.y(), 1.2 + surprise(), 1.2 + surprise() * 1.3), QPen(alpha(t, 0.9), 0.55));
            return;
        }
        double smile = 1.6;
        switch (s.condition) {
        case GenieVitalLevel::Sick:
            smile = -1.2;
            break;
        case GenieVitalLevel::Hungry:
            smile = -0.4;
            break;
        case GenieVitalLevel::Peckish:
            smile = 0.6;
            break;
        default:
            break;
        }
        const double grin = smile + giggle() * 1.2 + cheer() + dancing() * 1.4 + hover() * 1.1 +
                            (s.mood == GenieMood::Thinking ? -1.2 : 0);
        QPainterPath line;
        line.moveTo(c.x() - 2.6, c.y());
        line.quadTo(c.x(), c.y() + grin, c.x() + 2.6, c.y());
        stroke(p, line, alpha(t, 0.95), 0.6);
    }

    void burst(QPainter &p) const
    {
        if (eureka() <= 0.05)
            return;
        const double progress = clamp01(s.reactionAge / GenieSpiderPose::duration(GenieReaction::Eureka));
        for (int i = 0; i < 8; ++i) {
            const double a = double(i) / 8 * M_PI * 2;
            const double inner = 7 + progress * 4;
            const double outer = inner + 4 * eureka();
            QPainterPath ray;
            ray.moveTo(50 + std::cos(a) * inner, 41 + std::sin(a) * inner);
            ray.lineTo(50 + std::cos(a) * outer, 41 + std::sin(a) * outer);
            stroke(p, ray, alpha(tint(), eureka() * (1 - progress * 0.7)), 0.7);
        }
    }

    void notes(QPainter &p) const
    {
        const double d = dancing();
        if (d <= 0.05)
            return;
        for (int k = 0; k < 3; ++k) {
            const double u = std::fmod(s.reactionAge * 0.7 + k / 3.0, 1.0);
            const QPointF at((k % 2 ? 80 : 20) + std::sin(u * 6 + k) * 3, 52 - u * 34);
            note(p, at, k == 1, alpha(tint(), std::sin(u * M_PI) * d));
        }
    }

    void note(QPainter &p, const QPointF &at, bool pair, const QColor &color) const
    {
        glow(p, at.x() + 3, at.y() - 4, 7, alpha(color, 0.3));
        QPainterPath stems;
        stems.moveTo(at + QPointF(2, -0.4));
        stems.lineTo(at + QPointF(2, -9));
        if (pair) {
            stems.moveTo(at + QPointF(8, -1.9));
            stems.lineTo(at + QPointF(8, -10.5));
        } else {
            stems.quadTo(at + QPointF(2.6, -6), at + QPointF(5.4, -5));
        }
        stroke(p, stems, color, 0.7);
        if (pair) {
            QPainterPath beam;
            beam.moveTo(at + QPointF(2, -9));
            beam.lineTo(at + QPointF(8, -10.5));
            stroke(p, beam, color, 1.5);
        }
        for (int i = 0; i < (pair ? 2 : 1); ++i) {
            p.save();
            p.translate(at + QPointF(i * 6, -i * 1.5));
            p.rotate(-20);
            p.fillPath(ovalPath(0, 0, 2.3, 1.6), color);
            p.restore();
        }
    }

    void puffs(QPainter &p) const
    {
        const double sniff = clamp01(s.sniff);
        if (sniff <= 0.05)
            return;
        const double side = s.lean < 0 ? -1 : 1;
        for (int k = 0; k < 3; ++k) {
            const double u = std::fmod(s.t * 1.8 + k / 3.0, 1.0);
            const QPointF at(50 + side * (12 + u * 22), 76 - u * 10 + std::sin(u * 9 + k) * 1.5);
            p.fillPath(ovalPath(at.x(), at.y(), 1.2 + u * 1.6, 1.2 + u * 1.6),
                       alpha(kGlowSoft, sniff * std::sin(u * M_PI) * 0.75));
        }
    }

    void sweat(QPainter &p) const
    {
        const double cycle = std::fmod(s.t, 2.6) / 2.6;
        const double y = 57 + cycle * 8;
        QPainterPath drop;
        drop.moveTo(75, y - 2.4);
        drop.quadTo(77.6, y + 1, 75, y + 1.4);
        drop.quadTo(72.4, y + 1, 75, y - 2.4);
        p.fillPath(drop, alpha(kGlowSoft, (1 - cycle) * 0.85));
    }
};

}

double GenieSpiderPose::duration(GenieReaction reaction)
{
    switch (reaction) {
    case GenieReaction::Wave:
        return 1.8;
    case GenieReaction::Cheer:
        return 1.6;
    case GenieReaction::Surprise:
        return 1.3;
    case GenieReaction::Nod:
        return 0.9;
    case GenieReaction::Dizzy:
        return 1.7;
    case GenieReaction::Eureka:
        return 1.4;
    case GenieReaction::Giggle:
        return 1.1;
    case GenieReaction::Dance:
        return 3.6;
    case GenieReaction::None:
        break;
    }
    return 0;
}

namespace GenieSpiderPainter {

void paint(QPainter &painter, const QRectF &box, const GenieSpiderPose &pose)
{
    const double side = std::min(box.width(), box.height());
    if (side <= 0)
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.translate(box.x() + (box.width() - side) / 2, box.y() + (box.height() - side) / 2);
    painter.scale(side / 100, side / 100);
    const QTransform device = painter.deviceTransform();
    const double scale = std::sqrt(std::abs(device.m11() * device.m22() - device.m12() * device.m21()));
    const double fade = clamp01(pose.fade);
    if (fade < 0.004) {
        painter.restore();
        return;
    }
    if (fade > 0.996) {
        Spider(pose, scale).paint(painter);
        painter.restore();
        return;
    }
    const int pixels = std::max(1, int(std::ceil(120 * scale)));
    QImage layer(pixels, pixels, QImage::Format_ARGB32_Premultiplied);
    layer.fill(Qt::transparent);
    {
        QPainter q(&layer);
        q.setRenderHint(QPainter::Antialiasing, true);
        q.scale(pixels / 120.0, pixels / 120.0);
        q.translate(10, 10);
        Spider(pose, scale).paint(q);
    }
    painter.setOpacity(painter.opacity() * fade);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
    painter.drawImage(QRectF(-10, -10, 120, 120), layer);
    painter.restore();
}

}

namespace {

const QColor kGold(255, 206, 84);
const QColor kGoldDeep(196, 134, 36);
const QColor kGoldPale(255, 240, 190);
const QColor kSmoke(178, 152, 224);

double rnd(quint32 seed, int i, int salt = 0)
{
    quint32 x = seed * 747796405u + quint32(i) * 2891336453u + quint32(salt) * 2246822519u + 1u;
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return (x & 0xffffffu) / double(0x1000000);
}

double ease(double u)
{
    u = clamp01(u);
    return u * u * (3 - 2 * u);
}

double easeOut(double u)
{
    u = clamp01(u);
    return 1 - (1 - u) * (1 - u);
}

double span(double a, double from, double to)
{
    return clamp01((a - from) / (to - from));
}

double window(double a, double in0, double in1, double out0, double out1)
{
    return span(a, in0, in1) * (1 - span(a, out0, out1));
}

QPointF bezier(const QPointF &a, const QPointF &c, const QPointF &b, double u)
{
    return a * ((1 - u) * (1 - u)) + c * (2 * u * (1 - u)) + b * (u * u);
}

QPointF unitOf(const QPointF &v)
{
    const double length = std::max(std::hypot(v.x(), v.y()), 0.001);
    return v / length;
}

QPointF rainbowPeak(const QPointF &from, const QPointF &to)
{
    const double distance = std::hypot(to.x() - from.x(), to.y() - from.y());
    return QPointF((from.x() + to.x()) / 2, std::min(from.y(), to.y()) - 30 - 0.3 * distance);
}

void sparkle(QPainter &p, const QPointF &at, double r, const QColor &color)
{
    if (r <= 0.05 || color.alphaF() <= 0.01)
        return;
    glow(p, at.x(), at.y(), r * 2.2, alpha(color, 0.45));
    QPainterPath star;
    const double w = r * 0.28;
    star.moveTo(at.x(), at.y() - r);
    star.quadTo(at.x() + w, at.y() - w, at.x() + r, at.y());
    star.quadTo(at.x() + w, at.y() + w, at.x(), at.y() + r);
    star.quadTo(at.x() - w, at.y() + w, at.x() - r, at.y());
    star.quadTo(at.x() - w, at.y() - w, at.x(), at.y() - r);
    p.fillPath(star, color);
}

void dot(QPainter &p, const QPointF &at, double r, const QColor &color)
{
    if (r <= 0.05 || color.alphaF() <= 0.01)
        return;
    glow(p, at.x(), at.y(), r * 3.2, alpha(color, 0.4));
    p.fillPath(ovalPath(at.x(), at.y(), r, r), radial(at, r, {alpha(Qt::white, color.alphaF()), color}));
}

void blob(QPainter &p, const QPointF &at, double r, double opacity)
{
    if (r <= 0.2 || opacity <= 0.01)
        return;
    p.fillPath(ovalPath(at.x(), at.y(), r, r),
               radial(at, r, {alpha(kSmoke, 0.55 * opacity), alpha(kViolet, 0.32 * opacity), alpha(kViolet, 0)}));
}

void puff(QPainter &p, const QPointF &at, double r, double opacity, quint32 seed, double swirl)
{
    if (opacity <= 0.01 || r <= 0.5)
        return;
    blob(p, at, r * 0.75, opacity);
    for (int i = 0; i < 8; ++i) {
        const double angle = i * M_PI / 4 + rnd(seed, i) * 0.6 + swirl;
        const double d = r * 0.5 * (0.7 + 0.4 * rnd(seed, i, 1));
        blob(p, at + QPointF(std::cos(angle) * d, std::sin(angle) * d * 0.8), r * 0.45 * (0.8 + 0.4 * rnd(seed, i, 2)),
             opacity);
    }
    for (int i = 0; i < 5; ++i) {
        const double angle = i * 1.26 + rnd(seed, i, 3) * 2 + swirl * 1.5;
        const double d = r * (0.6 + 0.5 * rnd(seed, i, 4));
        sparkle(p, at + QPointF(std::cos(angle) * d, std::sin(angle) * d), 2.2 * opacity,
                alpha(i % 2 ? kGold : kGlow, opacity));
    }
}

void trail(QPainter &p, const QList<QPointF> &points, double from, double to, const QColor &color, double width)
{
    const int n = int(points.size());
    if (n < 2)
        return;
    const int first = std::clamp(int(from * (n - 1)), 0, n - 1);
    const int last = std::clamp(int(std::ceil(to * (n - 1))), 0, n - 1);
    for (int k = first; k < last; ++k) {
        const double u = (k - first + 1.0) / std::max(1, last - first);
        QPainterPath seg;
        seg.moveTo(points.at(k));
        seg.lineTo(points.at(k + 1));
        stroke(p, seg, alpha(color, 0.3 * u), width * 3.2);
        stroke(p, seg, alpha(color, u), width);
    }
}

class Magic
{
public:
    explicit Magic(const GenieMagic &magic)
        : m(magic)
        , a(magic.age)
        , unit(magic.side / 100)
        , r(magic.target)
        , c(magic.target.center())
    {
    }

    void paint(QPainter &p)
    {
        switch (m.kind) {
        case GenieInteraction::Poof:
            poof(p);
            break;
        case GenieInteraction::Levitate:
            levitate(p);
            break;
        case GenieInteraction::Lamp:
            lamp(p);
            break;
        case GenieInteraction::Wishes:
            wishes(p);
            break;
        case GenieInteraction::SmokeRings:
            smokeRings(p);
            break;
        case GenieInteraction::Shimmer:
            shimmer(p);
            break;
        case GenieInteraction::Stardust:
            stardust(p);
            break;
        case GenieInteraction::Carpet:
            carpet(p);
            break;
        case GenieInteraction::Vortex:
            vortex(p);
            break;
        case GenieInteraction::Crystal:
            crystal(p);
            break;
        case GenieInteraction::Runes:
            runes(p);
            break;
        case GenieInteraction::Halo:
            halo(p);
            break;
        case GenieInteraction::Fireflies:
            fireflies(p);
            break;
        case GenieInteraction::Whirl:
            whirl(p);
            break;
        case GenieInteraction::Grow:
            grow(p);
            break;
        case GenieInteraction::Bottle:
            bottle(p);
            break;
        case GenieInteraction::Rainbow:
            rainbow(p);
            break;
        case GenieInteraction::Fireworks:
            fireworks(p);
            break;
        case GenieInteraction::Bubbles:
            bubbles(p);
            break;
        case GenieInteraction::Glitter:
            glitter(p);
            break;
        case GenieInteraction::JewelBeam:
            jewelBeam(p);
            break;
        case GenieInteraction::Ripple:
            ripple(p);
            break;
        case GenieInteraction::Moons:
            moons(p);
            break;
        case GenieInteraction::Constellation:
            constellation(p);
            break;
        case GenieInteraction::Crescent:
            crescent(p);
            break;
        case GenieInteraction::Mirage:
            mirage(p);
            break;
        case GenieInteraction::Coins:
            coins(p);
            break;
        case GenieInteraction::Flourish:
            flourish(p);
            break;
        case GenieInteraction::Incense:
            incense(p);
            break;
        case GenieInteraction::Whirlwind:
            whirlwind(p);
            break;
        default:
            break;
        }
    }

private:
    const GenieMagic &m;
    double a = 0;
    double unit = 0.78;
    QRectF r;
    QPointF c;

    QPointF body(double u, double v) const
    {
        return QPointF(m.foot.x() - m.side / 2 + u * unit, m.foot.y() - m.side + 3 + v * unit);
    }
    QPointF jewel() const { return body(50, 40.5); }
    QPointF crown() const { return body(50, 22); }
    double floor() const { return m.foot.y() + m.lift; }
    double dir() const { return c.x() >= m.foot.x() ? 1 : -1; }
    double life() const { return GenieMagic::length(m.kind); }
    QColor hue(int k) const
    {
        static const QColor colors[4] = {kGold, kGlow, Qt::white, kLilac};
        return colors[k % 4];
    }

    void poof(QPainter &p) const
    {
        const double u1 = span(a, 0.05, 1.05);
        if (u1 > 0 && u1 < 1)
            puff(p, m.from - QPointF(0, m.side * 0.42), m.side * (0.3 + 0.45 * easeOut(u1)), 1 - u1, m.seed, u1 * 1.4);
        const double u2 = span(a, GenieMagic::kJump - 0.08, GenieMagic::kJump + 0.95);
        if (u2 > 0 && u2 < 1)
            puff(p, m.foot - QPointF(0, m.side * 0.42), m.side * (0.3 + 0.45 * easeOut(u2)), 1 - u2, m.seed + 7,
                 -u2 * 1.4);
    }

    void levitate(QPainter &p) const
    {
        const double lift = m.lift;
        const double e = clamp01(lift / 8);
        if (e <= 0.01)
            return;
        const double ground = floor();
        p.save();
        p.translate(m.foot.x(), ground);
        p.scale(1, 0.18);
        p.fillPath(ovalPath(0, 0, 26 - lift * 0.3, 26 - lift * 0.3),
                   radial(QPointF(), 26, {alpha(kPitch, 0.3 * e), alpha(kPitch, 0)}));
        p.restore();
        QList<QPointF> points;
        const int n = 16;
        for (int k = 0; k <= n; ++k) {
            const double u = double(k) / n;
            const double y = m.foot.y() - 8 + u * (lift + 6);
            const double sway = std::sin(u * 5 + m.t * 4) * (2 + u * 7);
            points << QPointF(m.foot.x() + sway, y);
        }
        for (int k = 0; k < n; ++k) {
            const double u = double(k) / n;
            QPainterPath seg;
            seg.moveTo(points.at(k));
            seg.lineTo(points.at(k + 1));
            stroke(p, seg, alpha(kSmoke, 0.32 * e), 13 - u * 9);
            stroke(p, seg, alpha(kLilac, 0.45 * e), 6 - u * 4.5);
        }
        const QPointF tip = points.last();
        QPainterPath curl;
        curl.moveTo(tip);
        curl.cubicTo(tip + QPointF(8, 2), tip + QPointF(9, -7), tip + QPointF(3, -6));
        stroke(p, curl, alpha(kLilac, 0.5 * e), 2);
        for (int k = 0; k < 4; ++k) {
            const double phase = std::fmod(m.t * 0.9 + k * 0.25, 1.0);
            const QPointF at = points.at(int(phase * n)) + QPointF(std::sin(k * 2.0 + m.t * 3) * 8, 0);
            sparkle(p, at, 1.8, alpha(k % 2 ? kGold : kGlow, e * (1 - phase)));
        }
    }

    void lamp(QPainter &p) const
    {
        const double appear = easeOut(span(a, 0, 0.35));
        const double fade = 1 - span(a, 2.7, 3.2);
        const double op = appear * fade;
        if (op <= 0.01)
            return;
        const double d = dir();
        const QPointF base(m.foot.x() + d * m.side * 0.52, floor());
        const QPointF tip = base + QPointF(d * 23 * appear, -18 * appear);
        const double reach = ease(span(a, 0.45, 2.0));
        if (reach > 0) {
            QList<QPointF> points;
            const QPointF peak = (tip + c) / 2 + QPointF(0, -40);
            const QPointF side = unitOf(QPointF(-(c.y() - tip.y()), c.x() - tip.x()));
            const int n = 28;
            for (int k = 0; k <= n; ++k) {
                const double u = double(k) / n * reach;
                points << bezier(tip, peak, c, u) + side * (std::sin(u * 9 - a * 5) * 7 * u);
            }
            for (int k = 0; k < n; ++k) {
                const double u = double(k) / n;
                blob(p, points.at(k), 3 + u * 7, fade * (0.5 + 0.5 * u));
            }
            if (reach >= 1) {
                const double burst = span(a, 2.0, 2.7);
                for (int k = 0; k < 7; ++k) {
                    const double angle = k * 2 * M_PI / 7 + 0.4;
                    const double dist = 6 + 22 * easeOut(burst);
                    sparkle(p, c + QPointF(std::cos(angle) * dist, std::sin(angle) * dist), 2.6 * (1 - burst),
                            alpha(k % 2 ? kGold : kGlow, 1 - burst));
                }
            }
        }
        p.save();
        p.translate(base);
        p.scale(d * appear, appear);
        p.setOpacity(p.opacity() * fade);
        glow(p, 0, -10, 26, alpha(kGold, 0.35));
        QPainterPath foot;
        foot.addEllipse(oval(0, -1.6, 7, 2));
        foot.addRect(QRectF(-3, -6, 6, 4.6));
        p.fillPath(foot, linear(QPointF(0, -6), QPointF(0, 0), {kGold, kGoldDeep}));
        QPainterPath bowl;
        bowl.moveTo(-13, -10);
        bowl.cubicTo(-13, -3, 10, -3, 11, -10);
        bowl.lineTo(17, -12.5);
        bowl.quadTo(21, -14, 23, -18);
        bowl.quadTo(19, -15.5, 13.5, -14.5);
        bowl.quadTo(8, -16.5, 0, -16.5);
        bowl.quadTo(-13, -16.5, -13, -10);
        bowl.closeSubpath();
        p.fillPath(bowl, linear(QPointF(-6, -18), QPointF(6, -4), {kGoldPale, kGold, kGoldDeep}));
        stroke(p, bowl, alpha(kGoldDeep, 0.9), 0.7);
        QPainterPath shine;
        shine.moveTo(-9, -12.5);
        shine.quadTo(-4, -15, 3, -14.8);
        stroke(p, shine, alpha(Qt::white, 0.75), 1);
        QPainterPath handle;
        handle.moveTo(-12, -13);
        handle.cubicTo(-20, -16, -20, -6, -12, -7.5);
        stroke(p, handle, kGoldDeep, 2.2);
        stroke(p, handle, kGold, 1.2);
        p.fillPath(ovalPath(-1, -17, 5, 1.8), linear(QPointF(-1, -19), QPointF(-1, -15), {kGoldPale, kGoldDeep}));
        p.fillPath(ovalPath(-1, -19.6, 1.7, 1.7), radial(QPointF(-1.5, -20.2), 2, {Qt::white, kGold, kGoldDeep}));
        p.restore();
    }

    void wishes(QPainter &p) const
    {
        const QPointF from = jewel();
        const QColor colors[3] = {kGold, kGlow, kLilac};
        for (int i = 0; i < 3; ++i) {
            const double start = 0.2 + 0.7 * i;
            const double fly = span(a, start, start + 0.85);
            const double burst = span(a, start + 0.85, start + 1.4);
            const QPointF dest(r.left() + r.width() * (0.25 + 0.25 * i), c.y());
            const QPointF peak = (from + dest) / 2 + QPointF(0, -50 - 18 * i);
            if (fly > 0 && fly < 1) {
                for (int k = 4; k >= 0; --k) {
                    const double u = ease(fly - k * 0.04);
                    if (fly - k * 0.04 <= 0)
                        continue;
                    const QPointF at = bezier(from, peak, dest, u);
                    const double size = k == 0 ? 4 : 2.6 - k * 0.4;
                    dot(p, at, size, alpha(colors[i], k == 0 ? 1 : 0.5 - k * 0.08));
                }
            }
            if (burst > 0 && burst < 1) {
                glow(p, dest.x(), dest.y(), 16 * (1 - burst), alpha(colors[i], 0.7 * (1 - burst)));
                for (int k = 0; k < 8; ++k) {
                    const double angle = k * M_PI / 4 + i;
                    const double dist = 4 + 22 * easeOut(burst);
                    sparkle(p, dest + QPointF(std::cos(angle) * dist, std::sin(angle) * dist), 2.8 * (1 - burst * 0.6),
                            alpha(k % 2 ? colors[i] : QColor(Qt::white), 1 - burst));
                }
            }
        }
    }

    void smokeRings(QPainter &p) const
    {
        const QPointF mouth = body(50 + dir() * 10, 74);
        for (int i = 0; i < 3; ++i) {
            const double start = 0.2 + 0.6 * i;
            const double u = span(a, start, start + 1.35);
            if (u <= 0 || u >= 1)
                continue;
            const QPointF at = mix(mouth, c, easeOut(u)) + QPointF(0, -std::sin(u * M_PI) * 14);
            const double radius = 4 + 17 * easeOut(u);
            const double op = (1 - u) * std::min(1.0, u * 6);
            const QRectF ring = oval(at.x(), at.y(), radius * 0.5, radius);
            QPainterPath path;
            path.addEllipse(ring);
            stroke(p, path, alpha(kViolet, 0.45 * op), 8 - 3 * u);
            stroke(p, path, alpha(kSmoke, 0.75 * op), 3.6 - 1.4 * u);
            stroke(p, path.translated(-0.8, -0.8), alpha(Qt::white, 0.3 * op), 0.7);
        }
    }

    void shimmer(QPainter &p) const
    {
        const double env = window(a, 0.3, 0.45, 1.8, 2.05);
        if (env <= 0.01)
            return;
        const double u = ease(span(a, 0.3, 1.95));
        const double band = std::max(26.0, r.width() * 0.28);
        const double x = r.left() - band + (r.width() + 2 * band) * u;
        QPainterPath area;
        area.addRoundedRect(r.adjusted(-2, -2, 2, 2), 4, 4);
        p.save();
        p.setClipPath(area);
        QLinearGradient light(QPointF(x - band, r.top()), QPointF(x + band, r.top() + band * 0.35));
        light.setColorAt(0, alpha(kGold, 0));
        light.setColorAt(0.35, alpha(kGold, 0.4 * env));
        light.setColorAt(0.5, alpha(kGoldPale, 0.65 * env));
        light.setColorAt(0.65, alpha(kGold, 0.4 * env));
        light.setColorAt(1, alpha(kGold, 0));
        p.fillRect(r.adjusted(-2, -2, 2, 2), light);
        p.restore();
        QLinearGradient edge(QPointF(x - band, 0), QPointF(x + band, 0));
        edge.setColorAt(0, alpha(kGold, 0));
        edge.setColorAt(0.5, alpha(kGoldPale, 0.9 * env));
        edge.setColorAt(1, alpha(kGold, 0));
        p.setPen(QPen(QBrush(edge), 1.4));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(r.adjusted(-2, -2, 2, 2), 4, 4);
        sparkle(p, QPointF(std::clamp(x, r.left(), r.right()), r.top() - 2), 3.2 * env, alpha(kGoldPale, env));
    }

    void stardust(QPainter &p) const
    {
        const QPointF from = crown();
        const double fade = 1 - span(a, 2.6, 3.2);
        const int n = 26;
        for (int k = 0; k < n; ++k) {
            const double launch = 0.15 + 1.25 * k / n;
            const double u = span(a, launch, launch + 0.75);
            if (u <= 0)
                continue;
            const QPointF dest(r.left() + 6 + std::max(0.0, r.width() - 12) * rnd(m.seed, k), r.top() - 1 - 3 * rnd(m.seed, k, 1));
            const QPointF peak = (from + dest) / 2 + QPointF(0, -46 - 0.12 * std::abs(dest.x() - from.x()));
            const QColor color = hue(k);
            if (u < 1) {
                for (int j = 2; j >= 0; --j) {
                    const double v = easeOut(u - j * 0.05);
                    if (u - j * 0.05 <= 0)
                        continue;
                    sparkle(p, bezier(from, peak, dest, v), j == 0 ? 2.2 : 1.2, alpha(color, fade * (1 - j * 0.3)));
                }
            } else {
                const double twinkle = 0.55 + 0.45 * std::sin(m.t * 9 + k * 1.7);
                sparkle(p, dest, 1.9 * twinkle, alpha(color, fade * (0.5 + 0.5 * twinkle)));
            }
        }
    }

    void carpet(QPainter &p) const
    {
        const double d = dir();
        const double in = ease(span(a, 0, 0.6));
        const double out = ease(span(a, 3.0, 3.4));
        const double op = window(a, 0, 0.2, 3.15, 3.4);
        if (op <= 0.01)
            return;
        const double cx = m.foot.x() - d * 90 * (1 - in) + d * 90 * out;
        const double top = m.foot.y() + 1;
        const double fly = clamp01(m.lift / 22);
        if (fly > 0.02) {
            p.save();
            p.translate(cx, floor());
            p.scale(1, 0.14);
            p.fillPath(ovalPath(0, 0, 34, 34), radial(QPointF(), 34, {alpha(kPitch, 0.3 * fly * op), alpha(kPitch, 0)}));
            p.restore();
        }
        const int n = 14;
        const double half = 37;
        QList<QPointF> upper;
        for (int k = 0; k <= n; ++k) {
            const double x = -half + 2 * half * k / n;
            const double wave = std::sin(x * 0.14 - m.t * 7 * d) * (0.8 + 2.4 * fly + (1 - in) * 2);
            const double curl = std::pow(std::abs(x) / half, 6) * -3;
            upper << QPointF(cx + x, top + wave + curl);
        }
        QPainterPath cloth;
        cloth.moveTo(upper.first());
        for (int k = 1; k <= n; ++k)
            cloth.lineTo(upper.at(k));
        for (int k = n; k >= 0; --k)
            cloth.lineTo(upper.at(k) + QPointF(0, 6));
        cloth.closeSubpath();
        p.save();
        p.setOpacity(p.opacity() * op);
        p.fillPath(cloth, linear(QPointF(cx, top), QPointF(cx, top + 6), {kViolet, kPlum, kPitch}));
        QPainterPath border;
        border.moveTo(upper.first() + QPointF(0, 1.2));
        for (int k = 1; k <= n; ++k)
            border.lineTo(upper.at(k) + QPointF(0, 1.2));
        stroke(p, border, kGold, 0.9);
        QPainterPath seam;
        seam.moveTo(upper.first() + QPointF(0, 3.4));
        for (int k = 1; k <= n; ++k)
            seam.lineTo(upper.at(k) + QPointF(0, 3.4));
        QPen dashed = pen(alpha(kGlow, 0.8), 0.8);
        dashed.setDashPattern({2, 2.5});
        p.strokePath(seam, dashed);
        for (const QPointF &end : {upper.first(), upper.last()}) {
            const double outward = end.x() < cx ? -1 : 1;
            for (int t = 0; t < 3; ++t) {
                QPainterPath tassel;
                const QPointF from = end + QPointF(0, 1 + t * 2);
                tassel.moveTo(from);
                tassel.lineTo(from + QPointF(outward * 4, 1 + std::sin(m.t * 9 + t) * 1.2));
                stroke(p, tassel, kGold, 0.8);
            }
        }
        p.restore();
    }

    void vortex(QPainter &p) const
    {
        const double u = span(a, 0.2, 2.2);
        const double in = span(a, 0.2, 0.5);
        const double outer = std::min(std::max(r.width(), r.height()) / 2 + 26, 150.0);
        const double squash = std::clamp(r.height() / std::max(r.width(), 1.0) + 0.45, 0.55, 1.0);
        const int n = 16;
        if (u < 1) {
            for (int k = 0; k < n; ++k) {
                const double phase = k * 2 * M_PI / n;
                const double radius = outer * (1 - ease(u)) * (0.85 + 0.3 * rnd(m.seed, k)) + 2;
                for (int j = 3; j >= 0; --j) {
                    const double angle = phase + ease(u) * 5 * M_PI + a * 2 - j * 0.14;
                    const QPointF at = c + QPointF(std::cos(angle) * radius, std::sin(angle) * radius * squash);
                    const QColor color = k % 3 == 0 ? kGold : k % 3 == 1 ? kGlow : kLilac;
                    if (j == 0)
                        sparkle(p, at, 2.2, alpha(color, in));
                    else
                        dot(p, at, 1.2 - j * 0.25, alpha(color, in * (0.6 - j * 0.15)));
                }
            }
        }
        const double flash = span(a, 2.2, 2.8);
        if (flash > 0 && flash < 1) {
            glow(p, c.x(), c.y(), 10 + 40 * easeOut(flash), alpha(Qt::white, 0.85 * (1 - flash)));
            glow(p, c.x(), c.y(), 16 + 50 * easeOut(flash), alpha(kGlow, 0.5 * (1 - flash)));
            QPainterPath ring;
            ring.addEllipse(oval(c.x(), c.y(), 6 + 34 * easeOut(flash), (6 + 34 * easeOut(flash)) * squash));
            stroke(p, ring, alpha(kGlow, 0.8 * (1 - flash)), 1.4);
        }
    }

    void crystal(QPainter &p) const
    {
        const double u = span(a, 0, 0.45);
        const double appear = u < 1 ? easeOut(u) + std::sin(u * M_PI) * 0.15 : 1;
        const double fade = 1 - span(a, 2.7, 3.2);
        const double op = clamp01(u * 3) * fade;
        if (op <= 0.01)
            return;
        const double ground = floor();
        const QPointF ball(m.foot.x() + dir() * m.side * 0.56, ground - 22);
        const double radius = 12.5 * appear;
        p.save();
        p.setOpacity(p.opacity() * op);
        QPainterPath stand;
        stand.moveTo(ball.x() - 6, ball.y() + 10.5);
        stand.lineTo(ball.x() + 6, ball.y() + 10.5);
        stand.lineTo(ball.x() + 9.5, ground);
        stand.lineTo(ball.x() - 9.5, ground);
        stand.closeSubpath();
        p.fillPath(stand, linear(QPointF(ball.x(), ball.y() + 10), QPointF(ball.x(), ground), {kGoldPale, kGold, kGoldDeep}));
        glow(p, ball.x(), ball.y(), radius * 2.1, alpha(kGlow, 0.3));
        QPainterPath sphere = ovalPath(ball.x(), ball.y(), radius, radius);
        p.fillPath(sphere, radial(ball + QPointF(-3, -3), radius * 1.3,
                                  {alpha(kGlowSoft, 0.35), alpha(kLilac, 0.4), alpha(kPlum, 0.75)}));
        p.save();
        p.setClipPath(sphere);
        for (int k = 0; k < 4; ++k) {
            const double angle = a * 2.6 + k * M_PI / 2;
            const QPointF at = ball + QPointF(std::cos(angle) * radius * 0.42, std::sin(angle) * radius * 0.28);
            glow(p, at.x(), at.y(), radius * 0.6, alpha(k % 2 ? kGlow : kLilac, 0.55));
        }
        QPainterPath swirl;
        for (int k = 0; k <= 20; ++k) {
            const double v = k / 20.0;
            const double angle = a * 3 + v * 2.6 * M_PI;
            const QPointF at = ball + QPointF(std::cos(angle) * radius * 0.7 * v, std::sin(angle) * radius * 0.45 * v);
            if (k == 0)
                swirl.moveTo(at);
            else
                swirl.lineTo(at);
        }
        stroke(p, swirl, alpha(Qt::white, 0.45), 1.2);
        p.restore();
        stroke(p, sphere, alpha(Qt::white, 0.5), 0.8);
        p.fillPath(ovalPath(ball.x() - radius * 0.38, ball.y() - radius * 0.42, radius * 0.28, radius * 0.18),
                   alpha(Qt::white, 0.7));
        const double glint = std::fmod(a, 1.1);
        if (glint < 0.35)
            sparkle(p, ball + QPointF(radius * 0.5, -radius * 0.6), 2.4 * std::sin(glint / 0.35 * M_PI), kGoldPale);
        p.restore();
    }

    void runes(QPainter &p) const
    {
        static const QList<QList<QPointF>> shapes[6] = {
            {{{-5, 5}, {0, -6}, {5, 5}, {-5, 5}}, {{0, -1}, {0, 2}}},
            {{{-4, -3}, {0, -7}, {4, -3}}, {{0, -7}, {0, 7}}},
            {{{0, -7}, {4, 0}, {0, 7}, {-4, 0}, {0, -7}}},
            {{{-4, -6}, {4, -6}, {-4, 6}, {4, 6}, {-4, -6}}},
            {{{0, -7}, {0, 7}}, {{-4, -4}, {0, 0}, {4, -4}}, {{-3, 7}, {3, 7}}},
            {{{-4, -5}, {0, -2}, {4, -5}}, {{-4, 1}, {0, 4}, {4, 1}}},
        };
        const double fade = 1 - span(a, 2.6, 3.2);
        const double rx = r.width() / 2 + 20;
        const double ry = r.height() / 2 + 16;
        QPainterPath orbit;
        orbit.addEllipse(oval(c.x(), c.y(), rx, ry));
        stroke(p, orbit, alpha(kLilac, 0.18 * fade * span(a, 0.1, 0.6)), 1);
        for (int k = 0; k < 6; ++k) {
            const double op = span(a, 0.1 + 0.12 * k, 0.45 + 0.12 * k) * fade;
            if (op <= 0.01)
                continue;
            const double angle = k * M_PI / 3 + a * 0.9 - M_PI / 2;
            const QPointF at = c + QPointF(std::cos(angle) * rx, std::sin(angle) * ry);
            const double scale = 0.9 + 0.1 * std::sin(m.t * 5 + k);
            glow(p, at.x(), at.y(), 12, alpha(kGlow, 0.25 * op));
            QPainterPath rune;
            for (const QList<QPointF> &line : shapes[k]) {
                for (int j = 0; j < line.size(); ++j) {
                    const QPointF q = at + line.at(j) * scale;
                    if (j == 0)
                        rune.moveTo(q);
                    else
                        rune.lineTo(q);
                }
            }
            stroke(p, rune, alpha(kGlow, 0.35 * op), 3.4);
            stroke(p, rune, alpha(k % 2 ? QColor(220, 255, 250) : QColor(222, 206, 255), op), 1.3);
        }
    }

    void halo(QPainter &p) const
    {
        const double op = span(a, 0.1, 0.35) * (1 - span(a, 2.2, 2.8));
        if (op <= 0.01)
            return;
        const double rise = ease(span(a, 0.2, 1.2));
        const double rx = std::clamp(r.width() * 0.3, 12.0, 30.0);
        const double ry = rx * 0.28;
        const double y = c.y() + (r.top() - 14 - ry - c.y()) * rise + std::sin(a * 3) * 2 * rise;
        const QPointF at(c.x(), y);
        glow(p, at.x(), at.y(), rx * 1.5, alpha(kGold, 0.28 * op));
        QPainterPath ring = ovalPath(at.x(), at.y(), rx, ry);
        stroke(p, ring, alpha(kGold, 0.35 * op), 5);
        stroke(p, ring, alpha(kGold, op), 2.2);
        QPainterPath front;
        front.arcMoveTo(oval(at.x(), at.y(), rx, ry), 200);
        front.arcTo(oval(at.x(), at.y(), rx, ry), 200, 140);
        stroke(p, front, alpha(kGoldPale, op), 0.9);
        const double glint = std::fmod(a, 1.2);
        if (glint < 0.4)
            sparkle(p, at + QPointF(rx * 0.7, -ry * 0.5), 2.8 * std::sin(glint / 0.4 * M_PI), alpha(Qt::white, op));
    }

    void fireflies(QPainter &p) const
    {
        const QPointF from = crown();
        const double fade = 1 - span(a, 2.8, 3.4);
        for (int k = 0; k < 10; ++k) {
            const double start = 0.1 + 0.12 * k;
            const double u = span(a, start, start + 1.7);
            if (u <= 0)
                continue;
            const QPointF dest(r.left() + r.width() * (0.1 + 0.8 * rnd(m.seed, k)),
                               r.top() + r.height() * (0.1 + 0.8 * rnd(m.seed, k, 1)));
            const QPointF peak = (from + dest) / 2 + QPointF((rnd(m.seed, k, 2) - 0.5) * 60, -40 - 30 * rnd(m.seed, k, 3));
            const QPointF wander(std::sin(m.t * 1.7 + k * 1.3) * 7, std::cos(m.t * 2.3 + k * 0.7) * 5);
            const QPointF at = bezier(from, peak, dest, ease(u)) + wander * std::min(1.0, u * 2);
            const double blink = 0.5 + 0.5 * std::sin(m.t * 6 + k * 2.1);
            const QColor color = k % 3 == 0 ? kGlow : k % 3 == 1 ? kGold : QColor(214, 255, 150);
            dot(p, at, 1.5, alpha(color, fade * std::min(1.0, u * 5) * (0.35 + 0.65 * blink)));
        }
    }

    void whirl(QPainter &p) const
    {
        const double env = window(a, 0.1, 0.4, 1.9, 2.4);
        if (env <= 0.01)
            return;
        for (int ring = 0; ring < 2; ++ring) {
            const double y = m.foot.y() - m.side * (ring ? 0.58 : 0.22);
            const double rx = m.side * (ring ? 0.5 : 0.56);
            const double ry = m.side * 0.13;
            for (int k = 0; k < 10; ++k) {
                const double angle = k * M_PI / 5 + a * 7 * (ring ? -1 : 1) + ring;
                const QPointF at(m.foot.x() + std::cos(angle) * rx, y + std::sin(angle) * ry);
                blob(p, at, 6 + 2 * std::sin(angle * 2 + a * 3), env * (0.55 + 0.45 * std::sin(angle)));
            }
            QPainterPath swish;
            const QRectF box = oval(m.foot.x(), y, rx * 1.08, ry * 1.3);
            const double startDeg = std::fmod(a * 400 * (ring ? -1 : 1) + ring * 90, 360);
            swish.arcMoveTo(box, startDeg);
            swish.arcTo(box, startDeg, 110);
            stroke(p, swish, alpha(kLilac, 0.55 * env), 1.6);
        }
    }

    void grow(QPainter &p) const
    {
        const QPointF centre(m.foot.x(), m.foot.y() - m.side * 0.48 * m.grow);
        const double first = window(a, 0.15, 0.35, 0.8, 1.35);
        const double second = window(a, 1.95, 2.1, 2.5, 3.0);
        puff(p, centre, m.side * (0.55 + 0.25 * span(a, 0.15, 1.35)) * m.grow, first, m.seed, a);
        puff(p, centre, m.side * (0.45 + 0.2 * span(a, 1.95, 3.0)) * m.grow, second, m.seed + 3, -a);
        if (m.grow > 1.001) {
            const double side = m.side * m.grow;
            GenieSpiderPainter::paint(p, QRectF(m.foot.x() - side / 2, m.foot.y() - (m.side - 3) * m.grow, side, side),
                                      m.pose);
        }
        const double wisps = std::max(first, second) * 0.6;
        for (int k = 0; k < 3; ++k) {
            const double angle = k * 2.1 + a * 1.5;
            blob(p, centre + QPointF(std::cos(angle) * m.side * 0.45 * m.grow, m.side * 0.3 * m.grow), 9 * m.grow,
                 wisps);
        }
    }

    void bottle(QPainter &p) const
    {
        const double appear = easeOut(span(a, 0, 0.35));
        const double fade = 1 - span(a, 2.6, 3.2);
        const double op = clamp01(appear * 2) * fade;
        if (op <= 0.01)
            return;
        const QPointF base(m.foot.x(), floor());
        const double inside = window(a, 1.0, 1.2, 1.9, 2.05);
        const double wobble = std::sin(a * 14) * 6 * window(a, 1.2, 1.3, 1.8, 2.0);
        p.save();
        p.translate(base);
        p.rotate(wobble);
        p.scale(0.4 + 0.6 * appear, 0.4 + 0.6 * appear);
        p.setOpacity(p.opacity() * op);
        QPainterPath glass;
        glass.moveTo(-5, -34);
        glass.lineTo(-5, -26);
        glass.cubicTo(-16, -22, -15, -1, 0, -1);
        glass.cubicTo(15, -1, 16, -22, 5, -26);
        glass.lineTo(5, -34);
        glass.closeSubpath();
        glow(p, 0, -14, 24 + 8 * inside, alpha(kGlow, 0.25 + 0.3 * inside));
        p.fillPath(glass, radial(QPointF(-3, -14), 18,
                                 {alpha(kGlowSoft, 0.35 + 0.3 * inside), alpha(kLilac, 0.45), alpha(kPlum, 0.7)}));
        if (inside > 0.01) {
            p.save();
            p.setClipPath(glass);
            for (int k = 0; k < 3; ++k) {
                const double angle = a * 6 + k * 2.1;
                glow(p, std::cos(angle) * 5, -12 + std::sin(angle) * 5, 7, alpha(k % 2 ? kGlow : kLilac, 0.7 * inside));
            }
            p.restore();
        }
        stroke(p, glass, alpha(Qt::white, 0.6), 1);
        QPainterPath shine;
        shine.moveTo(-8, -21);
        shine.quadTo(-11, -12, -6, -5);
        stroke(p, shine, alpha(Qt::white, 0.55), 1.2);
        QPainterPath lip;
        lip.addRoundedRect(QRectF(-6.5, -37.5, 13, 3.6), 1.6, 1.6);
        p.fillPath(lip, linear(QPointF(0, -37.5), QPointF(0, -34), {kGoldPale, kGoldDeep}));
        p.restore();
        const double pop = span(a, 2.0, 2.65);
        if (pop > 0 && pop < 1) {
            const QPointF neck = base + QPointF(0, -38);
            for (int k = 0; k < 10; ++k) {
                const double angle = -M_PI / 2 + (k - 4.5) * 0.32;
                const double dist = 4 + 30 * easeOut(pop);
                sparkle(p, neck + QPointF(std::cos(angle) * dist, std::sin(angle) * dist), 2.6 * (1 - pop),
                        alpha(hue(k), 1 - pop));
            }
            blob(p, neck, 10 * (1 - pop) + 4, 1 - pop);
        }
        const double draw = window(a, 0.35, 0.5, 1.0, 1.15);
        if (draw > 0.01) {
            const QPointF neck = base + QPointF(0, -38);
            for (int k = 0; k < 6; ++k) {
                const double angle = a * 9 + k * M_PI / 3;
                const double dist = 14 * (1 - span(a, 0.35, 1.15)) + 3;
                sparkle(p, neck + QPointF(std::cos(angle) * dist, std::sin(angle) * dist * 0.6 - 10 * (1 - span(a, 0.35, 1.15))),
                        1.6, alpha(k % 2 ? kGlow : kLilac, draw));
            }
        }
    }

    void rainbow(QPainter &p) const
    {
        const QPointF from = jewel();
        const QPointF to = c;
        const QPointF peak = rainbowPeak(from, to);
        const double reach = ease(span(a, 0.2, 1.1));
        const double fade = 1 - span(a, 2.3, 3.0);
        if (reach <= 0 || fade <= 0)
            return;
        static const QColor bands[6] = {QColor(255, 120, 150), kGold, QColor(160, 240, 120), kGlow, kGlowSoft, kLilac};
        const int n = 36;
        for (int b = 0; b < 6; ++b) {
            const QPointF shift(0, (b - 2.5) * 2.6);
            QPainterPath path;
            for (int k = 0; k <= n; ++k) {
                const QPointF at = bezier(from + shift * 0.3, peak + shift, to + shift * 0.6, reach * k / n);
                if (k == 0)
                    path.moveTo(at);
                else
                    path.lineTo(at);
            }
            const double shimmer = 0.75 + 0.25 * std::sin(m.t * 5 + b * 0.9);
            stroke(p, path, alpha(bands[b], 0.18 * fade * shimmer), 6);
            stroke(p, path, alpha(bands[b], 0.6 * fade * shimmer), 2.4);
        }
        const QPointF head = bezier(from, peak, to, reach);
        sparkle(p, head, 3.4 * fade, alpha(Qt::white, fade));
        if (reach >= 1)
            glow(p, to.x(), to.y(), 18, alpha(kGoldPale, 0.35 * fade));
    }

    void fireworks(QPainter &p) const
    {
        const QPointF launch(c.x(), r.top());
        const QPointF burstAt(c.x(), r.top() - 46);
        const double climb = span(a, 0.15, 0.6);
        if (climb > 0 && climb < 1) {
            const QPointF at = mix(launch, burstAt, easeOut(climb));
            for (int j = 3; j >= 0; --j)
                dot(p, at + QPointF(std::sin(j + a * 30) * 0.8, j * 4), 1.8 - j * 0.35, alpha(kGold, 1 - j * 0.22));
        }
        const auto burst = [&](const QPointF &centre, double from, double to, int n, double speed, const QColor &color,
                               int salt) {
            const double f = span(a, from, to);
            if (f <= 0 || f >= 1)
                return;
            if (f < 0.25)
                glow(p, centre.x(), centre.y(), 18 * (1 - f * 4) + 4, alpha(Qt::white, 0.8 * (1 - f * 4)));
            for (int k = 0; k < n; ++k) {
                const double angle = k * 2 * M_PI / n + rnd(m.seed, k, salt) * 0.25;
                const double v = speed * (0.8 + 0.35 * rnd(m.seed, k, salt + 1));
                const QPointF dirv(std::cos(angle), std::sin(angle));
                const QPointF at = centre + dirv * (v * easeOut(f)) + QPointF(0, 18 * f * f);
                const QPointF tail = centre + dirv * (v * easeOut(std::max(0.0, f - 0.12))) +
                                     QPointF(0, 18 * std::max(0.0, f - 0.12) * std::max(0.0, f - 0.12));
                QPainterPath streak;
                streak.moveTo(tail);
                streak.lineTo(at);
                const QColor tint = k % 3 == 0 ? QColor(Qt::white) : color;
                stroke(p, streak, alpha(tint, 0.7 * (1 - f)), 1.2);
                dot(p, at, 1.3, alpha(tint, 1 - f));
            }
        };
        burst(burstAt, 0.6, 1.9, 18, 34, kGold, 4);
        burst(burstAt + QPointF(dir() * -22, 14), 1.0, 2.2, 12, 22, kGlow, 9);
        burst(burstAt + QPointF(dir() * 20, 8), 1.35, 2.5, 10, 18, kLilac, 13);
    }

    void bubbles(QPainter &p) const
    {
        const int n = 10;
        for (int k = 0; k < n; ++k) {
            const double start = 0.1 + 0.2 * k;
            const double lifeK = 1.1 + 0.4 * rnd(m.seed, k);
            const double u = span(a, start, start + lifeK);
            if (u <= 0 || u >= 1)
                continue;
            const double x0 = r.left() + r.width() * (0.1 + 0.8 * rnd(m.seed, k, 1));
            const double y0 = r.top() + r.height() * 0.45;
            const QPointF at(x0 + std::sin(u * 6 + k) * 6, y0 - u * (60 + 30 * rnd(m.seed, k, 2)));
            const double radius = (3.5 + 5 * rnd(m.seed, k, 3)) * (0.6 + 0.4 * std::min(1.0, u * 4));
            if (u < 0.9) {
                p.fillPath(ovalPath(at.x(), at.y(), radius, radius),
                           radial(at, radius, {alpha(Qt::white, 0.04), alpha(kGlowSoft, 0.12), alpha(kLilac, 0.28)}));
                QConicalGradient sheen(at, m.t * 90 + k * 40);
                sheen.setColorAt(0, alpha(kGlow, 0.9));
                sheen.setColorAt(0.33, alpha(kLilac, 0.9));
                sheen.setColorAt(0.66, alpha(kGold, 0.9));
                sheen.setColorAt(1, alpha(kGlow, 0.9));
                p.setPen(QPen(QBrush(sheen), 1.1));
                p.setBrush(Qt::NoBrush);
                p.drawEllipse(at, radius, radius);
                QPainterPath hi;
                hi.arcMoveTo(oval(at.x(), at.y(), radius * 0.65, radius * 0.65), 110);
                hi.arcTo(oval(at.x(), at.y(), radius * 0.65, radius * 0.65), 110, 60);
                stroke(p, hi, alpha(Qt::white, 0.85), 1);
            } else {
                const double pop = (u - 0.9) / 0.1;
                for (int j = 0; j < 6; ++j) {
                    const double angle = j * M_PI / 3;
                    const double d = radius * (1 + pop * 0.9);
                    dot(p, at + QPointF(std::cos(angle) * d, std::sin(angle) * d), 0.9, alpha(j % 2 ? kGlow : kLilac, 1 - pop));
                }
            }
        }
    }

    void glitter(QPainter &p) const
    {
        const double fade = 1 - span(a, 2.6, 3.0);
        for (int k = 0; k < 34; ++k) {
            const double start = 0.05 + 1.4 * rnd(m.seed, k, 1);
            const double u = span(a, start, start + 1.2 + 0.5 * rnd(m.seed, k, 3));
            if (u <= 0 || u >= 1)
                continue;
            const double x = r.left() + r.width() * rnd(m.seed, k, 2) + std::sin(u * 5 + k) * 6;
            const double y = r.top() - 46 + (r.height() + 50) * u;
            const double turn = m.t * 6 + k;
            const double flip = std::abs(std::cos(turn));
            const double op = fade * std::min(1.0, u * 6) * (1 - span(u, 0.8, 1)) * (0.35 + 0.65 * flip);
            const QColor color = hue(k);
            QPainterPath flake;
            const double s = 2.4;
            flake.moveTo(x, y - s);
            flake.lineTo(x + s * flip, y);
            flake.lineTo(x, y + s);
            flake.lineTo(x - s * flip, y);
            flake.closeSubpath();
            p.fillPath(flake, alpha(color, op));
            if (flip > 0.92)
                sparkle(p, QPointF(x, y), 2.6, alpha(Qt::white, op));
        }
    }

    void jewelBeam(QPainter &p) const
    {
        const QPointF from = jewel();
        const double grow = ease(span(a, 0.2, 0.65));
        const double fade = 1 - span(a, 2.2, 2.8);
        if (grow <= 0 || fade <= 0)
            return;
        const QPointF end = mix(from, c, grow);
        const QPointF across = unitOf(QPointF(-(c.y() - from.y()), c.x() - from.x()));
        const double wide = std::min(r.height() * 0.5, 14.0) * grow;
        QPolygonF cone;
        cone << from + across * 1.5 << end + across * wide << end - across * wide << from - across * 1.5;
        p.setPen(Qt::NoPen);
        p.setBrush(linear(from, end, {alpha(kGlowSoft, 0.6 * fade), alpha(kGlow, 0.22 * fade)}));
        p.drawPolygon(cone);
        QPainterPath core;
        core.moveTo(from);
        core.lineTo(end);
        stroke(p, core, alpha(Qt::white, 0.55 * fade), 1.1);
        glow(p, from.x(), from.y(), 10, alpha(Qt::white, 0.8 * fade));
        const double lit = span(a, 0.55, 0.9) * fade;
        if (lit > 0.01) {
            QPainterPath box;
            box.addRoundedRect(r.adjusted(-3, -3, 3, 3), 5, 5);
            p.fillPath(box, alpha(kGlow, 0.1 * lit * (0.8 + 0.2 * std::sin(m.t * 6))));
            stroke(p, box, alpha(kGlow, 0.25 * lit), 6);
            stroke(p, box, alpha(kGlow, 0.85 * lit), 1.5);
        }
    }

    void ripple(QPainter &p) const
    {
        const double reach = std::min(std::max(r.width(), r.height()) * 0.5 + 40, 180.0);
        for (int i = 0; i < 4; ++i) {
            const double start = 0.2 + 0.4 * i;
            const double u = span(a, start, start + 1.4);
            if (u <= 0 || u >= 1)
                continue;
            const double grow = easeOut(u);
            const double rx = r.width() / 2 * grow + reach * 0.5 * grow;
            const double ry = r.height() / 2 * grow + reach * 0.35 * grow;
            const double op = (1 - u) * std::min(1.0, u * 8);
            QPainterPath ring = ovalPath(c.x(), c.y(), rx, ry);
            stroke(p, ring, alpha(kGlow, 0.25 * op), 4.5);
            stroke(p, ring, alpha(QColor(210, 255, 248), 0.85 * op), 1.3);
        }
        glow(p, c.x(), c.y(), 12, alpha(kGlow, 0.4 * window(a, 0.1, 0.3, 1.6, 2.4)));
    }

    void moons(QPainter &p) const
    {
        const QPointF centre = body(50, 30);
        const double rx = m.side * 0.52;
        const double ry = m.side * 0.12;
        const double fade = 1 - span(a, 2.8, 3.4);
        const QColor tints[3] = {kSilver, kLilac, kGlowSoft};
        for (int k = 0; k < 3; ++k) {
            const double release = 1.8 + 0.15 * k;
            const double appear = span(a, 0.05 + 0.15 * k, 0.35 + 0.15 * k);
            const auto orbitAt = [&](double time) {
                const double angle = k * 2 * M_PI / 3 + time * 4.5;
                return centre + QPointF(std::cos(angle) * rx, std::sin(angle) * ry);
            };
            QPointF at;
            if (a < release) {
                at = orbitAt(a);
            } else {
                const QPointF start = orbitAt(release);
                const QPointF dest(r.left() + r.width() * (0.3 + 0.2 * k), c.y());
                const QPointF peak = (start + dest) / 2 + QPointF(0, -36);
                at = bezier(start, peak, dest, ease(span(a, release, release + 0.7)));
            }
            const double op = appear * fade;
            if (op <= 0.01)
                continue;
            const double arrived = span(a, release + 0.7, release + 0.9);
            glow(p, at.x(), at.y(), 9 + 6 * arrived, alpha(tints[k], 0.45 * op));
            p.fillPath(ovalPath(at.x(), at.y(), 3.6, 3.6),
                       radial(at + QPointF(-1.2, -1.2), 4.4, {alpha(Qt::white, op), alpha(tints[k], op), alpha(kViolet, op)}));
            p.fillPath(ovalPath(at.x() + 1, at.y() + 0.6, 0.9, 0.8), alpha(kViolet, 0.4 * op));
        }
    }

    void constellation(QPainter &p) const
    {
        const double fade = 1 - span(a, 2.8, 3.4);
        QList<QPointF> stars;
        for (int k = 0; k < 7; ++k) {
            const double angle = k * 2 * M_PI / 7 + rnd(m.seed, k) * 0.5 - M_PI / 2;
            const double rx = r.width() / 2 + 14 + 10 * rnd(m.seed, k, 1);
            const double ry = r.height() / 2 + 12 + 8 * rnd(m.seed, k, 2);
            stars << c + QPointF(std::cos(angle) * rx, std::sin(angle) * ry);
        }
        for (int k = 0; k < 7; ++k) {
            const double draw = span(a, 1.3 + 0.15 * k, 1.55 + 0.15 * k);
            if (draw <= 0)
                continue;
            const QPointF from = stars.at(k);
            const QPointF to = stars.at((k + 1) % 7);
            QPainterPath line;
            line.moveTo(from);
            line.lineTo(mix(from, to, draw));
            stroke(p, line, alpha(kGlowSoft, 0.45 * fade), 0.9);
        }
        for (int k = 0; k < 7; ++k) {
            const double u = span(a, 0.1 + 0.18 * k, 0.35 + 0.18 * k);
            if (u <= 0)
                continue;
            const double pop = u < 1 ? easeOut(u) + std::sin(u * M_PI) * 0.3 : 1;
            const double twinkle = 0.8 + 0.25 * std::sin(m.t * 7 + k * 1.3);
            sparkle(p, stars.at(k), 2.6 * pop * twinkle, alpha(k % 3 ? QColor(Qt::white) : kGold, fade));
        }
    }

    void crescent(QPainter &p) const
    {
        const double op = span(a, 0.1, 0.4) * (1 - span(a, 2.6, 3.2));
        if (op <= 0.01)
            return;
        const double rise = ease(span(a, 0.2, 1.2));
        const QPointF at(c.x(), r.top() - 4 - 30 * rise);
        const double radius = 11;
        glow(p, at.x(), at.y(), 26, alpha(kGold, 0.32 * op));
        p.save();
        p.translate(at);
        p.rotate(-20 + std::sin(a * 2.2) * 8);
        QPainterPath moon = ovalPath(0, 0, radius, radius);
        moon = moon.subtracted(ovalPath(radius * 0.45, -radius * 0.25, radius * 0.86, radius * 0.86));
        p.fillPath(moon, radial(QPointF(-radius * 0.5, 0), radius * 1.3, {alpha(kGoldPale, op), alpha(kGold, op)}));
        stroke(p, moon, alpha(Qt::white, 0.5 * op), 0.6);
        p.restore();
        const QPointF offsets[5] = {{-24, -6}, {-14, -20}, {18, -18}, {26, 4}, {-30, 10}};
        for (int k = 0; k < 5; ++k) {
            const double show = span(a, 0.6 + 0.12 * k, 0.85 + 0.12 * k) * op;
            const double twinkle = 0.6 + 0.4 * std::sin(m.t * 8 + k * 1.9);
            sparkle(p, at + offsets[k], 2.2 * twinkle, alpha(k % 2 ? kGoldPale : QColor(Qt::white), show));
        }
    }

    void mirage(QPainter &p) const
    {
        const double perimeter = 2 * (r.width() + r.height());
        const int n = std::clamp(int(perimeter / 6), 12, 240);
        for (int i = 0; i < 3; ++i) {
            const double start = 0.15 + 0.45 * i;
            const double u = span(a, start, start + 1.6);
            if (u <= 0 || u >= 1)
                continue;
            const double rise = 40 * ease(u);
            const double op = std::sin(u * M_PI) * 0.75;
            QPainterPath ghost;
            for (int k = 0; k <= n; ++k) {
                double d = perimeter * k / n;
                QPointF q;
                if (d < r.width()) {
                    q = QPointF(r.left() + d, r.top());
                } else if ((d -= r.width()) < r.height()) {
                    q = QPointF(r.right(), r.top() + d);
                } else if ((d -= r.height()) < r.width()) {
                    q = QPointF(r.right() - d, r.bottom());
                } else {
                    d -= r.width();
                    q = QPointF(r.left(), r.bottom() - d);
                }
                q += QPointF(std::sin(q.y() * 0.22 + m.t * 6 + i) * 5 * (0.4 + u),
                             std::sin(q.x() * 0.12 + m.t * 5 + i) * 2.5 * (0.4 + u) - rise);
                if (k == 0)
                    ghost.moveTo(q);
                else
                    ghost.lineTo(q);
            }
            ghost.closeSubpath();
            p.fillPath(ghost, alpha(kGlow, 0.05 * op));
            stroke(p, ghost, alpha(kGlow, 0.22 * op), 4);
            stroke(p, ghost, alpha(i % 2 ? kLilac : QColor(200, 255, 248), op), 1.2);
        }
    }

    void coins(QPainter &p) const
    {
        const double fade = 1 - span(a, 2.5, 3.0);
        const double x0 = dir() > 0 ? r.left() - 12 : r.right() + 12;
        const double ground = r.bottom();
        for (int k = 0; k < 7; ++k) {
            const double start = 0.1 + 0.18 * k;
            const double u = span(a, start, start + 0.55);
            if (u <= 0)
                continue;
            const double x = x0 + (rnd(m.seed, k) - 0.5) * 10;
            const double rest = ground - 2 - 2.6 * k;
            const double bounce = std::sin(M_PI * span(a, start + 0.55, start + 0.8)) * 4;
            const double y = r.top() - 40 + (rest - (r.top() - 40)) * u * u - bounce;
            p.save();
            p.translate(x, y);
            if (u < 1) {
                const double face = std::max(0.18, std::abs(std::cos(m.t * 12 + k)));
                p.scale(face, 1);
                p.fillPath(ovalPath(0, 0, 5.2, 5.2), radial(QPointF(-1.5, -1.5), 6.5, {alpha(kGoldPale, fade), alpha(kGold, fade), alpha(kGoldDeep, fade)}));
                QPainterPath rim = ovalPath(0, 0, 5.2, 5.2);
                stroke(p, rim, alpha(kGoldDeep, fade), 0.8);
                QPainterPath inner = ovalPath(0, 0, 3.2, 3.2);
                stroke(p, inner, alpha(kGoldDeep, 0.6 * fade), 0.6);
            } else {
                p.fillPath(ovalPath(0, 0, 5.6, 1.9), linear(QPointF(0, -1.9), QPointF(0, 1.9), {alpha(kGoldPale, fade), alpha(kGoldDeep, fade)}));
                QPainterPath rim = ovalPath(0, 0, 5.6, 1.9);
                stroke(p, rim, alpha(kGoldDeep, fade), 0.6);
            }
            p.restore();
            const double glint = span(a, start + 0.55, start + 0.95);
            if (glint > 0 && glint < 1)
                sparkle(p, QPointF(x + 3, y - 2), 3 * std::sin(glint * M_PI), alpha(Qt::white, fade));
        }
        const double shine = std::fmod(a, 0.9);
        if (a > 1.4 && shine < 0.3)
            sparkle(p, QPointF(x0 - 2, ground - 8), 2.6 * std::sin(shine / 0.3 * M_PI), alpha(kGoldPale, fade));
    }

    void flourish(QPainter &p) const
    {
        const QPointF from = m.hand;
        const QPointF to = c;
        const QPointF along = to - from;
        const QPointF ahead = unitOf(along);
        const QPointF across(-ahead.y(), ahead.x());
        const double bow = -std::copysign(1.0, ahead.x());
        QList<QPointF> points;
        const int n = 90;
        for (int k = 0; k <= n; ++k) {
            const double u = double(k) / n;
            const double loop = std::sin(u * M_PI) * 9;
            const double angle = u * 6 * M_PI;
            points << from + along * u + across * (bow * std::sin(u * M_PI) * 22) + ahead * (std::cos(angle) * loop - loop) +
                          across * (std::sin(angle) * loop);
        }
        const double head = ease(span(a, 0.15, 1.3));
        const double tail = ease(span(a, 1.3, 2.6));
        if (head <= tail)
            return;
        trail(p, points, tail, head, kGold, 1.6);
        trail(p, points, tail, head, kGoldPale, 0.6);
        const QPointF tip = points.at(std::clamp(int(head * n), 0, n));
        sparkle(p, tip, 3.4, alpha(Qt::white, 1 - tail));
        if (head >= 1)
            glow(p, to.x(), to.y(), 16, alpha(kGold, 0.4 * (1 - tail)));
    }

    void incense(QPainter &p) const
    {
        const double env = window(a, 0.1, 0.6, 2.5, 3.2);
        if (env <= 0.01)
            return;
        const double xs[4] = {-0.42, -0.15, 0.15, 0.42};
        for (int k = 0; k < 4; ++k) {
            const double grow = span(a, 0.1 + 0.15 * k, 1.2 + 0.15 * k);
            const double height = m.side * (0.9 + 0.3 * rnd(m.seed, k)) * grow;
            if (height < 2)
                continue;
            const QPointF base(m.foot.x() + xs[k] * m.side, m.foot.y() - 3 - (k == 1 || k == 2 ? 4 : 0));
            dot(p, base, 1.2, alpha(kGold, env));
            QList<QPointF> points;
            const int n = 14;
            for (int j = 0; j <= n; ++j) {
                const double v = double(j) / n;
                points << base + QPointF(std::sin(v * 5 + m.t * 2.2 + k * 1.7) * (1.5 + v * 7), -height * v);
            }
            for (int j = 0; j < n; ++j) {
                const double v = double(j) / n;
                QPainterPath seg;
                seg.moveTo(points.at(j));
                seg.lineTo(points.at(j + 1));
                stroke(p, seg, alpha(kSmoke, 0.3 * env * (1 - v)), 3.5 + v * 3);
                stroke(p, seg, alpha(kLilac, 0.55 * env * (1 - v)), 1.2 + v);
            }
        }
    }

    void whirlwind(QPainter &p) const
    {
        const double u = span(a, 0.2, 2.3);
        const double scatter = span(a, 2.3, 3.1);
        if (u <= 0 || scatter >= 1)
            return;
        const double ground = floor();
        for (int level = 0; level < 6; ++level) {
            const double shown = clamp01(u * 7 - level);
            if (shown <= 0)
                continue;
            const double y = ground - m.side * 0.2 * level - 14 * scatter;
            const double rx = m.side * (0.26 + 0.07 * level) * (1 + 1.6 * scatter);
            const double ry = rx * 0.22;
            const double op = shown * (1 - scatter);
            for (int k = 0; k < 7; ++k) {
                const double angle = k * 2 * M_PI / 7 + a * (9 - level * 0.6) + level;
                const QPointF at(m.foot.x() + std::cos(angle) * rx, y + std::sin(angle) * ry);
                blob(p, at, 4.5 + level * 0.6, op * (0.6 + 0.4 * std::sin(angle)));
            }
            QPainterPath streak;
            const QRectF box = oval(m.foot.x(), y, rx, ry);
            const double startDeg = std::fmod(-a * 520 - level * 50, 360);
            streak.arcMoveTo(box, startDeg);
            streak.arcTo(box, startDeg, 120);
            stroke(p, streak, alpha(kLilac, 0.5 * op), 1.4);
        }
    }
};

}

double GenieMagic::length(GenieInteraction kind)
{
    switch (kind) {
    case GenieInteraction::Poof:
    case GenieInteraction::Shimmer:
    case GenieInteraction::Whirl:
        return 2.4;
    case GenieInteraction::Fireworks:
        return 2.6;
    case GenieInteraction::Halo:
    case GenieInteraction::JewelBeam:
    case GenieInteraction::Ripple:
    case GenieInteraction::Mirage:
    case GenieInteraction::Flourish:
        return 2.8;
    case GenieInteraction::Vortex:
    case GenieInteraction::Grow:
    case GenieInteraction::Rainbow:
    case GenieInteraction::Glitter:
    case GenieInteraction::Coins:
        return 3.0;
    case GenieInteraction::Levitate:
    case GenieInteraction::Lamp:
    case GenieInteraction::SmokeRings:
    case GenieInteraction::Stardust:
    case GenieInteraction::Crystal:
    case GenieInteraction::Runes:
    case GenieInteraction::Bottle:
    case GenieInteraction::Bubbles:
    case GenieInteraction::Crescent:
    case GenieInteraction::Incense:
    case GenieInteraction::Whirlwind:
        return 3.2;
    case GenieInteraction::Wishes:
    case GenieInteraction::Carpet:
    case GenieInteraction::Fireflies:
    case GenieInteraction::Moons:
    case GenieInteraction::Constellation:
        return 3.4;
    default:
        break;
    }
    return 0;
}

GenieMagicBody GenieMagic::body(GenieInteraction kind, double age)
{
    GenieMagicBody out;
    const double a = age;
    switch (kind) {
    case GenieInteraction::Poof:
        out.fade = a < kJump ? 1 - span(a, 0.1, kJump - 0.06) : span(a, kJump + 0.05, kJump + 0.45);
        break;
    case GenieInteraction::Levitate: {
        const double e = ease(span(a, 0.1, 0.8)) * (1 - ease(span(a, 2.4, 3.1)));
        out.lift = 30 * e + 3 * std::sin(a * 4.2) * e;
        out.air = 0.35 * e;
        break;
    }
    case GenieInteraction::Carpet: {
        const double e = ease(span(a, 0.6, 1.2)) * (1 - ease(span(a, 2.4, 2.95)));
        out.lift = 22 * e + 2 * std::sin(a * 5) * e;
        break;
    }
    case GenieInteraction::Whirl:
        out.spin = ease(span(a, 0.2, 1.9)) * 4 * M_PI;
        break;
    case GenieInteraction::Grow:
        out.grow = 1 + 0.5 * (ease(span(a, 0.35, 0.95)) - ease(span(a, 2.0, 2.6)));
        break;
    case GenieInteraction::Bottle: {
        const double in = ease(span(a, 0.35, 1.15));
        const double back = span(a, 2.0, 2.45);
        if (a < 2.0) {
            out.size = 1 - 0.95 * in;
            out.twirl = 720 * in;
            out.fade = 1 - span(a, 0.85, 1.15);
        } else {
            out.size = 0.05 + 0.95 * easeOut(back) + 0.14 * std::sin(back * M_PI);
            out.twirl = -360 * (1 - easeOut(back));
            out.fade = span(a, 2.0, 2.1);
        }
        break;
    }
    case GenieInteraction::Incense:
        out.incense = ease(span(a, 0.1, 0.8)) * (1 - ease(span(a, 2.5, 3.2)));
        break;
    case GenieInteraction::Whirlwind: {
        const double u = span(a, 0.6, 2.3);
        out.lift = 10 * std::sin(M_PI * u);
        out.spin = ease(u) * 2 * M_PI;
        out.air = 0.25 * std::sin(M_PI * u);
        break;
    }
    default:
        break;
    }
    return out;
}

QRectF GenieMagic::bounds() const
{
    const double tall = side * std::max(grow, 1.0);
    QRectF box(foot.x() - tall / 2 - 70, foot.y() - tall - 70, tall + 140, tall + 70 + lift + 14);
    if (!target.isEmpty())
        box = box.united(target.adjusted(-70, -110, 70, 30));
    if (kind == GenieInteraction::Poof)
        box = box.united(QRectF(from.x() - side / 2 - 50, from.y() - side - 50, side + 100, side + 64));
    if (kind == GenieInteraction::Rainbow) {
        const QPointF jewel(foot.x(), foot.y() - side + 3 + side * 0.405);
        const QPointF peak = rainbowPeak(jewel, target.center());
        box = box.united(QRectF(peak.x() - 20, peak.y() - 24, 40, 40));
    }
    return box;
}

namespace GenieSpiderPainter {

void paintMagic(QPainter &painter, const GenieMagic &magic)
{
    if (GenieMagic::length(magic.kind) <= 0)
        return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    Magic(magic).paint(painter);
    painter.restore();
}

}
