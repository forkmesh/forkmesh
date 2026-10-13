// Models for the relay's owner-scope agent queue:
// `/api/repo/<owner>/<repo>/agent-sessions`.
//
// This is a different surface from the owner-sealed `/agents` snapshots the
// Agents tab has always read. Those are end-to-end encrypted to owner devices
// and a phone without the vault key can only count them. These rows are
// relay-readable by design: they are the *queue*, the thing a client writes to
// when it wants an agent to start, and the thing it reads back to see how far
// the run has got. Which is exactly what a phone needs in order to go from a
// typed prompt to a branch and a pull request without a desktop in the loop.

import Foundation
import FMBase

/// `_ms`: the relay writes seconds in some columns and milliseconds in others;
/// anything below 10^11 cannot be a millisecond timestamp in any year this
/// code will run in. Strings may be ISO dates or numbers.
func agentLaunchMs(_ value: JSONValue?, timeZone: TimeZone = .current) -> Int {
    func scale(_ n: Int) -> Int { n > 0 && n < 100_000_000_000 ? n * 1_000 : n }
    switch value {
    case .int?, .double?:
        return scale(ModelJSON.int(value))
    case .string(let text)?:
        if let parsed = DartCore.dateTimeTryParseMs(text, timeZone: timeZone) { return parsed }
        let trimmed = text.dartTrimmed
        if let n = DartCore.intTryParse(trimmed) { return scale(n) }
        if let d = DartCore.doubleTryParse(trimmed) { return scale(ModelJSON.int(.double(d))) }
        return 0
    default:
        return 0
    }
}

/// `_strings`: a list's entries as text, blanks dropped; anything else is empty.
private func launchStrings(_ value: JSONValue?) -> [String] {
    (value?.arrayValue ?? []).map { ModelJSON.string($0) }.filter { !$0.isEmpty }
}

/// A node that can execute agent work for this repository.
public struct AgentLaunchTarget: Equatable, Hashable, Sendable {
    public var node: String
    public var providers: [String]
    public var online: Bool
    public var lastSeenAtMs: Int
    /// The relay marks the repository's own source node with `source: true`;
    /// every other target is a mirror of it.
    public var source: String
    /// `desktop` or `headless`, when the relay knows it.
    public var runtimeMode: String
    public var runner: String
    public var needsLogin: [String]
    public var queueNode: String
    public var runnerPin: String

    public init(
        node: String, providers: [String] = [], online: Bool = false, lastSeenAtMs: Int = 0, source: String = "",
        runtimeMode: String = "", runner: String = "", needsLogin: [String] = [], queueNode: String = "",
        runnerPin: String = ""
    ) {
        self.node = node
        self.providers = providers
        self.online = online
        self.lastSeenAtMs = lastSeenAtMs
        self.source = source
        self.runtimeMode = runtimeMode
        self.runner = runner
        self.needsLogin = needsLogin
        self.queueNode = queueNode
        self.runnerPin = runnerPin
    }

    public init(json: JSONObject) {
        self.init(
            node: ModelJSON.string(json["node"]),
            providers: launchStrings(json["providers"]),
            online: json.bool("online"),
            lastSeenAtMs: agentLaunchMs(json["lastSeenAt"]),
            source: ModelJSON.string(json["source"]),
            runtimeMode: ModelJSON.string(json["runtimeMode"]),
            runner: ModelJSON.string(json["runner"]),
            needsLogin: json["needsLogin"]?.objectValue?.keys ?? [],
            queueNode: ModelJSON.string(json["queueNode"]),
            runnerPin: ModelJSON.string(json["runnerPin"])
        )
    }

    public var isSource: Bool { source.lowercased() == "true" }

    /// A mirror node runs headless and is the one that can be relied on to be
    /// there when the phone is the only thing awake. The owner route names the
    /// runtime; the organization route only says which target is the source.
    public var isMirror: Bool {
        let runtime = runtimeMode.lowercased()
        if !runtime.isEmpty { return runtime == "headless" || runtime.contains("mirror") || isRunner }
        return !isSource
    }

