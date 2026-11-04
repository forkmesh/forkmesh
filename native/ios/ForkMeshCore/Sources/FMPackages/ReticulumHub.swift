import FMBase
import Foundation
import Observation

public struct ReticulumNotice: Equatable, Sendable {
    public let text: String
    public let isError: Bool

    public init(_ text: String, isError: Bool = false) {
        self.text = text
        self.isError = isError
    }
}

public struct ReticulumTile: Equatable, Sendable, Identifiable {
    public let id: Int
    public let label: String
    public let value: String
    public let detail: String
    public let tone: Int
    public let spark: [Double]
}

public struct ReticulumRow: Equatable, Sendable, Identifiable {
    public let label: String
    public let value: String
    public let mono: Bool
    public var id: String { label }

    public init(_ label: String, _ value: String, mono: Bool = false) {
        self.label = label
        self.value = value
        self.mono = mono
    }
}

public enum ReticulumTab: Int, CaseIterable, Sendable {
    case network
    case messages
    case interfaces
    case destinations
    case activity
    case identity

    public var title: String {
        switch self {
        case .network: return "Network"
        case .messages: return "Messages"
        case .interfaces: return "Interfaces"
        case .destinations: return "Destinations"
        case .activity: return "Activity"
        case .identity: return "Identity"
        }
    }
}

public enum ReticulumSort: String, CaseIterable, Sendable {
    case lastHeard
    case hops
    case announces
    case name

    public var title: String {
        switch self {
        case .lastHeard: return "Last heard"
        case .hops: return "Hops"
        case .announces: return "Announces"
        case .name: return "Name"
        }
    }
}

public struct ReticulumInterfaceRow: Equatable, Sendable, Identifiable {
    public let config: ReticulumInterfaceConfig
    public let status: ReticulumInterfaceStatus
    public let onlineFor: String
    public let heard: Int
    public var id: String { config.id }
}

public struct ReticulumDiscoveredRow: Equatable, Sendable, Identifiable {
    public let hash: [UInt8]
    public let found: ReticulumDiscoveredInterface
    public let name: String
    public let address: String
    public let proof: String
    public let location: String
    public let heard: String
    public let usable: Bool
    public var id: [UInt8] { hash }
}

public struct ReticulumAnnounceInterval: Equatable, Sendable, Identifiable {
    public let minutes: Int
    public let label: String
    public var id: Int { minutes }

    public static let choices: [ReticulumAnnounceInterval] = [
        ReticulumAnnounceInterval(minutes: 0, label: "Only when I tap Announce"),
        ReticulumAnnounceInterval(minutes: 15, label: "Every 15 minutes"),
        ReticulumAnnounceInterval(minutes: 30, label: "Every 30 minutes"),
        ReticulumAnnounceInterval(minutes: 60, label: "Every hour"),
        ReticulumAnnounceInterval(minutes: 180, label: "Every 3 hours"),
    ]
}

@MainActor
@Observable
public final class ReticulumHub {
    public static let packageId = "reticulum"
    public static let title = "Reticulum"
    static let prefix = "market/packages/reticulum/data/"
    public static let interfacesKey = prefix + "interfaces"
    public static let identityKey = prefix + "identity"
    public static let announceNameKey = prefix + "announceName"
    public static let announceMinutesKey = prefix + "announceMinutes"
    public static let answerPathsKey = prefix + "answerPathRequests"
    public static let runningKey = prefix + "networkOn"
    public static let passphrasePrefix = prefix + "passphrase/"
    public static let messagesFile = "messages.json"
    public static let phoneKinds: Set<ReticulumInterfaceKind> = [.tcp]
    public static let pulseSeconds: Double = 1.6

    @ObservationIgnored public let node: ReticulumNode
    @ObservationIgnored private let prefs: KeyValueStore
    @ObservationIgnored private let secure: SecureStore
    @ObservationIgnored private let scheduler: ReticulumScheduler
    @ObservationIgnored private var announceTimer: ReticulumTimer?
    @ObservationIgnored private var started = false
    @ObservationIgnored private var previousOnline = 0
    @ObservationIgnored private let device: String
    @ObservationIgnored public let messenger: ReticulumMessenger
    @ObservationIgnored private let disk: RSSReaderDisk?
    @ObservationIgnored public var onIncomingMessage: ((ReticulumChatMessage, String) -> Void)?
    public var visible = false
    public var chatVisible = false
    public var currentPeer: [UInt8] = []
    public var composerText = ""

