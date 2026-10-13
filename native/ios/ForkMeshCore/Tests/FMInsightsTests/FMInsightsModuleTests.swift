import XCTest

@testable import FMInsights

final class FMInsightsModuleTests: XCTestCase {
    func testModuleIsOwned() {
        XCTAssertEqual(FMInsightsModule.owner, "WS20")
    }
}