    public var isRunner: Bool { runtimeMode.lowercased().contains("runner") }

    public var signInNode: String {
        let named = runner.dartTrimmed
        return isRunner && !named.isEmpty ? named : node
    }

    public var kindLabel: String { isRunner ? "Runner" : isMirror ? "Mirror" : "Source node" }

    public func runs(_ provider: String) -> Bool {
        provider.isEmpty || provider == "auto" || providers.contains(provider)
    }

    public var statusLabel: String { online ? "online" : "offline" }
}

/// One turn in a queued session's relay-readable history.
public struct AgentLaunchMessage: Equatable, Hashable, Sendable {
    public var role: String
    public var author: String
    public var text: String
    public var atMs: Int
    public var state: String
    public var attachments: [AgentAttachmentMeta]

    public init(
        role: String = "", author: String = "", text: String = "", atMs: Int = 0, state: String = "",
        attachments: [AgentAttachmentMeta] = []
    ) {
        self.role = role
        self.author = author
        self.text = text
        self.atMs = atMs
        self.state = state
        self.attachments = attachments
    }

    public init(json: JSONObject) {
        self.init(
            role: ModelJSON.string(json["role"]),
            author: ModelJSON.string(json["author"]),
            text: ModelJSON.string(json["text"]),
            atMs: agentLaunchMs(json["at"]),
            state: ModelJSON.string(json["state"]),
            attachments: AgentAttachmentMeta.list(json["attachments"], limit: PromptAttachmentLimits.maxCount)
        )
    }

    public var isUser: Bool { role.lowercased() == "user" }

    public var isStopRequest: Bool { state.lowercased() == "stopping" }

    public var isTask: Bool { isUser && !isStopRequest }
}

/// A session on the relay queue.
public struct AgentLaunchSession: Equatable, Hashable, Sendable, Identifiable {
    public var sessionId: String
    public var owner: String
    public var repo: String
    public var provider: String
    public var status: String
    public var displayStatus: String
    public var targetNode: String
    public var title: String
    public var createdBy: String
    public var createdAtMs: Int
    public var updatedAtMs: Int
    public var completedAtMs: Int
    public var requestedModel: String
    public var createPr: Bool
    public var issueNumber: Int
    public var history: [AgentLaunchMessage]
    public var diagnostic: String
    /// The node's own id for the run. Zero until a node has claimed the job,
    /// which is also when the relay starts accepting follow-up prompts.
    public var localAgentId: Int
    public var taskKey: String
    /// The branch the node cut for this run, once it has named one.
    public var branchName: String
    public var baseBranch: String
    /// The pull request the node opened from `branchName`; zero until it has.
    public var prNumber: Int
    /// What the node reported. A headless mirror never learns that its pull
    /// request was merged, so the pull ledger is the better witness: see
    /// `AgentTaskChain`.
    public var merged: Bool
    public var mergedAtMs: Int
    public var lastError: String
    /// Whether this row lives on the organization route rather than the
    /// owner's, so follow-ups and deletes go back to where it came from.
    public var orgScope: Bool
    public var liveText: String
    public var liveAtMs: Int
    public var stopRequestedAtMs: Int
    public var runner: String = ""
    public var branchStats = AgentBranchStats()
    public var branchMerge = AgentBranchMerge()
    public var canMerge = false
    public var canOpenPull = false
    public var queue: AgentRunQueue?

    public var id: String { sessionId }

    public var onRunner: Bool { !runner.dartTrimmed.isEmpty }

    public var isMergedBranch: Bool { merged || branchMerge.isMerged }

    public var diagnosticNote: String {
        let note = diagnostic.dartTrimmed
        return note == lastError.dartTrimmed ? "" : note
    }

    public func attachmentRefs(_ message: AgentLaunchMessage, owner: String, repo: String) -> [AgentAttachmentRef] {
        message.attachments.map {
            AgentAttachmentRef(meta: $0, origin: .relay(owner: owner, repo: repo, sessionId: sessionId, orgScope: orgScope))
        }
    }

