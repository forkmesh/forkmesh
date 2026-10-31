import FMApi
import FMBase
import FMNet
import XCTest
@testable import FMTools

func commentJSON(
    _ id: String, parent: String = "", author: String = "ana", body: String = "Looks good", quote: String = "",
    prefix: String = "", suffix: String = "", start: Int = 0, createdAt: Int64 = 1, resolvedAt: Int64 = 0,
    mentions: [String] = []
) -> JSONValue {
    .object([
        "id": .string(id), "parentId": .string(parent), "author": .string(author), "body": .string(body),
        "quote": .string(quote), "prefix": .string(prefix), "suffix": .string(suffix),
        "start": .int(Int64(start)), "end": .int(Int64(start + quote.utf16.count)),
        "mentions": .array(mentions.map { .string($0) }), "createdAt": .int(createdAt), "updatedAt": .int(createdAt),
        "editedAt": 0, "resolvedAt": .int(resolvedAt), "resolvedBy": .string(resolvedAt > 0 ? "bob" : ""),
    ])
}

func commentsPayload(_ comments: [JSONValue], canResolve: Bool = true, people: [String] = ["alice", "ana", "bob"]) -> JSONObject {
    ["ok": true, "comments": .array(comments), "people": .array(people.map { .string($0) }), "canResolve": .bool(canResolve)]
}

final class NoteAnchorTests: XCTestCase {
    func testCaptureTakesTheQuoteAndUpToThirtyTwoUnitsOfContext() {
        let text = String(repeating: "a", count: 40) + "TARGET" + String(repeating: "b", count: 40)
        let quote = NoteAnchor.capture(text, selection: 40..<46)
        XCTAssertEqual(quote?.quote, "TARGET")
        XCTAssertEqual(quote?.prefix, String(repeating: "a", count: 32))
        XCTAssertEqual(quote?.suffix, String(repeating: "b", count: 32))
        XCTAssertEqual(quote?.start, 40)
        XCTAssertEqual(quote?.end, 46)
        XCTAssertEqual(quote?.body, ["quote": "TARGET", "prefix": .string(String(repeating: "a", count: 32)),
                                     "suffix": .string(String(repeating: "b", count: 32)), "start": 40, "end": 46])

        let short = NoteAnchor.capture("one two three", selection: 4..<7)
        XCTAssertEqual(short, NoteQuote(quote: "two", prefix: "one ", suffix: " three", start: 4, end: 7))
        XCTAssertNil(NoteAnchor.capture("one two", selection: 3..<4))
        XCTAssertNil(NoteAnchor.capture("one two", selection: 2..<2))
        XCTAssertEqual(NoteAnchor.capture("abc", selection: 1..<99)?.quote, "bc")

        let long = String(repeating: "x", count: 1500)
        XCTAssertEqual(NoteAnchor.capture(long, selection: 0..<1500)?.quote.utf16.count, 1000)
    }

    func testCaptureCountsUTF16AndNeverSplitsASurrogatePair() {
        let text = "\u{1F600}hi there"
        XCTAssertEqual(NoteAnchor.capture(text, selection: 2..<4)?.quote, "hi")
        XCTAssertEqual(NoteAnchor.capture(text, selection: 2..<4)?.prefix, "\u{1F600}")
        XCTAssertEqual(NoteAnchor.capture(text, selection: 1..<4)?.quote, "hi")
        let context = "\u{1F600}" + String(repeating: "c", count: 31) + "Q"
        let quote = NoteAnchor.capture(context, selection: 33..<34)
        XCTAssertEqual(quote?.quote, "Q")
        XCTAssertEqual(quote?.prefix, String(repeating: "c", count: 31))
        let trailing = "Q" + String(repeating: "c", count: 31) + "\u{1F600}"
        XCTAssertEqual(NoteAnchor.capture(trailing, selection: 0..<1)?.suffix, String(repeating: "c", count: 31))
    }

