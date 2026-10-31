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

    func testMatchesEveryMakerByCompanyIdOrService() {
        for maker in GlassesMaker.allCases {
            let found: GlassesSighting? =
                if let id = maker.companyId {
                    GlassesSignatures.match(advert(ids: [id]), range: .room, now: start)
                } else {
                    GlassesSignatures.match(
                        GlassesAdvert(identifier: "A", name: nil, rssi: -60, companyIds: [], serviceUuids: maker.services), range: .room,
                        now: start)
                }
            XCTAssertEqual(found?.maker, maker)
            XCTAssertEqual(found?.kind, maker.kind)
            XCTAssertEqual(found?.label, maker.shown)
        }
        XCTAssertTrue(GlassesSignatures.companyIds.isSuperset(of: [0x01AB, 0x058E, 0x0D53]))
        XCTAssertFalse(GlassesSignatures.companyIds.contains(0x012D))
    }

    func testMatchesCamerasByCompanyIdAndService() {
        let gopro: GlassesSighting? = GlassesSignatures.match(advert(ids: [0x02F2]), range: .room, now: start)
        XCTAssertEqual(gopro?.kind, .camera)
        XCTAssertEqual(gopro?.label, "GoPro camera")
        XCTAssertEqual(gopro?.kind.title, "Camera nearby")
        XCTAssertEqual(GlassesSignatures.match(advert(ids: [0x0D53]), range: .room, now: start)?.kind, .glasses)
        let axon: GlassesSighting? = GlassesSignatures.match(
            GlassesAdvert(identifier: "B", name: nil, rssi: -60, companyIds: [], serviceUuids: [0xFC81]), range: .room, now: start)
        XCTAssertEqual(axon?.maker, .axon)
        XCTAssertEqual(axon?.label, "Axon body camera")
    }

    func testMatchesCameraNames() {
        for name in ["GoPro 1234", "Insta360 X4", "ILCE-7M4", "Canon EOS R6", "NIKON Z 6_2", "Nextbase 622GW", "OSMO ACTION 4", "Mavic 3"] {
            XCTAssertEqual(GlassesSignatures.nameKind(name), .camera, name)
        }
        let named: GlassesSighting? = GlassesSignatures.match(advert(name: " ZV-E10 "), range: .room, now: start)
        XCTAssertEqual(named?.label, "ZV-E10")
        XCTAssertEqual(named?.kind, .camera)
        XCTAssertEqual(GlassesSignatures.nameKind("Ray-Ban Meta"), .glasses)
        XCTAssertNil(GlassesSignatures.nameKind("Cosmo watch"))
        XCTAssertNil(GlassesSignatures.nameKind("Carlos iPhone"))
    }

    func testCameraMakerPrintersStayQuiet() {
        XCTAssertNil(GlassesSignatures.match(advert(name: "Canon TS8300 series PIXMA", ids: [0x01A9]), range: .room, now: start))
        XCTAssertNil(GlassesSignatures.match(advert(name: "INSTAX-Link 1234 instax mini Link"), range: .room, now: start))
        XCTAssertNil(GlassesSignatures.kind(advert(name: "Canon SELPHY CP1500", ids: [0x01A9])))
        XCTAssertEqual(GlassesSignatures.kind(advert(name: "Canon EOS R5", ids: [0x01A9])), .camera)
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
