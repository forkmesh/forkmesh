import FMApi
import FMBase
import Foundation

public enum ChatLookField: String, CaseIterable, Sendable {
    case background, name, photo
}

public struct ChatBackgroundPreset: Equatable, Sendable, Identifiable {
    public let id: String
    public let label: String
    public let colors: [UInt32]
    public let dark: Bool
}

public enum ChatBackgroundCatalog {
    public static let presets: [ChatBackgroundPreset] = [
        ChatBackgroundPreset(id: "sunset", label: "Sunset", colors: [0x3A6EA5, 0xF4A261, 0xE76F51], dark: true),
        ChatBackgroundPreset(id: "beach", label: "Beach", colors: [0x8EC5FC, 0xE0C3FC, 0xF6D365], dark: false),
        ChatBackgroundPreset(id: "ocean", label: "Ocean", colors: [0x0F2027, 0x203A43, 0x2C5364], dark: true),
        ChatBackgroundPreset(id: "forest", label: "Forest", colors: [0x134E5E, 0x71B280], dark: true),
        ChatBackgroundPreset(id: "aurora", label: "Aurora", colors: [0x0B1026, 0x1D976C, 0x93F9B9], dark: true),
        ChatBackgroundPreset(id: "lavender", label: "Lavender", colors: [0xE0C3FC, 0x8EC5FC], dark: false),
        ChatBackgroundPreset(id: "peach", label: "Peach", colors: [0xFFECD2, 0xFCB69F], dark: false),
        ChatBackgroundPreset(id: "mint", label: "Mint", colors: [0xD4FC79, 0x96E6A1], dark: false),
        ChatBackgroundPreset(id: "rose", label: "Rose", colors: [0xFBC2EB, 0xA6C1EE], dark: false),
        ChatBackgroundPreset(id: "midnight", label: "Midnight", colors: [0x0F0C29, 0x302B63, 0x24243E], dark: true),
        ChatBackgroundPreset(id: "ember", label: "Ember", colors: [0x200122, 0x6F0000], dark: true),
        ChatBackgroundPreset(id: "sky", label: "Sky", colors: [0x2193B0, 0x6DD5ED], dark: false),
    ]

    public static func preset(_ id: String) -> ChatBackgroundPreset? { presets.first { $0.id == id } }
}

public struct ChatLook: Equatable, Sendable {
    public var backgroundPreset = ""
    public var backgroundImage: Data?
    public var name = ""
    public var photo: Data?
    public var stamps: [String: ChatLookStamp] = [:]
    public var ownBackground = false
    public var ownBackgroundPreset = ""
    public var ownBackgroundImage: Data?

    public init() {}

    public var hasBackground: Bool { backgroundImage != nil || ChatBackgroundCatalog.preset(backgroundPreset) != nil }

    public var isEmpty: Bool { stamps.isEmpty && !ownBackground }

    public var shown: ChatLook {
        guard ownBackground else { return self }
        var look = self
        look.backgroundPreset = ownBackgroundImage == nil ? ownBackgroundPreset : ""
        look.backgroundImage = ownBackgroundImage
        return look
    }

    public func overriding(_ change: ChatLookChange?) -> ChatLook {
        var look = self
        guard let change, change.field == .background else {
            look.ownBackground = false
            look.ownBackgroundPreset = ""
            look.ownBackgroundImage = nil
            return look
        }
        look.ownBackground = true
        look.ownBackgroundImage = change.image.flatMap { $0.isEmpty || $0.count > ChatLookFrames.maxImageBytes ? nil : $0 }
        look.ownBackgroundPreset =
            look.ownBackgroundImage == nil && ChatBackgroundCatalog.preset(change.preset) != nil ? change.preset : ""
        return look
    }
}

public struct ChatLookStamp: Equatable, Sendable {
    public let ts: Int64
    public let id: String

    public init(ts: Int64, id: String) {
        self.ts = ts
        self.id = id
    }

