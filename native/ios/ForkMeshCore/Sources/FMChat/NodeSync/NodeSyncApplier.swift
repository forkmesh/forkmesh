// Applies drained relay inbox items into an on-device repository's native `.forkmesh/`
// collaboration format, then commits them. It preserves the node's conservative ack contract:
// returning `true` is a promise that the item is durably held by this device, so it only ever
// returns `true` after a real commit (or after recognising an item an earlier sync already
// committed). Everything else (no local working copy, a kind this device cannot write, a
// malformed payload, a failed write or commit) returns `false`, and the row stays on the relay
// for a desktop node.
//
// The formats are the ones the Qt desktop (IssueStore.cpp, DiscussionStore.cpp, PullStore.cpp)
// and the mirror daemon (server/intake.go) already produce, so the clients never fight over the
// same files:
//
// * issues: `.forkmesh/issues/{open,closed}/<n>/issue-<n>.json`, a `forkmesh-issue-v1` record
//   whose `events` array is appended to, encoded with four-space indentation, keys sorted and a
//   trailing newline (QJsonDocument::Indented, Go's json.MarshalIndent(v, "", "    ")).
// * discussions: `.forkmesh/discussions/<n>/discussion.md` for the `open` event and
//   `.forkmesh/discussions/<n>/NNNN-comment.md` (four digits, from 0002) per comment.
// * pulls: `.forkmesh/pulls/<n>/pull.md` plus `changes.patch` / `commits.mbox`, and
//   `.forkmesh/pulls/<n>/NNNN-<type>.md` for comments and reviews, only on the `forkmesh/pulls`
//   ledger branch.
//
// Nothing is pushed. A commit is the durability the ack promises; publishing it is the user's
// explicit Push action.

import FMAccount
import FMApi
import FMBase
import Foundation
import Observation

/// Raised when a drained item cannot be applied durably. Every throw leaves the relay row
/// unacknowledged, so a desktop can still pick the work up.
public struct NodeSyncApplyException: Error, Equatable, CustomStringConvertible, Sendable {
    public let message: String

    public init(_ message: String) { self.message = message }

    public var description: String { message }
}

/// What happened to one drained item this phone was offered.
public enum NodeSyncApplyOutcome: String, Sendable {
    /// Written into the repository's native format and committed.
    case applied
    /// Already applied by an earlier sync; the row may be acked again.
    case alreadyApplied
    /// Left on the relay for a desktop node. See `NodeSyncApplyRecord.reason`.
    case left
}

/// One line of the applier's ledger, for the "Node activity" section.
public struct NodeSyncApplyRecord: Equatable, Sendable {
    public let repoFullName: String
    public let kind: NodeSyncKind
    public let outcome: NodeSyncApplyOutcome
    /// Why the item was left, in the app's own voice. Empty when applied.
    public let reason: String
    public let commitId: String

    public init(repoFullName: String, kind: NodeSyncKind, outcome: NodeSyncApplyOutcome, reason: String = "", commitId: String = "") {
        self.repoFullName = repoFullName
        self.kind = kind
        self.outcome = outcome
        self.reason = reason
        self.commitId = commitId
    }
}

/// One distinct reason items were left, with how often.
public struct NodeSyncLeftReason: Equatable, Hashable, Identifiable, Sendable {
    public let reason: String
    public let count: Int

    public var id: String { reason }

    public init(reason: String, count: Int) {
        self.reason = reason
        self.count = count
    }
}

@MainActor
@Observable
public final class LocalNodeSyncApplier {
    /// The branch the desktop and the mirror daemon keep pull records on.
    nonisolated public static let pullLedgerBranch = "forkmesh/pulls"

    /// Fingerprints of items already committed, so a re-offered row (an ack whose DELETE failed)
    /// is recognised instead of applied twice. A string list in prefs.
    nonisolated public static let appliedPrefsKey = "nodeSync/applied"
    nonisolated public static let appliedLedgerLimit = 500

    /// Most recent ledger lines kept for the activity card.
    nonisolated public static let recentLimit = 20

    @ObservationIgnored private let repos: NodeSyncRepositorySource
    @ObservationIgnored private let author: @MainActor () -> NodeSyncAuthor
    @ObservationIgnored private let prefs: KeyValueStore
    /// `appliedPrefsKey` in the project's namespace: the ledger names one
    /// instance's inbox rows.
    @ObservationIgnored private let ledgerKey: String
    @ObservationIgnored private let clockSource: Clock

    /// `addListener` / `removeListener` for observers outside SwiftUI.
    @ObservationIgnored public let listeners = Listeners()

