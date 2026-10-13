import XCTest
@testable import FMApi

final class HomeDashboardTests: XCTestCase {
    func testTwoCountTilesInHudOrder() {
        let stats: [HomeStat] = HomeDashboard.stats(HomeDashboardInput())
        XCTAssertEqual(stats.map(\.kind), [.agents, .pings])
        XCTAssertEqual(stats.map(\.value), ["0", "0"])
        XCTAssertEqual(Set(stats.map(\.id)).count, stats.count)
    }

    func testAgentToneFollowsTheWorstState() {
        var input = HomeDashboardInput(agents: HomeAgentTally(working: 2))
        XCTAssertEqual(HomeDashboard.agents(input).tone, .live)
        XCTAssertEqual(HomeDashboard.agents(input).value, "2")
        XCTAssertEqual(HomeDashboard.agents(input).detail, "running")
        input.agents.waiting = 1
        XCTAssertEqual(HomeDashboard.agents(input).tone, .attention)
        XCTAssertEqual(HomeDashboard.agents(input).detail, "1 waiting")
        XCTAssertEqual(HomeDashboard.agents(input).value, "3")
        input.agents.failed = 1
        XCTAssertEqual(HomeDashboard.agents(input).tone, .alert)
        XCTAssertEqual(HomeDashboard.agents(HomeDashboardInput(agents: HomeAgentTally(done: 4))).detail, "4 done")
        XCTAssertEqual(HomeDashboard.agents(HomeDashboardInput()).detail, "idle")
    }

    func testRunningCountCoversAgentsTheWatchHasNotSeated() {
        let input = HomeDashboardInput(agents: HomeAgentTally(working: 1, waiting: 1), runningAgents: 4)
        XCTAssertEqual(input.working, 3)
        XCTAssertEqual(HomeDashboard.agents(input).value, "4")
    }

    func testMeshToneTracksTheRelay() {
        XCTAssertEqual(HomeDashboard.mesh(HomeDashboardInput(online: 5, relay: .connected)).tone, .live)
        XCTAssertEqual(HomeDashboard.mesh(HomeDashboardInput(online: 5, relay: .connected)).value, "5")
        XCTAssertEqual(HomeDashboard.mesh(HomeDashboardInput(relay: .connecting)).detail, "linking")
        XCTAssertEqual(HomeDashboard.mesh(HomeDashboardInput(relay: .offline)).tone, .alert)
    }

    func testCountTilesFlagWhatNeedsTheUser() {
        let input = HomeDashboardInput(pings: 3, chatUnread: 2, timers: 1, hostsAttention: 1)
        XCTAssertEqual(HomeDashboard.pings(input).tone, .attention)
        XCTAssertEqual(HomeDashboard.chat(input).tone, .attention)
        XCTAssertEqual(HomeDashboard.timers(input).tone, .live)
        XCTAssertEqual(HomeDashboard.hosts(input).tone, .alert)
    }

    func testTelemetryTiles() {
        XCTAssertEqual(HomeDashboard.memory(megabytes: 212.4).value, "212")
        XCTAssertEqual(HomeDashboard.memory(megabytes: 212.4).tone, .idle)
        XCTAssertEqual(HomeDashboard.memory(megabytes: 1200).value, "1.2k")
        XCTAssertEqual(HomeDashboard.memory(megabytes: 1200).tone, .attention)
        XCTAssertEqual(HomeDashboard.memory(megabytes: .nan).value, "–")
        XCTAssertEqual(HomeDashboard.requests(perSecond: 0).value, "0")
        XCTAssertEqual(HomeDashboard.requests(perSecond: 0).tone, .idle)
        XCTAssertEqual(HomeDashboard.requests(perSecond: 1.5).value, "1.5")
        XCTAssertEqual(HomeDashboard.requests(perSecond: 14.6).value, "15")
        XCTAssertEqual(HomeDashboard.requests(perSecond: -.infinity).value, "0")
    }

    func testCompactCounts() {
        XCTAssertEqual(HomeDashboard.compact(-2), "0")
        XCTAssertEqual(HomeDashboard.compact(999), "999")
        XCTAssertEqual(HomeDashboard.compact(1000), "1k")
        XCTAssertEqual(HomeDashboard.compact(1250), "1.2k")
        XCTAssertEqual(HomeDashboard.compact(25_400), "25k")
        XCTAssertEqual(HomeDashboard.compact(3_100_000), "3M")
    }

    func testGreetingAndUptime() {
        XCTAssertEqual(HomeDashboard.greeting(hour: 7), "Good morning")
        XCTAssertEqual(HomeDashboard.greeting(hour: 13), "Good afternoon")
        XCTAssertEqual(HomeDashboard.greeting(hour: 19), "Good evening")
        XCTAssertEqual(HomeDashboard.greeting(hour: 2), "Up late")
        XCTAssertEqual(HomeDashboard.uptime(42), "42s")
        XCTAssertEqual(HomeDashboard.uptime(125), "2m 5s")
        XCTAssertEqual(HomeDashboard.uptime(3 * 3600 + 7 * 60), "3h 7m")
    }

    func testHeadlineSaysWhatIsHappeningFirst() {
        XCTAssertEqual(HomeDashboard.headline(HomeDashboardInput(relay: .connected)), "All quiet")
        XCTAssertEqual(HomeDashboard.headline(HomeDashboardInput()), "Offline · nothing running")
        let busy = HomeDashboardInput(
            agents: HomeAgentTally(working: 1, waiting: 2), pings: 4, chatUnread: 1, relay: .connected)
        XCTAssertEqual(HomeDashboard.headline(busy), "1 agent working · 2 waiting on you · 4 pings")
    }
}
