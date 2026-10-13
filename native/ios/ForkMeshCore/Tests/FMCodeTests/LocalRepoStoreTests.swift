import FMBase
import FMGit
import Foundation
import XCTest
@testable import FMCode

@MainActor
final class LocalRepoStoreTests: XCTestCase {
    private func makeStore() throws -> (URL, LocalRepoStore) {
        let temp = try localTempDirectory(self, "fm-repo-store")
        let root = temp.appendingPathComponent("docs/forkmesh/repos")
        let fixed = Date(epochMillis: 1_787_486_400_000) // 2026-08-23T12:00:00Z
        return (temp, LocalRepoStore(root: root, now: { fixed }))
    }

    func testValidatesRepositoryNames() async throws {
        XCTAssertTrue(LocalRepoStore.isValidName("my-repo_1.0"))
        XCTAssertFalse(LocalRepoStore.isValidName(""))
        XCTAssertFalse(LocalRepoStore.isValidName("."))
        XCTAssertFalse(LocalRepoStore.isValidName(".."))
        XCTAssertFalse(LocalRepoStore.isValidName("a/b"))
        XCTAssertFalse(LocalRepoStore.isValidName("a b"))
        XCTAssertFalse(LocalRepoStore.isValidName("ü"))
        XCTAssertTrue(LocalRepoStore.isValidName(String(repeating: "x", count: 100)))
        XCTAssertFalse(LocalRepoStore.isValidName(String(repeating: "x", count: 101)))
        XCTAssertNotNil(LocalRepoStore.validateName(""))
        XCTAssertNil(LocalRepoStore.validateName("ok"))
        XCTAssertEqual(LocalRepoStore.validateName(String(repeating: "x", count: 101)), "Keep the name under 100 characters.")
        XCTAssertEqual(LocalRepoStore.validateName("a b"), "Use letters, digits, dots, dashes, or underscores only.")
    }

    func testCreatesListsRefreshesAndDeletesRepositories() async throws {
        let (_, store) = try makeStore()
        let empty = try await store.list()
        XCTAssertTrue(empty.isEmpty)
        XCTAssertEqual(EngineFS.entityType(store.root.path), .directory)

        var notifications = 0
        store.addListener { notifications += 1 }

        let record = try await store.create(name: "notes", author: localTestAuthor, description: "My notes")
        XCTAssertEqual(record.name, "notes")
        XCTAssertEqual(record.path, "\(store.root.path)/notes")
        XCTAssertEqual(record.branch, "main")
        XCTAssertEqual(record.lastCommitSubject, "Initial commit")
        XCTAssertNotNil(record.lastCommitAt)
        XCTAssertEqual(record.createdAt, Date(epochMillis: 1_787_486_400_000))
        XCTAssertFalse(record.isPublished)
        XCTAssertGreaterThan(notifications, 0)
        XCTAssertGreaterThan(store.revision, 0)

        // README seeded with the name and description.
        XCTAssertEqual(try localRead(record.worktree.appendingPathComponent("README.md")), "# notes\n\nMy notes\n")

        // Index persisted as JSON under the root.
        let index = try JSON.parse(try Data(contentsOf: store.root.appendingPathComponent("index.json")))
        XCTAssertEqual(index["version"], .int(1))
        let repos = index["repos"]?.arrayValue ?? []
        XCTAssertEqual(repos.count, 1)
        XCTAssertEqual(repos.first?["name"], .string("notes"))
        XCTAssertEqual(repos.first?["createdAt"], .string("2026-08-23T12:00:00.000Z"))

        // A fresh store instance reloads from disk.
        let reloaded = LocalRepoStore(root: store.root)
        let reloadedList = try await reloaded.list()
        XCTAssertEqual(reloadedList.map(\.name), ["notes"])
        let found = try await reloaded.find("notes")
        XCTAssertEqual(found?.description, "My notes")
        XCTAssertEqual(found?.createdAt, Date(epochMillis: 1_787_486_400_000))
        let missing = try await reloaded.find("missing")
        XCTAssertNil(missing)

        // Refresh picks up new commits.
        let workspace = store.workspace(record)
        try await workspace.writeFile("a.txt", "a")
        try await workspace.stageAll()
        _ = try await workspace.commit(message: "Add a", author: localTestAuthor)
        workspace.dispose()
        let refreshed = try await store.refresh("notes")
        XCTAssertEqual(refreshed?.lastCommitSubject, "Add a")

        try await store.markPublished("notes", "alice/notes")
        let published = try await store.find("notes")
        XCTAssertEqual(published?.publishedAs, "alice/notes")
        XCTAssertEqual(published?.syncLabel, "Published")

        try await store.delete("notes")
        let afterDelete = try await store.list()
        XCTAssertTrue(afterDelete.isEmpty)
        XCTAssertEqual(EngineFS.entityType(record.path), .notFound)
    }

