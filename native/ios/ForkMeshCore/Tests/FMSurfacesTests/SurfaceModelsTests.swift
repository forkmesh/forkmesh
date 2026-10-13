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

    func testContentStateDecodesServerPayload() throws {
        let json = """
        {"status":"needs_approval","title":"Fix","repo":"o/r","detail":"Allow?","turns":3,
         "startedAt":1790000000123,"progress":0.5,"needsApproval":true,"extra":"ignored"}
        """
        let state = try JSONDecoder().decode(AgentRunAttributes.ContentState.self, from: Data(json.utf8))
        XCTAssertEqual(state.state, .needsApproval)
        XCTAssertEqual(state.turns, 3)
        XCTAssertEqual(state.startedDate, Date(timeIntervalSince1970: 1_790_000_000.123))
        XCTAssertEqual(
            AgentRunAttributes.ContentState(status: "running", startedAt: 1_790_000_000).startedDate,
            Date(timeIntervalSince1970: 1_790_000_000))
        XCTAssertNil(state.approvalRoute)
        let minimal = try JSONDecoder().decode(AgentRunAttributes.ContentState.self, from: Data("{}".utf8))
        XCTAssertEqual(minimal.status, "running")
        XCTAssertFalse(minimal.needsApproval)
        let clamped = AgentRunAttributes.ContentState(status: "running", startedAt: 1_000_000_000)
        XCTAssertNil(clamped.startedDate)
        let attributes = try JSONDecoder().decode(
            AgentRunAttributes.self, from: Data(#"{"sessionId":"desktop:4","repo":"o/r","title":"T"}"#.utf8))
        XCTAssertEqual(attributes.route, .agent(id: "desktop:4"))
    }

    func testRunKeys() {
        XCTAssertEqual(AgentRunKey.make("desktop:12"), "desktop:12")
        XCTAssertEqual(AgentRunKey.make("node:3f2a-9_x.y"), "node:3f2a-9_x.y")
        XCTAssertNil(AgentRunKey.make("node:a/b"))
        XCTAssertNil(AgentRunKey.make(""))
        XCTAssertNil(AgentRunKey.make(String(repeating: "a", count: 65)))
        XCTAssertNil(AgentRunKey.make("node:é"))
    }

    func testPlannerStartsUpdatesAndEnds() {
        let cards = [
            AgentRunCard(id: "desktop:1", title: "One", repo: "o/r", state: .running, startedAtMs: 1_790_000_000_000),
            AgentRunCard(id: "desktop:2", title: "Two", repo: "o/r", state: .needsApproval, approvalId: "ap"),
            AgentRunCard(id: "desktop:3", title: "Three", repo: "o/r", state: .completed),
            AgentRunCard(id: "bad/id", title: "Bad", repo: "o/r", state: .running),
            AgentRunCard(id: "desktop:5", title: "Five", repo: "o/r", state: .queued),
        ]
        let surface = AgentsSurface(runs: cards)
        let same = AgentRunAttributes.ContentState.from(cards[0])
        let existing: [String: AgentRunAttributes.ContentState] = [
            "desktop:1": same,
            "desktop:3": AgentRunAttributes.ContentState(status: "running"),
            "desktop:9": AgentRunAttributes.ContentState(status: "running"),
        ]
        let actions = LiveActivityPlanner.plan(agents: surface, existing: existing, canStart: true, limit: 2)
        XCTAssertEqual(actions.count, 3)
        XCTAssertEqual(actions[0], .end("desktop:3", AgentRunAttributes.ContentState.from(cards[2])))
        XCTAssertEqual(actions[1], .end("desktop:9", nil))
        guard case let .start(attributes, state) = actions[2] else { return XCTFail("expected a start") }
        XCTAssertEqual(attributes.sessionId, "desktop:2")
        XCTAssertTrue(state.needsApproval)
        XCTAssertEqual(state.approvalRoute, .approval(id: "ap"))
        XCTAssertEqual(same.startedAt, 1_790_000_000_000)
        let quiet = LiveActivityPlanner.plan(agents: surface, existing: [:], canStart: false)
        XCTAssertTrue(quiet.isEmpty)
        var changed = existing
        changed["desktop:1"] = AgentRunAttributes.ContentState(status: "queued")
        let updates = LiveActivityPlanner.plan(agents: surface, existing: changed, canStart: false)
        XCTAssertTrue(updates.contains(.update("desktop:1", same)))
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
