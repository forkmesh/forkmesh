import FMApi
import FMBase
import FMPlanning
import XCTest

@testable import FMSurfaces

final class SurfaceModelsTests: XCTestCase {
    func testRunStateMapsStatuses() {
        XCTAssertEqual(AgentRunState(status: "Running", isActive: true), .running)
        XCTAssertEqual(AgentRunState(status: "waiting_for_input", isActive: true), .waiting)
        XCTAssertEqual(AgentRunState(status: "needs_approval", isActive: true), .needsApproval)
        XCTAssertEqual(AgentRunState(status: "cancelled", isActive: false), .stopped)
        XCTAssertEqual(AgentRunState(status: "success", isActive: false), .completed)
        XCTAssertEqual(AgentRunState(status: "error", isActive: false), .failed)
        XCTAssertEqual(AgentRunState(status: "mystery", isActive: true), .running)
        XCTAssertEqual(AgentRunState(status: "mystery", isActive: false), .completed)
        XCTAssertEqual(AgentRunState(status: "done", isActive: false, needsApproval: true), .needsApproval)
        XCTAssertTrue(AgentRunState.queued.isActive)
        XCTAssertFalse(AgentRunState.failed.isActive)
    }

    private func run(_ id: String, _ status: String, active: Bool, local: Int = 0, desktop: Bool = true,
                     updated: Int64 = 0) -> AgentRunInput {
        AgentRunInput(
            id: id, title: "Run \(id)", repoOwner: "forkmesh", repoName: "app", status: status, isActive: active,
            provider: "Claude", detail: "step", updatedAtMs: updated, localId: local, isDesktop: desktop, canStop: true)
    }

    func testAgentsSurfaceCountsAndOrders() {
        let runs = [
            run("desktop:1", "done", active: false, local: 1, updated: 900),
            run("desktop:2", "running", active: true, local: 2, updated: 100),
            run("node:abc", "queued", active: true, desktop: false, updated: 300),
            run("desktop:3", "running", active: true, local: 3, updated: 200),
            run("desktop:3", "running", active: true, local: 3, updated: 999),
        ]
        let approvals = [
            AgentApprovalInput(id: "ap-1", sessionId: 3, question: "Allow rm -rf build?", askedAtMs: 500),
            AgentApprovalInput(id: "ap-9", sessionId: 42, question: "Other", askedAtMs: 10),
        ]
        let surface = AgentsSurface.build(runs: runs, approvals: approvals)
        XCTAssertEqual(surface.needsYou, 2)
        XCTAssertEqual(surface.running, 1)
        XCTAssertEqual(surface.queued, 1)
        XCTAssertEqual(surface.runs.map(\.id), ["desktop:3", "desktop:2", "node:abc", "desktop:1"])
        let first = surface.runs[0]
        XCTAssertEqual(first.state, .needsApproval)
        XCTAssertEqual(first.approvalId, "ap-1")
        XCTAssertEqual(first.detail, "Allow rm -rf build?")
        XCTAssertEqual(first.route, .approval(id: "ap-1"))
        XCTAssertEqual(surface.runs[1].route, .agent(id: "desktop:2"))
        XCTAssertFalse(surface.runs[3].canStop)
        XCTAssertEqual(surface.summaryLine, "2 need you, 1 running, 1 queued")
        XCTAssertEqual(surface.total, 4)
        let limited = AgentsSurface.build(runs: runs, approvals: [], limit: 2)
        XCTAssertEqual(limited.runs.count, 2)
        XCTAssertEqual(limited.total, 4)
        XCTAssertEqual(AgentsSurface.empty.summaryLine, "No agents running")
    }

    func testNodeRunsNeverTakeDesktopApprovals() {
        let surface = AgentsSurface.build(
            runs: [run("node:7", "running", active: true, local: 7, desktop: false)],
            approvals: [AgentApprovalInput(id: "a", sessionId: 7, question: "q", askedAtMs: 1)])
        XCTAssertEqual(surface.runs[0].state, .running)
        XCTAssertEqual(surface.needsYou, 1)
    }

