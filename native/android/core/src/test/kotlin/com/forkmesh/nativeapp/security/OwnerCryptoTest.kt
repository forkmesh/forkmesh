package com.forkmesh.nativeapp.security

import com.forkmesh.nativeapp.crypto.AesGcm
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.CryptoInterop
import com.forkmesh.nativeapp.crypto.Hkdf
import com.forkmesh.nativeapp.crypto.MlKem768
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.crypto.X25519
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import java.util.Base64

@Suppress("UNCHECKED_CAST")
class OwnerCryptoTest {
    private val url = Base64.getUrlEncoder().withoutPadding()

    private fun qtBinary(): File? {
        val configured = System.getenv("FORKMESH_QT_TESTS_BINARY")?.let(::File)
        val binary = configured ?: CryptoInterop.repoRoot?.let { File(it, "desktop/build/forkmesh-tests") }
        if (binary == null || !binary.canExecute()) {
            println("       skip: desktop/build/forkmesh-tests is not built (run ./desktop/run.sh test or set FORKMESH_QT_TESTS_BINARY)")
            return null
        }
        return binary
    }

    private fun sealByQt(binary: File, bundle: Map<String, Any?>): Map<String, Any?> {
        val result = CryptoInterop.run(binary.absolutePath, "--seal-owner-envelope-to", bundle["x25519"] as String, bundle["mlkem768"] as String)
        expect(result != null && result.exit == 0, "Qt seal failed: ${result?.stderr}")
        return Json.parse(result!!.text) as Map<String, Any?>
    }

    fun testThisDeviceOpensASessionSnapshotTheQtNodeSealedToIt() {
        val binary = qtBinary() ?: return
        println("       interop: ${binary.path}")
        val owner = OwnerCrypto.loadOrCreate(MemorySecretStore())
        val payload = sealByQt(binary, owner.publicBundle)
        val envelope = payload["envelope"] as Map<String, Any?>

        assertEq(listOf(payload["desktopKeyId"], payload["companionKeyId"]), OwnerCrypto.recipientKeyIds(envelope))
        assertEq(owner.keyId, payload["companionKeyId"])
        expect(owner.isRecipientOf(envelope))

        val opened = owner.open(envelope)
        expect(opened != null)
        assertBytes(Base64.getDecoder().decode(payload["plaintext"] as String), opened!!)

        val json = owner.openJson(envelope)
        assertEq("forkmesh.agent-session", json?.get("kind"))
        expect((json?.get("transcript") as String).contains("only owner devices read"))
        expect(!Json.stringify(envelope).contains("only owner devices read"))
    }

    fun testADifferentDeviceCannotOpenTheSameEnvelope() {
        val binary = qtBinary() ?: return
        val owner = OwnerCrypto.loadOrCreate(MemorySecretStore())
        val envelope = sealByQt(binary, owner.publicBundle)["envelope"] as Map<String, Any?>
        val stranger = OwnerCrypto.loadOrCreate(MemorySecretStore())
        expect(stranger.keyId != owner.keyId)
        expect(!stranger.isRecipientOf(envelope))
        assertEq(null, stranger.open(envelope))
    }

    fun testTamperedCiphertextFailsAuthenticationInsteadOfReturningData() {
        val binary = qtBinary() ?: return
        val owner = OwnerCrypto.loadOrCreate(MemorySecretStore())
        val envelope = LinkedHashMap(sealByQt(binary, owner.publicBundle)["envelope"] as Map<String, Any?>)
        val body = B64.decode((envelope["body"] as String).let { it.padEnd((it.length + 3) and 3.inv(), '=') })
        body[0] = (body[0].toInt() xor 1).toByte()
        envelope["body"] = url.encodeToString(body)
        assertEq(null, owner.open(envelope))
    }

    fun testAPromptThisDeviceSealsIsOpenedByTheRecipientItNames() {
        val phone = OwnerCrypto.loadOrCreate(MemorySecretStore())
        val desktop = OwnerCrypto.loadOrCreate(MemorySecretStore())
        val sealed = phone.sealJson(
            mapOf("kind" to "forkmesh.agent-prompt", "text" to "tighten the retry backoff"),
            listOf(phone.publicBundle, desktop.publicBundle),
        )
        expect(sealed != null)
        assertEq(listOf(phone.keyId, desktop.keyId), OwnerCrypto.recipientKeyIds(sealed!!))
        assertEq("tighten the retry backoff", desktop.openJson(sealed)?.get("text"))
        assertEq("forkmesh.agent-prompt", phone.openJson(sealed)?.get("kind"))
        assertEq("tighten the retry backoff", desktop.openJson(Json.parse(Json.stringify(sealed)) as Map<String, Any?>)?.get("text"))
    }

