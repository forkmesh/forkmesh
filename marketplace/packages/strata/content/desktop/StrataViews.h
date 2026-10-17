#pragma once

#include "StrataModel.h"

#include <QAbstractScrollArea>
#include <QColor>
#include <QPainterPath>
#include <QPixmap>
#include <QPointer>
#include <QPolygonF>
#include <QWidget>

#include <functional>

class QLineEdit;
class QTextBrowser;
class QTreeWidget;
class QTreeWidgetItem;
class QVariantAnimation;

namespace forkmesh::strata {

struct Colors {
    bool dark = true;
    QColor background;
    QColor surface;
    QColor raised;
    QColor border;
    QColor text;
    QColor muted;
    QColor faint;
    QColor accent;
    QColor dependent;
    QColor healthy;
    QColor problematic;
    QColor unhealthy;
    static Colors from(const QWidget *widget);
};

QColor scaleColor(double t);
QColor healthColor(double health);
QColor ratingColor(int rating);
QColor severityColor(Severity severity);
QColor instabilityColor(double instability);
QColor mix(const QColor &a, const QColor &b, double t);

enum class SelectionKind { None, Module, File };

struct Selection {
    SelectionKind kind = SelectionKind::None;
    int index = -1;
    bool operator==(const Selection &other) const { return kind == other.kind && index == other.index; }
    bool operator!=(const Selection &other) const { return !(*this == other); }
};

struct ViewCallbacks {
    std::function<void(Selection)> select;
    std::function<void(int, int)> open;
    std::function<void(int, int)> cell;
    std::function<void(int)> showModule;
};

class GraphView : public QWidget
{
public:
    enum class ColorBy { Rating, Health, Instability, Complexity };

    explicit GraphView(QWidget *parent = nullptr);

    void setModel(const ModelPtr &model);
    void setCallbacks(const ViewCallbacks &callbacks) { m_callbacks = callbacks; }
    void setColorBy(ColorBy colorBy);
    ColorBy colorBy() const { return m_colorBy; }
    void setSelection(const Selection &selection);
    void setFilter(const QString &filter);
    void fit(bool animated = true);
    void drillInto(int module);
    void drillOut();
    int drilledModule() const { return m_drill.module; }
    bool settled() const;
    void setOnDrillChanged(std::function<void()> callback) { m_onDrillChanged = std::move(callback); }
    QString legendTitle() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    struct Item {
        QRectF rect;
        bool module = true;
        bool dependent = false;
        int index = -1;
        QString title;
        QString caption;
        QColor fill;
        bool cycle = false;
        bool match = true;
        bool external = false;
        QVector<QPair<QRectF, QColor>> cells;
    };
    struct Link {
        int from = -1;
        int to = -1;
        int weight = 1;
        bool cycle = false;
        double width = 1;
        QPainterPath path;
        QPolygonF arrow;
    };

    void rebuildScene();
    void recolor();
    QColor colorForModule(int module) const;
    QColor colorForFile(int file) const;
    int itemAt(const QPointF &position) const;
    QPointF toWorld(const QPointF &screen) const;
    QPointF toScreen(const QPointF &world) const;
    void invalidate();
    void ensureCache();
    void paintScene(QPainter &painter, const QRectF &world, bool dimmed) const;
    void paintItem(QPainter &painter, const Item &item, double scale, double opacity, bool raised) const;
    void paintLink(QPainter &painter, const Link &link, const QColor &color, double scale, double widthBoost) const;
    void paintLegend(QPainter &painter) const;
    void paintEmpty(QPainter &painter) const;
    void startHighlight();
    void animateTo(double scale, const QPointF &offset);
    void showTooltip(int item, const QPoint &global);
    int focusItem() const;

    ModelPtr m_model;
    ViewCallbacks m_callbacks;
    ColorBy m_colorBy = ColorBy::Rating;
    Selection m_selection;
    QString m_filter;
    DrillGraph m_drill;
    QVector<Item> m_items;
    QVector<Link> m_links;
    QRectF m_bounds;
    double m_scale = 1;
    QPointF m_offset;
    bool m_fitted = false;
    QPixmap m_cache;
    double m_cacheScale = 0;
    QPointF m_cacheOffset;
    QSize m_cacheSize;
    bool m_cacheValid = false;
    int m_hover = -1;
    int m_pressedItem = -1;
    QPoint m_pressPos;
    QPointF m_pressOffset;
    bool m_dragging = false;
    double m_highlight = 0;
    QVariantAnimation *m_highlightAnimation = nullptr;
    QVariantAnimation *m_viewAnimation = nullptr;
    double m_fromScale = 1;
    double m_toScale = 1;
    QPointF m_fromOffset;
    QPointF m_toOffset;
    std::function<void()> m_onDrillChanged;
};

class MatrixView : public QAbstractScrollArea
{
public:
    explicit MatrixView(QWidget *parent = nullptr);

