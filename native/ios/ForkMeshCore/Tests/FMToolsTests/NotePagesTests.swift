import FMApi
import FMBase
import XCTest
@testable import FMTools

let pageId = String(repeating: "d", count: 32)
let secondNoteId = String(repeating: "e", count: 32)
let lockedNoteId = String(repeating: "f", count: 32)

func pageJSON(version: Int = 2, role: String = "owner", outline: [JSONValue]? = nil) -> JSONObject {
    [
        "id": .string(pageId), "title": "Launch hub", "emoji": "🚀", "description": "Everything for **launch**",
        "accent": "purple", "layout": "outline", "showEmoji": true, "role": .string(role),
        "version": .int(Int64(version)), "createdAt": 1, "updatedAt": 2,
        "shares": [["type": "user", "name": "bob", "role": "viewer"]],
        "outline": .array(outline ?? [
            ["id": "sec-plan-1", "kind": "section", "depth": 0, "label": "Plan", "noteId": "", "url": "", "collapsed": false],
            ["id": "ent-note-1", "kind": "note", "depth": 1, "label": "", "noteId": .string(noteId), "url": "", "collapsed": false],
            ["id": "ent-note-2", "kind": "note", "depth": 1, "label": "Specs", "noteId": .string(secondNoteId), "url": "", "collapsed": false],
            ["id": "sec-more-1", "kind": "section", "depth": 0, "label": "More", "noteId": "", "url": "", "collapsed": true],
            ["id": "ent-lock-1", "kind": "note", "depth": 1, "label": "", "noteId": .string(lockedNoteId), "url": "", "collapsed": false],
            ["id": "ent-link-1", "kind": "link", "depth": 0, "label": "", "url": "https://example.com/doc", "noteId": "", "collapsed": false],
        ]),
    ]
}

func pageNotesJSON(withSecondBody: Bool = false) -> JSONObject {
    var second: JSONObject = ["id": .string(secondNoteId), "title": "Spec sheet", "emoji": "", "updatedAt": 50, "lastEditedBy": "bob", "unread": false, "role": "editor"]
    if withSecondBody { second["markdown"] = "spec body" }
    return [
        noteId: ["id": .string(noteId), "title": "Launch plan", "emoji": "📝", "updatedAt": 40, "lastEditedBy": "bob", "unread": true, "role": "owner", "markdown": "# Launch"],
        secondNoteId: .object(second),
    ]
}

final class NoteEmojiTests: XCTestCase {
    func testTheServerRulesHoldOnTheDevice() {
        XCTAssertEqual(NoteEmoji.clean(""), "")
        XCTAssertEqual(NoteEmoji.clean("  "), "")
        XCTAssertEqual(NoteEmoji.clean(" 🚀 "), "🚀")
        XCTAssertEqual(NoteEmoji.clean("👍🏽"), "👍🏽")
        XCTAssertEqual(NoteEmoji.clean("🇫🇷"), "🇫🇷")
        XCTAssertEqual(NoteEmoji.clean("👩‍💻"), "👩‍💻")
        XCTAssertEqual(NoteEmoji.clean("❤️"), "❤️")
        XCTAssertEqual(NoteEmoji.clean("1️⃣"), "1️⃣")
        XCTAssertEqual(NoteEmoji.clean("#️⃣"), "#️⃣")
        XCTAssertEqual(NoteEmoji.clean("🏴󠁧󠁢󠁳󠁣󠁴󠁿"), "🏴󠁧󠁢󠁳󠁣󠁴󠁿")
        XCTAssertNil(NoteEmoji.clean("a"))
        XCTAssertNil(NoteEmoji.clean("🚀a"))
        XCTAssertNil(NoteEmoji.clean("1"))
        XCTAssertNil(NoteEmoji.clean("#"))
        XCTAssertNil(NoteEmoji.clean("🚀 🚀"))
        XCTAssertNil(NoteEmoji.clean("é"))
        XCTAssertNil(NoteEmoji.clean("!"))
        XCTAssertNil(NoteEmoji.clean("\u{0007}"))
        XCTAssertNil(NoteEmoji.clean(String(repeating: "⭐", count: 17)))
        XCTAssertEqual(NoteEmoji.clean(String(repeating: "⭐", count: 16)), String(repeating: "⭐", count: 16))
        XCTAssertEqual(NoteEmoji.clean(String(repeating: "🚀", count: 16))?.utf8.count, 64)
        XCTAssertEqual(NoteEmoji.clean("1\u{20E3}"), "1\u{20E3}")
        XCTAssertEqual(NoteEmoji.clean("🚀1\u{FE0F}\u{20E3}"), "🚀1\u{FE0F}\u{20E3}")
        XCTAssertNil(NoteEmoji.clean("\u{20E3}1"))
        XCTAssertNil(NoteEmoji.clean("1🚀\u{20E3}"))
        XCTAssertEqual(NoteEmoji.common.count, 40)
        for emoji in NoteEmoji.common {
            XCTAssertEqual(NoteEmoji.clean(emoji), emoji, emoji)
        }
    }
}

