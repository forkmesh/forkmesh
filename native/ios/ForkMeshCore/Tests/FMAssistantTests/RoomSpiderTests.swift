import XCTest
@testable import FMAssistant

final class RoomSpiderTests: XCTestCase {
    private let viewer = RoomPoint(0, 1.4, 0)

    private let floorPlane = RoomSurface(id: "floor", kind: .floor, center: RoomPoint(0, 0, -1), normal: RoomPoint(0, 1, 0), across: RoomPoint(1, 0, 0), half: SIMD2<Float>(2, 2))
    private let wall = RoomSurface(id: "wall", kind: .wall, center: RoomPoint(0, 1.2, -2), normal: RoomPoint(0, 0, -1), across: RoomPoint(1, 0, 0), half: SIMD2<Float>(2, 0.8))
    private let table = RoomSurface(id: "table", kind: .table, center: RoomPoint(1, 0.75, -1), normal: RoomPoint(0, 1, 0), across: RoomPoint(1, 0, 0), half: SIMD2<Float>(0.4, 0.3))
    private let ceiling = RoomSurface(id: "ceiling", kind: .ceiling, center: RoomPoint(0, 2.5, -1), normal: RoomPoint(0, 1, 0), across: RoomPoint(1, 0, 0), half: SIMD2<Float>(2, 2))

    private func room(_ surfaces: [RoomSurface]? = nil) -> RoomMap {
        var map = RoomMap()
        map.look(from: viewer)
        for surface in surfaces ?? [floorPlane, wall, table, ceiling] { map.put(surface) }
        return map
    }

    private func run(_ spider: inout RoomSpider, _ map: RoomMap, seconds: Double, until done: (RoomSpider) -> Bool = { _ in false }) {
        var left = seconds
        while left > 0 {
            spider.step(1.0 / 60.0, in: map)
            left -= 1.0 / 60.0
            if done(spider) { return }
        }
    }

    private func landed(_ map: RoomMap, at point: RoomPoint, seed: UInt64 = 7) -> RoomSpider {
        var spider = RoomSpider(seed: seed)
        spider.appear(at: point, in: map)
        run(&spider, map, seconds: 10) { $0.mode == .crawling }
        return spider
    }

    private func throwSpider(_ spider: inout RoomSpider, from a: RoomPoint, to b: RoomPoint, over seconds: Double, map: RoomMap) {
        spider.grab(at: a, time: 10)
        for index in 1...5 {
            let t = Float(index) / 5
            spider.drag(to: a + (b - a) * t, time: 10 + seconds * Double(t))
        }
        spider.release(time: 10 + seconds, in: map)
    }

    func testSurfacesFaceTheViewerAndTheCeilingFacesDown() {
        let map = room()
        XCTAssertEqual(map.surface("wall")?.normal, RoomPoint(0, 0, 1))
        XCTAssertEqual(map.surface("ceiling")?.normal, RoomPoint(0, -1, 0))
        XCTAssertEqual(map.surface("table")?.normal, RoomPoint(0, 1, 0))
        XCTAssertEqual(map.floorY, 0)
        XCTAssertTrue(map.floorFound)
    }

    func testARefinedPlaneKeepsTheSideItFirstFaced() {
        var map = room()
        map.look(from: RoomPoint(0, 1.4, -3))
        map.put(RoomSurface(id: "wall", kind: .wall, center: RoomPoint(0, 1.25, -2), normal: RoomPoint(0, 0, -1), across: RoomPoint(1, 0, 0), half: SIMD2<Float>(2.2, 0.8)))
        XCTAssertEqual(map.surface("wall")?.normal, RoomPoint(0, 0, 1))
    }

    func testTheSummaryNamesWhatWasFound() {
        XCTAssertEqual(room().summary, "the floor, a wall, a table and the ceiling")
        let second = RoomSurface(id: "wall2", kind: .wall, center: RoomPoint(2, 1.2, -1), normal: RoomPoint(-1, 0, 0), across: RoomPoint(0, 0, 1), half: SIMD2<Float>(1, 0.8))
        XCTAssertEqual(room([floorPlane, wall, second]).summary, "the floor and 2 walls")
        XCTAssertEqual(room([]).summary, "")
    }

    func testWithoutPlanesTheFloorIsGuessedBelowThePhone() {
        let map = room([])
        XCTAssertEqual(map.floorY, viewer.y - RoomMap.fallbackDrop, accuracy: 1e-5)
        XCTAssertFalse(map.floorFound)
        XCTAssertFalse(map.floor.bounded)
    }

    func testWallsReachDownToTheFloor() {
        let map = room()
        let wall = map.surface("wall")!
        XCTAssertTrue(wall.covers(RoomPoint(0, 0.05, -1.97), floor: map.floorY))
        XCTAssertFalse(wall.covers(RoomPoint(0, 2.3, -1.97), floor: map.floorY))
        XCTAssertFalse(wall.covers(RoomPoint(2.5, 1, -1.97), floor: map.floorY))
    }

