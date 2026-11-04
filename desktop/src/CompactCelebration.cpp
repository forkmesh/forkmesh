#include "CompactCelebration.h"

#include <QApplication>
#include <QFontMetrics>
#include <QPainter>
#include <QPainterPath>
#include <QTextLayout>
#include <QTextOption>
#include <QTimer>

#include <cmath>

namespace forkmesh::ui {

namespace {

constexpr int kPaddingX = 18;
constexpr int kPaddingY = 12;
constexpr int kLineGap = 4;
constexpr int kMinCardWidth = 200;
constexpr int kHeadlineMaxLines = 3;
constexpr int kDetailMaxLines = 4;
constexpr int kCardRadius = 14;

QStringList wrapLines(const QString &text, const QFont &font, int width, int maxLines)
{
    QStringList lines;
    const QString flat = text.simplified();
    if (flat.isEmpty() || width <= 0 || maxLines <= 0)
        return lines;
    QTextLayout layout(flat, font);
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    layout.setTextOption(option);
    QList<QPair<int, int>> spans;
    layout.beginLayout();
    for (QTextLine line = layout.createLine(); line.isValid(); line = layout.createLine()) {
        line.setLineWidth(width);
        spans.append({line.textStart(), line.textLength()});
    }
    layout.endLayout();
    const QFontMetrics metrics(font);
    for (int index = 0; index < spans.size(); ++index) {
        if (index == maxLines - 1 && spans.size() > maxLines) {
            lines.append(metrics.elidedText(flat.mid(spans.at(index).first).trimmed(),
                                            Qt::ElideRight, width));
            break;
        }
        const QString line = flat.mid(spans.at(index).first, spans.at(index).second).trimmed();
        lines.append(metrics.horizontalAdvance(line) > width
                         ? metrics.elidedText(line, Qt::ElideRight, width)
                         : line);
    }
    return lines;
}

int balancedWidth(const QString &text, const QFont &font, int width, int maxLines)
{
    const int lines = int(wrapLines(text, font, width, maxLines + 1).size());
    if (lines <= 1 || lines > maxLines)
        return width;
    int low = width / 2;
    int high = width;
    while (low < high) {
        const int middle = (low + high) / 2;
        if (int(wrapLines(text, font, middle, maxLines + 1).size()) <= lines)
            high = middle;
        else
            low = middle + 1;
    }
    return high;
}

int widest(const QStringList &lines, const QFont &font)
{
    const QFontMetrics metrics(font);
    int wide = 0;
    for (const QString &line : lines)
        wide = qMax(wide, metrics.horizontalAdvance(line));
    return wide;
}

}

CompactCelebrationLayout CompactCelebrationOverlay::layoutFor(const QFont &base,
                                                              const QString &headline,
                                                              const QString &detail,
                                                              const QSize &window)
{
    CompactCelebrationLayout out;
    out.headlineFont = base;
    out.headlineFont.setPointSizeF(kHeadlinePoints);
    out.headlineFont.setBold(true);
    out.detailFont = base;
    out.detailFont.setPointSizeF(kDetailPoints);
    out.detailFont.setBold(false);
    const QFontMetrics headMetrics(out.headlineFont);
    const QFontMetrics detailMetrics(out.detailFont);
    out.headlineLineHeight = headMetrics.height();
    out.detailLineHeight = detailMetrics.height();

    const int maxCard = qMax(1, qMin(kMaxCardWidth, window.width() - 2 * kWindowMargin));
    const int textRoom = qMax(1, maxCard - 2 * kPaddingX);
    const int maxHigh = qMax(1, window.height() - 2 * kWindowMargin);
    const int roomLines = qMax(1, (maxHigh - 2 * kPaddingY) / qMax(1, out.detailLineHeight));
    const int headLines = qBound(1, roomLines, kHeadlineMaxLines);
    out.headlineLines =
        wrapLines(headline, out.headlineFont,
                  balancedWidth(headline, out.headlineFont, textRoom, headLines), headLines);
    const int usedHead = int(out.headlineLines.size()) * out.headlineLineHeight;
    const int detailRoom =
        (maxHigh - 2 * kPaddingY - usedHead - kLineGap) / qMax(1, out.detailLineHeight);
    out.detailLines = wrapLines(detail, out.detailFont, textRoom, qMin(kDetailMaxLines, detailRoom));

    const int textWide = qMax(widest(out.headlineLines, out.headlineFont),
                              widest(out.detailLines, out.detailFont));
    const int wide = qMin(maxCard, qMax(qMin(kMinCardWidth, maxCard), textWide + 2 * kPaddingX));
    int high = 2 * kPaddingY + usedHead;
    if (!out.detailLines.isEmpty())
        high += kLineGap + int(out.detailLines.size()) * out.detailLineHeight;
    out.card = QSize(wide, high);
    return out;
}

CompactCelebrationOverlay::CompactCelebrationOverlay(QWidget *parent, const QString &headline,
                                                     const QString &detail)
    : QWidget(parent), m_headline(headline), m_detail(detail)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    setAttribute(Qt::WA_ShowWithoutActivating);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(QStringLiteral("Celebration"));
    relayout();
    m_timer = new QTimer(this);
    m_timer->setInterval(kTickMs);
    connect(m_timer, &QTimer::timeout, this, [this] { tick(kTickMs); });
    m_timer->start();
    raise();
    show();
}

