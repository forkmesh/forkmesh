#include "BinarySizeMap.h"

#include <QFontMetricsF>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QToolTip>

#include <algorithm>

using namespace forkmesh::binarysize;

namespace {

constexpr double kGap = 1.0;
constexpr double kHeader = 20.0;
constexpr double kLegendHeight = 26.0;
constexpr double kMinNested = 56.0;

const char *const kLightHues[] = {"#2a78d6", "#eb6834", "#1baf7a", "#eda100",
                                  "#e87ba4", "#008300", "#4a3aa7", "#e34948", "#9a9994"};
const char *const kDarkHues[] = {"#3987e5", "#d95926", "#199e70", "#c98500",
                                 "#d55181", "#008300", "#9085e9", "#e66767", "#6f6e69"};

QColor mix(const QColor &a, const QColor &b, double t)
{
    return QColor::fromRgbF(float(a.redF() * t + b.redF() * (1 - t)),
                            float(a.greenF() * t + b.greenF() * (1 - t)),
                            float(a.blueF() * t + b.blueF() * (1 - t)));
}

QString share(qint64 part, qint64 whole)
{
    if (whole <= 0)
        return QString();
    const double percent = 100.0 * double(part) / double(whole);
    return QStringLiteral("%1%").arg(percent, 0, 'f', percent < 10 ? 1 : 0);
}

}

BinarySizeMap::BinarySizeMap(QWidget *parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 220);
}

void BinarySizeMap::setReport(Report report)
{
    m_report = std::move(report);
    m_focus = 0;
    m_hover = -1;
    relayout();
    update();
    if (m_onFocusChanged)
        m_onFocusChanged(m_focus);
}

void BinarySizeMap::setFocusNode(int node)
{
    if (node < 0 || node >= m_report.nodes.size() || node == m_focus)
        return;
    m_focus = node;
    m_hover = -1;
    relayout();
    update();
    if (m_onFocusChanged)
        m_onFocusChanged(m_focus);
}

bool BinarySizeMap::zoomOut()
{
    if (m_focus <= 0 || m_focus >= m_report.nodes.size())
        return false;
    setFocusNode(std::max(0, m_report.nodes.at(m_focus).parent));
    return true;
}

QVector<int> BinarySizeMap::chainTo(int node) const
{
    QVector<int> chain;
    while (node >= 0 && node < m_report.nodes.size()) {
        chain.prepend(node);
        node = m_report.nodes.at(node).parent;
    }
    return chain;
}

QVector<QRectF> BinarySizeMap::squarify(const QVector<double> &weights, const QRectF &area)
{
    QVector<QRectF> rects(weights.size(), QRectF(area.topLeft(), QSizeF(0, 0)));
    double total = 0;
    for (double weight : weights)
        total += std::max(0.0, weight);
    if (!(total > 0) || area.width() <= 0 || area.height() <= 0)
        return rects;
    const double scale = area.width() * area.height() / total;
    QRectF rest = area;
    int i = 0;
    while (i < weights.size()) {
        const double side = std::max(std::min(rest.width(), rest.height()), 1e-9);
        double sum = 0, smallest = 1e300, largest = 0, best = 1e300;
        int j = i;
        while (j < weights.size()) {
            const double cell = std::max(0.0, weights.at(j)) * scale;
            const double nextSum = sum + cell;
            const double nextMin = std::min(smallest, cell);
            const double nextMax = std::max(largest, cell);
            const double worst = std::max(side * side * nextMax / (nextSum * nextSum),
                                          nextSum * nextSum / (side * side * std::max(nextMin, 1e-12)));
            if (j > i && worst > best)
                break;
            best = worst;
            sum = nextSum;
            smallest = nextMin;
            largest = nextMax;
            ++j;
        }
        if (rest.width() >= rest.height()) {
            const double stripWidth = std::min(rest.width(), sum / std::max(rest.height(), 1e-9));
            double y = rest.top();
            for (int k = i; k < j; ++k) {
                const double h = stripWidth > 0 ? std::max(0.0, weights.at(k)) * scale / stripWidth : 0;
                rects[k] = QRectF(rest.left(), y, stripWidth, h);
                y += h;
            }
            rest.setLeft(rest.left() + stripWidth);
        } else {
            const double stripHeight = std::min(rest.height(), sum / std::max(rest.width(), 1e-9));
            double x = rest.left();
            for (int k = i; k < j; ++k) {
                const double w = stripHeight > 0 ? std::max(0.0, weights.at(k)) * scale / stripHeight : 0;
                rects[k] = QRectF(x, rest.top(), w, stripHeight);
                x += w;
            }
            rest.setTop(rest.top() + stripHeight);
        }
        i = j;
    }
    return rects;
}

