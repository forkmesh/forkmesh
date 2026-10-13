package com.forkmesh.nativeapp.crypto

internal object Fe {
    const val LIMBS = 10

    private val WIDTH = intArrayOf(26, 25, 26, 25, 26, 25, 26, 25, 26, 25)

    fun create(): LongArray = LongArray(LIMBS)

    fun zero(h: LongArray) = h.fill(0L)

    fun one(h: LongArray) {
        h.fill(0L)
        h[0] = 1L
    }

    fun copy(h: LongArray, f: LongArray) = System.arraycopy(f, 0, h, 0, LIMBS)

    fun fromHex(littleEndianHex: String): LongArray = create().also { fromBytes(it, Bytes.fromHex(littleEndianHex)) }

    fun fromBytes(h: LongArray, s: ByteArray, offset: Int = 0) {
        h[0] = load4(s, offset) and 0x3ffffffL
        h[1] = (load4(s, offset + 3) ushr 2) and 0x1ffffffL
        h[2] = (load4(s, offset + 6) ushr 3) and 0x3ffffffL
        h[3] = (load4(s, offset + 9) ushr 5) and 0x1ffffffL
        h[4] = (load4(s, offset + 12) ushr 6) and 0x3ffffffL
        h[5] = load4(s, offset + 16) and 0x1ffffffL
        h[6] = (load4(s, offset + 19) ushr 1) and 0x3ffffffL
        h[7] = (load4(s, offset + 22) ushr 3) and 0x1ffffffL
        h[8] = (load4(s, offset + 25) ushr 4) and 0x3ffffffL
        h[9] = (load4(s, offset + 28) ushr 6) and 0x1ffffffL
    }

    fun toBytes(s: ByteArray, f: LongArray, offset: Int = 0) {
        val h = f.copyOf()
        carry(h)
        var q = (19 * h[9] + (1L shl 24)) shr 25
        q = (h[0] + q) shr 26
        q = (h[1] + q) shr 25
        q = (h[2] + q) shr 26
        q = (h[3] + q) shr 25
        q = (h[4] + q) shr 26
        q = (h[5] + q) shr 25
        q = (h[6] + q) shr 26
        q = (h[7] + q) shr 25
        q = (h[8] + q) shr 26
        q = (h[9] + q) shr 25
        h[0] += 19 * q
        for (i in 0 until 9) {
            val c = h[i] shr WIDTH[i]
            h[i + 1] += c
            h[i] -= c shl WIDTH[i]
        }
        h[9] -= (h[9] shr 25) shl 25
        var acc = 0L
        var bits = 0
        var pos = offset
        for (i in 0 until LIMBS) {
            acc = acc or (h[i] shl bits)
            bits += WIDTH[i]
            while (bits >= 8) {
                s[pos++] = acc.toByte()
                acc = acc ushr 8
                bits -= 8
            }
        }
        s[pos] = acc.toByte()
    }

    fun carry(h: LongArray) {
        var c = (h[0] + (1L shl 25)) shr 26; h[1] += c; h[0] -= c shl 26
        c = (h[1] + (1L shl 24)) shr 25; h[2] += c; h[1] -= c shl 25
        c = (h[2] + (1L shl 25)) shr 26; h[3] += c; h[2] -= c shl 26
        c = (h[3] + (1L shl 24)) shr 25; h[4] += c; h[3] -= c shl 25
        c = (h[4] + (1L shl 25)) shr 26; h[5] += c; h[4] -= c shl 26
        c = (h[5] + (1L shl 24)) shr 25; h[6] += c; h[5] -= c shl 25
        c = (h[6] + (1L shl 25)) shr 26; h[7] += c; h[6] -= c shl 26
        c = (h[7] + (1L shl 24)) shr 25; h[8] += c; h[7] -= c shl 25
        c = (h[8] + (1L shl 25)) shr 26; h[9] += c; h[8] -= c shl 26
        c = (h[9] + (1L shl 24)) shr 25; h[0] += c * 19; h[9] -= c shl 25
        c = (h[0] + (1L shl 25)) shr 26; h[1] += c; h[0] -= c shl 26
    }

    fun add(h: LongArray, f: LongArray, g: LongArray) {
        for (i in 0 until LIMBS) h[i] = f[i] + g[i]
        carry(h)
    }

    fun sub(h: LongArray, f: LongArray, g: LongArray) {
        for (i in 0 until LIMBS) h[i] = f[i] - g[i]
        carry(h)
    }

    fun neg(h: LongArray, f: LongArray) {
        for (i in 0 until LIMBS) h[i] = -f[i]
    }

    fun mulSmall(h: LongArray, f: LongArray, n: Long) {
        for (i in 0 until LIMBS) h[i] = f[i] * n
        carry(h)
    }

