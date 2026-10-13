#include "CrmStageStrip.h"

#include <QEvent>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRegularExpression>
#include <QToolTip>

#include <algorithm>

namespace forkmesh::crm {
namespace {

// Space between one chevron's tip and the notch of the next.
constexpr qreal kGap = 3.0;
constexpr qreal kPadLeft = 14.0;
constexpr qreal kPadRight = 10.0;
constexpr qreal kCountSpacing = 8.0;
constexpr qreal kCornerRadius = 6.0;

QColor blend(const QColor &from, const QColor &to, qreal amount)
{
    return QColor::fromRgbF(
        float(from.redF() + (to.redF() - from.redF()) * amount),
        float(from.greenF() + (to.greenF() - from.greenF()) * amount),
        float(from.blueF() + (to.blueF() - from.blueF()) * amount));
}

qreal luminance(const QColor &color)
{
    return 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
}

QFont countFont(QFont font)
{
    font.setBold(true);
    return font;
}

} // namespace

CrmStageStrip::CrmStageStrip(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("crmStageStrip"));
    setAccessibleName(QStringLiteral("Pipeline stages"));
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void CrmStageStrip::setStages(const QList<Stage> &stages, const QString &selected)
{
    m_stages = stages;
    m_selected = selected;
    if (m_hover >= m_stages.size())
        m_hover = -1;
    QStringList parts;
    for (const Stage &stage : std::as_const(m_stages))
        parts << QStringLiteral("%1 %2").arg(stage.label).arg(stage.count);
    setAccessibleDescription(parts.join(QStringLiteral(", ")));
    updateGeometry();
    update();
}

QString CrmStageStrip::stageLabel(const QString &name)
{
    QString label = name;
    label.replace(QRegularExpression(QStringLiteral("[-_]+")), QStringLiteral(" "));
    label = label.simplified();
    if (label.isEmpty())
        return QStringLiteral("No stage");
    label[0] = label.at(0).toUpper();
    return label;
}

QColor CrmStageStrip::stageColor(const QString &name, int index)
{
    const QString key = name.toLower();
    const auto any = [&key](std::initializer_list<const char *> words) {
        return std::any_of(words.begin(), words.end(), [&key](const char *word) {
            return key.contains(QLatin1String(word));
        });
    };
    // Order matters: "closed-lost" is lost, "disqualified" is not qualified.
    if (any({"lost", "churn", "reject", "dead", "disqualif", "cancel"}))
        return QColor(QStringLiteral("#f85149"));
    if (any({"won", "complete", "done", "customer", "closed", "signed", "paid", "success"}))
        return QColor(QStringLiteral("#3fb950"));
    if (any({"progress", "contacted", "working", "active", "follow", "ongoing"}))
        return QColor(QStringLiteral("#e3b341"));
    if (any({"qualif", "proposal", "negotiat", "demo", "meeting", "review", "trial"}))
        return QColor(QStringLiteral("#bc8cff"));
    if (any({"new", "lead", "prospect", "inbox", "todo", "open", "backlog"}))
        return QColor(QStringLiteral("#58a6ff"));
    static const char *const kPalette[] = {"#58a6ff", "#e3b341", "#bc8cff", "#39c5cf",
                                           "#db61a2", "#3fb950", "#db6d28", "#f85149"};
    return QColor(QLatin1String(kPalette[qMax(0, index) % 8]));
}

qreal CrmStageStrip::arrowDepth() const
{
    return qRound(height() * 0.32);
}

qreal CrmStageStrip::naturalBody(int index) const
{
    const Stage &stage = m_stages.at(index);
    const qreal label = QFontMetricsF(font()).horizontalAdvance(stage.label);
    const qreal count = QFontMetricsF(countFont(font()))
                            .horizontalAdvance(QLocale().toString(stage.count));
    return (index > 0 ? arrowDepth() : 0.0) + kPadLeft + label + kCountSpacing + count +
           kPadRight;
}

QSize CrmStageStrip::sizeHint() const
{
    const int h = int(QFontMetricsF(countFont(font())).height()) + 16;
    qreal width = 0;
    for (int at = 0; at < m_stages.size(); ++at)
        width += naturalBody(at) + kGap;
    return {int(width + h * 0.32), h};
}

QSize CrmStageStrip::minimumSizeHint() const
{
    const QSize hint = sizeHint();
    return {int(m_stages.size() * (hint.height() * 0.64 + 36)), hint.height()};
}

// Chevron bodies share the width: each at least as wide as its words when the
// strip has room, all shrunk alike (labels elided) when it does not.
QList<CrmStageStrip::Segment> CrmStageStrip::segments() const
{
    QList<Segment> result;
    const int n = int(m_stages.size());
    if (n == 0)
        return result;
    const qreal depth = arrowDepth();
    QList<qreal> bodies;
    qreal natural = 0;
    for (int at = 0; at < n; ++at) {
        bodies << naturalBody(at);
        natural += bodies.last();
    }
    const qreal room = width() - (n - 1) * kGap - depth;
    qreal left = 0;
    for (int at = 0; at < n; ++at) {
        qreal body = room >= natural ? bodies.at(at) + (room - natural) / n
                                     : bodies.at(at) * qMax(0.0, room) / natural;
        body = qMax(body, (at > 0 ? depth : 0.0) + 12.0);
        result.append({left, body});
        left += body + kGap;
    }
    return result;
}

int CrmStageStrip::segmentAt(const QPointF &position) const
{
    const QList<Segment> list = segments();
    const qreal depth = arrowDepth();
    for (int at = 0; at < list.size(); ++at) {
        const Segment &segment = list.at(at);
        // Past the notch of the next chevron counts as that chevron.
        const qreal end = segment.left + segment.body + (at + 1 < list.size() ? kGap : depth);
        if (position.x() >= segment.left && position.x() < end)
            return at;
    }
    return -1;
}

void CrmStageStrip::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QList<Segment> list = segments();
    const qreal depth = arrowDepth();
    const qreal h = height();
    const QColor window = palette().color(QPalette::Window);
    const bool dark = window.lightness() < 128;
    const QFont labelFont = font();
    const QFont numberFont = countFont(font());
    const QFontMetricsF labelMetrics(labelFont);
    const QFontMetricsF numberMetrics(numberFont);
    for (int at = 0; at < list.size(); ++at) {
        const Stage &stage = m_stages.at(at);
        const Segment &segment = list.at(at);
        const qreal x = segment.left;
        const qreal right = x + segment.body;
        const qreal notch = at > 0 ? depth : 0.0;

        QPainterPath path;
        if (at == 0) {
            path.moveTo(x + kCornerRadius, 0);
            path.lineTo(right, 0);
            path.lineTo(right + depth, h / 2);
            path.lineTo(right, h);
            path.lineTo(x + kCornerRadius, h);
            path.quadTo(x, h, x, h - kCornerRadius);
            path.lineTo(x, kCornerRadius);
            path.quadTo(x, 0, x + kCornerRadius, 0);
        } else {
            path.moveTo(x, 0);
            path.lineTo(right, 0);
            path.lineTo(right + depth, h / 2);
            path.lineTo(right, h);
            path.lineTo(x, h);
            path.lineTo(x + notch, h / 2);
        }
        path.closeSubpath();

        const QColor color = stageColor(stage.name, at);
        const bool selected = !m_selected.isEmpty() && stage.name == m_selected;
        const bool hovered = at == m_hover;
        QColor fill;
        QColor ink;
        if (selected) {
            fill = hovered ? (dark ? color.lighter(112) : color.darker(108)) : color;
            ink = luminance(fill) > 0.55 ? QColor(QStringLiteral("#1f2328")) : QColor(Qt::white);
        } else {
            qreal amount = dark ? 0.26 : 0.16;
            if (!m_selected.isEmpty())
                amount *= 0.6; // another stage is the filter
            if (hovered)
                amount += 0.12;
            fill = blend(window, color, amount);
            ink = dark ? color.lighter(135) : color.darker(150);
            if (stage.count == 0)
                ink.setAlphaF(0.65f);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        painter.drawPath(path);

        // Name and count, centered together in the chevron.
        const QString number = QLocale().toString(stage.count);
        const qreal numberWidth = numberMetrics.horizontalAdvance(number);
        const qreal areaLeft = x + notch + kPadLeft * 0.6;
        const qreal areaRight = right + depth * 0.5 - kPadRight * 0.6;
        const qreal labelRoom =
            qMax(0.0, areaRight - areaLeft - numberWidth - kCountSpacing);
        const QString label =
            labelMetrics.elidedText(stage.label, Qt::ElideRight, labelRoom);
        const qreal labelWidth = label.isEmpty() ? 0.0 : labelMetrics.horizontalAdvance(label);
        const qreal content = labelWidth + (label.isEmpty() ? 0.0 : kCountSpacing) + numberWidth;
        qreal textLeft = areaLeft + qMax(0.0, (areaRight - areaLeft - content) / 2);
        painter.setPen(ink);
        if (!label.isEmpty()) {
            painter.setFont(labelFont);
            painter.drawText(QRectF(textLeft, 0, labelWidth + 1, h),
                             Qt::AlignLeft | Qt::AlignVCenter, label);
            textLeft += labelWidth + kCountSpacing;
        }
        painter.setFont(numberFont);
        painter.drawText(QRectF(textLeft, 0, numberWidth + 1, h),
                         Qt::AlignLeft | Qt::AlignVCenter, number);
    }
}

void CrmStageStrip::mouseMoveEvent(QMouseEvent *event)
{
    const int hover = segmentAt(event->position());
    if (hover != m_hover) {
        m_hover = hover;
        setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void CrmStageStrip::mouseReleaseEvent(QMouseEvent *event)
{
    const int at = event->button() == Qt::LeftButton ? segmentAt(event->position()) : -1;
    if (at < 0 || !onPicked) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    // The callback may hand this strip new stages, so nothing of the old
    // list is read after it runs.
    const QString name = m_stages.at(at).name;
    const auto picked = onPicked;
    picked(name == m_selected ? QString() : name);
}

void CrmStageStrip::leaveEvent(QEvent *event)
{
    if (m_hover >= 0) {
        m_hover = -1;
        unsetCursor();
        update();
    }
    QWidget::leaveEvent(event);
}

bool CrmStageStrip::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        const auto *help = static_cast<QHelpEvent *>(event);
        const int at = segmentAt(help->pos());
        if (at < 0) {
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        const Stage &stage = m_stages.at(at);
        QToolTip::showText(
            help->globalPos(),
            QStringLiteral("%1 %2 at %3\n%4")
                .arg(QLocale().toString(stage.count),
                     stage.count == 1 ? QStringLiteral("contact") : QStringLiteral("contacts"),
                     stage.label,
                     stage.name == m_selected ? QStringLiteral("Click to show every stage")
                                              : QStringLiteral("Click to show only this stage")),
            this);
        return true;
    }
    return QWidget::event(event);
}

} // namespace forkmesh::crm
