package com.forkmesh.nativeapp.qr

class QrMatrix(val size: Int, private val modules: Array<BooleanArray>) {
    operator fun get(x: Int, y: Int): Boolean = modules[y][x]

    fun rows(): List<BooleanArray> = modules.map { it.copyOf() }
}

object QrCode {
    enum class Ecl(val index: Int, val formatBits: Int) { Low(0, 1), Medium(1, 0), Quartile(2, 3), High(3, 2) }

    internal val ECC_CODEWORDS_PER_BLOCK = arrayOf(
        intArrayOf(-1, 7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30),
        intArrayOf(-1, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28),
        intArrayOf(-1, 13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30),
        intArrayOf(-1, 17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30),
    )

    internal val NUM_EC_BLOCKS = arrayOf(
        intArrayOf(-1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18, 19, 19, 20, 21, 22, 24, 25),
        intArrayOf(-1, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31, 33, 35, 37, 38, 40, 43, 45, 47, 49),
        intArrayOf(-1, 1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40, 43, 45, 48, 51, 53, 56, 59, 62, 65, 68),
        intArrayOf(-1, 1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48, 51, 54, 57, 60, 63, 66, 70, 74, 77, 81),
    )

    private fun bit(x: Long, i: Int): Boolean = ((x ushr i) and 1L) != 0L

    internal fun numRawDataModules(ver: Int): Int {
        var result = (16 * ver + 128) * ver + 64
        if (ver >= 2) {
            val numAlign = ver / 7 + 2
            result -= (25 * numAlign - 10) * numAlign - 55
            if (ver >= 7) result -= 36
        }
        return result
    }

    private fun numDataCodewords(ver: Int, ecl: Int): Int =
        numRawDataModules(ver) / 8 - ECC_CODEWORDS_PER_BLOCK[ecl][ver] * NUM_EC_BLOCKS[ecl][ver]

    internal fun rsMultiply(x: Int, y: Int): Int {
        var z = 0
        for (i in 7 downTo 0) {
            z = (z shl 1) xor ((z ushr 7) * 0x11D)
            z = z xor (((y ushr i) and 1) * x)
        }
        return z and 0xFF
    }

    internal fun rsDivisor(degree: Int): IntArray {
        val result = IntArray(degree)
        result[degree - 1] = 1
        var root = 1
        repeat(degree) {
            for (j in result.indices) {
                result[j] = rsMultiply(result[j], root)
                if (j + 1 < result.size) result[j] = result[j] xor result[j + 1]
            }
            root = rsMultiply(root, 0x02)
        }
        return result
    }

    internal fun rsRemainder(data: IntArray, divisor: IntArray): IntArray {
        val result = IntArray(divisor.size)
        for (b in data) {
            val factor = b xor result[0]
            System.arraycopy(result, 1, result, 0, result.size - 1)
            result[result.size - 1] = 0
            for (i in result.indices) result[i] = result[i] xor rsMultiply(divisor[i], factor)
        }
        return result
    }

    internal fun alignmentPositions(ver: Int): IntArray {
        if (ver == 1) return IntArray(0)
        val numAlign = ver / 7 + 2
        val step = if (ver == 32) 26 else (ver * 4 + numAlign * 2 + 1) / (numAlign * 2 - 2) * 2
        val size = ver * 4 + 17
        val result = ArrayList<Int>()
        var pos = size - 7
        for (i in 0 until numAlign - 1) {
            result.add(0, pos)
            pos -= step
        }
        result.add(0, 6)
        return result.toIntArray()
    }

    private fun addEccAndInterleave(data: IntArray, ver: Int, ecl: Int): IntArray {
        val numBlocks = NUM_EC_BLOCKS[ecl][ver]
        val blockEccLen = ECC_CODEWORDS_PER_BLOCK[ecl][ver]
        val rawCodewords = numRawDataModules(ver) / 8
        val numShortBlocks = numBlocks - rawCodewords % numBlocks
        val shortBlockLen = rawCodewords / numBlocks
        val divisor = rsDivisor(blockEccLen)
        val blocks = ArrayList<IntArray>()
        var k = 0
        for (i in 0 until numBlocks) {
            val datLen = shortBlockLen - blockEccLen + if (i < numShortBlocks) 0 else 1
            val dat = data.copyOfRange(k, k + datLen)
            k += datLen
            val ecc = rsRemainder(dat, divisor)
            val block = ArrayList<Int>(shortBlockLen + 1)
            dat.forEach { block.add(it) }
            if (i < numShortBlocks) block.add(0)
            ecc.forEach { block.add(it) }
            blocks.add(block.toIntArray())
        }
        val result = ArrayList<Int>()
        for (i in blocks[0].indices) {
            for (j in blocks.indices) {
                if (i != shortBlockLen - blockEccLen || j >= numShortBlocks) result.add(blocks[j][i])
            }
        }
        return result.toIntArray()
    }

