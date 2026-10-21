package com.forkmesh.nativeapp.security

object HiddenUnicode {
    const val POLICY = "forkmesh-hidden-unicode-v1"

    private val DROP = intArrayOf(
        0x0000, 0x0008, 0x000B, 0x000C, 0x000E, 0x001F, 0x007F, 0x009F,
        0x00AD, 0x00AD, 0x034F, 0x034F, 0x115F, 0x1160, 0x17B4, 0x17B5,
        0x180B, 0x180F, 0x200B, 0x200B, 0x202A, 0x202E, 0x2060, 0x206F,
        0x3164, 0x3164, 0xD800, 0xDFFF, 0xFDD0, 0xFDEF, 0xFE00, 0xFE0D,
        0xFEFF, 0xFEFF, 0xFFA0, 0xFFA0, 0xFFF9, 0xFFFB, 0x1BCA0, 0x1BCA3,
        0x1D173, 0x1D17A, 0xE0000, 0xE007F, 0xE0100, 0xE01EF,
    )
    private val EMOJI = intArrayOf(
        0x00A9, 0x00A9, 0x00AE, 0x00AE, 0x203C, 0x203C, 0x2049, 0x2049,
        0x2122, 0x2122, 0x2139, 0x2139, 0x2190, 0x2BFF, 0x3030, 0x3030,
        0x303D, 0x303D, 0x3297, 0x3297, 0x3299, 0x3299, 0x1F000, 0x1FAFF,
    )
    private val JOINING = intArrayOf(
        0x0600, 0x08FF, 0x0900, 0x0DFF, 0x0F00, 0x109F,
        0x1780, 0x18AF, 0xFB50, 0xFDFF, 0xFE70, 0xFEFC,
    )
    private val RTL = intArrayOf(
        0x0590, 0x08FF, 0xFB1D, 0xFDFF, 0xFE70, 0xFEFC,
        0x10800, 0x10FFF, 0x1E800, 0x1EFFF,
    )

    private const val NONE = -1
    private const val ZWNJ = 0x200C
    private const val ZWJ = 0x200D
    private const val BLACK_FLAG = 0x1F3F4
    private const val CANCEL_TAG = 0xE007F

    private fun within(cp: Int, ranges: IntArray): Boolean {
        if (cp == NONE) return false
        var i = 0
        while (i < ranges.size) {
            if (cp >= ranges[i] && cp <= ranges[i + 1]) return true
            i += 2
        }
        return false
    }

    private fun isWhitespace(cp: Int) = cp == 0x09 || cp == 0x0A || cp == 0x0D || cp == 0x20

    private fun isSelector(cp: Int) = cp == 0xFE0E || cp == 0xFE0F

    private fun candidate(unit: Char): Boolean {
        val c = unit.code
        if (c < 0x20) return !isWhitespace(c)
        if (c < 0x7F) return false
        return c <= 0x9F || c == 0x00AD || c == 0x034F || c == 0x061C || c == 0x115F || c == 0x1160 ||
            c == 0x17B4 || c == 0x17B5 || c in 0x180B..0x180F || c in 0x200B..0x200F ||
            c in 0x2028..0x202E || c in 0x2060..0x206F || c == 0x3164 || c in 0xD800..0xDFFF ||
            c in 0xFDD0..0xFDEF || c in 0xFE00..0xFE0F || c == 0xFEFF || c == 0xFFA0 || c >= 0xFFF9
    }

    private fun codePoints(text: String): IntArray {
        val out = IntArray(text.length)
        var count = 0
        var i = 0
        while (i < text.length) {
            val high = text[i]
            if (high.isHighSurrogate() && i + 1 < text.length && text[i + 1].isLowSurrogate()) {
                out[count++] = Character.toCodePoint(high, text[i + 1])
                i += 2
            } else {
                out[count++] = high.code
                i += 1
            }
        }
        return out.copyOf(count)
    }

    private fun flagTagLength(cps: IntArray, start: Int): Int {
        var index = start
        while (index < cps.size && index - start < 8) {
            val cp = cps[index]
            if (cp == CANCEL_TAG) {
                val run = index - start
                return if (run in 2..7) run + 1 else 0
            }
            if (!(cp in 0xE0030..0xE0039 || cp in 0xE0061..0xE007A)) return 0
            index++
        }
        return 0
    }

    fun clean(text: String): String {
        if (text.isEmpty() || text.none(::candidate)) return text
        val cps = codePoints(text)
        val out = StringBuilder(text.length)
        var prev = NONE
        var changed = false
        var index = 0
        fun keep(cp: Int) {
            out.appendCodePoint(cp)
            prev = cp
        }
        while (index < cps.size) {
            val cp = cps[index]
            val following = if (index + 1 < cps.size) cps[index + 1] else NONE
            index++
            val kept = when {
                cp == 0x2028 || cp == 0x2029 -> {
                    keep(0x0A)
                    changed = true
                    continue
                }
                isSelector(cp) -> prev != NONE && !isWhitespace(prev) && !isSelector(prev) && prev != ZWJ && prev != ZWNJ
                cp == ZWJ -> ((prev == 0xFE0F || within(prev, EMOJI)) && within(following, EMOJI)) ||
                    (within(prev, JOINING) && within(following, JOINING))
                cp == ZWNJ -> within(prev, JOINING) && within(following, JOINING)
                cp == 0x061C || cp == 0x200E || cp == 0x200F -> within(prev, RTL) || within(following, RTL)
                cp == BLACK_FLAG -> {
                    keep(cp)
                    val run = flagTagLength(cps, index)
                    for (t in 0 until run) keep(cps[index + t])
                    index += run
                    continue
                }
                within(cp, DROP) || (cp and 0xFFFE) == 0xFFFE -> false
                else -> true
            }
            if (kept) keep(cp) else changed = true
        }
        return if (changed) out.toString() else text
    }

    fun hasHidden(text: String): Boolean = clean(text) != text

    fun cleanedOffset(text: String, offset: Int): Int =
        clean(text.substring(0, offset.coerceIn(0, text.length))).length
}
