import XCTest
@testable import FMAgents

final class FMAgentsModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMAgentsModule.name, "FMAgents")
    }
}
