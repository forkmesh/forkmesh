// Change listeners for the services in this module outside SwiftUI:
// `addListener` / `removeListener` / `notifyListeners` (for example, the user status service
// follows the auth session this way). SwiftUI views observe the same services through
// Observation instead; see `SettingsService.revision`.

import Foundation

/// Handle returned by `addListener`; pass it to `removeListener`.
public final class ListenerToken: Hashable, Sendable {
    public init() {}

    public static func == (lhs: ListenerToken, rhs: ListenerToken) -> Bool { lhs === rhs }

    public func hash(into hasher: inout Hasher) { hasher.combine(ObjectIdentifier(self)) }
}

/// An ordered set of callbacks. Safe from any thread; callbacks run on the thread that notifies
/// (the main actor for every service in this module except the performance monitor).
public final class Listeners: @unchecked Sendable {
    private let lock = NSLock()
    private var entries: [(ListenerToken, @Sendable () -> Void)] = []

    public init() {}

    @discardableResult
    public func add(_ listener: @escaping @Sendable () -> Void) -> ListenerToken {
        let token = ListenerToken()
        lock.lock()
        entries.append((token, listener))
        lock.unlock()
        return token
    }

    public func remove(_ token: ListenerToken) {
        lock.lock()
        entries.removeAll { $0.0 == token }
        lock.unlock()
    }

    public var isEmpty: Bool {
        lock.lock()
        defer { lock.unlock() }
        return entries.isEmpty
    }

    /// Calls a snapshot of the listeners, so one may remove itself while being called.
    public func notify() {
        lock.lock()
        let snapshot = entries
        lock.unlock()
        for (_, listener) in snapshot { listener() }
    }
}
