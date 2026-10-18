package com.forkmesh.nativeapp.feature.codeinsight

import android.provider.Settings
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.gestures.calculateCentroid
import androidx.compose.foundation.gestures.calculatePan
import androidx.compose.foundation.gestures.calculateZoom
import androidx.compose.foundation.horizontalScroll
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxScope
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.selection.selectable
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.runtime.snapshotFlow
import androidx.compose.runtime.withFrameNanos
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.focus.onFocusChanged
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.positionChange
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.platform.LocalFocusManager
import androidx.compose.ui.platform.LocalViewConfiguration
import androidx.compose.ui.semantics.CustomAccessibilityAction
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.customActions
import androidx.compose.ui.semantics.onClick
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.semantics.stateDescription
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import com.forkmesh.nativeapp.codeinsight.Synapse
import com.forkmesh.nativeapp.codeinsight.SynapseColourBy
import com.forkmesh.nativeapp.codeinsight.SynapseColours
import com.forkmesh.nativeapp.codeinsight.SynapseScene
import com.forkmesh.nativeapp.codeinsight.SynapseShowMode
import com.forkmesh.nativeapp.ui.kit.FmEmptyState
import com.forkmesh.nativeapp.ui.kit.FmIconButton
import com.forkmesh.nativeapp.ui.kit.FmLinearProgressIndicator
import com.forkmesh.nativeapp.ui.kit.FmTextField
import com.forkmesh.nativeapp.ui.navigation.Navigator
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmIcon
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import com.forkmesh.nativeapp.ui.theme.FmType
import java.text.NumberFormat
import kotlin.math.abs
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.isActive

@Composable
fun SynapsePage(context: CodeInsightContext, navigator: Navigator) {
    val snapshot = context.snapshot
    if (snapshot.error.isNotEmpty()) {
        FmEmptyState("hub", "Nothing to connect", message = snapshot.error)
        return
    }
    if (snapshot.files.isEmpty()) {
        FmEmptyState("hub", "No source files", message = "This repository has no source files Synapse can read.")
        return
    }
    val state = remember(snapshot) { SynapsePageState(snapshot) }
    val android = LocalContext.current
    val reduced = Fm.reducedMotion
    val scale = remember(android) {
        Settings.Global.getFloat(android.contentResolver, Settings.Global.ANIMATOR_DURATION_SCALE, 1f)
    }
    LaunchedEffect(state, reduced, scale) { state.applyMotionDefaults(reduced || scale == 0f, scale) }
    LaunchedEffect(state) { context.setSummary(state.summary) }
    LaunchedEffect(state, state.includeIsolated) { state.ensureLayout() }
    LaunchedEffect(state) {
        while (isActive) {
            if (!state.wantsFrames()) {
                state.resetClock()
                snapshotFlow { state.wantsFrames() || state.frame < 0 }.first { it }
            }
            withFrameNanos { state.onFrame(it) }
        }
    }
    Column(Modifier.fillMaxSize()) {
        SynapseControls(state)
        SynapseSearch(state)
        Box(Modifier.weight(1f).fillMaxWidth()) {
            SynapseBrain(state)
            SynapseOverlay(state)
        }
    }
    if (state.sheet != SynapseSheet.None) {
        SynapseDetailsSheet(state, context, onDismiss = { state.sheet = SynapseSheet.None })
    }
}

@Composable
private fun SynapseChip(label: String, selected: Boolean, description: String, onClick: () -> Unit) {
    val colors = Fm.colors
    val shape = RoundedCornerShape(FmRadius.md)
    Row(
        Modifier
            .height(30.dp)
            .clip(shape)
            .background(if (selected) colors.accentSubtle else colors.bgRaised)
            .border(1.dp, if (selected) colors.accent else colors.border, shape)
            .selectable(selected = selected, role = Role.RadioButton, onClick = onClick)
            .semantics { contentDescription = description }
            .padding(horizontal = FmSpace.x3),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            label,
            maxLines = 1,
            style = Fm.text.bodyMedium.copy(
                color = if (selected) colors.accent else colors.textSecondary,
                fontSize = FmType.meta,
                fontWeight = FmType.emphasis,
            ),
        )
    }
}

