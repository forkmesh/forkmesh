#include "ReticulumGraphics.h"

#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace forkmesh::reticulum {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kPulseMs = 1600;

double hashFraction(const QByteArray &hash, int offset)
{
    quint32 value = 0;
    for (int i = 0; i < 4 && offset + i < hash.size(); ++i)
        value = (value << 8) | quint8(hash.at(offset + i));
    return double(value) / 4294967296.0;
}

double niceCeiling(double value)
{
    if (value <= 0)
        return 1;
    const double exponent = std::pow(10.0, std::floor(std::log10(value)));
    const double fraction = value / exponent;
    const double nice = fraction <= 1 ? 1 : fraction <= 2 ? 2 : fraction <= 5 ? 5 : 10;
    return nice * exponent;
}

QFont scaledFont(const QWidget *widget, double points, bool bold = false)
{
    QFont font = widget->font();
    font.setPointSizeF(points);
    font.setBold(bold);
    return font;
}

}

QColor ChartPalette::series(int slot) const
{
    static const char *darkSlots[] = {"#3987e5", "#d95926", "#199e70", "#c98500", "#d55181", "#008300"};
    static const char *lightSlots[] = {"#2a78d6", "#eb6834", "#1baf7a", "#eda100", "#e87ba4", "#008300"};
    const int index = qBound(0, slot, 5);
    return QColor(QString::fromLatin1(dark ? darkSlots[index] : lightSlots[index]));
}

QColor ChartPalette::group(MapGroup value) const
{
    switch (value) {
    case MapGroup::Peer:
        return series(0);
    case MapGroup::Node:
        return series(1);
    case MapGroup::Propagation:
        return series(2);
    case MapGroup::Transport:
        return series(3);
    case MapGroup::Other:
        break;
    }
    return QColor(dark ? "#6e7681" : "#8c959f");
}

ChartPalette ChartPalette::forWidget(const QWidget *widget)
{
    ChartPalette palette;
    const QPalette source = widget->palette();
    palette.surface = source.color(QPalette::Window);
    palette.dark = palette.surface.lightness() < 128;
    palette.text = source.color(QPalette::WindowText);
    palette.raised = QColor(palette.dark ? "#161b22" : "#ffffff");
    palette.border = QColor(palette.dark ? "#30363d" : "#d0d7de");
    palette.grid = QColor(palette.dark ? "#262c36" : "#d8dee4");
    palette.muted = QColor(palette.dark ? "#8b949e" : "#656d76");
    palette.accent = QColor(palette.dark ? "#58a6ff" : "#0969da");
    palette.good = QColor(palette.dark ? "#3fb950" : "#1a7f37");
    palette.warning = QColor(palette.dark ? "#d29922" : "#9a6700");
    palette.critical = QColor(palette.dark ? "#f85149" : "#cf222e");
    return palette;
}

MapGroup mapGroup(const QString &kind)
{
    if (kind == QStringLiteral("peer") || kind == QStringLiteral("forkmesh"))
        return MapGroup::Peer;
    if (kind == QStringLiteral("node") || kind == QStringLiteral("service"))
        return MapGroup::Node;
    if (kind == QStringLiteral("propagation"))
        return MapGroup::Propagation;
    if (kind == QStringLiteral("transport") || kind == QStringLiteral("discovery"))
        return MapGroup::Transport;
    return MapGroup::Other;
}

QString mapGroupLabel(MapGroup group)
{
    switch (group) {
    case MapGroup::Peer:
        return QStringLiteral("People");
    case MapGroup::Node:
        return QStringLiteral("Nodes and services");
    case MapGroup::Propagation:
        return QStringLiteral("Propagation nodes");
    case MapGroup::Transport:
        return QStringLiteral("Transport");
    case MapGroup::Other:
        break;
    }
    return QStringLiteral("Other");
}

QString formatBytes(double bytes)
{
    static const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    int unit = 0;
    while (bytes >= 1000 && unit < 4) {
        bytes /= 1000;
        ++unit;
    }
    return unit == 0 ? QStringLiteral("%1 B").arg(qint64(bytes))
                     : QStringLiteral("%1 %2").arg(bytes, 0, 'f', bytes < 10 ? 1 : 0).arg(QLatin1String(units[unit]));
}

