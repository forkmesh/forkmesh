import FMApi
import FMBase
import XCTest
@testable import FMTools

final class NotesParityTests: XCTestCase {
    let clock = FixedClock(epochMillis: toolsNowMillis)
    let instant: ToolsSleep = { _ in }
    let site: ToolsWebLink = { path in URL(string: "https://forkmesh.com" + path) }

    @MainActor
    func testFiltersCountAndSearchCoversTheBody() async {
        let other = String(repeating: "c", count: 32), third = String(repeating: "d", count: 32)
        let api = FakeToolsApi { _, _, _ in
            ["ok": true, "notes": [
                .object(merged(noteJSON(), ["markdown": "Rollout checklist"])),
                .object(merged(noteJSON(role: "viewer"), ["id": .string(other), "title": "Team handbook", "shares": []])),
                .object(merged(noteJSON(), ["id": .string(third), "title": "Public roadmap", "visibility": "public"])),
            ]]
        }
        let list = NotesListModel(call: api.call, clock: clock)
        await list.load()
        XCTAssertEqual(NotesFilter.allCases.map { list.count($0) }, [3, 2, 1, 1])
        XCTAssertEqual(NotesFilter.allCases.map(\.label), ["All", "Mine", "Shared", "Public"])

        list.filter = .shared
        XCTAssertEqual(list.rows.map(\.title), ["Team handbook"])
        list.filter = .published
        XCTAssertEqual(list.rows.map(\.title), ["Public roadmap"])
        XCTAssertEqual(list.filterIndex, 3)

        list.filter = .all
        list.query = "rollout"
        XCTAssertEqual(list.rows.map(\.title), ["Launch plan"])
        XCTAssertEqual(NotesFilter.allCases.map { list.count($0) }, [1, 1, 0, 0])
        list.filter = .shared
        XCTAssertEqual(list.emptyFilterMessage, "No notes match \u{201C}rollout\u{201D}.")
        list.query = ""
        list.filter = .published
        XCTAssertEqual(list.emptyFilterMessage, "You haven't published any notes.")
    }

