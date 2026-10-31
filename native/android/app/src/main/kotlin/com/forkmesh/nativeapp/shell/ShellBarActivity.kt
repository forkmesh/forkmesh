package com.forkmesh.nativeapp.shell

import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.requiredSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.produceState
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.unit.Dp
import com.forkmesh.nativeapp.feature.agents.rememberOpsTick
import com.forkmesh.nativeapp.feature.navigation.ShellMenuBadge
import com.forkmesh.nativeapp.feature.teammates.TeammatesFeature
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.kit.FmBusyRing
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.delay
import kotlin.math.PI
import kotlin.math.sin

internal class ShellHubActivity(val active: Set<Route.Kind>, val progress: Map<Route.Kind, ShellBarProgress>)

@Composable
internal fun rememberShellHubActivity(hubBadges: Map<Route.Kind, ShellMenuBadge>): ShellHubActivity {
    val store by produceState(TeammatesFeature.startedShortcuts) {
        while (value == null) {
            delay(2000)
            value = TeammatesFeature.startedShortcuts
        }
    }
    val shortcuts = store
    val tick = if (shortcuts != null) rememberOpsTick(shortcuts) else 0
    val timers = (hubBadges[Route.Kind.Timers]?.count ?: 0) > 0
    return remember(shortcuts, tick, timers) {
        val active = mutableSetOf<Route.Kind>()
        val progress = mutableMapOf<Route.Kind, ShellBarProgress>()
        if (timers) active += Route.Kind.Timers
        if (shortcuts != null) {
            val runs = shortcuts.runs.values.filter { it.running }.map { ShellBarProgress(it.startedAtMs, it.expectedMs) } +
                shortcuts.catalog?.entries.orEmpty().filter { it.running }.map { ShellBarProgress(it.startedAtMs, it.expectedMs) }
            if (runs.isNotEmpty()) active += Route.Kind.Shortcuts
            ShellBarProgress.latest(runs)?.let { progress[Route.Kind.Shortcuts] = it }
        }
        ShellHubActivity(active, progress)
    }
}

internal object ShellActivityHalo {
    const val TURN_MS = FmBusyRing.TURN_MS
    const val BREATH_MS = 2400
    const val ARC = FmBusyRing.ARC
    const val SWEEP = 0.28f
}

@Composable
internal fun ShellActivityHaloView(size: Dp) {
    val c = Fm.colors
    val motion = rememberInfiniteTransition(label = "shellHalo")
    val spin by motion.animateFloat(0f, 360f, infiniteRepeatable(tween(ShellActivityHalo.TURN_MS, easing = LinearEasing)), label = "spin")
    val phase by motion.animateFloat(0f, 1f, infiniteRepeatable(tween(ShellActivityHalo.BREATH_MS, easing = LinearEasing), RepeatMode.Restart), label = "breath")
    val breath = 0.5f + 0.5f * sin(phase * 2f * PI.toFloat())
    Canvas(Modifier.requiredSize(size)) {
        val r = this.size.minDimension / 2
        val glow = r * (0.85f + 0.2f * breath)
        drawCircle(Brush.radialGradient(listOf(c.accent.copy(alpha = 0.32f * (0.6f + 0.4f * breath)), c.accent.copy(alpha = 0f)), center, glow), glow)
        val stroke = 1.75f * density
        drawCircle(c.accent.copy(alpha = 0.14f), r - stroke / 2, style = Stroke(1.5f * density))
        rotate(spin) {
            drawArc(
                Brush.sweepGradient(0f to c.accent.copy(alpha = 0f), ShellActivityHalo.ARC to c.accent, center = center),
                0f, 360f * ShellActivityHalo.ARC, false,
                Offset(stroke / 2, stroke / 2), Size(this.size.width - stroke, this.size.height - stroke),
                style = Stroke(stroke, cap = StrokeCap.Round),
            )
        }
    }
}

@Composable
internal fun ShellBarProgressRing(progress: ShellBarProgress, size: Dp, lineWidth: Float) {
    val c = Fm.colors
    var now by remember { mutableLongStateOf(System.currentTimeMillis()) }
    LaunchedEffect(progress) {
        while (true) {
            delay(500)
            now = System.currentTimeMillis()
        }
    }
    val fraction = progress.fraction(now)
    val motion = rememberInfiniteTransition(label = "shellRing")
    val spin by motion.animateFloat(0f, 360f, infiniteRepeatable(tween(ShellActivityHalo.TURN_MS, easing = LinearEasing)), label = "spin")
    Canvas(Modifier.requiredSize(size)) {
        val stroke = lineWidth * density
        val inset = Offset(stroke / 2, stroke / 2)
        val box = Size(this.size.width - stroke, this.size.height - stroke)
        drawCircle(c.accent.copy(alpha = 0.18f), this.size.minDimension / 2 - stroke / 2, style = Stroke(stroke))
        rotate(if (fraction == null) spin else 0f) {
            drawArc(c.accent, -90f, 360f * (fraction?.toFloat() ?: ShellActivityHalo.SWEEP), false, inset, box, style = Stroke(stroke, cap = StrokeCap.Round))
        }
    }
}
