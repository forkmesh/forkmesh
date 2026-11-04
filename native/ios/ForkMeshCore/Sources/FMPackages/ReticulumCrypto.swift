import FMCrypto
import Foundation

struct ReticulumAes256 {
    static let sbox: [UInt8] = {
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
        return sbox
    }()

    static let inverse: [UInt8] = {
        var out = [UInt8](repeating: 0, count: 256)
        for (index, value) in sbox.enumerated() { out[Int(value)] = UInt8(index) }
        return out
    }()

    static func multiply(_ a: UInt8, _ b: UInt8) -> UInt8 {
        var x = a
        var y = b
        var product: UInt8 = 0
        while y != 0 {
            if y & 1 != 0 { product ^= x }
            x = (x << 1) ^ (x & 0x80 != 0 ? 0x1b : 0)
            y >>= 1
        }
        return product
    }

    static let tables: [[UInt8]] = [2, 3, 9, 11, 13, 14].map { factor in (0..<256).map { multiply(UInt8($0), factor) } }

    private var roundKeys = [UInt8](repeating: 0, count: 240)

    init(key: [UInt8]) {
        let sbox = Self.sbox
        for index in 0..<32 { roundKeys[index] = key[index] }
        var rcon: UInt8 = 1
        var word = 8
        while word < 60 {
            var temp = Array(roundKeys[((word - 1) * 4)..<(word * 4)])
            if word % 8 == 0 {
                temp = [sbox[Int(temp[1])] ^ rcon, sbox[Int(temp[2])], sbox[Int(temp[3])], sbox[Int(temp[0])]]
                rcon = Self.multiply(rcon, 2)
            } else if word % 8 == 4 {
                temp = temp.map { sbox[Int($0)] }
            }
            for byte in 0..<4 { roundKeys[word * 4 + byte] = roundKeys[(word - 8) * 4 + byte] ^ temp[byte] }
            word += 1
        }
    }

    func encrypt(_ state: inout [UInt8]) {
        let sbox = Self.sbox
        let m2 = Self.tables[0]
        let m3 = Self.tables[1]
        for index in 0..<16 { state[index] ^= roundKeys[index] }
        for round in 1...14 {
            var shifted = [UInt8](repeating: 0, count: 16)
            for column in 0..<4 {
                for row in 0..<4 { shifted[row + 4 * column] = sbox[Int(state[row + 4 * ((column + row) % 4)])] }
            }
            if round < 14 {
                for column in 0..<4 {
                    let a0 = shifted[4 * column], a1 = shifted[4 * column + 1], a2 = shifted[4 * column + 2], a3 = shifted[4 * column + 3]
                    shifted[4 * column] = m2[Int(a0)] ^ m3[Int(a1)] ^ a2 ^ a3
                    shifted[4 * column + 1] = a0 ^ m2[Int(a1)] ^ m3[Int(a2)] ^ a3
                    shifted[4 * column + 2] = a0 ^ a1 ^ m2[Int(a2)] ^ m3[Int(a3)]
                    shifted[4 * column + 3] = m3[Int(a0)] ^ a1 ^ a2 ^ m2[Int(a3)]
                }
            }
            for index in 0..<16 { state[index] = shifted[index] ^ roundKeys[round * 16 + index] }
        }
    }

    func decrypt(_ state: inout [UInt8]) {
        let inverse = Self.inverse
        let m9 = Self.tables[2], m11 = Self.tables[3], m13 = Self.tables[4], m14 = Self.tables[5]
        for index in 0..<16 { state[index] ^= roundKeys[224 + index] }
        for round in stride(from: 13, through: 0, by: -1) {
            var shifted = [UInt8](repeating: 0, count: 16)
            for column in 0..<4 {
                for row in 0..<4 { shifted[row + 4 * ((column + row) % 4)] = inverse[Int(state[row + 4 * column])] }
            }
            for index in 0..<16 { shifted[index] ^= roundKeys[round * 16 + index] }
            if round > 0 {
                for column in 0..<4 {
                    let a0 = shifted[4 * column], a1 = shifted[4 * column + 1], a2 = shifted[4 * column + 2], a3 = shifted[4 * column + 3]
                    shifted[4 * column] = m14[Int(a0)] ^ m11[Int(a1)] ^ m13[Int(a2)] ^ m9[Int(a3)]
                    shifted[4 * column + 1] = m9[Int(a0)] ^ m14[Int(a1)] ^ m11[Int(a2)] ^ m13[Int(a3)]
                    shifted[4 * column + 2] = m13[Int(a0)] ^ m9[Int(a1)] ^ m14[Int(a2)] ^ m11[Int(a3)]
                    shifted[4 * column + 3] = m11[Int(a0)] ^ m13[Int(a1)] ^ m9[Int(a2)] ^ m14[Int(a3)]
                }
            }
            state = shifted
        }
    }

