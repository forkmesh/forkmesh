import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class BranchOpsTests: XCTestCase {
    private let headOid = String(repeating: "b", count: 40)
    private let baseOid = String(repeating: "a", count: 40)
    private var feature: RepoBranch { RepoBranch(name: "feature/login", sha: headOid.uppercased()) }
    private var main: RepoBranch { RepoBranch(name: "main", sha: baseOid, isDefault: true) }

    func testRequestBodiesCarryThePinsAndResumeFields() {
        let merge = BranchOpRequest.merge(feature, into: main, deleteBranch: true, defaultBranch: "main")
        XCTAssertEqual(merge.body, [
            "op": "merge", "branch": "feature/login", "expectedOid": .string(headOid), "baseOid": .string(baseOid),
            "deleteBranch": true, "base": "main", "defaultBranch": "main",
        ])
        let update = BranchOpRequest.update(feature, from: "main", defaultBranch: "main")
        XCTAssertEqual(update.body, [
            "op": "update", "branch": "feature/login", "expectedOid": .string(headOid), "base": "main",
            "defaultBranch": "main",
        ])
        let delete = BranchOpRequest.delete(feature, defaultBranch: "main")
        XCTAssertEqual(delete.body.keys.sorted(), ["branch", "defaultBranch", "expectedOid", "op"])

        let answer = BranchOpAnswer(json: [
            "ok": true, "executed": true, "status": "processing", "requestId": "core_abcdefghijkl", "node": "fmnext-n2",
            "pinBase": "feature/login", "pinOid": .string(baseOid),
        ], httpStatus: 202)
        XCTAssertTrue(answer.isProcessing)
        let resumed = update.resumed(answer)
        XCTAssertEqual(resumed.body["requestId"], "core_abcdefghijkl")
        XCTAssertEqual(resumed.body["node"], "fmnext-n2")
        XCTAssertEqual(resumed.body["pinBase"], "feature/login")
        XCTAssertEqual(resumed.body["pinOid"], .string(baseOid))
        XCTAssertEqual(resumed.body["op"], "update")

        var noPin = answer
        noPin.pinOid = ""
        XCTAssertNil(merge.resumed(noPin).body["pinOid"])
        XCTAssertEqual(merge.resumed(noPin).body["pinBase"], "feature/login")
        XCTAssertFalse(BranchOpRequest.delete(RepoBranch(name: "x", sha: "abc"), defaultBranch: "main").isPinned)
        XCTAssertFalse(BranchOpRequest.merge(feature, into: RepoBranch(name: "main"), deleteBranch: false, defaultBranch: "main").isPinned)
    }

    func testAnswersParseExecutedQueuedFailedAndConflicts() {
        let merged = BranchOpAnswer(json: [
            "ok": true, "executed": true, "status": "merged", "branch": "feature/login", "base": "main",
            "mergeOid": .string(baseOid), "branchDeleted": true, "cleanupPending": ["fmnext-n3"],
            "message": "Merged feature/login into main.",
        ], httpStatus: 200)
        XCTAssertTrue(merged.isExecuted)
        XCTAssertFalse(merged.isQueued)
        XCTAssertEqual(merged.cleanupPending, ["fmnext-n3"])
        XCTAssertTrue(merged.branchDeleted)

        let queued = BranchOpAnswer(json: ["ok": true, "queued": true, "ops": []], httpStatus: 200)
        XCTAssertTrue(queued.isQueued)
        XCTAssertFalse(queued.isExecuted)

        let conflict = BranchOpAnswer(json: [
            "ok": false, "executed": true, "status": "failed", "error": "merge_conflict",
            "message": "feature/login conflicts with main, so nothing changed.", "conflicts": ["a.txt", "b.txt"],
        ], httpStatus: 409)
        XCTAssertTrue(conflict.isConflict)
        XCTAssertFalse(conflict.isQueued)
        XCTAssertEqual(
            BranchOpText.failure(conflict),
            "feature/login conflicts with main, so nothing changed. Conflicts: a.txt, b.txt.")
        XCTAssertEqual(
            BranchOpText.failure(BranchOpAnswer(json: ["error": "forbidden"], httpStatus: 403)),
            "Only the repository owner can manage branches.")
        XCTAssertEqual(
            BranchOpText.failure(BranchOpAnswer(json: ["error": "stale_head"], httpStatus: 409)),
            "A branch moved since you looked. Nothing changed; refresh and try again.")
        XCTAssertTrue(BranchOpAnswer(json: ["error": "merge_status_pending"], httpStatus: 503).isRetryable)
        XCTAssertFalse(BranchOpAnswer(json: ["error": "merge_conflict"], httpStatus: 409).isRetryable)
    }

    func testAMergeWaitsOutTheCoreWithTheSameIds() async throws {
        let calls = PortBox(0)
        let mainOid = baseOid
        let http = FakeHTTPClient { _ in
            calls.value += 1
            if calls.value == 1 {
                return portJSON([
                    "ok": true, "executed": true, "status": "processing", "requestId": "core_abcdefghijkl",
                    "node": "fmnext-n1", "pinBase": "main", "message": "Merging feature/login into main…",
                ], 202)
            }
            if calls.value == 2 {
                return portJSON(["ok": false, "executed": true, "error": "merge_status_pending"], 503)
            }
            return portJSON([
                "ok": true, "executed": true, "status": "merged", "branch": "feature/login", "base": "main",
                "mergeOid": .string(mainOid), "branchDeleted": false, "cleanupPending": [],
                "message": "Merged feature/login into main.",
            ])
        }
        let api = ApiPortFixture.api(http)
        var progress: [String] = []
        let outcome = try await api.branchOp(
            "alice", "site", .merge(feature, into: main, deleteBranch: false, defaultBranch: "main")
        ) { progress.append($0) }
        XCTAssertEqual(outcome.kind, .done)
        XCTAssertEqual(outcome.message, "Merged feature/login into main.")
        XCTAssertEqual(outcome.mergeOid, baseOid)
        XCTAssertTrue(outcome.landedOnDefault)
        XCTAssertEqual(progress.first, "Merging feature/login into main…")
        XCTAssertEqual(http.requests.count, 3)
        XCTAssertEqual(portMethodPath(http.requests[0]), "POST /api/repo/alice/site/branches/ops")
        XCTAssertEqual(http.requests[0].headers["Origin"], "http://relay.test")
        let bodies = http.requests.map(portBody)
        XCTAssertNil(bodies[0]["requestId"])
        for body in bodies.dropFirst() {
            XCTAssertEqual(body["requestId"], "core_abcdefghijkl")
            XCTAssertEqual(body["node"], "fmnext-n1")
            XCTAssertEqual(body["pinBase"], "main")
            XCTAssertNil(body["pinOid"])
            XCTAssertEqual(body["expectedOid"], .string(headOid))
            XCTAssertEqual(body["baseOid"], .string(baseOid))
        }
    }

    func testAQueuedAnswerSaysTheDesktopRunsIt() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": true, "queued": true, "ops": [["op": "delete"]]]) }
        let api = ApiPortFixture.api(http)
        let outcome = try await api.branchOp("alice", "site", .delete(feature, defaultBranch: "main"))
        XCTAssertEqual(outcome.kind, .queued)
        XCTAssertEqual(outcome.message, "Deleting feature/login is queued. It runs when the owner's desktop is online.")
        XCTAssertFalse(outcome.landedOnDefault)
    }

    func testRefusalsAndConflictsAreFailures() async throws {
        let http = FakeHTTPClient { request in
            if portBody(request)["op"] == "update" {
                return portJSON([
                    "ok": false, "executed": true, "status": "failed", "error": "merge_conflict",
                    "conflicts": ["src/app.swift"],
                ], 409)
            }
            return portJSON(["error": "forbidden"], 403)
        }
        let api = ApiPortFixture.api(http)
        let conflict = try await api.branchOp("alice", "site", .update(feature, from: "main", defaultBranch: "main"))
        XCTAssertEqual(conflict.kind, .failed)
        XCTAssertTrue(conflict.isConflict)
        XCTAssertEqual(conflict.conflicts, ["src/app.swift"])
        XCTAssertEqual(conflict.message, "The branches conflict, so nothing changed. Conflicts: src/app.swift.")
        let refused = try await api.branchOp("alice", "site", .delete(feature, defaultBranch: "main"))
        XCTAssertEqual(refused.kind, .failed)
        XCTAssertEqual(refused.message, BranchOpText.ownerOnly)
        XCTAssertEqual(http.requests.count, 2)
    }

    func testThePollCapLeavesTheIdsForTryAgain() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true, "executed": true, "status": "processing", "requestId": "core_abcdefghijkl",
                "node": "fmnext-n2", "pinBase": "feature/login", "pinOid": .string(String(repeating: "c", count: 40)),
            ], 202)
        }
        let api = ApiPortFixture.api(http)
        let outcome = try await api.branchOp(
            "alice", "site", .update(feature, from: "main", defaultBranch: "main"), maxPolls: 3)
        XCTAssertEqual(outcome.kind, .pending)
        XCTAssertEqual(http.requests.count, 3)
        XCTAssertEqual(outcome.message, "The core is still working on feature/login. Try again to check on the same request.")
        XCTAssertEqual(outcome.request.requestId, "core_abcdefghijkl")
        XCTAssertEqual(outcome.request.pinOid, String(repeating: "c", count: 40))
        _ = try await api.branchOp("alice", "site", outcome.request, maxPolls: 1)
        XCTAssertEqual(portBody(http.requests[3])["requestId"], "core_abcdefghijkl")
    }

    func testAnUnpinnedBranchSendsNothing() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": true]) }
        let api = ApiPortFixture.api(http)
        let outcome = try await api.branchOp(
            "alice", "site", .delete(RepoBranch(name: "feature/login"), defaultBranch: "main"))
        XCTAssertEqual(outcome.kind, .failed)
        XCTAssertEqual(outcome.message, BranchOpText.notPinned)
        XCTAssertTrue(http.requests.isEmpty)
    }

    func testRunnerUpdatePollsWithTheSameRequestId() async throws {
        let calls = PortBox(0)
        let http = FakeHTTPClient { _ in
            calls.value += 1
            if calls.value == 1 { return portJSON(["status": "processing", "requestId": "req_abcdefghijkl"], 202) }
            if calls.value == 2 { return portJSON(["ok": false, "error": "merge_status_pending"], 503) }
            return portJSON([
                "ok": true, "status": "updated", "branch": "agent/12-fix-login", "base": "main", "head": "h1",
                "message": "Updated agent/12-fix-login from main.",
            ])
        }
        let api = ApiPortFixture.api(http)
        let result = try await api.updateRunnerAgent("alice", "site", "s1")
        XCTAssertTrue(result.isUpdated)
        XCTAssertEqual(result.message, "Updated agent/12-fix-login from main.")
        XCTAssertEqual(http.requests.count, 3)
        let bodies = http.requests.map(portBody)
        XCTAssertEqual(bodies[0].keys.sorted(), ["action", "requestId"])
        XCTAssertEqual(bodies[0]["action"], "update")
        XCTAssertEqual(bodies[1]["requestId"], "req_abcdefghijkl")
        XCTAssertEqual(bodies[2]["requestId"], "req_abcdefghijkl")
        XCTAssertEqual(portMethodPath(http.requests[0]), "POST /api/repo/alice/site/agent-sessions/s1")

        let already = FakeHTTPClient { _ in
            portJSON(["ok": true, "status": "already", "message": "agent/x is already up to date with main."])
        }
        let answer = try await ApiPortFixture.api(already).updateRunnerAgent("acme", "site", "s1", orgScope: true)
        XCTAssertTrue(answer.isUpdated)
        XCTAssertTrue(portRawPath(already.requests[0]).hasSuffix("/repos/site/agent-bots/s1"))
    }

    func testARunnerUpdateConflictNamesTheFiles() async throws {
        let http = FakeHTTPClient { _ in
            portJSON(["ok": false, "error": "merge_conflict", "conflicts": ["a.txt"], "message": "agent/x conflicts with main."], 409)
        }
        let error = await portCaughtError { try await ApiPortFixture.api(http).updateRunnerAgent("alice", "site", "s1") }
        let refusal = try XCTUnwrap(error as? AgentLaunchException)
        XCTAssertEqual(refusal.code, "merge_conflict")
        XCTAssertEqual(refusal.message, "agent/x conflicts with main. Conflicts: a.txt.")
    }

    func testTheLinkedTaskIdAndCompletionNote() async throws {
        let id = "0123456789abcdef0123456789ABCDEF"
        XCTAssertEqual(RunnerAgentText.taskId(AgentLaunchSession(sessionId: "s", taskKey: "task:\(id)")), id.lowercased())
        XCTAssertEqual(
            RunnerAgentText.taskId(AgentLaunchSession(sessionId: "s", title: "[task:\(id)] Fix login")), id.lowercased())
        XCTAssertEqual(
            RunnerAgentText.taskId(AgentLaunchSession(
                sessionId: "s", title: "Fix login", history: [AgentLaunchMessage(role: "user", text: " [task:\(id)] Fix it")],
                taskKey: "issue:4")),
            id.lowercased())
        XCTAssertEqual(RunnerAgentText.taskId(AgentLaunchSession(sessionId: "s", title: "Fix [task:\(id)]")), "")
        XCTAssertEqual(RunnerAgentText.taskId(AgentLaunchSession(sessionId: "s", taskKey: "task:short")), "")
        XCTAssertEqual(
            RunnerAgentText.completionNote(
                branch: "agent/12-fix-login", base: "main", mergeOid: "abcdef0123456789", message: "Merged and cleaned up."),
            "Merged agent/12-fix-login into main (abcdef0). Merged and cleaned up.")
        XCTAssertEqual(
            RunnerAgentText.completionNote(branch: "agent/x", base: "main", mergeOid: "", message: ""),
            "Merged agent/x into main.")

        let http = FakeHTTPClient { _ in portJSON(["ok": true, "task": ["id": .string(id), "status": "done"]]) }
        try await ApiPortFixture.api(http).completeTask(id, note: "Merged agent/x into main.")
        XCTAssertEqual(portMethodPath(http.requests[0]), "POST /api/tasks/\(id)/complete")
        XCTAssertEqual(portBody(http.requests[0]), ["completionNote": "Merged agent/x into main."])
    }
}
