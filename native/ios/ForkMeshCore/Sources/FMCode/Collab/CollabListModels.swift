// View models of the three collaboration tabs: issues (with the filter bar,
// status chip and filter pill state), pulls and discussions.

import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

extension CollabAccount {
    /// `context.watch<AuthService?>()?.session`.
    public init(session: AuthSession?) {
        self.init(nodeName: session?.nodeName, pubkey: session?.pubkey)
    }
}

/// The issue filters of each repository for as long as the app runs, held
/// here so they survive switching tabs even when the tab is rebuilt.
@MainActor
public enum IssueFilterMemory {
    private static var saved: [String: IssueFilters] = [:]

    static func key(_ repo: Repository) -> String { "\(repo.owner)/\(repo.name)" }

    public static func filters(for repo: Repository) -> IssueFilters { saved[key(repo)] ?? IssueFilters() }

    public static func remember(_ filters: IssueFilters, for repo: Repository) { saved[key(repo)] = filters }

    public static func forgetAll() { saved.removeAll() }
}

/// `_IssuesTab`: the published issues, the filter bar and the visible rows.
@MainActor
@Observable
public final class RepoIssuesModel {
    public static let emptyList = "No published issues yet."
    public static let noMatch = "No issues match these filters. Clear them to see the whole list again."
    public static let clearLabel = "Clear issue filters"

    public let repo: Repository
    public private(set) var state: CollabLoad<[Issue]> = .loading
    /// The selection as the user made it; `effectiveFilters` is what applies.
    public private(set) var filters: IssueFilters

    @ObservationIgnored private let api: CollabAPI
    @ObservationIgnored private let account: @MainActor () -> CollabAccount
    @ObservationIgnored private var started = false

    public init(
        repo: Repository, api: CollabAPI, account: @escaping @MainActor () -> CollabAccount = { .signedOut },
        filters: IssueFilters? = nil
    ) {
        self.repo = repo
        self.api = api
        self.account = account
        self.filters = filters ?? IssueFilterMemory.filters(for: repo)
    }

    /// First appearance: loads once per tab.
    public func start() async {
        if started { return }
        started = true
        await load()
    }

    public func load() async {
        do {
            state = .ready(try await api.publishedIssues(repo.owner, repo.name))
        } catch {
            state = .failed(dartErrorText(error))
        }
    }

    /// `session.nodeName.trim()`.
    public var accountName: String { account().filterAccount }

    public var issues: [Issue] { state.value ?? [] }

    /// Stale selections dropped (`_normalizeIssueFilters`).
    public var effectiveFilters: IssueFilters { normalizeIssueFilters(filters, issues, account: accountName) }

    public var visible: [Issue] {
        let applied = effectiveFilters
        let account = accountName
        return issues.filter { issueMatchesFilters($0, applied, account: account) }
    }

    /// "$visibleCount of ${issues.length}".
    public var countLabel: String { "\(visible.count) of \(issues.count)" }

    public var status: IssueStatusFilter { filters.status }

    public var hasMetadataFilter: Bool { effectiveFilters.hasMetadataFilter }

    public func options(_ field: IssueFilterField) -> [IssueFilterOption] {
        issueFilterOptions(field, issues, account: accountName)
    }

    /// The option the pill shows as chosen (the "any" entry when none is).
    public func selection(_ field: IssueFilterField) -> IssueFilterOption {
        issueFilterSelection(options(field), effectiveFilters.valueOf(field))
    }

    /// The pill reads as active (accent) while a value is chosen.
    public func isActive(_ field: IssueFilterField) -> Bool { !selection(field).value.isEmpty }

    /// Pill caption: the chosen label, or the field title.
    public func pillLabel(_ field: IssueFilterField) -> String {
        let chosen = selection(field)
        return chosen.value.isEmpty ? field.title : chosen.label
    }

    /// The menu opens only with something to pick (`enabled: options.length > 1`).
    public func pillEnabled(_ field: IssueFilterField) -> Bool { options(field).count > 1 }

    public func select(_ field: IssueFilterField, _ value: String) {
        update(effectiveFilters.selecting(field, value))
    }

    public func setStatus(_ status: IssueStatusFilter) {
        update(effectiveFilters.withStatus(status))
    }

    public func clearMetadata() {
        update(effectiveFilters.clearingMetadata())
    }

    private func update(_ next: IssueFilters) {
        filters = next
        IssueFilterMemory.remember(next, for: repo)
    }
}

/// `_PullsTab`.
@MainActor
@Observable
public final class RepoPullsModel {
    public static let emptyList = "No published pull requests yet."

    public let repo: Repository
    public private(set) var state: CollabLoad<[PublishedPull]> = .loading
    public var status: PullStatusFilter = .open

    public var visible: [PublishedPull] { (state.value ?? []).filter { status.matches($0) } }

    public func count(_ filter: PullStatusFilter) -> Int { (state.value ?? []).filter { filter.matches($0) }.count }

    public var noMatch: String {
        switch status {
        case .open: return "No open pull requests."
        case .closed: return "No closed or merged pull requests."
        case .all: return Self.emptyList
        }
    }

    @ObservationIgnored private let api: CollabAPI

    public init(repo: Repository, api: CollabAPI) {
        self.repo = repo
        self.api = api
    }

    public func start() async {
        await load()
    }

    public func load() async {
        do {
            state = .ready(try await api.publishedPulls(repo.owner, repo.name))
        } catch {
            state = .failed(dartErrorText(error))
        }
    }
}

/// `_DiscussionsTab`.
@MainActor
@Observable
public final class RepoDiscussionsModel {
    public static let emptyList = "No discussions have been published yet."

    public let repo: Repository
    public private(set) var state: CollabLoad<[RepoDiscussion]> = .loading

    @ObservationIgnored private let api: CollabAPI
    @ObservationIgnored private var started = false

    public init(repo: Repository, api: CollabAPI) {
        self.repo = repo
        self.api = api
    }

    public func start() async {
        if started { return }
        started = true
        await load()
    }

    public func load() async {
        do {
            state = .ready(try await api.publishedDiscussions(repo.owner, repo.name))
        } catch {
            state = .failed(dartErrorText(error))
        }
    }
}
