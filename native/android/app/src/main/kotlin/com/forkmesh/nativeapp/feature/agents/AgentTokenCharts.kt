package com.forkmesh.nativeapp.feature.agents

import android.content.Context
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.os.Build
import android.view.Surface
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
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalView
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.agents.AgentTokenSeries
import com.forkmesh.nativeapp.agents.AgentTokenTank
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.SettingsService
import com.forkmesh.nativeapp.feature.genie.screenVector
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.delay

private object TokenCharts {
    val WIDTH = 126.dp
    val HEIGHT = 12.dp
    val GAP = 6.dp
    const val INPUT_FLOOR = 1_000.0
    const val OUTPUT_FLOOR = 50.0
    const val TRACK_OPACITY = 0.1f
    val CORNER = 2.dp
    val SHADES = listOf(0.55f, 0.7f, 0.85f, 1f)
    const val EVERY_MS = 1_000L
    const val IDLE_MS = 200L
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
    val chartWidth = (TokenCharts.WIDTH - TokenCharts.GAP) / 2
    val series = remember { AgentTokenSeries() }
    val input = remember { AgentTokenTank(chartWidth.value.toDouble(), TokenCharts.HEIGHT.value.toDouble(), seed = 1L) }
    val output = remember { AgentTokenTank(chartWidth.value.toDouble(), TokenCharts.HEIGHT.value.toDouble(), seed = 2L) }
    var summary by remember { mutableStateOf(AgentTokenSeries.summary(series)) }
    var frame by remember { mutableLongStateOf(0L) }
    LaunchedEffect(Unit) {
        while (true) {
            val feed = agentTokenFeed()
            val rows = runCatching { AgentsFeature.control.rows }.getOrDefault(emptyList())
            series.tick(AgentTokenSeries.totals(feed?.totals.orEmpty(), rows))
            input.pour(series.input.lastOrNull() ?: 0.0, AgentTokenTank.scale(series.input, TokenCharts.INPUT_FLOOR))
            output.pour(series.output.lastOrNull() ?: 0.0, AgentTokenTank.scale(series.output, TokenCharts.OUTPUT_FLOOR))
            summary = AgentTokenSeries.summary(series)
            delay(TokenCharts.EVERY_MS)
        }
    }
    LaunchedEffect(Unit) {
        var last = 0L
        while (true) {
            withFrameNanos { now ->
                if (last != 0L) {
                    val seconds = (now - last) / 1e9
                    input.advance(seconds)
                    output.advance(seconds)
                }
                last = now
            }
            frame++
            if (input.idle && output.idle) {
                last = 0L
                delay(TokenCharts.IDLE_MS)
            }
        }
    }
    TiltTanks(input, output)
    Row(
        modifier
            .width(TokenCharts.WIDTH)
            .height(TokenCharts.HEIGHT)
            .semantics { contentDescription = summary },
        horizontalArrangement = Arrangement.spacedBy(TokenCharts.GAP),
    ) {
        TokenTank(input, c.accent, { frame }, Modifier.width(chartWidth).height(TokenCharts.HEIGHT))
        TokenTank(output, c.success, { frame }, Modifier.width(chartWidth).height(TokenCharts.HEIGHT))
    }
}

@Composable
private fun TiltTanks(vararg tanks: AgentTokenTank) {
    val context = LocalContext.current
    val view = LocalView.current
    DisposableEffect(Unit) {
        val sensors = context.getSystemService(Context.SENSOR_SERVICE) as? SensorManager
        val listener = object : SensorEventListener {
            override fun onSensorChanged(event: SensorEvent) {
                val rotation = view.display?.rotation ?: Surface.ROTATION_0
                val pull = screenVector(event.values[0], event.values[1], rotation)
                for (tank in tanks) tank.tilt(pull.dx / SensorManager.GRAVITY_EARTH, pull.dy / SensorManager.GRAVITY_EARTH)
            }

            override fun onAccuracyChanged(sensor: Sensor?, accuracy: Int) {}
        }
        sensors?.getDefaultSensor(Sensor.TYPE_ACCELEROMETER)?.let { sensors.registerListener(listener, it, SensorManager.SENSOR_DELAY_GAME) }
        onDispose { sensors?.unregisterListener(listener) }
    }
}

@Composable
private fun TokenTank(tank: AgentTokenTank, tone: Color, frame: () -> Long, modifier: Modifier) {
    Canvas(modifier) {
        frame()
        drawRoundRect(tone.copy(alpha = TokenCharts.TRACK_OPACITY), cornerRadius = CornerRadius(TokenCharts.CORNER.toPx()))
        val unit = size.width / tank.width.toFloat()
        val grain = Size(unit, unit)
        val columns = tank.width.toInt() - 1
        val rows = tank.height.toInt() - 1
        val shades = TokenCharts.SHADES.map { tone.copy(alpha = it) }
        for (i in 0 until tank.grains) {
            val x = tank.xs[i].toInt().coerceIn(0, columns) * unit
            val y = tank.ys[i].toInt().coerceIn(0, rows) * unit
            drawRect(shades[(tank.shades[i] - 1).coerceIn(0, shades.size - 1)], topLeft = Offset(x, y), size = grain)
        }
    }
}
