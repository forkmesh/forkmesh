package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.model.ChatMessage
import com.forkmesh.nativeapp.model.UserStatus
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class RelayRoomStatusTest {
    private fun relay(): RelayRoom {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        return RelayRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), loop = EventLoop("status", virtualTime = true))
    }

    fun testOwnStatusIsUnknownUntilSetThenLiveForSelf() {
        val room = relay()
        try {
            assertEq(null, room.liveStatusFor(room.selfId))
            assertEq(null, room.liveStatusFor(""))
            val away = UserStatus(away = true, emoji = "🌴", text = "off")
            room.setOwnStatus(away)
            room.loop.awaitIdle()
            assertEq(away, room.liveStatusFor(room.selfId))
            assertEq(away, room.ownStatus)
            assertEq(away, room.roster().first { it.self }.userStatus)
            assertEq(null, room.liveStatusFor("someone-else"))
        } finally {
            room.dispose()
        }
    }

    fun testDeletingOfflineRemovesTheLocalCopyAndReportsNotSent() {
        val room = relay()
        try {
            val msg = ChatMessage(id = "m1", conversation = "#general", senderId = room.selfId, senderName = "me", text = "hi", timestamp = 1, self = true)
            assertEq(false, room.deleteMessage(msg))
            expect(room.messages("#general").isEmpty())
        } finally {
            room.dispose()
        }
    }

    fun testASwitchForgetsTheRelayBeingLeft() {
        val room = relay()
        try {
            room.resetForRelaySwitch()
            expect(room.log.isEmpty())
            expect(room.roster().size == 1)
        } finally {
            room.dispose()
        }
    }
}
