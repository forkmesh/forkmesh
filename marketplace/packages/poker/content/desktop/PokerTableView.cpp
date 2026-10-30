#include "PokerTableView.h"

#include <QDateTime>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QTimer>
#include <QtMath>

#include <algorithm>
#include <cmath>

namespace forkmesh::poker {
namespace {

QPainterPath stadium(const QRectF &rect)
{
    QPainterPath path;
    const double radius = std::min(rect.width(), rect.height()) / 2.0;
    path.addRoundedRect(rect, radius, radius);
    return path;
}

QColor alpha(QColor color, double value)
{
    color.setAlphaF(float(std::clamp(value, 0.0, 1.0)));
    return color;
}

QPointF lerp(const QPointF &from, const QPointF &to, double progress)
{
    return from + (to - from) * progress;
}

double ease(double progress)
{
    const double clamped = std::clamp(progress, 0.0, 1.0);
    return 1.0 - std::pow(1.0 - clamped, 3.0);
}

QFont sized(const QFont &base, double pixels, int weight = QFont::Normal)
{
    QFont font(base);
    font.setPixelSize(std::max(8, int(std::lround(pixels))));
    font.setWeight(QFont::Weight(weight));
    return font;
}

QColor nameColor(const QString &name)
{
    const QList<QColor> palette{QColor("#5c6bc0"), QColor("#26a69a"), QColor("#ef5350"), QColor("#ab47bc"),
                                QColor("#ffa726"), QColor("#42a5f5"), QColor("#8d6e63"), QColor("#66bb6a"),
                                QColor("#ec407a")};
    uint hash = 0;
    for (const QChar character : name)
        hash = hash * 31u + uint(character.unicode());
    return palette.at(int(hash % uint(palette.size())));
}

QColor actionColor(const QString &kind)
{
    if (kind == QLatin1String("fold") || kind == QLatin1String("timeout"))
        return QColor("#6d6d6d");
    if (kind == QLatin1String("check"))
        return QColor("#4f7cac");
    if (kind == QLatin1String("call"))
        return QColor("#1e88e5");
    if (kind == QLatin1String("bet") || kind == QLatin1String("raise"))
        return QColor("#e65100");
    if (kind == QLatin1String("allin") || kind == QLatin1String("all in"))
        return QColor("#c62828");
    if (kind.contains(QLatin1String("blind")) || kind == QLatin1String("sb") || kind == QLatin1String("bb"))
        return QColor("#5d4037");
    return QColor("#455a64");
}

void pill(QPainter &painter, const QRectF &rect, const QColor &fill, const QColor &border = QColor())
{
    painter.setPen(border.isValid() ? QPen(border, 1) : Qt::NoPen);
    painter.setBrush(fill);
    painter.drawRoundedRect(rect, rect.height() / 2.0, rect.height() / 2.0);
}

}

PokerTableView::PokerTableView(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("pokerTableView"));
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setMinimumSize(minimumSizeHint());
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(QStringLiteral("Poker table"));
    m_clock.start();
    m_frames = new QTimer(this);
    m_frames->setTimerType(Qt::PreciseTimer);
    connect(m_frames, &QTimer::timeout, this, [this] { frame(); });
    relayout();
}

qint64 PokerTableView::serverNow() const
{
    return QDateTime::currentMSecsSinceEpoch() + m_serverOffset;
}

qint64 PokerTableView::clockNow() const
{
    return m_clock.elapsed();
}

void PokerTableView::setAvatarSource(std::function<QPixmap(const QString &)> source)
{
    m_avatarSource = std::move(source);
    m_avatars.clear();
    update();
}

void PokerTableView::avatarsChanged()
{
    m_avatars.clear();
    update();
}

void PokerTableView::clearTable()
{
    m_table = PokerTable();
    m_previous = PokerTable();
    m_anims.clear();
    relayout();
    update();
}

void PokerTableView::setTable(const PokerTable &table, const QList<PokerEvent> &fresh)
{
    const bool sameTable = m_table.valid && m_table.id == table.id;
    const PokerTable previous = m_table;
    const bool relayoutNeeded = !sameTable || previous.seatCount != table.seatCount || previous.mySeat != table.mySeat;
    m_previous = previous;
    m_table = table;
    if (relayoutNeeded)
        relayout();
    if (!sameTable || relayoutNeeded) {
        m_anims.clear();
    } else if (!fresh.isEmpty()) {
        schedule(previous, fresh);
    }
    if (!m_table.myTurn())
        m_lastTick = -1;
    QStringList seated;
    for (const PokerSeat &seat : m_table.seats)
        if (!seat.empty)
            seated << seat.displayName();
    setAccessibleDescription(QStringLiteral("%1, %2. %3").arg(m_table.name, phaseLabel(m_table.phase), seated.join(QStringLiteral(", "))));
    startFrames();
    update();
}

void PokerTableView::finishAnimations()
{
    m_anims.clear();
    update();
}

void PokerTableView::relayout()
{
    const int seats = m_table.valid && m_table.seatCount > 0 ? m_table.seatCount : 6;
    m_layout = PokerLayout::compute(QSizeF(size()), seats, m_table.valid ? m_table.mySeat : -1);
    m_cards.clear();
    m_chips.clear();
    m_avatars.clear();
    m_background = QPixmap();
}

void PokerTableView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
    m_anims.clear();
}

void PokerTableView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
        relayout();
}

int PokerTableView::seatAt(const QPointF &point) const
{
    for (const PokerSeatGeometry &geometry : m_layout.seats) {
        const double radius = m_layout.avatarRadius * 1.35;
        if (std::hypot(point.x() - geometry.avatar.x(), point.y() - geometry.avatar.y()) <= radius)
            return geometry.seat;
    }
    return -1;
}

void PokerTableView::mousePressEvent(QMouseEvent *event)
{
    const int seat = seatAt(event->position());
    const PokerSeat *entry = m_table.seat(seat);
    if (event->button() == Qt::LeftButton && entry && entry->empty && !m_table.seated() && emptySeatClicked) {
        emptySeatClicked(seat);
        return;
    }
    QWidget::mousePressEvent(event);
}

