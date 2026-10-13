package com.forkmesh.nativeapp.vault

import com.forkmesh.nativeapp.crypto.MlKem768
import com.forkmesh.nativeapp.crypto.Sha256
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.int
import com.forkmesh.nativeapp.json.maps
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.resourceText

class VaultCryptoTest {
    private val openssl = Json.parseObject(resourceText("vault/openssl-vectors.json"))

    fun testBlake2bKnownAnswers() {
        assertEq(
            "786a02f742015903c6c6fd852552d272912f4740e15847618a86e217f71f5419d25e1031afee585313896444934eb04b903a685b1448b755d56f701afe9be2ce",
            VaultCrypto.toHex(Blake2b.hash(64)),
        )
        assertEq(
            "ba80a53f981c4d0d6a2797b69f12f6e94c212f14685ac4b74b12bb6fdbffa2d17d87c5392aab792dc252d5de4533cc9518d38aa8dbf1925ab92386edd4009923",
            VaultCrypto.toHex(Blake2b.hash(64, "abc".toByteArray())),
        )
        val long = ByteArray(300) { it.toByte() }
        val split = Blake2b(32).update(long, 0, 128).update(long, 128, 172).digest()
        assertEq(VaultCrypto.toHex(Blake2b.hash(32, long)), VaultCrypto.toHex(split))
    }

    fun testArgon2idRfc9106Vector() {
        val tag = Argon2id.deriveKey(
            password = ByteArray(32) { 1 },
            salt = ByteArray(16) { 2 },
            parallelism = 4,
            memoryKiB = 32,
            iterations = 3,
            hashLength = 32,
            secret = ByteArray(8) { 3 },
            associatedData = ByteArray(12) { 4 },
        )
        assertEq("0d640df58d78766c08c037a34a8b53c9d01ef0452d75b65eb52520e96b01e659", VaultCrypto.toHex(tag))
    }

    fun testArgon2idAgainstOpenSSL() {
        var sawDefault = false
        val fresh = VaultKdf.fresh()
        for (v in openssl.maps("argon2id")) {
            val key = Argon2id.deriveKey(
                password = v.str("password").toByteArray(Charsets.UTF_8),
                salt = VaultCrypto.fromHex(v.str("salt"))!!,
                parallelism = v.int("parallelism"),
                memoryKiB = v.int("memoryKiB"),
                iterations = v.int("passes"),
                hashLength = v.int("length"),
            )
            assertEq(v.str("key"), VaultCrypto.toHex(key), "m=${v.int("memoryKiB")} t=${v.int("passes")} p=${v.int("parallelism")}")
            if (v.int("memoryKiB") == fresh.memoryKiB && v.int("passes") == fresh.passes && v.int("parallelism") == fresh.parallelism) {
                sawDefault = true
            }
        }
        expect(sawDefault, "the default stretch is one of the vectors")
    }

    fun testMlKemAgainstOpenSSL() {
        val vectors = openssl.maps("mlkem768")
        expect(vectors.isNotEmpty())
        for (v in vectors) {
            val pair = MlKem768.generateKeyPair(VaultCrypto.fromHex(v.str("seed"))!!)
            assertEq(v.str("publicKey"), VaultCrypto.toHex(pair.publicKey))
            assertEq(v.str("secretKeySha256"), VaultCrypto.toHex(Sha256.hash(pair.secretKey)))
            assertEq(v.str("sharedSecret"), VaultCrypto.toHex(MlKem768.decapsulate(pair.secretKey, VaultCrypto.fromHex(v.str("ciphertext"))!!)))
            assertEq(v.str("rejectionSecret"), VaultCrypto.toHex(MlKem768.decapsulate(pair.secretKey, VaultCrypto.fromHex(v.str("tamperedCiphertext"))!!)))
            val unreduced = pair.publicKey.copyOf()
            unreduced[0] = 0xff.toByte()
            unreduced[1] = (unreduced[1].toInt() or 0x0f).toByte()
            expect(VaultBundle.mlkemKeyReduced(pair.publicKey))
            expect(!VaultBundle.mlkemKeyReduced(unreduced))
        }
    }

