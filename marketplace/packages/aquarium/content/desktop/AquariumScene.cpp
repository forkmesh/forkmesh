#include "AquariumScene.h"

#include <QFont>
#include <QFontMetricsF>
#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QTransform>
#include <QVarLengthArray>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace forkmesh::aquarium {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSand = 22;

double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

double ease(double u)
{
    u = clamp01(u);
    return u * u * (3 - 2 * u);
}

QPointF mix(const QPointF &a, const QPointF &b, double u)
{
    return a + (b - a) * u;
}

double span(double u, double from, double to)
{
    return clamp01((u - from) / std::max(1e-6, to - from));
}

QString shorten(QString text, int limit)
{
    text = text.simplified();
    if (text.size() > limit)
        text = text.left(limit - 1).trimmed() + QChar(0x2026);
    return text;
}

QString noun(Spot kind)
{
    switch (kind) {
    case Spot::Button:
        return QStringLiteral("a button");
    case Spot::Heading:
        return QStringLiteral("a heading");
    case Spot::Label:
        return QStringLiteral("some text");
    case Spot::Input:
        return QStringLiteral("a text box");
    case Spot::Toggle:
        return QStringLiteral("a switch");
    case Spot::Picture:
        return QStringLiteral("a picture");
    case Spot::Progress:
        return QStringLiteral("a progress bar");
    case Spot::Row:
        return QStringLiteral("a row");
    case Spot::Tab:
        return QStringLiteral("a tab");
    case Spot::Document:
        return QStringLiteral("the page of text");
    }
    return QStringLiteral("something");
}

QString quoted(const Landmark &mark)
{
    const QString text = shorten(mark.text, 28);
    if (text.isEmpty())
        return noun(mark.kind);
    return QStringLiteral("“%1”").arg(text);
}

QList<Spot> kindsFor(Trick trick)
{
    using S = Spot;
    switch (trick) {
    case Trick::Peek:
        return {S::Button, S::Tab, S::Toggle, S::Picture};
    case Trick::Boop:
    case Trick::Polish:
    case Trick::Trampoline:
        return {S::Button, S::Tab};
    case Trick::Read:
    case Trick::Lantern:
    case Trick::Highlight:
        return {S::Heading, S::Label};
    case Trick::Underline:
    case Trick::Count:
        return {S::Label, S::Heading, S::Button};
    case Trick::HideAndPeek:
        return {S::Document, S::Picture, S::Row, S::Button, S::Input};
    case Trick::Nibble:
        return {S::Label, S::Button, S::Heading};
    case Trick::Mirror:
    case Trick::Type:
        return {S::Input};
    case Trick::Checkmark:
        return {S::Toggle};
    case Trick::Surf:
        return {S::Progress, S::Row, S::Input};
    case Trick::TabHop:
        return {S::Tab, S::Button};
    case Trick::RowPatrol:
        return {S::Row};
    case Trick::Perch:
        return {S::Button, S::Heading, S::Tab};
    case Trick::Measure:
        return {S::Button, S::Heading, S::Label, S::Input, S::Progress, S::Row, S::Picture};
    case Trick::Trace:
        return {S::Button, S::Picture, S::Input, S::Progress, S::Toggle};
    case Trick::BlowKiss:
        return {S::Heading, S::Picture, S::Button};
    case Trick::Startle:
    case Trick::Shout:
        return {S::Button};
    case Trick::Fetch:
        return {S::Document, S::Label, S::Row, S::Heading};
    case Trick::BubbleRing:
        return {S::Picture, S::Toggle, S::Button, S::Tab};
    case Trick::Quote:
        return {S::Label, S::Heading, S::Row};
    case Trick::Carry:
        return {S::Toggle, S::Picture, S::Button, S::Tab};
    case Trick::Wobble:
        return {S::Button, S::Tab, S::Picture, S::Toggle};
    case Trick::Magnify:
        return {S::Label, S::Document, S::Row, S::Heading};
    case Trick::Squeegee:
        return {S::Document, S::Row, S::Picture, S::Input};
    default:
        break;
    }
    return {};
}

bool fits(Trick trick, const Landmark &mark)
{
    const QRectF &r = mark.rect;
    const bool texty = mark.text.simplified().size() >= 2;
    switch (trick) {
    case Trick::Read:
    case Trick::Underline:
    case Trick::Highlight:
        return texty && r.width() >= 30 && r.width() <= 700;
    case Trick::Count:
        return texty && mark.text.simplified().size() <= 24;
    case Trick::Quote:
    case Trick::Shout:
    case Trick::Fetch:
        return texty;
    case Trick::Magnify:
        return texty && r.width() >= 40;
    case Trick::HideAndPeek:
        return r.width() >= 60 && r.height() >= 24;
    case Trick::Surf:
    case Trick::Squeegee:
        return r.width() >= 120;
    case Trick::Measure:
        return r.width() >= 80 && r.width() <= 900;
    case Trick::Carry:
    case Trick::Wobble:
    case Trick::Trampoline:
        return r.width() <= 200 && r.height() <= 70;
    case Trick::BubbleRing:
        return r.width() <= 140 && r.height() <= 90;
    case Trick::Perch:
        return r.width() >= 36;
    default:
        break;
    }
    return true;
}

double lengthFor(Trick trick)
{
    switch (trick) {
    case Trick::Peek:
        return 3.2;
    case Trick::Boop:
        return 2.6;
    case Trick::HideAndPeek:
    case Trick::Perch:
    case Trick::DanceParty:
    case Trick::Shadow:
    case Trick::Ride:
    case Trick::Nap:
    case Trick::HideBehind:
    case Trick::PlayFetch:
        return 5;
    case Trick::Polish:
    case Trick::Mirror:
    case Trick::Fetch:
    case Trick::Pearl:
        return 3.5;
    case Trick::Nibble:
    case Trick::Shout:
    case Trick::NoseBoop:
    case Trick::HighFive:
    case Trick::Kiss:
        return 2.4;
    case Trick::Wobble:
        return 2.2;
    case Trick::Tag:
    case Trick::LegCount:
    case Trick::SilkSwing:
    case Trick::CopyDance:
        return 4.5;
    case Trick::Orbit:
    case Trick::Lantern:
    case Trick::Serenade:
    case Trick::Juggle:
    case Trick::Balloon:
        return 4;
    default:
        break;
    }
    return 3;
}

void star(QPainter &p, const QPointF &at, double outer, double inner, int points)
{
    QPainterPath path;
    for (int i = 0; i < points * 2; ++i) {
        const double a = -kPi / 2 + i * kPi / points;
        const double r = i % 2 ? inner : outer;
        const QPointF point = at + QPointF(std::cos(a), std::sin(a)) * r;
        if (i == 0)
            path.moveTo(point);
        else
            path.lineTo(point);
    }
    path.closeSubpath();
    p.drawPath(path);
}

void heart(QPainter &p, const QPointF &at, double size)
{
    QPainterPath path;
    const double s = size;
    path.moveTo(at + QPointF(0, s * 0.9));
    path.cubicTo(at + QPointF(-s * 1.4, -s * 0.1), at + QPointF(-s * 0.6, -s * 1.2),
                 at + QPointF(0, -s * 0.4));
    path.cubicTo(at + QPointF(s * 0.6, -s * 1.2), at + QPointF(s * 1.4, -s * 0.1),
                 at + QPointF(0, s * 0.9));
    p.drawPath(path);
}

void bubble(QPainter &p, const QPointF &at, double r, double alpha)
{
    p.setBrush(QColor(200, 240, 255, int(40 * alpha)));
    p.setPen(QPen(QColor(230, 250, 255, int(190 * alpha)), std::max(0.8, r * 0.18)));
    p.drawEllipse(at, r, r);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, int(200 * alpha)));
    p.drawEllipse(at + QPointF(-r * 0.35, -r * 0.35), r * 0.28, r * 0.28);
}

QPointF perimeterPoint(const QRectF &r, double u)
{
    const double total = 2 * (r.width() + r.height());
    double d = std::fmod(std::max(0.0, u), 1.0) * total;
    if (d < r.width())
        return QPointF(r.left() + d, r.top());
    d -= r.width();
    if (d < r.height())
        return QPointF(r.right(), r.top() + d);
    d -= r.height();
    if (d < r.width())
        return QPointF(r.right() - d, r.bottom());
    d -= r.width();
    return QPointF(r.left(), r.bottom() - d);
}

struct Shape {
    double a = 0.5;
    double k = 0.86;
    double b = 1.0;
    double belly = 1.0;
};

Shape shapeOf(int species)
{
    switch (species) {
    case 0:
        return {0.5, 0.86, 1.0, 1.12};
    case 1:
        return {0.45, 0.84, 0.85, 1.0};
    case 2:
        return {0.62, 0.9, 1.25, 1.0};
    case 3:
        return {0.42, 0.8, 0.55, 1.08};
    case 4:
        return {0.5, 0.8, 1.05, 1.0};
    default:
        return {0.5, 0.83, 0.9, 1.02};
    }
}

double profile(const Shape &shape, double s)
{
    s = clamp01(s);
    const double peak = shape.a / (shape.k * (shape.a + shape.b));
    const double top = std::pow(peak, shape.a) * std::pow(1 - shape.k * peak, shape.b);
    return std::pow(s, shape.a) * std::pow(1 - shape.k * s, shape.b) / top;
}

double bodyHeight(int species, double L, double puffed)
{
    switch (species) {
    case 1:
        return L * 0.5;
    case 2:
        return L * 0.8;
    case 3:
        return L * (0.62 + 0.25 * puffed);
    case 4:
        return L * 0.38;
    case 5:
        return L * 0.42;
    default:
        return L * 0.55;
    }
}

double bodyWidth(int species, double L)
{
    return species == 3 ? L * 0.68 : L * 0.78;
}

QPainterPath outline(const Shape &shape, double w, double h)
{
    constexpr int n = 22;
    QPolygonF loop;
    for (int i = 0; i <= n; ++i) {
        const double s = double(i) / n;
        loop << QPointF(w / 2 - s * w, -profile(shape, s) * h / 2);
    }
    for (int i = n; i >= 0; --i) {
        const double s = double(i) / n;
        loop << QPointF(w / 2 - s * w, profile(shape, s) * h / 2 * shape.belly);
    }
    QPainterPath path(loop.first());
    for (int i = 1; i + 1 < loop.size(); ++i)
        path.quadTo(loop.at(i), (loop.at(i) + loop.at(i + 1)) / 2);
    path.lineTo(loop.last());
    path.closeSubpath();
    return path;
}

QColor muted(QColor color, double fade, int dark)
{
    float h = 0;
    float s = 0;
    float v = 0;
    float a = 0;
    color.getHsvF(&h, &s, &v, &a);
    color.setHsvF(std::max(0.0f, h), float(s * (1 - fade)), v, a);
    return dark > 100 ? color.darker(dark) : color;
}

QColor blend(const QColor &a, const QColor &b, double u)
{
    return QColor::fromRgbF(float(a.redF() + (b.redF() - a.redF()) * u),
                            float(a.greenF() + (b.greenF() - a.greenF()) * u),
                            float(a.blueF() + (b.blueF() - a.blueF()) * u),
                            float(a.alphaF() + (b.alphaF() - a.alphaF()) * u));
}

QColor alpha(QColor color, int value)
{
    color.setAlpha(value);
    return color;
}

QPainterPath band(double x, double half, double h, double sweep)
{
    QPainterPath path;
    path.moveTo(x - half, -h);
    path.quadTo(x - half - sweep, 0, x - half, h);
    path.lineTo(x + half, h);
    path.quadTo(x + half - sweep, 0, x + half, -h);
    path.closeSubpath();
    return path;
}

QPointF quadPoint(const QPointF &a, const QPointF &c, const QPointF &b, double t)
{
    return a * ((1 - t) * (1 - t)) + c * (2 * (1 - t) * t) + b * (t * t);
}

void spines(int species, double W, double H, double grow, QPainterPath *membranes, QVarLengthArray<QLineF, 40> *rays)
{
    const Shape shape = shapeOf(species);
    const auto x = [&](double s) { return W / 2 - s * W; };
    const auto edge = [&](double s, bool top) {
        const double f = profile(shape, std::min(1.0, s)) * H / 2 * (top ? 1 : shape.belly);
        return QPointF(x(s), top ? -f : f);
    };
    const auto paired = [&](double s0, double s1, double tipS, double h, bool top) {
        h *= grow;
        const QPointF base0 = edge(s0, top);
        const QPointF base1 = edge(s1, top);
        const double sign = top ? -1 : 1;
        const QPointF tip(x(tipS), edge(tipS, top).y() + sign * h);
        const QPointF lead(x(s0 + (tipS - s0) * 0.25), base0.y() + sign * h * 0.9);
        const QPointF trail(x((tipS + s1) / 2), tip.y() + (base1.y() - tip.y()) * 0.6);
        membranes->moveTo(base0);
        membranes->quadTo(lead, tip);
        membranes->quadTo(trail, base1);
        membranes->lineTo(edge((s0 + s1) / 2, top) - QPointF(0, sign * H * 0.08));
        membranes->closeSubpath();
        for (int i = 0; i < 4; ++i) {
            const double t = (i + 1) / 5.0;
            rays->append(QLineF(base0 + (base1 - base0) * (t * 0.9), quadPoint(base0, lead, tip, 0.35 + 0.65 * t)));
        }
    };
    membranes->setFillRule(Qt::WindingFill);
    switch (species) {
    case 0:
        paired(0.28, 0.62, 0.6, H * 0.42, true);
        paired(0.66, 0.86, 0.9, H * 0.3, false);
        break;
    case 1:
        paired(0.3, 0.72, 0.62, H * 0.32, true);
        paired(0.62, 0.86, 0.86, H * 0.3, false);
        break;
    case 2:
        paired(0.3, 0.72, 0.92, H * 0.42, true);
        paired(0.4, 0.76, 0.92, H * 0.38, false);
        break;
    case 3:
        paired(0.68, 0.84, 0.88, H * 0.25, true);
        paired(0.68, 0.84, 0.88, H * 0.22, false);
        break;
    case 4:
        paired(0.38, 0.56, 0.58, H * 0.45, true);
        paired(0.6, 0.88, 0.92, H * 0.32, false);
        paired(0.84, 0.9, 0.92, H * 0.12, true);
        break;
    default:
        paired(0.45, 0.92, 1.02, H * 0.55, true);
        paired(0.3, 0.96, 1.08, H * 0.75, false);
        break;
    }
    paired(0.38, 0.46, 0.52, H * 0.22, false);
}

QPainterPath caudalOf(int species, double L, double H, double grow, QVarLengthArray<QLineF, 8> *rays)
{
    double tl = L * 0.32;
    double th = H * 0.7;
    double fork = 0.2;
    switch (species) {
    case 0:
        tl = L * 0.42;
        th = H * 0.75;
        fork = 0.38;
        break;
    case 1:
        tl = L * 0.28;
        th = H * 0.6;
        fork = -0.2;
        break;
    case 2:
        tl = L * 0.3;
        th = H * 0.6;
        fork = 0.25;
        break;
    case 3:
        tl = L * 0.22;
        th = H * 0.5;
        fork = -0.2;
        break;
    case 4:
        tl = L * 0.3;
        th = H * 0.8;
        fork = 0.5;
        break;
    default:
        tl = L * 0.6;
        th = H * 1.5;
        fork = -0.3;
        break;
    }
    tl *= grow;
    th *= grow;
    const double ped = profile(shapeOf(species), 1) * H / 2;
    const QPointF up(-tl, -th / 2);
    const QPointF down(-tl, th / 2);
    const QPointF notch(-tl * (1 - fork), 0);
    QPainterPath caudal(QPointF(2, -ped));
    caudal.quadTo(QPointF(-tl * 0.45, -th * 0.3), up);
    caudal.quadTo(notch, down);
    caudal.quadTo(QPointF(-tl * 0.45, th * 0.3), QPointF(2, ped));
    caudal.closeSubpath();
    for (int i = 0; i < 6; ++i) {
        const double t = 0.06 + 0.88 * i / 5.0;
        rays->append(QLineF(QPointF(0, -ped + 2 * ped * t), quadPoint(up, notch, down, t)));
    }
    return caudal;
}

double flex(double tail, double amp, double s)
{
    return amp * s * s * std::sin(tail - s * 2.6);
}

} // namespace

Scene::Scene(quint32 seed)
    : m_random(seed ? seed : QRandomGenerator::global()->generate())
{
}

double Scene::rnd(double lo, double hi)
{
    return lo + m_random.generateDouble() * (hi - lo);
}

void Scene::setSize(const QSizeF &size)
{
    if (size == m_size)
        return;
    m_size = size;
    buildComposite();
}

void Scene::setPixelRatio(qreal ratio)
{
    ratio = std::clamp(ratio, 1.0, 4.0);
    if (qFuzzyCompare(ratio, m_ratio))
        return;
    m_ratio = ratio;
    for (Fish &fish : m_fish)
        dress(fish);
}

void Scene::setBackdrop(const QImage &image)
{
    m_backdrop = image;
    buildComposite();
}

void Scene::setLandmarks(const QList<Landmark> &landmarks)
{
    m_landmarks = landmarks;
}

QRectF Scene::tank() const
{
    return QRectF(24, 40, std::max(10.0, m_size.width() - 48), std::max(10.0, floorY() - 56));
}

double Scene::floorY() const
{
    return m_size.height() - kSand;
}

void Scene::spawn(const Swimmer &swimmer)
{
    Fish fish;
    fish.id = swimmer.id;
    fish.name = swimmer.name.isEmpty() ? QStringLiteral("Agent") : swimmer.name;
    fish.hash = qHash(fish.name) ^ (qHash(swimmer.kind) * 2654435761u);
    fish.mood = swimmer.mood;
    fish.files = swimmer.files;
    fish.lines = swimmer.lines;
    fish.conflicted = swimmer.conflicted;
    fish.work = swimmer.work;
    dress(fish);
    const QRectF area = tank();
    fish.pos = QPointF(rnd(area.left() + 40, area.right() - 40), -40);
    fish.goal = QPointF(fish.pos.x(), rnd(area.top() + 40, area.center().y()));
    fish.vel = QPointF(0, 80);
    fish.nextTrick = m_time + rnd(1.5, 4) * (fish.mood == Mood::Working ? 1 : 3);
    fish.nextMurmur = m_time + rnd(3, 9);
    fish.tail = rnd(0, 6);
    for (int i = 0; i < 4; ++i)
        puff(Puff::Ring, QPointF(fish.pos.x(), 4), 0.9, 6 + 6 * i).rect =
            QRectF(fish.pos.x() - 8, 0, 16, 6);
    m_fish.append(fish);
}