    func testLocateScoresContextThenDistance() {
        let text = "the cat sat. the cat ran. the cat sat."
        XCTAssertEqual(NoteAnchor.locate(quote: "cat", prefix: "the ", suffix: " ran", start: 0, in: text), 17..<20)
        XCTAssertEqual(NoteAnchor.locate(quote: "cat", prefix: "the ", suffix: " sat", start: 30, in: text), 30..<33)
        XCTAssertEqual(NoteAnchor.locate(quote: "cat", prefix: "the ", suffix: " sat", start: 0, in: text), 4..<7)
        XCTAssertEqual(NoteAnchor.locate(quote: "cat", prefix: "zzz", suffix: "zzz", start: 18, in: text), 17..<20)
        XCTAssertEqual(NoteAnchor.locate(quote: "cat", prefix: "", suffix: "", start: 100, in: text), 30..<33)
        XCTAssertNil(NoteAnchor.locate(quote: "dog", prefix: "", suffix: "", start: 0, in: text))
        XCTAssertNil(NoteAnchor.locate(quote: "", prefix: "", suffix: "", start: 0, in: text))
        XCTAssertEqual(NoteAnchor.locate(quote: "aa", prefix: "", suffix: "", start: 1, in: "aaa"), 1..<3)
    }

    func testLocateFollowsTextThatMovedAndCountsUTF16() {
        let original = "Intro\n\nShip the release on Friday."
        let quote = NoteAnchor.capture(original, selection: 16..<23)!
        XCTAssertEqual(quote.quote, "release")
        let edited = "\u{1F680} New line\n" + original
        let found = NoteAnchor.locate(quote: quote.quote, prefix: quote.prefix, suffix: quote.suffix, start: quote.start, in: edited)
        XCTAssertEqual(found, 28..<35)
        XCTAssertEqual((edited as NSString).substring(with: NSRange(location: 28, length: 7)), "release")
    }
}

final class NoteThreadTests: XCTestCase {
    func testThreadsAreInDocumentOrderWithWholeFirstDetachedLastAndResolvedApart() {
        let markdown = "alpha beta gamma"
        let comments: [NoteComment] = [
            NoteComment(id: "c-gamma", body: "g", quote: "gamma", start: 11, createdAt: 1),
            NoteComment(id: "c-gone", body: "x", quote: "delta", start: 0, createdAt: 2),
            NoteComment(id: "c-whole-2", body: "w2", createdAt: 5),
            NoteComment(id: "c-alpha", body: "a", quote: "alpha", start: 0, createdAt: 3),
            NoteComment(id: "c-whole-1", body: "w1", createdAt: 4),
            NoteComment(id: "c-beta", body: "b", quote: "beta", start: 6, createdAt: 6, resolvedAt: 9),
            NoteComment(id: "r2", parentId: "c-alpha", body: "second", createdAt: 8),
            NoteComment(id: "r1", parentId: "c-alpha", body: "first", createdAt: 7),
            NoteComment(id: "orphan", parentId: "missing", body: "?", createdAt: 7),
        ]
        let list = NoteThreadList.build(comments, markdown: markdown)
        XCTAssertEqual(list.open.map(\.id), ["c-whole-1", "c-whole-2", "c-alpha", "c-gamma", "c-gone"])
        XCTAssertEqual(list.resolved.map(\.id), ["c-beta"])
        XCTAssertEqual(list.open[2].replies.map(\.id), ["r1", "r2"])
        XCTAssertEqual(list.open[2].place, .inline(0..<5))
        XCTAssertEqual(list.open[3].range, 11..<16)
        XCTAssertTrue(list.open[4].detached)
        XCTAssertEqual(list.open[0].place, .whole)
        XCTAssertNil(list.open[0].range)
        XCTAssertEqual(list.all.count, 6)
    }
}

final class NoteMentionTests: XCTestCase {
    func testOnlyKeptMentionsAreHighlighted() {
        let spans = NoteMentions.segments("Hi @Ana and @bob, mail me@x.org or @zed-.", mentions: ["ana", "zed"])
        XCTAssertEqual(spans, [
            NoteMentionSpan("Hi "), NoteMentionSpan("@Ana", mention: true), NoteMentionSpan(" and @bob, mail me@x.org or "),
            NoteMentionSpan("@zed", mention: true), NoteMentionSpan("-."),
        ])
        XCTAssertEqual(NoteMentions.segments("plain", mentions: []), [NoteMentionSpan("plain")])
        XCTAssertEqual(NoteMentions.segments("", mentions: ["a"]), [])
        XCTAssertEqual(NoteMentions.names(in: "@Ana @ana @b-o @9x a@b @@c _@d"), ["ana", "b-o"])
    }