    public private(set) var revision = 0
    public private(set) var pulses: [[UInt8]: Double] = [:]
    public var tab: ReticulumTab = .network
    public var selected: [UInt8] = []
    public var selectedInterface = ""
    public var notice: ReticulumNotice?
    public var mapFilter = ""
    public var destinationFilter = ""
    public var kindFilter: ReticulumMapGroup?
    public var sort: ReticulumSort = .lastHeard
    public var lookupText = ""
    public var lookupHint = ""
    public var announceName = ""
    public private(set) var announceMinutes = 0
    public private(set) var answerPathRequests = true

    public init(prefs: KeyValueStore, secure: SecureStore, connector: ReticulumConnector, scheduler: ReticulumScheduler,
                accountName: String = "", device: String = "iPhone", disk: RSSReaderDisk? = nil) {
        self.prefs = prefs
        self.secure = secure
        self.scheduler = scheduler
        self.device = device
        self.disk = disk
        node = ReticulumNode(scheduler: scheduler, connector: connector)
        messenger = ReticulumMessenger(node: node, scheduler: scheduler)
        node.onChange = { [weak self] in self?.nodeChanged() }
        node.onAnnounce = { [weak self] hash in
            self?.pulse(hash)
            self?.messenger.destinationAnnounced(hash)
        }
        node.packetHandler = { [weak self] id, packet, raw, hops in
            self?.messenger.handlePacket(interfaceId: id, packet: packet, raw: raw, hops: hops)
        }
        messenger.onChange = { [weak self] in self?.revision += 1 }
        messenger.onSave = { [weak self] json in self?.disk?.write(Self.messagesFile, Data(json.utf8)) }
        messenger.onIncoming = { [weak self] message in self?.incoming(message) }
        loadSettings(accountName: accountName)
    }

    private func incoming(_ message: ReticulumChatMessage) {
        if (chatVisible || (visible && tab == .messages)) && currentPeer == message.peer {
            messenger.markRead(message.peer)
            return
        }
        onIncomingMessage?(message, messenger.peerName(message.peer))
    }

    public var deviceNoun: String { device }

    private func loadSettings(accountName: String) {
        let stored = prefs.string(forKey: Self.interfacesKey) ?? ""
        let configs = (stored.isEmpty ? [] : ReticulumInterfaceConfig.list(fromJSON: stored)).map { config -> ReticulumInterfaceConfig in
            var filled = config
            if let passphrase = secure.read(Self.passphrasePrefix + config.id), !passphrase.isEmpty { filled.passphrase = passphrase }
            return filled
        }
        node.setInterfaces(configs)
        ensureIdentity()
        let name = prefs.string(forKey: Self.announceNameKey) ?? ""
        node.setAnnounceName(name.isEmpty ? accountName : name)
        announceName = node.announceName
        messenger.setDisplayName(node.announceName)
        if let data = disk?.read(Self.messagesFile) { messenger.load(String(decoding: data, as: UTF8.self)) }
        let minutes = prefs.int(forKey: Self.announceMinutesKey) ?? 0
        announceMinutes = ReticulumAnnounceInterval.choices.contains { $0.minutes == minutes } ? minutes : 0
        answerPathRequests = prefs.bool(forKey: Self.answerPathsKey) ?? true
        node.answerPathRequests = answerPathRequests
        updateAnnounceTimer()
    }

    private func ensureIdentity() {
        let stored = secure.read(Self.identityKey) ?? ""
        if let bytes = CryptoHex.decode(stored), let identity = ReticulumIdentity(privateKey: bytes) {
            node.setIdentity(identity)
            return
        }
        node.setIdentity(ReticulumIdentity.generate())
        saveIdentity()
    }

    private func saveIdentity() {
        guard node.identity.hasPrivateKey else { return }
        secure.write(Self.identityKey, ReticulumWire.hex(node.identity.privateKey))
    }

