package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.assertHex
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.hexOf
import com.forkmesh.nativeapp.testing.resourceText

class MlKem768CryptoTest {
    fun testAccumulatedKnownAnswer() {
        assertEq("1114b1b6699ed191734fa339376afa7e285c9e6acf6ff0177d346696ce564415", accumulated(100))
        if (System.getenv("FORKMESH_CRYPTO_SLOW_KAT") == "1") {
            assertEq("8a518cc63da366322a8e7a818c7a0d63483cb3528d34a4cf42f35d5ad73f22fc", accumulated(10000))
        } else {
            println("       note: set FORKMESH_CRYPTO_SLOW_KAT=1 to also run the 10000-iteration accumulated vector")
        }
    }

    fun testOpenSslKnownAnswers() {
        val kat = resourceText("crypto/mlkem768-openssl-kat.txt").lines()
            .filter { it.contains('=') && !it.startsWith("#") }
            .associate { it.substringBefore('=') to it.substringAfter('=') }
        val pair = MlKem768.generateKeyPair(bytesOfHex(kat.getValue("seed")))
        assertHex(kat.getValue("ek_sha256"), Sha256.hash(pair.publicKey))
        assertHex(kat.getValue("dk_sha256"), Sha256.hash(pair.secretKey))
        assertHex(kat.getValue("shared_secret"), MlKem768.decapsulate(pair.secretKey, bytesOfHex(kat.getValue("ciphertext"))))
        assertHex(kat.getValue("rejection_secret"), MlKem768.decapsulate(pair.secretKey, bytesOfHex(kat.getValue("tampered_ciphertext"))))
    }

    fun testRoundTripAndImplicitRejection() {
        val pair = MlKem768.generateKeyPair()
        assertEq(MlKem768.PUBLIC_KEY_BYTES, pair.publicKey.size)
        assertEq(MlKem768.SECRET_KEY_BYTES, pair.secretKey.size)
        repeat(5) {
            val encapsulation = MlKem768.encapsulate(pair.publicKey)
            assertEq(MlKem768.CIPHERTEXT_BYTES, encapsulation.ciphertext.size)
            assertBytes(encapsulation.sharedSecret, MlKem768.decapsulate(pair.secretKey, encapsulation.ciphertext))
            val z = pair.secretKey.copyOfRange(MlKem768.SECRET_KEY_BYTES - 32, MlKem768.SECRET_KEY_BYTES)
            for ((index, flip) in listOf(1000 to 0x04, 0 to 0x80, 1087 to 0x80, 500 to 0x01)) {
                val tampered = encapsulation.ciphertext.copyOf().also { it[index] = (it[index].toInt() xor flip).toByte() }
                val rejected = MlKem768.decapsulate(pair.secretKey, tampered)
                expect(!rejected.contentEquals(encapsulation.sharedSecret), "byte $index xor $flip")
                assertBytes(Keccak.shake256(32, z, tampered), rejected)
            }
        }
        val other = MlKem768.generateKeyPair()
        val encapsulation = MlKem768.encapsulate(pair.publicKey)
        expect(!MlKem768.decapsulate(other.secretKey, encapsulation.ciphertext).contentEquals(encapsulation.sharedSecret))
    }

    fun testMalformedInputsThrowLikePqcrypto() {
        val pair = MlKem768.generateKeyPair(ByteArray(64) { 3 })
        val badKey = pair.publicKey.copyOf().also { it[0] = -1; it[1] = (it[1].toInt() or 0x0f).toByte() }
        assertFails<IllegalArgumentException> { MlKem768.encapsulate(badKey) }
        assertFails<IllegalArgumentException> { MlKem768.encapsulate(pair.publicKey.copyOf(1183)) }
        assertFails<IllegalArgumentException> { MlKem768.decapsulate(pair.secretKey, ByteArray(1087)) }
        val badHash = pair.secretKey.copyOf().also { it[1152 + 1184] = (it[1152 + 1184].toInt() xor 1).toByte() }
        assertFails<IllegalArgumentException> { MlKem768.decapsulate(badHash, ByteArray(MlKem768.CIPHERTEXT_BYTES)) }
        assertFails<IllegalArgumentException> { MlKem768.generateKeyPair(ByteArray(32)) }
        assertEq(hexOf(pair.publicKey), hexOf(MlKem768.generateKeyPair(ByteArray(64) { 3 }).publicKey))
    }

    private fun accumulated(iterations: Int): String {
        val source = Keccak.shake128()
        val output = Keccak.shake128()
        val seed = ByteArray(MlKem768.SEED_BYTES)
        val message = ByteArray(32)
        val randomCiphertext = ByteArray(MlKem768.CIPHERTEXT_BYTES)
        repeat(iterations) {
            source.squeeze(seed)
            val pair = MlKem768.generateKeyPair(seed)
            output.update(pair.publicKey)
            source.squeeze(message)
            val encapsulation = MlKem768.encapsulate(pair.publicKey, message)
            output.update(encapsulation.ciphertext)
            output.update(encapsulation.sharedSecret)
            assertBytes(encapsulation.sharedSecret, MlKem768.decapsulate(pair.secretKey, encapsulation.ciphertext))
            source.squeeze(randomCiphertext)
            output.update(MlKem768.decapsulate(pair.secretKey, randomCiphertext))
        }
        return hexOf(output.digest(32))
    }
}
