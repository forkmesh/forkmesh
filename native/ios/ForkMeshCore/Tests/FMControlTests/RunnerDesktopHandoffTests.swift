import FMApi
import FMBase
import FMChat
import FMCrypto
import Foundation
import XCTest

@testable import FMControl

final class RunnerDesktopHandoffTests: XCTestCase {
    private func failed() -> AgentLaunchSession {
        var session = AgentLaunchSession(
            sessionId: "r9", owner: "fmnext-n1", repo: "site", provider: "claude-code", status: "failed",
            targetNode: "fmnext-n1", title: "Fix the login page", requestedModel: "opus", localAgentId: 4,
            branchName: "agent/9-fix-login", baseBranch: "main")
        session.runner = "fmnext-r1"
        session.lastError = "OAuth session expired\nand could not be refreshed"
        session.history = [
            AgentLaunchMessage(role: "user", text: "Fix the login redirect"),
            AgentLaunchMessage(role: "assistant", text: "Found the bug in\nauth.ts."),
            AgentLaunchMessage(role: "user", text: "Also add a test"),
        ]
        return session
    }

    func testTheTaskNamesTheRunnerTheBranchAndWhatHappened() {
        let task = RunnerDesktopHandoff.task(failed())
        XCTAssertTrue(task.hasPrefix("Continue a task that the ForkMesh runner fmnext-r1 started but did not finish"))
        XCTAssertTrue(task.contains("(session r9, it ended as \"failed\")."))
        XCTAssertTrue(task.contains("It stopped with: OAuth session expired and could not be refreshed"))
        XCTAssertTrue(task.contains("pushed to branch agent/9-fix-login (started from main)."))
        XCTAssertTrue(task.contains("Task:\nFix the login redirect"))
        XCTAssertTrue(task.hasSuffix("What happened on the runner:\nAgent: Found the bug in auth.ts.\nMe: Also add a test"))
        XCTAssertEqual(RunnerDesktopHandoff.title(failed()), "Continue: Fix the login page")
    }

    func testANeverPushedRunStartsFromTheBaseAndLongHistoryIsTrimmed() {
        var session = failed()
        session.branchName = ""
        session.history.append(contentsOf: (0..<40).map {
            AgentLaunchMessage(role: "assistant", text: "step \($0) " + String(repeating: "x", count: 900))
        })
        let task = RunnerDesktopHandoff.task(session)
        XCTAssertTrue(task.contains("It left no branch, so start from the base branch."))
        XCTAssertLessThanOrEqual(task.utf16.count, RunnerDesktopHandoff.maxTaskUnits)
        XCTAssertTrue(task.contains("… earlier steps omitted"))
        XCTAssertTrue(task.contains("step 39 "))
        XCTAssertFalse(task.contains("step 0 "))
    }

    func testOnlyARelayRunWithARepositoryCanMove() {
        let row = UnifiedAgentRow.fromCloud(failed())
        XCTAssertTrue(RunnerDesktopHandoff.canMove(row, desktopPaired: true))
        XCTAssertFalse(RunnerDesktopHandoff.canMove(row, desktopPaired: false))
        var merged = row
        merged.merged = true
        XCTAssertFalse(RunnerDesktopHandoff.canMove(merged, desktopPaired: true))
    }

    func testFailuresSayWhatToDo() {
        XCTAssertEqual(
            RunnerDesktopHandoff.failureText(LocalControlException("no_checkout"), repo: "alice/site", stoppedOn: "fmnext-r1"),
            "The desktop has no working copy of alice/site. Clone it there first, then move the agent. The run on fmnext-r1 is stopped; Continue restarts it there.")
        XCTAssertEqual(
            RunnerDesktopHandoff.failureText(LocalControlException("unsupported"), repo: "alice/site"),
            "Update ForkMesh on the desktop to move runner agents to it.")
        XCTAssertEqual(
            RunnerDesktopHandoff.failureText(
                LocalControlException("not_started", message: "Branch agent/9 is not on this node."),
                repo: "alice/site", stoppedOn: "fmnext-r1"),
            "The desktop could not start the agent. Branch agent/9 is not on this node. The run on fmnext-r1 is stopped; Continue restarts it there.")
        XCTAssertEqual(
            RunnerDesktopHandoff.failureText(LocalControlException("not_started"), repo: "alice/site"),
            "The desktop could not start the agent. It says why in its Agents list.")
        XCTAssertEqual(
            RunnerDesktopMove(agentId: 42, desktopName: "desk", branch: "agent/9-fix-login").message,
            "Continuing on desk as agent #42, on agent/9-fix-login.")
    }

