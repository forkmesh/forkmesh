package com.forkmesh.nativeapp.feature.agentwatch

import android.graphics.Canvas
import android.graphics.DashPathEffect
import android.graphics.LinearGradient
import android.graphics.Paint
import android.graphics.Path
import android.graphics.RadialGradient
import android.graphics.RectF
import android.graphics.Shader
import android.graphics.Typeface
import kotlin.math.PI
import kotlin.math.abs
import kotlin.math.cos
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.min
import kotlin.math.sin

internal data class O(val x: Double, val y: Double) {
    operator fun plus(o: O) = O(x + o.x, y + o.y)
    operator fun minus(o: O) = O(x - o.x, y - o.y)
    operator fun times(k: Double) = O(x * k, y * k)
    operator fun div(k: Double) = O(x / k, y / k)
    operator fun unaryMinus() = O(-x, -y)
    val distance: Double get() = hypot(x, y)

    companion object {
        val zero = O(0.0, 0.0)
    }
}

internal fun O(x: Number, y: Number) = O(x.toDouble(), y.toDouble())

internal fun box(x: Double, y: Double, w: Double, h: Double) = RectF(x.toFloat(), y.toFloat(), (x + w).toFloat(), (y + h).toFloat())

internal val RectF.bottomLeftO: O get() = O(left.toDouble(), bottom.toDouble())
internal val RectF.bottomRightO: O get() = O(right.toDouble(), bottom.toDouble())

internal fun RectF.translated(dx: Double, dy: Double) = RectF(left + dx.toFloat(), top + dy.toFloat(), right + dx.toFloat(), bottom + dy.toFloat())

internal fun radians(degrees: Double) = degrees * PI / 180.0

internal fun onQuad(from: O, control: O, to: O, t: Double): O {
    val u = 1 - t
    return from * (u * u) + control * (2 * u * t) + to * (t * t)
}

internal fun easeOut(t: Double): Double {
    val c = t.coerceIn(0.0, 1.0)
    return 1.0 - (1.0 - c) * (1.0 - c) * (1.0 - c)
}

internal fun easeInOut(t: Double): Double {
    val c = t.coerceIn(0.0, 1.0)
    return c * c * (3.0 - 2.0 * c)
}

internal fun polar(centre: O, radius: Double, angle: Double) = centre + O(cos(angle), sin(angle)) * radius

internal fun scatter(value: Int): Double {
    var seed = value.toLong() and 0xffffffffL
    seed = seed xor (seed ushr 16)
    seed = (seed * 0x7feb352dL) and 0xffffffffL
    seed = seed xor (seed ushr 15)
    seed = (seed * 0x846ca68bL) and 0xffffffffL
    seed = seed xor (seed ushr 16)
    return (seed % 100000).toDouble() / 100000.0
}

internal fun argb(a: Int, r: Int, g: Int, b: Int): Int =
    (a.coerceIn(0, 255) shl 24) or (r.coerceIn(0, 255) shl 16) or (g.coerceIn(0, 255) shl 8) or b.coerceIn(0, 255)

internal fun alphaOf(colour: Int): Double = ((colour ushr 24) and 0xff) / 255.0

internal fun withAlpha(colour: Int, alpha: Double): Int =
    (Math.round(alpha.coerceIn(0.0, 1.0) * 255).toInt() shl 24) or (colour and 0xffffff)

internal fun watchInk(hue: Double, saturation: Double, lightness: Double, alpha: Double = 1.0): Int {
    val h = hue.coerceIn(0.0, 0.9999) * 360.0
    val s = saturation.coerceIn(0.0, 1.0)
    val l = lightness.coerceIn(0.0, 1.0)
    val chroma = (1.0 - abs(2.0 * l - 1.0)) * s
    val secondary = chroma * (1.0 - abs((h / 60.0) % 2.0 - 1.0))
    val match = l - chroma / 2.0
    val (r, g, b) = when {
        h < 60 -> Triple(chroma, secondary, 0.0)
        h < 120 -> Triple(secondary, chroma, 0.0)
        h < 180 -> Triple(0.0, chroma, secondary)
        h < 240 -> Triple(0.0, secondary, chroma)
        h < 300 -> Triple(secondary, 0.0, chroma)
        else -> Triple(chroma, 0.0, secondary)
    }
    fun byte(v: Double) = Math.round((v + match) * 255).toInt()
    return argb(Math.round(alpha.coerceIn(0.0, 1.0) * 255).toInt(), byte(r), byte(g), byte(b))
}

