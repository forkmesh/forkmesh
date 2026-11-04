import FMBase
import Foundation

public enum ReticulumInterfaceKind: String, Sendable, Equatable, CaseIterable {
    case tcp
    case shared
    case udp
}

public enum ReticulumLinkState: Sendable, Equatable {
    case disabled
    case connecting
    case online
    case waiting
    case blocked
    case failed

    public var label: String {
        switch self {
        case .disabled: return "Off"
        case .connecting: return "Connecting"
        case .online: return "Online"
        case .waiting: return "Retrying"
        case .blocked: return "Blocked"
        case .failed: return "Failed"
        }
    }
}

public struct ReticulumInterfaceConfig: Equatable, Sendable, Identifiable {
    public var id = ""
    public var name = ""
    public var kind: ReticulumInterfaceKind = .tcp
    public var enabled = true
    public var host = ""
    public var port = 4242
    public var kiss = false
    public var instanceName = "default"
    public var sharedOverTcp = false
    public var listenAddress = "0.0.0.0"
    public var listenPort = 4242
    public var forwardAddress = "255.255.255.255"
    public var forwardPort = 4242
    public var networkName = ""
    public var passphrase = ""
    public var accessCodeSize = 16

    public init(id: String = "", name: String = "", host: String = "", port: Int = 4242, kiss: Bool = false) {
        self.id = id
        self.name = name
        self.host = host
        self.port = port
        self.kiss = kiss
    }

    public static func newId() -> String { UUID().uuidString.lowercased() }

    public var usesAccessCode: Bool { !networkName.isEmpty || !passphrase.isEmpty }

    public var json: JSONObject {
        var object: JSONObject = ["id": .string(id), "name": .string(name), "kind": .string(kind.rawValue), "enabled": .bool(enabled)]
        if usesAccessCode {
            object["networkName"] = .string(networkName)
            object["passphrase"] = .string(passphrase)
            object["accessCodeSize"] = .int(Int64(accessCodeSize))
        }
        switch kind {
        case .tcp:
            object["host"] = .string(host)
            object["port"] = .int(Int64(port))
            object["kiss"] = .bool(kiss)
        case .shared:
            object["instanceName"] = .string(instanceName)
            object["sharedOverTcp"] = .bool(sharedOverTcp)
            object["port"] = .int(Int64(port))
        case .udp:
            object["listenAddress"] = .string(listenAddress)
            object["listenPort"] = .int(Int64(listenPort))
            object["forwardAddress"] = .string(forwardAddress)
            object["forwardPort"] = .int(Int64(forwardPort))
        }
        return object
    }

    public init?(json object: JSONObject) {
        func text(_ key: String, _ fallback: String = "") -> String {
            if case .string(let value)? = object[key] { return value }
            return fallback
        }
        func number(_ key: String, _ fallback: Int) -> Int {
            switch object[key] {
            case .int(let value)?: return Int(truncatingIfNeeded: value)
            case .double(let value)? where value.isFinite && abs(value) < 1e9: return Int(value)
            default: return fallback
            }
        }
        func flag(_ key: String, _ fallback: Bool) -> Bool {
            if case .bool(let value)? = object[key] { return value }
            return fallback
        }
        guard let kind = ReticulumInterfaceKind(rawValue: text("kind")) else { return nil }
        self.kind = kind
        id = text("id")
        name = text("name").trimmingCharacters(in: .whitespacesAndNewlines)
        enabled = flag("enabled", true)
        host = text("host").trimmingCharacters(in: .whitespacesAndNewlines)
        port = number("port", kind == .shared ? 37428 : 4242)
        kiss = flag("kiss", false)
        instanceName = text("instanceName", "default").trimmingCharacters(in: .whitespacesAndNewlines)
        sharedOverTcp = flag("sharedOverTcp", false)
        listenAddress = text("listenAddress", "0.0.0.0").trimmingCharacters(in: .whitespacesAndNewlines)
        listenPort = number("listenPort", 4242)
        forwardAddress = text("forwardAddress", "255.255.255.255").trimmingCharacters(in: .whitespacesAndNewlines)
        forwardPort = number("forwardPort", 4242)
        networkName = text("networkName")
        passphrase = text("passphrase")
        accessCodeSize = number("accessCodeSize", 16)
        if id.isEmpty { id = Self.newId() }
        if validationError != nil { return nil }
    }

    public static func list(fromJSON text: String) -> [ReticulumInterfaceConfig] {
        guard let value = try? JSON.parse(text) else { return [] }
        return value.arrayOrEmpty.compactMap { $0.objectValue.flatMap(ReticulumInterfaceConfig.init(json:)) }
    }

    public static func json(_ configs: [ReticulumInterfaceConfig]) -> String {
        JSON.encode(.array(configs.map { .object($0.json) }))
    }

    public var endpoint: String {
        switch kind {
        case .tcp: return "\(host.contains(":") ? "[\(host)]" : host):\(port)"
        case .shared: return sharedOverTcp ? "127.0.0.1:\(port)" : "rns/\(instanceName)"
        case .udp: return "\(listenAddress):\(listenPort) → \(forwardAddress):\(forwardPort)"
        }
    }

    public var kindLabel: String {
        switch kind {
        case .tcp: return kiss ? "TCP (KISS)" : "TCP"
        case .shared: return "Shared instance"
        case .udp: return "UDP"
        }
    }

