// The ApiService cases of the agent queue (the pure model cases are in AgentLaunchTests.swift),
// plus the request shapes behind agent sessions and action sessions (their model parses are in
// ModelsTests.swift).

import FMBase
import FMNet
import Foundation
import XCTest
@testable import FMApi

@MainActor
final class ApiAgentsTests: XCTestCase {
    // MARK: agent queue

    func testQueueingARunPostsThePromptWithARecognizedOrigin() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true,
                "queueState": "ready_for_claim",
                "targetOnline": true,
                "message": "Queued on desk-1.",
                "session": ["id": "sess-1", "status": "queued"],
            ], 202)
        }
        let api = ApiPortFixture.api(http)

        let result = try await api.launchAgentSession(
            "alice", "mobile", prompt: "Fix the crash", targetNode: "desk-1", title: "Fix the crash")

        let seen = try XCTUnwrap(http.requests.first)
        XCTAssertEqual(http.requests.count, 1)
        XCTAssertEqual(seen.method, "POST")
        XCTAssertEqual(portRawPath(seen), "/api/repo/alice/mobile/agent-sessions")
        // The Worker refuses a mutation whose Origin it does not recognize, so this header is
        // what makes the call work at all from a native client.
        XCTAssertEqual(seen.headers["Origin"], "http://relay.test")
        XCTAssertEqual(seen.headers["Authorization"], "Bearer session-token")

        let sent = portBody(seen)
        XCTAssertEqual(sent["prompt"], "Fix the crash")
        XCTAssertEqual(sent["targetNode"], "desk-1")
        XCTAssertEqual(sent["createPr"], false)

        XCTAssertTrue(result.targetOnline)
        XCTAssertFalse(result.waitingForNode)
        XCTAssertEqual(result.label, "Queued on desk-1.")
        XCTAssertEqual(result.session?.sessionId, "sess-1")
    }

    func testAMemberWhoDoesNotOwnTheNodeIsSentToTheOrgRoute() async throws {
        let http = FakeHTTPClient { request in
            if portRawPath(request).hasSuffix("/agent-sessions") {
                return portJSON(["ok": false, "error": "forbidden"], 403)
            }
            if request.method == "GET" {
                return portJSON([
                    "ok": true,
                    "engineeringAccess": true,
                    "targets": [
                        ["node": "desk-1", "online": true, "source": true],
                        ["node": "mirror-1", "online": true, "source": false],
                    ],
                    "sessions": [["id": "sess-9", "status": "running"]],
                ])
            }
            return portJSON(["ok": true, "queueState": "ready_for_claim"], 202)
        }
        let api = ApiPortFixture.api(http)

        let options = try await api.agentLaunchOptions("acme", "mobile")
        XCTAssertTrue(options.orgScope)
        XCTAssertTrue(options.canQueue)
        // A mirror is always up; the source desktop may not be.
        XCTAssertEqual(options.preferredTarget?.node, "mirror-1")
        XCTAssertEqual(options.sessions.count, 1)
        XCTAssertEqual(options.sessions.first?.orgScope, true)

        _ = try await api.launchAgentSession("acme", "mobile", prompt: "Fix it", orgScope: options.orgScope)
        _ = try await api.steerAgentSession("acme", "mobile", "sess-9", prompt: "And test it", orgScope: true)
        XCTAssertEqual(http.requests.map(portMethodPath), [
            "GET /api/repo/acme/mobile/agent-sessions",
            "GET /api/orgs/acme/repos/mobile/agent-bots",
            "POST /api/orgs/acme/repos/mobile/agent-bots",
            "POST /api/orgs/acme/repos/mobile/agent-bots/sess-9",
        ])
    }

    func testTheOrgRouteIsAskedByTheOrganizationNotTheServingMirror() async throws {
        let http = FakeHTTPClient { request in
            let path = portRawPath(request)
            if path == "/api/repositories" {
                return portJSON([[
                    "owner": "mirror35",
                    "name": "forkmesh",
                    "logicalOwners": [["kind": "organization", "owner": "forkmesh"]],
                ]])
            }
            if path.hasSuffix("/agent-sessions") {
                return portJSON(["ok": false, "error": "forbidden"], 403)
            }
            return portJSON(["ok": true, "canQueueAgent": true], request.method == "GET" ? 200 : 202)
        }
        let api = ApiPortFixture.api(http)

        let repos = try await api.repositories()
        XCTAssertEqual(repos.count, 1)
        let repo = try XCTUnwrap(repos.first)
        XCTAssertEqual(repo.owner, "mirror35")
        XCTAssertEqual(repo.fullName, "forkmesh/forkmesh")

        let options = try await api.agentLaunchOptions(repo.owner, repo.name)
        _ = try await api.launchAgentSession(repo.owner, repo.name, prompt: "Fix it", orgScope: options.orgScope)
        XCTAssertEqual(http.requests.map(portMethodPath), [
            "GET /api/repositories",
            "GET /api/repo/mirror35/forkmesh/agent-sessions",
            "GET /api/orgs/forkmesh/repos/forkmesh/agent-bots",
            "POST /api/orgs/forkmesh/repos/forkmesh/agent-bots",
        ])
    }

    func testAnOfflineTargetStillQueuesAndSaysSo() async throws {
        let http = FakeHTTPClient { _ in
            portJSON(["ok": true, "queueState": "waiting_for_desktop", "targetOnline": false], 202)
        }
        let api = ApiPortFixture.api(http)

        let result = try await api.launchAgentSession("alice", "mobile", prompt: "Add a test")

        XCTAssertTrue(result.waitingForNode)
        XCTAssertTrue(result.label.contains("as soon as the node connects"), result.label)
    }

    func testARefusalSurfacesTheRelayMessageNotTheStatusCode() async {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": false, "error": "forbidden", "message": "Only this repository owner can start agents.",
            ], 403)
        }
        let api = ApiPortFixture.api(http)

        let error = await portCaughtError { try await api.launchAgentSession("alice", "mobile", prompt: "Go") }
        let refusal = error as? AgentLaunchException
        XCTAssertNotNil(refusal, "\(String(describing: error))")
        XCTAssertEqual(refusal?.message, "Only this repository owner can start agents.")
        XCTAssertEqual(refusal?.code, "forbidden")
    }

    func testASignedOutClientAsksTheRelayNothing() async {
        let http = FakeHTTPClient { _ in portJSON([:]) }
        let api = ApiPortFixture.api(http, token: "")

        let options = try? await api.agentLaunchOptions("alice", "mobile")

        XCTAssertEqual(http.requests.count, 0)
        XCTAssertNotNil(options)
        XCTAssertEqual(options?.hasTarget, false)
        let error = await portCaughtError { try await api.launchAgentSession("alice", "mobile", prompt: "Go") }
        XCTAssertTrue(error is AgentLaunchException, "\(String(describing: error))")
        XCTAssertEqual(http.requests.count, 0)
    }

    func testListingPicksAnOnlineNodeOverAnOfflineOne() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true,
                "canQueueAgent": true,
                "targets": [
                    ["node": "sleepy", "online": false],
                    ["node": "mirror-7", "online": true, "source": "mirror"],
                ],
                "sessions": [[
                    "id": "sess-9",
                    "status": "running",
                    "targetNode": "mirror-7",
                    "title": "Fix the crash",
                    "history": [["role": "user", "text": "Fix the crash"]],
                ]],
            ])
        }
        let api = ApiPortFixture.api(http)

        let options = try await api.agentLaunchOptions("alice", "mobile")

        XCTAssertTrue(options.canQueue)
        XCTAssertEqual(options.preferredTarget?.node, "mirror-7")
        XCTAssertEqual(options.preferredTarget?.isMirror, true)
        XCTAssertEqual(options.sessions.count, 1)
        XCTAssertEqual(options.sessions.first?.isActive, true)
        XCTAssertEqual(options.sessions.first?.displayTitle, "Fix the crash")
    }

    func testTheAccountListCarriesWhereItsFinishedWindowWasCut() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true,
                "sessions": [["id": "sess-1", "status": "completed", "updatedAt": 1_700_000_500_000]],
                "finishedBefore": 1_700_000_500_000,
            ])
        }
        let api = ApiPortFixture.api(http)

        let list = try await api.accountAgentSessions()

        XCTAssertEqual(http.requests.map(portMethodPath), ["GET /api/account/agent-sessions"])
        XCTAssertEqual(list.sessions.map(\.sessionId), ["sess-1"])
        XCTAssertEqual(list.finishedBeforeMs, 1_700_000_500_000)

        let signedOut = try await ApiPortFixture.api(http, token: "").accountAgentSessions()
        XCTAssertEqual(signedOut, AgentSessionList())
    }

    func testLargeAgentAndAccountRepliesDecodeOffTheMainActorTheSame() async throws {
        let turn: String = String(repeating: "Read the file \u{2014} then edit it. ", count: 120)
        let sessions: [JSONValue] = (0..<12).map { (index: Int) -> JSONValue in
            .object([
                "id": .string("sess-\(index)"),
                "status": .string(index == 0 ? "running" : "completed"),
                "history": .array([
                    .object(["role": "user", "text": .string("Task \(index)")]),
                    .object(["role": "assistant", "text": .string(turn)]),
                ]),
            ])
        }
        let reply: JSONObject = ["ok": true, "canQueueAgent": true, "sessions": .array(sessions)]
        let notes: JSONObject = ["ok": true, "notes": .array((0..<200).map { .object(["id": .int(Int64($0)), "body": .string(turn)]) })]
        XCTAssertGreaterThan(JSON.encodeData(.object(reply)).count, ApiService.detachedParseBytes)
        XCTAssertGreaterThan(JSON.encodeData(.object(notes)).count, ApiService.detachedParseBytes)
        let http = FakeHTTPClient { request in
            request.url.path.hasPrefix("/api/notes") ? portJSON(.object(notes)) : portJSON(.object(reply))
        }
        let api = ApiPortFixture.api(http)

        let options = try await api.agentLaunchOptions("alice", "mobile")
        let account = try await api.accountAgentSessions()
        let listed = try await api.accountApiCall("GET", "/api/notes")

        XCTAssertTrue(options.canQueue)
        XCTAssertEqual(options.sessions.map(\.sessionId), (0..<12).map { "sess-\($0)" })
        XCTAssertEqual(options.sessions.first?.history.last?.text, turn)
        XCTAssertEqual(account.sessions.count, 12)
        XCTAssertEqual(account.sessions.first?.isActive, true)
        XCTAssertEqual(listed.list("notes").count, 200)
    }

    func testAFollowUpPromptSteersTheNamedSession() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": true], 202) }
        let api = ApiPortFixture.api(http)

        _ = try await api.steerAgentSession("alice", "mobile", "sess 1/2", prompt: "Also update the changelog")

        XCTAssertEqual(http.lastRequest.map(portRawPath), "/api/repo/alice/mobile/agent-sessions/sess%201%2F2")
    }

    func testRetryingAFailedRunResumesItOnItsOwnBranch() async throws {
        let http = FakeHTTPClient { _ in
            portJSON(["ok": true, "status": "queued", "message": "Follow-up sent."], 202)
        }
        let api = ApiPortFixture.api(http)

        let outcome = try await api.retryAgentSession("alice", "mobile", AgentLaunchSession(
            sessionId: "sess-9",
            status: "failed",
            targetNode: "mirror-7",
            history: [
                AgentLaunchMessage(role: "user", text: "Fix the crash"),
                AgentLaunchMessage(role: "assistant", text: "Ran out of context."),
            ],
            // A node claimed this one, so its branch and its conversation are still there to
            // carry on from.
            localAgentId: 4,
            branchName: "agent/web-54-retry"))

        XCTAssertTrue(outcome.resumed)
        let seen = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(seen.method, "POST")
        XCTAssertEqual(portRawPath(seen), "/api/repo/alice/mobile/agent-sessions/sess-9")
        // The task it was given, not the agent's last word about it.
        XCTAssertEqual(portBody(seen)["prompt"], "Fix the crash")
        XCTAssertEqual(outcome.label, "Follow-up sent.")
    }

    func testRetryingARunNoNodeEverStartedQueuesItAgain() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true,
                "queueState": "ready_for_claim",
                "targetOnline": true,
                "message": "Sent to mirror-7.",
                "session": ["id": "sess-10", "status": "queued"],
            ], 202)
        }
        let api = ApiPortFixture.api(http)

        let outcome = try await api.retryAgentSession("alice", "mobile", AgentLaunchSession(
            sessionId: "sess-9",
            provider: "claude-code",
            status: "rejected",
            targetNode: "mirror-7",
            title: "Fix the crash",
            requestedModel: "opus",
            createPr: true,
            issueNumber: 54,
            history: [AgentLaunchMessage(role: "user", text: "Fix the crash")]))

        XCTAssertFalse(outcome.resumed)
        let seen = try XCTUnwrap(http.lastRequest)
        // The collection, not the session: there is nothing to follow up on.
        XCTAssertEqual(portRawPath(seen), "/api/repo/alice/mobile/agent-sessions")
        let sent = portBody(seen)
        XCTAssertEqual(sent["prompt"], "Fix the crash")
        XCTAssertEqual(sent["provider"], "claude-code")
        XCTAssertEqual(sent["model"], "opus")
        XCTAssertEqual(sent["targetNode"], "mirror-7")
        XCTAssertEqual(sent["issueNumber"], 54)
        XCTAssertEqual(sent["createPr"], true)
        // The new run is the one to follow from here.
        XCTAssertEqual(outcome.session?.sessionId, "sess-10")
    }

    func testASessionThatKeptNoTaskIsNotRetriedBlind() async {
        let http = FakeHTTPClient { _ in portJSON([:], 202) }
        let api = ApiPortFixture.api(http)

        let error = await portCaughtError {
            try await api.retryAgentSession("alice", "mobile", AgentLaunchSession(sessionId: "sess-9", status: "failed"))
        }
        XCTAssertTrue(error is AgentLaunchException, "\(String(describing: error))")
        XCTAssertEqual(http.requests.count, 0)
    }

    /// The Agents screen drives two retry routes; this pins their two outcomes at the API level.
    func testTheRetryFixturesOfTheAgentsScreenTakeTheirRoutes() async throws {
        let http = FakeHTTPClient { _ in
            portJSON(["queueState": "ready_for_claim", "targetOnline": true, "message": "ok"], 202)
        }
        let api = ApiPortFixture.api(http)
        let failed = AgentLaunchSession(
            sessionId: "sess-9", provider: "claude-code", status: "failed", targetNode: "mirror-7",
            title: "Fix the crash", createPr: true,
            history: [
                AgentLaunchMessage(role: "user", text: "Fix the crash"),
                AgentLaunchMessage(role: "assistant", text: "Ran out of context."),
            ],
            localAgentId: 4, branchName: "agent/web-54-retry", lastError: "The provider ran out of context.")
        let neverStarted = AgentLaunchSession(
            sessionId: "sess-8", provider: "claude-code", status: "rejected", targetNode: "mirror-7",
            title: "Fix the crash", createPr: true, issueNumber: 54,
            history: [AgentLaunchMessage(role: "user", text: "Fix the crash")])

        let resumed = try await api.retryAgentSession("alice", "mobile", failed)
        let requeued = try await api.retryAgentSession("alice", "mobile", neverStarted)
        XCTAssertTrue(resumed.resumed)
        XCTAssertFalse(requeued.resumed)
        XCTAssertEqual(http.requests.map(portMethodPath), [
            "POST /api/repo/alice/mobile/agent-sessions/sess-9",
            "POST /api/repo/alice/mobile/agent-sessions",
        ])
        XCTAssertEqual(JSON.encode(.object(portBody(http.requests[0]))), #"{"prompt":"Fix the crash"}"#)
        let launch = portBody(http.requests[1])
        XCTAssertEqual(launch["targetNode"], "mirror-7")
        XCTAssertEqual(launch["provider"], "claude-code")
        XCTAssertEqual(launch["issueNumber"], 54)
    }

    func testAFollowUpOnARetryResumesWithItOrRunsTheTaskAgainWithIt() async throws {
        let http = FakeHTTPClient { _ in
            portJSON(["queueState": "ready_for_claim", "targetOnline": true, "message": "ok"], 202)
        }
        let api = ApiPortFixture.api(http)
        let claimed = AgentLaunchSession(
            sessionId: "sess-9", provider: "claude-code", status: "failed", targetNode: "mirror-7",
            history: [AgentLaunchMessage(role: "user", text: "Fix the crash")], localAgentId: 4)
        let neverStarted = AgentLaunchSession(
            sessionId: "sess-8", provider: "claude-code", status: "failed", targetNode: "fmnext-n1",
            title: "Fix the crash", history: [AgentLaunchMessage(role: "user", text: "Fix the crash")])
        XCTAssertFalse(claimed.restartsOnPrompt)
        XCTAssertTrue(neverStarted.restartsOnPrompt)

        _ = try await api.retryAgentSession("alice", "mobile", claimed, followUp: " and the test ", attachments: [])
        _ = try await api.retryAgentSession("alice", "mobile", neverStarted, followUp: " and the test ", attachments: [])
        XCTAssertEqual(http.requests.map(portMethodPath), [
            "POST /api/repo/alice/mobile/agent-sessions/sess-9",
            "POST /api/repo/alice/mobile/agent-sessions",
        ])
        XCTAssertEqual(JSON.encode(.object(portBody(http.requests[0]))), #"{"prompt":"and the test"}"#)
        XCTAssertEqual(portBody(http.requests[1])["prompt"], "Fix the crash\n\nand the test")
    }

    // MARK: agent sessions (API side)

    func testAgentSessionsPostTheListAndParseTheRows() async throws {
        let http = FakeHTTPClient { _ in
            portJSON([
                "ok": true,
                "agents": [[
                    "id": 42, "issueNumber": 12, "issueTitle": "Stabilize signed inbox", "status": "running",
                    "provider": "openai", "model": "gpt-5.5", "branchName": "agent/fix-inbox", "numTurns": 7,
                    "durationMs": 125000, "costUsd": 1.25,
                ]],
            ])
        }
        let api = ApiPortFixture.api(http)

        let sessions = try await api.agentSessions("owner", "repo", ownerAccount: " Owner ")

        let seen = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(portMethodPath(seen), "POST /api/repo/owner/repo/agents/list")
        XCTAssertEqual(seen.bodyText, #"{"ownerAccount":"owner"}"#)
        XCTAssertEqual(seen.headers["Authorization"], "Bearer session-token")
        XCTAssertEqual(sessions.count, 1)
        XCTAssertEqual(sessions.first?.id, 42)
        XCTAssertEqual(sessions.first?.issueTitle, "Stabilize signed inbox")
        XCTAssertEqual(sessions.first?.providerLabel, "OpenAI")
        XCTAssertEqual(sessions.first?.durationLabel, "2m 5s")

        _ = try await api.agentSessions("owner", "repo")
        XCTAssertEqual(http.lastRequest?.bodyText, "{}")
    }

    func testAPlainTranscriptIsReadAsIs() async throws {
        let http = FakeHTTPClient { _ in
            portJSON(["status": "done", "transcript": "Running tests\ndart test passed\nOpened PR #7"])
        }
        let api = ApiPortFixture.api(http)

        let transcript = try await api.agentTranscript("owner", "repo", 42)

        XCTAssertEqual(http.lastRequest.map(portMethodPath), "POST /api/repo/owner/repo/agents/42/transcript")
        XCTAssertEqual(transcript.status, "done")
        XCTAssertTrue(transcript.transcript.contains("dart test passed"))
        XCTAssertFalse(transcript.sealedForOtherDevice)
    }

    // MARK: actions and desktop commands (API side)

    func testActionReadsCarryTheSignedOwnerFields() async throws {
        let http = FakeHTTPClient { request in
            if portRawPath(request).hasSuffix("/log") {
                return portJSON(["id": "7", "status": "success", "log": "checkout\ndart test passed"])
            }
            return portJSON([
                "workflows": [["path": ".forkmesh/test.yml", "name": "Test suite", "on": ["push", "workflow_dispatch"]]],
                "runs": [["id": "7", "workflowName": "Test suite", "status": "running", "ref": "refs/heads/main"]],
            ])
        }
        let api = ApiPortFixture.api(http)

        let actions = try await api.repoActions("owner", "repo", ownerAccount: " Owner ", ts: " 1700 ", sig: " sig ")
        let log = try await api.actionLog("owner", "repo", 7, ownerAccount: "owner", ts: "1700", sig: "sig")

        XCTAssertEqual(http.requests.map(portMethodPath), [
            "POST /api/repo/owner/repo/actions/list",
            "POST /api/repo/owner/repo/actions/7/log",
        ])
        XCTAssertEqual(http.requests[0].bodyText, #"{"ownerAccount":"owner","ts":"1700","sig":"sig"}"#)
        XCTAssertEqual(http.requests[1].bodyText, #"{"ownerAccount":"owner","ts":"1700","sig":"sig"}"#)
        XCTAssertEqual(actions.workflows.first?.triggers, ["push", "workflow_dispatch"])
        XCTAssertEqual(actions.runs.first?.statusLabel, "Running")
        XCTAssertEqual(log.id, 7)
        XCTAssertEqual(log.statusLabel, "Success")

        _ = try await api.repoActions("owner", "repo")
        XCTAssertEqual(http.lastRequest?.bodyText, "{}")
    }

    func testARerunRequestPostsTheDesktopCommand() async throws {
        let http = FakeHTTPClient { _ in portJSON(["ok": true, "queued": 1]) }
        let api = ApiPortFixture.api(http)

        let result = try await api.desktopCommand(
            "owner", "repo", ownerAccount: " Owner ", command: " action.rerun ", target: "run:7", ts: "1700000000000",
            sig: "c2ln", payload: ["runId": 7, "workflowPath": ".forkmesh/test.yml"])

        let seen = try XCTUnwrap(http.lastRequest)
        XCTAssertEqual(portMethodPath(seen), "POST /api/repo/owner/repo/desktop-commands")
        XCTAssertEqual(
            seen.bodyText,
            #"{"ownerAccount":"owner","command":"action.rerun","target":"run:7","ts":"1700000000000","sig":"c2ln","#
                + #""payload":{"runId":7,"workflowPath":".forkmesh/test.yml"}}"#)
        XCTAssertTrue(result.ok)
        XCTAssertEqual(result.queued, 1)
    }
}
