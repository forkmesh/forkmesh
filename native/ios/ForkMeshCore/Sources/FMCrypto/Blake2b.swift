// BLAKE2b (RFC 7693), unkeyed, with a caller-chosen digest length. It is the hash inside
// Argon2id, which the vault stretches the vault password with (see Argon2id.swift). There is no
// Kotlin counterpart; native/android does not port the vault.

struct Blake2b {
    private static let iv: [UInt64] = [
        0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
        0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179,
    ]

    private static let sigma: [[Int]] = [
        [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
        [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3],
        [11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4],
        [7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8],
        [9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13],
        [2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9],
        [12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11],
        [13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10],
        [6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5],
        [10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0],
    ]

    private var h: [UInt64]
    private var buffer = [UInt8](repeating: 0, count: 128)
    private var buffered = 0
    private var total: UInt64 = 0
    private let outputLength: Int

    /// `outputLength` in 1...64.
    init(outputLength: Int) {
        precondition(outputLength >= 1 && outputLength <= 64, "BLAKE2b digest length out of range")
        self.outputLength = outputLength
        h = Blake2b.iv
        h[0] ^= 0x0101_0000 ^ UInt64(outputLength)
    }

    static func hash(outputLength: Int, _ parts: [UInt8]...) -> [UInt8] {
        var state = Blake2b(outputLength: outputLength)
        for part in parts { state.update(part) }
        return state.finalize()
    }

    mutating func update(_ data: [UInt8]) {
        for byte in data {
            // A full buffer is only compressed once more input arrives: the last block is
            // compressed with the finalisation flag instead.
            if buffered == 128 {
                total &+= 128
                compress(last: false)
                buffered = 0
            }
            buffer[buffered] = byte
            buffered += 1
        }
    }

    mutating func finalize() -> [UInt8] {
        total &+= UInt64(buffered)
        for i in buffered..<128 { buffer[i] = 0 }
        compress(last: true)
        var out = [UInt8](repeating: 0, count: outputLength)
        for i in 0..<outputLength { out[i] = UInt8(truncatingIfNeeded: h[i >> 3] >> UInt64(8 * (i & 7))) }
        for i in 0..<8 { h[i] = 0 }
        CryptoBytes.wipe(&buffer)
        return out
    }

    private mutating func compress(last: Bool) {
        var m = [UInt64](repeating: 0, count: 16)
        for i in 0..<16 {
            var word: UInt64 = 0
            for j in 0..<8 { word |= UInt64(buffer[8 * i + j]) << UInt64(8 * j) }
            m[i] = word
        }
        var v = h + Blake2b.iv
        v[12] ^= total
        if last { v[14] = ~v[14] }
        func g(_ a: Int, _ b: Int, _ c: Int, _ d: Int, _ x: UInt64, _ y: UInt64) {
            v[a] = v[a] &+ v[b] &+ x
            v[d] = (v[d] ^ v[a]) >> 32 | (v[d] ^ v[a]) << 32
            v[c] = v[c] &+ v[d]
            v[b] = (v[b] ^ v[c]) >> 24 | (v[b] ^ v[c]) << 40
            v[a] = v[a] &+ v[b] &+ y
            v[d] = (v[d] ^ v[a]) >> 16 | (v[d] ^ v[a]) << 48
            v[c] = v[c] &+ v[d]
            v[b] = (v[b] ^ v[c]) >> 63 | (v[b] ^ v[c]) << 1
        }
        for round in 0..<12 {
            let s = Blake2b.sigma[round % 10]
            g(0, 4, 8, 12, m[s[0]], m[s[1]])
            g(1, 5, 9, 13, m[s[2]], m[s[3]])
            g(2, 6, 10, 14, m[s[4]], m[s[5]])
            g(3, 7, 11, 15, m[s[6]], m[s[7]])
            g(0, 5, 10, 15, m[s[8]], m[s[9]])
            g(1, 6, 11, 12, m[s[10]], m[s[11]])
            g(2, 7, 8, 13, m[s[12]], m[s[13]])
            g(3, 4, 9, 14, m[s[14]], m[s[15]])
        }
        for i in 0..<8 { h[i] ^= v[i] ^ v[i + 8] }
        for i in 0..<16 {
            m[i] = 0
            v[i] = 0
        }
    }
}