    func testTurningFromTheFloorOntoAWallPointsUpTheWall() {
        let turned = RoomMath.rotate(RoomPoint(0, 0, -1), from: RoomPoint(0, 1, 0), to: RoomPoint(0, 0, 1))
        XCTAssertEqual(turned.x, 0, accuracy: 1e-5)
        XCTAssertEqual(turned.y, 1, accuracy: 1e-5)
        XCTAssertEqual(turned.z, 0, accuracy: 1e-5)
    }

    func testItDropsInOnSilkAndLandsOnTheFloor() {
        let map = room()
        var spider = RoomSpider(seed: 1)
        spider.appear(at: RoomPoint(-1, 1.2, -1), in: map)
        XCTAssertNotNil(spider.silkAnchor)
        XCTAssertEqual(spider.caption, RoomSpiderAct.hello.line)
        run(&spider, map, seconds: 10) { $0.mode == .crawling }
        XCTAssertEqual(spider.mode, .crawling)
        XCTAssertEqual(spider.surface, "floor")
        XCTAssertEqual(spider.position.y, RoomSpider.stand, accuracy: 0.01)
    }

    func testDroppingOverTheTableLandsOnTheTable() {
        let spider = landed(room(), at: RoomPoint(1, 1.3, -1))
        XCTAssertEqual(spider.surface, "table")
        XCTAssertEqual(spider.act, .dance)
    }

    func testThrownAtTheWallItSticksAndClimbs() {
        let map = room()
        var spider = landed(map, at: RoomPoint(0, 1, -1))
        throwSpider(&spider, from: RoomPoint(0, 1.2, -0.6), to: RoomPoint(0, 1.25, -0.9), over: 0.06, map: map)
        XCTAssertEqual(spider.mode, .flying)
        run(&spider, map, seconds: 3) { $0.mode == .crawling }
        XCTAssertEqual(spider.surface, "wall")
        XCTAssertEqual(spider.up, RoomPoint(0, 0, 1))
        XCTAssertEqual(spider.position.z, -2 + RoomSpider.stand, accuracy: 0.01)
        XCTAssertGreaterThan(spider.thumps, 0)
    }

    func testAVeryHardThrowLeavesItDizzy() {
        let map = room()
        var spider = landed(map, at: RoomPoint(0, 1, -1))
        throwSpider(&spider, from: RoomPoint(0, 1.2, -0.4), to: RoomPoint(0, 1.2, -0.9), over: 0.06, map: map)
        run(&spider, map, seconds: 3) { $0.mode == .crawling }
        XCTAssertEqual(spider.surface, "wall")
        XCTAssertEqual(spider.act, .dizzy)
    }

    func testDroppedHardOnTheFloorItBouncesFirst() {
        let map = room()
        var spider = landed(map, at: RoomPoint(-1, 1, -0.5))
        let before = spider.thumps
        throwSpider(&spider, from: RoomPoint(-1, 1.3, -0.5), to: RoomPoint(-1, 1.1, -0.5), over: 0.05, map: map)
        run(&spider, map, seconds: 4) { $0.mode == .crawling }
        XCTAssertEqual(spider.surface, "floor")
        XCTAssertGreaterThan(spider.thumps - before, 1)
    }

    func testPutDownGentlyAgainstTheWallItHoldsOn() {
        let map = room()
        var spider = landed(map, at: RoomPoint(0, 1, -1))
        let spot = RoomPoint(0.5, 1.5, -2 + RoomSpider.stand + 0.02)
        spider.grab(at: spot, time: 1)
        spider.drag(to: spot, time: 1.2)
        spider.release(time: 1.3, in: map)
        XCTAssertEqual(spider.mode, .crawling)
        XCTAssertEqual(spider.surface, "wall")
    }

    func testTappedSpotsOnTheWallAndBackOnTheFloorAreReached() {
        let map = room()
        var spider = landed(map, at: RoomPoint(-1, 1, -1))
        spider.come(to: RoomPoint(-0.5, 1.0, -2), on: "wall", in: map)
        run(&spider, map, seconds: 25) { $0.surface == "wall" && $0.goal == nil }
        XCTAssertEqual(spider.surface, "wall")
        XCTAssertEqual(spider.position.y, 1.0, accuracy: 0.1)
        spider.come(to: RoomPoint(-1, 0, -0.5), on: RoomMap.floorId, in: map)
        run(&spider, map, seconds: 25) { ($0.surface == RoomMap.floorId || $0.surface == "floor") && $0.goal == nil }
        XCTAssertTrue(spider.surface == RoomMap.floorId || spider.surface == "floor")
    }