    private func saveInterfaces() {
        let configs = node.interfaces
        let objects: [JSONValue] = configs.map { config in
            var object = config.json
            object["passphrase"] = nil
            if config.usesAccessCode { object["accessCodeSize"] = .int(Int64(config.accessCodeSize)) }
            return .object(object)
        }
        prefs.set(JSON.encode(.array(objects)), forKey: Self.interfacesKey)
        let ids = Set(configs.map(\.id))
        for config in configs {
            let key = Self.passphrasePrefix + config.id
            if config.passphrase.isEmpty {
                if secure.read(key) != nil { secure.delete(key) }
            } else if secure.read(key) != config.passphrase {
                secure.write(key, config.passphrase)
            }
        }
        for key in secure.readAll().keys where key.hasPrefix(Self.passphrasePrefix) && !ids.contains(String(key.dropFirst(Self.passphrasePrefix.count))) {
            secure.delete(key)
        }
    }

    public func saveNow() { messenger.flush() }

    private func nodeChanged() {
        let online = node.onlineCount
        if online > 0 && previousOnline == 0 && announceMinutes > 0 {
            _ = scheduler.after(3) { [weak self] in
                guard let self else { return }
                self.node.announce()
            }
        }
        previousOnline = online
        revision += 1
    }

    private func pulse(_ hash: [UInt8]) {
        let now = scheduler.now
        pulses = pulses.filter { now - $0.value <= Self.pulseSeconds }
        pulses[hash] = now
    }

    public func activePulses(at now: Double) -> [[UInt8]: Double] {
        pulses.filter { now - $0.value >= 0 && now - $0.value <= Self.pulseSeconds }
    }

    public var now: Double { scheduler.now }

    public func open() {
        if !started {
            started = true
            if prefs.bool(forKey: Self.runningKey) ?? true { node.start() }
        } else if node.running {
            for config in node.interfaces where config.enabled && node.status(config.id).state != .online {
                node.reconnectInterface(config.id)
            }
        }
        revision += 1
    }

    public func show(_ text: String, error: Bool = false) {
        notice = text.isEmpty ? nil : ReticulumNotice(text, isError: error)
    }

    public func toggleNetwork() {
        if node.running {
            node.stop()
            prefs.set(false, forKey: Self.runningKey)
            show("Disconnected from every interface. Nothing is sent until you connect again.")
        } else {
            node.start()
            prefs.set(true, forKey: Self.runningKey)
            notice = nil
        }
        revision += 1
    }

    public func addInterface(_ config: ReticulumInterfaceConfig) {
        if let existing = node.interfaces.first(where: { $0.sameTarget(config) }) {
            show("\(config.endpoint) is already in your interfaces as \(existing.name).")
            if !existing.enabled { node.setInterfaceEnabled(existing.id, true) }
            saveInterfaces()
            return
        }
        node.upsertInterface(config)
        if !node.running { node.start() }
        saveInterfaces()
        show("Added \(config.name) (\(config.endpoint)).")
        revision += 1
    }

    public func addPublicEntryPoint(_ entry: ReticulumInterfaceConfig) {
        var copy = entry
        copy.id = ReticulumInterfaceConfig.newId()
        addInterface(copy)
    }

    public func joinPublicNetwork() {
        guard let first = ReticulumInterfaceConfig.publicEntryPoints.first else { return }
        addPublicEntryPoint(first)
        prefs.set(true, forKey: Self.runningKey)
        tab = .network
    }

    public func saveInterface(_ config: ReticulumInterfaceConfig, creating: Bool) -> String? {
        if let problem = config.validationError { return problem }
        if !Self.phoneKinds.contains(config.kind) { return "Only TCP interfaces can run on this \(device)." }
        if creating {
            addInterface(config)
        } else {
            node.upsertInterface(config)
            saveInterfaces()
            revision += 1
        }
        return nil
    }

    public func setInterfaceEnabled(_ id: String, _ enabled: Bool) {
        node.setInterfaceEnabled(id, enabled)
        saveInterfaces()
        revision += 1
    }

    public func reconnect(_ id: String) {
        node.reconnectInterface(id)
        revision += 1
    }

    public func removeInterface(_ id: String) {
        node.removeInterface(id)
        if selectedInterface == id { selectedInterface = "" }
        saveInterfaces()
        revision += 1
    }

    public func discoveredDraft(_ hash: [UInt8]) -> ReticulumInterfaceConfig? {
        guard let found = node.destinations[hash]?.discovery, found.connectable, found.stampValid else { return nil }
        var config = ReticulumInterfaceConfig(id: ReticulumInterfaceConfig.newId(), name: found.name.isEmpty ? found.reachableOn : found.name,
                                              host: found.reachableOn, port: found.port)
        config.kiss = found.type == "KISSInterface"
        config.name = String(decoding: Array(config.name.utf16.prefix(80)), as: UTF16.self)
        return config
    }

