package com.forkmesh.nativeapp.shell

import android.os.Build
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
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.alpha
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.hosts.ShellDesktopLights
import com.forkmesh.nativeapp.feature.hosts.ShellDesktopLightsRow
import com.forkmesh.nativeapp.feature.hosts.ShellResourceBars
import com.forkmesh.nativeapp.feature.hosts.rememberShellDesktopLights
import com.forkmesh.nativeapp.feature.insights.ShellStatusDot
import com.forkmesh.nativeapp.feature.insights.StatusPageFeature
import com.forkmesh.nativeapp.feature.tools.ToolsFeature
import com.forkmesh.nativeapp.insights.FleetLight
import com.forkmesh.nativeapp.insights.FleetLightKind
import com.forkmesh.nativeapp.insights.StatusDot
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmIcon

@Composable
internal fun rememberShellIslandLayout(): ShellIslandLayout {
    val density = LocalDensity.current
    val view = LocalView.current
    val statusPx = WindowInsets.statusBars.getTop(density)
    val widthPx = LocalWindowInfo.current.containerSize.width.takeIf { it > 0 } ?: view.rootView.width
    return remember(statusPx, widthPx, density) {
        val scale = density.density
        val cutout = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.P) {
            view.rootWindowInsets?.displayCutout?.boundingRects
                ?.filter { it.top < statusPx && it.width() < widthPx }
                ?.minByOrNull { kotlin.math.abs(it.centerX() - widthPx / 2) }
                ?.let { IslandRect(it.left / scale, it.top / scale, it.right / scale, it.bottom / scale) }
        } else {
            null
        }
        ShellIsland.layout(widthPx / scale, statusPx / scale, cutout)
    }
}

@Composable
internal fun rememberShellIslandEars(): Boolean {
    val settings = Services.get<SettingsService>()
    val diagnostics = settings?.showTelemetryStrip?.collectAsState()?.value ?: true
    return diagnostics && rememberShellIslandLayout().inStatusBar
}

@Composable
internal fun ShellIslandStatus(modifier: Modifier = Modifier) {
    val settings = Services.get<SettingsService>()
    val diagnostics = settings?.showTelemetryStrip?.collectAsState()?.value ?: true
    if (!diagnostics) return
    val statusOn by StatusPageFeature.installed.collectAsState()
    val row by StatusPageFeature.row.collectAsState()
    val desktop = rememberShellDesktopLights()
    val layout = rememberShellIslandLayout()
    val dots = if (statusOn) row.services else emptyList()
    val fleet = if (statusOn) row.fleet else emptyList()
    val lightsShown = desktop != null || fleet.isNotEmpty()
    val density = LocalDensity.current
    var origin by remember { mutableStateOf(androidx.compose.ui.geometry.Offset.Zero) }
    val dx = with(density) { -origin.x.toDp() }
    val dy = with(density) { -origin.y.toDp() }
    Box(modifier.fillMaxWidth().onGloballyPositioned { origin = it.positionInWindow() }) {
        if (layout.inStatusBar) {
            if (dots.isNotEmpty()) {
                val band = layout.dots
                Box(Modifier.band(band, dx, dy), contentAlignment = Alignment.Center) { IslandDots(dots) }
            }
            if (lightsShown) {
                Box(Modifier.band(layout.lights, dx, dy), contentAlignment = Alignment.Center) { ClockLights(desktop, fleet) }
            }
            layout.charts?.let { band ->
                Box(Modifier.band(band, dx, dy), contentAlignment = Alignment.Center) {
                    ToolsFeature.TelemetryEarSparks(band.width.dp, band.height.dp)
                }
            }
        } else if (lightsShown || dots.isNotEmpty()) {
            val band = layout.lights
            Row(
                Modifier.band(band, dx, dy),
                horizontalArrangement = Arrangement.spacedBy(6.dp, Alignment.CenterHorizontally),
                verticalAlignment = Alignment.CenterVertically,
            ) {
                if (lightsShown) ClockLights(desktop, fleet)
                if (dots.isNotEmpty()) IslandDots(dots)
            }
        }
    }
}

private fun Modifier.band(band: IslandBand, dx: Dp, dy: Dp): Modifier =
    offset(x = band.start.dp + dx, y = band.top.dp + dy).width(band.width.dp).height(band.height.dp)

@Composable
private fun IslandDots(dots: List<StatusDot>) {
    Row(
        Modifier
            .clearAndSetSemantics { contentDescription = "Status: " + dots.joinToString("; ") { it.description } }
            .testTag("top-island-status-dots"),
        horizontalArrangement = Arrangement.spacedBy(2.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        dots.forEach { ShellStatusDot(it, ShellIsland.DOT.dp) }
    }
}

@Composable
private fun ClockLights(desktop: ShellDesktopLights?, fleet: List<FleetLight>) {
    val cores = fleet.filter { it.kind == FleetLightKind.Core }
    val runners = fleet.filter { it.kind == FleetLightKind.Runner }
    val label = (listOfNotNull(desktop?.label) + cores.map { it.label } + runners.map { it.label }).joinToString("; ")
    val side = ShellIsland.EAR_HEIGHT.dp
    val overlap = ShellIsland.STACK_OVERLAP.dp
    Row(
        Modifier
            .height(side)
            .clearAndSetSemantics { contentDescription = label }
            .testTag("top-clock-lights"),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        desktop?.let { ShellDesktopLightsRow(it, side, overlap) }
        if (cores.isNotEmpty()) {
            Row(horizontalArrangement = Arrangement.spacedBy(overlap), verticalAlignment = Alignment.CenterVertically) {
                cores.forEach { FleetLightGlyph(it) }
            }
        }
        if (runners.isNotEmpty()) {
            Row(horizontalArrangement = Arrangement.spacedBy(1.dp), verticalAlignment = Alignment.CenterVertically) {
                runners.forEach { FleetLightGlyph(it) }
            }
        }
    }
}

@Composable
private fun FleetLightGlyph(light: FleetLight) {
    val c = Fm.colors
    val alpha = if (!light.healthy && !Fm.reducedMotion) {
        val value by rememberInfiniteTransition(label = "fleet-light").animateFloat(
            initialValue = 1f,
            targetValue = 0.25f,
            animationSpec = infiniteRepeatable(tween(600), RepeatMode.Reverse),
            label = "fleet-light-alpha",
        )
        value
    } else {
        1f
    }
    Box {
        Box(Modifier.background(c.bgBase, RoundedCornerShape(3.dp)).padding(1.dp)) {
            FmIcon(
                if (light.kind == FleetLightKind.Core) "dns_outlined" else "memory",
                modifier = Modifier.alpha(alpha).size(11.dp),
                size = 11.dp,
                tint = if (light.healthy) c.success else c.danger,
            )
        }
        light.resources?.let { ShellResourceBars(it.values, Modifier.align(Alignment.BottomEnd)) }
    }
}
