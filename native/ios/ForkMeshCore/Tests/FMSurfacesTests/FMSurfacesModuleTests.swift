import XCTest

@testable import FMSurfaces

final class FMSurfacesModuleTests: XCTestCase {
    func testModuleIsOwned() {
        XCTAssertEqual(FMSurfacesModule.owner, "WS12")
    }
}
