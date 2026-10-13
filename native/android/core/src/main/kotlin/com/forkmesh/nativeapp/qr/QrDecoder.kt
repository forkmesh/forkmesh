package com.forkmesh.nativeapp.qr

import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.Charset
import java.nio.charset.CodingErrorAction

object QrDecoder {
    private val EXP = IntArray(512)
    private val LOG = IntArray(256)

    init {
        var x = 1
        for (i in 0 until 255) {
            EXP[i] = x
            LOG[x] = i
            x = x shl 1
            if (x and 0x100 != 0) x = x xor 0x11D
        }
        for (i in 255 until 512) EXP[i] = EXP[i - 255]
    }

    private fun mul(a: Int, b: Int): Int = if (a == 0 || b == 0) 0 else EXP[LOG[a] + LOG[b]]

    private fun div(a: Int, b: Int): Int = if (a == 0) 0 else EXP[(LOG[a] + 255 - LOG[b]) % 255]

    private fun pow(e: Int): Int = EXP[((e % 255) + 255) % 255]

    private fun formatCode(eclFormat: Int, mask: Int): Int {
        val data = eclFormat shl 3 or mask
        var rem = data
        repeat(10) { rem = (rem shl 1) xor ((rem ushr 9) * 0x537) }
        return (data shl 10 or rem) xor 0x5412
    }

    private fun versionCode(ver: Int): Int {
        var rem = ver
        repeat(12) { rem = (rem shl 1) xor ((rem ushr 11) * 0x1F25) }
        return (ver shl 12) or rem
    }

    internal fun correct(codeword: IntArray, eccLen: Int): Boolean {
        val n = codeword.size
        val syndromes = IntArray(eccLen)
        var clean = true
        for (i in 0 until eccLen) {
            var s = 0
            for (j in 0 until n) s = mul(s, pow(i)) xor codeword[j]
            syndromes[i] = s
            if (s != 0) clean = false
        }
        if (clean) return true
        var c = intArrayOf(1)
        var b = intArrayOf(1)
        var l = 0
        var m = 1
        var bScale = 1
        for (k in 0 until eccLen) {
            var d = syndromes[k]
            for (i in 1..l) if (i < c.size) d = d xor mul(c[i], syndromes[k - i])
            if (d == 0) {
                m++
                continue
            }
            val coef = div(d, bScale)
            val next = IntArray(maxOf(c.size, b.size + m))
            for (i in c.indices) next[i] = c[i]
            for (i in b.indices) next[i + m] = next[i + m] xor mul(coef, b[i])
            if (2 * l <= k) {
                b = c
                l = k + 1 - l
                bScale = d
                m = 1
            } else {
                m++
            }
            c = next
        }
        val lambda = IntArray(l + 1) { if (it < c.size) c[it] else 0 }
        if (l > eccLen / 2) return false
        val positions = ArrayList<Int>()
        for (j in 0 until n) {
            val xInv = pow(-(n - 1 - j))
            var v = 0
            for (i in lambda.indices.reversed()) v = mul(v, xInv) xor lambda[i]
            if (v == 0) positions.add(j)
        }
        if (positions.size != l) return false
        val omega = IntArray(eccLen)
        for (i in 0 until eccLen) {
            var v = 0
            for (k in 0..minOf(i, l)) v = v xor mul(lambda[k], syndromes[i - k])
            omega[i] = v
        }
        for (j in positions) {
            val power = n - 1 - j
            val xInv = pow(-power)
            var num = 0
            for (i in omega.indices.reversed()) num = mul(num, xInv) xor omega[i]
            var den = 0
            var i = 1
            while (i < lambda.size) {
                den = den xor mul(lambda[i], pow(-power * (i - 1)))
                i += 2
            }
            if (den == 0) return false
            codeword[j] = codeword[j] xor mul(pow(power), div(num, den))
        }
        for (i in 0 until eccLen) {
            var s = 0
            for (j in 0 until n) s = mul(s, pow(i)) xor codeword[j]
            if (s != 0) return false
        }
        return true
    }

