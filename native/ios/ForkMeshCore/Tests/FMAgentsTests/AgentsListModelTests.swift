// The logic of the Agents screen, the start card and retry: the reads, rows and decisions.

import FMAccount
import FMApi
import FMBase
import FMCode
import FMLocalAI
import FMNet
import Observation
import XCTest
@testable import FMAgents

@MainActor
final class FakeAgentsApi: AgentsApi {
    var repos: [Repository] = [
        Repository(owner: "alice", name: "mobile", cloneOnline: true),
        Repository(owner: "bob", name: "worker", liveHost: true),
    ]
    var repositoriesCalls = 0
    var sessionCalls: [String: Int] = [:]
    var pages: [String: (Int) -> AgentSessionPage] = [:]
    var options: [String: AgentLaunchOptions] = [:]
    var optionsError: Error?
    var repositoriesError: Error?
    var pageError: Error?
    var ledger: [PublishedPull] = []
    var pullCalls = 0
    var launches: [(owner: String, repo: String, prompt: String, node: String, provider: String, issue: Int)] = []
    var steers: [(sessionId: String, prompt: String)] = []
    var cancels: [String] = []
    var queueResult = AgentQueueResult(queueState: "ready_for_claim", targetOnline: true, message: "Queued on desk-1.")
    var repositoriesGate: CheckedContinuation<Void, Never>?
    var holdRepositories = false

    func repositories() async throws -> [Repository] {
        repositoriesCalls += 1
        if holdRepositories {
            await withCheckedContinuation { repositoriesGate = $0 }
        }
        if let repositoriesError { throw repositoriesError }
        return repos
    }

    func agentSessionPage(_ owner: String, _ name: String, ownerAccount: String) async throws -> AgentSessionPage {
        let key = "\(owner)/\(name)"
        sessionCalls[key, default: 0] += 1
        if let pageError { throw pageError }
        return pages[key]?(sessionCalls[key] ?? 1) ?? AgentSessionPage()
    }

    func agentTranscript(_ owner: String, _ name: String, _ id: Int, ownerAccount: String) async throws -> AgentTranscript {
        AgentTranscript(status: id == 7 ? "done" : "running", transcript: "Session \(id) transcript for \(owner)/\(name)")
    }

    func agentLaunchOptions(_ owner: String, _ repo: String) async throws -> AgentLaunchOptions {
        if let optionsError { throw optionsError }
        return options["\(owner)/\(repo)"] ?? AgentLaunchOptions()
    }

    func publishedPulls(_ owner: String, _ name: String) async throws -> [PublishedPull] {
        pullCalls += 1
        return ledger
    }

    func launchAgentSession(
        _ owner: String, _ repo: String, prompt: String, provider: String, model: String,
        targetNode: String, title: String, createPr: Bool, issueNumber: Int, orgScope: Bool
    ) async throws -> AgentQueueResult {
        launches.append((owner, repo, prompt, targetNode, provider, issueNumber))
        return queueResult
    }

    func steerAgentSession(_ owner: String, _ repo: String, _ sessionId: String, prompt: String, orgScope: Bool) async throws -> AgentQueueResult {
        steers.append((sessionId, prompt))
        return AgentQueueResult(message: "Follow-up sent.")
    }

    func retryAgentSession(_ owner: String, _ repo: String, _ session: AgentLaunchSession) async throws -> AgentRetryResult {
        // The same decision ApiService makes (tested in FMApi).
        if session.canResume {
            let result = try await steerAgentSession(owner, repo, session.sessionId, prompt: session.retryPrompt, orgScope: session.orgScope)
            return AgentRetryResult(result: result, resumed: true)
        }
        let result = try await launchAgentSession(
            owner, repo, prompt: session.retryPrompt, provider: session.provider, model: session.requestedModel,
            targetNode: session.targetNode, title: session.title, createPr: session.createPr,
            issueNumber: session.issueNumber, orgScope: session.orgScope)
        return AgentRetryResult(result: result, resumed: false)
    }

    func cancelAgentSession(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws {
        cancels.append(sessionId)
    }

    var liveResponses: [(status: Int, body: String)] = []
    var liveOpened: [String] = []

    func agentLiveStream(_ owner: String, _ repo: String, _ sessionId: String, orgScope: Bool) async throws
        -> HTTPStreamedResponse
    {
        liveOpened.append("\(owner)/\(repo)/\(sessionId)")
        let next = liveResponses.isEmpty ? (status: 404, body: "") : liveResponses.removeFirst()
        let body = AsyncThrowingStream<Data, Error> { continuation in
            if !next.body.isEmpty { continuation.yield(Data(next.body.utf8)) }
            continuation.finish()
        }
        return HTTPStreamedResponse(status: next.status, body: body)
    }
}

@MainActor
final class AgentsListModelTests: XCTestCase {
    private func globalApi() -> FakeAgentsApi {
        let api = FakeAgentsApi()
        api.pages["alice/mobile"] = { call in
            AgentSessionPage(sessions: [
                AgentSession(
                    id: 7, issueNumber: 12, issueTitle: "Stabilize signed inbox",
                    status: call == 1 ? "running" : "done", provider: "codex",
                    branchName: "agent/signed-inbox", numTurns: call, createPr: true, prNumber: 7,
                    baseBranch: "main", totalTokens: 1500, filesChanged: 3, ahead: 2, behind: 1, conflicted: true
                ),
            ])
        }
        api.pages["bob/worker"] = { _ in
            AgentSessionPage(sessions: [AgentSession(id: 8, issueTitle: "Publish Worker relay tests", status: "done", provider: "claude-code")])
        }
        return api
    }

