import XCTest
@testable import FMDiff

final class FMDiffModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMDiffModule.name, "FMDiff")
    }
}