void PokerTableView::mouseMoveEvent(QMouseEvent *event)
{
    const int seat = seatAt(event->position());
    const PokerSeat *entry = m_table.seat(seat);
    const int hover = entry && entry->empty && !m_table.seated() && m_table.valid ? seat : -1;
    if (hover != m_hoverSeat) {
        m_hoverSeat = hover;
        setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        setToolTip(hover >= 0 ? QStringLiteral("Sit down in seat %1").arg(hover + 1) : QString());
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void PokerTableView::leaveEvent(QEvent *event)
{
    if (m_hoverSeat >= 0) {
        m_hoverSeat = -1;
        unsetCursor();
        update();
    }
    QWidget::leaveEvent(event);
}

QSizeF PokerTableView::holeCardSize(int seat) const
{
    const PokerSeatGeometry *geometry = m_layout.seat(seat);
    if (geometry && geometry->bottom)
        return m_layout.myCard;
    const PokerSeat *entry = m_table.seat(seat);
    if (entry && holeCardsFaceUp(*entry))
        return m_layout.card * 0.84;
    return m_layout.card * 0.7;
}

QPointF PokerTableView::holeCardCentre(int seat, int index) const
{
    const PokerSeatGeometry *geometry = m_layout.seat(seat);
    if (!geometry)
        return m_layout.centre;
    const QSizeF size = holeCardSize(seat);
    const double sign = index == 0 ? -1.0 : 1.0;
    if (geometry->bottom)
        return geometry->cards + QPointF(sign * size.width() * 0.53, index == 0 ? 2 * m_layout.scale : 0);
    return geometry->cards + QPointF(sign * size.width() * 0.34, 0);
}

double PokerTableView::holeCardAngle(int seat, int index) const
{
    const PokerSeatGeometry *geometry = m_layout.seat(seat);
    const double sign = index == 0 ? -1.0 : 1.0;
    if (geometry && geometry->bottom)
        return sign * 4.0;
    return sign * 7.0;
}

QPointF PokerTableView::buttonPoint(int seat) const
{
    const PokerSeatGeometry *geometry = m_layout.seat(seat);
    return geometry ? geometry->button : m_layout.centre;
}

bool PokerTableView::holeCardsFaceUp(const PokerSeat &seat) const
{
    if (seat.cards.isEmpty())
        return false;
    for (const PokerCard &card : seat.cards)
        if (card.hidden)
            return false;
    return true;
}

void PokerTableView::addAnim(Anim anim)
{
    m_anims << anim;
}

qint64 PokerTableView::animationsEnd() const
{
    qint64 end = 0;
    for (const Anim &anim : m_anims)
        end = std::max(end, anim.start + anim.duration);
    return end;
}

void PokerTableView::schedule(const PokerTable &previous, const QList<PokerEvent> &fresh)
{
    const qint64 now = clockNow();
    qint64 at = std::max(now, animationsEnd());
    if (at - now > 2600) {
        m_anims.clear();
        at = now;
    }
    const double s = m_layout.scale;
    const QPointF tray(m_layout.centre.x(), m_layout.felt.top() + 18 * s);
    QHash<int, qint64> bets;
    for (const PokerSeat &seat : previous.seats)
        if (!seat.empty && seat.bet > 0)
            bets.insert(seat.index, seat.bet);
    qint64 heldPot = 0;
    for (const PokerPot &pot : previous.pots)
        heldPot += pot.amount;
    m_heldPot = heldPot;

    const auto sound = [this](qint64 start, const QString &name, float volume = 1.0f) {
        Anim anim;
        anim.kind = AnimKind::Sound;
        anim.start = start;
        anim.duration = 1;
        anim.sound = name;
        anim.volume = volume;
        addAnim(anim);
    };
    const auto chips = [this](qint64 start, const QPointF &from, const QPointF &to, qint64 amount,
                              const QString &key, qint64 duration) {
        Anim anim;
        anim.kind = AnimKind::Chips;
        anim.start = start;
        anim.duration = duration;
        anim.from = from;
        anim.to = to;
        anim.amount = amount;
        anim.key = key;
        addAnim(anim);
    };

    if (previous.dealer >= 0 && m_table.dealer >= 0 && previous.dealer != m_table.dealer &&
        m_layout.seat(previous.dealer) && m_layout.seat(m_table.dealer)) {
        Anim anim;
        anim.kind = AnimKind::Button;
        anim.start = at;
        anim.duration = 650;
        anim.from = buttonPoint(previous.dealer);
        anim.to = buttonPoint(m_table.dealer);
        anim.key = QStringLiteral("button");
        addAnim(anim);
    }

    for (const PokerEvent &event : fresh) {
        const QString &kind = event.kind;
        const PokerSeatGeometry *geometry = m_layout.seat(event.seat);
        if (kind == QLatin1String("shuffle")) {
            sound(at, QStringLiteral("shuffle"));
            at += 420;
        } else if (kind == QLatin1String("deal")) {
            QList<int> order;
            const int count = std::max(1, m_table.seatCount);
            for (int step = 1; step <= count; ++step) {
                const int index = (std::max(0, m_table.dealer) + step) % count;
                const PokerSeat *seat = m_table.seat(index);
                if (seat && !seat->empty && !seat->cards.isEmpty())
                    order << index;
            }
            for (int round = 0; round < 2; ++round) {
                for (const int index : std::as_const(order)) {
                    const PokerSeat *seat = m_table.seat(index);
                    Anim anim;
                    anim.kind = AnimKind::Card;
                    anim.start = at;
                    anim.duration = 280;
                    anim.from = tray;
                    anim.to = holeCardCentre(index, round);
                    anim.size = holeCardSize(index);
                    anim.fromAngle = -25;
                    anim.toAngle = holeCardAngle(index, round);
                    const bool mine = index == m_table.mySeat && seat->cards.size() > round && !seat->cards.at(round).hidden;
                    anim.card = mine ? seat->cards.at(round) : PokerCard::parse(QStringLiteral("??"));
                    anim.reveal = mine;
                    anim.key = QStringLiteral("hole:%1:%2").arg(index).arg(round);
                    anim.sound = QStringLiteral("deal");
                    anim.volume = 0.55f;
                    addAnim(anim);
                    at += 85;
                }
            }
            at += 220;
        } else if (kind == QLatin1String("blinds")) {
            bool any = false;
            for (const auto &[index, fallback] : {qMakePair(m_table.smallBlind, m_table.small),
                                                  qMakePair(m_table.bigBlind, m_table.big)}) {
                const PokerSeatGeometry *blind = m_layout.seat(index);
                const PokerSeat *seat = m_table.seat(index);
                if (!blind || !seat || seat->empty)
                    continue;
                const qint64 amount = m_table.phase == QLatin1String("preflop") && seat->bet > 0
                    ? std::min(seat->bet, fallback) : fallback;
                chips(at, blind->avatar, blind->bet, amount, QStringLiteral("bet:%1").arg(index), 340);
                bets[index] = bets.value(index) + amount;
                any = true;
            }
            if (any)
                sound(at, QStringLiteral("chip"), 0.8f);
            at += 360;
        } else if (kind == QLatin1String("call") || kind == QLatin1String("bet") || kind == QLatin1String("raise") ||
                   kind == QLatin1String("allin")) {
            if (geometry) {
                const PokerSeat *seat = m_table.seat(event.seat);
                qint64 amount = event.amount;
                if (amount <= 0 && seat)
                    amount = seat->bet;
                if (amount > 0) {
                    chips(at, geometry->avatar, geometry->bet, amount, QStringLiteral("bet:%1").arg(event.seat), 340);
                    bets[event.seat] = bets.value(event.seat) + amount;
                }
            }
            sound(at, kind == QLatin1String("allin") ? QStringLiteral("allin") : QStringLiteral("chip"));
            at += kind == QLatin1String("allin") ? 520 : 320;
        } else if (kind == QLatin1String("check")) {
            sound(at, QStringLiteral("check"));
            at += 240;
        } else if (kind == QLatin1String("fold")) {
            const PokerSeat *before = previous.seat(event.seat);
            if (geometry && before && !before->cards.isEmpty() && event.seat != m_table.mySeat) {
                for (int index = 0; index < 2; ++index) {
                    Anim anim;
                    anim.kind = AnimKind::Fold;
                    anim.start = at;
                    anim.duration = 360;
                    anim.from = holeCardCentre(event.seat, index);
                    anim.to = tray;
                    anim.size = m_layout.card * 0.7;
                    anim.fromAngle = holeCardAngle(event.seat, index);
                    anim.toAngle = index == 0 ? 160 : 200;
                    addAnim(anim);
                }
            }
            sound(at, QStringLiteral("fold"));
            at += 300;
        } else if (streetEndEvent(kind)) {
            bool gathered = false;
            for (auto bet = bets.cbegin(); bet != bets.cend(); ++bet) {
                const PokerSeatGeometry *from = m_layout.seat(bet.key());
                if (!from || bet.value() <= 0)
                    continue;
                chips(at, from->bet, m_layout.pot, bet.value(), QStringLiteral("potHold"), 420);
                gathered = true;
            }
            bets.clear();
            if (gathered) {
                sound(at, QStringLiteral("chip"), 0.7f);
                at += 440;
            }
            int first = -1;
            int last = -1;
            if (kind == QLatin1String("flop")) {
                first = 0;
                last = 2;
            } else if (kind == QLatin1String("turn")) {
                first = last = 3;
            } else if (kind == QLatin1String("river")) {
                first = last = 4;
            }
            for (int index = first; index >= 0 && index <= last && index < m_table.board.size(); ++index) {
                Anim anim;
                anim.kind = AnimKind::Card;
                anim.start = at;
                anim.duration = 380;
                anim.from = tray;
                anim.to = m_layout.boardCard(index);
                anim.size = m_layout.card;
                anim.fromAngle = -30;
                anim.toAngle = 0;
                anim.card = m_table.board.at(index);
                anim.reveal = true;
                anim.key = QStringLiteral("board:%1").arg(index);
                anim.sound = QStringLiteral("deal");
                anim.volume = 0.8f;
                addAnim(anim);
                at += 140;
            }
            if (first >= 0)
                at += 240;
            if (kind == QLatin1String("showdown")) {
                bool flipped = false;
                for (const PokerSeat &seat : m_table.seats) {
                    const PokerSeat *before = previous.seat(seat.index);
                    if (seat.empty || seat.index == m_table.mySeat || !holeCardsFaceUp(seat) ||
                        (before && holeCardsFaceUp(*before)))
                        continue;
                    for (int index = 0; index < std::min<int>(2, int(seat.cards.size())); ++index) {
                        Anim anim;
                        anim.kind = AnimKind::Flip;
                        anim.start = at;
                        anim.duration = 380;
                        anim.from = holeCardCentre(seat.index, index);
                        anim.size = holeCardSize(seat.index);
                        anim.fromAngle = holeCardAngle(seat.index, index);
                        anim.card = seat.cards.at(index);
                        anim.key = QStringLiteral("hole:%1:%2").arg(seat.index).arg(index);
                        addAnim(anim);
                    }
                    flipped = true;
                }
                if (flipped) {
                    sound(at, QStringLiteral("deal"), 0.7f);
                    at += 520;
                }
            }
        } else if (kind == QLatin1String("win")) {
            if (geometry) {
                qint64 amount = event.amount;
                const PokerSeat *seat = m_table.seat(event.seat);
                if (amount <= 0 && seat)
                    amount = seat->won;
                chips(at, m_layout.pot, geometry->avatar, std::max<qint64>(amount, 1), QStringLiteral("potWin"), 700);
            }
            sound(at, event.seat == m_table.mySeat && m_table.mySeat >= 0 ? QStringLiteral("win") : QStringLiteral("chip"));
            at += 560;
        }
    }
}

void PokerTableView::startFrames()
{
    if (m_anims.isEmpty() && !turnClockRunning()) {
        m_frames->stop();
        return;
    }
    const int interval = m_anims.isEmpty() ? 66 : 16;
    if (!m_frames->isActive() || m_frames->interval() != interval)
        m_frames->start(interval);
}

bool PokerTableView::turnClockRunning() const
{
    return m_table.valid && m_table.handRunning() && m_table.toAct >= 0 && m_table.actDeadline > 0 &&
           serverNow() < m_table.actDeadline + 500;
}

void PokerTableView::frame()
{
    const qint64 now = clockNow();
    const bool wasAnimating = !m_anims.isEmpty();
    for (Anim &anim : m_anims) {
        if (!anim.sounded && now >= anim.start) {
            anim.sounded = true;
            if (!anim.sound.isEmpty() && soundRequested)
                soundRequested(anim.sound, anim.volume);
        }
    }
    m_anims.erase(std::remove_if(m_anims.begin(), m_anims.end(),
                                 [now](const Anim &anim) { return anim.sounded && now >= anim.start + anim.duration; }),
                  m_anims.end());
    if (m_table.myTurn()) {
        const PokerClock clock = turnClock(serverNow(), m_table.actStartedAt, m_table.actDeadline);
        const int second = int(std::ceil(clock.remaining));
        if (clock.running && second >= 1 && second <= 5 && second != m_lastTick) {
            m_lastTick = second;
            if (soundRequested)
                soundRequested(QStringLiteral("clock"), 0.8f);
        } else if (second > 5) {
            m_lastTick = -1;
        }
    }
    const PokerSeatGeometry *acting = m_layout.seat(m_table.toAct);
    if (!wasAnimating && acting) {
        const double reach = m_layout.avatarRadius * 1.12 + 12 * m_layout.scale;
        update(QRectF(acting->avatar.x() - reach, acting->avatar.y() - reach, reach * 2, reach * 2).toAlignedRect());
    } else {
        update();
    }
    startFrames();
}

QSet<QString> PokerTableView::heldKeys(qint64 now) const
{
    QSet<QString> held;
    for (const Anim &anim : m_anims)
        if (!anim.key.isEmpty() && now < anim.start + anim.duration)
            held.insert(anim.key);
    return held;
}

qint64 PokerTableView::potShown(const QSet<QString> &held, qint64 now) const
{
    if (held.contains(QStringLiteral("potHold")))
        return m_heldPot;
    qint64 waiting = 0;
    bool winning = false;
    for (const Anim &anim : m_anims) {
        if (anim.key != QLatin1String("potWin"))
            continue;
        winning = true;
        if (now < anim.start)
            waiting += anim.amount;
    }
    bool paid = false;
    for (const PokerSeat &seat : m_table.seats)
        paid = paid || (!seat.empty && seat.winner);
    if (winning || (m_table.phase == QLatin1String("showdown") && paid))
        return waiting;
    qint64 total = 0;
    for (const PokerPot &pot : m_table.pots)
        total += pot.amount;
    if (m_table.pots.isEmpty() && m_table.potTotal > 0)
        total = std::max<qint64>(0, m_table.potTotal - m_table.streetBets());
    return total;
}

void PokerTableView::ensureBackground()
{
    const qreal ratio = devicePixelRatioF();
    if (!m_background.isNull() && m_backgroundSize == size() && qFuzzyCompare(m_backgroundRatio, ratio))
        return;
    m_backgroundSize = size();
    m_backgroundRatio = ratio;
    m_background = QPixmap(QSize(std::max(1, int(std::ceil(width() * ratio))), std::max(1, int(std::ceil(height() * ratio)))));
    m_background.setDevicePixelRatio(ratio);
    m_background.fill(Qt::black);
    QPainter painter(&m_background);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    paintBackground(painter, size());
}

void PokerTableView::paintBackground(QPainter &painter, const QSize &area)
{
    const double s = m_layout.scale;
    const QRectF room(QPointF(0, 0), QSizeF(area));
    QRadialGradient roomLight(QPointF(room.center().x(), m_layout.rail.center().y() - room.height() * 0.08),
                              std::max(room.width(), room.height()) * 0.72);
    roomLight.setColorAt(0.0, QColor("#2b2622"));
    roomLight.setColorAt(0.45, QColor("#17151a"));
    roomLight.setColorAt(1.0, QColor("#060607"));
    painter.fillRect(room, roomLight);

    QRadialGradient carpet(m_layout.rail.center(), m_layout.rail.width() * 0.75);
    carpet.setColorAt(0.0, QColor(120, 70, 40, 34));
    carpet.setColorAt(1.0, QColor(0, 0, 0, 0));
    painter.fillRect(room, carpet);

    paintDealer(painter);

    for (int ring = 7; ring >= 1; --ring) {
        const double grow = ring * 4.0 * s;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 22));
        painter.drawPath(stadium(m_layout.rail.adjusted(-grow, -grow * 0.6 + 10 * s, grow, grow + 12 * s)));
    }

    const QRectF rail = m_layout.rail;
    const double walnut = 17 * s;
    const double leatherWidth = 15 * s;
    QLinearGradient wood(rail.topLeft(), rail.bottomLeft());
    wood.setColorAt(0.0, QColor("#8a5428"));
    wood.setColorAt(0.18, QColor("#6b3b19"));
    wood.setColorAt(0.55, QColor("#4a2610"));
    wood.setColorAt(1.0, QColor("#2a1407"));
    painter.setPen(QPen(QColor("#160a03"), 1.2 * s));
    painter.setBrush(wood);
    painter.drawPath(stadium(rail));
    QRandomGenerator grain(17);
    painter.setBrush(Qt::NoBrush);
    for (int line = 1; line < 12; ++line) {
        const double inset = line * walnut / 12.0;
        const int tone = int(grain.bounded(0, 3));
        const QColor color = tone == 0 ? QColor(30, 14, 4, 60) : tone == 1 ? QColor(150, 95, 50, 34) : QColor(70, 35, 12, 50);
        painter.setPen(QPen(color, (0.5 + grain.generateDouble() * 0.7) * s));
        painter.drawPath(stadium(rail.adjusted(inset, inset, -inset, -inset)));
    }
    QLinearGradient shine(rail.topLeft(), QPointF(rail.left(), rail.top() + rail.height() * 0.5));
    shine.setColorAt(0.0, QColor(255, 220, 170, 90));
    shine.setColorAt(1.0, QColor(255, 220, 170, 0));
    painter.setPen(QPen(QBrush(shine), 2.2 * s));
    painter.drawPath(stadium(rail.adjusted(2.2 * s, 2.2 * s, -2.2 * s, -2.2 * s)));

    const QRectF leather = rail.adjusted(walnut, walnut, -walnut, -walnut);
    QLinearGradient hide(leather.topLeft(), leather.bottomLeft());
    hide.setColorAt(0.0, QColor("#2c2c2e"));
    hide.setColorAt(0.5, QColor("#161617"));
    hide.setColorAt(1.0, QColor("#0b0b0c"));
    painter.setPen(QPen(QColor("#050505"), 1.0 * s));
    painter.setBrush(hide);
    painter.drawPath(stadium(leather));
    QLinearGradient sheen(leather.topLeft(), QPointF(leather.left(), leather.top() + leather.height() * 0.6));
    sheen.setColorAt(0.0, QColor(255, 255, 255, 40));
    sheen.setColorAt(1.0, QColor(255, 255, 255, 0));
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QBrush(sheen), leatherWidth * 0.45));
    const double middle = leatherWidth * 0.5;
    painter.drawPath(stadium(leather.adjusted(middle, middle, -middle, -middle)));
    QPen stitch(QColor(120, 110, 95, 110), 0.9 * s, Qt::CustomDashLine);
    stitch.setDashPattern({3.0, 2.6});
    painter.setPen(stitch);
    painter.drawPath(stadium(leather.adjusted(2.6 * s, 2.6 * s, -2.6 * s, -2.6 * s)));
    painter.drawPath(stadium(leather.adjusted(leatherWidth - 2.6 * s, leatherWidth - 2.6 * s,
                                              -leatherWidth + 2.6 * s, -leatherWidth + 2.6 * s)));

    const QRectF felt = m_layout.felt;
    const QPainterPath feltPath = stadium(felt);
    QRadialGradient cloth(QPointF(0, 0), 1.0);
    cloth.setColorAt(0.0, QColor("#10683f"));
    cloth.setColorAt(0.35, QColor("#0d5c3a"));
    cloth.setColorAt(0.75, QColor("#0a4a2f"));
    cloth.setColorAt(1.0, QColor("#083d27"));
    QBrush clothBrush(cloth);
    QTransform clothTransform;
    clothTransform.translate(felt.center().x(), felt.center().y() - felt.height() * 0.06);
    clothTransform.scale(felt.width() * 0.62, felt.height() * 0.72);
    clothBrush.setTransform(clothTransform);
    painter.setPen(Qt::NoPen);
    painter.setBrush(clothBrush);
    painter.drawPath(feltPath);

    painter.save();
    painter.setClipPath(feltPath);
    const qreal ratio = painter.device()->devicePixelRatioF();
    QImage weave(64, 64, QImage::Format_ARGB32_Premultiplied);
    weave.fill(Qt::transparent);
    QRandomGenerator threads(5);
    for (int y = 0; y < 64; ++y) {
        for (int x = 0; x < 64; ++x) {
            const bool warp = ((x / 2) + (y / 2)) % 2 == 0;
            const int noise = int(threads.bounded(0, 12));
            const int value = warp ? 255 : 0;
            const int strength = (warp ? 6 : 10) + noise;
            weave.setPixel(x, y, qPremultiply(qRgba(value, value, value, strength)));
        }
    }
    QPixmap tile = QPixmap::fromImage(weave);
    tile.setDevicePixelRatio(std::max<qreal>(1.0, ratio));
    painter.setBrush(QBrush(tile));
    painter.drawRect(felt);
    for (int band = 0; band < 3; ++band) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(QColor(0, 0, 0, 46 - band * 12), (18 - band * 5) * s));
        painter.drawPath(feltPath);
    }
    painter.restore();

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(QColor(212, 175, 55, 70), 3.2 * s));
    painter.drawPath(stadium(m_layout.inset));
    painter.setPen(QPen(QColor(232, 196, 92, 200), 1.3 * s));
    painter.drawPath(stadium(m_layout.inset));

    const QPointF mark = m_layout.centre + QPointF(0, 2 * s);
    const double hexRadius = felt.height() * 0.2;
    QPolygonF hexagon;
    for (int corner = 0; corner < 6; ++corner) {
        const double angle = qDegreesToRadians(-90.0 + corner * 60.0);
        hexagon << mark + QPointF(std::cos(angle) * hexRadius, std::sin(angle) * hexRadius);
    }
    QPen logo(QColor(255, 255, 255, 16), 5 * s);
    logo.setJoinStyle(Qt::RoundJoin);
    painter.setPen(logo);
    painter.drawPolygon(hexagon);
    painter.setPen(QPen(QColor(255, 255, 255, 13), 4 * s, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(mark, hexagon.at(0));
    painter.drawLine(mark, hexagon.at(2));
    painter.drawLine(mark, hexagon.at(4));
    painter.setPen(QColor(255, 255, 255, 20));
    QFont brand = sized(font(), 15 * s, QFont::DemiBold);
    brand.setLetterSpacing(QFont::AbsoluteSpacing, 6 * s);
    painter.setFont(brand);
    painter.drawText(QRectF(mark.x() - 200 * s, mark.y() + hexRadius + 6 * s, 400 * s, 24 * s),
                     Qt::AlignCenter, QStringLiteral("FORKMESH"));

    painter.setBrush(Qt::NoBrush);
    for (int index = 0; index < 5; ++index) {
        const QPointF centre = m_layout.boardCard(index);
        const QRectF slot(centre.x() - m_layout.card.width() / 2, centre.y() - m_layout.card.height() / 2,
                          m_layout.card.width(), m_layout.card.height());
        painter.setPen(QPen(QColor(255, 255, 255, 26), 1.1 * s));
        painter.drawRoundedRect(slot, 5 * s, 5 * s);
    }

    paintDealerTray(painter);
}

void PokerTableView::paintDealer(QPainter &painter)
{
    const double s = m_layout.scale;
    const double x = m_layout.centre.x();
    const double top = m_layout.rail.top();
    QRadialGradient halo(QPointF(x, top - 20 * s), 90 * s);
    halo.setColorAt(0.0, QColor(255, 220, 170, 40));
    halo.setColorAt(1.0, QColor(255, 220, 170, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(halo);
    painter.drawEllipse(QPointF(x, top - 20 * s), 90 * s, 70 * s);

    const QRectF body(x - 60 * s, top - 28 * s, 120 * s, 70 * s);
    QLinearGradient jacket(body.topLeft(), body.bottomLeft());
    jacket.setColorAt(0.0, QColor("#2a2a30"));
    jacket.setColorAt(1.0, QColor("#0c0c10"));
    painter.setBrush(jacket);
    painter.drawRoundedRect(body, 26 * s, 26 * s);
    QPainterPath shirt;
    shirt.moveTo(x - 17 * s, top - 28 * s);
    shirt.lineTo(x + 17 * s, top - 28 * s);
    shirt.lineTo(x, top + 4 * s);
    shirt.closeSubpath();
    painter.setBrush(QColor("#f2f2f2"));
    painter.drawPath(shirt);
    QPainterPath vest;
    vest.moveTo(x - 30 * s, top - 22 * s);
    vest.lineTo(x - 12 * s, top - 22 * s);
    vest.lineTo(x, top + 4 * s);
    vest.lineTo(x + 12 * s, top - 22 * s);
    vest.lineTo(x + 30 * s, top - 22 * s);
    vest.lineTo(x + 30 * s, top + 20 * s);
    vest.lineTo(x - 30 * s, top + 20 * s);
    vest.closeSubpath();
    painter.setBrush(QColor("#7a1212"));
    painter.drawPath(vest);
    painter.setBrush(QColor("#c62828"));
    QPolygonF left{QPointF(x, top - 22 * s), QPointF(x - 10 * s, top - 27 * s), QPointF(x - 10 * s, top - 17 * s)};
    QPolygonF right{QPointF(x, top - 22 * s), QPointF(x + 10 * s, top - 27 * s), QPointF(x + 10 * s, top - 17 * s)};
    painter.drawPolygon(left);
    painter.drawPolygon(right);
    painter.drawEllipse(QPointF(x, top - 22 * s), 2.6 * s, 2.6 * s);
    painter.setBrush(QColor("#c99b74"));
    painter.drawRoundedRect(QRectF(x - 6 * s, top - 36 * s, 12 * s, 10 * s), 3 * s, 3 * s);
    const QPointF head(x, top - 44 * s);
    QRadialGradient skin(head + QPointF(-4 * s, -5 * s), 20 * s);
    skin.setColorAt(0.0, QColor("#f0c9a0"));
    skin.setColorAt(1.0, QColor("#b98558"));
    painter.setBrush(skin);
    painter.drawEllipse(head, 14 * s, 16 * s);
    QPainterPath hair;
    hair.moveTo(head + QPointF(-14.5 * s, 0));
    hair.cubicTo(head + QPointF(-15 * s, -22 * s), head + QPointF(15 * s, -22 * s), head + QPointF(14.5 * s, -1 * s));
    hair.cubicTo(head + QPointF(9 * s, -10 * s), head + QPointF(-6 * s, -12 * s), head + QPointF(-14.5 * s, 0));
    painter.setBrush(QColor("#2b1d14"));
    painter.drawPath(hair);
    painter.setBrush(QColor("#3a2618"));
    painter.drawEllipse(head + QPointF(-5 * s, 1 * s), 1.6 * s, 1.8 * s);
    painter.drawEllipse(head + QPointF(5 * s, 1 * s), 1.6 * s, 1.8 * s);
    painter.setPen(QPen(QColor("#7a4a32"), 1.2 * s, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    painter.drawArc(QRectF(head.x() - 5 * s, head.y() + 3 * s, 10 * s, 6 * s), 200 * 16, 140 * 16);
}

void PokerTableView::paintDealerTray(QPainter &painter)
{
    const double s = m_layout.scale;
    const QRectF tray(m_layout.centre.x() - 86 * s, m_layout.felt.top() + 5 * s, 172 * s, 26 * s);
    painter.setPen(QPen(QColor("#050505"), 1 * s));
    QLinearGradient body(tray.topLeft(), tray.bottomLeft());
    body.setColorAt(0.0, QColor("#2e2e30"));
    body.setColorAt(1.0, QColor("#111113"));
    painter.setBrush(body);
    painter.drawRoundedRect(tray, 5 * s, 5 * s);
    const QList<qint64> order{1, 5, 25, 100, 500, 1000, 5000, 100, 25, 5};
    const double slot = (tray.width() - 10 * s) / order.size();
    for (int index = 0; index < order.size(); ++index) {
        const QRectF stack(tray.left() + 5 * s + index * slot + 1.2 * s, tray.top() + 4 * s, slot - 2.4 * s,
                           tray.height() - 8 * s);
        QColor color(chipColor(order.at(index)));
        QLinearGradient chipSide(stack.topLeft(), stack.topRight());
        chipSide.setColorAt(0.0, color.darker(150));
        chipSide.setColorAt(0.5, color.lighter(115));
        chipSide.setColorAt(1.0, color.darker(160));
        painter.setPen(Qt::NoPen);
        painter.setBrush(chipSide);
        painter.drawRoundedRect(stack, 2 * s, 2 * s);
        painter.setPen(QPen(QColor(255, 255, 255, 120), 0.8 * s));
        for (double y = stack.top() + 2.5 * s; y < stack.bottom(); y += 3 * s)
            painter.drawLine(QPointF(stack.left() + stack.width() * 0.35, y), QPointF(stack.right() - stack.width() * 0.35, y));
    }
}

void PokerTableView::paintDealerLine(QPainter &painter)
{
    QString line = m_table.dealerLine;
    if (line.isEmpty())
        line = m_table.valid ? phaseLabel(m_table.phase) : QStringLiteral("Pick a table in the lobby");
    if (line.isEmpty())
        return;
    const double s = m_layout.scale;
    const QFont font = sized(this->font(), 13 * s, QFont::DemiBold);
    const QFontMetricsF metrics(font);
    const double width = std::min(metrics.horizontalAdvance(line) + 26 * s, m_layout.inset.width() * 0.62);
    const QRectF box(m_layout.dealerLine.x() - width / 2, m_layout.dealerLine.y() - 12 * s, width, 24 * s);
    pill(painter, box, QColor(0, 0, 0, 105), QColor(232, 196, 92, 90));
    painter.setFont(font);
    painter.setPen(QColor("#f4ecd8"));
    painter.drawText(box.adjusted(10 * s, 0, -10 * s, 0), Qt::AlignCenter,
                     metrics.elidedText(line, Qt::ElideRight, box.width() - 20 * s));
}

void PokerTableView::paintPot(QPainter &painter, const QSet<QString> &held)
{
    const double s = m_layout.scale;
    const qint64 shown = potShown(held, clockNow());
    if (shown > 0)
        paintChipStack(painter, m_layout.pot + QPointF(0, 6 * s), shown, false);
    qint64 total = m_table.potTotal;
    if (held.contains(QStringLiteral("potHold")) || held.contains(QStringLiteral("potWin")))
        total = std::max(total, shown);
    if (total <= 0 || (m_table.phase == QLatin1String("showdown") && shown <= 0))
        return;
    QString label = QStringLiteral("Pot %1").arg(amountText(total));
    if (m_table.pots.size() > 1) {
        QStringList parts;
        for (int index = 0; index < m_table.pots.size(); ++index)
            parts << (index == 0 ? QStringLiteral("Main %1") : QStringLiteral("Side %1"))
                         .arg(amountText(m_table.pots.at(index).amount));
        label += QStringLiteral("  ·  ") + parts.join(QStringLiteral("  ·  "));
    }
    const QFont font = sized(this->font(), 12.5 * s, QFont::Bold);
    const QFontMetricsF metrics(font);
    const double width = metrics.horizontalAdvance(label) + 20 * s;
    const QRectF box(m_layout.pot.x() - width / 2, m_layout.pot.y() + 9 * s, width, 20 * s);
    pill(painter, box, QColor(0, 0, 0, 120));
    painter.setFont(font);
    painter.setPen(QColor("#fff3c4"));
    painter.drawText(box, Qt::AlignCenter, label);
}

void PokerTableView::paintBoard(QPainter &painter, const QSet<QString> &held)
{
    QSet<QString> best;
    if (m_table.phase == QLatin1String("showdown") && m_table.lastHand.number == m_table.handNumber)
        for (const PokerWinner &winner : m_table.lastHand.winners)
            for (const PokerCard &card : winner.best)
                best.insert(card.code());
    for (int index = 0; index < m_table.board.size() && index < 5; ++index) {
        if (held.contains(QStringLiteral("board:%1").arg(index)))
            continue;
        const PokerCard &card = m_table.board.at(index);
        const bool lifted = best.contains(card.code());
        const double dim = !best.isEmpty() && !lifted ? 0.55 : 1.0;
        paintCard(painter, card, m_layout.boardCard(index) + QPointF(0, lifted ? -10 * m_layout.scale : 0),
                  m_layout.card, 0, 1, dim);
    }
}

void PokerTableView::paintAvatar(QPainter &painter, const PokerSeat &seat, const QPointF &centre, double radius)
{
    const double s = m_layout.scale;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 120));
    painter.drawEllipse(centre + QPointF(0, 2.5 * s), radius + 3 * s, radius + 3 * s);
    painter.setBrush(QColor("#101012"));
    painter.drawEllipse(centre, radius + 3 * s, radius + 3 * s);
    const QPixmap picture = avatarPixmap(seat.name + (seat.bot ? QStringLiteral("\x01") : QString()));
    if (!picture.isNull()) {
        const QRectF target(centre.x() - radius, centre.y() - radius, radius * 2, radius * 2);
        painter.drawPixmap(target, picture, QRectF(picture.rect()));
    }
    const bool mine = seat.index == m_table.mySeat;
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(mine ? QColor(232, 196, 92, 230) : QColor(255, 255, 255, 60), (mine ? 2.0 : 1.2) * s));
    painter.drawEllipse(centre, radius, radius);
}

QPixmap PokerTableView::avatarPixmap(const QString &key)
{
    const bool bot = key.endsWith(QLatin1Char('\x01'));
    const QString name = bot ? key.chopped(1) : key;
    const double radius = m_layout.avatarRadius * 1.12;
    const QString cacheKey = key + QLatin1Char('|') + QString::number(int(radius));
    const auto found = m_avatars.constFind(cacheKey);
    if (found != m_avatars.constEnd())
        return found.value();
    const qreal ratio = devicePixelRatioF();
    const int side = std::max(8, int(std::ceil(radius * 2 * ratio)));
    QPixmap out(side, side);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    QPainterPath clip;
    clip.addEllipse(QRectF(0, 0, side, side));
    painter.setClipPath(clip);
    QPixmap picture;
    if (!bot && m_avatarSource)
        picture = m_avatarSource(name);
    if (!picture.isNull()) {
        painter.drawPixmap(QRectF(0, 0, side, side), picture, QRectF(picture.rect()));
    } else {
        const QColor base = bot ? QColor("#546e7a") : nameColor(name);
        QLinearGradient fill(0, 0, 0, side);
        fill.setColorAt(0.0, base.lighter(135));
        fill.setColorAt(1.0, base.darker(140));
        painter.fillRect(QRectF(0, 0, side, side), fill);
        painter.setPen(QColor(255, 255, 255, 235));
        const QString initial = name.isEmpty() ? QStringLiteral("?") : name.left(1).toUpper();
        painter.setFont(sized(font(), side * 0.46, QFont::Bold));
        painter.drawText(QRectF(0, bot ? -side * 0.06 : 0, side, side), Qt::AlignCenter, initial);
        if (bot) {
            painter.setFont(sized(font(), side * 0.16, QFont::Bold));
            painter.setPen(QColor(255, 255, 255, 190));
            painter.drawText(QRectF(0, side * 0.6, side, side * 0.24), Qt::AlignCenter, QStringLiteral("BOT"));
        }
    }
    painter.end();
    m_avatars.insert(cacheKey, out);
    return out;
}

void PokerTableView::paintPlate(QPainter &painter, const PokerSeat &seat, const QPointF &centre, double radius)
{
    Q_UNUSED(radius);
    const double s = m_layout.scale;
    const QRectF plate(centre.x() - 62 * s, centre.y() - 19 * s, 124 * s, 38 * s);
    QLinearGradient fill(plate.topLeft(), plate.bottomLeft());
    const bool turn = seat.isTurn || (m_table.toAct == seat.index && m_table.handRunning());
    fill.setColorAt(0.0, turn ? QColor("#3a3320") : QColor("#24262b"));
    fill.setColorAt(1.0, turn ? QColor("#1d1a10") : QColor("#111215"));
    painter.setPen(QPen(turn ? QColor(232, 196, 92, 200) : QColor(255, 255, 255, 34), 1.0 * s));
    painter.setBrush(fill);
    painter.drawRoundedRect(plate, 8 * s, 8 * s);
    const QFont nameFont = sized(font(), 12 * s, QFont::DemiBold);
    const QFontMetricsF nameMetrics(nameFont);
    painter.setFont(nameFont);
    painter.setPen(QColor("#f1f1f1"));
    const QRectF nameRect = plate.adjusted(8 * s, 2 * s, -8 * s, -plate.height() / 2 + 1 * s);
    painter.drawText(nameRect, Qt::AlignCenter, nameMetrics.elidedText(seat.displayName(), Qt::ElideRight, nameRect.width()));
    const QString status = statusLabel(seat);
    QString second;
    QColor secondColor("#ffd54f");
    if (seat.status == QLatin1String("allin") || (seat.stack <= 0 && seat.inHand())) {
        second = QStringLiteral("ALL-IN");
        secondColor = QColor("#ff7961");
    } else if (seat.away || seat.status == QLatin1String("leaving") || seat.busted()) {
        second = status;
        secondColor = QColor("#9e9e9e");
    } else {
        second = amountText(seat.stack);
    }
    painter.setFont(sized(font(), 12.5 * s, QFont::Bold));
    painter.setPen(secondColor);
    painter.drawText(plate.adjusted(8 * s, plate.height() / 2 - 1 * s, -8 * s, -2 * s), Qt::AlignCenter, second);
}

void PokerTableView::paintTag(QPainter &painter, const QString &text, const QPointF &anchor, const QColor &color)
{
    if (text.isEmpty())
        return;
    const double s = m_layout.scale;
    const QFont font = sized(this->font(), 10.5 * s, QFont::Bold);
    const QFontMetricsF metrics(font);
    const double width = metrics.horizontalAdvance(text.toUpper()) + 14 * s;
    const QRectF box(anchor.x() - width / 2, anchor.y() - 9 * s, width, 18 * s);
    pill(painter, box, color, QColor(255, 255, 255, 70));
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(box, Qt::AlignCenter, text.toUpper());
}

void PokerTableView::paintButton(QPainter &painter, const QPointF &centre)
{
    const double s = m_layout.scale;
    const double radius = 15 * s;
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 120));
    painter.drawEllipse(centre + QPointF(0.8 * s, 2.6 * s), radius, radius * 0.96);
    QLinearGradient edge(centre + QPointF(0, -radius), centre + QPointF(0, radius));
    edge.setColorAt(0.0, QColor("#f5f5f2"));
    edge.setColorAt(1.0, QColor("#b8b8b2"));
    painter.setBrush(edge);
    painter.drawEllipse(centre + QPointF(0, 1.6 * s), radius, radius);
    QRadialGradient face(centre + QPointF(-4 * s, -5 * s), radius * 1.5);
    face.setColorAt(0.0, QColor("#ffffff"));
    face.setColorAt(1.0, QColor("#e4e4df"));
    painter.setBrush(face);
    painter.setPen(QPen(QColor("#a5a5a0"), 0.8 * s));
    painter.drawEllipse(centre, radius, radius);
    painter.setPen(QPen(QColor(26, 26, 26, 190), 0.9 * s));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(centre, radius * 0.82, radius * 0.82);
    QFont font = sized(this->font(), 6.4 * s, QFont::Black);
    const QFontMetricsF metrics(font);
    const double room = radius * 1.42;
    const double width = metrics.horizontalAdvance(QStringLiteral("DEALER"));
    if (width > room)
        font.setPixelSize(std::max(5, int(std::floor(font.pixelSize() * room / width))));
    painter.setPen(QColor("#1a1a1a"));
    painter.setFont(font);
    painter.drawText(QRectF(centre.x() - radius, centre.y() - radius, radius * 2, radius * 2), Qt::AlignCenter,
                     QStringLiteral("DEALER"));
}

