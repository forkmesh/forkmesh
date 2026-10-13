// The network catalog's logic: search, filter and sort, and what each row
// says.

import FMApi
import FMBase
import Foundation
import Observation

/// `_RepoFilter`.
public enum RepoFilter: String, CaseIterable, Sendable {
    case all, online, offline, `public`, `private`

    public var label: String {
        switch self {
        case .all: return "All"
        case .online: return "Online"
        case .offline: return "Offline"
        case .public: return "Public"
        case .private: return "Private"
        }
    }
}

/// `_RepoSort`.
public enum RepoSort: String, CaseIterable, Sendable {
    case recentlyUpdated, name, mirrors

    public var buttonLabel: String {
        switch self {
        case .recentlyUpdated: return "Updated"
        case .name: return "Name"
        case .mirrors: return "Mirrors"
        }
    }

    public var menuLabel: String {
        switch self {
        case .recentlyUpdated: return "Recently updated first"
        case .name: return "Name A-Z"
        case .mirrors: return "Mirrors high-low"
        }
    }
}

/// One `_RepoMetaItem` of a catalog row.
public struct RepoMetaItem: Equatable, Hashable, Sendable {
    /// Material icon name.
    public var icon: String
    public var text: String
    /// The exact value behind an abbreviated one, or nil.
    public var tooltip: String?
    /// The language dot: accentEdge at 9 pt instead of the muted 14 pt icon.
    public var isLanguage: Bool

    public init(icon: String, text: String, tooltip: String? = nil, isLanguage: Bool = false) {
        self.icon = icon
        self.text = text
        self.tooltip = tooltip
        self.isLanguage = isLanguage
    }
}

public enum ReposCatalog {
    /// `_isRepoOnline`.
    public static func isOnline(_ repo: Repository) -> Bool { repo.cloneOnline || repo.liveHost }

    /// `_matchesQuery`; `query` is already trimmed and lower-cased.
    public static func matchesQuery(_ repo: Repository, _ query: String) -> Bool {
        if query.isEmpty { return true }
        return repo.fullName.lowercased().contains(query) || repo.owner.lowercased().contains(query)
            || repo.description.lowercased().contains(query) || repo.language.lowercased().contains(query)
    }

    /// `_matchesFilter`.
    public static func matchesFilter(_ repo: Repository, _ filter: RepoFilter) -> Bool {
        let online = isOnline(repo)
        switch filter {
        case .all: return true
        case .online: return online
        case .offline: return !online
        case .public: return !repo.isPrivate
        case .private: return repo.isPrivate
        }
    }

    /// Lower-cased full names in UTF-16 code unit order.
    static func compareNames(_ a: Repository, _ b: Repository) -> Int {
        compareUTF16(a.fullName.lowercased(), b.fullName.lowercased())
    }

    static func compareUTF16(_ a: String, _ b: String) -> Int {
        let x = Array(a.utf16)
        let y = Array(b.utf16)
        for i in 0..<min(x.count, y.count) where x[i] != y[i] {
            return x[i] < y[i] ? -1 : 1
        }
        return x.count == y.count ? 0 : (x.count < y.count ? -1 : 1)
    }

    /// `_compareRepos`.
    public static func compare(_ a: Repository, _ b: Repository, _ sort: RepoSort) -> Int {
        switch sort {
        case .recentlyUpdated:
            let updated = b.updatedMs == a.updatedMs ? 0 : (b.updatedMs < a.updatedMs ? -1 : 1)
            return updated == 0 ? compareNames(a, b) : updated
        case .name:
            return compareNames(a, b)
        case .mirrors:
            let mirrors = b.mirrors == a.mirrors ? 0 : (b.mirrors < a.mirrors ? -1 : 1)
            return mirrors == 0 ? compareNames(a, b) : mirrors
        }
    }

    /// Ties only arise between identical names; they keep catalog order.
    public static func filteredAndSorted(
        _ repos: [Repository], query: String, filter: RepoFilter, sort: RepoSort
    ) -> [Repository] {
        let filtered = repos.enumerated().filter { matchesQuery($0.element, query) && matchesFilter($0.element, filter) }
        return filtered.sorted { left, right in
            let order = compare(left.element, right.element, sort)
            return order == 0 ? left.offset < right.offset : order < 0
        }.map(\.element)
    }

    /// The meta line under a row (`_RepoTile`'s Wrap), in order. Zero stars,
    /// forks and mirrors are left out.
    public static func metaItems(_ repo: Repository, nowMillis: Int64 = SystemClock().nowMillis) -> [RepoMetaItem] {
        var items: [RepoMetaItem] = []
        if !repo.language.isEmpty {
            items.append(RepoMetaItem(icon: "circle", text: repo.language, isLanguage: true))
        }
        if let stars = Format.countOrNil(repo.stars) {
            items.append(RepoMetaItem(icon: "star_border", text: stars))
        }
        if let forks = Format.countOrNil(repo.forks) {
            items.append(RepoMetaItem(icon: "call_split", text: forks))
        }
        if let mirrors = Format.countOrNil(repo.mirrors, singular: "mirror") {
            items.append(RepoMetaItem(icon: "dns_outlined", text: mirrors))
        }
        items.append(RepoMetaItem(
            icon: repo.isPrivate ? "lock_outline" : "public", text: repo.isPrivate ? "Private" : "Public"))
        if repo.updatedMs > 0 {
            items.append(RepoMetaItem(
                icon: "update",
                text: "Updated \(Format.relativeTime(Int64(repo.updatedMs), nowMillis: nowMillis))",
                tooltip: "Updated \(Format.absoluteDate(Int64(repo.updatedMs)))"))
        }
        return items
    }
}

/// `_ReposScreenState`: the public catalog of repositories across the network.
@MainActor
@Observable
public final class ReposScreenModel {
    public let catalog = RepoFuture<[Repository]>()
    /// The search field as typed; matching uses it trimmed and lower-cased.
    public var queryText = ""
    public var filter = RepoFilter.all
    public var sort = RepoSort.recentlyUpdated

    @ObservationIgnored private let client: RepoClient

    public init(client: RepoClient) {
        self.client = client
    }

    /// `_query`.
    public var query: String { queryText.dartTrimmed.lowercased() }

    /// First load (`initState`).
    public func start() async {
        await catalog.start { try await client.repositories() }
    }

    /// `_reload` (the Refresh button and pull-to-refresh).
    public func reload() async {
        await catalog.load { try await client.repositories() }
    }

    /// The rows to show: empty while loading or failed.
    public var visibleRepos: [Repository] {
        if catalog.loading || catalog.error != nil { return [] }
        return ReposCatalog.filteredAndSorted(catalog.data ?? [], query: query, filter: filter, sort: sort)
    }

    /// The state the catalog is in, for the fade between skeleton and list.
    public var stateKey: String {
        if catalog.loading { return "loading" }
        if catalog.error != nil { return "error" }
        return "list-\(filter.rawValue)-\(sort.rawValue)-\(query)-\(visibleRepos.count)"
    }

    public var hasActiveControls: Bool { !query.isEmpty || filter != .all }

    public var emptyTitle: String { hasActiveControls ? "No matching repositories" : "No repositories" }

    public var emptyMessage: String {
        hasActiveControls ? "Try another search term or filter." : "Repository mirrors will appear here when available."
    }
}
