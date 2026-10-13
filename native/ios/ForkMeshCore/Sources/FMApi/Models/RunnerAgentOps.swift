import FMBase
import Foundation

public struct AgentBranchStats: Equatable, Hashable, Sendable {
    public var files: Int
    public var added: Int
    public var removed: Int
    public var ahead: Int
    public var behind: Int
    public var conflicted: Bool
    public var known: Bool

    public init(
        files: Int = -1, added: Int = -1, removed: Int = -1, ahead: Int = -1, behind: Int = -1,
        conflicted: Bool = false, known: Bool = false
    ) {
        self.files = files
        self.added = added
        self.removed = removed
        self.ahead = ahead
        self.behind = behind
        self.conflicted = conflicted
        self.known = known
    }

    public init(json: JSONObject) {
        guard json.bool("known") else {
            self.init()
            return
        }
        self.init(
            files: json.int("files"), added: json.int("added"), removed: json.int("removed"),
            ahead: json.int("ahead"), behind: json.int("behind"), conflicted: json.bool("conflicted"), known: true)
    }
}

public struct AgentBranchMerge: Equatable, Hashable, Sendable {
    public var state: String
    public var base: String
    public var mergeOid: String
    public var mergedAtMs: Int
    public var branchDeleted: Bool
    public var cleanupPending: [String]
    public var conflicts: [String]
    public var message: String

    public init(
        state: String = "", base: String = "", mergeOid: String = "", mergedAtMs: Int = 0,
        branchDeleted: Bool = false, cleanupPending: [String] = [], conflicts: [String] = [], message: String = ""
    ) {
        self.state = state
        self.base = base
        self.mergeOid = mergeOid
        self.mergedAtMs = mergedAtMs
        self.branchDeleted = branchDeleted
        self.cleanupPending = cleanupPending
        self.conflicts = conflicts
        self.message = message
    }

    public init(json: JSONObject) {
        self.init(
            state: json.str("state").lowercased(), base: json.str("base"), mergeOid: json.str("mergeOid"),
            mergedAtMs: agentLaunchMs(json["mergedAt"]), branchDeleted: json.bool("branchDeleted"),
            cleanupPending: runnerStrings(json["cleanupPending"]), conflicts: runnerStrings(json["conflicts"]),
            message: json.str("message"))
    }

    public var isMerged: Bool { state == "merged" }
    public var isConflict: Bool { state == "conflict" }
    public var needsCleanup: Bool { isMerged && (!branchDeleted || !cleanupPending.isEmpty) }
}

func runnerStrings(_ value: JSONValue?) -> [String] {
    (value?.arrayValue ?? []).map { ModelJSON.string($0) }.filter { !$0.isEmpty }
}

public struct RunnerAgentDiffPage: Equatable, Sendable {
    public var raw: JSONObject
    public var branch: String
    public var base: String
    public var page: Int
    public var pageSize: Int
    public var pageCount: Int
    public var commits: Int
    public var ahead: Int
    public var behind: Int
    public var clean: Bool?
    public var conflicts: [String]
    public var merged: Bool
    public var fileCount: Int
    public var added: Int
    public var removed: Int
    public var committed: String

    public init(json: JSONObject) {
        raw = json
        branch = json.str("branch")
        base = json.str("base")
        page = max(0, json.int("page"))
        pageSize = json.int("pageSize")
        pageCount = max(1, json.int("pageCount"))
        commits = json.int("commits")
        ahead = json.int("ahead")
        behind = json.int("behind")
        if case .bool(let value)? = json["clean"] { clean = value } else { clean = nil }
        conflicts = runnerStrings(json["conflicts"])
        merged = json.bool("merged")
        let files = json.maps("files")
        fileCount = files.count
        added = json["added"] == nil ? files.reduce(0) { $0 + $1.int("added") } : json.int("added")
        removed = json["removed"] == nil ? files.reduce(0) { $0 + $1.int("removed") } : json.int("removed")
        committed = json.str("committed")
    }

    public var hasMore: Bool { page + 1 < pageCount }

    public var stats: AgentBranchStats {
        AgentBranchStats(
            files: fileCount, added: added, removed: removed, ahead: ahead, behind: behind,
            conflicted: clean == false || !conflicts.isEmpty, known: true)
    }
}

public struct RunnerAgentMergeResult: Equatable, Sendable {
    public var status: String
    public var requestId: String
    public var branch: String
    public var base: String
    public var mergeOid: String
    public var branchDeleted: Bool
    public var cleanupPending: [String]
    public var message: String
    public var raw: JSONObject

