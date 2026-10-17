#include "StrataViews.h"

#include <QApplication>
#include <QEasingCurve>
#include <QHeaderView>
#include <QLineEdit>
#include <QLinearGradient>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPicture>
#include <QRadialGradient>
#include <QScrollBar>
#include <QSet>
#include <QSplitter>
#include <QTextBrowser>
#include <QToolTip>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QVariantAnimation>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <numeric>

namespace forkmesh::strata {

using insight::FileInfo;
using insight::ModuleNode;

namespace {

QString baseName(const QString &path)
{
    const int slash = path.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? path : path.mid(slash + 1);
}

QColor withAlpha(QColor color, double alpha)
{
    color.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return color;
}

double luminance(const QColor &color)
{
    return 0.2126 * color.redF() + 0.7152 * color.greenF() + 0.0722 * color.blueF();
}

QColor readableOn(const QColor &fill)
{
    return luminance(fill) > 0.52 ? QColor(QStringLiteral("#0d1117")) : QColor(QStringLiteral("#ffffff"));
}

QFont fontPx(const QWidget *widget, double pixels, bool bold = false)
{
    QFont font = widget ? widget->font() : QApplication::font();
    font.setPixelSize(std::max(1, int(std::lround(pixels))));
    font.setBold(bold);
    return font;
}

QPainterPath edgePath(const QVector<QPointF> &points, bool flat)
{
    QPainterPath path;
    if (points.size() < 2)
        return path;
    path.moveTo(points.first());
    if (flat && points.size() == 3) {
        const QPointF s = points[0];
        const QPointF m = points[1];
        const QPointF e = points[2];
        path.cubicTo(QPointF(s.x(), s.y() + (m.y() - s.y()) * 1.33), QPointF(e.x(), e.y() + (m.y() - e.y()) * 1.33), e);
        return path;
    }
    for (int i = 0; i + 1 < points.size(); ++i) {
        const QPointF a = points[i];
        const QPointF b = points[i + 1];
        if (std::abs(a.x() - b.x()) < 0.5) {
            path.lineTo(b);
            continue;
        }
        const double dy = (b.y() - a.y()) * 0.5;
        path.cubicTo(QPointF(a.x(), a.y() + dy), QPointF(b.x(), b.y() - dy), b);
    }
    return path;
}

void softShadow(QPainter &painter, const QRectF &rect, double radius, const QColor &shadow, double scale)
{
    painter.setPen(Qt::NoPen);
    const double unit = 1.0 / std::max(0.2, scale);
    for (int i = 3; i >= 1; --i) {
        QColor color = shadow;
        color.setAlphaF(shadow.alphaF() * (0.45 / i));
        painter.setBrush(color);
        const double grow = i * 1.6 * unit;
        painter.drawRoundedRect(rect.adjusted(-grow, -grow + 2.5 * unit, grow, grow + 3.5 * unit), radius + grow, radius + grow);
    }
}

void drawDot(QPainter &painter, const QPointF &center, double radius, const QColor &color)
{
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawEllipse(center, radius, radius);
}

QPixmap dotIcon(const QColor &color)
{
    const qreal dpr = qApp ? qApp->devicePixelRatio() : 1.0;
    QPixmap pixmap(QSize(14, 14) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    drawDot(painter, QPointF(7, 7), 4.5, color);
    return pixmap;
}

double moduleComplexityT(const ModuleNode &node)
{
    const double average = node.functions > 0 ? double(node.complexity) / node.functions : 1.0;
    return std::clamp((average - 1.5) / 8.0, 0.0, 1.0);
}

bool matches(const QString &text, const QString &filter)
{
    return filter.isEmpty() || text.contains(filter, Qt::CaseInsensitive);
}

}

Colors Colors::from(const QWidget *widget)
{
    Colors c;
    const QPalette palette = widget ? widget->palette() : QApplication::palette();
    c.dark = palette.color(QPalette::Window).lightness() < 128;
    c.background = c.dark ? QColor(0x0d, 0x11, 0x17) : QColor(0xff, 0xff, 0xff);
    c.surface = c.dark ? QColor(0x16, 0x1b, 0x22) : QColor(0xf6, 0xf8, 0xfa);
    c.raised = c.dark ? QColor(0x1c, 0x23, 0x2d) : QColor(0xff, 0xff, 0xff);
    c.border = c.dark ? QColor(0x30, 0x36, 0x3d) : QColor(0xd0, 0xd7, 0xde);
    c.text = c.dark ? QColor(0xe6, 0xed, 0xf3) : QColor(0x1f, 0x23, 0x28);
    c.muted = c.dark ? QColor(0x8b, 0x94, 0x9e) : QColor(0x65, 0x6d, 0x76);
    c.faint = c.dark ? QColor(0x21, 0x26, 0x2d) : QColor(0xea, 0xee, 0xf2);
    c.accent = c.dark ? QColor(0x58, 0xa6, 0xff) : QColor(0x09, 0x69, 0xda);
    c.dependent = c.dark ? QColor(0xbc, 0x8c, 0xff) : QColor(0x82, 0x50, 0xdf);
    c.healthy = QColor(0x3f, 0xb9, 0x8f);
    c.problematic = QColor(0xe3, 0xb3, 0x41);
    c.unhealthy = QColor(0xe5, 0x53, 0x4b);
    return c;
}

QColor mix(const QColor &a, const QColor &b, double t)
{
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

QColor scaleColor(double t)
{
    t = std::clamp(t, 0.0, 1.0);
    static const QColor good(0x3f, 0xb9, 0x8f);
    static const QColor mid(0xe3, 0xb3, 0x41);
    static const QColor bad(0xe5, 0x53, 0x4b);
    return t < 0.5 ? mix(good, mid, t * 2.0) : mix(mid, bad, (t - 0.5) * 2.0);
}

QColor healthColor(double health)
{
    return scaleColor((10.0 - std::clamp(health, 1.0, 10.0)) / 9.0);
}

QColor ratingColor(int rating)
{
    static const QColor colors[] = {QColor(0x3f, 0xb9, 0x8f), QColor(0x8c, 0xc2, 0x65), QColor(0xe3, 0xb3, 0x41),
                                    QColor(0xf0, 0x88, 0x3e), QColor(0xe5, 0x53, 0x4b)};
    return colors[std::clamp(rating, 0, 4)];
}

QColor severityColor(Severity severity)
{
    switch (severity) {
    case Severity::Critical:
        return QColor(0xe5, 0x53, 0x4b);
    case Severity::High:
        return QColor(0xf0, 0x88, 0x3e);
    case Severity::Medium:
        return QColor(0xe3, 0xb3, 0x41);
    case Severity::Low:
        return QColor(0x8b, 0x94, 0x9e);
    }
    return QColor(0x8b, 0x94, 0x9e);
}

QColor instabilityColor(double instability)
{
    static const QColor stable(0x4c, 0x8e, 0xda);
    static const QColor middle(0x9a, 0x7c, 0xe0);
    static const QColor volatileColor(0xf0, 0x9a, 0x4a);
    const double t = std::clamp(instability, 0.0, 1.0);
    return t < 0.5 ? mix(stable, middle, t * 2.0) : mix(middle, volatileColor, (t - 0.5) * 2.0);
}

GraphView::GraphView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setFocusPolicy(Qt::ClickFocus);
    setMinimumSize(320, 240);
    m_highlightAnimation = new QVariantAnimation(this);
    m_highlightAnimation->setDuration(320);
    m_highlightAnimation->setEasingCurve(QEasingCurve::OutCubic);
    m_highlightAnimation->setStartValue(0.0);
    m_highlightAnimation->setEndValue(1.0);
    connect(m_highlightAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_highlight = value.toDouble();
        update();
    });
    m_viewAnimation = new QVariantAnimation(this);
    m_viewAnimation->setDuration(360);
    m_viewAnimation->setEasingCurve(QEasingCurve::InOutCubic);
    m_viewAnimation->setStartValue(0.0);
    m_viewAnimation->setEndValue(1.0);
    connect(m_viewAnimation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        const double t = value.toDouble();
        m_scale = std::exp(std::log(m_fromScale) + (std::log(m_toScale) - std::log(m_fromScale)) * t);
        m_offset = m_fromOffset + (m_toOffset - m_fromOffset) * t;
        invalidate();
        update();
    });
}

void GraphView::setModel(const ModelPtr &model)
{
    m_model = model;
    m_drill = DrillGraph();
    m_selection = Selection();
    m_hover = -1;
    m_highlight = 0;
    m_fitted = false;
    rebuildScene();
    fit(false);
    if (m_onDrillChanged)
        m_onDrillChanged();
}

bool GraphView::settled() const
{
    return m_model && m_fitted && m_viewAnimation->state() != QAbstractAnimation::Running;
}

QString GraphView::legendTitle() const
{
    switch (m_colorBy) {
    case ColorBy::Rating:
        return QStringLiteral("Debt rating");
    case ColorBy::Health:
        return QStringLiteral("Health");
    case ColorBy::Instability:
        return QStringLiteral("Instability");
    case ColorBy::Complexity:
        return QStringLiteral("Complexity");
    }
    return {};
}

void GraphView::setColorBy(ColorBy colorBy)
{
    if (m_colorBy == colorBy)
        return;
    m_colorBy = colorBy;
    recolor();
    invalidate();
    update();
}

void GraphView::setSelection(const Selection &selection)
{
    if (m_selection == selection)
        return;
    const int before = focusItem();
    m_selection = selection;
    const int after = focusItem();
    if (after != before) {
        if (after >= 0)
            startHighlight();
        else
            m_highlight = 0;
    }
    update();
}

void GraphView::setFilter(const QString &filter)
{
    const QString trimmed = filter.trimmed();
    if (trimmed == m_filter)
        return;
    m_filter = trimmed;
    recolor();
    invalidate();
    update();
}

void GraphView::startHighlight()
{
    m_highlightAnimation->stop();
    if (!isVisible()) {
        m_highlight = 1;
        return;
    }
    m_highlight = 0;
    m_highlightAnimation->start();
}

void GraphView::drillInto(int module)
{
    if (!m_model || module < 0 || module >= m_model->report.graph.nodes.size())
        return;
    m_drill = drillGraph(*m_model->snapshot, m_model->report.graph, module);
    m_hover = -1;
    rebuildScene();
    fit(false);
    startHighlight();
    if (m_onDrillChanged)
        m_onDrillChanged();
}

void GraphView::drillOut()
{
    if (m_drill.module < 0)
        return;
    m_drill = DrillGraph();
    m_hover = -1;
    rebuildScene();
    fit(false);
    startHighlight();
    if (m_onDrillChanged)
        m_onDrillChanged();
}

void GraphView::rebuildScene()
{
    m_items.clear();
    m_links.clear();
    m_bounds = QRectF();
    if (!m_model || !m_model->snapshot)
        return;
    const Report &report = m_model->report;
    const insight::Snapshot &snapshot = *m_model->snapshot;
    const QLocale locale;
    const GraphLayout *layout = nullptr;
    if (m_drill.module < 0) {
        layout = &m_model->layout;
        for (int i = 0; i < report.graph.nodes.size() && i < layout->boxes.size(); ++i) {
            const ModuleNode &node = report.graph.nodes[i];
            Item item;
            item.rect = layout->boxes[i];
            item.module = true;
            item.index = i;
            item.title = node.name.isEmpty() ? node.path : node.name;
            item.caption = QStringLiteral("%1 LOC · %2 %3")
                               .arg(locale.toString(node.codeLines))
                               .arg(node.files.size())
                               .arg(node.files.size() == 1 ? QStringLiteral("file") : QStringLiteral("files"));
            item.cycle = node.cycle >= 0;
            QVector<int> files = node.files;
            std::stable_sort(files.begin(), files.end(), [&snapshot](int a, int b) {
                return snapshot.files[a].codeLines > snapshot.files[b].codeLines;
            });
            if (files.size() > 240)
                files.resize(240);
            QVector<double> weights;
            for (int f : files)
                weights.append(std::max(1, snapshot.files[f].codeLines));
            const QRectF inner = item.rect.adjusted(7, 24 + 18, -7, -7);
            if (inner.height() > 6 && inner.width() > 6) {
                const QVector<QRectF> cells = squarify(weights, inner);
                for (int k = 0; k < cells.size(); ++k) {
                    if (cells[k].isEmpty())
                        continue;
                    item.cells.append({cells[k], healthColor(snapshot.files[files[k]].health)});
                }
            }
            m_items.append(item);
        }
    } else {
        layout = &m_drill.layout;
        for (int k = 0; k < m_drill.nodes.size() && k < layout->boxes.size(); ++k) {
            const DrillNode &node = m_drill.nodes[k];
            Item item;
            item.rect = layout->boxes[k];
            item.module = node.module;
            item.external = node.module;
            item.dependent = node.dependent;
            item.index = node.index;
            if (node.module) {
                const ModuleNode &mod = report.graph.nodes[node.index];
                item.title = mod.name.isEmpty() ? mod.path : mod.name;
                item.caption = node.dependent ? QStringLiteral("uses these files · %1").arg(node.weight)
                                              : QStringLiteral("used · %1").arg(node.weight);
                item.cycle = mod.cycle >= 0 && mod.cycle == report.graph.nodes[m_drill.module].cycle;
            } else {
                const FileInfo &file = snapshot.files[node.index];
                item.title = baseName(file.path);
                item.caption = QStringLiteral("%1 LOC · max cc %2").arg(locale.toString(file.codeLines)).arg(file.maxComplexity);
                item.cycle = false;
            }
            m_items.append(item);
        }
    }
    for (const RoutedEdge &edge : layout->edges) {
        Link link;
        link.from = edge.from;
        link.to = edge.to;
        link.weight = edge.weight;
        link.cycle = edge.cycle;
        link.width = std::clamp(1.0 + 1.25 * std::log2(1.0 + edge.weight), 1.0, 7.0);
        QVector<QPointF> points = edge.points;
        if (points.size() >= 2) {
            const QPointF tip = points.last();
            const QPointF before = points[points.size() - 2];
            const double direction = tip.y() >= before.y() ? 1.0 : -1.0;
            const double length = 8.0 + link.width * 1.4;
            const double half = 4.0 + link.width * 0.9;
            QPolygonF arrow;
            arrow << tip << QPointF(tip.x() - half, tip.y() - direction * length)
                  << QPointF(tip.x() + half, tip.y() - direction * length);
            link.arrow = arrow;
            points.last() = QPointF(tip.x(), tip.y() - direction * length * 0.8);
            link.path = edgePath(points, edge.flat);
        }
        m_links.append(link);
    }
    m_bounds = layout->bounds;
    recolor();
    invalidate();
}

