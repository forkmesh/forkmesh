#include "PromptDeckBar.h"

#include <QAccessible>
#include <QApplication>
#include <QClipboard>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QEasingCurve>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <cmath>

namespace {

constexpr int kPadX = 10;
constexpr int kPadY = 6;
constexpr int kEdge = 3;
constexpr int kArrow = 22;
constexpr int kMaxLines = 8;
constexpr int kSwipe = 48;

}

PromptDeckBar::PromptDeckBar(QWidget *parent) : QWidget(parent)
{
    setFocusPolicy(Qt::ClickFocus);
    setMouseTracking(true);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(tr("Your last prompt"));
    QSizePolicy policy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    m_anim = new QVariantAnimation(this);
    m_anim->setDuration(220);
    m_anim->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_anim, &QVariantAnimation::valueChanged, this, [this](const QVariant &v) {
        m_slide = v.toReal();
        update();
    });
    setColors(QColor(0x16, 0x1b, 0x22), QColor(0x1f, 0x6f, 0xeb, 30), QColor(0x30, 0x36, 0x3d),
              QColor(0xe6, 0xed, 0xf3), QColor(0x8b, 0x94, 0x9e), QColor(0x2f, 0x81, 0xf7));
    hide();
}

QString PromptDeckBar::cleanPrompt(const QString &text)
{
    static const QRegularExpression imageLine(
        QStringLiteral("^\\s*Attached image:.*$"), QRegularExpression::MultilineOption);
    QString cleaned = text;
    cleaned.remove(imageLine);
    return cleaned.trimmed();
}

void PromptDeckBar::addPrompt(const QString &text)
{
    const QString cleaned = cleanPrompt(text);
    if (cleaned.isEmpty())
        return;
    const int before = heightForWidth(width());
    const bool wasEmpty = m_prompts.isEmpty();
    m_prompts.append(cleaned);
    m_index = m_prompts.size() - 1;
    m_expanded = false;
    m_drag = 0;
    setVisible(true);
    if (!wasEmpty)
        slideIn(width() / 4);
    update();
    refreshAccessible();
    if (wasEmpty || heightForWidth(width()) != before)
        emit heightChanged();
}

void PromptDeckBar::clearPrompts()
{
    if (m_prompts.isEmpty())
        return;
    m_prompts.clear();
    m_index = 0;
    m_expanded = false;
    m_drag = 0;
    m_slide = 0;
    m_anim->stop();
    hide();
    emit heightChanged();
}

void PromptDeckBar::setColors(const QColor &surface, const QColor &tint, const QColor &border,
                              const QColor &text, const QColor &muted, const QColor &accent)
{
    m_surface = surface;
    m_tint = tint;
    m_border = border;
    m_text = text;
    m_muted = muted;
    m_accent = accent;
    update();
}

QString PromptDeckBar::shownPrompt() const
{
    return m_prompts.isEmpty() ? QString() : m_prompts.at(m_index);
}

QString PromptDeckBar::positionText() const
{
    if (m_prompts.size() < 2)
        return QString();
    return QStringLiteral("%1/%2").arg(m_index + 1).arg(m_prompts.size());
}

int PromptDeckBar::behind() const
{
    return qMin(2, m_index);
}

int PromptDeckBar::cardHeight(int width) const
{
    const QFontMetrics fm(font());
    const int line = fm.height();
    if (!m_expanded || m_prompts.isEmpty())
        return line + 2 * kPadY;
    const QRect probe(0, 0, qMax(40, width), 10000);
    const int textWidth = textRect(probe).width();
    const QRect bound = fm.boundingRect(QRect(0, 0, textWidth, 10000),
                                        Qt::TextWordWrap, shownPrompt());
    return qMin(bound.height(), line * kMaxLines) + 2 * kPadY;
}

int PromptDeckBar::heightForWidth(int width) const
{
    if (m_prompts.isEmpty())
        return 0;
    return cardHeight(width) + behind() * kEdge;
}

QSize PromptDeckBar::sizeHint() const
{
    return QSize(320, heightForWidth(qMax(width(), 320)));
}

QRect PromptDeckBar::cardRect() const
{
    return QRect(0, 0, width(), cardHeight(width()));
}

QRect PromptDeckBar::olderRect() const
{
    const QRect card = cardRect();
    return QRect(card.right() - 2 * kArrow - 4, card.top() + (qMin(card.height(), 30) - kArrow) / 2,
                 kArrow, kArrow);
}

QRect PromptDeckBar::newerRect() const
{
    const QRect older = olderRect();
    return older.translated(kArrow, 0);
}

