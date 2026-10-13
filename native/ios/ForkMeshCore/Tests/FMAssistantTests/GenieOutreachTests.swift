import FMBase
import FMCrypto
import FMLocalAI
import FMPackages
import XCTest
@testable import FMAssistant

final class FakeDeckTransport: PackagesTransport {
    var refuse = false
    var requests: [(method: String, path: String, body: JSONObject?)] = []

    func send(_ method: String, _ path: String, query: [(String, String)], body: JSONObject?) async throws -> PackagesReply {
        requests.append((method, path, body))
        if refuse { return PackagesReply(status: 409, json: ["error": "deck_needs_investor_pipeline"]) }
        return PackagesReply(status: 201, json: ["ok": true, "link": .object(["token": "abcdef123456", "url": "https://forkmesh.com/pitch-deck/abcdef123456", "active": true])])
    }
}

@MainActor
final class GenieOutreachTests: XCTestCase {
    private let adaId = String(repeating: "a", count: 32)
    private let bobId = String(repeating: "b", count: 32)
    private let carolId = String(repeating: "c", count: 32)
    private let danId = String(repeating: "d", count: 32)
    private let eveId = String(repeating: "e", count: 32)
    private let finId = String(repeating: "f", count: 32)

    private func organization(flow: String = "Investors") -> CrmOrganizationData {
        CrmOrganizationData(json: [
            "organization": "acme",
            "contacts": .array([
                .object(["id": .string(adaId), "name": "Ada Lovelace", "email": "ada@example.com", "flow": "customers", "stage": "lead"]),
                .object(["id": .string(bobId), "name": "Bob Builder", "email": "Bob@Example.com", "company": "Builders", "title": "CTO", "github": "https://github.com/bobb", "flow": "customers", "stage": "lead", "fields": .object(["photo": "https://img.test/bob.jpg"])]),
                .object(["id": .string(carolId), "name": "Carol", "mastodon": "@carol@social.test", "flow": "customers", "stage": "lead"]),
                .object(["id": .string(danId), "name": "Dan", "email": "dan@example.com", "flow": "customers", "stage": "Not interested"]),
                .object(["id": .string(eveId), "name": "Eve", "email": "eve@example.com", "flow": "customers", "stage": "customer"]),
                .object(["id": .string(finId), "name": "Fin", "phone": "123", "flow": "customers", "stage": "lead", "lastContactedAt": 0]),
            ]),
            "emails": .array([
                .object(["id": "e1", "contactId": .string(adaId), "subject": "Intro", "status": "delivered", "sentAt": 1_790_000_000_000]),
                .object(["id": "e2", "contactId": .string(bobId), "subject": "Hello", "status": "external", "direction": "inbound"]),
                .object(["id": "e3", "contactId": .string(carolId), "subject": "Not yet", "status": "draft"]),
            ]),
            "settings": .object(["fromName": "Sam from ForkMesh", "flows": .array([.object(["id": "customers", "name": .string(flow), "stages": .array(["lead", "qualified", "customer"])])])]),
        ])
    }

    private func pick(_ id: String, flow: String = "Investors") -> GenieOutreachPick {
        let data = organization(flow: flow)
        let contact = data.contacts.first { $0.id == id }!
        return GenieOutreachPick(organization: "acme", contact: contact, flowName: flow, channel: GenieOutreach.channel(contact)!, sender: "Sam")
    }

    func testOnlyOpenReachableContactsNeverEmailedAreCandidates() {
        let data = organization()
        XCTAssertEqual(GenieOutreach.candidates(data).map(\.id), [bobId, carolId])
        XCTAssertEqual(GenieOutreach.candidates(data, skip: [bobId]).map(\.id), [carolId])
        var contacted = data
        contacted.contacts[1].lastContactedAt = 1
        XCTAssertEqual(GenieOutreach.candidates(contacted).map(\.id), [carolId])
        XCTAssertEqual(GenieOutreach.channel(data.contacts[1]), .email)
        XCTAssertEqual(GenieOutreach.channel(data.contacts[2]), .mastodon)
        XCTAssertNil(GenieOutreach.channel(data.contacts[5]))
    }

