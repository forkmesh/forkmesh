import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMNet
import Foundation
import Observation

@MainActor
public protocol ChatAccountCalling: AnyObject {
    func accountApiCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject
}

extension ApiService: ChatAccountCalling {}

public struct OfficeRoom: Equatable, Sendable, Identifiable {
    public let id: String
    public let name: String
    public let isPrivate: Bool
    public let canManage: Bool
    public let topic: String
    public let members: [String]
    public let keyVersion: Int
    public var serverVisible = false
    public var readOnly = false

    public var conversation: String { OfficeFrames.conversation(for: name) }
}

public enum OfficeFrames {
    public static let prefix = "#office/"
    public static let maxRooms = 100
    public static let maxLiveRooms = 8
    public static let maxTopicChars = 200
    public static let maxMessagesPerRoom = 200
    public static let maxPendingRefs = 200
    public static let recipientsKind = "forkmesh-recipients"

    public static func conversation(for name: String) -> String {
        let trimmed = String(name.trimmingCharacters(in: .whitespacesAndNewlines).prefix(ChatLimits.maxDisplayNameChars))
        return trimmed.isEmpty ? "" : prefix + trimmed
    }

    public static func isOffice(_ conversation: String) -> Bool { conversation.hasPrefix(prefix) }

    public static func validChannelId(_ id: String) -> Bool {
        id.count == 32 && id.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    public static func validName(_ name: String) -> Bool {
        let chars = Array(name)
        guard (1...40).contains(chars.count) else { return false }
        let ok: (Character) -> Bool = { ($0.isLowercase && $0.isLetter && $0.isASCII) || ($0.isNumber && $0.isASCII) }
        guard ok(chars.first!), ok(chars.last!) else { return false }
        return chars.allSatisfy { ok($0) || $0 == "-" }
    }

    public static func rooms(from payload: JSONObject) -> [OfficeRoom] {
        var out: [OfficeRoom] = []
        var seen = Set<String>()
        for channel in payload["channels"]?.objects ?? [] {
            if out.count >= maxRooms { break }
            let id = channel["id"]?.text ?? ""
            guard validChannelId(id) else { continue }
            let name = String((channel["name"]?.text ?? "").prefix(ChatLimits.maxDisplayNameChars))
            let conversation = conversation(for: name)
            if conversation.isEmpty || seen.contains(conversation) { continue }
            seen.insert(conversation)
            let isPrivate = (channel["visibility"]?.text ?? "") == "private"
            var members: [String] = []
            if isPrivate {
                for raw in channel["members"]?.strings ?? [] {
                    let member = String(raw.trimmingCharacters(in: .whitespaces).lowercased().prefix(ChatLimits.maxDisplayNameChars))
                    if !member.isEmpty && !members.contains(member) { members.append(member) }
                }
            }
            let topic = (channel["topic"]?.text ?? "").split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
            out.append(
                OfficeRoom(
                    id: id, name: name, isPrivate: isPrivate, canManage: channel["canManage"]?.isTrue == true,
                    topic: String(topic.prefix(maxTopicChars)), members: members,
                    keyVersion: max(1, channel["keyVersion"]?.intValue ?? 1),
                    serverVisible: channel["serverVisible"]?.isTrue == true, readOnly: channel["readOnly"]?.isTrue == true))
        }
        return out
    }

    public static func recipients(_ envelope: JSONObject) -> Int? {
        guard case .int(let count)? = envelope["recipients"], count >= 0, count <= Int64(Int32.max) else { return nil }
        return Int(count)
    }

    public static func sentRef(_ text: String) -> String {
        String(Sha256.hex(Array(text.utf8)).prefix(40))
    }

    public static func recipientsAck(_ envelope: JSONObject) -> (ref: String, recipients: Int)? {
        guard (envelope["kind"]?.text ?? "") == recipientsKind, let count = recipients(envelope) else { return nil }
        let ref = envelope["ref"]?.text ?? ""
        guard ref.count == 40 else { return nil }
        return (ref, count)
    }

    public static func recipientsLabel(_ count: Int?) -> String? {
        guard let count, count >= 0 else { return nil }
        return "to \(count)"
    }

    public static func recipientsDescription(_ count: Int?) -> String? {
        guard let count, count >= 0 else { return nil }
        return count == 1 ? "Delivered to 1 person" : "Delivered to \(count) people"
    }

    public static func chatMessage(
        _ plain: JSONObject, conversation: String, selfId: String, recipients: Int? = nil, channelId: String = ""
    ) -> ChatMessage? {
        if conversation.isEmpty || (plain["type"]?.text ?? "") != "chat" { return nil }
        let id = plain["id"]?.text ?? ""
        if id.isEmpty || id.count > 64 { return nil }
        let rawText = plain["text"]?.text ?? ""
        if rawText.count > ChatLimits.maxTextChars { return nil }
        let senderId = String((plain["senderId"]?.text ?? "").prefix(64))
        var sender = String((plain["sender"]?.text ?? "").prefix(ChatLimits.maxDisplayNameChars))
        if sender.isEmpty { sender = "Office visitor" }
        let fileName = String((plain["fileName"]?.text ?? "").prefix(ChatLimits.maxFileNameChars))
        if rawText.isEmpty && fileName.isEmpty { return nil }
        let ts = plain["ts"]?.doubleValue.map { Date(epochMillis: Int64($0)) } ?? Date(epochMillis: 0)
        return ChatMessage(
            id: id, conversation: conversation, senderId: senderId, senderName: sender, text: rawText, timestamp: ts,
            isSelf: !selfId.isEmpty && senderId == selfId, fileName: fileName, recipients: recipients,
            app: AppsChat.message(plain, channelId: channelId))
    }

    public static func webSocketURL(_ path: String, origin: URL) -> URL? {
        guard !path.isEmpty, let resolved = URL(string: path, relativeTo: origin)?.absoluteURL,
            var parts = URLComponents(url: resolved, resolvingAgainstBaseURL: false)
        else { return nil }
        switch parts.scheme?.lowercased() {
        case "https": parts.scheme = "wss"
        case "http": parts.scheme = "ws"
        case "wss", "ws": break
        default: return nil
        }
        guard parts.host == origin.host else { return nil }
        return parts.url
    }

    public static func errorMessage(_ code: String) -> String {
        switch code {
        case "invalid_channel_name": return "Use 1 to 40 lowercase letters, digits or dashes."
        case "channel_name_taken": return "A channel with that name already exists."
        case "too_many_channels": return "This project already has the most channels it can hold."
        case "manager_required": return "Only the channel's creator or an administrator can change it."
        case "user_not_found": return "One of those people has no account here."
        case "members_not_allowed": return "Public channels have no member list."
        case "not_found": return "That channel is gone."
        default: return code.isEmpty ? "The server refused the change." : code.replacingOccurrences(of: "_", with: " ")
        }
    }
}

public enum OfficeError: Error, Equatable, CustomStringConvertible {
    case refused(String)
    case unavailable