final class NotePageOutlineTests: XCTestCase {
    @MainActor
    func testRowsShowSectionsNotesLockedEntriesLinksAndCollapse() {
        let page = NotePage(json: pageJSON())
        var notes: [String: NotePageNote] = [:]
        for (key, value) in pageNotesJSON() {
            if let object = value.objectValue { notes[key] = NotePageNote(json: object) }
        }
        let rows = NotePageOutline.rows(page.outline, notes: notes, showEmoji: true)
        XCTAssertEqual(rows.map(\.title), ["Plan", "Launch plan", "Specs", "More", "You don't have access to this note", "example.com/doc"])
        XCTAssertEqual(rows.map(\.depth), [0, 1, 1, 0, 1, 0])
        XCTAssertEqual(rows[1].kind, .note(id: noteId))
        XCTAssertEqual(rows[1].emoji, "📝")
        XCTAssertTrue(rows[1].unread)
        XCTAssertFalse(rows[2].unread)
        XCTAssertEqual(rows[4].kind, .locked)
        XCTAssertEqual(rows[5].kind, .link(url: "https://example.com/doc"))
        XCTAssertNil(rows[0].noteId)
        XCTAssertEqual(NotePageOutline.rows(page.outline, notes: notes, showEmoji: false)[1].emoji, "")

        let folded = NotePageOutline.rows(page.outline, notes: notes, showEmoji: true, collapsed: ["sec-more-1"])
        XCTAssertEqual(folded.map(\.id), ["sec-plan-1", "ent-note-1", "ent-note-2", "sec-more-1"])
        XCTAssertEqual(folded[3].kind, .section(collapsed: true))
        let plan = NotePageOutline.rows(page.outline, notes: notes, showEmoji: true, collapsed: ["sec-plan-1"])
        XCTAssertEqual(plan.map(\.id), ["sec-plan-1", "sec-more-1", "ent-lock-1", "ent-link-1"])

        let seen = NoteViewsSeen()
        seen.record(noteId, updatedAt: 40)
        XCTAssertFalse(NotePageOutline.rows(page.outline, notes: notes, showEmoji: true, seen: seen)[1].unread)
    }

