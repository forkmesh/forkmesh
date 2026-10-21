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

    fun testChatMentionsSplitAndLinkPeopleOnly() {
        val pieces = ChatSmartMentions.split("hi @jett and @Sam, @here. @rob[bot] `@code` @a_b", listOf("Jett"))
        assertEq(
            listOf(
                ChatMentionPiece("hi "), ChatMentionPiece("@jett", ChatMentionKind.You), ChatMentionPiece(" and "),
                ChatMentionPiece("@Sam", ChatMentionKind.Person), ChatMentionPiece(", "), ChatMentionPiece("@here", ChatMentionKind.Broadcast),
                ChatMentionPiece(". "), ChatMentionPiece("@rob", ChatMentionKind.Person), ChatMentionPiece("[bot] `@code` "),
                ChatMentionPiece("@a_b", ChatMentionKind.Person),
            ),
            pieces,
        )
        val links = pieces.indices.map { ChatProfileLink.mentionLink(pieces, it) }
        assertEq(listOf(null, null, null, "forkmesh://profile/@sam", null, null, null, null, null, null), links)
    }
}
