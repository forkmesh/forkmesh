package com.forkmesh.nativeapp.agents

import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

class AgentTokenTank(
    val width: Double,
    val height: Double,
    seed: Long = 1L,
    private val leakSeconds: Double = LEAK_SECONDS,
) {
    companion object {
        const val LEAK_SECONDS = 12.0
        const val POUR_SECONDS = 1.0
        const val STEPS_PER_SECOND = 60.0
        const val MAX_ADVANCE_SECONDS = 0.1
        const val GRAVITY = 180.0
        const val FLAT_BIAS = 0.25
        const val MAX_PULL = 3.0
        const val RAIN_SPEED = 40.0
        const val RADIUS = 2.0
        const val REST_DENSITY = 1.2
        const val STIFFNESS = 0.3
        const val NEAR_STIFFNESS = 0.5
        const val DAMPING = 0.6
        const val SHADES = 4
        const val NEIGHBORS = 48

        fun scale(values: List<Double>, floor: Double): Double = max(floor, values.maxOrNull() ?: 0.0)
    }

    val capacity: Int = max(0, floor(width).toInt() * floor(height).toInt())
    val xs = DoubleArray(capacity)
    val ys = DoubleArray(capacity)
    val shades = IntArray(capacity)
    var grains = 0
        private set
    var gravityX = 0.0
        private set
    var gravityY = GRAVITY
        private set

    private val vxs = DoubleArray(capacity)
    private val vys = DoubleArray(capacity)
    private val lastXs = DoubleArray(capacity)
    private val lastYs = DoubleArray(capacity)
    private val gridColumns = max(1, ceil(width / RADIUS).toInt())
    private val gridRows = max(1, ceil(height / RADIUS).toInt())
    private val cellStart = IntArray(gridColumns * gridRows + 1)
    private val cellOf = IntArray(capacity)
    private val sorted = IntArray(capacity)
    private val neighbors = IntArray(NEIGHBORS)
    private val nearXs = DoubleArray(NEIGHBORS)
    private val nearYs = DoubleArray(NEIGHBORS)
    private val nearQs = DoubleArray(NEIGHBORS)
    private var random = if (seed == 0L) 0x9E3779B97F4A7C15uL.toLong() else seed
    private var owed = 0.0
    private var pourRate = 0.0
    private var rainCarry = 0.0
    private var leakCarry = 0.0
    private var stepCarry = 0.0

    val idle: Boolean get() = grains == 0 && owed < 1.0 && rainCarry < 1.0

    fun pour(tokens: Double, scale: Double) {
        if (capacity == 0 || tokens <= 0 || scale <= 0) return
        drop(min(1.0, tokens / scale) * capacity / leakSeconds)
    }

    fun drop(count: Double) {
        if (count <= 0) return
        owed += count
        pourRate = owed / POUR_SECONDS
    }

    fun tilt(x: Double, y: Double) {
        val scale = GRAVITY / (1 + FLAT_BIAS)
        var gx = x * scale
        var gy = (y + FLAT_BIAS) * scale
        val length = sqrt(gx * gx + gy * gy)
        val most = GRAVITY * MAX_PULL
        if (length > most) {
            gx *= most / length
            gy *= most / length
        }
        gravityX = gx
        gravityY = gy
    }

    fun advance(seconds: Double) {
        if (capacity == 0) return
        stepCarry += min(max(seconds, 0.0), MAX_ADVANCE_SECONDS) * STEPS_PER_SECOND
        while (stepCarry >= 1) {
            step(1 / STEPS_PER_SECOND)
            stepCarry -= 1
        }
    }

    private fun step(dt: Double) {
        rain(dt)
        leak(dt)
        if (grains == 0) return
        val damping = 1 - DAMPING * dt
        for (i in 0 until grains) {
            vxs[i] = (vxs[i] + gravityX * dt) * damping
            vys[i] = (vys[i] + gravityY * dt) * damping
            lastXs[i] = xs[i]
            lastYs[i] = ys[i]
            xs[i] += vxs[i] * dt
            ys[i] += vys[i] * dt
        }
        relax()
        for (i in 0 until grains) {
            contain(i)
            vxs[i] = (xs[i] - lastXs[i]) / dt
            vys[i] = (ys[i] - lastYs[i]) / dt
        }
    }

    private fun rain(dt: Double) {
        if (owed <= 0) return
        val due = min(owed, pourRate * dt)
        owed -= due
        rainCarry += due
        while (rainCarry >= 1) {
            rainCarry -= 1
            spawn()
        }
    }

    private fun leak(dt: Double) {
        if (grains == 0) {
            leakCarry = 0.0
            return
        }
        leakCarry += grains * dt / leakSeconds
        while (leakCarry >= 1 && grains > 0) {
            leakCarry -= 1
            remove(lowest())
        }
    }

    private fun spawn() {
        if (grains >= capacity) return
        val i = grains
        val along = next()
        val length = sqrt(gravityX * gravityX + gravityY * gravityY)
        val ux = if (length > 0) gravityX / length else 0.0
        val uy = if (length > 0) gravityY / length else 1.0
        if (abs(uy) >= abs(ux)) {
            xs[i] = 0.5 + along * (width - 1)
            ys[i] = if (uy >= 0) 0.5 else height - 0.5
        } else {
            xs[i] = if (ux >= 0) 0.5 else width - 0.5
            ys[i] = 0.5 + along * (height - 1)
        }
        vxs[i] = ux * RAIN_SPEED
        vys[i] = uy * RAIN_SPEED
        shades[i] = 1 + (next() * SHADES).toInt().coerceAtMost(SHADES - 1)
        grains += 1
    }

    private fun lowest(): Int {
        var best = 0
        var bestDepth = Double.NEGATIVE_INFINITY
        for (i in 0 until grains) {
            val depth = xs[i] * gravityX + ys[i] * gravityY
            if (depth > bestDepth) {
                bestDepth = depth
                best = i
            }
        }
        return best
    }

    private fun remove(i: Int) {
        val last = grains - 1
        xs[i] = xs[last]
        ys[i] = ys[last]
        vxs[i] = vxs[last]
        vys[i] = vys[last]
        shades[i] = shades[last]
        grains = last
    }

    private fun contain(i: Int) {
        xs[i] = min(max(xs[i], 0.5), width - 0.5)
        ys[i] = min(max(ys[i], 0.5), height - 0.5)
    }

    private fun cell(x: Double, y: Double): Int {
        val column = min(max((x / RADIUS).toInt(), 0), gridColumns - 1)
        val row = min(max((y / RADIUS).toInt(), 0), gridRows - 1)
        return row * gridColumns + column
    }

    private fun relax() {
        cellStart.fill(0)
        for (i in 0 until grains) {
            val c = cell(xs[i], ys[i])
            cellOf[i] = c
            cellStart[c + 1] += 1
        }
        for (c in 0 until gridColumns * gridRows) cellStart[c + 1] += cellStart[c]
        val fill = cellStart.copyOf()
        for (i in 0 until grains) {
            sorted[fill[cellOf[i]]] = i
            fill[cellOf[i]] += 1
        }
        for (i in 0 until grains) {
            val column = cellOf[i] % gridColumns
            val row = cellOf[i] / gridColumns
            var density = 0.0
            var near = 0.0
            var found = 0
            for (r in max(0, row - 1)..min(gridRows - 1, row + 1)) {
                for (c in max(0, column - 1)..min(gridColumns - 1, column + 1)) {
                    val cellIndex = r * gridColumns + c
                    for (k in cellStart[cellIndex] until cellStart[cellIndex + 1]) {
                        val j = sorted[k]
                        if (j == i || found == NEIGHBORS) continue
                        var dx = xs[j] - xs[i]
                        var dy = ys[j] - ys[i]
                        val square = dx * dx + dy * dy
                        if (square >= RADIUS * RADIUS) continue
                        var distance = sqrt(square)
                        if (distance < 1e-6) {
                            val angle = next() * 2 * Math.PI
                            dx = Math.cos(angle) * 1e-3
                            dy = Math.sin(angle) * 1e-3
                            distance = 1e-3
                        }
                        val q = 1 - distance / RADIUS
                        density += q * q
                        near += q * q * q
                        neighbors[found] = j
                        nearXs[found] = dx / distance
                        nearYs[found] = dy / distance
                        nearQs[found] = q
                        found += 1
                    }
                }
            }
            val pressure = STIFFNESS * (density - REST_DENSITY)
            val nearPressure = NEAR_STIFFNESS * near
            var moveX = 0.0
            var moveY = 0.0
            for (n in 0 until found) {
                val q = nearQs[n]
                val push = (pressure * q + nearPressure * q * q) / 2
                val px = nearXs[n] * push
                val py = nearYs[n] * push
                xs[neighbors[n]] += px
                ys[neighbors[n]] += py
                moveX -= px
                moveY -= py
            }
            xs[i] += moveX
            ys[i] += moveY
        }
    }

    private fun next(): Double {
        var x = random
        x = x xor (x ushr 12)
        x = x xor (x shl 25)
        x = x xor (x ushr 27)
        random = x
        return ((x * 2685821657736338717L) ushr 11).toDouble() / 9007199254740992.0
    }
}
