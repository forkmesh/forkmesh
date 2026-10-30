package com.forkmesh.nativeapp.chat

import kotlin.math.PI
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin
import kotlin.math.sqrt

data class DirectLivePlanePoint(val x: Double, val y: Double)

data class DirectLiveFaceMap(val a: Double, val b: Double, val c: Double, val d: Double, val tx: Double, val ty: Double)

data class DirectLiveKick(val x: Double, val y: Double, val z: Double)

class DirectLivePhonePose(roll: Double, pitch: Double, push: DirectLiveKick = DirectLiveKick(0.0, 0.0, 0.0)) {
    val roll: Double = if (roll.isFinite()) roll else 0.0
    val pitch: Double = if (pitch.isFinite()) min(max(pitch, -PI / 2), PI / 2) else 0.0
    val kickX: Double
    val kickY: Double
    val kickZ: Double

    init {
        val reach = sqrt(push.x * push.x + push.y * push.y + push.z * push.z)
        val scale = if (reach > MAX_KICK) MAX_KICK / reach else 1.0
        val turned = turn(push.x * scale, push.y * scale, push.z * scale, this.roll, this.pitch)
        kickX = turned[0]
        kickY = turned[1]
        kickZ = turned[2]
    }

    constructor(sample: DirectLiveMotionSample, push: DirectLiveKick? = null) :
        this(sample.steadyRoll, sample.pitch, push ?: DirectLiveKick(sample.ax, sample.ay, sample.az))

    val jolt: Double get() = sqrt(kickX * kickX + kickY * kickY + kickZ * kickZ)

    val offset: DirectLivePlanePoint get() = DirectLivePlanePoint(kickX * KICK_REACH, kickY * KICK_REACH)

    val lift: Double get() = 1 + kickZ * 0.06

    val faceShowing: Boolean get() = cos(pitch) > 0.015

    fun project(u: Double, v: Double, w: Double): DirectLivePlanePoint {
        val turned = turn(u, v, w, roll, pitch)
        return DirectLivePlanePoint(turned[0], turned[1])
    }

    val faceMap: DirectLiveFaceMap
        get() {
            val s = sin(roll)
            val c = cos(roll)
            val tilt = cos(pitch)
            return DirectLiveFaceMap(a = c, b = s * tilt, c = -s, d = c * tilt, tx = 0.0, ty = -DEPTH / 2 * sin(pitch))
        }

    fun outline(steps: Int = 5): List<DirectLivePlanePoint> {
        val halfWidth = WIDTH / 2 - CORNER
        val halfHeight = HEIGHT / 2 - CORNER
        val half = DEPTH / 2
        val centres = listOf(
            Triple(halfWidth, halfHeight, 0.0),
            Triple(-halfWidth, halfHeight, PI / 2),
            Triple(-halfWidth, -halfHeight, PI),
            Triple(halfWidth, -halfHeight, PI * 1.5),
        )
        val points = ArrayList<DirectLivePlanePoint>()
        val count = max(1, steps)
        for ((cx, cy, start) in centres) {
            for (step in 0..count) {
                val angle = start + step.toDouble() / count * PI / 2
                val u = cx + CORNER * cos(angle)
                val v = cy + CORNER * sin(angle)
                points.add(project(u, v, half))
                points.add(project(u, v, -half))
            }
        }
        return hull(points)
    }

    companion object {
        const val WIDTH = 20.0
        const val HEIGHT = 32.0
        const val DEPTH = 1.8
        const val CORNER = 5.0
        const val BEZEL = 1.3
        const val KICK_HOLD_MS = 260L
        const val KICK_REACH = 3.2
        const val MAX_KICK = 2.0
        val still = DirectLivePhonePose(0.0, 0.0)

        fun live(track: DirectLiveMotionTrack, now: Long): DirectLivePhonePose? {
            val current = track.sample(now) ?: return null
            var push = DirectLiveKick(current.ax, current.ay, current.az)
            var strongest = current.jolt
            for (sample in track.samples) {
                if (sample.at > now || now - sample.at >= KICK_HOLD_MS) continue
                val fade = 1 - (now - sample.at).toDouble() / KICK_HOLD_MS.toDouble()
                val felt = sample.jolt * fade
                if (felt > strongest) {
                    strongest = felt
                    push = DirectLiveKick(sample.ax * fade, sample.ay * fade, sample.az * fade)
                }
            }
            return DirectLivePhonePose(current, push)
        }

        internal fun turn(u: Double, v: Double, w: Double, roll: Double, pitch: Double): DoubleArray {
            val s = sin(roll)
            val c = cos(roll)
            val across = u * c + v * s
            val up = -u * s + v * c
            val high = up * cos(pitch) + w * sin(pitch)
            val near = -up * sin(pitch) + w * cos(pitch)
            return doubleArrayOf(across, -high, near)
        }

        internal fun hull(points: List<DirectLivePlanePoint>): List<DirectLivePlanePoint> {
            val sorted = points.sortedWith(compareBy({ it.x }, { it.y }))
            if (sorted.size <= 2) return sorted
            fun cross(o: DirectLivePlanePoint, a: DirectLivePlanePoint, b: DirectLivePlanePoint) = (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x)
            val lower = ArrayList<DirectLivePlanePoint>()
            for (point in sorted) {
                while (lower.size >= 2 && cross(lower[lower.size - 2], lower[lower.size - 1], point) <= 0) lower.removeAt(lower.size - 1)
                lower.add(point)
            }
            val upper = ArrayList<DirectLivePlanePoint>()
            for (point in sorted.asReversed()) {
                while (upper.size >= 2 && cross(upper[upper.size - 2], upper[upper.size - 1], point) <= 0) upper.removeAt(upper.size - 1)
                upper.add(point)
            }
            return lower.dropLast(1) + upper.dropLast(1)
        }
    }
}
