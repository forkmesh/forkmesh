import Foundation
import XCTest

@testable import FMAgentWatch

@MainActor
final class AgentWatchActivityTests: XCTestCase {
    private final class Run: @unchecked Sendable {
        var text = ""
        var reads = 0
    }

    private func target(_ id: Int, _ run: Run) -> AgentWatchTarget {
        AgentWatchTarget(key: "k\(id)", id: id, title: "t\(id)") {
            run.reads += 1
            return AgentWatchSnapshot(status: "running", beats: beatsFromTranscript(run.text))
        }
    }

    private func walk(_ controller: AgentWatchController, _ clock: ManualAgentWatchClock, _ seconds: Double) {
        var left = seconds
        while left > 0 {
            clock.advance(by: 0.05)
            controller.advance()
            left -= 0.05
        }
    }

    func testASteadyStreamOfRingsKeepsReading() async {
        let run = Run()
        let clock = ManualAgentWatchClock()
        let controller = AgentWatchController(
            targets: [target(1, run)], output: RecordingOutput(), clock: clock, debounce: 0.4)
        await controller.start()
        let before = run.reads
        for _ in 0..<40 {
            controller.ring()
            clock.advance(by: 0.05)
            await controller.settle()
        }
        XCTAssertGreaterThanOrEqual(run.reads - before, 4)
        controller.dispose()
    }

    func testTheBusiestAgentRisesWearsItsLatestWordAndSlidesToItsSeat() async {
        let runs = [1: Run(), 2: Run()]
        let clock = ManualAgentWatchClock()
        let controller = AgentWatchController(
            targets: [target(1, runs[1]!), target(2, runs[2]!)], output: RecordingOutput(), clock: clock,
            debounce: 0.1)
        await controller.start()
        walk(controller, clock, kRankSeconds + 1)
        let leader = controller.agents.first { $0.rank == 0 }!
        let busy = controller.agents.first { $0.rank == 1 }!
        let size = CGSize(width: 390, height: 900)
        let before = WatchFieldLayout(size, controller.agents)
        before.glide(clock.now)
        let leaderSeat = before.pointFor(leader)
        XCTAssertLessThan(Double(leaderSeat.x), Double(before.pointFor(busy).x))

        runs[busy.id]!.text = "[Bash git status]\n[Read /a]\n[Grep b]\n[Edit /a]\n[Bash swift test]\n[Read /c]\n"
        let rang = clock.now
        controller.ring()
        clock.advance(by: 0.1)
        await controller.settle()
        walk(controller, clock, 4)
        XCTAssertEqual(busy.tag?.label, AgentMotion.read.label)
        XCTAssertGreaterThan(busy.tagSeconds, rang)
        XCTAssertGreaterThan(controller.bytesIn, 0)
        XCTAssertGreaterThan(controller.bytesPerSecond(clock.now), 0)
        XCTAssertGreaterThanOrEqual(controller.eventsPerMinute(clock.now), 6)

        walk(controller, clock, kRankSeconds)
        XCTAssertEqual(busy.rank, 0)
        XCTAssertEqual(leader.rank, 1)
        let after = WatchFieldLayout(size, controller.agents)
        XCTAssertLessThan(Double(after.seatFor(busy).x), Double(after.seatFor(leader).x))
        XCTAssertEqual(Double(after.pointFor(leader).x), Double(leaderSeat.x), accuracy: 0.01)
        XCTAssertTrue(after.glide(clock.now + 0.05))
        var t = clock.now + 0.05
        while after.glide(t) { t += 0.05 }
        XCTAssertEqual(Double(after.pointFor(leader).x), Double(after.seatFor(leader).x), accuracy: 0.01)
        controller.dispose()
    }
}
