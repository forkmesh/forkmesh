#include "MarkupCanvas.h"

#include <QBrush>
#include <QFont>
#include <QFontMetrics>
#include <QInputDialog>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPolygonF>
#include <QStringList>

#include <cmath>

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr int kMinTextPointSize = 8;
constexpr int kMaxTextPointSize = 144;
constexpr int kMinLayerSize = 8;
constexpr int kResizeHandleSize = 10;

bool isDirectional(MarkupTool tool)
{
    return tool == MarkupTool::Line || tool == MarkupTool::Arrow;
}

QStringList textLines(const QString &text)
{
    return text.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
}
}

MarkupCanvas::MarkupCanvas(const QImage &base, QWidget *parent)
    : QWidget(parent), m_base(base), m_color(QColor(255, 50, 50))
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    setCursor(Qt::CrossCursor);
}

void MarkupCanvas::setTool(MarkupTool tool)
{
    m_tool = tool;
    m_movingOpIndex = -1;
    m_resizingOpIndex = -1;
    if (tool == MarkupTool::Move)
        setCursor(Qt::OpenHandCursor);
    else
        setCursor(tool == MarkupTool::Text ? Qt::IBeamCursor : Qt::CrossCursor);
    if (tool != MarkupTool::Move && tool != MarkupTool::Text)
        m_selectedOpIndex = -1;
    update();
}

void MarkupCanvas::setColor(const QColor &color)
{
    m_color = color;
}

void MarkupCanvas::setShapeFilled(bool filled)
{
    m_shapeFilled = filled;
}

void MarkupCanvas::setTextPointSize(int pointSize)
{
    m_textPointSize = qBound(kMinTextPointSize, pointSize, kMaxTextPointSize);
}

bool MarkupCanvas::pasteImage(const QImage &source)
{
    if (source.isNull())
        return false;

    QImage image = source.convertToFormat(QImage::Format_ARGB32);
    // The screenshot and canvas use image pixels as their coordinate system.
    // Clipboard images may carry a device-pixel ratio from their source, so
    // normalize that metadata before assigning a target rectangle.
    image.setDevicePixelRatio(1.0);

    QSize targetSize = image.size();
    const QSize maxSize(qMax(kMinLayerSize, (m_base.width() * 3) / 4),
                        qMax(kMinLayerSize, (m_base.height() * 3) / 4));
    if (targetSize.width() > maxSize.width() || targetSize.height() > maxSize.height())
        targetSize.scale(maxSize, Qt::KeepAspectRatio);

    MarkupOp op;
    op.kind = MarkupOp::Kind::Image;
    op.image = image;
    op.rect = QRect(QPoint(), targetSize);
    op.rect.moveCenter(QRect(QPoint(), m_base.size()).center());

    pushUndoState();
    m_ops.append(op);
    m_selectedOpIndex = m_ops.size() - 1;
    update();
    return true;
}

void MarkupCanvas::pushUndoState()
{
    m_undoStack.append(m_ops);
    constexpr int kMaxUndoStates = 100;
    if (m_undoStack.size() > kMaxUndoStates)
        m_undoStack.removeFirst();
}

void MarkupCanvas::undo()
{
    if (m_undoStack.isEmpty())
        return;
    m_ops = m_undoStack.takeLast();
    m_drawing = false;
    m_movingOpIndex = -1;
    m_resizingOpIndex = -1;
    m_selectedOpIndex = -1;
    update();
}

QImage MarkupCanvas::flattenedImage() const
{
    QImage result = m_base.convertToFormat(QImage::Format_ARGB32);
    QPainter p(&result);
    renderOps(p, m_ops);
    return result;
}

QSize MarkupCanvas::sizeHint() const
{
    return m_base.size();
}

