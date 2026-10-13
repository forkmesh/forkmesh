package com.forkmesh.nativeapp.chat

import com.forkmesh.nativeapp.account.guestChatName
import com.forkmesh.nativeapp.api.AccountSettings
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.crypto.Bytes
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect

class ChatPersonaTest {
    private val identity = Identity.fromSeed(Bytes.random(32))

    fun testGuestDigitsMatchTheDesktopDerivation() {
        assertEq("Guest 3813", guestChatName("AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"))
        assertEq("Guest 4804", guestChatName("demo-key"))
        assertEq("Guest", guestChatName(""))
    }

    fun testASignedOutDeviceSpeaksAsAGuestNeverAsItsNode() {
        val persona = ChatPersona.resolve(identity.publicKeyB64url, account = "", displayName = "jett")
        assertEq(guestChatName(identity.publicKeyB64url), persona.name)
        expect(!persona.name.contains(identity.shortKey))
        assertEq("guest", persona.accountKind)
        assertEq("", persona.ownerUser)
        expect(persona.isGuest)
    }

    fun testASignedInDeviceSpeaksAsTheUserAndNamesItsOwner() {
        val plain = ChatPersona.resolve(identity.publicKeyB64url, account = "jett")
        assertEq("jett", plain.name)
        assertEq("user", plain.accountKind)
        assertEq("jett", plain.ownerUser)

        val named = ChatPersona.resolve(identity.publicKeyB64url, account = "jett", displayName = "Jett R.")
        assertEq("Jett R.", named.name)
        assertEq("jett", named.ownerUser)

        val frame = LinkedHashMap<String, Any?>()
        named.stamp(frame)
        assertEq(mapOf("accountKind" to "user", "ownerUser" to "jett"), frame)
    }

    fun testTheRelayRosterRowFollowsTheSessionNotTheNodeKey() {
        val settings = AccountSettings.create(MemoryPrefStore(), MemorySecretStore())
        val relay = RelayRoom(settings, identity, MemorySecretStore(), loop = EventLoop("persona", virtualTime = true))
        try {
            var account = ""
            relay.accountProvider = { account }
            var self = relay.roster().first { it.self }
            assertEq(guestChatName(identity.publicKeyB64url), self.name)
            assertEq("", self.owner)

            account = "jett"
            relay.identityChanged()
            self = relay.roster().first { it.self }
            assertEq("jett", self.name)
            assertEq("jett", self.owner)
        } finally {
            relay.dispose()
        }
    }
}
