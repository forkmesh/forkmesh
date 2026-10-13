import XCTest

@testable import FMDiscord

final class FMDiscordModuleTests: XCTestCase {
    func testModuleIsOwned() {
        XCTAssertEqual(FMDiscordModule.owner, "WS13")
    }
}