    func testTextIsClipped() {
        let long = String(repeating: "x", count: 300)
        let surface = AgentsSurface.build(
            runs: [AgentRunInput(id: "desktop:1", title: long + "\nmore", repoOwner: "o", repoName: "r",
                                 status: "running", isActive: true)],
            approvals: [])
        XCTAssertEqual(surface.runs[0].title.count, 80)
        XCTAssertTrue(surface.runs[0].title.hasSuffix("…"))
        XCTAssertEqual(surface.runs[0].title.contains("\n"), false)
    }

    func testInboxSurfaceTakesNewestUnread() {
        let notes = [
            ForkNotification(id: "1", kind: "pull_submitted", title: "PR opened", repo: "forkmesh/app", ts: 10,
                             meta: ["number": .int(4)]),
            ForkNotification(id: "2", kind: "mention", title: "", repo: "", ts: 30),
            ForkNotification(id: "3", kind: "ping", title: "Read", ts: 40, readAt: 41),
            ForkNotification(id: "4", kind: "issue_assigned", title: "Assigned", repo: "forkmesh/app", ts: 20,
                             meta: ["number": .int(9)]),
        ]
        let surface = InboxSurface.build(unread: 7, notifications: notes, limit: 2)
        XCTAssertEqual(surface.unread, 7)
        XCTAssertEqual(surface.items.map(\.id), ["2", "4"])
        XCTAssertEqual(surface.items[1].route, .issue(RepoRef(owner: "forkmesh", name: "app"), number: 9))
        XCTAssertFalse(surface.items[0].title.isEmpty)
        XCTAssertEqual(InboxSurface.build(unread: -3, notifications: []).unread, 0)
    }

    func testRouteCodecRoundTrips() throws {
        let routes: [Route] = [
            .inbox(.approvals),
            .compose(ComposeTarget(repo: RepoRef(owner: "a", name: "b"), text: "Fix it + ship / now?")),
            .pull(RepoRef(owner: "a", name: "b"), number: 12, tab: .files),
            .agent(id: "desktop:5"),
            .timers,
        ]
        for route in routes {
            let token = try XCTUnwrap(SurfaceRouteCodec.encode(route))
            XCTAssertFalse(token.contains("/"))
            XCTAssertFalse(token.contains("+"))
            XCTAssertFalse(token.contains("="))
            XCTAssertEqual(SurfaceRouteCodec.decode(token), route)
            let url = try XCTUnwrap(SurfaceLink.url(for: route))
            XCTAssertTrue(SurfaceLink.isSurfaceLink(url))
            XCTAssertEqual(SurfaceLink.route(from: url), route)
        }
        XCTAssertNil(SurfaceRouteCodec.decode(""))
        XCTAssertNil(SurfaceRouteCodec.decode("not base64 !!"))
        XCTAssertNil(SurfaceRouteCodec.decode(String(repeating: "A", count: 5_000)))
        XCTAssertNil(SurfaceRouteCodec.encode(.compose(ComposeTarget(text: String(repeating: "y", count: 4_000)))))
        XCTAssertNil(SurfaceLink.route(from: URL(string: "forkmesh://ios-udid")!))
        XCTAssertNil(SurfaceLink.route(from: URL(string: "https://surface/open?route=abc")!))
    }

