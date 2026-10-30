package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
import com.forkmesh.nativeapp.testing.assertEq
import java.time.ZoneId
import java.time.ZonedDateTime

class ChatConversationLayoutTest {
    private val zone = ZoneId.of("UTC")
    private val base = ZonedDateTime.of(2026, 10, 20, 15, 0, 0, 0, zone).toInstant().toEpochMilli()

    private fun row(id: String, sender: String, at: Long, text: String = "hi", self: Boolean = false, receipt: ChatReceipt? = null) =
        ChatRow(
            ChatMessage(id, "direct:x", sender, sender, text, at, self = self),
            continuesGroup = false,
            deletable = false,
            receipt = receipt,
        )

    private fun bubbles(items: List<ChatConversationItem>) = items.filterIsInstance<ChatConversationItem.Message>().map { it.bubble }

    fun testRunsJoinOnOneSenderAndBreakOnGapsAndSenders() {
        val rows = listOf(
            row("a", "pat", base),
            row("b", "pat", base + 60_000),
            row("c", "pat", base + 120_000),
            row("d", "me", base + 180_000, self = true),
            row("e", "me", base + 180_000 + ChatConversationLayout.TIME_GAP_MS, self = true, receipt = ChatReceipt.READ),
        )
        val items = ChatConversationLayout.items(rows, group = false, zone = zone)
        assertEq(listOf("time-a", "a", "b", "c", "d", "time-e", "e"), items.map { it.id })
        val shown = bubbles(items)
        assertEq(
            listOf(ChatBubblePosition.FIRST, ChatBubblePosition.MIDDLE, ChatBubblePosition.LAST, ChatBubblePosition.SINGLE, ChatBubblePosition.SINGLE),
            shown.map { it.position },
        )
        assertEq(listOf(null, null, null, ChatReceipt.SENT, ChatReceipt.READ), shown.map { it.receipt })
        assertEq(listOf(false, true, true, false, false), shown.map { it.joinsAbove })
        assertEq(listOf(true, true, false, false, false), shown.map { it.joinsBelow })
    }

    fun testGroupsNameTheSenderAtTheStartOfARun() {
        val rows = listOf(row("a", "pat", base), row("b", "pat", base + 1_000), row("c", "sam", base + 2_000), row("d", "me", base + 3_000, self = true))
        val shown = bubbles(ChatConversationLayout.items(rows, group = true, zone = zone))
        assertEq(listOf(true, false, true, false), shown.map { it.showsSender })
        assertEq(listOf(false, false, false, false), bubbles(ChatConversationLayout.items(rows, group = false, zone = zone)).map { it.showsSender })
    }

    fun testSystemLinesBreakRuns() {
        val notice = ChatRow(
            ChatMessage("system-k", "direct:x", DirectMessageFrames.SYSTEM_SENDER, "ForkMesh", "New device", base + 1_000, ephemeral = true),
            continuesGroup = false,
            deletable = false,
        )
        val items = ChatConversationLayout.items(listOf(row("a", "pat", base), notice, row("b", "pat", base + 2_000)), group = false, zone = zone)
        assertEq(listOf("time-a", "a", "system-k", "b"), items.map { it.id })
        assertEq(listOf(ChatBubblePosition.SINGLE, ChatBubblePosition.SINGLE), bubbles(items).map { it.position })
    }

    fun testOnlyShortEmojiMessagesAreJumbo() {
        assertEq(true, ChatConversationLayout.isJumboEmoji("😀"))
        assertEq(true, ChatConversationLayout.isJumboEmoji(" 😀 👍🏽 "))
        assertEq(true, ChatConversationLayout.isJumboEmoji("❤️"))
        assertEq(true, ChatConversationLayout.isJumboEmoji("👨‍👩‍👧"))
        assertEq(false, ChatConversationLayout.isJumboEmoji("❤"))
        assertEq(false, ChatConversationLayout.isJumboEmoji("😀😀😀😀"))
        assertEq(false, ChatConversationLayout.isJumboEmoji("ok 😀"))
        assertEq(false, ChatConversationLayout.isJumboEmoji("1️⃣"))
        assertEq(false, ChatConversationLayout.isJumboEmoji(""))
    }

    fun testTimeLabelsReadLikeMessages() {
        val now = base + 3_600_000
        assertEq("Today 3:00 PM", ChatConversationLayout.timeLabel(base, now, zone))
        assertEq("Yesterday 3:00 PM", ChatConversationLayout.timeLabel(base - 86_400_000, now, zone))
        assertEq("Sunday 3:00 PM", ChatConversationLayout.timeLabel(base - 2 * 86_400_000, now, zone))
        assertEq("Oct 1 at 3:00 PM", ChatConversationLayout.timeLabel(base - 19 * 86_400_000L, now, zone))
        assertEq("Oct 20, 2025 at 3:00 PM", ChatConversationLayout.timeLabel(base - 365 * 86_400_000L, now, zone))
    }

    fun testExpiryReadsInWholeUnits() {
        assertEq("1 minute", ChatConversationLayout.expiryRemaining(10.0))
        assertEq("5 minutes", ChatConversationLayout.expiryRemaining(290.0))
        assertEq("5 hours", ChatConversationLayout.expiryRemaining(5 * 3600.0))
        assertEq("3 days", ChatConversationLayout.expiryRemaining(72 * 3600.0))
    }

    fun testPrivateRoomsAndDirectMessagesAreConversations() {
        val hub = ChatHub(FakeChatRelay())
        assertEq(true, hub.isConversation("direct:${"d".repeat(32)}"))
        assertEq(true, hub.isConversation("@node-1"))
        assertEq(false, hub.isConversation("#general"))
        assertEq(false, hub.isGroupConversation("direct:${"d".repeat(32)}"))
    }
}
