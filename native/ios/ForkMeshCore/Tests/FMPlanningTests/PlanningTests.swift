import FMApi
import FMBase
import Foundation
import XCTest

@testable import FMPlanning

@MainActor
final class FakePlanningClient: PlanningClient {
    var calls: [(method: String, path: String, body: JSONObject?)] = []
    var answers: [String: JSONObject] = [:]
    var failures: [String: Error] = [:]
    var trees: [String: RepoTree] = [:]
    var blobs: [String: String] = [:]
    var issueList: [Issue] = []
    var cleared = 0

    func accountApiCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        calls.append((method, path, body))
        let key = "\(method) \(path.split(separator: "?").first.map(String.init) ?? path)"
        if let failure = failures[key] { throw failure }
        return answers[key] ?? ["ok": true]
    }

    func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree {
        guard let tree = trees[path] else { throw OrgApiException("not_found", "not found") }
        return tree
    }

    func blob(_ owner: String, _ name: String, _ path: String, ref: String) async throws -> RepoBlob {
        guard let content = blobs[path] else { throw OrgApiException("not_found", "not found") }
        return RepoBlob(path: path, content: content)
    }

    func publishedIssues(_ owner: String, _ name: String) async throws -> [Issue] { issueList }

    func clearRepoCache(_ owner: String, _ name: String) { cleared += 1 }
}

final class PlanningTests: XCTestCase {
    private let utc = TimeZone(identifier: "UTC")!
    private let day: Int64 = 86_400_000

    private func ms(_ text: String) -> Int64 {
        let formatter = ISO8601DateFormatter()
        return formatter.date(from: text)!.epochMillis
    }

    func testRecurrenceExpandsWithinRangeAndUntil() {
        let weekly = CalendarEvent(
            id: String(repeating: "a", count: 32), startAt: ms("2026-01-05T09:00:00Z"), endAt: ms("2026-01-05T10:00:00Z"),
            timezone: "UTC", title: "Standup", recurrence: .weekly, recurrenceUntil: ms("2026-03-01T00:00:00Z"))
        let feb = CalendarMath.occurrences(of: weekly, from: ms("2026-02-01T00:00:00Z"), to: ms("2026-03-31T00:00:00Z"), timeZone: utc)
        XCTAssertEqual(feb.map { CalendarMath.dayKey($0.startAt, timeZone: utc) }, ["2026-02-02", "2026-02-09", "2026-02-16", "2026-02-23"])
        let monthly = CalendarEvent(
            id: "m", startAt: ms("2020-01-31T12:00:00Z"), endAt: ms("2020-01-31T13:00:00Z"), timezone: "UTC", title: "Rent",
            recurrence: .monthly)
        let later = CalendarMath.occurrences(of: monthly, from: ms("2030-02-01T00:00:00Z"), to: ms("2030-04-01T00:00:00Z"), timeZone: utc)
        XCTAssertEqual(later.map { CalendarMath.dayKey($0.startAt, timeZone: utc) }, ["2030-02-28", "2030-03-31"])
        let single = CalendarEvent(id: "s", startAt: ms("2026-01-05T23:00:00Z"), endAt: ms("2026-01-06T01:00:00Z"), title: "Late")
        XCTAssertEqual(CalendarMath.occurrences(of: single, from: ms("2026-01-06T00:00:00Z"), to: ms("2026-01-07T00:00:00Z"), timeZone: utc).count, 1)
        XCTAssertTrue(CalendarMath.occurrences(of: single, from: ms("2026-01-06T01:00:00Z"), to: ms("2026-01-07T00:00:00Z"), timeZone: utc).isEmpty)
        let daily = CalendarEvent(id: "d", startAt: 0, endAt: 60_000, title: "Tick", recurrence: .daily)
        XCTAssertEqual(CalendarMath.occurrences(of: daily, from: 0, to: 5_000 * day, timeZone: utc).count, CalendarMath.maxOccurrencesPerEvent)
    }

    func testGridsAndDayKeys() {
        let anchor = ms("2026-09-24T15:00:00Z")
        let grid = CalendarMath.monthGrid(anchor, firstWeekday: 2, timeZone: utc)
        XCTAssertEqual(grid.count, 42)
        XCTAssertEqual(CalendarMath.dayKey(grid[0], timeZone: utc), "2026-08-31")
        XCTAssertEqual(CalendarMath.weekDays(anchor, firstWeekday: 1, timeZone: utc).map { CalendarMath.dayKey($0, timeZone: utc) }.first, "2026-09-20")
        XCTAssertEqual(CalendarMath.parseDayKey("2026-02-30", timeZone: utc), nil)
        XCTAssertEqual(CalendarMath.parseDayKey("2026-09-24", timeZone: utc), ms("2026-09-24T00:00:00Z"))
        let range = CalendarMath.range(for: .week, anchor: anchor, firstWeekday: 2, timeZone: utc)
        XCTAssertEqual(range.end - range.start, 7 * day)
    }

