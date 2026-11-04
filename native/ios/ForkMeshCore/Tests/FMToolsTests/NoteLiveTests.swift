import FMApi
import FMBase
@testable import FMTools
import XCTest

final class NoteLiveTests: XCTestCase {
    let clock = FixedClock(epochMillis: toolsNowMillis)
    let instant: ToolsSleep = { _ in }
    let minute: Int64 = 60_000

    func testActivityEntriesUseTheWebWording() {
        let data: JSONObject = [
            "counts": ["views": 7, "readers": 2, "versions": 4, "actions": 1, "webViews": 5, "webReaders": 3, "webLastViewedAt": .int(200 * minute)],
            "events": [
                ["kind": "view", "actor": "ana", "at": .int(150 * minute), "count": 3],
                ["kind": "edit", "actor": "ana", "at": .int(100 * minute), "version": 6],
                ["kind": "edit", "actor": "ana", "at": .int(80 * minute), "version": 5],
                ["kind": "edit", "actor": "ana", "at": .int(60 * minute), "version": 4],
                ["kind": "edit", "actor": "ana", "at": .int(10 * minute), "version": 3],
                ["kind": "share", "actor": "me", "at": .int(5 * minute), "detail": ["type": "user", "name": "bo", "role": "editor"]],
                ["kind": ""],
                ["kind": "create", "actor": "me", "at": 0, "version": 1],
            ],
        ]
        let entries = NoteActivity.entries(data)
        XCTAssertEqual(entries.map(\.kind), ["web", "view", "edit", "edit", "share", "create"])
        XCTAssertEqual(entries.map { $0.line(me: "Me") }, [
            "5 reads of the published page by 3 readers",
            "ana viewed it 3 times",
            "ana edited it · v4–v6, 3 saves",
            "ana edited it · v3",
            "You shared it with bo · can edit",
            "You created the note",
        ])
        XCTAssertEqual(NoteActivity.summary(data), "7 views · 4 versions · 1 action")
        XCTAssertEqual(entries[1].icon, "visibility")
        XCTAssertEqual(NoteActivity.icon("mystery"), "history")
        XCTAssertTrue(NoteActivity.entries([:]).isEmpty)
    }

    func testEveryActivityKindHasItsSentence() {
        func text(_ kind: String, _ detail: JSONObject = [:], count: Int = 1) -> String {
            NoteActivityEntry(kind: kind, actor: "ana", count: count, detail: detail).text
        }
        XCTAssertEqual(text("view"), "viewed it")
        XCTAssertEqual(text("restore", ["version": 3, "as": 9]), "restored version 3 as v9")
        XCTAssertEqual(text("restore", ["version": 3]), "restored version 3")
        XCTAssertEqual(text("role", ["name": "bo", "role": "viewer"]), "changed bo to can view")
        XCTAssertEqual(text("unshare", ["name": "acme"]), "stopped sharing with acme")
        XCTAssertEqual(text("publish"), "published it to the web")
        XCTAssertEqual(text("unpublish"), "unpublished it")
        XCTAssertEqual(text("link", ["owner": "ana", "repo": "site", "number": 12]), "linked ana/site#12")
        XCTAssertEqual(text("unlink", ["owner": "ana", "repo": "site", "number": 12]), "unlinked ana/site#12")
        XCTAssertEqual(text("export"), "downloaded the Markdown")
        XCTAssertEqual(text("copy-link"), "copied the link")
        XCTAssertEqual(text("task"), "turned it into a task")
        XCTAssertEqual(text("issue", ["owner": "ana", "repo": "site", "number": 4]), "filed issue ana/site#4 from it")
        XCTAssertEqual(text("issue"), "filed an issue from it")
        XCTAssertEqual(text("comment"), "commented")
        XCTAssertEqual(text("comment", ["reply": true]), "replied to a comment")
        XCTAssertEqual(text("resolve"), "resolved a comment")
        XCTAssertEqual(text("reopen"), "reopened a comment")
        XCTAssertEqual(text("mystery"), "mystery")
        XCTAssertEqual(NoteActivityEntry(kind: "view").line(me: "me"), "Someone viewed it")
        XCTAssertEqual(NoteLiveTab.allCases.map(\.label), ["History", "Activity", "Chat"])
    }

