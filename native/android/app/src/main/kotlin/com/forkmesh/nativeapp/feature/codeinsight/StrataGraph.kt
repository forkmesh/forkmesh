package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.animation.core.Animatable
import com.forkmesh.nativeapp.ui.theme.FmType
import androidx.compose.animation.core.tween
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.draw.clip
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.RoundRect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipPath
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.withTransform
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextLayoutResult
import androidx.compose.ui.text.TextMeasurer
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.IntSize
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.codeinsight.Strata
import com.forkmesh.nativeapp.codeinsight.StrataDrillGraph
import com.forkmesh.nativeapp.codeinsight.StrataGraphLayout
import com.forkmesh.nativeapp.codeinsight.StrataLayout
import com.forkmesh.nativeapp.codeinsight.StrataModel
import com.forkmesh.nativeapp.codeinsight.StrataPoint
import com.forkmesh.nativeapp.codeinsight.StrataRect
import com.forkmesh.nativeapp.ui.kit.FmListRow
import com.forkmesh.nativeapp.ui.kit.FmSectionHeader
import com.forkmesh.nativeapp.ui.kit.FmToneButton
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmRadius
import com.forkmesh.nativeapp.ui.theme.FmSpace
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min

internal class StrataSceneItem(
    val rect: StrataRect,
    val module: Boolean,
    val external: Boolean,
    val dependent: Boolean,
    val index: Int,
    val title: String,
    val caption: String,
    val cycle: Boolean,
    val cells: List<Pair<StrataRect, Color>>,
)

internal class StrataSceneLink(
    val from: Int,
    val to: Int,
    val cycle: Boolean,
    val width: Double,
    val path: Path,
    val arrow: Path?,
)

internal class StrataScene(
    val items: List<StrataSceneItem>,
    val links: List<StrataSceneLink>,
    val bounds: StrataRect,
    val outgoing: List<Set<Int>>,
    val incoming: List<Set<Int>>,
)

private fun edgePath(points: List<StrataPoint>, flat: Boolean): Path {
    val path = Path()
    if (points.size < 2) return path
    path.moveTo(points[0].x.toFloat(), points[0].y.toFloat())
    if (flat && points.size == 3) {
        val s = points[0]
        val m = points[1]
        val e = points[2]
        path.cubicTo(
            s.x.toFloat(), (s.y + (m.y - s.y) * 1.33).toFloat(),
            e.x.toFloat(), (e.y + (m.y - e.y) * 1.33).toFloat(),
            e.x.toFloat(), e.y.toFloat(),
        )
        return path
    }
    for (i in 0 until points.size - 1) {
        val a = points[i]
        val b = points[i + 1]
        if (abs(a.x - b.x) < 0.5) {
            path.lineTo(b.x.toFloat(), b.y.toFloat())
            continue
        }
        val dy = (b.y - a.y) * 0.5
        path.cubicTo(a.x.toFloat(), (a.y + dy).toFloat(), b.x.toFloat(), (b.y - dy).toFloat(), b.x.toFloat(), b.y.toFloat())
    }
    return path
}

private fun sceneLinks(layout: StrataGraphLayout, count: Int): Triple<List<StrataSceneLink>, List<Set<Int>>, List<Set<Int>>> {
    val outgoing = List(count) { HashSet<Int>() }
    val incoming = List(count) { HashSet<Int>() }
    val links = layout.edges.map { edge ->
        if (edge.from in 0 until count && edge.to in 0 until count) {
            outgoing[edge.from].add(edge.to)
            incoming[edge.to].add(edge.from)
        }
        val width = StrataLayout.edgeWidth(edge.weight)
        val points = edge.points.toMutableList()
        var arrow: Path? = null
        if (points.size >= 2) {
            val tip = points.last()
            val before = points[points.size - 2]
            val direction = if (tip.y >= before.y) 1.0 else -1.0
            val length = 8.0 + width * 1.4
            val half = 4.0 + width * 0.9
            arrow = Path().apply {
                moveTo(tip.x.toFloat(), tip.y.toFloat())
                lineTo((tip.x - half).toFloat(), (tip.y - direction * length).toFloat())
                lineTo((tip.x + half).toFloat(), (tip.y - direction * length).toFloat())
                close()
            }
            points[points.size - 1] = StrataPoint(tip.x, tip.y - direction * length * 0.8)
        }
        StrataSceneLink(edge.from, edge.to, edge.cycle, width, edgePath(points, edge.flat), arrow)
    }
    return Triple(links, outgoing, incoming)
}

