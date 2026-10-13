// Port of native/android/.../crypto/Hashes.kt (`HmacSha256`, `Hkdf`, `Pbkdf2`), as used by owner
// crypto, room seal, room crypto, portable identity and the vault.

#if canImport(CryptoKit)
import CryptoKit
import Foundation // [UInt8]: DataProtocol
#endif
#if canImport(CommonCrypto)
import CommonCrypto
#endif

/// HMAC-SHA256 (RFC 2104).
public enum HmacSha256 {
    public static let byteCount = 32

    public static func mac(key: [UInt8], _ data: [UInt8]...) -> [UInt8] {
        mac(key: key, parts: data)
    }

    public static func mac(key: [UInt8], parts: [[UInt8]]) -> [UInt8] {
        #if canImport(CryptoKit)
        // HMAC zero-pads every key to the block size, so an empty key and a single zero byte are
        // the same key (RFC 2104 §2); this keeps an empty `SymmetricKey` out of CryptoKit.
        let keyBytes: [UInt8] = key.isEmpty ? [0] : key
        var hmac = CryptoKit.HMAC<CryptoKit.SHA256>(key: SymmetricKey(data: keyBytes))
        for part in parts { hmac.update(data: part) }
        return Array(hmac.finalize())
        #else
        return macPortable(key: key, parts: parts)
        #endif
    }

    /// The pure Swift path, always compiled so tests can cross-check CryptoKit against it.
    public static func macPortable(key: [UInt8], parts: [[UInt8]]) -> [UInt8] {
        var state = HmacSha256State(key: key)
        for part in parts { state.update(part) }
        return state.finalize()
    }
}

/// A keyed, incremental HMAC-SHA256. Copying it forks the MAC, so a key is absorbed once however
/// many messages it authenticates.
struct HmacSha256State {
    private(set) var inner = Sha256State()
    private(set) var outer = Sha256State()

    init(key: [UInt8]) {
        var block = key.count > 64 ? Sha256.hashPortable(parts: [key]) : key
        block.append(contentsOf: [UInt8](repeating: 0, count: 64 - block.count))
        var ipad = block
        var opad = block
        for i in 0..<64 {
            ipad[i] ^= 0x36
            opad[i] ^= 0x5c
        }
        inner.update(ipad)
        outer.update(opad)
        CryptoBytes.wipe(&block)
        CryptoBytes.wipe(&ipad)
        CryptoBytes.wipe(&opad)
    }

    mutating func update(_ data: [UInt8]) {
        inner.update(data)
    }

    mutating func finalize() -> [UInt8] {
        var digest = inner.finalize()
        outer.update(digest)
        CryptoBytes.wipe(&digest)
        return outer.finalize()
    }
}

/// HKDF-SHA256 (RFC 5869), matching `Hkdf(hmac: Hmac.sha256(), outputLength: n)`.
public enum Hkdf {
    /// `deriveKey(secretKey: ikm, nonce: salt, info: info)`. On Apple platforms the HMAC calls
    /// underneath are CryptoKit's.
    public static func sha256(ikm: [UInt8], salt: [UInt8], info: [UInt8], length: Int) -> [UInt8] {
        precondition(length >= 1 && length <= 255 * HmacSha256.byteCount, "HKDF output length out of range")
        var prk = HmacSha256.mac(key: salt, ikm)
        var out = [UInt8]()
        out.reserveCapacity(length)
        var previous = [UInt8]()
        var counter: UInt8 = 1
        while out.count < length {
            previous = HmacSha256.mac(key: prk, previous, info, [counter])
            out.append(contentsOf: previous.prefix(length - out.count))
            counter &+= 1
        }
        CryptoBytes.wipe(&prk)
        CryptoBytes.wipe(&previous)
        return out
    }
}

/// PBKDF2-HMAC-SHA256 (RFC 8018 §5.2).
public enum Pbkdf2 {
    /// Derives `length` bytes from the exact `password` bytes (callers pass the UTF-8 of the
    /// secret).
    ///
    /// Apple platforms use CommonCrypto for a non-empty password and salt, where it is byte-exact;
    /// anything else, and Linux, takes the portable loop.
    public static func hmacSha256(password: [UInt8], salt: [UInt8], iterations: Int, length: Int) -> [UInt8] {
        precondition(iterations > 0 && length > 0, "PBKDF2 needs positive iterations and length")
        #if canImport(CommonCrypto)
        if !password.isEmpty && !salt.isEmpty && iterations <= Int(UInt32.max) {
            var out = [UInt8](repeating: 0, count: length)
            let status: Int32 = password.withUnsafeBufferPointer { pw in
                salt.withUnsafeBufferPointer { sl in
                    out.withUnsafeMutableBufferPointer { derived in
                        pw.baseAddress!.withMemoryRebound(to: CChar.self, capacity: pw.count) { chars in
                            CCKeyDerivationPBKDF(
                                CCPBKDFAlgorithm(kCCPBKDF2), chars, pw.count, sl.baseAddress!, sl.count,
                                CCPseudoRandomAlgorithm(kCCPRFHmacAlgSHA256), UInt32(iterations),
                                derived.baseAddress!, derived.count)
                        }
                    }
                }
            }
            if status == 0 { return out }
        }
        #endif
        return hmacSha256Portable(password: password, salt: salt, iterations: iterations, length: length)
    }