void BinarySizeMap::relayout()
{
    m_cells.clear();
    m_legend = QRectF();
    if (m_report.nodes.isEmpty())
        return;
    const QRectF whole = QRectF(rect());
    m_legend = QRectF(whole.left(), whole.bottom() - kLegendHeight, whole.width(), kLegendHeight);
    const QRectF area = whole.adjusted(0, 0, 0, -kLegendHeight - 4);
    if (m_report.nodes.at(m_focus).children.isEmpty()) {
        m_cells.append({m_focus, area.adjusted(kGap, kGap, -kGap, -kGap), 1});
        return;
    }
    layoutChildren(m_focus, area, 1);
}

void BinarySizeMap::layoutChildren(int parent, const QRectF &area, int depth)
{
    const QVector<int> &children = m_report.nodes.at(parent).children;
    QVector<double> weights;
    weights.reserve(children.size());
    for (int child : children)
        weights.append(double(m_report.nodes.at(child).bytes));
    const QVector<QRectF> rects = squarify(weights, area);
    for (int i = 0; i < children.size(); ++i) {
        const QRectF cell = rects.at(i).adjusted(kGap, kGap, -kGap, -kGap);
        if (cell.width() < 1.5 || cell.height() < 1.5)
            continue;
        m_cells.append({children.at(i), cell, depth});
        if (depth == 1 && !m_report.nodes.at(children.at(i)).children.isEmpty() &&
            cell.width() >= kMinNested && cell.height() >= kMinNested)
            layoutChildren(children.at(i), cell.adjusted(3, kHeader, -3, -3), 2);
    }
}

bool BinarySizeMap::dark() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

QColor BinarySizeMap::categoryColor(int category) const
{
    const int slot = std::clamp(category, 0, int(Other));
    return QColor(QLatin1String(dark() ? kDarkHues[slot] : kLightHues[slot]));
}

void BinarySizeMap::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::TextAntialiasing, true);
    painter.fillRect(rect(), palette().color(QPalette::Base));
    if (m_report.nodes.isEmpty()) {
        painter.setPen(palette().color(QPalette::Disabled, QPalette::Text));
        painter.drawText(rect(), Qt::AlignCenter,
                         m_report.error.isEmpty()
                             ? QStringLiteral("No scan yet")
                             : QStringLiteral("Could not read the binary: %1").arg(m_report.error));
        return;
    }
    for (const Cell &cell : std::as_const(m_cells))
        paintCell(painter, cell);
    paintLegend(painter);
}