    func testTheListReadsEveryRepositoryAndFlattensRunningFirst() async {
        let api = globalApi()
        let model = AgentsListModel(api: api)
        await model.refresh()

        XCTAssertEqual(api.repositoriesCalls, 1)
        XCTAssertEqual(api.sessionCalls, ["alice/mobile": 1, "bob/worker": 1])
        XCTAssertEqual(model.runningCount, 1)
        XCTAssertEqual(model.sessionCount, 2)
        XCTAssertEqual(model.desktopRunningCount, 1)
        XCTAssertEqual(model.listState, "list-2")
        XCTAssertEqual(model.entries.map(\.id), ["desktop:alice/mobile:7", "desktop:bob/worker:8"])
        XCTAssertEqual(model.desktopRunning.map(\.session.id), [7])

        // Everything else about a run is its second line.
        guard case .desktop(let repo, let session) = model.entries[0] else { return XCTFail("desktop row") }
        XCTAssertEqual(
            AgentsLogic.desktopDetail(repo, session),
            "Desktop · alice/mobile · Issue #12 · Codex · agent/signed-inbox · 1 turns · PR #7 · 3 files · ↑2 ↓1 · Conflict · 1500 tokens"
        )
        guard case .desktop(let other, let done) = model.entries[1] else { return XCTFail("desktop row") }
        XCTAssertEqual(AgentsLogic.desktopDetail(other, done), "Desktop · bob/worker · Claude Code")
        XCTAssertEqual(AgentsLogic.statusTone(session.statusLabel), .accent)
        XCTAssertEqual(AgentsLogic.statusTone(done.statusLabel), .success)
    }

    func testARingReloadsTheListOnceForABurst() async throws {
        let api = globalApi()
        let signal = AgentListSignal()
        let model = AgentsListModel(api: api, signal: signal, debounce: 0.05)
        model.start()
        await model.waitForLoad()
        XCTAssertEqual(api.sessionCalls["alice/mobile"], 1)

        // A desktop pushing every step of a run rings in bursts; one reload
        // answers the burst.
        signal.ring()
        signal.ring()
        signal.ring()
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(api.sessionCalls["alice/mobile"], 2)
        XCTAssertEqual(model.groups.first { $0.repo.name == "mobile" }?.sessions.first?.statusLabel, "Done")
        model.stop()
    }

    func testARingDuringALoadLetsItFinishAndReadsOnceMore() async throws {
        let api = globalApi()
        let signal = AgentListSignal()
        let model = AgentsListModel(api: api, signal: signal, debounce: 0.05)
        model.start()
        await model.waitForLoad()
        api.holdRepositories = true
        signal.ring()
        try await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertEqual(api.repositoriesCalls, 2)

        signal.ring()
        signal.ring()
        try await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertEqual(api.repositoriesCalls, 2)

        api.holdRepositories = false
        api.repositoriesGate?.resume()
        try await Task.sleep(nanoseconds: 300_000_000)
        XCTAssertEqual(api.repositoriesCalls, 3)
        XCTAssertEqual(api.sessionCalls["alice/mobile"], 3)
        XCTAssertNil(model.loadError)
        model.stop()
    }

    func testAnUnchangedReloadDoesNotRedrawTheList() async {
        let api = FakeAgentsApi()
        api.pages["bob/worker"] = { _ in
            AgentSessionPage(sessions: [AgentSession(id: 8, issueTitle: "Same", status: "done", provider: "claude-code")])
        }
        let model = AgentsListModel(api: api)
        await model.refresh()
        let redrawn = ObservedFlag()
        withObservationTracking {
            _ = model.groups
            _ = model.isPendingFirstLoad
            _ = model.isFailed
            _ = model.listState
        } onChange: {
            redrawn.raise()
        }
        await model.refresh()
        XCTAssertEqual(api.sessionCalls["bob/worker"], 2)
        XCTAssertFalse(redrawn.raised)
    }

    func testNoTimerReadsAgainOnItsOwn() async throws {
        let api = globalApi()
        let model = AgentsListModel(api: api)
        model.start()
        await model.waitForLoad()
        try await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(api.sessionCalls["alice/mobile"], 1)
        await model.refresh()
        XCTAssertEqual(api.sessionCalls["alice/mobile"], 2)
    }