    func testRejectsDuplicatesBadNamesAndSymlinkedPaths() async throws {
        let (temp, store) = try makeStore()
        _ = try await store.create(name: "dup", author: localTestAuthor)
        await localAssertThrows(try await store.create(name: "dup", author: localTestAuthor)) { error in
            XCTAssertEqual((error as? LocalRepoStoreException)?.message, "A local repository with that name already exists.")
        }
        await localAssertThrows(try await store.create(name: "../escape", author: localTestAuthor)) { error in
            XCTAssertTrue(error is LocalRepoStoreException)
        }
        await localAssertThrows(try await store.create(name: "..", author: localTestAuthor)) { error in
            XCTAssertTrue(error is LocalRepoStoreException)
        }
        // A symlink planted at a repo path must be refused, not followed.
        let outside = temp.appendingPathComponent("outside")
        try FileManager.default.createDirectory(at: outside, withIntermediateDirectories: true)
        try FileManager.default.createSymbolicLink(
            atPath: store.root.appendingPathComponent("linked").path, withDestinationPath: outside.path)
        await localAssertThrows(try await store.create(name: "linked", author: localTestAuthor)) { error in
            XCTAssertTrue(error is LocalRepoStoreException)
        }
        await localAssertThrows(try await store.delete("linked")) { error in
            XCTAssertTrue(error is LocalRepoStoreException)
        }
        XCTAssertEqual(EngineFS.entityType(outside.path), .directory)
    }

    func testRefusesASymlinkedStoreRoot() async throws {
        let temp = try localTempDirectory(self, "fm-repo-store-link")
        let real = temp.appendingPathComponent("real")
        try FileManager.default.createDirectory(at: real, withIntermediateDirectories: true)
        let link = temp.appendingPathComponent("link")
        try FileManager.default.createSymbolicLink(atPath: link.path, withDestinationPath: real.path)
        let linked = LocalRepoStore(root: link)
        await localAssertThrows(try await linked.list()) { error in
            XCTAssertEqual((error as? LocalRepoStoreException)?.message, "Local repository root is a symlink and was refused.")
        }
        // The non-throwing form keeps the reason for the section to show.
        await linked.ensureLoaded()
        XCTAssertEqual(linked.loadError, "Local repository root is a symlink and was refused.")
        XCTAssertTrue(linked.records.isEmpty)
    }

    func testAFailedCreateLeavesNoFolderOrIndexEntry() async throws {
        let (_, store) = try makeStore()
        // An unsafe author makes the engine reject the commit.
        let bad = GitAuthor(name: "bad\nname", email: "x@y")
        await localAssertThrows(try await store.create(name: "broken", author: bad)) { error in
            XCTAssertTrue(error is GitWorkspaceException, "\(error)")
        }
        let listed = try await store.list()
        XCTAssertTrue(listed.isEmpty)
        XCTAssertEqual(EngineFS.entityType(store.root.appendingPathComponent("broken").path), .notFound)
        XCTAssertFalse(store.isBusy)
    }

    func testDropsIndexEntriesWhoseWorktreeVanished() async throws {
        let (_, store) = try makeStore()
        let record = try await store.create(name: "gone", author: localTestAuthor)
        try FileManager.default.removeItem(at: record.worktree)
        let reloaded = try await LocalRepoStore(root: store.root).list()
        XCTAssertTrue(reloaded.isEmpty)
    }

    // iOS gives an app a container whose UUID changes on reinstall and on some
    // upgrades. The repositories are still on disk under the new root — but
    // every absolute path recorded in index.json names the old one.
    func testSurvivesTheStoreRootMovingUnderTheIndex() async throws {
        let (temp, store) = try makeStore()
        _ = try await store.create(name: "keeper", author: localTestAuthor)
        _ = try await store.create(name: "second", author: localTestAuthor)

        let moved = temp.appendingPathComponent("moved/forkmesh/repos")
        try FileManager.default.createDirectory(at: moved.deletingLastPathComponent(), withIntermediateDirectories: true)
        try FileManager.default.moveItem(at: store.root, to: moved)

        let onDisk = try JSON.parse(try Data(contentsOf: moved.appendingPathComponent("index.json")))
        let recorded = (onDisk["repos"]?.arrayValue ?? []).compactMap { $0["path"]?.stringValue }
        XCTAssertTrue(recorded.allSatisfy { $0.hasPrefix(store.root.path) }, "the fixture must reproduce stale absolute paths")

        let listed = try await LocalRepoStore(root: moved).list()
        XCTAssertEqual(Set(listed.map(\.name)), ["keeper", "second"])
        for record in listed {
            XCTAssertEqual(EngineFS.entityType(record.path), .directory)
            XCTAssertTrue(record.path.hasPrefix(moved.path))
        }
    }

