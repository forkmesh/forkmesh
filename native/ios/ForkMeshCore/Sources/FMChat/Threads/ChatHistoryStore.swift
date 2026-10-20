import FMApi
import FMBase
import Foundation

public struct ChatHistorySnapshot: Equatable, Sendable {
    public var messages: [ChatMessage] = []
    public var replies: [ChatMessage] = []
    public var replyRoots: [String: String] = [:]
    public var reactions: [String: [String: [String: String]]] = [:]
    public var reactionOrder: [String: [String]] = [:]
    public var stickers: [String: [String: [String: String]]] = [:]
    public var edits: [String: String] = [:]
    public var privateChannels: [String] = []
    public var hiddenChannels: [String] = []
    public var hiddenMessages: [String] = []
    public var removedMessages: [String] = []
    public var looks: [String: ChatLook] = [:]
    public var lookEvents: [ChatLookEvent] = []
    public var receipts: [String: ChatReceipt] = [:]
    public var receiptTimes: [String: ChatReceiptTimes] = [:]

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

    static func encodeMarks(_ marks: [String: [String: [String: String]]]) -> JSONValue {
        var out: JSONObject = [:]
        for (id, state) in marks {
            var perValue: JSONObject = [:]
            for (value, reactors) in state {
                var names: JSONObject = [:]
                for (reactor, name) in reactors { names[reactor] = .string(name) }
                perValue[value] = .object(names)
            }
            out[id] = .object(perValue)
        }
        return .object(out)
    }

    static func decodeMarks(_ value: JSONValue?) -> [String: [String: [String: String]]] {
        var out: [String: [String: [String: String]]] = [:]
        for (id, value) in value?.objectOrEmpty ?? [:] {
            var state: [String: [String: String]] = [:]
            for (emoji, reactors) in value.objectOrEmpty {
                var names: [String: String] = [:]
                for (reactor, name) in reactors.objectOrEmpty { names[reactor] = name.text }
                if !names.isEmpty { state[emoji] = names }
            }
            if !state.isEmpty { out[id] = state }
        }
        return out
    }

    public static func encode(_ snapshot: ChatHistorySnapshot) -> Data {
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
        var receiptTimes: JSONObject = [:]
        for (id, times) in snapshot.receiptTimes where !times.isEmpty {
            var entry: JSONObject = [:]
            if let delivered = times.delivered { entry["delivered"] = .int(delivered.epochMillis) }
            if let read = times.read { entry["read"] = .int(read.epochMillis) }
            receiptTimes[id] = .object(entry)
        }
        let root: JSONObject = [
            "v": .int(Int64(version)),
            "messages": .array(snapshot.messages.map(encodeMessage)),
            "replies": .array(snapshot.replies.map(encodeMessage)),
            "roots": .object(roots),
            "reactions": encodeMarks(snapshot.reactions),
            "stickers": encodeMarks(snapshot.stickers),
            "order": .object(order),
            "edits": .object(edits),
            "private": JSONValue(snapshot.privateChannels),
            "hidden": JSONValue(snapshot.hiddenChannels),
            "hiddenMessages": JSONValue(snapshot.hiddenMessages),
            "removed": JSONValue(snapshot.removedMessages),
            "looks": .object(looks),
            "lookEvents": .array(snapshot.lookEvents.map(ChatLookFrames.encodeEvent)),
            "receipts": .object(receipts),
            "receiptTimes": .object(receiptTimes),
        ]
        return JSON.encodeData(.object(root))
    }