    func testADisabledScreenIgnoresInFlightLoads() async throws {
        let api = globalApi()
        api.holdRepositories = true
        let model = AgentsListModel(api: api)
        model.start()
        try await Task.sleep(nanoseconds: 50_000_000)
        XCTAssertEqual(api.repositoriesCalls, 1)
        model.enabled = false
        api.repositoriesGate?.resume()
        await model.waitForLoad()
        XCTAssertTrue(api.sessionCalls.isEmpty)
        XCTAssertTrue(model.groups.isEmpty)
    }

    func testOwnerAccountIsTheSignedInNodeOrTheOwner() async {
        let repo = Repository(owner: "bob", name: "x")
        XCTAssertEqual(AgentsListModel(api: FakeAgentsApi(), ownerAccountName: { " Alice " }).ownerAccount(for: repo), "alice")
        XCTAssertEqual(AgentsListModel(api: FakeAgentsApi()).ownerAccount(for: repo), "bob")
    }

    func testSealedOnlyRepositoriesStayAndEmptyOnesGo() async {
        let api = globalApi()
        api.pages["bob/worker"] = { _ in AgentSessionPage(sessions: [], sealedForOthers: 2) }
        api.repos.append(Repository(owner: "carol", name: "empty"))
        let model = AgentsListModel(api: api, hasDesktopNode: { false })
        await model.refresh()
        XCTAssertEqual(model.groups.map(\.repo.name), ["mobile", "worker"])
        XCTAssertEqual(model.sealedCount, 2)

        let empty = FakeAgentsApi()
        let none = AgentsListModel(api: empty, hasDesktopNode: { false })
        await none.refresh()
        XCTAssertEqual(none.listState, "no-host")
    }

    func testAQueuedTaskIsFollowedFromItsPromptToTheMerge() async {
        let api = FakeAgentsApi()
        api.repos = [Repository(owner: "alice", name: "mobile")]
        api.options["alice/mobile"] = AgentLaunchOptions(sessions: [
            AgentLaunchSession(
                sessionId: "sess-1", status: "completed", targetNode: "vultr-1", title: "Fix the crash",
                createdBy: "alice", createPr: true, localAgentId: 3, branchName: "agent/web-7-fix",
                baseBranch: "main", prNumber: 12
            ),
        ])
        api.ledger = [PublishedPull(number: 12, title: "Fix the crash", status: "merged", base: "main", head: "agent/web-7-fix")]
        let model = AgentsListModel(api: api)
        await model.refresh()

        XCTAssertEqual(api.pullCalls, 1, "the ledger is read once a task has a branch")
        guard case .launched(let repo, let session, let pulls, let cloud) = model.entries.first else { return XCTFail("queued row") }
        XCTAssertFalse(cloud)
        let chain = AgentsLogic.taskChain(session, pulls)
        // The list says how far it got from the pull ledger, not the mirror.
        XCTAssertEqual(chain.summary, "Merged")
        XCTAssertEqual(chain.stages.map(\.key), ["prompt", "mirror", "branch", "pull", "merge"])
        XCTAssertEqual(AgentsLogic.launchedDetail(repo, session, pulls: pulls), "Desktop · alice/mobile · vultr-1 · agent/web-7-fix · Merged")
    }

    func testARingKeepsThePullLedgerItReadUntilATaskMovesOrItGoesStale() async throws {
        let api = FakeAgentsApi()
        api.repos = [Repository(owner: "alice", name: "mobile")]
        func queue(_ status: String, pr: Int) {
            api.options["alice/mobile"] = AgentLaunchOptions(sessions: [
                AgentLaunchSession(
                    sessionId: "sess-1", status: status, targetNode: "vultr-1", title: "Fix the crash",
                    createdBy: "alice", createPr: true, localAgentId: 3, branchName: "agent/web-7-fix",
                    baseBranch: "main", prNumber: pr
                ),
            ])
        }
        queue("running", pr: 0)
        var clock = Date(timeIntervalSince1970: 1_000)
        let signal = AgentListSignal()
        let model = AgentsListModel(api: api, signal: signal, debounce: 0.01, now: { clock })
        model.start()
        await model.waitForLoad()
        XCTAssertEqual(api.pullCalls, 1)

        signal.ring()
        try await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertEqual(api.pullCalls, 1, "nothing moved, so the ledger is not read again")

        queue("completed", pr: 12)
        api.ledger = [PublishedPull(number: 12, title: "Fix the crash", status: "open", base: "main", head: "agent/web-7-fix")]
        signal.ring()
        try await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertEqual(api.pullCalls, 2, "the task opened a pull request")

        clock += AgentsListModel.pullsRefreshInterval
        api.ledger = [PublishedPull(number: 12, title: "Fix the crash", status: "merged", base: "main", head: "agent/web-7-fix")]
        signal.ring()
        try await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertEqual(api.pullCalls, 3, "an open pull request is checked again once the read is old")
        guard case .launched(_, let session, let pulls, _) = model.entries.first else { return XCTFail("queued row") }
        XCTAssertEqual(AgentsLogic.taskChain(session, pulls).summary, "Merged")

        clock += AgentsListModel.pullsRefreshInterval
        signal.ring()
        try await Task.sleep(nanoseconds: 150_000_000)
        XCTAssertEqual(api.pullCalls, 3, "a merged pull request is not read again on a ring")

        await model.refresh()
        XCTAssertEqual(api.pullCalls, 4, "an explicit refresh reads the ledger")
        model.stop()
    }