    @MainActor
    func testActivityModelLoadsAndKeepsTheListOnAFailedReload() async {
        var fail = false
        let api = FakeToolsApi { method, suffix, _ in
            XCTAssertEqual(method, "GET")
            XCTAssertEqual(suffix, "/\(noteId)/activity")
            if fail { throw OrgApiException("note_not_found", "gone") }
            return ["ok": true, "counts": ["views": 1], "events": [["kind": "view", "actor": "ana", "at": .int(toolsNowMillis - 120_000)]]]
        }
        let model = NoteActivityModel(noteId: noteId, me: "me", call: api.call, clock: clock, sleep: instant)
        await model.load()
        XCTAssertTrue(model.loaded)
        XCTAssertEqual(model.entries.count, 1)
        XCTAssertEqual(model.line(model.entries[0]), "ana viewed it")
        XCTAssertEqual(model.when(model.entries[0]), "2m ago")
        XCTAssertEqual(model.summary, "1 view · 0 versions · 0 actions")
        fail = true
        await model.load()
        XCTAssertEqual(model.entries.count, 1)
        XCTAssertEqual(model.error, "")
    }

    @MainActor
    func testChatMessagesUnreadAndSeen() async {
        var messages: [JSONValue] = [
            ["id": "m2", "author": "bo", "body": "second", "createdAt": 20],
            ["id": "m1", "author": "ana", "body": "first", "createdAt": 10],
            ["id": "", "body": "dropped"],
        ]
        let api = FakeToolsApi { _, _, _ in ["ok": true, "messages": .array(messages), "canModerate": false] }
        let seen = NoteChatSeen()
        let model = NoteChatModel(noteId: noteId, me: "Me", call: api.call, clock: clock, sleep: instant, seen: seen)
        await model.load()
        XCTAssertEqual(api.last?.suffix, "/\(noteId)/chat")
        XCTAssertEqual(model.messages.map(\.id), ["m1", "m2"])
        XCTAssertEqual(model.unread, 2)
        XCTAssertEqual(model.badge, "2")
        XCTAssertEqual(model.buttonLabel, "Chat, 2 unread messages")
        model.setVisible(true)
        XCTAssertEqual(model.unread, 0)
        XCTAssertEqual(seen.seenAt(noteId), 20)
        model.setVisible(false)
        messages.append(["id": "m3", "author": "me", "body": "mine", "createdAt": 30])
        messages.append(["id": "m4", "author": "ana", "body": "new", "createdAt": 40])
        await model.load()
        XCTAssertEqual(model.unread, 1)
        XCTAssertEqual(model.buttonLabel, "Chat, 1 unread message")
        XCTAssertEqual(model.authorLabel(model.messages[2]), "You")
        XCTAssertEqual(model.authorLabel(model.messages[0]), "@ana")
        XCTAssertTrue(model.canDelete(model.messages[2]))
        XCTAssertFalse(model.canDelete(model.messages[0]))
        let again = NoteChatModel(noteId: noteId, me: "me", call: api.call, clock: clock, sleep: instant, seen: seen)
        XCTAssertEqual(again.seenAt, 20)
        XCTAssertEqual(NoteChat.unread([NoteChatMessage(id: "x", author: "bo", createdAt: 5)], me: "me", seenAt: 5), 0)
        XCTAssertEqual(NoteChat.badge(0), "")
        XCTAssertEqual(NoteChat.badge(150), "99+")
    }

