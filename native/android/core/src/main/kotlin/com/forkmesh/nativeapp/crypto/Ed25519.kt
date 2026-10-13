package com.forkmesh.nativeapp.crypto

object Ed25519 {
    const val SEED_BYTES = 32
    const val PUBLIC_KEY_BYTES = 32
    const val SIGNATURE_BYTES = 64

    fun generateSeed(): ByteArray = Bytes.random(SEED_BYTES)

    fun publicKey(seed: ByteArray): ByteArray {
        require(seed.size == SEED_BYTES) { "Seed must have 32 bytes" }
        val h = Sha512.hash(seed)
        val a = h.copyOfRange(0, 32)
        clamp(a)
        val w = Scratch()
        val point = P3()
        scalarMultBase(point, a, w)
        Bytes.wipe(h, a)
        return encode(point, w)
    }

    fun sign(seed: ByteArray, message: ByteArray, publicKey: ByteArray? = null): ByteArray {
        require(seed.size == SEED_BYTES) { "Seed must have 32 bytes" }
        val w = Scratch()
        val point = P3()
        val h = Sha512.hash(seed)
        val a = h.copyOfRange(0, 32)
        clamp(a)
        val pub = if (publicKey != null) {
            require(publicKey.size == PUBLIC_KEY_BYTES) { "Public key must have 32 bytes" }
            publicKey
        } else {
            scalarMultBase(point, a, w)
            encode(point, w)
        }
        val digest = Sha512.digest()
        digest.update(h, 32, 32)
        digest.update(message)
        val r = reduce(digest.digest())
        scalarMultBase(point, r, w)
        val encodedR = encode(point, w)
        digest.update(encodedR)
        digest.update(pub)
        digest.update(message)
        val k = reduce(digest.digest())
        val s = mulAdd(k, a, r)
        Bytes.wipe(h, a, r)
        return Bytes.concat(encodedR, s)
    }

    fun verify(publicKey: ByteArray, message: ByteArray, signature: ByteArray): Boolean =
        verifyInternal(publicKey, message, signature, strict = false)

    fun verifyStrict(publicKey: ByteArray, message: ByteArray, signature: ByteArray): Boolean =
        verifyInternal(publicKey, message, signature, strict = true)

    fun isCanonicalScalar(s: ByteArray, offset: Int = 0): Boolean {
        for (i in 31 downTo 0) {
            val a = s[offset + i].toInt() and 0xff
            val b = L[i].toInt() and 0xff
            if (a < b) return true
            if (a > b) return false
        }
        return false
    }

    private fun verifyInternal(publicKey: ByteArray, message: ByteArray, signature: ByteArray, strict: Boolean): Boolean {
        if (publicKey.size != PUBLIC_KEY_BYTES || signature.size != SIGNATURE_BYTES) return false
        if (!isCanonicalScalar(signature, 32)) return false
        val w = Scratch()
        val a = P3()
        if (!decode(a, publicKey, 0, w)) return false
        if (strict) {
            if (isIdentity(a, w)) return false
            val torsion = P3()
            scalarMultVartime(torsion, a, L, w)
            if (!isIdentity(torsion, w)) return false
        }
        val digest = Sha512.digest()
        digest.update(signature, 0, 32)
        digest.update(publicKey)
        digest.update(message)
        val k = reduce(digest.digest())
        val ka = P3()
        scalarMultVartime(ka, a, k, w)
        val check = P3()
        scalarMultBase(check, signature.copyOfRange(32, 64), w)
        val cached = Cached()
        toCached(cached, ka)
        add(check, check, cached.ypx, cached.ymx, cached.z, cached.t2d, negate = true, w = w)
        return Bytes.constantTimeEquals(encode(check, w), signature.copyOfRange(0, 32))
    }

    private val D = Fe.fromHex("a3785913ca4deb75abd841414d0a700098e879777940c78c73fe6f2bee6c0352")
    private val D2 = Fe.fromHex("59f1b226949bd6eb56b183829a14e00030d1f3eef2808e19e7fcdf56dcd90624")
    private val SQRT_M1 = Fe.fromHex("b0a00e4a271beec478e42fad0618432fa7d7fb3d99004d2b0bdfc14f8024832b")
    private val BASE_X = Fe.fromHex("1ad5258f602d56c9b2a7259560c72c695cdcd6fd31e2a4c0fe536ecdd3366921")
    private val BASE_Y = Fe.fromHex("5866666666666666666666666666666666666666666666666666666666666666")
    private val L = Bytes.fromHex("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010")