    public init(json: JSONObject) {
        raw = json
        status = json.str("status").lowercased()
        requestId = json.str("requestId")
        branch = json.str("branch")
        base = json.str("base")
        mergeOid = json.str("mergeOid")
        branchDeleted = json.bool("branchDeleted")
        cleanupPending = runnerStrings(json["cleanupPending"])
        message = json.str("message")
    }

    public init(pending requestId: String, message: String) {
        self.init(json: ["status": "pending", "requestId": .string(requestId), "message": .string(message)])
    }

    public var isPending: Bool { status == "pending" || status == "processing" }
    public var isMerged: Bool { status == "merged" }
    public var isCleaned: Bool { status == "cleaned" }
    public var isUpdated: Bool { status == "updated" || status == "already" }
}

public struct RunnerAgentPullLink: Equatable, Sendable {
    public var status: String
    public var branch: String
    public var prNumber: Int
    public var message: String

    public init(json: JSONObject) {
        status = json.str("status").lowercased()
        branch = json.str("branch")
        prNumber = json.int("prNumber")
        message = json.str("message")
    }
}

public struct RunnerPullDraft: Equatable, Sendable {
    public var title: String
    public var base: String
    public var head: String
    public var description: String

    public init(title: String, base: String, head: String, description: String) {
        self.title = title
        self.base = base
        self.head = head
        self.description = description
    }

    public init(session: AgentLaunchSession, branch: String, base: String) {
        let head = session.branchName.dartTrimmed.isEmpty ? branch.dartTrimmed : session.branchName.dartTrimmed
        let named = String(session.title.dartTrimmed.prefix(RunnerAgentText.pullTitleLimit)).dartTrimmed
        self.init(
            title: named.isEmpty ? head : named, base: base, head: head,
            description: RunnerAgentText.pullDescription(
                runner: session.runner, targetNode: session.targetNode, branch: head,
                summary: RunnerAgentText.lastAssistantText(session.history)))
    }
}

public enum RunnerAgentText {
    public static let continuePrompt = "Continue where you left off."
    public static let pullTitleLimit = 160
    public static let pullSummaryLimit = 4000
    public static let pullPatchLimit = 4 * 1024 * 1024

    public static func lastAssistantText(_ history: [AgentLaunchMessage]) -> String {
        history.last { $0.role.lowercased() == "assistant" && !$0.text.dartTrimmed.isEmpty }?.text.dartTrimmed ?? ""
    }

    public static func pullDescription(runner: String, targetNode: String, branch: String, summary: String) -> String {
        let runnerName = runner.dartTrimmed
        let node = targetNode.dartTrimmed
        let place = !runnerName.isEmpty ? "the \(runnerName) runner" : (node.isEmpty ? "a ForkMesh node" : node)
        let opening = "Opened from the agent run on \(place) (\(branch))."
        let trimmed = String(summary.dartTrimmed.prefix(pullSummaryLimit))
        return trimmed.isEmpty ? opening : "\(opening)\n\n\(trimmed)"
    }

    public static func pullNoChanges(branch: String, base: String) -> String {
        "\(branch) has no changes against \(base), so there is nothing to open a pull request for."
    }

    public static func pullBranchMissing(_ branch: String) -> String {
        "\(branch) is not on ForkMesh yet, so no pull request was opened."
    }

    public static func pullTooLarge(_ branch: String) -> String {
        "\(branch) is too large for a pull request from the phone. Open it from a desktop."
    }

    public static func pullNotOpened(_ reason: String) -> String {
        let text = reason.dartTrimmed
        return text.isEmpty ? "The pull request could not be opened." : "The pull request could not be opened (\(text))."
    }

    public static let pullNumberPending = "The pull request was sent but has no number yet. Refresh in a moment."

    public static func pullLinkFailed(_ number: Int, _ message: String) -> String {
        "Opened pull request #\(number), but the run could not be linked to it: \(message)"
    }

    public static func pullOpened(_ number: Int) -> String { "Opened pull request #\(number)." }

    public static func linkMessage(_ body: JSONObject, _ status: Int) -> String {
        let message = body.str("message").dartTrimmed
        if !message.isEmpty { return message }
        switch body.str("error").dartTrimmed {
        case "pull_exists": return "This run already has a pull request."
        case "invalid_pull": return "That pull request number is not valid."
        case "no_branch": return "This run has not pushed a branch yet."
        case "forbidden": return "Only an owner of this repository can link a pull request to this run."
        case "invalid_session": return "Sign in again before opening a pull request."
        case "": return "The request was refused (HTTP \(status))."
        case let code: return code.replacingOccurrences(of: "_", with: " ")
        }
    }

