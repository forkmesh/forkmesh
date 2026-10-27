#include "WorkflowsRunFlow.h"

#include "WorkflowsModel.h"

#include <QEasingCurve>
#include <QFontMetricsF>
#include <QHelpEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QRandomGenerator>
#include <QTableWidget>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace forkmesh::workflows {
namespace {

constexpr int kSlot = 104;
constexpr int kHeight = 84;
constexpr qreal kRadius = 12.0;
constexpr qint64 kStagger = 160;
constexpr qint64 kReveal = 70;
constexpr qint64 kAppear = 280;
constexpr qint64 kPop = 440;
constexpr qint64 kFill = 320;
constexpr int kFrameMs = 33;
constexpr qint64 kSparkleMs = 1400;
constexpr int kSparks = 28;
constexpr qreal kPi = 3.14159265358979323846;

bool doneStatus(const QString &status)
{
    return status == QLatin1String("success") || status == QLatin1String("error") ||
           status == QLatin1String("skipped") || status == QLatin1String("stopped");
}

qreal unit(qreal value)
{
    return std::clamp(value, 0.0, 1.0);
}

QColor faded(QColor color, qreal alpha)
{
    color.setAlphaF(float(unit(color.alphaF() * alpha)));
    return color;
}

QColor statusColor(const Tones &tones, const QString &status)
{
    if (status == QLatin1String("success"))
        return tones.success;
    if (status == QLatin1String("error"))
        return tones.danger;
    if (status == QLatin1String("stopped"))
        return tones.attention;
    if (status == QLatin1String("running"))
        return tones.accent;
    return tones.strong;
}

QPainterPath starPath(qreal radius)
{
    const qreal waist = radius * 0.28;
    QPainterPath path;
    path.moveTo(0, -radius);
    path.lineTo(waist, -waist);
    path.lineTo(radius, 0);
    path.lineTo(waist, waist);
    path.lineTo(0, radius);
    path.lineTo(-waist, waist);
    path.lineTo(-radius, 0);
    path.lineTo(-waist, -waist);
    path.closeSubpath();
    return path;
}

void glow(QPainter &painter, const QPointF &at, qreal radius, const QColor &color)
{
    QRadialGradient gradient(at, radius);
    gradient.setColorAt(0, color);
    gradient.setColorAt(1, faded(color, 0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawEllipse(at, radius, radius);
}

} // namespace

RunFlow::RunFlow(QWidget *parent) : QWidget(parent)
{
    setObjectName(QStringLiteral("workflowsRunFlow"));
    setAccessibleName(QStringLiteral("Run steps"));
    setAttribute(Qt::WA_OpaquePaintEvent, false);
    setAutoFillBackground(false);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_tones = Tones::from(palette());
    m_clock.start();
    m_timer.setInterval(kFrameMs);
    QObject::connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
}

void RunFlow::setTones(const Tones &tones)
{
    m_tones = tones;
    update();
}

qint64 RunFlow::clock() const
{
    return m_clock.elapsed() + 1;
}

QString RunFlow::statusAt(const Node &node, qint64 at) const
{
    return node.changeAt > at ? node.from : node.step.status;
}

void RunFlow::setSteps(const QString &key, const QList<FlowStep> &steps, bool live, const QString &previousKey)
{
    const qint64 at = clock();
    const bool keep = key == m_key || (!previousKey.isEmpty() && previousKey == m_key);
    const QList<Node> old = keep ? m_nodes : QList<Node>();
    QList<bool> used(old.size(), false);
    QList<Node> next;
    int changed = 0;
    int added = 0;
    for (const FlowStep &step : steps) {
        int match = -1;
        for (int index = 0; index < old.size() && match < 0; ++index)
            if (!used.at(index) && old.at(index).step.name == step.name)
                match = index;
        Node node;
        if (match >= 0) {
            used[match] = true;
            node = old.at(match);
            if (node.step.status != step.status) {
                node.from = statusAt(node, at);
                node.changeAt = at + kStagger * changed++;
            }
            node.step = step;
        } else {
            node.step = step;
            node.from = step.status;
            node.shownAt = at + kReveal * added++;
        }
        next.append(node);
    }
    m_nodes = next;
    m_key = key;
    m_live = live;
    QStringList spoken;
    for (const Node &node : std::as_const(m_nodes))
        spoken << QStringLiteral("%1: %2").arg(node.step.name, localStatusLabel(node.step.status));
    setAccessibleDescription(spoken.join(QStringLiteral(", ")));
    updateGeometry();
    wake();
    update();
}

void RunFlow::clear()
{
    m_nodes.clear();
    m_key.clear();
    m_live = false;
    setAccessibleDescription(QString());
    m_timer.stop();
    updateGeometry();
    update();
}

QList<FlowStep> RunFlow::steps() const
{
    QList<FlowStep> out;
    for (const Node &node : m_nodes)
        out.append(node.step);
    return out;
}

QString RunFlow::shownStatus(int index) const
{
    if (index < 0 || index >= m_nodes.size())
        return {};
    return statusAt(m_nodes.at(index), clock());
}

qreal RunFlow::slotWidth() const
{
    return std::max<qreal>(kSlot, qreal(width()) / std::max<qsizetype>(1, m_nodes.size()));
}

qreal RunFlow::centerX(int index) const
{
    return slotWidth() * index + slotWidth() / 2.0;
}

qreal RunFlow::centerY() const
{
    return std::max(0, (height() - kHeight) / 2) + 24.0;
}

int RunFlow::focusX() const
{
    int focus = -1;
    for (int index = 0; index < m_nodes.size(); ++index) {
        const QString status = m_nodes.at(index).step.status;
        if (status == QLatin1String("running"))
            return int(centerX(index));
        if (doneStatus(status))
            focus = index;
    }
    return focus < 0 ? 0 : int(centerX(focus));
}

QSize RunFlow::sizeHint() const
{
    return {int(std::max<qsizetype>(1, m_nodes.size())) * kSlot, kHeight};
}

QSize RunFlow::minimumSizeHint() const
{
    return sizeHint();
}

bool RunFlow::busy(qint64 at) const
{
    if (!isVisible())
        return false;
    if (m_live)
        return true;
    for (const Node &node : m_nodes) {
        if (node.shownAt > 0 && at < node.shownAt + kAppear)
            return true;
        if (node.changeAt > 0 && at < node.changeAt + std::max(kPop, kFill) + kFrameMs)
            return true;
    }
    return false;
}

void RunFlow::wake()
{
    if (busy(clock()) && !m_timer.isActive())
        m_timer.start();
}

void RunFlow::tick()
{
    update();
    if (!busy(clock()))
        m_timer.stop();
}

void RunFlow::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    wake();
}

bool RunFlow::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int index = int(help->pos().x() / slotWidth());
        if (index < 0 || index >= m_nodes.size()) {
            QToolTip::hideText();
            event->ignore();
            return true;
        }
        const FlowStep &step = m_nodes.at(index).step;
        QStringList lines{step.name, localStatusLabel(step.status)};
        if (!step.note.isEmpty() && step.note != lines.last())
            lines.last() += QStringLiteral(" · ") + step.note;
        if (!step.detail.isEmpty())
            lines << step.detail.left(600);
        QToolTip::showText(help->globalPos(), lines.join(QLatin1Char('\n')), this);
        return true;
    }
    return QWidget::event(event);
}