    public init(
        sessionId: String, owner: String = "", repo: String = "", provider: String = "", status: String = "",
        displayStatus: String = "", targetNode: String = "", title: String = "", createdBy: String = "",
        createdAtMs: Int = 0, updatedAtMs: Int = 0, completedAtMs: Int = 0, requestedModel: String = "",
        createPr: Bool = false, issueNumber: Int = 0, history: [AgentLaunchMessage] = [], diagnostic: String = "",
        localAgentId: Int = 0, taskKey: String = "", branchName: String = "", baseBranch: String = "",
        prNumber: Int = 0, merged: Bool = false, mergedAtMs: Int = 0, lastError: String = "",
        orgScope: Bool = false, liveText: String = "", liveAtMs: Int = 0, stopRequestedAtMs: Int = 0
    ) {
        self.sessionId = sessionId
        self.owner = owner
        self.repo = repo
        self.provider = provider
        self.status = status
        self.displayStatus = displayStatus
        self.targetNode = targetNode
        self.title = title
        self.createdBy = createdBy
        self.createdAtMs = createdAtMs
        self.updatedAtMs = updatedAtMs
        self.completedAtMs = completedAtMs
        self.requestedModel = requestedModel
        self.createPr = createPr
        self.issueNumber = issueNumber
        self.history = history
        self.diagnostic = diagnostic
        self.localAgentId = localAgentId
        self.taskKey = taskKey
        self.branchName = branchName
        self.baseBranch = baseBranch
        self.prNumber = prNumber
        self.merged = merged
        self.mergedAtMs = mergedAtMs
        self.lastError = lastError
        self.orgScope = orgScope
        self.liveText = liveText
        self.liveAtMs = liveAtMs
        self.stopRequestedAtMs = stopRequestedAtMs
    }

    public init(json: JSONObject, orgScope: Bool = false) {
        let info = json.obj("agentInfo")
        let live = json.obj("live")
        func text(_ value: JSONValue?) -> String { ModelJSON.string(value) }
        // The node reports the run in `agentInfo`; a few relays flatten the same
        // facts onto the row. The row wins when both speak.
        func fact(_ key: String) -> String {
            let row = text(json[key])
            return row.isEmpty ? text(info[key]) : row
        }
        let issue = ModelJSON.int(json["issueNumber"])
        let pr = ModelJSON.int(json["prNumber"])
        let baseBranch = fact("baseBranch")
        let id = text(json["id"])
        self.init(
            sessionId: id.isEmpty ? text(json["sessionId"]) : id,
            owner: text(json["org"]),
            repo: text(json["repo"]),
            provider: text(json["provider"]),
            status: text(json["status"]),
            displayStatus: text(json["displayStatus"]),
            targetNode: text(json["targetNode"]),
            title: text(json["title"]),
            createdBy: text(json["createdBy"]),
            createdAtMs: agentLaunchMs(json["createdAt"]),
            updatedAtMs: agentLaunchMs(json["updatedAt"]),
            completedAtMs: agentLaunchMs(json["completedAt"]),
            requestedModel: text(json["requestedModel"]),
            createPr: json.bool("createPr") || info.bool("createPr"),
            issueNumber: issue > 0 ? issue : ModelJSON.int(info["issueNumber"]),
            history: json.maps("history").map(AgentLaunchMessage.init(json:)),
            diagnostic: json["diagnostic"]?.objectValue.map { text($0["message"]) } ?? "",
            localAgentId: ModelJSON.int(json["localAgentId"]),
            taskKey: text(json["taskKey"]),
            branchName: text(json["branch"]).isEmpty ? fact("branchName") : text(json["branch"]),
            baseBranch: baseBranch.isEmpty ? fact("baseRef") : baseBranch,
            prNumber: pr > 0 ? pr : ModelJSON.int(info["prNumber"]),
            merged: json.bool("merged") || info.bool("merged"),
            mergedAtMs: agentLaunchMs(info["mergedAt"]),
            lastError: fact("lastError"),
            orgScope: orgScope,
            liveText: ModelJSON.string(live["text"]),
            liveAtMs: agentLaunchMs(live["at"]),
            stopRequestedAtMs: agentLaunchMs(json["stopRequestedAt"])
        )
        runner = text(json["runner"])
        branchStats = AgentBranchStats(json: json.obj("diffStats"))
        branchMerge = AgentBranchMerge(json: json.obj("merge"))
        canMerge = json.bool("canMerge")
        canOpenPull = json.bool("canOpenPull")
        queue = json["queue"]?.objectValue.flatMap { AgentRunQueue(json: $0) }
        if branchMerge.isMerged {
            merged = true
            if mergedAtMs == 0 { mergedAtMs = branchMerge.mergedAtMs }
        }
    }

