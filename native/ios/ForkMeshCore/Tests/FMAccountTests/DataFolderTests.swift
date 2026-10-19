import FMBase
import FMCrypto
import Foundation
import XCTest
@testable import FMAccount

final class DataFolderTests: XCTestCase {
    private var scratch: URL!

    override func setUpWithError() throws {
        scratch = FileManager.default.temporaryDirectory
            .appendingPathComponent("data-folder-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: scratch, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        if let scratch, scratch.path.hasPrefix(FileManager.default.temporaryDirectory.path) {
            try? FileManager.default.removeItem(at: scratch)
        }
    }

    private func write(_ text: String, _ path: String, under base: URL) throws {
        let url = base.appendingPathComponent(path)
        try FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try Data(text.utf8).write(to: url)
    }

    private func read(_ path: String, under base: URL) -> String? {
        (try? Data(contentsOf: base.appendingPathComponent(path))).flatMap { String(data: $0, encoding: .utf8) }
    }

    func testTheFolderIsNamedLikeTheDesktopOne() {
        let picked = URL(fileURLWithPath: "/files/On My iPhone/ForkMesh", isDirectory: true)
        XCTAssertEqual(DataFolder.root(forPicked: picked).path, "/files/On My iPhone/ForkMesh/forkmesh_data")
        let already = URL(fileURLWithPath: "/files/forkmesh_data", isDirectory: true)
        XCTAssertEqual(DataFolder.root(forPicked: already).path, "/files/forkmesh_data")
        XCTAssertEqual(DataFolder.projectFolder(.legacy), "forkmesh")
        let project = ProjectScope(projectId: "work.example.com:8443", namespace: "p/work.example.com:8443/")
        XCTAssertEqual(DataFolder.projectFolder(project), "work.example.com_8443")
    }

    func testAFolderInsideTheAppIsRefused() {
        let container = URL(fileURLWithPath: "/var/mobile/Containers/Data/Application/ABC", isDirectory: true)
        XCTAssertNotNil(DataFolder.problem(with: container.appendingPathComponent("Documents"), appContainer: container))
        XCTAssertNil(DataFolder.problem(
            with: URL(fileURLWithPath: "/var/mobile/Library/Mobile Documents/ForkMesh"), appContainer: container))
    }

    func testTheAlertStaysUntilTheFolderAndIdentityAreSaved() {
        let prefs = InMemoryKeyValueStore()
        XCTAssertEqual(DataFolder.need(prefs: prefs, slug: "forkmesh", publicKey: "K", reachable: false), .chooseFolder)
        prefs.set("bookmark", forKey: DataFolder.locationKey)
        XCTAssertEqual(DataFolder.need(prefs: prefs, slug: "forkmesh", publicKey: "K", reachable: false), .reconnectFolder)
        XCTAssertEqual(DataFolder.need(prefs: prefs, slug: "forkmesh", publicKey: "K", reachable: true), .saveIdentity)
        prefs.set("K", forKey: DataFolder.kitKey("forkmesh"))
        XCTAssertNil(DataFolder.need(prefs: prefs, slug: "forkmesh", publicKey: "K", reachable: true))
        XCTAssertEqual(DataFolder.need(prefs: prefs, slug: "forkmesh", publicKey: "ROTATED", reachable: true), .saveIdentity)
    }

    func testSettingsRoundTripWithoutDeviceKeys() {
        let phone = InMemoryKeyValueStore()
        phone.set("dark", forKey: "theme/mode")
        phone.set(true, forKey: "notifications/enabled")
        phone.set(3, forKey: "chat/fontStep")
        phone.set(1.5, forKey: "chat/scale")
        phone.set(["a", "b"], forKey: "p/work.example.com/server/room")
        phone.set("bookmark", forKey: DataFolder.locationKey)
        phone.set(true, forKey: "install/installed")
        let document = DataFolder.exportSettings(phone)
        let reinstalled = InMemoryKeyValueStore()
        XCTAssertEqual(DataFolder.applySettings(JSON.parseObject(JSON.encode(document)), to: reinstalled), 5)
        XCTAssertEqual(reinstalled.string(forKey: "theme/mode"), "dark")
        XCTAssertEqual(reinstalled.bool(forKey: "notifications/enabled"), true)
        XCTAssertEqual(reinstalled.int(forKey: "chat/fontStep"), 3)
        XCTAssertEqual(reinstalled.double(forKey: "chat/scale"), 1.5)
        XCTAssertEqual(reinstalled.stringList(forKey: "p/work.example.com/server/room"), ["a", "b"])
        XCTAssertFalse(reinstalled.contains(DataFolder.locationKey))
        XCTAssertFalse(reinstalled.contains("install/installed"))
        XCTAssertEqual(DataFolder.applySettings(["values": .object(["x": "y"])], to: reinstalled), 0)
    }

