package com.forkmesh.nativeapp.qr

import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.roundToInt

class LuminanceImage(val width: Int, val height: Int, val pixels: IntArray) {
    init {
        require(pixels.size >= width * height)
    }

    operator fun get(x: Int, y: Int): Int = pixels[y * width + x]
}

object QrReader {
    private class Point(var x: Float, var y: Float, var moduleSize: Float, var count: Int = 1)

    private class BitImage(val width: Int, val height: Int, val dark: BooleanArray) {
        operator fun get(x: Int, y: Int): Boolean = dark[y * width + x]
    }

    fun read(image: LuminanceImage): String? {
        val bits = binarize(image)
        return readBits(bits) ?: readBits(BitImage(bits.width, bits.height, BooleanArray(bits.dark.size) { !bits.dark[it] }))
    }

    private fun readBits(bits: BitImage): String? {
        val finders = findFinders(bits)
        if (finders.size < 3) return null
        for (triple in rankTriples(finders).take(4)) {
            val ordered = order(triple)
            val (bl, tl, tr) = ordered
            val module = moduleAlong(bits, tl, tr, bl) ?: ((bl.moduleSize + tl.moduleSize + tr.moduleSize) / 3f)
            for (dimension in dimensions(tl, tr, bl, module)) {
                decodeAt(bits, tl, tr, bl, module, dimension)?.let { return it }
            }
        }
        return null
    }

    private fun decodeAt(bits: BitImage, tl: Point, tr: Point, bl: Point, module: Float, dimension: Int): String? {
        val grids = ArrayList<Array<BooleanArray>>()
        val alignment = if (dimension > 21) findAlignment(bits, tl, tr, bl, module, dimension) else null
        if (alignment != null) sample(bits, tl, tr, bl, alignment, dimension, dimension - 6.5f)?.let(grids::add)
        val brX = tr.x - tl.x + bl.x
        val brY = tr.y - tl.y + bl.y
        sample(bits, tl, tr, bl, Point(brX, brY, module), dimension, dimension - 3.5f)?.let(grids::add)
        for (grid in grids) {
            QrDecoder.decode(grid)?.let { return it }
            val ver = QrDecoder.readVersion(grid)
            if (ver != null && ver * 4 + 17 != dimension && ver >= 7) {
                val corrected = ver * 4 + 17
                sample(bits, tl, tr, bl, Point(brX, brY, module), corrected, corrected - 3.5f)?.let { g -> QrDecoder.decode(g)?.let { return it } }
            }
            QrDecoder.decode(transpose(grid))?.let { return it }
        }
        return null
    }

    private fun transpose(grid: Array<BooleanArray>): Array<BooleanArray> = Array(grid.size) { y -> BooleanArray(grid.size) { x -> grid[x][y] } }

    private fun binarize(image: LuminanceImage): BitImage {
        val w = image.width
        val h = image.height
        val dark = BooleanArray(w * h)
        if (w < 40 || h < 40) {
            var sum = 0L
            for (i in 0 until w * h) sum += image.pixels[i]
            val mean = (sum / maxOf(1, w * h)).toInt()
            for (i in 0 until w * h) dark[i] = image.pixels[i] < mean
            return BitImage(w, h, dark)
        }
        val block = 8
        val bw = (w + block - 1) / block
        val bh = (h + block - 1) / block
        val black = Array(bh) { IntArray(bw) }
        for (by in 0 until bh) {
            for (bx in 0 until bw) {
                val y0 = minOf(by * block, h - block)
                val x0 = minOf(bx * block, w - block)
                var sum = 0
                var min = 255
                var max = 0
                for (y in y0 until y0 + block) {
                    for (x in x0 until x0 + block) {
                        val v = image[x, y]
                        sum += v
                        if (v < min) min = v
                        if (v > max) max = v
                    }
                }
                var average = sum / (block * block)
                if (max - min <= 24) {
                    average = min / 2
                    if (by > 0 && bx > 0) {
                        val neighbours = (black[by - 1][bx] + 2 * black[by][bx - 1] + black[by - 1][bx - 1]) / 4
                        if (min < neighbours) average = neighbours
                    }
                }
                black[by][bx] = average
            }
        }
        for (by in 0 until bh) {
            for (bx in 0 until bw) {
                val cx = bx.coerceIn(2, maxOf(2, bw - 3))
                val cy = by.coerceIn(2, maxOf(2, bh - 3))
                var sum = 0
                var n = 0
                for (dy in -2..2) for (dx in -2..2) {
                    val yy = cy + dy
                    val xx = cx + dx
                    if (yy in 0 until bh && xx in 0 until bw) {
                        sum += black[yy][xx]
                        n++
                    }
                }
                val threshold = sum / n
                val y0 = minOf(by * block, h - block)
                val x0 = minOf(bx * block, w - block)
                for (y in y0 until y0 + block) for (x in x0 until x0 + block) dark[y * w + x] = image[x, y] <= threshold
            }
        }
        return BitImage(w, h, dark)
    }