    func testOneListMarksEachRunDesktopOrCloud() async {
        let api = FakeAgentsApi()
        api.repos = [Repository(owner: "alice", name: "worker")]
        api.options["alice/worker"] = AgentLaunchOptions(
            targets: [
                AgentLaunchTarget(node: "alice", online: true, runtimeMode: "desktop"),
                AgentLaunchTarget(node: "mirror9", online: true, runtimeMode: "headless"),
            ],
            sessions: [
                AgentLaunchSession(sessionId: "cloud-1", provider: "claude-code", status: "running", targetNode: "MIRROR9", title: "Rebuild the mirror index", updatedAtMs: 3000),
                AgentLaunchSession(sessionId: "desk-1", provider: "codex", status: "running", targetNode: "alice", title: "Local desktop run", updatedAtMs: 2000),
            ]
        )
        let model = AgentsListModel(api: api)
        await model.refresh()

        XCTAssertEqual(model.entries.map(\.id), ["queued:alice/worker:cloud-1", "queued:alice/worker:desk-1"])
        XCTAssertEqual(model.entries.map(\.isCloud), [true, false])
        guard case .launched(let repo, let session, let pulls, let cloud) = model.entries[0] else { return XCTFail("queued row") }
        XCTAssertEqual(AgentsLogic.launchedDetail(repo, session, pulls: pulls, cloud: cloud), "Cloud · alice/worker · MIRROR9 · claude-code")
        XCTAssertFalse(AgentsLogic.runsInCloud(AgentLaunchSession(sessionId: "s", targetNode: "ghost"), model.groups[0].targets))
        XCTAssertFalse(AgentsLogic.runsInCloud(AgentLaunchSession(sessionId: "s"), model.groups[0].targets))
    }

    func testARunningQueuedTaskNamesItsBranch() async {
        let session = AgentLaunchSession(sessionId: "s", status: "running", targetNode: "vultr-1", title: "Fix", createPr: true, branchName: "agent/web-7-fix")
        XCTAssertEqual(AgentsLogic.taskChain(session, []).summary, "On agent/web-7-fix")
        let fresh = AgentLaunchSession(sessionId: "s", status: "queued", title: "Fix", createPr: true)
        XCTAssertEqual(AgentsLogic.launchedDetail(Repository(owner: "a", name: "b"), fresh, pulls: []), "Desktop · a/b · opens PR")
        XCTAssertEqual(AgentsLogic.chipTone("RUNNING"), .accent)
        XCTAssertEqual(AgentsLogic.chipTone("completed"), .success)
        XCTAssertEqual(AgentsLogic.chipTone("stopped"), .danger)
        XCTAssertEqual(AgentsLogic.chipTone("codex"), .secondary)
    }

    func testAFailedRepositoryReadKeepsTheListItHad() async {
        let api = globalApi()
        let model = AgentsListModel(api: api)
        await model.refresh()
        XCTAssertEqual(model.listState, "list-2")

        api.repositoriesError = AgentLaunchException("offline")
        await model.refresh()
        XCTAssertEqual(model.groups.map(\.repo.name), ["mobile", "worker"])
        XCTAssertEqual(model.loadError, "offline")
        XCTAssertFalse(model.isFailed)
        XCTAssertEqual(model.listState, "list-2")

        api.repositoriesError = nil
        await model.refresh()
        XCTAssertNil(model.loadError)
    }

    func testARepositoryWhoseReadsFailKeepsItsRows() async {
        let api = FakeAgentsApi()
        api.repos = [Repository(owner: "alice", name: "mobile")]
        api.pages["alice/mobile"] = { _ in
            AgentSessionPage(sessions: [AgentSession(id: 8, status: "done", provider: "codex")], sealedForOthers: 1)
        }
        api.options["alice/mobile"] = AgentLaunchOptions(sessions: [
            AgentLaunchSession(sessionId: "q1", status: "completed", updatedAtMs: 4_000),
        ])
        let model = AgentsListModel(api: api)
        await model.refresh()
        XCTAssertEqual(model.entries.map(\.id), ["desktop:alice/mobile:8", "queued:alice/mobile:q1"])

        api.optionsError = AgentLaunchException("busy", status: 503)
        await model.refresh()
        XCTAssertEqual(model.entries.map(\.id), ["desktop:alice/mobile:8", "queued:alice/mobile:q1"])

        api.optionsError = nil
        api.pageError = AgentLaunchException("busy", status: 503)
        api.options["alice/mobile"] = AgentLaunchOptions()
        await model.refresh()
        XCTAssertEqual(model.entries.map(\.id), ["desktop:alice/mobile:8"])
        XCTAssertEqual(model.sealedCount, 1)
    }

