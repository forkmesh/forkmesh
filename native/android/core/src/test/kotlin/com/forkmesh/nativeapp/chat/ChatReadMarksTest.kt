package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ChatReadMarksTest {
    private val directKey = "direct:" + "a".repeat(32)

    private class FakeCalling : ChatAccountCalling {
        val calls = ArrayList<Triple<String, String, Map<String, Any?>?>>()
        var marks: Map<String, Any?> = emptyMap()

        override fun accountApiCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> {
            calls.add(Triple(method, path, body))
            return if (method == "GET") mapOf("marks" to marks) else mapOf("ok" to true)
        }
    }

    private fun marks(api: FakeCalling) = ChatReadMarks(api, delayMs = 3_600_000L)

    private fun chat(relay: FakeChatRelay, id: String, ts: Long) =
        relay.handle(mapOf("type" to "chat", "id" to id, "senderId" to "remote", "sender" to "Remote", "ts" to ts,
            "channel" to "#random", "text" to "hi"))

    fun testOnlySharedConversationKeysAreSynced() {
        expect(ChatReadMarks.isSyncable("#general"))
        expect(ChatReadMarks.isSyncable(directKey))
        expect(ChatReadMarks.isSyncable("private:" + "b".repeat(32)))
        expect(!ChatReadMarks.isSyncable("#office/lobby"))
        expect(!ChatReadMarks.isSyncable("#General"))
        expect(!ChatReadMarks.isSyncable("@peer"))
        expect(!ChatReadMarks.isSyncable("direct:abc"))
    }

    fun testQueuedMarksPostOnceAndNeverMoveBack() {
        val api = FakeCalling()
        val readMarks = marks(api)
        readMarks.queue("#general", 2_000)
        readMarks.queue("#general", 1_000)
        readMarks.queue(directKey, 5_000)
        readMarks.queue("#office/lobby", 5_000)
        readMarks.flush()
        assertEq(1, api.calls.size)
        assertEq("POST", api.calls[0].first)
        assertEq(ChatReadMarks.ENDPOINT, api.calls[0].second)
        assertEq(mapOf("#general" to 2_000L, directKey to 5_000L), api.calls[0].third?.get("marks"))
        readMarks.queue("#general", 2_000)
        readMarks.flush()
        assertEq(1, api.calls.size)
    }

    fun testFetchedMarksMerge() {
        val api = FakeCalling()
        api.marks = mapOf("#general" to 9_000, "#random" to 4_000L)
        val readMarks = marks(api)
        readMarks.queue("#random", 6_000)
        readMarks.flush()
        expect(readMarks.fetch())
        assertEq(9_000L, readMarks.mark("#general"))
        assertEq(6_000L, readMarks.mark("#random"))
        expect(readMarks.isRead("#general", 9_000))
        expect(!readMarks.isRead("#general", 9_001))
    }

    fun testReadingElsewhereClearsUnreadAndSkipsReplayedMessages() {
        val relay = FakeChatRelay()
        val hub = ChatHub(relay)
        val api = FakeCalling()
        hub.readMarks = marks(api)
        chat(relay, "one", 1_000)
        chat(relay, "two", 2_000)
        assertEq(2, relay.unread["#random"])
        api.marks = mapOf("#random" to 1_500L)
        hub.applyReadMarks()
        assertEq(2, relay.unread["#random"])
        api.marks = mapOf("#random" to 2_000L)
        hub.applyReadMarks(force = true)
        expect(relay.unread.isEmpty())
        chat(relay, "replayed", 1_800)
        chat(relay, "fresh", 3_000)
        assertEq(1, relay.unread["#random"])
    }

    fun testReadMarksAreFetchedAtMostOnceInFiveSecondsUnlessPushed() {
        val relay = FakeChatRelay()
        val hub = ChatHub(relay)
        val api = FakeCalling()
        hub.readMarks = marks(api)
        var now = 100_000L
        hub.readMarksClock = { now }
        chat(relay, "one", 1_000)
        api.marks = mapOf("#random" to 500L)
        hub.applyReadMarks()
        val fetched = api.calls.size
        api.marks = mapOf("#random" to 1_000L)
        now += 4_000
        hub.applyReadMarks()
        assertEq(fetched, api.calls.size)
        assertEq(1, relay.unread["#random"])
        now += 1_000
        hub.applyReadMarks()
        assertEq(fetched * 2, api.calls.size)
        expect(relay.unread.isEmpty())
        hub.applyReadMarks(force = true)
        assertEq(fetched * 3, api.calls.size)
    }

    fun testOpeningAConversationQueuesItsNewestMessage() {
        val relay = FakeChatRelay()
        val hub = ChatHub(relay)
        val api = FakeCalling()
        val readMarks = marks(api)
        hub.readMarks = readMarks
        chat(relay, "one", 7_000)
        hub.select("#random")
        readMarks.flush()
        assertEq(mapOf("#random" to 7_000L), api.calls.last().third?.get("marks"))
        expect(relay.unread.isEmpty())
    }
}