    @ObservationIgnored private var workspaces: [String: NodeSyncWorkspace] = [:]
    /// Insertion-ordered set: `appliedOrder` holds the order.
    @ObservationIgnored private var applied = Set<String>()
    @ObservationIgnored private var appliedOrder: [String] = []
    @ObservationIgnored private var leftCounts: [(reason: String, count: Int)] = []
    @ObservationIgnored private var queue: Task<Void, Never>?
    @ObservationIgnored private var ledgerLoaded = false
    @ObservationIgnored private var disposed = false

    /// Items written into a local repository and committed by this device.
    public private(set) var appliedTotal = 0
    /// Items handed back to the relay for a desktop node.
    public private(set) var leftTotal = 0
    /// Newest first, at most `recentLimit`.
    public private(set) var recent: [NodeSyncApplyRecord] = []
    /// Distinct reasons items were left, most common first.
    public private(set) var leftReasons: [NodeSyncLeftReason] = []

    /// - Parameters:
    ///   - repos: the on-device repositories (FMCode's store, through an adapter).
    ///   - author: the commit author.
    ///   - prefs: where the applied-fingerprint ledger lives.
    ///   - ledgerKey: its key, `nodeSync/applied` in the project's namespace; the legacy
    ///     namespace by default.
    public init(
        repos: NodeSyncRepositorySource,
        author: @escaping @MainActor () -> NodeSyncAuthor,
        prefs: KeyValueStore,
        ledgerKey: String = LocalNodeSyncApplier.appliedPrefsKey,
        clock: Clock = SystemClock()
    ) {
        self.repos = repos
        self.author = author
        self.prefs = prefs
        self.ledgerKey = ledgerKey
        self.clockSource = clock
    }

    /// Unused today, but the applier owns the clock so a test can pin commit timestamps.
    public var clock: Date { clockSource.now }

    @discardableResult
    public func addListener(_ listener: @escaping @Sendable () -> Void) -> ListenerToken { listeners.add(listener) }

    public func removeListener(_ token: ListenerToken) { listeners.remove(token) }

    /// Install this applier on `service`. Kept as an explicit call so a test (or a future
    /// surface) can offer items to the applier without a live node.
    public func installOn(_ service: NodeSyncService) {
        service.applier = { item in await self.call(item) }
    }

    public func dispose() {
        disposed = true
        for workspace in workspaces.values { workspace.dispose() }
        workspaces.removeAll()
    }

    /// The `NodeSyncApplier` entry point. One item at a time: a repository's worktree must never
    /// see two half-finished applies interleaved.
    public func call(_ item: NodeSyncItem) async -> Bool {
        let previous = queue
        let task = Task { @MainActor () -> Bool in
            await previous?.value
            return await self.apply(item)
        }
        queue = Task { _ = await task.value }
        return await task.value
    }

    private func apply(_ item: NodeSyncItem) async -> Bool {
        if disposed { return false }
        loadLedger()
        let fingerprint = fingerprintOf(item)
        if !fingerprint.isEmpty && applied.contains(fingerprint) {
            // Committed by an earlier sync whose ack never landed. The item is durably held,
            // which is exactly what an ack promises.
            note(item, .alreadyApplied)
            return true
        }
        guard let record = await recordFor(item) else {
            left(item, "no local copy of \(item.repoFullName) on this phone")
            return false
        }
        let workspace = workspaceFor(record)
        do {
            let commit: String
            switch item.kind {
            case .issues: commit = try await applyIssue(workspace, item)
            case .discussions: commit = try await applyDiscussion(workspace, item)
            case .pulls: commit = try await applyPull(workspace, item)
            default:
                // Read-destructive kinds never reach here: they are not in
                // NodeSyncService.ackableKinds, so the service never offers them.
                throw NodeSyncApplyException("\(item.kind.wireName) items are not applied on this device")
            }
            if !fingerprint.isEmpty { remember(fingerprint) }
            note(item, .applied, commitId: commit)
            // The apply is also the relay-ack boundary: finish refreshing the local index before
            // returning, so callers can observe the commit. Skipped once disposed; a refresh
            // failure must not turn a committed apply into a failed one.
            if !disposed {
                do {
                    try await repos.refresh(record.name)
                } catch {
                    Log.warning("nodeSync", "node sync: refresh after apply failed: \(dartErrorText(error))")
                }
            }
            return true
        } catch let error as NodeSyncApplyException {
            left(item, error.message)
            return false
        } catch {
            left(item, LocalNodeSyncApplier.short(dartErrorText(error)))
            return false
        }
    }

    // MARK: Repository resolution