    func testARepositoryThatRefusesIsNotAskedAgainOnTheNextRing() async {
        let api = FakeAgentsApi()
        api.repos = [Repository(owner: "vm1", name: "forkmesh")]
        api.pageError = ApiException("HTTP 403 for /api/repo/vm1/forkmesh/agents/list")
        let model = AgentsListModel(api: api, ownerAccountName: { "alice" })
        await model.refresh()
        await model.refresh()
        XCTAssertEqual(api.sessionCalls, ["vm1/forkmesh": 1])

        api.pageError = ApiException("HTTP 500 for /api/repo/vm1/forkmesh/agents/list")
        api.repos = [Repository(owner: "vm2", name: "forkmesh")]
        await model.refresh()
        await model.refresh()
        XCTAssertEqual(api.sessionCalls["vm2/forkmesh"], 2)
    }

    func testModelsSharingRefusalsDoNotAskARefusingRepositoryTwice() async {
        let api = FakeAgentsApi()
        api.repos = [Repository(owner: "vm1", name: "forkmesh")]
        api.pageError = ApiException("HTTP 403 for /api/repo/vm1/forkmesh/agents/list")
        let refusals = AgentReadRefusals()
        await AgentsListModel(api: api, ownerAccountName: { "alice" }, refusals: refusals).refresh()
        await AgentsListModel(api: api, ownerAccountName: { "alice" }, refusals: refusals).refresh()
        XCTAssertEqual(api.sessionCalls, ["vm1/forkmesh": 1])

        await AgentsListModel(api: api, ownerAccountName: { "alice" }).refresh()
        XCTAssertEqual(api.sessionCalls, ["vm1/forkmesh": 2])
    }

    func testRefusalsLapseAfterTheirTtl() {
        let refusals = AgentReadRefusals()
        let at = Date(timeIntervalSince1970: 1_000)
        refusals.refuse("sealed|alice|vm1/forkmesh", now: at)
        XCTAssertTrue(refusals.isRefused("sealed|alice|vm1/forkmesh", ttl: 600, now: at.addingTimeInterval(599)))
        XCTAssertFalse(refusals.isRefused("sealed|alice|vm1/forkmesh", ttl: 600, now: at.addingTimeInterval(600)))
        XCTAssertFalse(refusals.isRefused("sealed|alice|vm1/forkmesh", ttl: 600, now: at))
    }

    func testRefusalsAreForbiddenOrMissingOnly() async {
        XCTAssertTrue(AgentsListModel.isRefusal(ApiException("HTTP 403 for /api/repo/a/b/agents/list")))
        XCTAssertTrue(AgentsListModel.isRefusal(AgentLaunchException("nope", status: 404)))
        XCTAssertFalse(AgentsListModel.isRefusal(AgentLaunchException("busy", status: 503)))
        XCTAssertFalse(AgentsListModel.isRefusal(ApiException("HTTP 4030")))
    }

    func testFinishedQueuedRunsBeyondTheListedWindowStay() async {
        let api = FakeAgentsApi()
        api.repos = [Repository(owner: "alice", name: "mobile")]
        api.options["alice/mobile"] = AgentLaunchOptions(sessions: [
            AgentLaunchSession(sessionId: "live", status: "running", updatedAtMs: 9_000),
            AgentLaunchSession(sessionId: "old", status: "completed", updatedAtMs: 3_000),
            AgentLaunchSession(sessionId: "gone", status: "failed", updatedAtMs: 8_500),
            AgentLaunchSession(sessionId: "stale", status: "queued", updatedAtMs: 2_000),
        ])
        let model = AgentsListModel(api: api)
        await model.refresh()
        XCTAssertEqual(model.groups.first?.launched.count, 4)

        api.options["alice/mobile"] = AgentLaunchOptions(
            sessions: [AgentLaunchSession(sessionId: "live", status: "running", updatedAtMs: 9_500)],
            finishedBeforeMs: 8_000)
        await model.refresh()
        XCTAssertEqual(model.groups.first?.launched.map(\.sessionId), ["live", "old"])

        api.options["alice/mobile"] = AgentLaunchOptions(
            sessions: [AgentLaunchSession(sessionId: "live", status: "running", updatedAtMs: 9_500)])
        await model.refresh()
        XCTAssertEqual(model.groups.first?.launched.map(\.sessionId), ["live"])
    }

    // MARK: - Retry

    private let failed = AgentLaunchSession(
        sessionId: "sess-9", provider: "claude-code", status: "failed", targetNode: "mirror-7", title: "Fix the crash",
        history: [AgentLaunchMessage(role: "user", text: "Fix the crash")], localAgentId: 5, branchName: "agent/web-9-fix"
    )
    private let neverStarted = AgentLaunchSession(
        sessionId: "sess-8", provider: "claude-code", status: "failed", targetNode: "mirror-7", title: "Fix the crash",
        issueNumber: 54, history: [AgentLaunchMessage(role: "user", text: "Fix the crash")]
    )

