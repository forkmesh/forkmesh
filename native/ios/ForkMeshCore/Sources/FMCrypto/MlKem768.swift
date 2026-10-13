// Port of native/android/.../crypto/MlKem768.kt, used by owner crypto and the vault: FIPS 203
// ML-KEM-768 in pure Swift, interoperable with OpenSSL's and Go's ML-KEM-768 and the Qt node's
// MirrorCrypto.
//
// Key generation expands the 64-byte seed d || z exactly as FIPS 203 does: (rho, sigma) =
// G(d || k) with k = 3 appended (the final standard, not the round-3 Kyber G(d)), which is what
// OpenSSL's `hexseed` and the desktop both produce. CryptoKit's MLKEM768 needs iOS 26, so
// this is the only path on every platform.
//
// Unlike the Kotlin port, reductions and the compression division are branch-free and
// division-free: an unoptimised Swift build would otherwise emit a hardware divide on secret
// coefficients.

public enum MlKem768 {
    public static let publicKeyBytes = 1184
    public static let secretKeyBytes = 2400
    public static let ciphertextBytes = 1088
    public static let sharedSecretBytes = 32
    /// d || z, as `generateKeyPair(seed)` and OpenSSL's `hexseed` take it.
    public static let seedBytes = 64

    public struct KeyPair {
        public let publicKey: [UInt8]
        public let secretKey: [UInt8]

        public init(publicKey: [UInt8], secretKey: [UInt8]) {
            self.publicKey = publicKey
            self.secretKey = secretKey
        }
    }

    public struct Encapsulation {
        public let ciphertext: [UInt8]
        public let sharedSecret: [UInt8]

        public init(ciphertext: [UInt8], sharedSecret: [UInt8]) {
            self.ciphertext = ciphertext
            self.sharedSecret = sharedSecret
        }
    }

    private typealias Poly = [Int32]

    private static let n = 256
    private static let q: Int32 = 3329
    private static let k = 3
    private static let eta1 = 2
    private static let eta2 = 2
    private static let du = 10
    private static let dv = 4
    private static let polyBytes = 384
    private static let uBytes = 256 * 10 / 8
    private static let vBytes = 256 * 4 / 8

    private static let zetas: [Int32] = (0..<128).map { modPow(17, bitRev7($0)) }
    private static let gammas: [Int32] = (0..<128).map { modPow(17, 2 * bitRev7($0) + 1) }

    /// A fresh key pair (ML-KEM.KeyGen).
    public static func generateKeyPair() -> KeyPair {
        var seed = CryptoBytes.random(seedBytes)
        defer { CryptoBytes.wipe(&seed) }
        return keyGen(seed)
    }

    /// ML-KEM.KeyGen_internal(d, z) from a 64-byte seed d || z.
    public static func generateKeyPair(seed: [UInt8]) throws -> KeyPair {
        guard seed.count == seedBytes else { throw CryptoArgumentError("ML-KEM seed must be 64 bytes (d || z)") }
        return keyGen(seed)
    }