    /// The pure Swift path. After the first round every HMAC input is one 32-byte block, so each
    /// round is exactly two SHA-256 compressions from the keyed chaining values; the loop runs on
    /// raw words rather than through the incremental hasher, which matters at 100k+ rounds.
    public static func hmacSha256Portable(password: [UInt8], salt: [UInt8], iterations: Int, length: Int) -> [UInt8] {
        precondition(iterations > 0 && length > 0, "PBKDF2 needs positive iterations and length")
        let keyed = HmacSha256State(key: password)
        var innerIV = keyed.inner.chaining
        var outerIV = keyed.outer.chaining
        var out = [UInt8]()
        out.reserveCapacity(length)
        // h0..h7 plus the message schedule; a 64-byte block: 32 bytes of U, 0x80, zeros, and the
        // bit length of key pad + U (768) big endian.
        var state = [UInt32](repeating: 0, count: 8 + 64)
        var block = [UInt8](repeating: 0, count: 64)
        var acc = [UInt32](repeating: 0, count: 8)
        var index: UInt32 = 1
        while out.count < length {
            var first = keyed
            first.update(salt)
            first.update([UInt8(truncatingIfNeeded: index >> 24), UInt8(truncatingIfNeeded: index >> 16),
                          UInt8(truncatingIfNeeded: index >> 8), UInt8(truncatingIfNeeded: index)])
            var u = first.finalize()
            for i in 0..<64 { block[i] = 0 }
            for i in 0..<32 { block[i] = u[i] }
            block[32] = 0x80
            block[62] = 0x03
            for i in 0..<8 {
                acc[i] = UInt32(u[4 * i]) << 24 | UInt32(u[4 * i + 1]) << 16 | UInt32(u[4 * i + 2]) << 8 | UInt32(u[4 * i + 3])
            }
            CryptoBytes.wipe(&u)
            state.withUnsafeMutableBufferPointer { sBuffer in
                block.withUnsafeMutableBufferPointer { bBuffer in
                    acc.withUnsafeMutableBufferPointer { accBuffer in
                        let s = sBuffer.baseAddress!, b = bBuffer.baseAddress!, a = accBuffer.baseAddress!
                        if iterations > 1 {
                            for _ in 1..<iterations {
                                for i in 0..<8 { s[i] = innerIV[i] }
                                Sha256State.compress(s, b)
                                Pbkdf2.store(s, b)
                                for i in 0..<8 { s[i] = outerIV[i] }
                                Sha256State.compress(s, b)
                                Pbkdf2.store(s, b)
                                for i in 0..<8 { a[i] ^= s[i] }
                            }
                        }
                    }
                }
            }
            for i in 0..<8 where out.count < length {
                for shift in [24, 16, 8, 0] where out.count < length {
                    out.append(UInt8(truncatingIfNeeded: acc[i] >> UInt32(shift)))
                }
            }
            index &+= 1
        }
        for i in 0..<state.count { state[i] = 0 }
        for i in 0..<8 {
            acc[i] = 0
            innerIV[i] = 0
            outerIV[i] = 0
        }
        CryptoBytes.wipe(&block)
        return out
    }

    /// Writes h0..h7 big endian over the first 32 bytes of the block: the next HMAC input.
    @inline(__always)
    private static func store(_ s: UnsafeMutablePointer<UInt32>, _ b: UnsafeMutablePointer<UInt8>) {
        for i in 0..<8 {
            let v = s[i]
            b[4 * i] = UInt8(truncatingIfNeeded: v >> 24)
            b[4 * i + 1] = UInt8(truncatingIfNeeded: v >> 16)
            b[4 * i + 2] = UInt8(truncatingIfNeeded: v >> 8)
            b[4 * i + 3] = UInt8(truncatingIfNeeded: v)
        }
    }
}
