import XCTest
@testable import FMAgents

final class AgentTokenTankTests: XCTestCase {
    private func run(_ tank: AgentTokenTank, _ seconds: Double) {
        for _ in 0..<Int(seconds * 30) { tank.advance(1.0 / 30) }
    }

    private func meanX(_ tank: AgentTokenTank) -> Double {
        tank.xs.prefix(tank.grains).reduce(0, +) / Double(tank.grains)
    }

    private func assertInside(_ tank: AgentTokenTank) {
        for i in 0..<tank.grains {
            XCTAssertTrue((0...tank.width).contains(tank.xs[i]) && (0...tank.height).contains(tank.ys[i]), "grain \(i) left the tank")
        }
    }

    func testDroppedGrainsRainInOverASecond() {
        let tank = AgentTokenTank(width: 60, height: 12, leakSeconds: 1e9)
        XCTAssertTrue(tank.idle)
        tank.drop(30)
        XCTAssertFalse(tank.idle)
        run(tank, 0.5)
        XCTAssertTrue((10...20).contains(tank.grains), "half a second rained \(tank.grains)")
        run(tank, 0.6)
        XCTAssertEqual(tank.grains, 30)
    }

    func testGrainsSettleOnTheFloor() {
        let tank = AgentTokenTank(width: 60, height: 12, leakSeconds: 1e9)
        tank.drop(240)
        run(tank, 5)
        XCTAssertEqual(tank.grains, 240)
        assertInside(tank)
        for i in 0..<tank.grains { XCTAssertGreaterThan(tank.ys[i], 6, "grain \(i) floats") }
    }

    func testTiltPoursToTheLowSide() {
        let tank = AgentTokenTank(width: 60, height: 12, leakSeconds: 1e9)
        tank.drop(200)
        run(tank, 3)
        XCTAssertTrue((25...35).contains(meanX(tank)), "level tank centred at \(meanX(tank))")
        tank.tilt(x: 1, y: 0)
        run(tank, 4)
        XCTAssertGreaterThan(meanX(tank), 45)
        tank.tilt(x: -1, y: 0)
        run(tank, 4)
        XCTAssertLessThan(meanX(tank), 15)
        assertInside(tank)
    }

    func testOverflowStopsAtCapacity() {
        let tank = AgentTokenTank(width: 60, height: 12, leakSeconds: 1e9)
        XCTAssertEqual(tank.capacity, 720)
        tank.drop(5_000)
        run(tank, 3)
        XCTAssertEqual(tank.grains, tank.capacity)
        assertInside(tank)
    }

    func testLeakEmptiesTheTankOnceTokensStop() {
        let tank = AgentTokenTank(width: 60, height: 12)
        tank.drop(200)
        run(tank, 1.5)
        let full = tank.grains
        run(tank, 10)
        XCTAssertLessThan(tank.grains, full)
        run(tank, 120)
        XCTAssertEqual(tank.grains, 0)
        XCTAssertTrue(tank.idle)
    }

    func testPourScalesTheSecondToThePeak() {
        let peak = AgentTokenTank(width: 60, height: 12)
        peak.pour(tokens: 1_000, scale: 1_000)
        run(peak, 1.1)
        XCTAssertTrue((55...60).contains(peak.grains), "a peak second rained \(peak.grains)")
        let over = AgentTokenTank(width: 60, height: 12)
        over.pour(tokens: 9_000, scale: 1_000)
        run(over, 1.1)
        XCTAssertEqual(over.grains, peak.grains)
        let quarter = AgentTokenTank(width: 60, height: 12)
        quarter.pour(tokens: 250, scale: 1_000)
        run(quarter, 1.1)
        XCTAssertTrue((13...15).contains(quarter.grains), "a quarter second rained \(quarter.grains)")
        let none = AgentTokenTank(width: 60, height: 12)
        none.pour(tokens: 0, scale: 1_000)
        XCTAssertTrue(none.idle)
        XCTAssertEqual(AgentTokenTank.scale([10, 400], floor: 1_000), 1_000)
        XCTAssertEqual(AgentTokenTank.scale([10, 4_000], floor: 1_000), 4_000)
        XCTAssertEqual(AgentTokenTank.scale([], floor: 50), 50)
    }

    func testTiltKeepsAFlatPhoneFallingDownAndCapsAShake() {
        let tank = AgentTokenTank(width: 60, height: 12)
        tank.tilt(x: 0, y: 1)
        XCTAssertEqual(tank.gravityX, 0)
        XCTAssertEqual(tank.gravityY, AgentTokenTank.gravity)
        tank.tilt(x: 0, y: 0)
        XCTAssertTrue(tank.gravityY > 0 && tank.gravityX == 0)
        tank.tilt(x: 0, y: -1)
        XCTAssertLessThan(tank.gravityY, 0)
        tank.tilt(x: 20, y: 0)
        XCTAssertEqual(hypot(tank.gravityX, tank.gravityY), AgentTokenTank.gravity * AgentTokenTank.maxPull, accuracy: 1e-9)
    }

    func testEmptyTankIgnoresEverything() {
        let tank = AgentTokenTank(width: 0, height: 12)
        XCTAssertEqual(tank.capacity, 0)
        tank.drop(10)
        tank.pour(tokens: 100, scale: 10)
        run(tank, 1)
        XCTAssertEqual(tank.grains, 0)
    }

    func testThePairAsksForTheAccelerometerOnlyWhileGrainsAreInATank() {
        let pair = AgentTokenTanks(width: 60, height: 12)
        XCTAssertFalse(pair.wantsMotion)
        pair.output.pour(tokens: 500, scale: 1_000)
        XCTAssertTrue(pair.wantsMotion)
        for frame in 0...(30 * 200) {
            pair.advance(to: Date(timeIntervalSinceReferenceDate: Double(frame) * AgentTokenTanks.frameSeconds))
        }
        XCTAssertTrue(pair.idle)
        XCTAssertFalse(pair.wantsMotion)
    }

    func testThirtyFramesASecondRunTheSameSimulationAsTheDisplayRate() {
        XCTAssertEqual(AgentTokenTanks.frameSeconds, 1.0 / 30, accuracy: 1e-12)
        let paced = AgentTokenTanks(width: 60, height: 12)
        let fast = AgentTokenTanks(width: 60, height: 12)
        for pair in [paced, fast] {
            pair.input.drop(300)
            pair.output.drop(100)
            pair.tilt(x: 0.4, y: 0.1)
        }
        for frame in 0...(32 * 3) { paced.advance(to: Date(timeIntervalSinceReferenceDate: Double(frame) / 32)) }
        for frame in 0...(128 * 3) { fast.advance(to: Date(timeIntervalSinceReferenceDate: Double(frame) / 128)) }
        XCTAssertEqual(paced.input.stepsTaken, 180)
        XCTAssertEqual(fast.input.stepsTaken, 180)
        for (a, b) in [(paced.input, fast.input), (paced.output, fast.output)] {
            XCTAssertEqual(a.grains, b.grains)
            XCTAssertEqual(Array(a.xs.prefix(a.grains)), Array(b.xs.prefix(b.grains)))
            XCTAssertEqual(Array(a.ys.prefix(a.grains)), Array(b.ys.prefix(b.grains)))
        }
    }
}
