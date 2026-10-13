#pragma once

// ProjectGantt: a plain QWidget that paints the projects tab's
// Gantt chart — projects as parent bars with their linked issues indented
// beneath. Header-only and Q_OBJECT-free (like IssueBurnupChart) so it needs no
// moc entry; the host section wraps it in a QScrollArea and feeds it rows plus
// a theme probe.

#include "Theme.h"
#include <QDateTime>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <QWidget>

#include <functional>

struct ProjectGanttRow {
    QString label;
    qint64 start = 0;      // epoch ms; 0 = unset
    qint64 end = 0;        // epoch ms; 0 = unset
    bool isProject = false;
    QString status;        // open | closed
    int progress = 0;      // 0..100
    int indent = 0;        // 0 = project, 1 = linked issue
    bool inferred = false; // dates were inferred (no signed dates event)
};

class ProjectGantt final : public QWidget
{
public:
    explicit ProjectGantt(QWidget *parent = nullptr) : QWidget(parent)
    {
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }

    void setRows(QList<ProjectGanttRow> rows)
    {
        m_rows = std::move(rows);
        setMinimumHeight(kAxisHeight + m_rows.size() * kRowHeight + 16);
        updateGeometry();
        update();
    }

    void setDarkProbe(std::function<bool()> probe) { m_darkProbe = std::move(probe); }