    public func precedes(_ other: ChatLookStamp) -> Bool {
        if ts != other.ts { return ts < other.ts }
        return id.utf16.lexicographicallyPrecedes(other.id.utf16)
    }
}

public struct ChatLookEvent: Equatable, Sendable, Identifiable {
    public let id: String
    public let conversation: String
    public let field: ChatLookField
    public let actorName: String
    public let isSelf: Bool
    public let timestamp: Date
    public let cleared: Bool
    public let value: String

    public init(
        id: String, conversation: String, field: ChatLookField, actorName: String, isSelf: Bool, timestamp: Date,
        cleared: Bool, value: String = ""
    ) {
        self.id = id
        self.conversation = conversation
        self.field = field
        self.actorName = actorName
        self.isSelf = isSelf
        self.timestamp = timestamp
        self.cleared = cleared
        self.value = value
    }

    public var text: String {
        let who = isSelf ? "You" : (actorName.isEmpty ? "Someone" : actorName)
        switch field {
        case .background:
            return cleared ? "\(who) removed the background." : "\(who) changed the background."
        case .name:
            return cleared ? "\(who) removed the name from the conversation." : "\(who) named the conversation \u{201C}\(value)\u{201D}."
        case .photo:
            return cleared ? "\(who) removed the group photo." : "\(who) changed the group photo."
        }
    }
}

public struct ChatLookChange: Equatable, Sendable {
    public let field: ChatLookField
    public let preset: String
    public let image: Data?
    public let name: String

    public init(field: ChatLookField, preset: String = "", image: Data? = nil, name: String = "") {
        self.field = field
        self.preset = preset
        self.image = image
        self.name = name
    }

    public var cleared: Bool {
        switch field {
        case .background: return image == nil && ChatBackgroundCatalog.preset(preset) == nil
        case .name: return name.isEmpty
        case .photo: return image == nil
        }
    }
}

public enum ChatLookFrames {
    public static let type = "look"
    public static let maxImageBytes = 600 * 1024
    public static let maxNameChars = 64
    public static let maxEventsPerConversation = 40

    public static func fields(_ change: ChatLookChange) -> JSONObject {
        var out: JSONObject = ["field": .string(change.field.rawValue)]
        switch change.field {
        case .background:
            if let image = change.image, !image.isEmpty, image.count <= maxImageBytes {
                out["image"] = .string(image.base64EncodedString())
            } else if ChatBackgroundCatalog.preset(change.preset) != nil {
                out["preset"] = .string(change.preset)
            }
        case .name:
            out["name"] = .string(cleanName(change.name))
        case .photo:
            if let image = change.image, !image.isEmpty, image.count <= maxImageBytes {
                out["image"] = .string(image.base64EncodedString())
            }
        }
        return out
    }

    public static func change(from frame: JSONObject) -> ChatLookChange? {
        guard (frame["type"]?.text ?? "") == type, let field = ChatLookField(rawValue: frame["field"]?.text ?? "") else {
            return nil
        }
        var image: Data?
        let encoded = frame["image"]?.text ?? ""
        if !encoded.isEmpty {
            if encoded.count > (maxImageBytes / 3 + 1) * 4 { return nil }
            guard let data = Data(base64Encoded: encoded), !data.isEmpty, data.count <= maxImageBytes else { return nil }
            image = data
        }
        switch field {
        case .background:
            let preset = frame["preset"]?.text ?? ""
            return ChatLookChange(field: field, preset: ChatBackgroundCatalog.preset(preset) == nil ? "" : preset, image: image)
        case .name:
            return ChatLookChange(field: field, name: cleanName(frame["name"]?.text ?? ""))
        case .photo:
            return ChatLookChange(field: field, image: image)
        }
    }

    public static func cleanName(_ raw: String) -> String {
        let collapsed = raw.split(whereSeparator: { $0.isWhitespace || $0.isNewline }).joined(separator: " ")
        return String(collapsed.prefix(maxNameChars))
    }

