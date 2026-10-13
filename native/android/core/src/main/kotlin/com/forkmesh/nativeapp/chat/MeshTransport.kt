package com.forkmesh.nativeapp.chat

import java.net.Socket

const val MESH_BLUETOOTH_SERVICE_UUID = "7f9e5c10-4d3b-4a21-9c6f-2b7ad1e05f11"
const val MESH_BLUETOOTH_SERVICE_NAME = "ForkMesh local mesh"

enum class MeshBluetoothEventKind { LINK_OPENED, DATA, LINK_CLOSED }

class MeshBluetoothEvent(
    val kind: MeshBluetoothEventKind,
    val linkId: Int,
    val outbound: Boolean = false,
    val bytes: ByteArray = ByteArray(0),
)

interface MeshNativeTransport {
    val listening: Boolean

    val unavailable: Boolean

    fun startBluetooth(): Boolean

    fun stopBluetooth()

    fun sendBluetooth(linkId: Int, frame: ByteArray): Boolean

    fun closeBluetoothLink(linkId: Int)

    fun closeBluetoothLink(linkId: Int, duplicate: Boolean) = closeBluetoothLink(linkId)

    fun acquireMulticastLock(): Boolean

    fun releaseMulticastLock()

    fun rescanBluetooth() {}

    fun tuneLinkSocket(socket: Socket) {}

    fun addEventListener(listener: (MeshBluetoothEvent) -> Unit)

    fun removeEventListener(listener: (MeshBluetoothEvent) -> Unit)

    fun dispose() {}

    companion object {
        val UNAVAILABLE: MeshNativeTransport = object : MeshNativeTransport {
            override val listening: Boolean get() = false
            override val unavailable: Boolean get() = true

            override fun startBluetooth(): Boolean = false

            override fun stopBluetooth() {}

            override fun sendBluetooth(linkId: Int, frame: ByteArray): Boolean = false

            override fun closeBluetoothLink(linkId: Int) {}

            override fun acquireMulticastLock(): Boolean = false

            override fun releaseMulticastLock() {}

            override fun addEventListener(listener: (MeshBluetoothEvent) -> Unit) {}

            override fun removeEventListener(listener: (MeshBluetoothEvent) -> Unit) {}
        }
    }
}
