import FMCrypto
import Foundation

public enum Reticulum {
    public static let hashLength = 16
    public static let fullHashLength = 32
    public static let nameHashLength = 10
    public static let randomHashLength = 10
    public static let publicKeyLength = 64
    public static let privateKeyLength = 64
    public static let signatureLength = 64
    public static let ratchetLength = 32
    public static let mtu = 500
    public static let headerMinSize = 2 + 1 + hashLength
    public static let headerMaxSize = 2 + 1 + hashLength * 2
    public static let streamHardwareMtu = 262_144
    public static let maxHops = 128
    public static let stampSize = 32
    public static let discoveryExpandRounds = 20
    public static let discoveryStampValue = 16
    public static let nodeAspect = "forkmesh.node"
}

public struct ReticulumError: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
}

public enum ReticulumPacketType: Int, Sendable, Equatable {
    case data = 0
    case announce = 1
    case linkRequest = 2
    case proof = 3
}

public enum ReticulumDestinationType: Int, Sendable, Equatable {
    case single = 0
    case group = 1
    case plain = 2
    case link = 3
}

public enum ReticulumContext {
    public static let none: UInt8 = 0x00
    public static let resource: UInt8 = 0x01
    public static let resourceAdvertisement: UInt8 = 0x02
    public static let resourceRequest: UInt8 = 0x03
    public static let resourceHashmapUpdate: UInt8 = 0x04
    public static let resourceProof: UInt8 = 0x05
    public static let resourceInitiatorCancel: UInt8 = 0x06
    public static let resourceReceiverCancel: UInt8 = 0x07
    public static let cacheRequest: UInt8 = 0x08
    public static let request: UInt8 = 0x09
    public static let response: UInt8 = 0x0A
    public static let pathResponse: UInt8 = 0x0B
    public static let command: UInt8 = 0x0C
    public static let commandStatus: UInt8 = 0x0D
    public static let channel: UInt8 = 0x0E
    public static let keepalive: UInt8 = 0xFA
    public static let linkIdentify: UInt8 = 0xFB
    public static let linkClose: UInt8 = 0xFC
    public static let linkProof: UInt8 = 0xFD
    public static let linkRtt: UInt8 = 0xFE
    public static let linkRequestProof: UInt8 = 0xFF
}

public struct ReticulumPacket: Equatable, Sendable {
    public var ifac = false
    public var headerType = 0
    public var contextFlag = false
    public var transportType = 0
    public var destinationType: ReticulumDestinationType = .single
    public var packetType: ReticulumPacketType = .data
    public var hops = 0
    public var transportId: [UInt8] = []
    public var destinationHash: [UInt8] = []
    public var context: UInt8 = ReticulumContext.none
    public var data: [UInt8] = []

    public init() {}
}

public struct ReticulumAnnounce: Equatable, Sendable {
    public var destinationHash: [UInt8] = []
    public var publicKey: [UInt8] = []
    public var nameHash: [UInt8] = []
    public var randomHash: [UInt8] = []
    public var ratchet: [UInt8] = []
    public var signature: [UInt8] = []
    public var appData: [UInt8] = []
    public var hasAppData = false
    public var identityHash: [UInt8] = []
    public var emittedAt: Int64 = 0
}

public struct ReticulumKnownAspect: Equatable, Sendable {
    public let name: String
    public let label: String
    public let kind: String
    public let nameHash: [UInt8]
}

public struct ReticulumDiscoveredInterface: Equatable, Sendable {
    public var name = ""
    public var type = ""
    public var reachableOn = ""
    public var port = 0
    public var transport = false
    public var hasLocation = false
    public var latitude: Double = 0
    public var longitude: Double = 0
    public var height: Double = 0
    public var implementation = ""
    public var version = ""
    public var transportId: [UInt8] = []
    public var networkName = ""
    public var publishesPassphrase = false
    public var frequency: Double = 0
    public var bandwidth: Double = 0
    public var modulation = ""
    public var encrypted = false
    public var stampValue = 0
    public var stampValid = false

    public var connectable: Bool {
        !reachableOn.isEmpty && port > 0 && (type == "TCPServerInterface" || type == "BackboneInterface")
    }
}

public struct ReticulumIdentity: Equatable, Sendable {
    public private(set) var publicKey: [UInt8] = []
    public private(set) var privateKey: [UInt8] = []

    public init() {}

    public static func generate() -> ReticulumIdentity {
        let encryption = CryptoBytes.random(32)
        let signing = Ed25519.generateSeed()
        return ReticulumIdentity(privateKey: encryption + signing) ?? ReticulumIdentity()
    }

