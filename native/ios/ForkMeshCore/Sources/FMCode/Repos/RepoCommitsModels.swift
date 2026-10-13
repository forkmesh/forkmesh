// The Commits tab: History | Branches with counts and the clone URL footer,
// the branch list's ordering and row text, the commit timeline row, and the
// commit detail screen with its summary, changed-files and diff cards.

import FMApi
import FMBase
import FMDiff
import Foundation
import Observation

/// `_GitView`.
public enum RepoGitView: Int, CaseIterable, Sendable {
    case history, branches, graph

    public var label: String {
        switch self {
        case .history: return "History"
        case .branches: return "Branches"
        case .graph: return "Graph"
        }
    }
}

/// One row of `_BranchesView`.
public struct RepoBranchRow: Equatable, Sendable, Identifiable {
    public var branch: RepoBranch
    /// The default branch: green, with the Default badge.
    public var current: Bool
    /// "abc1234 · worktree", or "Branch".
    public var subtitle: String

    public var id: String { branch.name }

    /// Tapping opens the tip commit; nil when the branch has no sha.
    public var tipCommit: JSONObject? {
        if branch.sha.isEmpty { return nil }
        return ["hash": .string(branch.sha), "message": .string("Tip of \(branch.name)")]
    }
}

/// One `_CommitTimelineRow`.
public struct RepoCommitRow: Equatable, Sendable, Identifiable {
    public var index: Int
    public var commit: JSONObject
    public var subject: String
    /// The author, or "Unknown author".
    public var author: String
    public var short: String
    /// "3d", or "" without a date.
    public var time: String
    public var isHead: Bool
    public var isFirst: Bool
    public var isLast: Bool

    public var id: Int { index }
}

public struct RepoHistoryRead: Equatable, Sendable {
    public var commits: [JSONObject]
    public var head: String?

    public init(commits: [JSONObject], head: String? = nil) {
        self.commits = commits
        self.head = head
    }

    public init(json: JSONValue) {
        let object = json.objectValue
        let list = json.arrayValue ?? object?["commits"]?.arrayValue ?? []
        let head = object?["commit"]?.stringValue ?? ""
        self.init(commits: list.compactMap(\.objectValue), head: head.isEmpty ? nil : head)
    }

    public var tip: String? {
        if let head { return head.lowercased() }
        let first = commits.first.map(RepoDisplay.commitHash) ?? ""
        return first.isEmpty ? nil : first.lowercased()
    }
}

public enum RepoHistoryRefSupport: Equatable, Sendable {
    case honored
    case ignored
    case unknown

    public static func detect(
        ref: String, read: RepoHistoryRead, branches: [RepoBranch], defaultBranch: String
    ) -> RepoHistoryRefSupport {
        guard let tip = read.tip else { return .unknown }
        let requested = branches.first { $0.name == ref }?.sha ?? (isCommitId(ref) ? ref : "")
        let fallback = branches.first { $0.isDefault || (!defaultBranch.isEmpty && $0.name == defaultBranch) }?.sha ?? ""
        if sameCommit(requested, tip) { return .honored }
        if sameCommit(fallback, tip) {
            return requested.isEmpty || sameCommit(requested, fallback) ? .unknown : .ignored
        }
        return fallback.isEmpty && requested.isEmpty ? .unknown : .honored
    }

    static func sameCommit(_ left: String, _ right: String) -> Bool {
        let a = left.lowercased()
        let b = right.lowercased()
        if a.utf8.count < 7 || b.utf8.count < 7 { return false }
        return a.hasPrefix(b) || b.hasPrefix(a)
    }

    static func isCommitId(_ value: String) -> Bool {
        let bytes = Array(value.utf8)
        return (7...64).contains(bytes.count)
            && bytes.allSatisfy { (0x30...0x39).contains($0) || (0x61...0x66).contains($0) || (0x41...0x46).contains($0) }
    }
}

/// `_CommitsTabState`.
@MainActor
@Observable
public final class RepoCommitsTabModel {
    public let repo: Repository
    public let ref: String?
    public var view = RepoGitView.history
    public let commits = RepoFuture<[JSONObject]>()
    public let branches = RepoFuture<[RepoBranch]>()
    public private(set) var refSupport: RepoHistoryRefSupport?

    @ObservationIgnored private let client: RepoClient

    public init(client: RepoClient, repo: Repository, ref: String? = nil) {
        self.client = client
        self.repo = repo
        let trimmed = ref?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        self.ref = trimmed.isEmpty || trimmed == repo.defaultBranch ? nil : trimmed
    }

    public func start() async {
        let owner = repo.owner
        let name = repo.name
        guard ref != nil else {
            await commits.start { try await client.commits(owner, name) }
            await branches.start { try await client.branches(owner, name) }
            return
        }
        await branches.start { try await client.branches(owner, name) }
        await commits.start { try await self.refHistory() }
    }

