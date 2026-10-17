// New for the native app (WS7): which channel reaches the owner's desktop right now. The LAN
// channel when LocalControlService holds one (it pairs, reconnects and carries files); otherwise
// the same protocol through the relay, to a desktop this phone paired with on the LAN before.
//
// Features ask for `link()` and subscribe to `addEventTap`; the hub re-attaches their taps when the
// channel underneath changes, so a Hosts screen keeps its bells across a walk out of the house.

import FMAccount
import FMApi
import FMBase
import FMChat
import FMCrypto
import Foundation
import Observation

/// Where the hub stands, for a status line.
public enum DesktopReach: Equatable, Sendable {
    /// No desktop has ever been paired with this phone.
    case notPaired
    case offline(String)
    case connecting(String)
    case connected(ControlTransport, String)

    public var isConnected: Bool {
        if case .connected = self { return true }
        return false
    }

    public var label: String {
        switch self {
        case .notPaired: return "Sign in to ForkMesh on your desktop with this account"
        case .offline(let reason): return reason.isEmpty ? "Desktop not reachable" : reason
        case .connecting(let name): return "Connecting to \(name)…"
        case .connected(let transport, let name):
            return transport == .lan ? "\(name) · on this network" : "\(name) · through the relay"
        }
    }
}

/// Opens a relay channel. The app passes one that makes a fresh `AccountEventChannel`; tests pass
/// an in-memory relay.
public typealias RelayControlOpener = @MainActor (PairedDesktop) async throws -> RelayControlConnection

public typealias AccountDesktopsLoader = @MainActor () async throws -> [PairedDesktop]

@MainActor
@Observable
public final class DesktopControlHub {
    @ObservationIgnored public let local: LocalControlService?
    @ObservationIgnored private let openRelay: RelayControlOpener?
    @ObservationIgnored private let pairedProvider: @MainActor () -> [PairedDesktop]
    @ObservationIgnored private let accountDesktopsLoader: AccountDesktopsLoader?
    @ObservationIgnored public private(set) var accountDesktops: [PairedDesktop] = []
    @ObservationIgnored private var accountDesktopsLoadedAt: Date?
    @ObservationIgnored private var accountDesktopsLoad: (epoch: Int, task: Task<Void, Never>)?
    @ObservationIgnored private var lastReachedKey = ""
    @ObservationIgnored private var epoch = 0

    public private(set) var reach: DesktopReach = .notPaired
    /// Bumped whenever the channel underneath changes.
    public private(set) var revision = 0

    @ObservationIgnored private var relay: RelayControlConnection?
    @ObservationIgnored private var lanLink: LanControlLink?
    @ObservationIgnored private var opening = false
    @ObservationIgnored private var openingWaiters: [CheckedContinuation<DesktopControlLink, Error>] = []
    @ObservationIgnored private var taps: [UUID: @MainActor (String, JSONObject) -> Void] = [:]
    @ObservationIgnored private var attached: (link: AnyObject, token: UUID)?
    @ObservationIgnored private var localToken: ListenerToken?
    @ObservationIgnored private var linkListeners: [UUID: @MainActor () -> Void] = [:]
    @ObservationIgnored private var announcedLink: ObjectIdentifier?
    @ObservationIgnored private var warmRetry: Task<Void, Never>?
    @ObservationIgnored private var warmDelay: TimeInterval = 2
    @ObservationIgnored public var relayIdleProbe: TimeInterval = 45

    public init(
        local: LocalControlService?,
        openRelay: RelayControlOpener?,
        paired: (@MainActor () -> [PairedDesktop])? = nil,
        accountDesktops: AccountDesktopsLoader? = nil
    ) {
        self.local = local
        self.openRelay = openRelay
        self.pairedProvider = paired ?? { local?.paired ?? [] }
        self.accountDesktopsLoader = accountDesktops
        if let local {
            lanLink = LanControlLink(service: local)
            localToken = local.addListener { [weak self] in
                MainActor.assumeIsolated { self?.localChanged() }
            }
        }
        refreshReach()
    }

    /// The desktop this hub reaches: the connected one on the LAN, else the most recently paired.
    public var desktop: PairedDesktop? { candidates.first }

    public var candidates: [PairedDesktop] {
        let paired = pairedProvider().sorted(by: { $0.pairedAtMs > $1.pairedAtMs })
        var out: [PairedDesktop] = []
        if let local, local.connected, let match = paired.first(where: { $0.key == local.desktopKey }) {
            out.append(match)
        }
        for item in paired + accountDesktops {
            if let index = out.firstIndex(where: { $0.key == item.key }) {
                if out[index].name.isEmpty { out[index] = item }
            } else {
                out.append(item)
            }
        }
        if let index = out.firstIndex(where: { $0.key == lastReachedKey }), index > 0 {
            out.insert(out.remove(at: index), at: 0)
        }
        return out
    }

