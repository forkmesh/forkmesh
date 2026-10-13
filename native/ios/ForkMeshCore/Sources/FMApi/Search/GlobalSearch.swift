import FMBase
import Foundation
import Observation

public enum GlobalSearchScope: String, CaseIterable, Codable, Sendable {
    case all, code, issues, chat, people

    public var title: String {
        switch self {
        case .all: return "All"
        case .code: return "Code"
        case .issues: return "Issues & PRs"
        case .chat: return "Chat"
        case .people: return "People"
        }
    }

    public var routeScope: SearchScope {
        switch self {
        case .all: return .everything
        case .code: return .code
        case .issues: return .issues
        case .chat: return .chat
        case .people: return .people
        }
    }

    public init(_ scope: SearchScope) {
        switch scope {
        case .code, .repos: self = .code
        case .issues, .pulls: self = .issues
        case .chat: self = .chat
        case .people: self = .people
        case .everything, .notes, .commands: self = .all
        }
    }
}

public enum SearchBucketKind: String, CaseIterable, Sendable {
    case repositories, destinations, people, commits, issues, pulls, code, chat

    public var title: String {
        switch self {
        case .destinations: return "Go to"
        case .repositories: return "Repositories"
        case .issues: return "Issues"
        case .pulls: return "Pull requests"
        case .commits: return "Commits"
        case .code: return "Code"
        case .people: return "People"
        case .chat: return "Chat"
        }
    }

    public var scope: GlobalSearchScope {
        switch self {
        case .destinations: return .all
        case .issues, .pulls: return .issues
        case .repositories, .commits, .code: return .code
        case .people: return .people
        case .chat: return .chat
        }
    }
}

public struct SearchHit: Identifiable, Equatable, Hashable, Sendable {
    public let id: String
    public let title: String
    public let subtitle: String
    public let symbol: String
    public let route: Route

    public init(id: String, title: String, subtitle: String = "", symbol: String, route: Route) {
        self.id = id
        self.title = title
        self.subtitle = subtitle
        self.symbol = symbol
        self.route = route
    }
}

public struct SearchBucket: Identifiable, Equatable, Sendable {
    public let kind: SearchBucketKind
    public let hits: [SearchHit]
    public let total: Int
    public var id: String { kind.rawValue }

    public init(kind: SearchBucketKind, hits: [SearchHit], total: Int) {
        self.kind = kind
        self.hits = hits
        self.total = total
    }

    public var hasMore: Bool { total > hits.count }
}

public protocol GlobalSearchSource: Sendable {
    var kind: SearchBucketKind { get }
    func search(_ query: String) async throws -> [SearchHit]
}

public enum SearchMatch {
    public static func score(_ text: String, _ query: String) -> Int? {
        let needle = query.trimmingCharacters(in: .whitespaces).lowercased()
        guard !needle.isEmpty else { return nil }
        let haystack = text.lowercased()
        if haystack == needle { return 0 }
        if haystack.hasPrefix(needle) { return 1 }
        if haystack.split(whereSeparator: { " /-_.#@".contains($0) }).contains(where: { $0.hasPrefix(needle) }) {
            return 2
        }
        if haystack.contains(needle) { return 3 }
        let loose = compact(needle)
        if !loose.isEmpty && compact(haystack).contains(loose) { return 4 }
        return nil
    }

    static func compact(_ text: String) -> String {
        String(text.filter { $0.isLetter || $0.isNumber })
    }

    public static func rank<T>(_ items: [T], _ query: String, text: (T) -> [String]) -> [T] {
        let scored: [(Int, Int, T)] = items.enumerated().compactMap { (offset, item) in
            let best = text(item).compactMap { score($0, query) }.min()
            return best.map { ($0, offset, item) }
        }
        return scored.sorted { ($0.0, $0.1) < ($1.0, $1.1) }.map(\.2)
    }
}

public struct DestinationsSearchSource: GlobalSearchSource {
    public let kind: SearchBucketKind = .destinations
    public let routes: [Route]

    public init(routes: [Route] = DestinationsSearchSource.defaultRoutes) {
        self.routes = routes
    }

    public static var allRoutes: [Route] {
        AppTab.barOrder.map(\.root) + HubCatalog.routes + [
            .agents(.needsYou), .agents(.running), .inbox(.approvals), .inbox(.reviews), .inbox(.mentions),
            .localRepos, .pingSettings, .projects,
        ]
    }

    public static var defaultRoutes: [Route] {
        let installed = PackageGate.installed
        return allRoutes.filter { PackageGate.shows($0, installed: installed) }
    }