    fun testWeakOrMalformedStretchSettingsAreRefused() {
        val good = VaultKdf.fresh().toJson()
        assertEq(65536L, good["memoryKiB"])
        assertEq(3L, good["passes"])
        expect(Json.stringify(good).startsWith("{\"algorithm\":\"argon2id\",\"version\":19,\"memoryKiB\":65536,\"passes\":3,\"parallelism\":1,\"salt\":\""))
        VaultKdf.fromJson(good)
        val weak = listOf<Pair<String, Any?>>(
            "memoryKiB" to 1024L,
            "passes" to 1L,
            "algorithm" to "pbkdf2",
            "version" to 16L,
            "salt" to "AAAA",
            "memoryKiB" to 65536.5,
            "memoryKiB" to 65536.0,
            "parallelism" to 0L,
            "passes" to 17L,
            "salt" to "not base64!",
        )
        for ((key, value) in weak) {
            val kdf = LinkedHashMap(good)
            kdf[key] = value
            expectCode("unsafe_kdf_parameters") { VaultKdf.fromJson(kdf) }
        }
        val asDouble = LinkedHashMap(good)
        asDouble["version"] = 19.0
        VaultKdf.fromJson(asDouble)
    }

    fun testANonAsciiPasswordIsNeverStretchedUnNormalized() {
        expectCode("normalization_unavailable") { VaultCrypto.stretch("päss", VaultKdf.fresh(), normalizer = null) }
    }

    fun testStretchingNormalizesUnicode() {
        val kdf = VaultKdf.fresh(VaultKdf.FLOOR_MEMORY_KIB, VaultKdf.FLOOR_PASSES)
        val composed = VaultCrypto.stretch("paßword Ω", kdf)
        assertEq(VaultCrypto.toHex(composed), VaultCrypto.toHex(VaultCrypto.stretch("paßword Ω", kdf)))
        assertEq("Å", VaultCrypto.NFKC("Å"))
    }

