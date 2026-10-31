package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq

class NoteCommentsTest {
    private fun comment(id: String, quote: String = "", parent: String = "", created: Long = 0, resolved: Long = 0, start: Int = 0, prefix: String = "", suffix: String = "") =
        NoteComment(id, parentId = parent, author = "ana", body = "b", quote = quote, prefix = prefix, suffix = suffix, start = start, end = start + quote.length, createdAt = created, resolvedAt = resolved)

    fun testParsesTheContractShape() {
        val payload = NoteCommentsPayload.from(mapOf(
            "comments" to listOf(
                mapOf(
                    "id" to "c1", "parentId" to null, "author" to "ana", "body" to "hi @bo", "quote" to "fox", "prefix" to "the ", "suffix" to " jumps",
                    "start" to 4.0, "end" to 7.0, "mentions" to listOf("bo"), "createdAt" to 10.0, "updatedAt" to 12.0, "editedAt" to 0,
                    "resolvedAt" to null, "resolvedBy" to "",
                ),
                mapOf("id" to "c2", "parentId" to "c1", "author" to "bo", "body" to "ok", "createdAt" to 11L),
                mapOf("id" to ""),
            ),
            "people" to listOf("ana", "bo", "Bo", ""),
            "canResolve" to true,
            "comment" to mapOf("id" to "c2", "parentId" to "c1"),
        ))
        assertEq(2, payload.comments.size)
        val first = payload.comments[0]
        assertEq(true, first.isThread)
        assertEq(true, first.isInline)
        assertEq(false, first.isResolved)
        assertEq(4, first.start)
        assertEq(7, first.end)
        assertEq(listOf("bo"), first.mentions)
        assertEq(10L, first.createdAt)
        assertEq("c1", payload.comments[1].parentId)
        assertEq(listOf("ana", "bo"), payload.people)
        assertEq(true, payload.canResolve)
        assertEq("c2", payload.comment?.id)
        assertEq(false, NoteCommentsPayload.from(emptyMap()).canResolve)
        assertEq(true, first.isBy(" ANA "))
        assertEq(false, first.isBy(""))
    }

    fun testQuoteTakesThirtyTwoCharactersOfContext() {
        val text = "a".repeat(40) + "QUOTE" + "b".repeat(40)
        val quote = NoteAnchoring.quote(text, 45, 40)!!
        assertEq("QUOTE", quote.quote)
        assertEq("a".repeat(32), quote.prefix)
        assertEq("b".repeat(32), quote.suffix)
        assertEq(40, quote.start)
        assertEq(45, quote.end)
        assertEq(NoteQuote("x", "", "", 0, 1), NoteAnchoring.quote("x", 0, 1))
        assertEq(null, NoteAnchoring.quote("abc", 1, 1))
        assertEq(null, NoteAnchoring.quote("a   b", 1, 4))
        assertEq(1000, NoteAnchoring.quote("z".repeat(1500), 0, 1500)!!.quote.length)
        assertEq(linkedMapOf<String, Any?>("quote" to "x", "prefix" to "", "suffix" to "", "start" to 0, "end" to 1), NoteQuote("x", "", "", 0, 1).body())
    }

    fun testQuoteOffsetsAreUtf16Units() {
        val text = "😀 hi there"
        val quote = NoteAnchoring.quote(text, 3, 5)!!
        assertEq("hi", quote.quote)
        assertEq("😀 ", quote.prefix)
        assertEq(3..4, NoteAnchoring.locate(text, "hi", "😀 ", " there", 3))
    }