    public init?(privateKey key: [UInt8]) {
        guard key.count == Reticulum.privateKeyLength,
              let encryption = try? X25519.publicKey(privateKey: Array(key[0..<32])),
              let signing = try? Ed25519.publicKey(seed: Array(key[32..<64])) else { return nil }
        privateKey = key
        publicKey = encryption + signing
    }

    public init?(publicKey key: [UInt8]) {
        guard key.count == Reticulum.publicKeyLength else { return nil }
        publicKey = key
    }

    public var isValid: Bool { publicKey.count == Reticulum.publicKeyLength }
    public var hasPrivateKey: Bool { privateKey.count == Reticulum.privateKeyLength }
    public var hash: [UInt8] { ReticulumWire.truncatedHash(publicKey) }
    public var encryptionPublicKey: [UInt8] { isValid ? Array(publicKey[0..<32]) : [] }
    public var signingPublicKey: [UInt8] { isValid ? Array(publicKey[32..<64]) : [] }
    public var encryptionPrivateKey: [UInt8] { hasPrivateKey ? Array(privateKey[0..<32]) : [] }

    public func sign(_ data: [UInt8]) -> [UInt8] {
        guard hasPrivateKey else { return [] }
        return (try? Ed25519.sign(seed: Array(privateKey[32..<64]), message: data, publicKey: signingPublicKey)) ?? []
    }

    public func verify(signature: [UInt8], data: [UInt8]) -> Bool {
        guard isValid, signature.count == Reticulum.signatureLength else { return false }
        return Ed25519.verify(publicKey: signingPublicKey, message: data, signature: signature)
    }
}

public enum ReticulumFraming: Sendable, Equatable {
    case hdlc
    case kiss
}

public struct ReticulumFrameDecoder: Sendable {
    private static let hdlcFlag: UInt8 = 0x7E
    private static let hdlcEscape: UInt8 = 0x7D
    private static let hdlcMask: UInt8 = 0x20
    private static let kissFend: UInt8 = 0xC0
    private static let kissFesc: UInt8 = 0xDB
    private static let kissTfend: UInt8 = 0xDC
    private static let kissTfesc: UInt8 = 0xDD
    private static let kissData = 0x00

    public private(set) var framing: ReticulumFraming
    public private(set) var invalidFrames: Int64 = 0
    private let maximumFrame: Int
    private var buffer: [UInt8] = []
    private var inFrame = false
    private var escape = false
    private var kissCommand = -1
    private var overflow = false

    public init(framing: ReticulumFraming = .hdlc, maximumFrame: Int = Reticulum.streamHardwareMtu) {
        self.framing = framing
        self.maximumFrame = maximumFrame
    }

    public mutating func reset() {
        buffer.removeAll()
        inFrame = false
        escape = false
        kissCommand = -1
        overflow = false
    }

    public mutating func setFraming(_ value: ReticulumFraming) {
        framing = value
        reset()
    }

    public mutating func feed(_ bytes: [UInt8]) -> [[UInt8]] {
        framing == .kiss ? feedKiss(bytes) : feedHdlc(bytes)
    }

    private mutating func finishFrame(_ frames: inout [[UInt8]]) {
        if overflow {
            invalidFrames += 1
        } else if !buffer.isEmpty {
            if buffer.count <= Reticulum.headerMinSize {
                invalidFrames += 1
            } else {
                frames.append(buffer)
            }
        }
        buffer.removeAll(keepingCapacity: true)
        escape = false
        overflow = false
    }

    private mutating func feedHdlc(_ bytes: [UInt8]) -> [[UInt8]] {
        var frames: [[UInt8]] = []
        for value in bytes {
            if value == Self.hdlcFlag {
                if inFrame { finishFrame(&frames) }
                inFrame = true
                continue
            }
            if !inFrame || overflow { continue }
            if escape {
                escape = false
                buffer.append(value ^ Self.hdlcMask)
            } else if value == Self.hdlcEscape {
                escape = true
                continue
            } else {
                buffer.append(value)
            }
            if buffer.count > maximumFrame {
                overflow = true
                buffer.removeAll()
            }
        }
        return frames
    }

    private mutating func feedKiss(_ bytes: [UInt8]) -> [[UInt8]] {
        var frames: [[UInt8]] = []
        for value in bytes {
            if value == Self.kissFend {
                if inFrame && kissCommand == Self.kissData { finishFrame(&frames) }
                inFrame = true
                kissCommand = -1
                buffer.removeAll(keepingCapacity: true)
                escape = false
                overflow = false
                continue
            }
            if !inFrame { continue }
            if kissCommand < 0 {
                kissCommand = Int(value & 0x0F)
                continue
            }
            if kissCommand != Self.kissData || overflow { continue }
            if value == Self.kissFesc {
                escape = true
                continue
            }
            var out = value
            if escape {
                if value == Self.kissTfend {
                    out = Self.kissFend
                } else if value == Self.kissTfesc {
                    out = Self.kissFesc
                }
                escape = false
            }
            buffer.append(out)
            if buffer.count > maximumFrame {
                overflow = true
                buffer.removeAll()
            }
        }
        return frames
    }
}

