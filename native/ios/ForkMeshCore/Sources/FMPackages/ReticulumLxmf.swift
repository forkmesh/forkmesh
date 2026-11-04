import FMCrypto
import Foundation

public enum ReticulumLxmf {
    public static let overhead = 2 * Reticulum.hashLength + Reticulum.signatureLength + 8 + 8
    public static let encryptedPacketMdu = 383
    public static let linkMdu = 431
    public static let opportunisticMaxContent = encryptedPacketMdu - overhead + Reticulum.hashLength
    public static let linkPacketMaxContent = linkMdu - overhead
    public static let stampRounds = 3000
    public static let maxStampCost = 24
    public static let resourceMapHashLength = 4
    public static let resourceRandomLength = 4
    public static let resourceHashmapMaxLength = (linkMdu - 134) / resourceMapHashLength
    public static let resourceMaxSize = 1024 * 1024 - 1
    public static let deliveryAspect = "lxmf.delivery"
}

public struct ReticulumAttachment: Equatable, Sendable {
    public var name: String
    public var data: [UInt8]

    public init(name: String, data: [UInt8]) {
        self.name = name
        self.data = data
    }
}

public struct ReticulumLxmfMessage: Equatable, Sendable {
    public var destinationHash: [UInt8] = []
    public var sourceHash: [UInt8] = []
    public var signature: [UInt8] = []
    public var hash: [UInt8] = []
    public var stamp: [UInt8] = []
    public var signedPayload: [UInt8] = []
    public var packed: [UInt8] = []
    public var timestamp: Double = 0
    public var title = ""
    public var content = ""
    public var attachments: [ReticulumAttachment] = []
    public var contentSize = 0
}

public final class ReticulumCancel: @unchecked Sendable {
    private let lock = NSLock()
    private var flag = false

    public init() {}

    public var cancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return flag
    }

    public func cancel() {
        lock.lock()
        flag = true
        lock.unlock()
    }
}

public struct ReticulumResourceAdvertisement: Equatable, Sendable {
    public var transferSize: Int64 = 0
    public var dataSize: Int64 = 0
    public var parts = 0
    public var hash: [UInt8] = []
    public var randomHash: [UInt8] = []
    public var originalHash: [UInt8] = []
    public var segmentIndex = 1
    public var segments = 1
    public var requestId: [UInt8] = []
    public var flags = 0
    public var hashmap: [UInt8] = []

    public init() {}

    public var encrypted: Bool { flags & 0x01 != 0 }
    public var compressed: Bool { flags & 0x02 != 0 }
    public var split: Bool { flags & 0x04 != 0 }
    public var isRequest: Bool { flags & 0x08 != 0 }
    public var isResponse: Bool { flags & 0x10 != 0 }
    public var hasMetadata: Bool { flags & 0x20 != 0 }
}

extension ReticulumLxmf {
    static func leftUTF16(_ text: String, _ count: Int) -> String {
        text.utf16.count <= count ? text : String(decoding: Array(text.utf16.prefix(count)), as: UTF16.self)
    }

    public static func payload(timestamp: Double, title: String, content: String) -> [UInt8] {
        ReticulumMsgPackWriter().array(4).real(timestamp).bin(Array(title.utf8)).bin(Array(content.utf8)).map(0).bytes
    }

    public static func messageId(destination: [UInt8], source: [UInt8], payload: [UInt8]) -> [UInt8] {
        ReticulumWire.fullHash(destination + source + payload)
    }

    public static func pack(source: ReticulumIdentity, sourceHash: [UInt8], destinationHash: [UInt8], timestamp: Double,
                            title: String, content: String, stamp: [UInt8] = []) -> ReticulumLxmfMessage? {
        guard source.hasPrivateKey, sourceHash.count == Reticulum.hashLength, destinationHash.count == Reticulum.hashLength else { return nil }
        let payload = payload(timestamp: timestamp, title: title, content: content)
        let hash = messageId(destination: destinationHash, source: sourceHash, payload: payload)
        let signature = source.sign(destinationHash + sourceHash + payload + hash)
        guard !signature.isEmpty else { return nil }
        var packedPayload = payload
        if !stamp.isEmpty {
            packedPayload[0] = 0x95
            packedPayload += ReticulumMsgPackWriter().bin(stamp).bytes
        }
        var message = ReticulumLxmfMessage()
        message.destinationHash = destinationHash
        message.sourceHash = sourceHash
        message.signature = signature
        message.hash = hash
        message.stamp = stamp
        message.signedPayload = payload
        message.packed = destinationHash + sourceHash + signature + packedPayload
        message.timestamp = timestamp
        message.title = title
        message.content = content
        message.contentSize = packedPayload.count - 16
        return message
    }

