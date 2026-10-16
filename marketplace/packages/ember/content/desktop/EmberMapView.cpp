#include "EmberMapView.h"

#include <QFontMetricsF>
#include <QHelpEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRadialGradient>
#include <QLinearGradient>
#include <QToolTip>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace forkmesh::ember {

namespace {

constexpr int kTransitionMs = 450;
constexpr int kLinkMs = 420;
constexpr int kMaxFileLabels = 700;
constexpr int kMaxFolderLabels = 90;
constexpr int kMaxLinks = 40;

QColor withAlpha(QColor color, int alpha)
{
    color.setAlpha(std::clamp(alpha, 0, 255));
    return color;
}

QColor lighten(const QColor &color, double amount)
{
    return mix(color, QColor(255, 255, 255), amount);
}

QColor darken(const QColor &color, double amount)
{
    return mix(color, QColor(0, 0, 0), amount);
}

void haloText(QPainter &painter, const QRectF &rect, int flags, const QString &text,
              const QColor &color, const QColor &halo, bool withHalo)
{
    if (withHalo) {
        painter.setPen(halo);
        static const QPointF offsets[] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}, {0.7, 0.7}};
        for (const QPointF &offset : offsets)
            painter.drawText(rect.translated(offset), flags, text);
    }
    painter.setPen(color);
    painter.drawText(rect, flags, text);
}

QPointF bezier(const QPointF &a, const QPointF &c, const QPointF &b, double t)
{
    const double u = 1 - t;
    return a * (u * u) + c * (2 * u * t) + b * (t * t);
}

}

EmberMapView::EmberMapView(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("emberMap"));
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 260);
    setAttribute(Qt::WA_OpaquePaintEvent);
    m_timer.setInterval(16);
    m_timer.setTimerType(Qt::PreciseTimer);
    connect(&m_timer, &QTimer::timeout, this, [this] { tick(); });
    m_settleTimer.setSingleShot(true);
    m_settleTimer.setInterval(150);
    connect(&m_settleTimer, &QTimer::timeout, this, [this] { settle(); });
    m_clock.start();
}

bool EmberMapView::dark() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

QColor EmberMapView::background() const
{
    const QColor base = palette().color(QPalette::Base);
    return dark() ? mix(base, QColor(0, 0, 0), 0.15) : mix(base, palette().color(QPalette::Window), 0.5);
}

void EmberMapView::setData(insight::SnapshotPtr snapshot, MapLayoutPtr layout,
                           std::shared_ptr<const CouplingIndex> coupling)
{
    m_snapshot = std::move(snapshot);
    m_layout = std::move(layout);
    m_coupling = std::move(coupling);
    m_selected = -1;
    m_hover = -1;
    m_focus = 0;
    m_transition = false;
    m_message.clear();
    if (m_layout && !m_layout->isEmpty())
        m_view = fitTransform(m_layout->nodes[0], size());
    recolor();
    invalidate();
    if (m_onFocus)
        m_onFocus(m_focus);
}

void EmberMapView::clearData()
{
    m_snapshot.reset();
    m_layout.reset();
    m_coupling.reset();
    m_selected = -1;
    m_hover = -1;
    m_focus = 0;
    m_transition = false;
    m_coloring = TabColoring();
    m_fill.clear();
    m_light.clear();
    m_shade.clear();
    invalidate();
}

void EmberMapView::setColoring(const TabColoring &coloring)
{
    m_coloring = coloring;
    recolor();
    invalidate();
    updateTimer();
}

void EmberMapView::setMessage(const QString &text)
{
    m_message = text;
    invalidate();
}

void EmberMapView::recolor()
{
    const int n = m_coloring.files.size();
    m_colorDark = dark();
    const QColor bg = background();
    m_fill.resize(n);
    m_light.resize(n);
    m_shade.resize(n);
    for (int i = 0; i < n; ++i) {
        const FileStyle &style = m_coloring.files[i];
        QColor fill = style.color.isValid() ? style.color : neutralColor();
        if (style.dimmed)
            fill = mix(fill, bg, m_colorDark ? 0.74 : 0.7);
        m_fill[i] = fill;
        m_light[i] = lighten(fill, style.dimmed ? 0.06 : 0.3);
        m_shade[i] = darken(fill, style.dimmed ? 0.06 : 0.28);
    }
}

void EmberMapView::invalidate()
{
    m_dirty = true;
    update();
}

QPointF EmberMapView::toScreen(const QPointF &world) const
{
    return QPointF((world.x() - m_view.cx) * m_view.k + width() / 2.0,
                   (world.y() - m_view.cy) * m_view.k + height() / 2.0);
}

QPointF EmberMapView::toWorld(const QPointF &screen) const
{
    return QPointF((screen.x() - width() / 2.0) / m_view.k + m_view.cx,
                   (screen.y() - height() / 2.0) / m_view.k + m_view.cy);
}

double EmberMapView::screenRadius(int node) const
{
    if (!m_layout || node < 0 || node >= m_layout->nodes.size())
        return 0;
    return m_layout->nodes[node].r * m_view.k;
}

QVector<int> EmberMapView::focusChain() const
{
    return m_layout ? m_layout->chain(m_focus) : QVector<int>();
}

double EmberMapView::minScale() const
{
    if (!m_layout || m_layout->isEmpty())
        return 1e-6;
    return fitTransform(m_layout->nodes[0], size()).k * 0.6;
}

double EmberMapView::maxScale() const
{
    return std::max(minScale() * 2, 90.0);
}

void EmberMapView::focusNode(int node, bool animate)
{
    if (!m_layout || node < 0 || node >= m_layout->nodes.size())
        return;
    if (!m_layout->isFolder(node))
        node = std::max(0, m_layout->nodes[node].parent);
    m_focus = node;
    const ViewTransform target = fitTransform(m_layout->nodes[node], size());
    if (animate && isVisible())
        startTransition(target);
    else {
        m_transition = false;
        m_view = target;
        invalidate();
    }
    if (m_onFocus)
        m_onFocus(m_focus);
}