    func testDraftValidationAndBody() {
        var draft = CalendarEventDraft(startAt: 1_000, endAt: 2_000, timezone: "Europe/Paris", calendar: "acme")
        XCTAssertEqual(draft.validationError, "Give the event a title.")
        draft.title = " Review "
        draft.meetingUrl = "http://meet"
        XCTAssertEqual(draft.validationError, "Meeting links must start with https://.")
        draft.meetingUrl = "https://meet.example/x"
        draft.reminders = [7]
        XCTAssertEqual(draft.validationError, "Choose reminders from the list.")
        draft.reminders = [10, 60, 10]
        draft.attendees = [" Bob ", ""]
        XCTAssertNil(draft.validationError)
        let body = draft.body
        XCTAssertEqual(body["title"], "Review")
        XCTAssertEqual(body["scope"], "organization")
        XCTAssertEqual(body["calendar"], "acme")
        XCTAssertEqual(body["reminders"], .array([60, 10]))
        XCTAssertEqual(body["attendees"], .array(["bob"]))
        XCTAssertEqual(CalendarEventDraft(startAt: 0, endAt: 1, timezone: "").body["scope"], "personal")
        XCTAssertEqual(CalendarEventDraft.reminderLabel(120), "2 hours before")
    }

    @MainActor func testCalendarModelLoadsMutatesAndRsvps() async {
        let client = FakePlanningClient()
        let id = String(repeating: "b", count: 32)
        let clock = FixedClock(epochMillis: ms("2026-09-24T10:30:00Z"))
        let event: JSONObject = [
            "id": .string(id), "owner": "carol", "calendar": "personal", "scope": "personal",
            "startAt": .int(ms("2026-09-24T12:00:00Z")), "endAt": .int(ms("2026-09-24T13:00:00Z")), "title": "Lunch",
            "attendees": .array([.object(["name": "alice", "status": "invited"])]),
        ]
        client.answers["GET /api/calendar"] = [
            "ok": true, "actor": "Alice", "events": .array([.object(event)]),
            "calendars": .array([.object(["id": "personal", "name": "personal", "scope": "personal"]),
                                 .object(["id": "acme", "name": "acme", "scope": "organization", "role": "admin"])]),
        ]
        let model = CalendarModel(client: client, mode: .day, timeZone: utc, firstWeekday: 2, clock: clock)
        await model.load()
        XCTAssertEqual(model.actor, "alice")
        XCTAssertEqual(model.calendars.count, 2)
        XCTAssertEqual(model.occurrences(onDay: model.today).map(\.event.title), ["Lunch"])
        XCTAssertFalse(model.canEdit(model.events[0]))
        XCTAssertTrue(client.calls[0].path.hasPrefix("/api/calendar?start="))

        let draft = model.newDraft()
        XCTAssertEqual(draft.startAt, ms("2026-09-24T11:00:00Z"))
        var invalid = draft
        invalid.endAt = invalid.startAt
        let refused = await model.create(invalid)
        XCTAssertNil(refused)
        XCTAssertEqual(client.calls.count, 1)

        var created = event
        created["id"] = .string(String(repeating: "c", count: 32))
        created["owner"] = "alice"
        created["title"] = "Mine"
        client.answers["POST /api/calendar"] = ["ok": true, "event": .object(created)]
        var named = draft
        named.title = "Mine"
        let made = await model.create(named)
        XCTAssertEqual(made?.title, "Mine")
        XCTAssertTrue(model.canEdit(made!))

        let answered = await model.rsvp(id, status: "accepted")
        XCTAssertTrue(answered)
        XCTAssertEqual(model.event(id)?.attendeeStatus("alice"), "accepted")
        let badStatus = await model.rsvp(id, status: "maybe")
        XCTAssertFalse(badStatus)
        let badId = await model.delete("../x")
        XCTAssertFalse(badId)
        let deleted = await model.delete(id)
        XCTAssertTrue(deleted)
        XCTAssertNil(model.event(id))

        client.failures["DELETE /api/calendar/\(String(repeating: "c", count: 32))"] = OrgApiException("forbidden", "forbidden")
        let refusedDelete = await model.delete(String(repeating: "c", count: 32))
        XCTAssertFalse(refusedDelete)
        XCTAssertEqual(model.error, "You do not have access to that.")
    }

    @MainActor func testSingleEventLoadAndAgendaGroups() async {
        let client = FakePlanningClient()
        let id = String(repeating: "d", count: 32)
        client.answers["GET /api/calendar/\(id)"] = [
            "ok": true, "actor": "Bob",
            "event": ["id": .string(id), "title": "Deep link", "startAt": .int(ms("2026-09-25T08:00:00Z")),
                      "endAt": .int(ms("2026-09-25T09:00:00Z"))],
        ]
        let model = CalendarModel(client: client, mode: .agenda, anchorDay: "2026-09-24", timeZone: utc, firstWeekday: 2,
                                  clock: FixedClock(epochMillis: 0))
        await model.loadEvent("nope")
        XCTAssertEqual(model.error, "That event link is not valid.")
        await model.loadEvent(id)
        XCTAssertNil(model.error)
        XCTAssertEqual(model.actor, "bob")
        XCTAssertEqual(model.agenda.map(\.day), ["2026-09-25"])
        XCTAssertEqual(model.agenda.first?.items.first?.event.title, "Deep link")
    }

