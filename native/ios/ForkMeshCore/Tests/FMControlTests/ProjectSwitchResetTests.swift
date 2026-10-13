import FMApi
import FMBase
import FMChat
import XCTest

@testable import FMControl

final class ProjectSwitchResetTests: XCTestCase {
    private func session(_ id: String, _ repo: String) -> AgentLaunchSession {
        AgentLaunchSession(
            sessionId: id, owner: "alice", repo: repo, provider: "codex", status: "running", targetNode: "mirror35",
            title: repo, updatedAtMs: 1)
    }

    @MainActor func testTheAgentsListForgetsTheProjectLeft() async throws {
        let hub = DesktopControlHub(local: nil, openRelay: nil, paired: { [] })
        let cloudApi = FakeCloudAgents()
        cloudApi.sessions = [session("old", "old-repo")]
        cloudApi.finishedBefore = 5
        let center = AgentControlCenter(hub: hub, cloud: cloudApi)
        await center.refresh()
        XCTAssertEqual(center.rows.map(\.repoName), ["old-repo"])

        center.resetForProjectSwitch()
        XCTAssertTrue(center.rows.isEmpty)
        XCTAssertFalse(center.loading)

        cloudApi.sessions = [session("new", "new-repo")]
        await center.refresh()
        XCTAssertEqual(center.rows.map(\.repoName), ["new-repo"])
    }

    @MainActor func testAccountDesktopsLoadedForTheProjectLeftAreDropped() async throws {
        let box = DesktopsBox()
        box.answer = [PairedDesktop(key: "k-old", name: "old-desk", pairedAtMs: 1)]
        let hub = DesktopControlHub(
            local: nil, openRelay: nil, paired: { [] },
            accountDesktops: {
                if box.delay > 0 { try await Task.sleep(nanoseconds: box.delay) }
                return box.answer
            })
        await hub.refreshAccountDesktops()
        XCTAssertEqual(hub.candidates.map(\.key), ["k-old"])

        box.delay = 60_000_000
        let late = Task { @MainActor in await hub.refreshAccountDesktops(force: true) }
        try await Task.sleep(nanoseconds: 10_000_000)
        hub.resetForProjectSwitch()
        _ = await late.value
        XCTAssertTrue(hub.candidates.isEmpty)
        XCTAssertEqual(hub.reach, .notPaired)

        box.delay = 0
        box.answer = [PairedDesktop(key: "k-new", name: "new-desk", pairedAtMs: 2)]
        await hub.refreshAccountDesktops()
        XCTAssertEqual(hub.candidates.map(\.key), ["k-new"])
    }

    @MainActor func testOverlappingAccountDesktopReadsShareOneRequest() async {
        let box = DesktopsBox()
        box.answer = [PairedDesktop(key: "k-one", name: "desk", pairedAtMs: 1)]
        box.delay = 50_000_000
        let hub = DesktopControlHub(
            local: nil, openRelay: nil, paired: { [] },
            accountDesktops: {
                box.loads += 1
                try await Task.sleep(nanoseconds: box.delay)
                return box.answer
            })
        async let warm: [PairedDesktop] = hub.refreshAccountDesktops()
        async let agents: [PairedDesktop] = hub.refreshAccountDesktops()
        let (first, second) = await (warm, agents)
        XCTAssertEqual(box.loads, 1)
        XCTAssertEqual(first.map(\.key), ["k-one"])
        XCTAssertEqual(second.map(\.key), ["k-one"])

        await hub.refreshAccountDesktops(force: true)
        XCTAssertEqual(box.loads, 2)
    }
}

final class DesktopsBox: @unchecked Sendable {
    var loads = 0
    var answer: [PairedDesktop] = []
    var delay: UInt64 = 0
}
