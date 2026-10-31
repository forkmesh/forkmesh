package com.forkmesh.nativeapp.chat.face

data class CallOutfitColor(val r: Double, val g: Double, val b: Double, val a: Double = 1.0) {
    companion object {
        fun hex(hex: String): CallOutfitColor {
            val value = hex.drop(1).toInt(16)
            return CallOutfitColor(((value shr 16) and 255).toDouble(), ((value shr 8) and 255).toDouble(), (value and 255).toDouble())
        }

        fun css(css: String): CallOutfitColor {
            if (css.startsWith("#")) return hex(css)
            val parts = css.substringAfter("(").substringBefore(")").split(",").map { it.trim().toDouble() }
            return CallOutfitColor(parts[0], parts[1], parts[2], if (parts.size > 3) parts[3] else 1.0)
        }
    }
}

enum class CallOutfitFabricKind { WOOL, JERSEY, HEATHER, RIB, KNIT, DENIM, PLAID }

data class CallOutfitFabric(val kind: CallOutfitFabricKind, val colors: List<String>)

data class CallOutfitGradientStop(val offset: Double, val color: CallOutfitColor)

data class CallOutfitGradient(val x0: Double, val y0: Double, val x1: Double, val y1: Double, val stops: List<CallOutfitGradientStop>)

sealed class CallOutfitPaint {
    data class Color(val color: CallOutfitColor) : CallOutfitPaint()
    data class Fabric(val fabric: CallOutfitFabric) : CallOutfitPaint()
    data class Gradient(val gradient: CallOutfitGradient) : CallOutfitPaint()
}

sealed class CallOutfitCommand {
    object Save : CallOutfitCommand()
    object Restore : CallOutfitCommand()
    object BeginPath : CallOutfitCommand()
    object ClosePath : CallOutfitCommand()
    object Fill : CallOutfitCommand()
    object Stroke : CallOutfitCommand()
    object Clip : CallOutfitCommand()
    data class MoveTo(val x: Double, val y: Double) : CallOutfitCommand()
    data class LineTo(val x: Double, val y: Double) : CallOutfitCommand()
    data class QuadTo(val cx: Double, val cy: Double, val x: Double, val y: Double) : CallOutfitCommand()
    data class Arc(val x: Double, val y: Double, val radius: Double, val start: Double, val end: Double) : CallOutfitCommand()
    data class FillRect(val x: Double, val y: Double, val w: Double, val h: Double) : CallOutfitCommand()
    data class Translate(val x: Double, val y: Double) : CallOutfitCommand()
    data class Rotate(val angle: Double) : CallOutfitCommand()
    data class FillStyle(val paint: CallOutfitPaint) : CallOutfitCommand()
    data class StrokeStyle(val paint: CallOutfitPaint) : CallOutfitCommand()
    data class LineWidth(val width: Double) : CallOutfitCommand()
    data class LineCap(val cap: String) : CallOutfitCommand()
    data class LineJoin(val join: String) : CallOutfitCommand()
    data class LineDash(val dash: List<Double>) : CallOutfitCommand()
    data class Composite(val operation: String) : CallOutfitCommand()
}

object CallOutfitDrawing {
    fun commands(id: String): List<CallOutfitCommand>? {
        val pen = Pen()
        when (id) {
            "suit" -> pen.suit()
            "hoodie" -> pen.hoodie()
            "sweater" -> pen.sweater()
            "denim" -> pen.denim()
            "flannel" -> pen.flannel()
            "turtleneck" -> pen.turtleneck()
            else -> return null
        }
        return pen.ops
    }

    fun fabricTile(fabric: CallOutfitFabric): ByteArray {
        val side = CallOutfits.TILE
        val data = ByteArray(side * side * 4)
        val colors = fabric.colors.map(::rgb)
        for (y in 0 until side) {
            for (x in 0 until side) {
                val shade = shade(fabric.kind, colors, x, y)
                val at = (y * side + x) * 4
                data[at] = clampByte(shade[0])
                data[at + 1] = clampByte(shade[1])
                data[at + 2] = clampByte(shade[2])
                data[at + 3] = 255.toByte()
            }
        }
        return data
    }

