// The agent launch model cases (target choice, the prompt-to-merge chain) plus the
// parsing rules the ApiService cases lean on.

import XCTest
import FMBase
@testable import FMApi

final class AgentLaunchTests: XCTestCase {
    func testAHeadlessOwnerNodeIsPreferredOverAnOnlineDesktop() {
        let options = AgentLaunchOptions(json: [
            "canQueueAgent": true,
            "targets": [
                ["node": "desk-1", "online": true, "source": true, "runtimeMode": "desktop"],
                ["node": "vultr-1", "online": true, "source": false, "runtimeMode": "headless"],
            ],
        ])
        XCTAssertEqual(options.preferredTarget?.node, "vultr-1")
        XCTAssertFalse(options.targets[0].isMirror)
        XCTAssertTrue(options.targets[0].isSource)
    }

    func testListingPicksAnOnlineNodeOverAnOfflineOne() {
        let options = AgentLaunchOptions(json: [
            "ok": true, "canQueueAgent": true,
            "targets": [["node": "sleepy", "online": false], ["node": "mirror-7", "online": true, "source": "mirror"]],
            "sessions": [[
                "id": "sess-9", "status": "running", "targetNode": "mirror-7", "title": "Fix the crash",
                "history": [["role": "user", "text": "Fix the crash"]],
            ]],
        ])
        XCTAssertTrue(options.canQueue)
        XCTAssertEqual(options.preferredTarget?.node, "mirror-7")
        XCTAssertEqual(options.preferredTarget?.isMirror, true)
        XCTAssertEqual(options.sessions.first?.isActive, true)
        XCTAssertEqual(options.sessions.first?.displayTitle, "Fix the crash")
        XCTAssertEqual(options.targetNamed("sleepy")?.statusLabel, "offline")
        XCTAssertNil(options.targetNamed("nobody"))
        XCTAssertNil(AgentLaunchOptions().preferredTarget)
    }

    func testOptionsReadModelsProvidersAndScope() {
        let options = AgentLaunchOptions(
            json: [
                "engineeringAccess": true, "accessReason": "why", "providers": ["claude-code", "", nil, "codex"],
                "models": ["codex": ["gpt-5", ""], "odd": "nope"],
                "sessions": [["sessionId": "s1"]],
            ],
            orgScope: true)
        XCTAssertTrue(options.canQueue)
        XCTAssertEqual(options.accessReason, "why")
        XCTAssertEqual(options.providers, ["claude-code", "codex"])
        XCTAssertEqual(options.models["codex"], ["gpt-5"])
        XCTAssertEqual(options.models["odd"], [])
        XCTAssertEqual(options.sessions.first?.sessionId, "s1")
        XCTAssertEqual(options.sessions.first?.orgScope, true)
        let target = AgentLaunchTarget(node: "n", providers: ["codex"])
        XCTAssertTrue(target.runs("auto"))
        XCTAssertTrue(target.runs(""))
        XCTAssertTrue(target.runs("codex"))
        XCTAssertFalse(target.runs("openai"))
        XCTAssertTrue(AgentLaunchTarget(node: "m", runtimeMode: "cloud-mirror").isMirror)
    }

    func testTimestampsReadSecondsMillisecondsAndDates() {
        XCTAssertEqual(agentLaunchMs(1_700_000_000), 1_700_000_000_000)
        XCTAssertEqual(agentLaunchMs(1_700_000_000_000), 1_700_000_000_000)
        XCTAssertEqual(agentLaunchMs(" 1700000000"), 1_700_000_000_000)
        // A bare ten-digit string reads as a compact date first (year
        // 170000, month 0, day 0), as DartCore.dateTimeTryParseMs does.
        XCTAssertEqual(agentLaunchMs("1700000000"), DartCore.dateTimeTryParseMs("1700000000"))
        XCTAssertNotEqual(agentLaunchMs("1700000000"), 1_700_000_000_000)
        XCTAssertEqual(agentLaunchMs(" 1.7e9 "), 1_700_000_000_000)
        XCTAssertEqual(agentLaunchMs("2026-07-04T12:00:00Z"), 1_783_166_400_000)
        XCTAssertEqual(agentLaunchMs("soon"), 0)
        XCTAssertEqual(agentLaunchMs(nil), 0)
        XCTAssertEqual(agentLaunchMs(true), 0)
        let message = AgentLaunchMessage(json: ["role": "User", "text": "hi", "at": 5])
        XCTAssertTrue(message.isUser)
        XCTAssertEqual(message.atMs, 5_000)
    }