    func testEditingTheOutline() {
        let a = NotePageEntry(id: "aaaaaaaa", kind: .section, label: "A")
        let b = NotePageEntry(id: "bbbbbbbb", kind: .note, noteId: noteId)
        let c = NotePageEntry(id: "cccccccc", kind: .link, url: "https://x.y")
        let d = NotePageEntry(id: "dddddddd", kind: .section, label: "D")
        let outline = [a, b, c, d]
        XCTAssertEqual(NotePageOutline.move(outline, from: IndexSet(integer: 0), to: 4).map(\.id), ["bbbbbbbb", "cccccccc", "dddddddd", "aaaaaaaa"])
        XCTAssertEqual(NotePageOutline.move(outline, from: IndexSet(integer: 3), to: 0).map(\.id), ["dddddddd", "aaaaaaaa", "bbbbbbbb", "cccccccc"])
        XCTAssertEqual(NotePageOutline.move(outline, from: IndexSet([0, 2]), to: 4).map(\.id), ["bbbbbbbb", "dddddddd", "aaaaaaaa", "cccccccc"])
        XCTAssertEqual(NotePageOutline.move(outline, from: IndexSet(integer: 1), to: 1).map(\.id), ["aaaaaaaa", "bbbbbbbb", "cccccccc", "dddddddd"])
        XCTAssertEqual(NotePageOutline.step(outline, "aaaaaaaa", by: -1).map(\.id), outline.map(\.id))
        XCTAssertEqual(NotePageOutline.step(outline, "aaaaaaaa", by: 1).map(\.id), ["bbbbbbbb", "aaaaaaaa", "cccccccc", "dddddddd"])
        var deep = outline
        for _ in 0..<5 { deep = NotePageOutline.indent(deep, "bbbbbbbb", by: 1) }
        XCTAssertEqual(deep[1].depth, 3)
        for _ in 0..<5 { deep = NotePageOutline.indent(deep, "bbbbbbbb", by: -1) }
        XCTAssertEqual(deep[1].depth, 0)
        XCTAssertEqual(NotePageOutline.validLink(" https://example.com/a "), "https://example.com/a")
        XCTAssertNil(NotePageOutline.validLink("http://example.com"))
        XCTAssertNil(NotePageOutline.validLink("https://"))
        XCTAssertNil(NotePageOutline.validLink("https://a b"))
        let id = NotePageEntry.makeId()
        XCTAssertEqual(id.count, 16)
        XCTAssertTrue(id.allSatisfy { $0.isASCII && ($0.isLetter || $0.isNumber) })
        XCTAssertEqual(NotePageEntry(json: a.json), a)
        XCTAssertNil(NotePageEntry(json: ["id": "x", "kind": "bogus"]))
        XCTAssertEqual(NotePageEntry(json: ["id": "abcdefgh", "kind": "note", "depth": 9])?.depth, 3)
    }
}

final class NotePageModelTests: XCTestCase {
    @MainActor
    func testThePagesListCreatesAPage() async {
        let api = FakeToolsApi { method, suffix, body in
            switch (method, suffix) {
            case ("GET", "/pages"):
                return ["ok": true, "pages": [.object(merged(pageJSON(), ["outline": .null, "entries": 6]))]]
            case ("POST", "/pages"):
                return ["ok": true, "page": .object(merged(pageJSON(), ["id": "new-page-id", "title": "Untitled page", "outline": []]))]
            default:
                throw OrgApiException("unexpected", "\(method) \(suffix)")
            }
        }
        let model = NotePagesModel(call: api.call)
        await model.load()
        XCTAssertEqual(model.pages.map(\.title), ["Launch hub"])
        XCTAssertEqual(model.pages[0].entriesLabel, "6 entries")
        XCTAssertEqual(model.pages[0].accent, .purple)
        let created = await model.create()
        XCTAssertEqual(created.id, "new-page-id")
        XCTAssertEqual(model.pages.first?.entriesLabel, "0 entries")
        XCTAssertEqual(api.last?.body, ["title": ""])
    }