QColor GraphView::colorForModule(int module) const
{
    const Report &report = m_model->report;
    const ModuleNode &node = report.graph.nodes[module];
    switch (m_colorBy) {
    case ColorBy::Rating:
        return ratingColor(module < report.modules.size() ? report.modules[module].rating : 0);
    case ColorBy::Health:
        return healthColor(node.health);
    case ColorBy::Instability:
        return instabilityColor(node.instability);
    case ColorBy::Complexity:
        return scaleColor(moduleComplexityT(node));
    }
    return ratingColor(0);
}

QColor GraphView::colorForFile(int file) const
{
    const Report &report = m_model->report;
    const FileInfo &info = m_model->snapshot->files[file];
    switch (m_colorBy) {
    case ColorBy::Rating:
        return ratingColor(file < report.files.size() ? report.files[file].rating : 0);
    case ColorBy::Health:
        return healthColor(info.health);
    case ColorBy::Instability: {
        const int sum = info.dependsOn.size() + info.dependedOnBy.size();
        return instabilityColor(sum ? double(info.dependsOn.size()) / sum : 0.0);
    }
    case ColorBy::Complexity:
        return scaleColor(std::clamp((info.maxComplexity - 4) / 22.0, 0.0, 1.0));
    }
    return ratingColor(0);
}

void GraphView::recolor()
{
    if (!m_model)
        return;
    const insight::Snapshot &snapshot = *m_model->snapshot;
    const auto &nodes = m_model->report.graph.nodes;
    for (Item &item : m_items) {
        if (item.module) {
            item.fill = colorForModule(item.index);
            item.match = m_filter.isEmpty() || matches(nodes[item.index].path, m_filter)
                         || matches(nodes[item.index].name, m_filter);
            if (!item.match && !item.external) {
                for (int f : nodes[item.index].files) {
                    if (matches(snapshot.files[f].path, m_filter)) {
                        item.match = true;
                        break;
                    }
                }
            }
        } else {
            item.fill = colorForFile(item.index);
            item.match = matches(snapshot.files[item.index].path, m_filter);
        }
    }
}

int GraphView::focusItem() const
{
    if (m_selection.kind == SelectionKind::None)
        return -1;
    for (int i = 0; i < m_items.size(); ++i) {
        const Item &item = m_items[i];
        if (m_selection.kind == SelectionKind::Module && item.module && item.index == m_selection.index)
            return i;
        if (m_selection.kind == SelectionKind::File && !item.module && item.index == m_selection.index)
            return i;
    }
    return -1;
}

QPointF GraphView::toWorld(const QPointF &screen) const
{
    return (screen - m_offset) / m_scale;
}

QPointF GraphView::toScreen(const QPointF &world) const
{
    return world * m_scale + m_offset;
}

int GraphView::itemAt(const QPointF &position) const
{
    const QPointF world = toWorld(position);
    for (int i = m_items.size() - 1; i >= 0; --i) {
        if (m_items[i].rect.contains(world))
            return i;
    }
    return -1;
}

void GraphView::invalidate()
{
    m_cacheValid = false;
}

void GraphView::fit(bool animated)
{
    if (m_bounds.isEmpty() || width() <= 0 || height() <= 0) {
        m_fitted = m_model != nullptr;
        return;
    }
    const QRectF area = QRectF(rect()).adjusted(24, 24, -24, -64);
    const double scale = std::clamp(std::min(area.width() / m_bounds.width(), area.height() / m_bounds.height()), 0.04, 1.25);
    const QPointF offset = area.center() - m_bounds.center() * scale;
    m_fitted = true;
    if (animated && isVisible())
        animateTo(scale, offset);
    else {
        m_viewAnimation->stop();
        m_scale = scale;
        m_offset = offset;
        invalidate();
        update();
    }
}

void GraphView::animateTo(double scale, const QPointF &offset)
{
    m_viewAnimation->stop();
    m_fromScale = m_scale;
    m_toScale = scale;
    m_fromOffset = m_offset;
    m_toOffset = offset;
    m_viewAnimation->start();
}

void GraphView::ensureCache()
{
    const qreal dpr = devicePixelRatioF();
    const QSize pixelSize = size() * dpr;
    if (m_cacheValid && m_cache.size() == pixelSize && m_cacheScale == m_scale && m_cacheOffset == m_offset)
        return;
    if (m_cache.size() != pixelSize)
        m_cache = QPixmap(pixelSize);
    m_cache.setDevicePixelRatio(dpr);
    const Colors colors = Colors::from(this);
    m_cache.fill(colors.background);
    QPainter painter(&m_cache);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const double spacing = 28;
    QColor dot = colors.dark ? QColor(255, 255, 255, 16) : QColor(0, 0, 0, 20);
    painter.setPen(Qt::NoPen);
    painter.setBrush(dot);
    const double step = spacing;
    const double ox = std::fmod(m_offset.x(), step);
    const double oy = std::fmod(m_offset.y(), step);
    for (double y = oy - step; y < height() + step; y += step) {
        for (double x = ox - step; x < width() + step; x += step)
            painter.drawEllipse(QPointF(x, y), 1.0, 1.0);
    }
    const QRectF world(toWorld(QPointF(0, 0)), toWorld(QPointF(width(), height())));
    painter.translate(m_offset);
    painter.scale(m_scale, m_scale);
    paintScene(painter, world, false);
    m_cacheScale = m_scale;
    m_cacheOffset = m_offset;
    m_cacheValid = true;
}

void GraphView::paintLink(QPainter &painter, const Link &link, const QColor &color, double scale, double widthBoost) const
{
    QPen pen(color, std::max(0.8, (link.width + widthBoost) * std::clamp(scale, 0.35, 1.6)));
    pen.setCosmetic(true);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(link.path);
    if (scale * 8 > 2.5 && !link.arrow.isEmpty()) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawPolygon(link.arrow);
    }
}

void GraphView::paintItem(QPainter &painter, const Item &item, double scale, double opacity, bool raised) const
{
    const Colors colors = Colors::from(this);
    painter.save();
    painter.setOpacity(opacity);
    const QRectF r = item.rect;
    const double screenHeight = r.height() * scale;
    if (item.external) {
        const double radius = r.height() / 2.0;
        softShadow(painter, r, radius, QColor(0, 0, 0, colors.dark ? 90 : 40), scale);
        painter.setBrush(raised ? colors.raised : colors.surface);
        QPen pen(item.dependent ? withAlpha(colors.dependent, 0.75) : withAlpha(colors.accent, 0.7), 1.3);
        pen.setCosmetic(true);
        pen.setStyle(Qt::DashLine);
        painter.setPen(pen);
        painter.drawRoundedRect(r, radius, radius);
        drawDot(painter, QPointF(r.left() + 16, r.center().y()), 4.5, item.fill);
        if (screenHeight > 12) {
            painter.setPen(colors.text);
            painter.setFont(fontPx(this, std::min(17.0, std::max(12.5, 10.5 / scale)), true));
            const QRectF textRect = r.adjusted(28, 2, -12, -r.height() / 2.0 + 2);
            painter.drawText(textRect, Qt::AlignLeft | Qt::AlignBottom,
                             painter.fontMetrics().elidedText(item.title, Qt::ElideMiddle, int(textRect.width())));
            painter.setPen(colors.muted);
            painter.setFont(fontPx(this, std::min(15.0, std::max(10.5, 9.0 / scale))));
            painter.drawText(r.adjusted(28, r.height() / 2.0, -12, -2), Qt::AlignLeft | Qt::AlignTop, item.caption);
        }
        painter.restore();
        return;
    }
    const double radius = item.module ? 9.0 : 7.0;
    softShadow(painter, r, radius, QColor(0, 0, 0, colors.dark ? 120 : 45), scale);
    QLinearGradient body(r.topLeft(), r.bottomLeft());
    const QColor base = raised ? colors.raised : colors.surface;
    body.setColorAt(0, mix(base, item.fill, colors.dark ? 0.14 : 0.10));
    body.setColorAt(1, mix(base, item.fill, colors.dark ? 0.07 : 0.05));
    painter.setBrush(body);
    QPen border(item.cycle ? colors.unhealthy : mix(colors.border, item.fill, 0.55), item.cycle ? 2.0 : 1.2);
    border.setCosmetic(true);
    painter.setPen(border);
    painter.drawRoundedRect(r, radius, radius);
    if (item.module) {
        const QRectF header(r.left(), r.top(), r.width(), 24);
        QPainterPath clip;
        clip.addRoundedRect(r.adjusted(0.6, 0.6, -0.6, -0.6), radius, radius);
        painter.save();
        painter.setClipPath(clip);
        QLinearGradient strip(header.topLeft(), header.bottomLeft());
        strip.setColorAt(0, item.fill.lighter(108));
        strip.setColorAt(1, item.fill.darker(108));
        painter.fillRect(header, strip);
        painter.restore();
        if (screenHeight > 22) {
            painter.setPen(readableOn(item.fill));
            painter.setFont(fontPx(this, std::min(19.0, std::max(12.5, 10.5 / scale)), true));
            const QRectF titleRect = header.adjusted(9, 0, -9, 0);
            painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(item.title, Qt::ElideMiddle, int(titleRect.width())));
        }
        if (screenHeight > 46) {
            painter.setPen(colors.muted);
            painter.setFont(fontPx(this, std::min(14.0, std::max(10.5, 9.0 / scale))));
            painter.drawText(QRectF(r.left() + 8, r.top() + 25, r.width() - 16, 16), Qt::AlignLeft | Qt::AlignVCenter, item.caption);
        }
        if (screenHeight > 24) {
            painter.setPen(Qt::NoPen);
            const bool fine = scale * 8 > 3;
            for (const auto &cell : item.cells) {
                QColor color = cell.second;
                color.setAlphaF(colors.dark ? 0.78 : 0.85);
                painter.setBrush(color);
                const QRectF cr = fine ? cell.first.adjusted(0.6, 0.6, -0.6, -0.6) : cell.first;
                if (cr.width() * scale >= 2.5 && cr.height() * scale >= 2.5)
                    painter.drawRoundedRect(cr, 1.5, 1.5);
                else
                    painter.drawRect(cr);
            }
        }
    } else {
        painter.setPen(Qt::NoPen);
        painter.setBrush(item.fill);
        QPainterPath bar;
        bar.addRoundedRect(QRectF(r.left(), r.top(), 6, r.height()), 3, 3);
        painter.drawPath(bar);
        if (screenHeight > 14) {
            painter.setPen(colors.text);
            painter.setFont(fontPx(this, std::min(20.0, std::max(12.5, 10.5 / scale)), true));
            const QRectF titleRect = r.adjusted(14, 5, -8, -r.height() / 2.0);
            painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(item.title, Qt::ElideMiddle, int(titleRect.width())));
            painter.setPen(colors.muted);
            painter.setFont(fontPx(this, std::min(16.0, std::max(10.5, 9.0 / scale))));
            painter.drawText(r.adjusted(14, r.height() / 2.0, -8, -4), Qt::AlignLeft | Qt::AlignVCenter, item.caption);
        }
    }
    painter.restore();
}

void GraphView::paintScene(QPainter &painter, const QRectF &world, bool) const
{
    const Colors colors = Colors::from(this);
    const QRectF visible = world.adjusted(-40, -40, 40, 40);
    const QColor plain = colors.dark ? QColor(0x8b, 0x94, 0x9e, 105) : QColor(0x57, 0x60, 0x6a, 95);
    const QColor cycle = withAlpha(colors.unhealthy, 0.85);
    for (int pass = 0; pass < 2; ++pass) {
        for (const Link &link : m_links) {
            if (link.cycle != (pass == 1))
                continue;
            if (!link.path.controlPointRect().intersects(visible))
                continue;
            const bool faded = link.from < m_items.size() && link.to < m_items.size()
                               && (!m_items[link.from].match || !m_items[link.to].match);
            QColor color = link.cycle ? cycle : plain;
            if (faded)
                color.setAlphaF(color.alphaF() * 0.25);
            paintLink(painter, link, color, m_scale, 0);
        }
    }
    for (const Item &item : m_items) {
        if (!item.rect.intersects(visible))
            continue;
        paintItem(painter, item, m_scale, item.match ? 1.0 : 0.22, false);
    }
}

