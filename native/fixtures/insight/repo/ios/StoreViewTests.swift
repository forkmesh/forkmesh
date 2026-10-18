import XCTest

final class StoreViewTests: XCTestCase {
    func testLoad() {
        XCTAssertTrue(MemoryStore().load().isEmpty)
    }
}
