package com.forkmesh.nativeapp.git.objects

import com.forkmesh.nativeapp.git.bytes.GitBytesBuilder
import com.forkmesh.nativeapp.git.bytes.asciiDecode
import com.forkmesh.nativeapp.git.bytes.asciiEncode
import com.forkmesh.nativeapp.git.bytes.indexOfByte
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException

@Suppress("EnumEntryName")
enum class GitTreeMode(val wireValue: String) {
    regular("100644"),
    executable("100755"),
    symlink("120000"),
    directory("40000"),
    ;

    companion object {
        fun parse(value: String): GitTreeMode = when (value) {
            "100644" -> regular
            "100755" -> executable
            "120000" -> symlink
            "40000" -> directory
            "160000" -> throw GitException(GitErrorCode.unsupportedRepositoryFormat, "gitlinks are unsupported")
            else -> throw GitException(GitErrorCode.invalidTree, "unsupported tree mode")
        }
    }
}

class GitTreeEntry internal constructor(val mode: GitTreeMode, name: ByteArray, val objectId: GitObjectId) {
    private val nameBytes: ByteArray = name.copyOf()

    val name: ByteArray get() = nameBytes.copyOf()

    internal val rawName: ByteArray get() = nameBytes

    init {
        if (nameBytes.isEmpty() || nameBytes.indexOfByte(0) != -1 || nameBytes.indexOfByte(0x2f) != -1) {
            throw GitException(GitErrorCode.invalidTree, "tree entry name is invalid")
        }
    }

    companion object {
        fun regular(name: ByteArray, objectId: GitObjectId) = GitTreeEntry(GitTreeMode.regular, name, objectId)

        fun executable(name: ByteArray, objectId: GitObjectId) = GitTreeEntry(GitTreeMode.executable, name, objectId)

        fun symlink(name: ByteArray, objectId: GitObjectId) = GitTreeEntry(GitTreeMode.symlink, name, objectId)

        fun directory(name: ByteArray, objectId: GitObjectId) = GitTreeEntry(GitTreeMode.directory, name, objectId)
    }
}

fun compareGitTreeEntries(left: GitTreeEntry, right: GitTreeEntry): Int {
    val leftName = left.rawName
    val rightName = right.rawName
    val leftLength = leftName.size + if (left.mode == GitTreeMode.directory) 1 else 0
    val rightLength = rightName.size + if (right.mode == GitTreeMode.directory) 1 else 0
    var index = 0
    while (index < leftLength && index < rightLength) {
        val leftByte = if (index < leftName.size) leftName[index].toInt() and 0xff else 0x2f
        val rightByte = if (index < rightName.size) rightName[index].toInt() and 0xff else 0x2f
        val comparison = leftByte - rightByte
        if (comparison != 0) return comparison
        index += 1
    }
    return leftLength - rightLength
}

class GitTree private constructor(val entries: List<GitTreeEntry>, @Suppress("UNUSED_PARAMETER") parsed: Boolean) {
    constructor(entries: Iterable<GitTreeEntry>) : this(
        entries.sortedWith { left, right -> compareGitTreeEntries(left, right) },
        false,
    ) {
        validateUnique(this.entries)
    }

    fun encode(): ByteArray {
        val output = GitBytesBuilder()
        for (entry in entries) {
            output.add(asciiEncode("${entry.mode.wireValue} "))
            output.add(entry.rawName)
            output.addByte(0)
            output.add(entry.objectId.rawBytes)
        }
        return output.takeBytes()
    }

    companion object {
        fun parse(body: ByteArray): GitTree {
            val bytes = body
            val parsed = ArrayList<GitTreeEntry>()
            var offset = 0
            while (offset < bytes.size) {
                val modeEnd = bytes.indexOfByte(0x20, offset)
                if (modeEnd <= offset) {
                    throw GitException(GitErrorCode.invalidTree, "tree mode is missing")
                }
                val nameEnd = bytes.indexOfByte(0, modeEnd + 1)
                if (nameEnd < modeEnd + 1 || nameEnd + 21 > bytes.size) {
                    throw GitException(GitErrorCode.invalidTree, "tree entry is truncated")
                }
                val modeText = asciiDecode(bytes, offset, modeEnd)
                val entry = GitTreeEntry(
                    GitTreeMode.parse(modeText),
                    bytes.copyOfRange(modeEnd + 1, nameEnd),
                    GitObjectId.fromSha1Bytes(bytes, nameEnd + 1),
                )
                if (parsed.isNotEmpty() && compareGitTreeEntries(parsed.last(), entry) >= 0) {
                    throw GitException(GitErrorCode.invalidTree, "tree entries are not strictly sorted")
                }
                parsed.add(entry)
                offset = nameEnd + 21
            }
            return GitTree(parsed, true)
        }

        private fun validateUnique(entries: List<GitTreeEntry>) {
            for (index in 1 until entries.size) {
                if (compareGitTreeEntries(entries[index - 1], entries[index]) == 0) {
                    throw GitException(GitErrorCode.invalidTree, "duplicate tree entry")
                }
            }
        }
    }
}