    func testAFailedRunIsRetriedFromTheListOnItsOwnBranch() async {
        let api = FakeAgentsApi()
        let model = AgentsListModel(api: api)
        let message = await model.retry(Repository(owner: "alice", name: "mobile"), failed)
        XCTAssertEqual(api.steers.map(\.sessionId), ["sess-9"])
        XCTAssertEqual(api.steers.first?.prompt, "Fix the crash")
        XCTAssertTrue(api.launches.isEmpty)
        XCTAssertEqual(message, "Follow-up sent.")
        XCTAssertTrue(model.retrying.isEmpty)
    }

    func testARunThatNeverStartedIsQueuedAgainAsItWas() async {
        let api = FakeAgentsApi()
        _ = await AgentsListModel(api: api).retry(Repository(owner: "alice", name: "mobile"), neverStarted)
        XCTAssertTrue(api.steers.isEmpty)
        XCTAssertEqual(api.launches.count, 1)
        XCTAssertEqual(api.launches.first?.node, "mirror-7")
        XCTAssertEqual(api.launches.first?.provider, "claude-code")
        XCTAssertEqual(api.launches.first?.issue, 54)
    }

    func testTheSessionScreenSaysWhatARetryWillDo() async {
        XCTAssertTrue(AgentsLogic.retryDetail(failed).contains("picks this session up on the branch"))
        XCTAssertTrue(AgentsLogic.retryDetail(neverStarted).contains("queued again on mirror-7"))
        let completed = AgentLaunchSession(sessionId: "s1", status: "completed", title: "Fix the crash", localAgentId: 4)
        XCTAssertFalse(completed.isRetryable)

        let api = FakeAgentsApi()
        api.queueResult = AgentQueueResult(message: "Sent to mirror-7.", session: AgentLaunchSession(sessionId: "sess-10", status: "queued"))
        let screen = LaunchedAgentSessionModel(api: api, repo: Repository(owner: "alice", name: "mobile"), session: neverStarted)
        XCTAssertEqual(screen.followUpClosedText, "This session never started on a node, so there is nothing to resume.")
        await screen.retry()
        XCTAssertEqual(api.launches.count, 1)
        // The screen follows the run it started instead of the one that failed.
        XCTAssertEqual(screen.session.sessionId, "sess-10")
        XCTAssertEqual(screen.notice, "Sent to mirror-7.")
    }

    func testAFollowUpSteersTheSessionAndDropRemovesIt() async {
        let api = FakeAgentsApi()
        let screen = LaunchedAgentSessionModel(api: api, repo: Repository(owner: "alice", name: "mobile"), session: failed)
        XCTAssertNil(screen.followUpClosedText)
        screen.followUp = "  also the README  "
        await screen.send()
        XCTAssertEqual(api.steers.first?.prompt, "also the README")
        XCTAssertEqual(screen.followUp, "")
        let gone = await screen.cancel()
        XCTAssertTrue(gone)
        XCTAssertEqual(api.cancels, ["sess-9"])
    }

    func testTheSessionScreenShowsLiveFramesAndStopsWhenTheRelayHasNoStream() async {
        let api = FakeAgentsApi()
        let running = AgentLaunchSession(
            sessionId: "sess-5", status: "running", title: "Fix", localAgentId: 3, liveText: "old", liveAtMs: 1_000)
        api.liveResponses = [
            (200, "retry: 0\n\nevent: live\ndata: {\"at\":1700000000000,\"text\":\"Reading\"}\n\n: keepalive\n\nevent: live\ndata: {\"append\":\" files\"}\n\n"),
            (404, ""),
        ]
        let screen = LaunchedAgentSessionModel(api: api, repo: Repository(owner: "alice", name: "mobile"), session: running)
        XCTAssertTrue(screen.followsLive)
        await screen.followLive()
        XCTAssertEqual(api.liveOpened, ["alice/mobile/sess-5", "alice/mobile/sess-5"])
        XCTAssertEqual(screen.session.liveText, "Reading files")
        XCTAssertEqual(screen.session.liveAtMs, 1_700_000_000_000)
        XCTAssertTrue(screen.liveRefused)
        XCTAssertFalse(screen.followsLive)
        await screen.followLive()
        XCTAssertEqual(api.liveOpened.count, 2)
    }

    func testTheSessionScreenRereadsTheRunWhenTheStreamSaysItEnded() async {
        let api = FakeAgentsApi()
        let running = AgentLaunchSession(sessionId: "sess-5", status: "running", title: "Fix", localAgentId: 3)
        api.options["alice/mobile"] = AgentLaunchOptions(sessions: [
            AgentLaunchSession(sessionId: "sess-5", status: "completed", title: "Fix", localAgentId: 3),
        ])
        api.liveResponses = [
            (200, "event: live\ndata: {\"at\":1700000000000,\"text\":\"Done soon\"}\n\nevent: end\ndata: {\"status\":\"completed\"}\n\n"),
        ]
        let screen = LaunchedAgentSessionModel(api: api, repo: Repository(owner: "alice", name: "mobile"), session: running)
        await screen.followLive()
        XCTAssertEqual(api.liveOpened.count, 1)
        XCTAssertEqual(screen.session.status, "completed")
        XCTAssertEqual(screen.session.liveText, "")
        XCTAssertFalse(screen.liveRefused)
        XCTAssertFalse(screen.followsLive)
    }

