package com.forkmesh.nativeapp.feature.codeinsight

import android.graphics.Paint
import android.graphics.Typeface
import android.text.TextPaint
import android.text.TextUtils
import androidx.compose.foundation.Canvas
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.gestures.detectTransformGestures
import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Modifier
import androidx.compose.ui.geometry.Offset
import androidx.compose.ui.geometry.Size
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.Path
import androidx.compose.ui.graphics.PathEffect
import androidx.compose.ui.graphics.StrokeCap
import androidx.compose.ui.graphics.drawscope.DrawScope
import androidx.compose.ui.graphics.drawscope.Stroke
import androidx.compose.ui.graphics.drawscope.drawIntoCanvas
import androidx.compose.ui.graphics.lerp
import androidx.compose.ui.graphics.nativeCanvas
import androidx.compose.ui.graphics.toArgb
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.layout.onSizeChanged
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import com.forkmesh.nativeapp.codeinsight.Ember
import com.forkmesh.nativeapp.codeinsight.EmberColor
import com.forkmesh.nativeapp.codeinsight.EmberColoring
import com.forkmesh.nativeapp.codeinsight.EmberCouplingRef
import com.forkmesh.nativeapp.codeinsight.EmberMap
import com.forkmesh.nativeapp.codeinsight.EmberPanel
import com.forkmesh.nativeapp.codeinsight.EmberTab
import com.forkmesh.nativeapp.codeinsight.InsightSnapshot
import com.forkmesh.nativeapp.ui.theme.Fm
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlin.math.PI
import kotlin.math.ceil
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

internal fun EmberColor.compose(): Color = Color(r.toFloat(), g.toFloat(), b.toFloat(), a.toFloat())

private class EmberFills(val fill: Array<Color>, val light: Array<Color>, val shade: Array<Color>)

private fun fillsFor(coloring: EmberColoring?, background: Color, dark: Boolean): EmberFills {
    val styles = coloring?.files ?: emptyList()
    val fill = Array(styles.size) { Color.Gray }
    val light = Array(styles.size) { Color.Gray }
    val shade = Array(styles.size) { Color.Gray }
    for (i in styles.indices) {
        var c = styles[i].color.compose()
        if (styles[i].dimmed) c = lerp(c, background, if (dark) 0.74f else 0.7f)
        fill[i] = c
        light[i] = lerp(c, Color.White, if (styles[i].dimmed) 0.06f else 0.3f)
        shade[i] = lerp(c, Color.Black, if (styles[i].dimmed) 0.06f else 0.28f)
    }
    return EmberFills(fill, light, shade)
}

