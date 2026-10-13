import FMBase
import Foundation

public struct ChatRetentionPreset: Equatable, Hashable, Sendable, Identifiable {
    public let ms: Int64
    public let label: String

    public init(ms: Int64, label: String) {
        self.ms = ms
        self.label = label
    }

    public var id: Int64 { ms }
}

public struct ChatRetention: Equatable, Hashable, Sendable {
    public static let minMs: Int64 = 3_600_000
    public static let maxMs: Int64 = 604_800_000
    public static let standardMs: Int64 = maxMs
    public static let standard = ChatRetention()

    public static let presets: [ChatRetentionPreset] = [
        ChatRetentionPreset(ms: 604_800_000, label: "7 days"),
        ChatRetentionPreset(ms: 259_200_000, label: "3 days"),
        ChatRetentionPreset(ms: 86_400_000, label: "1 day"),
        ChatRetentionPreset(ms: 28_800_000, label: "8 hours"),
        ChatRetentionPreset(ms: 3_600_000, label: "1 hour"),
    ]

    public var defaultMs: Int64
    public var chats: [String: Int64]
    public var updatedAt: Int64

    public init(defaultMs: Int64 = ChatRetention.standardMs, chats: [String: Int64] = [:], updatedAt: Int64 = 0) {
        self.defaultMs = ChatRetention.clamp(defaultMs)
        var clamped: [String: Int64] = [:]
        for (key, value) in chats where !key.isEmpty { clamped[key] = ChatRetention.clamp(value) }
        self.chats = clamped
        self.updatedAt = max(0, updatedAt)
    }

    public init(json: JSONValue?) {
        guard let object = json?.objectValue else {
            self = .standard
            return
        }
        var chats: [String: Int64] = [:]
        for (key, value) in object["chats"]?.objectOrEmpty ?? [:] {
            if let ms = ChatRetention.millis(value), ms > 0, !key.isEmpty { chats[key] = ms }
        }
        let raw = ChatRetention.millis(object["defaultMs"]) ?? 0
        self.init(
            defaultMs: raw > 0 ? raw : ChatRetention.standardMs, chats: chats,
            updatedAt: ChatRetention.millis(object["updatedAt"]) ?? 0)
    }

    static func millis(_ value: JSONValue?) -> Int64? {
        guard let value, !value.isNull else { return nil }
        if let number = value.doubleValue, number.isFinite { return Int64(number.rounded()) }
        if let text = value.stringValue { return Int64(text.trimmingCharacters(in: .whitespaces)) }
        return nil
    }

    public func toJSON() -> JSONObject {
        var chatObject = JSONObject()
        for key in chats.keys.sorted() { chatObject[key] = .int(chats[key] ?? ChatRetention.standardMs) }
        return ["defaultMs": .int(defaultMs), "chats": .object(chatObject), "updatedAt": .int(updatedAt)]
    }

    public static func clamp(_ ms: Int64) -> Int64 { min(max(ms, minMs), maxMs) }

    public static func key(conversation: String, officeRoomId: String? = nil) -> String {
        if let id = officeRoomId?.trimmingCharacters(in: .whitespaces), !id.isEmpty { return "channel:\(id)" }
        return conversation
    }

    public func override(for key: String) -> Int64? { chats[key] }

    public func effectiveMs(for key: String) -> Int64 { ChatRetention.clamp(chats[key] ?? defaultMs) }

    public func cutoff(for key: String, now: Date) -> Date {
        Date(epochMillis: now.epochMillis - effectiveMs(for: key))
    }

    public func isExpired(_ timestamp: Date, key: String, now: Date) -> Bool {
        timestamp.epochMillis <= now.epochMillis - effectiveMs(for: key)
    }

    public func settingDefault(_ ms: Int64?) -> ChatRetention {
        ChatRetention(defaultMs: ms ?? ChatRetention.standardMs, chats: chats, updatedAt: updatedAt)
    }

    public func settingChat(_ key: String, _ ms: Int64?) -> ChatRetention {
        var next = chats
        next[key] = ms
        return ChatRetention(defaultMs: defaultMs, chats: next, updatedAt: updatedAt)
    }

    public static func label(_ ms: Int64) -> String {
        let value = clamp(ms)
        if let preset = presets.first(where: { $0.ms == value }) { return preset.label }
        let day: Int64 = 86_400_000
        if value % day == 0 { return unit(value / day, "day") }
        if value % minMs == 0 { return unit(value / minMs, "hour") }
        return unit(max(1, value / 60_000), "minute")
    }

    private static func unit(_ count: Int64, _ name: String) -> String {
        count == 1 ? "1 \(name)" : "\(count) \(name)s"
    }

    public var defaultLabel: String { ChatRetention.label(defaultMs) }

    public func useDefaultLabel() -> String { "Use default (\(defaultLabel))" }
}

public struct ChatRetentionError: Error, Equatable, Sendable, CustomStringConvertible {
    public let code: String
    public let message: String

    public init(_ code: String, _ message: String) {
        self.code = code
        self.message = message
    }

    public var description: String { message }
}

public func chatRetentionErrorMessage(_ code: String) -> String {
    switch code {
    case "invalid_chat_retention": return "That disappearing-message time is not valid."
    case "unauthorized": return "Your session expired. Please sign in again."
    case "unreachable": return "Could not reach ForkMesh. Check your connection and try again."
    default: return "Could not change when messages disappear. Please try again."
    }
}