    func testTheTranscriptScreenPrefersItsOwnStatus() async {
        let api = FakeAgentsApi()
        let model = AgentTranscriptModel(
            api: api, repo: Repository(owner: "alice", name: "mobile"),
            session: AgentSession(id: 7, status: "running"), ownerAccount: "alice")
        await model.load()
        XCTAssertEqual(model.statusLabel, "Done")
        XCTAssertEqual(model.bodyText, "Session 7 transcript for alice/mobile")
    }
}

@MainActor
final class AgentStartModelTests: XCTestCase {
    private func api(hasNode: Bool = true, hasMirror: Bool = false, providers: [String] = []) -> FakeAgentsApi {
        let api = FakeAgentsApi()
        api.repos = [Repository(owner: "alice", name: "mobile", cloneOnline: true)]
        var targets: [AgentLaunchTarget] = []
        if hasNode {
            targets.append(AgentLaunchTarget(node: "desk-1", providers: providers, online: true, source: "true", runtimeMode: "desktop"))
            if hasMirror {
                targets.append(AgentLaunchTarget(node: "vultr-1", online: true, source: "false", runtimeMode: "headless"))
            }
        }
        api.options["alice/mobile"] = AgentLaunchOptions(canQueue: hasNode, targets: targets)
        return api
    }

    func testAMirroredMeshRepositoryIsNamedAndClonedByItsPublicOwner() {
        let flagship = AgentStartTarget(
            repo: Repository(owner: "mirror35", name: "forkmesh", mirrors: 4, publicOwner: "forkmesh"))
        XCTAssertEqual(flagship.label, "forkmesh/forkmesh")
        XCTAssertEqual(flagship.owner, "mirror35")
        XCTAssertEqual(flagship.publicOwner, "forkmesh")
        XCTAssertEqual(flagship.detail, "On the mesh · 4 mirrors · not on this device yet")
        XCTAssertEqual(
            AgentStartTarget(repo: Repository(owner: "bob", name: "x", mirrors: 0)).detail,
            "On the mesh · not on this device yet")
    }

    func testAnAccountWithNoAgentsIsOfferedAPrompt() async {
        let model = AgentStartModel(api: api(), records: nil, signedIn: { true })
        await model.loadTargets()
        XCTAssertEqual(model.targetLabel, "alice/mobile")
        XCTAssertEqual(model.plan?.target.detail, "On the mesh · 1 mirror · not on this device yet")
        // The node is named, so it is clear where the work will happen.
        XCTAssertEqual(model.runsOnText, "Sent to node desk-1, which is online now.")
        XCTAssertEqual(model.submitLabel, "Send to mirror")
    }

    func testANodeWithAnAgentCLIOffersToSignThatAgentIn() async {
        let model = AgentStartModel(api: api(providers: ["claude-code", "codex", "other"]), records: nil, signedIn: { true })
        await model.loadTargets()
        XCTAssertEqual(model.signInProviders, ["claude-code", "codex"])
    }

    func testTheStartButtonWaitsForAPrompt() async {
        let model = AgentStartModel(api: api(), records: nil, signedIn: { true })
        await model.loadTargets()
        XCTAssertFalse(model.canStart)
        model.prompt = "   "
        XCTAssertFalse(model.canStart)
        model.prompt = "Fix it"
        XCTAssertTrue(model.canStart)
    }

    func testATypedPromptQueuesARunOnTheNode() async {
        let fake = api()
        let model = AgentStartModel(api: fake, records: nil, signedIn: { true })
        await model.loadTargets()
        model.prompt = "Fix the crash when opening an empty repository"
        let started = await model.startOnNode()
        XCTAssertTrue(started)
        XCTAssertEqual(fake.launches.count, 1)
        XCTAssertEqual(fake.launches.first?.owner, "alice")
        XCTAssertEqual(fake.launches.first?.repo, "mobile")
        XCTAssertEqual(fake.launches.first?.prompt, "Fix the crash when opening an empty repository")
        XCTAssertEqual(fake.launches.first?.node, "desk-1")
        XCTAssertEqual(model.notice, "Queued on desk-1.")
        XCTAssertEqual(model.prompt, "")
    }

    func testAMirrorServedRepositoryGoesByItsPublicName() async {
        let fake = api()
        fake.repos = [Repository(owner: "mirror35", name: "forkmesh", cloneOnline: true, publicOwner: "forkmesh")]
        fake.options = ["mirror35/forkmesh": AgentLaunchOptions(canQueue: true, targets: [AgentLaunchTarget(node: "desk-1", online: true)])]
        let model = AgentStartModel(api: fake, records: nil, signedIn: { true })
        await model.loadTargets()
        XCTAssertEqual(model.targetLabel, "forkmesh/forkmesh")
        model.prompt = "Fix the crash"
        await model.startOnNode()
        // The relay is still asked by the catalog row, which routes the run.
        XCTAssertEqual(fake.launches.first?.owner, "mirror35")
    }