void GraphView::paintLegend(QPainter &painter) const
{
    const Colors colors = Colors::from(this);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    const QFont label = fontPx(this, 11);
    const QFont strong = fontPx(this, 11, true);
    QStringList names;
    QVector<QColor> swatches;
    bool gradient = false;
    QString low;
    QString high;
    switch (m_colorBy) {
    case ColorBy::Rating:
        for (int i = 0; i < 5; ++i) {
            names << QString(ratingLetter(i));
            swatches << ratingColor(i);
        }
        break;
    case ColorBy::Health:
        gradient = true;
        low = QStringLiteral("Unhealthy");
        high = QStringLiteral("Healthy");
        break;
    case ColorBy::Instability:
        gradient = true;
        low = QStringLiteral("Stable");
        high = QStringLiteral("Unstable");
        break;
    case ColorBy::Complexity:
        gradient = true;
        low = QStringLiteral("Simple");
        high = QStringLiteral("Complex");
        break;
    }
    const double h = 34;
    const double w = gradient ? 520 : 470;
    const QRectF box(14, height() - h - 14, w, h);
    painter.setPen(Qt::NoPen);
    painter.setBrush(withAlpha(colors.surface, 0.94));
    painter.drawRoundedRect(box, 8, 8);
    double x = box.left() + 12;
    const double cy = box.center().y();
    painter.setFont(strong);
    painter.setPen(colors.text);
    const QString title = legendTitle();
    painter.drawText(QRectF(x, box.top(), 120, h), Qt::AlignLeft | Qt::AlignVCenter, title);
    x += painter.fontMetrics().horizontalAdvance(title) + 12;
    painter.setFont(label);
    if (gradient) {
        painter.setPen(colors.muted);
        const double lw = painter.fontMetrics().horizontalAdvance(low);
        painter.drawText(QRectF(x, box.top(), lw + 2, h), Qt::AlignLeft | Qt::AlignVCenter, low);
        x += lw + 8;
        QLinearGradient bar(QPointF(x, 0), QPointF(x + 110, 0));
        for (int i = 0; i <= 10; ++i) {
            const double t = i / 10.0;
            QColor c = m_colorBy == ColorBy::Health ? healthColor(1 + 9 * t)
                       : m_colorBy == ColorBy::Instability ? instabilityColor(t)
                                                           : scaleColor(t);
            bar.setColorAt(t, c);
        }
        painter.setPen(Qt::NoPen);
        painter.setBrush(bar);
        painter.drawRoundedRect(QRectF(x, cy - 5, 110, 10), 5, 5);
        x += 118;
        painter.setPen(colors.muted);
        const double hw = painter.fontMetrics().horizontalAdvance(high);
        painter.drawText(QRectF(x, box.top(), hw + 2, h), Qt::AlignLeft | Qt::AlignVCenter, high);
        x += hw + 18;
    } else {
        for (int i = 0; i < names.size(); ++i) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(swatches[i]);
            painter.drawRoundedRect(QRectF(x, cy - 8, 16, 16), 4, 4);
            painter.setPen(readableOn(swatches[i]));
            painter.setFont(strong);
            painter.drawText(QRectF(x, cy - 8, 16, 16), Qt::AlignCenter, names[i]);
            x += 21;
        }
        painter.setFont(label);
        x += 12;
    }
    const auto lineKey = [&](const QColor &color, const QString &text) {
        QPen pen(color, 2.2);
        pen.setCapStyle(Qt::RoundCap);
        painter.setPen(pen);
        painter.drawLine(QPointF(x, cy), QPointF(x + 18, cy));
        painter.setPen(colors.muted);
        const double tw = painter.fontMetrics().horizontalAdvance(text);
        painter.drawText(QRectF(x + 24, box.top(), tw + 2, h), Qt::AlignLeft | Qt::AlignVCenter, text);
        x += 24 + tw + 14;
    };
    lineKey(colors.accent, QStringLiteral("uses"));
    lineKey(colors.dependent, QStringLiteral("used by"));
    lineKey(colors.unhealthy, QStringLiteral("cycle"));
    painter.setPen(colors.muted);
    painter.setFont(fontPx(this, 10.5));
    const QString hint = m_drill.module < 0 ? QStringLiteral("Scroll to zoom · drag to pan · double-click a module to see its files")
                                            : QStringLiteral("Double-click a file to open it, a module to go there");
    painter.drawText(QRectF(box.right() + 14, box.top(), width() - box.right() - 28, h), Qt::AlignRight | Qt::AlignVCenter, hint);
    painter.restore();
}

void GraphView::paintEmpty(QPainter &painter) const
{
    const Colors colors = Colors::from(this);
    painter.fillRect(rect(), colors.background);
    painter.setPen(colors.muted);
    painter.setFont(fontPx(this, 13));
    QString text = QStringLiteral("Open or clone a repository, or pick a folder to analyze.");
    if (m_model && m_model->snapshot && !m_model->snapshot->files.isEmpty())
        text = QStringLiteral("No modules with source files were found.");
    painter.drawText(rect(), Qt::AlignCenter, text);
}

void GraphView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    if (!m_model || m_items.isEmpty()) {
        paintEmpty(painter);
        return;
    }
    ensureCache();
    painter.drawPixmap(0, 0, m_cache);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    const Colors colors = Colors::from(this);
    const int focus = focusItem();
    if (focus >= 0 && m_highlight > 0) {
        const double h = m_highlight;
        QColor veil = colors.background;
        veil.setAlphaF(0.62 * h);
        painter.fillRect(rect(), veil);
        painter.save();
        painter.translate(m_offset);
        painter.scale(m_scale, m_scale);
        QSet<int> uses;
        QSet<int> usedBy;
        for (const Link &link : m_links) {
            if (link.from == focus) {
                uses.insert(link.to);
                paintLink(painter, link, withAlpha(colors.accent, 0.95 * h), m_scale, 1.2);
            } else if (link.to == focus) {
                usedBy.insert(link.from);
                paintLink(painter, link, withAlpha(colors.dependent, 0.95 * h), m_scale, 1.2);
            }
        }
        const auto ring = [&](int index, const QColor &color, double width) {
            const Item &item = m_items[index];
            const double radius = item.external ? item.rect.height() / 2.0 : (item.module ? 9.0 : 7.0);
            for (int i = 3; i >= 1; --i) {
                QPen glow(withAlpha(color, 0.16 * h / i), width + i * 3.0);
                glow.setCosmetic(true);
                painter.setPen(glow);
                painter.setBrush(Qt::NoBrush);
                painter.drawRoundedRect(item.rect, radius, radius);
            }
            QPen pen(withAlpha(color, h), width);
            pen.setCosmetic(true);
            painter.setPen(pen);
            painter.drawRoundedRect(item.rect, radius, radius);
        };
        for (int i : usedBy) {
            paintItem(painter, m_items[i], m_scale, 1.0, true);
            ring(i, colors.dependent, 1.6);
        }
        for (int i : uses) {
            paintItem(painter, m_items[i], m_scale, 1.0, true);
            ring(i, colors.accent, 1.6);
        }
        paintItem(painter, m_items[focus], m_scale, 1.0, true);
        ring(focus, colors.accent, 2.6);
        painter.restore();
    }
    if (m_hover >= 0 && m_hover < m_items.size() && m_hover != focus) {
        painter.save();
        painter.translate(m_offset);
        painter.scale(m_scale, m_scale);
        const Item &item = m_items[m_hover];
        const double radius = item.external ? item.rect.height() / 2.0 : (item.module ? 9.0 : 7.0);
        QPen pen(withAlpha(colors.accent, 0.8), 1.6);
        pen.setCosmetic(true);
        painter.setPen(pen);
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(item.rect.adjusted(-2 / m_scale, -2 / m_scale, 2 / m_scale, 2 / m_scale), radius + 2, radius + 2);
        painter.restore();
    }
    paintLegend(painter);
}

void GraphView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    invalidate();
    if (!m_fitted || m_viewAnimation->state() != QAbstractAnimation::Running)
        fit(false);
}

void GraphView::showTooltip(int index, const QPoint &global)
{
    if (index < 0 || !m_model) {
        QToolTip::hideText();
        return;
    }
    const Item &item = m_items[index];
    const Report &report = m_model->report;
    const QLocale locale;
    QString html;
    if (item.module) {
        const ModuleNode &node = report.graph.nodes[item.index];
        const Score &score = report.modules[item.index];
        html = QStringLiteral("<b>%1</b><br>%2 lines of code · %3 files<br>Rating %4 · %5 debt<br>"
                              "Ca %6 · Ce %7 · I %8 · A %9")
                   .arg(node.path.toHtmlEscaped(), locale.toString(node.codeLines))
                   .arg(node.files.size())
                   .arg(ratingLetter(score.rating))
                   .arg(formatDebt(score.debt))
                   .arg(node.afferent)
                   .arg(node.efferent)
                   .arg(QString::number(node.instability, 'f', 2), QString::number(node.abstractness, 'f', 2));
        if (node.cycle >= 0)
            html += QStringLiteral("<br><span style=\"color:#e5534b\">In a dependency cycle</span>");
    } else {
        const FileInfo &file = m_model->snapshot->files[item.index];
        const Score &score = report.files[item.index];
        html = QStringLiteral("<b>%1</b><br>%2 lines of code · max complexity %3<br>Rating %4 · %5 debt · health %6<br>"
                              "Imports %7 files · imported by %8")
                   .arg(file.path.toHtmlEscaped(), locale.toString(file.codeLines))
                   .arg(file.maxComplexity)
                   .arg(ratingLetter(score.rating))
                   .arg(formatDebt(score.debt))
                   .arg(QString::number(file.health, 'f', 1))
                   .arg(file.dependsOn.size())
                   .arg(file.dependedOnBy.size());
    }
    QToolTip::showText(global, html, this);
}

void GraphView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(event);
    m_pressPos = event->position().toPoint();
    m_pressOffset = m_offset;
    m_pressedItem = itemAt(event->position());
    m_dragging = false;
}

void GraphView::mouseMoveEvent(QMouseEvent *event)
{
    if (event->buttons() & Qt::LeftButton) {
        const QPoint delta = event->position().toPoint() - m_pressPos;
        if (!m_dragging && delta.manhattanLength() > 4) {
            m_dragging = true;
            m_viewAnimation->stop();
            setCursor(Qt::ClosedHandCursor);
            QToolTip::hideText();
        }
        if (m_dragging) {
            m_offset = m_pressOffset + QPointF(delta);
            invalidate();
            update();
        }
        return;
    }
    const int hover = itemAt(event->position());
    if (hover != m_hover) {
        m_hover = hover;
        setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
        showTooltip(hover, event->globalPosition().toPoint());
    }
}

void GraphView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return QWidget::mouseReleaseEvent(event);
    if (m_dragging) {
        m_dragging = false;
        setCursor(m_hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        return;
    }
    if (!m_callbacks.select)
        return;
    if (m_pressedItem >= 0 && m_pressedItem < m_items.size()) {
        const Item &item = m_items[m_pressedItem];
        m_callbacks.select({item.module ? SelectionKind::Module : SelectionKind::File, item.index});
    } else {
        m_callbacks.select({});
    }
}

void GraphView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int index = itemAt(event->position());
    if (index < 0) {
        fit(true);
        return;
    }
    const Item item = m_items[index];
    if (item.module) {
        if (m_drill.module < 0 || item.external) {
            drillInto(item.index);
            if (m_callbacks.select)
                m_callbacks.select({SelectionKind::Module, item.index});
        }
    } else if (m_callbacks.open) {
        m_callbacks.open(item.index, 0);
    }
}

void GraphView::wheelEvent(QWheelEvent *event)
{
    const double delta = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y();
    if (delta == 0)
        return;
    m_viewAnimation->stop();
    const double factor = std::pow(1.0015, delta);
    const double scale = std::clamp(m_scale * factor, 0.03, 4.0);
    const QPointF cursor = event->position();
    m_offset = cursor - (cursor - m_offset) * (scale / m_scale);
    m_scale = scale;
    invalidate();
    update();
    event->accept();
}

void GraphView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_hover >= 0) {
        m_hover = -1;
        update();
    }
}

void GraphView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    if (m_highlightAnimation->state() == QAbstractAnimation::Running) {
        m_highlightAnimation->stop();
        m_highlight = 1;
    }
    if (m_viewAnimation->state() == QAbstractAnimation::Running) {
        m_viewAnimation->stop();
        m_scale = m_toScale;
        m_offset = m_toOffset;
    }
    m_cache = QPixmap();
    m_cacheValid = false;
}

void GraphView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    invalidate();
}

void GraphView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        invalidate();
        update();
    }
}

MatrixView::MatrixView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setMouseTracking(true);
    viewport()->setAttribute(Qt::WA_OpaquePaintEvent);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
}

void MatrixView::setModel(const ModelPtr &model)
{
    m_model = model;
    m_weights.clear();
    m_maxWeight = 1;
    m_cellRow = m_cellColumn = -1;
    m_hoverRow = m_hoverColumn = -1;
    if (m_model) {
        const auto &graph = m_model->report.graph;
        const int n = graph.nodes.size();
        for (const insight::ModuleEdge &edge : graph.edges) {
            if (edge.from < 0 || edge.to < 0)
                continue;
            m_weights[qint64(edge.from) * n + edge.to] += std::max(1, edge.weight);
            m_maxWeight = std::max(m_maxWeight, m_weights.value(qint64(edge.from) * n + edge.to));
        }
    }
    relayout();
    viewport()->update();
}

void MatrixView::setSelection(const Selection &selection)
{
    m_selection = selection;
    viewport()->update();
}

void MatrixView::setCell(int row, int column)
{
    m_cellRow = row;
    m_cellColumn = column;
    viewport()->update();
}

int MatrixView::weight(int from, int to) const
{
    if (!m_model)
        return 0;
    return m_weights.value(qint64(from) * m_model->report.graph.nodes.size() + to, 0);
}

void MatrixView::relayout()
{
    const int n = m_model ? m_model->order.size() : 0;
    int label = 80;
    if (m_model) {
        const QFontMetrics metrics(fontPx(this, 12));
        for (const ModuleNode &node : m_model->report.graph.nodes)
            label = std::max(label, metrics.horizontalAdvance(node.name));
    }
    m_header = std::clamp(label + 30, 160, 250);
    const QSize area = viewport()->size();
    if (n > 0) {
        const int available = std::min(area.width(), area.height()) - m_header - 12;
        m_cell = std::clamp(available / n, 9, 46);
    }
    const int content = m_header + n * m_cell + 12;
    horizontalScrollBar()->setRange(0, std::max(0, content - area.width()));
    horizontalScrollBar()->setPageStep(area.width());
    horizontalScrollBar()->setSingleStep(m_cell);
    verticalScrollBar()->setRange(0, std::max(0, content - area.height()));
    verticalScrollBar()->setPageStep(area.height());
    verticalScrollBar()->setSingleStep(m_cell);
}

