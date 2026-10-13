// Pull request submission, plus the repository adapter for FMDiff's
// PatchBuilder, which takes flattened trees and blob readers because FMDiff
// does not depend on FMGit.

import FMApi
import FMBase
import FMDiff
import FMGit
import Foundation

/// Why a branch cannot become a pull request yet. Each maps to something the
/// person can actually do about it.
public enum PullRequestBlocker: String, Sendable {
    /// The repository only exists on this phone, so there is nowhere to send it.
    case notPublished
    /// The branch named does not exist locally.
    case missingBranch
    /// The branch has no commits the base does not already have.
    case noCommits
    /// The commits exist but change nothing in the tree.
    case noChanges
    /// The branch changes binary files, which this build cannot put in a patch.
    case binaryChanges
}

/// The result of inspecting a branch: either a submittable pull request, or a
/// precise reason it is not one.
public struct PullRequestPreparation: Equatable, Sendable {
    public let blocker: PullRequestBlocker?
    public let message: String
    public let owner: String
    public let repo: String
    public let base: String
    public let head: String
    public let patch: String
    public let changedPaths: [String]
    public let binaryPaths: [String]

    init(
        message: String, blocker: PullRequestBlocker? = nil, owner: String = "", repo: String = "",
        base: String = "", head: String = "", patch: String = "", changedPaths: [String] = [], binaryPaths: [String] = []
    ) {
        self.message = message
        self.blocker = blocker
        self.owner = owner
        self.repo = repo
        self.base = base
        self.head = head
        self.patch = patch
        self.changedPaths = changedPaths
        self.binaryPaths = binaryPaths
    }

    public static func blocked(_ blocker: PullRequestBlocker, _ message: String, binaryPaths: [String] = []) -> PullRequestPreparation {
        PullRequestPreparation(message: message, blocker: blocker, binaryPaths: binaryPaths)
    }

    public var isReady: Bool { blocker == nil }

    /// A one-line summary for a confirmation sheet.
    public var summary: String {
        isReady ? "\(changedPaths.count) \(changedPaths.count == 1 ? "file" : "files") · \(head) → \(base)" : message
    }
}

/// The error `submit` throws.
public struct PullRequestSubmitError: Error, Equatable, Sendable, CustomStringConvertible, LocalizedError {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var description: String { message }
    public var errorDescription: String? { message }
}

/// Pushes a branch and submits it as a signed pull request.
///
/// The dependencies arrive as functions rather than services so this stays
/// testable without a network, and so the same flow serves both the coding
/// agent and a "create pull request" action in the repository browser.
public struct PullRequestService: Sendable {
    public typealias PushBranch = @MainActor @Sendable (
        _ workspace: GitWorkspace, _ owner: String, _ repo: String, _ branch: String
    ) async throws -> LocalPushResult

    public typealias SubmitPull = @MainActor @Sendable (
        _ owner: String, _ name: String, _ title: String, _ base: String, _ head: String, _ patch: String
    ) async throws -> Void

    public let pushBranch: PushBranch
    public let submitPull: SubmitPull

    /// How far back to look for the point the two branches diverged. A mobile
    /// agent branch is a handful of commits from its base; this bound only
    /// stops a pathological history from walking forever.
    public static let maxAncestorWalk = 2000

    public init(pushBranch: @escaping PushBranch, submitPull: @escaping SubmitPull) {
        self.pushBranch = pushBranch
        self.submitPull = submitPull
    }

    /// The app wiring: push through `publisher`, submit through `inbox`.
    @MainActor
    public static func live(publisher: PublishService, inbox: InboxService) -> PullRequestService {
        PullRequestService(
            pushBranch: { workspace, owner, repo, branch in
                try await publisher.push(workspace: workspace, owner: owner, repo: repo, branch: branch)
            },
            submitPull: { owner, name, title, base, head, patch in
                try await inbox.submitNewPull(owner, name, title: title, base: base, head: head, patch: patch)
            })
    }

