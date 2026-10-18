package com.forkmesh.nativeapp.feature.codeinsight

import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.PorterDuff
import android.graphics.PorterDuffColorFilter
import android.graphics.PorterDuffXfermode
import android.graphics.RadialGradient
import android.graphics.Rect
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import android.util.SparseArray
import com.forkmesh.nativeapp.codeinsight.Synapse
import com.forkmesh.nativeapp.codeinsight.SynapseBrainShape
import com.forkmesh.nativeapp.codeinsight.SynapseColourBy
import com.forkmesh.nativeapp.codeinsight.SynapseColours
import com.forkmesh.nativeapp.codeinsight.SynapseGraph
import com.forkmesh.nativeapp.codeinsight.SynapseProjection
import com.forkmesh.nativeapp.codeinsight.SynapseScene
import com.forkmesh.nativeapp.codeinsight.SynapseShowMode
import com.forkmesh.nativeapp.codeinsight.SynapseStyle
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

internal class SynapseRenderer(private val density: Float, private val fontScale: Float) {
    private val add = PorterDuffXfermode(PorterDuff.Mode.ADD)
    private val fibre = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        xfermode = add
    }
    private val dot = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        xfermode = add
    }
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val glow = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG).apply { xfermode = add }
    private val sprite = Paint(Paint.ANTI_ALIAS_FLAG or Paint.FILTER_BITMAP_FLAG)
    private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val sky = Paint()
    private val vignette = Paint()
    private val label = Paint(Paint.ANTI_ALIAS_FLAG).apply { textSize = 12f * density * fontScale }
    private val strong = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = 13f * density * fontScale
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
    }
    private val lobeText = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        textSize = 10f * density * fontScale
        typeface = Typeface.create(Typeface.DEFAULT, Typeface.BOLD)
        letterSpacing = 0.12f
    }
    private val pill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val pillBorder = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }

    private val haloBitmap = radialBitmap(floatArrayOf(0f, 0.25f, 0.6f, 1f), intArrayOf(0xFFFFFFFF.toInt(), 0x73FFFFFF, 0x1AFFFFFF, 0x00FFFFFF), 0f, 0f)
    private val sphereBitmap = radialBitmap(
        floatArrayOf(0f, 0.32f, 0.78f, 1f),
        intArrayOf(0xFFFFFFFF.toInt(), 0xFFE0E0E0.toInt(), 0xFF8A8A8A.toInt(), 0xFF555555.toInt()),
        -0.38f,
        -0.42f,
    )
    private val haloSrc = Rect(0, 0, haloBitmap.width, haloBitmap.height)
    private val sphereSrc = Rect(0, 0, sphereBitmap.width, sphereBitmap.height)
    private val dst = RectF()
    private val tint = SparseArray<PorterDuffColorFilter>()
    private val shade = SparseArray<PorterDuffColorFilter>()
    private val metrics = Paint.FontMetrics()

    private var skyWidth = 0f
    private var skyHeight = 0f
    private var lines = FloatArray(64)
    private var points = Array(3) { FloatArray(0) }
    private val pointCounts = IntArray(3)
    private var order = LongArray(0)
    private var taken = FloatArray(4 * 96)
    private var takenCount = 0

    private var styleKey: SynapseStyle? = null
    private var edgeMid = IntArray(0)
    private var edgeStart = IntArray(0)
    private var edgeEnd = IntArray(0)
    private var graphKey: SynapseGraph? = null
    private var names: Array<String> = emptyArray()
    private var lobeNames: Array<String> = emptyArray()
    private var lobeOrder = IntArray(0)
    private var neighbourOrder = IntArray(0)
    private var centres = FloatArray(0)
    private var centresFor: FloatArray? = null

    val dust: FloatArray = SynapseScene.dust(SynapseBrainShape())
    val stars: FloatArray = SynapseScene.stars()

    private fun radialBitmap(stops: FloatArray, colours: IntArray, offsetX: Float, offsetY: Float): Bitmap {
        val size = 96
        val bitmap = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
        val canvas = Canvas(bitmap)
        val r = size / 2f
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        paint.shader = RadialGradient(r + offsetX * r, r + offsetY * r, r, colours, stops, Shader.TileMode.CLAMP)
        canvas.drawCircle(r, r, r - 0.5f, paint)
        return bitmap
    }

    private fun filterFor(cache: SparseArray<PorterDuffColorFilter>, colour: Int, mode: PorterDuff.Mode): PorterDuffColorFilter {
        val key = colour or 0xFF000000.toInt()
        cache.get(key)?.let { return it }
        if (cache.size() > 4096) cache.clear()
        val filter = PorterDuffColorFilter(key, mode)
        cache.put(key, filter)
        return filter
    }

    private fun tintFor(colour: Int): PorterDuffColorFilter = filterFor(tint, colour, PorterDuff.Mode.SRC_IN)

    private fun shadeFor(colour: Int): PorterDuffColorFilter = filterFor(shade, colour, PorterDuff.Mode.MULTIPLY)

    private fun prepare(style: SynapseStyle, positions: FloatArray) {
        val graph = style.graph
        if (graphKey !== graph) {
            graphKey = graph
            names = Array(graph.nodes.size) { Synapse.fileName(graph.snapshot.files[graph.nodes[it].file].path) }
            lobeNames = Array(graph.lobes.size) { SynapseScene.shortLobeName(graph.lobes[it].name).uppercase() }
            lobeOrder = graph.lobes.indices.sortedByDescending { graph.lobes[it].nodes.size }.toIntArray()
            order = LongArray(graph.nodes.size)
            centresFor = null
        }
        if (centresFor !== positions) {
            centresFor = positions
            centres = if (positions.size == graph.nodes.size * 3) Synapse.lobeCentres(graph, positions) else FloatArray(0)
        }
        if (styleKey !== style) {
            styleKey = style
            val count = graph.edges.size
            if (edgeMid.size < count) {
                edgeMid = IntArray(count)
                edgeStart = IntArray(count)
                edgeEnd = IntArray(count)
            }
            for (e in style.drawEdges) {
                edgeMid[e] = style.edgeColour(e, 0.5f)
                if (style.highlighted(e)) {
                    edgeStart[e] = style.edgeColour(e, 0f)
                    edgeEnd[e] = style.edgeColour(e, 1f)
                }
            }
            neighbourOrder = if (style.options.selected >= 0) {
                val neighbours = style.neighbours
                graph.nodes.indices
                    .filter { style.nodeState[it] == SynapseScene.IMPORT || style.nodeState[it] == SynapseScene.IMPORTER }
                    .sortedWith(
                        compareBy<Int> {
                            val file = graph.nodes[it].file
                            max(neighbours.imports[file] ?: 9, 0) + (neighbours.importers[file] ?: 0)
                        }.thenByDescending { graph.nodes[it].fanIn },
                    )
                    .take(28)
                    .toIntArray()
            } else {
                IntArray(0)
            }
        }
    }

    fun draw(canvas: Canvas, state: SynapsePageState, moving: Boolean) {
        val width = state.width
        val height = state.height
        drawBackdrop(canvas, width, height)
        val style = state.style ?: return
        val graph = style.graph
        val positions = state.positions
        if (positions.size != graph.nodes.size * 3) return
        prepare(style, positions)
        val projection = state.projection
        projection.update(style, positions, state.camera, width, height)
        drawAura(canvas, projection, graph)
        drawStars(canvas, projection, width, height)
        drawDust(canvas, projection)
        val time = state.clock
        val animatePulses = style.options.pulses && !state.reducedMotion
        drawFibres(canvas, style, positions, projection, moving, time, animatePulses)
        drawNodes(canvas, style, projection, moving, time, animatePulses)
        drawLabels(canvas, style, state, projection)
    }

    private fun drawBackdrop(canvas: Canvas, width: Float, height: Float) {
        if (width != skyWidth || height != skyHeight) {
            skyWidth = width
            skyHeight = height
            sky.shader = LinearGradient(0f, 0f, 0f, height, 0xFF0B1122.toInt(), 0xFF03050A.toInt(), Shader.TileMode.CLAMP)
            val radius = kotlin.math.hypot(width, height) * 0.62f
            vignette.shader = RadialGradient(
                width / 2f,
                height / 2f,
                max(1f, radius),
                intArrayOf(0x00000000, 0x00000000, 0x96000000.toInt()),
                floatArrayOf(0f, 0.55f, 1f),
                Shader.TileMode.CLAMP,
            )
        }
        canvas.drawRect(0f, 0f, width, height, sky)
        canvas.drawRect(0f, 0f, width, height, vignette)
    }

    private fun drawAura(canvas: Canvas, projection: SynapseProjection, graph: SynapseGraph) {
        if (!projection.project(0f, 0f, 0f) || projection.pDepth <= 1f) return
        val radius = graph.shape.boundingRadius * 1.25f * projection.focal / projection.pDepth
        glow.colorFilter = tintFor(0xFF2848A0.toInt())
        glow.alpha = 70
        dst.set(projection.px - radius, projection.py - radius, projection.px + radius, projection.py + radius)
        canvas.drawBitmap(haloBitmap, haloSrc, dst, glow)
    }

    private fun ensurePoints(bucket: Int, count: Int) {
        if (points[bucket].size < count * 2) points[bucket] = FloatArray(count * 2)
    }

    private fun drawStars(canvas: Canvas, projection: SynapseProjection, width: Float, height: Float) {
        val count = stars.size / 4
        for (b in 0 until 3) {
            ensurePoints(b, count)
            pointCounts[b] = 0
        }
        for (i in 0 until count) {
            if (!projection.project(stars[i * 4] * STAR_RADIUS, stars[i * 4 + 1] * STAR_RADIUS, stars[i * 4 + 2] * STAR_RADIUS)) continue
            if (projection.pDepth <= 1f) continue
            val x = projection.px
            val y = projection.py
            if (x < 0 || y < 0 || x > width || y > height) continue
            val bright = stars[i * 4 + 3]
            val bucket = if (bright > 0.4f) 2 else if (bright > 0.08f) 1 else 0
            val at = pointCounts[bucket]++
            points[bucket][at * 2] = x
            points[bucket][at * 2 + 1] = y
        }
        for (b in 0 until 3) {
            if (pointCounts[b] == 0) continue
            val bright = 0.04f + 0.45f * b
            dot.strokeWidth = (1.0f + 1.1f * bright * 2f) * density * 0.6f
            dot.color = SynapseColours.withAlpha(0xFFAAC4FF.toInt(), (40f + 150f * bright) / 255f)
            canvas.drawPoints(points[b], 0, pointCounts[b] * 2, dot)
        }
    }

    private fun drawDust(canvas: Canvas, projection: SynapseProjection) {
        val count = dust.size / 3
        for (b in 0 until 3) {
            ensurePoints(b, count)
            pointCounts[b] = 0
        }
        for (i in 0 until count) {
            if (!projection.project(dust[i * 3], dust[i * 3 + 1], dust[i * 3 + 2]) || projection.pDepth <= 1f) continue
            val fog = projection.fogAt(projection.pDepth, 0.8f)
            val bucket = if (fog > 0.75f) 2 else if (fog > 0.45f) 1 else 0
            val at = pointCounts[bucket]++
            points[bucket][at * 2] = projection.px
            points[bucket][at * 2 + 1] = projection.py
        }
        dot.strokeWidth = 1.8f * density * 0.6f
        for (b in 0 until 3) {
            if (pointCounts[b] == 0) continue
            val fog = 0.3f + 0.35f * b
            dot.color = SynapseColours.withAlpha(0xFF78A0EB.toInt(), 90f * fog / 255f)
            canvas.drawPoints(points[b], 0, pointCounts[b] * 2, dot)
        }
    }

    private fun quad(a: Float, c: Float, b: Float, t: Float): Float {
        val u = 1f - t
        return a * (u * u) + c * (2f * u * t) + b * (t * t)
    }

    private fun drawFibres(
        canvas: Canvas,
        style: SynapseStyle,
        positions: FloatArray,
        projection: SynapseProjection,
        moving: Boolean,
        time: Float,
        animatePulses: Boolean,
    ) {
        val graph = style.graph
        var plain = 0
        for (e in style.drawEdges) if (!style.highlighted(e)) plain++
        val stride = if (moving) max(1, plain / PLAIN_WHILE_MOVING) else 1
        var plainIndex = 0
        var pulsesDrawn = 0
        val width = projection.width
        val height = projection.height
        val bow = 1f - SynapseScene.BOW
        for (e in style.drawEdges) {
            val edge = graph.edges[e]
            val highlighted = style.highlighted(e)
            if (moving && !highlighted) {
                if (style.edgeState[e] == SynapseScene.EDGE_DIM && plain > PLAIN_WHILE_MOVING) continue
                if (plainIndex++ % stride != 0) continue
            }
            val a = edge.from
            val b = edge.to
            if (projection.depth[a] <= 0f || projection.depth[b] <= 0f) continue
            val ax = projection.x[a]
            val ay = projection.y[a]
            val bx = projection.x[b]
            val by = projection.y[b]
            val mx = (positions[a * 3] + positions[b * 3]) * 0.5f * bow
            val my = (positions[a * 3 + 1] + positions[b * 3 + 1]) * 0.5f * bow
            val mz = (positions[a * 3 + 2] + positions[b * 3 + 2]) * 0.5f * bow
            if (!projection.project(mx, my, mz)) continue
            val cx = projection.px
            val cy = projection.py
            val minX = min(min(ax, bx), cx)
            val maxX = max(max(ax, bx), cx)
            val minY = min(min(ay, by), cy)
            val maxY = max(max(ay, by), cy)
            if (maxX < -40f || minX > width + 40f || maxY < -40f || minY > height + 40f) continue
            val fog = 0.5f * (projection.fog[a] + projection.fog[b])
            val segments = if (highlighted) 8 else 3
            val strokeWidth = style.edgeWidth(e) * density
            if (highlighted) {
                val mid = edgeMid[e]
                fibre.color = SynapseColours.withAlpha(mid, SynapseColours.alpha(mid) * fog * 0.18f)
                fibre.strokeWidth = strokeWidth * 4f
                polyline(canvas, ax, ay, cx, cy, bx, by, segments, fibre)
                fibre.strokeWidth = strokeWidth * 1.4f
                var px = ax
                var py = ay
                for (s in 1..segments) {
                    val t = s.toFloat() / segments
                    val x = quad(ax, cx, bx, t)
                    val y = quad(ay, cy, by, t)
                    val colour = SynapseColours.mix(edgeStart[e], edgeEnd[e], t - 0.5f / segments)
                    fibre.color = SynapseColours.withAlpha(colour, SynapseColours.alpha(colour) * fog)
                    canvas.drawLine(px, py, x, y, fibre)
                    px = x
                    py = y
                }
            } else {
                val mid = edgeMid[e]
                fibre.color = SynapseColours.withAlpha(mid, SynapseColours.alpha(mid) * fog)
                fibre.strokeWidth = strokeWidth * 1.1f
                polyline(canvas, ax, ay, cx, cy, bx, by, segments, fibre)
            }
            if (animatePulses && pulsesDrawn < MAX_PULSES && style.edgePulses(e)) {
                pulsesDrawn++
                val head = ((time * 0.32f + SynapseScene.phaseOf(edge.seed)) % 1f + 1f) % 1f
                val colour = SynapseColours.mix(edgeMid[e], SynapseColours.WHITE, 0.55f)
                for (k in 0 until 4) {
                    val t = head - k * 0.022f
                    if (t < 0f) break
                    val x = quad(ax, cx, bx, t)
                    val y = quad(ay, cy, by, t)
                    val size = (3.6f - k * 0.7f) * density * 0.55f
                    val alpha = ((0.9f - 0.2f * k) * fog).coerceIn(0f, 1f)
                    glow.colorFilter = tintFor(colour)
                    glow.alpha = (alpha * 255f).toInt()
                    dst.set(x - size * 2f, y - size * 2f, x + size * 2f, y + size * 2f)
                    canvas.drawBitmap(haloBitmap, haloSrc, dst, glow)
                }
            }
        }
    }

    private fun polyline(canvas: Canvas, ax: Float, ay: Float, cx: Float, cy: Float, bx: Float, by: Float, segments: Int, paint: Paint) {
        if (lines.size < segments * 4) lines = FloatArray(segments * 4)
        var px = ax
        var py = ay
        for (s in 1..segments) {
            val t = s.toFloat() / segments
            val x = quad(ax, cx, bx, t)
            val y = quad(ay, cy, by, t)
            lines[(s - 1) * 4] = px
            lines[(s - 1) * 4 + 1] = py
            lines[(s - 1) * 4 + 2] = x
            lines[(s - 1) * 4 + 3] = y
            px = x
            py = y
        }
        canvas.drawLines(lines, 0, segments * 4, paint)
    }

    private fun drawNodes(canvas: Canvas, style: SynapseStyle, projection: SynapseProjection, moving: Boolean, time: Float, animatePulses: Boolean) {
        val graph = style.graph
        val n = graph.nodes.size
        var count = 0
        for (i in 0 until n) {
            if (!projection.onScreen[i] || style.nodeState[i] == SynapseScene.HIDDEN) continue
            val bits = java.lang.Float.floatToRawIntBits(projection.depth[i])
            order[count++] = ((Int.MAX_VALUE - bits).toLong() shl 32) or i.toLong()
        }
        java.util.Arrays.sort(order, 0, count)
        val many = count > (if (moving) 1500 else 4000)
        val pulse = if (animatePulses) 0.5f + 0.5f * sin(time * 4f) else 0.5f
        for (k in 0 until count) {
            val i = (order[k] and 0xffffffffL).toInt()
            val state = style.nodeState[i]
            val colour = style.nodeColour[i]
            val r = max(0.9f * density * 0.5f, projection.radius[i])
            val fog = projection.fog[i]
            val x = projection.x[i]
            val y = projection.y[i]
            val cycle = graph.nodes[i].cycle && state == SynapseScene.NORMAL
            if (state != SynapseScene.DIM && r >= 1.2f && (!many || state != SynapseScene.NORMAL || cycle)) {
                var intensity = if (state == SynapseScene.NORMAL) 0.32f else 0.62f
                if (state == SynapseScene.SELECTED) intensity = 0.9f
                if (cycle) intensity = 0.4f + if (style.options.pulses) 0.45f * pulse else 0.2f
                glow.colorFilter = tintFor(colour)
                glow.alpha = (intensity * fog.coerceIn(0f, 1f) * 255f).toInt().coerceIn(0, 255)
                val h = r * 3f
                dst.set(x - h, y - h, x + h, y + h)
                canvas.drawBitmap(haloBitmap, haloSrc, dst, glow)
            }
            val opacity = (if (state == SynapseScene.DIM) fog * 0.85f else 0.25f + 0.75f * fog).coerceIn(0f, 1f)
            if (r < 1.6f) {
                fill.color = SynapseColours.withAlpha(colour, opacity)
                canvas.drawCircle(x, y, r, fill)
                continue
            }
            sprite.colorFilter = shadeFor(colour)
            sprite.alpha = (opacity * 255f).toInt()
            dst.set(x - r, y - r, x + r, y + r)
            canvas.drawBitmap(sphereBitmap, sphereSrc, dst, sprite)
            if (cycle && style.options.pulses) {
                fill.color = SynapseColours.withAlpha(0xFFFFBEB4.toInt(), 0.25f * pulse * opacity)
                canvas.drawCircle(x, y, r * 0.9f, fill)
            }
            if (r >= 2.5f) {
                fill.color = SynapseColours.withAlpha(SynapseColours.WHITE, 0.55f * opacity)
                canvas.drawCircle(x - 0.36f * r, y - 0.4f * r, r * 0.22f, fill)
            }
            if (state == SynapseScene.SELECTED) {
                ring.strokeWidth = 1.4f * density
                ring.color = 0xC8FFFFFF.toInt()
                canvas.drawCircle(x, y, r + 4f * density, ring)
                ring.strokeWidth = 1f * density
                ring.color = 0x5A78C8FF
                canvas.drawCircle(x, y, r + 8f * density, ring)
            }
        }
    }

    private fun place(canvas: Canvas, x: Float, y: Float, offset: Float, text: String, paint: Paint, colour: Int, border: Int, pill: Boolean): Boolean {
        val padX = if (pill) 7f * density else 0f
        val padY = if (pill) 3f * density else 0f
        paint.getFontMetrics(metrics)
        val w = paint.measureText(text) + padX * 2f
        val h = (metrics.descent - metrics.ascent) + padY * 2f
        var left = x + offset
        val top = y - h / 2f
        val margin = 4f * density
        if (left + w > skyWidth - 6f * density) left = x - offset - w
        if (left < margin || top < margin || left + w > skyWidth - margin || top + h > skyHeight - margin) return false
        val gapX = 3f * density
        val gapY = 2f * density
        for (t in 0 until takenCount) {
            val l = taken[t * 4] - gapX
            val tp = taken[t * 4 + 1] - gapY
            val r = taken[t * 4 + 2] + gapX
            val b = taken[t * 4 + 3] + gapY
            if (left < r && left + w > l && top < b && top + h > tp) return false
        }
        if (takenCount * 4 + 4 > taken.size) taken = taken.copyOf(taken.size * 2)
        taken[takenCount * 4] = left
        taken[takenCount * 4 + 1] = top
        taken[takenCount * 4 + 2] = left + w
        taken[takenCount * 4 + 3] = top + h
        takenCount++
        val baseline = top + padY - metrics.ascent
        if (pill) {
            dst.set(left, top, left + w, top + h)
            this.pill.color = 0xCD0A0E1A.toInt()
            canvas.drawRoundRect(dst, h / 2f, h / 2f, this.pill)
            pillBorder.color = border
            pillBorder.strokeWidth = density
            canvas.drawRoundRect(dst, h / 2f, h / 2f, pillBorder)
            paint.color = colour
            canvas.drawText(text, left + padX, baseline, paint)
        } else {
            paint.color = 0x96000000.toInt()
            canvas.drawText(text, left, baseline + density, paint)
            paint.color = colour
            canvas.drawText(text, left, baseline, paint)
        }
        return true
    }

    private fun nodeLabel(canvas: Canvas, projection: SynapseProjection, node: Int, border: Int, bold: Boolean) {
        if (node < 0 || !projection.onScreen[node]) return
        place(
            canvas,
            projection.x[node],
            projection.y[node],
            max(4f * density, projection.radius[node]) + 7f * density,
            names[node],
            if (bold) strong else label,
            SynapseColours.TEXT,
            border,
            true,
        )
    }

    private fun drawLabels(canvas: Canvas, style: SynapseStyle, state: SynapsePageState, projection: SynapseProjection) {
        val graph = style.graph
        takenCount = 0
        val selected = style.options.selected
        if (selected >= 0) {
            val node = graph.nodeFor(selected)
            if (node >= 0 && style.nodeState[node] != SynapseScene.HIDDEN) nodeLabel(canvas, projection, node, 0xDC78C8FF.toInt(), true)
            for (i in neighbourOrder) {
                val border = if (style.nodeState[i] == SynapseScene.IMPORT) SynapseColours.withAlpha(SynapseColours.IMPORT, 0.6f) else SynapseColours.withAlpha(SynapseColours.IMPORTER, 0.6f)
                nodeLabel(canvas, projection, i, border, false)
            }
            return
        }
        val isolated = style.options.isolatedCycle
        if (isolated >= 0 && isolated < graph.cycles.size) {
            for (file in graph.cycles[isolated].files) nodeLabel(canvas, projection, graph.nodeFor(file), SynapseColours.withAlpha(SynapseColours.CYCLE, 0.7f), false)
        }
        for (hub in style.hubs) nodeLabel(canvas, projection, hub, 0x32FFFFFF, false)
        if (style.options.showMode != SynapseShowMode.Everything || style.options.query.isNotBlank() || isolated >= 0) return
        if (centres.size != graph.lobes.size * 3) return
        var shown = 0
        for (l in lobeOrder) {
            if (shown >= 18 || graph.lobes[l].nodes.size < 2) break
            if (!projection.project(centres[l * 3], centres[l * 3 + 1], centres[l * 3 + 2]) || projection.pDepth <= 1f) continue
            val colour = if (style.options.colourBy == SynapseColourBy.Module) {
                SynapseColours.withAlpha(SynapseColours.lighter(SynapseColours.lobe(l), 115), 0.9f)
            } else {
                0xBEC8D2E6.toInt()
            }
            val text = lobeNames[l]
            val x = projection.px - lobeText.measureText(text) / 2f
            if (place(canvas, x, projection.py, 0f, text, lobeText, colour, 0, false)) shown++
        }
    }

    private companion object {
        const val STAR_RADIUS = 1500f
        const val PLAIN_WHILE_MOVING = 1200
        const val MAX_PULSES = 400
    }
}