void EmberMapView::zoomOut()
{
    if (!m_layout || m_layout->isEmpty())
        return;
    const int parent = m_layout->nodes[m_focus].parent;
    focusNode(parent >= 0 ? parent : 0, true);
}

void EmberMapView::zoomBy(double factor)
{
    if (!m_layout || m_layout->isEmpty())
        return;
    ViewTransform target = m_transition ? m_to : m_view;
    target.k = std::clamp(target.k * factor, minScale(), maxScale());
    startTransition(target);
}

void EmberMapView::setSelectedFile(int file, bool zoomTo)
{
    if (!m_snapshot || file >= m_snapshot->files.size())
        file = -1;
    const bool changed = file != m_selected;
    m_selected = file;
    if (changed) {
        m_linkProgress = 0;
        m_lastTick = m_clock.elapsed();
    }
    if (file >= 0 && zoomTo && m_layout) {
        const int node = m_layout->fileNode(file);
        const QSizeF viewport = size();
        const double r = screenRadius(node);
        const QPointF p = toScreen(QPointF(m_layout->nodes[node].x, m_layout->nodes[node].y));
        const bool visible = r >= 20 && rect().adjusted(40, 40, -40, -40).contains(p.toPoint());
        if (!visible)
            focusNode(readableFocus(*m_layout, node, viewport), true);
    }
    updateTimer();
    update();
}

void EmberMapView::finishAnimation()
{
    if (m_transition) {
        m_transition = false;
        m_view = m_to;
    }
    m_linkProgress = 1;
    m_fast = false;
    invalidate();
    updateTimer();
}

void EmberMapView::startTransition(const ViewTransform &target)
{
    m_from = m_view;
    m_to = target;
    m_transition = true;
    m_fast = true;
    m_transitionClock.start();
    m_lastTick = m_clock.elapsed();
    updateTimer();
    update();
}

void EmberMapView::updateTimer()
{
    const bool links = m_selected >= 0 && m_linkProgress < 1;
    const bool glow = m_coloring.tab == Tab::Hotspots && m_coloring.glowing > 0 && m_glowVisible;
    const bool run = isVisible() && (m_transition || links || glow);
    if (run && !m_timer.isActive()) {
        m_lastTick = m_clock.elapsed();
        m_timer.start();
    } else if (!run && m_timer.isActive()) {
        m_timer.stop();
    }
}

void EmberMapView::tick()
{
    const qint64 now = m_clock.elapsed();
    const double dt = std::max<qint64>(0, now - m_lastTick);
    m_lastTick = now;
    if (m_transition) {
        const double t = std::min(1.0, m_transitionClock.elapsed() / double(kTransitionMs));
        m_view = interpolateView(m_from, m_to, easeInOut(t), size());
        if (t >= 1) {
            m_transition = false;
            m_view = m_to;
            m_fast = false;
            m_dirty = true;
        }
    }
    if (m_linkProgress < 1)
        m_linkProgress = std::min(1.0, m_linkProgress + dt / kLinkMs);
    update();
    updateTimer();
}

void EmberMapView::beginInteraction()
{
    m_fast = true;
    m_dirty = true;
    m_settleTimer.start();
    update();
}

void EmberMapView::settle()
{
    if (m_pressed && m_dragging) {
        m_settleTimer.start();
        return;
    }
    m_fast = false;
    updateFocusFromView();
    invalidate();
}

void EmberMapView::updateFocusFromView()
{
    if (!m_layout || m_layout->isEmpty())
        return;
    const double need = 0.42 * std::min(width(), height());
    const QVector<int> chain = m_layout->chain(m_layout->nodeAt(toWorld(rect().center())));
    int focus = 0;
    for (int node : chain) {
        if (m_layout->isFolder(node) && screenRadius(node) >= need)
            focus = node;
    }
    if (focus != m_focus) {
        m_focus = focus;
        if (m_onFocus)
            m_onFocus(m_focus);
    }
}

void EmberMapView::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    const bool currentDark = dark();
    if (currentDark != m_colorDark && !m_coloring.files.isEmpty())
        recolor();
    if (m_fast || m_transition) {
        paintScene(painter, true);
    } else {
        const qreal dpr = devicePixelRatioF();
        const QSize pixels = (QSizeF(size()) * dpr).toSize();
        if (m_dirty || m_static.size() != pixels) {
            m_static = QPixmap(pixels);
            m_static.setDevicePixelRatio(dpr);
            QPainter cache(&m_static);
            paintScene(cache, false);
            m_dirty = false;
        }
        painter.drawPixmap(0, 0, m_static);
    }
    paintOverlay(painter);
}

