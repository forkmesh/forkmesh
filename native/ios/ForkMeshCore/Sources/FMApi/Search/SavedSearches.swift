import FMBase
import Foundation
import Observation

public struct SavedSearch: Codable, Equatable, Identifiable, Sendable {
    public var query: String
    public var scope: GlobalSearchScope
    public var lastCount: Int
    public var lastRunAt: Double

    public init(query: String, scope: GlobalSearchScope, lastCount: Int = 0, lastRunAt: Double = 0) {
        self.query = query
        self.scope = scope
        self.lastCount = lastCount
        self.lastRunAt = lastRunAt
    }

    public var id: String { "\(scope.rawValue):\(query.lowercased())" }

    public var route: Route { .search(query: query, scope: scope.routeScope) }
}

public struct RecentSearch: Codable, Equatable, Identifiable, Sendable {
    public var query: String
    public var scope: GlobalSearchScope

    public init(query: String, scope: GlobalSearchScope) {
        self.query = query
        self.scope = scope
    }

    public var id: String { "\(scope.rawValue):\(query.lowercased())" }
}

@MainActor
@Observable
public final class SavedSearchStore {
    public static let savedKey = "fm.search.saved.v1"
    public static let recentKey = "fm.search.recent.v1"
    public static let maxSaved = 20
    public static let maxRecent = 10

    public private(set) var saved: [SavedSearch] = []
    public private(set) var recent: [RecentSearch] = []

    @ObservationIgnored private let store: KeyValueStore?
    @ObservationIgnored private let now: () -> Date

    public init(store: KeyValueStore?, now: @escaping () -> Date = Date.init) {
        self.store = store
        self.now = now
        saved = Self.read([SavedSearch].self, store?.string(forKey: Self.savedKey))
        recent = Self.read([RecentSearch].self, store?.string(forKey: Self.recentKey))
    }

    public func contains(query: String, scope: GlobalSearchScope) -> Bool {
        let id = SavedSearch(query: query, scope: scope).id
        return saved.contains { $0.id == id }
    }

    public func save(query: String, scope: GlobalSearchScope, count: Int) {
        guard !query.isEmpty else { return }
        let entry = SavedSearch(query: query, scope: scope, lastCount: count, lastRunAt: now().timeIntervalSince1970)
        saved = Array(([entry] + saved.filter { $0.id != entry.id }).prefix(Self.maxSaved))
        persist()
    }

    public func remove(_ search: SavedSearch) {
        saved.removeAll { $0.id == search.id }
        persist()
    }

    public func recordRun(query: String, scope: GlobalSearchScope, count: Int) {
        let id = SavedSearch(query: query, scope: scope).id
        guard let index = saved.firstIndex(where: { $0.id == id }) else { return }
        saved[index].lastCount = count
        saved[index].lastRunAt = now().timeIntervalSince1970
        persist()
    }

    public func commit(query: String, scope: GlobalSearchScope) {
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return }
        let entry = RecentSearch(query: trimmed, scope: scope)
        recent = Array(([entry] + recent.filter { $0.id != entry.id }).prefix(Self.maxRecent))
        persist()
    }

    public func clearRecent() {
        recent = []
        persist()
    }

    private func persist() {
        guard let store = store else { return }
        store.set(Self.write(saved), forKey: Self.savedKey)
        store.set(Self.write(recent), forKey: Self.recentKey)
    }

    private static func read<T: Decodable>(_ type: T.Type, _ text: String?) -> T where T: ExpressibleByArrayLiteral {
        guard let text = text, let data = text.data(using: .utf8),
              let value = try? JSONDecoder().decode(T.self, from: data) else { return [] }
        return value
    }

    private static func write<T: Encodable>(_ value: T) -> String {
        guard let data = try? JSONEncoder().encode(value) else { return "[]" }
        return String(decoding: data, as: UTF8.self)
    }
}