    @MainActor
    func testAPageLoadsContentFetchesMissingBodiesAndTracksReading() async {
        let api = FakeToolsApi { method, suffix, _ in
            switch (method, suffix) {
            case ("GET", "/pages/\(pageId)?content=1"):
                return ["ok": true, "page": .object(pageJSON()), "notes": .object(pageNotesJSON())]
            case ("GET", "/\(secondNoteId)"):
                return ["ok": true, "note": ["id": .string(secondNoteId), "markdown": "fetched body"]]
            default:
                throw OrgApiException("unexpected", "\(method) \(suffix)")
            }
        }
        let seen = NoteViewsSeen()
        let model = NotePageModel(pageId: pageId, selectedNoteId: noteId, call: api.call, sleep: { _ in }, webLink: { URL(string: "https://app.test\($0)") }, seen: seen)
        await model.load()
        XCTAssertFalse(model.loading)
        XCTAssertEqual(model.page?.title, "Launch hub")
        XCTAssertEqual(model.page?.emoji, "🚀")
        XCTAssertTrue(model.isOwner)
        XCTAssertEqual(model.collapsed, ["sec-more-1"])
        XCTAssertEqual(model.rows.map(\.id), ["sec-plan-1", "ent-note-1", "ent-note-2", "sec-more-1"])
        XCTAssertEqual(model.allRows.count, 6)
        XCTAssertEqual(model.selectedNoteId, noteId)
        XCTAssertEqual(model.body(noteId), "# Launch")
        XCTAssertNil(model.body(secondNoteId))
        XCTAssertEqual(model.accessLink?.absoluteString, "https://app.test/dashboard/notes?page=\(pageId)")

        await model.ensureAllBodies()
        XCTAssertEqual(model.body(secondNoteId), "fetched body")
        await model.ensureBody(lockedNoteId)
        XCTAssertEqual(api.calls.filter { $0.suffix == "/\(secondNoteId)" }.count, 1)
        XCTAssertFalse(api.calls.contains { $0.suffix == "/\(lockedNoteId)" })

        XCTAssertTrue(model.rows[1].unread)
        model.opened(noteId)
        XCTAssertFalse(model.rows[1].unread)
        model.toggle("sec-more-1")
        XCTAssertEqual(model.rows.count, 6)
    }

    @MainActor
    func testCustomizingSavesEveryFieldAndKeepsTheDraftOnAConflict() async {
        var version = 2
        var conflict = false
        let api = FakeToolsApi { method, suffix, body in
            switch (method, suffix) {
            case ("GET", "/pages/\(pageId)?content=1"):
                return ["ok": true, "page": .object(pageJSON(version: version)), "notes": .object(pageNotesJSON(withSecondBody: true))]
            case ("GET", ""):
                return ["ok": true, "notes": [
                    .object(noteJSON()),
                    .object(merged(noteJSON(), ["id": "ffffffffffffffffffffffffffff0001", "title": "Budget", "emoji": "💰"])),
                    .object(merged(noteJSON(), ["id": "ffffffffffffffffffffffffffff0002", "title": "Retro"])),
                ]]
            case ("PATCH", "/pages/\(pageId)"):
                if conflict {
                    conflict = false
                    version = 3
                    throw OrgApiException("version_conflict", "conflict")
                }
                version += 1
                return ["ok": true, "page": .object(pageJSON(version: version))]
            default:
                throw OrgApiException("unexpected", "\(method) \(suffix)")
            }
        }
        let model = NotePageModel(pageId: pageId, call: api.call, sleep: { _ in }, seen: NoteViewsSeen())
        await model.load()
        model.startCustomize()
        XCTAssertEqual(model.draft?.title, "Launch hub")
        XCTAssertEqual(model.draft?.outline.count, 6)
        await model.loadMyNotes()
        model.noteQuery = "bud"
        XCTAssertEqual(model.noteChoices.map(\.title), ["Budget"])
        model.noteQuery = ""
        XCTAssertEqual(model.noteChoices.map(\.title), ["Budget", "Retro"])
        model.addNote("ffffffffffffffffffffffffffff0001")
        XCTAssertEqual(model.noteChoices.map(\.title), ["Retro"])
        XCTAssertFalse(model.addSection("   "))
        XCTAssertTrue(model.addSection("Later"))
        XCTAssertEqual(model.addLink(label: "Docs", url: "ftp://x").notice?.message, "Enter a link that starts with https://")
        XCTAssertNil(model.addLink(label: "Docs", url: "https://docs.test").notice)
        let added = model.draft!.outline
        XCTAssertEqual(added.suffix(3).map(\.kind), [.note, .section, .link])
        XCTAssertEqual(model.entryTitle(added[added.count - 3]), "Budget")
        XCTAssertEqual(model.entryTitle(added[4]), "You don't have access to this note")
        model.rename("ent-note-1", "  Plan of record  ")
        model.indent("sec-more-1")
        model.outdent("ent-note-2")
        model.moveDown("sec-plan-1")
        model.remove("ent-link-1")
        model.move(from: IndexSet(integer: 0), to: 2)
        XCTAssertEqual(model.draft?.outline.prefix(3).map(\.id), ["sec-plan-1", "ent-note-1", "ent-note-2"])
        XCTAssertEqual(model.draft?.outline.first { $0.id == "ent-note-1" }?.label, "Plan of record")
        XCTAssertEqual(model.draft?.outline.first { $0.id == "sec-more-1" }?.depth, 1)
        XCTAssertEqual(model.draft?.outline.first { $0.id == "ent-note-2" }?.depth, 0)
        model.draft?.title = "  Launch central  "
        model.draft?.emoji = "nope"
        let invalid = await model.saveCustomize()
        XCTAssertEqual(invalid.notice?.message, "Use a single emoji, such as 📝 or 🇫🇷.")
        model.draft?.emoji = "🎯"
        model.draft?.accent = .teal
        model.draft?.layout = .continuous
        model.draft?.showEmoji = false
        model.draft?.description = String(repeating: "x", count: 2001)
        let long = await model.saveCustomize()
        XCTAssertEqual(long.notice?.isError, true)
        model.draft?.description = "New description"

        conflict = true
        let clash = await model.saveCustomize()
        XCTAssertEqual(clash.notice?.message, NotePageModel.conflictMessage)
        XCTAssertNotNil(model.draft)
        XCTAssertEqual(model.page?.version, 3)

        let saved = await model.saveCustomize()
        XCTAssertEqual(saved.notice?.message, "Page saved")
        XCTAssertNil(model.draft)
        let body = api.calls.last { $0.method == "PATCH" }!.body!
        XCTAssertEqual(body["baseVersion"], 3)
        XCTAssertEqual(body["title"], "Launch central")
        XCTAssertEqual(body["emoji"], "🎯")
        XCTAssertEqual(body["accent"], "teal")
        XCTAssertEqual(body["layout"], "continuous")
        XCTAssertEqual(body["showEmoji"], false)
        XCTAssertEqual(body["description"], "New description")
        let sent = body["outline"]!.arrayValue!.compactMap(\.objectValue)
        XCTAssertEqual(sent.count, 8)
        XCTAssertEqual(sent.last?["kind"], "link")
        XCTAssertEqual(sent.last?["url"], "https://docs.test")
        XCTAssertEqual(sent.first { $0["noteId"] == "ffffffffffffffffffffffffffff0001" }?["kind"], "note")
    }

