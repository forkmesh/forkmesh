package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import java.util.Base64

class IdentityCryptoTest {
    private val url = Base64.getUrlEncoder().withoutPadding()

    fun testCreatesStoresAndReloadsTheSeed() {
        val store = MemorySecretStore()
        val identity = Identity.loadOrCreate(store)
        val stored = store.read("identity/ed25519Seed")!!
        assertEq(44, stored.length)
        expect(stored.endsWith("="), "standard padded base64, as Dart's base64.encode")
        assertBytes(Base64.getDecoder().decode(stored), identity.exportSeed())
        val again = Identity.loadOrCreate(store)
        assertEq(identity.nodeId, again.nodeId)
        assertEq(identity.publicKeyB64url, identity.nodeId)
        assertEq(43, identity.nodeId.length)
        assertBytes(Ed25519.publicKey(identity.exportSeed()), identity.publicKeyBytes)
        assertEq(url.encodeToString(identity.publicKeyBytes), identity.publicKeyB64url)
    }

    fun testAStoredSeedGivesTheSameKeyAndSignatures() {
        val seed = bytesOfHex("9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60")
        val store = MemorySecretStore(mapOf(Identity.SEED_KEY to Base64.getEncoder().encodeToString(seed)))
        val identity = Identity.loadOrCreate(store)
        assertEq(url.encodeToString(bytesOfHex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a")), identity.nodeId)
        assertEq(
            url.encodeToString(
                bytesOfHex(
                    "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
                ),
            ),
            identity.sign(ByteArray(0)),
        )
        val k = identity.publicKeyB64url
        assertEq("${k.substring(0, 8)}...${k.substring(k.length - 8)}", identity.shortKey)
        assertEq(19, identity.shortKey.length)
        assertEq(k, Identity.fromSeed(seed).nodeId)
    }

    fun testSignaturesVerifyAgainstThePublicKey() {
        val identity = Identity.loadOrCreate(MemorySecretStore())
        val data = "forkmesh-inbox-v1\nowner\nrepo".toByteArray()
        val signature = identity.sign(data)
        expect(!signature.contains('=') && signature.length == 86)
        expect(Ed25519.verify(identity.publicKeyBytes, data, Base64.getUrlDecoder().decode(signature)))
    }

    fun testRotateReplacesTheKeyInPlaceAndPersistsIt() {
        val store = MemorySecretStore()
        val identity = Identity.loadOrCreate(store)
        val oldId = identity.nodeId
        val oldKey = identity.publicKeyBytes
        identity.rotate()
        expect(identity.nodeId != oldId)
        assertEq(identity.nodeId, Identity.loadOrCreate(store).nodeId)
        val signature = Base64.getUrlDecoder().decode(identity.sign("after".toByteArray()))
        expect(!Ed25519.verify(oldKey, "after".toByteArray(), signature))
        expect(Ed25519.verify(identity.publicKeyBytes, "after".toByteArray(), signature))
        assertFails<IllegalStateException> { Identity.fromSeed(ByteArray(32)).rotate() }
    }

    fun testClearStoredSeedAndCorruptSeeds() {
        val store = MemorySecretStore()
        val identity = Identity.loadOrCreate(store)
        Identity.clearStoredSeed(store)
        assertEq(null, store.read(Identity.SEED_KEY))
        expect(Identity.loadOrCreate(store).nodeId != identity.nodeId)
        assertFails<IllegalArgumentException> { Identity.loadOrCreate(MemorySecretStore(mapOf(Identity.SEED_KEY to "AAAA"))) }
        assertFails<IllegalArgumentException> { Identity.loadOrCreate(MemorySecretStore(mapOf(Identity.SEED_KEY to "not base64"))) }
        val exported = identity.exportSeed()
        exported.fill(0)
        expect(!identity.exportSeed().contentEquals(exported), "exportSeed returns a copy")
    }
}