    func testTypingAtSuggestsPeopleAndCompletes() {
        let people = ["alice", "ana", "bob", "jana", "me"]
        XCTAssertNil(NoteMentions.query("no mention"))
        XCTAssertNil(NoteMentions.query("mail a@"))
        XCTAssertNil(NoteMentions.query("@ana done"))
        XCTAssertEqual(NoteMentions.query("hey @"), "")
        XCTAssertEqual(NoteMentions.query("hey @An"), "an")
        XCTAssertEqual(NoteMentions.suggestions(people, for: "hey @an", me: "me"), ["ana", "jana"])
        XCTAssertEqual(NoteMentions.suggestions(people, for: "@", me: "Me"), ["alice", "ana", "bob", "jana"])
        XCTAssertEqual(NoteMentions.suggestions(people, for: "@ana", me: "me"), ["jana"])
        XCTAssertEqual(NoteMentions.suggestions(people, for: "hello", me: "me"), [])
        XCTAssertEqual(NoteMentions.complete("thanks @an", with: "ana"), "thanks @ana ")
        XCTAssertEqual(NoteMentions.complete("no at", with: "ana"), "no at")
    }
}

final class NoteCommentsModelTests: XCTestCase {
    let clock = FixedClock(epochMillis: toolsNowMillis)
    let instant: ToolsSleep = { _ in }

