package com.forkmesh.nativeapp.notes

import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.TestSettings
import com.forkmesh.nativeapp.docs.DocsMarkdown
import com.forkmesh.nativeapp.model.OrgApiException
import com.forkmesh.nativeapp.testing.assertEq

class NoteReactionsTest {
    private val markdown = "alpha beta gamma beta"

    private fun reaction(id: String, author: String = "ana", emoji: String = "👍", quote: String = "", prefix: String = "", suffix: String = "", start: Int = 0): Map<String, Any?> =
        mapOf("id" to id, "author" to author, "emoji" to emoji, "quote" to quote, "prefix" to prefix, "suffix" to suffix, "start" to start.toDouble(), "end" to (start + quote.length).toDouble(), "createdAt" to 1.0)

    private val sample = listOf(
        reaction("r1", "bob", "👍", "beta", "alpha ", " gamma", 6),
        reaction("r2", "alice", "❤️"),
        reaction("r3", "Alice", "👍", "beta", "alpha ", "", 6),
        reaction("r4", "cy", "🎉", "beta", "gamma ", "", 17),
        reaction("r5", "bob", "❤️", "beta", "alpha ", " gamma", 6),
        reaction("r6", "dee", "👍", "delta", "", "", 0),
        reaction("r7", "bob", "👍"),
        reaction("", "bob", "👍"),
        reaction("r9", "bob", ""),
    )

    fun testDecodesAndGroupsByResolvedPassage() {
        val reactions = NoteCommentsPayload.from(mapOf("comments" to emptyList<Any>(), "reactions" to sample)).reactions
        assertEq(listOf("r1", "r2", "r3", "r4", "r5", "r6", "r7"), reactions.map { it.id })
        assertEq(NoteReaction("r1", "bob", "👍", "beta", "alpha ", " gamma", 6, 10, 1L), reactions[0])
        assertEq(true, reactions[1].isWhole)
        assertEq(null, reactions[1].anchor)
        assertEq(emptyList<NoteReaction>(), NoteCommentsPayload.from(emptyMap()).reactions)

        val groups = NoteReactions.groups(reactions, markdown, "Alice")
        assertEq(listOf("whole", "6-10", "17-21"), groups.map { it.id })
        assertEq(true, groups[0].isWhole)
        assertEq(listOf(NoteReactionChip("❤️", listOf("alice"), listOf("r2")), NoteReactionChip("👍", listOf("bob"), emptyList())), groups[0].chips)
        assertEq(6..9, groups[1].range)
        assertEq("beta", groups[1].quote)
        assertEq(NoteQuote("beta", "alpha ", " gamma", 6, 10), groups[1].anchor)
        assertEq(listOf("👍", "❤️"), groups[1].chips.map { it.emoji })
        assertEq(2, groups[1].chips[0].count)
        assertEq(true, groups[1].chips[0].reacted)
        assertEq(false, groups[1].chips[1].reacted)
        assertEq("bob, Alice reacted with 👍", groups[1].chips[0].label)
        assertEq("👍 2  ❤️ 1", groups[1].summary)
        assertEq("bob, Alice reacted with 👍. bob reacted with ❤️", groups[1].label)
        assertEq(listOf(NoteReactionChip("🎉", listOf("cy"), emptyList())), groups[2].chips)
        assertEq(emptyList<String>(), NoteReactions.groups(reactions, markdown, "").flatMap { group -> group.chips.flatMap { it.mine } })
        assertEq(emptyList<NoteReactionGroup>(), NoteReactions.groups(emptyList(), markdown, "alice"))
        assertEq(listOf("👍", "❤️", "😂", "🎉", "😮", "👀"), NoteReactions.quickSet.map { it.emoji })
    }