    @MainActor
    func testSharingAPageAlsoSharesItsNotesAndOnlyTheOwnerDeletes() async {
        let api = FakeToolsApi { method, suffix, body in
            switch (method, suffix) {
            case ("GET", "/pages/\(pageId)?content=1"):
                return ["ok": true, "page": .object(pageJSON()), "notes": .object(pageNotesJSON())]
            case ("POST", "/pages/\(pageId)/shares"):
                return ["ok": true, "shares": [["type": "user", "name": "carol", "role": "viewer"]], "sharedNotes": .array([.string(noteId), .string(secondNoteId)])]
            case ("DELETE", "/pages/\(pageId)"):
                return ["ok": true]
            default:
                throw OrgApiException("unexpected", "\(method) \(suffix)")
            }
        }
        let model = NotePageModel(pageId: pageId, call: api.call, sleep: { _ in }, webLink: { URL(string: "https://app.test\($0)") }, seen: NoteViewsSeen())
        await model.load()
        let share = model.shareModel()!
        XCTAssertTrue(share.isPage)
        XCTAssertTrue(share.includeNotes)
        XCTAssertEqual(share.accessLink?.absoluteString, "https://app.test/dashboard/notes?page=\(pageId)")
        XCTAssertEqual(share.subtitleText, NoteShareModel.pageSubtitle)
        share.name = "carol"
        share.nameChanged()
        XCTAssertNil(share.suggestTask)
        let shared = await share.add()
        XCTAssertEqual(api.last?.body, ["type": "user", "name": "carol", "role": "viewer", "includeNotes": true])
        XCTAssertEqual(shared.notice?.message, "Shared with carol, with 2 notes on this page. Copy the link below to send it.")
        share.includeNotes = false
        share.name = "dan"
        _ = await share.add()
        XCTAssertEqual(api.last?.body?["includeNotes"], false)

        let deleted = await model.delete()
        XCTAssertTrue(deleted.close)

        let viewerApi = FakeToolsApi { _, _, _ in ["ok": true, "page": .object(pageJSON(role: "viewer")), "notes": [:]] }
        let viewer = NotePageModel(pageId: pageId, call: viewerApi.call, sleep: { _ in }, seen: NoteViewsSeen())
        await viewer.load()
        XCTAssertFalse(viewer.canEdit)
        XCTAssertNil(viewer.shareModel())
        viewer.startCustomize()
        XCTAssertNil(viewer.draft)
        let refused = await viewer.delete()
        XCTAssertFalse(refused.close)
        XCTAssertEqual(viewerApi.calls.count, 1)
    }

