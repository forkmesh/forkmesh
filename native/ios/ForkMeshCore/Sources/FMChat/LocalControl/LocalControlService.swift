// Drives a desktop on the same network. Finds it through the local mesh, connects (asking the
// desktop to approve this phone the first time), then lists, starts, steers, stops and continues
// its agents and moves files both ways, straight over the LAN with nothing through the relay.
//
// On-device AI (see "On-device AI for paired desktops" below and docs/FMChat.md): with the
// user's consent (`serveLocalAIKey`, off by default) the phone answers `ai.info` /
// `ai.chat` requests the paired desktop sends as `ai.request` events, running them on
// FMLocalAI's `LocalInferenceService`. Strictly additive on the wire: requests and answers travel
// as `{event, data}` frames, which an older phone ignores (unknown event) and an older desktop
// ignores (LocalControlServer drops any frame without a positive `id` and a `method`).

import FMAccount
import FMApi
import FMBase
import FMCrypto
import FMLocalAI
import Foundation
import Observation

/// Where this phone stands with the desktop it is driving.
public enum LocalControlStatus: String, Sendable {
    /// No desktop is being driven.
    case idle
    case connecting
    /// The desktop is showing its approval prompt with `pairingCode`.
    case awaitingApproval
    case connected
    case failed
}

/// A desktop this phone was approved on, remembered so it reconnects on its own whenever the two
/// are on the same network.
public struct PairedDesktop: Equatable, Sendable, Identifiable {
    public let key: String
    public let name: String
    public let user: String
    public let pairedAtMs: Int64

    public init(key: String, name: String = "", user: String = "", pairedAtMs: Int64 = 0) {
        self.key = key
        self.name = name
        self.user = user
        self.pairedAtMs = pairedAtMs
    }

    public var id: String { key }

    public func toJSON() -> JSONObject {
        ["key": .string(key), "name": .string(name), "user": .string(user), "pairedAtMs": .int(pairedAtMs)]
    }

    public static func fromJSON(_ json: JSONValue?) -> PairedDesktop? {
        guard let json = json?.objectValue else { return nil }
        let key = dartString(json["key"])
        if controlFromB64(key, 32) == nil { return nil }
        let paired: Int64
        if case .int(let n)? = json["pairedAtMs"] { paired = n } else { paired = 0 }
        return PairedDesktop(key: key, name: dartString(json["name"]), user: dartString(json["user"]), pairedAtMs: paired)
    }
}

/// One agent session on the desktop, as its Agents list has it.
public struct LocalAgentRow: Equatable, Sendable, Identifiable {
    public let owner: String
    public let name: String
    public let session: AgentSession
    public let prompt: String
    public let stoppable: Bool
    public let attachments: [AgentAttachmentMeta]
    public let mode: String
    public let unread: Bool?

    public init(
        owner: String, name: String, session: AgentSession, prompt: String = "", stoppable: Bool = false,
        attachments: [AgentAttachmentMeta] = [], mode: String = "", unread: Bool? = nil
    ) {
        self.owner = owner
        self.name = name
        self.session = session
        self.prompt = prompt
        self.stoppable = stoppable
        self.attachments = attachments
        self.mode = mode
        self.unread = unread
    }

    public var id: Int { session.id }

    public var repoLabel: String { owner.isEmpty ? name : "\(owner)/\(name)" }

    /// The session's title, else the first line of what it was asked.
    public var title: String {
        if !session.issueTitle.isEmpty || session.issueNumber > 0 { return session.displayTitle }
        let line = (prompt.dartTrimmed.components(separatedBy: "\n").first ?? "").dartTrimmed
        if line.isEmpty { return session.displayTitle }
        return line.utf16.count > 90 ? "\(dartPrefix(line, 89))…" : line
    }

    public var canContinue: Bool { !session.isActive && !session.merged }

    public static func fromJSON(_ json: JSONObject) -> LocalAgentRow {
        LocalAgentRow(
            owner: dartString(json["repositoryOwner"]), name: dartString(json["repositoryName"]),
            session: AgentSession(json: json), prompt: dartString(json["prompt"]),
            stoppable: json["stoppable"] == .bool(true), attachments: AgentAttachmentMeta.list(json["attachments"]),
            mode: dartString(json["mode"]), unread: json["unread"]?.boolValue)
    }
}

public struct LocalAgentProvider: Equatable, Sendable, Identifiable {
    public let id: String
    public let label: String

    public init(_ id: String, _ label: String) {
        self.id = id
        self.label = label
    }
}

public struct LocalAgentRepository: Equatable, Sendable, Identifiable {
    public let owner: String
    public let name: String

    public init(_ owner: String, _ name: String) {
        self.owner = owner
        self.name = name
    }

    public var label: String { "\(owner)/\(name)" }
    public var id: String { label }
}

public struct LocalOutsideAgent: Equatable, Sendable, Identifiable {
    public let id: Int
    public let kind: String
    public let label: String
    public let provider: String
    public let model: String
    public let title: String
    public let cwd: String
    public let branchName: String
    public let owner: String
    public let name: String
    public let lastActivityMs: Int
    public let steer: Bool
    public let typeable: Bool
    public let stoppable: Bool
    public let otherUser: String
    public let terminal: String