void EmberMapView::paintScene(QPainter &painter, bool fast)
{
    const QColor bg = background();
    painter.fillRect(rect(), bg);
    {
        QRadialGradient vignette(QPointF(width() * 0.45, height() * 0.4), std::max(width(), height()) * 0.75);
        vignette.setColorAt(0, dark() ? withAlpha(QColor(255, 255, 255), 9) : withAlpha(QColor(255, 255, 255), 90));
        vignette.setColorAt(1, withAlpha(QColor(0, 0, 0), dark() ? 30 : 8));
        painter.fillRect(rect(), vignette);
    }
    m_circlesDrawn = 0;
    m_labelsDrawn = 0;
    m_glowVisible = false;
    if (!m_layout || m_layout->isEmpty() || !m_snapshot) {
        if (!m_message.isEmpty())
            paintNote(painter, m_message, true);
        return;
    }
    painter.setRenderHint(QPainter::Antialiasing, true);
    const bool isDark = dark();
    const QRectF viewport = QRectF(rect()).adjusted(-2, -2, 2, 2);
    const QColor folderFill = isDark ? QColor(255, 255, 255, 10) : QColor(20, 30, 50, 10);
    const QColor folderRim = isDark ? QColor(255, 255, 255, 34) : QColor(20, 30, 50, 42);
    const QColor focusRim = isDark ? QColor(255, 255, 255, 60) : QColor(20, 30, 50, 70);
    const QColor shadow = isDark ? QColor(0, 0, 0, 90) : QColor(30, 40, 60, 40);
    const QColor markRing = isDark ? QColor(255, 255, 255, 200) : QColor(30, 35, 45, 200);

    QVector<int> stack;
    stack.reserve(256);
    stack.append(0);
    QVector<int> fileLabels;
    QVector<int> folderLabels;
    QVector<QPair<QRectF, int>> tiny;
    tiny.reserve(4096);
    const QColor neutral = neutralColor();
    const FileStyle noStyle;
    const auto &nodes = m_layout->nodes;
    while (!stack.isEmpty()) {
        const int index = stack.takeLast();
        const MapNode &node = nodes[index];
        const QPointF c = toScreen(QPointF(node.x, node.y));
        const double r = node.r * m_view.k;
        if (r < 0.6)
            continue;
        const QRectF box(c.x() - r, c.y() - r, 2 * r, 2 * r);
        if (!viewport.intersects(box))
            continue;
        if (r > 4 * std::hypot(width(), height())) {
            const double d = std::hypot(c.x() - width() / 2.0, c.y() - height() / 2.0);
            if (d + std::hypot(width(), height()) < r) {
                for (int i = node.children.size() - 1; i >= 0; --i)
                    stack.append(node.children[i]);
                continue;
            }
        }
        ++m_circlesDrawn;
        if (node.file < 0) {
            painter.setPen(index == m_focus ? QPen(focusRim, 1.2) : QPen(folderRim, 1));
            painter.setBrush(folderFill);
            if (r < 3)
                painter.setPen(Qt::NoPen);
            if (fast) {
                painter.setRenderHint(QPainter::Antialiasing, false);
                painter.drawEllipse(c, r, r);
                painter.setRenderHint(QPainter::Antialiasing, true);
            } else {
                painter.drawEllipse(c, r, r);
            }
            if (r >= 70 && index != 0 && folderLabels.size() < kMaxFolderLabels)
                folderLabels.append(index);
            for (int i = node.children.size() - 1; i >= 0; --i)
                stack.append(node.children[i]);
            continue;
        }
        const int file = node.file;
        const bool hasStyle = file < m_fill.size();
        if (r < 2.2) {
            tiny.append({QRectF(c.x() - r * 0.89, c.y() - r * 0.89, r * 1.78, r * 1.78), file});
            continue;
        }
        const QColor &fill = hasStyle ? m_fill[file] : neutral;
        const FileStyle &style = file < m_coloring.files.size() ? m_coloring.files[file] : noStyle;
        if (!fast && r >= 5 && !style.dimmed) {
            painter.setPen(Qt::NoPen);
            painter.setBrush(shadow);
            painter.drawEllipse(QPointF(c.x(), c.y() + std::max(1.0, r * 0.07)), r * 1.01, r * 1.01);
        }
        painter.setPen(Qt::NoPen);
        if (!fast && r >= 4 && hasStyle) {
            QRadialGradient g(QPointF(c.x() - r * 0.38, c.y() - r * 0.45), r * 1.45,
                              QPointF(c.x() - r * 0.38, c.y() - r * 0.45));
            g.setColorAt(0, m_light[file]);
            g.setColorAt(0.55, fill);
            g.setColorAt(1, m_shade[file]);
            painter.setBrush(g);
        } else {
            painter.setBrush(fill);
        }
        painter.drawEllipse(c, r, r);
        if (r >= 7 && !style.dimmed) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(withAlpha(darken(fill, 0.45), 110), 1));
            painter.drawEllipse(c, r - 0.5, r - 0.5);
        }
        if (style.marked && r >= 3.5) {
            QPen pen(markRing, std::clamp(r * 0.06, 1.0, 2.0), Qt::DashLine);
            pen.setDashPattern({2.0, 2.0});
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(c, r + 2.5, r + 2.5);
        }
        if (style.glow > 0 && r >= 2.5)
            m_glowVisible = true;
        if (r >= 18 && fileLabels.size() < kMaxFileLabels)
            fileLabels.append(index);
    }

    if (!tiny.isEmpty()) {
        painter.setRenderHint(QPainter::Antialiasing, false);
        for (const auto &dot : tiny)
            painter.fillRect(dot.first, dot.second < m_fill.size() ? m_fill[dot.second] : neutral);
        painter.setRenderHint(QPainter::Antialiasing, true);
    }
    paintLabels(painter, fileLabels, folderLabels, fast);
}

