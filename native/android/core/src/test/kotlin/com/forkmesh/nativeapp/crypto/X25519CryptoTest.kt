package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertHex
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect

class X25519CryptoTest {
    fun testRfc7748ScalarMultiplication() {
        assertHex(
            "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552",
            X25519.scalarMult(
                bytesOfHex("a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4"),
                bytesOfHex("e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c"),
            ),
        )
        assertHex(
            "95cbde9476e8907d7aade45cb4b873f88b595a68799fa152e6f8f7647aac7957",
            X25519.scalarMult(
                bytesOfHex("4b66e9d4d1b4673c5ad22691957d6af5c11b6421e0ea01d42ca4169e7918ba0d"),
                bytesOfHex("e5210f12786811d3f4b7959d0538ae2c31dbe7106fc03c3efc4cd549c715a493"),
            ),
        )
    }

    fun testRfc7748Iterations() {
        var k = ByteArray(32).also { it[0] = 9 }
        var u = k.copyOf()
        for (i in 1..1000) {
            val result = X25519.scalarMult(k, u)
            u = k
            k = result
            if (i == 1) assertHex("422c8e7a6227d7bca1350b3e2bb7279f7897b87bb6854b783c60e80311ae3079", k)
        }
        assertHex("684cf59ba83309552800ef566f2f4d3c1c3887c49360e3875f2eb94d99532c51", k)
    }

    fun testRfc7748DiffieHellman() {
        val alice = bytesOfHex("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a")
        val bob = bytesOfHex("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb")
        val alicePublic = X25519.publicKey(alice)
        val bobPublic = X25519.publicKey(bob)
        assertHex("8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a", alicePublic)
        assertHex("de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f", bobPublic)
        val shared = "4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742"
        assertHex(shared, X25519.sharedSecret(alice, bobPublic))
        assertHex(shared, X25519.sharedSecret(bob, alicePublic))
    }

    fun testClampingMatchesDart() {
        val seed = ByteArray(32) { -1 }
        val clamped = X25519.clampPrivateKey(seed)
        assertHex("f8" + "ff".repeat(30) + "7f", clamped)
        assertBytes(X25519.publicKey(seed), X25519.publicKey(clamped))
        repeat(20) {
            val key = X25519.generatePrivateKey()
            expect((key[0].toInt() and 7) == 0 && (key[31].toInt() and 0xC0) == 0x40)
        }
    }

    fun testHighBitOfPeerKeyIsIgnoredAndSmallOrderYieldsZero() {
        val k = X25519.generatePrivateKey()
        val u = X25519.publicKey(X25519.generatePrivateKey())
        val high = u.copyOf().also { it[31] = (it[31].toInt() or 0x80).toByte() }
        assertBytes(X25519.sharedSecret(k, u), X25519.sharedSecret(k, high))
        expect(Bytes.isAllZero(X25519.sharedSecret(k, ByteArray(32))))
        expect(Bytes.isAllZero(X25519.sharedSecret(k, ByteArray(32).also { it[0] = 1 })))
    }
}