    fun testEveryBindingABlobCarriesIsEnforced() {
        val alice = VaultKeyset.generate()
        val bob = VaultKeyset.generate()
        val aliceBundle = VaultBundle.of(alice, "alice")
        val bobBundle = VaultBundle.of(bob, "bob")
        val id = VaultCrypto.newItemId()
        val otherId = VaultCrypto.newItemId()
        val itemKey = VaultCrypto.randomBytes(32)

        val body = VaultCrypto.sealItem(itemKey, id, mapOf("title" to "t", "password" to "p"))
        assertEq("p", VaultCrypto.openItem(itemKey, id, body)["password"])
        assertEq(body.length, VaultCrypto.sealItem(itemKey, id, mapOf("title" to "t", "password" to "p".repeat(60))).length)
        expectCode("decryption_failed") { VaultCrypto.openItem(itemKey, otherId, body) }
        val flipped = VaultCrypto.fromBase64(body)
        flipped[flipped.size - 1] = (flipped[flipped.size - 1].toInt() xor 1).toByte()
        expectCode("decryption_failed") { VaultCrypto.openItem(itemKey, id, VaultCrypto.toBase64(flipped)) }
        expectCode("invalid_item_id") { VaultCrypto.openItem(itemKey, "ABC", body) }
        expectCode("invalid_encoding") { VaultCrypto.openItem(itemKey, id, "a+b/") }
        expectCode("invalid_ciphertext") { VaultCrypto.openItem(itemKey, id, "AAAA") }
        expectCode("invalid_key") { VaultCrypto.openItem(byteArrayOf(1, 2, 3), id, body) }

        val wrapped = VaultCrypto.wrapItemKey(alice, id, itemKey)
        expectCode("decryption_failed") { VaultCrypto.unwrapItemKey(alice, otherId, wrapped) }
        expectCode("decryption_failed") { VaultCrypto.unwrapItemKey(bob, id, wrapped) }

        val grant = VaultCrypto.sealGrant(alice, "alice", bobBundle, id, itemKey, "read")
        assertEq(listOf("v", "suite", "x25519", "mlkem768", "wrapped", "signature"), grant.keys.toList())
        expect(VaultCrypto.openGrant(bob, "bob", aliceBundle, id, grant, "read").contentEquals(itemKey))
        expectCode("invalid_permission") { VaultCrypto.sealGrant(alice, "alice", bobBundle, id, itemKey, "admin") }
        expectCode("invalid_share") { VaultCrypto.sealGrant(alice, "alice", bobBundle, id, byteArrayOf(1), "read") }
        expectCode("invalid_share") { VaultCrypto.openGrant(bob, "bob", aliceBundle, id, mapOf("v" to 2L), "read") }
        expect(aliceBundle.fingerprint() != bobBundle.fingerprint())
        expect(Regex("^([0-9a-f]{4} ){15}[0-9a-f]{4}$").matches(aliceBundle.fingerprint()))

        val kdf = VaultKdf.fresh(VaultKdf.FLOOR_MEMORY_KIB, VaultKdf.FLOOR_PASSES)
        val masterKey = VaultCrypto.stretch("paßword Ω", kdf)
        val blob = VaultCrypto.wrapKeyset(masterKey, "master", "Alice ", alice)
        expect(VaultCrypto.unwrapKeyset(masterKey, "master", "alice", blob).vaultKey.contentEquals(alice.vaultKey))
        expectCode("wrong_password") { VaultCrypto.unwrapKeyset(masterKey, "master", "bob", blob) }
        expectCode("wrong_password") { VaultCrypto.unwrapKeyset(masterKey, "recovery", "alice", blob) }
        expectCode("wrong_password") { VaultCrypto.unwrapKeyset(masterKey, "master", "alice", "!!") }
        expectCode("invalid_account") { VaultCrypto.unwrapKeyset(masterKey, "master", "-x", blob) }
        val kek = VaultCrypto.hkdf(masterKey, "keyset-kek|master")
        val future = VaultCrypto.seal(kek, "keyset|master|alice", "{\"v\":2}".toByteArray())
        expectCode("unsupported_keyset") { VaultCrypto.unwrapKeyset(masterKey, "master", "alice", future) }
        val short = VaultCrypto.seal(kek, "keyset|master|alice", "{\"v\":1,\"vaultKey\":\"AA\",\"x25519\":\"AA\",\"mlkemSeed\":\"AA\",\"ed25519\":\"AA\"}".toByteArray())
        expectCode("invalid_keyset") { VaultCrypto.unwrapKeyset(masterKey, "master", "alice", short) }
    }

    fun testKeysetJsonRoundTripsAndWipes() {
        val keyset = VaultKeyset.generate()
        val json = keyset.toJson()
        assertEq(listOf("v", "vaultKey", "x25519", "mlkemSeed", "ed25519"), json.keys.toList())
        assertEq(json, VaultKeyset.fromJson(json).toJson())
        expectCode("unsupported_keyset") { VaultKeyset.fromJson(null) }
        keyset.mlkem
        keyset.wipe()
        expect((keyset.vaultKey + keyset.x25519 + keyset.mlkemSeed + keyset.ed25519).all { it.toInt() == 0 })
    }

    fun testGenerator() {
        val seen = HashSet<String>()
        repeat(200) {
            val password = VaultCrypto.generatePassword(length = 16)
            assertEq(16, password.length)
            expect(password.any { it in 'a'..'z' })
            expect(password.any { it in 'A'..'Z' })
            expect(password.any { it in '0'..'9' })
            expect(password.any { !(it in 'a'..'z' || it in 'A'..'Z' || it in '0'..'9') })
            expect(password.none { it in "lIO01" })
            seen.add(password)
        }
        assertEq(200, seen.size)
        assertEq(24, VaultCrypto.generatePassword().length)
        assertEq(8, VaultCrypto.generatePassword(length = 2).length)
        assertEq(128, VaultCrypto.generatePassword(length = 500).length)
        val digits = VaultCrypto.generatePassword(length = 8, lower = false, upper = false, symbols = false)
        expect(digits.all { it in "23456789" })
        expectCode("no_character_classes") { VaultCrypto.generatePassword(lower = false, upper = false, digits = false, symbols = false) }
    }