public indirect enum ReticulumMsgPack: Equatable, Sendable {
    case none
    case bool(Bool)
    case int(Int64)
    case uint(UInt64)
    case double(Double)
    case string(String)
    case bytes([UInt8])
    case array([ReticulumMsgPack])
    case map([String: ReticulumMsgPack])

    public var isNone: Bool { self == .none }

    public var list: [ReticulumMsgPack] {
        if case .array(let items) = self { return items }
        return []
    }

    public var dictionary: [String: ReticulumMsgPack] {
        if case .map(let items) = self { return items }
        return [:]
    }

    public var text: String {
        switch self {
        case .none, .array, .map: return ""
        case .bool(let value): return value ? "true" : "false"
        case .int(let value): return String(value)
        case .uint(let value): return String(value)
        case .double(let value): return ReticulumMsgPack.number(value)
        case .string(let value): return value
        case .bytes(let value): return String(decoding: value, as: UTF8.self)
        }
    }

    public var boolValue: Bool {
        switch self {
        case .bool(let value): return value
        case .int(let value): return value != 0
        case .uint(let value): return value != 0
        case .double(let value): return value != 0
        case .string, .bytes:
            let value = text.lowercased()
            return !value.isEmpty && value != "0" && value != "false"
        default: return false
        }
    }

    public var doubleValue: Double {
        switch self {
        case .bool(let value): return value ? 1 : 0
        case .int(let value): return Double(value)
        case .uint(let value): return Double(value)
        case .double(let value): return value
        case .string, .bytes: return Double(text.trimmingCharacters(in: .whitespaces)) ?? 0
        default: return 0
        }
    }

    public var intValue: Int {
        let limit = Double(Int32.max)
        switch self {
        case .bool(let value): return value ? 1 : 0
        case .int(let value): return value >= Int64(Int32.min) && value <= Int64(Int32.max) ? Int(value) : 0
        case .uint(let value): return value <= UInt64(Int32.max) ? Int(value) : 0
        case .double(let value):
            guard value.isFinite, abs(value) <= limit else { return 0 }
            return Int(value.rounded())
        case .string, .bytes: return Int(Int32(text.trimmingCharacters(in: .whitespaces)) ?? 0)
        default: return 0
        }
    }

    public var int64Value: Int64 {
        switch self {
        case .bool(let value): return value ? 1 : 0
        case .int(let value): return value
        case .uint(let value): return value <= UInt64(Int64.max) ? Int64(value) : 0
        case .double(let value): return value.isFinite && abs(value) < 9e18 ? Int64(value.rounded()) : 0
        case .string, .bytes: return Int64(text.trimmingCharacters(in: .whitespaces)) ?? 0
        default: return 0
        }
    }

    public var byteValue: [UInt8] {
        switch self {
        case .bytes(let value): return value
        case .none, .array, .map: return []
        default: return Array(text.utf8)
        }
    }

    static func number(_ value: Double) -> String {
        if value.isNaN { return "nan" }
        if value.isInfinite { return value < 0 ? "-inf" : "inf" }
        if value == value.rounded(), abs(value) < 1e16 { return String(Int64(value)) }
        return "\(value)"
    }
}

private struct ReticulumMsgPackReader {
    let bytes: [UInt8]
    var offset = 0
    var items = 0

    init(_ bytes: [UInt8]) { self.bytes = bytes }

    var remaining: UInt64 { UInt64(bytes.count - offset) }