    static func text(_ value: ReticulumMsgPack?) -> String {
        guard let value else { return "" }
        if case .bytes(let raw) = value { return String(decoding: raw, as: UTF8.self) }
        return value.text
    }

    public static func unpack(_ packed: [UInt8]) throws -> ReticulumLxmfMessage {
        let header = 2 * Reticulum.hashLength + Reticulum.signatureLength
        guard packed.count > header else { throw ReticulumError("message is too short") }
        var message = ReticulumLxmfMessage()
        message.destinationHash = Array(packed[0..<16])
        message.sourceHash = Array(packed[16..<32])
        message.signature = Array(packed[32..<header])
        message.packed = packed
        let payload = Array(packed[header...])
        guard let items = ReticulumWire.msgPackArrayItems(payload), items.count >= 4 else {
            throw ReticulumError("message payload is not an LXMF array")
        }
        func item(_ index: Int) -> [UInt8] { Array(payload[items[index]]) }
        message.signedPayload = [0x94] + Array(payload[items[0].lowerBound..<items[3].upperBound])
        if items.count >= 5 { message.stamp = Array((ReticulumWire.decodeMsgPack(item(4))?.byteValue ?? []).prefix(64)) }
        message.hash = messageId(destination: message.destinationHash, source: message.sourceHash, payload: message.signedPayload)
        guard let timestamp = ReticulumWire.decodeMsgPack(item(0)) else { throw ReticulumError("message timestamp is unreadable") }
        message.timestamp = timestamp.doubleValue
        message.title = leftUTF16(text(ReticulumWire.decodeMsgPack(item(1))), 1024)
        message.content = text(ReticulumWire.decodeMsgPack(item(2)))
        let fields = ReticulumWire.decodeMsgPack(item(3))?.dictionary ?? [:]
        for entry in fields["5"]?.list ?? [] {
            let pair = entry.list
            if pair.count >= 2 { message.attachments.append(ReticulumAttachment(name: leftUTF16(text(pair[0]), 255), data: pair[1].byteValue)) }
        }
        message.contentSize = payload.count - 16
        return message
    }

    public static func verify(_ message: ReticulumLxmfMessage, sourcePublicKey: [UInt8]) -> Bool {
        guard sourcePublicKey.count == Reticulum.publicKeyLength else { return false }
        return ReticulumCipher.verify(Array(sourcePublicKey[32..<64]), signature: message.signature,
                                      data: message.destinationHash + message.sourceHash + message.signedPayload + message.hash)
    }

    public static func announceData(displayName: String, stampCost: Int = 0) -> [UInt8] {
        let writer = ReticulumMsgPackWriter().array(3)
        if displayName.isEmpty { writer.null() } else { writer.bin(Array(displayName.utf8)) }
        if stampCost > 0 && stampCost < 255 { writer.integer(Int64(stampCost)) } else { writer.null() }
        return writer.array(0).bytes
    }

    public static func stampCost(appData: [UInt8]) -> Int {
        guard let first = appData.first, (0x90...0x9f).contains(first) || first == 0xdc,
              let list = ReticulumWire.decodeMsgPack(appData)?.list, list.count >= 2, !list[1].isNone else { return 0 }
        let cost = list[1].intValue
        return cost > 0 && cost < 255 ? cost : 0
    }

    public static func workblock(_ material: [UInt8], rounds: Int) -> [UInt8] {
        var block: [UInt8] = []
        block.reserveCapacity(rounds * 256)
        for round in 0..<rounds {
            block += ReticulumWire.hkdf(length: 256, material: material,
                                        salt: ReticulumWire.fullHash(material + ReticulumMsgPackWriter().integer(Int64(round)).bytes))
        }
        return block
    }

    static func leadingZeroBits(_ digest: [UInt8]) -> Int {
        var value = 0
        for byte in digest {
            if byte == 0 {
                value += 8
                continue
            }
            value += byte.leadingZeroBitCount
            break
        }
        return value
    }

