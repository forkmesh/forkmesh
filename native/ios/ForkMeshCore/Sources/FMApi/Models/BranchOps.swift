import FMBase
import Foundation

public enum BranchOpKind: String, Equatable, Hashable, Sendable {
    case merge, update, delete
}

public struct BranchOpRequest: Equatable, Hashable, Sendable {
    public var op: BranchOpKind
    public var branch: String
    public var expectedOid: String
    public var baseOid: String
    public var base: String
    public var deleteBranch: Bool
    public var defaultBranch: String
    public var requestId: String
    public var node: String
    public var pinBase: String
    public var pinOid: String

    public init(
        op: BranchOpKind, branch: String, expectedOid: String, baseOid: String = "", base: String = "",
        deleteBranch: Bool = false, defaultBranch: String = "", requestId: String = "", node: String = "",
        pinBase: String = "", pinOid: String = ""
    ) {
        self.op = op
        self.branch = branch
        self.expectedOid = expectedOid
        self.baseOid = baseOid
        self.base = base
        self.deleteBranch = deleteBranch
        self.defaultBranch = defaultBranch
        self.requestId = requestId
        self.node = node
        self.pinBase = pinBase
        self.pinOid = pinOid
    }

    public static func merge(
        _ branch: RepoBranch, into base: RepoBranch, deleteBranch: Bool, defaultBranch: String
    ) -> BranchOpRequest {
        BranchOpRequest(
            op: .merge, branch: branch.name, expectedOid: branch.sha.lowercased(), baseOid: base.sha.lowercased(),
            base: base.name, deleteBranch: deleteBranch, defaultBranch: defaultBranch)
    }

    public static func update(_ branch: RepoBranch, from base: String, defaultBranch: String) -> BranchOpRequest {
        BranchOpRequest(
            op: .update, branch: branch.name, expectedOid: branch.sha.lowercased(), base: base,
            defaultBranch: defaultBranch)
    }

    public static func delete(_ branch: RepoBranch, defaultBranch: String) -> BranchOpRequest {
        BranchOpRequest(
            op: .delete, branch: branch.name, expectedOid: branch.sha.lowercased(), defaultBranch: defaultBranch)
    }

    public static func validOid(_ value: String) -> Bool {
        let hex = value.lowercased()
        guard hex.count == 40 || hex.count == 64 else { return false }
        return hex.allSatisfy { ("0"..."9").contains($0) || ("a"..."f").contains($0) }
    }

    public var isPinned: Bool {
        guard BranchOpRequest.validOid(expectedOid), !branch.isEmpty else { return false }
        return op != .merge || BranchOpRequest.validOid(baseOid)
    }

    public var isResuming: Bool { !requestId.isEmpty && !node.isEmpty }

    public var body: JSONObject {
        var out: JSONObject = [
            "op": .string(op.rawValue), "branch": .string(branch), "expectedOid": .string(expectedOid),
        ]
        if op == .merge {
            out["baseOid"] = .string(baseOid)
            out["deleteBranch"] = .bool(deleteBranch)
        }
        if op != .delete && !base.isEmpty { out["base"] = .string(base) }
        if !defaultBranch.isEmpty { out["defaultBranch"] = .string(defaultBranch) }
        if isResuming {
            out["requestId"] = .string(requestId)
            out["node"] = .string(node)
            out["pinBase"] = .string(pinBase)
            if !pinOid.isEmpty { out["pinOid"] = .string(pinOid) }
        }
        return out
    }

    public func resumed(_ answer: BranchOpAnswer) -> BranchOpRequest {
        guard !answer.requestId.isEmpty, !answer.node.isEmpty else { return self }
        var next = self
        next.requestId = answer.requestId
        next.node = answer.node
        next.pinBase = answer.pinBase
        next.pinOid = answer.pinOid
        return next
    }
}

public struct BranchOpAnswer: Equatable, Sendable {
    public var httpStatus: Int
    public var ok: Bool
    public var executed: Bool
    public var queued: Bool
    public var status: String
    public var error: String
    public var requestId: String
    public var node: String
    public var pinBase: String
    public var pinOid: String
    public var branch: String
    public var base: String
    public var mergeOid: String
    public var branchDeleted: Bool
    public var cleanupPending: [String]
    public var conflicts: [String]
    public var message: String

    public init(json: JSONObject, httpStatus: Int) {
        func strings(_ key: String) -> [String] {
            (json[key]?.arrayValue ?? []).map { ModelJSON.string($0) }.filter { !$0.isEmpty }
        }
        self.httpStatus = httpStatus
        ok = json.bool("ok")
        executed = json.bool("executed")
        queued = json.bool("queued")
        status = json.str("status").dartTrimmed.lowercased()
        error = json.str("error").dartTrimmed
        requestId = json.str("requestId").dartTrimmed
        node = json.str("node").dartTrimmed
        pinBase = json.str("pinBase").dartTrimmed
        pinOid = json.str("pinOid").dartTrimmed
        branch = json.str("branch")
        base = json.str("base")
        mergeOid = json.str("mergeOid").dartTrimmed
        branchDeleted = json.bool("branchDeleted")
        cleanupPending = strings("cleanupPending")
        conflicts = strings("conflicts")
        message = json.str("message").dartTrimmed
    }

