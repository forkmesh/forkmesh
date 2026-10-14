import FMBase
import Foundation

public struct ParsedLink: Equatable, Sendable {
    public var project: String?
    public var stack: [Route]
    public var explicitProject: Bool

    public init(project: String? = nil, stack: [Route], explicitProject: Bool = true) {
        self.project = project
        self.stack = stack
        self.explicitProject = explicitProject
    }

    public var destination: Route? { stack.last }
}

public enum RouteParser {
    public static let universalLinkHosts: [String] = [
        "app.next.forkmesh.com", "next.forkmesh.com", "app.forkmesh.com", "forkmesh.com",
    ]
    public static let customScheme = "forkmesh"

    static let reservedOwners: Set<String> = [
        "api", "dashboard", "assets", "static", ".well-known", "docs", "login", "logout", "signup", "auth",
        "admin", "world", "legal", "blog", "download", "downloads", "pricing", "help", "raw", "ws", "org", "orgs", "oauth",
        "settings", "status", "ledger", "market", "search", "explore", "new", "privacy", "terms",
    ]

    private static let safeSegment = try! NSRegularExpression(pattern: "^[A-Za-z0-9._-]+$")
    private static let commitHash = try! NSRegularExpression(pattern: "^[0-9a-f]{7,64}$")

    public static func isUniversalLinkHost(_ host: String) -> Bool {
        universalLinkHosts.contains(host.lowercased())
    }

    public static func projectId(forHost host: String) -> String {
        let lowered = host.lowercased()
        return AppEnv.canonicalProjectId(lowered.hasPrefix("app.") ? lowered : "app.\(lowered)")
    }

    public static func parse(_ url: URL, me: String = "") -> ParsedLink? {
        guard let components = URLComponents(url: url, resolvingAgainstBaseURL: false) else { return nil }
        let scheme = components.scheme?.lowercased() ?? ""
        let query = queryItems(components)
        let explicitProject = query["project"].flatMap(normalizedProject)
        let pathSegments = segments(components.percentEncodedPath)
        if scheme == customScheme {
            guard let host = components.host, !host.isEmpty else { return nil }
            let all = [host] + pathSegments
            guard let stack = customRoutes(all, query: query, me: me) else { return nil }
            return ParsedLink(project: explicitProject, stack: stack)
        }
        guard scheme == "https", let host = components.host, isUniversalLinkHost(host) else { return nil }
        guard let stack = routes(segments: pathSegments, query: query, me: me) else { return nil }
        return ParsedLink(
            project: explicitProject ?? projectId(forHost: host), stack: stack, explicitProject: explicitProject != nil)
    }

    public static func routes(segments: [String], query: [String: String] = [:], me: String = "") -> [Route]? {
        guard let first = segments.first else { return nil }
        if first.hasPrefix("@") {
            let name = String(first.dropFirst())
            guard segments.count == 1, isSafe(name) else { return nil }
            return [.profile(name: name)]
        }
        switch first.lowercased() {
        case "dashboard": return dashboard(Array(segments.dropFirst()), query: query, me: me)
        case "ledger.html": return segments.count == 1 ? [.ledger(section: nil)] : nil
        case "market.html": return segments.count == 1 ? [.market(filter: nil)] : nil
        case "status.html": return segments.count == 1 ? [.status] : nil
        default: break
        }
        guard segments.count >= 2 else { return nil }
        let owner = segments[0]
        let name = segments[1]
        guard isSafe(owner), isSafe(name), !reservedOwners.contains(owner.lowercased()) else { return nil }
        return repoRoutes(RepoRef(owner: owner, name: name), Array(segments.dropFirst(2)))
    }

