package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.hexOf
import java.util.Random

class OpenSslInteropCryptoTest {
    private val ed25519Pkcs8 = bytesOfHex("302e020100300506032b657004220420")
    private val x25519Pkcs8 = bytesOfHex("302e020100300506032b656e04220420")

    private fun openssl(vararg args: String): CryptoInterop.Result? = CryptoInterop.run("openssl", *args)

    private fun ok(result: CryptoInterop.Result?, what: String) {
        expect(result != null && result.exit == 0, "openssl $what failed: ${result?.stderr}")
    }

    private fun supports(algorithm: String): Boolean {
        val version = openssl("version")
        if (version == null || version.exit != 0) {
            println("       skip: no openssl on PATH")
            return false
        }
        val listing = if (algorithm == "ML-KEM-768") openssl("list", "-kem-algorithms") else openssl("list", "-public-key-algorithms")
        val supported = listing != null && listing.exit == 0 && listing.text.uppercase().contains(algorithm)
        if (!supported) println("       skip: ${version.text.trim()} lacks $algorithm")
        else println("       interop: ${version.text.trim()} $algorithm")
        return supported
    }

    fun testEd25519MatchesOpenSsl() {
        if (!supports("ED25519")) return
        val random = Random(2026)
        CryptoInterop.withFiles("ed25519-key.der", "ed25519-pub.der", "ed25519-msg.bin", "ed25519-openssl.sig", "ed25519-kotlin.sig") { (key, pub, msg, opensslSig, kotlinSig) ->
            for (size in listOf(1, 64, 1000, 4096)) {
                val seed = ByteArray(32).also(random::nextBytes)
                val message = ByteArray(size).also(random::nextBytes)
                key.writeBytes(ed25519Pkcs8 + seed)
                msg.writeBytes(message)
                ok(openssl("pkey", "-inform", "DER", "-in", key.path, "-pubout", "-outform", "DER", "-out", pub.path), "pkey")
                val publicKey = Ed25519.publicKey(seed)
                assertBytes(pub.readBytes().takeLast(32).toByteArray(), publicKey)
                ok(openssl("pkeyutl", "-sign", "-inkey", key.path, "-keyform", "DER", "-rawin", "-in", msg.path, "-out", opensslSig.path), "sign")
                val signature = Ed25519.sign(seed, message)
                assertBytes(opensslSig.readBytes(), signature)
                expect(Ed25519.verifyStrict(publicKey, message, opensslSig.readBytes()))
                kotlinSig.writeBytes(signature)
                ok(openssl("pkeyutl", "-verify", "-pubin", "-inkey", pub.path, "-keyform", "DER", "-rawin", "-in", msg.path, "-sigfile", kotlinSig.path), "verify")
                kotlinSig.writeBytes(signature.copyOf().also { it[40] = (it[40].toInt() xor 1).toByte() })
                val rejected = openssl("pkeyutl", "-verify", "-pubin", "-inkey", pub.path, "-keyform", "DER", "-rawin", "-in", msg.path, "-sigfile", kotlinSig.path)
                expect(rejected != null && rejected.exit != 0, "openssl must reject a tampered signature")
            }
        }
    }

    fun testX25519MatchesOpenSsl() {
        if (!supports("X25519")) return
        val random = Random(7748)
        CryptoInterop.withFiles("x25519-a.der", "x25519-b.der", "x25519-b-pub.der", "x25519-shared.bin") { (aKey, bKey, bPub, shared) ->
            repeat(5) {
                val a = ByteArray(32).also(random::nextBytes)
                val b = ByteArray(32).also(random::nextBytes)
                aKey.writeBytes(x25519Pkcs8 + a)
                bKey.writeBytes(x25519Pkcs8 + b)
                ok(openssl("pkey", "-inform", "DER", "-in", bKey.path, "-pubout", "-outform", "DER", "-out", bPub.path), "pkey")
                assertBytes(bPub.readBytes().takeLast(32).toByteArray(), X25519.publicKey(b))
                ok(openssl("pkeyutl", "-derive", "-inkey", aKey.path, "-keyform", "DER", "-peerkey", bPub.path, "-peerform", "DER", "-out", shared.path), "derive")
                assertBytes(shared.readBytes(), X25519.sharedSecret(a, X25519.publicKey(b)))
            }
        }
    }

