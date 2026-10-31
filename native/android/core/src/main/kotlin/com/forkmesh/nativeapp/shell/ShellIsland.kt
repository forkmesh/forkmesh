package com.forkmesh.nativeapp.shell

import kotlin.math.max

data class IslandRect(val left: Float, val top: Float, val right: Float, val bottom: Float) {
    val width: Float get() = right - left
    val center: Float get() = (left + right) / 2
}

data class IslandBand(val start: Float, val top: Float, val width: Float, val height: Float)

data class ShellIslandLayout(
    val dots: IslandBand,
    val lights: IslandBand,
    val charts: IslandBand?,
    val inStatusBar: Boolean,
)

object ShellIsland {
    const val ISLAND_WIDTH = 126f
    const val CLOCK_LINE = 16f
    const val EAR_HEIGHT = 12f
    const val EAR_MARGIN = 10f
    const val EAR_LEADING = 24f
    const val DOT = 5f
    const val STRIP = 7f
    const val MIN_EAR = 40f
    const val STACK_OVERLAP = -4f

    fun island(width: Float, statusTop: Float, cutout: IslandRect?): IslandRect? {
        val rect = cutout ?: return null
        if (rect.width <= 0 || rect.top >= statusTop) return null
        if (rect.center < width * 0.25f || rect.center > width * 0.75f) return null
        return rect
    }

    fun layout(width: Float, statusTop: Float, cutout: IslandRect?): ShellIslandLayout {
        val island = island(width, statusTop, cutout)
        val left = island?.left ?: ((width - ISLAND_WIDTH) / 2)
        val right = island?.right ?: ((width + ISLAND_WIDTH) / 2)
        val room = (statusTop - CLOCK_LINE) / 2
        val lightsWidth = max(0f, left - EAR_LEADING)
        val chartsWidth = max(0f, width - right - EAR_MARGIN * 2)
        val fits = room >= EAR_HEIGHT + 1 && lightsWidth >= MIN_EAR && chartsWidth >= MIN_EAR
        if (!fits) {
            val row = IslandBand(0f, statusTop + 1, width, EAR_HEIGHT)
            return ShellIslandLayout(row, row, null, inStatusBar = false)
        }
        val top = max(1f, (room - EAR_HEIGHT) / 2)
        val dots = when {
            island == null -> IslandBand(0f, 0f, width, max(DOT + 2, top))
            island.top >= DOT + 2 -> IslandBand(0f, 0f, width, island.top)
            else -> IslandBand(0f, statusTop + 1, width, STRIP)
        }
        return ShellIslandLayout(
            dots = dots,
            lights = IslandBand(EAR_LEADING, top, lightsWidth, EAR_HEIGHT),
            charts = IslandBand(right + EAR_MARGIN, top, chartsWidth, EAR_HEIGHT),
            inStatusBar = true,
        )
    }

    fun fittedChartWidth(width: Float, count: Int, spacing: Float, widest: Float): Float {
        if (count <= 0) return widest
        val room = width - spacing * (count - 1)
        return max(2f, minOf(widest, room / count))
    }
}
