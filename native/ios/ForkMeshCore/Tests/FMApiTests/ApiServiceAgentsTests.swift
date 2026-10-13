// The agent, action, desktop-command and bounty parts of native/android
// core/src/test/.../api/ApiServiceTest.kt.

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class ApiServiceAgentsTests: XCTestCase {
    // MARK: Agent sessions

    func testAgentSessionListPostsOwnerAccountAndParsesAgents() async throws {
        let http = FakeHTTPClient { request in
            XCTAssertEqual(request.method, "POST")
            XCTAssertEqual(request.apiPath, "/api/repo/owner/repo/agents/list")
            return apiJSON([
                "ok": true,
                "agents": [[
                    "id": 42,
                    "status": "done",
                    "provider": "claude-code",
                    "prNumber": 7,
                    "diffStats": ["files": 3, "ahead": 2, "behind": 1],
                ]],
            ])
        }
        let sessions = try await makeTestApi(http).agentSessions("owner", "repo", ownerAccount: " Owner ")

        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(http.lastRequest?.bodyText, #"{"ownerAccount":"owner"}"#)
        XCTAssertEqual(sessions.map(\.id), [42])
        XCTAssertEqual(sessions.first?.providerLabel, "Claude Code")
        XCTAssertEqual(sessions.first?.prNumber, 7)
        XCTAssertEqual(sessions.first?.diffLabel, "3 files · ↑2 ↓1")
    }

    func testAgentTranscriptPostsOwnerAccountAndParsesTranscriptDetail() async throws {
        let http = FakeHTTPClient { request in
            XCTAssertEqual(request.method, "POST")
            XCTAssertEqual(request.apiPath, "/api/repo/owner/repo/agents/42/transcript")
            return apiJSON(["ok": true, "status": "done", "transcript": "Running tests\nflutter test passed"])
        }
        let detail = try await makeTestApi(http).agentTranscript("owner", "repo", 42, ownerAccount: "owner")

        XCTAssertEqual(http.lastRequest?.bodyText, #"{"ownerAccount":"owner"}"#)
        XCTAssertEqual(detail.status, "done")
        XCTAssertTrue(detail.transcript.contains("flutter test passed"))
        XCTAssertFalse(detail.sealedForOtherDevice)
    }

    // MARK: Actions and desktop commands

    func testActionListAndLogPostOwnerAccountAndParseActionPayloads() async throws {
        let http = apiRouteByPath { path, _, _ in
            switch path {
            case "/api/repo/owner/repo/actions/list":
                return apiJSON([
                    "ok": true,
                    "workflows": [["path": ".forkmesh/test.yml", "name": "Test suite", "on": ["push"], "stepCount": 2]],
                    "runs": [["id": 7, "workflowName": "Test suite", "status": "running", "ref": "refs/heads/main"]],
                ])
            case "/api/repo/owner/repo/actions/7/log":
                return apiJSON(["ok": true, "id": 7, "status": "success", "log": "flutter test passed"])
            default:
                return nil
            }
        }
        let api = makeTestApi(http)

        let actions = try await api.repoActions("owner", "repo", ownerAccount: "owner", ts: "1000", sig: "signed-list")
        let log = try await api.actionLog("owner", "repo", 7, ownerAccount: "owner", ts: "1001", sig: "signed-log")

        let requests = http.requests
        XCTAssertEqual(requests.count, 2)
        XCTAssertEqual(requests[0].method, "POST")
        XCTAssertEqual(requests[0].apiPath, "/api/repo/owner/repo/actions/list")
        XCTAssertEqual(requests[0].bodyText, #"{"ownerAccount":"owner","ts":"1000","sig":"signed-list"}"#)
        XCTAssertEqual(actions.workflows.map(\.name), ["Test suite"])
        XCTAssertEqual(actions.runs.first?.statusLabel, "Running")
        XCTAssertEqual(requests[1].method, "POST")
        XCTAssertEqual(requests[1].apiPath, "/api/repo/owner/repo/actions/7/log")
        XCTAssertEqual(requests[1].bodyText, #"{"ownerAccount":"owner","ts":"1001","sig":"signed-log"}"#)
        XCTAssertEqual(log.statusLabel, "Success")
        XCTAssertEqual(log.log, "flutter test passed")

        // Blank optional fields are left out.
        _ = try await api.repoActions("owner", "repo")
        XCTAssertEqual(http.lastRequest?.bodyText, "{}")
    }

    func testDesktopCommandPostsSignedMobileRequestPayloadAndParsesQueueStatus() async throws {
        let http = FakeHTTPClient { _ in apiJSON(["ok": true, "queued": 1]) }
        let result = try await makeTestApi(http).desktopCommand(
            "owner", "repo", ownerAccount: "owner", command: "action.rerun", target: "run:7", ts: "1000",
            sig: "signed-command", payload: ["runId": 7, "workflowPath": ".forkmesh/test.yml"])

        XCTAssertTrue(result.ok)
        XCTAssertEqual(result.queued, 1)
        XCTAssertEqual(http.requests.count, 1)
        let request = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(request.method, "POST")
        XCTAssertEqual(request.apiPath, "/api/repo/owner/repo/desktop-commands")
        // Key order on the wire is fixed.
        XCTAssertEqual(
            request.bodyText,
            #"{"ownerAccount":"owner","command":"action.rerun","target":"run:7","ts":"1000","sig":"signed-command","#
                + #""payload":{"runId":7,"workflowPath":".forkmesh/test.yml"}}"#)
    }

    // MARK: Bounties

    func testIssueBountyStatusPostsBodyAndParsesFundingState() async throws {
        let http = apiRouteByPath { path, _, request in
            guard request.method == "POST", path == "/api/repo/owner/repo/bounty" else { return nil }
            return apiJSON([
                "ok": true,
                "bounty": [
                    "address": "escrow-status",
                    "amountUsd": 25,
                    "requiredLamports": 250_000_000,
                    "amountSol": 0.25,
                    "receivedLamports": 125_000_000,
                    "confirmed": false,
                    "status": "open",
                    "payee": "dev-node",
                    "payoutSig": "",
                    "uri": "solana:escrow-status?amount=0.25",
                ],
            ])
        }
        let bounty = try await makeTestApi(http).issueBountyStatus("owner", "repo", 12)

        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(http.lastRequest?.bodyText, #"{"action":"status","number":12}"#)
        XCTAssertEqual(bounty.address, "escrow-status")
        XCTAssertEqual(bounty.amountUsd, 25)
        XCTAssertEqual(bounty.requiredLamports, 250_000_000)
        XCTAssertEqual(bounty.receivedLamports, 125_000_000)
        XCTAssertEqual(bounty.amountSol, 0.25)
        XCTAssertEqual(bounty.status, "open")
        XCTAssertEqual(bounty.payee, "dev-node")
        XCTAssertEqual(bounty.payUri, "solana:escrow-status?amount=0.25")

        // A repo without the endpoint reads as "no bounty"; other failures surface.
        let missing = try await makeTestApi(FakeHTTPClient { _ in apiText("", status: 404) })
            .issueBountyStatus("owner", "repo", 1)
        XCTAssertEqual(missing.address, "")
        let refusing = makeTestApi(FakeHTTPClient { _ in apiText("", status: 400) })
        _ = await apiAssertThrows { try await refusing.issueBountyStatus("owner", "repo", 1) }
    }

    func testCustodialBountyCreationAndWalletPreparationFailLocally() async throws {
        let http = FakeHTTPClient()
        let api = makeTestApi(http)

        let create = await apiAssertThrows {
            try await api.createIssueBounty(
                "owner", "repo", number: 12, amountUsd: 150, payee: "owner", ts: "1770000000000", sig: "sig-create")
        }
        XCTAssertTrue(create is ApiUnsupportedError, "\(String(describing: create))")
        let wallet = await apiAssertThrows {
            try await api.bountyWallet("owner", "repo", ts: "1770000000000", sig: "wallet-sig")
        }
        XCTAssertTrue(wallet is ApiUnsupportedError, "\(String(describing: wallet))")
        XCTAssertEqual(http.requests.count, 0)
    }
}
