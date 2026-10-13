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

    private fun expectStartsWith(prefix: String, actual: String) {
        if (!actual.startsWith(prefix)) throw AssertionError("expected <$actual> to start with <$prefix>")
    }

    private fun expectEndsWith(suffix: String, actual: String) {
        if (!actual.endsWith(suffix)) throw AssertionError("expected <$actual> to end with <$suffix>")
    }
}
