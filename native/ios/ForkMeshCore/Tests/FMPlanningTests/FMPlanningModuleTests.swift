import XCTest

@testable import FMPlanning

final class FMPlanningModuleTests: XCTestCase {
    func testModuleIsOwned() {
        XCTAssertEqual(FMPlanningModule.owner, "WS15")
    }
}
