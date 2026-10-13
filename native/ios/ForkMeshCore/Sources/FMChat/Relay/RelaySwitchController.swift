// Moves the running app from one relay to another, in order.
//
// A relay switch is a change of world. The socket, the REST caches, the account session, the
// node-sync drain, the account event channel and push registration all hold relay state, and
// flipping the URL under them is what put one relay's repositories under another's name. So the
// order is fixed and every step is awaited: take the old relay's sockets down, drop everything
// learned from it, adopt the new relay's credentials, then bring the new relay's sockets up. A
// generation counter makes a second switch started mid-flight abandon the first.

import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

/// Where a relay switch has got to. The overlay shows one line per phase, so a stalled switch
/// says which step is slow.
public enum RelaySwitchPhase: String, Sendable {
    case idle
    /// Old relay's sockets coming down.
    case leaving
    /// Cached answers from the old relay being dropped.
    case clearing
    /// Credentials for the new relay being loaded.
    case adopting
    /// New relay's sockets coming up.
    case joining
    case done
    case failed
}

/// How a switch ended.
public struct RelaySwitchResult: Equatable, Sendable {
    public let ok: Bool
    public let relay: RelayProfile?
    /// The app is now on `relay` but holds no session there. The shell already swaps to the auth
    /// flow; the relay list uses this to say so in place.
    public let needsSignIn: Bool
    /// The switch completed but the relay's socket did not come up. A banner, not a rollback.
    public let offline: Bool
    public let error: String

    public init(ok: Bool, relay: RelayProfile?, needsSignIn: Bool = false, offline: Bool = false, error: String = "") {
        self.ok = ok
        self.relay = relay
        self.needsSignIn = needsSignIn
        self.offline = offline
        self.error = error
    }
}

/// The relay socket as the controller drives it. `RelayService` conforms.
@MainActor
public protocol RelaySwitchSocket: AnyObject {
    var state: RelayConnectionState { get }
    func connect() async throws
    func disconnect() async throws
    func resetForRelaySwitch()
}

/// What caches answers from one relay. `ApiService` conforms.
@MainActor
public protocol RelaySwitchCache: AnyObject {
    func resetForRelaySwitch()
}

extension RelayService: RelaySwitchSocket {}
extension ApiService: RelaySwitchCache {}

@MainActor
@Observable
public final class RelaySwitchController {
    @ObservationIgnored let settings: SettingsService
    @ObservationIgnored let auth: AuthService
    @ObservationIgnored let api: RelaySwitchCache
    @ObservationIgnored let relay: RelaySwitchSocket
    @ObservationIgnored let joinTimeout: TimeInterval
    /// A floor on how long a switch takes, so the overlay's transition always plays: a local relay
    /// finishes in ~30 ms, and a scrim that appears and vanishes inside two frames reads as a
    /// glitch.
    @ObservationIgnored let minimumDuration: TimeInterval
    @ObservationIgnored let clock: Clock
    @ObservationIgnored let sleeper: @Sendable (TimeInterval) async throws -> Void

    /// Stops the per-account machinery the shell owns (node-sync drain, account event socket,
    /// push rebinding) before the relay moves, and starts it again after. Settable, because the
    /// shell builds that machinery after the chat services.
    @ObservationIgnored public var onSuspend: (@MainActor () async throws -> Void)?
    @ObservationIgnored public var onResume: (@MainActor () async throws -> Void)?
    /// Moves storage and the per-project services (node key, owner key, secrets) to the project
    /// being switched to, and erases a removed project's own data. Nil means the one the app
    /// registered in `Services` (`ProjectActivator`), if any; tests leave both empty.
    @ObservationIgnored public var projectActivator: ProjectActivator?
    @ObservationIgnored public let listeners = Listeners()

    private var activator: ProjectActivator? { projectActivator ?? Services.get(ProjectActivator.self) }

    public private(set) var phase: RelaySwitchPhase = .idle
    /// The relay being moved to while a switch is running.
    public private(set) var target: RelayProfile?
    public private(set) var error = ""

    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var resuming: Task<Void, Error>?
    @ObservationIgnored private var fromLabel = ""