void MatrixView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    relayout();
}

void MatrixView::scrollContentsBy(int, int)
{
    viewport()->update();
}

QPair<int, int> MatrixView::cellAt(const QPoint &position) const
{
    const int n = m_model ? m_model->order.size() : 0;
    const int x = position.x() - m_header + horizontalScrollBar()->value();
    const int y = position.y() - m_header + verticalScrollBar()->value();
    int row = -1;
    int column = -1;
    if (position.y() >= m_header && y >= 0 && y / m_cell < n)
        row = y / m_cell;
    if (position.x() >= m_header && x >= 0 && x / m_cell < n)
        column = x / m_cell;
    if (position.x() < m_header && position.y() >= m_header)
        column = -2;
    if (position.y() < m_header && position.x() >= m_header)
        row = -2;
    return {row, column};
}

void MatrixView::paintEvent(QPaintEvent *)
{
    QPainter painter(viewport());
    const Colors colors = Colors::from(this);
    painter.fillRect(viewport()->rect(), colors.background);
    const int n = m_model ? m_model->order.size() : 0;
    if (n == 0) {
        painter.setPen(colors.muted);
        painter.setFont(fontPx(this, 13));
        painter.drawText(viewport()->rect(), Qt::AlignCenter, QStringLiteral("Open or clone a repository, or pick a folder to analyze."));
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing);
    const auto &graph = m_model->report.graph;
    const QVector<int> &order = m_model->order;
    const int sx = horizontalScrollBar()->value();
    const int sy = verticalScrollBar()->value();
    const int ox = m_header - sx;
    const int oy = m_header - sy;
    const QSize area = viewport()->size();
    const int c0 = std::max(0, sx / m_cell);
    const int c1 = std::min(n - 1, (sx + area.width() - m_header) / m_cell + 1);
    const int r0 = std::max(0, sy / m_cell);
    const int r1 = std::min(n - 1, (sy + area.height() - m_header) / m_cell + 1);
    const QVector<int> layers = moduleLayers(graph);
    const double logMax = std::log(1.0 + m_maxWeight);
    const QFont numberFont = fontPx(this, std::clamp(m_cell * 0.42, 8.0, 11.5));

    painter.save();
    painter.setClipRect(QRect(m_header, m_header, area.width() - m_header, area.height() - m_header));
    const QRectF gridRect(ox, oy, n * m_cell, n * m_cell);
    painter.fillRect(gridRect, colors.surface);
    if (m_hoverRow >= 0)
        painter.fillRect(QRectF(ox, oy + m_hoverRow * m_cell, n * m_cell, m_cell), withAlpha(colors.accent, 0.10));
    if (m_hoverColumn >= 0)
        painter.fillRect(QRectF(ox + m_hoverColumn * m_cell, oy, m_cell, n * m_cell), withAlpha(colors.accent, 0.10));
    painter.setFont(numberFont);
    for (int r = r0; r <= r1; ++r) {
        for (int c = c0; c <= c1; ++c) {
            const QRectF cell(ox + c * m_cell, oy + r * m_cell, m_cell, m_cell);
            if (r == c) {
                painter.fillRect(cell.adjusted(1, 1, -1, -1), colors.faint);
                continue;
            }
            const int a = order[r];
            const int b = order[c];
            const int uses = weight(a, b);
            const int usedBy = weight(b, a);
            if (!uses && !usedBy)
                continue;
            QColor color = uses && usedBy ? colors.unhealthy : uses ? colors.accent : colors.dependent;
            const int shown = uses ? uses : usedBy;
            const double t = logMax > 0 ? std::log(1.0 + shown) / logMax : 1.0;
            QColor fill = withAlpha(color, 0.28 + 0.62 * t);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(cell.adjusted(1.5, 1.5, -1.5, -1.5), 3, 3);
            if (m_cell >= 18) {
                painter.setPen(t > 0.45 ? readableOn(color) : colors.text);
                painter.drawText(cell, Qt::AlignCenter, shown >= 1000 ? QStringLiteral("%1k").arg(shown / 1000) : QString::number(shown));
            }
        }
    }
    QPen gridPen(withAlpha(colors.border, 0.45), 1);
    painter.setPen(gridPen);
    if (m_cell >= 12) {
        for (int i = std::max(0, std::min(r0, c0)); i <= n; ++i) {
            painter.drawLine(QPointF(ox, oy + i * m_cell), QPointF(ox + n * m_cell, oy + i * m_cell));
            painter.drawLine(QPointF(ox + i * m_cell, oy), QPointF(ox + i * m_cell, oy + n * m_cell));
        }
    }
    QPen layerPen(withAlpha(colors.muted, 0.6), 1.4);
    painter.setPen(layerPen);
    for (int i = 1; i < n; ++i) {
        if (layers[order[i]] != layers[order[i - 1]]) {
            painter.drawLine(QPointF(ox, oy + i * m_cell), QPointF(ox + n * m_cell, oy + i * m_cell));
            painter.drawLine(QPointF(ox + i * m_cell, oy), QPointF(ox + i * m_cell, oy + n * m_cell));
        }
    }
    QPen cyclePen(colors.unhealthy, 2);
    painter.setBrush(Qt::NoBrush);
    for (int i = 0; i < n;) {
        const int c = graph.nodes[order[i]].cycle;
        int j = i + 1;
        while (c >= 0 && j < n && graph.nodes[order[j]].cycle == c)
            ++j;
        if (c >= 0 && j - i > 1) {
            painter.setPen(cyclePen);
            painter.drawRoundedRect(QRectF(ox + i * m_cell, oy + i * m_cell, (j - i) * m_cell, (j - i) * m_cell).adjusted(1, 1, -1, -1), 4, 4);
        }
        i = j;
    }
    if (m_cellRow >= 0 && m_cellColumn >= 0) {
        QPen pen(colors.accent, 2.4);
        painter.setPen(pen);
        painter.drawRoundedRect(QRectF(ox + m_cellColumn * m_cell, oy + m_cellRow * m_cell, m_cell, m_cell).adjusted(0.5, 0.5, -0.5, -0.5), 3, 3);
    }
    painter.restore();

    const int selectedModule = m_selection.kind == SelectionKind::Module ? m_selection.index : -1;
    const QFont labelFont = fontPx(this, std::clamp(m_cell * 0.6, 9.0, 12.5));
    const QFont boldFont = fontPx(this, std::clamp(m_cell * 0.6, 9.0, 12.5), true);
    painter.save();
    painter.setClipRect(QRect(0, m_header, m_header, area.height() - m_header));
    painter.fillRect(QRect(0, m_header, m_header, area.height() - m_header), colors.background);
    for (int r = r0; r <= r1; ++r) {
        const int m = order[r];
        const QRectF row(0, oy + r * m_cell, m_header - 6, m_cell);
        const bool highlighted = r == m_hoverRow || m == selectedModule || r == m_cellRow;
        if (highlighted)
            painter.fillRect(row.adjusted(0, 0, 6, 0), withAlpha(colors.accent, m == selectedModule ? 0.18 : 0.10));
        if (graph.nodes[m].cycle >= 0)
            drawDot(painter, QPointF(10, row.center().y()), std::min(3.5, m_cell * 0.2), colors.unhealthy);
        if (m_cell >= 10) {
            painter.setFont(m == selectedModule ? boldFont : labelFont);
            painter.setPen(m == selectedModule ? colors.accent : colors.text);
            painter.drawText(row.adjusted(18, 0, -4, 0), Qt::AlignRight | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(graph.nodes[m].name, Qt::ElideMiddle, int(row.width() - 22)));
        }
    }
    painter.restore();
    painter.save();
    painter.setClipRect(QRect(m_header, 0, area.width() - m_header, m_header));
    painter.fillRect(QRect(m_header, 0, area.width() - m_header, m_header), colors.background);
    for (int c = c0; c <= c1; ++c) {
        const int m = order[c];
        const double x = ox + c * m_cell;
        const bool highlighted = c == m_hoverColumn || m == selectedModule || c == m_cellColumn;
        if (highlighted)
            painter.fillRect(QRectF(x, 0, m_cell, m_header), withAlpha(colors.accent, m == selectedModule ? 0.18 : 0.10));
        if (graph.nodes[m].cycle >= 0)
            drawDot(painter, QPointF(x + m_cell / 2.0, m_header - 10), std::min(3.5, m_cell * 0.2), colors.unhealthy);
        if (m_cell >= 10) {
            painter.save();
            painter.translate(x + m_cell / 2.0, m_header - 18);
            painter.rotate(-90);
            painter.setFont(m == selectedModule ? boldFont : labelFont);
            painter.setPen(m == selectedModule ? colors.accent : colors.text);
            const QRectF text(0, -m_cell / 2.0, m_header - 24, m_cell);
            painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(graph.nodes[m].name, Qt::ElideMiddle, int(text.width())));
            painter.restore();
        }
    }
    painter.restore();
    painter.fillRect(QRect(0, 0, m_header, m_header), colors.background);
    painter.setFont(fontPx(this, 11));
    const auto key = [&](int line, const QColor &color, const QString &text) {
        const double y = 16 + line * 20;
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRoundedRect(QRectF(12, y, 12, 12), 3, 3);
        painter.setPen(colors.muted);
        painter.drawText(QRectF(30, y - 3, m_header - 34, 18), Qt::AlignLeft | Qt::AlignVCenter, text);
    };
    key(0, colors.accent, QStringLiteral("Row uses column"));
    key(1, colors.dependent, QStringLiteral("Column uses row"));
    key(2, colors.unhealthy, QStringLiteral("Both ways: cycle"));
    painter.setPen(colors.muted);
    painter.setFont(fontPx(this, 10.5));
    painter.drawText(QRectF(12, 80, m_header - 20, m_header - 86), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap,
                     QStringLiteral("Numbers count file imports. Click a cell to list them."));
}

void MatrixView::mouseMoveEvent(QMouseEvent *event)
{
    const auto cell = cellAt(event->position().toPoint());
    const int row = cell.first >= 0 ? cell.first : -1;
    const int column = cell.second >= 0 ? cell.second : -1;
    if (row == m_hoverRow && column == m_hoverColumn)
        return;
    m_hoverRow = row;
    m_hoverColumn = column;
    viewport()->update();
    if (!m_model || row < 0 || column < 0) {
        QToolTip::hideText();
        return;
    }
    const auto &graph = m_model->report.graph;
    const int a = m_model->order[row];
    const int b = m_model->order[column];
    QString text = QStringLiteral("<b>%1</b> → <b>%2</b>: %3 imports<br><b>%2</b> → <b>%1</b>: %4 imports")
                       .arg(graph.nodes[a].name.toHtmlEscaped(), graph.nodes[b].name.toHtmlEscaped())
                       .arg(weight(a, b))
                       .arg(weight(b, a));
    if (a == b)
        text = QStringLiteral("<b>%1</b>: %2 internal imports").arg(graph.nodes[a].name.toHtmlEscaped()).arg(graph.nodes[a].internalEdges);
    QToolTip::showText(event->globalPosition().toPoint(), text, viewport());
}

void MatrixView::mousePressEvent(QMouseEvent *event)
{
    if (!m_model || event->button() != Qt::LeftButton)
        return;
    const auto cell = cellAt(event->position().toPoint());
    if (cell.first >= 0 && cell.second >= 0) {
        m_cellRow = cell.first;
        m_cellColumn = cell.second;
        viewport()->update();
        if (m_callbacks.cell)
            m_callbacks.cell(m_model->order[cell.first], m_model->order[cell.second]);
    } else if (cell.first >= 0 && cell.second == -2) {
        if (m_callbacks.select)
            m_callbacks.select({SelectionKind::Module, m_model->order[cell.first]});
    } else if (cell.first == -2 && cell.second >= 0) {
        if (m_callbacks.select)
            m_callbacks.select({SelectionKind::Module, m_model->order[cell.second]});
    }
}

void MatrixView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_model)
        return;
    const auto cell = cellAt(event->position().toPoint());
    int module = -1;
    if (cell.first >= 0 && cell.second == -2)
        module = m_model->order[cell.first];
    else if (cell.first == -2 && cell.second >= 0)
        module = m_model->order[cell.second];
    if (module >= 0 && m_callbacks.showModule)
        m_callbacks.showModule(module);
}

void MatrixView::leaveEvent(QEvent *event)
{
    QAbstractScrollArea::leaveEvent(event);
    m_hoverRow = m_hoverColumn = -1;
    viewport()->update();
}

ScatterView::ScatterView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(320, 260);
}

void ScatterView::setModel(const ModelPtr &model)
{
    m_model = model;
    m_hover = -1;
    m_drawOrder.clear();
    m_maxLines = 1;
    if (m_model) {
        const auto &nodes = m_model->report.graph.nodes;
        for (int i = 0; i < nodes.size(); ++i) {
            m_drawOrder.append(i);
            m_maxLines = std::max<double>(m_maxLines, nodes[i].codeLines);
        }
        std::stable_sort(m_drawOrder.begin(), m_drawOrder.end(), [&nodes](int a, int b) {
            return nodes[a].codeLines > nodes[b].codeLines;
        });
    }
    m_background = QPixmap();
    update();
}

void ScatterView::setSelection(const Selection &selection)
{
    m_selection = selection;
    update();
}

void ScatterView::setFilter(const QString &filter)
{
    m_filter = filter.trimmed();
    update();
}

