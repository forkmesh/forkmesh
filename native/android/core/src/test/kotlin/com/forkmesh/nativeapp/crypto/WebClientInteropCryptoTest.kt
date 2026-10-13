package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.PortableIdentity
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File

class WebClientInteropCryptoTest {
    private fun webRoot(): File? {
        val root = CryptoInterop.repoRoot
        if (root == null || !File(root, "app/public/portable-identity.js").isFile) {
            println("       skip: repository app/public not found")
            return null
        }
        val version = CryptoInterop.run("node", "--version")
        if (version == null || version.exit != 0) {
            println("       skip: node not on PATH")
            return null
        }
        println("       interop: node ${version.text.trim()}")
        return root
    }

    private fun node(script: String, vararg args: String): Any? {
        val result = CryptoInterop.run("node", "--input-type=module", "-e", script, *args)
        expect(result != null && result.exit == 0, "node failed: ${result?.stderr}")
        return Json.parse(result!!.text)
    }

    fun testWebClientImportsAKotlinPortableIdentity() {
        val root = webRoot() ?: return
        val identity = linkedMapOf<String, Any?>(
            "v" to 1L,
            "kind" to "forkmesh.device-identity",
            "ed25519Seed" to B64.encodeUrlNoPadding(ByteArray(32) { 7 }),
            "label" to "Android kit ü 🚀",
        )
        val passphrase = "correct horse battery staple"
        val kit = PortableIdentity.export(identity, passphrase, createdAt = 1783000000000)
        CryptoInterop.withFiles("portable-kit.json") { (file) ->
            file.writeText(Json.stringify(kit))
            val restored = node(
                """
                import { readFileSync } from "node:fs";
                import { pathToFileURL } from "node:url";
                const [modulePath, kitPath, passphrase] = process.argv.slice(1);
                const { importPortableIdentity } = await import(pathToFileURL(modulePath).href);
                const identity = await importPortableIdentity(JSON.parse(readFileSync(kitPath, "utf8")), passphrase);
                process.stdout.write(JSON.stringify(identity));
                """.trimIndent(),
                File(root, "app/public/portable-identity.js").path,
                file.path,
                passphrase,
            )
            assertEq(identity, restored)
        }
    }

    @Suppress("UNCHECKED_CAST")
    fun testWebClientAndKotlinExchangeRoomSealFrames() {
        val root = webRoot() ?: return
        val room = "world-general"
        val browserPrivate = "UE5ZIyTdbSCtuDSqkI3g5TnXBMw1nx4Oz1BfmvE6CGc"
        val browserKey = RoomSeal.fromPrivateKey(room, B64.decode("$browserPrivate="))
        val android = RoomSeal.fromPrivateKey(room, B64.decode("cJMcO0LKgE1B6DRDn8z2mCcgt-cKyuWEtafYL2hFjHk="))
        expect(android.acceptKeyFrame(browserKey.keyFrame()))
        val plain = linkedMapOf<String, Any?>("type" to "dm", "id" to "android-seal-1", "text" to "sealed on Android é 😀")
        CryptoInterop.withFiles("seal-frame.json", "seal-android-key.json") { (frameFile, keyFile) ->
            frameFile.writeText(Json.stringify(android.seal(plain)))
            keyFile.writeText(Json.stringify(android.keyFrame()))
            val out = node(
                """
                import { readFileSync } from "node:fs";
                import { pathToFileURL } from "node:url";
                const [modulePath, framePath, keyPath, room, d, x] = process.argv.slice(1);
                const { acceptSealKey, createRoomSealStore, sealFrame, unsealFrame } = await import(pathToFileURL(modulePath).href);
                const items = new Map([["forkmesh.chat.e2ee.x25519.v2", JSON.stringify({ kty: "OKP", crv: "X25519", d, x })]]);
                const storage = {
                  getItem: (key) => items.has(key) ? items.get(key) : null,
                  setItem: (key, value) => items.set(key, String(value)),
                  removeItem: (key) => items.delete(key),
                };
                const seal = await createRoomSealStore({ storage }).resolve("scope", room);
                const opened = await unsealFrame(JSON.parse(readFileSync(framePath, "utf8")), seal);
                await acceptSealKey(JSON.parse(readFileSync(keyPath, "utf8")), seal);
                const reply = await sealFrame({ type: "dm", id: "browser-seal-1", text: "sealed in the browser" }, seal);
                process.stdout.write(JSON.stringify({ keyId: seal.keyId, opened, reply }));
                """.trimIndent(),
                File(root, "app/public/chat-seal.js").path,
                frameFile.path,
                keyFile.path,
                room,
                browserPrivate,
                browserKey.keyFrame()["pub"] as String,
            ) as Map<String, Any?>
            assertEq(browserKey.keyId, out["keyId"])
            assertEq(plain, out["opened"])
            assertEq(
                mapOf("type" to "dm", "id" to "browser-seal-1", "text" to "sealed in the browser"),
                android.unseal(out["reply"] as Map<String, Any?>),
            )
        }
    }

    fun testWebClientDecryptsKotlinRoomEnvelopes() {
        val root = webRoot() ?: return
        val plain = linkedMapOf<String, Any?>("type" to "chat", "id" to "android-1", "text" to "from Kotlin é 😀", "n" to 7L)
        val cases = listOf(
            Triple("world-general", "relay-derived-room-key", RoomCrypto.withPassphrase("world-general", "relay-derived-room-key")),
            Triple("general", "forkmesh-shared-room-key-v1", RoomCrypto.shared("general")),
        )
        for ((room, secret, crypto) in cases) {
            CryptoInterop.withFiles("room-envelope.json") { (file) ->
                file.writeText(Json.stringify(crypto.encrypt(plain)))
                val opened = node(
                    """
                    import { readFileSync } from "node:fs";
                    import { pathToFileURL } from "node:url";
                    const [modulePath, envelopePath, secret, room] = process.argv.slice(1);
                    const { derivePassphraseKey, decryptObject } = await import(pathToFileURL(modulePath).href);
                    const key = await derivePassphraseKey(secret, room);
                    process.stdout.write(JSON.stringify(await decryptObject(JSON.parse(readFileSync(envelopePath, "utf8")), key)));
                    """.trimIndent(),
                    File(root, "app/public/chat-crypto.js").path,
                    file.path,
                    secret,
                    room,
                )
                assertEq(plain, opened)
            }
        }
    }
}
