import FMTools
import Foundation
import XCTest

final class GlassesHomingTests: XCTestCase {
    private let start: Date = Date(timeIntervalSince1970: 1_800_000_000)

    private func at(_ seconds: TimeInterval) -> Date { start.addingTimeInterval(seconds) }

    private func seen(_ id: String, _ seconds: TimeInterval, rssi: Int = -60) -> GlassesSighting {
        GlassesSighting(id: id, label: "Glasses \(id)", maker: .luxottica, namedAsGlasses: false, rssi: rssi, seenAt: at(seconds))
    }

    private func near(_ a: Double, _ b: Double, within: Double) -> Bool {
        abs(GlassesHoming.unwrap(a, b) - a) <= within
    }

    private func device(_ id: String, _ name: String?, _ rssi: Int, _ seconds: TimeInterval, _ ids: [Int] = []) -> NearbyDevice? {
        NearbyKinds.device(GlassesAdvert(identifier: id, name: name, rssi: rssi, companyIds: ids), now: at(seconds))
    }

    func testFindsTheDirectionOfTheStrongestSignal() throws {
        var homing: GlassesHoming = GlassesHoming()
        homing.follow([seen("A", 0)], now: at(0))
        var t: TimeInterval = 0
        for step in 0..<120 {
            let heading: Double = Double(step) * 3
            let rssi: Int = -70 + Int((10 * cos((heading - 100) * .pi / 180)).rounded())
            homing.hear("A", rssi: rssi, heading: heading, now: at(t))
            t += 0.1
        }
        let reading: GlassesHomingReading = try XCTUnwrap(homing.reading(at(t)))
        let bearing: Double = try XCTUnwrap(reading.bearing)
        XCTAssertTrue(near(bearing, 100, within: 15), "bearing \(bearing)")
        XCTAssertGreaterThanOrEqual(reading.confidence, 0.5)
        XCTAssertTrue(near(try XCTUnwrap(reading.arrow(heading: 10)), 90, within: 15))
        XCTAssertEqual(reading.hint, "Strongest signal this way.")
        XCTAssertTrue(reading.live)
    }

    func testNoDirectionUntilEnoughHeadingsAreHeard() {
        var homing: GlassesHoming = GlassesHoming()
        homing.follow([seen("A", 0)], now: at(0))
        for i in 0..<10 {
            homing.hear("A", rssi: -60, heading: 45, now: at(Double(i) * 0.1))
        }
        let reading: GlassesHomingReading? = homing.reading(at(1))
        XCTAssertNil(reading?.bearing)
        XCTAssertEqual(reading?.hint, "Hold the phone in front of you and turn slowly to find the direction.")
        var blind: GlassesHoming = GlassesHoming()
        blind.follow([seen("A", 0)], now: at(0))
        blind.hear("A", rssi: -60, heading: nil, now: at(0))
        XCTAssertEqual(blind.reading(at(0.01))?.hint, "No compass reading, so only closeness is shown.")
    }

    func testTrendFollowsTheSignal() {
        var homing: GlassesHoming = GlassesHoming()
        homing.follow([seen("A", 0)], now: at(0))
        for i in 0...10 { homing.hear("A", rssi: -80, heading: nil, now: at(Double(i) * 0.5)) }
        for i in 11...20 { homing.hear("A", rssi: -62, heading: nil, now: at(Double(i) * 0.5)) }
        XCTAssertEqual(homing.reading(at(10))?.trend, .closer)
        var away: GlassesHoming = GlassesHoming()
        away.follow([seen("A", 0)], now: at(0))
        for i in 0...10 { away.hear("A", rssi: -55, heading: nil, now: at(Double(i) * 0.5)) }
        for i in 11...20 { away.hear("A", rssi: -75, heading: nil, now: at(Double(i) * 0.5)) }
        XCTAssertEqual(away.reading(at(10))?.trend, .farther)
    }