void MarkupCanvas::paintEvent(QPaintEvent *)
{
    QPainter painter(this);
    painter.drawImage(0, 0, m_base);
    renderOps(painter, m_ops);
    if (m_drawing)
        renderOp(painter, m_current);
    if (m_selectedOpIndex >= 0 && m_selectedOpIndex < m_ops.size()) {
        const MarkupOp &selected = m_ops.at(m_selectedOpIndex);
        if (isMovableLayer(selected)) {
            const QRect bounds = layerBounds(selected);
            QPen selectionPen(QColor(46, 160, 67));
            selectionPen.setWidth(1);
            selectionPen.setStyle(Qt::DashLine);
            painter.setPen(selectionPen);
            painter.setBrush(QColor(46, 160, 67, 28));
            painter.drawRoundedRect(bounds.adjusted(-5, -4, 5, 4), 3, 3);

            const QRect handle = resizeHandleRect(selected);
            painter.setPen(QPen(QColor(46, 160, 67), 1));
            painter.setBrush(Qt::white);
            painter.drawRect(handle);
        }
    }
}

void MarkupCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() != Qt::LeftButton)
        return;

    const QPoint position = event->position().toPoint();
    if (m_tool == MarkupTool::Text || m_tool == MarkupTool::Move) {
        const bool includeImages = m_tool == MarkupTool::Move;
        if (m_selectedOpIndex >= 0 && m_selectedOpIndex < m_ops.size()) {
            const MarkupOp &selected = m_ops.at(m_selectedOpIndex);
            if ((includeImages || selected.kind == MarkupOp::Kind::Text) &&
                isMovableLayer(selected) && resizeHandleRect(selected).contains(position)) {
                m_resizingOpIndex = m_selectedOpIndex;
                m_resizeStartBounds = layerBounds(selected);
                m_resizeStartTextPointSize = selected.textPointSize;
                m_resizeChanged = false;
                setCursor(Qt::SizeFDiagCursor);
                return;
            }
        }

        const int hit = movableLayerAt(position, includeImages);
        if (hit >= 0) {
            m_movingOpIndex = hit;
            m_selectedOpIndex = hit;
            const MarkupOp &op = m_ops.at(hit);
            const QPoint anchor = op.kind == MarkupOp::Kind::Text
                                      ? op.textPos
                                      : op.rect.topLeft();
            m_moveDragOffset = position - anchor;
            m_moveChanged = false;
            setCursor(Qt::ClosedHandCursor);
            update();
            return;
        }

        m_selectedOpIndex = -1;
        if (m_tool == MarkupTool::Move) {
            update();
            return;
        }

        bool ok = false;
        const QString text = QInputDialog::getMultiLineText(
            this, tr("Add Text"), tr("Text:"), QString(), &ok);
        if (ok && !text.trimmed().isEmpty()) {
            MarkupOp op;
            op.kind = MarkupOp::Kind::Text;
            op.color = m_color;
            op.textPos = position;
            op.text = text;
            op.textPointSize = m_textPointSize;
            pushUndoState();
            m_ops.append(op);
            m_selectedOpIndex = m_ops.size() - 1;
            update();
        }
        return;
    }

    m_drawing = true;
    m_current = MarkupOp{};
    m_current.color = m_color;
    if (m_tool == MarkupTool::Pencil) {
        m_current.kind = MarkupOp::Kind::Stroke;
        m_current.points.append(event->position().toPoint());
    } else {
        m_current.kind = MarkupOp::Kind::Shape;
        m_current.shapeType = m_tool;
        m_current.filled = m_shapeFilled;
        m_dragOrigin = event->position().toPoint();
        m_current.rect = QRect(m_dragOrigin, m_dragOrigin);
        m_current.p1 = m_dragOrigin;
        m_current.p2 = m_dragOrigin;
    }
    update();
}