    private static func keyGen(_ seed: [UInt8]) -> KeyPair {
        var d = Array(seed[0..<32])
        var z = Array(seed[32..<64])
        var rhoSigma = Keccak.sha3_512(d, [UInt8(k)])
        let rho = Array(rhoSigma[0..<32])
        var sigma = Array(rhoSigma[32..<64])
        let a = sampleMatrix(rho)
        var nonce = 0
        var s = [Poly]()
        var e = [Poly]()
        for _ in 0..<k {
            s.append(cbd(prf(sigma, nonce, eta1), eta1))
            nonce += 1
        }
        for _ in 0..<k {
            e.append(cbd(prf(sigma, nonce, eta1), eta1))
            nonce += 1
        }
        for i in 0..<k {
            ntt(&s[i])
            ntt(&e[i])
        }
        var publicKey = [UInt8](repeating: 0, count: publicKeyBytes)
        var secretKey = [UInt8](repeating: 0, count: secretKeyBytes)
        for i in 0..<k {
            var t = Poly(repeating: 0, count: n)
            for j in 0..<k { addInto(&t, multiplyNtts(a[i][j], s[j])) }
            addInto(&t, e[i])
            byteEncode(t, 12, &publicKey, i * polyBytes)
            byteEncode(s[i], 12, &secretKey, i * polyBytes)
        }
        publicKey.replaceSubrange(k * polyBytes..<k * polyBytes + 32, with: rho)
        var offset = k * polyBytes
        secretKey.replaceSubrange(offset..<offset + publicKeyBytes, with: publicKey)
        offset += publicKeyBytes
        secretKey.replaceSubrange(offset..<offset + 32, with: Keccak.sha3_256(publicKey))
        offset += 32
        secretKey.replaceSubrange(offset..<offset + 32, with: z)
        for i in 0..<k {
            wipe(&s[i])
            wipe(&e[i])
        }
        CryptoBytes.wipe(&d)
        CryptoBytes.wipe(&z)
        CryptoBytes.wipe(&rhoSigma)
        CryptoBytes.wipe(&sigma)
        return KeyPair(publicKey: publicKey, secretKey: secretKey)
    }

    /// ML-KEM.Encaps; throws `CryptoArgumentError` for a malformed public key.
    public static func encapsulate(publicKey: [UInt8]) throws -> Encapsulation {
        var message = CryptoBytes.random(32)
        defer { CryptoBytes.wipe(&message) }
        return try encapsulate(publicKey: publicKey, message: message)
    }

    /// ML-KEM.Encaps_internal with a caller-chosen 32-byte message (tests and known answers only).
    public static func encapsulate(publicKey: [UInt8], message: [UInt8]) throws -> Encapsulation {
        try validatePublicKey(publicKey)
        guard message.count == 32 else { throw CryptoArgumentError("ML-KEM message must be 32 bytes") }
        var kr = Keccak.sha3_512(message, Keccak.sha3_256(publicKey))
        var r = Array(kr[32..<64])
        let ciphertext = pkeEncrypt(publicKey, message, r)
        let key = Array(kr[0..<32])
        CryptoBytes.wipe(&kr)
        CryptoBytes.wipe(&r)
        return Encapsulation(ciphertext: ciphertext, sharedSecret: key)
    }

    /// ML-KEM.Decaps with implicit rejection: a ciphertext that does not re-encrypt yields
    /// J(z || c) instead of an error. Wrong lengths, a public-key part with coefficients >= q, or a
    /// hash mismatch inside `secretKey` throw `CryptoArgumentError`.
    public static func decapsulate(secretKey: [UInt8], ciphertext: [UInt8]) throws -> [UInt8] {
        guard secretKey.count == secretKeyBytes else { throw CryptoArgumentError("Invalid ML-KEM secret key length") }
        guard ciphertext.count == ciphertextBytes else { throw CryptoArgumentError("Invalid ML-KEM ciphertext length") }
        let ekOffset = k * polyBytes
        let hOffset = ekOffset + publicKeyBytes
        let zOffset = hOffset + 32
        let publicKey = Array(secretKey[ekOffset..<hOffset])
        try validatePublicKey(publicKey)
        let h = Array(secretKey[hOffset..<zOffset])
        guard CryptoBytes.constantTimeEquals(Keccak.sha3_256(publicKey), h) else {
            throw CryptoArgumentError("Invalid ML-KEM secret key hash")
        }
        var z = Array(secretKey[zOffset..<secretKeyBytes])

        var message = try pkeDecrypt(secretKey, ciphertext)
        var kr = Keccak.sha3_512(message, h)
        var r = Array(kr[32..<64])
        let reencrypted = pkeEncrypt(publicKey, message, r)
        var rejection = Keccak.shake256(32, z, ciphertext)

        var diff: UInt8 = 0
        for i in 0..<ciphertextBytes { diff |= ciphertext[i] ^ reencrypted[i] }
        // All ones when the ciphertexts match.
        let equal = UInt8(truncatingIfNeeded: (UInt32(diff) &- 1) >> 8)
        var out = [UInt8](repeating: 0, count: sharedSecretBytes)
        for i in 0..<sharedSecretBytes { out[i] = (kr[i] & equal) | (rejection[i] & ~equal) }
        CryptoBytes.wipe(&message)
        CryptoBytes.wipe(&kr)
        CryptoBytes.wipe(&r)
        CryptoBytes.wipe(&rejection)
        CryptoBytes.wipe(&z)
        return out
    }

