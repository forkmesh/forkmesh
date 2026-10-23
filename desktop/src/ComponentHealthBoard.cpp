#include "ComponentHealthBoard.h"

#include <QFontMetrics>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QVariantMap>

namespace {

// Tiny on purpose: at 150x74 a 1600px-wide board shows ~10 per row, so the
// whole 310-file codebase is about 28 rows — one scroll, not a hunt.
constexpr int kCardWidth = 150;
constexpr int kCardHeight = 74;
constexpr int kGap = 8;
constexpr int kRadius = 6;
constexpr int kStatusEdge = 3;
constexpr int kInset = 7;

QFont resized(const QFont &base, double delta)
{
    QFont font = base;
    const double size = font.pointSizeF() > 0 ? font.pointSizeF() : 10.0;
    font.setPointSizeF(qMax(6.0, size + delta));
    return font;
}

QString compactCount(int value)
{
    if (value >= 1000000)
        return QStringLiteral("%1M").arg(value / 1000000.0, 0, 'f', 1);
    if (value >= 1000)
        return QStringLiteral("%1k").arg(value / 1000.0, 0, 'f', 1);
    return QString::number(value);
}

struct NextLook {
    bool on = false;
    QColor surface;
    QColor hover;
    QColor border;
    QColor fg;
    QColor muted;
    QColor subtle;
    QColor success;
    QColor warning;
    QColor danger;
};

NextLook nextLook(const QWidget *widget)
{
    NextLook look;
    const QVariantMap map = widget->property("nextColors").toMap();
    if (map.isEmpty())
        return look;
    const auto pick = [&map](const char *key) {
        return map.value(QLatin1String(key)).value<QColor>();
    };
    look.on = true;
    look.surface = pick("surface");
    look.hover = pick("surfaceRaised");
    look.border = pick("border");
    look.fg = pick("fg");
    look.muted = pick("fgMuted");
    look.subtle = pick("fgSubtle");
    look.success = pick("success");
    look.warning = pick("warning");
    look.danger = pick("danger");
    return look;
}

constexpr int kNextCardWidth = 176;
constexpr int kNextCardHeight = 92;
constexpr int kNextGap = 10;
constexpr int kNextPadding = 24;
constexpr int kNextRadius = 12;
constexpr int kNextInset = 12;

QFont pixelFont(const QFont &base, int px, QFont::Weight weight = QFont::Normal)
{
    QFont font = base;
    font.setPixelSize(px);
    font.setWeight(weight);
    return font;
}

QString compactMs(int ms)
{
    if (ms >= 1000)
        return QStringLiteral("%1s").arg(ms / 1000.0, 0, 'f', 1);
    return QStringLiteral("%1ms").arg(ms);
}

void paintNextCard(QPainter &painter, const QFont &base, const HealthCard &card,
                   const QRect &rect, bool hovered, const QColor &status,
                   const NextLook &look)
{
    const QRectF outline = QRectF(rect).adjusted(0.5, 0.5, -0.5, -0.5);
    QColor edge = look.border;
    if (card.health < 80) {
        edge = status;
        edge.setAlpha(hovered ? 150 : 90);
    }
    painter.setPen(QPen(edge, 1));
    painter.setBrush(hovered ? look.hover : look.surface);
    painter.drawRoundedRect(outline, kNextRadius, kNextRadius);

    const QRect inner = rect.adjusted(kNextInset, kNextInset, -kNextInset, -kNextInset);
    const QFont titleFont = pixelFont(base, 14, QFont::Medium);
    const QFontMetrics titleMetrics(titleFont);
    painter.setFont(titleFont);
    painter.setPen(look.fg);
    const QRect titleRect(inner.left(), inner.top(), inner.width(), titleMetrics.height());
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                     titleMetrics.elidedText(card.title, Qt::ElideLeft, titleRect.width()));

    const QFont smallFont = pixelFont(base, 12);
    const QFontMetrics smallMetrics(smallFont);
    const int line = smallMetrics.height();
    painter.setFont(smallFont);
    painter.setPen(look.muted);
    const QRect subtitleRect(inner.left(), titleRect.bottom() + 2, inner.width(), line);
    painter.drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                     smallMetrics.elidedText(card.subtitle, Qt::ElideRight,
                                             subtitleRect.width()));

    const QFont scoreFont = pixelFont(base, 18, QFont::DemiBold);
    const QFontMetrics scoreMetrics(scoreFont);
    const QString score = QString::number(card.health);
    const int scoreWidth = scoreMetrics.horizontalAdvance(score);
    painter.setFont(scoreFont);
    painter.setPen(status);
    painter.drawText(QRect(inner.right() - scoreWidth - 1, inner.bottom() - scoreMetrics.height() + 1,
                           scoreWidth + 2, scoreMetrics.height()),
                     Qt::AlignRight | Qt::AlignBottom, score);

    QStringList flags;
    if (card.stallMs > 0)
        flags << compactMs(card.stallMs) + QStringLiteral(" stall");
    if (card.findings > 0)
        flags << QStringLiteral("%1 flag%2")
                     .arg(card.findings)
                     .arg(card.findings == 1 ? QString() : QStringLiteral("s"));
    const int factsWidth = inner.width() - scoreWidth - 8;
    painter.setFont(smallFont);
    painter.setPen(look.subtle);
    const QRect flagRect(inner.left(), inner.bottom() - line + 1, factsWidth, line);
    const QRect locRect(inner.left(), flagRect.top() - line, factsWidth, line);
    painter.drawText(locRect, Qt::AlignLeft | Qt::AlignVCenter,
                     smallMetrics.elidedText(
                         QStringLiteral("%1 lines").arg(compactCount(card.loc)),
                         Qt::ElideRight, locRect.width()));
    painter.drawText(flagRect, Qt::AlignLeft | Qt::AlignVCenter,
                     flags.isEmpty()
                         ? QStringLiteral("No flags")
                         : smallMetrics.elidedText(flags.join(QString::fromUtf8(" \xC2\xB7 ")),
                                                   Qt::ElideRight, flagRect.width()));
}

} // namespace

