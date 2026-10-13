import Foundation
import XCTest

@testable import FMAgentWatch

@MainActor
final class AgentWatchLayoutTests: XCTestCase {
    private func agent(_ id: Int, _ status: String) -> WatchedAgent {
        let target = AgentWatchTarget(key: "\(id)", id: id, title: "t\(id)") {
            AgentWatchSnapshot(status: "", beats: [])
        }
        let agent = WatchedAgent(target, Voice.forAgent(id, "p"))
        agent.setState(watchStateFor(status))
        return agent
    }

    func testSeatsEachBandInItsOwnRowsFewerToARowOnAPhone() async {
        let agents = (1...4).map { agent($0, "running") } + (5...14).map { agent($0, "done") }
        let phone = WatchFieldLayout(CGSize(width: 390, height: 1400), agents)
        XCTAssertEqual(WatchFieldLayout.rowSizeFor(390), 3)
        XCTAssertEqual(Set(phone.rows.map(\.band)), [.working, .resting])
        XCTAssertEqual(phone.rows.first?.count, 4)
        XCTAssertLessThan(phone.pointFor(agents[0]).y, phone.pointFor(agents[agents.count - 1]).y)
        let desktop = WatchFieldLayout(CGSize(width: 1600, height: 1000), agents)
        XCTAssertEqual(desktop.rows.count, 3)
        XCTAssertGreaterThan(WatchFieldLayout.heightFor(390, agents), 800)
        // A tap on a body finds it.
        let first = agents[0]
        XCTAssertTrue(phone.agentAt(phone.pointFor(first), 0) === first)
        XCTAssertNil(phone.agentAt(CGPoint(x: -500, y: -500), 0))
    }

    func testAPhoneSeatsItsRowsFromTheTopBesideTheSoundColumn() async {
        let agents = (1...4).map { agent($0, "running") }
        XCTAssertEqual(WatchFieldLayout.rowSizeFor(250), 2)
        let tall = WatchFieldLayout(CGSize(width: 250, height: 900), agents)
        XCTAssertEqual(tall.rows.count, 2)
        let first = tall.rows[0]
        let rowTop = Double(tall.field.minY) + (first.y - first.height / 2) * Double(tall.field.height)
        XCTAssertLessThan(rowTop, 60)
        XCTAssertEqual(first.height * Double(tall.field.height), 150, accuracy: 0.5)
        XCTAssertLessThan(Double(tall.pointFor(agents[3]).y), 400)
        let height = WatchFieldLayout.heightFor(250, agents)
        let fitted = WatchFieldLayout(CGSize(width: 250, height: height), agents)
        let last = fitted.rows[fitted.rows.count - 1]
        let lastBottom = (last.y + last.height / 2) * Double(fitted.field.height) + Double(fitted.field.minY)
        XCTAssertLessThan(lastBottom, Double(fitted.desktopNode.y) - 30)
        let spread = WatchFieldLayout(CGSize(width: 250, height: 900), agents, packed: false)
        XCTAssertGreaterThan(spread.rows[0].height, first.height)
    }

    func testTheFaceIsTheOneTheDesktopDrawsForTheModel() async {
        XCTAssertEqual(watchFaceName(provider: "claude-code", model: "claude-opus-5-5"), "opus")
        XCTAssertEqual(watchFaceName(provider: "claude-code", model: ""), "feather")
        XCTAssertEqual(watchFaceName(provider: "codex", model: "gpt-5"), "sun")
        XCTAssertEqual(watchFaceName(provider: "", model: ""), "globe-leaf")
    }

    func testCardLinesSayWhoItIs() async {
        let target = AgentWatchTarget(key: "a", id: 7, title: "#7 - Sound the agents", subtitle: "forkmesh/forkmesh") {
            AgentWatchSnapshot(status: "done", beats: [])
        }
        let controller = AgentWatchController(
            targets: [target], output: SilentAgentWatchOutput(), clock: ManualAgentWatchClock())
        let agent = controller.agents[0]
        agent.setState(.done)
        let lines = watchCardLines(controller, agent)
        XCTAssertEqual(lines[0], "#7 forkmesh \u{00B7} Sound the agents")
        XCTAssertTrue(lines[1].hasPrefix("done \u{00B7} playing as "))
        XCTAssertEqual(lines.last, "double tap to stop hearing it")
        controller.dispose()
    }
}