    internal class Matrix(val ver: Int) {
        val size = ver * 4 + 17
        val modules = Array(size) { BooleanArray(size) }
        val isFunction = Array(size) { BooleanArray(size) }

        fun setFunction(x: Int, y: Int, dark: Boolean) {
            modules[y][x] = dark
            isFunction[y][x] = true
        }

        fun drawFinder(x: Int, y: Int) {
            for (dy in -4..4) {
                for (dx in -4..4) {
                    val dist = maxOf(kotlin.math.abs(dx), kotlin.math.abs(dy))
                    val xx = x + dx
                    val yy = y + dy
                    if (xx in 0 until size && yy in 0 until size) setFunction(xx, yy, dist != 2 && dist != 4)
                }
            }
        }

        fun drawAlignment(x: Int, y: Int) {
            for (dy in -2..2) for (dx in -2..2) setFunction(x + dx, y + dy, maxOf(kotlin.math.abs(dx), kotlin.math.abs(dy)) != 1)
        }

        fun drawFormatBits(eclFmt: Int, mask: Int) {
            val data = eclFmt shl 3 or mask
            var rem = data
            repeat(10) { rem = (rem shl 1) xor ((rem ushr 9) * 0x537) }
            val bits = ((data shl 10 or rem) xor 0x5412).toLong()
            for (i in 0..5) setFunction(8, i, bit(bits, i))
            setFunction(8, 7, bit(bits, 6))
            setFunction(8, 8, bit(bits, 7))
            setFunction(7, 8, bit(bits, 8))
            for (i in 9 until 15) setFunction(14 - i, 8, bit(bits, i))
            for (i in 0 until 8) setFunction(size - 1 - i, 8, bit(bits, i))
            for (i in 8 until 15) setFunction(8, size - 15 + i, bit(bits, i))
            setFunction(8, size - 8, true)
        }

        fun drawVersion() {
            if (ver < 7) return
            var rem = ver
            repeat(12) { rem = (rem shl 1) xor ((rem ushr 11) * 0x1F25) }
            val bits = (ver.toLong() shl 12) or rem.toLong()
            for (i in 0 until 18) {
                val b = bit(bits, i)
                val a = size - 11 + i % 3
                val c = i / 3
                setFunction(a, c, b)
                setFunction(c, a, b)
            }
        }

        fun drawFunctionPatterns(eclFmt: Int) {
            for (i in 0 until size) {
                setFunction(6, i, i % 2 == 0)
                setFunction(i, 6, i % 2 == 0)
            }
            drawFinder(3, 3)
            drawFinder(size - 4, 3)
            drawFinder(3, size - 4)
            val align = alignmentPositions(ver)
            val n = align.size
            for (i in 0 until n) {
                for (j in 0 until n) {
                    if ((i == 0 && j == 0) || (i == 0 && j == n - 1) || (i == n - 1 && j == 0)) continue
                    drawAlignment(align[i], align[j])
                }
            }
            drawFormatBits(eclFmt, 0)
            drawVersion()
        }

        fun drawCodewords(data: IntArray) {
            var i = 0
            var right = size - 1
            while (right >= 1) {
                if (right == 6) right = 5
                for (vert in 0 until size) {
                    for (j in 0 until 2) {
                        val x = right - j
                        val upward = ((right + 1) and 2) == 0
                        val y = if (upward) size - 1 - vert else vert
                        if (!isFunction[y][x] && i < data.size * 8) {
                            modules[y][x] = ((data[i ushr 3] ushr (7 - (i and 7))) and 1) != 0
                            i++
                        }
                    }
                }
                right -= 2
            }
        }

        fun applyMask(mask: Int) {
            for (y in 0 until size) {
                for (x in 0 until size) {
                    val invert = when (mask) {
                        0 -> (x + y) % 2 == 0
                        1 -> y % 2 == 0
                        2 -> x % 3 == 0
                        3 -> (x + y) % 3 == 0
                        4 -> (x / 3 + y / 2) % 2 == 0
                        5 -> x * y % 2 + x * y % 3 == 0
                        6 -> (x * y % 2 + x * y % 3) % 2 == 0
                        else -> ((x + y) % 2 + x * y % 3) % 2 == 0
                    }
                    if (!isFunction[y][x] && invert) modules[y][x] = !modules[y][x]
                }
            }
        }

