package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.testing.assertEq
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.util.Locale

class ChatListPreviewTest {
    private fun message(id: String, text: String, ts: Long, self: Boolean = false, fileName: String = "", sender: String = "Pat") =
        ChatMessage(id, "#general", if (self) "me-node" else "peer-1", sender, text, ts, self = self, fileName = fileName)

    fun testMarkdownIsReducedToOnePlainLine() {
        assertEq("bold and code here", ChatListPreviews.plain("**bold** and `code`\n\n  here"))
        assertEq("see the docs now", ChatListPreviews.plain("see [the docs](https://forkmesh.com/docs) now"))
        assertEq("Title item one quoted", ChatListPreviews.plain("# Title\n- item one\n> quoted"))
        assertEq("let x = 1", ChatListPreviews.plain("```kotlin\nlet x = 1\n```"))
        assertEq("snake_case stays", ChatListPreviews.plain("snake_case stays"))
        val long = ChatListPreviews.plain("a".repeat(400))
        assertEq(ChatListPreviews.MAX_LINE_CHARS, long.length)
        assertEq('…', long.last())
    }

    fun testLinesNameTheSenderOrYouAndFallBackToTheFile() {
        assertEq("Pat: hello", ChatListPreviews.line(message("1", "hello", 1), "hello", direct = false))
        assertEq("hello", ChatListPreviews.line(message("1", "hello", 1), "hello", direct = true))
        assertEq("You: hi", ChatListPreviews.line(message("2", "hi", 1, self = true), "hi", direct = true))
        assertEq("You: hi", ChatListPreviews.line(message("2", "hi", 1, self = true), "hi", direct = false))
        assertEq("Someone: x", ChatListPreviews.line(message("3", "x", 1, sender = " "), "x", direct = false))
        assertEq("📎 plan.pdf", ChatListPreviews.body("", "plan.pdf"))
        assertEq("caption", ChatListPreviews.body("caption", "plan.pdf"))
        assertEq("", ChatListPreviews.body("  ", ""))
    }

    fun testNewestSkipsEphemeralHiddenAndEmptyMessages() {
        val list = listOf(
            message("a", "first", 10),
            message("b", "second", 20),
            message("c", "", 30),
            message("d", "notice", 40).copy(ephemeral = true),
            message("e", "hidden", 50),
        )
        val preview = ChatListPreviews.newest(list, direct = false, skip = { it.id == "e" })!!
        assertEq("Pat: second", preview.line)
        assertEq(20L, preview.timestamp)
        assertEq("b", preview.messageId)
        assertEq(null, ChatListPreviews.newest(emptyList(), direct = true))
        val unordered = listOf(message("x", "late", 90), message("y", "early", 5))
        assertEq("x", ChatListPreviews.newest(unordered, direct = true)!!.messageId)
    }

    fun testTimeLabelsAreTodayWeekdayOrDate() {
        val zone = ZoneId.of("UTC")
        val now = LocalDateTime.of(2026, 10, 13, 15, 0).toInstant(ZoneOffset.UTC).toEpochMilli()
        val at = { day: Int, hour: Int, minute: Int -> LocalDateTime.of(2026, 10, day, hour, minute).toInstant(ZoneOffset.UTC).toEpochMilli() }
        assertEq("14:02", ChatListPreviews.timeLabel(at(13, 14, 2), now, zone, Locale.UK))
        assertEq("Sun", ChatListPreviews.timeLabel(at(11, 9, 0), now, zone, Locale.UK))
        assertEq("3 Oct", ChatListPreviews.timeLabel(at(3, 9, 0), now, zone, Locale.UK))
        assertEq("", ChatListPreviews.timeLabel(0, now, zone, Locale.UK))
        assertEq(9000L, ChatListPreviews.recencyMs(9))
        assertEq(1_760_000_000_000L, ChatListPreviews.recencyMs(1_760_000_000_000L))
    }

    fun testChannelPreviewFollowsTopLevelMessagesEditsAndDeletes() {
        val (hub, relay) = hubWith()
        assertEq(null, hub.listPreview("#general"))
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "root", "ts" to 10L), id = "root-1"))
        relay.handle(peerFrame("thread-reply", mapOf("channel" to "#general", "rootId" to "root-1", "text" to "in thread", "ts" to 20L), id = "tr-1"))
        relay.handle(peerFrame("reaction", mapOf("conversation" to "#general", "target" to "root-1", "emoji" to "👍", "reactorId" to "peer-1", "added" to true)))
        assertEq("Pat: root", hub.listPreview("#general")?.line)
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "mine **now**", "ts" to 30L), id = "me-1", sender = "me-node"))
        assertEq("You: mine now", hub.listPreview("#general")?.line)
        relay.handle(peerFrame("edit", mapOf("conversation" to "#general", "target" to "me-1", "text" to "mine, fixed"), id = "ed-1", sender = "me-node"))
        assertEq("You: mine, fixed", hub.listPreview("#general")?.line)
        relay.handle(peerFrame("delete", mapOf("conversation" to "#general", "target" to "me-1"), id = "del-1", sender = "me-node"))
        assertEq("Pat: root", hub.listPreview("#general")?.line)
        relay.handle(peerFrame("chat", mapOf("channel" to "#general", "text" to "", "fileName" to "plan.pdf", "ts" to 40L), id = "file-1"))
        assertEq("Pat: 📎 plan.pdf", hub.listPreview("#general")?.line)
        hub.hideMessage(hub.rows("#general").last().message)
        assertEq("Pat: root", hub.listPreview("#general")?.line)
        assertEq(null, hub.listPreview("#random"))
    }
}
