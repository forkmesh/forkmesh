// Port of native/android/.../crypto/Ed25519.kt, used by identity, the vault and the strict
// trust-list verifier: RFC 8032 Ed25519 in pure Swift over fixed-width limbs, with a constant-time
// comb for signing.
//
// Deliberately not CryptoKit on any platform. CryptoKit's Curve25519.Signing signatures are
// randomised; Kotlin's are the deterministic RFC 8032 ones, and signature vectors are
// pinned across clients (see docs/FMCrypto.md). Verification stays here too, so the acceptance
// rules (canonical S, canonical encodings, the strict subgroup check) are the same everywhere.

public enum Ed25519 {
    public static let seedBytes = 32
    public static let publicKeyBytes = 32
    public static let signatureBytes = 64

    /// A fresh random 32-byte private seed (`Ed25519().newKeyPair()`).
    public static func generateSeed() -> [UInt8] {
        CryptoBytes.random(seedBytes)
    }

    /// The 32-byte public key of `seed` (`newKeyPairFromSeed(seed).extractPublicKey()`).
    public static func publicKey(seed: [UInt8]) throws -> [UInt8] {
        guard seed.count == seedBytes else { throw CryptoArgumentError("Seed must have 32 bytes") }
        var h = Sha512.hash(seed)
        var a = Array(h[0..<32])
        clamp(&a)
        let out = encode(scalarMultBase(a))
        CryptoBytes.wipe(&h)
        CryptoBytes.wipe(&a)
        return out
    }

    /// The 64-byte detached signature of `message` (`Ed25519().sign`). Pass `publicKey` when it is
    /// already known to skip recomputing it; it must belong to `seed`.
    public static func sign(seed: [UInt8], message: [UInt8], publicKey: [UInt8]? = nil) throws -> [UInt8] {
        guard seed.count == seedBytes else { throw CryptoArgumentError("Seed must have 32 bytes") }
        if let publicKey, publicKey.count != publicKeyBytes { throw CryptoArgumentError("Public key must have 32 bytes") }
        var h = Sha512.hash(seed)
        var a = Array(h[0..<32])
        clamp(&a)
        let pub = publicKey ?? encode(scalarMultBase(a))
        var prefix = Array(h[32..<64])
        var r = reduce(Sha512.hash(prefix, message))
        let encodedR = encode(scalarMultBase(r))
        let k = reduce(Sha512.hash(encodedR, pub, message))
        let s = mulAdd(k, a, r)
        CryptoBytes.wipe(&h)
        CryptoBytes.wipe(&a)
        CryptoBytes.wipe(&prefix)
        CryptoBytes.wipe(&r)
        return encodedR + s
    }

    /// RFC 8032 §5.1.7 verification (cofactorless): canonical `S < L`, canonical point encodings
    /// for the key and `R`, and `[S]B == R + [k]A`.
    public static func verify(publicKey: [UInt8], message: [UInt8], signature: [UInt8]) -> Bool {
        verifyInternal(publicKey, message, signature, strict: false)
    }

    /// `verify` plus the key checks of `app/src/collaboration.py`'s strict verifier: the public
    /// key must not be the identity and must lie in the prime-order subgroup (`[L]A == identity`).
    public static func verifyStrict(publicKey: [UInt8], message: [UInt8], signature: [UInt8]) -> Bool {
        verifyInternal(publicKey, message, signature, strict: true)
    }

    /// Whether the 32 little-endian bytes at `offset` encode an integer below the group order L.
    public static func isCanonicalScalar(_ s: [UInt8], offset: Int = 0) -> Bool {
        guard offset >= 0 && s.count >= offset + 32 else { return false }
        var i = 31
        while i >= 0 {
            let a = s[offset + i]
            let b = order[i]
            if a < b { return true }
            if a > b { return false }
            i -= 1
        }
        return false
    }

    private static func verifyInternal(_ publicKey: [UInt8], _ message: [UInt8], _ signature: [UInt8], strict: Bool) -> Bool {
        if publicKey.count != publicKeyBytes || signature.count != signatureBytes { return false }
        if !isCanonicalScalar(signature, offset: 32) { return false }
        guard let a = decode(publicKey) else { return false }
        if strict {
            if isIdentity(a) { return false }
            if !isIdentity(scalarMultVartime(a, order)) { return false }
        }
        let encodedR = Array(signature[0..<32])
        let k = reduce(Sha512.hash(encodedR, publicKey, message))
        let ka = scalarMultVartime(a, k)
        let sb = scalarMultBase(Array(signature[32..<64]))
        let check = add(sb, Cached(ka), negate: true)
        return CryptoBytes.constantTimeEquals(encode(check), encodedR)
    }