    internal fun clampByte(value: Double): Byte {
        if (!(value > 0)) return 0
        if (value >= 255) return 255.toByte()
        return Math.rint(value).toInt().toByte()
    }

    private fun rgb(hex: String): DoubleArray {
        val value = hex.drop(1).toInt(16)
        return doubleArrayOf(((value shr 16) and 255).toDouble(), ((value shr 8) and 255).toDouble(), (value and 255).toDouble())
    }

    internal fun hash(x: Int, y: Int, seed: Int): Double {
        val sum = x.toLong() * 374761393L + y.toLong() * 668265263L + seed.toLong() * 2147483647L
        var h = sum.toInt()
        h = (h xor (h ushr 13)) * 1274126177
        return ((h xor (h ushr 16)).toLong() and 0xffffffffL).toDouble() / 4294967295.0
    }

    private fun wave(value: Int, period: Double): Double = StrictMath.sin((value * 2 * Math.PI) / period)

    private fun scaled(color: DoubleArray, k: Double): DoubleArray = DoubleArray(color.size) { CallFaceJS.max(0.0, CallFaceJS.min(255.0, color[it] * k)) }

    private fun band(v: Int): Int {
        val m = v % 128
        if (m < 36) return 2
        if (m < 44 || (m >= 76 && m < 84)) return 1
        if (m >= 100 && m < 104) return 3
        return 0
    }

    private fun shade(kind: CallOutfitFabricKind, colors: List<DoubleArray>, x: Int, y: Int): DoubleArray {
        val base = colors[0]
        return when (kind) {
            CallOutfitFabricKind.WOOL -> scaled(base, 0.96 + 0.05 * hash(x, y, 1) + 0.025 * wave(x + y, 4.0))
            CallOutfitFabricKind.JERSEY -> scaled(base, 0.97 + 0.045 * hash(x, y, 2))
            CallOutfitFabricKind.HEATHER -> {
                val fleck = hash(x shr 1, y, 3)
                scaled(base, 0.9 + 0.12 * hash(x, y, 4) + (if (fleck > 0.93) 0.16 else if (fleck < 0.05) -0.12 else 0.0))
            }
            CallOutfitFabricKind.RIB -> scaled(base, 0.9 + 0.1 * Math.abs(wave(x, 8.0)) + 0.04 * hash(x, y, 5))
            CallOutfitFabricKind.KNIT -> {
                val cx = (x % 8) - 4
                val cy = ((y + (if (Math.abs(cx) > 0) Math.abs(cx) else 0)) % 6).toDouble() / 6
                scaled(base, 0.82 + 0.16 * StrictMath.sin(cy * Math.PI) + 0.05 * hash(x, y, 6) - 0.06 * (if (Math.abs(cx) < 1) 1 else 0))
            }
            CallOutfitFabricKind.DENIM -> {
                val twill = 0.5 + 0.5 * wave(x + y * 2, 6.0)
                val weft = if (hash(x, y, 7) > 0.82) 1.22 else 1.0
                scaled(base, (0.86 + 0.16 * twill + 0.06 * hash(x shr 2, y, 8)) * weft)
            }
            CallOutfitFabricKind.PLAID -> {
                val a = band(x)
                val b = band(y)
                val pick = if ((y + x) % 4 < 2) a else b
                val other = colors[if (a == b) a else maxOf(a, b)]
                val chosen = colors[pick]
                val mixed = DoubleArray(chosen.size) { (chosen[it] + other[it]) / 2 }
                scaled(mixed, 0.94 + 0.08 * hash(x, y, 9) + 0.03 * wave(x - y, 4.0))
            }
        }
    }

    internal class Pen {
        val ops = ArrayList<CallOutfitCommand>()
        private val unit = CallOutfits.UNIT
        private val size = CallOutfits.SIZE

        private fun at(x: Double, y: Double): Pair<Double, Double> = (CallOutfits.neck.x + x * unit) to (CallOutfits.neck.y + y * unit)

        private fun color(css: String): CallOutfitPaint = CallOutfitPaint.Color(CallOutfitColor.css(css))

