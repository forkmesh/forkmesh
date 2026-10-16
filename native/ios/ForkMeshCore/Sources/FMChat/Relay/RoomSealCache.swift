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

    subscript(room: String) -> RoomSeal? { loaded[room] }

    func seal(_ room: String, storageScope: String) async throws -> RoomSeal {
        if let existing = loaded[room] { return existing }
        if let pending = loading[room] { return try await pending.value }
        let store = secureStore
        let task = Task.detached(priority: .userInitiated) {
            try RoomSeal.loadOrCreate(room, storageScope: storageScope, secureStore: store)
        }
        loading[room] = task
        defer { loading.removeValue(forKey: room) }
        let seal = try await task.value
        loaded[room] = seal
        return seal
    }
}
