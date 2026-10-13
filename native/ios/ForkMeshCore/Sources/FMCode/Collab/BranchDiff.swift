// The branch comparison logic: the base fallback, reload and error text, the
// comparison header lines, the file list's filter and viewed progress, the
// commit list's empty state and rows, a patch commit's meta line, and the patch
// files header and age.

import FMApi
import FMBase
import FMDiff
import Foundation
import Observation

/// `_BranchComparison`: the raw comparison, its files and commits parsed once.
@MainActor
public final class BranchComparison {
    public let raw: RepoComparison
    public let files: [DiffFile]
    public let commits: [PatchCommit]
    private var commitFiles: [String: [DiffFile]] = [:]

    public init(_ raw: RepoComparison) {
        self.raw = raw
        files = UnifiedDiff.parse(raw.patch)
        commits = UnifiedDiff.parseFormatPatchSeries(raw.commits)
    }

    /// A commit's own files, parsed on first use.
    public func filesOf(_ commit: PatchCommit) -> [DiffFile] {
        if let cached = commitFiles[commit.hash] { return cached }
        let parsed = UnifiedDiff.parse(commit.patch)
        commitFiles[commit.hash] = parsed
        return parsed
    }

    public var additions: Int { diffAdditions(files) }
    public var deletions: Int { diffDeletions(files) }
}

public func diffAdditions(_ files: [DiffFile]) -> Int { files.reduce(0) { $0 + $1.additions } }

public func diffDeletions(_ files: [DiffFile]) -> Int { files.reduce(0) { $0 + $1.deletions } }

/// `_age`: how long ago, in the compact form of the commit lists.
public func diffAge(_ date: Date, now: Date = Date()) -> String {
    let seconds = now.timeIntervalSince(date)
    let minutes = Int(seconds / 60)
    let hours = Int(seconds / 3600)
    let days = Int(seconds / 86400)
    if minutes < 1 { return "now" }
    if hours < 1 { return "\(minutes)m ago" }
    if days < 1 { return "\(hours)h ago" }
    if days < 30 { return "\(days)d ago" }
    if days < 365 { return "\(days / 30)mo ago" }
    return "\(days / 365)y ago"
}

public enum BranchDiff {
    public static let loadingLabel = "Loading diff"
    public static let errorTitle = "Could not load the diff"
    public static let retry = "Try again"
    public static let patchCopied = "Patch copied"

    /// The base to compare against: the given one, else the repository's
    /// default branch, else "main".
    public static func base(_ base: String, repo: Repository) -> String {
        let given = base.dartTrimmed
        if !given.isEmpty { return given }
        let fallback = repo.defaultBranch.dartTrimmed
        return fallback.isEmpty ? "main" : fallback
    }

    /// `_describeError`: the failure in the words of a reader, not a router.
    public static func describeError(_ text: String, head: String, base: String) -> String {
        if text.contains("ref was not found") || text.contains("not_found") {
            return "\(head) is not on a serving mirror yet. Branches appear here once the node that ran the agent has pushed them."
        }
        if text.contains("merge base") {
            return "\(head) and \(base) share no history, so there is no diff between them to show."
        }
        if text.contains("too large") || text.contains("output limit") {
            return "This branch changes more than a phone-sized comparison can carry. Open it on a desktop node instead."
        }
        return text
    }

    public static func filesTab(_ comparison: BranchComparison) -> String { "Files · \(comparison.files.count)" }

    public static func commitsTab(_ comparison: BranchComparison) -> String {
        let count = comparison.commits.isEmpty ? comparison.raw.commitCount : comparison.commits.count
        return "Commits · \(count)"
    }

    /// "1 commit · 2 files".
    public static func summary(commits: Int, files: Int) -> String {
        "\(commits) \(commits == 1 ? "commit" : "commits") · \(files) \(files == 1 ? "file" : "files")"
    }

    public static func shortOid(_ oid: String) -> String { oid.count > 7 ? String(oid.prefix(7)) : oid }

    /// "head abc1234 · from def5678", or nil without a head.
    public static func headLine(_ raw: RepoComparison) -> String? {
        if raw.headOid.isEmpty { return nil }
        return "head \(shortOid(raw.headOid))" + (raw.mergeBaseOid.isEmpty ? "" : " · from \(shortOid(raw.mergeBaseOid))")
    }

    /// The pager's context line.
    public static func subtitle(head: String, base: String) -> String { "\(head) → \(base)" }

    /// The Files tab with nothing to list.
    public static func emptyMessage(head: String, base: String, commitCount: Int) -> String {
        if commitCount == 0 {
            return "\(head) has nothing that \(base) does not already have — it may have been merged."
        }
        return "The commits on \(head) do not change any files."
    }

    /// `_CommitList` with no commits: the title and the explanation.
    public static func commitListEmpty(_ count: Int) -> (title: String, message: String) {
        if count == 0 { return ("No commits", "This branch has no commits of its own.") }
        return (
            "\(count) commits",
            "The range includes a merge commit, so the mirror did not send the commits one by one. The Files tab shows their combined change."
        )
    }