    private fun matches(counts: IntArray, weights: IntArray): Boolean {
        var total = 0
        for (c in counts) {
            if (c == 0) return false
            total += c
        }
        val units = weights.sum()
        if (total < units) return false
        val module = total.toFloat() / units
        val variance = module / 2f
        for (i in counts.indices) if (abs(module * weights[i] - counts[i]) >= variance * weights[i]) return false
        return true
    }

    private fun crossCheck(bits: BitImage, fixed: Int, start: Int, vertical: Boolean, maxCount: Int, originalTotal: Int, weights: IntArray): Float {
        val limit = if (vertical) bits.height else bits.width
        fun dark(p: Int): Boolean = if (vertical) bits[fixed, p] else bits[p, fixed]
        val c = IntArray(5)
        var p = start
        while (p >= 0 && dark(p)) { c[2]++; p-- }
        if (p < 0) return Float.NaN
        while (p >= 0 && !dark(p) && c[1] <= maxCount) { c[1]++; p-- }
        if (p < 0 || c[1] > maxCount) return Float.NaN
        while (p >= 0 && dark(p) && c[0] <= maxCount) { c[0]++; p-- }
        if (c[0] > maxCount) return Float.NaN
        p = start + 1
        while (p < limit && dark(p)) { c[2]++; p++ }
        if (p == limit) return Float.NaN
        while (p < limit && !dark(p) && c[3] < maxCount) { c[3]++; p++ }
        if (p == limit || c[3] >= maxCount) return Float.NaN
        while (p < limit && dark(p) && c[4] < maxCount) { c[4]++; p++ }
        if (c[4] >= maxCount) return Float.NaN
        val total = c.sum()
        if (5 * abs(total - originalTotal) >= 2 * originalTotal) return Float.NaN
        return if (matches(c, weights)) (p - c[4] - c[3]).toFloat() - c[2] / 2f else Float.NaN
    }

    private fun scan(bits: BitImage, weights: IntArray, x0: Int, x1: Int, y0: Int, y1: Int, found: (Float, Float, Float) -> Unit) {
        val maxCount = { c: IntArray -> maxOf(c[2], c.maxOrNull() ?: 0) * 2 }
        fun handle(c: IntArray, y: Int, end: Int) {
            val total = c.sum()
            val cx = (end - c[4] - c[3]).toFloat() - c[2] / 2f
            val cy = crossCheck(bits, cx.toInt(), y, true, maxCount(c), total, weights)
            if (cy.isNaN()) return
            val cx2 = crossCheck(bits, cy.toInt(), cx.toInt(), false, maxCount(c), total, weights)
            if (cx2.isNaN()) return
            found(cx2, cy, total.toFloat() / weights.sum())
        }
        for (y in y0 until y1) {
            val c = IntArray(5)
            var state = 0
            for (x in x0 until x1) {
                if (bits[x, y]) {
                    if (state and 1 == 1) state++
                    c[state]++
                } else {
                    if (state and 1 == 0) {
                        if (state == 4) {
                            if (matches(c, weights)) handle(c, y, x)
                            c[0] = c[2]; c[1] = c[3]; c[2] = c[4]; c[3] = 1; c[4] = 0
                            state = 3
                        } else {
                            state++
                            c[state]++
                        }
                    } else {
                        c[state]++
                    }
                }
            }
            if (state == 4 && matches(c, weights)) handle(c, y, x1)
        }
    }

    private fun findFinders(bits: BitImage): List<Point> {
        val points = ArrayList<Point>()
        scan(bits, intArrayOf(1, 1, 3, 1, 1), 0, bits.width, 0, bits.height) { x, y, m ->
            val near = points.firstOrNull { abs(it.x - x) <= it.moduleSize && abs(it.y - y) <= it.moduleSize && abs(it.moduleSize - m) <= maxOf(1f, it.moduleSize) }
            if (near == null) {
                points.add(Point(x, y, m))
            } else {
                val n = near.count
                near.x = (near.x * n + x) / (n + 1)
                near.y = (near.y * n + y) / (n + 1)
                near.moduleSize = (near.moduleSize * n + m) / (n + 1)
                near.count = n + 1
            }
        }
        val strong = points.filter { it.count >= 2 }
        return (if (strong.size >= 3) strong else points).sortedByDescending { it.count }.take(12)
    }