    public var validationError: String? {
        if name.isEmpty || name.utf16.count > 80 { return "Give the interface a name of up to 80 characters." }
        func badPort(_ value: Int) -> Bool { value < 1 || value > 65535 }
        if usesAccessCode && (accessCodeSize < 1 || accessCodeSize > 64) { return "The access code size must be between 1 and 64 bytes." }
        if networkName.utf16.count > 256 || passphrase.utf16.count > 256 { return "The network name and passphrase may be up to 256 characters." }
        switch kind {
        case .tcp:
            if !ReticulumAddress.validHost(host) { return "Enter a host name or IP address." }
            if badPort(port) { return "The port must be between 1 and 65535." }
        case .shared:
            if !sharedOverTcp && !ReticulumAddress.validInstance(instanceName) {
                return "The instance name may use letters, digits, dot, dash and underscore."
            }
            if sharedOverTcp && badPort(port) { return "The port must be between 1 and 65535." }
        case .udp:
            if !ReticulumAddress.isLiteral(listenAddress) || !ReticulumAddress.isLiteral(forwardAddress) {
                return "UDP needs a listen address and a forward address."
            }
            if badPort(listenPort) || badPort(forwardPort) { return "The ports must be between 1 and 65535." }
        }
        return nil
    }

    public func sameTarget(_ other: ReticulumInterfaceConfig) -> Bool {
        kind == other.kind && endpoint.lowercased() == other.endpoint.lowercased()
    }

    public static var publicEntryPoints: [ReticulumInterfaceConfig] {
        [
            ReticulumInterfaceConfig(id: "public-rmap-1", name: "RMAP Backbone 1", host: "rmap.world", port: 4242),
            ReticulumInterfaceConfig(id: "public-rmap-2", name: "RMAP Backbone 2", host: "rmap.world", port: 4343),
        ]
    }

    public static var sharedInstance: ReticulumInterfaceConfig {
        var shared = ReticulumInterfaceConfig(id: "shared-instance", name: "Reticulum on this computer", port: 37428)
        shared.kind = .shared
        return shared
    }
}

public enum ReticulumAddress {
    public static func ipv4(_ text: String) -> Bool {
        let parts = text.split(separator: ".", omittingEmptySubsequences: false)
        guard parts.count == 4 else { return false }
        return parts.allSatisfy { part in
            guard !part.isEmpty, part.count <= 3, part.allSatisfy({ $0.isASCII && $0.isNumber }), let value = Int(part) else { return false }
            return value <= 255
        }
    }

    public static func ipv6(_ input: String) -> Bool {
        var text = input
        if let percent = text.firstIndex(of: "%") {
            guard text.index(after: percent) < text.endIndex else { return false }
            text = String(text[..<percent])
        }
        guard text.contains(":"), text.count <= 45 else { return false }
        let halves = text.components(separatedBy: "::")
        guard halves.count <= 2 else { return false }
        var groups = 0
        for (index, half) in halves.enumerated() where !half.isEmpty {
            let parts = half.split(separator: ":", omittingEmptySubsequences: false)
            for (position, part) in parts.enumerated() {
                if position == parts.count - 1 && index == halves.count - 1 && part.contains(".") {
                    guard ipv4(String(part)) else { return false }
                    groups += 2
                    continue
                }
                guard (1...4).contains(part.count), part.allSatisfy({ $0.isASCII && $0.isHexDigit }) else { return false }
                groups += 1
            }
        }
        return halves.count == 2 ? groups < 8 : groups == 8
    }

    public static func isLiteral(_ text: String) -> Bool { ipv4(text) || ipv6(text) }

    public static func validHost(_ host: String) -> Bool {
        if isLiteral(host) { return true }
        guard !host.isEmpty, host.utf8.count <= 253 else { return false }
        var labels = host.split(separator: ".", omittingEmptySubsequences: false)
        if labels.count > 1, labels.last?.isEmpty == true { labels.removeLast() }
        return labels.allSatisfy { label in
            guard (1...63).contains(label.count), label.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "-") }) else { return false }
            return label.first != "-" && label.last != "-"
        }
    }

    public static func validInstance(_ name: String) -> Bool {
        (1...64).contains(name.count) && name.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber || "._-".contains($0)) }
    }
}

public struct ReticulumTrafficSample: Equatable, Sendable {
    public var rx: Int64 = 0
    public var tx: Int64 = 0
    public var announces = 0

    public init(rx: Int64 = 0, tx: Int64 = 0, announces: Int = 0) {
        self.rx = rx
        self.tx = tx
        self.announces = announces
    }
}

public struct ReticulumInterfaceStatus: Equatable, Sendable {
    public var state: ReticulumLinkState = .disabled
    public var detail = ""
    public var rxBytes: Int64 = 0
    public var txBytes: Int64 = 0
    public var rxPackets: Int64 = 0
    public var txPackets: Int64 = 0
    public var announces: Int64 = 0
    public var invalidFrames: Int64 = 0
    public var rejected: Int64 = 0
    public var connectMs: Int64 = -1
    public var attempts = 0
    public var sessions = 0
    public var onlineSince: Double?
    public var nextAttempt: Double?
    public var history: [ReticulumTrafficSample] = []

    public init() {}
}

public struct ReticulumDestination: Equatable, Sendable, Identifiable {
    public var hash: [UInt8] = []
    public var identityHash: [UInt8] = []
    public var publicKey: [UInt8] = []
    public var nameHash: [UInt8] = []
    public var transportId: [UInt8] = []
    public var appData: [UInt8] = []
    public var aspect = ""
    public var label = ""
    public var kind = ""
    public var displayName = ""
    public var interfaceId = ""
    public var hops = 0
    public var announces = 0
    public var ratchet = false
    public var ratchetKey: [UInt8] = []
    public var viaSharedInstance = false
    public var emittedAt: Int64 = 0
    public var firstHeard: Double = 0
    public var lastHeard: Double = 0
    public var discovery: ReticulumDiscoveredInterface?

    public init() {}

    public var id: [UInt8] { hash }
    public var group: ReticulumMapGroup { ReticulumMapGroup(kind: kind) }
}

public struct ReticulumEvent: Equatable, Sendable, Identifiable {
    public let id: Int
    public let at: Double
    public let kind: String
    public let text: String
    public let destination: [UInt8]
}

public struct ReticulumPathLookup: Equatable, Sendable {
    public var destination: [UInt8] = []
    public var tag: [UInt8] = []
    public var started: Double = 0
    public var answered: Double?
    public var hops = -1
    public var interfaceId = ""
    public var timedOut = false

