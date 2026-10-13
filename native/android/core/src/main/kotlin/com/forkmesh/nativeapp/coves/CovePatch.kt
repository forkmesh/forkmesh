package com.forkmesh.nativeapp.coves

import com.forkmesh.nativeapp.crypto.Bytes
import java.security.MessageDigest
import java.time.Instant
import java.time.ZoneOffset

object CovePatch {
    fun gitBlobId(bytes: ByteArray): String {
        val digest = MessageDigest.getInstance("SHA-1")
        digest.update("blob ${bytes.size}\u0000".toByteArray(Charsets.UTF_8))
        digest.update(bytes)
        return Bytes.hex(digest.digest())
    }

    private fun lines(bytes: ByteArray): Pair<List<String>, Boolean> {
        if (bytes.isEmpty()) return emptyList<String>() to true
        val result = ArrayList<String>()
        var start = 0
        for (index in bytes.indices) {
            if (bytes[index] == 0x0A.toByte()) {
                result.add(String(bytes, start, index - start, Charsets.UTF_8))
                start = index + 1
            }
        }
        val ends = start == bytes.size
        if (!ends) result.add(String(bytes, start, bytes.size - start, Charsets.UTF_8))
        return result to ends
    }

    private fun range(count: Int): String = when (count) {
        0 -> "0,0"
        1 -> "1"
        else -> "1,$count"
    }

    fun replaceFile(path: String, before: ByteArray?, after: ByteArray): String {
        if (before != null && before.contentEquals(after)) return ""
        val newId = gitBlobId(after)
        val (newLines, newEnds) = lines(after)
        val out = StringBuilder("diff --git a/$path b/$path\n")
        if (before != null) {
            val oldId = gitBlobId(before)
            val (oldLines, oldEnds) = lines(before)
            out.append("index $oldId..$newId\n--- a/$path\n+++ b/$path\n")
            out.append("@@ -${range(oldLines.size)} +${range(newLines.size)} @@\n")
            oldLines.forEachIndexed { index, line ->
                out.append('-').append(line).append('\n')
                if (index == oldLines.size - 1 && !oldEnds) out.append("\\ No newline at end of file\n")
            }
        } else {
            out.append("new file mode 100644\nindex 0000000000000000000000000000000000000000..$newId\n--- /dev/null\n+++ b/$path\n")
            out.append("@@ -0,0 +${range(newLines.size)} @@\n")
        }
        newLines.forEachIndexed { index, line ->
            out.append('+').append(line).append('\n')
            if (index == newLines.size - 1 && !newEnds) out.append("\\ No newline at end of file\n")
        }
        return out.toString()
    }

    fun head(account: String, slug: String, nowMs: Long): String {
        val time = Instant.ofEpochMilli(nowMs).atZone(ZoneOffset.UTC)
        fun two(value: Int): String = value.toString().padStart(2, '0')
        val stamp = "${two(time.monthValue)}${two(time.dayOfMonth)}-${two(time.hour)}${two(time.minute)}${two(time.second)}"
        val branch = "mobile-cove/${slug.take(12)}-$stamp"
        val owner = CoveStore.normalizedAccount(account)
        return if (owner.isEmpty()) branch else "$owner:$branch"
    }
}
