import FMApi
import FMBase
import Foundation
import Observation

public struct IssueBurnupPoint: Equatable, Sendable, Identifiable {
    public var timestampMs: Int64
    public var openCount: Int
    public var closedCount: Int
    public var id: Int64 { timestampMs }
    public var total: Int { openCount + closedCount }
}

public enum IssueBurnup {
    public static func createdAt(_ issue: Issue) -> Int64 {
        var created: Int64 = 0
        for event in issue.events where event.type == "open" && event.ts > 0 {
            let ts = Int64(event.ts)
            if created == 0 || ts < created { created = ts }
        }
        return created
    }

    public static func status(_ issue: Issue, at timestampMs: Int64) -> String {
        let changes = issue.events
            .filter { $0.type == "status" && $0.ts > 0 && Int64($0.ts) <= timestampMs }
            .sorted { $0.ts < $1.ts }
        var status = "open"
        for change in changes where change.status == "open" || change.status == "closed" {
            status = change.status
        }
        return status
    }

    public static func series(_ issues: [Issue], start: Int64, end: Int64, intervals: Int) -> [IssueBurnupPoint] {
        var start = start
        var end = end
        if end < start { swap(&start, &end) }
        let steps = max(1, intervals)
        let duration = end - start
        let created = issues.map { (issue: $0, createdAt: createdAt($0)) }
        return (0...steps).map { i in
            let timestamp = start + duration * Int64(i) / Int64(steps)
            var point = IssueBurnupPoint(timestampMs: timestamp, openCount: 0, closedCount: 0)
            for entry in created where entry.createdAt > 0 && entry.createdAt <= timestamp {
                if status(entry.issue, at: timestamp) == "closed" {
                    point.closedCount += 1
                } else {
                    point.openCount += 1
                }
            }
            return point
        }
    }

    public static func firstTimestamp(_ issues: [Issue], fallback: Int64) -> Int64 {
        let first: Int64 = issues.map { IssueBurnup.createdAt($0) }.filter { $0 > 0 }.min() ?? 0
        return first > 0 ? first : fallback
    }
}

public struct ContributorCount: Equatable, Sendable, Identifiable {
    public var name: String
    public var commits: Int
    public var id: String { name }
}

public struct WeekCount: Equatable, Sendable, Identifiable {
    public var weekStartMs: Int64
    public var commits: Int
    public var id: Int64 { weekStartMs }
}

public struct RepoInsightsSummary: Equatable, Sendable {
    public static let weeks = 12
    public static let topContributors = 8

    public var commitsSeen: Int
    public var weekly: [WeekCount]
    public var contributors: [ContributorCount]
    public var openIssues: Int
    public var closedIssues: Int
    public var openPulls: Int
    public var mergedPulls: Int
    public var closedPulls: Int
    public var burnup: [IssueBurnupPoint]
    public var language: String
    public var stars: Int
    public var forks: Int
    public var mirrors: Int

    public init(commits: [JSONObject], issues: [Issue], pulls: [PullRequest], repository: Repository?, nowMs: Int64,
                burnupIntervals: Int = 12) {
        commitsSeen = commits.count
        let week: Int64 = 7 * 24 * 60 * 60 * 1000
        let currentWeek = nowMs - nowMs % week
        let firstWeek = currentWeek - week * Int64(RepoInsightsSummary.weeks - 1)
        var buckets = [Int](repeating: 0, count: RepoInsightsSummary.weeks)
        var authors: [String: Int] = [:]
        for commit in commits {
            let author = RepoInsightsSummary.author(commit)
            if !author.isEmpty { authors[author, default: 0] += 1 }
            guard let ms = RepoInsightsSummary.dateMs(commit), ms >= firstWeek, ms < currentWeek + week else { continue }
            buckets[Int((ms - firstWeek) / week)] += 1
        }
        weekly = buckets.enumerated().map { WeekCount(weekStartMs: firstWeek + week * Int64($0.offset), commits: $0.element) }
        contributors = authors.map { ContributorCount(name: $0.key, commits: $0.value) }
            .sorted { $0.commits != $1.commits ? $0.commits > $1.commits : $0.name.lowercased() < $1.name.lowercased() }
            .prefix(RepoInsightsSummary.topContributors).map { $0 }
        openIssues = issues.filter(\.isOpen).count
        closedIssues = issues.count - openIssues
        openPulls = pulls.filter { $0.status == "open" }.count
        mergedPulls = pulls.filter { $0.status == "merged" }.count
        closedPulls = pulls.filter { $0.status == "closed" }.count
        let start = IssueBurnup.firstTimestamp(issues, fallback: nowMs - 30 * 24 * 60 * 60 * 1000)
        burnup = issues.isEmpty ? [] : IssueBurnup.series(issues, start: start, end: nowMs, intervals: burnupIntervals)
        language = repository?.language ?? ""
        stars = repository?.stars ?? 0
        forks = repository?.forks ?? 0
        mirrors = repository?.mirrors ?? 0
    }

