package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.foundation.Canvas
import com.forkmesh.nativeapp.ui.theme.FmType
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.input.pointer.pointerInput
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
import com.forkmesh.nativeapp.codeinsight.Strata
import com.forkmesh.nativeapp.codeinsight.StrataLayout
import com.forkmesh.nativeapp.codeinsight.StrataModel
import com.forkmesh.nativeapp.codeinsight.StrataRect
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlin.math.max
import kotlin.math.sqrt

internal fun strataTreemapValue(model: StrataModel, file: Int, by: StrataTreemapColor, maxCommits: Double): Double {
    val info = model.snapshot.files[file]
    return when (by) {
        StrataTreemapColor.Health -> (10.0 - info.health.coerceIn(1.0, 10.0)) / 9.0
        StrataTreemapColor.MaxComplexity -> ((info.maxComplexity - 4) / 26.0).coerceIn(0.0, 1.0)
        StrataTreemapColor.Debt -> (model.report.files.getOrNull(file)?.ratio ?: 0.0).div(0.5).coerceIn(0.0, 1.0)
        StrataTreemapColor.ChangeFrequency -> sqrt(info.commits / maxCommits)
    }
}

internal fun strataTreemapColor(t: Double, by: StrataTreemapColor, dark: Boolean): Color =
    if (by == StrataTreemapColor.ChangeFrequency) StrataColors.change(t, dark) else StrataColors.scale(t)

