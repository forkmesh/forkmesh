package com.forkmesh.nativeapp.composer

import kotlin.math.atan2
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

enum class MarkupTool { Pen, Line, Arrow, Rect, Ellipse }

data class MarkupPoint(val x: Float, val y: Float)

data class MarkupStroke(val tool: MarkupTool, val color: Long, val points: List<MarkupPoint>)

data class MarkupFit(val left: Float, val top: Float, val width: Float, val height: Float)

object Markup {
    val COLORS: List<Long> = listOf(
        0xFFE5484D,
        0xFFF76B15,
        0xFFFFC53D,
        0xFF30A46C,
        0xFF0090FF,
        0xFF8E4EC6,
        0xFF111111,
        0xFFFFFFFF,
    )
    val DEFAULT_COLOR: Long = COLORS.first()
    const val CAPTURE_NAME = "screenshot"
    private const val STROKE_FRACTION = 0.006f
    private const val MIN_STROKE = 3f
    private const val MIN_SHAPE = 0.004f

    fun fit(imageWidth: Int, imageHeight: Int, boxWidth: Float, boxHeight: Float): MarkupFit {
        if (imageWidth <= 0 || imageHeight <= 0 || boxWidth <= 0f || boxHeight <= 0f) return MarkupFit(0f, 0f, 0f, 0f)
        val scale = min(boxWidth / imageWidth, boxHeight / imageHeight)
        val width = imageWidth * scale
        val height = imageHeight * scale
        return MarkupFit((boxWidth - width) / 2f, (boxHeight - height) / 2f, width, height)
    }

    fun normalize(x: Float, y: Float, fit: MarkupFit): MarkupPoint {
        if (fit.width <= 0f || fit.height <= 0f) return MarkupPoint(0f, 0f)
        return MarkupPoint(
            ((x - fit.left) / fit.width).coerceIn(0f, 1f),
            ((y - fit.top) / fit.height).coerceIn(0f, 1f),
        )
    }

    fun strokeWidth(width: Float, height: Float): Float = max(MIN_STROKE, min(width, height) * STROKE_FRACTION)

    fun extend(stroke: MarkupStroke, point: MarkupPoint): MarkupStroke = when (stroke.tool) {
        MarkupTool.Pen -> if (stroke.points.lastOrNull() == point) stroke else stroke.copy(points = stroke.points + point)
        else -> stroke.copy(points = listOf(stroke.points.first(), point))
    }

    fun isVisible(stroke: MarkupStroke): Boolean {
        val first = stroke.points.firstOrNull() ?: return false
        if (stroke.tool == MarkupTool.Pen) return true
        val last = stroke.points.last()
        return hypot(last.x - first.x, last.y - first.y) >= MIN_SHAPE
    }

    fun arrowHead(fromX: Float, fromY: Float, toX: Float, toY: Float, stroke: Float): List<Pair<Float, Float>> {
        val angle = atan2(toY - fromY, toX - fromX)
        val length = min(stroke * 5f, hypot(toX - fromX, toY - fromY) * 0.5f)
        val spread = Math.toRadians(28.0).toFloat()
        return listOf(
            Pair(toX - length * cos(angle - spread), toY - length * sin(angle - spread)),
            Pair(toX - length * cos(angle + spread), toY - length * sin(angle + spread)),
        )
    }
}