internal fun drawWatchGlow(canvas: Canvas, centre: O, radius: Double, colour: Int) {
    val alpha = alphaOf(colour)
    if (alpha <= 0.004 || radius <= 0.2) return
    val solid = argb(
        Math.round(alpha * 255).toInt(),
        (colour shr 16) and 0xfc,
        (colour shr 8) and 0xfc,
        colour and 0xfc,
    )
    val paint = Paint(Paint.ANTI_ALIAS_FLAG)
    paint.shader = RadialGradient(
        centre.x.toFloat(), centre.y.toFloat(), radius.toFloat(),
        intArrayOf(solid, solid and 0xffffff), null, Shader.TileMode.CLAMP,
    )
    canvas.drawCircle(centre.x.toFloat(), centre.y.toFloat(), radius.toFloat(), paint)
}

internal enum class Dash { solid, dash, dot }

internal data class Pen(
    val colour: Int,
    val width: Double = 1.0,
    val dash: Dash = Dash.solid,
    val cap: Paint.Cap = Paint.Cap.SQUARE,
    val join: Paint.Join = Paint.Join.BEVEL,
    val shader: Shader? = null,
) {
    fun withColour(value: Int) = Pen(value, width, dash, cap, join)
    fun withWidth(value: Double) = copy(width = value)
    fun withDash(value: Dash) = copy(dash = value)
}

internal fun evenOddPath() = Path().apply { fillType = Path.FillType.EVEN_ODD }