internal fun moduleScene(model: StrataModel): StrataScene {
    val report = model.report
    val files = model.snapshot.files
    val layout = model.layout
    val items = report.graph.nodes.mapIndexed { i, node ->
        val rect = layout.boxes.getOrElse(i) { StrataRect() }
        val ranked = node.files.sortedByDescending { files[it].codeLines }.take(80)
        val inner = rect.adjusted(7.0, 42.0, -7.0, -7.0)
        val cells = if (inner.height > 6 && inner.width > 6) {
            val rects = StrataLayout.squarify(ranked.map { max(1, files[it].codeLines).toDouble() }, inner)
            rects.indices.filter { !rects[it].isEmpty }.map { rects[it] to StrataColors.health(files[ranked[it]].health) }
        } else {
            emptyList()
        }
        StrataSceneItem(
            rect = rect,
            module = true,
            external = false,
            dependent = false,
            index = i,
            title = node.name.ifEmpty { node.path },
            caption = "${Strata.number(node.codeLines)} LOC · ${node.files.size} ${if (node.files.size == 1) "file" else "files"}",
            cycle = node.cycle >= 0,
            cells = cells,
        )
    }
    val (links, outgoing, incoming) = sceneLinks(layout, items.size)
    return StrataScene(items, links, layout.bounds, outgoing, incoming)
}

internal fun drillScene(model: StrataModel, drill: StrataDrillGraph): StrataScene {
    val report = model.report
    val files = model.snapshot.files
    val layout = drill.layout
    val ownCycle = report.graph.nodes.getOrNull(drill.module)?.cycle ?: -1
    val items = drill.nodes.mapIndexed { k, node ->
        val rect = layout.boxes.getOrElse(k) { StrataRect() }
        if (node.module) {
            val mod = report.graph.nodes[node.index]
            StrataSceneItem(
                rect, true, true, node.dependent, node.index, mod.name.ifEmpty { mod.path },
                if (node.dependent) "uses these files · ${node.weight}" else "used · ${node.weight}",
                mod.cycle >= 0 && mod.cycle == ownCycle, emptyList(),
            )
        } else {
            val file = files[node.index]
            StrataSceneItem(
                rect, false, false, false, node.index, Strata.fileName(file.path),
                "${Strata.number(file.codeLines)} LOC · max cc ${file.maxComplexity}", false, emptyList(),
            )
        }
    }
    val (links, outgoing, incoming) = sceneLinks(layout, items.size)
    return StrataScene(items, links, layout.bounds, outgoing, incoming)
}

private fun moduleComplexityT(model: StrataModel, module: Int): Double {
    val node = model.report.graph.nodes[module]
    val average = if (node.functions > 0) node.complexity.toDouble() / node.functions else 1.0
    return ((average - 1.5) / 8.0).coerceIn(0.0, 1.0)
}

internal fun strataModuleColor(model: StrataModel, module: Int, by: StrataGraphColor): Color {
    val node = model.report.graph.nodes[module]
    return when (by) {
        StrataGraphColor.Rating -> StrataColors.rating(model.report.modules.getOrNull(module)?.rating ?: 0)
        StrataGraphColor.Health -> StrataColors.health(node.health)
        StrataGraphColor.Instability -> StrataColors.instability(node.instability)
        StrataGraphColor.Complexity -> StrataColors.scale(moduleComplexityT(model, module))
    }
}

