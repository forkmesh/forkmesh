package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.assertHex
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import java.util.Random

class PrimitivesCryptoTest {
    fun testBase64EncodesLikeDart() {
        assertEq("", B64.encode(ByteArray(0)))
        assertEq("Zg==", B64.encode("f".toByteArray()))
        assertEq("Zm8=", B64.encode("fo".toByteArray()))
        assertEq("Zm9v", B64.encode("foo".toByteArray()))
        val high = byteArrayOf(0xfb.toByte(), 0xff.toByte(), 0xbf.toByte())
        assertEq("+/+/", B64.encode(high))
        assertEq("-_-_", B64.encodeUrl(high))
        assertEq("-_8=", B64.encodeUrl(high.copyOf(2)))
        assertEq("-_8", B64.encodeUrlNoPadding(high.copyOf(2)))
        val random = Random(7)
        for (size in 0..70) {
            val bytes = ByteArray(size).also(random::nextBytes)
            assertEq(java.util.Base64.getEncoder().encodeToString(bytes), B64.encode(bytes))
            assertEq(java.util.Base64.getUrlEncoder().withoutPadding().encodeToString(bytes), B64.encodeUrlNoPadding(bytes))
            assertBytes(bytes, B64.decode(B64.encode(bytes)))
            assertBytes(bytes, B64.decode(B64.encodeUrl(bytes)))
        }
    }

    fun testBase64DecodesWithDartRules() {
        val foob = "foob".toByteArray()
        assertBytes(foob, B64.decode("Zm9vYg=="))
        assertBytes(foob, B64.decode("Zm9vYg%3D%3D"))
        assertBytes(foob, B64.decode("Zm9vYg%3d="))
        assertBytes("fooba".toByteArray(), B64.decode("Zm9vYmE="))
        assertBytes(B64.decode("+/+/"), B64.decode("-_-_"))
        assertBytes(B64.decode("+/+/"), B64.decode("-/+_"))
        assertEq(0, B64.decode("").size)
        for (bad in listOf(
            "Zm9vYg",
            "Zm9vYg=",
            "Zm9vYh==",
            "Zm9vYmF=",
            "Zm9v Yg==",
            "Zm9vYg==x",
            "Zm9vYg===",
            "Z===",
            "Zm9vé",
            "Zm9vYg%3D",
        )) {
            assertFails<IllegalArgumentException> { B64.decode(bad) }
            assertEq(null, B64.decodeOrNull(bad), bad)
        }
    }

    fun testUtf8MatchesDart() {
        assertHex("efbfbd", Utf8.encode("\uD800"))
        assertHex("61efbfbd62", Utf8.encode("a\uDC00b"))
        assertHex("f09f9880", Utf8.encode("😀"))
        assertHex("c3a9", Utf8.encode("é"))
        expect(!"\uD800".toByteArray(Charsets.UTF_8).contentEquals(Utf8.encode("\uD800")), "JDK encodes lone surrogates differently")
        assertEq("é😀", Utf8.decode(bytesOfHex("c3a9f09f9880")))
        for (bad in listOf("c0af", "eda080", "e282", "ff", "f4908080")) {
            assertFails<IllegalArgumentException> { Utf8.decode(bytesOfHex(bad)) }
        }
        val random = Random(11)
        repeat(200) {
            val text = buildString { repeat(random.nextInt(20)) { appendCodePoint(random.nextInt(0x10FFFF).let { if (it in 0xD800..0xDFFF) 0x41 else it }) } }
            assertBytes(text.toByteArray(Charsets.UTF_8), Utf8.encode(text))
            assertEq(text, Utf8.decode(Utf8.encode(text)))
        }
    }

    fun testDartTrim() {
        assertEq("room", dartTrim(" \t\n\u00A0\u2003room\u3000\uFEFF\u0085"))
        assertEq("\u001Croom\u001C", dartTrim("\u001Croom\u001C"))
        assertEq("", dartTrim(" \u2028 "))
    }

    fun testSha2() {
        assertHex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", Sha256.hash("abc".toByteArray()))
        assertEq("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", Sha256.hex("a".toByteArray(), "bc".toByteArray()))
        assertHex(
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
            Sha512.hash("abc".toByteArray()),
        )
    }

    fun testHmacSha256() {
        assertHex(
            "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843",
            HmacSha256.mac("Jefe".toByteArray(), "what do ya want for nothing?".toByteArray()),
        )
        assertHex("e528c4d99e6177f5841f712a143b90843299a4aa181a06501422d9ca862bd2a5", HmacSha256.mac(ByteArray(0), "data".toByteArray()))
    }

