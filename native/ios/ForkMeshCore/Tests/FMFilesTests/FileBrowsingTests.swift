import FMBase
import XCTest
@testable import FMFiles

final class FileBrowsingTests: XCTestCase {
    let entries = [
        FileEntry(name: "b.txt", path: "b.txt", isFolder: false, size: 10, modified: Date(timeIntervalSince1970: 300)),
        FileEntry(name: "A.txt", path: "A.txt", isFolder: false, size: 30, modified: Date(timeIntervalSince1970: 100)),
        FileEntry(name: "zeta", path: "zeta", isFolder: true, modified: Date(timeIntervalSince1970: 200)),
        FileEntry(name: ".env", path: ".env", isFolder: false, size: 1, hidden: true),
        FileEntry(name: "file10", path: "file10", isFolder: false, size: 20),
        FileEntry(name: "file9", path: "file9", isFolder: false, size: 20),
    ]

    func testSortsFoldersFirstByNameDateAndSize() {
        XCTAssertEqual(FileBrowsing.sorted(entries, by: .name).map(\.name), ["zeta", ".env", "A.txt", "b.txt", "file9", "file10"])
        XCTAssertEqual(FileBrowsing.sorted(entries, by: .date, ascending: false).map(\.name).prefix(3), ["zeta", "b.txt", "A.txt"])
        XCTAssertEqual(FileBrowsing.sorted(entries, by: .size).map(\.name), ["zeta", ".env", "b.txt", "file9", "file10", "A.txt"])
    }

    func testFiltersHiddenAndByName() {
        XCTAssertEqual(FileBrowsing.visible(entries, showHidden: false, filter: "").count, 5)
        XCTAssertEqual(FileBrowsing.visible(entries, showHidden: true, filter: "").count, 6)
        XCTAssertEqual(FileBrowsing.visible(entries, showHidden: false, filter: " TXT ").map(\.name), ["b.txt", "A.txt"])
    }

    func testPaths() {
        XCTAssertEqual(FileBrowsing.join("", "a"), "a")
        XCTAssertEqual(FileBrowsing.join("/x//y/", "a"), "x/y/a")
        XCTAssertEqual(FileBrowsing.parent("x/y/a"), "x/y")
        XCTAssertEqual(FileBrowsing.parent("a"), "")
        XCTAssertEqual(FileBrowsing.name("x/y/a.txt"), "a.txt")
        XCTAssertEqual(FileBrowsing.crumbs("x/y", root: "usb").map(\.path), ["", "x", "x/y"])
        XCTAssertEqual(FileBrowsing.crumbs("x/y", root: "usb").map(\.title), ["usb", "x", "y"])
        XCTAssertTrue(FileBrowsing.isInside("a/b/c", folder: "a/b"))
        XCTAssertFalse(FileBrowsing.isInside("a/bc", folder: "a/b"))
        XCTAssertEqual(SmbClient.wire("/docs/deep/a.md"), "docs\\deep\\a.md")
    }

    func testNamesAndCopies() {
        XCTAssertNil(FileBrowsing.nameProblem("report.txt"))
        XCTAssertEqual(FileBrowsing.nameProblem("  "), "Enter a name.")
        XCTAssertEqual(FileBrowsing.nameProblem("a/b"), "Names cannot contain / \\ : * ? \" < > or |.")
        XCTAssertEqual(FileBrowsing.nameProblem(".."), "That name is not allowed.")
        XCTAssertEqual(FileBrowsing.uniqueName("a.txt", existing: ["b.txt"]), "a.txt")
        XCTAssertEqual(FileBrowsing.uniqueName("a.txt", existing: ["A.TXT"]), "a copy.txt")
        XCTAssertEqual(FileBrowsing.uniqueName("a.txt", existing: ["a.txt", "a copy.txt"]), "a copy 2.txt")
        XCTAssertEqual(FileBrowsing.uniqueName("docs", existing: ["docs"]), "docs copy")
        XCTAssertEqual(FileBrowsing.uniqueName(".env", existing: [".env"]), ".env copy")
        XCTAssertEqual(FileBrowsing.deleteQuestion(entries[2]), "Delete zeta and everything in it?")
        XCTAssertEqual(FileBrowsing.deleteQuestion(entries[0]), "Delete b.txt?")
    }