    static func cbcEncrypt(key: [UInt8], iv: [UInt8], plaintext: [UInt8]) -> [UInt8] {
        guard key.count == 32, iv.count == 16 else { return [] }
        let cipher = ReticulumAes256(key: key)
        let pad = 16 - plaintext.count % 16
        let padded = plaintext + [UInt8](repeating: UInt8(pad), count: pad)
        var out: [UInt8] = []
        out.reserveCapacity(padded.count)
        var previous = iv
        var block = [UInt8](repeating: 0, count: 16)
        var offset = 0
        while offset < padded.count {
            for index in 0..<16 { block[index] = padded[offset + index] ^ previous[index] }
            cipher.encrypt(&block)
            out += block
            previous = block
            offset += 16
        }
        return out
    }

    static func cbcDecrypt(key: [UInt8], iv: [UInt8], ciphertext: [UInt8]) -> [UInt8]? {
        guard key.count == 32, iv.count == 16, !ciphertext.isEmpty, ciphertext.count % 16 == 0 else { return nil }
        let cipher = ReticulumAes256(key: key)
        var out: [UInt8] = []
        out.reserveCapacity(ciphertext.count)
        var previous = iv
        var offset = 0
        while offset < ciphertext.count {
            let chunk = Array(ciphertext[offset..<(offset + 16)])
            var block = chunk
            cipher.decrypt(&block)
            for index in 0..<16 { block[index] ^= previous[index] }
            out += block
            previous = chunk
            offset += 16
        }
        guard let pad = out.last, pad >= 1, pad <= 16, out.suffix(Int(pad)).allSatisfy({ $0 == pad }) else { return nil }
        return Array(out.dropLast(Int(pad)))
    }
}

struct ReticulumSha256 {
    private static let k: [UInt32] = [
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
    ]

    private(set) var state: [UInt32] = [0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19]
    private(set) var length: UInt64 = 0

    mutating func absorbBlocks(_ data: [UInt8]) {
        precondition(data.count % 64 == 0)
        var offset = 0
        while offset < data.count {
            compress(data, offset)
            offset += 64
        }
        length += UInt64(data.count)
    }

    private mutating func compress(_ data: [UInt8], _ offset: Int) {
        var w = [UInt32](repeating: 0, count: 64)
        for index in 0..<16 {
            let base = offset + index * 4
            w[index] = UInt32(data[base]) << 24 | UInt32(data[base + 1]) << 16 | UInt32(data[base + 2]) << 8 | UInt32(data[base + 3])
        }
        for index in 16..<64 {
            let s0 = w[index - 15].rotated(7) ^ w[index - 15].rotated(18) ^ (w[index - 15] >> 3)
            let s1 = w[index - 2].rotated(17) ^ w[index - 2].rotated(19) ^ (w[index - 2] >> 10)
            w[index] = w[index - 16] &+ s0 &+ w[index - 7] &+ s1
        }
        var a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7]
        for index in 0..<64 {
            let s1 = e.rotated(6) ^ e.rotated(11) ^ e.rotated(25)
            let choice = (e & f) ^ (~e & g)
            let t1 = h &+ s1 &+ choice &+ Self.k[index] &+ w[index]
            let s0 = a.rotated(2) ^ a.rotated(13) ^ a.rotated(22)
            let majority = (a & b) ^ (a & c) ^ (b & c)
            let t2 = s0 &+ majority
            h = g; g = f; f = e; e = d &+ t1; d = c; c = b; b = a; a = t1 &+ t2
        }
        state[0] &+= a; state[1] &+= b; state[2] &+= c; state[3] &+= d
        state[4] &+= e; state[5] &+= f; state[6] &+= g; state[7] &+= h
    }

    func finish(_ tail: [UInt8]) -> [UInt8] {
        var copy = self
        let total = (length + UInt64(tail.count)) * 8
        var padded = tail + [0x80]
        while padded.count % 64 != 56 { padded.append(0) }
        for shift in stride(from: 56, through: 0, by: -8) { padded.append(UInt8(truncatingIfNeeded: total >> UInt64(shift))) }
        var offset = 0
        while offset < padded.count {
            copy.compress(padded, offset)
            offset += 64
        }
        var out: [UInt8] = []
        out.reserveCapacity(32)
        for word in copy.state { out += [UInt8(word >> 24), UInt8(word >> 16 & 0xff), UInt8(word >> 8 & 0xff), UInt8(word & 0xff)] }
        return out
    }
}

