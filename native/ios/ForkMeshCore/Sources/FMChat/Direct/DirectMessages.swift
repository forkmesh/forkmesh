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
    public static let typingType = "typing"
    public static let outboxLimit = 64
    public static let outboxMaxAgeMs: Int64 = 7 * 24 * 60 * 60 * 1000
    public static let maxWatchedRooms = 16
    public static let initialReconnectDelay: TimeInterval = 1
    public static let maxReconnectDelay: TimeInterval = 30
    public static let ackTimeout: TimeInterval = 5
    public static let ackRetries = 3
    public static let directAckTimeout: TimeInterval = 3
    public static let resetIntervalMs: Int64 = 60_000
    public static let replyField = "replyTo"

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
        let viewOnce = ViewOnceFrames.parse(plain[ViewOnceFrames.field])
        let text = viewOnce?.kind.fallbackText ?? plain["text"]?.text ?? ""
        if text.count > ChatLimits.maxTextChars { return nil }
        let fileName = viewOnce != nil ? "" : String((plain["fileName"]?.text ?? "").prefix(ChatLimits.maxFileNameChars))
        if text.isEmpty && fileName.isEmpty { return nil }
        let senderId = String((plain["senderId"]?.text ?? "").prefix(64))
        var sender = String((plain["sender"]?.text ?? "").prefix(ChatLimits.maxDisplayNameChars))
        if sender.isEmpty { sender = record.otherUser }
        let ts = plain["ts"]?.doubleValue.map { Date(epochMillis: Int64($0)) } ?? Date(epochMillis: 0)
        return ChatMessage(
            id: id, conversation: record.conversation, senderId: senderId, senderName: sender, text: text,
            timestamp: ts, isSelf: !selfId.isEmpty && senderId == selfId, fileName: fileName,
            replyTo: replyQuote(plain[replyField]))
    }

    public static func replyQuote(_ value: JSONValue?) -> ChatReplyQuote? {
        guard let object = value?.objectValue else { return nil }
        let id = object["id"]?.text ?? ""
        guard !id.isEmpty, id.count <= 64 else { return nil }
        return ChatReplyQuote(
            id: id, senderId: String((object["senderId"]?.text ?? "").prefix(64)),
            senderName: String((object["sender"]?.text ?? "").prefix(ChatLimits.maxDisplayNameChars)),
            text: String((object["text"]?.text ?? "").prefix(ChatReplyQuote.maxTextChars)))
    }

    public static func replyFields(_ quote: ChatReplyQuote) -> JSONValue {
        .object([
            "id": .string(quote.id), "senderId": .string(quote.senderId),
            "sender": .string(String(quote.senderName.prefix(ChatLimits.maxDisplayNameChars))),
            "text": .string(String(quote.text.prefix(ChatReplyQuote.maxTextChars))),
        ])
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
    var mediaChecked = false

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
    @ObservationIgnored let accountName: (@MainActor () -> String)?
    @ObservationIgnored let clock: Clock
    @ObservationIgnored private var sockets: [String: DirectRoomSocket] = [:]
    @ObservationIgnored private var cryptos: [String: RoomCrypto] = [:]
    @ObservationIgnored private let seals: RoomSealCache
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
    @ObservationIgnored private var unacked: Set<String> = []
    @ObservationIgnored public var isExpired: ChatExpiry?
    @ObservationIgnored public var onMessage: (@MainActor (ChatMessage) -> Void)?
    @ObservationIgnored public var onCallFrame: (@MainActor (JSONObject, String) -> Void)?
    @ObservationIgnored public var onLiveFrame: (@MainActor (JSONObject, String) -> Void)?
    @ObservationIgnored private var liveBudgets: [String: (perWindow: Int, windowMs: Int)] = [:]
    @ObservationIgnored public var onFrame: (@MainActor (JSONObject, String) -> Void)?
    @ObservationIgnored public private(set) weak var mesh: LocalMeshService?
    @ObservationIgnored private var focusedConversation: String?
    @ObservationIgnored private var publishedFocus: String?
    @ObservationIgnored private var directAcks: [String: DirectAckWait] = [:]
    @ObservationIgnored private var safetyFlags: [String: Bool] = [:]
    @ObservationIgnored public var onTrustEvent: (@MainActor (DirectTrustEvent) -> Void)?
    @ObservationIgnored public var isRemoved: (@MainActor (String) -> Bool)?
    @ObservationIgnored public var onRemoved: (@MainActor (String, String) -> Void)?
    @ObservationIgnored public var onChange: (@MainActor () -> Void)?
    @ObservationIgnored private var refBooks: [String: DirectRefBook] = [:]
    @ObservationIgnored private var receiptOrder: [String] = []
    @ObservationIgnored private var pendingDeletes: [String: [String: String]] = [:]
    @ObservationIgnored private var pendingDeleteOrder: [String: [String]] = [:]
    @ObservationIgnored private var removedIds: Set<String> = []
    @ObservationIgnored public let viewOnce: ViewOnceStore
    @ObservationIgnored var heldReceipts: [String: (conversationId: String, refs: [String])] = [:]
    @ObservationIgnored var heldOrder: [String] = []
    @ObservationIgnored var resetTimes: [String: Int64] = [:]

    public private(set) var records: [DirectConversation] = []
    public private(set) var nextCursor = ""
    public private(set) var loading = false
    public private(set) var lastError: String?
    public private(set) var connected: Set<String> = []
    public private(set) var serverCopies: [String: Int] = [:]
    public private(set) var receipts: [String: ChatReceipt] = [:]
    public private(set) var trustRevision = 0
    public private(set) var routeRevision = 0
    private var history: [String: [ChatMessage]] = [:]

    public init(
        api: ChatAccountCalling, connector: WebSocketConnector, secureStore: SecureStore,
        origin: @escaping @MainActor () -> URL?, selfId: @escaping @MainActor () -> String,
        displayName: @escaping @MainActor () -> String, clock: Clock = SystemClock(),
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        },
        viewOncePrefs: KeyValueStore? = nil, account: (@MainActor () -> String)? = nil
    ) {
        self.viewOnce = ViewOnceStore(prefs: viewOncePrefs)
        self.accountName = account
        self.api = api
        self.connector = connector
        self.secureStore = secureStore
        seals = RoomSealCache(secureStore: secureStore)
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

    public var historySnapshot: [String: [ChatMessage]] { history }

    public var receiptSnapshot: [String: ChatReceipt] {
        receipts.filter { $0.value == .delivered || $0.value == .read }
    }

    public func restore(_ messages: [ChatMessage], receipts saved: [String: ChatReceipt]) {
        for message in messages where DirectMessageFrames.isDirect(message.conversation) && !isGone(message.id) {
            append(message, conversationId: DirectMessageFrames.conversationId(message.conversation))
        }
        for (id, receipt) in saved where receipt == .delivered || receipt == .read { setReceipt(receipt, id) }
    }

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
                viewOnce.forget(conversation: old.conversation)
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

    public var currentAccount: String {
        let named = (accountName?() ?? displayName()).trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        return named.isEmpty ? selfId() : named
    }

    private func participants(_ conversationId: String) -> Set<String>? {
        guard let other = records.first(where: { $0.id == conversationId })?.otherUser.lowercased(), !other.isEmpty else {
            return nil
        }
        return [currentAccount, other]
    }

    private func seal(for room: String, conversationId: String) async throws -> RoomSeal {
        try await seals.seal(
            room, scope: DirectMessageFrames.prefix + conversationId, policy: .strict, acct: currentAccount,
            node: selfId(), participants: participants(conversationId))
    }

    private func loadedSeal(_ record: DirectConversation) -> RoomSeal? {
        guard let room = roomNames[record.id], let seal = seals[room], (seal.node ?? "") == selfId(),
            (seal.acct ?? "") == currentAccount
        else { return nil }
        return seal
    }

    private func sealFor(_ record: DirectConversation) async -> RoomSeal? {
        if let loaded = loadedSeal(record) { return loaded }
        if let room = roomNames[record.id] { return try? await seal(for: room, conversationId: record.id) }
        let store = secureStore
        let scope = record.conversation
        let room = await Task.detached(priority: .userInitiated) {
            (try? ChatKeyring.shared(store))?.trust(scope).room ?? ""
        }.value
        if room.isEmpty { return nil }
        roomNames[record.id] = room
        return try? await seal(for: room, conversationId: record.id)
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
        publishFocus()
        cancelReconnects()
        if foreground {
            backoff.removeAll()
            Task { [weak self] in
                guard let self else { return }
                for socket in Array(self.sockets.values) { await self.close(socket) }
                self.watchWanted()
            }
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
            let seal = try await seal(for: roomName, conversationId: id)
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
            if receivedRefs[id]?.isEmpty == false { Task { await self.flushReceived(id) } }
            if let record = records.first(where: { $0.id == id }) { processHeld(seal, record: record) }
            trustChanged()
            return true
        } catch {
            lastError = "\(error)"
            return false
        }
    }

    private func receive(_ message: WebSocketMessage, conversationId: String) {
        PerformanceMonitorService.section("chat.direct.receive") { receiveFrame(message, conversationId: conversationId) }
    }

    private func receiveFrame(_ message: WebSocketMessage, conversationId: String) {
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
            if !socket.mediaChecked {
                socket.mediaChecked = true
                let conversation = record.conversation
                Task { await self.refreshViewOnce(conversation) }
            }
            return
        }
        if let update = ViewOnceFrames.relayUpdate(envelope) {
            applyViewOnce(update.id, update.state)
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
            var delivered: Set<String> = []
            for ref in refs {
                if let id = refBooks[conversationId]?.ids[ref] {
                    setReceipt(.delivered, id)
                    delivered.insert(id)
                }
            }
            if !delivered.isEmpty { outbox[conversationId]?.removeAll { delivered.contains($0.messageId) } }
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
        guard !refs.isEmpty else { return }
        guard let connection = sockets[conversationId]?.connection else {
            receivedRefs[conversationId] = refs + (receivedRefs[conversationId] ?? []).filter { !refs.contains($0) }
            return
        }
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
            acceptBundle(frame, record: record, seal: seal, live: live, socket: socket)
            return false
        }
        guard RoomSeal.isSealedFrame(frame) else { return false }
        let kids = RoomSeal.wrappedKeyIds(frame)
        let addressed = kids.contains(seal.keyId) || RoomSeal.frameKeyId(frame) == seal.keyId
        let result = addressed ? seal.open(frame, now: clock.nowMillis, history: !live) : .notForMe
        switch result {
        case .opened(_, let senderKid, _, _):
            guard let plain = result.authenticatedPlain else { return false }
            if isReset(plain) {
                resealForReset(record, seal: seal, kid: senderKid)
                return true
            }
            return handleOpened(plain, kids: kids, envelope: envelope, record: record, live: live)
        case .notForMe:
            if !socket.persistedAdvert {
                socket.persistedAdvert = true
                advertise(socket, persist: true)
            }
            return false
        case .noSession(let kid):
            recoverSession(record, seal: seal, kid: kid, counter: frame["ctr"]?.int64Value)
            return false
        case .held:
            trustChanged()
            return false
        case .replay:
            return true
        case .own:
            if let id = refBooks[record.id]?.ids[DirectMessageFrames.relayRef(envelope)] { setReceipt(.sent, id) }
            return false
        case .invalid:
            return false
        }
    }

    func isReset(_ plain: JSONObject) -> Bool { plain["type"]?.text == ChatV4.resetType }

    func recoverSession(_ record: DirectConversation, seal: RoomSeal, kid: String, counter: Int64? = nil) {
        if let counter, counter < seal.highestCounter(from: kid) { return }
        let key = "\(record.id)|\(kid)"
        let now = clock.nowMillis
        if let last = resetTimes[key], now - last < DirectMessageFrames.resetIntervalMs, now >= last { return }
        resetTimes[key] = now
        seal.resetSessions(with: kid)
        let plain: JSONObject = ["type": .string(ChatV4.resetType), "senderId": .string(selfId())]
        let sealed = try? seal.seal(plain, for: [kid], now: now)
        let socket = sockets[record.id]
        if let socket {
            if let sealed, !RoomSeal.wrappedKeyIds(sealed).isEmpty, var envelope = try? socket.crypto.encrypt(sealed) {
                envelope["persist"] = true
                let text = JSON.encode(envelope)
                let connection = socket.connection
                Task { try? await connection.send(text: text) }
            }
            advertise(socket, persist: true)
        } else if let sealed, !RoomSeal.wrappedKeyIds(sealed).isEmpty, let mesh {
            let conversation = record.conversation
            Task { _ = await mesh.sendDirectSealed(sealed, channel: conversation, to: kid) }
        }
    }

    func resealForReset(_ record: DirectConversation, seal: RoomSeal, kid: String) {
        let now = clock.nowMillis
        let entries = (outbox[record.id] ?? []).filter { now - $0.ts < DirectMessageFrames.outboxMaxAgeMs }
        if entries.isEmpty { return }
        let socket = sockets[record.id]
        var texts: [String] = []
        var direct: [JSONObject] = []
        for entry in entries {
            guard let sealed = try? seal.seal(entry.plain, for: [kid], now: clock.nowMillis),
                !RoomSeal.wrappedKeyIds(sealed).isEmpty
            else { continue }
            guard let socket else {
                direct.append(sealed)
                continue
            }
            guard var envelope = try? socket.crypto.encrypt(sealed) else { continue }
            envelope["persist"] = true
            if entry.notify { envelope["notify"] = true }
            envelope["resend"] = true
            let text = JSON.encode(envelope)
            if DirectMessageFrames.carriesReceipt(entry.plain) {
                refBooks[record.id, default: DirectRefBook()].track(DirectMessageFrames.frameRef(text), id: entry.messageId)
            }
            texts.append(text)
        }
        if let socket, !texts.isEmpty {
            let connection = socket.connection
            Task {
                for text in texts { try? await connection.send(text: text) }
            }
        } else if let mesh, !direct.isEmpty {
            let conversation = record.conversation
            Task {
                for sealed in direct { _ = await mesh.sendDirectSealed(sealed, channel: conversation, to: kid) }
            }
        }
    }

    private func acceptBundle(
        _ frame: JSONObject, record: DirectConversation, seal: RoomSeal, live: Bool, socket: DirectRoomSocket?
    ) {
        guard let result = seal.acceptBundle(frame, now: clock.nowMillis), result.isNew || result.updated else { return }
        if result.isNew && result.state == .pending {
            onTrustEvent?(.pendingDevice(conversation: record.conversation, kid: result.kid))
        }
        processHeld(seal, record: record)
        trustChanged()
        if live, let socket {
            advertise(socket)
            reseal(socket)
        }
    }

    private func processHeld(_ seal: RoomSeal, record: DirectConversation) {
        for result in seal.reopenHeld(now: clock.nowMillis) {
            switch result {
            case .opened(_, let senderKid, _, _):
                guard let plain = result.authenticatedPlain else { continue }
                if isReset(plain) {
                    resealForReset(record, seal: seal, kid: senderKid)
                } else {
                    _ = handleOpened(plain, kids: [], envelope: [:], record: record, live: true)
                }
            case .noSession(let kid):
                recoverSession(record, seal: seal, kid: kid)
            default:
                continue
            }
        }
    }

    @discardableResult
    private func handleOpened(
        _ plain: JSONObject, kids: [String], envelope: JSONObject, record: DirectConversation, live: Bool
    ) -> Bool {
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
        if type == DirectMessageFrames.typingType {
            if live { onFrame?(plain, record.conversation) }
            return false
        }
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
        var held = false
        if type == "chat", let media = ViewOnceFrames.parse(plain[ViewOnceFrames.field]),
            let id = plain["id"]?.text, !id.isEmpty
        {
            let entry = viewOnce.register(
                messageId: id, conversation: record.conversation, media: media,
                mine: !me.isEmpty && plain["senderId"]?.text == me)
            if viewOnce.isSpent(media.media) { forgetOutbox(media: media.media, conversationId: record.id) }
            if viewOnce.holdsReceipt(media: media.media) {
                holdReceipt(entry.messageId, conversationId: record.id, ref: DirectMessageFrames.relayRef(envelope))
                held = true
            }
        }
        guard let message = DirectMessageFrames.chatMessage(plain, record: record, selfId: me) else {
            onFrame?(plain, record.conversation)
            return !held
        }
        let added = append(message, conversationId: record.id)
        onFrame?(plain, record.conversation)
        if added && live && !message.isSelf { onMessage?(message) }
        return !held
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
        if receipt == .delivered || receipt == .read { onChange?() }
    }

    private func forgetReceipt(_ id: String) {
        guard receipts.removeValue(forKey: id) != nil else { return }
        receiptOrder.removeAll { $0 == id }
    }

    private func markReadByPeer(_ conversationId: String) {
        var ids: [String] = (history[DirectMessageFrames.prefix + conversationId] ?? []).filter(\.isSelf).map(\.id)
        ids.append(contentsOf: refBooks[conversationId]?.refs.keys.map { $0 } ?? [])
        for id in ids where receipts[id] != .sending { setReceipt(.read, id) }
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
        onChange?()
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
        var entries = (outbox[id] ?? []).filter {
            now - $0.ts < DirectMessageFrames.outboxMaxAgeMs && receipts[$0.messageId] != .delivered
                && receipts[$0.messageId] != .read
        }
        let peers = Set(socket.seal.peerKeyIds)
        var texts: [String] = []
        var superseded: [String] = []
        for index in entries.indices {
            let stale = !peers.isEmpty && !peers.isSubset(of: Set(entries[index].kids))
            let messageId = entries[index].messageId
            let lost = unacked.remove(messageId) != nil && receipts[messageId] == .sending
            if stale || lost {
                guard let sealed = try? socket.seal.seal(entries[index].plain, now: clock.nowMillis),
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
        onChange?()
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
        if changed { onChange?() }
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
        guard let record = record(for: conversation), let seal = loadedSeal(record) else { return [] }
        return seal.approvedKeyIds
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
    public func send(_ conversation: String, text: String, replyTo: ChatReplyQuote? = nil) async -> Bool {
        let body = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !body.isEmpty, let record = record(for: conversation) else { return false }
        var extra: JSONObject = ["text": .string(String(body.prefix(ChatLimits.maxTextChars)))]
        if let replyTo { extra[DirectMessageFrames.replyField] = DirectMessageFrames.replyFields(replyTo) }
        let plain = frame(record, type: "chat", extra: extra)
        return await sendSealed(record, plain)
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
        return await sendSealed(record, plain, fileBytes: data.count)
    }

    @discardableResult
    public func sendLook(_ conversation: String, change: ChatLookChange) async -> Bool {
        guard let record = record(for: conversation) else { return false }
        let plain = frame(record, type: ChatLookFrames.type, extra: ChatLookFrames.fields(change))
        return await sendSealed(record, plain, notify: false)
    }

    public func liveInterval(_ conversation: String) -> Int64 {
        guard let budget = liveBudgets[DirectMessageFrames.conversationId(conversation)] else {
            return DirectLiveFrames.fallbackIntervalMs
        }
        return DirectLiveFrames.interval(perWindow: budget.perWindow, windowMs: budget.windowMs)
    }

    @discardableResult
    public func sendLive(_ conversation: String, fields: JSONObject) async -> Bool {
        guard let record = record(for: conversation) else { return false }
        let plain = frame(record, type: DirectLiveFrames.type, extra: fields)
        let plan = await directPlan(record, fileBytes: 0)
        let reached = await sendDirect(record, plain, to: plan.kids)
        if plan.directOnly && reached > 0 { return true }
        guard let socket = sockets[record.id] else { return reached > 0 }
        do {
            var envelope = try socket.crypto.encrypt(try socket.seal.seal(plain, now: clock.nowMillis))
            envelope["live"] = true
            let text = JSON.encode(envelope)
            if text.utf8.count > DirectLiveFrames.maxEnvelopeBytes { return reached > 0 }
            try await socket.connection.send(text: text)
            readvertiseIfRotated(socket)
            return true
        } catch {
            return reached > 0
        }
    }

    private func sendSealed(_ record: DirectConversation, _ plain: JSONObject, notify: Bool = true, fileBytes: Int = 0)
        async -> Bool
    {
        let id = plain["id"]?.text ?? ""
        let receipted = DirectMessageFrames.carriesReceipt(plain)
        var shown = false
        if let message = DirectMessageFrames.chatMessage(plain, record: record, selfId: selfId()) {
            shown = append(message, conversationId: record.id)
        }
        if receipted { setReceipt(.sending, id) }
        let plan = await directPlan(record, fileBytes: fileBytes)
        let reached = await sendDirect(record, plain, to: plan.kids)
        if plan.directOnly && reached > 0 {
            if receipted { setReceipt(.sent, id) }
            if !id.isEmpty {
                directAcks[id] = DirectAckWait(
                    conversationId: record.id, plain: plain, notify: notify, kids: Set(plan.kids))
                watchDirectAck(id)
            }
            if plain["type"]?.text == "delete" { applyDelete(plain, record: record, stash: false) }
            onFrame?(plain, record.conversation)
            return true
        }
        return await sendRelay(record, plain, notify: notify, reached: reached, shown: shown)
    }

    private func sendRelay(
        _ record: DirectConversation, _ plain: JSONObject, notify: Bool, reached: Int, shown: Bool, local: Bool = true
    ) async -> Bool {
        let id = plain["id"]?.text ?? ""
        let receipted = DirectMessageFrames.carriesReceipt(plain)
        if sockets[record.id] == nil, !(await open(record.conversation)) {
            return settleUnsent(record, plain, shown: shown, reached: reached)
        }
        guard let socket = sockets[record.id] else { return settleUnsent(record, plain, shown: shown, reached: reached) }
        do {
            let sealed = try socket.seal.seal(plain, now: clock.nowMillis)
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
            if receipted { watchAck(id, conversationId: record.id, connection: socket.connection, attempt: 0) }
            if local {
                if plain["type"]?.text == "delete" { applyDelete(plain, record: record, stash: false) }
                onFrame?(plain, record.conversation)
            }
            readvertiseIfRotated(socket)
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
        return await sendSealed(record, plain, notify: type == "thread-reply") ? plain : nil
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
    public func sendTyping(_ conversation: String, active: Bool) async -> Bool {
        guard let record = record(for: conversation) else { return false }
        var plain = frame(record, type: DirectMessageFrames.typingType, extra: ["active": .bool(active)])
        plain.removeValue(forKey: "channel")
        plain["conversation"] = .string(record.conversation)
        let plan = await directPlan(record, fileBytes: 0)
        let reached = await sendDirect(record, plain, to: plan.kids)
        if plan.directOnly && reached > 0 { return true }
        guard let socket = sockets[record.id] else { return reached > 0 }
        do {
            let envelope = try socket.crypto.encrypt(try socket.seal.seal(plain, now: clock.nowMillis))
            try await socket.connection.send(text: JSON.encode(envelope))
            return true
        } catch {
            return reached > 0
        }
    }

    @discardableResult
    public func sendCallFrame(_ conversation: String, type: String, extra: JSONObject) async -> Bool {
        guard type.hasPrefix("call-"), let record = record(for: conversation), let socket = sockets[record.id]
        else { return false }
        let plain = frame(record, type: type, extra: extra)
        do {
            let envelope = try socket.crypto.encrypt(try socket.seal.seal(plain, now: clock.nowMillis))
            try await socket.connection.send(text: JSON.encode(envelope))
            return true
        } catch {
            return false
        }
    }

    private func advertise(_ socket: DirectRoomSocket, persist: Bool = false) {
        let bundle = socket.seal.keyFrame(now: clock.nowMillis)
        guard !bundle.isEmpty, var envelope = try? socket.crypto.encrypt(bundle) else { return }
        if persist { envelope["persist"] = true }
        let connection = socket.connection
        Task { try? await connection.send(text: JSON.encode(envelope)) }
    }

    private func watchAck(_ id: String, conversationId: String, connection: WebSocketConnection, attempt: Int) {
        let sleeper = self.sleeper
        Task { [weak self] in
            do { try await sleeper(DirectMessageFrames.ackTimeout) } catch { return }
            guard let self, self.receipts[id] == .sending,
                let socket = self.sockets[conversationId], socket.connection === connection
            else { return }
            self.flagUnacked(conversationId)
            await self.close(socket)
            guard await self.connect(conversationId), attempt + 1 < DirectMessageFrames.ackRetries,
                let fresh = self.sockets[conversationId]
            else { return }
            self.watchAck(id, conversationId: conversationId, connection: fresh.connection, attempt: attempt + 1)
        }
    }

    private func flagUnacked(_ conversationId: String) {
        for entry in outbox[conversationId] ?? [] where receipts[entry.messageId] == .sending {
            unacked.insert(entry.messageId)
        }
    }

    private func dropped(_ conversationId: String, _ connection: WebSocketConnection) {
        guard let socket = sockets[conversationId], socket.connection === connection else { return }
        flagUnacked(conversationId)
        sockets.removeValue(forKey: conversationId)
        connected.remove(conversationId)
        serverCopies.removeValue(forKey: conversationId)
        scheduleReconnect(conversationId)
    }

    private func close(_ socket: DirectRoomSocket) async {
        socket.reader?.cancel()
        sockets.removeValue(forKey: socket.conversationId)
        connected.remove(socket.conversationId)
        serverCopies.removeValue(forKey: socket.conversationId)
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
        unacked = []
        liveBudgets = [:]
        receipts = [:]
        receiptOrder = []
        refBooks = [:]
        pendingDeletes = [:]
        pendingDeleteOrder = [:]
        removedIds = []
        viewOnce.clear()
        heldReceipts = [:]
        heldOrder = []
        directAcks = [:]
        focusedConversation = nil
        publishedFocus = nil
        safetyFlags = [:]
        resetTimes = [:]
        roomNames = [:]
        seals.removeAll()
        trustChanged()
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
    public func attachMesh(_ mesh: LocalMeshService) {
        self.mesh = mesh
        mesh.onDirectSealed = { [weak self] channel, frame, kid in
            guard let self else { return }
            Task { await self.receiveDirect(channel: channel, frame: frame, from: kid) }
        }
        mesh.onDirectAck = { [weak self] channel, id, kid in self?.directAcked(channel: channel, id: id, from: kid) }
        mesh.onDirectFocus = { [weak self] _, _, _ in self?.routeChanged() }
        mesh.onLinkAuthenticated = { [weak self] kid in self?.linkAuthenticated(kid) }
        mesh.onLinkClosed = { [weak self] kid in self?.linkClosed(kid) }
        routeChanged()
    }

    func directPlan(_ record: DirectConversation, fileBytes: Int) async -> (kids: [String], directOnly: Bool) {
        guard let mesh, mesh.running else { return ([], false) }
        let linked = mesh.directLinks()
        if linked.isEmpty { return ([], false) }
        guard let seal = await sealFor(record) else { return ([], false) }
        let me = currentAccount
        var kids: [String] = []
        var directOnly = false
        let focused = hasFocus(record)
        for device in seal.trust.records(in: .approved) where device.kid != seal.keyId {
            guard let kind = linked[device.kid] else { continue }
            if fileBytes > 0 {
                let limit =
                    kind == .wifi
                    ? LocalMeshService.directFileBytesOverWifi : LocalMeshService.directFileBytesOverBluetooth
                if fileBytes > limit { continue }
            }
            kids.append(device.kid)
            if focused && device.acct != me && mesh.peerFocus(device.kid, channel: record.conversation) {
                directOnly = true
            }
        }
        return (kids, directOnly && !kids.isEmpty)
    }

    func sendDirect(_ record: DirectConversation, _ plain: JSONObject, to kids: [String]) async -> Int {
        guard let mesh, !kids.isEmpty, let seal = await sealFor(record) else { return 0 }
        var reached = 0
        for kid in kids {
            guard let sealed = try? seal.seal(plain, for: [kid], now: clock.nowMillis) else { continue }
            if await mesh.sendDirectSealed(sealed, channel: record.conversation, to: kid) { reached += 1 }
        }
        if let socket = sockets[record.id] { readvertiseIfRotated(socket) }
        return reached
    }

    func readvertiseIfRotated(_ socket: DirectRoomSocket) {
        if socket.seal.needsAdvertisement(now: clock.nowMillis) { advertise(socket) }
    }

    func receiveDirect(channel: String, frame: JSONObject, from kid: String) async {
        guard let record = record(for: channel), RoomSeal.frameKeyId(frame) == kid, let seal = await sealFor(record),
            seal.trust.record(kid) != nil
        else { return }
        let result = seal.open(frame, now: clock.nowMillis)
        switch result {
        case .opened:
            guard let plain = result.authenticatedPlain else { return }
            if isReset(plain) {
                resealForReset(record, seal: seal, kid: kid)
                return
            }
            _ = handleOpened(plain, kids: [], envelope: [:], record: record, live: true)
            let id = plain["id"]?.text ?? ""
            let type = plain["type"]?.text ?? ""
            if !id.isEmpty && type != DirectMessageFrames.typingType && type != DirectLiveFrames.type {
                await mesh?.sendDirectAck(channel: record.conversation, id: id, to: kid)
            }
        case .held:
            trustChanged()
        case .noSession(let sender):
            recoverSession(record, seal: seal, kid: sender, counter: frame["ctr"]?.int64Value)
        default:
            break
        }
    }

    func directAcked(channel: String, id: String, from kid: String) {
        if let wait = directAcks[id] {
            guard wait.kids.contains(kid), records.first(where: { $0.id == wait.conversationId })?.conversation == channel
            else { return }
            directAcks.removeValue(forKey: id)
            if DirectMessageFrames.carriesReceipt(wait.plain) { setReceipt(.delivered, id) }
            return
        }
        guard let record = record(for: channel), loadedSeal(record)?.trust.record(kid)?.state == .approved,
            messages(channel).contains(where: { $0.id == id && $0.isSelf }), receipts[id] != nil
        else { return }
        setReceipt(.delivered, id)
    }

    func watchDirectAck(_ id: String) {
        let sleeper = self.sleeper
        Task { [weak self] in
            do { try await sleeper(DirectMessageFrames.directAckTimeout) } catch { return }
            await self?.fallBackToRelay(id)
        }
    }

    func fallBackToRelay(_ id: String) async {
        guard let wait = directAcks.removeValue(forKey: id),
            let record = records.first(where: { $0.id == wait.conversationId })
        else { return }
        _ = await sendRelay(record, wait.plain, notify: wait.notify, reached: 1, shown: false, local: false)
    }

    func linkAuthenticated(_ kid: String) {
        routeChanged()
        guard let conversation = publishedFocus, let record = record(for: conversation), let mesh else { return }
        Task { [weak self] in
            guard let self, let seal = await self.sealFor(record), seal.trust.record(kid)?.state == .approved else { return }
            await mesh.sendFocus(channel: record.conversation, on: true, to: kid)
        }
    }

    func linkClosed(_ kid: String) {
        routeChanged()
        let linked = mesh?.directLinks() ?? [:]
        let stranded = directAcks.filter { entry in
            entry.value.kids.contains(kid) && !entry.value.kids.contains { other in linked[other] != nil }
        }
        for id in stranded.keys { Task { await self.fallBackToRelay(id) } }
    }

    public func setFocus(_ conversation: String, _ on: Bool) {
        if on {
            focusedConversation = conversation
        } else if focusedConversation == conversation {
            focusedConversation = nil
        }
        publishFocus()
    }

    public var focus: String? { foregroundFocus }

    private var foregroundFocus: String? { foreground ? focusedConversation : nil }

    func hasFocus(_ record: DirectConversation) -> Bool { foregroundFocus == record.conversation }

    func publishFocus() {
        let next = foregroundFocus
        if next == publishedFocus { return }
        let previous = publishedFocus
        publishedFocus = next
        if let previous { sendFocus(previous, false) }
        if let next { sendFocus(next, true) }
        routeChanged()
    }

    private func sendFocus(_ conversation: String, _ on: Bool) {
        guard let mesh, let record = record(for: conversation) else { return }
        Task { [weak self] in
            guard let self, let seal = await self.sealFor(record) else { return }
            let linked = mesh.directLinks()
            for device in seal.trust.records(in: .approved) where device.kid != seal.keyId && linked[device.kid] != nil {
                await mesh.sendFocus(channel: record.conversation, on: on, to: device.kid)
            }
        }
    }

    public func route(_ conversation: String) -> DirectRoute {
        _ = routeRevision
        _ = trustRevision
        guard let mesh, mesh.running, let record = record(for: conversation), let seal = loadedSeal(record) else {
            return .none
        }
        let linked = mesh.directLinks()
        let me = currentAccount
        var kinds: Set<MeshLinkKind> = []
        var directOnly = false
        for device in seal.trust.records(in: .approved) where device.kid != seal.keyId {
            guard let kind = linked[device.kid] else { continue }
            kinds.insert(kind)
            if hasFocus(record) && device.acct != me && mesh.peerFocus(device.kid, channel: conversation) {
                directOnly = true
            }
        }
        if kinds.isEmpty { return .none }
        if directOnly { return .directOnly }
        return kinds.contains(.wifi) ? .wifi : .bluetooth
    }

    func routeChanged() {
        routeRevision &+= 1
    }

    func trustChanged() {
        trustRevision &+= 1
        for record in records {
            guard let seal = loadedSeal(record) else { continue }
            let changed = seal.trust.safetyNumberChanged
            let before = safetyFlags[record.id] ?? false
            safetyFlags[record.id] = changed
            if changed && !before { onTrustEvent?(.safetyNumberChanged(conversation: record.conversation)) }
        }
        routeChanged()
    }

    public func devices(_ conversation: String) -> [ChatDeviceInfo] {
        _ = trustRevision
        guard let record = record(for: conversation), let seal = loadedSeal(record) else { return [] }
        let me = currentAccount
        return seal.trust.all.map { ChatDeviceInfo($0, ownKid: seal.keyId, ownAcct: me) }
    }

    public func pendingDevices(_ conversation: String) -> [ChatDeviceInfo] {
        devices(conversation).filter { $0.state == .pending }
    }

    public func approveDevice(_ conversation: String, kid: String) async {
        guard let record = record(for: conversation), let seal = await sealFor(record), seal.approve(kid) else { return }
        processHeld(seal, record: record)
        if let socket = sockets[record.id] { reseal(socket) }
        trustChanged()
    }

    public func blockDevice(_ conversation: String, kid: String) async {
        guard let record = record(for: conversation), let seal = await sealFor(record), kid != seal.keyId,
            seal.block(kid)
        else { return }
        trustChanged()
    }

    public func safetyNumber(_ conversation: String) -> ChatSafetyNumber? {
        _ = trustRevision
        guard let record = record(for: conversation), let seal = loadedSeal(record) else { return nil }
        return seal.safetyNumber()
    }

    public func safetyNumberChanged(_ conversation: String) -> Bool {
        _ = trustRevision
        guard let record = record(for: conversation), let seal = loadedSeal(record) else { return false }
        return seal.trust.safetyNumberChanged
    }

    public func markVerified(_ conversation: String) {
        guard let record = record(for: conversation), let seal = loadedSeal(record) else { return }
        seal.trust.markVerified()
        trustChanged()
    }

    @discardableResult
    public func verifySafetyCode(_ conversation: String, scanned: String) -> Bool {
        guard let record = record(for: conversation), let seal = loadedSeal(record) else { return false }
        let verified = seal.trust.verify(scanned: scanned)
        if verified { trustChanged() }
        return verified
    }

    public func acknowledgeSafetyChange(_ conversation: String) {
        guard let record = record(for: conversation), let seal = loadedSeal(record) else { return }
        seal.trust.acknowledgeSafetyChange()
        trustChanged()
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

extension DirectMessageService {
    func viewOnceTransfer(_ record: DirectConversation) -> ViewOnceTransfer {
        ViewOnceTransfer(api: api, conversationId: record.id)
    }

    func applyViewOnce(_ media: String, _ state: ViewOnceState) {
        viewOnce.apply(media: media, state: state)
        if state.isTerminal, let entry = viewOnce.entry(media: media),
            let id = records.first(where: { $0.conversation == entry.conversation })?.id
        {
            forgetOutbox(media: media, conversationId: id)
            if !viewOnce.holdsReceipt(media: media) { releaseReceipt(entry.messageId) }
        }
    }

    public var heldReceiptCount: Int { heldReceipts.count }

    func holdReceipt(_ messageId: String, conversationId: String, ref: String) {
        guard !ref.isEmpty else { return }
        var held = heldReceipts[messageId] ?? (conversationId, [])
        if !held.refs.contains(ref) { held.refs.append(ref) }
        if heldReceipts[messageId] == nil { heldOrder.append(messageId) }
        heldReceipts[messageId] = held
        let limit = DirectMessageFrames.maxMessagesPerRoom * 4
        while heldOrder.count > limit { heldReceipts.removeValue(forKey: heldOrder.removeFirst()) }
    }

    func releaseReceipt(_ messageId: String) {
        guard let held = heldReceipts[messageId], let connection = sockets[held.conversationId]?.connection else { return }
        heldReceipts.removeValue(forKey: messageId)
        heldOrder.removeAll { $0 == messageId }
        Task {
            for start in stride(from: 0, to: held.refs.count, by: DirectMessageFrames.receivedRefsPerFrame) {
                let chunk = Array(
                    held.refs[start..<min(start + DirectMessageFrames.receivedRefsPerFrame, held.refs.count)])
                try? await connection.send(text: JSON.encode(DirectMessageFrames.receivedFrame(chunk)))
            }
        }
    }

    func forgetOutbox(media: String, conversationId: String) {
        guard var entries = outbox[conversationId] else { return }
        entries.removeAll { $0.plain[ViewOnceFrames.field]?.objectValue?["media"]?.text == media }
        outbox[conversationId] = entries.isEmpty ? nil : entries
    }

    public func refreshViewOnce(_ conversation: String) async {
        guard let record = record(for: conversation) else { return }
        let transfer = viewOnceTransfer(record)
        for media in viewOnce.pendingConsume.sorted() {
            guard let entry = viewOnce.entry(media: media), entry.conversation == conversation else { continue }
            do {
                _ = try await transfer.consume(media)
            } catch ViewOnceError.gone {
            } catch {
                continue
            }
            viewOnce.setConsumePending(media, false)
            releaseReceipt(entry.messageId)
        }
        let ids = viewOnce.unresolved(conversation)
        guard !ids.isEmpty, let statuses = try? await transfer.statuses(ids) else { return }
        for status in statuses { applyViewOnce(status.id, status.state) }
    }

    public func sendViewOnce(
        _ conversation: String, kind: ViewOnceKind, mime: String, data: Data, durationMs: Int64,
        progress: (@MainActor (Double) -> Void)? = nil
    ) async throws {
        guard let record = record(for: conversation) else { throw ViewOnceError.unavailable }
        if data.isEmpty { throw ViewOnceError.empty }
        if data.count + ViewOnceFrames.tagBytes > kind.maxBytes { throw ViewOnceError.tooLarge }
        if durationMs < 0 || durationMs > kind.maxDurationMs { throw ViewOnceError.tooLong }
        let type = mime.lowercased()
        let sealed = try ViewOnceCrypto.seal(Array(data))
        let transfer = viewOnceTransfer(record)
        let mediaId = try await transfer.upload(kind: kind, body: sealed.body, durationMs: durationMs, progress: progress)
        let media = ViewOnceMedia(
            kind: kind, media: mediaId, key: sealed.key, iv: sealed.iv,
            mime: type.hasPrefix(kind.mimePrefix) ? type : kind.defaultMime, durationMs: durationMs,
            size: sealed.body.count, parts: ViewOnceFrames.parts(size: sealed.body.count))
        let plain = frame(
            record, type: "chat",
            extra: ["text": .string(kind.fallbackText), ViewOnceFrames.field: ViewOnceFrames.build(media)])
        let messageId = plain["id"]?.text ?? ""
        viewOnce.register(messageId: messageId, conversation: record.conversation, media: media, mine: true)
        guard await sendSealed(record, plain) else {
            viewOnce.apply(media: mediaId, state: .failed)
            _ = try? await transfer.withdraw(mediaId)
            throw ViewOnceError.unavailable
        }
    }

    public func playViewOnce(_ messageId: String, progress: (@MainActor (Double) -> Void)? = nil) async throws
        -> ViewOnceClip
    {
        guard let entry = viewOnce.entry(messageId) else { throw ViewOnceError.unavailable }
        guard !entry.mine else { throw ViewOnceError.refused("recipient_only") }
        guard entry.state == .ready, let material = entry.material else { throw ViewOnceError.gone(entry.state) }
        guard let record = record(for: entry.conversation) else { throw ViewOnceError.unavailable }
        let transfer = viewOnceTransfer(record)
        viewOnce.setBusy(messageId, true)
        defer { viewOnce.setBusy(messageId, false) }
        do {
            let plain = try await transfer.download(material, progress: progress)
            return ViewOnceClip(
                messageId: messageId, kind: entry.kind, mime: entry.mime, durationMs: entry.durationMs, data: Data(plain))
        } catch ViewOnceError.gone {
            let state = (try? await transfer.status(material.media))?.state ?? .expired
            let final: ViewOnceState = state.isTerminal ? state : .expired
            applyViewOnce(material.media, final)
            throw ViewOnceError.gone(final)
        }
    }

    public func consumeViewOnce(_ messageId: String) async {
        guard let entry = viewOnce.entry(messageId), !entry.mine,
            let record = record(for: entry.conversation)
        else { return }
        let media = entry.mediaId
        viewOnce.setConsumePending(media, true)
        applyViewOnce(media, .opened)
        do {
            _ = try await viewOnceTransfer(record).consume(media)
        } catch ViewOnceError.gone {
        } catch {
            return
        }
        viewOnce.setConsumePending(media, false)
        releaseReceipt(messageId)
    }

    public func withdrawViewOnce(_ messageId: String) async throws {
        guard let entry = viewOnce.entry(messageId), entry.mine, !entry.state.isTerminal,
            let record = record(for: entry.conversation)
        else { throw ViewOnceError.unavailable }
        let state = try await viewOnceTransfer(record).withdraw(entry.mediaId)
        applyViewOnce(entry.mediaId, state.isTerminal ? state : .withdrawn)
    }
}