    private class P3 {
        val x = Fe.create()
        val y = Fe.create()
        val z = Fe.create()
        val t = Fe.create()
    }

    private class Cached {
        val ypx = Fe.create()
        val ymx = Fe.create()
        val z = Fe.create()
        val t2d = Fe.create()
    }

    private class Precomp {
        val ypx = Fe.create()
        val ymx = Fe.create()
        val xy2d = Fe.create()
    }

    private class Scratch {
        val a = Fe.create()
        val b = Fe.create()
        val c = Fe.create()
        val d = Fe.create()
        val e = Fe.create()
        val f = Fe.create()
        val g = Fe.create()
        val h = Fe.create()
        val u = Fe.create()
        val v = Fe.create()
        val select = Precomp()
    }

    private fun setIdentity(p: P3) {
        Fe.zero(p.x)
        Fe.one(p.y)
        Fe.one(p.z)
        Fe.zero(p.t)
    }

    private fun copyPoint(r: P3, p: P3) {
        Fe.copy(r.x, p.x)
        Fe.copy(r.y, p.y)
        Fe.copy(r.z, p.z)
        Fe.copy(r.t, p.t)
    }

    private fun add(
        r: P3,
        p: P3,
        qYpx: LongArray,
        qYmx: LongArray,
        qz: LongArray?,
        qT2d: LongArray,
        negate: Boolean,
        w: Scratch,
    ) {
        Fe.sub(w.a, p.y, p.x)
        Fe.mul(w.a, w.a, if (negate) qYpx else qYmx)
        Fe.add(w.b, p.y, p.x)
        Fe.mul(w.b, w.b, if (negate) qYmx else qYpx)
        Fe.mul(w.c, p.t, qT2d)
        if (qz != null) {
            Fe.mul(w.d, p.z, qz)
            Fe.add(w.d, w.d, w.d)
        } else {
            Fe.add(w.d, p.z, p.z)
        }
        Fe.sub(w.e, w.b, w.a)
        if (negate) {
            Fe.add(w.f, w.d, w.c)
            Fe.sub(w.g, w.d, w.c)
        } else {
            Fe.sub(w.f, w.d, w.c)
            Fe.add(w.g, w.d, w.c)
        }
        Fe.add(w.h, w.b, w.a)
        Fe.mul(r.x, w.e, w.f)
        Fe.mul(r.y, w.g, w.h)
        Fe.mul(r.t, w.e, w.h)
        Fe.mul(r.z, w.f, w.g)
    }

    private fun double(r: P3, p: P3, w: Scratch) {
        Fe.sq(w.a, p.x)
        Fe.sq(w.b, p.y)
        Fe.sq(w.c, p.z)
        Fe.add(w.c, w.c, w.c)
        Fe.add(w.h, w.a, w.b)
        Fe.add(w.e, p.x, p.y)
        Fe.sq(w.e, w.e)
        Fe.sub(w.e, w.e, w.h)
        Fe.sub(w.g, w.b, w.a)
        Fe.sub(w.f, w.g, w.c)
        Fe.neg(w.h, w.h)
        Fe.mul(r.x, w.e, w.f)
        Fe.mul(r.y, w.g, w.h)
        Fe.mul(r.t, w.e, w.h)
        Fe.mul(r.z, w.f, w.g)
    }

    private fun toCached(q: Cached, p: P3) {
        Fe.add(q.ypx, p.y, p.x)
        Fe.sub(q.ymx, p.y, p.x)
        Fe.copy(q.z, p.z)
        Fe.mul(q.t2d, p.t, D2)
    }

    private fun toPrecomp(p: P3): Precomp {
        val out = Precomp()
        val zInverse = Fe.create()
        val x = Fe.create()
        val y = Fe.create()
        Fe.invert(zInverse, p.z)
        Fe.mul(x, p.x, zInverse)
        Fe.mul(y, p.y, zInverse)
        Fe.add(out.ypx, y, x)
        Fe.sub(out.ymx, y, x)
        Fe.mul(out.xy2d, x, y)
        Fe.mul(out.xy2d, out.xy2d, D2)
        return out
    }

