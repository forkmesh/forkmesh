import FMAccount
import FMApi
import FMBase
import FMCrypto
import Foundation
import Observation

private let beaconIntervalDefaultMs = 15_000
private let peerTimeoutDefaultMs = 60_000
private let sweepIntervalMs = 5_000
private let seenCacheLimit = 4096
private let historyLimit = 200
// History is written this long after the last change, and at stop().
private let historySaveDelayMs = 1_000
private let historyFileVersion = 1
private let maxLinks = 64
// Unicast beacons go to the whole local subnet only every few beats: a /24 is 253 datagrams, and a
// peer found that way is then beaconed on its own.
private let addressSweepEveryBeats = 4
// Twice as often while nobody is linked: a desktop whose firewall drops unsolicited datagrams only
// hears this device's sweep while its own sweep is still fresh in that firewall's table.
private let addressSweepEveryBeatsAlone = 2
// A sweep goes out this many hosts at a time, this far apart: a whole /24 at once fills the send
// buffer part way through while hardware addresses are still being resolved.
private let sweepBatch = 16
private let sweepBatchGapMs = 500
private let groupSendRetryBeats = 20
private let dialTimeoutSeconds: TimeInterval = 8
private let keepAliveIdleSeconds = 10
private let keepAliveIntervalSeconds = 5
private let keepAliveCount = 3
private let deliberateDropReasons: Set<String> = ["duplicate link", "replaced", "identity changed"]

/// A desktop on this network that offers a local control channel.
public struct MeshControlHost: Equatable, Hashable, Sendable, Identifiable {
    /// The desktop's Ed25519 identity, which its control channel signs with.
    public let nodeId: String
    public let address: MeshAddress
    public let port: Int
    public let name: String
    public let nodeName: String
    public let ownerUser: String
    public let platform: String

    public init(
        nodeId: String, address: MeshAddress, port: Int, name: String = "", nodeName: String = "",
        ownerUser: String = "", platform: String = ""
    ) {
        self.nodeId = nodeId
        self.address = address
        self.port = port
        self.name = name
        self.nodeName = nodeName
        self.ownerUser = ownerUser
        self.platform = platform
    }

    public var id: String { nodeId }

    /// What to call it on screen: its machine name, else its user's name.
    public var label: String {
        if !nodeName.isEmpty { return nodeName }
        if !name.isEmpty { return name }
        return dartPrefix(nodeId, 8)
    }
}

/// What the room's transports have done since `start`, for the debug bar in the #mesh room.
public struct MeshDiagnostics: Sendable {
    public var multicastSent = 0
    public var broadcastSent = 0
    public var unicastSent = 0  // to peers already heard, and replies
    public var sweepSent = 0
    public var sendErrors = 0
    public var lastSendError = ""
    public var datagramsIn = 0
    public var ownBeaconsIn = 0  // our multicast/broadcast looped back
    public var offNetworkIn = 0
    public var undecryptableIn = 0  // not JSON, another room key, or not a beacon
    public var peerBeaconsIn = 0
    public var lastBeaconFrom = ""
    public var lastBeaconMs: Int64 = 0
    public var dials = 0
    public var dialsOk = 0
    public var dialsFailed = 0
    public var lastDialError = ""
    public var inboundAccepted = 0
    public var inboundRefused = 0
    public var framesIn = 0
    public var framesBad = 0
    public var chatIn = 0
    public var chatOut = 0
    public var lastDropReason = ""
    public var listenError = ""
    public var discoveryError = ""
    public var multicastJoinError = ""
    public var startNote = ""
    public var interfaces: [String] = []
}

func meshAgeText(_ now: Int64, _ thenMs: Int64) -> String {
    if thenMs <= 0 { return "never" }
    let seconds = min(max((now - thenMs) / 1000, 0), 1 << 31)
    if seconds < 120 { return "\(seconds)s ago" }
    if seconds < 7200 { return "\(seconds / 60)m ago" }
    return "\(seconds / 3600)h ago"
}

func meshErrorText(_ error: Error) -> String {
    if let socket = error as? MeshSocketError { return socket.description }
    return "\(error)"
}

public func defaultMeshHistoryFile(project: String) -> URL? {
    guard let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else {
        return nil
    }
    return directory.appendingPathComponent(meshHistoryFileName(project: project))
}

public func meshHistoryFileName(project: String) -> String {
    "local_mesh_history-\(ChatHistoryCodec.scopeName(serverUrl: project, room: meshConversation)).json"
}

public enum MeshLinkKind: String, Equatable, Sendable {
    case wifi, bluetooth
}

final class MeshLink {
    let socket: MeshTCPConnection?
    let bluetoothId: Int?
    let outbound: Bool
    let buffer = MeshByteBuffer()
    var handshake: MeshLinkHandshake?
    var pending: [JSONObject] = []
    var peerId = ""
    var nodeId = ""
    var helloMs: Int64 = 0
    var keyKnown = false
    var keyId = ""
    var focus: Set<String> = []
    var reader: Task<Void, Never>?
    var deadline: Task<Void, Never>?

    init(socket: MeshTCPConnection? = nil, bluetoothId: Int? = nil, outbound: Bool) {
        self.socket = socket
        self.bluetoothId = bluetoothId
        self.outbound = outbound
    }

    var bluetooth: Bool { bluetoothId != nil }

    var authenticated: Bool { handshake?.authenticated == true && !peerId.isEmpty }

    var kind: MeshLinkKind { bluetooth ? .bluetooth : .wifi }
}

final class MeshPeer {
    var name = ""
    var nodeName = ""
    var platform = ""
    var version = ""
    var nodeId = ""
    var address: MeshAddress?
    var port = 0
    var lastSeenMs: Int64 = 0
    var unlinkedSinceMs: Int64 = 0
    var linked = false
}

final class MeshHeard {
    var address: MeshAddress
    var port: Int
    var lastSeenMs: Int64 = 0
    var unlinkedSinceMs: Int64 = 0
    var control = false
    var kid = ""

    init(address: MeshAddress, port: Int) {
        self.address = address
        self.port = port
    }
}

@MainActor
@Observable
public final class LocalMeshService {
    nonisolated public static let directFileBytesOverWifi = 1024 * 1024
    nonisolated public static let directFileBytesOverBluetooth = 256 * 1024
    nonisolated static let handshakeTimeoutSeconds: TimeInterval = 10
    nonisolated static let forwardMemory = 4096
    nonisolated static let resealOwnLimit = 32
    nonisolated public static let readableNotice = "Anyone nearby with ForkMesh open can read #mesh."
    nonisolated static let maxKnownBundles = 64
    nonisolated static let maxDirectChannelChars = 80

    @ObservationIgnored let settings: SettingsService
    @ObservationIgnored let identity: DeviceIdentity
    @ObservationIgnored let secureStore: SecureStore
    @ObservationIgnored let native: MeshNativeTransport
    @ObservationIgnored let historyFileSource: @MainActor () -> URL?
    @ObservationIgnored let clock: Clock
    @ObservationIgnored let platformName: String
    @ObservationIgnored public let listeners = Listeners()

    @ObservationIgnored private var crypto: RoomCrypto?
    @ObservationIgnored private var seal: RoomSeal?
    @ObservationIgnored private var discovery: MeshUDPSocket?
    @ObservationIgnored private var discoveryReader: Task<Void, Never>?
    @ObservationIgnored private var server: MeshTCPListener?
    @ObservationIgnored private var serverReader: Task<Void, Never>?
    @ObservationIgnored private var beaconTask: Task<Void, Never>?
    @ObservationIgnored private var sweepTask: Task<Void, Never>?
    @ObservationIgnored private var sweepPaceTask: Task<Void, Never>?
    @ObservationIgnored private var sweepQueue: [MeshAddress] = []
    @ObservationIgnored private var sweepDatagram: [UInt8]?
    @ObservationIgnored private var sweepDelivered = 0
    @ObservationIgnored private var sweepUnreachable = 0
    @ObservationIgnored private var sweepRun = 0
    @ObservationIgnored private var groupSendRefused = 0
    @ObservationIgnored private var bluetoothReader: Task<Void, Never>?
    @ObservationIgnored public private(set) var diagnostics = MeshDiagnostics()
    @ObservationIgnored private var startedMs: Int64 = 0