        private fun shade(r: Double, g: Double, b: Double, a: Double): CallOutfitPaint = CallOutfitPaint.Color(CallOutfitColor(r, g, b, a))

        private fun fabric(kind: CallOutfitFabricKind, vararg colors: String): CallOutfitPaint = CallOutfitPaint.Fabric(CallOutfitFabric(kind, colors.toList()))

        private fun moveTo(p: Pair<Double, Double>) = ops.add(CallOutfitCommand.MoveTo(p.first, p.second))
        private fun lineTo(p: Pair<Double, Double>) = ops.add(CallOutfitCommand.LineTo(p.first, p.second))
        private fun quadTo(c: Pair<Double, Double>, p: Pair<Double, Double>) = ops.add(CallOutfitCommand.QuadTo(c.first, c.second, p.first, p.second))

        private fun path(points: List<Pair<Double, Double>>, close: Boolean = true) {
            ops.add(CallOutfitCommand.BeginPath)
            points.forEachIndexed { i, point -> if (i == 0) moveTo(at(point.first, point.second)) else lineTo(at(point.first, point.second)) }
            if (close) ops.add(CallOutfitCommand.ClosePath)
        }

        private fun curve(from: Pair<Double, Double>, steps: List<DoubleArray>) {
            ops.add(CallOutfitCommand.BeginPath)
            moveTo(at(from.first, from.second))
            for (step in steps) {
                if (step.size == 4) quadTo(at(step[0], step[1]), at(step[2], step[3])) else lineTo(at(step[0], step[1]))
            }
        }

        private fun fillWith(style: CallOutfitPaint, draw: () -> Unit) {
            ops.add(CallOutfitCommand.Save)
            ops.add(CallOutfitCommand.FillStyle(style))
            draw()
            ops.add(CallOutfitCommand.Fill)
            ops.add(CallOutfitCommand.Restore)
        }

        private fun stroke(style: CallOutfitPaint, widthUnits: Double, dash: List<Double>? = null, draw: () -> Unit) {
            ops.add(CallOutfitCommand.Save)
            ops.add(CallOutfitCommand.StrokeStyle(style))
            ops.add(CallOutfitCommand.LineWidth(widthUnits * unit))
            ops.add(CallOutfitCommand.LineCap("round"))
            ops.add(CallOutfitCommand.LineJoin("round"))
            if (dash != null) ops.add(CallOutfitCommand.LineDash(dash.map { it * unit }))
            draw()
            ops.add(CallOutfitCommand.Stroke)
            ops.add(CallOutfitCommand.Restore)
        }

        private fun shoulderLine(neck: Double): List<Pair<Double, Double>> {
            val line = ArrayList<Pair<Double, Double>>()
            var x = -5.5
            while (x <= -neck) {
                line.add(x to (-0.75 + 0.3 * (-x - 0.9)))
                x += 0.25
            }
            return line
        }

        private fun cutTop(neckHalf: Double, neckline: () -> Unit) {
            ops.add(CallOutfitCommand.Save)
            ops.add(CallOutfitCommand.Composite("destination-out"))
            val left = shoulderLine(neckHalf)
            val right = left.map { -it.first to it.second }.reversed()
            ops.add(CallOutfitCommand.BeginPath)
            moveTo(at(-5.6, -2.2))
            for (point in left) lineTo(at(point.first, point.second))
            neckline()
            for (point in right) lineTo(at(point.first, point.second))
            lineTo(at(5.6, -2.2))
            ops.add(CallOutfitCommand.ClosePath)
            ops.add(CallOutfitCommand.Fill)
            ops.add(CallOutfitCommand.Restore)
        }

        private fun roundNeck(half: Double, depth: Double, top: Double = -0.7): () -> Unit = {
            lineTo(at(-half, top))
            quadTo(at(-half * 0.92, depth), at(0.0, depth))
            quadTo(at(half * 0.92, depth), at(half, top))
        }

