import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class RunnerAgentOpsTests: XCTestCase {
    private let sessionJSON: JSONObject = [
        "id": "s1", "org": "alice", "repo": "site", "status": "completed", "targetNode": "fmnext-n1",
        "runner": "fmnext-r1", "branch": "agent/12-fix-login", "baseBranch": "main", "localAgentId": 7,
        "diffStats": ["files": 3, "added": 40, "removed": 2, "ahead": 2, "behind": 1, "conflicted": false, "known": true],
        "merge": [
            "state": "merged", "base": "main", "mergeOid": "abc", "mergedAt": 1_700_000_000_000,
            "branchDeleted": false, "cleanupPending": ["fmnext-n2"], "conflicts": [], "message": "",
        ],
        "canMerge": false,
        "agentInfo": ["branchName": "agent/old", "baseBranch": "dev"],
    ]

    func testTheSessionCarriesItsBranchRunnerStatsAndMerge() async throws {
        let session = AgentLaunchSession(json: sessionJSON)
        XCTAssertEqual(session.branchName, "agent/12-fix-login")
        XCTAssertEqual(session.baseBranch, "main")
        XCTAssertEqual(session.runner, "fmnext-r1")
        XCTAssertTrue(session.onRunner)
        XCTAssertEqual(session.branchStats, AgentBranchStats(files: 3, added: 40, removed: 2, ahead: 2, behind: 1, known: true))
        XCTAssertTrue(session.branchMerge.isMerged)
        XCTAssertTrue(session.branchMerge.needsCleanup)
        XCTAssertEqual(session.branchMerge.cleanupPending, ["fmnext-n2"])
        XCTAssertTrue(session.merged)
        XCTAssertEqual(session.mergedAtMs, 1_700_000_000_000)
        XCTAssertFalse(session.canMerge)

        let older = AgentLaunchSession(json: ["id": "s2", "agentInfo": ["branchName": "agent/s2", "baseRef": "main"]])
        XCTAssertEqual(older.branchName, "agent/s2")
        XCTAssertEqual(older.baseBranch, "main")
        XCTAssertFalse(older.branchStats.known)
        XCTAssertEqual(older.branchStats.files, -1)
        XCTAssertEqual(older.runner, "")
    }

    func testARunnerPoolTargetIsACloudRunnerNotADesktop() async throws {
        let runner = AgentLaunchTarget(json: ["node": "fmnext-r1", "online": true, "runtimeMode": "runner-pool"])
        XCTAssertTrue(runner.isRunner)
        XCTAssertTrue(runner.isMirror)
        XCTAssertEqual(runner.kindLabel, "Runner")
        XCTAssertEqual(AgentLaunchTarget(json: ["node": "m", "runtimeMode": "headless"]).kindLabel, "Mirror")
        XCTAssertEqual(RunnerAgentText.badge("fmnext-r1"), "Runner \u{00B7} fmnext-r1")
        XCTAssertEqual(RunnerAgentText.badge(""), "Node")
    }

    func testTheDiffIsReadOnePageAtATime() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true, "id": "s1", "branch": "agent/12-fix-login", "base": "main", "commits": 2, "ahead": 2,
                "behind": 1, "clean": false, "conflicts": ["a.txt"], "merged": false,
                "files": [["path": "a.txt", "added": 4, "removed": 1], ["path": "b.txt", "added": 1, "removed": 0]],
                "added": 5, "removed": 1, "page": 1, "pageSize": 1, "pageCount": 3,
                "committed": "diff --git a/b.txt b/b.txt\n", "uncommitted": "", "untracked": [], "truncated": true,
            ])
        }
        let api = ApiPortFixture.api(http)
        let page = try await api.runnerAgentDiff("alice", "site", "s1", page: 1, pageSize: 1)
        let seen = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(portMethodPath(seen), "GET /api/repo/alice/site/agent-sessions/s1/diff")
        XCTAssertEqual(portQuery(seen), ["page": "1", "pageSize": "1"])
        XCTAssertEqual(page.page, 1)
        XCTAssertTrue(page.hasMore)
        XCTAssertEqual(page.clean, false)
        XCTAssertEqual(page.conflicts, ["a.txt"])
        XCTAssertEqual(page.stats, AgentBranchStats(files: 2, added: 5, removed: 1, ahead: 2, behind: 1, conflicted: true, known: true))

        _ = try await api.runnerAgentDiff("acme", "site", "s1", page: 0, pageSize: 500, orgScope: true)
        XCTAssertEqual(portQuery(http.requests[1])["pageSize"], "100")
        XCTAssertTrue(portRawPath(http.requests[1]).hasSuffix("/repos/site/agent-bots/s1/diff"))
    }

    func testADiffBeforeTheBranchExistsSaysSo() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": false, "error": "no_branch"], 409) }
        let api = ApiPortFixture.api(http)
        let error = await portCaughtError { try await api.runnerAgentDiff("alice", "site", "s1") }
        let refusal = try XCTUnwrap(error as? AgentLaunchException)
        XCTAssertEqual(refusal.code, "no_branch")
        XCTAssertEqual(refusal.message, "This run has not pushed a branch yet.")
    }

    func testMergeWaitsOutProcessingWithTheSameRequestId() async throws {
        let calls = PortBox(0)
        let http = FakeHTTPClient { _ in
            calls.value += 1
            if calls.value < 3 { return portJSON(["ok": true, "status": "processing", "requestId": "req_abcdefghijkl"], 202) }
            return portJSON([
                "ok": true, "status": "merged", "requestId": "req_abcdefghijkl", "branch": "agent/12-fix-login",
                "base": "main", "mergeOid": "m1", "branchDeleted": true, "cleanupPending": [],
                "stats": ["base": "main", "commit": "m1", "commits": 2, "files": 3, "insertions": 40, "deletions": 2],
                "message": "Merged agent/12-fix-login into main and cleaned up.",
            ])
        }
        let api = ApiPortFixture.api(http)
        let result = try await api.mergeRunnerAgent("alice", "site", "s1")
        XCTAssertTrue(result.isMerged)
        XCTAssertTrue(result.branchDeleted)
        XCTAssertEqual(result.raw.obj("stats").int("insertions"), 40)
        XCTAssertEqual(http.requests.count, 3)
        let bodies = http.requests.map(portBody)
        XCTAssertEqual(bodies[0]["action"], "merge")
        XCTAssertEqual(bodies[0]["cleanup"], true)
        XCTAssertEqual(bodies[1]["requestId"], "req_abcdefghijkl")
        XCTAssertEqual(bodies[2]["requestId"], "req_abcdefghijkl")
        XCTAssertEqual(http.requests[0].headers["Origin"], "http://relay.test")
        XCTAssertEqual(portMethodPath(http.requests[0]), "POST /api/repo/alice/site/agent-sessions/s1")
    }

    func testAMergeSignsADeployApprovalForEachRequestId() async throws {
        let calls = PortBox(0)
        let http = FakeHTTPClient { _ in
            calls.value += 1
            if calls.value < 3 { return portJSON(["ok": true, "status": "processing", "requestId": "req_abcdefghijkl"], 202) }
            return portJSON(["ok": true, "status": "merged", "requestId": "req_abcdefghijkl"])
        }
        let api = ApiPortFixture.api(http)
        let identity = try Identity.fromSeed(Array(0..<32))
        api.deployApprovalIdentity = identity

        _ = try await api.mergeRunnerAgent("Alice", "site", "s1")

        let bodies = http.requests.map(portBody)
        XCTAssertEqual(bodies.count, 3)
        for body in bodies {
            let approval = body.obj("deployApproval")
            XCTAssertEqual(approval.str("publicKey"), "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg")
            XCTAssertEqual(approval.int64("ts"), 1_700_000_000_000)
            let message = DeployApproval.message(
                kind: "branch", owner: "alice", repo: "site", target: "s1", base: "", head: "",
                requestId: body.str("requestId"), ts: 1_700_000_000_000)
            let signature = Array(try B64.decodeUnpadded(approval.str("signature")))
            XCTAssertTrue(Ed25519.verify(publicKey: identity.publicKeyBytes, message: Array(message.utf8), signature: signature))
        }
        XCTAssertEqual(bodies[1].obj("deployApproval"), bodies[2].obj("deployApproval"))
        XCTAssertNotEqual(bodies[0].obj("deployApproval"), bodies[1].obj("deployApproval"))
    }

    func testOnlyAMergeCarriesADeployApproval() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": true, "status": "cleaned", "branchDeleted": true, "cleanupPending": []]) }
        let api = ApiPortFixture.api(http)
        api.deployApprovalIdentity = try Identity.fromSeed(Array(0..<32))

        _ = try await api.cleanupRunnerAgent("alice", "site", "s1")

        XCTAssertNil(portBody(http.lastRequest)["deployApproval"])
    }

    func testAMergeWithoutAnIdentityIsStillSent() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": true, "status": "merged"]) }
        let api = ApiPortFixture.api(http)

        _ = try await api.mergeRunnerAgent("alice", "site", "s1")

        let body = portBody(http.lastRequest)
        XCTAssertEqual(body["action"], "merge")
        XCTAssertFalse(body.str("requestId").isEmpty)
        XCTAssertNil(body["deployApproval"])
    }

    func testAConflictNamesTheFilesAndChangesNothing() async throws {
        let http = FakeHTTPClient { _ in
            portJSON(["ok": false, "error": "merge_conflict", "conflicts": ["a.txt", "b.txt"]], 409)
        }
        let api = ApiPortFixture.api(http)
        let error = await portCaughtError { try await api.mergeRunnerAgent("alice", "site", "s1") }
        let refusal = try XCTUnwrap(error as? AgentLaunchException)
        XCTAssertEqual(refusal.code, "merge_conflict")
        XCTAssertEqual(
            refusal.message, "The branch conflicts with its base, so nothing was merged. Conflicts: a.txt, b.txt.")
        XCTAssertEqual(http.requests.count, 1)
    }

    func testAStaleRefIsRetriedOnceWithAFreshRequestId() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": false, "error": "stale_head"], 409) }
        let api = ApiPortFixture.api(http)
        let error = await portCaughtError { try await api.mergeRunnerAgent("alice", "site", "s1") }
        XCTAssertEqual((error as? AgentLaunchException)?.code, "stale_head")
        XCTAssertEqual(http.requests.count, 2)
        let ids = http.requests.map { portBody($0).str("requestId") }
        XCTAssertNotEqual(ids[0], ids[1])
    }

    func testCleanupOfAnUnmergedBranchIsRefused() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": false, "error": "not_merged"], 409) }
        let api = ApiPortFixture.api(http)
        let error = await portCaughtError { try await api.cleanupRunnerAgent("alice", "site", "s1") }
        XCTAssertEqual(
            (error as? AgentLaunchException)?.message, "This branch is not merged yet, so it was kept. Merge it first.")
        let body = portBody(http.requests.first)
        XCTAssertEqual(body["action"], "cleanup")
        XCTAssertNil(body["cleanup"])
    }

    func testCleanupReportsTheCoresStillHoldingTheBranch() async throws {
        let http = FakeHTTPClient { _ in
            portJSON(["ok": true, "status": "cleaned", "branchDeleted": true, "cleanupPending": ["fmnext-n3"]])
        }
        let api = ApiPortFixture.api(http)
        let result = try await api.cleanupRunnerAgent("alice", "site", "s1")
        XCTAssertTrue(result.isCleaned)
        XCTAssertEqual(result.cleanupPending, ["fmnext-n3"])
    }

    func testDiscardAsksTheRelayToDeleteAnUnmergedBranch() async throws {
        let http = FakeHTTPClient { _ in
            portJSON(["ok": true, "status": "cleaned", "branchDeleted": true, "cleanupPending": []])
        }
        let api = ApiPortFixture.api(http)
        let result = try await api.discardRunnerAgentBranch("alice", "site", "s1")
        XCTAssertTrue(result.isCleaned)
        let body = portBody(http.requests.first)
        XCTAssertEqual(body["action"], "cleanup")
        XCTAssertEqual(body["discard"], true)
        XCTAssertNil(portBody(http.requests.first)["cleanup"])
    }

    func testContinueSendsTheContinuePrompt() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": true, "status": "queued"], 202) }
        let api = ApiPortFixture.api(http)
        _ = try await api.continueRunnerAgent("alice", "site", "s1")
        XCTAssertEqual(portBody(http.requests.first)["prompt"], "Continue where you left off.")
    }

    func testStartingSendsTheBaseBranchAndABranchSlug() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": true, "queueState": "ready_for_claim"], 202) }
        let api = ApiPortFixture.api(http)
        _ = try await api.launchAgentSession(
            "alice", "site", prompt: "Fix the login page", targetNode: "fmnext-r1", title: "Fix the login page!",
            baseBranch: "develop", branchName: "Fix the login page!")
        let body = portBody(http.requests.first)
        XCTAssertEqual(body["baseBranch"], "develop")
        XCTAssertEqual(body["branchName"], "fix-the-login-page")
        XCTAssertEqual(body["targetNode"], "fmnext-r1")

        _ = try await api.launchAgentSession("alice", "site", prompt: "x", baseBranch: " ", branchName: "!!")
        let bare = portBody(http.requests[1])
        XCTAssertNil(bare["baseBranch"])
        XCTAssertNil(bare["branchName"])
    }

    func testTheSessionCarriesItsPullAndWhetherOneCanBeOpened() async throws {
        var json = sessionJSON
        json["canOpenPull"] = true
        let open = AgentLaunchSession(json: json)
        XCTAssertTrue(open.canOpenPull)
        XCTAssertEqual(open.prNumber, 0)
        XCTAssertFalse(AgentLaunchSession(json: sessionJSON).canOpenPull)

        json["canOpenPull"] = false
        json["prNumber"] = 14
        json["agentInfo"] = ["prNumber": 3]
        let linked = AgentLaunchSession(json: json)
        XCTAssertFalse(linked.canOpenPull)
        XCTAssertEqual(linked.prNumber, 14)
        XCTAssertEqual(AgentLaunchSession(json: ["id": "s3", "agentInfo": ["prNumber": 3]]).prNumber, 3)
    }

    func testLinkingAPullPostsTheNumberToTheSession() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true, "status": "linked", "branch": "agent/12-fix-login", "prNumber": 31,
                "message": "Opened pull request #31 for agent/12-fix-login.",
            ])
        }
        let api = ApiPortFixture.api(http)
        let link = try await api.linkRunnerAgentPull("alice", "site", "s1", number: 31)
        let seen = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(portMethodPath(seen), "POST /api/repo/alice/site/agent-sessions/s1")
        XCTAssertEqual(seen.headers["Origin"], "http://relay.test")
        let body = portBody(seen)
        XCTAssertEqual(body["action"], "link-pull")
        XCTAssertEqual(body["number"], 31)
        XCTAssertEqual(body.keys.sorted(), ["action", "number"])
        XCTAssertEqual(link.status, "linked")
        XCTAssertEqual(link.prNumber, 31)
        XCTAssertEqual(link.branch, "agent/12-fix-login")
        XCTAssertEqual(link.message, "Opened pull request #31 for agent/12-fix-login.")

        _ = try await api.linkRunnerAgentPull("acme", "site", "s1", number: 2, orgScope: true)
        XCTAssertTrue(portRawPath(http.requests[1]).hasSuffix("/repos/site/agent-bots/s1"))
    }

    func testALinkRefusalSaysWhy() async throws {
        let http = FakeHTTPClient { _ in
            portJSON(["ok": false, "error": "pull_exists", "message": "Pull request #9 is already linked to this run."], 409)
        }
        let api = ApiPortFixture.api(http)
        let error = await portCaughtError { try await api.linkRunnerAgentPull("alice", "site", "s1", number: 31) }
        let refusal = try XCTUnwrap(error as? AgentLaunchException)
        XCTAssertEqual(refusal.code, "pull_exists")
        XCTAssertEqual(refusal.status, 409)
        XCTAssertEqual(refusal.message, "Pull request #9 is already linked to this run.")
        XCTAssertEqual(http.requests.count, 1)

        XCTAssertEqual(RunnerAgentText.linkMessage(["error": "invalid_pull"], 400), "That pull request number is not valid.")
        XCTAssertEqual(RunnerAgentText.linkMessage(["error": "no_branch"], 409), "This run has not pushed a branch yet.")
        XCTAssertEqual(
            RunnerAgentText.linkMessage(["error": "forbidden"], 403),
            "Only an owner of this repository can link a pull request to this run.")
        XCTAssertEqual(RunnerAgentText.linkMessage([:], 500), "The request was refused (HTTP 500).")
    }

    func testThePullDescriptionNamesTheRunnerAndCarriesTheLastAnswer() async throws {
        XCTAssertEqual(
            RunnerAgentText.pullDescription(
                runner: "fmnext-r1", targetNode: "fmnext-n1", branch: "agent/12-fix-login", summary: "  Fixed it.  "),
            "Opened from the agent run on the fmnext-r1 runner (agent/12-fix-login).\n\nFixed it.")
        XCTAssertEqual(
            RunnerAgentText.pullDescription(runner: "", targetNode: "mirror35", branch: "agent/x", summary: ""),
            "Opened from the agent run on mirror35 (agent/x).")
        let long = String(repeating: "y", count: 5000)
        let described = RunnerAgentText.pullDescription(runner: "r", targetNode: "", branch: "agent/x", summary: long)
        XCTAssertTrue(described.hasSuffix("\n\n" + String(repeating: "y", count: 4000)))
        XCTAssertEqual(
            RunnerAgentText.lastAssistantText([
                AgentLaunchMessage(role: "assistant", text: "first"), AgentLaunchMessage(role: "user", text: "more"),
                AgentLaunchMessage(role: "assistant", text: " second "), AgentLaunchMessage(role: "assistant", text: " "),
            ]),
            "second")

        var json = sessionJSON
        json["title"] = "Fix the login page"
        json["history"] = [["role": "user", "text": "Fix it"], ["role": "assistant", "text": "Done."]]
        let draft = RunnerPullDraft(session: AgentLaunchSession(json: json), branch: "", base: "main")
        XCTAssertEqual(draft.title, "Fix the login page")
        XCTAssertEqual(draft.head, "agent/12-fix-login")
        XCTAssertEqual(draft.base, "main")
        XCTAssertEqual(draft.description, "Opened from the agent run on the fmnext-r1 runner (agent/12-fix-login).\n\nDone.")
        let untitled = RunnerPullDraft(session: AgentLaunchSession(json: sessionJSON), branch: "", base: "main")
        XCTAssertEqual(untitled.title, "agent/12-fix-login")
    }

    func testPullFailuresAreWorded() async throws {
        XCTAssertEqual(
            RunnerAgentText.pullNoChanges(branch: "agent/x", base: "main"),
            "agent/x has no changes against main, so there is nothing to open a pull request for.")
        XCTAssertEqual(
            RunnerAgentText.pullBranchMissing("agent/x"), "agent/x is not on ForkMesh yet, so no pull request was opened.")
        XCTAssertEqual(
            RunnerAgentText.pullLinkFailed(31, "This run already has a pull request."),
            "Opened pull request #31, but the run could not be linked to it: This run already has a pull request.")
    }

    func testBranchSlugsAreShortAndSafe() async throws {
        XCTAssertEqual(RunnerAgentText.branchSlug("Add a dark mode toggle to settings, please"), "add-a-dark-mode-toggle")
        XCTAssertEqual(RunnerAgentText.branchSlug("Ünïcode ✨ only"), "n-code-only")
        XCTAssertEqual(RunnerAgentText.branchSlug(""), "")
    }
}