    private fun isIdentity(p: P3, w: Scratch): Boolean {
        if (!Fe.isZero(p.x)) return false
        Fe.sub(w.u, p.y, p.z)
        return Fe.isZero(w.u)
    }

    private fun encode(p: P3, w: Scratch): ByteArray {
        Fe.invert(w.u, p.z)
        Fe.mul(w.v, p.x, w.u)
        Fe.mul(w.u, p.y, w.u)
        val s = ByteArray(32)
        Fe.toBytes(s, w.u)
        s[31] = (s[31].toInt() xor (Fe.isNegative(w.v) shl 7)).toByte()
        return s
    }

    private fun decode(p: P3, s: ByteArray, offset: Int, w: Scratch): Boolean {
        val y = p.y
        Fe.fromBytes(y, s, offset)
        val canonical = ByteArray(32)
        Fe.toBytes(canonical, y)
        for (i in 0 until 31) if (canonical[i] != s[offset + i]) return false
        if (canonical[31].toInt() != (s[offset + 31].toInt() and 0x7f)) return false
        val sign = (s[offset + 31].toInt() ushr 7) and 1

        val u = w.a
        val v = w.b
        Fe.sq(u, y)
        Fe.mul(v, u, D)
        Fe.sub(u, u, ONE)
        Fe.add(v, v, ONE)
        val v3 = w.c
        Fe.sq(v3, v)
        Fe.mul(v3, v3, v)
        val x = p.x
        Fe.sq(x, v3)
        Fe.mul(x, x, v)
        Fe.mul(x, x, u)
        Fe.pow22523(x, x)
        Fe.mul(x, x, v3)
        Fe.mul(x, x, u)

        val vxx = w.d
        Fe.sq(vxx, x)
        Fe.mul(vxx, vxx, v)
        Fe.sub(w.e, vxx, u)
        if (!Fe.isZero(w.e)) {
            Fe.add(w.e, vxx, u)
            if (!Fe.isZero(w.e)) return false
            Fe.mul(x, x, SQRT_M1)
        }
        if (Fe.isZero(x) && sign == 1) return false
        if (Fe.isNegative(x) != sign) Fe.neg(x, x)
        Fe.one(p.z)
        Fe.mul(p.t, x, y)
        return true
    }

    private val ONE = Fe.create().also { Fe.one(it) }

    private val baseTable: Array<Array<Precomp>> by lazy {
        val w = Scratch()
        val base = P3()
        Fe.copy(base.x, BASE_X)
        Fe.copy(base.y, BASE_Y)
        Fe.one(base.z)
        Fe.mul(base.t, BASE_X, BASE_Y)
        val cached = Cached()
        val current = P3()
        Array(32) {
            toCached(cached, base)
            copyPoint(current, base)
            val row = Array(8) { j ->
                if (j > 0) add(current, current, cached.ypx, cached.ymx, cached.z, cached.t2d, negate = false, w = w)
                toPrecomp(current)
            }
            repeat(8) { double(base, base, w) }
            row
        }
    }

    private fun equalMask(b: Int, c: Int): Int = ((b xor c) - 1) ushr 31

    private fun select(t: Precomp, pos: Int, digit: Int, w: Scratch) {
        val negative = digit ushr 31
        val abs = digit - ((-negative and digit) shl 1)
        Fe.one(t.ypx)
        Fe.one(t.ymx)
        Fe.zero(t.xy2d)
        val row = baseTable[pos]
        for (j in 0 until 8) {
            val take = equalMask(abs, j + 1)
            Fe.cmov(t.ypx, row[j].ypx, take)
            Fe.cmov(t.ymx, row[j].ymx, take)
            Fe.cmov(t.xy2d, row[j].xy2d, take)
        }
        Fe.cswap(t.ypx, t.ymx, negative)
        Fe.neg(w.u, t.xy2d)
        Fe.cmov(t.xy2d, w.u, negative)
    }

