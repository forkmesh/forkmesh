package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.objects.GitTreeMode
import com.forkmesh.nativeapp.git.repository.GitRepository

class PatchBuildResult(
    val patch: String,
    val changedPaths: List<String>,
    val binaryPaths: List<String>,
) {
    val isEmpty: Boolean get() = patch.isBlank()
    val hasBinaryChanges: Boolean get() = binaryPaths.isNotEmpty()
}

object PatchBuilder {
    const val defaultContext = 3

    private const val binarySniffBytes = 8000

    private const val maxEditDistance = 600

    private const val maxTreeEntries = 20000
    private const val maxTreeDepth = 64

    private const val zeroId = "0000000000000000000000000000000000000000"

    private const val noEolMark = '\u0000'

    fun between(repository: GitRepository, base: GitObjectId, head: GitObjectId, context: Int = defaultContext): PatchBuildResult {
        val baseTree = flatten(repository, base)
        val headTree = flatten(repository, head)

        val paths = (baseTree.keys + headTree.keys).toSortedSet().toList()
        val buffer = StringBuilder()
        val changed = ArrayList<String>()
        val binary = ArrayList<String>()

        for (path in paths) {
            val before = baseTree[path]
            val after = headTree[path]
            if (before != null && after != null && before.id == after.id) {
                if (before.mode == after.mode) continue
                buffer.append("diff --git a/$path b/$path\n")
                    .append("old mode ${before.mode}\n")
                    .append("new mode ${after.mode}\n")
                changed.add(path)
                continue
            }

            val beforeBytes = if (before == null) ByteArray(0) else repository.readBlob(before.id)
            val afterBytes = if (after == null) ByteArray(0) else repository.readBlob(after.id)

            if (isBinary(beforeBytes) || isBinary(afterBytes)) {
                binary.add(path)
                continue
            }

            val fileDiff = fileDiff(
                path = path,
                before = before,
                after = after,
                beforeText = String(beforeBytes, Charsets.UTF_8),
                afterText = String(afterBytes, Charsets.UTF_8),
                context = context,
            )
            if (fileDiff.isEmpty()) continue
            buffer.append(fileDiff)
            changed.add(path)
        }
        return PatchBuildResult(patch = buffer.toString(), changedPaths = changed, binaryPaths = binary)
    }

    fun forFile(
        path: String,
        beforeBytes: ByteArray,
        afterText: String,
        oldId: GitObjectId,
        newId: GitObjectId,
        context: Int = defaultContext,
    ): PatchBuildResult {
        val afterBytes = afterText.toByteArray(Charsets.UTF_8)
        if (isBinary(beforeBytes) || isBinary(afterBytes)) {
            return PatchBuildResult(patch = "", changedPaths = emptyList(), binaryPaths = listOf(path))
        }
        val beforeText = strictUtf8(beforeBytes)
            ?: return PatchBuildResult(patch = "", changedPaths = emptyList(), binaryPaths = listOf(path))
        val fileDiff = fileDiff(
            path = path,
            before = BlobRef(oldId, unknownMode),
            after = BlobRef(newId, unknownMode),
            beforeText = beforeText,
            afterText = afterText,
            context = context,
            writeMode = false,
        )
        return PatchBuildResult(
            patch = fileDiff,
            changedPaths = if (fileDiff.isEmpty()) emptyList() else listOf(path),
            binaryPaths = emptyList(),
        )
    }

    private const val unknownMode = ""

    private fun strictUtf8(bytes: ByteArray): String? = try {
        Charsets.UTF_8.newDecoder()
            .onMalformedInput(java.nio.charset.CodingErrorAction.REPORT)
            .onUnmappableCharacter(java.nio.charset.CodingErrorAction.REPORT)
            .decode(java.nio.ByteBuffer.wrap(bytes))
            .toString()
    } catch (_: java.nio.charset.CharacterCodingException) {
        null
    }

    private fun flatten(repository: GitRepository, commitId: GitObjectId): Map<String, BlobRef> {
        val commit = repository.readCommit(commitId)
        val out = LinkedHashMap<String, BlobRef>()
        walk(repository, commit.tree, "", out, 0)
        return out
    }

    private fun walk(repository: GitRepository, treeId: GitObjectId, prefix: String, out: MutableMap<String, BlobRef>, depth: Int) {
        if (depth > maxTreeDepth || out.size >= maxTreeEntries) return
        val tree = repository.readTree(treeId)
        for (entry in tree.entries) {
            val name = String(entry.name, Charsets.UTF_8)
            val path = if (prefix.isEmpty()) name else "$prefix/$name"
            if (entry.mode == GitTreeMode.directory) {
                walk(repository, entry.objectId, path, out, depth + 1)
            } else {
                if (out.size >= maxTreeEntries) return
                out[path] = BlobRef(entry.objectId, entry.mode.wireValue)
            }
        }
    }

