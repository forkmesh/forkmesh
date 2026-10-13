package com.forkmesh.nativeapp.crypto

object X25519 {
    const val KEY_BYTES = 32

    private val BASE_POINT = ByteArray(32).also { it[0] = 9 }

    fun clampPrivateKey(seed: ByteArray): ByteArray {
        require(seed.size == KEY_BYTES) { "Seed must be 32 bytes" }
        val k = seed.copyOf()
        k[0] = (k[0].toInt() and 0xf8).toByte()
        k[31] = ((k[31].toInt() and 0x7f) or 0x40).toByte()
        return k
    }

    fun generatePrivateKey(): ByteArray = clampPrivateKey(Bytes.random(KEY_BYTES))

    fun publicKey(privateKey: ByteArray): ByteArray = scalarMult(privateKey, BASE_POINT)

    fun sharedSecret(privateKey: ByteArray, remotePublicKey: ByteArray): ByteArray = scalarMult(privateKey, remotePublicKey)

    fun scalarMult(scalar: ByteArray, u: ByteArray): ByteArray {
        require(u.size == KEY_BYTES) { "Public key must be 32 bytes" }
        val k = clampPrivateKey(scalar)
        val x1 = Fe.create()
        Fe.fromBytes(x1, u)
        val x2 = Fe.create().also { Fe.one(it) }
        val z2 = Fe.create()
        val x3 = x1.copyOf()
        val z3 = Fe.create().also { Fe.one(it) }
        val a = Fe.create()
        val aa = Fe.create()
        val b = Fe.create()
        val bb = Fe.create()
        val e = Fe.create()
        val c = Fe.create()
        val d = Fe.create()
        val da = Fe.create()
        val cb = Fe.create()
        var swap = 0
        for (t in 254 downTo 0) {
            val bit = ((k[t ushr 3].toInt() and 0xff) ushr (t and 7)) and 1
            swap = swap xor bit
            Fe.cswap(x2, x3, swap)
            Fe.cswap(z2, z3, swap)
            swap = bit
            Fe.add(a, x2, z2)
            Fe.sq(aa, a)
            Fe.sub(b, x2, z2)
            Fe.sq(bb, b)
            Fe.sub(e, aa, bb)
            Fe.add(c, x3, z3)
            Fe.sub(d, x3, z3)
            Fe.mul(da, d, a)
            Fe.mul(cb, c, b)
            Fe.add(x3, da, cb)
            Fe.sq(x3, x3)
            Fe.sub(z3, da, cb)
            Fe.sq(z3, z3)
            Fe.mul(z3, z3, x1)
            Fe.mul(x2, aa, bb)
            Fe.mulSmall(z2, e, 121665)
            Fe.add(z2, z2, aa)
            Fe.mul(z2, z2, e)
        }
        Fe.cswap(x2, x3, swap)
        Fe.cswap(z2, z3, swap)
        Fe.invert(z2, z2)
        Fe.mul(x2, x2, z2)
        k.fill(0)
        return ByteArray(KEY_BYTES).also { Fe.toBytes(it, x2) }
    }
}
