#include "GenieSpiderWidget.h"

#include <QCheckBox>
#include <QChildEvent>
#include <QContextMenuEvent>
#include <QCursor>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEnterEvent>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QMimeData>
#include <QRandomGenerator>
#include <QScreen>
#include <QScrollArea>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cmath>

namespace {

constexpr double kGravity = 2200;
constexpr double kDrag = 0.00055;
constexpr double kMaxSpeed = 4200;
constexpr double kSettle = 240;
constexpr double kFloorBounce = 0.3;
constexpr double kWallBounce = 0.42;
constexpr double kGlance = 0.78;
constexpr double kFriction = 2400;
constexpr double kRest = 20;
constexpr double kStep = 1.0 / 120.0;
constexpr double kThrowWindow = 0.08;
constexpr double kClimb = 260;
constexpr int kLongPressMs = 550;
constexpr int kEscapeMargin = 12;
constexpr double kHopChance = 0.3;
constexpr double kPlayChance = 0.5;
constexpr double kSceneEvery = 2;
constexpr double kHopFlight = 0.63;

double envelope(double a, double in, double from, double to)
{
    return std::clamp(a / in, 0.0, 1.0) * std::clamp((to - a) / (to - from), 0.0, 1.0);
}

class SilkThread : public QWidget
{
public:
    explicit SilkThread(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("genieSilk"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }

    void setLine(const QPointF &from, const QPointF &to, double reach, double slack)
    {
        const QPointF end = from + (to - from) * reach;
        const QRect box = QRectF(from, end).normalized().adjusted(-16, -16, 16, 16).toAlignedRect();
        m_from = from - QPointF(box.topLeft());
        m_to = end - QPointF(box.topLeft());
        m_reach = reach;
        m_slack = slack;
        if (geometry() != box)
            setGeometry(box);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const QPointF mid = (m_from + m_to) / 2 + QPointF(0, 12 * m_slack * m_reach);
        QPainterPath path;
        path.moveTo(m_from);
        path.quadTo(mid, m_to);
        p.strokePath(path, QPen(QColor(64, 242, 219, 70), 3.2, Qt::SolidLine, Qt::RoundCap));
        p.strokePath(path, QPen(QColor(235, 245, 255, 220), 1.1, Qt::SolidLine, Qt::RoundCap));
        if (m_reach >= 0.99) {
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(235, 245, 255, 230));
            p.drawEllipse(m_to, 2.2, 2.2);
        }
    }

private:
    QPointF m_from;
    QPointF m_to;
    double m_reach = 0;
    double m_slack = 1;
};

class MagicLayer : public QWidget
{
public:
    explicit MagicLayer(QWidget *parent)
        : QWidget(parent)
    {
        setObjectName(QStringLiteral("genieMagic"));
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setFocusPolicy(Qt::NoFocus);
    }

