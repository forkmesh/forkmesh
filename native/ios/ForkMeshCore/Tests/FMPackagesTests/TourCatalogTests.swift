import FMApi
import XCTest

@testable import FMPackages

final class TourCatalogTests: XCTestCase {
    func testTracksNest() {
        let essentials: [String] = TourCatalog.stops(for: .essentials).map(\.id)
        let advanced: [String] = TourCatalog.stops(for: .advanced).map(\.id)
        let everything: [String] = TourCatalog.stops(for: .everything).map(\.id)
        XCTAssertEqual(essentials, ["home", "agents", "newAgent", "inbox", "hub"])
        XCTAssertEqual(Array(advanced.prefix(essentials.count)), essentials)
        XCTAssertEqual(Array(everything.prefix(advanced.count)), advanced)
        XCTAssertEqual(advanced.count, 10)
        XCTAssertEqual(everything.count, TourScene.allCases.count)
    }

    func testEveryStopIsWritten() {
        XCTAssertEqual(Set(TourCatalog.stops.map(\.id)).count, TourCatalog.stops.count)
        for stop in TourCatalog.stops {
            XCTAssertFalse(stop.title.isEmpty, stop.id)
            XCTAssertFalse(stop.message.isEmpty, stop.id)
            XCTAssertFalse(stop.tryIt.isEmpty, stop.id)
            XCTAssertFalse(stop.symbol.isEmpty, stop.id)
            XCTAssertEqual(stop.tips.count, 2, stop.id)
            XCTAssertEqual(stop.jump == nil, stop.jumpLabel.isEmpty, stop.id)
            XCTAssertLessThanOrEqual(stop.message.count, 200, stop.id)
        }
    }

    func testMissingTabDropsItsStop() {
        let installed: Set<String> = ["inbox", "code", "chat"]
        let ids: [String] = TourCatalog.stops(for: .everything, installed: installed.contains).map(\.id)
        XCTAssertFalse(ids.contains("agents"))
        XCTAssertFalse(ids.contains("newAgent"))
        XCTAssertFalse(ids.contains("voice"))
        XCTAssertTrue(ids.contains("home"))
        XCTAssertTrue(ids.contains("inbox"))
    }

    func testMissingPluginPointsAtTheMarket() {
        let starter: Set<String> = PackageGate.starterPackages
        let stops: [TourStop] = TourCatalog.stops(for: .everything, installed: starter.contains)
        let workspace: TourStop = try! XCTUnwrap(stops.first { $0.scene == .workspace })
        XCTAssertTrue(workspace.fromMarket)
        XCTAssertEqual(workspace.jump, .route(.market(filter: nil)))
        XCTAssertEqual(workspace.jumpLabel, "Find it in the Market")
        let chat: TourStop = try! XCTUnwrap(stops.first { $0.scene == .chat })
        XCTAssertFalse(chat.fromMarket)
        XCTAssertEqual(chat.jump, .tab(.chat))
    }

    func testOnePluginIsEnough() {
        let stops: [TourStop] = TourCatalog.stops(for: .everything, installed: { $0 == "tasks" })
        let workspace: TourStop = try! XCTUnwrap(stops.first { $0.scene == .workspace })
        XCTAssertFalse(workspace.fromMarket)
        XCTAssertEqual(workspace.jump, .route(.notes))
    }

    func testMinutes() {
        XCTAssertEqual(TourCatalog.minutes(for: 0), 1)
        XCTAssertEqual(TourCatalog.minutes(for: 5), 2)
        XCTAssertEqual(TourCatalog.minutes(for: 18), 6)
    }

    func testTrackIncludes() {
        XCTAssertTrue(TourTrack.everything.includes(.essentials))
        XCTAssertTrue(TourTrack.advanced.includes(.advanced))
        XCTAssertFalse(TourTrack.essentials.includes(.advanced))
        XCTAssertEqual(Set(TourTrack.allCases.map(\.title)).count, 3)
    }

    func testPlaceRoundTrips() {
        let place: TourPlace = TourPlace(track: .advanced, stop: "code")
        XCTAssertEqual(place.encoded, "advanced:code")
        XCTAssertEqual(TourPlace(encoded: place.encoded), place)
        XCTAssertNil(TourPlace(encoded: "advanced"))
        XCTAssertNil(TourPlace(encoded: "nope:code"))
        XCTAssertNil(TourPlace(encoded: "advanced:"))
        XCTAssertEqual(TourCatalog.index(of: place, in: TourCatalog.stops(for: .advanced)), 6)
        XCTAssertNil(TourCatalog.index(of: place, in: TourCatalog.stops(for: .essentials)))
    }
}
