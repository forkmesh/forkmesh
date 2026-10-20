package com.forkmesh.nativeapp.feature.health

import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.StrokeJoin
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.unit.Dp
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.LocalFmReducedMotion
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

class StepsWalkClock {
    var phase = 0.0
    var scroll = 0.0
    var speed = 0.0
    var run = 0.0
    private var last: Double? = null

    fun advance(seconds: Double, cadence: Double, running: Boolean, legLength: Double) {
        val previous = last
        val dt = if (previous != null) min(0.1, max(0.0, seconds - previous)) else 0.0
        last = seconds
        speed += (cadence - speed) * min(1.0, dt * 3)
        if (speed < 0.08 && cadence == 0.0) speed = 0.0
        val target = if (running) 1.0 else StepsWalkPose.run(speed)
        run += (target - run) * min(1.0, dt * 3)
        if (speed == 0.0) run = 0.0
        phase = (phase + dt * speed * Math.PI) % (2 * Math.PI)
        scroll += dt * 2 * legLength * StepsWalkPose.swing(speed, run) * speed
    }
}

class StepsWalkPose(x: Double, groundY: Double, phase: Double, speed: Double, run: Double) {
    val hip: Offset
    val shoulder: Offset
    val head: Offset
    val legs: List<Pair<Offset, Offset>>
    val arms: List<Pair<Offset, Offset>>

    init {
        val stride = stride(speed)
        val swing = swing(speed, run)
        val knee = stride * (0.55 + 0.75 * run)
        val armSwing = stride * (0.35 + 0.45 * run)
        val elbow = 0.25 + stride * (0.2 + 1.0 * run)
        val lean = 0.04 * stride + 0.16 * run
        val legLength = THIGH + SHIN
        val flight = run * 3 * abs(cos(phase))
        val hipY = groundY - legLength * cos(swing * sin(phase)) - flight
        hip = Offset(x.toFloat(), hipY.toFloat())
        shoulder = Offset((x + TORSO * sin(lean)).toFloat(), (hipY - TORSO * cos(lean)).toFloat())
        head = Offset(
            (shoulder.x + (HEAD_RADIUS + 3) * sin(lean)).toFloat(),
            (shoulder.y - (HEAD_RADIUS + 3) * cos(lean)).toFloat(),
        )
        legs = listOf(1.0, -1.0).map { side ->
            val angle = side * swing * sin(phase)
            val bend = knee * max(0.0, side * cos(phase))
            val kneePoint = along(hip, angle, THIGH)
            kneePoint to along(kneePoint, angle - bend, SHIN)
        }
        arms = listOf(1.0, -1.0).map { side ->
            val angle = -side * armSwing * sin(phase)
            val elbowPoint = along(shoulder, angle, UPPER_ARM)
            elbowPoint to along(elbowPoint, angle + elbow, FOREARM)
        }
    }

    companion object {
        const val THIGH = 19.0
        const val SHIN = 19.0
        const val TORSO = 25.0
        const val UPPER_ARM = 13.0
        const val FOREARM = 12.0
        const val HEAD_RADIUS = 7.5

        fun stride(speed: Double): Double = min(1.0, speed / 1.4)
        fun run(speed: Double): Double = min(1.0, max(0.0, (speed - 2.1) / 0.5))
        fun swing(speed: Double, run: Double): Double = stride(speed) * (0.42 + 0.18 * run)

        private fun along(from: Offset, angle: Double, length: Double): Offset =
            Offset((from.x + length * sin(angle)).toFloat(), (from.y + length * cos(angle)).toFloat())
    }
}

