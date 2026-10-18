package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.animation.core.Animatable
import androidx.compose.animation.core.exponentialDecay
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectDragGestures
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.input.pointer.util.VelocityTracker
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.codeinsight.StrataLayout
import com.forkmesh.nativeapp.codeinsight.StrataModel
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.launch
import kotlin.math.ln
import kotlin.math.max
import kotlin.math.min

@Composable
internal fun StrataMatrixView(model: StrataModel, state: StrataPageState) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val density = LocalDensity.current.density
    val graph = model.report.graph
    val order = model.order
    val n = order.size
    val weights = remember(model) {
        val map = HashMap<Long, Int>()
        for (edge in graph.edges) {
            if (edge.from < 0 || edge.to < 0) continue
            val key = edge.from.toLong() * n + edge.to
            map[key] = (map[key] ?: 0) + max(1, edge.weight)
        }
        map
    }
    val maxWeight = remember(weights) { max(1, weights.values.maxOrNull() ?: 1) }
    val layers = remember(model) { StrataLayout.moduleLayers(graph) }
    val measurer = rememberTextMeasurer(cacheSize = 512)
    val labels = remember(model) { HashMap<Pair<Int, Boolean>, TextLayoutResult>() }
    val numbers = remember(model) { HashMap<String, TextLayoutResult>() }
    var size by remember { mutableStateOf(IntSize.Zero) }
    val header = 128f * density
    val cell = if (n == 0) 0f else ((min(size.width, size.height) - header - 12f * density) / n).coerceIn(28f * density, 48f * density)
    val content = header + n * cell + 12f * density
    var sx by remember(model) { mutableFloatStateOf(0f) }
    var sy by remember(model) { mutableFloatStateOf(0f) }
    val maxX = max(0f, content - size.width)
    val maxY = max(0f, content - size.height)
    sx = sx.coerceIn(0f, maxX)
    sy = sy.coerceIn(0f, maxY)
    val scope = rememberCoroutineScope()
    val selectedModule = (state.selection as? StrataFocus.Module)?.index ?: -1
    val selectedCell = state.selection as? StrataFocus.Cell
    val dependent = strataDependent()
    fun weight(a: Int, b: Int): Int = weights[a.toLong() * n + b] ?: 0
    fun hit(p: Offset): Pair<Int, Int> {
        val row = if (p.y >= header) ((p.y - header + sy) / cell).toInt().takeIf { it in 0 until n } ?: -1 else -2
        val column = if (p.x >= header) ((p.x - header + sx) / cell).toInt().takeIf { it in 0 until n } ?: -1 else -2
        return row to column
    }
    val labelStyle = TextStyle(fontSize = 11.sp, color = c.textPrimary)
    val boldStyle = TextStyle(fontSize = 11.sp, color = c.accent, fontWeight = FontWeight.Bold)
    val numberStyle = TextStyle(fontSize = 10.sp, fontWeight = FontWeight.Medium)
    val keyStyle = TextStyle(fontSize = 10.sp, color = c.textSecondary)
    val keys = remember(c.textSecondary) {
        listOf("Row uses column", "Column uses row", "Both ways: cycle", "Numbers count file imports.", "Tap a cell to list them.")
            .map { measurer.measure(it, keyStyle) }
    }
    Canvas(
        Modifier
            .fillMaxSize()
            .background(c.bgBase)
            .onSizeChanged { size = it }
            .semantics {
                contentDescription = "Dependency structure matrix of $n modules in layer order. Drag to scroll, tap a cell to list its imports, " +
                    "tap a module name to select it, double tap it to show it in the graph. The graph's List view and the module details list the same dependencies."
            }
            .pointerInput(model, size) {
                val tracker = VelocityTracker()
                val flingX = Animatable(0f)
                val flingY = Animatable(0f)
                coroutineScope {
                    detectDragGestures(
                        onDragStart = {
                            tracker.resetTracking()
                            launch { flingX.stop() }
                            launch { flingY.stop() }
                        },
                        onDragEnd = {
                            val velocity = tracker.calculateVelocity()
                            launch {
                                flingX.snapTo(sx)
                                flingX.animateDecay(-velocity.x, exponentialDecay()) { sx = value.coerceIn(0f, maxX) }
                            }
                            launch {
                                flingY.snapTo(sy)
                                flingY.animateDecay(-velocity.y, exponentialDecay()) { sy = value.coerceIn(0f, maxY) }
                            }
                        },
                    ) { change, drag ->
                        tracker.addPosition(change.uptimeMillis, change.position)
                        change.consume()
                        sx = (sx - drag.x).coerceIn(0f, maxX)
                        sy = (sy - drag.y).coerceIn(0f, maxY)
                    }
                }
            }
            .pointerInput(model, size) {
                detectTapGestures(
                    onTap = { p ->
                        val (row, column) = hit(p)
                        when {
                            row >= 0 && column >= 0 -> {
                                val a = order[row]
                                val b = order[column]
                                if (a == b || weight(a, b) > 0 || weight(b, a) > 0) {
                                    haptics.selection()
                                    state.select(StrataFocus.Cell(a, b))
                                }
                            }
                            row >= 0 && column == -2 -> {
                                haptics.selection()
                                state.select(StrataFocus.Module(order[row]))
                            }
                            row == -2 && column >= 0 -> {
                                haptics.selection()
                                state.select(StrataFocus.Module(order[column]))
                            }
                        }
                    },
                    onDoubleTap = { p ->
                        val (row, column) = hit(p)
                        val module = when {
                            row >= 0 && column == -2 -> order[row]
                            row == -2 && column >= 0 -> order[column]
                            else -> -1
                        }
                        if (module >= 0) {
                            scope.launch {
                                state.drillOut()
                                state.view = StrataView.Graph
                                state.select(StrataFocus.Module(module), open = false)
                            }
                        }
                    },
                )
            },
    ) {
        if (n == 0) return@Canvas
        val ox = header - sx
        val oy = header - sy
        val c0 = max(0, (sx / cell).toInt())
        val c1 = min(n - 1, ((sx + this.size.width - header) / cell).toInt() + 1)
        val r0 = max(0, (sy / cell).toInt())
        val r1 = min(n - 1, ((sy + this.size.height - header) / cell).toInt() + 1)
        val logMax = ln(1.0 + maxWeight)
        val selectedRow = if (selectedModule >= 0) order.indexOf(selectedModule) else -1
        val cellRow = if (selectedCell != null) order.indexOf(selectedCell.from) else -1
        val cellColumn = if (selectedCell != null) order.indexOf(selectedCell.to) else -1
        clipRect(header, header, this.size.width, this.size.height) {
            drawRect(c.bgRaised, Offset(ox, oy), Size(n * cell, n * cell))
            if (selectedRow >= 0) {
                drawRect(c.accent.copy(alpha = .10f), Offset(ox, oy + selectedRow * cell), Size(n * cell, cell))
                drawRect(c.accent.copy(alpha = .10f), Offset(ox + selectedRow * cell, oy), Size(cell, n * cell))
            }
            for (r in r0..r1) for (col in c0..c1) {
                val x = ox + col * cell
                val y = oy + r * cell
                if (r == col) {
                    drawRect(c.bgOverlay, Offset(x + 1, y + 1), Size(cell - 2, cell - 2))
                    continue
                }
                val a = order[r]
                val b = order[col]
                val uses = weight(a, b)
                val usedBy = weight(b, a)
                if (uses == 0 && usedBy == 0) continue
                val color = if (uses != 0 && usedBy != 0) StrataColors.unhealthy else if (uses != 0) c.accent else dependent
                val shown = if (uses != 0) uses else usedBy
                val t = if (logMax > 0) ln(1.0 + shown) / logMax else 1.0
                drawRoundRect(
                    color.copy(alpha = (0.28 + 0.62 * t).toFloat()),
                    Offset(x + 1.5f * density, y + 1.5f * density),
                    Size(cell - 3f * density, cell - 3f * density),
                    CornerRadius(3f * density, 3f * density),
                )
                val text = if (shown >= 1000) "${shown / 1000}k" else shown.toString()
                val layout = numbers.getOrPut(text) { measurer.measure(text, numberStyle) }
                drawText(
                    layout,
                    color = if (t > 0.45) StrataColors.readableOn(color) else c.textPrimary,
                    topLeft = Offset(x + (cell - layout.size.width) / 2f, y + (cell - layout.size.height) / 2f),
                )
            }
            val grid = c.border.copy(alpha = .45f)
            for (i in 0..n) {
                drawLine(grid, Offset(ox, oy + i * cell), Offset(ox + n * cell, oy + i * cell), 1f)
                drawLine(grid, Offset(ox + i * cell, oy), Offset(ox + i * cell, oy + n * cell), 1f)
            }
            val layerPen = c.textTertiary.copy(alpha = .6f)
            for (i in 1 until n) {
                if (layers[order[i]] != layers[order[i - 1]]) {
                    drawLine(layerPen, Offset(ox, oy + i * cell), Offset(ox + n * cell, oy + i * cell), 1.4f * density)
                    drawLine(layerPen, Offset(ox + i * cell, oy), Offset(ox + i * cell, oy + n * cell), 1.4f * density)
                }
            }
            var i = 0
            while (i < n) {
                val cycle = graph.nodes[order[i]].cycle
                var j = i + 1
                while (cycle >= 0 && j < n && graph.nodes[order[j]].cycle == cycle) j++
                if (cycle >= 0 && j - i > 1) {
                    drawRoundRect(
                        StrataColors.unhealthy,
                        Offset(ox + i * cell + 1, oy + i * cell + 1),
                        Size((j - i) * cell - 2, (j - i) * cell - 2),
                        CornerRadius(4f * density, 4f * density),
                        style = Stroke(width = 2f * density),
                    )
                }
                i = j
            }
            if (cellRow >= 0 && cellColumn >= 0) {
                drawRoundRect(
                    c.accent,
                    Offset(ox + cellColumn * cell + 1, oy + cellRow * cell + 1),
                    Size(cell - 2, cell - 2),
                    CornerRadius(3f * density, 3f * density),
                    style = Stroke(width = 2.4f * density),
                )
            }
        }
        clipRect(0f, header, header, this.size.height) {
            drawRect(c.bgBase, Offset(0f, header), Size(header, this.size.height - header))
            for (r in r0..r1) {
                val m = order[r]
                val y = oy + r * cell
                if (m == selectedModule || r == cellRow) drawRect(c.accent.copy(alpha = if (m == selectedModule) .18f else .10f), Offset(0f, y), Size(header, cell))
                if (graph.nodes[m].cycle >= 0) drawCircle(StrataColors.unhealthy, 3.5f * density, Offset(8f * density, y + cell / 2f))
                val label = labels.getOrPut(m to (m == selectedModule)) {
                    measurer.measure(graph.nodes[m].name, if (m == selectedModule) boldStyle else labelStyle, softWrap = false, maxLines = 1)
                }
                clipRect(16f * density, y, header - 6f * density, y + cell) {
                    drawText(label, topLeft = Offset(max(16f * density, header - 6f * density - label.size.width), y + (cell - label.size.height) / 2f))
                }
            }
        }
        clipRect(header, 0f, this.size.width, header) {
            drawRect(c.bgBase, Offset(header, 0f), Size(this.size.width - header, header))
            for (col in c0..c1) {
                val m = order[col]
                val x = ox + col * cell
                if (m == selectedModule || col == cellColumn) drawRect(c.accent.copy(alpha = if (m == selectedModule) .18f else .10f), Offset(x, 0f), Size(cell, header))
                if (graph.nodes[m].cycle >= 0) drawCircle(StrataColors.unhealthy, 3.5f * density, Offset(x + cell / 2f, header - 8f * density))
                val label = labels.getOrPut(m to (m == selectedModule)) {
                    measurer.measure(graph.nodes[m].name, if (m == selectedModule) boldStyle else labelStyle, softWrap = false, maxLines = 1)
                }
                val pivot = Offset(x + cell / 2f, header - 16f * density)
                rotate(-90f, pivot) {
                    clipRect(pivot.x, pivot.y - cell / 2f, pivot.x + header - 22f * density, pivot.y + cell / 2f) {
                        drawText(label, topLeft = Offset(pivot.x, pivot.y - label.size.height / 2f))
                    }
                }
            }
        }
        drawRect(c.bgBase, Offset.Zero, Size(header, header))
        val swatches = listOf(c.accent, dependent, StrataColors.unhealthy)
        for ((k, layout) in keys.withIndex()) {
            val y = 10f * density + k * 18f * density
            if (k < 3) {
                drawRoundRect(swatches[k], Offset(8f * density, y + 2f * density), Size(10f * density, 10f * density), CornerRadius(2f * density, 2f * density))
            }
            drawText(layout, topLeft = Offset(if (k < 3) 22f * density else 8f * density, y), color = Color.Unspecified)
        }
    }
}
