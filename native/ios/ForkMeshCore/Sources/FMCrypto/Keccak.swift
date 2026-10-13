// Port of native/android/.../crypto/Keccak.kt: SHA3-256, SHA3-512, SHAKE128 and SHAKE256 (FIPS 202).
// ML-KEM-768 hashes with them, and the vault uses SHA3-256 for its X-Wing combiner. CryptoKit has
// no SHA-3 on iOS 17, so this is the only path on every platform.

/// An incremental Keccak sponge: absorb with `update`, then read with `squeeze` or `digest`.
public struct Keccak {
    private var state = [UInt64](repeating: 0, count: 25)
    private let rate: Int
    private let domain: UInt8
    private var offset = 0
    private var squeezing = false

    private init(rate: Int, domain: UInt8) {
        self.rate = rate
        self.domain = domain
    }

    public static func sha3_256() -> Keccak { Keccak(rate: 136, domain: 0x06) }
    public static func sha3_512() -> Keccak { Keccak(rate: 72, domain: 0x06) }
    public static func shake128() -> Keccak { Keccak(rate: 168, domain: 0x1f) }
    public static func shake256() -> Keccak { Keccak(rate: 136, domain: 0x1f) }

    public static func sha3_256(_ parts: [UInt8]...) -> [UInt8] {
        var k = sha3_256()
        for part in parts { k.update(part) }
        return k.digest(32)
    }

    public static func sha3_512(_ parts: [UInt8]...) -> [UInt8] {
        var k = sha3_512()
        for part in parts { k.update(part) }
        return k.digest(64)
    }

    public static func shake128(_ length: Int, _ parts: [UInt8]...) -> [UInt8] {
        var k = shake128()
        for part in parts { k.update(part) }
        return k.digest(length)
    }

    public static func shake256(_ length: Int, _ parts: [UInt8]...) -> [UInt8] {
        var k = shake256()
        for part in parts { k.update(part) }
        return k.digest(length)
    }

    public mutating func update(_ data: [UInt8]) {
        update(data, from: 0, length: data.count)
    }

    public mutating func update(_ data: [UInt8], from: Int, length: Int) {
        precondition(!squeezing, "Keccak already squeezing")
        precondition(from >= 0 && length >= 0 && from + length <= data.count, "Keccak update out of range")
        let rate = self.rate
        var offset = self.offset
        data.withUnsafeBufferPointer { input in
            state.withUnsafeMutableBufferPointer { lanes in
                let a = lanes.baseAddress!
                for i in from..<(from + length) {
                    a[offset >> 3] ^= UInt64(input[i]) << UInt64((offset & 7) * 8)
                    offset += 1
                    if offset == rate {
                        Keccak.permute(a)
                        offset = 0
                    }
                }
            }
        }
        self.offset = offset
    }

