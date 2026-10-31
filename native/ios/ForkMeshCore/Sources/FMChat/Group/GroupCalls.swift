import FMBase
import Foundation

public enum GroupCallFrames {
    public static let presenceType = "group-call-presence"
    public static let keyType = "group-call-key"
    public static let verifyType = "group-call-verify"
    public static let types: Set<String> = [presenceType, keyType, verifyType]
    public static let heartbeat: TimeInterval = 20
    public static let presenceTTL: TimeInterval = 60
    public static let rotateDelay: TimeInterval = 1
    public static let pingEvery: TimeInterval = 20
    public static let keyBytes = 16

    public static func validPeer(_ peer: String) -> Bool {
        peer.count == 32 && peer.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    public static func presence(peer: String, state: String, media: String = "") -> JSONObject {
        var fields: JSONObject = ["peer": .string(peer), "call": .string(state)]
        if !media.isEmpty { fields["media"] = .string(media) }
        return fields
    }

    public static func key(peer: String, keyId: Int, raw: [UInt8]) -> JSONObject {
        ["peer": .string(peer), "keyId": .int(Int64(keyId)), "key": .string(Data(raw).base64EncodedString())]
    }
}

public struct GroupCallTicket: Equatable, Sendable {
    public let ticket: String
    public let url: String
    public let peer: String
    public let room: String
    public let fmm: GroupCallFmmAccess?

    public init?(json: JSONObject) {
        let ticket = json["ticket"]?.text ?? ""
        let url = json["url"]?.text ?? ""
        let peer = json["peer"]?.text ?? ""
        guard !ticket.isEmpty, url.hasPrefix("wss://"), GroupCallFrames.validPeer(peer) else { return nil }
        self.ticket = ticket
        self.url = url
        self.peer = peer
        self.room = json["room"]?.text ?? ""
        self.fmm = GroupCallFmmAccess(json: json["fmm"])
    }
}

public struct GroupCallMember: Equatable, Sendable {
    public let peer: String
    public let name: String
}

public struct GroupCallKey: Equatable, Sendable {
    public let keyId: Int
    public let raw: [UInt8]
    public let name: String
}

public enum GroupCallFrameEvent: Equatable, Sendable {
    case joined(peer: String, isNew: Bool)
    case left(peer: String)
    case key(peer: String, key: GroupCallKey)
}

@MainActor
public final class GroupCallRoster {
    private var presence: [String: [String: (name: String, at: TimeInterval)]] = [:]
    private var owners: [String: [String: (name: String, at: TimeInterval)]] = [:]
    private var keyring: [String: [String: GroupCallKey]] = [:]
    private let now: () -> TimeInterval

    public init(now: @escaping () -> TimeInterval = { Date().timeIntervalSince1970 }) {
        self.now = now
    }

    public func handle(_ plain: JSONObject, conversation: String, selfPeer: String) -> GroupCallFrameEvent? {
        guard let type = plain["type"]?.text, GroupCallFrames.types.contains(type) else { return nil }
        let peer = plain["peer"]?.text ?? ""
        guard GroupCallFrames.validPeer(peer), peer != selfPeer else { return nil }
        let name = (plain["sender"]?.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard !name.isEmpty else { return nil }
        if type == GroupCallFrames.presenceType {
            guard claim(conversation, peer: peer, name: name) else { return nil }
            let out = plain["call"]?.text == "out"
            var entries = presence[conversation] ?? [:]
            let known = entries[peer] != nil
            if out {
                entries.removeValue(forKey: peer)
                owners[conversation]?.removeValue(forKey: peer)
                keyring[conversation]?.removeValue(forKey: peer)
            } else {
                entries[peer] = (name, now())
            }
            presence[conversation] = entries
            return out ? .left(peer: peer) : .joined(peer: peer, isNew: !known)
        }
        guard type == GroupCallFrames.keyType, let keyId = plain["keyId"]?.intValue, (0...255).contains(keyId),
            let text = plain["key"]?.text, let decoded = Data(base64Encoded: text),
            decoded.count == GroupCallFrames.keyBytes,
            claim(conversation, peer: peer, name: name)
        else { return nil }
        let key = GroupCallKey(keyId: keyId, raw: [UInt8](decoded), name: name)
        var ring = keyring[conversation] ?? [:]
        ring[peer] = key
        keyring[conversation] = ring
        return .key(peer: peer, key: key)
    }

    private func claim(_ conversation: String, peer: String, name: String) -> Bool {
        let at = now()
        if let owner = owners[conversation]?[peer], at - owner.at <= GroupCallFrames.presenceTTL, owner.name != name {
            return false
        }
        owners[conversation, default: [:]][peer] = (name, at)
        return true
    }

    public func roster(_ conversation: String) -> [GroupCallMember] {
        let cutoff = now() - GroupCallFrames.presenceTTL
        var entries = presence[conversation] ?? [:]
        entries = entries.filter { $0.value.at >= cutoff }
        presence[conversation] = entries
        return entries.map { GroupCallMember(peer: $0.key, name: $0.value.name) }.sorted { $0.name < $1.name }
    }

    public func name(_ conversation: String, peer: String) -> String {
        presence[conversation]?[peer]?.name ?? ""
    }

    public func keys(_ conversation: String) -> [String: GroupCallKey] {
        keyring[conversation] ?? [:]
    }
}

public struct GroupCallSendKey: Sendable {
    public private(set) var keyId: Int = 0
    public private(set) var raw: [UInt8]

    public init(random: () -> [UInt8] = GroupCallSendKey.randomKey) {
        raw = random()
    }

    public mutating func rotate(random: () -> [UInt8] = GroupCallSendKey.randomKey) {
        keyId = (keyId + 1) & 0xff
        raw = random()
    }

    public static func randomKey() -> [UInt8] {
        var generator = SystemRandomNumberGenerator()
        return (0..<GroupCallFrames.keyBytes).map { _ in UInt8.random(in: 0...255, using: &generator) }
    }
}
