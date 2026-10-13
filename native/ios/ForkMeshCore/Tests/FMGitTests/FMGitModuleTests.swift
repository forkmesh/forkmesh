import XCTest
@testable import FMGit

final class FMGitModuleTests: XCTestCase {
    func testModuleIsNamed() {
        XCTAssertEqual(FMGitModule.name, "FMGit")
    }
}
