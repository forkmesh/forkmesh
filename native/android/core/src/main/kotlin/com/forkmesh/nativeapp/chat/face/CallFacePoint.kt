package com.forkmesh.nativeapp.chat.face

data class CallFacePoint(val x: Double, val y: Double, val z: Double? = null) {
    val dims: Int get() = if (z == null) 2 else 3
    val flat: CallFacePoint get() = CallFacePoint(x, y)
    val values: DoubleArray get() = if (z == null) doubleArrayOf(x, y) else doubleArrayOf(x, y, z)
    internal val depth: Double get() = z ?: Double.NaN

    companion object {
        fun of(values: DoubleArray): CallFacePoint = CallFacePoint(
            if (values.isNotEmpty()) values[0] else Double.NaN,
            if (values.size > 1) values[1] else Double.NaN,
            if (values.size > 2) values[2] else null,
        )

        fun of(values: List<Double>): CallFacePoint = of(values.toDoubleArray())
    }
}

internal object CallFaceJS {
    fun hypot(a: Double, b: Double): Double {
        if (a.isNaN() || b.isNaN()) return if (a.isInfinite() || b.isInfinite()) Double.POSITIVE_INFINITY else Double.NaN
        val x = Math.abs(a)
        val y = Math.abs(b)
        val top = if (x > y) x else y
        if (top == Double.POSITIVE_INFINITY) return Double.POSITIVE_INFINITY
        if (top == 0.0) return 0.0
        var sum = 0.0
        var compensation = 0.0
        for (value in doubleArrayOf(x, y)) {
            val n = value / top
            val summand = n * n - compensation
            val preliminary = sum + summand
            compensation = (preliminary - sum) - summand
            sum = preliminary
        }
        return StrictMath.sqrt(sum) * top
    }

    fun hypot(a: Double, b: Double, c: Double): Double {
        val values = doubleArrayOf(Math.abs(a), Math.abs(b), Math.abs(c))
        var top = 0.0
        var nan = false
        for (value in values) {
            if (value.isNaN()) nan = true else if (value > top) top = value
        }
        if (top == Double.POSITIVE_INFINITY) return Double.POSITIVE_INFINITY
        if (nan) return Double.NaN
        if (top == 0.0) return 0.0
        var sum = 0.0
        var compensation = 0.0
        for (value in values) {
            val n = value / top
            val summand = n * n - compensation
            val preliminary = sum + summand
            compensation = (preliminary - sum) - summand
            sum = preliminary
        }
        return StrictMath.sqrt(sum) * top
    }

    fun round(x: Double): Double {
        if (!x.isFinite()) return x
        val floor = Math.floor(x)
        return if (x - floor >= 0.5) floor + 1 else floor
    }

    fun min(a: Double, b: Double): Double {
        if (a.isNaN() || b.isNaN()) return Double.NaN
        return if (a < b) a else b
    }

    fun max(a: Double, b: Double): Double {
        if (a.isNaN() || b.isNaN()) return Double.NaN
        return if (a > b) a else b
    }

    fun max(vararg values: Double): Double {
        var out = Double.NEGATIVE_INFINITY
        for (value in values) out = max(out, value)
        return out
    }

    fun or(value: Double, fallback: Double): Double = if (value == 0.0 || value.isNaN()) fallback else value

    fun truthy(value: Double?): Boolean = value != null && value != 0.0 && !value.isNaN()

    fun sign(value: Double): Double = if (value > 0) 1.0 else if (value < 0) -1.0 else value

    fun <T> sorted(items: List<T>, compare: (T, T) -> Double): List<T> = items.sortedWith { a, b ->
        val order = compare(a, b)
        if (order < 0) -1 else if (order > 0) 1 else 0
    }
}
