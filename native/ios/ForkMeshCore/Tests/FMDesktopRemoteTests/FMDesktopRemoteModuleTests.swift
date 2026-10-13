import XCTest

@testable import FMDesktopRemote

final class FMDesktopRemoteModuleTests: XCTestCase {
    func testModuleIsOwned() {
        XCTAssertEqual(FMDesktopRemoteModule.owner, "WS16")
    }
}