    public var description: String {
        switch self {
        case .refused(let code): return OfficeFrames.errorMessage(code)
        case .unavailable: return "The office room could not be reached."
        }
    }
}

@MainActor
final class OfficeRoomSocket {
    let roomId: String
    let crypto: RoomCrypto
    let seal: RoomSeal
    let connection: WebSocketConnection
    var reader: Task<Void, Never>?

    init(roomId: String, crypto: RoomCrypto, seal: RoomSeal, connection: WebSocketConnection) {
        self.roomId = roomId
        self.crypto = crypto
        self.seal = seal
        self.connection = connection
    }
}

@MainActor
@Observable
public final class OfficeChannelService {
    @ObservationIgnored let api: ChatAccountCalling
    @ObservationIgnored let connector: WebSocketConnector
    @ObservationIgnored let secureStore: SecureStore
    @ObservationIgnored let origin: @MainActor () -> URL?
    @ObservationIgnored let selfId: @MainActor () -> String
    @ObservationIgnored let displayName: @MainActor () -> String
    @ObservationIgnored let clock: Clock
    @ObservationIgnored private var sockets: [String: OfficeRoomSocket] = [:]
    @ObservationIgnored private var cryptos: [String: RoomCrypto] = [:]
    @ObservationIgnored private var seals: [String: RoomSeal] = [:]
    @ObservationIgnored private var lastTs: [String: Int64] = [:]
    @ObservationIgnored private var seen: [String: Set<String>] = [:]
    @ObservationIgnored private var pendingRefs: [String: String] = [:]
    @ObservationIgnored private var pendingOrder: [String] = []
    @ObservationIgnored private var ackedCounts: [String: Int] = [:]
    @ObservationIgnored private var sequence = 0
    @ObservationIgnored public var isExpired: ChatExpiry?
    @ObservationIgnored public var onFrame: (@MainActor (JSONObject, String) -> Void)?
    @ObservationIgnored public var onMessage: (@MainActor (ChatMessage) -> Void)?
    @ObservationIgnored public var isRemoved: (@MainActor (String) -> Bool)?
    @ObservationIgnored public var onRemoved: (@MainActor (String, String) -> Void)?
    @ObservationIgnored private var pendingDeletes: [String: String] = [:]
    @ObservationIgnored private var pendingDeleteOrder: [String] = []
    @ObservationIgnored private var removedIds: Set<String> = []

