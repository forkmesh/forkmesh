package com.forkmesh.nativeapp.qr

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.bytesOfHex
import com.forkmesh.nativeapp.testing.expect
import com.forkmesh.nativeapp.testing.hexOf
import com.forkmesh.nativeapp.testing.resourceText

class QrCodeTest {
    fun testMatchesTheDesktopEncoderBitForBit() {
        val lines = resourceText("qr/desktop_golden.txt").lines().filter { it.isNotBlank() }
        assertEq(8, lines.size)
        for (line in lines) {
            val (eclName, hex, sizeText, bits) = line.split(' ')
            val ecl = QrCode.Ecl.entries.first { it.name.startsWith(eclName) }
            val matrix = QrCode.encode(bytesOfHex(hex), ecl) ?: error("no matrix for $hex")
            assertEq(sizeText.toInt(), matrix.size, hex)
            val actual = StringBuilder()
            for (y in 0 until matrix.size) for (x in 0 until matrix.size) actual.append(if (matrix[x, y]) '1' else '0')
            assertEq(bits, actual.toString(), "$eclName ${hex.take(24)}")
        }
    }

    fun testReedSolomonMatchesTheStandardExample() {
        val data = bytesOfHex("20 5b 0b 78 d1 72 dc 4d 43 40 ec 11 ec 11 ec 11".replace(" ", "")).map { it.toInt() and 0xff }.toIntArray()
        val ecc = QrCode.rsRemainder(data, QrCode.rsDivisor(10))
        assertEq("c4232777ebd7e7e25d17", hexOf(ByteArray(ecc.size) { ecc[it].toByte() }))
    }

    fun testTooMuchDataReturnsNull() {
        assertEq(null, QrCode.encode(ByteArray(3000), QrCode.Ecl.High))
        expect(QrCode.encodeText("solana:So11111111111111111111111111111111111111112") != null)
    }
}
