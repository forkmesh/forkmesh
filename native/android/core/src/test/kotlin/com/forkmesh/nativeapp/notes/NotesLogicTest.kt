package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq

class NotesLogicTest {
    fun testReachAndSubtitles() {
        val now = 1_700_000_000_000L
        assertEq("Public", NotesLogic.reach(mapOf("visibility" to "public")))
        assertEq("Shared with 2", NotesLogic.reach(mapOf("shares" to listOf(mapOf("name" to "a"), mapOf("name" to "b")))))
        assertEq("Private", NotesLogic.reach(emptyMap()))
        val shared = mapOf("updatedAt" to now, "role" to "editor", "version" to 3L)
        expectStartsWith("Edited ", NotesLogic.rowSubtitle(shared, now))
        expectEndsWith(" · Private · Can edit", NotesLogic.rowSubtitle(shared, now))
        expectEndsWith(" · Read only", NotesLogic.rowSubtitle(mapOf("role" to "viewer"), now))
        expectStartsWith("Version 3 · edited ", NotesLogic.editorMeta(shared, now))
        expectEndsWith(" · read only", NotesLogic.editorMeta(mapOf("role" to "viewer"), now))
        assertEq("Only you can see a new note until you share or publish it.", NotesLogic.editorMeta(null))
    }

    fun testSearchAndShareNames() {
        val notes = listOf(mapOf("title" to "Groceries"), mapOf("title" to "Roadmap"))
        assertEq(listOf("Roadmap"), NotesLogic.filter(notes, " road ").map { it["title"] })
        assertEq(2, NotesLogic.filter(notes, "").size)
        assertEq("alice", NotesLogic.shareName("  @@Alice "))
    }

    fun testErrorsAndRoles() {
        assertEq(
            "Someone else saved this note first. Copy your changes, then refresh to see theirs.",
            NotesLogic.saveError(OrgApiException("version_conflict", "x")),
        )
        assertEq("The note could not be saved.", NotesLogic.saveError(IllegalStateException()))
        assertEq("No organization has that name.", NotesLogic.shareError(OrgApiException("principal_not_found", ""), "organization"))
        assertEq(true, NotesLogic.canEdit(null))
        assertEq(false, NotesLogic.canEdit(mapOf("role" to "viewer")))
    }