    fun testLocateScoresContextThenDistance() {
        val text = "one cat. two cat. three cat."
        assertEq(13..15, NoteAnchoring.locate(text, "cat", "two ", ". three", 0))
        assertEq(13..15, NoteAnchoring.locate(text, "cat", "", ". three", 0))
        assertEq(4..6, NoteAnchoring.locate(text, "cat", "zzz", "zzz", 0))
        assertEq(24..26, NoteAnchoring.locate(text, "cat", "zzz", "zzz", 30))
        assertEq(13..15, NoteAnchoring.locate(text, "cat", "zzz", "zzz", 14))
        assertEq(null, NoteAnchoring.locate(text, "dog", "", "", 0))
        assertEq(null, NoteAnchoring.locate(text, "", "", "", 0))
        val moved = "Intro added.\n" + text
        assertEq(26..28, NoteAnchoring.locate(moved, "cat", "two ", ". three", 13))
        assertEq(1..2, NoteAnchoring.locate("aaa", "aa", "a", "", 0))
        assertEq(0..2, NoteAnchoring.locate("cat", "cat", "longer prefix than the start", "", 0))
    }

    fun testThreadsSortInDocumentOrder() {
        val markdown = "alpha beta gamma"
        val comments = listOf(
            comment("gamma", "gamma", created = 1, start = 11),
            comment("gone", "deleted words", created = 2),
            comment("whole", "", created = 5),
            comment("alpha", "alpha", created = 9),
            comment("done", "beta", created = 3, resolved = 20, start = 6),
            comment("r2", parent = "alpha", created = 12),
            comment("r1", parent = "alpha", created = 11),
            comment("orphan", parent = "missing", created = 1),
        )
        val list = NoteThreads.build(comments, markdown)
        assertEq(listOf("whole", "alpha", "gamma", "gone"), list.open.map { it.id })
        assertEq(listOf("done"), list.resolved.map { it.id })
        assertEq(listOf("r1", "r2"), list.open[1].replies.map { it.id })
        assertEq(0..4, list.open[1].anchor)
        assertEq(true, list.open[3].isDetached)
        assertEq(true, list.open[0].isWholeNote)
        assertEq(false, list.open[0].isDetached)
        assertEq(4, list.openCount)
        assertEq(4, NoteThreads.openCount(comments))
        assertEq("Resolved (1)", list.resolvedLabel)
        assertEq("alpha", list.find("r2")?.id)
        assertEq("done", list.find("done")?.id)
        assertEq(null, list.find("orphan"))
        assertEq("Comments (3)", NoteThreads.buttonLabel(3))
        assertEq("Comments", NoteThreads.buttonLabel(0))
        assertEq("Removed from the note", NoteThreads.quoteLabel(list.open[3]))
        assertEq("On the whole note", NoteThreads.quoteLabel(list.open[0]))
        assertEq("", NoteThreads.quoteLabel(list.open[1]))
    }

    fun testResolvePermission() {
        val thread = NoteThread(comment("t"), emptyList(), null)
        assertEq(true, thread.canResolve(true, "bo"))
        assertEq(true, thread.canResolve(false, "ana"))
        assertEq(false, thread.canResolve(false, "bo"))
        val replied = NoteThread(comment("t"), listOf(comment("r", parent = "t").copy(author = "bo"), comment("r2", parent = "t")), null)
        assertEq(listOf("ana", "bo"), replied.participants)
    }

    fun testOwnerCanDeleteAnyComment() {
        val mine = comment("c")
        val theirs = comment("d").copy(author = "bo")
        assertEq(true, mine.canEdit("ana"))
        assertEq(false, theirs.canEdit("ana"))
        assertEq(true, mine.canDelete("ana", isOwner = false))
        assertEq(false, theirs.canDelete("ana", isOwner = false))
        assertEq(true, theirs.canDelete("ana", isOwner = true))
        assertEq(false, theirs.canEdit("ana"))
        assertEq("Delete thread?", NoteCommentText.deleteTitle(mine))
        assertEq("Delete comment?", NoteCommentText.deleteTitle(comment("r", parent = "c")))
        assertEq("This thread will be removed for everyone.", NoteCommentText.deletePrompt(mine, 0, "ana"))
        assertEq("This thread and its reply will be removed for everyone.", NoteCommentText.deletePrompt(mine, 1, "ana"))
        assertEq("This thread and its 3 replies will be removed for everyone.", NoteCommentText.deletePrompt(mine, 3, "ana"))
        assertEq("bo's thread and its 2 replies will be removed for everyone.", NoteCommentText.deletePrompt(theirs, 2, "ana"))
        assertEq("This comment will be removed for everyone.", NoteCommentText.deletePrompt(comment("r", parent = "c"), 0, "ana"))
        assertEq("bo's comment will be removed for everyone.", NoteCommentText.deletePrompt(comment("r", parent = "c").copy(author = "bo"), 0, "ana"))
    }