    func testBackupCopiesChangedFilesAndRestoreBringsThemBack() throws {
        let documents = scratch.appendingPathComponent("app/Documents", isDirectory: true)
        let support = scratch.appendingPathComponent("app/Support", isDirectory: true)
        try write("hello", "notes/a.txt", under: documents)
        try write("chat", "chat-history/relay.json", under: support)
        try write("big", "LocalModels/model.bin", under: support)
        try write("other project", "projects/work/x.txt", under: support)
        let store = DataFolderStore(root: DataFolder.root(forPicked: scratch.appendingPathComponent("picked")))
        try store.prepare(platform: "ios", nowMillis: 1)
        try store.prepareProject(DataFolderProject(slug: "forkmesh", projectId: "app.forkmesh.com", namespace: "", name: "ForkMesh", publicKey: "K"))
        XCTAssertTrue(store.hasData)
        for root in DataFolder.roots {
            var isDirectory: ObjCBool = false
            XCTAssertTrue(FileManager.default.fileExists(atPath: store.projectDir("forkmesh").appendingPathComponent(root).path, isDirectory: &isDirectory))
            XCTAssertTrue(isDirectory.boolValue)
        }
        let trees = [DataFolderTree(name: "documents", local: documents, skippedTopLevel: ["projects"]),
                     DataFolderTree(name: "support", local: support, skippedTopLevel: ["projects"])]

        let first = store.backup(slug: "forkmesh", trees: trees)
        XCTAssertEqual(first.copied, 2)
        XCTAssertTrue(first.failed.isEmpty)
        let data = store.projectDir("forkmesh").appendingPathComponent("data")
        XCTAssertEqual(read("documents/notes/a.txt", under: data), "hello")
        XCTAssertEqual(read("support/chat-history/relay.json", under: data), "chat")
        XCTAssertNil(read("support/LocalModels/model.bin", under: data))
        XCTAssertNil(read("support/projects/work/x.txt", under: data))

        let second = store.backup(slug: "forkmesh", trees: trees)
        XCTAssertEqual(second.copied, 0)
        XCTAssertEqual(second.unchanged, 2)

        try write("hello again", "notes/a.txt", under: documents)
        try FileManager.default.setAttributes([.modificationDate: Date(timeIntervalSinceNow: 60)],
                                              ofItemAtPath: documents.appendingPathComponent("notes/a.txt").path)
        XCTAssertEqual(store.backup(slug: "forkmesh", trees: trees).copied, 1)
        XCTAssertEqual(read("documents/notes/a.txt", under: data), "hello again")

        let fresh = scratch.appendingPathComponent("reinstalled/Documents", isDirectory: true)
        let freshSupport = scratch.appendingPathComponent("reinstalled/Support", isDirectory: true)
        try write("made before restoring", "notes/a.txt", under: fresh)
        let restored = store.restore(slug: "forkmesh", trees: [
            DataFolderTree(name: "documents", local: fresh), DataFolderTree(name: "support", local: freshSupport)])
        XCTAssertEqual(restored.copied, 1)
        XCTAssertEqual(restored.unchanged, 1)
        XCTAssertEqual(read("notes/a.txt", under: fresh), "made before restoring")
        XCTAssertEqual(read("chat-history/relay.json", under: freshSupport), "chat")
    }

    func testProjectsAndSettingsAreReadBackFromTheFolder() throws {
        let store = DataFolderStore(root: scratch.appendingPathComponent("forkmesh_data"))
        try store.prepare(platform: "ios", nowMillis: 1)
        let work = DataFolderProject(slug: "work.example.com", projectId: "work.example.com",
                                     namespace: "p/work.example.com/", name: "Work", publicKey: "W")
        try store.prepareProject(work)
        try store.prepareProject(DataFolderProject(slug: "forkmesh", projectId: "", namespace: "", name: "ForkMesh", publicKey: "K"))
        try write("{}", "stray/project.json", under: store.root)
        XCTAssertEqual(store.projects().map(\.slug), ["forkmesh", "work.example.com"])
        XCTAssertEqual(store.projects().last, work)
        let prefs = InMemoryKeyValueStore()
        prefs.set("dark", forKey: "theme/mode")
        try store.writeSettings(DataFolder.exportSettings(prefs), slug: "forkmesh")
        XCTAssertEqual(read("forkmesh/config/ForkMesh/settings.json", under: store.root)?.contains("theme/mode"), true)
        XCTAssertNotNil(store.readSettings(slug: "forkmesh"))
        XCTAssertNil(store.readSettings(slug: "work.example.com"))
    }

    func testTheIdentityKitRestoresTheSameKey() throws {
        let store = DataFolderStore(root: scratch.appendingPathComponent("forkmesh_data"))
        let identity = try Identity.fromSeed(Array(repeating: 7, count: 32))
        let prefs = InMemoryKeyValueStore()
        XCTAssertThrowsError(try DataFolderIdentity.saveKit(
            identity: identity, passphrase: "short", confirm: "short", slug: "forkmesh", store: store, prefs: prefs, nowMillis: 1))
        try DataFolderIdentity.saveKit(
            identity: identity, passphrase: "correct horse battery", confirm: "correct horse battery",
            slug: "forkmesh", store: store, prefs: prefs, nowMillis: 1)
        XCTAssertEqual(prefs.string(forKey: DataFolder.kitKey("forkmesh")), identity.publicKeyB64url)
        let kit = try XCTUnwrap(store.readKit(slug: "forkmesh"))
        XCTAssertFalse(kit.contains(B64.encodeUrlNoPadding(identity.exportSeed())))

        let keychain = InMemorySecureStore()
        XCTAssertThrowsError(try DataFolderIdentity.restoreSeed(
            kit: kit, passphrase: "wrong passphrase!!", namespace: "", secureStore: keychain))
        XCTAssertNil(keychain.read(Identity.seedKey))
        let restored = try DataFolderIdentity.restoreSeed(
            kit: kit, passphrase: "correct horse battery", namespace: "p/work/", secureStore: keychain)
        XCTAssertEqual(restored, identity.publicKeyB64url)
        let reloaded = try Identity.loadOrCreate(secureStore: ScopedSecureStore(base: keychain, prefix: "p/work/"))
        XCTAssertEqual(reloaded.publicKeyB64url, identity.publicKeyB64url)
    }
}
