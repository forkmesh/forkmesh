package com.forkmesh.nativeapp.vault

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import java.io.Writer

class VaultBridge private constructor(private val process: Process) {
    private val input: Writer = process.outputStream.bufferedWriter()
    private val output: BufferedReader = BufferedReader(InputStreamReader(process.inputStream))

    @Synchronized
    fun send(message: Map<String, Any?>): Map<String, Any?> {
        input.write(Json.stringify(message) + "\n")
        input.flush()
        val line = output.readLine() ?: throw VaultException("bridge_closed")
        return Json.parseObject(line)
    }

    @Suppress("UNCHECKED_CAST")
    fun control(name: String, extra: Map<String, Any?> = emptyMap()): Map<String, Any?> =
        send(mapOf("control" to name) + extra)["data"] as? Map<String, Any?> ?: emptyMap()

    fun client(actor: String): VaultClient = VaultClient({ method, path, body ->
        val reply = send(mapOf("actor" to actor, "method" to method, "path" to path, "body" to body))
        @Suppress("UNCHECKED_CAST")
        val data = reply["data"] as? Map<String, Any?> ?: emptyMap()
        val status = (reply["status"] as? Long ?: 0L).toInt()
        if (status >= 400 || data["ok"] == false) throw VaultException(data["error"] as? String ?: "http_$status")
        data
    }).apply {
        newMemoryKiB = VaultKdf.FLOOR_MEMORY_KIB
        newPasses = VaultKdf.FLOOR_PASSES
    }

    fun stop() {
        runCatching { input.close() }
        process.destroy()
        process.waitFor()
    }

    companion object {
        fun start(): VaultBridge? {
            var dir: File? = File("").absoluteFile
            while (dir != null && !File(dir, "marketplace/packages/vault/tests/vault_stdio_server.py").isFile) dir = dir.parentFile
            dir ?: return null
            val builder = ProcessBuilder("python3", File(dir, "marketplace/packages/vault/tests/vault_stdio_server.py").path)
            builder.environment().remove("FORKMESH_HOME")
            builder.environment().remove("FORKMESH_PROJECTS_DIR")
            builder.redirectError(ProcessBuilder.Redirect.DISCARD)
            return try {
                VaultBridge(builder.start())
            } catch (_: Exception) {
                null
            }
        }
    }
}

fun refused(code: String, block: () -> Unit) {
    try {
        block()
    } catch (error: VaultException) {
        assertEq(code, error.code)
        return
    }
    throw AssertionError("accepted; expected $code")
}

class VaultClientFlowTest {
    fun testTheWholeVaultFlowAgainstTheRealWorkerModule() {
        val bridge = VaultBridge.start() ?: run {
            println("SKIP the vault stdio bridge is not reachable")
            return
        }
        try {
            flow(bridge)
        } finally {
            bridge.stop()
        }
    }

