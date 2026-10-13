#include "StatsDashboard.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QJsonObject>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QTimer>
#include <QToolTip>

#include <algorithm>
#include <cmath>

namespace {

constexpr int kRadius = 10;
constexpr int kInsetX = 14;
constexpr int kInsetY = 10;
constexpr int kControlSize = 18;
constexpr int kControlGap = 4;
constexpr int kMaxSpan = 2;
constexpr int kStatusEdge = 4;
constexpr int kUpdateDot = 8;
constexpr int kUpdatePulseMs = 1'500;
constexpr int kFadeTickMs = 120;

// Dataviz reference status palette: fixed across light and dark surfaces.
QColor statusColor(StatsTileStatus status)
{
    switch (status) {
    case StatsTileStatus::Good:
        return QColor(QStringLiteral("#0ca30c"));
    case StatsTileStatus::Warning:
        return QColor(QStringLiteral("#fab219"));
    case StatsTileStatus::Critical:
        return QColor(QStringLiteral("#d03b3b"));
    case StatsTileStatus::None:
        break;
    }
    return QColor();
}

QString statusText(StatsTileStatus status)
{
    switch (status) {
    case StatsTileStatus::Good:
        return QStringLiteral("Operating well");
    case StatsTileStatus::Warning:
        return QStringLiteral("Needs attention");
    case StatsTileStatus::Critical:
        return QStringLiteral("Critical");
    case StatsTileStatus::None:
        break;
    }
    return QString();
}

QString trendText(double percent)
{
    const QString arrow = percent >= 0 ? QString::fromUtf8("\xE2\x96\xB2")
                                       : QString::fromUtf8("\xE2\x96\xBC");
    const double magnitude = std::fabs(percent);
    if (magnitude >= 1000)
        return arrow + QStringLiteral(" >999%");
    return arrow + QStringLiteral(" %1%").arg(magnitude, 0, 'f', magnitude < 10 ? 1 : 0);
}

// The global stylesheet sizes widget fonts in pixels, which leaves
// pointSizeF() at -1: scale whichever unit the font actually carries.
QFont resizedFont(QFont font, qreal deltaPoints)
{
    if (font.pixelSize() > 0)
        font.setPixelSize(qMax(9, qRound(font.pixelSize() + deltaPoints * 4.0 / 3.0)));
    else if (font.pointSizeF() > 0)
        font.setPointSizeF(qMax(7.0, font.pointSizeF() + deltaPoints));
    return font;
}

int clampSpan(int span)
{
    return qBound(1, span, kMaxSpan);
}

QString sizeLabel(int width, int height)
{
    if (width == 1 && height == 1)
        return QString::fromUtf8("One square (1 \xC3\x97 1)");
    if (width == 2 && height == 1)
        return QString::fromUtf8("Two squares, wide (2 \xC3\x97 1)");
    if (width == 1 && height == 2)
        return QString::fromUtf8("Two squares, tall (1 \xC3\x97 2)");
    return QString::fromUtf8("Four squares (2 \xC3\x97 2)");
}

} // namespace

StatsDashboard::StatsDashboard(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("statsDashboard"));
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    // Runs only while an update dot is fading; repaints just those dots.
    m_fadeTimer = new QTimer(this);
    m_fadeTimer->setInterval(kFadeTickMs);
    connect(m_fadeTimer, &QTimer::timeout, this, [this] { tickUpdateFade(); });
    relayout(width());
}

void StatsDashboard::setCatalog(const QVector<StatsTileCatalogEntry> &catalog)
{
    m_catalog = catalog;
    m_catalogIndex.clear();
    m_catalogIndex.reserve(m_catalog.size());
    for (int i = 0; i < m_catalog.size(); ++i)
        m_catalogIndex.insert(m_catalog.at(i).id, i);
    update();
}

void StatsDashboard::setDataProvider(
    std::function<StatsTileData(const QString &)> provider)
{
    m_provider = std::move(provider);
}

void StatsDashboard::setDefaultLayout(const QVector<StatsTilePlacement> &layout)
{
    m_defaultLayout = layout;
}

void StatsDashboard::setLayoutModel(const QVector<StatsTilePlacement> &layout)
{
    m_tiles.clear();
    for (const StatsTilePlacement &placement : layout) {
        if (placement.id.isEmpty() || hasTile(placement.id))
            continue;
        if (!m_catalog.isEmpty() && !catalogEntry(placement.id))
            continue; // a stat that no longer exists
        m_tiles.append({placement.id, clampSpan(placement.width),
                        clampSpan(placement.height)});
    }
    refresh();
}

void StatsDashboard::resetLayout()
{
    setLayoutModel(m_defaultLayout);
    notifyLayoutChanged();
}

int StatsDashboard::indexOf(const QString &id) const
{
    for (int i = 0; i < m_tiles.size(); ++i)
        if (m_tiles.at(i).id == id)
            return i;
    return -1;
}

const StatsTileCatalogEntry *StatsDashboard::catalogEntry(const QString &id) const
{
    const auto hit = m_catalogIndex.constFind(id);
    if (hit == m_catalogIndex.constEnd())
        return nullptr;
    return &m_catalog.at(hit.value());
}

QStringList StatsDashboard::tileIds() const
{
    QStringList ids;
    ids.reserve(m_tiles.size());
    for (const StatsTilePlacement &placement : m_tiles)
        ids.append(placement.id);
    return ids;
}

bool StatsDashboard::addTile(const QString &id, int width, int height)
{
    if (id.isEmpty() || hasTile(id) || !catalogEntry(id))
        return false;
    if (width < 1 || width > kMaxSpan || height < 1 || height > kMaxSpan)
        return false;
    m_tiles.append({id, width, height});
    if (m_provider)
        m_data.insert(id, m_provider(id));
    relayout(this->width());
    notifyLayoutChanged();
    update();
    return true;
}

bool StatsDashboard::removeTile(const QString &id)
{
    const int index = indexOf(id);
    if (index < 0)
        return false;
    m_tiles.removeAt(index);
    m_data.remove(id);
    m_updatedAtMs.remove(id);
    m_hoverTile = -1;
    m_hoverPoint = -1;
    relayout(width());
    notifyLayoutChanged();
    update();
    return true;
}