    /// `_refresh`: drops the cached reads and loads both again.
    public func refresh() async {
        client.clearRepoCache(repo.owner, repo.name)
        let owner = repo.owner
        let name = repo.name
        guard ref != nil else {
            await commits.load { try await client.commits(owner, name) }
            await branches.load { try await client.branches(owner, name) }
            return
        }
        await branches.load { try await client.branches(owner, name) }
        await commits.load { try await self.refHistory() }
    }

    func refHistory() async throws -> [JSONObject] {
        guard let ref else { return try await client.commits(repo.owner, repo.name) }
        let read = try await client.history(repo.owner, repo.name, ref: ref)
        refSupport = RepoHistoryRefSupport.detect(
            ref: ref, read: read, branches: branches.data ?? [], defaultBranch: repo.defaultBranch)
        return read.commits
    }

    public var branchLabel: String { ref ?? defaultBranchLabel }

    public var refNotice: String? {
        guard let ref, refSupport == .ignored else { return nil }
        return "This server lists only the default branch's history, so these are the commits of \(defaultBranchLabel), not \(ref)."
    }

    /// The segment counts (nil until loaded).
    public var historyCount: Int? { commits.data?.count }
    public var branchCount: Int? { branches.data?.count }

    /// The footer's branch label.
    public var defaultBranchLabel: String { repo.defaultBranch.isEmpty ? "default branch" : repo.defaultBranch }

    /// The smart-HTTP clone URL the footer copies.
    public var cloneURL: String { client.cloneURL(repo.owner, repo.name).absoluteString }

    public var commitsErrorMessage: String? { commits.error.map { "Could not load commits: \($0)" } }
    public var branchesErrorMessage: String? { branches.error.map { "Could not load branches: \($0)" } }

    public func commitRows(now: Date = Date()) -> [RepoCommitRow] {
        RepoCommitsTabModel.commitRows(commits.data ?? [], now: now)
    }

    public static func commitRows(_ commits: [JSONObject], now: Date = Date(), timeZone: TimeZone = .current) -> [RepoCommitRow] {
        commits.enumerated().map { i, commit in
            let author = RepoDisplay.commitAuthor(commit)
            return RepoCommitRow(
                index: i,
                commit: commit,
                subject: RepoDisplay.commitSubject(commit),
                author: author.isEmpty ? "Unknown author" : author,
                short: RepoDisplay.commitShort(commit),
                time: RepoDisplay.relativeCommitTime(commit, now: now, timeZone: timeZone),
                isHead: i == 0,
                isFirst: i == 0,
                isLast: i == commits.count - 1)
        }
    }

    public var branchRows: [RepoBranchRow] { RepoCommitsTabModel.branchRows(branches.data ?? [], repo: repo) }

    /// `_BranchesView`: the default branch first, then by name.
    public static func branchRows(_ branches: [RepoBranch], repo: Repository) -> [RepoBranchRow] {
        func isCurrent(_ b: RepoBranch) -> Bool {
            b.isDefault || (!repo.defaultBranch.isEmpty && b.name == repo.defaultBranch)
        }
        let sorted = branches.enumerated().sorted { left, right in
            let ca = isCurrent(left.element) ? 0 : 1
            let cb = isCurrent(right.element) ? 0 : 1
            if ca != cb { return ca < cb }
            let order = ReposCatalog.compareUTF16(left.element.name.lowercased(), right.element.name.lowercased())
            return order == 0 ? left.offset < right.offset : order < 0
        }.map(\.element)
        return sorted.map { branch in
            let short = branch.sha.utf16.count > 7 ? RepoDisplay.shortHash(branch.sha) : branch.sha
            var meta: [String] = []
            if !short.isEmpty { meta.append(short) }
            if branch.hasWorktree { meta.append("worktree") }
            return RepoBranchRow(
                branch: branch, current: isCurrent(branch), subtitle: meta.isEmpty ? "Branch" : meta.joined(separator: " · "))
        }
    }
}

/// One `_ChangedFileRow`.
public struct RepoChangedFileRow: Equatable, Sendable, Identifiable {
    public var index: Int
    public var path: String
    /// "lib/" of "lib/main.dart" ("" at the root).
    public var directory: String
    /// "main.dart".
    public var name: String
    /// The status letter, or nil when the diff does not name the file.
    public var kind: RepoChangeKind?
    /// Additions and deletions as shown; "" hides a zero.
    public var adds: String
    public var dels: String

    public var id: Int { index }

    public init(index: Int, path: String, kind: RepoChangeKind?, adds: String, dels: String) {
        self.index = index
        self.path = path
        if let slash = path.lastIndex(of: "/") {
            directory = String(path[...slash])
            name = String(path[path.index(after: slash)...])
        } else {
            directory = ""
            name = path
        }
        self.kind = kind
        self.adds = adds == "0" ? "" : adds
        self.dels = dels == "0" ? "" : dels
    }
}

/// `_ChangedFilesCard`'s content.
public struct RepoChangedFiles: Equatable, Sendable {
    public var title: String
    public var totalAdds: Int
    public var totalDels: Int
    public var rows: [RepoChangedFileRow]

