import FMBase
import FMLocalAI
import FMPackages
import XCTest
@testable import FMAssistant

final class FakeAutoSendTransport: PackagesTransport {
    var replies: [PackagesReply]
    var requests: [(method: String, path: String, body: JSONObject?)] = []

    init(_ replies: [PackagesReply]) {
        self.replies = replies
    }

    func send(_ method: String, _ path: String, query: [(String, String)], body: JSONObject?) async throws -> PackagesReply {
        requests.append((method, path, body))
        return replies.isEmpty ? PackagesReply(status: 500, json: ["error": "unexpected"]) : replies.removeFirst()
    }
}

@MainActor
final class GenieAutoSendTests: XCTestCase {
    private let samId = String(repeating: "a", count: 32)
    private let start = Date(timeIntervalSince1970: 1_790_000_000)

    private var utc: Calendar {
        var calendar = Calendar(identifier: .gregorian)
        calendar.timeZone = TimeZone(identifier: "UTC")!
        return calendar
    }

    private var pick: GenieOutreachPick {
        GenieOutreachPick(
            organization: "acme", contact: CrmContact(json: ["id": .string(samId), "name": "Sam Dev", "email": "sam@example.com"]),
            flowName: "Customers", channel: .email, sender: "alice")
    }

    private var sent: PackagesReply {
        PackagesReply(status: 201, json: [
            "ok": true, "subject": "Quick idea", "text": "Hi Sam,\n\nhttps://forkmesh.com/r/alice", "via": "zoho",
            "fromEmail": "founders@acme.example", "model": "host-model", "sentToday": 7, "dailyLimit": 100,
            "email": .object(["id": .string(String(repeating: "e", count: 32)), "to": "sam@example.com"]),
        ])
    }

    func testPaceSendsOneEveryFourteenMinutesUpToAHundredADay() {
        let store = InMemoryKeyValueStore()
        let pace = GenieAutoSendPace(store: store, calendar: utc)
        XCTAssertEqual(GenieAutoSendPace.every, 864)
        XCTAssertFalse(pace.due(now: start))
        pace.setOn(true)
        XCTAssertTrue(pace.due(now: start))
        pace.sent(at: start)
        XCTAssertEqual(pace.sentToday(now: start), 1)
        XCTAssertFalse(pace.due(now: start.addingTimeInterval(863)))
        XCTAssertEqual(pace.wait(now: start.addingTimeInterval(800)), 64, accuracy: 0.001)
        XCTAssertTrue(pace.due(now: start.addingTimeInterval(864)))

        let again = GenieAutoSendPace(store: store, calendar: utc)
        XCTAssertTrue(again.on)
        XCTAssertEqual(again.sentToday(now: start), 1)
        XCTAssertEqual(again.sentToday(now: start.addingTimeInterval(24 * 60 * 60)), 0)
    }

    func testAFullDayWaitsForMidnight() {
        let pace = GenieAutoSendPace(store: InMemoryKeyValueStore(), calendar: utc)
        pace.setOn(true)
        let midnight = utc.startOfDay(for: start)
        for index in 0..<GenieAutoSendPace.perDay { pace.sent(at: midnight.addingTimeInterval(Double(index))) }
        let later = midnight.addingTimeInterval(2 * 60 * 60)
        XCTAssertFalse(pace.due(now: later))
        XCTAssertEqual(pace.wait(now: later), 22 * 60 * 60, accuracy: 0.001)
        XCTAssertTrue(pace.due(now: midnight.addingTimeInterval(24 * 60 * 60)))
    }

    func testCountdownRunsFromTheLastSendToTheNext() {
        let pace = GenieAutoSendPace(store: InMemoryKeyValueStore(), calendar: utc)
        XCTAssertNil(pace.countdown(now: start))
        pace.setOn(true)
        XCTAssertNil(pace.countdown(now: start))
        pace.sent(at: start)
        let window = pace.countdown(now: start.addingTimeInterval(200))
        XCTAssertEqual(window?.start, start)
        XCTAssertEqual(window?.end, start.addingTimeInterval(864))
        XCTAssertNil(pace.countdown(now: start.addingTimeInterval(864)))
        pace.setOn(false)
        XCTAssertNil(pace.countdown(now: start.addingTimeInterval(200)))
    }

