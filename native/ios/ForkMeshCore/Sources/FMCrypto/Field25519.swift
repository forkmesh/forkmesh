// Port of native/android/.../crypto/Field25519.kt: the GF(2^255 - 19) arithmetic under Ed25519
// and X25519, in ten fixed-width limbs in radix 2^25.5 (the
// ref10 layout: 26, 25, 26, ... bits), 64-bit intermediate products, no big integers.
//
// Every add, sub and mul leaves limbs carried to |even| <= 2^25 and |odd| <= 2^24 (plus a small
// slack), so a product of any two outputs stays below 2^58 and never overflows an Int64.
//
// Kotlin mutates LongArrays in place to avoid allocation; here a field element is a struct of ten
// Int64s, so values live on the stack and the same goal is met without the aliasing rules.

struct Fe {
    var l0: Int64 = 0, l1: Int64 = 0, l2: Int64 = 0, l3: Int64 = 0, l4: Int64 = 0
    var l5: Int64 = 0, l6: Int64 = 0, l7: Int64 = 0, l8: Int64 = 0, l9: Int64 = 0

    static let zero = Fe()
    static let one = Fe(l0: 1)

    private static let widths: [Int64] = [26, 25, 26, 25, 26, 25, 26, 25, 26, 25]

    init() {}

    init(l0: Int64) {
        self.l0 = l0
    }

    init(littleEndianHex: String) {
        self.init(bytes: CryptoBytes.fromHex(littleEndianHex)!)
    }

    /// Loads the low 255 bits of a little-endian 32-byte string (the top bit is ignored).
    init(bytes s: [UInt8], offset: Int = 0) {
        func load4(_ i: Int) -> Int64 {
            Int64(s[offset + i]) | Int64(s[offset + i + 1]) << 8 | Int64(s[offset + i + 2]) << 16 | Int64(s[offset + i + 3]) << 24
        }
        l0 = load4(0) & 0x3ffffff
        l1 = (load4(3) >> 2) & 0x1ffffff
        l2 = (load4(6) >> 3) & 0x3ffffff
        l3 = (load4(9) >> 5) & 0x1ffffff
        l4 = (load4(12) >> 6) & 0x3ffffff
        l5 = load4(16) & 0x1ffffff
        l6 = (load4(19) >> 1) & 0x3ffffff
        l7 = (load4(22) >> 3) & 0x1ffffff
        l8 = (load4(25) >> 4) & 0x3ffffff
        l9 = (load4(28) >> 6) & 0x1ffffff
    }

    /// The canonical (fully reduced) little-endian encoding.
    func toBytes() -> [UInt8] {
        var f = self
        f.carry()
        var h = [f.l0, f.l1, f.l2, f.l3, f.l4, f.l5, f.l6, f.l7, f.l8, f.l9]
        var q = (19 &* h[9] &+ (1 << 24)) &>> 25
        for i in 0..<10 { q = (h[i] &+ q) &>> Fe.widths[i] }
        h[0] &+= 19 &* q
        for i in 0..<9 {
            let c = h[i] &>> Fe.widths[i]
            h[i + 1] &+= c
            h[i] &-= c &<< Fe.widths[i]
        }
        h[9] &-= (h[9] &>> 25) &<< 25
        var s = [UInt8](repeating: 0, count: 32)
        var acc: UInt64 = 0
        var bits: UInt64 = 0
        var pos = 0
        for i in 0..<10 {
            acc |= UInt64(bitPattern: h[i]) &<< bits
            bits += UInt64(Fe.widths[i])
            while bits >= 8 {
                s[pos] = UInt8(truncatingIfNeeded: acc)
                pos += 1
                acc >>= 8
                bits -= 8
            }
        }
        s[pos] = UInt8(truncatingIfNeeded: acc)
        for i in 0..<10 { h[i] = 0 }
        return s
    }

