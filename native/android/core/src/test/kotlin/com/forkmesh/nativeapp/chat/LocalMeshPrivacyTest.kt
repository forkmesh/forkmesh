package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.crypto.ChatTrust
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.net.InetAddress

class LocalMeshPrivacyTest {
    private val loopback: InetAddress = InetAddress.getByName("127.0.0.1")

    private class Device(val room: LocalMeshRoom, val identity: Identity, val secrets: MemorySecretStore)

    private fun device(name: String): Device {
        val settings = AccountSettings.create(MemoryPrefStore(mapOf("profile/name" to name)), MemorySecretStore())
        val identity = Identity.fromSeed(Bytes.random(32))
        val secrets = MemorySecretStore()
        val room = LocalMeshRoom(settings, identity, secrets, platform = "linux")
        room.listenAddress = loopback
        room.multicastEnabled = false
        room.bluetoothEnabled = false
        room.accountProvider = { name }
        return Device(room, identity, secrets)
    }

    private fun closeAll(vararg devices: Device) {
        for (device in devices) {
            device.room.stop()
            device.room.dispose()
        }
    }

    fun testMeshBundlesNameTheNodeAndFramesCarryNoAccount() {
        val ada = device("ada")
        val grace = device("grace")
        try {
            expect(ada.room.start() && grace.room.start())
            ada.room.connectToPeer(loopback, grace.room.listenPort)
            expect(waitUntil { grace.room.members().any { !it.self && it.name == "ada" } }, "the hello arrives")
            val record = ChatTrust.forStore(grace.secrets, MESH_ROOM_NAME).record(ada.room.sealKeyId)
            assertEq(ada.identity.nodeId, record?.acct, "a #mesh bundle names the node, not the account")
            assertEq(ada.identity.nodeId, record?.node)
            val seen = grace.room.members().single { !it.self }
            assertEq("", seen.owner, "no ownerUser travels in #mesh frames")
            for (type in listOf("hello", "chat", "typing")) expect(!ada.room.outgoingFrame(type).containsKey("ownerUser"), type)
        } finally {
            closeAll(ada, grace)
        }
    }

    fun testFocusIsSentOnlyToTheDirectMessagePartnersDevices() {
        val ann = device("ann")
        val bob = device("bob")
        val carol = device("carol")
        try {
            expect(ann.room.start() && bob.room.start() && carol.room.start())
            ann.room.connectToPeer(loopback, bob.room.listenPort)
            ann.room.connectToPeer(loopback, carol.room.listenPort)
            expect(waitUntil { ann.room.linkedKids().size == 2 })
            val channel = DirectMessageFrames.PREFIX + "f".repeat(32)
            ann.room.setFocus(channel, true, setOf(bob.room.sealKeyId))
            expect(waitUntil { bob.room.peerFocused(ann.room.sealKeyId, channel) }, "the partner learns the focus")
            expect(!waitUntil(500) { carol.room.peerFocused(ann.room.sealKeyId, channel) }, "another nearby device does not")
            ann.room.setFocus(channel, false, setOf(bob.room.sealKeyId))
            expect(waitUntil { !bob.room.peerFocused(ann.room.sealKeyId, channel) })
        } finally {
            closeAll(ann, bob, carol)
        }
    }

    fun testTheBeaconIdIsRandomAndNotTheDeviceKey() {
        val ada = device("ada")
        try {
            expect(ada.room.start())
            val first = ada.room.beacon()!!.kid
            expect(first != ada.room.sealKeyId, "the beacon does not carry the device key id")
            assertEq(16, first.length)
            ada.room.stop()
            expect(ada.room.start())
            expect(ada.room.beacon()!!.kid != first, "a new id each time the mesh starts")
        } finally {
            closeAll(ada)
        }
    }

    fun testEarlierMessagesAreNotResealedToADeviceThatLinksLater() {
        val ada = device("ada")
        val grace = device("grace")
        val linus = device("linus")
        try {
            expect(ada.room.start() && grace.room.start() && linus.room.start())
            ada.room.connectToPeer(loopback, grace.room.listenPort)
            expect(waitUntil { grace.room.peerCount == 1 })
            ada.room.sendMessage("before linus")
            expect(waitUntil { grace.room.messages.isNotEmpty() })
            linus.room.connectToPeer(loopback, ada.room.listenPort)
            expect(waitUntil { linus.room.peerCount == 1 && ada.room.peerCount == 2 })
            expect(!waitUntil(1_500) { linus.room.messages.isNotEmpty() }, "the hello backfill does not re-seal own history to a new key")
            ada.room.sendMessage("after linus")
            expect(waitUntil { linus.room.messages.map { it.text } == listOf("after linus") })
        } finally {
            closeAll(ada, grace, linus)
        }
    }
}