    func testWithNowhereToRunTheFormSaysSo() async {
        let model = AgentStartModel(api: api(hasNode: false), records: nil, signedIn: { true })
        await model.loadTargets()
        XCTAssertTrue(model.runsOnText.contains("cannot mirror it to the device"))
        model.prompt = "Fix the crash"
        XCTAssertFalse(model.canStart)
    }

    func testThePromptGoesToTheMirrorWhenTheAccountHasOne() async {
        let fake = api(hasMirror: true)
        let model = AgentStartModel(api: fake, records: nil, signedIn: { true })
        await model.loadTargets()
        XCTAssertTrue(model.runsOnText.contains("mirror vultr-1"))
        XCTAssertTrue(model.canPickNode)
        model.prompt = "Fix the crash"
        await model.startOnNode()
        XCTAssertEqual(fake.launches.first?.node, "vultr-1")

        model.nodeChoice = "desk-1"
        XCTAssertEqual(model.node?.node, "desk-1")
    }

    func testSignedOutTheFormAsksForASignIn() async {
        let model = AgentStartModel(api: api(), records: nil, signedIn: { false })
        await model.loadTargets()
        model.prompt = "x"
        XCTAssertTrue(model.canStart)
        XCTAssertEqual(model.submitLabel, "Sign in to start")
        XCTAssertTrue(model.runsOnText.hasPrefix("Sign in and the prompt goes to a mirror"))
    }

    func testAForbiddenQueueIsAnOrdinaryAnswer() async {
        let fake = api()
        fake.optionsError = AgentLaunchException("nope", status: 403)
        let model = AgentStartModel(api: fake, records: { [] }, signedIn: { true })
        await model.loadTargets()
        XCTAssertEqual(model.plan?.error, "")
        XCTAssertEqual(model.effectiveMode, .device)
        XCTAssertEqual(model.runsOnText, "Clones alice/mobile from its mirrors to this device first, then runs here on your own API key.")
        model.prompt = "Fix"
        XCTAssertEqual(model.deviceStart(), .clone(owner: "alice", name: "mobile", prompt: "Fix"))
    }

    func testMirrorsOnThisDeviceComeFirstAndFoldIntoTheirMeshRepository() async throws {
        let fake = api()
        fake.repos.append(Repository(owner: "bob", name: "tool"))
        let mirror = LocalRepoRecord(name: "mobile", path: "/tmp/mobile", createdAt: Date(), publishedAs: "alice/mobile")
        let orphan = LocalRepoRecord(name: "scratch", path: "/tmp/scratch", createdAt: Date())
        let model = AgentStartModel(api: fake, records: { [orphan, mirror] }, signedIn: { true })
        await model.loadTargets()
        XCTAssertEqual(model.targets?.map(\.label), ["alice/mobile", "scratch", "bob/tool"])
        XCTAssertEqual(model.plan?.target.detail, "Mirrored on this device")
        XCTAssertTrue(model.offersBothModes)
        model.mode = .device
        XCTAssertEqual(model.runsOnText, "Runs here, on the mirror already on this device.")
        model.prompt = "Fix"
        XCTAssertEqual(model.deviceStart(), .open(mirror, prompt: "Fix"))

        // An unpublished repository is still a mirror here: it runs on it.
        await model.selectTarget(try XCTUnwrap(model.targets?[1]))
        XCTAssertEqual(model.plan?.target.detail, "On this device · not published")
        XCTAssertEqual(model.runsOnText, "Runs here, on the mirror already on this device.")
        XCTAssertEqual(model.deviceStart(), .open(orphan, prompt: "Fix"))
    }

    func testARunOnThisDeviceCanUseGemma() async throws {
        let temp = TempDirectory()
        defer { temp.cleanUp() }
        let manager = try localModels(FakeLocalChatEngine(), in: temp, enabled: false)
        let secure = InMemorySecureStore()
        let store = AgentCredentialStore(secureStore: secure, http: FakeHTTPClient(), localModels: manager)
        store.write(.onDevice(model: tinyModel.id))
        let fake = api(hasNode: false)
        let model = AgentStartModel(api: fake, records: { [] }, signedIn: { true }, credentialStore: store, localModels: { manager })
        await model.loadTargets()
        XCTAssertTrue(model.deviceRunsOnGemma)
        XCTAssertEqual(model.deviceModelLabel, "Tiny · on this iPhone")
        XCTAssertEqual(model.runsOnText, "Clones alice/mobile from its mirrors to this device first, then runs here with Gemma 4 on this iPhone.")
        XCTAssertEqual(model.gemmaReadiness?.canBeFixedInSettings, true)
        manager.setEnabled(true)
        XCTAssertEqual(model.gemmaReadiness?.isReady, true)
    }
}

private final class ObservedFlag: @unchecked Sendable {
    private let lock = NSLock()
    private var value = false

    var raised: Bool { lock.withLock { value } }

    func raise() { lock.withLock { value = true } }
}