    public static func stampValue(workblock: [UInt8], stamp: [UInt8]) -> Int {
        leadingZeroBits(ReticulumWire.fullHash(workblock + stamp))
    }

    public static func generateStamp(messageId: [UInt8], cost: Int, cancel: ReticulumCancel? = nil) -> (stamp: [UInt8], value: Int)? {
        guard cost > 0, cost <= maxStampCost else { return nil }
        let block = workblock(messageId, rounds: stampRounds)
        var base = ReticulumSha256()
        let whole = block.count - block.count % 64
        base.absorbBlocks(Array(block[0..<whole]))
        let tail = Array(block[whole...])
        var attempt: UInt64 = 0
        while true {
            if attempt & 0xff == 0, cancel?.cancelled == true { return nil }
            attempt += 1
            let candidate = CryptoBytes.random(32)
            let bits = leadingZeroBits(base.finish(tail + candidate))
            if bits >= cost { return (candidate, bits) }
        }
    }

    public static func validateStamp(messageId: [UInt8], stamp: [UInt8], cost: Int) -> Bool {
        if cost <= 0 { return true }
        guard stamp.count == 32 else { return false }
        return stampValue(workblock: workblock(messageId, rounds: stampRounds), stamp: stamp) >= cost
    }

    public static func packAdvertisement(_ advertisement: ReticulumResourceAdvertisement, segment: Int = 0) -> [UInt8] {
        let start = segment * resourceHashmapMaxLength
        let end = min((segment + 1) * resourceHashmapMaxLength, advertisement.parts)
        let low = min(advertisement.hashmap.count, max(0, start * resourceMapHashLength))
        let high = min(advertisement.hashmap.count, max(low, end * resourceMapHashLength))
        let writer = ReticulumMsgPackWriter().map(11)
        writer.str("t").integer(advertisement.transferSize)
        writer.str("d").integer(advertisement.dataSize)
        writer.str("n").integer(Int64(advertisement.parts))
        writer.str("h").bin(advertisement.hash)
        writer.str("r").bin(advertisement.randomHash)
        writer.str("o").bin(advertisement.originalHash)
        writer.str("i").integer(Int64(advertisement.segmentIndex))
        writer.str("l").integer(Int64(advertisement.segments))
        writer.str("q")
        if advertisement.requestId.isEmpty { writer.null() } else { writer.bin(advertisement.requestId) }
        writer.str("f").integer(Int64(advertisement.flags))
        writer.str("m").bin(Array(advertisement.hashmap[low..<high]))
        return writer.bytes
    }

    public static func unpackAdvertisement(_ bytes: [UInt8]) -> ReticulumResourceAdvertisement? {
        guard let decoded = ReticulumWire.decodeMsgPack(bytes), case .map(let map) = decoded else { return nil }
        for key in ["t", "d", "n", "h", "r", "o", "i", "l", "f", "m"] where map[key] == nil { return nil }
        var advertisement = ReticulumResourceAdvertisement()
        advertisement.transferSize = map["t"]?.int64Value ?? 0
        advertisement.dataSize = map["d"]?.int64Value ?? 0
        advertisement.parts = map["n"]?.intValue ?? 0
        advertisement.hash = map["h"]?.byteValue ?? []
        advertisement.randomHash = map["r"]?.byteValue ?? []
        advertisement.originalHash = map["o"]?.byteValue ?? []
        advertisement.segmentIndex = map["i"]?.intValue ?? 0
        advertisement.segments = map["l"]?.intValue ?? 0
        advertisement.requestId = map["q"]?.byteValue ?? []
        advertisement.flags = map["f"]?.intValue ?? 0
        advertisement.hashmap = map["m"]?.byteValue ?? []
        guard advertisement.transferSize > 0, advertisement.transferSize <= 3 * Int64(resourceMaxSize),
              advertisement.parts > 0, advertisement.parts <= 100_000, advertisement.hash.count == Reticulum.fullHashLength,
              advertisement.randomHash.count == resourceRandomLength, advertisement.hashmap.count % resourceMapHashLength == 0 else { return nil }
        return advertisement
    }

    public static func mapHash(_ part: [UInt8], randomHash: [UInt8]) -> [UInt8] {
        Array(ReticulumWire.fullHash(part + randomHash).prefix(resourceMapHashLength))
    }
}