    /// Inspect `headBranch` against `baseBranch` and build the patch.
    ///
    /// The diff is taken from the point the branches actually diverged, not
    /// from the base branch's current tip. If the base moved on while the
    /// agent was working, a tip-to-tip diff would try to *undo* those newer
    /// commits.
    public func prepare(
        workspace: GitWorkspace,
        record: LocalRepoRecord,
        headBranch: String,
        baseBranch: String
    ) async throws -> PullRequestPreparation {
        let published = (record.publishedAs ?? "").dartTrimmed
        guard !published.isEmpty, published.contains("/") else {
            return .blocked(
                .notPublished,
                "This repository is only on this device. Publish it to ForkMesh "
                    + "first, then its branches can become pull requests.")
        }
        let parts = published.split(separator: "/", omittingEmptySubsequences: false).map(String.init)
        let owner = parts.first ?? ""
        let repo = parts.last ?? ""

        let branches = try await workspace.branches()
        guard let headTip = branches.first(where: { $0.name == headBranch })?.tipId,
              let baseTip = branches.first(where: { $0.name == baseBranch })?.tipId else {
            return .blocked(.missingBranch, "Could not find both \(headBranch) and \(baseBranch) in this repository.")
        }

        let computed = try await workspace.withRepository("pull-prepare") { repository, _ in
            let headId = try GitObjectId.parseSha1(headTip)
            let baseId = try GitObjectId.parseSha1(baseTip)
            let divergedAt = PullRequestService.mergeBase(repository, baseId, headId)
            if let divergedAt, divergedAt.hex == headId.hex { return Optional<PatchBuildResult>.none }
            // With no common ancestor the branches are unrelated histories;
            // diffing against the base tip is the only meaningful thing left.
            return try await LocalPatch.between(repository, base: divergedAt ?? baseId, head: headId)
        }
        guard let result = computed else {
            return .blocked(.noCommits, "\(headBranch) has no commits that \(baseBranch) does not already have.")
        }

        if result.hasBinaryChanges {
            let names = result.binaryPaths.prefix(3).joined(separator: ", ")
            let more = result.binaryPaths.count > 3 ? " and \(result.binaryPaths.count - 3) more" : ""
            return .blocked(
                .binaryChanges,
                "This branch changes binary files (\(names)\(more)), which a pull request "
                    + "from mobile cannot carry yet. The commits are safe on \(headBranch) — "
                    + "open the pull request from a desktop node instead.",
                binaryPaths: result.binaryPaths)
        }
        if result.isEmpty {
            return .blocked(.noChanges, "The commits on \(headBranch) do not change any files.")
        }
        return PullRequestPreparation(
            message: "Ready to open a pull request.",
            owner: owner,
            repo: repo,
            base: baseBranch,
            head: headBranch,
            patch: result.patch,
            changedPaths: result.changedPaths)
    }

    /// Push the branch, then submit the signed pull request.
    ///
    /// Push happens first on purpose: the receiving node applies the patch
    /// with `git apply --3way`, which needs the pre-image blobs present to
    /// fall back on when its copy has moved on.
    @MainActor
    public func submit(
        workspace: GitWorkspace,
        preparation: PullRequestPreparation,
        title: String,
        onProgress: ((String) -> Void)? = nil
    ) async throws {
        if !preparation.isReady {
            throw PullRequestSubmitError("This branch is not ready: \(preparation.message)")
        }
        let trimmed = title.dartTrimmed
        let subject = trimmed.isEmpty ? "Changes from \(preparation.head)" : trimmed

        onProgress?("Pushing \(preparation.head)…")
        let push = try await pushBranch(workspace, preparation.owner, preparation.repo, preparation.head)
        if push.isRejected {
            throw PullRequestSubmitError(
                "The mesh rejected the push of \(preparation.head), so the pull "
                    + "request was not opened. Fetch and retry.")
        }

        onProgress?("Opening the pull request…")
        try await submitPull(preparation.owner, preparation.repo, subject, preparation.base, preparation.head, preparation.patch)
    }

