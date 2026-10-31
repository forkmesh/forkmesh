package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.model.MemberGroup
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ChatShareTargetsTest {
    private fun group(id: String, name: String, online: Boolean = true, self: Boolean = false, owner: String = name) =
        MemberGroup(name, listOf(Member(id = id, name = name, online = online, self = self, owner = owner)), self = self)

    private fun targets(): List<ChatShareTarget> = ChatShareTargets.build(
        channels = listOf("#general", "#reading", "#office/ops", "#reading"),
        offices = listOf("#office/ops", "not-office"),
        directs = listOf(DirectConversation("d1", "alpha"), DirectConversation("d2", " "), DirectConversation("d3", "Alpha")),
        people = listOf(
            group("n1", "alpha"),
            group("n2", "bravo"),
            group("n3", "charlie", online = false),
            group("n4", "me", self = true),
            group("n5", "Guest 12", owner = ""),
            group("n6", "echo", owner = ""),
        ),
        privateChannels = setOf("#reading"),
    )

    fun testBuildListsChannelsDirectsAndOnlinePeople() {
        val list = targets()
        assertEq(
            listOf(
                ChatShareTarget(ChatShareKind.CHANNEL, "#general", "#general"),
                ChatShareTarget(ChatShareKind.CHANNEL, "#reading", "#reading", private = true),
                ChatShareTarget(ChatShareKind.OFFICE, "#office/ops", "#ops"),
                ChatShareTarget(ChatShareKind.DIRECT, DirectMessageFrames.PREFIX + "d1", "alpha", user = "alpha"),
                ChatShareTarget(ChatShareKind.PERSON, "@n2", "bravo", user = "bravo"),
            ),
            list,
        )
        assertEq("@bravo", list[4].guestProbe, "a person is reached through their account")
        assertEq("@alpha", list[3].guestProbe)
        assertEq("#general", list[0].guestProbe)
    }

    fun testFilterAndSearchResults() {
        val list = targets()
        assertEq(listOf("#reading"), ChatShareTargets.filter(list, "#read").map { it.title })
        assertEq(listOf("bravo"), ChatShareTargets.filter(list, "@BRA").map { it.title })
        assertEq(listOf("#ops", "bravo"), ChatShareTargets.filter(list, "o").map { it.title })
        assertEq(list, ChatShareTargets.filter(list, "  "))
        expect(!ChatShareTargets.searchable("@a"))
        expect(ChatShareTargets.searchable("al"))
        val found = ChatShareTargets.users(listOf("ALPHA", "delta", "Delta", " "), list)
        assertEq(listOf(ChatShareTarget(ChatShareKind.USER, "", "delta", user = "delta")), found)
        assertEq("@delta", found[0].guestProbe)
    }
}
