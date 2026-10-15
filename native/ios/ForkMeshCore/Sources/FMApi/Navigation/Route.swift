// Gate 2 (docs/native/DESKTOP_PARITY.md §19.2, docs/native/NAVIGATION.md §8.1): every destination
// in the app as one `Codable, Hashable` value. Screens push routes, never views; features register
// a destination for a route's `kind` in `RouteRegistry` from their installer. WS8 owns this file
// after Gate 2; a workstream that needs a new case asks the coordinator (or WS8).

import Foundation

/// A repository, by owner and name.
public struct RepoRef: Codable, Hashable, Sendable, CustomStringConvertible {
    public let owner: String
    public let name: String

    public init(owner: String, name: String) {
        self.owner = owner
        self.name = name
    }

    /// "owner/name"; nil unless there are exactly two non-empty parts.
    public init?(slug: String) {
        let parts = slug.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        guard parts.count == 2, !parts[0].isEmpty, !parts[1].isEmpty else { return nil }
        self.init(owner: parts[0], name: parts[1])
    }

    public var slug: String { "\(owner)/\(name)" }
    public var description: String { slug }
}

/// The rows of a repository's root list (NAVIGATION.md §3.4): the desktop's repository tabs.
public enum RepoSection: String, Codable, Hashable, Sendable, CaseIterable {
    case code, issues, pulls, discussions, actions, agents, worktrees, releases, branches, commits
    case projects, security, insights, quality, mirrors, artifacts, settings, about
}

/// The tabs of a pull request screen.
public enum PullTab: String, Codable, Hashable, Sendable, CaseIterable {
    case conversation, commits, files, checks
}

/// Which agents the Agents root shows.
public enum AgentFilter: String, Codable, Hashable, Sendable, CaseIterable {
    case all, desktop, cloud, needsYou, running, queued, recent
}

/// Which items the Inbox shows.
public enum InboxFilter: String, Codable, Hashable, Sendable, CaseIterable {
    case all, pings, reviews, assigned, mentions, approvals
}

/// Which repositories the Code root lists.
public enum RepoScope: String, Codable, Hashable, Sendable, CaseIterable {
    case all, pinned, mine, organizations, network, onDevice
}

/// What a search looks through.
public enum SearchScope: String, Codable, Hashable, Sendable, CaseIterable {
    case everything, repos, code, issues, pulls, people, notes, commands, chat
}

/// The calendar's layout.
public enum CalendarMode: String, Codable, Hashable, Sendable, CaseIterable {
    case day, week, month, agenda
}

/// Where the composer starts an agent, and on what.
public struct ComposeTarget: Codable, Hashable, Sendable {
    public var repo: RepoRef?
    public var issue: Int?
    public var pull: Int?
    public var agentId: String?
    public var channel: String?
    public var text: String?
    public var onDevice: Bool?

    public init(repo: RepoRef? = nil, issue: Int? = nil, pull: Int? = nil, agentId: String? = nil,
                channel: String? = nil, text: String? = nil, onDevice: Bool? = nil) {
        self.repo = repo
        self.issue = issue
        self.pull = pull
        self.agentId = agentId
        self.channel = channel
        self.text = text
        self.onDevice = onDevice
    }
}

/// The four tabs (NAVIGATION.md §2). The prompt button and the You hub are not tabs.
public enum AppTab: String, Codable, Hashable, Sendable, CaseIterable {
    case agents, inbox, code, chat
}

