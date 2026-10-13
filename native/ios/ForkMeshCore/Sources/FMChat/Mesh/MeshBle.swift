import Foundation

public let meshBleServiceUuid = "7F9E5C14-4D3B-4A21-9C6F-2B7AD1E05F11"
public let meshBleInboxUuid = "7F9E5C15-4D3B-4A21-9C6F-2B7AD1E05F11"
public let meshBleOutboxUuid = "7F9E5C16-4D3B-4A21-9C6F-2B7AD1E05F11"

public enum MeshBle {
    public static let maxWriteBytes = 512
    public static let minChunkBytes = 20
    public static let namePrefix = "FM-"
    public static let quietAfterDuplicateSeconds: TimeInterval = 60
    public static let rescanAfterDropSeconds: TimeInterval = 5
    public static let rescanSeconds: TimeInterval = 120
    public static let openTimeoutSeconds: TimeInterval = 20

    public static func newToken() -> String {
        String(format: "%08x", UInt32.random(in: 0...UInt32.max))
    }

    public static func advertisedName(token: String) -> String { namePrefix + token }

    public static func token(fromAdvertisedName name: String?) -> String? {
        guard let name, name.hasPrefix(namePrefix) else { return nil }
        let token = String(name.dropFirst(namePrefix.count))
        return token.isEmpty ? nil : token
    }

    public static func shouldDial(ownToken: String, peerName: String?) -> Bool {
        guard let theirs = token(fromAdvertisedName: peerName) else { return true }
        return !ownToken.utf8.lexicographicallyPrecedes(theirs.utf8)
    }

    public static func chunkBytes(_ reported: Int) -> Int {
        min(max(reported, minChunkBytes), maxWriteBytes)
    }

    public static func joinWrites(_ parts: [(offset: Int, value: Data)]) -> Data? {
        var joined = Data()
        for part in parts {
            if part.offset != joined.count { return nil }
            joined.append(part.value)
        }
        return joined
    }
}

public struct MeshBleOutbox {
    private var pending = Data()
    private var start = 0
    private var head: Data?

    public init() {}

    public var isEmpty: Bool { head == nil && start >= pending.count }

    public mutating func enqueue(_ bytes: Data) {
        if start > 0 && start >= pending.count / 2 {
            pending = Data(pending[(pending.startIndex + start)...])
            start = 0
        }
        pending.append(bytes)
    }

    public mutating func peek(max: Int) -> Data? {
        if let head { return head }
        let left = pending.count - start
        if left <= 0 { return nil }
        let size = Swift.min(Swift.max(max, 1), left)
        let from = pending.startIndex + start
        let chunk = Data(pending[from..<(from + size)])
        start += size
        if start >= pending.count {
            pending = Data()
            start = 0
        }
        head = chunk
        return chunk
    }

    public mutating func advance() {
        head = nil
    }

    public mutating func clear() {
        pending = Data()
        start = 0
        head = nil
    }
}
