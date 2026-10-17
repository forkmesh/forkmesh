import FMApi
import FMBase
import FMNet
import Foundation
import Observation

public typealias NoteSocketOpen = @MainActor (_ noteId: String) async throws -> WebSocketConnection

public struct NotePeer: Equatable, Identifiable, Sendable {
    public let id: String
    public let name: String
    public let seen: Int64
    public let draftAt: Int64

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
}

public struct NotePresence: Sendable {
    public static let presenceMillis: Int64 = 15_000
    public static let draftMillis = 180
    public static let editingMillis: Int64 = 5_000
    static let peerFrames: Set<String> = ["presence", "cursor", "draft"]

    public let clientId: String
    private var known: [String: NotePeer] = [:]
    private var order: [String] = []

    public init(clientId: String) {
        self.clientId = clientId
    }

    public var presenceFrame: String {
        JSON.encode(.object(["type": "presence", "clientId": .string(clientId)]))
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
            known[sender] = NotePeer(id: sender, name: name, seen: now, draftAt: type == "draft" ? now : previous?.draftAt ?? 0)
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
        default: event = nil
        }
        return (event, newcomer)
    }

    public mutating func peers(now: Int64) -> [NotePeer] {
        let stale = order.filter { now - (known[$0]?.seen ?? 0) > Self.presenceMillis * 5 / 2 }
        for id in stale { known[id] = nil }
        order.removeAll { known[$0] == nil }
        return order.compactMap { known[$0] }
    }

    public mutating func label(open: Bool, now: Int64) -> String {
        if !open { return "Offline — edits still save" }
        let present = peers(now: now)
        if let editing = present.first(where: { $0.draftAt > 0 && now - $0.draftAt < Self.editingMillis }) {
            return "\(editing.name) is editing…"
        }
        if present.count == 1 { return "Live · \(present[0].name) is here" }
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
    @ObservationIgnored private var draftGeneration = 0
    @ObservationIgnored private var stopped = false
    public let noteId: String

    public private(set) var connected = false
    public private(set) var everConnected = false
    public private(set) var faces: [NotePeer] = []
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
                await send(presence.presenceFrame)
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
                await self.send(self.presence.presenceFrame)
                self.refresh()
            }
        }
    }

    func handle(_ text: String) -> Bool {
        let (event, newcomer) = presence.receive(text, now: clock.nowMillis)
        if newcomer {
            let frame = presence.presenceFrame
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
        faces = Array(presence.peers(now: now).prefix(Self.maxFaces))
        label = presence.label(open: connected, now: now)
    }

    public func draftChanged(title: String, markdown: String, baseVersion: Int) {
        draftGeneration += 1
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