    func testKinds() {
        XCTAssertEqual(FileKinds.kind(name: "a.PNG"), .image)
        XCTAssertEqual(FileKinds.kind(name: "a.pdf"), .pdf)
        XCTAssertEqual(FileKinds.kind(name: "main.swift"), .text)
        XCTAssertEqual(FileKinds.kind(name: "Makefile"), .text)
        XCTAssertEqual(FileKinds.kind(name: "movie.mkv"), .other)
        XCTAssertTrue(FileKinds.looksLikeText(Array("plain text ✓".utf8)))
        XCTAssertFalse(FileKinds.looksLikeText([0x41, 0x00, 0x42]))
        XCTAssertFalse(FileKinds.looksLikeText([0xff, 0xfe, 0xfd]))
        let long = [UInt8](repeating: 0x61, count: FileKinds.sniffLength - 1) + Array("✓✓".utf8)
        XCTAssertTrue(FileKinds.looksLikeText(long))
        XCTAssertEqual(FileKinds.symbol(entries[2]), "folder")
    }

    func testAddresses() {
        let plain = SmbAddress.parse("192.168.1.1").address
        XCTAssertEqual(plain?.host, "192.168.1.1")
        XCTAssertEqual(plain?.port, 445)
        let full = SmbAddress.parse("smb://bob@nas.lan:1445/media/films/new").address
        XCTAssertEqual(full?.host, "nas.lan")
        XCTAssertEqual(full?.port, 1445)
        XCTAssertEqual(full?.share, "media")
        XCTAssertEqual(full?.path, "films/new")
        XCTAssertEqual(full?.user, "bob")
        XCTAssertEqual(SmbAddress.parse("\\\\router\\usb").address?.share, "usb")
        XCTAssertEqual(SmbAddress.parse("8.8.8.8").error, "Network shares must be on your local network.")
        XCTAssertEqual(SmbAddress.parse("").error, "Enter the address of the computer or router that shares the files.")
        XCTAssertTrue(SmbAddress.allLocal(["192.168.1.1", "fe80::1"]))
        XCTAssertFalse(SmbAddress.allLocal(["192.168.1.1", "1.1.1.1"]))
        XCTAssertFalse(SmbAddress.allLocal([]))
    }

    func testNetworkShareModel() {
        let share = NetworkShare(address: "router.lan", share: "usb", user: "bob")
        XCTAssertEqual(share.title, "usb on router.lan")
        XCTAssertEqual(share.url, "smb://bob@router.lan/usb")
        XCTAssertEqual(share.url(for: "docs/a.txt"), "smb://bob@router.lan/usb/docs/a.txt")
        XCTAssertEqual(NetworkShare(address: "router.lan:1445", share: "usb", guest: true).url(for: ""), "smb://router.lan:1445/usb/")
        XCTAssertEqual(NetworkShare.problem(address: "router.lan", share: "", user: "", guest: true), "Enter the share name, or press Find shares.")
        XCTAssertEqual(NetworkShare.problem(address: "router.lan", share: "usb", user: "", guest: false), "Enter the user name, or switch on Sign in as guest.")
        XCTAssertEqual(NetworkShare.problem(address: "router.lan", share: "a/b", user: "", guest: true), "Type only the share name, without slashes.")
        XCTAssertNil(NetworkShare.problem(address: "router.lan", share: "usb", user: "", guest: true))
        XCTAssertEqual(RouterShareSuggestion(name: "usb", host: "192.168.1.1", guests: true).title, "usb on router")
    }

    func testShareStoreKeepsPasswordsOnlyWhenRemembered() {
        let prefs = InMemoryKeyValueStore()
        let secure = InMemorySecureStore()
        let store = NetworkShareStore(prefs: prefs, secure: secure)
        let saved = store.save(NetworkShare(address: "router.lan", share: "usb", user: "bob", rememberPassword: true), password: "pw")
        XCTAssertEqual(store.password(for: saved), "pw")
        let again = store.save(NetworkShare(address: "ROUTER.lan", share: "USB", user: "Bob", rememberPassword: false), password: "pw")
        XCTAssertEqual(again.id, saved.id)
        XCTAssertEqual(store.all().count, 1)
        XCTAssertNil(store.password(for: again))
        XCTAssertNil(secure.read(NetworkShareStore.passwordPrefix + saved.id))
        store.remove(saved.id)
        XCTAssertTrue(store.all().isEmpty)
    }
}

final class FileSourceTests: XCTestCase {
    var folder: URL!

