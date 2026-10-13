package com.forkmesh.nativeapp.crypto

object MlKem768 {
    const val PUBLIC_KEY_BYTES = 1184
    const val SECRET_KEY_BYTES = 2400
    const val CIPHERTEXT_BYTES = 1088
    const val SHARED_SECRET_BYTES = 32
    const val SEED_BYTES = 64

    class KeyPair(val publicKey: ByteArray, val secretKey: ByteArray)

    class Encapsulation(val ciphertext: ByteArray, val sharedSecret: ByteArray)

    private const val N = 256
    private const val Q = 3329
    private const val K = 3
    private const val ETA1 = 2
    private const val ETA2 = 2
    private const val DU = 10
    private const val DV = 4
    private const val POLY_BYTES = 384
    private const val U_BYTES = N * DU / 8
    private const val V_BYTES = N * DV / 8

    private val ZETAS = IntArray(128) { modPow(17, bitRev7(it)) }
    private val GAMMAS = IntArray(128) { modPow(17, 2 * bitRev7(it) + 1) }

    fun generateKeyPair(): KeyPair = generateKeyPair(Bytes.random(SEED_BYTES))

    fun generateKeyPair(seed: ByteArray): KeyPair {
        require(seed.size == SEED_BYTES) { "ML-KEM seed must be 64 bytes (d || z)" }
        val d = seed.copyOfRange(0, 32)
        val z = seed.copyOfRange(32, 64)
        val rhoSigma = Keccak.sha3_512(d, byteArrayOf(K.toByte()))
        val rho = rhoSigma.copyOfRange(0, 32)
        val sigma = rhoSigma.copyOfRange(32, 64)
        val a = sampleMatrix(rho)
        var nonce = 0
        val s = Array(K) { cbd(prf(sigma, nonce++, ETA1), ETA1) }
        val e = Array(K) { cbd(prf(sigma, nonce++, ETA1), ETA1) }
        s.forEach(::ntt)
        e.forEach(::ntt)
        val publicKey = ByteArray(PUBLIC_KEY_BYTES)
        val secretKey = ByteArray(SECRET_KEY_BYTES)
        for (i in 0 until K) {
            val t = IntArray(N)
            for (j in 0 until K) addInto(t, multiplyNtts(a[i][j], s[j]))
            addInto(t, e[i])
            byteEncode(t, 12, publicKey, i * POLY_BYTES)
            byteEncode(s[i], 12, secretKey, i * POLY_BYTES)
        }
        System.arraycopy(rho, 0, publicKey, K * POLY_BYTES, 32)
        var offset = K * POLY_BYTES
        System.arraycopy(publicKey, 0, secretKey, offset, PUBLIC_KEY_BYTES)
        offset += PUBLIC_KEY_BYTES
        System.arraycopy(Keccak.sha3_256(publicKey), 0, secretKey, offset, 32)
        offset += 32
        System.arraycopy(z, 0, secretKey, offset, 32)
        s.forEach { it.fill(0) }
        Bytes.wipe(d, z, rhoSigma, sigma)
        return KeyPair(publicKey, secretKey)
    }

    fun encapsulate(publicKey: ByteArray): Encapsulation = encapsulate(publicKey, Bytes.random(32))

    fun encapsulate(publicKey: ByteArray, message: ByteArray): Encapsulation {
        validatePublicKey(publicKey)
        require(message.size == 32) { "ML-KEM message must be 32 bytes" }
        val kr = Keccak.sha3_512(message, Keccak.sha3_256(publicKey))
        val r = kr.copyOfRange(32, 64)
        val ciphertext = pkeEncrypt(publicKey, 0, message, r)
        val key = kr.copyOfRange(0, 32)
        Bytes.wipe(kr, r)
        return Encapsulation(ciphertext, key)
    }

