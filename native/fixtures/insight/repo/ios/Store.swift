import Foundation

protocol Store {
    func load() -> [String]
}

final class MemoryStore: Store {
    private var values: [String] = []

    func load() -> [String] {
        guard !values.isEmpty else { return [] }
        return values.filter { !$0.isEmpty }
    }
}
