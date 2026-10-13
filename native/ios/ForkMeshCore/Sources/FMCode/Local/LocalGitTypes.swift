// The value types of GitWorkspace and GitOperationRunner: plain Sendable
// values that cross from the worker task to the main actor.

import FMApi
import FMGit
import Foundation

/// Commit identity used for author and committer.
///
/// The engine accepts UTF-8 identity fields but rejects angle brackets and
/// control characters (they would corrupt the `name <email>` header framing);
/// `GitAuthor.fromAccount` derives a safe identity from the signed-in account
/// (node name + e-mail) with deterministic fallbacks.
public struct GitAuthor: Equatable, Hashable, Sendable {
    public let name: String
    public let email: String

    public init(name: String, email: String) {
        self.name = name
        self.email = email
    }

    /// Build a commit identity from the logged-in account.
    public static func fromAccount(nodeName: String, email: String = "") -> GitAuthor {
        let name = identityField(nodeName, fallback: "forkmesh")
        let safeEmail = identityField(email, fallback: "")
        return GitAuthor(name: name, email: safeEmail.contains("@") ? safeEmail : "\(name)@mobile.forkmesh")
    }

    /// Keeps UTF-8 text, dropping control characters and angle brackets.
    static func identityField(_ raw: String, fallback: String) -> String {
        var kept = String.UnicodeScalarView()
        for scalar in raw.dartTrimmed.unicodeScalars {
            if scalar.value < 0x20 || scalar.value == 0x7f { continue }
            if scalar.value == 0x3c || scalar.value == 0x3e { continue }
            kept.append(scalar)
        }
        let text = String(kept).dartTrimmed
        return text.isEmpty ? fallback : text
    }

    func toIdentity(_ now: Date, timeZone: TimeZone = .current) throws -> GitIdentity {
        try GitIdentity(
            name: name,
            email: email,
            timestampSeconds: Int((now.timeIntervalSince1970 * 1000).rounded(.down)) / 1000,
            timezoneOffsetMinutes: timeZone.secondsFromGMT(for: now) / 60)
    }
}

public enum LocalFileKind: String, Sendable {
    case file, directory, symlink
}

/// One entry of a worktree directory listing.
public struct LocalFileEntry: Equatable, Hashable, Sendable, Identifiable {
    public let name: String
    /// Repository-relative path with `/` separators.
    public let path: String
    public let kind: LocalFileKind
    public let size: Int
    /// True when the path (or, for directories, anything under it) is in the index.
    public let tracked: Bool

    public init(name: String, path: String, kind: LocalFileKind, size: Int, tracked: Bool) {
        self.name = name
        self.path = path
        self.kind = kind
        self.size = size
        self.tracked = tracked
    }

    public var id: String { path }
    public var isDirectory: Bool { kind == .directory }
}

/// Contents of one worktree file prepared for the viewer/editor.
public struct LocalFileContent: Equatable, Sendable {
    public let path: String
    public let bytes: Int
    /// UTF-8 text when `isBinary` and `isTooLarge` are both false.
    public let text: String
    public let isBinary: Bool
    public let isTooLarge: Bool

    public init(path: String, bytes: Int, text: String, isBinary: Bool, isTooLarge: Bool) {
        self.path = path
        self.bytes = bytes
        self.text = text
        self.isBinary = isBinary
        self.isTooLarge = isTooLarge
    }

    public var isEditable: Bool { !isBinary && !isTooLarge }

    /// Largest file the in-app text editor will open.
    public static let maxEditableBytes = 2 * 1024 * 1024
}

public enum LocalChangeKind: String, Sendable {
    case added, modified, deleted, untracked
}

/// A changed path in either the index (staged) or the worktree.
public struct LocalChange: Equatable, Hashable, Sendable, Identifiable {
    public let path: String
    public let kind: LocalChangeKind
    public let staged: Bool

    public init(path: String, kind: LocalChangeKind, staged: Bool) {
        self.path = path
        self.kind = kind
        self.staged = staged
    }

    public var id: String { "\(staged ? "s" : "w"):\(path)" }
}

/// Snapshot of `status`: index vs HEAD, worktree vs index, and untracked.
public struct LocalStatus: Equatable, Sendable {
    public let staged: [LocalChange]
    public let unstaged: [LocalChange]
    public let untracked: [LocalChange]

    public init(staged: [LocalChange], unstaged: [LocalChange], untracked: [LocalChange]) {
        self.staged = staged
        self.unstaged = unstaged
        self.untracked = untracked
    }

    public static let empty = LocalStatus(staged: [], unstaged: [], untracked: [])

    public var isClean: Bool { staged.isEmpty && unstaged.isEmpty && untracked.isEmpty }
    public var hasWorktreeChanges: Bool { !unstaged.isEmpty || !untracked.isEmpty }
    public var changeCount: Int { staged.count + unstaged.count + untracked.count }
}

public struct LocalBranch: Equatable, Hashable, Sendable, Identifiable {
    public let name: String
    public let isCurrent: Bool
    /// Hex object id of the branch tip; nil for an unborn current branch.
    public let tipId: String?