void Scene::dress(Fish &fish) const
{
    const quint32 hash = fish.hash;
    fish.species = int(hash % 6);
    const int hue = int((hash >> 4) % 360);
    double length = 52;
    switch (fish.species) {
    case 0:
        fish.body = QColor::fromHsv(20 + hue % 30, 220, 250);
        fish.fin = fish.body.lighter(115);
        fish.accent = QColor(255, 236, 170);
        fish.iris = QColor(235, 180, 60);
        length = 54;
        break;
    case 1:
        fish.body = QColor(255, 128, 32);
        fish.fin = QColor(255, 150, 60);
        fish.accent = QColor(255, 255, 255);
        fish.iris = QColor(250, 120, 30);
        length = 50;
        break;
    case 2:
        fish.body = QColor::fromHsv(hue, 60, 235);
        fish.fin = QColor::fromHsv(hue, 90, 220);
        fish.accent = QColor(50, 50, 70);
        fish.iris = QColor(200, 40, 40);
        length = 52;
        break;
    case 3:
        fish.body = QColor::fromHsv(48 + hue % 20, 150, 245);
        fish.fin = QColor::fromHsv(40, 120, 230);
        fish.accent = QColor(110, 80, 40);
        fish.iris = QColor(60, 170, 110);
        length = 48;
        break;
    case 4:
        fish.body = QColor::fromHsv(200 + hue % 30, 70, 230);
        fish.fin = QColor(230, 240, 255);
        fish.accent = QColor(40, 230, 255);
        fish.iris = QColor(150, 200, 230);
        length = 46;
        break;
    default:
        fish.body = QColor::fromHsv(hue, 210, 230);
        fish.fin = QColor::fromHsv((hue + 30) % 360, 190, 245);
        fish.accent = QColor::fromHsv((hue + 180) % 360, 120, 255);
        fish.iris = QColor(40, 40, 60);
        length = 52;
        break;
    }
    fish.length = length * std::min(1.45, 1 + 0.09 * std::log2(1.0 + std::max(0, fish.files)));
    fish.finScale = 1 + std::min(0.4, 0.1 * std::log10(1.0 + std::max(0, fish.lines)));
    double fade = 0;
    int dark = 100;
    if (fish.mood == Mood::Failed) {
        fade = 0.7;
        dark = 118;
    } else if (fish.mood == Mood::Stopped) {
        fade = 0.5;
        dark = 112;
    } else if (fish.mood == Mood::Queued) {
        fade = 0.3;
    }
    fish.body = muted(fish.body, fade, dark);
    fish.fin = muted(fish.fin, fade, dark);
    fish.accent = muted(fish.accent, fade, dark);
    fish.presence = fish.mood == Mood::Queued ? 0.72 : 1;

    const Shape shape = shapeOf(fish.species);
    const double L = fish.length;
    const double H = bodyHeight(fish.species, L, 0);
    const double W = bodyWidth(fish.species, L);
    const QPainterPath body = outline(shape, W, H);
    QPainterPath membranes;
    QVarLengthArray<QLineF, 40> rays;
    spines(fish.species, W, H, fish.finScale, &membranes, &rays);
    const double margin = fish.conflicted ? H * 0.18 : 1.5;
    fish.skinRect = body.boundingRect().adjusted(-margin, -margin, margin, margin).united(membranes.boundingRect().adjusted(-1.5, -1.5, 1.5, 1.5));
    const qreal ratio = m_ratio;
    fish.skin = QImage(QSize(int(std::ceil(fish.skinRect.width() * ratio)), int(std::ceil(fish.skinRect.height() * ratio))),
                       QImage::Format_ARGB32_Premultiplied);
    fish.skin.setDevicePixelRatio(ratio);
    fish.skin.fill(Qt::transparent);
    QPainter p(&fish.skin);
    p.setRenderHint(QPainter::Antialiasing);
    p.translate(-fish.skinRect.topLeft());
    p.setPen(Qt::NoPen);

    p.setBrush(alpha(fish.fin, fish.species == 5 ? 205 : 225));
    p.drawPath(membranes);
    p.setPen(QPen(alpha(fish.fin.darker(140), 90), 0.6));
    p.drawLines(rays.constData(), int(rays.size()));
    p.setPen(Qt::NoPen);

    {
        QVarLengthArray<QLineF, 8> tailRays;
        const QPainterPath caudal = caudalOf(fish.species, L, H, fish.finScale, &tailRays);
        fish.tailRect = caudal.boundingRect().adjusted(-1.5, -1.5, 1.5, 1.5);
        fish.tailSkin = QImage(QSize(int(std::ceil(fish.tailRect.width() * ratio)), int(std::ceil(fish.tailRect.height() * ratio))),
                               QImage::Format_ARGB32_Premultiplied);
        fish.tailSkin.setDevicePixelRatio(ratio);
        fish.tailSkin.fill(Qt::transparent);
        QPainter t(&fish.tailSkin);
        t.setRenderHint(QPainter::Antialiasing);
        t.translate(-fish.tailRect.topLeft());
        t.setPen(Qt::NoPen);
        t.setBrush(alpha(fish.fin, fish.species == 5 ? 195 : 220));
        t.drawPath(caudal);
        t.save();
        t.scale(0.5, 0.6);
        t.setBrush(alpha(fish.fin.darker(112), 120));
        t.drawPath(caudal);
        t.restore();
        t.setPen(QPen(alpha(fish.fin.darker(140), 90), 0.6));
        t.drawLines(tailRays.constData(), int(tailRays.size()));
    }

    const QColor belly = blend(fish.body, QColor(255, 250, 240), fish.species == 4 ? 0.7 : 0.5);
    QLinearGradient shade(0, -H / 2, 0, H / 2 * shape.belly);
    shade.setColorAt(0, fish.body.darker(160));
    shade.setColorAt(0.3, fish.body.darker(108));
    shade.setColorAt(0.55, fish.body);
    shade.setColorAt(0.8, fish.body.lighter(116));
    shade.setColorAt(1, belly);
    p.setBrush(shade);
    p.drawPath(body);

    p.save();
    p.setClipPath(body);
    QRandomGenerator dots(hash);
    switch (fish.species) {
    case 0:
        if ((hash >> 12) % 3 == 0) {
            p.setBrush(QColor(255, 250, 240, 210));
            for (int i = 0; i < 3; ++i)
                p.drawEllipse(QPointF(W * (0.3 - 0.25 * i) + dots.bounded(6), -H * 0.1 + dots.bounded(int(H * 0.4))),
                              W * 0.12, H * 0.16);
        }
        p.setBrush(QColor(255, 240, 180, 60));
        p.drawEllipse(QPointF(-W * 0.05, -H * 0.05), W * 0.3, H * 0.14);
        break;
    case 1:
        for (const double x : {W * 0.24, -W * 0.06, -W * 0.36}) {
            p.setBrush(QColor(20, 20, 24));
            p.drawPath(band(x, L * 0.075, H, H * 0.12));
            p.setBrush(fish.accent);
            p.drawPath(band(x, L * 0.05, H, H * 0.12));
        }
        break;
    case 2:
        p.setBrush(alpha(fish.accent, 150));
        for (const double x : {W * 0.2, -W * 0.08, -W * 0.34})
            p.drawPath(band(x, L * 0.035, H, H * 0.08));
        p.setBrush(alpha(fish.accent, 50));
        p.drawPath(band(W * 0.06, L * 0.012, H, H * 0.08));
        p.drawPath(band(-W * 0.22, L * 0.012, H, H * 0.08));
        break;
    case 3:
        p.setBrush(fish.accent);
        for (int i = 0; i < 14; ++i)
            p.drawEllipse(QPointF(W * (0.25 - dots.generateDouble() * 0.7), -H * (0.1 + dots.generateDouble() * 0.35)),
                          1.3 + dots.generateDouble(), 1.3 + dots.generateDouble());
        p.setBrush(QColor(255, 255, 240, 170));
        p.drawEllipse(QPointF(0, H * 0.32), W * 0.42, H * 0.2);
        break;
    case 4: {
        QLinearGradient neon(0, -H * 0.16, 0, H * 0.04);
        neon.setColorAt(0, alpha(fish.accent, 0));
        neon.setColorAt(0.5, fish.accent);
        neon.setColorAt(1, alpha(fish.accent, 0));
        p.setBrush(neon);
        p.drawRect(QRectF(-W * 0.5, -H * 0.16, W * 0.85, H * 0.2));
        p.setBrush(muted(QColor(240, 60, 70, 210), fade, dark));
        p.drawRect(QRectF(-W * 0.55, H * 0.06, W * 0.6, H * 0.45));
        break;
    }
    default: {
        QLinearGradient sheen(-W / 2, -H / 2, W / 2, H / 2);
        sheen.setColorAt(0, alpha(fish.fin, 120));
        sheen.setColorAt(0.5, alpha(fish.accent, 40));
        sheen.setColorAt(1, alpha(fish.fin, 90));
        p.setBrush(sheen);
        p.drawPath(body);
        break;
    }
    }

    const double sc = std::max(2.6, L * 0.065);
    p.setBrush(Qt::NoBrush);
    const QPen scaleDark(alpha(fish.body.darker(150), 60), 0.6);
    const QPen scaleLight(QColor(255, 255, 255, 38), 0.5);
    int row = 0;
    for (double y = -H / 2; y <= H / 2 * shape.belly; y += sc * 0.62, ++row) {
        for (double x = -W / 2 + (row % 2) * sc / 2; x < W * 0.2; x += sc) {
            const QRectF cell(x - sc / 2, y - sc / 2, sc, sc);
            p.setPen(scaleDark);
            p.drawArc(cell, 100 * 16, 160 * 16);
            p.setPen(scaleLight);
            p.drawArc(cell.translated(0.6, 0), 110 * 16, 140 * 16);
        }
    }

    QPainterPath lateral;
    for (int i = 0; i <= 12; ++i) {
        const double s = 0.3 + 0.65 * i / 12;
        const QPointF at(W / 2 - s * W, -profile(shape, s) * H / 2 * 0.3);
        if (i == 0)
            lateral.moveTo(at);
        else
            lateral.lineTo(at);
    }
    QPen lateralPen(alpha(fish.body.darker(165), 110), 0.7);
    lateralPen.setDashPattern({2.0, 2.0});
    p.setPen(lateralPen);
    p.drawPath(lateral);

    const int freckles = std::min(16, std::max(0, fish.files));
    const int columns = (freckles + 1) / 2;
    for (int i = 0; i < freckles; ++i) {
        const int column = i / 2;
        const double s = 0.36 + (columns > 1 ? double(column) / (columns - 1) : 0.5) * 0.48;
        const double f = profile(shape, s) * H / 2;
        const QPointF at(W / 2 - s * W, i % 2 ? f * 0.42 : -f * 0.12);
        const double r = std::max(1.1, L * 0.022);
        p.setPen(QPen(alpha(fish.body.darker(170), 140), 0.6));
        p.setBrush(blend(fish.body, QColor(255, 255, 245), 0.75));
        p.drawEllipse(at, r, r);
    }

    const double gs = 0.27;
    const double gx = W / 2 - gs * W;
    const double gf = profile(shape, gs) * H / 2;
    QPainterPath gill;
    gill.moveTo(gx + W * 0.02, -gf * 0.8);
    gill.quadTo(gx - W * 0.07, gf * 0.1, gx + W * 0.01, gf * 0.85 * shape.belly);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(alpha(fish.body.darker(175), 150), 1.0, Qt::SolidLine, Qt::RoundCap));
    p.drawPath(gill);
    p.setPen(QPen(QColor(255, 255, 255, 50), 0.8, Qt::SolidLine, Qt::RoundCap));
    p.drawPath(gill.translated(-1.0, 0));

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(255, 255, 255, 60));
    p.drawEllipse(QPointF(W * 0.08, -H * 0.24), W * 0.28, H * 0.08);

    if (fish.mood == Mood::Failed) {
        p.save();
        p.translate(-W * 0.06, -H * 0.12);
        for (const double turn : {35.0, -35.0}) {
            p.save();
            p.rotate(turn);
            p.setBrush(QColor(236, 206, 166));
            p.drawRoundedRect(QRectF(-W * 0.15, -H * 0.075, W * 0.3, H * 0.15), 2, 2);
            p.setBrush(QColor(214, 180, 140));
            p.drawRect(QRectF(-W * 0.04, -H * 0.075, W * 0.08, H * 0.15));
            p.restore();
        }
        p.restore();
    }
    p.restore();

    if (fish.mood == Mood::Merged) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(205, 160, 255, 230), 1.6));
        p.drawPath(body);
    } else {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(alpha(fish.body.darker(180), 150), 0.8));
        p.drawPath(body);
    }

    if (fish.conflicted) {
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(QColor(230, 50, 64), 1.3, Qt::SolidLine, Qt::RoundCap));
        for (const double turn : {24.0, -18.0, 60.0}) {
            p.save();
            p.translate(-W * 0.06, H * 0.02);
            p.rotate(turn);
            p.drawEllipse(QPointF(), W * 0.15, H * 0.6);
            p.restore();
        }
        p.drawLine(QPointF(-W * 0.2, H * 0.45), QPointF(-W * 0.3, H * 0.62));
    }
}

bool Scene::playful(const Fish &fish)
{
    return fish.mood == Mood::Working || fish.mood == Mood::Done || fish.mood == Mood::Merged;
}

void Scene::moodChanged(Fish &fish, Mood was, bool wasConflicted)
{
    if (fish.mood != was) {
        if (fish.act && !playful(fish) && fish.mood != Mood::Waiting)
            finish(fish);
        fish.sleepy = false;
        fish.roll = 0;
        fish.wag = 1;
        fish.wanderUntil = 0;
        switch (fish.mood) {
        case Mood::Working:
            say(fish, was == Mood::Queued ? QStringLiteral("My turn!") : QStringLiteral("Back to work!"), 1.8);
            break;
        case Mood::Done:
            say(fish, QStringLiteral("Done!"), 1.8);
            bubbles(mouthOf(fish), 5, 4);
            break;
        case Mood::Merged:
            say(fish, QStringLiteral("Merged!"), 2);
            for (int i = 0; i < 8; ++i)
                puff(Puff::Sparkle, fish.pos + QPointF(std::cos(i * kPi / 4), std::sin(i * kPi / 4)) * fish.length * 0.6, 0.9, 5)
                    .color = QColor(215, 175, 255);
            break;
        case Mood::Failed:
            say(fish, QStringLiteral("Oh no…"), 2);
            break;
        case Mood::Stopped:
            say(fish, QStringLiteral("Time for a nap"), 2);
            break;
        default:
            break;
        }
    }
    if (fish.conflicted && !wasConflicted)
        say(fish, QStringLiteral("Merge conflict!"), 2.2);
    else if (!fish.conflicted && wasConflicted)
        say(fish, QStringLiteral("Untangled!"), 1.8);
}

QPointF Scene::lineTop(const Fish &fish) const
{
    return QPointF(fish.pos.x() - fish.heading * 26 + std::sin(m_time * 0.7 + fish.id) * 10, 0);
}

bool Scene::shadowed(const Fish &fish) const
{
    return !m_size.isEmpty() && !fish.leaving && fish.pos.y() > floorY() - 90 && fish.pos.y() < floorY();
}

void Scene::setSwimmers(const QList<Swimmer> &swimmers)
{
    QHash<int, Swimmer> wanted;
    for (const Swimmer &swimmer : swimmers)
        wanted.insert(swimmer.id, swimmer);
    for (Fish &fish : m_fish) {
        const auto found = wanted.constFind(fish.id);
        if (found == wanted.constEnd()) {
            if (!fish.leaving) {
                fish.leaving = true;
                fish.act.reset();
                fish.sleepy = false;
                fish.roll = 0;
                say(fish, QStringLiteral("Bye!"), 1.6);
                if (m_petFish == fish.id)
                    m_petFish = -1;
            }
            continue;
        }
        fish.leaving = false;
        if (found->work > fish.work)
            bubbles(mouthOf(fish), 3, 3);
        fish.work = found->work;
        if (!found->name.isEmpty())
            fish.name = found->name;
        const Mood was = fish.mood;
        const bool wasConflicted = fish.conflicted;
        if (was != found->mood || fish.files != found->files || fish.lines != found->lines ||
            wasConflicted != found->conflicted) {
            fish.mood = found->mood;
            fish.files = found->files;
            fish.lines = found->lines;
            fish.conflicted = found->conflicted;
            dress(fish);
            moodChanged(fish, was, wasConflicted);
        }
        wanted.remove(fish.id);
    }
    for (const Swimmer &swimmer : swimmers) {
        if (wanted.contains(swimmer.id))
            spawn(swimmer);
    }
}

void Scene::setPet(const Pet &pet)
{
    m_pet = pet;
    if (!pet.present && m_petFish >= 0) {
        for (Fish &fish : m_fish) {
            if (fish.id == m_petFish && fish.act && petTrick(fish.act->trick))
                finish(fish);
        }
        m_petFish = -1;
    }
}

QList<PetCue> Scene::takeCues()
{
    QList<PetCue> cues;
    cues.swap(m_cues);
    return cues;
}

QList<Note> Scene::takeNotes()
{
    QList<Note> notes;
    notes.swap(m_notes);
    return notes;
}

QList<Scene::FishInfo> Scene::fish() const
{
    QList<FishInfo> list;
    for (const Fish &fish : m_fish) {
        FishInfo info;
        info.id = fish.id;
        info.name = fish.name;
        info.pos = fish.pos;
        info.busy = fish.act.has_value();
        info.leaving = fish.leaving;
        if (fish.act)
            info.trick = fish.act->trick;
        info.mood = fish.mood;
        info.conflicted = fish.conflicted;
        info.length = fish.length;
        list.append(info);
    }
    return list;
}

int Scene::fishAt(const QPointF &point) const
{
    const auto hits = [&](const Fish &fish) {
        if (fish.leaving || clamp01(fish.alpha) * fish.presence < 0.2)
            return false;
        const double rx = std::max(14.0, bodyWidth(fish.species, fish.length) * fish.scale * 0.65 + 6);
        const double ry = std::max(12.0, bodyHeight(fish.species, fish.length, fish.puffed) * fish.scale * 0.65 + 6);
        const QPointF d = point - fish.pos;
        if ((d.x() * d.x()) / (rx * rx) + (d.y() * d.y()) / (ry * ry) <= 1)
            return true;
        if (fish.tag.isNull())
            return false;
        const QSizeF tag = fish.tag.deviceIndependentSize();
        return QRectF(fish.pos.x() - tag.width() / 2, fish.pos.y() + fish.length * 0.42 + 4, tag.width(), tag.height())
            .contains(point);
    };
    for (bool behind : {false, true}) {
        for (auto it = m_fish.crbegin(); it != m_fish.crend(); ++it) {
            if (it->behind == behind && hits(*it))
                return it->id;
        }
    }
    return 0;
}

void Scene::cue(Cue cue, const QPointF &at)
{
    if (m_pet.present)
        m_cues.append({cue, at});
}

Scene::Particle &Scene::puff(Puff kind, const QPointF &at, double life, double size)
{
    Particle particle;
    particle.kind = kind;
    particle.pos = at;
    particle.life = life;
    particle.size = size;
    particle.color = Qt::white;
    m_particles.append(particle);
    if (m_particles.size() > 600)
        m_particles.removeFirst();
    return m_particles.last();
}

void Scene::bubbles(const QPointF &at, int count, double spread)
{
    for (int i = 0; i < count; ++i) {
        Particle &p = puff(Puff::Bubble, at + QPointF(rnd(-spread, spread), rnd(-spread, spread)),
                           rnd(1.6, 3.2), rnd(1.8, 4.2));
        p.vel = QPointF(rnd(-12, 12), rnd(-40, -10));
        p.rise = rnd(30, 60);
    }
}

void Scene::say(Fish &fish, const QString &text, double seconds)
{
    fish.say = text;
    fish.sayUntil = m_time + seconds;
}

QPointF Scene::mouthOf(const Fish &fish) const
{
    const double sign = fish.facing < 0 ? -1 : 1;
    return fish.pos + QPointF(sign * fish.length * 0.42 * fish.scale, fish.length * 0.04);
}

QPointF Scene::petHead() const
{
    return QPointF(m_pet.body.center().x(), m_pet.body.top() + m_pet.body.height() * 0.3);
}

QPointF Scene::petFoot() const
{
    return QPointF(m_pet.body.center().x(), m_pet.body.bottom() - 3);
}

bool Scene::once(Act &act, int bit)
{
    const unsigned mask = 1u << bit;
    if (act.fired & mask)
        return false;
    act.fired |= mask;
    return true;
}