    void setMagic(const GenieMagic &magic, const QRect &box)
    {
        m_magic = magic;
        if (geometry() != box)
            setGeometry(box);
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.translate(-QPointF(geometry().topLeft()));
        GenieSpiderPainter::paintMagic(p, m_magic);
    }

private:
    GenieMagic m_magic;
};

void capSpeed(double &vx, double &vy)
{
    const double speed = std::hypot(vx, vy);
    if (speed > kMaxSpeed) {
        vx *= kMaxSpeed / speed;
        vy *= kMaxSpeed / speed;
    }
}

QPointF trailVelocity(const QList<QPair<double, QPointF>> &trail, double now, const QPointF &at)
{
    for (const auto &[when, spot] : trail) {
        if (when < now - kThrowWindow)
            continue;
        const double span = now - when;
        if (span <= 0.008)
            break;
        double vx = (at.x() - spot.x()) / span;
        double vy = (at.y() - spot.y()) / span;
        capSpeed(vx, vy);
        return QPointF(vx, vy);
    }
    return QPointF();
}

const char *kPanelStyle = R"(
QWidget { background: transparent; color: #e6edf3; font-size: 13px; }
QLabel#genieTitle { font-size: 15px; font-weight: 700; color: #ffffff; }
QLabel#genieHearts { font-size: 14px; color: #ff5c9e; }
QLabel#genieMuted, QLabel#genieMeasured { color: #9aa7c7; font-size: 12px; }
QLabel#genieMeterLabel { color: #c4cde6; font-size: 12px; font-weight: 600; }
QLabel#genieStage { font-size: 12px; font-weight: 700; color: #40f2db; letter-spacing: 1px; }
QLabel#genieHeadline { font-size: 14px; font-weight: 600; color: #ffffff; }
QLabel#genieSection { font-size: 12px; font-weight: 700; color: #bc8cff; letter-spacing: 1px; }
QLabel#genieStepNext { font-weight: 700; color: #ffffff; }
QLabel#genieStepDone { color: #7d8aa8; text-decoration: line-through; }
QLabel#genieStepProgress { color: #8fe9dc; font-size: 11px; }
QPushButton {
    background-color: rgba(64, 242, 219, 0.14); border: 1px solid rgba(64, 242, 219, 0.55);
    border-radius: 6px; padding: 5px 12px; color: #c9fff6; font-weight: 600;
}
QPushButton:hover { background-color: rgba(64, 242, 219, 0.26); }
QPushButton#genieGo { padding: 1px 8px; font-size: 11px; }
QToolButton { border: 0; color: #9aa7c7; font-size: 15px; padding: 0 4px; }
QToolButton:hover { color: #ffffff; }
QCheckBox { spacing: 6px; padding: 0; }
QCheckBox::indicator { width: 14px; height: 14px; border-radius: 3px; border: 1px solid #5d6a8c; background: rgba(255,255,255,0.04); }
QCheckBox::indicator:checked { background: #40f2db; border-color: #40f2db; }
QCheckBox::indicator:disabled { border-color: #3a4560; }
QCheckBox::indicator:checked:disabled { background: #2aa898; border-color: #2aa898; }
QProgressBar { border: 0; border-radius: 3px; background: rgba(255, 255, 255, 0.09); max-height: 7px; min-height: 7px; }
QProgressBar::chunk { border-radius: 3px; background: #40f2db; }
QProgressBar#genieMarketing::chunk { background: #bc8cff; }
QScrollBar:vertical { width: 6px; background: transparent; }
QScrollBar::handle:vertical { background: rgba(255,255,255,0.18); border-radius: 3px; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
)";

QString heartsText(int hearts)
{
    QString text;
    for (int i = 0; i < GenieVitals::maxHearts; ++i)
        text += i < hearts ? QChar(0x2665) : QChar(0x2661);
    return text;
}

void soften(QImage &image)
{
    const int w = image.width();
    const int h = image.height();
    for (int pass = 0; pass < 2; ++pass) {
        const QImage from = image.copy();
        for (int y = 0; y < h; ++y) {
            QRgb *out = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < w; ++x) {
                int sum = 0;
                for (int sy = std::max(0, y - 1); sy <= std::min(h - 1, y + 1); ++sy) {
                    const QRgb *row = reinterpret_cast<const QRgb *>(from.constScanLine(sy));
                    for (int sx = std::max(0, x - 1); sx <= std::min(w - 1, x + 1); ++sx)
                        sum += qAlpha(row[sx]);
                }
                out[x] = qRgba(0, 0, 0, sum / 9);
            }
        }
    }
}

void paintCard(QWidget *widget, const QRectF &card, double radius)
{
    QPainter p(widget);
    p.setRenderHint(QPainter::Antialiasing, true);
    QLinearGradient fill(card.topLeft(), card.bottomRight());
    fill.setColorAt(0, QColor(40, 26, 74, 250));
    fill.setColorAt(0.55, QColor(20, 20, 42, 250));
    fill.setColorAt(1, QColor(10, 16, 30, 250));
    p.setPen(QPen(QColor(64, 242, 219, 120), 1));
    p.setBrush(fill);
    p.drawRoundedRect(card, radius, radius);
}

}

GenieSpiderWidget::GenieSpiderWidget(QWidget *host)
    : QWidget(host)
{
    setObjectName(QStringLiteral("genieSpider"));
    setStyleSheet(QStringLiteral("background: transparent;"));
    const int outer = kSide + 2 * kPad;
    setFixedSize(outer, outer);
    setCursor(Qt::OpenHandCursor);
    setMouseTracking(true);
    const QRegion shape =
        QRegion(0, 0, outer, outer, QRegion::Ellipse).united(QRect(0, outer / 2, outer, outer - outer / 2));
    setMask(shape.intersected(QRect(0, 0, outer, kPad + kSide + 2)));
    m_clock.start();
    m_vitals = GenieVitals::measure(GenieVitalSigns(), 0);
    m_vitals.health = 0.7;
    m_pose.vitality = 0.7;
    m_frame = new QTimer(this);
    m_frame->setTimerType(Qt::CoarseTimer);
    connect(m_frame, &QTimer::timeout, this, &GenieSpiderWidget::tick);
    m_wander = new QTimer(this);
    m_wander->setSingleShot(true);
    connect(m_wander, &QTimer::timeout, this, &GenieSpiderWidget::wander);
    m_longPress = new QTimer(this);
    m_longPress->setSingleShot(true);
    m_longPress->setInterval(kLongPressMs);
    connect(m_longPress, &QTimer::timeout, this, [this] {
        if (!m_held || m_dragged)
            return;
        m_held = false;
        setCursor(Qt::OpenHandCursor);
        play(GenieReaction::Surprise);
        emit menuRequested(m_pressGlobal);
    });
}

void GenieSpiderWidget::setVitals(const GenieVitals &vitals)
{
    m_vitals = vitals;
    m_pose.vitality = vitals.health;
    m_pose.condition = vitals.level();
    m_dirty = true;
    update();
}

void GenieSpiderWidget::setArena(const QRect &area, const QList<QRect> &avoid)
{
    const bool first = m_x < 0;
    if (area != m_area) {
        stopPlay();
        rescanSoon();
    }
    m_area = area;
    m_avoid = avoid;
    if (m_area.width() < kSide || m_area.height() < kSide)
        return;
    if (first) {
        m_x = clampX(m_parked ? m_area.right() + kSide : m_area.left() + m_fraction * m_area.width());
        m_y = floorAt(m_x);
    } else if (m_parked && m_targetX < 0) {
        m_x = clampX(m_area.right() + kSide);
        m_y = floorAt(m_x);
    } else {
        m_x = clampX(m_x);
        if (m_targetX >= 0)
            m_targetX = clampX(m_targetX);
    }
    place();
}

void GenieSpiderWidget::setScene(std::function<GenieScene()> scene)
{
    m_sceneSource = std::move(scene);
    rescanSoon();
}

void GenieSpiderWidget::rescanSoon()
{
    m_sceneStale = true;
    m_sceneDue = std::min(m_sceneDue, m_clock.elapsed() / 1000.0 + 0.25);
}

void GenieSpiderWidget::calm()
{
    stopPlay();
}

void GenieSpiderWidget::setWanders(bool wanders)
{
    m_wanders = wanders;
    if (!wanders) {
        stopPlay();
        m_targetX = -1;
    }
    scheduleWander();
}

void GenieSpiderWidget::setMood(GenieMood mood)
{
    m_pose.mood = mood;
    m_dirty = true;
    update();
}

void GenieSpiderWidget::play(GenieReaction reaction)
{
    if (dancing() && (reaction == GenieReaction::Nod || reaction == GenieReaction::Eureka ||
                      reaction == GenieReaction::Cheer))
        return;
    m_pose.reaction = reaction;
    m_reactionStart = m_clock.elapsed() / 1000.0;
    m_pose.reactionAge = 0;
    if (isVisible() && (!m_frame->isActive() || m_frame->interval() > 42))
        pace(42);
}

void GenieSpiderWidget::dance(int which)
{
    if (dancing())
        return;
    stopPlay();
    if (which < 0 || which >= kGenieDanceCount) {
        which = QRandomGenerator::global()->bounded(kGenieDanceCount - (m_lastDance >= 0 ? 1 : 0));
        if (m_lastDance >= 0 && which >= m_lastDance)
            ++which;
    }
    m_lastDance = which;
    m_pose.dance = GenieDance(which);
    play(GenieReaction::Dance);
}

void GenieSpiderWidget::setParked(bool parked)
{
    if (m_parked == parked)
        return;
    stopPlay();
    m_parked = parked;
    m_targetX = -1;
    if (parked)
        head(m_area.right() + kSide * 0.18);
    else
        head(restingX(m_area.left() + m_fraction * m_area.width()));
    if (isVisible() && (!m_frame->isActive() || m_frame->interval() > 42))
        pace(42);
    scheduleWander();
}

void GenieSpiderWidget::walkTo(double x)
{
    stopPlay();
    head(x);
}

void GenieSpiderWidget::head(double x)
{
    if (m_area.isEmpty())
        return;
    m_targetX = clampX(x);
}

bool GenieSpiderWidget::grounded() const
{
    return m_x >= 0 && m_vx == 0 && m_vy == 0 && std::abs(m_y - floorAt(m_x)) <= 0.5;
}

GenieStage GenieSpiderWidget::stage() const
{
    GenieStage s;
    s.left = clampX(-1e9);
    s.right = clampX(1e9);
    s.side = kSide;
    s.floor = [this](double x) { return floorAt(x); };
    s.blocks = m_scene.blocks;
    return s;
}

void GenieSpiderWidget::refreshScene(double t, double maxAge)
{
    if (!m_sceneSource)
        return;
    if (!m_sceneStale && t - m_sceneAt < maxAge)
        return;
    m_scene = m_sceneSource();
    m_sceneAt = t;
    m_sceneStale = false;
}

double GenieSpiderWidget::restingX(double wanted)
{
    if (!m_sceneSource || m_roams || m_area.isEmpty())
        return wanted;
    refreshScene(m_clock.elapsed() / 1000.0, 1.0);
    return GenieKeepClear::clearSpot(stage(), wanted);
}

void GenieSpiderWidget::keepClear(double t)
{
    if (!m_sceneSource || m_roams || m_parked || m_submerged || m_hovered || m_held || dancing() || playing() ||
        m_targetX >= 0 || t < m_sceneDue || !grounded())
        return;
    m_sceneDue = t + kSceneEvery;
    refreshScene(t, 0);
    const double x = GenieKeepClear::clearSpot(stage(), m_x);
    if (std::abs(x - m_x) > 1.5)
        head(x);
}

bool GenieSpiderWidget::startPlay(double t)
{
    if (!m_sceneSource || m_roams || m_parked || m_submerged || m_held || m_hovered || dancing() || playing() ||
        m_targetX >= 0 || !grounded() || QGuiApplication::applicationState() != Qt::ApplicationActive)
        return false;
    QRandomGenerator *random = QRandomGenerator::global();
    if (random->generateDouble() >= kPlayChance)
        return false;
    refreshScene(t, 1.0);
    if (m_scene.quiet || m_scene.marks.isEmpty())
        return false;
    const GenieStage s = stage();
    const quint32 roll = random->generate();
    GeniePlan plans[kGenieInteractionCount + 1];
    bool tried[kGenieInteractionCount + 1] = {};
    const GenieInteraction kind = m_bag.take([&](GenieInteraction k) {
        const int i = int(k);
        if (!tried[i]) {
            tried[i] = true;
            plans[i] = GeniePlay::plan(k, m_scene.marks, s, m_x, roll);
        }
        return plans[i].valid();
    });
    if (kind == GenieInteraction::None)
        return false;
    m_play = plans[int(kind)];
    m_playActing = false;
    m_playWalk = false;
    m_playStart = t;
    m_playStep = -1;
    m_playIndex = 0;
    m_playRoll = roll;
    m_playFrom = m_play.spot;
    if (kind == GenieInteraction::Hop) {
        const double distance = m_play.spot - m_x;
        m_playFrom = std::abs(distance) > 170 ? m_play.spot - std::copysign(150.0, distance) : m_x;
    }
    if (kind == GenieInteraction::Poof)
        m_playFrom = m_x;
    head(m_playFrom);
    pace(42);
    return true;
}

void GenieSpiderWidget::stopPlay()
{
    if (!playing())
        return;
    const bool walking = m_playWalk || !m_playActing;
    m_play = GeniePlan();
    m_playActing = false;
    m_playWalk = false;
    m_perk = 0;
    m_focusUntil = -10;
    hideSilk();
    calmMagic();
    if (walking)
        m_targetX = -1;
    m_sceneDue = std::min(m_sceneDue, m_clock.elapsed() / 1000.0 + 0.3);
}

void GenieSpiderWidget::act(double t)
{
    if (!playing())
        return;
    if (!m_playActing) {
        if (t - m_playStart > 15) {
            stopPlay();
            return;
        }
        if (m_targetX >= 0 || !grounded())
            return;
        if (std::abs(m_x - clampX(m_playFrom)) > 3) {
            stopPlay();
            return;
        }
        m_playActing = true;
        m_playStart = t;
        m_playStep = -1;
        m_poofFrom = QPointF(m_x, m_y);
        if (m_play.kind == GenieInteraction::Hop) {
            m_vx = (m_play.spot - m_x) / kHopFlight * 1.03;
            m_vy = -kGravity * kHopFlight / 2;
            m_flight = 0;
            m_thrown = false;
        }
    }
    m_pose.point = 0;
    m_pose.lean = 0;
    m_pose.sniff = 0;
    m_pose.tug = 0;
    m_pose.size = 1;
    m_pose.spin = 0;
    m_pose.twirl = 0;
    m_pose.fade = 1;
    m_pose.incense = 0;
    m_lift = 0;
    m_grow = 1;
    if (!script(t - m_playStart, t))
        stopPlay();
}

void GenieSpiderWidget::focus(const QPointF &at, double t)
{
    m_focus = at;
    m_focusUntil = t + 0.3;
}

QPointF GenieSpiderWidget::aim(const QPointF &at) const
{
    return at - QPointF(m_x, m_y - kSide + 3 + kSide * 0.76);
}

QPointF GenieSpiderWidget::fingertip(const QPointF &aim) const
{
    const double unit = kSide / 100.0;
    const double length = std::max(std::hypot(aim.x(), aim.y()), 0.01);
    const QPointF shoulder(aim.x() >= 0 ? 65 : 35, 76);
    const QPointF reach = shoulder + QPointF(aim.x() / length, aim.y() / length) * 38;
    const QPointF tip(std::clamp(reach.x(), 3.0, 97.0), std::clamp(reach.y(), 3.0, 97.0));
    return QPointF(m_x - kSide / 2.0, m_y - kSide + 3) + tip * unit;
}

void GenieSpiderWidget::showSilk(const QPointF &from, const QPointF &to, double reach, double slack)
{
    QWidget *host = parentWidget();
    if (!host)
        return;
    if (m_silk && m_silk->parentWidget() != host)
        delete m_silk.data();
    if (!m_silk)
        m_silk = new SilkThread(host);
    static_cast<SilkThread *>(m_silk.data())->setLine(from, to, reach, slack);
    if (!m_silk->isVisible())
        m_silk->show();
    m_silk->stackUnder(this);
}

void GenieSpiderWidget::hideSilk()
{
    if (m_silk)
        m_silk->hide();
}

bool GenieSpiderWidget::conjure(double a, double t)
{
    const GenieInteraction kind = m_play.kind;
    const QRectF target = m_play.marks.first();
    const QPointF c0 = target.center();
    const double length = GenieMagic::length(kind);
    const auto once = [&](int step, double at) {
        if (m_playStep >= step || a < at)
            return false;
        m_playStep = step;
        return true;
    };
    focus(c0, t);
    if (kind == GenieInteraction::Poof && once(0, GenieMagic::kJump)) {
        m_x = clampX(m_play.spot);
        m_y = floorAt(m_x);
        emit moved();
    }
    if (kind == GenieInteraction::Lamp && once(0, 2.0))
        play(GenieReaction::Eureka);
    if (kind == GenieInteraction::JewelBeam && once(0, 0.15))
        play(GenieReaction::Eureka);
    if (kind == GenieInteraction::Fireworks && once(0, 0.6))
        play(GenieReaction::Cheer);
    if (kind == GenieInteraction::Bottle && once(0, 2.1))
        play(GenieReaction::Giggle);
    if (kind == GenieInteraction::Grow && once(0, 2.6))
        play(GenieReaction::Giggle);
    if (kind == GenieInteraction::Wishes && once(0, 2.9))
        play(GenieReaction::Nod);
    if (kind == GenieInteraction::Shimmer || kind == GenieInteraction::Vortex || kind == GenieInteraction::Runes ||
        kind == GenieInteraction::Flourish) {
        m_pose.point = envelope(a, 0.25, length - 0.7, length - 0.3);
        m_pose.pointAt = aim(c0);
    }
    const GenieMagicBody body = GenieMagic::body(kind, a);
    m_pose.fade = body.fade;
    m_pose.size = body.size;
    m_pose.spin = body.spin;
    m_pose.twirl = body.twirl;
    m_pose.incense = body.incense;
    m_pose.air = std::max(m_pose.air, body.air);
    m_lift = body.lift;
    m_grow = body.grow;
    if (a >= length)
        return false;
    showMagic(a, t);
    return true;
}

void GenieSpiderWidget::showMagic(double a, double t)
{
    QWidget *host = parentWidget();
    if (!host)
        return;
    GenieMagic magic;
    magic.kind = m_play.kind;
    magic.age = a;
    magic.t = t;
    magic.target = m_play.marks.first();
    magic.foot = QPointF(m_x, m_y - m_lift);
    magic.from = m_poofFrom;
    magic.hand = fingertip(m_pose.point > 0.01 ? m_pose.pointAt : aim(magic.target.center()));
    magic.side = kSide;
    magic.grow = m_grow;
    magic.lift = m_lift;
    magic.seed = m_playRoll;
    magic.pose = m_pose;
    const QRect box = magic.bounds().toAlignedRect().intersected(host->rect());
    if (box.isEmpty()) {
        hideMagic();
        return;
    }
    if (m_magic && m_magic->parentWidget() != host)
        delete m_magic.data();
    if (!m_magic)
        m_magic = new MagicLayer(host);
    static_cast<MagicLayer *>(m_magic.data())->setMagic(magic, box);
    if (!m_magic->isVisible())
        m_magic->show();
    m_magic->stackUnder(this);
}

void GenieSpiderWidget::hideMagic()
{
    if (m_magic)
        m_magic->hide();
}

void GenieSpiderWidget::calmMagic()
{
    hideMagic();
    m_pose.size = 1;
    m_pose.spin = 0;
    m_pose.twirl = 0;
    m_pose.fade = 1;
    m_pose.incense = 0;
    m_grow = 1;
    if (m_lift != 0) {
        m_lift = 0;
        place();
    }
    m_dirty = true;
    update();
}

bool GenieSpiderWidget::script(double a, double t)
{
    if (GeniePlay::magic(m_play.kind))
        return conjure(a, t);
    const QList<QRectF> &marks = m_play.marks;
    const QPointF c0 = marks.first().center();
    const double dir = c0.x() >= m_x ? 1 : -1;
    const auto once = [&](int step, double at) {
        if (m_playStep >= step || a < at)
            return false;
        m_playStep = step;
        return true;
    };
    switch (m_play.kind) {
    case GenieInteraction::Peek:
        focus(c0, t);
        m_perk = envelope(a, 0.25, 1.5, 1.9);
        m_pose.lean = 0.2 * dir * m_perk;
        return a < 1.9;
    case GenieInteraction::Read: {
        const QRectF &r = marks.first();
        const double u = std::clamp((a - 0.2) / 2.0, 0.0, 1.0);
        focus(QPointF(r.left() + 6 + u * std::max(0.0, r.width() - 12), r.center().y()), t);
        m_pose.lean = (u - 0.5) * 0.5 * envelope(a, 0.3, 2.2, 2.6);
        if (once(0, 2.25))
            play(GenieReaction::Nod);
        return a < 3.2;
    }
    case GenieInteraction::Wave:
        focus(c0, t);
        m_pose.lean = 0.2 * dir * envelope(a, 0.3, 1.5, 1.9);
        if (once(0, 0.1))
            play(GenieReaction::Wave);
        return a < 1.9;
    case GenieInteraction::Point:
        focus(c0, t);
        m_pose.point = envelope(a, 0.25, 1.4, 1.75);
        m_pose.pointAt = aim(c0);
        return a < 1.8;
    case GenieInteraction::Sniff:
        focus(c0, t);
        m_pose.lean = 0.8 * dir * envelope(a, 0.3, 1.5, 1.8);
        m_pose.sniff = envelope(a, 0.35, 1.4, 1.6);
        if (once(0, 1.7))
            play(GenieReaction::Giggle);
        return a < 2.9;
    case GenieInteraction::Cheer:
        focus(c0, t);
        if (once(0, 0.1))
            play(GenieReaction::Cheer);
        return a < 1.8;
    case GenieInteraction::TabNod: {
        if (m_playIndex >= marks.size())
            return false;
        focus(marks.at(m_playIndex).center(), t);
        if (m_playStep < 0) {
            m_playStep = 0;
            m_playMark = t;
            play(GenieReaction::Nod);
            return true;
        }
        if (m_playWalk) {
            if (m_targetX >= 0 || !grounded())
                return true;
            m_playWalk = false;
            m_playMark = t;
            play(GenieReaction::Nod);
            return true;
        }
        if (t - m_playMark < 1.0)
            return true;
        if (++m_playIndex >= marks.size())
            return false;
        head(marks.at(m_playIndex).center().x());
        m_playWalk = true;
        return true;
    }
    case GenieInteraction::Silk: {
        const QPointF to(c0.x(), marks.first().bottom() - 2);
        focus(c0, t);
        m_pose.point = envelope(a, 0.25, 2.1, 2.4);
        m_pose.pointAt = aim(to);
        double reach = 0;
        if (a >= 0.25 && a < 1.8)
            reach = std::clamp((a - 0.25) / 0.35, 0.0, 1.0);
        else if (a >= 1.8)
            reach = 1 - std::clamp((a - 1.8) / 0.35, 0.0, 1.0);
        const double pull = a >= 0.6 && a < 1.8 ? std::max(0.0, std::sin((a - 0.6) * M_PI * 2.5)) : 0;
        m_pose.tug = pull;
        if (reach > 0.01)
            showSilk(fingertip(m_pose.pointAt), to, reach, 1 - pull);
        else
            hideSilk();
        return a < 2.4;
    }
    case GenieInteraction::Hop:
        focus(c0, t);
        if (m_playStep < 0) {
            if (a < 0.1 || airborne() || !grounded())
                return a < 3.5;
            m_playStep = 0;
            m_playMark = t;
            play(GenieReaction::Giggle);
        }
        return t - m_playMark < 1.3;
    case GenieInteraction::Eureka:
        focus(c0, t);
        if (once(0, 0.1))
            play(GenieReaction::Eureka);
        return a < 1.6;
    case GenieInteraction::Startle:
        focus(c0, t);
        if (once(0, 0.05))
            play(GenieReaction::Surprise);
        if (once(1, 0.2) && grounded()) {
            m_vx = -dir * 70;
            m_vy = -460;
            m_flight = 0;
            m_thrown = false;
        }
        return a < 1.5;
    case GenieInteraction::Giggle:
        focus(c0, t);
        if (once(0, 0.1))
            play(GenieReaction::Giggle);
        if (once(1, 1.2))
            play(GenieReaction::Giggle);
        return a < 2.4;
    case GenieInteraction::Count: {
        const int n = int(marks.size());
        const int k = std::min(n - 1, int(std::max(0.0, a - 0.1) / 0.6));
        focus(marks.at(k).center(), t);
        if (a < n * 0.6 + 0.1 && k >= m_playIndex) {
            m_playIndex = k + 1;
            play(GenieReaction::Nod);
        }
        if (once(0, n * 0.6 + 0.15))
            play(GenieReaction::Eureka);
        return a < n * 0.6 + 1.0;
    }
    case GenieInteraction::Compare: {
        const int k = int(a / 0.55);
        focus(marks.at(k < 4 ? k % 2 : int(m_playRoll % 2)).center(), t);
        if (once(1, 2.25))
            play(GenieReaction::Nod);
        return a < 3.2;
    }
    default:
        break;
    }
    return false;
}

double GenieSpiderWidget::homeFraction() const
{
    if (m_area.width() <= 0)
        return m_fraction;
    return std::clamp((m_x - m_area.left()) / double(m_area.width()), 0.0, 1.0);
}

void GenieSpiderWidget::setHomeFraction(double fraction)
{
    m_fraction = std::clamp(fraction, 0.0, 1.0);
}

QRect GenieSpiderWidget::bodyRect() const
{
    return geometry().adjusted(kPad, kPad, -kPad, -kPad);
}

void GenieSpiderWidget::setRoams(bool roams)
{
    if (roams)
        stopPlay();
    m_roams = roams;
    scheduleWander();
}

void GenieSpiderWidget::throwFrom(const QPointF &foot, const QPointF &velocity)
{
    stopPlay();
    m_held = false;
    m_dragged = false;
    m_parked = false;
    m_targetX = -1;
    m_x = foot.x();
    m_y = foot.y();
    if (m_area.width() >= kSide && m_area.height() >= kSide) {
        m_x = clampX(m_x);
        m_y = std::clamp(m_y, double(m_area.top() + kSide), double(m_area.bottom()));
    }
    m_vx = velocity.x();
    m_vy = velocity.y();
    capSpeed(m_vx, m_vy);
    m_flight = 0;
    m_thrown = true;
    m_trail.clear();
    m_longPress->stop();
    setCursor(Qt::OpenHandCursor);
    place();
    if (m_vx == 0 && m_vy == 0 && m_y >= floorAt(m_x) - 0.5)
        settle();
    if (isVisible())
        pace(16);
    scheduleWander();
}

QPixmap GenieSpiderWidget::snapshot(qreal ratio) const
{
    QPixmap pixmap(QSize(kSide, kSide) * ratio);
    pixmap.setDevicePixelRatio(ratio);
    pixmap.fill(Qt::transparent);
    QPainter p(&pixmap);
    GenieSpiderPose pose = m_pose;
    pose.air = 1;
    pose.walking = 0;
    pose.hasGaze = false;
    GenieSpiderPainter::paint(p, QRectF(0, 0, kSide, kSide), pose);
    return pixmap;
}

double GenieSpiderWidget::clampX(double x) const
{
    const double half = kSide / 2.0;
    const double right = m_parked ? m_area.right() + kSide * 0.18 : m_area.right() - half;
    return std::clamp(x, m_area.left() + half, std::max(m_area.left() + half, right));
}

double GenieSpiderWidget::floorAt(double x, double above) const
{
    double floor = m_area.bottom();
    const double reach = kSide * 0.3;
    for (const QRect &r : m_avoid) {
        if (r.isEmpty() || x + reach < r.left() || x - reach > r.right() || r.top() < above - 0.5)
            continue;
        if (r.top() < floor && r.top() - kSide >= m_area.top())
            floor = r.top();
    }
    return floor;
}

void GenieSpiderWidget::setSubmerged(bool submerged)
{
    if (m_submerged == submerged)
        return;
    if (submerged)
        stopPlay();
    m_submerged = submerged;
    m_dirty = true;
    update();
    if (isVisible())
        pace(42);
}

QImage GenieSpiderWidget::frameImage()
{
    if (m_dirty || m_image.isNull() || m_image.devicePixelRatio() != devicePixelRatioF())
        render();
    return m_image;
}

void GenieSpiderWidget::lookAt(const QPointF &at)
{
    m_lookAt = at;
    m_lookUntil = m_clock.elapsed() / 1000.0 + 0.6;
}

void GenieSpiderWidget::hop(double vx, double vy)
{
    if (m_x < 0 || m_held || m_vx != 0 || m_vy != 0 || m_area.width() < kSide)
        return;
    throwFrom(QPointF(m_x, m_y), QPointF(vx, vy));
}

void GenieSpiderWidget::place()
{
    if (m_x < 0)
        return;
    move(int(std::lround(m_x - kSide / 2.0 - kPad)), int(std::lround(m_y - m_lift - kSide + 3 - kPad)));
}

void GenieSpiderWidget::scheduleWander()
{
    m_wander->stop();
    if (!m_wanders || m_parked || !isVisible())
        return;
    const double pace = 0.55 + 0.45 * std::clamp(m_vitals.health, 0.0, 1.0);
    const double rest = 5 + QRandomGenerator::global()->generateDouble() * 8;
    m_wander->start(int(rest / pace * 1000));
}

void GenieSpiderWidget::wander()
{
    if (playing() || (m_hovered && !m_held)) {
        scheduleWander();
        return;
    }
    if (m_wanders && m_roams && !m_held && m_area.width() > kSide * 2) {
        QRandomGenerator *random = QRandomGenerator::global();
        const double target = m_area.left() + random->generateDouble() * m_area.width();
        const bool grounded = m_vx == 0 && m_vy == 0 && m_y >= floorAt(m_x) - 0.5;
        if (grounded && random->generateDouble() < kHopChance) {
            m_targetX = -1;
            m_vy = -(900 + random->generateDouble() * 900);
            m_vx = std::copysign(150 + random->generateDouble() * 450, target - m_x);
            m_flight = 0;
            m_thrown = true;
        } else {
            walkTo(target);
        }
        pace(16);
    } else if (m_wanders && !m_held && m_area.width() > kSide * 2) {
        if (startPlay(m_clock.elapsed() / 1000.0)) {
            scheduleWander();
            return;
        }
        const double span = std::min(260.0, m_area.width() / 3.0);
        const double offset = (QRandomGenerator::global()->generateDouble() * 2 - 1) * span;
        double target = m_x + offset;
        const double home = m_area.left() + m_fraction * m_area.width();
        if (std::abs(target - home) > m_area.width() * 0.3)
            target = home + (QRandomGenerator::global()->generateDouble() * 2 - 1) * 60;
        head(restingX(target));
        if (!m_frame->isActive() || m_frame->interval() > 42)
            pace(42);
    }
    scheduleWander();
}

void GenieSpiderWidget::tick()
{
    const QWindow *handle = window() ? window()->windowHandle() : nullptr;
    if (handle && !handle->isExposed()) {
        pace(500);
        return;
    }
    const double t = m_clock.elapsed() / 1000.0;
    const double dt = std::clamp(t - m_lastTick, 0.0, 0.1);
    m_lastTick = t;
    m_pose.t = t;
    if (m_pose.reaction != GenieReaction::None) {
        m_pose.reactionAge = t - m_reactionStart;
        if (m_pose.reactionAge > GenieSpiderPose::duration(m_pose.reaction))
            m_pose.reaction = GenieReaction::None;
    }
    bool busy = m_pose.reaction != GenieReaction::None || m_pose.mood != GenieMood::Idle || playing();
    bool flying = false;
    if (!playing()) {
        const double fade = dt * 5;
        m_pose.point = std::max(0.0, m_pose.point - fade);
        m_pose.sniff = std::max(0.0, m_pose.sniff - fade);
        m_pose.tug = std::max(0.0, m_pose.tug - fade);
        m_pose.lean -= std::clamp(m_pose.lean, -fade, fade);
        if (m_pose.point > 0 || m_pose.sniff > 0 || m_pose.tug > 0 || m_pose.lean != 0)
            busy = true;
    }
    if (m_held) {
        m_pose.air = 1;
        m_pose.walking = 0;
        m_pose.stridePhase += dt * 14;
        busy = true;
    } else if (m_x >= 0) {
        const double floor = floorAt(m_x);
        if (m_y < floor - 0.5 || m_vx != 0 || m_vy != 0) {
            fly(dt, t);
            busy = true;
            flying = true;
        } else if (m_y > floor + 0.5) {
            m_y = std::max(floor, m_y - kClimb * dt);
            m_pose.walking = 1;
            m_pose.stridePhase += dt * 16;
            busy = true;
        } else {
            m_y = floor;
            m_pose.air = 0;
            if (m_targetX >= 0 && !dancing()) {
                const double speed = 60 + 60 * std::clamp(m_vitals.health, 0.0, 1.0);
                const double dx = m_targetX - m_x;
                const double step = std::copysign(std::min(std::abs(dx), speed * dt), dx);
                m_x += step;
                m_pose.stridePhase += std::abs(step) * 0.17;
                m_pose.walking = std::min(1.0, m_pose.walking + dt * 5);
                m_pose.facing = dx < 0 ? -1 : 1;
                m_lastMove = t;
                if (std::abs(dx) < 0.6) {
                    m_targetX = -1;
                    emit moved();
                }
                busy = true;
            } else {
                m_pose.walking = std::max(0.0, m_pose.walking - dt * 4);
                if (m_pose.walking > 0)
                    busy = true;
                else
                    m_pose.facing *= std::max(0.0, 1 - dt * 3);
            }
        }
        act(t);
        keepClear(t);
    }
    m_pose.land = std::max(0.0, 1 - (t - m_landAt) / 0.25);
    if (m_pose.land > 0)
        busy = true;
    const double perk = m_hovered && !m_held ? 1.0 : playing() ? m_perk : 0.0;
    if (m_pose.hover < perk)
        m_pose.hover = std::min(perk, m_pose.hover + dt * 6);
    else if (m_pose.hover > perk)
        m_pose.hover = std::max(perk, m_pose.hover - dt * 4);
    if (m_pose.hover > 0)
        busy = true;
    bool near = false;
    if (parentWidget()) {
        const QPoint cursor = parentWidget()->mapFromGlobal(QCursor::pos());
        const QPointF centre(m_x, m_y - kSide * 0.45);
        const QPointF d = QPointF(cursor) - centre;
        const double distance = std::hypot(d.x(), d.y());
        m_pose.hasGaze = parentWidget()->rect().contains(cursor) && distance < 480 && !m_held;
        if (m_pose.hasGaze) {
            m_pose.gaze = QPointF(d.x() / std::max(distance, 1.0), d.y() / std::max(distance, 1.0)) *
                          std::min(1.0, distance / 120.0);
            near = distance < 200;
        }
        if (playing() && t < m_focusUntil && !m_held && !m_hovered) {
            const QPointF d = m_focus - QPointF(m_x, m_y - kSide * 0.45);
            const double distance = std::hypot(d.x(), d.y());
            m_pose.hasGaze = true;
            m_pose.gaze = QPointF(d.x() / std::max(distance, 1.0), d.y() / std::max(distance, 1.0)) *
                          std::min(1.0, distance / 60.0);
        }
        if ((m_submerged || !m_pose.hasGaze) && t < m_lookUntil && !m_held) {
            const QPointF d = m_lookAt - QPointF(m_x, m_y - kSide * 0.45);
            const double distance = std::hypot(d.x(), d.y());
            m_pose.hasGaze = true;
            m_pose.gaze = QPointF(d.x() / std::max(distance, 1.0), d.y() / std::max(distance, 1.0)) *
                          std::min(1.0, distance / 120.0);
        }
    }
    place();
    m_dirty = true;
    if (!m_submerged)
        update();
    const bool awake = m_roams || QGuiApplication::applicationState() == Qt::ApplicationActive;
    pace(flying ? 16 : busy || m_submerged ? 42 : near ? 66 : awake ? 125 : 250);
}

void GenieSpiderWidget::pace(int ms)
{
    const Qt::TimerType type = ms <= 16 ? Qt::PreciseTimer : Qt::CoarseTimer;
    if (m_frame->isActive() && m_frame->interval() == ms && m_frame->timerType() == type)
        return;
    m_frame->setTimerType(type);
    m_frame->start(ms);
}

void GenieSpiderWidget::render()
{
    m_dirty = false;
    const qreal ratio = devicePixelRatioF();
    const QSize device = size() * ratio;
    if (m_image.size() != device)
        m_image = QImage(device, QImage::Format_ARGB32_Premultiplied);
    m_image.setDevicePixelRatio(ratio);
    m_image.fill(Qt::transparent);
    {
        QPainter p(&m_image);
        GenieSpiderPainter::paint(p, QRectF(kPad, kPad, kSide, kSide), m_pose);
    }
    const int small = std::max(4, (width() + 3) / 4);
    if (!m_halo.isNull() && m_halo.width() == small && std::abs(m_pose.t - m_haloAt) < 0.12)
        return;
    m_haloAt = m_pose.t;
    m_halo = m_image.scaled(small, small, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    m_halo.setDevicePixelRatio(1);
    soften(m_halo);
    QPainter tint(&m_halo);
    tint.setCompositionMode(QPainter::CompositionMode_SourceIn);
    const double strength = 0.25 + 0.17 * std::max(m_pose.hover, m_held ? 1.0 : 0.0);
    tint.fillRect(m_halo.rect(), QColor(64, 242, 219, int(255 * strength)));
}

void GenieSpiderWidget::fly(double dt, double t)
{
    m_flight += dt;
    while (m_flight >= kStep) {
        m_flight -= kStep;
        if (!flightStep(t)) {
            m_flight = 0;
            m_sceneDue = std::min(m_sceneDue, t + 0.3);
            if (m_thrown)
                settle();
            break;
        }
    }
    const bool grounded = m_vy == 0 && m_y >= floorAt(m_x) - 0.5;
    m_pose.air = grounded ? 0 : std::clamp(std::hypot(m_vx, m_vy) / 500.0, 0.3, 1.0);
    m_pose.walking = grounded ? std::min(1.0, std::abs(m_vx) / 150.0) : 0;
    if (m_vx != 0)
        m_pose.facing = m_vx < 0 ? -1 : 1;
    m_pose.stridePhase += dt * (grounded ? 16 : 12);
}

bool GenieSpiderWidget::flightStep(double t)
{
    const double left = clampX(-1e9);
    const double right = clampX(1e9);
    const double top = m_area.top() + kSide;
    if (m_vy < 0 || m_y < floorAt(m_x) - 0.5) {
        const double from = m_y;
        capSpeed(m_vx, m_vy);
        const double beforeX = m_vx;
        const double beforeY = m_vy;
        m_vy += kGravity * kStep;
        const auto slow = [](double velocity) {
            return velocity / (1 + kDrag * std::abs(velocity) * kStep);
        };
        m_vx = slow(m_vx);
        m_vy = slow(m_vy);
        capSpeed(m_vx, m_vy);
        m_x += (beforeX + m_vx) * kStep / 2;
        m_y += (beforeY + m_vy) * kStep / 2;
        if ((m_x < left && m_vx < 0) || (m_x > right && m_vx > 0)) {
            m_vx = -m_vx * kWallBounce;
            m_vy *= kGlance;
        }
        m_x = std::clamp(m_x, left, right);
        if (m_y < top) {
            m_y = top;
            if (m_vy < 0) {
                m_vy = -m_vy * kWallBounce;
                m_vx *= kGlance;
            }
        }
        const double floor = floorAt(m_x, from);
        if (m_y < floor)
            return true;
        m_y = floor;
        m_landAt = t;
        if (m_vy < kSettle) {
            m_vy = 0;
            if (std::abs(m_vx) >= kRest)
                return true;
            m_vx = 0;
            return false;
        }
        if (m_vy > 700)
            play(GenieReaction::Giggle);
        m_vy = -m_vy * kFloorBounce;
        m_vx *= kGlance;
        return true;
    }
    m_vy = 0;
    const double speed = std::abs(m_vx) - kFriction * kStep;
    if (speed < kRest) {
        m_vx = 0;
        return false;
    }
    m_vx = std::copysign(speed, m_vx);
    const double next = m_x + m_vx * kStep;
    const double there = floorAt(next);
    if (there < floorAt(m_x) - 0.5 && there < m_y - 0.5) {
        m_vx = -m_vx * kWallBounce;
        return true;
    }
    m_x = next;
    if ((m_x < left && m_vx < 0) || (m_x > right && m_vx > 0))
        m_vx = -m_vx * kWallBounce;
    m_x = std::clamp(m_x, left, right);
    return true;
}

void GenieSpiderWidget::settle()
{
    m_thrown = false;
    m_fraction = homeFraction();
    emit moved();
}

void GenieSpiderWidget::paintEvent(QPaintEvent *)
{
    if (m_submerged || m_grow > 1.001)
        return;
    if (m_dirty || m_image.isNull() || m_image.devicePixelRatio() != devicePixelRatioF())
        render();
    QPainter p(this);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    const double drop = 3 + 2 * std::max(m_pose.hover, m_held ? 1.0 : 0.0);
    p.drawImage(QRectF(rect()).translated(0, drop), m_halo);
    p.drawImage(QPointF(0, 0), m_image);
}

void GenieSpiderWidget::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    stopPlay();
    m_held = true;
    m_dragged = false;
    m_grab = event->position().toPoint();
    m_pressGlobal = event->globalPosition().toPoint();
    m_targetX = -1;
    m_vx = 0;
    m_vy = 0;
    m_flight = 0;
    m_thrown = false;
    m_trail.clear();
    setCursor(Qt::ClosedHandCursor);
    raise();
    m_longPress->start();
    if (!m_frame->isActive() || m_frame->interval() > 42)
        pace(42);
}

void GenieSpiderWidget::mouseMoveEvent(QMouseEvent *event)
{
    if (!m_held || !parentWidget())
        return;
    if ((event->globalPosition().toPoint() - m_pressGlobal).manhattanLength() > 6)
        m_dragged = true;
    if (m_dragged)
        m_longPress->stop();
    if (!m_dragged)
        return;
    if (m_escapes && window()) {
        const QPoint inWindow = window()->mapFromGlobal(event->globalPosition().toPoint());
        const QRect bounds =
            window()->rect().adjusted(-kEscapeMargin, -kEscapeMargin, kEscapeMargin, kEscapeMargin);
        if (!bounds.contains(inWindow)) {
            m_held = false;
            m_dragged = false;
            m_trail.clear();
            setCursor(Qt::OpenHandCursor);
            emit escaped(m_grab - QPoint(kPad, kPad));
            return;
        }
    }
    const QPoint at = parentWidget()->mapFromGlobal(event->globalPosition().toPoint()) - m_grab;
    m_x = clampX(at.x() + kPad + kSide / 2.0);
    m_y = std::clamp(double(at.y() + kPad + kSide - 3), double(m_area.top() + kSide), double(m_area.bottom()));
    m_vy = 0;
    const double now = m_clock.elapsed() / 1000.0;
    m_trail.append({now, QPointF(m_x, m_y)});
    while (m_trail.size() > 2 && m_trail.first().first < now - kThrowWindow * 2)
        m_trail.removeFirst();
    place();
}

void GenieSpiderWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_held)
        return;
    m_held = false;
    m_longPress->stop();
    setCursor(Qt::OpenHandCursor);
    if (!m_dragged) {
        play(GenieReaction::Wave);
        emit clicked();
        return;
    }
    m_parked = false;
    const QPoint at = parentWidget()->mapFromGlobal(event->globalPosition().toPoint()) - m_grab;
    m_x = clampX(at.x() + kPad + kSide / 2.0);
    m_y = std::clamp(double(at.y() + kPad + kSide - 3), double(m_area.top() + kSide), double(m_area.bottom()));
    const double now = m_clock.elapsed() / 1000.0;
    m_trail.append({now, QPointF(m_x, m_y)});
    while (m_trail.size() > 2 && m_trail.first().first < now - kThrowWindow * 2)
        m_trail.removeFirst();
    const QPointF velocity = trailVelocity(m_trail, now, QPointF(m_x, m_y));
    m_vx = velocity.x();
    m_vy = velocity.y();
    capSpeed(m_vx, m_vy);
    m_trail.clear();
    m_thrown = true;
    m_flight = 0;
    if (m_vx == 0 && m_vy == 0 && m_y >= floorAt(m_x) - 0.5)
        settle();
}

void GenieSpiderWidget::contextMenuEvent(QContextMenuEvent *event)
{
    stopPlay();
    emit menuRequested(event->globalPos());
}

void GenieSpiderWidget::enterEvent(QEnterEvent *event)
{
    QWidget::enterEvent(event);
    m_hovered = true;
    if (m_held)
        return;
    stopPlay();
    if (!m_parked && !m_thrown && m_vx == 0 && m_vy == 0)
        m_targetX = -1;
    const double now = m_clock.elapsed() / 1000.0;
    if (m_pose.reaction == GenieReaction::None && now - m_greetAt > 4) {
        m_greetAt = now;
        play(GenieReaction::Giggle);
    } else if (isVisible() && (!m_frame->isActive() || m_frame->interval() > 42)) {
        pace(42);
    }
}

void GenieSpiderWidget::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    m_hovered = false;
}

void GenieSpiderWidget::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    m_lastTick = m_clock.elapsed() / 1000.0;
    m_dirty = true;
    pace(42);
    scheduleWander();
}

void GenieSpiderWidget::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    stopPlay();
    m_hovered = false;
    m_pose.hover = 0;
    m_frame->stop();
    m_wander->stop();
    m_longPress->stop();
}

GenieTipBubble::GenieTipBubble(QWidget *host)
    : QFrame(host)
{
    setObjectName(QStringLiteral("genieTipBubble"));
    setStyleSheet(QStringLiteral("QWidget { background: transparent; color: #e6edf3; font-size: 13px; }"));
    setCursor(Qt::PointingHandCursor);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 9, 12, 17);
    m_text = new QLabel;
    m_text->setWordWrap(true);
    m_text->setTextFormat(Qt::PlainText);
    m_text->setAttribute(Qt::WA_TransparentForMouseEvents);
    layout->addWidget(m_text);
    m_task = new QLabel;
    m_task->setWordWrap(true);
    m_task->setTextFormat(Qt::PlainText);
    m_task->setAttribute(Qt::WA_TransparentForMouseEvents);
    m_task->setStyleSheet(QStringLiteral("color: #8ff5e6; font-size: 12px;"));
    layout->addWidget(m_task);
    m_add = new QPushButton(QStringLiteral("Add to agent"));
    m_add->setObjectName(QStringLiteral("genieAddToAgent"));
    m_add->setCursor(Qt::PointingHandCursor);
    m_add->setToolTip(QStringLiteral("Put the instructions for this in the agent composer"));
    m_add->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: rgba(64, 242, 219, 0.14); border: 1px solid rgba(64, 242, 219, 0.55);"
        " border-radius: 6px; padding: 2px 10px; color: #c9fff6; font-size: 12px; font-weight: 600; }"
        "QPushButton:hover { background-color: rgba(64, 242, 219, 0.28); }"));
    connect(m_add, &QPushButton::clicked, this, [this] {
        hide();
        emit addToAgent(m_prompt);
    });
    m_reply = new QPushButton(QStringLiteral("Reply"));
    m_reply->setObjectName(QStringLiteral("genieReply"));
    m_reply->setCursor(Qt::PointingHandCursor);
    m_reply->setToolTip(QStringLiteral("Tell the spider what it got wrong, such as someone you already closed with"));
    m_reply->setStyleSheet(QStringLiteral(
        "QPushButton { background-color: transparent; border: 1px solid rgba(201, 255, 246, 0.35);"
        " border-radius: 6px; padding: 2px 10px; color: #c9fff6; font-size: 12px; }"
        "QPushButton:hover { background-color: rgba(201, 255, 246, 0.12); }"));
    connect(m_reply, &QPushButton::clicked, this, [this] {
        hide();
        emit reply(m_text->text());
    });
    auto *actions = new QHBoxLayout;
    actions->setContentsMargins(0, 2, 0, 0);
    actions->setSpacing(6);
    actions->addStretch(1);
    actions->addWidget(m_reply);
    actions->addWidget(m_add);
    layout->addLayout(actions);
    m_hide = new QTimer(this);
    m_hide->setSingleShot(true);
    connect(m_hide, &QTimer::timeout, this, &QWidget::hide);
    hide();
}