    public init(
        settings: SettingsService,
        auth: AuthService,
        api: RelaySwitchCache,
        relay: RelaySwitchSocket,
        onSuspend: (@MainActor () async throws -> Void)? = nil,
        onResume: (@MainActor () async throws -> Void)? = nil,
        joinTimeout: TimeInterval = 12,
        minimumDuration: TimeInterval = 0.2,
        clock: Clock = SystemClock(),
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.settings = settings
        self.auth = auth
        self.api = api
        self.relay = relay
        self.onSuspend = onSuspend
        self.onResume = onResume
        self.joinTimeout = joinTimeout
        self.minimumDuration = minimumDuration
        self.clock = clock
        self.sleeper = sleep
    }

    public var busy: Bool {
        switch phase {
        case .leaving, .clearing, .adopting, .joining: return true
        default: return false
        }
    }

    public var covering: Bool {
        switch phase {
        case .leaving, .clearing, .adopting: return true
        default: return false
        }
    }

    public var canSwitch: Bool { !covering }

    /// What the overlay says while the switch runs.
    public var statusLine: String {
        switch phase {
        case .leaving: return "Leaving \(fromLabel)…"
        case .clearing: return "Clearing cached data…"
        case .adopting: return "Loading your account…"
        case .joining: return "Connecting to \(target?.label ?? "relay")…"
        case .done: return "Connected to \(target?.label ?? "relay")"
        case .failed: return error
        case .idle: return ""
        }
    }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    public func clearError() {
        if error.isEmpty && phase != .failed { return }
        error = ""
        phase = .idle
        listeners.notify()
    }

    /// Move to `relayId`. Returns a result rather than throwing: the only outright refusals are an
    /// unknown relay and a switch already in flight.
    @discardableResult
    public func switchTo(_ relayId: String) async -> RelaySwitchResult {
        guard let relayProfile = settings.relays.byId(relayId) else {
            return RelaySwitchResult(ok: false, relay: nil, error: "That relay is no longer in your list.")
        }
        if covering {
            return RelaySwitchResult(
                ok: false, relay: relayProfile, error: "Already switching relays. Wait for that to finish.")
        }
        if relayId == settings.relays.activeId {
            // Already there: a double tap in the relay list is harmless, but a missing session is
            // still surfaced.
            return RelaySwitchResult(ok: true, relay: relayProfile, needsSignIn: !auth.hasAppAccess)
        }

        generation += 1
        let current = generation
        let started = clock.now
        target = relayProfile
        fromLabel = settings.relays.active.label
        error = ""
        setPhase(.leaving)

        do {
            if let resuming {
                _ = try? await resuming.value
                if current != generation { return abandoned(relayProfile) }
            }
            // 1. Down. The account machinery first, then the socket: stopping the event channel
            //    after the socket would let a frame arrive for a relay already stopped.
            try await onSuspend?()
            try await relay.disconnect()
            if current != generation { return abandoned(relayProfile) }

            // 2. Forget. Between the two sockets, so nothing can repopulate a cache from the old
            //    relay while it is being cleared.
            setPhase(.clearing)
            api.resetForRelaySwitch()
            relay.resetForRelaySwitch()
            if current != generation { return abandoned(relayProfile) }

            // 3. Move. The directory flips first; auth reads its scope from it, so this order
            //    hands the new relay its own session. Storage follows the directory before the
            //    session does, so everything the session change wakes (owner-key enrolment, the
            //    vault, push) already sees the new project's keys.
            setPhase(.adopting)
            settings.relays.setActive(relayId)
            activator?.activate(relayProfile)
            auth.adoptActiveRelay()
            await settle(since: started)
            if current != generation { return abandoned(relayProfile) }

            // 4. Up.
            setPhase(.joining)
            let resume = Task { @MainActor in if let onResume = self.onResume { try await onResume() } }
            resuming = resume
            defer { if resuming == resume { resuming = nil } }
            try await resume.value
            if current != generation { return abandoned(relayProfile) }
            if !relayProfile.hasInstance {
                setPhase(.done)
                return RelaySwitchResult(ok: true, relay: relayProfile)
            }
            try await relay.connect()
            let connected = await awaitConnection(current)
            if current != generation { return abandoned(relayProfile) }

            // Nothing is raised here: with no session on the new relay `hasAppAccess` is false and
            // the shell already swaps to the auth flow. This flag is for callers that say so in
            // place, such as the relay list.
            let needsSignIn = !auth.hasAppAccess
            setPhase(.done)
            return RelaySwitchResult(
                ok: true, relay: relayProfile, needsSignIn: needsSignIn, offline: !connected,
                error: connected
                    ? ""
                    : "Switched to \(relayProfile.label), but its relay is not answering yet. "
                        + "It will keep retrying in the background.")
        } catch {
            if current != generation { return abandoned(relayProfile) }
            // The directory may already have flipped, so the app is on the new relay whatever went
            // wrong. Say so plainly instead of pretending the switch did not happen.
            self.error =
                "Could not finish switching to \(relayProfile.label). \(RelaySwitchController.humanize(error))"
            setPhase(.failed)
            return RelaySwitchResult(ok: false, relay: relayProfile, error: self.error)
        }
    }

