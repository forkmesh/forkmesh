import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import Observation

public struct DirectConversation: Equatable, Sendable, Identifiable {
    public let id: String
    public let otherUser: String
    public let updatedAt: Int64
    public let keyVersion: Int
    public var unreadCount: Int

    public var conversation: String { DirectMessageFrames.prefix + id }

    public init(id: String, otherUser: String, updatedAt: Int64 = 0, keyVersion: Int = 1, unreadCount: Int = 0) {
        self.id = id
        self.otherUser = otherUser
        self.updatedAt = updatedAt
        self.keyVersion = keyVersion
        self.unreadCount = unreadCount
    }
}

public enum DirectMessageFrames {
    public static let prefix = "direct:"
    public static let endpoint = "/api/chat/direct-messages"
    public static let pageLimit = 50
    public static let maxConversations = 200
    public static let maxMessagesPerRoom = 200
    public static let maxFileBytes = 1024 * 1024
    public static let historyEnd = "forkmesh-history-end"
    public static let serverCopiesKind = "forkmesh-server-copies"
    public static let receivedKind = "forkmesh-received"
    public static let receivedRefsPerFrame = 64
    public static let supersededKind = "forkmesh-superseded"
    public static let deliveredKind = "forkmesh-delivered"
    public static let readKind = "forkmesh-read"
    public static let maxTrackedRefs = 256
    public static let maxReceipts = 512
    public static let maxPendingDeletes = 64
    public static let roomFrameTypes: Set<String> = ["thread-reply", "reaction", "edit", "delete"]
    public static let outboxLimit = 64
    public static let outboxMaxAgeMs: Int64 = 7 * 24 * 60 * 60 * 1000
    public static let maxWatchedRooms = 16
    public static let initialReconnectDelay: TimeInterval = 1
    public static let maxReconnectDelay: TimeInterval = 30

    public static func frameRef(_ text: String) -> String {
        String(Sha256.hex(Array(text.utf8)).prefix(40))
    }

    public static func wrappedKeyIds(_ sealed: JSONObject) -> [String] {
        (sealed["keys"]?.arrayValue ?? []).compactMap { $0.objectValue?["kid"]?.stringValue }
    }

    public static func supersededFrame(_ refs: [String]) -> JSONObject {
        ["kind": .string(supersededKind), "v": 1, "refs": .array(refs.map { .string($0) })]
    }

    public static func relayRef(_ envelope: JSONObject) -> String {
        let ref = envelope["ref"]?.text ?? ""
        guard ref.count == 40, ref.allSatisfy({ ("0"..."9").contains($0) || ("a"..."f").contains($0) })
        else { return "" }
        return ref
    }