private extension UInt32 {
    func rotated(_ count: UInt32) -> UInt32 { (self >> count) | (self << (32 - count)) }
}

public final class ReticulumMsgPackWriter {
    public private(set) var bytes: [UInt8] = []

    public init() {}

    private func header(_ small: UInt8, _ smallLimit: Int, _ base8: UInt8, _ base16: UInt8, _ base32: UInt8, _ size: Int) {
        if size < smallLimit && small != 0 {
            bytes.append(small | UInt8(size))
        } else if size <= 0xff && base8 != 0 {
            bytes += [base8, UInt8(size)]
        } else if size <= 0xffff {
            bytes += [base16, UInt8(size >> 8), UInt8(size & 0xff)]
        } else {
            bytes.append(base32)
            for shift in stride(from: 24, through: 0, by: -8) { bytes.append(UInt8((size >> shift) & 0xff)) }
        }
    }

    @discardableResult public func null() -> Self {
        bytes.append(0xc0)
        return self
    }

    @discardableResult public func boolean(_ value: Bool) -> Self {
        bytes.append(value ? 0xc3 : 0xc2)
        return self
    }

    @discardableResult public func integer(_ value: Int64) -> Self {
        func big(_ type: UInt8, _ bits: UInt64, _ width: Int) {
            bytes.append(type)
            for shift in stride(from: (width - 1) * 8, through: 0, by: -8) { bytes.append(UInt8(truncatingIfNeeded: bits >> UInt64(shift))) }
        }
        let bits = UInt64(bitPattern: value)
        if value >= 0 {
            if value <= 0x7f { bytes.append(UInt8(value)) }
            else if value <= 0xff { big(0xcc, bits, 1) }
            else if value <= 0xffff { big(0xcd, bits, 2) }
            else if value <= 0xffff_ffff { big(0xce, bits, 4) }
            else { big(0xcf, bits, 8) }
        } else {
            if value >= -32 { bytes.append(UInt8(truncatingIfNeeded: value)) }
            else if value >= -128 { big(0xd0, bits, 1) }
            else if value >= -32768 { big(0xd1, bits, 2) }
            else if value >= -2_147_483_648 { big(0xd2, bits, 4) }
            else { big(0xd3, bits, 8) }
        }
        return self
    }

    @discardableResult public func real(_ value: Double) -> Self {
        bytes.append(0xcb)
        let bits = value.bitPattern
        for shift in stride(from: 56, through: 0, by: -8) { bytes.append(UInt8(truncatingIfNeeded: bits >> UInt64(shift))) }
        return self
    }

    @discardableResult public func bin(_ value: [UInt8]) -> Self {
        header(0, 0, 0xc4, 0xc5, 0xc6, value.count)
        bytes += value
        return self
    }

    @discardableResult public func str(_ value: String) -> Self {
        let utf8 = Array(value.utf8)
        header(0xa0, 32, 0xd9, 0xda, 0xdb, utf8.count)
        bytes += utf8
        return self
    }

    @discardableResult public func array(_ count: Int) -> Self {
        header(0x90, 16, 0, 0xdc, 0xdd, count)
        return self
    }

    @discardableResult public func map(_ count: Int) -> Self {
        header(0x80, 16, 0, 0xde, 0xdf, count)
        return self
    }

    @discardableResult public func raw(_ value: [UInt8]) -> Self {
        bytes += value
        return self
    }

}

public struct ReticulumAccessCode: Equatable, Sendable {
    public static let salt: [UInt8] = CryptoBytes.fromHex("adf54d882c9a9b80771eb4995d702d4a3e733391b2a0f53f416d9f907e55cff8") ?? []

    public private(set) var key: [UInt8] = []
    private var signingKey: [UInt8] = []
    public private(set) var size = 0

    public init() {}

    public init(networkName: String, passphrase: String, size: Int = 16) {
        guard !(networkName.isEmpty && passphrase.isEmpty), (1...64).contains(size) else { return }
        var origin: [UInt8] = []
        if !networkName.isEmpty { origin += ReticulumWire.fullHash(Array(networkName.utf8)) }
        if !passphrase.isEmpty { origin += ReticulumWire.fullHash(Array(passphrase.utf8)) }
        key = ReticulumWire.hkdf(length: 64, material: ReticulumWire.fullHash(origin), salt: Self.salt)
        signingKey = Array(key[32..<64])
        self.size = size
    }

    public var enabled: Bool { size > 0 }