    QSize sizeHint() const override
    {
        return QSize(760, kAxisHeight + qMax(4, int(m_rows.size())) * kRowHeight + 16);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const bool dark = m_darkProbe ? m_darkProbe() : true;

        const QColor text = Theme::remapColor(QColor(dark ? "#e6edf3" : "#1f2328"));
        const QColor muted = Theme::remapColor(QColor(dark ? "#8b949e" : "#656d76"));
        const QColor grid = Theme::remapColor(QColor(dark ? "#30363d" : "#d8dee4"));
        const QColor rowAlt = Theme::remapColor(QColor(dark ? "#161b22" : "#f6f8fa"));
        const QColor projectBar(dark ? "#1f6feb" : "#54aeff");
        const QColor projectFill(dark ? "#58a6ff" : "#0969da");
        const QColor issueBar(dark ? "#238636" : "#4ac26b");
        const QColor issueFill(dark ? "#3fb950" : "#1a7f37");
        const QColor closedBar = Theme::remapColor(QColor(dark ? "#30363d" : "#d0d7de"));
        const QColor closedFill = Theme::remapColor(QColor(dark ? "#484f58" : "#8c959f"));
        const QColor todayColor(dark ? "#f78166" : "#cf222e");

        if (m_rows.isEmpty()) {
            painter.setPen(muted);
            painter.drawText(rect(), Qt::AlignCenter,
                             QStringLiteral("No projects to chart yet"));
            return;
        }

        const QRectF plot(kLabelGutter, kAxisHeight,
                          width() - kLabelGutter - 16.0,
                          m_rows.size() * qreal(kRowHeight));
        if (plot.width() <= 40)
            return;

        qint64 minTs = 0, maxTs = 0;
        timeRange(minTs, maxTs);
        const qint64 span = qMax<qint64>(kDayMs, maxTs - minTs);
        auto xFor = [&](qint64 ts) {
            return plot.left() + plot.width() * (ts - minTs) / double(span);
        };

        for (int i = 0; i < m_rows.size(); ++i) {
            if (i % 2 == 1)
                painter.fillRect(QRectF(0, plot.top() + i * kRowHeight,
                                        width(), kRowHeight),
                                 rowAlt);
        }

        const QDate first = QDateTime::fromMSecsSinceEpoch(minTs).date();
        const QDate last = QDateTime::fromMSecsSinceEpoch(maxTs).date();
        const qint64 days = first.daysTo(last);
        QList<QPair<qint64, QString>> ticks;
        if (days > 120) {
            QDate tick(first.year(), first.month(), 1);
            for (; tick <= last; tick = tick.addMonths(1))
                ticks.append({tick.startOfDay().toMSecsSinceEpoch(),
                              tick.toString(days > 500 ? QStringLiteral("MMM yy")
                                                       : QStringLiteral("MMM"))});
        } else if (days > 21) {
            QDate tick = first.addDays((8 - first.dayOfWeek()) % 7); // next Monday
            for (; tick <= last; tick = tick.addDays(7))
                ticks.append({tick.startOfDay().toMSecsSinceEpoch(),
                              tick.toString(QStringLiteral("MMM d"))});
        } else {
            for (QDate tick = first; tick <= last; tick = tick.addDays(qMax<qint64>(1, days / 10)))
                ticks.append({tick.startOfDay().toMSecsSinceEpoch(),
                              tick.toString(QStringLiteral("MMM d"))});
        }
        QFont axisFont = font();
        axisFont.setPointSizeF(qMax(7.5, axisFont.pointSizeF() - 1.5));
        painter.setFont(axisFont);
        for (const auto &tick : ticks) {
            const qreal x = xFor(tick.first);
            if (x < plot.left() - 1 || x > plot.right() + 1)
                continue;
            painter.setPen(QPen(grid, 1));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            painter.setPen(muted);
            painter.drawText(QRectF(x - 40, 8, 80, kAxisHeight - 12),
                             Qt::AlignHCenter | Qt::AlignBottom, tick.second);
        }
        painter.setPen(QPen(grid, 1));
        painter.drawLine(QPointF(plot.left(), plot.top()),
                         QPointF(plot.right(), plot.top()));

        for (int i = 0; i < m_rows.size(); ++i) {
            const ProjectGanttRow &row = m_rows.at(i);
            const qreal rowTop = plot.top() + i * kRowHeight;
            const bool closed = row.status == QLatin1String("closed");

            QFont labelFont = font();
            labelFont.setPointSizeF(qMax(7.5, labelFont.pointSizeF() - 1.0));
            labelFont.setBold(row.isProject);
            painter.setFont(labelFont);
            painter.setPen(closed ? muted : text);
            const qreal indentPx = 10.0 + row.indent * 16.0;
            const QString elided = QFontMetrics(labelFont).elidedText(
                row.label, Qt::ElideRight, int(kLabelGutter - indentPx - 8));
            painter.drawText(QRectF(indentPx, rowTop, kLabelGutter - indentPx - 8,
                                    kRowHeight),
                             Qt::AlignLeft | Qt::AlignVCenter, elided);

            const qreal barH = row.isProject ? 16.0 : 10.0;
            const qreal barTop = rowTop + (kRowHeight - barH) / 2.0;
            const qreal x1 = qBound(plot.left(), xFor(row.start), plot.right());
            const qreal x2 = qBound(plot.left(), xFor(row.end), plot.right());
            QRectF bar(x1, barTop, qMax<qreal>(4.0, x2 - x1), barH);

            const QColor base = closed ? closedBar
                                       : (row.isProject ? projectBar : issueBar);
            const QColor fill = closed ? closedFill
                                       : (row.isProject ? projectFill : issueFill);
            if (row.inferred) {
                // No signed dates yet: a hollow, dashed placeholder spanning the
                // inferred createdAt -> today range.
                QPen dashed(closed ? closedFill : base, 1.2, Qt::DashLine);
                painter.setPen(dashed);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(bar, barH / 2.0, barH / 2.0);
            } else {
                QColor track = base;
                track.setAlphaF(closed ? 0.45 : 0.35);
                painter.setPen(Qt::NoPen);
                painter.setBrush(track);
                painter.drawRoundedRect(bar, barH / 2.0, barH / 2.0);
                const int pct = closed ? 100 : qBound(0, row.progress, 100);
                if (pct > 0) {
                    QRectF done = bar;
                    done.setWidth(qMax<qreal>(barH, bar.width() * pct / 100.0));
                    QColor doneColor = fill;
                    if (closed)
                        doneColor.setAlphaF(0.8);
                    painter.setBrush(doneColor);
                    painter.drawRoundedRect(done, barH / 2.0, barH / 2.0);
                }
            }
        }

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (now >= minTs && now <= maxTs) {
            const qreal x = xFor(now);
            painter.setPen(QPen(todayColor, 1.4));
            painter.drawLine(QPointF(x, plot.top() - 4), QPointF(x, plot.bottom()));
            painter.setFont(axisFont);
            painter.drawText(QRectF(x - 40, plot.top() - kAxisHeight + 6, 80, 14),
                             Qt::AlignHCenter | Qt::AlignTop,
                             QStringLiteral("Today"));
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const int row = int((event->position().y() - kAxisHeight) / kRowHeight);
        if (row < 0 || row >= m_rows.size()) {
            QToolTip::hideText();
            return;
        }
        const ProjectGanttRow &r = m_rows.at(row);
        const QString range =
            QString::fromUtf8("%1 \xE2\x86\x92 %2")
                .arg(QDateTime::fromMSecsSinceEpoch(r.start)
                         .toString(QStringLiteral("yyyy-MM-dd")),
                     QDateTime::fromMSecsSinceEpoch(r.end)
                         .toString(QStringLiteral("yyyy-MM-dd")));
        QToolTip::showText(
            event->globalPosition().toPoint(),
            QString::fromUtf8("%1\n%2%3 \xC2\xB7 %4% complete")
                .arg(r.label, range,
                     r.inferred ? QStringLiteral(" (no dates set)") : QString())
                .arg(r.status == QLatin1String("closed") ? 100
                                                         : qBound(0, r.progress, 100)),
            this);
    }

private:
    static constexpr int kLabelGutter = 180;
    static constexpr int kAxisHeight = 40;
    static constexpr int kRowHeight = 28;
    static constexpr qint64 kDayMs = 24 * 60 * 60 * 1000LL;

    void timeRange(qint64 &minTs, qint64 &maxTs) const
    {
        minTs = 0;
        maxTs = 0;
        for (const ProjectGanttRow &row : m_rows) {
            if (row.start > 0 && (minTs == 0 || row.start < minTs))
                minTs = row.start;
            if (row.end > maxTs)
                maxTs = row.end;
        }
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (minTs == 0)
            minTs = now - 14 * kDayMs;
        if (maxTs == 0)
            maxTs = now + 14 * kDayMs;
        const qint64 pad = qMax<qint64>(kDayMs, (maxTs - minTs) / 20);
        minTs -= pad;
        maxTs += pad;
    }

    QList<ProjectGanttRow> m_rows;
    std::function<bool()> m_darkProbe;
};
