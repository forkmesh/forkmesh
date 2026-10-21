import Foundation
import FMBase

/// The tabs of the repository detail screen.
public enum RepoDetailTab: String, CaseIterable, Sendable {
    case code, issues, pulls, discussions, commits, mirrors, releases, about, actions, agents, worktrees
}

/// Where a notification (or any in-app `/owner/repo/...` link) should open:
/// the repository, its tab, and the issue/pull/discussion number or commit
/// reference when the link names one.
public struct NotificationDeepLink: Equatable, Hashable, Sendable {
    public var repo: Repository
    public var initialTab: RepoDetailTab
    public var number: Int
    /// A commit hash (`/owner/repo/commit/<sha>`, or a bare hash under `commits/`).
    public var reference: String
    /// A branch (`/owner/repo/commits/<branch>`, `/tree/<branch>`, or a `?branch=`/`?ref=`
    /// query): what a code-push ping links to. Branch names may hold slashes.
    public var branch: String

    public init(
        repo: Repository, initialTab: RepoDetailTab, number: Int = 0, reference: String = "", branch: String = ""
    ) {
        self.repo = repo
        self.initialTab = initialTab
        self.number = number
        self.reference = reference
        self.branch = branch
    }

    /// A code-push ping's target: the commits tab at a branch.
    public var isBranchCommits: Bool { initialTab == .commits && reference.isEmpty && !branch.isEmpty }

    private static let safeSegment = try! NSRegularExpression(pattern: "^[A-Za-z0-9._-]+$")
    private static let scheme = try! NSRegularExpression(pattern: "^[A-Za-z][A-Za-z0-9+.-]*:")
    private static let commitHash = try! NSRegularExpression(pattern: "^[0-9a-f]{7,64}$")

    /// Nil for anything that is not a relative link into a repository.
    public static func parse(_ href: String) -> NotificationDeepLink? {
        let trimmed = href.dartTrimmed
        if trimmed.isEmpty { return nil }
        // A scheme or an authority means it is not an in-app path.
        if DartRegex.matches(scheme, trimmed) || trimmed.hasPrefix("//") { return nil }
        var path = Substring(trimmed)
        if let hash = path.firstIndex(of: "#") { path = path[..<hash] }
        var query: Substring = ""
        if let mark = path.firstIndex(of: "?") {
            query = path[path.index(after: mark)...]
            path = path[..<mark]
        }
        // Each segment is percent-decoded twice; a malformed escape is refused.
        var parts: [String] = []
        for segment in path.split(separator: "/") {
            guard let once = DartCore.decodeComponent(String(segment)),
                  let twice = DartCore.decodeComponent(once) else { return nil }
            parts.append(twice)
        }
        if parts.count < 2 { return nil }
        let owner = parts[0]
        let name = parts[1]
        if !DartRegex.matches(safeSegment, owner) || !DartRegex.matches(safeSegment, name) { return nil }
        if owner == "api" || owner == "dashboard" { return nil }

        var tab = RepoDetailTab.code
        var number = 0
        var reference = ""
        // Everything from `from` on, as one path (a branch may hold slashes).
        func rest(_ from: Int) -> String { parts.count > from ? parts[from...].joined(separator: "/") : "" }
        var branch = (Self.queryValue(query, "branch") ?? Self.queryValue(query, "ref") ?? "").dartTrimmed
        if parts.count >= 3 {
            switch parts[2] {
            case "issues":
                tab = .issues
                number = Self.number(parts)
            case "pulls", "pull":
                tab = .pulls
                number = Self.number(parts)
            case "discussions":
                tab = .discussions
                number = Self.number(parts)
            case "commit":
                tab = .commits
                reference = parts.count >= 4 ? parts[3] : ""
            case "commits":
                // `commits/<x>` names a branch on the web; a bare hash there is still a commit.
                tab = .commits
                let named = rest(3)
                if DartRegex.matches(commitHash, named) {
                    reference = named
                } else if !named.isEmpty {
                    branch = named
                }
            case "tree":
                tab = .code
                if branch.isEmpty { branch = rest(3) }
            case "mirrors": tab = .mirrors
            case "releases": tab = .releases
            case "about": tab = .about
            case "actions":
                tab = .actions
                if parts.count >= 5, parts[3] == "runs", (DartCore.intTryParse(parts[4]) ?? 0) > 0 {
                    reference = parts[4]
                }
            case "agents":
                tab = .agents
                number = Self.number(parts)
            case "worktrees": tab = .worktrees
            default: tab = .code
            }
        }
        return NotificationDeepLink(
            repo: Repository(owner: owner, name: name), initialTab: tab, number: number, reference: reference,
            branch: branch)
    }

    /// One query parameter, decoded; nil when absent.
    private static func queryValue(_ query: Substring, _ key: String) -> String? {
        for pair in query.split(separator: "&") {
            let parts = pair.split(separator: "=", maxSplits: 1, omittingEmptySubsequences: false)
            guard parts.count == 2, parts[0] == Substring(key) else { continue }
            let raw = parts[1].replacingOccurrences(of: "+", with: " ")
            return DartCore.decodeComponent(raw) ?? raw
        }
        return nil
    }

    private static func number(_ parts: [String]) -> Int {
        parts.count < 4 ? 0 : DartCore.intTryParse(parts[3]) ?? 0
    }
}