    private fun fileDiff(
        path: String,
        before: BlobRef?,
        after: BlobRef?,
        beforeText: String,
        afterText: String,
        context: Int,
        writeMode: Boolean = true,
    ): String {
        if (beforeText == afterText && before?.mode == after?.mode) return ""

        val beforeLines = keyLines(beforeText)
        val afterLines = keyLines(afterText)

        val header = StringBuilder("diff --git a/$path b/$path\n")
        if (!writeMode) Unit else if (before == null) {
            header.append("new file mode ${after!!.mode}\n")
        } else if (after == null) {
            header.append("deleted file mode ${before.mode}\n")
        } else if (before.mode != after.mode) {
            header.append("old mode ${before.mode}\n").append("new mode ${after.mode}\n")
        }
        val oldId = before?.id?.hex ?: zeroId
        val newId = after?.id?.hex ?: zeroId
        val modeSuffix = when {
            !writeMode -> ""
            before != null && after != null && before.mode == after.mode -> " ${before.mode}"
            before == null -> " ${after!!.mode}"
            else -> ""
        }
        header.append("index $oldId..$newId$modeSuffix\n")
        header.append(if (before == null) "--- /dev/null\n" else "--- a/$path\n")
        header.append(if (after == null) "+++ /dev/null\n" else "+++ b/$path\n")

        val rows = rows(diffLines(beforeLines, afterLines))
        val hunks = hunks(rows, context, beforeLines.size, afterLines.size)
        if (hunks.isEmpty()) {
            return header.toString()
        }

        val body = StringBuilder()
        for (hunk in hunks) {
            body.append(hunk.headerLine).append('\n')
            for (row in hunk.rows) {
                body.append(row.marker).append(row.displayText).append('\n')
                if (row.noEol) body.append("\\ No newline at end of file\n")
            }
        }
        return "$header$body"
    }

    private fun rows(edits: List<Edit>): List<Row> {
        val rows = ArrayList<Row>()
        var oldNo = 1
        var newNo = 1
        for (edit in edits) {
            when (edit.op) {
                Op.keep -> {
                    rows.add(Row(Op.keep, edit.text, oldNo, newNo, oldNo))
                    oldNo++
                    newNo++
                }
                Op.delete -> {
                    rows.add(Row(Op.delete, edit.text, oldNo, 0, oldNo))
                    oldNo++
                }
                Op.insert -> {
                    rows.add(Row(Op.insert, edit.text, 0, newNo, oldNo))
                    newNo++
                }
            }
        }
        return rows
    }

    private fun hunks(rows: List<Row>, context: Int, oldTotal: Int, newTotal: Int): List<Hunk> {
        val changed = rows.indices.filter { rows[it].op != Op.keep }
        if (changed.isEmpty()) return emptyList()

        val hunks = ArrayList<Hunk>()
        var index = 0
        while (index < changed.size) {
            val start = (changed[index] - context).coerceIn(0, rows.size)
            var end = changed[index]
            while (index + 1 < changed.size && changed[index + 1] - end <= context * 2) {
                index++
                end = changed[index]
            }
            end = (end + context + 1).coerceIn(0, rows.size)
            index++

            val slice = rows.subList(start, end)
            val oldCount = slice.count { it.op != Op.insert }
            val newCount = slice.count { it.op != Op.delete }
            val oldStart = if (oldCount == 0) {
                (slice.first().oldAnchor - 1).coerceIn(0, oldTotal)
            } else {
                slice.first { it.op != Op.insert }.oldNo
            }
            val newStart = if (newCount == 0) {
                (slice.first().newNoOrAnchor - 1).coerceIn(0, newTotal)
            } else {
                slice.first { it.op != Op.delete }.newNo
            }
            hunks.add(Hunk(rows = slice, headerLine = "@@ -${range(oldStart, oldCount)} +${range(newStart, newCount)} @@"))
        }
        return hunks
    }

    private fun range(start: Int, count: Int): String = if (count == 1) "$start" else "$start,$count"

