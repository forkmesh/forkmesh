package com.forkmesh.nativeapp.crypto

class Keccak private constructor(private val rate: Int, private val domain: Int) {
    private val state = LongArray(25)
    private val c = LongArray(5)
    private val b = LongArray(25)
    private var offset = 0
    private var squeezing = false

    fun update(data: ByteArray, from: Int = 0, length: Int = data.size - from): Keccak {
        check(!squeezing) { "Keccak already squeezing" }
        for (i in from until from + length) {
            val lane = offset ushr 3
            state[lane] = state[lane] xor ((data[i].toLong() and 0xff) shl ((offset and 7) * 8))
            if (++offset == rate) {
                permute()
                offset = 0
            }
        }
        return this
    }

    fun squeeze(out: ByteArray, from: Int = 0, length: Int = out.size - from) {
        if (!squeezing) finish()
        for (i in from until from + length) {
            if (offset == rate) {
                permute()
                offset = 0
            }
            out[i] = (state[offset ushr 3] ushr ((offset and 7) * 8)).toByte()
            offset++
        }
    }

    fun digest(length: Int): ByteArray = ByteArray(length).also { squeeze(it) }

    private fun xorByte(position: Int, value: Int) {
        val lane = position ushr 3
        state[lane] = state[lane] xor ((value.toLong() and 0xff) shl ((position and 7) * 8))
    }

    private fun finish() {
        xorByte(offset, domain)
        xorByte(rate - 1, 0x80)
        permute()
        offset = 0
        squeezing = true
    }

    private fun permute() {
        val a = state
        for (round in 0 until 24) {
            for (x in 0 until 5) c[x] = a[x] xor a[x + 5] xor a[x + 10] xor a[x + 15] xor a[x + 20]
            for (x in 0 until 5) {
                val d = c[(x + 4) % 5] xor java.lang.Long.rotateLeft(c[(x + 1) % 5], 1)
                for (y in 0 until 5) a[x + 5 * y] = a[x + 5 * y] xor d
            }
            for (x in 0 until 5) {
                for (y in 0 until 5) {
                    b[y + 5 * ((2 * x + 3 * y) % 5)] = java.lang.Long.rotateLeft(a[x + 5 * y], ROTATIONS[x + 5 * y])
                }
            }
            for (y in 0 until 5) {
                val row = 5 * y
                for (x in 0 until 5) {
                    a[x + row] = b[x + row] xor (b[(x + 1) % 5 + row].inv() and b[(x + 2) % 5 + row])
                }
            }
            a[0] = a[0] xor ROUND_CONSTANTS[round]
        }
    }

    companion object {
        private val ROUND_CONSTANTS = LongArray(24).also { constants ->
            var lfsr = 1
            for (round in 0 until 24) {
                var constant = 0L
                for (j in 0 until 7) {
                    if ((lfsr and 1) != 0) constant = constant xor (1L shl ((1 shl j) - 1))
                    lfsr = lfsr shl 1
                    if ((lfsr and 0x100) != 0) lfsr = lfsr xor 0x171
                }
                constants[round] = constant
            }
        }

        private val ROTATIONS = IntArray(25).also { offsets ->
            var x = 1
            var y = 0
            for (t in 0 until 24) {
                offsets[x + 5 * y] = ((t + 1) * (t + 2) / 2) % 64
                val nextY = (2 * x + 3 * y) % 5
                x = y
                y = nextY
            }
        }

        fun sha3_256(): Keccak = Keccak(136, 0x06)
        fun sha3_512(): Keccak = Keccak(72, 0x06)
        fun shake128(): Keccak = Keccak(168, 0x1F)
        fun shake256(): Keccak = Keccak(136, 0x1F)

        fun sha3_256(vararg parts: ByteArray): ByteArray = sha3_256().also { k -> parts.forEach { k.update(it) } }.digest(32)

        fun sha3_512(vararg parts: ByteArray): ByteArray = sha3_512().also { k -> parts.forEach { k.update(it) } }.digest(64)

        fun shake128(length: Int, vararg parts: ByteArray): ByteArray =
            shake128().also { k -> parts.forEach { k.update(it) } }.digest(length)

        fun shake256(length: Int, vararg parts: ByteArray): ByteArray =
            shake256().also { k -> parts.forEach { k.update(it) } }.digest(length)
    }
}
