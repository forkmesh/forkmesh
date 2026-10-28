package com.forkmesh.nativeapp.feature.hosts

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.control.DesktopChoice
import com.forkmesh.nativeapp.control.ShellLocalDesktop
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.hosts.DesktopHealth
import com.forkmesh.nativeapp.hosts.DesktopHealthLevel
import com.forkmesh.nativeapp.insights.NodeResources
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon
import kotlinx.coroutines.delay

internal data class ShellDesktopLights(
    val desktop: ShellLocalDesktop,
    val others: List<DesktopChoice>,
    val load: DesktopHealthLevel?,
    val usage: NodeResources?,
) {
    val label: String
        get() {
            val parts = arrayListOf(desktop.label)
            if (desktop.isConnected && load != null) parts.add(load.label.lowercase())
            usage?.let { parts.add(it.label) }
            return (listOf(parts.joinToString(", ")) + others.map { it.label }).joinToString("; ")
        }

    companion object {
        const val CALM_REFRESH_MS = 15 * 60_000L
        const val BUSY_REFRESH_MS = 2 * 60_000L

        fun usage(health: DesktopHealth?): NodeResources? {
            val read = health ?: return null
            val cpu = read.cpuPercent.takeIf { it >= 0 }
            val memory = read.memoryUsedShareOrNull?.times(100)
            val disk = if (read.diskTotal > 0 && read.diskFree >= 0) read.diskUsedShare * 100 else null
            return NodeResources.of(cpu, memory, disk)
        }
    }
}

@Composable
internal fun rememberShellDesktopLights(): ShellDesktopLights? {
    val hub = remember { runCatching { AgentsFeature.hub }.getOrNull() } ?: return null
    val store = remember(hub) { runCatching { DesktopRemoteFeature.diagnostics }.getOrNull() }
    var tick by remember(hub) { mutableIntStateOf(0) }
    DisposableEffect(hub, store) {
        val listener: () -> Unit = { tick++ }
        hub.addListener(listener)
        store?.addListener(listener)
        onDispose {
            hub.removeListener(listener)
            store?.removeListener(listener)
        }
    }
    val connected = tick.let { hub.currentLink != null }
    LaunchedEffect(hub, store, connected) {
        if (store == null || !connected) return@LaunchedEffect
        while (true) {
            if (AppLifecycle.foreground.value && hub.currentLink != null) offMain { store.refreshHealth() }
            val level = store.health?.level
            delay(if (level == null || level == DesktopHealthLevel.Ok) ShellDesktopLights.CALM_REFRESH_MS else ShellDesktopLights.BUSY_REFRESH_MS)
        }
    }
    tick.let { }
    val desktop = hub.localDesktop
    val choices = hub.choices
    if (desktop == ShellLocalDesktop.Hidden && choices.isEmpty()) return null
    val others = if (choices.size > 1) choices.filter { !it.active } else emptyList()
    val health = if (desktop.isConnected) store?.health else null
    return ShellDesktopLights(desktop, others, health?.level?.takeIf { it != DesktopHealthLevel.Unknown }, ShellDesktopLights.usage(health))
}

@Composable
internal fun ShellDesktopLightsRow(lights: ShellDesktopLights, side: Dp, overlap: Dp) {
    Row(horizontalArrangement = Arrangement.spacedBy(overlap), verticalAlignment = Alignment.CenterVertically) {
        ShellDesktopLight(lights, side)
        for (other in lights.others) ShellStackedDesktopLight(other, side)
    }
}

@Composable
private fun ShellDesktopLight(lights: ShellDesktopLights, side: Dp) {
    val c = Fm.colors
    val desktop = lights.desktop
    val alarmed = !desktop.isConnected || lights.load == DesktopHealthLevel.Strained
    val dot = when {
        alarmed -> c.danger
        lights.load == DesktopHealthLevel.Busy -> c.warning
        else -> c.success
    }
    val alpha = if (alarmed && !Fm.reducedMotion) {
        val value by rememberInfiniteTransition(label = "desktop-light").animateFloat(
            initialValue = 1f,
            targetValue = 0.25f,
            animationSpec = infiniteRepeatable(tween(600), RepeatMode.Reverse),
            label = "desktop-light-alpha",
        )
        value
    } else {
        1f
    }
    val relayed = desktop is ShellLocalDesktop.Relay
    Box(Modifier.size(side), contentAlignment = Alignment.Center) {
        FmIcon("desktop_windows", size = 10.dp, tint = c.textSecondary)
        Box(
            Modifier
                .align(Alignment.TopEnd)
                .offset(x = 3.dp, y = (-2).dp)
                .alpha(alpha)
                .size(8.dp)
                .background(c.bgBase, CircleShape)
                .padding(1.dp)
                .background(dot, CircleShape),
        )
        val usage = lights.usage
        if (usage != null) {
            ShellResourceBars(usage.values, Modifier.align(Alignment.BottomEnd).offset(x = 3.dp, y = 2.dp))
            if (relayed) RelayGlobe(Modifier.align(Alignment.BottomStart).offset(x = (-4).dp, y = 3.dp))
        } else if (relayed) {
            RelayGlobe(Modifier.align(Alignment.BottomEnd).offset(x = 4.dp, y = 3.dp))
        }
    }
}

@Composable
private fun RelayGlobe(modifier: Modifier) {
    val c = Fm.colors
    Box(modifier.background(c.bgBase, CircleShape).padding(0.5.dp)) {
        FmIcon("public", size = 7.dp, tint = c.textSecondary)
    }
}

@Composable
private fun ShellStackedDesktopLight(choice: DesktopChoice, side: Dp) {
    val c = Fm.colors
    Box(Modifier.height(side), contentAlignment = Alignment.Center) {
        Box(Modifier.background(c.bgBase, RoundedCornerShape(3.dp)).padding(1.dp)) {
            FmIcon("desktop_windows", size = 10.dp, tint = c.textSecondary)
        }
        Box(
            Modifier
                .align(Alignment.TopEnd)
                .offset(x = 2.dp, y = (-2).dp)
                .size(7.dp)
                .background(c.bgBase, CircleShape)
                .padding(1.dp)
                .background(desktopPresenceColor(choice.presence), CircleShape),
        )
    }
}

@Composable
internal fun ShellResourceBars(values: List<Double?>, modifier: Modifier = Modifier, barWidth: Dp = 1.5.dp, height: Dp = 6.dp) {
    val c = Fm.colors
    Row(
        modifier
            .background(c.bgBase, RoundedCornerShape(1.5.dp))
            .padding(0.75.dp),
        horizontalArrangement = Arrangement.spacedBy(0.5.dp),
        verticalAlignment = Alignment.Bottom,
    ) {
        for (value in values) {
            Box(Modifier.width(barWidth).height(height).background(c.textTertiary.copy(alpha = 0.35f)), contentAlignment = Alignment.BottomCenter) {
                if (value != null) {
                    val tint = when {
                        value >= NodeResources.BAD -> c.danger
                        value >= NodeResources.WARNING -> c.warning
                        else -> c.success
                    }
                    val share = (value.coerceIn(0.0, 100.0) / 100).toFloat()
                    Box(Modifier.fillMaxWidth().height(maxOf(1.5.dp, height * share)).background(tint))
                }
            }
        }
    }
}
