package com.forkmesh.nativeapp.feature.agentwatch

import android.graphics.Canvas
import android.graphics.Paint
import com.forkmesh.nativeapp.agentwatch.AgentMotion
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow
import kotlin.math.sin

private val ROUND = Paint.Cap.ROUND
private val SQUARE = Paint.Cap.SQUARE
private val JROUND = Paint.Join.ROUND
private val MITER = Paint.Join.MITER

private fun paintCueGlyph(painter: QtPainter, motion: AgentMotion, at: O, size: Double, colour: Int, turn: Double) {
    val pen = Pen(colour, max(1.0, size * 0.26), Dash.solid, ROUND, JROUND)
    val fill = withAlpha(colour, alphaOf(colour) * 0.3)
    painter.save()
    painter.translate(at)
    when (motion) {
        AgentMotion.connect -> {
            painter.noPen()
            painter.setBrush(colour)
            painter.drawEllipse(O(0.0, size * 0.55), size * 0.22, size * 0.22)
            painter.setPen(pen)
            painter.noBrush()
            for (k in 1..2) {
                val radius = size * 0.5 * k
                painter.drawArc(box(-radius, size * 0.55 - radius, radius * 2, radius * 2), 45 * 16, 90 * 16)
            }
        }
        AgentMotion.task -> {
            painter.rotate(turn * 180.0)
            painter.setPen(pen)
            painter.setBrush(fill)
            painter.drawRect(box(-size * 0.6, -size * 0.6, size * 1.2, size * 1.2))
        }
        AgentMotion.logError -> {
            painter.rotate(turn * 30.0)
            painter.setPen(Pen(colour, max(1.2, size * 0.34), Dash.solid, ROUND))
            painter.drawLine(O(-size * 0.7, -size * 0.7), O(size * 0.7, size * 0.7))
            painter.drawLine(O(-size * 0.7, size * 0.7), O(size * 0.7, -size * 0.7))
        }
        AgentMotion.logStall -> {
            painter.setPen(pen)
            painter.setBrush(fill)
            painter.drawPolygon(listOf(O(-size * 0.6, -size * 0.8), O(size * 0.6, -size * 0.8), O(-size * 0.6, size * 0.8), O(size * 0.6, size * 0.8)))
        }
        AgentMotion.logSync -> {
            painter.rotate(turn * 180.0)
            painter.setPen(pen)
            painter.noBrush()
            val b = box(-size * 0.75, -size * 0.75, size * 1.5, size * 1.5)
            for (k in 0 until 2) {
                painter.drawArc(b, (k * 180 + 20) * 16, 130 * 16)
                val tip = (k * 180 + 150) * PI / 180.0
                val end = O(cos(tip) * size * 0.75, -sin(tip) * size * 0.75)
                val back = O(cos(tip - 0.5) * size * 1.0, -sin(tip - 0.5) * size * 1.0)
                painter.drawLine(end, back)
            }
        }
        AgentMotion.logGit -> {
            painter.setPen(pen)
            painter.drawLine(O(-size * 0.4, size * 0.8), O(-size * 0.4, -size * 0.8))
            painter.drawLine(O(-size * 0.4, size * 0.2), O(size * 0.45, -size * 0.3))
            painter.noPen()
            painter.setBrush(colour)
            for (node in listOf(O(-size * 0.4, size * 0.8), O(-size * 0.4, -size * 0.8), O(size * 0.45, -size * 0.3))) {
                painter.drawEllipse(node, size * 0.24, size * 0.24)
            }
        }
        AgentMotion.logCloud -> {
            painter.noPen()
            painter.setBrush(colour)
            painter.drawEllipse(O(-size * 0.45, size * 0.15), size * 0.42, size * 0.38)
            painter.drawEllipse(O(0.0, -size * 0.15), size * 0.5, size * 0.48)
            painter.drawEllipse(O(size * 0.45, size * 0.15), size * 0.42, size * 0.38)
            painter.drawRect(box(-size * 0.45, size * 0.1, size * 0.9, size * 0.43))
        }
        AgentMotion.logTask -> {
            painter.setPen(pen)
            painter.setBrush(fill)
            painter.drawRect(box(-size * 0.55, -size * 0.75, size * 1.1, size * 1.5))
            painter.drawLine(O(-size * 0.25, -size * 0.25), O(size * 0.25, -size * 0.25))
            painter.drawLine(O(-size * 0.25, size * 0.2), O(size * 0.25, size * 0.2))
        }
        AgentMotion.logAccount -> {
            painter.rotate(-30.0 + turn * 40.0)
            painter.setPen(pen)
            painter.noBrush()
            painter.drawEllipse(O(-size * 0.45, 0.0), size * 0.35, size * 0.35)
            painter.drawLine(O(-size * 0.1, 0.0), O(size * 0.8, 0.0))
            painter.drawLine(O(size * 0.55, 0.0), O(size * 0.55, size * 0.3))
            painter.drawLine(O(size * 0.8, 0.0), O(size * 0.8, size * 0.3))
        }
        AgentMotion.logInfo -> {
            painter.noPen()
            painter.setBrush(colour)
            painter.drawEllipse(O(0.0, -size * 0.6), size * 0.2, size * 0.2)
            painter.setPen(pen)
            painter.drawLine(O(0.0, -size * 0.15), O(0.0, size * 0.75))
        }
        else -> {}
    }
    painter.restore()
}