    fun readVersion(modules: Array<BooleanArray>): Int? {
        val size = modules.size
        val fromSize = (size - 17) / 4
        if (fromSize < 7) return fromSize
        var bits1 = 0
        var bits2 = 0
        for (i in 0 until 18) {
            val a = size - 11 + i % 3
            val c = i / 3
            if (modules[c][a]) bits1 = bits1 or (1 shl i)
            if (modules[a][c]) bits2 = bits2 or (1 shl i)
        }
        var best = -1
        var bestDistance = 99
        for (v in 7..40) {
            val code = versionCode(v)
            val d = minOf(Integer.bitCount(code xor bits1), Integer.bitCount(code xor bits2))
            if (d < bestDistance) {
                bestDistance = d
                best = v
            }
        }
        return if (bestDistance <= 3) best else null
    }

    fun decode(modules: Array<BooleanArray>): String? {
        val size = modules.size
        if (size < 21 || (size - 17) % 4 != 0 || modules.any { it.size != size }) return null
        val ver = (size - 17) / 4
        if (ver > 40) return null
        if (ver >= 7 && readVersion(modules) != ver) return null
        fun at(x: Int, y: Int): Int = if (modules[y][x]) 1 else 0
        var copy1 = 0
        var copy2 = 0
        val first = ArrayList<Pair<Int, Int>>()
        for (i in 0..5) first.add(8 to i)
        first.add(8 to 7)
        first.add(8 to 8)
        first.add(7 to 8)
        for (i in 9 until 15) first.add((14 - i) to 8)
        val second = ArrayList<Pair<Int, Int>>()
        for (i in 0 until 8) second.add((size - 1 - i) to 8)
        for (i in 8 until 15) second.add(8 to (size - 15 + i))
        for (i in 0 until 15) {
            copy1 = copy1 or (at(first[i].first, first[i].second) shl i)
            copy2 = copy2 or (at(second[i].first, second[i].second) shl i)
        }
        var eclFormat = -1
        var mask = -1
        var bestDistance = 99
        for (e in 0 until 4) {
            for (mk in 0 until 8) {
                val code = formatCode(e, mk)
                val d = minOf(Integer.bitCount(code xor copy1), Integer.bitCount(code xor copy2))
                if (d < bestDistance) {
                    bestDistance = d
                    eclFormat = e
                    mask = mk
                }
            }
        }
        if (bestDistance > 3) return null
        val ecl = QrCode.Ecl.entries.first { it.formatBits == eclFormat }.index
        val function = QrCode.Matrix(ver).apply { drawFunctionPatterns(0) }.isFunction
        val raw = QrCode.numRawDataModules(ver) / 8
        val codewords = IntArray(raw)
        var bitIndex = 0
        var right = size - 1
        while (right >= 1) {
            if (right == 6) right = 5
            for (vert in 0 until size) {
                for (j in 0 until 2) {
                    val x = right - j
                    val upward = ((right + 1) and 2) == 0
                    val y = if (upward) size - 1 - vert else vert
                    if (!function[y][x] && bitIndex < raw * 8) {
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
                        if (modules[y][x] != invert) codewords[bitIndex ushr 3] = codewords[bitIndex ushr 3] or (1 shl (7 - (bitIndex and 7)))
                        bitIndex++
                    }
                }
            }
            right -= 2
        }
        val numBlocks = QrCode.NUM_EC_BLOCKS[ecl][ver]
        val eccLen = QrCode.ECC_CODEWORDS_PER_BLOCK[ecl][ver]
        val numShort = numBlocks - raw % numBlocks
        val shortLen = raw / numBlocks
        val blocks = Array(numBlocks) { IntArray(shortLen + 1) }
        var k = 0
        for (i in 0..shortLen) {
            for (j in 0 until numBlocks) {
                if (i == shortLen - eccLen && j < numShort) continue
                blocks[j][i] = codewords[k++]
            }
        }
        val data = ArrayList<Int>()
        for (j in 0 until numBlocks) {
            val dataLen = shortLen - eccLen + if (j < numShort) 0 else 1
            val word = if (j < numShort) {
                IntArray(shortLen) { if (it < dataLen) blocks[j][it] else blocks[j][it + 1] }
            } else {
                blocks[j].copyOf()
            }
            if (!correct(word, eccLen)) return null
            for (i in 0 until dataLen) data.add(word[i])
        }
        return parse(data.toIntArray(), ver)
    }

