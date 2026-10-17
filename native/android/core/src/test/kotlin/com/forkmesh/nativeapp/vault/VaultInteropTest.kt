package com.forkmesh.nativeapp.vault

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.obj
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText

fun expectCode(code: String, block: () -> Unit) {
    try {
        block()
    } catch (error: VaultCryptoException) {
        assertEq(code, error.code)
        return
    }
    throw AssertionError("accepted; expected $code")
}

class VaultInteropTest {
    private val passwordBytes = "correct horse battery staple ".toByteArray() + byteArrayOf(0xe2.toByte(), 0x84.toByte(), 0xab.toByte())

    fun testOpensTheBrowsersFixture() = opens("interop-from-js.json")

    fun testOpensTheDesktopsFixture() = opens("interop-from-cpp.json")

    fun testOpensTheDartFixture() = opens("interop-from-dart.json")

    fun testOpensWhatItWrites() = opens(Json.parseObject(Json.stringify(makeFixture())), "kotlin")

    private fun opens(name: String) = opens(Json.parseObject(resourceText("vault/$name")), name)

    private fun opens(v: Map<String, Any?>, name: String) {
        assertEq("forkmesh-vault-interop-v1", v.str("format"), name)
        assertEq(VaultCrypto.SUITE, v.str("suite"), name)
        expect(v.str("password").toByteArray().contentEquals(passwordBytes), "$name: the password keeps U+212B")
        val kdf = VaultKdf.fromJson(v["kdf"])
        val masterKey = VaultCrypto.stretch(v.str("password"), kdf)
        assertEq(v.str("masterKey"), VaultCrypto.toHex(masterKey), "$name: Argon2id master key")

        val accounts = v.obj("accounts")
        val alice = VaultKeyset.fromJson(accounts.obj("alice")["keyset"])
        val bob = VaultKeyset.fromJson(accounts.obj("bob")["keyset"])
        val aliceBundle = VaultBundle.verify(accounts.obj("alice")["bundle"], "alice")
        VaultBundle.verify(accounts.obj("bob")["bundle"], "bob")

        val opened = VaultCrypto.unwrapKeyset(masterKey, "master", "alice", v["wrappedKeyset"])
        assertEq(alice.toJson(), opened.toJson(), name)
        expectCode("wrong_password") { VaultCrypto.unwrapKeyset(masterKey, "master", "bob", v["wrappedKeyset"]) }
        expectCode("wrong_password") { VaultCrypto.unwrapKeyset(masterKey, "recovery", "alice", v["wrappedKeyset"]) }

        val recoveryCode = v.str("recoveryCode")
        val recovery = VaultCrypto.recoverySecret(recoveryCode)
        assertEq(v.str("recoverySecret"), VaultCrypto.toHex(recovery), name)
        val loose = recoveryCode.lowercase().replace("-", " ").replace("0", "o")
        assertEq(v.str("recoverySecret"), VaultCrypto.toHex(VaultCrypto.recoverySecret(loose)), name)
        assertEq(recoveryCode, VaultCrypto.recoveryCode(recovery), name)
        val recovered = VaultCrypto.unwrapKeyset(recovery, "recovery", "alice", v["wrappedRecovery"])
        assertEq(alice.toJson(), recovered.toJson(), name)

        for ((who, keyset) in listOf("alice" to alice, "bob" to bob)) {
            val derived = VaultBundle.of(keyset, who)
            val published = accounts.obj(who).obj("bundle")
            assertEq(published.str("x25519"), VaultCrypto.toBase64(derived.x25519), "$name $who")
            assertEq(published.str("mlkem768"), VaultCrypto.toBase64(derived.mlkem768), "$name $who mlkem")
            assertEq(published.str("ed25519"), VaultCrypto.toBase64(derived.ed25519), "$name $who")
            assertEq(accounts.obj(who).str("fingerprint"), derived.fingerprint(), "$name $who")
            VaultBundle.verify(derived.toJson(), who)
        }

        val item = v.obj("item")
        val itemId = item.str("id")
        val itemKey = VaultCrypto.unwrapItemKey(alice, itemId, item["wrappedKey"])
        assertEq(item.str("key"), VaultCrypto.toHex(itemKey), name)
        assertEq(item.obj("fields"), VaultCrypto.openItem(itemKey, itemId, item["body"]), name)
        assertEq(12 + 16, VaultCrypto.fromBase64(item["body"]).size % 256, name)
        assertEq(v.obj("trust").obj("value"), VaultCrypto.openPrivate(alice, "trust", v.obj("trust")["blob"]), name)

        val grants = v.obj("grants")
        fun open(
            keyset: VaultKeyset = bob,
            recipient: String = "bob",
            sender: VaultBundle = aliceBundle,
            id: String = itemId,
            grant: Any? = grants["read"],
            permission: String = "read",
        ): ByteArray = VaultCrypto.openGrant(keyset, recipient, sender, id, grant, permission)
        for (permission in VaultCrypto.PERMISSIONS) {
            assertEq(item.str("key"), VaultCrypto.toHex(open(grant = grants[permission], permission = permission)), "$name $permission")
        }
        expectCode("invalid_share_signature") { open(permission = "write") }
        expectCode("invalid_share_signature") { open(id = VaultCrypto.newItemId()) }
        expectCode("invalid_share_signature") { open(keyset = alice, recipient = "alice") }
        val forged = VaultBundle.of(VaultKeyset.generate(), "alice")
        expectCode("invalid_share_signature") { open(sender = forged) }
        for (field in listOf("x25519", "mlkem768", "wrapped")) {
            val bytes = VaultCrypto.fromBase64(grants.obj("read")[field])
            bytes[3] = (bytes[3].toInt() xor 1).toByte()
            val tampered = LinkedHashMap(grants.obj("read"))
            tampered[field] = VaultCrypto.toBase64(bytes)
            expectCode("invalid_share_signature") { open(grant = tampered) }
        }
        val relabelled = LinkedHashMap(accounts.obj("bob").obj("bundle"))
        relabelled["account"] = "mallory"
        expectCode("invalid_public_keys") { VaultBundle.verify(relabelled, "mallory") }
        val swapped = LinkedHashMap(accounts.obj("bob").obj("bundle"))
        swapped["mlkem768"] = accounts.obj("alice").obj("bundle")["mlkem768"]
        expectCode("invalid_public_keys") { VaultBundle.verify(swapped, "bob") }
        expectCode("decryption_failed") { VaultCrypto.openItem(itemKey, VaultCrypto.newItemId(), item["body"]) }
    }

