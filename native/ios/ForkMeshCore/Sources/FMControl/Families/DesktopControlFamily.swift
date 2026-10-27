// Gate 2 (docs/native/DESKTOP_PARITY.md §19.3): one method family per workstream on the desktop's
// local control (`LocalControl::registerFamily("teammates.", handler)` on the desktop), and one
// typed client per family here. A family client never sees another family's methods or bells.
//
// Families and their owners: agentops. (WS10), mergequeue. (WS11), fleet. files.browse. diag. (WS16),
// teammates. insights. (WS17), discord. (WS13), wallet. (WS19).

import FMBase
import FMChat
import Foundation

/// What a family client needs from the channel to the desktop: `DesktopControlHub` in the app, an
/// in-memory fake in tests.
@MainActor
public protocol DesktopControlCalling: AnyObject {
    func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject
    @discardableResult
    func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID
    func removeEventTap(_ id: UUID)
}

extension DesktopControlHub: DesktopControlCalling {}

@MainActor
public final class DesktopScopedChannel: DesktopControlCalling {
    public let hub: DesktopControlHub
    public let desktopKey: String

    public init(hub: DesktopControlHub, desktopKey: String) {
        self.hub = hub
        self.desktopKey = desktopKey
    }

    public func request(_ method: String, _ params: JSONObject, timeout: TimeInterval) async throws -> JSONObject {
        try await hub.request(method, params, timeout: timeout, to: desktopKey)
    }

    @discardableResult
    public func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        hub.addEventTap(tap, for: desktopKey)
    }

    public func removeEventTap(_ id: UUID) { hub.removeEventTap(id) }
}

extension DesktopControlHub {
    public func channel(for desktopKey: String) -> DesktopControlCalling {
        desktopKey.isEmpty ? self : DesktopScopedChannel(hub: self, desktopKey: desktopKey)
    }
}

/// The method prefixes Gate 2 reserves. The desktop answers `unsupported` for a family it does not
/// have yet (an older desktop), which `DesktopControlFamily.isUnsupported` recognises.
public enum DesktopControlFamilyName: String, CaseIterable, Sendable {
    case agentOps = "agentops."
    case mergeQueue = "mergequeue."
    case pulls = "pulls."
    case fleet = "fleet."
    case filesBrowse = "files.browse."
    case diagnostics = "diag."
    case teammates = "teammates."
    case insights = "insights."
    case discord = "discord."
    case secrets = "secrets."
    case stats = "stats."
    case repos = "repos."
    case update = "update."
    case pings = "pings."

    /// The prefix of a full method name, if it belongs to a reserved family.
    public static func owning(_ method: String) -> DesktopControlFamilyName? {
        // Longest prefix first, so `files.browse.` never loses to a shorter one.
        allCases.sorted { $0.rawValue.count > $1.rawValue.count }.first { method.hasPrefix($0.rawValue) }
    }
}

/// A typed client for one family: `call("list")` sends `teammates.list`; `events` delivers only the
/// family's bells (`teammates.changed` and the like) with the prefix removed.
@MainActor
public final class DesktopControlFamily {
    public let name: DesktopControlFamilyName
    private let channel: DesktopControlCalling

    public init(_ name: DesktopControlFamilyName, channel: DesktopControlCalling) {
        self.name = name
        self.channel = channel
    }

    public var prefix: String { name.rawValue }

    /// The full method name for a family-relative one.
    public func method(_ suffix: String) -> String {
        suffix.hasPrefix(prefix) ? suffix : prefix + suffix
    }

    public func call(_ suffix: String, _ params: JSONObject = [:], timeout: TimeInterval = 20) async throws
        -> JSONObject
    {
        try await channel.request(method(suffix), params, timeout: timeout)
    }

    /// Bells for this family only, as (suffix, data). Remove with `removeEventTap`.
    @discardableResult
    public func addEventTap(_ tap: @escaping @MainActor (String, JSONObject) -> Void) -> UUID {
        let prefix = self.prefix
        return channel.addEventTap { event, data in
            guard event.hasPrefix(prefix) else { return }
            tap(String(event.dropFirst(prefix.count)), data)
        }
    }

    public func removeEventTap(_ id: UUID) { channel.removeEventTap(id) }

    /// The same bells as an async stream; the tap is removed when the stream ends.
    public func events() -> AsyncStream<(String, JSONObject)> {
        AsyncStream { continuation in
            let id = addEventTap { suffix, data in continuation.yield((suffix, data)) }
            continuation.onTermination = { [weak self] _ in
                Task { @MainActor in self?.removeEventTap(id) }
            }
        }
    }

    /// True when the desktop does not know this family yet (it predates the workstream).
    public nonisolated static func isUnsupported(_ error: Error) -> Bool {
        guard let error = error as? LocalControlException else { return false }
        return error.code == "unsupported" || error.code == "unknown_method"
    }
}
