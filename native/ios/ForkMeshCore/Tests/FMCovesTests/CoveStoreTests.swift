import FMBase
import XCTest

@testable import FMCoves

final class CoveStoreTests: XCTestCase {
    func testNewPasswordCoveRoundTripsAndHidesEverything() throws {
        let doc = CoveDocument(id: "d1", name: "plan.md", body: "secret launch plan", updatedAtMs: 5)
        let sealed = try CoveStore.newPasswordCove(
            name: "  Launch  ", password: "pw", creator: "creator-account-one", notifyOnOpen: true, documents: [doc], nowMs: 42, rounds: 50)
        XCTAssertEqual(sealed.name, "Launch")
        XCTAssertEqual(sealed.slug.count, 32)
        XCTAssertEqual(sealed.relPath, ".forkmesh/coves/\(sealed.slug).cove")
        XCTAssertEqual(sealed.grants.count, 4)
        let text = CoveStore.envelopeText(sealed)
        XCTAssertTrue(text.hasSuffix("}\n"))
        for leak in ["Launch", "secret launch plan", "creator-account-one", "password", "notify"] { XCTAssertFalse(text.contains(leak), leak) }
        var reopened = try XCTUnwrap(CoveStore.parseEnvelope(Array(text.utf8)))
        XCTAssertTrue(CoveStore.unlock(&reopened, password: "pw"))
        XCTAssertEqual(reopened.name, "Launch")
        XCTAssertEqual(reopened.documents, [doc])
        XCTAssertEqual(reopened.createdAtMs, 42)
        XCTAssertTrue(reopened.notifyOnOpen)
        XCTAssertThrowsError(try CoveStore.newPasswordCove(name: " ", password: "pw", creator: "", notifyOnOpen: false, nowMs: 0))
        XCTAssertThrowsError(try CoveStore.newPasswordCove(name: "x", password: "", creator: "", notifyOnOpen: false, nowMs: 0))
    }

    func testAccountCoveGrantsArePaddedToBlocksAndShuffled() throws {
        let sealed = try CoveStore.newAccountCove(
            name: "Team", creatorAccount: "Alice", invitedAccounts: ["b", "c", "d", "e"], creator: "k", notifyOnOpen: false,
            nowMs: 1, rounds: 20)
        XCTAssertEqual(sealed.invitedAccounts, ["alice", "b", "c", "d", "e"])
        XCTAssertEqual(sealed.grants.count, 8)
        let text = CoveStore.envelopeText(sealed)
        for leak in ["alice", "Team", "account"] { XCTAssertFalse(text.contains("\"\(leak)\""), leak) }
        for member in ["alice", "b", "e"] {
            var copy = try XCTUnwrap(CoveStore.parseEnvelope(Array(text.utf8)))
            XCTAssertTrue(CoveStore.unlockForAccount(&copy, account: member), member)
            XCTAssertEqual(copy.contentKey, sealed.contentKey)
        }
        var outsider = try XCTUnwrap(CoveStore.parseEnvelope(Array(text.utf8)))
        XCTAssertFalse(CoveStore.unlockForAccount(&outsider, account: "f"))
        for grant in sealed.grants {
            XCTAssertEqual(CoveBase64.decode(grant.nonce).count, 12)
            XCTAssertEqual(CoveBase64.decode(grant.tag).count, 16)
            XCTAssertEqual(CoveBase64.decode(grant.body).count, 32)
        }
    }

    func testSealRulesMatchDesktop() throws {
        var cove = Cove()
        cove.accessMode = "account"
        XCTAssertThrowsError(try CoveStore.sealPassword(cove, password: "x")) { XCTAssertEqual($0 as? CoveStoreError, .accountScoped) }
        XCTAssertThrowsError(try CoveStore.sealAccount(cove)) { XCTAssertEqual($0 as? CoveStoreError, .locked) }
        cove.accessMode = "password"
        XCTAssertThrowsError(try CoveStore.sealAccount(cove)) { XCTAssertEqual($0 as? CoveStoreError, .notAccountScoped) }
        XCTAssertThrowsError(try CoveStore.sealPassword(cove, password: "x")) { XCTAssertEqual($0 as? CoveStoreError, .missingParameters) }
    }

    func testAccessLogIsBounded() {
        var cove = Cove()
        for i in 0..<510 { CoveStore.appendAccess(&cove, CoveAccessEntry(ts: Int64(i))) }
        XCTAssertEqual(cove.accessLog.count, 500)
        XCTAssertEqual(cove.accessLog.first?.ts, 10)
    }

    func testAccountHelpers() {
        XCTAssertEqual(CoveStore.normalizedAccounts([" Bob", "alice", "BOB", ""]), ["alice", "bob"])
        XCTAssertEqual(CoveStore.cleanPath("a//b/./c/../d.md"), "a/b/d.md")
        XCTAssertEqual(CoveStore.slug(fromPath: ".forkmesh/coves/0123.cove"), "0123")
        var cove = Cove()
        cove.accessMode = "account"
        cove.creatorAccount = "Alice"
        cove.invitedAccounts = ["bob"]
        XCTAssertEqual(cove.members, ["alice", "bob"])
        XCTAssertTrue(cove.createdBy(account: " ALICE"))
        XCTAssertFalse(cove.createdBy(account: "bob"))
        XCTAssertEqual(
            String(decoding: CoveStore.openCanonical(coveId: "c", creatorKey: "k", openerKey: "o", ts: 9), as: UTF8.self),
            "forkmesh-cove-open-v1\nc\nk\no\n9")
    }
}