    fun decapsulate(secretKey: ByteArray, ciphertext: ByteArray): ByteArray {
        require(secretKey.size == SECRET_KEY_BYTES) { "Invalid ML-KEM secret key length" }
        require(ciphertext.size == CIPHERTEXT_BYTES) { "Invalid ML-KEM ciphertext length" }
        val ekOffset = K * POLY_BYTES
        val hOffset = ekOffset + PUBLIC_KEY_BYTES
        val zOffset = hOffset + 32
        val publicKey = secretKey.copyOfRange(ekOffset, hOffset)
        validatePublicKey(publicKey)
        val h = secretKey.copyOfRange(hOffset, zOffset)
        require(Bytes.constantTimeEquals(Keccak.sha3_256(publicKey), h)) { "Invalid ML-KEM secret key hash" }
        val z = secretKey.copyOfRange(zOffset, SECRET_KEY_BYTES)

        val message = pkeDecrypt(secretKey, ciphertext)
        val kr = Keccak.sha3_512(message, h)
        val r = kr.copyOfRange(32, 64)
        val reencrypted = pkeEncrypt(publicKey, 0, message, r)
        val rejection = Keccak.shake256(32, z, ciphertext)

        var diff = 0
        for (i in 0 until CIPHERTEXT_BYTES) diff = diff or ((ciphertext[i].toInt() xor reencrypted[i].toInt()) and 0xff)
        val equal = -((diff - 1) ushr 31)
        val out = ByteArray(SHARED_SECRET_BYTES)
        for (i in 0 until SHARED_SECRET_BYTES) {
            out[i] = ((kr[i].toInt() and equal) or (rejection[i].toInt() and equal.inv())).toByte()
        }
        Bytes.wipe(message, kr, r, rejection, z)
        return out
    }

    private fun pkeEncrypt(publicKey: ByteArray, offset: Int, message: ByteArray, r: ByteArray): ByteArray {
        val t = Array(K) { byteDecode(publicKey, offset + it * POLY_BYTES, 12) }
        val rho = publicKey.copyOfRange(offset + K * POLY_BYTES, offset + K * POLY_BYTES + 32)
        val a = sampleMatrix(rho)
        var nonce = 0
        val y = Array(K) { cbd(prf(r, nonce++, ETA1), ETA1) }
        val e1 = Array(K) { cbd(prf(r, nonce++, ETA2), ETA2) }
        val e2 = cbd(prf(r, nonce, ETA2), ETA2)
        y.forEach(::ntt)
        val out = ByteArray(CIPHERTEXT_BYTES)
        for (i in 0 until K) {
            val u = IntArray(N)
            for (j in 0 until K) addInto(u, multiplyNtts(a[j][i], y[j]))
            nttInverse(u)
            addInto(u, e1[i])
            compress(u, DU)
            byteEncode(u, DU, out, i * U_BYTES)
        }
        val v = IntArray(N)
        for (i in 0 until K) addInto(v, multiplyNtts(t[i], y[i]))
        nttInverse(v)
        addInto(v, e2)
        val mu = byteDecode(message, 0, 1)
        decompress(mu, 1)
        addInto(v, mu)
        compress(v, DV)
        byteEncode(v, DV, out, K * U_BYTES)
        y.forEach { it.fill(0) }
        return out
    }

    private fun pkeDecrypt(secretKey: ByteArray, ciphertext: ByteArray): ByteArray {
        val w = IntArray(N)
        for (i in 0 until K) {
            val u = byteDecode(ciphertext, i * U_BYTES, DU)
            decompress(u, DU)
            ntt(u)
            val s = byteDecode(secretKey, i * POLY_BYTES, 12)
            require(s.all { it < Q }) { "Invalid ML-KEM secret key encoding" }
            addInto(w, multiplyNtts(s, u))
            s.fill(0)
        }
        nttInverse(w)
        val v = byteDecode(ciphertext, K * U_BYTES, DV)
        decompress(v, DV)
        for (i in 0 until N) {
            val diff = v[i] - w[i]
            w[i] = if (diff < 0) diff + Q else diff
        }
        compress(w, 1)
        val message = ByteArray(32)
        byteEncode(w, 1, message, 0)
        return message
    }

    private fun validatePublicKey(publicKey: ByteArray) {
        require(publicKey.size == PUBLIC_KEY_BYTES) { "Invalid ML-KEM public key length" }
        for (i in 0 until K) {
            val t = byteDecode(publicKey, i * POLY_BYTES, 12)
            require(t.all { it < Q }) { "Invalid ML-KEM public key encoding" }
        }
    }

    private fun sampleMatrix(rho: ByteArray): Array<Array<IntArray>> =
        Array(K) { i -> Array(K) { j -> sampleNtt(rho, j, i) } }

    private fun sampleNtt(rho: ByteArray, j: Int, i: Int): IntArray {
        val xof = Keccak.shake128().update(rho).update(byteArrayOf(j.toByte(), i.toByte()))
        val a = IntArray(N)
        val block = ByteArray(168 * 3)
        var count = 0
        var pos = block.size
        while (count < N) {
            if (pos + 3 > block.size) {
                xof.squeeze(block)
                pos = 0
            }
            val c0 = block[pos].toInt() and 0xff
            val c1 = block[pos + 1].toInt() and 0xff
            val c2 = block[pos + 2].toInt() and 0xff
            pos += 3
            val d1 = c0 + 256 * (c1 and 15)
            val d2 = (c1 ushr 4) + 16 * c2
            if (d1 < Q) a[count++] = d1
            if (d2 < Q && count < N) a[count++] = d2
        }
        return a
    }