    func testOutcomesSkipTheContactOrPauseTheSender() {
        guard case let .sent(receipt) = GenieAutoSend.outcome(sent, pick: pick) else { return XCTFail("not sent") }
        XCTAssertEqual(receipt.subject, "Quick idea")
        XCTAssertEqual(receipt.to, "sam@example.com")
        XCTAssertEqual(receipt.contactName, "Sam Dev")
        XCTAssertEqual(receipt.viaLabel, "Zoho Mail")
        XCTAssertEqual(receipt.sentToday, 7)
        XCTAssertEqual(GenieAutoSend.outcome(PackagesReply(status: 409, json: ["error": "recently_emailed"]), pick: pick), .skip("recently_emailed"))
        XCTAssertEqual(
            GenieAutoSend.outcome(PackagesReply(status: 429, json: ["error": "daily_limit_reached", "dailyLimit": 100]), pick: pick),
            .pause("The CRM's limit of 100 emails a day is reached."))
        XCTAssertEqual(
            GenieAutoSend.outcome(PackagesReply(status: 409, json: ["error": "zoho_send_not_allowed", "detail": "Reconnect Zoho Mail."]), pick: pick),
            .pause("Reconnect Zoho Mail."))
    }

    func testAgentRepliesParseIntoSubjectAndBody() {
        let parsed = GenieAutoSend.parseReply("Here it is:\n**Subject:** \"Quick idea\"\n\nHi Sam,\n\nThanks\n")
        XCTAssertEqual(parsed?.subject, "Quick idea")
        XCTAssertEqual(parsed?.body, "Hi Sam,\n\nThanks")
        XCTAssertNil(GenieAutoSend.parseReply("Hi Sam, no subject here"))
    }

    func testWithoutAServerModelThePhoneWritesAndSendsAgain() async {
        let transport = FakeAutoSendTransport([
            PackagesReply(status: 503, json: ["error": "ai_unavailable", "prompt": "Write to Sam."]), sent,
        ])
        let client = PackagesClient(transport: transport)
        let box = ForkMeshToolbox(sources: AssistantSources(crm: { client }))
        var seen: [String] = []
        let writer = GenieOutreachWriter(name: "Phone model", timeout: 5) { (messages: [LocalChatMessage]) async throws -> String in
            seen = messages.map(\.content)
            return "Subject: Quick idea\n\nHi Sam,\n\nhttps://forkmesh.com/r/alice"
        }
        var stages: [GenieAutoSendStage] = []
        let outcome = await box.outreachAutoSend(pick, writers: [writer]) { stages.append($0) }
        guard case .sent = outcome else { return XCTFail("\(outcome)") }
        XCTAssertEqual(stages, [.writing, .sending, .sent])
        XCTAssertEqual(seen, ["Write to Sam."])
        XCTAssertEqual(transport.requests.map(\.path), ["/api/crm/acme/outreach/auto", "/api/crm/acme/outreach/auto"])
        let retry = transport.requests[1].body ?? [:]
        XCTAssertEqual(retry.str("subject"), "Quick idea")
        XCTAssertEqual(retry.str("model"), "Phone model")
        XCTAssertEqual(retry.str("contactId"), samId)
    }

    func testAServerDraftGoesStraightFromWritingToSent() async {
        let client = PackagesClient(transport: FakeAutoSendTransport([sent]))
        let box = ForkMeshToolbox(sources: AssistantSources(crm: { client }))
        var stages: [GenieAutoSendStage] = []
        _ = await box.outreachAutoSend(pick, writers: []) { stages.append($0) }
        XCTAssertEqual(stages, [.writing, .sent])

        let refused = PackagesClient(transport: FakeAutoSendTransport([PackagesReply(status: 409, json: ["error": "recently_emailed"])]))
        stages = []
        _ = await ForkMeshToolbox(sources: AssistantSources(crm: { refused })).outreachAutoSend(pick, writers: []) { stages.append($0) }
        XCTAssertEqual(stages, [.writing])
    }

    func testRatingsPostToTheCrm() async throws {
        let transport = FakeAutoSendTransport([PackagesReply(status: 200, json: ["ok": true])])
        try await PackagesClient(transport: transport).rateCrmAutoEmail(
            "acme", emailId: String(repeating: "e", count: 32), rating: 4, note: "Shorter")
        XCTAssertEqual(transport.requests.first?.path, "/api/crm/acme/outreach/rate")
        XCTAssertEqual(transport.requests.first?.body?.int("rating"), 4)
        XCTAssertEqual(transport.requests.first?.body?.str("note"), "Shorter")
    }

    func testBestCanAskForEmailOnly() {
        let data = CrmOrganizationData(json: [
            "organization": "acme",
            "contacts": .array([
                .object(["id": .string(samId), "name": "Carol", "mastodon": "@carol@social.test", "value": 900, "flow": "customers", "stage": "lead"]),
                .object(["id": .string(String(repeating: "b", count: 32)), "name": "Bob", "email": "bob@example.com", "flow": "customers", "stage": "lead"]),
            ]),
        ])
        XCTAssertEqual(GenieOutreach.best([data])?.contact.name, "Carol")
        XCTAssertEqual(GenieOutreach.best([data], only: .email)?.contact.name, "Bob")
    }
}
