package com.forkmesh.nativeapp.chat.facestream

internal object FaceStreamBits {
    const val PROB_ONE = 4096
    const val PROB_HALF = 2048
    const val TOP = 1L shl 24
    const val MAX_LENGTH = 16
    const val MAX_INT = 65535
    const val MASK32 = 0xFFFFFFFFL
}

internal class FaceStreamIntModel(zeroPrior: Int = FaceStreamBits.PROB_HALF) {
    val zero = intArrayOf(zeroPrior, zeroPrior)
    val sign = intArrayOf(FaceStreamBits.PROB_HALF)
    val length = IntArray(FaceStreamBits.MAX_LENGTH) { FaceStreamBits.PROB_HALF }
    val mant = IntArray(FaceStreamBits.MAX_LENGTH) { FaceStreamBits.PROB_HALF }

    fun copy(): FaceStreamIntModel {
        val out = FaceStreamIntModel()
        zero.copyInto(out.zero)
        sign.copyInto(out.sign)
        length.copyInto(out.length)
        mant.copyInto(out.mant)
        return out
    }
}

internal fun clampInt(value: Int, low: Int, high: Int): Int = if (value < low) low else if (value > high) high else value

internal class FaceStreamRangeEncoder(prefix: List<Byte> = emptyList()) {
    private var low = 0L
    private var range = FaceStreamBits.MASK32
    private var cache = 0
    private var cacheSize = 1L
    private var started = false
    private val out = ArrayList<Byte>(prefix)

    private fun emit(value: Int) {
        if (!started) {
            started = true
            return
        }
        out.add((value and 0xFF).toByte())
    }

    private fun shiftLow() {
        val low32 = low and FaceStreamBits.MASK32
        val carry = (low ushr 32).toInt()
        if (low32 < 0xFF000000L || carry != 0) {
            var temp = cache
            do {
                emit(temp + carry)
                temp = 0xFF
                cacheSize -= 1
            } while (cacheSize != 0L)
            cache = (low32 ushr 24).toInt()
        }
        cacheSize += 1
        low = (low32 shl 8) and FaceStreamBits.MASK32
    }

    private fun normalize() {
        while (range < FaceStreamBits.TOP) {
            range = (range shl 8) and FaceStreamBits.MASK32
            shiftLow()
        }
    }

    fun bit(probs: IntArray, at: Int, bit: Int) {
        val p = probs[at]
        val bound = (range ushr 12) * p
        if (bit == 0) {
            range = bound
            probs[at] = p + ((FaceStreamBits.PROB_ONE - p) shr 4)
        } else {
            low += bound
            range -= bound
            probs[at] = p - (p shr 4)
        }
        normalize()
    }

    fun direct(value: Long, count: Int) {
        for (i in count - 1 downTo 0) {
            range = range ushr 1
            if ((value ushr i) and 1L == 1L) low += range
            normalize()
        }
    }

    fun finish(): ByteArray {
        for (k in intArrayOf(32, 24)) {
            val mask = (1L shl k) - 1
            val value = (low + mask) and mask.inv()
            if (value < low + range) {
                low = value
                break
            }
        }
        repeat(5) { shiftLow() }
        var end = out.size
        while (end > 0 && out[end - 1] == 0.toByte()) end -= 1
        return ByteArray(end) { out[it] }
    }

    private fun magnitude(model: FaceStreamIntModel, a: Long) {
        var n = 0
        var v = a
        while (v != 0L) {
            n += 1
            v = v ushr 1
        }
        for (i in 1 until n) bit(model.length, i - 1, 1)
        if (n < FaceStreamBits.MAX_LENGTH) bit(model.length, n - 1, 0)
        if (n >= 2) {
            bit(model.mant, n - 2, ((a ushr (n - 2)) and 1L).toInt())
            if (n >= 3) direct(a and ((1L shl (n - 2)) - 1), n - 2)
        }
    }

    fun uint(model: FaceStreamIntModel, ctx: Int, value: Int) {
        val v = clampInt(value, 0, FaceStreamBits.MAX_INT)
        if (v == 0) {
            bit(model.zero, ctx, 0)
            return
        }
        bit(model.zero, ctx, 1)
        magnitude(model, v.toLong())
    }

    fun int(model: FaceStreamIntModel, ctx: Int, value: Int) {
        var v = clampInt(value, -FaceStreamBits.MAX_INT, FaceStreamBits.MAX_INT)
        if (v == 0) {
            bit(model.zero, ctx, 0)
            return
        }
        bit(model.zero, ctx, 1)
        if (v < 0) {
            bit(model.sign, 0, 1)
            v = -v
        } else {
            bit(model.sign, 0, 0)
        }
        magnitude(model, v.toLong())
    }
}

internal class FaceStreamRangeDecoder(private val data: ByteArray, private val start: Int = 0) {
    private var code = 0L
    private var range = FaceStreamBits.MASK32
    private var pos = start

    init {
        repeat(4) { code = ((code shl 8) or next().toLong()) and FaceStreamBits.MASK32 }
    }

    private fun next(): Int {
        val value = if (pos < data.size) data[pos].toInt() and 0xFF else 0
        pos += 1
        return value
    }

    private fun normalize() {
        while (range < FaceStreamBits.TOP) {
            range = (range shl 8) and FaceStreamBits.MASK32
            code = ((code shl 8) or next().toLong()) and FaceStreamBits.MASK32
        }
    }

    fun bit(probs: IntArray, at: Int): Int {
        val p = probs[at]
        val bound = (range ushr 12) * p
        return if (code < bound) {
            range = bound
            probs[at] = p + ((FaceStreamBits.PROB_ONE - p) shr 4)
            normalize()
            0
        } else {
            code -= bound
            range -= bound
            probs[at] = p - (p shr 4)
            normalize()
            1
        }
    }

    fun direct(count: Int): Long {
        var value = 0L
        repeat(count) {
            range = range ushr 1
            var bit = 0L
            if (code >= range) {
                code -= range
                bit = 1L
            }
            value = (value shl 1) or bit
            normalize()
        }
        return value
    }

    val overrun: Boolean get() = pos - start > data.size - start + 8

    private fun magnitude(model: FaceStreamIntModel): Int {
        var n = 1
        while (n < FaceStreamBits.MAX_LENGTH && bit(model.length, n - 1) == 1) n += 1
        var a = 1L
        if (n >= 2) {
            a = (a shl 1) or bit(model.mant, n - 2).toLong()
            if (n >= 3) a = (a shl (n - 2)) or direct(n - 2)
        }
        return a.toInt()
    }

    fun uint(model: FaceStreamIntModel, ctx: Int): Int = if (bit(model.zero, ctx) == 0) 0 else magnitude(model)

    fun int(model: FaceStreamIntModel, ctx: Int): Int {
        if (bit(model.zero, ctx) == 0) return 0
        val negative = bit(model.sign, 0) == 1
        val a = magnitude(model)
        return if (negative) -a else a
    }
}