bool StatsDashboard::resizeTile(const QString &id, int width, int height)
{
    const int index = indexOf(id);
    if (index < 0 || width < 1 || width > kMaxSpan || height < 1 ||
        height > kMaxSpan)
        return false;
    StatsTilePlacement &placement = m_tiles[index];
    if (placement.width == width && placement.height == height)
        return true;
    placement.width = width;
    placement.height = height;
    relayout(this->width());
    notifyLayoutChanged();
    update();
    return true;
}

bool StatsDashboard::moveTile(const QString &id, int toIndex)
{
    const int from = indexOf(id);
    if (from < 0)
        return false;
    toIndex = qBound(0, toIndex, m_tiles.size() - 1);
    if (from == toIndex)
        return true;
    m_tiles.move(from, toIndex);
    relayout(width());
    notifyLayoutChanged();
    update();
    return true;
}

void StatsDashboard::notifyLayoutChanged()
{
    if (layoutChanged)
        layoutChanged();
}

void StatsDashboard::refresh()
{
    // A tile read for the first time is not "updated"; only a headline that
    // differs from the previous refresh lights the dot.
    const QHash<QString, StatsTileData> previous = m_data;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool changed = false;
    m_data.clear();
    if (m_provider)
        for (const StatsTilePlacement &placement : m_tiles) {
            StatsTileData data = m_provider(placement.id);
            const auto before = previous.constFind(placement.id);
            if (before != previous.cend() &&
                (before->available != data.available ||
                 (data.available && before->value != data.value))) {
                m_updatedAtMs.insert(placement.id, now);
                changed = true;
            }
            m_data.insert(placement.id, data);
        }
    relayout(width());
    update();
    if (changed && !m_fadeTimer->isActive())
        m_fadeTimer->start();
}

double StatsDashboard::updateFade(const QString &id, qint64 now) const
{
    const auto stamp = m_updatedAtMs.constFind(id);
    if (stamp == m_updatedAtMs.cend())
        return 0.0;
    const qint64 elapsed = now - stamp.value();
    if (elapsed < 0 || elapsed >= kUpdateFadeMs)
        return 0.0;
    return 1.0 - double(elapsed) / double(kUpdateFadeMs);
}

QRect StatsDashboard::updateDotRect(const Tile &tile) const
{
    const QRect inner = tile.rect.adjusted(kInsetX, kInsetY, -kInsetX, -kInsetY);
    return QRect(inner.right() - kUpdateDot + 1, inner.top() + (16 - kUpdateDot) / 2,
                 kUpdateDot, kUpdateDot);
}

void StatsDashboard::tickUpdateFade()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool fading = false;
    for (const Tile &tile : m_layout) {
        const auto stamp = m_updatedAtMs.constFind(tile.placement.id);
        if (stamp == m_updatedAtMs.cend())
            continue;
        const qint64 elapsed = now - stamp.value();
        // One tick past the end, so the last faint frame is cleared.
        if (elapsed > kUpdateFadeMs + kFadeTickMs)
            continue;
        fading = fading || elapsed <= kUpdateFadeMs;
        update(updateDotRect(tile).adjusted(-10, -10, 10, 10));
    }
    if (!fading)
        m_fadeTimer->stop();
}

bool StatsDashboard::trendPercent(const StatsTileSeries &series, double *percent)
{
    const int n = series.values.size();
    if (series.categorical || n < 4)
        return false;
    const int half = n / 2;
    double older = 0, newer = 0;
    for (int i = 0; i < half; ++i) {
        older += series.values.at(i);
        newer += series.values.at(n - half + i);
    }
    older /= half;
    newer /= half;
    if (std::fabs(older) < 1e-9)
        return false;
    if (percent)
        *percent = 100.0 * (newer - older) / std::fabs(older);
    return true;
}

StatsTileStatus StatsDashboard::tileStatus(const QString &id) const
{
    return m_data.value(id).status;
}

bool StatsDashboard::tileRecentlyUpdated(const QString &id) const
{
    return updateFade(id, QDateTime::currentMSecsSinceEpoch()) > 0;
}

QJsonArray StatsDashboard::layoutToJson(const QVector<StatsTilePlacement> &layout)
{
    QJsonArray array;
    for (const StatsTilePlacement &placement : layout)
        array.append(QJsonObject{{QStringLiteral("id"), placement.id},
                                 {QStringLiteral("w"), placement.width},
                                 {QStringLiteral("h"), placement.height}});
    return array;
}

QVector<StatsTilePlacement> StatsDashboard::layoutFromJson(const QJsonArray &array)
{
    QVector<StatsTilePlacement> layout;
    for (const QJsonValue &value : array) {
        const QJsonObject object = value.toObject();
        const QString id = object.value(QStringLiteral("id")).toString();
        if (id.isEmpty())
            continue;
        layout.append({id, clampSpan(object.value(QStringLiteral("w")).toInt(1)),
                       clampSpan(object.value(QStringLiteral("h")).toInt(1))});
    }
    return layout;
}

StatsDashboard::LayoutMerge StatsDashboard::mergeSavedLayout(
    const QVector<StatsTilePlacement> &saved, bool hasSaved,
    const QVector<StatsTilePlacement> &defaults,
    const QVector<StatsTileCatalogEntry> &catalog,
    const QStringList &offered)
{
    LayoutMerge merge;
    QSet<QString> catalogIds;
    for (const StatsTileCatalogEntry &entry : catalog)
        catalogIds.insert(entry.id);
    QSet<QString> alreadyOffered(offered.cbegin(), offered.cend());

    QSet<QString> placed;
    const QVector<StatsTilePlacement> &start = hasSaved ? saved : defaults;
    for (const StatsTilePlacement &placement : start) {
        if (placement.id.isEmpty() || placed.contains(placement.id))
            continue;
        placed.insert(placement.id);
        merge.layout.append({placement.id, clampSpan(placement.width),
                             clampSpan(placement.height)});
    }
    if (hasSaved) {
        for (const StatsTilePlacement &placement : defaults) {
            if (placement.id.isEmpty() || placed.contains(placement.id) ||
                alreadyOffered.contains(placement.id) ||
                !catalogIds.contains(placement.id)) {
                continue;
            }
            placed.insert(placement.id);
            merge.layout.append({placement.id, clampSpan(placement.width),
                                 clampSpan(placement.height)});
            merge.changed = true;
        }
    } else {
        merge.changed = true;
    }

    merge.offered = offered;
    for (const StatsTileCatalogEntry &entry : catalog) {
        if (!alreadyOffered.contains(entry.id)) {
            merge.offered.append(entry.id);
            alreadyOffered.insert(entry.id);
            merge.changed = true;
        }
    }
    return merge;
}