    mutating func read(_ depth: Int, _ ok: inout Bool) -> ReticulumMsgPack {
        if depth > 8 || offset >= bytes.count {
            ok = false
            return .none
        }
        items += 1
        if items > 4096 {
            ok = false
            return .none
        }
        let type = bytes[offset]
        offset += 1
        if type <= 0x7f { return .int(Int64(type)) }
        if type >= 0xe0 { return .int(Int64(Int8(bitPattern: type))) }
        if type & 0xf0 == 0x80 { return readMap(UInt64(type & 0x0f), depth, &ok) }
        if type & 0xf0 == 0x90 { return readArray(UInt64(type & 0x0f), depth, &ok) }
        if type & 0xe0 == 0xa0 { return readString(UInt64(type & 0x1f), &ok) }
        switch type {
        case 0xc0: return .none
        case 0xc2: return .bool(false)
        case 0xc3: return .bool(true)
        case 0xc4: return readBytes(readUnsigned(1, &ok), &ok)
        case 0xc5: return readBytes(readUnsigned(2, &ok), &ok)
        case 0xc6: return readBytes(readUnsigned(4, &ok), &ok)
        case 0xca: return .double(Double(Float(bitPattern: UInt32(truncatingIfNeeded: readUnsigned(4, &ok)))))
        case 0xcb: return .double(Double(bitPattern: readUnsigned(8, &ok)))
        case 0xcc: return .uint(readUnsigned(1, &ok))
        case 0xcd: return .uint(readUnsigned(2, &ok))
        case 0xce: return .uint(readUnsigned(4, &ok))
        case 0xcf: return .uint(readUnsigned(8, &ok))
        case 0xd0: return .int(Int64(Int8(truncatingIfNeeded: readUnsigned(1, &ok))))
        case 0xd1: return .int(Int64(Int16(truncatingIfNeeded: readUnsigned(2, &ok))))
        case 0xd2: return .int(Int64(Int32(truncatingIfNeeded: readUnsigned(4, &ok))))
        case 0xd3: return .int(Int64(bitPattern: readUnsigned(8, &ok)))
        case 0xd9: return readString(readUnsigned(1, &ok), &ok)
        case 0xda: return readString(readUnsigned(2, &ok), &ok)
        case 0xdb: return readString(readUnsigned(4, &ok), &ok)
        case 0xdc: return readArray(readUnsigned(2, &ok), depth, &ok)
        case 0xdd: return readArray(readUnsigned(4, &ok), depth, &ok)
        case 0xde: return readMap(readUnsigned(2, &ok), depth, &ok)
        case 0xdf: return readMap(readUnsigned(4, &ok), depth, &ok)
        default:
            ok = false
            return .none
        }
    }

    mutating func readUnsigned(_ width: Int, _ ok: inout Bool) -> UInt64 {
        if offset + width > bytes.count {
            ok = false
            return 0
        }
        var value: UInt64 = 0
        for _ in 0..<width {
            value = (value << 8) | UInt64(bytes[offset])
            offset += 1
        }
        return value
    }

    mutating func readRaw(_ length: UInt64, _ ok: inout Bool) -> [UInt8] {
        if !ok || length > remaining {
            ok = false
            return []
        }
        let out = Array(bytes[offset..<(offset + Int(length))])
        offset += Int(length)
        return out
    }

    mutating func readBytes(_ length: UInt64, _ ok: inout Bool) -> ReticulumMsgPack {
        let raw = readRaw(length, &ok)
        return ok ? .bytes(raw) : .none
    }

    mutating func readString(_ length: UInt64, _ ok: inout Bool) -> ReticulumMsgPack {
        let raw = readRaw(length, &ok)
        return ok ? .string(String(decoding: raw, as: UTF8.self)) : .none
    }

    mutating func readArray(_ count: UInt64, _ depth: Int, _ ok: inout Bool) -> ReticulumMsgPack {
        if !ok || count > remaining {
            ok = false
            return .none
        }
        var list: [ReticulumMsgPack] = []
        var index: UInt64 = 0
        while index < count && ok {
            list.append(read(depth + 1, &ok))
            index += 1
        }
        return .array(list)
    }

    mutating func readMap(_ count: UInt64, _ depth: Int, _ ok: inout Bool) -> ReticulumMsgPack {
        if !ok || count.multipliedReportingOverflow(by: 2).partialValue > remaining || count > UInt64(Int32.max) {
            ok = false
            return .none
        }
        var map: [String: ReticulumMsgPack] = [:]
        var index: UInt64 = 0
        while index < count && ok {
            let key = read(depth + 1, &ok)
            let value = read(depth + 1, &ok)
            map[key.text] = value
            index += 1
        }
        return .map(map)
    }
}

public enum ReticulumWire {
    public static func fullHash(_ data: [UInt8]) -> [UInt8] { Sha256.hash(data) }

    public static func truncatedHash(_ data: [UInt8]) -> [UInt8] { Array(fullHash(data).prefix(Reticulum.hashLength)) }

    public static func hmacSha256(key: [UInt8], data: [UInt8]) -> [UInt8] { HmacSha256.mac(key: key, data) }

    public static func hkdf(length: Int, material: [UInt8], salt: [UInt8], context: [UInt8] = []) -> [UInt8] {
        guard length >= 1, !material.isEmpty else { return [] }
        let key = hmacSha256(key: salt.isEmpty ? [UInt8](repeating: 0, count: Reticulum.fullHashLength) : salt, data: material)
        var block: [UInt8] = []
        var derived: [UInt8] = []
        var index = 0
        while derived.count < length {
            block = hmacSha256(key: key, data: block + context + [UInt8((index + 1) % 256)])
            derived += block
            index += 1
        }
        return Array(derived.prefix(length))
    }