    func testAMovedRootIsNotTreatedAsAVanishedWorktree() async throws {
        let (temp, store) = try makeStore()
        _ = try await store.create(name: "keeper", author: localTestAuthor)
        let moved = temp.appendingPathComponent("moved2/forkmesh/repos")
        try FileManager.default.createDirectory(at: moved.deletingLastPathComponent(), withIntermediateDirectories: true)
        try FileManager.default.moveItem(at: store.root, to: moved)
        _ = try await LocalRepoStore(root: moved).list()
        let second = try await LocalRepoStore(root: moved).list()
        XCTAssertEqual(second.map(\.name), ["keeper"])
    }

    func testAdoptsAWorktreeTheIndexHasForgotten() async throws {
        let (_, store) = try makeStore()
        _ = try await store.create(name: "orphan", author: localTestAuthor)
        try FileManager.default.removeItem(at: store.root.appendingPathComponent("index.json"))

        let listed = try await LocalRepoStore(root: store.root).list()
        XCTAssertEqual(listed.map(\.name), ["orphan"])
        XCTAssertEqual(EngineFS.entityType(listed[0].path), .directory)
        let again = try await LocalRepoStore(root: store.root).list()
        XCTAssertEqual(again.map(\.name), ["orphan"])
    }

    func testAdoptsOnlyFoldersThatAreReallyWorktrees() async throws {
        let (_, store) = try makeStore()
        let manager = FileManager.default
        try manager.createDirectory(at: store.root.appendingPathComponent("not-a-repo"), withIntermediateDirectories: true)
        try manager.createDirectory(at: store.root.appendingPathComponent("bad name/.git"), withIntermediateDirectories: true)
        let listed = try await LocalRepoStore(root: store.root).list()
        XCTAssertTrue(listed.isEmpty)
    }

    func testAdoptionDoesNotResurrectADeletedRepository() async throws {
        let (_, store) = try makeStore()
        let record = try await store.create(name: "gone", author: localTestAuthor)
        try await store.delete("gone")
        XCTAssertEqual(EngineFS.entityType(record.path), .notFound)
        let listed = try await LocalRepoStore(root: store.root).list()
        XCTAssertTrue(listed.isEmpty)
    }

    func testImportsADirectoryIntoANewRepository() async throws {
        let (temp, store) = try makeStore()
        let source = temp.appendingPathComponent("src")
        try localWrite(source.appendingPathComponent("hello.txt"), "hi")
        let record = try await store.importDirectory(name: "imported", source: source, author: localTestAuthor)
        XCTAssertEqual(record.lastCommitSubject, "Import from Files")
        let workspace = store.workspace(record)
        defer { workspace.dispose() }
        let hello = try await workspace.readFile("hello.txt")
        XCTAssertEqual(hello.text, "hi")
    }

    func testRecordJsonRoundTripAndLenientParsing() async throws {
        let record = LocalRepoRecord(
            name: "x", path: "/tmp/x", createdAt: Date(epochMillis: 1_700_000_000_123), description: "d",
            branch: "dev", lastCommitSubject: "s", lastCommitAt: Date(epochMillis: 1_700_000_100_000),
            publishedAs: "a/x", origin: .cloned)
        XCTAssertEqual(LocalRepoRecord.fromJSON(record.toJSON()), record)
        XCTAssertEqual(record.syncLabel, "Cloned")
        // The retired Flutter app wrote microseconds; the Swift reader keeps milliseconds.
        let dart = LocalRepoRecord.fromJSON([
            "name": "y", "path": "/p/y", "createdAt": "2026-08-23T12:00:00.123456Z", "lastCommitAt": nil,
        ])
        XCTAssertEqual(dart?.createdAt, Date(epochMillis: 1_787_486_400_123))
        XCTAssertEqual(dart?.branch, "main")
        XCTAssertNil(dart?.lastCommitAt)
        XCTAssertEqual(dart?.origin, .authored)
        XCTAssertNil(LocalRepoRecord.fromJSON(["name": "bad name", "path": "/p"]))
        XCTAssertNil(LocalRepoRecord.fromJSON(["name": "ok"]))
        let rebased = record.rebasedOn(URL(fileURLWithPath: "/new/root"))
        XCTAssertEqual(rebased.path, "/new/root/x")
        XCTAssertEqual(record.rebasedOn(URL(fileURLWithPath: "/tmp")), record)
    }