// code-health: allow(deep-nested-loop) packs the Stats page's tiles on a grid of width / 176 px columns; the inner cell loops cover a span clamped to kMaxSpan (2)
QVector<QRect> StatsDashboard::packLayout(
    const QVector<StatsTilePlacement> &layout, int columns)
{
    columns = qMax(1, columns);
    QVector<QRect> cells;
    cells.reserve(layout.size());
    QVector<QVector<bool>> occupied; // [row][column]
    auto ensureRows = [&](int rows) {
        while (occupied.size() < rows)
            occupied.append(QVector<bool>(columns, false));
    };
    auto fits = [&](int col, int row, int w, int h) {
        ensureRows(row + h);
        for (int r = row; r < row + h; ++r)
            for (int c = col; c < col + w; ++c)
                if (occupied[r][c])
                    return false;
        return true;
    };
    for (const StatsTilePlacement &placement : layout) {
        const int w = qMin(columns, clampSpan(placement.width));
        const int h = clampSpan(placement.height);
        QRect cell;
        for (int row = 0; cell.isNull(); ++row) {
            for (int col = 0; col + w <= columns; ++col) {
                if (!fits(col, row, w, h))
                    continue;
                cell = QRect(col, row, w, h);
                for (int r = row; r < row + h; ++r)
                    for (int c = col; c < col + w; ++c)
                        occupied[r][c] = true;
                break;
            }
        }
        cells.append(cell);
    }
    return cells;
}

