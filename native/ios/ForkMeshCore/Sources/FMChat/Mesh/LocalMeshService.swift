// The local mesh room.
//
// Chat with whoever is running ForkMesh in the same place, with no relay, no account, no sign-in
// and nothing sent to the internet. Byte-compatible with the desktop client's LocalMeshNode, so a
// phone and a laptop join the same room.
//
// Two transports carry it:
//  * The local network. An encrypted multicast beacon (TTL 1, so no router forwards it) advertises
//    this device, and peers link over direct TCP. Every peer address is checked against
//    `isLocalNetworkAddress` before a socket is opened or accepted.
//  * Bluetooth, through `MeshNativeTransport`.
//
// Encryption is the room's: RoomCrypto (AES-256-GCM, key derived from the room name) wraps every
// datagram and frame, and RoomSeal seals message bodies to the individual devices in the room.
//
// A BSD socket survives a refused send, so this sends on the discovery socket itself and keeps
// the refusal bookkeeping (what `refused` reports, EHOSTUNREACH as "Local Network is off").
// Darwin's EHOSTUNREACH is read from the platform (65 on iOS) rather than hard-coded.

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

/// Where the room's history lives between launches: Application Support.
public func defaultMeshHistoryFile() -> URL? {
    guard let directory = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first else {
        return nil
    }
    return directory.appendingPathComponent("local_mesh_history.json")
}

final class MeshLink {
    let socket: MeshTCPConnection?
    let bluetoothId: Int?
    let outbound: Bool
    let buffer = MeshByteBuffer()
    var pending: [JSONObject] = []
    var peerId = ""
    var helloMs: Int64 = 0
    var keyKnown = false
    var keyId = ""
    var reader: Task<Void, Never>?

    init(socket: MeshTCPConnection? = nil, bluetoothId: Int? = nil, outbound: Bool) {
        self.socket = socket
        self.bluetoothId = bluetoothId
        self.outbound = outbound
    }

    var bluetooth: Bool { bluetoothId != nil }
}

final class MeshPeer {
    var name = ""
    var nodeName = ""
    var ownerUser = ""
    var platform = ""
    var version = ""
    var address: MeshAddress?
    var port = 0
    var lastSeenMs: Int64 = 0
    // When a beacon first arrived from this peer with no link to it, or 0. The higher id dials
    // once this is a while ago: the lower id's own dial is evidently not getting through.
    var unlinkedSinceMs: Int64 = 0
    var linked = false
    var control = false
}

@MainActor
@Observable
public final class LocalMeshService {
    @ObservationIgnored let settings: SettingsService
    @ObservationIgnored let identity: DeviceIdentity
    @ObservationIgnored let secureStore: SecureStore
    @ObservationIgnored let native: MeshNativeTransport
    @ObservationIgnored let historyFileSource: () -> URL?
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
    @ObservationIgnored private var groupSendRefused = 0  // beats since multicast was refused, or 0
    @ObservationIgnored private var bluetoothReader: Task<Void, Never>?
    /// Transport counters for the debug bar. Not observed: the bar refreshes on its own timer.
    @ObservationIgnored public private(set) var diagnostics = MeshDiagnostics()
    @ObservationIgnored private var startedMs: Int64 = 0

    @ObservationIgnored private(set) var links: [MeshLink] = []
    @ObservationIgnored private var bluetoothHeldOff: [String: Int] = [:]
    @ObservationIgnored private var peerIds: [String] = []
    @ObservationIgnored private var peers: [String: MeshPeer] = [:]
    @ObservationIgnored private var history: [JSONObject] = []
    @ObservationIgnored private var seenIds = Set<String>()
    @ObservationIgnored private var seenOrder: [String] = []
    @ObservationIgnored private var dialing = Set<String>()
    // Ids of this device's messages no device has taken yet. They wait in the history, which
    // every device that joins is given.
    @ObservationIgnored private var queued = Set<String>()
    @ObservationIgnored private var historyFile: URL?
    @ObservationIgnored private var historyLoaded = false
    @ObservationIgnored private var historySaveTask: Task<Void, Never>?
    @ObservationIgnored private var beaconCount = 0
    @ObservationIgnored public var isExpired: ChatExpiry?
    @ObservationIgnored public var wakeTokenProvider: (@MainActor () async -> String)?
    @ObservationIgnored public var wakeSender: (@MainActor ([String]) async -> Void)?
    @ObservationIgnored public var onIncoming: (@MainActor (ChatMessage, Int) -> Void)?
    @ObservationIgnored public var onDirect: (@MainActor (String, String, JSONObject) -> Void)?
    @ObservationIgnored private var ownWakeToken = ""
    @ObservationIgnored private var wakeRefreshing = false
    @ObservationIgnored private var wakeTokens: [String: String] = [:]
    @ObservationIgnored private var wakeOrder: [String] = []

