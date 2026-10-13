package com.forkmesh.nativeapp.vault

class Blake2b(private val outputLength: Int) {
    private val h = IV.copyOf()
    private val buffer = ByteArray(128)
    private var buffered = 0
    private var total = 0L
    private val m = LongArray(16)
    private val v = LongArray(16)

    init {
        require(outputLength in 1..64) { "BLAKE2b digest length out of range" }
        h[0] = h[0] xor (0x01010000L xor outputLength.toLong())
    }

    fun update(data: ByteArray, from: Int = 0, length: Int = data.size - from): Blake2b {
        var i = from
        val end = from + length
        while (i < end) {
            if (buffered == 128) {
                total += 128
                compress(last = false)
                buffered = 0
            }
            val take = minOf(128 - buffered, end - i)
            System.arraycopy(data, i, buffer, buffered, take)
            buffered += take
            i += take
        }
        return this
    }

    fun digest(): ByteArray {
        total += buffered
        for (i in buffered until 128) buffer[i] = 0
        compress(last = true)
        val out = ByteArray(outputLength)
        for (i in 0 until outputLength) out[i] = (h[i ushr 3] ushr (8 * (i and 7))).toByte()
        h.fill(0)
        buffer.fill(0)
        return out
    }

    private fun g(a: Int, b: Int, c: Int, d: Int, x: Long, y: Long) {
        v[a] = v[a] + v[b] + x
        v[d] = (v[d] xor v[a]).rotateRight(32)
        v[c] = v[c] + v[d]
        v[b] = (v[b] xor v[c]).rotateRight(24)
        v[a] = v[a] + v[b] + y
        v[d] = (v[d] xor v[a]).rotateRight(16)
        v[c] = v[c] + v[d]
        v[b] = (v[b] xor v[c]).rotateRight(63)
    }

    private fun compress(last: Boolean) {
        for (i in 0 until 16) {
            var word = 0L
            for (j in 0 until 8) word = word or ((buffer[8 * i + j].toLong() and 0xff) shl (8 * j))
            m[i] = word
        }
        for (i in 0 until 8) {
            v[i] = h[i]
            v[i + 8] = IV[i]
        }
        v[12] = v[12] xor total
        if (last) v[14] = v[14].inv()
        for (round in 0 until 12) {
            val s = SIGMA[round % 10]
            g(0, 4, 8, 12, m[s[0]], m[s[1]])
            g(1, 5, 9, 13, m[s[2]], m[s[3]])
            g(2, 6, 10, 14, m[s[4]], m[s[5]])
            g(3, 7, 11, 15, m[s[6]], m[s[7]])
            g(0, 5, 10, 15, m[s[8]], m[s[9]])
            g(1, 6, 11, 12, m[s[10]], m[s[11]])
            g(2, 7, 8, 13, m[s[12]], m[s[13]])
            g(3, 4, 9, 14, m[s[14]], m[s[15]])
        }
        for (i in 0 until 8) h[i] = h[i] xor v[i] xor v[i + 8]
        m.fill(0)
        v.fill(0)
    }

    companion object {
        private val IV = longArrayOf(
            0x6a09e667f3bcc908L, -0x4498517a7b3558c5L, 0x3c6ef372fe94f82bL, -0x5ab00ac5a0e2c90fL,
            0x510e527fade682d1L, -0x64fa9773d4c193e1L, 0x1f83d9abfb41bd6bL, 0x5be0cd19137e2179L,
        )

        private val SIGMA = arrayOf(
            intArrayOf(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15),
            intArrayOf(14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3),
            intArrayOf(11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4),
            intArrayOf(7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8),
            intArrayOf(9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13),
            intArrayOf(2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9),
            intArrayOf(12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11),
            intArrayOf(13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10),
            intArrayOf(6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5),
            intArrayOf(10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0),
        )

        fun hash(outputLength: Int, vararg parts: ByteArray): ByteArray {
            val state = Blake2b(outputLength)
            for (part in parts) state.update(part)
            return state.digest()
        }
    }
}