void EmberMapView::paintLabels(QPainter &painter, const QVector<int> &files,
                               const QVector<int> &folders, bool fast)
{
    const bool isDark = dark();
    const QColor halo = isDark ? QColor(8, 10, 14, 170) : QColor(255, 255, 255, 190);
    const QColor folderText = isDark ? QColor(230, 237, 243, 215) : QColor(31, 35, 40, 220);
    const auto &nodes = m_layout->nodes;
    QFont base = font();
    for (int index : folders) {
        const MapNode &node = nodes[index];
        const QPointF c = toScreen(QPointF(node.x, node.y));
        const double r = node.r * m_view.k;
        QFont f = base;
        f.setPixelSize(int(std::clamp(r * 0.05, 11.0, 14.0)));
        f.setWeight(QFont::DemiBold);
        const double baseline = r - QFontMetricsF(f).ascent() * 0.35 - std::max(3.0, r * 0.012);
        const QPointF top(c.x(), c.y() - baseline);
        if (!rect().adjusted(-40, -10, 40, 10).contains(top.toPoint()))
            continue;
        if (!m_legendRect.isEmpty() && m_legendRect.adjusted(-30, -6, 30, 10).contains(top))
            continue;
        paintCurvedLabel(painter, c, baseline, node.name, f, folderText, !fast);
        ++m_labelsDrawn;
    }
    QFont fonts[8];
    for (int s = 0; s < 8; ++s) {
        fonts[s] = base;
        fonts[s].setPixelSize(8 + s);
    }
    QFont metricFont = base;
    metricFont.setPixelSize(10);
    const QColor text(255, 255, 255);
    const QColor fileHalo(0, 0, 0, 120);
    const QColor muted(255, 255, 255, 200);
    for (int index : files) {
        const MapNode &node = nodes[index];
        const QPointF c = toScreen(QPointF(node.x, node.y));
        const double r = node.r * m_view.k;
        const bool dimmed = node.file < m_coloring.files.size() && m_coloring.files[node.file].dimmed;
        int s = int(std::clamp(r * 0.24 - 8, 0.0, 7.0));
        const double room = r * 1.86;
        while (s > 0 && QFontMetricsF(fonts[s]).horizontalAdvance(node.name) > room)
            --s;
        const QFont &f = fonts[s];
        const QFontMetricsF fm(f);
        const QString name = fm.elidedText(node.name, Qt::ElideRight, room);
        if (name.size() < 5 && name != node.name)
            continue;
        painter.setFont(f);
        const bool showMetric = r >= 38 && !fast;
        const double lineH = fm.height();
        const QRectF nameRect(c.x() - r, c.y() - lineH / 2 - (showMetric ? lineH * 0.45 : 0), 2 * r, lineH);
        const QColor color = dimmed ? (isDark ? QColor(230, 237, 243, 85) : QColor(31, 35, 40, 110)) : text;
        haloText(painter, nameRect, Qt::AlignCenter, name, color, dimmed ? halo : fileHalo, !fast && !dimmed);
        if (showMetric) {
            const QString metric = metricLabel(node.file);
            if (!metric.isEmpty()) {
                painter.setFont(metricFont);
                const QRectF metricRect(c.x() - r, nameRect.bottom() - 1, 2 * r, QFontMetricsF(metricFont).height());
                haloText(painter, metricRect, Qt::AlignCenter,
                         QFontMetricsF(metricFont).elidedText(metric, Qt::ElideRight, r * 1.5),
                         dimmed ? color : muted, fileHalo, !dimmed);
            }
        }
        ++m_labelsDrawn;
    }
}

void EmberMapView::paintCurvedLabel(QPainter &painter, const QPointF &center, double radius,
                                    const QString &text, const QFont &font, const QColor &color,
                                    bool halo)
{
    const QFontMetricsF fm(font);
    const double maxSpan = M_PI * 0.8;
    QString label = text;
    double width = fm.horizontalAdvance(label);
    if (width / radius > maxSpan) {
        label = fm.elidedText(text, Qt::ElideMiddle, maxSpan * radius);
        width = fm.horizontalAdvance(label);
    }
    if (label.isEmpty())
        return;
    painter.save();
    painter.setFont(font);
    const QColor haloColor = dark() ? QColor(8, 10, 14, 200) : QColor(255, 255, 255, 220);
    double angle = -M_PI / 2 - (width / radius) / 2;
    for (const QChar ch : label) {
        const double advance = fm.horizontalAdvance(ch);
        const double mid = angle + (advance / 2) / radius;
        const QPointF at(center.x() + radius * std::cos(mid), center.y() + radius * std::sin(mid));
        painter.save();
        painter.translate(at);
        painter.rotate((mid + M_PI / 2) * 180 / M_PI);
        const QRectF cell(-advance / 2 - 1, -fm.ascent() / 2 - 3, advance + 2, fm.height());
        haloText(painter, cell, Qt::AlignCenter, QString(ch), color, haloColor, halo);
        painter.restore();
        angle += advance / radius;
    }
    painter.restore();
}

QString EmberMapView::metricLabel(int file) const
{
    if (!m_snapshot || file < 0 || file >= m_snapshot->files.size())
        return {};
    const insight::FileInfo &f = m_snapshot->files[file];
    const QLocale locale;
    switch (m_coloring.tab) {
    case Tab::Hotspots:
        return f.commits > 0 ? QStringLiteral("%1 commits").arg(locale.toString(f.commits)) : QString();
    case Tab::RefactoringTargets:
    case Tab::CodeHealth:
        return QStringLiteral("health %1").arg(locale.toString(f.health, 'f', 1));
    case Tab::ChangeCoupling:
        if (m_coupling && file < m_coupling->size() && !(*m_coupling)[file].isEmpty())
            return QStringLiteral("%1 coupled").arg((*m_coupling)[file].size());
        return {};
    case Tab::Knowledge:
        return f.mainAuthor.isEmpty() ? QString()
                                      : QStringLiteral("%1 %2").arg(f.mainAuthor, percentText(f.mainAuthorShare));
    case Tab::Defects:
        return f.fixCommits > 0 ? QStringLiteral("%1 fixes").arg(f.fixCommits) : QString();
    case Tab::Languages:
        return f.language;
    }
    return {};
}

void EmberMapView::paintOverlay(QPainter &painter)
{
    painter.setRenderHint(QPainter::Antialiasing, true);
    if (m_layout && !m_layout->isEmpty() && m_snapshot) {
        const auto &nodes = m_layout->nodes;
        if (m_coloring.tab == Tab::Hotspots && m_coloring.glowing > 0) {
            const double phase = m_clock.elapsed() / 1000.0 * 2 * M_PI / 2.4;
            const QColor glow(255, 123, 82);
            for (int i = 0; i < m_coloring.files.size(); ++i) {
                const FileStyle &style = m_coloring.files[i];
                if (style.glow <= 0 || style.dimmed)
                    continue;
                const int index = m_layout->fileNode(i);
                if (index < 0)
                    continue;
                const double r = nodes[index].r * m_view.k;
                if (r < 2.5)
                    continue;
                const QPointF c = toScreen(QPointF(nodes[index].x, nodes[index].y));
                const double breathe = 0.5 + 0.5 * std::sin(phase + i * 0.7);
                const double outer = r + std::max(6.0, r * (0.22 + 0.12 * breathe));
                if (!QRectF(rect()).adjusted(-outer, -outer, outer, outer).contains(c))
                    continue;
                QRadialGradient g(c, outer);
                const double inner = r / outer;
                g.setColorAt(std::max(0.0, inner - 0.02), withAlpha(glow, 0));
                g.setColorAt(inner, withAlpha(glow, int((90 + 90 * breathe) * style.glow)));
                g.setColorAt(1, withAlpha(glow, 0));
                painter.setPen(Qt::NoPen);
                painter.setBrush(g);
                painter.drawEllipse(c, outer, outer);
            }
        }
        if (m_hover >= 0 && m_hover < nodes.size() && m_hover != 0) {
            const MapNode &node = nodes[m_hover];
            const QPointF c = toScreen(QPointF(node.x, node.y));
            const double r = node.r * m_view.k;
            painter.setBrush(Qt::NoBrush);
            if (node.file >= 0)
                painter.setPen(QPen(dark() ? QColor(255, 255, 255, 220) : QColor(31, 35, 40, 200), 1.5));
            else
                painter.setPen(QPen(dark() ? QColor(255, 255, 255, 90) : QColor(31, 35, 40, 90), 1.5));
            painter.drawEllipse(c, r + 1, r + 1);
        }
        paintLinks(painter);
        paintBadges(painter);
    }
    paintLegend(painter);
    if (m_layout && !m_layout->isEmpty()) {
        paintControls(painter);
        if (!m_coloring.note.isEmpty())
            paintNote(painter, m_coloring.note, false);
    }
}

