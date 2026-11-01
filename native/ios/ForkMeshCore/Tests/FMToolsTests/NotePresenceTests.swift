import FMApi
import FMBase
import FMNet
import XCTest
@testable import FMTools

@MainActor
func eventually(_ condition: @MainActor () -> Bool) async -> Bool {
    for _ in 0..<400 {
        if condition() { return true }
        try? await Task.sleep(nanoseconds: 5_000_000)
    }
    return condition()
}

let parkedSleep: ToolsSleep = { millis in
    if millis >= 1000 { try await Task.sleep(nanoseconds: 60_000_000_000) }
}

func peerFrame(_ type: String, _ extra: JSONObject = [:], client: String = "peer-1", name: String = "ana") -> String {
    var frame: JSONObject = ["type": .string(type), "clientId": .string(client), "name": .string(name)]
    for (key, value) in extra { frame[key] = value }
    return JSON.encode(.object(frame))
}

final class NotePresenceTests: XCTestCase {
    func testFramesBecomeFacesLabelsAndEvents() {
        var presence = NotePresence(clientId: "me")
        let now: Int64 = 1_000_000
        XCTAssertEqual(presence.label(open: false, now: now), "Offline — edits still save")
        XCTAssertEqual(presence.label(open: true, now: now), "Live")

        let first = presence.receive(peerFrame("presence"), now: now)
        XCTAssertTrue(first.newcomer)
        XCTAssertNil(first.event)
        XCTAssertFalse(presence.receive(peerFrame("presence"), now: now).newcomer)
        XCTAssertEqual(presence.label(open: true, now: now), "Live · ana is viewing")
        XCTAssertFalse(presence.receive(peerFrame("presence", client: "me", name: "me"), now: now).newcomer)

        _ = presence.receive(peerFrame("cursor", client: "peer-2", name: "bo"), now: now)
        XCTAssertEqual(presence.label(open: true, now: now), "Live · 2 others here")
        let draft = presence.receive(peerFrame("draft", ["title": "T", "markdown": "M", "baseVersion": 3], client: "peer-2", name: "bo"), now: now)
        XCTAssertEqual(draft.event, .draft(title: "T", markdown: "M", baseVersion: 3))
        XCTAssertEqual(presence.label(open: true, now: now + 1000), "bo is editing…")
        XCTAssertEqual(presence.label(open: true, now: now + 6000), "Live · 2 others here")
        XCTAssertEqual(presence.peers(now: now).map(\.initials), ["A", "B"])
        XCTAssertTrue(presence.peers(now: now + 40_000).isEmpty)

        XCTAssertEqual(presence.receive(#"{"type":"note-updated","version":7}"#, now: now).event, .updated(version: 7))
        XCTAssertEqual(presence.receive(#"{"type":"note-deleted"}"#, now: now).event, .deleted)
        XCTAssertEqual(presence.receive(#"{"type":"access-changed"}"#, now: now).event, .accessChanged)
        XCTAssertNil(presence.receive("not json", now: now).event)
        XCTAssertNil(presence.receive(peerFrame("draft", client: "me"), now: now).event)

        let sent = (try? JSON.parse(presence.draftFrame(title: "a", markdown: "b", baseVersion: 2)))?.objectValue
        XCTAssertEqual(sent, ["type": "draft", "clientId": "me", "baseVersion": 2, "title": "a", "markdown": "b"])
    }

    @MainActor
    func testTheRoomAnnouncesItselfAnswersNewcomersAndSendsDrafts() async {
        let connector = FakeWebSocketConnector()
        var events: [NoteRoomEvent] = []
        let room = NoteRoomModel(
            noteId: noteId, clientId: "me",
            open: { id in try await connector.connect(url: URL(string: "wss://forkmesh.test/api/notes/\(id)/ws")!) },
            clock: FixedClock(epochMillis: toolsNowMillis), sleep: parkedSleep, onEvent: { events.append($0) })
        room.start()
        let ready = await connector.waitForConnections(1)
        XCTAssertTrue(ready)
        let socket = connector.lastConnection!
        let announced = await socket.waitForSent(1)
        XCTAssertTrue(announced)
        XCTAssertEqual(socket.sentJSON.first, ["type": "presence", "clientId": "me", "state": "viewing"])
        XCTAssertEqual(socket.url.absoluteString, "wss://forkmesh.test/api/notes/\(noteId)/ws")
        XCTAssertTrue(room.connected)
        XCTAssertEqual(room.label, "Live")

        socket.receive(text: peerFrame("presence"))
        let joined = await eventually { room.faces.map(\.name) == ["ana"] }
        XCTAssertTrue(joined)
        XCTAssertEqual(room.label, "Live · ana is viewing")
        let answered = await socket.waitForSent(2)
        XCTAssertTrue(answered)

        room.draftChanged(title: "One", markdown: "a", baseVersion: 3)
        room.draftChanged(title: "One", markdown: "ab", baseVersion: 3)
        let drafted = await socket.waitForSent(4)
        XCTAssertTrue(drafted)
        try? await Task.sleep(nanoseconds: 20_000_000)
        XCTAssertEqual(socket.sentJSON.filter { $0["type"] == "draft" }.map { $0["markdown"] }, ["ab"])
        XCTAssertEqual(socket.sentJSON.filter { $0["type"] == "presence" }.map { $0["state"] }, ["viewing", "viewing", "editing"])

        socket.receive(text: #"{"type":"note-updated","version":4}"#)
        let updated = await eventually { events == [.updated(version: 4)] }
        XCTAssertTrue(updated)

        room.stop()
        let closed = await socket.waitUntilClosed()
        XCTAssertTrue(closed)
        XCTAssertFalse(room.connected)
        XCTAssertTrue(room.faces.isEmpty)
    }

    func testPeersCarryTheirStateAndLeaveAtOnce() {
        var presence = NotePresence(clientId: "me")
        let now: Int64 = 2_000_000
        _ = presence.receive(peerFrame("presence", ["state": "editing"]), now: now)
        XCTAssertEqual(presence.peers(now: now).map(\.state), [.editing])
        XCTAssertEqual(presence.label(open: true, now: now), "ana is editing…")
        XCTAssertEqual(presence.peers(now: now).first?.stateLabel, "ana · editing")

        _ = presence.receive(peerFrame("cursor"), now: now)
        XCTAssertEqual(presence.peers(now: now).map(\.state), [.editing])
        _ = presence.receive(peerFrame("presence", ["state": "viewing"]), now: now)
        XCTAssertEqual(presence.label(open: true, now: now), "Live · ana is viewing")
        _ = presence.receive(peerFrame("presence", ["state": "bogus"], client: "peer-2", name: "bo"), now: now)
        XCTAssertEqual(presence.peers(now: now).map(\.state), [.viewing, .viewing])
        XCTAssertEqual(presence.label(open: true, now: now), "Live · 2 others here")

        _ = presence.receive(#"{"type":"leave","clientId":"peer-1","name":"ana"}"#, now: now)
        XCTAssertEqual(presence.peers(now: now).map(\.name), ["bo"])
        _ = presence.receive(#"{"type":"leave","clientId":"me","name":"me"}"#, now: now)
        _ = presence.receive(#"{"type":"leave","clientId":"nobody"}"#, now: now)
        XCTAssertEqual(presence.peers(now: now).map(\.name), ["bo"])
        _ = presence.receive(peerFrame("presence"), now: now)
        XCTAssertEqual(presence.peers(now: now).map(\.name), ["bo", "ana"])

        XCTAssertEqual(presence.receive(#"{"type":"note-comments","actor":"ana"}"#, now: now).event, .commentsChanged)
    }

    func testOwnStateIsEditingForTenSecondsAfterTyping() {
        var presence = NotePresence(clientId: "me")
        XCTAssertEqual(presence.localState(now: 5_000), .viewing)
        presence.typed(now: 10_000)
        XCTAssertEqual(presence.localState(now: 10_000), .editing)
        XCTAssertEqual(presence.localState(now: 19_999), .editing)
        XCTAssertEqual(presence.localState(now: 20_000), .viewing)
        let frame = (try? JSON.parse(presence.presenceFrame(state: .editing)))?.objectValue
        XCTAssertEqual(frame, ["type": "presence", "clientId": "me", "state": "editing"])
    }

    @MainActor
    func testTheRoomSendsStateChangesShowsFourFacesAndForgetsLeavers() async {
        let connector = FakeWebSocketConnector()
        let clock = FixedClock(epochMillis: toolsNowMillis)
        let room = NoteRoomModel(
            noteId: noteId, clientId: "me",
            open: { _ in try await connector.connect(url: URL(string: "wss://forkmesh.test/ws")!) },
            clock: clock, sleep: parkedSleep, onEvent: { _ in })
        room.start()
        _ = await connector.waitForConnections(1)
        let socket = connector.lastConnection!
        _ = await socket.waitForSent(1)

        room.typed()
        let editing = await socket.waitForSent(2)
        XCTAssertTrue(editing)
        XCTAssertEqual(socket.sentJSON.last, ["type": "presence", "clientId": "me", "state": "editing"])
        room.typed()
        clock.advance(by: 5)
        room.checkState()
        try? await Task.sleep(nanoseconds: 20_000_000)
        XCTAssertEqual(socket.sent.count, 2)
        clock.advance(by: 10)
        room.checkState()
        let viewing = await socket.waitForSent(3)
        XCTAssertTrue(viewing)
        XCTAssertEqual(socket.sentJSON.last, ["type": "presence", "clientId": "me", "state": "viewing"])

        for (index, name) in ["ana", "bo", "cy", "di", "ed", "flo"].enumerated() {
            socket.receive(text: peerFrame("presence", ["state": index == 1 ? "editing" : "viewing"], client: "p\(index)", name: name))
        }
        let six = await eventually { room.everyone.count == 6 }
        XCTAssertTrue(six)
        XCTAssertEqual(room.faces.map(\.name), ["ana", "bo", "cy", "di"])
        XCTAssertEqual(room.overflow, 2)
        XCTAssertEqual(room.label, "bo is editing…")

        socket.receive(text: #"{"type":"leave","clientId":"p1","name":"bo"}"#)
        let left = await eventually { room.everyone.count == 5 }
        XCTAssertTrue(left)
        XCTAssertEqual(room.faces.map(\.name), ["ana", "cy", "di", "ed"])
        XCTAssertEqual(room.overflow, 1)
        XCTAssertEqual(room.label, "Live · 5 others here")
        room.stop()
    }

    @MainActor
    func testAccessChangedReconnectsAtOnce() async {
        let connector = FakeWebSocketConnector()
        let room = NoteRoomModel(
            noteId: noteId, clientId: "me",
            open: { _ in try await connector.connect(url: URL(string: "wss://forkmesh.test/ws")!) },
            clock: FixedClock(epochMillis: toolsNowMillis), sleep: parkedSleep, onEvent: { _ in })
        room.start()
        _ = await connector.waitForConnections(1)
        connector.lastConnection!.receive(text: #"{"type":"access-changed"}"#)
        let again = await connector.waitForConnections(2)
        XCTAssertTrue(again)
        room.stop()
    }

    @MainActor
    func testTheEditorFollowsTheRoom() async {
        let connector = FakeWebSocketConnector()
        var serverVersion = 3
        var gone = false
        let api = FakeToolsApi { _, _, _ in
            if gone { throw OrgApiException("note_not_found", "note not found") }
            return ["ok": true, "note": .object(merged(noteJSON(version: serverVersion), ["markdown": .string("v\(serverVersion)")]))]
        }
        let editor = NoteEditorModel(
            noteId: noteId, call: api.call, clock: FixedClock(epochMillis: toolsNowMillis), sleep: parkedSleep,
            openSocket: { _ in try await connector.connect(url: URL(string: "wss://forkmesh.test/ws")!) })
        await editor.load()
        XCTAssertNotNil(editor.room)
        _ = await connector.waitForConnections(1)
        let socket = connector.lastConnection!

        socket.receive(text: peerFrame("draft", ["title": "Live title", "markdown": "live text", "baseVersion": 3]))
        let followed = await eventually { editor.markdown == "live text" }
        XCTAssertTrue(followed)
        XCTAssertFalse(editor.dirty)
        XCTAssertEqual(editor.room?.label, "ana is editing…")

        editor.markdown = "my own words"
        socket.receive(text: peerFrame("draft", ["title": "Live title", "markdown": "theirs", "baseVersion": 3]))
        try? await Task.sleep(nanoseconds: 30_000_000)
        XCTAssertEqual(editor.markdown, "my own words")
        editor.discard()

        serverVersion = 4
        socket.receive(text: #"{"type":"note-updated","version":4}"#)
        let reloaded = await eventually { editor.version == 4 }
        XCTAssertTrue(reloaded)
        XCTAssertEqual(editor.markdown, "v4")

        gone = true
        socket.receive(text: #"{"type":"note-deleted"}"#)
        let deleted = await eventually { editor.error == NoteEditorModel.deletedMessage }
        XCTAssertTrue(deleted)
        XCTAssertNil(editor.room)
    }

    @MainActor
    func testTheListSearchesBodiesOnTheServer() async {
        let other = String(repeating: "c", count: 32)
        var list: NotesListModel!
        var queries: [String] = []
        let call: ToolsApiCall = { _, suffix, _ in
            if suffix.isEmpty {
                return ["ok": true, "notes": [
                    .object(merged(noteJSON(), ["markdown": .null])),
                    .object(merged(noteJSON(role: "viewer"), ["id": .string(other), "title": "Team handbook", "shares": [], "markdown": .null])),
                ]]
            }
            queries.append(suffix)
            if suffix == "?query=ship" {
                list.query = "launch"
                list.queryChanged()
                await list.searchTask?.value
                return ["ok": true, "notes": [.object(noteJSON())]]
            }
            if suffix == "?query=launch" {
                return ["ok": true, "notes": [.object(merged(noteJSON(role: "viewer"), ["id": .string(other), "title": "Team handbook"]))]]
            }
            return ["ok": true, "notes": [.object(noteJSON())]]
        }
        list = NotesListModel(call: call, clock: FixedClock(epochMillis: toolsNowMillis), sleep: { _ in })
        await list.load()

        list.query = "La"
        list.queryChanged()
        list.query = "Lau"
        list.queryChanged()
        XCTAssertEqual(list.rows.map(\.title), ["Launch plan"])
        XCTAssertTrue(list.searching)
        await list.searchTask?.value
        XCTAssertEqual(queries, ["?query=lau"])
        XCTAssertFalse(list.searching)

        list.query = "ship"
        list.queryChanged()
        XCTAssertTrue(list.rows.isEmpty)
        XCTAssertEqual(list.emptyFilterMessage, NotesListModel.searchingMessage)
        await list.searchTask?.value
        XCTAssertEqual(queries, ["?query=lau", "?query=ship", "?query=launch"])
        XCTAssertEqual(list.rows.map(\.title), ["Team handbook"])
        XCTAssertEqual(NotesFilter.allCases.map { list.count($0) }, [1, 0, 1, 0])

        list.query = "a b&c"
        list.queryChanged()
        await list.searchTask?.value
        XCTAssertEqual(queries.last, "?query=a%20b%26c")

        list.query = String(repeating: "x", count: 201)
        list.queryChanged()
        await list.searchTask?.value
        XCTAssertEqual(queries.count, 4)
        XCTAssertFalse(list.searching)
        list.query = ""
        list.queryChanged()
        XCTAssertEqual(list.rows.count, 2)
    }
}