void BinarySizeMap::paintCell(QPainter &painter, const Cell &cell) const
{
    const Node &node = m_report.nodes.at(cell.node);
    const QColor base = palette().color(QPalette::Base);
    const QColor text = palette().color(QPalette::Text);
    QColor secondary = text;
    secondary.setAlphaF(0.68f);
    const QColor hue = categoryColor(node.category);
    const bool leaf = node.children.isEmpty() || cell.depth == 2 ||
                      cell.rect.width() < kMinNested || cell.rect.height() < kMinNested;
    const bool hovered = cell.node == m_hover;

    const double strength = leaf ? (dark() ? 0.55 : 0.42) : (dark() ? 0.22 : 0.14);
    painter.setPen(QPen(mix(hue, base, hovered ? 1.0 : 0.75), hovered ? 2.0 : 1.0));
    painter.setBrush(mix(hue, base, hovered ? strength + 0.12 : strength));
    const double radius = cell.depth == 1 ? 4.0 : 2.0;
    painter.drawRoundedRect(cell.rect.adjusted(0.5, 0.5, -0.5, -0.5), radius, radius);

    if (cell.depth == 1 ? cell.rect.width() < 34 || cell.rect.height() < 14
                        : cell.rect.width() < 48 || cell.rect.height() < 26)
        return;
    QFont nameFont = font();
    nameFont.setWeight(cell.depth == 1 ? QFont::DemiBold : QFont::Normal);
    if (cell.depth == 2)
        nameFont.setPointSizeF(std::max(6.0, nameFont.pointSizeF() - 1.0));
    QFont sizeFont = nameFont;
    sizeFont.setWeight(QFont::Normal);
    sizeFont.setPointSizeF(std::max(6.0, nameFont.pointSizeF() - 1.0));
    const QFontMetricsF nameMetrics(nameFont);
    const QFontMetricsF sizeMetrics(sizeFont);
    const QRectF inner = cell.rect.adjusted(5, 2, -5, -2);
    const QString size = formatBytes(node.bytes);

    const double lineHeight = std::min(inner.height(), nameMetrics.height() + 2);
    const QRectF firstLine(inner.left(), inner.top(), inner.width(), lineHeight);
    painter.setPen(text);
    painter.setFont(nameFont);
    const bool sizeBeside = !leaf || inner.height() < lineHeight + sizeMetrics.height();
    const double sizeWidth = sizeBeside ? sizeMetrics.horizontalAdvance(size) + 8 : 0;
    const double nameWidth = inner.width() - (sizeWidth < inner.width() * 0.6 ? sizeWidth : 0);
    const QString name = nameMetrics.elidedText(
        node.name, m_focus == 0 ? Qt::ElideRight : Qt::ElideLeft, nameWidth);
    if (name.size() < 3)
        return;
    painter.drawText(QRectF(firstLine.left(), firstLine.top(), nameWidth, lineHeight),
                     Qt::AlignLeft | Qt::AlignVCenter, name);
    painter.setFont(sizeFont);
    painter.setPen(secondary);
    if (sizeBeside) {
        if (nameWidth < inner.width())
            painter.drawText(firstLine, Qt::AlignRight | Qt::AlignVCenter, size);
    } else {
        painter.drawText(QRectF(inner.left(), inner.top() + lineHeight, inner.width(),
                                sizeMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         sizeMetrics.elidedText(size + QStringLiteral(" · ") +
                                                    share(node.bytes, m_report.nodes.at(m_focus).bytes),
                                                Qt::ElideRight, inner.width()));
    }
}

void BinarySizeMap::paintLegend(QPainter &painter) const
{
    const Node &root = m_report.nodes.at(0);
    QVector<int> present = root.children;
    std::sort(present.begin(), present.end(), [this](int a, int b) {
        return m_report.nodes.at(a).category < m_report.nodes.at(b).category;
    });
    QFont legendFont = font();
    legendFont.setPointSizeF(std::max(6.0, legendFont.pointSizeF() - 1.0));
    painter.setFont(legendFont);
    const QFontMetricsF metrics(legendFont);
    double x = m_legend.left() + 2;
    const double middle = m_legend.center().y();
    for (int index : std::as_const(present)) {
        const Node &node = m_report.nodes.at(index);
        const QString label = QStringLiteral("%1 %2").arg(node.name, formatBytes(node.bytes));
        const double width = 14 + metrics.horizontalAdvance(label) + 14;
        if (x + width > m_legend.right())
            break;
        painter.setPen(Qt::NoPen);
        painter.setBrush(categoryColor(node.category));
        painter.drawRoundedRect(QRectF(x, middle - 5, 10, 10), 2, 2);
        painter.setPen(palette().color(QPalette::Text));
        painter.drawText(QRectF(x + 14, m_legend.top(), width - 14, m_legend.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, label);
        x += width;
    }
}

int BinarySizeMap::nodeAt(const QPoint &position) const
{
    int hit = -1;
    for (const Cell &cell : m_cells) {
        if (cell.rect.contains(position))
            hit = cell.node;
    }
    return hit;
}

QRectF BinarySizeMap::cellRect(int node) const
{
    for (const Cell &cell : m_cells) {
        if (cell.node == node)
            return cell.rect;
    }
    return QRectF();
}

QString BinarySizeMap::tooltipFor(int node) const
{
    const Node &item = m_report.nodes.at(node);
    QString tip = item.name;
    if (!item.detail.isEmpty() && item.detail != item.name)
        tip += QStringLiteral("\n") + item.detail;
    tip += QStringLiteral("\n%1 · %2 of the binary")
               .arg(formatBytes(item.bytes), share(item.bytes, m_report.fileSize));
    if (item.parent > 0) {
        const Node &parent = m_report.nodes.at(item.parent);
        tip += QStringLiteral(" · %1 of %2").arg(share(item.bytes, parent.bytes), parent.name);
    }
    tip += QStringLiteral("\n") + categoryName(item.category);
    if (!item.children.isEmpty())
        tip += QStringLiteral("\n%1 parts · click to zoom in").arg(item.children.size());
    if (m_focus > 0)
        tip += QStringLiteral("\nRight-click or Backspace to zoom out");
    return tip;
}

void BinarySizeMap::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    relayout();
}

void BinarySizeMap::mouseMoveEvent(QMouseEvent *event)
{
    const int hit = nodeAt(event->position().toPoint());
    if (hit == m_hover)
        return;
    m_hover = hit;
    const bool zoomable = hit >= 0 && hit != m_focus &&
                          !m_report.nodes.at(hit).children.isEmpty();
    setCursor(zoomable ? Qt::PointingHandCursor : Qt::ArrowCursor);
    update();
}

void BinarySizeMap::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::RightButton || event->button() == Qt::BackButton) {
        zoomOut();
        return;
    }
    if (event->button() != Qt::LeftButton)
        return;
    int target = nodeAt(event->position().toPoint());
    while (target > 0 && target != m_focus && m_report.nodes.at(target).children.isEmpty())
        target = m_report.nodes.at(target).parent;
    if (target > 0 && target != m_focus)
        setFocusNode(target);
}

void BinarySizeMap::keyPressEvent(QKeyEvent *event)
{
    if ((event->key() == Qt::Key_Backspace || event->key() == Qt::Key_Escape) && zoomOut())
        return;
    QWidget::keyPressEvent(event);
}

void BinarySizeMap::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_hover < 0)
        return;
    m_hover = -1;
    unsetCursor();
    update();
}

void BinarySizeMap::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::FontChange)
        update();
}

bool BinarySizeMap::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const int hit = nodeAt(help->pos());
        if (hit >= 0)
            QToolTip::showText(help->globalPos(), tooltipFor(hit), this);
        else
            QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}
