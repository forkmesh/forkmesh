package com.forkmesh.nativeapp.aquarium

import kotlin.math.max
import kotlin.math.min

data class AquariumPoint(val x: Double, val y: Double) {
    companion object {
        val ZERO = AquariumPoint(0.0, 0.0)
    }
}

data class AquariumSize(val width: Double, val height: Double) {
    companion object {
        val ZERO = AquariumSize(0.0, 0.0)
    }
}

data class AquariumRect(val x: Double, val y: Double, val width: Double, val height: Double) {
    val minX: Double get() = x
    val maxX: Double get() = x + width
    val minY: Double get() = y
    val maxY: Double get() = y + height
    val midX: Double get() = x + width / 2
    val midY: Double get() = y + height / 2

    fun insetBy(dx: Double, dy: Double): AquariumRect = AquariumRect(x + dx, y + dy, width - dx * 2, height - dy * 2)

    fun offsetBy(dx: Double, dy: Double): AquariumRect = AquariumRect(x + dx, y + dy, width, height)

    fun contains(point: AquariumPoint): Boolean = point.x >= minX && point.x < maxX && point.y >= minY && point.y < maxY

    fun intersection(other: AquariumRect): AquariumRect? {
        val left = max(minX, other.minX)
        val right = min(maxX, other.maxX)
        val top = max(minY, other.minY)
        val bottom = min(maxY, other.maxY)
        if (right < left || bottom < top) return null
        return AquariumRect(left, top, right - left, bottom - top)
    }

    companion object {
        val ZERO = AquariumRect(0.0, 0.0, 0.0, 0.0)
    }
}
