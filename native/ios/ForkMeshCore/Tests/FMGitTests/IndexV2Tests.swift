import XCTest
@testable import FMGit

final class IndexV2Tests: GitTestCase {
    func testWritesAndReadsChecksummedVersion2Index() throws {
        let bytes = try GitIndexV2([
            try GitIndexEntry(path: utf8("hello.txt"), objectId: fixtureId(2), mode: .regular, size: 6),
            try GitIndexEntry(path: utf8("bin/run"), objectId: fixtureId(1), mode: .executable, size: 12),
        ]).encode()
        let index = try GitIndexV2.parse(bytes, .standard)
        XCTAssertEqual(index.entries.map(\.pathText), ["bin/run", "hello.txt"])
        XCTAssertEqual(index.entries.first?.mode, .executable)
        XCTAssertEqual(index.entries.first?.size, 12)
        XCTAssertEqual(index.entries.last?.objectId, fixtureId(2))
    }

    func testRejectsIndexWithChangedChecksumByte() throws {
        var bytes = try GitIndexV2([
            try GitIndexEntry(path: utf8("hello.txt"), objectId: fixtureId(2), mode: .regular),
        ]).encode()
        bytes[bytes.count - 1] ^= 0x01
        assertGitError(.indexCorrupt) { _ = try GitIndexV2.parse(bytes, .standard) }
    }

    func testRejectsTraversalPathAndNonZeroStageEntry() {
        assertGitError(.pathEscapesWorktree) {
            _ = try GitIndexEntry(path: utf8("../outside"), objectId: fixtureId(3), mode: .regular)
        }
        assertGitError(.pathEscapesWorktree) {
            _ = try GitIndexEntry(path: [0x61, 0xff], objectId: fixtureId(3), mode: .regular)
        }
        assertGitError(.unsupportedRepositoryFormat) {
            _ = try GitIndexEntry(path: utf8("conflict.txt"), objectId: fixtureId(3), mode: .regular, stage: 1)
        }
        assertGitError(.indexCorrupt) {
            _ = try GitIndexV2([
                try GitIndexEntry(path: utf8("a"), objectId: fixtureId(1), mode: .regular),
                try GitIndexEntry(path: utf8("a"), objectId: fixtureId(2), mode: .regular),
            ])
        }
    }

    func testIndexStoreRoundTripAndLock() throws {
        let root = try makeTemp("forkmesh-index-store-")
        let store = GitIndexStore(root, .standard)
        XCTAssertTrue(try store.read().entries.isEmpty)
        let index = try GitIndexV2([try GitIndexEntry(path: utf8("a"), objectId: fixtureId(1), mode: .regular)])
        try store.write(index)
        XCTAssertEqual(try store.read().entries.count, 1)
        try Data().write(to: URL(fileURLWithPath: store.file + ".lock"))
        assertGitError(.refLocked) { try store.write(index) }
    }

    func testReadsAnIndexWrittenByGit() throws {
        guard let git = GitBinary.locate() else { throw XCTSkip("git is not installed") }
        let root = try makeTemp("forkmesh-index-interop-")
        try git.run(["init", "--quiet", root])
        try FileManager.default.createDirectory(atPath: root + "/bin", withIntermediateDirectories: true)
        try Data(utf8("hello\n")).write(to: URL(fileURLWithPath: root + "/hello.txt"))
        try Data(utf8("#!/bin/sh\n")).write(to: URL(fileURLWithPath: root + "/bin/run"))
        try FileManager.default.setAttributes([.posixPermissions: 0o755], ofItemAtPath: root + "/bin/run")
        try git.run(["-C", root, "add", "."])
        let store = GitIndexStore(root + "/.git", .standard)
        let index = try store.read()
        XCTAssertEqual(index.entries.map(\.pathText), ["bin/run", "hello.txt"])
        XCTAssertEqual(index.entries[0].mode, .executable)
        XCTAssertEqual(index.entries[1].objectId.hex, "ce013625030ba8dba906f756967f9e9ca394464a")
        // And git accepts an index this engine wrote.
        try store.write(try GitIndexV2([index.entries[1]]))
        XCTAssertEqual(try git.run(["-C", root, "ls-files", "--stage"]),
                       "100644 ce013625030ba8dba906f756967f9e9ca394464a 0\thello.txt\n")
    }
}
