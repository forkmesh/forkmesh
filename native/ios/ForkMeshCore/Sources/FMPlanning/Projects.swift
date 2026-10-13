import FMApi
import FMBase
import Foundation
import Observation

public struct RepoMilestone: Equatable, Hashable, Sendable, Identifiable {
    public var title: String
    public var dueMs: Int64
    public var status: String
    public var description: String
    public var id: String { title }

    public init(title: String, dueMs: Int64 = 0, status: String = "open", description: String = "") {
        self.title = title
        self.dueMs = dueMs
        self.status = status
        self.description = description
    }

    public var isOpen: Bool { status != "closed" }
}

public struct RepoProject: Equatable, Hashable, Sendable, Identifiable {
    public var number: Int
    public var title: String
    public var body: String
    public var status: String
    public var startMs: Int64
    public var endMs: Int64
    public var milestone: String
    public var issues: [Int]
    public var authorName: String
    public var updatedAtMs: Int64
    public var id: Int { number }

    public init(number: Int, title: String, body: String = "", status: String = "open", startMs: Int64 = 0, endMs: Int64 = 0,
                milestone: String = "", issues: [Int] = [], authorName: String = "", updatedAtMs: Int64 = 0) {
        self.number = number
        self.title = title
        self.body = body
        self.status = status
        self.startMs = startMs
        self.endMs = endMs
        self.milestone = milestone
        self.issues = issues
        self.authorName = authorName
        self.updatedAtMs = updatedAtMs
    }

    public init?(json: JSONObject) {
        let number = json.int("number")
        guard number > 0, json.str("schema", "forkmesh-project-v1") == "forkmesh-project-v1" else { return nil }
        let deleted = json.maps("events").contains { $0.str("type") == "delete" && $0.str("target", "self") == "self" }
        guard !deleted else { return nil }
        self.init(
            number: number, title: json.str("title"), body: json.str("body"), status: json.str("status", "open"),
            startMs: json.int64("startDate"), endMs: json.int64("endDate"), milestone: json.str("milestone"),
            issues: json.list("issues").compactMap(\.intValue).filter { $0 > 0 }, authorName: json.str("authorName"),
            updatedAtMs: json.int64("updatedAt"))
    }

    public var hasDates: Bool { startMs > 0 && endMs > startMs }
}

public struct PlanningProgress: Equatable, Sendable {
    public var closed: Int
    public var total: Int

    public var fraction: Double { total == 0 ? 0 : Double(closed) / Double(total) }
    public var percent: Int { Int((fraction * 100).rounded()) }
    public var label: String { total == 0 ? "No issues" : "\(closed) of \(total) closed · \(percent)%" }
}

public enum PlanningRecords {
    public static let projectsPath = ".forkmesh/projects"
    public static let milestonesPath = ".forkmesh/issues/milestones.json"
    public static let maxProjects = 200

    public static func milestones(_ text: String) -> [RepoMilestone] {
        guard let list = (try? JSON.parse(text))?.arrayValue else { return [] }
        return list.compactMap(\.objectValue).prefix(500).compactMap { map in
            let title = map.str("title").trimmingCharacters(in: .whitespaces)
            return title.isEmpty ? nil : RepoMilestone(
                title: title, dueMs: map.int64("due"), status: map.str("status", "open"), description: map.str("description"))
        }
    }

    public static func progress(project: RepoProject, issues: [Issue]) -> PlanningProgress {
        let linked: [Issue]
        if !project.issues.isEmpty {
            let wanted = Set(project.issues)
            linked = issues.filter { wanted.contains($0.number) }
        } else if !project.milestone.isEmpty {
            linked = issues.filter { $0.milestone == project.milestone }
        } else {
            linked = []
        }
        return PlanningProgress(closed: linked.filter { !$0.isOpen }.count, total: linked.count)
    }

    public static func progress(milestone: RepoMilestone, issues: [Issue]) -> PlanningProgress {
        let linked = issues.filter { $0.milestone == milestone.title }
        return PlanningProgress(closed: linked.filter { !$0.isOpen }.count, total: linked.count)
    }