    public static func applying(_ change: ChatLookChange, to look: ChatLook, stamp: ChatLookStamp) -> ChatLook? {
        if let current = look.stamps[change.field.rawValue], !current.precedes(stamp) { return nil }
        var next = look
        switch change.field {
        case .background:
            next.backgroundImage = change.image
            next.backgroundPreset = change.image == nil ? change.preset : ""
        case .name:
            next.name = change.name
        case .photo:
            next.photo = change.image
        }
        next.stamps[change.field.rawValue] = stamp
        return next
    }

    static func encode(_ look: ChatLook) -> JSONValue {
        var object: JSONObject = [:]
        if !look.backgroundPreset.isEmpty { object["bp"] = .string(look.backgroundPreset) }
        if let image = look.backgroundImage { object["bi"] = .string(image.base64EncodedString()) }
        if !look.name.isEmpty { object["n"] = .string(look.name) }
        if let photo = look.photo { object["p"] = .string(photo.base64EncodedString()) }
        var stamps: JSONObject = [:]
        for (field, stamp) in look.stamps { stamps[field] = .object(["ts": .int(stamp.ts), "id": .string(stamp.id)]) }
        object["st"] = .object(stamps)
        if look.ownBackground {
            object["ob"] = true
            if !look.ownBackgroundPreset.isEmpty { object["obp"] = .string(look.ownBackgroundPreset) }
            if let image = look.ownBackgroundImage { object["obi"] = .string(image.base64EncodedString()) }
        }
        return .object(object)
    }

    static func decode(_ object: JSONObject) -> ChatLook {
        var look = ChatLook()
        let preset = object["bp"]?.text ?? ""
        if ChatBackgroundCatalog.preset(preset) != nil { look.backgroundPreset = preset }
        look.backgroundImage = image(object["bi"])
        look.name = cleanName(object["n"]?.text ?? "")
        look.photo = image(object["p"])
        for (field, value) in object["st"]?.objectOrEmpty ?? [:] where ChatLookField(rawValue: field) != nil {
            let stamp = value.objectOrEmpty
            look.stamps[field] = ChatLookStamp(ts: stamp["ts"]?.int64Value ?? 0, id: stamp["id"]?.text ?? "")
        }
        if object["ob"]?.isTrue == true {
            look.ownBackground = true
            let own = object["obp"]?.text ?? ""
            if ChatBackgroundCatalog.preset(own) != nil { look.ownBackgroundPreset = own }
            look.ownBackgroundImage = image(object["obi"])
        }
        return look
    }

    static func encodeEvent(_ event: ChatLookEvent) -> JSONValue {
        var object: JSONObject = [
            "id": .string(event.id), "c": .string(event.conversation), "f": .string(event.field.rawValue),
            "a": .string(event.actorName), "ts": .int(event.timestamp.epochMillis),
        ]
        if event.isSelf { object["self"] = true }
        if event.cleared { object["x"] = true }
        if !event.value.isEmpty { object["v"] = .string(event.value) }
        return .object(object)
    }

    static func decodeEvent(_ object: JSONObject) -> ChatLookEvent? {
        let id = object["id"]?.text ?? ""
        let conversation = object["c"]?.text ?? ""
        guard !id.isEmpty, !conversation.isEmpty, let field = ChatLookField(rawValue: object["f"]?.text ?? "") else {
            return nil
        }
        return ChatLookEvent(
            id: id, conversation: conversation, field: field,
            actorName: String((object["a"]?.text ?? "").prefix(ChatLimits.maxDisplayNameChars)),
            isSelf: object["self"]?.isTrue == true, timestamp: Date(epochMillis: object["ts"]?.int64Value ?? 0),
            cleared: object["x"]?.isTrue == true, value: cleanName(object["v"]?.text ?? ""))
    }

    private static func image(_ value: JSONValue?) -> Data? {
        let encoded = value?.text ?? ""
        guard !encoded.isEmpty, let data = Data(base64Encoded: encoded), !data.isEmpty, data.count <= maxImageBytes else {
            return nil
        }
        return data
    }
}