void EmberMapView::paintBadges(QPainter &painter)
{
    if (m_coloring.tab != Tab::RefactoringTargets || m_coloring.targets.isEmpty())
        return;
    const bool isDark = dark();
    const QColor bg = background();
    QFont font = this->font();
    font.setBold(true);
    font.setPixelSize(10);
    painter.setFont(font);
    for (const Target &target : m_coloring.targets) {
        const int index = m_layout->fileNode(target.file);
        if (index < 0)
            continue;
        const MapNode &node = m_layout->nodes[index];
        const QPointF c = toScreen(QPointF(node.x, node.y));
        const double r = node.r * m_view.k;
        const double br = 8.5;
        const QPointF at = r >= 10 ? c + QPointF(r * 0.72, -r * 0.72) : c + QPointF(r + br - 2, -r - br + 2);
        if (!rect().adjusted(-12, -12, 12, 12).contains(at.toPoint()))
            continue;
        painter.setPen(QPen(bg, 2));
        painter.setBrush(isDark ? QColor(240, 246, 252) : QColor(31, 35, 40));
        painter.drawEllipse(at, br, br);
        painter.setPen(isDark ? QColor(13, 17, 23) : QColor(255, 255, 255));
        painter.drawText(QRectF(at.x() - br, at.y() - br, 2 * br, 2 * br), Qt::AlignCenter,
                         QString::number(target.rank));
    }
}

void EmberMapView::paintLinks(QPainter &painter)
{
    if (m_selected < 0 || !m_layout)
        return;
    const auto &nodes = m_layout->nodes;
    const int selNode = m_layout->fileNode(m_selected);
    if (selNode < 0)
        return;
    const QPointF a = toScreen(QPointF(nodes[selNode].x, nodes[selNode].y));
    const double ra = nodes[selNode].r * m_view.k;
    const QColor link = couplingLinkColor();
    const bool isDark = dark();
    const double p = easeInOut(m_linkProgress);
    QList<QPair<QPointF, QString>> labels;
    if (m_coupling && m_selected < m_coupling->size()) {
        const QVector<CouplingRef> &refs = (*m_coupling)[m_selected];
        const int count = std::min<int>(kMaxLinks, refs.size());
        for (int i = 0; i < count; ++i) {
            const CouplingRef &ref = refs[i];
            const int other = m_layout->fileNode(ref.file);
            if (other < 0)
                continue;
            const QPointF b = toScreen(QPointF(nodes[other].x, nodes[other].y));
            const double rb = nodes[other].r * m_view.k;
            const QPointF d = b - a;
            const double len = std::hypot(d.x(), d.y());
            if (len < 1)
                continue;
            const QPointF normal(-d.y() / len, d.x() / len);
            const QPointF control = (a + b) / 2 + normal * (len * 0.22);
            QPainterPath path(a);
            const int steps = 36;
            const int upto = std::max(1, int(std::ceil(steps * p)));
            for (int s = 1; s <= upto; ++s)
                path.lineTo(bezier(a, control, b, std::min(p, double(s) / steps)));
            const double w = 1.2 + 3.8 * std::clamp(ref.degree, 0.0, 1.0);
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(withAlpha(isDark ? QColor(0, 0, 0) : QColor(255, 255, 255), 110), w + 2.5,
                                Qt::SolidLine, Qt::RoundCap));
            painter.drawPath(path);
            painter.setPen(QPen(withAlpha(link, int(110 + 130 * ref.degree)), w, Qt::SolidLine, Qt::RoundCap));
            painter.drawPath(path);
            if (p >= 1) {
                painter.setPen(QPen(link, 2));
                painter.setBrush(rb < 3 ? QBrush(link) : QBrush(Qt::NoBrush));
                painter.drawEllipse(b, std::max(rb + 1.5, 3.0), std::max(rb + 1.5, 3.0));
                if (len > 70)
                    labels.append({bezier(a, control, b, 0.5), percentText(ref.degree)});
            }
        }
    }
    QFont f = font();
    f.setPixelSize(10);
    f.setBold(true);
    painter.setFont(f);
    const QFontMetricsF fm(f);
    for (const auto &label : labels) {
        const double w = fm.horizontalAdvance(label.second) + 10;
        const QRectF pill(label.first.x() - w / 2, label.first.y() - 8, w, 16);
        painter.setPen(Qt::NoPen);
        painter.setBrush(isDark ? QColor(22, 27, 34, 235) : QColor(255, 255, 255, 240));
        painter.drawRoundedRect(pill, 8, 8);
        painter.setPen(isDark ? link : darken(link, 0.45));
        painter.drawText(pill, Qt::AlignCenter, label.second);
    }
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(isDark ? QColor(13, 17, 23) : QColor(31, 35, 40), 3));
    painter.drawEllipse(a, ra + 1.5, ra + 1.5);
    painter.setPen(QPen(isDark ? QColor(240, 246, 252) : QColor(255, 255, 255), 1.5));
    painter.drawEllipse(a, ra + 3.5, ra + 3.5);
    if (ra < 8) {
        const double pulse = 10 + 6 * (1 - p);
        painter.setPen(QPen(withAlpha(isDark ? QColor(240, 246, 252) : QColor(31, 35, 40), 160), 1.2));
        painter.drawEllipse(a, ra + pulse, ra + pulse);
    }
}

