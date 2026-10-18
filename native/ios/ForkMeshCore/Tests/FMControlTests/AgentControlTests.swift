// The unified Agents list: desktop agents (LAN or relay) and node runs (relay queue) in one list,
// each with its transport badge and the controls that transport supports.

import FMApi
import FMBase
import FMChat
import FMCrypto
import FMNet
import Foundation
import XCTest

@testable import FMControl

@MainActor
final class FakeCloudAgents: CloudAgentsApi {
    var sessions: [AgentLaunchSession] = []
    var finishedBefore = 0
    var failure: Error?
    var stopped: [String] = []
    var steered: [(String, String)] = []

    var hold: (@MainActor () async -> Void)?

    func accountAgentSessions() async throws -> AgentSessionList {
        if let hold { await hold() }
        if let failure { throw failure }
        return AgentSessionList(sessions: sessions, finishedBeforeMs: finishedBefore)
    }

    var sessionReads = 0

    func agentLaunchSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> AgentLaunchSession
    {
        sessionReads += 1
        guard var session = sessions.first(where: { $0.sessionId == sessionId }) else {
            throw AgentLaunchException("not found", code: "not_found", status: 404)
        }
        session.history = [
            AgentLaunchMessage(
                role: "user", author: "alice", text: "Fix it",
                attachments: [AgentAttachmentMeta(id: "p1", name: "shot.png", mediaType: "image/png", size: 10)]),
            AgentLaunchMessage(role: "assistant", author: "claude-code", text: "Fixed."),
        ]
        return session
    }

    func stopAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> AgentStopResult
    {
        stopped.append(sessionId)
        return AgentStopResult(stopped: false, outcome: "stop_requested", message: "Stop sent.")
    }

    var cancelled: [String] = []
    var cancelFailure: Error?

    func cancelAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws {
        if let cancelFailure { throw cancelFailure }
        cancelled.append(sessionId)
        sessions.removeAll { $0.sessionId == sessionId }
    }

    var steeredImages: [[PromptAttachment]] = []

    func steerAgentSession(
        _ owner: String, _ repo: String, _ sessionId: String, prompt: String, orgScope: Bool,
        attachments: [PromptAttachment]
    ) async throws -> AgentQueueResult {
        steered.append((sessionId, prompt))
        steeredImages.append(attachments)
        return AgentQueueResult(json: ["ok": true, "status": "queued"])
    }

    var retried: [(String, String, Int)] = []

    func retryAgentSession(
        _ owner: String, _ repo: String, _ session: AgentLaunchSession, followUp: String,
        attachments: [PromptAttachment]
    ) async throws -> AgentRetryResult {
        retried.append((session.sessionId, session.retryPrompt(followUp: followUp), attachments.count))
        return AgentRetryResult(
            result: AgentQueueResult(json: ["ok": true, "message": "Queued again."]), resumed: session.canResume)
    }

    var liveStatus = 404
    var liveBody = ""
    var liveOpened: [String] = []

    func agentLiveStream(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> HTTPStreamedResponse
    {
        liveOpened.append("\(owner)/\(repo)/\(sessionId)")
        let text = liveBody
        let body = AsyncThrowingStream<Data, Error> { continuation in
            if !text.isEmpty { continuation.yield(Data(text.utf8)) }
            continuation.finish()
        }
        return HTTPStreamedResponse(status: liveStatus, body: body)
    }
}

@MainActor
final class CloudGate {
    private var waiters: [CheckedContinuation<Void, Never>] = []
    private var opened = false

    func wait() async {
        if opened { return }
        await withCheckedContinuation { waiters.append($0) }
    }

    func open() {
        opened = true
        let pending = waiters
        waiters = []
        for waiter in pending { waiter.resume() }
    }
}

final class AgentControlTests: XCTestCase {
    private func desktopRow(_ id: Int, status: String, stoppable: Bool = false) -> LocalAgentRow {
        LocalAgentRow.fromJSON([
            "id": .int(Int64(id)), "repositoryOwner": "alice", "repositoryName": "app", "status": .string(status),
            "provider": "claude-code", "prompt": "Fix the login page", "stoppable": .bool(stoppable),
            "createdAtMs": .int(Int64(id * 1000)),
        ])
    }

    private func cloud(_ id: String, status: String, node: String, localId: Int = 0, updated: Int = 0) -> AgentLaunchSession {
        AgentLaunchSession(
            sessionId: id, owner: "alice", repo: "site", provider: "codex", status: status, targetNode: node,
            title: "Update the docs", updatedAtMs: updated, localAgentId: localId)
    }

    func testRowsCarryTheirTransportAndControls() {
        let lan = UnifiedAgentRow.fromDesktop(desktopRow(7, status: "running", stoppable: true), transport: .lan, desktop: "desk")
        XCTAssertEqual(lan.badge, .desktopLan)
        XCTAssertEqual(lan.id, "desktop:7")
        XCTAssertTrue(lan.canStop && lan.canPrompt && !lan.canContinue)
        XCTAssertEqual(lan.title, "Fix the login page")
        let relay = UnifiedAgentRow.fromDesktop(desktopRow(8, status: "completed"), transport: .relay, desktop: "desk")
        XCTAssertEqual(relay.badge, .desktopRelay)
        XCTAssertTrue(relay.canContinue && !relay.canStop)

        let node = UnifiedAgentRow.fromCloud(cloud("s1", status: "running", node: "mirror35", localId: 3))
        XCTAssertEqual(node.badge, .node)
        XCTAssertEqual(node.node, "mirror35")
        XCTAssertTrue(node.canStop && node.canPrompt && !node.canContinue)
        let queued = UnifiedAgentRow.fromCloud(cloud("s2", status: "queued", node: "mirror36"))
        XCTAssertTrue(queued.canStop, "a queued run can be withdrawn")
        XCTAssertFalse(queued.canPrompt, "no follow-up before a node claims it")
        XCTAssertEqual(Set(AgentTransportBadge.allCases.map(\.rawValue)), ["lan", "relay", "node"])
    }

    func testTheLiveDesktopRowMatchesRepoAndSession() {
        let desk = UnifiedAgentRow.fromDesktop(desktopRow(7, status: "done"), transport: .relay, desktop: "desk")
        var outside = desk
        outside.outside = true
        let node = UnifiedAgentRow.fromCloud(
            AgentLaunchSession(sessionId: "s7", owner: "alice", repo: "app", status: "completed", localAgentId: 7))
        let rows = [outside, node, desk]
        XCTAssertEqual(liveDesktopRow(rows, repoName: "App", sessionId: 7), desk)
        XCTAssertNil(liveDesktopRow(rows, repoName: "app", sessionId: 8))
        XCTAssertNil(liveDesktopRow(rows, repoName: "site", sessionId: 7))
    }

    func testRowsCarryTheBranchTheirDiffCompares() {
        let desk = UnifiedAgentRow.fromDesktop(
            LocalAgentRow.fromJSON([
                "id": .int(9), "repositoryOwner": "alice", "repositoryName": "app", "status": "completed",
                "branchName": "agent/9-fix", "baseRef": "main",
            ]), transport: .lan, desktop: "desk")
        XCTAssertEqual(desk.branchName, "agent/9-fix")
        XCTAssertEqual(desk.baseBranch, "main")
        let node = UnifiedAgentRow.fromCloud(
            AgentLaunchSession(
                sessionId: "s3", owner: "alice", repo: "site", status: "completed", branchName: "agent/s3",
                baseBranch: "dev"))
        XCTAssertEqual(node.branchName, "agent/s3")
        XCTAssertEqual(node.baseBranch, "dev")
    }