@Composable
internal fun StrataTreemapView(model: StrataModel, state: StrataPageState) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val density = LocalDensity.current.density
    val files = model.snapshot.files
    val graph = model.report.graph
    var size by remember { mutableStateOf(IntSize.Zero) }
    val sizeBy = state.treemapSize
    val colorBy = state.treemapColor
    val map = remember(model, sizeBy, size) {
        if (size.width <= 0 || size.height <= 0) {
            null
        } else {
            StrataLayout.treemap(
                model.snapshot, graph, sizeBy,
                StrataRect(0.0, 0.0, size.width.toDouble(), size.height.toDouble()),
                headerHeight = 16.0 * density,
                headerMinHeight = 40.0 * density,
                headerMinWidth = 64.0 * density,
                inset = 1.5 * density,
            )
        }
    }
    val maxCommits = remember(model) { max(1.0, files.maxOfOrNull { it.commits.toDouble() } ?: 1.0) }
    val colors = remember(model, colorBy, c.dark) {
        files.indices.map { strataTreemapColor(strataTreemapValue(model, it, colorBy, maxCommits), colorBy, c.dark) }
    }
    var scale by remember(model, sizeBy) { mutableFloatStateOf(1f) }
    var offset by remember(model, sizeBy) { mutableStateOf(Offset.Zero) }
    val measurer = rememberTextMeasurer(cacheSize = 512)
    val fileLabels = remember(model) { HashMap<Int, TextLayoutResult>() }
    val moduleLabels = remember(model) { HashMap<Int, TextLayoutResult>() }
    val fileStyle = TextStyle(fontFamily = FmType.ui, fontSize = 10.sp)
    val moduleStyle = TextStyle(fontFamily = FmType.ui, fontSize = 11.sp, fontWeight = FontWeight.SemiBold, color = c.textPrimary)
    val selectedFile = (state.selection as? StrataFocus.File)?.index ?: -1
    val selectedModule = (state.selection as? StrataFocus.Module)?.index ?: -1
    fun toWorld(p: Offset) = Offset((p.x - offset.x) / scale, (p.y - offset.y) / scale)
    Column(Modifier.fillMaxSize()) {
        Canvas(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .padding(horizontal = FmSpace.x2)
                .background(c.bgBase)
                .onSizeChanged { size = it }
                .semantics {
                    contentDescription = "Treemap of ${files.size} files in ${graph.nodes.size} modules, sized by ${sizeBy.label} and coloured by ${colorBy.label}. " +
                        "Pinch to zoom, tap a file for its details. Find files by name with the search button."
                }
                .pointerInput(model, sizeBy) {
                    detectTransformGestures { centroid, pan, zoom, _ ->
                        val next = (scale * zoom).coerceIn(1f, 12f)
                        val ratio = next / scale
                        offset = centroid - (centroid - offset) * ratio + pan
                        scale = next
                        if (scale == 1f) offset = Offset.Zero
                    }
                }
                .pointerInput(model, sizeBy, map) {
                    detectTapGestures(
                        onTap = { p ->
                            val layout = map ?: return@detectTapGestures
                            val w = toWorld(p)
                            val module = layout.modules.indexOfFirst { it.contains(w.x.toDouble(), w.y.toDouble()) }
                            if (module < 0) {
                                state.select(null)
                                return@detectTapGestures
                            }
                            val file = graph.nodes[module].files.firstOrNull { layout.files[it].contains(w.x.toDouble(), w.y.toDouble()) && !layout.files[it].isEmpty }
                            haptics.selection()
                            state.select(if (file != null) StrataFocus.File(file) else StrataFocus.Module(module))
                        },
                        onDoubleTap = {
                            scale = 1f
                            offset = Offset.Zero
                        },
                    )
                },
        ) {
            val layout = map ?: return@Canvas
            withTransform({
                translate(offset.x, offset.y)
                scale(scale, scale, Offset.Zero)
            }) {
                for ((m, node) in graph.nodes.withIndex()) {
                    val box = layout.modules[m]
                    if (box.width < 2 || box.height < 2) continue
                    drawRect(c.bgRaised, Offset(box.x.toFloat(), box.y.toFloat()), Size(box.width.toFloat(), box.height.toFloat()))
                    for (f in node.files) {
                        val cell = layout.files[f]
                        if (cell.width < 0.6 || cell.height < 0.6) continue
                        val inset = if (cell.width > 3 && cell.height > 3) 0.5f else 0f
                        drawRect(
                            colors[f],
                            Offset(cell.x.toFloat() + inset, cell.y.toFloat() + inset),
                            Size(cell.width.toFloat() - 2 * inset, cell.height.toFloat() - 2 * inset),
                        )
                    }
                    drawRect(c.bgBase, Offset(box.x.toFloat(), box.y.toFloat()), Size(box.width.toFloat(), box.height.toFloat()), style = Stroke(2f / scale))
                }
                if (selectedModule in graph.nodes.indices) {
                    val box = layout.modules[selectedModule]
                    drawRect(c.accent, Offset(box.x.toFloat(), box.y.toFloat()), Size(box.width.toFloat(), box.height.toFloat()), style = Stroke(3f / scale))
                }
                if (selectedFile in files.indices) {
                    val cell = layout.files[selectedFile]
                    drawRect(c.accent, Offset(cell.x.toFloat(), cell.y.toFloat()), Size(cell.width.toFloat(), cell.height.toFloat()), style = Stroke(3f / scale))
                }
            }
            for ((m, node) in graph.nodes.withIndex()) {
                val box = layout.modules[m]
                val left = offset.x + box.x.toFloat() * scale
                val top = offset.y + box.y.toFloat() * scale
                val width = box.width.toFloat() * scale
                val height = box.height.toFloat() * scale
                if (left > size.width || top > size.height || left + width < 0 || top + height < 0) continue
                if (height > 40f * density && width > 64f * density) {
                    val label = moduleLabels.getOrPut(m) { measurer.measure(node.name, moduleStyle, softWrap = false, maxLines = 1) }
                    clipRect(left + 4f, top, left + width - 4f, top + 16f * density * scale) {
                        drawText(label, topLeft = Offset(left + 5f * density, top + (16f * density * scale - label.size.height) / 2f))
                    }
                }
                for (f in node.files) {
                    val cell = layout.files[f]
                    val cw = cell.width.toFloat() * scale
                    val ch = cell.height.toFloat() * scale
                    if (cw < 64f * density || ch < 18f * density) continue
                    val cx = offset.x + cell.x.toFloat() * scale
                    val cy = offset.y + cell.y.toFloat() * scale
                    val label = fileLabels.getOrPut(f) { measurer.measure(Strata.fileName(files[f].path), fileStyle, softWrap = false, maxLines = 1) }
                    clipRect(cx + 3f, cy, cx + cw - 3f, cy + ch) {
                        drawText(label, color = StrataColors.readableOn(colors[f]).copy(alpha = .92f), topLeft = Offset(cx + 4f * density, cy + 2f * density))
                    }
                }
            }
        }
        StrataLegendRow {
            val high = when (colorBy) {
                StrataTreemapColor.Health -> "Unhealthy"
                StrataTreemapColor.MaxComplexity -> "30+"
                StrataTreemapColor.Debt -> "50 %+"
                StrataTreemapColor.ChangeFrequency -> "${maxCommits.toInt()} commits"
            }
            StrataGradientKey(colorBy.low, high, (0..10).map { strataTreemapColor(it / 10.0, colorBy, c.dark) }, colorBy.label)
            Text("Size: ${sizeBy.label}", style = Fm.text.labelSmall, color = c.textSecondary)
        }
    }
}