    func testClosenessAndLiveness() {
        XCTAssertEqual(GlassesHoming.closeness(-100), 0)
        XCTAssertEqual(GlassesHoming.closeness(-40), 1)
        XCTAssertEqual(GlassesHoming.closeness(-70), 0.5)
        var homing: GlassesHoming = GlassesHoming(liveFor: 5)
        homing.follow([seen("A", 0)], now: at(0))
        homing.hear("A", rssi: -64, heading: nil, now: at(0))
        XCTAssertEqual(homing.reading(at(4))?.live, true)
        let stale: GlassesHomingReading? = homing.reading(at(6))
        XCTAssertEqual(stale?.live, false)
        XCTAssertEqual(stale?.hint, "Signal lost. Walk around to pick it up again.")
    }

    func testSticksToItsTargetAndIgnoresOthers() {
        var homing: GlassesHoming = GlassesHoming(sticky: 10)
        homing.follow([seen("A", 0, rssi: -70)], now: at(0))
        homing.hear("B", rssi: -40, heading: 0, now: at(0.1))
        XCTAssertEqual(homing.reading(at(0.2))?.rssi, -70)
        homing.follow([seen("B", 1, rssi: -50), seen("A", 0.9, rssi: -70)], now: at(1))
        XCTAssertEqual(homing.target?.id, "A")
        homing.follow([seen("B", 14, rssi: -50), seen("A", 0.9, rssi: -70)], now: at(14))
        XCTAssertEqual(homing.target?.id, "B")
        homing.follow([], now: at(15))
        XCTAssertNil(homing.reading(at(15)))
    }

    func testAngleHelpers() throws {
        XCTAssertEqual(GlassesHoming.normalize(-10), 350)
        XCTAssertEqual(GlassesHoming.unwrap(350, 10), 370)
        XCTAssertEqual(GlassesHoming.unwrap(0, 350), -10)
        XCTAssertEqual(GlassesHoming.clock(355), "at 12 o'clock")
        XCTAssertEqual(GlassesHoming.clock(92), "at 3 o'clock")
        XCTAssertEqual(GlassesHoming.clock(-90), "at 9 o'clock")
        XCTAssertTrue(near(GlassesHoming.smoothHeading(350, 10, factor: 0.5), 0, within: 0.01))
        XCTAssertTrue(near(try XCTUnwrap(GlassesHoming.heading(rotation: [1, 0, 0, 0, 1, 0, 0, 0, 1])), 0, within: 0.01))
        XCTAssertTrue(near(try XCTUnwrap(GlassesHoming.heading(rotation: [0, 0, -1, -1, 0, 0, 0, 1, 0])), 90, within: 0.01))
        let flatNorth: [Double] = try XCTUnwrap(GlassesHoming.rotation(up: [0, 0, 1], magnetic: [0, 20, -40]))
        XCTAssertTrue(near(try XCTUnwrap(GlassesHoming.heading(rotation: flatNorth)), 0, within: 0.01))
        let flatEast: [Double] = try XCTUnwrap(GlassesHoming.rotation(up: [0, 0, 1], magnetic: [-20, 0, -40]))
        XCTAssertTrue(near(try XCTUnwrap(GlassesHoming.heading(rotation: flatEast)), 90, within: 0.01))
    }