    public var normalizedStatus: String { ModelJSON.normalizedStatus(status) }

    public var isActive: Bool { ["security-pending", "queued", "running"].contains(normalizedStatus) }

    public var isComplete: Bool { normalizedStatus == "completed" }

    public var isFailed: Bool { normalizedStatus == "failed" || normalizedStatus == "rejected" }

    /// A run that ended before it finished because something stopped it: the
    /// node was told to, or it went away mid-run. The relay writes `failed`
    /// for most of those; a node that reports the stop by name gets read here
    /// rather than falling through to an unknown status.
    public var isStopped: Bool {
        ["stopped", "cancelled", "canceled", "interrupted"].contains(normalizedStatus)
            || (stopRequestedAtMs > 0 && isFailed)
    }

    /// The relay takes a follow-up only once a node has claimed the run.
    public var isPromptable: Bool {
        localAgentId > 0 && ["queued", "running", "completed", "failed"].contains(normalizedStatus)
    }

    /// What this session was first asked to do.
    public var firstPrompt: String { history.first { $0.isTask }?.text.dartTrimmed ?? "" }

    /// What running this session again would send. The task it was started
    /// with, because that is the work; the title only stands in when the
    /// history has been trimmed past the opening prompt.
    public var retryPrompt: String { firstPrompt.isEmpty ? title.dartTrimmed : firstPrompt }

    public func retryPrompt(followUp: String) -> String {
        [retryPrompt, followUp.dartTrimmed].filter { !$0.isEmpty }.joined(separator: "\n\n")
    }

    public var restartsOnPrompt: Bool { !isPromptable && isRetryable }

    /// Whether this run ended without finishing and can be put back to work.
    /// A run with nothing left to send is not offered: the relay refuses an
    /// empty prompt, so a button for it would only ever fail.
    public var isRetryable: Bool { (isFailed || isStopped) && !retryPrompt.isEmpty }

    /// Whether a retry picks the existing run back up instead of queueing a
    /// new one. A node that claimed the session still holds its branch and its
    /// provider conversation, so the task carries on from where it stopped
    /// (the desktop's Continue). One that never got that far has nothing to
    /// resume, and has to be queued again from the top.
    public var canResume: Bool { isPromptable }

    public var statusLabel: String {
        if isStopped { return "stopped" }
        return displayStatus.isEmpty ? status.replacingOccurrences(of: "_", with: " ") : displayStatus
    }

    public var displayTitle: String {
        let trimmedTitle = title.dartTrimmed
        if !trimmedTitle.isEmpty { return trimmedTitle }
        let text = history.first { $0.isTask }?.text.dartTrimmed ?? ""
        if !text.isEmpty { return text.components(separatedBy: "\n").first ?? text }
        return "Agent session"
    }
}

/// What `GET /agent-sessions` answers: where an agent can run, what it can be
/// asked to run as, and what is already queued.
public struct AgentLaunchOptions: Equatable, Hashable, Sendable {
    /// True when these came from the organization route
    /// (`/api/orgs/<org>/repos/<repo>/agent-bots`) because the signed-in
    /// account does not own the repository's node.
    public var orgScope: Bool
    /// Why this account may not queue, in the relay's words.
    public var accessReason: String
    public var canQueue: Bool
    public var targets: [AgentLaunchTarget]
    public var providers: [String]
    public var models: [String: [String]]
    public var sessions: [AgentLaunchSession]
    public var finishedBeforeMs: Int

