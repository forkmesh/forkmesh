package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.Member
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class MemberGroupsTest {
    fun testYourOtherDevicesJoinYourOwnRow() {
        val groups = memberGroupsOf(
            listOf(
                Member(id = "MXcjPhoneNode2mOE", name = "jett", owner = "jett", self = true, online = true, platform = "ios"),
                Member(id = "fungiA", name = "fungi2", owner = "fungi2", online = true, platform = "ios"),
                Member(id = "w0F5DesktopNodeggm4", name = "jett", owner = "Jett", online = true, platform = "linux"),
            ),
        )
        assertEq(listOf("jett", "fungi2"), groups.map { it.name })
        val me = groups[0]
        expect(me.self)
        assertEq(listOf("MXcjPhoneNode2mOE", "w0F5DesktopNodeggm4"), me.members.map { it.id })
        assertEq("", me.disambiguator)
        assertEq("MXcjPhoneNode2mOE", me.id)
    }

    fun testAGuestDeviceStaysItsOwnRow() {
        val groups = memberGroupsOf(
            listOf(
                Member(id = "me", name = "guest-1234", self = true, online = true),
                Member(id = "other", name = "guest-1234", online = true),
            ),
        )
        assertEq(2, groups.size)
        expect(groups[0].self && !groups[1].self)
        assertEq("me", groups[0].disambiguator)
        assertEq("other", groups[1].disambiguator)
    }

    fun testTheMeshRoomGroupsDevicesOfOneAccount() {
        val groups = meshMemberGroups(
            listOf(
                Member(id = "phone", name = "Ada", owner = "ada", self = true, online = true),
                Member(id = "laptop", name = "Ada's laptop", owner = "ada", online = true),
                Member(id = "bob", name = "bob", online = true),
            ),
        )
        assertEq(listOf("ada", "bob"), groups.map { it.name })
        assertEq(2, groups[0].members.size)
        expect(groups[0].self)
    }
}
