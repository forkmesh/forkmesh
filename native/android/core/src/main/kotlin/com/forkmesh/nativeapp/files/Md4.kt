package com.forkmesh.nativeapp.files

object Md4 {
    fun digest(input: ByteArray): ByteArray {
        val bitLength = input.size.toLong() * 8
        val padded = ByteArray(((input.size + 8) / 64 + 1) * 64)
        input.copyInto(padded)
        padded[input.size] = 0x80.toByte()
        for (i in 0 until 8) padded[padded.size - 8 + i] = (bitLength ushr (8 * i)).toByte()
        var a = 0x67452301
        var b = 0xefcdab89.toInt()
        var c = 0x98badcfe.toInt()
        var d = 0x10325476
        val x = IntArray(16)
        for (block in padded.indices step 64) {
            for (i in 0 until 16) {
                val o = block + i * 4
                x[i] = (padded[o].toInt() and 0xff) or ((padded[o + 1].toInt() and 0xff) shl 8) or
                    ((padded[o + 2].toInt() and 0xff) shl 16) or ((padded[o + 3].toInt() and 0xff) shl 24)
            }
            val aa = a
            val bb = b
            val cc = c
            val dd = d
            fun f(x: Int, y: Int, z: Int) = (x and y) or (x.inv() and z)
            fun g(x: Int, y: Int, z: Int) = (x and y) or (x and z) or (y and z)
            fun h(x: Int, y: Int, z: Int) = x xor y xor z
            for (i in 0 until 4) {
                val k = i * 4
                a = Integer.rotateLeft(a + f(b, c, d) + x[k], 3)
                d = Integer.rotateLeft(d + f(a, b, c) + x[k + 1], 7)
                c = Integer.rotateLeft(c + f(d, a, b) + x[k + 2], 11)
                b = Integer.rotateLeft(b + f(c, d, a) + x[k + 3], 19)
            }
            for (i in 0 until 4) {
                a = Integer.rotateLeft(a + g(b, c, d) + x[i] + 0x5a827999, 3)
                d = Integer.rotateLeft(d + g(a, b, c) + x[i + 4] + 0x5a827999, 5)
                c = Integer.rotateLeft(c + g(d, a, b) + x[i + 8] + 0x5a827999, 9)
                b = Integer.rotateLeft(b + g(c, d, a) + x[i + 12] + 0x5a827999, 13)
            }
            for (i in intArrayOf(0, 2, 1, 3)) {
                a = Integer.rotateLeft(a + h(b, c, d) + x[i] + 0x6ed9eba1, 3)
                d = Integer.rotateLeft(d + h(a, b, c) + x[i + 8] + 0x6ed9eba1, 9)
                c = Integer.rotateLeft(c + h(d, a, b) + x[i + 4] + 0x6ed9eba1, 11)
                b = Integer.rotateLeft(b + h(c, d, a) + x[i + 12] + 0x6ed9eba1, 15)
            }
            a += aa
            b += bb
            c += cc
            d += dd
        }
        val out = ByteArray(16)
        for ((index, word) in intArrayOf(a, b, c, d).withIndex()) {
            for (i in 0 until 4) out[index * 4 + i] = (word ushr (8 * i)).toByte()
        }
        return out
    }
}