        private fun gradient(from: Pair<Double, Double>, to: Pair<Double, Double>, stops: List<Pair<Double, String>>): CallOutfitPaint =
            CallOutfitPaint.Gradient(CallOutfitGradient(from.first, from.second, to.first, to.second, stops.map { CallOutfitGradientStop(it.first, CallOutfitColor.css(it.second)) }))

        private fun soften() {
            ops.add(CallOutfitCommand.Save)
            ops.add(CallOutfitCommand.Composite("source-atop"))
            ops.add(CallOutfitCommand.FillStyle(gradient(at(0.0, -0.6), at(0.0, 1.6), listOf(0.0 to "rgba(0,0,0,0.28)", 1.0 to "rgba(0,0,0,0)"))))
            ops.add(CallOutfitCommand.FillRect(0.0, 0.0, size, size))
            for (side in listOf(-1.0, 1.0)) {
                ops.add(CallOutfitCommand.FillStyle(gradient(at(side * 1.8, 0.0), at(side * 3.4, 0.0), listOf(0.0 to "rgba(0,0,0,0)", 1.0 to "rgba(0,0,0,0.18)"))))
                val x0 = at(side * 1.8, 0.0).first
                ops.add(CallOutfitCommand.FillRect(if (side < 0) 0.0 else x0, 0.0, if (side < 0) x0 else size - x0, size))
            }
            ops.add(CallOutfitCommand.Restore)
        }

        private fun folds(lines: List<DoubleArray>, strength: Double = 0.12) {
            for (line in lines) {
                val x0 = line[0]
                val y0 = line[1]
                val x1 = line[2]
                val y1 = line[3]
                stroke(shade(0.0, 0.0, 0.0, strength), 0.09) { curve(x0 to y0, listOf(doubleArrayOf((x0 + x1) / 2 + 0.15, (y0 + y1) / 2, x1, y1))) }
                stroke(shade(255.0, 255.0, 255.0, strength * 0.5), 0.05) { curve((x0 + 0.08) to y0, listOf(doubleArrayOf((x0 + x1) / 2 + 0.23, (y0 + y1) / 2, x1 + 0.08, y1))) }
            }
        }

        private fun button(x: Double, y: Double, radius: Double, fill: String, rim: String = "rgba(0,0,0,0.45)") {
            ops.add(CallOutfitCommand.Save)
            ops.add(CallOutfitCommand.FillStyle(color(fill)))
            ops.add(CallOutfitCommand.StrokeStyle(color(rim)))
            ops.add(CallOutfitCommand.LineWidth(0.025 * unit))
            ops.add(CallOutfitCommand.BeginPath)
            val center = at(x, y)
            ops.add(CallOutfitCommand.Arc(center.first, center.second, radius * unit, 0.0, Math.PI * 2))
            ops.add(CallOutfitCommand.Fill)
            ops.add(CallOutfitCommand.Stroke)
            ops.add(CallOutfitCommand.Restore)
        }

        private fun d(vararg values: Double) = values