    public static func badge(_ runner: String) -> String {
        let name = runner.dartTrimmed
        return name.isEmpty ? "Node" : "Runner \u{00B7} \(name)"
    }

    public static func branchSlug(_ title: String, limit: Int = 40) -> String {
        var out = ""
        var dash = false
        for scalar in title.lowercased().unicodeScalars {
            let ascii = scalar.isASCII && (CharacterSet.alphanumerics.contains(scalar))
            if ascii {
                out.unicodeScalars.append(scalar)
                dash = false
            } else if !out.isEmpty && !dash {
                out.append("-")
                dash = true
            }
            if out.count >= limit { break }
        }
        while out.hasSuffix("-") { out.removeLast() }
        let words = out.split(separator: "-").prefix(5)
        return words.joined(separator: "-")
    }

    public static func taskId(_ session: AgentLaunchSession) -> String {
        let key = session.taskKey.dartTrimmed
        if key.hasPrefix("task:") {
            let id = String(key.dropFirst(5))
            if isTaskId(id) { return id.lowercased() }
        }
        let prompt = session.history.first { $0.role.lowercased() == "user" }?.text ?? ""
        for text in [session.title, prompt] {
            let id = leadingTaskId(text)
            if !id.isEmpty { return id }
        }
        return ""
    }

    public static func leadingTaskId(_ text: String) -> String {
        let trimmed = text.dartTrimmed
        guard trimmed.hasPrefix("[task:"), let close = trimmed.firstIndex(of: "]") else { return "" }
        let id = String(trimmed[trimmed.index(trimmed.startIndex, offsetBy: 6)..<close])
        return isTaskId(id) ? id.lowercased() : ""
    }

    static func isTaskId(_ value: String) -> Bool {
        value.count == 32 && value.lowercased().allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    public static func completionNote(branch: String, base: String, mergeOid: String, message: String) -> String {
        var note = "Merged \(branch) into \(base)"
        let oid = mergeOid.dartTrimmed
        if !oid.isEmpty { note += " (\(oid.prefix(7)))" }
        note += "."
        let said = message.dartTrimmed
        if !said.isEmpty { note += " \(said)" }
        return note
    }

    public static let taskCompleted = "The linked task is marked done."

    public static func taskNotCompleted(_ reason: String) -> String {
        let text = reason.dartTrimmed
        return text.isEmpty
            ? "The merge landed, but the linked task could not be marked done."
            : "The merge landed, but the linked task could not be marked done (\(text))."
    }

    public static func mergeMessage(_ body: JSONObject, _ status: Int) -> String {
        let message = body.str("message").dartTrimmed
        let code = body.str("error").dartTrimmed
        if code == "merge_conflict" {
            let files = runnerStrings(body["conflicts"])
            let head = message.isEmpty ? "The branch conflicts with its base, so nothing was merged." : message
            if files.isEmpty { return head }
            let listed = files.prefix(5).joined(separator: ", ")
            let more = files.count > 5 ? " and \(files.count - 5) more" : ""
            return "\(head) Conflicts: \(listed)\(more)."
        }
        if !message.isEmpty { return message }
        switch code {
        case "stale_base", "stale_head":
            return "The branch or its base moved while this was being merged. Try again."
        case "already_merged": return "This branch is already merged. Clean it up instead."
        case "not_merged": return "This branch is not merged yet, so it was kept. Merge it first."
        case "session_running": return "The agent is still running. Stop it or wait for it to finish, then merge."
        case "no_base": return "This run has no base branch to update from."
        case "merge_status_pending": return "The result is not known yet. Try again."
        case "no_branch": return "This run has not pushed a branch yet."
        case "branch_not_found": return "The branch is not on ForkMesh yet."
        case "forbidden": return "Only an owner of this repository can merge from here."
        case "invalid_session": return "Sign in again before merging."
        case "merge_node_unavailable", "mirror_unavailable":
            return "No node can run the merge right now. Try again shortly."
        case "diff_too_large":
            return "This branch changes more than a phone-sized comparison can carry. Open it on a desktop instead."
        case "": return "The request was refused (HTTP \(status))."
        default: return code.replacingOccurrences(of: "_", with: " ")
        }
    }
}