    func testPickUsesTheCrmSenderAndTheRandomDraw() {
        let data = organization()
        let first = GenieOutreach.pick([data], sender: "ada") { 0 }
        XCTAssertEqual(first?.contact.id, bobId)
        XCTAssertEqual(first?.sender, "Sam from ForkMesh")
        XCTAssertEqual(first?.flowName, "Investors")
        XCTAssertEqual(first?.caption, "Email Bob Builder?")
        let last = GenieOutreach.pick([data]) { 0.99 }
        XCTAssertEqual(last?.contact.id, carolId)
        XCTAssertEqual(last?.channel, .mastodon)
        XCTAssertEqual(last?.caption, "Toot to Carol?")
        XCTAssertNil(GenieOutreach.pick([data], skip: [bobId, carolId]))
    }

    func testBestPutsTheBiggestDealFirstThenANextStepThenEmailThenTheOldest() {
        var data = organization()
        XCTAssertEqual(GenieOutreach.best([data])?.contact.id, bobId)
        data.contacts[2].value = 5_000
        XCTAssertEqual(GenieOutreach.best([data])?.contact.id, carolId)
        data.contacts[1].value = 5_000
        XCTAssertEqual(GenieOutreach.best([data])?.contact.id, bobId)
        data.contacts[2].nextStep = "Send the demo"
        XCTAssertEqual(GenieOutreach.best([data])?.contact.id, carolId)
        data.contacts[2].nextStep = ""
        data.contacts[1].mastodon = "@bob@social.test"
        data.contacts[1].email = ""
        data.contacts[1].createdAt = 200
        data.contacts[2].createdAt = 100
        XCTAssertEqual(GenieOutreach.best([data])?.contact.id, carolId)
        XCTAssertEqual(GenieOutreach.best([data], skip: [carolId])?.contact.id, bobId)
        XCTAssertEqual(GenieOutreach.best([data], skip: [carolId])?.sender, "Sam from ForkMesh")
        XCTAssertNil(GenieOutreach.best([data], skip: [bobId, carolId]))
    }

    func testTheToolboxPicksSomeoneNotYetEmailedFromTheCrm() async {
        let transport = FakeCrmTransport()
        transport.contacts = [
            ["id": .string(adaId), "name": "Ada Lovelace", "email": "ada@example.com", "flow": "customers", "stage": "lead"],
            ["id": .string(bobId), "name": "Bob Builder", "email": "bob@example.com", "flow": "customers", "stage": "lead"],
        ]
        let client = PackagesClient(transport: transport)
        let sources = AssistantSources(account: { AssistantAccount(username: "sam") }, crm: { client })
        let box = ForkMeshToolbox(sources: sources)
        let found = await box.outreachPick(skip: []) { 0 }
        XCTAssertEqual(found?.contact.id, bobId)
        XCTAssertEqual(found?.organization, "acme")
        XCTAssertEqual(found?.sender, "sam")
        let none = await box.outreachPick(skip: [bobId]) { 0 }
        XCTAssertNil(none)
    }

    func testMastodonHandles() {
        XCTAssertEqual(GenieMastodonHandle("@carol@Social.test")?.mention, "@carol@social.test")
        XCTAssertEqual(GenieMastodonHandle("carol@social.test")?.user, "carol")
        XCTAssertEqual(GenieMastodonHandle("https://social.test/@carol")?.mention, "@carol@social.test")
        XCTAssertEqual(GenieMastodonHandle("@carol@social.test")?.lookupURL?.absoluteString, "https://social.test/api/v1/accounts/lookup?acct=carol")
        XCTAssertNil(GenieMastodonHandle("@carol"))
        XCTAssertNil(GenieMastodonHandle("https://social.test/about"))
        XCTAssertNil(GenieMastodonHandle("carol smith@social.test"))
    }

    func testImagesComeFromAPhotoGithubAndGravatar() {
        let bob = pick(bobId).contact
        let urls = GenieOutreach.images(bob).map(\.absoluteString)
        let hash = Sha256.hex(Array("bob@example.com".utf8))
        XCTAssertEqual(urls, [
            "https://img.test/bob.jpg", "https://github.com/bobb.png?size=160", "https://gravatar.com/avatar/\(hash)?s=160&d=404",
        ])
        XCTAssertEqual(GenieOutreach.images(pick(carolId).contact), [])
    }