    public static func decode(_ data: Data) -> ChatHistorySnapshot? {
        let root = JSON.parseObject(data)
        guard let stored = root["v"]?.intValue, stored >= 1 else { return nil }
        var snapshot = ChatHistorySnapshot()
        snapshot.messages = (root["messages"]?.objects ?? []).compactMap(decodeMessage)
        snapshot.replies = (root["replies"]?.objects ?? []).compactMap(decodeMessage)
        for (id, value) in root["roots"]?.objectOrEmpty ?? [:] { snapshot.replyRoots[id] = value.text }
        snapshot.reactions = decodeMarks(root["reactions"])
        snapshot.stickers = decodeMarks(root["stickers"])
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
        for (id, value) in root["receiptTimes"]?.objectOrEmpty ?? [:] {
            let entry = value.objectOrEmpty
            let times = ChatReceiptTimes(
                delivered: entry["delivered"]?.int64Value.map { Date(epochMillis: $0) },
                read: entry["read"]?.int64Value.map { Date(epochMillis: $0) })
            if !times.isEmpty { snapshot.receiptTimes[id] = times }
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

    public static func projectScope(projectId: String, room: String) -> String {
        "project-" + scopeName(serverUrl: projectId, room: room)
    }

    public static let relayPaths = ["/api/repo/mainnode/forkmesh/rooms/general/ws", AppEnv.relayPath]

    public static func formerScopes(serverUrl: String, room: String) -> [String] {
        var urls = [serverUrl]
        if var components = URLComponents(string: serverUrl.trimmingCharacters(in: .whitespacesAndNewlines)),
            let host = components.host?.lowercased(), relayPaths.contains(components.percentEncodedPath)
        {
            let prod = components.port == nil && (host == AppEnv.prodHost || AppEnv.isProdAlias(host))
            let hosts = prod ? [AppEnv.prodHost] + AppEnv.prodAliasHosts.sorted() : [host]
            let schemes = prod ? ["wss"] : [components.scheme ?? "wss"]
            for scheme in schemes {
                for name in hosts {
                    for path in relayPaths {
                        components.scheme = scheme
                        components.host = name
                        components.percentEncodedPath = path
                        if let url = components.string { urls.append(url) }
                    }
                }
            }
        }
        var seen = Set<String>()
        return urls.map { scopeName(serverUrl: $0, room: room) }.filter { seen.insert($0).inserted }
    }

    public static func merge(_ snapshots: [ChatHistorySnapshot]) -> ChatHistorySnapshot {
        var out = ChatHistorySnapshot()
        func union(_ lists: [[ChatMessage]]) -> [ChatMessage] {
            var ids = Set<String>()
            var kept: [ChatMessage] = []
            for list in lists { for message in list where ids.insert(message.id).inserted { kept.append(message) } }
            return kept.enumerated().sorted {
                $0.element.timestamp == $1.element.timestamp ? $0.offset < $1.offset : $0.element.timestamp < $1.element.timestamp
            }.map(\.element)
        }
        func names(_ lists: [[String]]) -> [String] {
            var seen = Set<String>()
            return lists.flatMap { $0 }.filter { seen.insert($0).inserted }
        }
        out.messages = union(snapshots.map(\.messages))
        out.replies = union(snapshots.map(\.replies))
        var eventIds = Set<String>()
        out.lookEvents = snapshots.flatMap(\.lookEvents).filter { eventIds.insert($0.id).inserted }
        out.privateChannels = names(snapshots.map(\.privateChannels))
        out.hiddenChannels = names(snapshots.map(\.hiddenChannels))
        out.hiddenMessages = names(snapshots.map(\.hiddenMessages))
        out.removedMessages = names(snapshots.map(\.removedMessages))
        for snapshot in snapshots {
            out.replyRoots.merge(snapshot.replyRoots) { kept, _ in kept }
            out.reactions.merge(snapshot.reactions) { kept, _ in kept }
            out.reactionOrder.merge(snapshot.reactionOrder) { kept, _ in kept }
            out.stickers.merge(snapshot.stickers) { kept, _ in kept }
            out.edits.merge(snapshot.edits) { kept, _ in kept }
            out.looks.merge(snapshot.looks) { kept, _ in kept }
            out.receipts.merge(snapshot.receipts) { kept, _ in kept }
            out.receiptTimes.merge(snapshot.receiptTimes) { kept, _ in kept }
        }
        let removed = Set(out.removedMessages)
        out.messages.removeAll { removed.contains($0.id) }
        out.replies.removeAll { removed.contains($0.id) }
        return out
    }
}

public enum ChatHistoryLoad: Equatable, Sendable {
    case loaded(ChatHistorySnapshot)
    case missing
    case unreadable
}

public struct ChatHistoryParts: Sendable {
    public var conversations: [String: [ChatMessage]] = [:]
    public var replies: [ChatMessage] = []
    public var replyRoots: [String: String] = [:]
    public var reactions: [String: [String: [String: String]]] = [:]
    public var reactionOrder: [String: [String]] = [:]
    public var stickers: [String: [String: [String: String]]] = [:]
    public var edits: [String: String] = [:]
    public var privateChannels: Set<String> = []
    public var hiddenChannels: Set<String> = []
    public var hiddenMessages: [String] = []
    public var removedMessages: [String] = []
    public var looks: [String: ChatLook] = [:]
    public var lookEvents: [ChatLookEvent] = []
    public var receipts: [String: ChatReceipt] = [:]
    public var receiptTimes: [String: ChatReceiptTimes] = [:]
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
        snapshot.stickers = stickers.filter { known.contains($0.key) }
        snapshot.edits = edits.filter { known.contains($0.key) }
        snapshot.privateChannels = privateChannels.sorted()
        snapshot.hiddenChannels = hiddenChannels.sorted()
        snapshot.hiddenMessages = hiddenMessages
        snapshot.removedMessages = removedMessages
        snapshot.looks = looks
        snapshot.lookEvents = lookEvents.filter { !isExpired($0.conversation, $0.timestamp) }
        snapshot.receipts = receipts.filter { kept.contains($0.key) }
        snapshot.receiptTimes = receiptTimes.filter { kept.contains($0.key) }
        return snapshot
    }

    private func isExpired(_ key: String, _ timestamp: Date) -> Bool {
        retention?.isExpired(timestamp, key: key, now: now) ?? false
    }
}

final class ChatHistoryWriter: @unchecked Sendable {
    private let queue = DispatchQueue(label: "com.forkmesh.chat-history", qos: .utility)
    private var written: [URL: ChatHistorySnapshot] = [:]
    private var blocked: Set<URL> = []
    private(set) var writes = 0