    void setModel(const ModelPtr &model);
    void setCallbacks(const ViewCallbacks &callbacks) { m_callbacks = callbacks; }
    void setSelection(const Selection &selection);
    void setCell(int row, int column);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    void relayout();
    QPair<int, int> cellAt(const QPoint &position) const;
    int weight(int from, int to) const;

    ModelPtr m_model;
    ViewCallbacks m_callbacks;
    Selection m_selection;
    QHash<qint64, int> m_weights;
    int m_maxWeight = 1;
    int m_cell = 20;
    int m_header = 160;
    int m_hoverRow = -1;
    int m_hoverColumn = -1;
    int m_cellRow = -1;
    int m_cellColumn = -1;
};

class ScatterView : public QWidget
{
public:
    explicit ScatterView(QWidget *parent = nullptr);

    void setModel(const ModelPtr &model);
    void setCallbacks(const ViewCallbacks &callbacks) { m_callbacks = callbacks; }
    void setSelection(const Selection &selection);
    void setFilter(const QString &filter);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    QRectF plotRect() const;
    QPointF pointFor(int module) const;
    double radiusFor(int module) const;
    int moduleAt(const QPointF &position) const;
    void ensureBackground();

    ModelPtr m_model;
    ViewCallbacks m_callbacks;
    Selection m_selection;
    QString m_filter;
    QPixmap m_background;
    int m_hover = -1;
    QVector<int> m_drawOrder;
    double m_maxLines = 1;
};

class MetricsView : public QWidget
{
public:
    enum class SizeBy { Lines, Complexity, Functions };
    enum class ColorBy { Health, MaxComplexity, Debt, ChangeFrequency };

    explicit MetricsView(QWidget *parent = nullptr);

    void setModel(const ModelPtr &model);
    void setCallbacks(const ViewCallbacks &callbacks) { m_callbacks = callbacks; }
    void setSizeBy(SizeBy sizeBy);
    void setColorBy(ColorBy colorBy);
    void setSelection(const Selection &selection);
    void setFilter(const QString &filter);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void relayout();
    void ensureCache();
    double sizeOf(int file) const;
    double colorValue(int file) const;
    QColor colorOf(int file) const;
    int fileAt(const QPointF &position) const;
    QRectF mapRect() const;

    ModelPtr m_model;
    ViewCallbacks m_callbacks;
    SizeBy m_sizeBy = SizeBy::Lines;
    ColorBy m_colorBy = ColorBy::Health;
    Selection m_selection;
    QString m_filter;
    QVector<QRectF> m_fileRects;
    QVector<QRectF> m_moduleRects;
    QVector<int> m_moduleOfRect;
    QPixmap m_cache;
    bool m_cacheValid = false;
    int m_hover = -1;
    double m_maxCommits = 1;
};

class DashboardView : public QAbstractScrollArea
{
public:
    explicit DashboardView(QWidget *parent = nullptr);

    void setModel(const ModelPtr &model);
    void setCallbacks(const ViewCallbacks &callbacks) { m_callbacks = callbacks; }

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void scrollContentsBy(int dx, int dy) override;

private:
    struct Hit {
        QRectF rect;
        Selection selection;
    };
    int contentHeight(int width);
    void paintContent(QPainter &painter, int width);
    int hitAt(const QPoint &position) const;

    ModelPtr m_model;
    ViewCallbacks m_callbacks;
    QVector<Hit> m_hits;
    int m_hover = -1;
    int m_contentHeight = 0;
};

class RulesView : public QWidget
{
public:
    explicit RulesView(QWidget *parent = nullptr);

    void setModel(const ModelPtr &model);
    void setCallbacks(const ViewCallbacks &callbacks) { m_callbacks = callbacks; }
    void setFilter(const QString &filter);
    void focusFilter();
    int visibleRules() const;

private:
    void rebuild();
    void populate(QTreeWidgetItem *ruleItem);
    void describe(int rule);
    bool violationMatches(const Violation &violation) const;

    ModelPtr m_model;
    ViewCallbacks m_callbacks;
    QLineEdit *m_filter = nullptr;
    QTreeWidget *m_tree = nullptr;
    QTextBrowser *m_description = nullptr;
    QString m_text;
};

}