    func testSessionPrefersRowFactsAndFallsBackToAgentInfo() {
        let session = AgentLaunchSession(json: [
            "sessionId": "s", "org": "acme", "status": "failed", "issueNumber": 0, "localAgentId": "3",
            "diagnostic": ["message": "boom"], "branchName": "",
            "agentInfo": [
                "branchName": "agent/x", "baseRef": "main", "prNumber": 9, "issueNumber": "4", "merged": true,
                "mergedAt": 1_700_000_000, "createPr": true, "lastError": "info error",
            ],
            "lastError": "row error",
        ])
        XCTAssertEqual(session.sessionId, "s")
        XCTAssertEqual(session.owner, "acme")
        XCTAssertEqual(session.branchName, "agent/x")
        XCTAssertEqual(session.baseBranch, "main")
        XCTAssertEqual(session.prNumber, 9)
        XCTAssertEqual(session.issueNumber, 4)
        XCTAssertTrue(session.merged)
        XCTAssertEqual(session.mergedAtMs, 1_700_000_000_000)
        XCTAssertTrue(session.createPr)
        XCTAssertEqual(session.lastError, "row error")
        XCTAssertEqual(session.diagnostic, "boom")
        XCTAssertEqual(session.localAgentId, 3)
        XCTAssertTrue(session.isPromptable)
        XCTAssertTrue(session.canResume)
    }

    func testRetryRules() {
        let failed = AgentLaunchSession(
            sessionId: "a", status: "failed", title: "  Title  ",
            history: [AgentLaunchMessage(role: "agent", text: "x"), AgentLaunchMessage(role: "user", text: " Do it \nnow")])
        XCTAssertEqual(failed.firstPrompt, "Do it \nnow")
        XCTAssertEqual(failed.retryPrompt, "Do it \nnow")
        XCTAssertTrue(failed.isRetryable)
        XCTAssertFalse(failed.canResume, "no node ever claimed it")
        XCTAssertEqual(failed.displayTitle, "Title")
        XCTAssertEqual(AgentLaunchSession(sessionId: "b", history: failed.history).displayTitle, "Do it ")
        XCTAssertEqual(AgentLaunchSession(sessionId: "c").displayTitle, "Agent session")
        // A session that kept no task is not retried blind.
        XCTAssertFalse(AgentLaunchSession(sessionId: "sess-9", status: "failed").isRetryable)
        XCTAssertTrue(AgentLaunchSession(sessionId: "d", status: "Interrupted", title: "t").isStopped)
        XCTAssertFalse(AgentLaunchSession(sessionId: "e", status: "completed", title: "t").isRetryable)
        XCTAssertEqual(AgentLaunchSession(sessionId: "f", status: "security_pending").statusLabel, "security pending")
        XCTAssertTrue(AgentLaunchSession(sessionId: "f", status: "security_pending").isActive)
        XCTAssertEqual(AgentLaunchSession(sessionId: "g", status: "x", displayStatus: "Shown").statusLabel, "Shown")
    }

    func testARelayStoppedRunIsStoppedAndRetriesItsTask() {
        let stopped = AgentLaunchSession(json: [
            "id": "s", "status": "failed", "title": "Fix login", "localAgentId": 4, "stopRequestedAt": 1_700_000_000_000,
            "history": [
                ["role": "user", "text": "fix the login form"],
                ["role": "agent", "text": "working"],
                ["role": "user", "text": "Stop this run.", "state": "stopping"],
            ],
        ])
        XCTAssertEqual(stopped.stopRequestedAtMs, 1_700_000_000_000)
        XCTAssertTrue(stopped.isStopped)
        XCTAssertTrue(stopped.isRetryable)
        XCTAssertTrue(stopped.canResume)
        XCTAssertEqual(stopped.statusLabel, "stopped")
        XCTAssertEqual(stopped.retryPrompt, "fix the login form")
        XCTAssertEqual(AgentRunLog(AgentTaskChain(stopped), repo: "r").status, "stopped")

        var trimmed = stopped
        trimmed.history = [AgentLaunchMessage(role: "user", text: "Stop this run.", state: "stopping")]
        trimmed.title = ""
        XCTAssertEqual(trimmed.retryPrompt, "")
        XCTAssertFalse(trimmed.isRetryable)
        XCTAssertEqual(trimmed.displayTitle, "Agent session")
        trimmed.title = "Fix login"
        XCTAssertEqual(trimmed.retryPrompt, "Fix login")

        var failed = stopped
        failed.stopRequestedAtMs = 0
        XCTAssertFalse(failed.isStopped)
        XCTAssertEqual(failed.statusLabel, "failed")
        var running = stopped
        running.status = "running"
        XCTAssertFalse(running.isStopped)
    }

