package com.forkmesh.nativeapp.chat

import kotlin.math.hypot
import kotlin.math.max

enum class CallPreviewCorner(val wire: String) {
    TOP_LEADING("topLeading"),
    TOP_TRAILING("topTrailing"),
    BOTTOM_LEADING("bottomLeading"),
    BOTTOM_TRAILING("bottomTrailing"),
    ;

    companion object {
        const val KEY = "fm.call.previewCorner"
        val DEFAULT = TOP_TRAILING

        fun from(wire: String?): CallPreviewCorner = entries.firstOrNull { it.wire == wire } ?: DEFAULT
    }
}

data class CallPoint(val x: Float, val y: Float)

object CallPreviewLayout {
    const val TILE_WIDTH = 110f
    const val TILE_HEIGHT = 160f
    const val MARGIN = 14f
    const val TOP_CLEARANCE = 90f
    const val BOTTOM_CLEARANCE = 180f

    fun center(
        corner: CallPreviewCorner,
        width: Float,
        height: Float,
        bottomClearance: Float = BOTTOM_CLEARANCE,
        topClearance: Float = TOP_CLEARANCE,
    ): CallPoint {
        val left = MARGIN + TILE_WIDTH / 2
        val right = max(left, width - MARGIN - TILE_WIDTH / 2)
        val top = topClearance + TILE_HEIGHT / 2
        val bottom = max(top, height - bottomClearance - TILE_HEIGHT / 2)
        return when (corner) {
            CallPreviewCorner.TOP_LEADING -> CallPoint(left, top)
            CallPreviewCorner.TOP_TRAILING -> CallPoint(right, top)
            CallPreviewCorner.BOTTOM_LEADING -> CallPoint(left, bottom)
            CallPreviewCorner.BOTTOM_TRAILING -> CallPoint(right, bottom)
        }
    }

    fun nearest(
        point: CallPoint,
        width: Float,
        height: Float,
        bottomClearance: Float = BOTTOM_CLEARANCE,
        topClearance: Float = TOP_CLEARANCE,
    ): CallPreviewCorner =
        CallPreviewCorner.entries.minBy { corner ->
            val spot = center(corner, width, height, bottomClearance, topClearance)
            hypot(spot.x - point.x, spot.y - point.y)
        }

    fun settle(
        from: CallPreviewCorner,
        dragX: Float,
        dragY: Float,
        velocityX: Float,
        velocityY: Float,
        width: Float,
        height: Float,
        bottomClearance: Float = BOTTOM_CLEARANCE,
        topClearance: Float = TOP_CLEARANCE,
    ): CallPreviewCorner {
        val spot = center(from, width, height, bottomClearance, topClearance)
        val end = CallPoint(spot.x + dragX + velocityX * FLING_SECONDS, spot.y + dragY + velocityY * FLING_SECONDS)
        return nearest(end, width, height, bottomClearance, topClearance)
    }

    const val FLING_SECONDS = 0.15f
}

sealed class CallControlsArrangement {
    data class OneRow(val spacing: Float) : CallControlsArrangement()
    data object TwoRows : CallControlsArrangement()
}

object CallControlsLayout {
    const val WIDE_GAP = 16f
    const val NARROW_GAP = 8f
    const val LOOKS_ROW_GAP = 24f
    const val ESSENTIALS_ROW_GAP = 32f

    fun rowWidth(buttons: List<Float>, spacing: Float): Float =
        if (buttons.isEmpty()) 0f else buttons.sum() + spacing * (buttons.size - 1)

    fun arrange(available: Float, buttons: List<Float>): CallControlsArrangement = when {
        rowWidth(buttons, WIDE_GAP) <= available -> CallControlsArrangement.OneRow(WIDE_GAP)
        rowWidth(buttons, NARROW_GAP) <= available -> CallControlsArrangement.OneRow(NARROW_GAP)
        else -> CallControlsArrangement.TwoRows
    }
}

enum class CallMiniSide(val wire: String) {
    LEADING("leading"),
    TRAILING("trailing"),
    ;

    companion object {
        const val SIDE_KEY = "fm.call.miniSide"
        const val Y_KEY = "fm.call.miniY"

        fun from(wire: String?): CallMiniSide = entries.firstOrNull { it.wire == wire } ?: TRAILING
    }
}

data class CallMiniSpot(val side: CallMiniSide, val fraction: Double)

object CallMiniLayout {
    const val MARGIN = 12f
    const val VIDEO_WIDTH = 116f
    const val VIDEO_HEIGHT = 164f
    const val VOICE_WIDTH = 156f
    const val VOICE_HEIGHT = 96f
    const val SNAP_CORNER = 0.12
    const val DEFAULT_FRACTION = 0.18
    const val FLING_SECONDS = 0.15f

    private data class Span(val left: Float, val right: Float, val top: Float, val bottom: Float)

    private fun range(tileWidth: Float, tileHeight: Float, width: Float, height: Float): Span {
        val left = MARGIN + tileWidth / 2
        val right = max(left, width - MARGIN - tileWidth / 2)
        val top = MARGIN + tileHeight / 2
        val bottom = max(top, height - MARGIN - tileHeight / 2)
        return Span(left, right, top, bottom)
    }

    fun center(spot: CallMiniSpot, tileWidth: Float, tileHeight: Float, width: Float, height: Float): CallPoint {
        val box = range(tileWidth, tileHeight, width, height)
        val y = box.top + spot.fraction.coerceIn(0.0, 1.0).toFloat() * (box.bottom - box.top)
        return CallPoint(if (spot.side == CallMiniSide.LEADING) box.left else box.right, y)
    }

    fun snap(point: CallPoint, tileWidth: Float, tileHeight: Float, width: Float, height: Float): CallMiniSpot {
        val box = range(tileWidth, tileHeight, width, height)
        val side = if (point.x < (box.left + box.right) / 2) CallMiniSide.LEADING else CallMiniSide.TRAILING
        val span = box.bottom - box.top
        var fraction = if (span > 0) ((point.y - box.top) / span).toDouble() else 0.0
        fraction = fraction.coerceIn(0.0, 1.0)
        if (fraction < SNAP_CORNER) fraction = 0.0
        if (fraction > 1 - SNAP_CORNER) fraction = 1.0
        return CallMiniSpot(side, fraction)
    }

    fun settle(
        from: CallMiniSpot,
        dragX: Float,
        dragY: Float,
        velocityX: Float,
        velocityY: Float,
        tileWidth: Float,
        tileHeight: Float,
        width: Float,
        height: Float,
    ): CallMiniSpot {
        val spot = center(from, tileWidth, tileHeight, width, height)
        val end = CallPoint(spot.x + dragX + velocityX * FLING_SECONDS, spot.y + dragY + velocityY * FLING_SECONDS)
        return snap(end, tileWidth, tileHeight, width, height)
    }

    fun duration(seconds: Long): String {
        val total = max(0L, seconds)
        val hours = total / 3600
        val minutes = (total % 3600) / 60
        val rest = total % 60
        return if (hours > 0) "%d:%02d:%02d".format(hours, minutes, rest) else "%d:%02d".format(minutes, rest)
    }
}