    // ---------------------------------------------------------------------------------------------
    // K-PKE (FIPS 203 §5).

    private static func pkeEncrypt(_ publicKey: [UInt8], _ message: [UInt8], _ r: [UInt8]) -> [UInt8] {
        let t = (0..<k).map { byteDecode(publicKey, $0 * polyBytes, 12) }
        let rho = Array(publicKey[k * polyBytes..<k * polyBytes + 32])
        let a = sampleMatrix(rho)
        var nonce = 0
        var y = [Poly]()
        var e1 = [Poly]()
        for _ in 0..<k {
            y.append(cbd(prf(r, nonce, eta1), eta1))
            nonce += 1
        }
        for _ in 0..<k {
            e1.append(cbd(prf(r, nonce, eta2), eta2))
            nonce += 1
        }
        var e2 = cbd(prf(r, nonce, eta2), eta2)
        for i in 0..<k { ntt(&y[i]) }
        var out = [UInt8](repeating: 0, count: ciphertextBytes)
        for i in 0..<k {
            var u = Poly(repeating: 0, count: n)
            for j in 0..<k { addInto(&u, multiplyNtts(a[j][i], y[j])) }
            nttInverse(&u)
            addInto(&u, e1[i])
            compress(&u, du)
            byteEncode(u, du, &out, i * uBytes)
        }
        var v = Poly(repeating: 0, count: n)
        for i in 0..<k { addInto(&v, multiplyNtts(t[i], y[i])) }
        nttInverse(&v)
        addInto(&v, e2)
        var mu = byteDecode(message, 0, 1)
        decompress(&mu, 1)
        addInto(&v, mu)
        compress(&v, dv)
        byteEncode(v, dv, &out, k * uBytes)
        for i in 0..<k {
            wipe(&y[i])
            wipe(&e1[i])
        }
        wipe(&e2)
        wipe(&mu)
        return out
    }

    private static func pkeDecrypt(_ secretKey: [UInt8], _ ciphertext: [UInt8]) throws -> [UInt8] {
        var w = Poly(repeating: 0, count: n)
        for i in 0..<k {
            var u = byteDecode(ciphertext, i * uBytes, du)
            decompress(&u, du)
            ntt(&u)
            var s = byteDecode(secretKey, i * polyBytes, 12)
            guard isReduced(s) else {
                wipe(&s)
                throw CryptoArgumentError("Invalid ML-KEM secret key encoding")
            }
            addInto(&w, multiplyNtts(s, u))
            wipe(&s)
        }
        nttInverse(&w)
        var v = byteDecode(ciphertext, k * uBytes, dv)
        decompress(&v, dv)
        for i in 0..<n {
            let diff = v[i] &- w[i]
            w[i] = diff &+ (q & (diff &>> 31))
        }
        compress(&w, 1)
        var message = [UInt8](repeating: 0, count: 32)
        byteEncode(w, 1, &message, 0)
        wipe(&w)
        return message
    }

    /// FIPS 203 §7.2 modulus check.
    private static func validatePublicKey(_ publicKey: [UInt8]) throws {
        guard publicKey.count == publicKeyBytes else { throw CryptoArgumentError("Invalid ML-KEM public key length") }
        for i in 0..<k {
            guard isReduced(byteDecode(publicKey, i * polyBytes, 12)) else {
                throw CryptoArgumentError("Invalid ML-KEM public key encoding")
            }
        }
    }

    /// Whether every coefficient is below q, without an early exit.
    private static func isReduced(_ f: Poly) -> Bool {
        var over: Int32 = 0
        for c in f { over |= (q &- 1 &- c) &>> 31 }
        return over == 0
    }

