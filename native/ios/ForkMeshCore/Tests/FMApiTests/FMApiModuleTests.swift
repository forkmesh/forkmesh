import XCTest
@testable import FMApi

final class FMApiModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMApiModule.name, "FMApi")
    }
}
