package com.forkmesh.nativeapp.feature.agentwatch

import android.graphics.Bitmap
import android.graphics.BitmapShader
import android.graphics.Canvas
import android.graphics.DashPathEffect
import android.graphics.Matrix
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import com.forkmesh.nativeapp.agentwatch.AgentMotion
import com.forkmesh.nativeapp.agentwatch.AgentWatchController
import com.forkmesh.nativeapp.agentwatch.WatchFieldLayout
import com.forkmesh.nativeapp.agentwatch.WatchPoint
import com.forkmesh.nativeapp.agentwatch.WatchState
import com.forkmesh.nativeapp.agentwatch.WatchedAgent
import com.forkmesh.nativeapp.agentwatch.bandLabel
import com.forkmesh.nativeapp.agentwatch.kHistorySeconds
import com.forkmesh.nativeapp.agentwatch.kWatchBandHeader
import com.forkmesh.nativeapp.agentwatch.kWatchLabelGap
import com.forkmesh.nativeapp.agentwatch.kWatchLabelPixels
import com.forkmesh.nativeapp.agentwatch.kWatchLedGapDots
import com.forkmesh.nativeapp.agentwatch.kWatchLedHold
import com.forkmesh.nativeapp.agentwatch.kWatchLedPad
import com.forkmesh.nativeapp.agentwatch.kWatchLedPitch
import com.forkmesh.nativeapp.agentwatch.kWatchLedRows
import com.forkmesh.nativeapp.agentwatch.kWatchLedSpeed
import com.forkmesh.nativeapp.agentwatch.watchCardLines
import com.forkmesh.nativeapp.agentwatch.watchTitleWithoutNumber
import kotlin.math.PI
import kotlin.math.ceil
import kotlin.math.cos
import kotlin.math.exp
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

internal enum class WatchBackdrop(val label: String) { theme("Theme"), space("Space"), aurora("Aurora"), ocean("Ocean") }

internal const val WATCH_BACKGROUND: Int = 0xFF060910.toInt()
internal const val WATCH_FOREGROUND: Int = 0xFFECF2FA.toInt()
internal const val WATCH_MUTED: Int = 0xFF8C98AC.toInt()
private const val WATCH_GUIDE: Int = 0x10FFFFFF
private const val RING_MARKS = 40

internal fun WatchPoint.o() = O(x, y)

private fun circleRect(c: O, r: Double) = RectF((c.x - r).toFloat(), (c.y - r).toFloat(), (c.x + r).toFloat(), (c.y + r).toFloat())

internal class WatchText(val density: Float) {
    private val cache = HashMap<Long, Paint>()

    fun paint(size: Double, bold: Boolean, colour: Int, tracking: Double = 0.0): Paint {
        val key = (size * 100).toLong() * 4 + (if (bold) 2 else 0) + (if (tracking != 0.0) 1 else 0)
        val paint = cache.getOrPut(key) {
            Paint(Paint.ANTI_ALIAS_FLAG).apply {
                textSize = size.toFloat()
                typeface = if (bold) Typeface.DEFAULT_BOLD else Typeface.DEFAULT
                if (tracking != 0.0) letterSpacing = (tracking / size).toFloat()
            }
        }
        paint.color = colour
        return paint
    }

    fun fit(paint: Paint, text: String, maxWidth: Double?): String {
        if (maxWidth == null || paint.measureText(text) <= maxWidth || text.isEmpty()) return text
        var low = 0
        var high = text.length
        var best = "…"
        while (low <= high) {
            val mid = (low + high) / 2
            val candidate = text.substring(0, mid) + "…"
            if (paint.measureText(candidate) <= maxWidth) {
                best = candidate
                low = mid + 1
            } else {
                high = mid - 1
            }
        }
        return best
    }

    fun lineHeight(paint: Paint): Double = (paint.fontMetrics.descent - paint.fontMetrics.ascent).toDouble()

    fun drawTop(canvas: Canvas, paint: Paint, text: String, x: Double, top: Double) {
        canvas.drawText(text, x.toFloat(), (top - paint.fontMetrics.ascent).toFloat(), paint)
    }
}

internal object WatchSky {
    private class Star(val x: Double, val y: Double, val size: Double, val brightness: Double, val phase: Double)