        fun suit() {
            ops.add(CallOutfitCommand.FillStyle(fabric(CallOutfitFabricKind.WOOL, "#33373e")))
            ops.add(CallOutfitCommand.FillRect(0.0, 0.0, size, size))
            fillWith(fabric(CallOutfitFabricKind.JERSEY, "#eceff3")) { path(listOf(-1.15 to -0.8, 0.0 to 3.4, 1.15 to -0.8)) }
            fillWith(fabric(CallOutfitFabricKind.WOOL, "#6a2234")) { path(listOf(-0.2 to 0.05, 0.2 to 0.05, 0.13 to 0.42, -0.13 to 0.42)) }
            fillWith(fabric(CallOutfitFabricKind.WOOL, "#5e1d2e")) { path(listOf(-0.13 to 0.42, 0.13 to 0.42, 0.3 to 3.0, 0.0 to 3.35, -0.3 to 3.0)) }
            ops.add(CallOutfitCommand.Save)
            ops.add(CallOutfitCommand.BeginPath)
            path(listOf(-0.13 to 0.42, 0.13 to 0.42, 0.3 to 3.0, 0.0 to 3.35, -0.3 to 3.0))
            ops.add(CallOutfitCommand.Clip)
            var y = 0.2
            while (y < 3.6) {
                val row = y
                stroke(color("rgba(214,178,120,0.55)"), 0.05) { path(listOf(-0.5 to row, 0.5 to (row + 0.5)), close = false) }
                y += 0.42
            }
            ops.add(CallOutfitCommand.Restore)
            for (side in listOf(-1.0, 1.0)) {
                fillWith(color("#f4f6f9")) { path(listOf(side * 0.82 to -0.78, side * 0.18 to 0.08, side * 0.36 to 0.58, side * 1.0 to -0.28)) }
                stroke(color("rgba(0,0,0,0.25)"), 0.025) { path(listOf(side * 0.18 to 0.08, side * 0.36 to 0.58, side * 1.0 to -0.28), close = false) }
            }
            for (side in listOf(-1.0, 1.0)) {
                val lapel = listOf(side * 1.15 to -0.8, side * 1.62 to 0.95, side * 1.32 to 1.12, side * 1.5 to 1.32, side * 0.05 to 3.42, side * 0.02 to 3.4)
                fillWith(fabric(CallOutfitFabricKind.WOOL, "#3a3f47")) { path(lapel) }
                stroke(color("rgba(0,0,0,0.35)"), 0.04) { path(lapel.subList(0, 5), close = false) }
                stroke(color("rgba(0,0,0,0.3)"), 0.12) { path(listOf(side * 1.17 to -0.75, side * 0.08 to 3.35), close = false) }
            }
            stroke(color("rgba(0,0,0,0.4)"), 0.03) { path(listOf(0.02 to 3.4, 0.12 to 9.5), close = false) }
            button(0.1, 3.85, 0.12, "#1d1f23")
            button(0.1, 5.05, 0.12, "#1d1f23")
            stroke(color("rgba(0,0,0,0.4)"), 0.05) { path(listOf(1.35 to 2.3, 2.35 to 2.18), close = false) }
            fillWith(color("#e9edf2")) { path(listOf(1.55 to 2.29, 1.75 to 2.0, 1.92 to 2.18, 2.1 to 1.98, 2.2 to 2.2)) }
            folds(listOf(d(-2.3, 1.6, -2.1, 4.6), d(2.2, 1.7, 2.0, 4.5), d(-0.9, 3.9, -0.7, 6.0)), 0.1)
            cutTop(0.82, roundNeck(0.74, -0.15, -0.82))
            soften()
        }

        fun hoodie() {
            ops.add(CallOutfitCommand.FillStyle(fabric(CallOutfitFabricKind.HEATHER, "#8e9299")))
            ops.add(CallOutfitCommand.FillRect(0.0, 0.0, size, size))
            ops.add(CallOutfitCommand.Save)
            curve(-1.85 to -0.55, listOf(d(-1.5, 1.25, 0.0, 1.2), d(1.5, 1.25, 1.85, -0.55)))
            lineTo(at(1.05, -0.6))
            quadTo(at(0.9, 0.45), at(0.0, 0.45))
            quadTo(at(-0.9, 0.45), at(-1.05, -0.6))
            ops.add(CallOutfitCommand.ClosePath)
            ops.add(CallOutfitCommand.FillStyle(fabric(CallOutfitFabricKind.HEATHER, "#9a9ea5")))
            ops.add(CallOutfitCommand.Fill)
            ops.add(CallOutfitCommand.Clip)
            ops.add(CallOutfitCommand.FillStyle(gradient(at(0.0, -0.4), at(0.0, 1.25), listOf(0.0 to "rgba(255,255,255,0.12)", 0.7 to "rgba(0,0,0,0)", 1.0 to "rgba(0,0,0,0.3)"))))
            ops.add(CallOutfitCommand.FillRect(0.0, 0.0, size, size))
            ops.add(CallOutfitCommand.Restore)
            stroke(color("rgba(0,0,0,0.3)"), 0.07) { curve(-1.85 to -0.55, listOf(d(-1.5, 1.25, 0.0, 1.2), d(1.5, 1.25, 1.85, -0.55))) }
            stroke(color("rgba(0,0,0,0.2)"), 0.04) { curve(-1.05 to -0.6, listOf(d(-0.9, 0.45, 0.0, 0.45), d(0.9, 0.45, 1.05, -0.6))) }
            for ((x0, x1, end) in listOf(Triple(-0.36, -0.44, 3.3), Triple(0.36, 0.3, 3.55))) {
                button(x0, 1.0, 0.07, "#6f737a", "rgba(0,0,0,0.5)")
                stroke(color("#e6e2d8"), 0.085) { curve(x0 to 1.0, listOf(d(x0 - 0.05, (1 + end) / 2, x1, end))) }
                stroke(color("#2b2d31"), 0.11) { path(listOf(x1 to end, x1 to (end + 0.3)), close = false) }
            }
            stroke(color("rgba(0,0,0,0.22)"), 0.05) { curve(-1.9 to 6.4, listOf(d(0.0, 5.8, 1.9, 6.4))) }
            folds(listOf(d(-2.2, 1.4, -1.9, 4.8), d(2.1, 1.5, 1.9, 4.6), d(-0.9, 2.2, -1.1, 5.2), d(1.0, 2.4, 0.8, 5.4)), 0.13)
            cutTop(1.05, roundNeck(0.92, 0.05, -0.6))
            soften()
        }