internal fun paintWatchFigure(painter: QtPainter, motion: AgentMotion, c: O, r: Double, t: Double, angle: Double, s: Double) {
    val canvas: Canvas = painter.canvas
    val hue = motion.hue
    val fade = (1.0 - t).pow(1.2) * (0.35 + 0.65 * s)
    if (fade <= 0.01) return
    val a = angle * 2.0 * PI
    val dir = O(cos(a), sin(a))
    val normal = O(-dir.y, dir.x)
    val line = watchInk(hue, 0.92, 0.66, fade)
    val lit = watchInk(hue, 1.0, 0.80, fade)
    val width = max(0.8, motion.weight * (0.9 + 1.4 * s) * (1.0 - 0.5 * t))
    val pen = Pen(line, width, Dash.solid, ROUND, JROUND)
    fun circleBox(centre: O, radius: Double) = box(centre.x - radius, centre.y - radius, radius * 2, radius * 2)

    when (motion) {
        AgentMotion.start -> {
            painter.noPen()
            painter.setBrush(watchInk(hue, 0.9, 0.66, fade * 0.8))
            val reach = r * (1.0 + 1.2 * easeOut(t))
            val petals = 6
            for (i in 0 until petals) {
                val at = a + i * 2.0 * PI / petals + t * 0.6
                painter.save()
                painter.translate(polar(c, reach, at))
                painter.rotate(at * 180.0 / PI)
                val len = r * 0.42 * (1.0 - 0.4 * t)
                painter.drawEllipse(O.zero, len, len * 0.38)
                painter.restore()
            }
            painter.setPen(pen)
            painter.noBrush()
            val ring = r * (1.0 + 1.9 * easeOut(t))
            painter.drawEllipse(c, ring, ring)
        }
        AgentMotion.speak -> {
            painter.noBrush()
            for (k in 0 until 3) {
                val tk = (t - k * 0.14) / (1.0 - 0.28)
                if (tk <= 0 || tk >= 1) continue
                painter.setPen(pen.withColour(watchInk(hue, 0.92, 0.70, fade * (1.0 - tk))))
                val radius = r * (1.15 + 1.6 * tk)
                val mid = -a * 180.0 / PI
                painter.drawArc(circleBox(c, radius), ((mid - 32) * 16).toInt(), 64 * 16)
            }
        }
        AgentMotion.think -> {
            val motes = 2
            for (k in 0 until motes) {
                val at = a + k * 2.0 * PI / motes + t * 2.4
                val radius = r * (1.35 + 0.22 * sin(t * 6.0 + k * 2.0))
                painter.setPen(Pen(watchInk(hue, 0.8, 0.72, fade * 0.6), 1.4, Dash.solid, ROUND))
                painter.noBrush()
                painter.drawArc(circleBox(c, radius), (-at * 180.0 / PI * 16).toInt(), 40 * 16)
                painter.noPen()
                painter.setBrush(lit)
                val dot = 1.8 + 1.6 * s
                painter.drawEllipse(polar(c, radius, at), dot, dot)
            }
        }
        AgentMotion.read -> {
            val out = r * (1.0 + 1.7 * easeOut(min(1.0, t * 2.2)))
            var tip = c + dir * out
            if (t > 0.5) tip = c + dir * (r + (out - r) * (1.0 - easeInOut((t - 0.5) * 2.0)))
            painter.setPen(Pen(watchInk(hue, 0.9, 0.7, fade * 0.7), 1.2, Dash.solid, ROUND))
            painter.drawLine(c + dir * r, tip)
            painter.save()
            painter.translate(tip)
            painter.rotate(a * 180.0 / PI + t * 90.0)
            val half = 2.5 + 2.5 * s
            painter.setPen(Pen(lit, 1.0))
            painter.setBrush(watchInk(hue, 0.9, 0.55, fade * 0.5))
            painter.drawRect(box(-half, -half * 1.3, half * 2, half * 2.6))
            painter.restore()
        }
        AgentMotion.search -> {
            val radius = r * (1.2 + 1.3 * easeOut(t))
            val start = -a * 180.0 / PI - t * 220.0
            val edge = watchInk(hue, 0.95, 0.66, fade * 0.55)
            painter.noPen()
            painter.setBrushShader(radialShader(c, radius, intArrayOf(edge and 0xffffff, edge)))
            painter.drawPie(circleBox(c, radius), (start * 16).toInt(), 34 * 16)
            painter.setPen(pen.withDash(Dash.dash).withWidth(max(0.8, width * 0.7)))
            painter.noBrush()
            painter.drawEllipse(c, radius, radius)
        }
        AgentMotion.web -> {
            val out = r * (1.2 + 2.0 * easeOut(min(1.0, t * 2.0)))
            val at = c + dir * (if (t < 0.6) out else r + (out - r) * (1.0 - easeInOut((t - 0.6) / 0.4)))
            painter.setPen(Pen(watchInk(hue, 0.9, 0.7, fade * 0.6), 1.0, Dash.dot, ROUND))
            painter.drawLine(c + dir * r, at)
            val g = 3.5 + 3.0 * s
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 0.9, 0.5, fade * 0.25))
            painter.drawEllipse(at, g, g)
            painter.noBrush()
            val turn = abs(cos(t * 6.0))
            painter.drawEllipse(at, g * turn, g)
            painter.drawLine(at + O(-g, 0.0), at + O(g, 0.0))
        }
        AgentMotion.plan -> {
            val origin = polar(c, r * 1.5, a) + O(0.0, -r * 0.5)
            val row = 4.0 + 1.5 * s
            for (k in 0 until 3) {
                val appear = (t * 3.2 - k * 0.6).coerceIn(0.0, 1.0)
                if (appear <= 0) continue
                val at = origin + O(0.0, k * row * 1.5)
                painter.setPen(Pen(watchInk(hue, 0.9, 0.7, fade * appear), 1.1, Dash.solid, ROUND, JROUND))
                painter.noBrush()
                painter.drawRect(box(at.x, at.y - row * 0.5, row, row))
                painter.drawLine(at + O(row * 1.5, 0.0), at + O(row * 1.5 + row * 2.2 * appear, 0.0))
                if (appear >= 1.0) {
                    painter.setPen(Pen(lit, 1.4, Dash.solid, ROUND, JROUND))
                    painter.drawPolyline(listOf(at + O(row * 0.15, 0.0), at + O(row * 0.45, row * 0.35), at + O(row * 1.0, -row * 0.45)))
                }
            }
        }
        AgentMotion.write -> {
            val half = r * (0.8 + 0.9 * easeOut(t))
            painter.save()
            painter.translate(c)
            painter.rotate(45.0 + t * 35.0 + angle * 90.0)
            painter.setPen(pen)
            painter.noBrush()
            painter.drawRect(box(-half, -half, half * 2, half * 2))
            painter.noPen()
            painter.setBrush(lit)
            for (i in 0 until 4) {
                val corner = O(if (i % 2 != 0) half else -half, if (i / 2 != 0) half else -half)
                val spark = 1.4 + 1.8 * s * (1.0 - t)
                painter.drawEllipse(corner, spark, spark)
            }
            painter.restore()
            painter.noPen()
            painter.setBrush(watchInk(hue, 0.9, 0.7, fade * 0.8))
            for (i in 0 until 3) {
                val drop = O(c.x + (i - 1) * r * 0.45, c.y + r + t * r * (1.2 + 0.4 * i))
                painter.drawEllipse(drop, 1.6, 2.2)
            }
        }
        AgentMotion.run -> {
            val reach = r * (1.0 + 1.9 * easeOut(min(1.0, t * 3.0)))
            val flicker = (t * 14.0).toInt()
            val from = c + dir * r
            val bolt = evenOddPath().apply { moveTo(from.x.toFloat(), from.y.toFloat()) }
            for (i in 1..5) {
                val u = i / 5.0
                val side = if (i == 5) 0.0 else (if ((i + flicker) % 2 != 0) 1.0 else -1.0) * r * (0.16 + 0.1 * scatter(i * 31 + flicker))
                val to = c + dir * (r + (reach - r) * u) + normal * side
                bolt.lineTo(to.x.toFloat(), to.y.toFloat())
            }
            painter.noBrush()
            painter.setPen(Pen(watchInk(hue, 1.0, 0.85, fade * 0.35), width * 3.0, Dash.solid, ROUND, JROUND))
            painter.drawPath(bolt)
            painter.setPen(pen)
            painter.drawPath(bolt)
            painter.noPen()
            painter.setBrush(lit)
            val tip = c + dir * reach
            for (i in 0 until 3) {
                val at = a + (i - 1) * 0.7
                painter.drawEllipse(polar(tip, r * 0.5 * easeOut(t), at), 1.3, 1.3)
            }
        }
        AgentMotion.delegate -> {
            val from = c + dir * r
            val to = c + dir * r * 3.4 + normal * r * 0.8
            val control = c + dir * r * 2.0 - normal * r * 1.2
            painter.noPen()
            for (k in 6 downTo 0) {
                val u = easeOut(t) - k * 0.035
                if (u <= 0) continue
                painter.setBrush(watchInk(hue, 0.9, 0.72, fade * (1.0 - k / 7.0)))
                val dot = (2.2 + 2.2 * s) * (1.0 - k / 9.0)
                painter.drawEllipse(onQuad(from, control, to, u), dot, dot)
            }
            if (t > 0.75) {
                painter.setPen(pen)
                painter.noBrush()
                val pop = r * 0.9 * (t - 0.75) * 4.0
                painter.drawEllipse(to, pop, pop)
            }
        }
        AgentMotion.arrive -> {
            val radius = r * (1.0 + 2.1 * (1.0 - easeOut(t)))
            val motes = 6
            for (i in 0 until motes) {
                val at = a + i * 2.0 * PI / motes
                val head = polar(c, radius, at)
                painter.setPen(Pen(watchInk(hue, 0.9, 0.7, fade * 0.55), 1.1, Dash.solid, ROUND))
                painter.drawLine(head, polar(c, radius + r * 0.45 * (1.0 - t), at))
                painter.noPen()
                painter.setBrush(lit)
                painter.drawEllipse(head, 1.4 + 1.2 * s, 1.4 + 1.2 * s)
            }
        }
        AgentMotion.prompt -> {
            val fall = easeInOut(min(1.0, t * 1.7))
            val from = c + O(-r * 1.2 + 2.4 * r * angle, -r * 4.5)
            val head = from + (c - from) * fall
            if (fall < 1.0) {
                val tail = linearShader(from, head, intArrayOf(watchInk(hue, 1.0, 0.8, 0.0), watchInk(hue, 1.0, 0.8, fade)))
                painter.setPen(Pen(0, 2.2 + 1.6 * s, Dash.solid, ROUND, Paint.Join.BEVEL, tail))
                painter.drawLine(from + (head - from) * 0.4, head)
                painter.noPen()
                drawWatchGlow(canvas, head, 7 + 5 * s, watchInk(hue, 1.0, 0.85, fade))
            } else {
                val u = (t - 1.0 / 1.7) / (1.0 - 1.0 / 1.7)
                painter.setPen(pen)
                painter.noBrush()
                val ring = r * (1.0 + 2.2 * easeOut(u))
                painter.drawEllipse(c, ring, ring)
            }
        }
        AgentMotion.notice -> {
            val shake = sin(t * 70.0) * 2.5 * (1.0 - t)
            val radius = r * (1.25 + 1.2 * easeOut(t))
            val triangle = (0 until 3).map { polar(c + O(shake, 0.0), radius, -PI / 2 + it * 2.0 * PI / 3.0) }
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 0.9, 0.5, fade * 0.18))
            painter.drawPolygon(triangle)
        }
        AgentMotion.finish -> {
            val burst = r * (1.0 + 3.0 * easeOut(t))
            painter.noPen()
            val sparks = 12
            for (i in 0 until sparks) {
                val at = a + i * 2.0 * PI / sparks
                val p = polar(c, burst * (0.8 + 0.2 * scatter(i * 13)), at) + O(0.0, t * t * r * 1.4)
                painter.setBrush(watchInk(hue + 0.04 * (i % 3), 1.0, 0.78, fade))
                painter.drawEllipse(p, 1.5 + 1.4 * s, 1.5 + 1.4 * s)
            }
            painter.setPen(pen)
            painter.noBrush()
            val ring = r * (1.0 + 1.6 * easeOut(t))
            painter.drawEllipse(c, ring, ring)
        }
        AgentMotion.frame -> {
            val at = polar(c, r * 1.35, a)
            val half = (3.0 + 2.0 * s) * (1.0 - 0.3 * t)
            val gap = half * (1.4 - 0.8 * easeOut(t))
            painter.save()
            painter.translate(at)
            painter.rotate(a * 180.0 / PI)
            painter.setPen(Pen(lit, 1.1, Dash.solid, SQUARE, MITER))
            painter.noBrush()
            for (side in listOf(-1.0, 1.0)) {
                painter.drawPolyline(listOf(O(side * (gap - half * 0.4), -half), O(side * gap, -half), O(side * gap, half), O(side * (gap - half * 0.4), half)))
            }
            painter.restore()
        }
        AgentMotion.compose -> {
            painter.noPen()
            val shown = ((t * 4.0).toInt() + 1).coerceIn(1, 3)
            for (k in 0 until shown) {
                val at = polar(c, r * (1.3 + 0.1 * k), a + k * 0.28)
                painter.setBrush(watchInk(hue, 0.9, 0.74, fade * (1.0 - 0.2 * k)))
                val dot = 1.3 + 0.9 * s
                painter.drawEllipse(at, dot, dot)
            }
        }
        AgentMotion.usage -> {
            val rise = easeOut(min(1.0, t * 2.5))
            val base = c + O(-r * 0.5, r * 1.25)
            painter.noPen()
            for (k in 0 until 3) {
                val height = r * (0.25 + 0.2 * k) * rise * (0.5 + 0.5 * s)
                painter.setBrush(watchInk(hue, 0.9, 0.62 + 0.06 * k, fade * 0.9))
                painter.drawRect(box(base.x + k * r * 0.38, base.y + r * 0.7 - height, max(1.6, r * 0.24), height))
            }
        }
        AgentMotion.system -> {
            val at = polar(c, r * 1.5, a)
            val radius = (3.2 + 2.0 * s) * (1.0 + 0.3 * easeOut(t))
            painter.save()
            painter.translate(at)
            painter.rotate(t * 120.0)
            painter.setPen(Pen(lit, 1.1, Dash.solid, ROUND))
            painter.noBrush()
            painter.drawEllipse(O.zero, radius * 0.55, radius * 0.55)
            for (i in 0 until 6) {
                val at2 = i * PI / 3.0
                painter.drawLine(polar(O.zero, radius * 0.6, at2), polar(O.zero, radius, at2))
            }
            painter.restore()
        }
        AgentMotion.tick -> {
            val at = polar(c, r * 1.4, a)
            val size = (2.5 + 2.5 * s) * (1.0 - 0.5 * t)
            painter.save()
            painter.translate(at)
            painter.rotate(t * 90.0)
            painter.setPen(Pen(lit, 1.0, Dash.solid, ROUND))
            painter.drawLine(O(-size, 0.0), O(size, 0.0))
            painter.drawLine(O(0.0, -size), O(0.0, size))
            painter.restore()
        }
        AgentMotion.network -> {
            painter.noPen()
            for (k in 0 until 2) {
                val at = a + k * PI + t * 5.0
                val radius = r * (1.1 + 1.2 * t)
                painter.setBrush(watchInk(hue, 0.9, 0.78, fade))
                painter.drawEllipse(polar(c, radius, at), 1.6, 1.6)
            }
        }
        AgentMotion.scan -> {
            val dots = 5
            painter.noPen()
            for (k in 0 until dots) {
                val appear = (t * (dots + 2) * 0.5 - k * 0.5).coerceIn(0.0, 1.0)
                if (appear <= 0) continue
                val at = a - 0.6 + 1.2 * k / max(1, dots - 1)
                painter.setBrush(watchInk(hue, 0.9, 0.72, fade * appear))
                painter.drawEllipse(polar(c, r * (1.7 + 0.5 * easeOut(t)), at), 1.5 + s, 1.5 + s)
            }
        }
        AgentMotion.create -> {
            val at = c + dir * r * (1.6 + 1.3 * easeOut(t))
            painter.setPen(Pen(watchInk(hue, 0.9, 0.7, fade * 0.5), 1.0, Dash.dot))
            painter.drawLine(c + dir * r, at)
            val size = (4.0 + 5.0 * s) * easeOut(min(1.0, t * 2.5))
            val star = evenOddPath()
            for (i in 0 until 8) {
                val at2 = a + t * 1.5 + i * PI / 4.0
                val p = polar(at, if (i % 2 != 0) size * 0.3 else size, at2)
                if (i == 0) star.moveTo(p.x.toFloat(), p.y.toFloat()) else star.lineTo(p.x.toFloat(), p.y.toFloat())
            }
            star.close()
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 1.0, 0.75, fade * 0.7))
            painter.drawPath(star)
        }
        AgentMotion.test -> {
            val swing = sin(t * 2.0 * PI * 3.0) * 0.55
            val pivot = c + dir * r
            val bob = polar(pivot, r * (0.9 + 0.5 * s), a + swing)
            painter.setPen(Pen(watchInk(hue, 0.8, 0.7, fade * 0.6), 1.0))
            painter.drawLine(pivot, bob)
            painter.noPen()
            painter.setBrush(lit)
            painter.drawEllipse(bob, 2.4 + 1.6 * s, 2.4 + 1.6 * s)
            painter.setPen(Pen(lit, 1.3, Dash.solid, ROUND, JROUND))
            painter.noBrush()
            for (side in listOf(-0.55, 0.55)) {
                val end = polar(pivot, r * 1.7, a + side)
                painter.drawPolyline(listOf(end + O(-2.5, 0.0), end + O(-0.5, 2.2), end + O(3.0, -2.6)))
            }
        }
        AgentMotion.build -> {
            val bricks = 3
            val w = r * 0.55
            val hgt = r * 0.24
            val base = c + dir * r * 1.9
            for (k in 0 until bricks) {
                val land = (t * bricks * 1.6 - k).coerceIn(0.0, 1.0)
                if (land <= 0) continue
                val drop = (1.0 - easeOut(land)) * r * 1.2
                val brick = box(base.x - w / 2 + (k % 2) * w * 0.25, base.y - (k + 1) * hgt * 1.15 - drop, w, hgt)
                painter.setPen(Pen(lit, 1.0))
                painter.setBrush(watchInk(hue, 0.8, 0.5, fade * 0.7))
                painter.drawRect(brick)
            }
            painter.setPen(Pen(watchInk(hue, 0.9, 0.65, fade * (1.0 - t)), 2.0))
            painter.noBrush()
            val ring = r * (1.0 + 0.6 * easeOut((t * bricks) % 1.0))
            painter.drawEllipse(c, ring, ring)
        }
        AgentMotion.git -> {
            val grow = easeOut(min(1.0, t * 1.8))
            val root = c + dir * r
            val fork = root + dir * r * 0.9 * grow
            painter.setPen(pen)
            painter.drawLine(root, fork)
            painter.setBrush(lit)
            val branches = 2
            for (k in 0 until branches) {
                val spread = (k - (branches - 1) / 2.0) * 0.6
                val tip = polar(fork, r * 1.1 * grow, a + spread)
                painter.setPen(pen)
                painter.drawLine(fork, tip)
                painter.noPen()
                painter.drawEllipse(tip, 2.2 + s, 2.2 + s)
            }
            painter.drawEllipse(fork, 1.8 + s, 1.8 + s)
        }
        AgentMotion.install -> {
            val fall = easeInOut(min(1.0, t * 1.5))
            val from = c + dir * r * 3.4
            val at = from + (c + dir * r * 1.1 - from) * fall
            val size = 3.5 + 3.0 * s
            painter.save()
            painter.translate(at)
            painter.rotate(a * 180.0 / PI + (1.0 - fall) * 120.0)
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 0.8, 0.5, fade * 0.5))
            painter.drawRect(box(-size, -size, size * 2, size * 2))
            painter.drawLine(O(-size, 0.0), O(size, 0.0))
            painter.restore()
        }
        AgentMotion.spawn -> {
            val u = easeOut(t)
            val at = a + u * 2.4
            val p = polar(c, r * (1.2 + 2.2 * u), at)
            painter.setPen(Pen(watchInk(hue, 0.9, 0.7, fade * 0.4), 1.0, Dash.dot))
            painter.noBrush()
            val first = polar(c, r * 1.2, a)
            val trail = evenOddPath().apply { moveTo(first.x.toFloat(), first.y.toFloat()) }
            for (k in 1..12) {
                val v = u * k / 12.0
                val next = polar(c, r * (1.2 + 2.2 * v), a + v * 2.4)
                trail.lineTo(next.x.toFloat(), next.y.toFloat())
            }
            painter.drawPath(trail)
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 0.9, 0.6, fade * 0.6))
            painter.drawEllipse(p, r * 0.28, r * 0.28)
        }
        AgentMotion.wait -> {
            val radius = r * 1.55
            painter.setPen(Pen(watchInk(hue, 0.7, 0.6, fade * 0.35), 1.0, Dash.dot))
            painter.noBrush()
            painter.drawEllipse(c, radius, radius)
            painter.setPen(pen.withWidth(width * 1.4))
            painter.drawArc(circleBox(c, radius), 90 * 16, (-360.0 * easeInOut(t) * 16).toInt())
        }
        AgentMotion.ask -> {
            val at = c + O(r * 0.6, -r * (1.6 + 1.2 * easeOut(t)))
            val size = 6.0 + 4.0 * s
            painter.save()
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 0.8, 0.5, fade * 0.35))
            val bubble = box(at.x - size, at.y - size * 0.8, size * 2, size * 1.6)
            painter.drawRoundedRect(bubble, size * 0.5, size * 0.5)
            painter.setPen(Pen(lit))
            painter.drawText(bubble, "?", (size * 1.4).toInt())
            painter.restore()
        }
        AgentMotion.skill -> {
            val out = r * (1.3 + 1.4 * sin(PI * t))
            val at = c + dir * out
            val size = 4.0 + 4.0 * s
            val star = evenOddPath()
            for (i in 0 until 10) {
                val p = polar(at, if (i % 2 != 0) size * 0.42 else size, -PI / 2 + t * 5.0 + i * PI / 5)
                if (i == 0) star.moveTo(p.x.toFloat(), p.y.toFloat()) else star.lineTo(p.x.toFloat(), p.y.toFloat())
            }
            star.close()
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 1.0, 0.7, fade * 0.6))
            painter.drawPath(star)
        }
        AgentMotion.mcp -> {
            val from = c + dir * r
            val to = c + dir * r * 3.0 + normal * r * 0.6
            val control = c + dir * r * 2.0 - normal * r * 0.8
            val u = easeOut(min(1.0, t * 1.8))
            val cable = evenOddPath().apply { moveTo(from.x.toFloat(), from.y.toFloat()) }
            for (k in 1..12) {
                val next = onQuad(from, control, to, u * k / 12.0)
                cable.lineTo(next.x.toFloat(), next.y.toFloat())
            }
            painter.setPen(pen)
            painter.noBrush()
            painter.drawPath(cable)
            val end = onQuad(from, control, to, u)
            val size = 3.0 + 2.5 * s
            painter.setBrush(if (u >= 1.0) lit else watchInk(hue, 0.8, 0.45, fade * 0.5))
            painter.drawRect(box(end.x - size, end.y - size, size * 2, size * 2))
        }
        AgentMotion.todo -> {
            val origin = polar(c, r * 1.6, a) + O(0.0, -r * 0.4)
            val rows = 3
            val row = 3.5 + 1.5 * s
            for (k in 0 until rows) {
                val appear = (t * rows * 1.4 - k * 0.8).coerceIn(0.0, 1.0)
                if (appear <= 0) continue
                val at = origin + O(0.0, k * row * 1.6)
                painter.noPen()
                painter.setBrush(watchInk(hue, 0.9, 0.7, fade * appear))
                painter.drawEllipse(at, 1.4, 1.4)
                painter.setPen(Pen(watchInk(hue, 0.8, 0.65, fade * appear), 1.0))
                painter.drawLine(at + O(row, 0.0), at + O(row * 4, 0.0))
                if (appear >= 1.0 && k < rows - 1) {
                    painter.setPen(Pen(lit, 1.3))
                    painter.drawLine(at + O(row * 0.6, -row * 0.35), at + O(row * 4.4, row * 0.35))
                }
            }
        }
        AgentMotion.fumble -> {
            val shake = sin(t * 60.0) * 2.0 * (1.0 - t)
            val radius = r * (1.2 + 0.9 * easeOut(t))
            val shards = 5
            painter.setPen(pen)
            painter.noBrush()
            for (k in 0 until shards) {
                val start = k * 360.0 / shards + t * 25.0 * (if (k % 2 != 0) 1 else -1)
                val centre = c + O(shake, 0.0) + polar(O.zero, r * 0.3 * t, (k + 0.5) * 2 * PI / shards)
                painter.drawArc(circleBox(centre, radius), (-start * 16).toInt(), (-(360.0 / shards - 12) * 16).toInt())
            }
        }
        AgentMotion.denied -> {
            val radius = r * (1.15 + 0.8 * easeOut(t))
            painter.setPen(pen.withWidth(width * 1.6))
            painter.setBrush(watchInk(hue, 0.9, 0.45, fade * 0.15))
            painter.drawEllipse(c, radius, radius)
            painter.drawLine(polar(c, radius * 0.72, -PI * 0.75), polar(c, radius * 0.72, PI * 0.25))
        }
        AgentMotion.image -> {
            val fall = easeInOut(t)
            val at = c + dir * r * (3.0 - 1.8 * fall)
            val w = 7.0 + 5.0 * s
            val frame = box(at.x - w, at.y - w * 0.7, w * 2, w * 1.4)
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 0.6, 0.3, fade * 0.6))
            painter.drawRect(frame)
            painter.noPen()
            painter.setBrush(lit)
            val left = frame.left.toDouble()
            val top = frame.top.toDouble()
            val bottom = frame.bottom.toDouble()
            painter.drawPolygon(listOf(
                frame.bottomLeftO,
                O(left + w * 0.7, top + w * 0.6),
                O(left + w * 1.1, bottom - w * 0.4),
                O(left + w * 1.5, top + w * 0.8),
                frame.bottomRightO,
            ))
            painter.drawEllipse(O(frame.right - w * 0.4, top + w * 0.35), 1.5, 1.5)
        }
        AgentMotion.compact -> {
            painter.noBrush()
            for (k in 0 until 3) {
                val tk = (t * 1.4 - k * 0.2).coerceIn(0.0, 1.0)
                if (tk <= 0 || tk >= 1) continue
                painter.setPen(pen.withColour(watchInk(hue, 0.9, 0.7, fade * (0.3 + 0.7 * tk))))
                val radius = r * (4.0 - 3.0 * easeInOut(tk))
                painter.drawEllipse(c, radius, radius * (1.0 - 0.3 * tk))
            }
        }
        AgentMotion.retry -> {
            val radius = r * 1.5
            val start = a * 180.0 / PI + t * 540.0
            painter.setPen(pen)
            painter.noBrush()
            painter.drawArc(circleBox(c, radius), (-start * 16).toInt(), -220 * 16)
            val tipAngle = (start + 220.0) * PI / 180.0
            val tip = polar(c, radius, tipAngle)
            val back = polar(c, radius, tipAngle - 0.3)
            val side = (tip - back) * 0.8
            painter.drawLine(tip, back + O(-side.y, side.x))
            painter.drawLine(tip, back - O(-side.y, side.x))
        }
        AgentMotion.fail -> {
            val grow = easeOut(min(1.0, t * 2.5))
            val arm = r * (1.0 + 0.8 * grow)
            painter.setPen(pen.withWidth(width * 1.8))
            painter.drawLine(c + O(-arm, -arm) * 0.7, c + O(arm, arm) * 0.7)
            painter.drawLine(c + O(arm, -arm) * 0.7, c + O(-arm, arm) * 0.7)
            painter.noPen()
            val drops = 8
            for (i in 0 until drops) {
                val x = (i.toDouble() / (drops - 1) - 0.5) * r * 3.0
                val y = r + t * t * r * (2.5 + scatter(i * 7) * 2.0)
                painter.setBrush(watchInk(hue, 1.0, 0.65, fade))
                painter.drawEllipse(c + O(x, y), 1.4 + s, 1.4 + s)
            }
        }
        AgentMotion.close -> {
            val at = polar(c, r * 1.35, a)
            val half = (3.0 + 2.0 * s) * (1.0 - 0.3 * t)
            val gap = half * (0.4 + 1.0 * (1.0 - easeOut(t)))
            painter.save()
            painter.translate(at)
            painter.rotate(a * 180.0 / PI)
            painter.setPen(Pen(lit, 1.1, Dash.solid, SQUARE, MITER))
            painter.noBrush()
            for (side in listOf(-1.0, 1.0)) {
                painter.drawPolyline(listOf(O(side * (gap + half * 0.4), -half), O(side * gap, -half), O(side * gap, half), O(side * (gap + half * 0.4), half)))
            }
            painter.drawLine(O(0.0, -half * 0.7), O(0.0, half * 0.7))
            painter.restore()
        }
        AgentMotion.seal -> {
            val at = polar(c, r * 1.45, a)
            val size = (2.4 + 1.8 * s) * (1.0 + 0.3 * easeOut(t))
            painter.noPen()
            painter.setBrush(watchInk(hue, 0.9, 0.55, fade))
            painter.drawEllipse(at, size, size)
            painter.setPen(Pen(watchInk(hue, 0.4, 0.95, fade), 1.0))
            painter.noBrush()
            painter.drawEllipse(at, size * 1.5, size * 1.5)
            val glint = -size + 2.0 * size * t
            painter.drawLine(at + O(glint - 1.5, -size), at + O(glint + 1.5, size))
        }
        AgentMotion.connect, AgentMotion.task, AgentMotion.logError, AgentMotion.logStall, AgentMotion.logSync,
        AgentMotion.logGit, AgentMotion.logCloud, AgentMotion.logTask, AgentMotion.logAccount, AgentMotion.logInfo -> {
            val travel = if (motion.inbound) 1.0 - 0.6 * easeOut(t) else easeOut(t)
            val at = polar(c, r * (1.3 + 0.8 * travel), a)
            val size = (3.2 + 2.4 * s) * (1.0 + 0.25 * easeOut(t))
            paintCueGlyph(painter, motion, at, size, lit, t)
            painter.setPen(Pen(line, max(0.8, width * 0.6)))
            painter.noBrush()
            val ring = r * (1.0 + (motion.reach - 1.0) * easeOut(t))
            painter.drawEllipse(c, ring, ring)
        }
        AgentMotion.hook -> {
            val swing = sin(t * 2.0 * PI * 1.5) * 0.35 * (1.0 - t)
            val pivot = polar(c, r * 1.1, a)
            val end = polar(pivot, r * 0.9, a + swing)
            painter.setPen(Pen(lit, 1.2, Dash.solid, ROUND))
            painter.noBrush()
            painter.drawLine(pivot, end)
            val size = 2.5 + 1.5 * s
            painter.drawArc(circleBox(end, size), ((-(a + swing) * 180.0 / PI + 90) * 16).toInt(), 200 * 16)
        }
    }
}