void GenieTipBubble::showTip(const QString &text, const QRect &anchor, int ms, const QString &prompt,
                             const QString &task)
{
    present(text, anchor, ms, prompt, task, false);
}

void GenieTipBubble::say(const QString &line, const QRect &anchor, int ms)
{
    present(line, anchor, ms, QString(), QString(), true);
}

void GenieTipBubble::present(const QString &text, const QRect &anchor, int ms, const QString &prompt,
                             const QString &task, bool line)
{
    if (!parentWidget())
        return;
    m_line = line;
    m_prompt = prompt;
    m_text->setText(text);
    m_task->setText(task.isEmpty() ? QString() : QStringLiteral("An agent can %1.").arg(task));
    m_task->setVisible(!task.isEmpty() && !prompt.isEmpty());
    m_add->setVisible(!prompt.isEmpty());
    m_reply->setVisible(m_replies && !line);
    m_text->ensurePolished();
    const int room = std::min(300, std::max(160, parentWidget()->width() - 24));
    const int width = line ? std::min(room, std::max(120, m_text->fontMetrics().horizontalAdvance(text) + 32)) : room;
    m_text->setFixedWidth(width - 24);
    m_task->setFixedWidth(width - 24);
    adjustSize();
    setFixedSize(width, sizeHint().height());
    const QRect host = parentWidget()->rect();
    int x = anchor.center().x() - width / 2;
    x = std::clamp(x, host.left() + 8, std::max(host.left() + 8, host.right() - width - 8));
    const int y = std::max(host.top() + 8, anchor.top() - height() + 8);
    move(x, y);
    m_tailX = anchor.center().x() - x;
    show();
    raise();
    update();
    m_hide->start(ms);
}