    public static func isRef(_ value: String) -> Bool {
        value.count == 40 && value.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    public static func acknowledgedRef(_ envelope: JSONObject) -> String? {
        guard envelope["kind"]?.text == OfficeFrames.recipientsKind else { return nil }
        return relayRef(envelope)
    }

    public static func deliveredRefs(_ envelope: JSONObject) -> [String]? {
        guard envelope["kind"]?.text == deliveredKind else { return nil }
        return (envelope["refs"]?.arrayValue ?? []).map(\.text).filter(isRef)
    }

    public static func isReadReceipt(_ envelope: JSONObject) -> Bool { envelope["kind"]?.text == readKind }

    public static func carriesReceipt(_ plain: JSONObject) -> Bool {
        let type = plain["type"]?.text ?? ""
        return type == "chat" || type == "thread-reply"
    }

    public static func frameConversation(_ plain: JSONObject) -> String {
        let named = plain["conversation"]?.text ?? ""
        return named.isEmpty ? (plain["channel"]?.text ?? "") : named
    }

    public static func serverCopies(_ envelope: JSONObject) -> Int? {
        guard envelope["kind"]?.text == serverCopiesKind, let stored = envelope["stored"]?.intValue, stored >= 0
        else { return nil }
        return stored
    }

    public static func receivedFrame(_ refs: [String]) -> JSONObject {
        ["kind": .string(receivedKind), "v": 1, "refs": .array(refs.map { .string($0) })]
    }

    public static func serverCopiesLabel(_ stored: Int) -> String {
        switch stored {
        case 0: return "Not stored on the server"
        case 1: return "1 message on the server"
        default: return "\(stored) messages on the server"
        }
    }

    public static func isDirect(_ conversation: String) -> Bool {
        guard conversation.hasPrefix(prefix) else { return false }
        return OfficeFrames.validChannelId(String(conversation.dropFirst(prefix.count)))
    }

    public static func conversationId(_ conversation: String) -> String {
        isDirect(conversation) ? String(conversation.dropFirst(prefix.count)) : ""
    }

    public static func parseConversation(_ value: JSONObject) -> DirectConversation? {
        let id = value["id"]?.text ?? ""
        guard OfficeFrames.validChannelId(id) else { return nil }
        let other = String(
            (value["otherUser"]?.text ?? "").trimmingCharacters(in: .whitespaces).lowercased()
                .prefix(ChatLimits.maxDisplayNameChars))
        guard !other.isEmpty else { return nil }
        return DirectConversation(
            id: id, otherUser: other, updatedAt: value["updatedAt"]?.int64Value ?? 0,
            keyVersion: max(1, value["keyVersion"]?.intValue ?? 1),
            unreadCount: max(0, value["unreadCount"]?.intValue ?? 0))
    }

    public static func conversations(from payload: JSONObject) -> [DirectConversation] {
        var out: [DirectConversation] = []
        var seen = Set<String>()
        for value in payload["conversations"]?.objects ?? [] {
            if out.count >= maxConversations { break }
            guard let parsed = parseConversation(value), !seen.contains(parsed.id) else { continue }
            seen.insert(parsed.id)
            out.append(parsed)
        }
        return out
    }

    public static func users(from payload: JSONObject) -> [String] {
        var out: [String] = []
        for value in payload["users"]?.objects ?? [] {
            let name = String(
                (value["name"]?.text ?? "").trimmingCharacters(in: .whitespaces).lowercased()
                    .prefix(ChatLimits.maxDisplayNameChars))
            if !name.isEmpty && !out.contains(name) { out.append(name) }
        }
        return out
    }

    public static func chatMessage(_ plain: JSONObject, record: DirectConversation, selfId: String) -> ChatMessage? {
        guard (plain["type"]?.text ?? "") == "chat" else { return nil }
        guard (plain["channel"]?.text ?? "") == record.conversation else { return nil }
        let id = plain["id"]?.text ?? ""
        if id.isEmpty || id.count > 64 { return nil }
        let text = plain["text"]?.text ?? ""
        if text.count > ChatLimits.maxTextChars { return nil }
        let fileName = String((plain["fileName"]?.text ?? "").prefix(ChatLimits.maxFileNameChars))
        if text.isEmpty && fileName.isEmpty { return nil }
        let senderId = String((plain["senderId"]?.text ?? "").prefix(64))
        var sender = String((plain["sender"]?.text ?? "").prefix(ChatLimits.maxDisplayNameChars))
        if sender.isEmpty { sender = record.otherUser }
        let ts = plain["ts"]?.doubleValue.map { Date(epochMillis: Int64($0)) } ?? Date(epochMillis: 0)
        return ChatMessage(
            id: id, conversation: record.conversation, senderId: senderId, senderName: sender, text: text,
            timestamp: ts, isSelf: !selfId.isEmpty && senderId == selfId, fileName: fileName)
    }

    public static func isCallFrame(_ plain: JSONObject) -> Bool {
        (plain["type"]?.text ?? "").hasPrefix("call-")
    }

    public static func groupChannelName(_ names: [String]) -> String {
        var parts: [String] = []
        for name in names {
            var slug = ""
            var lastDash = true
            for character in name.lowercased() {
                let ok = (character.isLowercase && character.isLetter && character.isASCII)
                    || (character.isNumber && character.isASCII)
                if ok {
                    slug.append(character)
                    lastDash = false
                } else if !lastDash {
                    slug.append("-")
                    lastDash = true
                }
            }
            while slug.hasSuffix("-") { slug.removeLast() }
            slug = String(slug.prefix(10))
            while slug.hasSuffix("-") { slug.removeLast() }
            if !slug.isEmpty { parts.append(slug) }
        }
        var base = String(parts.joined(separator: "-").prefix(34))
        while base.hasSuffix("-") { base.removeLast() }
        if base.isEmpty { base = "group" }
        let suffix = String(format: "%04x", Int.random(in: 0..<0x10000))
        return "\(base)-\(suffix)"
    }

    public static func errorMessage(_ code: String) -> String {
        switch code {
        case "user_not_found": return "That active registered user was not found."
        case "cannot_message_self": return "Choose another person to message."
        case "rate_limited": return "Too many new conversations. Try again later."
        case "invalid_session": return "Sign in again to use direct messages."
        default: return code.isEmpty ? "The direct message could not be opened." : code.replacingOccurrences(of: "_", with: " ")
        }
    }
}

public enum DirectMessageError: Error, Equatable, CustomStringConvertible {
    case refused(String)
    case unavailable

    public var description: String {
        switch self {
        case .refused(let code): return DirectMessageFrames.errorMessage(code)
        case .unavailable: return "Direct messages could not be reached."
        }
    }
}

@MainActor
final class DirectRoomSocket {
    let conversationId: String
    let crypto: RoomCrypto
    let seal: RoomSeal
    let connection: WebSocketConnection
    var reader: Task<Void, Never>?
    var persistedAdvert = false

    init(conversationId: String, crypto: RoomCrypto, seal: RoomSeal, connection: WebSocketConnection) {
        self.conversationId = conversationId
        self.crypto = crypto
        self.seal = seal
        self.connection = connection
    }
}

struct DirectRefBook: Sendable {
    var ids: [String: String] = [:]
    var refs: [String: String] = [:]
    var order: [String] = []

    mutating func track(_ ref: String, id: String) {
        guard !ref.isEmpty, !id.isEmpty else { return }
        if ids.updateValue(id, forKey: ref) == nil { order.append(ref) }
        refs[id] = ref
        while order.count > DirectMessageFrames.maxTrackedRefs {
            let dropped = order.removeFirst()
            if let old = ids.removeValue(forKey: dropped), refs[old] == dropped { refs.removeValue(forKey: old) }
        }
    }
}

struct DirectOutboxEntry: Sendable {
    var ref: String
    let plain: JSONObject
    var kids: [String]
    let notify: Bool
    let ts: Int64

