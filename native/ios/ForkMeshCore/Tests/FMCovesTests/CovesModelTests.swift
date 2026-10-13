import FMApi
import FMBase
import XCTest

@testable import FMCoves

@MainActor
private final class FakeRepository {
    var files: [String: [UInt8]] = [:]
    var proposals: [(title: String, head: String, patch: String)] = []
    var accounts: [String: CoveAccountCheck] = [:]
    var failPropose = false

    var io: CoveRepositoryIO {
        CoveRepositoryIO(
            listFiles: { _ in self.files.keys.sorted().map { CoveFileRef(path: $0, size: self.files[$0]?.count ?? 0) } + [CoveFileRef(path: ".forkmesh/coves/README.md")] },
            readFile: { _, path, _ in
                guard let bytes = self.files[path] else { throw ApiException("missing") }
                return bytes
            },
            propose: { _, title, head, patch, _ in
                if self.failPropose { throw ApiException("inbox_full") }
                self.proposals.append((title, head, patch))
            },
            checkAccount: { name in self.accounts[name] ?? CoveAccountCheck(exists: false, active: false) })
    }
}

final class CovesModelTests: XCTestCase {
    private let repo = RepoRef(owner: "alice", name: "plans")

    @MainActor func testLoadsDesktopCovesAndOpensAccountCovesAutomatically() async {
        let fake = FakeRepository()
        fake.files[".forkmesh/coves/aaaa.cove"] = Array(DesktopCoveVectors.account.utf8)
        fake.files[".forkmesh/coves/bbbb.cove"] = Array(DesktopCoveVectors.password.utf8)
        fake.files[".forkmesh/coves/cccc.cove"] = Array("garbage".utf8)
        let model = CovesModel(repo: repo, account: "Bob", io: fake.io, rounds: 20)
        await model.load()
        XCTAssertEqual(model.phase, .loaded)
        XCTAssertEqual(model.entries.count, 2)
        XCTAssertEqual(model.skipped, 1)
        XCTAssertEqual(model.unlockedCoves.map(\.name), ["Investor list"])
        XCTAssertEqual(model.lockedCoves.count, 1)
        let passwordId = model.lockedCoves[0].id
        let wrong = await model.unlock(passwordId, password: "nope")
        XCTAssertFalse(wrong)
        let right = await model.unlock(passwordId, password: "correct horse battery staple")
        XCTAssertTrue(right)
        XCTAssertEqual(model.cove(passwordId)?.documents.count, 2)
        model.lock(passwordId)
        XCTAssertEqual(model.cove(passwordId)?.unlocked, false)
        XCTAssertEqual(model.cove(passwordId)?.documents, [])
        XCTAssertEqual(model.shareLink("x"), "forkmesh://cove/alice/plans/x")
    }

    @MainActor func testSavingProposesAReplacementEnvelope() async throws {
        let fake = FakeRepository()
        let path = ".forkmesh/coves/bbbb.cove"
        fake.files[path] = Array(DesktopCoveVectors.password.utf8)
        let model = CovesModel(repo: repo, account: "Bob", openerKey: "bobKey", io: fake.io, rounds: 20)
        await model.load()
        let id = model.entries[0].cove.id
        _ = await model.unlock(id, password: "correct horse battery staple")
        try await model.saveDocument(coveId: id, name: "notes.md", body: "changed")
        XCTAssertEqual(fake.proposals.count, 1)
        XCTAssertEqual(fake.proposals[0].title, "cove: bbbb")
        XCTAssertTrue(fake.proposals[0].head.hasPrefix("bob:mobile-cove/bbbb-"))
        let patch = fake.proposals[0].patch
        XCTAssertTrue(patch.hasPrefix("diff --git a/\(path) b/\(path)\nindex \(CoveSha1.gitBlobId(fake.files[path]!))..") )
        XCTAssertFalse(patch.contains("changed"))
        XCTAssertTrue(model.entries[0].pendingReview)
        let newText = patch.split(separator: "\n").filter { $0.hasPrefix("+") && !$0.hasPrefix("+++") }.map { String($0.dropFirst()) }.joined(separator: "\n") + "\n"
        var reopened = try XCTUnwrap(CoveStore.parseEnvelope(Array(newText.utf8)))
        XCTAssertTrue(CoveStore.unlock(&reopened, password: "correct horse battery staple"))
        XCTAssertEqual(reopened.document(named: "notes.md")?.body, "changed")
        XCTAssertEqual(reopened.accessLog.last?.action, "edit")
        XCTAssertEqual(reopened.accessLog.last?.who, "bobKey")
        XCTAssertEqual(reopened.name, "Launch plans \u{2014} Q4")
    }