    fun testEnvelopeFramingBoundsMatchTheRelayValidator() {
        val owner = OwnerCrypto.loadOrCreate(MemorySecretStore())
        val bundle = owner.publicBundle
        val bytes = byteArrayOf(1, 2, 3)

        assertEq(null, owner.seal(bytes, emptyList()))
        assertEq(null, owner.seal(bytes, List(OwnerCrypto.MAX_RECIPIENTS + 1) { bundle }))
        assertEq(null, owner.seal(bytes, listOf(bundle, bundle)))

        val sealed = owner.seal(bytes, listOf(bundle))!!
        assertEq("forkmesh.owner-sealed", sealed["kind"])
        assertEq("x25519+mlkem768/aes256gcm", sealed["alg"])
        assertEq(listOf("kind", "v", "alg", "nonce", "tag", "body", "recipients"), sealed.keys.toList())
        assertEq(listOf(owner.keyId), OwnerCrypto.recipientKeyIds(sealed))
        for (field in listOf("nonce", "tag", "body")) expect(!(sealed[field] as String).contains('='))
        val recipient = (sealed["recipients"] as List<*>).first() as Map<String, Any?>
        assertEq(listOf("kid", "x25519", "mlkem768", "nonce", "tag", "key"), recipient.keys.toList())

        fun with(key: String, value: Any?): Map<String, Any?> = LinkedHashMap(sealed).also { it[key] = value }
        assertEq(emptyList<String>(), OwnerCrypto.recipientKeyIds(with("alg", "aes")))
        assertEq(emptyList<String>(), OwnerCrypto.recipientKeyIds(with("recipients", emptyList<Any>())))
        assertEq(emptyList<String>(), OwnerCrypto.recipientKeyIds(with("recipients", listOf(recipient, recipient))))
        assertEq(emptyList<String>(), OwnerCrypto.recipientKeyIds(with("recipients", listOf("x"))))
        assertEq(emptyList<String>(), OwnerCrypto.recipientKeyIds(with("recipients", listOf(LinkedHashMap(recipient).also { it["kid"] = "short" }))))
        assertEq(emptyList<String>(), OwnerCrypto.recipientKeyIds(with("v", 2L)))
        assertEq(listOf(owner.keyId), OwnerCrypto.recipientKeyIds(with("v", 1.0)), "Dart num equality")

        assertEq(null, owner.open(with("nonce", (sealed["nonce"] as String) + "=")))
        assertEq(null, owner.open(with("tag", url.encodeToString(ByteArray(15)))))
        assertEq(null, owner.open(with("body", "")))
        assertEq(null, owner.openJson(owner.seal("not json".toByteArray(), listOf(bundle))!!))
        assertBytes(bytes, owner.open(sealed)!!)

        val badKem = ByteArray(MlKem768.PUBLIC_KEY_BYTES).also { it.fill(-1) }
        assertFails<IllegalArgumentException> {
            owner.seal(bytes, listOf(mapOf("v" to 1L, "x25519" to bundle["x25519"], "mlkem768" to url.encodeToString(badKem))))
        }
        assertEq(null, owner.seal(bytes, listOf(mapOf("v" to 2L, "x25519" to bundle["x25519"], "mlkem768" to bundle["mlkem768"]))))
    }

    fun testTheStoredIdentitySurvivesAReloadAndIsDeviceSpecific() {
        val store = MemorySecretStore()
        val first = OwnerCrypto.loadOrCreate(store)
        val reloaded = OwnerCrypto.loadOrCreate(store)
        assertEq(first.keyId, reloaded.keyId)

        val sealed = first.sealJson(mapOf("a" to 1L), listOf(first.publicBundle))!!
        assertEq(1L, reloaded.openJson(sealed)?.get("a"))

        OwnerCrypto.forget(store)
        val fresh = OwnerCrypto.loadOrCreate(store)
        expect(fresh.keyId != first.keyId)
        assertEq(null, fresh.open(sealed))

        store.write(OwnerCrypto.SEED_KEY, "{not json")
        val replaced = OwnerCrypto.loadOrCreate(store)
        expect(replaced.keyId != fresh.keyId, "an unparseable blob is replaced by a new identity")
        assertEq(replaced.keyId, OwnerCrypto.loadOrCreate(store).keyId)
    }

