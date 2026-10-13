// Port of native/android/.../crypto/AesGcm.kt: AES-256-GCM with 12-byte nonces and 16-byte tags,
// as used by owner crypto, room seal, room crypto, portable identity and the vault.
//
// Apple platforms run CryptoKit's AES.GCM. The pure Swift path (FIPS 197 with T-tables, NIST SP
// 800-38D with a bitwise GHASH) is what Linux builds and tests run; its table lookups are not
// hardened against cache timing, which is why it is not the path that ships.

#if canImport(CryptoKit)
import CryptoKit
import Foundation // [UInt8]: DataProtocol
#endif

/// An argument no caller should ever pass (wrong key or seed length): Kotlin's
/// `IllegalArgumentException`.
public struct CryptoArgumentError: Error, Equatable, CustomStringConvertible {
    public let message: String

    public init(_ message: String) {
        self.message = message
    }

    public var description: String { message }
}

public enum AesGcm {
    public static let keyBytes = 32
    public static let nonceBytes = 12
    public static let tagBytes = 16

    /// The ciphertext, its nonce and its authentication tag (`mac`).
    public struct SecretBox: Equatable {
        public let cipherText: [UInt8]
        public let nonce: [UInt8]
        public let mac: [UInt8]

        public init(cipherText: [UInt8], nonce: [UInt8], mac: [UInt8]) {
            self.cipherText = cipherText
            self.nonce = nonce
            self.mac = mac
        }
    }

    /// A tag, key or nonce did not authenticate (`SecretBoxAuthenticationError`).
    public struct AuthenticationError: Error, Equatable {
        public init() {}
    }

    /// Encrypts `plaintext` under a 32-byte `key`; a fresh random nonce unless one is given.
    public static func encrypt(_ plaintext: [UInt8], key: [UInt8], nonce: [UInt8]? = nil, aad: [UInt8] = []) throws -> SecretBox {
        let nonce = nonce ?? CryptoBytes.random(nonceBytes)
        try check(key: key, nonce: nonce)
        #if canImport(CryptoKit)
        let sealed = try CryptoKit.AES.GCM.seal(
            plaintext, using: SymmetricKey(data: key), nonce: CryptoKit.AES.GCM.Nonce(data: nonce), authenticating: aad)
        return SecretBox(cipherText: Array(sealed.ciphertext), nonce: nonce, mac: Array(sealed.tag))
        #else
        return try encryptPortable(plaintext, key: key, nonce: nonce, aad: aad)
        #endif
    }

    /// Decrypts and authenticates; throws `AuthenticationError` on any mismatch.
    public static func decrypt(cipherText: [UInt8], nonce: [UInt8], mac: [UInt8], key: [UInt8], aad: [UInt8] = []) throws -> [UInt8] {
        guard key.count == keyBytes else { throw CryptoArgumentError("AES-256-GCM needs a 32-byte key") }
        if nonce.count != nonceBytes || mac.count != tagBytes { throw AuthenticationError() }
        #if canImport(CryptoKit)
        do {
            let box = try CryptoKit.AES.GCM.SealedBox(
                nonce: CryptoKit.AES.GCM.Nonce(data: nonce), ciphertext: cipherText, tag: mac)
            return Array(try CryptoKit.AES.GCM.open(box, using: SymmetricKey(data: key), authenticating: aad))
        } catch {
            throw AuthenticationError()
        }
        #else
        return try decryptPortable(cipherText: cipherText, nonce: nonce, mac: mac, key: key, aad: aad)
        #endif
    }

    /// `decrypt`, or nil when authentication fails.
    public static func decryptOrNull(cipherText: [UInt8], nonce: [UInt8], mac: [UInt8], key: [UInt8], aad: [UInt8] = []) -> [UInt8]? {
        try? decrypt(cipherText: cipherText, nonce: nonce, mac: mac, key: key, aad: aad)
    }