    fun testMlKem768MatchesOpenSsl() {
        if (!supports("ML-KEM-768")) return
        val random = Random(203)
        CryptoInterop.withFiles("mlkem-key.der", "mlkem-pub.der", "mlkem-openssl.ct", "mlkem-openssl.ss", "mlkem-kotlin.ct", "mlkem-kotlin.ss") { files ->
            val (key, pub, opensslCt, opensslSs, kotlinCt) = files
            val kotlinSs = files[5]
            repeat(3) {
                val seed = ByteArray(MlKem768.SEED_BYTES).also(random::nextBytes)
                ok(openssl("genpkey", "-algorithm", "ML-KEM-768", "-pkeyopt", "hexseed:${hexOf(seed)}", "-outform", "DER", "-out", key.path), "genpkey")
                ok(openssl("pkey", "-inform", "DER", "-in", key.path, "-pubout", "-outform", "DER", "-out", pub.path), "pkey")
                val pair = MlKem768.generateKeyPair(seed)
                assertBytes(pub.readBytes().takeLast(MlKem768.PUBLIC_KEY_BYTES).toByteArray(), pair.publicKey)
                assertBytes(key.readBytes().takeLast(MlKem768.SECRET_KEY_BYTES).toByteArray(), pair.secretKey)

                ok(openssl("pkeyutl", "-encap", "-pubin", "-inkey", pub.path, "-keyform", "DER", "-out", opensslCt.path, "-secret", opensslSs.path), "encap")
                assertBytes(opensslSs.readBytes(), MlKem768.decapsulate(pair.secretKey, opensslCt.readBytes()))

                val encapsulation = MlKem768.encapsulate(pair.publicKey)
                kotlinCt.writeBytes(encapsulation.ciphertext)
                ok(openssl("pkeyutl", "-decap", "-inkey", key.path, "-keyform", "DER", "-in", kotlinCt.path, "-secret", kotlinSs.path), "decap")
                assertBytes(encapsulation.sharedSecret, kotlinSs.readBytes())

                val tampered = encapsulation.ciphertext.copyOf().also { it[it.size - 1] = (it[it.size - 1].toInt() xor 0x80).toByte() }
                kotlinCt.writeBytes(tampered)
                ok(openssl("pkeyutl", "-decap", "-inkey", key.path, "-keyform", "DER", "-in", kotlinCt.path, "-secret", kotlinSs.path), "decap (rejection)")
                assertBytes(kotlinSs.readBytes(), MlKem768.decapsulate(pair.secretKey, tampered))
            }
        }
    }

    fun testKdfsMatchOpenSsl() {
        val version = openssl("version")
        if (version == null || version.exit != 0) {
            println("       skip: no openssl on PATH")
            return
        }
        val random = Random(5869)
        repeat(4) { round ->
            val ikm = ByteArray(40 + round).also(random::nextBytes)
            val salt = ByteArray(16 + round).also(random::nextBytes)
            val info = ByteArray(20 + round).also(random::nextBytes)
            val result = openssl(
                "kdf", "-keylen", "48", "-kdfopt", "digest:SHA256", "-kdfopt", "hexkey:${hexOf(ikm)}",
                "-kdfopt", "hexsalt:${hexOf(salt)}", "-kdfopt", "hexinfo:${hexOf(info)}", "HKDF",
            )
            if (result == null || result.exit != 0) {
                println("       skip: openssl kdf HKDF unavailable")
                return
            }
            assertEq(result.text.trim().replace(":", "").lowercase(), hexOf(Hkdf.sha256(ikm, salt, info, 48)))
        }
        for (password in listOf("correct horse battery staple".toByteArray(), Utf8.encode("pässwörd 😀"), byteArrayOf(0x41, 0))) {
            val salt = ByteArray(16).also(random::nextBytes)
            val result = openssl(
                "kdf", "-keylen", "32", "-kdfopt", "digest:SHA256", "-kdfopt", "hexpass:${hexOf(password)}",
                "-kdfopt", "hexsalt:${hexOf(salt)}", "-kdfopt", "iter:2048", "PBKDF2",
            )
            ok(result, "kdf PBKDF2")
            assertEq(result!!.text.trim().replace(":", "").lowercase(), hexOf(Pbkdf2.hmacSha256(password, salt, 2048, 32)))
        }
    }
}