@Composable
private fun SynapseChipRow(title: String, content: @Composable () -> Unit) {
    Row(
        Modifier.fillMaxWidth().horizontalScroll(rememberScrollState()).padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
        horizontalArrangement = Arrangement.spacedBy(FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(title, style = Fm.text.labelSmall, color = Fm.colors.textTertiary)
        content()
    }
}

@Composable
private fun SynapseControls(state: SynapsePageState) {
    val haptics = FmHaptics.current
    SynapseChipRow("COLOUR") {
        for (mode in SynapseColourBy.entries) {
            val selected = state.colourBy == mode
            SynapseChip(mode.label, selected, if (selected) "Colour by ${mode.label}, selected" else "Colour by ${mode.label}") {
                if (!selected) haptics.selection()
                state.setColour(mode)
            }
        }
    }
    SynapseChipRow("SHOW") {
        for (mode in SynapseShowMode.entries) {
            val selected = state.showMode == mode && (mode != SynapseShowMode.Cycles || state.isolatedCycle < 0)
            SynapseChip(mode.label, selected, if (selected) "Show ${mode.label}, selected" else "Show ${mode.label}") {
                if (!selected) haptics.selection()
                state.setShow(mode)
            }
        }
        Spacer(Modifier.width(FmSpace.x2))
        for (hops in 1..3) {
            val selected = state.depth == hops
            val label = if (hops == 1) "1 hop" else "$hops hops"
            SynapseChip(label, selected, if (selected) "Neighbourhood depth $label, selected" else "Neighbourhood depth $label") {
                if (!selected) haptics.selection()
                state.setNeighbourhoodDepth(hops)
            }
        }
    }
}

@Composable
private fun SynapseSearch(state: SynapsePageState) {
    var text by remember(state) { mutableStateOf("") }
    var focused by remember { mutableStateOf(false) }
    val focus = LocalFocusManager.current
    val haptics = FmHaptics.current
    val snapshot = state.snapshot
    val matches = remember(text, state.graph) { SynapseScene.searchMatches(snapshot, text, 8, state.graph) }
    val choose = { file: Int ->
        haptics.selection()
        text = ""
        state.setSearch("")
        focus.clearFocus()
        state.select(file, fly = true)
        state.sheet = SynapseSheet.File
    }
    Column(Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1)) {
        FmTextField(
            value = text,
            onValueChange = {
                text = it
                state.setSearch(it)
            },
            modifier = Modifier.fillMaxWidth().onFocusChanged { focused = it.isFocused },
            hint = "Find a file…",
            leading = { FmIcon("search", size = 20.dp, contentDescription = null) },
            trailing = if (text.isEmpty()) null else ({
                FmIconButton("close", "Clear search", onPressed = {
                    text = ""
                    state.setSearch("")
                })
            }),
            keyboardOptions = KeyboardOptions(imeAction = ImeAction.Search),
            keyboardActions = KeyboardActions(onSearch = { matches.firstOrNull()?.let(choose) }),
        )
        if (focused && matches.isNotEmpty()) {
            Column(
                Modifier
                    .fillMaxWidth()
                    .padding(top = FmSpace.x1)
                    .clip(RoundedCornerShape(FmRadius.lg))
                    .background(Fm.colors.bgRaised)
                    .border(1.dp, Fm.colors.border, RoundedCornerShape(FmRadius.lg)),
            ) {
                for (file in matches) {
                    val info = snapshot.files[file]
                    Column(
                        Modifier
                            .fillMaxWidth()
                            .heightIn(min = 44.dp)
                            .clickable(onClickLabel = "Select and fly to") { choose(file) }
                            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x1),
                        verticalArrangement = Arrangement.Center,
                    ) {
                        Text(Synapse.fileName(info.path), style = Fm.text.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
                        Text(
                            "${info.path} · imports ${info.dependsOn.size} · imported by ${info.dependedOnBy.size}",
                            style = Fm.text.bodySmall,
                            color = Fm.colors.textSecondary,
                            maxLines = 1,
                            overflow = TextOverflow.MiddleEllipsis,
                        )
                    }
                }
            }
        }
    }
}

private fun brainDescription(state: SynapsePageState): String {
    val format = NumberFormat.getIntegerInstance()
    val graph = state.graph
    val parts = ArrayList<String>()
    parts.add("Import graph")
    if (graph != null) parts.add("${format.format(graph.nodes.size)} files, ${format.format(graph.imports)} imports")
    parts.add(Synapse.plural(state.snapshot.cycles.size, "circular dependency", "circular dependencies", format))
    if (state.selected >= 0) parts.add("selected ${state.snapshot.files[state.selected].path}")
    return parts.joinToString(", ")
}

