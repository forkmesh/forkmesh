#include "PromptFlightOverlay.h"

#include "Theme.h"

#include <QEasingCurve>
#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QVariantAnimation>

#include <cmath>

namespace {

constexpr qreal kTravelEnd = 0.78;
constexpr qreal kLiftEnd = 0.2;
constexpr qreal kLandStart = 0.85;
constexpr int kTrail = 14;
constexpr int kBurstDots = 10;

QPointF centerIn(const QWidget *host, const QWidget *widget)
{
    return QPointF(host->mapFromGlobal(widget->mapToGlobal(widget->rect().center())));
}

bool shownIn(const QWidget *host, const QWidget *widget)
{
    return widget && widget->isVisible() && widget->window() == host->window() &&
           widget->width() > 0 && widget->height() > 0;
}

QPointF lerp(const QPointF &a, const QPointF &b, qreal t)
{
    return a + (b - a) * t;
}

QColor withAlpha(QColor color, qreal alpha)
{
    color.setAlphaF(qBound(0.0, alpha, 1.0));
    return color;
}

void drawStar(QPainter &p, const QPointF &center, qreal radius, qreal angle,
              const QColor &color)
{
    if (radius <= 0.2)
        return;
    const qreal waist = radius * 0.28;
    QPainterPath star;
    for (int i = 0; i < 8; ++i) {
        const qreal r = (i % 2 == 0) ? radius : waist;
        const qreal theta = angle + i * M_PI / 4.0;
        const QPointF point(center.x() + std::cos(theta) * r,
                            center.y() + std::sin(theta) * r);
        if (i == 0)
            star.moveTo(point);
        else
            star.lineTo(point);
    }
    star.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(color);
    p.drawPath(star);
}

void drawGlow(QPainter &p, const QPointF &center, qreal radius, const QColor &color,
              qreal alpha)
{
    if (radius <= 0.5 || alpha <= 0)
        return;
    QRadialGradient glow(center, radius);
    glow.setColorAt(0.0, withAlpha(color, alpha));
    glow.setColorAt(1.0, withAlpha(color, 0.0));
    p.setPen(Qt::NoPen);
    p.setBrush(glow);
    p.drawEllipse(center, radius, radius);
}

}

PromptFlightOverlay *PromptFlightOverlay::launchSparkle(QWidget *host,
                                                        const QWidget *from,
                                                        const QWidget *to)
{
    if (!host || !from)
        return nullptr;
    const QPointF start = centerIn(host, from);
    const QPointF end = shownIn(host, to) ? centerIn(host, to)
                                          : start + QPointF(0, -160);
    return new PromptFlightOverlay(host, Kind::Sparkle, start, end, QString(),
                                   kSparkleMs);
}

PromptFlightOverlay *PromptFlightOverlay::launchPrompt(QWidget *host,
                                                       const QWidget *from,
                                                       const QWidget *to,
                                                       const QString &text)
{
    const QString line = text.simplified();
    if (!host || !from || line.isEmpty())
        return nullptr;
    const QPointF start = centerIn(host, from);
    QPointF end = start + QPointF(0, -160);
    if (shownIn(host, to)) {
        const QPointF bottom(host->mapFromGlobal(
            to->mapToGlobal(QPoint(to->width() / 2, to->height()))));
        end = bottom - QPointF(0, 40);
    }
    return new PromptFlightOverlay(host, Kind::Prompt, start, end, line, kPromptMs);
}

