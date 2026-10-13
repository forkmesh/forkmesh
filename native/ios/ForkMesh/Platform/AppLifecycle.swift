// SwiftUI's ScenePhase mapped onto FMAccount's AppLifecycleState and fanned
// out to whoever registered (the performance monitor, the vault, the inbox,
// push rebinding: see AppServices).

import FMAccount
import Foundation
import Observation
import SwiftUI

@MainActor
@Observable
final class AppLifecycle {
    static let shared: AppLifecycle = AppLifecycle()

    /// The last state delivered. The app starts resumed.
    private(set) var state: AppLifecycleState = .resumed

    var isForeground: Bool {
        return state == .resumed
    }

    @ObservationIgnored private var observers: [UUID: (AppLifecycleState) -> Void] = [:]

    /// active -> resumed, inactive -> inactive, background -> paused.
    static func map(_ phase: ScenePhase) -> AppLifecycleState {
        switch phase {
        case .active:
            return .resumed
        case .inactive:
            return .inactive
        case .background:
            return .paused
        @unknown default:
            return .inactive
        }
    }

    /// Called with every change after registration; the token removes it.
    @discardableResult
    func observe(_ handler: @escaping (AppLifecycleState) -> Void) -> UUID {
        let token: UUID = UUID()
        observers[token] = handler
        return token
    }

    func remove(_ token: UUID) {
        observers.removeValue(forKey: token)
    }

    /// The root view's `.onChange(of: scenePhase)`.
    func scenePhaseChanged(_ phase: ScenePhase) {
        let next: AppLifecycleState = AppLifecycle.map(phase)
        if next == state {
            return
        }
        state = next
        let handlers: [(AppLifecycleState) -> Void] = Array(observers.values)
        for handler in handlers {
            handler(next)
        }
    }
}