    /// Propagates carries so every limb is back within its width (centred).
    @inline(__always)
    mutating func carry() {
        var c = (l0 &+ (1 << 25)) &>> 26; l1 &+= c; l0 &-= c &<< 26
        c = (l1 &+ (1 << 24)) &>> 25; l2 &+= c; l1 &-= c &<< 25
        c = (l2 &+ (1 << 25)) &>> 26; l3 &+= c; l2 &-= c &<< 26
        c = (l3 &+ (1 << 24)) &>> 25; l4 &+= c; l3 &-= c &<< 25
        c = (l4 &+ (1 << 25)) &>> 26; l5 &+= c; l4 &-= c &<< 26
        c = (l5 &+ (1 << 24)) &>> 25; l6 &+= c; l5 &-= c &<< 25
        c = (l6 &+ (1 << 25)) &>> 26; l7 &+= c; l6 &-= c &<< 26
        c = (l7 &+ (1 << 24)) &>> 25; l8 &+= c; l7 &-= c &<< 25
        c = (l8 &+ (1 << 25)) &>> 26; l9 &+= c; l8 &-= c &<< 26
        c = (l9 &+ (1 << 24)) &>> 25; l0 &+= c &* 19; l9 &-= c &<< 25
        c = (l0 &+ (1 << 25)) &>> 26; l1 &+= c; l0 &-= c &<< 26
    }

    static func add(_ f: Fe, _ g: Fe) -> Fe {
        var h = Fe()
        h.l0 = f.l0 &+ g.l0; h.l1 = f.l1 &+ g.l1; h.l2 = f.l2 &+ g.l2; h.l3 = f.l3 &+ g.l3; h.l4 = f.l4 &+ g.l4
        h.l5 = f.l5 &+ g.l5; h.l6 = f.l6 &+ g.l6; h.l7 = f.l7 &+ g.l7; h.l8 = f.l8 &+ g.l8; h.l9 = f.l9 &+ g.l9
        h.carry()
        return h
    }

    static func sub(_ f: Fe, _ g: Fe) -> Fe {
        var h = Fe()
        h.l0 = f.l0 &- g.l0; h.l1 = f.l1 &- g.l1; h.l2 = f.l2 &- g.l2; h.l3 = f.l3 &- g.l3; h.l4 = f.l4 &- g.l4
        h.l5 = f.l5 &- g.l5; h.l6 = f.l6 &- g.l6; h.l7 = f.l7 &- g.l7; h.l8 = f.l8 &- g.l8; h.l9 = f.l9 &- g.l9
        h.carry()
        return h
    }

    static func neg(_ f: Fe) -> Fe {
        var h = Fe()
        h.l0 = 0 &- f.l0; h.l1 = 0 &- f.l1; h.l2 = 0 &- f.l2; h.l3 = 0 &- f.l3; h.l4 = 0 &- f.l4
        h.l5 = 0 &- f.l5; h.l6 = 0 &- f.l6; h.l7 = 0 &- f.l7; h.l8 = 0 &- f.l8; h.l9 = 0 &- f.l9
        return h
    }

    /// f * n for a small constant n (< 2^20).
    static func mulSmall(_ f: Fe, _ n: Int64) -> Fe {
        var h = Fe()
        h.l0 = f.l0 &* n; h.l1 = f.l1 &* n; h.l2 = f.l2 &* n; h.l3 = f.l3 &* n; h.l4 = f.l4 &* n
        h.l5 = f.l5 &* n; h.l6 = f.l6 &* n; h.l7 = f.l7 &* n; h.l8 = f.l8 &* n; h.l9 = f.l9 &* n
        h.carry()
        return h
    }