QString formatRate(double bytesPerSecond)
{
    return formatBytes(bytesPerSecond) + QStringLiteral("/s");
}

QString formatDuration(qint64 seconds)
{
    if (seconds < 60)
        return QStringLiteral("%1 s").arg(seconds);
    if (seconds < 3600)
        return QStringLiteral("%1 min").arg(seconds / 60);
    if (seconds < 86400)
        return QStringLiteral("%1 h %2 min").arg(seconds / 3600).arg((seconds % 3600) / 60);
    return QStringLiteral("%1 d %2 h").arg(seconds / 86400).arg((seconds % 86400) / 3600);
}

QString formatAge(const QDateTime &when, const QDateTime &now)
{
    if (!when.isValid())
        return QStringLiteral("never");
    const qint64 seconds = qMax<qint64>(0, when.secsTo(now));
    if (seconds < 5)
        return QStringLiteral("just now");
    return formatDuration(seconds) + QStringLiteral(" ago");
}

void paintGroupMarker(QPainter *painter, MapGroup group, const QPointF &center, double radius,
                      const QColor &fill, const QColor &ring)
{
    painter->setPen(QPen(ring, 2));
    painter->setBrush(fill);
    switch (group) {
    case MapGroup::Peer:
        painter->drawEllipse(center, radius, radius);
        break;
    case MapGroup::Node:
        painter->drawRoundedRect(QRectF(center.x() - radius * 0.9, center.y() - radius * 0.9, radius * 1.8,
                                        radius * 1.8),
                                 1.5, 1.5);
        break;
    case MapGroup::Propagation: {
        QPolygonF diamond;
        diamond << QPointF(center.x(), center.y() - radius * 1.2) << QPointF(center.x() + radius * 1.2, center.y())
                << QPointF(center.x(), center.y() + radius * 1.2) << QPointF(center.x() - radius * 1.2, center.y());
        painter->drawPolygon(diamond);
        break;
    }
    case MapGroup::Transport: {
        QPolygonF triangle;
        triangle << QPointF(center.x(), center.y() - radius * 1.25)
                 << QPointF(center.x() + radius * 1.15, center.y() + radius * 0.85)
                 << QPointF(center.x() - radius * 1.15, center.y() + radius * 0.85);
        painter->drawPolygon(triangle);
        break;
    }
    case MapGroup::Other:
        painter->drawEllipse(center, radius * 0.75, radius * 0.75);
        break;
    }
}