    /// The on-device repository this item belongs to, or nil. Matching is on `publishedAs` only:
    /// that is the repository's mesh identity, and matching a bare local slug could commit a
    /// stranger's issue into an unrelated repository that merely shares a name.
    private func recordFor(_ item: NodeSyncItem) async -> NodeSyncRepoRecord? {
        let wanted = item.repoFullName.lowercased()
        if wanted.isEmpty || !wanted.contains("/") { return nil }
        await repos.ensureLoaded()
        for record in repos.records {
            let published = (record.publishedAs ?? "").dartTrimmed.lowercased()
            if !published.isEmpty && published == wanted { return record }
        }
        return nil
    }

    private func workspaceFor(_ record: NodeSyncRepoRecord) -> NodeSyncWorkspace {
        if let existing = workspaces[record.path] { return existing }
        let created = repos.workspace(for: record)
        workspaces[record.path] = created
        return created
    }

    // MARK: Issues

    private static let issuesRoot = ".forkmesh/issues"

    private func applyIssue(_ workspace: NodeSyncWorkspace, _ item: NodeSyncItem) async throws -> String {
        let root = LocalNodeSyncApplier.issuesRoot
        let event = try eventOf(item)
        let type = str(event["type"])
        if type.isEmpty { throw NodeSyncApplyException("issue event has no type") }
        // Attachments ride along as base64 bytes. The workspace writes text only, so acking here
        // would silently drop the submitter's screenshots.
        if let attachments = item.data["attachmentData"]?.arrayValue, !attachments.isEmpty {
            throw NodeSyncApplyException("issue carries file attachments; a desktop must apply it")
        }
        var number = item.numberOrZero

        func locate(_ candidate: Int) async throws -> String? {
            for directory in ["\(root)/open/\(candidate)", "\(root)/closed/\(candidate)", "\(root)/\(candidate)"] {
                let path = "\(directory)/issue-\(candidate).json"
                if try await readTextOrNil(workspace, path) != nil { return path }
            }
            return nil
        }

        var issue: JSONObject
        var currentPath: String?
        if type == "open" {
            // The relay signs an `open` against number 0 and proposes a slot; the desktop
            // reassigns when the slot is taken, so a collision can never staple a second `open`
            // onto an existing issue.
            if number <= 0 {
                number = await nextNumber(workspace, [root, "\(root)/open", "\(root)/closed"])
            } else if try await locate(number) != nil {
                number = await nextNumber(workspace, [root, "\(root)/open", "\(root)/closed"])
            }
            issue = newIssue(item, event, number)
        } else {
            currentPath = try await locate(number)
            if number <= 0 { throw NodeSyncApplyException("issue event has no issue number to apply to") }
            if let currentPath {
                issue = try decodeIssue(try await readTextOrNil(workspace, currentPath))
            } else {
                issue = placeholderIssue(item, number)
            }
            var events = eventList(issue)
            let id = str(event["id"])
            if !id.isEmpty && events.contains(where: { str($0["id"]) == id }) {
                // The desktop treats an already-merged event as applied, and so does this device.
                return ""
            }
            events.append(event)
            issue["events"] = .array(events.map(JSONValue.object))
        }

        recomputeIssue(&issue)
        let status = str(issue["status"]) == "closed" ? "closed" : "open"
        let targetDirectory = "\(root)/\(status)/\(number)"
        let targetPath = "\(targetDirectory)/issue-\(number).json"
        var touched = [targetPath]
        if let currentPath, currentPath != targetPath {
            // A status change moves the whole folder on the desktop, attachments included. This
            // device can only carry the JSON, so it refuses rather than orphan files.
            let folder = currentPath.range(of: "/", options: .backwards).map { String(currentPath[..<$0.lowerBound]) } ?? ""
            let siblings = await listOrEmpty(workspace, folder)
            if siblings.count > 1 {
                throw NodeSyncApplyException("issue #\(number) has attachments and must change status on a desktop")
            }
            try await workspace.deleteFile(currentPath)
            touched.append(currentPath)
        }
        try await workspace.writeFile(targetPath, LocalNodeSyncApplier.encodeIssueJson(issue))
        let who = LocalNodeSyncApplier.authorLabel(event)
        let message = type == "open"
            ? "issue #\(number): opened (from \(who))"
            : "issue #\(number): \(type) (from \(who))"
        return try await commit(workspace, touched, message)
    }

