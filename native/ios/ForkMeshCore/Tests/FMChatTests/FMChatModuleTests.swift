import XCTest
@testable import FMChat

final class FMChatModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMChatModule.name, "FMChat")
    }
}