    private fun dist(a: Point, b: Point): Float = hypot(a.x - b.x, a.y - b.y)

    private fun rankTriples(points: List<Point>): List<List<Point>> {
        val scored = ArrayList<Pair<Float, List<Point>>>()
        for (i in points.indices) for (j in i + 1 until points.size) for (k in j + 1 until points.size) {
            val t = listOf(points[i], points[j], points[k])
            val sizes = t.map { it.moduleSize }
            val mean = sizes.average().toFloat()
            val spread = (sizes.max() - sizes.min()) / mean
            if (spread > 0.5f) continue
            val d = listOf(dist(t[0], t[1]), dist(t[1], t[2]), dist(t[0], t[2])).sorted()
            if (d[0] < mean * 10f) continue
            val legs = abs(d[0] - d[1]) / d[1]
            val hyp = abs(d[2] * d[2] - (d[0] * d[0] + d[1] * d[1])) / (d[2] * d[2])
            if (legs > 0.3f || hyp > 0.3f) continue
            scored.add((legs + hyp + spread) to t)
        }
        return scored.sortedBy { it.first }.map { it.second }
    }

    private fun order(t: List<Point>): Triple<Point, Point, Point> {
        val ab = dist(t[0], t[1])
        val bc = dist(t[1], t[2])
        val ac = dist(t[0], t[2])
        var a: Point
        val b: Point
        var c: Point
        if (bc >= ab && bc >= ac) {
            b = t[0]; a = t[1]; c = t[2]
        } else if (ac >= bc && ac >= ab) {
            b = t[1]; a = t[0]; c = t[2]
        } else {
            b = t[2]; a = t[0]; c = t[1]
        }
        val cross = (c.x - b.x) * (a.y - b.y) - (c.y - b.y) * (a.x - b.x)
        if (cross < 0f) {
            val swap = a
            a = c
            c = swap
        }
        return Triple(a, b, c)
    }

    private fun runFrom(bits: BitImage, p: Point, dx: Float, dy: Float): Float {
        var state = 0
        var t = 0f
        var last = true
        while (true) {
            val x = (p.x + dx * t).toInt()
            val y = (p.y + dy * t).toInt()
            if (x < 0 || y < 0 || x >= bits.width || y >= bits.height) return Float.NaN
            val dark = bits[x, y]
            if (dark != last) {
                state++
                last = dark
                if (state == 3) return t
            }
            t += 1f
            if (t > 4000f) return Float.NaN
        }
    }

    private fun moduleAlong(bits: BitImage, tl: Point, tr: Point, bl: Point): Float? {
        val sizes = ArrayList<Float>()
        for ((a, b) in listOf(tl to tr, tr to tl, tl to bl, bl to tl)) {
            val d = dist(a, b)
            if (d == 0f) continue
            val dx = (b.x - a.x) / d
            val dy = (b.y - a.y) / d
            val run = runFrom(bits, a, dx, dy) + runFrom(bits, a, -dx, -dy)
            if (!run.isNaN()) sizes.add(run / 7f)
        }
        if (sizes.isEmpty()) return null
        return sizes.average().toFloat()
    }

    private fun dimensions(tl: Point, tr: Point, bl: Point, module: Float): List<Int> {
        val estimate = ((dist(tl, tr) / module + dist(tl, bl) / module) / 2f).roundToInt() + 7
        val out = LinkedHashSet<Int>()
        when (estimate and 3) {
            0 -> { out.add(estimate + 1); out.add(estimate - 3) }
            1 -> { out.add(estimate); out.add(estimate + 4); out.add(estimate - 4) }
            2 -> { out.add(estimate - 1); out.add(estimate + 3) }
            else -> { out.add(estimate - 2); out.add(estimate + 2) }
        }
        return out.filter { it in 21..177 }
    }