    public func search(_ query: String) async throws -> [SearchHit] {
        let ranked = SearchMatch.rank(routes, query) { [$0.title, RouteCatalog.title($0.kind)] }
        return ranked.map { route in
            SearchHit(
                id: "route:\(route.kind.rawValue):\(route.title)", title: route.title,
                symbol: RouteCatalog.symbol(route.kind), route: route)
        }
    }
}

public struct LocalRepositoryItem: Equatable, Sendable {
    public let name: String
    public let description: String
    public let publishedAs: String

    public init(name: String, description: String = "", publishedAs: String = "") {
        self.name = name
        self.description = description
        self.publishedAs = publishedAs
    }
}

public struct RepositoriesSearchSource: GlobalSearchSource {
    public let kind: SearchBucketKind = .repositories
    let load: @Sendable () async throws -> [Repository]
    let local: @Sendable () async -> [LocalRepositoryItem]

    public init(
        load: @escaping @Sendable () async throws -> [Repository],
        local: @escaping @Sendable () async -> [LocalRepositoryItem] = { [] }
    ) {
        self.load = load
        self.local = local
    }

    public func search(_ query: String) async throws -> [SearchHit] {
        let device = await local()
        let repos: [Repository]
        do {
            repos = try await load()
        } catch {
            if device.isEmpty { throw error }
            repos = []
        }
        let network = Set(repos.map { "\($0.owner)/\($0.name)".lowercased() })
        let candidates: [(texts: [String], hit: SearchHit)] = repos.map { repo in
            (
                [repo.name, "\(repo.owner)/\(repo.name)", repo.description],
                SearchHit(
                    id: "repo:\(repo.owner)/\(repo.name)", title: repo.name,
                    subtitle: Self.subtitle(repo.owner, repo.description), symbol: RouteCatalog.symbol(.repo),
                    route: .repo(RepoRef(owner: repo.owner, name: repo.name), section: nil))
            )
        } + device.filter { !network.contains($0.publishedAs.lowercased()) }.map { item in
            (
                [item.name, item.publishedAs, item.description],
                SearchHit(
                    id: "local:\(item.name)", title: item.name,
                    subtitle: Self.subtitle("On this device", item.description), symbol: RouteCatalog.symbol(.localRepo),
                    route: .localRepo(id: item.name))
            )
        }
        return SearchMatch.rank(candidates, query) { $0.texts }.map(\.hit)
    }

    static func subtitle(_ place: String, _ description: String) -> String {
        description.isEmpty ? place : "\(place) · \(description)"
    }

    public static func named(_ query: String, in catalog: [Repository], limit: Int) -> [RepoRef] {
        let names: (Repository) -> [String] = { [$0.name, "\($0.owner)/\($0.name)"] }
        let close = catalog.filter { repo in names(repo).contains { (SearchMatch.score($0, query) ?? Int.max) <= 1 } }
        return SearchMatch.rank(close, query, text: names).prefix(limit).map { RepoRef(owner: $0.owner, name: $0.name) }
    }
}

public struct SearchableItem: Equatable, Sendable {
    public let number: Int
    public let title: String
    public let body: String
    public let status: String
    public let isPull: Bool

    public init(number: Int, title: String, body: String = "", status: String = "", isPull: Bool) {
        self.number = number
        self.title = title
        self.body = body
        self.status = status
        self.isPull = isPull
    }
}

public struct IssuesSearchSource: GlobalSearchSource {
    public let kind: SearchBucketKind
    let repos: @Sendable (String) async -> [RepoRef]
    let items: @Sendable (RepoRef) async throws -> [SearchableItem]

    public init(
        pulls: Bool, repos: @escaping @Sendable (String) async -> [RepoRef],
        items: @escaping @Sendable (RepoRef) async throws -> [SearchableItem]
    ) {
        self.kind = pulls ? .pulls : .issues
        self.repos = repos
        self.items = items
    }

    public func search(_ query: String) async throws -> [SearchHit] {
        let wantsPulls = kind == .pulls
        var hits: [SearchHit] = []
        for repo in await repos(query) {
            let list = (try? await items(repo)) ?? []
            let wanted = list.filter { $0.isPull == wantsPulls }
            let number = Int(query.trimmingCharacters(in: CharacterSet(charactersIn: "# ")))
            let ranked = SearchMatch.rank(wanted, query) { [$0.title, $0.body] }
            let exact = number.map { value in wanted.filter { $0.number == value } } ?? []
            for item in exact + ranked.filter({ candidate in !exact.contains(candidate) }) {
                let route: Route = wantsPulls
                    ? .pull(repo, number: item.number, tab: .conversation)
                    : .issue(repo, number: item.number)
                hits.append(SearchHit(
                    id: "\(kind.rawValue):\(repo.slug)#\(item.number)", title: item.title,
                    subtitle: "\(repo.slug) #\(item.number)\(item.status.isEmpty ? "" : " · \(item.status)")",
                    symbol: RouteCatalog.symbol(route.kind), route: route))
            }
        }
        return hits
    }
}