    @MainActor func testCreateAddDocumentAndInvite() async throws {
        let fake = FakeRepository()
        fake.accounts["carol"] = CoveAccountCheck(exists: true, active: true)
        fake.accounts["dave"] = CoveAccountCheck(exists: true, active: false)
        let model = CovesModel(repo: repo, account: "Alice", io: fake.io, rounds: 20)
        let created = try await model.createAccountCove(name: "Board")
        XCTAssertTrue(fake.proposals[0].patch.contains("new file mode 100644"))
        let template = try XCTUnwrap(CoveFiles.template("lead-pipeline"))
        let name = try await model.addDocument(coveId: created.id, template: template)
        XCTAssertEqual(name, "leads.pipeline.json")
        let second = try await model.addDocument(coveId: created.id, template: template)
        XCTAssertEqual(second, "leads-2.pipeline.json")
        XCTAssertTrue(fake.proposals[2].patch.contains("new file mode 100644"))
        do {
            try await model.invite(coveId: created.id, account: "zed")
            XCTFail("unknown account")
        } catch { XCTAssertEqual(error as? CoveActionError, .invalidAccount("zed")) }
        do {
            try await model.invite(coveId: created.id, account: "dave")
            XCTFail("inactive account")
        } catch { XCTAssertEqual(error as? CoveActionError, .inactiveAccount("dave")) }
        do {
            try await model.invite(coveId: created.id, account: "alice")
            XCTFail("member")
        } catch { XCTAssertEqual(error as? CoveActionError, .alreadyMember("alice")) }
        try await model.invite(coveId: created.id, account: " Carol")
        XCTAssertEqual(model.cove(created.id)?.members, ["alice", "carol"])
        let last = try XCTUnwrap(fake.proposals.last?.patch)
        let text = last.split(separator: "\n").filter { $0.hasPrefix("+") && !$0.hasPrefix("+++") }.map { String($0.dropFirst()) }.joined(separator: "\n")
        var asCarol = try XCTUnwrap(CoveStore.parseEnvelope(Array(text.utf8)))
        XCTAssertTrue(CoveStore.unlockForAccount(&asCarol, account: "carol"))
        XCTAssertEqual(asCarol.documents.count, 2)

        let other = CovesModel(repo: repo, account: "carol", io: fake.io, rounds: 20)
        fake.files[created.relPath] = Array(text.utf8) + [0x0A]
        await other.load()
        do {
            try await other.invite(coveId: created.id, account: "carol2")
            XCTFail("only creator")
        } catch { XCTAssertEqual(error as? CoveActionError, .onlyCreatorShares) }
    }

    @MainActor func testInviteRelaysOrFallsBackToTheShareLink() async throws {
        let fake = FakeRepository()
        fake.accounts["carol"] = CoveAccountCheck(exists: true, active: true)
        fake.accounts["dave"] = CoveAccountCheck(exists: true, active: true)
        let model = CovesModel(repo: repo, account: "Alice", io: fake.io, rounds: 20)
        let created = try await model.createAccountCove(name: "Board")
        let unwired = try await model.invite(coveId: created.id, account: "carol")
        XCTAssertEqual(unwired, .shareLink("forkmesh://cove/alice/plans/\(created.id)"))
        var sent: [CoveInviteNotice] = []
        var relayUp = true
        model.notifier = CoveNotifier(
            invite: { (notice: CoveInviteNotice) -> Bool in
                sent.append(notice)
                return relayUp
            },
            opened: { (_: CoveOpenNotice) -> Bool in true })
        fake.accounts["erin"] = CoveAccountCheck(exists: true, active: true)
        let relayed = try await model.invite(coveId: created.id, account: " Dave")
        XCTAssertEqual(relayed, .relayed)
        XCTAssertEqual(sent.last?.inviteeAccount, "dave")
        XCTAssertEqual(sent.last?.coveName, "Board")
        XCTAssertEqual(sent.last?.inviterName, "alice")
        XCTAssertEqual(sent.last?.shareUrl, model.shareLink(created.id))
        relayUp = false
        let fallback = try await model.invite(coveId: created.id, account: "erin")
        XCTAssertEqual(fallback, .shareLink(model.shareLink(created.id)))
        XCTAssertEqual(sent.count, 2)
    }