    private func newIssue(_ item: NodeSyncItem, _ event: JSONObject, _ number: Int) -> JSONObject {
        let meta = item.data["meta"]?.objectValue ?? JSONObject()
        let createdAt = int(event["ts"])
        let reporter = str(meta["reporter"])
        return [
            "schema": "forkmesh-issue-v1",
            "number": .int(Int64(number)),
            "title": .string(str(event["title"])),
            "status": "open",
            "labels": JSONValue(stringList(meta["labels"])),
            "milestone": .string(str(meta["milestone"])),
            "startDate": 0,
            "endDate": 0,
            "priority": .int(Int64(min(max(int(meta["priority"]), 0), 8999))),
            "progress": 0,
            "assignees": JSONValue(stringList(meta["assignees"])),
            "createdAt": .int(Int64(createdAt)),
            "updatedAt": .int(Int64(createdAt)),
            "author": .string(str(event["author"])),
            "authorName": .string(reporter.isEmpty ? str(event["authorName"]) : reporter),
            "bountyUsd": 0,
            "bountyAddress": "",
            "bountyStatus": "",
            "votes": 0,
            "events": .array([.object(event)]),
        ]
    }

    /// The desktop's stand-in when an event arrives for an issue this device has never seen: a
    /// synthetic `open` with an empty signature.
    private func placeholderIssue(_ item: NodeSyncItem, _ number: Int) -> JSONObject {
        let title = str(item.data["titleIfNew"])
        return [
            "schema": "forkmesh-issue-v1",
            "number": .int(Int64(number)),
            "title": .string(title),
            "status": "open",
            "labels": [],
            "milestone": "",
            "startDate": 0,
            "endDate": 0,
            "priority": 0,
            "progress": 0,
            "assignees": [],
            "createdAt": 0,
            "updatedAt": 0,
            "author": "",
            "authorName": "",
            "bountyUsd": 0,
            "bountyAddress": "",
            "bountyStatus": "",
            "votes": 0,
            "events": [
                [
                    "type": "open",
                    "id": .string("open-\(number)"),
                    "title": .string(title),
                    "body": "",
                    "author": "",
                    "authorName": "",
                    "ts": 0,
                    "sig": "",
                ],
            ],
        ]
    }

    /// Replay every event in arrival order; the last writer of a field wins. Mirrors
    /// `IssueStore::recomputeMetadata` plus the `updatedAt`/`votes` derivations of `Issue::toJson`.
    private func recomputeIssue(_ issue: inout JSONObject) {
        let events = eventList(issue)
        var votes = 0
        var updatedAt = int(issue["createdAt"])
        for event in events {
            let ts = int(event["ts"])
            if ts > updatedAt { updatedAt = ts }
            switch str(event["type"]) {
            case "open":
                issue["title"] = .string(str(event["title"]))
            case "title":
                let title = str(event["title"])
                if !title.isEmpty { issue["title"] = .string(title) }
            case "status":
                issue["status"] = .string(str(event["status"]))
            case "labels":
                issue["labels"] = JSONValue(stringList(event["labels"]))
            case "milestone":
                issue["milestone"] = .string(str(event["milestone"]))
            case "dates":
                issue["startDate"] = .int(Int64(int(event["startDate"])))
                issue["endDate"] = .int(Int64(int(event["endDate"])))
            case "priority":
                issue["priority"] = .int(Int64(int(event["priority"])))
            case "progress":
                issue["progress"] = .int(Int64(int(event["progress"])))
            case "bounty":
                issue["bountyUsd"] = NodeSyncParse.nonNull(event["bountyUsd"]) ?? .int(0)
                issue["bountyAddress"] = .string(str(event["bountyAddress"]))
                issue["bountyStatus"] = .string(str(event["bountyStatus"]))
            case "assignees":
                issue["assignees"] = JSONValue(stringList(event["assignees"]))
            case "vote":
                votes += 1
            default:
                break
            }
        }
        issue["votes"] = .int(Int64(votes))
        issue["updatedAt"] = .int(Int64(updatedAt))
    }

    private func decodeIssue(_ text: String?) throws -> JSONObject {
        guard let text else { throw NodeSyncApplyException("issue record is missing") }
        let decoded: JSONValue
        do {
            decoded = try JSON.parse(text)
        } catch {
            throw NodeSyncApplyException("issue record is not valid JSON")
        }
        guard let object = decoded.objectValue else { throw NodeSyncApplyException("issue record is not an object") }
        return object
    }

    private func eventList(_ issue: JSONObject) -> [JSONObject] {
        (issue["events"]?.arrayValue ?? []).compactMap(\.objectValue)
    }

    // MARK: Discussions

    private static let discussionsRoot = ".forkmesh/discussions"

