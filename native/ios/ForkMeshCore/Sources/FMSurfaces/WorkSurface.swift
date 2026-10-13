import FMApi
import FMBase
import Foundation

public struct WorkSurface: Codable, Hashable, Sendable {
    public static let maxRepos = 20
    public static let maxTaskPages = 5

    public var openIssues: Int?
    public var openPulls: Int?
    public var openTasks: Int?
    public var moreTasks: Bool
    public var updatedAtMs: Int64

    public init(
        openIssues: Int? = nil, openPulls: Int? = nil, openTasks: Int? = nil, moreTasks: Bool = false,
        updatedAtMs: Int64 = 0
    ) {
        self.openIssues = openIssues
        self.openPulls = openPulls
        self.openTasks = openTasks
        self.moreTasks = moreTasks
        self.updatedAtMs = updatedAtMs
    }

    public static let empty = WorkSurface()

    public static func repos(_ repos: [Repository], account: String, limit: Int = WorkSurface.maxRepos) -> [RepoRef] {
        let me = account.trimmingCharacters(in: .whitespaces).lowercased()
        guard !me.isEmpty else { return [] }
        let owned = repos.filter { $0.owner.lowercased() == me || $0.displayOwner.lowercased() == me }
            .sorted { a, b in
                if a.updatedMs != b.updatedMs { return a.updatedMs > b.updatedMs }
                return a.name < b.name
            }
        var seen: Set<String> = []
        var refs: [RepoRef] = []
        for repo in owned where refs.count < max(0, limit) {
            let key = "\(repo.owner.lowercased())/\(repo.name.lowercased())"
            if seen.insert(key).inserted {
                refs.append(RepoRef(owner: repo.owner, name: repo.name))
            }
        }
        return refs
    }

    public static func openIssues(_ lists: [[Issue]?]) -> Int? {
        total(lists) { $0.isOpen }
    }

    public static func openPulls(_ lists: [[PullRequest]?]) -> Int? {
        total(lists) { $0.status.lowercased() == "open" }
    }

    static func total<T>(_ lists: [[T]?], _ open: (T) -> Bool) -> Int? {
        let loaded = lists.compactMap { $0 }
        if loaded.isEmpty && !lists.isEmpty { return nil }
        return loaded.reduce(0) { $0 + $1.filter(open).count }
    }

    public struct TaskPage: Hashable, Sendable {
        public var open: Int
        public var nextOffset: Int?
    }

    public static func taskPage(_ data: JSONObject) -> TaskPage {
        let tasks = (data["tasks"]?.arrayValue ?? []).compactMap(\.objectValue)
        let open = tasks.filter { ($0["status"]?.stringValue ?? "") != "done" }.count
        let next = data["hasMore"]?.boolValue == true ? data["nextOffset"]?.intValue : nil
        return TaskPage(open: open, nextOffset: next)
    }

    public static func label(_ value: Int?, more: Bool = false) -> String {
        guard let value else { return "–" }
        return more ? "\(value)+" : "\(value)"
    }
}
