// The pull request logic: the pull row, detail ship controls and desktop
// requests (their checks and canonical string), timeline tiles, pull actions,
// the review composer, new pull validation, patch file parsing, the changed
// files title and rows, and the diff line kinds. After native/android
// core/.../collab/PullThread.kt.

import FMApi
import FMBase
import Foundation

/// The pull row of the Pulls tab.
public enum PullCard {
    public static func tone(_ pull: PublishedPull) -> CollabTone {
        switch pull.status {
        case "merged": return .accentEdge
        case "closed": return .danger
        default: return .success
        }
    }

    public static func subtitle(_ pull: PublishedPull) -> String {
        "#\(pull.number)" + (pull.head.isEmpty && pull.base.isEmpty ? "" : " · \(pull.head) → \(pull.base)")
    }

    public static func badge(_ pull: PublishedPull) -> String { pull.signed ? "signed" : pull.status }

    public static func author(_ pull: PublishedPull) -> String {
        let name = pull.authorName.dartTrimmed
        if !name.isEmpty { return name }
        let key = pull.author.dartTrimmed
        return key.count > 10 ? String(key.prefix(8)) + "…" : key
    }

    public static func branches(_ pull: PublishedPull) -> String {
        PullDetail.branchesChip(pull) ?? ""
    }

    public static func age(_ pull: PublishedPull, now: Date = Date()) -> String {
        pull.createdMs > 0 ? RepoDisplay.agoLabel(pull.createdMs, now: now) : ""
    }

    public static func comments(_ pull: PublishedPull) -> String {
        let count = pull.events.count
        return count == 0 ? "" : "\(count) \(count == 1 ? "comment" : "comments")"
    }

    public static func review(_ pull: PublishedPull) -> String? {
        guard let last = pull.events.last(where: { $0.type == "review" && $0.state != "comment" && !$0.state.isEmpty })
        else { return nil }
        return last.displayTitle
    }

    public static func meta(_ pull: PublishedPull, now: Date = Date()) -> String {
        (["#\(pull.number)", author(pull), age(pull, now: now), comments(pull)] + [diffStat(pull)])
            .filter { !$0.isEmpty }
            .joined(separator: " · ")
    }

    public static func diffStat(_ pull: PublishedPull) -> String {
        if pull.patch.isEmpty { return "" }
        var files = 0, adds = 0, dels = 0
        for line in pull.patch.split(separator: "\n", omittingEmptySubsequences: true) {
            if line.hasPrefix("diff --git ") {
                files += 1
            } else if line.hasPrefix("+++") || line.hasPrefix("---") {
                continue
            } else if line.hasPrefix("+") {
                adds += 1
            } else if line.hasPrefix("-") {
                dels += 1
            }
        }
        if adds == 0 && dels == 0 { return "" }
        let stat = "+\(adds) −\(dels)"
        return files > 0 ? "\(stat) in \(files) \(files == 1 ? "file" : "files")" : stat
    }

    public static func snippet(_ pull: PublishedPull) -> String {
        for raw in pull.body.split(separator: "\n", omittingEmptySubsequences: true) {
            let line = String(raw).dartTrimmed
            if line.isEmpty || line.hasPrefix("```") || line.hasPrefix("diff --git ") || line.hasPrefix("---")
                || line.hasPrefix("🤖") || line.hasPrefix("<!--") {
                continue
            }
            var text = Substring(line)
            while text.first == "#" || text.first == ">" { text = text.dropFirst() }
            let cleaned = String(text).dartTrimmed
            if !cleaned.isEmpty { return cleaned }
        }
        return ""
    }
}

public enum PullStatusFilter: String, Sendable, CaseIterable {
    case open, closed, all

    public var label: String {
        switch self {
        case .open: return "Open"
        case .closed: return "Closed"
        case .all: return "All"
        }
    }

    public func matches(_ pull: PublishedPull) -> Bool {
        let status = pull.status.dartTrimmed.lowercased()
        let done = status == "closed" || status == "merged"
        switch self {
        case .open: return !done
        case .closed: return done
        case .all: return true
        }
    }
}

