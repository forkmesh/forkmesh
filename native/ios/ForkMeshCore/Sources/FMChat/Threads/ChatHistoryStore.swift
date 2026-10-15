import FMApi
import FMBase
import Foundation

public struct ChatHistorySnapshot: Equatable, Sendable {
    public var messages: [ChatMessage] = []
    public var replies: [ChatMessage] = []
    public var replyRoots: [String: String] = [:]
    public var reactions: [String: [String: [String: String]]] = [:]
    public var reactionOrder: [String: [String]] = [:]
    public var edits: [String: String] = [:]
    public var privateChannels: [String] = []
    public var hiddenChannels: [String] = []
    public var hiddenMessages: [String] = []
    public var removedMessages: [String] = []
    public var looks: [String: ChatLook] = [:]
    public var lookEvents: [ChatLookEvent] = []
    public var receipts: [String: ChatReceipt] = [:]

    public init() {}
}

public enum ChatHistoryCodec {
    public static let version = 1

    public static func bounded(
        _ conversations: [String: [ChatMessage]], retention: ChatRetention? = nil, now: Date = Date()
    ) -> [ChatMessage] {
        var out: [ChatMessage] = []
        for key in conversations.keys.sorted() {
            if key == "#team" || key == meshConversation
                || !(key.hasPrefix("#") || key.hasPrefix("@") || DirectMessageFrames.isDirect(key))
            {
                continue
            }
            var list = conversations[key] ?? []
            if let retention { list = list.filter { !retention.isExpired($0.timestamp, key: key, now: now) } }
            var kept: [ChatMessage] = []
            var chars = 0
            for message in list.reversed() {
                if kept.count >= ChatLimits.historyEntriesPerChannel { break }
                let cost = message.text.count + 64
                if chars + cost > ChatLimits.historyCharsPerChannel && !kept.isEmpty { break }
                chars += cost
                kept.append(message)
            }
            out.append(contentsOf: kept.reversed())
        }
        return out
    }

    static func encodeMessage(_ message: ChatMessage) -> JSONValue {
        var object: JSONObject = [
            "id": .string(message.id), "c": .string(message.conversation), "sid": .string(message.senderId),
            "s": .string(message.senderName), "t": .string(message.text), "ts": .int(message.timestamp.epochMillis),
        ]
        if message.isSelf { object["self"] = true }
        if !message.fileName.isEmpty { object["f"] = .string(message.fileName) }
        if let quote = message.replyTo { object["r"] = DirectMessageFrames.replyFields(quote) }
        return .object(object)
    }

    static func decodeMessage(_ object: JSONObject) -> ChatMessage? {
        let id = object["id"]?.text ?? ""
        let conversation = object["c"]?.text ?? ""
        if id.isEmpty || conversation.isEmpty { return nil }
        return ChatMessage(
            id: id, conversation: conversation, senderId: object["sid"]?.text ?? "",
            senderName: String((object["s"]?.text ?? "").prefix(ChatLimits.maxDisplayNameChars)),
            text: String((object["t"]?.text ?? "").prefix(ChatLimits.maxTextChars)),
            timestamp: Date(epochMillis: object["ts"]?.int64Value ?? 0), isSelf: object["self"]?.isTrue == true,
            fileName: String((object["f"]?.text ?? "").prefix(ChatLimits.maxFileNameChars)),
            replyTo: DirectMessageFrames.replyQuote(object["r"]))
    }

