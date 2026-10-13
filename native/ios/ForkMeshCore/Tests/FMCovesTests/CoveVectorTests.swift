import FMBase
import XCTest

@testable import FMCoves

final class CoveVectorTests: XCTestCase {
    private func parse(_ text: String, path: String = ".forkmesh/coves/abc.cove") -> Cove {
        guard let cove = CoveStore.parseEnvelope(Array(text.utf8), path: path) else {
            XCTFail("envelope did not parse")
            return Cove()
        }
        return cove
    }

    func testDesktopPasswordCoveUnlocks() {
        var cove = parse(DesktopCoveVectors.password)
        XCTAssertEqual(cove.version, 2)
        XCTAssertEqual(cove.name, "")
        XCTAssertEqual(cove.accessMode, "")
        XCTAssertEqual(cove.grants.count, 4)
        XCTAssertEqual(cove.slug, "abc")
        var wrong = cove
        XCTAssertFalse(CoveStore.unlock(&wrong, password: "correct horse battery stapl"))
        XCTAssertFalse(wrong.unlocked)
        XCTAssertTrue(CoveStore.unlock(&cove, password: "correct horse battery staple"))
        XCTAssertEqual(cove.name, "Launch plans \u{2014} Q4")
        XCTAssertEqual(cove.accessMode, "password")
        XCTAssertEqual(cove.creator, "creatorPubKeyB64url")
        XCTAssertEqual(cove.createdAtMs, 1_758_700_000_000)
        XCTAssertTrue(cove.notifyOnOpen)
        XCTAssertEqual(cove.documents.map(\.name), ["notes.md", "sheet.csv"])
        XCTAssertEqual(cove.documents[0].body, "# Notes\n\nSecret \u{1F511} plan\n")
        XCTAssertEqual(cove.documents[1].mime, "text/csv")
        XCTAssertEqual(cove.documents[1].body, "A,B\r\n1,2\r\n")
        XCTAssertEqual(cove.accessLog, [CoveAccessEntry(who: "openerKey", name: "bob", node: "node1", ts: 1_758_700_003_000, action: "edit")])
    }

    func testDesktopAccountCoveUnlocksForEveryMemberOnly() {
        let locked = parse(DesktopCoveVectors.account)
        XCTAssertEqual(locked.grants.count, 4)
        for member in ["alice", " Bob", "CAROL "] {
            var cove = locked
            XCTAssertTrue(CoveStore.unlockForAccount(&cove, account: member), member)
            XCTAssertEqual(cove.name, "Investor list")
            XCTAssertEqual(cove.accessMode, "account")
            XCTAssertEqual(cove.creatorAccount, "alice")
            XCTAssertEqual(cove.invitedAccounts, ["alice", "bob", "carol"])
            XCTAssertEqual(Hex.encode(cove.contentKey), "1f1e1d1c1b1a191817161514131211100f0e0d0c0b0a09080706050403020100")
            XCTAssertEqual(cove.documents.first?.name, "leads.pipeline.json")
        }
        var outsider = locked
        XCTAssertFalse(CoveStore.unlockForAccount(&outsider, account: "zed"))
        var password = locked
        XCTAssertFalse(CoveStore.unlock(&password, password: "alice"))
    }

    func testLegacyV1Envelopes() {
        let legacy = parse(DesktopCoveVectors.legacyAccount)
        XCTAssertEqual(legacy.version, 1)
        XCTAssertEqual(legacy.name, "Legacy account cove")
        XCTAssertEqual(legacy.accessMode, "account")
        XCTAssertTrue(CoveStore.accountCanAccess(legacy, account: "Erin"))
        XCTAssertEqual(CoveStore.accountSecretV1(legacy), "Hufy1KlPzJD2c741SrzdVLJkR5m4mda3d08X7714ke8")
        var erin = legacy
        XCTAssertTrue(CoveStore.unlockForAccount(&erin, account: "erin"))
        XCTAssertEqual(erin.documents.first?.body, "legacy body")
        XCTAssertEqual(erin.name, "Legacy account cove")
        var mallory = legacy
        XCTAssertFalse(CoveStore.unlockForAccount(&mallory, account: "mallory"))

        var old = parse(DesktopCoveVectors.legacyPassword)
        XCTAssertEqual(old.accessMode, "password")
        XCTAssertTrue(old.notifyOnOpen)
        XCTAssertTrue(CoveStore.unlock(&old, password: "hunter2"))
        XCTAssertEqual(old.name, "Old password cove")
        XCTAssertEqual(old.documents.first?.name, "old.md")
    }

    func testLegacyAccountCoveUpgradesToAnonymousV2OnSave() throws {
        var erin = parse(DesktopCoveVectors.legacyAccount)
        XCTAssertTrue(CoveStore.unlockForAccount(&erin, account: "erin"))
        XCTAssertTrue(erin.contentKey.isEmpty)
        let sealed = try CoveStore.sealAccount(erin)
        XCTAssertEqual(sealed.contentKey.count, 32)
        let text = CoveStore.envelopeText(sealed)
        XCTAssertFalse(text.contains("erin"))
        XCTAssertFalse(text.contains("Legacy"))
        var reopened = try XCTUnwrap(CoveStore.parseEnvelope(Array(text.utf8)))
        XCTAssertEqual(reopened.version, 2)
        XCTAssertTrue(CoveStore.unlockForAccount(&reopened, account: "dave"))
        XCTAssertEqual(reopened.documents.first?.body, "legacy body")
    }

    func testRejectsEnvelopesThatAreNotCoves() {
        XCTAssertNil(CoveStore.parseEnvelope(Array("{\"kind\":\"note\",\"id\":\"x\"}".utf8)))
        XCTAssertNil(CoveStore.parseEnvelope(Array("{\"kind\":\"cove\",\"id\":\"x\",\"kdf\":{\"rounds\":0,\"salt\":\"AA==\"}}".utf8)))
        XCTAssertNil(CoveStore.parseEnvelope(Array("not json".utf8)))
    }
}
