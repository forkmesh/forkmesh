import FMBase
import XCTest

@testable import FMVault

private final class MemoryKeys: VaultAutofillKeyStore {
    var key: [UInt8]?
    var writes = 0
    func readKey() throws -> [UInt8]? { key }
    func writeKey(_ key: [UInt8]) throws {
        self.key = key
        writes += 1
    }
    func deleteKey() throws { key = nil }
}

private final class MemoryBlobs: VaultAutofillBlobStore {
    var blob: Data?
    func readBlob() throws -> Data? { blob }
    func writeBlob(_ data: Data) throws { blob = data }
    func deleteBlob() throws { blob = nil }
}

final class VaultAutofillCacheTests: XCTestCase {
    private let logins = [
        VaultAutofillLogin(id: "1", title: "GitHub", username: "octo", password: "correct-horse-battery-1", host: "github.com"),  // forkmesh-secret-scan:ignore-line
        VaultAutofillLogin(id: "2", title: "Bank", username: "me", password: "correct-horse-battery-2", host: "bank.example"),  // forkmesh-secret-scan:ignore-line
        VaultAutofillLogin(id: "3", title: "No host", username: "x", password: "correct-horse-battery-3", host: ""),  // forkmesh-secret-scan:ignore-line
        VaultAutofillLogin(id: "4", title: "No password", username: "x", password: "", host: "a.com"),
        VaultAutofillLogin(id: "1", title: "Dup", username: "d", password: "p", host: "d.com"),
    ]

    func testSealedCacheHoldsNoPlaintextAndOpensWithItsKey() throws {
        let key = [UInt8](repeating: 9, count: 32)
        let sealed = try VaultAutofillCache.seal(account: "alice", logins: logins, key: key)
        let text = String(decoding: sealed, as: UTF8.self)
        for leak in ["correct-horse-battery-1", "correct-horse-battery-2", "github.com", "bank.example"] { XCTAssertFalse(text.contains(leak), leak) }
        let opened = try VaultAutofillCache.open(sealed, key: key)
        XCTAssertEqual(opened.account, "alice")
        XCTAssertEqual(opened.logins.map(\.id), ["1", "2"])
        XCTAssertThrowsError(try VaultAutofillCache.open(sealed, key: [UInt8](repeating: 8, count: 32)))
        var tampered = [UInt8](sealed)
        tampered[tampered.count / 2] ^= 1
        XCTAssertThrowsError(try VaultAutofillCache.open(Data(tampered), key: key))
    }

    func testIdentitiesCarryNoPasswords() {
        let identities = VaultAutofillCache.identities(logins)
        XCTAssertEqual(identities, [
            VaultAutofillIdentity(recordIdentifier: "1", host: "github.com", user: "octo"),
            VaultAutofillIdentity(recordIdentifier: "2", host: "bank.example", user: "me"),
        ])
    }

    func testExactHostMatching() {
        let github = logins[0]
        XCTAssertTrue(VaultAutofillCache.matches(github, serviceIdentifiers: ["https://github.com/login"]))
        XCTAssertTrue(VaultAutofillCache.matches(github, serviceIdentifiers: ["www.github.com"]))
        XCTAssertTrue(VaultAutofillCache.matches(github, serviceIdentifiers: ["GitHub.com"]))
        XCTAssertFalse(VaultAutofillCache.matches(github, serviceIdentifiers: ["evilgithub.com"]))
        XCTAssertFalse(VaultAutofillCache.matches(github, serviceIdentifiers: ["gist.github.com"]))
        XCTAssertFalse(VaultAutofillCache.matches(github, serviceIdentifiers: []))
        let ranked = VaultAutofillCache.ranked(VaultAutofillCache.usable(logins), serviceIdentifiers: ["github.com"])
        XCTAssertEqual(ranked.suggested.map(\.id), ["1"])
        XCTAssertEqual(ranked.others.map(\.id), ["2"])
        XCTAssertEqual(VaultAutofillCache.ranked(VaultAutofillCache.usable(logins), serviceIdentifiers: [], query: "bank").others.map(\.id), ["2"])
    }