    public var answerMs: Int64? { answered.map { Int64((($0 - started) * 1000).rounded()) } }
}

public struct ReticulumCounters: Equatable, Sendable {
    public var data: Int64 = 0
    public var announces: Int64 = 0
    public var linkRequests: Int64 = 0
    public var proofs: Int64 = 0
    public var duplicates: Int64 = 0
    public var invalid: Int64 = 0
    public var rejectedAnnounces: Int64 = 0
    public var pathRequestsSeen: Int64 = 0
    public var pathRequestsAnswered: Int64 = 0
    public var announcesSent: Int64 = 0

    public init() {}
}

public enum ReticulumLinkEvent: Equatable, Sendable {
    case connected
    case received([UInt8])
    case closed
    case failed(String)
}

public protocol ReticulumLink: AnyObject {
    func open(_ handler: @escaping (ReticulumLinkEvent) -> Void)
    func write(_ bytes: [UInt8]) -> Int
    func close()
}

public protocol ReticulumConnector: AnyObject {
    func tcp(host: String, port: Int) -> ReticulumLink?
    func local(instance: String) -> ReticulumLink?
    func unavailableReason(_ kind: ReticulumInterfaceKind) -> String?
}

extension ReticulumConnector {
    public func local(instance: String) -> ReticulumLink? { nil }
    public func unavailableReason(_ kind: ReticulumInterfaceKind) -> String? { nil }
}

public protocol ReticulumTimer: AnyObject {
    func cancel()
}

public protocol ReticulumScheduler: AnyObject {
    var now: Double { get }
    func after(_ seconds: Double, _ action: @escaping () -> Void) -> ReticulumTimer
    func background(_ work: @escaping () -> Void)
}

extension ReticulumScheduler {
    public func background(_ work: @escaping () -> Void) { work() }
}

public final class ReticulumDispatchScheduler: ReticulumScheduler {
    private final class Item: ReticulumTimer {
        let work: DispatchWorkItem
        init(_ work: DispatchWorkItem) { self.work = work }
        func cancel() { work.cancel() }
    }

    private let queue: DispatchQueue

    public init(queue: DispatchQueue = .main) { self.queue = queue }

    public var now: Double { Date().timeIntervalSince1970 }

    public func after(_ seconds: Double, _ action: @escaping () -> Void) -> ReticulumTimer {
        let work = DispatchWorkItem(block: action)
        queue.asyncAfter(deadline: .now() + max(0, seconds), execute: work)
        return Item(work)
    }

    public func background(_ work: @escaping () -> Void) {
        DispatchQueue.global(qos: .utility).async(execute: DispatchWorkItem(block: work))
    }
}

public final class ReticulumInterfaceRuntime {
    static let retryFirstMs = 5000
    static let retryMaxMs = 120_000
    static let connectTimeout: Double = 10
    static let keepalive: Double = 60

    public private(set) var config: ReticulumInterfaceConfig
    public private(set) var status = ReticulumInterfaceStatus()
    unowned let node: ReticulumNode
    private var decoder: ReticulumFrameDecoder
    private var accessCode: ReticulumAccessCode
    private var link: ReticulumLink?
    private var linkGeneration = 0
    private var linkConnected = false
    private var retryTimer: ReticulumTimer?
    private var connectTimer: ReticulumTimer?
    private var keepaliveTimer: ReticulumTimer?
    private var connectStarted: Double = 0
    private(set) var active = false
    private var authorized = false
    var current = ReticulumTrafficSample()

    init(node: ReticulumNode, config: ReticulumInterfaceConfig) {
        self.node = node
        self.config = config
        decoder = ReticulumFrameDecoder(framing: config.kiss ? .kiss : .hdlc)
        accessCode = ReticulumAccessCode(networkName: config.networkName, passphrase: config.passphrase, size: config.accessCodeSize)
        status.history.reserveCapacity(ReticulumNode.historySeconds)
    }

    public var online: Bool { status.state == .online }

    func setConfig(_ next: ReticulumInterfaceConfig) {
        let changed = !config.sameTarget(next) || config.kiss != next.kiss || config.networkName != next.networkName
            || config.passphrase != next.passphrase || config.accessCodeSize != next.accessCodeSize
        let wasActive = active
        config = next
        if changed {
            authorized = false
            decoder.setFraming(next.kiss ? .kiss : .hdlc)
            accessCode = ReticulumAccessCode(networkName: next.networkName, passphrase: next.passphrase, size: next.accessCodeSize)
        }
        if !next.enabled {
            stop()
            return
        }
        if changed && wasActive {
            stop()
            start()
        } else if !wasActive && node.running {
            start()
        }
    }

    func start() {
        guard config.enabled else {
            setState(.disabled)
            return
        }
        active = true
        retryTimer?.cancel()
        retryTimer = nil
        dropLink()
        decoder.reset()
        status.attempts += 1
        connectStarted = node.scheduler.now
        if let reason = node.connector.unavailableReason(config.kind) {
            active = false
            setState(.failed, reason)
            return
        }
        switch config.kind {
        case .tcp:
            connectTcp()
        case .shared:
            if config.sharedOverTcp { connectTcp() } else { connectLocal() }
        case .udp:
            active = false
            setState(.failed, "UDP is not available on this device")
        }
    }

    func stop() {
        active = false
        retryTimer?.cancel()
        retryTimer = nil
        keepaliveTimer?.cancel()
        keepaliveTimer = nil
        dropLink()
        status.nextAttempt = nil
        setState(.disabled, config.enabled ? "Stopped" : "")
    }

    func reconnectNow() {
        authorized = false
        status.attempts = 0
        guard config.enabled else { return }
        stop()
        start()
    }

    private func authorize(_ method: String, _ url: String) -> Bool {
        if authorized { return true }
        if let authorizer = node.authorizer, !authorizer(method, url) { return false }
        authorized = true
        return true
    }