public enum PullDetail {
    public static let noDescription = "No description provided."
    public static let noPatch = "No patch content was published for this pull request yet."
    public static let reviewActionsCopy =
        "Submit comments, approvals, or requested changes through the signed Worker pull inbox."
    public static let timelineEmpty = "No review or comment events have been published yet."
    public static let guestAction = "review pull requests"
    public static let changesTitle = "Files changed"
    public static let loadingChanges = "Loading the changes…"
    public static let reviewChanges = "Review changes"
    public static let compareBranch = "Compare branches"
    public static let reviewTitle = "Review"
    public static let reviewCopy =
        "Approve, request changes or comment. Reviews are signed and sent to the pull inbox."
    public static let desktopTitle = "On the desktop"
    public static let desktopCopy = "Queue a signed request; the desktop runs Git after the repo owner approves it there."

    public static func filesTab(_ count: Int) -> String { "Files \(count)" }

    public static func commitsTab(_ count: Int) -> String { "Commits \(count)" }

    public static func commitsLine(_ count: Int) -> String { "\(count) \(count == 1 ? "commit" : "commits")" }

    public static func meta(_ pull: PublishedPull, now: Date = Date()) -> String {
        [PullCard.author(pull), PullCard.age(pull, now: now)].filter { !$0.isEmpty }.joined(separator: " · ")
    }

    public static func canEditHead(_ pull: PublishedPull) -> Bool {
        pull.branchBacked && !pull.head.dartTrimmed.isEmpty && pull.status.dartTrimmed.lowercased() == "open"
    }

    public static func title(_ number: Int) -> String { "PR #\(number)" }

    public static func canRequestMerge(_ pull: PublishedPull) -> Bool {
        pull.status.dartTrimmed.lowercased() == "open"
    }

    public static func shipControlsCopy(_ pull: PublishedPull) -> String {
        if canRequestMerge(pull) {
            return "Queue signed PR requests. The desktop node will verify mergeability, show conflicts if any, and run Git only after the repo owner approves locally."
        }
        return "This pull request is \(pull.status); merge/review-changing requests may be rejected by the desktop node if the local state does not support them."
    }

    public static func body(_ pull: PublishedPull) -> String { pull.body.isEmpty ? noDescription : pull.body }

    public static func chips(_ pull: PublishedPull) -> [CollabChip] {
        var chips = [CollabChip("label_outline", pull.status)]
        if pull.signed { chips.append(CollabChip("verified_outlined", "signed")) }
        if let branches = branchesChip(pull) { chips.append(CollabChip("account_tree_outlined", branches)) }
        return chips
    }

    public static func branchesChip(_ pull: PublishedPull) -> String? {
        pull.head.isEmpty && pull.base.isEmpty ? nil : "\(pull.head) → \(pull.base)"
    }

    /// `_DiffViewer`'s full-screen title.
    public static func diffTitle(_ pull: PublishedPull) -> String { "Pull #\(pull.number)" }

    public static func commandTarget(_ pull: PublishedPull) -> String { "pull:\(pull.number)" }

    public static func commandPayload(_ pull: PublishedPull) -> JSONObject {
        var payload: JSONObject = ["pullNumber": .int(Int64(pull.number))]
        if !pull.base.isEmpty { payload["base"] = .string(pull.base) }
        if !pull.head.isEmpty { payload["head"] = .string(pull.head) }
        return payload
    }
}

/// A queued desktop request from the Ship controls card.
public struct PullShipCommand: Equatable, Sendable {
    public let command: String
    public let label: String
    public let icon: String
    public let successMessage: String
    /// The filled (primary) button.
    public let filled: Bool
    /// Only offered while the pull is open.
    public let needsOpen: Bool

    public var chipLabel: String {
        switch command {
        case "pull.merge": return "Merge on desktop"
        case "pull.merge_delete": return "Merge + delete branch"
        case "pull.fix_agent": return "Fix with agent"
        case "pull.close": return "Close"
        case "pull.reopen": return "Reopen"
        case "pull.delete": return "Delete"
        default: return label
        }
    }
}

