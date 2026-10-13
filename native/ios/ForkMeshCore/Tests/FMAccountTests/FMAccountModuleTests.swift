import XCTest
@testable import FMAccount

final class FMAccountModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMAccountModule.name, "FMAccount")
    }
}