    fun testMentions() {
        val body = "hey @bo and @Cy, mail bo@x.com or @@zed and @nobody"
        val ranges = NoteMentions.ranges(body, listOf("bo", "cy", "zed"))
        assertEq(listOf("@bo", "@Cy"), ranges.map { body.substring(it) })
        assertEq(emptyList<IntRange>(), NoteMentions.ranges(body, emptyList()))
        assertEq(NoteMentionQuery(6, 9, "an"), NoteMentions.active("hello @an", 9))
        assertEq(NoteMentionQuery(0, 1, ""), NoteMentions.active("@", 1))
        assertEq(null, NoteMentions.active("mail a@b", 8))
        assertEq(null, NoteMentions.active("hello @an there", 15))
        assertEq(NoteMentionQuery(6, 9, "an"), NoteMentions.active("hello @an there", 9))
        val people = listOf("ana", "Bo", "dana", "me", "anabel")
        assertEq(listOf("ana", "anabel", "dana"), NoteMentions.suggestions(people, "an", "me"))
        assertEq(listOf("ana", "Bo", "dana", "anabel"), NoteMentions.suggestions(people, "", "ME"))
        assertEq(NoteEdit("hi @ana ", 8, 8), NoteMentions.insert("hi @an", NoteMentionQuery(3, 6, "an"), "ana"))
        assertEq(NoteEdit("hi @ana there", 8, 8), NoteMentions.insert("hi @an there", NoteMentionQuery(3, 6, "an"), "ana"))
    }

    fun testCommentTextAndErrors() {
        assertEq(false, NoteCommentText.canSend("   "))
        assertEq(true, NoteCommentText.canSend(" ok "))
        assertEq(false, NoteCommentText.canSend("x".repeat(4001)))
        assertEq("Write a comment of up to 4000 characters.", NoteCommentText.error(OrgApiException("invalid_comment", "")))
        assertEq("This note has reached its limit of 1000 comments.", NoteCommentText.error(OrgApiException("too_many_comments", "")))
        assertEq("The comment was not saved.", NoteCommentText.error(IllegalStateException()))
        val comment = comment("c", created = 1_700_000_000_000L).copy(editedAt = 1_700_000_000_500L)
        assertEq("just now · edited", comment.meta(1_700_000_001_000L))
    }