    public init(
        canQueue: Bool = false, targets: [AgentLaunchTarget] = [], providers: [String] = [],
        models: [String: [String]] = [:], sessions: [AgentLaunchSession] = [], orgScope: Bool = false,
        accessReason: String = "", finishedBeforeMs: Int = 0
    ) {
        self.canQueue = canQueue
        self.targets = targets
        self.providers = providers
        self.models = models
        self.sessions = sessions
        self.orgScope = orgScope
        self.accessReason = accessReason
        self.finishedBeforeMs = finishedBeforeMs
    }

    public init(json: JSONObject, orgScope: Bool = false) {
        var models: [String: [String]] = [:]
        if let raw = json["models"]?.objectValue {
            for (key, value) in raw { models[key] = launchStrings(value) }
        }
        self.init(
            canQueue: json.bool("canQueueAgent") || json.bool("engineeringAccess"),
            targets: json.maps("targets").map(AgentLaunchTarget.init(json:)),
            providers: launchStrings(json["providers"]),
            models: models,
            sessions: json.maps("sessions").map { AgentLaunchSession(json: $0, orgScope: orgScope) },
            orgScope: orgScope,
            accessReason: ModelJSON.string(json["accessReason"]),
            finishedBeforeMs: agentLaunchMs(json["finishedBefore"])
        )
    }

    public var sessionList: AgentSessionList {
        AgentSessionList(sessions: sessions, finishedBeforeMs: finishedBeforeMs)
    }

    public var hasTarget: Bool { !targets.isEmpty }

    /// The node to queue on when the person did not choose one. An online
    /// mirror first: it is headless and always up, so the run starts now and
    /// does not depend on a desktop staying awake. Then any online node, then
    /// a mirror that will claim the job when it next connects.
    public var preferredTarget: AgentLaunchTarget? {
        if targets.isEmpty { return nil }
        return targets.first { $0.online && $0.isMirror }
            ?? targets.first { $0.online }
            ?? targets.first { $0.isMirror }
            ?? targets[0]
    }

    public func targetNamed(_ node: String) -> AgentLaunchTarget? {
        if node.isEmpty { return nil }
        return targets.first { $0.node == node }
            ?? targets.first { $0.isRunner && $0.runner == node }
            ?? targets.first { $0.isRunner && $0.runnerPin.isEmpty && $0.queueNode == node }
    }
}

/// How far one stage of an agent task has got.
public enum AgentStageState: String, Sendable, CaseIterable {
    case pending, active, done, failed
}

/// One step on the way from a prompt to a merge.
public struct AgentTaskStage: Equatable, Hashable, Sendable {
    /// `prompt`, `mirror`, `branch`, `pull` or `merge`.
    public let key: String
    public let label: String
    public let detail: String
    public let state: AgentStageState

    public init(key: String, label: String, detail: String, state: AgentStageState) {
        self.key = key
        self.label = label
        self.detail = detail
        self.state = state
    }
}

/// The whole life of a queued agent task, in the order it happens: the
/// prompt is sent, a mirror claims it, the agent commits to a branch, the
/// mirror opens a pull request from that branch, and the pull request is
/// merged. This is the same chain the web session page lays out as its
/// Mirror / Branch / PR rows, read from the same fields.
public struct AgentTaskChain: Equatable, Hashable, Sendable {
    public let session: AgentLaunchSession
    /// The pull request for this task: the one the node reported, or failing
    /// that the one the ledger holds for the task's branch.
    public let prNumber: Int
    /// The pull request's status in the repository's pull ledger (`open`,
    /// `merged`, `closed`), when it has been read. The ledger outranks the
    /// node's report: a mirror does not hear about a merge made elsewhere.
    public let pullStatus: String

    public init(_ session: AgentLaunchSession, pullStatus: String = "", pullNumber: Int = 0) {
        self.session = session
        self.pullStatus = pullStatus
        self.prNumber = session.prNumber > 0 ? session.prNumber : pullNumber
    }

    public var isMerged: Bool { session.merged || pullStatus.dartTrimmed.lowercased() == "merged" }