    private static func check(key: [UInt8], nonce: [UInt8]) throws {
        guard key.count == keyBytes else { throw CryptoArgumentError("AES-256-GCM needs a 32-byte key") }
        guard nonce.count == nonceBytes else { throw CryptoArgumentError("AES-GCM nonce must be 12 bytes") }
    }

    // ---------------------------------------------------------------------------------------------
    // The pure Swift path, always compiled so tests can cross-check CryptoKit against it.

    public static func encryptPortable(_ plaintext: [UInt8], key: [UInt8], nonce: [UInt8], aad: [UInt8] = []) throws -> SecretBox {
        try check(key: key, nonce: nonce)
        var cipher = Aes256(key: key)
        let cipherText = ctr(&cipher, nonce: nonce, input: plaintext)
        let tag = self.tag(&cipher, nonce: nonce, aad: aad, cipherText: cipherText)
        cipher.wipe()
        return SecretBox(cipherText: cipherText, nonce: nonce, mac: tag)
    }

    public static func decryptPortable(cipherText: [UInt8], nonce: [UInt8], mac: [UInt8], key: [UInt8], aad: [UInt8] = []) throws -> [UInt8] {
        guard key.count == keyBytes else { throw CryptoArgumentError("AES-256-GCM needs a 32-byte key") }
        if nonce.count != nonceBytes || mac.count != tagBytes { throw AuthenticationError() }
        var cipher = Aes256(key: key)
        defer { cipher.wipe() }
        let expected = tag(&cipher, nonce: nonce, aad: aad, cipherText: cipherText)
        // The tag is checked before any plaintext exists.
        if !CryptoBytes.constantTimeEquals(expected, mac) { throw AuthenticationError() }
        return ctr(&cipher, nonce: nonce, input: cipherText)
    }

    /// CTR keystream from counter 2 (counter 1 masks the tag), SP 800-38D §7.1.
    private static func ctr(_ cipher: inout Aes256, nonce: [UInt8], input: [UInt8]) -> [UInt8] {
        var out = [UInt8](repeating: 0, count: input.count)
        var counterBlock = nonce + [0, 0, 0, 0]
        var stream = [UInt8](repeating: 0, count: 16)
        var counter: UInt32 = 2
        var offset = 0
        while offset < input.count {
            counterBlock[12] = UInt8(truncatingIfNeeded: counter >> 24)
            counterBlock[13] = UInt8(truncatingIfNeeded: counter >> 16)
            counterBlock[14] = UInt8(truncatingIfNeeded: counter >> 8)
            counterBlock[15] = UInt8(truncatingIfNeeded: counter)
            cipher.encryptBlock(counterBlock, into: &stream)
            let take = min(16, input.count - offset)
            for i in 0..<take { out[offset + i] = input[offset + i] ^ stream[i] }
            offset += take
            counter &+= 1
        }
        CryptoBytes.wipe(&stream)
        return out
    }

    private static func tag(_ cipher: inout Aes256, nonce: [UInt8], aad: [UInt8], cipherText: [UInt8]) -> [UInt8] {
        var h = [UInt8](repeating: 0, count: 16)
        cipher.encryptBlock([UInt8](repeating: 0, count: 16), into: &h)
        var ghash = Ghash(h: h)
        ghash.absorbPadded(aad)
        ghash.absorbPadded(cipherText)
        var lengths = [UInt8](repeating: 0, count: 16)
        let aadBits = UInt64(aad.count) * 8
        let textBits = UInt64(cipherText.count) * 8
        for i in 0..<8 {
            lengths[7 - i] = UInt8(truncatingIfNeeded: aadBits >> UInt64(8 * i))
            lengths[15 - i] = UInt8(truncatingIfNeeded: textBits >> UInt64(8 * i))
        }
        ghash.absorbPadded(lengths)
        var mask = [UInt8](repeating: 0, count: 16)
        cipher.encryptBlock(nonce + [0, 0, 0, 1], into: &mask)
        var out = ghash.digest()
        for i in 0..<16 { out[i] ^= mask[i] }
        CryptoBytes.wipe(&h)
        CryptoBytes.wipe(&mask)
        return out
    }
}