QImage Scene::slice(const QRectF &rect) const
{
    if (m_backdrop.isNull())
        return QImage();
    const qreal ratio = m_backdrop.devicePixelRatio();
    const QRect source = QRectF(rect.topLeft() * ratio, rect.size() * ratio).toAlignedRect();
    QImage piece = m_backdrop.copy(source.intersected(m_backdrop.rect()));
    piece.setDevicePixelRatio(ratio);
    return piece;
}

bool Scene::landmarkFor(Trick trick, Landmark *mark, QList<Landmark> *marks) const
{
    if (petTrick(trick))
        return false;
    const QRectF area = QRectF(QPointF(0, 0), m_size).adjusted(10, 10, -10, -10);
    const QList<Spot> kinds = kindsFor(trick);
    QList<int> candidates;
    for (int i = 0; i < m_landmarks.size(); ++i) {
        const Landmark &landmark = m_landmarks.at(i);
        if (!kinds.contains(landmark.kind) || !area.contains(landmark.rect.center()) ||
            landmark.rect.width() > m_size.width() * 0.95 ||
            landmark.rect.height() > m_size.height() * 0.6 || !fits(trick, landmark))
            continue;
        if (m_pet.present && landmark.rect.intersects(m_pet.frame))
            continue;
        candidates.append(i);
    }
    if (trick == Trick::TabHop || trick == Trick::RowPatrol) {
        QList<QList<Landmark>> groups;
        for (int index : candidates) {
            const Landmark &landmark = m_landmarks.at(index);
            if (trick == Trick::TabHop && landmark.rect.width() > 420)
                continue;
            bool placed = false;
            for (QList<Landmark> &group : groups) {
                const QRectF &first = group.first().rect;
                const bool same =
                    trick == Trick::TabHop
                        ? group.first().kind == landmark.kind &&
                              std::abs(first.center().y() - landmark.rect.center().y()) < 6
                        : std::abs(first.left() - landmark.rect.left()) < 6;
                if (same) {
                    group.append(landmark);
                    placed = true;
                    break;
                }
            }
            if (!placed)
                groups.append({landmark});
        }
        const int least = trick == Trick::TabHop ? 3 : 2;
        QList<QList<Landmark>> good;
        for (QList<Landmark> group : std::as_const(groups)) {
            if (group.size() < least)
                continue;
            if (trick == Trick::TabHop && group.first().kind == Spot::Tab)
                good.prepend(group);
            else
                good.append(group);
        }
        if (good.isEmpty())
            return false;
        if (marks) {
            QList<Landmark> group =
                good.at(trick == Trick::TabHop && good.first().first().kind == Spot::Tab
                            ? 0
                            : int(m_random.bounded(int(good.size()))));
            std::sort(group.begin(), group.end(), [trick](const Landmark &a, const Landmark &b) {
                return trick == Trick::TabHop ? a.rect.left() < b.rect.left()
                                              : a.rect.top() < b.rect.top();
            });
            *marks = group.mid(0, 6);
            if (mark)
                *mark = marks->first();
        }
        return true;
    }
    if (candidates.isEmpty())
        return false;
    if (mark) {
        QList<int> preferred;
        for (int index : std::as_const(candidates)) {
            if (m_landmarks.at(index).kind == kinds.first())
                preferred.append(index);
        }
        const QList<int> &pool =
            preferred.isEmpty() || m_random.generateDouble() < 0.3
                ? candidates
                : preferred;
        *mark = m_landmarks.at(pool.at(int(m_random.bounded(int(pool.size())))));
    }
    return true;
}

bool Scene::viable(Trick trick) const
{
    if (petTrick(trick))
        return m_pet.present;
    return landmarkFor(trick, nullptr, nullptr);
}

bool Scene::perform(int fishId, Trick trick)
{
    for (Fish &fish : m_fish) {
        if (fish.id != fishId || fish.leaving)
            continue;
        if (fish.act)
            finish(fish);
        if (petTrick(trick)) {
            if (!m_pet.present)
                return false;
            if (m_petFish >= 0 && m_petFish != fish.id) {
                for (Fish &other : m_fish) {
                    if (other.id == m_petFish && other.act)
                        finish(other);
                }
            }
            begin(fish, trick, Landmark(), {});
            return true;
        }
        Landmark mark;
        QList<Landmark> marks;
        if (!landmarkFor(trick, &mark, &marks))
            return false;
        begin(fish, trick, mark, marks);
        return true;
    }
    return false;
}

void Scene::begin(Fish &fish, Trick trick, const Landmark &mark, const QList<Landmark> &marks)
{
    Act act;
    act.trick = trick;
    act.mark = mark;
    act.marks = marks;
    act.length = lengthFor(trick);
    act.seed = rnd(0, 1);
    if (petTrick(trick)) {
        act.side = fish.pos.x() < petHead().x() ? -1 : 1;
        m_petFish = fish.id;
    } else {
        act.side = fish.pos.x() < mark.rect.center().x() ? -1 : 1;
    }
    switch (trick) {
    case Trick::Read:
        act.length = std::clamp(mark.rect.width() / 60.0, 2.5, 6.0);
        break;
    case Trick::Underline:
    case Trick::Highlight:
        act.length = std::clamp(mark.rect.width() / 85.0, 2.0, 5.0);
        break;
    case Trick::Surf:
    case Trick::Magnify:
        act.length = std::clamp(mark.rect.width() / 110.0, 2.5, 5.0);
        break;
    case Trick::Squeegee:
        act.length = std::clamp(mark.rect.width() / 120.0, 2.0, 4.5);
        break;
    case Trick::Trace:
        act.length = std::clamp(2 * (mark.rect.width() + mark.rect.height()) / 240.0, 2.5, 6.0);
        break;
    case Trick::Count:
        act.length = std::clamp(mark.text.simplified().size() * 0.28, 2.0, 6.5);
        break;
    case Trick::TabHop:
    case Trick::RowPatrol:
        act.length = std::max(2.0, 0.85 * marks.size());
        break;
    case Trick::Carry:
    case Trick::Wobble:
    case Trick::Trampoline:
        act.piece = slice(mark.rect);
        break;
    default:
        break;
    }
    fish.act = act;
    fish.wanderUntil = 0;
}

bool Scene::pick(Fish &fish)
{
    const bool petOk = m_pet.present && m_petFish < 0 && m_time >= m_petFree;
    const bool petFirst = petOk && m_random.generateDouble() < 0.45;
    for (int round = 0; round < 2; ++round) {
        const bool pet = (round == 0) == petFirst;
        if (pet) {
            if (!petOk)
                continue;
            if (m_petBag.isEmpty()) {
                for (int i = 0; i < kPetTricks; ++i)
                    m_petBag.append(kElementTricks + i);
                std::shuffle(m_petBag.begin(), m_petBag.end(), m_random);
            }
            begin(fish, Trick(m_petBag.takeFirst()), Landmark(), {});
            return true;
        }
        if (m_elementBag.isEmpty()) {
            for (int i = 0; i < kElementTricks; ++i)
                m_elementBag.append(i);
            std::shuffle(m_elementBag.begin(), m_elementBag.end(), m_random);
        }
        for (int i = 0; i < m_elementBag.size(); ++i) {
            const Trick trick = Trick(m_elementBag.at(i));
            Landmark mark;
            QList<Landmark> marks;
            if (!landmarkFor(trick, &mark, &marks))
                continue;
            m_elementBag.removeAt(i);
            begin(fish, trick, mark, marks);
            return true;
        }
    }
    return false;
}

QPointF Scene::approachPoint(const Fish &fish) const
{
    const Act &act = *fish.act;
    const QRectF r = act.mark.rect;
    const double L = fish.length;
    const double side = act.side;
    const QPointF head = petHead();
    const QRectF body = m_pet.body;
    const QPointF beside(side < 0 ? r.left() - L * 0.6 : r.right() + L * 0.6, r.center().y());
    switch (act.trick) {
    case Trick::Read:
        return QPointF(r.left(), r.top() - 10 - L * 0.15);
    case Trick::Underline:
        return QPointF(r.left(), r.bottom() + 8);
    case Trick::Highlight:
    case Trick::Squeegee:
        return QPointF(r.left(), r.center().y());
    case Trick::Measure:
        return QPointF(r.left(), r.top() - 14);
    case Trick::Surf:
        return QPointF(r.left(), r.top() - L * 0.25);
    case Trick::Lantern:
        return QPointF(r.left(), r.top() - 34);
    case Trick::Magnify:
        return QPointF(r.left() - 34, r.center().y() + 8);
    case Trick::Count:
        return QPointF(r.left(), r.top() - 18);
    case Trick::Perch:
    case Trick::Trampoline:
    case Trick::Carry:
        return QPointF(r.center().x(), r.top() - L * 0.3);
    case Trick::Trace:
        return r.adjusted(-8, -8, 8, 8).topLeft();
    case Trick::TabHop:
        return act.marks.isEmpty() ? r.center() : act.marks.first().rect.center() - QPointF(0, 30);
    case Trick::RowPatrol:
        return QPointF(r.left() + L * 0.6 + 6, r.center().y());
    case Trick::Fetch:
        return QPointF(r.center().x(), r.top() - 30);
    case Trick::Nibble:
        return QPointF(r.right() + L * 0.5, r.top());
    case Trick::Mirror:
    case Trick::Type:
        return QPointF(r.left() - L * 0.6, r.center().y());
    case Trick::HideAndPeek:
        return QPointF(r.right() - L * 0.3, r.center().y());
    case Trick::Startle:
        return QPointF(r.center().x() + side * (r.width() / 2 + 140), r.center().y());
    case Trick::Checkmark:
    case Trick::BubbleRing:
        return QPointF(r.center().x(), r.top() - 30);
    case Trick::Shout:
    case Trick::Quote:
        return QPointF(r.center().x() + side * (r.width() / 2 + L * 0.5), r.top() - 10);
    case Trick::Hello:
    case Trick::Serenade:
    case Trick::Photo:
    case Trick::Spotlight:
    case Trick::BarrelRoll:
    case Trick::DanceParty:
    case Trick::CopyDance:
    case Trick::Kiss:
    case Trick::NoseBoop:
    case Trick::Tag:
    case Trick::InkCloud:
        return QPointF(head.x() + side * 90, head.y() - 10);
    case Trick::Orbit:
    case Trick::Crown:
    case Trick::Juggle:
    case Trick::FeatherTickle:
    case Trick::Balloon:
    case Trick::SilkSwing:
    case Trick::BubbleBath:
    case Trick::Shadow:
        return QPointF(head.x(), body.top() - 40);
    case Trick::Launch:
        return QPointF(head.x() + side * 60, petFoot().y() - 14);
    case Trick::LegCount:
        return QPointF(body.left() - 10, petFoot().y() - 26);
    case Trick::Peekaboo:
    case Trick::Nap:
        return QPointF(head.x() + side * 56, floorY() - 10);
    case Trick::HideBehind:
    case Trick::Ride:
        return QPointF(body.center().x(), body.top() - 10);
    case Trick::HighFive:
        return QPointF(side < 0 ? body.left() - 30 : body.right() + 30, petFoot().y() - 30);
    case Trick::Race:
        return QPointF(petFoot().x() + side * 34, floorY() - 18);
    case Trick::Pearl:
        return QPointF(head.x() + side * 40, body.top() - 50);
    case Trick::PlayFetch:
        return QPointF(head.x() + side * 50, head.y() - 20);
    case Trick::LeapFrog:
        return QPointF(head.x() + side * 90, petFoot().y() - 18);
    default:
        break;
    }
    return beside;
}

QString Scene::describe(const Act &act) const
{
    const QString what = quoted(act.mark);
    switch (act.trick) {
    case Trick::Peek:
        return QStringLiteral("peeked at %1").arg(what);
    case Trick::Boop:
        return QStringLiteral("booped %1 three times").arg(what);
    case Trick::Read:
        return QStringLiteral("read %1 letter by letter").arg(what);
    case Trick::Underline:
        return QStringLiteral("underlined %1 with bubbles").arg(what);
    case Trick::HideAndPeek:
        return QStringLiteral("played peekaboo behind %1").arg(what);
    case Trick::Polish:
        return QStringLiteral("polished %1 until it shone").arg(what);
    case Trick::Nibble:
        return QStringLiteral("nibbled the corner of %1").arg(what);
    case Trick::Mirror:
        return QStringLiteral("admired itself in %1").arg(what);
    case Trick::Type:
        return QStringLiteral("typed bubble letters into %1").arg(what);
    case Trick::Checkmark:
        return QStringLiteral("drew a bubble tick over %1").arg(what);
    case Trick::Surf:
        return QStringLiteral("surfed along %1").arg(what);
    case Trick::TabHop:
        return QStringLiteral("hopped across %1 %2s").arg(act.marks.size()).arg(
            act.mark.kind == Spot::Tab ? QStringLiteral("tab") : QStringLiteral("button"));
    case Trick::RowPatrol:
        return QStringLiteral("checked %1 rows from %2 down").arg(act.marks.size()).arg(what);
    case Trick::Perch:
        return QStringLiteral("took a nap on top of %1").arg(what);
    case Trick::Lantern:
        return QStringLiteral("shone its lantern on %1").arg(what);
    case Trick::Measure:
        return QStringLiteral("measured %1: %2 px wide").arg(what).arg(int(std::lround(act.mark.rect.width())));
    case Trick::Trace:
        return QStringLiteral("traced a bubble frame around %1").arg(what);
    case Trick::BlowKiss:
        return QStringLiteral("blew a kiss to %1").arg(what);
    case Trick::Count:
        return QStringLiteral("counted the letters in %1: %2").arg(what).arg(act.mark.text.simplified().size());
    case Trick::Startle:
        return QStringLiteral("got startled by %1 and dashed off").arg(what);
    case Trick::Fetch:
        return QStringLiteral("fished a letter out of %1").arg(what);
    case Trick::BubbleRing:
        return QStringLiteral("blew a bubble ring around %1").arg(what);
    case Trick::Highlight:
        return QStringLiteral("highlighted %1").arg(what);
    case Trick::Shout:
        return QStringLiteral("called out %1").arg(what);
    case Trick::Quote:
        return QStringLiteral("quoted %1").arg(what);
    case Trick::Carry:
        return QStringLiteral("carried %1 for a little swim").arg(what);
    case Trick::Wobble:
        return QStringLiteral("bumped %1 and made it wobble").arg(what);
    case Trick::Magnify:
        return QStringLiteral("looked at %1 through a magnifier").arg(what);
    case Trick::Squeegee:
        return QStringLiteral("cleaned the glass over %1").arg(what);
    case Trick::Trampoline:
        return QStringLiteral("bounced on %1 like a trampoline").arg(what);
    case Trick::Hello:
        return QStringLiteral("said hello to the spider");
    case Trick::NoseBoop:
        return QStringLiteral("booped the spider's nose");
    case Trick::Tag:
        return QStringLiteral("played tag with the spider");
    case Trick::Orbit:
        return QStringLiteral("swam rings around the spider until it got dizzy");
    case Trick::Crown:
        return QStringLiteral("gave the spider a bubble crown");
    case Trick::DanceParty:
        return QStringLiteral("threw a dance party with the spider");
    case Trick::Launch:
        return QStringLiteral("bounced the spider into the air");
    case Trick::FeatherTickle:
        return QStringLiteral("tickled the spider's feather");
    case Trick::HideBehind:
        return QStringLiteral("hid behind the spider");
    case Trick::HighFive:
        return QStringLiteral("high-fived the spider");
    case Trick::Race:
        return QStringLiteral("raced the spider along the sand");
    case Trick::Pearl:
        return QStringLiteral("brought the spider a pearl");
    case Trick::Serenade:
        return QStringLiteral("sang the spider a song");
    case Trick::Shadow:
        return QStringLiteral("shadowed the spider across the tank");
    case Trick::BubbleBath:
        return QStringLiteral("gave the spider a bubble bath");
    case Trick::Peekaboo:
        return QStringLiteral("popped out of the sand at the spider");
    case Trick::Ride:
        return QStringLiteral("rode on the spider's back");
    case Trick::Nap:
        return QStringLiteral("napped beside the spider");
    case Trick::Kiss:
        return QStringLiteral("kissed the spider on the cheek");
    case Trick::PlayFetch:
        return QStringLiteral("played fetch with the spider");
    case Trick::Juggle:
        return QStringLiteral("juggled bubbles for the spider");
    case Trick::BarrelRoll:
        return QStringLiteral("did a barrel roll for the spider");
    case Trick::InkCloud:
        return QStringLiteral("got spooked by the spider and puffed ink");
    case Trick::LegCount:
        return QStringLiteral("counted the spider's eight legs");
    case Trick::Photo:
        return QStringLiteral("took the spider's picture");
    case Trick::LeapFrog:
        return QStringLiteral("leapt over the spider");
    case Trick::SilkSwing:
        return QStringLiteral("swung on the spider's silk");
    case Trick::CopyDance:
        return QStringLiteral("copied the spider's dance moves");
    case Trick::Spotlight:
        return QStringLiteral("put the spider in the spotlight");
    case Trick::Balloon:
        return QStringLiteral("tied a bubble balloon to the spider");
    }
    return QString();
}

QString Scene::trickName(Trick trick)
{
    static const char *const names[kTricks] = {
        "Peek",          "Boop",        "Read",        "Underline",  "Peekaboo behind",
        "Polish",        "Nibble",      "Mirror",      "Type",       "Bubble tick",
        "Surf",          "Tab hop",     "Row patrol",  "Perch",      "Lantern",
        "Measure",       "Trace",       "Blow a kiss", "Count",      "Startle",
        "Fetch a letter", "Bubble ring", "Highlight",  "Shout",      "Quote",
        "Carry",         "Wobble",      "Magnify",     "Squeegee",   "Trampoline",
        "Hello",         "Nose boop",   "Tag",         "Orbit",      "Bubble crown",
        "Dance party",   "Launch",      "Feather tickle", "Hide behind", "High five",
        "Race",          "Pearl",       "Serenade",    "Shadow",     "Bubble bath",
        "Pop out of the sand", "Ride",  "Nap",         "Kiss",       "Play fetch",
        "Juggle",        "Barrel roll", "Ink cloud",   "Leg count",  "Photo",
        "Leapfrog",      "Silk swing",  "Copy dance",  "Spotlight",  "Balloon",
    };
    const int index = int(trick);
    return index >= 0 && index < kTricks ? QString::fromLatin1(names[index]) : QString();
}

QString Scene::moodName(Mood mood)
{
    switch (mood) {
    case Mood::Working:
        return QStringLiteral("working");
    case Mood::Waiting:
        return QStringLiteral("needs you");
    case Mood::Queued:
        return QStringLiteral("queued");
    case Mood::Done:
        return QStringLiteral("done");
    case Mood::Merged:
        return QStringLiteral("merged");
    case Mood::Failed:
        return QStringLiteral("failed");
    case Mood::Stopped:
        return QStringLiteral("stopped");
    }
    return QString();
}

QColor Scene::moodColor(Mood mood)
{
    switch (mood) {
    case Mood::Working:
        return QColor(120, 220, 255);
    case Mood::Waiting:
        return QColor(255, 200, 90);
    case Mood::Queued:
        return QColor(190, 200, 215);
    case Mood::Done:
        return QColor(120, 225, 150);
    case Mood::Merged:
        return QColor(200, 160, 255);
    case Mood::Failed:
        return QColor(255, 110, 110);
    case Mood::Stopped:
        return QColor(165, 175, 190);
    }
    return QColor(225, 245, 255);
}

