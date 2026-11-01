package com.forkmesh.nativeapp.ui.kit

import android.os.SystemClock
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.Stable
import androidx.compose.runtime.State
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.drawWithContent
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.geometry.center
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Shader
import androidx.compose.ui.graphics.ShaderBrush
import androidx.compose.ui.graphics.Shape
import androidx.compose.ui.graphics.SweepGradientShader
import androidx.compose.ui.graphics.drawOutline
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.inset
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.delay

object FmBusyRing {
    const val TURN_MS = 1600
    const val ARC = 0.7f
    const val TRACK_ALPHA = 0.14f
    const val TAP_WINDOW_MS = 1_000L
    const val CAP_MS = 60_000L
    val stroke = 1.75.dp
}

@Stable
class FmBusyTap internal constructor() {
    internal var tappedAt = 0L
    internal var working by mutableStateOf(false)

    fun tapped() {
        tappedAt = SystemClock.uptimeMillis()
    }

    fun busy(forced: Boolean? = null): Boolean = forced ?: working
}

@Composable
fun rememberFmBusyTap(enabled: Boolean): FmBusyTap {
    val tap = remember { FmBusyTap() }
    LaunchedEffect(enabled) {
        if (enabled) {
            tap.working = false
            return@LaunchedEffect
        }
        if (SystemClock.uptimeMillis() - tap.tappedAt > FmBusyRing.TAP_WINDOW_MS) return@LaunchedEffect
        tap.working = true
        delay(FmBusyRing.CAP_MS)
        tap.working = false
    }
    return tap
}

@Composable
private fun rememberFmBusyTurn(): State<Float> {
    if (Fm.reducedMotion) return remember { mutableFloatStateOf(0f) }
    return rememberInfiniteTransition(label = "fmBusy").animateFloat(
        0f,
        360f,
        infiniteRepeatable(tween(FmBusyRing.TURN_MS, easing = LinearEasing)),
        label = "spin",
    )
}

@Composable
fun Modifier.fmBusyRing(busy: Boolean, shape: Shape = CircleShape, inset: Dp = 0.dp, color: Color = Fm.colors.accent): Modifier {
    if (!busy) return this
    val turn = rememberFmBusyTurn()
    return drawWithContent {
        drawContent()
        drawFmBusyRing(shape, turn.value, color, inset.toPx())
    }
}

@Composable
fun fmBusyBorder(busy: Boolean, color: Color = Fm.colors.accent): BorderStroke? {
    if (!busy) return null
    val turn by rememberFmBusyTurn()
    return BorderStroke(FmBusyRing.stroke, FmBusySweep(color, turn))
}

fun DrawScope.drawFmBusyRing(shape: Shape, degrees: Float, color: Color, gap: Float = 0f) {
    val stroke = FmBusyRing.stroke.toPx()
    val pad = gap + stroke / 2
    if (size.width - pad * 2 <= 0f || size.height - pad * 2 <= 0f) return
    inset(pad) {
        drawOutline(shape.createOutline(size, layoutDirection, this), FmBusySweep(color, degrees), style = Stroke(stroke))
    }
}

private class FmBusySweep(private val color: Color, private val degrees: Float) : ShaderBrush() {
    override fun createShader(size: Size): Shader {
        val center = size.center
        val track = color.copy(alpha = FmBusyRing.TRACK_ALPHA)
        val shader = SweepGradientShader(
            center,
            listOf(track, color, track, track),
            listOf(0f, FmBusyRing.ARC, FmBusyRing.ARC + 0.002f, 1f),
        )
        shader.setLocalMatrix(android.graphics.Matrix().apply { setRotate(degrees, center.x, center.y) })
        return shader
    }
}
