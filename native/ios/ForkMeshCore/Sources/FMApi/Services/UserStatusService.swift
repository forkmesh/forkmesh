// The signed-in member's own away flag and custom status.
//
// Reads and writes go through `POST /api/accounts/user-status`; every change is handed to the
// relay (`OwnStatusAnnouncer`, FMChat's RelayService), which announces it to the room with a
// presence frame straight away. When the custom status reaches its clear-after moment this drops
// it locally and repaints, so nothing shows a status that has lapsed while the app sat open.

import FMAccount
import FMBase
import Foundation
import Observation

/// What announces this member's status on the relay (`RelayService.setOwnStatus`). FMChat's
/// RelayService conforms; FMApi cannot import FMChat.
@MainActor
public protocol OwnStatusAnnouncer: AnyObject {
    func setOwnStatus(_ status: UserStatus?)
}

@MainActor
@Observable
public final class UserStatusService {
    @ObservationIgnored let api: ApiService
    /// Receives every adopted status. Set it once the relay service exists.
    @ObservationIgnored public weak var relay: OwnStatusAnnouncer?
    /// The current account session token, or "" when signed out.
    @ObservationIgnored public let sessionToken: @MainActor () -> String
    @ObservationIgnored let clock: Clock
    @ObservationIgnored let sleeper: @Sendable (TimeInterval) async throws -> Void
    @ObservationIgnored public let listeners = Listeners()

    private var stored: UserStatus?
    private var isBusy = false
    /// Bumped on every change (also when a status lapses), so views re-render.
    public private(set) var revision = 0
    @ObservationIgnored private var token = ""
    @ObservationIgnored private var readAt: Date?
    @ObservationIgnored private var lapse: Task<Void, Never>?
    @ObservationIgnored private var disposed = false

    public init(
        api: ApiService, relay: OwnStatusAnnouncer? = nil, sessionToken: @escaping @MainActor () -> String,
        clock: Clock = SystemClock(),
        sleep: @escaping @Sendable (TimeInterval) async throws -> Void = { seconds in
            try await Task.sleep(nanoseconds: UInt64(max(0, seconds) * 1_000_000_000))
        }
    ) {
        self.api = api
        self.relay = relay
        self.sessionToken = sessionToken
        self.clock = clock
        self.sleeper = sleep
    }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    /// The status as it should be painted now.
    public var status: UserStatus {
        _ = revision
        return (stored ?? .empty).visibleAt(clock.now)
    }

    /// False until the account's status has been read at least once.
    public var known: Bool { stored != nil }

    /// True while a write is in flight.
    public var busy: Bool { isBusy }

    public var signedIn: Bool { !sessionToken().isEmpty }

    /// Follows the session: a new sign-in reads the account's status, a sign-out forgets it (and
    /// stops announcing it on the relay).
    public func syncSession() async {
        let current = sessionToken()
        if current == token { return }
        token = current
        if current.isEmpty {
            adopt(nil)
            return
        }
        do {
            try await refresh()
        } catch {
            Log.warning("user-status", "User status read failed: \(error)")
        }
    }

    /// Re-reads the account's status from the server.
    public func refresh() async throws {
        if !signedIn { return }
        let current = sessionToken()
        readAt = clock.now
        let status = try await api.updateUserStatus()
        // A sign-out (or another account) raced the read.
        if current != sessionToken() { return }
        adopt(status)
    }

    /// The resume read: one request when the app comes back after a while (default 10 minutes).
    public func refreshIfStale(maxAge: TimeInterval = 10 * 60) async throws {
        if let readAt, clock.now.timeIntervalSince(readAt) < maxAge { return }
        try await refresh()
    }

    /// Marks the member away (or back). Applied at once and rolled back if the server refuses;
    /// throws `UserStatusException`.
    public func setAway(_ away: Bool) async throws {
        let before = stored
        adopt(status.copyWith(away: away))
        do {
            try await write(["away": .bool(away)])
        } catch {
            adopt(before)
            throw error
        }
    }

    /// Sets the custom status; `expiresAt` is epoch ms, 0 for never. Throws `UserStatusException`.
    public func setCustom(emoji: String, text: String, expiresAt: Int) async throws {
        try await write([
            "emoji": .string(emoji.dartTrimmed),
            "text": .string(text.dartTrimmed),
            "expiresAt": .int(Int64(expiresAt)),
        ])
    }

    /// Drops the emoji, text and expiry; the away flag stays. Throws `UserStatusException`.
    public func clearCustom() async throws {
        try await write(["clear": true])
    }

    private func write(_ changes: JSONObject) async throws {
        isBusy = true
        notify()
        defer {
            isBusy = false
            notify()
        }
        adopt(try await api.updateUserStatus(changes))
    }

    private func adopt(_ status: UserStatus?) {
        lapse?.cancel()
        lapse = nil
        let now = clock.now
        let visible = status?.visibleAt(now)
        stored = visible
        if let visible, let lapsesAt = visible.lapsesAt {
            let delay = max(0, lapsesAt.timeIntervalSince(now))
            let sleeper = self.sleeper
            lapse = Task { [weak self] in
                do { try await sleeper(delay) } catch { return }
                guard let self, !Task.isCancelled else { return }
                self.adopt(self.stored)
            }
        }
        relay?.setOwnStatus(visible)
        notify()
    }

    private func notify() {
        if disposed { return }
        revision &+= 1
        listeners.notify()
    }

    public func dispose() {
        disposed = true
        lapse?.cancel()
        lapse = nil
    }
}
