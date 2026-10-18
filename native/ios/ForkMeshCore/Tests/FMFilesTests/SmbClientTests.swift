import FMBase
import XCTest
@testable import FMFiles

final class SmbHashTests: XCTestCase {
    func testMd4Vectors() {
        XCTAssertEqual(SmbText.hex(Md4.hash([])), "31d6cfe0d16ae931b73c59d7e0c089c0")
        XCTAssertEqual(SmbText.hex(Md4.hash(Array("abc".utf8))), "a448017aaf21d8525fc10ae87aa6729d")
        XCTAssertEqual(SmbText.hex(Md4.hash(Array("12345678901234567890123456789012345678901234567890123456789012345678901234567890".utf8))), "e33b4ddc9c38f2199c3e7b164fcc0536")
    }

    func testMd5AndHmacVectors() {
        XCTAssertEqual(SmbText.hex(Md5.hash([])), "d41d8cd98f00b204e9800998ecf8427e")
        XCTAssertEqual(SmbText.hex(Md5.hash(Array("abc".utf8))), "900150983cd24fb0d6963f7d28e17f72")
        XCTAssertEqual(SmbText.hex(HmacMd5.mac(key: [UInt8](repeating: 0x0b, count: 16), Array("Hi There".utf8))), "9294727a3638bb1c13f48ef8158bfc9d")
        XCTAssertEqual(SmbText.hex(HmacMd5.mac(key: Array("Jefe".utf8), Array("what do ya want for nothing?".utf8))), "750c783e6ab0b503eaa86e310a5db738")
    }
}

final class NtlmTests: XCTestCase {
    let targetInfo = SmbText.bytes(hex: "02000c0044006f006d00610069006e0001000c0053006500720076006500720000000000")

    func testMsNlmpNtlmv2Vectors() {
        XCTAssertEqual(SmbText.hex(Ntlm.ntHash("Password")), "a4f49c406510bdcab6824ee7c30fd852")
        let key = Ntlm.ntowfv2(password: "Password", user: "User", domain: "Domain")
        XCTAssertEqual(SmbText.hex(key), "0c868a403bfd7a93a3001ef22ef02e3f")
        let blob = Ntlm.blob(timestamp: 0, clientChallenge: [UInt8](repeating: 0xaa, count: 8), targetInfo: targetInfo)
        let proof = Ntlm.ntProof(ntowfv2: key, serverChallenge: SmbText.bytes(hex: "0123456789abcdef"), blob: blob)
        XCTAssertEqual(SmbText.hex(proof), "68cd0ab851e51c96aabc927bebef6a1c")
        XCTAssertEqual(SmbText.hex(HmacMd5.mac(key: key, proof)), "8de40ccadbc14a82f15cb0ad0de95ca3")
    }

    func testAuthenticateMessageCarriesTheVectorResponse() {
        let challenge = NtlmChallenge(flags: Ntlm.clientFlags, serverChallenge: SmbText.bytes(hex: "0123456789abcdef"), targetInfo: targetInfo, timestamp: 0)
        let auth = Ntlm.authenticate(challenge: challenge, user: "Domain\\User", password: "Password", clientChallenge: [UInt8](repeating: 0xaa, count: 8))
        let reader = SmbReader(auth.message)
        XCTAssertEqual(reader.u32(8), 3)
        let nt = reader.slice(Int(reader.u32(24)), Int(reader.u16(20)))
        XCTAssertEqual(SmbText.hex(Array(nt.prefix(16))), "68cd0ab851e51c96aabc927bebef6a1c")
        XCTAssertEqual(SmbText.string(reader.slice(Int(reader.u32(32)), Int(reader.u16(28)))), "Domain")
        XCTAssertEqual(SmbText.string(reader.slice(Int(reader.u32(40)), Int(reader.u16(36)))), "User")
        XCTAssertEqual(SmbText.string(reader.slice(Int(reader.u32(48)), Int(reader.u16(44)))), "FORKMESH")
        XCTAssertEqual(reader.u16(12), 24)
        XCTAssertEqual(SmbText.hex(auth.sessionKey), "8de40ccadbc14a82f15cb0ad0de95ca3")
    }

    func testAnonymousAuthenticateIsEmpty() {
        let challenge = NtlmChallenge(flags: 0, serverChallenge: [UInt8](repeating: 1, count: 8), targetInfo: [], timestamp: nil)
        let auth = Ntlm.authenticate(challenge: challenge, user: "", password: "", clientChallenge: [UInt8](repeating: 2, count: 8), anonymous: true)
        let reader = SmbReader(auth.message)
        XCTAssertEqual(reader.u16(12), 0)
        XCTAssertEqual(reader.u16(20), 0)
        XCTAssertEqual(reader.u16(36), 0)
        XCTAssertNotEqual(reader.u32(60) & Ntlm.negotiateAnonymous, 0)
        XCTAssertTrue(auth.sessionKey.isEmpty)
    }

