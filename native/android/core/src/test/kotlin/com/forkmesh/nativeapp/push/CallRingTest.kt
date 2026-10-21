package com.forkmesh.nativeapp.push

import com.forkmesh.nativeapp.testing.assertEq

class CallRingTest {
    private val id = "c".repeat(32)

    fun testARingEventNamesItsDirectConversation() {
        val frame = mapOf("type" to "event", "topic" to "calls", "conversation" to "direct:$id")
        assertEq("direct:$id", CallRings.conversation(frame))
        assertEq(null, CallRings.conversation(frame + ("topic" to "direct-messages")))
        assertEq(null, CallRings.conversation(frame + ("conversation" to "direct:short")))
        assertEq(null, CallRings.conversation(frame + ("conversation" to "#general")))
        assertEq(null, CallRings.conversation(frame - "conversation"))
    }

    fun testThePartnerComesFromTheConversationList() {
        val rows = listOf(
            mapOf("id" to "a".repeat(32), "otherUser" to "sam"),
            mapOf("id" to id, "otherUser" to " Pat "),
        )
        assertEq("pat", CallRings.partner(rows, "direct:$id"))
        assertEq("", CallRings.partner(rows, "direct:" + "b".repeat(32)))
        assertEq("", CallRings.partner(null, "direct:$id"))
    }

    fun testTheRingOpensTheConversation() {
        val target = CallRings.target("project-1", "direct:$id")
        assertEq("direct:$id", directMessageTargetConversation(target))
        assertEq("project-1", target.projectId)
        assertEq("@pat is calling you", CallRings.text("pat"))
        assertEq("Incoming call", CallRings.title(""))
    }
}