    fun testHkdfSha256() {
        assertHex(
            "3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865",
            Hkdf.sha256(ByteArray(22) { 0x0b }, ByteArray(13) { it.toByte() }, ByteArray(10) { (0xf0 + it).toByte() }, 42),
        )
        assertHex("8da4e775a563c18f715f802a063c5a31b8a11f5c5ee1879ec3454e5f3c738d2d", Hkdf.sha256(ByteArray(22) { 0x0b }, ByteArray(0), ByteArray(0), 32))
    }

    fun testPbkdf2HmacSha256() {
        val salt16 = "saltsaltsaltsalt".toByteArray()
        assertHex(
            "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783",
            Pbkdf2.hmacSha256("passwd".toByteArray(), "salt".toByteArray(), 1, 64),
        )
        assertHex(
            "4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56a1d425a1225833549adb841b51c9b3176a272bdebba1d078478f62b397f33c8d",
            Pbkdf2.hmacSha256("Password".toByteArray(), "NaCl".toByteArray(), 80000, 64),
        )
        assertHex("c75fbc54c506766c5456c7738010488ab4cf91311ff441fb0d994dfb78a7d830", Pbkdf2.hmacSha256(Utf8.encode("pässwörd-😀"), salt16, 1000, 32))
        assertHex("b88f67b6a828cb87ec03d16d8243f678544a017f2526f0e5e5b60a83369dde09", Pbkdf2.hmacSha256(ByteArray(0), salt16, 1000, 32))
        val password = "correct horse battery staple".toByteArray()
        assertBytes(Pbkdf2.hmacSha256Portable(password, salt16, 2000, 32), Pbkdf2.hmacSha256(password, salt16, 2000, 32))
    }

    fun testAesGcm() {
        val key = ByteArray(32) { it.toByte() }
        val nonce = ByteArray(12) { (100 + it).toByte() }
        val box = AesGcm.encrypt("forkmesh aes-gcm vector".toByteArray(), key, nonce, "aad bytes".toByteArray())
        assertHex("2e74ac0d148c25f61e033a9bf702099062b46369ff0381", box.cipherText)
        assertHex("9747de6ef4c6f54b48950fa3d372cad9", box.mac)
        assertBytes(nonce, box.nonce)
        assertEq("forkmesh aes-gcm vector", String(AesGcm.decrypt(box.cipherText, nonce, box.mac, key, "aad bytes".toByteArray())))
        val empty = AesGcm.encrypt(ByteArray(0), key, nonce)
        assertEq(0, empty.cipherText.size)
        assertHex("609bd50f6ad76df665f048ba8c5fd706", empty.mac)

        assertEq(null, AesGcm.decryptOrNull(box.cipherText, nonce, box.mac, key, "aad byteZ".toByteArray()))
        assertEq(null, AesGcm.decryptOrNull(box.cipherText, nonce, box.mac, key))
        val tamperedTag = box.mac.copyOf().also { it[0] = (it[0].toInt() xor 1).toByte() }
        assertFails<AesGcm.AuthenticationException> { AesGcm.decrypt(box.cipherText, nonce, tamperedTag, key, "aad bytes".toByteArray()) }
        assertFails<AesGcm.AuthenticationException> { AesGcm.decrypt(box.cipherText, nonce, box.mac.copyOf(12), key, "aad bytes".toByteArray()) }
        val random = AesGcm.encrypt("x".toByteArray(), key)
        expect(!random.nonce.contentEquals(AesGcm.encrypt("x".toByteArray(), key).nonce), "fresh nonces")
    }

    fun testConstantTimeHelpers() {
        expect(Bytes.constantTimeEquals(byteArrayOf(1, 2, 3), byteArrayOf(1, 2, 3)))
        expect(!Bytes.constantTimeEquals(byteArrayOf(1, 2, 3), byteArrayOf(1, 2, 4)))
        expect(!Bytes.constantTimeEquals(byteArrayOf(1, 2, 3), byteArrayOf(1, 2)))
        expect(Bytes.isAllZero(ByteArray(32)))
        expect(!Bytes.isAllZero(ByteArray(32).also { it[31] = -128 }))
        assertEq("00ff7f80", Bytes.hex(byteArrayOf(0, -1, 127, -128)))
        assertBytes(byteArrayOf(0, -1, 127, -128), Bytes.fromHex("00FF7f80"))
        expect(dartNumEquals(1L, 1) && dartNumEquals(1, 1) && dartNumEquals(1.0, 1) && !dartNumEquals("1", 1) && !dartNumEquals(1.5, 1))
    }
}