@Composable
internal fun EmberMapCanvas(
    state: EmberMapState,
    snapshot: InsightSnapshot,
    coloring: EmberColoring?,
    coupling: List<List<EmberCouplingRef>>,
    description: String,
    onTapFile: (Int) -> Unit,
    onClearSelection: () -> Unit,
    modifier: Modifier = Modifier,
) {
    val colors = Fm.colors
    val dark = colors.dark
    val background = colors.bgBase
    val density = LocalDensity.current.density.toDouble()
    state.density = density
    state.reducedMotion = Fm.reducedMotion
    val fills = remember(coloring, background, dark) { fillsFor(coloring, background, dark) }
    val labelPaint = remember { TextPaint(Paint.ANTI_ALIAS_FLAG) }
    val overlayPaint = remember { TextPaint(Paint.ANTI_ALIAS_FLAG) }
    val haloPaint = remember { TextPaint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE; strokeJoin = Paint.Join.ROUND } }
    var glowPhase by remember { mutableFloatStateOf(0f) }
    val glowing = coloring?.tab == EmberTab.Hotspots && (coloring.glowing) > 0 && !Fm.reducedMotion
    LaunchedEffect(glowing) {
        if (!glowing) return@LaunchedEffect
        val start = System.nanoTime()
        while (true) {
            glowPhase = ((System.nanoTime() - start) / 1e9 * 2 * PI / 2.4).toFloat()
            delay(50)
        }
    }
    val tapFile by rememberUpdatedState(onTapFile)
    val clearSelection by rememberUpdatedState(onClearSelection)
    LaunchedEffect(state) {
        snapshotFlow { state.interaction }.collectLatest { count ->
            if (count == 0) return@collectLatest
            delay(150)
            state.settle()
        }
    }
    Box(
        modifier
            .onSizeChanged { state.resize(it.width.toDouble(), it.height.toDouble()) }
            .semantics { contentDescription = description }
            .pointerInput(state.layout) {
                detectTransformGestures { centroid, pan, zoom, _ ->
                    state.transform(centroid.x.toDouble(), centroid.y.toDouble(), pan.x.toDouble(), pan.y.toDouble(), zoom.toDouble())
                }
            }
            .pointerInput(state.layout) {
                detectTapGestures(
                    onDoubleTap = { offset -> state.zoomBy(2.0, offset.x.toDouble(), offset.y.toDouble()) },
                    onTap = { offset ->
                        if (!state.ready) return@detectTapGestures
                        val tap = EmberMap.tapAt(
                            state.layout, state.view, state.focus, state.selected, offset.x.toDouble(), offset.y.toDouble(),
                            state.width, state.height, fileRadius = 2.5 * density, folderRadius = 14.0 * density,
                        )
                        when (tap) {
                            is EmberMap.Tap.SelectFile -> tapFile(tap.file)
                            is EmberMap.Tap.Focus -> state.focusNode(tap.node)
                            EmberMap.Tap.ZoomOut -> state.zoomOut()
                            EmberMap.Tap.ClearSelection -> clearSelection()
                            EmberMap.Tap.None -> Unit
                        }
                    },
                )
            },
    ) {
        Canvas(Modifier.fillMaxSize()) {
            drawRect(background)
            if (!state.ready) return@Canvas
            drawScene(state, snapshot, coupling, coloring, fills, dark, state.moving, density, labelPaint, haloPaint)
        }
        Canvas(Modifier.fillMaxSize()) {
            if (!state.ready) return@Canvas
            if (coloring != null && coloring.tab == EmberTab.Hotspots && coloring.glowing > 0) drawGlow(state, coloring, glowPhase, density)
            drawLinks(state, coupling, dark, density, overlayPaint)
            if (coloring != null && coloring.tab == EmberTab.RefactoringTargets) drawBadges(state, coloring, dark, background, density, overlayPaint)
            drawSelection(state, dark, density)
        }
    }
}