void PokerTableView::paintEmptySeat(QPainter &painter, const PokerSeatGeometry &geometry)
{
    const double s = m_layout.scale;
    const double radius = m_layout.avatarRadius * 0.9;
    const bool open = m_table.valid && !m_table.seated();
    const bool hover = geometry.seat == m_hoverSeat;
    painter.setPen(Qt::NoPen);
    painter.setBrush(hover ? QColor(232, 196, 92, 60) : QColor(0, 0, 0, 90));
    painter.drawEllipse(geometry.avatar, radius, radius);
    QPen ring(hover ? QColor(232, 196, 92, 230) : QColor(255, 255, 255, open ? 90 : 45), 1.4 * s, Qt::DashLine);
    painter.setPen(ring);
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(geometry.avatar, radius, radius);
    painter.setPen(hover ? QColor("#ffe9a8") : QColor(255, 255, 255, open ? 180 : 90));
    painter.setFont(sized(font(), (open ? 11.5 : 10) * s, QFont::DemiBold));
    painter.drawText(QRectF(geometry.avatar.x() - radius, geometry.avatar.y() - radius, radius * 2, radius * 2),
                     Qt::AlignCenter, open ? QStringLiteral("Sit") : QStringLiteral("Empty"));
}

void PokerTableView::paintSeat(QPainter &painter, const PokerSeat &seat, const PokerSeatGeometry &geometry,
                               const QSet<QString> &held)
{
    if (seat.empty) {
        paintEmptySeat(painter, geometry);
        return;
    }
    const double s = m_layout.scale;
    const bool mine = seat.index == m_table.mySeat;
    const double radius = geometry.bottom ? m_layout.avatarRadius * 1.12 : m_layout.avatarRadius;
    const bool showdown = m_table.phase == QLatin1String("showdown");
    const bool folded = seat.status == QLatin1String("folded");
    const bool sittingOut = seat.away || seat.status == QLatin1String("out") || seat.status == QLatin1String("leaving");
    const double fade = folded || sittingOut ? 0.5 : 1.0;
    const bool plateAbove = !geometry.bottom && geometry.avatar.y() < m_layout.centre.y() - m_layout.rail.height() * 0.3;

    if (seat.winner && showdown) {
        QRadialGradient glow(geometry.avatar, radius * 2.4);
        glow.setColorAt(0.0, QColor(255, 220, 100, 220));
        glow.setColorAt(0.5, QColor(255, 196, 60, 110));
        glow.setColorAt(1.0, QColor(255, 196, 60, 0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(glow);
        painter.drawEllipse(geometry.avatar, radius * 2.4, radius * 2.4);
    }

    QSet<QString> best;
    if (seat.winner && showdown && m_table.lastHand.number == m_table.handNumber)
        for (const PokerWinner &winner : m_table.lastHand.winners)
            if (winner.seat == seat.index)
                for (const PokerCard &card : winner.best)
                    best.insert(card.code());
    const bool faceUp = holeCardsFaceUp(seat);
    const bool showCards = !seat.cards.isEmpty() && (seat.inHand() || (mine && folded) || (showdown && faceUp && !folded));
    if (showCards) {
        for (int index = 0; index < std::min<int>(2, int(seat.cards.size())); ++index) {
            if (held.contains(QStringLiteral("hole:%1:%2").arg(seat.index).arg(index)))
                continue;
            const PokerCard &card = seat.cards.at(index);
            const bool lifted = best.contains(card.code());
            paintCard(painter, card, holeCardCentre(seat.index, index) + QPointF(0, lifted ? -10 * s : 0),
                      holeCardSize(seat.index), holeCardAngle(seat.index, index), 1, folded ? 0.45 : 1.0);
        }
    }

    painter.save();
    painter.setOpacity(fade);
    const PokerClock clock = turnClock(serverNow(), m_table.actStartedAt, m_table.actDeadline);
    const bool turn = m_table.handRunning() && m_table.toAct == seat.index;
    if (turn && clock.running) {
        const double ringRadius = radius + 6 * s;
        painter.setPen(QPen(QColor(0, 0, 0, 150), 5 * s));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(geometry.avatar, ringRadius, ringRadius);
        QPen arc(QColor(clock.color), 4.2 * s, Qt::SolidLine, Qt::RoundCap);
        painter.setPen(arc);
        const QRectF ring(geometry.avatar.x() - ringRadius, geometry.avatar.y() - ringRadius, ringRadius * 2, ringRadius * 2);
        painter.drawArc(ring, 90 * 16, int(std::lround(clock.fraction * 360 * 16)));
    } else if (turn) {
        painter.setPen(QPen(QColor(232, 196, 92, 200), 3 * s));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(geometry.avatar, radius + 6 * s, radius + 6 * s);
    }
    paintAvatar(painter, seat, geometry.avatar, radius);
    const QPointF plateCentre = plateAbove ? geometry.avatar + QPointF(0, -(radius * 0.78 + 19 * s))
                                           : geometry.avatar + QPointF(0, radius * 0.78 + 19 * s);
    paintPlate(painter, seat, plateCentre, radius);
    painter.restore();

    if (m_table.handRunning() && !seat.lastAction.kind.isEmpty()) {
        const QPointF tagAnchor = plateAbove ? plateCentre + QPointF(0, -29 * s) : plateCentre + QPointF(0, 29 * s);
        paintTag(painter, actionTag(seat.lastAction), tagAnchor, actionColor(seat.lastAction.kind));
    } else if (seat.winner && showdown) {
        const QPointF tagAnchor = plateAbove ? plateCentre + QPointF(0, -29 * s) : plateCentre + QPointF(0, 29 * s);
        paintTag(painter, seat.won > 0 ? QStringLiteral("+%1").arg(amountText(seat.won)) : QStringLiteral("Winner"),
                 tagAnchor, QColor("#b8860b"));
    } else if (showdown && !seat.handName.isEmpty() && faceUp) {
        const QPointF tagAnchor = plateAbove ? plateCentre + QPointF(0, -29 * s) : plateCentre + QPointF(0, 29 * s);
        paintTag(painter, seat.handName, tagAnchor, QColor("#37474f"));
    }

    if (seat.bet > 0 && !held.contains(QStringLiteral("bet:%1").arg(seat.index)))
        paintChipStack(painter, geometry.bet, seat.bet, true);
}

void PokerTableView::paintMyHand(QPainter &painter, const PokerSeatGeometry &geometry)
{
    if (m_table.myHandName.isEmpty())
        return;
    const PokerSeat *seat = m_table.mine();
    if (!seat || seat->cards.isEmpty() || seat->status == QLatin1String("folded"))
        return;
    const double s = m_layout.scale;
    const QFont font = sized(this->font(), 12 * s, QFont::DemiBold);
    const QFontMetricsF metrics(font);
    const double width = metrics.horizontalAdvance(m_table.myHandName) + 22 * s;
    const double right = geometry.cards.x() - m_layout.myCard.width() * 1.15;
    const QRectF box(right - width, geometry.cards.y() + 6 * s, width, 22 * s);
    pill(painter, box, QColor(10, 30, 20, 190), QColor(232, 196, 92, 150));
    painter.setFont(font);
    painter.setPen(QColor("#ffe9a8"));
    painter.drawText(box, Qt::AlignCenter, m_table.myHandName);
}

void PokerTableView::paintBanner(QPainter &painter)
{
    if (m_table.phase != QLatin1String("showdown"))
        return;
    QString text;
    if (m_table.lastHand.number == m_table.handNumber && !m_table.lastHand.winners.isEmpty()) {
        QStringList parts;
        for (const PokerWinner &winner : m_table.lastHand.winners) {
            const PokerSeat *seat = m_table.seat(winner.seat);
            const QString name = seat ? seat->displayName() : winner.name;
            parts << (winner.handName.isEmpty()
                          ? QStringLiteral("%1 wins %2").arg(name, amountText(winner.amount))
                          : QStringLiteral("%1 wins %2 with %3").arg(name, amountText(winner.amount), winner.handName));
        }
        text = parts.join(QStringLiteral("  ·  "));
    } else {
        QStringList parts;
        for (const PokerSeat &seat : m_table.seats)
            if (!seat.empty && seat.winner)
                parts << (seat.handName.isEmpty()
                              ? QStringLiteral("%1 wins %2").arg(seat.displayName(), amountText(seat.won))
                              : QStringLiteral("%1 wins %2 with %3").arg(seat.displayName(), amountText(seat.won), seat.handName));
        text = parts.join(QStringLiteral("  ·  "));
    }
    if (text.isEmpty())
        return;
    const double s = m_layout.scale;
    const QFont font = sized(this->font(), 14.5 * s, QFont::Bold);
    const QFontMetricsF metrics(font);
    const double width = std::min(metrics.horizontalAdvance(text) + 40 * s, m_layout.inset.width() * 0.9);
    const QRectF box(m_layout.centre.x() - width / 2, m_layout.board.bottom() + 12 * s, width, 30 * s);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 90));
    painter.drawRoundedRect(box.translated(0, 3 * s), 15 * s, 15 * s);
    QLinearGradient gold(box.topLeft(), box.bottomLeft());
    gold.setColorAt(0.0, QColor("#ffe08a"));
    gold.setColorAt(0.5, QColor("#e0b23c"));
    gold.setColorAt(1.0, QColor("#b8860b"));
    painter.setPen(QPen(QColor("#fff2c2"), 1 * s));
    painter.setBrush(gold);
    painter.drawRoundedRect(box, 15 * s, 15 * s);
    painter.setFont(font);
    painter.setPen(QColor("#2a1d00"));
    painter.drawText(box.adjusted(14 * s, 0, -14 * s, 0), Qt::AlignCenter,
                     metrics.elidedText(text, Qt::ElideRight, box.width() - 28 * s));
}

