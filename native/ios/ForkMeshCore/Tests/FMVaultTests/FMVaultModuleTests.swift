import XCTest
@testable import FMVault

final class FMVaultModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMVaultModule.name, "FMVault")
    }
}