    public private(set) var rooms: [OfficeRoom] = []
    public private(set) var loading = false
    public private(set) var lastError: String?
    public private(set) var connected: Set<String> = []
    public private(set) var receipts: [String: ChatReceipt] = [:]
    private var history: [String: [ChatMessage]] = [:]

    public init(
        api: ChatAccountCalling, connector: WebSocketConnector, secureStore: SecureStore,
        origin: @escaping @MainActor () -> URL?, selfId: @escaping @MainActor () -> String,
        displayName: @escaping @MainActor () -> String, clock: Clock = SystemClock()
    ) {
        self.api = api
        self.connector = connector
        self.secureStore = secureStore
        self.origin = origin
        self.selfId = selfId
        self.displayName = displayName
        self.clock = clock
    }

    public var conversations: [String] { rooms.map(\.conversation) }

    public func room(for conversation: String) -> OfficeRoom? { rooms.first { $0.conversation == conversation } }

    public func messages(_ conversation: String) -> [ChatMessage] { history[conversation] ?? [] }

    public func receipt(_ messageId: String) -> ChatReceipt? { receipts[messageId] }

    public func isConnected(_ conversation: String) -> Bool {
        guard let room = room(for: conversation) else { return false }
        return connected.contains(room.id)
    }

    public func refresh() async {
        loading = true
        defer { loading = false }
        do {
            let payload = try await api.accountApiCall("GET", "/api/chat/channels", nil)
            let next = OfficeFrames.rooms(from: payload)
            let live = Set(next.map(\.id))
            for (id, socket) in sockets where !live.contains(id) {
                await close(socket)
            }
            for old in rooms where !live.contains(old.id) { history.removeValue(forKey: old.conversation) }
            for room in next {
                if let previous = rooms.first(where: { $0.id == room.id }), previous.conversation != room.conversation {
                    history[room.conversation] = (history.removeValue(forKey: previous.conversation) ?? []).map {
                        var moved = $0
                        moved.conversation = room.conversation
                        return moved
                    }
                }
            }
            rooms = next
            lastError = nil
        } catch {
            lastError = "\(error)"
        }
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
        let loaded = try RoomSeal.loadOrCreate(room, storageScope: "office|\(scope)", secureStore: secureStore)
        seals[room] = loaded
        return loaded
    }

    public func loadHistory(_ conversation: String) async {
        guard let room = room(for: conversation) else { return }
        do {
            let since = lastTs[room.id] ?? 0
            let payload = try await api.accountApiCall("GET", "/api/chat/channels/\(room.id)/history?since=\(since)", nil)
            let roomName = payload["room"]?.text ?? ""
            let passphrase = payload["passphrase"]?.text ?? ""
            if roomName.isEmpty || passphrase.isEmpty { return }
            let crypto = await derive(roomName, passphrase)
            let seal = try seal(for: roomName)
            for entry in payload["messages"]?.objects ?? [] {
                let ts = entry["ts"]?.int64Value ?? 0
                lastTs[room.id] = max(lastTs[room.id] ?? 0, ts)
                guard let envelope = JSON.parseObject(entry["body"]?.text ?? "") as JSONObject?, !envelope.isEmpty,
                    let plain = crypto.decrypt(envelope)
                else { continue }
                accept(plain, room: room, seal: seal, live: false, recipients: OfficeFrames.recipients(envelope))
            }
        } catch {
            lastError = "\(error)"
        }
    }