QPixmap PokerTableView::cardPixmap(const PokerCard &card, const QSizeF &size)
{
    const qreal ratio = devicePixelRatioF();
    const QString key = QStringLiteral("%1|%2x%3").arg(card.code()).arg(int(size.width() * 10)).arg(int(size.height() * 10));
    const auto found = m_cards.constFind(key);
    if (found != m_cards.constEnd())
        return found.value();
    const double w = size.width();
    const double h = size.height();
    QPixmap out(std::max(4, int(std::ceil(w * ratio))), std::max(4, int(std::ceil(h * ratio))));
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const QRectF rect(0.5, 0.5, w - 1, h - 1);
    const double corner = w * 0.09;
    if (card.hidden) {
        painter.setPen(QPen(QColor("#d0d4dc"), 0.8));
        painter.setBrush(QColor("#fbfbfd"));
        painter.drawRoundedRect(rect, corner, corner);
        const QRectF inner = rect.adjusted(w * 0.075, w * 0.075, -w * 0.075, -w * 0.075);
        QLinearGradient blue(inner.topLeft(), inner.bottomRight());
        blue.setColorAt(0.0, QColor("#2848a8"));
        blue.setColorAt(0.5, QColor("#1e3a8a"));
        blue.setColorAt(1.0, QColor("#152b6b"));
        painter.setPen(Qt::NoPen);
        painter.setBrush(blue);
        painter.drawRoundedRect(inner, corner * 0.6, corner * 0.6);
        painter.save();
        QPainterPath clip;
        clip.addRoundedRect(inner, corner * 0.6, corner * 0.6);
        painter.setClipPath(clip);
        const double step = std::max(3.0, w * 0.13);
        painter.setPen(QPen(QColor(120, 150, 230, 120), std::max(0.6, w * 0.012)));
        for (double offset = -h; offset < w + h; offset += step) {
            painter.drawLine(QPointF(inner.left() + offset, inner.top()), QPointF(inner.left() + offset - inner.height(), inner.bottom()));
            painter.drawLine(QPointF(inner.left() + offset - inner.height(), inner.top()), QPointF(inner.left() + offset, inner.bottom()));
        }
        painter.restore();
        painter.setPen(QPen(QColor(200, 215, 255, 150), std::max(0.6, w * 0.016)));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(inner.adjusted(w * 0.05, w * 0.05, -w * 0.05, -w * 0.05), corner * 0.4, corner * 0.4);
        const QPointF centre = inner.center();
        QPolygonF diamond{centre + QPointF(0, -w * 0.16), centre + QPointF(w * 0.11, 0), centre + QPointF(0, w * 0.16),
                          centre + QPointF(-w * 0.11, 0)};
        painter.setPen(QPen(QColor(220, 230, 255, 200), std::max(0.6, w * 0.02)));
        painter.setBrush(QColor("#1e3a8a"));
        painter.drawPolygon(diamond);
    } else {
        QLinearGradient face(rect.topLeft(), rect.bottomLeft());
        face.setColorAt(0.0, QColor("#ffffff"));
        face.setColorAt(1.0, QColor("#f0efea"));
        painter.setPen(QPen(QColor("#bdbdbd"), 0.8));
        painter.setBrush(face);
        painter.drawRoundedRect(rect, corner, corner);
        const QColor ink = card.red() ? QColor("#c62828") : QColor("#1a1a1a");
        const QString rank = card.rankLabel();
        const QString suit = card.suitSymbol();
        const QFont rankFont = sized(font(), w * (rank.size() > 1 ? 0.27 : 0.31), QFont::Bold);
        const QFont suitFont = sized(font(), w * 0.24, QFont::Normal);
        const auto corners = [&](bool flipped) {
            painter.save();
            if (flipped) {
                painter.translate(w, h);
                painter.rotate(180);
            }
            painter.setPen(ink);
            painter.setFont(rankFont);
            const QRectF rankRect(w * 0.03, h * 0.02, w * 0.3, w * 0.34);
            painter.drawText(rankRect, Qt::AlignHCenter | Qt::AlignVCenter, rank);
            painter.setFont(suitFont);
            painter.drawText(QRectF(w * 0.03, h * 0.02 + w * 0.29, w * 0.3, w * 0.28), Qt::AlignCenter, suit);
            painter.restore();
        };
        corners(false);
        corners(true);
        const bool court = card.rank == QLatin1Char('J') || card.rank == QLatin1Char('Q') || card.rank == QLatin1Char('K');
        if (court) {
            const QRectF frame(w * 0.27, h * 0.17, w * 0.46, h * 0.66);
            QLinearGradient tint(frame.topLeft(), frame.bottomLeft());
            tint.setColorAt(0.0, alpha(ink, 0.06));
            tint.setColorAt(1.0, alpha(ink, 0.14));
            painter.setPen(QPen(alpha(ink, 0.65), std::max(0.6, w * 0.018)));
            painter.setBrush(tint);
            painter.drawRoundedRect(frame, w * 0.04, w * 0.04);
            painter.setPen(ink);
            painter.setFont(sized(font(), w * 0.36, QFont::Black));
            painter.drawText(frame.adjusted(0, 0, 0, -frame.height() * 0.3), Qt::AlignCenter, rank);
            painter.setFont(sized(font(), w * 0.26));
            painter.drawText(frame.adjusted(0, frame.height() * 0.45, 0, 0), Qt::AlignCenter, suit);
        } else {
            const double pip = card.rank == QLatin1Char('A') ? 0.62 : 0.5;
            painter.setPen(ink);
            painter.setFont(sized(font(), w * pip));
            painter.drawText(QRectF(0, h * 0.06, w, h * 0.88), Qt::AlignCenter, suit);
        }
    }
    painter.end();
    m_cards.insert(key, out);
    return out;
}

