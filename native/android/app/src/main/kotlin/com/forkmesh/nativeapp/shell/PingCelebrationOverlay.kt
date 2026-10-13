package com.forkmesh.nativeapp.shell

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.LinearEasing
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.scale
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.NotificationService
import com.forkmesh.nativeapp.model.ForkNotification
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.LocalFmReducedMotion
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

private const val CELEBRATION_MS = 6000
private const val RAIN = 140
private const val BURST = 60

@Composable
fun PingCelebrationOverlay() {
    val service = Services.get<NotificationService>() ?: return
    val celebration by service.celebration.collectAsState()
    val baseline = remember(service) { service.celebration.value?.count ?: 0 }
    val reduced = LocalFmReducedMotion.current
    val haptics = FmHaptics.current
    val c = Fm.colors
    val colors = listOf(c.success, c.accent, c.warning, c.danger, c.codeType, c.codeKeyword)
    var shown by remember { mutableStateOf<ForkNotification?>(null) }
    val progress = remember { Animatable(0f) }
    val pop = remember { Animatable(0f) }

    LaunchedEffect(celebration?.count) {
        val next = celebration ?: return@LaunchedEffect
        if (next.count <= baseline) return@LaunchedEffect
        shown = next.ping
        haptics.commit()
        progress.snapTo(0f)
        pop.snapTo(0.4f)
        pop.animateTo(1f, tween(320))
        progress.animateTo(1f, tween(CELEBRATION_MS - 320, easing = LinearEasing))
        shown = null
    }

    val ping = shown ?: return
    val seconds = progress.value * CELEBRATION_MS / 1000f
    val fade = min(1f, max(0f, (CELEBRATION_MS / 1000f - seconds)))
    Box(Modifier.fillMaxSize()) {
        if (!reduced) {
            Canvas(Modifier.fillMaxSize()) {
                drawRain(seconds, fade, colors)
                drawBurst(seconds, fade, colors)
            }
        }
        Column(
            Modifier
                .align(Alignment.Center)
                .offset(y = (-80).dp)
                .padding(horizontal = 24.dp)
                .scale(pop.value)
                .graphicsLayer { alpha = fade }
                .background(Color.Black.copy(alpha = 0.78f), RoundedCornerShape(22.dp))
                .padding(horizontal = 26.dp, vertical = 18.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
        ) {
            Text(ping.title, color = Color.White, fontSize = 26.sp, fontWeight = FontWeight.ExtraBold, textAlign = TextAlign.Center)
            if (ping.body.isNotEmpty()) {
                Text(
                    ping.body,
                    color = Color.White.copy(alpha = 0.85f),
                    fontSize = 15.sp,
                    fontWeight = FontWeight.Medium,
                    textAlign = TextAlign.Center,
                    maxLines = 2,
                    overflow = TextOverflow.Ellipsis,
                    modifier = Modifier.padding(top = 4.dp),
                )
            }
        }
    }
}

private fun DrawScope.drawRain(t: Float, fade: Float, colors: List<Color>) {
    for (index in 0 until RAIN) {
        val seed = index.toFloat()
        val delay = abs(sin(seed * 3.17f)) * 1.6f
        val age = t - delay
        if (age <= 0f) continue
        val fall = (140f + 160f * abs(sin(seed * 78.233f))) * density
        val x = abs(sin(seed * 12.9898f)) * size.width + sin(age * 2f + seed) * 24f * density
        val y = -20f * density + fall * age
        if (y > size.height + 20f * density) continue
        piece(Offset(x, y), age * (3f + seed % 4f) + seed, index, colors[index % colors.size], fade)
    }
}

private fun DrawScope.drawBurst(t: Float, fade: Float, colors: List<Color>) {
    val alpha = fade * max(0f, 1f - t / 3f)
    if (alpha <= 0f) return
    for (index in 0 until BURST) {
        val seed = index.toFloat()
        val angle = seed / BURST * 2f * PI.toFloat() + 0.35f * sin(seed * 12.9898f)
        val speed = (220f + 180f * abs(sin(seed * 78.233f))) * density
        val x = size.width / 2f + cos(angle) * speed * t
        val y = size.height * 0.4f + sin(angle) * speed * t + 260f * density * t * t
        piece(Offset(x, y), t * 6f + seed, index, colors[(index + 2) % colors.size], alpha)
    }
}

private fun DrawScope.piece(at: Offset, spin: Float, index: Int, color: Color, alpha: Float) {
    val tint = color.copy(alpha = alpha.coerceIn(0f, 1f))
    rotate(spin * 180f / PI.toFloat(), pivot = at) {
        if (index % 3 == 0) {
            drawCircle(tint, radius = 3.5f * density, center = at)
        } else {
            val w = 10f * density
            val h = 4f * density
            drawRoundRect(tint, topLeft = Offset(at.x - w / 2f, at.y - h / 2f), size = Size(w, h), cornerRadius = CornerRadius(density))
        }
    }
}