        fun sweater() {
            ops.add(CallOutfitCommand.FillStyle(fabric(CallOutfitFabricKind.KNIT, "#2d3d5e")))
            ops.add(CallOutfitCommand.FillRect(0.0, 0.0, size, size))
            for (cx in listOf(-1.15, 0.0, 1.15)) {
                var y = 0.65
                while (y < 10.5) {
                    val row = y
                    for (dir in listOf(-1.0, 1.0)) {
                        stroke(color("rgba(0,0,0,0.32)"), 0.13) { curve((cx - dir * 0.2) to row, listOf(d(cx + dir * 0.3, row + 0.35, cx + dir * 0.2, row + 0.7))) }
                        stroke(color("rgba(150,170,210,0.22)"), 0.06) { curve((cx - dir * 0.2) to (row - 0.03), listOf(d(cx + dir * 0.3, row + 0.32, cx + dir * 0.2, row + 0.67))) }
                    }
                    y += 0.7
                }
                for (side in listOf(-1.0, 1.0)) {
                    stroke(color("rgba(0,0,0,0.28)"), 0.05) { path(listOf((cx + side * 0.36) to 0.6, (cx + side * 0.36) to 10.5), close = false) }
                }
            }
            ops.add(CallOutfitCommand.Save)
            curve(-1.25 to -0.72, listOf(d(-1.15, 0.48, 0.0, 0.48), d(1.15, 0.48, 1.25, -0.72)))
            lineTo(at(0.92, -0.72))
            quadTo(at(0.85, 0.1), at(0.0, 0.1))
            quadTo(at(-0.85, 0.1), at(-0.92, -0.72))
            ops.add(CallOutfitCommand.ClosePath)
            ops.add(CallOutfitCommand.FillStyle(fabric(CallOutfitFabricKind.RIB, "#34466b")))
            ops.add(CallOutfitCommand.Fill)
            ops.add(CallOutfitCommand.Clip)
            var k = -1.0
            while (k <= 1.001) {
                val angle = k * 1.25
                val inner = (StrictMath.sin(angle) * 0.92) to (0.1 - (1 - StrictMath.cos(angle)) * 0.75)
                val outer = (StrictMath.sin(angle) * 1.25) to (0.48 - (1 - StrictMath.cos(angle)) * 1.0)
                stroke(color("rgba(0,0,0,0.22)"), 0.025) { path(listOf(inner, outer), close = false) }
                k += 0.06
            }
            ops.add(CallOutfitCommand.Restore)
            stroke(color("rgba(0,0,0,0.3)"), 0.04) { curve(-1.25 to -0.72, listOf(d(-1.15, 0.48, 0.0, 0.48), d(1.15, 0.48, 1.25, -0.72))) }
            folds(listOf(d(-2.3, 1.4, -2.0, 4.6), d(2.2, 1.5, 2.0, 4.6)), 0.1)
            cutTop(0.92, roundNeck(0.86, 0.1, -0.72))
            soften()
        }