    @discardableResult
    public func importConfig(_ text: String, from source: String) -> String {
        let parsed = ReticulumConfigImport.parse(text)
        var skipped = parsed.skipped
        var added = 0
        for config in parsed.interfaces {
            guard Self.phoneKinds.contains(config.kind) else {
                skipped.append("\(config.name) (\(config.kind == .shared ? "no Reticulum daemon on this \(device)" : "UDP is not available on this \(device)"))")
                continue
            }
            if node.interfaces.contains(where: { $0.sameTarget(config) }) { continue }
            node.upsertInterface(config)
            added += 1
        }
        saveInterfaces()
        revision += 1
        var message = "Imported \(added) interface\(added == 1 ? "" : "s") from \(source)."
        if !skipped.isEmpty {
            message += " Skipped (not supported here, or protected by a network name): \(skipped.joined(separator: ", "))."
        }
        show(message)
        return message
    }

    public func commitAnnounceName() {
        node.setAnnounceName(announceName)
        messenger.setDisplayName(node.announceName)
        prefs.set(node.announceName, forKey: Self.announceNameKey)
        announceName = node.announceName
    }

    public func announceNow() {
        commitAnnounceName()
        if node.announce() {
            let count = node.onlineCount
            show("Announced \(ReticulumWire.pretty(node.destinationHash)) on \(count) interface\(count == 1 ? "" : "s").")
        } else {
            show("Connect an interface first.", error: true)
        }
        revision += 1
    }

    public func setAnnounceMinutes(_ minutes: Int) {
        announceMinutes = minutes
        prefs.set(minutes, forKey: Self.announceMinutesKey)
        updateAnnounceTimer()
    }

    public func setAnswerPathRequests(_ on: Bool) {
        answerPathRequests = on
        node.answerPathRequests = on
        prefs.set(on, forKey: Self.answerPathsKey)
    }

    private func updateAnnounceTimer() {
        announceTimer?.cancel()
        announceTimer = nil
        guard announceMinutes > 0 else { return }
        announceTimer = scheduler.after(Double(announceMinutes * 60)) { [weak self] in
            guard let self else { return }
            if self.node.onlineCount > 0 { self.node.announce() }
            self.updateAnnounceTimer()
        }
    }

    public func newIdentity() {
        node.setIdentity(ReticulumIdentity.generate())
        saveIdentity()
        show("New identity \(ReticulumWire.pretty(node.destinationHash)). Announce it so others can find this \(device).")
        revision += 1
    }

    public func requestPath(_ hash: [UInt8]) {
        guard hash.count == Reticulum.hashLength else { return }
        if node.requestPath(hash).isEmpty {
            show("Connect an interface first.", error: true)
            return
        }
        lookupText = ReticulumWire.hex(hash)
        lookupHint = ""
        revision += 1
    }

    public func lookup() {
        let hash = ReticulumWire.parseHash(lookupText)
        guard !hash.isEmpty else {
            lookupHint = "A destination hash is 32 hexadecimal characters, like the ones in the list below."
            return
        }
        requestPath(hash)
    }

    public func select(_ hash: [UInt8], showMap: Bool = false) {
        selected = hash
        if showMap { tab = .network }
    }

    public func forget(_ hash: [UInt8]) {
        node.forgetDestination(hash)
        if selected == hash { selected = [] }
        revision += 1
    }

    public func clearDestinations() {
        node.clearDestinations()
        selected = []
        revision += 1
    }

    public func interfaceName(_ id: String) -> String {
        if let config = node.interfaces.first(where: { $0.id == id }) { return config.name }
        return id.isEmpty ? "—" : "Removed interface"
    }

    public var enabledCount: Int { node.interfaces.filter(\.enabled).count }

    public var summary: String {
        _ = revision
        guard node.running else { return "Disconnected" }
        return "\(node.onlineCount) of \(enabledCount) interfaces online · you are \(ReticulumWire.short(node.destinationHash))"
    }

    public var showsOnboarding: Bool {
        _ = revision
        return !node.interfaces.contains { $0.kind != .shared } && node.onlineCount == 0
    }

    public var canAnnounce: Bool {
        _ = revision
        return node.onlineCount > 0
    }

    public var running: Bool {
        _ = revision
        return node.running
    }