    public init(name: String, isCurrent: Bool, tipId: String?) {
        self.name = name
        self.isCurrent = isCurrent
        self.tipId = tipId
    }

    public var id: String { name }
}

public struct LocalCommit: Equatable, Hashable, Sendable, Identifiable {
    public let id: String
    public let parents: [String]
    public let subject: String
    public let message: String
    public let authorName: String
    public let authorEmail: String
    public let time: Date

    public init(
        id: String, parents: [String], subject: String, message: String,
        authorName: String, authorEmail: String, time: Date
    ) {
        self.id = id
        self.parents = parents
        self.subject = subject
        self.message = message
        self.authorName = authorName
        self.authorEmail = authorEmail
        self.time = time
    }

    public var shortId: String { id.count > 7 ? String(id.prefix(7)) : id }
}

/// Cheap per-repository summary used by the repo list.
public struct LocalRepoSummary: Equatable, Sendable {
    public let branch: String
    public let headId: String?
    public let lastCommitSubject: String
    public let lastCommitAt: Date?

    public init(branch: String, headId: String?, lastCommitSubject: String, lastCommitAt: Date?) {
        self.branch = branch
        self.headId = headId
        self.lastCommitSubject = lastCommitSubject
        self.lastCommitAt = lastCommitAt
    }
}

/// Result of an import from a directory or zip archive.
public struct LocalImportResult: Equatable, Sendable {
    public let importedFiles: Int
    public let skippedEntries: Int
    public let commitId: String

    public init(importedFiles: Int, skippedEntries: Int, commitId: String) {
        self.importedFiles = importedFiles
        self.skippedEntries = skippedEntries
        self.commitId = commitId
    }
}

/// One progress sample from a clone, fetch, or push, forwarded from the
/// worker task.
public struct GitTransferProgress: Equatable, Sendable {
    /// Name of the engine's `GitProgressPhase` (`receivingPack`, ...).
    public let phase: String
    public let bytes: Int
    public let objects: Int
    public let refs: Int
    public let total: Int

    public init(phase: String, bytes: Int = 0, objects: Int = 0, refs: Int = 0, total: Int = 0) {
        self.phase = phase
        self.bytes = bytes
        self.objects = objects
        self.refs = refs
        self.total = total
    }

    /// Short sentence for the busy overlay.
    public var label: String {
        switch phase {
        case "connecting": return "Connecting to ForkMesh"
        case "listingRefs": return "Reading branches"
        case "negotiating": return "Working out what to transfer"
        case "receivingPack": return bytes > 0 ? "Downloading \(Self.humanBytes(bytes))" : "Downloading"
        case "indexingPack":
            if total > 0 && objects > 0 && objects < total { return "Indexing \(objects) of \(Self.count(total, "object"))" }
            if total > 0 { return "Indexing \(Self.count(total, "object"))" }
            return objects > 0 ? "Indexing \(Self.count(objects, "object"))" : "Indexing objects"
        case "updatingRefs": return "Updating branches"
        case "checkingOut":
            if total > 0 && objects > 0 && objects < total { return "Checking out \(objects) of \(Self.count(total, "file"))" }
            return total > 0 ? "Checking out \(Self.count(total, "file"))" : "Checking out files"
        case "preparingPack": return "Packing \(objects) objects"
        case "sendingPack": return bytes > 0 ? "Uploading \(Self.humanBytes(bytes))" : "Uploading"
        case "awaitingReport": return "Waiting for ForkMesh"
        case "completed": return "Finishing up"
        default: return "Working"
        }
    }

    static func count(_ value: Int, _ noun: String) -> String {
        value == 1 ? "1 \(noun)" : "\(value) \(noun)s"
    }

    static func humanBytes(_ bytes: Int) -> String {
        if bytes < 1024 { return "\(bytes) B" }
        if bytes < 1024 * 1024 { return "\(DartCore.toStringAsFixed(Double(bytes) / 1024, 0)) KB" }
        return "\(DartCore.toStringAsFixed(Double(bytes) / (1024 * 1024), 1)) MB"
    }
}

/// Outcome for one ref of a push.
public struct LocalPushRef: Equatable, Sendable {
    public let remoteRef: String
    public let newId: String
    /// `updated`, `upToDate`, or `rejected`.
    public let outcome: String
    public let message: String

    public init(remoteRef: String, newId: String, outcome: String, message: String = "") {
        self.remoteRef = remoteRef
        self.newId = newId
        self.outcome = outcome
        self.message = message
    }

    public var isRejected: Bool { outcome == "rejected" }
    public var branch: String {
        remoteRef.hasPrefix("refs/heads/") ? String(remoteRef.dropFirst("refs/heads/".count)) : remoteRef
    }
}

public struct LocalPushResult: Equatable, Sendable {
    public let refs: [LocalPushRef]
    public let packBytes: Int
    public let objects: Int

    public init(refs: [LocalPushRef], packBytes: Int, objects: Int) {
        self.refs = refs
        self.packBytes = packBytes
        self.objects = objects
    }

    public var isRejected: Bool { refs.contains { $0.isRejected } }
    public var isUpToDate: Bool { !refs.isEmpty && refs.allSatisfy { $0.outcome == "upToDate" } }
}