internal fun strataFileColor(model: StrataModel, file: Int, by: StrataGraphColor): Color {
    val info = model.snapshot.files[file]
    return when (by) {
        StrataGraphColor.Rating -> StrataColors.rating(model.report.files.getOrNull(file)?.rating ?: 0)
        StrataGraphColor.Health -> StrataColors.health(info.health)
        StrataGraphColor.Instability -> {
            val sum = info.dependsOn.size + info.dependedOnBy.size
            StrataColors.instability(if (sum != 0) info.dependsOn.size.toDouble() / sum else 0.0)
        }
        StrataGraphColor.Complexity -> StrataColors.scale(((info.maxComplexity - 4) / 22.0).coerceIn(0.0, 1.0))
    }
}

private class StrataViewport {
    var scale by mutableFloatStateOf(1f)
    var offset by mutableStateOf(Offset.Zero)
    var fitted by mutableStateOf(false)

    fun fit(bounds: StrataRect, size: IntSize) {
        if (size.width <= 0 || size.height <= 0) return
        val bw = max(1.0, bounds.width)
        val bh = max(1.0, bounds.height)
        val s = (min(size.width / bw, size.height / bh) * 0.92).coerceIn(0.03, 2.5).toFloat()
        scale = s
        offset = Offset(
            (size.width - bw.toFloat() * s) / 2f - bounds.x.toFloat() * s,
            (size.height - bh.toFloat() * s) / 2f - bounds.y.toFloat() * s,
        )
        fitted = true
    }

    fun toWorld(p: Offset): Offset = Offset((p.x - offset.x) / scale, (p.y - offset.y) / scale)
}

@Composable
internal fun StrataGraphView(model: StrataModel, state: StrataPageState, context: CodeInsightContext) {
    val drilled = state.drilled
    val drill = state.drill
    val scene = remember(model, drill, drilled) {
        if (drilled >= 0 && drill != null && drill.module == drilled) drillScene(model, drill) else if (drilled < 0) moduleScene(model) else null
    }
    Column(Modifier.fillMaxSize()) {
        StrataBreadcrumb(model, state)
        Box(Modifier.weight(1f).fillMaxWidth()) {
            when {
                scene == null -> Unit
                state.graphList -> StrataGraphList(model, state, context)
                else -> StrataGraphCanvas(model, state, context, scene)
            }
        }
        if (!state.graphList) StrataGraphLegend(state)
    }
}