    var messageId: String { plain["id"]?.text ?? "" }
}

@MainActor
@Observable
public final class DirectMessageService {
    @ObservationIgnored let api: ChatAccountCalling
    @ObservationIgnored let connector: WebSocketConnector
    @ObservationIgnored let secureStore: SecureStore
    @ObservationIgnored let origin: @MainActor () -> URL?
    @ObservationIgnored let selfId: @MainActor () -> String
    @ObservationIgnored let displayName: @MainActor () -> String
    @ObservationIgnored let clock: Clock
    @ObservationIgnored private var sockets: [String: DirectRoomSocket] = [:]
    @ObservationIgnored private var cryptos: [String: RoomCrypto] = [:]
    @ObservationIgnored private var seals: [String: RoomSeal] = [:]
    @ObservationIgnored private var roomNames: [String: String] = [:]
    @ObservationIgnored private var seen: [String: Set<String>] = [:]
    @ObservationIgnored private var sequence = 0
    @ObservationIgnored private var receivedRefs: [String: [String]] = [:]
    @ObservationIgnored let sleeper: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored private var pinned: Set<String> = []
    @ObservationIgnored private var opening: [String: Task<Bool, Never>] = [:]
    @ObservationIgnored private var reconnects: [String: Task<Void, Never>] = [:]
    @ObservationIgnored private var backoff: [String: TimeInterval] = [:]
    @ObservationIgnored private var foreground = true
    @ObservationIgnored private var outbox: [String: [DirectOutboxEntry]] = [:]
    @ObservationIgnored public var isExpired: ChatExpiry?
    @ObservationIgnored public var onMessage: (@MainActor (ChatMessage) -> Void)?
    @ObservationIgnored public var onCallFrame: (@MainActor (JSONObject, String) -> Void)?
    @ObservationIgnored public var onLiveFrame: (@MainActor (JSONObject, String) -> Void)?
    @ObservationIgnored private var liveBudgets: [String: (perWindow: Int, windowMs: Int)] = [:]
    @ObservationIgnored public var onFrame: (@MainActor (JSONObject, String) -> Void)?
    @ObservationIgnored public var meshSender: (@MainActor (JSONObject, [String]) async -> Int)?
    @ObservationIgnored public var isRemoved: (@MainActor (String) -> Bool)?
    @ObservationIgnored public var onRemoved: (@MainActor (String, String) -> Void)?
    @ObservationIgnored private var refBooks: [String: DirectRefBook] = [:]
    @ObservationIgnored private var receiptOrder: [String] = []
    @ObservationIgnored private var pendingDeletes: [String: [String: String]] = [:]
    @ObservationIgnored private var pendingDeleteOrder: [String: [String]] = [:]
    @ObservationIgnored private var removedIds: Set<String> = []

    public private(set) var records: [DirectConversation] = []
    public private(set) var nextCursor = ""
    public private(set) var loading = false
    public private(set) var lastError: String?
    public private(set) var connected: Set<String> = []
    public private(set) var serverCopies: [String: Int] = [:]
    public private(set) var receipts: [String: ChatReceipt] = [:]
    private var history: [String: [ChatMessage]] = [:]

    public init(
        api: ChatAccountCalling, connector: WebSocketConnector, secureStore: SecureStore,
        origin: @escaping @MainActor () -> URL?, selfId: @escaping @MainActor () -> String,
        displayName: @escaping @MainActor () -> String, clock: Clock = SystemClock(),
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.api = api
        self.connector = connector
        self.secureStore = secureStore
        self.origin = origin
        self.selfId = selfId
        self.displayName = displayName
        self.clock = clock
        self.sleeper = sleep
    }

    public var conversations: [String] { records.map(\.conversation) }

    public var currentSelfId: String { selfId() }

    public var unreadTotal: Int { records.reduce(0) { $0 + $1.unreadCount } }

    public func record(for conversation: String) -> DirectConversation? {
        records.first { $0.conversation == conversation }
    }

    public func messages(_ conversation: String) -> [ChatMessage] { history[conversation] ?? [] }

    public func receipt(_ messageId: String) -> ChatReceipt? { receipts[messageId] }

    public func ref(of messageId: String, in conversation: String) -> String? {
        refBooks[DirectMessageFrames.conversationId(conversation)]?.refs[messageId]
    }

    public func serverCopies(_ conversation: String) -> Int? {
        serverCopies[DirectMessageFrames.conversationId(conversation)]
    }

