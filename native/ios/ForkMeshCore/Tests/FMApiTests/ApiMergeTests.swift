// Merging a pull request through ApiService: the merge plan's pinned ids and the merge request
// itself.

import FMBase
import FMCrypto
import FMNet
import Foundation
import XCTest
@testable import FMApi

private let mergeUpperA = String(repeating: "A", count: 40)
private let mergeUpperB = String(repeating: "B", count: 40)
private let mergeUpperC = String(repeating: "C", count: 40)

@MainActor
final class ApiMergeTests: XCTestCase {
    private func pull(head: String = "agent/fix", status: String = "open", branchBacked: Bool = true) -> PublishedPull {
        PublishedPull(number: 12, title: "Fix the crash", status: status, base: "main", head: head, branchBacked: branchBacked)
    }

    private let plan = PullMergePlan(
        number: 12,
        expectedBaseOid: String(repeating: "a", count: 40),
        expectedHeadOid: String(repeating: "b", count: 40),
        expectedPullsOid: String(repeating: "c", count: 40))

    func testThePlanPinsTheLedgerBaseAndHeadCommits() async throws {
        let http = FakeHTTPClient { request in
            let path = portRawPath(request)
            if path == "/api/repo/alice/mobile/tree" {
                XCTAssertEqual(portQuery(request)["ref"], "forkmesh/pulls")
                return portJSON(["commit": .string(mergeUpperC), "entries": []])
            }
            if path == "/api/repo/alice/mobile/branches" {
                return portJSON(["branches": [
                    ["name": "main", "commit": .string(mergeUpperA)],
                    ["name": "agent/fix", "commit": .string(mergeUpperB)],
                ]])
            }
            if path == "/api/repo/alice/mobile/merge-preview" {
                XCTAssertEqual(portQuery(request)["base"], String(repeating: "a", count: 40))
                XCTAssertEqual(portQuery(request)["head"], "agent/fix")
                return portJSON([
                    "baseOid": .string(mergeUpperA), "headOid": .string(mergeUpperB), "clean": true, "conflicts": [],
                ])
            }
            return portJSON([:], 404)
        }
        let api = ApiPortFixture.api(http)

        let found = await api.pullMergePlan("alice", "mobile", pull())

        let plan = try XCTUnwrap(found)
        XCTAssertEqual(plan.expectedBaseOid, String(repeating: "a", count: 40))
        XCTAssertEqual(plan.expectedHeadOid, String(repeating: "b", count: 40))
        XCTAssertEqual(plan.expectedPullsOid, String(repeating: "c", count: 40))
        XCTAssertTrue(plan.isReady)
    }

    func testAConflictedBranchYieldsAPlanThatIsNotReady() async throws {
        let http = FakeHTTPClient { request in
            let path = portRawPath(request)
            if path == "/api/repo/alice/mobile/tree" {
                return portJSON(["commit": .string(mergeUpperC), "entries": []])
            }
            if path == "/api/repo/alice/mobile/branches" {
                return portJSON(["branches": [["name": "main", "commit": .string(mergeUpperA)]]])
            }
            return portJSON([
                "baseOid": .string(mergeUpperA), "headOid": .string(mergeUpperB), "clean": false,
                "conflicts": ["lib/main.dart"],
            ])
        }
        let api = ApiPortFixture.api(http)

        let found = await api.pullMergePlan("alice", "mobile", pull())

        let plan = try XCTUnwrap(found)
        XCTAssertTrue(plan.isConsistent)
        XCTAssertFalse(plan.isReady)
        XCTAssertEqual(plan.conflicts, ["lib/main.dart"])
    }

    func testAPreviewResolvedAgainstAMovedBaseIsRefused() async {
        let http = FakeHTTPClient { request in
            let path = portRawPath(request)
            if path == "/api/repo/alice/mobile/tree" {
                return portJSON(["commit": .string(mergeUpperC), "entries": []])
            }
            if path == "/api/repo/alice/mobile/branches" {
                return portJSON(["branches": [["name": "main", "commit": .string(mergeUpperA)]]])
            }
            // The mirror answered about a different base tip than the one asked about, so
            // nothing here describes the merge the person is looking at.
            return portJSON([
                "baseOid": .string(String(repeating: "D", count: 40)), "headOid": .string(mergeUpperB), "clean": true,
            ])
        }
        let api = ApiPortFixture.api(http)

        let plan = await api.pullMergePlan("alice", "mobile", pull())
        XCTAssertNil(plan)
    }