public enum PullShipCommands {
    public static func offered(_ pull: PublishedPull) -> [PullShipCommand] {
        let status = pull.status.dartTrimmed.lowercased()
        let isOpen = status == "open"
        return all(pull).filter { (command: PullShipCommand) -> Bool in
            switch command.command {
            case "pull.merge": return isOpen
            case "pull.merge_delete": return isOpen && !pull.head.dartTrimmed.isEmpty
            case "pull.fix_agent": return isOpen
            case "pull.close": return isOpen
            case "pull.reopen": return status == "closed"
            default: return true
            }
        }
    }

    public static func all(_ pull: PublishedPull) -> [PullShipCommand] {
        [
            PullShipCommand(
                command: "pull.merge", label: "Request merge on desktop", icon: "call_merge_outlined",
                successMessage: "Merge queued. Approve it in the ForkMesh desktop app to merge pull request #\(pull.number).",
                filled: true, needsOpen: true),
            PullShipCommand(
                command: "pull.merge_delete", label: "Request merge + delete branch", icon: "delete_sweep_outlined",
                successMessage: "Merge + delete request queued. Approve it in the ForkMesh desktop app.",
                filled: false, needsOpen: true),
            PullShipCommand(
                command: "pull.fix_agent", label: "Request fix with agent", icon: "smart_toy_outlined",
                successMessage: "Fix-with-agent request queued. Approve it in the ForkMesh desktop app.",
                filled: false, needsOpen: false),
            PullShipCommand(
                command: "pull.close", label: "Request close on desktop", icon: "archive_outlined",
                successMessage: "Close request queued. Approve it in the ForkMesh desktop app.",
                filled: false, needsOpen: false),
            PullShipCommand(
                command: "pull.reopen", label: "Request reopen on desktop", icon: "lock_open_outlined",
                successMessage: "Reopen request queued. Approve it in the ForkMesh desktop app.",
                filled: false, needsOpen: false),
            PullShipCommand(
                command: "pull.delete", label: "Request delete on desktop", icon: "delete_outline",
                successMessage: "Delete request queued. Approve it in the ForkMesh desktop app.",
                filled: false, needsOpen: false),
        ]
    }
}

/// `_requestDesktopCommand`: the owner checks and the signed canonical string.
public enum DesktopCommand {
    public static let failureMessage = "Desktop request was not queued."
    public static let guestAction = "request desktop control"

    public static func canonical(owner: String, name: String, command: String, target: String, ts: String) -> String {
        DesktopCommandContract.canonical(owner: owner, name: name, command: command, target: target, ts: ts)
    }

    /// The account allowed to sign the request. Throws an exception
    /// (shown as "Exception: ...") when there is no session or identity, or
    /// the session cannot sign as the account owner.
    public static func ownerAccount(_ account: CollabAccount, identityPublicKey: String?) throws -> String {
        let name = account.nodeName?.dartTrimmed.lowercased() ?? ""
        if account.nodeName == nil || identityPublicKey == nil || name.isEmpty {
            throw ApiException("Sign in with the repo owner account to request desktop control.")
        }
        if !account.canOwnerSign {
            throw ApiException("Pair this mobile device with the owner key before requesting desktop control.")
        }
        return name
    }
}

/// `_PullTimelineTile`.
public enum PullTimeline {
    public static func tone(_ event: PullEvent) -> CollabTone {
        switch event.displayTitle {
        case "Approved": return .success
        case "Requested changes": return .danger
        default: return .tertiary
        }
    }

    public static func icon(_ event: PullEvent) -> String {
        switch event.displayTitle {
        case "Approved": return "check_circle_outline"
        case "Requested changes": return "cancel_outlined"
        default: return "rate_review_outlined"
        }
    }
}

/// The pull actions sheets and the review composer.
public enum PullReview {
    public static let reviewHint = "Add a review note"
    public static let noteRequired = "A note is required to request changes."
    public static let submitLabel = "Submit review"

    public static func header(_ number: Int, _ title: String) -> String { "#\(number) · \(title)" }

    public static func subtitle(head: String, base: String, status: String) -> String { "\(head) → \(base) · \(status)" }

    public static func subtitle(_ pull: PublishedPull) -> String {
        subtitle(head: pull.head, base: pull.base, status: pull.status)
    }