    private func mask(_ framed: inout [UInt8], code: [UInt8]) {
        let stream = ReticulumWire.hkdf(length: framed.count, material: code, salt: key)
        for index in 0..<framed.count where !(index >= 2 && index < 2 + size) {
            framed[index] ^= stream[index]
        }
    }

    public func apply(_ raw: [UInt8]) -> [UInt8] {
        guard enabled, raw.count >= 2 else { return raw }
        let code = Array(ReticulumCipher.sign(signingKey, raw).suffix(size))
        var framed = [raw[0] | 0x80, raw[1]] + code + raw[2...]
        mask(&framed, code: code)
        framed[0] |= 0x80
        return framed
    }

    public func remove(_ raw: [UInt8]) -> [UInt8]? {
        guard enabled, raw.count > 2 + size, raw[0] & 0x80 != 0 else { return nil }
        let code = Array(raw[2..<(2 + size)])
        var unmasked = raw
        mask(&unmasked, code: code)
        let packet = [unmasked[0] & 0x7f, unmasked[1]] + unmasked[(2 + size)...]
        let expected = Array(ReticulumCipher.sign(signingKey, packet).suffix(size))
        guard expected.count == code.count, CryptoBytes.constantTimeEquals(expected, code) else { return nil }
        return packet
    }
}

public enum ReticulumCipher {
    public static func x25519Public(_ privateKey: [UInt8]) -> [UInt8] {
        guard privateKey.count == 32 else { return [] }
        return (try? X25519.publicKey(privateKey: privateKey)) ?? []
    }

    public static func x25519Exchange(_ privateKey: [UInt8], _ peerPublicKey: [UInt8]) -> [UInt8] {
        guard privateKey.count == 32, peerPublicKey.count == 32,
              let shared = try? X25519.sharedSecret(privateKey: privateKey, remotePublicKey: peerPublicKey),
              !CryptoBytes.isAllZero(shared) else { return [] }
        return shared
    }

    public static func ed25519Public(_ privateKey: [UInt8]) -> [UInt8] {
        guard privateKey.count == 32 else { return [] }
        return (try? Ed25519.publicKey(seed: privateKey)) ?? []
    }

    public static func sign(_ privateKey: [UInt8], _ data: [UInt8]) -> [UInt8] {
        guard privateKey.count == 32 else { return [] }
        return (try? Ed25519.sign(seed: privateKey, message: data)) ?? []
    }

    public static func verify(_ publicKey: [UInt8], signature: [UInt8], data: [UInt8]) -> Bool {
        guard publicKey.count == 32, signature.count == Reticulum.signatureLength else { return false }
        return Ed25519.verify(publicKey: publicKey, message: data, signature: signature)
    }

    public static func tokenEncrypt(key: [UInt8], plaintext: [UInt8], iv: [UInt8] = []) -> [UInt8] {
        guard key.count == 64 else { return [] }
        let vector = iv.count == 16 ? iv : ReticulumWire.randomBytes(16)
        let ciphertext = ReticulumAes256.cbcEncrypt(key: Array(key[32..<64]), iv: vector, plaintext: plaintext)
        guard !ciphertext.isEmpty else { return [] }
        let signed = vector + ciphertext
        return signed + ReticulumWire.hmacSha256(key: Array(key[0..<32]), data: signed)
    }

    public static func tokenDecrypt(key: [UInt8], token: [UInt8]) -> [UInt8]? {
        guard key.count == 64, token.count >= 64, (token.count - 48) % 16 == 0 else { return nil }
        let mac = ReticulumWire.hmacSha256(key: Array(key[0..<32]), data: Array(token.prefix(token.count - 32)))
        guard CryptoBytes.constantTimeEquals(mac, Array(token.suffix(32))) else { return nil }
        return ReticulumAes256.cbcDecrypt(key: Array(key[32..<64]), iv: Array(token.prefix(16)),
                                          ciphertext: Array(token[16..<(token.count - 32)]))
    }

    public static func buildProof(signingPrivateKey: [UInt8], hash: [UInt8], explicit: Bool, destination: [UInt8],
                                  destinationType: ReticulumDestinationType) -> [UInt8] {
        let signature = sign(signingPrivateKey, hash)
        guard !signature.isEmpty else { return [] }
        var packet = ReticulumPacket()
        packet.packetType = .proof
        packet.destinationType = destinationType
        packet.destinationHash = destination
        packet.data = explicit ? hash + signature : signature
        return ReticulumWire.packPacket(packet)
    }

