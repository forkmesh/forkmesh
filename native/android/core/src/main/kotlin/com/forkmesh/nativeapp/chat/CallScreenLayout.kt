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
