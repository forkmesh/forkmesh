package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.ChatReceipt
import com.forkmesh.nativeapp.model.ChatReceiptTimes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.LocalDateTime
import java.time.ZoneOffset

class ChatMessageInfoTest {
    private val zone = ZoneOffset.UTC
    private val conversation = DirectMessageFrames.PREFIX + "d".repeat(32)

    private fun at(day: Int, hour: Int, minute: Int, second: Int = 0): Long =
        LocalDateTime.of(2026, 10, day, hour, minute, second).toInstant(zone).toEpochMilli()

    private val now = at(19, 23, 30)

    private fun mine(id: String = "m1", ts: Long = at(19, 23, 0), self: Boolean = true, where: String = conversation) =
        ChatMessage(id = id, conversation = where, senderId = "me", senderName = if (self) "Me" else "Pat", text = "hi", timestamp = ts, self = self)

    fun testTheLineUnderTheLastSentMessageNamesTheReceiptAndItsTime() {
        val delivered = ChatReceiptTimes(delivered = at(19, 23, 4))
        assertEq("Delivered 11:04 PM", ChatMessageInfo.receiptStatus(mine(), ChatReceipt.DELIVERED, delivered, now, zone))
        val read = ChatReceiptTimes(delivered = at(19, 23, 4), read = at(18, 15, 20))
        assertEq("Read Yesterday 3:20 PM", ChatMessageInfo.receiptStatus(mine(), ChatReceipt.READ, read, now, zone))
        assertEq("Read Friday 9:05 AM", ChatMessageInfo.receiptStatus(mine(), ChatReceipt.READ, ChatReceiptTimes(read = at(16, 9, 5)), now, zone))
        assertEq("Read", ChatMessageInfo.receiptStatus(mine(), ChatReceipt.READ, ChatReceiptTimes(), now, zone))
        assertEq(null, ChatMessageInfo.receiptStatus(mine(), ChatReceipt.SENT, delivered, now, zone))
        assertEq(null, ChatMessageInfo.receiptStatus(mine(), ChatReceipt.SENDING, ChatReceiptTimes(), now, zone))
        assertEq(null, ChatMessageInfo.receiptStatus(mine(where = "#general"), ChatReceipt.DELIVERED, delivered, now, zone))
    }

    fun testTheLastOwnMessageSkipsPeerAndSystemLines() {
        val rows = listOf(
            ChatRow(mine("a"), continuesGroup = false, deletable = true),
            ChatRow(mine("b"), continuesGroup = false, deletable = true),
            ChatRow(mine("c", self = false), continuesGroup = false, deletable = false),
        )
        assertEq("b", ChatMessageInfo.lastOwnId(rows))
        assertEq(null, ChatMessageInfo.lastOwnId(rows.drop(2)))
    }

    fun testAnOwnDirectMessageShowsSentDeliveredAndReadWithSeconds() {
        val row = ChatRow(mine(), continuesGroup = false, deletable = true, receipt = ChatReceipt.READ, edited = true)
        val times = ChatReceiptTimes(delivered = at(19, 23, 4, 12), read = at(19, 23, 6, 45))
        val page = ChatMessageInfo.page(row, times, expiresAt = at(26, 23, 0), onServer = false, nowMs = now, zone = zone)
        assertEq("Your message", page.heading)
        assertEq(listOf("Sent", "Delivered", "Read"), page.timeline.map { it.title })
        assertEq("Today at 11:04 PM", page.timeline[1].value)
        assertEq("11:04:12 PM", page.timeline[1].detail)
        expect(page.timeline[2].accent)
        assertEq(listOf("Edited", "Disappears"), page.more.map { it.title })
        assertEq("Oct 26, 2026 at 11:00 PM", page.more[1].value)
        assertEq(null, page.note)
    }

    fun testMissingTimesSayNotYetOrTheBareStateAndExplainOldReads() {
        val sent = ChatMessageInfo.page(
            ChatRow(mine(), continuesGroup = false, deletable = true, receipt = ChatReceipt.SENT),
            ChatReceiptTimes(), expiresAt = null, onServer = true, nowMs = now, zone = zone,
        )
        assertEq(listOf(ChatMessageInfo.NOT_YET, ChatMessageInfo.NOT_YET), sent.timeline.drop(1).map { it.value })
        expect(sent.timeline[1].pending)
        assertEq(listOf("Server copy"), sent.more.map { it.title })
        val oldRead = ChatMessageInfo.page(
            ChatRow(mine(), continuesGroup = false, deletable = true, receipt = ChatReceipt.READ),
            ChatReceiptTimes(), expiresAt = null, onServer = false, nowMs = now, zone = zone,
        )
        assertEq(listOf("Delivered", "Read"), oldRead.timeline.drop(1).map { it.value })
        assertEq("Read before this device kept read times.", oldRead.note)
        val sending = ChatMessageInfo.page(
            ChatRow(mine(), continuesGroup = false, deletable = true, receipt = ChatReceipt.SENDING),
            ChatReceiptTimes(), expiresAt = null, onServer = false, nowMs = now, zone = zone,
        )
        assertEq("Sending…", sending.timeline[0].value)
    }

    fun testAPeerMessageShowsWhoSentItAndNoReceipts() {
        val page = ChatMessageInfo.page(
            ChatRow(mine(self = false), continuesGroup = false, deletable = false),
            ChatReceiptTimes(), expiresAt = null, onServer = false, nowMs = now, zone = zone,
        )
        assertEq("Message", page.heading)
        assertEq(listOf("From", "Sent"), page.timeline.map { it.title })
        assertEq("Pat", page.timeline[0].value)
    }

    fun testTheSwipeOpensPastThirtyPercentOrOnAFling() {
        expect(ChatMessageInfo.opens(-121f, 0f, 400f))
        expect(!ChatMessageInfo.opens(-100f, -150f, 400f))
        expect(ChatMessageInfo.opens(-40f, -260f, 400f))
        expect(ChatMessageInfo.closes(150f, 0f, 400f))
        expect(!ChatMessageInfo.closes(120f, 100f, 400f))
    }
}
