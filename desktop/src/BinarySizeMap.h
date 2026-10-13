#pragma once

#include "BinarySize.h"

#include <QRectF>
#include <QVector>
#include <QWidget>

#include <functional>

class BinarySizeMap : public QWidget
{
public:
    explicit BinarySizeMap(QWidget *parent = nullptr);

    void setReport(forkmesh::binarysize::Report report);
    const forkmesh::binarysize::Report &report() const { return m_report; }

    int focusNode() const { return m_focus; }
    void setFocusNode(int node);
    bool zoomOut();
    QVector<int> chainTo(int node) const;
    void setOnFocusChanged(std::function<void(int)> handler) { m_onFocusChanged = std::move(handler); }

    int nodeAt(const QPoint &position) const;
    QRectF cellRect(int node) const;
    QColor categoryColor(int category) const;

    static QVector<QRectF> squarify(const QVector<double> &weights, const QRectF &area);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Cell {
        int node = -1;
        QRectF rect;
        int depth = 1;
    };

    void relayout();
    void layoutChildren(int parent, const QRectF &area, int depth);
    void paintCell(QPainter &painter, const Cell &cell) const;
    void paintLegend(QPainter &painter) const;
    bool dark() const;
    QString tooltipFor(int node) const;

    forkmesh::binarysize::Report m_report;
    QVector<Cell> m_cells;
    QRectF m_legend;
    int m_focus = 0;
    int m_hover = -1;
    std::function<void(int)> m_onFocusChanged;
};
