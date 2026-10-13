#pragma once

#include <QColor>
#include <QHash>
#include <QJsonArray>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <functional>

class QMenu;
class QTimer;

// How a reading stands. A status paints as a coloured edge and a faint wash
// on the tile (and colours categorical bars through their tones); the number
// and note beside it always say why, so colour never carries meaning alone.
enum class StatsTileStatus { None, Good, Warning, Critical };

// Which way a time series should move. The trend chip beside the headline is
// green when the series moves the good way, red when it moves the bad way and
// grey for Neutral.
enum class StatsTrendPolarity { Neutral, UpIsGood, DownIsGood };

// One statistic the dashboard can show. The catalog is supplied by the owner
// (MainWindow knows every store); the dashboard only knows ids and titles.
struct StatsTileCatalogEntry {
    QString id;
    QString title;
    QString group; // "Repositories", "Agents", ...: submenu in the Add menu
    QString hint;  // tooltip text explaining the number
};

// A tile's current reading. `series` is optional: a time series paints as a
// sparkline / area, a categorical series (labels are names, not periods)
// paints as bars.
struct StatsTileSeries {
    QStringList labels;
    QVector<double> values;
    bool categorical = false;
    // Optional, one per value: bars whose label means good or bad news
    // ("Failed", "Conflicts") paint in the status colour instead of the accent.
    QVector<StatsTileStatus> tones;
    QStringList notes; // optional, one per point, appended to its tooltip
    bool isEmpty() const { return values.isEmpty(); }
};

struct StatsTileData {
    QString value;             // headline, already formatted ("12.9K")
    QString note;              // one line of context ("open · 4 assigned")
    QString unit;              // tooltip suffix for series points ("tokens")
    StatsTileSeries series;
    bool available = true;     // false paints "No data yet"
    StatsTileStatus status = StatsTileStatus::None;
    StatsTrendPolarity trend = StatsTrendPolarity::Neutral;
};

// Where a tile sits: an ordered list of (id, width, height) in grid squares.
// Position is implied by order and first-fit packing, so moving a tile means
// reordering the list, and the same layout reflows to any column count.
struct StatsTilePlacement {
    QString id;
    int width = 1;  // 1 or 2 squares
    int height = 1; // 1 or 2 squares
};

// BI-style grid of stat tiles: big numbers with charts, each one square, two
// squares (wide or tall) or four squares (2 × 2); every square is a true
// square, sized by the column width. Tiles can be added from a
// catalog, removed, resized and dragged into a new order; the owner persists
// the layout through layoutChanged. Plain QWidget (no moc): callbacks are
// std::function, like AgentToolEfficiencyBoard.
class StatsDashboard : public QWidget
{
public:
    explicit StatsDashboard(QWidget *parent = nullptr);

    void setCatalog(const QVector<StatsTileCatalogEntry> &catalog);
    const QVector<StatsTileCatalogEntry> &catalog() const { return m_catalog; }
    void setDataProvider(std::function<StatsTileData(const QString &id)> provider);
    // The layout a first launch (or Reset) starts from.
    void setDefaultLayout(const QVector<StatsTilePlacement> &layout);
    void setLayoutModel(const QVector<StatsTilePlacement> &layout);
    QVector<StatsTilePlacement> layoutModel() const { return m_tiles; }
    void resetLayout();

    // Edits. Each returns false when the id is unknown / already placed /
    // the size is out of range; every successful edit fires layoutChanged.
    bool addTile(const QString &id, int width = 1, int height = 1);
    bool removeTile(const QString &id);
    bool resizeTile(const QString &id, int width, int height);
    bool moveTile(const QString &id, int toIndex);
    bool hasTile(const QString &id) const { return indexOf(id) >= 0; }
    QStringList tileIds() const;
    std::function<void()> layoutChanged;
    // A click on one point of a tile's chart (a bar, or a sample of a line)
    // that did not turn into a drag.
    std::function<void(const QString &id, int pointIndex)> pointClicked;
    // Where a point of a placed tile's chart is drawn, in widget coordinates;
    // a null point when the tile or the point is not laid out.
    QPoint tilePointPosition(const QString &id, int pointIndex) const;

    // Re-read every placed tile through the provider and repaint.
    void refresh();

    // Menu of catalog entries not yet on the board, grouped; picking one adds
    // it. The caller owns the returned menu.
    QMenu *buildAddMenu(QWidget *parent);
    void showTileMenu(const QString &id, const QPoint &globalPos);

    // JSON persistence: [{"id":"repos","w":2,"h":1}, ...].
    static QJsonArray layoutToJson(const QVector<StatsTilePlacement> &layout);
    static QVector<StatsTilePlacement> layoutFromJson(const QJsonArray &array);