@Composable
private fun BoxScope.SynapseBrain(state: SynapsePageState) {
    val density = LocalDensity.current
    val renderer = remember(density.density, density.fontScale) { SynapseRenderer(density.density, density.fontScale) }
    val haptics = FmHaptics.current
    val viewConfiguration = LocalViewConfiguration.current
    val touchSlop = viewConfiguration.touchSlop
    val doubleTapMillis = viewConfiguration.doubleTapTimeoutMillis
    val reach = with(density) { 10.dp.toPx() }
    val near = with(density) { 28.dp.toPx() }
    val orbitScale = 1.25f / density.density
    Canvas(
        Modifier
            .matchParentSize()
            .onSizeChanged { state.setSize(it.width.toFloat(), it.height.toFloat()) }
            .semantics {
                contentDescription = brainDescription(state)
                stateDescription = if (state.running) "Growing the brain" else "Ready"
                onClick(label = "Show the overview") {
                    state.sheet = if (state.selected >= 0) SynapseSheet.File else SynapseSheet.Overview
                    true
                }
                customActions = listOf(
                    CustomAccessibilityAction("Fit the brain") {
                        state.fit(true)
                        true
                    },
                    CustomAccessibilityAction("Clear the selection") {
                        state.clearSelection()
                        true
                    },
                )
            }
            .pointerInput(state) {
                var lastTapTime = 0L
                var lastTapX = 0f
                var lastTapY = 0f
                awaitEachGesture {
                    val down = awaitFirstDown(requireUnconsumed = false)
                    state.touchStarted()
                    var moved = false
                    var multi = false
                    var travel = 0f
                    try {
                        while (true) {
                            val event = awaitPointerEvent()
                            val pressed = event.changes.count { it.pressed }
                            if (pressed == 0) break
                            if (pressed >= 2) {
                                multi = true
                                moved = true
                                val zoom = event.calculateZoom()
                                val pan = event.calculatePan()
                                val centroid = event.calculateCentroid(useCurrent = true)
                                state.panZoom(pan.x, pan.y, zoom, centroid.x, centroid.y)
                            } else if (!multi) {
                                val change = event.changes.firstOrNull { it.pressed } ?: break
                                val delta = change.positionChange()
                                travel += abs(delta.x) + abs(delta.y)
                                if (travel > touchSlop) moved = true
                                if (moved) state.orbit(delta.x * orbitScale, delta.y * orbitScale)
                            }
                            for (change in event.changes) if (change.pressed && moved) change.consume()
                        }
                    } finally {
                        state.touchEnded()
                    }
                    if (!moved && !multi) {
                        val x = down.position.x
                        val y = down.position.y
                        val now = down.uptimeMillis
                        val double = now - lastTapTime <= doubleTapMillis && abs(x - lastTapX) + abs(y - lastTapY) < near
                        lastTapTime = if (double) 0L else now
                        lastTapX = x
                        lastTapY = y
                        val file = state.pick(x, y, reach, near)
                        if (double) {
                            if (file >= 0) {
                                state.select(file, fly = true)
                            } else {
                                state.fit(true)
                            }
                        } else if (file >= 0) {
                            if (file != state.selected) haptics.selection()
                            state.select(file, fly = false)
                        } else if (state.selected >= 0) {
                            state.clearSelection()
                        }
                    }
                }
            },
    ) {
        state.frame
        drawIntoCanvas { canvas -> renderer.draw(canvas.nativeCanvas, state, state.touching || state.wantsFrames()) }
    }
}

@Composable
private fun BoxScope.SynapseOverlay(state: SynapsePageState) {
    val graph = state.graph
    if (graph != null && graph.nodes.isEmpty() && !state.running) {
        Text(
            "No file imports another file of this repository yet.",
            modifier = Modifier.align(Alignment.Center).padding(FmSpace.x6),
            color = Color(0xFFC8D2E1),
            style = Fm.text.bodyMedium,
        )
    }
    if (state.running) {
        Column(Modifier.align(Alignment.TopStart).padding(FmSpace.x4).width(170.dp)) {
            Text(
                "Growing the brain · ${(state.progress * 100).toInt()}%",
                style = Fm.text.bodySmall,
                color = Color(0xDCC8D6EB),
            )
            Spacer(Modifier.height(FmSpace.x1))
            FmLinearProgressIndicator(
                modifier = Modifier.height(3.dp),
                progress = { state.progress },
                color = Color(0xFF58A6FF),
                trackColor = Color(0x1EFFFFFF),
            )
        }
    }
    Column(
        Modifier.align(Alignment.BottomStart).fillMaxWidth().padding(FmSpace.x3),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x2),
    ) {
        SynapseLegend(state, Modifier)
        val selected = state.selected
        if (selected >= 0) SynapseSelectionCard(state, selected, Modifier.fillMaxWidth())
    }
    Column(
        Modifier.align(Alignment.TopEnd).padding(FmSpace.x2),
        verticalArrangement = Arrangement.spacedBy(FmSpace.x1),
    ) {
        SynapseCanvasButton("view_list", "Overview and lists") {
            state.sheet = SynapseSheet.Overview
        }
        SynapseCanvasButton("fullscreen", "Fit") { state.fit(true) }
        SynapseCanvasButton(
            if (state.autoRotate) "pause" else "rotate_right",
            if (state.autoRotate) "Stop auto-rotate" else "Auto-rotate",
            active = state.autoRotate,
        ) { state.toggleAutoRotate() }
        SynapseCanvasButton("bolt", if (state.pulses) "Turn pulses off" else "Turn pulses on", active = state.pulses) { state.togglePulses() }
    }
}