void MarkupCanvas::mouseMoveEvent(QMouseEvent *event)
{
    const QPoint position = event->position().toPoint();

    if (m_resizingOpIndex >= 0 && m_resizingOpIndex < m_ops.size()) {
        const MarkupOp &op = m_ops.at(m_resizingOpIndex);
        if (op.kind == MarkupOp::Kind::Text) {
            const qreal horizontalScale =
                qreal(position.x() - m_resizeStartBounds.left() + 1) /
                qMax(1, m_resizeStartBounds.width());
            const qreal verticalScale =
                qreal(position.y() - m_resizeStartBounds.top() + 1) /
                qMax(1, m_resizeStartBounds.height());
            const qreal scale = qMax(0.1, qMax(horizontalScale, verticalScale));
            const int pointSize = qBound(
                kMinTextPointSize,
                qRound(m_resizeStartTextPointSize * scale),
                kMaxTextPointSize);
            if (pointSize != op.textPointSize) {
                if (!m_resizeChanged) {
                    pushUndoState();
                    m_resizeChanged = true;
                }
                m_ops[m_resizingOpIndex].textPointSize = pointSize;
            }
        } else if (op.kind == MarkupOp::Kind::Image) {
            const qreal horizontalScale =
                qreal(position.x() - m_resizeStartBounds.left() + 1) /
                qMax(1, m_resizeStartBounds.width());
            const qreal verticalScale =
                qreal(position.y() - m_resizeStartBounds.top() + 1) /
                qMax(1, m_resizeStartBounds.height());
            const qreal scale = qMax(0.1, qMax(horizontalScale, verticalScale));
            const QSize newSize(
                qMax(kMinLayerSize, qRound(m_resizeStartBounds.width() * scale)),
                qMax(kMinLayerSize, qRound(m_resizeStartBounds.height() * scale)));
            const QRect resized(m_resizeStartBounds.topLeft(), newSize);
            if (resized != op.rect) {
                if (!m_resizeChanged) {
                    pushUndoState();
                    m_resizeChanged = true;
                }
                m_ops[m_resizingOpIndex].rect = resized;
            }
        }
        update();
        return;
    }

    if (m_movingOpIndex >= 0 && m_movingOpIndex < m_ops.size()) {
        const MarkupOp &op = m_ops.at(m_movingOpIndex);
        const QPoint anchor = position - m_moveDragOffset;
        if (op.kind == MarkupOp::Kind::Text && anchor != op.textPos) {
            if (!m_moveChanged) {
                pushUndoState();
                m_moveChanged = true;
            }
            m_ops[m_movingOpIndex].textPos = anchor;
        } else if (op.kind == MarkupOp::Kind::Image && anchor != op.rect.topLeft()) {
            if (!m_moveChanged) {
                pushUndoState();
                m_moveChanged = true;
            }
            m_ops[m_movingOpIndex].rect.moveTopLeft(anchor);
        }
        update();
        return;
    }

    if (!m_drawing)
        return;
    if (m_current.kind == MarkupOp::Kind::Stroke) {
        m_current.points.append(position);
    } else if (isDirectional(m_current.shapeType)) {
        m_current.p2 = position; // p1 stays anchored at the drag origin
    } else {
        m_current.rect = QRect(m_dragOrigin, position).normalized();
    }
    update();
}

void MarkupCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_resizingOpIndex >= 0 && event->button() == Qt::LeftButton) {
        m_resizingOpIndex = -1;
        setCursor(m_tool == MarkupTool::Move ? Qt::OpenHandCursor : Qt::IBeamCursor);
        update();
        return;
    }
    if (m_movingOpIndex >= 0 && event->button() == Qt::LeftButton) {
        m_movingOpIndex = -1;
        setCursor(m_tool == MarkupTool::Move ? Qt::OpenHandCursor : Qt::IBeamCursor);
        update();
        return;
    }
    if (!m_drawing || event->button() != Qt::LeftButton)
        return;

    m_drawing = false;
    const QPoint position = event->position().toPoint();
    if (m_current.kind == MarkupOp::Kind::Stroke) {
        m_current.points.append(position);
        if (!m_current.points.isEmpty()) {
            pushUndoState();
            m_ops.append(m_current);
        }
    } else if (isDirectional(m_current.shapeType)) {
        m_current.p2 = position;
        const QPoint delta = m_current.p2 - m_current.p1;
        if (delta.manhattanLength() >= 3) {
            pushUndoState();
            m_ops.append(m_current);
        }
    } else {
        m_current.rect = QRect(m_dragOrigin, position).normalized();
        if (m_current.rect.width() >= 3 && m_current.rect.height() >= 3) {
            pushUndoState();
            m_ops.append(m_current);
        }
    }
    m_current = MarkupOp{};
    update();
}