    private fun diffLines(a: List<String>, b: List<String>): List<Edit> {
        var prefix = 0
        while (prefix < a.size && prefix < b.size && a[prefix] == b[prefix]) prefix++
        var suffix = 0
        while (suffix < a.size - prefix && suffix < b.size - prefix && a[a.size - 1 - suffix] == b[b.size - 1 - suffix]) {
            suffix++
        }
        val midA = a.subList(prefix, a.size - suffix)
        val midB = b.subList(prefix, b.size - suffix)

        val edits = ArrayList<Edit>()
        for (i in 0 until prefix) edits.add(Edit(Op.keep, a[i]))
        edits.addAll(myers(midA, midB))
        for (i in a.size - suffix until a.size) edits.add(Edit(Op.keep, a[i]))
        return edits
    }

    private fun myers(a: List<String>, b: List<String>): List<Edit> {
        if (a.isEmpty() && b.isEmpty()) return emptyList()
        if (a.isEmpty()) return b.map { Edit(Op.insert, it) }
        if (b.isEmpty()) return a.map { Edit(Op.delete, it) }

        val n = a.size
        val m = b.size
        val maxD = if (n + m < maxEditDistance) n + m else maxEditDistance
        val offset = maxD
        val size = 2 * maxD + 3
        val v = IntArray(size)
        val trace = ArrayList<IntArray>()

        for (d in 0..maxD) {
            trace.add(v.copyOf())
            var k = -d
            while (k <= d) {
                val ki = k + offset
                if (ki <= 0 || ki >= size - 1) {
                    k += 2
                    continue
                }
                var x = if (k == -d || (k != d && v[ki - 1] < v[ki + 1])) v[ki + 1] else v[ki - 1] + 1
                var y = x - k
                while (x < n && y < m && a[x] == b[y]) {
                    x++
                    y++
                }
                v[ki] = x
                if (x >= n && y >= m) return backtrack(trace, a, b, offset, size)
                k += 2
            }
        }

        return a.map { Edit(Op.delete, it) } + b.map { Edit(Op.insert, it) }
    }

    private fun backtrack(trace: List<IntArray>, a: List<String>, b: List<String>, offset: Int, size: Int): List<Edit> {
        val edits = ArrayList<Edit>()
        var x = a.size
        var y = b.size
        var d = trace.size - 1
        while (d >= 0 && (x > 0 || y > 0)) {
            val v = trace[d]
            val k = x - y
            val ki = k + offset
            val prevK = if (k == -d || (k != d && ki - 1 > 0 && ki + 1 < size && v[ki - 1] < v[ki + 1])) k + 1 else k - 1
            val prevKi = prevK + offset
            val prevX = if (prevKi in 0 until size) v[prevKi] else 0
            val prevY = prevX - prevK

            while (x > prevX && y > prevY) {
                edits.add(Edit(Op.keep, a[x - 1]))
                x--
                y--
            }
            if (d == 0) break
            if (x > prevX) {
                edits.add(Edit(Op.delete, a[x - 1]))
                x--
            } else if (y > prevY) {
                edits.add(Edit(Op.insert, b[y - 1]))
                y--
            }
            d--
        }
        return edits.asReversed()
    }

    private fun isBinary(bytes: ByteArray): Boolean {
        val limit = if (bytes.size < binarySniffBytes) bytes.size else binarySniffBytes
        for (i in 0 until limit) {
            if (bytes[i].toInt() == 0) return true
        }
        return false
    }

    private fun keyLines(text: String): List<String> {
        if (text.isEmpty()) return emptyList()
        val lines = text.split("\n").toMutableList()
        val endsWithNewline = lines.isNotEmpty() && lines.last().isEmpty()
        if (endsWithNewline) {
            lines.removeAt(lines.lastIndex)
        } else if (lines.isNotEmpty()) {
            lines[lines.lastIndex] = lines.last() + noEolMark
        }
        return lines
    }

    private class BlobRef(val id: GitObjectId, val mode: String)

    @Suppress("EnumEntryName")
    private enum class Op { keep, delete, insert }

    private class Edit(val op: Op, val text: String)

    private class Row(
        val op: Op,
        val text: String,
        val oldNo: Int,
        val newNo: Int,
        val oldAnchor: Int,
    ) {
        val newNoOrAnchor: Int get() = if (newNo == 0) 1 else newNo

        val noEol: Boolean get() = text.endsWith(noEolMark)

        val displayText: String get() = if (noEol) text.substring(0, text.length - 1) else text

        val marker: Char
            get() = when (op) {
                Op.keep -> ' '
                Op.delete -> '-'
                Op.insert -> '+'
            }
    }

    private class Hunk(val rows: List<Row>, val headerLine: String)
}