    fun testStorageFormatAndKeyIdStayStable() {
        val store = MemorySecretStore()
        val owner = OwnerCrypto.loadOrCreate(store)
        val raw = store.read(OwnerCrypto.SEED_KEY)!!
        val data = Json.parse(raw) as Map<String, Any?>
        assertEq(listOf("v", "x25519Pub", "x25519Priv", "mlkemPub", "mlkemPriv"), data.keys.toList())
        assertEq(1L, data["v"])
        val decoder = Base64.getDecoder()
        val xPub = decoder.decode(data["x25519Pub"] as String)
        val xPriv = decoder.decode(data["x25519Priv"] as String)
        val mPub = decoder.decode(data["mlkemPub"] as String)
        val mPriv = decoder.decode(data["mlkemPriv"] as String)
        assertEq(listOf(32, 32, 1184, 2400), listOf(xPub.size, xPriv.size, mPub.size, mPriv.size))
        expect((xPriv[0].toInt() and 7) == 0 && (xPriv[31].toInt() and 0xC0) == 0x40, "X25519 private key stored clamped")
        assertBytes(X25519.publicKey(xPriv), xPub)
        assertBytes(mPub, mPriv.copyOfRange(1152, 1152 + 1184))
        val digest = java.security.MessageDigest.getInstance("SHA-256").also { it.update(xPub); it.update(mPub) }.digest()
        assertEq(url.encodeToString(digest), owner.keyId)
        assertEq(mapOf("v" to 1L, "x25519" to url.encodeToString(xPub), "mlkem768" to url.encodeToString(mPub)), owner.publicBundle)
        assertEq("""{"v":1,"x25519":"${url.encodeToString(xPub)}","mlkem768":"${url.encodeToString(mPub)}"}""", Json.stringify(owner.publicBundle))

        val fixedX = X25519.clampPrivateKey(ByteArray(32) { 42 })
        val fixedM = MlKem768.generateKeyPair(ByteArray(64) { 7 })
        val encoder = Base64.getEncoder()
        val storedSeed = MemorySecretStore(
            mapOf(
                OwnerCrypto.SEED_KEY to """{"v":1,"x25519Pub":"${encoder.encodeToString(X25519.publicKey(fixedX))}",""" +
                    """"x25519Priv":"${encoder.encodeToString(fixedX)}","mlkemPub":"${encoder.encodeToString(fixedM.publicKey)}",""" +
                    """"mlkemPriv":"${encoder.encodeToString(fixedM.secretKey)}"}""",
            ),
        )
        val restored = OwnerCrypto.loadOrCreate(storedSeed)
        assertEq(url.encodeToString(Sha256.hash(X25519.publicKey(fixedX), fixedM.publicKey)), restored.keyId)
    }

    fun testAnEnvelopeAssembledFromTheDocumentedConstructionOpens() {
        val owner = OwnerCrypto.loadOrCreate(MemorySecretStore())
        val bundle = owner.publicBundle
        val xPub = Base64.getUrlDecoder().decode(bundle["x25519"] as String)
        val mPub = Base64.getUrlDecoder().decode(bundle["mlkem768"] as String)
        val contentKey = ByteArray(32) { (it * 3).toByte() }
        val payload = AesGcm.encrypt("""{"kind":"forkmesh.agent-session","id":42}""".toByteArray(), contentKey)

        val ephemeral = X25519.clampPrivateKey(ByteArray(32) { 99 })
        val ephemeralPublic = X25519.publicKey(ephemeral)
        val kem = MlKem768.encapsulate(mPub)
        val kek = Hkdf.sha256(
            X25519.sharedSecret(ephemeral, xPub) + kem.sharedSecret,
            "forkmesh-mirror-kem-v1".toByteArray(),
            ephemeralPublic + kem.ciphertext,
            32,
        )
        val wrap = AesGcm.encrypt(contentKey, kek)
        val envelope = mapOf(
            "kind" to "forkmesh.owner-sealed",
            "v" to 1L,
            "alg" to "x25519+mlkem768/aes256gcm",
            "nonce" to url.encodeToString(payload.nonce),
            "tag" to url.encodeToString(payload.mac),
            "body" to url.encodeToString(payload.cipherText),
            "recipients" to listOf(
                mapOf(
                    "kid" to owner.keyId,
                    "x25519" to url.encodeToString(ephemeralPublic),
                    "mlkem768" to url.encodeToString(kem.ciphertext),
                    "nonce" to url.encodeToString(wrap.nonce),
                    "tag" to url.encodeToString(wrap.mac),
                    "key" to url.encodeToString(wrap.cipherText),
                ),
            ),
        )
        assertEq(mapOf("kind" to "forkmesh.agent-session", "id" to 42L), owner.openJson(envelope))
        val otherKem = MlKem768.encapsulate(mPub)
        val mixed = envelope.toMutableMap().also {
            it["recipients"] = listOf((envelope["recipients"] as List<Map<String, Any?>>)[0].toMutableMap().also { r -> r["mlkem768"] = url.encodeToString(otherKem.ciphertext) })
        }
        assertEq(null, owner.open(mixed))
    }

    fun testSessionsSealedToThisDeviceOpenAndForeignOnesDoNot() {
        val owner = OwnerCrypto.loadOrCreate(MemorySecretStore())
        fun sessionPayload(id: Long, status: String) = mapOf("kind" to "forkmesh.agent-session", "id" to id, "status" to status, "title" to "Session $id")
        val mine = owner.sealJson(sessionPayload(42, "running"), listOf(owner.publicBundle))!!
        assertEq(sessionPayload(42, "running"), owner.openJson(mine))

        val otherDevice = OwnerCrypto.loadOrCreate(MemorySecretStore())
        val foreign = otherDevice.sealJson(sessionPayload(7, "running"), listOf(otherDevice.publicBundle))!!
        expect(!owner.isRecipientOf(foreign))
        assertEq(null, owner.openJson(foreign))
        expect(OwnerCrypto.recipientKeyIds(foreign).isNotEmpty(), "a foreign envelope is still well-formed, so it can be counted")
    }
}