void MarkupCanvas::renderOp(QPainter &painter, const MarkupOp &op)
{
    painter.save();
    QPen pen(op.color);
    pen.setWidth(3);
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    painter.setPen(pen);
    painter.setBrush(Qt::NoBrush);
    painter.setRenderHint(QPainter::Antialiasing, true);

    if (op.kind == MarkupOp::Kind::Stroke) {
        if (op.points.size() == 1)
            painter.drawPoint(op.points.first());
        else if (op.points.size() > 1)
            painter.drawPolyline(op.points.constData(), op.points.size());
    } else if (op.kind == MarkupOp::Kind::Text) {
        QFont font = painter.font();
        font.setPointSize(op.textPointSize);
        font.setBold(true);
        painter.setFont(font);
        const QFontMetrics metrics(font);
        const QStringList lines = textLines(op.text);
        for (qsizetype i = 0; i < lines.size(); ++i) {
            painter.drawText(op.textPos + QPoint(0, metrics.ascent() +
                                                    int(i) * metrics.lineSpacing()),
                             lines.at(i));
        }
    } else if (op.kind == MarkupOp::Kind::Image) {
        painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
        painter.drawImage(op.rect, op.image);
    } else if (op.shapeType == MarkupTool::Rect) {
        painter.setBrush(op.filled ? QBrush(op.color) : QBrush(Qt::NoBrush));
        painter.drawRect(op.rect);
    } else if (op.shapeType == MarkupTool::Ellipse) {
        painter.setBrush(op.filled ? QBrush(op.color) : QBrush(Qt::NoBrush));
        painter.drawEllipse(op.rect);
    } else if (op.shapeType == MarkupTool::Line) {
        painter.drawLine(op.p1, op.p2);
    } else { // Arrow: a shaft plus a filled triangular head at p2
        painter.drawLine(op.p1, op.p2);
        const double angle = std::atan2(op.p2.y() - op.p1.y(), op.p2.x() - op.p1.x());
        constexpr double kHeadLength = 14.0;
        constexpr double kHeadSpread = kPi / 7.0; // ~25.7 degrees off the shaft
        const QPointF tip(op.p2);
        const QPointF left = tip - QPointF(kHeadLength * std::cos(angle - kHeadSpread),
                                           kHeadLength * std::sin(angle - kHeadSpread));
        const QPointF right = tip - QPointF(kHeadLength * std::cos(angle + kHeadSpread),
                                            kHeadLength * std::sin(angle + kHeadSpread));
        painter.setBrush(op.color);
        painter.drawPolygon(QPolygonF({tip, left, right}));
    }
    painter.restore();
}

void MarkupCanvas::renderOps(QPainter &painter, const QVector<MarkupOp> &ops)
{
    for (const MarkupOp &op : ops)
        renderOp(painter, op);
}

QRect MarkupCanvas::textBounds(const MarkupOp &op)
{
    QFont font;
    font.setPointSize(op.textPointSize);
    font.setBold(true);
    const QFontMetrics metrics(font);
    const QStringList lines = textLines(op.text);
    int width = 1;
    for (const QString &line : lines)
        width = qMax(width, metrics.horizontalAdvance(line));
    const int height = qMax(1, int(lines.size()) * metrics.lineSpacing());
    return QRect(op.textPos, QSize(width, height));
}

QRect MarkupCanvas::layerBounds(const MarkupOp &op)
{
    if (op.kind == MarkupOp::Kind::Text)
        return textBounds(op);
    if (op.kind == MarkupOp::Kind::Image)
        return op.rect.normalized();
    return {};
}

QRect MarkupCanvas::resizeHandleRect(const MarkupOp &op)
{
    const QPoint center = layerBounds(op).bottomRight();
    return QRect(center - QPoint(kResizeHandleSize / 2, kResizeHandleSize / 2),
                 QSize(kResizeHandleSize, kResizeHandleSize));
}

bool MarkupCanvas::isMovableLayer(const MarkupOp &op)
{
    return op.kind == MarkupOp::Kind::Text || op.kind == MarkupOp::Kind::Image;
}

int MarkupCanvas::movableLayerAt(const QPoint &position, bool includeImages) const
{
    for (int i = m_ops.size() - 1; i >= 0; --i) {
        const MarkupOp &op = m_ops.at(i);
        if (!isMovableLayer(op) ||
            (!includeImages && op.kind == MarkupOp::Kind::Image))
            continue;
        if (layerBounds(op).adjusted(-7, -7, 7, 7).contains(position))
            return i;
    }
    return -1;
}
