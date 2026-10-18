package com.forkmesh.nativeapp.codeinsight

import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.max
import kotlin.math.sqrt

class EmberCircle(var x: Double = 0.0, var y: Double = 0.0, var r: Double = 0.0) {
    fun copy(): EmberCircle = EmberCircle(x, y, r)

    override fun toString(): String = "EmberCircle($x, $y, $r)"
}

object EmberPack {
    fun packSiblings(circles: List<EmberCircle>): Double {
        val n = circles.size
        if (n == 0) return 0.0
        val first = circles[0]
        first.x = 0.0
        first.y = 0.0
        if (n == 1) return first.r
        val second = circles[1]
        first.x = -second.r
        second.x = first.r
        second.y = 0.0
        if (n == 2) {
            val e = encloseBasis2(first, second)
            for (c in circles) {
                c.x -= e.x
                c.y -= e.y
            }
            return e.r
        }
        place(circles[1], circles[0], circles[2])
        val next = IntArray(n)
        val previous = IntArray(n)
        var a = 0
        var b = 1
        var c = 2
        next[a] = b
        previous[c] = b
        next[b] = c
        previous[a] = c
        next[c] = a
        previous[b] = a

        var i = 3
        while (i < n) {
            place(circles[a], circles[b], circles[i])
            c = i
            var j = next[b]
            var k = previous[a]
            var sj = circles[b].r
            var sk = circles[a].r
            var retry = false
            do {
                if (sj <= sk) {
                    if (intersects(circles[j], circles[c])) {
                        b = j
                        next[a] = b
                        previous[b] = a
                        retry = true
                        break
                    }
                    sj += circles[j].r
                    j = next[j]
                } else {
                    if (intersects(circles[k], circles[c])) {
                        a = k
                        next[a] = b
                        previous[b] = a
                        retry = true
                        break
                    }
                    sk += circles[k].r
                    k = previous[k]
                }
            } while (j != next[k])
            if (retry) continue
            previous[c] = a
            next[c] = b
            next[a] = c
            previous[b] = c
            b = c
            var aa = score(circles, a, next)
            while (true) {
                c = next[c]
                if (c == b) break
                val ca = score(circles, c, next)
                if (ca < aa) {
                    a = c
                    aa = ca
                }
            }
            b = next[a]
            i++
        }

        val front = ArrayList<EmberCircle>()
        front.add(circles[b].copy())
        var it = next[b]
        while (it != b) {
            front.add(circles[it].copy())
            it = next[it]
        }
        val e = encloseCircles(front)
        for (circle in circles) {
            circle.x -= e.x
            circle.y -= e.y
        }
        return e.r
    }

    fun encloseCircles(input: List<EmberCircle>): EmberCircle {
        if (input.isEmpty()) return EmberCircle()
        val circles = input.map { it.copy() }.toMutableList()
        var state = 1L
        fun random(): Double {
            state = (1664525L * state + 1013904223L) % 4294967296L
            return state.toDouble() / 4294967296.0
        }
        var m = circles.size
        while (m > 0) {
            val i = (random() * m).toInt()
            m--
            val t = circles[m]
            circles[m] = circles[i]
            circles[i] = t
        }
        var basis: List<EmberCircle> = emptyList()
        var e = EmberCircle()
        var haveE = false
        var guard = 0
        val limit = max(1000, circles.size * 64)
        var i = 0
        while (i < circles.size) {
            val p = circles[i]
            if (haveE && enclosesWeak(e, p)) {
                i++
                continue
            }
            val next = extendBasis(basis, p)
            guard++
            if (next == null || guard > limit) return boundingFallback(input)
            basis = next
            e = encloseBasis(basis)
            haveE = true
            i = 0
        }
        return e
    }

    private fun place(b: EmberCircle, a: EmberCircle, c: EmberCircle) {
        val dx = b.x - a.x
        val dy = b.y - a.y
        val d2 = dx * dx + dy * dy
        if (d2 > 0) {
            var a2 = a.r + c.r
            a2 *= a2
            var b2 = b.r + c.r
            b2 *= b2
            if (a2 > b2) {
                val x = (d2 + b2 - a2) / (2 * d2)
                val y = sqrt(max(0.0, b2 / d2 - x * x))
                c.x = b.x - x * dx - y * dy
                c.y = b.y - x * dy + y * dx
            } else {
                val x = (d2 + a2 - b2) / (2 * d2)
                val y = sqrt(max(0.0, a2 / d2 - x * x))
                c.x = a.x + x * dx - y * dy
                c.y = a.y + x * dy + y * dx
            }
        } else {
            c.x = a.x + c.r
            c.y = a.y
        }
    }

    private fun intersects(a: EmberCircle, b: EmberCircle): Boolean {
        val dr = a.r + b.r - 1e-6
        val dx = b.x - a.x
        val dy = b.y - a.y
        return dr > 0 && dr * dr > dx * dx + dy * dy
    }

