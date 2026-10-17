import FMBase
import FMCrypto
import Foundation

@MainActor
final class RoomSealCache {
    private let secureStore: SecureStore
    private var loaded: [String: RoomSeal] = [:]
    private var loading: [String: Task<RoomSeal, Error>] = [:]

    init(secureStore: SecureStore) {
        self.secureStore = secureStore
    }

    subscript(room: String) -> RoomSeal? { loaded[room.dartTrimmed] }

    var all: [RoomSeal] { Array(loaded.values) }

    func seal(
        _ room: String, scope: String? = nil, policy: ChatTrustPolicy, acct: String, node: String,
        participants: Set<String>? = nil
    ) async throws -> RoomSeal {
        let name = room.dartTrimmed
        let key = "\(name)\n\(acct)\n\(node)"
        if let existing = loaded[name], (existing.acct ?? "") == acct, (existing.node ?? "") == node,
            existing.scope == (scope ?? name), existing.participants == participants
        {
            return existing
        }
        if let pending = loading[key] { return try await pending.value }
        let store = secureStore
        let task = Task.detached(priority: .userInitiated) {
            try RoomSeal.loadOrCreate(
                name, scope: scope, policy: policy, acct: acct, node: node, participants: participants, secureStore: store)
        }
        loading[key] = task
        defer { loading.removeValue(forKey: key) }
        let seal = try await task.value
        loaded[name] = seal
        return seal
    }

    func removeAll() {
        loaded.removeAll()
    }
}
