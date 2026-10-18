package com.forkmesh.nativeapp.control

import com.forkmesh.nativeapp.chat.LocalMeshRoom
import com.forkmesh.nativeapp.chat.MeshControlHost as MeshPeerHost
import java.net.InetAddress

class MeshControlHosts(private val mesh: LocalMeshRoom) : ControlHostSource {
    override val displayName: String get() = mesh.displayName

    override fun controlHosts(): List<MeshControlHost> = mesh.controlHosts().map(::from)

    override fun addListener(listener: () -> Unit) = mesh.addListener(listener)

    override fun removeListener(listener: () -> Unit) = mesh.removeListener(listener)

    override fun setRememberedTargets(provider: () -> List<String>) {
        mesh.rememberedBeaconTargets = { provider().mapNotNull(::numericAddress) }
    }

    companion object {
        fun numericAddress(text: String): InetAddress? {
            val v4 = text.isNotEmpty() && text.all { it.isDigit() || it == '.' }
            val v6 = ':' in text && text.all { it == ':' || it == '.' || it.isDigit() || it.lowercaseChar() in 'a'..'f' }
            return if (v4 || v6) runCatching { InetAddress.getByName(text) }.getOrNull() else null
        }

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