    @MainActor
    func testAutosaveWaitsForTheLastKeystroke() async {
        var version = 3
        let api = FakeToolsApi { method, _, body in
            if method == "PATCH" {
                version += 1
                return ["ok": true, "note": .object(merged(noteJSON(version: version), ["markdown": body!["markdown"]!]))]
            }
            return ["ok": true, "note": .object(noteJSON())]
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        await editor.load()
        editor.edited()
        XCTAssertNil(editor.autosaveTask)
        editor.markdown = "a"
        editor.edited()
        editor.markdown = "ab"
        editor.edited()
        await editor.autosaveTask?.value
        let patches = api.calls.filter { $0.method == "PATCH" }
        XCTAssertEqual(patches.count, 1)
        XCTAssertEqual(patches[0].body?["markdown"], "ab")
        XCTAssertEqual(editor.saveState, "Saved")
        XCTAssertEqual(editor.version, 4)
    }

    @MainActor
    func testTypingDuringASaveIsKeptAndSavedNext() async {
        var editor: NoteEditorModel!
        var bodies: [JSONObject] = []
        var version = 3
        let call: ToolsApiCall = { method, _, body in
            if method == "GET" { return ["ok": true, "note": .object(noteJSON())] }
            bodies.append(body ?? [:])
            if bodies.count == 1 { editor.markdown = "typed while saving" }
            version += 1
            return ["ok": true, "note": .object(merged(noteJSON(version: version), ["markdown": body!["markdown"]!]))]
        }
        editor = NoteEditorModel(noteId: noteId, call: call, clock: clock, sleep: instant)
        await editor.load()
        editor.markdown = "first"
        _ = await editor.save()
        XCTAssertEqual(editor.markdown, "typed while saving")
        XCTAssertTrue(editor.dirty)
        await editor.autosaveTask?.value
        XCTAssertEqual(bodies.map { $0["markdown"] }, ["first", "typed while saving"])
        XCTAssertEqual(bodies[1]["baseVersion"], 4)
        XCTAssertFalse(editor.dirty)
    }

    @MainActor
    func testAConflictOffersKeepMineOrTakeTheirs() async {
        var refuse = true
        let api = FakeToolsApi { method, _, body in
            switch method {
            case "GET":
                return ["ok": true, "note": .object(merged(noteJSON(version: refuse ? 3 : 5), ["markdown": refuse ? "# Launch\n\n- ship it" : "theirs"]))]
            case "PATCH":
                if refuse {
                    refuse = false
                    throw OrgApiException("version_conflict", "version conflict")
                }
                return ["ok": true, "note": .object(merged(noteJSON(version: 6), ["markdown": body!["markdown"]!]))]
            default:
                throw OrgApiException("unexpected", "unexpected")
            }
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        await editor.load()
        editor.markdown = "mine"
        let outcome = await editor.save()
        XCTAssertEqual(outcome.notice?.message, NoteEditorModel.conflictMessage)
        XCTAssertNotNil(editor.conflict)
        XCTAssertEqual(editor.saveState, "Not saved — resolve the conflict")
        XCTAssertTrue(editor.confirmsLeave)
        XCTAssertFalse(editor.saveEnabled)
        editor.edited()
        XCTAssertNil(editor.autosaveTask)

        let kept = await editor.keepMine()
        XCTAssertEqual(kept.notice?.message, "Note saved")
        XCTAssertEqual(api.last?.body?["baseVersion"], 5)
        XCTAssertEqual(api.last?.body?["markdown"], "mine")
        XCTAssertNil(editor.conflict)
        XCTAssertEqual(editor.version, 6)
    }

    @MainActor
    func testTakeTheirsReplacesTheText() async {
        let api = FakeToolsApi { method, _, _ in
            if method == "PATCH" { throw OrgApiException("version_conflict", "version conflict") }
            return ["ok": true, "note": .object(merged(noteJSON(version: 5), ["markdown": "theirs"]))]
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        await editor.load()
        editor.markdown = "mine"
        _ = await editor.save()
        XCTAssertEqual(editor.takeTheirs().notice?.message, "Showing the latest saved version")
        XCTAssertEqual(editor.markdown, "theirs")
        XCTAssertFalse(editor.dirty)
        XCTAssertEqual(editor.saveState, "Saved")
    }

    @MainActor
    func testAFailedSaveAsksBeforeLeaving() async {
        let api = FakeToolsApi { method, _, _ in
            if method == "PATCH" { throw ApiException("offline") }
            return ["ok": true, "note": .object(noteJSON())]
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        await editor.load()
        editor.title = "Changed"
        let outcome = await editor.save()
        XCTAssertEqual(outcome.notice?.isError, true)
        XCTAssertEqual(editor.saveState, "Not saved")
        XCTAssertTrue(editor.confirmsLeave)
    }

    @MainActor
    func testShareSuggestionsAreDebouncedAndPickSetsTheType() async {
        var share: NoteShareModel!
        var suffixes: [String] = []
        let call: ToolsApiCall = { _, suffix, _ in
            suffixes.append(suffix)
            return ["ok": true, "shares": [], "suggestions": [
                ["type": "user", "name": "bob"],
                ["type": "organization", "name": "Acme"],
                ["type": "user", "name": "alice"],
            ]]
        }
        share = NoteShareModel(
            noteId: noteId, initial: [["type": "user", "name": "bob", "role": "editor"]], call: call, sleep: instant)
        share.name = "@A"
        share.nameChanged()
        share.name = "@Ac"
        share.nameChanged()
        await share.suggestTask?.value
        XCTAssertEqual(suffixes, ["/\(noteId)/shares?query=ac"])
        XCTAssertEqual(share.suggestions.map(\.id), ["organization:acme", "user:alice"])
        XCTAssertEqual(share.suggestions[0].typeLabel, "Organization")

        share.pick(share.suggestions[0])
        XCTAssertEqual(share.name, "acme")
        XCTAssertEqual(share.type, "organization")
        XCTAssertTrue(share.suggestions.isEmpty)
        share.nameChanged()
        XCTAssertTrue(share.suggestions.isEmpty)
        XCTAssertEqual(suffixes.count, 1)

        share.name = "  "
        share.nameChanged()
        XCTAssertTrue(share.suggestions.isEmpty)
        share.name = String(repeating: "x", count: 64)
        share.nameChanged()
        XCTAssertEqual(suffixes.count, 1)
    }

    @MainActor
    func testAStaleSuggestionAnswerIsDropped() async {
        var share: NoteShareModel!
        let call: ToolsApiCall = { _, suffix, _ in
            if suffix.hasSuffix("query=ca") {
                share.name = "cat"
                share.nameChanged()
                await share.suggestTask?.value
                XCTAssertEqual(share.suggestions.map(\.name), ["catherine"])
                return ["ok": true, "shares": [], "suggestions": [["type": "user", "name": "carol"]]]
            }
            return ["ok": true, "shares": [], "suggestions": [["type": "user", "name": "catherine"]]]
        }
        share = NoteShareModel(noteId: noteId, initial: [], call: call, sleep: instant)
        share.name = "ca"
        share.nameChanged()
        let first = share.suggestTask
        await first?.value
        XCTAssertEqual(share.suggestions.map(\.name), ["catherine"])
    }

    @MainActor
    func testRolesRemovalAndTheAccessLink() async {
        let api = FakeToolsApi { method, _, body in
            if method == "DELETE" { return ["ok": true, "shares": []] }
            return ["ok": true, "shares": [["type": "user", "name": "bob", "role": body!["role"]!]]]
        }
        let share = NoteShareModel(
            noteId: noteId, initial: [["type": "user", "name": "bob", "role": "editor"]], call: api.call, sleep: instant, webLink: site)
        XCTAssertTrue(share.showsAccessLink)
        XCTAssertEqual(share.accessLink?.absoluteString, "https://forkmesh.com/dashboard/notes?note=\(noteId)")
        XCTAssertEqual(share.shareRole(share.shares[0]), "editor")
        XCTAssertEqual(NoteShareModel.roles.map(\.label), ["Can edit", "Can view"])

        let changed = await share.setRole(share.shares[0], "viewer")
        XCTAssertEqual(changed.notice?.message, "bob can now view")
        XCTAssertEqual(api.last?.method, "POST")
        XCTAssertEqual(api.last?.suffix, "/\(noteId)/shares")
        XCTAssertEqual(api.last?.body, ["type": "user", "name": "bob", "role": "viewer"])
        XCTAssertEqual(share.shareRole(share.shares[0]), "viewer")

        let removed = await share.stop(share.shares[0])
        XCTAssertEqual(removed.notice?.message, "Removed bob")
        XCTAssertEqual(api.last?.body, ["type": "user", "name": "bob"])
        XCTAssertFalse(share.showsAccessLink)
    }

    @MainActor
    func testEditorLinksForEveryRoleAndThePublishedPage() async {
        var visibility = "private"
        let api = FakeToolsApi { _, _, _ in
            ["ok": true, "note": .object(merged(noteJSON(role: "viewer"), ["visibility": .string(visibility)]))]
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant, webLink: site)
        await editor.load()
        XCTAssertEqual(editor.accessLink?.absoluteString, "https://forkmesh.com/dashboard/notes?note=\(noteId)")
        XCTAssertNil(editor.publicLink)
        visibility = "public"
        await editor.load()
        XCTAssertEqual(editor.publicLink?.absoluteString, "https://forkmesh.com/notes/\(noteId)")
        XCTAssertEqual(editor.menuItems.map(\.value), ["history", "copy_link", "open_public", "export"])
    }

    func testParsesIssueAndPullReferences() {
        XCTAssertEqual(NoteLinkRef.parse(" alice/app#12 ", fallbackKind: "pull"), NoteLinkRef(owner: "alice", repo: "app", kind: "pull", number: 12))
        XCTAssertEqual(
            NoteLinkRef.parse("https://forkmesh.com/alice/my.app/issues/7", fallbackKind: "pull"),
            NoteLinkRef(owner: "alice", repo: "my.app", kind: "issue", number: 7))
        XCTAssertEqual(
            NoteLinkRef.parse("https://forkmesh.com/alice/app?tab=pulls#3", fallbackKind: "issue"),
            NoteLinkRef(owner: "alice", repo: "app", kind: "pull", number: 3))
        XCTAssertEqual(
            NoteLinkRef.parse("/alice/app/discussions/9", fallbackKind: "issue"),
            NoteLinkRef(owner: "alice", repo: "app", kind: "discussion", number: 9))
        XCTAssertNil(NoteLinkRef.parse("alice#12", fallbackKind: "issue"))
        XCTAssertNil(NoteLinkRef.parse("alice/app#0", fallbackKind: "issue"))
        XCTAssertNil(NoteLinkRef.parse("alice/app#x", fallbackKind: "issue"))
        XCTAssertNil(NoteLinkRef.parse("https://forkmesh.com/alice/app", fallbackKind: "issue"))
        let link = NoteLinkRef(owner: "alice", repo: "app", kind: "pull", number: 3)
        XCTAssertEqual(link.title, "app#3")
        XCTAssertEqual(link.subtitle, "alice/app · PR")
        XCTAssertEqual(link.path, "/alice/app?tab=pulls#3")
    }

    @MainActor
    func testLinksAreAddedAndRemoved() async {
        let api = FakeToolsApi { method, suffix, body in
            switch (method, suffix) {
            case ("POST", "/\(noteId)/links"):
                return ["ok": true, "links": [.object(merged(body!, ["createdAt": 1]))]]
            case ("DELETE", "/\(noteId)/links"):
                return ["ok": true, "links": []]
            default:
                return ["ok": true, "note": .object(noteJSON())]
            }
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        await editor.load()
        XCTAssertTrue(editor.showsLinks)
        editor.linkText = "nonsense"
        let refused = await editor.addLink()
        XCTAssertEqual(refused.notice?.message, "Paste an issue or pull request URL, or type owner/repo#123.")
        XCTAssertEqual(api.calls.count, 1)

        editor.title = "Typed"
        editor.linkText = "alice/app#12"
        let added = await editor.addLink()
        XCTAssertEqual(added.notice?.message, "Linked app#12")
        XCTAssertEqual(api.last?.body, ["owner": "alice", "repo": "app", "kind": "issue", "number": 12])
        XCTAssertEqual(editor.links.map(\.title), ["app#12"])
        XCTAssertEqual(editor.linkText, "")
        XCTAssertEqual(editor.title, "Typed")

        let removed = await editor.removeLink(editor.links[0])
        XCTAssertEqual(removed.notice?.message, "Unlinked app#12")
        XCTAssertEqual(api.last?.method, "DELETE")
        XCTAssertTrue(editor.links.isEmpty)
    }

    @MainActor
    func testARepositoryRefusalIsExplained() async {
        let api = FakeToolsApi { method, _, _ in
            if method == "POST" { throw OrgApiException("repository_not_found", "repository not found") }
            return ["ok": true, "note": .object(noteJSON())]
        }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        await editor.load()
        editor.linkText = "alice/secret#1"
        let outcome = await editor.addLink()
        XCTAssertEqual(outcome.notice?.message, "That repository doesn't exist, or you don't have access to it.")
        XCTAssertEqual(editor.linkText, "alice/secret#1")
    }

    @MainActor
    func testExportsMarkdownWithTheTitle() async throws {
        let api = FakeToolsApi { _, _, _ in ["ok": true, "note": .object(noteJSON())] }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: instant)
        await editor.load()
        editor.title = "  Launch plan: Q4! "
        XCTAssertEqual(editor.exportFileName, "launch-plan-q4.md")
        XCTAssertEqual(editor.exportText, "# Launch plan: Q4!\n\n# Launch\n\n- ship it")
        editor.title = "!!!"
        XCTAssertEqual(editor.exportFileName, "note.md")

        let directory = URL(fileURLWithPath: NSTemporaryDirectory())
        let file = try editor.exportFile(in: directory)
        XCTAssertEqual(file.lastPathComponent, "note.md")
        XCTAssertEqual(try String(contentsOf: file, encoding: .utf8), editor.exportText)
        try FileManager.default.removeItem(at: file)
    }

    func testFormattingWrapsTheSelectionOrPrefixesTheLine() {
        let bold = NoteFormatting.apply(.bold, to: "make this loud", selection: 10..<14)
        XCTAssertEqual(bold.text, "make this **loud**")
        XCTAssertEqual(bold.selection, 12..<16)
        let link = NoteFormatting.apply(.link, to: "", selection: 0..<0)
        XCTAssertEqual(link.text, "[link text](https://)")
        XCTAssertEqual(link.selection, 1..<10)
        let heading = NoteFormatting.apply(.heading, to: "one\ntwo", selection: 5..<6)
        XCTAssertEqual(heading.text, "one\n## two")
        XCTAssertEqual(heading.selection, 8..<9)
        XCTAssertEqual(NoteFormatting.apply(.code, to: "é", selection: 0..<1).text, "`é`")
    }

    @MainActor
    func testTheToolbarFormatsTheEndAndPreviewIsAMode() async {
        let api = FakeToolsApi { _, _, _ in ["ok": true, "note": .object(noteJSON())] }
        let editor = NoteEditorModel(noteId: noteId, call: api.call, clock: clock, sleep: { _ in throw CancellationError() })
        await editor.load()
        XCTAssertEqual(editor.shownMode, .write)
        editor.format(.task)
        XCTAssertEqual(editor.markdown, "# Launch\n\n- [ ] - ship it")
        XCTAssertEqual(editor.counts, "8 words · 1 min read")
        editor.mode = .preview
        XCTAssertEqual(editor.shownMode, .preview)
    }
}