QRectF ScatterView::plotRect() const
{
    const double side = std::max(120.0, std::min(width() - 140.0, height() - 110.0));
    return QRectF((width() - side) / 2.0 + 20, 30, side, side);
}

QPointF ScatterView::pointFor(int module) const
{
    const QRectF plot = plotRect();
    const ModuleNode &node = m_model->report.graph.nodes[module];
    return QPointF(plot.left() + std::clamp(node.instability, 0.0, 1.0) * plot.width(),
                   plot.bottom() - std::clamp(node.abstractness, 0.0, 1.0) * plot.height());
}

double ScatterView::radiusFor(int module) const
{
    const double side = plotRect().width();
    const ModuleNode &node = m_model->report.graph.nodes[module];
    return std::clamp(4.0 + side * 0.04 * std::sqrt(node.codeLines / m_maxLines), 4.0, side * 0.05);
}

int ScatterView::moduleAt(const QPointF &position) const
{
    if (!m_model)
        return -1;
    int best = -1;
    double bestDistance = 1e9;
    for (int i = m_drawOrder.size() - 1; i >= 0; --i) {
        const int m = m_drawOrder[i];
        const double d = QLineF(pointFor(m), position).length();
        const double r = radiusFor(m) + 3;
        if (d <= r && d / r < bestDistance) {
            bestDistance = d / r;
            best = m;
        }
    }
    return best;
}

void ScatterView::ensureBackground()
{
    const qreal dpr = devicePixelRatioF();
    if (!m_background.isNull() && m_background.size() == size() * dpr)
        return;
    m_background = QPixmap(size() * dpr);
    m_background.setDevicePixelRatio(dpr);
    const Colors colors = Colors::from(this);
    m_background.fill(colors.background);
    QPainter painter(&m_background);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF plot = plotRect();
    painter.setPen(Qt::NoPen);
    painter.setBrush(colors.surface);
    painter.drawRoundedRect(plot.adjusted(-1, -1, 1, 1), 6, 6);
    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(plot, 6, 6);
    painter.setClipPath(clip);
    QRadialGradient pain(plot.bottomLeft(), plot.width() * 0.62);
    pain.setColorAt(0, withAlpha(colors.unhealthy, colors.dark ? 0.34 : 0.24));
    pain.setColorAt(0.7, withAlpha(colors.unhealthy, 0.06));
    pain.setColorAt(1, withAlpha(colors.unhealthy, 0.0));
    painter.fillRect(plot, pain);
    QRadialGradient useless(plot.topRight(), plot.width() * 0.62);
    useless.setColorAt(0, withAlpha(colors.problematic, colors.dark ? 0.30 : 0.24));
    useless.setColorAt(0.7, withAlpha(colors.problematic, 0.05));
    useless.setColorAt(1, withAlpha(colors.problematic, 0.0));
    painter.fillRect(plot, useless);
    QPolygonF band;
    const double b = 0.12;
    band << QPointF(plot.left(), plot.top()) << QPointF(plot.left() + b * plot.width(), plot.top())
         << QPointF(plot.right(), plot.bottom() - b * plot.height()) << QPointF(plot.right(), plot.bottom())
         << QPointF(plot.right() - b * plot.width(), plot.bottom()) << QPointF(plot.left(), plot.top() + b * plot.height());
    painter.setBrush(withAlpha(colors.healthy, colors.dark ? 0.08 : 0.07));
    painter.drawPolygon(band);
    QPen grid(withAlpha(colors.border, 0.55), 1);
    painter.setPen(grid);
    for (int i = 1; i < 4; ++i) {
        const double t = i / 4.0;
        painter.drawLine(QPointF(plot.left() + t * plot.width(), plot.top()), QPointF(plot.left() + t * plot.width(), plot.bottom()));
        painter.drawLine(QPointF(plot.left(), plot.top() + t * plot.height()), QPointF(plot.right(), plot.top() + t * plot.height()));
    }
    QPen sequence(withAlpha(colors.healthy, 0.85), 2);
    sequence.setStyle(Qt::DashLine);
    painter.setPen(sequence);
    painter.drawLine(plot.topLeft(), plot.bottomRight());
    painter.restore();
    painter.setFont(fontPx(this, 12, true));
    painter.setPen(withAlpha(colors.unhealthy, 0.95));
    painter.drawText(QRectF(plot.left() + 14, plot.bottom() - plot.height() * 0.2, 200, 22), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Zone of pain"));
    painter.setPen(withAlpha(colors.problematic, 0.95));
    painter.drawText(QRectF(plot.right() - 214, plot.top() + 12, 200, 22), Qt::AlignRight | Qt::AlignVCenter, QStringLiteral("Zone of uselessness"));
    painter.save();
    painter.translate(plot.left() + plot.width() * 0.58, plot.top() + plot.height() * 0.42);
    painter.rotate(45);
    painter.setPen(withAlpha(colors.healthy, 0.95));
    painter.drawText(QRectF(-80, -22, 160, 18), Qt::AlignCenter, QStringLiteral("Main sequence"));
    painter.restore();
    painter.setFont(fontPx(this, 11));
    painter.setPen(colors.muted);
    for (int i = 0; i <= 4; ++i) {
        const double t = i / 4.0;
        const QString label = QString::number(t, 'f', i % 2 ? 2 : 1);
        painter.drawText(QRectF(plot.left() + t * plot.width() - 20, plot.bottom() + 6, 40, 16), Qt::AlignCenter, label);
        painter.drawText(QRectF(plot.left() - 46, plot.bottom() - t * plot.height() - 8, 38, 16), Qt::AlignRight | Qt::AlignVCenter, label);
    }
    painter.setFont(fontPx(this, 12, true));
    painter.setPen(colors.text);
    painter.drawText(QRectF(plot.left(), plot.bottom() + 26, plot.width(), 20), Qt::AlignCenter,
                     QStringLiteral("Instability  I = Ce ÷ (Ca + Ce)"));
    painter.save();
    painter.translate(plot.left() - 58, plot.center().y());
    painter.rotate(-90);
    painter.drawText(QRectF(-plot.height() / 2.0, -10, plot.height(), 20), Qt::AlignCenter,
                     QStringLiteral("Abstractness  A = abstract types ÷ types"));
    painter.restore();
    const QRectF legend(plot.right() + 18, plot.top(), std::max(60.0, width() - plot.right() - 24), 140);
    if (legend.width() >= 90) {
        painter.setFont(fontPx(this, 11, true));
        painter.setPen(colors.text);
        painter.drawText(QRectF(legend.left(), legend.top(), legend.width(), 18), Qt::AlignLeft, QStringLiteral("Distance D"));
        QLinearGradient bar(QPointF(0, legend.top() + 24), QPointF(0, legend.top() + 124));
        for (int i = 0; i <= 10; ++i)
            bar.setColorAt(i / 10.0, scaleColor(i / 10.0));
        painter.setPen(Qt::NoPen);
        painter.setBrush(bar);
        painter.drawRoundedRect(QRectF(legend.left(), legend.top() + 24, 10, 100), 5, 5);
        painter.setFont(fontPx(this, 10.5));
        painter.setPen(colors.muted);
        painter.drawText(QRectF(legend.left() + 16, legend.top() + 18, 120, 14), Qt::AlignLeft, QStringLiteral("0 on the sequence"));
        painter.drawText(QRectF(legend.left() + 16, legend.top() + 114, 120, 14), Qt::AlignLeft, QStringLiteral("1 far from it"));
        painter.drawText(QRectF(legend.left(), legend.top() + 140, legend.width(), 40), Qt::AlignLeft | Qt::TextWordWrap,
                         QStringLiteral("Bubble size: lines of code"));
    }
}

void ScatterView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const Colors colors = Colors::from(this);
    if (!m_model || m_model->report.graph.nodes.isEmpty()) {
        painter.fillRect(rect(), colors.background);
        painter.setPen(colors.muted);
        painter.setFont(fontPx(this, 13));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Open or clone a repository, or pick a folder to analyze."));
        return;
    }
    ensureBackground();
    painter.drawPixmap(0, 0, m_background);
    painter.setRenderHint(QPainter::Antialiasing);
    const auto &nodes = m_model->report.graph.nodes;
    const int selected = m_selection.kind == SelectionKind::Module ? m_selection.index : -1;
    for (int m : m_drawOrder) {
        const QPointF p = pointFor(m);
        const double r = radiusFor(m);
        const bool match = m_filter.isEmpty() || matches(nodes[m].path, m_filter);
        const QColor color = scaleColor(nodes[m].distance);
        QRadialGradient fill(p - QPointF(r * 0.35, r * 0.35), r * 1.3);
        fill.setColorAt(0, withAlpha(color.lighter(130), match ? 0.92 : 0.18));
        fill.setColorAt(1, withAlpha(color, match ? 0.78 : 0.14));
        painter.setBrush(fill);
        QPen pen(withAlpha(color.darker(140), match ? 0.9 : 0.2), 1);
        painter.setPen(pen);
        painter.drawEllipse(p, r, r);
    }
    QVector<int> labelled;
    for (int i = 0; i < m_drawOrder.size() && labelled.size() < 10; ++i)
        labelled.append(m_drawOrder[i]);
    for (int m : m_drawOrder) {
        if (nodes[m].distance > 0.7 && labelled.size() < 18 && !labelled.contains(m))
            labelled.append(m);
    }
    if (selected >= 0 && !labelled.contains(selected))
        labelled.prepend(selected);
    if (m_hover >= 0 && !labelled.contains(m_hover))
        labelled.prepend(m_hover);
    QVector<QRectF> taken;
    painter.setFont(fontPx(this, 11.5, true));
    const QFontMetricsF metrics(painter.font());
    for (int m : labelled) {
        const QPointF p = pointFor(m);
        const double r = radiusFor(m);
        const QString text = nodes[m].name;
        const double w = metrics.horizontalAdvance(text) + 12;
        QRectF box(p.x() + r + 4, p.y() - 10, w, 20);
        if (box.right() > width() - 4)
            box.moveRight(p.x() - r - 4);
        bool clash = false;
        for (const QRectF &other : taken) {
            if (other.intersects(box)) {
                clash = true;
                break;
            }
        }
        if (clash && m != selected && m != m_hover)
            continue;
        taken.append(box);
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(colors.surface, 0.88));
        painter.drawRoundedRect(box, 5, 5);
        painter.setPen(m == selected ? colors.accent : colors.text);
        painter.drawText(box, Qt::AlignCenter, text);
    }
    if (selected >= 0 && selected < nodes.size()) {
        const QPointF p = pointFor(selected);
        const double r = radiusFor(selected);
        painter.setBrush(Qt::NoBrush);
        for (int i = 3; i >= 1; --i) {
            painter.setPen(QPen(withAlpha(colors.accent, 0.18 / i), 2.0 + i * 3.0));
            painter.drawEllipse(p, r + 2, r + 2);
        }
        painter.setPen(QPen(colors.accent, 2.4));
        painter.drawEllipse(p, r + 2, r + 2);
    }
    if (m_hover >= 0 && m_hover != selected) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(withAlpha(colors.accent, 0.75), 1.6));
        painter.drawEllipse(pointFor(m_hover), radiusFor(m_hover) + 2, radiusFor(m_hover) + 2);
    }
}

void ScatterView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    m_background = QPixmap();
}

void ScatterView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        m_background = QPixmap();
        update();
    }
}

void ScatterView::mouseMoveEvent(QMouseEvent *event)
{
    const int hover = moduleAt(event->position());
    if (hover == m_hover)
        return;
    m_hover = hover;
    setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
    if (hover < 0) {
        QToolTip::hideText();
        return;
    }
    const ModuleNode &node = m_model->report.graph.nodes[hover];
    QToolTip::showText(event->globalPosition().toPoint(),
                       QStringLiteral("<b>%1</b><br>I %2 · A %3 · D %4<br>Ca %5 · Ce %6 · %7 lines of code")
                           .arg(node.path.toHtmlEscaped(), QString::number(node.instability, 'f', 2),
                                QString::number(node.abstractness, 'f', 2), QString::number(node.distance, 'f', 2))
                           .arg(node.afferent)
                           .arg(node.efferent)
                           .arg(QLocale().toString(node.codeLines)),
                       this);
}

void ScatterView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_callbacks.select)
        return;
    const int m = moduleAt(event->position());
    m_callbacks.select(m >= 0 ? Selection{SelectionKind::Module, m} : Selection{});
}

void ScatterView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int m = moduleAt(event->position());
    if (m >= 0 && m_callbacks.showModule)
        m_callbacks.showModule(m);
}

void ScatterView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_hover >= 0) {
        m_hover = -1;
        update();
    }
}

MetricsView::MetricsView(QWidget *parent)
    : QWidget(parent)
{
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMinimumSize(320, 240);
}

void MetricsView::setModel(const ModelPtr &model)
{
    m_model = model;
    m_hover = -1;
    m_maxCommits = 1;
    if (m_model) {
        for (const FileInfo &file : m_model->snapshot->files)
            m_maxCommits = std::max<double>(m_maxCommits, file.commits);
    }
    relayout();
    update();
}

void MetricsView::setSizeBy(SizeBy sizeBy)
{
    if (m_sizeBy == sizeBy)
        return;
    m_sizeBy = sizeBy;
    relayout();
    update();
}

void MetricsView::setColorBy(ColorBy colorBy)
{
    if (m_colorBy == colorBy)
        return;
    m_colorBy = colorBy;
    m_cacheValid = false;
    update();
}

void MetricsView::setSelection(const Selection &selection)
{
    m_selection = selection;
    update();
}

void MetricsView::setFilter(const QString &filter)
{
    const QString trimmed = filter.trimmed();
    if (trimmed == m_filter)
        return;
    m_filter = trimmed;
    m_cacheValid = false;
    update();
}

QRectF MetricsView::mapRect() const
{
    return QRectF(rect()).adjusted(14, 6, -14, -46);
}

