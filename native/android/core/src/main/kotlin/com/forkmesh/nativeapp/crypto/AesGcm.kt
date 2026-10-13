package com.forkmesh.nativeapp.crypto

import java.security.GeneralSecurityException
import javax.crypto.AEADBadTagException
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec

object AesGcm {
    const val KEY_BYTES = 32
    const val NONCE_BYTES = 12
    const val TAG_BYTES = 16

    private val EMPTY = ByteArray(0)

    class SecretBox(val cipherText: ByteArray, val nonce: ByteArray, val mac: ByteArray)

    class AuthenticationException : GeneralSecurityException("AES-GCM authentication failed")

    fun encrypt(
        plaintext: ByteArray,
        key: ByteArray,
        nonce: ByteArray = Bytes.random(NONCE_BYTES),
        aad: ByteArray = EMPTY,
    ): SecretBox {
        require(key.size == KEY_BYTES) { "AES-256-GCM needs a 32-byte key" }
        require(nonce.size == NONCE_BYTES) { "AES-GCM nonce must be 12 bytes" }
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG_BYTES * 8, nonce))
        if (aad.isNotEmpty()) cipher.updateAAD(aad)
        val combined = cipher.doFinal(plaintext)
        val split = combined.size - TAG_BYTES
        return SecretBox(combined.copyOfRange(0, split), nonce.copyOf(), combined.copyOfRange(split, combined.size))
    }

    fun decrypt(
        cipherText: ByteArray,
        nonce: ByteArray,
        mac: ByteArray,
        key: ByteArray,
        aad: ByteArray = EMPTY,
    ): ByteArray {
        require(key.size == KEY_BYTES) { "AES-256-GCM needs a 32-byte key" }
        if (nonce.size != NONCE_BYTES || mac.size != TAG_BYTES) throw AuthenticationException()
        val cipher = Cipher.getInstance("AES/GCM/NoPadding")
        cipher.init(Cipher.DECRYPT_MODE, SecretKeySpec(key, "AES"), GCMParameterSpec(TAG_BYTES * 8, nonce))
        if (aad.isNotEmpty()) cipher.updateAAD(aad)
        return try {
            cipher.doFinal(Bytes.concat(cipherText, mac))
        } catch (_: AEADBadTagException) {
            throw AuthenticationException()
        }
    }

    fun decryptOrNull(
        cipherText: ByteArray,
        nonce: ByteArray,
        mac: ByteArray,
        key: ByteArray,
        aad: ByteArray = EMPTY,
    ): ByteArray? = try {
        decrypt(cipherText, nonce, mac, key, aad)
    } catch (_: AuthenticationException) {
        null
    }
}
