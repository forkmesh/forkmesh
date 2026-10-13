package com.forkmesh.nativeapp.qr

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.cos
import kotlin.math.sin
import kotlin.random.Random

class QrReaderTest {
    private fun grid(matrix: QrMatrix): Array<BooleanArray> = Array(matrix.size) { y -> BooleanArray(matrix.size) { x -> matrix[x, y] } }

    private fun render(matrix: QrMatrix, scale: Int, quiet: Int = 4): LuminanceImage {
        val side = (matrix.size + quiet * 2) * scale
        val pixels = IntArray(side * side) { 235 }
        for (y in 0 until matrix.size) for (x in 0 until matrix.size) {
            if (!matrix[x, y]) continue
            for (dy in 0 until scale) for (dx in 0 until scale) {
                pixels[((y + quiet) * scale + dy) * side + (x + quiet) * scale + dx] = 25
            }
        }
        return LuminanceImage(side, side, pixels)
    }

    private fun rotate(src: LuminanceImage, degrees: Double, canvas: Int, shiftX: Int = 0, shiftY: Int = 0): LuminanceImage {
        val out = IntArray(canvas * canvas)
        val r = Math.toRadians(degrees)
        val c = cos(r)
        val s = sin(r)
        val cx = canvas / 2.0 + shiftX
        val cy = canvas / 2.0 + shiftY
        for (y in 0 until canvas) for (x in 0 until canvas) {
            val dx = x - cx
            val dy = y - cy
            val sx = c * dx + s * dy + src.width / 2.0
            val sy = -s * dx + c * dy + src.height / 2.0
            val x0 = kotlin.math.floor(sx).toInt()
            val y0 = kotlin.math.floor(sy).toInt()
            if (x0 < 0 || y0 < 0 || x0 + 1 >= src.width || y0 + 1 >= src.height) {
                out[y * canvas + x] = 235
                continue
            }
            val fx = sx - x0
            val fy = sy - y0
            val v = src[x0, y0] * (1 - fx) * (1 - fy) + src[x0 + 1, y0] * fx * (1 - fy) + src[x0, y0 + 1] * (1 - fx) * fy + src[x0 + 1, y0 + 1] * fx * fy
            out[y * canvas + x] = v.toInt()
        }
        return LuminanceImage(canvas, canvas, out)
    }

    private fun noisy(src: LuminanceImage, seed: Int, amplitude: Int, gradient: Boolean = false): LuminanceImage {
        val random = Random(seed)
        val out = IntArray(src.width * src.height) {
            val x = it % src.width
            val shade = if (gradient) (x * 60 / src.width) else 0
            (src.pixels[it] - shade + random.nextInt(-amplitude, amplitude + 1)).coerceIn(0, 255)
        }
        return LuminanceImage(src.width, src.height, out)
    }

    fun testDecodesTheEncoderAtEveryLevelAndSeveralVersions() {
        val texts = listOf(
            "forkmesh://link-node?code=ABCD-1234",
            "solana:So11111111111111111111111111111111111111112",
            "https://app.forkmesh.com/@forkmesh/node/mirror35?link=" + "x".repeat(180),
            "héllo wörld ✓ " + "0123456789".repeat(30),
        )
        for (text in texts) for (ecl in QrCode.Ecl.entries) {
            val matrix = QrCode.encodeText(text, ecl) ?: continue
            assertEq(text, QrDecoder.decode(grid(matrix)), "${ecl.name} ${matrix.size}")
        }
    }

    fun testCorrectsDamagedModules() {
        val text = "forkmesh://link-node?code=" + "Q".repeat(40)
        val matrix = QrCode.encodeText(text, QrCode.Ecl.High)!!
        val g = grid(matrix)
        val random = Random(7)
        val function = QrCode.Matrix((matrix.size - 17) / 4).apply { drawFunctionPatterns(0) }.isFunction
        var flipped = 0
        while (flipped < 18) {
            val x = random.nextInt(matrix.size)
            val y = random.nextInt(matrix.size)
            if (function[y][x]) continue
            g[y][x] = !g[y][x]
            flipped++
        }
        assertEq(text, QrDecoder.decode(g))
    }

    fun testReedSolomonFixesUpToHalfTheEccAndRejectsMore() {
        val data = IntArray(20) { (it * 37 + 11) and 0xFF }
        val ecc = QrCode.rsRemainder(data, QrCode.rsDivisor(10))
        val good = data + ecc
        val five = good.copyOf()
        for (i in intArrayOf(0, 4, 9, 17, 29)) five[i] = five[i] xor (i + 1)
        expect(QrDecoder.correct(five, 10))
        assertEq(good.toList(), five.toList())
        val six = good.copyOf()
        for (i in intArrayOf(0, 3, 6, 12, 19, 25)) six[i] = six[i] xor 0x5A
        expect(!QrDecoder.correct(six, 10) || !six.contentEquals(good))
    }

    fun testReadsARenderedImage() {
        val text = "forkmesh://link-node?code=7H2K-99QX&relay=app.forkmesh.com"
        val matrix = QrCode.encodeText(text)!!
        assertEq(text, QrReader.read(render(matrix, 6)))
    }

    fun testReadsRotatedShiftedAndNoisyImages() {
        val text = "forkmesh://link-node?code=7H2K-99QX&relay=app.forkmesh.com"
        val matrix = QrCode.encodeText(text)!!
        val base = render(matrix, 7)
        for ((i, angle) in listOf(90.0, 180.0, 270.0, 17.0, -33.0, 45.0).withIndex()) {
            val image = noisy(rotate(base, angle, base.width + 160, shiftX = 20 * (i % 3) - 20, shiftY = -15), seed = i, amplitude = 45, gradient = i % 2 == 0)
            assertEq(text, QrReader.read(image), "angle $angle")
        }
    }

    fun testReadsALargerVersionWithAlignmentAndVersionInfo() {
        val text = "https://app.forkmesh.com/" + "abcdefghij".repeat(22)
        val matrix = QrCode.encodeText(text, QrCode.Ecl.Medium)!!
        expect(matrix.size >= 45, "version 7 or above")
        val image = noisy(rotate(render(matrix, 5), 12.0, matrix.size * 5 + 160), seed = 3, amplitude = 30)
        assertEq(text, QrReader.read(image))
    }

    fun testReadsLightOnDark() {
        val text = "forkmesh://link-node?code=INVERTED"
        val src = render(QrCode.encodeText(text)!!, 6)
        val inverted = LuminanceImage(src.width, src.height, IntArray(src.pixels.size) { 255 - src.pixels[it] })
        assertEq(text, QrReader.read(inverted))
    }

    fun testBlankAndRandomImagesReadNothing() {
        assertEq(null, QrReader.read(LuminanceImage(200, 200, IntArray(40_000) { 200 })))
        val random = Random(1)
        assertEq(null, QrReader.read(LuminanceImage(200, 200, IntArray(40_000) { if (random.nextBoolean()) 20 else 230 })))
    }
}
