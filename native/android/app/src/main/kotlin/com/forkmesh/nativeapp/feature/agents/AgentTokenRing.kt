package com.forkmesh.nativeapp.feature.agents

import androidx.compose.foundation.Canvas
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentChipFacts
import com.forkmesh.nativeapp.agents.AgentTokenClock
import com.forkmesh.nativeapp.agents.AgentTokenPace
import com.forkmesh.nativeapp.agents.AgentTokenSampler
import com.forkmesh.nativeapp.control.DesktopTokenFeed
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.delay

private object TokenRing {
    val WIDTH = 1.dp
    const val DASH_ON = 2.5f
    const val DASH_OFF = 2f
    const val WRAP = (DASH_ON + DASH_OFF).toDouble()
}

private data class TokenFlowState(
    val reading: List<Long> = listOf(0L, 0L),
    val inPace: AgentTokenPace = AgentTokenPace.Idle,
    val outPace: AgentTokenPace = AgentTokenPace.Idle,
    val inTravel: Double = 0.0,
    val outTravel: Double = 0.0,
)

internal fun agentTokenFeed(): DesktopTokenFeed? = runCatching { AgentsFeature.tokens }.getOrNull()

@Composable
internal fun WatchDesktopTokens() {
    val foreground by AppLifecycle.foreground.collectAsState()
    DisposableEffect(foreground) {
        val feed = if (foreground) agentTokenFeed() else null
        feed?.acquire()
        onDispose { feed?.release() }
    }
}

@Composable
internal fun AgentTokenRing(facts: AgentChipFacts, radius: Dp, modifier: Modifier = Modifier) {
    val c = Fm.colors
    val reduced = Fm.reducedMotion
    val sampler = remember { AgentTokenSampler() }
    val clock = remember { AgentTokenClock() }
    var flow by remember { mutableStateOf(TokenFlowState()) }
    var frame by remember { mutableLongStateOf(0L) }
    val base = listOf(maxOf(facts.tokensIn, 0).toLong(), maxOf(facts.tokensOut, 0).toLong())
    LaunchedEffect(facts.desktopSessionId, facts.active, base) {
        var previous: List<Long>? = null
        var changedAt = System.currentTimeMillis()
        while (true) {
            val fed = if (facts.desktopSessionId > 0) agentTokenFeed()?.agentTokens(facts.desktopSessionId) else null
            val now = if (fed != null && fed.size == 2) listOf(maxOf(base[0], fed[0]), maxOf(base[1], fed[1])) else base
            val at = System.currentTimeMillis()
            if (now != previous) {
                sampler.sample(now[0], now[1], at)
                previous = now
                changedAt = at
            } else if (facts.active) {
                sampler.hold(at)
            }
            if (at - changedAt >= AgentTokenSampler.SETTLE_AFTER_MS) sampler.settle()
            flow = if (facts.active) {
                TokenFlowState(now, sampler.inPace, sampler.outPace, sampler.inTravel, sampler.outTravel)
            } else {
                TokenFlowState(now)
            }
            if (!facts.active) break
            delay(AgentTokenSampler.HOLD_EVERY_MS)
        }
    }
    val moving = !reduced && (flow.inTravel > 0 || flow.outTravel > 0)
    LaunchedEffect(moving) {
        if (!moving) return@LaunchedEffect
        while (true) withFrameNanos { frame = it }
    }
    val counted = flow.reading[0] > 0 || flow.reading[1] > 0
    val hairline = c.border
    val inTint = paceTint(flow.inPace)
    val outTint = paceTint(flow.outPace)
    Canvas(modifier) {
        if (!counted) {
            val inset = TokenRing.WIDTH.toPx()
            val stroke = 0.5.dp.toPx()
            drawRoundRect(
                hairline,
                topLeft = Offset(inset, inset),
                size = Size(size.width - inset * 2, size.height - inset * 2),
                cornerRadius = CornerRadius((radius.toPx() - inset).coerceAtLeast(0f)),
                style = Stroke(stroke),
            )
            return@Canvas
        }
        clock.advance(frame / 1e9, moving, flow.inTravel, flow.outTravel, TokenRing.WRAP)
        val width = TokenRing.WIDTH.toPx()
        lane(width / 2, width, radius.toPx(), inTint, clock.inPhase)
        lane(width * 1.5f, width, radius.toPx(), outTint, clock.outPhase)
    }
}

private fun DrawScope.lane(inset: Float, width: Float, radius: Float, tint: Color, phase: Double) {
    val on = TokenRing.DASH_ON.dp.toPx()
    val off = TokenRing.DASH_OFF.dp.toPx()
    val wrap = on + off
    val shift = ((phase.toFloat().dp.toPx() % wrap) + wrap) % wrap
    drawRoundRect(
        tint,
        topLeft = Offset(inset, inset),
        size = Size(size.width - inset * 2, size.height - inset * 2),
        cornerRadius = CornerRadius((radius - inset).coerceAtLeast(0f)),
        style = Stroke(width, pathEffect = PathEffect.dashPathEffect(floatArrayOf(on, off), shift)),
    )
}

@Composable
private fun paceTint(pace: AgentTokenPace): Color {
    val c = Fm.colors
    return when (pace) {
        AgentTokenPace.Idle -> c.textTertiary
        AgentTokenPace.Slow -> c.accent
        AgentTokenPace.Steady -> c.success
        AgentTokenPace.Fast -> c.warning
        AgentTokenPace.Blazing -> c.danger
    }
}