    private fun score(circles: List<EmberCircle>, node: Int, next: IntArray): Double {
        val a = circles[node]
        val b = circles[next[node]]
        val ab = a.r + b.r
        val dx = (a.x * b.r + b.x * a.r) / ab
        val dy = (a.y * b.r + b.y * a.r) / ab
        return dx * dx + dy * dy
    }

    private fun enclosesNot(a: EmberCircle, b: EmberCircle): Boolean {
        val dr = a.r - b.r
        val dx = b.x - a.x
        val dy = b.y - a.y
        return dr < 0 || dr * dr < dx * dx + dy * dy
    }

    private fun enclosesWeak(a: EmberCircle, b: EmberCircle): Boolean {
        val dr = a.r - b.r + max(max(a.r, b.r), 1.0) * 1e-9
        val dx = b.x - a.x
        val dy = b.y - a.y
        return dr > 0 && dr * dr > dx * dx + dy * dy
    }

    private fun enclosesWeakAll(a: EmberCircle, basis: List<EmberCircle>): Boolean = basis.all { enclosesWeak(a, it) }

    private fun encloseBasis2(a: EmberCircle, b: EmberCircle): EmberCircle {
        val x21 = b.x - a.x
        val y21 = b.y - a.y
        val r21 = b.r - a.r
        val l = sqrt(x21 * x21 + y21 * y21)
        if (l <= 0) return if (a.r >= b.r) a.copy() else b.copy()
        return EmberCircle((a.x + b.x + x21 / l * r21) / 2, (a.y + b.y + y21 / l * r21) / 2, (l + a.r + b.r) / 2)
    }

    private fun encloseBasis3(a: EmberCircle, b: EmberCircle, c: EmberCircle): EmberCircle {
        val x1 = a.x
        val y1 = a.y
        val r1 = a.r
        val x2 = b.x
        val y2 = b.y
        val r2 = b.r
        val x3 = c.x
        val y3 = c.y
        val r3 = c.r
        val a2 = x1 - x2
        val a3 = x1 - x3
        val b2 = y1 - y2
        val b3 = y1 - y3
        val c2 = r2 - r1
        val c3 = r3 - r1
        val d1 = x1 * x1 + y1 * y1 - r1 * r1
        val d2 = d1 - x2 * x2 - y2 * y2 + r2 * r2
        val d3 = d1 - x3 * x3 - y3 * y3 + r3 * r3
        val ab = a3 * b2 - a2 * b3
        val xa = (b2 * d3 - b3 * d2) / (ab * 2) - x1
        val xb = (b3 * c2 - b2 * c3) / ab
        val ya = (a3 * d2 - a2 * d3) / (ab * 2) - y1
        val yb = (a2 * c3 - a3 * c2) / ab
        val qa = xb * xb + yb * yb - 1
        val qb = 2 * (r1 + xa * xb + ya * yb)
        val qc = xa * xa + ya * ya - r1 * r1
        val r = -(if (abs(qa) > 1e-6) (qb + sqrt(max(0.0, qb * qb - 4 * qa * qc))) / (2 * qa) else qc / qb)
        return EmberCircle(x1 + xa + xb * r, y1 + ya + yb * r, r)
    }

    private fun encloseBasis(basis: List<EmberCircle>): EmberCircle = when (basis.size) {
        1 -> basis[0].copy()
        2 -> encloseBasis2(basis[0], basis[1])
        else -> encloseBasis3(basis[0], basis[1], basis[2])
    }

    private fun extendBasis(basis: List<EmberCircle>, p: EmberCircle): List<EmberCircle>? {
        if (enclosesWeakAll(p, basis)) return listOf(p)
        for (b in basis) {
            if (enclosesNot(p, b) && enclosesWeakAll(encloseBasis2(b, p), basis)) return listOf(b, p)
        }
        for (i in 0 until basis.size - 1) {
            for (j in i + 1 until basis.size) {
                if (enclosesNot(encloseBasis2(basis[i], basis[j]), p) &&
                    enclosesNot(encloseBasis2(basis[i], p), basis[j]) &&
                    enclosesNot(encloseBasis2(basis[j], p), basis[i]) &&
                    enclosesWeakAll(encloseBasis3(basis[i], basis[j], p), basis)
                ) {
                    return listOf(basis[i], basis[j], p)
                }
            }
        }
        return null
    }

    private fun boundingFallback(circles: List<EmberCircle>): EmberCircle {
        var cx = 0.0
        var cy = 0.0
        for (c in circles) {
            cx += c.x
            cy += c.y
        }
        cx /= circles.size
        cy /= circles.size
        var r = 0.0
        for (c in circles) r = max(r, hypot(c.x - cx, c.y - cy) + c.r)
        return EmberCircle(cx, cy, r)
    }
}
