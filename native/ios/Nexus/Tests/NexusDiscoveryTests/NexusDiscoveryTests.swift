import XCTest
@testable import NexusDiscovery
@testable import NexusCore

final class NexusDiscoveryTests: XCTestCase {
    func testInventoryMergesBonjourServices() {
        var inventory = DeviceInventory()
        inventory.merge(service: BonjourService(name: "Printer", type: "_ipp._tcp", hostName: "printer.local"))
        XCTAssertEqual(inventory.devices.count, 1)
    }
}