    // ---------------------------------------------------------------------------------------------
    // Sampling, NTT and encodings (FIPS 203 §4).

    /// Â[i][j] = SampleNTT(rho || j || i).
    private static func sampleMatrix(_ rho: [UInt8]) -> [[Poly]] {
        (0..<k).map { i in (0..<k).map { j in sampleNtt(rho, j, i) } }
    }

    private static func sampleNtt(_ rho: [UInt8], _ j: Int, _ i: Int) -> Poly {
        var xof = Keccak.shake128()
        xof.update(rho)
        xof.update([UInt8(j), UInt8(i)])
        var a = Poly(repeating: 0, count: n)
        var block = [UInt8]()
        var count = 0
        var pos = 0
        while count < n {
            if pos + 3 > block.count {
                block = xof.squeeze(168 * 3)
                pos = 0
            }
            let c0 = Int32(block[pos]), c1 = Int32(block[pos + 1]), c2 = Int32(block[pos + 2])
            pos += 3
            let d1 = c0 + 256 * (c1 & 15)
            let d2 = (c1 >> 4) + 16 * c2
            if d1 < q {
                a[count] = d1
                count += 1
            }
            if d2 < q && count < n {
                a[count] = d2
                count += 1
            }
        }
        return a
    }

    /// PRF_eta(s, b) = SHAKE256(s || b, 64 * eta bytes).
    private static func prf(_ seed: [UInt8], _ nonce: Int, _ eta: Int) -> [UInt8] {
        Keccak.shake256(64 * eta, seed, [UInt8(nonce)])
    }

    private static func cbd(_ input: [UInt8], _ eta: Int) -> Poly {
        var bytes = input
        var f = Poly(repeating: 0, count: n)
        bytes.withUnsafeBufferPointer { b in
            f.withUnsafeMutableBufferPointer { f in
                for i in 0..<n {
                    var x: Int32 = 0
                    var y: Int32 = 0
                    let base = 2 * i * eta
                    for j in 0..<eta {
                        let xi = base + j, yi = base + eta + j
                        x &+= Int32((b[xi >> 3] >> UInt8(xi & 7)) & 1)
                        y &+= Int32((b[yi >> 3] >> UInt8(yi & 7)) & 1)
                    }
                    let v = x &- y
                    f[i] = v &+ (q & (v &>> 31))
                }
            }
        }
        CryptoBytes.wipe(&bytes)
        return f
    }

    /// x mod q as a multiply and shift. With m = ceil(2^36 / q), floor(x * m / 2^36) equals
    /// floor(x / q) for every x below 2^36 / (m * q - 2^36), about 41 million; the largest value
    /// reduced here is below 2 * q^2, about 22 million.
    @inline(__always)
    static func reduce(_ x: Int32) -> Int32 {
        let quotient = Int32(truncatingIfNeeded: (UInt64(UInt32(bitPattern: x)) &* 20_642_679) &>> 36)
        return x &- quotient &* q
    }

    private static func ntt(_ poly: inout Poly) {
        zetas.withUnsafeBufferPointer { zetas in
            poly.withUnsafeMutableBufferPointer { f in
                var kk = 1
                var len = 128
                while len >= 2 {
                    var start = 0
                    while start < n {
                        let zeta = zetas[kk]
                        kk += 1
                        for j in start..<(start + len) {
                            let t = reduce(zeta &* f[j + len])
                            let lo = f[j] &- t
                            f[j + len] = lo &+ (q & (lo &>> 31))
                            let hi = f[j] &+ t &- q
                            f[j] = hi &+ (q & (hi &>> 31))
                        }
                        start += 2 * len
                    }
                    len >>= 1
                }
            }
        }
    }