/// Every destination (NAVIGATION.md §8.1). Sections that only group rows take a free-form
/// `section` string, so a workstream can add sections without a contract change.
public enum Route: Codable, Hashable, Sendable {
    // Agents (WS10, WS7, WS4, WS17)
    case agents(AgentFilter)
    case agent(id: String)
    case approval(id: String)
    case compose(ComposeTarget)
    case agentLogin(target: String)
    case agentWatch(AgentFilter)
    case allAgents
    case launchedAgent(RepoRef, sessionId: String)
    // Teammates (WS17)
    case teammates
    case teammate(id: String)
    case teammateEditor(id: String?)
    case shortcuts(RepoRef?)
    // Inbox (WS9, WS2)
    case inbox(InboxFilter)
    case pingSettings
    // Code (WS11, WS14, WS15, WS20, WS21, WS5)
    case repos(RepoScope)
    case repo(RepoRef, section: RepoSection?)
    case tree(RepoRef, ref: String, path: String)
    case file(RepoRef, ref: String, path: String)
    case fileHistory(RepoRef, ref: String, path: String)
    case codeMap(RepoRef, ref: String)
    case commits(RepoRef, ref: String)
    case commit(RepoRef, sha: String)
    case range(RepoRef, base: String, head: String)
    case compare(RepoRef, base: String, head: String)
    case conflicts(RepoRef, branch: String)
    case issue(RepoRef, number: Int)
    case newIssue(RepoRef)
    case board(RepoRef)
    case pull(RepoRef, number: Int, tab: PullTab)
    case review(RepoRef, number: Int)
    case discussion(RepoRef, number: Int)
    case run(RepoRef, id: String)
    case release(RepoRef, tag: String)
    case project(RepoRef, id: String)
    case milestones(RepoRef)
    case repoSettings(RepoRef, section: String?)
    case localRepos
    case localRepo(id: String)
    case quickEdit(repoId: String, path: String)
    // Chat (WS13)
    case chat
    case channel(id: String)
    case thread(channel: String, id: String)
    case dm(user: String)
    case discord(guild: String, channel: String?)
    // People and search (WS18, WS8)
    case profile(name: String)
    case people
    case docs
    case search(query: String, scope: SearchScope)
    // Planning (WS15)
    case notes
    case note(id: String)
    case tasks(filter: String?)
    case task(id: String)
    case calendar(date: String?, mode: CalendarMode)
    case event(id: String)
    case timers
    // Desktop, hosts and network (WS16, WS7, WS20)
    case desktops
    case desktop(id: String, section: String?)
    case hosts
    case host(name: String, section: String?)
    case provision(id: String)
    case network(section: String?)
    case node(name: String)
    // Business packages (WS19)
    case market(filter: String?)
    case package(id: String)
    case crm(pipeline: String?)
    case contact(id: String)
    case crmOrg(id: String)
    case marketing(section: String?)
    case analytics(section: String?)
    case competitors
    case ads
    case ledger(section: String?)
    // Personal (WS21 and existing screens)
    case vault(section: String?)
    case coves(RepoRef?)
    case cove(id: String)
    case calories
    case steps
    case water
    case sleep
    case sugarBreak
    case stretch
    case foodGuide
    case speech
    case glassesAlert
    case faceChanger
    case appStoreStatus
    case rssReader
    case localAI
    case activity
    // Insights and admin (WS20)
    case stats
    case status
    case admin(section: String?)
    // App (WS18, WS1, WS8)
    case settings(section: String?)
    case orgs
    case org(name: String)
    case projects
    case projectAccess(id: String)
    case tour
    case updateRequired

    /// The case without its payload: the key destinations are registered under.
    public var kind: Kind {
        switch self {
        case .agents: return .agents
        case .agent: return .agent
        case .approval: return .approval
        case .compose: return .compose
        case .agentLogin: return .agentLogin
        case .agentWatch: return .agentWatch
        case .allAgents: return .allAgents
        case .launchedAgent: return .launchedAgent
        case .teammates: return .teammates
        case .teammate: return .teammate
        case .teammateEditor: return .teammateEditor
        case .shortcuts: return .shortcuts
        case .inbox: return .inbox
        case .pingSettings: return .pingSettings
        case .repos: return .repos
        case .repo: return .repo
        case .tree: return .tree
        case .file: return .file
        case .fileHistory: return .fileHistory
        case .codeMap: return .codeMap
        case .commits: return .commits
        case .commit: return .commit
        case .range: return .range
        case .compare: return .compare
        case .conflicts: return .conflicts
        case .issue: return .issue
        case .newIssue: return .newIssue
        case .board: return .board
        case .pull: return .pull
        case .review: return .review
        case .discussion: return .discussion
        case .run: return .run
        case .release: return .release
        case .project: return .project
        case .milestones: return .milestones
        case .repoSettings: return .repoSettings
        case .localRepos: return .localRepos
        case .localRepo: return .localRepo
        case .quickEdit: return .quickEdit
        case .chat: return .chat
        case .channel: return .channel
        case .thread: return .thread
        case .dm: return .dm
        case .discord: return .discord
        case .profile: return .profile
        case .people: return .people
        case .docs: return .docs
        case .search: return .search
        case .notes: return .notes
        case .note: return .note
        case .tasks: return .tasks
        case .task: return .task
        case .calendar: return .calendar
        case .event: return .event
        case .timers: return .timers
        case .desktops: return .desktops
        case .desktop: return .desktop
        case .hosts: return .hosts
        case .host: return .host
        case .provision: return .provision
        case .network: return .network
        case .node: return .node
        case .market: return .market
        case .package: return .package
        case .crm: return .crm
        case .contact: return .contact
        case .crmOrg: return .crmOrg
        case .marketing: return .marketing
        case .analytics: return .analytics
        case .competitors: return .competitors
        case .ads: return .ads
        case .ledger: return .ledger
        case .vault: return .vault
        case .coves: return .coves
        case .cove: return .cove
        case .calories: return .calories
        case .steps: return .steps
        case .water: return .water
        case .sleep: return .sleep
        case .sugarBreak: return .sugarBreak
        case .stretch: return .stretch
        case .foodGuide: return .foodGuide
        case .speech: return .speech
        case .glassesAlert: return .glassesAlert
        case .faceChanger: return .faceChanger
        case .appStoreStatus: return .appStoreStatus
        case .rssReader: return .rssReader
        case .localAI: return .localAI
        case .activity: return .activity
        case .stats: return .stats
        case .status: return .status
        case .admin: return .admin
        case .settings: return .settings
        case .orgs: return .orgs
        case .org: return .org
        case .projects: return .projects
        case .projectAccess: return .projectAccess
        case .tour: return .tour
        case .updateRequired: return .updateRequired
        }
    }