    private func connectTcp() {
        let shared = config.kind == .shared
        let host = shared ? "127.0.0.1" : config.host
        let port = config.port
        let literal = host.contains(":") ? "[\(host)]" : host
        guard authorize("Reticulum", "tcp://\(literal):\(port)") else {
            setState(.blocked, "The ForkMesh firewall did not allow \(config.endpoint)")
            active = false
            return
        }
        guard let created = node.connector.tcp(host: host, port: port) else {
            active = false
            setState(.failed, "TCP is not available on this device")
            return
        }
        attach(created, detail: "Connecting to \(config.endpoint)", timeout: true)
    }

    private func connectLocal() {
        guard let created = node.connector.local(instance: config.instanceName) else {
            active = false
            setState(.failed, "No Reticulum daemon can run on this device")
            return
        }
        attach(created, detail: "Looking for the Reticulum daemon", timeout: false)
    }

    private func attach(_ created: ReticulumLink, detail: String, timeout: Bool) {
        linkGeneration += 1
        let generation = linkGeneration
        link = created
        linkConnected = false
        setState(.connecting, detail)
        if timeout {
            connectTimer = node.scheduler.after(Self.connectTimeout) { [weak self] in
                guard let self, self.linkGeneration == generation, !self.linkConnected else { return }
                self.lostConnection("No answer from \(self.config.endpoint) after \(Int(Self.connectTimeout)) s")
            }
        }
        created.open { [weak self] event in
            guard let self, self.linkGeneration == generation, self.link != nil else { return }
            self.linkEvent(event)
        }
    }

    private func linkEvent(_ event: ReticulumLinkEvent) {
        switch event {
        case .connected:
            connectTimer?.cancel()
            connectTimer = nil
            linkConnected = true
            wentOnline()
        case .received(let bytes):
            receive(bytes)
        case .closed:
            lostConnection(config.kind == .shared && !config.sharedOverTcp
                           ? "The Reticulum daemon closed the connection"
                           : "\(config.endpoint) closed the connection")
        case .failed(let reason):
            if !linkConnected { lostConnection(reason) }
        }
    }

    private func wentOnline() {
        let now = node.scheduler.now
        status.connectMs = Int64(((now - connectStarted) * 1000).rounded())
        status.onlineSince = now
        status.attempts = 0
        status.nextAttempt = nil
        status.sessions += 1
        if config.kind == .shared { scheduleKeepalive() }
        let detail = config.kind == .udp
            ? "Listening on \(config.listenAddress):\(config.listenPort)"
            : "Connected in \(Self.elapsedText(status.connectMs))"
        setState(.online, detail)
        node.addEvent("interface", "\(config.name) is online")
    }

    private func scheduleKeepalive() {
        keepaliveTimer?.cancel()
        keepaliveTimer = node.scheduler.after(Self.keepalive) { [weak self] in
            guard let self, self.online else { return }
            _ = self.link?.write(ReticulumWire.hdlcKeepalive())
            self.scheduleKeepalive()
        }
    }

    static func elapsedText(_ ms: Int64) -> String {
        if ms < 1000 { return "\(ms) ms" }
        return String(format: "%.1f s", Double(ms) / 1000)
    }

    private func lostConnection(_ reason: String) {
        guard active else { return }
        let wasOnline = status.state == .online
        keepaliveTimer?.cancel()
        keepaliveTimer = nil
        if wasOnline { node.addEvent("interface", "\(config.name) went offline: \(reason)") }
        scheduleRetry(reason)
    }

    private func scheduleRetry(_ reason: String) {
        dropLink()
        let exponent = min(5, max(0, status.attempts - 1))
        let delay = min(Self.retryMaxMs, Self.retryFirstMs << exponent)
        status.nextAttempt = node.scheduler.now + Double(delay) / 1000
        setState(.waiting, reason)
        retryTimer?.cancel()
        retryTimer = node.scheduler.after(Double(delay) / 1000) { [weak self] in
            guard let self else { return }
            self.retryTimer = nil
            self.start()
        }
    }

    private func dropLink() {
        connectTimer?.cancel()
        connectTimer = nil
        linkGeneration += 1
        let old = link
        link = nil
        linkConnected = false
        old?.close()
    }

    private func setState(_ state: ReticulumLinkState, _ detail: String = "") {
        if state != .online { status.onlineSince = nil }
        status.state = state
        status.detail = detail
        node.notifyChanged()
    }

    private func receive(_ bytes: [UInt8]) {
        let count = Int64(bytes.count)
        status.rxBytes += count
        current.rx += count
        node.current.rx += count
        let before = decoder.invalidFrames
        let frames = decoder.feed(bytes)
        status.invalidFrames += decoder.invalidFrames - before
        for frame in frames {
            status.rxPackets += 1
            let shared = config.kind == .shared
            if accessCode.enabled {
                guard let inner = accessCode.remove(frame) else {
                    status.invalidFrames += 1
                    continue
                }
                node.ingest(interfaceId: config.id, raw: inner, fromSharedInstance: shared)
                continue
            }
            if let first = frame.first, first & 0x80 != 0 {
                status.invalidFrames += 1
                continue
            }
            node.ingest(interfaceId: config.id, raw: frame, fromSharedInstance: shared)
        }
    }

    func countInvalid() { status.invalidFrames += 1 }
    func countAnnounce() {
        status.announces += 1
        current.announces += 1
    }
    func countRejected() { status.rejected += 1 }

    @discardableResult
    func send(_ packet: [UInt8]) -> Bool {
        guard online, !packet.isEmpty, let link else { return false }
        let raw = accessCode.enabled ? accessCode.apply(packet) : packet
        let framed = config.kiss ? ReticulumWire.kissFrame(raw) : ReticulumWire.hdlcFrame(raw)
        let written = link.write(framed)
        guard written > 0 else { return false }
        status.txPackets += 1
        status.txBytes += Int64(written)
        current.tx += Int64(written)
        node.current.tx += Int64(written)
        return true
    }