    @MainActor
    func testLoadsPostsRepliesEditsResolvesAndDeletes() async {
        var stored: [JSONValue] = [commentJSON("t1", author: "bob", body: "Hi @ana", quote: "Launch", start: 2, mentions: ["ana"])]
        let api = FakeToolsApi { method, suffix, body in
            switch (method, suffix) {
            case ("GET", "/\(noteId)/comments"):
                return commentsPayload(stored, canResolve: false)
            case ("POST", "/\(noteId)/comments"):
                let id = "n\(stored.count)"
                stored.append(commentJSON(id, parent: body?["parentId"]?.stringValue ?? "", author: "alice",
                                          body: body?["body"]?.stringValue ?? "", quote: body?["quote"]?.stringValue ?? ""))
                var payload = commentsPayload(stored, canResolve: false)
                payload["comment"] = stored.last!
                return payload
            case ("PATCH", "/\(noteId)/comments/t1"):
                throw OrgApiException("resolve_not_allowed", "no")
            case ("PATCH", "/\(noteId)/comments/n1"):
                stored[1] = commentJSON("n1", author: "alice", body: body?["body"]?.stringValue ?? "", resolvedAt: body?["resolved"] == true ? 5 : 0)
                return commentsPayload(stored, canResolve: false)
            case ("DELETE", "/\(noteId)/comments/n1"):
                stored = stored.filter { $0.objectValue?["id"]?.stringValue != "n1" && $0.objectValue?["parentId"]?.stringValue != "n1" }
                return commentsPayload(stored, canResolve: false)
            default:
                throw OrgApiException("unexpected", "\(method) \(suffix)")
            }
        }
        let model = NoteCommentsModel(noteId: noteId, me: "Alice", call: api.call, clock: clock, sleep: instant)
        await model.load()
        XCTAssertTrue(model.loaded)
        XCTAssertEqual(model.comments.map(\.id), ["t1"])
        XCTAssertEqual(model.comments[0].mentions, ["ana"])
        XCTAssertEqual(model.people, ["alice", "ana", "bob"])
        XCTAssertEqual(model.openCount, 1)
        XCTAssertEqual(model.buttonLabel, "Comments, 1 open")
        let thread = model.threads(markdown: "# Launch").open[0]
        XCTAssertFalse(model.canResolve(thread))
        XCTAssertFalse(model.canEdit(thread.comment))
        XCTAssertFalse(model.canDelete(thread.comment))
        model.isOwner = true
        XCTAssertTrue(model.canDelete(thread.comment))
        model.isOwner = false
        XCTAssertEqual(model.when(thread.comment), "56y ago")

        let refused = await model.setResolved(thread, true)
        XCTAssertEqual(refused.notice?.message, "Only the note's owner, an editor or the thread's author can resolve it.")
        XCTAssertEqual(refused.notice?.isError, true)

        model.startComment(nil)
        model.draft = "   "
        let blank = await model.post()
        XCTAssertNil(blank.notice)
        XCTAssertEqual(api.calls.count, 2)
        model.draft = String(repeating: "x", count: 4001)
        let tooLong = await model.post()
        XCTAssertEqual(tooLong.notice?.isError, true)
        model.draft = "  On the whole note  "
        _ = await model.post()
        XCTAssertEqual(api.last?.body, ["body": "On the whole note"])
        XCTAssertFalse(model.composing)
        XCTAssertEqual(model.focusedId, "n1")
        XCTAssertEqual(model.openCount, 2)

        let mine = model.threads(markdown: "# Launch").open.first { $0.id == "n1" }!
        XCTAssertEqual(mine.place, .whole)
        XCTAssertTrue(model.canResolve(mine))
        XCTAssertTrue(model.canEdit(mine.comment))
        XCTAssertTrue(model.canDelete(mine.comment))

        model.startComment(NoteQuote(quote: "Launch", prefix: "# ", suffix: "", start: 2, end: 8))
        model.draft = "Rename?"
        _ = await model.post()
        XCTAssertEqual(api.last?.body, ["body": "Rename?", "quote": "Launch", "prefix": "# ", "suffix": "", "start": 2, "end": 8])

        model.replies["t1"] = "Agreed @bob"
        _ = await model.reply(to: "t1")
        XCTAssertEqual(api.last?.body, ["body": "Agreed @bob", "parentId": "t1"])
        XCTAssertNil(model.replies["t1"])
        XCTAssertEqual(model.threads(markdown: "# Launch").open.first { $0.id == "t1" }?.replies.count, 1)
        let t1 = model.comments.first { $0.id == "t1" }!
        XCTAssertEqual(model.replyCount(t1), 1)
        XCTAssertEqual(model.deleteConfirmation(t1).title, "Delete thread?")
        XCTAssertEqual(model.deleteConfirmation(t1).message, "This removes the comment and its reply for everyone.")
        XCTAssertEqual(model.deleteConfirmation(mine.comment), NoteCommentsModel.deleteConfirmation)
        let reply = model.comments.first { $0.parentId == "t1" }!
        XCTAssertEqual(model.replyCount(reply), 0)
        XCTAssertTrue(model.canDelete(reply))
        XCTAssertFalse(model.canDelete(t1))
        model.isOwner = true
        XCTAssertTrue(model.canDelete(t1))
        XCTAssertFalse(model.canEdit(t1))
        model.isOwner = false

        model.startEdit(thread.comment)
        XCTAssertNil(model.editingId)
        model.startEdit(mine.comment)
        XCTAssertEqual(model.editText, "On the whole note")
        model.editText = "Edited"
        _ = await model.saveEdit()
        XCTAssertEqual(api.last?.method, "PATCH")
        XCTAssertEqual(api.last?.body, ["body": "Edited"])
        XCTAssertNil(model.editingId)

        let resolved = await model.setResolved(mine, true)
        XCTAssertEqual(resolved.notice?.message, "Thread resolved")
        XCTAssertEqual(api.last?.body, ["resolved": true])
        let list = model.threads(markdown: "# Launch")
        XCTAssertEqual(list.resolved.map(\.id), ["n1"])
        XCTAssertEqual(model.resolvedTitle(list), "Resolved (1)")
        XCTAssertEqual(model.resolvedLine(list.resolved[0]), "Resolved by bob · 56y ago")

        XCTAssertEqual(model.focusedId, "n2")
        model.focus("n1")
        XCTAssertTrue(model.showResolved)
        let deleted = await model.delete(mine.comment)
        XCTAssertEqual(deleted.notice?.message, "Thread deleted")
        XCTAssertNil(model.focusedId)
        XCTAssertFalse(model.comments.contains { $0.id == "n1" })
    }

    @MainActor
    func testReloadsAreDebouncedAndFocusOpensTheThread() async {
        var gets = 0
        let api = FakeToolsApi { _, _, _ in
            gets += 1
            return commentsPayload([
                commentJSON("t1", resolvedAt: 4),
                commentJSON("r1", parent: "t1", body: "reply"),
            ])
        }
        let model = NoteCommentsModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        model.reloadSoon()
        model.reloadSoon()
        model.reloadSoon()
        await model.reloadTask?.value
        try? await Task.sleep(nanoseconds: 20_000_000)
        XCTAssertEqual(gets, 1)
        XCTAssertFalse(model.showResolved)
        model.focus("r1")
        XCTAssertEqual(model.focusedThreadId, "t1")
        XCTAssertTrue(model.showResolved)
        model.focus("missing")
        XCTAssertNil(model.focusedThreadId)
    }