    private func accept(_ frame: JSONObject, room: OfficeRoom, seal: RoomSeal, live: Bool, recipients: Int? = nil) {
        var plain = frame
        if RoomSeal.isKeyFrame(plain) {
            if seal.acceptKeyFrame(plain), live, let socket = sockets[room.id] { advertise(socket) }
            return
        }
        if RoomSeal.isSealedFrame(plain) {
            guard let opened = seal.unseal(plain) else { return }
            plain = opened
        } else if room.isPrivate && !room.serverVisible {
            return
        }
        let type = plain["type"]?.text ?? ""
        let id = plain["id"]?.text ?? ""
        if type == "chat" || type == "thread-reply" {
            if isGone(id) || consumePendingDelete(plain, conversation: room.conversation) { return }
        }
        if type == "delete" { applyDelete(plain, conversation: room.conversation, stash: true) }
        guard
            let message = OfficeFrames.chatMessage(
                plain, conversation: room.conversation, selfId: selfId(), recipients: recipients,
                channelId: room.isPrivate ? "" : room.id)
        else {
            onFrame?(plain, room.conversation)
            return
        }
        let added = append(message, roomId: room.id)
        onFrame?(plain, room.conversation)
        if added && live && !message.isSelf { onMessage?(message) }
    }

    private func isGone(_ id: String) -> Bool {
        !id.isEmpty && (removedIds.contains(id) || isRemoved?(id) == true)
    }

    @discardableResult
    private func applyDelete(_ plain: JSONObject, conversation: String, stash: Bool) -> Bool {
        let target = plain["target"]?.text ?? ""
        let senderId = plain["senderId"]?.text ?? ""
        guard !target.isEmpty, !senderId.isEmpty else { return false }
        if let message = history[conversation]?.first(where: { $0.id == target }) {
            guard message.senderId == senderId else { return false }
            removeLocal(target, conversation: conversation)
            return true
        }
        guard stash, !removedIds.contains(target) else { return false }
        if pendingDeletes.updateValue(senderId, forKey: target) == nil { pendingDeleteOrder.append(target) }
        while pendingDeleteOrder.count > DirectMessageFrames.maxPendingDeletes {
            pendingDeletes.removeValue(forKey: pendingDeleteOrder.removeFirst())
        }
        return false
    }

    private func consumePendingDelete(_ plain: JSONObject, conversation: String) -> Bool {
        let id = plain["id"]?.text ?? ""
        guard !id.isEmpty, let sender = pendingDeletes[id], sender == (plain["senderId"]?.text ?? "") else { return false }
        pendingDeletes.removeValue(forKey: id)
        pendingDeleteOrder.removeAll { $0 == id }
        removedIds.insert(id)
        onRemoved?(conversation, id)
        return true
    }

    private func removeLocal(_ id: String, conversation: String) {
        if var list = history[conversation] {
            list.removeAll { $0.id == id }
            history[conversation] = list.isEmpty ? nil : list
        }
        removedIds.insert(id)
        receipts.removeValue(forKey: id)
        onRemoved?(conversation, id)
    }

    public func retentionKey(_ conversation: String) -> String {
        ChatRetention.key(conversation: conversation, officeRoomId: room(for: conversation)?.id)
    }

    @discardableResult
    public func pruneExpired() -> Bool {
        guard let isExpired else { return false }
        var changed = false
        for (conversation, list) in history {
            let key = retentionKey(conversation)
            let kept = list.filter { !isExpired(key, $0.timestamp) }
            if kept.count == list.count { continue }
            changed = true
            history[conversation] = kept.isEmpty ? nil : kept
        }
        return changed
    }

