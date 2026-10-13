import Foundation
import Observation

public enum AgentDeleteOutcome: Equatable, Sendable {
    case deleted(String)
    case failed(String)

    public var message: String {
        switch self {
        case .deleted(let text), .failed(let text): return text
        }
    }

    public var failed: Bool {
        if case .failed = self { return true }
        return false
    }
}

public struct AgentUndoOffer: Equatable, Sendable {
    public let key: String
    public let message: String
    public let serial: Int
}

@MainActor
@Observable
public final class AgentPendingDeletes {
    public static let shared = AgentPendingDeletes()
    public static let undoWindow: Duration = .seconds(5)

    public private(set) var hidden: Set<String> = []
    public private(set) var offer: AgentUndoOffer?
    public private(set) var failure: String?
    public private(set) var failures = 0
    public private(set) var report: String?
    public private(set) var reports = 0

    @ObservationIgnored public let window: Duration
    @ObservationIgnored private var waits: [String: Task<Void, Never>] = [:]
    @ObservationIgnored private var serial = 0

    public init(window: Duration = AgentPendingDeletes.undoWindow) {
        self.window = window
    }

    public func isHidden(_ key: String) -> Bool {
        hidden.contains(key)
    }

    public func isWaiting(_ key: String) -> Bool {
        waits[key] != nil
    }

    @discardableResult
    public func schedule(_ key: String, message: String, commit: @escaping @MainActor () async -> AgentDeleteOutcome) -> Bool {
        guard !hidden.contains(key) else { return false }
        hidden.insert(key)
        serial += 1
        offer = AgentUndoOffer(key: key, message: message, serial: serial)
        let window = window
        waits[key] = Task { @MainActor [weak self] in
            try? await Task.sleep(for: window)
            guard !Task.isCancelled, let self else { return }
            self.waits[key] = nil
            if self.offer?.key == key { self.offer = nil }
            switch await commit() {
            case .deleted(let text):
                self.report = text
                self.reports += 1
            case .failed(let text):
                self.hidden.remove(key)
                self.failure = text
                self.failures += 1
            }
        }
        return true
    }

    @discardableResult
    public func undo(_ key: String) -> Bool {
        guard let wait = waits.removeValue(forKey: key) else { return false }
        wait.cancel()
        hidden.remove(key)
        if offer?.key == key { offer = nil }
        return true
    }
}

extension AgentLineDeletion {
    public var key: String {
        switch self {
        case .desktop(let sessionId): return "desktop:\(sessionId)"
        case .outside(let id): return "desktop:\(id)"
        case .runner(let ref, _, _): return "runner:\(ref.sessionId)"
        }
    }
}