    private func applyDiscussion(_ workspace: NodeSyncWorkspace, _ item: NodeSyncItem) async throws -> String {
        let root = LocalNodeSyncApplier.discussionsRoot
        let event = try eventOf(item)
        let type = str(event["type"])
        // The desktop accepts exactly these two and refuses the rest.
        if type != "open" && type != "comment" {
            throw NodeSyncApplyException("unsupported discussion event \"\(type.isEmpty ? "unknown" : type)\"")
        }
        var number = item.numberOrZero
        let who = LocalNodeSyncApplier.authorLabel(event)

        if type == "open" {
            if number <= 0 {
                number = await nextNumber(workspace, [root])
            } else if try await readTextOrNil(workspace, "\(root)/\(number)/discussion.md") != nil {
                number = await nextNumber(workspace, [root])
            }
            let path = "\(root)/\(number)/discussion.md"
            try await workspace.writeFile(path, LocalNodeSyncApplier.frontMatterDocument(
                fields: [
                    ("schema", "forkmesh-discussion-v1"),
                    ("number", "\(number)"),
                    ("title", str(event["title"])),
                    ("category", str(event["category"])),
                    ("status", "open"),
                    ("createdAt", "\(int(event["ts"]))"),
                    ("author", str(event["author"])),
                    ("authorName", str(event["authorName"])),
                    ("type", "open"),
                    ("id", str(event["id"])),
                    ("ts", "\(int(event["ts"]))"),
                    ("sig", str(event["sig"])),
                ],
                body: str(event["body"])))
            return try await commit(workspace, [path], "discussion #\(number): opened (from \(who))")
        }

        if number <= 0 { throw NodeSyncApplyException("discussion comment has no discussion number") }
        var touched: [String] = []
        let rootPath = "\(root)/\(number)/discussion.md"
        if try await readTextOrNil(workspace, rootPath) == nil {
            // Same stand-in the desktop writes for a comment on a discussion it has never seen,
            // down to the "Ideas" category and the empty signature.
            try await workspace.writeFile(rootPath, LocalNodeSyncApplier.frontMatterDocument(
                fields: [
                    ("schema", "forkmesh-discussion-v1"),
                    ("number", "\(number)"),
                    ("title", str(item.data["titleIfNew"])),
                    ("category", "Ideas"),
                    ("status", "open"),
                    ("createdAt", "0"),
                    ("author", ""),
                    ("authorName", ""),
                    ("type", "open"),
                    ("id", "open-\(number)"),
                    ("ts", "0"),
                    ("sig", ""),
                ],
                body: ""))
            touched.append(rootPath)
        }
        let id = str(event["id"])
        let existing = await listOrEmpty(workspace, "\(root)/\(number)")
        if !id.isEmpty {
            for entry in existing where LocalNodeSyncApplier.isCommentFile(entry.name) {
                if let text = try await readTextOrNil(workspace, entry.path), text.contains("\nid: \(id)\n") {
                    return ""
                }
            }
        }
        // `discussion.md` is event one, so comments start at 0002. The desktop renumbers
        // positionally and never leaves gaps, which makes max+1 the same answer.
        let index = LocalNodeSyncApplier.nextEventIndex(existing, LocalNodeSyncApplier.isCommentFile)
        let path = "\(root)/\(number)/\(LocalNodeSyncApplier.pad4(index))-comment.md"
        try await workspace.writeFile(path, LocalNodeSyncApplier.frontMatterDocument(
            fields: [
                ("type", "comment"),
                ("id", id),
                ("author", str(event["author"])),
                ("authorName", str(event["authorName"])),
                ("ts", "\(int(event["ts"]))"),
                ("sig", str(event["sig"])),
            ],
            body: str(event["body"])))
        touched.append(path)
        return try await commit(workspace, touched, "discussion #\(number): comment (from \(who))")
    }

    // MARK: Pulls

    private static let pullsRoot = ".forkmesh/pulls"

    private func applyPull(_ workspace: NodeSyncWorkspace, _ item: NodeSyncItem) async throws -> String {
        let summary = try await workspace.summary()
        if summary.branch != LocalNodeSyncApplier.pullLedgerBranch {
            throw NodeSyncApplyException(
                "pull records live on the \(LocalNodeSyncApplier.pullLedgerBranch) ledger branch, which this "
                    + "phone does not have checked out")
        }
        if let pull = item.data["pull"]?.objectValue {
            return try await applyNewPull(workspace, item, pull)
        }
        return try await applyPullEvent(workspace, item)
    }

