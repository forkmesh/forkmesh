import FMApi
import FMBase
import Foundation
import XCTest

@testable import FMPlanning

@MainActor
final class HeldCalendarClient: PlanningClient {
    var held: [CheckedContinuation<JSONObject, Never>] = []
    var writes: [String: JSONObject] = [:]

    func accountApiCall(_ method: String, _ path: String, _ body: JSONObject?) async throws -> JSONObject {
        if method == "GET" {
            return await withCheckedContinuation { held.append($0) }
        }
        let key = "\(method) \(path)"
        guard let answer = writes[key] else { throw OrgApiException("not_found", "not found") }
        return answer
    }

    func waitForHeld(_ count: Int) async {
        while held.count < count { await Task.yield() }
    }

    func release(_ index: Int, _ answer: JSONObject) {
        held[index].resume(returning: answer)
    }

    func tree(_ owner: String, _ name: String, path: String, ref: String) async throws -> RepoTree {
        throw OrgApiException("not_found", "not found")
    }

    func blob(_ owner: String, _ name: String, _ path: String, ref: String) async throws -> RepoBlob {
        throw OrgApiException("not_found", "not found")
    }

    func publishedIssues(_ owner: String, _ name: String) async throws -> [Issue] { [] }

    func clearRepoCache(_ owner: String, _ name: String) {}
}

final class CalendarSaveTests: XCTestCase {
    private let utc = TimeZone(identifier: "UTC")!
    private let now: Int64 = 1_790_000_000_000

    private func event(_ id: Character, _ title: String, at start: Int64) -> JSONObject {
        [
            "id": .string(String(repeating: id, count: 32)), "owner": "alice", "calendar": "personal",
            "scope": "personal", "startAt": .int(start), "endAt": .int(start + 3_600_000), "title": .string(title),
        ]
    }

    private func list(_ events: [JSONObject]) -> JSONObject {
        ["ok": true, "actor": "alice", "events": .array(events.map { .object($0) })]
    }

    @MainActor func testCreatedEventSurvivesAListThatWasAlreadyInFlight() async {
        let client = HeldCalendarClient()
        let model = CalendarModel(client: client, mode: .day, timeZone: utc, firstWeekday: 2, clock: FixedClock(epochMillis: now))
        let draft = model.newDraft()
        let created = event("c", "Dentist", at: draft.startAt)
        client.writes["POST /api/calendar"] = ["ok": true, "event": .object(created)]

        let stale = Task { await model.load() }
        await client.waitForHeld(1)
        var named = draft
        named.title = "Dentist"
        let saved = await model.create(named)
        XCTAssertEqual(saved?.title, "Dentist")
        client.release(0, list([]))
        await stale.value
        XCTAssertEqual(model.occurrences(onDay: model.today).map(\.event.title), ["Dentist"])
        XCTAssertNil(model.error)

        let fresh = Task { await model.load() }
        await client.waitForHeld(2)
        client.release(1, list([created]))
        await fresh.value
        XCTAssertEqual(model.events.map(\.title), ["Dentist"])
    }

    @MainActor func testDeletedEventIsNotRestoredByAStaleList() async {
        let client = HeldCalendarClient()
        let model = CalendarModel(client: client, mode: .day, timeZone: utc, firstWeekday: 2, clock: FixedClock(epochMillis: now))
        let lunch = event("d", "Lunch", at: now + 3_600_000)
        let first = Task { await model.load() }
        await client.waitForHeld(1)
        client.release(0, list([lunch]))
        await first.value
        XCTAssertEqual(model.events.map(\.title), ["Lunch"])

        client.writes["DELETE /api/calendar/\(String(repeating: "d", count: 32))"] = ["ok": true, "deleted": true]
        let stale = Task { await model.load() }
        await client.waitForHeld(2)
        let deleted = await model.delete(String(repeating: "d", count: 32))
        XCTAssertTrue(deleted)
        client.release(1, list([lunch]))
        await stale.value
        XCTAssertTrue(model.events.isEmpty)
    }

    @MainActor func testOlderLoadFinishingLastIsIgnored() async {
        let client = HeldCalendarClient()
        let model = CalendarModel(client: client, mode: .day, timeZone: utc, firstWeekday: 2, clock: FixedClock(epochMillis: now))
        let older = Task { await model.load() }
        await client.waitForHeld(1)
        let newer = Task { await model.load() }
        await client.waitForHeld(2)
        client.release(1, list([event("e", "Current", at: now + 3_600_000)]))
        await newer.value
        XCTAssertFalse(model.loading)
        client.release(0, list([event("f", "Outdated", at: now + 3_600_000)]))
        await older.value
        XCTAssertEqual(model.events.map(\.title), ["Current"])
        XCTAssertFalse(model.loading)
    }

    @MainActor func testFailedSaveKeepsTheErrorUntilCleared() async {
        let client = HeldCalendarClient()
        let model = CalendarModel(client: client, mode: .day, timeZone: utc, firstWeekday: 2, clock: FixedClock(epochMillis: now))
        var draft = model.newDraft()
        draft.title = "Standup"
        let saved = await model.create(draft)
        XCTAssertNil(saved)
        XCTAssertEqual(model.error, "That was not found. It may have been deleted.")
        XCTAssertFalse(model.busy)
        model.clearError()
        XCTAssertNil(model.error)
    }
}