    func rotateHistory() {
        status.history.append(current)
        if status.history.count > ReticulumNode.historySeconds {
            status.history.removeFirst(status.history.count - ReticulumNode.historySeconds)
        }
        current = ReticulumTrafficSample()
    }
}

public final class ReticulumNode {
    public typealias Authorizer = (String, String) -> Bool

    public static let historySeconds = 180
    public static let maxDestinations = 20_000
    public static let maxEvents = 400
    public static let maxSeenPackets = 60_000
    public static let pathTimeoutSeconds: Double = 20
    public static let changeDelay: Double = 0.15

    let scheduler: ReticulumScheduler
    let connector: ReticulumConnector
    public var authorizer: Authorizer?
    public var onChange: (() -> Void)?
    public var onAnnounce: (([UInt8]) -> Void)?
    public var packetHandler: ((String, ReticulumPacket, [UInt8], Int) -> Void)?
    public private(set) var runtimes: [ReticulumInterfaceRuntime] = []
    public private(set) var running = false
    public private(set) var startedAt: Double?
    public private(set) var identity = ReticulumIdentity()
    public let aspect = Reticulum.nodeAspect
    public private(set) var announceName = ""
    public var answerPathRequests = true
    public private(set) var lastAnnounce: Double?
    private var lastPathAnswers: [[UInt8]: Double] = [:]
    private var local: [String: () -> [UInt8]] = [:]
    public private(set) var destinations: [[UInt8]: ReticulumDestination] = [:]
    public private(set) var lookups: [[UInt8]: ReticulumPathLookup] = [:]
    public private(set) var events: [ReticulumEvent] = []
    private var eventSerial = 0
    private var seen = Set<[UInt8]>()
    private var seenOrder: [[UInt8]] = []
    private var seenHead = 0
    public private(set) var counters = ReticulumCounters()
    public private(set) var history: [ReticulumTrafficSample] = []
    var current = ReticulumTrafficSample()
    private var changeTimer: ReticulumTimer?
    private var tickTimer: ReticulumTimer?

    public init(scheduler: ReticulumScheduler, connector: ReticulumConnector) {
        self.scheduler = scheduler
        self.connector = connector
        history.reserveCapacity(Self.historySeconds)
        addLocalDestination(aspect) { [weak self] in Array((self?.announceName ?? "").utf8) }
    }

    deinit {
        changeTimer?.cancel()
        tickTimer?.cancel()
    }

    public func addLocalDestination(_ aspect: String, appData: @escaping () -> [UInt8]) { local[aspect] = appData }

    public func localDestinationHash(_ aspect: String) -> [UInt8] {
        guard identity.isValid, local[aspect] != nil else { return [] }
        return ReticulumWire.destinationHash(name: aspect, identityHash: identity.hash)
    }

    public func localAspect(_ hash: [UInt8]) -> String? {
        guard identity.isValid, hash.count == Reticulum.hashLength else { return nil }
        return local.keys.first { ReticulumWire.destinationHash(name: $0, identityHash: identity.hash) == hash }
    }

    public func isLocalDestination(_ hash: [UInt8]) -> Bool { localAspect(hash) != nil }

    public func identityFor(_ destination: [UInt8]) -> ReticulumIdentity? {
        destinations[destination].flatMap { ReticulumIdentity(publicKey: $0.publicKey) }
    }

    public func interfaceOnline(_ id: String) -> Bool { runtime(id)?.online ?? false }

    @discardableResult
    public func sendRaw(_ raw: [UInt8], onlyInterface: String? = nil) -> [UInt8] {
        guard !raw.isEmpty else { return [] }
        _ = rememberPacket(ReticulumWire.packetHash(raw))
        transmit(raw, onlyInterface: onlyInterface)
        notifyChanged()
        return raw
    }

    @discardableResult
    public func sendPacket(_ input: ReticulumPacket, onlyInterface: String? = nil) -> [UInt8] {
        var packet = input
        packet.headerType = 0
        packet.transportType = 0
        packet.transportId = []
        let raw = ReticulumWire.packPacket(packet)
        _ = rememberPacket(ReticulumWire.packetHash(raw))
        let found = destinations[packet.destinationHash]
        if let found, packet.packetType != .announce, packet.destinationType != .plain, packet.destinationType != .group,
           onlyInterface?.isEmpty ?? true, interfaceOnline(found.interfaceId) {
            if found.hops > 1 || (found.hops == 1 && found.viaSharedInstance) {
                var routed = packet
                routed.headerType = 1
                routed.transportType = 1
                routed.transportId = found.transportId.count == Reticulum.hashLength ? found.transportId : found.hash
                transmit(ReticulumWire.packPacket(routed), onlyInterface: found.interfaceId)
            } else {
                transmit(raw, onlyInterface: found.interfaceId)
            }
        } else {
            transmit(raw, onlyInterface: onlyInterface)
        }
        notifyChanged()
        return raw
    }

    public var interfaces: [ReticulumInterfaceConfig] { runtimes.map(\.config) }

    public func setInterfaces(_ configs: [ReticulumInterfaceConfig]) {
        let keep = Set(configs.map(\.id))
        for runtime in runtimes where !keep.contains(runtime.config.id) { removeInterface(runtime.config.id) }
        for config in configs { upsertInterface(config) }
        runtimes = configs.compactMap { config in runtimes.first { $0.config.id == config.id } }
        notifyChanged()
    }

    public func upsertInterface(_ config: ReticulumInterfaceConfig) {
        if let existing = runtime(config.id) {
            existing.setConfig(config)
        } else {
            let created = ReticulumInterfaceRuntime(node: self, config: config)
            runtimes.append(created)
            if running && config.enabled { created.start() }
        }
        updateTicking()
        notifyChanged()
    }

    public func removeInterface(_ id: String) {
        guard let found = runtime(id) else { return }
        runtimes.removeAll { $0 === found }
        found.stop()
        updateTicking()
        notifyChanged()
    }

