import FMApi
import FMBase
import XCTest
@testable import FMTools

func fixedCalendar(_ zone: String) -> Calendar {
    var calendar = Calendar(identifier: .gregorian)
    calendar.timeZone = TimeZone(identifier: zone)!
    return calendar
}

final class NotesListDetailsTests: XCTestCase {
    let utc = fixedCalendar("UTC")

    func testTheLastEditReadsTodayYesterdayThisYearOrOlder() {
        let now = toolsNowMillis
        XCTAssertEqual(NoteEditTime.label(1_789_398_840_000, nowMillis: now, calendar: utc), "Today 3:14 PM")
        XCTAssertEqual(NoteEditTime.label(1_789_290_120_000, nowMillis: now, calendar: utc), "Yesterday 9:02 AM")
        XCTAssertEqual(NoteEditTime.label(1_788_016_440_000, nowMillis: now, calendar: utc), "Aug 29, 3:14 PM")
        XCTAssertEqual(NoteEditTime.label(1_761_750_840_000, nowMillis: now, calendar: utc), "Oct 29, 2025")
        XCTAssertEqual(NoteEditTime.label(0, nowMillis: now, calendar: utc), "")
        XCTAssertEqual(NoteEditTime.label(1_789_342_200_000, nowMillis: now, calendar: utc), "Yesterday 11:30 PM")
        XCTAssertEqual(NoteEditTime.label(1_789_342_200_000, nowMillis: now, calendar: fixedCalendar("Asia/Tokyo")), "Today 8:30 AM")
        XCTAssertEqual(NoteEditTime.label(1_767_225_900_000, nowMillis: now, calendar: utc), "Jan 1, 12:05 AM")
        XCTAssertEqual(NoteEditTime.label(1_767_225_900_000, nowMillis: now, calendar: fixedCalendar("America/New_York")), "Dec 31, 2025")
    }

    func testTheLineNamesSomeoneElseOnly() {
        let now = toolsNowMillis
        XCTAssertEqual(NoteEditTime.line(1_789_398_840_000, editedBy: "bob", me: "alice", nowMillis: now, calendar: utc), "Today 3:14 PM · by bob")
        XCTAssertEqual(NoteEditTime.line(1_789_398_840_000, editedBy: "Alice", me: "alice", nowMillis: now, calendar: utc), "Today 3:14 PM")
        XCTAssertEqual(NoteEditTime.line(1_789_398_840_000, editedBy: "", me: "alice", nowMillis: now, calendar: utc), "Today 3:14 PM")
        XCTAssertEqual(NoteEditTime.line(0, editedBy: "bob", me: "", nowMillis: now, calendar: utc), "by bob")
    }

    func testBadgesAndAvatars() {
        XCTAssertEqual(NoteReachBadge.of(["visibility": "public", "shares": []]), .publicNote)
        XCTAssertEqual(NoteReachBadge.of(["visibility": "private", "role": "owner", "shares": []]), .privateNote)
        XCTAssertEqual(NoteReachBadge.of(["visibility": "private", "role": "owner", "shares": [["type": "user", "name": "bob"]]]), .shared)
        XCTAssertEqual(NoteReachBadge.of(["visibility": "private", "role": "viewer", "shares": []]), .privateNote)
        XCTAssertEqual([NoteReachBadge.privateNote, .shared, .publicNote].map(\.label), ["Private", "Shared", "Public"])

        let shares: JSONObject = ["shares": [
            ["type": "user", "name": "bob smith"], ["type": "organization", "name": "acme"], ["type": "user", "name": "Bob Smith"],
            ["type": "user", "name": "cy"], ["type": "user", "name": "di"], ["type": "user", "name": ""],
        ]]
        let avatars = NoteAvatar.of(shares)
        XCTAssertEqual(avatars.shown.map(\.initials), ["BS", "A", "C"])
        XCTAssertEqual(avatars.shown.map(\.organization), [false, true, false])
        XCTAssertEqual(avatars.more, 1)
        XCTAssertEqual(NoteAvatar.of([:]).shown, [])
        XCTAssertEqual(NoteAvatar.of([:]).more, 0)
    }

    func testTheAccessHeaderNamesRoleSharingReachAndTheFullEdit() {
        XCTAssertEqual(NoteAccessHeader.roleLine("owner"), "You own this note")
        XCTAssertEqual(NoteAccessHeader.roleLine("editor"), "You can edit")
        XCTAssertEqual(NoteAccessHeader.roleLine("viewer"), "View only")
        XCTAssertEqual(NoteAccessHeader.shareLines([]), ["Not shared"])
        XCTAssertEqual(
            NoteAccessHeader.shareLines([["type": "user", "name": "bo", "role": "editor"], ["type": "organization", "name": "crew", "role": "viewer"], ["type": "user", "name": " "]]),
            ["bo · Can edit", "crew (organization) · Can view"])
        let note: JSONObject = [
            "role": "editor", "visibility": "private", "updatedAt": 1_789_398_840_000, "lastEditedBy": "bob",
            "shares": [["type": "user", "name": "bob", "role": "editor"]],
        ]
        XCTAssertEqual(NoteAccessHeader.lines(note, me: "alice", calendar: utc), [
            "You can edit", "bob · Can edit", "Shared", "Last edited Monday, September 14, 2026 at 3:14 PM by bob",
        ])
        XCTAssertEqual(NoteAccessHeader.lines(merged(note, ["lastEditedBy": "Alice"]), me: "alice", calendar: utc).last,
                       "Last edited Monday, September 14, 2026 at 3:14 PM by you")
        XCTAssertEqual(NoteAccessHeader.lines(merged(note, ["lastEditedBy": ""]), me: "alice", calendar: utc).last,
                       "Last edited Monday, September 14, 2026 at 3:14 PM")
        XCTAssertEqual(NoteAccessHeader.lines(["role": "owner", "visibility": "public", "shares": []], me: "", calendar: utc),
                       ["You own this note", "Not shared", "Public"])
    }