    private static func nttInverse(_ poly: inout Poly) {
        zetas.withUnsafeBufferPointer { zetas in
            poly.withUnsafeMutableBufferPointer { f in
                var kk = 127
                var len = 2
                while len <= 128 {
                    var start = 0
                    while start < n {
                        let zeta = zetas[kk]
                        kk -= 1
                        for j in start..<(start + len) {
                            let t = f[j]
                            let sum = t &+ f[j + len] &- q
                            f[j] = sum &+ (q & (sum &>> 31))
                            let diff = f[j + len] &- t
                            f[j + len] = reduce(zeta &* (diff &+ (q & (diff &>> 31))))
                        }
                        start += 2 * len
                    }
                    len <<= 1
                }
                for i in 0..<n { f[i] = reduce(f[i] &* 3303) }
            }
        }
    }

    private static func multiplyNtts(_ f: Poly, _ g: Poly) -> Poly {
        var h = Poly(repeating: 0, count: n)
        gammas.withUnsafeBufferPointer { gammas in
            f.withUnsafeBufferPointer { f in
                g.withUnsafeBufferPointer { g in
                    h.withUnsafeMutableBufferPointer { h in
                        for i in 0..<128 {
                            let a0 = f[2 * i], a1 = f[2 * i + 1]
                            let b0 = g[2 * i], b1 = g[2 * i + 1]
                            h[2 * i] = reduce(a0 &* b0 &+ reduce(a1 &* b1) &* gammas[i])
                            h[2 * i + 1] = reduce(a0 &* b1 &+ a1 &* b0)
                        }
                    }
                }
            }
        }
        return h
    }

    private static func addInto(_ acc: inout Poly, _ f: Poly) {
        for i in 0..<n {
            let v = acc[i] &+ f[i] &- q
            acc[i] = v &+ (q & (v &>> 31))
        }
    }

    /// Compress_d: round(2^d / q * x) mod 2^d, with the division by q as a multiply and shift.
    private static func compress(_ f: inout Poly, _ d: Int) {
        let mask: Int32 = (1 << Int32(d)) - 1
        for i in 0..<n {
            let numerator = UInt64(UInt32(bitPattern: f[i])) &<< UInt64(d) &+ 1664
            f[i] = Int32(truncatingIfNeeded: (numerator &* 20_642_679) &>> 36) & mask
        }
    }

    private static func decompress(_ f: inout Poly, _ d: Int) {
        let half: Int32 = 1 << Int32(d - 1)
        for i in 0..<n { f[i] = (f[i] &* q &+ half) &>> Int32(d) }
    }

    private static func byteEncode(_ f: Poly, _ d: Int, _ out: inout [UInt8], _ offset: Int) {
        var acc: UInt64 = 0
        var bits = 0
        var pos = offset
        for i in 0..<n {
            acc |= UInt64(UInt32(bitPattern: f[i])) << UInt64(bits)
            bits += d
            while bits >= 8 {
                out[pos] = UInt8(truncatingIfNeeded: acc)
                pos += 1
                acc >>= 8
                bits -= 8
            }
        }
    }

    private static func byteDecode(_ bytes: [UInt8], _ offset: Int, _ d: Int) -> Poly {
        var f = Poly(repeating: 0, count: n)
        let mask: UInt64 = (1 << UInt64(d)) - 1
        var acc: UInt64 = 0
        var bits = 0
        var pos = offset
        for i in 0..<n {
            while bits < d {
                acc |= UInt64(bytes[pos]) << UInt64(bits)
                pos += 1
                bits += 8
            }
            f[i] = Int32(truncatingIfNeeded: acc & mask)
            acc >>= UInt64(d)
            bits -= d
        }
        return f
    }

    private static func wipe(_ f: inout Poly) {
        for i in 0..<f.count { f[i] = 0 }
    }

    private static func bitRev7(_ value: Int) -> Int {
        var out = 0
        for i in 0..<7 where (value >> i) & 1 == 1 { out |= 1 << (6 - i) }
        return out
    }

    private static func modPow(_ base: Int, _ exponent: Int) -> Int32 {
        var result = 1
        var b = base % 3329
        var e = exponent
        while e > 0 {
            if e & 1 == 1 { result = result * b % 3329 }
            b = b * b % 3329
            e >>= 1
        }
        return Int32(result)
    }
}