    public init(
        id: Int, kind: String, label: String, provider: String = "", model: String = "", title: String = "",
        cwd: String = "", branchName: String = "", owner: String = "", name: String = "", lastActivityMs: Int = 0,
        steer: Bool = false, typeable: Bool = false, stoppable: Bool = false, otherUser: String = "",
        terminal: String = ""
    ) {
        self.id = id
        self.kind = kind
        self.label = label
        self.provider = provider
        self.model = model
        self.title = title
        self.cwd = cwd
        self.branchName = branchName
        self.owner = owner
        self.name = name
        self.lastActivityMs = lastActivityMs
        self.steer = steer
        self.typeable = typeable
        self.stoppable = stoppable
        self.otherUser = otherUser
        self.terminal = terminal
    }

    public static func fromJSON(_ json: JSONObject) -> LocalOutsideAgent {
        let kind = dartString(json["kind"])
        let label = dartString(json["label"])
        return LocalOutsideAgent(
            id: json.int("id"), kind: kind, label: label.isEmpty ? kind : label,
            provider: dartString(json["provider"]), model: dartString(json["model"]),
            title: dartString(json["title"]), cwd: dartString(json["cwd"]),
            branchName: dartString(json["branchName"]), owner: dartString(json["repositoryOwner"]),
            name: dartString(json["repositoryName"]), lastActivityMs: json.int("lastActivityMs"),
            steer: json.bool("steer"), typeable: json.bool("typeable"), stoppable: json.bool("stoppable"),
            otherUser: dartString(json["otherUser"]), terminal: dartString(json["terminal"]))
    }
}

public struct LocalAgentQueue: Equatable, Sendable {
    public let running: Int
    public let limit: Int
    public let limitMax: Int
    public let order: [Int]

    public init(running: Int = 0, limit: Int = 1, limitMax: Int = 99, order: [Int] = []) {
        self.running = running
        self.limit = limit
        self.limitMax = limitMax
        self.order = order
    }

    public var waiting: Int { order.count }

    public func position(of id: Int) -> Int? { order.firstIndex(of: id) }

    public func moved(_ id: Int, to index: Int) -> LocalAgentQueue {
        guard let from = order.firstIndex(of: id) else { return self }
        var next = order
        next.remove(at: from)
        next.insert(id, at: min(max(0, index), next.count))
        return LocalAgentQueue(running: running, limit: limit, limitMax: limitMax, order: next)
    }

    public static func target(from source: Int, offset: Int) -> Int {
        offset > source ? offset - 1 : offset
    }

    public var summary: String {
        let head = "\(running) of \(limit) running"
        return waiting == 0 ? head : "\(head) · \(waiting) waiting"
    }

    public static func fromJSON(_ value: JSONValue?) -> LocalAgentQueue? {
        guard let json = value?.objectValue, json["limit"] != nil else { return nil }
        let limit = max(1, json.int("limit", 1))
        return LocalAgentQueue(
            running: max(0, json.int("running")), limit: limit, limitMax: max(limit, json.int("limitMax", 99)),
            order: json.list("queue").map { $0.int(or: 0) }.filter { $0 > 0 })
    }
}

/// The desktop's Agents list and what a new agent can be started with.
public struct LocalAgentFolder: Equatable, Sendable, Identifiable {
    public let id: String
    public let owner: String
    public let repoName: String
    public let name: String
    public let collapsed: Bool
    public let members: [String]

    public init(
        id: String, owner: String, repoName: String, name: String, collapsed: Bool = false, members: [String] = []
    ) {
        self.id = id
        self.owner = owner
        self.repoName = repoName
        self.name = name
        self.collapsed = collapsed
        self.members = members
    }

    public static func fromJSON(_ json: JSONObject) -> LocalAgentFolder {
        let name = dartString(json["name"]).dartTrimmed
        return LocalAgentFolder(
            id: dartString(json["id"]), owner: dartString(json["repositoryOwner"]),
            repoName: dartString(json["repositoryName"]), name: name.isEmpty ? "Folder" : name,
            collapsed: json["collapsed"] == .bool(true),
            members: (json["members"]?.arrayValue ?? []).compactMap(\.stringValue).filter { !$0.isEmpty })
    }
}

public struct LocalAgentsSnapshot: Equatable, Sendable {
    public let sessions: [LocalAgentRow]
    public let outside: [LocalOutsideAgent]
    public let repositories: [LocalAgentRepository]
    public let providers: [LocalAgentProvider]
    public let defaultProvider: String
    public let running: Int
    public let queue: LocalAgentQueue?
    public let defaultMode: String
    public let folders: [LocalAgentFolder]

    public init(
        sessions: [LocalAgentRow] = [], outside: [LocalOutsideAgent] = [], repositories: [LocalAgentRepository] = [],
        providers: [LocalAgentProvider] = [], defaultProvider: String = "", running: Int = 0,
        queue: LocalAgentQueue? = nil, defaultMode: String = "", folders: [LocalAgentFolder] = []
    ) {
        self.sessions = sessions
        self.outside = outside
        self.repositories = repositories
        self.providers = providers
        self.defaultProvider = defaultProvider
        self.running = running
        self.queue = queue
        self.defaultMode = defaultMode
        self.folders = folders
    }

    public func with(queue: LocalAgentQueue?) -> LocalAgentsSnapshot {
        LocalAgentsSnapshot(
            sessions: sessions, outside: outside, repositories: repositories, providers: providers,
            defaultProvider: defaultProvider, running: running, queue: queue, defaultMode: defaultMode,
            folders: folders)
    }