QImage Scene::portrait(const Swimmer &swimmer, int size, qreal ratio)
{
    Scene scene(1);
    scene.spawn(swimmer);
    Fish fish = scene.m_fish.takeLast();
    fish.pos = QPointF();
    fish.vel = QPointF();
    fish.facing = 1;
    fish.heading = 1;
    fish.tail = 1.2;
    fish.entering = false;
    fish.sleepy = swimmer.mood == Mood::Stopped;
    fish.roll = swimmer.mood == Mood::Failed ? 0.3 : 0;
    const double fit = size / (fish.length * (fish.species == 5 ? 2.0 : 1.6));
    scene.m_ratio = std::clamp(ratio * fit, 1.0, 4.0);
    scene.dress(fish);
    QImage image(QSize(size, size) * ratio, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(ratio);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    p.translate(size / 2.0 + size * 0.08, size / 2.0);
    p.scale(fit, fit);
    scene.paintFish(p, fish);
    return image;
}

void Scene::finish(Fish &fish)
{
    if (!fish.act)
        return;
    const Act act = *fish.act;
    fish.act.reset();
    fish.snap = false;
    fish.alpha = 1;
    fish.scale = 1;
    fish.behind = false;
    fish.lantern = false;
    fish.sleepy = false;
    fish.pucker = false;
    fish.roll = 0;
    fish.puffed = 0;
    fish.hasGaze = false;
    fish.wag = 1;
    fish.nextTrick = m_time + (fish.mood == Mood::Working || fish.mood == Mood::Waiting ? rnd(2.0, 5.0) : rnd(7.0, 16.0));
    fish.wanderUntil = 0;
    if (petTrick(act.trick)) {
        if (m_petFish == fish.id)
            m_petFish = -1;
        m_petFree = m_time + rnd(2.5, 5.0);
        if (act.trick == Trick::Crown) {
            for (int i = 0; i < 5; ++i) {
                Particle &p = puff(Puff::Bubble,
                                   petHead() + QPointF(-24 + 12 * i, -m_pet.body.height() * 0.75 -
                                                                         (i == 2 ? 8 : i % 2 ? 4 : 0)),
                                   2.5, 4);
                p.rise = 40;
            }
        }
    } else if (act.trick == Trick::Highlight) {
        Particle &p = puff(Puff::Streak, act.mark.rect.center(), 1.4);
        p.rect = act.mark.rect.adjusted(-3, -1, 3, 1);
        p.color = fish.accent;
    } else if (act.trick == Trick::Squeegee) {
        Particle &p = puff(Puff::Streak, act.mark.rect.center(), 1.6);
        p.rect = act.mark.rect;
        p.image = slice(act.mark.rect);
    }
}

void Scene::perform(Fish &fish, double dt)
{
    Act &act = *fish.act;
    if (petTrick(act.trick) && !m_pet.present) {
        finish(fish);
        return;
    }
    fish.snap = false;
    fish.hasGaze = false;
    fish.speed = 130;
    fish.wag = 1;
    fish.behind = false;
    fish.lantern = false;
    fish.sleepy = false;
    fish.pucker = false;
    if (!act.performing) {
        act.approach += dt;
        fish.goal = approachPoint(fish);
        fish.speed = 170;
        if (act.trick == Trick::Peekaboo) {
            fish.alpha += (0.2 - fish.alpha) * std::min(1.0, dt * 3);
            fish.scale = 0.85;
        }
        if (act.trick == Trick::HideBehind)
            fish.behind = true;
        const QPointF d = fish.goal - fish.pos;
        if (std::hypot(d.x(), d.y()) >= 14 && act.approach <= 6)
            return;
        act.performing = true;
        act.t = -dt;
        fish.speed = 130;
        fish.alpha = act.trick == Trick::Peekaboo ? fish.alpha : 1;
        m_notes.append({fish.id, fish.name, describe(act)});
    }
    act.t += dt;
    const double u = clamp01(act.t / act.length);
    if (petTrick(act.trick))
        performPet(fish, act, u, dt);
    else
        performElement(fish, act, u, dt);
    if (fish.act && act.t >= act.length)
        finish(fish);
}

void Scene::performElement(Fish &fish, Act &act, double u, double dt)
{
    const QRectF r = act.mark.rect;
    const double L = fish.length;
    const double side = act.side;
    const double t = act.t;
    const auto every = [&](double period, int slot) {
        const int tick = int(t / period);
        if (tick == act.step + slot * 100000)
            return false;
        act.step = tick + slot * 100000;
        return true;
    };
    switch (act.trick) {
    case Trick::Peek: {
        fish.goal = QPointF(side < 0 ? r.left() - L * 0.55 : r.right() + L * 0.55,
                            r.center().y() + std::sin(t * 2) * 4);
        fish.snap = true;
        fish.gaze = r.center();
        fish.hasGaze = true;
        fish.scale = 1 + 0.08 * std::sin(t * 3);
        if (u > 0.55 && once(act, 0)) {
            Particle &p = puff(Puff::Text, fish.pos + QPointF(0, -L * 0.6), 1.2, 14);
            p.text = QStringLiteral("!?");
            p.rise = 10;
            p.color = QColor(255, 240, 160);
        }
        break;
    }
    case Trick::Boop: {
        const double edge = side < 0 ? r.left() : r.right();
        const double phase = std::fmod(u * 3, 1.0);
        const double d = std::sin(phase * kPi);
        fish.goal = QPointF(edge + side * (L * 0.48 + 16 * (1 - d)), r.center().y());
        fish.snap = true;
        fish.mouth = d;
        const int boop = int(u * 3);
        if (phase > 0.45 && boop != act.step && boop < 3) {
            act.step = boop;
            Particle &ring = puff(Puff::Ring, r.center(), 0.7);
            ring.rect = r;
            ring.color = fish.accent;
        }
        break;
    }
    case Trick::Read: {
        const double x = r.left() + r.width() * ease(u);
        fish.goal = QPointF(x, r.top() - 10 - L * 0.15);
        fish.snap = true;
        fish.gaze = QPointF(x + 24, r.center().y());
        fish.hasGaze = true;
        fish.mouth = 0.5 + 0.5 * std::sin(t * 12);
        const QString text = act.mark.text.simplified();
        if (!text.isEmpty() && every(0.22, 0)) {
            const int index = std::clamp(int(ease(u) * text.size()), 0, int(text.size()) - 1);
            if (!text.at(index).isSpace()) {
                Particle &p = puff(Puff::Glyph, QPointF(x + 10, r.top() - 4), 1.3, 11);
                p.text = text.at(index);
                p.rise = 18;
                p.vel = QPointF(0, -20);
            }
        }
        break;
    }
    case Trick::Underline: {
        const double x = r.left() + r.width() * u;
        fish.goal = QPointF(x, r.bottom() + 9);
        fish.snap = true;
        if (every(0.05, 0)) {
            Particle &p = puff(Puff::Bubble, QPointF(x - 8, r.bottom() + 3), 2.4, rnd(2, 3.2));
            p.rise = 3;
        }
        break;
    }
    case Trick::HideAndPeek: {
        const double phase = std::fmod(u * 3, 1.0);
        const double out = std::pow(std::sin(phase * kPi), 2);
        fish.goal = QPointF(r.right() - L * 0.35 + out * L * 0.95, r.center().y() + std::sin(t) * 6);
        fish.snap = true;
        fish.alpha = 0.25 + 0.75 * out;
        fish.scale = 0.85 + 0.15 * out;
        fish.gaze = fish.pos + QPointF(40, 0);
        fish.hasGaze = true;
        const int cycle = int(u * 3);
        if (out > 0.9 && cycle != act.step) {
            act.step = cycle;
            puff(Puff::Sparkle, fish.pos + QPointF(0, -L * 0.5), 0.8, 5).color = QColor(255, 250, 200);
        }
        break;
    }
    case Trick::Polish: {
        fish.goal = QPointF(r.left() + r.width() * (0.5 + 0.45 * std::sin(t * 5)),
                            r.center().y() + 0.3 * r.height() * std::sin(t * 2.3));
        fish.snap = true;
        fish.wag = 2.5;
        if (every(0.12, 0)) {
            Particle &p = puff(Puff::Sparkle,
                               QPointF(rnd(r.left(), r.right()), rnd(r.top(), r.bottom())), 0.7, rnd(3, 6));
            p.color = QColor(255, 255, 220);
        }
        if (u > 0.95 && once(act, 0)) {
            Particle &ring = puff(Puff::Ring, r.center(), 0.9);
            ring.rect = r;
            ring.color = QColor(255, 230, 120);
        }
        break;
    }
    case Trick::Nibble: {
        const QPointF corner(r.right(), r.top());
        fish.goal = corner + QPointF(L * 0.5, std::sin(t * 30) * 2);
        fish.snap = true;
        fish.gaze = corner;
        fish.hasGaze = true;
        fish.mouth = std::sin(t * 25) > 0 ? 1 : 0;
        if (every(0.11, 0)) {
            QColor color(220, 220, 220);
            if (!m_backdrop.isNull()) {
                const qreal ratio = m_backdrop.devicePixelRatio();
                const QPoint at = ((corner + QPointF(-3, 3)) * ratio).toPoint();
                if (m_backdrop.rect().contains(at))
                    color = m_backdrop.pixelColor(at);
            }
            Particle &p = puff(Puff::Crumb, corner + QPointF(rnd(-4, 2), rnd(-2, 4)), 1.6, rnd(1.8, 3));
            p.color = color;
            p.vel = QPointF(rnd(-20, 30), rnd(-30, 5));
            p.rise = -45;
        }
        break;
    }
    case Trick::Mirror: {
        fish.goal = QPointF(r.left() - L * 0.55, r.center().y() + std::sin(t * 2) * 3);
        fish.snap = true;
        fish.gaze = r.center();
        fish.hasGaze = true;
        if (u > 0.5 && once(act, 0)) {
            Particle &p = puff(Puff::Heart, QPointF(r.left() + 4, r.center().y() - 14), 1.4, 6);
            p.rise = 25;
            p.color = QColor(255, 120, 170);
        }
        break;
    }
    case Trick::Type: {
        fish.goal = QPointF(r.left() - L * 0.5, r.center().y());
        fish.snap = true;
        fish.gaze = r.center();
        fish.hasGaze = true;
        fish.mouth = 0.5 + 0.5 * std::sin(t * 14);
        if (every(0.22, 0)) {
            const QString name = fish.name.isEmpty() ? QStringLiteral("fish") : fish.name;
            const int index = int(t / 0.22) % name.size();
            const double x = r.left() + 12 + std::fmod(index * 9.0, std::max(20.0, r.width() - 20));
            Particle &p = puff(Puff::Glyph, QPointF(x, r.center().y()), 1.7, 11);
            p.text = name.at(index);
            p.rise = 26;
        }
        break;
    }
    case Trick::Checkmark: {
        const QRectF box = QRectF(r.center() - QPointF(22, 22), QSizeF(44, 44)).united(r);
        const QPointF a(box.left() + box.width() * 0.15, box.center().y());
        const QPointF b(box.left() + box.width() * 0.42, box.bottom() - 2);
        const QPointF c(box.right() + 8, box.top() - 10);
        fish.goal = u < 0.4 ? mix(a, b, u / 0.4) : mix(b, c, (u - 0.4) / 0.6);
        fish.snap = true;
        if (every(0.04, 0)) {
            Particle &p = puff(Puff::Bubble, fish.goal, 1.9, 3);
            p.rise = 2;
        }
        if (u > 0.96 && once(act, 0)) {
            for (int i = 0; i < 6; ++i) {
                Particle &p = puff(Puff::Sparkle, c + QPointF(rnd(-10, 10), rnd(-10, 10)), 0.8, 5);
                p.color = QColor(160, 255, 180);
            }
        }
        break;
    }
    case Trick::Surf: {
        fish.goal = QPointF(r.left() + r.width() * ease(u), r.top() - L * 0.25);
        fish.snap = true;
        fish.tilt = -0.25;
        if (every(0.05, 0)) {
            Particle &p = puff(Puff::Bubble, fish.pos + QPointF(-fish.facing * L * 0.4, L * 0.2), 0.9, 2.4);
            p.vel = QPointF(-fish.facing * rnd(30, 60), rnd(-50, -20));
            p.rise = -40;
        }
        break;
    }
    case Trick::TabHop: {
        const int n = int(act.marks.size());
        if (n == 0)
            break;
        const int i = std::min(n - 1, int(u * n));
        const double v = u * n - i;
        const QPointF to = act.marks.at(i).rect.center() - QPointF(0, act.marks.at(i).rect.height() / 2 + 12);
        const QPointF from = i == 0 ? to - QPointF(40, 0)
                                    : act.marks.at(i - 1).rect.center() -
                                          QPointF(0, act.marks.at(i - 1).rect.height() / 2 + 12);
        fish.goal = mix(from, to, ease(v)) + QPointF(0, -40 * std::sin(v * kPi));
        fish.snap = true;
        fish.gaze = act.marks.at(i).rect.center();
        fish.hasGaze = v > 0.7;
        if (v > 0.9 && act.step != i) {
            act.step = i;
            Particle &ring = puff(Puff::Ring, act.marks.at(i).rect.center(), 0.6);
            ring.rect = act.marks.at(i).rect;
            ring.color = fish.accent;
        }
        break;
    }
    case Trick::RowPatrol: {
        const int n = int(act.marks.size());
        if (n == 0)
            break;
        const int i = std::min(n - 1, int(u * n));
        const double v = u * n - i;
        const QRectF row = act.marks.at(i).rect;
        const QRectF last = i == 0 ? row : act.marks.at(i - 1).rect;
        const QPointF at(row.left() + L * 0.6 + 6, row.center().y());
        const QPointF was(last.left() + L * 0.6 + 6, last.center().y());
        fish.goal = mix(was, at, ease(v / 0.4));
        fish.snap = true;
        fish.gaze = QPointF(row.left() + 90, row.center().y());
        fish.hasGaze = true;
        if (v > 0.5 && act.step != i) {
            act.step = i;
            Particle &p = puff(Puff::Text, QPointF(std::min(row.right() - 14, row.left() + 220), row.center().y()), 1.1, 12);
            p.text = QStringLiteral("✓");
            p.color = QColor(150, 255, 170);
            p.rise = 12;
        }
        break;
    }
    case Trick::Perch: {
        fish.goal = QPointF(r.center().x(), r.top() - L * 0.2 + std::sin(t * 1.5) * 1.5);
        fish.snap = true;
        fish.wag = 0.2;
        fish.sleepy = u > 0.15 && u < 0.92;
        if (fish.sleepy && every(0.8, 0)) {
            Particle &p = puff(Puff::Zzz, fish.pos + QPointF(fish.facing * 8, -L * 0.4), 1.8, 9);
            p.vel = QPointF(8, -14);
        }
        break;
    }
    case Trick::Lantern: {
        fish.goal = QPointF(r.left() + r.width() * ease(u), r.top() - 34);
        fish.snap = true;
        fish.lantern = true;
        fish.gaze = QPointF(fish.pos.x() + 20, r.center().y());
        fish.hasGaze = true;
        break;
    }
    case Trick::Measure: {
        fish.goal = QPointF(r.left() + r.width() * ease(std::min(1.0, u * 1.25)), r.top() - 14);
        fish.snap = true;
        fish.gaze = QPointF(fish.pos.x(), r.top());
        fish.hasGaze = true;
        break;
    }
    case Trick::Trace: {
        fish.goal = perimeterPoint(r.adjusted(-8, -8, 8, 8), u);
        fish.snap = true;
        if (every(0.05, 0)) {
            Particle &p = puff(Puff::Bubble, fish.goal, 1.6, 2.6);
            p.rise = 2;
        }
        break;
    }
    case Trick::BlowKiss: {
        fish.goal = QPointF(side < 0 ? r.left() - L * 0.8 : r.right() + L * 0.8, r.center().y() - 6);
        fish.snap = true;
        fish.pucker = u > 0.2 && u < 0.45;
        fish.gaze = r.center();
        fish.hasGaze = true;
        if (u > 0.4 && once(act, 0)) {
            Particle &p = puff(Puff::Heart, mouthOf(fish), 1.3, 7);
            p.vel = (r.center() - mouthOf(fish)) / 1.1;
            p.color = QColor(255, 110, 160);
        }
        if (u > 0.85 && once(act, 1)) {
            Particle &ring = puff(Puff::Ring, r.center(), 0.8);
            ring.rect = r;
            ring.color = QColor(255, 140, 190);
        }
        break;
    }
    case Trick::Count: {
        const QString text = act.mark.text.simplified();
        const int n = std::max(1, int(text.size()));
        const int i = std::min(n - 1, int(u * 0.9 * n));
        const double x = r.left() + r.width() * (i + 0.5) / n;
        fish.goal = QPointF(x, r.top() - 18);
        fish.snap = true;
        fish.gaze = QPointF(x, r.center().y());
        fish.hasGaze = true;
        if (act.step != i) {
            act.step = i;
            Particle &p = puff(Puff::Text, QPointF(x, r.top() - 2), 0.9, 10);
            p.text = QString::number(i + 1);
            p.rise = 20;
            p.color = QColor(255, 255, 200);
        }
        if (u > 0.9 && once(act, 0))
            say(fish, QStringLiteral("%1!").arg(n), 1.4);
        break;
    }
    case Trick::Startle: {
        const double edge = side < 0 ? r.left() : r.right();
        if (u < 0.45) {
            fish.goal = QPointF(edge + side * L * 0.7, r.center().y());
            fish.speed = 28;
            fish.gaze = r.center();
            fish.hasGaze = true;
        } else {
            if (once(act, 0)) {
                Particle &p = puff(Puff::Text, fish.pos + QPointF(0, -L * 0.7), 1.0, 18);
                p.text = QStringLiteral("!");
                p.color = QColor(255, 220, 90);
                puff(Puff::Flash, fish.pos, 0.3, L * 0.8);
                if (fish.species == 3)
                    fish.puffed = 1;
            }
            const QRectF area = tank();
            fish.goal = QPointF(std::clamp(edge + side * 340, area.left(), area.right()),
                                std::clamp(r.center().y() - 60, area.top(), area.bottom()));
            fish.speed = 420;
            if (every(0.04, 0)) {
                Particle &p = puff(Puff::Streak, fish.pos - QPointF(fish.facing * L * 0.6, 0), 0.35);
                p.rect = QRectF(-12, -1, 24, 2);
                p.color = QColor(255, 255, 255, 170);
            }
        }
        break;
    }
    case Trick::Fetch: {
        if (u < 0.35) {
            fish.goal = r.center();
            fish.alpha = 1 - 0.65 * span(u, 0.1, 0.35);
            fish.scale = 0.9;
        } else if (u < 0.5) {
            fish.goal = r.center() + QPointF(std::sin(t * 18) * 4, 0);
            fish.snap = true;
            fish.alpha = 0.35;
        } else {
            fish.goal = QPointF(r.center().x(), r.top() - 60);
            fish.alpha = std::min(1.0, fish.alpha + dt * 3);
            fish.scale = 1;
        }
        if (u > 0.92 && once(act, 0)) {
            const QString text = act.mark.text.simplified();
            Particle &p = puff(Puff::Glyph, mouthOf(fish), 2.4, 15);
            p.text = text.isEmpty() ? QStringLiteral("A") : text.left(1).toUpper();
            p.rise = 30;
        }
        break;
    }
    case Trick::BubbleRing: {
        const double radius = std::max(r.width(), r.height()) / 2 + 18;
        const double a = u * 2 * kPi * 1.1 - kPi / 2;
        fish.goal = r.center() + QPointF(std::cos(a) * radius, std::sin(a) * radius * 0.85);
        fish.snap = true;
        if (u < 0.92 && every(0.05, 0)) {
            Particle &p = puff(Puff::Bubble, fish.goal, act.length * (1 - u) + 0.3, 3.4);
            p.rise = 0;
        }
        if (u > 0.95 && once(act, 0)) {
            for (int i = 0; i < 10; ++i) {
                const double b = i * 2 * kPi / 10;
                Particle &p = puff(Puff::Sparkle, r.center() + QPointF(std::cos(b), std::sin(b) * 0.85) * radius, 0.7, 5);
                p.color = QColor(200, 245, 255);
            }
        }
        break;
    }
    case Trick::Highlight: {
        fish.goal = QPointF(r.left() + r.width() * u, r.center().y());
        fish.snap = true;
        fish.alpha = 0.9;
        break;
    }
    case Trick::Shout: {
        fish.goal = QPointF(r.center().x() + side * (r.width() / 2 + L * 0.5),
                            r.top() - 10 - std::abs(std::sin(t * 8)) * 6);
        fish.snap = true;
        fish.mouth = 1;
        fish.gaze = r.center();
        fish.hasGaze = true;
        if (once(act, 0))
            say(fish, shorten(act.mark.text, 24) + QStringLiteral("!"), act.length - 0.2);
        break;
    }
    case Trick::Quote: {
        fish.goal = QPointF(r.center().x() + side * (r.width() / 2 + L * 0.5), r.top() - 10);
        fish.snap = true;
        fish.mouth = 0.4 + 0.4 * std::sin(t * 9);
        if (once(act, 0))
            say(fish, QStringLiteral("“%1”").arg(shorten(act.mark.text, 30)), act.length - 0.2);
        break;
    }
    case Trick::Carry: {
        const double lift = ease(span(u, 0.15, 0.4)) * (1 - ease(span(u, 0.75, 0.97)));
        const double sway = std::sin(t * 3) * 14 * lift;
        act.memo = QPointF(sway, -lift * 46);
        fish.goal = QPointF(r.center().x(), r.top() - L * 0.3) + act.memo;
        fish.snap = true;
        fish.wag = 1.8;
        if (u > 0.97 && once(act, 0)) {
            Particle &ring = puff(Puff::Ring, r.center(), 0.6);
            ring.rect = r;
            ring.color = fish.accent;
        }
        break;
    }
    case Trick::Wobble: {
        const double edge = side < 0 ? r.left() : r.right();
        const double bump = u < 0.25 ? ease(u / 0.25) : 1 - ease(span(u, 0.25, 0.5)) * 0.6;
        fish.goal = QPointF(edge + side * (L * 0.48 + 30 * (1 - bump)), r.center().y());
        fish.snap = true;
        if (u >= 0.25 && once(act, 0)) {
            Particle &p = puff(Puff::Star, QPointF(edge, r.center().y()), 0.5, 6);
            p.color = QColor(255, 240, 150);
        }
        const double w = span(u, 0.25, 1.0);
        act.memo = QPointF(u < 0.25 ? 0 : std::sin(w * 30) * 0.12 * (1 - w), 0);
        break;
    }
    case Trick::Magnify: {
        const QPointF lens(r.left() + r.width() * ease(u), r.center().y());
        act.memo = lens;
        fish.goal = lens + QPointF(-38, 12);
        fish.snap = true;
        fish.gaze = lens;
        fish.hasGaze = true;
        break;
    }
    case Trick::Squeegee: {
        const double x = r.left() + r.width() * u;
        fish.goal = QPointF(x, r.center().y() + std::sin(t * 9) * r.height() * 0.3);
        fish.snap = true;
        fish.wag = 2;
        act.memo = QPointF(x, 0);
        break;
    }
    case Trick::Trampoline: {
        const double phase = std::fmod(u * 3, 1.0);
        const double h = 4 * phase * (1 - phase);
        fish.goal = QPointF(r.center().x(), r.top() - L * 0.25 - h * 64);
        fish.snap = true;
        act.memo = QPointF(phase < 0.12 || phase > 0.92 ? 1 : 0, 0);
        const int bounce = int(u * 3);
        if (phase < 0.1 && bounce != act.step) {
            act.step = bounce;
            bubbles(QPointF(r.center().x(), r.top()), 3, 8);
        }
        break;
    }
    default:
        break;
    }
}

void Scene::performPet(Fish &fish, Act &act, double u, double dt)
{
    Q_UNUSED(dt);
    const QRectF body = m_pet.body;
    const QPointF head = petHead();
    const QPointF foot = petFoot();
    const double side = act.side;
    const double t = act.t;
    const double L = fish.length;
    const QRectF area = tank();
    fish.gaze = head;
    fish.hasGaze = true;
    const auto every = [&](double period) {
        const int tick = int(t / period);
        if (tick == act.step)
            return false;
        act.step = tick;
        return true;
    };
    switch (act.trick) {
    case Trick::Hello:
        fish.goal = QPointF(head.x() + side * 62, head.y() - 6 + std::sin(t * 4) * 4);
        fish.snap = true;
        fish.wag = 2.5;
        if (once(act, 0)) {
            cue(Cue::Wave);
            say(fish, QStringLiteral("Hi, spider!"), 1.8);
        }
        if (u > 0.6 && once(act, 1)) {
            Particle &p = puff(Puff::Heart, fish.pos + QPointF(0, -L * 0.5), 1.4, 6);
            p.rise = 25;
            p.color = QColor(255, 120, 170);
        }
        break;
    case Trick::NoseBoop: {
        const double pulse = std::sin(span(u, 0.25, 0.65) * kPi);
        fish.goal = QPointF(head.x() + side * (L * 0.5 + 10 + 50 * (1 - pulse)), head.y());
        fish.snap = true;
        if (u > 0.45 && once(act, 0)) {
            cue(Cue::Giggle);
            Particle &ring = puff(Puff::Ring, head, 0.6);
            ring.rect = QRectF(head - QPointF(10, 10), QSizeF(20, 20));
            puff(Puff::Star, head + QPointF(side * 14, -6), 0.6, 6).color = QColor(255, 240, 150);
        }
        break;
    }
    case Trick::Tag:
        if (u < 0.2) {
            fish.goal = QPointF(head.x() + side * (L * 0.5 + 8), head.y());
            fish.speed = 260;
        } else {
            if (once(act, 0)) {
                cue(Cue::Surprise);
                say(fish, QStringLiteral("Tag! You're it!"), 1.6);
                act.memo = QPointF(std::clamp(head.x() - side * 360, area.left() + 40, area.right() - 40), 0);
            }
            fish.goal = QPointF(act.memo.x(), floorY() - 70 + std::sin(t * 5) * 10);
            fish.speed = 300;
            fish.hasGaze = false;
            if (u > 0.3 && once(act, 1))
                cue(Cue::Walk, QPointF(act.memo.x(), 0));
            if (u > 0.9 && once(act, 2))
                cue(Cue::Giggle);
        }
        break;
    case Trick::Orbit: {
        const QPointF c(body.center().x(), body.center().y() - 10);
        const double a = -side * u * 4 * kPi + (side < 0 ? kPi : 0);
        fish.goal = c + QPointF(std::cos(a) * (body.width() * 0.5 + 26), std::sin(a) * (body.height() * 0.4 + 14));
        fish.snap = true;
        fish.gaze = c;
        if (u > 0.85 && once(act, 0)) {
            cue(Cue::Dizzy);
            for (int i = 0; i < 3; ++i)
                puff(Puff::Star, QPointF(head.x() - 14 + 14 * i, body.top() - 6), 1.0, 4).color = QColor(255, 235, 120);
        }
        break;
    }
    case Trick::Crown:
        fish.goal = QPointF(head.x() + std::sin(t * 2) * 30, body.top() - 30);
        fish.snap = true;
        fish.mouth = 0.5 + 0.5 * std::sin(t * 10);
        if (int(u * 5.5) > act.step && act.step < 5) {
            act.step = int(u * 5.5);
            bubbles(mouthOf(fish), 1, 1);
        }
        if (u > 0.9 && once(act, 0)) {
            cue(Cue::Cheer);
            for (int i = 0; i < 6; ++i)
                puff(Puff::Sparkle, QPointF(head.x() + rnd(-28, 28), body.top() - rnd(0, 18)), 0.8, 5).color =
                    QColor(255, 250, 200);
        }
        break;
    case Trick::DanceParty:
        if (once(act, 0))
            cue(Cue::Dance);
        fish.goal = QPointF(head.x() + side * 80 + std::sin(t * 4) * 20, head.y() - 20 + std::sin(t * 8) * 10);
        fish.snap = true;
        fish.tilt = std::sin(t * 8) * 0.3;
        if (every(0.35)) {
            Particle &p = puff(Puff::Note, fish.pos + QPointF(rnd(-10, 10), -L * 0.5), 1.4, 12);
            p.vel = QPointF(rnd(-15, 15), -30);
            p.color = QColor::fromHsv(int(rnd(0, 359)), 120, 255);
        }
        break;
    case Trick::Launch:
        if (u < 0.4) {
            fish.goal = QPointF(foot.x() + side * 40 * (1 - u / 0.4), foot.y() - 12);
            fish.snap = u > 0.1;
        } else {
            if (once(act, 0)) {
                cue(Cue::Hop, QPointF(-side * 80, -980));
                cue(Cue::Surprise);
                bubbles(foot, 8, 14);
            }
            fish.goal = QPointF(foot.x() + side * 20, body.top() - 120);
            fish.speed = 240;
        }
        break;
    case Trick::FeatherTickle:
        fish.goal = QPointF(head.x() + side * 18 + std::sin(t * 16) * 3, body.top() - 4);
        fish.snap = true;
        fish.mouth = std::sin(t * 20) > 0 ? 1 : 0.2;
        fish.gaze = QPointF(head.x(), body.top());
        if (u > 0.15 && once(act, 0))
            cue(Cue::Giggle);
        if (u > 0.65 && once(act, 1))
            cue(Cue::Giggle);
        if (every(0.2)) {
            puff(Puff::Sparkle, QPointF(head.x() + rnd(-12, 12), body.top() - rnd(0, 10)), 0.6, 4).color =
                QColor(255, 220, 250);
        }
        break;
    case Trick::HideBehind: {
        fish.behind = true;
        const double phase = std::fmod(span(u, 0.1, 0.85) * 2, 1.0);
        const double out = u < 0.1 || u > 0.85 ? 0 : std::pow(std::sin(phase * kPi), 2);
        const int cycle = int(span(u, 0.1, 0.85) * 2);
        const double dir = cycle % 2 ? 1 : -1;
        fish.goal = QPointF(body.center().x() + dir * out * (body.width() * 0.5 + 6), body.center().y() - 4);
        fish.snap = true;
        fish.gaze = fish.pos + QPointF(dir * 40, 0);
        if (u > 0.1 && u < 0.85 && every(0.6))
            cue(Cue::Look, QPointF(body.center().x() - dir * 300, body.center().y()));
        if (u > 0.86 && once(act, 0)) {
            fish.behind = false;
            say(fish, QStringLiteral("Boo!"), 1.2);
            cue(Cue::Surprise);
        }
        if (u > 0.86) {
            fish.behind = false;
            fish.goal = QPointF(body.center().x() + side * (body.width() * 0.5 + L), body.top() - 10);
            fish.snap = false;
        }
        break;
    }
    case Trick::HighFive: {
        const double reach = std::sin(span(u, 0.25, 0.6) * kPi);
        const double x = side < 0 ? body.left() - L * 0.5 - 18 * (1 - reach) : body.right() + L * 0.5 + 18 * (1 - reach);
        fish.goal = QPointF(x, foot.y() - 30);
        fish.snap = true;
        if (u > 0.42 && once(act, 0)) {
            cue(Cue::Cheer);
            const QPointF contact(side < 0 ? body.left() : body.right(), foot.y() - 28);
            for (int i = 0; i < 5; ++i) {
                Particle &p = puff(Puff::Star, contact, 0.8, 5);
                p.vel = QPointF(std::cos(i * 1.256) * 70, std::sin(i * 1.256) * 70);
                p.color = QColor(255, 230, 110);
            }
        }
        break;
    }
    case Trick::Race:
        if (once(act, 0)) {
            const double target = foot.x() < m_size.width() / 2 ? area.right() - 60 : area.left() + 60;
            act.memo = QPointF(target, 0);
            act.length = std::max(4.0, std::abs(target - foot.x()) / 95.0 + 1.2);
            say(fish, QStringLiteral("Ready, set, go!"), 1.2);
            cue(Cue::Walk, QPointF(target, 0));
        }
        fish.goal = QPointF(act.memo.x(), floorY() - 18 - std::abs(std::sin(t * 6)) * 6);
        fish.speed = 100;
        fish.wag = 2.2;
        fish.hasGaze = false;
        if (u > 0.97 && once(act, 1)) {
            cue(Cue::Cheer);
            for (int i = 0; i < 6; ++i)
                puff(Puff::Sparkle, QPointF(act.memo.x() + rnd(-20, 20), floorY() - rnd(10, 40)), 0.9, 5).color =
                    QColor(255, 250, 190);
        }
        break;
    case Trick::Pearl:
        fish.goal = QPointF(head.x() + side * 40, body.top() - 50);
        fish.snap = u > 0.1;
        if (u > 0.3 && once(act, 0)) {
            Particle &p = puff(Puff::Pearl, mouthOf(fish), 6, 5);
            p.vel = QPointF(-side * 10, 20);
            p.rise = -60;
            p.color = QColor(250, 245, 255);
        }
        if (u > 0.6 && once(act, 1))
            cue(Cue::Eureka);
        break;
    case Trick::Serenade:
        fish.goal = QPointF(head.x() + side * 90, head.y() - 30 + std::sin(t * 3) * 5);
        fish.snap = true;
        fish.mouth = 0.5 + 0.5 * std::sin(t * 7);
        if (every(0.4)) {
            Particle &p = puff(Puff::Note, mouthOf(fish), 1.5, 12);
            p.vel = (head - mouthOf(fish)) / 1.6 + QPointF(0, -10);
            p.color = QColor::fromHsv(int(rnd(180, 330)), 110, 255);
        }
        if (u > 0.25 && once(act, 0))
            cue(Cue::Nod);
        if (u > 0.65 && once(act, 1))
            cue(Cue::Nod);
        break;
    case Trick::Shadow:
        if (once(act, 0)) {
            const double target = std::clamp(foot.x() + (foot.x() < m_size.width() / 2 ? 1 : -1) * rnd(180, 320),
                                             area.left() + 40, area.right() - 40);
            cue(Cue::Walk, QPointF(target, 0));
        }
        fish.goal = QPointF(head.x(), body.top() - 60 + std::sin(t * 3) * 4);
        fish.speed = 170;
        if (u > 0.95 && once(act, 1))
            cue(Cue::Nod);
        break;
    case Trick::BubbleBath:
        fish.goal = QPointF(head.x() + side * 10, body.top() - 56);
        fish.snap = true;
        fish.mouth = 1;
        if (every(0.08)) {
            for (int i = 0; i < 2; ++i) {
                Particle &p = puff(Puff::Bubble, QPointF(rnd(body.left(), body.right()), rnd(body.top(), body.bottom())),
                                   rnd(1.2, 2.2), rnd(2.5, 5.5));
                p.rise = 30;
                p.vel = QPointF(rnd(-15, 15), rnd(-15, 5));
            }
        }
        if (u > 0.2 && once(act, 0))
            cue(Cue::Giggle);
        if (u > 0.9 && once(act, 1))
            cue(Cue::Cheer);
        break;
    case Trick::Peekaboo:
        if (u < 0.5) {
            fish.goal = QPointF(head.x() + side * 46, floorY() - 6);
            fish.alpha = 0.2;
            fish.scale = 0.85;
        } else {
            if (once(act, 0)) {
                say(fish, QStringLiteral("Boo!"), 1.3);
                cue(Cue::Surprise);
                for (int i = 0; i < 10; ++i) {
                    Particle &p = puff(Puff::Sand, QPointF(head.x() + side * 46 + rnd(-12, 12), floorY()), 1.0, rnd(2, 4));
                    p.vel = QPointF(rnd(-50, 50), rnd(-90, -30));
                    p.rise = -120;
                }
            }
            fish.alpha = 1;
            fish.scale = 1;
            fish.goal = QPointF(head.x() + side * 46, head.y() - 10);
            fish.speed = 260;
        }
        break;
    case Trick::Ride:
        if (once(act, 0)) {
            const double target = std::clamp(foot.x() + (foot.x() < m_size.width() / 2 ? 1 : -1) * 280.0,
                                             area.left() + 40, area.right() - 40);
            cue(Cue::Walk, QPointF(target, 0));
            say(fish, QStringLiteral("Giddy up!"), 1.4);
        }
        fish.goal = QPointF(body.center().x(), body.top() + 6);
        fish.snap = true;
        fish.wag = 0.5;
        fish.hasGaze = false;
        if (u > 0.95 && once(act, 1))
            cue(Cue::Cheer);
        break;
    case Trick::Nap:
        fish.goal = QPointF(foot.x() + side * 58, floorY() - 12);
        fish.snap = true;
        fish.wag = 0.15;
        fish.sleepy = u < 0.93;
        fish.hasGaze = false;
        if (fish.sleepy && every(0.9)) {
            Particle &p = puff(Puff::Zzz, fish.pos + QPointF(0, -L * 0.4), 1.8, 9);
            p.vel = QPointF(6, -14);
            if (act.step % 2 == 0) {
                Particle &q = puff(Puff::Zzz, QPointF(head.x() + 8, body.top()), 2.0, 11);
                q.vel = QPointF(8, -12);
            }
        }
        if (u > 0.94 && once(act, 0))
            cue(Cue::Nod);
        break;
    case Trick::Kiss: {
        const double close = std::sin(span(u, 0.2, 0.7) * kPi);
        fish.goal = QPointF(head.x() + side * (L * 0.45 + 12 + 40 * (1 - close)), head.y() + 6);
        fish.snap = true;
        fish.pucker = close > 0.3;
        if (u > 0.45 && once(act, 0)) {
            cue(Cue::Cheer);
            for (int i = 0; i < 3; ++i) {
                Particle &p = puff(Puff::Heart, head + QPointF(side * 12, -6), 1.5, 5 + i);
                p.vel = QPointF(rnd(-20, 20), -30 - 10 * i);
                p.color = QColor(255, 110, 160);
            }
        }
        break;
    }
    case Trick::PlayFetch:
        if (once(act, 0))
            act.memo = QPointF(std::clamp(foot.x() - side * 220, area.left() + 30, area.right() - 30), 0);
        if (u < 0.15) {
            fish.goal = QPointF(head.x() + side * 50, head.y() - 20);
            fish.snap = true;
        } else {
            if (once(act, 1))
                cue(Cue::Walk, QPointF(act.memo.x(), 0));
            fish.goal = u < 0.4 ? QPointF(head.x() + side * 50, head.y() - 20)
                                : QPointF(act.memo.x() + side * 30, floorY() - 30);
        }
        if (u > 0.75 && once(act, 2)) {
            cue(Cue::Cheer);
            for (int i = 0; i < 6; ++i)
                puff(Puff::Sparkle, QPointF(act.memo.x() + rnd(-10, 10), floorY() - 8 + rnd(-10, 6)), 0.7, 5).color =
                    QColor(200, 245, 255);
        }
        break;
    case Trick::Juggle:
        fish.goal = QPointF(head.x() + side * 4, body.top() - 34 + std::sin(t * 6) * 3);
        fish.snap = true;
        fish.tilt = -0.5;
        fish.gaze = QPointF(head.x(), body.top() - 90);
        if (u > 0.2 && once(act, 0))
            cue(Cue::Eureka);
        if (u > 0.95 && once(act, 1)) {
            for (int i = 0; i < 6; ++i)
                puff(Puff::Sparkle, QPointF(head.x() + rnd(-22, 22), body.top() - rnd(60, 100)), 0.7, 5).color =
                    QColor(200, 245, 255);
        }
        break;
    case Trick::BarrelRoll:
        fish.goal = QPointF(head.x() + side * 80, head.y() - 20 - std::sin(u * kPi) * 20);
        fish.snap = true;
        fish.roll = ease(u) * 4 * kPi;
        if (u > 0.5 && once(act, 0))
            for (int i = 0; i < 4; ++i)
                puff(Puff::Sparkle, fish.pos + QPointF(rnd(-20, 20), rnd(-20, 20)), 0.6, 5).color = QColor(255, 255, 220);
        if (u > 0.9 && once(act, 1))
            cue(Cue::Cheer);
        break;
    case Trick::InkCloud:
        if (u < 0.35) {
            fish.goal = QPointF(head.x() + side * (L * 0.5 + 30), head.y());
            fish.speed = 45;
        } else {
            if (once(act, 0)) {
                cue(Cue::Surprise);
                say(fish, QStringLiteral("Eek!"), 1.0);
                fish.puffed = fish.species == 3 ? 1 : 0;
                for (int i = 0; i < 9; ++i) {
                    Particle &p = puff(Puff::Ink, fish.pos + QPointF(rnd(-8, 8), rnd(-8, 8)), rnd(1.4, 2.2), rnd(6, 10));
                    p.vel = QPointF(rnd(-25, 25), rnd(-25, 25));
                    p.color = QColor(50, 30, 80);
                }
            }
            fish.goal = QPointF(std::clamp(head.x() + side * 360, area.left(), area.right()), head.y() - 120);
            fish.speed = 420;
            fish.hasGaze = false;
        }
        break;
    case Trick::LegCount: {
        const int i = std::min(7, int(u * 8.4));
        const double x = body.left() + body.width() * (0.06 + 0.88 * i / 7.0);
        fish.goal = QPointF(x, foot.y() - 26);
        fish.snap = true;
        fish.gaze = QPointF(x, foot.y());
        if (act.step != i) {
            act.step = i;
            Particle &p = puff(Puff::Text, QPointF(x, foot.y() - 6), 0.9, 10);
            p.text = QString::number(i + 1);
            p.rise = 20;
            p.color = QColor(255, 255, 200);
        }
        if (u > 0.95 && once(act, 0)) {
            say(fish, QStringLiteral("Eight legs!"), 1.4);
            cue(Cue::Nod);
        }
        break;
    }
    case Trick::Photo:
        fish.goal = QPointF(head.x() + side * 120, head.y() - 10);
        fish.snap = true;
        if (u > 0.15 && once(act, 0)) {
            cue(Cue::Wave);
            say(fish, QStringLiteral("Say cheese!"), 1.0);
        }
        if (u > 0.5 && once(act, 1)) {
            puff(Puff::Flash, fish.pos + QPointF(-side * 10, 0), 0.35, 40);
            Particle &p = puff(Puff::Photo, fish.pos + QPointF(0, -40), 2.8, 46);
            p.image = m_pet.image;
            p.rise = 12;
            p.vel = QPointF(-side * 10, -20);
        }
        break;
    case Trick::LeapFrog: {
        const QPointF start(head.x() + side * 90, foot.y() - 18);
        const QPointF end(head.x() - side * 90, foot.y() - 18);
        fish.goal = mix(start, end, ease(u)) + QPointF(0, -std::sin(u * kPi) * (body.height() + 50));
        fish.snap = true;
        fish.gaze = fish.goal + QPointF(-side * 40, 0);
        if (u > 0.5 && once(act, 0))
            cue(Cue::Surprise);
        if (u > 0.95 && once(act, 1)) {
            cue(Cue::Nod);
            bubbles(end, 5, 8);
        }
        break;
    }
    case Trick::SilkSwing: {
        const QPointF pivot(head.x(), std::max(12.0, body.top() - 190));
        const double a = std::sin(t * 2.4) * 0.9 * std::min(1.0, u * 3);
        const double length = std::max(60.0, (body.top() - 30) - pivot.y());
        act.memo = pivot;
        fish.goal = pivot + QPointF(std::sin(a), std::cos(a)) * length;
        fish.snap = u > 0.08;
        if (u > 0.1 && once(act, 0))
            cue(Cue::Eureka);
        break;
    }
    case Trick::CopyDance:
        if (once(act, 0))
            cue(Cue::Dance);
        fish.goal = QPointF(head.x() + side * 84 + std::sin(t * 2 * kPi * 2) * 10, head.y() - 10 - std::abs(std::sin(t * kPi * 2)) * 14);
        fish.snap = true;
        fish.tilt = std::sin(t * 2 * kPi) * 0.35;
        if (every(0.5))
            puff(Puff::Sparkle, fish.pos + QPointF(rnd(-16, 16), rnd(-16, 0)), 0.6, 4).color = QColor(255, 230, 255);
        break;
    case Trick::Spotlight:
        fish.goal = QPointF(head.x() + side * 70, body.top() - 40 + std::sin(t * 2) * 4);
        fish.snap = true;
        fish.lantern = true;
        if (u > 0.4 && once(act, 0))
            cue(Cue::Eureka);
        break;
    case Trick::Balloon:
        fish.goal = QPointF(head.x() + side * 36, body.top() - 76 + std::sin(t * 2) * 6);
        fish.snap = true;
        if (u > 0.35 && once(act, 0)) {
            cue(Cue::Hop, QPointF(0, -560));
            cue(Cue::Giggle);
        }
        if (u > 0.95 && once(act, 1)) {
            cue(Cue::Surprise);
            for (int i = 0; i < 8; ++i)
                puff(Puff::Sparkle, QPointF(head.x() + 10 + rnd(-16, 16), body.top() - 74 + rnd(-16, 16)), 0.6, 5).color =
                    QColor(200, 245, 255);
        }
        break;
    default:
        break;
    }
}

void Scene::swim(Fish &fish, double dt)
{
    const QPointF before = fish.pos;
    if (!fish.leaving && !fish.entering && m_size.width() > fish.length * 2 && m_size.height() > fish.length * 2) {
        const double L = fish.length;
        fish.goal.setX(std::clamp(fish.goal.x(), L * 0.6, m_size.width() - L * 0.6));
        fish.goal.setY(std::clamp(fish.goal.y(), L * 0.45, std::max(L * 0.45, floorY() - L * 0.25)));
    }
    const QPointF to = fish.goal - fish.pos;
    const double distance = std::hypot(to.x(), to.y());
    if (fish.snap) {
        fish.pos += to * std::min(1.0, dt * 10);
        fish.vel = dt > 0 ? (fish.pos - before) / dt : QPointF();
    } else {
        const QPointF desired = distance > 0.5 ? to / distance * fish.speed * std::min(1.0, distance / 60) : QPointF();
        fish.vel += (desired - fish.vel) * std::min(1.0, dt * 3.0);
        fish.pos += fish.vel * dt;
    }
    const double speed = std::hypot(fish.vel.x(), fish.vel.y());
    if (std::abs(fish.vel.x()) > 10)
        fish.heading = fish.vel.x() < 0 ? -1 : 1;
    else if (fish.hasGaze && std::abs(fish.gaze.x() - fish.pos.x()) > 6)
        fish.heading = fish.gaze.x() < fish.pos.x() ? -1 : 1;
    fish.facing += (fish.heading - fish.facing) * std::min(1.0, dt * 7);
    const bool held = fish.act && fish.act->performing &&
                      (fish.act->trick == Trick::Surf || fish.act->trick == Trick::DanceParty ||
                       fish.act->trick == Trick::Juggle || fish.act->trick == Trick::CopyDance);
    if (!held) {
        const double tilt = std::clamp(std::atan2(fish.vel.y(), std::abs(fish.vel.x()) + 40) * 0.7, -0.6, 0.6);
        fish.tilt += (tilt - fish.tilt) * std::min(1.0, dt * 6);
    }
    fish.tail += dt * (5 + speed / 16) * fish.wag;
    fish.mouth = std::max(0.0, fish.mouth - dt * 2);
    if (!fish.act || !fish.act->performing || (fish.act->trick != Trick::Peekaboo && fish.act->trick != Trick::HideAndPeek &&
                                                fish.act->trick != Trick::Fetch)) {
        if (!(fish.act && !fish.act->performing && fish.act->trick == Trick::Peekaboo))
            fish.alpha += (1 - fish.alpha) * std::min(1.0, dt * 4);
    }
    if (!fish.act)
        fish.puffed = std::max(0.0, fish.puffed - dt * 0.5);
}

void Scene::think(Fish &fish, double dt)
{
    const QRectF area = tank();
    if (fish.leaving) {
        fish.goal = QPointF(fish.pos.x(), -90);
        fish.speed = 160;
        return;
    }
    if (fish.entering) {
        fish.speed = 140;
        if (fish.pos.y() > 50)
            fish.entering = false;
        return;
    }
    if (fish.act) {
        perform(fish, dt);
        return;
    }
    fish.snap = false;
    if (m_time >= fish.nextTrick && playful(fish)) {
        if (pick(fish))
            return;
        fish.nextTrick = m_time + 2;
    }
    if (fish.mood == Mood::Waiting && m_time >= fish.nextTrick) {
        if (m_random.generateDouble() < 0.5 && pick(fish))
            return;
        fish.nextTrick = m_time + rnd(3, 6);
    }
    if (m_time >= fish.wanderUntil) {
        double top = area.top() + 20;
        double bottom = area.bottom();
        double slow = 55;
        double fast = 110;
        double rest = 2.5;
        double restMore = 5.5;
        double x = rnd(area.left() + 20, area.right() - 20);
        switch (fish.mood) {
        case Mood::Waiting:
            top = area.top();
            bottom = area.top() + area.height() * 0.4;
            break;
        case Mood::Done:
        case Mood::Merged:
            slow = 40;
            fast = 80;
            rest = 3;
            restMore = 7;
            break;
        case Mood::Queued:
            top = area.top() + area.height() * 0.3;
            bottom = area.top() + area.height() * 0.7;
            slow = 25;
            fast = 45;
            rest = 4;
            restMore = 8;
            break;
        case Mood::Failed:
            top = area.bottom() - area.height() * 0.25;
            slow = 20;
            fast = 40;
            rest = 5;
            restMore = 9;
            break;
        case Mood::Stopped:
            top = bottom = floorY() - bodyHeight(fish.species, fish.length, 0) * 0.55 - 3;
            slow = 14;
            fast = 24;
            rest = 12;
            restMore = 25;
            x = std::clamp(fish.pos.x() + rnd(-120, 120), area.left() + 20, area.right() - 20);
            break;
        default:
            break;
        }
        fish.goal = QPointF(x, rnd(top, std::max(top, bottom)));
        fish.speed = rnd(slow, fast);
        fish.wanderUntil = m_time + rnd(rest, restMore);
    }
    if (fish.mood == Mood::Waiting && fish.sayUntil < m_time && m_random.generateDouble() < dt * 0.25)
        say(fish, QStringLiteral("I need you!"), 2.2);
    if (fish.mood == Mood::Stopped) {
        fish.sleepy = true;
        fish.wag = 0.3;
        if (floorY() - fish.pos.y() > 80)
            fish.speed = 70;
    } else if (fish.mood == Mood::Failed) {
        fish.roll += (0.32 - fish.roll) * std::min(1.0, dt * 2);
        fish.wag = 0.6;
    }
    if (m_time < fish.nextMurmur)
        return;
    fish.nextMurmur = m_time + rnd(2.5, 4.5);
    if (fish.mood == Mood::Stopped) {
        Particle &z = puff(Puff::Zzz, fish.pos + QPointF(fish.heading * 8, -fish.length * 0.4), 2.2, 9);
        z.vel = QPointF(fish.heading * 6, -14);
    } else if (fish.mood == Mood::Merged) {
        const double r = fish.length * 0.4;
        puff(Puff::Sparkle, fish.pos + QPointF(rnd(-r, r), rnd(-r * 0.6, r * 0.6)), 0.8, 4).color = QColor(215, 180, 255);
        fish.nextMurmur = m_time + rnd(1.2, 2.4);
    }
    if (fish.sayUntil >= m_time || m_random.generateDouble() > 0.3)
        return;
    if (fish.conflicted) {
        static const QStringList lines = {QStringLiteral("Merge conflict!"), QStringLiteral("I'm snagged!"),
                                          QStringLiteral("Help me untangle this")};
        say(fish, lines.at(m_random.bounded(int(lines.size()))), 2.2);
        fish.vel += QPointF(0, -45);
    } else if (fish.mood == Mood::Failed) {
        static const QStringList lines = {QStringLiteral("Oops…"), QStringLiteral("That didn't work"),
                                          QStringLiteral("Can you take a look?")};
        say(fish, lines.at(m_random.bounded(int(lines.size()))), 2.2);
        bubbles(mouthOf(fish), 1, 2);
    }
}

void Scene::ambient(double dt)
{
    if (m_size.isEmpty())
        return;
    if (m_time >= m_nextBubble) {
        m_nextBubble = m_time + rnd(0.25, 0.7);
        const double sources[] = {0.12, 0.47, 0.86};
        const double x = m_size.width() * sources[m_random.bounded(3)] + rnd(-6, 6);
        Particle &p = puff(Puff::Bubble, QPointF(x, floorY() - 8), 12, rnd(1.5, 4.5));
        p.rise = rnd(20, 45);
        p.vel = QPointF(0, -20);
    }
    for (int i = int(m_particles.size()) - 1; i >= 0; --i) {
        Particle &p = m_particles[i];
        p.age += dt;
        p.vel.ry() -= p.rise * dt;
        if (p.kind == Puff::Bubble) {
            p.vel.rx() *= 1 - std::min(1.0, dt * 1.5);
            p.pos.rx() += std::sin(p.age * 5 + p.size) * 0.25;
            p.vel.setY(std::max(p.vel.y(), -140.0));
        } else if (p.kind == Puff::Glyph || p.kind == Puff::Note || p.kind == Puff::Heart || p.kind == Puff::Zzz) {
            p.vel *= 1 - std::min(1.0, dt * 0.8);
        } else if (p.kind == Puff::Ink) {
            p.vel *= 1 - std::min(1.0, dt * 2);
            p.size += dt * 9;
        }
        p.pos += p.vel * dt;
        if ((p.kind == Puff::Pearl || p.kind == Puff::Crumb || p.kind == Puff::Sand) && p.pos.y() > floorY() - p.size) {
            p.pos.setY(floorY() - p.size);
            p.vel = QPointF();
            p.rise = 0;
        }
        if (p.kind == Puff::Bubble && p.pos.y() < 4)
            p.age = p.life;
        if (p.age >= p.life)
            m_particles.removeAt(i);
    }
}

void Scene::step(double dt)
{
    dt = std::clamp(dt, 0.0, 0.1);
    m_time += dt;
    ambient(dt);
    for (Fish &fish : m_fish) {
        think(fish, dt);
        swim(fish, dt);
        if (!fish.leaving && !fish.entering && !(fish.act && fish.act->performing)) {
            const QRectF area = tank().adjusted(-20, -30, 20, 10);
            fish.pos.setX(std::clamp(fish.pos.x(), area.left(), area.right()));
            fish.pos.setY(std::clamp(fish.pos.y(), area.top(), area.bottom()));
        }
    }
    for (int i = int(m_fish.size()) - 1; i >= 0; --i) {
        if (m_fish.at(i).leaving && m_fish.at(i).pos.y() < -40) {
            if (m_petFish == m_fish.at(i).id)
                m_petFish = -1;
            m_fish.removeAt(i);
        }
    }
    if (m_pet.present && m_time >= m_lookAt) {
        m_lookAt = m_time + 0.15;
        const Fish *target = nullptr;
        double best = 520;
        for (const Fish &fish : std::as_const(m_fish)) {
            if (fish.id == m_petFish) {
                target = &fish;
                break;
            }
            const QPointF d = fish.pos - petHead();
            const double distance = std::hypot(d.x(), d.y());
            if (distance < best) {
                best = distance;
                target = &fish;
            }
        }
        const bool hiding = target && target->act && target->act->trick == Trick::HideBehind;
        if (target && !hiding)
            cue(Cue::Look, target->pos);
    }
}

QRegion Scene::dirtyRegion()
{
    QList<QRectF> now;
    const double w = m_size.width();
    now.append(QRectF(0, 0, w, 26));
    const double stems[] = {0.03, 0.06, 0.2, 0.47, 0.5, 0.78, 0.93, 0.965};
    for (const double stem : stems)
        now.append(QRectF(w * stem - 34, floorY() - std::min(170.0, m_size.height() * 0.3) - 10, 68,
                          std::min(170.0, m_size.height() * 0.3) + 20));
    if (m_pet.present)
        now.append(m_pet.frame.adjusted(-14, -14, 14, 14));
    for (const Fish &fish : m_fish) {
        const double L = fish.length;
        const double reach = std::max(fish.sayUntil > m_time ? 140.0 : 130.0, L * 1.7);
        now.append(QRectF(fish.pos.x() - reach, fish.pos.y() - L - 56, 2 * reach, 2 * L + 92));
        if (shadowed(fish))
            now.append(QRectF(fish.pos.x() - L * 0.6, floorY() - 4, L * 1.2, 12));
        if (fish.conflicted && !fish.leaving) {
            const QPointF top = lineTop(fish);
            const QPointF mouth = mouthOf(fish);
            now.append(QRectF(std::min(top.x(), mouth.x()) - 4, 0, std::abs(top.x() - mouth.x()) + 8, std::max(0.0, mouth.y()) + 8));
        }
        if (fish.act && fish.act->performing) {
            if (petTrick(fish.act->trick))
                now.append(m_pet.frame.adjusted(-200, -240, 200, 40));
            else
                now.append(fish.act->mark.rect.adjusted(-70, -90, 70, 70));
            for (const Landmark &mark : fish.act->marks)
                now.append(mark.rect.adjusted(-20, -60, 20, 20));
        }
    }
    for (const Particle &particle : m_particles) {
        const double reach = particle.size * 2.5 + 8;
        QRectF area(particle.pos - QPointF(std::max(44.0, reach), reach + 6), QSizeF(2 * std::max(44.0, reach), 2 * reach + 12));
        if (particle.kind == Puff::Ring)
            area = particle.rect.adjusted(-particle.life * 20 - 4, -particle.life * 12 - 4, particle.life * 20 + 4,
                                          particle.life * 12 + 4);
        else if (particle.kind == Puff::Streak)
            area = area.united(particle.rect.translated(particle.rect.left() < 0 ? particle.pos : QPointF()).adjusted(-4, -4, 4, 4));
        else if (particle.kind == Puff::Photo || particle.kind == Puff::Ink || particle.kind == Puff::Flash)
            area = QRectF(particle.pos - QPointF(60, 60), QSizeF(120, 120));
        now.append(area);
    }
    QRegion region;
    QRectF bound;
    const QRectF screen(QPointF(), m_size);
    QList<QRectF> all = m_lastDirty + now;
    for (const QRectF &rect : std::as_const(all))
        bound = bound.united(rect);
    if (all.size() > 160) {
        region = QRegion(bound.intersected(screen).toAlignedRect());
    } else {
        for (const QRectF &rect : std::as_const(all))
            region += rect.intersected(screen).toAlignedRect();
    }
    m_lastDirty = now;
    return region;
}

void Scene::buildComposite()
{
    if (m_size.isEmpty()) {
        m_composite = QImage();
        return;
    }
    const qreal ratio = m_backdrop.isNull() ? 1.0 : m_backdrop.devicePixelRatio();
    m_composite = QImage((m_size * ratio).toSize(), QImage::Format_RGB32);
    m_composite.setDevicePixelRatio(ratio);
    m_composite.fill(QColor(12, 50, 80));
    QPainter p(&m_composite);
    p.setRenderHint(QPainter::Antialiasing);
    if (!m_backdrop.isNull())
        p.drawImage(QRectF(QPointF(), m_size), m_backdrop);
    paintWater(p);
}

void Scene::paintWater(QPainter &p) const
{
    const double w = m_size.width();
    const double h = m_size.height();
    QLinearGradient tint(0, 0, 0, h);
    tint.setColorAt(0, QColor(60, 190, 220, 70));
    tint.setColorAt(0.5, QColor(20, 110, 170, 105));
    tint.setColorAt(1, QColor(6, 40, 90, 150));
    p.fillRect(QRectF(0, 0, w, h), tint);
    QLinearGradient surface(0, 0, 0, 14);
    surface.setColorAt(0, QColor(220, 250, 255, 110));
    surface.setColorAt(1, QColor(220, 250, 255, 0));
    p.fillRect(QRectF(0, 0, w, 14), surface);

    p.setPen(Qt::NoPen);
    for (int i = 0; i < 4; ++i) {
        const double x = w * (0.15 + 0.23 * i) + std::sin(m_time * 0.15 + i * 2) * 60;
        QLinearGradient ray(0, 0, 0, h * 0.8);
        ray.setColorAt(0, QColor(255, 255, 240, 34));
        ray.setColorAt(1, QColor(255, 255, 240, 0));
        p.setBrush(ray);
        QPolygonF beam;
        beam << QPointF(x - 30, 0) << QPointF(x + 30, 0) << QPointF(x + 130 + 40 * std::sin(m_time * 0.2 + i), h * 0.8)
             << QPointF(x - 10 + 40 * std::sin(m_time * 0.2 + i), h * 0.8);
        p.drawPolygon(beam);
    }

    QRandomGenerator stones(7);
    QPainterPath sand;
    sand.moveTo(0, h);
    sand.lineTo(0, floorY() + 2);
    for (double x = 0; x <= w + 40; x += 40)
        sand.quadTo(x + 20, floorY() - 3 + stones.bounded(5), x + 40, floorY() + stones.bounded(4));
    sand.lineTo(w, h);
    sand.closeSubpath();
    QLinearGradient sandFill(0, floorY() - 4, 0, h);
    sandFill.setColorAt(0, QColor(236, 214, 160, 235));
    sandFill.setColorAt(1, QColor(190, 160, 105, 245));
    p.setPen(Qt::NoPen);
    p.setBrush(sandFill);
    p.drawPath(sand);
    for (int i = 0; i < int(w / 9); ++i) {
        p.setBrush(QColor(150 + stones.bounded(60), 130 + stones.bounded(50), 90 + stones.bounded(50), 200));
        const double x = stones.generateDouble() * w;
        const double y = floorY() + 3 + stones.generateDouble() * (kSand - 5);
        p.drawEllipse(QPointF(x, y), 1.2 + stones.generateDouble() * 1.6, 1 + stones.generateDouble());
    }
    for (int i = 0; i < 6; ++i) {
        const double x = stones.generateDouble() * w;
        const double r = 6 + stones.generateDouble() * 10;
        QRadialGradient rock(QPointF(x - r * 0.3, floorY() - r * 0.4), r * 1.3);
        rock.setColorAt(0, QColor(150, 150, 160, 240));
        rock.setColorAt(1, QColor(70, 75, 90, 240));
        p.setBrush(rock);
        p.drawEllipse(QPointF(x, floorY() + 2), r * 1.3, r * 0.8);
    }

    const QRectF chest(w * 0.12 - 18, floorY() - 22, 36, 24);
    p.setBrush(QColor(120, 72, 36, 245));
    p.drawRoundedRect(chest, 3, 3);
    p.setBrush(QColor(150, 92, 46, 245));
    QPainterPath lid;
    lid.moveTo(chest.left(), chest.top() + 6);
    lid.quadTo(chest.center().x(), chest.top() - 10, chest.right(), chest.top() + 6);
    lid.closeSubpath();
    p.drawPath(lid);
    p.setBrush(QColor(240, 200, 80, 245));
    p.drawRect(QRectF(chest.left(), chest.top() + 5, chest.width(), 3));
    p.drawRect(QRectF(chest.center().x() - 3, chest.top() + 8, 6, 7));
    p.setBrush(QColor(255, 230, 120, 200));
    for (int i = 0; i < 4; ++i)
        p.drawEllipse(QPointF(chest.left() + 7 + i * 7, chest.top() + 3), 2.4, 2.4);
}

void Scene::paintWeeds(QPainter &p) const
{
    const double w = m_size.width();
    const double stems[] = {0.03, 0.06, 0.2, 0.47, 0.5, 0.78, 0.93, 0.965};
    const double heights[] = {150, 110, 80, 70, 120, 90, 160, 120};
    for (int i = 0; i < 8; ++i) {
        const double x = w * stems[i];
        const double height = std::min(heights[i], m_size.height() * 0.3);
        QPolygonF left;
        QPolygonF right;
        const int segments = 10;
        for (int s = 0; s <= segments; ++s) {
            const double v = double(s) / segments;
            const double sway = std::sin(m_time * 1.1 + i * 1.7 + v * 2.5) * v * v * 22;
            const double width = 5 * (1 - v) + 1;
            const QPointF at(x + sway, floorY() + 4 - v * height);
            left << at - QPointF(width, 0);
            right.prepend(at + QPointF(width, 0));
        }
        QPolygonF ribbon = left + right;
        QLinearGradient green(0, floorY(), 0, floorY() - height);
        const int hue = i % 3 == 0 ? 130 : i % 3 == 1 ? 100 : 160;
        green.setColorAt(0, QColor::fromHsv(hue, 200, 110, 225));
        green.setColorAt(1, QColor::fromHsv(hue, 170, 200, 200));
        p.setPen(Qt::NoPen);
        p.setBrush(green);
        p.drawPolygon(ribbon);
    }
}

void Scene::paint(QPainter &p) const
{
    if (m_size.isEmpty())
        return;
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::SmoothPixmapTransform);
    const double w = m_size.width();
    if (m_fade < 1 && !m_backdrop.isNull())
        p.drawImage(QRectF(QPointF(), m_size), m_backdrop);
    p.setOpacity(clamp01(m_fade));
    if (!m_composite.isNull())
        p.drawImage(QRectF(QPointF(), m_size), m_composite);

    p.save();
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 14; ++i) {
        const double x = std::fmod(i * 137.0 + m_time * (12 + i % 4 * 5), w + 120) - 60;
        const double y = 6 + std::sin(m_time * 0.9 + i) * 3;
        p.setBrush(QColor(255, 255, 255, 30));
        p.drawEllipse(QPointF(x, y), 36 + (i % 3) * 14, 4);
    }
    p.restore();
    paintWeeds(p);

    for (const Fish &fish : m_fish) {
        if (fish.behind)
            paintFish(p, fish);
    }
    for (const Fish &fish : m_fish)
        paintActBehind(p, fish);
    paintPet(p);
    for (const Fish &fish : m_fish) {
        if (!fish.behind)
            paintFish(p, fish);
        paintActFront(p, fish);
    }
    for (const Particle &particle : m_particles)
        paintParticle(p, particle);
    for (const Fish &fish : m_fish)
        paintLabel(p, fish);
    if (m_fish.isEmpty()) {
        QFont font = p.font();
        font.setPixelSize(13);
        p.setFont(font);
        p.setPen(QColor(230, 250, 255, 200));
        p.drawText(QRectF(0, floorY() - 60, w, 24), Qt::AlignCenter,
                   QStringLiteral("No agents yet. The tank is resting."));
    }
    p.setOpacity(1);
}