    func block(_ target: URL) {
        queue.sync { _ = blocked.insert(target) }
    }

    func unblock(_ target: URL) {
        queue.sync { _ = blocked.remove(target) }
    }

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

    func wrote(_ target: URL) -> Bool { queue.sync { written[target] != nil } }

    private func write(_ snapshot: ChatHistorySnapshot, to target: URL, in directory: URL) {
        if blocked.contains(target) || written[target] == snapshot { return }
        let data = ChatHistoryCodec.encode(snapshot)
        try? FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        #if os(iOS)
        let options: Data.WritingOptions = [.atomic, .completeFileProtectionUntilFirstUserAuthentication]
        #else
        let options: Data.WritingOptions = [.atomic]
        #endif
        do {
            try data.write(to: target, options: options)
            ChatFiles.excludeFromBackup(directory)
            ChatFiles.excludeFromBackup(target)
            written[target] = snapshot
            writes += 1
        } catch {
            written[target] = nil
        }
    }
}

public enum ChatFiles {
    public static func excludeFromBackup(_ url: URL) {
        #if canImport(Darwin)
        var values = URLResourceValues()
        values.isExcludedFromBackup = true
        var target = url
        try? target.setResourceValues(values)
        #endif
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
        if case .loaded(let snapshot) = read(scope: scope) { return snapshot }
        return nil
    }

    public func read(scope: String, formerScopes: [String] = []) -> ChatHistoryLoad {
        let target = file(scope: scope)
        let manager = FileManager.default
        if !manager.fileExists(atPath: target.path) {
            let former = formerScopes.filter { $0 != scope }.map(file(scope:)).filter { manager.fileExists(atPath: $0.path) }
            if former.isEmpty {
                writer.unblock(target)
                return .missing
            }
            return adopt(former, into: target)
        }
        guard let data = try? Data(contentsOf: target) else {
            writer.block(target)
            return .unreadable
        }
        guard let snapshot = ChatHistoryCodec.decode(data) else {
            if setAside(target, as: "unreadable-\(Date().epochMillis)") {
                writer.unblock(target)
                return .missing
            }
            writer.block(target)
            return .unreadable
        }
        writer.unblock(target)
        writer.remember(snapshot, at: target)
        return .loaded(snapshot)
    }

    private func adopt(_ former: [URL], into target: URL) -> ChatHistoryLoad {
        var found: [(URL, Date, ChatHistorySnapshot)] = []
        for url in former {
            guard let data = try? Data(contentsOf: url) else {
                writer.block(target)
                return .unreadable
            }
            guard let snapshot = ChatHistoryCodec.decode(data) else { continue }
            let modified = (try? url.resourceValues(forKeys: [.contentModificationDateKey]))?.contentModificationDate
            found.append((url, modified ?? .distantPast, snapshot))
        }
        writer.unblock(target)
        if found.isEmpty { return .missing }
        let merged = ChatHistoryCodec.merge(found.sorted { $0.1 > $1.1 }.map(\.2))
        writer.saveNow(merged, to: target, in: directory)
        if writer.wrote(target) {
            for (url, _, _) in found { setAside(url, as: "migrated") }
        }
        return .loaded(merged)
    }

    @discardableResult
    private func setAside(_ url: URL, as suffix: String) -> Bool {
        (try? FileManager.default.moveItem(at: url, to: url.appendingPathExtension(suffix))) != nil
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