    public var tiles: [ReticulumTile] {
        _ = revision
        let online = node.onlineCount
        let enabled = enabledCount
        let status: (String, String, Int)
        if !node.running {
            status = ("Off", "Disconnected by you", 0)
        } else if online > 0 {
            status = ("Online", "\(online) of \(enabled) interfaces up", 1)
        } else if enabled > 0 {
            status = ("Connecting", "No interface is up yet", 2)
        } else {
            status = ("No interfaces", "Add one in Interfaces", 0)
        }
        let now = scheduler.now
        let records = node.destinations.values
        let recent = records.filter { now - $0.firstHeard < 600 }.count
        let nearest = records.filter { $0.hops <= 1 }.count
        let history = node.history
        let lastMinute = history.suffix(60).reduce(0) { $0 + $1.announces }
        let window = Double(min(10, max(1, history.count)))
        let rx = history.suffix(10).reduce(Int64(0)) { $0 + $1.rx }
        let tx = history.suffix(10).reduce(Int64(0)) { $0 + $1.tx }
        var answered = 0
        var pending = 0
        var fastest: Int64 = -1
        for lookup in node.lookups.values {
            if let ms = lookup.answerMs {
                answered += 1
                fastest = fastest < 0 ? ms : min(fastest, ms)
            } else if !lookup.timedOut {
                pending += 1
            }
        }
        let paths = pending > 0 ? "\(pending) waiting for an answer" : fastest >= 0 ? "Fastest answer \(fastest) ms" : "Ask for one in Destinations"
        return [
            ReticulumTile(id: 0, label: "Connection", value: status.0, detail: status.1, tone: status.2, spark: []),
            ReticulumTile(id: 1, label: "Destinations", value: "\(node.destinations.count)",
                          detail: "\(recent) new in 10 min · \(nearest) one hop away", tone: 0, spark: []),
            ReticulumTile(id: 2, label: "Announces", value: "\(lastMinute)/min",
                          detail: "\(node.counters.announces) heard · \(node.counters.rejectedAnnounces) rejected", tone: 0,
                          spark: history.map { Double($0.announces) }),
            ReticulumTile(id: 3, label: "Traffic", value: ReticulumFormat.rate(Double(rx + tx) / window),
                          detail: "↓ \(ReticulumFormat.rate(Double(rx) / window)) · ↑ \(ReticulumFormat.rate(Double(tx) / window))",
                          tone: 0, spark: history.map { Double($0.rx + $0.tx) }),
            messagesTile,
            ReticulumTile(id: 5, label: "Path lookups", value: "\(answered) found", detail: paths, tone: 0, spark: []),
        ]
    }

    public var spokes: [ReticulumMapSpoke] {
        _ = revision
        return node.interfaces.map { ReticulumMapSpoke(id: $0.id, name: $0.name, online: node.status($0.id).state == .online) }
    }

    public var destinationList: [ReticulumDestination] {
        _ = revision
        return Array(node.destinations.values)
    }

    public var history: [ReticulumTrafficSample] {
        _ = revision
        return node.history
    }

    public var selectedRecord: ReticulumDestination? {
        _ = revision
        return node.destinations[selected]
    }

    public static func matches(_ record: ReticulumDestination, filter: String) -> Bool {
        let needle = filter.trimmingCharacters(in: .whitespacesAndNewlines)
        if needle.isEmpty { return true }
        return record.displayName.localizedCaseInsensitiveContains(needle) || record.label.localizedCaseInsensitiveContains(needle)
            || ReticulumWire.hex(record.hash).localizedCaseInsensitiveContains(needle)
    }

    public var detailTitle: String {
        guard let record = selectedRecord else { return "Nothing selected" }
        return record.displayName.isEmpty ? record.label : record.displayName
    }