    private func applyNewPull(_ workspace: NodeSyncWorkspace, _ item: NodeSyncItem, _ pull: JSONObject) async throws -> String {
        let root = LocalNodeSyncApplier.pullsRoot
        var number = int(pull["number"])
        if number <= 0 { number = item.numberOrZero }
        if number <= 0 {
            number = await nextNumber(workspace, [root])
        } else if try await readTextOrNil(workspace, "\(root)/\(number)/pull.md") != nil {
            number = await nextNumber(workspace, [root])
        }
        let branchBacked = str(pull["derive"]) == "branch"
        var touched: [String] = []
        let path = "\(root)/\(number)/pull.md"
        var fields: [(String, String)] = [
            ("schema", "forkmesh-pull-v1"),
            ("number", "\(number)"),
            ("title", str(pull["title"])),
            ("base", str(pull["base"])),
            ("head", str(pull["head"])),
            // The desktop forces an inbox pull open regardless of what the submitter proposed.
            ("status", "open"),
        ]
        if branchBacked { fields.append(("derive", "branch")) }
        for key in ["creationBaseOid", "creationHeadOid", "mergeBase", "mergeHead"] where !str(pull[key]).isEmpty {
            fields.append((key, str(pull[key])))
        }
        fields.append(("ts", "\(int(pull["ts"]))"))
        fields.append(("author", str(pull["author"])))
        fields.append(("authorName", str(pull["authorName"])))
        fields.append(("sig", str(pull["sig"])))
        for key in ["client", "clientSig"] where !str(pull[key]).isEmpty {
            fields.append((key, str(pull[key])))
        }
        try await workspace.writeFile(path, LocalNodeSyncApplier.frontMatterDocument(fields: fields, body: str(pull["description"])))
        touched.append(path)
        // A branch-backed pull carries no patch: its content is the branch.
        if !branchBacked {
            let patch = str(pull["patch"])
            if !patch.isEmpty {
                let patchPath = "\(root)/\(number)/changes.patch"
                try await workspace.writeFile(patchPath, patch)
                touched.append(patchPath)
            }
            let commits = str(pull["commits"])
            if !commits.isEmpty {
                let mboxPath = "\(root)/\(number)/commits.mbox"
                try await workspace.writeFile(mboxPath, commits)
                touched.append(mboxPath)
            }
        }
        return try await commit(workspace, touched, "pull #\(number): opened (from \(LocalNodeSyncApplier.authorLabel(pull)))")
    }

    private func applyPullEvent(_ workspace: NodeSyncWorkspace, _ item: NodeSyncItem) async throws -> String {
        let root = LocalNodeSyncApplier.pullsRoot
        let event = try eventOf(item)
        let type = str(event["type"])
        // `PullStore::applyRemoteEvent` only ever commits a comment or a review; the richer thread
        // types need diff context this device does not carry.
        if type != "comment" && type != "review" {
            throw NodeSyncApplyException("unsupported pull event \"\(type.isEmpty ? "unknown" : type)\"")
        }
        let number = item.numberOrZero
        if number <= 0 { throw NodeSyncApplyException("pull event has no pull number") }
        if try await readTextOrNil(workspace, "\(root)/\(number)/pull.md") == nil {
            throw NodeSyncApplyException("pull #\(number) is not in this repository")
        }
        let id = str(event["id"])
        let existing = await listOrEmpty(workspace, "\(root)/\(number)")
        if !id.isEmpty {
            for entry in existing where LocalNodeSyncApplier.isPullEventFile(entry.name) {
                if let text = try await readTextOrNil(workspace, entry.path), text.contains("\nid: \(id)\n") {
                    return ""
                }
            }
        }
        let index = LocalNodeSyncApplier.nextEventIndex(existing, LocalNodeSyncApplier.isPullEventFile)
        let path = "\(root)/\(number)/\(LocalNodeSyncApplier.pad4(index))-\(type).md"
        var fields: [(String, String)] = [
            ("type", type),
            ("id", id),
            ("author", str(event["author"])),
            ("authorName", str(event["authorName"])),
            ("ts", "\(int(event["ts"]))"),
        ]
        if type == "review" { fields.append(("state", str(event["state"]))) }
        fields.append(("sig", str(event["sig"])))
        try await workspace.writeFile(path, LocalNodeSyncApplier.frontMatterDocument(fields: fields, body: str(event["body"])))
        return try await commit(workspace, [path], "pull #\(number): \(type) (from \(LocalNodeSyncApplier.authorLabel(event)))")
    }

    // MARK: Git

