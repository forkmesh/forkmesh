import FMTools
import Foundation
import XCTest

final class GlassesWatchTests: XCTestCase {
    private let start: Date = Date(timeIntervalSince1970: 1_800_000_000)

    private func advert(_ id: String = "A", name: String? = nil, rssi: Int = -60, ids: [Int] = []) -> GlassesAdvert {
        GlassesAdvert(identifier: id, name: name, rssi: rssi, companyIds: ids)
    }

    private func sighting(_ id: String, at seconds: TimeInterval, rssi: Int = -60) -> GlassesSighting {
        GlassesSighting(id: id, label: "x", maker: .meta, namedAsGlasses: false, rssi: rssi, seenAt: start.addingTimeInterval(seconds))
    }

    func testReadsTheLittleEndianCompanyId() {
        XCTAssertEqual(GlassesSignatures.companyId(manufacturerData: [0x53, 0x0D, 0x01]), 0x0D53)
        XCTAssertEqual(GlassesSignatures.companyId(manufacturerData: [0xAB, 0x01]), 0x01AB)
        XCTAssertNil(GlassesSignatures.companyId(manufacturerData: [0x53]))
    }

    func testMatchesEveryMetaCompanyId() {
        for maker in GlassesMaker.allCases {
            XCTAssertEqual(GlassesSignatures.match(advert(ids: [maker.companyId]), range: .room, now: start)?.maker, maker)
        }
        XCTAssertEqual(GlassesSignatures.companyIds, [0x01AB, 0x058E, 0x0D53])
    }

    func testIgnoresOtherMakersAndUnknownSignal() {
        XCTAssertNil(GlassesSignatures.match(advert(name: "AirPods", ids: [0x004C]), range: .wide, now: start))
        XCTAssertNil(GlassesSignatures.match(advert(rssi: 127, ids: [0x0D53]), range: .wide, now: start))
    }

    func testMatchesGlassesNamesWithoutACompanyId() {
        let found: GlassesSighting? = GlassesSignatures.match(advert(name: " Ray-Ban Meta 0F2A "), range: .room, now: start)
        XCTAssertEqual(found?.label, "Ray-Ban Meta 0F2A")
        XCTAssertEqual(found?.namedAsGlasses, true)
        XCTAssertTrue(GlassesSignatures.nameMatches("RAYBAN STORIES"))
        XCTAssertFalse(GlassesSignatures.nameMatches(""))
    }

    func testLabels() {
        XCTAssertEqual(GlassesSignatures.match(advert(ids: [0x0D53]), range: .room, now: start)?.label, "Ray-Ban or Oakley Meta glasses")
        XCTAssertEqual(GlassesSignatures.match(advert(ids: [0x058E]), range: .room, now: start)?.label, "Meta glasses or headset")
    }

    func testRangeDropsWeakSignals() {
        let far: GlassesAdvert = advert(rssi: -80, ids: [0x0D53])
        XCTAssertNil(GlassesSignatures.match(far, range: .room, now: start))
        XCTAssertNotNil(GlassesSignatures.match(far, range: .wide, now: start))
        XCTAssertEqual(GlassesRange.of("bogus"), .room)
        XCTAssertEqual(GlassesRange.of("near"), .near)
    }

    func testDistanceWords() {
        XCTAssertEqual(GlassesSignatures.distance(-55), "Very close")
        XCTAssertEqual(GlassesSignatures.distance(-68), "Close")
        XCTAssertEqual(GlassesSignatures.distance(-85), "In the area")
    }

    func testTrackerAlertsOncePerDeviceAndHonoursTheCooldown() {
        var tracker: GlassesTracker = GlassesTracker(forgetAfter: 60, cooldown: 20)
        XCTAssertTrue(tracker.observe(sighting("A", at: 0)))
        XCTAssertFalse(tracker.observe(sighting("A", at: 1)))
        XCTAssertFalse(tracker.observe(sighting("B", at: 5)))
        XCTAssertTrue(tracker.observe(sighting("C", at: 25)))
        XCTAssertEqual(tracker.nearby(start.addingTimeInterval(30)).map(\.id).sorted(), ["A", "B", "C"])
        XCTAssertEqual(tracker.nearby(start.addingTimeInterval(80)).map(\.id), ["C"])
        XCTAssertTrue(tracker.observe(sighting("A", at: 100)))
        tracker.reset()
        XCTAssertTrue(tracker.nearby(start.addingTimeInterval(100)).isEmpty)
    }

    func testNearbyListsStrongestFirst() {
        var tracker: GlassesTracker = GlassesTracker()
        _ = tracker.observe(sighting("A", at: 0, rssi: -80))
        _ = tracker.observe(sighting("B", at: 0, rssi: -50))
        XCTAssertEqual(tracker.nearby(start).map(\.id), ["B", "A"])
    }
}