void GenieTipBubble::setReplies(bool on)
{
    m_replies = on;
    m_reply->setVisible(on && isVisible() && !m_line);
}

void GenieTipBubble::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    const QRectF card = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -8.5);
    QPainterPath path;
    path.addRoundedRect(card, 12, 12);
    const double tail = std::clamp(double(m_tailX), 20.0, width() - 20.0);
    QPainterPath point;
    point.moveTo(tail - 8, card.bottom() - 1);
    point.lineTo(tail, card.bottom() + 8);
    point.lineTo(tail + 8, card.bottom() - 1);
    point.closeSubpath();
    path = path.united(point);
    QLinearGradient fill(card.topLeft(), card.bottomRight());
    fill.setColorAt(0, QColor(40, 26, 74, 245));
    fill.setColorAt(1, QColor(14, 18, 36, 245));
    p.setPen(QPen(QColor(64, 242, 219, 140), 1));
    p.setBrush(fill);
    p.drawPath(path);
}

void GenieTipBubble::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        hide();
        emit clicked();
    }
}

void GenieTipBubble::enterEvent(QEnterEvent *event)
{
    QFrame::enterEvent(event);
    m_hide->stop();
}

void GenieTipBubble::leaveEvent(QEvent *event)
{
    QFrame::leaveEvent(event);
    if (isVisible())
        m_hide->start(4000);
}