    // ---------------------------------------------------------------------------------------------
    // Group arithmetic on extended twisted Edwards coordinates (X:Y:Z:T), x = X/Z, y = Y/Z, xy = T/Z.

    private static let d = Fe(littleEndianHex: "a3785913ca4deb75abd841414d0a700098e879777940c78c73fe6f2bee6c0352")
    private static let d2 = Fe(littleEndianHex: "59f1b226949bd6eb56b183829a14e00030d1f3eef2808e19e7fcdf56dcd90624")
    private static let sqrtM1 = Fe(littleEndianHex: "b0a00e4a271beec478e42fad0618432fa7d7fb3d99004d2b0bdfc14f8024832b")
    private static let baseX = Fe(littleEndianHex: "1ad5258f602d56c9b2a7259560c72c695cdcd6fd31e2a4c0fe536ecdd3366921")
    private static let baseY = Fe(littleEndianHex: "5866666666666666666666666666666666666666666666666666666666666666")
    private static let order: [UInt8] = CryptoBytes.fromHex("edd3f55c1a631258d69cf7a2def9de1400000000000000000000000000000010")!

    private struct P3 {
        var x = Fe.zero
        var y = Fe.one
        var z = Fe.one
        var t = Fe.zero

        static let identity = P3()
    }

    /// Projective Niels form (Y+X, Y-X, Z, 2dT) for repeated additions of one point.
    private struct Cached {
        var ypx: Fe
        var ymx: Fe
        var z: Fe
        var t2d: Fe

        init(_ p: P3) {
            ypx = Fe.add(p.y, p.x)
            ymx = Fe.sub(p.y, p.x)
            z = p.z
            t2d = Fe.mul(p.t, Ed25519.d2)
        }
    }

    /// Affine Niels form (y+x, y-x, 2dxy) of a table entry.
    private struct Precomp {
        var ypx = Fe.one
        var ymx = Fe.one
        var xy2d = Fe.zero

        init() {}

        init(_ p: P3) {
            let zInverse = Fe.invert(p.z)
            let x = Fe.mul(p.x, zInverse)
            let y = Fe.mul(p.y, zInverse)
            ypx = Fe.add(y, x)
            ymx = Fe.sub(y, x)
            xy2d = Fe.mul(Fe.mul(x, y), Ed25519.d2)
        }
    }

    /// p + q, or p - q when `negate`, for q given as (Y+X, Y-X, Z, 2dT); a nil `qz` means Z = 1.
    /// Unified add-2008-hwcd-3 for a = -1, complete on this curve. `negate` is never secret.
    @inline(__always)
    private static func add(_ p: P3, ypx: Fe, ymx: Fe, z qz: Fe?, t2d: Fe, negate: Bool) -> P3 {
        let a = Fe.mul(Fe.sub(p.y, p.x), negate ? ypx : ymx)
        let b = Fe.mul(Fe.add(p.y, p.x), negate ? ymx : ypx)
        let c = Fe.mul(p.t, t2d)
        let dd: Fe
        if let qz {
            let zz = Fe.mul(p.z, qz)
            dd = Fe.add(zz, zz)
        } else {
            dd = Fe.add(p.z, p.z)
        }
        let e = Fe.sub(b, a)
        let f = negate ? Fe.add(dd, c) : Fe.sub(dd, c)
        let g = negate ? Fe.sub(dd, c) : Fe.add(dd, c)
        let h = Fe.add(b, a)
        return P3(x: Fe.mul(e, f), y: Fe.mul(g, h), z: Fe.mul(f, g), t: Fe.mul(e, h))
    }

    private static func add(_ p: P3, _ q: Cached, negate: Bool) -> P3 {
        add(p, ypx: q.ypx, ymx: q.ymx, z: q.z, t2d: q.t2d, negate: negate)
    }

    /// 2p (dbl-2008-hwcd, a = -1).
    private static func double(_ p: P3) -> P3 {
        let a = Fe.sq(p.x)
        let b = Fe.sq(p.y)
        let zz = Fe.sq(p.z)
        let c = Fe.add(zz, zz)
        let sum = Fe.add(a, b)
        let e = Fe.sub(Fe.sq(Fe.add(p.x, p.y)), sum)
        let g = Fe.sub(b, a)
        let f = Fe.sub(g, c)
        let h = Fe.neg(sum)
        return P3(x: Fe.mul(e, f), y: Fe.mul(g, h), z: Fe.mul(f, g), t: Fe.mul(e, h))
    }