void EmberMapView::paintLegend(QPainter &painter)
{
    m_legendRect = QRectF();
    m_infoRect = QRectF();
    if (!m_layout || m_layout->isEmpty())
        return;
    const bool isDark = dark();
    QFont f = font();
    f.setPixelSize(12);
    painter.setFont(f);
    const QFontMetricsF fm(f);
    const QColor text = palette().color(QPalette::WindowText);
    const double h = 28;
    const double x0 = 12;
    const double y0 = 12;
    double x = x0 + 10;
    const double cy = y0 + h / 2;
    struct Item {
        double x;
        int kind;
        QColor color;
        QString label;
    };
    QList<Item> items;
    x += 18;
    if (m_coloring.scale) {
        items.append({x, 0, QColor(), m_coloring.scaleLowLabel});
        x += fm.horizontalAdvance(m_coloring.scaleLowLabel) + 6;
        items.append({x, 1, QColor(), QString()});
        x += 64 + 6;
        items.append({x, 0, QColor(), m_coloring.scaleHighLabel});
        x += fm.horizontalAdvance(m_coloring.scaleHighLabel) + 14;
    }
    const double maxWidth = width() - 120;
    for (const LegendEntry &entry : m_coloring.legend) {
        const double w = 14 + fm.horizontalAdvance(entry.label) + 14;
        if (x + w - x0 > maxWidth)
            break;
        items.append({x, entry.ring ? 3 : 2, entry.color, entry.label});
        x += w;
    }
    const QRectF chip(x0, y0, x - x0 - 4, h);
    m_legendRect = chip;
    painter.setPen(QPen(isDark ? QColor(255, 255, 255, 26) : QColor(31, 35, 40, 30), 1));
    painter.setBrush(isDark ? QColor(22, 27, 34, 225) : QColor(255, 255, 255, 235));
    painter.drawRoundedRect(chip, h / 2, h / 2);
    m_infoRect = QRectF(x0 + 8, cy - 8, 16, 16);
    painter.setPen(QPen(withAlpha(text, 170), 1.3));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(m_infoRect.adjusted(1, 1, -1, -1));
    QFont small = f;
    small.setPixelSize(10);
    small.setBold(true);
    painter.setFont(small);
    painter.drawText(m_infoRect, Qt::AlignCenter, QStringLiteral("i"));
    painter.setFont(f);
    for (const Item &item : items) {
        switch (item.kind) {
        case 0:
            painter.setPen(withAlpha(text, 200));
            painter.drawText(QRectF(item.x, y0, fm.horizontalAdvance(item.label) + 2, h), Qt::AlignVCenter,
                             item.label);
            break;
        case 1: {
            const QRectF bar(item.x, cy - 4, 64, 8);
            QLinearGradient g(bar.topLeft(), bar.topRight());
            if (m_coloring.tab == Tab::Hotspots) {
                g.setColorAt(0, m_coloring.scaleLow);
                g.setColorAt(0.5, QColor(QStringLiteral("#f0883e")));
                g.setColorAt(1, m_coloring.scaleHigh);
            } else {
                g.setColorAt(0, m_coloring.scaleLow);
                g.setColorAt(1, m_coloring.scaleHigh);
            }
            painter.setPen(Qt::NoPen);
            painter.setBrush(g);
            painter.drawRoundedRect(bar, 4, 4);
            break;
        }
        case 2:
            painter.setPen(Qt::NoPen);
            painter.setBrush(item.color);
            painter.drawEllipse(QPointF(item.x + 4, cy), 4.5, 4.5);
            painter.setPen(withAlpha(text, 210));
            painter.drawText(QRectF(item.x + 14, y0, fm.horizontalAdvance(item.label) + 2, h), Qt::AlignVCenter,
                             item.label);
            break;
        case 3: {
            painter.setBrush(Qt::NoBrush);
            if (item.color.isValid()) {
                QRadialGradient g(QPointF(item.x + 4, cy), 8);
                g.setColorAt(0.45, withAlpha(item.color, 0));
                g.setColorAt(0.6, withAlpha(item.color, 200));
                g.setColorAt(1, withAlpha(item.color, 0));
                painter.setPen(Qt::NoPen);
                painter.setBrush(g);
                painter.drawEllipse(QPointF(item.x + 4, cy), 8, 8);
            } else {
                QPen pen(withAlpha(text, 210), 1.2, Qt::DashLine);
                pen.setDashPattern({2.0, 2.0});
                painter.setPen(pen);
                painter.drawEllipse(QPointF(item.x + 4, cy), 5, 5);
            }
            painter.setPen(withAlpha(text, 210));
            painter.drawText(QRectF(item.x + 14, y0, fm.horizontalAdvance(item.label) + 2, h), Qt::AlignVCenter,
                             item.label);
            break;
        }
        default:
            break;
        }
    }
}

QRectF EmberMapView::controlRect(int index) const
{
    const double size = 28;
    const double x = width() - 12 - size;
    const double y = height() - 12 - size * (3 - index) - 4 * (2 - index);
    return QRectF(x, y, size, size);
}

int EmberMapView::controlAt(const QPointF &position) const
{
    if (!m_layout || m_layout->isEmpty())
        return -1;
    for (int i = 0; i < 3; ++i) {
        if (controlRect(i).contains(position))
            return i;
    }
    return -1;
}