    public static func subtitle(_ pr: PullRequest) -> String {
        subtitle(head: pr.headBranch, base: pr.baseBranch, status: pr.status)
    }

    public static func commentTitle(_ number: Int) -> String { "Comment on PR #\(number)" }

    public static func composerTitle(_ state: String, _ number: Int) -> String {
        switch state {
        case "approve": return "Approve PR #\(number)"
        case "request-changes": return "Request changes on PR #\(number)"
        default: return "Review comment on PR #\(number)"
        }
    }

    public static func composerHelper(_ state: String) -> String {
        switch state {
        case "approve":
            return "Approval is signed and sent to the pull inbox. The repo owner still applies canonical changes from their desktop node."
        case "request-changes":
            return "Request-changes reviews are signed and sent to the pull inbox. Add a clear note so the author knows what to fix."
        default:
            return "Review comments are signed and sent to the pull inbox, pending the repo owner applying them."
        }
    }

    /// The composer's validation: nil lets the trimmed body through.
    public static func validationError(_ state: String, _ body: String) -> String? {
        state == "request-changes" && body.dartTrimmed.isEmpty ? noteRequired : nil
    }
}

/// Change status of one file in a unified diff (`FmChangeKind`).
public enum PatchChangeKind: String, Sendable, CaseIterable {
    case added, modified, deleted, renamed
}

/// Per-file summary parsed from a unified diff (`_PatchFile`).
public struct PatchFile: Equatable, Sendable {
    public let path: String
    public let kind: PatchChangeKind
    public let adds: Int
    public let dels: Int

    public init(_ path: String, _ kind: PatchChangeKind, adds: Int = 0, dels: Int = 0) {
        self.path = path
        self.kind = kind
        self.adds = adds
        self.dels = dels
    }

    /// `_ChangedFileRow`: the directory part, with its trailing slash.
    public var directory: String {
        guard let slash = path.lastIndex(of: "/") else { return "" }
        return String(path[...slash])
    }

    /// `_ChangedFileRow`: the file name.
    public var name: String {
        guard let slash = path.lastIndex(of: "/") else { return path }
        return String(path[path.index(after: slash)...])
    }
}

/// \n, \r\n and \r end lines, and a final terminator adds no empty line.
public func dartLines(_ text: String) -> [String] {
    if text.isEmpty { return [] }
    var lines: [String] = []
    var current: [UInt16] = []
    var previousCR = false
    for unit in text.utf16 {
        if unit == 0x0A {
            if previousCR {
                previousCR = false
                continue
            }
            lines.append(String(decoding: current, as: UTF16.self))
            current.removeAll(keepingCapacity: true)
        } else if unit == 0x0D {
            lines.append(String(decoding: current, as: UTF16.self))
            current.removeAll(keepingCapacity: true)
            previousCR = true
            continue
        } else {
            current.append(unit)
        }
        previousCR = false
    }
    if !current.isEmpty { lines.append(String(decoding: current, as: UTF16.self)) }
    return lines
}

/// `_parsePatchFiles`.
public func parsePatchFiles(_ diff: String) -> [PatchFile] {
    if diff.dartTrimmed.isEmpty { return [] }
    var files: [PatchFile] = []
    var path: String?
    var kind = PatchChangeKind.modified
    var adds = 0
    var dels = 0
    func flush() {
        guard let current = path, !current.isEmpty else { return }
        files.append(PatchFile(current, kind, adds: adds, dels: dels))
    }
    for line in dartLines(diff) {
        if line.hasPrefix("diff --git ") {
            flush()
            path = diffGitPath(line).dartTrimmed
            kind = .modified
            adds = 0
            dels = 0
        } else if line.hasPrefix("new file mode") || line.hasPrefix("--- /dev/null") {
            kind = .added
        } else if line.hasPrefix("deleted file mode") || line.hasPrefix("+++ /dev/null") {
            kind = .deleted
        } else if line.hasPrefix("rename from ") || line.hasPrefix("similarity index") {
            kind = .renamed
        } else if line.hasPrefix("rename to ") {
            path = String(line.dropFirst("rename to ".count)).dartTrimmed
        } else if line.hasPrefix("+++ b/") {
            if path == nil { path = String(line.dropFirst("+++ b/".count)).dartTrimmed }
        } else if line.hasPrefix("+++ ") || line.hasPrefix("--- ") {
            continue
        } else if line.hasPrefix("+") {
            adds += 1
        } else if line.hasPrefix("-") {
            dels += 1
        }
    }
    flush()
    return files
}

