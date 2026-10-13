import XCTest
@testable import FMCode

final class FMCodeModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMCodeModule.name, "FMCode")
    }
}
