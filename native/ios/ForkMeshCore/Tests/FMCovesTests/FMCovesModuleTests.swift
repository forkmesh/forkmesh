import XCTest

@testable import FMCoves

final class FMCovesModuleTests: XCTestCase {
    func testModuleIsOwned() {
        XCTAssertEqual(FMCovesModule.owner, "WS21")
    }
}