    @ObservationIgnored private(set) var links: [MeshLink] = []
    @ObservationIgnored private var bluetoothHeldOff: [String: Int] = [:]
    @ObservationIgnored private var peerIds: [String] = []
    @ObservationIgnored private var peers: [String: MeshPeer] = [:]
    @ObservationIgnored private var history: [JSONObject] = []
    @ObservationIgnored private var seenIds = Set<String>()
    @ObservationIgnored private var seenOrder: [String] = []
    @ObservationIgnored private var forwarded = Set<String>()
    @ObservationIgnored private var forwardedOrder: [String] = []
    @ObservationIgnored private var knownBundles: [String: JSONObject] = [:]
    @ObservationIgnored private var knownBundleOrder: [String] = []
    @ObservationIgnored private var ownSealed: [String: JSONObject] = [:]
    @ObservationIgnored private var heard: [String: MeshHeard] = [:]
    @ObservationIgnored private var heardOrder: [String] = []
    @ObservationIgnored private(set) var beaconId = ""
    @ObservationIgnored private var resumeInProject = false
    @ObservationIgnored private var dialing = Set<String>()
    @ObservationIgnored private var queued = Set<String>()
    @ObservationIgnored private var historyFile: URL?
    @ObservationIgnored private var historyLoaded = false
    @ObservationIgnored private var historySaveTask: Task<Void, Never>?
    @ObservationIgnored private var beaconCount = 0
    @ObservationIgnored public var isExpired: ChatExpiry?
    @ObservationIgnored public var projectScope: @MainActor () -> String = { "" }
    @ObservationIgnored public var onIncoming: (@MainActor (ChatMessage, Int) -> Void)?
    @ObservationIgnored public var onDirectSealed: (@MainActor (String, JSONObject, String) -> Void)?
    @ObservationIgnored public var onDirectAck: (@MainActor (String, String, String) -> Void)?
    @ObservationIgnored public var onDirectFocus: (@MainActor (String, String, Bool) -> Void)?
    @ObservationIgnored public var onLinkAuthenticated: (@MainActor (String) -> Void)?
    @ObservationIgnored public var onLinkClosed: (@MainActor (String) -> Void)?
    @ObservationIgnored public var rememberedBeaconTargets: (@MainActor () -> [MeshAddress])?

    public private(set) var messages: [ChatMessage] = []
    public private(set) var running = false
    public private(set) var networkActive = false
    public private(set) var unread = 0
    public private(set) var localNetworkBlocked = false
    public private(set) var revision = 0

    @ObservationIgnored public var discoveryPort = meshDiscoveryPort
    @ObservationIgnored public var beaconIntervalMs = beaconIntervalDefaultMs
    @ObservationIgnored public var peerTimeoutMs = peerTimeoutDefaultMs
    @ObservationIgnored public var multicastEnabled = true
    @ObservationIgnored public var addressSweepEnabled = LocalMeshService.defaultAddressSweep
    @ObservationIgnored public var networkEnabled = true
    @ObservationIgnored public var bluetoothEnabled = true
    @ObservationIgnored var loopbackIsLocalNetwork = false
    @ObservationIgnored var linkReplaceAfterMs = 10_000
    @ObservationIgnored var linkStableMs = 5_000
    @ObservationIgnored var redialDelayMs = 1_500

    nonisolated static var defaultAddressSweep: Bool {
        #if os(iOS)
        return true
        #else
        return false
        #endif
    }

    public init(
        settings: SettingsService,
        identity: DeviceIdentity,
        secureStore: SecureStore,
        transport: MeshNativeTransport? = nil,
        historyFile: (@MainActor () -> URL?)? = nil,
        platform: String = AppBuild.currentPlatform,
        clock: Clock = SystemClock()
    ) {
        self.settings = settings
        self.identity = identity
        self.secureStore = secureStore
        self.native = transport ?? UnavailableMeshTransport()
        self.historyFileSource = historyFile ?? { nil }
        self.platformName = platform
        self.clock = clock
    }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    private func notifyListeners() {
        revision &+= 1
        listeners.notify()
    }

    public var bluetoothActive: Bool { native.listening }
    public var hasUnread: Bool { unread > 0 }
    public var listenPort: Int { server?.port ?? 0 }
    public var peerCount: Int { peers.values.filter(\.linked).count }
    public var sealKeyId: String { seal?.keyId ?? "" }

    public func isQueued(_ messageId: String) -> Bool { queued.contains(messageId) }

    // MARK: Diagnostics

    /// One compact line of transport counters for the #mesh debug bar.
    public func diagnosticsLine() -> String {
        let d = diagnostics
        if !running {
            return d.startNote.isEmpty ? "mesh not running" : "mesh not running · \(d.startNote) · \(d.listenError)"
        }
        let now = clock.nowMillis
        let keyed = links.filter(\.keyKnown).count
        var parts: [String] = [
            "udp \(discoveryPort) \(discovery != nil ? "bound" : "CLOSED")\(d.multicastJoinError.isEmpty ? "" : " no-mcast")",
            "tcp \(server.map { String($0.port) } ?? "CLOSED")",
            d.interfaces.isEmpty ? "no interfaces seen" : d.interfaces.joined(separator: ", "),
            "tx mc \(d.multicastSent)\(groupSendRefused > 0 ? " (refused)" : "") "
                + "bc \(d.broadcastSent) uc \(d.unicastSent) "
                + "sweep \(d.sweepSent)\(d.sendErrors > 0 ? " ERR \(d.sendErrors)" : "")",
            "rx \(d.datagramsIn): own \(d.ownBeaconsIn) peer \(d.peerBeaconsIn) "
                + "bad \(d.undecryptableIn) off-net \(d.offNetworkIn)",
            d.lastBeaconMs > 0 ? "heard \(d.lastBeaconFrom) \(meshAgeText(now, d.lastBeaconMs))" : "heard nobody",
            "dial \(d.dials) ok \(d.dialsOk) fail \(d.dialsFailed) · in \(d.inboundAccepted)",
            "links \(links.count) keyed \(keyed) · peers \(peers.count)",
            "frames \(d.framesIn) bad \(d.framesBad) · chat in \(d.chatIn) out \(d.chatOut) queued \(queued.count)",
            native.listening ? "bt on" : "bt off",
        ]
        if localNetworkBlocked { parts.append("LOCAL NETWORK BLOCKED") }
        return parts.joined(separator: " · ")
    }

    /// `diagnosticsLine` plus the last errors and one row per device heard, for a bug report.
    public func diagnosticsDetail() -> String {
        let d = diagnostics
        let now = clock.nowMillis
        var lines: [String] = [
            diagnosticsLine(),
            "node \(dartPrefix(nodeId, 12)) · \(platformName) \(RelayService.clientVersion) · up \(meshAgeText(now, startedMs))",
            "sweep \(addressSweepEnabled ? "on" : "off"), queue \(sweepQueue.count)",
        ]
        if !d.listenError.isEmpty { lines.append("tcp error: \(d.listenError)") }
        if !d.discoveryError.isEmpty { lines.append("udp error: \(d.discoveryError)") }
        if !d.multicastJoinError.isEmpty { lines.append("multicast join: \(d.multicastJoinError)") }
        if !d.lastSendError.isEmpty { lines.append("last send error: \(d.lastSendError)") }
        if !d.lastDialError.isEmpty { lines.append("last dial error: \(d.lastDialError)") }
        if !d.lastDropReason.isEmpty { lines.append("last link drop: \(d.lastDropReason)") }
        if d.inboundRefused > 0 { lines.append("inbound refused: \(d.inboundRefused)") }
        if peers.isEmpty { lines.append("peers: none") }
        for id in peerIds {
            guard let peer = peers[id] else { continue }
            let state: String
            if peer.linked {
                state = "linked"
            } else if peer.unlinkedSinceMs > 0 {
                state = "unlinked since \(meshAgeText(now, peer.unlinkedSinceMs))"
            } else {
                state = "unlinked"
            }
            lines.append(
                "peer \(peer.name.isEmpty ? "?" : peer.name) (\(dartPrefix(id, 8))) "
                    + "\(peer.address?.address ?? "-"):\(peer.port) \(peer.platform) "
                    + "\(peer.version) · \(state) · seen \(meshAgeText(now, peer.lastSeenMs))")
        }
        return lines.joined(separator: "\n")
    }

    private var nodeId: String { identity.nodeId }

    @ObservationIgnored public var accountProvider: (@MainActor () -> String)?

    public var persona: ChatPersona {
        ChatPersona.resolve(
            nodeKey: identity.publicKeyB64url, account: accountProvider?() ?? "", displayName: settings.displayName)
    }