void Scene::paintPet(QPainter &p) const
{
    if (!m_pet.present || m_pet.image.isNull())
        return;
    QRadialGradient glow(QPointF(m_pet.body.center().x(), m_pet.body.bottom() + 2), m_pet.body.width() * 0.6);
    glow.setColorAt(0, QColor(64, 242, 219, 70));
    glow.setColorAt(1, QColor(64, 242, 219, 0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(QPointF(m_pet.body.center().x(), m_pet.body.bottom() + 2), m_pet.body.width() * 0.6,
                  m_pet.body.width() * 0.25);
    p.drawImage(m_pet.frame.topLeft(), m_pet.image);
}

void Scene::paintActBehind(QPainter &p, const Fish &fish) const
{
    if (!fish.act || !fish.act->performing)
        return;
    const Act &act = *fish.act;
    const QRectF r = act.mark.rect;
    const double u = clamp01(act.t / act.length);
    p.save();
    switch (act.trick) {
    case Trick::Lantern: {
        const QPointF at(fish.pos.x() + fish.facing * fish.length * 0.5, r.center().y());
        QRadialGradient light(at, r.height() * 1.4 + 34);
        light.setColorAt(0, QColor(255, 236, 150, 120));
        light.setColorAt(1, QColor(255, 236, 150, 0));
        p.setCompositionMode(QPainter::CompositionMode_Screen);
        p.setPen(Qt::NoPen);
        p.setBrush(light);
        p.drawEllipse(at, r.height() * 1.4 + 34, r.height() * 1.4 + 34);
        break;
    }
    case Trick::Highlight: {
        QColor color = fish.accent;
        color.setAlpha(110);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawRoundedRect(QRectF(r.left() - 3, r.top() - 1, std::max(0.0, fish.pos.x() - r.left() + 3), r.height() + 2), 4, 4);
        break;
    }
    case Trick::Measure: {
        const double x = std::min(fish.pos.x(), r.right());
        QPen pen(QColor(255, 255, 210, 230), 1.4, Qt::DashLine);
        p.setPen(pen);
        p.drawLine(QPointF(r.left(), r.top() - 4), QPointF(x, r.top() - 4));
        p.setPen(QPen(QColor(255, 255, 210, 230), 1.4));
        p.drawLine(QPointF(r.left(), r.top() - 9), QPointF(r.left(), r.top() + 1));
        if (u > 0.78) {
            p.drawLine(QPointF(r.right(), r.top() - 9), QPointF(r.right(), r.top() + 1));
            QFont font = p.font();
            font.setPixelSize(11);
            font.setBold(true);
            p.setFont(font);
            const QString label = QStringLiteral("%1 px").arg(int(std::lround(r.width())));
            const QRectF box(r.center().x() - 32, r.top() - 34, 64, 18);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(10, 40, 70, 200));
            p.drawRoundedRect(box, 9, 9);
            p.setPen(QColor(255, 255, 220));
            p.drawText(box, Qt::AlignCenter, label);
        }
        break;
    }
    case Trick::Squeegee: {
        const double x = act.memo.x();
        const QRectF clean(r.left(), r.top(), std::max(0.0, x - r.left()), r.height());
        if (!m_backdrop.isNull() && clean.width() > 1) {
            const qreal ratio = m_backdrop.devicePixelRatio();
            p.drawImage(clean, m_backdrop, QRectF(clean.topLeft() * ratio, clean.size() * ratio));
            p.setPen(QPen(QColor(255, 255, 255, 200), 2));
            p.drawLine(QPointF(clean.right(), clean.top()), QPointF(clean.right(), clean.bottom()));
        }
        break;
    }
    case Trick::Carry:
    case Trick::Wobble:
    case Trick::Trampoline: {
        if (act.piece.isNull())
            break;
        const bool moved = act.trick == Trick::Carry ? (act.memo.manhattanLength() > 0.5)
                         : act.trick == Trick::Wobble ? std::abs(act.memo.x()) > 0.001
                                                       : act.memo.x() > 0.5;
        if (!moved)
            break;
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(8, 50, 80, 190));
        p.drawRoundedRect(r, 4, 4);
        if (act.trick == Trick::Carry) {
            const QRectF lifted = r.translated(act.memo);
            p.setBrush(QColor(0, 0, 0, 70));
            p.drawRoundedRect(lifted.translated(4, 6), 4, 4);
            p.drawImage(lifted, act.piece);
        } else if (act.trick == Trick::Wobble) {
            p.translate(r.center());
            p.rotate(act.memo.x() * 180 / kPi);
            p.drawImage(QRectF(-r.width() / 2, -r.height() / 2, r.width(), r.height()), act.piece);
        } else {
            const QRectF squashed(r.left() - 3, r.top() + r.height() * 0.15, r.width() + 6, r.height() * 0.85);
            p.drawImage(squashed, act.piece);
        }
        break;
    }
    default:
        break;
    }
    p.restore();
}