    /// Stage exactly the paths this apply touched and commit them. Narrower than the desktop's
    /// `git add -A -- .forkmesh/<kind>`: a phone's worktree may hold the user's own uncommitted
    /// edits under `.forkmesh/`, and the node has no business sweeping those into its commit.
    private func commit(_ workspace: NodeSyncWorkspace, _ touched: [String], _ message: String) async throws -> String {
        try await workspace.stage(touched)
        do {
            return try await workspace.commit(message: message, author: author())
        } catch {
            // The desktop treats "nothing to commit" as success: the content is already in the
            // tree, so the item really is durably held.
            if let known = error as? NodeSyncWorkspaceError, known == .nothingStaged { return "" }
            let text = dartErrorText(error)
            if text.contains("Nothing is staged") { return "" }
            throw NodeSyncApplyException("commit failed: \(LocalNodeSyncApplier.short(text))")
        }
    }

    /// `max(numeric folder name across roots) + 1`, the desktop's allocator.
    private func nextNumber(_ workspace: NodeSyncWorkspace, _ roots: [String]) async -> Int {
        var highest = 0
        for root in roots {
            for entry in await listOrEmpty(workspace, root) where entry.isDirectory {
                let value = NodeSyncParse.dartIntTryParse(entry.name) ?? 0
                if value > highest { highest = value }
            }
        }
        return highest + 1
    }

    /// `^\d{4}-comment\.md$`.
    nonisolated static func isCommentFile(_ name: String) -> Bool {
        hasFourDigitPrefix(name) && name.dropFirst(4) == "-comment.md"
    }

    /// `^\d{4}-[a-z-]+\.md$`.
    nonisolated static func isPullEventFile(_ name: String) -> Bool {
        guard hasFourDigitPrefix(name), name.hasSuffix(".md") else { return false }
        let middle = name.dropFirst(4).dropLast(3)
        guard middle.first == "-", middle.count >= 2 else { return false }
        return middle.dropFirst().allSatisfy { ($0.isASCII && $0.isLetter && $0.isLowercase) || $0 == "-" }
    }

    nonisolated private static func hasFourDigitPrefix(_ name: String) -> Bool {
        let head = name.prefix(4)
        return head.count == 4 && head.allSatisfy { $0.isASCII && $0.isNumber }
    }

    /// The next `NNNN-` slot. `discussion.md` / `pull.md` occupy slot one, so a directory with
    /// no event files yields 2.
    nonisolated static func nextEventIndex(_ entries: [NodeSyncFileEntry], _ matches: (String) -> Bool) -> Int {
        var highest = 1
        for entry in entries where matches(entry.name) {
            let value = NodeSyncParse.dartIntTryParse(String(entry.name.prefix(4))) ?? 0
            if value > highest { highest = value }
        }
        return highest + 1
    }

    /// `index.toString().padLeft(4, '0')`.
    nonisolated static func pad4(_ index: Int) -> String {
        let text = String(index)
        return text.count >= 4 ? text : String(repeating: "0", count: 4 - text.count) + text
    }

    private func listOrEmpty(_ workspace: NodeSyncWorkspace, _ directory: String) async -> [NodeSyncFileEntry] {
        (try? await workspace.listFiles(directory)) ?? []
    }

    /// The file's text, or nil when it does not exist. A file that exists but cannot be read as
    /// text is fatal: overwriting it would destroy data.
    private func readTextOrNil(_ workspace: NodeSyncWorkspace, _ path: String) async throws -> String? {
        let content: NodeSyncFileContent
        do {
            content = try await workspace.readFile(path)
        } catch {
            return nil
        }
        if content.isBinary || content.isTooLarge {
            throw NodeSyncApplyException("\(path) is not readable as text")
        }
        return content.text
    }

    // MARK: Formatting

    /// `---\n<key: value>...\n---\n\n<body>\n`, byte for byte with the desktop's
    /// `writeDiscussionFile` / `writePull` / `writeEventFile`.
    nonisolated public static func frontMatterDocument(fields: [(String, String)], body: String) -> String {
        var lines = ["---"]
        for (key, value) in fields { lines.append("\(key): \(frontMatterValue(value))") }
        lines.append("---")
        lines.append("")
        return lines.joined(separator: "\n") + "\n" + body + "\n"
    }

    /// Front matter is one line per key, so a value can never carry a newline.
    nonisolated static func frontMatterValue(_ value: String) -> String {
        // Swift treats "\r\n" as one Character, so work on scalars.
        var out = String.UnicodeScalarView()
        var scalars = value.unicodeScalars.makeIterator()
        var pending = scalars.next()
        while let scalar = pending {
            pending = scalars.next()
            if scalar == "\r" {
                out.append(" ")
                if pending == "\n" { pending = scalars.next() }
            } else if scalar == "\n" {
                out.append(" ")
            } else {
                out.append(scalar)
            }
        }
        return String(out)
    }