    private val stars = (0 until 460).map { i ->
        val r = scatter(i * 7 + 3)
        Star(scatter(i * 7 + 1), scatter(i * 7 + 2), 0.5 + 1.9 * r * r * r, 0.25 + 0.75 * scatter(i * 7 + 4), 6.2832 * scatter(i * 7 + 6))
    }

    fun paint(canvas: Canvas, w: Double, h: Double, backdrop: WatchBackdrop) {
        val rect = RectF(0f, 0f, w.toFloat(), h.toFloat())
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        fun vertical(colours: IntArray, stops: FloatArray) {
            paint.shader = android.graphics.LinearGradient(0f, 0f, 0f, h.toFloat(), colours, stops, Shader.TileMode.CLAMP)
            canvas.drawRect(rect, paint)
            paint.shader = null
        }
        when (backdrop) {
            WatchBackdrop.theme -> vertical(intArrayOf(0xFF0B101C.toInt(), WATCH_BACKGROUND), floatArrayOf(0f, 1f))
            WatchBackdrop.space -> {
                vertical(intArrayOf(0xFF02030C.toInt(), 0xFF07091C.toInt(), 0xFF0D0920.toInt()), floatArrayOf(0f, 0.6f, 1f))
                val clouds = listOf(doubleArrayOf(0.22, 0.32, 0.46, 0.78), doubleArrayOf(0.76, 0.64, 0.52, 0.53), doubleArrayOf(0.56, 0.12, 0.30, 0.93))
                for (cloud in clouds) {
                    val centre = O(cloud[0] * w, cloud[1] * h)
                    val radius = cloud[2] * max(w, h)
                    val hue = cloud[3]
                    paint.shader = android.graphics.RadialGradient(
                        centre.x.toFloat(), centre.y.toFloat(), radius.toFloat(),
                        intArrayOf(watchInk(hue, 0.65, 0.45, 0.17), watchInk(hue, 0.6, 0.35, 0.06), watchInk(hue, 0.6, 0.3, 0.0)),
                        floatArrayOf(0f, 0.5f, 1f), Shader.TileMode.CLAMP,
                    )
                    canvas.drawCircle(centre.x.toFloat(), centre.y.toFloat(), radius.toFloat(), paint)
                    paint.shader = null
                }
                paintStars(canvas, w, h, 1.0, 1.0)
            }
            WatchBackdrop.aurora -> {
                vertical(intArrayOf(0xFF020710.toInt(), 0xFF05121E.toInt(), 0xFF070C18.toInt()), floatArrayOf(0f, 0.55f, 1f))
                paintStars(canvas, w, h, 0.5, 0.7)
                val hues = doubleArrayOf(0.38, 0.46, 0.78)
                for (k in 0 until 3) {
                    val base = h * (0.16 + 0.11 * k)
                    val band = h * (0.20 + 0.04 * k)
                    val top = ArrayList<O>()
                    var x = -20.0
                    while (x <= w + 20) {
                        val u = x / max(1.0, w)
                        top += O(x, base + h * 0.05 * sin(u * 8.2 + k) + h * 0.02 * sin(u * 19.5 + k * 2.0))
                        x += 14
                    }
                    val curtain = Path()
                    curtain.moveTo(top[0].x.toFloat(), top[0].y.toFloat())
                    for (p in top) curtain.lineTo(p.x.toFloat(), p.y.toFloat())
                    for (p in top.asReversed()) curtain.lineTo(p.x.toFloat(), (p.y + band).toFloat())
                    curtain.close()
                    paint.shader = android.graphics.LinearGradient(
                        0f, (base - h * 0.07).toFloat(), 0f, (base + band + h * 0.07).toFloat(),
                        intArrayOf(watchInk(hues[k], 0.9, 0.55, 0.0), watchInk(hues[k], 0.9, 0.55, 0.20), watchInk(hues[k], 0.9, 0.45, 0.0)),
                        floatArrayOf(0f, 0.3f, 1f), Shader.TileMode.CLAMP,
                    )
                    canvas.drawPath(curtain, paint)
                    paint.shader = null
                }
            }
            WatchBackdrop.ocean -> {
                vertical(intArrayOf(0xFF06203A.toInt(), 0xFF031226.toInt(), 0xFF010814.toInt()), floatArrayOf(0f, 0.5f, 1f))
                for (i in 0 until 5) {
                    val x = w * (0.1 + 0.2 * i) + 40 * sin(i * 1.3)
                    val spread = w * 0.05 + 20 * sin(i.toDouble())
                    val shaft = Path()
                    shaft.moveTo((x - spread * 0.4).toFloat(), 0f)
                    shaft.lineTo((x + spread * 0.4).toFloat(), 0f)
                    shaft.lineTo((x + spread * 2.2 + 60).toFloat(), h.toFloat())
                    shaft.lineTo((x - spread * 0.6 + 60).toFloat(), h.toFloat())
                    shaft.close()
                    paint.shader = android.graphics.LinearGradient(0f, 0f, 0f, h.toFloat(), 0x1AA0DCFF, 0x00A0DCFF, Shader.TileMode.CLAMP)
                    canvas.drawPath(shaft, paint)
                    paint.shader = null
                }
                var i = 0
                while (i < stars.size) {
                    val star = stars[i]
                    val alpha = 0.10 + 0.25 * star.brightness * (1.0 - star.y * 0.6)
                    paint.color = argb(Math.round(alpha * 255).toInt(), 179, 230, 255)
                    canvas.drawCircle((star.x * w).toFloat(), (star.y * h).toFloat(), (star.size * 0.8).toFloat(), paint)
                    i += 3
                }
            }
        }
    }