    func testThePullAgentIsFoundByNumberOrBranch() {
        let desk = UnifiedAgentRow.fromDesktop(
            LocalAgentRow.fromJSON([
                "id": .int(9), "repositoryOwner": "alice", "repositoryName": "app", "status": "completed",
                "branchName": "agent/9-fix", "diffStats": ["conflicted": true],
            ]), transport: .lan, desktop: "desk")
        XCTAssertTrue(desk.conflicted)
        let node = UnifiedAgentRow.fromCloud(
            AgentLaunchSession(sessionId: "s3", owner: "alice", repo: "app", status: "completed", branchName: "agent/s3", prNumber: 42))
        let rows = [desk, node]
        XCTAssertEqual(UnifiedAgentRow.forPull(rows, owner: "alice", name: "app", number: 42, head: "feature")?.id, node.id)
        XCTAssertEqual(UnifiedAgentRow.forPull(rows, owner: "alice", name: "app", number: 7, head: "agent/9-fix")?.id, desk.id)
        XCTAssertNil(UnifiedAgentRow.forPull(rows, owner: "bob", name: "app", number: 42, head: ""))
        XCTAssertNil(UnifiedAgentRow.forPull(rows, owner: "alice", name: "app", number: 0, head: ""))
    }

    func testMergeShowsADesktopRunOnceAndPutsRunningFirst() {
        let desk = [
            UnifiedAgentRow.fromDesktop(desktopRow(3, status: "completed"), transport: .lan, desktop: "desk"),
            UnifiedAgentRow.fromDesktop(desktopRow(4, status: "running", stoppable: true), transport: .lan, desktop: "desk"),
        ]
        let nodes = [
            // The desktop took this website job itself: it is desktop row 4.
            UnifiedAgentRow.fromCloud(cloud("dup", status: "running", node: "Desk", localId: 4, updated: 99_000)),
            UnifiedAgentRow.fromCloud(cloud("m", status: "completed", node: "mirror35", localId: 4, updated: 50_000)),
            UnifiedAgentRow.fromCloud(cloud("q", status: "queued", node: "mirror36", updated: 1)),
        ]
        let merged = mergeAgentRows(desktop: desk, cloud: nodes, desktopName: "desk")
        XCTAssertEqual(merged.map(\.id), ["desktop:4", "node:q", "node:m", "desktop:3"])
    }

    func testTranscriptDeltasAppendAndReset() {
        var follower = TranscriptFollower(limit: 20)
        follower.apply(AgentTranscriptDelta.fromDesktop(
            ["transcript": "hello ", "from": 0, "next": 6, "reset": false, "active": true, "status": "running"], since: 0))
        follower.apply(AgentTranscriptDelta.fromDesktop(
            ["transcript": "world", "from": 6, "next": 11, "reset": false, "active": true], since: 6))
        XCTAssertEqual(follower.text, "hello world")
        XCTAssertEqual(follower.next, 11)
        follower.apply(AgentTranscriptDelta.fromDesktop(["transcript": "fresh", "from": 0, "next": 5, "reset": true], since: 11))
        XCTAssertEqual(follower.text, "fresh")
        // An older desktop ignores `since` and sends the whole tail: replace, never duplicate.
        let old = AgentTranscriptDelta.fromDesktop(["transcript": "whole log", "id": 3], since: 11)
        XCTAssertTrue(old.reset)
        follower.apply(AgentTranscriptDelta(text: String(repeating: "x", count: 30), next: 40, reset: false, active: false, status: ""))
        XCTAssertEqual(follower.text.count, 20, "the follower keeps only the newest text")
    }

    func testFollowerKeepsTheListedAttachmentsOrFallsBackToMarkers() {
        var follower = TranscriptFollower()
        follower.apply(AgentTranscriptDelta.fromDesktop(
            ["transcript": "Attached image: /tmp/shot.png\n", "from": 0, "next": 30, "reset": true], since: 0, sessionId: 4))
        XCTAssertEqual(follower.attachments.map(\.name), ["shot.png"])
        XCTAssertEqual(follower.attachments.first?.origin, .elsewhere)
        let id: String = String(repeating: "b", count: 32)
        follower.apply(AgentTranscriptDelta.fromDesktop(
            ["transcript": "more", "from": 30, "next": 34, "attachments": [["id": .string(id), "name": "b.jpg"]]], since: 30, sessionId: 4))
        XCTAssertEqual(follower.attachments.map(\.id), ["desktop:4:\(id)"])
        follower.apply(AgentTranscriptDelta.fromDesktop(["transcript": "!", "from": 34, "next": 35], since: 34, sessionId: 4))
        XCTAssertEqual(follower.attachments.map(\.name), ["b.jpg"])
    }

    @MainActor func testControlsRouteToTheDesktopOrTheRelayQueue() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        desktop.handler = { method, params in
            switch method {
            case "agents.list":
                return [
                    "sessions": [[
                        "id": 7, "repositoryOwner": "alice", "repositoryName": "app", "status": "running",
                        "provider": "claude-code", "prompt": "Fix", "stoppable": true, "startedAtMs": 500,
                        "mode": "Plan",
                    ]],
                    "running": 1,
                    "defaultMode": "Ask",
                ]
            case "agents.transcript":
                XCTAssertEqual(params.int("since"), 5)
                return [
                    "transcript": "more", "from": 5, "next": 9, "active": true, "status": "running",
                    "attachments": [["id": .string(String(repeating: "a", count: 32)), "name": "a.png", "mediaType": "image/png"]],
                ]
            default:
                return nil
            }
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [cloud("s1", status: "running", node: "mirror35", localId: 3, updated: 10)]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)

        await center.refresh()
        XCTAssertEqual(center.rows.map(\.id), ["desktop:7", "node:s1"])
        XCTAssertEqual(center.rows.first?.badge, .desktopRelay)

        let deskRow = center.rows[0]
        let nodeRow = center.rows[1]
        try await center.stop(deskRow)
        XCTAssertTrue(desktop.requests.contains { $0.str("method") == "agents.stop" && $0.obj("params").int("id") == 7 })
        try await center.stop(nodeRow)
        XCTAssertEqual(cloudApi.stopped, ["s1"])
        try await center.prompt(nodeRow, "  and add a test ")
        XCTAssertEqual(cloudApi.steered.first?.1, "and add a test")
        try await center.prompt(deskRow, "go on")
        XCTAssertTrue(desktop.requests.contains { $0.str("method") == "agents.prompt" && $0.obj("params").str("text") == "go on" })
        XCTAssertEqual(deskRow.mode, "Plan")
        XCTAssertTrue(deskRow.offersModeChoice)
        XCTAssertFalse(nodeRow.offersModeChoice)
        XCTAssertEqual(center.desktopDefaultMode, "Ask")
        try await center.prompt(deskRow, "now do it", images: [], model: "claude-sonnet-5", mode: "Auto")
        let switched = try XCTUnwrap(desktop.requests.last { $0.str("method") == "agents.prompt" }).obj("params")
        XCTAssertEqual(switched.str("mode"), "Auto")
        XCTAssertEqual(switched.str("model"), "claude-sonnet-5")
        XCTAssertNil(switched["provider"])
        try await center.prompt(
            deskRow, "over to codex", images: [], model: "gpt-5.6-terra", mode: "", provider: "codex")
        let handedOff = try XCTUnwrap(desktop.requests.last { $0.str("method") == "agents.prompt" }).obj("params")
        XCTAssertEqual(handedOff.str("provider"), "codex")
        XCTAssertEqual(handedOff.str("model"), "gpt-5.6-terra")
        try await center.prompt(
            deskRow, "stay", images: [], model: "claude-haiku-4-5", mode: "", provider: "claude-code")
        XCTAssertNil(try XCTUnwrap(desktop.requests.last { $0.str("method") == "agents.prompt" }).obj("params")["provider"])
        try await center.prompt(nodeRow, "and docs", images: [], model: "claude-sonnet-5", mode: "Auto")
        XCTAssertEqual(cloudApi.steered.last?.1, "and docs")