/// `RegExp(r'^diff --git a/(.*?) b/(.*)$')`'s second group, else the text
/// after "diff --git ". The lazy first group makes the split the first
/// " b/" after "a/".
private func diffGitPath(_ line: String) -> String {
    let rest = String(line.dropFirst("diff --git ".count))
    if rest.hasPrefix("a/"), let split = rest.range(of: " b/", range: rest.index(rest.startIndex, offsetBy: 2)..<rest.endIndex) {
        return String(rest[split.upperBound...])
    }
    return rest
}

/// `_changedFilesTitle`.
public func changedFilesTitle(_ count: Int) -> String { "\(count) changed \(count == 1 ? "file" : "files")" }

/// The totals `_PatchFilesCard` rolls up (shown only for more than one file).
public func patchFileTotals(_ files: [PatchFile]) -> (adds: Int, dels: Int) {
    files.reduce((0, 0)) { ($0.0 + $1.adds, $0.1 + $1.dels) }
}

/// How `_DiffViewer`'s inline preview tints a raw patch line.
public enum PatchPreviewLineKind: String, Sendable, CaseIterable {
    case added, removed, hunk, context

    public init(line: String) {
        if line.hasPrefix("+") && !line.hasPrefix("+++") {
            self = .added
        } else if line.hasPrefix("-") && !line.hasPrefix("---") {
            self = .removed
        } else if line.hasPrefix("@@") {
            self = .hunk
        } else {
            self = .context
        }
    }
}

/// `showNewPullDialog`.
public enum NewPullComposer {
    public static let title = "New pull request"
    public static let guestAction = "open a pull request"

    /// The dialog's `ok` check: nothing is sent without a title and a patch.
    public static func ready(title: String, patch: String) -> Bool {
        !title.dartTrimmed.isEmpty && !patch.dartTrimmed.isEmpty
    }
}

/// `_PullMergeCard`'s copy.
public enum PullMerge {
    public static let title = "Merge"
    public static let checking = "Checking whether this can be merged from here…"
    public static let merged = "Merged."
    public static let guestAction = "merge a pull request"
    public static let recheck = "Re-check"
    public static let waiverTitle = "Merge without an independent review?"
    public static let waiverCopy =
        "This pull request has not been reviewed by anyone else. As the repository owner you can merge it anyway; the audit log records that the review gate was waived."
    public static let waiverConfirm = "Merge anyway"

    public static func notOwner(_ owner: String) -> String {
        "A merge run by the mesh is addressed to the organization that serves a repository, and this account is not an owner of \(owner). Review this pull request below, or queue the merge for the desktop node."
    }

    public static let notPinned =
        "This pull request could not be pinned to exact commits, so it is not offered for merge here. Use the desktop request below."

    public static func planCopy(_ plan: PullMergePlan, _ pull: PublishedPull) -> String {
        if plan.isReady {
            return "Merges \(pull.head.isEmpty ? "this patch" : pull.head) into \(pull.base), at the exact commits shown to you now. A node runs the merge; if anything moved in between it is refused rather than merged."
        }
        return "The branches conflict, so this cannot be merged as it stands. Resolve the conflict and refresh."
    }

    /// "Conflicts: a, b and 3 more" (the first eight), or nil.
    public static func conflicts(_ plan: PullMergePlan) -> String? {
        if plan.conflicts.isEmpty { return nil }
        let shown = plan.conflicts.prefix(8).joined(separator: ", ")
        let more = plan.conflicts.count > 8 ? " and \(plan.conflicts.count - 8) more" : ""
        return "Conflicts: \(shown)\(more)"
    }

    public static func mergeLabel(busy: Bool) -> String { busy ? "Merging…" : "Merge pull request" }

    public static func mergedNotice(_ number: Int) -> String { "Merged pull request #\(number)." }
}