    public static func fromJSON(_ json: JSONObject) -> LocalAgentsSnapshot {
        func maps(_ value: JSONValue?) -> [JSONObject] { (value?.arrayValue ?? []).compactMap(\.objectValue) }
        let running: Int
        if case .int(let n)? = json["running"] { running = Int(n) } else { running = 0 }
        return LocalAgentsSnapshot(
            sessions: maps(json["sessions"]).map(LocalAgentRow.fromJSON),
            outside: maps(json["outside"]).map(LocalOutsideAgent.fromJSON).filter { $0.id != 0 },
            repositories: maps(json["repositories"]).map { LocalAgentRepository(dartString($0["owner"]), dartString($0["name"])) }
                .filter { !$0.owner.isEmpty && !$0.name.isEmpty },
            providers: maps(json["providers"]).map { LocalAgentProvider(dartString($0["id"]), dartString($0["label"])) }
                .filter { !$0.id.isEmpty },
            defaultProvider: dartString(json["defaultProvider"]),
            running: running,
            queue: LocalAgentQueue.fromJSON(json["queue"]),
            defaultMode: dartString(json["defaultMode"]),
            folders: maps(json["folders"]).map(LocalAgentFolder.fromJSON).filter { !$0.id.isEmpty && !$0.repoName.isEmpty })
    }
}

/// A file the desktop sent to this phone.
public struct LocalReceivedFile: Equatable, Sendable, Identifiable {
    public let name: String
    public let path: String
    public let size: Int
    public let receivedAtMs: Int64

    public init(_ name: String, _ path: String, _ size: Int, _ receivedAtMs: Int64) {
        self.name = name
        self.path = path
        self.size = size
        self.receivedAtMs = receivedAtMs
    }

    public var id: String { path }
}

final class IncomingFile {
    let name: String
    let size: Int
    var path = ""
    var handle: FileHandle?
    var received = 0

    init(name: String, size: Int) {
        self.name = name
        self.size = size
    }
}

/// Opens a channel; a test swaps in its own.
public typealias LocalControlConnector = @MainActor (MeshControlHost, String?) async throws -> LocalControlConnection

@MainActor
@Observable
public final class LocalControlService {
    nonisolated public static let pairedKey = "localControl/pairedDesktops"
    /// Whether paired desktops may run requests on this phone's on-device model (Bool, default
    /// false). New for the native app.
    nonisolated public static let serveLocalAIKey = "localControl/serveLocalAI"
    nonisolated public static let lanAddressesKey = "localControl/lanAddresses"
    nonisolated static let lanProbeWindowMs: Int64 = 1_500

    @ObservationIgnored let mesh: LocalMeshService
    @ObservationIgnored let identity: DeviceIdentity
    @ObservationIgnored let prefs: KeyValueStore
    @ObservationIgnored let userName: @MainActor () -> String
    @ObservationIgnored let connectorOverride: LocalControlConnector?
    @ObservationIgnored let filesDirectory: () -> URL
    @ObservationIgnored let clock: Clock
    @ObservationIgnored let sleeper: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored let platformName: String
    @ObservationIgnored public let listeners = Listeners()

    /// Where the on-device model comes from, looked up per request (FMLocalAI registers it after
    /// the chat services exist). nil: this build offers no on-device model.
    @ObservationIgnored public var inferenceProvider: (@MainActor () -> LocalInferenceService?)?

    public private(set) var paired: [PairedDesktop] = []
    public private(set) var status: LocalControlStatus = .idle
    public private(set) var pairingCode = ""
    public private(set) var error = ""
    public private(set) var desktopKey = ""
    public private(set) var desktopName = ""
    public private(set) var desktopUser = ""
    public private(set) var host: MeshControlHost?
    public private(set) var agents: LocalAgentsSnapshot?
    public private(set) var agentsLoading = false
    public private(set) var receivedFiles: [LocalReceivedFile] = []
    public private(set) var uploadName = ""
    /// 0...1 while a file is going to the desktop, else nil.
    public private(set) var uploadProgress: Double?
    /// Bumped by every change (and by every mesh change, which moves `hosts`).
    public private(set) var revision = 0
    /// Requests from the desktop answered since launch, for the settings line.
    public private(set) var aiRequestsServed = 0