ComponentHealthBoard::ComponentHealthBoard(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setMinimumWidth(kCardWidth + 2 * kGap);
}

void ComponentHealthBoard::setCards(const QVector<HealthCard> &cards)
{
    m_cards = cards;
    m_hover = -1;
    relayout(width());
    update();
}

void ComponentHealthBoard::setFilter(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (trimmed == m_filter)
        return;
    m_filter = trimmed;
    m_hover = -1;
    relayout(width());
    update();
}

QColor ComponentHealthBoard::healthColor(int health) const
{
    const NextLook look = nextLook(this);
    if (look.on)
        return health >= 80 ? look.success : health >= 55 ? look.warning : look.danger;
    // Three bands, and the number is always printed beside the colour so the
    // colour never carries the meaning on its own.
    if (health >= 80)
        return QColor(0x2E, 0x9E, 0x5B);
    if (health >= 55)
        return QColor(0xC8, 0x8A, 0x1E);
    return QColor(0xC0, 0x39, 0x3D);
}

void ComponentHealthBoard::relayout(int width)
{
    m_visible.clear();
    const bool next = nextLook(this).on;
    const int gap = next ? kNextGap : kGap;
    const int padding = next ? kNextPadding : kGap;
    const int minWidth = next ? kNextCardWidth : kCardWidth;
    const int cardHeight = next ? kNextCardHeight : kCardHeight;
    const int usable = qMax(minWidth, width - 2 * padding);
    const int columns = qMax(1, (usable + gap) / (minWidth + gap));
    const int cardWidth = next ? (usable - (columns - 1) * gap) / columns : kCardWidth;
    int shown = 0;
    for (int i = 0; i < m_cards.size(); ++i) {
        const HealthCard &card = m_cards.at(i);
        if (!m_filter.isEmpty() &&
            !card.title.contains(m_filter, Qt::CaseInsensitive) &&
            !card.subtitle.contains(m_filter, Qt::CaseInsensitive) &&
            !card.id.contains(m_filter, Qt::CaseInsensitive))
            continue;
        const int row = shown / columns;
        const int column = shown % columns;
        m_visible.append({i, QRect(padding + column * (cardWidth + gap),
                                   padding + row * (cardHeight + gap),
                                   cardWidth, cardHeight)});
        ++shown;
    }
    const int rows = (shown + columns - 1) / columns;
    m_contentHeight = next ? (rows > 0 ? 2 * padding + rows * cardHeight + (rows - 1) * gap
                                       : 2 * padding)
                           : (rows > 0 ? kGap + rows * (kCardHeight + kGap) : kGap);
    // The scroll area owns our width (widgetResizable) and scrolls on our
    // minimum height. Only set it when it changes, so a resize cannot bounce
    // between two layouts.
    if (minimumHeight() != m_contentHeight)
        setMinimumHeight(m_contentHeight);
}

void ComponentHealthBoard::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout(width());
}

void ComponentHealthBoard::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    if (m_visible.isEmpty()) {
        const NextLook look = nextLook(this);
        if (look.on) {
            painter.setFont(pixelFont(font(), 14, QFont::Medium));
            painter.setPen(look.fg);
        } else {
            painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        }
        painter.drawText(rect(), Qt::AlignCenter,
                         m_cards.isEmpty()
                             ? QStringLiteral("No scan yet")
                             : QStringLiteral("Nothing matches this filter"));
        return;
    }
    const QRect clip = event->rect();
    for (const Placed &placed : std::as_const(m_visible)) {
        if (!placed.rect.intersects(clip))
            continue; // only paint what is on screen
        paintCard(painter, placed, placed.index == m_hover);
    }
}