@Composable
private fun SynapseCanvasButton(icon: String, label: String, active: Boolean = false, onClick: () -> Unit) {
    Box(
        Modifier
            .size(44.dp)
            .clip(CircleShape)
            .background(Color(0xB30D1220))
            .border(1.dp, if (active) Color(0x9958A6FF) else Color(0x33FFFFFF), CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        FmIconButton(icon, label, onPressed = onClick, color = if (active) Color(0xFF8CC8FF) else Color(0xFFC8D2E1))
    }
}

private fun legendRows(state: SynapsePageState): List<Pair<Int, String>> {
    val graph = state.graph ?: return emptyList()
    val rows = ArrayList<Pair<Int, String>>()
    when (state.colourBy) {
        SynapseColourBy.Module -> graph.lobes.indices.sortedByDescending { graph.lobes[it].nodes.size }.take(4)
            .forEach { rows.add(SynapseColours.lobe(it) to SynapseScene.shortLobeName(graph.lobes[it].name)) }
        SynapseColourBy.Language -> graph.nodes.groupingBy { graph.snapshot.files[it.file].language }.eachCount().entries
            .sortedWith(compareByDescending<Map.Entry<String, Int>> { it.value }.thenBy { it.key }).take(4)
            .forEach { rows.add(SynapseColours.language(it.key) to it.key.ifEmpty { "Other" }) }
        SynapseColourBy.Health -> {
            rows.add(SynapseColours.health(10.0) to "Healthy")
            rows.add(SynapseColours.health(5.0) to "Problematic")
            rows.add(SynapseColours.health(1.0) to "Unhealthy")
        }
        SynapseColourBy.Hotspot -> {
            rows.add(SynapseColours.hotspot(1.0) to "Changes often")
            rows.add(SynapseColours.hotspot(0.5) to "Sometimes")
            rows.add(SynapseColours.hotspot(0.0) to "Rarely")
        }
    }
    if (state.selected >= 0) {
        rows.add(SynapseColours.IMPORT to "Imports")
        rows.add(SynapseColours.IMPORTER to "Imported by")
    }
    if (graph.cycles.isNotEmpty()) rows.add(SynapseColours.CYCLE to "Circular dependency")
    return rows
}

@Composable
private fun SynapseLegend(state: SynapsePageState, modifier: Modifier) {
    val rows = remember(state.graph, state.colourBy, state.selected >= 0) { legendRows(state) }
    if (rows.isEmpty()) return
    Column(modifier.widthIn(max = 180.dp), verticalArrangement = Arrangement.spacedBy(2.dp)) {
        for ((colour, text) in rows) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Box(Modifier.size(8.dp).clip(CircleShape).background(Color(colour)))
                Spacer(Modifier.width(FmSpace.x2))
                Text(text, style = Fm.text.labelSmall, color = Color(0xC8C8D2E1), maxLines = 1, overflow = TextOverflow.MiddleEllipsis)
            }
        }
    }
}

@Composable
private fun SynapseSelectionCard(state: SynapsePageState, file: Int, modifier: Modifier) {
    val info = state.snapshot.files[file]
    val shape = RoundedCornerShape(FmRadius.lg)
    Row(
        modifier
            .widthIn(max = 480.dp)
            .clip(shape)
            .background(Color(0xEB0D1220))
            .border(1.dp, Color(0x28FFFFFF), shape)
            .clickable(onClickLabel = "Show details") { state.sheet = SynapseSheet.File }
            .padding(start = FmSpace.x3, top = FmSpace.x1, bottom = FmSpace.x1),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(Synapse.fileName(info.path), style = Fm.text.bodyMedium, color = Color(SynapseColours.TEXT), maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                "Imports ${info.dependsOn.size} · Imported by ${info.dependedOnBy.size}" + if (info.cycle >= 0) " · in a loop" else "",
                style = Fm.text.bodySmall,
                color = if (info.cycle >= 0) Color(SynapseColours.CYCLE) else Color(SynapseColours.MUTED),
                maxLines = 1,
            )
        }
        FmIconButton("close", "Clear the selection", onPressed = { state.clearSelection() }, color = Color(0xFFC8D2E1))
    }
}