GenieDesktopStage::GenieDesktopStage()
    : QWidget(nullptr,
#ifdef Q_OS_MACOS
              Qt::Window |
#else
              Qt::Tool |
#endif
                  Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::NoDropShadowWindowHint |
                  Qt::WindowDoesNotAcceptFocus)
{
    setObjectName(QStringLiteral("genieDesktopStage"));
    setWindowTitle(QStringLiteral("Genie"));
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAttribute(Qt::WA_QuitOnClose, false);
    setAcceptDrops(true);
    m_clock.start();
    setMask(QRegion(0, 0, 1, 1));
}

void GenieDesktopStage::open(QScreen *screen)
{
    if (!isVisible()) {
        if (screen)
            setGeometry(screen->availableGeometry());
        if (QGuiApplication::platformName().startsWith(QLatin1String("wayland"), Qt::CaseInsensitive))
            showMaximized();
        else
            show();
    }
    raise();
}

void GenieDesktopStage::setCatching(bool catching)
{
    m_catching = catching;
    if (catching) {
        m_caught = false;
        m_trail.clear();
    }
    updateMask();
}

bool GenieDesktopStage::takeCatch(QPointF *at, QPointF *velocity)
{
    if (!m_caught)
        return false;
    m_caught = false;
    if (at)
        *at = m_at;
    if (velocity)
        *velocity = m_velocity;
    return true;
}