    public var isPullClosed: Bool { pullStatus.dartTrimmed.lowercased() == "closed" }

    public var stages: [AgentTaskStage] {
        let s = session
        let claimed = s.localAgentId > 0 || !s.branchName.isEmpty
            || ["running", "completed"].contains(s.normalizedStatus)
        let node = s.targetNode.isEmpty ? "a mirror" : s.targetNode
        let hasBranch = !s.branchName.isEmpty
        let hasPull = prNumber > 0
        let failed = s.isFailed

        func after(_ reached: Bool, _ previous: Bool) -> AgentStageState {
            if reached { return .done }
            if failed { return previous ? .failed : .pending }
            return previous ? .active : .pending
        }

        let mirror: AgentStageState = claimed ? (s.isActive && !hasBranch ? .active : .done) : after(false, true)
        let branch: AgentStageState = hasBranch ? (s.isActive && !hasPull ? .active : .done) : after(false, claimed)
        // A finished run with a branch and no pull request is not a failure: the
        // mirror only opens one when the branch actually moved.
        let pull: AgentStageState = hasPull ? .done : (failed && hasBranch && s.createPr ? .failed : .pending)
        let merge: AgentStageState = isMerged ? .done : isPullClosed ? .failed : hasPull ? .active : .pending

        let mirrorDetail: String
        switch mirror {
        case .done: mirrorDetail = "Ran on \(node)"
        case .active: mirrorDetail = claimed ? "Running on \(node)" : "Waiting for \(node) to pick it up"
        case .failed: mirrorDetail = "Never started on \(node)"
        case .pending: mirrorDetail = node
        }
        let branchDetail: String
        if hasBranch {
            branchDetail = s.baseBranch.isEmpty ? s.branchName : "\(s.branchName) from \(s.baseBranch)"
        } else {
            branchDetail = branch == .failed ? "No branch was pushed" : "Named when the mirror starts work"
        }
        let pullDetail: String
        if hasPull {
            pullDetail = "#\(prNumber)"
        } else if !s.createPr {
            pullDetail = "Not requested"
        } else if pull == .failed {
            pullDetail = "Not opened"
        } else if s.isComplete {
            pullDetail = "None was opened — the branch did not change"
        } else {
            pullDetail = "Opens when the agent finishes"
        }
        let mergeDetail: String
        if isMerged {
            mergeDetail = "Merged"
        } else if isPullClosed {
            mergeDetail = "Closed without merging"
        } else if hasPull {
            mergeDetail = "Open — review and merge #\(prNumber)"
        } else {
            mergeDetail = "After the pull request"
        }

        return [
            AgentTaskStage(
                key: "prompt", label: "Prompt",
                detail: s.createdBy.isEmpty ? "Sent" : "Sent by @\(s.createdBy)", state: .done),
            AgentTaskStage(key: "mirror", label: "Mirror", detail: mirrorDetail, state: mirror),
            AgentTaskStage(key: "branch", label: "Branch", detail: branchDetail, state: branch),
            AgentTaskStage(key: "pull", label: "Pull request", detail: pullDetail, state: pull),
            AgentTaskStage(key: "merge", label: "Merge", detail: mergeDetail, state: merge),
        ]
    }

    /// The furthest stage reached, for a one-line summary in a list row.
    public var summary: String {
        if isMerged { return "Merged" }
        if prNumber > 0 { return isPullClosed ? "PR #\(prNumber) closed" : "PR #\(prNumber) open" }
        if !session.branchName.isEmpty { return "On \(session.branchName)" }
        return ""
    }
}

/// The relay's answer to a queue request.
public struct AgentQueueResult: Equatable, Hashable, Sendable {
    public var queueState: String
    public var targetOnline: Bool
    public var message: String
    public var session: AgentLaunchSession?

    public init(queueState: String = "", targetOnline: Bool = false, message: String = "", session: AgentLaunchSession? = nil) {
        self.queueState = queueState
        self.targetOnline = targetOnline
        self.message = message
        self.session = session
    }

