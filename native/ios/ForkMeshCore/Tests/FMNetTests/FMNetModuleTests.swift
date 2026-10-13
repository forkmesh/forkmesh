import XCTest
@testable import FMNet

final class FMNetModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMNetModule.name, "FMNet")
    }
}