    @MainActor func testCalendarStepsReloadOnlyWhenLeavingLoadedRange() async {
        let client = FakePlanningClient()
        let model = CalendarModel(client: client, mode: .day, anchorDay: "2026-09-24", timeZone: utc, firstWeekday: 2,
                                  clock: FixedClock(epochMillis: 0))
        await model.load()
        await model.step(1)
        XCTAssertEqual(model.anchorKey, "2026-09-25")
        XCTAssertEqual(client.calls.count, 1)
        await model.setMode(.month)
        await model.step(3)
        XCTAssertEqual(model.anchorKey, "2026-12-01")
        XCTAssertEqual(client.calls.count, 2)
    }

    @MainActor func testProjectsMilestonesAndProgress() async {
        let client = FakePlanningClient()
        client.trees[PlanningRecords.projectsPath] = RepoTree(path: PlanningRecords.projectsPath, entries: [
            RepoTreeEntry(json: ["name": "1", "path": ".forkmesh/projects/1", "type": "dir"]),
            RepoTreeEntry(json: ["name": "2", "path": ".forkmesh/projects/2", "type": "dir"]),
            RepoTreeEntry(json: ["name": "notes.md", "path": ".forkmesh/projects/notes.md", "type": "file"]),
        ])
        client.blobs[".forkmesh/projects/1/project-1.json"] = """
        {"schema":"forkmesh-project-v1","number":1,"title":"Launch","status":"open","startDate":1000,"endDate":5000,"milestone":"v1","issues":[],"events":[]}
        """
        client.blobs[".forkmesh/projects/2/project-2.json"] = """
        {"schema":"forkmesh-project-v1","number":2,"title":"Gone","events":[{"type":"delete","target":"self"}]}
        """
        client.blobs[PlanningRecords.milestonesPath] = """
        [{"title":"v1","due":9000,"status":"open"},{"title":"old","status":"closed"},{"title":" "}]
        """
        client.issueList = [
            Issue(json: ["number": 1, "title": "a", "status": "closed", "milestone": "v1"]),
            Issue(json: ["number": 2, "title": "b", "status": "open", "milestone": "v1"]),
            Issue(json: ["number": 3, "title": "c", "status": "open"]),
        ]
        let model = RepoPlanningModel(client: client, repo: RepoRef(owner: "a", name: "b"))
        await model.load(refresh: true)
        XCTAssertEqual(client.cleared, 1)
        XCTAssertEqual(model.projects.map(\.title), ["Launch"])
        XCTAssertEqual(model.shownMilestones.map(\.title), ["v1"])
        model.showClosed = true
        XCTAssertEqual(model.shownMilestones.map(\.title), ["v1", "old"])
        let launch = model.projects[0]
        XCTAssertEqual(model.progress(launch).label, "1 of 2 closed · 50%")
        XCTAssertEqual(model.progress(model.milestones[0]).percent, 50)
        XCTAssertEqual(model.linkedIssues(launch).map(\.number), [1, 2])
        let span = model.timeline!
        let bar = PlanningRecords.bar(launch, in: span)!
        XCTAssertEqual(bar.offset, 0)
        XCTAssertEqual(bar.width, 1)
        XCTAssertEqual(PlanningRecords.progress(project: RepoProject(number: 9, title: "x", issues: [3]), issues: client.issueList).label,
                       "0 of 1 closed · 0%")
        XCTAssertEqual(PlanningProgress(closed: 0, total: 0).label, "No issues")
    }

    @MainActor func testTimerPingsOnlyForRecentRungTimers() async {
        let client = FakePlanningClient()
        let now: Int64 = 10_000_000
        let recent = PlanningTimer(id: "t-1", kind: .countdown, label: "Tea", createdAtMs: 0, finishedAtMs: now - 60_000)
        let stale = PlanningTimer(id: "t-2", kind: .alarm, label: "Old", createdAtMs: 0, finishedAtMs: now - TimerPings.maxAgeMs - 1)
        let watch = PlanningTimer(id: "t-3", kind: .stopwatch, label: "W", createdAtMs: 0, finishedAtMs: now)
        let sent = await TimerPings.post([recent, stale, watch], client: client, nowMillis: now)
        XCTAssertEqual(sent, 1)
        XCTAssertEqual(client.calls.map(\.path), ["/api/notifications/timer"])
        XCTAssertEqual(client.calls[0].body, ["timerId": "t-1", "kind": "countdown", "endsAt": .int(now - 60_000), "label": "Tea"])
    }
}