void CompactCelebrationOverlay::showMessage(const QString &headline, const QString &detail)
{
    m_headline = headline;
    m_detail = detail;
    m_elapsedMs = qMin(m_elapsedMs, kPopMs);
    relayout();
    raise();
    if (m_timer && !m_timer->isActive())
        m_timer->start();
    update();
}

QRect CompactCelebrationOverlay::cardGeometry() const
{
    return QRect(geometry().topLeft() + QPoint(kSparkleMargin, kSparkleMargin), m_layout.card);
}

void CompactCelebrationOverlay::advanceForTest(int ms)
{
    tick(ms);
}

void CompactCelebrationOverlay::relayout()
{
    const QWidget *host = parentWidget();
    m_window = host ? host->size() : QSize(640, 480);
    m_layout = layoutFor(host ? host->font() : QApplication::font(), m_headline, m_detail,
                         m_window);
    const QSize outer = m_layout.card + QSize(2 * kSparkleMargin, 2 * kSparkleMargin);
    setGeometry(QRect(QPoint((m_window.width() - outer.width()) / 2,
                             (m_window.height() - outer.height()) / 2),
                      outer));
}

void CompactCelebrationOverlay::tick(int ms)
{
    m_elapsedMs += ms;
    if (m_elapsedMs >= kDurationMs) {
        if (m_timer)
            m_timer->stop();
        hide();
        deleteLater();
        return;
    }
    if (parentWidget() && parentWidget()->size() != m_window)
        relayout();
    update();
}

void CompactCelebrationOverlay::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const int remaining = kDurationMs - m_elapsedMs;
    const qreal fade = remaining >= kFadeMs ? 1.0 : qMax(0.0, qreal(remaining) / kFadeMs);
    const qreal grow = m_elapsedMs < kPopMs ? qreal(m_elapsedMs) / kPopMs : 1.0;
    const qreal pop = 0.92 + 0.08 * grow;
    p.setOpacity(qBound(0.0, fade * qMin(1.0, 0.35 + grow), 1.0));

    const QRectF card(kSparkleMargin, kSparkleMargin, m_layout.card.width(),
                      m_layout.card.height());
    p.translate(card.center());
    p.scale(pop, pop);
    p.translate(-card.center());

    p.setPen(Qt::NoPen);
    p.setBrush(QColor(15, 23, 42, 228));
    p.drawRoundedRect(card, kCardRadius, kCardRadius);

    const qreal phase = m_elapsedMs / 1000.0;
    const QPointF sparkles[] = {
        card.topLeft() + QPointF(-4, 6),       card.topRight() + QPointF(4, 10),
        card.bottomLeft() + QPointF(10, 5),    card.bottomRight() + QPointF(-14, 5),
        QPointF(card.center().x() - card.width() * 0.3, card.top() - 6),
        QPointF(card.center().x() + card.width() * 0.25, card.bottom() + 6),
    };
    const int hues[] = {45, 330, 190, 45, 280, 140};
    for (int index = 0; index < 6; ++index) {
        const qreal twinkle = 0.5 + 0.5 * std::sin(phase * 5.0 + index * 1.7);
        const qreal arm = 2.5 + 3.0 * twinkle;
        QColor color = QColor::fromHsv(hues[index], 150, 255);
        color.setAlphaF(qBound(0.0, 0.35 + 0.65 * twinkle, 1.0));
        const QPointF c = sparkles[index];
        QPainterPath star;
        star.moveTo(c.x(), c.y() - arm);
        star.quadTo(c, QPointF(c.x() + arm, c.y()));
        star.quadTo(c, QPointF(c.x(), c.y() + arm));
        star.quadTo(c, QPointF(c.x() - arm, c.y()));
        star.quadTo(c, QPointF(c.x(), c.y() - arm));
        p.fillPath(star, color);
    }

    const qreal textLeft = card.left() + kPaddingX;
    const qreal textWide = card.width() - 2 * kPaddingX;
    qreal y = card.top() + kPaddingY;
    p.setPen(Qt::white);
    p.setFont(m_layout.headlineFont);
    for (const QString &line : std::as_const(m_layout.headlineLines)) {
        p.drawText(QRectF(textLeft, y, textWide, m_layout.headlineLineHeight),
                   Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine, line);
        y += m_layout.headlineLineHeight;
    }
    if (!m_layout.detailLines.isEmpty()) {
        y += kLineGap;
        QColor soft(Qt::white);
        soft.setAlphaF(0.85);
        p.setPen(soft);
        p.setFont(m_layout.detailFont);
        for (const QString &line : std::as_const(m_layout.detailLines)) {
            p.drawText(QRectF(textLeft, y, textWide, m_layout.detailLineHeight),
                       Qt::AlignHCenter | Qt::AlignVCenter | Qt::TextSingleLine, line);
            y += m_layout.detailLineHeight;
        }
    }
}

}
