package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.account.projects.ScopedSecretStore
import com.forkmesh.nativeapp.api.MemoryPrefStore
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.settings.DataFolder
import com.forkmesh.nativeapp.settings.DataFolderIdentity
import com.forkmesh.nativeapp.settings.DataFolderStore
import com.forkmesh.nativeapp.settings.FileDataFolderIo
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.nio.file.Files

class ChatKeyReinstallTest {
    private val room = "chat-direct:0123456789abcdef0123456789abcdef:v1"
    private val scope = "direct:0123456789abcdef0123456789abcdef"
    private val installedAt = 1_790_000_000_000L
    private val reinstalledAt = installedAt + 3_600_000L

    private fun phone(store: MemorySecretStore, now: Long): RoomSeal {
        Identity.loadOrCreate(store)
        return RoomSeal.loadOrCreate(room, scope, SealPolicy.STRICT, "alice", store, clock = { now }, node = "alice-node")
    }

    private fun peer(now: Long): RoomSeal =
        RoomSeal.loadOrCreate(room, scope, SealPolicy.STRICT, "bob", MemorySecretStore(), clock = { now }, node = "bob-node")

    private fun ctr(frame: Map<String, Any?>): Long = (frame["ctr"] as Number).toLong()

    fun testAReinstallThatRestoresTheNodeKeyKeepsTheSafetyNumber() {
        val oldStore = MemorySecretStore()
        val alice = phone(oldStore, installedAt)
        val bob = peer(installedAt)
        assertEq(TrustState.APPROVED, bob.acceptKeyFrame(alice.keyFrame(installedAt), installedAt)?.state)
        alice.acceptKeyFrame(bob.keyFrame(installedAt), installedAt)
        var last = 0L
        for (index in 1..3) {
            val frame = alice.seal(mapOf("type" to "chat", "id" to "a$index", "senderId" to "alice-node", "text" to "hi"), installedAt)
            assertEq("hi", bob.unseal(frame, installedAt)?.get("text"))
            last = ctr(frame)
        }
        val before = bob.safetyNumber().digits
        expect(bob.trust.markVerified())

        val newStore = MemorySecretStore()
        newStore.write(Identity.SEED_KEY, oldStore.read(Identity.SEED_KEY)!!)
        val reinstalled = phone(newStore, reinstalledAt)
        assertEq(alice.keyId, reinstalled.keyId)
        val outcome = bob.acceptKeyFrame(reinstalled.keyFrame(reinstalledAt), reinstalledAt)
        assertEq(TrustState.APPROVED, outcome?.state, "the peer sees the same device, not a new one waiting for approval")
        assertEq(false, outcome?.isNew)
        reinstalled.acceptKeyFrame(bob.keyFrame(reinstalledAt), reinstalledAt)
        assertEq(before, bob.safetyNumber().digits)
        expect(bob.trust.verified(), "the peer keeps verified")
        assertEq(before, reinstalled.safetyNumber().digits)

        val frame = reinstalled.seal(mapOf("type" to "chat", "id" to "r1", "senderId" to "alice-node", "text" to "back"), reinstalledAt)
        expect(ctr(frame) > last, "the new install's counters pass the old ones")
        assertEq("back", bob.unseal(frame, reinstalledAt)?.get("text"))
    }

    fun testANewNodeKeyIsANewDevice() {
        assertEq(false, phone(MemorySecretStore(), installedAt).keyId == phone(MemorySecretStore(), installedAt).keyId)
    }

    fun testAStoredChatKeyIsKeptAsItIs() {
        val store = MemorySecretStore()
        val earlier = ChatDeviceKeys.forStore(store).kid
        val stored = store.read(ChatDeviceKeys.IDENTITY_STORE_KEY)
        Identity.loadOrCreate(store)
        val copy = MemorySecretStore()
        copy.write(ChatDeviceKeys.IDENTITY_STORE_KEY, stored!!)
        copy.write(Identity.SEED_KEY, store.read(Identity.SEED_KEY)!!)
        assertEq(earlier, ChatDeviceKeys.forStore(copy).kid, "an install from before keeps its key, so its safety numbers stay")
        assertEq(null, copy.read(ChatDeviceKeys.COUNTER_FLOOR_STORE_KEY))
    }

    fun testWithoutANodeKeyTheChatKeyIsRandomAndCountsFromOne() {
        val store = MemorySecretStore()
        val seal = RoomSeal.loadOrCreate(room, scope, SealPolicy.OPEN, "alice", store, clock = { installedAt }, node = "alice-node")
        assertEq(null, store.read(Identity.SEED_KEY))
        assertEq(null, store.read(ChatDeviceKeys.COUNTER_FLOOR_STORE_KEY))
        assertEq(1L, ctr(seal.seal(mapOf("type" to "chat", "id" to "a1", "senderId" to "alice-node"), installedAt)))
    }

    fun testTheDerivedKeyIsSeparateFromTheNodeKey() {
        val node = Ed25519.generateSeed()
        val derived = ChatDeviceKeys.derivedSeed(node)
        assertEq(Ed25519.SEED_BYTES, derived.size)
        expect(!derived.contentEquals(node))
        expect(derived.contentEquals(ChatDeviceKeys.derivedSeed(node)))
    }

    fun testRestoringTheIdentityKeyBringsBackTheChatKey() {
        val original = MemorySecretStore()
        val identity = Identity.loadOrCreate(original)
        val chatKey = ChatDeviceKeys.forStore(original).kid
        val root = Files.createTempDirectory("fm-chat-key-restore").toFile()
        try {
            val folder = DataFolderStore(FileDataFolderIo(File(root, DataFolder.DEFAULT_FOLDER_NAME)))
            DataFolderIdentity.saveKit(identity, "correct horse battery", "correct horse battery", "forkmesh", folder, MemoryPrefStore(), 1L)
            val kit = folder.readKit("forkmesh") ?: throw AssertionError("no kit")

            val keychain = MemorySecretStore()
            val scoped = ScopedSecretStore(keychain, "p/work/")
            Identity.loadOrCreate(scoped)
            expect(ChatDeviceKeys.forStore(scoped).kid != chatKey)
            expect(keychain.read("p/work/" + ChatDeviceKeys.IDENTITY_STORE_KEY) != null)
            DataFolderIdentity.restoreSeed(kit, "correct horse battery", "p/work/", keychain)
            assertEq(null, keychain.read("p/work/" + ChatDeviceKeys.IDENTITY_STORE_KEY), "the chat key is made again from the restored node key")
            val restarted = MemorySecretStore()
            restarted.write(Identity.SEED_KEY, keychain.read("p/work/" + Identity.SEED_KEY)!!)
            assertEq(chatKey, ChatDeviceKeys.forStore(restarted).kid)

            keychain.write("p/work/" + ChatDeviceKeys.IDENTITY_STORE_KEY, restarted.read(ChatDeviceKeys.IDENTITY_STORE_KEY)!!)
            DataFolderIdentity.restoreSeed(kit, "correct horse battery", "p/work/", keychain)
            expect(keychain.read("p/work/" + ChatDeviceKeys.IDENTITY_STORE_KEY) != null, "restoring the same key keeps the chat key")
        } finally {
            root.deleteRecursively()
        }
    }
}
