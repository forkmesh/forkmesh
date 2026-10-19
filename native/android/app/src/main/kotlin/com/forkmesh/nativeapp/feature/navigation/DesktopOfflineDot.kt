package com.forkmesh.nativeapp.feature.navigation

import androidx.compose.animation.core.RepeatMode
import androidx.compose.animation.core.animateFloat
import androidx.compose.animation.core.infiniteRepeatable
import androidx.compose.animation.core.rememberInfiniteTransition
import androidx.compose.animation.core.tween
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.draw.clip
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.control.DesktopReach
import com.forkmesh.nativeapp.feature.agents.AgentsFeature
import com.forkmesh.nativeapp.ui.theme.Fm

@Composable
fun rememberDesktopOffline(): Boolean {
    val hub = remember { runCatching { AgentsFeature.hub }.getOrNull() } ?: return false
    var offline by remember(hub) { mutableStateOf(hub.reach is DesktopReach.Offline) }
    DisposableEffect(hub) {
        val listener: () -> Unit = { offline = hub.reach is DesktopReach.Offline }
        hub.addListener(listener)
        listener()
        onDispose { hub.removeListener(listener) }
    }
    return offline
}

@Composable
fun DesktopOfflineDot(modifier: Modifier = Modifier) {
    val alpha = if (Fm.reducedMotion) {
        1f
    } else {
        val value by rememberInfiniteTransition(label = "desktop-offline-dot").animateFloat(
            initialValue = 1f,
            targetValue = 0.2f,
            animationSpec = infiniteRepeatable(tween(1200), RepeatMode.Reverse),
            label = "desktop-offline-dot-alpha",
        )
        value
    }
    Box(
        modifier
            .alpha(alpha)
            .clip(CircleShape)
            .background(Fm.colors.bgRaised)
            .padding(1.5.dp)
            .size(7.dp)
            .clip(CircleShape)
            .background(Fm.colors.warning),
    )
}