    @MainActor
    func testTheRowMenuCopiesSharesAndDeletesForTheOwnerOnly() async {
        let other = String(repeating: "c", count: 32)
        let api = FakeToolsApi { method, suffix, _ in
            switch (method, suffix) {
            case ("GET", ""):
                return ["ok": true, "notes": [.object(noteJSON()), .object(merged(noteJSON(role: "viewer"), ["id": .string(other)]))]]
            case ("DELETE", "/\(noteId)"):
                return ["ok": true]
            default:
                throw OrgApiException("unexpected", "\(method) \(suffix)")
            }
        }
        let list = NotesListModel(
            call: api.call, clock: FixedClock(epochMillis: toolsNowMillis), calendar: utc, seen: NoteViewsSeen(),
            webLink: { path in URL(string: "https://app.forkmesh.test\(path)") })
        await list.load()
        XCTAssertEqual(list.rows.map(\.isOwner), [true, false])
        XCTAssertEqual(list.accessLink(noteId)?.absoluteString, "https://app.forkmesh.test/dashboard/notes?note=\(noteId)")
        XCTAssertEqual(list.accessLines(other).first, "View only")
        XCTAssertEqual(list.accessLines("missing"), [])
        XCTAssertNil(list.shareModel(other))
        XCTAssertEqual(list.shareModel(noteId)?.shares.count, 1)
        let refused = await list.delete(other)
        XCTAssertNil(refused.notice)
        let deleted = await list.delete(noteId)
        XCTAssertEqual(deleted.notice?.message, "Note deleted")
        XCTAssertEqual(list.rows.map(\.id), [other])
        XCTAssertEqual(api.calls.filter { $0.method == "DELETE" }.count, 1)
    }

    @MainActor
    func testRowsShowUnreadPeopleCommentsAndOpeningClearsUnread() async {
        let api = FakeToolsApi { method, suffix, _ in
            switch (method, suffix) {
            case ("GET", ""):
                return ["ok": true, "notes": [
                    .object(merged(noteJSON(role: "editor"), [
                        "unread": true, "lastEditedBy": "bob", "openComments": 2, "unreadComments": 1,
                        "updatedAt": 1_789_398_840_000,
                    ])),
                ]]
            case ("GET", "/\(noteId)"):
                return ["ok": true, "note": .object(merged(noteJSON(), ["unread": true]))]
            case ("POST", "/\(noteId)/activity"):
                return ["ok": true]
            default:
                throw OrgApiException("unexpected", "\(method) \(suffix)")
            }
        }
        let seen = NoteViewsSeen()
        let list = NotesListModel(call: api.call, clock: FixedClock(epochMillis: toolsNowMillis), me: "alice", calendar: utc, seen: seen)
        await list.load()
        let row = list.rows[0]
        XCTAssertTrue(row.unread)
        XCTAssertEqual(row.subtitle, "Today 3:14 PM · by bob")
        XCTAssertEqual(row.openComments, 2)
        XCTAssertEqual(row.reach, .shared)
        XCTAssertEqual(row.people.map(\.name), ["bob"])
        list.markViewed(noteId)
        XCTAssertFalse(list.rows[0].unread)
        XCTAssertEqual(list.rows[0].openComments, 2)
        await list.load()
        XCTAssertFalse(list.rows[0].unread)
        seen.record(noteId, updatedAt: 1)
        XCTAssertFalse(list.rows[0].unread)
        XCTAssertTrue(NoteViewsSeen().unread(["id": .string(noteId), "unread": true, "updatedAt": 5]))
        XCTAssertFalse(NoteViewsSeen().unread(["id": .string(noteId), "unread": false]))
        let later = NoteViewsSeen()
        later.record(noteId, updatedAt: 5)
        XCTAssertTrue(later.unread(["id": .string(noteId), "unread": true, "updatedAt": 6]))

        let viewed = NoteViewsSeen()
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: FixedClock(epochMillis: toolsNowMillis), sleep: { _ in }, seen: viewed)
        await editor.recordView()
        XCTAssertFalse(api.calls.contains { $0.suffix.hasSuffix("/activity") })
        await editor.load()
        await editor.recordView()
        await editor.recordView()
        XCTAssertEqual(api.calls.filter { $0.suffix.hasSuffix("/activity") }.count, 1)
        XCTAssertEqual(api.last?.body, ["kind": "view"])
        XCTAssertEqual(editor.note?["unread"], false)
        XCTAssertFalse(viewed.unread(merged(noteJSON(), ["unread": true])))
    }
}