QString StatsDashboard::compactNumber(double value)
{
    const double magnitude = std::fabs(value);
    // Qt's C locale omits group separators; English keeps the "1,284" form.
    const QLocale grouped(QLocale::English, QLocale::UnitedStates);
    if (magnitude < 1000) {
        if (std::fabs(value - std::round(value)) < 0.05)
            return grouped.toString(qint64(std::llround(value)));
        return QString::number(value, 'f', 1);
    }
    if (magnitude < 10'000)
        return grouped.toString(qint64(std::llround(value)));
    if (magnitude < 1'000'000)
        return QStringLiteral("%1K").arg(value / 1000.0, 0, 'f',
                                          magnitude < 100'000 ? 1 : 0);
    if (magnitude < 1'000'000'000)
        return QStringLiteral("%1M").arg(value / 1'000'000.0, 0, 'f',
                                          magnitude < 10'000'000 ? 2 : 1);
    return QStringLiteral("%1B").arg(value / 1'000'000'000.0, 0, 'f', 1);
}

QString StatsDashboard::compactBytes(qint64 bytes)
{
    const char *units[] = {"B", "KB", "MB", "GB", "TB"};
    double value = double(qMax<qint64>(0, bytes));
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    if (unit == 0)
        return QStringLiteral("%1 B").arg(qint64(value));
    return QStringLiteral("%1 %2").arg(value, 0, 'f', value < 10 ? 1 : 0)
        .arg(QLatin1String(units[unit]));
}

QString StatsDashboard::compactUsd(double value)
{
    const double magnitude = std::fabs(value);
    const QString sign = value < 0 ? QStringLiteral("-") : QString();
    if (magnitude < 100)
        return sign + QStringLiteral("$%1").arg(magnitude, 0, 'f', 2);
    return sign + QStringLiteral("$") + compactNumber(magnitude);
}

QString StatsDashboard::compactDuration(qint64 ms)
{
    const qint64 seconds = qMax<qint64>(0, ms / 1000);
    if (seconds < 60)
        return QStringLiteral("%1s").arg(seconds);
    const qint64 minutes = seconds / 60;
    if (minutes < 60)
        return QStringLiteral("%1m").arg(minutes);
    const qint64 hours = minutes / 60;
    if (hours < 48)
        return QStringLiteral("%1h %2m").arg(hours).arg(minutes % 60);
    const qint64 days = hours / 24;
    return QStringLiteral("%1d %2h").arg(days).arg(hours % 24);
}

QRect StatsDashboard::tileRect(const QString &id) const
{
    for (const Tile &tile : m_layout)
        if (tile.placement.id == id)
            return tile.rect;
    return QRect();
}

QString StatsDashboard::tileAt(const QPoint &point) const
{
    for (const Tile &tile : m_layout)
        if (tile.rect.contains(point))
            return tile.placement.id;
    return QString();
}

QString StatsDashboard::tileValue(const QString &id) const
{
    return m_data.value(id).value;
}

QRect StatsDashboard::tileRemoveRect(const QString &id) const
{
    for (const Tile &tile : m_layout)
        if (tile.placement.id == id)
            return tile.removeButton;
    return QRect();
}

QRect StatsDashboard::tileSizeRect(const QString &id) const
{
    for (const Tile &tile : m_layout)
        if (tile.placement.id == id)
            return tile.sizeButton;
    return QRect();
}

QSize StatsDashboard::sizeHint() const
{
    return QSize(qMax(kMinSquareWidth, width()), qMax(kSquareHeight, m_contentHeight));
}

QSize StatsDashboard::minimumSizeHint() const
{
    return QSize(kMinSquareWidth, qMax(kSquareHeight, m_contentHeight));
}

void StatsDashboard::relayout(int width)
{
    width = qMax(kMinSquareWidth, width);
    m_columns = qMax(1, (width + kGap) / (kMinSquareWidth + kGap));
    // Every grid cell is a true square: the side is whatever width one column
    // gets, so the board stays square-tiled at any window width.
    const int side = (width - (m_columns - 1) * kGap) / m_columns;
    const QVector<QRect> cells = packLayout(m_tiles, m_columns);

    m_layout.clear();
    m_layout.reserve(m_tiles.size());
    int bottom = 0;
    for (int i = 0; i < m_tiles.size(); ++i) {
        Tile tile;
        tile.placement = m_tiles.at(i);
        tile.data = m_data.value(tile.placement.id);
        tile.cell = cells.at(i);
        tile.rect = QRect(tile.cell.x() * (side + kGap),
                          tile.cell.y() * (side + kGap),
                          tile.cell.width() * side + (tile.cell.width() - 1) * kGap,
                          tile.cell.height() * side + (tile.cell.height() - 1) * kGap);
        bottom = qMax(bottom, tile.rect.bottom() + 1);

        const QRect inner = tile.rect.adjusted(kInsetX, kInsetY, -kInsetX, -kInsetY);
        tile.removeButton = QRect(inner.right() - kControlSize + 1, inner.top(),
                                  kControlSize, kControlSize);
        tile.sizeButton = tile.removeButton.translated(-kControlSize - kControlGap, 0);

        const bool single = tile.cell.width() == 1 && tile.cell.height() == 1;
        const bool hasSeries = !tile.data.series.isEmpty() && tile.data.available;
        if (hasSeries) {
            // Title, value and note stack at the top; the chart takes the
            // full width below them, whatever the tile's span.
            const QFont valueFont = resizedFont(font(), single ? 7.0 : 13.0);
            const int textBottom =
                inner.top() + 18 + QFontMetrics(valueFont).height() + 18;
            tile.plot = QRect(inner.left(), textBottom + 4, inner.width(),
                              inner.bottom() - textBottom - 4);
            if (tile.plot.height() < 18 || tile.plot.width() < 24)
                tile.plot = QRect();
        }
        computePoints(tile);
        m_layout.append(tile);
    }
    m_contentHeight = m_tiles.isEmpty() ? side : bottom;
    m_layoutWidth = width;
    if (m_hoverTile >= m_layout.size())
        m_hoverTile = -1;
    updateGeometry();
}

void StatsDashboard::computePoints(Tile &tile) const
{
    tile.points.clear();
    if (tile.plot.isEmpty())
        return;
    const StatsTileSeries &series = tile.data.series;
    const int n = series.values.size();
    if (n == 0)
        return;
    double maximum = 0;
    for (double v : series.values)
        maximum = qMax(maximum, v);
    if (maximum <= 0)
        maximum = 1;
    // Larger plots keep a strip for axis labels below the marks.
    const bool labelled = tile.plot.height() >= 60;
    const QRect area = labelled ? tile.plot.adjusted(0, 12, 0, -14) : tile.plot;
    tile.points.reserve(n);
    if (series.categorical) {
        const qreal slot = area.width() / qreal(n);
        for (int i = 0; i < n; ++i) {
            const qreal x = area.left() + slot * i + slot / 2.0;
            const qreal y = area.bottom() - area.height() * series.values.at(i) / maximum;
            tile.points.append(QPointF(x, y));
        }
        return;
    }
    for (int i = 0; i < n; ++i) {
        const qreal x = n == 1 ? area.center().x()
                               : area.left() + area.width() * i / qreal(n - 1);
        const qreal y = area.bottom() - area.height() * series.values.at(i) / maximum;
        tile.points.append(QPointF(x, y));
    }
}

bool StatsDashboard::darkSurface() const
{
    return palette().color(QPalette::Window).lightness() < 128;
}

QColor StatsDashboard::accentColor() const
{
    // Dataviz reference palette, categorical slot 1, stepped per surface.
    return QColor(darkSurface() ? QStringLiteral("#3987e5")
                                : QStringLiteral("#2a78d6"));
}

QColor StatsDashboard::mutedColor() const
{
    return palette().color(QPalette::PlaceholderText);
}


void StatsDashboard::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    if (width() != m_layoutWidth)
        relayout(width());
}

void StatsDashboard::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_layout.isEmpty()) {
        paintEmptyState(painter);
        return;
    }
    // The fade timer repaints single dots; skip every tile outside the dirty
    // rect instead of re-painting the whole board ten times a second.
    const QRect dirty = event ? event->rect() : rect();
    int dragged = -1;
    for (int i = 0; i < m_layout.size(); ++i) {
        const Tile &tile = m_layout.at(i);
        if (!m_dragging && !tile.rect.intersects(dirty))
            continue;
        if (m_dragging && tile.placement.id == m_pressedTile) {
            dragged = i;
            // The slot the tile will land in.
            QColor outline = accentColor();
            outline.setAlpha(160);
            painter.setPen(QPen(outline, 1.5, Qt::DashLine));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(tile.rect).adjusted(1, 1, -1, -1),
                                    kRadius, kRadius);
            continue;
        }
        paintTile(painter, tile, i == m_hoverTile && !m_dragging, false);
    }
    if (dragged >= 0) {
        Tile ghost = m_layout.at(dragged);
        const QPoint delta = m_dragPos - ghost.rect.topLeft();
        ghost.rect.translate(delta);
        ghost.plot.translate(delta);
        ghost.removeButton.translate(delta);
        ghost.sizeButton.translate(delta);
        for (QPointF &point : ghost.points)
            point += delta;
        painter.setOpacity(0.92);
        paintTile(painter, ghost, false, true);
        painter.setOpacity(1.0);
    }
}

void StatsDashboard::paintEmptyState(QPainter &painter) const
{
    painter.setPen(mutedColor());
    painter.setFont(font());
    painter.drawText(rect(), Qt::AlignCenter | Qt::TextWordWrap,
                     QString::fromUtf8("No tiles on the board \xE2\x80\x94 use "
                                       "Add tile, or Reset to bring the "
                                       "default set back."));
}