private fun DrawScope.drawScene(
    state: EmberMapState,
    snapshot: InsightSnapshot,
    coupling: List<List<EmberCouplingRef>>,
    coloring: EmberColoring?,
    fills: EmberFills,
    dark: Boolean,
    fast: Boolean,
    density: Double,
    labelPaint: TextPaint,
    haloPaint: TextPaint,
) {
    val layout = state.layout
    val nodes = layout.nodes
    val w = size.width.toDouble()
    val h = size.height.toDouble()
    val folderFill = if (dark) Color(255, 255, 255, 10) else Color(20, 30, 50, 10)
    val folderRim = if (dark) Color(255, 255, 255, 34) else Color(20, 30, 50, 42)
    val focusRim = if (dark) Color(255, 255, 255, 60) else Color(20, 30, 50, 70)
    val shadow = if (dark) Color(0, 0, 0, 90) else Color(30, 40, 60, 40)
    val markRing = if (dark) Color(255, 255, 255, 200) else Color(30, 35, 45, 200)
    val neutral = Color(0xFF6E7681)
    val styles = coloring?.files
    val stack = ArrayList<Int>(256)
    stack.add(0)
    val fileLabels = ArrayList<Int>()
    val folderLabels = ArrayList<Int>()
    val dp = density.toFloat()
    val diagonal = hypot(w, h)
    while (stack.isNotEmpty()) {
        val index = stack.removeAt(stack.size - 1)
        val node = nodes[index]
        val cx = state.screenX(node.x)
        val cy = state.screenY(node.y)
        val r = node.r * state.view.k
        if (r < 0.6) continue
        if (cx + r < -2 || cx - r > w + 2 || cy + r < -2 || cy - r > h + 2) continue
        if (r > 4 * diagonal && hypot(cx - w / 2, cy - h / 2) + diagonal < r) {
            for (i in node.children.indices.reversed()) stack.add(node.children[i])
            continue
        }
        val c = Offset(cx.toFloat(), cy.toFloat())
        val rf = r.toFloat()
        if (node.file < 0) {
            drawCircle(folderFill, rf, c)
            if (r >= 3) drawCircle(if (index == state.focus) focusRim else folderRim, rf, c, style = Stroke(if (index == state.focus) 1.2f * dp else dp))
            if (r >= 70 * density && index != 0 && folderLabels.size < 40) folderLabels.add(index)
            for (i in node.children.indices.reversed()) stack.add(node.children[i])
            continue
        }
        val file = node.file
        val hasStyle = file < fills.fill.size
        val fill = if (hasStyle) fills.fill[file] else neutral
        if (r < 2.2) {
            val s = (r * 0.89).toFloat()
            drawRect(fill, Offset(c.x - s, c.y - s), Size(2 * s, 2 * s))
            continue
        }
        val style = styles?.getOrNull(file)
        val dimmed = style?.dimmed == true
        if (!fast && r >= 5 && !dimmed) drawCircle(shadow, rf * 1.01f, Offset(c.x, c.y + max(1f, rf * 0.07f)))
        if (!fast && r >= 4 && hasStyle) {
            val center = Offset(c.x - rf * 0.38f, c.y - rf * 0.45f)
            drawCircle(
                Brush.radialGradient(0f to fills.light[file], 0.55f to fill, 1f to fills.shade[file], center = center, radius = rf * 1.45f),
                rf,
                c,
            )
        } else {
            drawCircle(fill, rf, c)
        }
        if (r >= 7 && !dimmed) drawCircle(lerp(fill, Color.Black, 0.45f).copy(alpha = 110f / 255f), rf - 0.5f, c, style = Stroke(dp))
        if (style?.marked == true && r >= 3.5) {
            drawCircle(
                markRing, rf + 2.5f * dp, c,
                style = Stroke((r * 0.06).coerceIn(1.0, 2.0).toFloat() * dp, pathEffect = PathEffect.dashPathEffect(floatArrayOf(2f * dp, 2f * dp))),
            )
        }
        if (r >= 18 * density && fileLabels.size < 160) fileLabels.add(index)
    }
    drawLabels(state, snapshot, coupling, coloring, fileLabels, folderLabels, dark, fast, density, labelPaint, haloPaint)
}