    public init(json: JSONObject) {
        self.init(
            queueState: ModelJSON.string(json["queueState"]),
            targetOnline: json.bool("targetOnline"),
            message: ModelJSON.string(json["message"]),
            session: json["session"]?.objectValue.map { AgentLaunchSession(json: $0) }
        )
    }

    /// True when the run is waiting for a node that is not connected yet. The
    /// job is durable either way; this only changes what the person is told.
    public var waitingForNode: Bool { queueState == "waiting_for_desktop" || !targetOnline }

    public var label: String {
        if !message.isEmpty { return message }
        return waitingForNode
            ? "Queued. It starts as soon as the node connects."
            : "Queued. The node is picking it up now."
    }
}

/// What became of a retry: the relay's answer, and which of the two things a
/// retry can be it turned out to be.
public struct AgentRetryResult: Equatable, Hashable, Sendable {
    public var result: AgentQueueResult
    /// True when the failed run itself was resumed, false when it was queued
    /// again as a new session because there was nothing to resume.
    public var resumed: Bool

    public init(result: AgentQueueResult, resumed: Bool) {
        self.result = result
        self.resumed = resumed
    }

    /// The new session, when the retry started one. A resumed run keeps its
    /// own id and the relay answers without a session, so this is nil there.
    public var session: AgentLaunchSession? { result.session }

    public var label: String {
        if !result.message.isEmpty { return result.message }
        return resumed ? "Sent back to the mirror. It picks the session up on its branch." : result.label
    }
}

/// Raised when the relay refuses an agent request, carrying the message it
/// wrote for a person rather than a bare status code.
public struct AgentLaunchException: Error, Equatable, Sendable, CustomStringConvertible {
    public let message: String
    public let code: String
    public let status: Int

    public init(_ message: String, code: String = "", status: Int = 0) {
        self.message = message
        self.code = code
        self.status = status
    }

    public var description: String { message }
}

/// Everything needed to merge one pull request, pinned to exact object ids.
///
/// A merge that names branches instead of commits merges whatever those
/// branches point at when the node gets round to it, which is not necessarily
/// what was reviewed. Every field here is an immutable object id for that
/// reason, and the relay rejects the request if any of them has moved.
public struct PullMergePlan: Equatable, Hashable, Sendable {
    public var number: Int
    public var expectedBaseOid: String
    public var expectedHeadOid: String
    public var expectedPullsOid: String
    public var branchBacked: Bool
    /// Whether a three-way merge trial on the mirror came back without
    /// conflicts. False means the merge would stop partway, so the button says
    /// so rather than starting it.
    public var clean: Bool
    public var conflicts: [String]

    public init(
        number: Int, expectedBaseOid: String, expectedHeadOid: String, expectedPullsOid: String,
        branchBacked: Bool = true, clean: Bool = true, conflicts: [String] = []
    ) {
        self.number = number
        self.expectedBaseOid = expectedBaseOid
        self.expectedHeadOid = expectedHeadOid
        self.expectedPullsOid = expectedPullsOid
        self.branchBacked = branchBacked
        self.clean = clean
        self.conflicts = conflicts
    }

    /// All three ids come from the same repository, so they are the same
    /// length. A mismatch means one of the reads answered with something other
    /// than a commit id, and the merge must not be attempted.
    public var isConsistent: Bool {
        !expectedBaseOid.isEmpty && !expectedHeadOid.isEmpty && !expectedPullsOid.isEmpty
            && Set([expectedBaseOid.utf16.count, expectedHeadOid.utf16.count, expectedPullsOid.utf16.count]).count == 1
    }

    public var isReady: Bool { isConsistent && clean }
}

/// What became of a merge request.
public struct PullMergeOutcome: Equatable, Hashable, Sendable {
    public var merged: Bool
    public var message: String
    /// The node accepted the job and is still working. The merge is durable;
    /// this only means the answer did not arrive inside the poll window.
    public var pending: Bool

    public init(merged: Bool, message: String = "", pending: Bool = false) {
        self.merged = merged
        self.message = message
        self.pending = pending
    }
}
