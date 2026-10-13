package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.testing.assertEq

class ChatProfileLinkTest {
    private fun row(senderId: String = "peer-1", name: String = "Pat", self: Boolean = false, visitor: Boolean = false) =
        ChatRow(ChatMessage("m1", "#general", senderId, name, "hi", 1L, self), continuesGroup = false, deletable = false, visitor = visitor)

    fun testAnotherMembersMessageOffersTheirProfile() {
        assertEq("Pat", ChatProfileLink.senderProfile(row()))
    }

    fun testOwnVisitorAndForkBotMessagesOfferNoProfile() {
        assertEq(null, ChatProfileLink.senderProfile(row(self = true)))
        assertEq(null, ChatProfileLink.senderProfile(row(visitor = true)))
        assertEq(null, ChatProfileLink.senderProfile(row(senderId = "forkbot", name = "ForkBot")))
        assertEq(null, ChatProfileLink.senderProfile(row(senderId = "app:hello-app", name = "Hello App (app)")))
        assertEq(null, ChatProfileLink.senderProfile(row(name = "  ")))
    }
}