    func testThePromptSaysWhoTheyAreAndHowToPitch() {
        let email = GenieOutreach.messages(pick(bobId))
        XCTAssertEqual(email.count, 2)
        XCTAssertTrue(email[0].content.contains("Write as Sam."))
        XCTAssertTrue(email[0].content.contains("https://forkmesh.com"))
        XCTAssertTrue(email[1].content.contains("Role: CTO"))
        XCTAssertTrue(email[1].content.contains("Company: Builders"))
        XCTAssertTrue(email[1].content.contains("as an investor"))
        XCTAssertTrue(email[1].content.contains("\"Subject:\""))
        let toot = GenieOutreach.messages(pick(carolId, flow: "Press"))
        XCTAssertTrue(toot[1].content.contains("Start with their handle @carol@social.test"))
        XCTAssertTrue(toot[1].content.contains("as a journalist"))
    }

    func testParsingAnEmailAndAToot() {
        let reply = "Subject: Your builds, everywhere\n\nHi Bob,\n\nBuilders ships a lot of code, and ForkMesh keeps every repository mirrored across hosts.\n\nBest,\nSam"
        let email = GenieOutreach.parse(reply, pick: pick(bobId))
        XCTAssertEqual(email?.subject, "Your builds, everywhere")
        XCTAssertEqual(email?.to, "Bob@Example.com")
        XCTAssertTrue(email?.body.hasPrefix("Hi Bob,") ?? false)
        XCTAssertTrue(email?.mailtoURL?.absoluteString.hasPrefix("mailto:Bob@Example.com?subject=Your%20builds,%20everywhere&body=Hi%20Bob") ?? false)
        XCTAssertNil(GenieOutreach.parse("Hi Bob, no subject line here but a long enough body to pass.", pick: pick(bobId)))
        XCTAssertNil(GenieOutreach.parse("Subject: Hi\n\nHi [Name], I am [Your name] and this is long enough.", pick: pick(bobId)))

        let toot = GenieOutreach.parse("\"Loved your talk on federated git! ForkMesh mirrors repos across hosts. #git\"", pick: pick(carolId))
        XCTAssertEqual(toot?.body, "@carol@social.test Loved your talk on federated git! ForkMesh mirrors repos across hosts. #git")
        XCTAssertNil(toot?.mailtoURL)
        let long = GenieOutreach.parse("@carol " + String(repeating: "word ", count: 200), pick: pick(carolId))
        XCTAssertLessThanOrEqual(long?.body.count ?? 999, GenieOutreach.tootLimit)
        XCTAssertTrue(long?.body.hasSuffix("…") ?? false)
    }

    func testFallbackDraftsNeedNoModel() async {
        let email = GenieOutreach.fallback(pick(bobId))
        XCTAssertFalse(email.byModel)
        XCTAssertEqual(email.subject, "ForkMesh for Builders")
        XCTAssertTrue(email.body.hasPrefix("Hi Bob,"))
        XCTAssertTrue(email.body.hasSuffix("Sam"))
        let toot = GenieOutreach.fallback(pick(carolId))
        XCTAssertTrue(toot.body.hasPrefix("@carol@social.test Hi Carol!"))
        XCTAssertLessThanOrEqual(toot.body.count, GenieOutreach.tootLimit)
        let none = await GenieOutreach.write(pick(bobId), model: nil)
        XCTAssertEqual(none, email)
    }

    func testTheModelWritesTheDraft() async {
        let engine = FakeLocalChatEngine()
        try? await engine.load(modelDirectory: URL(fileURLWithPath: "/m"), model: LocalModelCatalog.gemma4E2B)
        engine.enqueueReply(["Subject: Hello from ForkMesh\n\n", "Hi Bob, as CTO at Builders you may like ForkMesh: repositories mirrored across hosts.\n\nSam"])
        let model = GenieModel(name: "Fake", engine: engine)
        let draft = await GenieOutreach.write(pick(bobId), model: model)
        XCTAssertTrue(draft.byModel)
        XCTAssertEqual(draft.subject, "Hello from ForkMesh")
        XCTAssertEqual(engine.requests.last?.parameters.maxTokens, 420)
        engine.enqueueReply(["Sorry, I can't."])
        let fallback = await GenieOutreach.write(pick(bobId), model: model)
        XCTAssertFalse(fallback.byModel)
    }