    public static func randomBytes(_ count: Int) -> [UInt8] { count > 0 ? CryptoBytes.random(count) : [] }

    public static func validAspectName(_ name: String) -> Bool {
        guard !name.isEmpty, name.utf16.count <= 128 else { return false }
        let whitespace: Set<Character> = [" ", "\t", "\n", "\r", "\u{0B}", "\u{0C}"]
        let parts = name.split(separator: ".", omittingEmptySubsequences: false)
        return parts.allSatisfy { part in !part.isEmpty && !part.contains { whitespace.contains($0) } }
    }

    public static func nameHash(_ fullName: String) -> [UInt8] {
        Array(fullHash(Array(fullName.utf8)).prefix(Reticulum.nameHashLength))
    }

    public static func destinationHash(nameHash: [UInt8], identityHash: [UInt8] = []) -> [UInt8] {
        truncatedHash(nameHash + identityHash)
    }

    public static func destinationHash(name: String, identityHash: [UInt8] = []) -> [UInt8] {
        destinationHash(nameHash: nameHash(name), identityHash: identityHash)
    }

    public static func packedFlags(_ packet: ReticulumPacket) -> UInt8 {
        var flags: UInt8 = packet.ifac ? 0x80 : 0
        flags |= UInt8(packet.headerType & 1) << 6
        flags |= (packet.contextFlag ? 1 : 0) << 5
        flags |= UInt8(packet.transportType & 1) << 4
        flags |= UInt8(packet.destinationType.rawValue & 3) << 2
        flags |= UInt8(packet.packetType.rawValue & 3)
        return flags
    }

    public static func parsePacket(_ raw: [UInt8]) throws -> ReticulumPacket {
        guard raw.count > Reticulum.headerMinSize else { throw ReticulumError("frame shorter than a Reticulum header") }
        var packet = ReticulumPacket()
        let flags = raw[0]
        packet.ifac = flags & 0x80 != 0
        packet.headerType = Int((flags & 0x40) >> 6)
        packet.contextFlag = flags & 0x20 != 0
        packet.transportType = Int((flags & 0x10) >> 4)
        packet.destinationType = ReticulumDestinationType(rawValue: Int((flags & 0x0C) >> 2)) ?? .single
        packet.packetType = ReticulumPacketType(rawValue: Int(flags & 0x03)) ?? .data
        packet.hops = Int(raw[1])
        if packet.ifac { throw ReticulumError("packet carries an interface access code") }
        if packet.hops >= Reticulum.maxHops { throw ReticulumError("hop count \(packet.hops) is out of range") }
        var offset = 2
        if packet.headerType == 1 {
            guard raw.count > Reticulum.headerMaxSize else { throw ReticulumError("frame shorter than a transport header") }
            packet.transportId = Array(raw[offset..<(offset + Reticulum.hashLength)])
            offset += Reticulum.hashLength
        }
        packet.destinationHash = Array(raw[offset..<(offset + Reticulum.hashLength)])
        offset += Reticulum.hashLength
        packet.context = raw[offset]
        offset += 1
        packet.data = Array(raw[offset...])
        if packet.data.isEmpty { throw ReticulumError("packet has no data") }
        return packet
    }

    private static func padded(_ bytes: [UInt8]) -> [UInt8] {
        let head = Array(bytes.prefix(Reticulum.hashLength))
        return head + [UInt8](repeating: 0, count: Reticulum.hashLength - head.count)
    }

    public static func packPacket(_ packet: ReticulumPacket) -> [UInt8] {
        var raw: [UInt8] = [packedFlags(packet), UInt8(truncatingIfNeeded: packet.hops)]
        raw.reserveCapacity(Reticulum.headerMaxSize + packet.data.count)
        if packet.headerType == 1 { raw += padded(packet.transportId) }
        raw += padded(packet.destinationHash)
        raw.append(packet.context)
        raw += packet.data
        return raw
    }

    public static func packetHash(_ raw: [UInt8]) -> [UInt8] {
        guard raw.count >= 3 else { return [] }
        let header2 = raw[0] & 0x40 != 0
        let start = header2 ? Reticulum.hashLength + 2 : 2
        let tail = start < raw.count ? Array(raw[start...]) : []
        return fullHash([raw[0] & 0x0F] + tail)
    }

    public static func hdlcFrame(_ data: [UInt8]) -> [UInt8] {
        var out: [UInt8] = [0x7E]
        out.reserveCapacity(data.count + 8)
        for value in data {
            if value == 0x7D || value == 0x7E {
                out.append(0x7D)
                out.append(value ^ 0x20)
            } else {
                out.append(value)
            }
        }
        out.append(0x7E)
        return out
    }