    private static func isIdentity(_ p: P3) -> Bool {
        p.x.isZero && Fe.sub(p.y, p.z).isZero
    }

    private static func encode(_ p: P3) -> [UInt8] {
        let zInverse = Fe.invert(p.z)
        let x = Fe.mul(p.x, zInverse)
        var s = Fe.mul(p.y, zInverse).toBytes()
        s[31] ^= UInt8(x.isNegative) << 7
        return s
    }

    /// Strict RFC 8032 §5.1.3 decoding: y < p, a square root must exist, and x = 0 forbids the sign bit.
    private static func decode(_ s: [UInt8]) -> P3? {
        let y = Fe(bytes: s)
        let canonical = y.toBytes()
        for i in 0..<31 where canonical[i] != s[i] { return nil }
        if canonical[31] != s[31] & 0x7f { return nil }
        let sign = Int(s[31] >> 7)

        let yy = Fe.sq(y)
        let u = Fe.sub(yy, Fe.one)                   // u = y^2 - 1
        let v = Fe.add(Fe.mul(yy, d), Fe.one)        // v = d y^2 + 1
        let v3 = Fe.mul(Fe.sq(v), v)
        let uv7 = Fe.mul(Fe.mul(Fe.sq(v3), v), u)
        var x = Fe.mul(Fe.mul(Fe.pow22523(uv7), v3), u) // u v^3 (u v^7)^((p-5)/8)

        let vxx = Fe.mul(Fe.sq(x), v)
        if !Fe.sub(vxx, u).isZero {
            if !Fe.add(vxx, u).isZero { return nil }
            x = Fe.mul(x, sqrtM1)
        }
        if x.isZero && sign == 1 { return nil }
        if x.isNegative != sign { x = Fe.neg(x) }
        return P3(x: x, y: y, z: Fe.one, t: Fe.mul(x, y))
    }

    /// baseTable[i][j] = (j + 1) * 256^i * B, for the signed radix-16 comb.
    private static let baseTable: [[Precomp]] = {
        var base = P3(x: baseX, y: baseY, z: Fe.one, t: Fe.mul(baseX, baseY))
        var table = [[Precomp]]()
        for _ in 0..<32 {
            let cached = Cached(base)
            var current = base
            var row = [Precomp]()
            for j in 0..<8 {
                if j > 0 { current = add(current, cached, negate: false) }
                row.append(Precomp(current))
            }
            for _ in 0..<8 { base = double(base) }
            table.append(row)
        }
        return table
    }()

    /// Constant-time table lookup of digit * 256^pos * B, digit in [-8, 8].
    private static func select(_ row: UnsafeBufferPointer<Precomp>, _ digit: Int32) -> Precomp {
        let negative = Int32(bitPattern: UInt32(bitPattern: digit) >> 31)
        let abs = digit &- (((0 &- negative) & digit) &<< 1)
        var t = Precomp()
        for j in 0..<8 {
            let take = Int64((UInt32(bitPattern: (abs ^ Int32(j + 1)) &- 1)) >> 31)
            t.ypx.cmov(row[j].ypx, take)
            t.ymx.cmov(row[j].ymx, take)
            t.xy2d.cmov(row[j].xy2d, take)
        }
        Fe.cswap(&t.ypx, &t.ymx, Int64(negative))
        t.xy2d.cmov(Fe.neg(t.xy2d), Int64(negative))
        return t
    }

    private static func recode(_ a: [UInt8]) -> [Int32] {
        var e = [Int32](repeating: 0, count: 64)
        for i in 0..<32 {
            e[2 * i] = Int32(a[i] & 15)
            e[2 * i + 1] = Int32(a[i] >> 4)
        }
        var carry: Int32 = 0
        for i in 0..<63 {
            e[i] += carry
            carry = (e[i] + 8) >> 4
            e[i] -= carry << 4
        }
        e[63] += carry
        return e
    }