    @MainActor
    func testChatSendDeleteAndErrors() async {
        let api = FakeToolsApi { method, suffix, body in
            switch (method, suffix) {
            case ("POST", "/\(noteId)/chat"):
                if body?["body"]?.stringValue == "boom" { throw OrgApiException("invalid_message", "bad") }
                return ["ok": true, "messages": [["id": "m1", "author": "me", "body": "hello", "createdAt": 1]], "canModerate": true]
            case ("DELETE", "/\(noteId)/chat/m1"):
                return ["ok": true, "messages": [], "canModerate": true]
            case ("DELETE", "/\(noteId)/chat/gone"):
                throw OrgApiException("message_not_found", "gone")
            default:
                return ["ok": true, "messages": [], "canModerate": false]
            }
        }
        let model = NoteChatModel(noteId: noteId, me: "me", call: api.call, clock: clock, sleep: instant, seen: NoteChatSeen())
        await model.load()
        XCTAssertFalse(model.canSend)
        model.draft = "   "
        XCTAssertFalse(model.canSend)
        model.draft = "  hello  "
        XCTAssertTrue(model.canSend)
        let sent = await model.post()
        XCTAssertNil(sent.notice)
        XCTAssertEqual(api.last?.body, ["body": "hello"])
        XCTAssertEqual(model.draft, "")
        XCTAssertTrue(model.canModerate)
        model.draft = String(repeating: "x", count: 2001)
        let long = await model.post()
        XCTAssertEqual(long.notice?.message, "Write a message of up to 2000 characters.")
        model.draft = "boom"
        let refused = await model.post()
        XCTAssertEqual(refused.notice?.message, "Write a message of up to 2000 characters.")
        XCTAssertEqual(model.draft, "boom")
        let words = model.deleteConfirmation(model.messages[0])
        XCTAssertEqual(words.title, "Delete message?")
        XCTAssertEqual(words.message, "This message will be removed for everyone.")
        XCTAssertEqual(NoteChat.deleteConfirmation(NoteChatMessage(id: "x", author: "bo"), me: "me").message, "bo's message will be removed for everyone.")
        let deleted = await model.delete(model.messages[0])
        XCTAssertEqual(deleted.notice?.message, "Message deleted")
        XCTAssertTrue(model.messages.isEmpty)
        let gone = await model.delete(NoteChatMessage(id: "gone"))
        XCTAssertEqual(gone.notice?.message, "That message was already deleted.")
        XCTAssertEqual(gone.notice?.isError, true)
    }

    @MainActor
    func testHistoryViewsAndRestoresAVersion() async {
        let api = FakeToolsApi { method, suffix, _ in
            switch (method, suffix) {
            case ("GET", "/\(noteId)/versions"):
                return ["ok": true, "versions": [
                    ["version": 4, "createdAt": .int(toolsNowMillis), "author": "bob"],
                    ["version": 3, "createdAt": .int(toolsNowMillis - 3_600_000), "author": "ana"],
                ]]
            case ("GET", "/\(noteId)/versions?version=3"):
                return ["ok": true, "version": [
                    "version": 3, "createdAt": .int(toolsNowMillis - 3_600_000), "author": "ana", "title": "Old", "markdown": "# Old", "emoji": "📝",
                ]]
            case ("GET", "/\(noteId)/versions?version=9"):
                throw OrgApiException("version_not_found", "gone")
            case ("POST", "/\(noteId)/versions"):
                return ["ok": true, "note": .object(noteJSON(version: 5))]
            default:
                throw OrgApiException("unexpected", suffix)
            }
        }
        let history = NoteHistoryModel(noteId: noteId, currentVersion: 4, canRestore: true, call: api.call, clock: clock, sleep: instant)
        await history.load()
        XCTAssertEqual(history.versions?.count, 2)
        XCTAssertFalse(history.restorable(4))
        XCTAssertTrue(history.restorable(3))
        let failure = await history.view(3)
        XCTAssertNil(failure)
        let shown = history.shown!
        XCTAssertEqual(shown.heading, "📝 Old")
        XCTAssertEqual(shown.markdown, "# Old")
        XCTAssertEqual(history.viewTitle(shown), "Version 3")
        XCTAssertEqual(history.viewSubtitle(shown), "1h ago · by @ana")
        let missing = await history.view(9)
        XCTAssertEqual(missing, "That version is no longer kept.")
        XCTAssertEqual(history.restoreConfirmation(3).title, "Restore version 3?")
        let restored = await history.restore(3)
        XCTAssertNotNil(restored.note)
        XCTAssertEqual(api.last?.body, ["version": 3, "baseVersion": 4])
        XCTAssertNil(history.shown)
        XCTAssertFalse(history.busy)
        XCTAssertEqual(history.currentVersion, 5)
        XCTAssertEqual(NoteHistoryModel.restoredMessage(3), "Restored version 3 as a new version")
        history.closeView()
        XCTAssertNil(history.shown)
    }