PromptFlightOverlay::PromptFlightOverlay(QWidget *host, Kind kind,
                                         const QPointF &start, const QPointF &end,
                                         const QString &text, int durationMs)
    : QWidget(host), m_kind(kind), m_start(start), m_end(end)
{
    setObjectName(QStringLiteral("promptFlightOverlay"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(host->rect());

    const qreal dx = m_end.x() - m_start.x();
    if (m_kind == Kind::Sparkle)
        m_control = QPointF(m_start.x() + dx * 0.35,
                            qMax(12.0, qMin(m_start.y(), m_end.y()) -
                                           qMax(60.0, std::abs(dx) * 0.18)));
    else
        m_control = QPointF(m_start.x() + dx * 0.5,
                            qMax(12.0, qMin(m_start.y(), m_end.y()) - 90.0));

    if (m_kind == Kind::Prompt) {
        const QFontMetricsF metrics(font());
        const qreal maxText = qMax(120.0, qMin(420.0, host->width() * 0.6) - 28);
        m_text = metrics.elidedText(text, Qt::ElideRight, maxText);
        const qreal width = metrics.horizontalAdvance(m_text) + 28;
        const qreal height = metrics.height() + 16;
        m_bubble = QRectF(-width / 2, -height / 2, width, height);
    }

    m_animation = new QVariantAnimation(this);
    m_animation->setStartValue(0.0);
    m_animation->setEndValue(1.0);
    m_animation->setDuration(durationMs);
    connect(m_animation, &QVariantAnimation::valueChanged, this,
            [this](const QVariant &value) {
                m_progress = value.toReal();
                if (parentWidget() && geometry() != parentWidget()->rect())
                    setGeometry(parentWidget()->rect());
                update();
            });
    connect(m_animation, &QVariantAnimation::finished, this,
            &QObject::deleteLater);
    raise();
    show();
    m_animation->start();
}

QPointF PromptFlightOverlay::sparklePoint(qreal s) const
{
    const qreal t = QEasingCurve(QEasingCurve::InOutCubic).valueForProgress(
        qBound(0.0, s, 1.0));
    const qreal u = 1.0 - t;
    return m_start * (u * u) + m_control * (2 * u * t) + m_end * (t * t);
}

QPointF PromptFlightOverlay::promptCenter(qreal t) const
{
    const QPointF lifted = m_start + QPointF(0, -14);
    if (t < kLiftEnd)
        return lerp(m_start, lifted,
                    QEasingCurve(QEasingCurve::OutCubic).valueForProgress(t / kLiftEnd));
    if (t < kLandStart) {
        const qreal s = (t - kLiftEnd) / (kLandStart - kLiftEnd);
        const qreal t2 =
            QEasingCurve(QEasingCurve::InOutSine).valueForProgress(s);
        const qreal u = 1.0 - t2;
        return lifted * (u * u) + m_control * (2 * u * t2) + m_end * (t2 * t2) +
               QPointF(std::sin(s * 2.0 * M_PI) * 6.0, 0);
    }
    const qreal a = (t - kLandStart) / (1.0 - kLandStart);
    return m_end + QPointF(0, 6.0 * a);
}

void PromptFlightOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    if (m_kind == Kind::Sparkle)
        paintSparkle(p);
    else
        paintPrompt(p);
}

void PromptFlightOverlay::paintSparkle(QPainter &p) const
{
    const QColor gold = Theme::remapColor(QColor(QStringLiteral("#e3b341")));
    const QColor white(255, 255, 255);
    const qreal t = m_progress;
    if (t < kTravelEnd) {
        const qreal s = t / kTravelEnd;
        const QPointF head = sparklePoint(s);
        for (int k = kTrail; k >= 1; --k) {
            const qreal back = s - k * 0.035;
            if (back <= 0)
                continue;
            const qreal fade = 1.0 - qreal(k) / (kTrail + 1);
            const QPointF at = sparklePoint(back) +
                               QPointF(std::sin(k * 2.3 + s * 18.0),
                                       std::cos(k * 1.7 + s * 15.0)) *
                                   (k * 0.6);
            const QColor tint = (k % 3 == 0) ? white : gold;
            if (k % 4 == 1)
                drawStar(p, at, 2.0 + 3.0 * fade, s * 9.0 + k,
                         withAlpha(tint, 0.85 * fade));
            else {
                p.setPen(Qt::NoPen);
                p.setBrush(withAlpha(tint, 0.75 * fade));
                p.drawEllipse(at, 0.8 + 2.2 * fade, 0.8 + 2.2 * fade);
            }
        }
        const qreal grow = s < 0.15 ? 0.4 + 0.6 * (s / 0.15) : 1.0;
        const qreal radius = 9.0 * grow;
        drawGlow(p, head, radius * 3.0, gold, 0.45);
        drawStar(p, head, radius, s * 2.0 * M_PI, gold);
        drawStar(p, head, radius * 0.45, s * 2.0 * M_PI, white);
        return;
    }
    const qreal a = (t - kTravelEnd) / (1.0 - kTravelEnd);
    const qreal fade = 1.0 - a;
    drawGlow(p, m_end, 6.0 + 22.0 * a, gold, 0.5 * fade);
    drawStar(p, m_end, 9.0 * (1.0 - a * 0.7), 2.0 * M_PI + a * M_PI,
             withAlpha(gold, fade));
    const qreal ring = 4.0 + 22.0 * QEasingCurve(QEasingCurve::OutCubic)
                                        .valueForProgress(a);
    p.setPen(Qt::NoPen);
    for (int i = 0; i < kBurstDots; ++i) {
        const qreal theta = i * 2.0 * M_PI / kBurstDots + 0.3;
        const QPointF at(m_end.x() + std::cos(theta) * ring,
                         m_end.y() + std::sin(theta) * ring);
        p.setBrush(withAlpha(i % 2 ? white : gold, fade));
        p.drawEllipse(at, 1.8 * fade + 0.4, 1.8 * fade + 0.4);
    }
}

void PromptFlightOverlay::paintPrompt(QPainter &p) const
{
    const qreal t = m_progress;
    qreal opacity = 1.0;
    qreal scale = 1.0;
    if (t < kLiftEnd) {
        const qreal a = t / kLiftEnd;
        opacity = a;
        scale = 0.92 + 0.08 * a;
    } else if (t >= kLandStart) {
        const qreal a = (t - kLandStart) / (1.0 - kLandStart);
        opacity = 1.0 - a;
        scale = 1.0 - 0.04 * a;
    }
    const QPalette pal = palette();
    const QColor fill = pal.color(QPalette::Base);
    const QColor edge = pal.color(QPalette::Highlight);
    const QColor ink = pal.color(QPalette::Text);

    p.save();
    p.setOpacity(qBound(0.0, opacity, 1.0));
    p.translate(promptCenter(t));
    p.scale(scale, scale);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 60));
    p.drawRoundedRect(m_bubble.translated(0, 3), 10, 10);
    p.setBrush(fill);
    p.setPen(QPen(edge, 1.5));
    p.drawRoundedRect(m_bubble, 10, 10);
    p.setPen(ink);
    p.drawText(m_bubble, Qt::AlignCenter, m_text);
    p.restore();
}