    func testATaskReadsFromPromptToMerge() {
        func chain(_ json: JSONObject, pull: String = "") -> AgentTaskChain {
            AgentTaskChain(AgentLaunchSession(json: json), pullStatus: pull)
        }
        func states(_ c: AgentTaskChain) -> [String] { c.stages.map { "\($0.key):\($0.state.rawValue)" } }

        let queued = chain(["id": "s", "status": "queued", "targetNode": "vultr-1", "createPr": true])
        XCTAssertEqual(states(queued), ["prompt:done", "mirror:active", "branch:pending", "pull:pending", "merge:pending"])
        XCTAssertEqual(queued.stages[1].detail, "Waiting for vultr-1 to pick it up")
        XCTAssertEqual(queued.stages[3].detail, "Opens when the agent finishes")

        let running = chain([
            "id": "s", "status": "running", "localAgentId": 4, "createPr": true,
            "agentInfo": ["branchName": "agent/web-7-fix", "baseBranch": "main"],
        ])
        XCTAssertEqual(states(running), ["prompt:done", "mirror:done", "branch:active", "pull:pending", "merge:pending"])
        XCTAssertEqual(running.summary, "On agent/web-7-fix")
        XCTAssertEqual(running.stages[2].detail, "agent/web-7-fix from main")

        let open = chain([
            "id": "s", "status": "completed", "localAgentId": 4, "createPr": true,
            "agentInfo": ["branchName": "agent/web-7-fix", "prNumber": 12],
        ], pull: "open")
        XCTAssertEqual(states(open).last, "merge:active")
        XCTAssertEqual(open.summary, "PR #12 open")
        XCTAssertEqual(open.stages[4].detail, "Open — review and merge #12")

        // The mirror reports merged:false for ever; the ledger knows better.
        let merged = chain([
            "id": "s", "status": "completed", "localAgentId": 4, "createPr": true,
            "agentInfo": ["branchName": "agent/web-7-fix", "prNumber": 12, "merged": false],
        ], pull: "merged")
        XCTAssertTrue(states(merged).allSatisfy { $0.hasSuffix(":done") })
        XCTAssertEqual(merged.summary, "Merged")

        let failed = chain(["id": "s", "status": "failed", "targetNode": "vultr-1", "createPr": true])
        XCTAssertEqual(states(failed)[1], "mirror:failed")
        XCTAssertEqual(failed.stages[1].detail, "Never started on vultr-1")

        let closed = chain(["id": "s", "status": "completed", "createdBy": "ann", "agentInfo": ["prNumber": 3]], pull: " Closed ")
        XCTAssertEqual(closed.summary, "PR #3 closed")
        XCTAssertEqual(closed.stages[0].detail, "Sent by @ann")
        XCTAssertEqual(closed.stages[4].state, .failed)

        let noChange = chain([
            "id": "s", "status": "completed", "localAgentId": 1, "createPr": true, "branchName": "b",
        ])
        XCTAssertEqual(noChange.stages[3].detail, "None was opened — the branch did not change")
        XCTAssertEqual(noChange.stages[3].state, .pending)
        XCTAssertEqual(AgentTaskChain(AgentLaunchSession(sessionId: "s"), pullNumber: 7).prNumber, 7)
    }

    func testQueueAndRetryLabels() {
        XCTAssertEqual(AgentQueueResult(json: ["queueState": "queued", "targetOnline": true]).label,
                       "Queued. The node is picking it up now.")
        XCTAssertTrue(AgentQueueResult(json: ["queueState": "waiting_for_desktop", "targetOnline": true]).waitingForNode)
        XCTAssertEqual(AgentQueueResult().label, "Queued. It starts as soon as the node connects.")
        XCTAssertEqual(AgentQueueResult(message: "Custom").label, "Custom")
        XCTAssertEqual(AgentQueueResult(json: ["session": ["id": "new"]]).session?.sessionId, "new")
        XCTAssertEqual(AgentRetryResult(result: AgentQueueResult(), resumed: true).label,
                       "Sent back to the mirror. It picks the session up on its branch.")
        XCTAssertEqual(AgentRetryResult(result: AgentQueueResult(targetOnline: true), resumed: false).label,
                       "Queued. The node is picking it up now.")
        XCTAssertEqual(AgentLaunchException("Nope", code: "x", status: 403).description, "Nope")
    }