    @discardableResult
    public func attach(_ relayId: String, to instance: RelayProfile) async -> RelaySwitchResult {
        guard let attached = settings.relays.attach(relayId, to: instance) else {
            return RelaySwitchResult(
                ok: false, relay: settings.relays.byId(relayId), error: "That project is already connected to an instance.")
        }
        if relayId != settings.relays.activeId { return RelaySwitchResult(ok: true, relay: attached) }
        do {
            try await onSuspend?()
            try await relay.disconnect()
            api.resetForRelaySwitch()
            relay.resetForRelaySwitch()
            auth.adoptActiveRelay()
            try await onResume?()
            try await relay.connect()
        } catch {
            return RelaySwitchResult(
                ok: false, relay: attached,
                error: "\(attached.label) is now on \(attached.host), but could not connect yet. \(RelaySwitchController.humanize(error))")
        }
        return RelaySwitchResult(ok: true, relay: attached, needsSignIn: !auth.hasAppAccess)
    }

    /// Remove `relayId`, switching away from it first when it is active. The session held for it
    /// is forgotten in the same step, so re-adding it cannot resurrect a live token.
    @discardableResult
    public func removeRelay(_ relayId: String) async -> RelaySwitchResult {
        guard let relayProfile = settings.relays.byId(relayId) else {
            return RelaySwitchResult(ok: false, relay: nil, error: "That relay is no longer in your list.")
        }
        if relayProfile.builtIn {
            return RelaySwitchResult(
                ok: false, relay: relayProfile,
                error: "\(relayProfile.label) is the relay this app ships with and cannot be removed. "
                    + "You can switch away from it instead.")
        }
        var result = RelaySwitchResult(ok: true, relay: relayProfile)
        if relayId == settings.relays.activeId {
            result = await switchTo(settings.relays.fallbackFor(relayId).id)
            if !result.ok { return result }
        }
        auth.forgetRelay(relayId)
        if settings.relays.remove(relayId) {
            // Only the project's own namespace; a legacy-namespace row shares production's.
            activator?.erase(relayProfile)
        }
        return result
    }

    /// Wait for the socket to report connected, but not past `joinTimeout`.
    private func awaitConnection(_ current: Int) async -> Bool {
        if relay.state == .connected { return true }
        let deadline = clock.now.addingTimeInterval(joinTimeout)
        while clock.now < deadline {
            if current != generation { return false }
            if relay.state == .connected { return true }
            if relay.state == .offline { return false }
            try? await sleeper(0.08)
        }
        return relay.state == .connected
    }

    private func settle(since started: Date) async {
        let elapsed = clock.now.timeIntervalSince(started)
        if elapsed >= minimumDuration { return }
        try? await sleeper(minimumDuration - elapsed)
    }

    private func abandoned(_ relayProfile: RelayProfile) -> RelaySwitchResult {
        RelaySwitchResult(ok: false, relay: relayProfile, error: "")
    }

    private func setPhase(_ next: RelaySwitchPhase) {
        if phase == next { return }
        phase = next
        listeners.notify()
    }

    nonisolated static func humanize(_ error: Error) -> String {
        var text = dartErrorText(error)
        if let range = text.range(of: "Exception: ") { text.removeSubrange(range) }
        text = text.dartTrimmed
        if text.isEmpty { return "Try again." }
        return text.hasSuffix(".") ? text : "\(text)."
    }
}
