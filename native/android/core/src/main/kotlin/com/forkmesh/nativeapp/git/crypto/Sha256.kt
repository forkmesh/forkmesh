package com.forkmesh.nativeapp.git.crypto

import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException

private val roundConstants = intArrayOf(
    0x428a2f98, 0x71374491, 0xb5c0fbcf.toInt(), 0xe9b5dba5.toInt(), 0x3956c25b, 0x59f111f1,
    0x923f82a4.toInt(), 0xab1c5ed5.toInt(), 0xd807aa98.toInt(), 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe.toInt(), 0x9bdc06a7.toInt(), 0xc19bf174.toInt(), 0xe49b69c1.toInt(),
    0xefbe4786.toInt(), 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152.toInt(), 0xa831c66d.toInt(), 0xb00327c8.toInt(), 0xbf597fc7.toInt(), 0xc6e00bf3.toInt(),
    0xd5a79147.toInt(), 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e.toInt(), 0x92722c85.toInt(), 0xa2bfe8a1.toInt(), 0xa81a664b.toInt(),
    0xc24b8b70.toInt(), 0xc76c51a3.toInt(), 0xd192e819.toInt(), 0xd6990624.toInt(), 0xf40e3585.toInt(),
    0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
    0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814.toInt(), 0x8cc70208.toInt(), 0x90befffa.toInt(),
    0xa4506ceb.toInt(), 0xbef9a3f7.toInt(), 0xc67178f2.toInt(),
)

class ForkMeshSha256(maxInputBytes: Long = maximumInputBytes) : GitDigest {
    private val maxInputBytes: Long
    private val state = intArrayOf(
        0x6a09e667, 0xbb67ae85.toInt(), 0x3c6ef372, 0xa54ff53a.toInt(),
        0x510e527f, 0x9b05688c.toInt(), 0x1f83d9ab, 0x5be0cd19,
    )
    private var lengthBytes = 0L
    private val block = ByteArray(64)
    private var blockLength = 0
    private val words = IntArray(64)
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
                "SHA-256 input length exceeds supported accounting range",
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
        for (index in 16 until 64) {
            val sigma0 = Integer.rotateRight(w[index - 15], 7) xor
                Integer.rotateRight(w[index - 15], 18) xor (w[index - 15] ushr 3)
            val sigma1 = Integer.rotateRight(w[index - 2], 17) xor
                Integer.rotateRight(w[index - 2], 19) xor (w[index - 2] ushr 10)
            w[index] = w[index - 16] + sigma0 + w[index - 7] + sigma1
        }
        var a = state[0]
        var b = state[1]
        var c = state[2]
        var d = state[3]
        var e = state[4]
        var f = state[5]
        var g = state[6]
        var h = state[7]
        for (index in 0 until 64) {
            val sum1 = Integer.rotateRight(e, 6) xor Integer.rotateRight(e, 11) xor Integer.rotateRight(e, 25)
            val choice = (e and f) xor (e.inv() and g)
            val temp1 = h + sum1 + choice + roundConstants[index] + w[index]
            val sum0 = Integer.rotateRight(a, 2) xor Integer.rotateRight(a, 13) xor Integer.rotateRight(a, 22)
            val majority = (a and b) xor (a and c) xor (b and c)
            val temp2 = sum0 + majority
            h = g
            g = f
            f = e
            e = d + temp1
            d = c
            c = b
            b = a
            a = temp1 + temp2
        }
        state[0] += a
        state[1] += b
        state[2] += c
        state[3] += d
        state[4] += e
        state[5] += f
        state[6] += g
        state[7] += h
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
        val output = ByteArray(32)
        for (index in state.indices) {
            ForkMeshSha1.writeInt(output, index * 4, state[index])
        }
        return output
    }

    companion object {
        const val maximumInputBytes = 0x1fffffffffffffffL
    }
}
