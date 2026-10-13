import FMApi
import FMBase
import FMControl
import FMPackages
import XCTest
@testable import FMAssistant

@MainActor
private final class WeekCrmTransport: PackagesTransport {
    var contacts: [JSONObject] = []
    var emails: [JSONObject] = []
    var mail: [JSONObject] = []

    func send(_ method: String, _ path: String, query: [(String, String)], body: JSONObject?) async throws -> PackagesReply {
        if path.split(separator: "/").count == 2 {
            return PackagesReply(status: 200, json: ["organizations": .array([.object(["name": "acme", "role": "owner", "installed": true])])])
        }
        return PackagesReply(status: 200, json: [
            "organization": "acme",
            "contacts": .array(contacts.map(JSONValue.object)),
            "emails": .array(emails.map(JSONValue.object)),
            "mail": .array(mail.map(JSONValue.object)),
        ])
    }
}

@MainActor
final class GenieVitalsTests: XCTestCase {
    private let nowDate = Date(timeIntervalSince1970: 1_790_000_000)
    private let day: Int64 = 24 * 3600 * 1000

    private var nowMs: Int64 { Int64(nowDate.timeIntervalSince1970 * 1000) }

    private func hex(_ n: Int) -> String {
        let digits = String(n, radix: 16)
        return String(repeating: "0", count: 32 - digits.count) + digits
    }

    private func iso(_ ms: Int64) -> String {
        let format = ISO8601DateFormatter()
        format.timeZone = TimeZone(identifier: "UTC")
        return format.string(from: Date(timeIntervalSince1970: Double(ms) / 1000))
    }

    private func desktopRow(_ id: Int, createdAtMs: Int64, merged: Bool = false, outside: Bool = false) -> UnifiedAgentRow {
        var row = UnifiedAgentRow(
            id: "desktop:\(id)", badge: .desktopLan, title: "Task \(id)", repoOwner: "ada", repoName: "site", node: "desk",
            status: "finished", statusLabel: "Finished", isActive: false, updatedAtMs: Int(nowMs), localId: id,
            outside: outside, merged: merged)
        row.createdAtMs = Int(createdAtMs)
        return row
    }

    func testCommitDatesWrittenAsTextAreRead() {
        XCTAssertEqual(ForkMeshToolbox.commitDate(["date": "2026-08-21T18:09:41Z"]), 1_787_335_781_000)
        XCTAssertEqual(ForkMeshToolbox.commitDate(["date": "2026-08-21T18:09:41-04:00"]), 1_787_350_181_000)
        XCTAssertEqual(ForkMeshToolbox.commitDate(["date": 1_755_900_000]), 1_755_900_000_000)
        XCTAssertEqual(ForkMeshToolbox.commitDate(["date": "soon", "ts": 5]), 5000)
        XCTAssertEqual(ForkMeshToolbox.commitDate(["subject": "x"]), 0)
    }

    func testCrmCountsPeopleEmailedRepliesAndBouncesNotEditedRecords() {
        let since = nowMs - 7 * day
        var contacts: [JSONObject] = (1...93).map { n in
            ["id": .string(hex(n)), "name": .string("Contact \(n)"), "email": .string("c\(n)@example.com"), "updatedAt": .int(nowMs - day)]
        }
        contacts[0]["lastContactedAt"] = .int(nowMs - day)
        contacts[1]["lastContactedAt"] = .int(nowMs - 2 * day)
        contacts[2]["lastContactedAt"] = .int(nowMs - 30 * day)
        contacts[3]["fields"] = .object(["other_emails": "Dee <dee@home.example>, x"])
        let emails: [JSONObject] = [
            ["id": "e1", "contactId": .string(hex(1)), "to": "c1@example.com", "status": "delivered", "sentAt": .int(nowMs - day)],
            ["id": "e2", "contactId": .string(hex(1)), "to": "c1@example.com", "status": "open", "sentAt": .int(nowMs - day)],
            ["id": "e3", "contactId": .string(hex(2)), "to": "c2@example.com", "status": "bounce", "sentAt": .int(nowMs - day)],
            ["id": "e4", "contactId": .string(hex(3)), "to": "c3@example.com", "status": "delivered", "sentAt": .int(nowMs - 20 * day)],
            ["id": "e5", "contactId": .string(hex(5)), "to": "c5@example.com", "status": "queued", "createdAt": .int(nowMs - day)],
            ["id": "e6", "contactId": .string(hex(1)), "fromEmail": "c1@example.com", "direction": "inbound", "sentAt": .int(nowMs - day)],
        ]
        let mail: [JSONObject] = [
            ["id": .string(hex(500)), "from": "Dee <DEE@home.example>", "receivedAt": .int(nowMs - day)],
            ["id": .string(hex(501)), "from": "stranger@else.example", "receivedAt": .int(nowMs - day)],
            ["id": .string(hex(502)), "from": "c7@example.com", "receivedAt": .int(nowMs - 9 * day)],
        ]
        let data = CrmOrganizationData(json: [
            "organization": "acme", "contacts": .array(contacts.map(JSONValue.object)),
            "emails": .array(emails.map(JSONValue.object)), "mail": .array(mail.map(JSONValue.object)),
        ])
        XCTAssertEqual(GenieCrmCounts(data, since: since), GenieCrmCounts(emailed: 1, reached: 2, replies: 2, bounced: 1))
    }