    /// The case names, for registration and for the "Coming soon" placeholder.
    public enum Kind: String, Codable, Hashable, Sendable, CaseIterable {
        case agents, agent, approval, compose, agentLogin, agentWatch, allAgents, launchedAgent
        case teammates, teammate, teammateEditor, shortcuts
        case inbox, pingSettings
        case repos, repo, tree, file, fileHistory, codeMap, commits, commit, range, compare, conflicts
        case issue, newIssue, board, pull, review, discussion, run, release, project, milestones
        case repoSettings, localRepos, localRepo, quickEdit
        case chat, channel, thread, dm, discord
        case profile, people, docs, search
        case notes, note, tasks, task, calendar, event, timers
        case desktops, desktop, hosts, host, provision, network, node
        case market, package, crm, contact, crmOrg, marketing, analytics, competitors, ads, ledger
        case vault, coves, cove, calories, steps, water, sleep, sugarBreak, stretch, foodGuide, speech, glassesAlert, faceChanger, appStoreStatus, rssReader, localAI, activity
        case stats, status, admin
        case settings, orgs, org, projects, projectAccess, tour, updateRequired

        /// The workstream that builds this destination (NAVIGATION.md §8.1), for the placeholder.
        public var owner: String {
            switch self {
            case .agents, .agent, .approval, .agentLogin: return "WS10"
            case .compose: return "WS3"
            case .agentWatch: return "WS4"
            case .teammates, .teammate, .teammateEditor, .shortcuts: return "WS17"
            case .allAgents, .hosts, .host: return "WS7"
            case .inbox: return "WS9"
            case .pingSettings: return "WS2"
            case .repos, .repo, .tree, .file, .fileHistory, .codeMap, .commits, .commit, .range, .compare,
                .conflicts, .issue, .newIssue, .board, .pull, .review, .discussion, .repoSettings:
                return "WS11"
            case .run, .release: return "WS14"
            case .project, .milestones, .notes, .note, .tasks, .task, .calendar, .event, .timers: return "WS15"
            case .localRepos, .localRepo, .quickEdit: return "WS5"
            case .chat, .channel, .thread, .dm, .discord: return "WS13"
            case .profile, .people, .docs, .settings: return "WS18"
            case .search, .tour, .updateRequired, .launchedAgent: return "WS8"
            case .desktops, .desktop, .provision, .node: return "WS16"
            case .network, .stats, .status, .admin, .activity, .appStoreStatus: return "WS20"
            case .market, .package, .crm, .contact, .crmOrg, .marketing, .analytics, .competitors, .ads,
                .ledger:
                return "WS19"
            case .vault, .coves, .cove: return "WS21"
            case .calories, .steps, .water, .sleep, .sugarBreak, .stretch, .foodGuide, .speech, .glassesAlert, .faceChanger, .rssReader, .localAI, .orgs,
                .org:
                return "existing"
            case .projects, .projectAccess: return "WS1"
            }
        }
    }

    /// The tab a deep link to this route opens on (NAVIGATION.md §8.1). Nil: the current tab
    /// (hub destinations and anything reached from the You hub).
    public var homeTab: AppTab? {
        switch kind {
        case .agents, .agent, .agentWatch, .allAgents, .launchedAgent: return .agents
        case .approval, .inbox: return .inbox
        case .repos, .repo, .tree, .file, .fileHistory, .codeMap, .commits, .commit, .range, .compare,
            .conflicts, .issue, .newIssue, .board, .pull, .review, .discussion, .run, .release, .project,
            .milestones, .repoSettings, .localRepos, .localRepo, .quickEdit:
            return .code
        case .chat, .channel, .thread, .dm, .discord: return .chat
        default: return nil
        }
    }

    /// Routes that open as a sheet or full-screen cover rather than a push.
    public var isModal: Bool {
        switch kind {
        case .compose, .updateRequired, .agentLogin: return true
        default: return false
        }
    }
}