void GenieDesktopStage::childEvent(QChildEvent *event)
{
    QWidget::childEvent(event);
    if (event->added() && event->child()->isWidgetType())
        event->child()->installEventFilter(this);
    if (event->removed())
        updateMask();
}

bool GenieDesktopStage::eventFilter(QObject *watched, QEvent *event)
{
    if (watched->parent() == this) {
        switch (event->type()) {
        case QEvent::Move:
        case QEvent::Resize:
        case QEvent::Show:
        case QEvent::Hide:
            updateMask();
            break;
        default:
            break;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void GenieDesktopStage::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    emit arenaChanged();
}

void GenieDesktopStage::dragEnterEvent(QDragEnterEvent *event)
{
    if (!event->mimeData()->hasFormat(QLatin1String(kMime)))
        return;
    m_trail.clear();
    note(event->position());
    event->acceptProposedAction();
}

void GenieDesktopStage::dragMoveEvent(QDragMoveEvent *event)
{
    if (!event->mimeData()->hasFormat(QLatin1String(kMime)))
        return;
    note(event->position());
    event->acceptProposedAction();
}

void GenieDesktopStage::dropEvent(QDropEvent *event)
{
    if (!event->mimeData()->hasFormat(QLatin1String(kMime)))
        return;
    const double now = m_clock.elapsed() / 1000.0;
    m_at = event->position();
    m_velocity = trailVelocity(m_trail, now, m_at);
    m_trail.clear();
    m_caught = true;
    event->acceptProposedAction();
}

void GenieDesktopStage::note(const QPointF &at)
{
    const double now = m_clock.elapsed() / 1000.0;
    m_trail.append({now, at});
    while (m_trail.size() > 2 && m_trail.first().first < now - kThrowWindow * 2)
        m_trail.removeFirst();
}

void GenieDesktopStage::updateMask()
{
    if (m_catching) {
        clearMask();
        return;
    }
    QRegion region;
    for (QObject *object : children()) {
        auto *widget = qobject_cast<QWidget *>(object);
        if (widget && !widget->isWindow() && !widget->isHidden())
            region += widget->geometry();
    }
    if (region.isEmpty())
        region = QRegion(0, 0, 1, 1);
    if (region != mask())
        setMask(region);
}

GenieCoachPanel::GenieCoachPanel(QWidget *host)
    : QFrame(host)
{
    setObjectName(QStringLiteral("genieCoachPanel"));
    setStyleSheet(QString::fromLatin1(kPanelStyle));
    setFixedWidth(360);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 12, 10, 12);
    outer->setSpacing(8);

    auto *header = new QHBoxLayout;
    auto *title = new QLabel(QStringLiteral("Genie"));
    title->setObjectName(QStringLiteral("genieTitle"));
    m_mood = new QLabel;
    m_mood->setObjectName(QStringLiteral("genieMuted"));
    m_hearts = new QLabel;
    m_hearts->setObjectName(QStringLiteral("genieHearts"));
    auto *close = new QToolButton;
    close->setText(QStringLiteral("×"));
    close->setToolTip(QStringLiteral("Close"));
    close->setCursor(Qt::PointingHandCursor);
    connect(close, &QToolButton::clicked, this, &GenieCoachPanel::closeRequested);
    header->addWidget(title);
    header->addSpacing(6);
    header->addWidget(m_mood);
    header->addStretch(1);
    header->addWidget(m_hearts);
    header->addWidget(close);
    outer->addLayout(header);

    auto *scroll = new QScrollArea;
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 0, 6, 0);
    layout->setSpacing(8);
    scroll->setWidget(body);
    outer->addWidget(scroll, 1);

    auto meter = [layout](const QString &name, QWidget **holder, QProgressBar **bar, const char *object) {
        *holder = new QWidget;
        auto *row = new QHBoxLayout(*holder);
        row->setContentsMargins(0, 0, 0, 0);
        auto *label = new QLabel(name);
        label->setObjectName(QStringLiteral("genieMeterLabel"));
        label->setFixedWidth(74);
        *bar = new QProgressBar;
        (*bar)->setObjectName(QLatin1String(object));
        (*bar)->setRange(0, 100);
        (*bar)->setTextVisible(false);
        row->addWidget(label);
        row->addWidget(*bar, 1);
        layout->addWidget(*holder);
    };
    meter(QStringLiteral("Code"), &m_codeRow, &m_code, "genieCode");
    meter(QStringLiteral("Marketing"), &m_marketingRow, &m_marketing, "genieMarketing");

    m_summary = new QLabel;
    m_summary->setObjectName(QStringLiteral("genieMuted"));
    m_summary->setWordWrap(true);
    layout->addWidget(m_summary);

    auto *coach = new QLabel(QStringLiteral("MARKETING COACH"));
    coach->setObjectName(QStringLiteral("genieSection"));
    layout->addSpacing(4);
    layout->addWidget(coach);
    m_stage = new QLabel;
    m_stage->setObjectName(QStringLiteral("genieStage"));
    layout->addWidget(m_stage);
    m_headline = new QLabel;
    m_headline->setObjectName(QStringLiteral("genieHeadline"));
    m_headline->setWordWrap(true);
    layout->addWidget(m_headline);
    m_advice = new QLabel;
    m_advice->setWordWrap(true);
    layout->addWidget(m_advice);
    m_action = new QPushButton;
    m_action->setCursor(Qt::PointingHandCursor);
    connect(m_action, &QPushButton::clicked, this, [this] {
        if (!m_actionPage.isEmpty())
            emit navigate(m_actionPage);
    });
    auto *actionRow = new QHBoxLayout;
    actionRow->addWidget(m_action);
    actionRow->addStretch(1);
    layout->addLayout(actionRow);

    m_seenTitle = new QLabel(QStringLiteral("WHAT I SEE"));
    m_seenTitle->setObjectName(QStringLiteral("genieSection"));
    layout->addSpacing(4);
    layout->addWidget(m_seenTitle);
    m_seen = new QWidget;
    m_seenLayout = new QVBoxLayout(m_seen);
    m_seenLayout->setContentsMargins(0, 0, 0, 0);
    m_seenLayout->setSpacing(6);
    layout->addWidget(m_seen);

    m_playbookTitle = new QLabel;
    m_playbookTitle->setObjectName(QStringLiteral("genieSection"));
    layout->addSpacing(4);
    layout->addWidget(m_playbookTitle);
    m_steps = new QWidget;
    m_stepsLayout = new QVBoxLayout(m_steps);
    m_stepsLayout->setContentsMargins(0, 0, 0, 0);
    m_stepsLayout->setSpacing(6);
    layout->addWidget(m_steps);
    layout->addStretch(1);

    auto *footer = new QHBoxLayout;
    m_measured = new QLabel;
    m_measured->setObjectName(QStringLiteral("genieMeasured"));
    m_measured->setWordWrap(true);
    auto *refresh = new QToolButton;
    refresh->setText(QStringLiteral("↻"));
    refresh->setToolTip(QStringLiteral("Measure again"));
    refresh->setCursor(Qt::PointingHandCursor);
    connect(refresh, &QToolButton::clicked, this, &GenieCoachPanel::refreshRequested);
    footer->addWidget(m_measured, 1);
    footer->addWidget(refresh);
    outer->addLayout(footer);
    hide();
}