    public static func stack(for route: Route) -> [Route] {
        switch route {
        case let .repo(repo, section):
            return section == nil ? [route] : [.repo(repo, section: nil), route]
        case let .tree(repo, _, _), let .codeMap(repo, _), let .commits(repo, _):
            return [.repo(repo, section: nil), route]
        case let .file(repo, ref, path):
            return [.repo(repo, section: nil), .tree(repo, ref: ref, path: parentPath(path)), route]
        case let .fileHistory(repo, ref, path):
            return [.repo(repo, section: nil), .file(repo, ref: ref, path: path), route]
        case let .commit(repo, _):
            return sectionStack(repo, .commits, route)
        case let .range(repo, _, _), let .compare(repo, _, _), let .conflicts(repo, _):
            return sectionStack(repo, .branches, route)
        case let .issue(repo, _), let .newIssue(repo), let .board(repo):
            return sectionStack(repo, .issues, route)
        case let .pull(repo, _, _):
            return sectionStack(repo, .pulls, route)
        case let .review(repo, number):
            return sectionStack(repo, .pulls, .pull(repo, number: number, tab: .conversation)) + [route]
        case let .discussion(repo, _):
            return sectionStack(repo, .discussions, route)
        case let .run(repo, _):
            return sectionStack(repo, .actions, route)
        case let .release(repo, _):
            return sectionStack(repo, .releases, route)
        case let .project(repo, _), let .milestones(repo):
            return sectionStack(repo, .projects, route)
        case let .repoSettings(repo, _):
            return sectionStack(repo, .settings, route)
        case let .thread(channel, _):
            return [.channel(id: channel), route]
        case .teammate:
            return [.teammates, route]
        case let .teammateEditor(id):
            if let id = id, !id.isEmpty { return [.teammates, .teammate(id: id), route] }
            return [.teammates, route]
        case .note:
            return [.notes, route]
        case .task:
            return [.tasks(filter: nil), route]
        case .host, .provision:
            return [.hosts, route]
        case .node:
            return [.network(section: nil), route]
        case .package:
            return [.market(filter: nil), route]
        case .contact, .crmOrg:
            return [.crm(pipeline: nil), route]
        case .org:
            return [.orgs, route]
        case .projectAccess:
            return [.projects, route]
        case .desktop:
            return [.desktops, route]
        case .localRepo:
            return [.localRepos, route]
        case .approval:
            return [.inbox(.approvals), route]
        default:
            return [route]
        }
    }

    private static func sectionStack(_ repo: RepoRef, _ section: RepoSection, _ route: Route) -> [Route] {
        [.repo(repo, section: nil), .repo(repo, section: section), route]
    }

