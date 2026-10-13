package com.forkmesh.nativeapp.chat

const val MESH_BLE_SERVICE_UUID = "7f9e5c14-4d3b-4a21-9c6f-2b7ad1e05f11"
const val MESH_BLE_INBOX_UUID = "7f9e5c15-4d3b-4a21-9c6f-2b7ad1e05f11"
const val MESH_BLE_OUTBOX_UUID = "7f9e5c16-4d3b-4a21-9c6f-2b7ad1e05f11"
const val MESH_BLE_CCCD_UUID = "00002902-0000-1000-8000-00805f9b34fb"

object MeshBle {
    const val MAX_WRITE_BYTES = 512
    const val MIN_CHUNK_BYTES = 20
    const val REQUESTED_MTU = 517
    const val SCAN_MS = 15_000L
    const val OPEN_TIMEOUT_MS = 20_000L
    const val QUIET_AFTER_DUPLICATE_MS = 600_000L

    fun chunkBytes(mtu: Int): Int = (mtu - 3).coerceIn(MIN_CHUNK_BYTES, MAX_WRITE_BYTES)
}

class MeshBleOutbox {
    private var pending = ByteArray(0)
    private var start = 0
    private var head: ByteArray? = null

    val isEmpty: Boolean get() = head == null && start >= pending.size

    fun enqueue(bytes: ByteArray) {
        if (bytes.isEmpty()) return
        val left = pending.size - start
        val next = ByteArray(left + bytes.size)
        System.arraycopy(pending, start, next, 0, left)
        System.arraycopy(bytes, 0, next, left, bytes.size)
        pending = next
        start = 0
    }

    fun peek(max: Int): ByteArray? {
        head?.let { return it }
        val left = pending.size - start
        if (left <= 0) return null
        val size = minOf(maxOf(max, 1), left)
        val chunk = pending.copyOfRange(start, start + size)
        start += size
        if (start >= pending.size) {
            pending = ByteArray(0)
            start = 0
        }
        head = chunk
        return chunk
    }

    fun advance() {
        head = null
    }

    fun clear() {
        pending = ByteArray(0)
        start = 0
        head = null
    }
}