    public func setInterfaceEnabled(_ id: String, _ enabled: Bool) {
        guard let found = runtime(id) else { return }
        var config = found.config
        config.enabled = enabled
        found.setConfig(config)
        if enabled && running { found.reconnectNow() }
        updateTicking()
        notifyChanged()
    }

    public func reconnectInterface(_ id: String) {
        runtime(id)?.reconnectNow()
        updateTicking()
    }

    public func status(_ id: String) -> ReticulumInterfaceStatus { runtime(id)?.status ?? ReticulumInterfaceStatus() }

    public var onlineCount: Int { runtimes.filter(\.online).count }

    public func start() {
        guard !running else { return }
        running = true
        startedAt = scheduler.now
        for runtime in runtimes where runtime.config.enabled { runtime.start() }
        updateTicking()
        notifyChanged()
    }

    public func stop() {
        running = false
        for runtime in runtimes { runtime.stop() }
        updateTicking()
        notifyChanged()
    }

    public func setIdentity(_ next: ReticulumIdentity) {
        identity = next
        notifyChanged()
    }

    public var destinationHash: [UInt8] { localDestinationHash(aspect) }

    public func setAnnounceName(_ name: String) {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        announceName = String(String.UnicodeScalarView(trimmed.unicodeScalars.prefix(64)))
    }

    @discardableResult
    public func announceDestination(_ destination: [UInt8], pathResponse: Bool = false, onlyInterface: String? = nil) -> Bool {
        guard let name = localAspect(destination), identity.hasPrivateKey, onlineCount > 0 else { return false }
        let raw = ReticulumWire.buildAnnounce(identity: identity, name: name, appData: local[name]?() ?? [],
                                              pathResponse: pathResponse, unixTime: Int64(scheduler.now))
        guard !raw.isEmpty else { return false }
        _ = rememberPacket(ReticulumWire.packetHash(raw))
        transmit(raw, onlyInterface: onlyInterface)
        counters.announcesSent += 1
        if !pathResponse {
            lastAnnounce = scheduler.now
            addEvent("announce-sent", "Announced \(name) as \(ReticulumWire.pretty(destination))", destination)
        }
        notifyChanged()
        return true
    }

    @discardableResult
    public func announce(pathResponse: Bool = false, onlyInterface: String? = nil) -> Bool {
        var sent = false
        for name in local.keys.sorted() {
            sent = announceDestination(localDestinationHash(name), pathResponse: pathResponse, onlyInterface: onlyInterface) || sent
        }
        return sent
    }

    @discardableResult
    public func requestPath(_ destination: [UInt8]) -> [UInt8] {
        guard destination.count == Reticulum.hashLength, onlineCount > 0 else { return [] }
        let tag = ReticulumWire.truncatedHash(ReticulumWire.randomBytes(32))
        let raw = ReticulumWire.buildPathRequest(destination: destination, tag: tag)
        _ = rememberPacket(ReticulumWire.packetHash(raw))
        transmit(raw)
        var lookup = ReticulumPathLookup()
        lookup.destination = destination
        lookup.tag = tag
        lookup.started = scheduler.now
        lookups[destination] = lookup
        addEvent("path-request", "Asked the network for a path to \(ReticulumWire.pretty(destination))", destination)
        updateTicking()
        notifyChanged()
        return tag
    }

    public func forgetDestination(_ hash: [UInt8]) {
        destinations.removeValue(forKey: hash)
        lookups.removeValue(forKey: hash)
        notifyChanged()
    }

    public func clearDestinations() {
        destinations.removeAll()
        lookups.removeAll()
        notifyChanged()
    }

    func runtime(_ id: String) -> ReticulumInterfaceRuntime? { runtimes.first { $0.config.id == id } }

    public func interfaceName(_ id: String) -> String { runtime(id)?.config.name ?? id }

    public func transmit(_ raw: [UInt8], onlyInterface: String? = nil) {
        for runtime in runtimes {
            if let only = onlyInterface, !only.isEmpty, runtime.config.id != only { continue }
            runtime.send(raw)
        }
    }

    func notifyChanged() {
        guard changeTimer == nil else { return }
        changeTimer = scheduler.after(Self.changeDelay) { [weak self] in
            guard let self else { return }
            self.changeTimer = nil
            self.onChange?()
        }
    }

    public func addEvent(_ kind: String, _ text: String, _ destination: [UInt8] = []) {
        eventSerial += 1
        events.insert(ReticulumEvent(id: eventSerial, at: scheduler.now, kind: kind, text: text, destination: destination), at: 0)
        if events.count > Self.maxEvents { events.removeLast(events.count - Self.maxEvents) }
        notifyChanged()
    }

    private func rememberPacket(_ hash: [UInt8]) -> Bool {
        guard !hash.isEmpty, !seen.contains(hash) else { return false }
        seen.insert(hash)
        seenOrder.append(hash)
        while seenOrder.count - seenHead > Self.maxSeenPackets {
            seen.remove(seenOrder[seenHead])
            seenHead += 1
        }
        if seenHead > Self.maxSeenPackets {
            seenOrder.removeFirst(seenHead)
            seenHead = 0
        }
        return true
    }