    func testAPatchBackedPullPinsTheLedgerCommitEvenWithAHeadName() async throws {
        let http = FakeHTTPClient { request in
            let path = portRawPath(request)
            if path == "/api/repo/alice/mobile/tree" {
                if portQuery(request)["path"] == ".forkmesh/pulls/12" {
                    return portJSON(["commit": .string(mergeUpperC), "entries": [
                        ["name": "pull.md", "type": "blob", "path": ".forkmesh/pulls/12/pull.md"],
                        ["name": "commits.mbox", "type": "blob", "path": ".forkmesh/pulls/12/commits.mbox"],
                    ]])
                }
                return portJSON(["commit": .string(mergeUpperC), "entries": []])
            }
            if path == "/api/repo/alice/mobile/branches" {
                return portJSON(["branches": [["name": "main", "commit": .string(mergeUpperA)]]])
            }
            XCTFail("unexpected \(path)")
            return portJSON([:], 404)
        }
        let api = ApiPortFixture.api(http)

        let found = await api.pullMergePlan("alice", "mobile", pull(head: "contributor/fix", branchBacked: false))

        let plan = try XCTUnwrap(found)
        XCTAssertFalse(plan.branchBacked)
        XCTAssertEqual(plan.expectedBaseOid, String(repeating: "a", count: 40))
        XCTAssertEqual(plan.expectedHeadOid, String(repeating: "c", count: 40))
        XCTAssertEqual(plan.expectedPullsOid, String(repeating: "c", count: 40))
        XCTAssertTrue(plan.isReady)
    }

    func testAPatchBackedPullWithoutAPayloadHasNoPlan() async {
        let http = FakeHTTPClient { request in
            let path = portRawPath(request)
            if path == "/api/repo/alice/mobile/tree" {
                return portJSON(["commit": .string(mergeUpperC), "entries": [
                    ["name": "pull.md", "type": "blob", "path": ".forkmesh/pulls/12/pull.md"],
                ]])
            }
            if path == "/api/repo/alice/mobile/branches" {
                return portJSON(["branches": [["name": "main", "commit": .string(mergeUpperA)]]])
            }
            return portJSON([:], 404)
        }
        let api = ApiPortFixture.api(http)

        let plan = await api.pullMergePlan("alice", "mobile", pull(head: "contributor/fix", branchBacked: false))
        XCTAssertNil(plan)
    }

    func testAClosedPullRequestHasNoPlan() async {
        let http = FakeHTTPClient { _ in portJSON([:]) }
        let api = ApiPortFixture.api(http)

        let plan = await api.pullMergePlan("alice", "mobile", pull(status: "merged"))
        XCTAssertNil(plan)
    }

    func testTheMergeBodyCarriesExactlyTheFieldsTheRelayAllows() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": true, "status": "merged"]) }
        let api = ApiPortFixture.api(http)

        let outcome = try await api.mergePull("alice", "mobile", plan)