    func testVaultRotatesTheKeyOnEveryStoreAndClears() throws {
        let keys = MemoryKeys()
        let blobs = MemoryBlobs()
        let vault = VaultAutofillVault(keys: keys, blobs: blobs)
        XCTAssertThrowsError(try vault.load())
        try vault.store(account: "alice", logins: logins)
        let firstKey = keys.key
        XCTAssertEqual(try vault.load().logins.count, 2)
        try vault.store(account: "alice", logins: Array(logins.prefix(1)))
        XCTAssertNotEqual(keys.key, firstKey)
        XCTAssertEqual(keys.writes, 2)
        XCTAssertEqual(try vault.load().logins.map(\.id), ["1"])
        vault.clear()
        XCTAssertNil(keys.key)
        XCTAssertNil(blobs.blob)
    }

    func testAPickedLoginIsSuggestedAgainForThatApp() {
        var links = VaultAutofillLinks()
        let github = logins[0]
        XCTAssertEqual(links.link(github, serviceIdentifiers: ["github.com", "https://code.example.app/sign-in", ""]), ["code.example.app"])
        XCTAssertEqual(links.link(github, serviceIdentifiers: ["code.example.app"]), [])
        let ranked = VaultAutofillCache.ranked(VaultAutofillCache.usable(logins), serviceIdentifiers: ["www.code.example.app"], links: links)
        XCTAssertEqual(ranked.suggested.map(\.id), ["1"])
        XCTAssertEqual(ranked.others.map(\.id), ["2"])
        XCTAssertEqual(VaultAutofillCache.identities(logins, links: links).last,
                       VaultAutofillIdentity(recordIdentifier: "1", host: "code.example.app", user: "octo"))

        let reread = VaultAutofillLinks(data: links.data)
        XCTAssertEqual(reread, links)
        XCTAssertEqual(VaultAutofillLinks(data: Data("not json".utf8)), VaultAutofillLinks())

        links.link(logins[1], serviceIdentifiers: ["code.example.app"])
        XCTAssertEqual(links.hosts["code.example.app"], ["2", "1"])
        links.keep(["2"])
        XCTAssertEqual(links.hosts, ["code.example.app": ["2"]])
        links.keep([])
        XCTAssertTrue(links.isEmpty)
    }

    func testAppsWithoutAWebsiteAreLinkedByBundleId() {
        XCTAssertEqual(VaultAutofillCache.host(of: "app:com.Example.Chat"), "app:com.Example.Chat")
        XCTAssertEqual(VaultAutofillCache.host(of: "app:"), "")
        XCTAssertFalse(VaultAutofillCache.matches(logins[0], serviceIdentifiers: ["app:github.com"]))
        var links = VaultAutofillLinks()
        XCTAssertEqual(links.link(logins[1], serviceIdentifiers: ["app:com.Example.Bank"]), ["app:com.Example.Bank"])
        let ranked = VaultAutofillCache.ranked(VaultAutofillCache.usable(logins), serviceIdentifiers: ["app:com.Example.Bank"], links: links)
        XCTAssertEqual(ranked.suggested.map(\.id), ["2"])
        XCTAssertEqual(VaultAutofillCache.identities(logins, links: links).last?.host, "app:com.Example.Bank")
    }

    func testLinksStopGrowingAtTheirCap() {
        var links = VaultAutofillLinks()
        for n in 0..<(VaultAutofillLinks.maxHosts + 5) {
            links.link(logins[0], serviceIdentifiers: ["app\(n).example"])
        }
        XCTAssertEqual(links.hosts.count, VaultAutofillLinks.maxHosts)
        for n in 0..<(VaultAutofillLinks.maxPerHost + 3) {
            links.link(VaultAutofillLogin(id: "id\(n)", title: "", username: "", password: "p", host: "x.example"), serviceIdentifiers: ["app0.example"])
        }
        XCTAssertEqual(links.hosts["app0.example"]?.count, VaultAutofillLinks.maxPerHost)
        XCTAssertEqual(links.hosts["app0.example"]?.first, "id\(VaultAutofillLinks.maxPerHost + 2)")
    }
}