    /// The next `length` bytes of output; may be called repeatedly.
    public mutating func squeeze(_ length: Int) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: length)
        out.withUnsafeMutableBufferPointer { squeeze(into: $0.baseAddress!, length: length) }
        return out
    }

    mutating func squeeze(into out: UnsafeMutablePointer<UInt8>, length: Int) {
        if !squeezing { finish() }
        let rate = self.rate
        var offset = self.offset
        state.withUnsafeMutableBufferPointer { lanes in
            let a = lanes.baseAddress!
            for i in 0..<length {
                if offset == rate {
                    Keccak.permute(a)
                    offset = 0
                }
                out[i] = UInt8(truncatingIfNeeded: a[offset >> 3] >> UInt64((offset & 7) * 8))
                offset += 1
            }
        }
        self.offset = offset
    }

    public mutating func digest(_ length: Int) -> [UInt8] {
        squeeze(length)
    }

    private mutating func finish() {
        state[offset >> 3] ^= UInt64(domain) << UInt64((offset & 7) * 8)
        state[(rate - 1) >> 3] ^= UInt64(0x80) << UInt64(((rate - 1) & 7) * 8)
        state.withUnsafeMutableBufferPointer { Keccak.permute($0.baseAddress!) }
        offset = 0
        squeezing = true
    }

    /// Round constants of FIPS 202 §3.2.5.
    private static let roundConstants: [UInt64] = [
        0x0000000000000001, 0x0000000000008082, 0x800000000000808a, 0x8000000080008000,
        0x000000000000808b, 0x0000000080000001, 0x8000000080008081, 0x8000000000008009,
        0x000000000000008a, 0x0000000000000088, 0x0000000080008009, 0x000000008000000a,
        0x000000008000808b, 0x800000000000008b, 0x8000000000008089, 0x8000000000008003,
        0x8000000000008002, 0x8000000000000080, 0x000000000000800a, 0x800000008000000a,
        0x8000000080008081, 0x8000000000008080, 0x0000000080000001, 0x8000000080008008,
    ]

    @inline(__always)
    private static func rol(_ v: UInt64, _ n: UInt64) -> UInt64 {
        v << n | v >> (64 &- n)
    }

    /// Keccak-f[1600], with theta, rho, pi and chi written out per lane: the generic double loop
    /// of the Kotlin port is several times slower in an unoptimised Swift build.
    private static func permute(_ a: UnsafeMutablePointer<UInt64>) {
        roundConstants.withUnsafeBufferPointer { constants in
            var a00 = a[0], a01 = a[1], a02 = a[2], a03 = a[3], a04 = a[4]
            var a05 = a[5], a06 = a[6], a07 = a[7], a08 = a[8], a09 = a[9]
            var a10 = a[10], a11 = a[11], a12 = a[12], a13 = a[13], a14 = a[14]
            var a15 = a[15], a16 = a[16], a17 = a[17], a18 = a[18], a19 = a[19]
            var a20 = a[20], a21 = a[21], a22 = a[22], a23 = a[23], a24 = a[24]
            for round in 0..<24 {
                let c0 = a00 ^ a05 ^ a10 ^ a15 ^ a20
                let c1 = a01 ^ a06 ^ a11 ^ a16 ^ a21
                let c2 = a02 ^ a07 ^ a12 ^ a17 ^ a22
                let c3 = a03 ^ a08 ^ a13 ^ a18 ^ a23
                let c4 = a04 ^ a09 ^ a14 ^ a19 ^ a24
                let d0 = c4 ^ rol(c1, 1)
                let d1 = c0 ^ rol(c2, 1)
                let d2 = c1 ^ rol(c3, 1)
                let d3 = c2 ^ rol(c4, 1)
                let d4 = c3 ^ rol(c0, 1)

                let b00 = a00 ^ d0
                let b10 = rol(a01 ^ d1, 1)
                let b20 = rol(a02 ^ d2, 62)
                let b05 = rol(a03 ^ d3, 28)
                let b15 = rol(a04 ^ d4, 27)
                let b16 = rol(a05 ^ d0, 36)
                let b01 = rol(a06 ^ d1, 44)
                let b11 = rol(a07 ^ d2, 6)
                let b21 = rol(a08 ^ d3, 55)
                let b06 = rol(a09 ^ d4, 20)
                let b07 = rol(a10 ^ d0, 3)
                let b17 = rol(a11 ^ d1, 10)
                let b02 = rol(a12 ^ d2, 43)
                let b12 = rol(a13 ^ d3, 25)
                let b22 = rol(a14 ^ d4, 39)
                let b23 = rol(a15 ^ d0, 41)
                let b08 = rol(a16 ^ d1, 45)
                let b18 = rol(a17 ^ d2, 15)
                let b03 = rol(a18 ^ d3, 21)
                let b13 = rol(a19 ^ d4, 8)
                let b14 = rol(a20 ^ d0, 18)
                let b24 = rol(a21 ^ d1, 2)
                let b09 = rol(a22 ^ d2, 61)
                let b19 = rol(a23 ^ d3, 56)
                let b04 = rol(a24 ^ d4, 14)

                a00 = b00 ^ (~b01 & b02) ^ constants[round]
                a01 = b01 ^ (~b02 & b03)
                a02 = b02 ^ (~b03 & b04)
                a03 = b03 ^ (~b04 & b00)
                a04 = b04 ^ (~b00 & b01)
                a05 = b05 ^ (~b06 & b07)
                a06 = b06 ^ (~b07 & b08)
                a07 = b07 ^ (~b08 & b09)
                a08 = b08 ^ (~b09 & b05)
                a09 = b09 ^ (~b05 & b06)
                a10 = b10 ^ (~b11 & b12)
                a11 = b11 ^ (~b12 & b13)
                a12 = b12 ^ (~b13 & b14)
                a13 = b13 ^ (~b14 & b10)
                a14 = b14 ^ (~b10 & b11)
                a15 = b15 ^ (~b16 & b17)
                a16 = b16 ^ (~b17 & b18)
                a17 = b17 ^ (~b18 & b19)
                a18 = b18 ^ (~b19 & b15)
                a19 = b19 ^ (~b15 & b16)
                a20 = b20 ^ (~b21 & b22)
                a21 = b21 ^ (~b22 & b23)
                a22 = b22 ^ (~b23 & b24)
                a23 = b23 ^ (~b24 & b20)
                a24 = b24 ^ (~b20 & b21)
            }
            a[0] = a00; a[1] = a01; a[2] = a02; a[3] = a03; a[4] = a04
            a[5] = a05; a[6] = a06; a[7] = a07; a[8] = a08; a[9] = a09
            a[10] = a10; a[11] = a11; a[12] = a12; a[13] = a13; a[14] = a14
            a[15] = a15; a[16] = a16; a[17] = a17; a[18] = a18; a[19] = a19
            a[20] = a20; a[21] = a21; a[22] = a22; a[23] = a23; a[24] = a24
        }
    }
}
