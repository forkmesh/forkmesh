package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class MeshBleTest {
    fun testUuidsMatchTheDesktopAndTheIPhone() {
        assertEq("7f9e5c14-4d3b-4a21-9c6f-2b7ad1e05f11", MESH_BLE_SERVICE_UUID)
        assertEq("7f9e5c15-4d3b-4a21-9c6f-2b7ad1e05f11", MESH_BLE_INBOX_UUID)
        assertEq("7f9e5c16-4d3b-4a21-9c6f-2b7ad1e05f11", MESH_BLE_OUTBOX_UUID)
        expect(MESH_BLE_SERVICE_UUID != MESH_BLUETOOTH_SERVICE_UUID, "BLE and RFCOMM advertise different services")
    }

    fun testChunkSizeStaysInsideAttLimits() {
        assertEq(20, MeshBle.chunkBytes(23))
        assertEq(182, MeshBle.chunkBytes(185))
        assertEq(512, MeshBle.chunkBytes(517))
    }

    fun testOutboxResendsTheHeadUntilAdvanced() {
        val outbox = MeshBleOutbox()
        expect(outbox.isEmpty, "a new outbox is empty")
        expect(outbox.peek(4) == null, "nothing to send")
        outbox.enqueue(byteArrayOf(1, 2, 3, 4, 5, 6))
        assertEq(listOf<Byte>(1, 2, 3, 4), outbox.peek(4)!!.toList())
        assertEq(listOf<Byte>(1, 2, 3, 4), outbox.peek(2)!!.toList())
        outbox.advance()
        outbox.enqueue(byteArrayOf(7))
        assertEq(listOf<Byte>(5, 6, 7), outbox.peek(4)!!.toList())
        outbox.advance()
        expect(outbox.isEmpty, "everything went")
    }

    fun testOutboxCarriesALargeFrameInOrder() {
        val outbox = MeshBleOutbox()
        val frame = ByteArray(100_000) { (it % 251).toByte() }
        outbox.enqueue(frame.copyOfRange(0, 40_000))
        val received = java.io.ByteArrayOutputStream()
        var rounds = 0
        while (true) {
            val chunk = outbox.peek(182) ?: break
            received.write(chunk)
            outbox.advance()
            rounds += 1
            if (rounds == 10) outbox.enqueue(frame.copyOfRange(40_000, frame.size))
        }
        expect(received.toByteArray().contentEquals(frame), "the frame arrives whole and in order")
    }
}
