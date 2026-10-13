#include "CrmTeamChart.h"

#include <QEvent>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QToolTip>

#include <algorithm>

namespace forkmesh::crm {
namespace {

constexpr qreal kLegendHeight = 24.0;
constexpr qreal kAxisWidth = 36.0;
constexpr qreal kLabelHeight = 22.0;
constexpr qreal kPadRight = 8.0;
constexpr qreal kSegmentGap = 2.0;
constexpr qreal kCornerRadius = 4.0;
constexpr int kGridLines = 4;

const char *const kSeriesLabels[] = {"Emails", "Outreach", "Contacts added"};

int dayValue(const CrmTeamChart::Day &day, int series)
{
    return series == 0 ? day.emails : series == 1 ? day.outreach : day.contacts;
}

QPainterPath topRounded(const QRectF &rect, qreal radius)
{
    const qreal r = std::min({radius, rect.width() / 2, rect.height()});
    QPainterPath path;
    path.moveTo(rect.bottomLeft());
    path.lineTo(rect.left(), rect.top() + r);
    path.quadTo(rect.topLeft(), QPointF(rect.left() + r, rect.top()));
    path.lineTo(rect.right() - r, rect.top());
    path.quadTo(rect.topRight(), QPointF(rect.right(), rect.top() + r));
    path.lineTo(rect.bottomRight());
    path.closeSubpath();
    return path;
}

} // namespace

CrmTeamChart::CrmTeamChart(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(QStringLiteral("Daily CRM activity"));
}

QColor CrmTeamChart::seriesColor(int series, bool dark)
{
    static const QColor light[] = {QColor(0x2a, 0x78, 0xd6), QColor(0xeb, 0x68, 0x34),
                                   QColor(0x1b, 0xaf, 0x7a)};
    static const QColor night[] = {QColor(0x39, 0x87, 0xe5), QColor(0xd9, 0x59, 0x26),
                                   QColor(0x19, 0x9e, 0x70)};
    const int index = std::clamp(series, 0, 2);
    return dark ? night[index] : light[index];
}

void CrmTeamChart::setDays(const QList<Day> &days)
{
    m_days = days;
    m_hover = -1;
    update();
}

QSize CrmTeamChart::sizeHint() const
{
    return {640, 280};
}

QSize CrmTeamChart::minimumSizeHint() const
{
    return {320, 180};
}

bool CrmTeamChart::dark() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

QRectF CrmTeamChart::plotRect() const
{
    return QRectF(kAxisWidth, kLegendHeight + 6,
                  std::max(1.0, width() - kAxisWidth - kPadRight),
                  std::max(1.0, height() - kLegendHeight - 6 - kLabelHeight));
}

int CrmTeamChart::axisStep() const
{
    int peak = 0;
    for (const Day &day : m_days)
        peak = std::max(peak, day.total());
    return std::max(1, (peak + kGridLines - 1) / kGridLines);
}

int CrmTeamChart::dayAt(const QPointF &position) const
{
    const QRectF plot = plotRect();
    if (m_days.isEmpty() || position.x() < plot.left() || position.x() >= plot.right() ||
        position.y() < plot.top() || position.y() > plot.bottom())
        return -1;
    const qreal slot = plot.width() / m_days.size();
    return std::clamp(int((position.x() - plot.left()) / slot), 0, int(m_days.size()) - 1);
}

void CrmTeamChart::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool isDark = dark();
    const QColor ink = palette().color(QPalette::WindowText);
    QColor muted = ink;
    muted.setAlphaF(0.6f);
    QColor grid = ink;
    grid.setAlphaF(0.12f);
    QFont small = font();
    small.setPointSizeF(small.pointSizeF() * 0.85);
    painter.setFont(small);
    const QFontMetricsF metrics(small);