    private fun findAlignment(bits: BitImage, tl: Point, tr: Point, bl: Point, module: Float, dimension: Int): Point? {
        val brX = tr.x - tl.x + bl.x
        val brY = tr.y - tl.y + bl.y
        val between = dimension - 7
        val correction = 1f - 3f / between
        val estX = tl.x + correction * (brX - tl.x)
        val estY = tl.y + correction * (brY - tl.y)
        for (factor in intArrayOf(4, 8, 16)) {
            val allowance = (factor * module).toInt()
            val x0 = maxOf(0, (estX - allowance).toInt())
            val x1 = minOf(bits.width, (estX + allowance).toInt())
            val y0 = maxOf(0, (estY - allowance).toInt())
            val y1 = minOf(bits.height, (estY + allowance).toInt())
            if (x1 - x0 < module * 3 || y1 - y0 < module * 3) continue
            var best: Point? = null
            var bestD = Float.MAX_VALUE
            scan(bits, intArrayOf(1, 1, 1, 1, 1), x0, x1, y0, y1) { x, y, m ->
                if (abs(m - module) <= module * 0.6f) {
                    val d = hypot(x - estX, y - estY)
                    if (d < bestD) {
                        bestD = d
                        best = Point(x, y, m)
                    }
                }
            }
            if (best != null) return best
        }
        return null
    }

    private class Transform(val a11: Float, val a21: Float, val a31: Float, val a12: Float, val a22: Float, val a32: Float, val a13: Float, val a23: Float, val a33: Float) {
        fun times(o: Transform) = Transform(
            a11 * o.a11 + a21 * o.a12 + a31 * o.a13, a11 * o.a21 + a21 * o.a22 + a31 * o.a23, a11 * o.a31 + a21 * o.a32 + a31 * o.a33,
            a12 * o.a11 + a22 * o.a12 + a32 * o.a13, a12 * o.a21 + a22 * o.a22 + a32 * o.a23, a12 * o.a31 + a22 * o.a32 + a32 * o.a33,
            a13 * o.a11 + a23 * o.a12 + a33 * o.a13, a13 * o.a21 + a23 * o.a22 + a33 * o.a23, a13 * o.a31 + a23 * o.a32 + a33 * o.a33,
        )

        fun adjoint() = Transform(
            a22 * a33 - a23 * a32, a23 * a31 - a21 * a33, a21 * a32 - a22 * a31,
            a13 * a32 - a12 * a33, a11 * a33 - a13 * a31, a12 * a31 - a11 * a32,
            a12 * a23 - a13 * a22, a13 * a21 - a11 * a23, a11 * a22 - a12 * a21,
        )

        fun map(x: Float, y: Float): Pair<Float, Float> {
            val den = a13 * x + a23 * y + a33
            return ((a11 * x + a21 * y + a31) / den) to ((a12 * x + a22 * y + a32) / den)
        }

        companion object {
            fun squareToQuad(x0: Float, y0: Float, x1: Float, y1: Float, x2: Float, y2: Float, x3: Float, y3: Float): Transform {
                val dx3 = x0 - x1 + x2 - x3
                val dy3 = y0 - y1 + y2 - y3
                if (dx3 == 0f && dy3 == 0f) return Transform(x1 - x0, x2 - x1, x0, y1 - y0, y2 - y1, y0, 0f, 0f, 1f)
                val dx1 = x1 - x2
                val dx2 = x3 - x2
                val dy1 = y1 - y2
                val dy2 = y3 - y2
                val den = dx1 * dy2 - dx2 * dy1
                val a13 = (dx3 * dy2 - dx2 * dy3) / den
                val a23 = (dx1 * dy3 - dx3 * dy1) / den
                return Transform(x1 - x0 + a13 * x1, x3 - x0 + a23 * x3, x0, y1 - y0 + a13 * y1, y3 - y0 + a23 * y3, y0, a13, a23, 1f)
            }

            fun quadToQuad(src: FloatArray, dst: FloatArray): Transform {
                val q2s = squareToQuad(src[0], src[1], src[2], src[3], src[4], src[5], src[6], src[7]).adjoint()
                val s2q = squareToQuad(dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7])
                return s2q.times(q2s)
            }
        }
    }

    private fun sample(bits: BitImage, tl: Point, tr: Point, bl: Point, br: Point, dimension: Int, brModule: Float): Array<BooleanArray>? {
        val t = Transform.quadToQuad(
            floatArrayOf(3.5f, 3.5f, dimension - 3.5f, 3.5f, brModule, brModule, 3.5f, dimension - 3.5f),
            floatArrayOf(tl.x, tl.y, tr.x, tr.y, br.x, br.y, bl.x, bl.y),
        )
        val grid = Array(dimension) { BooleanArray(dimension) }
        for (y in 0 until dimension) {
            for (x in 0 until dimension) {
                val (px, py) = t.map(x + 0.5f, y + 0.5f)
                val ix = px.toInt()
                val iy = py.toInt()
                if (px.isNaN() || ix < 0 || iy < 0 || ix >= bits.width || iy >= bits.height) return null
                grid[y][x] = bits[ix, iy]
            }
        }
        return grid
    }
}