public struct CodeMatchItem: Equatable, Sendable {
    public let path: String
    public let line: Int
    public let text: String

    public init(path: String, line: Int, text: String) {
        self.path = path
        self.line = line
        self.text = text
    }
}

public struct CommitItem: Equatable, Sendable {
    public let sha: String
    public let subject: String
    public let author: String

    public init(sha: String, subject: String, author: String = "") {
        self.sha = sha
        self.subject = subject
        self.author = author
    }
}

public struct CommitsSearchSource: GlobalSearchSource {
    public let kind: SearchBucketKind = .commits
    let repos: @Sendable (String) async -> [RepoRef]
    let commits: @Sendable (RepoRef) async throws -> [CommitItem]

    public init(
        repos: @escaping @Sendable (String) async -> [RepoRef],
        commits: @escaping @Sendable (RepoRef) async throws -> [CommitItem]
    ) {
        self.repos = repos
        self.commits = commits
    }

    public func search(_ query: String) async throws -> [SearchHit] {
        let needle = query.trimmingCharacters(in: .whitespaces).lowercased()
        guard needle.count >= 2 else { return [] }
        let hex = needle.count >= 4 && needle.allSatisfy(\.isHexDigit)
        var hits: [SearchHit] = []
        for repo in await repos(query) {
            let list = ((try? await commits(repo)) ?? []).filter { !$0.sha.isEmpty }
            let bySha = hex ? list.filter { $0.sha.lowercased().hasPrefix(needle) } : []
            let ranked = SearchMatch.rank(list, query) { [$0.subject, $0.author] }
            for item in bySha + ranked.filter({ candidate in !bySha.contains(candidate) }) {
                let short = String(item.sha.prefix(7))
                let author = item.author.isEmpty ? "" : " · \(item.author)"
                hits.append(SearchHit(
                    id: "commit:\(repo.slug)@\(item.sha)", title: item.subject.isEmpty ? short : item.subject,
                    subtitle: "\(repo.slug) \(short)\(author)", symbol: RouteCatalog.symbol(.commit),
                    route: .commit(repo, sha: item.sha)))
            }
        }
        return hits
    }
}

public struct CodeSearchSource: GlobalSearchSource {
    public let kind: SearchBucketKind = .code
    let repos: @Sendable (String) async -> [RepoRef]
    let search: @Sendable (RepoRef, String) async throws -> [CodeMatchItem]

    public init(
        repos: @escaping @Sendable (String) async -> [RepoRef],
        search: @escaping @Sendable (RepoRef, String) async throws -> [CodeMatchItem]
    ) {
        self.repos = repos
        self.search = search
    }

    public func search(_ query: String) async throws -> [SearchHit] {
        guard query.trimmingCharacters(in: .whitespaces).count >= 2 else { return [] }
        var hits: [SearchHit] = []
        for repo in await repos(query) {
            for match in (try? await search(repo, query)) ?? [] {
                hits.append(SearchHit(
                    id: "code:\(repo.slug):\(match.path):\(match.line)", title: match.path,
                    subtitle: "\(repo.slug)\(match.line > 0 ? ":\(match.line)" : "") \(match.text.trimmingCharacters(in: .whitespaces))",
                    symbol: "doc.text", route: .file(repo, ref: "", path: match.path)))
            }
        }
        return hits
    }
}

public struct PeopleSearchSource: GlobalSearchSource {
    public let kind: SearchBucketKind = .people
    let known: @Sendable () async -> [String]
    let accounts: @Sendable (String) async -> [String]

    public init(
        known: @escaping @Sendable () async -> [String] = { [] },
        accounts: @escaping @Sendable (String) async -> [String] = { _ in [] }
    ) {
        self.known = known
        self.accounts = accounts
    }

    public func search(_ query: String) async throws -> [SearchHit] {
        let trimmed = query.trimmingCharacters(in: .whitespaces)
        let name = trimmed.hasPrefix("@") ? String(trimmed.dropFirst()) : trimmed
        let valid = name.count >= 2 && name.allSatisfy { $0.isLetter || $0.isNumber || "-_.".contains($0) }
        let found = valid ? await accounts(name) : []
        var names: [String] = []
        for person in SearchMatch.rank(await known() + found, name, text: { [$0] })
        where !person.isEmpty && !names.contains(where: { $0.lowercased() == person.lowercased() }) {
            names.append(person)
        }
        return names.map { person in
            SearchHit(id: "person:\(person.lowercased())", title: "@\(person)", symbol: "person.crop.circle",
                      route: .profile(name: person))
        }
    }
}

