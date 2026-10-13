import FMBase
import FMChat
import FMControl
import Foundation
import XCTest

@testable import FMTeammates

@MainActor
final class FakeShortcutMirror: ShortcutMirror {
    var trees: [String: URL] = [:]

    func mirroredRepos() -> [String] { trees.keys.sorted() }

    func worktree(repo: String) -> URL? {
        trees.first { $0.key.caseInsensitiveCompare(repo) == .orderedSame }?.value
    }
}

final class PhoneShortcutsTests: XCTestCase {
    var root: URL!

    override func setUpWithError() throws {
        root = FileManager.default.temporaryDirectory.appendingPathComponent("phone-shortcuts-\(UUID().uuidString)")
        try write(".forkmesh/shortcuts/fix.md", "# name: Fix UI stalls\n# description: Fix freezes.\n# model: opus\n\nFix them.\nLog: {{stallLog}}\n")
        try write(".forkmesh/shortcuts/deploy.sh", "#!/bin/sh\n# name: Deploy\necho hi\n")
        try write(".forkmesh/shortcuts/empty.md", "# name: Empty\n")
        try write(".forkmesh/skills/review.skill", "# Review the code\nReview it.\n")
    }

    override func tearDownWithError() throws {
        if let root, root.path.hasPrefix(FileManager.default.temporaryDirectory.path) {
            try? FileManager.default.removeItem(at: root)
        }
    }

    private func write(_ path: String, _ text: String) throws {
        let url = root.appendingPathComponent(path)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(text.utf8).write(to: url)
    }

    func testPromptDropsHeadersAndDesktopPlaceholders() {
        let prompt = PhoneShortcuts.prompt("# name: A\n# agent: codex\nDo it.\n# keep this\n{{appLog}}\n")
        XCTAssertEqual(prompt, "Do it.\n# keep this\n\(PhoneShortcuts.desktopOnly)")
    }

    func testMetaReadsHeaders() {
        let meta = PhoneShortcuts.meta(file: "x.md", text: "#!/bin/sh\n# name: X\n# agent: Codex\n# model: m\n")
        XCTAssertEqual(meta, ShortcutMeta(name: "X", description: "", agent: "codex", model: "m"))
        XCTAssertEqual(PhoneShortcuts.meta(file: "y.skill", text: "# Plain words\n").description, "Plain words")
    }

    func testEntriesReadTheMirror() throws {
        let entries = PhoneShortcuts.entries(worktree: root)
        let byFile = Dictionary(uniqueKeysWithValues: entries.map { ($0.file, $0) })
        XCTAssertEqual(Set(byFile.keys), ["fix.md", "deploy.sh", "empty.md", "review.skill"])
        XCTAssertTrue(entries.allSatisfy(\.onPhone))
        XCTAssertEqual(byFile["fix.md"]?.name, "Fix UI stalls")
        XCTAssertEqual(byFile["fix.md"]?.kind, "prompt")
        XCTAssertEqual(byFile["fix.md"]?.runnable, true)
        XCTAssertEqual(byFile["deploy.sh"]?.kind, "script")
        XCTAssertEqual(byFile["deploy.sh"]?.runnable, false)
        XCTAssertEqual(byFile["empty.md"]?.runnable, false)
        XCTAssertEqual(byFile["review.skill"]?.scope, "repository")
        XCTAssertEqual(byFile["review.skill"]?.runnable, true)
    }

    func testFileRefusesEscapes() {
        XCTAssertNil(PhoneShortcuts.file(worktree: root, scope: "shortcut", file: "../x.md"))
        XCTAssertNil(PhoneShortcuts.file(worktree: root, scope: "app", file: "x.md"))
    }

    @MainActor func testStoreFallsBackToThePhoneWhenTheDesktopFails() async throws {
        let channel = FakeTeamChannel()
        channel.failures["insights.shortcuts"] = LocalControlException("not_connected")
        let store = ShortcutsStore(channel: channel)
        let mirror = FakeShortcutMirror()
        mirror.trees["f/r"] = root
        store.mirror = mirror
        await store.refresh(repo: "F/R")
        let catalog = try XCTUnwrap(store.catalog)
        XCTAssertTrue(catalog.onPhone)
        XCTAssertEqual(catalog.repos, ["f/r"])
        let fix = try XCTUnwrap(catalog.shortcuts.first { $0.file == "fix.md" })
        let prompt = try await store.phonePrompt(repo: "f/r", entry: fix)
        XCTAssertEqual(prompt, "Fix them.\nLog: \(PhoneShortcuts.desktopOnly)")
        XCTAssertEqual(channel.count("insights.shortcut"), 0)
    }

    @MainActor func testDesktopCatalogReplacesThePhoneOne() async throws {
        let channel = FakeTeamChannel()
        channel.replies["insights.shortcuts"] = ShortcutsStoreTests.catalogJSON
        let store = ShortcutsStore(channel: channel)
        let mirror = FakeShortcutMirror()
        mirror.trees["f/r"] = root
        store.mirror = mirror
        await store.refresh(repo: "f/r")
        XCTAssertEqual(store.catalog?.onPhone, false)
        XCTAssertEqual(store.catalog?.repos, ["f/R", "f/other"])
    }

    @MainActor func testUnmirroredRepoWaitsForTheDesktop() async {
        let channel = FakeTeamChannel()
        channel.failures["insights.shortcuts"] = LocalControlException("not_connected")
        let store = ShortcutsStore(channel: channel)
        store.mirror = FakeShortcutMirror()
        await store.refresh(repo: "f/r")
        XCTAssertNil(store.catalog)
        XCTAssertFalse(store.error.isEmpty)
    }
}
