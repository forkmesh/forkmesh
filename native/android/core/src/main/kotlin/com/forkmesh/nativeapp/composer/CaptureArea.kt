package com.forkmesh.nativeapp.composer

import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

data class CaptureArea(val x: Double, val y: Double, val width: Double, val height: Double) {
    companion object {
        val EMPTY = CaptureArea(0.0, 0.0, 0.0, 0.0)
    }
}

data class CaptureHit(val width: Double, val height: Double, val actionable: Boolean)

object CaptureAreaRules {
    const val MINIMUM_SIDE = 16.0
    const val LARGEST_ELEMENT_SHARE = 0.5
    const val HOLD_MS = 600L
    const val DRAW_DELAY_MS = 350L
    const val LONG_SIDE_PX = 2048

    fun area(fromX: Double, fromY: Double, toX: Double, toY: Double, boundsWidth: Double, boundsHeight: Double): CaptureArea {
        if (boundsWidth <= 0 || boundsHeight <= 0) return CaptureArea.EMPTY
        val left = min(max(min(fromX, toX), 0.0), boundsWidth)
        val right = min(max(max(fromX, toX), 0.0), boundsWidth)
        val top = min(max(min(fromY, toY), 0.0), boundsHeight)
        val bottom = min(max(max(fromY, toY), 0.0), boundsHeight)
        return CaptureArea(left, top, right - left, bottom - top)
    }

    fun isUsable(area: CaptureArea): Boolean = area.width >= MINIMUM_SIDE && area.height >= MINIMUM_SIDE

    fun pixels(area: CaptureArea, scale: Double, pixelWidth: Int, pixelHeight: Int): CaptureArea {
        if (scale <= 0 || pixelWidth <= 0 || pixelHeight <= 0) return CaptureArea.EMPTY
        val left = min(max(floor(area.x * scale), 0.0), pixelWidth.toDouble())
        val top = min(max(floor(area.y * scale), 0.0), pixelHeight.toDouble())
        val right = min(max(ceil((area.x + area.width) * scale), left), pixelWidth.toDouble())
        val bottom = min(max(ceil((area.y + area.height) * scale), top), pixelHeight.toDouble())
        return CaptureArea(left, top, right - left, bottom - top)
    }

    fun isContent(elementWidth: Double, elementHeight: Double, boundsWidth: Double, boundsHeight: Double): Boolean {
        if (elementWidth <= 0 || elementHeight <= 0) return false
        return elementWidth * elementHeight <= boundsWidth * boundsHeight * LARGEST_ELEMENT_SHARE
    }

    fun isBlank(hits: List<CaptureHit>, boundsWidth: Double, boundsHeight: Double): Boolean =
        hits.none { it.actionable || isContent(it.width, it.height, boundsWidth, boundsHeight) }

    fun fitted(width: Int, height: Int, longSide: Int = LONG_SIDE_PX): Pair<Int, Int> {
        val long = max(width, height)
        if (long <= longSide || long <= 0) return width to height
        val scale = longSide.toDouble() / long
        return max(1, (width * scale).toInt()) to max(1, (height * scale).toInt())
    }
}