    @discardableResult
    private func append(_ message: ChatMessage, roomId: String) -> Bool {
        if isExpired?(ChatRetention.key(conversation: message.conversation, officeRoomId: roomId), message.timestamp) == true {
            return false
        }
        var message = message
        if message.recipients == nil, let count = ackedCounts[message.id] { message.recipients = count }
        var ids = seen[roomId] ?? []
        if ids.contains(message.id) {
            if let count = message.recipients { setRecipients(count, id: message.id, conversation: message.conversation) }
            return false
        }
        ids.insert(message.id)
        seen[roomId] = ids
        var list = history[message.conversation] ?? []
        var at = list.count
        while at > 0 && RelayService.compareMessages(list[at - 1], message) > 0 { at -= 1 }
        list.insert(message, at: at)
        if list.count > OfficeFrames.maxMessagesPerRoom { list.removeFirst(list.count - OfficeFrames.maxMessagesPerRoom) }
        history[message.conversation] = list
        return true
    }

    public func open(_ conversation: String) async -> Bool {
        guard let room = room(for: conversation) else { return false }
        if sockets[room.id] != nil { return true }
        guard let base = origin() else { return false }
        do {
            let access = try await api.accountApiCall("GET", "/api/chat/channels/\(room.id)/room-access", nil)
            let roomName = access["room"]?.text ?? ""
            let passphrase = access["passphrase"]?.text ?? ""
            guard !roomName.isEmpty, !passphrase.isEmpty,
                let url = OfficeFrames.webSocketURL(access["webSocketUrl"]?.text ?? "", origin: base)
            else {
                lastError = OfficeError.unavailable.description
                return false
            }
            let crypto = await derive(roomName, passphrase)
            let seal = try seal(for: roomName)
            let connection = try await connector.connect(
                url: url, options: WebSocketOptions(connectTimeout: RelayService.connectTimeout, pingInterval: RelayService.pingInterval))
            let socket = OfficeRoomSocket(roomId: room.id, crypto: crypto, seal: seal, connection: connection)
            sockets[room.id] = socket
            connected.insert(room.id)
            socket.reader = Task { [weak self] in
                do {
                    for try await message in connection.messages {
                        guard let self else { return }
                        self.receive(message, roomId: room.id)
                    }
                } catch {}
                self?.dropped(room.id, connection)
            }
            advertise(socket)
            return true
        } catch {
            lastError = "\(error)"
            return false
        }
    }

    private func receive(_ message: WebSocketMessage, roomId: String) {
        guard let socket = sockets[roomId], let room = rooms.first(where: { $0.id == roomId }) else { return }
        let text: String
        switch message {
        case .text(let value): text = value
        case .data(let value): text = String(decoding: value, as: UTF8.self)
        }
        let envelope = JSON.parseObject(text)
        if envelope.isEmpty { return }
        if let ack = OfficeFrames.recipientsAck(envelope) {
            applyAck(ack.ref, ack.recipients, conversation: room.conversation)
            return
        }
        guard let plain = socket.crypto.decrypt(envelope) else { return }
        accept(plain, room: room, seal: socket.seal, live: true, recipients: OfficeFrames.recipients(envelope))
    }

    private func rememberSent(_ ref: String, id: String) {
        if pendingRefs.updateValue(id, forKey: ref) == nil { pendingOrder.append(ref) }
        receipts[id] = .sending
        while pendingOrder.count > OfficeFrames.maxPendingRefs {
            let dropped = pendingOrder.removeFirst()
            if let oldId = pendingRefs.removeValue(forKey: dropped) {
                ackedCounts.removeValue(forKey: oldId)
                receipts.removeValue(forKey: oldId)
            }
        }
    }

    private func unsent(_ id: String, conversation: String, shown: Bool) {
        receipts.removeValue(forKey: id)
        guard shown, var list = history[conversation] else { return }
        list.removeAll { $0.id == id }
        history[conversation] = list.isEmpty ? nil : list
        for roomId in seen.keys { seen[roomId]?.remove(id) }
    }

    private func applyAck(_ ref: String, _ count: Int, conversation: String) {
        guard let id = pendingRefs[ref] else { return }
        ackedCounts[id] = count
        if receipts[id] == .sending { receipts[id] = .sent }
        setRecipients(count, id: id, conversation: conversation)
    }

