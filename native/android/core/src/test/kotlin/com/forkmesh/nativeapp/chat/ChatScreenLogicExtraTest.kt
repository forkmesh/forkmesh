package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ChatScreenLogicExtraTest {
    fun testTheMeshBadgeCountsLinkedDevices() {
        assertEq("local only", meshBadge(networkActive = false, bluetoothActive = false, peerCount = 3))
        assertEq("local · this network", meshBadge(true, false))
        assertEq("local · this network + bluetooth · 1 device", meshBadge(true, true, 1))
        assertEq("local · bluetooth · 2 devices", meshBadge(false, true, 2))
    }

    fun testTheEmptyMeshSaysWhetherItIsStillLooking() {
        expect(meshEmptyMessage(networkActive = true, peerCount = 0).endsWith("Looking for devices with ForkMesh open on this Wi-Fi."))
        expect(!meshEmptyMessage(networkActive = true, peerCount = 1).contains("Looking"))
        expect(meshEmptyMessage(networkActive = false, peerCount = 0, localNetworkBlocked = true).startsWith("This phone is not allowed"))
    }

    fun testOnlyYourOwnRelayMessagesCanBeDeleted() {
        val mine = ChatMessage(id = "1", conversation = "#general", senderId = "me", senderName = "me", text = "hi", timestamp = 0, self = true)
        expect(canDeleteMessage(mine))
        expect(!canDeleteMessage(mine.copy(self = false)))
        expect(!canDeleteMessage(mine.copy(conversation = MESH_CONVERSATION)))
        assertEq("hi", messageCopyText(mine))
        assertEq("notes.txt", messageCopyText(mine.copy(text = "", fileName = "notes.txt")))
    }
}