@Composable
private fun StrataBreadcrumb(model: StrataModel, state: StrataPageState) {
    val c = Fm.colors
    val graph = model.report.graph
    Row(
        Modifier.fillMaxWidth().padding(horizontal = FmSpace.x4, vertical = FmSpace.x1).heightIn(min = 32.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        if (state.drilled < 0) {
            Text(
                "${graph.nodes.size} modules · ${graph.edges.size} dependencies · depth ${graph.depth}",
                style = Fm.text.bodySmall,
                color = c.textSecondary,
            )
        } else {
            FmToneButton("All modules", onPressed = { state.drillOut() }, icon = "arrow_back", tooltip = "Back to all modules", height = 30.dp)
            Spacer(Modifier.width(FmSpace.x2))
            Text("›", color = c.textTertiary)
            Spacer(Modifier.width(FmSpace.x2))
            val node = graph.nodes.getOrNull(state.drilled)
            Text(
                node?.path?.ifEmpty { node.name } ?: "",
                style = Fm.text.bodySmall,
                color = c.textPrimary,
                fontWeight = FontWeight.SemiBold,
                maxLines = 1,
                overflow = TextOverflow.MiddleEllipsis,
                modifier = Modifier.weight(1f),
            )
            val hidden = state.drill?.hiddenFiles ?: 0
            if (hidden > 0) Text("$hidden more files", style = Fm.text.labelSmall, color = c.textTertiary)
        }
    }
}

@Composable
private fun StrataGraphLegend(state: StrataPageState) {
    StrataLegendRow {
        when (state.graphColor) {
            StrataGraphColor.Rating -> Row(verticalAlignment = Alignment.CenterVertically) {
                Text("Debt rating", style = Fm.text.labelSmall, color = Fm.colors.textPrimary)
                for (r in 0 until 5) {
                    Spacer(Modifier.width(4.dp))
                    StrataRatingChip(r, 18.dp)
                }
            }
            StrataGraphColor.Health -> StrataGradientKey("Unhealthy", "Healthy", (0..10).map { StrataColors.health(1 + 0.9 * it) }, "Health")
            StrataGraphColor.Instability -> StrataGradientKey("Stable", "Unstable", (0..10).map { StrataColors.instability(it / 10.0) }, "Instability")
            StrataGraphColor.Complexity -> StrataGradientKey("Simple", "Complex", (0..10).map { StrataColors.scale(it / 10.0) }, "Complexity")
        }
        StrataLegendKey(Fm.colors.accent, "uses")
        StrataLegendKey(strataDependent(), "used by")
        StrataLegendKey(StrataColors.unhealthy, "cycle")
    }
}

private fun focusItem(scene: StrataScene, selection: StrataFocus?): Int = when (selection) {
    is StrataFocus.Module -> scene.items.indexOfFirst { it.module && it.index == selection.index }
    is StrataFocus.File -> scene.items.indexOfFirst { !it.module && it.index == selection.index }
    else -> -1
}

@Composable
private fun StrataGraphCanvas(model: StrataModel, state: StrataPageState, context: CodeInsightContext, scene: StrataScene) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val density = LocalDensity.current.density
    val measurer = rememberTextMeasurer(cacheSize = 512)
    val viewport = remember(scene) { StrataViewport() }
    var size by remember { mutableStateOf(IntSize.Zero) }
    LaunchedEffect(scene, size) { if (!viewport.fitted) viewport.fit(scene.bounds, size) }
    LaunchedEffect(state.fitRequest) { if (state.fitRequest > 0) viewport.fit(scene.bounds, size) }
    val focus = focusItem(scene, state.selection)
    val highlight = remember(scene) { Animatable(0f) }
    val reduced = Fm.reducedMotion
    LaunchedEffect(focus) {
        if (focus < 0 || reduced) highlight.snapTo(if (focus < 0) 0f else 1f) else {
            highlight.snapTo(0f)
            highlight.animateTo(1f, tween(320))
        }
    }
    val dependent = strataDependent()
    val colorBy = state.graphColor
    val fills = remember(scene, colorBy) {
        scene.items.map { if (it.module) strataModuleColor(model, it.index, colorBy) else strataFileColor(model, it.index, colorBy) }
    }
    val titleStyle = TextStyle(fontFamily = FmType.ui, fontSize = 12.sp, fontWeight = FontWeight.SemiBold)
    val captionStyle = TextStyle(fontFamily = FmType.ui, fontSize = 10.sp)
    val titles = remember(scene) { HashMap<Int, TextLayoutResult>() }
    val captions = remember(scene) { HashMap<Int, TextLayoutResult>() }
    fun hit(p: Offset): Int {
        val w = viewport.toWorld(p)
        for (i in scene.items.indices.reversed()) {
            if (scene.items[i].rect.contains(w.x.toDouble(), w.y.toDouble())) return i
        }
        return -1
    }
    val graphName = if (state.drilled >= 0) "Files of ${model.report.graph.nodes[state.drilled].path}" else "Dependency graph"
    Box(Modifier.fillMaxSize()) {
        Canvas(
            Modifier
                .fillMaxSize()
                .background(c.bgBase)
                .onSizeChanged { size = it }
                .semantics {
                    contentDescription = "$graphName: ${scene.items.size} boxes and ${scene.links.size} dependencies. " +
                        "Pinch to zoom, drag to pan, tap to select, double tap to " +
                        (if (state.drilled < 0) "see a module's files" else "open a file") + ". The List button shows the same facts as a list."
                }
                .pointerInput(scene) {
                    detectTransformGestures { centroid, pan, zoom, _ ->
                        val next = (viewport.scale * zoom).coerceIn(0.02f, 6f)
                        val ratio = next / viewport.scale
                        viewport.offset = centroid - (centroid - viewport.offset) * ratio + pan
                        viewport.scale = next
                    }
                }
                .pointerInput(scene, state.drilled) {
                    detectTapGestures(
                        onTap = { p ->
                            val i = hit(p)
                            if (i < 0) {
                                state.select(null)
                            } else {
                                haptics.selection()
                                val item = scene.items[i]
                                state.select(if (item.module) StrataFocus.Module(item.index) else StrataFocus.File(item.index), open = false)
                            }
                        },
                        onDoubleTap = { p ->
                            val i = hit(p)
                            if (i >= 0) {
                                val item = scene.items[i]
                                haptics.tap()
                                when {
                                    !item.module -> context.openFile(model.snapshot.files[item.index].path, 0)
                                    item.external -> {
                                        state.drillOut()
                                        state.select(StrataFocus.Module(item.index), open = false)
                                    }
                                    else -> {
                                        state.drillInto(item.index)
                                        state.select(StrataFocus.Module(item.index), open = false)
                                    }
                                }
                            }
                        },
                    )
                },
        ) {
            val scale = viewport.scale
            val offset = viewport.offset
            val h = highlight.value
            val related = HashSet<Int>()
            if (focus >= 0) {
                related.add(focus)
                related.addAll(scene.outgoing[focus])
                related.addAll(scene.incoming[focus])
            }
            val plain = if (c.dark) Color(0x698B949E) else Color(0x5F57606A)
            val cycleColor = StrataColors.unhealthy.copy(alpha = .85f)
            val visible = StrataRect(
                (-offset.x / scale - 40).toDouble(), (-offset.y / scale - 40).toDouble(),
                (this.size.width / scale + 80).toDouble(), (this.size.height / scale + 80).toDouble(),
            )
            withTransform({
                translate(offset.x, offset.y)
                scale(scale, scale, Offset.Zero)
            }) {
                for (pass in 0..1) {
                    for (link in scene.links) {
                        val isFocus = focus >= 0 && (link.from == focus || link.to == focus)
                        if (pass == 0 && (link.cycle || isFocus)) continue
                        if (pass == 1 && !(link.cycle || isFocus)) continue
                        var color = if (link.cycle) cycleColor else plain
                        var boost = 0.0
                        if (focus >= 0) {
                            if (isFocus) {
                                val target = if (link.from == focus) c.accent else dependent
                                color = StrataColors.mix(color, target, h.toDouble())
                                boost = 1.2 * h
                            } else {
                                color = color.copy(alpha = color.alpha * (1f - 0.8f * h))
                            }
                        }
                        val screenWidth = max(0.8, (link.width + boost) * scale.toDouble().coerceIn(0.35, 1.6)) * density / 1.5
                        drawPath(link.path, color, style = Stroke(width = (screenWidth / scale).toFloat(), cap = StrokeCap.Round))
                        if (scale * 8 > 2.5 && link.arrow != null) drawPath(link.arrow, color)
                    }
                }
                for ((i, item) in scene.items.withIndex()) {
                    if (!item.rect.intersects(visible)) continue
                    val alpha = if (focus < 0 || i in related) 1f else 1f - 0.75f * h
                    drawSceneItem(item, fills[i], scale, alpha, i == focus, c.dark, c.bgRaised, c.border, c.accent, dependent, density)
                }
            }
            for ((i, item) in scene.items.withIndex()) {
                if (!item.rect.intersects(visible)) continue
                val alpha = if (focus < 0 || i in related) 1f else 1f - 0.75f * h
                drawSceneLabels(
                    measurer, item, i, fills[i], scale, offset, alpha, density, titles, captions, titleStyle, captionStyle,
                    c.textPrimary, c.textSecondary,
                )
            }
        }
        val selected = state.selection
        if (focus >= 0 && selected != null) {
            StrataSelectionBar(model, selected, Modifier.align(Alignment.BottomCenter), onDetails = { state.sheetOpen = true }, onDrill = if (selected is StrataFocus.Module && state.drilled < 0) {
                { state.drillInto(selected.index) }
            } else {
                null
            })
        }
    }
}

