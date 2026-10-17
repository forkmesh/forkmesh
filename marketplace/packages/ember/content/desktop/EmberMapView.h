#pragma once

#include "EmberModel.h"

#include <QElapsedTimer>
#include <QPixmap>
#include <QPointer>
#include <QRectF>
#include <QTimer>
#include <QWidget>

#include <functional>
#include <memory>

namespace forkmesh::ember {

class EmberMapView : public QWidget
{
public:
    explicit EmberMapView(QWidget *parent = nullptr);

    void setData(insight::SnapshotPtr snapshot, MapLayoutPtr layout,
                 std::shared_ptr<const CouplingIndex> coupling);
    void clearData();
    void setColoring(const TabColoring &coloring);
    void setMessage(const QString &text);
    void setSelectedFile(int file, bool zoomTo);
    void focusNode(int node, bool animate);
    void zoomOut();
    void zoomBy(double factor);
    void finishAnimation();

    int selectedFile() const { return m_selected; }
    int hoveredNode() const { return m_hover; }
    int focus() const { return m_focus; }
    QVector<int> focusChain() const;
    bool animating() const { return m_transition; }
    ViewTransform transform() const { return m_view; }
    MapLayoutPtr layout() const { return m_layout; }
    int labelsDrawn() const { return m_labelsDrawn; }
    int circlesDrawn() const { return m_circlesDrawn; }
    QPointF toScreen(const QPointF &world) const;
    QPointF toWorld(const QPointF &screen) const;
    double screenRadius(int node) const;

    void setOnSelect(std::function<void(int)> callback) { m_onSelect = std::move(callback); }
    void setOnFocusChanged(std::function<void(int)> callback) { m_onFocus = std::move(callback); }
    void setOnOpenFile(std::function<void(int)> callback) { m_onOpen = std::move(callback); }

    void clickAt(const QPointF &position);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    bool dark() const;
    QColor background() const;
    void invalidate();
    void recolor();
    void paintScene(QPainter &painter, bool fast);
    void paintLabels(QPainter &painter, const QVector<int> &files, const QVector<int> &folders,
                     bool fast);
    void paintCurvedLabel(QPainter &painter, const QPointF &center, double radius,
                          const QString &text, const QFont &font, const QColor &color, bool halo);
    void paintOverlay(QPainter &painter);
    void paintLinks(QPainter &painter);
    void paintBadges(QPainter &painter);
    void paintLegend(QPainter &painter);
    void paintControls(QPainter &painter);
    void paintNote(QPainter &painter, const QString &text, bool center);
    QString metricLabel(int file) const;
    QString tooltipFor(int node) const;
    void startTransition(const ViewTransform &target);
    void tick();
    void updateTimer();
    void beginInteraction();
    void settle();
    void updateFocusFromView();
    double minScale() const;
    double maxScale() const;
    int controlAt(const QPointF &position) const;
    QRectF controlRect(int index) const;

    insight::SnapshotPtr m_snapshot;
    MapLayoutPtr m_layout;
    std::shared_ptr<const CouplingIndex> m_coupling;
    TabColoring m_coloring;
    QVector<QColor> m_fill;
    QVector<QColor> m_light;
    QVector<QColor> m_shade;
    QString m_message;
    ViewTransform m_view;
    ViewTransform m_from;
    ViewTransform m_to;
    bool m_transition = false;
    QElapsedTimer m_transitionClock;
    QElapsedTimer m_clock;
    double m_linkProgress = 1;
    qint64 m_lastTick = 0;
    QTimer m_timer;
    QTimer m_settleTimer;
    QPixmap m_static;
    bool m_dirty = true;
    bool m_fast = false;
    bool m_colorDark = true;
    bool m_glowVisible = false;
    int m_focus = 0;
    int m_selected = -1;
    int m_hover = -1;
    int m_hoverControl = -1;
    bool m_pressed = false;
    bool m_dragging = false;
    QPointF m_pressPos;
    QPointF m_lastPos;
    QRectF m_legendRect;
    QRectF m_infoRect;
    int m_labelsDrawn = 0;
    int m_circlesDrawn = 0;
    std::function<void(int)> m_onSelect;
    std::function<void(int)> m_onFocus;
    std::function<void(int)> m_onOpen;
};

} // namespace forkmesh::ember