QRect PromptDeckBar::textRect(const QRect &card) const
{
    const QFontMetrics fm(font());
    int right = card.right() - kPadX;
    if (m_prompts.size() > 1) {
        right -= 2 * kArrow + 6;
        right -= fm.horizontalAdvance(QStringLiteral("%1/%1").arg(m_prompts.size())) + 8;
    }
    const int left = card.left() + kPadX + 18;
    return QRect(left, card.top() + kPadY, qMax(10, right - left), card.height() - 2 * kPadY);
}

PromptDeckBar::Hit PromptDeckBar::hitAt(const QPoint &pos) const
{
    if (m_prompts.size() > 1) {
        if (olderRect().contains(pos))
            return Hit::Older;
        if (newerRect().contains(pos))
            return Hit::Newer;
    }
    return cardRect().contains(pos) ? Hit::Card : Hit::None;
}

void PromptDeckBar::paintEvent(QPaintEvent *)
{
    if (m_prompts.isEmpty())
        return;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRect base = cardRect();
    const qreal radius = 8;

    for (int depth = behind(); depth >= 1; --depth) {
        const qreal inset = base.width() * 0.025 * depth;
        QRectF edge = QRectF(base).adjusted(inset, 0, -inset, 0).translated(0, depth * kEdge);
        QPainterPath path;
        path.addRoundedRect(edge.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
        p.fillPath(path, m_surface);
        QColor tint = m_tint;
        tint.setAlphaF(tint.alphaF() * 0.6);
        p.fillPath(path, tint);
        p.setPen(QPen(m_border, 1));
        p.drawPath(path);
    }

    const qreal offset = m_drag + m_slide;
    p.save();
    p.setOpacity(qMax(0.35, 1.0 - std::abs(m_slide) / qMax(1.0, width() * 0.5)));
    p.translate(offset, 0);
    QPainterPath card;
    card.addRoundedRect(QRectF(base).adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);
    p.fillPath(card, m_surface);
    p.fillPath(card, m_tint);
    p.setPen(QPen(m_border, 1));
    p.drawPath(card);

    const QFontMetrics fm(font());
    const QRect text = textRect(base);
    QFont glyphFont = font();
    glyphFont.setBold(true);
    p.setFont(glyphFont);
    p.setPen(m_accent);
    p.drawText(QRect(base.left() + kPadX, text.top(), 16, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
               m_index == m_prompts.size() - 1 ? QStringLiteral("“") : QStringLiteral("↺"));
    p.setFont(font());
    p.setPen(m_text);
    if (m_expanded) {
        p.setClipRect(text);
        p.drawText(text, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignTop, shownPrompt());
        p.setClipping(false);
    } else {
        QString headline = shownPrompt();
        headline.replace(QRegularExpression(QStringLiteral("\\s+")), QStringLiteral(" "));
        p.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(headline, Qt::ElideRight, text.width()));
    }
    p.restore();

    if (m_prompts.size() > 1) {
        const QRect older = olderRect();
        const QRect newer = newerRect();
        p.setPen(m_muted);
        const QString pos = positionText();
        const int pw = fm.horizontalAdvance(pos);
        p.drawText(QRect(older.left() - pw - 6, older.top(), pw, older.height()),
                   Qt::AlignRight | Qt::AlignVCenter, pos);
        auto arrow = [&](const QRect &r, bool left, bool enabled, bool hover) {
            if (hover && enabled) {
                QColor hl = m_accent;
                hl.setAlphaF(0.18);
                p.setPen(Qt::NoPen);
                p.setBrush(hl);
                p.drawRoundedRect(r, 5, 5);
                p.setBrush(Qt::NoBrush);
            }
            QColor c = enabled ? (hover ? m_accent : m_muted) : m_muted;
            if (!enabled)
                c.setAlphaF(0.35);
            p.setPen(QPen(c, 1.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            const QPointF mid = QRectF(r).center();
            const qreal dx = left ? 2.5 : -2.5;
            QPolygonF chevron;
            chevron << QPointF(mid.x() + dx, mid.y() - 4.5) << QPointF(mid.x() - dx, mid.y())
                    << QPointF(mid.x() + dx, mid.y() + 4.5);
            p.drawPolyline(chevron);
        };
        arrow(older, true, m_index > 0, m_hover == Hit::Older);
        arrow(newer, false, m_index < m_prompts.size() - 1, m_hover == Hit::Newer);
    }
}

void PromptDeckBar::step(int delta)
{
    const int next = qBound(0, m_index + delta, int(m_prompts.size()) - 1);
    if (m_prompts.isEmpty() || next == m_index)
        return;
    const int before = heightForWidth(width());
    m_index = next;
    m_expanded = false;
    slideIn(delta < 0 ? -width() / 4 : width() / 4);
    update();
    refreshAccessible();
    if (heightForWidth(width()) != before)
        emit heightChanged();
}

void PromptDeckBar::slideIn(int from)
{
    m_anim->stop();
    m_anim->setStartValue(qreal(from) + m_drag);
    m_anim->setEndValue(qreal(0));
    m_drag = 0;
    m_anim->start();
}

void PromptDeckBar::showOlder()
{
    step(-1);
}

void PromptDeckBar::showNewer()
{
    step(1);
}

void PromptDeckBar::showLatest()
{
    step(int(m_prompts.size()) - 1 - m_index);
}

void PromptDeckBar::setExpanded(bool expanded)
{
    if (m_expanded == expanded || m_prompts.isEmpty())
        return;
    m_expanded = expanded;
    update();
    emit heightChanged();
}

void PromptDeckBar::refreshAccessible()
{
    setAccessibleDescription(QStringList{shownPrompt(), positionText()}.filter(QRegularExpression(QStringLiteral("\\S"))).join(QStringLiteral(", ")));
    setToolTip(tr("Swipe or use the arrows to see earlier and later prompts. Click to read it all."));
}

void PromptDeckBar::mousePressEvent(QMouseEvent *e)
{
    if (e->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(e);
    m_pressAt = e->position().toPoint();
    m_pressed = true;
    m_dragging = false;
    e->accept();
}

void PromptDeckBar::mouseMoveEvent(QMouseEvent *e)
{
    const QPoint pos = e->position().toPoint();
    if (!m_pressed) {
        const Hit hover = hitAt(pos);
        if (hover != m_hover) {
            m_hover = hover;
            update();
        }
        return;
    }
    const int dx = pos.x() - m_pressAt.x();
    if (!m_dragging && std::abs(dx) > 6 && std::abs(dx) > std::abs(pos.y() - m_pressAt.y()))
        m_dragging = true;
    if (!m_dragging)
        return;
    const bool free = dx > 0 ? m_index > 0 : m_index < m_prompts.size() - 1;
    m_drag = free ? dx * 0.6 : dx * 0.15;
    update();
}

void PromptDeckBar::mouseReleaseEvent(QMouseEvent *e)
{
    if (!m_pressed || e->button() != Qt::LeftButton)
        return QWidget::mouseReleaseEvent(e);
    m_pressed = false;
    const QPoint pos = e->position().toPoint();
    if (m_dragging) {
        m_dragging = false;
        const int dx = pos.x() - m_pressAt.x();
        if (dx > kSwipe && m_index > 0) {
            step(-1);
        } else if (dx < -kSwipe && m_index < m_prompts.size() - 1) {
            step(1);
        } else {
            m_anim->stop();
            m_anim->setStartValue(m_drag);
            m_anim->setEndValue(qreal(0));
            m_drag = 0;
            m_anim->start();
        }
        return;
    }
    switch (hitAt(pos)) {
    case Hit::Older:
        showOlder();
        break;
    case Hit::Newer:
        showNewer();
        break;
    case Hit::Card:
        setExpanded(!m_expanded);
        break;
    case Hit::None:
        break;
    }
}

void PromptDeckBar::wheelEvent(QWheelEvent *e)
{
    const QPoint delta = e->pixelDelta().isNull() ? e->angleDelta() : e->pixelDelta();
    if (std::abs(delta.x()) <= std::abs(delta.y()) || m_prompts.size() < 2) {
        e->ignore();
        return;
    }
    e->accept();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_wheelStepAt < 350) {
        m_wheel = 0;
        return;
    }
    m_wheel += delta.x();
    if (std::abs(m_wheel) < 60)
        return;
    const bool towardOlder = m_wheel > 0;
    m_wheel = 0;
    m_wheelStepAt = now;
    step(towardOlder ? -1 : 1);
}

void PromptDeckBar::keyPressEvent(QKeyEvent *e)
{
    switch (e->key()) {
    case Qt::Key_Left:
        showOlder();
        break;
    case Qt::Key_Right:
        showNewer();
        break;
    case Qt::Key_End:
        showLatest();
        break;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_Space:
        setExpanded(!m_expanded);
        break;
    default:
        QWidget::keyPressEvent(e);
        return;
    }
    e->accept();
}

void PromptDeckBar::contextMenuEvent(QContextMenuEvent *e)
{
    if (m_prompts.isEmpty())
        return;
    QMenu menu(this);
    const QString text = shownPrompt();
    menu.addAction(tr("Copy prompt"), this, [text]() { QApplication::clipboard()->setText(text); });
    if (m_index != m_prompts.size() - 1)
        menu.addAction(tr("Back to the latest"), this, &PromptDeckBar::showLatest);
    menu.exec(e->globalPos());
}

void PromptDeckBar::leaveEvent(QEvent *e)
{
    if (m_hover != Hit::None) {
        m_hover = Hit::None;
        update();
    }
    QWidget::leaveEvent(e);
}