    fun testRequestsGoToTheNotesApi() {
        val calls = mutableListOf<String>()
        val api = object : ApiService(TestSettings()) {
            override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
                calls.add("$method $path ${body ?: ""}")
                return mapOf("notes" to listOf(mapOf("id" to "n1")), "note" to mapOf("id" to "n1"), "shares" to emptyList<Any>())
            }
        }
        val notes = NotesApi(api)
        assertEq(1, notes.list().size)
        notes.save("n1", 4, " T ", "body")
        notes.addShare("n1", "user", "@Bob", "viewer")
        assertEq("GET /api/notes ", calls[0])
        assertEq("PATCH /api/notes/n1 {baseVersion=4, title=T, markdown=body}", calls[1])
        assertEq("POST /api/notes/n1/shares {type=user, name=bob, role=viewer}", calls[2])
    }

    fun testFiltersCountsAndServerSearch() {
        val notes = listOf(
            mapOf("id" to "a", "title" to "Groceries", "role" to "owner", "visibility" to "private"),
            mapOf("id" to "b", "title" to "Roadmap", "role" to "owner", "visibility" to "public"),
            mapOf("id" to "c", "title" to "Team plan", "role" to "editor", "visibility" to "private"),
        )
        val counts = NotesLogic.counts(notes)
        assertEq(3, counts[NoteFilter.All])
        assertEq(2, counts[NoteFilter.Mine])
        assertEq(1, counts[NoteFilter.Shared])
        assertEq(1, counts[NoteFilter.Public])
        assertEq(listOf("c"), NotesLogic.filter(notes, "", NoteFilter.Shared).map { it["id"] })
        assertEq(notes, NotesLogic.searchResults(notes, "  ", null))
        assertEq(listOf("c"), NotesLogic.searchResults(notes, " PLAN", null).map { it["id"] })
        assertEq(true, NotesLogic.searchPending(" PLAN", null))
        val answer = NoteSearch("plan", listOf(notes[0], notes[2]))
        assertEq(false, NotesLogic.searchPending(" PLAN", answer))
        val found = NotesLogic.searchResults(notes, " PLAN", answer)
        assertEq(listOf("a", "c"), found.map { it["id"] })
        assertEq(1, NotesLogic.counts(found)[NoteFilter.Mine])
        assertEq(listOf("a"), NotesLogic.filter(found, "", NoteFilter.Mine).map { it["id"] })
        assertEq(listOf("b"), NotesLogic.searchResults(notes, "road", answer).map { it["id"] })
        assertEq(true, NotesLogic.searchPending("road", answer))
        assertEq("", NotesLogic.searchQuery("x".repeat(201)))
        assertEq(false, NotesLogic.searchPending("x".repeat(201), null))
        assertEq("Searching notes\u2026", NotesLogic.emptyListText("eggs", NoteFilter.All, pending = true))
        assertEq("No notes match \u201cxyz\u201d.", NotesLogic.emptyListText(" xyz ", NoteFilter.All))
        assertEq("Nobody has shared a note with you yet.", NotesLogic.emptyListText("", NoteFilter.Shared))
    }

    fun testLinksAndUrls() {
        assertEq(NoteLink("acme", "web.app", "pull", 12), NoteLink.parse(" acme/web.app#12 ", "pull"))
        assertEq(NoteLink("acme", "web", "issue", 7), NoteLink.parse("acme/web#7", "bogus"))
        assertEq(NoteLink("acme", "web", "pull", 3), NoteLink.parse("https://app.forkmesh.com/acme/web/pull/3"))
        assertEq(NoteLink("acme", "web", "pull", 4), NoteLink.parse("https://forkmesh.com/acme/web/pulls/4/files"))
        assertEq(NoteLink("acme", "web", "discussion", 9), NoteLink.parse("/acme/web/discussions/9"))
        assertEq(NoteLink("acme", "web", "issue", 5), NoteLink.parse("https://app.forkmesh.com/acme/web?tab=issues#5"))
        assertEq(null, NoteLink.parse("acme/web"))
        assertEq(null, NoteLink.parse("https://app.forkmesh.com/acme/web/issues/0"))
        assertEq(null, NoteLink.parse(""))
        val pull = NoteLink("acme", "web", "pull", 3)
        assertEq("web#3", pull.label)
        assertEq("https://h.example/acme/web/pulls/3", NotesLogic.linkUrl("https://h.example/", pull))
        assertEq("/acme/web/issues/1", NoteLink("acme", "web", "issue", 1).path)
        assertEq(listOf(pull), NoteLink.list(listOf(mapOf("owner" to "acme", "repo" to "web", "kind" to "pull", "number" to 3.0), mapOf("owner" to ""))))
        assertEq("https://h.example/dashboard/notes?note=abc", NotesLogic.accessUrl("https://h.example/", "abc"))
        assertEq("https://h.example/notes/abc", NotesLogic.publicUrl("https://h.example", "abc"))
        assertEq("wss://h.example/api/notes/abc/ws", NotePresence.roomUrl("https://h.example/", "abc"))
        assertEq("ws://localhost:8787/api/notes/abc/ws", NotePresence.roomUrl("http://localhost:8787", "abc"))
    }

    fun testSuggestionsAndShareText() {
        assertEq("bo", NotesLogic.suggestionQuery(" @Bo "))
        assertEq("", NotesLogic.suggestionQuery("x".repeat(64)))
        assertEq(63, NotesLogic.suggestionQuery("x".repeat(63)).length)
        val parsed = NotesLogic.suggestions(mapOf("suggestions" to listOf(
            mapOf("type" to "user", "name" to "bob"),
            mapOf("type" to "organization", "name" to "bobcats"),
            mapOf("type" to "user", "name" to ""),
        )))
        assertEq(listOf(NoteShareSuggestion("user", "bob"), NoteShareSuggestion("organization", "bobcats")), parsed)
        assertEq("Organization", parsed[1].typeLabel)
        assertEq("Shared with bob. Copy the link below to send it.", NotesLogic.sharedNotice(" @Bob"))
        assertEq("bob can now edit", NotesLogic.roleNotice("bob", "editor"))
        assertEq("Can view", NotesLogic.roleLabel("viewer"))
    }

    fun testFormatExportAndSaveState() {
        assertEq(NoteEdit("say **hi** there", 6, 8), NotesLogic.format("say hi there", 4, 6, "bold"))
        assertEq(NoteEdit("**bold text**", 2, 11), NotesLogic.format("", 0, 0, "bold"))
        assertEq(NoteEdit("a\n## b", 5, 5), NotesLogic.format("a\nb", 2, 2, "heading"))
        assertEq(NoteEdit("- [ ] x", 6, 7), NotesLogic.format("x", 0, 1, "task"))
        assertEq(NoteEdit("[go](https://)", 1, 3), NotesLogic.format("go", 0, 2, "link"))
        assertEq(NoteEdit("ab", 1, 1), NotesLogic.format("ab", 1, 1, "unknown"))
        assertEq("my-plan-v2.md", NotesLogic.exportFileName("  My plan: v2! "))
        assertEq("note.md", NotesLogic.exportFileName("!!!"))
        assertEq("# Untitled note\n\nbody", NotesLogic.exportMarkdown(" ", "body"))
        assertEq("Not saved \u2014 resolve the conflict", NotesLogic.saveStateLabel(NoteSaveState.Conflict))
        assertEq("Boom", NotesLogic.saveStateLabel(NoteSaveState.Failed, "Boom"))
        assertEq("Paste an issue or pull request URL, or type owner/repo#123.", NotesLogic.linkError(OrgApiException("invalid_link", "")))
    }

    fun testPresenceFrames() {
        val presence = NotePresence("me")
        assertEq("Offline \u2014 edits still save", presence.label(false, 0))
        assertEq("Live", presence.label(true, 0))
        val hello = presence.receive("""{"type":"presence","clientId":"p1","name":"ana"}""", 1_000)
        assertEq(true, hello.newcomer)
        assertEq(null, hello.event)
        assertEq(false, presence.receive("""{"type":"cursor","clientId":"p1","name":"ana"}""", 2_000).newcomer)
        assertEq("Live \u00b7 ana is here", presence.label(true, 2_000))
        assertEq(false, presence.receive("""{"type":"presence","clientId":"me"}""", 2_000).newcomer)
        val draft = presence.receive("""{"type":"draft","clientId":"p2","name":"bo","title":"T","markdown":"M","baseVersion":4}""", 3_000)
        assertEq(NoteRoomEvent.Draft("T", "M", 4), draft.event)
        assertEq("bo is editing\u2026", presence.label(true, 3_000))
        assertEq("Live \u00b7 2 others here", presence.label(true, 9_000))
        assertEq(NoteRoomEvent.Updated(5), presence.receive("""{"type":"note-updated","version":5}""", 9_000).event)
        assertEq(NoteRoomEvent.Deleted, presence.receive("""{"type":"note-deleted"}""", 9_000).event)
        assertEq(null, presence.receive("not json", 9_000).event)
        assertEq(0, presence.peers(60_000).size)
        val frame = jsonObject(com.forkmesh.nativeapp.json.Json.parse(presence.draftFrame("T", "M", 3)))
        assertEq("draft", frame["type"])
        assertEq(3L, jsonInt(frame["baseVersion"]))
    }

    fun testConflictLinksAndSuggestionRequests() {
        val calls = mutableListOf<String>()
        val api = object : ApiService(TestSettings()) {
            override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
                calls.add("$method $path ${body ?: ""}")
                if (method == "PATCH") throw OrgApiException("version_conflict", "version conflict")
                return mapOf(
                    "note" to mapOf("id" to "n1", "version" to 9L),
                    "links" to listOf(mapOf("owner" to "acme", "repo" to "web", "kind" to "issue", "number" to 2L)),
                    "suggestions" to listOf(mapOf("type" to "user", "name" to "ann")),
                )
            }
        }
        val notes = NotesApi(api)
        val conflict = try {
            notes.save("n1", 4, "T", "body")
            null
        } catch (error: NoteConflictException) {
            error
        }
        assertEq(9L, conflict?.latest?.get("version"))
        assertEq("GET /api/notes/n1 ", calls[1])
        assertEq(listOf(NoteShareSuggestion("user", "ann")), notes.shareSuggestions("n1", " @An n"))
        assertEq("GET /api/notes/n1/shares?query=an+n ", calls[2])
        assertEq(emptyList<Any>(), notes.shareSuggestions("n1", "  "))
        assertEq(3, calls.size)
        val link = NoteLink("acme", "web", "issue", 2)
        assertEq(listOf(link), notes.addLink("n1", link))
        assertEq("POST /api/notes/n1/links {owner=acme, repo=web, kind=issue, number=2}", calls[3])
        notes.removeLink("n1", link)
        assertEq("DELETE /api/notes/n1/links {owner=acme, repo=web, kind=issue, number=2}", calls[4])
        notes.addShare("n1", "organization", "Crew", "editor")
        assertEq("POST /api/notes/n1/shares {type=organization, name=crew, role=editor}", calls[5])
        notes.search(" Milk & Eggs ")
        assertEq("GET /api/notes?query=milk+%26+eggs ", calls[6])
        notes.search("   ")
        assertEq(7, calls.size)
    }

    private fun expectStartsWith(prefix: String, actual: String) {
        if (!actual.startsWith(prefix)) throw AssertionError("expected <$actual> to start with <$prefix>")
    }

    private fun expectEndsWith(suffix: String, actual: String) {
        if (!actual.endsWith(suffix)) throw AssertionError("expected <$actual> to end with <$suffix>")
    }
}