        fun denim() {
            ops.add(CallOutfitCommand.FillStyle(fabric(CallOutfitFabricKind.DENIM, "#4b6f9e")))
            ops.add(CallOutfitCommand.FillRect(0.0, 0.0, size, size))
            val tee = listOf(-0.95 to -0.9, 0.95 to -0.9, 0.8 to 10.5, -0.8 to 10.5)
            fillWith(fabric(CallOutfitFabricKind.JERSEY, "#efece5")) { path(tee) }
            stroke(color("rgba(0,0,0,0.18)"), 0.06) { curve(-0.78 to -0.75, listOf(d(-0.7, 0.3, 0.0, 0.3), d(0.7, 0.3, 0.78, -0.75))) }
            for (side in listOf(-1.0, 1.0)) {
                val edge = listOf(side * 0.92 to -0.2, side * 0.8 to 10.5)
                stroke(color("rgba(0,0,0,0.35)"), 0.14) { path(edge.map { (it.first - side * 0.02) to it.second }, close = false) }
                stroke(color("#c8873a"), 0.025, listOf(0.09, 0.07)) { path(edge.map { (it.first + side * 0.12) to it.second }, close = false) }
                stroke(color("#c8873a"), 0.025, listOf(0.09, 0.07)) { path(listOf(side * 0.92 to 1.45, side * 3.2 to 1.25), close = false) }
                stroke(color("#c8873a"), 0.025, listOf(0.09, 0.07)) { path(listOf(side * 0.92 to 1.58, side * 3.2 to 1.38), close = false) }
                val flap = listOf(side * 1.35 to 1.85, side * 2.45 to 1.85, side * 2.45 to 2.25, side * 1.9 to 2.5, side * 1.35 to 2.25)
                fillWith(fabric(CallOutfitFabricKind.DENIM, "#45679a")) { path(flap) }
                stroke(color("rgba(0,0,0,0.35)"), 0.035) { path(flap) }
                stroke(color("#c8873a"), 0.022, listOf(0.08, 0.06)) { path(flap.map { (it.first - side * 0.0) to (it.second + 0.07) }, close = true) }
                stroke(color("rgba(0,0,0,0.25)"), 0.03) { path(listOf(side * 1.4 to 2.3, side * 1.4 to 3.6, side * 2.4 to 3.6, side * 2.4 to 2.3), close = false) }
                button(side * 1.9, 2.3, 0.08, "#b07a3c")
                val collar = listOf(side * 0.95 to -0.95, side * 1.85 to -0.4, side * 1.5 to 1.25, side * 0.92 to 0.45)
                fillWith(fabric(CallOutfitFabricKind.DENIM, "#557aab")) { path(collar) }
                stroke(color("rgba(0,0,0,0.38)"), 0.04) { path(collar) }
                stroke(color("#c8873a"), 0.022, listOf(0.08, 0.06)) { path(listOf(side * 1.75 to -0.35, side * 1.43 to 1.08, side * 1.0 to 0.45), close = false) }
            }
            var y = 1.9
            while (y < 10) {
                button(-1.05, y, 0.09, "#b07a3c")
                y += 1.25
            }
            folds(listOf(d(-2.3, 2.2, -2.0, 5.0), d(2.3, 2.2, 2.0, 5.0)), 0.12)
            cutTop(0.95, roundNeck(0.72, 0.1, -0.9))
            soften()
        }