    private let deck = "https://forkmesh.com/pitch-deck/abcdef123456"

    private func investorPick() -> GenieOutreachPick {
        let data = CrmOrganizationData(json: [
            "organization": "acme",
            "contacts": .array([
                .object(["id": .string(bobId), "name": "Bob Builder", "email": "bob@fund.test", "company": "Builders Fund", "flow": "investors", "stage": "lead"]),
            ]),
            "settings": .object([
                "fromName": "Sam", "zoho": .object(["region": "eu"]),
                "flows": .array([.object(["id": "investors", "name": "Investors", "icon": "investors", "stages": .array(["lead", "pitched", "committed"])])]),
            ]),
        ])
        return GenieOutreach.pick([data]) { 0 }!
    }

    private func writer(_ name: String, _ reply: String?) -> GenieOutreachWriter {
        GenieOutreachWriter(name: name, timeout: 5) { _ in
            guard let reply else { throw CancellationError() }
            return reply
        }
    }

    func testInvestorPipelinesGetADeckAndTheirZohoRegion() {
        let pick = investorPick()
        XCTAssertTrue(pick.investor)
        XCTAssertTrue(pick.needsDeck)
        XCTAssertEqual(pick.zohoRegion, "eu")
        XCTAssertFalse(self.pick(bobId).investor)
        let flows = [CrmFlow(id: "angels", name: "Angel round", stages: ["a", "b"]), CrmFlow(id: "users", name: "Users", stages: ["a", "b"])]
        var contact = pick.contact
        contact.flow = "angels"
        XCTAssertTrue(GenieOutreach.investor(contact, flows: flows))
        contact.flow = "users"
        XCTAssertFalse(GenieOutreach.investor(contact, flows: flows))
    }

    func testTheInvestorPromptCarriesTheDeckFactsAndTheirLink() {
        let messages = GenieOutreach.messages(investorPick(), deck: deck)
        XCTAssertTrue(messages[0].content.contains("never invent traction"))
        XCTAssertTrue(messages[0].content.contains("raising $1.0M"))
        XCTAssertTrue(messages[1].content.contains("Their personal deck link: \(deck)"))
        XCTAssertTrue(messages[1].content.contains("20-minute call"))
        XCTAssertFalse(GenieOutreach.messages(pick(bobId))[0].content.contains("raising"))
    }

    func testDraftsOpenInZohoMail() {
        let draft = GenieOutreach.fallback(investorPick(), deck: deck)
        XCTAssertTrue(draft.body.contains(deck))
        XCTAssertEqual(draft.subject, "ForkMesh deck for Builders Fund")
        let url = draft.zohoURL?.absoluteString ?? ""
        XCTAssertTrue(url.hasPrefix("https://mail.zoho.eu/zm/comp.do?ct=mailto%3Abob%40fund.test%3Fsubject%3D"))
        XCTAssertTrue(url.contains("forkmesh.com%2Fpitch-deck%2Fabcdef123456"))
        XCTAssertTrue(GenieOutreach.fallback(pick(bobId)).zohoURL?.absoluteString.hasPrefix("https://mail.zoho.com/zm/comp.do") ?? false)
        XCTAssertNil(GenieOutreach.fallback(pick(carolId)).zohoURL)
    }

    func testAMissingDeckLinkIsPutBackBeforeTheSignOff() {
        let reply = "Subject: ForkMesh\n\nHi Bob,\n\nForkMesh turns coding agents into attributable contributors.\n\nBest,\nSam"
        let draft = GenieOutreach.parse(reply, pick: investorPick(), deck: deck, writer: "Claude Opus 5.5")
        XCTAssertEqual(draft?.body, "Hi Bob,\n\nForkMesh turns coding agents into attributable contributors.\n\nOur deck, made for you: \(deck)\n\nBest,\nSam")
        XCTAssertEqual(draft?.writer, "Claude Opus 5.5")
        XCTAssertEqual(draft?.deck, deck)
        let kept = GenieOutreach.parse("Subject: Hi\n\nHi Bob, here is the deck:\n\(deck)\n\nSam", pick: investorPick(), deck: deck)
        XCTAssertEqual(kept?.body.components(separatedBy: deck).count, 2)
    }