    @discardableResult
    public func refreshAccountDesktops(force: Bool = false) async -> [PairedDesktop] {
        guard let accountDesktopsLoader else { return accountDesktops }
        if !force, let loaded = accountDesktopsLoadedAt, Date().timeIntervalSince(loaded) < 600 {
            return accountDesktops
        }
        let started = epoch
        if !force, let pending = accountDesktopsLoad, pending.epoch == started {
            await pending.task.value
            return accountDesktops
        }
        let task = Task { await self.loadAccountDesktops(accountDesktopsLoader, epoch: started) }
        accountDesktopsLoad = (started, task)
        await task.value
        if accountDesktopsLoad?.task == task { accountDesktopsLoad = nil }
        return accountDesktops
    }

    private func loadAccountDesktops(_ loader: AccountDesktopsLoader, epoch started: Int) async {
        guard let loaded = try? await loader(), started == epoch else { return }
        accountDesktops = loaded
        accountDesktopsLoadedAt = Date()
        local?.setAccountDesktops(Set(loaded.map(\.key)))
        let before = reach
        refreshReach()
        if before != reach { revision &+= 1 }
    }

    /// The open channel, if any, without dialling.
    public var currentLink: DesktopControlLink? {
        if let lanLink, lanLink.isOpen { return lanLink }
        if let relay, relay.isOpen { return relay }
        return nil
    }

    /// An open channel: the LAN when it is up, else the relay (opened on demand).
    public func link() async throws -> DesktopControlLink {
        if let current = currentLink {
            if let relayed = current as? RelayControlConnection, relayed.idleSeconds >= relayIdleProbe {
                if await relayed.confirmAlive() {
                    attachTaps(to: relayed)
                    return relayed
                }
                if let replacement = currentLink {
                    attachTaps(to: replacement)
                    return replacement
                }
            } else {
                attachTaps(to: current)
                return current
            }
        }
        if opening {
            // Someone is already dialling; share their channel.
            return try await withCheckedThrowingContinuation { continuation in
                openingWaiters.append(continuation)
            }
        }
        guard let openRelay else { throw LocalControlException("not_connected") }
        opening = true
        defer {
            opening = false
        }
        let started = epoch
        await refreshAccountDesktops()
        let targets = Array(candidates.filter { !$0.name.isEmpty }.prefix(4))
        guard let first = targets.first else {
            refreshReach()
            let error = LocalControlException("not_paired")
            let waiting = openingWaiters
            openingWaiters.removeAll()
            for waiter in waiting { waiter.resume(throwing: error) }
            throw error
        }
        reach = .connecting(first.name)
        do {
            let (connection, target) = try await DesktopControlHub.race(targets, openRelay)
            if started != epoch {
                await connection.close()
                return try await relink()
            }
            lastReachedKey = target.key
            relay = connection
            connection.onClosed = { [weak self] reason in
                guard let self else { return }
                if self.relay === connection { self.relay = nil }
                self.reach = .offline(ControlErrorText.describe(code: reason))
                self.revision &+= 1
                self.announceLink()
                if self.keepWarm { self.scheduleWarm() }
            }
            attachTaps(to: connection)
            refreshReach()
            revision &+= 1
            announceLink()
            let waiting = openingWaiters
            openingWaiters.removeAll()
            for waiter in waiting { waiter.resume(returning: connection) }
            return connection
        } catch {
            if started != epoch { return try await relink() }
            reach = .offline(ControlErrorText.isCancellation(error) ? "" : ControlErrorText.describe(error))
            let waiting = openingWaiters
            openingWaiters.removeAll()
            for waiter in waiting { waiter.resume(throwing: error) }
            throw error
        }
    }

    private func relink() async throws -> DesktopControlLink {
        opening = false
        let waiting = openingWaiters
        openingWaiters.removeAll()
        do {
            let fresh = try await link()
            for waiter in waiting { waiter.resume(returning: fresh) }
            return fresh
        } catch {
            for waiter in waiting { waiter.resume(throwing: error) }
            throw error
        }
    }

    @ObservationIgnored public var keepWarm = false {
        didSet {
            if keepWarm {
                warm()
            } else {
                warmRetry?.cancel()
                warmRetry = nil
            }
        }
    }

    public func warm() {
        if currentLink != nil || opening || openRelay == nil { return }
        Task {
            do {
                _ = try await self.link()
                self.warmDelay = 2
            } catch let refusal as LocalControlException where refusal.code == "not_paired" {
                return
            } catch {
                if self.keepWarm { self.scheduleWarm() }
            }
        }
    }