private fun DrawScope.drawLabels(
    state: EmberMapState,
    snapshot: InsightSnapshot,
    coupling: List<List<EmberCouplingRef>>,
    coloring: EmberColoring?,
    files: List<Int>,
    folders: List<Int>,
    dark: Boolean,
    fast: Boolean,
    density: Double,
    paint: TextPaint,
    halo: TextPaint,
) {
    val nodes = state.layout.nodes
    val dp = density.toFloat()
    drawIntoCanvas { canvas ->
        val native = canvas.nativeCanvas
        paint.typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
        halo.typeface = paint.typeface
        val folderText = if (dark) Color(230, 237, 243, 215) else Color(31, 35, 40, 220)
        val folderHalo = if (dark) Color(8, 10, 14, 200) else Color(255, 255, 255, 220)
        for (index in folders) {
            val node = nodes[index]
            val r = (node.r * state.view.k).toFloat()
            val cx = state.screenX(node.x).toFloat()
            val cy = state.screenY(node.y).toFloat()
            val textSize = (r / dp * 0.05f).coerceIn(11f, 14f) * dp
            paint.textSize = textSize
            halo.textSize = textSize
            val baseline = r - paint.fontMetrics.let { -it.ascent } * 0.35f - max(3f * dp, r * 0.012f) - textSize * 0.3f
            if (cy - baseline < -10 * dp || cy - baseline > size.height + 10 * dp) continue
            val maxSpan = (PI * 0.8).toFloat()
            var label: String = node.name
            var width = paint.measureText(label)
            if (width / baseline > maxSpan) {
                label = TextUtils.ellipsize(node.name, paint, maxSpan * baseline, TextUtils.TruncateAt.MIDDLE).toString()
                width = paint.measureText(label)
            }
            if (label.isEmpty() || baseline <= 0) continue
            val sweep = Math.toDegrees((width / baseline).toDouble()).toFloat()
            val path = android.graphics.Path().apply {
                addArc(cx - baseline, cy - baseline, cx + baseline, cy + baseline, -90f - sweep / 2, sweep)
            }
            if (!fast) {
                halo.color = folderHalo.toArgb()
                halo.strokeWidth = 3f * dp
                native.drawTextOnPath(label, path, 0f, 0f, halo)
            }
            paint.color = folderText.toArgb()
            native.drawTextOnPath(label, path, 0f, 0f, paint)
        }
        paint.typeface = Typeface.DEFAULT
        halo.typeface = Typeface.DEFAULT
        val dimText = if (dark) Color(230, 237, 243, 85) else Color(31, 35, 40, 110)
        val fileHalo = Color(0, 0, 0, 120)
        for (index in files) {
            val node = nodes[index]
            val r = (node.r * state.view.k).toFloat()
            val cx = state.screenX(node.x).toFloat()
            val cy = state.screenY(node.y).toFloat()
            val dimmed = coloring?.files?.getOrNull(node.file)?.dimmed == true
            val room = r * 1.86f
            var s = (r / dp * 0.24f - 8f).coerceIn(0f, 7f).toInt()
            paint.textSize = (8 + s) * dp
            while (s > 0 && paint.measureText(node.name) > room) {
                s--
                paint.textSize = (8 + s) * dp
            }
            val name = TextUtils.ellipsize(node.name, paint, room, TextUtils.TruncateAt.END).toString()
            if (name.length < 5 && name != node.name) continue
            val showMetric = r >= 38 * dp && !fast
            val metrics = paint.fontMetrics
            val lineH = metrics.descent - metrics.ascent
            val nameY = cy - (if (showMetric) lineH * 0.45f else 0f) + lineH / 2 - metrics.descent
            val x = cx - paint.measureText(name) / 2
            if (!fast && !dimmed) {
                halo.textSize = paint.textSize
                halo.color = fileHalo.toArgb()
                halo.strokeWidth = 2.5f * dp
                native.drawText(name, x, nameY, halo)
            }
            paint.color = (if (dimmed) dimText else Color.White).toArgb()
            native.drawText(name, x, nameY, paint)
            if (showMetric && coloring != null) {
                val metric = EmberPanel.metricLabel(snapshot, coloring.tab, coupling, node.file)
                if (metric.isNotEmpty()) {
                    paint.textSize = 10 * dp
                    val shown = TextUtils.ellipsize(metric, paint, r * 1.5f, TextUtils.TruncateAt.END).toString()
                    val my = nameY + metrics.descent + 1 * dp - paint.fontMetrics.ascent
                    val mx = cx - paint.measureText(shown) / 2
                    if (!dimmed) {
                        halo.textSize = paint.textSize
                        native.drawText(shown, mx, my, halo)
                    }
                    paint.color = (if (dimmed) dimText else Color(255, 255, 255, 200)).toArgb()
                    native.drawText(shown, mx, my, paint)
                }
            }
        }
    }
}