    func testAgentCountsIncludeDesktopRunsAndSkipOutsideOnes() {
        let since = nowMs - 7 * day
        let rows = [
            desktopRow(1, createdAtMs: nowMs - day, merged: true),
            desktopRow(2, createdAtMs: nowMs - 2 * day),
            desktopRow(3, createdAtMs: nowMs - 10 * day, merged: true),
            desktopRow(4, createdAtMs: nowMs - day, outside: true),
        ]
        XCTAssertEqual(GenieAgentCounts(rows: rows, relay: nil, since: since), GenieAgentCounts(runs: 2, merged: 1))
        let relay = [
            AgentLaunchSession(sessionId: "a", createdAtMs: Int(nowMs - day), merged: true),
            AgentLaunchSession(sessionId: "b", createdAtMs: Int(nowMs - 9 * day)),
        ]
        XCTAssertEqual(GenieAgentCounts(rows: nil, relay: relay, since: since), GenieAgentCounts(runs: 1, merged: 1))
    }

    func testAWeekOfCodeAndEditedContactsReadsTrue() async {
        let transport = WeekCrmTransport()
        transport.contacts = (1...93).map { n in
            ["id": .string(hex(n)), "name": .string("Contact \(n)"), "email": .string("c\(n)@example.com"), "updatedAt": .int(nowMs - day)]
        }
        let control = FakeAgentControl()
        control.rows = (1...5).map { desktopRow($0, createdAtMs: nowMs - Int64($0) * day / 2, merged: $0 <= 2) }
        let commits: [JSONObject] = (0..<12).map { n in ["hash": .string("h\(n)"), "date": .string(iso(nowMs - Int64(n) * day - 1000))] }
        let client = PackagesClient(transport: transport)
        let sources = AssistantSources(
            account: { AssistantAccount(username: "ada") },
            repositories: { [Repository(owner: "ada", name: "site", updatedMs: 5)] },
            commits: { _, _ in commits },
            crm: { client },
            agentControl: { control })
        let toolbox = ForkMeshToolbox(sources: sources, timeZone: TimeZone(identifier: "UTC")!, now: { self.nowDate })
        let signs = await toolbox.vitalSigns()
        XCTAssertEqual(signs.commits, 7)
        XCTAssertEqual(signs.agentRuns, 5)
        XCTAssertEqual(signs.merged, 2)
        XCTAssertEqual(signs.emailsSent, 0)
        XCTAssertEqual(signs.contactsTouched, 0)
        let vitals = GenieVitals(signs, now: nowDate)
        XCTAssertGreaterThan(vitals.code, 0.9)
        XCTAssertEqual(vitals.marketing, 0)
        XCTAssertTrue(vitals.summary.contains("Code is humming: 7 commits, 5 agent runs and 2 merges this week."), vitals.summary)
        XCTAssertTrue(vitals.summary.contains("Marketing is starving with no outreach this week."), vitals.summary)
    }

    func testMarketingWithoutRepliesStaysCapped() {
        let now = nowDate
        let silent = GenieVitals(GenieVitalSigns(commits: 30, emailsSent: 20, contactsTouched: 20, measuredAt: now), now: now)
        XCTAssertEqual(silent.marketing, GenieVitals.silentCap, accuracy: 0.0001)
        XCTAssertTrue(silent.summary.contains("Marketing is slowing down: 20 people emailed this week."), silent.summary)
        XCTAssertTrue(silent.summary.contains("Nobody has replied, so the marketing is not working yet."), silent.summary)
        let reached = GenieVitals(GenieVitalSigns(commits: 30, contactsTouched: 93, measuredAt: now), now: now)
        XCTAssertLessThanOrEqual(reached.marketing, GenieVitals.silentCap)
        XCTAssertTrue(reached.summary.contains("93 contacts reached this week"), reached.summary)
        XCTAssertTrue(reached.summary.contains("No email went out, so there was nothing for anyone to reply to."), reached.summary)
        let few = GenieVitals(GenieVitalSigns(emailsSent: 20, replies: 2, measuredAt: now), now: now)
        XCTAssertEqual(few.marketing, GenieVitals.fewRepliesCap, accuracy: 0.0001)
        let answered = GenieVitals(GenieVitalSigns(emailsSent: 6, replies: 3, measuredAt: now), now: now)
        XCTAssertGreaterThan(answered.marketing, 0.9)
        XCTAssertTrue(answered.summary.contains("6 people emailed and 3 replies this week."), answered.summary)
    }

    func testOldStoredReadingsAreDropped() {
        let store = InMemoryKeyValueStore()
        store.set(["0", "0", "0", "0", "93", "0", "1790000000"], forKey: GenieVitalSigns.storeKey)
        XCTAssertNil(GenieVitalSigns.load(store))
    }
}