    @MainActor
    func testAFailedFirstLoadShowsTheError() async {
        let api = FakeToolsApi { _, _, _ in throw OrgApiException("note_not_found", "gone") }
        let model = NoteCommentsModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        await model.load()
        XCTAssertFalse(model.loaded)
        XCTAssertEqual(model.error, "This note no longer exists, or you no longer have access to it.")
    }
}

final class NoteEditorCommentsTests: XCTestCase {
    let clock = FixedClock(epochMillis: toolsNowMillis)

    @MainActor
    func testALinkOpensTheThreadAndPickingSelectsItsPassage() async {
        let api = FakeToolsApi { _, suffix, _ in
            if suffix.hasSuffix("/comments") {
                return commentsPayload([
                    commentJSON("t1", quote: "ship it", prefix: "- ", start: 12),
                    commentJSON("r1", parent: "t1", body: "done"),
                ])
            }
            return ["ok": true, "note": .object(noteJSON())]
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: { _ in }, me: "alice", openComment: "r1")
        await editor.load()
        XCTAssertTrue(editor.commentsOpen)
        XCTAssertTrue(editor.canComment)
        await editor.comments?.load()
        XCTAssertEqual(editor.comments?.focusedThreadId, "t1")
        XCTAssertEqual(editor.highlightRange, 12..<19)

        editor.selectionChanged(13..<15)
        XCTAssertEqual(editor.comments?.focusedThreadId, "t1")
        editor.selectionChanged(0..<1)
        XCTAssertNil(editor.comments?.focusedThreadId)
        XCTAssertNil(editor.highlightRange)

        editor.mode = .preview
        let thread = editor.comments!.threads(markdown: editor.markdown).open[0]
        editor.pick(thread)
        XCTAssertEqual(editor.mode, .write)
        XCTAssertEqual(editor.selectionRequest, NoteSelectionRequest(range: 12..<19, serial: 1))
        XCTAssertEqual(editor.selection, 12..<19)
        XCTAssertEqual(editor.highlightRange, 12..<19)

        editor.commentsOpen = false
        editor.selectionChanged(2..<8)
        editor.commentOnSelection()
        XCTAssertTrue(editor.commentsOpen)
        XCTAssertEqual(editor.comments?.draftQuote, NoteQuote(quote: "Launch", prefix: "# ", suffix: "\n\n- ship it", start: 2, end: 8))
        editor.mode = .preview
        editor.commentOnSelection()
        XCTAssertNil(editor.comments?.draftQuote)
    }

    @MainActor
    func testViewersCommentFromTheMarkdownViewAndRoomFramesReload() async {
        let connector = FakeWebSocketConnector()
        var commentGets = 0
        let api = FakeToolsApi { _, suffix, _ in
            if suffix.hasSuffix("/comments") {
                commentGets += 1
                return commentsPayload([], canResolve: false)
            }
            return ["ok": true, "note": .object(noteJSON(role: "viewer"))]
        }
        let editor = NoteEditorModel(
            noteId: noteId, call: api.call, clock: clock, sleep: { millis in if millis >= 1000 { try await Task.sleep(nanoseconds: 60_000_000_000) } },
            openSocket: { _ in try await connector.connect(url: URL(string: "wss://forkmesh.test/ws")!) })
        await editor.load()
        XCTAssertFalse(editor.commentsOpen)
        XCTAssertEqual(editor.shownMode, .preview)
        XCTAssertTrue(editor.canComment)
        XCTAssertEqual(NoteMode.write.label(canEdit: editor.canEdit), "Markdown")
        XCTAssertEqual(NoteMode.write.label(canEdit: true), "Write")
        editor.mode = .write
        XCTAssertEqual(editor.shownMode, .write)
        editor.format(.bold)
        XCTAssertEqual(editor.markdown, "# Launch\n\n- ship it")

        _ = await connector.waitForConnections(1)
        connector.lastConnection!.receive(text: #"{"type":"note-comments","actor":"bob"}"#)
        let reloaded = await eventually { commentGets == 1 }
        XCTAssertTrue(reloaded)
        editor.leave()
    }

    @MainActor
    func testTheToolbarFormatsTheSelection() async {
        let api = FakeToolsApi { _, _, _ in ["ok": true, "note": .object(noteJSON())] }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: { _ in throw CancellationError() })
        await editor.load()
        editor.selectionChanged(2..<8)
        editor.format(.bold)
        XCTAssertEqual(editor.markdown, "# **Launch**\n\n- ship it")
        XCTAssertEqual(editor.selectionRequest?.range, 4..<10)
    }
}