    public func ingest(interfaceId: String, raw: [UInt8], fromSharedInstance: Bool = false) {
        let source = runtime(interfaceId)
        guard let packet = try? ReticulumWire.parsePacket(raw) else {
            counters.invalid += 1
            source?.countInvalid()
            return
        }
        let hops = fromSharedInstance ? packet.hops : packet.hops + 1
        let repeatable = [ReticulumContext.keepalive, ReticulumContext.resourceRequest, ReticulumContext.resourceProof,
                          ReticulumContext.resource, ReticulumContext.cacheRequest, ReticulumContext.channel].contains(packet.context)
        let fresh = rememberPacket(ReticulumWire.packetHash(raw)) || (repeatable && packet.packetType != .announce)
        if packet.packetType != .announce && packet.headerType == 1 && !fromSharedInstance {
            counters.duplicates += 1
            return
        }
        switch packet.packetType {
        case .announce:
            counters.announces += 1
            current.announces += 1
            source?.countAnnounce()
            if !fresh {
                counters.duplicates += 1
                if var found = destinations[packet.destinationHash], hops < found.hops {
                    found.hops = hops
                    found.interfaceId = interfaceId
                    found.transportId = packet.transportId
                    destinations[packet.destinationHash] = found
                    notifyChanged()
                }
                return
            }
            handleAnnounce(interfaceId: interfaceId, packet: packet, hops: hops)
        case .data:
            counters.data += 1
            if !fresh {
                counters.duplicates += 1
                return
            }
            if packet.destinationType == .plain && packet.destinationHash == ReticulumWire.pathRequestDestination {
                handlePathRequest(interfaceId: interfaceId, packet: packet)
            } else {
                packetHandler?(interfaceId, packet, raw, hops)
            }
        case .linkRequest:
            counters.linkRequests += 1
            if fresh { packetHandler?(interfaceId, packet, raw, hops) }
        case .proof:
            counters.proofs += 1
            if fresh { packetHandler?(interfaceId, packet, raw, hops) }
        }
        notifyChanged()
    }

    private func handleAnnounce(interfaceId: String, packet: ReticulumPacket, hops: Int) {
        let announce: ReticulumAnnounce
        do {
            announce = try ReticulumWire.parseAnnounce(packet)
            try ReticulumWire.validateAnnounce(announce)
        } catch {
            counters.rejectedAnnounces += 1
            runtime(interfaceId)?.countRejected()
            if counters.rejectedAnnounces <= 20 || counters.rejectedAnnounces % 100 == 0 {
                let reason = (error as? ReticulumError)?.message ?? "\(error)"
                addEvent("rejected", "Dropped an announce for \(ReticulumWire.pretty(packet.destinationHash)) from \(interfaceName(interfaceId)): \(reason)", packet.destinationHash)
            }
            return
        }
        if isLocalDestination(announce.destinationHash) { return }
        let now = scheduler.now
        let previous = destinations[announce.destinationHash]
        if let previous, previous.publicKey != announce.publicKey {
            counters.rejectedAnnounces += 1
            addEvent("rejected", "Ignored an announce for \(ReticulumWire.pretty(announce.destinationHash)) with a different key than before", announce.destinationHash)
            return
        }
        if let previous, announce.emittedAt < previous.emittedAt, packet.context != ReticulumContext.pathResponse {
            counters.duplicates += 1
            return
        }
        var record = previous ?? ReticulumDestination()
        record.hash = announce.destinationHash
        record.identityHash = announce.identityHash
        record.publicKey = announce.publicKey
        record.nameHash = announce.nameHash
        record.transportId = packet.transportId
        record.appData = Array(announce.appData.prefix(1024))
        record.ratchet = !announce.ratchet.isEmpty
        if record.ratchet { record.ratchetKey = announce.ratchet }
        if let source = runtime(interfaceId) { record.viaSharedInstance = source.config.kind == .shared }
        record.emittedAt = max(record.emittedAt, announce.emittedAt)
        record.hops = hops
        record.interfaceId = interfaceId
        record.lastHeard = now
        if previous == nil { record.firstHeard = now }
        record.announces += 1
        if let aspect = ReticulumWire.knownAspect(nameHash: announce.nameHash) {
            record.aspect = aspect.name
            record.label = aspect.label
            record.kind = aspect.kind
        } else {
            record.aspect = ""
            record.label = "Unknown application"
            record.kind = "unknown"
        }
        record.displayName = ReticulumWire.displayName(kind: record.kind, appData: announce.appData)
        if record.kind == "discovery" && (previous == nil || previous?.appData != record.appData) {
            record.discovery = try? ReticulumWire.parseDiscovery(announce.appData)
        }
        if let discovery = record.discovery, !discovery.name.isEmpty { record.displayName = discovery.name }
        destinations[record.hash] = record
        onAnnounce?(record.hash)
        if var lookup = lookups[record.hash], lookup.answered == nil {
            lookup.answered = now
            lookup.hops = hops
            lookup.interfaceId = interfaceId
            lookup.timedOut = false
            lookups[record.hash] = lookup
            let ms = Int64(((now - lookup.started) * 1000).rounded())
            let who = record.displayName.isEmpty ? ReticulumWire.pretty(record.hash) : record.displayName
            addEvent("path-found", "Found a path to \(who): \(hops) hop\(hops == 1 ? "" : "s") via \(interfaceName(interfaceId)) in \(ms) ms", record.hash)
        } else if previous == nil {
            let who = record.displayName.isEmpty ? ReticulumWire.pretty(record.hash) : "\(record.displayName) \(ReticulumWire.pretty(record.hash))"
            addEvent("announce", "\(who) · \(record.label) · \(hops) hop\(hops == 1 ? "" : "s") via \(interfaceName(interfaceId))", record.hash)
        }
        if destinations.count > Self.maxDestinations { trimDestinations() }
    }

    private func handlePathRequest(interfaceId: String, packet: ReticulumPacket) {
        guard let request = ReticulumWire.parsePathRequest(packet) else { return }
        counters.pathRequestsSeen += 1
        guard answerPathRequests, !request.tag.isEmpty, let name = localAspect(request.destination), identity.hasPrivateKey else { return }
        let now = scheduler.now
        if let last = lastPathAnswers[request.destination], now - last < 10 { return }
        lastPathAnswers[request.destination] = now
        if announceDestination(request.destination, pathResponse: true, onlyInterface: interfaceId) {
            counters.pathRequestsAnswered += 1
            addEvent("path-answered", "Answered a path request for \(name) on \(interfaceName(interfaceId))", request.destination)
        }
    }

    private func trimDestinations() {
        let excess = destinations.count - Self.maxDestinations + Self.maxDestinations / 10
        guard excess > 0 else { return }
        let oldest = destinations.values.sorted { $0.lastHeard < $1.lastHeard }.prefix(excess)
        for record in oldest { destinations.removeValue(forKey: record.hash) }
    }