    public static func timeline(_ projects: [RepoProject]) -> (start: Int64, end: Int64)? {
        let dated = projects.filter(\.hasDates)
        guard let start = dated.map(\.startMs).min(), let end = dated.map(\.endMs).max(), end > start else { return nil }
        return (start, end)
    }

    public static func bar(_ project: RepoProject, in span: (start: Int64, end: Int64)) -> (offset: Double, width: Double)? {
        guard project.hasDates else { return nil }
        let total = Double(span.end - span.start)
        let offset = Double(project.startMs - span.start) / total
        let width = Double(project.endMs - project.startMs) / total
        return (max(0, min(1, offset)), max(0.01, min(1 - max(0, offset), width)))
    }
}

@MainActor
@Observable
public final class RepoPlanningModel {
    public let repo: RepoRef
    public private(set) var projects: [RepoProject] = []
    public private(set) var milestones: [RepoMilestone] = []
    public private(set) var issues: [Issue] = []
    public private(set) var loading = false
    public private(set) var error: String?
    public var showClosed = false

    @ObservationIgnored private let client: PlanningClient

    public init(client: PlanningClient, repo: RepoRef) {
        self.client = client
        self.repo = repo
    }

    public var shownProjects: [RepoProject] {
        let list = showClosed ? projects : projects.filter { $0.status != "closed" }
        return list.sorted {
            if $0.hasDates != $1.hasDates { return $0.hasDates }
            if $0.startMs != $1.startMs { return $0.startMs < $1.startMs }
            return $0.number < $1.number
        }
    }

    public var shownMilestones: [RepoMilestone] {
        let list = showClosed ? milestones : milestones.filter(\.isOpen)
        return list.sorted { ($0.dueMs == 0 ? Int64.max : $0.dueMs) < ($1.dueMs == 0 ? Int64.max : $1.dueMs) }
    }

    public var timeline: (start: Int64, end: Int64)? { PlanningRecords.timeline(shownProjects) }

    public func project(_ number: Int) -> RepoProject? { projects.first { $0.number == number } }

    public func progress(_ project: RepoProject) -> PlanningProgress { PlanningRecords.progress(project: project, issues: issues) }

    public func progress(_ milestone: RepoMilestone) -> PlanningProgress { PlanningRecords.progress(milestone: milestone, issues: issues) }

    public func linkedIssues(_ project: RepoProject) -> [Issue] {
        if !project.issues.isEmpty {
            let wanted = Set(project.issues)
            return issues.filter { wanted.contains($0.number) }
        }
        return project.milestone.isEmpty ? [] : issues.filter { $0.milestone == project.milestone }
    }

    public func issues(in milestone: RepoMilestone) -> [Issue] { issues.filter { $0.milestone == milestone.title } }

    public func load(refresh: Bool = false) async {
        loading = true
        defer { loading = false }
        if refresh { client.clearRepoCache(repo.owner, repo.name) }
        var found: [RepoProject] = []
        if let listing = try? await client.tree(repo.owner, repo.name, path: PlanningRecords.projectsPath, ref: "") {
            let dirs = listing.entries.filter { $0.isDirectory && Int($0.name) != nil }.prefix(PlanningRecords.maxProjects)
            for dir in dirs {
                let path = "\(PlanningRecords.projectsPath)/\(dir.name)/project-\(dir.name).json"
                if let blob = try? await client.blob(repo.owner, repo.name, path, ref: ""),
                   let map = (try? JSON.parse(blob.content))?.objectValue,
                   let project = RepoProject(json: map) {
                    found.append(project)
                }
            }
        }
        projects = found
        if let blob = try? await client.blob(repo.owner, repo.name, PlanningRecords.milestonesPath, ref: "") {
            milestones = PlanningRecords.milestones(blob.content)
        } else {
            milestones = []
        }
        issues = (try? await client.publishedIssues(repo.owner, repo.name)) ?? []
        error = nil
    }
}
