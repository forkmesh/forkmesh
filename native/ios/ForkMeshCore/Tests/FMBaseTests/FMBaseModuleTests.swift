import XCTest
@testable import FMBase

final class FMBaseModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMBaseModule.name, "FMBase")
    }
}