    fun mul(h: LongArray, f: LongArray, g: LongArray) {
        val f0 = f[0]; val f1 = f[1]; val f2 = f[2]; val f3 = f[3]; val f4 = f[4]
        val f5 = f[5]; val f6 = f[6]; val f7 = f[7]; val f8 = f[8]; val f9 = f[9]
        val g0 = g[0]; val g1 = g[1]; val g2 = g[2]; val g3 = g[3]; val g4 = g[4]
        val g5 = g[5]; val g6 = g[6]; val g7 = g[7]; val g8 = g[8]; val g9 = g[9]
        val g1x19 = 19 * g1; val g2x19 = 19 * g2; val g3x19 = 19 * g3; val g4x19 = 19 * g4
        val g5x19 = 19 * g5; val g6x19 = 19 * g6; val g7x19 = 19 * g7; val g8x19 = 19 * g8; val g9x19 = 19 * g9
        val f1x2 = 2 * f1; val f3x2 = 2 * f3; val f5x2 = 2 * f5; val f7x2 = 2 * f7; val f9x2 = 2 * f9

        h[0] = f0 * g0 + f1x2 * g9x19 + f2 * g8x19 + f3x2 * g7x19 + f4 * g6x19 +
            f5x2 * g5x19 + f6 * g4x19 + f7x2 * g3x19 + f8 * g2x19 + f9x2 * g1x19
        h[1] = f0 * g1 + f1 * g0 + f2 * g9x19 + f3 * g8x19 + f4 * g7x19 +
            f5 * g6x19 + f6 * g5x19 + f7 * g4x19 + f8 * g3x19 + f9 * g2x19
        h[2] = f0 * g2 + f1x2 * g1 + f2 * g0 + f3x2 * g9x19 + f4 * g8x19 +
            f5x2 * g7x19 + f6 * g6x19 + f7x2 * g5x19 + f8 * g4x19 + f9x2 * g3x19
        h[3] = f0 * g3 + f1 * g2 + f2 * g1 + f3 * g0 + f4 * g9x19 +
            f5 * g8x19 + f6 * g7x19 + f7 * g6x19 + f8 * g5x19 + f9 * g4x19
        h[4] = f0 * g4 + f1x2 * g3 + f2 * g2 + f3x2 * g1 + f4 * g0 +
            f5x2 * g9x19 + f6 * g8x19 + f7x2 * g7x19 + f8 * g6x19 + f9x2 * g5x19
        h[5] = f0 * g5 + f1 * g4 + f2 * g3 + f3 * g2 + f4 * g1 +
            f5 * g0 + f6 * g9x19 + f7 * g8x19 + f8 * g7x19 + f9 * g6x19
        h[6] = f0 * g6 + f1x2 * g5 + f2 * g4 + f3x2 * g3 + f4 * g2 +
            f5x2 * g1 + f6 * g0 + f7x2 * g9x19 + f8 * g8x19 + f9x2 * g7x19
        h[7] = f0 * g7 + f1 * g6 + f2 * g5 + f3 * g4 + f4 * g3 +
            f5 * g2 + f6 * g1 + f7 * g0 + f8 * g9x19 + f9 * g8x19
        h[8] = f0 * g8 + f1x2 * g7 + f2 * g6 + f3x2 * g5 + f4 * g4 +
            f5x2 * g3 + f6 * g2 + f7x2 * g1 + f8 * g0 + f9x2 * g9x19
        h[9] = f0 * g9 + f1 * g8 + f2 * g7 + f3 * g6 + f4 * g5 +
            f5 * g4 + f6 * g3 + f7 * g2 + f8 * g1 + f9 * g0
        carry(h)
    }

    fun sq(h: LongArray, f: LongArray) = mul(h, f, f)

    private fun pow2250(out: LongArray, z11: LongArray, z: LongArray) {
        val t1 = create()
        val t2 = create()
        val t3 = create()
        sq(z11, z)
        sq(t1, z11); sq(t1, t1)
        mul(t1, z, t1)
        mul(z11, z11, t1)
        sq(t2, z11)
        mul(t1, t1, t2)
        sq(t2, t1); repeat(4) { sq(t2, t2) }
        mul(t1, t2, t1)
        sq(t2, t1); repeat(9) { sq(t2, t2) }
        mul(t2, t2, t1)
        sq(t3, t2); repeat(19) { sq(t3, t3) }
        mul(t2, t3, t2)
        sq(t2, t2); repeat(9) { sq(t2, t2) }
        mul(t1, t2, t1)
        sq(t2, t1); repeat(49) { sq(t2, t2) }
        mul(t2, t2, t1)
        sq(t3, t2); repeat(99) { sq(t3, t3) }
        mul(t2, t3, t2)
        sq(t2, t2); repeat(49) { sq(t2, t2) }
        mul(out, t2, t1)
    }

    fun invert(out: LongArray, z: LongArray) {
        val t = create()
        val z11 = create()
        pow2250(t, z11, z)
        sq(t, t); repeat(4) { sq(t, t) }
        mul(out, t, z11)
    }

    fun pow22523(out: LongArray, z: LongArray) {
        val t = create()
        val z11 = create()
        pow2250(t, z11, z)
        sq(t, t); sq(t, t)
        mul(out, t, z)
    }

    fun cmov(f: LongArray, g: LongArray, b: Int) {
        val mask = -b.toLong()
        for (i in 0 until LIMBS) f[i] = f[i] xor (mask and (f[i] xor g[i]))
    }

    fun cswap(f: LongArray, g: LongArray, b: Int) {
        val mask = -b.toLong()
        for (i in 0 until LIMBS) {
            val x = mask and (f[i] xor g[i])
            f[i] = f[i] xor x
            g[i] = g[i] xor x
        }
    }

    fun isNegative(f: LongArray): Int {
        val s = ByteArray(32)
        toBytes(s, f)
        return s[0].toInt() and 1
    }

    fun isZero(f: LongArray): Boolean {
        val s = ByteArray(32)
        toBytes(s, f)
        return Bytes.isAllZero(s)
    }

    private fun load4(s: ByteArray, i: Int): Long =
        (s[i].toLong() and 0xff) or
            ((s[i + 1].toLong() and 0xff) shl 8) or
            ((s[i + 2].toLong() and 0xff) shl 16) or
            ((s[i + 3].toLong() and 0xff) shl 24)
}