        private fun addHistory(runIn: Int, hist: IntArray) {
            var run = runIn
            if (hist[0] == 0) run += size
            for (i in 6 downTo 1) hist[i] = hist[i - 1]
            hist[0] = run
        }

        private fun countPatterns(h: IntArray): Int {
            val n = h[1]
            val core = n > 0 && h[2] == n && h[3] == n * 3 && h[4] == n && h[5] == n
            return (if (core && h[0] >= n * 4 && h[6] >= n) 1 else 0) + (if (core && h[6] >= n * 4 && h[0] >= n) 1 else 0)
        }

        private fun terminateAndCount(runColor: Boolean, runIn: Int, hist: IntArray): Int {
            var run = runIn
            if (runColor) {
                addHistory(run, hist)
                run = 0
            }
            run += size
            addHistory(run, hist)
            return countPatterns(hist)
        }

        private fun lineScore(get: (Int) -> Boolean): Long {
            var result = 0L
            var runColor = false
            var run = 0
            val hist = IntArray(7)
            for (i in 0 until size) {
                val v = get(i)
                if (v == runColor) {
                    run++
                    if (run == 5) result += 3 else if (run > 5) result++
                } else {
                    addHistory(run, hist)
                    if (!runColor) result += countPatterns(hist) * 40L
                    runColor = v
                    run = 1
                }
            }
            result += terminateAndCount(runColor, run, hist) * 40L
            return result
        }

        fun penaltyScore(): Long {
            var result = 0L
            for (y in 0 until size) result += lineScore { x -> modules[y][x] }
            for (x in 0 until size) result += lineScore { y -> modules[y][x] }
            for (y in 0 until size - 1) {
                for (x in 0 until size - 1) {
                    val c = modules[y][x]
                    if (c == modules[y][x + 1] && c == modules[y + 1][x] && c == modules[y + 1][x + 1]) result += 3
                }
            }
            var dark = 0
            for (row in modules) for (v in row) if (v) dark++
            val total = size * size
            val k = ((kotlin.math.abs(dark * 20L - total * 10L) + total - 1) / total).toInt() - 1
            result += k.toLong() * 10
            return result
        }
    }

    fun encode(data: ByteArray, ecl: Ecl = Ecl.Medium): QrMatrix? {
        val e = ecl.index
        var ver = 0
        for (v in 1..40) {
            val capacityBits = numDataCodewords(v, e) * 8
            val ccBits = if (v <= 9) 8 else 16
            val needed = 4L + ccBits + 8L * data.size
            if (needed <= capacityBits) {
                ver = v
                break
            }
        }
        if (ver == 0) return null
        val ccBits = if (ver <= 9) 8 else 16
        val bits = ArrayList<Boolean>()
        fun append(value: Int, n: Int) {
            for (i in n - 1 downTo 0) bits.add(((value ushr i) and 1) != 0)
        }
        append(0x4, 4)
        append(data.size, ccBits)
        for (b in data) append(b.toInt() and 0xFF, 8)
        val capacityBits = numDataCodewords(ver, e) * 8
        append(0, minOf(4, capacityBits - bits.size))
        append(0, (8 - bits.size % 8) % 8)
        var pad = 0xEC
        while (bits.size < capacityBits) {
            append(pad, 8)
            pad = pad xor 0xEC xor 0x11
        }
        val dataCodewords = IntArray(bits.size / 8)
        for (i in bits.indices) if (bits[i]) dataCodewords[i ushr 3] = dataCodewords[i ushr 3] or (1 shl (7 - (i and 7)))
        val all = addEccAndInterleave(dataCodewords, ver, e)
        val m = Matrix(ver)
        m.drawFunctionPatterns(ecl.formatBits)
        m.drawCodewords(all)
        var bestMask = 0
        var minPenalty = Long.MAX_VALUE
        for (mask in 0 until 8) {
            m.applyMask(mask)
            m.drawFormatBits(ecl.formatBits, mask)
            val p = m.penaltyScore()
            if (p < minPenalty) {
                minPenalty = p
                bestMask = mask
            }
            m.applyMask(mask)
        }
        m.applyMask(bestMask)
        m.drawFormatBits(ecl.formatBits, bestMask)
        return QrMatrix(m.size, m.modules)
    }

    fun encodeText(text: String, ecl: Ecl = Ecl.Medium): QrMatrix? = encode(text.toByteArray(Charsets.UTF_8), ecl)
}