    @MainActor
    func testAMissingPageShowsTheError() async {
        let api = FakeToolsApi { _, _, _ in throw OrgApiException("page_not_found", "gone") }
        let model = NotePageModel(pageId: pageId, call: api.call, sleep: { _ in }, seen: NoteViewsSeen())
        await model.load()
        XCTAssertFalse(model.loading)
        XCTAssertEqual(model.error, "This page no longer exists, or you no longer have access to it.")
    }
}

final class NoteEmojiEditorTests: XCTestCase {
    @MainActor
    func testTheEditorSetsAndClearsTheEmoji() async {
        var emoji = ""
        let api = FakeToolsApi { method, suffix, body in
            switch (method, suffix) {
            case ("GET", "/\(noteId)"):
                return ["ok": true, "note": .object(merged(noteJSON(), ["emoji": .string(emoji)]))]
            case ("PATCH", "/\(noteId)"):
                if let next = body?["emoji"]?.stringValue { emoji = next }
                return ["ok": true, "note": .object(merged(noteJSON(version: 4), ["emoji": .string(emoji)]))]
            case ("POST", ""):
                return ["ok": true, "note": .object(merged(noteJSON(), ["emoji": body?["emoji"] ?? ""]))]
            default:
                throw OrgApiException("unexpected", "\(method) \(suffix)")
            }
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: FixedClock(epochMillis: toolsNowMillis), sleep: { _ in throw CancellationError() }, seen: NoteViewsSeen())
        await editor.load()
        XCTAssertEqual(editor.emoji, "")
        let bad = await editor.setEmoji("abc")
        XCTAssertEqual(bad.notice?.message, NoteEmoji.invalidMessage)
        _ = await editor.setEmoji("🎯")
        XCTAssertEqual(api.last?.body, ["baseVersion": 3, "emoji": "🎯"])
        XCTAssertEqual(editor.emoji, "🎯")
        XCTAssertEqual(editor.version, 4)
        editor.markdown = "changed"
        _ = await editor.setEmoji("")
        XCTAssertEqual(api.last?.body, ["baseVersion": 4, "title": "Launch plan", "markdown": "changed", "emoji": ""])
        XCTAssertEqual(editor.emoji, "")
        XCTAssertNil(editor.emojiDraft)

        let fresh = NoteEditorModel(noteId: nil, call: api.call, sleep: { _ in throw CancellationError() }, seen: NoteViewsSeen())
        _ = await fresh.setEmoji("📚")
        XCTAssertEqual(fresh.emoji, "📚")
        fresh.title = "New"
        _ = await fresh.save()
        XCTAssertEqual(api.last?.body, ["title": "New", "markdown": "", "visibility": "private", "emoji": "📚"])

        let list = NotesListModel(call: { _, _, _ in ["ok": true, "notes": [.object(merged(noteJSON(), ["emoji": "🧪"]))]] }, seen: NoteViewsSeen())
        await list.load()
        XCTAssertEqual(list.rows.first?.emoji, "🧪")
    }
}