    qreal legendX = width() - kPadRight;
    for (int series = 2; series >= 0; --series) {
        const QString label = QString::fromLatin1(kSeriesLabels[series]);
        const qreal textWidth = metrics.horizontalAdvance(label);
        legendX -= textWidth;
        painter.setPen(muted);
        painter.drawText(QRectF(legendX, 0, textWidth + 1, kLegendHeight),
                         Qt::AlignVCenter | Qt::AlignLeft, label);
        legendX -= 14;
        painter.setPen(Qt::NoPen);
        painter.setBrush(seriesColor(series, isDark));
        painter.drawRoundedRect(QRectF(legendX, kLegendHeight / 2 - 5, 10, 10), 2, 2);
        legendX -= 14;
    }

    const QRectF plot = plotRect();
    const int step = axisStep();
    const qreal max = step * kGridLines;
    for (int line = 0; line <= kGridLines; ++line) {
        const qreal y = plot.bottom() - plot.height() * line / kGridLines;
        painter.setPen(QPen(grid, 1));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(muted);
        painter.drawText(QRectF(0, y - 8, kAxisWidth - 6, 16), Qt::AlignRight | Qt::AlignVCenter,
                         QLocale().toString(step * line));
    }
    if (m_days.isEmpty())
        return;

    const qreal slot = plot.width() / m_days.size();
    const qreal bar = std::clamp(slot - 4, 3.0, 28.0);
    const int labelEvery = m_days.size() > 10 ? 5 : 1;
    for (int index = 0; index < m_days.size(); ++index) {
        const Day &day = m_days.at(index);
        const qreal x = plot.left() + index * slot + (slot - bar) / 2;
        if (index == m_hover) {
            QColor hover = ink;
            hover.setAlphaF(0.06f);
            painter.setPen(Qt::NoPen);
            painter.setBrush(hover);
            painter.drawRect(QRectF(plot.left() + index * slot, plot.top(), slot, plot.height()));
        }
        QList<int> shown;
        for (int series = 0; series < 3; ++series) {
            if (dayValue(day, series) > 0)
                shown.append(series);
        }
        qreal base = plot.bottom();
        for (int position = 0; position < shown.size(); ++position) {
            const int series = shown.at(position);
            const qreal full = plot.height() * dayValue(day, series) / max;
            const qreal gap = position == 0 ? 0.0 : kSegmentGap;
            base -= full;
            const QRectF rect(x, base + gap, bar, std::max(1.0, full - gap));
            painter.setPen(Qt::NoPen);
            painter.setBrush(seriesColor(series, isDark));
            if (position == shown.size() - 1)
                painter.drawPath(topRounded(rect, kCornerRadius));
            else
                painter.drawRect(rect);
        }
        if ((m_days.size() - 1 - index) % labelEvery == 0) {
            painter.setPen(muted);
            const QString label = QLocale().toString(day.date, QStringLiteral("MMM d"));
            const qreal labelWidth = metrics.horizontalAdvance(label) + 2;
            const qreal labelLeft = std::clamp(x + bar / 2 - labelWidth / 2, 0.0,
                                               width() - labelWidth);
            painter.drawText(QRectF(labelLeft, plot.bottom() + 4, labelWidth, kLabelHeight - 4),
                             Qt::AlignHCenter | Qt::AlignTop, label);
        }
    }
}

void CrmTeamChart::mouseMoveEvent(QMouseEvent *event)
{
    const int hover = dayAt(event->position());
    if (hover != m_hover) {
        m_hover = hover;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void CrmTeamChart::leaveEvent(QEvent *event)
{
    m_hover = -1;
    update();
    QWidget::leaveEvent(event);
}

bool CrmTeamChart::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int index = dayAt(help->pos());
        if (index < 0) {
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        const Day &day = m_days.at(index);
        QStringList lines{QLocale().toString(day.date, QStringLiteral("ddd MMM d"))};
        for (int series = 0; series < 3; ++series) {
            lines.append(QStringLiteral("%1: %2").arg(QString::fromLatin1(kSeriesLabels[series]),
                                                      QLocale().toString(dayValue(day, series))));
        }
        QToolTip::showText(help->globalPos(), lines.join(QLatin1Char('\n')), this);
        return true;
    }
    return QWidget::event(event);
}

} // namespace forkmesh::crm
