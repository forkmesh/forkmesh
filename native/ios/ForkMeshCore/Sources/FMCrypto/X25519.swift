// Port of native/android/.../crypto/X25519.kt, used by room seal, owner crypto and the vault: RFC
// 7748 X25519 as a constant-time Montgomery ladder over fixed-width limbs.
//
// Apple platforms run CryptoKit's Curve25519.KeyAgreement for canonical peer keys; see
// `scalarMult` for what stays on the pure Swift ladder and why.

#if canImport(CryptoKit)
import CryptoKit
import Foundation // [UInt8]: DataProtocol
#endif

public enum X25519 {
    public static let keyBytes = 32

    /// Clears the low three bits and bit 255, sets bit 254. Private keys are stored and exported
    /// in this clamped form.
    public static func clampPrivateKey(_ seed: [UInt8]) throws -> [UInt8] {
        guard seed.count == keyBytes else { throw CryptoArgumentError("Seed must be 32 bytes") }
        var k = seed
        k[0] &= 0xf8
        k[31] = (k[31] & 0x7f) | 0x40
        return k
    }

    /// A fresh clamped private key (`X25519().newKeyPair()` then `extractPrivateKeyBytes()`).
    public static func generatePrivateKey() -> [UInt8] {
        var k = CryptoBytes.random(keyBytes)
        k[0] &= 0xf8
        k[31] = (k[31] & 0x7f) | 0x40
        return k
    }

    /// The public key of `privateKey` (clamped first, like `newKeyPairFromSeed`).
    public static func publicKey(privateKey: [UInt8]) throws -> [UInt8] {
        try scalarMult(scalar: privateKey, u: basePoint)
    }

    /// The raw 32-byte shared secret (`sharedSecretKey`); all zero for a small-order peer key.
    public static func sharedSecret(privateKey: [UInt8], remotePublicKey: [UInt8]) throws -> [UInt8] {
        try scalarMult(scalar: privateKey, u: remotePublicKey)
    }

    /// RFC 7748 §5 X25519(k, u): k is clamped, u's top bit is masked.
    public static func scalarMult(scalar: [UInt8], u: [UInt8]) throws -> [UInt8] {
        guard u.count == keyBytes else { throw CryptoArgumentError("Public key must be 32 bytes") }
        guard scalar.count == keyBytes else { throw CryptoArgumentError("Seed must be 32 bytes") }
        #if canImport(CryptoKit)
        // CryptoKit for the ordinary case only. Peer keys that are not a canonical u (top bit
        // set, or u >= p) and low-order points, where CryptoKit may refuse where RFC 7748
        // answers, stay on the ladder below so behaviour cannot differ by platform.
        if isCanonicalU(u),
           let key = try? CryptoKit.Curve25519.KeyAgreement.PrivateKey(rawRepresentation: scalar),
           let peer = try? CryptoKit.Curve25519.KeyAgreement.PublicKey(rawRepresentation: u),
           let shared = try? key.sharedSecretFromKeyAgreement(with: peer) {
            let bytes = shared.withUnsafeBytes { Array($0) }
            if bytes.count == keyBytes && !CryptoBytes.isAllZero(bytes) { return bytes }
        }
        #endif
        return try scalarMultPortable(scalar: scalar, u: u)
    }

    /// Whether `u` is below p = 2^255 - 19 with the top bit clear.
    static func isCanonicalU(_ u: [UInt8]) -> Bool {
        if u[31] & 0x80 != 0 { return false }
        if u[31] != 0x7f { return true }
        for i in 1..<31 where u[i] != 0xff { return true }
        return u[0] < 0xed
    }

    private static let basePoint: [UInt8] = [9] + [UInt8](repeating: 0, count: 31)

    /// The pure Swift ladder, always compiled so tests can cross-check CryptoKit against it.
    public static func scalarMultPortable(scalar: [UInt8], u: [UInt8]) throws -> [UInt8] {
        guard u.count == keyBytes else { throw CryptoArgumentError("Public key must be 32 bytes") }
        var k = try clampPrivateKey(scalar)
        let x1 = Fe(bytes: u)
        var x2 = Fe.one
        var z2 = Fe.zero
        var x3 = x1
        var z3 = Fe.one
        var swap: Int64 = 0
        var t = 254
        while t >= 0 {
            let bit = Int64((k[t >> 3] >> UInt8(t & 7)) & 1)
            swap ^= bit
            Fe.cswap(&x2, &x3, swap)
            Fe.cswap(&z2, &z3, swap)
            swap = bit
            let a = Fe.add(x2, z2)
            let aa = Fe.sq(a)
            let b = Fe.sub(x2, z2)
            let bb = Fe.sq(b)
            let e = Fe.sub(aa, bb)
            let c = Fe.add(x3, z3)
            let d = Fe.sub(x3, z3)
            let da = Fe.mul(d, a)
            let cb = Fe.mul(c, b)
            x3 = Fe.sq(Fe.add(da, cb))
            z3 = Fe.mul(Fe.sq(Fe.sub(da, cb)), x1)
            x2 = Fe.mul(aa, bb)
            z2 = Fe.mul(Fe.add(Fe.mulSmall(e, 121665), aa), e)
            t -= 1
        }
        Fe.cswap(&x2, &x3, swap)
        Fe.cswap(&z2, &z3, swap)
        let out = Fe.mul(x2, Fe.invert(z2)).toBytes()
        CryptoBytes.wipe(&k)
        return out
    }
}
