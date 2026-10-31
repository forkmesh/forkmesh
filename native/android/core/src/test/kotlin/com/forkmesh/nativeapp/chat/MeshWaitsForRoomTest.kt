package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.net.InetAddress

class MeshWaitsForRoomTest {
    fun testTheMeshStaysOffUntilTheRoomIsFirstOpened() {
        val settings = AccountSettings.create(MemoryPrefStore(mapOf("profile/name" to "ada")), MemorySecretStore())
        val room = LocalMeshRoom(settings, Identity.fromSeed(Bytes.random(32)), MemorySecretStore(), platform = "linux")
        room.listenAddress = InetAddress.getByName("127.0.0.1")
        room.multicastEnabled = false
        room.bluetoothEnabled = false
        var opened = 0
        room.waitsForRoom = true
        room.onFirstRoomOpen = { opened++ }
        try {
            expect(!room.start(), "start waits for the room")
            expect(!room.running)
            assertEq(0, opened)
            expect(room.roomOpened(), "opening #mesh starts it")
            expect(room.running)
            assertEq(1, opened)
            expect(room.roomOpened())
            assertEq(1, opened, "only the first opening is reported")
        } finally {
            room.stop()
            room.dispose()
        }
    }
}
