/*
 * Ported from sha1collisiondetection (https://github.com/cr-marcstevens/sha1collisiondetection).
 * Copyright 2017 Marc Stevens <marc@marc-stevens.nl>, Dan Shumow <danshu@microsoft.com>
 * Distributed under the MIT Software License. See Sha1DcLicense.txt beside this file.
 */
package com.forkmesh.nativeapp.git.crypto

class Sha1Dc {
    private var total = 0L
    private val ihv = intArrayOf(0x67452301, 0xEFCDAB89.toInt(), 0x98BADCFE.toInt(), 0x10325476, 0xC3D2E1F0.toInt())
    private val ihv1 = IntArray(5)
    private val ihv2 = IntArray(5)
    private val ihvTmp = IntArray(5)
    private val m1 = IntArray(80)
    private val m2 = IntArray(80)
    private val state58 = IntArray(5)
    private val state65 = IntArray(5)
    private val buffer = ByteArray(64)
    private val block = IntArray(16)
    private var foundCollision = false
    var safeHash = false
    var useUbc = true
    var detectCollision = true
    var reducedRoundCollision = false

    fun update(bytes: ByteArray, offset: Int = 0, length: Int = bytes.size - offset) {
        var off = offset
        var len = length
        if (len == 0) return
        var left = (total and 63L).toInt()
        val fill = 64 - left
        if (left != 0 && len >= fill) {
            total += fill
            System.arraycopy(bytes, off, buffer, left, fill)
            process(buffer, 0)
            off += fill
            len -= fill
            left = 0
        }
        while (len >= 64) {
            total += 64
            process(bytes, off)
            off += 64
            len -= 64
        }
        if (len > 0) {
            total += len
            System.arraycopy(bytes, off, buffer, left, len)
        }
    }

    fun finish(output: ByteArray = ByteArray(20)): Boolean {
        val last = (total and 63L).toInt()
        val padn = if (last < 56) 56 - last else 120 - last
        val padding = ByteArray(padn)
        padding[0] = 0x80.toByte()
        update(padding)
        val bits = (total - padn) shl 3
        for (i in 0 until 8) buffer[56 + i] = (bits ushr (56 - 8 * i)).toByte()
        process(buffer, 0)
        for (i in 0 until 5) {
            output[4 * i] = (ihv[i] ushr 24).toByte()
            output[4 * i + 1] = (ihv[i] ushr 16).toByte()
            output[4 * i + 2] = (ihv[i] ushr 8).toByte()
            output[4 * i + 3] = ihv[i].toByte()
        }
        return foundCollision
    }

    private fun process(bytes: ByteArray, offset: Int) {
        for (i in 0 until 16) {
            val p = offset + 4 * i
            block[i] = ((bytes[p].toInt() and 0xff) shl 24) or ((bytes[p + 1].toInt() and 0xff) shl 16) or
                ((bytes[p + 2].toInt() and 0xff) shl 8) or (bytes[p + 3].toInt() and 0xff)
        }
        System.arraycopy(ihv, 0, ihv1, 0, 5)
        compressionStates()
        if (!detectCollision) return
        val ubcMask = if (useUbc) Sha1DcUbc.check(m1) else -1
        if (ubcMask == 0) return
        for (dv in Sha1DcUbc.vectors) {
            if (ubcMask and (1 shl dv.maskb) == 0) continue
            for (j in 0 until 80) m2[j] = m1[j] xor dv.dm[j]
            recompress(dv.testt, if (dv.testt == 58) state58 else state65)
            val full = (ihvTmp[0] xor ihv[0]) or (ihvTmp[1] xor ihv[1]) or (ihvTmp[2] xor ihv[2]) or
                (ihvTmp[3] xor ihv[3]) or (ihvTmp[4] xor ihv[4])
            val reduced = (ihv1[0] xor ihv2[0]) or (ihv1[1] xor ihv2[1]) or (ihv1[2] xor ihv2[2]) or
                (ihv1[3] xor ihv2[3]) or (ihv1[4] xor ihv2[4])
            if (full == 0 || (reducedRoundCollision && reduced == 0)) {
                foundCollision = true
                if (safeHash) {
                    compressW(m1)
                    compressW(m1)
                }
                break
            }
        }
    }

    private fun compressionStates() {
        for (t in 0 until 16) m1[t] = block[t]
        for (t in 16 until 80) m1[t] = Integer.rotateLeft(m1[t - 3] xor m1[t - 8] xor m1[t - 14] xor m1[t - 16], 1)
        val v = ihv.copyOf()
        for (t in 0 until 80) {
            if (t == 58) System.arraycopy(v, 0, state58, 0, 5)
            if (t == 65) System.arraycopy(v, 0, state65, 0, 5)
            forward(v, m1, t)
        }
        for (i in 0 until 5) ihv[i] += v[i]
    }

    private fun compressW(w: IntArray) {
        val v = ihv.copyOf()
        for (t in 0 until 80) forward(v, w, t)
        for (i in 0 until 5) ihv[i] += v[i]
    }

    private fun recompress(step: Int, state: IntArray) {
        val v = state.copyOf()
        for (t in step - 1 downTo 0) backward(v, m2, t)
        System.arraycopy(v, 0, ihv2, 0, 5)
        System.arraycopy(state, 0, v, 0, 5)
        for (t in step until 80) forward(v, m2, t)
        for (i in 0 until 5) ihvTmp[i] = ihv2[i] + v[i]
    }

    private companion object {
        fun role(r: Int, t: Int): Int = ((r - t) % 5 + 5) % 5

        fun f(t: Int, b: Int, c: Int, d: Int): Int = when {
            t < 20 -> d xor (b and (c xor d))
            t < 40 -> b xor c xor d
            t < 60 -> (b and c) + (d and (b xor c))
            else -> b xor c xor d
        }

        fun k(t: Int): Int = when {
            t < 20 -> 0x5A827999
            t < 40 -> 0x6ED9EBA1
            t < 60 -> 0x8F1BBCDC.toInt()
            else -> 0xCA62C1D6.toInt()
        }

        fun forward(v: IntArray, w: IntArray, t: Int) {
            val a = role(0, t)
            val b = role(1, t)
            val c = role(2, t)
            val d = role(3, t)
            val e = role(4, t)
            v[e] += Integer.rotateLeft(v[a], 5) + f(t, v[b], v[c], v[d]) + k(t) + w[t]
            v[b] = Integer.rotateLeft(v[b], 30)
        }

        fun backward(v: IntArray, w: IntArray, t: Int) {
            val a = role(0, t)
            val b = role(1, t)
            val c = role(2, t)
            val d = role(3, t)
            val e = role(4, t)
            v[b] = Integer.rotateRight(v[b], 30)
            v[e] -= Integer.rotateLeft(v[a], 5) + f(t, v[b], v[c], v[d]) + k(t) + w[t]
        }
    }
}

class GitSha1DcDetector : GitSha1CollisionDetector {
    override fun start(): GitSha1CollisionDetectorSession = GitSha1DcSession()
}

class GitSha1DcSession : GitSha1CollisionDetectorSession {
    private var context: Sha1Dc? = Sha1Dc()

    override fun add(bytes: ByteArray) {
        val active = context ?: throw IllegalStateException("SHA-1 screening session is closed")
        active.update(bytes)
    }

    override fun close(): GitSha1CollisionDetectorStatus {
        val active = context ?: return GitSha1CollisionDetectorStatus.failed
        val collision = active.finish()
        context = null
        return if (collision) GitSha1CollisionDetectorStatus.detected else GitSha1CollisionDetectorStatus.clear
    }

    override fun dispose() {
        context = null
    }
}