void PokerTableView::paintCard(QPainter &painter, const PokerCard &card, const QPointF &centre, const QSizeF &size,
                               double angle, double widthScale, double opacity)
{
    if (!card.valid)
        return;
    const QPixmap pixmap = cardPixmap(card, size);
    painter.save();
    painter.setOpacity(painter.opacity() * opacity);
    painter.translate(centre);
    if (angle != 0)
        painter.rotate(angle);
    if (widthScale != 1)
        painter.scale(std::max(0.02, widthScale), 1);
    const QRectF target(-size.width() / 2, -size.height() / 2, size.width(), size.height());
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 80));
    const double corner = size.width() * 0.09;
    painter.drawRoundedRect(target.translated(1.2 * m_layout.scale, 2.4 * m_layout.scale), corner, corner);
    painter.drawPixmap(target, pixmap, QRectF(pixmap.rect()));
    painter.restore();
}

QPixmap PokerTableView::chipPixmap(qint64 denomination, double radius)
{
    const QString key = QStringLiteral("%1|%2").arg(denomination).arg(int(radius * 10));
    const auto found = m_chips.constFind(key);
    if (found != m_chips.constEnd())
        return found.value();
    const qreal ratio = devicePixelRatioF();
    const double squash = 0.6;
    const double thickness = radius * 0.34;
    const double w = radius * 2 + 2;
    const double h = radius * 2 * squash + thickness + 2;
    QPixmap out(std::max(4, int(std::ceil(w * ratio))), std::max(4, int(std::ceil(h * ratio))));
    out.setDevicePixelRatio(ratio);
    out.fill(Qt::transparent);
    QPainter painter(&out);
    painter.setRenderHint(QPainter::Antialiasing);
    const QColor color(chipColor(denomination));
    const bool light = color.lightness() > 200;
    const QColor stripe = light ? QColor("#1565c0") : QColor("#ffffff");
    const QPointF centre(1 + radius, 1 + radius * squash);
    const QRectF topFace(centre.x() - radius, centre.y() - radius * squash, radius * 2, radius * 2 * squash);
    QPainterPath band;
    band.moveTo(topFace.left(), centre.y());
    band.lineTo(topFace.left(), centre.y() + thickness);
    band.arcTo(topFace.translated(0, thickness), 180, 180);
    band.lineTo(topFace.right(), centre.y());
    band.closeSubpath();
    painter.setPen(QPen(color.darker(220), 0.6));
    painter.setBrush(color.darker(light ? 125 : 145));
    painter.drawPath(band);
    painter.setPen(Qt::NoPen);
    painter.setBrush(alpha(stripe, light ? 0.85 : 0.9));
    for (const double angle : {-52.0, 0.0, 52.0}) {
        const double x = centre.x() + radius * std::sin(qDegreesToRadians(angle));
        const double half = radius * 0.13 * std::cos(qDegreesToRadians(angle));
        const double curve = radius * squash * std::cos(qDegreesToRadians(angle));
        painter.drawRect(QRectF(x - half, centre.y() + curve * 0.6, half * 2, thickness * 0.9));
    }
    painter.save();
    painter.translate(centre);
    painter.scale(1, squash);
    QRadialGradient face(QPointF(-radius * 0.3, -radius * 0.35), radius * 1.5);
    face.setColorAt(0.0, color.lighter(light ? 104 : 135));
    face.setColorAt(1.0, color.darker(110));
    painter.setPen(QPen(color.darker(200), 0.7));
    painter.setBrush(face);
    painter.drawEllipse(QPointF(0, 0), radius, radius);
    QPen dashes(alpha(stripe, 0.95), radius * 0.24, Qt::SolidLine, Qt::FlatCap);
    painter.setPen(dashes);
    painter.setBrush(Qt::NoBrush);
    const QRectF rim(-radius * 0.86, -radius * 0.86, radius * 1.72, radius * 1.72);
    for (int segment = 0; segment < 6; ++segment)
        painter.drawArc(rim, (segment * 60 + 10) * 16, 22 * 16);
    painter.setPen(QPen(alpha(light ? QColor("#90a4ae") : QColor("#ffffff"), 0.55), radius * 0.06));
    painter.setBrush(color.lighter(light ? 100 : 112));
    painter.drawEllipse(QPointF(0, 0), radius * 0.58, radius * 0.58);
    painter.restore();
    painter.end();
    m_chips.insert(key, out);
    return out;
}