    public static func encode(_ snapshot: ChatHistorySnapshot) -> Data {
        var reactions: JSONObject = [:]
        for (id, state) in snapshot.reactions {
            var perValue: JSONObject = [:]
            for (value, reactors) in state {
                var names: JSONObject = [:]
                for (reactor, name) in reactors { names[reactor] = .string(name) }
                perValue[value] = .object(names)
            }
            reactions[id] = .object(perValue)
        }
        var order: JSONObject = [:]
        for (id, values) in snapshot.reactionOrder { order[id] = JSONValue(values) }
        var edits: JSONObject = [:]
        for (id, text) in snapshot.edits { edits[id] = .string(text) }
        var roots: JSONObject = [:]
        for (id, root) in snapshot.replyRoots { roots[id] = .string(root) }
        var looks: JSONObject = [:]
        for (conversation, look) in snapshot.looks { looks[conversation] = ChatLookFrames.encode(look) }
        var receipts: JSONObject = [:]
        for (id, receipt) in snapshot.receipts { receipts[id] = .string(receipt.rawValue) }
        let root: JSONObject = [
            "v": .int(Int64(version)),
            "messages": .array(snapshot.messages.map(encodeMessage)),
            "replies": .array(snapshot.replies.map(encodeMessage)),
            "roots": .object(roots),
            "reactions": .object(reactions),
            "order": .object(order),
            "edits": .object(edits),
            "private": JSONValue(snapshot.privateChannels),
            "hidden": JSONValue(snapshot.hiddenChannels),
            "hiddenMessages": JSONValue(snapshot.hiddenMessages),
            "removed": JSONValue(snapshot.removedMessages),
            "looks": .object(looks),
            "lookEvents": .array(snapshot.lookEvents.map(ChatLookFrames.encodeEvent)),
            "receipts": .object(receipts),
        ]
        return JSON.encodeData(.object(root))
    }

    public static func decode(_ data: Data) -> ChatHistorySnapshot? {
        let root = JSON.parseObject(data)
        guard root["v"]?.intValue == version else { return nil }
        var snapshot = ChatHistorySnapshot()
        snapshot.messages = (root["messages"]?.objects ?? []).compactMap(decodeMessage)
        snapshot.replies = (root["replies"]?.objects ?? []).compactMap(decodeMessage)
        for (id, value) in root["roots"]?.objectOrEmpty ?? [:] { snapshot.replyRoots[id] = value.text }
        for (id, value) in root["reactions"]?.objectOrEmpty ?? [:] {
            var state: [String: [String: String]] = [:]
            for (emoji, reactors) in value.objectOrEmpty {
                var names: [String: String] = [:]
                for (reactor, name) in reactors.objectOrEmpty { names[reactor] = name.text }
                if !names.isEmpty { state[emoji] = names }
            }
            if !state.isEmpty { snapshot.reactions[id] = state }
        }
        for (id, value) in root["order"]?.objectOrEmpty ?? [:] { snapshot.reactionOrder[id] = value.strings }
        for (id, value) in root["edits"]?.objectOrEmpty ?? [:] { snapshot.edits[id] = value.text }
        snapshot.privateChannels = root["private"]?.strings ?? []
        snapshot.hiddenChannels = root["hidden"]?.strings ?? []
        snapshot.hiddenMessages = root["hiddenMessages"]?.strings ?? []
        snapshot.removedMessages = root["removed"]?.strings ?? []
        for (conversation, value) in root["looks"]?.objectOrEmpty ?? [:] where !conversation.isEmpty {
            snapshot.looks[conversation] = ChatLookFrames.decode(value.objectOrEmpty)
        }
        snapshot.lookEvents = (root["lookEvents"]?.objects ?? []).compactMap(ChatLookFrames.decodeEvent)
        for (id, value) in root["receipts"]?.objectOrEmpty ?? [:] {
            if let receipt = ChatReceipt(rawValue: value.text) { snapshot.receipts[id] = receipt }
        }
        return snapshot
    }

    public static func scopeName(serverUrl: String, room: String) -> String {
        var hash: UInt64 = 0xcbf2_9ce4_8422_2325
        for byte in "\(serverUrl)|\(room)".utf8 {
            hash ^= UInt64(byte)
            hash = hash &* 0x0000_0100_0000_01b3
        }
        return String(hash, radix: 16)
    }
}

public struct ChatHistoryParts: Sendable {
    public var conversations: [String: [ChatMessage]] = [:]
    public var replies: [ChatMessage] = []
    public var replyRoots: [String: String] = [:]
    public var reactions: [String: [String: [String: String]]] = [:]
    public var reactionOrder: [String: [String]] = [:]
    public var edits: [String: String] = [:]
    public var privateChannels: Set<String> = []
    public var hiddenChannels: Set<String> = []
    public var hiddenMessages: [String] = []
    public var removedMessages: [String] = []
    public var looks: [String: ChatLook] = [:]
    public var lookEvents: [ChatLookEvent] = []
    public var receipts: [String: ChatReceipt] = [:]
    public var retention: ChatRetention?
    public var now = Date()

    public init() {}