    private fun prf(seed: ByteArray, nonce: Int, eta: Int): ByteArray =
        Keccak.shake256(64 * eta, seed, byteArrayOf(nonce.toByte()))

    private fun cbd(bytes: ByteArray, eta: Int): IntArray {
        val f = IntArray(N)
        for (i in 0 until N) {
            var x = 0
            var y = 0
            val base = 2 * i * eta
            for (j in 0 until eta) {
                x += bit(bytes, base + j)
                y += bit(bytes, base + eta + j)
            }
            val v = x - y
            f[i] = if (v < 0) v + Q else v
        }
        bytes.fill(0)
        return f
    }

    private fun bit(bytes: ByteArray, index: Int): Int = ((bytes[index ushr 3].toInt() and 0xff) ushr (index and 7)) and 1

    private fun ntt(f: IntArray) {
        var k = 1
        var len = 128
        while (len >= 2) {
            var start = 0
            while (start < N) {
                val zeta = ZETAS[k++]
                for (j in start until start + len) {
                    val t = (zeta * f[j + len]) % Q
                    val lo = f[j] - t
                    f[j + len] = if (lo < 0) lo + Q else lo
                    val hi = f[j] + t
                    f[j] = if (hi >= Q) hi - Q else hi
                }
                start += 2 * len
            }
            len = len shr 1
        }
    }

    private fun nttInverse(f: IntArray) {
        var k = 127
        var len = 2
        while (len <= 128) {
            var start = 0
            while (start < N) {
                val zeta = ZETAS[k--]
                for (j in start until start + len) {
                    val t = f[j]
                    val sum = t + f[j + len]
                    f[j] = if (sum >= Q) sum - Q else sum
                    val diff = f[j + len] - t
                    f[j + len] = (zeta * (if (diff < 0) diff + Q else diff)) % Q
                }
                start += 2 * len
            }
            len = len shl 1
        }
        for (i in 0 until N) f[i] = (f[i] * 3303) % Q
    }

    private fun multiplyNtts(f: IntArray, g: IntArray): IntArray {
        val h = IntArray(N)
        for (i in 0 until 128) {
            val a0 = f[2 * i]
            val a1 = f[2 * i + 1]
            val b0 = g[2 * i]
            val b1 = g[2 * i + 1]
            h[2 * i] = (a0 * b0 + ((a1 * b1) % Q) * GAMMAS[i]) % Q
            h[2 * i + 1] = (a0 * b1 + a1 * b0) % Q
        }
        return h
    }

    private fun addInto(acc: IntArray, f: IntArray) {
        for (i in 0 until N) {
            val v = acc[i] + f[i]
            acc[i] = if (v >= Q) v - Q else v
        }
    }

    private fun compress(f: IntArray, d: Int) {
        val mask = (1 shl d) - 1
        for (i in 0 until N) f[i] = (((f[i] shl d) + Q / 2) / Q) and mask
    }

    private fun decompress(f: IntArray, d: Int) {
        for (i in 0 until N) f[i] = (f[i] * Q + (1 shl (d - 1))) ushr d
    }

    private fun byteEncode(f: IntArray, d: Int, out: ByteArray, offset: Int) {
        var acc = 0L
        var bits = 0
        var pos = offset
        for (i in 0 until N) {
            acc = acc or (f[i].toLong() shl bits)
            bits += d
            while (bits >= 8) {
                out[pos++] = acc.toByte()
                acc = acc ushr 8
                bits -= 8
            }
        }
    }

    private fun byteDecode(bytes: ByteArray, offset: Int, d: Int): IntArray {
        val f = IntArray(N)
        val mask = (1L shl d) - 1
        var acc = 0L
        var bits = 0
        var pos = offset
        for (i in 0 until N) {
            while (bits < d) {
                acc = acc or ((bytes[pos++].toLong() and 0xff) shl bits)
                bits += 8
            }
            f[i] = (acc and mask).toInt()
            acc = acc ushr d
            bits -= d
        }
        return f
    }

    private fun bitRev7(value: Int): Int {
        var out = 0
        for (i in 0 until 7) if ((value ushr i) and 1 == 1) out = out or (1 shl (6 - i))
        return out
    }

    private fun modPow(base: Int, exponent: Int): Int {
        var result = 1L
        var b = base.toLong() % Q
        var e = exponent
        while (e > 0) {
            if (e and 1 == 1) result = result * b % Q
            b = b * b % Q
            e = e shr 1
        }
        return result.toInt()
    }
}