    private fun flow(bridge: VaultBridge) {
        val alice = bridge.client("alice")
        val bob = bridge.client("bob")
        val carol = bridge.client("carol")

        alice.status()
        expect(!alice.installed)
        val recoveryCode = alice.setUp("alice master password")
        expect(Regex("^([0-9A-Z]{4}-){12}[0-9A-Z]{4}$").matches(recoveryCode))
        bob.setUp("bob master password")
        refused("vault_exists") { bridge.client("alice").setUp("x".repeat(12)) }

        val loginId = alice.save(
            mapOf("title" to "Registrar", "username" to "ops@acme.test", "password" to "s3cret!", "url" to "https://registrar.example/login"),
        )
        val bankId = alice.save(mapOf("title" to "Bank", "password" to "only-mine", "url" to "bank.example"))
        refused("title_required") { alice.save(mapOf("title" to "  ")) }
        alice.load()
        assertEq("s3cret!", alice.find(loginId)?.password)
        assertEq(listOf(loginId), alice.matching("registrar.example").map { it.id })
        assertEq(listOf(bankId), alice.matching("www.bank.example").map { it.id })
        expect(alice.matching("evil.example").isEmpty())
        expect(alice.matching("sub.registrar.example").isEmpty())

        val elsewhere = bridge.client("alice")
        refused("wrong_password") { elsewhere.unlock("not the password") }
        elsewhere.unlock("alice master password")
        assertEq(2, elsewhere.load().size)
        val resumed = bridge.client("alice")
        resumed.resume(elsewhere.exportKeyset())
        assertEq(2, resumed.load().size)
        refused("keyset_mismatch") { bridge.client("bob").resume(elsewhere.exportKeyset()) }

        val dump = Json.stringify(bridge.control("dump")["rows"])
        for (secret in listOf("s3cret!", "only-mine", "Registrar", "registrar.example", "master password")) {
            expect(!dump.contains(secret), "the database never sees $secret")
        }

        val preview = alice.previewShare("user", "bob")
        assertEq(listOf("new"), preview.map { it.state })
        assertEq(bob.myFingerprint(), preview.first().fingerprint)
        refused("recipient_has_no_vault") { alice.share(loginId, "user", "carol") }
        refused("recipient_not_found") { alice.share(loginId, "user", "nobody") }
        assertEq(listOf("bob"), alice.share(loginId, "user", "bob").sealedTo)
        bob.load()
        assertEq(1, bob.items.size)
        assertEq("s3cret!", bob.items.first().password)
        assertEq(false, bob.items.first().canEdit)
        refused("read_only") { bob.save(bob.find(loginId)!!.fields + ("password" to "x"), loginId) }
        assertEq(listOf("pinned"), alice.previewShare("user", "bob").map { it.state })

        alice.load()
        alice.share(loginId, "user", "bob", permission = "write")
        bob.load()
        bob.save(bob.find(loginId)!!.fields + ("password" to "rotated-by-bob"), loginId)
        alice.load()
        assertEq("rotated-by-bob", alice.find(loginId)?.password)
        assertEq("bob", alice.find(loginId)?.updatedBy)
        refused("owner_required") { bob.share(loginId, "user", "alice") }

        val org = alice.share(bankId, "organization", "acme")
        assertEq(listOf("bob"), org.sealedTo)
        assertEq(listOf("carol"), org.withoutVault)
        refused("organization_not_found") { alice.share(bankId, "organization", "globex") }
        carol.setUp("carol master password")
        expect(carol.load().isEmpty())
        alice.load()
        assertEq(1, alice.syncPending())
        assertEq(0, alice.syncPending())
        assertEq("acme", carol.load().firstOrNull()?.viaName)
        bridge.control("leave", mapOf("organization" to "acme", "member" to "bob"))
        bob.load()
        expect(bob.find(bankId) == null)
        expect(bob.find(loginId) != null)

        alice.unshare(loginId, "user", "bob")
        alice.load()
        assertEq("rotated-by-bob", alice.find(loginId)?.password)
        assertEq(emptyList<VaultShare>(), alice.find(loginId)?.shares)
        expect(bob.load().isEmpty())
        alice.share(bankId, "user", "bob")
        alice.unshare(bankId, "user", "bob")
        assertEq("only-mine", carol.load().firstOrNull()?.password)

        @Suppress("UNCHECKED_CAST")
        val honest = (bridge.send(mapOf("actor" to "alice", "method" to "GET", "path" to "/api/vault/users/bob", "body" to null))["data"] as Map<String, Any?>)["bundle"]
        val impostor = VaultBundle.of(VaultKeyset.generate(), "bob").toJson()
        bridge.control("set_bundle", mapOf("member" to "bob", "bundle" to impostor))
        assertEq(listOf("changed"), alice.previewShare("user", "bob").map { it.state })
        refused("recipient_keys_changed") { alice.share(loginId, "user", "bob") }
        bridge.control("set_bundle", mapOf("member" to "bob", "bundle" to honest))
        bob.load()
        val fromBob = bob.save(mapOf("title" to "From bob", "password" to "pw"))
        bob.load()
        bob.share(fromBob, "user", "alice")
        assertEq("From bob", alice.load().firstOrNull { it.id == fromBob }?.title)
        bridge.control("set_bundle", mapOf("member" to "bob", "bundle" to impostor))
        val suspicious = alice.load().firstOrNull { it.id == fromBob }
        assertEq(false, suspicious?.readable)
        assertEq("sender_keys_changed", suspicious?.error)
        bridge.control("set_bundle", mapOf("member" to "bob", "bundle" to honest))

        alice.changePassword("alice second password")
        refused("wrong_password") { bridge.client("alice").unlock("alice master password") }
        val recovered = bridge.client("alice")
        refused("wrong_recovery_code") { recovered.recover(VaultCrypto.newRecoveryCode(), "x".repeat(12)) }
        refused("invalid_recovery_code") { recovered.recover("not a code", "x".repeat(12)) }
        val renewed = recovered.recover(recoveryCode, "alice third password")
        expect(renewed != recoveryCode)
        assertEq(3, recovered.load().size)
        refused("wrong_recovery_code") { bridge.client("alice").recover(recoveryCode, "x".repeat(12)) }

        recovered.lock()
        expect(!recovered.unlocked)
        expect(recovered.items.isEmpty())
        refused("vault_locked") { recovered.save(mapOf("title" to "x")) }
        refused("vault_locked") { recovered.exportKeyset() }
    }

    fun testFieldsAreCleanedAndCut() {
        val clean = VaultFields.clean(
            mapOf("title" to "  Bank  ", "url" to " bank.example ", "password" to "p".repeat(5000), "extra" to "dropped"),
        )
        assertEq(listOf("title", "username", "password", "url", "notes"), clean.keys.toList())
        assertEq("Bank", clean["title"])
        assertEq("bank.example", clean["url"])
        assertEq("", clean["username"])
        assertEq(4096, (clean["password"] as String).length)
        refused("title_required") { VaultFields.clean(mapOf("title" to " ")) }
        assertEq("null", dartString(null))
        assertEq("3", dartString(3L))
    }
}