    private fun recode(a: ByteArray): IntArray {
        val e = IntArray(64)
        for (i in 0 until 32) {
            val v = a[i].toInt() and 0xff
            e[2 * i] = v and 15
            e[2 * i + 1] = v ushr 4
        }
        var carry = 0
        for (i in 0 until 63) {
            e[i] += carry
            carry = (e[i] + 8) shr 4
            e[i] -= carry shl 4
        }
        e[63] += carry
        return e
    }

    private fun scalarMultBase(h: P3, a: ByteArray, w: Scratch) {
        val e = recode(a)
        setIdentity(h)
        val t = w.select
        for (i in 1 until 64 step 2) {
            select(t, i / 2, e[i], w)
            add(h, h, t.ypx, t.ymx, null, t.xy2d, negate = false, w = w)
        }
        repeat(4) { double(h, h, w) }
        for (i in 0 until 64 step 2) {
            select(t, i / 2, e[i], w)
            add(h, h, t.ypx, t.ymx, null, t.xy2d, negate = false, w = w)
        }
        e.fill(0)
    }

    private fun scalarMultVartime(r: P3, p: P3, scalar: ByteArray, w: Scratch) {
        val e = recode(scalar)
        val table = Array(8) { Cached() }
        val current = P3()
        copyPoint(current, p)
        toCached(table[0], p)
        for (j in 1 until 8) {
            add(current, current, table[0].ypx, table[0].ymx, table[0].z, table[0].t2d, negate = false, w = w)
            toCached(table[j], current)
        }
        setIdentity(r)
        for (i in 63 downTo 0) {
            repeat(4) { double(r, r, w) }
            val digit = e[i]
            if (digit > 0) {
                val q = table[digit - 1]
                add(r, r, q.ypx, q.ymx, q.z, q.t2d, negate = false, w = w)
            } else if (digit < 0) {
                val q = table[-digit - 1]
                add(r, r, q.ypx, q.ymx, q.z, q.t2d, negate = true, w = w)
            }
        }
    }

    private fun clamp(a: ByteArray) {
        a[0] = (a[0].toInt() and 248).toByte()
        a[31] = ((a[31].toInt() and 127) or 64).toByte()
    }

    internal fun reduce(wide: ByteArray): ByteArray {
        val x = LongArray(64) { wide[it].toLong() and 0xff }
        return ByteArray(32).also { modL(it, x) }
    }

    internal fun mulAdd(k: ByteArray, a: ByteArray, r: ByteArray): ByteArray {
        val x = LongArray(64)
        for (i in 0 until 32) x[i] = r[i].toLong() and 0xff
        for (i in 0 until 32) {
            val ki = k[i].toLong() and 0xff
            for (j in 0 until 32) x[i + j] += ki * (a[j].toLong() and 0xff)
        }
        return ByteArray(32).also { modL(it, x) }
    }

    private val L_LIMBS = LongArray(32) { L[it].toLong() and 0xff }

    private fun modL(r: ByteArray, x: LongArray) {
        for (i in 63 downTo 32) {
            var carry = 0L
            var j = i - 32
            while (j < i - 12) {
                x[j] += carry - 16 * x[i] * L_LIMBS[j - (i - 32)]
                carry = (x[j] + 128) shr 8
                x[j] -= carry shl 8
                j++
            }
            x[j] += carry
            x[i] = 0
        }
        var carry = 0L
        for (j in 0 until 32) {
            x[j] += carry - (x[31] shr 4) * L_LIMBS[j]
            carry = x[j] shr 8
            x[j] = x[j] and 255
        }
        for (j in 0 until 32) x[j] -= carry * L_LIMBS[j]
        for (i in 0 until 32) {
            x[i + 1] += x[i] shr 8
            r[i] = (x[i] and 255).toByte()
        }
        val t = ByteArray(32)
        var borrow = 0
        for (i in 0 until 32) {
            val diff = (r[i].toInt() and 0xff) - (L[i].toInt() and 0xff) - borrow
            t[i] = diff.toByte()
            borrow = (diff ushr 31)
        }
        val keep = -borrow
        for (i in 0 until 32) r[i] = ((r[i].toInt() and keep) or (t[i].toInt() and keep.inv())).toByte()
        x.fill(0)
    }
}