void Scene::paintActFront(QPainter &p, const Fish &fish) const
{
    if (!fish.act || !fish.act->performing)
        return;
    const Act &act = *fish.act;
    const QRectF r = act.mark.rect;
    const double u = clamp01(act.t / act.length);
    const double t = act.t;
    p.save();
    switch (act.trick) {
    case Trick::Mirror: {
        p.setClipRect(r);
        paintGhostFish(p, fish, QPointF(r.left() + fish.length * 0.55, fish.pos.y()), -1, 0.4);
        break;
    }
    case Trick::Magnify: {
        const QPointF lens = act.memo;
        const double radius = 26;
        p.setPen(QPen(QColor(120, 80, 40), 4, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(lens + QPointF(-radius * 0.7, radius * 0.7), mouthOf(fish));
        if (!m_backdrop.isNull()) {
            QPainterPath circle;
            circle.addEllipse(lens, radius, radius);
            p.save();
            p.setClipPath(circle);
            const qreal ratio = m_backdrop.devicePixelRatio();
            const double source = radius / 1.9;
            p.drawImage(QRectF(lens - QPointF(radius, radius), QSizeF(2 * radius, 2 * radius)), m_backdrop,
                        QRectF((lens - QPointF(source, source)) * ratio, QSizeF(2 * source, 2 * source) * ratio));
            p.restore();
        }
        p.setBrush(QColor(255, 255, 255, 30));
        p.setPen(QPen(QColor(230, 200, 120), 3));
        p.drawEllipse(lens, radius, radius);
        break;
    }
    case Trick::Fetch:
        if (u > 0.5 && u < 0.92) {
            const QString text = act.mark.text.simplified();
            QFont font = p.font();
            font.setPixelSize(15);
            font.setBold(true);
            p.setFont(font);
            p.setPen(QColor(255, 255, 230));
            p.drawText(QRectF(mouthOf(fish) - QPointF(9, 6), QSizeF(18, 18)), Qt::AlignCenter,
                       text.isEmpty() ? QStringLiteral("A") : text.left(1).toUpper());
        }
        break;
    case Trick::Crown: {
        const int count = std::min(5, std::max(0, act.step));
        for (int i = 0; i < count; ++i) {
            const QPointF at = petHead() + QPointF(-24 + 12 * i, -m_pet.body.height() * 0.75 - (i == 2 ? 8 : i % 2 ? 4 : 0));
            bubble(p, at, 4.5, 1);
        }
        break;
    }
    case Trick::Juggle:
        for (int i = 0; i < 3; ++i) {
            const double phase = t * 1.1 + i / 3.0;
            const double x = petHead().x() + std::cos(phase * 2 * kPi) * 22;
            const double y = m_pet.body.top() - 60 - std::abs(std::sin(phase * 2 * kPi)) * 40;
            bubble(p, QPointF(x, y), 6, u > 0.95 ? 0 : 1);
        }
        break;
    case Trick::PlayFetch: {
        const QPointF from = mouthOf(fish);
        const QPointF to(act.memo.x(), floorY() - 7);
        QPointF ball = from;
        double alpha = 1;
        if (u >= 0.15 && u < 0.4) {
            const double v = span(u, 0.15, 0.4);
            ball = mix(QPointF(petHead().x() + act.side * 50, petHead().y() - 20), to, v) + QPointF(0, -std::sin(v * kPi) * 90);
        } else if (u >= 0.4) {
            ball = to;
            alpha = u > 0.75 ? 0 : 1;
        }
        if (alpha > 0) {
            p.setPen(QPen(QColor(255, 255, 255, 220), 1.2));
            p.setBrush(QColor(255, 120, 90, 230));
            p.drawEllipse(ball, 7, 7);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(255, 255, 255, 200));
            p.drawEllipse(ball + QPointF(-2.5, -2.5), 2, 2);
        }
        break;
    }
    case Trick::SilkSwing: {
        const QPointF pivot = act.memo;
        p.setPen(QPen(QColor(240, 250, 255, 210), 1.2));
        p.drawLine(QPointF(petHead().x(), m_pet.body.top() + 6), pivot);
        p.drawLine(pivot, fish.pos + QPointF(0, -fish.length * 0.2));
        break;
    }
    case Trick::Balloon: {
        const QPointF knot(petHead().x(), m_pet.body.top() + 8);
        const QPointF top(petHead().x() + 10, m_pet.body.top() - 74 + std::sin(t * 2) * 6);
        p.setPen(QPen(QColor(240, 250, 255, 210), 1.2));
        QPainterPath string;
        string.moveTo(knot);
        string.quadTo(knot + QPointF(12, -30), top + QPointF(0, 16));
        p.setBrush(Qt::NoBrush);
        p.drawPath(string);
        if (u < 0.95)
            bubble(p, top, 16, 1);
        break;
    }
    case Trick::Spotlight: {
        const QPointF at = m_pet.body.center();
        QRadialGradient light(at, m_pet.body.width() * 0.9);
        light.setColorAt(0, QColor(255, 240, 170, 120));
        light.setColorAt(1, QColor(255, 240, 170, 0));
        p.setCompositionMode(QPainter::CompositionMode_Screen);
        p.setPen(Qt::NoPen);
        p.setBrush(light);
        p.drawEllipse(at, m_pet.body.width() * 0.9, m_pet.body.width() * 0.9);
        QPolygonF cone;
        cone << fish.pos + QPointF(fish.facing * fish.length * 0.5, -fish.length * 0.7)
             << at + QPointF(-m_pet.body.width() * 0.5, 0) << at + QPointF(m_pet.body.width() * 0.5, 0);
        p.setBrush(QColor(255, 240, 170, 40));
        p.drawPolygon(cone);
        break;
    }
    default:
        break;
    }
    p.restore();
}

void Scene::paintGhostFish(QPainter &p, const Fish &fish, const QPointF &at, double facing, double alpha) const
{
    Fish ghost = fish;
    ghost.pos = at;
    ghost.facing = facing;
    ghost.alpha = alpha;
    ghost.act.reset();
    ghost.lantern = false;
    paintFish(p, ghost);
}

void Scene::paintFins(QPainter &p, const Fish &fish, double W, double H, double amp, bool far) const
{
    const double L = fish.length;
    const auto x = [&](double s) { return W / 2 - s * W; };
    const auto pectoral = [&](double s, double phase, double size, const QColor &color) {
        p.save();
        p.translate(x(s), H * 0.14 + flex(fish.tail, amp, s));
        p.rotate(std::sin(fish.tail + phase) * 22 + 22);
        p.scale(size, size);
        QPainterPath path;
        path.moveTo(0, 0);
        path.cubicTo(-L * 0.06, -L * 0.07, -L * 0.17, -L * 0.04, -L * 0.19, L * 0.01);
        path.cubicTo(-L * 0.15, L * 0.05, -L * 0.06, L * 0.05, 0, 0);
        p.setPen(Qt::NoPen);
        p.setBrush(color);
        p.drawPath(path);
        p.restore();
    };
    if (!far) {
        pectoral(0.3, 0, 1, alpha(fish.fin, 175));
        return;
    }
    pectoral(0.32, 1.4, 0.8, alpha(fish.fin.darker(125), 200));
    if (fish.tailSkin.isNull())
        return;
    const QPointF root(x(1), flex(fish.tail, amp, 1.0));
    const double before = flex(fish.tail, amp, 0.9);
    const double after = flex(fish.tail, amp, 1.0);
    const double angle = std::atan2(-(after - before), 0.1 * W) + std::sin(fish.tail - 3.1) * 0.3 * amp / std::max(1.0, H * 0.16);
    p.save();
    p.translate(root);
    p.rotate(angle * 180 / kPi);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.drawImage(fish.tailRect, fish.tailSkin);
    p.restore();
}

void Scene::paintFish(QPainter &p, const Fish &fish) const
{
    if (fish.skin.isNull())
        return;
    const double L = fish.length;
    const double flip = fish.facing < 0 ? -std::max(0.2, std::abs(fish.facing)) : std::max(0.2, std::abs(fish.facing));
    const double H0 = bodyHeight(fish.species, L, 0);
    const double H = bodyHeight(fish.species, L, fish.puffed);
    const double bodyW = bodyWidth(fish.species, L);
    const double stretch = H / H0;
    const double speed = std::hypot(fish.vel.x(), fish.vel.y());
    double amp = H0 * 0.16 * (0.55 + 0.45 * std::min(1.0, speed / 110));
    if (fish.sleepy || fish.mood == Mood::Stopped)
        amp *= 0.3;
    else if (fish.mood == Mood::Failed)
        amp *= 0.6;
    const double opacity = p.opacity() * clamp01(fish.alpha) * fish.presence;

    p.save();
    p.setOpacity(opacity);
    p.setPen(Qt::NoPen);
    if (shadowed(fish)) {
        const double near = 1 - (floorY() - fish.pos.y()) / 90;
        p.setBrush(QColor(20, 30, 40, int(70 * near)));
        p.drawEllipse(QPointF(fish.pos.x(), floorY() + 3), L * 0.45 * (0.6 + 0.4 * near), 3.5);
    }
    if (fish.conflicted && !fish.leaving) {
        const double pulse = 0.55 + 0.45 * std::sin(m_time * 3.2 + fish.id);
        QRadialGradient glow(fish.pos, L * 0.85);
        glow.setColorAt(0, QColor(255, 60, 70, 110));
        glow.setColorAt(1, QColor(255, 60, 70, 0));
        p.setOpacity(opacity * pulse);
        p.setBrush(glow);
        p.drawEllipse(fish.pos, L * 0.85, L * 0.6);
        p.setOpacity(opacity);
        if (!m_size.isEmpty()) {
            p.setPen(QPen(QColor(205, 222, 232), 1.0));
            p.drawLine(lineTop(fish), mouthOf(fish));
        }
    }
    p.restore();

    p.save();
    p.setOpacity(opacity);
    p.translate(fish.pos);
    p.scale(flip * fish.scale, fish.scale);
    p.rotate((fish.tilt + fish.roll) * 180 / kPi);
    p.setPen(Qt::NoPen);

    paintFins(p, fish, bodyW, H, amp, true);

    const QRectF r = fish.skinRect;
    const double ratio = fish.skin.devicePixelRatio();
    p.setRenderHint(QPainter::Antialiasing, false);
    const double cuts[] = {0.0, 0.45, 0.7, 0.88, 1.0};
    const double lastCut = (bodyW / 2 - r.left()) / bodyW;
    const auto bendAt = [&](double x) { return flex(fish.tail, amp, clamp01((bodyW / 2 - x) / bodyW)); };
    for (int i = 0; i + 1 < int(std::size(cuts)); ++i) {
        const double s0 = i == 0 ? -1.0 : cuts[i];
        const double s1 = i + 2 == int(std::size(cuts)) ? lastCut : cuts[i + 1];
        const double right = std::min(r.right(), bodyW / 2 - s0 * bodyW);
        const double left = std::max(r.left(), bodyW / 2 - s1 * bodyW);
        if (right <= left)
            continue;
        const double slope = (bendAt(right) - bendAt(left)) / (right - left);
        p.save();
        p.setTransform(QTransform(1, slope, 0, 1, 0, bendAt(right) - slope * right), true);
        const QRectF source((left - r.left()) * ratio, 0, (right - left) * ratio, r.height() * ratio);
        p.drawImage(QRectF(left, r.top() * stretch, right - left, r.height() * stretch), fish.skin, source);
        p.restore();
    }

    p.setRenderHint(QPainter::Antialiasing);
    paintFins(p, fish, bodyW, H, amp, false);

    if (fish.species == 3 && fish.puffed > 0.2) {
        p.setPen(QPen(fish.accent, 1.4));
        for (int i = 0; i < 12; ++i) {
            const double a = i * kPi / 6;
            const QPointF base(std::cos(a) * bodyW / 2, std::sin(a) * H / 2);
            p.drawLine(base, base * (1 + 0.25 * fish.puffed));
        }
        p.setPen(Qt::NoPen);
    }

    if (fish.lantern) {
        const QPointF bulb(bodyW * 0.55, -H * 1.0);
        p.setPen(QPen(fish.body.darker(130), 1.4));
        p.setBrush(Qt::NoBrush);
        QPainterPath stalk;
        stalk.moveTo(bodyW * 0.2, -H * 0.42);
        stalk.quadTo(bodyW * 0.3, -H * 1.2, bulb.x(), bulb.y());
        p.drawPath(stalk);
        p.setPen(Qt::NoPen);
        QRadialGradient glow(bulb, 12);
        glow.setColorAt(0, QColor(255, 250, 200, 255));
        glow.setColorAt(0.4, QColor(255, 230, 120, 200));
        glow.setColorAt(1, QColor(255, 230, 120, 0));
        p.setBrush(glow);
        p.drawEllipse(bulb, 12, 12);
    }

    const QPointF eye(bodyW * 0.3, -H * 0.1);
    const double er = std::max(2.8, L * 0.08) * (fish.act && fish.act->trick == Trick::Peek ? 1.2 : 1.0) *
                      (fish.mood == Mood::Waiting ? 1.12 : 1.0);
    p.setPen(Qt::NoPen);
    p.setBrush(alpha(fish.body.darker(140), 200));
    p.drawEllipse(eye, er * 1.2, er * 1.2);
    if (fish.sleepy) {
        p.setBrush(fish.body.darker(115));
        p.drawEllipse(eye, er, er);
        p.setPen(QPen(QColor(30, 30, 40), 1.3, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(eye.x() - er * 0.8, eye.y() - er * 0.5, er * 1.6, er * 1.1), 200 * 16, 140 * 16);
        p.setPen(Qt::NoPen);
    } else {
        p.setBrush(QColor(238, 242, 242));
        p.drawEllipse(eye, er, er);
        QPointF look(1, 0);
        if (fish.hasGaze) {
            const QPointF d = fish.gaze - fish.pos;
            const double n = std::max(1.0, std::hypot(d.x(), d.y()));
            look = QPointF(d.x() / n * (fish.facing < 0 ? -1 : 1), d.y() / n);
        }
        p.setBrush(fish.iris);
        p.drawEllipse(eye + look * er * 0.22, er * 0.8, er * 0.8);
        p.setBrush(QColor(10, 10, 16));
        p.drawEllipse(eye + look * er * 0.32, er * 0.5, er * 0.5);
        p.setBrush(Qt::white);
        p.drawEllipse(eye + QPointF(-er * 0.3, -er * 0.35), er * 0.22, er * 0.22);
        p.setBrush(QColor(255, 255, 255, 170));
        p.drawEllipse(eye + QPointF(er * 0.3, er * 0.3), er * 0.1, er * 0.1);
        if (fish.mood == Mood::Failed) {
            p.setBrush(fish.body.darker(112));
            p.drawChord(QRectF(eye.x() - er * 1.05, eye.y() - er * 1.05, er * 2.1, er * 2.1), 10 * 16, 160 * 16);
        }
    }

    const QPointF mouth(bodyW * 0.48, H * 0.06);
    if (fish.pucker) {
        p.setBrush(QColor(230, 80, 120));
        p.drawEllipse(mouth + QPointF(1, 0), 2.6, 2.2);
    } else if (fish.mouth > 0.2) {
        p.setBrush(QColor(60, 20, 30));
        p.drawEllipse(mouth, 1.8, 1.2 + 1.8 * fish.mouth);
    } else {
        p.setPen(QPen(fish.body.darker(170), 1.1, Qt::SolidLine, Qt::RoundCap));
        p.drawLine(mouth - QPointF(3, fish.mood == Mood::Failed ? -1.5 : -0.5), mouth);
    }
    if (fish.conflicted) {
        p.setPen(QPen(QColor(200, 210, 222), 1.2, Qt::SolidLine, Qt::RoundCap));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(mouth.x() - 1, mouth.y() - 1, 5, 6), 180 * 16, 200 * 16);
        p.drawLine(mouth + QPointF(4, 2), mouth + QPointF(4, -6));
    }
    p.restore();
}

void Scene::paintLabel(QPainter &p, const Fish &fish) const
{
    p.save();
    p.setOpacity(p.opacity() * clamp01(fish.alpha) * std::max(0.85, fish.presence));
    QFont font = p.font();
    font.setPixelSize(10);
    font.setBold(false);
    p.setFont(font);
    const QString name = shorten(fish.name, 20);
    QString detail = fish.mood == Mood::Working ? QString() : moodName(fish.mood);
    if (fish.files > 0)
        detail += (detail.isEmpty() ? QString() : QStringLiteral(" · ")) +
                  (fish.files == 1 ? QStringLiteral("1 file") : QStringLiteral("%1 files").arg(fish.files));
    if (fish.conflicted)
        detail += (detail.isEmpty() ? QString() : QStringLiteral(" · ")) + QStringLiteral("conflict");
    const QString key = QStringLiteral("%1|%2|%3|%4").arg(name, detail).arg(int(fish.mood)).arg(m_ratio);
    if (fish.tagKey != key) {
        fish.tagKey = key;
        const QFontMetricsF metrics(font);
        const double nameW = metrics.horizontalAdvance(name);
        const double detailW = detail.isEmpty() ? 0 : metrics.horizontalAdvance(QStringLiteral(" · ") + detail);
        const double width = std::ceil(nameW + detailW + 22);
        fish.tag = QImage(QSize(int(std::ceil(width * m_ratio)), int(std::ceil(15 * m_ratio))), QImage::Format_ARGB32_Premultiplied);
        fish.tag.setDevicePixelRatio(m_ratio);
        fish.tag.fill(Qt::transparent);
        QPainter t(&fish.tag);
        t.setRenderHint(QPainter::Antialiasing);
        t.setFont(font);
        const QRectF pill(0, 0, width, 15);
        t.setPen(Qt::NoPen);
        t.setBrush(QColor(4, 24, 44, 160));
        t.drawRoundedRect(pill, 7.5, 7.5);
        t.setBrush(moodColor(fish.mood));
        t.drawEllipse(QPointF(pill.left() + 8, pill.center().y()), 3, 3);
        t.setPen(fish.mood == Mood::Waiting ? QColor(255, 210, 120) : QColor(225, 245, 255));
        const QRectF text = pill.adjusted(14, 0, -6, 0);
        t.drawText(text, Qt::AlignLeft | Qt::AlignVCenter, name);
        if (!detail.isEmpty()) {
            t.setPen(fish.conflicted ? QColor(255, 130, 130) : moodColor(fish.mood));
            t.drawText(text.adjusted(nameW, 0, 0, 0), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral(" · ") + detail);
        }
    }
    p.drawImage(QPointF(std::round(fish.pos.x() - fish.tag.deviceIndependentSize().width() / 2),
                        std::round(fish.pos.y() + fish.length * 0.42 + 4)),
                fish.tag);

    const bool asking = fish.mood == Mood::Waiting && !fish.act;
    if (asking || (fish.conflicted && !fish.leaving)) {
        const double pulse = 0.5 + 0.5 * std::sin(m_time * 4);
        const QPointF at = fish.pos + QPointF(0, -fish.length * 0.6 - 8);
        QFont mark = font;
        mark.setPixelSize(11);
        mark.setBold(true);
        p.setFont(mark);
        if (asking) {
            bubble(p, at, 8 + pulse * 1.5, 1);
            p.setPen(QColor(255, 220, 120));
            p.drawText(QRectF(at - QPointF(8, 8), QSizeF(16, 16)), Qt::AlignCenter, QStringLiteral("?"));
        } else {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(225, 50, 64, int(200 + 55 * pulse)));
            p.drawEllipse(at, 7.5, 7.5);
            p.setPen(Qt::white);
            p.drawText(QRectF(at - QPointF(8, 8), QSizeF(16, 16)), Qt::AlignCenter, QStringLiteral("!"));
        }
    }

    if (!fish.say.isEmpty() && m_time < fish.sayUntil) {
        QFont speech = font;
        speech.setPixelSize(12);
        speech.setBold(true);
        p.setFont(speech);
        const QFontMetricsF sm(speech);
        const double w = std::min(260.0, sm.horizontalAdvance(fish.say) + 18);
        double x = fish.pos.x() - w / 2;
        x = std::clamp(x, 6.0, std::max(6.0, m_size.width() - w - 6));
        const double above = fish.pos.y() - fish.length * 0.6 - 34;
        const bool below = above < 4;
        const QRectF box(x, below ? fish.pos.y() + fish.length * 0.42 + 26 : above, w, 22);
        QPainterPath path;
        path.addRoundedRect(box, 11, 11);
        QPolygonF tip;
        const double tx = std::clamp(fish.pos.x(), box.left() + 12, box.right() - 12);
        if (below)
            tip << QPointF(tx - 5, box.top() + 1) << QPointF(tx + 5, box.top() + 1) << QPointF(tx, box.top() - 7);
        else
            tip << QPointF(tx - 5, box.bottom() - 1) << QPointF(tx + 5, box.bottom() - 1) << QPointF(tx, box.bottom() + 7);
        path.addPolygon(tip);
        path.setFillRule(Qt::WindingFill);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 235));
        p.drawPath(path.simplified());
        p.setPen(QColor(20, 40, 60));
        p.drawText(box, Qt::AlignCenter, sm.elidedText(fish.say, Qt::ElideRight, w - 12));
    }
    p.restore();
}