    static func mul(_ f: Fe, _ g: Fe) -> Fe {
        let f0 = f.l0, f1 = f.l1, f2 = f.l2, f3 = f.l3, f4 = f.l4
        let f5 = f.l5, f6 = f.l6, f7 = f.l7, f8 = f.l8, f9 = f.l9
        let g0 = g.l0, g1 = g.l1, g2 = g.l2, g3 = g.l3, g4 = g.l4
        let g5 = g.l5, g6 = g.l6, g7 = g.l7, g8 = g.l8, g9 = g.l9
        let g1x19 = 19 &* g1, g2x19 = 19 &* g2, g3x19 = 19 &* g3, g4x19 = 19 &* g4
        let g5x19 = 19 &* g5, g6x19 = 19 &* g6, g7x19 = 19 &* g7, g8x19 = 19 &* g8, g9x19 = 19 &* g9
        let f1x2 = 2 &* f1, f3x2 = 2 &* f3, f5x2 = 2 &* f5, f7x2 = 2 &* f7, f9x2 = 2 &* f9

        // Split per limb: one ten-term expression is more than the type checker will take.
        var h = Fe()
        var a = f0 &* g0 &+ f1x2 &* g9x19 &+ f2 &* g8x19 &+ f3x2 &* g7x19 &+ f4 &* g6x19
        var b = f5x2 &* g5x19 &+ f6 &* g4x19 &+ f7x2 &* g3x19 &+ f8 &* g2x19 &+ f9x2 &* g1x19
        h.l0 = a &+ b
        a = f0 &* g1 &+ f1 &* g0 &+ f2 &* g9x19 &+ f3 &* g8x19 &+ f4 &* g7x19
        b = f5 &* g6x19 &+ f6 &* g5x19 &+ f7 &* g4x19 &+ f8 &* g3x19 &+ f9 &* g2x19
        h.l1 = a &+ b
        a = f0 &* g2 &+ f1x2 &* g1 &+ f2 &* g0 &+ f3x2 &* g9x19 &+ f4 &* g8x19
        b = f5x2 &* g7x19 &+ f6 &* g6x19 &+ f7x2 &* g5x19 &+ f8 &* g4x19 &+ f9x2 &* g3x19
        h.l2 = a &+ b
        a = f0 &* g3 &+ f1 &* g2 &+ f2 &* g1 &+ f3 &* g0 &+ f4 &* g9x19
        b = f5 &* g8x19 &+ f6 &* g7x19 &+ f7 &* g6x19 &+ f8 &* g5x19 &+ f9 &* g4x19
        h.l3 = a &+ b
        a = f0 &* g4 &+ f1x2 &* g3 &+ f2 &* g2 &+ f3x2 &* g1 &+ f4 &* g0
        b = f5x2 &* g9x19 &+ f6 &* g8x19 &+ f7x2 &* g7x19 &+ f8 &* g6x19 &+ f9x2 &* g5x19
        h.l4 = a &+ b
        a = f0 &* g5 &+ f1 &* g4 &+ f2 &* g3 &+ f3 &* g2 &+ f4 &* g1
        b = f5 &* g0 &+ f6 &* g9x19 &+ f7 &* g8x19 &+ f8 &* g7x19 &+ f9 &* g6x19
        h.l5 = a &+ b
        a = f0 &* g6 &+ f1x2 &* g5 &+ f2 &* g4 &+ f3x2 &* g3 &+ f4 &* g2
        b = f5x2 &* g1 &+ f6 &* g0 &+ f7x2 &* g9x19 &+ f8 &* g8x19 &+ f9x2 &* g7x19
        h.l6 = a &+ b
        a = f0 &* g7 &+ f1 &* g6 &+ f2 &* g5 &+ f3 &* g4 &+ f4 &* g3
        b = f5 &* g2 &+ f6 &* g1 &+ f7 &* g0 &+ f8 &* g9x19 &+ f9 &* g8x19
        h.l7 = a &+ b
        a = f0 &* g8 &+ f1x2 &* g7 &+ f2 &* g6 &+ f3x2 &* g5 &+ f4 &* g4
        b = f5x2 &* g3 &+ f6 &* g2 &+ f7x2 &* g1 &+ f8 &* g0 &+ f9x2 &* g9x19
        h.l8 = a &+ b
        a = f0 &* g9 &+ f1 &* g8 &+ f2 &* g7 &+ f3 &* g6 &+ f4 &* g5
        b = f5 &* g4 &+ f6 &* g3 &+ f7 &* g2 &+ f8 &* g1 &+ f9 &* g0
        h.l9 = a &+ b
        h.carry()
        return h
    }