    private class Bits(private val bytes: IntArray) {
        var pos = 0
        fun available(): Int = bytes.size * 8 - pos
        fun read(n: Int): Int {
            var v = 0
            repeat(n) {
                v = (v shl 1) or ((bytes[pos ushr 3] ushr (7 - (pos and 7))) and 1)
                pos++
            }
            return v
        }
    }

    private const val ALPHANUMERIC = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"

    private fun text(bytes: ByteArray, charset: Charset?): String {
        if (charset != null) return String(bytes, charset)
        return try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes)).toString()
        } catch (_: CharacterCodingException) {
            String(bytes, Charsets.ISO_8859_1)
        }
    }

    private fun parse(data: IntArray, ver: Int): String? {
        val bits = Bits(data)
        val out = StringBuilder()
        var charset: Charset? = null
        val band = if (ver <= 9) 0 else if (ver <= 26) 1 else 2
        while (bits.available() >= 4) {
            val mode = bits.read(4)
            if (mode == 0) break
            when (mode) {
                1 -> {
                    var count = bits.read(intArrayOf(10, 12, 14)[band])
                    while (count >= 3) {
                        if (bits.available() < 10) return null
                        out.append(bits.read(10).toString().padStart(3, '0'))
                        count -= 3
                    }
                    if (count == 2) out.append(bits.read(7).toString().padStart(2, '0'))
                    if (count == 1) out.append(bits.read(4).toString())
                }
                2 -> {
                    var count = bits.read(intArrayOf(9, 11, 13)[band])
                    while (count >= 2) {
                        if (bits.available() < 11) return null
                        val v = bits.read(11)
                        if (v / 45 >= 45) return null
                        out.append(ALPHANUMERIC[v / 45]).append(ALPHANUMERIC[v % 45])
                        count -= 2
                    }
                    if (count == 1) {
                        val v = bits.read(6)
                        if (v >= 45) return null
                        out.append(ALPHANUMERIC[v])
                    }
                }
                4 -> {
                    val count = bits.read(intArrayOf(8, 16, 16)[band])
                    if (bits.available() < count * 8) return null
                    val bytes = ByteArray(count) { bits.read(8).toByte() }
                    out.append(text(bytes, charset))
                }
                7 -> {
                    val firstByte = bits.read(8)
                    val value = when {
                        firstByte and 0x80 == 0 -> firstByte
                        firstByte and 0xC0 == 0x80 -> ((firstByte and 0x3F) shl 8) or bits.read(8)
                        else -> ((firstByte and 0x1F) shl 16) or bits.read(16)
                    }
                    charset = when (value) {
                        26 -> Charsets.UTF_8
                        1, 3 -> Charsets.ISO_8859_1
                        20 -> runCatching { Charset.forName("Shift_JIS") }.getOrNull()
                        else -> null
                    }
                }
                8 -> {
                    val count = bits.read(intArrayOf(8, 10, 12)[band])
                    if (bits.available() < count * 13) return null
                    val sjis = ByteArray(count * 2)
                    for (i in 0 until count) {
                        val v = bits.read(13)
                        var code = (v / 0xC0 shl 8) or (v % 0xC0)
                        code += if (code < 0x1F00) 0x8140 else 0xC140
                        sjis[2 * i] = (code ushr 8).toByte()
                        sjis[2 * i + 1] = code.toByte()
                    }
                    val shiftJis = runCatching { Charset.forName("Shift_JIS") }.getOrNull() ?: return null
                    out.append(String(sjis, shiftJis))
                }
                else -> return null
            }
        }
        return out.toString()
    }
}