    private func setRecipients(_ count: Int, id: String, conversation: String) {
        guard var list = history[conversation], let index = list.firstIndex(where: { $0.id == id }) else { return }
        if list[index].recipients == count { return }
        list[index].recipients = count
        history[conversation] = list
    }

    private func dropped(_ roomId: String, _ connection: WebSocketConnection) {
        guard let socket = sockets[roomId], socket.connection === connection else { return }
        sockets.removeValue(forKey: roomId)
        connected.remove(roomId)
    }

    private func advertise(_ socket: OfficeRoomSocket) {
        guard let envelope = try? socket.crypto.encrypt(socket.seal.keyFrame()) else { return }
        let connection = socket.connection
        Task { try? await connection.send(text: JSON.encode(envelope)) }
    }

    private func close(_ socket: OfficeRoomSocket) async {
        socket.reader?.cancel()
        sockets.removeValue(forKey: socket.roomId)
        connected.remove(socket.roomId)
        await socket.connection.close(code: WebSocketStatus.normalClosure, reason: "")
    }

    public func closeAll() async {
        for socket in Array(sockets.values) { await close(socket) }
    }

    func frame(_ room: OfficeRoom, text: String, bot: Bool) -> JSONObject {
        sequence += 1
        let me = bot ? "forkbot" : selfId()
        var plain: JSONObject = [
            "type": "chat",
            "id": .string(bot ? UUID().uuidString.lowercased() : "\(me)-\(clock.nowMillis)-\(sequence)"),
            "senderId": .string(me),
            "sender": .string(bot ? "forkbot" : String(displayName().prefix(ChatLimits.maxDisplayNameChars))),
            "ts": .int(clock.nowMillis),
            "platform": .string(AppBuild.currentPlatform),
            "version": .string(RelayService.clientVersion),
            "channel": .string("#\(room.name)"),
            "text": .string(String(text.prefix(ChatLimits.maxTextChars))),
        ]
        if bot { plain["accountKind"] = "user" }
        if room.isPrivate { plain["private"] = true }
        return plain
    }

    @discardableResult
    public func send(_ conversation: String, text: String, asBot: Bool = false) async -> Bool {
        await post(conversation, text: text, asBot: asBot) != nil
    }

    public func post(_ conversation: String, text: String, asBot: Bool = false) async -> String? {
        let body = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !body.isEmpty, let room = room(for: conversation), !room.readOnly else { return nil }
        if asBot && room.isPrivate { return nil }
        if sockets[room.id] == nil, !(await open(conversation)) { return nil }
        guard let socket = sockets[room.id] else { return nil }
        let plain = frame(room, text: body, bot: asBot)
        do {
            let framed = room.isPrivate ? try socket.seal.seal(plain) : plain
            var envelope = try socket.crypto.encrypt(framed)
            envelope["persist"] = true
            envelope["notify"] = true
            let wire = JSON.encode(envelope)
            rememberSent(OfficeFrames.sentRef(wire), id: plain["id"]?.text ?? "")
            var shown = false
            if let message = OfficeFrames.chatMessage(plain, conversation: room.conversation, selfId: selfId()) {
                shown = append(message, roomId: room.id)
            }
            do {
                try await socket.connection.send(text: wire)
            } catch {
                unsent(plain["id"]?.text ?? "", conversation: room.conversation, shown: shown)
                throw error
            }
            return plain.str("id")
        } catch {
            lastError = "\(error)"
            return nil
        }
    }

    public var currentSelfId: String { selfId() }