    /// The most recent commit both branches share, or nil when they have no
    /// common ancestor within `maxAncestorWalk`.
    static func mergeBase(_ repository: GitRepository, _ a: GitObjectId, _ b: GitObjectId) -> GitObjectId? {
        var seen = Set<String>()
        var frontier = [a]
        var visited = 0
        while !frontier.isEmpty && visited < maxAncestorWalk {
            var next: [GitObjectId] = []
            for id in frontier {
                if !seen.insert(id.hex).inserted { continue }
                visited += 1
                // A missing object is a shallow or damaged history, not a
                // reason to fail the whole flow; the walk stops down this path.
                if let commit = try? repository.readCommit(id) { next.append(contentsOf: commit.parents) }
            }
            frontier = next
        }

        var queue = [b]
        var walked = Set<String>()
        var steps = 0
        while !queue.isEmpty && steps < maxAncestorWalk {
            let id = queue.removeFirst()
            if !walked.insert(id.hex).inserted { continue }
            steps += 1
            if seen.contains(id.hex) { return id }
            if let commit = try? repository.readCommit(id) { queue.append(contentsOf: commit.parents) }
        }
        return nil
    }
}

/// FMDiff's PatchBuilder over the engine: a patch between two commits, and a
/// patch for one file.
public enum LocalPatch {
    /// Diff `base`..`head` of `repository` as a unified patch.
    static func between(
        _ repository: GitRepository, base: GitObjectId, head: GitObjectId, context: Int = PatchBuilder.defaultContext
    ) async throws -> PatchBuildResult {
        let baseTree = try await flatten(repository, base)
        let headTree = try await flatten(repository, head)
        return try await PatchBuilder.between(baseTree: baseTree, headTree: headTree, context: context) { id in
            try repository.readBlob(try GitObjectId.parseSha1(id))
        }
    }

    static func flatten(_ repository: GitRepository, _ commitId: GitObjectId) async throws -> [String: PatchBlobRef] {
        let commit = try repository.readCommit(commitId)
        return try await PatchBuilder.flatten(rootTree: commit.tree.hex) { id in
            try repository.readTree(try GitObjectId.parseSha1(id)).entries.map {
                PatchTreeEntry(name: $0.nameText, id: $0.objectId.hex, mode: $0.mode.wireValue, isDirectory: $0.mode == .directory)
            }
        }
    }

    /// Diff one file's bytes against replacement text, with no repository.
    ///
    /// This is what lets the phone propose a change to a repository it has not
    /// cloned: a pull request on ForkMesh carries its own patch, and a diff
    /// needs blob ids, which are a pure function of the bytes. `oldId` must be
    /// the **real** blob id of `beforeBytes` (compute it from the raw bytes
    /// the server served, never from a re-encoded string).
    ///
    /// The header states nothing about file mode, because a caller working
    /// from raw bytes does not know it: `index <old>..<new>` without a mode is
    /// valid git for a modification and leaves the existing mode alone.
    public static func forFile(
        path: String,
        beforeBytes: [UInt8],
        afterText: String,
        oldId: GitObjectId,
        newId: GitObjectId,
        context: Int = PatchBuilder.defaultContext
    ) -> PatchBuildResult {
        let afterBytes = Array(afterText.utf8)
        if isBinary(beforeBytes) || isBinary(afterBytes) {
            return PatchBuildResult(patch: "", changedPaths: [], binaryPaths: [path])
        }
        // Lossy decoding would write U+FFFD back in place of bytes it could
        // not read and silently corrupt the file. Refuse rather than mangle —
        // the caller falls back to cloning, where the bytes survive.
        guard let beforeText = WorkspaceOps.strictUtf8(beforeBytes) else {
            return PatchBuildResult(patch: "", changedPaths: [], binaryPaths: [path])
        }
        // FMDiff's `fileDiff` always writes a mode; with the mode unknown both
        // sides carry "", which leaves exactly one stray space after the
        // `index` line's ids. Removing it gives the header without a mode.
        var diff = PatchBuilder.fileDiff(
            path: path,
            before: PatchBlobRef(id: oldId.hex, mode: ""),
            after: PatchBlobRef(id: newId.hex, mode: ""),
            beforeText: beforeText,
            afterText: afterText,
            context: context)
        let withSpace = "\nindex \(oldId.hex)..\(newId.hex) \n"
        if let range = diff.range(of: withSpace) {
            diff.replaceSubrange(range, with: "\nindex \(oldId.hex)..\(newId.hex)\n")
        }
        return PatchBuildResult(patch: diff, changedPaths: diff.isEmpty ? [] : [path], binaryPaths: [])
    }

    /// A file whose first 8000 bytes contain NUL is binary, as git decides.
    static func isBinary(_ bytes: [UInt8]) -> Bool {
        bytes.prefix(8000).contains(0)
    }
}
