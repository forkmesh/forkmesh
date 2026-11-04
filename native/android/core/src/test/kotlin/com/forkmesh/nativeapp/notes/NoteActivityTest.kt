package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq

class NoteActivityTest {
    private val minute = 60_000L

    fun testParsesEventsAndCounts() {
        val payload = NoteActivityPayload.from(mapOf(
            "ok" to true,
            "counts" to mapOf("views" to 7.0, "readers" to 2, "versions" to 4, "actions" to 1, "webViews" to 0),
            "events" to listOf(
                mapOf("kind" to "view", "actor" to "ana", "at" to 50 * minute, "firstAt" to 40 * minute, "count" to 3.0),
                mapOf("kind" to "share", "actor" to "me", "at" to 30 * minute, "detail" to mapOf("type" to "user", "name" to "bo", "role" to "editor")),
                mapOf("kind" to ""),
                mapOf("kind" to "create", "actor" to "me", "at" to 0L, "version" to 1),
            ),
        ))
        assertEq(listOf("view", "share", "create"), payload.entries.map { it.kind })
        assertEq(3L, payload.entries[0].count)
        assertEq("ana viewed it 3 times", payload.entries[0].line("me"))
        assertEq("You shared it with bo · can edit", payload.entries[1].line("ME"))
        assertEq("You created the note", payload.entries[2].line("me"))
        assertEq("7 views · 4 versions · 1 action", payload.summary)
        assertEq(emptyList<NoteActivityEntry>(), NoteActivityPayload.from(emptyMap()).entries)
    }

    fun testFoldsEditsAndAddsWebReads() {
        val entries = NoteActivity.entries(mapOf(
            "counts" to mapOf("webViews" to 5, "webReaders" to 3, "webLastViewedAt" to 200 * minute),
            "events" to listOf(
                mapOf("kind" to "edit", "actor" to "ana", "at" to 100 * minute, "version" to 6),
                mapOf("kind" to "edit", "actor" to "ana", "at" to 80 * minute, "version" to 5),
                mapOf("kind" to "edit", "actor" to "ana", "at" to 60 * minute, "version" to 4),
                mapOf("kind" to "edit", "actor" to "ana", "at" to 10 * minute, "version" to 3),
                mapOf("kind" to "edit", "actor" to "bo", "at" to 5 * minute, "version" to 2),
            ),
        ))
        assertEq(listOf("web", "edit", "edit", "edit"), entries.map { it.kind })
        assertEq("5 reads of the published page by 3 readers", entries[0].line("me"))
        assertEq("", entries[0].actorLabel("me"))
        assertEq("ana edited it · v4–v6, 3 saves", entries[1].line("me"))
        assertEq("ana edited it · v3", entries[2].line("me"))
        assertEq("bo edited it · v2", entries[3].line("me"))
    }

    fun testWordingMatchesTheWeb() {
        fun text(kind: String, detail: JsonObject = emptyMap(), count: Long = 1) = NoteActivityEntry(kind, "ana", count = count, detail = detail).text
        assertEq("viewed it", text("view"))
        assertEq("restored version 3 as v9", text("restore", mapOf("version" to 3, "as" to 9)))
        assertEq("restored version 3", text("restore", mapOf("version" to 3)))
        assertEq("changed bo to can view", text("role", mapOf("name" to "bo", "role" to "viewer")))
        assertEq("stopped sharing with acme", text("unshare", mapOf("name" to "acme", "type" to "organization")))
        assertEq("published it to the web", text("publish"))
        assertEq("unpublished it", text("unpublish"))
        assertEq("linked ana/site#12", text("link", mapOf("owner" to "ana", "repo" to "site", "kind" to "issue", "number" to 12)))
        assertEq("unlinked ana/site#12", text("unlink", mapOf("owner" to "ana", "repo" to "site", "number" to 12.0)))
        assertEq("downloaded the Markdown", text("export"))
        assertEq("copied the link", text("copy-link"))
        assertEq("turned it into a task", text("task"))
        assertEq("filed issue ana/site#4 from it", text("issue", mapOf("owner" to "ana", "repo" to "site", "number" to 4)))
        assertEq("filed an issue from it", text("issue"))
        assertEq("commented", text("comment"))
        assertEq("replied to a comment", text("comment", mapOf("reply" to true)))
        assertEq("resolved a comment", text("resolve"))
        assertEq("reopened a comment", text("reopen"))
        assertEq("mystery", text("mystery"))
        assertEq("Someone viewed it", NoteActivityEntry("view").line("me"))
        assertEq("visibility", NoteActivity.icon("view"))
        assertEq("person_add", NoteActivity.icon("share"))
        assertEq("history", NoteActivity.icon("mystery"))
    }