/// GHASH over GF(2^128) (SP 800-38D §6.4), branch-free shift-and-add on two 64-bit halves.
private struct Ghash {
    private let hHi: UInt64
    private let hLo: UInt64
    private var yHi: UInt64 = 0
    private var yLo: UInt64 = 0

    init(h: [UInt8]) {
        var hi: UInt64 = 0
        var lo: UInt64 = 0
        for i in 0..<8 {
            hi = hi << 8 | UInt64(h[i])
            lo = lo << 8 | UInt64(h[8 + i])
        }
        hHi = hi
        hLo = lo
    }

    /// Absorbs `data` in 16-byte blocks, zero-padding the last.
    mutating func absorbPadded(_ data: [UInt8]) {
        var offset = 0
        while offset < data.count {
            var hi: UInt64 = 0
            var lo: UInt64 = 0
            for i in 0..<16 {
                let byte: UInt64 = offset + i < data.count ? UInt64(data[offset + i]) : 0
                if i < 8 { hi = hi << 8 | byte } else { lo = lo << 8 | byte }
            }
            yHi ^= hi
            yLo ^= lo
            multiply()
            offset += 16
        }
    }

    private mutating func multiply() {
        var zHi: UInt64 = 0, zLo: UInt64 = 0
        var vHi = hHi, vLo = hLo
        for i in 0..<128 {
            let bit = i < 64 ? (yHi >> UInt64(63 - i)) & 1 : (yLo >> UInt64(127 - i)) & 1
            let take = 0 &- bit
            zHi ^= vHi & take
            zLo ^= vLo & take
            let reduce = 0 &- (vLo & 1)
            vLo = vLo >> 1 | vHi << 63
            vHi = vHi >> 1 ^ (0xe100000000000000 & reduce)
        }
        yHi = zHi
        yLo = zLo
    }

    func digest() -> [UInt8] {
        var out = [UInt8](repeating: 0, count: 16)
        for i in 0..<8 {
            out[i] = UInt8(truncatingIfNeeded: yHi >> UInt64(56 - 8 * i))
            out[8 + i] = UInt8(truncatingIfNeeded: yLo >> UInt64(56 - 8 * i))
        }
        return out
    }
}

/// The AES-256 forward cipher (FIPS 197); GCM never needs the inverse.
private struct Aes256 {
    private var roundKeys = [UInt32](repeating: 0, count: 60)

    /// The S-box, derived from its definition (inverse in GF(2^8), then the affine map) rather
    /// than typed out, and the four encryption T-tables built from it.
    private static let tables: (sbox: [UInt8], te: [[UInt32]]) = {
        var sbox = [UInt8](repeating: 0, count: 256)
        var p: UInt8 = 1
        var q: UInt8 = 1
        repeat {
            p = p ^ (p << 1) ^ (p & 0x80 != 0 ? 0x1b : 0)
            q ^= q << 1
            q ^= q << 2
            q ^= q << 4
            if q & 0x80 != 0 { q ^= 0x09 }
            let x = q ^ (q << 1 | q >> 7) ^ (q << 2 | q >> 6) ^ (q << 3 | q >> 5) ^ (q << 4 | q >> 4)
            sbox[Int(p)] = x ^ 0x63
        } while p != 1
        sbox[0] = 0x63
        var te = [[UInt32]](repeating: [UInt32](repeating: 0, count: 256), count: 4)
        for i in 0..<256 {
            let s = UInt32(sbox[i])
            let s2 = (s << 1 ^ (s & 0x80 != 0 ? 0x11b : 0)) & 0xff
            let s3 = s2 ^ s
            let word = s2 << 24 | s << 16 | s << 8 | s3
            te[0][i] = word
            te[1][i] = word >> 8 | word << 24
            te[2][i] = word >> 16 | word << 16
            te[3][i] = word >> 24 | word << 8
        }
        return (sbox, te)
    }()