    static func sq(_ f: Fe) -> Fe { mul(f, f) }

    private static func sq(_ f: Fe, times: Int) -> Fe {
        var h = f
        for _ in 0..<times { h = mul(h, h) }
        return h
    }

    /// z^(2^250 - 1), and z^11 alongside it.
    private static func pow2250(_ z: Fe) -> (Fe, z11: Fe) {
        let z2 = sq(z)                               // z^2
        let z9 = mul(z, sq(z2, times: 2))            // z^9
        let z11 = mul(z2, z9)                        // z^11
        let t5 = mul(z9, sq(z11))                    // z^(2^5 - 1)
        let t10 = mul(sq(t5, times: 5), t5)          // 2^10 - 1
        let t20 = mul(sq(t10, times: 10), t10)       // 2^20 - 1
        let t40 = mul(sq(t20, times: 20), t20)       // 2^40 - 1
        let t50 = mul(sq(t40, times: 10), t10)       // 2^50 - 1
        let t100 = mul(sq(t50, times: 50), t50)      // 2^100 - 1
        let t200 = mul(sq(t100, times: 100), t100)   // 2^200 - 1
        let t250 = mul(sq(t200, times: 50), t50)     // 2^250 - 1
        return (t250, z11)
    }

    /// z^(p - 2) = z^-1 (0 for z = 0).
    static func invert(_ z: Fe) -> Fe {
        let (t, z11) = pow2250(z)
        return mul(sq(t, times: 5), z11)             // 2^255 - 21
    }

    /// z^((p - 5) / 8) = z^(2^252 - 3).
    static func pow22523(_ z: Fe) -> Fe {
        let (t, _) = pow2250(z)
        return mul(sq(t, times: 2), z)               // 2^252 - 3
    }

    /// f = g when b == 1, unchanged when b == 0, without branching on b.
    @inline(__always)
    mutating func cmov(_ g: Fe, _ b: Int64) {
        let mask = 0 &- b
        l0 ^= mask & (l0 ^ g.l0); l1 ^= mask & (l1 ^ g.l1); l2 ^= mask & (l2 ^ g.l2); l3 ^= mask & (l3 ^ g.l3)
        l4 ^= mask & (l4 ^ g.l4); l5 ^= mask & (l5 ^ g.l5); l6 ^= mask & (l6 ^ g.l6); l7 ^= mask & (l7 ^ g.l7)
        l8 ^= mask & (l8 ^ g.l8); l9 ^= mask & (l9 ^ g.l9)
    }

    /// Swaps f and g when b == 1, without branching on b.
    @inline(__always)
    static func cswap(_ f: inout Fe, _ g: inout Fe, _ b: Int64) {
        let mask = 0 &- b
        var x = mask & (f.l0 ^ g.l0); f.l0 ^= x; g.l0 ^= x
        x = mask & (f.l1 ^ g.l1); f.l1 ^= x; g.l1 ^= x
        x = mask & (f.l2 ^ g.l2); f.l2 ^= x; g.l2 ^= x
        x = mask & (f.l3 ^ g.l3); f.l3 ^= x; g.l3 ^= x
        x = mask & (f.l4 ^ g.l4); f.l4 ^= x; g.l4 ^= x
        x = mask & (f.l5 ^ g.l5); f.l5 ^= x; g.l5 ^= x
        x = mask & (f.l6 ^ g.l6); f.l6 ^= x; g.l6 ^= x
        x = mask & (f.l7 ^ g.l7); f.l7 ^= x; g.l7 ^= x
        x = mask & (f.l8 ^ g.l8); f.l8 ^= x; g.l8 ^= x
        x = mask & (f.l9 ^ g.l9); f.l9 ^= x; g.l9 ^= x
    }

    /// 1 when the canonical encoding is odd ("negative"), else 0.
    var isNegative: Int { Int(toBytes()[0] & 1) }

    var isZero: Bool { CryptoBytes.isAllZero(toBytes()) }
}