    func testMergePlanNeedsThreeConsistentIds() {
        let a = String(repeating: "a", count: 40)
        let b = String(repeating: "b", count: 40)
        let c = String(repeating: "c", count: 40)
        XCTAssertTrue(PullMergePlan(number: 1, expectedBaseOid: a, expectedHeadOid: b, expectedPullsOid: c).isReady)
        XCTAssertFalse(PullMergePlan(number: 1, expectedBaseOid: a, expectedHeadOid: b, expectedPullsOid: c, clean: false).isReady)
        XCTAssertFalse(PullMergePlan(number: 1, expectedBaseOid: a, expectedHeadOid: "main", expectedPullsOid: c).isConsistent)
        XCTAssertFalse(PullMergePlan(number: 1, expectedBaseOid: "", expectedHeadOid: "", expectedPullsOid: "").isConsistent)
        XCTAssertFalse(PullMergeOutcome(merged: false, pending: true).merged)
    }

    func testTheListsReadWhereTheFinishedWindowWasCut() {
        let options = AgentLaunchOptions(json: [
            "sessions": [["id": "s1", "status": "completed", "updatedAt": 1_700_000_500_000]],
            "finishedBefore": 1_700_000_500_000,
        ])
        XCTAssertEqual(options.finishedBeforeMs, 1_700_000_500_000)
        XCTAssertEqual(options.sessionList.finishedBeforeMs, 1_700_000_500_000)
        XCTAssertEqual(options.sessionList.sessions.map(\.sessionId), ["s1"])
        XCTAssertEqual(AgentLaunchOptions(json: ["sessions": []]).finishedBeforeMs, 0)

        let list = AgentSessionList(json: [
            "sessions": [["id": "s2", "status": "running", "updatedAt": 1_700_000_900_000]],
            "finishedBefore": 1_700_000_100_000,
        ])
        XCTAssertEqual(list.finishedBeforeMs, 1_700_000_100_000)
        XCTAssertEqual(list.sessions.first?.updatedAtMs, 1_700_000_900_000)
        XCTAssertEqual(AgentSessionList(json: ["sessions": []]).finishedBeforeMs, 0)
    }

    func testAMissingFinishedRunOlderThanTheWindowIsKept() {
        let cut = 1_700_000_500_000
        let listed = [AgentLaunchSession(sessionId: "new", status: "completed", updatedAtMs: cut + 10)]
        let previous = [
            AgentLaunchSession(sessionId: "new", status: "running", updatedAtMs: cut - 50),
            AgentLaunchSession(sessionId: "old", status: "completed", updatedAtMs: cut - 100),
            AgentLaunchSession(sessionId: "edge", status: "failed", updatedAtMs: cut),
            AgentLaunchSession(sessionId: "deleted", status: "completed", updatedAtMs: cut + 5),
            AgentLaunchSession(sessionId: "active", status: "running", updatedAtMs: cut - 100),
            AgentLaunchSession(sessionId: "pending", status: "security_pending", updatedAtMs: cut - 100),
            AgentLaunchSession(sessionId: "queued", status: "queued", updatedAtMs: cut - 100),
            AgentLaunchSession(sessionId: "undated", status: "completed", updatedAtMs: 0),
        ]
        let kept = AgentSessionList(sessions: listed, finishedBeforeMs: cut).keeping(previous)
        XCTAssertEqual(kept.map(\.sessionId), ["new", "old", "edge"])
        XCTAssertEqual(kept.first?.status, "completed")

        let whole = AgentSessionList(sessions: listed, finishedBeforeMs: 0).keeping(previous)
        XCTAssertEqual(whole.map(\.sessionId), ["new"])
        XCTAssertFalse(previous[1].isBeyondListedWindow(finishedBeforeMs: 0))
        XCTAssertTrue(previous[1].isBeyondListedWindow(finishedBeforeMs: cut))
    }
}