public struct LocalFetchResult: Equatable, Sendable {
    public let updatedRefs: [String]
    public let packBytes: Int
    public let objects: Int
    public let remoteHead: String?
    public let servedBy: String?

    public init(updatedRefs: [String], packBytes: Int, objects: Int, remoteHead: String?, servedBy: String? = nil) {
        self.updatedRefs = updatedRefs
        self.packBytes = packBytes
        self.objects = objects
        self.remoteHead = remoteHead
        self.servedBy = servedBy
    }

    public var isUpToDate: Bool { updatedRefs.isEmpty }
}

public enum LocalPullOutcome: Equatable, Sendable {
    case upToDate
    case fastForwarded(from: String, to: String)
    case ahead
    case diverged
    case undetermined
    case noRemoteBranch
}

public struct LocalPullResult: Equatable, Sendable {
    public let branch: String
    public let fetch: LocalFetchResult
    public let outcome: LocalPullOutcome

    public init(branch: String, fetch: LocalFetchResult, outcome: LocalPullOutcome) {
        self.branch = branch
        self.fetch = fetch
        self.outcome = outcome
    }

    public var changedWorktree: Bool {
        if case .fastForwarded = outcome { return true }
        return false
    }

    public var needsAttention: Bool {
        switch outcome {
        case .diverged, .undetermined: return true
        case .upToDate, .fastForwarded, .ahead, .noRemoteBranch: return false
        }
    }

    public var title: String {
        switch outcome {
        case .upToDate, .ahead: return "Up to date"
        case .fastForwarded: return "Synced"
        case .diverged, .undetermined: return "Sync needs attention"
        case .noRemoteBranch: return "Nothing to sync"
        }
    }

    public var message: String {
        switch outcome {
        case .upToDate:
            return "\(branch) is already up to date with ForkMesh."
        case .fastForwarded:
            return "Synced \(branch) with ForkMesh."
        case .ahead:
            return "Nothing to sync: \(branch) here already contains ForkMesh's. Push to share your commits."
        case .diverged:
            return "\(branch) changed both here and on ForkMesh, and this device only syncs fast-forwards. "
                + "Push your commits to a new branch and open a pull request."
        case .undetermined:
            return "This device's history of \(branch) is too short to tell whether ForkMesh's builds on yours, "
                + "so \(branch) was left as it was. The fetched commits are on origin/\(branch)."
        case .noRemoteBranch:
            return "ForkMesh has no branch named \(branch) yet. Push it first."
        }
    }
}

public struct LocalCloneResult: Equatable, Sendable {
    /// Branch checked out, or nil when the remote was empty.
    public let branch: String?
    public let packBytes: Int
    public let objects: Int

    public init(branch: String?, packBytes: Int, objects: Int) {
        self.branch = branch
        self.packBytes = packBytes
        self.objects = objects
    }
}

/// How a local branch relates to its remote-tracking ref.
public struct LocalSyncState: Equatable, Sendable {
    public let branch: String
    public let localTip: String?
    public let remoteTip: String?
    /// Commits on the local branch that ForkMesh does not have, or nil when
    /// the answer is beyond the bounded walk.
    public let ahead: Int?
    public let behind: Int?

    public init(branch: String, localTip: String?, remoteTip: String?, ahead: Int?, behind: Int?) {
        self.branch = branch
        self.localTip = localTip
        self.remoteTip = remoteTip
        self.ahead = ahead
        self.behind = behind
    }

    public static func unknown(_ branch: String) -> LocalSyncState {
        LocalSyncState(branch: branch, localTip: nil, remoteTip: nil, ahead: nil, behind: nil)
    }

    public var hasRemote: Bool { !(remoteTip ?? "").isEmpty }
    public var isSynced: Bool { hasRemote && localTip == remoteTip }

    /// Short caption for the repository header.
    public var label: String {
        if !hasRemote { return "Not pushed yet" }
        if isSynced { return "In sync with ForkMesh" }
        var parts: [String] = []
        if ahead == nil || ahead! > 0 { parts.append("\(ahead.map(String.init) ?? "500+") to push") }
        if behind == nil || behind! > 0 { parts.append("\(behind.map(String.init) ?? "500+") to fetch") }
        return parts.isEmpty ? "In sync with ForkMesh" : parts.joined(separator: " · ")
    }
}

/// User-facing wrapper for failures surfaced by `GitWorkspace`.
public struct GitWorkspaceException: Error, Equatable, Sendable, CustomStringConvertible, LocalizedError {
    public let message: String
    public let code: GitErrorCode?
    public let detail: String

    public init(_ message: String, code: GitErrorCode? = nil, detail: String = "") {
        self.message = message
        self.code = code
        self.detail = detail
    }

    public var description: String { message }
    public var errorDescription: String? { message }
}

/// Non-engine failure raised inside the worker (I/O, decoding, ...).
public struct GitWorkerException: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String
    public let detail: String

    public init(_ message: String, _ detail: String = "") {
        self.message = message
        self.detail = detail
    }

    public var description: String { detail.isEmpty ? message : "\(message) (\(detail))" }
}
