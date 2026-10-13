package com.forkmesh.nativeapp.git.crypto

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException

class ForkMeshSha1(maxInputBytes: Long = maximumInputBytes) : GitDigest {
    private val maxInputBytes: Long
    private var lengthBytes = 0L
    private val block = ByteArray(64)
    private var blockLength = 0
    private val words = IntArray(80)
    private var h0 = 0x67452301
    private var h1 = 0xefcdab89.toInt()
    private var h2 = 0x98badcfe.toInt()
    private var h3 = 0x10325476
    private var h4 = 0xc3d2e1f0.toInt()
    private var closed = false

    init {
        if (maxInputBytes < 0 || maxInputBytes > maximumInputBytes) {
            throw IllegalArgumentException("Invalid argument (maxInputBytes): $maxInputBytes")
        }
        this.maxInputBytes = maxInputBytes
    }

    override fun add(bytes: ByteArray) = add(bytes, 0, bytes.size)

    override fun add(bytes: ByteArray, offset: Int, length: Int) {
        check(!closed) { "digest is closed" }
        if (length > maxInputBytes - lengthBytes) {
            throw GitException(
                GitErrorCode.hashImplementationFailure,
                "SHA-1 input length exceeds supported accounting range",
            )
        }
        lengthBytes += length
        var position = offset
        val end = offset + length
        if (blockLength > 0) {
            val take = minOf(64 - blockLength, end - position)
            System.arraycopy(bytes, position, block, blockLength, take)
            blockLength += take
            position += take
            if (blockLength == 64) {
                compress(block, 0)
                blockLength = 0
            }
        }
        while (end - position >= 64) {
            compress(bytes, position)
            position += 64
        }
        if (position < end) {
            System.arraycopy(bytes, position, block, 0, end - position)
            blockLength = end - position
        }
    }

    private fun compress(input: ByteArray, offset: Int) {
        val w = words
        for (index in 0 until 16) {
            val base = offset + index * 4
            w[index] = ((input[base].toInt() and 0xff) shl 24) or
                ((input[base + 1].toInt() and 0xff) shl 16) or
                ((input[base + 2].toInt() and 0xff) shl 8) or
                (input[base + 3].toInt() and 0xff)
        }
        for (index in 16 until 80) {
            w[index] = Integer.rotateLeft(w[index - 3] xor w[index - 8] xor w[index - 14] xor w[index - 16], 1)
        }
        var a = h0
        var b = h1
        var c = h2
        var d = h3
        var e = h4
        for (index in 0 until 80) {
            val f: Int
            val k: Int
            when {
                index < 20 -> {
                    f = (b and c) or (b.inv() and d)
                    k = 0x5a827999
                }
                index < 40 -> {
                    f = b xor c xor d
                    k = 0x6ed9eba1
                }
                index < 60 -> {
                    f = (b and c) or (b and d) or (c and d)
                    k = 0x8f1bbcdc.toInt()
                }
                else -> {
                    f = b xor c xor d
                    k = 0xca62c1d6.toInt()
                }
            }
            val temp = Integer.rotateLeft(a, 5) + f + e + k + w[index]
            e = d
            d = c
            c = Integer.rotateLeft(b, 30)
            b = a
            a = temp
        }
        h0 += a
        h1 += b
        h2 += c
        h3 += d
        h4 += e
    }

    override fun close(): ByteArray {
        check(!closed) { "digest is closed" }
        closed = true
        val bitLength = lengthBytes * 8
        val padding = ByteArray(if (blockLength < 56) 64 - blockLength else 128 - blockLength)
        padding[0] = 0x80.toByte()
        for (index in 0 until 8) {
            padding[padding.size - 1 - index] = (bitLength ushr (8 * index)).toByte()
        }
        var position = 0
        while (position < padding.size) {
            val take = minOf(64 - blockLength, padding.size - position)
            System.arraycopy(padding, position, block, blockLength, take)
            blockLength += take
            position += take
            if (blockLength == 64) {
                compress(block, 0)
                blockLength = 0
            }
        }
        val output = ByteArray(20)
        writeInt(output, 0, h0)
        writeInt(output, 4, h1)
        writeInt(output, 8, h2)
        writeInt(output, 12, h3)
        writeInt(output, 16, h4)
        return output
    }

    companion object {
        const val maximumInputBytes = 0x1fffffffffffffffL

        internal fun writeInt(output: ByteArray, offset: Int, value: Int) {
            output[offset] = (value ushr 24).toByte()
            output[offset + 1] = (value ushr 16).toByte()
            output[offset + 2] = (value ushr 8).toByte()
            output[offset + 3] = value.toByte()
        }
    }
}