    public static func hdlcKeepalive() -> [UInt8] { [0x7E, 0x7E] }

    public static func kissFrame(_ data: [UInt8]) -> [UInt8] {
        var out: [UInt8] = [0xC0, 0x00]
        out.reserveCapacity(data.count + 8)
        for value in data {
            if value == 0xDB {
                out += [0xDB, 0xDD]
            } else if value == 0xC0 {
                out += [0xDB, 0xDC]
            } else {
                out.append(value)
            }
        }
        out.append(0xC0)
        return out
    }

    public static func parseAnnounce(_ packet: ReticulumPacket) throws -> ReticulumAnnounce {
        guard packet.packetType == .announce else { throw ReticulumError("not an announce") }
        let ratchetLength = packet.contextFlag ? Reticulum.ratchetLength : 0
        let fixed = Reticulum.publicKeyLength + Reticulum.nameHashLength + Reticulum.randomHashLength + ratchetLength + Reticulum.signatureLength
        guard packet.data.count >= fixed else {
            throw ReticulumError("announce is \(packet.data.count) bytes, shorter than \(fixed)")
        }
        var announce = ReticulumAnnounce()
        var offset = 0
        func take(_ count: Int) -> [UInt8] {
            let out = Array(packet.data[offset..<(offset + count)])
            offset += count
            return out
        }
        announce.destinationHash = packet.destinationHash
        announce.publicKey = take(Reticulum.publicKeyLength)
        announce.nameHash = take(Reticulum.nameHashLength)
        announce.randomHash = take(Reticulum.randomHashLength)
        announce.ratchet = take(ratchetLength)
        announce.signature = take(Reticulum.signatureLength)
        announce.appData = Array(packet.data[offset...])
        announce.hasAppData = !announce.appData.isEmpty
        announce.identityHash = truncatedHash(announce.publicKey)
        var emitted: Int64 = 0
        for index in 5..<Reticulum.randomHashLength { emitted = (emitted << 8) | Int64(announce.randomHash[index]) }
        announce.emittedAt = emitted
        return announce
    }

    public static func validateAnnounce(_ announce: ReticulumAnnounce) throws {
        guard let identity = ReticulumIdentity(publicKey: announce.publicKey) else {
            throw ReticulumError("announce public key is not a valid key pair")
        }
        let signed = announce.destinationHash + announce.publicKey + announce.nameHash + announce.randomHash + announce.ratchet + announce.appData
        guard identity.verify(signature: announce.signature, data: signed) else {
            throw ReticulumError("announce signature does not verify")
        }
        guard destinationHash(nameHash: announce.nameHash, identityHash: identity.hash) == announce.destinationHash else {
            throw ReticulumError("announce destination does not match its key")
        }
    }

    public static func isValidAnnounce(_ announce: ReticulumAnnounce) -> Bool {
        (try? validateAnnounce(announce)) != nil
    }

    public static func buildAnnounce(
        identity: ReticulumIdentity, name: String, appData: [UInt8], pathResponse: Bool, unixTime: Int64,
        randomPrefix: [UInt8] = []
    ) -> [UInt8] {
        guard identity.hasPrivateKey, validAspectName(name) else { return [] }
        let hashedName = nameHash(name)
        let destination = destinationHash(nameHash: hashedName, identityHash: identity.hash)
        var random = randomPrefix.count >= 5 ? Array(randomPrefix.prefix(5)) : Array(fullHash(randomBytes(32)).prefix(5))
        for shift in stride(from: 32, through: 0, by: -8) {
            random.append(UInt8((UInt64(bitPattern: unixTime) >> UInt64(shift)) & 0xff))
        }
        let signature = identity.sign(destination + identity.publicKey + hashedName + random + appData)
        guard !signature.isEmpty else { return [] }
        var packet = ReticulumPacket()
        packet.packetType = .announce
        packet.destinationType = .single
        packet.destinationHash = destination
        packet.context = pathResponse ? ReticulumContext.pathResponse : ReticulumContext.none
        packet.data = identity.publicKey + hashedName + random + signature + appData
        return packPacket(packet)
    }

    public static let pathRequestDestination: [UInt8] = destinationHash(name: "rnstransport.path.request")

    public static func buildPathRequest(destination: [UInt8], tag: [UInt8]) -> [UInt8] {
        guard destination.count == Reticulum.hashLength, !tag.isEmpty else { return [] }
        var packet = ReticulumPacket()
        packet.destinationType = .plain
        packet.packetType = .data
        packet.destinationHash = pathRequestDestination
        packet.data = destination + Array(tag.prefix(Reticulum.hashLength))
        return packPacket(packet)
    }

