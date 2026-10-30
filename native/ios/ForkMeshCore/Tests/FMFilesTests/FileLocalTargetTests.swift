import Foundation
import XCTest
@testable import FMFiles

final class FileLocalTargetTests: XCTestCase {
    private let folder = URL(fileURLWithPath: "/tmp/fm-files-open-test/files-open-1", isDirectory: true)

    private func entry(_ name: String) -> FileEntry {
        FileEntry(name: name, path: "share/\(name)", isFolder: false)
    }

    func testAServerListedNameCannotLeaveTheDownloadFolder() throws {
        for name in ["../../Documents/x", "a/../../b", "/etc/passwd", "sub/report.pdf"] {
            let target = try entry(name).localTarget(in: folder)
            XCTAssertEqual(target.deletingLastPathComponent().path, folder.standardizedFileURL.path, name)
            XCTAssertFalse(target.lastPathComponent.contains("/"), name)
        }
        XCTAssertEqual(try entry("../../Documents/x").localTarget(in: folder).lastPathComponent, "x")
    }

    func testDotNamesAndEmptyNamesAreRefused() {
        for name in ["", ".", "..", "../..", "a/.."] {
            XCTAssertNil(entry(name).localName, name)
            XCTAssertThrowsError(try entry(name).localTarget(in: folder), name)
        }
    }

    func testOrdinaryNamesKeepTheirName() throws {
        for name in ["report.pdf", ".env", "My File (1).txt"] {
            XCTAssertEqual(try entry(name).localTarget(in: folder).lastPathComponent, name)
        }
    }
}