void GenieCoachPanel::setCoaching(const GenieVitals &vitals, const GenieCoaching &coaching, bool measuring)
{
    m_mood->setText(GenieVitals::title(vitals.level()));
    m_hearts->setText(heartsText(vitals.hearts()));
    m_hearts->setToolTip(QStringLiteral("%1 of %2 hearts").arg(vitals.hearts()).arg(GenieVitals::maxHearts));
    m_codeRow->setVisible(vitals.watch.code());
    m_marketingRow->setVisible(vitals.watch.outreach());
    m_code->setValue(int(std::lround(vitals.code * 100)));
    m_code->setToolTip(QStringLiteral("Code is %1 (%2%)").arg(GenieVitals::word(vitals.code)).arg(m_code->value()));
    m_marketing->setValue(int(std::lround(vitals.marketing * 100)));
    m_marketing->setToolTip(
        QStringLiteral("Marketing is %1 (%2%)").arg(GenieVitals::word(vitals.marketing)).arg(m_marketing->value()));
    m_summary->setText(vitals.summary());
    m_stage->setText(coaching.stage.toUpper());
    m_headline->setText(coaching.headline);
    m_advice->setText(coaching.advice);
    m_actionPage = coaching.page;
    m_action->setText(coaching.action);
    m_action->setVisible(!coaching.page.isEmpty() && !coaching.action.isEmpty());
    m_playbookTitle->setText(
        QStringLiteral("PLAYBOOK · %1 OF %2 DONE").arg(coaching.doneCount).arg(coaching.steps.size()));
    m_playbookTitle->setVisible(!coaching.steps.isEmpty());

    while (QLayoutItem *item = m_seenLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    for (const GenieSuggestion &suggestion : coaching.suggestions) {
        auto *row = new QWidget;
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(6);
        auto *text = new QLabel(QStringLiteral("• ") + suggestion.text);
        text->setWordWrap(true);
        line->addWidget(text, 1);
        if (!suggestion.page.isEmpty()) {
            auto *go = new QPushButton(QStringLiteral("Go"));
            go->setObjectName(QStringLiteral("genieGo"));
            go->setCursor(Qt::PointingHandCursor);
            go->setToolTip(suggestion.action);
            const QString page = suggestion.page;
            connect(go, &QPushButton::clicked, this, [this, page] { emit navigate(page); });
            line->addWidget(go, 0, Qt::AlignTop);
        }
        m_seenLayout->addWidget(row);
    }
    m_seenTitle->setVisible(!coaching.suggestions.isEmpty());
    m_seen->setVisible(!coaching.suggestions.isEmpty());

    while (QLayoutItem *item = m_stepsLayout->takeAt(0)) {
        if (QWidget *widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    for (int i = 0; i < coaching.steps.size(); ++i) {
        const GenieMarketingStep &step = coaching.steps.at(i);
        auto *row = new QWidget;
        row->setToolTip(step.detail);
        auto *line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(6);
        auto *box = new QCheckBox;
        box->setChecked(step.done);
        box->setEnabled(!step.automatic || !step.done);
        box->setToolTip(step.automatic && step.done ? QStringLiteral("Genie saw this happen")
                                                     : QStringLiteral("Tick it off when you have done it"));
        const QString id = step.id;
        connect(box, &QCheckBox::toggled, this, [this, id](bool on) { emit stepTicked(id, on); });
        auto *text = new QWidget;
        auto *stack = new QVBoxLayout(text);
        stack->setContentsMargins(0, 0, 0, 0);
        stack->setSpacing(0);
        auto *name = new QLabel((i == coaching.next ? QStringLiteral("▸ ") : QString()) + step.title);
        name->setWordWrap(true);
        if (step.done)
            name->setObjectName(QStringLiteral("genieStepDone"));
        else if (i == coaching.next)
            name->setObjectName(QStringLiteral("genieStepNext"));
        stack->addWidget(name);
        if (!step.progress.isEmpty()) {
            auto *progress = new QLabel(step.progress);
            progress->setObjectName(QStringLiteral("genieStepProgress"));
            stack->addWidget(progress);
        }
        if (i == coaching.next) {
            auto *detail = new QLabel(step.detail);
            detail->setObjectName(QStringLiteral("genieMuted"));
            detail->setWordWrap(true);
            stack->addWidget(detail);
        }
        line->addWidget(box, 0, Qt::AlignTop);
        line->addWidget(text, 1);
        if (!step.page.isEmpty() && !step.done) {
            auto *go = new QPushButton(QStringLiteral("Go"));
            go->setObjectName(QStringLiteral("genieGo"));
            go->setCursor(Qt::PointingHandCursor);
            go->setToolTip(step.action);
            const QString page = step.page;
            connect(go, &QPushButton::clicked, this, [this, page] { emit navigate(page); });
            line->addWidget(go, 0, Qt::AlignTop);
        }
        m_stepsLayout->addWidget(row);
    }
    m_measured->setText(measuring ? QStringLiteral("Measuring…")
                                  : QStringLiteral("Last 7 days · watching %1").arg(vitals.watch.described()));
}

void GenieCoachPanel::place(const QRect &anchor)
{
    if (!parentWidget())
        return;
    const QRect host = parentWidget()->rect().adjusted(8, 8, -8, -8);
    const int height = std::min(620, host.height());
    setFixedHeight(height);
    int x = anchor.center().x() - width() / 2;
    int y = anchor.top() - height - 4;
    if (y < host.top()) {
        x = anchor.left() - width() - 8;
        if (x < host.left())
            x = anchor.right() + 8;
        y = anchor.bottom() - height;
    }
    x = std::clamp(x, host.left(), std::max(host.left(), host.right() - width()));
    y = std::clamp(y, host.top(), std::max(host.top(), host.bottom() - height));
    move(x, y);
    raise();
}

void GenieCoachPanel::paintEvent(QPaintEvent *)
{
    paintCard(this, QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 14);
}
