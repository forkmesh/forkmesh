import FMApi
import FMBase
import FMNet
import Foundation
import Observation

public typealias NoteSocketOpen = @MainActor (_ noteId: String) async throws -> WebSocketConnection

public enum NotePeerState: String, Equatable, Sendable {
    case viewing, editing
}

public struct NotePeer: Equatable, Identifiable, Sendable {
    public let id: String
    public let name: String
    public let seen: Int64
    public let draftAt: Int64
    public let state: NotePeerState

    public init(id: String, name: String, seen: Int64, draftAt: Int64 = 0, state: NotePeerState = .viewing) {
        self.id = id
        self.name = name
        self.seen = seen
        self.draftAt = draftAt
        self.state = state
    }

    public var editing: Bool { state == .editing }
    public var stateLabel: String { "\(name) · \(state.rawValue)" }

    public var initials: String {
        let parts = name.split(whereSeparator: { !($0.isLetter || $0.isNumber) })
        let first = parts.first?.first.map(String.init) ?? "?"
        let second = parts.dropFirst().first?.first.map(String.init) ?? ""
        return (first + second).uppercased()
    }
}

public enum NoteRoomEvent: Equatable, Sendable {
    case draft(title: String, markdown: String, baseVersion: Int)
    case updated(version: Int)
    case deleted
    case accessChanged
    case commentsChanged
}

public struct NotePresence: Sendable {
    public static let presenceMillis: Int64 = 15_000
    public static let draftMillis = 180
    public static let editingMillis: Int64 = 5_000
    public static let typingMillis: Int64 = 10_000
    static let peerFrames: Set<String> = ["presence", "cursor", "draft"]

    public let clientId: String
    private var known: [String: NotePeer] = [:]
    private var order: [String] = []
    private var typedAt: Int64 = 0

    public init(clientId: String) {
        self.clientId = clientId
    }

    public mutating func typed(now: Int64) {
        typedAt = now
    }

    public func localState(now: Int64) -> NotePeerState {
        typedAt > 0 && now - typedAt < Self.typingMillis ? .editing : .viewing
    }

    public func presenceFrame(state: NotePeerState) -> String {
        JSON.encode(.object(["type": "presence", "clientId": .string(clientId), "state": .string(state.rawValue)]))
    }

    static func state(_ frame: JSONObject, type: String, previous: NotePeer?) -> NotePeerState {
        if let declared = NotePeerState(rawValue: ToolsJSON.text(frame["state"], "")) { return declared }
        return type == "presence" ? .viewing : previous?.state ?? .viewing
    }

    public func draftFrame(title: String, markdown: String, baseVersion: Int) -> String {
        JSON.encode(.object([
            "type": "draft", "clientId": .string(clientId), "baseVersion": .int(Int64(baseVersion)),
            "title": .string(title), "markdown": .string(markdown),
        ]))
    }

    public mutating func receive(_ text: String, now: Int64) -> (event: NoteRoomEvent?, newcomer: Bool) {
        guard let frame = (try? JSON.parse(text))?.objectValue else { return (nil, false) }
        let type = ToolsJSON.text(frame["type"], "")
        let sender = ToolsJSON.text(frame["clientId"], "")
        var newcomer = false
        let fromPeer = !sender.isEmpty && sender != clientId
        if Self.peerFrames.contains(type) && fromPeer {
            let previous = known[sender]
            newcomer = previous == nil
            if previous == nil { order.append(sender) }
            let trimmed = ToolsJSON.text(frame["name"], "").dartTrimmed
            let name = String((trimmed.isEmpty ? "Collaborator" : trimmed).prefix(32))
            known[sender] = NotePeer(
                id: sender, name: name, seen: now, draftAt: type == "draft" ? now : previous?.draftAt ?? 0,
                state: Self.state(frame, type: type, previous: previous))
        }
        if type == "leave" && fromPeer && known.removeValue(forKey: sender) != nil {
            order.removeAll { $0 == sender }
        }
        let event: NoteRoomEvent?
        switch type {
        case "draft":
            event = fromPeer
                ? .draft(title: ToolsJSON.text(frame["title"], "Untitled note"), markdown: ToolsJSON.text(frame["markdown"], ""),
                         baseVersion: ToolsJSON.int(frame["baseVersion"]))
                : nil
        case "note-updated": event = .updated(version: ToolsJSON.int(frame["version"]))
        case "note-deleted": event = .deleted
        case "access-changed": event = .accessChanged
        case "note-comments": event = .commentsChanged
        default: event = nil
        }
        return (event, newcomer)
    }

    public mutating func peers(now: Int64) -> [NotePeer] {
        let stale = order.filter { now - (known[$0]?.seen ?? 0) > Self.presenceMillis * 5 / 2 }
        for id in stale { known[id] = nil }
        order.removeAll { known[$0] == nil }
        return order.compactMap { known[$0] }.map { (peer: NotePeer) -> NotePeer in
            let drafting = peer.draftAt > 0 && now - peer.draftAt < Self.editingMillis
            return NotePeer(id: peer.id, name: peer.name, seen: peer.seen, draftAt: peer.draftAt, state: drafting ? .editing : peer.state)
        }
    }

    public mutating func label(open: Bool, now: Int64) -> String {
        if !open { return "Offline — edits still save" }
        let present = peers(now: now)
        if let editing = present.first(where: \.editing) {
            return "\(editing.name) is editing…"
        }
        if present.count == 1 { return "Live · \(present[0].name) is viewing" }
        if !present.isEmpty { return "Live · \(present.count) others here" }
        return "Live"
    }