public struct ChatSearchSource: GlobalSearchSource {
    public let kind: SearchBucketKind = .chat
    let conversations: @Sendable () async -> [String]

    public init(conversations: @escaping @Sendable () async -> [String]) {
        self.conversations = conversations
    }

    public func search(_ query: String) async throws -> [SearchHit] {
        let ranked = SearchMatch.rank(await conversations(), query) { [$0] }
        return ranked.map { conversation in
            let route: Route = conversation.hasPrefix("@")
                ? .dm(user: String(conversation.dropFirst()))
                : .channel(id: conversation)
            return SearchHit(id: "chat:\(conversation)", title: conversation, symbol: RouteCatalog.symbol(route.kind), route: route)
        }
    }
}

@MainActor
@Observable
public final class GlobalSearchModel {
    public static let bucketLimit = 5
    public static let seeAllLimit = 50

    public var query: String = ""
    public var scope: GlobalSearchScope = .all
    public private(set) var buckets: [SearchBucket] = []
    public private(set) var searching = false
    public private(set) var failures: [SearchBucketKind] = []
    public private(set) var expanded: SearchBucketKind?
    public let saved: SavedSearchStore

    @ObservationIgnored private let sources: [GlobalSearchSource]
    @ObservationIgnored private let debounce: Duration
    @ObservationIgnored private var pending: Task<Void, Never>?
    @ObservationIgnored private var generation = 0

    public init(sources: [GlobalSearchSource], saved: SavedSearchStore, debounce: Duration = .milliseconds(250)) {
        self.sources = sources
        self.saved = saved
        self.debounce = debounce
    }

    public var isEmptyQuery: Bool { query.trimmingCharacters(in: .whitespaces).isEmpty }

    public var total: Int { buckets.reduce(0) { $0 + $1.total } }

    public func sources(for scope: GlobalSearchScope) -> [GlobalSearchSource] {
        sources.filter { scope == .all || $0.kind.scope == scope }
    }

    public func queryChanged() {
        pending?.cancel()
        expanded = nil
        if isEmptyQuery {
            generation += 1
            buckets = []
            searching = false
            return
        }
        let delay = debounce
        pending = Task { @MainActor [weak self] in
            try? await Task.sleep(for: delay)
            if Task.isCancelled { return }
            await self?.run()
        }
    }

    public func expand(_ kind: SearchBucketKind?) async {
        pending?.cancel()
        expanded = kind
        await run()
    }

    public func run() async {
        generation += 1
        let mine = generation
        let text = query.trimmingCharacters(in: .whitespaces)
        guard !text.isEmpty else {
            buckets = []
            return
        }
        searching = true
        let active = sources(for: scope).filter { expanded == nil || $0.kind == expanded }
        var results: [SearchBucketKind: [SearchHit]] = [:]
        var failed: [SearchBucketKind] = []
        await withTaskGroup(of: (SearchBucketKind, [SearchHit]?).self) { group in
            for source in active {
                group.addTask {
                    do {
                        return (source.kind, try await source.search(text))
                    } catch {
                        return (source.kind, nil)
                    }
                }
            }
            for await (kind, hits) in group {
                if let hits = hits {
                    results[kind, default: []].append(contentsOf: hits)
                } else {
                    failed.append(kind)
                }
            }
        }
        guard mine == generation else { return }
        let limit = expanded == nil ? GlobalSearchModel.bucketLimit : GlobalSearchModel.seeAllLimit
        buckets = SearchBucketKind.allCases.compactMap { kind in
            guard let hits = results[kind], !hits.isEmpty else { return nil }
            return SearchBucket(kind: kind, hits: Array(hits.prefix(limit)), total: hits.count)
        }
        failures = failed.sorted { $0.rawValue < $1.rawValue }
        searching = false
        saved.recordRun(query: text, scope: scope, count: total)
    }

    public func saveCurrent() {
        saved.save(query: query.trimmingCharacters(in: .whitespaces), scope: scope, count: total)
    }

    public var isSaved: Bool {
        saved.contains(query: query.trimmingCharacters(in: .whitespaces), scope: scope)
    }

    public func apply(_ search: SavedSearch) async {
        pending?.cancel()
        query = search.query
        scope = search.scope
        expanded = nil
        await run()
    }
}