    @ObservationIgnored private var connection: LocalControlConnection?
    @ObservationIgnored private var eventTask: Task<Void, Never>?
    // Set by an explicit disconnect: the paired desktop stays unreconnected until the person
    // connects again.
    @ObservationIgnored private var autoConnectHeld = false
    @ObservationIgnored private var dialQueued = false
    @ObservationIgnored private var accountDesktopsTried: Set<String> = []
    @ObservationIgnored private var accountDesktopKeys: Set<String> = []
    @ObservationIgnored private var retryTask: Task<Void, Never>?
    @ObservationIgnored private var failures = 0
    @ObservationIgnored private var disposed = false
    @ObservationIgnored private var agentsInFlight: Task<Void, Never>?
    @ObservationIgnored private var agentsAgain = false
    @ObservationIgnored private var incoming: [String: IncomingFile] = [:]
    @ObservationIgnored private var meshToken: ListenerToken?
    @ObservationIgnored private var aiTasks: [Int64: Task<Void, Never>] = [:]
    @ObservationIgnored private var eventTaps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]
    @ObservationIgnored private var epoch = 0
    @ObservationIgnored private var lanProbeMs: Int64 = 0

    public init(
        mesh: LocalMeshService,
        identity: DeviceIdentity,
        prefs: KeyValueStore,
        userName: @escaping @MainActor () -> String = { "" },
        connector: LocalControlConnector? = nil,
        filesDirectory: (() -> URL)? = nil,
        platform: String = AppBuild.currentPlatform,
        clock: Clock = SystemClock(),
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.mesh = mesh
        self.identity = identity
        self.prefs = prefs
        self.userName = userName
        self.connectorOverride = connector
        self.filesDirectory = filesDirectory ?? LocalControlService.defaultFilesDirectory
        self.platformName = platform
        self.clock = clock
        self.sleeper = sleep
        meshToken = mesh.addListener { [weak self] in
            MainActor.assumeIsolated { self?.onMeshChanged() }
        }
        mesh.rememberedBeaconTargets = { [weak self] in self?.lanProbeTargets() ?? [] }
        loadPaired()
    }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    private func notifyListeners() {
        if disposed { return }
        revision &+= 1
        listeners.notify()
    }

    public var connected: Bool { status == .connected }

    public var lanPending: Bool {
        if connected || autoConnectHeld { return false }
        if dialQueued || status == .connecting { return true }
        return clock.nowMillis - lanProbeMs < LocalControlService.lanProbeWindowMs
    }

    private func rememberedAddresses() -> [String: String] {
        let raw = prefs.string(forKey: LocalControlService.lanAddressesKey) ?? ""
        guard !raw.isEmpty, case .object(let map)? = try? JSON.parse(raw) else { return [:] }
        var out: [String: String] = [:]
        for (key, value) in map {
            if let address = value.stringValue { out[key] = address }
        }
        return out
    }

    private func rememberAddress(_ key: String, _ address: String) {
        if key.isEmpty || address.isEmpty { return }
        var map = rememberedAddresses()
        if map[key] == address { return }
        map[key] = address
        let stored = JSONObject(map.sorted { $0.key < $1.key }.map { ($0.key, JSONValue.string($0.value)) })
        prefs.set(JSON.encode(.object(stored)), forKey: LocalControlService.lanAddressesKey)
    }

    private func lanProbeTargets() -> [MeshAddress] {
        if connected || autoConnectHeld || disposed { return [] }
        let heard = Set(mesh.controlHosts().map(\.nodeId))
        let targets = rememberedAddresses()
            .filter { isKnown($0.key) && !heard.contains($0.key) }
            .values.compactMap { MeshAddress($0) }
        if !targets.isEmpty { lanProbeMs = clock.nowMillis }
        return targets
    }

    public func isPaired(_ key: String) -> Bool { paired.contains { $0.key == key } }

    private func isKnown(_ key: String) -> Bool { isPaired(key) || accountDesktopKeys.contains(key) }

    public func setAccountDesktops(_ keys: Set<String>) {
        if keys == accountDesktopKeys { return }
        accountDesktopKeys = keys
        onMeshChanged()
    }

    /// Desktops on this network offering control right now.
    public var hosts: [MeshControlHost] { mesh.controlHosts() }

    public nonisolated static func defaultFilesDirectory() -> URL {
        let documents = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        return documents.appendingPathComponent("ForkMesh from desktop", isDirectory: true)
    }

    // MARK: Paired desktops

    private func readPaired() -> [PairedDesktop] {
        let raw = prefs.string(forKey: LocalControlService.pairedKey) ?? ""
        guard !raw.isEmpty, case .array(let list)? = try? JSON.parse(raw) else { return [] }
        return list.compactMap(PairedDesktop.fromJSON)
    }

    private func loadPaired() {
        paired = readPaired()
        notifyListeners()
        onMeshChanged()
    }

    private func savePaired() {
        prefs.set(JSON.encode(.array(paired.map { .object($0.toJSON()) })), forKey: LocalControlService.pairedKey)
    }

    private func setStatus(_ next: LocalControlStatus, error message: String = "") {
        status = next
        error = message
        notifyListeners()
    }

    /// A paired desktop that turns up on the network is reconnected to without being asked; so
    /// is one this phone lost a moment ago.
    private func onMeshChanged() {
        if disposed { return }
        notifyListeners()
        if autoConnectHeld || dialQueued || connection != nil { return }
        if status == .connecting { return }
        if retryTask != nil { return }
        guard
            let candidate = LocalControlService.automaticTarget(
                mesh.controlHosts(), paired: Set(paired.map(\.key)), accountDesktops: accountDesktopKeys,
                account: userName(), tried: accountDesktopsTried)
        else { return }
        if !isKnown(candidate.nodeId) { accountDesktopsTried.insert(candidate.nodeId) }
        dialQueued = true
        Task {
            self.dialQueued = false
            await self.connect(candidate, automatic: true)
        }
    }

    public nonisolated static func automaticTarget(
        _ hosts: [MeshControlHost], paired: Set<String>, accountDesktops: Set<String> = [], account: String,
        tried: Set<String>
    ) -> MeshControlHost? {
        if let known = hosts.first(where: { paired.contains($0.nodeId) }) { return known }
        if let owned = hosts.first(where: { accountDesktops.contains($0.nodeId) }) { return owned }
        let account = account.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if account.isEmpty { return nil }
        return hosts.first {
            !tried.contains($0.nodeId)
                && $0.ownerUser.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() == account
        }
    }

    private func open(_ target: MeshControlHost, _ expected: String?) async throws -> LocalControlConnection {
        if let connectorOverride { return try await connectorOverride(target, expected) }
        return try await LocalControlConnection.open(
            address: target.address, port: target.port, identity: identity, deviceName: mesh.displayName,
            user: userName(), platform: platformName, expectedKey: expected)
    }

    /// Connects to `target`, asking it to approve this phone if it never has.
    public func connect(_ target: MeshControlHost, automatic: Bool = false) async {
        if !automatic { autoConnectHeld = false }
        retryTask?.cancel()
        retryTask = nil
        if connection != nil { await disconnect(hold: false) }
        host = target
        desktopKey = target.nodeId
        desktopName = target.label
        desktopUser = target.ownerUser
        pairingCode = ""
        setStatus(.connecting)
        let started = epoch
        let opened: LocalControlConnection
        do {
            opened = try await open(target, target.nodeId)
        } catch {
            if started != epoch { return }
            failed(LocalControlService.describe(error), target)
            return
        }
        if disposed || started != epoch {
            opened.close()
            return
        }
        connection = opened
        failures = 0
        if !opened.serverName.isEmpty { desktopName = opened.serverName }
        if !opened.serverUser.isEmpty { desktopUser = opened.serverUser }
        let events = opened.events
        eventTask = Task { [weak self] in
            for await message in events {
                guard let self else { return }
                self.onEvent(message)
            }
        }
        Task { [weak self] in
            await opened.done()
            self?.onClosed(opened)
        }
    }

    private func failed(_ message: String, _ target: MeshControlHost) {
        connection = nil
        failures += 1
        setStatus(.failed, error: message)
        // A paired desktop is tried again a little later, backing off, for as long as the mesh
        // still hears it.
        if isKnown(target.nodeId) && !autoConnectHeld {
            scheduleRetry(TimeInterval(min(max(5 * failures, 5), 60)))
        }
    }

    private func scheduleRetry(_ seconds: TimeInterval) {
        retryTask?.cancel()
        let sleeper = self.sleeper
        retryTask = Task { [weak self] in
            do { try await sleeper(seconds) } catch { return }
            guard let self else { return }
            self.retryTask = nil
            self.onMeshChanged()
        }
    }

    nonisolated static func describe(_ error: Error) -> String {
        if let control = error as? LocalControlException {
            switch control.code {
            case "wrong_desktop": return "A different machine answered at that address. Try again in a moment."
            case "bad_signature", "bad_welcome": return "The desktop could not prove who it is."
            case "closed": return "The desktop closed the connection."
            case "pairing_denied": return "The desktop declined this phone."
            default: return "The desktop could not be reached (\(control.code))."
            }
        }
        if error is MeshSocketError { return "The desktop could not be reached on this network." }
        return "The desktop could not be reached."
    }

    private func onClosed(_ closed: LocalControlConnection) {
        guard closed === connection else { return }
        connection = nil
        eventTask?.cancel()
        eventTask = nil
        cancelAITasks()
        for file in incoming.values { discardIncoming(file) }
        incoming.removeAll()
        if status == .failed { return }
        let wasConnected = status == .connected
        setStatus(wasConnected ? .idle : .failed, error: wasConnected ? "" : "The desktop closed the connection.")
        if !autoConnectHeld, let host, isKnown(host.nodeId) { scheduleRetry(3) }
    }

    /// Every `{event, data}` frame the desktop sends, for features beyond agents and files (the
    /// Hosts area's `hosts.*` events). Taps run after this service has handled the frame.
    @discardableResult
    public func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        eventTaps[id] = tap
        return id
    }

    public func removeEventTap(_ id: UUID) { eventTaps.removeValue(forKey: id) }

    /// One request on the open channel, for methods this service does not wrap itself (`hosts.*`,
    /// `agents.transcript` with `since`). Throws `LocalControlException("not_connected")` when no
    /// desktop is connected.
    public func call(_ method: String, _ params: JSONObject = [:], timeout: TimeInterval = 20) async throws -> JSONObject {
        try await request(method, params, timeout: timeout)
    }

    private func onEvent(_ message: JSONObject) {
        defer {
            let name = message["event"]?.stringValue ?? ""
            let payload = message["data"]?.objectValue ?? JSONObject()
            for tap in eventTaps.values { tap(name, payload) }
        }
        let data = message["data"]?.objectValue ?? JSONObject()
        switch message["event"]?.stringValue ?? "" {
        case "session":
            let name = dartString(data["name"])
            if !name.isEmpty { desktopName = name }
            let user = dartString(data["user"])
            if !user.isEmpty { desktopUser = user }
            if data["paired"] == .bool(true) {
                pairingCode = ""
                rememberPaired()
                if let host { rememberAddress(desktopKey, host.address.address) }
                setStatus(.connected)
                announceCapabilities()
                Task { await self.refreshAgents() }
            } else {
                let code = data["code"].flatMap { $0.isNull ? nil : $0.text } ?? connection?.pairingCode ?? ""
                pairingCode = code
                setStatus(.awaitingApproval)
            }
        case "pairing-denied":
            autoConnectHeld = true
            setStatus(.failed, error: "The desktop declined this phone.")
            connection?.close()
        case "agents.changed":
            Task { await self.refreshAgents() }
        case "file.begin":
            beginIncoming(data)
        case "file.chunk":
            chunkIncoming(data)
        case "file.end":
            endIncoming(data, complete: true)
        case "file.cancel":
            endIncoming(data, complete: false)
        case LocalControlService.aiRequestEvent:
            handleAIRequest(data)
        case LocalControlService.aiCancelEvent:
            if case .int(let rid)? = data["rid"] { aiTasks.removeValue(forKey: rid)?.cancel() }
        default:
            break
        }
    }

    private func rememberPaired() {
        let key = desktopKey
        if key.isEmpty { return }
        let existing = paired.first { $0.key == key }
        let entry = PairedDesktop(
            key: key, name: desktopName, user: desktopUser, pairedAtMs: existing?.pairedAtMs ?? clock.nowMillis)
        paired = paired.filter { $0.key != key } + [entry]
        savePaired()
    }

    /// Closes the channel. With `hold`, a paired desktop is not reconnected to until `connect` is
    /// called again.
    public func disconnect(hold: Bool = true) async {
        if hold { autoConnectHeld = true }
        retryTask?.cancel()
        retryTask = nil
        let current = connection
        connection = nil
        eventTask?.cancel()
        eventTask = nil
        cancelAITasks()
        current?.close()
        agents = nil
        pairingCode = ""
        setStatus(.idle)
    }

    public func resetForProjectSwitch() {
        epoch &+= 1
        retryTask?.cancel()
        retryTask = nil
        let current = connection
        connection = nil
        eventTask?.cancel()
        eventTask = nil
        cancelAITasks()
        current?.close()
        for file in incoming.values { discardIncoming(file) }
        incoming.removeAll()
        agents = nil
        pairingCode = ""
        autoConnectHeld = false
        accountDesktopsTried = []
        accountDesktopKeys = []
        failures = 0
        host = nil
        desktopKey = ""
        desktopName = ""
        desktopUser = ""
        status = .idle
        error = ""
        paired = readPaired()
        notifyListeners()
        Task { [weak self] in self?.onMeshChanged() }
    }

    /// Drops a desktop's approval on this side; the desktop asks again next time.
    public func forget(_ key: String) async {
        paired = paired.filter { $0.key != key }
        savePaired()
        if desktopKey == key { await disconnect() }
        notifyListeners()
    }

    private func request(_ method: String, _ params: JSONObject = [:], timeout: TimeInterval = 20) async throws -> JSONObject {
        guard let connection, connected else { throw LocalControlException("not_connected") }
        return try await connection.request(method, params, timeout: timeout)
    }

    /// Re-reads the desktop's Agents list. Bells that ring while one read is in flight fold into a
    /// single follow-up read.
    public func refreshAgents() async {
        if let inFlight = agentsInFlight {
            agentsAgain = true
            await inFlight.value
            return
        }
        let task = Task { [weak self] in
            guard let self else { return }
            self.agentsLoading = true
            self.notifyListeners()
            do {
                repeat {
                    self.agentsAgain = false
                    let result = try await self.request("agents.list")
                    self.agents = LocalAgentsSnapshot.fromJSON(result)
                    self.notifyListeners()
                } while self.agentsAgain && self.connected
            } catch {
                // The status line already says why the desktop is out of reach.
            }
            self.agentsLoading = false
            self.agentsInFlight = nil
            self.notifyListeners()
        }
        agentsInFlight = task
        await task.value
    }

    /// Starts a new agent in the desktop's checkout of `owner`/`name`; returns its session id.
    public func startAgent(owner: String, name: String, task: String, provider: String = "") async throws -> Int {
        var params: JSONObject = ["owner": .string(owner), "name": .string(name), "task": .string(task)]
        if !provider.isEmpty { params["provider"] = .string(provider) }
        let result = try await request("agents.start", params)
        if case .int(let id)? = result["id"] { return Int(id) }
        return 0
    }

    public func sendPrompt(_ sessionId: Int, _ text: String) async throws {
        _ = try await request("agents.prompt", ["id": .int(Int64(sessionId)), "text": .string(text)])
    }

    public func stopAgent(_ sessionId: Int) async throws {
        _ = try await request("agents.stop", ["id": .int(Int64(sessionId))])
    }

    public func continueAgent(_ sessionId: Int) async throws {
        _ = try await request("agents.continue", ["id": .int(Int64(sessionId))])
    }

    public func transcript(_ sessionId: Int) async throws -> String {
        let result = try await request("agents.transcript", ["id": .int(Int64(sessionId))])
        return dartString(result["transcript"])
    }

    public func transcriptDetail(_ sessionId: Int) async throws -> LocalAgentTranscript {
        LocalAgentTranscript.fromJSON(try await request("agents.transcript", ["id": .int(Int64(sessionId))]), sessionId: sessionId)
    }

    public func attachment(_ sessionId: Int, _ attachmentId: String) async throws -> Data {
        try await attachmentPayload(sessionId, attachmentId, preview: false).data
    }

    public func attachmentPayload(_ sessionId: Int, _ attachmentId: String, preview: Bool) async throws
        -> AgentAttachmentPayload
    {
        let result = try await request(
            "agents.attachment",
            LocalAgentTranscript.attachmentParams(sessionId: sessionId, attachmentId: attachmentId, preview: preview))
        return try LocalAgentTranscript.attachmentPayload(result)
    }

    public nonisolated func attachmentFetch() -> @Sendable (AgentAttachmentRef) async throws -> Data {
        { [weak self] ref in
            guard let self else { throw AgentAttachmentError("The desktop is not connected.") }
            guard case .desktop(let sessionId) = ref.origin else {
                throw AgentAttachmentError("This attachment is not on the desktop.")
            }
            do {
                return try await self.attachment(sessionId, ref.meta.id)
            } catch let error as LocalControlException {
                throw AgentAttachmentError(LocalAgentTranscript.attachmentErrorText(error.code))
            }
        }
    }

    // MARK: Files

    /// Sends a file to the desktop, which saves it with its downloads. Returns the name it was
    /// saved under.
    public func sendFile(_ url: URL, name: String? = nil) async throws -> String {
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        let size = (attributes[.size] as? NSNumber)?.intValue ?? 0
        let fileName = name ?? url.lastPathComponent
        uploadName = fileName
        uploadProgress = 0
        notifyListeners()
        var transfer: String?
        defer {
            if let pendingTransfer = transfer, !pendingTransfer.isEmpty, connected {
                Task { _ = try? await self.request("files.cancel", ["transfer": .string(pendingTransfer)]) }
            }
            uploadProgress = nil
            uploadName = ""
            notifyListeners()
        }
        let begin = try await request("files.begin", ["name": .string(fileName), "size": .int(Int64(size))])
        transfer = dartString(begin["transfer"])
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        var sent = 0
        while true {
            let chunk = handle.readData(ofLength: controlFileChunkBytes)
            if chunk.isEmpty { break }
            _ = try await request("files.chunk", ["transfer": .string(transfer ?? ""), "data": .string(B64.encode(chunk))])
            sent += chunk.count
            uploadProgress = size == 0 ? 1 : Double(sent) / Double(size)
            notifyListeners()
        }
        let end = try await request("files.end", ["transfer": .string(transfer ?? "")])
        transfer = nil
        let saved = dartString(end["name"])
        return end["name"] == nil || end["name"] == .null ? fileName : saved
    }

    private func beginIncoming(_ data: JSONObject) {
        let transfer = dartString(data["transfer"])
        guard case .int(let size)? = data["size"], !transfer.isEmpty, size >= 0 else { return }
        let fileName = LocalControlService.safeName(dartString(data["name"]))
        let file = IncomingFile(name: fileName, size: Int(size))
        incoming[transfer] = file
        let directory = filesDirectory()
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        } catch {
            return
        }
        var path = directory.appendingPathComponent(fileName).path
        var n = 2
        while FileManager.default.fileExists(atPath: path) {
            let base = fileName as NSString
            let ext = base.pathExtension
            let dot = fileName.lastIndex(of: ".")
            if let dot, dot > fileName.startIndex, !ext.isEmpty {
                path = directory.appendingPathComponent("\(fileName[..<dot]) (\(n))\(fileName[dot...])").path
            } else {
                path = directory.appendingPathComponent("\(fileName) (\(n))").path
            }
            n += 1
        }
        file.path = path
        _ = FileManager.default.createFile(atPath: "\(path).part", contents: nil)
        file.handle = FileHandle(forWritingAtPath: "\(path).part")
    }

    private func chunkIncoming(_ data: JSONObject) {
        guard let file = incoming[dartString(data["transfer"])] else { return }
        guard let bytes = try? B64.decode(dartString(data["data"])) else { return }
        file.received += bytes.count
        file.handle?.write(bytes)
    }

    private func endIncoming(_ data: JSONObject, complete: Bool) {
        guard let file = incoming.removeValue(forKey: dartString(data["transfer"])) else { return }
        if !complete || file.received != file.size {
            discardIncoming(file)
            return
        }
        try? file.handle?.close()
        file.handle = nil
        do {
            try FileManager.default.moveItem(atPath: "\(file.path).part", toPath: file.path)
            receivedFiles.insert(
                LocalReceivedFile((file.path as NSString).lastPathComponent, file.path, file.size, clock.nowMillis),
                at: 0)
            notifyListeners()
        } catch {}
    }

    private func discardIncoming(_ file: IncomingFile) {
        try? file.handle?.close()
        file.handle = nil
        if !file.path.isEmpty { try? FileManager.default.removeItem(atPath: "\(file.path).part") }
    }

    nonisolated static func safeName(_ name: String) -> String {
        var base = name.replacingOccurrences(of: "\\", with: "/").components(separatedBy: "/").last ?? ""
        base = String(
            base.unicodeScalars.map { scalar -> Character in
                let v = scalar.value
                let ok =
                    (v >= 65 && v <= 90) || (v >= 97 && v <= 122) || (v >= 48 && v <= 57) || v == 46 || v == 95
                    || v == 45 || v == 40 || v == 41 || v == 32
                return ok ? Character(scalar) : "_"
            }
        ).dartTrimmed
        while base.hasPrefix(".") { base.removeFirst() }
        if base.count > 120 { base = String(base.suffix(120)) }
        return base.isEmpty ? "file" : base
    }

    // MARK: On-device AI for paired desktops
    //
    // desktop -> phone  {event: "ai.request", data: {rid, method: "ai.info"|"ai.chat", params, stream}}
    //                   {event: "ai.cancel",  data: {rid}}
    // phone -> desktop  {event: "ai.delta",     data: {rid, text}}           (stream only)
    //                   {event: "ai.tool_call", data: {rid, tool_call}}      (stream only)
    //                   {event: "ai.response",  data: {rid, ok, ...}}        (exactly one per request)
    //                   {event: "node.capabilities", data: {ai: {v, enabled, methods, stream}}}

    nonisolated public static let aiRequestEvent = "ai.request"
    nonisolated public static let aiCancelEvent = "ai.cancel"
    nonisolated public static let aiResponseEvent = "ai.response"
    nonisolated public static let capabilitiesEvent = "node.capabilities"

    /// Whether paired desktops may use this phone's on-device model (off by default).
    public var servesLocalAI: Bool {
        _ = revision
        return prefs.bool(forKey: LocalControlService.serveLocalAIKey) ?? false
    }

    public func setServesLocalAI(_ on: Bool) {
        prefs.set(on, forKey: LocalControlService.serveLocalAIKey)
        if !on { cancelAITasks() }
        notifyListeners()
        announceCapabilities()
    }

    /// Whether this build has an on-device model to offer at all.
    public var localAIAvailable: Bool { inferenceProvider?() != nil }

    /// Tells the connected (paired) desktop what this phone serves. Sent on every approval and on
    /// every change of the switch; a desktop that does not know the event ignores it.
    private func announceCapabilities() {
        guard let connection, connected else { return }
        let enabled = servesLocalAI && inferenceProvider?() != nil
        var ai: JSONObject = ["v": 1, "enabled": .bool(enabled)]
        if enabled {
            ai["methods"] = [.string(LocalInferenceService.infoMethod), .string(LocalInferenceService.chatMethod)]
            ai["stream"] = true
        }
        connection.sendEvent(LocalControlService.capabilitiesEvent, ["ai": .object(ai)])
    }

    private func handleAIRequest(_ data: JSONObject) {
        guard case .int(let rid)? = data["rid"], rid > 0 else { return }
        // Only the desktop this phone is paired with and connected to, over the authenticated
        // channel, may ask; anything else is not answered at all.
        guard let connection, connected, isPaired(desktopKey) else { return }
        let method = dartString(data["method"])
        let params = data["params"]?.objectValue ?? JSONObject()
        let stream = data["stream"] == .bool(true)
        if !servesLocalAI {
            replyAI(connection, rid, LocalControlService.aiError("disabled", "This phone does not share its on-device AI."))
            return
        }
        guard let service = inferenceProvider?() else {
            replyAI(connection, rid, LocalControlService.aiError("unavailable", "This phone has no on-device AI."))
            return
        }
        if aiTasks[rid] != nil {
            replyAI(connection, rid, LocalControlService.aiError("bad_request", "That request id is already running."))
            return
        }
        switch method {
        case LocalInferenceService.infoMethod:
            aiTasks[rid] = Task { [weak self] in
                let info = await service.info()
                self?.finishAI(connection, rid, info)
            }
        case LocalInferenceService.chatMethod where stream:
            aiTasks[rid] = Task { [weak self] in
                for await item in service.stream(params) {
                    guard let self, !Task.isCancelled else { return }
                    self.forwardAI(connection, rid, item)
                }
            }
        case LocalInferenceService.chatMethod:
            aiTasks[rid] = Task { [weak self] in
                let reply = await service.respond(to: params)
                self?.finishAI(connection, rid, reply)
            }
        default:
            replyAI(connection, rid, LocalControlService.aiError("unknown_method", "Unknown method \(method)."))
        }
    }

    private func forwardAI(_ connection: LocalControlConnection, _ rid: Int64, _ item: JSONObject) {
        var body = item
        let event = dartString(body.removeValue(forKey: "event"))
        switch event {
        case LocalInferenceService.deltaEvent, LocalInferenceService.toolCallEvent:
            var data: JSONObject = ["rid": .int(rid)]
            data.merge(body)
            connection.sendEvent(event, data)
        default:
            // ai.done / ai.error: the terminal reply.
            finishAI(connection, rid, body)
        }
    }

    private func finishAI(_ connection: LocalControlConnection, _ rid: Int64, _ result: JSONObject) {
        if aiTasks.removeValue(forKey: rid) == nil { return }
        replyAI(connection, rid, result)
    }

    private func replyAI(_ connection: LocalControlConnection, _ rid: Int64, _ result: JSONObject) {
        var data: JSONObject = ["rid": .int(rid)]
        var body = result
        body.removeValue(forKey: "event")
        data.merge(body)
        if connection.sendEvent(LocalControlService.aiResponseEvent, data) {
            aiRequestsServed += 1
        }
    }

    nonisolated static func aiError(_ code: String, _ message: String) -> JSONObject {
        ["ok": false, "error": .string(code), "message": .string(message)]
    }

    private func cancelAITasks() {
        let running = aiTasks
        aiTasks.removeAll()
        for (_, task) in running { task.cancel() }
    }

    // MARK: Teardown

    public func dispose() {
        disposed = true
        if let meshToken { mesh.removeListener(meshToken) }
        retryTask?.cancel()
        eventTask?.cancel()
        cancelAITasks()
        connection?.close()
    }
}

extension LocalControlService: ShellLocalDesktopBridge {
    /// The top bar's desktop button.
    public var localDesktop: ShellLocalDesktop {
        switch status {
        case .connected: return .connected(desktopName)
        case .awaitingApproval: return .awaitingApproval(desktopName)
        case .connecting: return .connecting(desktopName)
        case .idle, .failed: return hosts.isEmpty ? .hidden : .inReach
        }
    }
}