    @MainActor func testTheRunIsCheckedStoppedThenTakenOver() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        desktop.handler = { method, params in
            switch method {
            case "agents.list": return ["sessions": []]
            case "agents.takeover":
                return params.bool("check") ? ["id": 0, "check": true] : ["id": 42, "branch": "agent/9-fix-login"]
            default: return nil
            }
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let cloudApi = FakeCloudAgents()
        var running = failed()
        running.status = "running"
        cloudApi.sessions = [running]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)

        let move = try await center.continueOnDesktop(UnifiedAgentRow.fromCloud(running))
        let calls = desktop.requests.filter { $0.str("method") == "agents.takeover" }.map { $0.obj("params") }
        XCTAssertEqual(cloudApi.stopped, ["r9"])
        XCTAssertEqual(calls.count, 2)
        XCTAssertTrue(calls[0].bool("check"))
        XCTAssertFalse(calls[1].bool("check"))
        XCTAssertEqual(calls[1].str("cloudSession"), "r9")
        XCTAssertEqual(calls[1].str("owner"), "fmnext-n1")
        XCTAssertEqual(calls[1].str("name"), "site")
        XCTAssertEqual(calls[1].str("branch"), "agent/9-fix-login")
        XCTAssertEqual(calls[1].str("base"), "main")
        XCTAssertEqual(calls[1].str("runner"), "fmnext-r1")
        XCTAssertEqual(calls[1].str("provider"), "claude-code")
        XCTAssertEqual(calls[1].str("model"), "opus")
        XCTAssertEqual(move.agentId, 42)
        XCTAssertEqual(move.message, "Continuing on desk as agent #42, on agent/9-fix-login.")
        XCTAssertEqual(cloudApi.cancelled, ["r9"])
        XCTAssertFalse(move.runnerKept)
        XCTAssertFalse(center.rows.contains { $0.cloudSessionId == "r9" })
    }

    @MainActor func testARunnerCopyThatCannotBeDeletedIsReported() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        desktop.handler = { method, params in
            switch method {
            case "agents.list": return ["sessions": []]
            case "agents.takeover":
                return params.bool("check") ? ["id": 0, "check": true] : ["id": 42, "branch": "agent/9-fix-login"]
            default: return nil
            }
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [failed()]
        cloudApi.cancelFailure = AgentLaunchException("down", code: "session_delete_failed", status: 503)
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)

        let move = try await center.continueOnDesktop(UnifiedAgentRow.fromCloud(failed()))
        XCTAssertTrue(move.runnerKept)
        XCTAssertEqual(
            move.message,
            "Continuing on desk as agent #42, on agent/9-fix-login. Its run on the runner could not be deleted; delete it from Agents.")
    }

    @MainActor func testADesktopWithoutACheckoutLeavesTheRunnerAlone() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        desktop.refuse = { method, _ in method == "agents.takeover" ? "no_checkout" : nil }
        desktop.handler = { method, _ in method == "agents.list" ? ["sessions": []] : nil }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let cloudApi = FakeCloudAgents()
        var running = failed()
        running.status = "running"
        cloudApi.sessions = [running]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)

        do {
            _ = try await center.continueOnDesktop(UnifiedAgentRow.fromCloud(running))
            XCTFail("the move should fail")
        } catch {
            XCTAssertEqual(
                String(describing: error),
                "The desktop has no working copy of fmnext-n1/site. Clone it there first, then move the agent.")
        }
        XCTAssertTrue(cloudApi.stopped.isEmpty)
        XCTAssertTrue(cloudApi.cancelled.isEmpty)
    }
}