void PokerTableView::paintChipStack(QPainter &painter, const QPointF &base, qint64 amount, bool label, double opacity)
{
    if (amount <= 0)
        return;
    const double s = m_layout.scale;
    const double radius = m_layout.chipRadius;
    const double thickness = radius * 0.34;
    QList<QPair<qint64, int>> columns = chipBreakdown(amount, 20);
    while (columns.size() > 3)
        columns.removeLast();
    painter.save();
    painter.setOpacity(painter.opacity() * opacity);
    const double columnWidth = radius * 2 + 2 * s;
    const double totalWidth = columns.size() * columnWidth;
    double x = base.x() - totalWidth / 2 + columnWidth / 2;
    for (const auto &column : std::as_const(columns)) {
        const QPixmap chip = chipPixmap(column.first, radius);
        const QSizeF size(chip.width() / chip.devicePixelRatio(), chip.height() / chip.devicePixelRatio());
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, 70));
        painter.drawEllipse(QPointF(x + 1.5 * s, base.y() + thickness * 0.5), radius * 1.05, radius * 0.62);
        const int count = std::clamp(column.second, 1, 8);
        for (int index = 0; index < count; ++index) {
            const QPointF topLeft(x - size.width() / 2, base.y() - size.height() + radius * 0.6 - index * thickness);
            painter.drawPixmap(QRectF(topLeft, size), chip, QRectF(chip.rect()));
        }
        x += columnWidth;
    }
    if (label) {
        const QString text = shortAmountText(amount);
        const QFont font = sized(this->font(), 11 * s, QFont::Bold);
        const QFontMetricsF metrics(font);
        const double width = metrics.horizontalAdvance(text) + 12 * s;
        const QRectF box(base.x() + totalWidth / 2 + 2 * s, base.y() - 9 * s, width, 17 * s);
        pill(painter, box, QColor(0, 0, 0, 150));
        painter.setFont(font);
        painter.setPen(QColor("#fff8e1"));
        painter.drawText(box, Qt::AlignCenter, text);
    }
    painter.restore();
}

