package com.forkmesh.nativeapp.crypto

import com.forkmesh.nativeapp.testing.assertBytes
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertHex
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.hexOf
import java.math.BigInteger
import java.util.Random

class Ed25519CryptoTest {
    private val rfc8032 = listOf(
        arrayOf(
            "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
            "",
            "d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a",
            "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b",
        ),
        arrayOf(
            "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
            "72",
            "3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c",
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00",
        ),
        arrayOf(
            "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7",
            "af82",
            "fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025",
            "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a",
        ),
        arrayOf(
            "833fe62409237b9d62ec77587520911e9a759cec1d19755b7da901b96dca3d42",
            "ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423643ce80e2a9ac94fa54ca49f",
            "ec172b93ad5e563bf4932c70e1245034c35467ef2efd4d64ebf819683467e2bf",
            "dc2a4459e7369633a52b1bf277839a00201009a3efbf3ecb69bea2186c26b58909351fc9ac90b3ecfdfbc7c66431e0303dca179c138ac17ad9bef1177331a704",
        ),
    )

    private val p: BigInteger = BigInteger.ONE.shiftLeft(255).subtract(BigInteger.valueOf(19))
    private val l: BigInteger = BigInteger.ONE.shiftLeft(252).add(BigInteger("27742317777372353535851937790883648493"))

    fun testRfc8032Vectors() {
        for ((seedHex, messageHex, publicHex, signatureHex) in rfc8032) {
            val seed = bytesOfHex(seedHex)
            val message = bytesOfHex(messageHex)
            assertHex(publicHex, Ed25519.publicKey(seed))
            assertHex(signatureHex, Ed25519.sign(seed, message))
            assertHex(signatureHex, Ed25519.sign(seed, message, bytesOfHex(publicHex)))
            expect(Ed25519.verify(bytesOfHex(publicHex), message, bytesOfHex(signatureHex)))
            expect(Ed25519.verifyStrict(bytesOfHex(publicHex), message, bytesOfHex(signatureHex)))
        }
    }

    fun testSignaturesVerifyAndTamperingFails() {
        val random = Random(1234)
        repeat(60) { round ->
            val seed = ByteArray(32).also(random::nextBytes)
            val message = ByteArray(round * 7).also(random::nextBytes)
            val publicKey = Ed25519.publicKey(seed)
            val signature = Ed25519.sign(seed, message)
            expect(Ed25519.verify(publicKey, message, signature))
            expect(Ed25519.verifyStrict(publicKey, message, signature))
            val badSignature = signature.copyOf().also {
                val index = random.nextInt(64)
                it[index] = (it[index].toInt() xor (1 shl random.nextInt(8))).toByte()
            }
            expect(!Ed25519.verify(publicKey, message, badSignature), "tampered signature $round")
            if (message.isNotEmpty()) {
                val badMessage = message.copyOf().also { it[0] = (it[0].toInt() xor 1).toByte() }
                expect(!Ed25519.verify(publicKey, badMessage, signature), "tampered message $round")
            }
            expect(!Ed25519.verify(Ed25519.publicKey(ByteArray(32).also(random::nextBytes)), message, signature))
            expect(!Ed25519.verify(publicKey, message, signature.copyOf(63)))
        }
    }

    fun testNonCanonicalScalarIsRejected() {
        val seed = ByteArray(32) { 5 }
        val message = "malleable".toByteArray()
        val publicKey = Ed25519.publicKey(seed)
        val signature = Ed25519.sign(seed, message)
        val s = littleEndian(signature.copyOfRange(32, 64))
        val forged = signature.copyOfRange(0, 32) + toLittleEndian(s.add(l), 32)
        expect(!Ed25519.verify(publicKey, message, forged))
        expect(!Ed25519.verifyStrict(publicKey, message, forged))
        expect(!Ed25519.isCanonicalScalar(toLittleEndian(l, 32)))
        expect(Ed25519.isCanonicalScalar(toLittleEndian(l.subtract(BigInteger.ONE), 32)))
    }

    fun testStrictVerificationRejectsSmallOrderKeys() {
        val identity = bytesOfHex("01" + "00".repeat(31))
        val trivial = identity + ByteArray(32)
        val message = "anything".toByteArray()
        expect(Ed25519.verify(identity, message, trivial), "RFC 8032 cofactorless verification accepts the identity key")
        expect(!Ed25519.verifyStrict(identity, message, trivial), "strict verification rejects it")
        val order2 = bytesOfHex("ec" + "ff".repeat(30) + "7f")
        expect(!Ed25519.verifyStrict(order2, message, trivial))
        val seed = ByteArray(32) { 9 }
        expect(Ed25519.verifyStrict(Ed25519.publicKey(seed), message, Ed25519.sign(seed, message)))
    }

    fun testNonCanonicalPointEncodingsAreRejected() {
        val message = "lax".toByteArray()
        for (encoding in listOf("ee" + "ff".repeat(30) + "7f", "01" + "00".repeat(30) + "80")) {
            val key = bytesOfHex(encoding)
            expect(!Ed25519.verify(key, message, bytesOfHex("01" + "00".repeat(31)) + ByteArray(32)), encoding)
            expect(!Ed25519.verify(bytesOfHex("01" + "00".repeat(31)), message, key + ByteArray(32)), "non-canonical R $encoding")
        }
        expect(!Ed25519.verify(bytesOfHex("02" + "00".repeat(31)), message, ByteArray(64)))
    }