    public static func validateProof(signingPublicKey: [UInt8], hash: [UInt8], proof: [UInt8]) -> Bool {
        if proof.count == Reticulum.fullHashLength + Reticulum.signatureLength {
            return Array(proof.prefix(Reticulum.fullHashLength)) == hash
                && verify(signingPublicKey, signature: Array(proof.suffix(Reticulum.signatureLength)), data: hash)
        }
        if proof.count == Reticulum.signatureLength { return verify(signingPublicKey, signature: proof, data: hash) }
        return false
    }
}

extension ReticulumIdentity {
    public func encrypt(_ plaintext: [UInt8], ratchet: [UInt8] = []) -> [UInt8] {
        guard isValid else { return [] }
        let ephemeral = ReticulumIdentity.generate()
        let target = ratchet.count == 32 ? ratchet : encryptionPublicKey
        let shared = ReticulumCipher.x25519Exchange(ephemeral.encryptionPrivateKey, target)
        guard !shared.isEmpty else { return [] }
        let token = ReticulumCipher.tokenEncrypt(key: ReticulumWire.hkdf(length: 64, material: shared, salt: hash), plaintext: plaintext)
        guard !token.isEmpty else { return [] }
        return ephemeral.encryptionPublicKey + token
    }

    public func decrypt(_ token: [UInt8]) -> [UInt8]? {
        guard hasPrivateKey, token.count > 32 else { return nil }
        let shared = ReticulumCipher.x25519Exchange(encryptionPrivateKey, Array(token.prefix(32)))
        guard !shared.isEmpty else { return nil }
        return ReticulumCipher.tokenDecrypt(key: ReticulumWire.hkdf(length: 64, material: shared, salt: hash), token: Array(token.dropFirst(32)))
    }

    public var signingPrivateKey: [UInt8] { hasPrivateKey ? Array(privateKey[32..<64]) : [] }
}

extension ReticulumWire {
    public static func msgPackArrayItems(_ bytes: [UInt8]) -> [Range<Int>]? {
        guard let type = bytes.first else { return nil }
        var count = 0
        var offset = 1
        if type & 0xf0 == 0x90 {
            count = Int(type & 0x0f)
        } else if type == 0xdc, bytes.count >= 3 {
            count = Int(bytes[1]) << 8 | Int(bytes[2])
            offset = 3
        } else {
            return nil
        }
        guard count <= 64, decodeMsgPack(bytes) != nil else { return nil }
        var items: [Range<Int>] = []
        var position = offset
        for _ in 0..<count {
            guard let next = ReticulumMsgPackCursor.end(bytes, position, depth: 1) else { return nil }
            items.append(position..<next)
            position = next
        }
        return items
    }
}

enum ReticulumMsgPackCursor {
    static func end(_ bytes: [UInt8], _ start: Int, depth: Int) -> Int? {
        guard depth <= 8, start < bytes.count else { return nil }
        let type = bytes[start]
        let available = bytes.count - start
        func uint(_ width: Int) -> Int? {
            guard available >= 1 + width else { return nil }
            var value = 0
            for index in 1...width { value = value << 8 | Int(bytes[start + index]) }
            return value
        }
        func children(_ count: Int, header: Int) -> Int? {
            var position = start + header
            for _ in 0..<count {
                guard let next = end(bytes, position, depth: depth + 1) else { return nil }
                position = next
            }
            return position
        }
        func fixed(_ length: Int) -> Int? { length <= available ? start + length : nil }
        func sized(_ width: Int) -> Int? { uint(width).flatMap { fixed(1 + width + $0) } }
        switch type {
        case 0x00...0x7f, 0xe0...0xff, 0xc0, 0xc2, 0xc3: return start + 1
        case 0x80...0x8f: return children(Int(type & 0x0f) * 2, header: 1)
        case 0x90...0x9f: return children(Int(type & 0x0f), header: 1)
        case 0xa0...0xbf: return fixed(1 + Int(type & 0x1f))
        case 0xc4, 0xd9: return sized(1)
        case 0xc5, 0xda: return sized(2)
        case 0xc6, 0xdb: return sized(4)
        case 0xca, 0xce, 0xd2: return fixed(5)
        case 0xcb, 0xcf, 0xd3: return fixed(9)
        case 0xcc, 0xd0: return fixed(2)
        case 0xcd, 0xd1: return fixed(3)
        case 0xdc: return uint(2).flatMap { children($0, header: 3) }
        case 0xdd: return uint(4).flatMap { children($0, header: 5) }
        case 0xde: return uint(2).flatMap { children($0 * 2, header: 3) }
        case 0xdf: return uint(4).flatMap { children($0 * 2, header: 5) }
        default: return nil
        }
    }
}