    func testSpnegoWrapsAndChallengeIsFound() {
        let token = Spnego.initToken(Ntlm.negotiateMessage())
        XCTAssertEqual(token.first, 0x60)
        XCTAssertNotNil(Ntlm.find(token))
        XCTAssertEqual(Ntlm.find(token).map { SmbReader($0).u32(8) }, 1)
        let big = Spnego.responseToken([UInt8](repeating: 7, count: 300))
        XCTAssertEqual(Array(big.prefix(4)), [0xa1, 0x82, 0x01, 0x38])
        XCTAssertNil(Ntlm.parseChallenge([1, 2, 3]))
    }

    func testTimestampAndFileTime() {
        var info = SmbWriter()
        info.u16(7)
        info.u16(8)
        info.u64(42)
        info.u32(0)
        XCTAssertEqual(Ntlm.timestamp(in: info.bytes), 42)
        let date = Date(timeIntervalSince1970: 1_700_000_000)
        XCTAssertEqual(Ntlm.date(fileTime: Ntlm.fileTime(date))?.timeIntervalSince1970 ?? 0, 1_700_000_000, accuracy: 0.001)
        XCTAssertNil(Ntlm.date(fileTime: 0))
        XCTAssertEqual(Ntlm.splitUser(" WORK\\bob ").domain, "WORK")
        XCTAssertEqual(Ntlm.splitUser("bob").user, "bob")
    }
}

final class SmbClientTests: XCTestCase {
    var state: FakeSmbState!

    override func setUp() {
        state = FakeSmbState()
    }

    func client(share: String = "usb", user: String = "alice", password: String = "secret", guest: Bool = false) -> SmbClient {
        SmbClient(host: "router.lan", share: share, credentials: SmbCredentials(user: user, password: password, guest: guest), connector: state.connector())
    }

    func testListsTheShareRootWithoutDotEntries() async throws {
        let entries = try await client().list("")
        XCTAssertEqual(Set(entries.map(\.name)), [".hidden", "docs", "photo.jpg"])
        let docs = try XCTUnwrap(entries.first { $0.name == "docs" })
        XCTAssertNotEqual(docs.attributes & SmbAttributes.directory, 0)
        let nested = try await client().list("docs/deep")
        XCTAssertEqual(nested.map(\.name), ["a.md"])
        XCTAssertEqual(nested.first?.size, 3)
    }

    func testReadsInChunksAndThroughPendingReplies() async throws {
        state.maxRead = 4
        state.pendingReads = true
        let smb = client()
        let progress = Recorder()
        let data = try await smb.read("docs/notes.txt", progress: { done, total in progress.add(done, total) })
        XCTAssertEqual(String(decoding: data, as: UTF8.self), "hello from the router")
        XCTAssertGreaterThan(progress.count, 4)
        await XCTAssertThrowsSmb(try await smb.read("docs/notes.txt", limit: 3), SmbError.tooLarge.message)
    }

    func testWritesCreatesRenamesAndDeletes() async throws {
        let smb = client()
        try await smb.write("docs/new.txt", data: Array("fresh text".utf8))
        let awaited1 = try await smb.read("docs/new.txt")
        XCTAssertEqual(String(decoding: awaited1, as: UTF8.self), "fresh text")
        try await smb.makeFolder("docs/folder")
        try await smb.makeFile("docs/folder/empty.txt")
        await XCTAssertThrowsSmb(try await smb.makeFolder("docs/folder"), "Something with that name is already there.")
        try await smb.rename("docs/new.txt", to: "docs/folder/moved.txt")
        let awaited2 = try await smb.list("docs/folder").map(\.name)
        XCTAssertEqual(Set(awaited2), ["empty.txt", "moved.txt"])
        try await smb.delete("docs/folder/moved.txt", isFolder: false)
        try await smb.delete("docs", isFolder: true)
        let awaited3 = try await smb.list("").map(\.name)
        XCTAssertEqual(Set(awaited3), [".hidden", "photo.jpg"])
        await XCTAssertThrowsSmb(try await smb.list("docs"), "That file or folder no longer exists.")
        await XCTAssertThrowsSmb(try await smb.delete("", isFolder: true), "Access denied.")
    }

    func testDownloadAndUploadStreamThroughFiles() async throws {
        let smb = client()
        let temp = FileTransfer.temporaryURL("notes.txt")
        defer { FileTransfer.discard(temp) }
        try await smb.download("docs/notes.txt", to: temp)
        XCTAssertEqual(try String(contentsOf: temp, encoding: .utf8), "hello from the router")
        try await smb.upload(temp, to: "copy.txt")
        let awaited4 = try await smb.read("copy.txt")
        XCTAssertEqual(String(decoding: awaited4, as: UTF8.self), "hello from the router")
    }

    func testWrongPasswordAndMissingShare() async {
        await XCTAssertThrowsSmb(try await client(password: "nope").list(""), "The user name or password is wrong.")
        await XCTAssertThrowsSmb(try await client(share: "backup").list(""), "There is no share named backup on router.lan.")
    }