    /// a * B in constant time; requires a[31] <= 127.
    private static func scalarMultBase(_ a: [UInt8]) -> P3 {
        var e = recode(a)
        var h = P3.identity
        let table = baseTable
        var i = 1
        while i < 64 {
            let t = table[i / 2].withUnsafeBufferPointer { select($0, e[i]) }
            h = add(h, ypx: t.ypx, ymx: t.ymx, z: nil, t2d: t.xy2d, negate: false)
            i += 2
        }
        for _ in 0..<4 { h = double(h) }
        i = 0
        while i < 64 {
            let t = table[i / 2].withUnsafeBufferPointer { select($0, e[i]) }
            h = add(h, ypx: t.ypx, ymx: t.ymx, z: nil, t2d: t.xy2d, negate: false)
            i += 2
        }
        for j in 0..<64 { e[j] = 0 }
        return h
    }

    /// scalar * p, variable time (public inputs only); requires scalar[31] <= 127.
    private static func scalarMultVartime(_ p: P3, _ scalar: [UInt8]) -> P3 {
        let e = recode(scalar)
        let first = Cached(p)
        var table = [first]
        var current = p
        for _ in 1..<8 {
            current = add(current, first, negate: false)
            table.append(Cached(current))
        }
        var r = P3.identity
        var i = 63
        while i >= 0 {
            for _ in 0..<4 { r = double(r) }
            let digit = Int(e[i])
            if digit > 0 {
                r = add(r, table[digit - 1], negate: false)
            } else if digit < 0 {
                r = add(r, table[-digit - 1], negate: true)
            }
            i -= 1
        }
        return r
    }

    // ---------------------------------------------------------------------------------------------
    // Scalars modulo L = 2^252 + 27742317777372353535851937790883648493.

    private static func clamp(_ a: inout [UInt8]) {
        a[0] &= 248
        a[31] = (a[31] & 127) | 64
    }

    /// A 64-byte little-endian integer reduced modulo L.
    static func reduce(_ wide: [UInt8]) -> [UInt8] {
        var x = wide.map { Int64($0) }
        return modL(&x)
    }

    /// (r + k * a) mod L.
    static func mulAdd(_ k: [UInt8], _ a: [UInt8], _ r: [UInt8]) -> [UInt8] {
        var x = [Int64](repeating: 0, count: 64)
        for i in 0..<32 { x[i] = Int64(r[i]) }
        x.withUnsafeMutableBufferPointer { xs in
            for i in 0..<32 {
                let ki = Int64(k[i])
                for j in 0..<32 { xs[i + j] &+= ki &* Int64(a[j]) }
            }
        }
        return modL(&x)
    }

    private static let orderLimbs: [Int64] = order.map { Int64($0) }

    /// Reduces x (64 signed byte-sized limbs) modulo L, fully (TweetNaCl modL plus a final subtraction).
    private static func modL(_ x: inout [Int64]) -> [UInt8] {
        var r = [UInt8](repeating: 0, count: 32)
        let l = orderLimbs
        x.withUnsafeMutableBufferPointer { x in
            var i = 63
            while i >= 32 {
                var carry: Int64 = 0
                var j = i - 32
                while j < i - 12 {
                    x[j] &+= carry &- 16 &* x[i] &* l[j - (i - 32)]
                    carry = (x[j] &+ 128) &>> 8
                    x[j] &-= carry &<< 8
                    j += 1
                }
                x[j] &+= carry
                x[i] = 0
                i -= 1
            }
            var carry: Int64 = 0
            for j in 0..<32 {
                x[j] &+= carry &- (x[31] &>> 4) &* l[j]
                carry = x[j] &>> 8
                x[j] &= 255
            }
            for j in 0..<32 { x[j] &-= carry &* l[j] }
            for i in 0..<32 {
                x[i + 1] &+= x[i] &>> 8
                r[i] = UInt8(truncatingIfNeeded: x[i] & 255)
            }
            for i in 0..<64 { x[i] = 0 }
        }
        // Constant-time r -= L when r >= L.
        var t = [UInt8](repeating: 0, count: 32)
        var borrow: Int32 = 0
        for i in 0..<32 {
            let diff = Int32(r[i]) - Int32(order[i]) - borrow
            t[i] = UInt8(truncatingIfNeeded: diff)
            borrow = Int32(bitPattern: UInt32(bitPattern: diff) >> 31)
        }
        let keep = UInt8(truncatingIfNeeded: 0 &- borrow) // all ones when r < L
        for i in 0..<32 { r[i] = (r[i] & keep) | (t[i] & ~keep) }
        return r
    }
}