    public var detailRows: [ReticulumRow] {
        guard let record = selectedRecord else { return [] }
        let now = scheduler.now
        var rows: [ReticulumRow] = [
            ReticulumRow("Application", record.aspect.isEmpty ? record.label : "\(record.label) (\(record.aspect))"),
            ReticulumRow("Destination", ReticulumWire.hex(record.hash), mono: true),
            ReticulumRow("Identity", ReticulumWire.hex(record.identityHash), mono: true),
            ReticulumRow("Distance", ReticulumFormat.hops(record.hops)),
            ReticulumRow("Heard via", interfaceName(record.interfaceId)),
        ]
        if !record.transportId.isEmpty { rows.append(ReticulumRow("Next hop", ReticulumWire.hex(record.transportId), mono: true)) }
        rows.append(ReticulumRow("First heard", ReticulumFormat.age(record.firstHeard, now: now)))
        rows.append(ReticulumRow("Last heard", ReticulumFormat.age(record.lastHeard, now: now)))
        if record.emittedAt > 0 { rows.append(ReticulumRow("Announced at", ReticulumFormat.timestamp(record.emittedAt))) }
        rows.append(ReticulumRow("Announces", "\(record.announces)"))
        rows.append(ReticulumRow("Forward secrecy", record.ratchet ? "Ratchet key announced" : "No ratchet"))
        if let found = record.discovery {
            rows.append(ReticulumRow("Offers", "\(found.type) \(found.reachableOn)\(found.port > 0 ? ":\(found.port)" : "")"))
            rows.append(ReticulumRow("Proof of work", "\(found.stampValue) bits\(found.stampValid ? "" : " (below 16, not trusted)")"))
        }
        if let lookup = node.lookups[record.hash] {
            let text = lookup.answerMs.map { "Answered in \($0) ms" } ?? (lookup.timedOut ? "No answer" : "Waiting…")
            rows.append(ReticulumRow("Path lookup", text))
        }
        return rows
    }

    public var interfaceRows: [ReticulumInterfaceRow] {
        _ = revision
        let now = scheduler.now
        var heard: [String: Int] = [:]
        for record in node.destinations.values { heard[record.interfaceId, default: 0] += 1 }
        return node.interfaces.map { config in
            let status = node.status(config.id)
            let onlineFor = status.onlineSince.map { ReticulumFormat.duration(max(0, Int64(now - $0))) } ?? "—"
            return ReticulumInterfaceRow(config: config, status: status, onlineFor: onlineFor, heard: heard[config.id] ?? 0)
        }
    }

    public func interfaceLines(_ row: ReticulumInterfaceRow) -> [String] {
        let status = row.status
        var lines = ["\(row.config.kindLabel) · \(row.config.endpoint)", status.state.label + (status.detail.isEmpty ? "" : ": \(status.detail)")]
        if status.state == .waiting, let next = status.nextAttempt {
            lines.append("Next try in \(ReticulumFormat.duration(max(0, Int64(next - scheduler.now))))")
        }
        if status.connectMs >= 0 { lines.append("Last connect took \(status.connectMs) ms") }
        lines.append("Sessions \(status.sessions) · invalid frames \(status.invalidFrames) · rejected announces \(status.rejected)")
        lines.append("\(row.heard) destinations best reached through it")
        return lines
    }

    public var discoveredRows: [ReticulumDiscoveredRow] {
        _ = revision
        let now = scheduler.now
        return node.destinations.values.compactMap { record -> ReticulumDiscoveredRow? in
            guard let found = record.discovery else { return nil }
            let address = found.connectable ? "\(found.reachableOn):\(found.port)"
                : found.frequency > 0 ? String(format: "%.3f MHz", found.frequency / 1e6) : "Not reachable over IP"
            let location = found.hasLocation ? String(format: "%.3f, %.3f", found.latitude, found.longitude) : "—"
            return ReticulumDiscoveredRow(
                hash: record.hash, found: found, name: found.name.isEmpty ? "Unnamed" : found.name, address: address,
                proof: "\(found.stampValue) bits\(found.stampValid ? "" : " (low)")", location: location,
                heard: ReticulumFormat.age(record.lastHeard, now: now), usable: found.connectable && found.stampValid)
        }.sorted { $0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending }
    }

    public var filteredDestinations: [ReticulumDestination] {
        _ = revision
        let records = node.destinations.values.filter { record in
            (kindFilter == nil || record.group == kindFilter) && Self.matchesList(record, filter: destinationFilter)
        }
        switch sort {
        case .lastHeard: return records.sorted { $0.lastHeard > $1.lastHeard }
        case .hops: return records.sorted { ($0.hops, -$0.lastHeard) < ($1.hops, -$1.lastHeard) }
        case .announces: return records.sorted { $0.announces > $1.announces }
        case .name:
            return records.sorted {
                ($0.displayName.isEmpty ? "—" : $0.displayName).localizedCaseInsensitiveCompare($1.displayName.isEmpty ? "—" : $1.displayName) == .orderedAscending
            }
        }
    }