    private fun paintStars(canvas: Canvas, w: Double, h: Double, density: Double, brightness: Double) {
        val paint = Paint(Paint.ANTI_ALIAS_FLAG)
        val count = (stars.size * density).toInt()
        for (i in 0 until count) {
            val star = stars[i]
            val at = O(star.x * w, star.y * h)
            val shine = brightness * star.brightness * (0.62 + 0.38 * sin(star.phase))
            if (star.size > 1.6) drawWatchGlow(canvas, at, star.size * 4, argb(Math.round((0.35 * shine).coerceIn(0.0, 1.0) * 255).toInt(), 204, 222, 255))
            paint.color = argb(Math.round(shine.coerceIn(0.0, 1.0) * 255).toInt(), 235, 242, 255)
            canvas.drawCircle(at.x.toFloat(), at.y.toFloat(), (star.size * 0.6).toFloat(), paint)
        }
    }
}

internal class WatchLedStrip(val dots: IntArray, val width: Double)

internal class WatchLeds {
    private val strips = LinkedHashMap<String, WatchLedStrip>()

    fun stripFor(text: String): WatchLedStrip? {
        if (text.isEmpty()) return null
        strips[text]?.let { return it }
        val strip = build(text)
        strips[text] = strip
        if (strips.size > 64) strips.remove(strips.keys.first())
        return strip
    }

    private fun build(text: String): WatchLedStrip {
        val paint = Paint().apply {
            typeface = Typeface.MONOSPACE
            textSize = 8f
            color = 0xFFFFFFFF.toInt()
            isAntiAlias = true
        }
        val columns = max(1, ceil(paint.measureText(text).toDouble()).toInt() + 1)
        val rows = kWatchLedRows
        val bitmap = Bitmap.createBitmap(columns, rows, Bitmap.Config.ARGB_8888)
        Canvas(bitmap).drawText(text, 0f, (rows - 2).toFloat(), paint)
        val pixels = IntArray(columns * rows)
        bitmap.getPixels(pixels, 0, columns, 0, 0, columns, rows)
        bitmap.recycle()
        val dots = ArrayList<Int>()
        for (row in 0 until rows) {
            for (column in 0 until columns) {
                if ((pixels[row * columns + column] ushr 24) > 110) {
                    dots += column
                    dots += row
                }
            }
        }
        return WatchLedStrip(dots.toIntArray(), columns * kWatchLedPitch)
    }
}

internal class WatchFaces(private val load: (String) -> Bitmap?) {
    private val images = HashMap<String, Bitmap?>()

    fun face(name: String): Bitmap? = images.getOrPut(name) { load(name) }
}