    public func postFrame(_ conversation: String, type: String, fields: JSONObject) async -> JSONObject? {
        guard let room = room(for: conversation), !room.readOnly else { return nil }
        if sockets[room.id] == nil, !(await open(conversation)) { return nil }
        guard let socket = sockets[room.id] else { return nil }
        var plain = frame(room, text: "", bot: false)
        plain["type"] = .string(type)
        plain.removeValue(forKey: "text")
        for (key, value) in fields { plain[key] = value }
        do {
            let framed = room.isPrivate ? try socket.seal.seal(plain) : plain
            var envelope = try socket.crypto.encrypt(framed)
            envelope["persist"] = true
            if type == "chat" || type == "thread-reply" { envelope["notify"] = true }
            let wire = JSON.encode(envelope)
            if type == "chat" || type == "thread-reply" { rememberSent(OfficeFrames.sentRef(wire), id: plain["id"]?.text ?? "") }
            var shown = false
            if let message = OfficeFrames.chatMessage(plain, conversation: room.conversation, selfId: selfId()) {
                shown = append(message, roomId: room.id)
            }
            do {
                try await socket.connection.send(text: wire)
            } catch {
                unsent(plain["id"]?.text ?? "", conversation: room.conversation, shown: shown)
                throw error
            }
            if type == "delete" { applyDelete(plain, conversation: room.conversation, stash: false) }
            onFrame?(plain, room.conversation)
            return plain
        } catch {
            lastError = "\(error)"
            return nil
        }
    }

    @discardableResult
    public func deleteForEveryone(_ conversation: String, messageId: String) async -> Bool {
        guard !messageId.isEmpty, let room = room(for: conversation) else { return false }
        let fields: JSONObject = ["target": .string(messageId), "conversation": .string("#\(room.name)")]
        return await postFrame(conversation, type: "delete", fields: fields) != nil
    }

    public func create(name: String, isPrivate: Bool, members: [String]) async throws -> OfficeRoom? {
        let normalized = name.trimmingCharacters(in: .whitespaces).lowercased()
        guard OfficeFrames.validName(normalized) else { throw OfficeError.refused("invalid_channel_name") }
        var body: JSONObject = ["name": .string(normalized), "visibility": .string(isPrivate ? "private" : "public")]
        if isPrivate {
            body["members"] = JSONValue(Array(members.map { $0.trimmingCharacters(in: .whitespaces).lowercased() }.filter { !$0.isEmpty }.prefix(500)))
        }
        _ = try await call("POST", "/api/chat/channels", body)
        await refresh()
        return rooms.first { $0.name == normalized }
    }

    public func update(_ room: OfficeRoom, name: String? = nil, topic: String? = nil, isPrivate: Bool? = nil) async throws {
        var body: JSONObject = [:]
        if let name {
            let normalized = name.trimmingCharacters(in: .whitespaces).lowercased()
            guard OfficeFrames.validName(normalized) else { throw OfficeError.refused("invalid_channel_name") }
            body["name"] = .string(normalized)
        }
        if let topic {
            let collapsed = topic.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
            guard collapsed.count <= OfficeFrames.maxTopicChars else { throw OfficeError.refused("topic_too_long") }
            body["topic"] = .string(collapsed)
        }
        if let isPrivate { body["visibility"] = .string(isPrivate ? "private" : "public") }
        if body.isEmpty { return }
        _ = try await call("PATCH", "/api/chat/channels/\(room.id)", body)
        if let socket = sockets[room.id], isPrivate != nil { await close(socket) }
        await refresh()
    }

    public func delete(_ room: OfficeRoom) async throws {
        _ = try await call("DELETE", "/api/chat/channels/\(room.id)", nil)
        if let socket = sockets[room.id] { await close(socket) }
        await refresh()
    }

    public func members(_ room: OfficeRoom) async throws -> [String] {
        let payload = try await call("GET", "/api/chat/channels/\(room.id)/members", nil)
        return (payload["members"]?.objects ?? []).map { $0["username"]?.text ?? "" }.filter { !$0.isEmpty }
    }

    public func addMember(_ room: OfficeRoom, username: String) async throws {
        _ = try await call("POST", "/api/chat/channels/\(room.id)/members", ["username": .string(username.lowercased())])
        await refresh()
    }

    public func removeMember(_ room: OfficeRoom, username: String) async throws {
        _ = try await call("DELETE", "/api/chat/channels/\(room.id)/members", ["username": .string(username.lowercased())])
        await refresh()
    }

    private func call(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        do {
            return try await api.accountApiCall(method, path, body)
        } catch let error as OrgApiException {
            throw OfficeError.refused(error.code)
        }
    }
}