        fun flannel() {
            val plaid = fabric(CallOutfitFabricKind.PLAID, "#a8323a", "#2a2a2e", "#5a1c22", "#e6d9b8")
            ops.add(CallOutfitCommand.FillStyle(plaid))
            ops.add(CallOutfitCommand.FillRect(0.0, 0.0, size, size))
            stroke(color("rgba(0,0,0,0.3)"), 0.03) { path(listOf(-0.2 to 0.4, -0.2 to 10.5), close = false) }
            stroke(color("rgba(0,0,0,0.3)"), 0.03) { path(listOf(0.2 to 0.4, 0.2 to 10.5), close = false) }
            var y = 0.9
            while (y < 10) {
                button(0.0, y, 0.085, "#ece6d6")
                y += 1.05
            }
            for (side in listOf(-1.0, 1.0)) {
                val pocket = listOf(side * 1.25 to 1.75, side * 2.3 to 1.75, side * 2.3 to 3.15, side * 1.78 to 3.4, side * 1.25 to 3.15)
                ops.add(CallOutfitCommand.Save)
                path(pocket)
                ops.add(CallOutfitCommand.Clip)
                val corner = at(side * 1.78, 2.5)
                ops.add(CallOutfitCommand.Translate(corner.first, corner.second))
                ops.add(CallOutfitCommand.Rotate(Math.PI / 4))
                ops.add(CallOutfitCommand.FillStyle(plaid))
                ops.add(CallOutfitCommand.FillRect(-200.0, -200.0, 400.0, 400.0))
                ops.add(CallOutfitCommand.Restore)
                stroke(color("rgba(0,0,0,0.38)"), 0.035) { path(pocket) }
                stroke(color("rgba(0,0,0,0.3)"), 0.03) { path(listOf(side * 1.25 to 2.1, side * 2.3 to 2.1), close = false) }
                val collar = listOf(side * 0.86 to -0.85, side * 1.6 to -0.35, side * 1.0 to 1.0, side * 0.15 to 0.45)
                fillWith(fabric(CallOutfitFabricKind.PLAID, "#b23a42", "#303034", "#62202a", "#ecdfbf")) { path(collar) }
                stroke(color("rgba(0,0,0,0.4)"), 0.04) { path(collar) }
            }
            folds(listOf(d(-2.3, 1.8, -2.0, 4.8), d(2.2, 1.8, 2.0, 4.8)), 0.13)
            cutTop(0.86, roundNeck(0.78, 0.25, -0.85))
            soften()
        }

        fun turtleneck() {
            ops.add(CallOutfitCommand.FillStyle(fabric(CallOutfitFabricKind.RIB, "#1e1f23")))
            ops.add(CallOutfitCommand.FillRect(0.0, 0.0, size, size))
            ops.add(CallOutfitCommand.Save)
            curve(-0.86 to -1.18, listOf(d(0.0, -1.0, 0.86, -1.18), d(0.92, -0.72), d(0.0, -0.55, -0.92, -0.72)))
            ops.add(CallOutfitCommand.ClosePath)
            ops.add(CallOutfitCommand.FillStyle(color("rgba(255,255,255,0.07)")))
            ops.add(CallOutfitCommand.Fill)
            ops.add(CallOutfitCommand.Restore)
            stroke(color("rgba(0,0,0,0.5)"), 0.06) { curve(-0.92 to -0.72, listOf(d(0.0, -0.55, 0.92, -0.72))) }
            for (side in listOf(-1.0, 1.0)) {
                stroke(color("rgba(0,0,0,0.35)"), 0.12) { curve((side * 0.9) to -0.7, listOf(d(side * 1.05, -0.2, side * 1.6, -0.05))) }
            }
            folds(listOf(d(-2.2, 1.5, -2.0, 4.8), d(2.2, 1.5, 2.0, 4.8), d(-0.6, -0.3, -0.5, 0.2), d(0.55, -0.35, 0.5, 0.15)), 0.1)
            ops.add(CallOutfitCommand.Save)
            ops.add(CallOutfitCommand.Composite("destination-out"))
            ops.add(CallOutfitCommand.BeginPath)
            moveTo(at(-5.6, -2.2))
            for (point in shoulderLine(0.86)) lineTo(at(point.first, point.second))
            lineTo(at(-0.86, -1.18))
            quadTo(at(0.0, -1.0), at(0.86, -1.18))
            for (point in shoulderLine(0.86).map { -it.first to it.second }.reversed()) lineTo(at(point.first, point.second))
            lineTo(at(5.6, -2.2))
            ops.add(CallOutfitCommand.ClosePath)
            ops.add(CallOutfitCommand.Fill)
            ops.add(CallOutfitCommand.Restore)
            soften()
        }
    }
}
