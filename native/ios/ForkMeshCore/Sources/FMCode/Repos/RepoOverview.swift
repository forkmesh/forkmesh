import FMApi
import FMBase
import Foundation
import Observation

public protocol RepoOverviewClient: AnyObject {
    func publishedIssues(_ owner: String, _ name: String) async throws -> [Issue]
    func publishedPulls(_ owner: String, _ name: String) async throws -> [PublishedPull]
    func branches(_ owner: String, _ name: String) async throws -> [RepoBranch]
    func releases(_ owner: String, _ name: String) async throws -> [RepoRelease]
    func commits(_ owner: String, _ name: String) async throws -> [JSONObject]
}

extension ApiService: RepoOverviewClient {}

public struct RepoOverviewTile: Identifiable, Equatable, Sendable {
    public let section: RepoSection
    public let title: String
    public let symbol: String
    public let count: Int?

    public var id: RepoSection { section }

    public init(section: RepoSection, title: String, symbol: String, count: Int? = nil) {
        self.section = section
        self.title = title
        self.symbol = symbol
        self.count = count
    }
}

@MainActor
@Observable
public final class RepoOverviewModel {
    public static let order: [(RepoSection, String, String)] = [
        (.code, "Code", "chevron.left.forwardslash.chevron.right"),
        (.pulls, "Pull requests", "arrow.triangle.pull"),
        (.issues, "Issues", "exclamationmark.circle"),
        (.commits, "Commits", "clock.arrow.circlepath"),
        (.branches, "Branches", "arrow.triangle.branch"),
        (.agents, "Agents", "sparkles"),
        (.worktrees, "Worktrees", "square.stack.3d.up"),
        (.actions, "Actions", "play.rectangle"),
        (.releases, "Releases", "shippingbox"),
        (.discussions, "Discussions", "bubble.left.and.bubble.right"),
        (.mirrors, "Mirrors", "externaldrive.connected.to.line.below"),
        (.projects, "Projects", "calendar"),
        (.security, "Security", "lock.shield"),
        (.insights, "Insights", "chart.bar"),
        (.artifacts, "Artifacts", "archivebox"),
        (.about, "About", "info.circle"),
        (.settings, "Settings", "gearshape"),
    ]

    public private(set) var repo: Repository
    public private(set) var counts: [RepoSection: Int] = [:]
    public private(set) var loading = false

    @ObservationIgnored private let client: RepoOverviewClient?

    public init(repo: Repository, client: RepoOverviewClient?) {
        self.repo = repo
        self.client = client
        if repo.mirrors > 0 { counts[.mirrors] = repo.mirrors }
    }

    public static func title(of section: RepoSection) -> String {
        order.first { $0.0 == section }?.1 ?? section.rawValue.capitalized
    }

    public func tiles(canEditSettings: Bool) -> [RepoOverviewTile] {
        RepoOverviewModel.order
            .filter { canEditSettings || $0.0 != .settings }
            .map { RepoOverviewTile(section: $0.0, title: $0.1, symbol: $0.2, count: counts[$0.0]) }
    }

    public func adopt(_ catalog: [Repository]) {
        let key = "\(repo.owner)/\(repo.name)".lowercased()
        guard let found = catalog.first(where: { "\($0.owner)/\($0.name)".lowercased() == key }) else { return }
        repo = found
        if found.mirrors > 0 { counts[.mirrors] = found.mirrors }
    }

    public func load() async {
        guard let client else { return }
        loading = true
        defer { loading = false }
        let owner = repo.owner
        let name = repo.name
        async let issues = try? client.publishedIssues(owner, name)
        async let pulls = try? client.publishedPulls(owner, name)
        async let branches = try? client.branches(owner, name)
        async let releases = try? client.releases(owner, name)
        async let commits = try? client.commits(owner, name)
        if let list = await issues { counts[.issues] = list.filter(\.isOpen).count }
        if let list = await pulls { counts[.pulls] = list.filter(PullStatusFilter.open.matches).count }
        if let list = await branches { counts[.branches] = list.count }
        if let list = await releases { counts[.releases] = list.count }
        if let list = await commits { counts[.commits] = list.count }
    }
}

public final class CurrentRepoMemory: @unchecked Sendable {
    public static let key = "code/current-repo"

    private let store: KeyValueStore?

    public init(store: KeyValueStore?) {
        self.store = store
    }

    public var slug: String { store?.string(forKey: Self.key) ?? "" }

    public var ref: RepoRef? { RepoRef(slug: slug) }

    public func remember(owner: String, name: String) {
        if owner.isEmpty || name.isEmpty { return }
        store?.set("\(owner)/\(name)", forKey: Self.key)
    }
}