    private var name: String {
        let display = settings.displayName.dartTrimmed
        let account = (accountProvider?() ?? "").dartTrimmed
        if display.isEmpty || display.lowercased() == account.lowercased() {
            return dartPrefix(guestChatName(identity.publicKeyB64url), meshMaxNameChars)
        }
        return dartPrefix(display, meshMaxNameChars)
    }

    public var displayName: String { name }

    public func controlHosts() -> [MeshControlHost] {
        var hosts: [MeshControlHost] = []
        var named = Set<String>()
        for id in heardOrder {
            guard let entry = heard[id], entry.control, entry.port > 0, let peer = peers[entry.kid], !peer.nodeId.isEmpty,
                named.insert(peer.nodeId).inserted
            else { continue }
            hosts.append(
                MeshControlHost(
                    nodeId: peer.nodeId, address: entry.address, port: entry.port, name: peer.name, nodeName: peer.nodeName,
                    platform: peer.platform))
        }
        return hosts
    }

    public func members() -> [Member] {
        let now = clock.nowMillis
        var list = [
            Member(
                id: nodeId, name: name, isSelf: true, online: running, platform: platformName,
                owner: persona.ownerUser)
        ]
        for id in peerIds {
            guard let peer = peers[id] else { continue }
            let shown = peer.nodeId.isEmpty ? id : peer.nodeId
            list.append(
                Member(
                    id: shown, name: peer.name.isEmpty ? shown : peer.name,
                    online: peer.linked || now - peer.lastSeenMs <= Int64(peerTimeoutMs), platform: peer.platform,
                    version: peer.version, nodeName: peer.nodeName))
        }
        return list
    }

    public func directLinks() -> [String: MeshLinkKind] {
        var out: [String: MeshLinkKind] = [:]
        for link in links where link.authenticated {
            if out[link.peerId] == .wifi { continue }
            out[link.peerId] = link.kind
        }
        return out
    }

    public func peerFocus(_ kid: String, channel: String) -> Bool {
        links.contains { $0.authenticated && $0.peerId == kid && $0.focus.contains(channel) }
    }

    public func roomOpened() async {
        await native.roomOpened()
        if !running {
            _ = await start()
        } else if !native.listening {
            _ = await openBluetooth()
        }
    }

    public func markRead() {
        if unread == 0 { return }
        unread = 0
        notifyListeners()
    }