    public var isProcessing: Bool {
        (200..<300).contains(httpStatus) && (httpStatus == 202 || status == "processing")
    }

    public var isRetryable: Bool {
        httpStatus == 503 && (error == "merge_status_pending" || error == "merge_node_unavailable")
    }

    public var isExecuted: Bool {
        (200..<300).contains(httpStatus) && executed && ["merged", "updated", "deleted", "already"].contains(status)
    }

    public var isQueued: Bool { (200..<300).contains(httpStatus) && !executed && (queued || ok) }

    public var isConflict: Bool { error == "merge_conflict" }
}

public struct BranchOpOutcome: Equatable, Sendable {
    public enum Kind: Equatable, Sendable { case done, queued, pending, failed }

    public var kind: Kind
    public var request: BranchOpRequest
    public var answer: BranchOpAnswer?
    public var message: String

    public init(kind: Kind, request: BranchOpRequest, answer: BranchOpAnswer?, message: String) {
        self.kind = kind
        self.request = request
        self.answer = answer
        self.message = message
    }

    public var conflicts: [String] { answer?.conflicts ?? [] }
    public var isConflict: Bool { answer?.isConflict ?? false }
    public var mergeOid: String { answer?.mergeOid ?? "" }

    public var landedOnDefault: Bool {
        guard kind == .done, request.op == .merge, answer?.status == "merged" else { return false }
        return !request.defaultBranch.isEmpty && request.base == request.defaultBranch
    }
}

public enum BranchOpText {
    public static let ownerOnly = "Only the repository owner can manage branches."
    public static let notPinned = "This branch list has no commit for that branch. Refresh and try again."

    public static func mergeTitle(_ base: String) -> String { "Merge into \(base)" }
    public static let mergeDeleteTitle = "Merge & delete"
    public static func updateTitle(_ base: String) -> String { "Update from \(base)" }
    public static let deleteTitle = "Delete"

    public static func deleteConfirm(_ branch: String) -> String {
        "Deletes \(branch) on ForkMesh. Commits that are not merged anywhere else are kept only as a recovery ref."
    }

    public static func progress(_ request: BranchOpRequest) -> String {
        let base = request.base.isEmpty ? (request.defaultBranch.isEmpty ? "the default branch" : request.defaultBranch) : request.base
        switch request.op {
        case .merge: return "Merging \(request.branch) into \(base)…"
        case .update: return "Updating \(request.branch) from \(base)…"
        case .delete: return "Deleting \(request.branch)…"
        }
    }

    public static func queued(_ request: BranchOpRequest) -> String {
        let action: String
        switch request.op {
        case .merge: action = "The merge of \(request.branch)"
        case .update: action = "The update of \(request.branch)"
        case .delete: action = "Deleting \(request.branch)"
        }
        return "\(action) is queued. It runs when the owner's desktop is online."
    }

    public static func stillWorking(_ request: BranchOpRequest) -> String {
        "The core is still working on \(request.branch). Try again to check on the same request."
    }

    public static func done(_ answer: BranchOpAnswer, _ request: BranchOpRequest) -> String {
        if !answer.message.isEmpty { return answer.message }
        let base = answer.base.isEmpty ? request.base : answer.base
        switch answer.status {
        case "merged": return "Merged \(request.branch) into \(base)."
        case "updated": return "Updated \(request.branch) from \(base)."
        case "deleted": return "Deleted \(request.branch)."
        default: return "Nothing to do: \(request.branch) is already there."
        }
    }

    public static func failure(_ answer: BranchOpAnswer) -> String {
        if answer.httpStatus == 403 || answer.error == "forbidden" { return ownerOnly }
        var head = answer.message
        if head.isEmpty {
            switch answer.error {
            case "merge_conflict": head = "The branches conflict, so nothing changed."
            case "stale_base", "stale_head": head = "A branch moved since you looked. Nothing changed; refresh and try again."
            case "unsupported_branch", "default_branch": head = "The default branch cannot be changed this way."
            case "invalid_branch": head = "That branch name cannot be used."
            case "invalid_commit": head = BranchOpText.notPinned
            case "invalid_session": head = "Sign in again to manage branches."
            case "queue_full": head = "Too many branch changes are waiting for the desktop. Try again later."
            case "merge_status_pending", "merge_node_unavailable": head = "The core is not answering yet. Try again."
            case "": head = "The request was refused (HTTP \(answer.httpStatus))."
            default: head = answer.error.replacingOccurrences(of: "_", with: " ")
            }
        }
        if answer.isConflict && !answer.conflicts.isEmpty {
            let listed = answer.conflicts.prefix(5).joined(separator: ", ")
            let more = answer.conflicts.count > 5 ? " and \(answer.conflicts.count - 5) more" : ""
            return "\(head) Conflicts: \(listed)\(more)."
        }
        return head
    }
}