    fun testChatPayloadAndUnread() {
        val payload = NoteChatPayload.from(mapOf(
            "messages" to listOf(
                mapOf("id" to "m2", "author" to "bo", "body" to "second", "createdAt" to 20.0),
                mapOf("id" to "m1", "author" to "ana", "body" to "first", "createdAt" to 10L),
                mapOf("id" to "", "body" to "dropped"),
                mapOf("id" to "m3", "author" to "Me", "body" to "mine", "createdAt" to 30),
            ),
            "canModerate" to true,
            "message" to mapOf("id" to "m3", "author" to "me"),
        ))
        assertEq(listOf("m1", "m2", "m3"), payload.messages.map { it.id })
        assertEq(true, payload.canModerate)
        assertEq("m3", payload.message?.id)
        assertEq(30L, payload.latestAt)
        assertEq(false, NoteChatPayload.from(emptyMap()).canModerate)
        assertEq(0L, NoteChatPayload().latestAt)
        val (first, _, mine) = payload.messages
        assertEq("@ana", first.authorLabel("me"))
        assertEq("You", mine.authorLabel("me"))
        assertEq("Someone", NoteChatMessage("x").authorLabel("me"))
        assertEq(true, mine.canDelete("me", false))
        assertEq(false, first.canDelete("me", false))
        assertEq(true, first.canDelete("me", true))
        assertEq(2, NoteChat.unread(payload.messages, "me", 0))
        assertEq(1, NoteChat.unread(payload.messages, "me", 10))
        assertEq(0, NoteChat.unread(payload.messages, "me", 20))
        assertEq(0, NoteChat.unread(payload.messages, "me", 30))
        assertEq("", NoteChat.badge(0))
        assertEq("3", NoteChat.badge(3))
        assertEq("99+", NoteChat.badge(120))
        assertEq("Chat", NoteChat.buttonLabel(0))
        assertEq("Chat, 1 unread message", NoteChat.buttonLabel(1))
        assertEq("Chat, 4 unread messages", NoteChat.buttonLabel(4))
        assertEq("chatSeen.n1", NoteChat.seenKey("n1"))
    }

    fun testChatTextAndErrors() {
        assertEq(false, NoteChat.canSend("  "))
        assertEq(true, NoteChat.canSend(" hi "))
        assertEq(false, NoteChat.canSend("x".repeat(2001)))
        assertEq(true, NoteChat.canSend("x".repeat(2000)))
        assertEq("This message will be removed for everyone.", NoteChat.deletePrompt(NoteChatMessage("m", "me"), "me"))
        assertEq("bo's message will be removed for everyone.", NoteChat.deletePrompt(NoteChatMessage("m", "bo"), "me"))
        assertEq("Write a message of up to 2000 characters.", NoteChat.error(OrgApiException("invalid_message", "")))
        assertEq("Only the person who sent a message, or the note's owner, can delete it.", NoteChat.error(OrgApiException("message_author_required", "")))
        assertEq("That message was already deleted.", NoteChat.error(OrgApiException("message_not_found", "")))
        assertEq("The message was not sent.", NoteChat.error(IllegalStateException()))
        assertEq("That version is no longer kept.", NoteVersion.error(OrgApiException("version_not_found", "")))
    }

    fun testRequests() {
        val calls = mutableListOf<String>()
        val api = object : ApiService(TestSettings()) {
            override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
                calls.add("$method $path ${body ?: ""}")
                return when {
                    path.contains("/chat") -> mapOf("messages" to listOf(mapOf("id" to "m1", "author" to "ana", "body" to "hi", "createdAt" to 1)), "canModerate" to false)
                    path.contains("/activity") -> mapOf("counts" to mapOf("views" to 1), "events" to listOf(mapOf("kind" to "view", "actor" to "ana", "at" to 1)))
                    else -> mapOf("version" to mapOf("version" to 3, "createdAt" to 9, "author" to "ana", "title" to "Old", "markdown" to "# Old", "emoji" to ""))
                }
            }
        }
        val notes = NotesApi(api)
        assertEq(listOf("m1"), notes.chat("n1").messages.map { it.id })
        notes.sendChat("n1", "  hello  ")
        notes.deleteChat("n1", "m1")
        assertEq(1L, notes.activity("n1").views)
        val old = notes.version("n1", 3)
        assertEq(NoteVersion(3, 9, "ana", "Old", "# Old", ""), old)
        assertEq("GET /api/notes/n1/chat ", calls[0])
        assertEq("POST /api/notes/n1/chat {body=hello}", calls[1])
        assertEq("DELETE /api/notes/n1/chat/m1 ", calls[2])
        assertEq("GET /api/notes/n1/activity ", calls[3])
        assertEq("GET /api/notes/n1/versions?version=3 ", calls[4])
    }

    fun testHistoryText() {
        assertEq(listOf("History", "Activity", "Chat"), NoteLiveTab.entries.map { it.label })
        assertEq("just now · by @ana", NoteHistory.subtitle(1_000, " ana ", 2_000))
        assertEq("just now", NoteHistory.subtitle(1_000, "", 2_000))
        assertEq("Restore version 3?", NoteHistory.restoreTitle(3))
        assertEq("Restored version 3 as a new version", NoteHistory.restored(3))
        assertEq("Version 3 · Current", NoteHistory.viewTitle(3, true))
        assertEq("Version 2", NoteHistory.viewTitle(2, false))
    }

    fun testLiveFrames() {
        val presence = NotePresence("me")
        assertEq(NoteRoomEvent.Activity, presence.receive("""{"type":"note-activity"}""", 0).event)
        assertEq(NoteRoomEvent.Chat, presence.receive("""{"type":"note-chat","actor":"ana"}""", 0).event)
        assertEq(NoteRoomEvent.Updated(7), presence.receive("""{"type":"note-updated","version":7,"updatedAt":1,"actor":"ana"}""", 0).event)
    }
}