private fun DrawScope.drawGlow(state: EmberMapState, coloring: EmberColoring, phase: Float, density: Double) {
    val glow = Color(255, 123, 82)
    val layout = state.layout
    for (i in coloring.files.indices) {
        val style = coloring.files[i]
        if (style.glow <= 0 || style.dimmed) continue
        val index = layout.fileNode(i)
        if (index < 0) continue
        val r = (layout.nodes[index].r * state.view.k).toFloat()
        if (r < 2.5f * density) continue
        val c = Offset(state.screenX(layout.nodes[index].x).toFloat(), state.screenY(layout.nodes[index].y).toFloat())
        val breathe = 0.5f + 0.5f * sin(phase + i * 0.7f)
        val outer = r + max(6f * density.toFloat(), r * (0.22f + 0.12f * breathe))
        if (c.x < -outer || c.y < -outer || c.x > size.width + outer || c.y > size.height + outer) continue
        val inner = r / outer
        val alpha = ((90 + 90 * breathe) * style.glow.toFloat() / 255f).coerceIn(0f, 1f)
        drawCircle(
            Brush.radialGradient(
                max(0f, inner - 0.02f) to glow.copy(alpha = 0f),
                inner to glow.copy(alpha = alpha),
                1f to glow.copy(alpha = 0f),
                center = c,
                radius = outer,
            ),
            outer,
            c,
        )
    }
}

private fun bezier(a: Offset, c: Offset, b: Offset, t: Float): Offset {
    val u = 1 - t
    return Offset(a.x * u * u + c.x * 2 * u * t + b.x * t * t, a.y * u * u + c.y * 2 * u * t + b.y * t * t)
}

private fun DrawScope.drawLinks(state: EmberMapState, coupling: List<List<EmberCouplingRef>>, dark: Boolean, density: Double, paint: TextPaint) {
    val selected = state.selected
    if (selected < 0) return
    val layout = state.layout
    val selNode = layout.fileNode(selected)
    if (selNode < 0) return
    val dp = density.toFloat()
    val a = Offset(state.screenX(layout.nodes[selNode].x).toFloat(), state.screenY(layout.nodes[selNode].y).toFloat())
    val link = Color(0xFFF2CC60)
    val p = EmberMap.easeInOut(state.linkProgress.toDouble()).toFloat()
    val refs = coupling.getOrNull(selected) ?: return
    val labels = ArrayList<Pair<Offset, String>>()
    for (i in 0 until min(40, refs.size)) {
        val ref = refs[i]
        val other = layout.fileNode(ref.file)
        if (other < 0) continue
        val b = Offset(state.screenX(layout.nodes[other].x).toFloat(), state.screenY(layout.nodes[other].y).toFloat())
        val rb = (layout.nodes[other].r * state.view.k).toFloat()
        val d = b - a
        val len = hypot(d.x, d.y)
        if (len < 1) continue
        val normal = Offset(-d.y / len, d.x / len)
        val control = (a + b) / 2f + normal * (len * 0.22f)
        val path = Path().apply { moveTo(a.x, a.y) }
        val steps = 36
        val upto = max(1, ceil(steps * p).toInt())
        for (s in 1..upto) {
            val pt = bezier(a, control, b, min(p, s.toFloat() / steps))
            path.lineTo(pt.x, pt.y)
        }
        val width = (1.2f + 3.8f * ref.degree.toFloat().coerceIn(0f, 1f)) * dp
        drawPath(path, (if (dark) Color.Black else Color.White).copy(alpha = 110f / 255f), style = Stroke(width + 2.5f * dp, cap = StrokeCap.Round))
        drawPath(path, link.copy(alpha = ((110 + 130 * ref.degree) / 255.0).toFloat().coerceIn(0f, 1f)), style = Stroke(width, cap = StrokeCap.Round))
        if (p >= 1f) {
            val ring = max(rb + 1.5f * dp, 3f * dp)
            if (rb < 3 * dp) drawCircle(link, ring, b) else drawCircle(link, ring, b, style = Stroke(2f * dp))
            if (len > 70 * dp) labels.add(bezier(a, control, b, 0.5f) to Ember.percentText(ref.degree))
        }
    }
    if (labels.isEmpty()) return
    drawIntoCanvas { canvas ->
        paint.textSize = 10 * dp
        paint.typeface = Typeface.DEFAULT_BOLD
        for ((at, text) in labels) {
            val w = paint.measureText(text) + 10 * dp
            drawRoundRect(
                if (dark) Color(22, 27, 34, 235) else Color(255, 255, 255, 240),
                Offset(at.x - w / 2, at.y - 8 * dp),
                Size(w, 16 * dp),
                androidx.compose.ui.geometry.CornerRadius(8 * dp),
            )
            paint.color = (if (dark) link else lerp(link, Color.Black, 0.45f)).toArgb()
            val fm = paint.fontMetrics
            canvas.nativeCanvas.drawText(text, at.x - (w - 10 * dp) / 2, at.y - (fm.ascent + fm.descent) / 2, paint)
        }
        paint.typeface = Typeface.DEFAULT
    }
}

