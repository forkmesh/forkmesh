import FMAccount
import Foundation
import Observation

/// Rung when the relay says this account's agent sessions changed, so the Agents tab reloads the
/// moment a desktop starts, advances or ends a run. It carries no data: the account event frame
/// is only a doorbell, and the sessions are still read over the tab's own signed request.
/// SwiftUI observes `rings`; other code uses `addListener`.
@MainActor
@Observable
public final class AgentListSignal {
    public private(set) var rings = 0
    public private(set) var liveRings = 0
    @ObservationIgnored public let listeners = Listeners()
    @ObservationIgnored public let liveListeners = Listeners()

    public init() {}

    public func ring() {
        rings &+= 1
        listeners.notify()
    }

    public func ringLive() {
        liveRings &+= 1
        liveListeners.notify()
    }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    @discardableResult
    public func addLiveListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { liveListeners.add(listener) }

    public func removeLiveListener(_ token: ListenerToken) { liveListeners.remove(token) }
}
