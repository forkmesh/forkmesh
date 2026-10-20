package com.forkmesh.nativeapp.shell

import androidx.compose.animation.AnimatedVisibility
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.animation.fadeIn
import androidx.compose.animation.fadeOut
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.delay

const val SHELL_LOADING_DELAY_MS = 250L

@Composable
fun ShellLoadingBar(modifier: Modifier = Modifier, period: Int = 1200) {
    val accent = Fm.colors.accent
    val reduced = Fm.reducedMotion
    val sweep by rememberInfiniteTransition(label = "loading-bar").animateFloat(
        initialValue = 0f,
        targetValue = 1f,
        animationSpec = infiniteRepeatable(tween(period, easing = LinearEasing), RepeatMode.Restart),
        label = "loading-bar-sweep",
    )
    Canvas(
        modifier
            .fillMaxWidth()
            .height(2.dp)
            .semantics { contentDescription = "Loading" },
    ) {
        val width = size.width
        val radius = CornerRadius(size.height / 2f)
        if (reduced) {
            drawRoundRect(accent.copy(alpha = 0.4f), Offset.Zero, size, radius)
        } else {
            val segment = maxOf(width * 0.3f, 1f)
            val eased = sweep * sweep * (3f - 2f * sweep)
            val x = -segment + (width + segment) * eased
            clipRect {
                drawRoundRect(accent.copy(alpha = 0.75f), Offset(x, 0f), Size(segment, size.height), radius)
            }
        }
    }
}

@Composable
fun ShellLoadingOverlay(active: Boolean, modifier: Modifier = Modifier, delayMs: Long = SHELL_LOADING_DELAY_MS) {
    var shown by remember { mutableStateOf(false) }
    LaunchedEffect(active) {
        if (!active) {
            shown = false
        } else {
            delay(delayMs)
            shown = true
        }
    }
    AnimatedVisibility(visible = shown, modifier = modifier, enter = fadeIn(), exit = fadeOut()) {
        ShellLoadingBar()
    }
}