void RunFlow::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qint64 at = clock();
    const qreal cy = centerY();
    const Tones &tones = m_tones;

    if (m_nodes.isEmpty()) {
        if (!m_live)
            return;
        QFont font = this->font();
        font.setPixelSize(12);
        painter.setFont(font);
        const QFontMetricsF metrics(font);
        const QString label = QStringLiteral("Starting");
        const qreal textWidth = metrics.horizontalAdvance(label);
        const qreal left = (width() - textWidth - 30) / 2.0;
        painter.setPen(tones.secondary);
        painter.drawText(QPointF(left, cy + metrics.ascent() / 2.0 - 1), label);
        painter.setPen(Qt::NoPen);
        for (int dot = 0; dot < 3; ++dot) {
            const qreal phase = std::fmod(at / 700.0 - dot * 0.16 + 10.0, 1.0);
            const qreal lift = std::max(0.0, std::sin(phase * 2 * kPi));
            painter.setBrush(faded(tones.accent, 0.35 + 0.65 * lift));
            painter.drawEllipse(QPointF(left + textWidth + 8 + dot * 8, cy - lift * 4), 2.6, 2.6);
        }
        return;
    }

    const int count = int(m_nodes.size());
    const qreal slot = slotWidth();
    QList<qreal> appear;
    QList<QString> shown;
    bool anyRunning = false;
    for (const Node &node : std::as_const(m_nodes)) {
        appear << (node.shownAt > 0 ? unit(qreal(at - node.shownAt) / kAppear) : 1.0);
        shown << statusAt(node, at);
        anyRunning = anyRunning || shown.last() == QLatin1String("running");
    }
    const QEasingCurve outCubic(QEasingCurve::OutCubic);

    for (int index = 0; index + 1 < count; ++index) {
        const qreal x0 = centerX(index) + kRadius + 6;
        const qreal x1 = centerX(index + 1) - kRadius - 6;
        const qreal seen = std::min(appear.at(index), appear.at(index + 1));
        if (x1 <= x0 || seen <= 0)
            continue;
        painter.setPen(QPen(faded(tones.border, seen), 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(x0, cy), QPointF(x0 + (x1 - x0) * outCubic.valueForProgress(seen), cy));
        const Node &node = m_nodes.at(index);
        const QString status = shown.at(index);
        if (status != QLatin1String("success"))
            continue;
        const qreal fill = node.changeAt > 0 ? unit(qreal(at - node.changeAt) / kFill) : 1.0;
        painter.setPen(QPen(faded(tones.success, seen), 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(x0, cy), QPointF(x0 + (x1 - x0) * outCubic.valueForProgress(fill), cy));
        if (fill >= 1 && shown.at(index + 1) == QLatin1String("running")) {
            const qreal phase = std::fmod(at / 900.0, 1.0);
            const QPointF spark(x0 + (x1 - x0) * phase, cy);
            glow(painter, spark, 7, faded(tones.accent, 0.55 * std::sin(phase * kPi)));
            painter.setBrush(faded(tones.accent, std::sin(phase * kPi)));
            painter.drawEllipse(spark, 2.4, 2.4);
        }
    }

    if (m_live && !anyRunning && count > 1) {
        const qreal from = centerX(0);
        const qreal to = centerX(count - 1);
        const qreal phase = QEasingCurve(QEasingCurve::InOutSine).valueForProgress(std::fmod(at / 1800.0, 1.0));
        const qreal head = from + (to - from) * phase;
        const qreal tail = std::max(from, head - 46);
        QLinearGradient trail(QPointF(tail, cy), QPointF(head, cy));
        trail.setColorAt(0, faded(tones.accent, 0));
        trail.setColorAt(1, faded(tones.accent, 0.85));
        painter.setPen(QPen(QBrush(trail), 2.4, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(tail, cy), QPointF(head, cy));
        glow(painter, QPointF(head, cy), 9, faded(tones.accent, 0.45));
        painter.setBrush(tones.accent);
        painter.drawEllipse(QPointF(head, cy), 3, 3);
    }

    QFont nameFont = font();
    nameFont.setPixelSize(12);
    nameFont.setWeight(QFont::DemiBold);
    QFont noteFont = font();
    noteFont.setPixelSize(11);
    QFont digitFont = font();
    digitFont.setPixelSize(11);
    digitFont.setWeight(QFont::DemiBold);
    const QEasingCurve outBack(QEasingCurve::OutBack);

    for (int index = 0; index < count; ++index) {
        const qreal seen = appear.at(index);
        if (seen <= 0)
            continue;
        const Node &node = m_nodes.at(index);
        const QString status = shown.at(index);
        const QColor tint = statusColor(tones, status);
        const qreal eased = outCubic.valueForProgress(seen);
        const qreal cx = centerX(index);
        const qreal lift = (1 - eased) * 8;
        const bool popping = node.changeAt > 0 && at >= node.changeAt && at < node.changeAt + kPop;
        const qreal pop = popping ? qreal(at - node.changeAt) / kPop : -1;
        qreal scale = 0.55 + 0.45 * outBack.valueForProgress(seen);
        if (pop >= 0)
            scale *= 1 + 0.3 * std::sin(pop * kPi) * (1 - pop * 0.4);

        painter.save();
        painter.setOpacity(eased);
        painter.translate(cx, cy + lift);
        if (pop >= 0 && doneStatus(status)) {
            painter.setPen(QPen(faded(tint, 0.6 * (1 - pop)), 2));
            painter.setBrush(Qt::NoBrush);
            const qreal ring = kRadius + 2 + 16 * QEasingCurve(QEasingCurve::OutQuad).valueForProgress(pop);
            painter.drawEllipse(QPointF(0, 0), ring, ring);
        }
        if (status == QLatin1String("running")) {
            const qreal phase = std::fmod(at / 1200.0, 1.0);
            const qreal halo = kRadius + 3 + 10 * phase;
            painter.setPen(Qt::NoPen);
            painter.setBrush(faded(tones.accent, 0.28 * (1 - phase)));
            painter.drawEllipse(QPointF(0, 0), halo, halo);
        }
        painter.scale(scale, scale);
        if (status == QLatin1String("running")) {
            painter.setPen(QPen(tones.accent, 2));
            painter.setBrush(mix(tones.surface, tones.accent, 0.14));
        } else if (doneStatus(status) && status != QLatin1String("skipped")) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(tint);
        } else if (status == QLatin1String("skipped")) {
            painter.setPen(QPen(tones.strong, 1.5));
            painter.setBrush(tones.sunken);
        } else {
            painter.setPen(QPen(tones.strong, 1.5));
            painter.setBrush(tones.surface);
        }
        painter.drawEllipse(QPointF(0, 0), kRadius, kRadius);
        if (status == QLatin1String("running")) {
            const qreal spin = std::fmod(at * 0.36, 360.0);
            painter.setPen(QPen(tones.accent, 2.6, Qt::SolidLine, Qt::RoundCap));
            painter.setBrush(Qt::NoBrush);
            const qreal arc = kRadius + 4;
            painter.drawArc(QRectF(-arc, -arc, arc * 2, arc * 2), int(-spin * 16), 100 * 16);
        }
        const QColor onFill = QColor(Qt::white);
        if (status == QLatin1String("success")) {
            QPainterPath check;
            check.moveTo(-5, 0.5);
            check.lineTo(-1.5, 4);
            check.lineTo(5.5, -3.5);
            painter.setPen(QPen(onFill, 2.2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(check);
        } else if (status == QLatin1String("error")) {
            painter.setPen(QPen(onFill, 2.2, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(-4, -4), QPointF(4, 4));
            painter.drawLine(QPointF(4, -4), QPointF(-4, 4));
        } else if (status == QLatin1String("stopped")) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(onFill);
            painter.drawRoundedRect(QRectF(-3.5, -3.5, 7, 7), 1.5, 1.5);
        } else if (status == QLatin1String("skipped")) {
            painter.setPen(QPen(tones.muted, 2, Qt::SolidLine, Qt::RoundCap));
            painter.drawLine(QPointF(-4.5, 0), QPointF(4.5, 0));
        } else {
            painter.setFont(digitFont);
            painter.setPen(status == QLatin1String("running") ? tones.accent : tones.secondary);
            painter.drawText(QRectF(-kRadius, -kRadius, kRadius * 2, kRadius * 2), Qt::AlignCenter,
                             QString::number(index + 1));
        }
        painter.restore();

        painter.save();
        painter.setOpacity(eased);
        const QRectF nameBox(cx - slot / 2 + 4, cy + lift + kRadius + 7, slot - 8, 16);
        painter.setFont(nameFont);
        painter.setPen(status == QLatin1String("running") ? tones.accent
                       : doneStatus(status)               ? tones.ink
                                                          : tones.secondary);
        painter.drawText(nameBox, Qt::AlignHCenter | Qt::AlignVCenter,
                         QFontMetricsF(nameFont).elidedText(node.step.name, Qt::ElideRight, nameBox.width()));
        const QString note = node.step.note.isEmpty() || status != node.step.status ? localStatusLabel(status)
                                                                                      : node.step.note;
        painter.setFont(noteFont);
        painter.setPen(status == QLatin1String("error") ? tones.danger : tones.muted);
        const QRectF noteBox = nameBox.translated(0, 15);
        painter.drawText(noteBox, Qt::AlignHCenter | Qt::AlignVCenter,
                         QFontMetricsF(noteFont).elidedText(note, Qt::ElideRight, noteBox.width()));
        painter.restore();
    }
}

RunSparkle *RunSparkle::burst(QTableWidget *table, const QString &runId, const Tones &tones)
{
    if (!table || !table->isVisible() || runId.isEmpty())
        return nullptr;
    auto *sparkle = new RunSparkle(table, runId, tones);
    sparkle->show();
    sparkle->raise();
    return sparkle;
}

RunSparkle::RunSparkle(QTableWidget *table, const QString &runId, const Tones &tones)
    : QWidget(table->viewport()), m_table(table), m_runId(runId), m_tones(tones)
{
    setObjectName(QStringLiteral("workflowsRunSparkle"));
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    setGeometry(table->viewport()->rect());
    const QColor gold = tones.dark ? QColor(QStringLiteral("#e3b341")) : QColor(QStringLiteral("#d4a72c"));
    const QList<QColor> colors{gold, QColor(Qt::white), mix(tones.accent, QColor(Qt::white), 0.35),
                               mix(tones.agent, QColor(Qt::white), 0.2)};
    QRandomGenerator *random = QRandomGenerator::global();
    for (int index = 0; index < kSparks; ++index) {
        Spark spark;
        const bool lead = index < kSparks * 3 / 5;
        spark.at = QPointF(lead ? random->bounded(0.4) : 0.4 + random->bounded(0.6), -0.3 + random->bounded(1.6));
        spark.size = 5.0 + random->bounded(6.0);
        spark.delay = random->bounded(0.42);
        spark.spin = random->bounded(2.0) - 1.0;
        spark.color = colors.at(index % colors.size());
        m_sparks.append(spark);
    }
    m_clock.start();
    m_timer.setInterval(16);
    QObject::connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_timer.start();
}

QRectF RunSparkle::rowRect()
{
    if (!m_table)
        return m_last;
    for (int row = 0; row < m_table->rowCount(); ++row) {
        const QTableWidgetItem *item = m_table->item(row, 0);
        if (item && item->data(Qt::UserRole + 1).toString() == m_runId) {
            m_last = QRectF(0, m_table->rowViewportPosition(row), m_table->viewport()->width(),
                            m_table->rowHeight(row));
            return m_last;
        }
    }
    return m_last;
}

void RunSparkle::tick()
{
    if (!m_table || m_clock.elapsed() >= kSparkleMs) {
        m_timer.stop();
        deleteLater();
        return;
    }
    if (geometry() != m_table->viewport()->rect())
        setGeometry(m_table->viewport()->rect());
    const QRectF row = rowRect();
    const QRect area = row.adjusted(-12, -row.height(), 12, row.height()).toAlignedRect();
    update(area.united(m_painted));
    m_painted = area;
}

void RunSparkle::paintEvent(QPaintEvent *)
{
    const QRectF row = rowRect();
    if (row.isEmpty())
        return;
    const qreal t = unit(qreal(m_clock.elapsed()) / kSparkleMs);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    const QColor accent = m_tones.accent;

    const QColor flash(Qt::white);
    painter.fillRect(row, faded(accent, 0.22 * std::pow(1 - t, 2)));
    if (t < 0.3)
        painter.fillRect(row, faded(flash, 0.55 * (1 - t / 0.3)));
    if (t < 0.7) {
        const qreal sweep = QEasingCurve(QEasingCurve::OutCubic).valueForProgress(t / 0.7);
        const qreal band = row.width() * 0.3;
        const qreal centre = row.left() - band + (row.width() + band * 2) * sweep;
        QLinearGradient shine(QPointF(centre - band / 2, 0), QPointF(centre + band / 2, 0));
        shine.setColorAt(0, faded(flash, 0));
        shine.setColorAt(0.5, faded(flash, 0.5 * (1 - t / 0.7)));
        shine.setColorAt(1, faded(flash, 0));
        painter.fillRect(row, shine);
    }
    painter.fillRect(QRectF(row.left(), row.top(), 3, row.height()), faded(accent, 1 - t));

    for (const Spark &spark : std::as_const(m_sparks)) {
        const qreal life = (t - spark.delay) / 0.55;
        if (life <= 0 || life >= 1)
            continue;
        const qreal grow = std::sin(life * kPi);
        const QPointF at(row.left() + spark.at.x() * row.width(), row.top() + spark.at.y() * row.height());
        glow(painter, at, spark.size * 1.9 * grow, faded(spark.color, 0.3 * grow));
        painter.save();
        painter.translate(at);
        painter.rotate(spark.spin * life * 90);
        painter.setPen(Qt::NoPen);
        painter.setBrush(faded(spark.color, 0.95 * grow));
        painter.drawPath(starPath(spark.size * grow));
        painter.restore();
    }
}

} // namespace forkmesh::workflows