    func testNumbersAndLinksMustComeFromTheFacts() {
        let sources = GenieOutreach.messages(investorPick(), deck: deck).map(\.content).joined(separator: "\n")
        XCTAssertEqual(GenieOutreach.unsupported("We merged 7,416 agent branches; see \(deck).", sources: sources), [])
        XCTAssertEqual(GenieOutreach.unsupported("We have 3,000 paying teams and 40% growth.", sources: sources), ["3,000", "40"])
        XCTAssertEqual(GenieOutreach.unsupported("Read https://forkmesh.com/blog/traction today.", sources: sources), ["https://forkmesh.com/blog/traction"])
    }

    func testTheFirstWriterWithAFactualDraftWins() async {
        let pick = investorPick()
        let invented = "Subject: ForkMesh\n\nHi Bob, we already have 900 paying customers and want you in.\n\nSam"
        let factual = "Subject: ForkMesh, seed round\n\nHi Bob, ForkMesh builds ForkMesh: 7,416 agent branches merged into main.\n\n\(deck)\n\nSam"
        let draft = await GenieOutreach.write(pick, writers: [writer("Down", nil), writer("Fibber", invented), writer("Claude", factual)], deck: deck)
        XCTAssertEqual(draft.writer, "Claude")
        XCTAssertTrue(draft.byModel)
        XCTAssertEqual(draft.zohoRegion, "eu")
        let none = await GenieOutreach.write(pick, writers: [writer("Fibber", invented)], deck: deck)
        XCTAssertFalse(none.byModel)
        XCTAssertTrue(none.body.contains(deck))
    }

    func testTheToolboxMintsTheirDeckLink() async {
        let transport = FakeDeckTransport()
        let client = PackagesClient(transport: transport)
        let box = ForkMeshToolbox(sources: AssistantSources(account: { AssistantAccount(username: "sam") }, crm: { client }))
        let url = try? await box.outreachDeck(investorPick())
        XCTAssertEqual(url, deck)
        XCTAssertEqual(transport.requests.last?.path, "/api/crm/acme/decks")
        XCTAssertEqual(transport.requests.last?.body?.str("contactId"), bobId)
        transport.refuse = true
        do {
            _ = try await box.outreachDeck(investorPick())
            XCTFail("a refused deck link must not pass")
        } catch {
            XCTAssertTrue("\(error)".contains("investor pipeline"))
        }
    }

    func testTheLogPacesThePopUpsAndRemembersWhoWasShown() {
        let store = InMemoryKeyValueStore()
        let start = Date(timeIntervalSince1970: 1_790_000_000)
        let log = GenieOutreachLog(store: store)
        XCTAssertFalse(log.due(now: start.addingTimeInterval(60), activeSince: start))
        XCTAssertTrue(log.due(now: start.addingTimeInterval(GenieOutreachLog.firstAfter), activeSince: start))
        log.showed(bobId, at: start)
        XCTAssertFalse(log.due(now: start.addingTimeInterval(3600), activeSince: start.addingTimeInterval(-3600)))
        XCTAssertTrue(log.due(now: start.addingTimeInterval(GenieOutreachLog.every), activeSince: start.addingTimeInterval(-3600)))
        XCTAssertEqual(log.skip(now: start.addingTimeInterval(3600)), [bobId])
        XCTAssertEqual(log.skip(now: start.addingTimeInterval(GenieOutreachLog.restAfterShown + 1)), [])
        log.drafted(carolId)

        let again = GenieOutreachLog(store: store)
        XCTAssertEqual(again.last, start)
        XCTAssertEqual(again.skip(now: start.addingTimeInterval(3600)), [bobId, carolId])
        XCTAssertEqual(again.skip(now: start.addingTimeInterval(GenieOutreachLog.restAfterShown * 10)), [carolId])
        again.setOff(true)
        XCTAssertFalse(GenieOutreachLog(store: store).due(now: start.addingTimeInterval(GenieOutreachLog.every * 3), activeSince: start))
    }
}