private fun DrawScope.drawSceneItem(
    item: StrataSceneItem,
    fill: Color,
    scale: Float,
    alpha: Float,
    focused: Boolean,
    dark: Boolean,
    surface: Color,
    borderColor: Color,
    accent: Color,
    dependent: Color,
    density: Float,
) {
    val r = item.rect
    val topLeft = Offset(r.x.toFloat(), r.y.toFloat())
    val boxSize = Size(r.width.toFloat(), r.height.toFloat())
    val px = density / scale
    if (item.external) {
        val radius = CornerRadius(boxSize.height / 2f, boxSize.height / 2f)
        drawRoundRect(surface.copy(alpha = alpha), topLeft, boxSize, radius)
        drawRoundRect(
            (if (item.dependent) dependent else accent).copy(alpha = 0.75f * alpha), topLeft, boxSize, radius,
            style = Stroke(width = 1.3f * px, pathEffect = PathEffect.dashPathEffect(floatArrayOf(6f * px, 4f * px))),
        )
        drawCircle(fill.copy(alpha = alpha), 4.5f, Offset(r.x.toFloat() + 16f, r.centerY.toFloat()))
        if (focused) drawRoundRect(accent, topLeft, boxSize, radius, style = Stroke(width = 2.4f * px))
        return
    }
    val radius = if (item.module) 9f else 7f
    val corner = CornerRadius(radius, radius)
    val body = StrataColors.mix(surface, fill, if (dark) 0.12 else 0.08)
    drawRoundRect(body.copy(alpha = alpha), topLeft, boxSize, corner)
    val border = if (item.cycle) StrataColors.unhealthy else StrataColors.mix(borderColor, fill, 0.55)
    drawRoundRect(border.copy(alpha = alpha), topLeft, boxSize, corner, style = Stroke(width = (if (item.cycle) 2f else 1.2f) * px))
    if (item.module) {
        val clip = Path().apply { addRoundRect(RoundRect(r.x.toFloat(), r.y.toFloat(), r.right.toFloat(), r.bottom.toFloat(), corner)) }
        clipPath(clip) {
            drawRect(fill.copy(alpha = alpha), topLeft, Size(boxSize.width, 24f))
        }
        if (r.height * scale > 24 * density / 1.5) {
            val cellAlpha = (if (dark) 0.78f else 0.85f) * alpha
            for ((cell, color) in item.cells) {
                drawRect(
                    color.copy(alpha = cellAlpha),
                    Offset(cell.x.toFloat() + 0.6f, cell.y.toFloat() + 0.6f),
                    Size(max(0f, cell.width.toFloat() - 1.2f), max(0f, cell.height.toFloat() - 1.2f)),
                )
            }
        }
    } else {
        drawRoundRect(fill.copy(alpha = alpha), topLeft, Size(6f, boxSize.height), CornerRadius(3f, 3f))
    }
    if (focused) drawRoundRect(accent, Offset(topLeft.x - 3f * px, topLeft.y - 3f * px), Size(boxSize.width + 6f * px, boxSize.height + 6f * px), CornerRadius(radius + 3f * px, radius + 3f * px), style = Stroke(width = 2.4f * px))
}