    public func isConnected(_ conversation: String) -> Bool {
        connected.contains(DirectMessageFrames.conversationId(conversation))
    }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            let payload = try await call(
                "GET", "\(DirectMessageFrames.endpoint)?limit=\(DirectMessageFrames.pageLimit)", nil)
            apply(DirectMessageFrames.conversations(from: payload), replacing: true)
            nextCursor = payload["nextCursor"]?.text ?? ""
            lastError = nil
            watchWanted()
        } catch {
            lastError = "\(error)"
        }
    }

    public func loadMore() async {
        guard !nextCursor.isEmpty,
            let cursor = nextCursor.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed)
        else { return }
        do {
            let payload = try await call(
                "GET", "\(DirectMessageFrames.endpoint)?limit=\(DirectMessageFrames.pageLimit)&cursor=\(cursor)", nil)
            apply(DirectMessageFrames.conversations(from: payload), replacing: false)
            nextCursor = payload["nextCursor"]?.text ?? ""
        } catch {
            lastError = "\(error)"
        }
    }

    private func apply(_ incoming: [DirectConversation], replacing: Bool) {
        var merged = replacing ? [] : records
        for conversation in incoming where !merged.contains(where: { $0.id == conversation.id }) {
            merged.append(conversation)
        }
        if replacing {
            for old in records where !merged.contains(where: { $0.id == old.id }) {
                history.removeValue(forKey: old.conversation)
                pinned.remove(old.id)
                outbox.removeValue(forKey: old.id)
                reconnects.removeValue(forKey: old.id)?.cancel()
                if let socket = sockets[old.id] { Task { await self.close(socket) } }
            }
        }
        records = merged
    }

    public func searchUsers(_ query: String) async -> [String] {
        let trimmed = query.trimmingCharacters(in: .whitespaces).lowercased()
        guard !trimmed.isEmpty,
            let encoded = trimmed.addingPercentEncoding(withAllowedCharacters: .urlQueryAllowed)
        else { return [] }
        do {
            let payload = try await call("GET", "\(DirectMessageFrames.endpoint)/users?query=\(encoded)", nil)
            return DirectMessageFrames.users(from: payload)
        } catch {
            return []
        }
    }

    public func start(username: String) async throws -> DirectConversation {
        let normalized = username.trimmingCharacters(in: .whitespaces).lowercased()
        guard !normalized.isEmpty else { throw DirectMessageError.refused("user_not_found") }
        let payload = try await call("POST", DirectMessageFrames.endpoint, ["username": .string(normalized)])
        guard let value = payload["conversation"]?.objectValue, let parsed = DirectMessageFrames.parseConversation(value)
        else { throw DirectMessageError.unavailable }
        if let index = records.firstIndex(where: { $0.id == parsed.id }) {
            records[index] = parsed
        } else {
            records.insert(parsed, at: 0)
        }
        return parsed
    }

    public func markRead(_ conversation: String) async {
        let id = DirectMessageFrames.conversationId(conversation)
        guard let index = records.firstIndex(where: { $0.id == id }) else { return }
        if records[index].unreadCount != 0 { records[index].unreadCount = 0 }
        _ = try? await call("POST", "\(DirectMessageFrames.endpoint)/\(id)/read", nil)
    }

    public func clearUnread(_ conversation: String) {
        let id = DirectMessageFrames.conversationId(conversation)
        guard let index = records.firstIndex(where: { $0.id == id }), records[index].unreadCount != 0 else { return }
        records[index].unreadCount = 0
    }

    public func bumpUnread(_ conversation: String) {
        let id = DirectMessageFrames.conversationId(conversation)
        guard let index = records.firstIndex(where: { $0.id == id }) else { return }
        records[index].unreadCount += 1
    }

    private func derive(_ room: String, _ passphrase: String) async -> RoomCrypto {
        if let cached = cryptos["\(room)|\(passphrase)"] { return cached }
        let crypto = await RelayService.deriveRoomCrypto(room, passphrase)
        cryptos["\(room)|\(passphrase)"] = crypto
        return crypto
    }

    private func seal(for room: String) throws -> RoomSeal {
        if let existing = seals[room] { return existing }
        let scope = origin()?.host ?? ""
        let loaded = try RoomSeal.loadOrCreate(room, storageScope: "direct|\(scope)", secureStore: secureStore)
        seals[room] = loaded
        return loaded
    }

    @discardableResult
    public func open(_ conversation: String) async -> Bool {
        guard let record = record(for: conversation) else { return false }
        pinned.insert(record.id)
        return await connect(record.id)
    }

    public func appLifecycleChanged(_ state: AppLifecycleState) {
        if state == .inactive { return }
        foreground = state == .resumed
        cancelReconnects()
        if foreground {
            backoff.removeAll()
            watchWanted()
        }
    }

    private func wanted(_ id: String) -> Bool {
        guard foreground, let record = records.first(where: { $0.id == id }) else { return false }
        return pinned.contains(id) || record.unreadCount > 0
    }

    private func watchWanted() {
        var watched = 0
        for record in records where wanted(record.id) {
            if watched >= DirectMessageFrames.maxWatchedRooms { break }
            watched += 1
            let id = record.id
            if sockets[id] != nil || opening[id] != nil || reconnects[id] != nil { continue }
            Task { await self.connect(id) }
        }
    }

    @discardableResult
    private func connect(_ id: String) async -> Bool {
        if sockets[id] != nil { return true }
        if let pending = opening[id] { return await pending.value }
        let task = Task { await self.establish(id) }
        opening[id] = task
        let ok = await task.value
        opening.removeValue(forKey: id)
        if ok {
            backoff.removeValue(forKey: id)
            reconnects.removeValue(forKey: id)?.cancel()
        } else {
            scheduleReconnect(id)
        }
        return ok
    }

    private func scheduleReconnect(_ id: String) {
        guard wanted(id), reconnects[id] == nil, sockets[id] == nil else { return }
        let delay = backoff[id] ?? DirectMessageFrames.initialReconnectDelay
        backoff[id] = min(delay * 2, DirectMessageFrames.maxReconnectDelay)
        let sleeper = self.sleeper
        reconnects[id] = Task { [weak self] in
            do { try await sleeper(delay) } catch { return }
            guard let self, !Task.isCancelled else { return }
            self.reconnects.removeValue(forKey: id)
            guard self.wanted(id), self.sockets[id] == nil else { return }
            await self.connect(id)
        }
    }

    private func cancelReconnects() {
        for task in reconnects.values { task.cancel() }
        reconnects.removeAll()
    }

    private func establish(_ id: String) async -> Bool {
        guard records.contains(where: { $0.id == id }), let base = origin() else { return false }
        do {
            let access = try await call("GET", "\(DirectMessageFrames.endpoint)/\(id)/room-access", nil)
            let roomName = access["room"]?.text ?? ""
            let passphrase = access["passphrase"]?.text ?? ""
            guard !roomName.isEmpty, !passphrase.isEmpty,
                let url = OfficeFrames.webSocketURL(access["webSocketUrl"]?.text ?? "", origin: base)
            else {
                lastError = DirectMessageError.unavailable.description
                return false
            }
            let crypto = await derive(roomName, passphrase)
            let seal = try seal(for: roomName)
            roomNames[id] = roomName
            let connection = try await connector.connect(
                url: url,
                options: WebSocketOptions(
                    connectTimeout: RelayService.connectTimeout, pingInterval: RelayService.pingInterval))
            guard records.contains(where: { $0.id == id }), sockets[id] == nil else {
                await connection.close(code: WebSocketStatus.normalClosure, reason: "")
                return sockets[id] != nil
            }
            let socket = DirectRoomSocket(conversationId: id, crypto: crypto, seal: seal, connection: connection)
            sockets[id] = socket
            connected.insert(id)
            socket.reader = Task { [weak self] in
                do {
                    for try await message in connection.messages {
                        guard let self else { return }
                        self.receive(message, conversationId: id)
                    }
                } catch {}
                self?.dropped(id, connection)
            }
            advertise(socket, persist: seal.shouldPersistAdvertisement(now: clock.nowMillis))
            return true
        } catch {
            lastError = "\(error)"
            return false
        }
    }

    private func receive(_ message: WebSocketMessage, conversationId: String) {
        guard let socket = sockets[conversationId], let record = records.first(where: { $0.id == conversationId })
        else { return }
        let text: String
        switch message {
        case .text(let value): text = value
        case .data(let value): text = String(decoding: value, as: UTF8.self)
        }
        let envelope = JSON.parseObject(text)
        if envelope.isEmpty { return }
        if envelope["kind"]?.text == DirectMessageFrames.historyEnd {
            reseal(socket)
            return
        }
        if let stored = DirectMessageFrames.serverCopies(envelope) {
            serverCopies[conversationId] = stored
            if stored == 0 { outbox.removeValue(forKey: conversationId) }
            return
        }
        if let budget = DirectLiveFrames.budget(envelope) {
            liveBudgets[conversationId] = budget
            return
        }
        if let ref = DirectMessageFrames.acknowledgedRef(envelope) {
            if let id = refBooks[conversationId]?.ids[ref] { setReceipt(.sent, id) }
            return
        }
        if let refs = DirectMessageFrames.deliveredRefs(envelope) {
            for ref in refs {
                if let id = refBooks[conversationId]?.ids[ref] { setReceipt(.delivered, id) }
            }
            return
        }
        if DirectMessageFrames.isReadReceipt(envelope) {
            markReadByPeer(conversationId)
            return
        }
        guard let framed = socket.crypto.decrypt(envelope) else { return }
        if accept(framed, envelope: envelope, record: record, socket: socket) {
            noteReceived(DirectMessageFrames.relayRef(envelope), conversationId: conversationId)
        }
    }

    private func noteReceived(_ ref: String, conversationId: String) {
        guard !ref.isEmpty else { return }
        var refs = receivedRefs[conversationId] ?? []
        guard !refs.contains(ref) else { return }
        refs.append(ref)
        receivedRefs[conversationId] = refs
        guard refs.count == 1 else { return }
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: 150_000_000)
            await self?.flushReceived(conversationId)
        }
    }

    private func flushReceived(_ conversationId: String) async {
        let refs = receivedRefs.removeValue(forKey: conversationId) ?? []
        guard let connection = sockets[conversationId]?.connection, !refs.isEmpty else { return }
        for start in stride(from: 0, to: refs.count, by: DirectMessageFrames.receivedRefsPerFrame) {
            let chunk = Array(refs[start..<min(start + DirectMessageFrames.receivedRefsPerFrame, refs.count)])
            try? await connection.send(text: JSON.encode(DirectMessageFrames.receivedFrame(chunk)))
        }
    }

    @discardableResult
    private func accept(_ frame: JSONObject, envelope: JSONObject, record: DirectConversation, socket: DirectRoomSocket)
        -> Bool
    {
        let seal = socket.seal
        let live = envelope["historyReplay"] != .bool(true)
        if RoomSeal.isKeyFrame(frame) {
            if seal.acceptKeyFrame(frame), live {
                advertise(socket)
                reseal(socket)
            }
            return false
        }
        guard RoomSeal.isSealedFrame(frame) else { return false }
        let kids = DirectMessageFrames.wrappedKeyIds(frame)
        guard kids.contains(seal.keyId) else {
            if !socket.persistedAdvert {
                socket.persistedAdvert = true
                advertise(socket, persist: true)
            }
            return false
        }
        let sender = RoomSeal.frameKeyId(frame)
        let knownSender = sender == seal.keyId || seal.peerKeyIds.contains(sender)
        guard let plain = seal.unseal(frame) else { return false }
        if !knownSender && live { reseal(socket) }
        if DirectMessageFrames.isCallFrame(plain) {
            if live { onCallFrame?(plain, record.conversation) }
            return true
        }
        if DirectLiveFrames.isLiveFrame(plain) {
            if live { onLiveFrame?(plain, record.conversation) }
            return true
        }
        let me = selfId()
        let type = plain["type"]?.text ?? ""
        let messageId = plain["id"]?.text ?? ""
        let mine = !me.isEmpty && plain["senderId"]?.text == me
        if DirectMessageFrames.roomFrameTypes.contains(type), DirectMessageFrames.frameConversation(plain) != record.conversation {
            return true
        }
        if DirectMessageFrames.carriesReceipt(plain) {
            if isGone(messageId) { return true }
            if consumePendingDelete(plain, record: record) { return true }
        }
        if mine && DirectMessageFrames.carriesReceipt(plain) {
            refBooks[record.id, default: DirectRefBook()].track(DirectMessageFrames.relayRef(envelope), id: messageId)
            setReceipt(.sent, messageId)
        }
        if !live, mine {
            remember(
                DirectOutboxEntry(
                    ref: DirectMessageFrames.relayRef(envelope), plain: plain, kids: kids,
                    notify: envelope["notify"] == .bool(true),
                    ts: plain["ts"]?.doubleValue.map { Int64($0) } ?? clock.nowMillis),
                conversationId: record.id)
        }
        if type == "delete" { applyDelete(plain, record: record, stash: true) }
        guard let message = DirectMessageFrames.chatMessage(plain, record: record, selfId: me) else {
            onFrame?(plain, record.conversation)
            return true
        }
        let added = append(message, conversationId: record.id)
        onFrame?(plain, record.conversation)
        if added && live && !message.isSelf { onMessage?(message) }
        return true
    }

    private func isGone(_ id: String) -> Bool {
        !id.isEmpty && (removedIds.contains(id) || isRemoved?(id) == true)
    }

    private func setReceipt(_ receipt: ChatReceipt, _ id: String) {
        guard !id.isEmpty, !removedIds.contains(id) else { return }
        if let current = receipts[id] {
            if current.rank >= receipt.rank { return }
        } else {
            receiptOrder.append(id)
        }
        receipts[id] = receipt
        while receiptOrder.count > DirectMessageFrames.maxReceipts {
            receipts.removeValue(forKey: receiptOrder.removeFirst())
        }
    }

    private func forgetReceipt(_ id: String) {
        guard receipts.removeValue(forKey: id) != nil else { return }
        receiptOrder.removeAll { $0 == id }
    }

    private func markReadByPeer(_ conversationId: String) {
        guard let book = refBooks[conversationId] else { return }
        for id in book.refs.keys {
            if let current = receipts[id], current == .sent || current == .delivered { receipts[id] = .read }
        }
    }

    @discardableResult
    private func applyDelete(_ plain: JSONObject, record: DirectConversation, stash: Bool) -> Bool {
        let target = plain["target"]?.text ?? ""
        let senderId = plain["senderId"]?.text ?? ""
        guard !target.isEmpty, !senderId.isEmpty else { return false }
        if let message = history[record.conversation]?.first(where: { $0.id == target }) {
            guard message.senderId == senderId else { return false }
            removeLocal(target, record: record)
            return true
        }
        guard stash, !removedIds.contains(target) else { return false }
        var waiting = pendingDeletes[record.id] ?? [:]
        var order = pendingDeleteOrder[record.id] ?? []
        if waiting.updateValue(senderId, forKey: target) == nil { order.append(target) }
        while order.count > DirectMessageFrames.maxPendingDeletes { waiting.removeValue(forKey: order.removeFirst()) }
        pendingDeletes[record.id] = waiting
        pendingDeleteOrder[record.id] = order
        return false
    }

    private func consumePendingDelete(_ plain: JSONObject, record: DirectConversation) -> Bool {
        let id = plain["id"]?.text ?? ""
        guard !id.isEmpty, let sender = pendingDeletes[record.id]?[id], sender == (plain["senderId"]?.text ?? "") else {
            return false
        }
        pendingDeletes[record.id]?.removeValue(forKey: id)
        pendingDeleteOrder[record.id]?.removeAll { $0 == id }
        removedIds.insert(id)
        seen[record.id, default: []].insert(id)
        onRemoved?(record.conversation, id)
        return true
    }

    private func removeLocal(_ id: String, record: DirectConversation) {
        if var list = history[record.conversation] {
            list.removeAll { $0.id == id }
            history[record.conversation] = list.isEmpty ? nil : list
        }
        removedIds.insert(id)
        seen[record.id, default: []].insert(id)
        if outbox[record.id] != nil { outbox[record.id]?.removeAll { $0.messageId == id } }
        forgetReceipt(id)
        onRemoved?(record.conversation, id)
    }

    private func remember(_ entry: DirectOutboxEntry, conversationId: String) {
        var entries = outbox[conversationId] ?? []
        let messageId = entry.messageId
        if !messageId.isEmpty, let index = entries.firstIndex(where: { $0.messageId == messageId }) {
            entries[index].ref = entry.ref
            entries[index].kids = entry.kids
        } else {
            entries.append(entry)
        }
        if entries.count > DirectMessageFrames.outboxLimit {
            entries.removeFirst(entries.count - DirectMessageFrames.outboxLimit)
        }
        outbox[conversationId] = entries
    }

    private func reseal(_ socket: DirectRoomSocket) {
        let id = socket.conversationId
        let now = clock.nowMillis
        var entries = (outbox[id] ?? []).filter { now - $0.ts < DirectMessageFrames.outboxMaxAgeMs }
        let peers = Set(socket.seal.peerKeyIds)
        var texts: [String] = []
        var superseded: [String] = []
        if !peers.isEmpty {
            for index in entries.indices where !peers.isSubset(of: Set(entries[index].kids)) {
                guard let sealed = try? socket.seal.seal(entries[index].plain),
                    var envelope = try? socket.crypto.encrypt(sealed)
                else { continue }
                envelope["persist"] = true
                if entries[index].notify { envelope["notify"] = true }
                envelope["resend"] = true
                let text = JSON.encode(envelope)
                if !entries[index].ref.isEmpty { superseded.append(entries[index].ref) }
                entries[index].ref = DirectMessageFrames.frameRef(text)
                if DirectMessageFrames.carriesReceipt(entries[index].plain) {
                    refBooks[id, default: DirectRefBook()].track(entries[index].ref, id: entries[index].messageId)
                }
                entries[index].kids = DirectMessageFrames.wrappedKeyIds(sealed)
                texts.append(text)
            }
        }
        outbox[id] = entries.isEmpty ? nil : entries
        for start in stride(from: 0, to: superseded.count, by: DirectMessageFrames.receivedRefsPerFrame) {
            let chunk = Array(superseded[start..<min(start + DirectMessageFrames.receivedRefsPerFrame, superseded.count)])
            texts.append(JSON.encode(DirectMessageFrames.supersededFrame(chunk)))
        }
        guard !texts.isEmpty else { return }
        let connection = socket.connection
        Task {
            for text in texts { try? await connection.send(text: text) }
        }
    }

    @discardableResult
    private func append(_ message: ChatMessage, conversationId: String) -> Bool {
        if isExpired?(message.conversation, message.timestamp) == true { return false }
        var ids = seen[conversationId] ?? []
        if ids.contains(message.id) { return false }
        ids.insert(message.id)
        seen[conversationId] = ids
        var list = history[message.conversation] ?? []
        var at = list.count
        while at > 0 && RelayService.compareMessages(list[at - 1], message) > 0 { at -= 1 }
        list.insert(message, at: at)
        if list.count > DirectMessageFrames.maxMessagesPerRoom {
            list.removeFirst(list.count - DirectMessageFrames.maxMessagesPerRoom)
        }
        history[message.conversation] = list
        return true
    }

    @discardableResult
    public func pruneExpired() -> Bool {
        guard let isExpired else { return false }
        var changed = false
        for (conversation, list) in history {
            let kept = list.filter { !isExpired(conversation, $0.timestamp) }
            if kept.count == list.count { continue }
            changed = true
            history[conversation] = kept.isEmpty ? nil : kept
        }
        return changed
    }

    func frame(_ record: DirectConversation, type: String, extra: JSONObject = [:]) -> JSONObject {
        sequence += 1
        let me = selfId()
        var plain: JSONObject = [
            "type": .string(type),
            "id": .string("\(me)-\(clock.nowMillis)-\(sequence)"),
            "senderId": .string(me),
            "sender": .string(String(displayName().prefix(ChatLimits.maxDisplayNameChars))),
            "accountKind": "user",
            "ts": .int(clock.nowMillis),
            "platform": .string(AppBuild.currentPlatform),
            "version": .string(RelayService.clientVersion),
            "channel": .string(record.conversation),
        ]
        for (key, value) in extra { plain[key] = value }
        return plain
    }

    public func deviceKeys(_ conversation: String) -> [String] {
        guard let record = record(for: conversation), let room = roomNames[record.id], let seal = seals[room] else {
            return []
        }
        return seal.peerKeyIds
    }

    @discardableResult
    public func acceptMeshDirect(_ channel: String, sealKeyId: String, plain: JSONObject) -> Bool {
        guard let record = record(for: channel), !sealKeyId.isEmpty, plain["channel"]?.text == channel,
            deviceKeys(channel).contains(sealKeyId)
        else { return false }
        var frame = plain
        frame["type"] = .string("chat")
        if isGone(plain["id"]?.text ?? "") { return false }
        guard let message = DirectMessageFrames.chatMessage(frame, record: record, selfId: selfId()),
            append(message, conversationId: record.id)
        else { return false }
        if !message.isSelf { onMessage?(message) }
        return true
    }

    private func sendOverMesh(_ record: DirectConversation, _ plain: JSONObject) async -> Int {
        guard let meshSender else { return 0 }
        let keys = deviceKeys(record.conversation)
        if keys.isEmpty { return 0 }
        return await meshSender(plain, keys)
    }

    private func settleUnsent(_ record: DirectConversation, _ plain: JSONObject, shown: Bool, reached: Int) -> Bool {
        let id = plain["id"]?.text ?? ""
        if reached > 0 {
            if shown { setReceipt(.sent, id) }
            return true
        }
        if shown {
            if var list = history[record.conversation] {
                list.removeAll { $0.id == id }
                history[record.conversation] = list.isEmpty ? nil : list
            }
            seen[record.id]?.remove(id)
        }
        forgetReceipt(id)
        return false
    }

    @discardableResult
    public func send(_ conversation: String, text: String) async -> Bool {
        let body = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !body.isEmpty, let record = record(for: conversation) else { return false }
        let plain = frame(record, type: "chat", extra: ["text": .string(String(body.prefix(ChatLimits.maxTextChars)))])
        let reached = await sendOverMesh(record, plain)
        return await sendSealed(record, plain, reached: reached)
    }

    @discardableResult
    public func sendFile(_ conversation: String, fileName: String, mime: String, data: Data) async -> Bool {
        let name = String(fileName.trimmingCharacters(in: .whitespacesAndNewlines).prefix(ChatLimits.maxFileNameChars))
        guard !name.isEmpty, !data.isEmpty, data.count <= DirectMessageFrames.maxFileBytes,
            let record = record(for: conversation)
        else { return false }
        let plain = frame(
            record, type: "chat",
            extra: [
                "fileName": .string(name),
                "fileMime": .string(String(mime.prefix(ChatLimits.maxMimeChars))),
                "file": .string(data.base64EncodedString()),
            ])
        return await sendSealed(record, plain, reached: 0)
    }

    @discardableResult
    public func sendLook(_ conversation: String, change: ChatLookChange) async -> Bool {
        guard let record = record(for: conversation) else { return false }
        let plain = frame(record, type: ChatLookFrames.type, extra: ChatLookFrames.fields(change))
        return await sendSealed(record, plain, reached: 0, notify: false)
    }

    public func liveInterval(_ conversation: String) -> Int64 {
        guard let budget = liveBudgets[DirectMessageFrames.conversationId(conversation)] else {
            return DirectLiveFrames.fallbackIntervalMs
        }
        return DirectLiveFrames.interval(perWindow: budget.perWindow, windowMs: budget.windowMs)
    }

    @discardableResult
    public func sendLive(_ conversation: String, fields: JSONObject) async -> Bool {
        guard let record = record(for: conversation), let socket = sockets[record.id] else { return false }
        let plain = frame(record, type: DirectLiveFrames.type, extra: fields)
        do {
            var envelope = try socket.crypto.encrypt(try socket.seal.seal(plain))
            envelope["live"] = true
            let text = JSON.encode(envelope)
            if text.utf8.count > DirectLiveFrames.maxEnvelopeBytes { return false }
            try await socket.connection.send(text: text)
            return true
        } catch {
            return false
        }
    }

    private func sendSealed(_ record: DirectConversation, _ plain: JSONObject, reached: Int, notify: Bool = true) async
        -> Bool
    {
        let id = plain["id"]?.text ?? ""
        let receipted = DirectMessageFrames.carriesReceipt(plain)
        var shown = false
        if let message = DirectMessageFrames.chatMessage(plain, record: record, selfId: selfId()) {
            shown = append(message, conversationId: record.id)
        }
        if receipted { setReceipt(.sending, id) }
        if sockets[record.id] == nil, !(await open(record.conversation)) {
            return settleUnsent(record, plain, shown: shown, reached: reached)
        }
        guard let socket = sockets[record.id] else { return settleUnsent(record, plain, shown: shown, reached: reached) }
        do {
            let sealed = try socket.seal.seal(plain)
            var envelope = try socket.crypto.encrypt(sealed)
            envelope["persist"] = true
            if notify { envelope["notify"] = true }
            let sent = JSON.encode(envelope)
            if receipted { refBooks[record.id, default: DirectRefBook()].track(DirectMessageFrames.frameRef(sent), id: id) }
            try await socket.connection.send(text: sent)
            remember(
                DirectOutboxEntry(
                    ref: DirectMessageFrames.frameRef(sent), plain: plain,
                    kids: DirectMessageFrames.wrappedKeyIds(sealed), notify: notify, ts: clock.nowMillis),
                conversationId: record.id)
            if plain["type"]?.text == "delete" { applyDelete(plain, record: record, stash: false) }
            onFrame?(plain, record.conversation)
            return true
        } catch {
            lastError = "\(error)"
            return settleUnsent(record, plain, shown: shown, reached: reached)
        }
    }

    @discardableResult
    public func sendFrame(_ conversation: String, type: String, fields: JSONObject) async -> JSONObject? {
        guard DirectMessageFrames.roomFrameTypes.contains(type), let record = record(for: conversation) else { return nil }
        var plain = frame(record, type: type, extra: fields)
        if type != "thread-reply" {
            plain.removeValue(forKey: "channel")
            plain["conversation"] = .string(record.conversation)
        }
        if type == "reaction", (plain["reactorName"]?.text ?? "").isEmpty { plain["reactorName"] = plain["sender"] }
        if type == "edit", plain["editedAt"] == nil { plain["editedAt"] = plain["ts"] }
        return await sendSealed(record, plain, reached: 0, notify: type == "thread-reply") ? plain : nil
    }

    @discardableResult
    public func deleteForEveryone(_ conversation: String, messageId: String) async -> Bool {
        guard !messageId.isEmpty, let record = record(for: conversation) else { return false }
        let original = refBooks[record.id]?.refs[messageId]
        guard await sendFrame(conversation, type: "delete", fields: ["target": .string(messageId)]) != nil else {
            return false
        }
        outbox[record.id]?.removeAll { $0.messageId == messageId }
        if let original, let connection = sockets[record.id]?.connection {
            try? await connection.send(text: JSON.encode(DirectMessageFrames.supersededFrame([original])))
        }
        return true
    }

    @discardableResult
    public func sendCallFrame(_ conversation: String, type: String, extra: JSONObject) async -> Bool {
        guard type.hasPrefix("call-"), let record = record(for: conversation), let socket = sockets[record.id]
        else { return false }
        let plain = frame(record, type: type, extra: extra)
        do {
            let envelope = try socket.crypto.encrypt(try socket.seal.seal(plain))
            try await socket.connection.send(text: JSON.encode(envelope))
            return true
        } catch {
            return false
        }
    }

    private func advertise(_ socket: DirectRoomSocket, persist: Bool = false) {
        guard var envelope = try? socket.crypto.encrypt(socket.seal.keyFrame()) else { return }
        if persist { envelope["persist"] = true }
        let connection = socket.connection
        Task { try? await connection.send(text: JSON.encode(envelope)) }
    }

    private func dropped(_ conversationId: String, _ connection: WebSocketConnection) {
        guard let socket = sockets[conversationId], socket.connection === connection else { return }
        sockets.removeValue(forKey: conversationId)
        connected.remove(conversationId)
        scheduleReconnect(conversationId)
    }

    private func close(_ socket: DirectRoomSocket) async {
        socket.reader?.cancel()
        sockets.removeValue(forKey: socket.conversationId)
        connected.remove(socket.conversationId)
        await socket.connection.close(code: WebSocketStatus.normalClosure, reason: "")
    }

    public func closeAll() async {
        cancelReconnects()
        for socket in Array(sockets.values) { await close(socket) }
    }

    public func clear() async {
        await closeAll()
        records = []
        history = [:]
        nextCursor = ""
        pinned = []
        backoff = [:]
        outbox = [:]
        liveBudgets = [:]
        receipts = [:]
        receiptOrder = []
        refBooks = [:]
        pendingDeletes = [:]
        pendingDeleteOrder = [:]
        removedIds = []
    }

    private func call(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        do {
            return try await api.accountApiCall(method, path, body)
        } catch let error as OrgApiException {
            throw DirectMessageError.refused(error.code)
        }
    }
}

extension DirectMessageService {
    public func makeLiveEngine(clock: Clock = SystemClock()) -> DirectLiveEngine {
        let engine = DirectLiveEngine(
            selfId: { [weak self] in self?.currentSelfId ?? "" },
            send: { [weak self] conversation, fields in await self?.sendLive(conversation, fields: fields) ?? false },
            interval: { [weak self] conversation in
                self?.liveInterval(conversation) ?? DirectLiveFrames.fallbackIntervalMs
            },
            clock: clock, sleep: sleeper)
        onLiveFrame = { [weak engine] plain, conversation in engine?.receive(plain, conversation: conversation) }
        return engine
    }
}