private fun DrawScope.drawWalkScene(
    width: Float,
    height: Float,
    pxPerPt: Float,
    clock: StepsWalkClock,
    figure: Color,
    scenery: Color,
    ground: Color,
    figureX: Float,
) {
    scale(pxPerPt, pxPerPt, pivot = Offset.Zero) {
        val groundY = (height - 12).toDouble()
        val scroll = clock.scroll
        val hills = Path()
        hills.moveTo(0f, groundY.toFloat())
        for (step in 0..(width / 4).toInt()) {
            val x = step * 4.0
            val world = x + scroll * 0.15
            val y = groundY - 16 - 9 * sin(world / 53) - 6 * sin(world / 23 + 1.3)
            hills.lineTo(x.toFloat(), y.toFloat())
        }
        hills.lineTo(width, groundY.toFloat())
        hills.close()
        drawPath(hills, ground.copy(alpha = ground.alpha * 0.45f))
        val spacing = 74.0
        val treeShift = scroll * 0.55
        val firstTree = floor((treeShift - 20) / spacing).toInt()
        for (index in firstTree..(firstTree + (width / spacing).toInt() + 2)) {
            val variant = ((index % 7) + 7) % 7
            if (variant == 3) continue
            val x = index * spacing - treeShift + ((variant * 17) % 29)
            val treeHeight = 20.0 + ((variant * 11) % 13)
            drawLine(
                scenery, Offset(x.toFloat(), groundY.toFloat()), Offset(x.toFloat(), (groundY - treeHeight).toFloat()),
                strokeWidth = 2.5f, cap = StrokeCap.Round,
            )
            val crown = 7.0 + (variant % 3) * 2
            drawOval(
                scenery,
                topLeft = Offset((x - crown).toFloat(), (groundY - treeHeight - crown * 1.4).toFloat()),
                size = Size((crown * 2).toFloat(), (crown * 2.2).toFloat()),
            )
        }
        drawLine(ground, Offset(0f, groundY.toFloat()), Offset(width, groundY.toFloat()), strokeWidth = 2f)
        val dash = 22.0
        var x = -(scroll % dash)
        val dashes = Path()
        while (x < width) {
            dashes.moveTo(x.toFloat(), (groundY + 6).toFloat())
            dashes.lineTo((x + 8).toFloat(), (groundY + 6).toFloat())
            x += dash
        }
        drawPath(dashes, ground, style = Stroke(width = 2f, cap = StrokeCap.Round))
        val pose = StepsWalkPose(width * figureX.toDouble(), groundY - 1, clock.phase, clock.speed, clock.run)
        val limb = Stroke(width = 5f, cap = StrokeCap.Round, join = StrokeJoin.Round)
        fun stroke(points: List<Offset>, color: Color) {
            val path = Path()
            path.moveTo(points[0].x, points[0].y)
            for (point in points.drop(1)) path.lineTo(point.x, point.y)
            drawPath(path, color, style = limb)
        }
        val far = figure.copy(alpha = figure.alpha * 0.5f)
        stroke(listOf(pose.hip, pose.legs[1].first, pose.legs[1].second), far)
        stroke(listOf(pose.shoulder, pose.arms[1].first, pose.arms[1].second), far)
        stroke(listOf(pose.hip, pose.legs[0].first, pose.legs[0].second), figure)
        stroke(listOf(pose.hip, pose.shoulder), figure)
        val radius = StepsWalkPose.HEAD_RADIUS.toFloat()
        drawCircle(figure, radius, pose.head)
        stroke(listOf(pose.shoulder, pose.arms[0].first, pose.arms[0].second), figure)
    }
}

@Composable
fun StepsWalkScene(
    cadence: Double,
    running: Boolean,
    figure: Color,
    scenery: Color,
    ground: Color,
    modifier: Modifier = Modifier,
    figureX: Float = 0.38f,
    canvasPoints: Float? = null,
) {
    val reduce = LocalFmReducedMotion.current
    val clock = remember { StepsWalkClock() }
    var frame by remember { mutableLongStateOf(0L) }
    val latestCadence by rememberUpdatedState(cadence)
    val latestRunning by rememberUpdatedState(running)
    val idle = cadence == 0.0
    LaunchedEffect(idle, reduce) {
        var settledAt = -1L
        while (true) {
            val nanos = withFrameNanos { it }
            clock.advance(nanos / 1e9, if (reduce) 0.0 else latestCadence, latestRunning, StepsWalkPose.THIGH + StepsWalkPose.SHIN)
            frame = nanos
            if (reduce) break
            if (idle) {
                if (settledAt < 0) settledAt = nanos
                if (nanos - settledAt > 1_500_000_000L) break
            }
        }
    }
    val density = LocalDensity.current.density
    Canvas(modifier) {
        frame
        val widthPt = canvasPoints ?: (size.width / density)
        val heightPt = canvasPoints ?: (size.height / density)
        val pxPerPt = if (canvasPoints != null) size.minDimension / canvasPoints else density
        drawWalkScene(widthPt, heightPt, pxPerPt, clock, figure, scenery, ground, figureX)
    }
}

@Composable
fun StepsWalkBadge(cadence: Double, running: Boolean, size: Dp, modifier: Modifier = Modifier, figure: Color? = null) {
    val c = Fm.colors
    Box(modifier.size(size).clip(CircleShape).background(c.accent.copy(alpha = 0.12f))) {
        StepsWalkScene(
            cadence, running, figure ?: c.accent, c.textTertiary, c.border,
            Modifier.fillMaxSize(), figureX = 0.5f, canvasPoints = 104f,
        )
    }
}