internal class WatchFieldPainter(
    private val controller: AgentWatchController,
    private val layout: WatchFieldLayout,
    private val leds: WatchLeds,
    private val faces: (WatchedAgent) -> Bitmap?,
    private val hidden: Set<AgentMotion>,
    private val selected: WatchedAgent?,
    private val nodeLabel: String,
    private val text: WatchText,
    private val now: Double,
) {
    private val fill = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val stroke = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }

    fun paint(canvas: Canvas, width: Double, height: Double) {
        val painter = QtPainter(canvas)
        paintRows(canvas)
        paintDesktopNode(canvas, painter)
        paintPulses(canvas, painter)
        for (agent in controller.agents) {
            if (agent.presence > 0.01) layered(canvas, agent.presence) { paintBody(canvas, painter, agent) }
        }
        for (agent in controller.agents) {
            if (agent.presence <= 0.01 || agent.said.isEmpty()) continue
            val strip = leds.stripFor(agent.said) ?: continue
            layered(canvas, agent.presence) { paintLed(canvas, agent, strip) }
        }
        paintCard(canvas, painter, width, height)
    }

    private inline fun layered(canvas: Canvas, shown: Double, draw: () -> Unit) {
        if (shown < 0.99) {
            val count = canvas.saveLayerAlpha(null, (shown * 255).toInt().coerceIn(0, 255))
            draw()
            canvas.restoreToCount(count)
        } else {
            draw()
        }
    }

    private fun paintRows(canvas: Canvas) {
        val field = layout.bounds
        stroke.pathEffect = DashPathEffect(floatArrayOf(4f, 2f), 0f)
        stroke.strokeWidth = 1f
        stroke.color = WATCH_GUIDE
        val label = text.paint(10.0, true, WATCH_MUTED, 0.8)
        layout.rows.forEachIndexed { i, row ->
            if (row.count <= 0) return@forEachIndexed
            val rowTop = field.y + (row.y - row.height / 2) * field.height
            val top = rowTop - kWatchBandHeader * field.height
            if (i > 0) canvas.drawLine(field.x.toFloat(), top.toFloat(), field.maxX.toFloat(), top.toFloat(), stroke)
            val words = "${bandLabel(row.band)} · ${row.count}".uppercase()
            canvas.drawText(words, (field.x + 2).toFloat(), (rowTop - label.fontMetrics.descent - 1).toFloat(), label)
        }
        stroke.pathEffect = null
    }

    private fun paintDesktopNode(canvas: Canvas, painter: QtPainter) {
        val at = layout.desktopNode.o()
        val s = 9.0
        val colour = watchInk(0.58, 0.55, 0.78, 0.95)
        painter.save()
        painter.setPen(Pen(colour, max(1.0, s * 0.2), Dash.solid, Paint.Cap.ROUND, Paint.Join.ROUND))
        painter.setBrush(withAlpha(colour, alphaOf(colour) * 0.3))
        painter.drawRoundedRect(box(at.x - s, at.y - 0.8 * s, 2 * s, 1.25 * s), 0.18 * s, 0.18 * s)
        painter.drawLine(O(at.x, at.y + 0.45 * s), O(at.x, at.y + 0.8 * s))
        painter.drawLine(O(at.x - 0.5 * s, at.y + 0.8 * s), O(at.x + 0.5 * s, at.y + 0.8 * s))
        painter.restore()
        val label = text.paint(10.0, false, WATCH_MUTED)
        val w = label.measureText(nodeLabel)
        text.drawTop(canvas, label, nodeLabel, at.x - w / 2, at.y + 12)
    }

    private fun paintPulses(canvas: Canvas, painter: QtPainter) {
        for (pulse in controller.pulses) {
            if (pulse.pending(now) || pulse.motion in hidden) continue
            val agent = pulse.agent
            if (agent.presence <= 0.01) continue
            layered(canvas, agent.presence) {
                paintWatchFigure(painter, pulse.motion, layout.pointFor(agent).o(), layout.radiusFor(agent, now), pulse.life(now), pulse.angle, pulse.strength)
            }
        }
    }

    private fun paintBody(canvas: Canvas, painter: QtPainter, agent: WatchedAgent) {
        val centre = layout.pointFor(agent).o()
        val radius = layout.radiusFor(agent, now)
        val hovered = agent === selected
        val energy = agent.energyAt(now)
        val hue = agent.hue
        drawWatchGlow(canvas, centre, radius * (2.0 + 1.2 * energy), watchInk(hue, 0.95, 0.60, 0.10 + 0.50 * energy + (if (hovered) 0.12 else 0.0)))
        val ended = agent.state == WatchState.done || agent.state == WatchState.failed
        val light = O(centre.x - radius * 0.35, centre.y - radius * 0.4)
        val inner = watchInk(hue, if (ended) 0.35 else 0.75, if (ended) 0.45 else 0.70 + 0.15 * energy, if (ended) 0.8 else 1.0)
        val outer = watchInk(hue, if (ended) 0.3 else 0.7, if (ended) 0.18 else 0.30, if (ended) 0.8 else 1.0)
        fill.shader = radialShader(light, radius * 1.4, intArrayOf(inner, outer))
        canvas.drawCircle(centre.x.toFloat(), centre.y.toFloat(), radius.toFloat(), fill)
        fill.shader = null
        val outline = watchInk(hue, 0.95, 0.76, if (hovered) 1.0 else 0.7)
        val outlineWidth = if (hovered) 2.2f else 1.2f
        stroke.color = outline
        stroke.strokeWidth = outlineWidth
        stroke.strokeCap = Paint.Cap.BUTT
        canvas.drawCircle(centre.x.toFloat(), centre.y.toFloat(), radius.toFloat(), stroke)
        faces(agent)?.let { face ->
            val innerRadius = radius - (if (hovered) 1.1 else 0.6)
            val matrix = Matrix()
            val scale = (innerRadius * 2 / max(face.width, face.height)).toFloat()
            matrix.setScale(scale, scale)
            matrix.postTranslate((centre.x - innerRadius).toFloat(), (centre.y - innerRadius).toFloat())
            val shader = BitmapShader(face, Shader.TileMode.CLAMP, Shader.TileMode.CLAMP)
            shader.setLocalMatrix(matrix)
            fill.shader = shader
            fill.alpha = if (ended) 140 else 255
            canvas.drawCircle(centre.x.toFloat(), centre.y.toFloat(), innerRadius.toFloat(), fill)
            fill.shader = null
            fill.alpha = 255
            canvas.drawCircle(centre.x.toFloat(), centre.y.toFloat(), radius.toFloat(), stroke)
        }
        paintHistoryRing(painter, agent, centre, radius)
        if (agent.listening && controller.sound) {
            val beat = exp(-(now - agent.lastNoteSeconds) / 0.35)
            dottedCircle(canvas, centre, radius + 4 + 3 * beat, watchInk(hue, 1.0, 0.82, 0.45 + 0.5 * beat), 1.2 + 1.4 * beat, Paint.Cap.BUTT)
        }
        val running = agent.state == WatchState.running || agent.state == WatchState.starting
        if (running || agent.state == WatchState.waiting) {
            val colour = watchInk(hue, 1.0, 0.85, 0.55)
            if (running) {
                stroke.color = colour
                stroke.strokeWidth = 2f
                stroke.strokeCap = Paint.Cap.ROUND
                canvas.drawCircle(centre.x.toFloat(), centre.y.toFloat(), (radius + 15).toFloat(), stroke)
            } else {
                dottedCircle(canvas, centre, radius + 15, colour, 2.0, Paint.Cap.ROUND)
            }
        }
        if (agent.state == WatchState.failed) {
            stroke.color = argb(220, 248, 81, 73)
            stroke.strokeWidth = 2f
            canvas.drawCircle(centre.x.toFloat(), centre.y.toFloat(), (radius + 3).toFloat(), stroke)
        }
        if (agent.id > 0) {
            val tagPaint = text.paint(10.0, true, if (hovered) WATCH_FOREGROUND else watchInk(hue, 0.9, 0.82))
            val label = "${agent.id}"
            val tagHeight = text.lineHeight(tagPaint) + 2
            val tagWidth = max(tagHeight, tagPaint.measureText(label) + 8.0)
            val rect = box(centre.x - tagWidth / 2, centre.y - radius - tagHeight * 0.6, tagWidth, tagHeight)
            fill.color = argb(225, 8, 11, 20)
            canvas.drawRoundRect(rect, (tagHeight / 2).toFloat(), (tagHeight / 2).toFloat(), fill)
            stroke.color = watchInk(hue, 0.95, 0.76, if (hovered) 1.0 else 0.8)
            stroke.strokeWidth = 1f
            canvas.drawRoundRect(rect, (tagHeight / 2).toFloat(), (tagHeight / 2).toFloat(), stroke)
            val m = tagPaint.fontMetrics
            canvas.drawText(label, rect.centerX() - tagPaint.measureText(label) / 2, rect.centerY() - (m.ascent + m.descent) / 2, tagPaint)
        }
        val project = agent.target.projectName
        val task = watchTitleWithoutNumber(agent.id, agent.target.title)
        val ring = radius + kWatchLabelGap
        val gap = 0.45
        var projectSweep = 0.0
        val words = if (hovered) WATCH_FOREGROUND else if (ended) WATCH_MUTED else watchInk(hue, 0.5, 0.80)
        if (project.isNotEmpty()) {
            projectSweep = ringText(canvas, centre, ring, true, project, if (hovered || ended) words else watchInk(hue, 0.9, 0.78), true, PI)
        }
        if (task.isNotEmpty()) ringText(canvas, centre, ring, false, task, words, false, 2 * PI - projectSweep - 2 * gap)
    }

    private fun paintHistoryRing(painter: QtPainter, agent: WatchedAgent, centre: O, radius: Double) {
        val history = controller.history
        val ring = radius + 10.0
        val icon = (2.2 + radius * 0.06).coerceIn(2.6, 4.0)
        val marks = floor(2.0 * PI * ring / (icon * 3.0)).toInt().coerceIn(6, RING_MARKS)
        var drawn = 0
        var i = history.size - 1
        while (i >= 0 && drawn < marks) {
            val mark = history[i]
            i -= 1
            if (mark.agent !== agent || mark.motion in hidden) continue
            val at = -PI / 2 + drawn * (2.0 * PI / marks)
            val age = ((now - mark.seconds) / kHistorySeconds).coerceIn(0.0, 1.0)
            val alpha = (1.0 - age) * (1.0 - drawn.toDouble() / marks) * 0.9
            val size = if (mark.motion.isFaint) icon * 0.8 else icon * (0.85 + 0.3 * mark.strength)
            if (alpha > 0.01) paintWatchIcon(painter, mark.motion, centre + O(cos(at), sin(at)) * ring, size, alpha)
            drawn += 1
        }
    }

    private fun paintLed(canvas: Canvas, agent: WatchedAgent, strip: WatchLedStrip) {
        val centre = layout.pointFor(agent)
        val radius = layout.radiusFor(agent, now)
        val rect = layout.ledRect(agent, centre, radius)
        val r = box(rect.x, rect.y, rect.width, rect.height)
        fill.color = argb(235, 14, 10, 6)
        canvas.drawRoundRect(r, 3f, 3f, fill)
        stroke.color = watchInk(agent.hue, 0.6, 0.5, 0.7)
        stroke.strokeWidth = 1f
        canvas.drawRoundRect(r, 3f, 3f, stroke)
        val left = rect.x + kWatchLedPad
        val top = rect.y + kWatchLedPad
        val windowWidth = rect.width - 2 * kWatchLedPad
        val right = left + windowWidth
        canvas.save()
        canvas.clipRect(left.toFloat(), top.toFloat(), right.toFloat(), (rect.y + rect.height - kWatchLedPad).toFloat())
        val dot = (kWatchLedPitch * 0.36).toFloat()
        val columns = ceil(windowWidth / kWatchLedPitch).toInt()
        fill.color = argb(36, 255, 150, 40)
        for (row in 0 until kWatchLedRows) {
            for (column in 0 until columns) {
                canvas.drawCircle((left + (column + 0.5) * kWatchLedPitch).toFloat(), (top + (row + 0.5) * kWatchLedPitch).toFloat(), dot, fill)
            }
        }
        val lefts = ArrayList<Double>()
        if (strip.width <= windowWidth) {
            lefts += left + floor((windowWidth - strip.width) / 2 / kWatchLedPitch) * kWatchLedPitch
        } else {
            val first = left - scroll(agent, strip.width)
            lefts += first
            val again = first + strip.width + kWatchLedGapDots * kWatchLedPitch
            if (again < right) lefts += again
        }
        val haloRadius = dot * 1.5f
        for (pass in 0 until 2) {
            fill.color = if (pass == 0) argb(50, 255, 140, 0) else argb(255, 255, 190, 60)
            for (start in lefts) {
                var i = 0
                while (i + 1 < strip.dots.size) {
                    val x = start + (strip.dots[i] + 0.5) * kWatchLedPitch
                    if (x >= left - kWatchLedPitch && x <= right + kWatchLedPitch) {
                        val y = top + (strip.dots[i + 1] + 0.5) * kWatchLedPitch
                        canvas.drawCircle(x.toFloat(), y.toFloat(), if (pass == 0) haloRadius else dot, fill)
                    }
                    i += 2
                }
            }
        }
        canvas.restore()
    }

    private fun scroll(agent: WatchedAgent, width: Double): Double {
        val t = now - agent.saidSeconds - kWatchLedHold
        if (t <= 0) return 0.0
        val moved = (t * kWatchLedSpeed) % (width + kWatchLedGapDots * kWatchLedPitch)
        return floor(moved / kWatchLedPitch) * kWatchLedPitch
    }

    private fun paintCard(canvas: Canvas, painter: QtPainter, width: Double, height: Double) {
        val agent = selected ?: return
        if (agent.presence <= 0.01) return
        val lines = watchCardLines(controller, agent)
        val base = text.paint(11.0, false, WATCH_MUTED)
        val lineHeight = text.lineHeight(base) + 3
        val maxWidth = min(420.0, width - 28)
        var widest = 0.0
        val fitted = lines.map { line ->
            val fit = text.fit(base, line, maxWidth)
            widest = max(widest, base.measureText(fit).toDouble())
            fit
        }
        val keyRowsMax = 12
        val kinds = controller.kindsOf(agent)
        val keyRows = min(kinds.size, keyRowsMax)
        val keyMore = kinds.size > keyRowsMax
        val keyTotal = kinds.sumOf { it.second }
        val heading = "its marks: $keyTotal in all, ${kinds.size} kind${if (kinds.size == 1) "" else "s"}"
        val iconRoom = 20.0
        var countWidth = 0.0
        var labelWidth = 0.0
        for (i in 0 until keyRows) {
            countWidth = max(countWidth, base.measureText("${kinds[i].second}").toDouble())
            labelWidth = max(labelWidth, base.measureText(kinds[i].first.label).toDouble())
        }
        if (keyRows > 0) {
            widest = max(widest, base.measureText(heading).toDouble())
            for (i in 0 until keyRows) {
                val row = iconRoom + countWidth + 8 + labelWidth + 10 + base.measureText(kinds[i].first.description)
                widest = max(widest, min(maxWidth, row))
            }
        }
        val keyLines = if (keyRows > 0) keyRows + 1 + (if (keyMore) 1 else 0) else 0
        val w = min(widest + 20, width - 8)
        val h = lineHeight * lines.size + (if (keyLines > 0) lineHeight * keyLines + 6 else 0.0) + 14
        val anchor = layout.pointFor(agent)
        val gap = layout.radiusFor(agent, now) + 20
        var left = anchor.x + gap
        if (left + w > width - 8) left = anchor.x - gap - w
        left = left.coerceIn(4.0, max(4.0, width - w - 4))
        val top = (anchor.y - h / 2).coerceIn(4.0, max(4.0, height - h - 4))
        val card = box(left, top, w, h)
        fill.color = argb(235, 10, 14, 24)
        canvas.drawRoundRect(card, 7f, 7f, fill)
        stroke.color = watchInk(agent.hue, 0.8, 0.6, 0.6)
        stroke.strokeWidth = 1f
        canvas.drawRoundRect(card, 7f, 7f, stroke)
        val pad = (lineHeight - text.lineHeight(base)) / 2
        var y = top + 7
        fitted.forEachIndexed { i, line ->
            val paint = text.paint(11.0, false, if (i == 0) WATCH_FOREGROUND else WATCH_MUTED)
            text.drawTop(canvas, paint, line, left + 10, y + pad)
            y += lineHeight
        }
        if (keyRows == 0) return
        y += 3
        stroke.color = WATCH_GUIDE
        canvas.drawLine((left + 10).toFloat(), y.toFloat(), (left + w - 10).toFloat(), y.toFloat(), stroke)
        y += 3
        text.drawTop(canvas, text.paint(11.0, false, WATCH_MUTED), heading, left + 10, y + pad)
        y += lineHeight
        for (i in 0 until keyRows) {
            val (motion, count) = kinds[i]
            var x = left + 10
            paintWatchIcon(painter, motion, O(x + 7, y + lineHeight / 2), 6.0, 1.0)
            x += iconRoom
            val numberPaint = text.paint(11.0, false, WATCH_FOREGROUND)
            val number = "$count"
            text.drawTop(canvas, numberPaint, number, x + countWidth - numberPaint.measureText(number), y + pad)
            x += countWidth + 8
            text.drawTop(canvas, text.paint(11.0, false, watchInk(motion.hue, 0.8, 0.72)), motion.label, x, y + pad)
            x += labelWidth + 10
            val room = left + w - 10 - x
            if (room > 12) {
                val meaningPaint = text.paint(11.0, false, WATCH_MUTED)
                text.drawTop(canvas, meaningPaint, text.fit(meaningPaint, motion.description, room), x, y + pad)
            }
            y += lineHeight
        }
        if (keyMore) {
            var rest = 0
            for (i in keyRowsMax until kinds.size) rest += kinds[i].second
            val morePaint = text.paint(11.0, false, WATCH_MUTED)
            text.drawTop(canvas, morePaint, text.fit(morePaint, "and $rest more of ${kinds.size - keyRowsMax} other kinds", w - 20 - iconRoom), left + 10 + iconRoom, y + pad)
        }
    }

    private fun ringText(canvas: Canvas, centre: O, inner: Double, top: Boolean, words: String, colour: Int, bold: Boolean, sweep: Double): Double {
        val paint = text.paint(kWatchLabelPixels, bold, colour)
        val metrics = paint.fontMetrics
        val ascent = -metrics.ascent.toDouble()
        val descent = metrics.descent.toDouble()
        val baseline = inner + (if (top) descent else ascent)
        val spacing = if (bold) 1.1 else 1.0
        fun advance(letter: String) = paint.measureText(letter) * spacing
        val letters = words.map { it.toString() }.toMutableList()
        val room = sweep * baseline
        var total = letters.sumOf { advance(it) }
        if (total > room) {
            val dots = advance("…")
            while (letters.isNotEmpty() && total + dots > room) total -= advance(letters.removeAt(letters.size - 1))
            if (letters.isEmpty()) return 0.0
            letters += "…"
            total += dots
        }
        if (letters.isEmpty()) return 0.0
        val direction = if (top) 1.0 else -1.0
        val taken = total / baseline
        var at = (if (top) -PI / 2 else PI / 2) - direction * taken / 2
        for (letter in letters) {
            val w = advance(letter)
            val angle = at + direction * w / baseline / 2
            at += direction * w / baseline
            if (letter.isBlank()) continue
            canvas.save()
            canvas.translate((centre.x + cos(angle) * baseline).toFloat(), (centre.y + sin(angle) * baseline).toFloat())
            canvas.rotate(((angle + direction * PI / 2) * 180 / PI).toFloat())
            canvas.drawText(letter, -paint.measureText(letter) / 2, 0f, paint)
            canvas.restore()
        }
        return taken
    }

    private fun dottedCircle(canvas: Canvas, centre: O, radius: Double, colour: Int, penWidth: Double, cap: Paint.Cap) {
        val circumference = 2 * PI * radius
        val period = penWidth * 3
        val count = max(1, floor(circumference / period).toInt())
        val sweep = (penWidth / radius * 180 / PI).toFloat()
        stroke.color = colour
        stroke.strokeWidth = penWidth.toFloat()
        stroke.strokeCap = cap
        val rect = circleRect(centre, radius)
        for (i in 0 until count) {
            val start = (i * 360.0 / count).toFloat()
            canvas.drawArc(rect, start, sweep, false, stroke)
        }
        stroke.strokeCap = Paint.Cap.BUTT
    }

    companion object {
        fun ledsMoving(controller: AgentWatchController, leds: WatchLeds, layout: WatchFieldLayout): Boolean {
            for (agent in controller.agents) {
                if (agent.presence <= 0.01 || agent.said.isEmpty()) continue
                val strip = leds.stripFor(agent.said) ?: continue
                val centre = layout.pointFor(agent)
                val rect = layout.ledRect(agent, centre, layout.radiusFor(agent, controller.now))
                if (strip.width > rect.width - 2 * kWatchLedPad) return true
            }
            return false
        }
    }
}