    func testItLeapsOntoATappedTable() {
        let map = room()
        var spider = landed(map, at: RoomPoint(-0.5, 1, -1))
        spider.come(to: RoomPoint(1, 0.75, -1), on: "table", in: map)
        run(&spider, map, seconds: 20) { $0.surface == "table" }
        XCTAssertEqual(spider.surface, "table")
        XCTAssertEqual(spider.position.y, 0.75 + RoomSpider.stand, accuracy: 0.02)
    }

    func testATappedCeilingIsReachedUpAWallAndHungFrom() {
        let map = room()
        var spider = landed(map, at: RoomPoint(-1, 1, -1))
        spider.come(to: RoomPoint(0, 2.5, -1.5), on: "ceiling", in: map)
        var climbed = false
        run(&spider, map, seconds: 40) {
            if $0.surface == "wall" { climbed = true }
            return $0.surface == "ceiling"
        }
        XCTAssertTrue(climbed)
        XCTAssertEqual(spider.surface, "ceiling")
        XCTAssertEqual(spider.up, RoomPoint(0, -1, 0))
        XCTAssertEqual(spider.position.y, 2.5 - RoomSpider.stand, accuracy: 0.02)
    }

    func testWalkingOffTheTableItLowersItselfOnSilk() {
        let map = room()
        var spider = landed(map, at: RoomPoint(1, 1.3, -1))
        spider.come(to: RoomPoint(1, 0, 0), on: RoomMap.floorId, in: map)
        var sawSilk = false
        run(&spider, map, seconds: 30) {
            if $0.silkAnchor != nil { sawSilk = true }
            return $0.surface == RoomMap.floorId || $0.surface == "floor"
        }
        XCTAssertTrue(sawSilk)
        XCTAssertTrue(spider.surface == RoomMap.floorId || spider.surface == "floor")
    }

    func testALostPlaneDropsItToTheFloor() {
        var map = room()
        var spider = landed(map, at: RoomPoint(1, 1.3, -1))
        XCTAssertEqual(spider.surface, "table")
        map.remove("table")
        run(&spider, map, seconds: 5) { $0.mode == .crawling }
        XCTAssertTrue(spider.surface == RoomMap.floorId || spider.surface == "floor")
    }

    func testPokingMakesItGiggle() {
        let map = room()
        var spider = landed(map, at: RoomPoint(-1, 1, -1))
        spider.poke()
        XCTAssertEqual(spider.act, .giggle)
        XCTAssertEqual(spider.caption, RoomSpiderAct.giggle.line)
        run(&spider, map, seconds: 2)
        XCTAssertNotEqual(spider.act, .giggle)
    }

    func testEachKindOfThingHasItsOwnReaction() {
        XCTAssertEqual(RoomSpiderAct.arriving(on: .seat, upright: true), .nap)
        XCTAssertEqual(RoomSpiderAct.arriving(on: .table, upright: true), .dance)
        XCTAssertEqual(RoomSpiderAct.arriving(on: .window, upright: false), .peek)
        XCTAssertEqual(RoomSpiderAct.arriving(on: .door, upright: false), .knock)
        XCTAssertEqual(RoomSpiderAct.arriving(on: .ceiling, upright: false), .hang)
        XCTAssertNil(RoomSpiderAct.arriving(on: .floor, upright: true))
        for act in RoomSpiderAct.allCases {
            XCTAssertFalse(act.line.isEmpty)
            XCTAssertGreaterThan(act.seconds, 0)
        }
    }

    func testLeftAloneItExploresTheRoomAndNeverSinksThroughTheFloor() {
        let map = room()
        for seed in [UInt64(3), 11, 29] {
            var spider = landed(map, at: RoomPoint(0, 1, -1), seed: seed)
            var seen = Set<String>()
            for _ in 0..<(60 * 240) {
                spider.step(1.0 / 60.0, in: map)
                XCTAssertGreaterThanOrEqual(spider.position.y, map.floorY - 0.01)
                XCTAssertFalse(spider.position.x.isNaN || spider.position.y.isNaN || spider.position.z.isNaN)
                if let id = spider.surface { seen.insert(id) }
            }
            XCTAssertGreaterThan(seen.count, 2, "seed \(seed): \(seen)")
            XCTAssertLessThanOrEqual(spider.webs.count, RoomSpider.mostWebs)
        }
    }

    func testWebsAreSpunOnWallsAndCanBeCleared() {
        let map = room()
        var spider = landed(map, at: RoomPoint(0, 1, -1), seed: 5)
        var spun = false
        for _ in 0..<(60 * 600) {
            spider.step(1.0 / 60.0, in: map)
            if !spider.webs.isEmpty { spun = true; break }
        }
        XCTAssertTrue(spun)
        XCTAssertEqual(spider.webs.first?.surface, "wall")
        XCTAssertEqual(spider.webs.first?.normal, RoomPoint(0, 0, 1))
        spider.clearWebs()
        XCTAssertTrue(spider.webs.isEmpty)
    }
}