    func testRelocatesEveryRepositoryIntoAChosenFolder() async throws {
        let (temp, store) = try makeStore()
        let first = try await store.create(name: "first", author: localTestAuthor)
        _ = try await store.create(name: "second", author: localTestAuthor)
        try await store.markPublished("first", "alice/first")
        let chosen = temp.appendingPathComponent("On My iPhone/Repositories")

        let result = try await store.relocate(to: chosen)

        XCTAssertEqual(Set(result.moved), ["first", "second"])
        XCTAssertTrue(result.leftBehind.isEmpty)
        XCTAssertEqual(store.root.path, chosen.standardizedFileURL.path)
        XCTAssertEqual(EngineFS.entityType(first.path), .notFound)
        XCTAssertEqual(Set(store.records.map(\.name)), ["first", "second"])
        XCTAssertTrue(store.records.allSatisfy { $0.path.hasPrefix(chosen.path) })
        let reread = try await LocalRepoStore(root: chosen).list()
        XCTAssertEqual(reread.first { $0.name == "first" }?.publishedAs, "alice/first")
        let hello = try await store.create(name: "third", author: localTestAuthor)
        XCTAssertTrue(hello.path.hasPrefix(chosen.path))
    }

    func testAReinstallGetsItsRepositoriesBackByChoosingTheFolderAgain() async throws {
        let (temp, store) = try makeStore()
        _ = try await store.create(name: "kept", author: localTestAuthor)
        try await store.markPublished("kept", "alice/kept")
        let chosen = temp.appendingPathComponent("On My iPhone/Repositories")
        _ = try await store.relocate(to: chosen)

        let reinstalled = LocalRepoStore(root: temp.appendingPathComponent("new-container/forkmesh/repos"))
        let empty = try await reinstalled.list()
        XCTAssertTrue(empty.isEmpty)
        let result = try await reinstalled.relocate(to: chosen)

        XCTAssertTrue(result.moved.isEmpty)
        XCTAssertEqual(reinstalled.records.map(\.name), ["kept"])
        XCTAssertEqual(reinstalled.records.first?.publishedAs, "alice/kept")
    }

    func testRelocationLeavesANameClashWhereItWas() async throws {
        let (temp, store) = try makeStore()
        let clash = try await store.create(name: "clash", author: localTestAuthor)
        _ = try await store.create(name: "free", author: localTestAuthor)
        let chosen = temp.appendingPathComponent("chosen")
        try FileManager.default.createDirectory(at: chosen.appendingPathComponent("clash"), withIntermediateDirectories: true)
        let oldRoot = store.root

        let result = try await store.relocate(to: chosen)

        XCTAssertEqual(result.moved, ["free"])
        XCTAssertEqual(result.leftBehind, ["clash"])
        XCTAssertEqual(EngineFS.entityType(clash.path), .directory)
        XCTAssertEqual(store.records.map(\.name), ["free"])
        let left = try await LocalRepoStore(root: oldRoot).list()
        XCTAssertEqual(left.map(\.name), ["clash"])
    }

    func testRefusesAFolderInsideTheCurrentOne() async throws {
        let (_, store) = try makeStore()
        _ = try await store.create(name: "repo", author: localTestAuthor)
        do {
            _ = try await store.relocate(to: store.root.appendingPathComponent("repo/inner"))
            XCTFail("expected a refusal")
        } catch let error as LocalRepoStoreException {
            XCTAssertEqual(error.message, "Pick a folder outside the one the repositories are in now.")
        }
        XCTAssertEqual(store.records.map(\.name), ["repo"])
    }

    func testChecksTheFolderPickedForRepositories() {
        let container = URL(fileURLWithPath: "/var/mobile/Containers/Data/Application/ABC")
        XCTAssertEqual(
            LocalRepoFolder.problem(
                with: URL(fileURLWithPath: "/private/var/mobile/Containers/Data/Application/ABC/Documents/x"),
                appContainer: container, isInICloud: false),
            LocalRepoFolder.insideApp)
        XCTAssertEqual(LocalRepoFolder.problem(with: container, appContainer: container, isInICloud: false), LocalRepoFolder.insideApp)
        let onPhone = URL(fileURLWithPath: "/private/var/mobile/Containers/Shared/AppGroup/D/File Provider Storage/Repos")
        XCTAssertNil(LocalRepoFolder.problem(with: onPhone, appContainer: container, isInICloud: false))
        XCTAssertEqual(LocalRepoFolder.problem(with: onPhone, appContainer: container, isInICloud: true), LocalRepoFolder.inICloud)
        XCTAssertNil(LocalRepoFolder.problem(
            with: URL(fileURLWithPath: "/var/mobile/Containers/Data/Application/ABCD"), appContainer: container, isInICloud: false))
    }

    func testSaysWhatTheMoveDid() {
        XCTAssertEqual(
            LocalRepoFolder.movedMessage(LocalRepoRelocation(moved: [], leftBehind: []), to: "Repos"),
            "Repositories are now saved in “Repos”.")
        XCTAssertEqual(
            LocalRepoFolder.movedMessage(LocalRepoRelocation(moved: ["a", "b"], leftBehind: ["c"]), to: "Repos"),
            "Moved 2 repositories to “Repos”. c stayed where it was: “Repos” already has a folder with that name.")
    }
}