void ComponentHealthBoard::paintCard(QPainter &painter, const Placed &placed,
                                     bool hovered) const
{
    const HealthCard &card = m_cards.at(placed.index);
    const NextLook look = nextLook(this);
    if (look.on) {
        paintNextCard(painter, font(), card, placed.rect, hovered, healthColor(card.health),
                      look);
        return;
    }
    const QPalette colors = palette();
    const QColor status = healthColor(card.health);
    const QRectF outline = QRectF(placed.rect).adjusted(0.5, 0.5, -0.5, -0.5);

    QColor border(status.red(), status.green(), status.blue(),
                  hovered ? 235 : 150);
    painter.setPen(QPen(border, 1));
    painter.setBrush(colors.color(QPalette::Base));
    painter.drawRoundedRect(outline, kRadius, kRadius);

    // Faint wash plus a solid edge down the left, matching the Stats tiles.
    QPainterPath shape;
    shape.addRoundedRect(outline, kRadius, kRadius);
    painter.save();
    painter.setClipPath(shape);
    painter.setPen(Qt::NoPen);
    QColor wash = status;
    wash.setAlpha(hovered ? 34 : 18);
    painter.setBrush(wash);
    painter.drawRect(placed.rect);
    painter.setBrush(status);
    painter.drawRect(QRect(placed.rect.left(), placed.rect.top(), kStatusEdge,
                           placed.rect.height()));
    painter.restore();

    const QRect inner =
        placed.rect.adjusted(kInset + kStatusEdge, kInset, -kInset, -kInset);
    const QColor muted = colors.color(QPalette::Disabled, QPalette::Text);

    // Title: the file or function name, elided from the left so the
    // distinguishing tail of a long qualified name survives.
    QFont titleFont = resized(font(), -0.5);
    titleFont.setWeight(QFont::DemiBold);
    painter.setFont(titleFont);
    painter.setPen(colors.color(QPalette::Text));
    const QFontMetrics titleMetrics(titleFont);
    const QRect titleRect(inner.left(), inner.top(), inner.width(), 15);
    painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                     titleMetrics.elidedText(card.title, Qt::ElideLeft,
                                             titleRect.width()));

    QFont smallFont = resized(font(), -2.0);
    painter.setFont(smallFont);
    const QFontMetrics smallMetrics(smallFont);
    painter.setPen(muted);
    const QRect subtitleRect(inner.left(), inner.top() + 15, inner.width(), 13);
    painter.drawText(subtitleRect, Qt::AlignLeft | Qt::AlignVCenter,
                     smallMetrics.elidedText(card.subtitle, Qt::ElideRight,
                                             subtitleRect.width()));

    // Bottom row: size, then the two numbers that make a card worth opening.
    const QRect factsRect(inner.left(), inner.bottom() - 26, inner.width(), 13);
    painter.setPen(muted);
    painter.drawText(factsRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("%1 lines").arg(compactCount(card.loc)));

    QStringList flags;
    if (card.stallMs > 0)
        flags << compactMs(card.stallMs);
    if (card.findings > 0)
        flags << QStringLiteral("%1 flag%2")
                     .arg(card.findings)
                     .arg(card.findings == 1 ? QString() : QStringLiteral("s"));
    const QRect flagRect(inner.left(), inner.bottom() - 13, inner.width() - 26,
                         13);
    painter.setPen(flags.isEmpty() ? muted : status);
    painter.drawText(flagRect, Qt::AlignLeft | Qt::AlignVCenter,
                     flags.isEmpty()
                         ? QStringLiteral("clear")
                         : smallMetrics.elidedText(flags.join(QStringLiteral(" · ")),
                                                   Qt::ElideRight,
                                                   flagRect.width()));

    // The score itself, bottom right, so colour is never the only signal.
    QFont scoreFont = resized(font(), 1.0);
    scoreFont.setWeight(QFont::DemiBold);
    painter.setFont(scoreFont);
    painter.setPen(status);
    painter.drawText(QRect(inner.right() - 26, inner.bottom() - 15, 26, 15),
                     Qt::AlignRight | Qt::AlignVCenter,
                     QString::number(card.health));
}

int ComponentHealthBoard::cardAt(const QPoint &pos) const
{
    for (const Placed &placed : m_visible) {
        if (placed.rect.contains(pos))
            return placed.index;
    }
    return -1;
}

void ComponentHealthBoard::mouseMoveEvent(QMouseEvent *event)
{
    const int hit = cardAt(event->pos());
    if (hit == m_hover)
        return;
    m_hover = hit;
    setCursor(hit >= 0 && m_cards.at(hit).drillable ? Qt::PointingHandCursor
                                                    : Qt::ArrowCursor);
    update();
}

void ComponentHealthBoard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const int hit = cardAt(event->pos());
    if (hit < 0 || !m_onActivate)
        return;
    m_onActivate(m_cards.at(hit).id);
}

void ComponentHealthBoard::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_hover < 0)
        return;
    m_hover = -1;
    unsetCursor();
    update();
}

bool ComponentHealthBoard::event(QEvent *event)
{
    if (event->type() == QEvent::DynamicPropertyChange &&
        static_cast<QDynamicPropertyChangeEvent *>(event)->propertyName() == "nextColors") {
        relayout(width());
        update();
    }
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int hit = cardAt(help->pos());
        if (hit >= 0 && !m_cards.at(hit).tooltip.isEmpty())
            QToolTip::showText(help->globalPos(), m_cards.at(hit).tooltip, this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}