void StatsDashboard::paintTile(QPainter &painter, const Tile &tile,
                               bool hovered, bool ghost) const
{
    const QPalette colors = palette();
    QColor border = colors.color(QPalette::Mid);
    border.setAlpha(hovered || ghost ? 220 : 140);
    if (ghost) {
        QColor shadow(0, 0, 0, darkSurface() ? 120 : 50);
        painter.setPen(Qt::NoPen);
        painter.setBrush(shadow);
        painter.drawRoundedRect(QRectF(tile.rect).translated(0, 4).adjusted(2, 2, -2, -2),
                                kRadius, kRadius);
    }
    const QColor status = statusColor(tile.data.status);
    if (status.isValid())
        border = QColor(status.red(), status.green(), status.blue(),
                        hovered || ghost ? 230 : 150);
    const QRectF outline = QRectF(tile.rect).adjusted(0.5, 0.5, -0.5, -0.5);
    painter.setPen(QPen(border, 1));
    painter.setBrush(colors.color(QPalette::Base));
    painter.drawRoundedRect(outline, kRadius, kRadius);
    if (status.isValid()) {
        // A faint wash of the status colour and a solid edge down the left.
        QPainterPath shape;
        shape.addRoundedRect(outline, kRadius, kRadius);
        painter.save();
        painter.setClipPath(shape);
        painter.setPen(Qt::NoPen);
        QColor wash = status;
        wash.setAlpha(darkSurface() ? 24 : 16);
        painter.setBrush(wash);
        painter.drawRect(tile.rect);
        painter.setBrush(status);
        painter.drawRect(QRect(tile.rect.left(), tile.rect.top(), kStatusEdge,
                               tile.rect.height()));
        painter.restore();
    }

    const QRect inner = tile.rect.adjusted(kInsetX, kInsetY, -kInsetX, -kInsetY);
    const bool single = tile.cell.width() == 1 && tile.cell.height() == 1;
    const StatsTileCatalogEntry *entry = catalogEntry(tile.placement.id);
    const QString title = entry ? entry->title : tile.placement.id;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double fade = hovered || ghost ? 0.0 : updateFade(tile.placement.id, now);

    const QFont labelFont = resizedFont(font(), -1.0);
    const int controlsWidth = hovered ? 2 * kControlSize + kControlGap + 6
                                      : (fade > 0 ? kUpdateDot + 6 : 0);
    painter.setFont(labelFont);
    painter.setPen(mutedColor());
    const QRect labelRect(inner.left(), inner.top(), inner.width() - controlsWidth, 16);
    painter.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(labelFont).elidedText(title, Qt::ElideRight,
                                                        labelRect.width()));

    QFont valueFont = resizedFont(font(), single ? 7.0 : 13.0);
    valueFont.setWeight(QFont::DemiBold);
    painter.setFont(valueFont);
    painter.setPen(colors.color(QPalette::Text));
    const int valueHeight = QFontMetrics(valueFont).height();
    const QRect valueRect(inner.left(), inner.top() + 18, inner.width(), valueHeight);
    const QString value = tile.data.available ? tile.data.value
                                              : QString::fromUtf8("\xE2\x80\x94");
    const QString shownValue =
        QFontMetrics(valueFont).elidedText(value, Qt::ElideRight, valueRect.width());
    painter.drawText(valueRect, Qt::AlignLeft | Qt::AlignVCenter, shownValue);

    // Trend chip beside the headline, or at the end of the note line when a
    // one-square tile has no room beside a wide value.
    int noteReserve = 0;
    double percent = 0;
    if (tile.data.available && trendPercent(tile.data.series, &percent) &&
        std::fabs(percent) >= 1.0) {
        QFont chipFont = labelFont;
        chipFont.setWeight(QFont::DemiBold);
        const QFontMetrics chipMetrics(chipFont);
        const QString chip = trendText(percent);
        const int chipWidth = chipMetrics.horizontalAdvance(chip) + 10;
        const int chipHeight = chipMetrics.height() + 2;
        int left = inner.left() + QFontMetrics(valueFont).horizontalAdvance(shownValue) + 8;
        int top = valueRect.center().y() - chipHeight / 2 + 1;
        if (left + chipWidth > inner.right()) {
            left = inner.right() - chipWidth + 1;
            top = valueRect.bottom() + 2 + (16 - chipHeight) / 2;
            noteReserve = chipWidth + 6;
        }
        if (left > inner.left()) {
            QColor tone = mutedColor();
            const bool up = percent > 0;
            if (tile.data.trend == StatsTrendPolarity::UpIsGood)
                tone = statusColor(up ? StatsTileStatus::Good : StatsTileStatus::Critical);
            else if (tile.data.trend == StatsTrendPolarity::DownIsGood)
                tone = statusColor(up ? StatsTileStatus::Critical : StatsTileStatus::Good);
            const QRect chipRect(left, top, chipWidth, chipHeight);
            QColor fill = tone;
            fill.setAlpha(darkSurface() ? 46 : 30);
            painter.setPen(Qt::NoPen);
            painter.setBrush(fill);
            painter.drawRoundedRect(chipRect, chipHeight / 2.0, chipHeight / 2.0);
            painter.setFont(chipFont);
            painter.setPen(tone);
            painter.drawText(chipRect, Qt::AlignCenter, chip);
        }
    }

    if (fade > 0) {
        // Recently updated: a dot that fades out, with a ring that pulses
        // outwards for the first moment after the change.
        const QRectF dot = updateDotRect(tile);
        const QColor accent = accentColor();
        const qint64 elapsed = kUpdateFadeMs - qint64(fade * kUpdateFadeMs);
        painter.save();
        if (elapsed < kUpdatePulseMs) {
            const double t = double(elapsed) / kUpdatePulseMs;
            QColor ring = accent;
            ring.setAlphaF(0.55 * (1.0 - t));
            painter.setPen(QPen(ring, 1.5));
            painter.setBrush(Qt::NoBrush);
            const double radius = kUpdateDot / 2.0 + 6.0 * t;
            painter.drawEllipse(dot.center(), radius, radius);
        }
        QColor fill = accent;
        fill.setAlphaF(0.25 + 0.75 * fade);
        painter.setPen(Qt::NoPen);
        painter.setBrush(fill);
        const double radius = kUpdateDot / 2.0 * (0.6 + 0.4 * fade);
        painter.drawEllipse(dot.center(), radius, radius);
        painter.restore();
    }

    painter.setFont(labelFont);
    painter.setPen(mutedColor());
    const QString note = tile.data.available ? tile.data.note
                                             : QStringLiteral("No data yet");
    const QRect noteRect(inner.left(), valueRect.bottom() + 2, inner.width() - noteReserve, 16);
    painter.drawText(noteRect, Qt::AlignLeft | Qt::AlignVCenter,
                     QFontMetrics(labelFont).elidedText(note, Qt::ElideRight,
                                                        noteRect.width()));

    if (!tile.plot.isEmpty())
        paintSeries(painter, tile, hovered ? m_hoverPoint : -1);
    if (hovered)
        paintControls(painter, tile);
}