    fun testFieldArithmeticMatchesBigInteger() {
        val random = Random(99)
        val edges = listOf(
            BigInteger.ZERO, BigInteger.ONE, p.subtract(BigInteger.ONE), p.subtract(BigInteger.valueOf(2)),
            BigInteger.ONE.shiftLeft(255).subtract(BigInteger.ONE), p, p.add(BigInteger.ONE), BigInteger.ONE.shiftLeft(254),
        )
        val samples = edges + List(300) { BigInteger(255, random) }
        val a = Fe.create()
        val b = Fe.create()
        val h = Fe.create()
        for ((index, x) in samples.withIndex()) {
            val y = samples[(index * 7 + 3) % samples.size]
            Fe.fromBytes(a, toLittleEndian(x, 32))
            Fe.fromBytes(b, toLittleEndian(y, 32))
            val xm = x.mod(p)
            val ym = y.mod(p)
            Fe.mul(h, a, b); checkFe(xm.multiply(ym).mod(p), h)
            Fe.add(h, a, b); checkFe(xm.add(ym).mod(p), h)
            Fe.sub(h, a, b); checkFe(xm.subtract(ym).mod(p), h)
            Fe.sq(h, a); checkFe(xm.multiply(xm).mod(p), h)
            Fe.neg(h, a); checkFe(xm.negate().mod(p), h)
            Fe.mulSmall(h, a, 121665); checkFe(xm.multiply(BigInteger.valueOf(121665)).mod(p), h)
            if (xm.signum() != 0) {
                Fe.invert(h, a); checkFe(xm.modInverse(p), h)
            }
            Fe.pow22523(h, a); checkFe(xm.modPow(BigInteger.ONE.shiftLeft(252).subtract(BigInteger.valueOf(3)), p), h)
        }
        var bx = BigInteger(255, random).mod(p)
        val by = BigInteger(255, random).mod(p)
        Fe.fromBytes(a, toLittleEndian(bx, 32))
        Fe.fromBytes(b, toLittleEndian(by, 32))
        val t = Fe.create()
        repeat(2000) {
            Fe.add(t, a, b)
            Fe.sub(h, a, b)
            Fe.mul(a, t, h)
            Fe.add(a, a, a)
            bx = bx.add(by).multiply(bx.subtract(by)).shiftLeft(1).mod(p)
        }
        checkFe(bx, a)
    }

    fun testScalarReductionMatchesBigInteger() {
        val random = Random(5)
        val wides = mutableListOf(ByteArray(64), ByteArray(64) { -1 }, toLittleEndian(l, 64), toLittleEndian(l.subtract(BigInteger.ONE), 64), toLittleEndian(l.shiftLeft(1), 64))
        repeat(500) { wides.add(ByteArray(64).also(random::nextBytes)) }
        for (wide in wides) {
            assertEq(hexOf(toLittleEndian(littleEndian(wide).mod(l), 32)), hexOf(Ed25519.reduce(wide)))
        }
        repeat(500) {
            val k = toLittleEndian(BigInteger(253, random).mod(l), 32)
            val r = toLittleEndian(BigInteger(253, random).mod(l), 32)
            val a = ByteArray(32).also(random::nextBytes)
            a[0] = (a[0].toInt() and 248).toByte()
            a[31] = ((a[31].toInt() and 127) or 64).toByte()
            val expected = littleEndian(r).add(littleEndian(k).multiply(littleEndian(a))).mod(l)
            assertBytes(toLittleEndian(expected, 32), Ed25519.mulAdd(k, a, r))
        }
    }

    fun testCurveConstants() {
        val d = BigInteger.valueOf(-121665).multiply(BigInteger.valueOf(121666).modInverse(p)).mod(p)
        val probe = Fe.create()
        Fe.fromBytes(probe, toLittleEndian(d, 32))
        val encoded = ByteArray(32)
        Fe.toBytes(encoded, probe)
        assertEq("a3785913ca4deb75abd841414d0a700098e879777940c78c73fe6f2bee6c0352", hexOf(encoded))
        assertEq(hexOf(toLittleEndian(BigInteger.TWO.modPow(p.subtract(BigInteger.ONE).shiftRight(2), p), 32)), "b0a00e4a271beec478e42fad0618432fa7d7fb3d99004d2b0bdfc14f8024832b")
    }

    private fun checkFe(expected: BigInteger, h: LongArray) {
        val out = ByteArray(32)
        Fe.toBytes(out, h)
        assertEq(hexOf(toLittleEndian(expected, 32)), hexOf(out))
    }

    private fun littleEndian(bytes: ByteArray): BigInteger = BigInteger(1, bytes.reversedArray())

    private fun toLittleEndian(value: BigInteger, size: Int): ByteArray {
        val big = value.toByteArray()
        val out = ByteArray(size)
        for (i in big.indices) {
            val index = big.size - 1 - i
            if (i < size) out[i] = big[index]
        }
        return out
    }
}
