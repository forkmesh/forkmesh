package com.forkmesh.nativeapp.codeinsight

data class InsightLineCounts(val added: Int, val deleted: Int)

object InsightLineDiff {
    const val BINARY_SNIFF_BYTES = 8000
    private const val WORK_BUDGET = 60_000_000L

    fun isBinary(bytes: ByteArray): Boolean {
        val limit = minOf(bytes.size, BINARY_SNIFF_BYTES)
        for (i in 0 until limit) if (bytes[i].toInt() == 0) return true
        return false
    }

    fun count(old: ByteArray, new: ByteArray): InsightLineCounts? {
        if (isBinary(old) || isBinary(new)) return null
        val ids = HashMap<String, Int>()
        val a = lineIds(old, ids)
        val b = lineIds(new, ids)
        return countLines(a, b)
    }

    fun countLines(a: IntArray, b: IntArray): InsightLineCounts {
        var start = 0
        while (start < a.size && start < b.size && a[start] == b[start]) start++
        var endA = a.size
        var endB = b.size
        while (endA > start && endB > start && a[endA - 1] == b[endB - 1]) {
            endA--
            endB--
        }
        val n = endA - start
        val m = endB - start
        if (n == 0 || m == 0) return InsightLineCounts(added = m, deleted = n)
        val total = (n + m).toLong()
        val maxD = minOf(n + m, maxOf(256L, WORK_BUDGET / total).toInt())
        val d = editDistance(a, start, n, b, start, m, maxD)
        if (d < 0) return histogram(a, start, endA, b, start, endB)
        return InsightLineCounts(added = (d - n + m) / 2, deleted = (d + n - m) / 2)
    }

    private fun editDistance(a: IntArray, aStart: Int, n: Int, b: IntArray, bStart: Int, m: Int, maxD: Int): Int {
        val offset = maxD + 1
        val v = IntArray(2 * maxD + 3)
        for (d in 0..maxD) {
            var k = -d
            while (k <= d) {
                var x = if (k == -d || (k != d && v[offset + k - 1] < v[offset + k + 1])) v[offset + k + 1] else v[offset + k - 1] + 1
                var y = x - k
                while (x < n && y < m && a[aStart + x] == b[bStart + y]) {
                    x++
                    y++
                }
                v[offset + k] = x
                if (x >= n && y >= m) return d
                k += 2
            }
        }
        return -1
    }

    private fun histogram(a: IntArray, aStart: Int, aEnd: Int, b: IntArray, bStart: Int, bEnd: Int): InsightLineCounts {
        val counts = HashMap<Int, Int>()
        for (i in aStart until aEnd) counts[a[i]] = (counts[a[i]] ?: 0) + 1
        var added = 0
        for (i in bStart until bEnd) {
            val left = counts[b[i]] ?: 0
            if (left > 0) counts[b[i]] = left - 1 else added++
        }
        val deleted = counts.values.sum()
        return InsightLineCounts(added = added, deleted = deleted)
    }

    private fun lineIds(bytes: ByteArray, ids: HashMap<String, Int>): IntArray {
        if (bytes.isEmpty()) return IntArray(0)
        val out = ArrayList<Int>()
        var start = 0
        for (i in bytes.indices) {
            if (bytes[i] == '\n'.code.toByte()) {
                out.add(idOf(bytes, start, i + 1, ids))
                start = i + 1
            }
        }
        if (start < bytes.size) out.add(idOf(bytes, start, bytes.size, ids))
        return out.toIntArray()
    }

    private fun idOf(bytes: ByteArray, from: Int, to: Int, ids: HashMap<String, Int>): Int {
        val key = String(bytes, from, to - from, Charsets.ISO_8859_1)
        return ids.getOrPut(key) { ids.size }
    }
}