    // Merges a saved board with the current catalog. Every catalog tile the
    // board has never been offered is appended in default order; a tile that
    // was offered and later removed stays removed. Ids the catalog does not
    // know (a tile from a newer or older build) are kept, not dropped, so one
    // build cannot erase another's tiles. `offered` comes back as every id the
    // board has now been offered; `changed` says the result must be saved.
    struct LayoutMerge {
        QVector<StatsTilePlacement> layout;
        QStringList offered;
        bool changed = false;
    };
    static LayoutMerge mergeSavedLayout(
        const QVector<StatsTilePlacement> &saved, bool hasSaved,
        const QVector<StatsTilePlacement> &defaults,
        const QVector<StatsTileCatalogEntry> &catalog,
        const QStringList &offered);

    // First-fit packing on a grid `columns` wide: returns one cell rect
    // (x = column, y = row, in squares) per placement, in order. Tiles wider
    // than the grid are clamped to it.
    static QVector<QRect> packLayout(const QVector<StatsTilePlacement> &layout,
                                     int columns);

    static QString compactNumber(double value);
    static QString compactBytes(qint64 bytes);
    static QString compactUsd(double value);
    static QString compactDuration(qint64 ms);

    // Percent change of a time series: the mean of its newer half against the
    // mean of its older half. False for categorical series, fewer than four
    // points, or an older half of zero (no baseline to compare with).
    static bool trendPercent(const StatsTileSeries &series, double *percent);

    // A tile whose headline value (or availability) changed on a refresh shows
    // a small dot that pulses and fades out over kUpdateFadeMs.
    static constexpr qint64 kUpdateFadeMs = 10'000;

    // Probes for tests and tooltips.
    StatsTileStatus tileStatus(const QString &id) const;
    bool tileRecentlyUpdated(const QString &id) const;
    int columns() const { return m_columns; }
    int contentHeight() const { return m_contentHeight; }
    QRect tileRect(const QString &id) const;
    QString tileAt(const QPoint &point) const;
    QString tileValue(const QString &id) const;
    QRect tileRemoveRect(const QString &id) const;
    QRect tileSizeRect(const QString &id) const;
    // Squares of the grid, so a test can convert cell rects to pixels. A cell
    // is a true square whose side is the column width, never narrower than
    // kMinSquareWidth; kSquareHeight is that same minimum side.
    static constexpr int kMinSquareWidth = 176;
    static constexpr int kSquareHeight = kMinSquareWidth;
    static constexpr int kGap = 12;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Tile {
        StatsTilePlacement placement;
        StatsTileData data;
        QRect cell;   // squares
        QRect rect;   // pixels
        QRect plot;   // pixels, empty when the tile has no series
        QRect removeButton;
        QRect sizeButton;
        QVector<QPointF> points; // painted series positions, for hover
    };

    int indexOf(const QString &id) const;
    const StatsTileCatalogEntry *catalogEntry(const QString &id) const;
    void relayout(int width);
    void notifyLayoutChanged();
    void paintTile(QPainter &painter, const Tile &tile, bool hovered,
                   bool ghost) const;
    void paintSeries(QPainter &painter, const Tile &tile, int hoverPoint) const;
    void computePoints(Tile &tile) const;
    void paintControls(QPainter &painter, const Tile &tile) const;
    void paintEmptyState(QPainter &painter) const;
    void updateHover(const QPoint &pos);
    void dragTo(const QPoint &pos);
    bool darkSurface() const;
    QColor accentColor() const;
    QColor mutedColor() const;
    QString seriesTooltip(const Tile &tile, int index) const;
    // Fraction of the update dot still showing (1 just changed, 0 gone).
    double updateFade(const QString &id, qint64 now) const;
    QRect updateDotRect(const Tile &tile) const;
    void tickUpdateFade();

    QVector<StatsTileCatalogEntry> m_catalog;
    // id -> index into m_catalog. paintTile() resolves a tile's catalog entry
    // on every tile of every repaint, and the default layout places the whole
    // catalog, so the old linear scan made painting O(tiles x catalog) string
    // comparisons — QtPrivate::equalStrings was the sampled frame in a 15.8 s
    // stall record.
    QHash<QString, int> m_catalogIndex;
    QVector<StatsTilePlacement> m_tiles;
    QVector<StatsTilePlacement> m_defaultLayout;
    QVector<Tile> m_layout;
    QHash<QString, StatsTileData> m_data;
    QHash<QString, qint64> m_updatedAtMs; // id -> when its headline last changed
    QTimer *m_fadeTimer = nullptr;
    std::function<StatsTileData(const QString &)> m_provider;
    int m_columns = 1;
    int m_contentHeight = 0;
    int m_layoutWidth = -1;
    int m_hoverTile = -1;
    int m_hoverPoint = -1;
    // Drag state: the pressed tile, where the press landed inside it, and
    // whether the press has travelled far enough to count as a drag.
    QString m_pressedTile;
    QPoint m_pressPos;
    QPoint m_dragOffset;
    QPoint m_dragPos;
    bool m_dragging = false;
    bool m_dragChanged = false;
    QString m_dragSkipTile; // the tile just swapped with, until the cursor leaves it
};