    func testGuestFallsBackToAnonymous() async throws {
        state.users["guest"] = "something else"
        let smb = client(user: "", password: "", guest: true)
        let awaited5 = try await smb.list("docs").count
        XCTAssertEqual(awaited5, 2)
        let awaited6 = await smb.guestSession
        XCTAssertTrue(awaited6)
        XCTAssertEqual(state.lastUser, "")
        state.guestAllowed = false
        await XCTAssertThrowsSmb(try await client(user: "", password: "", guest: true).list(""), "The user name or password is wrong.")
    }

    func testGuestNameMapsToGuestFirst() async throws {
        let smb = client(user: "", password: "", guest: true)
        _ = try await smb.list("")
        XCTAssertEqual(state.lastUser, "guest")
        let awaited7 = await smb.guestSession
        XCTAssertTrue(awaited7)
    }

    func testSigningWhenTheServerRequiresIt() async throws {
        state.requireSigning = true
        let smb = client()
        try await smb.write("signed.txt", data: [1, 2, 3])
        let awaited8 = try await smb.read("signed.txt")
        XCTAssertEqual(awaited8, [1, 2, 3])
        XCTAssertEqual(state.badSignatures, 0)
        XCTAssertGreaterThan(state.signedRequests, 5)
    }

    func testRefusesServersWithoutSmb21() async {
        state.dialect = 0x0311
        await XCTAssertThrowsSmb(try await client().list(""), "router.lan does not speak SMB 2.1.")
    }

    func testReconnectsOnceAfterADroppedConnection() async throws {
        let smb = client()
        _ = try await smb.list("")
        state.dropNextRead = true
        let awaited9 = try await smb.list("docs").count
        XCTAssertEqual(awaited9, 2)
        XCTAssertEqual(state.connections, 2)
    }

    func testUnreachableHost() async {
        let smb = SmbClient(host: "10.0.0.9", share: "usb", credentials: SmbCredentials(guest: true)) { _, _ in throw SmbTransportError("refused") }
        await XCTAssertThrowsSmb(try await smb.list(""), "Could not reach 10.0.0.9. Is file sharing on?")
    }

    func testListsDiskSharesOverSrvsvc() async throws {
        let shares = try await client(share: "").shares()
        XCTAssertEqual(shares.map(\.name), ["media", "usb"])
        XCTAssertEqual(shares.last?.remark, "USB disk")
    }

    func testSerializesConcurrentCalls() async throws {
        let smb = client()
        async let first = smb.list("")
        async let second = smb.read("docs/notes.txt")
        async let third = smb.list("docs")
        let (a, b, c) = try await (first, second, third)
        XCTAssertEqual(a.count, 3)
        XCTAssertEqual(b.count, 21)
        XCTAssertEqual(c.count, 2)
    }

    func testStatusMessages() {
        XCTAssertEqual(SmbStatus.message(0xC000_0101, host: "h", share: "s"), "The folder is not empty.")
        XCTAssertEqual(SmbStatus.message(0xC000_0043, host: "h", share: "s"), "The file is open somewhere else.")
        XCTAssertEqual(SmbStatus.message(0xC000_007F, host: "h", share: "s"), "The disk is full.")
        XCTAssertEqual(SmbStatus.message(0xC000_00A2, host: "h", share: "s"), "The share is read only.")
        XCTAssertEqual(SmbStatus.message(0xC000_0035, host: "h", share: "s"), "Something with that name is already there.")
        XCTAssertEqual(SmbStatus.message(0xC000_003A, host: "h", share: "s"), "That file or folder no longer exists.")
        XCTAssertEqual(SmbStatus.message(0xC000_00BB, host: "h", share: "s"), "The file server answered 0xC00000BB.")
        XCTAssertEqual(SmbError.notLocal.message, "Network shares must be on your local network.")
    }

    func testShareInfoFilter() {
        XCTAssertTrue(SmbShareInfo(name: "usb", type: 0, remark: "").isBrowsableDisk)
        XCTAssertFalse(SmbShareInfo(name: "C$", type: 0x8000_0000, remark: "").isBrowsableDisk)
        XCTAssertFalse(SmbShareInfo(name: "secret$", type: 0, remark: "").isBrowsableDisk)
        XCTAssertFalse(SmbShareInfo(name: "IPC$", type: 3, remark: "").isBrowsableDisk)
    }
}

final class Recorder: @unchecked Sendable {
    private let lock = NSLock()
    private var values: [(Int64, Int64)] = []
    func add(_ done: Int64, _ total: Int64) { lock.withLock { values.append((done, total)) } }
    var count: Int { lock.withLock { values.count } }
}

func XCTAssertThrowsSmb<T>(_ expression: @autoclosure () async throws -> T, _ message: String, file: StaticString = #filePath, line: UInt = #line) async {
    do {
        _ = try await expression()
        XCTFail("expected \(message)", file: file, line: line)
    } catch {
        XCTAssertEqual((error as? LocalizedError)?.errorDescription ?? "\(error)", message, file: file, line: line)
    }
}