private fun DrawScope.drawSceneLabels(
    measurer: TextMeasurer,
    item: StrataSceneItem,
    key: Int,
    fill: Color,
    scale: Float,
    offset: Offset,
    alpha: Float,
    density: Float,
    titles: HashMap<Int, TextLayoutResult>,
    captions: HashMap<Int, TextLayoutResult>,
    titleStyle: TextStyle,
    captionStyle: TextStyle,
    text: Color,
    muted: Color,
) {
    val r = item.rect
    val left = offset.x + r.x.toFloat() * scale
    val top = offset.y + r.y.toFloat() * scale
    val width = r.width.toFloat() * scale
    val height = r.height.toFloat() * scale
    val unit = density
    val title = titles.getOrPut(key) { measurer.measure(item.title, titleStyle, softWrap = false, maxLines = 1) }
    val caption = captions.getOrPut(key) { measurer.measure(item.caption, captionStyle, softWrap = false, maxLines = 1) }
    if (item.external) {
        if (height < 22 * unit) return
        val textLeft = left + 26f * scale
        clipRect(textLeft, top, left + width - 10f * scale, top + height) {
            drawText(title, color = text.copy(alpha = alpha), topLeft = Offset(textLeft, top + height / 2f - title.size.height))
            drawText(caption, color = muted.copy(alpha = alpha), topLeft = Offset(textLeft, top + height / 2f))
        }
        return
    }
    if (item.module) {
        val header = 24f * scale
        if (header >= title.size.height * 0.8f) {
            clipRect(left + 6f, top, left + width - 6f, top + header) {
                drawText(
                    title,
                    color = StrataColors.readableOn(fill).copy(alpha = alpha),
                    topLeft = Offset(left + 8f * min(1f, scale) + 2f, top + (header - title.size.height) / 2f),
                )
            }
        }
        if (height > 46 * unit && 18f * scale >= caption.size.height * 0.8f) {
            clipRect(left + 6f, top + header, left + width - 6f, top + header + 18f * scale) {
                drawText(caption, color = muted.copy(alpha = alpha), topLeft = Offset(left + 8f * min(1f, scale) + 2f, top + header + 1f))
            }
        }
    } else {
        if (height < 26 * unit) return
        val textLeft = left + 12f * scale + 4f
        clipRect(textLeft, top, left + width - 4f, top + height) {
            drawText(title, color = text.copy(alpha = alpha), topLeft = Offset(textLeft, top + height / 2f - title.size.height))
            drawText(caption, color = muted.copy(alpha = alpha), topLeft = Offset(textLeft, top + height / 2f))
        }
    }
}