private fun DrawScope.drawBadges(state: EmberMapState, coloring: EmberColoring, dark: Boolean, background: Color, density: Double, paint: TextPaint) {
    val dp = density.toFloat()
    val layout = state.layout
    drawIntoCanvas { canvas ->
        paint.textSize = 10 * dp
        paint.typeface = Typeface.DEFAULT_BOLD
        for (target in coloring.targets) {
            val index = layout.fileNode(target.file)
            if (index < 0) continue
            val node = layout.nodes[index]
            val c = Offset(state.screenX(node.x).toFloat(), state.screenY(node.y).toFloat())
            val r = (node.r * state.view.k).toFloat()
            val br = 8.5f * dp
            val at = if (r >= 10 * dp) c + Offset(r * 0.72f, -r * 0.72f) else c + Offset(r + br - 2 * dp, -r - br + 2 * dp)
            if (at.x < -12 * dp || at.y < -12 * dp || at.x > size.width + 12 * dp || at.y > size.height + 12 * dp) continue
            drawCircle(if (dark) Color(240, 246, 252) else Color(31, 35, 40), br, at)
            drawCircle(background, br, at, style = Stroke(2 * dp))
            paint.color = (if (dark) Color(13, 17, 23) else Color.White).toArgb()
            val text = target.rank.toString()
            val fm = paint.fontMetrics
            canvas.nativeCanvas.drawText(text, at.x - paint.measureText(text) / 2, at.y - (fm.ascent + fm.descent) / 2, paint)
        }
        paint.typeface = Typeface.DEFAULT
    }
}

private fun DrawScope.drawSelection(state: EmberMapState, dark: Boolean, density: Double) {
    val node = state.layout.fileNode(state.selected)
    if (node < 0) return
    val dp = density.toFloat()
    val n = state.layout.nodes[node]
    val a = Offset(state.screenX(n.x).toFloat(), state.screenY(n.y).toFloat())
    val ra = (n.r * state.view.k).toFloat()
    drawCircle(if (dark) Color(13, 17, 23) else Color(31, 35, 40), ra + 1.5f * dp, a, style = Stroke(3 * dp))
    drawCircle(if (dark) Color(240, 246, 252) else Color.White, ra + 3.5f * dp, a, style = Stroke(1.5f * dp))
    if (ra < 8 * dp) {
        val p = EmberMap.easeInOut(state.linkProgress.toDouble()).toFloat()
        val pulse = (10 + 6 * (1 - p)) * dp
        drawCircle((if (dark) Color(240, 246, 252) else Color(31, 35, 40)).copy(alpha = 160f / 255f), ra + pulse, a, style = Stroke(1.2f * dp))
    }
}