    public func tick() {
        history.append(current)
        if history.count > Self.historySeconds { history.removeFirst(history.count - Self.historySeconds) }
        current = ReticulumTrafficSample()
        for runtime in runtimes { runtime.rotateHistory() }
        let now = scheduler.now
        for (key, lookup) in lookups where lookup.answered == nil && !lookup.timedOut && now - lookup.started >= Self.pathTimeoutSeconds {
            var updated = lookup
            updated.timedOut = true
            lookups[key] = updated
            addEvent("path-timeout", "No path to \(ReticulumWire.pretty(lookup.destination)) within \(Int(Self.pathTimeoutSeconds)) s", lookup.destination)
        }
        notifyChanged()
    }

    private func updateTicking() {
        let needed = running && runtimes.contains { $0.config.enabled }
        if needed && tickTimer == nil {
            scheduleTick()
        } else if !needed, let timer = tickTimer {
            timer.cancel()
            tickTimer = nil
        }
    }

    private func scheduleTick() {
        tickTimer = scheduler.after(1) { [weak self] in
            guard let self else { return }
            self.tickTimer = nil
            self.tick()
            self.updateTicking()
        }
    }
}

public enum ReticulumConfigImport {
    static func truthy(_ value: String?) -> Bool {
        guard let text = value?.trimmingCharacters(in: .whitespaces).lowercased() else { return false }
        return text == "yes" || text == "true" || text == "1" || text == "on"
    }

    static func number(_ value: String?) -> Int {
        Int(Int32(value?.trimmingCharacters(in: .whitespaces) ?? "") ?? 0)
    }

    public static func parse(_ text: String) -> (interfaces: [ReticulumInterfaceConfig], skipped: [String]) {
        var out: [ReticulumInterfaceConfig] = []
        var skipped: [String] = []
        var section = ""
        var current = ""
        var values: [String: String] = [:]
        var reticulum: [String: String] = [:]
        func flush() {
            guard !current.isEmpty else { return }
            let type = values["type"] ?? ""
            var config = ReticulumInterfaceConfig(id: ReticulumInterfaceConfig.newId(), name: String(decoding: Array(current.utf16.prefix(80)), as: UTF16.self))
            config.enabled = truthy(values["enabled"]) || truthy(values["interface_enabled"])
            var accepted = false
            if type == "TCPClientInterface" {
                config.kind = .tcp
                config.host = values["target_host"] ?? ""
                config.port = number(values["target_port"])
                config.kiss = truthy(values["kiss_framing"])
                accepted = true
            } else if type == "BackboneInterface", let remote = values["remote"] {
                config.kind = .tcp
                config.host = remote
                config.port = number(values["target_port"] ?? values["port"])
                accepted = true
            } else if type == "UDPInterface" {
                config.kind = .udp
                let both = number(values["port"])
                config.listenAddress = values["listen_ip"] ?? "0.0.0.0"
                config.forwardAddress = values["forward_ip"] ?? "255.255.255.255"
                config.listenPort = number(values["listen_port"])
                config.forwardPort = number(values["forward_port"])
                if both > 0 {
                    config.listenPort = both
                    config.forwardPort = both
                }
                accepted = true
            }
            config.networkName = values["networkname"] ?? values["network_name"] ?? ""
            config.passphrase = values["passphrase"] ?? values["pass_phrase"] ?? ""
            if let size = values["ifac_size"] { config.accessCodeSize = max(1, number(size) / 8) }
            if accepted && config.validationError == nil {
                out.append(config)
            } else {
                skipped.append("\(current) (\(type.isEmpty ? "no type" : type))")
            }
            current = ""
            values = [:]
        }
        for rawLine in text.components(separatedBy: "\n") {
            var line = rawLine
            if let hash = line.firstIndex(of: "#") { line = String(line[..<hash]) }
            line = line.trimmingCharacters(in: .whitespacesAndNewlines)
            if line.isEmpty { continue }
            if line.hasPrefix("[["), line.hasSuffix("]]"), line.count > 4 {
                let inner = String(line.dropFirst(2).dropLast(2)).trimmingCharacters(in: .whitespaces)
                if !inner.isEmpty {
                    flush()
                    if section == "interfaces" { current = inner }
                    continue
                }
            }
            if line.hasPrefix("["), line.hasSuffix("]"), line.count > 2 {
                let inner = String(line.dropFirst().dropLast())
                let name = inner.trimmingCharacters(in: .whitespaces)
                if !name.isEmpty, !inner.contains("["), !inner.contains("]") {
                    flush()
                    section = name.lowercased()
                    continue
                }
            }
            guard let equals = line.firstIndex(of: "=") else { continue }
            let key = line[..<equals].trimmingCharacters(in: .whitespaces)
            guard !key.isEmpty, key.allSatisfy({ $0.isASCII && ($0.isLetter || $0.isNumber || $0 == "_") }) else { continue }
            var value = line[line.index(after: equals)...].trimmingCharacters(in: .whitespaces)
            if value.count >= 2, let first = value.first, first == "\"" || first == "'", value.last == first {
                value = String(value.dropFirst().dropLast())
            }
            if !current.isEmpty {
                values[key.lowercased()] = value
            } else if section == "reticulum" {
                reticulum[key.lowercased()] = value
            }
        }
        flush()
        if reticulum["share_instance"] == nil || truthy(reticulum["share_instance"]) {
            var shared = ReticulumInterfaceConfig.sharedInstance
            shared.id = ReticulumInterfaceConfig.newId()
            if let instance = reticulum["instance_name"] { shared.instanceName = instance }
            if reticulum["shared_instance_type"]?.lowercased() == "tcp" { shared.sharedOverTcp = true }
            if let port = reticulum["shared_instance_port"] { shared.port = number(port) }
            if shared.validationError == nil { out.insert(shared, at: 0) }
        }
        return (out, skipped)
    }
}