double MetricsView::sizeOf(int file) const
{
    const FileInfo &info = m_model->snapshot->files[file];
    switch (m_sizeBy) {
    case SizeBy::Lines:
        return info.codeLines;
    case SizeBy::Complexity:
        return info.codeLines > 0 ? std::max(1, info.complexity) : 0;
    case SizeBy::Functions:
        return info.codeLines > 0 ? std::max<int>(1, info.functions.size()) : 0;
    }
    return info.codeLines;
}

double MetricsView::colorValue(int file) const
{
    const FileInfo &info = m_model->snapshot->files[file];
    switch (m_colorBy) {
    case ColorBy::Health:
        return (10.0 - std::clamp(info.health, 1.0, 10.0)) / 9.0;
    case ColorBy::MaxComplexity:
        return std::clamp((info.maxComplexity - 4) / 26.0, 0.0, 1.0);
    case ColorBy::Debt:
        return file < m_model->report.files.size() ? std::clamp(m_model->report.files[file].ratio / 0.5, 0.0, 1.0) : 0.0;
    case ColorBy::ChangeFrequency:
        return std::sqrt(info.commits / m_maxCommits);
    }
    return 0;
}

QColor MetricsView::colorOf(int file) const
{
    const double t = colorValue(file);
    if (m_colorBy == ColorBy::ChangeFrequency) {
        const Colors colors = Colors::from(this);
        static const QColor warm(0xd2, 0x99, 0x22);
        static const QColor hot(0xf8, 0x51, 0x49);
        const QColor cold = colors.dark ? QColor(0x2a, 0x3a, 0x4e) : QColor(0xc8, 0xd3, 0xe0);
        return t < 0.5 ? mix(cold, warm, t * 2.0) : mix(warm, hot, (t - 0.5) * 2.0);
    }
    return scaleColor(t);
}

void MetricsView::relayout()
{
    m_fileRects.clear();
    m_moduleRects.clear();
    m_moduleOfRect.clear();
    m_cacheValid = false;
    if (!m_model || width() <= 0 || height() <= 0)
        return;
    const auto &nodes = m_model->report.graph.nodes;
    const int fileCount = m_model->snapshot->files.size();
    m_fileRects.resize(fileCount);
    QVector<double> moduleWeights(nodes.size(), 0);
    for (int m = 0; m < nodes.size(); ++m) {
        for (int f : nodes[m].files)
            moduleWeights[m] += sizeOf(f);
    }
    m_moduleRects = squarify(moduleWeights, mapRect());
    for (int m = 0; m < nodes.size(); ++m) {
        const QRectF box = m_moduleRects[m];
        if (box.width() < 2 || box.height() < 2)
            continue;
        const double header = box.height() > 44 && box.width() > 70 ? 17 : 0;
        const QRectF inner = box.adjusted(2, 2 + header, -2, -2);
        QVector<double> weights;
        for (int f : nodes[m].files)
            weights.append(sizeOf(f));
        const QVector<QRectF> cells = squarify(weights, inner);
        for (int k = 0; k < cells.size(); ++k)
            m_fileRects[nodes[m].files[k]] = cells[k];
    }
}

void MetricsView::ensureCache()
{
    const qreal dpr = devicePixelRatioF();
    if (m_cacheValid && m_cache.size() == size() * dpr)
        return;
    m_cache = QPixmap(size() * dpr);
    m_cache.setDevicePixelRatio(dpr);
    const Colors colors = Colors::from(this);
    m_cache.fill(colors.background);
    QPainter painter(&m_cache);
    painter.setRenderHint(QPainter::Antialiasing, false);
    const auto &nodes = m_model->report.graph.nodes;
    const auto &files = m_model->snapshot->files;
    const QFont header = fontPx(this, 11, true);
    const QFont label = fontPx(this, 10.5);
    for (int m = 0; m < nodes.size() && m < m_moduleRects.size(); ++m) {
        const QRectF box = m_moduleRects[m];
        if (box.width() < 2 || box.height() < 2)
            continue;
        painter.fillRect(box.adjusted(0.5, 0.5, -0.5, -0.5), colors.surface);
        for (int f : nodes[m].files) {
            const QRectF cell = m_fileRects[f];
            if (cell.width() < 0.6 || cell.height() < 0.6)
                continue;
            const bool match = m_filter.isEmpty() || matches(files[f].path, m_filter);
            QColor color = colorOf(f);
            if (!match)
                color = mix(colors.surface, color, 0.18);
            const QRectF inset = cell.width() > 3 && cell.height() > 3 ? cell.adjusted(0.5, 0.5, -0.5, -0.5) : cell;
            if (inset.width() > 18 && inset.height() > 18) {
                QLinearGradient cushion(inset.topLeft(), inset.bottomRight());
                cushion.setColorAt(0, color.lighter(118));
                cushion.setColorAt(1, color.darker(118));
                painter.fillRect(inset, cushion);
            } else {
                painter.fillRect(inset, color);
            }
            if (inset.width() > 64 && inset.height() > 18) {
                painter.setFont(label);
                painter.setPen(withAlpha(readableOn(color), 0.92));
                const QRectF text = inset.adjusted(5, 2, -4, -2);
                painter.drawText(text, Qt::AlignLeft | Qt::AlignTop,
                                 painter.fontMetrics().elidedText(baseName(files[f].path), Qt::ElideMiddle, int(text.width())));
            }
        }
        painter.setPen(QPen(colors.background, 2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(box);
        if (box.height() > 44 && box.width() > 70) {
            painter.setFont(header);
            painter.setPen(colors.text);
            const QRectF text(box.left() + 6, box.top() + 1, box.width() - 12, 16);
            painter.drawText(text, Qt::AlignLeft | Qt::AlignVCenter,
                             painter.fontMetrics().elidedText(nodes[m].name, Qt::ElideMiddle, int(text.width())));
        }
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF legendRect(14, height() - 38, width() - 28, 30);
    static const char *sizeNames[] = {"Lines of code", "Complexity", "Functions"};
    static const char *colorNames[] = {"Health", "Max complexity", "Debt ratio", "Change frequency"};
    painter.setFont(fontPx(this, 11));
    painter.setPen(colors.muted);
    const QString caption = QStringLiteral("Size: %1   ·   Colour: %2")
                                .arg(QString::fromLatin1(sizeNames[int(m_sizeBy)]), QString::fromLatin1(colorNames[int(m_colorBy)]));
    painter.drawText(legendRect, Qt::AlignLeft | Qt::AlignVCenter, caption);
    const double barWidth = 220;
    const QRectF bar(legendRect.right() - barWidth - 70, legendRect.center().y() - 5, barWidth, 10);
    QLinearGradient gradient(bar.topLeft(), bar.topRight());
    for (int i = 0; i <= 10; ++i) {
        const double t = i / 10.0;
        QColor c;
        if (m_colorBy == ColorBy::ChangeFrequency) {
            static const QColor warm(0xd2, 0x99, 0x22);
            static const QColor hot(0xf8, 0x51, 0x49);
            const QColor cold = colors.dark ? QColor(0x2a, 0x3a, 0x4e) : QColor(0xc8, 0xd3, 0xe0);
            c = t < 0.5 ? mix(cold, warm, t * 2.0) : mix(warm, hot, (t - 0.5) * 2.0);
        } else {
            c = scaleColor(t);
        }
        gradient.setColorAt(t, c);
    }
    painter.setPen(Qt::NoPen);
    painter.setBrush(gradient);
    painter.drawRoundedRect(bar, 5, 5);
    QString low;
    QString high;
    switch (m_colorBy) {
    case ColorBy::Health:
        low = QStringLiteral("Healthy");
        high = QStringLiteral("Unhealthy");
        break;
    case ColorBy::MaxComplexity:
        low = QStringLiteral("≤ 4");
        high = QStringLiteral("30+");
        break;
    case ColorBy::Debt:
        low = QStringLiteral("0 %");
        high = QStringLiteral("50 %+");
        break;
    case ColorBy::ChangeFrequency:
        low = QStringLiteral("0");
        high = QStringLiteral("%1 commits").arg(int(m_maxCommits));
        break;
    }
    painter.setPen(colors.muted);
    painter.drawText(QRectF(bar.left() - 90, legendRect.top(), 84, legendRect.height()), Qt::AlignRight | Qt::AlignVCenter, low);
    painter.drawText(QRectF(bar.right() + 6, legendRect.top(), 70, legendRect.height()), Qt::AlignLeft | Qt::AlignVCenter, high);
    m_cacheValid = true;
}

void MetricsView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const Colors colors = Colors::from(this);
    if (!m_model || m_model->snapshot->files.isEmpty()) {
        painter.fillRect(rect(), colors.background);
        painter.setPen(colors.muted);
        painter.setFont(fontPx(this, 13));
        painter.drawText(rect(), Qt::AlignCenter, QStringLiteral("Open or clone a repository, or pick a folder to analyze."));
        return;
    }
    ensureCache();
    painter.drawPixmap(0, 0, m_cache);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(Qt::NoBrush);
    if (m_selection.kind == SelectionKind::Module && m_selection.index >= 0 && m_selection.index < m_moduleRects.size()) {
        painter.setPen(QPen(colors.accent, 2.5));
        painter.drawRect(m_moduleRects[m_selection.index].adjusted(1, 1, -1, -1));
    }
    if (m_selection.kind == SelectionKind::File && m_selection.index >= 0 && m_selection.index < m_fileRects.size()) {
        const QRectF cell = m_fileRects[m_selection.index];
        painter.setPen(QPen(withAlpha(colors.accent, 0.3), 6));
        painter.drawRect(cell.adjusted(-1, -1, 1, 1));
        painter.setPen(QPen(colors.accent, 2.5));
        painter.drawRect(cell.adjusted(-1, -1, 1, 1));
    }
    if (m_hover >= 0 && m_hover < m_fileRects.size()) {
        painter.setPen(QPen(colors.text, 1.6));
        painter.drawRect(m_fileRects[m_hover].adjusted(0.5, 0.5, -0.5, -0.5));
    }
}

int MetricsView::fileAt(const QPointF &position) const
{
    if (!m_model)
        return -1;
    const auto &nodes = m_model->report.graph.nodes;
    for (int m = 0; m < m_moduleRects.size(); ++m) {
        if (!m_moduleRects[m].contains(position))
            continue;
        for (int f : nodes[m].files) {
            if (m_fileRects[f].contains(position))
                return f;
        }
        return -1;
    }
    return -1;
}

void MetricsView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
}

void MetricsView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange) {
        m_cacheValid = false;
        update();
    }
}

void MetricsView::mouseMoveEvent(QMouseEvent *event)
{
    const int hover = fileAt(event->position());
    if (hover == m_hover)
        return;
    m_hover = hover;
    setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
    if (hover < 0) {
        QToolTip::hideText();
        return;
    }
    const FileInfo &file = m_model->snapshot->files[hover];
    const Score &score = m_model->report.files[hover];
    QToolTip::showText(event->globalPosition().toPoint(),
                       QStringLiteral("<b>%1</b><br>%2 lines of code · %3 functions<br>Complexity %4 · max %5<br>"
                                      "Health %6 · rating %7 · %8 debt<br>%9 commits")
                           .arg(file.path.toHtmlEscaped(), QLocale().toString(file.codeLines))
                           .arg(file.functions.size())
                           .arg(file.complexity)
                           .arg(file.maxComplexity)
                           .arg(QString::number(file.health, 'f', 1))
                           .arg(ratingLetter(score.rating))
                           .arg(formatDebt(score.debt))
                           .arg(file.commits),
                       this);
}

void MetricsView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_callbacks.select)
        return;
    const int f = fileAt(event->position());
    m_callbacks.select(f >= 0 ? Selection{SelectionKind::File, f} : Selection{});
}

void MetricsView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int f = fileAt(event->position());
    if (f >= 0 && m_callbacks.open)
        m_callbacks.open(f, 0);
}

void MetricsView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_hover >= 0) {
        m_hover = -1;
        update();
    }
}

DashboardView::DashboardView(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setMouseTracking(true);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
}

