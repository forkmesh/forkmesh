package com.forkmesh.nativeapp.feature.aquarium

import android.provider.Settings
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.navigationBars
import androidx.compose.foundation.layout.statusBars
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableLongStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.graphics.drawscope.scale
import androidx.compose.ui.graphics.graphicsLayer
import androidx.compose.ui.layout.onGloballyPositioned
import androidx.compose.ui.layout.positionInWindow
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalWindowInfo
import androidx.compose.ui.semantics.clearAndSetSemantics
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.Dp
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.isSpecified
import com.forkmesh.nativeapp.aquarium.AquariumRect
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlin.math.max

private const val FRAME_NANOS = 29_000_000L

@Composable
fun AquariumLayer(hidden: Boolean, floor: Dp, modifier: Modifier = Modifier) {
    val on by AquariumFeature.on.collectAsState()
    LaunchedEffect(Unit) { AquariumFeature.refresh() }
    LaunchedEffect(on) { if (!on) AquariumDirector.reset() }
    if (on) AquariumTankView(hidden, floor, modifier)
}

@Composable
private fun AquariumTankView(hidden: Boolean, floor: Dp, modifier: Modifier) {
    val foreground by AppLifecycle.foreground.collectAsState()
    val swimmers by AquariumFeature.swimmers.collectAsState()
    val landmarks by AquariumFeature.landmarks.collectAsState()
    val density = LocalDensity.current
    val context = LocalContext.current
    val calm = remember(foreground) {
        Settings.Global.getFloat(context.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f) == 0f
    }
    val running = !hidden && foreground
    val measurer = rememberTextMeasurer(cacheSize = 64)
    val layouts = remember { HashMap<String, TextLayoutResult>() }
    var box by remember { mutableStateOf(IntSize.Zero) }
    var origin by remember { mutableStateOf(Offset.Zero) }
    val windowHeight = LocalWindowInfo.current.containerSize.height
    val statusTop = WindowInsets.statusBars.getTop(density)
    val navBottom = WindowInsets.navigationBars.getBottom(density)
    var tick by remember { mutableLongStateOf(0L) }
    val currentSwimmers by rememberUpdatedState(swimmers)
    val currentCalm by rememberUpdatedState(calm)
    val currentFloor by rememberUpdatedState(floor)

    LaunchedEffect(landmarks) { AquariumDirector.see(landmarks) }

    LaunchedEffect(running) {
        if (!running) {
            AquariumDirector.pause()
            return@LaunchedEffect
        }
        var drawn = 0L
        while (true) {
            withFrameNanos { now ->
                val d = density.density.toDouble()
                if (box.width > 0 && box.height > 0 && now - drawn >= FRAME_NANOS) {
                    drawn = now
                    val bottomGap = max(0, navBottom - (windowHeight - (origin.y + box.height).toInt()))
                    val ground = if (currentFloor.isSpecified) currentFloor.value.toDouble() else (box.height - bottomGap) / d
                    AquariumDirector.frame(
                        now,
                        AquariumRect(0.0, 0.0, box.width / d, box.height / d),
                        max(0f, statusTop - origin.y) / d,
                        ground,
                        currentSwimmers,
                        currentCalm,
                    )
                    tick = now
                }
            }
        }
    }

    Canvas(
        modifier
            .fillMaxSize()
            .onGloballyPositioned {
                origin = it.positionInWindow()
                box = it.size
            }
            .graphicsLayer { alpha = if (hidden) 0f else 1f }
            .clearAndSetSemantics {},
    ) {
        if (tick < 0) return@Canvas
        val tank = AquariumDirector.current ?: return@Canvas
        scale(density.density, Offset.Zero) {
            AquariumPainter(tank, measurer, layouts).paint(this)
        }
    }
}