    /// The header's "+12 -3" only rolls up more than one file, and never zeros.
    public var showsTotals: Bool { rows.count > 1 && (totalAdds != 0 || totalDels != 0) }

    /// `_CommitFilesCard`: the reported file list, with letters from the diff.
    public static func forCommit(files: [RepoCommitFile], diff: String) -> RepoChangedFiles? {
        if files.isEmpty { return nil }
        var kinds: [String: RepoChangeKind] = [:]
        for file in RepoDisplay.parsePatchFiles(diff) { kinds[file.path] = file.kind }
        var adds = 0
        var dels = 0
        for file in files {
            adds += DartCore.intTryParse(file.adds) ?? 0
            dels += DartCore.intTryParse(file.dels) ?? 0
        }
        let rows = files.enumerated().map { i, file in
            RepoChangedFileRow(index: i, path: file.path, kind: kinds[file.path], adds: file.adds, dels: file.dels)
        }
        return RepoChangedFiles(
            title: RepoDisplay.changedFilesTitle(files.count), totalAdds: adds, totalDels: dels, rows: rows)
    }

    /// `_PatchFilesCard`: everything from a unified diff (pull request patches
    /// carry no separate file list). Nil when the patch names no file.
    public static func forPatch(_ patch: String) -> RepoChangedFiles? {
        let files = RepoDisplay.parsePatchFiles(patch)
        if files.isEmpty { return nil }
        let rows = files.enumerated().map { i, file in
            RepoChangedFileRow(
                index: i, path: file.path, kind: file.kind,
                adds: file.adds > 0 ? "\(file.adds)" : "", dels: file.dels > 0 ? "\(file.dels)" : "")
        }
        return RepoChangedFiles(
            title: RepoDisplay.changedFilesTitle(files.count),
            totalAdds: files.reduce(0) { $0 + $1.adds },
            totalDels: files.reduce(0) { $0 + $1.dels },
            rows: rows)
    }
}

/// `_CommitDetailScreen`.
@MainActor
@Observable
public final class RepoCommitDetailModel {
    public let repo: Repository
    public let commit: JSONObject
    public let detail: RepoFuture<RepoCommitDetail>
    public private(set) var review: DiffReviewModel?

    @ObservationIgnored private let client: RepoClient
    @ObservationIgnored private let store: KeyValueStore?

    public init(client: RepoClient, repo: Repository, commit: JSONObject, store: KeyValueStore? = nil) {
        self.client = client
        self.store = store
        self.repo = repo
        self.commit = commit
        let hash = RepoDisplay.commitHash(commit)
        // Without a hash there is nothing to fetch: the summary is all there is.
        detail = hash.isEmpty ? RepoFuture(data: RepoCommitDetail(commit: commit)) : RepoFuture()
    }

    public var hash: String { RepoDisplay.commitHash(commit) }

    /// The app bar title: the short hash, or "Commit".
    public var title: String {
        let short = RepoDisplay.shortHash(hash)
        return short.isEmpty ? "Commit" : short
    }

    /// Whether the diff cards are shown at all (a commit with a hash).
    public var loadsDiff: Bool { !hash.isEmpty }

    public func start() async {
        if hash.isEmpty { return }
        let request = (repo.owner, repo.name, hash, commit)
        await detail.start {
            try await client.commitDetail(request.0, request.1, request.2, fallbackCommit: request.3)
        }
        if review == nil, let loaded = detail.data {
            review = DiffReviewModel(
                files: UnifiedDiff.parse(loaded.diff), scope: reviewScope, split: DiffViewSettings.shared.split, store: store)
        }
    }

    public var reviewScope: String { "\(repo.owner)/\(repo.name)/commit/\(hash)" }

    /// `snap.data ?? RepoCommitDetail(commit: commit)`.
    public var shownDetail: RepoCommitDetail { detail.data ?? RepoCommitDetail(commit: commit) }

    public var errorMessage: String? { detail.error.map { "Could not load commit diff: \($0)" } }

    public var changedFiles: RepoChangedFiles? {
        let shown = shownDetail
        return RepoChangedFiles.forCommit(files: shown.files, diff: shown.diff)
    }

    /// `_CommitSummaryCard`.
    public var summary: RepoCommitSummary { RepoCommitSummary(repo: repo, detail: shownDetail) }
}

/// `_CommitSummaryCard`'s content.
public struct RepoCommitSummary: Equatable, Sendable {
    public var title: String
    /// The hash chip ("unknown" without one).
    public var hashLabel: String
    public var author: String
    public var repoName: String
    public var sourceLabel: String?
    public var body: String

    public init(repo: Repository, detail: RepoCommitDetail) {
        let commit = detail.commit
        let hash = RepoDisplay.commitHash(commit)
        title = RepoDisplay.commitSubject(commit)
        hashLabel = hash.isEmpty ? "unknown" : hash
        author = RepoDisplay.commitAuthor(commit)
        repoName = repo.fullName
        sourceLabel = RepoDisplay.sourceLabel(detail.source)
        body = RepoDisplay.commitBody(commit)
    }
}