    func testSummaryFoldsAgentsShortcutsAndTimer() throws {
        let cards = [
            AgentRunCard(id: "desktop:1", title: "One", repo: "o/r", state: .running, startedAtMs: 1_790_000_000_000),
            AgentRunCard(id: "desktop:2", title: "Two", repo: "o/r", state: .needsApproval, approvalId: "ap"),
            AgentRunCard(id: "desktop:3", title: "Three", repo: "o/r", state: .completed),
            AgentRunCard(id: "desktop:4", title: "Four", repo: "o/r", state: .queued),
            AgentRunCard(id: "desktop:5", title: "Five", repo: "o/r", state: .running),
        ]
        let surface = AgentsSurface(needsYou: 1, running: 2, queued: 1, runs: cards)
        let shortcuts = [
            ActivityShortcutInput(
                id: 7, title: "Deploy", repo: "o/r", running: true, startedAtMs: 1_790_000_000_000, expectedMs: 60_000),
            ActivityShortcutInput(id: 8, title: "Lint", repo: "o/r", running: true, startedAtMs: 0, expectedMs: -1),
            ActivityShortcutInput(id: 9, title: "Old", repo: "o/r", running: false, startedAtMs: 0, expectedMs: 1),
            ActivityShortcutInput(id: 6, title: "Test", repo: "o/r", running: true, startedAtMs: 0, expectedMs: -1),
        ]
        let timer = TimerActivitySnapshot(
            timerId: "t", kind: .countdown, label: "Tea", endsAtMs: 1_790_000_000_500, countsFromMs: 0, paused: false,
            pausedText: "")
        let state = ActivitySummaryAttributes.ContentState.build(
            agents: surface, shortcuts: shortcuts, timer: timer, unread: -2)
        XCTAssertEqual(state.agents.map(\.id), ["desktop:1", "desktop:2", "desktop:4"])
        XCTAssertEqual(state.activeAgents, 4)
        XCTAssertEqual(state.shortcutsRunning, 3)
        XCTAssertEqual(state.shortcuts.map(\.id), [8, 7])
        XCTAssertEqual(state.unread, 0)
        XCTAssertEqual(state.compactCount, 7)
        XCTAssertEqual(state.summaryLine, "1 needs you, 2 running, 1 queued · 3 shortcuts")
        XCTAssertEqual(state.route, .approval(id: "ap"))
        XCTAssertEqual(state.pendingApproval?.id, "desktop:2")
        XCTAssertNil(state.stoppable)
        XCTAssertEqual(state.leadState, .needsApproval)
        XCTAssertEqual(state.systemImage, "hand.raised.fill")
        XCTAssertEqual(state.agents[0].startedAt, Date(timeIntervalSince1970: 1_790_000_000))
        XCTAssertNil(state.agents[1].startedAt)
        XCTAssertEqual(state.shortcuts[1].expectedEnd, Date(timeIntervalSince1970: 1_790_000_060))
        XCTAssertNil(state.shortcuts[0].expectedEnd)
        XCTAssertEqual(state.shortcuts[1].route, .shortcuts(RepoRef(owner: "o", name: "r")))
        let decoded = try JSONDecoder().decode(
            ActivitySummaryAttributes.ContentState.self, from: JSONEncoder().encode(state))
        XCTAssertEqual(decoded, state)
        XCTAssertLessThan(try JSONEncoder().encode(state).count, 4_096)
    }

    func testSummaryLinesAndRoutes() {
        let one = AgentRunCard(id: "desktop:1", title: "One", repo: "o/r", state: .running, canStop: true)
        let single = ActivitySummaryAttributes.ContentState.build(agents: AgentsSurface(running: 1, runs: [one]))
        XCTAssertEqual(single.stoppable?.id, "desktop:1")
        XCTAssertEqual(single.route, .agent(id: "desktop:1"))
        XCTAssertEqual(single.summaryLine, "1 running")
        let many = ActivitySummaryAttributes.ContentState(running: 5, agents: [ActivitySummaryAttributes.Agent(one)])
        XCTAssertEqual(many.route, .agents(.running))
        XCTAssertNil(many.stoppable)
        let orphan = ActivitySummaryAttributes.ContentState(needsYou: 1)
        XCTAssertFalse(orphan.isEmpty)
        XCTAssertEqual(orphan.route, .agents(.needsYou))
        let timer = TimerActivitySnapshot(
            timerId: "t", kind: .countdown, label: "Tea", endsAtMs: 0, countsFromMs: 0, paused: false, pausedText: "")
        let timed = ActivitySummaryAttributes.ContentState(timer: timer)
        XCTAssertEqual(timed.summaryLine, "Tea")
        XCTAssertEqual(timed.route, .timers)
        XCTAssertEqual(timed.compactCount, 0)
        let shortcut = ActivitySummaryAttributes.Shortcut(id: 1, title: "S", repo: "", startedAtMs: 0, expectedMs: 0)
        XCTAssertEqual(ActivitySummaryAttributes.ContentState(shortcuts: [shortcut]).summaryLine, "1 shortcut")
        XCTAssertTrue(ActivitySummaryAttributes.ContentState(unread: 4).isEmpty)
        XCTAssertEqual(ActivitySummaryAttributes.ContentState().summaryLine, "All done")
    }