void Scene::paintParticle(QPainter &p, const Particle &particle) const
{
    const double fadeOut = clamp01((particle.life - particle.age) / std::min(0.5, particle.life));
    const double fadeIn = clamp01(particle.age / 0.08);
    const double alpha = fadeOut * fadeIn;
    if (alpha <= 0)
        return;
    p.save();
    p.setPen(Qt::NoPen);
    const QPointF at = particle.pos;
    QColor color = particle.color;
    switch (particle.kind) {
    case Puff::Bubble:
        bubble(p, at, particle.size, alpha);
        break;
    case Puff::Sparkle:
    case Puff::Star:
        color.setAlphaF(alpha * color.alphaF());
        p.setBrush(color);
        star(p, at, particle.size * (particle.kind == Puff::Star ? 1.2 : 1.0) * (1 + 0.3 * std::sin(particle.age * 12)),
             particle.size * 0.35, particle.kind == Puff::Star ? 5 : 4);
        break;
    case Puff::Heart:
        color.setAlphaF(alpha);
        p.setBrush(color);
        heart(p, at, particle.size);
        break;
    case Puff::Note:
    case Puff::Glyph:
    case Puff::Zzz:
    case Puff::Text: {
        QFont font = p.font();
        font.setPixelSize(int(particle.size + (particle.kind == Puff::Zzz ? particle.age * 4 : 0)));
        font.setBold(true);
        p.setFont(font);
        if (particle.kind == Puff::Glyph)
            bubble(p, at, particle.size * 0.85, alpha * 0.8);
        color.setAlphaF(alpha);
        p.setPen(particle.kind == Puff::Zzz ? QColor(220, 240, 255, int(230 * alpha)) : color);
        const QString text = particle.kind == Puff::Note ? QStringLiteral("♪")
                             : particle.kind == Puff::Zzz ? QStringLiteral("z")
                                                          : particle.text;
        p.drawText(QRectF(at - QPointF(40, particle.size), QSizeF(80, particle.size * 2)), Qt::AlignCenter, text);
        break;
    }
    case Puff::Ring: {
        const double grow = particle.age * 18;
        QColor ring = particle.color.isValid() ? particle.color : QColor(Qt::white);
        ring.setAlphaF(alpha * 0.9);
        p.setBrush(Qt::NoBrush);
        p.setPen(QPen(ring, 2));
        const QRectF base = particle.rect.isValid() ? particle.rect : QRectF(at - QPointF(4, 4), QSizeF(8, 8));
        p.drawRoundedRect(base.adjusted(-grow, -grow * 0.6, grow, grow * 0.6), 6, 6);
        break;
    }
    case Puff::Flash: {
        QRadialGradient flash(at, particle.size);
        flash.setColorAt(0, QColor(255, 255, 255, int(255 * alpha)));
        flash.setColorAt(1, QColor(255, 255, 255, 0));
        p.setBrush(flash);
        p.drawEllipse(at, particle.size, particle.size);
        break;
    }
    case Puff::Crumb:
    case Puff::Sand:
        color = particle.kind == Puff::Sand ? QColor(230, 205, 150) : color;
        color.setAlphaF(alpha);
        p.setBrush(color);
        p.drawRect(QRectF(at - QPointF(particle.size / 2, particle.size / 2), QSizeF(particle.size, particle.size)));
        break;
    case Puff::Ink:
        color.setAlphaF(alpha * 0.55);
        p.setBrush(color);
        p.drawEllipse(at, particle.size, particle.size * 0.85);
        break;
    case Puff::Pearl: {
        QRadialGradient pearl(at - QPointF(1.5, 1.5), particle.size * 1.2);
        pearl.setColorAt(0, QColor(255, 255, 255, int(255 * alpha)));
        pearl.setColorAt(1, QColor(200, 200, 230, int(255 * alpha)));
        p.setBrush(QColor(255, 255, 220, int(60 * alpha * (0.6 + 0.4 * std::sin(particle.age * 4)))));
        p.drawEllipse(at, particle.size * 2.4, particle.size * 2.4);
        p.setBrush(pearl);
        p.drawEllipse(at, particle.size, particle.size);
        break;
    }
    case Puff::Streak:
        if (!particle.image.isNull()) {
            p.setOpacity(p.opacity() * alpha);
            p.drawImage(particle.rect, particle.image);
        } else {
            color.setAlphaF(alpha * 0.45);
            p.setBrush(color);
            p.drawRoundedRect(particle.rect.translated(particle.rect.isValid() && particle.rect.left() < 0 ? at : QPointF()), 3, 3);
        }
        break;
    case Puff::Photo: {
        p.translate(at);
        p.rotate(std::sin(particle.age * 2) * 8);
        const double s = particle.size;
        p.setBrush(QColor(255, 255, 255, int(240 * alpha)));
        p.drawRect(QRectF(-s / 2 - 4, -s / 2 - 4, s + 8, s + 14));
        p.setBrush(QColor(30, 70, 100, int(255 * alpha)));
        p.drawRect(QRectF(-s / 2, -s / 2, s, s));
        if (!particle.image.isNull()) {
            p.setOpacity(p.opacity() * alpha);
            p.drawImage(QRectF(-s / 2, -s / 2, s, s), particle.image);
        }
        break;
    }
    }
    p.restore();
}

} // namespace forkmesh::aquarium
