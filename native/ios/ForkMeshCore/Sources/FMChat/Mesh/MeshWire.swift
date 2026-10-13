// The wire half of the local mesh room: constants, the discovery beacon, and the 4-byte length
// framing both the mesh links and the local control channel use. Byte-compatible with the
// desktop's LocalMeshNode.

import FMBase
import Foundation

public let meshConversation = "#mesh"
public let meshRoomName = "forkmesh-lan-mesh-v1"
public let meshMulticastGroup = "239.192.79.42"
public let meshDiscoveryPort = 48732
public let meshBeaconTag = "forkmesh-lan-mesh"
public let meshProtocolVersion = 1

public let meshMaxFrameBytes = 24 * 1024 * 1024
let meshMaxTextChars = 16000
let meshMaxNameChars = 32
let meshWakeTokenMaxChars = 200
let meshWakeMaxTokens = 32
let meshWakeRefreshBeforeMs: Int64 = 24 * 60 * 60 * 1000

func meshWakeTokenExpiry(_ token: String) -> Int64? {
    if token.isEmpty || token.count > meshWakeTokenMaxChars { return nil }
    let parts = token.split(separator: ".", omittingEmptySubsequences: false)
    guard parts.count == 4, parts[0] == "mw1", let expires = Int64(parts[2]) else { return nil }
    return expires
}

func meshWakeTokenUsable(_ token: String, now: Int64) -> Bool {
    guard let expires = meshWakeTokenExpiry(token) else { return false }
    return expires > now
}

/// The advertisement a device multicasts so others can find it.
///
/// A `reply` beacon is the unicast answer to a beacon heard from a device this one is not linked
/// to. It reaches a sender whose own multicast never arrives here (an iPhone without Apple's
/// multicast entitlement, or a desktop whose firewall drops unsolicited datagrams but passes the
/// answer to one it sent), and is never itself answered, so two devices cannot ping-pong.
public struct MeshBeacon: Equatable, Sendable {
    public var nodeId: String
    public var port: Int
    public var name: String
    public var nodeName: String
    public var ownerUser: String
    public var accountKind: String
    public var platform: String
    public var version: String
    public var timestampMs: Int64
    public var reply: Bool
    /// The sender takes local control channels on its port: a desktop this phone can drive.
    public var control: Bool

    public init(
        nodeId: String, port: Int, name: String = "", nodeName: String = "", ownerUser: String = "",
        accountKind: String = "", platform: String = "", version: String = "", timestampMs: Int64 = 0,
        reply: Bool = false, control: Bool = false
    ) {
        self.nodeId = nodeId
        self.port = port
        self.name = name
        self.nodeName = nodeName
        self.ownerUser = ownerUser
        self.accountKind = accountKind
        self.platform = platform
        self.version = version
        self.timestampMs = timestampMs
        self.reply = reply
        self.control = control
    }

    public func toJSON() -> JSONObject {
        var object: JSONObject = [
            "t": .string(meshBeaconTag),
            "v": .int(Int64(meshProtocolVersion)),
            "id": .string(nodeId),
            "port": .int(Int64(port)),
            "ts": .int(timestampMs),
        ]
        if !name.isEmpty { object["name"] = .string(name) }
        if !nodeName.isEmpty { object["nodeName"] = .string(nodeName) }
        if !ownerUser.isEmpty { object["ownerUser"] = .string(ownerUser) }
        if !accountKind.isEmpty { object["accountKind"] = .string(accountKind) }
        if !platform.isEmpty { object["platform"] = .string(platform) }
        if !version.isEmpty { object["version"] = .string(version) }
        if reply { object["reply"] = true }
        if control { object["control"] = true }
        return object
    }