    @MainActor
    func testTheEditorOpensTheLiveSheetAndReloadsOnFrames() async {
        var chatLoads = 0
        var activityLoads = 0
        var versionLoads = 0
        let api = FakeToolsApi { method, suffix, _ in
            switch (method, suffix) {
            case ("GET", "/\(noteId)"):
                return ["ok": true, "note": .object(noteJSON())]
            case ("GET", "/\(noteId)/chat"):
                chatLoads += 1
                return ["ok": true, "messages": [["id": "m1", "author": "ana", "body": "hi", "createdAt": 5]], "canModerate": false]
            case ("GET", "/\(noteId)/activity"):
                activityLoads += 1
                return ["ok": true, "counts": [:], "events": []]
            case ("GET", "/\(noteId)/versions"):
                versionLoads += 1
                return ["ok": true, "versions": []]
            default:
                return ["ok": true, "comments": [], "people": [], "canResolve": false]
            }
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant, me: "me", chatSeen: NoteChatSeen())
        await editor.load()
        let chat = editor.chat!
        await chat.load()
        XCTAssertEqual(chat.unread, 1)
        editor.showLive(.history)
        XCTAssertEqual(editor.liveTab, .history)
        XCTAssertEqual(editor.history?.currentVersion, 3)
        XCTAssertEqual(chat.unread, 1)
        editor.showLive(.chat)
        XCTAssertEqual(chat.unread, 0)
        editor.showLive(nil)
        XCTAssertNil(editor.liveTab)

        await editor.history?.load()
        await editor.activity?.load()
        editor.roomEvent(.chatChanged)
        editor.roomEvent(.activityChanged)
        editor.roomEvent(.updated(version: 3))
        let reloaded = await eventually { chatLoads == 2 && activityLoads == 2 && versionLoads == 2 }
        XCTAssertTrue(reloaded, "chat \(chatLoads) activity \(activityLoads) versions \(versionLoads)")
        XCTAssertTrue(editor.historyModel() === editor.history)
    }

    func testRoomFramesForActivityAndChat() {
        var presence = NotePresence(clientId: "me")
        XCTAssertEqual(presence.receive(#"{"type":"note-activity"}"#, now: 0).event, .activityChanged)
        XCTAssertEqual(presence.receive(#"{"type":"note-chat","actor":"ana"}"#, now: 0).event, .chatChanged)
        XCTAssertEqual(presence.receive(#"{"type":"note-updated","version":7,"updatedAt":1,"actor":"ana"}"#, now: 0).event, .updated(version: 7))
    }

    @MainActor
    func testChatSeenIsStoredPerNote() {
        let suite = "fm.notes.tests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: suite)!
        defer { defaults.removePersistentDomain(forName: suite) }
        let seen = NoteChatSeen(defaults: defaults)
        seen.record("a", at: 10)
        seen.record("a", at: 5)
        seen.record("b", at: 7)
        XCTAssertEqual(seen.seenAt("a"), 10)
        let again = NoteChatSeen(defaults: defaults)
        XCTAssertEqual(again.seenAt("a"), 10)
        XCTAssertEqual(again.seenAt("b"), 7)
        XCTAssertEqual(again.seenAt("c"), 0)
    }
}
