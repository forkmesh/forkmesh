package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.security.MemorySecretStore
import com.forkmesh.nativeapp.security.OwnerCrypto

class TimingCryptoTest {
    private fun time(label: String, iterations: Int, block: () -> Unit) {
        repeat(maxOf(2, iterations / 4)) { block() }
        val started = System.nanoTime()
        repeat(iterations) { block() }
        val micros = (System.nanoTime() - started) / 1000.0 / iterations
        println("       timing: %-42s %11.1f us/op".format(label, micros))
    }

    fun testPrintTimings() {
        val seed = ByteArray(32) { 1 }
        val publicKey = Ed25519.publicKey(seed)
        val message = ByteArray(1024) { it.toByte() }
        val signature = Ed25519.sign(seed, message, publicKey)
        time("Ed25519 sign, 1 KiB, known public key", 500) { Ed25519.sign(seed, message, publicKey) }
        time("Ed25519 sign, 1 KiB, derive public key", 300) { Ed25519.sign(seed, message) }
        time("Ed25519 verify, 1 KiB", 500) { check(Ed25519.verify(publicKey, message, signature)) }
        time("Ed25519 verifyStrict, 1 KiB", 200) { check(Ed25519.verifyStrict(publicKey, message, signature)) }
        val x25519Public = X25519.publicKey(seed)
        time("X25519 shared secret", 500) { X25519.sharedSecret(seed, x25519Public) }

        val pair = MlKem768.generateKeyPair()
        val encapsulation = MlKem768.encapsulate(pair.publicKey)
        time("ML-KEM-768 key generation", 100) { MlKem768.generateKeyPair() }
        time("ML-KEM-768 encapsulate", 200) { MlKem768.encapsulate(pair.publicKey) }
        time("ML-KEM-768 decapsulate", 200) { MlKem768.decapsulate(pair.secretKey, encapsulation.ciphertext) }

        val owner = OwnerCrypto.loadOrCreate(MemorySecretStore())
        val desktop = OwnerCrypto.loadOrCreate(MemorySecretStore())
        val payload = mapOf("kind" to "forkmesh.agent-session", "transcript" to "x".repeat(4096))
        val sealed = owner.sealJson(payload, listOf(owner.publicBundle, desktop.publicBundle))!!
        time("OwnerCrypto.sealJson, 4 KiB, 2 recipients", 50) { owner.sealJson(payload, listOf(owner.publicBundle, desktop.publicBundle)) }
        time("OwnerCrypto.openJson, 4 KiB", 50) { check(desktop.openJson(sealed) != null) }

        val salt = ByteArray(16) { 9 }
        time("PBKDF2-SHA256 210000 rounds (JCA factory)", 3) { Pbkdf2.hmacSha256("room passphrase".toByteArray(), salt, 210000, 32) }
        time("PBKDF2-SHA256 210000 rounds (Mac loop)", 2) { Pbkdf2.hmacSha256Portable("room passphrase".toByteArray(), salt, 210000, 32) }
    }
}