    static func matchesList(_ record: ReticulumDestination, filter: String) -> Bool {
        let needle = filter.trimmingCharacters(in: .whitespacesAndNewlines)
        if needle.isEmpty { return true }
        return record.displayName.localizedCaseInsensitiveContains(needle) || record.label.localizedCaseInsensitiveContains(needle)
            || record.aspect.localizedCaseInsensitiveContains(needle) || ReticulumWire.hex(record.hash).localizedCaseInsensitiveContains(needle)
    }

    public func destinationCount(shown: Int) -> String {
        let total = node.destinations.count
        return shown == total ? "\(total) destinations" : "\(shown) of \(total)"
    }

    public var lookupResult: String {
        _ = revision
        if !lookupHint.isEmpty { return lookupHint }
        guard let lookup = node.lookups[ReticulumWire.parseHash(lookupText)] else { return "" }
        if let ms = lookup.answerMs {
            return "Path found: \(ReticulumFormat.hops(lookup.hops)) via \(interfaceName(lookup.interfaceId)), answered in \(ms) ms."
        }
        if lookup.timedOut {
            return "No path within \(Int(ReticulumNode.pathTimeoutSeconds)) seconds. The destination may be offline, or no transport node you reach knows it."
        }
        return "Asking the network…"
    }

    public var events: [ReticulumEvent] {
        _ = revision
        return node.events
    }

    public var counterRows: [ReticulumRow] {
        _ = revision
        let counters = node.counters
        return [
            ReticulumRow("Announces", "\(counters.announces)"),
            ReticulumRow("Rejected announces", "\(counters.rejectedAnnounces)"),
            ReticulumRow("Duplicates", "\(counters.duplicates)"),
            ReticulumRow("Data packets", "\(counters.data)"),
            ReticulumRow("Link requests", "\(counters.linkRequests)"),
            ReticulumRow("Proofs", "\(counters.proofs)"),
            ReticulumRow("Unreadable frames", "\(counters.invalid)"),
            ReticulumRow("Path requests heard", "\(counters.pathRequestsSeen)"),
            ReticulumRow("Path requests answered", "\(counters.pathRequestsAnswered)"),
            ReticulumRow("Announces sent", "\(counters.announcesSent)"),
        ]
    }

    public var destinationHashText: String {
        _ = revision
        return ReticulumWire.pretty(node.destinationHash)
    }

    public var identityHashText: String {
        _ = revision
        return ReticulumWire.pretty(node.identity.hash)
    }

    public var lastAnnounceText: String {
        _ = revision
        guard let last = node.lastAnnounce else { return "Not announced since ForkMesh started" }
        return ReticulumFormat.age(last, now: scheduler.now)
    }

    public func isOnline(_ id: String) -> Bool { node.status(id).state == .online }

    public var messagesTile: ReticulumTile {
        let unread = messenger.unreadCount
        let conversations = messenger.conversations().count
        let links = messenger.activeLinks
        return ReticulumTile(id: 4, label: "Messages", value: unread > 0 ? "\(unread) unread" : "\(conversations)",
                             detail: "\(conversations) conversation\(conversations == 1 ? "" : "s") · \(links) link\(links == 1 ? "" : "s") open",
                             tone: unread > 0 ? 2 : 0, spark: [])
    }

    public var unreadCount: Int {
        _ = revision
        return messenger.unreadCount
    }

    public var messagingAddressText: String {
        _ = revision
        return ReticulumWire.pretty(messenger.address)
    }

    public var conversations: [ReticulumConversation] {
        _ = revision
        var list = messenger.conversations()
        if !currentPeer.isEmpty && !list.contains(where: { $0.peer == currentPeer }) {
            var fresh = ReticulumConversation()
            fresh.peer = currentPeer
            fresh.name = messenger.peerName(currentPeer)
            list.insert(fresh, at: 0)
        }
        return list
    }

    public func conversationPreview(_ conversation: ReticulumConversation) -> String {
        if conversation.count == 0 { return "New conversation" }
        let flat = conversation.last.content.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
        let preview = String(flat.prefix(80))
        return conversation.last.outgoing ? "You: " + preview : preview
    }

    public var chat: [ReticulumChatMessage] {
        _ = revision
        guard !currentPeer.isEmpty else { return [] }
        return Array(messenger.messages(with: currentPeer).suffix(300))
    }

