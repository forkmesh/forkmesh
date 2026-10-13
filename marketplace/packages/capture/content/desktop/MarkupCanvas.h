#pragma once

#include <QColor>
#include <QImage>
#include <QPoint>
#include <QRect>
#include <QSize>
#include <QString>
#include <QVector>
#include <QWidget>

class QPainter;

enum class MarkupTool { Pencil, Rect, Ellipse, Line, Arrow, Text, Move };

struct MarkupOp {
    enum class Kind { Stroke, Shape, Text, Image };
    Kind kind = Kind::Stroke;
    QColor color;
    QVector<QPoint> points; // used when kind == Stroke
    MarkupTool shapeType = MarkupTool::Rect; // used when kind == Shape
    QRect rect; // used when shapeType is Rect or Ellipse
    bool filled = false; // used when kind == Shape
    QPoint p1, p2; // used when shapeType is Line or Arrow (p2 is the arrow's head)
    QPoint textPos; // used when kind == Text (top-left origin)
    QString text; // used when kind == Text
    int textPointSize = 18; // used when kind == Text
    QImage image; // used when kind == Image
};

class MarkupCanvas : public QWidget
{
    Q_OBJECT
public:
    explicit MarkupCanvas(const QImage &base, QWidget *parent = nullptr);

    void setTool(MarkupTool tool);
    void setColor(const QColor &color);
    void setShapeFilled(bool filled);
    void setTextPointSize(int pointSize);
    bool pasteImage(const QImage &image);
    void undo();

    QImage flattenedImage() const;

    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    static void renderOp(QPainter &painter, const MarkupOp &op);
    static void renderOps(QPainter &painter, const QVector<MarkupOp> &ops);
    static QRect textBounds(const MarkupOp &op);
    static QRect layerBounds(const MarkupOp &op);
    static QRect resizeHandleRect(const MarkupOp &op);
    static bool isMovableLayer(const MarkupOp &op);
    int movableLayerAt(const QPoint &position, bool includeImages) const;
    void pushUndoState();

    QImage m_base;
    QVector<MarkupOp> m_ops;
    QVector<QVector<MarkupOp>> m_undoStack;
    MarkupOp m_current;
    bool m_drawing = false;
    QPoint m_dragOrigin;
    MarkupTool m_tool = MarkupTool::Pencil;
    QColor m_color;
    bool m_shapeFilled = false;
    int m_textPointSize = 18;
    int m_movingOpIndex = -1;
    int m_selectedOpIndex = -1;
    QPoint m_moveDragOffset;
    bool m_moveChanged = false;
    int m_resizingOpIndex = -1;
    QRect m_resizeStartBounds;
    int m_resizeStartTextPointSize = 18;
    bool m_resizeChanged = false;
};
