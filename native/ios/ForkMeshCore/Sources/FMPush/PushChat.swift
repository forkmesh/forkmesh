import FMBase
import FMCrypto
import Foundation

public enum PushPreviewMode: String, CaseIterable, Equatable, Sendable {
    case nameAndMessage = "name-and-message"
    case nameOnly = "name-only"
    case hidden = "none"

    public static let preferenceKey = "push.notificationContent"

    public var title: String {
        switch self {
        case .nameAndMessage: return "Name and message"
        case .nameOnly: return "Name only"
        case .hidden: return "No name or message"
        }
    }

    public static func parse(_ raw: String?) -> PushPreviewMode {
        PushPreviewMode(rawValue: (raw ?? "").trimmingCharacters(in: .whitespaces)) ?? .nameAndMessage
    }
}

public enum PushChatKind: Equatable, Sendable {
    case direct(String)
    case channel(String)
}

public struct PushChatRequest: Equatable, Sendable {
    public let kind: PushChatKind
    public let ref: String
    public let conversation: String
    public let keyVersion: Int

    public static let directPrefix = "direct:"
    public static let directEndpoint = "/api/chat/direct-messages"
    public static let channelEndpoint = "/api/chat/channels"

    public static func isRef(_ value: String) -> Bool {
        value.count == 40 && value.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    public static func isId(_ value: String) -> Bool {
        value.count == 32 && value.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    public static func parse(_ payload: JSONObject) -> PushChatRequest? {
        let ref = payload["ref"]?.text ?? ""
        guard isRef(ref) else { return nil }
        let conversation = (payload["conversation"]?.text ?? "").trimmingCharacters(in: .whitespaces)
        let channelId = payload["channelId"]?.text ?? ""
        let keyVersion = max(0, payload["keyVersion"]?.int(or: 0) ?? 0)
        if conversation.hasPrefix(directPrefix) {
            let id = String(conversation.dropFirst(directPrefix.count))
            guard isId(id) else { return nil }
            return PushChatRequest(kind: .direct(id), ref: ref, conversation: conversation, keyVersion: keyVersion)
        }
        guard isId(channelId) else { return nil }
        return PushChatRequest(kind: .channel(channelId), ref: ref, conversation: conversation, keyVersion: keyVersion)
    }

    public var isDirect: Bool {
        if case .direct = kind { return true }
        return false
    }

    public var trustScope: String? {
        if case .direct = kind { return conversation }
        return nil
    }

    public var pendingPath: String {
        switch kind {
        case .direct(let id): return "\(PushChatRequest.directEndpoint)/\(id)/pending?ref=\(ref)"
        case .channel(let id): return "\(PushChatRequest.channelEndpoint)/\(id)/pending?ref=\(ref)"
        }
    }

    public var directRoom: String? {
        guard case .direct(let id) = kind, keyVersion > 0 else { return nil }
        return "chat-direct:\(id):v\(keyVersion)"
    }

    public var deliveredPath: String? {
        guard case .direct(let id) = kind else { return nil }
        return "\(PushChatRequest.directEndpoint)/\(id)/delivered?ref=\(ref)"
    }

    public var threadIdentifier: String {
        if !conversation.isEmpty { return conversation }
        if case .channel(let id) = kind { return "channel:\(id)" }
        return ""
    }
}

public struct PushChatMessage: Equatable, Sendable {
    public let id: String
    public let sender: String
    public let senderId: String
    public let text: String
    public let fileName: String
    public let channel: String
    public let isReply: Bool
    public var media: PushChatMedia = .none
    public var bot = ""
}

public final class PushVerifiedNames: @unchecked Sendable {
    public static let item = "verified-names"
    public static let maxNodes = 256
    public static let maxDirect = 200

    private let store: SharedItemStore
    private let lock = NSLock()

    public init(store: SharedItemStore) {
        self.store = store
    }

    public func remember(node: String, account: String) {
        let name = account.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if node.isEmpty || node.count > PushChatOpener.maxIdChars || name.count > PushChatOpener.maxNameChars { return }
        lock.lock()
        defer { lock.unlock() }
        var (nodes, direct) = read()
        let known = nodes.first { $0.0 == node }?.1
        if known == name || (known == nil && name.isEmpty) { return }
        nodes.removeAll { $0.0 == node }
        if !name.isEmpty { nodes.append((node, name)) }
        if nodes.count > PushVerifiedNames.maxNodes { nodes.removeFirst(nodes.count - PushVerifiedNames.maxNodes) }
        write(nodes, direct)
    }

    public func rememberDirect(_ peers: [(conversation: String, account: String)]) {
        var next: [String: String] = [:]
        for (conversation, account) in peers.suffix(PushVerifiedNames.maxDirect) {
            let name = account.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            if conversation.isEmpty || name.isEmpty || name.count > PushChatOpener.maxNameChars { continue }
            next[conversation] = name
        }
        lock.lock()
        defer { lock.unlock() }
        let (nodes, direct) = read()
        if direct == next { return }
        write(nodes, next)
    }

    public func account(node: String) -> String? {
        lock.lock()
        defer { lock.unlock() }
        return read().nodes.last { $0.0 == node }?.1
    }

    public func directPeer(_ conversation: String) -> String? {
        lock.lock()
        defer { lock.unlock() }
        return read().direct[conversation]
    }

    public func clear() {
        store.remove(PushVerifiedNames.item)
    }

    private func read() -> (nodes: [(String, String)], direct: [String: String]) {
        guard let data = store.data(PushVerifiedNames.item) else { return ([], [:]) }
        let root = JSON.parseObject(data)
        var nodes: [(String, String)] = []
        for pair in root["nodes"]?.arrayValue ?? [] {
            guard let entry = pair.arrayValue, entry.count == 2 else { continue }
            let node = entry[0].text
            let account = entry[1].text
            if !node.isEmpty && !account.isEmpty { nodes.append((node, account)) }
        }
        var direct: [String: String] = [:]
        for (conversation, value) in root["direct"]?.objectValue ?? [:] where !value.text.isEmpty {
            direct[conversation] = value.text
        }
        return (nodes, direct)
    }

    private func write(_ nodes: [(String, String)], _ direct: [String: String]) {
        let pairs: [JSONValue] = nodes.map { .array([.string($0.0), .string($0.1)]) }
        var map: JSONObject = [:]
        for (conversation, account) in direct { map[conversation] = .string(account) }
        store.set(JSON.encodeData(.object(["v": 1, "nodes": .array(pairs), "direct": .object(map)])), PushVerifiedNames.item)
    }
}

public enum PushChatMedia: Equatable, Sendable {
    case none, photo, voice, video, viewOnceVideo, viewOnceVoice, file

    public var label: String {
        switch self {
        case .none, .file: return ""
        case .photo: return "\u{1F4F7} Photo"
        case .voice: return "\u{1F3A4} Voice message"
        case .video: return "\u{1F3AC} Video"
        case .viewOnceVideo: return "\u{1F3AC} Video message"
        case .viewOnceVoice: return "\u{1F3A4} Voice message"
        }
    }

    public static func of(_ plain: JSONObject) -> PushChatMedia {
        if let once = plain["viewOnce"]?.objectValue {
            switch once["kind"]?.text ?? "" {
            case "video": return .viewOnceVideo
            case "voice": return .viewOnceVoice
            default: return .none
            }
        }
        let mime = (plain["fileMime"]?.text ?? "").lowercased()
        if mime.hasPrefix("image/") { return .photo }
        if mime.hasPrefix("audio/") { return .voice }
        if mime.hasPrefix("video/") { return .video }
        return (plain["fileName"]?.text ?? "").isEmpty ? .none : .file
    }
}

public struct PushPreview: Equatable, Sendable {
    public let title: String
    public let body: String
}

public final class PushRoomKeys: @unchecked Sendable {
    public static let item = "room-keys"
    public static let maxEntries = 32

    private let store: SharedItemStore
    private let lock = NSLock()

    public init(store: SharedItemStore) {
        self.store = store
    }

    public static func cacheId(room: String, passphrase: String) -> String {
        Sha256.hex(Array("forkmesh-push-room-key\n\(room)\n\(passphrase)".utf8))
    }

    public static func roomId(_ room: String) -> String {
        Sha256.hex(Array("forkmesh-push-room\n\(room)".utf8))
    }

    public func cached(room: String) -> RoomCrypto? {
        let id = PushRoomKeys.roomId(room)
        lock.lock()
        defer { lock.unlock() }
        guard let entry = entries().last(where: { $0.room == id }), let bytes = B64.decodeOrNil(entry.key), bytes.count == 32
        else { return nil }
        return RoomCrypto.withKey(Array(bytes))
    }

    public func cached(room: String, passphrase: String) -> [UInt8]? {
        let id = PushRoomKeys.cacheId(room: room, passphrase: passphrase)
        lock.lock()
        defer { lock.unlock() }
        guard let entry = entries().first(where: { $0.id == id }), let bytes = B64.decodeOrNil(entry.key), bytes.count == 32
        else { return nil }
        return Array(bytes)
    }

    public func crypto(room: String, passphrase: String) -> RoomCrypto? {
        if let key = cached(room: room, passphrase: passphrase) { return RoomCrypto.withKey(key) }
        let key = RoomCrypto.derivedKey(room, passphrase: passphrase)
        remember(key, room: room, passphrase: passphrase)
        return RoomCrypto.withKey(key)
    }

    public func remember(_ key: [UInt8], room: String, passphrase: String) {
        guard key.count == 32 else { return }
        let id = PushRoomKeys.cacheId(room: room, passphrase: passphrase)
        lock.lock()
        defer { lock.unlock() }
        let roomId = PushRoomKeys.roomId(room)
        var list = entries().filter { $0.id != id && $0.room != roomId }
        list.append((id: id, key: B64.encode(key), room: roomId))
        if list.count > PushRoomKeys.maxEntries { list.removeFirst(list.count - PushRoomKeys.maxEntries) }
        let values: [JSONValue] = list.map { entry in
            var object: JSONObject = ["id": .string(entry.id), "key": .string(entry.key)]
            if !entry.room.isEmpty { object["r"] = .string(entry.room) }
            return .object(object)
        }
        store.set(JSON.encodeData(.object(["v": 1, "entries": .array(values)])), PushRoomKeys.item)
    }

    public func clear() {
        store.remove(PushRoomKeys.item)
    }

    private func entries() -> [(id: String, key: String, room: String)] {
        guard let data = store.data(PushRoomKeys.item) else { return [] }
        let root = JSON.parseObject(data)
        return (root["entries"]?.objects ?? []).compactMap { entry in
            let id = entry["id"]?.text ?? ""
            let key = entry["key"]?.text ?? ""
            return id.isEmpty || key.isEmpty ? nil : (id: id, key: key, room: entry["r"]?.text ?? "")
        }
    }
}

public enum PushChatOpened: Equatable, Sendable {
    case message(PushChatMessage)
    case unknownSender
    case unavailable

    public var message: PushChatMessage? {
        if case .message(let message) = self { return message }
        return nil
    }
}

public enum PushChatOpener {
    public static let maxIdChars = 64
    public static let maxNameChars = 64
    public static let maxTextChars = 16_000

    public static func open(
        _ pending: JSONObject, request: PushChatRequest, secureStore: SecureStore?, keys: PushRoomKeys,
        now: Int64 = Date().epochMillis
    ) -> PushChatOpened {
        let room = (pending["room"]?.text ?? "").trimmingCharacters(in: .whitespaces)
        let passphrase = pending["passphrase"]?.text ?? ""
        guard !room.isEmpty, !passphrase.isEmpty else { return .unavailable }
        switch request.kind {
        case .direct:
            let named = pending["conversation"]?.text ?? ""
            if !named.isEmpty && named != request.conversation { return .unavailable }
        case .channel(let id):
            let named = pending["channel"]?.text ?? ""
            if !named.isEmpty && named != id { return .unavailable }
        }
        let envelope: JSONObject
        if let object = pending["frame"]?.objectValue {
            envelope = object
        } else {
            envelope = JSON.parseObject(pending["frame"]?.stringValue ?? "")
        }
        guard !envelope.isEmpty, let crypto = keys.crypto(room: room, passphrase: passphrase) else { return .unavailable }
        return open(envelope, room: room, crypto: crypto, request: request, secureStore: secureStore, now: now)
    }

    public static func openPushed(
        _ pushed: JSONObject, request: PushChatRequest, secureStore: SecureStore?, keys: PushRoomKeys,
        now: Int64 = Date().epochMillis
    ) -> PushChatOpened {
        guard let room = request.directRoom, let envelope = pushed["frame"]?.objectValue, !envelope.isEmpty,
            let crypto = keys.cached(room: room)
        else { return .unavailable }
        return open(envelope, room: room, crypto: crypto, request: request, secureStore: secureStore, now: now)
    }

    static func open(
        _ envelope: JSONObject, room: String, crypto: RoomCrypto, request: PushChatRequest, secureStore: SecureStore?,
        now: Int64
    ) -> PushChatOpened {
        guard let framed = crypto.decrypt(envelope) else { return .unavailable }
        let plain: JSONObject
        if RoomSeal.isSealedFrame(framed) {
            guard let secureStore else { return .unavailable }
            let overlay = OverlaySecureStore(base: secureStore)
            guard let keyring = try? ChatKeyring.load(overlay, create: false),
                let seal = try? RoomSeal(
                    room: room, scope: request.trustScope ?? room, policy: request.isDirect ? .strict : .open, acct: nil,
                    node: nil, keyring: keyring, store: overlay)
            else { return .unavailable }
            let result = seal.open(framed, now: now, commit: seal.version != ChatV4.version)
            switch result {
            case .opened:
                guard let opened = result.authenticatedPlain else { return .unavailable }
                plain = opened
            case .held:
                return .unknownSender
            default:
                return .unavailable
            }
        } else if request.isDirect || RoomSeal.isKeyFrame(framed) || envelope["srv"]?.boolValue != true {
            return .unavailable
        } else {
            plain = integration(framed)
        }
        guard let message = message(plain, request: request) else { return .unavailable }
        return .message(message)
    }

    static func integration(_ frame: JSONObject) -> JSONObject {
        var out = frame
        var name = ""
        for key in ["bot", "sender"] where name.isEmpty {
            name = String((frame[key]?.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines).prefix(64))
        }
        if name.isEmpty || name.hasPrefix("app:") { name = "Integration" }
        for key in ["ownerUser", "owner", "acct", "accountKind", "bot"] { out.removeValue(forKey: key) }
        out["sender"] = .string(name)
        out["senderId"] = .string("app:" + name)
        return out
    }

    public static func withPushedFrame(_ pending: JSONObject, pushed: JSONObject) -> JSONObject {
        if pending["frame"]?.objectValue?.isEmpty == false || pending["frame"]?.stringValue?.isEmpty == false {
            return pending
        }
        guard let frame = pushed["frame"]?.objectValue, !frame.isEmpty else { return pending }
        var merged = pending
        merged["frame"] = .object(frame)
        return merged
    }

    public static func message(_ plain: JSONObject, request: PushChatRequest) -> PushChatMessage? {
        let type = plain["type"]?.text ?? ""
        guard type == "chat" || type == "thread-reply" else { return nil }
        let id = plain["id"]?.text ?? ""
        guard !id.isEmpty, id.count <= maxIdChars else { return nil }
        let channel = plain["channel"]?.text ?? ""
        if request.isDirect {
            guard channel == request.conversation else { return nil }
        } else {
            guard channel.hasPrefix("#"), channel.count > 1 else { return nil }
        }
        let text = (plain["text"]?.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
        let fileName = String((plain["fileName"]?.text ?? "").trimmingCharacters(in: .whitespacesAndNewlines).prefix(200))
        let media = PushChatMedia.of(plain)
        if text.count > maxTextChars || (text.isEmpty && fileName.isEmpty && media == .none) { return nil }
        return PushChatMessage(
            id: id, sender: String((plain["sender"]?.text ?? "").trimmingCharacters(in: .whitespaces).prefix(maxNameChars)),
            senderId: String((plain["senderId"]?.text ?? "").prefix(maxIdChars)), text: text, fileName: fileName,
            channel: channel, isReply: type == "thread-reply", media: media, bot: botName(plain))
    }

    static func botName(_ plain: JSONObject) -> String {
        let bot = String((plain["bot"]?.stringValue ?? "").trimmingCharacters(in: .whitespacesAndNewlines).prefix(maxNameChars))
        if bot.isEmpty || bot.hasPrefix("app:") { return "" }
        if bot.count == 43, (try? B64.decodeUnpadded(bot))?.count == 32 { return "" }
        return bot
    }
}

public enum PushPreviewText {
    public static let hiddenTitle = "ForkMesh"
    public static let genericBody = "New message"
    public static let maxBodyChars = 1_000
    public static let unknownSender = PushPreview(title: genericBody, body: "")

    public static let botVia = " \u{00B7} via "

    public static func verifiedAccount(
        _ message: PushChatMessage, request: PushChatRequest, names: PushVerifiedNames?
    ) -> String? {
        if message.senderId.hasPrefix("app:") || message.senderId.isEmpty { return nil }
        if let account = names?.account(node: message.senderId), !account.isEmpty { return account }
        if request.isDirect, let peer = names?.directPeer(request.conversation), !peer.isEmpty { return peer }
        return nil
    }

    public static func verifiedSender(
        _ message: PushChatMessage, request: PushChatRequest, names: PushVerifiedNames?
    ) -> String? {
        if message.senderId.hasPrefix("app:") { return message.sender.isEmpty ? nil : message.sender }
        guard let account = verifiedAccount(message, request: request, names: names) else { return nil }
        return message.bot.isEmpty ? account : message.bot + botVia + account
    }

    public static func preview(
        _ message: PushChatMessage?, request: PushChatRequest, mode: PushPreviewMode, names: PushVerifiedNames? = nil
    ) -> PushPreview? {
        if mode == .hidden { return PushPreview(title: hiddenTitle, body: genericBody) }
        guard let message else { return nil }
        let sender = verifiedSender(message, request: request, names: names)
        let title: String
        if request.isDirect {
            title = sender ?? genericBody
        } else {
            guard !message.channel.isEmpty else { return nil }
            title = sender.map { "\($0) in \(message.channel)" } ?? message.channel
        }
        if mode == .nameOnly { return PushPreview(title: title, body: genericBody) }
        return PushPreview(title: title, body: body(message))
    }

    public static func preview(
        _ opened: PushChatOpened, request: PushChatRequest, mode: PushPreviewMode, names: PushVerifiedNames? = nil
    ) -> PushPreview? {
        if mode == .hidden { return PushPreview(title: hiddenTitle, body: genericBody) }
        switch opened {
        case .message(let message): return preview(message, request: request, mode: mode, names: names)
        case .unknownSender: return unknownSender
        case .unavailable: return nil
        }
    }

    public static func body(_ message: PushChatMessage) -> String {
        let attached = message.media.label.isEmpty ? "\u{1F4CE} \(message.fileName)" : message.media.label
        let text = message.text.isEmpty ? attached : message.text
        if text.count <= maxBodyChars { return text }
        return String(text.prefix(maxBodyChars - 1)) + "\u{2026}"
    }
}

public enum PushMeshWake {
    public static let conversation = "#mesh"
    public static let identifier = "mesh-room"
    public static let localKey = "meshLocal"

    public static func isWake(_ info: [AnyHashable: Any]) -> Bool {
        info["topic"] as? String == "chat" && info["conversation"] as? String == conversation && info[localKey] == nil
    }

    public static func isLocalAlert(identifier: String, info: [AnyHashable: Any]) -> Bool {
        identifier == Self.identifier && info[localKey] != nil
    }
}