    override func setUpWithError() throws {
        folder = FileManager.default.temporaryDirectory.appendingPathComponent("fmfiles-\(UUID().uuidString)")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? FileManager.default.removeItem(at: folder)
    }

    func testLocalSourceOperations() async throws {
        let local = LocalFileSource(id: "local", title: "On this iPhone", root: folder)
        try await local.makeFolder("notes")
        try await local.makeFile("notes/a.txt")
        try await local.write("notes/a.txt", data: Data("hello".utf8))
        let awaited1 = try await local.read("notes/a.txt", limit: 100, progress: nil)
        XCTAssertEqual(String(decoding: awaited1, as: UTF8.self), "hello")
        await XCTAssertThrowsSmb(try await local.read("notes/a.txt", limit: 2, progress: nil), SmbError.tooLarge.message)
        await XCTAssertThrowsSmb(try await local.makeFile("notes/a.txt"), "Something with that name is already there.")
        try await local.rename("notes/a.txt", to: "notes/b.txt")
        let listed = try await local.list("notes")
        XCTAssertEqual(listed.map(\.name), ["b.txt"])
        XCTAssertEqual(listed.first?.size, 5)
        XCTAssertFalse(listed.first?.isFolder ?? true)
        await XCTAssertThrowsSmb(try await local.list("../.."), "That path leaves the folder.")
        await XCTAssertThrowsSmb(try await local.delete("", isFolder: true), "The top folder cannot be deleted.")
        try await local.makeFolder("notes/deeper")
        try await local.write("notes/deeper/c.txt", data: Data("c".utf8))
        try await local.delete("notes", isFolder: true)
        let awaited2 = try await local.list("").isEmpty
        XCTAssertTrue(awaited2)
        XCTAssertEqual(local.describe("x/y"), folder.standardizedFileURL.path + "/x/y")
    }

    func testCopyAndMoveAcrossDeviceAndNetwork() async throws {
        let state = FakeSmbState()
        let share = NetworkShare(address: "router.lan", share: "usb", guest: true)
        let network = SmbFileSource(share: share, client: SmbClient(host: "router.lan", share: "usb", credentials: SmbCredentials(guest: true), connector: state.connector()))
        let local = LocalFileSource(id: "local", title: "On this iPhone", root: folder)
        let docsListed = try await network.list("").first { $0.name == "docs" }
        let docs = try XCTUnwrap(docsListed)
        let copied = try await FileTransfer.paste(docs, from: network, to: local, into: "", mode: .copy)
        XCTAssertEqual(copied, "docs")
        let awaited3 = try await local.read("docs/deep/a.md", limit: 100, progress: nil)
        XCTAssertEqual(String(decoding: awaited3, as: UTF8.self), "# A")
        let again = try await FileTransfer.paste(docs, from: network, to: local, into: "", mode: .copy)
        XCTAssertEqual(again, "docs copy")
        let noteListed = try await local.list("docs").first { $0.name == "notes.txt" }
        let note = try XCTUnwrap(noteListed)
        let uploaded = try await FileTransfer.paste(note, from: local, to: network, into: "", mode: .move)
        XCTAssertEqual(uploaded, "notes.txt")
        let awaited4 = try await network.read("notes.txt", limit: 100, progress: nil)
        XCTAssertEqual(String(decoding: awaited4, as: UTF8.self), "hello from the router")
        let awaited5 = try await local.list("docs").contains { $0.name == "notes.txt" }
        XCTAssertFalse(awaited5)
        let photoListed = try await network.list("").first { $0.name == "photo.jpg" }
        let photo = try XCTUnwrap(photoListed)
        let awaited6 = try await FileTransfer.paste(photo, from: network, to: network, into: "docs", mode: .move)
        XCTAssertEqual(awaited6, "docs/photo.jpg")
        let awaited7 = try await network.list("docs").filter { $0.name == "photo.jpg" }.count
        XCTAssertEqual(awaited7, 1)
        let docsAgainListed = try await network.list("").first { $0.name == "docs" }
        let docsAgain = try XCTUnwrap(docsAgainListed)
        await XCTAssertThrowsSmb(try await FileTransfer.paste(docsAgain, from: network, to: network, into: "docs/deep", mode: .move), "A folder cannot go inside itself.")
        let hidden = try await network.list("")
        XCTAssertTrue(hidden.first { $0.name == ".hidden" }?.hidden ?? false)
        XCTAssertEqual(network.describe("docs"), "smb://router.lan/usb/docs")
    }
}