    fun testRecoveryCodes() {
        val code = VaultCrypto.newRecoveryCode()
        expect(Regex("^([0-9A-Z]{4}-){12}[0-9A-Z]{4}$").matches(code), code)
        assertEq(32, VaultCrypto.recoverySecret(code).size)
        val loose = code.lowercase().replace("-", " ").replace("0", "o").replace("1", "l")
        expect(VaultCrypto.recoverySecret(loose).contentEquals(VaultCrypto.recoverySecret(code)))
        assertEq("ZZZZ-".repeat(12) + "ZZZG", VaultCrypto.recoveryCode(ByteArray(32) { -1 }))
        assertEq("0000-".repeat(12) + "0000", VaultCrypto.recoveryCode(ByteArray(32)))
        expectCode("invalid_recovery_code") { VaultCrypto.recoverySecret("not a code") }
        expectCode("invalid_recovery_code") { VaultCrypto.recoverySecret(code.dropLast(1)) }
        expectCode("invalid_recovery_code") { VaultCrypto.recoverySecret(code.dropLast(1) + "U") }
    }

    fun testSiteMatching() {
        assertEq("bank.example", VaultCrypto.loginHost("https://www.Bank.Example/x"))
        assertEq("bank.example", VaultCrypto.loginHost("bank.example:8443/x"))
        assertEq("", VaultCrypto.loginHost("http://bank.example"))
        assertEq("localhost", VaultCrypto.loginHost("http://localhost:3000/a"))
        assertEq("::1", VaultCrypto.loginHost("http://[::1]:3000/a"))
        assertEq("registrar.example", VaultCrypto.loginHost("HTTPS://Registrar.Example"))
        for (bad in listOf("javascript:alert(1)", "data:text/html,x", "ftp://x.y", "", "   ", "file:///etc/passwd")) {
            assertEq("", VaultCrypto.loginHref(bad), bad)
        }
        assertEq("https://registrar.example", VaultCrypto.loginHref("registrar.example"))
        assertEq("https://registrar.example/Login", VaultCrypto.loginHref(" https://Registrar.Example/Login "))
        val saved = "https://registrar.example/login"
        expect(VaultCrypto.matchesHost(saved, "registrar.example"))
        expect(VaultCrypto.matchesHost(saved, "www.registrar.example"))
        for (other in listOf("evil.example", "registrar.example.evil.test", "sub.registrar.example", "notregistrar.example", "")) {
            expect(!VaultCrypto.matchesHost(saved, other), other)
        }
        expect(!VaultCrypto.matchesHost("", ""))
    }

    fun testEncodings() {
        assertEq("-_8", VaultCrypto.toBase64(byteArrayOf(0xfb.toByte(), 0xff.toByte())))
        expect(VaultCrypto.fromBase64("-_8").contentEquals(byteArrayOf(0xfb.toByte(), 0xff.toByte())))
        assertEq(0, VaultCrypto.fromBase64("").size)
        expectCode("invalid_encoding") { VaultCrypto.fromBase64("+/8") }
        expectCode("invalid_encoding") { VaultCrypto.fromBase64("AAAAA") }
        expectCode("invalid_encoding") { VaultCrypto.fromBase64("AA==") }
        assertEq(0, VaultCrypto.fromBase64(1L).size)
        expect(VaultCrypto.sameBytes(byteArrayOf(1, 2), byteArrayOf(1, 2)))
        expect(!VaultCrypto.sameBytes(byteArrayOf(1, 2), byteArrayOf(1, 3)))
        assertEq("wrong password", VaultCryptoException("wrong_password").toString())
    }
}