void StatsDashboard::paintSeries(QPainter &painter, const Tile &tile,
                                 int hoverPoint) const
{
    const StatsTileSeries &series = tile.data.series;
    if (tile.points.isEmpty())
        return;
    const bool labelled = tile.plot.height() >= 60;
    const QRect area = labelled ? tile.plot.adjusted(0, 12, 0, -14) : tile.plot;
    const QColor accent = accentColor();
    const QColor surface = palette().color(QPalette::Base);
    QColor muted = mutedColor();
    const QFont tickFont = resizedFont(font(), -2.0);
    const QFontMetrics tickMetrics(tickFont);
    double maximum = 0;
    int maxIndex = 0;
    for (int i = 0; i < series.values.size(); ++i)
        if (series.values.at(i) > maximum) {
            maximum = series.values.at(i);
            maxIndex = i;
        }

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    if (labelled) {
        // Recessive baseline and the peak value, so the scale is readable.
        QColor grid = muted;
        grid.setAlpha(70);
        painter.setPen(QPen(grid, 1));
        painter.drawLine(QPointF(area.left(), area.bottom() + 0.5),
                         QPointF(area.right(), area.bottom() + 0.5));
        painter.setFont(tickFont);
        painter.setPen(muted);
        painter.drawText(QRect(tile.plot.left(), tile.plot.top(), tile.plot.width(), 12),
                         Qt::AlignLeft | Qt::AlignVCenter,
                         QStringLiteral("peak %1").arg(compactNumber(maximum)));
    }

    if (series.categorical) {
        const int n = series.values.size();
        const qreal slot = area.width() / qreal(n);
        const qreal barWidth = qBound(3.0, slot - 2.0, 40.0);
        for (int i = 0; i < n; ++i) {
            const QPointF top = tile.points.at(i);
            const qreal height = qMax<qreal>(0, area.bottom() - top.y());
            QRectF bar(top.x() - barWidth / 2.0, area.bottom() - height,
                       barWidth, height);
            if (bar.height() < 1.5 && series.values.at(i) > 0)
                bar.setTop(bar.bottom() - 1.5);
            painter.setPen(Qt::NoPen);
            const QColor tone = statusColor(series.tones.value(i, StatsTileStatus::None));
            QColor fill = tone.isValid() ? tone : accent;
            if (hoverPoint >= 0 && hoverPoint != i)
                fill.setAlpha(150);
            painter.setBrush(fill);
            // 4px rounded data-end, square at the baseline.
            QPainterPath path;
            const qreal r = qMin(4.0, barWidth / 2.0);
            path.moveTo(bar.left(), bar.bottom());
            path.lineTo(bar.left(), bar.top() + r);
            path.quadTo(bar.left(), bar.top(), bar.left() + r, bar.top());
            path.lineTo(bar.right() - r, bar.top());
            path.quadTo(bar.right(), bar.top(), bar.right(), bar.top() + r);
            path.lineTo(bar.right(), bar.bottom());
            path.closeSubpath();
            painter.drawPath(path);
            if (labelled) {
                const QString label = series.labels.value(i);
                if (!label.isEmpty() && tickMetrics.horizontalAdvance(label) <= slot - 2) {
                    painter.setFont(tickFont);
                    painter.setPen(muted);
                    painter.drawText(QRectF(top.x() - slot / 2.0, area.bottom() + 2,
                                            slot, 12),
                                     Qt::AlignHCenter | Qt::AlignVCenter, label);
                }
            }
        }
        painter.restore();
        return;
    }

    // Time series: area fill under a grey line, the newest sample in the accent.
    QPainterPath line(tile.points.first());
    for (int i = 1; i < tile.points.size(); ++i)
        line.lineTo(tile.points.at(i));
    if (tile.points.size() >= 2) {
        QPainterPath fill = line;
        fill.lineTo(tile.points.last().x(), area.bottom());
        fill.lineTo(tile.points.first().x(), area.bottom());
        fill.closeSubpath();
        QColor wash = accent;
        wash.setAlpha(darkSurface() ? 38 : 26);
        painter.setPen(Qt::NoPen);
        painter.setBrush(wash);
        painter.drawPath(fill);
    }
    muted.setAlpha(190);
    painter.setPen(QPen(muted, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.setBrush(Qt::NoBrush);
    painter.drawPath(line);
    painter.setPen(Qt::NoPen);
    const QColor latest = statusColor(tile.data.status);
    painter.setBrush(latest.isValid() ? latest : accent);
    painter.drawEllipse(tile.points.last(), 3.0, 3.0);

    if (labelled) {
        painter.setFont(tickFont);
        painter.setPen(mutedColor());
        const QRect ticks(area.left(), area.bottom() + 2, area.width(), 12);
        painter.drawText(ticks, Qt::AlignLeft | Qt::AlignVCenter, series.labels.value(0));
        painter.drawText(ticks, Qt::AlignRight | Qt::AlignVCenter,
                         series.labels.value(series.labels.size() - 1));
        if (maxIndex > 0 && maxIndex < tile.points.size() - 1 && tile.plot.height() >= 90) {
            QColor dot = mutedColor();
            painter.setPen(Qt::NoPen);
            painter.setBrush(dot);
            painter.drawEllipse(tile.points.at(maxIndex), 2.0, 2.0);
        }
    }

    if (hoverPoint >= 0 && hoverPoint < tile.points.size()) {
        const QPointF point = tile.points.at(hoverPoint);
        QColor hair = mutedColor();
        hair.setAlpha(110);
        painter.setPen(QPen(hair, 1, Qt::DashLine));
        painter.drawLine(QPointF(point.x(), area.top()), QPointF(point.x(), area.bottom()));
        painter.setPen(QPen(surface, 2));
        painter.setBrush(accent);
        painter.drawEllipse(point, 4.5, 4.5);
    }
    painter.restore();
}

void StatsDashboard::paintControls(QPainter &painter, const Tile &tile) const
{
    QColor ink = mutedColor();
    const QColor surface = palette().color(QPalette::Button);
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    for (const QRect &button : {tile.sizeButton, tile.removeButton}) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(surface);
        painter.drawRoundedRect(button, 4, 4);
    }
    painter.setPen(QPen(ink, 1.4, Qt::SolidLine, Qt::RoundCap));
    painter.setBrush(Qt::NoBrush);
    // Size: a 2 × 2 of small squares.
    const QRect s = tile.sizeButton.adjusted(5, 5, -5, -5);
    const int half = s.width() / 2;
    painter.drawRect(QRect(s.left(), s.top(), half - 1, half - 1));
    painter.drawRect(QRect(s.left() + half + 1, s.top(), half - 1, half - 1));
    painter.drawRect(QRect(s.left(), s.top() + half + 1, half - 1, half - 1));
    painter.drawRect(QRect(s.left() + half + 1, s.top() + half + 1, half - 1, half - 1));
    // Remove: an X.
    const QRect x = tile.removeButton.adjusted(6, 6, -6, -6);
    painter.drawLine(x.topLeft(), x.bottomRight());
    painter.drawLine(x.topRight(), x.bottomLeft());
    painter.restore();
}

QString StatsDashboard::seriesTooltip(const Tile &tile, int index) const
{
    const StatsTileSeries &series = tile.data.series;
    if (index < 0 || index >= series.values.size())
        return QString();
    const QString label = series.labels.value(index);
    QString text = compactNumber(series.values.at(index));
    if (!tile.data.unit.isEmpty())
        text += QLatin1Char(' ') + tile.data.unit;
    const QString note = series.notes.value(index);
    if (!note.isEmpty())
        text += QString::fromUtf8(" \xC2\xB7 ") + note;
    return label.isEmpty() ? text : label + QStringLiteral(": ") + text;
}

void StatsDashboard::updateHover(const QPoint &pos)
{
    int tileIndex = -1;
    int pointIndex = -1;
    for (int i = 0; i < m_layout.size(); ++i) {
        const Tile &tile = m_layout.at(i);
        if (!tile.rect.contains(pos))
            continue;
        tileIndex = i;
        if (!tile.plot.isEmpty() && !tile.points.isEmpty() &&
            tile.plot.adjusted(-4, -4, 4, 4).contains(pos)) {
            qreal best = 1e9;
            for (int p = 0; p < tile.points.size(); ++p) {
                const qreal d = std::fabs(tile.points.at(p).x() - pos.x());
                if (d < best) {
                    best = d;
                    pointIndex = p;
                }
            }
        }
        break;
    }
    if (tileIndex == m_hoverTile && pointIndex == m_hoverPoint)
        return;
    m_hoverTile = tileIndex;
    m_hoverPoint = pointIndex;
    if (pointIndex >= 0) {
        const Tile &tile = m_layout.at(tileIndex);
        QToolTip::showText(mapToGlobal(pos + QPoint(12, 12)),
                           seriesTooltip(tile, pointIndex), this, tile.plot);
    } else {
        QToolTip::hideText();
    }
    update();
}

void StatsDashboard::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    const QPoint pos = event->pos();
    for (const Tile &tile : m_layout) {
        if (!tile.rect.contains(pos))
            continue;
        if (tile.removeButton.contains(pos)) {
            removeTile(tile.placement.id);
            return;
        }
        if (tile.sizeButton.contains(pos)) {
            showTileMenu(tile.placement.id, event->globalPosition().toPoint());
            return;
        }
        m_pressedTile = tile.placement.id;
        m_pressPos = pos;
        m_dragOffset = pos - tile.rect.topLeft();
        m_dragChanged = false;
        return;
    }
    QWidget::mousePressEvent(event);
}

