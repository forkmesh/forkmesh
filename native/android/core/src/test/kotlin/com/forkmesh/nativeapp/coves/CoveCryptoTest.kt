package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import com.forkmesh.nativeapp.testing.hexOf

class CoveCryptoTest {
    private val salt = ByteArray(16) { it.toByte() }

    fun testKeyDerivationMatchesDesktopOpenSSL() {
        assertEq("747aefd163ba4d3a6596ebc653329c342a4c9e7c1df2394d30ae4d05df8e000e", hexOf(CoveCrypto.deriveKey("correct horse battery staple", salt, 2000)))
        assertEq("c19d4443fbe043672ccf91e9076f9942ef6ea17637be65bdd27bda3dc961a047", hexOf(CoveCrypto.deriveKey("pässwörd 🔒", salt, 1000)))
    }

    fun testEncryptDecryptRoundTripAndTamper() {
        val crypto = CoveCrypto.withPassword("pw", salt, 10)
        val cipher = crypto.encrypt("hello cove".toByteArray())
        assertEq(12, CoveBase64.decode(cipher.nonce).size)
        assertEq(16, CoveBase64.decode(cipher.tag).size)
        assertEq("hello cove", crypto.decrypt(cipher)?.let { String(it) })
        assertEq(null, CoveCrypto.withPassword("pW", salt, 10).decrypt(cipher))
        val body = CoveBase64.decode(cipher.body)
        body[0] = (body[0].toInt() xor 1).toByte()
        assertEq(null, crypto.decrypt(cipher.copy(body = CoveBase64.encode(body))))
    }

    fun testRejectsBadInputsLikeDesktop() {
        assertFails<CoveCryptoException> { CoveCrypto.withPassword("", salt, 1) }
        assertFails<CoveCryptoException> { CoveCrypto.withPassword("x", ByteArray(0), 1) }
        assertFails<CoveCryptoException> { CoveCrypto.withPassword("x", salt, 0) }
        assertFails<CoveCryptoException> { CoveCrypto.withKey(byteArrayOf(1, 2, 3)) }
        val crypto = CoveCrypto.withKey(ByteArray(32))
        assertFails<CoveCryptoException> { crypto.encrypt(ByteArray(CoveCrypto.MAX_PLAIN_BYTES + 1)) }
        val empty = crypto.encrypt(ByteArray(0))
        assertEq(null, crypto.decrypt(empty))
        assertEq(null, crypto.decrypt(CoveCipher("AAAA", empty.tag, "AA==")))
    }
}