void DashboardView::setModel(const ModelPtr &model)
{
    m_model = model;
    m_hover = -1;
    const int height = contentHeight(viewport()->width());
    verticalScrollBar()->setRange(0, std::max(0, height - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    viewport()->update();
}

int DashboardView::contentHeight(int width)
{
    QPicture picture;
    QPainter painter(&picture);
    paintContent(painter, width);
    return m_contentHeight;
}

void DashboardView::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    const int height = contentHeight(viewport()->width());
    verticalScrollBar()->setRange(0, std::max(0, height - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(24);
}

void DashboardView::scrollContentsBy(int, int)
{
    viewport()->update();
}

void DashboardView::paintContent(QPainter &painter, int width)
{
    m_hits.clear();
    const Colors colors = Colors::from(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::TextAntialiasing);
    if (!m_model || !m_model->snapshot) {
        m_contentHeight = 200;
        return;
    }
    const Report &report = m_model->report;
    const Metrics &metrics = report.metrics;
    const QLocale locale;
    const auto compact = [&locale](qint64 value) {
        if (value >= 1000000)
            return locale.toString(value / 1000000.0, 'f', 2) + QStringLiteral("M");
        if (value >= 100000)
            return locale.toString(value / 1000.0, 'f', 0) + QStringLiteral("k");
        return locale.toString(value);
    };
    const double margin = 20;
    const double gap = 14;
    const double full = std::max(560.0, width - 2 * margin);
    double y = 14;
    const auto surface = [&](const QRectF &rect) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colors.surface);
        painter.drawRoundedRect(rect, 10, 10);
    };
    const auto text = [&](const QRectF &rect, const QString &value, double px, bool bold, const QColor &color, int flags) {
        painter.setFont(fontPx(this, px, bold));
        painter.setPen(color);
        painter.drawText(rect, flags, painter.fontMetrics().elidedText(value, Qt::ElideMiddle, int(rect.width())));
    };

    const bool wide = full >= 980;
    const double ratingWidth = wide ? 400 : full;
    const double topHeight = 168;
    const QRectF ratingRect(margin, y, ratingWidth, topHeight);
    surface(ratingRect);
    const QColor rc = ratingColor(report.total.rating);
    const QRectF badge(ratingRect.left() + 22, ratingRect.top() + 24, 120, 120);
    for (int i = 3; i >= 1; --i) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(rc, 0.08 / i));
        painter.drawRoundedRect(badge.adjusted(-i * 4, -i * 4, i * 4, i * 4), 22 + i * 4, 22 + i * 4);
    }
    QLinearGradient badgeFill(badge.topLeft(), badge.bottomRight());
    badgeFill.setColorAt(0, rc.lighter(112));
    badgeFill.setColorAt(1, rc.darker(112));
    painter.setBrush(badgeFill);
    painter.drawRoundedRect(badge, 22, 22);
    text(badge, QString(ratingLetter(report.total.rating)), 72, true, readableOn(rc), Qt::AlignCenter);
    const double tx = badge.right() + 22;
    const double tw = ratingRect.right() - tx - 16;
    text(QRectF(tx, ratingRect.top() + 22, tw, 18), QStringLiteral("Technical debt ratio"), 12, false, colors.muted, Qt::AlignLeft | Qt::AlignVCenter);
    text(QRectF(tx, ratingRect.top() + 40, tw, 38), formatPercent(report.total.ratio), 32, true, colors.text, Qt::AlignLeft | Qt::AlignVCenter);
    text(QRectF(tx, ratingRect.top() + 86, tw, 18), QStringLiteral("Total debt"), 12, false, colors.muted, Qt::AlignLeft | Qt::AlignVCenter);
    text(QRectF(tx, ratingRect.top() + 104, tw, 20), formatDebt(report.total.debt), 16, true, colors.text, Qt::AlignLeft | Qt::AlignVCenter);
    text(QRectF(tx, ratingRect.top() + 126, tw, 18), QStringLiteral("Annual interest"), 12, false, colors.muted, Qt::AlignLeft | Qt::AlignVCenter);
    text(QRectF(tx, ratingRect.top() + 142, tw, 20), report.history ? formatDebt(report.total.interest) + QStringLiteral(" a year") : QStringLiteral("Needs git history"),
         14, true, report.history ? colors.text : colors.muted, Qt::AlignLeft | Qt::AlignVCenter);

    struct Tile {
        QString label;
        QString value;
        QString note;
    };
    const QList<Tile> tiles = {
        {QStringLiteral("Lines of code"), compact(metrics.codeLines), metrics.codeLines >= 100000 ? locale.toString(metrics.codeLines) : QString()},
        {QStringLiteral("Files"), locale.toString(metrics.files), metrics.testFiles ? QStringLiteral("%1 tests").arg(locale.toString(metrics.testFiles)) : QString()},
        {QStringLiteral("Modules"), locale.toString(metrics.modules), report.graph.cycles.isEmpty() ? QStringLiteral("no cycles") : QStringLiteral("%1 in cycles").arg(std::accumulate(report.graph.cycles.begin(), report.graph.cycles.end(), 0, [](int sum, const QVector<int> &c) { return sum + int(c.size()); }))},
        {QStringLiteral("Types"), compact(metrics.types), QString()},
        {QStringLiteral("Functions"), compact(metrics.functions), QString()},
        {QStringLiteral("Average complexity"), locale.toString(metrics.averageComplexity, 'f', 1), QStringLiteral("per function")},
        {QStringLiteral("Max complexity"), locale.toString(metrics.maxComplexity), metrics.maxComplexityFunction},
        {QStringLiteral("Comments"), QStringLiteral("%1 %").arg(locale.toString(metrics.commentPercent, 'f', 1)), QString()},
        {QStringLiteral("Duplicated"), QStringLiteral("%1 %").arg(locale.toString(metrics.duplicatedPercent, 'f', 1)), QString()},
        {QStringLiteral("Rule violations"), locale.toString(report.total.violations), QStringLiteral("%1 rules").arg(std::count_if(report.rules.begin(), report.rules.end(), [](const Rule &r) { return !r.violations.isEmpty(); }))},
    };
    const double tilesLeft = wide ? ratingRect.right() + gap : margin;
    const double tilesTop = wide ? y : ratingRect.bottom() + gap;
    const double tilesWidth = margin + full - tilesLeft;
    const int columns = tilesWidth >= 760 ? 5 : (tilesWidth >= 480 ? 4 : 2);
    const int rows = (tiles.size() + columns - 1) / columns;
    const double tileWidth = (tilesWidth - gap * (columns - 1)) / columns;
    const double tileHeight = wide && rows == 2 ? (topHeight - gap) / 2.0 : 77;
    for (int i = 0; i < tiles.size(); ++i) {
        const int row = i / columns;
        const int column = i % columns;
        const QRectF tile(tilesLeft + column * (tileWidth + gap), tilesTop + row * (tileHeight + gap), tileWidth, tileHeight);
        surface(tile);
        text(QRectF(tile.left() + 14, tile.top() + 10, tile.width() - 28, 16), tiles[i].label, 11.5, false, colors.muted, Qt::AlignLeft | Qt::AlignVCenter);
        text(QRectF(tile.left() + 14, tile.top() + 28, tile.width() - 28, 28), tiles[i].value, 22, true, colors.text, Qt::AlignLeft | Qt::AlignVCenter);
        if (!tiles[i].note.isEmpty() && tile.height() >= 70)
            text(QRectF(tile.left() + 14, tile.top() + 54, tile.width() - 28, 14), tiles[i].note, 10.5, false, colors.muted, Qt::AlignLeft | Qt::AlignVCenter);
    }
    y = std::max(ratingRect.bottom(), tilesTop + rows * (tileHeight + gap) - gap) + gap + 6;

    const bool twoColumns = full >= 900;
    const double leftWidth = twoColumns ? (full - gap) * 0.44 : full;
    const double rightLeft = twoColumns ? margin + leftWidth + gap : margin;
    const double rightWidth = twoColumns ? full - leftWidth - gap : full;
    double left = y;
    const double rowHeight = 34;

    const QRectF gates(margin, left, leftWidth, 50 + report.gates.size() * rowHeight + 8);
    surface(gates);
    text(QRectF(gates.left() + 18, gates.top() + 12, gates.width() - 36, 22), QStringLiteral("Quality gates"), 14, true, colors.text, Qt::AlignLeft | Qt::AlignVCenter);
    const int failing = report.gatesWith(GateStatus::Fail);
    const int warning = report.gatesWith(GateStatus::Warn);
    text(QRectF(gates.left() + 18, gates.top() + 12, gates.width() - 36, 22),
         failing ? QStringLiteral("%1 failing").arg(failing) : warning ? QStringLiteral("%1 warning").arg(warning) : QStringLiteral("All pass"),
         12, true, failing ? colors.unhealthy : warning ? colors.problematic : colors.healthy, Qt::AlignRight | Qt::AlignVCenter);
    for (int i = 0; i < report.gates.size(); ++i) {
        const GateResult &gate = report.gates[i];
        const QRectF row(gates.left() + 12, gates.top() + 46 + i * rowHeight, gates.width() - 24, rowHeight);
        const QColor color = gate.status == GateStatus::Fail ? colors.unhealthy : gate.status == GateStatus::Warn ? colors.problematic : colors.healthy;
        const QRectF pill(row.left() + 6, row.center().y() - 10, 54, 20);
        painter.setPen(Qt::NoPen);
        painter.setBrush(withAlpha(color, 0.18));
        painter.drawRoundedRect(pill, 10, 10);
        drawDot(painter, QPointF(pill.left() + 11, pill.center().y()), 3.5, color);
        text(pill.adjusted(18, 0, -4, 0), gateStatusName(gate.status), 11, true, color, Qt::AlignLeft | Qt::AlignVCenter);
        const double measuredWidth = std::min(row.width() * 0.3, 150.0);
        text(QRectF(pill.right() + 12, row.top(), row.width() - measuredWidth - 84, row.height()), gate.name, 12.5, false, colors.text, Qt::AlignLeft | Qt::AlignVCenter);
        text(QRectF(row.right() - measuredWidth - 6, row.top(), measuredWidth, row.height()), gate.measured, 12, false, colors.muted, Qt::AlignRight | Qt::AlignVCenter);
        if (i + 1 < report.gates.size()) {
            painter.setPen(QPen(withAlpha(colors.border, 0.5), 1));
            painter.drawLine(QPointF(row.left() + 6, row.bottom()), QPointF(row.right() - 6, row.bottom()));
        }
    }
    left = gates.bottom() + gap;

    QList<int> topRules;
    for (int i = 0; i < report.rules.size(); ++i) {
        if (!report.rules[i].violations.isEmpty())
            topRules.append(i);
    }
    std::stable_sort(topRules.begin(), topRules.end(), [&report](int a, int b) { return report.rules[a].debt > report.rules[b].debt; });
    if (topRules.size() > 6)
        topRules = topRules.mid(0, 6);
    const QRectF severity(margin, left, leftWidth, 116 + topRules.size() * 30 + (topRules.isEmpty() ? 0 : 10));
    surface(severity);
    text(QRectF(severity.left() + 18, severity.top() + 12, severity.width() - 36, 22), QStringLiteral("Rule violations by severity"), 14, true, colors.text, Qt::AlignLeft | Qt::AlignVCenter);
    const QRectF bar(severity.left() + 18, severity.top() + 48, severity.width() - 36, 12);
    const int totalViolations = std::max(1, report.total.violations);
    double bx = bar.left();
    painter.save();
    QPainterPath barClip;
    barClip.addRoundedRect(bar, 6, 6);
    painter.setClipPath(barClip);
    painter.fillRect(bar, colors.faint);
    for (int s = 0; s < 4; ++s) {
        const double w = bar.width() * report.bySeverity[s] / totalViolations;
        painter.fillRect(QRectF(bx, bar.top(), w, bar.height()), severityColor(Severity(s)));
        bx += w;
    }
    painter.restore();
    double lx = bar.left();
    for (int s = 0; s < 4; ++s) {
        drawDot(painter, QPointF(lx + 5, bar.bottom() + 22), 4.5, severityColor(Severity(s)));
        const QString label = QStringLiteral("%1 %2").arg(severityName(Severity(s))).arg(locale.toString(report.bySeverity[s]));
        painter.setFont(fontPx(this, 12));
        const double w = painter.fontMetrics().horizontalAdvance(label);
        text(QRectF(lx + 14, bar.bottom() + 12, w + 4, 20), label, 12, false, colors.text, Qt::AlignLeft | Qt::AlignVCenter);
        lx += w + 34;
    }
    for (int i = 0; i < topRules.size(); ++i) {
        const Rule &rule = report.rules[topRules[i]];
        const QRectF row(severity.left() + 18, severity.top() + 100 + i * 30, severity.width() - 36, 28);
        drawDot(painter, QPointF(row.left() + 5, row.center().y()), 4, severityColor(Severity(rule.worst())));
        text(QRectF(row.left() + 16, row.top(), row.width() - 150, row.height()), rule.title, 12.5, false, colors.text, Qt::AlignLeft | Qt::AlignVCenter);
        text(QRectF(row.right() - 140, row.top(), 140, row.height()),
             QStringLiteral("%1 · %2").arg(locale.toString(rule.violations.size()), formatDebt(rule.debt)), 12, false, colors.muted,
             Qt::AlignRight | Qt::AlignVCenter);
    }
    left = severity.bottom() + gap;

    double right = twoColumns ? y : left;
    QList<int> modules;
    for (int i = 0; i < report.modules.size(); ++i) {
        if (report.modules[i].debt > 0)
            modules.append(i);
    }
    std::stable_sort(modules.begin(), modules.end(), [&report](int a, int b) { return report.modules[a].debt > report.modules[b].debt; });
    if (modules.size() > 5)
        modules = modules.mid(0, 5);
    QList<int> files;
    for (int i = 0; i < report.files.size(); ++i) {
        if (report.files[i].debt > 0)
            files.append(i);
    }
    std::stable_sort(files.begin(), files.end(), [&report](int a, int b) { return report.files[a].debt > report.files[b].debt; });
    if (files.size() > 10)
        files = files.mid(0, 10);

    const auto rankedList = [&](double top, const QString &title, const QList<int> &items, bool moduleList) {
        const double listRow = 32;
        const QRectF box(rightLeft, top, rightWidth, 50 + std::max<int>(1, items.size()) * listRow + 8);
        surface(box);
        text(QRectF(box.left() + 18, box.top() + 12, box.width() - 36, 22), title, 14, true, colors.text, Qt::AlignLeft | Qt::AlignVCenter);
        if (items.isEmpty()) {
            text(QRectF(box.left() + 18, box.top() + 46, box.width() - 36, listRow), QStringLiteral("No debt found."), 12.5, false, colors.muted, Qt::AlignLeft | Qt::AlignVCenter);
            return box.bottom();
        }
        const double maxDebt = moduleList ? report.modules[items.first()].debt : report.files[items.first()].debt;
        for (int i = 0; i < items.size(); ++i) {
            const int index = items[i];
            const Score &score = moduleList ? report.modules[index] : report.files[index];
            const QRectF row(box.left() + 10, box.top() + 46 + i * listRow, box.width() - 20, listRow);
            const int hit = m_hits.size();
            m_hits.append({row, {moduleList ? SelectionKind::Module : SelectionKind::File, index}});
            if (hit == m_hover) {
                painter.setPen(Qt::NoPen);
                painter.setBrush(withAlpha(colors.accent, 0.10));
                painter.drawRoundedRect(row, 6, 6);
            }
            const QRectF chip(row.left() + 8, row.center().y() - 10, 20, 20);
            painter.setPen(Qt::NoPen);
            painter.setBrush(ratingColor(score.rating));
            painter.drawRoundedRect(chip, 5, 5);
            text(chip, QString(ratingLetter(score.rating)), 11.5, true, readableOn(ratingColor(score.rating)), Qt::AlignCenter);
            const QString name = moduleList ? report.graph.nodes[index].path : m_model->snapshot->files[index].path;
            const double barArea = std::min(160.0, row.width() * 0.26);
            const double debtWidth = 86;
            text(QRectF(chip.right() + 10, row.top(), row.width() - barArea - debtWidth - 56, row.height()), name.isEmpty() ? QStringLiteral("(root)") : name,
                 12.5, false, hit == m_hover ? colors.accent : colors.text, Qt::AlignLeft | Qt::AlignVCenter);
            const QRectF track(row.right() - debtWidth - barArea - 8, row.center().y() - 3, barArea, 6);
            painter.setPen(Qt::NoPen);
            painter.setBrush(colors.faint);
            painter.drawRoundedRect(track, 3, 3);
            painter.setBrush(withAlpha(ratingColor(score.rating), 0.9));
            painter.drawRoundedRect(QRectF(track.left(), track.top(), std::max(6.0, track.width() * score.debt / std::max(1.0, maxDebt)), track.height()), 3, 3);
            text(QRectF(row.right() - debtWidth - 4, row.top(), debtWidth, row.height()), formatDebt(score.debt), 12, false, colors.muted, Qt::AlignRight | Qt::AlignVCenter);
        }
        return box.bottom();
    };
    right = rankedList(right, QStringLiteral("Modules with the most debt"), modules, true) + gap;
    right = rankedList(right, QStringLiteral("Files with the most debt"), files, false) + gap;
    m_contentHeight = int(std::max(left, right) + 6);
}