@Composable
private fun StrataSelectionBar(model: StrataModel, selection: StrataFocus, modifier: Modifier, onDetails: () -> Unit, onDrill: (() -> Unit)?) {
    val c = Fm.colors
    val report = model.report
    val (title, subtitle, rating) = when (selection) {
        is StrataFocus.Module -> {
            val node = report.graph.nodes[selection.index]
            val score = report.modules[selection.index]
            Triple(
                node.path.ifEmpty { node.name },
                "${Strata.number(node.codeLines)} LOC · Ca ${node.afferent} · Ce ${node.efferent} · ${Strata.formatDebt(score.debt)} debt",
                score.rating,
            )
        }
        is StrataFocus.File -> {
            val file = model.snapshot.files[selection.index]
            val score = report.files[selection.index]
            Triple(
                file.path,
                "${Strata.number(file.codeLines)} LOC · max cc ${file.maxComplexity} · ${Strata.formatDebt(score.debt)} debt",
                score.rating,
            )
        }
        is StrataFocus.Cell -> return
    }
    Row(
        modifier
            .fillMaxWidth()
            .padding(FmSpace.x3)
            .clip(RoundedCornerShape(FmRadius.lg))
            .background(if (c.dark) c.bgOverlay else c.bgRaised)
            .padding(horizontal = FmSpace.x3, vertical = FmSpace.x2),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        StrataRatingChip(rating, description = "Rating ${Strata.ratingLetter(rating)}")
        Spacer(Modifier.width(FmSpace.x3))
        Column(Modifier.weight(1f)) {
            Text(title, style = Fm.text.bodyMedium, color = c.textPrimary, maxLines = 1, overflow = TextOverflow.MiddleEllipsis)
            Text(subtitle, style = Fm.text.labelSmall, color = c.textSecondary, maxLines = 1, overflow = TextOverflow.Ellipsis)
        }
        if (onDrill != null) {
            Spacer(Modifier.width(FmSpace.x2))
            FmToneButton("Files", onPressed = onDrill, icon = "account_tree", tooltip = "Show the module's files")
        }
        Spacer(Modifier.width(FmSpace.x2))
        FmToneButton("Details", onPressed = onDetails, icon = "info_outline", tone = com.forkmesh.nativeapp.ui.kit.FmTone.Accent)
    }
}