StatTile::StatTile(const QString &label, QWidget *parent)
    : QWidget(parent)
    , m_label(label)
{
    setAccessibleName(label);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void StatTile::setValue(const QString &value, const QString &detail)
{
    if (value == m_value && detail == m_detail)
        return;
    m_value = value;
    m_detail = detail;
    setAccessibleDescription(detail.isEmpty() ? value : value + QStringLiteral(", ") + detail);
    setToolTip(detail);
    update();
}

void StatTile::setTone(int tone)
{
    if (tone == m_tone)
        return;
    m_tone = tone;
    update();
}

void StatTile::setSpark(const QVector<double> &values)
{
    m_spark = values;
    update();
}

QSize StatTile::sizeHint() const
{
    return {170, 84};
}

QSize StatTile::minimumSizeHint() const
{
    return {120, 84};
}

void StatTile::paintEvent(QPaintEvent *)
{
    const ChartPalette colors = ChartPalette::forWidget(this);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(colors.border, 1));
    painter.setBrush(colors.raised);
    painter.drawRoundedRect(box, 8, 8);

    const QColor toneColor = m_tone == 1   ? colors.good
                             : m_tone == 2 ? colors.warning
                             : m_tone == 3 ? colors.critical
                                           : colors.muted;
    double x = 14;
    if (m_tone != 0) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(toneColor);
        painter.drawEllipse(QPointF(x + 4, 19), 4, 4);
        x += 14;
    }
    painter.setFont(scaledFont(this, 8.5));
    painter.setPen(colors.muted);
    painter.drawText(QRectF(x, 10, width() - x - 10, 18), Qt::AlignLeft | Qt::AlignVCenter, m_label);

    if (m_spark.size() > 1) {
        const double maximum = std::max(1e-9, *std::max_element(m_spark.cbegin(), m_spark.cend()));
        const QRectF area(width() * 0.52, 34, width() * 0.48 - 12, 30);
        QPainterPath path;
        for (int i = 0; i < m_spark.size(); ++i) {
            const QPointF point(area.left() + area.width() * i / (m_spark.size() - 1),
                                area.bottom() - area.height() * m_spark.at(i) / maximum);
            if (i == 0)
                path.moveTo(point);
            else
                path.lineTo(point);
        }
        QPainterPath fill = path;
        fill.lineTo(area.bottomRight());
        fill.lineTo(area.bottomLeft());
        fill.closeSubpath();
        QColor soft = colors.series(0);
        soft.setAlphaF(0.16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(soft);
        painter.drawPath(fill);
        painter.setPen(QPen(colors.series(0), 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        painter.setBrush(Qt::NoBrush);
        painter.drawPath(path);
    }

    painter.setFont(scaledFont(this, 15, true));
    painter.setPen(colors.text);
    const double valueWidth = m_spark.size() > 1 ? width() * 0.5 - 14 : width() - 28;
    painter.drawText(QRectF(14, 30, valueWidth, 28), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetricsF(painter.font()).elidedText(m_value, Qt::ElideRight, valueWidth));
    painter.setFont(scaledFont(this, 8.5));
    painter.setPen(colors.muted);
    painter.drawText(QRectF(14, 58, width() - 28, 18), Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetricsF(painter.font()).elidedText(m_detail, Qt::ElideRight, width() - 28));
}

TrafficChart::TrafficChart(QWidget *parent)
    : QWidget(parent)
    , m_title(QStringLiteral("Traffic, last 3 minutes"))
{
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("Traffic chart"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
}

void TrafficChart::setSamples(const QVector<TrafficSample> &samples)
{
    m_samples = samples;
    qint64 rx = 0;
    qint64 tx = 0;
    for (const TrafficSample &sample : samples) {
        rx += sample.rx;
        tx += sample.tx;
    }
    setAccessibleDescription(QStringLiteral("Received %1 and sent %2 in the last %3 seconds")
                                 .arg(formatBytes(double(rx)), formatBytes(double(tx)))
                                 .arg(samples.size()));
    update();
}

void TrafficChart::setTitle(const QString &title)
{
    m_title = title;
    update();
}

QSize TrafficChart::sizeHint() const
{
    return {420, 190};
}

QSize TrafficChart::minimumSizeHint() const
{
    return {240, 150};
}

QRectF TrafficChart::plotRect() const
{
    return QRectF(62, 34, qMax(10, width() - 62 - 14), qMax(10, height() - 34 - 26));
}

void TrafficChart::paintEvent(QPaintEvent *)
{
    const ChartPalette colors = ChartPalette::forWidget(this);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = plotRect();
    const int slotCount = Node::kHistorySeconds;

    double maximum = 0;
    for (const TrafficSample &sample : m_samples)
        maximum = std::max({maximum, double(sample.rx), double(sample.tx)});
    const double top = niceCeiling(std::max(maximum * 1.1, 512.0));

    painter.setFont(scaledFont(this, 9.5, true));
    painter.setPen(colors.text);
    painter.drawText(QRectF(0, 4, width() * 0.55, 22), Qt::AlignLeft | Qt::AlignVCenter, m_title);

    painter.setFont(scaledFont(this, 8.5));
    const QList<QPair<QString, QColor>> legend = {{QStringLiteral("Received"), colors.series(0)},
                                                  {QStringLiteral("Sent"), colors.series(1)}};
    double legendX = width() - 14;
    for (int i = legend.size() - 1; i >= 0; --i) {
        const double textWidth = QFontMetricsF(painter.font()).horizontalAdvance(legend.at(i).first);
        legendX -= textWidth;
        painter.setPen(colors.muted);
        painter.drawText(QRectF(legendX, 4, textWidth, 22), Qt::AlignLeft | Qt::AlignVCenter, legend.at(i).first);
        legendX -= 18;
        painter.setPen(QPen(legend.at(i).second, 2, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(QPointF(legendX, 15), QPointF(legendX + 12, 15));
        legendX -= 14;
    }

    painter.setPen(colors.muted);
    for (int step = 0; step <= 4; ++step) {
        const double value = top * step / 4;
        const double y = plot.bottom() - plot.height() * step / 4;
        painter.setPen(QPen(step == 0 ? colors.border : colors.grid, 1));
        painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        painter.setPen(colors.muted);
        painter.drawText(QRectF(0, y - 9, plot.left() - 8, 18), Qt::AlignRight | Qt::AlignVCenter,
                         step == 0 ? QStringLiteral("0") : formatRate(value));
    }
    const QStringList marks = {QStringLiteral("3 min ago"), QStringLiteral("2 min"), QStringLiteral("1 min"),
                               QStringLiteral("now")};
    for (int i = 0; i < marks.size(); ++i) {
        const double x = plot.left() + plot.width() * i / 3;
        const Qt::Alignment align = i == 0 ? Qt::AlignLeft : i == 3 ? Qt::AlignRight : Qt::AlignHCenter;
        const QRectF label(i == 0 ? x : i == 3 ? x - 80 : x - 40, plot.bottom() + 4, 80, 18);
        painter.drawText(label, align | Qt::AlignVCenter, marks.at(i));
    }

    const int offset = slotCount - int(m_samples.size());
    const auto pointFor = [&](int index, double value) {
        const double x = plot.left() + plot.width() * (offset + index) / double(slotCount - 1);
        return QPointF(x, plot.bottom() - plot.height() * value / top);
    };
    if (m_samples.size() >= 2) {
        for (int series = 1; series >= 0; --series) {
            QPainterPath line;
            for (int i = 0; i < m_samples.size(); ++i) {
                const double value = double(series == 0 ? m_samples.at(i).rx : m_samples.at(i).tx);
                const QPointF point = pointFor(i, value);
                if (i == 0)
                    line.moveTo(point);
                else
                    line.lineTo(point);
            }
            QPainterPath area = line;
            area.lineTo(pointFor(int(m_samples.size()) - 1, 0));
            area.lineTo(pointFor(0, 0));
            area.closeSubpath();
            QColor soft = colors.series(series);
            soft.setAlphaF(0.12);
            painter.setPen(Qt::NoPen);
            painter.setBrush(soft);
            painter.drawPath(area);
            painter.setPen(QPen(colors.series(series), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            painter.setBrush(Qt::NoBrush);
            painter.drawPath(line);
        }
    } else {
        painter.setPen(colors.muted);
        painter.drawText(plot, Qt::AlignCenter, QStringLiteral("Traffic shows here a few seconds after an interface comes online"));
    }

    if (m_hover >= 0 && m_hover < m_samples.size()) {
        const TrafficSample &sample = m_samples.at(m_hover);
        const QPointF rx = pointFor(m_hover, double(sample.rx));
        const QPointF tx = pointFor(m_hover, double(sample.tx));
        painter.setPen(QPen(colors.muted, 1, Qt::DashLine));
        painter.drawLine(QPointF(rx.x(), plot.top()), QPointF(rx.x(), plot.bottom()));
        for (const auto &[point, color] : {std::pair{rx, colors.series(0)}, std::pair{tx, colors.series(1)}}) {
            painter.setPen(QPen(colors.surface, 2));
            painter.setBrush(color);
            painter.drawEllipse(point, 4.5, 4.5);
        }
        const int secondsAgo = int(m_samples.size()) - 1 - m_hover;
        const QStringList lines = {secondsAgo == 0 ? QStringLiteral("Now") : QStringLiteral("%1 s ago").arg(secondsAgo),
                                   QStringLiteral("Received %1").arg(formatRate(double(sample.rx))),
                                   QStringLiteral("Sent %1").arg(formatRate(double(sample.tx))),
                                   QStringLiteral("%1 announce%2").arg(sample.announces).arg(sample.announces == 1 ? QString() : QStringLiteral("s"))};
        painter.setFont(scaledFont(this, 8.5));
        const QFontMetricsF metrics(painter.font());
        double boxWidth = 0;
        for (const QString &line : lines)
            boxWidth = std::max(boxWidth, metrics.horizontalAdvance(line));
        boxWidth += 20;
        const double boxHeight = lines.size() * 16 + 12;
        double boxX = rx.x() + 12;
        if (boxX + boxWidth > width() - 4)
            boxX = rx.x() - 12 - boxWidth;
        const QRectF box(boxX, plot.top() + 4, boxWidth, boxHeight);
        painter.setPen(QPen(colors.border, 1));
        painter.setBrush(colors.raised);
        painter.drawRoundedRect(box, 6, 6);
        for (int i = 0; i < lines.size(); ++i) {
            painter.setPen(i == 0 ? colors.muted : colors.text);
            painter.drawText(QRectF(box.left() + 10, box.top() + 6 + i * 16, boxWidth - 20, 16),
                             Qt::AlignLeft | Qt::AlignVCenter, lines.at(i));
        }
    }
}

void TrafficChart::mouseMoveEvent(QMouseEvent *event)
{
    const QRectF plot = plotRect().adjusted(-8, -8, 8, 8);
    int hover = -1;
    if (plot.contains(event->position()) && !m_samples.isEmpty()) {
        const int slotCount = Node::kHistorySeconds;
        const int offset = slotCount - int(m_samples.size());
        const double slot = (event->position().x() - plotRect().left()) / plotRect().width() * (slotCount - 1);
        hover = qBound(0, int(std::lround(slot)) - offset, int(m_samples.size()) - 1);
    }
    if (hover != m_hover) {
        m_hover = hover;
        update();
    }
}

void TrafficChart::leaveEvent(QEvent *)
{
    m_hover = -1;
    update();
}

NetworkMap::NetworkMap(QWidget *parent)
    : QWidget(parent)
    , m_animation(new QTimer(this))
{
    setMouseTracking(true);
    setAccessibleName(QStringLiteral("Reticulum network map"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_animation->setInterval(33);
    connect(m_animation, &QTimer::timeout, this, [this] {
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto it = m_pulses.begin(); it != m_pulses.end();) {
            if (now - it.value() > kPulseMs)
                it = m_pulses.erase(it);
            else
                ++it;
        }
        if (m_pulses.isEmpty() || !isVisible())
            m_animation->stop();
        update();
    });
}

NetworkMap::~NetworkMap() = default;

QSize NetworkMap::sizeHint() const
{
    return {520, 420};
}

QSize NetworkMap::minimumSizeHint() const
{
    return {260, 240};
}

void NetworkMap::setData(const QList<Spoke> &spokes, const QList<DestinationRecord> &destinations,
                         const QByteArray &self, bool selfOnline)
{
    m_spokes = spokes;
    m_destinations = destinations;
    m_self = self;
    m_selfOnline = selfOnline;
    int maximum = 2;
    for (const DestinationRecord &record : destinations)
        maximum = std::max(maximum, record.hops);
    m_maxRing = std::min(maximum, 8);
    layoutNodes();
    setAccessibleDescription(QStringLiteral("%1 destinations on %2 interfaces").arg(destinations.size()).arg(spokes.size()));
    update();
}

void NetworkMap::setSelected(const QByteArray &hash)
{
    m_selected = hash;
    update();
}

void NetworkMap::setFilter(const QString &text)
{
    m_filter = text.trimmed();
    update();
}

void NetworkMap::pulse(const QByteArray &hash)
{
    m_pulses.insert(hash, QDateTime::currentMSecsSinceEpoch());
    ensureAnimation();
}

void NetworkMap::ensureAnimation()
{
    if (isVisible() && !m_animation->isActive())
        m_animation->start();
}

QPointF NetworkMap::center() const
{
    return QPointF(width() / 2.0, height() / 2.0 - 6);
}

double NetworkMap::ringRadius(int hops) const
{
    const double outer = std::max(40.0, std::min(width(), height()) / 2.0 - 44);
    const double inner = outer * 0.24;
    const int ring = qBound(1, hops, m_maxRing);
    if (m_maxRing <= 1)
        return outer;
    return inner + (outer - inner) * (ring - 1) / double(m_maxRing - 1);
}

void NetworkMap::resizeEvent(QResizeEvent *)
{
    layoutNodes();
}

void NetworkMap::layoutNodes()
{
    m_positions.clear();
    m_sectors.clear();
    QHash<QString, int> counts;
    QSet<QString> known;
    for (const Spoke &spoke : m_spokes)
        known.insert(spoke.id);
    for (const DestinationRecord &record : m_destinations)
        ++counts[known.contains(record.interfaceId) ? record.interfaceId : QString()];
    QStringList order;
    for (const Spoke &spoke : m_spokes)
        order.append(spoke.id);
    if (counts.contains(QString()))
        order.append(QString());
    if (order.isEmpty())
        return;
    double total = 0;
    for (const QString &id : order)
        total += std::max(1, counts.value(id)) + 2;
    double angle = -kPi / 2;
    for (const QString &id : order) {
        const double span = 2 * kPi * (std::max(1, counts.value(id)) + 2) / total;
        m_sectors.insert(id, {angle, span});
        angle += span;
    }
    const QPointF middle = center();
    const double ringGap = m_maxRing > 1 ? ringRadius(2) - ringRadius(1) : 30;
    for (const DestinationRecord &record : m_destinations) {
        const QString id = known.contains(record.interfaceId) ? record.interfaceId : QString();
        const auto sector = m_sectors.value(id);
        const double margin = std::min(0.12, sector.second * 0.15);
        const double theta = sector.first + margin + (sector.second - 2 * margin) * hashFraction(record.hash, 0);
        const double jitter = (hashFraction(record.hash, 4) - 0.5) * ringGap * 0.5;
        const double radius = ringRadius(record.hops) + jitter;
        m_positions.insert(record.hash, middle + QPointF(std::cos(theta) * radius, std::sin(theta) * radius));
    }
}

QByteArray NetworkMap::hashAt(const QPointF &point) const
{
    QByteArray best;
    double bestDistance = 12 * 12;
    for (auto it = m_positions.cbegin(); it != m_positions.cend(); ++it) {
        const QPointF delta = it.value() - point;
        const double distance = delta.x() * delta.x() + delta.y() * delta.y();
        if (distance < bestDistance) {
            bestDistance = distance;
            best = it.key();
        }
    }
    return best;
}

void NetworkMap::paintEvent(QPaintEvent *)
{
    const ChartPalette colors = ChartPalette::forWidget(this);
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QPointF middle = center();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();

    painter.setFont(scaledFont(this, 8));
    for (int ring = 1; ring <= m_maxRing; ++ring) {
        const double radius = ringRadius(ring);
        painter.setPen(QPen(colors.grid, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(middle, radius, radius);
        painter.setPen(colors.muted);
        const QString label = ring == m_maxRing && m_maxRing == 8 ? QStringLiteral("8+ hops")
                              : ring == 1                         ? QStringLiteral("1 hop")
                                                                  : QStringLiteral("%1 hops").arg(ring);
        painter.drawText(QRectF(middle.x() + 4, middle.y() - radius - 14, 80, 14), Qt::AlignLeft | Qt::AlignBottom, label);
    }

    const double outer = ringRadius(m_maxRing);
    QHash<QString, Spoke> spokeById;
    for (const Spoke &spoke : m_spokes)
        spokeById.insert(spoke.id, spoke);
    for (auto it = m_sectors.cbegin(); it != m_sectors.cend(); ++it) {
        const double start = it.value().first;
        const double mid = start + it.value().second / 2;
        if (m_sectors.size() > 1) {
            painter.setPen(QPen(colors.grid, 1, Qt::DashLine));
            painter.drawLine(middle + QPointF(std::cos(start), std::sin(start)) * 14,
                             middle + QPointF(std::cos(start), std::sin(start)) * (outer + 8));
        }
        const Spoke spoke = spokeById.value(it.key());
        const bool online = it.key().isEmpty() ? false : spoke.online;
        QColor trunk = online ? colors.accent : colors.border;
        painter.setPen(QPen(trunk, online ? 2.5 : 1.5, Qt::SolidLine, Qt::RoundCap));
        painter.drawLine(middle + QPointF(std::cos(mid), std::sin(mid)) * 14,
                         middle + QPointF(std::cos(mid), std::sin(mid)) * ringRadius(1) * 0.92);
        const QString name = it.key().isEmpty() ? QStringLiteral("Earlier interfaces") : spoke.name;
        const QPointF anchor = middle + QPointF(std::cos(mid), std::sin(mid)) * (outer + 18);
        painter.setFont(scaledFont(this, 8.5, true));
        const QFontMetricsF metrics(painter.font());
        const QString text = metrics.elidedText(name, Qt::ElideRight, 150);
        const double textWidth = metrics.horizontalAdvance(text) + 12;
        double x = anchor.x() - textWidth / 2;
        if (std::cos(mid) > 0.35)
            x = anchor.x() - 6;
        else if (std::cos(mid) < -0.35)
            x = anchor.x() - textWidth + 6;
        x = qBound(2.0, x, width() - textWidth - 2.0);
        const double y = qBound(2.0, anchor.y() - 9, height() - 20.0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(online ? colors.good : colors.muted);
        painter.drawEllipse(QPointF(x + 4, y + 9), 3, 3);
        painter.setPen(online ? colors.text : colors.muted);
        painter.drawText(QRectF(x + 10, y, textWidth, 18), Qt::AlignLeft | Qt::AlignVCenter, text);
    }

    const auto matches = [this](const DestinationRecord &record) {
        if (m_filter.isEmpty())
            return true;
        return record.displayName.contains(m_filter, Qt::CaseInsensitive) ||
               record.label.contains(m_filter, Qt::CaseInsensitive) ||
               hexHash(record.hash).contains(m_filter, Qt::CaseInsensitive);
    };

    if (!m_selected.isEmpty() && m_positions.contains(m_selected)) {
        painter.setPen(QPen(colors.accent, 1.5, Qt::DashLine));
        painter.drawLine(middle, m_positions.value(m_selected));
    }

    for (const DestinationRecord &record : m_destinations) {
        const QPointF point = m_positions.value(record.hash);
        const MapGroup group = mapGroup(record.kind);
        QColor fill = colors.group(group);
        const qint64 age = record.lastHeard.secsTo(now);
        double alpha = age < 600 ? 1.0 : age < 3600 ? 0.75 : 0.45;
        if (!matches(record))
            alpha = 0.12;
        fill.setAlphaF(alpha);
        const bool emphasised = record.hash == m_selected || record.hash == m_hover;
        paintGroupMarker(&painter, group, point, emphasised ? 6.5 : 4.5, fill, colors.surface);
        if (emphasised) {
            painter.setPen(QPen(colors.accent, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(point, 10, 10);
        }
        if (m_pulses.contains(record.hash)) {
            const double progress = double(nowMs - m_pulses.value(record.hash)) / kPulseMs;
            if (progress >= 0 && progress <= 1) {
                QColor ripple = colors.group(group);
                ripple.setAlphaF(0.8 * (1 - progress));
                painter.setPen(QPen(ripple, 2));
                painter.setBrush(Qt::NoBrush);
                painter.drawEllipse(point, 5 + 22 * progress, 5 + 22 * progress);
            }
        }
    }

    if (!m_selected.isEmpty() && m_positions.contains(m_selected)) {
        for (const DestinationRecord &record : m_destinations) {
            if (record.hash != m_selected)
                continue;
            const QPointF point = m_positions.value(record.hash);
            painter.setFont(scaledFont(this, 8.5, true));
            const QString label = record.displayName.isEmpty() ? prettyHash(record.hash).left(12) + QStringLiteral("…>")
                                                               : record.displayName;
            const QFontMetricsF metrics(painter.font());
            const double textWidth = metrics.horizontalAdvance(label) + 14;
            QRectF box(point.x() + 12, point.y() - 11, textWidth, 22);
            if (box.right() > width() - 2)
                box.moveRight(point.x() - 12);
            painter.setPen(QPen(colors.border, 1));
            painter.setBrush(colors.raised);
            painter.drawRoundedRect(box, 5, 5);
            painter.setPen(colors.text);
            painter.drawText(box, Qt::AlignCenter, label);
        }
    }

    painter.setPen(QPen(colors.surface, 3));
    painter.setBrush(m_selfOnline ? colors.accent : colors.muted);
    painter.drawEllipse(middle, 11, 11);
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.surface);
    painter.drawEllipse(middle, 4, 4);
    painter.setFont(scaledFont(this, 8.5, true));
    painter.setPen(colors.text);
    painter.drawText(QRectF(middle.x() - 70, middle.y() + 13, 140, 16), Qt::AlignHCenter | Qt::AlignTop,
                     QStringLiteral("This desktop"));

    QHash<int, int> groupCounts;
    for (const DestinationRecord &record : m_destinations)
        ++groupCounts[int(mapGroup(record.kind))];
    painter.setFont(scaledFont(this, 8));
    double legendY = height() - 18;
    double legendX = 8;
    for (MapGroup group : {MapGroup::Peer, MapGroup::Node, MapGroup::Propagation, MapGroup::Transport, MapGroup::Other}) {
        const QString text = QStringLiteral("%1 %2").arg(mapGroupLabel(group)).arg(groupCounts.value(int(group)));
        const double textWidth = QFontMetricsF(painter.font()).horizontalAdvance(text);
        if (legendX + textWidth + 24 > width()) {
            legendX = 8;
            legendY -= 16;
        }
        paintGroupMarker(&painter, group, QPointF(legendX + 5, legendY + 7), 4, colors.group(group), colors.surface);
        painter.setPen(colors.muted);
        painter.drawText(QRectF(legendX + 14, legendY, textWidth + 4, 14), Qt::AlignLeft | Qt::AlignVCenter, text);
        legendX += textWidth + 28;
    }

    if (m_destinations.isEmpty()) {
        painter.setFont(scaledFont(this, 9.5));
        painter.setPen(colors.muted);
        painter.drawText(QRectF(middle.x() - 170, middle.y() + 34, 340, 40), Qt::AlignCenter | Qt::TextWordWrap,
                         m_spokes.isEmpty() ? QStringLiteral("Add an interface to join a Reticulum network")
                                            : QStringLiteral("Destinations appear here as their announces arrive"));
    }
}

void NetworkMap::mouseMoveEvent(QMouseEvent *event)
{
    const QByteArray hash = hashAt(event->position());
    if (hash != m_hover) {
        m_hover = hash;
        update();
    }
    if (hash.isEmpty()) {
        QToolTip::hideText();
        setCursor(Qt::ArrowCursor);
        return;
    }
    setCursor(Qt::PointingHandCursor);
    for (const DestinationRecord &record : std::as_const(m_destinations)) {
        if (record.hash != hash)
            continue;
        const QString name = record.displayName.isEmpty() ? QStringLiteral("Unnamed") : record.displayName.toHtmlEscaped();
        QString via;
        for (const Spoke &spoke : std::as_const(m_spokes)) {
            if (spoke.id == record.interfaceId)
                via = spoke.name.toHtmlEscaped();
        }
        QToolTip::showText(event->globalPosition().toPoint(),
                           QStringLiteral("<b>%1</b><br>%2<br>%3 hop%4%5<br>Heard %6<br><code>%7</code>")
                               .arg(name, record.label.toHtmlEscaped())
                               .arg(record.hops)
                               .arg(record.hops == 1 ? QString() : QStringLiteral("s"))
                               .arg(via.isEmpty() ? QString() : QStringLiteral(" via ") + via)
                               .arg(formatAge(record.lastHeard), hexHash(record.hash)),
                           this);
        break;
    }
}

void NetworkMap::mousePressEvent(QMouseEvent *event)
{
    const QByteArray hash = hashAt(event->position());
    if (!hash.isEmpty() && m_activated)
        m_activated(hash);
}

void NetworkMap::leaveEvent(QEvent *)
{
    m_hover.clear();
    QToolTip::hideText();
    update();
}

}