    func testSortsDevicesIntoKinds() {
        XCTAssertEqual(NearbyKinds.kind(name: "Galaxy Buds2 Pro", companyIds: []), .headphones)
        XCTAssertEqual(NearbyKinds.kind(name: "AirPods Pro", companyIds: [0x004C]), .headphones)
        XCTAssertEqual(NearbyKinds.kind(name: "Pixel Watch 2", companyIds: []), .watch)
        XCTAssertEqual(NearbyKinds.kind(name: "[TV] Samsung Q80", companyIds: []), .tv)
        XCTAssertEqual(NearbyKinds.kind(name: nil, companyIds: [0x004C]), .phone)
        XCTAssertEqual(NearbyKinds.kind(name: nil, companyIds: [0x00D2]), .tag)
        XCTAssertEqual(NearbyKinds.kind(name: "Govee_H6008", companyIds: []), .light)
        XCTAssertEqual(NearbyKinds.kind(name: "GBK_H6159", companyIds: []), .light)
        XCTAssertEqual(NearbyKinds.kind(name: "Vertuo_DV4", companyIds: []), .appliance)
        XCTAssertEqual(NearbyKinds.kind(name: "tcl_B14S5", companyIds: []), .tv)
        XCTAssertEqual(NearbyKinds.kind(name: "ZX-41", companyIds: [0x1234]), .other)
        XCTAssertEqual(NearbyKinds.maker([0x004C]), "Apple")
        XCTAssertEqual(device("A", " Kitchen speaker ", -60, 0)?.label, "Kitchen speaker")
        XCTAssertEqual(device("B", nil, -60, 0, [0x004C])?.label, "Apple device")
        XCTAssertEqual(device("C", "", -60, 0)?.label, "Bluetooth device")
        XCTAssertNil(device("D", nil, 127, 0))
    }

    func testSortsDevicesByNameAppearanceServiceAndMaker() {
        XCTAssertEqual(NearbyKinds.kind(name: "Echo Frames", companyIds: []), .glasses)
        XCTAssertEqual(NearbyKinds.kind(name: "Phonak Audeo", companyIds: []), .hearingAid)
        XCTAssertEqual(NearbyKinds.kind(name: "Wireless Controller", companyIds: []), .gaming)
        XCTAssertEqual(NearbyKinds.kind(name: "GoPro 1234", companyIds: []), .camera)
        XCTAssertEqual(NearbyKinds.kind(name: "Mavic Air 2", companyIds: []), .drone)
        XCTAssertEqual(NearbyKinds.kind(name: "Polar H10 1A2B", companyIds: []), .fitness)
        XCTAssertEqual(NearbyKinds.kind(name: "Yale Lock", companyIds: []), .lock)
        XCTAssertEqual(NearbyKinds.kind(name: "SwitchBot Meter", companyIds: []), .smartHome)
        XCTAssertEqual(NearbyKinds.kind(name: "Ninebot Max", companyIds: []), .ride)
        XCTAssertEqual(NearbyKinds.kind(name: "Galaxy Tab S9", companyIds: []), .tablet)
        XCTAssertEqual(NearbyKinds.kind(name: "LE-Bose Revolve+", companyIds: []), .speaker)
        XCTAssertEqual(NearbyKinds.kind(name: "Brother PT-P710", companyIds: []), .printer)
        XCTAssertEqual(NearbyKinds.kind(name: "RENPHO", companyIds: []), .scale)
        XCTAssertEqual(NearbyKinds.kind(name: "Dexcom G7", companyIds: []), .health)
        XCTAssertEqual(NearbyKinds.kind(name: "Sonicare", companyIds: []), .appliance)
        XCTAssertEqual(NearbyKinds.kind(name: "Sam's iPhone", companyIds: []), .phone)
        XCTAssertEqual(NearbyKinds.kind(name: "Bose QC45", companyIds: []), .headphones)
        XCTAssertEqual(NearbyKinds.kind(name: nil, companyIds: [], appearance: 0x00C1), .watch)
        XCTAssertEqual(NearbyKinds.kind(name: nil, companyIds: [], appearance: 0x03C4), .gaming)
        XCTAssertEqual(NearbyKinds.kind(name: nil, companyIds: [], appearance: 0x0941), .headphones)
        XCTAssertEqual(NearbyKinds.kind(name: nil, companyIds: [0x004C], services: [0x180D]), .fitness)
        XCTAssertEqual(NearbyKinds.kind(name: nil, companyIds: [], services: [0xFD6F]), .phone)
        XCTAssertEqual(NearbyKinds.kind(name: nil, companyIds: [0x01DA]), .input)
        XCTAssertEqual(NearbyKinds.shortUuid("180D"), 0x180D)
        XCTAssertEqual(NearbyKinds.shortUuid("0000feaa-0000-1000-8000-00805f9b34fb"), 0xFEAA)
        XCTAssertNil(NearbyKinds.shortUuid("6E400001-B5A3-F393-E0A9-E50E24DCCA9E"))
        var census: GlassesCensus = GlassesCensus()
        census.observe(NearbyDevice(id: "A", name: nil, maker: nil, kind: .other, rssi: -60, seenAt: at(0)))
        census.observe(NearbyDevice(id: "A", name: nil, maker: nil, kind: .fitness, rssi: -60, seenAt: at(1)))
        XCTAssertEqual(census.devices(at(2)).first?.kind, .fitness)
    }

