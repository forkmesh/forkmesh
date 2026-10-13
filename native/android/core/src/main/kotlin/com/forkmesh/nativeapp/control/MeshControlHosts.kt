package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.chat.LocalMeshRoom
import com.forkmesh.nativeapp.chat.MeshControlHost as MeshPeerHost

class MeshControlHosts(private val mesh: LocalMeshRoom) : ControlHostSource {
    override val displayName: String get() = mesh.displayName

    override fun controlHosts(): List<MeshControlHost> = mesh.controlHosts().map(::from)

    override fun addListener(listener: () -> Unit) = mesh.addListener(listener)

    override fun removeListener(listener: () -> Unit) = mesh.removeListener(listener)

    companion object {
        fun from(host: MeshPeerHost): MeshControlHost = MeshControlHost(
            nodeId = host.nodeId,
            address = host.address.hostAddress ?: "",
            port = host.port,
            name = host.name,
            nodeName = host.nodeName,
            ownerUser = host.ownerUser,
            platform = host.platform,
        )
    }
}
