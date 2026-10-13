import XCTest

@testable import FMPackages

final class FMPackagesModuleTests: XCTestCase {
    func testModuleIsOwned() {
        XCTAssertEqual(FMPackagesModule.owner, "WS19")
    }
}