void EmberMapView::paintControls(QPainter &painter)
{
    const bool isDark = dark();
    const QColor text = palette().color(QPalette::WindowText);
    for (int i = 0; i < 3; ++i) {
        const QRectF r = controlRect(i);
        painter.setPen(QPen(isDark ? QColor(255, 255, 255, 30) : QColor(31, 35, 40, 34), 1));
        painter.setBrush(m_hoverControl == i ? (isDark ? QColor(48, 54, 61, 240) : QColor(234, 238, 242, 245))
                                             : (isDark ? QColor(22, 27, 34, 225) : QColor(255, 255, 255, 235)));
        painter.drawRoundedRect(r, 8, 8);
        painter.setPen(QPen(withAlpha(text, 200), 1.6, Qt::SolidLine, Qt::RoundCap));
        const QPointF c = r.center();
        if (i == 0 || i == 1) {
            painter.drawLine(c + QPointF(-6, 0), c + QPointF(6, 0));
            if (i == 0)
                painter.drawLine(c + QPointF(0, -6), c + QPointF(0, 6));
        } else {
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(c, 6, 6);
            painter.drawEllipse(c, 2, 2);
        }
    }
}

void EmberMapView::paintNote(QPainter &painter, const QString &text, bool center)
{
    const bool isDark = dark();
    QFont f = font();
    f.setPixelSize(center ? 14 : 12);
    painter.setFont(f);
    const QFontMetricsF fm(f);
    const double maxW = std::min(560.0, width() - 60.0);
    const QRectF bounds = fm.boundingRect(QRectF(0, 0, maxW, 400), Qt::TextWordWrap | Qt::AlignCenter, text);
    const double w = bounds.width() + 32;
    const double h = bounds.height() + 20;
    const QRectF box(width() / 2.0 - w / 2, center ? height() / 2.0 - h / 2 : 52, w, h);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setPen(QPen(isDark ? QColor(255, 255, 255, 26) : QColor(31, 35, 40, 30), 1));
    painter.setBrush(isDark ? QColor(22, 27, 34, 235) : QColor(255, 255, 255, 240));
    painter.drawRoundedRect(box, 10, 10);
    painter.setPen(palette().color(QPalette::WindowText));
    painter.drawText(box.adjusted(16, 10, -16, -10), Qt::TextWordWrap | Qt::AlignCenter, text);
}

QString EmberMapView::tooltipFor(int node) const
{
    if (!m_layout || !m_snapshot || node < 0 || node >= m_layout->nodes.size())
        return {};
    const QLocale locale;
    const MapNode &n = m_layout->nodes[node];
    if (n.file < 0) {
        return QStringLiteral("<b>%1</b><br>%2 files · %3 lines of code<br>Code health %4 (weighted by lines)")
            .arg(n.name.toHtmlEscaped(), locale.toString(n.files), locale.toString(n.codeLines),
                 locale.toString(n.health, 'f', 1));
    }
    const insight::FileInfo &f = m_snapshot->files[n.file];
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(n.name.toHtmlEscaped());
    lines << QStringLiteral("<span style='color:gray'>%1</span>").arg(f.path.toHtmlEscaped());
    lines << QStringLiteral("Code health %1 · %2")
                 .arg(locale.toString(f.health, 'f', 1),
                      insight::healthBandName(insight::healthBand(f.health)).toHtmlEscaped());
    lines << QStringLiteral("%1 lines of code · complexity %2").arg(locale.toString(f.codeLines),
                                                                    locale.toString(f.complexity));
    if (hasHistory(*m_snapshot)) {
        lines << QStringLiteral("%1 commits · %2 authors · %3 fixes")
                     .arg(locale.toString(f.commits), locale.toString(f.authors), locale.toString(f.fixCommits));
        if (!f.mainAuthor.isEmpty())
            lines << QStringLiteral("Main author %1 (%2)").arg(f.mainAuthor.toHtmlEscaped(),
                                                               percentText(f.mainAuthorShare));
    }
    if (n.file < m_coloring.files.size() && m_coloring.files[n.file].rank > 0)
        lines << QStringLiteral("Refactoring target #%1").arg(m_coloring.files[n.file].rank);
    lines << QStringLiteral("<span style='color:gray'>Click to select · double-click to open</span>");
    return lines.join(QStringLiteral("<br>"));
}

void EmberMapView::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (m_layout && !m_layout->isEmpty() && m_focus >= 0 && m_focus < m_layout->nodes.size()) {
        const ViewTransform target = fitTransform(m_layout->nodes[m_focus], size());
        if (m_transition)
            m_to = target;
        else
            m_view = target;
    }
    invalidate();
}

void EmberMapView::clickAt(const QPointF &position)
{
    if (!m_layout || m_layout->isEmpty())
        return;
    const int control = controlAt(position);
    if (control == 0)
        return zoomBy(1.6);
    if (control == 1)
        return zoomBy(1 / 1.6);
    if (control == 2)
        return focusNode(0, true);
    if (m_legendRect.contains(position))
        return;
    const QPointF world = toWorld(position);
    const int deepest = m_layout->nodeAt(world);
    if (deepest < 0) {
        if (m_focus != 0)
            zoomOut();
        else if (m_selected >= 0) {
            setSelectedFile(-1, false);
            if (m_onSelect)
                m_onSelect(-1);
        }
        return;
    }
    if (!m_layout->isFolder(deepest) && screenRadius(deepest) >= 2.5) {
        setSelectedFile(m_layout->nodes[deepest].file, false);
        if (m_onSelect)
            m_onSelect(m_selected);
        return;
    }
    const QVector<int> chain = m_layout->chain(deepest);
    int target = -1;
    for (int node : chain) {
        if (node != m_focus && m_layout->isFolder(node) && m_layout->isAncestor(m_focus, node)
            && screenRadius(node) >= 14)
            target = node;
    }
    if (target >= 0)
        return focusNode(target, true);
    if (!m_layout->isAncestor(m_focus, deepest) && m_focus != 0)
        return zoomOut();
    if (m_selected >= 0) {
        setSelectedFile(-1, false);
        if (m_onSelect)
            m_onSelect(-1);
    }
}

void EmberMapView::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return QWidget::mousePressEvent(event);
    setFocus(Qt::MouseFocusReason);
    m_pressed = true;
    m_dragging = false;
    m_pressPos = event->position();
    m_lastPos = event->position();
}