internal class QtPainter(val canvas: Canvas) {
    private var pen: Pen? = null
    private var brush: Int? = null
    private var brushShader: Shader? = null
    private val saved = ArrayList<Triple<Pen?, Int?, Shader?>>()
    private val fillPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.FILL }
    private val strokePaint = Paint(Paint.ANTI_ALIAS_FLAG).apply { style = Paint.Style.STROKE }
    private val textPaint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        typeface = Typeface.DEFAULT_BOLD
        textAlign = Paint.Align.CENTER
    }

    fun save() {
        canvas.save()
        saved += Triple(pen, brush, brushShader)
    }

    fun restore() {
        canvas.restore()
        val state = saved.removeAt(saved.size - 1)
        pen = state.first
        brush = state.second
        brushShader = state.third
    }

    fun translate(by: O) = canvas.translate(by.x.toFloat(), by.y.toFloat())

    fun rotate(degrees: Double) = canvas.rotate(degrees.toFloat())

    fun setPen(value: Pen) {
        pen = value
    }

    fun noPen() {
        pen = null
    }

    fun setBrush(colour: Int) {
        brush = colour
        brushShader = null
    }

    fun setBrushShader(shader: Shader) {
        brush = null
        brushShader = shader
    }

    fun noBrush() {
        brush = null
        brushShader = null
    }

    private fun fill(): Paint? {
        val colour = brush
        val shader = brushShader
        if (colour == null && shader == null) return null
        fillPaint.shader = shader
        if (shader == null) fillPaint.color = colour!! else fillPaint.color = 0xff000000.toInt()
        return fillPaint
    }

    private fun stroke(): Paint? {
        val p = pen ?: return null
        strokePaint.strokeWidth = p.width.toFloat()
        strokePaint.strokeCap = p.cap
        strokePaint.strokeJoin = p.join
        strokePaint.strokeMiter = 4f
        strokePaint.shader = p.shader
        strokePaint.color = if (p.shader != null) 0xff000000.toInt() else p.colour
        strokePaint.pathEffect = when (p.dash) {
            Dash.solid -> null
            Dash.dash -> DashPathEffect(floatArrayOf((4 * p.width).toFloat(), (2 * p.width).toFloat()), 0f)
            Dash.dot -> DashPathEffect(floatArrayOf(p.width.toFloat(), (2 * p.width).toFloat()), 0f)
        }
        return strokePaint
    }

    fun drawEllipse(centre: O, rx: Double, ry: Double) {
        val rect = RectF(
            (centre.x - abs(rx)).toFloat(), (centre.y - abs(ry)).toFloat(),
            (centre.x + abs(rx)).toFloat(), (centre.y + abs(ry)).toFloat(),
        )
        fill()?.let { canvas.drawOval(rect, it) }
        stroke()?.let { canvas.drawOval(rect, it) }
    }

    fun drawRect(rect: RectF) {
        fill()?.let { canvas.drawRect(rect, it) }
        stroke()?.let { canvas.drawRect(rect, it) }
    }

    fun drawRoundedRect(rect: RectF, xRadius: Double, yRadius: Double) {
        fill()?.let { canvas.drawRoundRect(rect, xRadius.toFloat(), yRadius.toFloat(), it) }
        stroke()?.let { canvas.drawRoundRect(rect, xRadius.toFloat(), yRadius.toFloat(), it) }
    }

    fun drawLine(from: O, to: O) {
        val paint = stroke() ?: return
        canvas.drawLine(from.x.toFloat(), from.y.toFloat(), to.x.toFloat(), to.y.toFloat(), paint)
    }

    fun drawPoint(at: O) {
        val paint = stroke() ?: return
        canvas.drawPoint(at.x.toFloat(), at.y.toFloat(), paint)
    }

    fun drawArc(rect: RectF, start16: Int, span16: Int) {
        val paint = stroke() ?: return
        canvas.drawArc(rect, -start16 / 16f, -span16 / 16f, false, paint)
    }

    fun drawPie(rect: RectF, start16: Int, span16: Int) {
        fill()?.let { canvas.drawArc(rect, -start16 / 16f, -span16 / 16f, true, it) }
        stroke()?.let { canvas.drawArc(rect, -start16 / 16f, -span16 / 16f, true, it) }
    }

    fun drawPath(path: Path) {
        fill()?.let { canvas.drawPath(path, it) }
        stroke()?.let { canvas.drawPath(path, it) }
    }

    fun drawPolygon(points: List<O>) {
        val path = evenOddPath()
        points.forEachIndexed { i, p -> if (i == 0) path.moveTo(p.x.toFloat(), p.y.toFloat()) else path.lineTo(p.x.toFloat(), p.y.toFloat()) }
        path.close()
        drawPath(path)
    }

    fun drawPolyline(points: List<O>) {
        val paint = stroke() ?: return
        val path = Path()
        points.forEachIndexed { i, p -> if (i == 0) path.moveTo(p.x.toFloat(), p.y.toFloat()) else path.lineTo(p.x.toFloat(), p.y.toFloat()) }
        canvas.drawPath(path, paint)
    }

    fun drawText(rect: RectF, text: String, pixelSize: Int) {
        val p = pen ?: return
        if (pixelSize <= 0) return
        textPaint.color = p.colour
        textPaint.textSize = pixelSize.toFloat()
        val metrics = textPaint.fontMetrics
        val baseline = rect.centerY() - (metrics.ascent + metrics.descent) / 2
        canvas.drawText(text, rect.centerX(), baseline, textPaint)
    }
}

internal fun linearShader(from: O, to: O, colours: IntArray): Shader =
    LinearGradient(from.x.toFloat(), from.y.toFloat(), to.x.toFloat(), to.y.toFloat(), colours, null, Shader.TileMode.CLAMP)

internal fun radialShader(centre: O, radius: Double, colours: IntArray): Shader =
    RadialGradient(centre.x.toFloat(), centre.y.toFloat(), max(radius, 0.01).toFloat(), colours, null, Shader.TileMode.CLAMP)

internal fun minD(a: Double, b: Double) = min(a, b)
