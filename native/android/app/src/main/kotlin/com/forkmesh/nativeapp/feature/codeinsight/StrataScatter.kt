package com.forkmesh.nativeapp.feature.codeinsight

import androidx.compose.foundation.Canvas
import com.forkmesh.nativeapp.ui.theme.FmType
import androidx.compose.foundation.background
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.CornerRadius
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.clipRect
import androidx.compose.ui.graphics.drawscope.rotate
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.TextStyle
import androidx.compose.ui.text.drawText
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.rememberTextMeasurer
import androidx.compose.ui.unit.sp
import com.forkmesh.nativeapp.codeinsight.Strata
import com.forkmesh.nativeapp.codeinsight.StrataModel
import com.forkmesh.nativeapp.ui.theme.Fm
import com.forkmesh.nativeapp.ui.theme.FmHaptics
import com.forkmesh.nativeapp.ui.theme.FmSpace
import androidx.compose.foundation.layout.padding
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sqrt

@Composable
internal fun StrataScatterView(model: StrataModel, state: StrataPageState) {
    val c = Fm.colors
    val haptics = FmHaptics.current
    val density = LocalDensity.current.density
    val nodes = model.report.graph.nodes
    val drawOrder = remember(model) { nodes.indices.sortedByDescending { nodes[it].codeLines } }
    val maxLines = remember(model) { max(1.0, nodes.maxOfOrNull { it.codeLines.toDouble() } ?: 1.0) }
    val measurer = rememberTextMeasurer(cacheSize = 256)
    var zoom by remember(model) { mutableFloatStateOf(1f) }
    var pan by remember(model) { mutableStateOf(Offset.Zero) }
    val plotHolder = remember { arrayOf(Rect.Zero) }
    val selected = (state.selection as? StrataFocus.Module)?.index ?: -1
    fun pointFor(m: Int, plot: Rect): Offset {
        val node = nodes[m]
        val x = plot.left + node.instability.coerceIn(0.0, 1.0).toFloat() * plot.width
        val y = plot.bottom - node.abstractness.coerceIn(0.0, 1.0).toFloat() * plot.height
        return Offset(plot.left + (x - plot.left) * zoom + pan.x, plot.top + (y - plot.top) * zoom + pan.y)
    }
    fun radiusFor(m: Int, plot: Rect): Float {
        val side = plot.width
        return (4.0 * density + side * 0.04 * sqrt(nodes[m].codeLines / maxLines)).coerceIn(4.0 * density, max(4.0 * density, side * 0.05)).toFloat()
    }
    val labelStyle = TextStyle(fontFamily = FmType.ui, fontSize = 11.sp, fontWeight = FontWeight.SemiBold, color = c.textPrimary)
    val selectedStyle = labelStyle.copy(color = c.accent)
    val axisStyle = TextStyle(fontFamily = FmType.ui, fontSize = 10.sp, color = c.textSecondary)
    val titleStyle = TextStyle(fontFamily = FmType.ui, fontSize = 11.sp, fontWeight = FontWeight.SemiBold, color = c.textPrimary)
    val zoneStyle = TextStyle(fontFamily = FmType.ui, fontSize = 11.sp, fontWeight = FontWeight.SemiBold)
    Column(Modifier.fillMaxSize()) {
        Canvas(
            Modifier
                .weight(1f)
                .fillMaxWidth()
                .background(c.bgBase)
                .semantics {
                    contentDescription = "Abstractness against instability for ${nodes.size} modules, with the zone of pain at the bottom left, " +
                        "the zone of uselessness at the top right and the main sequence between. Pinch to zoom, tap a bubble for its module."
                }
                .pointerInput(model) {
                    detectTransformGestures { centroid, delta, factor, _ ->
                        val next = (zoom * factor).coerceIn(1f, 8f)
                        val ratio = next / zoom
                        val anchor = Offset(plotHolder[0].left, plotHolder[0].top)
                        val local = centroid - anchor
                        pan = (pan - local) * ratio + local + delta
                        zoom = next
                        if (zoom == 1f) pan = Offset.Zero
                    }
                }
                .pointerInput(model) {
                    detectTapGestures(
                        onTap = { p ->
                            var best = -1
                            var bestDistance = Float.MAX_VALUE
                            for (i in drawOrder.indices.reversed()) {
                                val m = drawOrder[i]
                                val d = hypot(pointFor(m, plotHolder[0]).x - p.x, pointFor(m, plotHolder[0]).y - p.y)
                                val r = radiusFor(m, plotHolder[0]) + 10f * density
                                if (d <= r && d / r < bestDistance) {
                                    bestDistance = d / r
                                    best = m
                                }
                            }
                            if (best >= 0) {
                                haptics.selection()
                                state.select(StrataFocus.Module(best))
                            } else {
                                state.select(null)
                            }
                        },
                        onDoubleTap = {
                            zoom = 1f
                            pan = Offset.Zero
                        },
                    )
                },
        ) {
            val leftPad = 44f * density
            val bottomPad = 44f * density
            val side = max(120f * density, min(size.width - leftPad - 16f * density, size.height - bottomPad - 16f * density))
            val plot = Rect(leftPad + (size.width - leftPad - 16f * density - side) / 2f, 12f * density, leftPad + (size.width - leftPad - 16f * density - side) / 2f + side, 12f * density + side)
            plotHolder[0] = plot
            drawRoundRect(c.bgRaised, plot.topLeft, plot.size, CornerRadius(6f * density, 6f * density))
            clipRect(plot.left, plot.top, plot.right, plot.bottom) {
                val zoomed = Rect(plot.left + pan.x, plot.top + pan.y, plot.left + pan.x + plot.width * zoom, plot.top + pan.y + plot.height * zoom)
                drawRect(
                    Brush.radialGradient(
                        0f to StrataColors.unhealthy.copy(alpha = if (c.dark) .34f else .24f),
                        0.7f to StrataColors.unhealthy.copy(alpha = .06f),
                        1f to StrataColors.unhealthy.copy(alpha = 0f),
                        center = zoomed.bottomLeft,
                        radius = zoomed.width * 0.62f,
                    ),
                    plot.topLeft, plot.size,
                )
                drawRect(
                    Brush.radialGradient(
                        0f to StrataColors.problematic.copy(alpha = if (c.dark) .30f else .24f),
                        0.7f to StrataColors.problematic.copy(alpha = .05f),
                        1f to StrataColors.problematic.copy(alpha = 0f),
                        center = zoomed.topRight,
                        radius = zoomed.width * 0.62f,
                    ),
                    plot.topLeft, plot.size,
                )
                val b = 0.12f
                val band = Path().apply {
                    moveTo(zoomed.left, zoomed.top)
                    lineTo(zoomed.left + b * zoomed.width, zoomed.top)
                    lineTo(zoomed.right, zoomed.bottom - b * zoomed.height)
                    lineTo(zoomed.right, zoomed.bottom)
                    lineTo(zoomed.right - b * zoomed.width, zoomed.bottom)
                    lineTo(zoomed.left, zoomed.top + b * zoomed.height)
                    close()
                }
                drawPath(band, StrataColors.healthy.copy(alpha = if (c.dark) .08f else .07f))
                val grid = c.border.copy(alpha = .55f)
                for (i in 1 until 4) {
                    val t = i / 4f
                    drawLine(grid, Offset(zoomed.left + t * zoomed.width, zoomed.top), Offset(zoomed.left + t * zoomed.width, zoomed.bottom), 1f)
                    drawLine(grid, Offset(zoomed.left, zoomed.top + t * zoomed.height), Offset(zoomed.right, zoomed.top + t * zoomed.height), 1f)
                }
                drawLine(
                    StrataColors.healthy.copy(alpha = .85f), zoomed.topLeft, zoomed.bottomRight, 2f * density,
                    pathEffect = PathEffect.dashPathEffect(floatArrayOf(8f * density, 5f * density)),
                )
                val pain = measurer.measure("Zone of pain", zoneStyle.copy(color = StrataColors.unhealthy))
                drawText(pain, topLeft = Offset(zoomed.left + 10f * density, zoomed.bottom - zoomed.height * 0.2f))
                val useless = measurer.measure("Zone of uselessness", zoneStyle.copy(color = StrataColors.problematic))
                drawText(useless, topLeft = Offset(zoomed.right - useless.size.width - 10f * density, zoomed.top + 10f * density))
                val sequence = measurer.measure("Main sequence", zoneStyle.copy(color = StrataColors.healthy))
                val anchor = Offset(zoomed.left + zoomed.width * 0.58f, zoomed.top + zoomed.height * 0.42f)
                rotate(45f, anchor) {
                    drawText(sequence, topLeft = Offset(anchor.x - sequence.size.width / 2f, anchor.y - sequence.size.height - 4f * density))
                }
                for (m in drawOrder) {
                    val p = pointFor(m, plot)
                    val r = radiusFor(m, plot)
                    val color = StrataColors.scale(nodes[m].distance)
                    drawCircle(
                        Brush.radialGradient(
                            listOf(StrataColors.mix(color, Color.White, 0.3).copy(alpha = .92f), color.copy(alpha = .78f)),
                            center = p - Offset(r * .35f, r * .35f),
                            radius = r * 1.3f,
                        ),
                        r, p,
                    )
                    drawCircle(StrataColors.mix(color, Color.Black, 0.3).copy(alpha = .9f), r, p, style = Stroke(1f * density))
                }
                val labelled = ArrayList<Int>()
                for (m in drawOrder) if (labelled.size < 10) labelled.add(m)
                for (m in drawOrder) if (nodes[m].distance > 0.7 && labelled.size < 18 && m !in labelled) labelled.add(m)
                if (selected >= 0 && selected !in labelled) labelled.add(0, selected)
                val taken = ArrayList<Rect>()
                for (m in labelled) {
                    val p = pointFor(m, plot)
                    val r = radiusFor(m, plot)
                    val layout = measurer.measure(nodes[m].name, if (m == selected) selectedStyle else labelStyle, softWrap = false, maxLines = 1)
                    val w = layout.size.width + 10f * density
                    val h = layout.size.height + 4f * density
                    var box = Rect(p.x + r + 4f * density, p.y - h / 2f, p.x + r + 4f * density + w, p.y + h / 2f)
                    if (box.right > plot.right - 2f) box = Rect(p.x - r - 4f * density - w, box.top, p.x - r - 4f * density, box.bottom)
                    if (taken.any { it.overlaps(box) } && m != selected) continue
                    taken.add(box)
                    drawRoundRect(c.bgRaised.copy(alpha = .88f), box.topLeft, box.size, CornerRadius(5f * density, 5f * density))
                    drawText(layout, topLeft = Offset(box.left + 5f * density, box.top + 2f * density))
                }
                if (selected in nodes.indices) {
                    val p = pointFor(selected, plot)
                    val r = radiusFor(selected, plot)
                    drawCircle(c.accent.copy(alpha = .2f), r + 6f * density, p, style = Stroke(6f * density))
                    drawCircle(c.accent, r + 2f * density, p, style = Stroke(2.4f * density))
                }
            }
            for (i in 0..4) {
                val t = i / 4f
                val label = measurer.measure(Strata.fixed(t.toDouble(), if (i % 2 == 1) 2 else 1), axisStyle)
                val x = plot.left + pan.x + t * plot.width * zoom
                if (x >= plot.left - 1 && x <= plot.right + 1) {
                    drawText(label, topLeft = Offset(x - label.size.width / 2f, plot.bottom + 4f * density))
                }
                val y = plot.top + pan.y + (1 - t) * plot.height * zoom
                if (y >= plot.top - 1 && y <= plot.bottom + 1) {
                    drawText(label, topLeft = Offset(plot.left - label.size.width - 6f * density, y - label.size.height / 2f))
                }
            }
            val xTitle = measurer.measure("Instability  I = Ce ÷ (Ca + Ce)", titleStyle)
            drawText(xTitle, topLeft = Offset(plot.center.x - xTitle.size.width / 2f, plot.bottom + 20f * density))
            val yTitle = measurer.measure("Abstractness  A = abstract types ÷ types", titleStyle)
            val yAnchor = Offset(plot.left - 34f * density, plot.center.y)
            rotate(-90f, yAnchor) {
                drawText(yTitle, topLeft = Offset(yAnchor.x - yTitle.size.width / 2f, yAnchor.y - yTitle.size.height / 2f))
            }
        }
        StrataLegendRow {
            StrataGradientKey("0 on the sequence", "1 far from it", (0..10).map { StrataColors.scale(it / 10.0) }, "Distance D")
            Text("Bubble size: lines of code", style = Fm.text.labelSmall, color = c.textSecondary)
        }
        Text(
            "${nodes.count { it.distance > 0.7 }} modules far from the main sequence · double tap to reset the zoom",
            style = Fm.text.labelSmall,
            color = c.textTertiary,
            modifier = Modifier.padding(horizontal = FmSpace.x4, vertical = FmSpace.x1),
        )
    }
}