void StatsDashboard::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint pos = event->pos();
    if (!m_pressedTile.isEmpty() && (event->buttons() & Qt::LeftButton)) {
        if (!m_dragging &&
            (pos - m_pressPos).manhattanLength() >= QApplication::startDragDistance()) {
            m_dragging = true;
            m_dragSkipTile.clear();
            m_hoverPoint = -1;
            QToolTip::hideText();
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_dragging) {
            dragTo(pos);
            return;
        }
    }
    updateHover(pos);
}

void StatsDashboard::dragTo(const QPoint &pos)
{
    m_dragPos = pos - m_dragOffset;
    if (!m_dragSkipTile.isEmpty()) {
        const QRect skip = tileRect(m_dragSkipTile);
        if (!skip.contains(pos))
            m_dragSkipTile.clear();
    }
    for (const Tile &tile : m_layout) {
        if (tile.placement.id == m_pressedTile || !tile.rect.contains(pos))
            continue;
        if (tile.placement.id == m_dragSkipTile)
            break;
        const int from = indexOf(m_pressedTile);
        const int to = indexOf(tile.placement.id);
        if (from >= 0 && to >= 0 && from != to) {
            m_tiles.move(from, to);
            m_dragSkipTile = tile.placement.id;
            m_dragChanged = true;
            relayout(width());
        }
        break;
    }
    update();
}

void StatsDashboard::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool wasDragging = m_dragging;
    const bool changed = m_dragChanged;
    const QString pressedTile = m_pressedTile;
    m_dragging = false;
    m_dragChanged = false;
    m_pressedTile.clear();
    m_dragSkipTile.clear();
    if (wasDragging) {
        unsetCursor();
        if (changed)
            notifyLayoutChanged();
        update();
    }
    updateHover(event->pos());
    if (!wasDragging && pointClicked && !pressedTile.isEmpty() &&
        m_hoverTile >= 0 && m_hoverTile < m_layout.size() && m_hoverPoint >= 0 &&
        m_layout.at(m_hoverTile).placement.id == pressedTile) {
        // Last: the handler may navigate away or refresh this board.
        pointClicked(pressedTile, m_hoverPoint);
    }
}

