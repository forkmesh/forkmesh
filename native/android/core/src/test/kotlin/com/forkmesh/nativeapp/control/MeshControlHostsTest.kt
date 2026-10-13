package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.chat.MeshControlHost as MeshPeerHost
import com.forkmesh.nativeapp.testing.assertEq
import java.net.InetAddress

class MeshControlHostsTest {
    fun testMapsAMeshPeerToALanControlHost() {
        val peer = MeshPeerHost(
            nodeId = "node-1",
            address = InetAddress.getByAddress(byteArrayOf(192.toByte(), 168.toByte(), 1, 20)),
            port = 48732,
            name = "Desk",
            nodeName = "alice-desktop",
            ownerUser = "alice",
            platform = "linux",
        )
        assertEq(
            MeshControlHost(
                nodeId = "node-1",
                address = "192.168.1.20",
                port = 48732,
                name = "Desk",
                nodeName = "alice-desktop",
                ownerUser = "alice",
                platform = "linux",
            ),
            MeshControlHosts.from(peer),
        )
    }
}