        let seen = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(portRawPath(seen), "/api/repo/alice/mobile/pulls/12/merge")
        let sent = portBody(seen)
        XCTAssertEqual(Set(sent.keys), [
            "schemaVersion", "type", "pullNumber", "requestId", "expectedBaseOid", "expectedHeadOid", "expectedPullsOid",
        ])
        XCTAssertEqual(sent["type"], "forkmesh.pull-merge-v1")
        XCTAssertEqual(sent["schemaVersion"], 1)
        XCTAssertEqual(sent["pullNumber"], 12)
        XCTAssertTrue(outcome.merged)
    }

    func testASignedMergeCarriesADeployApprovalOverTheRequest() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": true, "status": "merged"]) }
        let api = ApiPortFixture.api(http)
        let identity = try Identity.fromSeed(Array(0..<32))
        api.deployApprovalIdentity = identity

        _ = try await api.mergePull("Alice", "mobile", plan)

        let sent = portBody(http.lastRequest)
        XCTAssertEqual(Set(sent.keys), [
            "schemaVersion", "type", "pullNumber", "requestId", "expectedBaseOid", "expectedHeadOid", "expectedPullsOid",
            "deployApproval",
        ])
        let approval = sent.obj("deployApproval")
        XCTAssertEqual(approval.keys, ["publicKey", "ts", "signature"])
        XCTAssertEqual(approval.str("publicKey"), "A6EHv_POEL4dcN0Y50vAmWfk1jCbpQ1fHdyGZBJVMbg")
        XCTAssertEqual(approval.int64("ts"), 1_700_000_000_000)
        let message = DeployApproval.message(
            kind: "pull", owner: "alice", repo: "mobile", target: "12", base: plan.expectedBaseOid,
            head: plan.expectedHeadOid, requestId: sent.str("requestId"), ts: 1_700_000_000_000)
        let signature = Array(try B64.decodeUnpadded(approval.str("signature")))
        XCTAssertTrue(Ed25519.verify(publicKey: identity.publicKeyBytes, message: Array(message.utf8), signature: signature))
    }

    func testThePolledRetriesResendTheSameDeployApproval() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 202, json: ["status": "processing"])
        http.enqueue(status: 200, json: ["status": "merged"])
        let api = ApiPortFixture.api(http)
        api.deployApprovalIdentity = try Identity.fromSeed(Array(0..<32))

        _ = try await api.mergePull("alice", "mobile", plan)

        let approvals = http.requests.map { portBody($0).obj("deployApproval") }
        XCTAssertEqual(approvals.count, 2)
        XCTAssertFalse(approvals[0].str("signature").isEmpty)
        XCTAssertEqual(approvals[0], approvals[1])
    }

    func testTheMergeIsAddressedToTheOrganizationNotTheNode() async throws {
        let http = FakeHTTPClient { request in
            let path = portRawPath(request)
            if path == "/api/repositories" {
                return portJSON([[
                    "owner": "mirror35", "name": "forkmesh",
                    "logicalOwners": [["kind": "organization", "owner": "forkmesh"]],
                ]])
            }
            if path == "/api/orgs/forkmesh" { return portJSON(["org": "forkmesh", "viewerRole": "owner"]) }
            return portJSON(["ok": true, "status": "merged"])
        }
        let api = ApiPortFixture.api(http)

        let repos = try await api.repositories()
        let repo = try XCTUnwrap(repos.first)
        XCTAssertEqual(repo.owner, "mirror35")
        let allowed = await api.canMergePulls(repo.owner, repo.name)
        XCTAssertTrue(allowed)
        let outcome = try await api.mergePull(repo.owner, repo.name, plan)

        XCTAssertTrue(outcome.merged)
        XCTAssertEqual(http.requests.map(portMethodPath), [
            "GET /api/repositories",
            "GET /api/orgs/forkmesh",
            "POST /api/repo/forkmesh/forkmesh/pulls/12/merge",
        ])
    }

    func testARepositoryWithNoOrganizationKeepsItsOwnNamespace() async throws {
        let http = FakeHTTPClient { _ in portJSON(["status": "merged"]) }
        let api = ApiPortFixture.api(http)

        _ = try await api.mergePull("alice", "mobile", plan)

        XCTAssertEqual(http.requests.map(portRawPath), ["/api/repo/alice/mobile/pulls/12/merge"])
    }

    func testAMergeDropsThePullListItJustMadeStale() async throws {
        let listings = PortBox(0)
        let http = FakeHTTPClient { request in
            if portRawPath(request) == "/api/repo/alice/mobile/tree" {
                listings.value += 1
                return portJSON(["commit": .string(mergeUpperC), "entries": []])
            }
            return portJSON(["status": "merged"])
        }
        let api = ApiPortFixture.api(http)

        _ = try await api.publishedPulls("alice", "mobile")
        _ = try await api.publishedPulls("alice", "mobile")
        XCTAssertEqual(listings.value, 1)

        _ = try await api.mergePull("alice", "mobile", plan)
        _ = try await api.publishedPulls("alice", "mobile")

        XCTAssertEqual(listings.value, 2)
    }

    func testWaivingReviewAddsTheOneOptionalField() async throws {
        let http = FakeHTTPClient { _ in portJSON(["status": "merged"]) }
        let api = ApiPortFixture.api(http)

        _ = try await api.mergePull("alice", "mobile", plan, overrideReview: true)

        XCTAssertEqual(portBody(http.lastRequest)["overrideReview"], true)
    }

    func testMergeAndDeleteAddsTheCleanupField() async throws {
        let http = FakeHTTPClient { _ in portJSON(["status": "merged"]) }
        let api = ApiPortFixture.api(http)

        _ = try await api.mergePull("alice", "mobile", plan, deleteHead: true)

        XCTAssertEqual(portBody(http.lastRequest)["deleteHead"], true)
    }

    func testA202IsPolledWithOneStableRequestIdUntilItLands() async throws {
        let http = FakeHTTPClient()
        http.enqueue(status: 202, json: ["status": "processing"])
        http.enqueue(status: 202, json: ["status": "processing"])
        http.enqueue(status: 200, json: ["status": "merged"])
        let api = ApiPortFixture.api(http)

        let outcome = try await api.mergePull("alice", "mobile", plan)

        XCTAssertEqual(http.requests.count, 3)
        // Re-sending one id is what makes the retry idempotent rather than a second merge.
        let ids = Set(http.requests.map { portBody($0).str("requestId") })
        XCTAssertEqual(ids.count, 1)
        XCTAssertTrue(outcome.merged)
    }

    func testARefusalExplainsItselfInWords() async {
        let http = FakeHTTPClient { _ in portJSON(["error": "review_required"], 409) }
        let api = ApiPortFixture.api(http)

        let error = await portCaughtError { try await api.mergePull("alice", "mobile", self.plan) }
        let refusal = error as? AgentLaunchException
        XCTAssertNotNil(refusal, "\(String(describing: error))")
        XCTAssertEqual(refusal?.code, "review_required")
        XCTAssertTrue(refusal?.message.contains("independent review") == true, refusal?.message ?? "")
    }

    func testAnUnpinnablePlanNeverReachesTheNetwork() async {
        let http = FakeHTTPClient { _ in portJSON([:]) }
        let api = ApiPortFixture.api(http)

        let error = await portCaughtError {
            try await api.mergePull("alice", "mobile", PullMergePlan(
                number: 12,
                expectedBaseOid: String(repeating: "a", count: 40),
                expectedHeadOid: "",
                expectedPullsOid: String(repeating: "c", count: 40)))
        }
        XCTAssertTrue(error is AgentLaunchException, "\(String(describing: error))")
        XCTAssertEqual(http.requests.count, 0)
    }
}
