import XCTest

@testable import FMTeammates

final class FMTeammatesModuleTests: XCTestCase {
    func testModuleIsOwned() {
        XCTAssertEqual(FMTeammatesModule.owner, "WS17")
    }
}