    fun testChipsToggleWithTheGroupsFirstAnchor() {
        val groups = NoteReactions.groups(NoteReaction.list(sample), markdown, "alice")
        val anchor = NoteQuote("beta", "alpha ", " gamma", 6, 10)
        assertEq(NoteReactionAction.Remove(listOf("r3")), NoteReactions.action(groups[1].chips[0], groups[1]))
        assertEq(NoteReactionAction.Add("❤️", anchor), NoteReactions.action(groups[1].chips[1], groups[1]))
        assertEq(NoteReactionAction.Add("👍", null), NoteReactions.action(groups[0].chips[1], groups[0]))
        assertEq(null, NoteReactions.adding("👍", groups[1]))
        assertEq(NoteReactionAction.Add("😮", anchor), NoteReactions.adding("😮", groups[1]))
        assertEq(linkedMapOf<String, Any?>("emoji" to "👍"), NoteReactions.body("👍", null))
        assertEq(
            linkedMapOf<String, Any?>("emoji" to "😮", "quote" to "beta", "prefix" to "alpha ", "suffix" to " gamma", "start" to 6, "end" to 10),
            NoteReactions.body("😮", anchor),
        )
    }

    fun testPreviewPlacesEachGroupUnderItsBlock() {
        val text = "# Title\n\nFirst paragraph that is\nwrapped over lines.\n\n- item one\n- item two\n\nTitle again"
        val probes = listOf("Title", "First paragraph that is wrapped over lines.", "item one", null, "missing words", "Title again")
        val starts = NoteReactions.blockStarts(probes, text)
        assertEq(listOf(2, 9, text.indexOf("item one"), null, null, text.indexOf("Title again")), starts)

        val parsed = DocsMarkdown.blocks(text).map(NoteReactions::probe)
        val found = NoteReactions.blockStarts(parsed, text)
        assertEq(parsed.size, found.size)
        assertEq(listOf(2, 9, text.indexOf("item one"), text.indexOf("item two"), text.indexOf("Title again")), found.filterNotNull())

        fun group(from: Int, to: Int) = NoteReactionGroup(from until to, null, emptyList())
        val wrapped = text.indexOf("wrapped")
        val again = text.indexOf("again")
        val placed = NoteReactions.placed(listOf(NoteReactionGroup(null, null, emptyList()), group(0, 1), group(wrapped, wrapped + 7), group(again, again + 5)), starts)
        assertEq(listOf(listOf("0-1"), listOf("$wrapped-${wrapped + 7}"), emptyList(), emptyList(), emptyList(), listOf("$again-${again + 5}")), placed.map { list -> list.map { it.id } })
        assertEq(listOf(0, 0), NoteReactions.placed(listOf(group(0, 1)), listOf(null, null)).map { it.size })
    }

    fun testReactionRequestsAndErrors() {
        val calls = mutableListOf<String>()
        val api = object : ApiService(TestSettings()) {
            override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
                calls.add("$method $path ${body ?: ""}")
                return mapOf("comments" to emptyList<Any>(), "reactions" to listOf(reaction("r1", emoji = "👍")))
            }
        }
        val notes = NotesApi(api)
        assertEq(listOf("r1"), notes.comments("n1").reactions.map { it.id })
        notes.addReaction("n1", "👍", null)
        notes.addReaction("n1", "🎉", NoteQuote("fox", "the ", " ran", 4, 7))
        assertEq(listOf("r1"), notes.deleteReaction("n1", "r1").reactions.map { it.id })
        notes.perform("n1", NoteReactionAction.Remove(listOf("a", "b")))
        notes.perform("n1", NoteReactionAction.Add("😮", null))
        notes.perform("n1", NoteReactionAction.Remove(emptyList()))
        assertEq(
            listOf(
                "GET /api/notes/n1/comments ",
                "POST /api/notes/n1/reactions {emoji=👍}",
                "POST /api/notes/n1/reactions {emoji=🎉, quote=fox, prefix=the , suffix= ran, start=4, end=7}",
                "DELETE /api/notes/n1/reactions/r1 ",
                "DELETE /api/notes/n1/reactions/a ",
                "DELETE /api/notes/n1/reactions/b ",
                "POST /api/notes/n1/reactions {emoji=😮}",
                "GET /api/notes/n1/comments ",
            ),
            calls,
        )
        assertEq("This note already has 2000 reactions, the most it can hold.", NoteCommentText.error(OrgApiException("too_many_reactions", ""), "x"))
        assertEq("React with a single emoji.", NoteCommentText.error(OrgApiException("invalid_reaction", ""), "x"))
        assertEq("The reaction was not added.", NoteCommentText.error(IllegalStateException(), "The reaction was not added."))
    }
}