    init(key: [UInt8]) {
        let sbox = Aes256.tables.sbox
        func subWord(_ w: UInt32) -> UInt32 {
            UInt32(sbox[Int(w >> 24)]) << 24 | UInt32(sbox[Int(w >> 16 & 0xff)]) << 16 |
                UInt32(sbox[Int(w >> 8 & 0xff)]) << 8 | UInt32(sbox[Int(w & 0xff)])
        }
        for i in 0..<8 {
            roundKeys[i] = UInt32(key[4 * i]) << 24 | UInt32(key[4 * i + 1]) << 16 |
                UInt32(key[4 * i + 2]) << 8 | UInt32(key[4 * i + 3])
        }
        var rcon: UInt32 = 1
        for i in 8..<60 {
            var temp = roundKeys[i - 1]
            if i % 8 == 0 {
                temp = subWord(temp << 8 | temp >> 24) ^ (rcon << 24)
                rcon = (rcon << 1 ^ (rcon & 0x80 != 0 ? 0x11b : 0)) & 0xff
            } else if i % 8 == 4 {
                temp = subWord(temp)
            }
            roundKeys[i] = roundKeys[i - 8] ^ temp
        }
    }

    mutating func wipe() {
        for i in 0..<roundKeys.count { roundKeys[i] = 0 }
    }

    func encryptBlock(_ input: [UInt8], into out: inout [UInt8]) {
        let tables = Aes256.tables
        let te0 = tables.te[0], te1 = tables.te[1], te2 = tables.te[2], te3 = tables.te[3]
        let sbox = tables.sbox
        let rk = roundKeys
        func load(_ i: Int) -> UInt32 {
            UInt32(input[i]) << 24 | UInt32(input[i + 1]) << 16 | UInt32(input[i + 2]) << 8 | UInt32(input[i + 3])
        }
        var s0 = load(0) ^ rk[0], s1 = load(4) ^ rk[1], s2 = load(8) ^ rk[2], s3 = load(12) ^ rk[3]
        for round in 1..<14 {
            let k = 4 * round
            let t0 = te0[Int(s0 >> 24)] ^ te1[Int(s1 >> 16 & 0xff)] ^ te2[Int(s2 >> 8 & 0xff)] ^ te3[Int(s3 & 0xff)] ^ rk[k]
            let t1 = te0[Int(s1 >> 24)] ^ te1[Int(s2 >> 16 & 0xff)] ^ te2[Int(s3 >> 8 & 0xff)] ^ te3[Int(s0 & 0xff)] ^ rk[k + 1]
            let t2 = te0[Int(s2 >> 24)] ^ te1[Int(s3 >> 16 & 0xff)] ^ te2[Int(s0 >> 8 & 0xff)] ^ te3[Int(s1 & 0xff)] ^ rk[k + 2]
            let t3 = te0[Int(s3 >> 24)] ^ te1[Int(s0 >> 16 & 0xff)] ^ te2[Int(s1 >> 8 & 0xff)] ^ te3[Int(s2 & 0xff)] ^ rk[k + 3]
            s0 = t0; s1 = t1; s2 = t2; s3 = t3
        }
        func last(_ a: UInt32, _ b: UInt32, _ c: UInt32, _ d: UInt32, _ key: UInt32) -> UInt32 {
            (UInt32(sbox[Int(a >> 24)]) << 24 | UInt32(sbox[Int(b >> 16 & 0xff)]) << 16 |
                UInt32(sbox[Int(c >> 8 & 0xff)]) << 8 | UInt32(sbox[Int(d & 0xff)])) ^ key
        }
        let o = [last(s0, s1, s2, s3, rk[56]), last(s1, s2, s3, s0, rk[57]), last(s2, s3, s0, s1, rk[58]), last(s3, s0, s1, s2, rk[59])]
        for i in 0..<4 {
            out[4 * i] = UInt8(truncatingIfNeeded: o[i] >> 24)
            out[4 * i + 1] = UInt8(truncatingIfNeeded: o[i] >> 16)
            out[4 * i + 2] = UInt8(truncatingIfNeeded: o[i] >> 8)
            out[4 * i + 3] = UInt8(truncatingIfNeeded: o[i])
        }
    }
}