    fun testCommentRequests() {
        val calls = mutableListOf<String>()
        val api = object : ApiService(TestSettings()) {
            override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
                calls.add("$method $path ${body ?: ""}")
                return mapOf("comments" to listOf(mapOf("id" to "c1")), "people" to listOf("ana"), "canResolve" to false, "comment" to mapOf("id" to "c1"))
            }
        }
        val notes = NotesApi(api)
        assertEq(listOf("c1"), notes.comments("n1").comments.map { it.id })
        notes.addComment("n1", " hi ", quote = NoteQuote("fox", "the ", " ran", 4, 7))
        notes.addComment("n1", "reply", parentId = "c1", quote = NoteQuote("fox", "", "", 0, 3))
        notes.addComment("n1", "whole")
        notes.editComment("n1", "c1", " new ")
        notes.resolveComment("n1", "c1", true)
        assertEq("c1", notes.deleteComment("n1", "c1").comment?.id)
        assertEq("GET /api/notes/n1/comments ", calls[0])
        assertEq("POST /api/notes/n1/comments {body=hi, quote=fox, prefix=the , suffix= ran, start=4, end=7}", calls[1])
        assertEq("POST /api/notes/n1/comments {body=reply, parentId=c1}", calls[2])
        assertEq("POST /api/notes/n1/comments {body=whole}", calls[3])
        assertEq("PATCH /api/notes/n1/comments/c1 {body=new}", calls[4])
        assertEq("PATCH /api/notes/n1/comments/c1 {resolved=true}", calls[5])
        assertEq("DELETE /api/notes/n1/comments/c1 ", calls[6])
    }

    fun testPresenceStatesAndLeave() {
        val presence = NotePresence("me")
        val hello = jsonObject(Json.parse(presence.presenceFrame()))
        assertEq("viewing", hello["state"])
        assertEq(null, presence.stateChangeFrame("viewing"))
        val editing = presence.stateChangeFrame("editing")
        assertEq("editing", jsonObject(Json.parse(editing!!))["state"])
        assertEq(null, presence.stateChangeFrame("editing"))
        assertEq("viewing", jsonObject(Json.parse(presence.stateChangeFrame("viewing")!!))["state"])
        assertEq("viewing", jsonObject(Json.parse(presence.presenceFrame("bogus")))["state"])

        presence.receive("""{"type":"presence","clientId":"p1","name":"ana","state":"viewing"}""", 1_000)
        assertEq("Live · ana is viewing", presence.label(true, 1_000))
        presence.receive("""{"type":"presence","clientId":"p2","name":"bo","state":"editing"}""", 1_000)
        assertEq("bo is editing…", presence.label(true, 1_000))
        assertEq(listOf("bo", "ana"), presence.peers(1_000).map { it.name })
        assertEq("bo · editing", presence.peers(1_000)[0].line(1_000))
        assertEq("ana · viewing", presence.peers(1_000)[1].line(1_000))
        presence.receive("""{"type":"cursor","clientId":"p2","name":"bo"}""", 2_000)
        assertEq("editing", presence.peers(2_000)[0].state)
        presence.receive("""{"type":"presence","clientId":"p2","name":"bo","state":"viewing"}""", 3_000)
        assertEq("Live · 2 others here", presence.label(true, 3_000))
        val left = presence.receive("""{"type":"leave","clientId":"p2","name":"bo"}""", 3_000)
        assertEq(NoteRoomEvent.Left("p2"), left.event)
        assertEq(listOf("ana"), presence.peers(3_000).map { it.name })
        assertEq(null, presence.receive("""{"type":"leave","clientId":"me"}""", 3_000).event)
        assertEq(NoteRoomEvent.Comments, presence.receive("""{"type":"note-comments"}""", 3_000).event)
        assertEq(true, presence.receive("""{"type":"presence","clientId":"p2","name":"bo"}""", 4_000).newcomer)
    }

    fun testLocalStateAndFaces() {
        assertEq("viewing", NotePresence.localState(0, 5_000))
        assertEq("editing", NotePresence.localState(1_000, 10_999))
        assertEq("viewing", NotePresence.localState(1_000, 11_000))
        assertEq(4_000L, NotePresence.msUntilViewing(1_000, 7_000))
        assertEq(0L, NotePresence.msUntilViewing(1_000, 20_000))
        val peers = (1..6).map { NotePeer("p$it", 0, clientId = "c$it") }
        val faces = NotePresence.faces(peers)
        assertEq(listOf("p1", "p2", "p3", "p4"), faces.shown.map { it.name })
        assertEq(2, faces.extra)
        assertEq("+2", faces.extraLabel)
        assertEq("", NotePresence.faces(peers.take(3)).extraLabel)
        assertEq("Live", NotePresence.label(true, emptyList(), 0))
        assertEq(true, NotePeer("a", 0, draftAt = 1_000).isEditing(2_000))
        assertEq(false, NotePeer("a", 0, draftAt = 1_000).isEditing(7_000))
    }
}