    func testSummaryPlannerStartsUpdatesAndEnds() {
        let card = AgentRunCard(id: "desktop:1", title: "One", repo: "o/r", state: .running)
        let busy = ActivitySummaryAttributes.ContentState.build(agents: AgentsSurface(running: 1, runs: [card]))
        let idle = ActivitySummaryAttributes.ContentState()
        XCTAssertEqual(ActivitySummaryPlanner.plan(next: busy, current: nil, canStart: true), .start(busy))
        XCTAssertNil(ActivitySummaryPlanner.plan(next: busy, current: nil, canStart: false))
        XCTAssertNil(ActivitySummaryPlanner.plan(next: idle, current: nil, canStart: true))
        XCTAssertNil(ActivitySummaryPlanner.plan(next: busy, current: busy, canStart: false))
        var queued = busy
        queued.running = 0
        queued.queued = 1
        XCTAssertEqual(ActivitySummaryPlanner.plan(next: queued, current: busy, canStart: false), .update(queued))
        XCTAssertEqual(ActivitySummaryPlanner.plan(next: idle, current: busy, canStart: true), .end(idle))
        XCTAssertNil(ActivitySummaryPlanner.plan(next: idle, current: busy, canStart: true, agentsReliable: false))
        var unread = idle
        unread.unread = 3
        XCTAssertEqual(
            ActivitySummaryPlanner.plan(next: unread, current: busy, canStart: true, agentsReliable: false),
            .update(unread.keepingAgents(of: busy)))
    }

    func testTimerSnapshotDates() {
        let snapshot = TimerActivitySnapshot(
            timerId: "t", kind: .countdown, label: "Tea", endsAtMs: 1_790_000_000_500, countsFromMs: 0, paused: false,
            pausedText: "")
        XCTAssertEqual(snapshot.endsAt, Date(timeIntervalSince1970: 1_790_000_000.5))
        XCTAssertNil(snapshot.countsFrom)
        XCTAssertEqual(snapshot.route, .timers)
    }

    func testEntityRefs() {
        let repo = RepoRef(owner: "forkmesh", name: "app")
        XCTAssertEqual(SurfaceEntityRef.of(.repo(repo, section: .pulls)), SurfaceEntityRef(kind: .repo, id: "forkmesh/app"))
        let issue = SurfaceEntityRef.of(.issue(repo, number: 5))
        XCTAssertEqual(issue?.id, "forkmesh/app#5")
        XCTAssertEqual(issue?.route, .issue(repo, number: 5))
        XCTAssertEqual(issue?.defaultTitle, "forkmesh/app #5")
        XCTAssertEqual(SurfaceEntityRef.of(.pull(repo, number: 2, tab: .files))?.route, .pull(repo, number: 2, tab: .conversation))
        XCTAssertNil(SurfaceEntityRef.of(.issue(repo, number: 0)))
        XCTAssertNil(SurfaceEntityRef.of(.note(id: "")))
        XCTAssertNil(SurfaceEntityRef.of(.inbox(.all)))
        let note = SurfaceEntityRef(kind: .note, id: "n1")
        XCTAssertEqual(SurfaceEntityRef.fromSpotlightIdentifier(note.spotlightIdentifier), note)
        XCTAssertNil(SurfaceEntityRef.fromSpotlightIdentifier("issue:forkmesh/app"))
        XCTAssertNil(SurfaceEntityRef.fromSpotlightIdentifier("weird:x"))
        XCTAssertNil(SurfaceEntityRef.parseNumbered("a/b#x"))
    }

    func testSnapshotSections() {
        let base = SurfaceSnapshot(generatedAtMs: 1, signedIn: true)
        XCTAssertEqual(base.changedSections(from: nil), Set(SurfaceSection.allCases))
        var next = base
        next.generatedAtMs = 99
        XCTAssertTrue(next.changedSections(from: base).isEmpty)
        next.inbox = InboxSurface(unread: 1)
        XCTAssertEqual(next.changedSections(from: base), [.inbox, .overview])
        var counted = base
        counted.work = WorkSurface(openIssues: 3, updatedAtMs: 5)
        XCTAssertEqual(counted.changedSections(from: base), [.overview])
        var restamped = counted
        restamped.work.updatedAtMs = 99
        XCTAssertTrue(restamped.changedSections(from: counted).isEmpty)
        var agents = base
        agents.agents = AgentsSurface(running: 1)
        XCTAssertEqual(agents.changedSections(from: base), [.agents, .overview])
        XCTAssertEqual(SurfaceSnapshot.signedOut(at: 5).changedSections(from: base), Set(SurfaceSection.allCases))
        XCTAssertEqual(SurfaceSection.agents.widgetKind, "com.forkmesh.widget.agents")
        XCTAssertEqual(SurfaceSection.overview.widgetKind, "com.forkmesh.widget.overview")
    }