    func testCensusKeepsNamesForgetsOldDevicesAndSortsByStrength() throws {
        var census: GlassesCensus = GlassesCensus(forgetAfter: 10, limit: 2)
        census.observe(try XCTUnwrap(device("A", "Pixel 8", -70, 0)))
        census.observe(try XCTUnwrap(device("A", nil, -65, 1)))
        census.observe(try XCTUnwrap(device("B", nil, -50, 2, [0x004C])))
        census.observe(try XCTUnwrap(device("C", nil, -90, 3)))
        let now: [NearbyDevice] = census.devices(at(4))
        XCTAssertEqual(now.map(\.id), ["B", "A"])
        XCTAssertEqual(now[1].name, "Pixel 8")
        XCTAssertEqual(now[1].kind, .phone)
        XCTAssertEqual(now[1].rssi, -68)
        XCTAssertEqual(census.devices(at(12)).map(\.id), ["B", "C"])
        census.reset()
        XCTAssertEqual(census.devices(at(12)).count, 0)
    }

    func testCensusKeepsDevicesForFortyFiveSecondsByDefault() throws {
        var census: GlassesCensus = GlassesCensus()
        census.observe(try XCTUnwrap(device("A", "Pixel 8", -70, 0)))
        XCTAssertEqual(census.devices(at(45)).map(\.id), ["A"])
        XCTAssertTrue(census.devices(at(45.001)).isEmpty)
    }

    func testRadarPlacesDevicesByStrengthAndGlassesByDirection() throws {
        XCTAssertEqual(GlassesRadarLayout.angle("AA:BB:CC:DD:EE:FF"), 81.5, accuracy: 0.001)
        XCTAssertEqual(GlassesRadarLayout.distance(-40), 0.2, accuracy: 0.001)
        XCTAssertEqual(GlassesRadarLayout.distance(-70), 0.5, accuracy: 0.001)
        XCTAssertEqual(GlassesRadarLayout.distance(-100), 0.92, accuracy: 0.001)
        let others: [NearbyDevice] = [try XCTUnwrap(device("A", "Pixel 8", -70, 0))]
        let sighting: GlassesSighting = GlassesSighting(id: "G", label: "Ray-Ban", maker: nil, namedAsGlasses: true, rssi: -80, seenAt: at(0))
        let homing: GlassesHomingReading = GlassesHomingReading(
            id: "G", label: "Ray-Ban", rssi: -60, closeness: 0.7, trend: nil, bearing: 100, confidence: 1, live: true, compass: true)
        let blips: [GlassesRadarBlip] = GlassesRadarLayout.blips(others: others, glasses: [sighting], homing: homing, heading: 40)
        XCTAssertEqual(blips.map(\.id), ["A", "G"])
        XCTAssertEqual(blips[0].angle, GlassesRadarLayout.angle("A"))
        XCTAssertFalse(blips[0].glasses)
        XCTAssertTrue(blips[1].glasses)
        XCTAssertEqual(blips[1].angle, 60, accuracy: 0.001)
        XCTAssertEqual(blips[1].distance, 0.3, accuracy: 0.001)
        let unknown: [GlassesRadarBlip] = GlassesRadarLayout.blips(others: [], glasses: [sighting], homing: homing, heading: nil)
        XCTAssertEqual(unknown[0].angle, GlassesRadarLayout.angle("G"))
    }
}