    public var chatTitle: String {
        _ = revision
        return currentPeer.isEmpty ? "Messages" : messenger.peerName(currentPeer)
    }

    public var chatSubtitle: String {
        _ = revision
        guard !currentPeer.isEmpty else { return "Your address: \(ReticulumWire.hex(messenger.address))" }
        var subtitle = ReticulumWire.hex(currentPeer)
        if let record = node.destinations[currentPeer] {
            subtitle += " · \(ReticulumFormat.hops(record.hops)) via \(interfaceName(record.interfaceId)) · heard \(ReticulumFormat.age(record.lastHeard, now: scheduler.now))"
        } else {
            subtitle += " · not heard yet"
        }
        let cost = messenger.peerStampCost(currentPeer)
        if cost > 0 { subtitle += " · asks for \(cost) bits of proof of work" }
        return subtitle
    }

    public var composerHint: String {
        _ = revision
        guard !currentPeer.isEmpty else { return "" }
        let size = composerText.utf8.count
        if size == 0 { return node.onlineCount > 0 ? "End-to-end encrypted with the recipient's key" : "Connect an interface to send" }
        let total = size + 16
        let route = total <= ReticulumLxmf.opportunisticMaxContent ? "fits in a single packet"
            : total <= ReticulumLxmf.linkPacketMaxContent ? "goes over an encrypted link" : "goes over a link as a resource"
        return "\(size) bytes · \(route)"
    }

    public var messagePeers: [ReticulumDestination] {
        _ = revision
        return node.destinations.values.filter { $0.aspect == ReticulumLxmf.deliveryAspect }.sorted { $0.lastHeard > $1.lastHeard }
    }

    public func openConversation(_ peer: [UInt8]) {
        guard peer.count == Reticulum.hashLength else { return }
        currentPeer = peer
        tab = .messages
        messenger.markRead(peer)
        if node.identityFor(peer) == nil && node.onlineCount > 0 && node.lookups[peer] == nil { node.requestPath(peer) }
        revision += 1
    }

    public func closeConversation() {
        currentPeer = []
        revision += 1
    }

    public func chatShown() {
        if chatVisible && !currentPeer.isEmpty { messenger.markRead(currentPeer) }
    }

    @discardableResult
    public func sendMessage() -> String {
        let text = composerText
        guard !currentPeer.isEmpty, !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else { return "" }
        if text.utf8.count > ReticulumMessenger.maxContentBytes {
            show("Messages can be up to \(ReticulumMessenger.maxContentBytes / 1024) KB.", error: true)
            return ""
        }
        let id = messenger.send(to: currentPeer, content: text)
        guard !id.isEmpty else {
            show("That message could not be queued.", error: true)
            return ""
        }
        composerText = ""
        revision += 1
        return id
    }

    public func retryMessage(_ id: String) {
        messenger.retry(id)
        revision += 1
    }

    public func deleteMessage(_ id: String) {
        messenger.forget(id)
        revision += 1
    }

    public func deleteConversation() {
        guard !currentPeer.isEmpty else { return }
        messenger.clearConversation(currentPeer)
        currentPeer = []
        revision += 1
    }

    public static func bubbleMeta(_ message: ReticulumChatMessage) -> String {
        var meta = [ReticulumFormat.shortDate(message.timestamp)]
        if message.outgoing {
            switch message.state {
            case .delivered: meta.append("✓✓ Delivered")
            case .sent: meta.append("✓ Sent")
            case .sending where message.progress > 0: meta.append("Sending \(message.progress)%")
            default: meta.append(message.state.label)
            }
        } else {
            meta.append(message.verified ? "Signature checked" : "Signature not checked")
        }
        if !message.method.isEmpty {
            meta.append(message.method == "opportunistic" ? "single packet" : message.method == "link" ? "over a link" : "as a resource")
        }
        return meta.joined(separator: " · ")
    }
}

enum CryptoHex {
    static func decode(_ text: String) -> [UInt8]? {
        let clean = text.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard clean.count % 2 == 0, clean.allSatisfy({ $0.isASCII && $0.isHexDigit }) else { return nil }
        var out: [UInt8] = []
        out.reserveCapacity(clean.count / 2)
        var index = clean.startIndex
        while index < clean.endIndex {
            let next = clean.index(index, offsetBy: 2)
            guard let byte = UInt8(clean[index..<next], radix: 16) else { return nil }
            out.append(byte)
            index = next
        }
        return out
    }
}