    func testWorkSurfacePicksOwnedRepos() {
        let repos = [
            Repository(owner: "jett", name: "old", updatedMs: 10),
            Repository(owner: "node-7", name: "site", updatedMs: 30, publicOwner: "Jett"),
            Repository(owner: "alice", name: "other", updatedMs: 99),
            Repository(owner: "Jett", name: "new", updatedMs: 50),
            Repository(owner: "jett", name: "NEW", updatedMs: 20),
        ]
        let refs = WorkSurface.repos(repos, account: " JETT ")
        XCTAssertEqual(refs, [
            RepoRef(owner: "Jett", name: "new"), RepoRef(owner: "node-7", name: "site"), RepoRef(owner: "jett", name: "old"),
        ])
        XCTAssertEqual(WorkSurface.repos(repos, account: "jett", limit: 1).count, 1)
        XCTAssertTrue(WorkSurface.repos(repos, account: "").isEmpty)
    }

    func testWorkSurfaceCountsOpenItems() {
        let issues: [[Issue]?] = [
            [Issue(number: 1, title: "a"), Issue(number: 2, title: "b", status: "closed")],
            nil,
            [Issue(number: 3, title: "c", status: "in-progress")],
        ]
        XCTAssertEqual(WorkSurface.openIssues(issues), 2)
        XCTAssertNil(WorkSurface.openIssues([nil, nil]))
        XCTAssertEqual(WorkSurface.openIssues([]), 0)
        let pulls: [[PullRequest]?] = [[
            PullRequest(number: 1, title: "a"), PullRequest(number: 2, title: "b", status: "merged"),
            PullRequest(number: 3, title: "c", status: "Open"),
        ]]
        XCTAssertEqual(WorkSurface.openPulls(pulls), 2)
    }

    func testWorkSurfaceTaskPages() {
        let task: (String) -> JSONValue = { .object(["status": .string($0)]) }
        let first: JSONObject = [
            "tasks": .array([task("idle"), task("active"), task("done")]), "hasMore": .bool(true), "nextOffset": .int(50),
        ]
        XCTAssertEqual(WorkSurface.taskPage(first), WorkSurface.TaskPage(open: 2, nextOffset: 50))
        let last: JSONObject = ["tasks": .array([task("done")]), "hasMore": .bool(false), "nextOffset": .int(99)]
        XCTAssertEqual(WorkSurface.taskPage(last), WorkSurface.TaskPage(open: 0, nextOffset: nil))
        XCTAssertEqual(WorkSurface.taskPage([:]), WorkSurface.TaskPage(open: 0, nextOffset: nil))
        XCTAssertEqual(WorkSurface.label(nil), "–")
        XCTAssertEqual(WorkSurface.label(7), "7")
        XCTAssertEqual(WorkSurface.label(250, more: true), "250+")
    }

    func testWalkActivityAttributesRoundTrip() throws {
        let start = Date(timeIntervalSince1970: 1_789_561_860.25)
        let attributes = WalkActivityAttributes(startedAt: start)
        XCTAssertEqual(attributes.startedAtMs, 1_789_561_860_250)
        XCTAssertEqual(attributes.startedAt, start)
        XCTAssertEqual(attributes.route, .steps)
        let walking = WalkActivityAttributes.ContentState(steps: 1234, todaySteps: 8012, distance: "0.93 km")
        XCTAssertFalse(walking.ended)
        XCTAssertNil(walking.endedAt)
        let ended = WalkActivityAttributes.ContentState(
            steps: 1234, todaySteps: 8012, distance: "0.93 km", endedAtMs: 1_789_563_060_000)
        XCTAssertTrue(ended.ended)
        XCTAssertEqual(ended.endedAt, Date(timeIntervalSince1970: 1_789_563_060))
        let data = try JSONEncoder().encode(ended)
        XCTAssertEqual(try JSONDecoder().decode(WalkActivityAttributes.ContentState.self, from: data), ended)
    }
}