    public var snapshot: ChatHistorySnapshot {
        var snapshot = ChatHistorySnapshot()
        snapshot.messages = ChatHistoryCodec.bounded(conversations, retention: retention, now: now)
        let kept = Set(snapshot.messages.map(\.id))
        snapshot.replies = replies.filter { !isExpired($0.conversation, $0.timestamp) }
        let replyIds = Set(snapshot.replies.map(\.id))
        snapshot.replyRoots = replyRoots.filter { replyIds.contains($0.key) }
        let known = kept.union(replyIds)
        snapshot.reactions = reactions.filter { known.contains($0.key) }
        snapshot.reactionOrder = reactionOrder.filter { known.contains($0.key) }
        snapshot.edits = edits.filter { known.contains($0.key) }
        snapshot.privateChannels = privateChannels.sorted()
        snapshot.hiddenChannels = hiddenChannels.sorted()
        snapshot.hiddenMessages = hiddenMessages
        snapshot.removedMessages = removedMessages
        snapshot.looks = looks
        snapshot.lookEvents = lookEvents.filter { !isExpired($0.conversation, $0.timestamp) }
        snapshot.receipts = receipts.filter { kept.contains($0.key) }
        return snapshot
    }

    private func isExpired(_ key: String, _ timestamp: Date) -> Bool {
        retention?.isExpired(timestamp, key: key, now: now) ?? false
    }
}

final class ChatHistoryWriter: @unchecked Sendable {
    private let queue = DispatchQueue(label: "com.forkmesh.chat-history", qos: .utility)
    private var written: [URL: ChatHistorySnapshot] = [:]
    private(set) var writes = 0

    func save(_ parts: ChatHistoryParts, to target: URL, in directory: URL) {
        queue.async { self.write(parts.snapshot, to: target, in: directory) }
    }

    func saveNow(_ snapshot: ChatHistorySnapshot, to target: URL, in directory: URL) {
        queue.sync { write(snapshot, to: target, in: directory) }
    }

    func remember(_ snapshot: ChatHistorySnapshot, at target: URL) {
        queue.async { self.written[target] = snapshot }
    }

    func erase(_ target: URL) {
        queue.sync {
            written[target] = nil
            try? FileManager.default.removeItem(at: target)
        }
    }

    func flush() -> Int { queue.sync { writes } }

    private func write(_ snapshot: ChatHistorySnapshot, to target: URL, in directory: URL) {
        if written[target] == snapshot { return }
        let data = ChatHistoryCodec.encode(snapshot)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        #if os(iOS)
        let options: Data.WritingOptions = [.atomic, .completeFileProtectionUntilFirstUserAuthentication]
        #else
        let options: Data.WritingOptions = [.atomic]
        #endif
        do {
            try data.write(to: target, options: options)
            written[target] = snapshot
            writes += 1
        } catch {
            written[target] = nil
        }
    }
}

@MainActor
public final class ChatHistoryStore {
    public let directory: URL
    private var saveTask: Task<Void, Never>?
    private let delay: TimeInterval
    private let writer = ChatHistoryWriter()

    public init(directory: URL, delay: TimeInterval = 2) {
        self.directory = directory
        self.delay = delay
    }

    public static func defaultDirectory() -> URL? {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first?
            .appendingPathComponent("chat-history", isDirectory: true)
    }

    public func file(scope: String) -> URL { directory.appendingPathComponent("relay-\(scope).json") }

    public func load(scope: String) -> ChatHistorySnapshot? {
        let target = file(scope: scope)
        guard let data = try? Data(contentsOf: target), let snapshot = ChatHistoryCodec.decode(data) else { return nil }
        writer.remember(snapshot, at: target)
        return snapshot
    }

    public func save(_ snapshot: ChatHistorySnapshot, scope: String) {
        writer.saveNow(snapshot, to: file(scope: scope), in: directory)
    }

    public func scheduleSave(scope: String, _ parts: @escaping @MainActor () -> ChatHistoryParts) {
        saveTask?.cancel()
        let delay = self.delay
        saveTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            guard let self, !Task.isCancelled else { return }
            self.writer.save(parts(), to: self.file(scope: scope), in: self.directory)
        }
    }

    @discardableResult
    public func flush() async -> Int {
        await saveTask?.value
        let writer = self.writer
        return await Task.detached { writer.flush() }.value
    }

    public func erase(scope: String) {
        saveTask?.cancel()
        writer.erase(file(scope: scope))
    }
}
