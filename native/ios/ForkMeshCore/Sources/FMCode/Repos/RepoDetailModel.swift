// The repository detail screen: the tab bar (`RepoDetailTab`, in FMApi next to
// NotificationDeepLink), the floating action per tab, and what a notification
// deep link opens on arrival.

import FMApi
import FMBase
import Foundation
import Observation

extension RepoDetailTab {
    /// The tab's label in the tab bar.
    public var tabLabel: String {
        switch self {
        case .code: return "Code"
        case .issues: return "Issues"
        case .pulls: return "Pulls"
        case .discussions: return "Discussions"
        case .commits: return "Commits"
        case .mirrors: return "Mirrors"
        case .releases: return "Releases"
        case .about: return "About"
        case .actions: return "Actions"
        case .agents: return "Agents"
        case .worktrees: return "Worktrees"
        }
    }

    /// `_RepoDetailTabLabel`'s second line, or nil for a one-line tab.
    public var tabCaption: String? {
        switch self {
        case .mirrors: return "replicas"
        case .actions: return "workflows"
        case .agents: return "sessions"
        default: return nil
        }
    }

    /// `_fab()`'s label, or nil where the tab has no floating action.
    public var tabFloatingActionLabel: String? {
        switch self {
        case .issues: return "New issue"
        case .pulls: return "New PR"
        case .discussions: return "New discussion"
        default: return nil
        }
    }
}

/// Where `_openInitialTarget` goes once the screen is up.
public enum RepoDetailDestination: Equatable, Sendable {
    case issue(Int)
    case pull(Int)
    case discussion(Int)
    /// A commit, as the commit map the detail screen reads.
    case commit(JSONObject)
}

/// `_RepoDetailScreenState`.
@MainActor
@Observable
public final class RepoDetailModel {
    public let repo: Repository
    public var tab: RepoDetailTab
    @ObservationIgnored public let initialTarget: NotificationDeepLink?
    @ObservationIgnored private var openedInitialTarget = false

    public init(repo: Repository, initialTab: RepoDetailTab = .code, initialTarget: NotificationDeepLink? = nil) {
        self.repo = repo
        self.tab = initialTab
        self.initialTarget = initialTarget
    }

    public var tabs: [RepoDetailTab] { RepoDetailTab.allCases }

    /// `_openInitialTarget`: what to open over the tab, once. Issues, pulls and
    /// discussions need a number; a commit needs a reference.
    public func takeInitialDestination() -> RepoDetailDestination? {
        if openedInitialTarget { return nil }
        openedInitialTarget = true
        guard let target = initialTarget else { return nil }
        return RepoDetailModel.destination(for: target)
    }

    public static func destination(for target: NotificationDeepLink) -> RepoDetailDestination? {
        switch target.initialTab {
        case .issues:
            return target.number > 0 ? .issue(target.number) : nil
        case .pulls:
            return target.number > 0 ? .pull(target.number) : nil
        case .discussions:
            return target.number > 0 ? .discussion(target.number) : nil
        case .commits:
            let hash = target.reference.dartTrimmed
            if hash.isEmpty { return nil }
            return .commit(["hash": .string(hash), "message": .string("Commit \(hash)")])
        case .code, .mirrors, .releases, .about, .actions, .agents, .worktrees:
            return nil
        }
    }
}