        let delta = try await center.transcript(deskRow, since: 5)
        XCTAssertEqual(delta.text, "more")
        XCTAssertEqual(delta.next, 9)
        XCTAssertEqual(delta.attachments?.map(\.origin), [.desktop(sessionId: 7)])
        let nodeTranscript = try await center.transcript(nodeRow, since: 0)
        XCTAssertTrue(nodeTranscript.reset)
        XCTAssertEqual(nodeTranscript.text, "> Fix it\n\nFixed.")
        XCTAssertEqual(nodeTranscript.attachments?.map(\.name), ["shot.png"])
        XCTAssertEqual(
            nodeTranscript.attachments?.first?.origin, .relay(owner: "alice", repo: "site", sessionId: "s1", orgScope: false))

        try await center.continueRun(nodeRow)
        XCTAssertEqual(cloudApi.steered.last?.1, "Continue where you left off.")
    }

    func testRunnerRowsCarryTheirRunnerStatsAndMergeControls() async throws {
        var session = AgentLaunchSession(
            sessionId: "r1", owner: "alice", repo: "site", status: "completed", targetNode: "fmnext-n1",
            localAgentId: 5, branchName: "agent/3-fix", baseBranch: "main")
        session.runner = "fmnext-r1"
        session.branchStats = AgentBranchStats(files: 2, added: 30, removed: 4, ahead: 1, behind: 0, known: true)
        session.canMerge = true
        let row = UnifiedAgentRow.fromCloud(session)
        XCTAssertEqual(row.badgeLabel, "Runner \u{00B7} fmnext-r1")
        XCTAssertEqual(row.node, "fmnext-r1")
        XCTAssertTrue(row.isRunner)
        XCTAssertEqual(row.filesChanged, 2)
        XCTAssertEqual(row.linesAdded, 30)
        XCTAssertEqual(row.linesRemoved, 4)
        XCTAssertTrue(row.canContinue)
        XCTAssertTrue(row.offersMerge)
        XCTAssertFalse(row.offersCleanup)

        session.branchMerge = AgentBranchMerge(state: "merged", base: "main", branchDeleted: false)
        session.merged = true
        let merged = UnifiedAgentRow.fromCloud(session)
        XCTAssertTrue(merged.merged)
        XCTAssertFalse(merged.offersMerge)
        XCTAssertTrue(merged.offersCleanup)
        XCTAssertFalse(merged.canContinue)

        let plain = UnifiedAgentRow.fromCloud(
            AgentLaunchSession(sessionId: "m", status: "running", targetNode: "mirror35", localAgentId: 2))
        XCTAssertEqual(plain.badgeLabel, "Node")
        XCTAssertEqual(plain.filesChanged, -1)
        XCTAssertFalse(plain.canContinue, "a running run is steered, not continued")
    }

    @MainActor func testAFailedRunNoRunnerClaimedTakesAFollowUpAsARunOfItsTaskAgain() async throws {
        let cloudApi = FakeCloudAgents()
        var stopped = AgentLaunchSession(
            sessionId: "r2", owner: "alice", repo: "site", provider: "claude-code", status: "failed",
            targetNode: "fmnext-n1", history: [AgentLaunchMessage(role: "user", text: "Say why")])
        stopped.runner = "fmnext-r1"
        cloudApi.sessions = [stopped, cloud("s1", status: "failed", node: "mirror35", localId: 3, updated: 10)]
        let center = AgentControlCenter(hub: DesktopControlHub(local: nil, openRelay: nil, paired: { [] }), cloud: cloudApi)

        await center.refresh()
        let unclaimed = try XCTUnwrap(center.rows.first { $0.id == "node:r2" })
        XCTAssertTrue(unclaimed.canPrompt && unclaimed.restartsOnPrompt)
        XCTAssertFalse(unclaimed.canContinue)
        XCTAssertEqual(unclaimed.composerHint, "Follow-up (runs the task again)")
        try await center.prompt(unclaimed, "  and on the phone ")
        XCTAssertEqual(cloudApi.retried.first?.0, "r2")
        XCTAssertEqual(cloudApi.retried.first?.1, "Say why\n\nand on the phone")
        XCTAssertTrue(cloudApi.steered.isEmpty)
        XCTAssertEqual(center.promptNotice, "Queued again.")

        let claimed = try XCTUnwrap(center.rows.first { $0.id == "node:s1" })
        XCTAssertTrue(claimed.canPrompt)
        XCTAssertFalse(claimed.restartsOnPrompt)
        try await center.prompt(claimed, "go on")
        XCTAssertEqual(cloudApi.steered.last?.1, "go on")
        XCTAssertEqual(cloudApi.retried.count, 1)

        let queued = UnifiedAgentRow.fromCloud(
            AgentLaunchSession(sessionId: "q", status: "queued", targetNode: "fmnext-n1", title: "Wait"))
        XCTAssertFalse(queued.canPrompt || queued.restartsOnPrompt)
    }

    func testRunnerRowsOfferAPullRequestUntilOneIsLinked() async throws {
        var session = AgentLaunchSession(
            sessionId: "r1", owner: "alice", repo: "site", status: "completed", targetNode: "fmnext-n1",
            localAgentId: 5, branchName: "agent/3-fix", baseBranch: "main")
        session.runner = "fmnext-r1"
        session.canOpenPull = true
        let open = UnifiedAgentRow.fromCloud(session)
        XCTAssertTrue(open.canOpenPull)
        XCTAssertTrue(open.offersPullOpen)
        XCTAssertEqual(open.prNumber, 0)

        session.prNumber = 14
        let linked = UnifiedAgentRow.fromCloud(session)
        XCTAssertEqual(linked.prNumber, 14)
        XCTAssertFalse(linked.canOpenPull)
        XCTAssertFalse(linked.offersPullOpen)

        session.prNumber = 0
        session.status = "running"
        XCTAssertFalse(UnifiedAgentRow.fromCloud(session).offersPullOpen)
    }

    func testTheCloudTranscriptKeepsTheAgentsStructure() async throws {
        var session = AgentLaunchSession(sessionId: "t", status: "failed", lastError: "Out of credits")
        session.history = [
            AgentLaunchMessage(role: "user", author: "alice", text: "Fix it\n\nand add tests"),
            AgentLaunchMessage(role: "assistant", text: "## Done\n\n| a | b |\n|---|---|\n| 1 | 2 |"),
        ]
        let text = UnifiedAgentRow.cloudTranscript(session)
        XCTAssertEqual(text, "> Fix it\nand add tests\n\n## Done\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n!! Out of credits")
        var cache = TranscriptBlockCache()
        let blocks = cache.update(text + "\n")
        XCTAssertTrue(blocks.contains { if case .prompt = $0 { return true }; return false })
        XCTAssertTrue(blocks.contains { if case .table = $0 { return true }; return false })
        XCTAssertTrue(blocks.contains { if case .error = $0 { return true }; return false })
    }

    func testAnAgentInAnotherLoginIsListedWithANote() {
        let agent = LocalOutsideAgent.fromJSON([
            "id": -3005, "kind": "claude", "label": "Claude Code", "title": "Claude Code (b)", "otherUser": "b",
        ])
        XCTAssertEqual(agent.otherUser, "b")
        let row = UnifiedAgentRow.fromOutside(agent, transport: .lan, desktop: "desk")
        XCTAssertEqual(row.title, "Claude Code (b)")
        XCTAssertFalse(row.canPrompt || row.canStop)
        XCTAssertEqual(
            row.outsideSummary,
            "Claude Code is running in b's login on desk. ForkMesh cannot read or steer another login's agents; "
                + "open ForkMesh in that login to follow it.")
        XCTAssertTrue(row.outsideNote.hasSuffix("Open ForkMesh in b's login to watch and steer it there."))
        XCTAssertTrue(ControlErrorText.describe(code: "outside_other_user").hasPrefix("That agent runs in another login"))
        XCTAssertEqual(UnifiedAgentRow.fromOutside(LocalOutsideAgent.fromJSON(["id": -3006, "kind": "claude"]),
                                                   transport: .lan, desktop: "desk").outsideUser, "")
    }