    @discardableResult
    public func start() async -> Bool {
        if running { return true }
        let store = secureStore
        let node = nodeId
        let nodeKey = identity
        let scope = meshSealScope
        let keys: (RoomCrypto, RoomSeal)
        do {
            keys = try await Task.detached(priority: .userInitiated) {
                (
                    RoomCrypto.shared(meshRoomName),
                    try RoomSeal.loadOrCreate(
                        meshRoomName, scope: scope, policy: .open, acct: node, node: node, secureStore: store,
                        nodeIdentity: nodeKey)
                )
            }.value
        } catch {
            diagnostics.startNote = "keys: \(error)"
            notifyListeners()
            return false
        }
        if running { return true }
        crypto = keys.0
        seal = keys.1
        loadHistory()
        diagnostics = MeshDiagnostics()
        startedMs = clock.nowMillis
        running = true
        beaconCount = 0
        groupSendRefused = 0
        renewBeaconId()
        networkActive = openListener()
        if networkActive {
            await openDiscovery()
            await sendBeacon()
            await sendUnicastBeacons()
        }
        let bluetooth = await openBluetooth()
        if !networkActive && !bluetooth {
            running = false
            diagnostics.startNote = "no local network and no Bluetooth"
            notifyListeners()
            return false
        }
        beaconTask?.cancel()
        beaconTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let interval = self?.beaconIntervalMs else { return }
                try? await Task.sleep(nanoseconds: UInt64(interval) * 1_000_000)
                guard let self, !Task.isCancelled, self.running else { return }
                await self.beat()
            }
        }
        sweepTask?.cancel()
        sweepTask = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(sweepIntervalMs) * 1_000_000)
                guard let self, !Task.isCancelled, self.running else { return }
                self.sweepPeers()
            }
        }
        notifyListeners()
        return true
    }

    private func beat() async {
        if !networkActive { networkActive = openListener() }
        if networkActive && discovery == nil { await openDiscovery() }
        await sendBeacon()
        await sendUnicastBeacons()
        if !native.listening { _ = await openBluetooth() }
        await advertiseIfRotated()
    }

    public func resume() async {
        if !running {
            _ = await start()
            return
        }
        closeListener()
        closeDiscovery()
        networkActive = openListener()
        if networkActive { await openDiscovery() }
        await announce()
        if !native.listening { _ = await openBluetooth() }
        notifyListeners()
    }

    public func networkChanged() async {
        if !running {
            _ = await start()
            return
        }
        closeDiscovery()
        renewBeaconId()
        if !networkActive { networkActive = openListener() }
        if networkActive { await openDiscovery() }
        await announce()
        notifyListeners()
    }

    var meshSealScope: String {
        let project = projectScope().trimmingCharacters(in: .whitespacesAndNewlines)
        return project.isEmpty ? meshRoomName : "\(meshRoomName)\n\(project)"
    }

    private func renewBeaconId() {
        beaconId = MeshBeacon.randomId()
        heard.removeAll()
        heardOrder.removeAll()
    }

    public func leaveProject() async {
        let wasRunning = running
        await stop()
        historySaveTask?.cancel()
        historySaveTask = nil
        crypto = nil
        seal = nil
        history = []
        historyFile = nil
        historyLoaded = false
        messages = []
        unread = 0
        seenIds.removeAll()
        seenOrder.removeAll()
        forwarded.removeAll()
        forwardedOrder.removeAll()
        knownBundles.removeAll()
        knownBundleOrder.removeAll()
        ownSealed.removeAll()
        queued.removeAll()
        peers.removeAll()
        peerIds.removeAll()
        heard.removeAll()
        heardOrder.removeAll()
        localNetworkBlocked = false
        resumeInProject = resumeInProject || wasRunning
        notifyListeners()
    }

    public func enterProject() async {
        if !resumeInProject { return }
        resumeInProject = false
        _ = await start()
    }

    public func announce() async {
        if !running { return }
        if networkActive && discovery == nil { await openDiscovery() }
        beaconCount = 0
        groupSendRefused = 0
        await sendBeacon()
        await sendUnicastBeacons()
    }

    private func closeListener() {
        serverReader?.cancel()
        serverReader = nil
        server?.closeAndWait(timeout: 0.5)
        server = nil
    }

    private func closeDiscovery() {
        discoveryReader?.cancel()
        discoveryReader = nil
        discovery?.close()
        discovery = nil
    }

    private func listenerEnded(_ listener: MeshTCPListener) {
        guard server === listener else { return }
        serverReader = nil
        server = nil
        networkActive = false
        diagnostics.listenError = "listener closed"
        notifyListeners()
    }

    private func discoveryEnded(_ socket: MeshUDPSocket) {
        guard discovery === socket else { return }
        discoveryReader = nil
        discovery = nil
        diagnostics.discoveryError = "discovery socket closed"
    }

    public func stop() async {
        if !running { return }
        await sendToAll(envelope("bye"))
        running = false
        if historySaveTask != nil {
            historySaveTask?.cancel()
            historySaveTask = nil
            saveHistory()
        }
        beaconTask?.cancel()
        sweepTask?.cancel()
        beaconTask = nil
        sweepTask = nil
        sweepPaceTask?.cancel()
        sweepPaceTask = nil
        sweepQueue = []
        sweepDatagram = nil
        for link in links {
            await closeLink(link, notify: false)
        }
        links.removeAll()
        dialing.removeAll()
        for peer in peers.values { peer.linked = false }
        closeDiscovery()
        await native.releaseMulticastLock()
        closeListener()
        bluetoothHeldOff.removeAll()
        await native.stopBluetooth()
        networkActive = false
        notifyListeners()
    }

    // MARK: Local network transport

    private func openListener() -> Bool {
        if server != nil { return true }
        if !networkEnabled { return false }
        if !hasLocalNetwork() { return false }
        do {
            // Any address, because the mesh spans every local interface; every peer is re-checked
            // against isLocalNetworkAddress() on accept.
            let listener: MeshTCPListener
            if let preferred = try? MeshTCPListener.bind(address: .anyIPv4, port: discoveryPort) {
                listener = preferred
            } else {
                listener = try MeshTCPListener.bind(address: .anyIPv4, port: 0)
            }
            server = listener
            serverReader = Task { [weak self] in
                for await connection in listener.connections {
                    guard let self else {
                        connection.close()
                        return
                    }
                    self.accept(connection)
                }
                self?.listenerEnded(listener)
            }
            diagnostics.listenError = ""
            return true
        } catch {
            diagnostics.listenError = meshErrorText(error)
            return false
        }
    }

    private func accept(_ connection: MeshTCPConnection) {
        if !running || links.count >= maxLinks || !isLocalNetworkAddress(connection.remoteAddress) {
            diagnostics.inboundRefused += 1
            connection.close()
            return
        }
        diagnostics.inboundAccepted += 1
        attachSocket(connection, outbound: false)
    }

    private func hasLocalNetwork() -> Bool {
        do {
            for iface in try MeshInterfaces.list(includeLoopback: loopbackIsLocalNetwork) {
                if iface.addresses.contains(where: isLocalNetworkAddress) { return true }
            }
            diagnostics.listenError = "no local IPv4 address"
        } catch {
            diagnostics.listenError = "interfaces: \(meshErrorText(error))"
        }
        return false
    }

    private func openDiscovery() async {
        if discovery != nil || !multicastEnabled { return }
        // Android hands multicast to an app only while a lock is held.
        _ = await native.acquireMulticastLock()
        do {
            let socket = try MeshUDPSocket.bind(address: .anyIPv4, port: discoveryPort, reuse: true)
            // TTL 1 is the hard stop that keeps a beacon on the local segment.
            socket.setMulticastHops(1)
            socket.setMulticastLoopback(true)
            socket.setBroadcastEnabled(true)
            do {
                try socket.joinMulticast(MeshAddress(meshMulticastGroup)!)
                diagnostics.multicastJoinError = ""
            } catch {
                // A network that filters the group still works for links opened by address, and
                // for Bluetooth.
                diagnostics.multicastJoinError = meshErrorText(error)
            }
            discovery = socket
            diagnostics.discoveryError = ""
            discoveryReader = Task { [weak self] in
                for await datagram in socket.datagrams {
                    guard let self else { return }
                    await self.onDatagram(datagram)
                }
                self?.discoveryEnded(socket)
            }
        } catch {
            discovery = nil
            diagnostics.discoveryError = meshErrorText(error)
        }
    }

    /// Sends `datagram` the way a beacon goes out: a test's way to have the system refuse a send.
    func sendDiscoveryDatagram(_ datagram: [UInt8], to address: MeshAddress, port: Int) -> Int {
        sendDatagram(datagram, address, port, count: {})
    }

    /// Sends one discovery datagram and counts it, or records why it failed. Returns the bytes
    /// written, 0 when the send buffer is full, or -1 when the system refused it (`refused` gets
    /// its errno).
    @discardableResult
    private func sendDatagram(
        _ datagram: [UInt8], _ to: MeshAddress, _ port: Int, count: () -> Void,
        refused: ((Int32) -> Void)? = nil
    ) -> Int {
        guard let socket = discovery else { return -1 }
        switch socket.send(datagram, to: to, port: port) {
        case .sent(let written):
            count()
            return written
        case .bufferFull:
            diagnostics.sendErrors += 1
            diagnostics.lastSendError = "\(to.address): send buffer full"
            return 0
        case .refused(let error):
            diagnostics.sendErrors += 1
            diagnostics.lastSendError = "\(to.address): \(error.description)"
            refused?(error.code)
            if error.meansSocketGone && discovery === socket {
                closeDiscovery()
                diagnostics.discoveryError = "discovery socket gone: \(error.description)"
            }
            return -1
        }
    }

    private func beaconDatagram(reply: Bool = false) -> [UInt8]? {
        guard let crypto, let server, !beaconId.isEmpty else { return nil }
        let beacon = MeshBeacon(nodeId: beaconId, port: server.port, timestampMs: clock.nowMillis, reply: reply)
        guard let envelope = try? crypto.encrypt(beacon.toJSON()) else { return nil }
        return Array(JSON.encode(.object(envelope)).utf8)
    }

    private func sendBeacon() async {
        if !running || discovery == nil { return }
        guard let datagram = beaconDatagram() else { return }
        // iOS refuses multicast and broadcast alike to an app without Apple's multicast
        // entitlement; once refused they are only retried now and then.
        if groupSendRefused > 0 {
            let beat = groupSendRefused
            groupSendRefused += 1
            if beat % groupSendRetryBeats != 0 { return }
        }
        sendDatagram(
            datagram, MeshAddress(meshMulticastGroup)!, discoveryPort,
            count: {
                diagnostics.multicastSent += 1
                groupSendRefused = 0
            },
            refused: { code in
                if code == MeshSocketError.hostUnreachable && self.groupSendRefused == 0 { self.groupSendRefused = 1 }
            })
        if groupSendRefused > 0 || !addressSweepEnabled { return }
        // And to the subnet's broadcast address. Many access points drop multicast between Wi-Fi
        // clients, but none drops broadcast: ARP and DHCP depend on it.
        for (address, prefixLength) in sweepInterfaceAddresses() {
            guard let broadcast = sweepBroadcastAddress(address, prefixLength: prefixLength) else { continue }
            sendDatagram(datagram, broadcast, discoveryPort, count: { diagnostics.broadcastSent += 1 })
        }
    }

    /// This device's IPv4 addresses, with their prefix lengths, on the interfaces
    /// `isSweptInterfaceName` accepts.
    private func sweepInterfaceAddresses() -> [(MeshAddress, Int)] {
        var addresses: [(MeshAddress, Int)] = []
        do {
            var seen: [String] = []
            for iface in try MeshInterfaces.list() {
                let swept = isSweptInterfaceName(iface.name)
                // Every interface is listed for the debug bar; "*" marks the swept.
                seen.append(
                    "\(iface.name) \(iface.addresses.map(\.address).joined(separator: "/"))\(swept ? "*" : "")")
                if !swept { continue }
                addresses.append(contentsOf: zip(iface.addresses, iface.prefixLengths).map { ($0, $1) })
            }
            diagnostics.interfaces = seen
        } catch {
            diagnostics.interfaces = ["interfaces: \(meshErrorText(error))"]
        }
        return addresses
    }

    /// Unicast beacons for a network where multicast does not arrive: to every unlinked peer
    /// already heard from, and every few beats to the whole local subnet. Sent from the discovery
    /// port, so the answer comes back to it through a stateful firewall.
    private func sendUnicastBeacons() async {
        if !running || discovery == nil { return }
        let sweepEvery = links.isEmpty ? addressSweepEveryBeatsAlone : addressSweepEveryBeats
        let sweep = addressSweepEnabled && beaconCount % sweepEvery == 0
        beaconCount += 1
        var targetOrder: [String] = []
        var targets: [String: MeshAddress] = [:]
        let linkedAddresses = Set(
            peerIds.compactMap { id in linkForPeer(id) == nil ? nil : peers[id]?.address?.address })
        for address in rememberedBeaconTargets?() ?? [] where !linkedAddresses.contains(address.address) {
            if targets[address.address] == nil { targetOrder.append(address.address) }
            targets[address.address] = address
        }
        for id in heardOrder {
            guard let entry = heard[id], linkForPeer(entry.kid) == nil else { continue }
            if targets[entry.address.address] == nil { targetOrder.append(entry.address.address) }
            targets[entry.address.address] = entry.address
        }
        for id in peerIds {
            guard let address = peers[id]?.address, linkForPeer(id) == nil else { continue }
            if targets[address.address] == nil { targetOrder.append(address.address) }
            targets[address.address] = address
        }
        var sweptOrder: [MeshAddress] = []
        var swept = Set<String>()
        if sweep {
            for (address, prefixLength) in sweepInterfaceAddresses() {
                for host in sweepAddresses(address, prefixLength: prefixLength) where targets[host.address] == nil {
                    if swept.insert(host.address).inserted { sweptOrder.append(host) }
                }
            }
        }
        if targets.isEmpty && sweptOrder.isEmpty { return }
        guard let datagram = beaconDatagram() else { return }
        for key in targetOrder {
            sendDatagram(datagram, targets[key]!, discoveryPort, count: { diagnostics.unicastSent += 1 })
        }
        if !sweep { return }
        // A sweep still draining when the next one is due is replaced by it.
        sweepQueue = sweptOrder
        sweepDatagram = datagram
        sweepDelivered = 0
        sweepUnreachable = 0
        sweepRun += 1
        drainSweep()
    }

    private func drainSweep() {
        sweepPaceTask?.cancel()
        sweepPaceTask = nil
        guard running, discovery != nil, let datagram = sweepDatagram else {
            sweepQueue = []
            sweepDatagram = nil
            return
        }
        var sent = 0
        while sent < sweepBatch && !sweepQueue.isEmpty {
            let host = sweepQueue[0]
            let written = sendDatagram(
                datagram, host, discoveryPort, count: { diagnostics.sweepSent += 1 },
                refused: { code in
                    if code == MeshSocketError.hostUnreachable { self.sweepUnreachable += 1 }
                })
            // Nothing written means the send buffer is full; the same host goes first next batch.
            // A refused host (known to be down) is skipped.
            if written == 0 { break }
            if written > 0 { sweepDelivered += 1 }
            sweepQueue.removeFirst()
            sent += 1
        }
        // A first batch refused whole is Local Network access being off: the rest of the subnet
        // would fare the same.
        if sweepDelivered == 0 && sweepUnreachable >= sweepBatch { sweepQueue = [] }
        if sweepQueue.isEmpty {
            sweepDatagram = nil
            let blocked = sweepDelivered == 0 && sweepUnreachable > 0
            if blocked != localNetworkBlocked {
                localNetworkBlocked = blocked
                notifyListeners()
            }
            return
        }
        let run = sweepRun
        sweepPaceTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(sweepBatchGapMs) * 1_000_000)
            guard let self, !Task.isCancelled, run == self.sweepRun else { return }
            self.drainSweep()
        }
    }

    private func onDatagram(_ datagram: MeshDatagram) async {
        guard running, let crypto else { return }
        diagnostics.datagramsIn += 1
        if !isLocalNetworkAddress(datagram.address) {
            diagnostics.offNetworkIn += 1
            return
        }
        var beacon: MeshBeacon?
        if case .object(let envelope)? = try? JSON.parse(datagram.data), let plain = crypto.decrypt(envelope) {
            beacon = MeshBeacon.parse(plain)
        }
        guard let beacon else {
            diagnostics.undecryptableIn += 1
            return
        }
        if beacon.nodeId == beaconId {
            diagnostics.ownBeaconsIn += 1
            return
        }
        let now = clock.nowMillis
        diagnostics.peerBeaconsIn += 1
        diagnostics.lastBeaconMs = now
        let entry = rememberHeard(beacon.nodeId, address: datagram.address, port: beacon.port)
        let who = peers[entry.kid].map(\.name).flatMap { $0.isEmpty ? nil : $0 } ?? dartPrefix(beacon.nodeId, 8)
        diagnostics.lastBeaconFrom = "\(who) \(datagram.address.address):\(beacon.port)\(beacon.reply ? " (reply)" : "")"
        let controlChanged = entry.control != beacon.control
        entry.control = beacon.control
        entry.lastSeenMs = now
        if controlChanged && !entry.kid.isEmpty { notifyListeners() }
        if let peer = peers[entry.kid], linkForPeer(entry.kid) != nil {
            entry.unlinkedSinceMs = 0
            peer.lastSeenMs = now
            return
        }
        if !beacon.reply, let answer = beaconDatagram(reply: true), discovery != nil {
            sendDatagram(answer, datagram.address, datagram.port, count: { diagnostics.unicastSent += 1 })
        }
        if entry.unlinkedSinceMs == 0 { entry.unlinkedSinceMs = now }
        if beaconId.utf16.lexicographicallyPrecedes(beacon.nodeId.utf16)
            || now - entry.unlinkedSinceMs >= Int64(beaconIntervalMs / 2)
        {
            let address = datagram.address
            let port = beacon.port
            Task { await self.connectToPeer(address, port: port) }
        }
    }

    private func rememberHeard(_ id: String, address: MeshAddress, port: Int) -> MeshHeard {
        if let entry = heard[id] {
            if entry.address != address || entry.port != port {
                entry.kid = ""
                entry.address = address
                entry.port = port
            }
            return entry
        }
        let entry = MeshHeard(address: address, port: port)
        heard[id] = entry
        heardOrder.append(id)
        while heardOrder.count > maxLinks {
            heard.removeValue(forKey: heardOrder.removeFirst())
        }
        return entry
    }

    private func bindHeard(_ address: MeshAddress, port: Int, kid: String) {
        if kid.isEmpty || port <= 0 { return }
        var bound = false
        for entry in heard.values where entry.address == address && entry.port == port && entry.kid != kid {
            entry.kid = kid
            entry.unlinkedSinceMs = 0
            bound = bound || entry.control
        }
        if bound { notifyListeners() }
    }

    @discardableResult
    public func connectToPeer(_ address: MeshAddress, port: Int) async -> Bool {
        if !running || port <= 0 || !isLocalNetworkAddress(address) { return false }
        if links.count >= maxLinks { return false }
        let key = "\(address.address):\(port)"
        if dialing.contains(key) { return false }
        if links.contains(where: {
            $0.outbound && $0.peerId.isEmpty && $0.socket?.remoteAddress == address && $0.socket?.remotePort == port
        }) {
            return false
        }
        dialing.insert(key)
        diagnostics.dials += 1
        do {
            let connection = try await MeshTCPConnection.connect(to: address, port: port, timeout: dialTimeoutSeconds)
            dialing.remove(key)
            diagnostics.dialsOk += 1
            if !running {
                connection.close()
                return false
            }
            attachSocket(connection, outbound: true)
            return true
        } catch {
            dialing.remove(key)
            diagnostics.dialsFailed += 1
            diagnostics.lastDialError = "\(key): \(meshErrorText(error))"
            return false
        }
    }

    private func attachSocket(_ connection: MeshTCPConnection, outbound: Bool) {
        connection.setKeepAlive(idle: keepAliveIdleSeconds, interval: keepAliveIntervalSeconds, count: keepAliveCount)
        let link = MeshLink(socket: connection, outbound: outbound)
        links.append(link)
        link.reader = Task { [weak self] in
            await self?.openLink(link)
            for await event in connection.events {
                guard let self else { return }
                switch event {
                case .data(let data):
                    await self.onLinkData(link, data)
                case .closed(let error):
                    await self.closeLink(link, reason: error.map(meshErrorText) ?? "closed by peer")
                }
            }
        }
    }

    private func openBluetooth() async -> Bool {
        if !bluetoothEnabled || !running { return false }
        if native.listening { return true }
        let started = await native.startBluetooth()
        if !started { return false }
        if bluetoothReader == nil {
            let events = native.events
            bluetoothReader = Task { [weak self] in
                for await event in events {
                    guard let self else { return }
                    await self.onBluetooth(event)
                }
            }
        }
        notifyListeners()
        return true
    }

    private func onBluetooth(_ event: MeshBluetoothEvent) async {
        switch event.kind {
        case .linkOpened:
            if !running {
                await native.closeBluetoothLink(event.linkId)
                return
            }
            let link = MeshLink(bluetoothId: event.linkId, outbound: event.outbound)
            links.append(link)
            await openLink(link)
        case .data:
            if let link = linkForBluetooth(event.linkId) { await onLinkData(link, event.bytes) }
        case .linkClosed:
            if let link = linkForBluetooth(event.linkId) { await closeLink(link, reason: "bluetooth closed") }
        }
    }

    private func linkForBluetooth(_ id: Int) -> MeshLink? { links.first { $0.bluetoothId == id } }

    private func openLink(_ link: MeshLink) async {
        guard let seal, let handshake = try? MeshLinkHandshake(keyring: seal.keyring, initiator: link.outbound) else {
            await closeLink(link, reason: "no link keys")
            return
        }
        link.handshake = handshake
        _ = await writeRaw(link, Array(JSON.encode(handshake.hello).utf8))
        let timeout = UInt64(LocalMeshService.handshakeTimeoutSeconds * 1_000_000_000)
        link.deadline = Task { [weak self, weak link] in
            try? await Task.sleep(nanoseconds: timeout)
            guard let self, let link, !Task.isCancelled, !link.authenticated, self.links.contains(where: { $0 === link })
            else { return }
            await self.closeLink(link, reason: "handshake timeout")
        }
    }

    private func onLinkData(_ link: MeshLink, _ data: Data) async {
        link.buffer.add(data)
        while true {
            let payload: Data?
            do {
                payload = try meshTakeFrame(
                    link.buffer, maxBytes: link.authenticated ? meshMaxFrameBytes : meshMaxHandshakeBytes)
            } catch {
                await closeLink(link, reason: "oversized frame")
                return
            }
            guard let payload else { return }
            await handleFrame(link, payload)
            if !links.contains(where: { $0 === link }) { return }
        }
    }

    private func handleFrame(_ link: MeshLink, _ payload: Data) async {
        guard let seal, let handshake = link.handshake else { return }
        diagnostics.framesIn += 1
        if !handshake.authenticated {
            guard case .object(let frame)? = try? JSON.parse(payload) else {
                diagnostics.framesBad += 1
                await closeLink(link, reason: "link handshake")
                return
            }
            do {
                if handshake.peerIdentityKey == nil {
                    let auth = try handshake.receiveHello(frame)
                    if seal.trust.record(handshake.peerKid)?.state == .blocked {
                        await closeLink(link, reason: "blocked device")
                        return
                    }
                    _ = await writeRaw(link, Array(JSON.encode(auth).utf8))
                } else {
                    try handshake.receiveAuth(frame)
                    await linkAuthenticated(link)
                }
            } catch {
                diagnostics.framesBad += 1
                await closeLink(link, reason: "link handshake")
            }
            return
        }
        guard let frame = try? handshake.open(Array(payload)) else {
            diagnostics.framesBad += 1
            await closeLink(link, reason: "bad link frame")
            return
        }
        await handleLinkFrame(link, frame)
    }

    private func linkAuthenticated(_ link: MeshLink) async {
        guard let seal, let handshake = link.handshake else { return }
        link.deadline?.cancel()
        link.deadline = nil
        let kid = handshake.peerKid
        let other = linkForPeer(kid)
        link.peerId = kid
        link.keyId = kid
        link.helloMs = clock.nowMillis
        if let other, other !== link {
            if link.bluetooth != other.bluetooth {
                let loser = link.bluetooth ? link : other
                await closeLink(loser, reason: "duplicate link")
                if loser === link { return }
            } else if link.helloMs - other.helloMs >= Int64(linkReplaceAfterMs) {
                await closeLink(other, reason: "replaced")
            } else {
                let mine = link.outbound ? seal.keyId : kid
                let theirs = other.outbound ? seal.keyId : kid
                if mine.utf16.lexicographicallyPrecedes(theirs.utf16) {
                    await closeLink(link, reason: "duplicate link")
                    return
                }
                await closeLink(other, reason: "duplicate link")
            }
        }
        if !links.contains(where: { $0 === link }) { return }
        rememberPeer(kid, [:], linked: false)
        if let peer = peers[kid], peer.address == nil, let socket = link.socket { peer.address = socket.remoteAddress }
        if link.outbound, let socket = link.socket { bindHeard(socket.remoteAddress, port: socket.remotePort, kid: kid) }
        if let bundle = ownBundle() { _ = await writeLink(link, bundle) }
        for known in knownBundleOrder where known != kid {
            if let bundle = knownBundles[known] { _ = await writeLink(link, bundle) }
        }
        var hello = envelope("hello")
        if let server { hello["port"] = .int(Int64(server.port)) }
        link.pending.insert(hello, at: 0)
        onLinkAuthenticated?(kid)
        notifyListeners()
    }

    private func handleLinkFrame(_ link: MeshLink, _ frame: JSONObject) async {
        switch dartString(frame["type"]) {
        case RoomSeal.keyType:
            await acceptMeshBundle(frame, from: link)
        case RoomSeal.sealedType:
            await acceptMeshSealed(frame, from: link)
        case "direct-sealed":
            let channel = dartString(frame["channel"])
            guard isDirectChannel(channel), let sealed = frame["frame"]?.objectValue,
                RoomSeal.frameKeyId(sealed) == link.peerId
            else {
                diagnostics.framesBad += 1
                return
            }
            onDirectSealed?(channel, sealed, link.peerId)
        case "direct-ack":
            let channel = dartString(frame["channel"])
            let id = dartString(frame["id"])
            guard isDirectChannel(channel), !id.isEmpty, id.count <= 64 else { return }
            onDirectAck?(channel, id, link.peerId)
        case "focus":
            let channel = dartString(frame["channel"])
            guard isDirectChannel(channel), let on = frame["on"]?.boolValue else { return }
            if on {
                if link.focus.count >= 16 && !link.focus.contains(channel) { return }
                link.focus.insert(channel)
            } else {
                link.focus.remove(channel)
            }
            onDirectFocus?(link.peerId, channel, on)
        default:
            diagnostics.framesBad += 1
        }
    }

    private func isDirectChannel(_ channel: String) -> Bool {
        channel.hasPrefix(DirectMessageFrames.prefix) && channel.count <= LocalMeshService.maxDirectChannelChars
    }

    private func acceptMeshBundle(_ frame: JSONObject, from link: MeshLink) async {
        guard let seal, let result = seal.acceptBundle(frame, now: clock.nowMillis) else {
            diagnostics.framesBad += 1
            return
        }
        let kid = result.kid
        if kid == seal.keyId { return }
        if result.isNew || result.updated || knownBundles[kid] == nil { rememberBundle(kid, frame) }
        if result.isNew || result.updated {
            for other in links where other !== link && other.authenticated && other.peerId != kid {
                _ = await writeLink(other, frame)
            }
        }
        if kid == link.peerId && !link.keyKnown {
            link.keyKnown = true
            let waiting = link.pending
            link.pending.removeAll()
            for plain in waiting { _ = await writePlain(link, plain, only: [kid]) }
        }
        if result.isNew && kid != link.peerId { await resealOwnHistory(to: kid, via: nil) }
        for opened in seal.reopenHeld(now: clock.nowMillis) { await handleOpened(opened, from: nil) }
        notifyListeners()
    }

    private func acceptMeshSealed(_ frame: JSONObject, from link: MeshLink) async {
        guard let seal, RoomSeal.isSealedFrame(frame) else {
            diagnostics.framesBad += 1
            return
        }
        guard rememberForwarded(dartString(frame["sig"])) else { return }
        let sender = dartString(frame["kid"])
        let result = seal.open(frame, now: clock.nowMillis)
        if case .invalid = result {
            diagnostics.framesBad += 1
            return
        }
        let addressed = Set(RoomSeal.wrappedKeyIds(frame))
        let flood = dartString(result.plain?["type"]) == "chat"
        for other in links where other !== link && other.authenticated && other.peerId != sender {
            if flood || addressed.contains(other.peerId) { _ = await writeLink(other, frame) }
        }
        await handleOpened(result, from: link)
    }

    private func handleOpened(_ result: RoomSealOpen, from link: MeshLink?) async {
        guard case .opened(_, let senderKid, _, _) = result, let plain = result.authenticatedPlain else { return }
        await handlePlain(plain, senderKid: senderKid)
    }

    private func handlePlain(_ plain: JSONObject, senderKid: String) async {
        let type = dartString(plain["type"])
        let senderId = dartString(plain["senderId"])
        if senderId.isEmpty || senderId == nodeId || senderKid == seal?.keyId { return }
        if dartString(plain["conversation"]) != meshConversation { return }
        let direct = linkForPeer(senderKid)
        if let direct { direct.nodeId = senderId }
        if type == "hello" || type == "bye" || type == "typing" {
            guard direct != nil else { return }
        }
        rememberPeer(senderKid, plain, linked: direct != nil)
        if type == "hello", let direct, let peer = peers[senderKid], let socket = direct.socket {
            if case .int(let port)? = plain["port"], port > 0 && port <= 65535 {
                peer.address = socket.remoteAddress
                peer.port = Int(port)
                bindHeard(socket.remoteAddress, port: Int(port), kid: senderKid)
            } else if peer.address == nil {
                peer.address = socket.remoteAddress
            }
        }
        if type == "typing" { return }

        let id = dartString(plain["id"])
        if id.isEmpty || !markSeen(id) { return }

        if type == "hello" {
            if let direct { await resealOwnHistory(to: senderKid, via: direct) }
            return
        }
        if type == "bye" {
            if removePeer(senderKid) { notifyListeners() }
            return
        }
        if type != "chat" { return }
        diagnostics.chatIn += 1
        storeHistory(plain)
        let message: ChatMessage = toMessage(plain)
        let added: Bool = appendMessage(message)
        notifyListeners()
        if added && !message.isSelf { onIncoming?(message, unread) }
    }

    private func resealOwnHistory(to kid: String, via link: MeshLink?) async {
        guard let seal else { return }
        let own = history.filter { dartString($0["senderId"]) == nodeId && dartString($0["type"]) == "chat" }
        for entry in own.suffix(LocalMeshService.resealOwnLimit) {
            let id = dartString(entry["id"])
            let sealed: JSONObject
            if queued.contains(id) {
                guard let fresh = try? seal.seal(entry, for: [kid], now: clock.nowMillis) else { continue }
                sealed = fresh
                rememberForwarded(dartString(sealed["sig"]))
            } else if let earlier = ownSealed[id], RoomSeal.wrappedKeyIds(earlier).contains(kid) {
                sealed = earlier
            } else {
                continue
            }
            let targets = link.map { [$0] } ?? links.filter { $0.authenticated && $0.keyKnown }
            var wrote = false
            for target in targets where target.authenticated {
                if await writeLink(target, sealed) { wrote = true }
            }
            if wrote && queued.remove(id) != nil {
                rememberOwnSealed(entry, sealed)
                scheduleHistorySave()
                notifyListeners()
            }
        }
        await advertiseIfRotated()
    }

    private func rememberOwnSealed(_ plain: JSONObject, _ sealed: JSONObject) {
        guard dartString(plain["type"]) == "chat", dartString(plain["senderId"]) == nodeId else { return }
        let id = dartString(plain["id"])
        if id.isEmpty { return }
        ownSealed[id] = sealed
        if ownSealed.count > historyLimit {
            let kept = Set(history.map { dartString($0["id"]) })
            ownSealed = ownSealed.filter { kept.contains($0.key) }
        }
    }

    private func ownBundle() -> JSONObject? {
        guard let seal else { return nil }
        let frame = seal.keyFrame(now: clock.nowMillis)
        return frame.isEmpty ? nil : frame
    }

    private func advertiseIfRotated() async {
        guard let seal, seal.needsAdvertisement(now: clock.nowMillis), let bundle = ownBundle() else { return }
        for link in links where link.authenticated { _ = await writeLink(link, bundle) }
    }

    private func rememberBundle(_ kid: String, _ frame: JSONObject) {
        if knownBundles.updateValue(frame, forKey: kid) == nil { knownBundleOrder.append(kid) }
        while knownBundleOrder.count > LocalMeshService.maxKnownBundles {
            knownBundles.removeValue(forKey: knownBundleOrder.removeFirst())
        }
    }

    @discardableResult
    private func rememberForwarded(_ signature: String) -> Bool {
        if signature.isEmpty || forwarded.contains(signature) { return false }
        forwarded.insert(signature)
        forwardedOrder.append(signature)
        while forwardedOrder.count > LocalMeshService.forwardMemory { forwarded.remove(forwardedOrder.removeFirst()) }
        return true
    }

    public func sendDirectSealed(_ frame: JSONObject, channel: String, to kid: String) async -> Bool {
        guard running, isDirectChannel(channel), let link = authenticatedLink(kid) else { return false }
        return await writeLink(link, ["type": "direct-sealed", "channel": .string(channel), "frame": .object(frame)])
    }

    @discardableResult
    public func sendDirectAck(channel: String, id: String, to kid: String) async -> Bool {
        guard running, isDirectChannel(channel), !id.isEmpty, let link = authenticatedLink(kid) else { return false }
        return await writeLink(link, ["type": "direct-ack", "channel": .string(channel), "id": .string(id)])
    }

    @discardableResult
    public func sendFocus(channel: String, on: Bool, to kid: String) async -> Bool {
        guard running, isDirectChannel(channel), let link = authenticatedLink(kid) else { return false }
        return await writeLink(link, ["type": "focus", "channel": .string(channel), "on": .bool(on)])
    }

    private func authenticatedLink(_ kid: String) -> MeshLink? {
        let candidates = links.filter { $0.authenticated && $0.peerId == kid }
        return candidates.first { !$0.bluetooth } ?? candidates.first
    }

    private func writePlain(_ link: MeshLink, _ plain: JSONObject, only: [String]? = nil) async -> Bool {
        guard let seal else { return false }
        let sealedFrame: JSONObject?
        if let only {
            sealedFrame = try? seal.seal(plain, for: only, now: clock.nowMillis)
        } else {
            sealedFrame = try? seal.seal(plain, now: clock.nowMillis)
        }
        guard let sealed = sealedFrame else { return false }
        rememberForwarded(dartString(sealed["sig"]))
        let written = await writeLink(link, sealed)
        let forOthers = RoomSeal.wrappedKeyIds(sealed).contains { $0 != seal.keyId }
        if written && forOthers && queued.remove(dartString(plain["id"])) != nil {
            rememberOwnSealed(plain, sealed)
            scheduleHistorySave()
            notifyListeners()
        }
        await advertiseIfRotated()
        return written
    }

    private func writeLink(_ link: MeshLink, _ frame: JSONObject) async -> Bool {
        guard link.authenticated, let handshake = link.handshake, let data = try? handshake.seal(frame) else { return false }
        return await writeRaw(link, data)
    }

    private func writeRaw(_ link: MeshLink, _ payload: [UInt8]) async -> Bool {
        if payload.count > meshMaxFrameBytes { return false }
        let framed = meshFrameBytes(payload)
        if let socket = link.socket {
            if socket.isClosed {
                await closeLink(link, reason: "write: \(MeshSocketError.closed.description)")
                return false
            }
            socket.write(framed)
            return true
        }
        if let id = link.bluetoothId { return await native.sendBluetooth(id, framed) }
        return false
    }

    private func sendToAll(_ plain: JSONObject) async {
        guard let seal else { return }
        var ready: [MeshLink] = []
        for link in links {
            if !link.authenticated || !link.keyKnown {
                if link.pending.count < 32 { link.pending.append(plain) }
                continue
            }
            ready.append(link)
        }
        if ready.isEmpty { return }
        guard let sealed = try? seal.seal(plain, now: clock.nowMillis) else { return }
        rememberForwarded(dartString(sealed["sig"]))
        var wrote = false
        for link in ready {
            if await writeLink(link, sealed) { wrote = true }
        }
        let forOthers = RoomSeal.wrappedKeyIds(sealed).contains { $0 != seal.keyId }
        if wrote && forOthers { rememberOwnSealed(plain, sealed) }
        if wrote && forOthers && queued.remove(dartString(plain["id"])) != nil {
            scheduleHistorySave()
            notifyListeners()
        }
        await advertiseIfRotated()
    }

    private func closeLink(_ link: MeshLink, notify: Bool = true, reason: String = "") async {
        let present = links.contains { $0 === link }
        if present {
            let who = link.peerId.isEmpty ? "unnamed link" : dartPrefix(link.peerId, 8)
            diagnostics.lastDropReason = "\(who): \(reason.isEmpty ? "closed" : reason)"
        }
        links.removeAll { $0 === link }
        link.deadline?.cancel()
        link.deadline = nil
        link.handshake?.wipe()
        link.socket?.close()
        let duplicate = reason == "duplicate link"
        let peerId = link.peerId
        if let id = link.bluetoothId {
            await native.closeBluetoothLink(id, duplicate: duplicate)
            if present && duplicate && !peerId.isEmpty { bluetoothHeldOff[peerId] = id }
        }
        guard !peerId.isEmpty, linkForPeer(peerId) == nil else { return }
        if present { onLinkClosed?(peerId) }
        guard let peer = peers[peerId] else { return }
        peer.linked = false
        if notify { notifyListeners() }
        if !present || !running || deliberateDropReasons.contains(reason) { return }
        let now = clock.nowMillis
        peer.lastSeenMs = max(peer.lastSeenMs, now)
        if !link.bluetooth, let heldOff = bluetoothHeldOff.removeValue(forKey: peerId) {
            await native.liftBluetoothHoldOff(heldOff)
        }
        if link.helloMs == 0 || now - link.helloMs < Int64(linkStableMs) { return }
        if link.bluetooth {
            await native.rescanBluetoothSoon()
        } else if let address = peer.address, peer.port > 0 {
            scheduleRedial(peerId, address, peer.port)
        }
    }

    private func scheduleRedial(_ peerId: String, _ address: MeshAddress, _ port: Int) {
        let delay = UInt64(max(redialDelayMs, 0)) * 1_000_000
        Task { [weak self] in
            try? await Task.sleep(nanoseconds: delay)
            guard let self, self.running, self.linkForPeer(peerId) == nil else { return }
            await self.connectToPeer(address, port: port)
        }
    }

    private func linkForPeer(_ peerId: String) -> MeshLink? {
        if peerId.isEmpty { return nil }
        return links.first { $0.peerId == peerId }
    }

    private func envelope(_ type: String) -> JSONObject {
        var frame: JSONObject = [
            "type": .string(type),
            "id": .string(LocalMeshService.messageId(nodeId, clock)),
            "senderId": .string(nodeId),
            "sender": .string(name),
            "conversation": .string(meshConversation),
            "ts": .int(clock.nowMillis),
            "platform": .string(platformName),
            "version": .string(RelayService.clientVersion),
        ]
        frame["accountKind"] = .string(persona.accountKind)
        return frame
    }

    @discardableResult
    public func sendMessage(_ text: String) async -> String {
        let trimmed = text.dartTrimmed
        if !running || trimmed.isEmpty { return "" }
        var message = envelope("chat")
        message["text"] = .string(dartPrefix(trimmed, meshMaxTextChars))
        let id = dartString(message["id"])
        _ = markSeen(id)
        storeHistory(message)
        queued.insert(id)
        diagnostics.chatOut += 1
        await sendToAll(message)
        appendMessage(toMessage(message))
        notifyListeners()
        return id
    }

    public func sendTyping(_ active: Bool) async {
        if !running { return }
        var frame = envelope("typing")
        frame["active"] = .bool(active)
        await sendToAll(frame)
    }

    private func rememberPeer(_ peerId: String, _ plain: JSONObject, linked: Bool) {
        if peerId.isEmpty || peerId == sealKeyId { return }
        let existing = peers[peerId]
        let peer = existing ?? MeshPeer()
        if existing == nil {
            peers[peerId] = peer
            peerIds.append(peerId)
        }
        func field(_ key: String) -> String {
            dartPrefix(dartString(plain[key]).dartTrimmed, meshMaxNameChars)
        }
        let senderName = field("sender")
        if !senderName.isEmpty { peer.name = senderName }
        let nodeName = field("nodeName")
        if !nodeName.isEmpty { peer.nodeName = nodeName }
        let platform = field("platform")
        if !platform.isEmpty { peer.platform = platform }
        let version = field("version")
        if !version.isEmpty { peer.version = version }
        let sender = dartPrefix(dartString(plain["senderId"]), 160)
        let namedChanged = !sender.isEmpty && sender != peer.nodeId
        if !sender.isEmpty { peer.nodeId = sender }
        if linked {
            peer.linked = true
            peer.unlinkedSinceMs = 0
        }
        peer.lastSeenMs = clock.nowMillis
        if existing == nil || linked || (namedChanged && heard.values.contains { $0.kid == peerId && $0.control }) {
            notifyListeners()
        }
    }

    @discardableResult
    private func removePeer(_ id: String) -> Bool {
        guard peers.removeValue(forKey: id) != nil else { return false }
        peerIds.removeAll { $0 == id }
        return true
    }

    private func sweepPeers() {
        let now = clock.nowMillis
        var changed = false
        for id in peerIds {
            guard let peer = peers[id] else { continue }
            let keep = linkForPeer(id) != nil || now - peer.lastSeenMs <= Int64(peerTimeoutMs)
            if !keep {
                removePeer(id)
                changed = true
            }
        }
        for id in heardOrder {
            guard let entry = heard[id], now - entry.lastSeenMs > Int64(peerTimeoutMs) else { continue }
            heard.removeValue(forKey: id)
            changed = changed || entry.control
        }
        heardOrder.removeAll { heard[$0] == nil }
        if changed { notifyListeners() }
    }

    private func expired(_ plain: JSONObject) -> Bool {
        guard let isExpired else { return false }
        return isExpired(meshConversation, toMessage(plain).timestamp)
    }

    @discardableResult
    public func pruneExpired() -> Bool {
        guard let isExpired else { return false }
        let keptHistory = history.filter { !expired($0) }
        let keptMessages = messages.filter { !isExpired(meshConversation, $0.timestamp) }
        let historyChanged = keptHistory.count != history.count
        if !historyChanged && keptMessages.count == messages.count { return false }
        if historyChanged {
            let live = Set(keptHistory.map { dartString($0["id"]) })
            queued = queued.filter { live.contains($0) }
            history = keptHistory
            scheduleHistorySave()
        }
        messages = keptMessages
        unread = min(unread, keptMessages.filter { !$0.isSelf }.count)
        notifyListeners()
        return true
    }

    private func storeHistory(_ plain: JSONObject) {
        if expired(plain) { return }
        history.append(plain)
        while history.count > historyLimit {
            queued.remove(dartString(history.removeFirst()["id"]))
        }
        scheduleHistorySave()
    }

    private func loadHistory() {
        if historyLoaded { return }
        historyLoaded = true
        historyFile = historyFileSource()
        guard let file = historyFile, FileManager.default.fileExists(atPath: file.path) else { return }
        guard let data = try? Data(contentsOf: file) else {
            historyFile = nil
            historyLoaded = false
            return
        }
        // An unreadable file is a room with no history, not a room that cannot open.
        guard case .object(let root)? = try? JSON.parse(data),
            let version = root["v"], version.numEquals(Int64(historyFileVersion))
        else { return }
        let queuedIds = Set((root["queued"]?.arrayValue ?? []).map { dartString($0) })
        for value in root["history"]?.arrayValue ?? [] {
            guard let entry = value.objectValue else { continue }
            let id = dartString(entry["id"])
            if id.isEmpty || entry["type"] != .string("chat") || entry["conversation"] != .string(meshConversation)
                || expired(entry) || !markSeen(id)
            {
                continue
            }
            history.append(entry)
            if queuedIds.contains(id) && entry["senderId"] == .string(nodeId) { queued.insert(id) }
            appendMessage(toMessage(entry), countUnread: false)
        }
        while history.count > historyLimit { history.removeFirst() }
    }

    private func scheduleHistorySave() {
        if historyFile == nil { return }
        historySaveTask?.cancel()
        historySaveTask = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(historySaveDelayMs) * 1_000_000)
            guard let self, !Task.isCancelled else { return }
            self.historySaveTask = nil
            self.saveHistory()
        }
    }

    private func saveHistory() {
        guard let file = historyFile else { return }
        let root: JSONObject = [
            "v": .int(Int64(historyFileVersion)),
            "history": .array(history.map { .object($0) }),
            "queued": JSONValue(Array(queued)),
        ]
        do {
            try FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
            #if os(iOS)
            try JSON.encodeData(.object(root)).write(to: file, options: [.atomic, .completeFileProtectionUnlessOpen])
            #else
            try JSON.encodeData(.object(root)).write(to: file, options: [.atomic])
            #endif
            ChatFiles.excludeFromBackup(file)
        } catch {}
    }

    private func markSeen(_ id: String) -> Bool {
        if seenIds.contains(id) { return false }
        seenIds.insert(id)
        seenOrder.append(id)
        while seenOrder.count > seenCacheLimit { seenIds.remove(seenOrder.removeFirst()) }
        return true
    }

    private func toMessage(_ plain: JSONObject) -> ChatMessage {
        let senderId = dartString(plain["senderId"])
        let ts: Date
        if let millis = plain["ts"]?.doubleValue {
            ts = Date(epochMillis: Int64(millis))
        } else {
            ts = clock.now
        }
        return ChatMessage(
            id: dartString(plain["id"]), conversation: meshConversation, senderId: senderId,
            senderName: dartString(plain["sender"]), text: dartString(plain["text"]), timestamp: ts,
            isSelf: senderId == nodeId)
    }

    @discardableResult
    private func appendMessage(_ message: ChatMessage, countUnread: Bool = true) -> Bool {
        if isExpired?(meshConversation, message.timestamp) == true { return false }
        if messages.contains(where: { $0.id == message.id }) { return false }
        var next = messages
        next.append(message)
        next.sort { RelayService.compareMessages($0, $1) < 0 }
        while next.count > historyLimit { next.removeFirst() }
        messages = next
        if !message.isSelf && countUnread { unread += 1 }
        return true
    }

    private static var seq = 0

    static func messageId(_ nodeId: String, _ clock: Clock) -> String {
        seq += 1
        let micros = Int64((clock.now.timeIntervalSince1970 * 1_000_000).rounded())
        return "\(nodeId)-\(micros)-\(seq)"
    }

    public func dispose() {
        beaconTask?.cancel()
        sweepTask?.cancel()
        sweepPaceTask?.cancel()
        if historySaveTask != nil {
            historySaveTask?.cancel()
            historySaveTask = nil
            saveHistory()
        }
        bluetoothReader?.cancel()
        discoveryReader?.cancel()
        discovery?.close()
        serverReader?.cancel()
        server?.close()
        for link in links {
            link.reader?.cancel()
            link.deadline?.cancel()
            link.socket?.close()
        }
        running = false
    }
}