void PokerTableView::paintAnim(QPainter &painter, const Anim &anim, qint64 now)
{
    if (now < anim.start || anim.kind == AnimKind::Sound)
        return;
    const double raw = anim.duration > 0 ? double(now - anim.start) / double(anim.duration) : 1.0;
    const double progress = ease(raw);
    switch (anim.kind) {
    case AnimKind::Card: {
        const QPointF position = lerp(anim.from, anim.to, progress);
        const double angle = anim.fromAngle + (anim.toAngle - anim.fromAngle) * progress;
        const QSizeF size = anim.size * (0.7 + 0.3 * progress);
        PokerCard shown = PokerCard::parse(QStringLiteral("??"));
        double widthScale = 1;
        if (anim.reveal) {
            if (raw >= 0.55) {
                const double flip = std::clamp((raw - 0.55) / 0.45, 0.0, 1.0);
                widthScale = std::fabs(std::cos(M_PI * flip));
                if (flip >= 0.5)
                    shown = anim.card;
            }
        }
        paintCard(painter, shown, position, size, angle, widthScale);
        break;
    }
    case AnimKind::Flip: {
        const double flip = std::clamp(raw, 0.0, 1.0);
        const PokerCard shown = flip >= 0.5 ? anim.card : PokerCard::parse(QStringLiteral("??"));
        paintCard(painter, shown, anim.from + QPointF(0, -std::sin(M_PI * flip) * 8 * m_layout.scale), anim.size,
                  anim.fromAngle, std::fabs(std::cos(M_PI * flip)));
        break;
    }
    case AnimKind::Fold: {
        const QPointF position = lerp(anim.from, anim.to, progress);
        const double angle = anim.fromAngle + (anim.toAngle - anim.fromAngle) * progress;
        paintCard(painter, PokerCard::parse(QStringLiteral("??")), position, anim.size * (1.0 - 0.35 * progress), angle, 1,
                  1.0 - progress * 0.9);
        break;
    }
    case AnimKind::Chips: {
        const QPointF position = lerp(anim.from, anim.to, progress);
        const double lift = std::sin(M_PI * std::clamp(raw, 0.0, 1.0)) * 10 * m_layout.scale;
        paintChipStack(painter, position + QPointF(0, -lift), anim.amount, false);
        break;
    }
    case AnimKind::Button:
        paintButton(painter, lerp(anim.from, anim.to, progress));
        break;
    case AnimKind::Sound:
        break;
    }
}

void PokerTableView::paintEvent(QPaintEvent *)
{
    ensureBackground();
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.setRenderHint(QPainter::TextAntialiasing);
    painter.drawPixmap(0, 0, m_background);
    const qint64 now = clockNow();
    const QSet<QString> held = heldKeys(now);
    paintDealerLine(painter);
    if (!m_table.valid)
        return;
    paintPot(painter, held);
    paintBoard(painter, held);
    for (const PokerSeatGeometry &geometry : m_layout.seats) {
        const PokerSeat *seat = m_table.seat(geometry.seat);
        if (seat)
            paintSeat(painter, *seat, geometry, held);
    }
    for (const PokerSeatGeometry &geometry : m_layout.seats)
        if (geometry.bottom && geometry.seat == m_table.mySeat)
            paintMyHand(painter, geometry);
    if (m_table.dealer >= 0 && !held.contains(QStringLiteral("button")) && m_layout.seat(m_table.dealer)) {
        const PokerSeat *dealer = m_table.seat(m_table.dealer);
        if (dealer && !dealer->empty)
            paintButton(painter, buttonPoint(m_table.dealer));
    }
    paintBanner(painter);
    for (const Anim &anim : std::as_const(m_anims))
        paintAnim(painter, anim, now);
}

}