    @MainActor func testAgentsStartedOutsideForkMeshListAndStreamTheirTranscripts() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        var gone = false
        desktop.refuse = { method, params in
            guard method == "agents.transcript" else { return nil }
            if gone { return "unknown_session" }
            return params.int("id") == -1001 ? "no_transcript" : nil
        }
        desktop.handler = { method, params in
            if method == "agents.transcript" && params.int("id") == -1002 {
                let text = "==> Claude Code started outside ForkMesh, in /home/alice/app\n\n> fix login\n"
                return [
                    "id": -1002, "transcript": .string(text), "from": 0, "next": .int(Int64(text.utf16.count)),
                    "reset": false, "active": true, "status": "running", "outside": true, "attachments": [],
                ]
            }
            guard method == "agents.list" else { return nil }
            return [
                "sessions": [
                    ["id": 7, "repositoryOwner": "alice", "repositoryName": "app", "status": "running"],
                    ["id": 8, "repositoryOwner": "alice", "repositoryName": "site", "status": "running"],
                    [
                        "id": 9, "repositoryOwner": "acme", "repositoryName": "tools", "status": "completed",
                        "finishedAtMs": .double(1_700_000_000_100),
                    ],
                    [
                        "id": 10, "repositoryOwner": "acme", "repositoryName": "tools", "status": "failed",
                        "finishedAtMs": .double(1_700_000_000_200),
                    ],
                ],
                "outside": [
                    [
                        "id": -1001, "kind": "codex", "label": "Codex", "provider": "codex", "model": "gpt-5",
                        "title": "scratch", "cwd": "/home/alice/scratch",
                    ],
                    [
                        "id": -1002, "kind": "claude", "label": "Claude Code", "title": "Fix login",
                        "cwd": "/home/alice/app", "repositoryOwner": "alice", "repositoryName": "app",
                        "lastActivityMs": .double(1_700_000_000_000),
                    ],
                    ["kind": "aider"],
                ],
            ]
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let center = AgentControlCenter(hub: hub, cloud: nil)

        await center.refresh()
        XCTAssertEqual(center.desktopSnapshot?.outside.map(\.id), [-1001, -1002])
        let codex = try XCTUnwrap(center.rows.first { $0.id == "outside:-1001" })
        XCTAssertTrue(codex.outside && codex.isActive && codex.isDesktop)
        XCTAssertFalse(codex.canStop || codex.canPrompt || codex.canContinue)
        XCTAssertEqual(codex.title, "scratch")
        XCTAssertEqual(codex.localId, -1001)
        XCTAssertEqual(center.rows.first { $0.id == "outside:-1002" }?.repoLabel, "alice/app")

        let live = desktopAgents(center.rows, shown: [(repo: "App", id: 7)])
        XCTAssertEqual(Set(live.prefix(3).map(\.id)), ["desktop:8", "outside:-1001", "outside:-1002"])
        XCTAssertEqual(live.dropFirst(3).map(\.id), ["desktop:10", "desktop:9"])
        XCTAssertEqual(desktopAgents(center.rows, shown: [], idle: 1).filter { !$0.isActive }.map(\.id), ["desktop:10"])

        let note = try await center.transcript(codex, since: 0)
        XCTAssertTrue(note.text.hasPrefix("Codex was started outside ForkMesh on desk, in /home/alice/scratch."))
        XCTAssertFalse(note.active)
        XCTAssertTrue(note.reset)

        let claude = try XCTUnwrap(center.rows.first { $0.id == "outside:-1002" })
        let live1 = try await center.transcript(claude, since: 0)
        XCTAssertTrue(live1.text.contains("> fix login"))
        XCTAssertTrue(live1.active)
        XCTAssertEqual(live1.status, "running")
        XCTAssertTrue(desktop.requests.contains {
            $0.str("method") == "agents.transcript" && $0.obj("params").int("id") == -1002
        })
        gone = true
        let ended = try await center.transcript(claude, since: live1.next)
        XCTAssertFalse(ended.reset)
        XCTAssertFalse(ended.active)
        XCTAssertEqual(ended.next, live1.next)
        do {
            try await center.stop(codex)
            XCTFail("an outside agent is not stopped from the phone")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "outside_stop_unsupported")
        }
        XCTAssertFalse(desktop.requests.contains { $0.str("method") == "agents.stop" })
    }

    @MainActor func testOutsideClaudeCodeIsSteeredThroughTheDesktop() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        desktop.refuse = { method, params in
            method == "agents.prompt" && params.str("text") == "gone" ? "not_steerable" : nil
        }
        desktop.handler = { method, params in
            if method == "agents.prompt" {
                return params.str("text") == "wait"
                    ? ["status": "held_for_approval", "cause": "no-mode-asserted"] : ["status": "delivered"]
            }
            guard method == "agents.list" else { return nil }
            return [
                "sessions": [],
                "outside": [
                    [
                        "id": -2001, "kind": "claude", "label": "Claude Code", "title": "Fix login",
                        "cwd": "/home/alice/app", "steer": true,
                    ],
                    ["id": -2002, "kind": "claude", "label": "Claude Code", "cwd": "/home/alice/old"],
                    ["id": -2003, "kind": "codex", "label": "Codex", "cwd": "/home/alice/scratch", "steer": true],
                ],
            ]
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let center = AgentControlCenter(hub: hub, cloud: nil)
        await center.refresh()

        XCTAssertEqual(center.desktopSnapshot?.outside.map(\.steer), [true, false, true])
        let steerable = try XCTUnwrap(center.rows.first { $0.id == "outside:-2001" })
        let terminalOnly = try XCTUnwrap(center.rows.first { $0.id == "outside:-2002" })
        let codex = try XCTUnwrap(center.rows.first { $0.id == "outside:-2003" })
        XCTAssertTrue(steerable.canPrompt)
        XCTAssertFalse(steerable.canStop || steerable.canContinue)
        XCTAssertFalse(terminalOnly.canPrompt)
        XCTAssertFalse(codex.canPrompt)
        XCTAssertEqual(
            steerable.outsideSummary,
            "Claude Code started outside ForkMesh on desk in app. You can watch and steer it here.")
        XCTAssertTrue(terminalOnly.outsideSummary.hasSuffix("steer it in the terminal that started it."))
        XCTAssertTrue(steerable.outsideNote.hasSuffix("you can still steer it here."))

        try await center.prompt(steerable, "  also run the tests ")
        let sent = try XCTUnwrap(desktop.requests.last { $0.str("method") == "agents.prompt" }).obj("params")
        XCTAssertEqual(sent.int("id"), -2001)
        XCTAssertEqual(sent.str("text"), "also run the tests")
        XCTAssertNil(sent["images"])
        XCTAssertEqual(center.promptNotice, "")
        try await center.prompt(steerable, "wait")
        XCTAssertTrue(center.promptNotice.hasPrefix("Sent. Claude Code is holding it for approval"))
        XCTAssertTrue(center.promptNotice.contains("crossSessionInbound"))

        let prompts = { desktop.requests.filter { $0.str("method") == "agents.prompt" }.count }
        let before = prompts()
        do {
            try await center.prompt(steerable, "look", images: [PromptAttachment(id: "a", name: "a.png", bytes: Data([1]), mediaType: "image/png")])
            XCTFail("photos do not reach an outside agent")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "outside_images_unsupported")
        }
        for row in [terminalOnly, codex] {
            do {
                try await center.prompt(row, "hello")
                XCTFail("\(row.id) is not steered from the phone")
            } catch let error as LocalControlException {
                XCTAssertEqual(error.code, "outside_cli_unsupported")
            }
        }
        XCTAssertEqual(prompts(), before)
        do {
            try await center.prompt(steerable, "gone")
            XCTFail("the desktop refused")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "not_steerable")
            XCTAssertTrue(ControlErrorText.describe(error).contains("terminal that started it"))
        }
        do {
            try await center.stop(steerable)
            XCTFail("an outside agent is not stopped from the phone")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "outside_stop_unsupported")
        }
        XCTAssertEqual([steerable, terminalOnly, codex].map(\.promptPath), [.peer, .none, .none])
        XCTAssertEqual(steerable.composerHint, "Message (arrives as another session)")
        XCTAssertEqual(center.desktopSnapshot?.outside.map(\.typeable), [false, false, false])
        XCTAssertEqual(center.desktopSnapshot?.outside.map(\.stoppable), [false, false, false])
    }

    @MainActor func testForkmeshTermAgentsAreTypedIntoAndInterrupted() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        var answer: JSONObject = ["status": "typed", "via": "terminal"]
        desktop.refuse = { method, params in
            guard method == "agents.prompt" else { return nil }
            switch params.str("text") {
            case "fast": return "typing_rate_limited"
            case "busy": return "terminal_user_typing"
            case "off": return "terminal_remote_off"
            case "closed": return "not_typeable"
            default: return nil
            }
        }
        desktop.handler = { method, params in
            if method == "agents.prompt" { return answer }
            if method == "agents.stop" {
                return ["status": "interrupted", "key": "esc"]
            }
            guard method == "agents.list" else { return nil }
            return [
                "sessions": [],
                "outside": [
                    [
                        "id": -3001, "kind": "claude", "label": "Claude Code", "cwd": "/home/alice/app",
                        "steer": true, "typeable": true, "stoppable": true,
                    ],
                    [
                        "id": -3002, "kind": "codex", "label": "Codex", "cwd": "/home/alice/scratch",
                        "typeable": true, "stoppable": true,
                    ],
                    ["id": -3003, "kind": "claude", "label": "Claude Code", "steer": true, "stoppable": false],
                    ["id": -3004, "kind": "aider", "label": "Aider", "stoppable": true],
                ],
            ]
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let center = AgentControlCenter(hub: hub, cloud: nil)
        await center.refresh()

        XCTAssertEqual(center.desktopSnapshot?.outside.map(\.typeable), [true, true, false, false])
        XCTAssertEqual(center.desktopSnapshot?.outside.map(\.stoppable), [true, true, false, true])
        let claude = try XCTUnwrap(center.rows.first { $0.id == "outside:-3001" })
        let codex = try XCTUnwrap(center.rows.first { $0.id == "outside:-3002" })
        let peer = try XCTUnwrap(center.rows.first { $0.id == "outside:-3003" })
        let aider = try XCTUnwrap(center.rows.first { $0.id == "outside:-3004" })
        XCTAssertEqual([claude, codex, peer, aider].map(\.promptPath), [.terminal, .terminal, .peer, .none])
        XCTAssertEqual([claude, codex, peer, aider].map(\.canPrompt), [true, true, true, false])
        XCTAssertEqual([claude, codex, peer, aider].map(\.canStop), [true, true, false, true])
        XCTAssertFalse(claude.canContinue || codex.canContinue)
        XCTAssertEqual(claude.composerHint, "Types into your terminal")
        XCTAssertEqual(peer.composerHint, "Message (arrives as another session)")
        XCTAssertEqual([claude, codex, aider].map(\.interruptKeyLabel), ["Esc", "Esc", "its interrupt key"])
        XCTAssertEqual(
            claude.outsideSummary,
            "Claude Code started outside ForkMesh on desk in app. What you send here is typed into its terminal.")
        XCTAssertEqual(
            codex.outsideSummary,
            "Codex started outside ForkMesh on desk in scratch. What you send here is typed into its terminal.")
        XCTAssertEqual(peer.outsideSummary, "Claude Code started outside ForkMesh on desk. You can watch and steer it here.")
        XCTAssertTrue(codex.outsideNote.hasSuffix("what you send here is typed into its terminal."))

        try await center.prompt(codex, "  run the tests ")
        let sent = try XCTUnwrap(desktop.requests.last { $0.str("method") == "agents.prompt" }).obj("params")
        XCTAssertEqual(sent.int("id"), -3002)
        XCTAssertEqual(sent.str("text"), "run the tests")
        XCTAssertEqual(center.promptNotice, "")

        answer = ["status": "held_for_approval", "via": "peer"]
        try await center.prompt(peer, "hello")
        XCTAssertTrue(center.promptNotice.hasPrefix("Sent. Claude Code is holding it for approval"))
        answer = ["status": "typed", "via": "terminal"]
        try await center.prompt(claude, "hi")
        XCTAssertEqual(center.promptNotice, "")

        do {
            try await center.prompt(claude, "look", images: [PromptAttachment(id: "a", name: "a.png", bytes: Data([1]), mediaType: "image/png")])
            XCTFail("photos do not reach an outside agent")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "outside_images_unsupported")
        }
        let expected = [
            "fast": ("typing_rate_limited", "Too many prompts from this phone in the last minute. Wait a moment and try again."),
            "busy": ("terminal_user_typing", "Someone is typing in that terminal right now. Try again in a moment."),
            "off": (
                "terminal_remote_off",
                "Phone typing is turned off in that terminal. Press Ctrl-^ there (or restart it without --no-remote) to turn it back on."
            ),
            "closed": ("not_typeable", "That terminal can no longer be typed into. It may have closed."),
        ]
        for (text, (code, sentence)) in expected {
            do {
                try await center.prompt(claude, text)
                XCTFail("the desktop refused \(text)")
            } catch let error as LocalControlException {
                XCTAssertEqual(error.code, code)
                XCTAssertEqual(ControlErrorText.describe(error), sentence)
            }
        }

        try await center.stop(claude)
        try await center.stop(codex)
        let stops = desktop.requests.filter { $0.str("method") == "agents.stop" }.map { $0.obj("params").int("id") }
        XCTAssertEqual(stops, [-3001, -3002])
        do {
            try await center.stop(peer)
            XCTFail("the peer path cannot interrupt")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "outside_stop_unsupported")
        }
        XCTAssertEqual(desktop.requests.filter { $0.str("method") == "agents.stop" }.count, 2)
    }

    func testOutsideRowDecodingDefaultsToNoTypingOrStop() {
        let old = LocalOutsideAgent.fromJSON(["id": -1, "kind": "claude", "steer": true])
        XCTAssertFalse(old.typeable || old.stoppable)
        let term = LocalOutsideAgent.fromJSON(["id": -2, "kind": "gemini", "typeable": true, "stoppable": true])
        XCTAssertTrue(term.typeable && term.stoppable)
        XCTAssertEqual(UnifiedAgentRow.outsidePromptPath(old), .peer)
        XCTAssertEqual(UnifiedAgentRow.outsidePromptPath(term), .terminal)
        XCTAssertEqual(UnifiedAgentRow.outsidePromptPath(LocalOutsideAgent(id: -3, kind: "codex", label: "Codex", steer: true)), .none)
        let row = UnifiedAgentRow.fromOutside(term, transport: .lan, desktop: "desk")
        XCTAssertTrue(row.canPrompt && row.canStop && row.outside)
        XCTAssertEqual(row.outsideKind, "gemini")
        let managed = UnifiedAgentRow.fromDesktop(desktopRow(1, status: "running", stoppable: true), transport: .lan, desktop: "desk")
        XCTAssertEqual(managed.promptPath, .none)
        XCTAssertEqual(managed.composerHint, "Follow-up for the agent")
    }

    func testATerminalStartedFromThePhoneIsAnOutsideRowWithItsTerminal() {
        let agent = LocalOutsideAgent.fromJSON([
            "id": -7, "kind": "claude", "label": "Claude Code", "title": "repo", "cwd": "/home/a/repo",
            "typeable": true, "stoppable": true, "terminal": "0123456789abcdef0123456789abcdef",
        ])
        XCTAssertEqual(agent.terminal, "0123456789abcdef0123456789abcdef")
        let row = UnifiedAgentRow.fromOutside(agent, transport: .relay, desktop: "desk")
        XCTAssertEqual(row.terminal, agent.terminal)
        XCTAssertTrue(row.isPhoneTerminal)
        XCTAssertTrue(row.canPrompt && row.canStop)
        XCTAssertEqual(row.promptPath, .terminal)
        XCTAssertEqual(
            row.outsideSummary,
            "Claude Code started from your phone in a terminal on desk in repo. Open the terminal for full control; "
                + "what you send here is typed into it.")
        XCTAssertTrue(row.outsideNote.hasPrefix("Claude Code was started from your phone in a terminal on desk, in /home/a/repo."))

        let plain = UnifiedAgentRow.fromOutside(LocalOutsideAgent.fromJSON(["id": -8, "kind": "claude", "typeable": true]),
                                                transport: .lan, desktop: "desk")
        XCTAssertEqual(plain.terminal, "")
        XCTAssertFalse(plain.isPhoneTerminal)
        XCTAssertTrue(plain.outsideSummary.contains("started outside ForkMesh"))
        var managed = UnifiedAgentRow.fromDesktop(desktopRow(1, status: "running", stoppable: true), transport: .lan, desktop: "desk")
        managed.terminal = "abc"
        XCTAssertFalse(managed.isPhoneTerminal, "only outside rows are phone terminals")
    }

    func testTerminalErrorsReadAsSentences() {
        for code in [
            "unknown_terminal", "unknown_repository", "unsupported_provider", "cli_missing", "guardrails_on",
            "terminal_vm", "terminal_limit", "terminal_failed", "bad_input",
        ] {
            let text = ControlErrorText.describe(code: code)
            XCTAssertNotEqual(text, code.replacingOccurrences(of: "_", with: " "), code)
            XCTAssertTrue(text.hasSuffix("."), code)
        }
        XCTAssertTrue(ControlErrorText.describe(code: "cli_missing").contains("not installed"))
        XCTAssertTrue(ControlErrorText.describe(code: "terminal_limit").hasPrefix("Six terminals"))
    }

    func testTerminalObjectsParse() {
        let info = DesktopTerminalInfo(json: [
            "terminal": "ab12", "provider": "codex", "label": "Codex", "cli": "/usr/bin/codex",
            "repositoryOwner": "alice", "repositoryName": "repo", "cwd": "/home/a/repo", "startedAtMs": 1700,
            "device": "Alice's iPhone", "running": true, "cols": 90, "rows": 30,
        ])
        XCTAssertEqual(info.terminal, "ab12")
        XCTAssertEqual(info.provider, "codex")
        XCTAssertEqual(info.label, "Codex")
        XCTAssertEqual(info.cli, "/usr/bin/codex")
        XCTAssertEqual(info.repoLabel, "alice/repo")
        XCTAssertEqual(info.cwd, "/home/a/repo")
        XCTAssertEqual(info.startedAtMs, 1700)
        XCTAssertEqual(info.device, "Alice's iPhone")
        XCTAssertTrue(info.running)
        XCTAssertEqual(info.cols, 90)
        XCTAssertEqual(info.rows, 30)
        XCTAssertFalse(DesktopTerminalInfo(json: ["terminal": "x", "running": false]).running)
        let listed = DesktopTerminalInfo.list(["terminals": [["terminal": "a"], ["terminal": ""], ["terminal": "b"]]])
        XCTAssertEqual(listed.map(\.terminal), ["a", "b"])
        XCTAssertTrue(DesktopTerminalSupport.supported(status: ["capabilities": ["agents", "terminal.agents"]]))
        XCTAssertFalse(DesktopTerminalSupport.supported(status: ["capabilities": ["agents"]]))
        XCTAssertFalse(DesktopTerminalSupport.supported(status: [:]))
    }

    @MainActor func testDesktopPicturesComeThroughTheRelayAsPreviews() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        let wanted = String(repeating: "d", count: 32)
        let preview = Data([0xFF, 0xD8, 0xFF, 0xE0] + [UInt8](repeating: 3, count: LocalAgentTranscript.maxPreviewBytes - 4))
        desktop.handler = { method, params in
            guard method == "agents.attachment" else { return nil }
            if params.str("attachment") != wanted { return nil }
            return [
                "name": "shot.png", "mediaType": "image/jpeg", "size": .int(Int64(preview.count)),
                "data": .string(preview.base64EncodedString()), "preview": .bool(params["preview"] == .bool(true)),
                "originalSize": 9_000_000, "width": 1600, "height": 1000,
            ]
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        XCTAssertTrue(hub.prefersAttachmentPreviews)
        let loader = hub.attachmentLoader(store: AgentAttachmentStore(), relay: { _ in throw AgentAttachmentError("unused") })
        let ref = AgentAttachmentRef(
            meta: AgentAttachmentMeta(id: wanted, name: "shot.png", mediaType: "image/png"), origin: .desktop(sessionId: 7))
        let payload = try await loader.loadPayload(ref)
        XCTAssertTrue(payload.preview)
        XCTAssertEqual(payload.data, preview)
        XCTAssertEqual(payload.originalSize, 9_000_000)
        let asked = desktop.requests.last { $0.str("method") == "agents.attachment" }
        XCTAssertEqual(asked?.obj("params")["preview"], .bool(true))
        XCTAssertEqual(asked?.obj("params").int("id"), 7)
        XCTAssertEqual(hub.transport, .relay)

        let elsewhere = AgentAttachmentRef(
            meta: AgentAttachmentMeta(id: wanted, name: "x.png", mediaType: "image/png"), origin: .elsewhere)
        do {
            _ = try await hub.attachmentPayload(elsewhere, preview: true)
            XCTFail("only desktop attachments go to the desktop")
        } catch let error as AgentAttachmentError {
            XCTAssertEqual(error.message, "This attachment is not on the desktop.")
        }
    }

    @MainActor func testTheDesktopsQueueIsReadReorderedAndResized() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        var order: [Int] = [5, 6, 7]
        var limit = 2
        func queueJSON() -> JSONObject {
            ["running": .int(Int64(limit)), "limit": .int(Int64(limit)), "limitMax": 99,
             "queue": .array(order.map { .int(Int64($0)) })]
        }
        desktop.handler = { method, params in
            switch method {
            case "agents.list":
                return [
                    "sessions": .array([5, 6, 7, 8].map { id in
                        .object([
                            "id": .int(Int64(id)), "repositoryOwner": "alice", "repositoryName": "app",
                            "status": .string(id == 8 ? "running" : "queued"), "prompt": .string("Task \(id)"),
                            "stoppable": true,
                        ])
                    }),
                    "running": 1,
                    "queue": .object(queueJSON()),
                ]
            case "agents.queue.move":
                let id = params.int("id")
                order.removeAll { $0 == id }
                order.insert(id, at: min(params.int("to"), order.count))
                return queueJSON()
            case "agents.queue.limit":
                limit = params.int("limit")
                return queueJSON()
            default:
                return nil
            }
        }
        desktop.refuse = { method, params in
            method == "agents.queue.move" && params.int("id") == 8 ? "not_queued" : nil
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let center = AgentControlCenter(hub: hub, cloud: nil)

        await center.refresh()
        XCTAssertTrue(center.supportsQueue)
        XCTAssertEqual(center.queue?.summary, "2 of 2 running · 3 waiting")
        XCTAssertEqual(center.queuedRows.map(\.localId), [5, 6, 7])
        let last = try XCTUnwrap(center.rows.first { $0.localId == 7 })
        XCTAssertEqual(center.queuePosition(last), 2)

        try await center.moveQueued(last, to: 0)
        XCTAssertTrue(desktop.requests.contains {
            $0.str("method") == "agents.queue.move" && $0.obj("params").int("id") == 7 && $0.obj("params").int("to") == 0
        })
        XCTAssertEqual(center.queue?.order, [7, 5, 6])
        XCTAssertEqual(center.queuePosition(last), 0)

        try await center.setQueueLimit(4)
        XCTAssertTrue(desktop.requests.contains {
            $0.str("method") == "agents.queue.limit" && $0.obj("params").int("limit") == 4
        })
        XCTAssertEqual(center.queue?.limit, 4)

        let running = try XCTUnwrap(center.rows.first { $0.localId == 8 })
        XCTAssertNil(center.queuePosition(running))
        do {
            try await center.moveQueued(running, to: 0)
            XCTFail("a running agent is not in the queue")
        } catch let error as LocalControlException {
            XCTAssertEqual(error.code, "not_queued")
        }
        XCTAssertEqual(center.queue?.order, [7, 5, 6])
    }

    func testTheDesktopsUnreadFlagIsReadWhenSent() {
        let flagged = LocalAgentRow.fromJSON([
            "id": 7, "repositoryOwner": "alice", "repositoryName": "app", "status": "done", "finishedAtMs": 900,
            "unread": true,
        ])
        XCTAssertEqual(flagged.unread, true)
        let row = UnifiedAgentRow.fromDesktop(flagged, transport: .lan, desktop: "desk")
        XCTAssertEqual(row.unread, true)
        XCTAssertEqual(row.finishedAtMs, 900)
        XCTAssertEqual(LocalAgentRow.fromJSON(["id": 7, "unread": false]).unread, false)
        XCTAssertNil(LocalAgentRow.fromJSON(["id": 7, "status": "done"]).unread, "an older desktop sends no flag")
        XCTAssertNil(UnifiedAgentRow.fromDesktop(desktopRow(8, status: "done"), transport: .lan, desktop: "desk").unread)
        let snapshot = LocalAgentsSnapshot.fromJSON(["sessions": [["id": 3, "unread": true], ["id": 4]]])
        XCTAssertEqual(snapshot.sessions.map(\.unread), [true, nil])
    }

    @MainActor func testOpeningADesktopAgentMarksItReadThere() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        desktop.handler = { method, params in
            switch method {
            case "agents.list":
                return ["sessions": [["id": 3, "status": "done", "prompt": "Fix", "unread": true]], "running": 0]
            case "agents.markRead":
                return params.int("id") == 3 ? [:] : nil
            default:
                return nil
            }
        }
        desktop.refuse = { method, params in
            method == "agents.markRead" && params.int("id") != 3 ? "unknown_session" : nil
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let center = AgentControlCenter(hub: hub, cloud: nil)
        await center.refresh()
        XCTAssertEqual(center.rows.first?.unread, true)

        await center.markRead(id: 3)
        XCTAssertTrue(desktop.requests.contains {
            $0.str("method") == "agents.markRead" && $0.obj("params").int("id") == 3
        })
        await center.markRead(id: 99)
        XCTAssertTrue(desktop.requests.contains {
            $0.str("method") == "agents.markRead" && $0.obj("params").int("id") == 99
        }, "a refusal is ignored")
        let sent = desktop.requests.count
        await center.markRead(id: 0)
        XCTAssertEqual(desktop.requests.count, sent)

        let alone = AgentControlCenter(hub: DesktopControlHub(local: nil, openRelay: nil, paired: { [] }), cloud: nil)
        await alone.markRead(id: 3)
    }

    @MainActor func testAnOlderDesktopHasNoQueue() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        desktop.handler = { method, _ in
            method == "agents.list" ? ["sessions": [["id": 3, "status": "queued", "prompt": "Wait"]], "running": 0] : nil
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let center = AgentControlCenter(hub: hub, cloud: nil)
        await center.refresh()
        XCTAssertEqual(center.rows.map(\.id), ["desktop:3"])
        XCTAssertFalse(center.supportsQueue)
        XCTAssertNil(center.queuePosition(center.rows[0]))
        XCTAssertEqual(center.queuedRows, [])
    }

    @MainActor func testDesktopRowsShowBeforeTheCloudAnswers() async throws {
        let desktop = try FakeRelayDesktop()
        let identity = try Identity.fromSeed(Ed25519.generateSeed())
        desktop.handler = { method, _ in
            method == "agents.list" ? ["sessions": [["id": 3, "status": "running", "prompt": "Go"]]] : nil
        }
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { target in
                try await RelayControlConnection.open(channel: desktop, identity: identity, desktopKey: target.key, node: "desk")
            },
            paired: { [PairedDesktop(key: desktop.publicKey, name: "desk", pairedAtMs: 1)] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [cloud("s9", status: "queued", node: "mirror37")]
        let gate = CloudGate()
        cloudApi.hold = { await gate.wait() }
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        let refresh = Task { await center.refresh() }
        for _ in 0..<300 where center.rows.isEmpty {
            try await Task.sleep(nanoseconds: 10_000_000)
        }
        XCTAssertEqual(center.rows.map(\.id), ["desktop:3"])
        gate.open()
        await refresh.value
        XCTAssertEqual(Set(center.rows.map(\.id)), ["desktop:3", "node:s9"])
    }

    @MainActor func testAMissingDesktopStillListsNodeRuns() async {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [cloud("s9", status: "queued", node: "mirror37")]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        XCTAssertEqual(center.rows.map(\.id), ["node:s9"])
        XCTAssertEqual(center.desktopNote, "")
    }

    @MainActor func testStopAllStopsEveryRunningRunAndNothingElse() async {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [
            cloud("running", status: "running", node: "mirror37"),
            cloud("queued", status: "queued", node: "mirror37"),
            cloud("done", status: "completed", node: "mirror37"),
        ]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        XCTAssertEqual(Set(AgentStopAll.targets(center.rows).map(\.cloudSessionId)), ["running", "queued"])
        let outcome = await center.stopAll()
        XCTAssertEqual(Set(cloudApi.stopped), ["running", "queued"])
        XCTAssertEqual(outcome, AgentStopAllOutcome(stopped: 2, failed: 0))
        XCTAssertEqual(outcome.message, "Asked 2 agents to stop.")
    }

    func testStopAllSkipsOutsideAgentsAndSaysWhatFailed() {
        var outside = UnifiedAgentRow.fromDesktop(desktopRow(7, status: "running", stoppable: true), transport: .lan, desktop: "desk")
        outside.outside = true
        let inside = UnifiedAgentRow.fromDesktop(desktopRow(8, status: "running", stoppable: true), transport: .lan, desktop: "desk")
        XCTAssertEqual(AgentStopAll.targets([outside, inside]).map(\.id), ["desktop:8"])
        XCTAssertEqual(AgentStopAll.question(1), "Stop the running agent?")
        XCTAssertEqual(AgentStopAll.question(3), "Stop all 3 running agents?")
        XCTAssertEqual(AgentStopAllOutcome(stopped: 0, failed: 0).message, "No agents are running.")
        XCTAssertEqual(AgentStopAllOutcome(stopped: 1, failed: 0).message, "Asked 1 agent to stop.")
        XCTAssertEqual(
            AgentStopAllOutcome(stopped: 0, failed: 2, error: "The desktop is not connected.").message,
            "Could not stop 2 agents. The desktop is not connected.")
        XCTAssertEqual(
            AgentStopAllOutcome(stopped: 2, failed: 1, error: "The desktop is not connected.").message,
            "Asked 2 agents to stop; 1 did not. The desktop is not connected.")
    }

    @MainActor func testAFailedOrStoppedNodeRunOffersItsRunForARetry() async {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [
            cloud("failed", status: "failed", node: "mirror37"),
            cloud("stopped", status: "stopped", node: "mirror37", localId: 4),
            cloud("running", status: "running", node: "mirror37"),
            cloud("done", status: "completed", node: "mirror37"),
        ]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        let row = { (id: String) in center.rows.first { $0.cloudSessionId == id }! }
        XCTAssertEqual(center.retryableRun(row("failed"))?.sessionId, "failed")
        XCTAssertEqual(center.retryableRun(row("stopped"))?.sessionId, "stopped")
        XCTAssertNil(center.retryableRun(row("running")))
        XCTAssertNil(center.retryableRun(row("done")))
        let desktop = UnifiedAgentRow.fromDesktop(desktopRow(7, status: "failed"), transport: .lan, desktop: "desk")
        XCTAssertNil(center.retryableRun(desktop))
    }

    @MainActor func testFinishedRunsBeyondTheListedWindowStayAndDeletedOnesGo() async {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [
            cloud("live", status: "running", node: "mirror37", updated: 9_000),
            cloud("recent", status: "completed", node: "mirror37", updated: 8_000),
            cloud("old", status: "completed", node: "mirror37", updated: 3_000),
            cloud("gone", status: "failed", node: "mirror37", updated: 8_500),
            cloud("stale", status: "running", node: "mirror37", updated: 2_000),
        ]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        XCTAssertEqual(center.rows.count, 5)

        cloudApi.sessions = [
            cloud("live", status: "running", node: "mirror37", updated: 9_500),
            cloud("recent", status: "completed", node: "mirror37", updated: 8_000),
        ]
        cloudApi.finishedBefore = 8_000
        await center.refresh()
        XCTAssertEqual(center.rows.map(\.id), ["node:live", "node:recent", "node:old"])

        cloudApi.finishedBefore = 0
        await center.refresh()
        XCTAssertEqual(center.rows.map(\.id), ["node:live", "node:recent"])
    }

    @MainActor func testWatchReadsOfANodeRunAreSharedUntilTheRowMoves() async throws {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [cloud("s1", status: "running", node: "fmnext-n1", updated: 1_000)]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        let row = try XCTUnwrap(center.rows.first)

        async let first = center.watchTranscript(row, now: 100)
        async let second = center.watchTranscript(row, now: 100)
        let (a, b) = try await (first, second)
        XCTAssertEqual(a, b)
        XCTAssertTrue(a.text.contains("Fixed."))
        XCTAssertEqual(cloudApi.sessionReads, 1)

        _ = try await center.watchTranscript(row, now: 120)
        XCTAssertEqual(cloudApi.sessionReads, 1)

        _ = try await center.watchTranscript(row, now: 100 + AgentControlCenter.cloudReadReuse)
        XCTAssertEqual(cloudApi.sessionReads, 2)

        cloudApi.sessions = [cloud("s1", status: "running", node: "fmnext-n1", updated: 2_000)]
        await center.refresh()
        _ = try await center.watchTranscript(row, now: 131)
        XCTAssertEqual(cloudApi.sessionReads, 3)

        _ = try await center.transcript(row, since: 0)
        XCTAssertEqual(cloudApi.sessionReads, 4)
    }

    @MainActor func testAFailedWatchReadOfANodeRunIsNotKept() async throws {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [cloud("s1", status: "running", node: "fmnext-n1", updated: 1_000)]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        let row = try XCTUnwrap(center.rows.first)
        let missing = cloudApi.sessions
        cloudApi.sessions = []

        do {
            _ = try await center.watchTranscript(row, now: 10)
            XCTFail("expected the read to fail")
        } catch {}
        cloudApi.sessions = missing
        _ = try await center.watchTranscript(row, now: 11)
        XCTAssertEqual(cloudApi.sessionReads, 2)
    }

    @MainActor func testAFailedCloudReadKeepsTheRowsItHad() async {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [cloud("s9", status: "completed", node: "mirror37", updated: 5_000)]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        XCTAssertEqual(center.rows.map(\.id), ["node:s9"])

        cloudApi.failure = AgentLaunchException("The relay is busy.", status: 503)
        await center.refresh()
        XCTAssertEqual(center.rows.map(\.id), ["node:s9"])
        XCTAssertEqual(center.cloudNote, "The relay is busy.")

        cloudApi.failure = nil
        cloudApi.sessions = []
        await center.refresh()
        XCTAssertTrue(center.rows.isEmpty)
        XCTAssertEqual(center.cloudNote, "")
    }

    @MainActor func testACancelledCloudReadShowsNoNote() async {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [cloud("s9", status: "completed", node: "mirror37", updated: 5_000)]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()

        for failure: Error in [CancellationError(), HTTPError.cancelled] {
            cloudApi.failure = failure
            await center.refresh()
            XCTAssertEqual(center.rows.map(\.id), ["node:s9"])
            XCTAssertEqual(center.cloudNote, "")
        }
    }

    @MainActor func testACancelledDesktopDialShowsNoNote() async {
        let hub = DesktopControlHub(
            local: nil,
            openRelay: { _ in throw CancellationError() },
            paired: { [PairedDesktop(key: "k", name: "desk", pairedAtMs: 1)] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [cloud("s9", status: "queued", node: "mirror37")]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        XCTAssertEqual(center.rows.map(\.id), ["node:s9"])
        XCTAssertEqual(center.desktopNote, "")
        XCTAssertEqual(hub.reach, .offline(""))
    }

    @MainActor func testANodeRunFollowsItsLiveStreamUntilTheRunEnds() async throws {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [cloud("s1", status: "running", node: "fmnext-n1", updated: 1_000)]
        cloudApi.liveStatus = 200
        cloudApi.liveBody = "retry: 1000\n\n: keepalive\n\nevent: live\ndata: {\"at\":1700000005000,\"text\":\"Working on it\"}\n\n"
            + "event: live\ndata: {\"append\":\"\\n\\nAlmost there\"}\n\nevent: end\ndata: {\"status\":\"completed\"}\n\n"
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        let row = try XCTUnwrap(center.rows.first)
        let before = try await center.transcript(row, since: 0)
        XCTAssertEqual(before.text, "> Fix it\n\nFixed.")

        var shown: [AgentTranscriptDelta] = []
        let outcome = await center.followCloudLive(row) { shown.append($0) }
        XCTAssertEqual(outcome, .ended("completed"))
        XCTAssertEqual(cloudApi.liveOpened, ["alice/site/s1"])
        XCTAssertEqual(shown.map(\.text), [
            "> Fix it\n\nFixed.\n\nWorking on it",
            "> Fix it\n\nFixed.\n\nWorking on it\n\nAlmost there",
        ])
        XCTAssertTrue(shown.allSatisfy { $0.reset && $0.active && $0.next == $0.text.utf16.count })

        let after = try await center.transcript(row, since: 0)
        XCTAssertEqual(after.text, "> Fix it\n\nFixed.")
    }

    @MainActor func testOnlyNodeRunsOnARelayThatServesTheStreamAreFollowed() async throws {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [cloud("s1", status: "running", node: "fmnext-n1", updated: 1_000)]
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        let row = try XCTUnwrap(center.rows.first)
        let refused = await center.followCloudLive(row) { _ in XCTFail("no frames") }
        XCTAssertEqual(refused, .refused(404))

        let desktop = UnifiedAgentRow.fromDesktop(desktopRow(7, status: "running"), transport: .lan, desktop: "desk")
        let skipped = await center.followCloudLive(desktop) { _ in XCTFail("no frames") }
        XCTAssertEqual(skipped, .refused(0))
        XCTAssertEqual(cloudApi.liveOpened, ["alice/site/s1"])
    }

    func testAgentQuestionKeepsOptionsAndSelectionMode() throws {
        let question = try XCTUnwrap(AgentQuestion([
            "id": "ask-1",
            "input": ["questions": [[
                "question": "Which target?",
                "multiSelect": true,
                "isSecret": true,
                "options": [["label": "iPhone", "description": "Use the phone build"]],
            ]]],
        ]))
        XCTAssertEqual(question.id, "ask-1")
        XCTAssertEqual(question.fields.first?.title, "Which target?")
        XCTAssertTrue(question.fields.first?.multiSelect == true)
        XCTAssertTrue(question.fields.first?.isSecret == true)
        XCTAssertEqual(question.fields.first?.options.first?.detail, "Use the phone build")
    }
}