    /// Four-space indentation, keys sorted, trailing newline: the bytes `QJsonDocument::Indented`
    /// and Go's `json.MarshalIndent` both produce, so a desktop rewrite is a no-op diff.
    nonisolated public static func encodeIssueJson(_ issue: JSONObject) -> String {
        JSON.encodePretty(sorted(.object(issue)), indent: "    ") + "\n"
    }

    /// Keys in UTF-16 code unit order, recursively.
    nonisolated static func sorted(_ value: JSONValue) -> JSONValue {
        switch value {
        case .object(let object):
            let keys = object.keys.sorted { $0.utf16.lexicographicallyPrecedes($1.utf16) }
            return .object(JSONObject(keys.map { ($0, sorted(object[$0] ?? .null)) }))
        case .array(let items):
            return .array(items.map(sorted))
        default:
            return value
        }
    }

    // MARK: Ledger and bookkeeping

    private func eventOf(_ item: NodeSyncItem) throws -> JSONObject {
        guard let event = item.data["event"]?.objectValue else { throw NodeSyncApplyException("item carries no event") }
        return event
    }

    /// Stable identity for one submission: the signed event id, else its signature, else the
    /// relay row. Scoped per repository and kind.
    func fingerprintOf(_ item: NodeSyncItem) -> String {
        let prefix = "\(item.repoFullName)|\(item.kind.wireName)|"
        for key in ["event", "pull"] {
            guard let source = item.data[key]?.objectValue else { continue }
            let id = str(source["id"])
            if !id.isEmpty { return prefix + id }
            let sig = str(source["sig"])
            if !sig.isEmpty { return prefix + sig }
        }
        return item.id > 0 ? "\(prefix)row:\(item.id)" : ""
    }

    private func loadLedger() {
        if ledgerLoaded { return }
        ledgerLoaded = true
        for fingerprint in prefs.stringList(forKey: ledgerKey) ?? [] {
            if applied.insert(fingerprint).inserted { appliedOrder.append(fingerprint) }
        }
    }

    private func remember(_ fingerprint: String) {
        if applied.insert(fingerprint).inserted { appliedOrder.append(fingerprint) }
        if appliedOrder.count > LocalNodeSyncApplier.appliedLedgerLimit {
            appliedOrder = Array(appliedOrder.suffix(LocalNodeSyncApplier.appliedLedgerLimit))
            applied = Set(appliedOrder)
        }
        prefs.set(appliedOrder, forKey: ledgerKey)
    }

    private func left(_ item: NodeSyncItem, _ reason: String) {
        leftTotal += 1
        if let index = leftCounts.firstIndex(where: { $0.reason == reason }) {
            leftCounts[index].count += 1
        } else {
            leftCounts.append((reason, 1))
        }
        // Stable sort, most common first; ties keep first-seen order.
        leftReasons = leftCounts
            .enumerated()
            .sorted { $0.element.count != $1.element.count ? $0.element.count > $1.element.count : $0.offset < $1.offset }
            .map { NodeSyncLeftReason(reason: $0.element.reason, count: $0.element.count) }
        note(item, .left, reason: reason)
    }

    private func note(_ item: NodeSyncItem, _ outcome: NodeSyncApplyOutcome, reason: String = "", commitId: String = "") {
        if outcome == .applied { appliedTotal += 1 }
        recent.insert(
            NodeSyncApplyRecord(repoFullName: item.repoFullName, kind: item.kind, outcome: outcome, reason: reason, commitId: commitId),
            at: 0)
        if recent.count > LocalNodeSyncApplier.recentLimit { recent.removeLast() }
        if !disposed { listeners.notify() }
    }

    nonisolated static func short(_ text: String) -> String {
        let single = text.replacingOccurrences(of: "\n", with: " ").dartTrimmed
        return single.utf16.count <= 120 ? single : NodeSyncParse.utf16Prefix(single, 119) + "\u{2026}"
    }

    private func str(_ value: JSONValue?) -> String { NodeSyncParse.string(value) }

    private func int(_ value: JSONValue?) -> Int { NodeSyncParse.int(value) }

    private func stringList(_ value: JSONValue?) -> [String] {
        (value?.arrayValue ?? []).compactMap { entry in
            let text = entry.text.dartTrimmed
            return text.isEmpty ? nil : text
        }
    }

    /// `authorName`, else the first eight characters of the signing key: the same `(from X)`
    /// suffix the desktop puts in its commit messages.
    nonisolated static func authorLabel(_ event: JSONObject) -> String {
        let name = NodeSyncParse.string(event["authorName"])
        if !name.isEmpty { return name }
        let author = NodeSyncParse.string(event["author"])
        return author.utf16.count > 8 ? NodeSyncParse.utf16Prefix(author, 8) : author
    }
}