    public private(set) var messages: [ChatMessage] = []
    public private(set) var running = false
    public private(set) var networkActive = false
    public private(set) var unread = 0
    /// True once a whole subnet sweep was refused with "no route to host" and none of it went out:
    /// on iOS that is Local Network access being off for this app.
    public private(set) var localNetworkBlocked = false
    /// Bumped by every change the room's screens care about.
    public private(set) var revision = 0

    // Knobs, not settings: a test drives them to isolate one transport; production leaves them.
    @ObservationIgnored public var discoveryPort = meshDiscoveryPort
    @ObservationIgnored public var beaconIntervalMs = beaconIntervalDefaultMs
    @ObservationIgnored public var peerTimeoutMs = peerTimeoutDefaultMs
    @ObservationIgnored public var multicastEnabled = true
    // iOS withholds multicast from apps without Apple's multicast entitlement, so there the subnet
    // sweep is how this device is found at all.
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

    /// `historyFile` nil keeps no history on disk (a unit test).
    public init(
        settings: SettingsService,
        identity: DeviceIdentity,
        secureStore: SecureStore,
        transport: MeshNativeTransport? = nil,
        historyFile: (() -> URL?)? = nil,
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

    /// True while a message this device wrote has not reached any device yet.
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

    // MARK: Identity and roster

    private var nodeId: String { identity.nodeId }

    @ObservationIgnored public var accountProvider: (@MainActor () -> String)?

    public var persona: ChatPersona {
        ChatPersona.resolve(
            nodeKey: identity.publicKeyB64url, account: accountProvider?() ?? "", displayName: settings.displayName)
    }

    private var name: String { dartPrefix(persona.name, meshMaxNameChars) }

    /// The name this device goes by in the room.
    public var displayName: String { name }

    /// Desktops on this network offering a local control channel, with where to reach them.
    public func controlHosts() -> [MeshControlHost] {
        var hosts: [MeshControlHost] = []
        for id in peerIds {
            guard let peer = peers[id], peer.control, let address = peer.address, peer.port > 0 else { continue }
            hosts.append(
                MeshControlHost(
                    nodeId: id, address: address, port: peer.port, name: peer.name, nodeName: peer.nodeName,
                    ownerUser: peer.ownerUser, platform: peer.platform))
        }
        return hosts
    }

    /// Self first, then every device currently in the room.
    public func members() -> [Member] {
        let now = clock.nowMillis
        var list = [
            Member(
                id: nodeId, name: name, isSelf: true, online: running, platform: platformName,
                owner: persona.ownerUser)
        ]
        for id in peerIds {
            guard let peer = peers[id] else { continue }
            list.append(
                Member(
                    id: id, name: peer.name.isEmpty ? (peer.ownerUser.isEmpty ? id : peer.ownerUser) : peer.name,
                    online: peer.linked || now - peer.lastSeenMs <= Int64(peerTimeoutMs), platform: peer.platform,
                    version: peer.version, owner: peer.ownerUser, nodeName: peer.nodeName))
        }
        return list
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

    // MARK: Start and stop

    /// Opens whichever transports this device has. False only when neither the network nor
    /// Bluetooth can carry the room.
    @discardableResult
    public func start() async -> Bool {
        if running { return true }
        let store = secureStore
        let scope = "\(meshRoomName)\n\(nodeId)"
        let keys: (RoomCrypto, RoomSeal)
        do {
            keys = try await Task.detached(priority: .userInitiated) {
                // Scoped to this device's own id as well as the room, matching the desktop, so
                // the sealing identity belongs to the device.
                (RoomCrypto.shared(meshRoomName), try RoomSeal.loadOrCreate(meshRoomName, storageScope: scope, secureStore: store))
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
        refreshWakeToken()
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
    }

    public func resume() async {
        if !running {
            _ = await start()
            return
        }
        refreshWakeToken()
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
        if !networkActive { networkActive = openListener() }
        if networkActive { await openDiscovery() }
        await announce()
        notifyListeners()
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
        guard let crypto, let server else { return nil }
        let me = persona
        let beacon = MeshBeacon(
            nodeId: nodeId, port: server.port, name: name, ownerUser: me.ownerUser, accountKind: me.accountKind,
            platform: platformName, version: RelayService.clientVersion, timestampMs: clock.nowMillis, reply: reply)
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
        // A beacon from outside this network is not a mesh peer, whatever it claims.
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
        if beacon.nodeId == nodeId {
            diagnostics.ownBeaconsIn += 1
            return
        }
        diagnostics.peerBeaconsIn += 1
        diagnostics.lastBeaconMs = clock.nowMillis
        let who = beacon.name.isEmpty ? dartPrefix(beacon.nodeId, 8) : beacon.name
        diagnostics.lastBeaconFrom = "\(who) \(datagram.address.address):\(beacon.port)\(beacon.reply ? " (reply)" : "")"

        rememberPeer(
            beacon.nodeId,
            [
                "sender": .string(beacon.name), "nodeName": .string(beacon.nodeName),
                "ownerUser": .string(beacon.ownerUser), "platform": .string(beacon.platform),
                "version": .string(beacon.version), "control": .bool(beacon.control),
            ], linked: false)
        guard let peer = peers[beacon.nodeId] else { return }
        let moved = peer.address?.address != datagram.address.address || peer.port != beacon.port
        peer.address = datagram.address
        peer.port = beacon.port
        // Where a desktop's control channel is reached changed: tell whoever drives it.
        if moved && peer.control { notifyListeners() }
        if linkForPeer(beacon.nodeId) != nil {
            peer.unlinkedSinceMs = 0
            return
        }
        // Answer straight back, so a sender that never hears this device's multicast still learns
        // where to reach it.
        if !beacon.reply, let answer = beaconDatagram(reply: true), discovery != nil {
            sendDatagram(answer, datagram.address, datagram.port, count: { diagnostics.unicastSent += 1 })
        }
        // Both sides beacon, so the lower id dials and the higher id listens; when the lower id's
        // dial never lands, the higher id dials too once the peer has stayed unlinked half a beat.
        let now = clock.nowMillis
        if peer.unlinkedSinceMs == 0 { peer.unlinkedSinceMs = now }
        if nodeId.utf16.lexicographicallyPrecedes(beacon.nodeId.utf16)
            || now - peer.unlinkedSinceMs >= Int64(beaconIntervalMs / 2)
        {
            let address = datagram.address
            let port = beacon.port
            Task { await self.connectToPeer(address, port: port) }
        }
    }

    /// Direct link to a peer this device already knows how to reach, for a network that filters
    /// multicast. The address must be a local one.
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
            // The key frame goes out before anything is read, so nothing can be written to the
            // link ahead of it.
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

    // MARK: Bluetooth transport

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

    // MARK: Shared link handling

    private func openLink(_ link: MeshLink) async {
        if let keyFrame = keyFrameBytes() { _ = await write(link, keyFrame) }
        // The hello cannot be sealed to a device whose key has not arrived yet, so it waits in the
        // link's queue. Ordering is preserved, so the peer still reads hello first.
        var hello = envelope("hello")
        if let server { hello["port"] = .int(Int64(server.port)) }
        link.pending.append(hello)
    }

    private func onLinkData(_ link: MeshLink, _ data: Data) async {
        link.buffer.add(data)
        while true {
            let payload: Data?
            do {
                payload = try meshTakeFrame(link.buffer)
            } catch {
                await closeLink(link, reason: "oversized frame")
                return
            }
            guard let payload else { return }
            await handleFrame(link, payload)
            if !links.contains(where: { $0 === link }) { return }  // the frame closed this link
        }
    }

    private func handleFrame(_ link: MeshLink, _ payload: Data) async {
        guard let crypto, let seal else { return }
        diagnostics.framesIn += 1
        guard case .object(let envelope)? = try? JSON.parse(payload), let decrypted = crypto.decrypt(envelope) else {
            diagnostics.framesBad += 1
            return
        }
        if RoomSeal.isKeyFrame(decrypted) {
            let keyId = dartString(decrypted["kid"])
            if seal.acceptKeyFrame(decrypted) {
                // Re-advertise so a peer that learned our key late can still open what we send.
                if let keyFrame = keyFrameBytes() { _ = await write(link, keyFrame) }
            }
            if keyId == seal.keyId || seal.peerKeyIds.contains(keyId) { link.keyId = keyId }
            if !link.keyKnown {
                link.keyKnown = true
                let queuedFrames = link.pending
                link.pending.removeAll()
                for frame in queuedFrames { _ = await writePlain(link, frame) }
            }
            return
        }
        guard let plain = seal.unseal(decrypted) else {
            diagnostics.framesBad += 1
            return
        }
        if dartString(plain["type"]) == "direct" {
            handleDirect(plain, sealKeyId: RoomSeal.frameKeyId(decrypted), ownKeyId: seal.keyId)
            return
        }
        await handlePlain(link, plain)
    }

    private func handleDirect(_ plain: JSONObject, sealKeyId: String, ownKeyId: String) {
        let channel = dartString(plain["channel"])
        if sealKeyId.isEmpty || sealKeyId == ownKeyId { return }
        if !channel.hasPrefix(DirectMessageFrames.prefix) || channel.count > 80 { return }
        onDirect?(channel, sealKeyId, plain)
    }

    public func sendDirect(_ plain: JSONObject, to keyIds: [String]) async -> Int {
        guard let seal else { return 0 }
        let channel = dartString(plain["channel"])
        if !running || !channel.hasPrefix(DirectMessageFrames.prefix) || dartString(plain["id"]).isEmpty || keyIds.isEmpty {
            return 0
        }
        var frame = plain
        frame["type"] = .string("direct")
        frame["conversation"] = .string(meshConversation)
        var reached = Set<String>()
        for link in links {
            let keyId = link.keyId
            if !link.keyKnown || keyId.isEmpty || keyId == seal.keyId || !keyIds.contains(keyId) || reached.contains(keyId) { continue }
            if await writePlain(link, frame, onlyKeyIds: [keyId]) { reached.insert(keyId) }
        }
        return reached.count
    }

    private func handlePlain(_ link: MeshLink, _ plain: JSONObject) async {
        let type = dartString(plain["type"])
        let senderId = dartString(plain["senderId"])
        if senderId.isEmpty || senderId == nodeId { return }
        if dartString(plain["conversation"]) != meshConversation { return }

        // A link's identity is fixed by its hello. Chat can still arrive over it authored by a
        // third device (that is what makes the room reach devices that never linked directly), so
        // only the link's own frames are held to the link's identity.
        if type == "hello" {
            if link.peerId.isEmpty {
                let other = linkForPeer(senderId)
                link.peerId = senderId
                link.helloMs = clock.nowMillis
                if let other {
                    if link.bluetooth != other.bluetooth {
                        let loser = link.bluetooth ? link : other
                        await closeLink(loser, reason: "duplicate link")
                        if loser === link { return }
                    } else if link.helloMs - other.helloMs >= Int64(linkReplaceAfterMs) {
                        await closeLink(other, reason: "replaced")
                    } else {
                        // Both devices dialled at once. Keep the link the higher node id opened: both
                        // sides reach the same verdict, so one survives.
                        let mine = link.outbound ? nodeId : senderId
                        let theirs = other.outbound ? nodeId : senderId
                        if mine.utf16.lexicographicallyPrecedes(theirs.utf16) {
                            await closeLink(link, reason: "duplicate link")
                            return
                        }
                        await closeLink(other, reason: "duplicate link")
                    }
                }
            } else if link.peerId != senderId {
                await closeLink(link, reason: "identity changed")
                return
            }
        } else if (type == "typing" || type == "bye") && link.peerId != senderId {
            return
        }

        rememberPeer(senderId, plain, linked: link.peerId == senderId)
        rememberWake(senderId, plain)
        if type == "hello" && link.peerId == senderId, let peer = peers[senderId], let socket = link.socket {
            // A peer that dialled this phone may never have been heard beaconing; its hello says
            // where it listens.
            if case .int(let port)? = plain["port"], port > 0 && port <= 65535 {
                let moved = peer.address != socket.remoteAddress || peer.port != Int(port)
                peer.address = socket.remoteAddress
                peer.port = Int(port)
                if moved && peer.control { notifyListeners() }
            } else if peer.address == nil {
                peer.address = socket.remoteAddress
            }
        }
        if type == "typing" { return }

        let id = dartString(plain["id"])
        if id.isEmpty || !markSeen(id) { return }

        if type == "hello" {
            // The joiner gets this device's recent room history, the only backfill a relay-less
            // room has.
            for entry in history { _ = await writePlain(link, entry) }
            return
        }
        if type == "bye" {
            if removePeer(senderId) { notifyListeners() }
            return
        }
        if type != "chat" { return }
        diagnostics.chatIn += 1
        storeHistory(plain)
        let message: ChatMessage = toMessage(plain)
        let added: Bool = appendMessage(message)
        // A mesh link is point to point, so relay what a peer said to the peers it may not be
        // able to see. The id cache above stops it going round.
        for other in links where other !== link && other.keyKnown {
            _ = await writePlain(other, plain)
        }
        notifyListeners()
        if added && !message.isSelf { onIncoming?(message, unread) }
    }

    /// The public half of this device's sealing key: the one frame that cannot itself be sealed;
    /// the room layer still encrypts it.
    private func keyFrameBytes() -> [UInt8]? {
        guard let crypto, let seal, let envelope = try? crypto.encrypt(seal.keyFrame()) else { return nil }
        return Array(JSON.encode(.object(envelope)).utf8)
    }

    private func writePlain(_ link: MeshLink, _ plain: JSONObject, onlyKeyIds: [String]? = nil) async -> Bool {
        guard let crypto, let seal else { return false }
        // Sealed at send time, not at compose time, so a device that joined after a message was
        // written is still a recipient of the replayed copy.
        let sealedFrame: JSONObject?
        if let onlyKeyIds {
            sealedFrame = try? seal.seal(plain, for: onlyKeyIds)
        } else {
            sealedFrame = try? seal.seal(plain)
        }
        guard let sealed = sealedFrame, let envelope = try? crypto.encrypt(sealed) else { return false }
        let written = await write(link, Array(JSON.encode(.object(envelope)).utf8))
        // Handed to a device: from here it travels on with that device's history.
        if written && queued.remove(dartString(plain["id"])) != nil {
            scheduleHistorySave()
            notifyListeners()
        }
        return written
    }

    private func write(_ link: MeshLink, _ payload: [UInt8]) async -> Bool {
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
        for link in links {
            if !link.keyKnown {
                // Hold it until the peer's sealing key lands, rather than send a frame that device
                // could never open.
                if link.pending.count < 32 { link.pending.append(plain) }
                continue
            }
            _ = await writePlain(link, plain)
        }
    }

    private func closeLink(_ link: MeshLink, notify: Bool = true, reason: String = "") async {
        let present = links.contains { $0 === link }
        if present {
            let who = link.peerId.isEmpty ? "unnamed link" : dartPrefix(link.peerId, 8)
            diagnostics.lastDropReason = "\(who): \(reason.isEmpty ? "closed" : reason)"
        }
        links.removeAll { $0 === link }
        link.socket?.close()
        let duplicate = reason == "duplicate link"
        let peerId = link.peerId
        if let id = link.bluetoothId {
            await native.closeBluetoothLink(id, duplicate: duplicate)
            if present && duplicate && !peerId.isEmpty { bluetoothHeldOff[peerId] = id }
        }
        guard !peerId.isEmpty, linkForPeer(peerId) == nil, let peer = peers[peerId] else { return }
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

    // MARK: Room state

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
        persona.stamp(&frame)
        if type == "hello" || type == "chat" || type == "wake", meshWakeTokenUsable(ownWakeToken, now: clock.nowMillis) {
            frame["wake"] = .string(ownWakeToken)
        }
        return frame
    }

    public func refreshWakeToken() {
        guard !wakeRefreshing, let provider = wakeTokenProvider else { return }
        if let expires = meshWakeTokenExpiry(ownWakeToken), expires - clock.nowMillis > meshWakeRefreshBeforeMs { return }
        wakeRefreshing = true
        Task { [weak self] in
            let token = await provider()
            guard let self else { return }
            self.wakeRefreshing = false
            guard meshWakeTokenUsable(token, now: self.clock.nowMillis), token != self.ownWakeToken else { return }
            self.ownWakeToken = token
            if self.running && !self.links.isEmpty { await self.sendToAll(self.envelope("wake")) }
        }
    }

    func wakeTargets() -> [String] {
        let now = clock.nowMillis
        let tokens = wakeOrder.reversed().compactMap { wakeTokens[$0] }.filter {
            $0 != ownWakeToken && meshWakeTokenUsable($0, now: now)
        }
        return Array(tokens.prefix(meshWakeMaxTokens))
    }

    private func rememberWake(_ senderId: String, _ plain: JSONObject) {
        if senderId.isEmpty || senderId == nodeId { return }
        let token = dartString(plain["wake"])
        guard let expires = meshWakeTokenExpiry(token), expires > clock.nowMillis else { return }
        if let known = wakeTokens[senderId], (meshWakeTokenExpiry(known) ?? 0) >= expires { return }
        wakeTokens[senderId] = token
        wakeOrder.removeAll { $0 == senderId }
        wakeOrder.append(senderId)
        while wakeOrder.count > meshWakeMaxTokens { wakeTokens.removeValue(forKey: wakeOrder.removeFirst()) }
    }

    private func wakePeers() {
        guard let wakeSender else { return }
        let tokens = wakeTargets()
        if tokens.isEmpty { return }
        Task { await wakeSender(tokens) }
    }

    /// Returns the sent message's id, or "" when nothing was sent.
    @discardableResult
    public func sendMessage(_ text: String) async -> String {
        let trimmed = text.dartTrimmed
        if !running || trimmed.isEmpty { return "" }
        var message = envelope("chat")
        message["text"] = .string(dartPrefix(trimmed, meshMaxTextChars))
        let id = dartString(message["id"])
        _ = markSeen(id)
        storeHistory(message)
        // Queued until a write to some device clears it: now, when a linked device's key lands, or
        // when a device joins and gets the history.
        queued.insert(id)
        diagnostics.chatOut += 1
        await sendToAll(message)
        wakePeers()
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
        if peerId.isEmpty || peerId == nodeId { return }
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
        let owner = field("ownerUser")
        if !owner.isEmpty { peer.ownerUser = owner }
        let platform = field("platform")
        if !platform.isEmpty { peer.platform = platform }
        let version = field("version")
        if !version.isEmpty { peer.version = version }
        var controlChanged = false
        if plain.contains("control") {
            let control = plain["control"] == .bool(true)
            controlChanged = control != peer.control
            peer.control = control
        }
        if linked {
            peer.linked = true
            peer.unlinkedSinceMs = 0
        }
        peer.lastSeenMs = clock.nowMillis
        if existing == nil || linked || controlChanged { notifyListeners() }
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
        guard let file = historyFile, let data = try? Data(contentsOf: file) else { return }
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
            rememberWake(dartString(entry["senderId"]), entry)
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
            let temp = file.appendingPathExtension("tmp")
            try JSON.encodeData(.object(root)).write(to: temp)
            _ = rename(temp.path, file.path)
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

    /// Closes every socket without the goodbye `stop` sends.
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
            link.socket?.close()
        }
        running = false
    }
}