    @MainActor func testOpeningANotifyingCoveSignsTheDesktopCanonicalOnce() async throws {
        let fake = FakeRepository()
        let owner = CovesModel(repo: repo, account: "Alice", openerKey: "aliceKey", io: fake.io, rounds: 20)
        let created = try await owner.createPasswordCove(name: "Plans", password: "pw", notifyOnOpen: true)
        var ownerOpens = 0
        owner.notifier = CoveNotifier(invite: { (_: CoveInviteNotice) -> Bool in true }, opened: { (_: CoveOpenNotice) -> Bool in
            ownerOpens += 1
            return true
        })
        XCTAssertFalse(owner.noteOpened(created.id))
        let patch = try XCTUnwrap(fake.proposals.last?.patch)
        let text = patch.split(separator: "\n").filter { $0.hasPrefix("+") && !$0.hasPrefix("+++") }.map { String($0.dropFirst()) }.joined(separator: "\n")
        fake.files[created.relPath] = Array(text.utf8) + [0x0A]

        let reader = CovesModel(repo: repo, account: "Bob", openerKey: "bobKey", io: fake.io, rounds: 20)
        var opens: [CoveOpenNotice] = []
        reader.notifier = CoveNotifier(invite: { (_: CoveInviteNotice) -> Bool in true }, opened: { (notice: CoveOpenNotice) -> Bool in
            opens.append(notice)
            return true
        })
        await reader.load()
        XCTAssertFalse(reader.noteOpened(created.id))
        let unlocked = await reader.unlock(created.id, password: "pw")
        XCTAssertTrue(unlocked)
        XCTAssertTrue(reader.noteOpened(created.id))
        XCTAssertFalse(reader.noteOpened(created.id))
        XCTAssertEqual(opens.count, 1)
        let notice = try XCTUnwrap(opens.first)
        XCTAssertEqual(notice.creatorKey, "aliceKey")
        XCTAssertEqual(notice.openerKey, "bobKey")
        XCTAssertEqual(notice.openerName, "bob")
        XCTAssertEqual(notice.coveName, "Plans")
        XCTAssertEqual(
            String(decoding: notice.canonical, as: UTF8.self),
            "forkmesh-cove-open-v1\n\(created.id)\naliceKey\nbobKey\n\(notice.ts)")
        reader.lock(created.id)
        _ = await reader.unlock(created.id, password: "pw")
        XCTAssertTrue(reader.noteOpened(created.id))
        XCTAssertEqual(ownerOpens, 0)
    }

    @MainActor func testFailuresLeaveStateAlone() async throws {
        let fake = FakeRepository()
        fake.failPropose = true
        let model = CovesModel(repo: repo, account: "Alice", io: fake.io, rounds: 20)
        do {
            _ = try await model.createPasswordCove(name: "X", password: "pw")
            XCTFail("should fail")
        } catch {
            guard case .submit = error as? CoveActionError else { return XCTFail("\(error)") }
        }
        XCTAssertTrue(model.entries.isEmpty)
        let signedOut = CovesModel(repo: repo, account: "", io: fake.io, rounds: 20)
        do {
            _ = try await signedOut.createAccountCove(name: "X")
            XCTFail("signed out")
        } catch { XCTAssertEqual(error as? CoveActionError, .signInRequired) }
    }
}