    public static func parsePathRequest(_ packet: ReticulumPacket) -> (destination: [UInt8], tag: [UInt8])? {
        guard packet.packetType == .data, packet.destinationType == .plain,
              packet.destinationHash == pathRequestDestination, packet.data.count >= Reticulum.hashLength else { return nil }
        let length = Reticulum.hashLength
        let destination = Array(packet.data[0..<length])
        let data = packet.data
        let tag: [UInt8]
        if data.count > length * 2 {
            tag = Array(data[(length * 2)..<min(data.count, length * 3)])
        } else if data.count > length {
            tag = Array(data[length..<min(data.count, length * 2)])
        } else {
            tag = []
        }
        return (destination, tag)
    }

    public static func decodeMsgPack(_ bytes: [UInt8]) -> ReticulumMsgPack? {
        guard !bytes.isEmpty else { return nil }
        var reader = ReticulumMsgPackReader(bytes)
        var ok = true
        let value = reader.read(0, &ok)
        return ok ? value : nil
    }

    public static let knownAspects: [ReticulumKnownAspect] = [
        ("lxmf.delivery", "LXMF messaging", "peer"),
        ("lxmf.propagation", "LXMF propagation node", "propagation"),
        ("lxmf.propagation.control", "Propagation node control", "propagation"),
        ("nomadnetwork.node", "Nomad Network node", "node"),
        ("rnstransport.discovery.interface", "Interface discovery", "discovery"),
        ("rnstransport.remote.management", "Transport management", "transport"),
        ("rnstransport.info.blackhole", "Blackhole list", "transport"),
        ("rnstransport.probe", "Transport probe", "transport"),
        ("lxst.telephony", "LXST voice calls", "peer"),
        ("call.audio", "Voice calls", "peer"),
        ("rncp.receive", "File transfer (rncp)", "service"),
        ("rnx.execute", "Remote command (rnx)", "service"),
        ("forkmesh.node", "ForkMesh device", "forkmesh"),
    ].map { ReticulumKnownAspect(name: $0.0, label: $0.1, kind: $0.2, nameHash: nameHash($0.0)) }

    public static func knownAspect(nameHash hash: [UInt8]) -> ReticulumKnownAspect? {
        knownAspects.first { $0.nameHash == hash }
    }

    static func isPrintable(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.properties.generalCategory {
        case .control, .format, .surrogate, .privateUse, .unassigned: return false
        default: return true
        }
    }

    static func isSpace(_ unit: UInt16) -> Bool {
        if (9...13).contains(unit) || unit == 0x20 || unit == 0x85 { return true }
        guard let scalar = Unicode.Scalar(unit) else { return false }
        switch scalar.properties.generalCategory {
        case .spaceSeparator, .lineSeparator, .paragraphSeparator: return true
        default: return false
        }
    }

    public static func cleanText(_ value: ReticulumMsgPack, limit: Int = 120) -> String {
        cleanText(value.text, limit: limit)
    }

    public static func cleanText(_ text: String, limit: Int = 120) -> String {
        var units: [UInt16] = []
        units.reserveCapacity(min(text.utf16.count, limit))
        for unit in text.utf16 where unit != 0 {
            if units.count >= limit { break }
            if UTF16.isLeadSurrogate(unit) || UTF16.isTrailSurrogate(unit) {
                units.append(0x20)
            } else if let scalar = Unicode.Scalar(unit), isPrintable(scalar) {
                units.append(unit)
            } else {
                units.append(0x20)
            }
        }
        var out: [UInt16] = []
        var pendingSpace = false
        for unit in units {
            if isSpace(unit) {
                pendingSpace = !out.isEmpty
                continue
            }
            if pendingSpace {
                out.append(0x20)
                pendingSpace = false
            }
            out.append(unit)
        }
        return String(decoding: out, as: UTF16.self)
    }

    public static func displayName(kind: String, appData: [UInt8]) -> String {
        guard let first = appData.first else { return "" }
        if kind == "propagation" {
            guard let decoded = decodeMsgPack(appData) else { return "" }
            let list = decoded.list
            guard list.count >= 7 else { return "" }
            return cleanText(list[6].dictionary["1"] ?? .none)
        }
        if kind == "discovery" { return "" }
        if (0x90...0x9f).contains(first) || first == 0xdc {
            guard let decoded = decodeMsgPack(appData), let head = decoded.list.first else { return "" }
            return cleanText(head)
        }
        let text = String(decoding: appData, as: UTF8.self)
        if text.unicodeScalars.contains("\u{FFFD}") { return "" }
        return cleanText(text)
    }