void EmberMapView::mouseMoveEvent(QMouseEvent *event)
{
    const QPointF pos = event->position();
    if (m_pressed) {
        if (!m_dragging && (pos - m_pressPos).manhattanLength() > 4) {
            m_dragging = true;
            m_transition = false;
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_dragging) {
            const QPointF delta = pos - m_lastPos;
            m_view.cx -= delta.x() / m_view.k;
            m_view.cy -= delta.y() / m_view.k;
            m_lastPos = pos;
            beginInteraction();
            return;
        }
    }
    const int control = controlAt(pos);
    int hover = -1;
    if (control < 0 && m_layout && !m_layout->isEmpty()) {
        const int deepest = m_layout->nodeAt(toWorld(pos));
        if (deepest >= 0) {
            hover = deepest;
            if (m_layout->isFolder(deepest) || screenRadius(deepest) < 2.5) {
                hover = -1;
                for (int node : m_layout->chain(deepest)) {
                    if (m_layout->isFolder(node) && node != m_focus && m_layout->isAncestor(m_focus, node)
                        && screenRadius(node) >= 14)
                        hover = node;
                }
            }
        }
    }
    if (hover != m_hover || control != m_hoverControl) {
        m_hover = hover;
        m_hoverControl = control;
        setCursor(hover >= 0 || control >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
        update();
    }
}

void EmberMapView::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton || !m_pressed)
        return QWidget::mouseReleaseEvent(event);
    m_pressed = false;
    if (m_dragging) {
        m_dragging = false;
        setCursor(Qt::ArrowCursor);
        m_settleTimer.start();
        return;
    }
    clickAt(event->position());
}

void EmberMapView::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (!m_layout || m_layout->isEmpty() || event->button() != Qt::LeftButton)
        return;
    const int deepest = m_layout->nodeAt(toWorld(event->position()));
    if (deepest >= 0 && !m_layout->isFolder(deepest) && screenRadius(deepest) >= 2.5 && m_onOpen)
        m_onOpen(m_layout->nodes[deepest].file);
}

void EmberMapView::wheelEvent(QWheelEvent *event)
{
    if (!m_layout || m_layout->isEmpty())
        return;
    const double steps = event->angleDelta().y() != 0 ? event->angleDelta().y() : event->pixelDelta().y() * 2.0;
    if (steps == 0)
        return;
    const QPointF pos = event->position();
    const QPointF anchor = toWorld(pos);
    const double k = std::clamp(m_view.k * std::pow(1.0018, steps), minScale(), maxScale());
    m_transition = false;
    m_view.k = k;
    m_view.cx = anchor.x() - (pos.x() - width() / 2.0) / k;
    m_view.cy = anchor.y() - (pos.y() - height() / 2.0) / k;
    beginInteraction();
    event->accept();
}

void EmberMapView::keyPressEvent(QKeyEvent *event)
{
    switch (event->key()) {
    case Qt::Key_Escape:
        if (m_selected >= 0) {
            setSelectedFile(-1, false);
            if (m_onSelect)
                m_onSelect(-1);
        } else {
            zoomOut();
        }
        return;
    case Qt::Key_Backspace:
        zoomOut();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        zoomBy(1.6);
        return;
    case Qt::Key_Minus:
        zoomBy(1 / 1.6);
        return;
    case Qt::Key_0:
        focusNode(0, true);
        return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
        if (m_selected >= 0 && m_onOpen)
            m_onOpen(m_selected);
        return;
    default:
        break;
    }
    QWidget::keyPressEvent(event);
}

void EmberMapView::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_hover >= 0 || m_hoverControl >= 0) {
        m_hover = -1;
        m_hoverControl = -1;
        update();
    }
}

void EmberMapView::showEvent(QShowEvent *event)
{
    QWidget::showEvent(event);
    updateTimer();
}

void EmberMapView::hideEvent(QHideEvent *event)
{
    QWidget::hideEvent(event);
    m_timer.stop();
    if (m_transition) {
        m_transition = false;
        m_view = m_to;
    }
    m_linkProgress = 1;
    m_fast = false;
    m_dirty = true;
}

void EmberMapView::changeEvent(QEvent *event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        recolor();
        invalidate();
    }
}

bool EmberMapView::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const QPointF pos = help->pos();
        if (m_infoRect.adjusted(-4, -4, 4, 4).contains(pos)) {
            QToolTip::showText(
                help->globalPos(),
                QStringLiteral("<b>How Ember scores files</b><br>"
                               "Each circle is a file, sized by lines of code; grey discs are folders.<br><br>"
                               "<b>Code health</b> runs from 10 (healthy) to 1 (unhealthy). It drops for long "
                               "and deeply nested functions, high cyclomatic complexity, many parameters, "
                               "large files, duplicated blocks and import cycles.<br>"
                               "Healthy is 8 or above, problematic 4 to 8, unhealthy below 4.<br><br>"
                               "<b>Hotspots</b> are files that change often and are large: change frequency in "
                               "the git history window times size. Refactoring targets are hotspots with low "
                               "health, ranked by hotspot × (10 − health)."),
                this, m_infoRect.toRect());
            return true;
        }
        if (controlAt(pos) >= 0) {
            static const QString tips[] = {QStringLiteral("Zoom in (+)"), QStringLiteral("Zoom out (−)"),
                                           QStringLiteral("Show the whole repository (0)")};
            QToolTip::showText(help->globalPos(), tips[controlAt(pos)], this);
            return true;
        }
        if (m_layout && !m_layout->isEmpty()) {
            const int deepest = m_layout->nodeAt(toWorld(pos));
            int node = deepest;
            if (deepest >= 0 && !m_layout->isFolder(deepest) && screenRadius(deepest) < 2.5)
                node = m_layout->nodes[deepest].parent;
            if (node > 0) {
                QToolTip::showText(help->globalPos(), tooltipFor(node), this);
                return true;
            }
        }
        QToolTip::hideText();
        event->ignore();
        return true;
    }
    return QWidget::event(event);
}

} // namespace forkmesh::ember