    /// nil for anything that is not a beacon of a version this client speaks.
    public static func parse(_ object: JSONObject) -> MeshBeacon? {
        guard object["t"] == .string(meshBeaconTag),
            let version = object["v"], version.numEquals(Int64(meshProtocolVersion))
        else { return nil }
        let id = dartString(object["id"]).dartTrimmed
        let portValue: Int
        if case .int(let n)? = object["port"] {
            portValue = Int(n)
        } else {
            portValue = Int(dartString(object["port"])) ?? 0
        }
        if id.isEmpty || portValue <= 0 || portValue > 65535 { return nil }
        func bounded(_ key: String, _ max: Int = meshMaxNameChars) -> String {
            dartPrefix(dartString(object[key]), max)
        }
        let ts: Int64
        switch object["ts"] {
        case .int(let n)?: ts = n
        case .double(let d)? where d.isFinite: ts = Int64(d)
        default: ts = 0
        }
        return MeshBeacon(
            nodeId: dartPrefix(id, 160),
            port: portValue,
            name: bounded("name"),
            nodeName: bounded("nodeName"),
            ownerUser: bounded("ownerUser"),
            accountKind: bounded("accountKind", 16),
            platform: bounded("platform", 16),
            version: bounded("version"),
            timestampMs: ts,
            reply: object["reply"] == .bool(true),
            control: object["control"] == .bool(true))
    }
}

/// A missing or null value is "", anything else its `text`.
func dartString(_ value: JSONValue?) -> String {
    guard let value, !value.isNull else { return "" }
    return value.text
}

/// The first `max` UTF-16 code units of `value`, or all of it when it is no longer than that.
func dartPrefix(_ value: String, _ max: Int) -> String {
    let units = Array(value.utf16)
    if units.count <= max { return value }
    return String(decoding: units[0..<max], as: UTF16.self)
}

/// 4-byte big-endian length, then that many bytes of payload.
public func meshFrameBytes<C: Collection>(_ payload: C) -> Data where C.Element == UInt8 {
    let length = payload.count
    var out = Data(capacity: length + 4)
    out.append(UInt8((length >> 24) & 0xff))
    out.append(UInt8((length >> 16) & 0xff))
    out.append(UInt8((length >> 8) & 0xff))
    out.append(UInt8(length & 0xff))
    out.append(contentsOf: payload)
    return out
}

/// A frame length that cannot be right: the caller must treat it as a protocol error and close
/// the link.
public struct MeshFrameError: Error, Equatable, Sendable {
    public let message: String
}

/// One complete frame taken off the front of `buffer`, or nil when it does not hold one yet.
/// Throws for a length outside 1...`maxBytes`.
public func meshTakeFrame(_ buffer: MeshByteBuffer, maxBytes: Int = meshMaxFrameBytes) throws -> Data? {
    if buffer.count < 4 { return nil }
    let head = buffer.peek(4)
    let length = (Int(head[0]) << 24) | (Int(head[1]) << 16) | (Int(head[2]) << 8) | Int(head[3])
    if length <= 0 || length > maxBytes { throw MeshFrameError(message: "mesh frame length out of range") }
    if buffer.count < length + 4 { return nil }
    buffer.discard(4)
    return buffer.take(length)
}

/// A growable byte buffer with cheap front removal (`BytesBuilderBuffer`).
public final class MeshByteBuffer {
    private var bytes: [UInt8] = []
    private var start = 0

    public init() {}

    public var count: Int { bytes.count - start }

    public func add<C: Collection>(_ data: C) where C.Element == UInt8 {
        if data.isEmpty { return }
        if start > 0 && start >= bytes.count / 2 {
            bytes.removeSubrange(0..<start)
            start = 0
        }
        bytes.append(contentsOf: data)
    }

    public func peek(_ n: Int) -> [UInt8] { Array(bytes[start..<(start + n)]) }

    public func discard(_ n: Int) {
        start += n
        if start >= bytes.count {
            bytes.removeAll(keepingCapacity: true)
            start = 0
        }
    }

    public func take(_ n: Int) -> Data {
        let out = Data(bytes[start..<(start + n)])
        discard(n)
        return out
    }
}