    private func scheduleWarm() {
        if warmRetry != nil { return }
        let delay = warmDelay
        warmDelay = min(warmDelay * 2, 60)
        warmRetry = Task { [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(delay * 1_000_000_000))
            guard let self, !Task.isCancelled else { return }
            self.warmRetry = nil
            if self.keepWarm { self.warm() }
        }
    }

    @discardableResult
    public func addLinkListener(_ listener: @escaping @MainActor () -> Void) -> UUID {
        let id = UUID()
        linkListeners[id] = listener
        return id
    }

    public func removeLinkListener(_ id: UUID) { linkListeners.removeValue(forKey: id) }

    private func announceLink() {
        let now = currentLink.map { ObjectIdentifier($0) }
        if now == announcedLink { return }
        announcedLink = now
        for listener in linkListeners.values { listener() }
    }

    private final class Race {
        var done = false
        var remaining: Int
        var lastError: Error = LocalControlException("not_paired")
        init(_ count: Int) { remaining = count }
    }

    private static func race(_ targets: [PairedDesktop], _ open: @escaping RelayControlOpener) async throws
        -> (RelayControlConnection, PairedDesktop)
    {
        let state = Race(targets.count)
        return try await withCheckedThrowingContinuation { continuation in
            for target in targets {
                Task { @MainActor in
                    do {
                        let connection = try await open(target)
                        if state.done {
                            await connection.close()
                            return
                        }
                        state.done = true
                        continuation.resume(returning: (connection, target))
                    } catch {
                        state.lastError = error
                        state.remaining -= 1
                        if state.remaining == 0 && !state.done {
                            state.done = true
                            continuation.resume(throwing: state.lastError)
                        }
                    }
                }
            }
        }
    }

    /// One request on whichever channel is up, dialling the relay when none is.
    public func request(_ method: String, _ params: JSONObject = [:], timeout: TimeInterval = 20) async throws
        -> JSONObject
    {
        let link = try await link()
        return try await link.request(method, params, timeout: timeout)
    }

    public var transport: ControlTransport? { currentLink?.transport }

    /// Events from the desktop, across channel changes.
    @discardableResult
    public func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let id = UUID()
        taps[id] = tap
        if let current = currentLink { attachTaps(to: current) }
        return id
    }

    public func removeEventTap(_ id: UUID) { taps.removeValue(forKey: id) }

    public func resetForProjectSwitch() {
        epoch &+= 1
        accountDesktops = []
        accountDesktopsLoadedAt = nil
        lastReachedKey = ""
        warmRetry?.cancel()
        warmRetry = nil
        warmDelay = 2
        if let current = relay {
            relay = nil
            current.onClosed = nil
            Task { await current.close() }
        }
        reach = .notPaired
        refreshReach()
        revision &+= 1
        announceLink()
    }

    /// Drops the relay channel (the LAN one belongs to LocalControlService).
    public func disconnectRelay() async {
        let current = relay
        relay = nil
        await current?.close()
        refreshReach()
        announceLink()
    }

    private func attachTaps(to link: DesktopControlLink) {
        if let attached, attached.link === link { return }
        if let attached, let old = attached.link as? DesktopControlLink { old.removeEventTap(attached.token) }
        let token = link.addEventTap { [weak self] name, data in
            guard let self else { return }
            for tap in self.taps.values { tap(name, data) }
        }
        attached = (link, token)
    }

    private func localChanged() {
        let before = reach
        refreshReach()
        // The phone came home: prefer the LAN and let the relay channel go.
        if let lanLink, lanLink.isOpen {
            attachTaps(to: lanLink)
            if relay != nil {
                Task { await self.disconnectRelay() }
            }
        }
        if before != reach { revision &+= 1 }
        announceLink()
    }

    private func refreshReach() {
        if let lanLink, lanLink.isOpen {
            reach = .connected(.lan, lanLink.desktopName)
        } else if let relay, relay.isOpen {
            reach = .connected(.relay, relay.desktopName.isEmpty ? relay.node : relay.desktopName)
        } else if pairedProvider().isEmpty && accountDesktops.isEmpty {
            reach = .notPaired
        } else if case .connecting = reach {
            // keep
        } else if case .offline = reach {
            // keep the reason
        } else {
            reach = .offline("")
        }
    }
}

extension DesktopControlHub: ShellLocalDesktopBridge {
    public var localDesktop: ShellLocalDesktop {
        let lan: ShellLocalDesktop = local?.localDesktop ?? .hidden
        if case .connected(.relay, let name) = reach, !lan.isConnected { return .relay(name) }
        return lan
    }
}