void StatsDashboard::leaveEvent(QEvent *event)
{
    QWidget::leaveEvent(event);
    if (m_dragging)
        return;
    m_hoverTile = -1;
    m_hoverPoint = -1;
    update();
}

bool StatsDashboard::event(QEvent *event)
{
    if (event->type() == QEvent::ToolTip) {
        auto *help = static_cast<QHelpEvent *>(event);
        const QPoint pos = help->pos();
        for (const Tile &tile : m_layout) {
            if (!tile.rect.contains(pos))
                continue;
            if (tile.removeButton.contains(pos)) {
                QToolTip::showText(help->globalPos(), QStringLiteral("Remove this tile"),
                                   this, tile.removeButton);
            } else if (tile.sizeButton.contains(pos)) {
                QToolTip::showText(help->globalPos(),
                                   QStringLiteral("Resize: one, two or four squares"),
                                   this, tile.sizeButton);
            } else if (m_hoverPoint < 0) {
                const StatsTileCatalogEntry *entry = catalogEntry(tile.placement.id);
                QString hint = entry ? entry->hint : QString();
                if (hint.isEmpty())
                    hint = entry ? entry->title : tile.placement.id;
                QStringList facts;
                if (tile.data.available && tile.data.status != StatsTileStatus::None)
                    facts << QStringLiteral("Status: ") + statusText(tile.data.status);
                double percent = 0;
                if (tile.data.available && trendPercent(tile.data.series, &percent))
                    facts << QStringLiteral("Trend: %1 against the older half of the chart")
                                 .arg(trendText(percent));
                const auto stamp = m_updatedAtMs.constFind(tile.placement.id);
                if (stamp != m_updatedAtMs.cend())
                    facts << QStringLiteral("Changed %1 ago").arg(compactDuration(
                        qMax<qint64>(0, QDateTime::currentMSecsSinceEpoch() - stamp.value())));
                if (!facts.isEmpty())
                    hint += QStringLiteral("\n\n") + facts.join(QLatin1Char('\n'));
                hint += QString::fromUtf8("\n\nDrag to move \xC2\xB7 right-click for "
                                          "size and removal");
                QToolTip::showText(help->globalPos(), hint, this, tile.rect);
            }
            return true;
        }
        QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}

void StatsDashboard::contextMenuEvent(QContextMenuEvent *event)
{
    const QString id = tileAt(event->pos());
    if (!id.isEmpty()) {
        showTileMenu(id, event->globalPos());
        return;
    }
    QMenu menu(this);
    QMenu *add = buildAddMenu(&menu);
    add->setTitle(QStringLiteral("Add tile"));
    menu.addMenu(add);
    QAction *reset = menu.addAction(QStringLiteral("Reset to default layout"));
    connect(reset, &QAction::triggered, this, [this] { resetLayout(); });
    menu.exec(event->globalPos());
}

void StatsDashboard::showTileMenu(const QString &id, const QPoint &globalPos)
{
    const int index = indexOf(id);
    if (index < 0)
        return;
    const StatsTileCatalogEntry *entry = catalogEntry(id);
    const StatsTilePlacement placement = m_tiles.at(index);
    QMenu menu(this);
    QAction *heading = menu.addAction(entry ? entry->title : id);
    heading->setEnabled(false);
    menu.addSeparator();
    const int sizes[][2] = {{1, 1}, {2, 1}, {1, 2}, {2, 2}};
    for (const auto &size : sizes) {
        const int w = size[0];
        const int h = size[1];
        QAction *action = menu.addAction(sizeLabel(w, h));
        action->setCheckable(true);
        action->setChecked(placement.width == w && placement.height == h);
        connect(action, &QAction::triggered, this,
                [this, id, w, h] { resizeTile(id, w, h); });
    }
    menu.addSeparator();
    QAction *earlier = menu.addAction(QStringLiteral("Move earlier"));
    earlier->setEnabled(index > 0);
    connect(earlier, &QAction::triggered, this,
            [this, id] { moveTile(id, indexOf(id) - 1); });
    QAction *later = menu.addAction(QStringLiteral("Move later"));
    later->setEnabled(index < m_tiles.size() - 1);
    connect(later, &QAction::triggered, this,
            [this, id] { moveTile(id, indexOf(id) + 1); });
    menu.addSeparator();
    QMenu *add = buildAddMenu(&menu);
    add->setTitle(QStringLiteral("Add tile"));
    menu.addMenu(add);
    QAction *remove = menu.addAction(QStringLiteral("Remove tile"));
    connect(remove, &QAction::triggered, this, [this, id] { removeTile(id); });
    menu.exec(globalPos);
}

QMenu *StatsDashboard::buildAddMenu(QWidget *parent)
{
    auto *menu = new QMenu(QStringLiteral("Add tile"), parent);
    QStringList groups;
    for (const StatsTileCatalogEntry &entry : m_catalog)
        if (!groups.contains(entry.group))
            groups.append(entry.group);
    int remaining = 0;
    for (const QString &group : groups) {
        QMenu *target = menu;
        if (!group.isEmpty() && groups.size() > 1)
            target = menu->addMenu(group);
        for (const StatsTileCatalogEntry &entry : m_catalog) {
            if (entry.group != group)
                continue;
            QAction *action = target->addAction(entry.title);
            action->setToolTip(entry.hint);
            const bool placed = hasTile(entry.id);
            action->setCheckable(true);
            action->setChecked(placed);
            action->setEnabled(!placed);
            if (!placed)
                ++remaining;
            const QString id = entry.id;
            connect(action, &QAction::triggered, this, [this, id] { addTile(id); });
        }
    }
    if (remaining == 0) {
        QAction *all = menu->addAction(QStringLiteral("Every stat is on the board"));
        all->setEnabled(false);
    }
    return menu;
}

QPoint StatsDashboard::tilePointPosition(const QString &id, int pointIndex) const
{
    for (const Tile &tile : m_layout) {
        if (tile.placement.id != id)
            continue;
        if (pointIndex < 0 || pointIndex >= tile.points.size())
            return QPoint();
        return tile.points.at(pointIndex).toPoint();
    }
    return QPoint();
}