void DashboardView::paintEvent(QPaintEvent *)
{
    QPainter painter(viewport());
    const Colors colors = Colors::from(this);
    painter.fillRect(viewport()->rect(), colors.background);
    if (!m_model || !m_model->snapshot || m_model->snapshot->files.isEmpty()) {
        painter.setPen(colors.muted);
        painter.setFont(fontPx(this, 13));
        painter.drawText(viewport()->rect(), Qt::AlignCenter, QStringLiteral("Open or clone a repository, or pick a folder to analyze."));
        return;
    }
    painter.translate(0, -verticalScrollBar()->value());
    paintContent(painter, viewport()->width());
}

int DashboardView::hitAt(const QPoint &position) const
{
    const QPointF point(position.x(), position.y() + verticalScrollBar()->value());
    for (int i = 0; i < m_hits.size(); ++i) {
        if (m_hits[i].rect.contains(point))
            return i;
    }
    return -1;
}

void DashboardView::mouseMoveEvent(QMouseEvent *event)
{
    const int hover = hitAt(event->position().toPoint());
    if (hover == m_hover)
        return;
    m_hover = hover;
    viewport()->setCursor(hover >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    viewport()->update();
}

void DashboardView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;
    const int hit = hitAt(event->position().toPoint());
    if (hit >= 0 && m_callbacks.select)
        m_callbacks.select(m_hits[hit].selection);
}

void DashboardView::mouseDoubleClickEvent(QMouseEvent *event)
{
    const int hit = hitAt(event->position().toPoint());
    if (hit < 0)
        return;
    const Selection selection = m_hits[hit].selection;
    if (selection.kind == SelectionKind::File && m_callbacks.open)
        m_callbacks.open(selection.index, 0);
    else if (selection.kind == SelectionKind::Module && m_callbacks.showModule)
        m_callbacks.showModule(selection.index);
}

void DashboardView::leaveEvent(QEvent *event)
{
    QAbstractScrollArea::leaveEvent(event);
    if (m_hover >= 0) {
        m_hover = -1;
        viewport()->update();
    }
}

RulesView::RulesView(QWidget *parent)
    : QWidget(parent)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 8, 20, 12);
    layout->setSpacing(8);
    m_filter = new QLineEdit;
    m_filter->setPlaceholderText(QStringLiteral("Filter rules, files and messages"));
    m_filter->setClearButtonEnabled(true);
    m_filter->setAccessibleName(QStringLiteral("Filter rules"));
    layout->addWidget(m_filter);
    auto *split = new QSplitter(Qt::Vertical);
    split->setChildrenCollapsible(false);
    m_tree = new QTreeWidget;
    m_tree->setColumnCount(3);
    m_tree->setHeaderLabels({QStringLiteral("Rule"), QStringLiteral("Violations"), QStringLiteral("Debt")});
    m_tree->setRootIsDecorated(true);
    m_tree->setUniformRowHeights(true);
    m_tree->setAlternatingRowColors(false);
    m_tree->header()->setStretchLastSection(false);
    m_tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_tree->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_tree->header()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    m_tree->setFrameShape(QFrame::NoFrame);
    split->addWidget(m_tree);
    m_description = new QTextBrowser;
    m_description->setOpenLinks(false);
    m_description->setFrameShape(QFrame::NoFrame);
    m_description->setMinimumHeight(120);
    split->addWidget(m_description);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    layout->addWidget(split, 1);

    connect(m_filter, &QLineEdit::textChanged, this, [this](const QString &text) {
        m_text = text.trimmed();
        rebuild();
    });
    connect(m_tree, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) { populate(item); });
    connect(m_tree, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem *item, QTreeWidgetItem *) {
        if (!item || !m_model)
            return;
        const int type = item->data(0, Qt::UserRole).toInt();
        const int rule = item->data(0, Qt::UserRole + 1).toInt();
        if (type == 1 || type == 2)
            describe(rule);
        if (type == 2 && m_callbacks.select) {
            const Violation &v = m_model->report.rules[rule].violations[item->data(0, Qt::UserRole + 2).toInt()];
            if (m_model->report.rules[rule].moduleScope && v.module >= 0)
                m_callbacks.select({SelectionKind::Module, v.module});
            else if (v.file >= 0)
                m_callbacks.select({SelectionKind::File, v.file});
        }
    });
    connect(m_tree, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem *item, int) {
        if (!item || !m_model || item->data(0, Qt::UserRole).toInt() != 2)
            return;
        const int rule = item->data(0, Qt::UserRole + 1).toInt();
        const Violation &v = m_model->report.rules[rule].violations[item->data(0, Qt::UserRole + 2).toInt()];
        if (v.file >= 0 && m_callbacks.open)
            m_callbacks.open(v.file, v.line);
        else if (v.module >= 0 && m_callbacks.showModule)
            m_callbacks.showModule(v.module);
    });
}

void RulesView::setModel(const ModelPtr &model)
{
    m_model = model;
    rebuild();
}

void RulesView::setFilter(const QString &filter)
{
    if (m_filter->text() != filter)
        m_filter->setText(filter);
}

void RulesView::focusFilter()
{
    m_filter->setFocus(Qt::ShortcutFocusReason);
    m_filter->selectAll();
}

int RulesView::visibleRules() const
{
    int count = 0;
    for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
        count += m_tree->topLevelItem(i)->childCount();
    return count;
}

bool RulesView::violationMatches(const Violation &v) const
{
    if (m_text.isEmpty())
        return true;
    if (v.message.contains(m_text, Qt::CaseInsensitive))
        return true;
    if (v.file >= 0 && v.file < m_model->snapshot->files.size() && m_model->snapshot->files[v.file].path.contains(m_text, Qt::CaseInsensitive))
        return true;
    if (v.module >= 0 && v.module < m_model->report.graph.nodes.size()
        && m_model->report.graph.nodes[v.module].path.contains(m_text, Qt::CaseInsensitive))
        return true;
    return false;
}

void RulesView::rebuild()
{
    m_tree->clear();
    m_description->clear();
    if (!m_model)
        return;
    const Report &report = m_model->report;
    const QLocale locale;
    const QFont bold = [this] {
        QFont font = m_tree->font();
        font.setBold(true);
        return font;
    }();
    QTreeWidgetItem *firstRule = nullptr;
    for (int c = 0; c < 4; ++c) {
        const Category category = Category(c);
        QTreeWidgetItem *group = nullptr;
        int count = 0;
        double debt = 0;
        for (int r = 0; r < report.rules.size(); ++r) {
            const Rule &rule = report.rules[r];
            if (rule.category != category)
                continue;
            const bool ruleMatch = m_text.isEmpty() || rule.title.contains(m_text, Qt::CaseInsensitive)
                                   || rule.id.contains(m_text, Qt::CaseInsensitive);
            int matching = rule.violations.size();
            double matchingDebt = rule.debt;
            if (!ruleMatch) {
                matching = 0;
                matchingDebt = 0;
                for (const Violation &v : rule.violations) {
                    if (violationMatches(v)) {
                        ++matching;
                        matchingDebt += v.debt;
                    }
                }
                if (matching == 0)
                    continue;
            }
            if (!group) {
                group = new QTreeWidgetItem(m_tree);
                group->setFont(0, bold);
                group->setData(0, Qt::UserRole, 0);
                group->setFirstColumnSpanned(false);
            }
            auto *item = new QTreeWidgetItem(group);
            item->setText(0, QStringLiteral("%1  %2").arg(rule.id, rule.title));
            item->setText(1, locale.toString(matching));
            item->setText(2, formatDebt(matchingDebt));
            item->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            item->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            item->setIcon(0, dotIcon(rule.violations.isEmpty() ? QColor(0x3f, 0xb9, 0x8f) : severityColor(Severity(rule.worst()))));
            item->setToolTip(0, rule.violations.isEmpty() ? QStringLiteral("No violations") : severityName(Severity(rule.worst())));
            item->setData(0, Qt::UserRole, 1);
            item->setData(0, Qt::UserRole + 1, r);
            item->setData(0, Qt::UserRole + 3, ruleMatch);
            if (matching > 0) {
                auto *placeholder = new QTreeWidgetItem(item);
                placeholder->setData(0, Qt::UserRole, 3);
            }
            if (!firstRule && matching > 0)
                firstRule = item;
            count += matching;
            debt += matchingDebt;
        }
        if (group) {
            group->setText(0, categoryName(category));
            group->setText(1, locale.toString(count));
            group->setText(2, formatDebt(debt));
            group->setTextAlignment(1, Qt::AlignRight | Qt::AlignVCenter);
            group->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
            group->setExpanded(true);
        }
    }
    if (firstRule) {
        m_tree->setCurrentItem(firstRule);
        describe(firstRule->data(0, Qt::UserRole + 1).toInt());
    }
}

void RulesView::populate(QTreeWidgetItem *ruleItem)
{
    if (!m_model || !ruleItem || ruleItem->data(0, Qt::UserRole).toInt() != 1)
        return;
    if (ruleItem->childCount() != 1 || ruleItem->child(0)->data(0, Qt::UserRole).toInt() != 3)
        return;
    delete ruleItem->takeChild(0);
    const int r = ruleItem->data(0, Qt::UserRole + 1).toInt();
    const bool all = ruleItem->data(0, Qt::UserRole + 3).toBool();
    const Rule &rule = m_model->report.rules[r];
    const auto &files = m_model->snapshot->files;
    const auto &nodes = m_model->report.graph.nodes;
    int shown = 0;
    int hidden = 0;
    QList<QTreeWidgetItem *> children;
    for (int i = 0; i < rule.violations.size(); ++i) {
        const Violation &v = rule.violations[i];
        if (!all && !violationMatches(v))
            continue;
        if (shown >= 500) {
            ++hidden;
            continue;
        }
        QString where;
        if (rule.moduleScope && v.module >= 0)
            where = nodes[v.module].path;
        else if (v.file >= 0)
            where = files[v.file].path + (v.line > 0 ? QStringLiteral(":%1").arg(v.line) : QString());
        auto *child = new QTreeWidgetItem;
        child->setText(0, QStringLiteral("%1 — %2").arg(where, v.message));
        child->setToolTip(0, QStringLiteral("%1\n%2\nDouble-click to open").arg(where, v.message));
        child->setText(2, formatDebt(v.debt));
        child->setTextAlignment(2, Qt::AlignRight | Qt::AlignVCenter);
        child->setIcon(0, dotIcon(severityColor(v.severity)));
        child->setData(0, Qt::UserRole, 2);
        child->setData(0, Qt::UserRole + 1, r);
        child->setData(0, Qt::UserRole + 2, i);
        children.append(child);
        ++shown;
    }
    if (hidden > 0) {
        auto *more = new QTreeWidgetItem;
        more->setText(0, QStringLiteral("and %1 more — export the report for the full list").arg(hidden));
        more->setData(0, Qt::UserRole, 4);
        children.append(more);
    }
    ruleItem->addChildren(children);
}

void RulesView::describe(int r)
{
    if (!m_model || r < 0 || r >= m_model->report.rules.size())
        return;
    const Rule &rule = m_model->report.rules[r];
    const Colors colors = Colors::from(this);
    const QColor color = rule.violations.isEmpty() ? colors.healthy : severityColor(Severity(rule.worst()));
    QString html = QStringLiteral("<div style=\"font-size:15px;font-weight:600\">%1</div>").arg(rule.title.toHtmlEscaped());
    html += QStringLiteral("<div style=\"color:%1\">%2 · %3 · default severity <span style=\"color:%4\">%5</span> · %6 violations · %7</div>")
                .arg(colors.muted.name(), rule.id, categoryName(rule.category), color.name(), severityName(rule.severity))
                .arg(rule.violations.size())
                .arg(formatDebt(rule.debt));
    html += QStringLiteral("<p>%1</p><p><b>How to fix.</b> %2</p>").arg(rule.description.toHtmlEscaped(), rule.howToFix.toHtmlEscaped());
    m_description->setHtml(html);
}

}