    private fun makeFixture(): Map<String, Any?> {
        val password = String(passwordBytes, Charsets.UTF_8)
        val kdf = VaultKdf.fresh(VaultKdf.FLOOR_MEMORY_KIB, VaultKdf.FLOOR_PASSES)
        val masterKey = VaultCrypto.stretch(password, kdf)
        val alice = VaultKeyset.generate()
        val bob = VaultKeyset.generate()
        val aliceBundle = VaultBundle.of(alice, "alice")
        val bobBundle = VaultBundle.of(bob, "bob")
        val recoveryCode = VaultCrypto.newRecoveryCode()
        val itemId = VaultCrypto.newItemId()
        val itemKey = VaultCrypto.randomBytes(32)
        val fields = linkedMapOf<String, Any?>(
            "title" to "Registrar — prod",
            "username" to "ops@acme.test",
            "password" to "päss \"quoted\" \\ wörd",
            "url" to "https://registrar.example/login",
            "notes" to "line one\nline two",
        )
        val trust = linkedMapOf<String, Any?>("pins" to linkedMapOf("bob" to bobBundle.fingerprint()))
        val grants = linkedMapOf<String, Any?>()
        for (permission in VaultCrypto.PERMISSIONS) {
            grants[permission] = VaultCrypto.sealGrant(alice, "alice", bobBundle, itemId, itemKey, permission)
        }
        val recovery = VaultCrypto.recoverySecret(recoveryCode)
        return linkedMapOf(
            "format" to "forkmesh-vault-interop-v1",
            "suite" to VaultCrypto.SUITE,
            "madeBy" to "kotlin",
            "password" to password,
            "kdf" to kdf.toJson(),
            "masterKey" to VaultCrypto.toHex(masterKey),
            "accounts" to linkedMapOf(
                "alice" to linkedMapOf("keyset" to alice.toJson(), "bundle" to aliceBundle.toJson(), "fingerprint" to aliceBundle.fingerprint()),
                "bob" to linkedMapOf("keyset" to bob.toJson(), "bundle" to bobBundle.toJson(), "fingerprint" to bobBundle.fingerprint()),
            ),
            "wrappedKeyset" to VaultCrypto.wrapKeyset(masterKey, "master", "alice", alice),
            "recoveryCode" to recoveryCode,
            "recoverySecret" to VaultCrypto.toHex(recovery),
            "wrappedRecovery" to VaultCrypto.wrapKeyset(recovery, "recovery", "alice", alice),
            "item" to linkedMapOf(
                "id" to itemId,
                "key" to VaultCrypto.toHex(itemKey),
                "fields" to fields,
                "body" to VaultCrypto.sealItem(itemKey, itemId, fields),
                "wrappedKey" to VaultCrypto.wrapItemKey(alice, itemId, itemKey),
            ),
            "trust" to linkedMapOf("value" to trust, "blob" to VaultCrypto.sealPrivate(alice, "trust", trust)),
            "grants" to grants,
        )
    }
}