    public static func stampValue(material: [UInt8], stamp: [UInt8], expandRounds: Int) -> Int {
        var workblock: [UInt8] = []
        workblock.reserveCapacity(expandRounds * 256)
        for round in 0..<expandRounds {
            workblock += hkdf(length: 256, material: material, salt: fullHash(material + [UInt8(truncatingIfNeeded: round)]))
        }
        var value = 0
        for byte in fullHash(workblock + stamp) {
            if byte == 0 {
                value += 8
                continue
            }
            value += byte.leadingZeroBitCount
            break
        }
        return value
    }

    public static func parseDiscovery(_ appData: [UInt8]) throws -> ReticulumDiscoveredInterface {
        guard appData.count > Reticulum.stampSize + 1 else { throw ReticulumError("discovery data is too short") }
        var found = ReticulumDiscoveredInterface()
        found.encrypted = appData[0] & 0x02 != 0
        if found.encrypted { throw ReticulumError("discovery data is encrypted for its network") }
        let payload = Array(appData[1...])
        let packed = Array(payload.prefix(payload.count - Reticulum.stampSize))
        let stamp = Array(payload.suffix(Reticulum.stampSize))
        guard let decoded = decodeMsgPack(packed), case .map(let info) = decoded, !info.isEmpty else {
            throw ReticulumError("discovery data is not a msgpack map")
        }
        func value(_ key: Int) -> ReticulumMsgPack? { info[String(key)] }
        func present(_ key: Int) -> Bool { value(key).map { !$0.isNone } ?? false }
        found.type = cleanText(value(0x00) ?? .none, limit: 40)
        found.transport = value(0x01)?.boolValue ?? false
        found.reachableOn = cleanText(value(0x02) ?? .none, limit: 255)
        found.port = value(0x06)?.intValue ?? 0
        found.name = cleanText(value(0xFF) ?? .none, limit: 80)
        found.transportId = Array((value(0xFE)?.byteValue ?? []).prefix(Reticulum.hashLength))
        found.implementation = cleanText(value(0xFD) ?? .none, limit: 40)
        found.version = cleanText(value(0xFC) ?? .none, limit: 40)
        found.networkName = cleanText(value(0x07) ?? .none, limit: 80)
        found.publishesPassphrase = present(0x08)
        found.frequency = value(0x09)?.doubleValue ?? 0
        found.bandwidth = value(0x0A)?.doubleValue ?? 0
        found.modulation = cleanText(value(0x0D) ?? .none, limit: 40)
        if present(0x03) && present(0x04) {
            found.latitude = value(0x03)?.doubleValue ?? 0
            found.longitude = value(0x04)?.doubleValue ?? 0
            found.height = value(0x05)?.doubleValue ?? 0
            found.hasLocation = found.latitude.isFinite && found.longitude.isFinite && abs(found.latitude) <= 90 && abs(found.longitude) <= 180
        }
        if found.port < 0 || found.port > 65535 { found.port = 0 }
        found.stampValue = stampValue(material: fullHash(packed), stamp: stamp, expandRounds: Reticulum.discoveryExpandRounds)
        found.stampValid = found.stampValue >= Reticulum.discoveryStampValue
        return found
    }

    public static func hex(_ bytes: [UInt8]) -> String { CryptoBytes.hex(bytes) }

    public static func pretty(_ bytes: [UInt8]) -> String { "<\(hex(bytes))>" }

    public static func short(_ bytes: [UInt8]) -> String {
        let text = hex(bytes)
        return text.count > 12 ? "<\(text.prefix(6))…\(text.suffix(4))>" : pretty(bytes)
    }

    public static func parseHash(_ text: String) -> [UInt8] {
        let clean = text.trimmingCharacters(in: .whitespacesAndNewlines).filter { !"<>: ".contains($0) }
        guard clean.count == 32, clean.allSatisfy({ $0.isASCII && $0.isHexDigit }) else { return [] }
        return CryptoBytes.fromHex(clean.lowercased()) ?? []
    }

    public static func contextName(_ context: UInt8) -> String {
        switch context {
        case 0x00: return "none"
        case 0x01...0x07: return "resource"
        case 0x08: return "cache request"
        case 0x09: return "request"
        case 0x0A: return "response"
        case 0x0B: return "path response"
        case 0x0C, 0x0D: return "command"
        case 0x0E: return "channel"
        case 0xFA: return "keepalive"
        case 0xFB...0xFF: return "link"
        default: return String(format: "0x%02x", context)
        }
    }

    public static func packetTypeName(_ type: ReticulumPacketType) -> String {
        switch type {
        case .data: return "data"
        case .announce: return "announce"
        case .linkRequest: return "link request"
        case .proof: return "proof"
        }
    }

    public static func destinationTypeName(_ type: ReticulumDestinationType) -> String {
        switch type {
        case .single: return "single"
        case .group: return "group"
        case .plain: return "plain"
        case .link: return "link"
        }
    }
}