    public var commitSeries: StatsTileSeries {
        StatsTileSeries(labels: weekly.map { String($0.weekStartMs) }, values: weekly.map { Double($0.commits) })
    }

    public var commitsInWindow: Int { weekly.reduce(0) { $0 + $1.commits } }

    public static func author(_ commit: JSONObject) -> String {
        if case .object(let author)? = commit["author"] {
            let name = author.str("name")
            return name.isEmpty ? author.str("email") : name
        }
        for key in ["author", "authorName", "committer"] {
            let value = commit.str(key).trimmingCharacters(in: .whitespaces)
            if !value.isEmpty { return value }
        }
        return ""
    }

    public static func dateMs(_ commit: JSONObject) -> Int64? {
        for key in ["date", "time", "timestamp", "createdAt", "committedDate", "authorDate", "ts"] {
            guard let value = commit[key] else { continue }
            if let number = value.doubleValue, value.stringValue == nil {
                let n = Int64(number)
                return n > 100_000_000_000 ? n : n * 1000
            }
            if let text = value.stringValue?.trimmingCharacters(in: .whitespaces), !text.isEmpty {
                if let n = Int64(text) { return n > 100_000_000_000 ? n : n * 1000 }
                if let date = RepoInsightsSummary.parseISO(text) { return Int64(date.timeIntervalSince1970 * 1000) }
            }
        }
        return nil
    }

    static func parseISO(_ text: String) -> Date? {
        let formatter = ISO8601DateFormatter()
        if let date = formatter.date(from: text) { return date }
        formatter.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return formatter.date(from: text)
    }
}

@MainActor
@Observable
public final class RepoInsightsModel {
    @ObservationIgnored private let loader: @MainActor () async throws -> RepoInsightsSummary
    public let repo: RepoRef
    public private(set) var summary: InsightsLoad<RepoInsightsSummary> = .idle

    public init(repo: RepoRef, loader: @escaping @MainActor () async throws -> RepoInsightsSummary) {
        self.repo = repo
        self.loader = loader
    }

    public convenience init(repo: RepoRef, api: ApiService, now: @escaping () -> Int64 = { Int64(Date().timeIntervalSince1970 * 1000) }) {
        self.init(repo: repo) {
            async let commits = (try? await api.commits(repo.owner, repo.name)) ?? []
            async let issues = (try? await api.publishedIssues(repo.owner, repo.name)) ?? []
            async let published = (try? await api.publishedPulls(repo.owner, repo.name)) ?? []
            let repository = try? await api.repositories().first {
                $0.owner.lowercased() == repo.owner.lowercased() && $0.name.lowercased() == repo.name.lowercased()
                    || $0.displayOwner.lowercased() == repo.owner.lowercased() && $0.name.lowercased() == repo.name.lowercased()
            }
            return RepoInsightsSummary(
                commits: await commits, issues: await issues,
                pulls: await published.map { PullRequest(number: $0.number, title: $0.title, status: $0.status) },
                repository: repository, nowMs: now())
        }
    }

    public func reload() async {
        if summary.value == nil { summary = .loading }
        do {
            summary = .loaded(try await loader())
        } catch {
            if summary.value == nil { summary = .failed(InsightsError.from(error)) }
        }
    }
}
