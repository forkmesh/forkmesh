package com.forkmesh.nativeapp.feature.agents

import android.os.Build
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.offset
import androidx.compose.foundation.layout.statusBars
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentTokenSeries
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.tools.drawTelemetryChart
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.delay

private object TokenCharts {
    val WIDTH = 126.dp
    val HEIGHT = 12.dp
    val GAP = 6.dp
    const val INPUT_FLOOR = 1_000.0
    const val OUTPUT_FLOOR = 50.0
    const val FILL_OPACITY = 0.35f
    const val EVERY_MS = 1_000L
}

@Composable
fun AgentTokenChartsOverlay(modifier: Modifier = Modifier) {
    val settings = Services.get<SettingsService>()
    val shown = settings?.showTelemetryStrip?.collectAsState()?.value ?: true
    if (!shown) return
    WatchDesktopTokens()
    val center = cutoutCenter()
    val density = LocalDensity.current
    var middle by remember { mutableStateOf<Dp?>(null) }
    Box(
        modifier
            .fillMaxWidth()
            .windowInsetsPadding(WindowInsets.statusBars)
            .onGloballyPositioned { box ->
                middle = with(density) { (box.positionInWindow().x + box.size.width / 2f).toDp() }
            },
    ) {
        val at = middle
        val shift = if (center == null || at == null) 0.dp else center - at
        AgentTokenCharts(Modifier.align(Alignment.TopCenter).offset(x = shift))
    }
}

@Composable
private fun cutoutCenter(): Dp? {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) return null
    val view = LocalView.current
    val density = LocalDensity.current
    val rect = view.rootWindowInsets?.displayCutout?.boundingRects?.firstOrNull { it.top <= 0 } ?: return null
    return with(density) { rect.exactCenterX().toDp() }
}

@Composable
private fun AgentTokenCharts(modifier: Modifier = Modifier) {
    val c = Fm.colors
    val series = remember { AgentTokenSeries() }
    var tick by remember { mutableStateOf(0) }
    LaunchedEffect(Unit) {
        while (true) {
            val feed = agentTokenFeed()
            val rows = runCatching { AgentsFeature.control.rows }.getOrDefault(emptyList())
            series.tick(AgentTokenSeries.totals(feed?.totals.orEmpty(), rows))
            tick++
            delay(TokenCharts.EVERY_MS)
        }
    }
    tick.let { }
    val input = series.input.toDoubleArray()
    val output = series.output.toDoubleArray()
    val summary = AgentTokenSeries.summary(series)
    val inTone = c.accent
    val outTone = c.success
    val chartWidth = (TokenCharts.WIDTH - TokenCharts.GAP) / 2
    Row(
        modifier
            .width(TokenCharts.WIDTH)
            .height(TokenCharts.HEIGHT)
            .semantics { contentDescription = summary },
        horizontalArrangement = Arrangement.spacedBy(TokenCharts.GAP),
    ) {
        TokenChart(input, TokenCharts.INPUT_FLOOR, inTone, Modifier.width(chartWidth).height(TokenCharts.HEIGHT))
        TokenChart(output, TokenCharts.OUTPUT_FLOOR, outTone, Modifier.width(chartWidth).height(TokenCharts.HEIGHT))
    }
}

@Composable
private fun TokenChart(values: DoubleArray, floor: Double, tone: Color, modifier: Modifier) {
    Canvas(modifier) {
        drawTelemetryChart(values, AgentTokenSeries.SECONDS, floor, tone, tone.copy(alpha = TokenCharts.FILL_OPACITY), strokeWidth = 1.dp.toPx())
    }
}