    public mutating func clear() {
        known = [:]
        order = []
    }
}

@MainActor
@Observable
public final class NoteRoomModel {
    public static let accessReconnectMillis = 400
    public static let maxFaces = 4

    @ObservationIgnored private let open: NoteSocketOpen
    @ObservationIgnored private let clock: Clock
    @ObservationIgnored private let sleep: ToolsSleep
    @ObservationIgnored private let onEvent: @MainActor (NoteRoomEvent) -> Void
    @ObservationIgnored private var presence: NotePresence
    @ObservationIgnored private var connection: WebSocketConnection?
    @ObservationIgnored var runTask: Task<Void, Never>?
    @ObservationIgnored var tickTask: Task<Void, Never>?
    @ObservationIgnored var draftTask: Task<Void, Never>?
    @ObservationIgnored var stateTask: Task<Void, Never>?
    @ObservationIgnored private var draftGeneration = 0
    @ObservationIgnored private var stopped = false
    @ObservationIgnored private var sentState: NotePeerState?
    public let noteId: String

    public private(set) var connected = false
    public private(set) var everConnected = false
    public private(set) var faces: [NotePeer] = []
    public private(set) var everyone: [NotePeer] = []
    public private(set) var overflow = 0
    public private(set) var label = ""

    public init(
        noteId: String, clientId: String = UUID().uuidString.lowercased(), open: @escaping NoteSocketOpen,
        clock: Clock = SystemClock(), sleep: @escaping ToolsSleep = toolsTaskSleep,
        onEvent: @escaping @MainActor (NoteRoomEvent) -> Void
    ) {
        self.noteId = noteId
        presence = NotePresence(clientId: clientId)
        self.open = open
        self.clock = clock
        self.sleep = sleep
        self.onEvent = onEvent
    }

    public var clientId: String { presence.clientId }
    public var showsLabel: Bool { everConnected }

    public func start() {
        guard runTask == nil else { return }
        stopped = false
        runTask = Task { [weak self] in await self?.run() }
    }

    public func stop() {
        stopped = true
        runTask?.cancel()
        tickTask?.cancel()
        draftTask?.cancel()
        stateTask?.cancel()
        runTask = nil
        let socket = connection
        connection = nil
        connected = false
        presence.clear()
        refresh()
        if let socket { Task { await socket.close() } }
    }

    private func run() async {
        while !stopped && !Task.isCancelled {
            var reconnectNow = false
            do {
                let socket = try await open(noteId)
                if stopped {
                    await socket.close()
                    return
                }
                connection = socket
                connected = true
                everConnected = true
                await announce()
                refresh()
                startTicker()
                for try await message in socket.messages {
                    guard let text = message.text else { continue }
                    if handle(text) { reconnectNow = true }
                }
            } catch {}
            tickTask?.cancel()
            connection = nil
            connected = false
            sentState = nil
            presence.clear()
            refresh()
            if stopped || Task.isCancelled { return }
            do {
                try await sleep(reconnectNow ? Self.accessReconnectMillis : Int(NotePresence.presenceMillis))
            } catch {
                return
            }
        }
    }

    private func startTicker() {
        tickTask?.cancel()
        let sleep = self.sleep
        tickTask = Task { [weak self] in
            while true {
                do { try await sleep(Int(NotePresence.presenceMillis)) } catch { return }
                guard let self, self.connected else { return }
                await self.announce()
                self.refresh()
            }
        }
    }

    func handle(_ text: String) -> Bool {
        let (event, newcomer) = presence.receive(text, now: clock.nowMillis)
        if newcomer {
            let frame = presence.presenceFrame(state: localState)
            Task { await self.send(frame) }
        }
        refresh()
        guard let event else { return false }
        if case .draft = event { scheduleRefresh() }
        onEvent(event)
        if event == .accessChanged {
            let socket = connection
            Task { await socket?.close() }
            return true
        }
        return false
    }

    private func scheduleRefresh() {
        let sleep = self.sleep
        Task { [weak self] in
            do { try await sleep(Int(NotePresence.editingMillis)) } catch { return }
            self?.refresh()
        }
    }

    public func refresh() {
        let now = clock.nowMillis
        everyone = presence.peers(now: now)
        faces = Array(everyone.prefix(Self.maxFaces))
        overflow = max(0, everyone.count - Self.maxFaces)
        label = presence.label(open: connected, now: now)
    }

    public var localState: NotePeerState { presence.localState(now: clock.nowMillis) }

    private func announce() async {
        let state = localState
        sentState = state
        await send(presence.presenceFrame(state: state))
    }

    public func typed() {
        presence.typed(now: clock.nowMillis)
        checkState()
        stateTask?.cancel()
        let sleep = self.sleep
        stateTask = Task { [weak self] in
            do { try await sleep(Int(NotePresence.typingMillis)) } catch { return }
            self?.checkState()
        }
    }

    func checkState() {
        let state = localState
        guard connected, state != sentState else { return }
        sentState = state
        let frame = presence.presenceFrame(state: state)
        Task { await self.send(frame) }
    }

    public func draftChanged(title: String, markdown: String, baseVersion: Int) {
        draftGeneration += 1
        typed()
        guard connected else { return }
        let generation = draftGeneration
        let frame = presence.draftFrame(title: title, markdown: markdown, baseVersion: baseVersion)
        let sleep = self.sleep
        draftTask = Task { [weak self] in
            do { try await sleep(NotePresence.draftMillis) } catch { return }
            guard let self, self.draftGeneration == generation else { return }
            await self.send(frame)
        }
    }

    private func send(_ text: String) async {
        try? await connection?.send(text: text)
    }
}