internal fun paintWatchIcon(painter: QtPainter, motion: AgentMotion, c: O, size: Double, alpha: Double) {
    val hue = motion.hue
    val colour = watchInk(hue, 0.92, 0.68, alpha)
    val pen = Pen(colour, max(1.0, size * 0.28), Dash.solid, ROUND, JROUND)
    painter.save()
    when (motion) {
        AgentMotion.start -> {
            painter.noPen()
            painter.setBrush(colour)
            for (i in 0 until 5) painter.drawEllipse(polar(c, size * 0.62, -PI / 2 + i * 2 * PI / 5), size * 0.34, size * 0.34)
        }
        AgentMotion.speak -> {
            painter.setPen(pen)
            painter.noBrush()
            for (k in 0 until 2) {
                val radius = size * (0.55 + 0.55 * k)
                val b = box(c.x - size - radius, c.y - radius, radius * 2, radius * 2)
                painter.drawArc(b.translated(size * 0.5, 0.0), -40 * 16, 80 * 16)
            }
        }
        AgentMotion.think -> {
            painter.noPen()
            painter.setBrush(colour)
            for (i in 0 until 3) painter.drawEllipse(polar(c, size * 0.6, -PI / 2 + i * 2 * PI / 3), size * 0.3, size * 0.3)
        }
        AgentMotion.read -> {
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 0.9, 0.55, alpha * 0.4))
            painter.drawRect(box(c.x - size * 0.6, c.y - size * 0.8, size * 1.2, size * 1.6))
        }
        AgentMotion.search -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawEllipse(c + O(-size * 0.2, -size * 0.2), size * 0.6, size * 0.6)
            painter.drawLine(c + O(size * 0.25, size * 0.25), c + O(size * 0.9, size * 0.9))
        }
        AgentMotion.web -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawEllipse(c, size * 0.85, size * 0.85)
            painter.drawEllipse(c, size * 0.38, size * 0.85)
            painter.drawLine(c + O(-size * 0.85, 0.0), c + O(size * 0.85, 0.0))
        }
        AgentMotion.plan -> {
            painter.setPen(pen.withWidth(max(1.0, size * 0.2)))
            painter.noBrush()
            for (k in -1..1) {
                val y = c.y + k * size * 0.62
                painter.drawLine(O(c.x - size * 0.8, y), O(c.x - size * 0.45, y + size * 0.2))
                painter.drawLine(O(c.x - size * 0.45, y + size * 0.2), O(c.x - size * 0.1, y - size * 0.25))
                painter.drawLine(O(c.x + size * 0.15, y), O(c.x + size * 0.9, y))
            }
        }
        AgentMotion.write -> {
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 0.9, 0.55, alpha * 0.4))
            painter.drawPolygon(listOf(c + O(0.0, -size), c + O(size * 0.8, 0.0), c + O(0.0, size), c + O(-size * 0.8, 0.0)))
        }
        AgentMotion.run -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawPolyline(listOf(c + O(size * 0.3, -size), c + O(-size * 0.35, size * 0.1), c + O(size * 0.35, -size * 0.1), c + O(-size * 0.3, size)))
        }
        AgentMotion.delegate -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawEllipse(c + O(-size * 0.35, size * 0.2), size * 0.5, size * 0.5)
            painter.noPen()
            painter.setBrush(colour)
            painter.drawEllipse(c + O(size * 0.6, -size * 0.55), size * 0.3, size * 0.3)
        }
        AgentMotion.arrive -> {
            painter.setPen(pen)
            painter.noBrush()
            for (i in 0 until 4) {
                val at = PI / 4 + i * PI / 2
                painter.drawLine(polar(c, size, at), polar(c, size * 0.45, at))
            }
            painter.noPen()
            painter.setBrush(colour)
            painter.drawEllipse(c, size * 0.25, size * 0.25)
        }
        AgentMotion.prompt -> {
            painter.setPen(pen)
            painter.drawLine(c + O(-size * 0.7, -size), c + O(size * 0.1, size * 0.2))
            painter.noPen()
            painter.setBrush(colour)
            painter.drawEllipse(c + O(size * 0.2, size * 0.35), size * 0.45, size * 0.45)
        }
        AgentMotion.notice -> {
            painter.setPen(pen)
            painter.setBrush(watchInk(hue, 0.9, 0.5, alpha * 0.3))
            painter.drawPolygon((0 until 3).map { polar(c, size, -PI / 2 + it * 2 * PI / 3) })
        }
        AgentMotion.finish -> {
            painter.setPen(pen)
            for (i in 0 until 8) {
                val at = i * PI / 4
                painter.drawLine(polar(c, size * 0.35, at), polar(c, size, at))
            }
        }
        AgentMotion.frame -> {
            painter.setPen(pen)
            painter.noBrush()
            for (side in listOf(-1.0, 1.0)) {
                painter.drawPolyline(listOf(c + O(side * size * 0.3, -size * 0.8), c + O(side * size * 0.7, -size * 0.8), c + O(side * size * 0.7, size * 0.8), c + O(side * size * 0.3, size * 0.8)))
            }
        }
        AgentMotion.compose -> {
            painter.noPen()
            painter.setBrush(colour)
            for (k in -1..1) painter.drawEllipse(c + O(k * size * 0.6, 0.0), size * 0.22, size * 0.22)
        }
        AgentMotion.usage -> {
            painter.noPen()
            painter.setBrush(colour)
            for (k in 0 until 3) {
                val height = size * (0.6 + 0.5 * k)
                painter.drawRect(box(c.x - size * 0.8 + k * size * 0.6, c.y + size * 0.8 - height, size * 0.4, height))
            }
        }
        AgentMotion.system -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawEllipse(c, size * 0.45, size * 0.45)
            for (i in 0 until 6) {
                val at = i * PI / 3.0
                painter.drawLine(polar(c, size * 0.5, at), polar(c, size * 0.9, at))
            }
        }
        AgentMotion.tick -> {
            painter.noPen()
            painter.setBrush(colour)
            painter.drawEllipse(c, size * 0.3, size * 0.3)
        }
        AgentMotion.network -> {
            painter.setPen(pen)
            painter.drawLine(c + O(-size * 0.4, -size * 0.7), c + O(-size * 0.4, size * 0.7))
            painter.drawLine(c + O(-size * 0.75, -size * 0.3), c + O(-size * 0.4, -size * 0.7))
            painter.drawLine(c + O(size * 0.4, -size * 0.7), c + O(size * 0.4, size * 0.7))
            painter.drawLine(c + O(size * 0.75, size * 0.3), c + O(size * 0.4, size * 0.7))
        }
        AgentMotion.scan -> {
            painter.noPen()
            painter.setBrush(colour)
            for (k in -2..2) painter.drawEllipse(polar(c + O(0.0, size * 0.6), size, -PI / 2 + k * 0.4), size * 0.18, size * 0.18)
        }
        AgentMotion.create, AgentMotion.skill -> {
            val points = if (motion == AgentMotion.create) 4 else 5
            val star = evenOddPath()
            for (i in 0 until points * 2) {
                val p = polar(c, if (i % 2 != 0) size * 0.4 else size, -PI / 2 + i * PI / points)
                if (i == 0) star.moveTo(p.x.toFloat(), p.y.toFloat()) else star.lineTo(p.x.toFloat(), p.y.toFloat())
            }
            star.close()
            painter.noPen()
            painter.setBrush(colour)
            painter.drawPath(star)
        }
        AgentMotion.test -> {
            painter.setPen(pen)
            painter.drawPolyline(listOf(c + O(-size * 0.7, 0.0), c + O(-size * 0.2, size * 0.5), c + O(size * 0.8, -size * 0.6)))
        }
        AgentMotion.build -> {
            painter.noPen()
            painter.setBrush(colour)
            for (k in 0 until 3) painter.drawRect(box(c.x - size * 0.8 + (k % 2) * size * 0.3, c.y + size * 0.5 - k * size * 0.55, size * 1.3, size * 0.42))
        }
        AgentMotion.git -> {
            painter.setPen(pen)
            painter.drawLine(c + O(-size * 0.4, size), c + O(-size * 0.4, -size))
            painter.drawLine(c + O(-size * 0.4, size * 0.2), c + O(size * 0.5, -size * 0.5))
            painter.noPen()
            painter.setBrush(colour)
            painter.drawEllipse(c + O(size * 0.5, -size * 0.5), size * 0.28, size * 0.28)
            painter.drawEllipse(c + O(-size * 0.4, -size), size * 0.28, size * 0.28)
        }
        AgentMotion.install -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawRect(box(c.x - size * 0.7, c.y - size * 0.3, size * 1.4, size * 1.1))
            painter.drawLine(c + O(0.0, -size), c + O(0.0, size * 0.3))
        }
        AgentMotion.spawn -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawEllipse(c + O(-size * 0.3, size * 0.3), size * 0.45, size * 0.45)
            painter.drawArc(box(c.x - size, c.y - size, size * 2, size * 2), 10 * 16, 80 * 16)
            painter.noPen()
            painter.setBrush(colour)
            painter.drawEllipse(c + O(size * 0.7, -size * 0.7), size * 0.25, size * 0.25)
        }
        AgentMotion.wait -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawEllipse(c, size * 0.85, size * 0.85)
            painter.drawLine(c, c + O(0.0, -size * 0.6))
            painter.drawLine(c, c + O(size * 0.45, 0.0))
        }
        AgentMotion.ask -> {
            painter.setPen(Pen(colour))
            painter.drawText(box(c.x - size, c.y - size * 1.2, size * 2, size * 2.4), "?", (size * 2.2).toInt())
        }
        AgentMotion.mcp -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawRect(box(c.x - size * 0.3, c.y - size * 0.5, size * 1.1, size))
            painter.drawLine(c + O(-size * 0.3, -size * 0.25), c + O(-size, -size * 0.25))
            painter.drawLine(c + O(-size * 0.3, size * 0.25), c + O(-size, size * 0.25))
        }
        AgentMotion.todo -> {
            painter.setPen(pen)
            for (k in -1..1) {
                val y = c.y + k * size * 0.62
                painter.drawLine(O(c.x - size * 0.4, y), O(c.x + size * 0.9, y))
                painter.drawPoint(O(c.x - size * 0.85, y))
            }
        }
        AgentMotion.fumble -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawArc(box(c.x - size * 0.8, c.y - size * 0.8, size * 1.6, size * 1.6), 20 * 16, 140 * 16)
            painter.drawArc(box(c.x - size * 0.7, c.y - size * 0.7, size * 1.6, size * 1.6), 200 * 16, 130 * 16)
        }
        AgentMotion.denied -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawEllipse(c, size * 0.85, size * 0.85)
            painter.drawLine(polar(c, size * 0.6, -PI * 0.75), polar(c, size * 0.6, PI * 0.25))
        }
        AgentMotion.image -> {
            painter.setPen(pen)
            painter.noBrush()
            val frame = box(c.x - size * 0.9, c.y - size * 0.7, size * 1.8, size * 1.4)
            painter.drawRect(frame)
            painter.drawPolyline(listOf(frame.bottomLeftO, O(c.x - size * 0.2, c.y - size * 0.1), frame.bottomRightO))
        }
        AgentMotion.compact -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawLine(c + O(-size, -size * 0.6), c + O(-size * 0.2, 0.0))
            painter.drawLine(c + O(-size, size * 0.6), c + O(-size * 0.2, 0.0))
            painter.drawLine(c + O(size, -size * 0.6), c + O(size * 0.2, 0.0))
            painter.drawLine(c + O(size, size * 0.6), c + O(size * 0.2, 0.0))
        }
        AgentMotion.retry -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawArc(box(c.x - size * 0.8, c.y - size * 0.8, size * 1.6, size * 1.6), 60 * 16, 270 * 16)
            painter.drawLine(polar(c, size * 0.8, -PI / 3), polar(c, size * 0.8, -PI / 3) + O(size * 0.4, 0.0))
        }
        AgentMotion.fail -> {
            painter.setPen(pen)
            painter.drawLine(c + O(-size * 0.7, -size * 0.7), c + O(size * 0.7, size * 0.7))
            painter.drawLine(c + O(size * 0.7, -size * 0.7), c + O(-size * 0.7, size * 0.7))
        }
        AgentMotion.close -> {
            painter.setPen(pen)
            painter.noBrush()
            for (side in listOf(-1.0, 1.0)) {
                painter.drawPolyline(listOf(c + O(side * size * 0.8, -size * 0.8), c + O(side * size * 0.4, -size * 0.8), c + O(side * size * 0.4, size * 0.8), c + O(side * size * 0.8, size * 0.8)))
            }
            painter.drawLine(c + O(0.0, -size * 0.5), c + O(0.0, size * 0.5))
        }
        AgentMotion.seal -> {
            painter.noPen()
            painter.setBrush(colour)
            painter.drawEllipse(c, size * 0.55, size * 0.55)
            painter.setPen(pen)
            painter.noBrush()
            painter.drawEllipse(c, size * 0.9, size * 0.9)
        }
        AgentMotion.connect, AgentMotion.task, AgentMotion.logError, AgentMotion.logStall, AgentMotion.logSync,
        AgentMotion.logGit, AgentMotion.logCloud, AgentMotion.logTask, AgentMotion.logAccount, AgentMotion.logInfo ->
            paintCueGlyph(painter, motion, c, size, colour, 0.0)
        AgentMotion.hook -> {
            painter.setPen(pen)
            painter.noBrush()
            painter.drawLine(c + O(size * 0.3, -size), c + O(size * 0.3, size * 0.2))
            painter.drawArc(box(c.x - size * 0.5, c.y - size * 0.2, size * 0.8, size * 0.9), 180 * 16, 180 * 16)
        }
    }
    painter.restore()
}