    /// "0123456 · Agent · 3d ago".
    public static func commitMeta(_ commit: PatchCommit, now: Date = Date()) -> String {
        var parts = [commit.shortHash]
        if !commit.author.isEmpty { parts.append(commit.author) }
        if let date = commit.date { parts.append(diffAge(date, now: now)) }
        return parts.joined(separator: " · ")
    }

    /// `PatchFilesScreen`'s header: "2 files".
    public static func fileCount(_ count: Int) -> String { "\(count) \(count == 1 ? "file" : "files")" }

    public static let commitEmpty = "This commit does not change any files."
}

/// DiffFileList's rules.
public enum DiffFileListLogic {
    public static let defaultEmpty = "No files changed."
    public static let emptyTitle = "No changes"
    public static let filterHint = "Filter files"

    /// Files whose path (or old path) contains the query, case-insensitively.
    public static func filter(_ files: [DiffFile], query: String) -> [DiffFile] {
        let wanted = query.dartTrimmed.lowercased()
        if wanted.isEmpty { return files }
        return files.filter { $0.path.lowercased().contains(wanted) || $0.oldPath.lowercased().contains(wanted) }
    }

    /// The filter field appears past five files.
    public static func showsFilter(_ count: Int) -> Bool { count > 5 }

    public static func progressLabel(seen: Int, total: Int) -> String { "\(seen) / \(total) viewed" }

    public static func progress(seen: Int, total: Int) -> Double { total == 0 ? 0 : Double(seen) / Double(total) }

    public static func noMatch(_ filter: String) -> String { "No files match \"\(filter)\"." }
}

/// `_BranchDiffScreenState`.
@MainActor
@Observable
public final class BranchDiffModel {
    public let repo: Repository
    public let head: String
    /// Shown under the branch name, e.g. the agent task that made it.
    public let title: String
    /// The base actually compared against.
    public let base: String
    public private(set) var comparison: BranchComparison?
    /// `'$error'` of the last failed read.
    public private(set) var error: String?
    /// A read is in flight (`connectionState != done`).
    public private(set) var loading = false
    public private(set) var review: DiffReviewModel?
    public let threads: BranchReviewSession?

    @ObservationIgnored private let api: CollabAPI
    @ObservationIgnored private let store: KeyValueStore?
    @ObservationIgnored private var started = false
    @ObservationIgnored private var generation = 0

    public init(
        repo: Repository, head: String, base: String = "", title: String = "", api: CollabAPI, store: KeyValueStore? = nil,
        threads: BranchReviewSession? = nil
    ) {
        self.repo = repo
        self.head = head
        self.title = title
        self.base = BranchDiff.base(base, repo: repo)
        self.api = api
        self.store = store
        self.threads = threads
    }

    public func start() async {
        if started { return }
        started = true
        await load()
    }

    /// `_load` / `_reload`: the last answer stays on screen while the next
    /// is read, as a FutureBuilder keeps its data.
    public func load() async {
        generation += 1
        let mine = generation
        loading = true
        do {
            let raw = try await api.compare(repo.owner, repo.name, base: base, head: head)
            if mine != generation { return }
            let loaded = BranchComparison(raw)
            comparison = loaded
            review = DiffReviewModel(
                files: loaded.files, scope: reviewScope, split: DiffViewSettings.shared.split, store: store,
                images: imageLoader(raw))
            error = nil
            threads?.attach(raw, base: base)
        } catch {
            if mine != generation { return }
            // A failed read drops the data, as AsyncSnapshot.withError does.
            comparison = nil
            review = nil
            self.error = dartErrorText(error)
        }
        loading = false
        if comparison != nil, let threads { await threads.load() }
    }

    private func imageLoader(_ raw: RepoComparison) -> DiffImageLoader? {
        guard let reader = api as? RepoArchiveReading, !raw.headOid.isEmpty else { return nil }
        let owner = repo.owner
        let name = repo.name
        let before = raw.mergeBaseOid.isEmpty ? raw.baseOid : raw.mergeBaseOid
        let after = raw.headOid
        return { (request: DiffImageRequest) in
            let ref = request.side == .old ? before : after
            if ref.isEmpty { throw DiffImageError(DiffImages.unreadable) }
            do {
                return try await reader.rawBytes(owner, name, request.path, ref: ref, maxBytes: DiffImages.maxBytes)
            } catch is CancellationError {
                throw CancellationError()
            } catch {
                throw DiffImageError("The mirror could not send this image.")
            }
        }
    }

    /// The error in reader's words (`_describeError`).
    public var errorMessage: String? { error.map { BranchDiff.describeError($0, head: head, base: base) } }

    /// The tabs show only once a comparison is in and no error stands.
    public var showsTabs: Bool { comparison != nil && error == nil }

    public var subtitle: String { BranchDiff.subtitle(head: head, base: base) }

    public var reviewScope: String { "\(repo.owner)/\(repo.name)/branch/\(head)" }

    public var emptyMessage: String {
        BranchDiff.emptyMessage(head: head, base: base, commitCount: comparison?.raw.commitCount ?? 0)
    }
}
