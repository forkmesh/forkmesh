import XCTest
@testable import FMCrypto

final class FMCryptoModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMCryptoModule.name, "FMCrypto")
    }
}