    private static func parentPath(_ path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return "" }
        return String(path[..<slash])
    }

    private static func repoRoutes(_ repo: RepoRef, _ rest: [String]) -> [Route]? {
        let root: Route = .repo(repo, section: nil)
        guard let head = rest.first else { return [root] }
        let tail = Array(rest.dropFirst())
        func section(_ value: RepoSection) -> [Route] { [root, .repo(repo, section: value)] }
        func number(_ index: Int) -> Int? {
            guard tail.count > index, let value = Int(tail[index]), value > 0 else { return nil }
            return value
        }
        switch head {
        case "tree":
            guard let ref = tail.first, !ref.isEmpty else { return section(.code) }
            return [root, .tree(repo, ref: ref, path: tail.dropFirst().joined(separator: "/"))]
        case "blob":
            guard tail.count >= 2 else { return nil }
            let path = tail.dropFirst().joined(separator: "/")
            return stack(for: .file(repo, ref: tail[0], path: path))
        case "issues":
            if tail.first == "new" { return section(.issues) + [.newIssue(repo)] }
            if let value = number(0) { return section(.issues) + [.issue(repo, number: value)] }
            return tail.isEmpty ? section(.issues) : nil
        case "pulls", "pull":
            if let value = number(0) {
                let tab = tail.count > 1 ? PullTab(rawValue: tail[1]) ?? .conversation : .conversation
                return section(.pulls) + [.pull(repo, number: value, tab: tab)]
            }
            return tail.isEmpty ? section(.pulls) : nil
        case "discussions":
            if let value = number(0) { return section(.discussions) + [.discussion(repo, number: value)] }
            return tail.isEmpty ? section(.discussions) : nil
        case "commit":
            guard let sha = tail.first, !sha.isEmpty else { return section(.commits) }
            return section(.commits) + [.commit(repo, sha: sha)]
        case "commits":
            let named = tail.joined(separator: "/")
            if named.isEmpty { return section(.commits) }
            if matches(commitHash, named) { return section(.commits) + [.commit(repo, sha: named)] }
            return [root, .commits(repo, ref: named)]
        case "actions":
            if tail.count >= 2, tail[0] == "runs", !tail[1].isEmpty {
                return section(.actions) + [.run(repo, id: tail[1])]
            }
            return section(.actions)
        case "releases":
            if tail.count >= 2, tail[0] == "tag" {
                return section(.releases) + [.release(repo, tag: tail.dropFirst().joined(separator: "/"))]
            }
            return section(.releases)
        case "compare":
            let spec = tail.joined(separator: "/")
            let parts = spec.components(separatedBy: "...")
            guard parts.count == 2, !parts[0].isEmpty, !parts[1].isEmpty else { return section(.branches) }
            return section(.branches) + [.compare(repo, base: parts[0], head: parts[1])]
        case "settings":
            if let first = tail.first, !first.isEmpty { return section(.settings) + [.repoSettings(repo, section: first)] }
            return section(.settings)
        case "projects":
            if let id = tail.first, !id.isEmpty { return section(.projects) + [.project(repo, id: id)] }
            return section(.projects)
        case "milestones":
            return section(.projects) + [.milestones(repo)]
        case "board":
            return section(.issues) + [.board(repo)]
        case "shortcuts":
            return tail.isEmpty ? [.shortcuts(repo)] : nil
        default:
            guard let value = RepoSection(rawValue: head), tail.isEmpty else { return nil }
            return section(value)
        }
    }

    private static func dashboard(_ rest: [String], query: [String: String], me: String) -> [Route]? {
        guard let page = rest.first else { return nil }
        let tail = Array(rest.dropFirst())
        let first: String? = tail.first.flatMap { $0.isEmpty ? nil : $0 }
        switch page {
        case "chat": return [.chat]
        case "notes":
            if let id = first { return [.notes, .note(id: id)] }
            return [.notes]
        case "tasks":
            if let id = first { return [.tasks(filter: nil), .task(id: id)] }
            return [.tasks(filter: query["filter"])]
        case "calendar":
            let mode = CalendarMode(rawValue: query["mode"] ?? "") ?? .month
            return [.calendar(date: query["date"], mode: mode)]
        case "settings": return [.settings(section: first)]
        case "network": return [.network(section: first)]
        case "ads": return [.ads]
        case "apps": return tail.isEmpty ? [.settings(section: "apps")] : nil
        case "calories": return [.calories]
        case "steps": return [.steps]
        case "water": return [.water]
        case "sleep": return [.sleep]
        case "sugar-break": return [.sugarBreak]
        case "stretch": return [.stretch]
        case "food-guide": return [.foodGuide]
        case "speech": return [.speech]
        case "glasses-alert": return [.glassesAlert]
        case "face-changer": return [.faceChanger]
        case "people": return [.people]
        case "docs": return [.docs]
        case "repos": return [.repos(.all)]
        case "profile": return me.isEmpty ? [.settings(section: "profile")] : [.profile(name: me)]
        case "agents": return [.agents(.all)]
        case "inbox", "pings", "notifications": return [.inbox(.all)]
        default: return nil
        }
    }

    private static func customRoutes(_ all: [String], query: [String: String], me: String) -> [Route]? {
        guard let head = all.first?.lowercased() else { return nil }
        let tail = Array(all.dropFirst())
        let rest = tail.joined(separator: "/")
        switch head {
        case "ios-udid", "surface":
            return nil
        case "agents":
            if !rest.isEmpty { return [.agents(.all), .agent(id: rest)] }
            return [.agents(AgentFilter(rawValue: query["filter"] ?? "") ?? .all)]
        case "approvals":
            if !rest.isEmpty { return [.inbox(.approvals), .approval(id: rest)] }
            return [.inbox(.approvals)]
        case "compose":
            let target = ComposeTarget(
                repo: query["repo"].flatMap(RepoRef.init(slug:)),
                issue: query["issue"].flatMap { Int($0) },
                pull: query["pull"].flatMap { Int($0) },
                agentId: nonEmpty(query["agent"]),
                channel: nonEmpty(query["channel"]),
                text: nonEmpty(query["text"]))
            return [.compose(target)]
        case "search":
            return [.search(query: query["q"] ?? "", scope: searchScope(query["scope"]))]
        case "inbox":
            return [.inbox(InboxFilter(rawValue: tail.first ?? "") ?? .all)]
        default:
            return routes(segments: all, query: query, me: me)
        }
    }

    public static func searchScope(_ raw: String?) -> SearchScope {
        guard let raw = raw?.lowercased(), !raw.isEmpty else { return .everything }
        if raw == "all" { return .everything }
        return SearchScope(rawValue: raw) ?? .everything
    }

    private static func nonEmpty(_ value: String?) -> String? {
        guard let value = value, !value.isEmpty else { return nil }
        return value
    }

    private static func normalizedProject(_ raw: String) -> String? {
        let trimmed = raw.trimmingCharacters(in: .whitespaces).lowercased()
        return trimmed.isEmpty ? nil : AppEnv.canonicalProjectId(trimmed)
    }

    private static func queryItems(_ components: URLComponents) -> [String: String] {
        var result: [String: String] = [:]
        for item in components.queryItems ?? [] where result[item.name] == nil {
            result[item.name] = item.value ?? ""
        }
        return result
    }

    private static func segments(_ encodedPath: String) -> [String] {
        encodedPath.split(separator: "/").map { String($0).removingPercentEncoding ?? String($0) }
    }

    private static func isSafe(_ segment: String) -> Bool {
        segment != "." && segment != ".." && matches(safeSegment, segment)
    }

    private static func matches(_ regex: NSRegularExpression, _ text: String) -> Bool {
        regex.firstMatch(in: text, range: NSRange(text.startIndex..., in: text)) != nil
    }
}

extension NotificationDeepLink {
    public var route: Route { InboxRoutes.route(self) }

    public var routeStack: [Route] { RouteParser.stack(for: route) }
}