@Composable
private fun StrataGraphList(model: StrataModel, state: StrataPageState, context: CodeInsightContext) {
    val c = Fm.colors
    val report = model.report
    val graph = report.graph
    val drill = state.drill
    LazyColumn(Modifier.fillMaxSize()) {
        if (state.drilled < 0) {
            items(model.order, key = { "m$it" }) { m ->
                val node = graph.nodes[m]
                val score = report.modules[m]
                FmListRow(
                    title = node.path.ifEmpty { node.name },
                    subtitle = "${Strata.number(node.codeLines)} LOC · ${node.files.size} files · Ca ${node.afferent} · Ce ${node.efferent}" +
                        (if (node.cycle >= 0) " · in a cycle" else "") + " · ${Strata.formatDebt(score.debt)} debt",
                    titleColor = if (node.cycle >= 0) StrataColors.unhealthy else null,
                    leading = { StrataRatingChip(score.rating, description = "Rating ${Strata.ratingLetter(score.rating)}") },
                    trailing = {
                        FmToneButton("Files", onPressed = { state.drillInto(m) }, icon = "account_tree", tooltip = "Show the files of ${node.name}")
                    },
                    showChevron = false,
                    onTap = { state.select(StrataFocus.Module(m)) },
                )
            }
        } else if (drill != null) {
            val fileNodes = drill.nodes.filter { !it.module }
            val uses = drill.nodes.filter { it.module && !it.dependent }
            val usedBy = drill.nodes.filter { it.module && it.dependent }
            item(key = "files-header") {
                FmSectionHeader("Files", Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), icon = "description", count = fileNodes.size)
            }
            items(fileNodes, key = { "f${it.index}" }) { node ->
                val file = model.snapshot.files[node.index]
                val score = report.files[node.index]
                FmListRow(
                    title = Strata.fileName(file.path),
                    subtitle = "${Strata.number(file.codeLines)} LOC · max cc ${file.maxComplexity} · imports ${file.dependsOn.size} · imported by ${file.dependedOnBy.size}",
                    leading = { StrataRatingChip(score.rating, description = "Rating ${Strata.ratingLetter(score.rating)}") },
                    onTap = { state.select(StrataFocus.File(node.index)) },
                )
            }
            if (drill.hiddenFiles > 0) {
                item(key = "hidden") {
                    Text(
                        "${drill.hiddenFiles} less connected files are not in the graph; the module's details list them all.",
                        style = Fm.text.bodySmall,
                        color = c.textSecondary,
                        modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2)
                            .clickable(role = Role.Button) { state.select(StrataFocus.Module(state.drilled)) },
                    )
                }
            }
            for ((label, list, color) in listOf(Triple("Uses", uses, c.accent), Triple("Used by", usedBy, Color.Unspecified))) {
                if (list.isEmpty()) continue
                item(key = "header-$label") {
                    FmSectionHeader(label, Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x2), icon = "folder", count = list.size)
                }
                items(list, key = { "$label${it.index}" }) { node ->
                    val other = graph.nodes[node.index]
                    FmListRow(
                        title = other.path.ifEmpty { other.name },
                        subtitle = "${node.weight} imports",
                        leading = { StrataDot(if (color == Color.Unspecified) strataDependent() else color, 10.dp) },
                        onTap = {
                            state.drillOut()
                            state.select(StrataFocus.Module(node.index))
                        },
                    )
                }
            }
        }
        item(key = "end") { Spacer(Modifier.padding(bottom = FmSpace.x5)) }
    }
}
