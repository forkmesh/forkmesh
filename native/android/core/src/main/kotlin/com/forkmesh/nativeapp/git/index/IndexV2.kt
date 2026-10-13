package com.forkmesh.nativeapp.git.index

import com.forkmesh.nativeapp.git.bytes.DartFormatException
import com.forkmesh.nativeapp.git.bytes.GitBytesBuilder
import com.forkmesh.nativeapp.git.bytes.asciiEncode
import com.forkmesh.nativeapp.git.bytes.constantTimeEquals
import com.forkmesh.nativeapp.git.bytes.indexOfByte
import com.forkmesh.nativeapp.git.bytes.readUint16
import com.forkmesh.nativeapp.git.bytes.readUint32
import com.forkmesh.nativeapp.git.bytes.uint16Bytes
import com.forkmesh.nativeapp.git.bytes.uint32Bytes
import com.forkmesh.nativeapp.git.bytes.utf8Decode
import com.forkmesh.nativeapp.git.crypto.ForkMeshSha1
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.security.GitResourceLimits

@Suppress("EnumEntryName")
enum class GitIndexMode(val fileMode: Int) {
    regular(33188),
    executable(33261),
    symlink(40960),
    ;

    companion object {
        fun parse(fileMode: Long): GitIndexMode = when (fileMode) {
            33188L -> regular
            33261L -> executable
            40960L -> symlink
            57344L -> throw GitException(GitErrorCode.unsupportedRepositoryFormat, "gitlinks are unsupported")
            else -> throw GitException(GitErrorCode.indexCorrupt, "unsupported index file mode")
        }
    }
}

class GitIndexEntry(
    path: ByteArray,
    val objectId: GitObjectId,
    val mode: GitIndexMode,
    val size: Long = 0,
    val stage: Int = 0,
) {
    val path: ByteArray = path.copyOf()

    init {
        if (size < 0 || size > 0xffffffffL) {
            throw IllegalArgumentException("Invalid argument (size): must fit in uint32: $size")
        }
        if (stage != 0) {
            throw GitException(GitErrorCode.unsupportedRepositoryFormat, "conflict index stages are unsupported")
        }
        validatePath(this.path)
    }
}

class GitIndexV2 private constructor(val entries: List<GitIndexEntry>, @Suppress("UNUSED_PARAMETER") parsed: Boolean) {
    constructor(entries: Iterable<GitIndexEntry>) : this(entries.sortedWith(::comparePaths), false) {
        for (index in 1 until this.entries.size) {
            if (comparePaths(this.entries[index - 1], this.entries[index]) == 0) {
                throw GitException(GitErrorCode.indexCorrupt, "duplicate index path")
            }
        }
    }

    fun encode(): ByteArray {
        val output = GitBytesBuilder()
        output.add(asciiEncode("DIRC"))
        output.add(uint32Bytes(2))
        output.add(uint32Bytes(entries.size.toLong()))
        for (entry in entries) {
            val entryOffset = output.length
            for (field in 0 until 6) {
                output.add(uint32Bytes(0))
            }
            output.add(uint32Bytes(entry.mode.fileMode.toLong()))
            output.add(uint32Bytes(0))
            output.add(uint32Bytes(0))
            output.add(uint32Bytes(entry.size))
            output.add(entry.objectId.rawBytes)
            val pathLength = if (entry.path.size > 0xffe) 0xfff else entry.path.size
            output.add(uint16Bytes(pathLength))
            output.add(entry.path)
            output.addByte(0)
            while ((output.length - entryOffset) % 8 != 0) {
                output.addByte(0)
            }
        }
        val withoutChecksum = output.takeBytes()
        val digest = ForkMeshSha1()
        digest.add(withoutChecksum)
        return withoutChecksum + digest.close()
    }

    companion object {
        fun parse(input: ByteArray, limits: GitResourceLimits): GitIndexV2 {
            val bytes = input
            if (bytes.size < 32) {
                throw GitException(GitErrorCode.indexCorrupt, "index is truncated")
            }
            val checksumOffset = bytes.size - 20
            requireDigest(bytes, checksumOffset)
            if (!(bytes[0] == 'D'.code.toByte() && bytes[1] == 'I'.code.toByte() &&
                    bytes[2] == 'R'.code.toByte() && bytes[3] == 'C'.code.toByte())
            ) {
                throw GitException(GitErrorCode.indexCorrupt, "index signature is invalid")
            }
            if (bytes.readUint32(4) != 2L) {
                throw GitException(GitErrorCode.unsupportedRepositoryFormat, "only index version 2 is supported")
            }
            val count = bytes.readUint32(8)
            if (count > GitResourceLimits.hardMaxRefCount || count > limits.maxRefCount) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "index entry count exceeds configured limit")
            }
            var offset = 12
            val entries = ArrayList<GitIndexEntry>(count.toInt())
            for (item in 0 until count.toInt()) {
                val entryOffset = offset
                if (checksumOffset - offset < 62) {
                    throw GitException(GitErrorCode.indexCorrupt, "index entry is truncated")
                }
                val mode = GitIndexMode.parse(bytes.readUint32(offset + 24))
                val size = bytes.readUint32(offset + 36)
                val id = GitObjectId.fromSha1Bytes(bytes, offset + 40)
                val flags = bytes.readUint16(offset + 60)
                val stage = (flags shr 12) and 0x3
                if (stage != 0 || flags and 0xc000 != 0) {
                    throw GitException(
                        GitErrorCode.unsupportedRepositoryFormat,
                        "conflict or extended index entry is unsupported",
                    )
                }
                val pathStart = offset + 62
                val nul = bytes.indexOfByte(0, pathStart)
                if (nul < 0 || nul >= checksumOffset) {
                    throw GitException(GitErrorCode.indexCorrupt, "index path is unterminated")
                }
                val path = bytes.copyOfRange(pathStart, nul)
                val declaredLength = flags and 0x0fff
                if (declaredLength != 0x0fff && declaredLength != path.size) {
                    throw GitException(GitErrorCode.indexCorrupt, "index path length flag is invalid")
                }
                val entryLength = nul + 1 - entryOffset
                offset = nul + 1 + ((8 - entryLength % 8) % 8)
                if (offset > checksumOffset) {
                    throw GitException(GitErrorCode.indexCorrupt, "index entry padding is truncated")
                }
                entries.add(GitIndexEntry(path = path, objectId = id, mode = mode, size = size, stage = stage))
            }
            skipExtensions(bytes, offset, checksumOffset, limits)
            val parsed = GitIndexV2(entries, true)
            for (index in 1 until parsed.entries.size) {
                if (comparePaths(parsed.entries[index - 1], parsed.entries[index]) >= 0) {
                    throw GitException(GitErrorCode.indexCorrupt, "index entries are not strictly sorted")
                }
            }
            return parsed
        }
    }
}

private fun skipExtensions(bytes: ByteArray, start: Int, checksumOffset: Int, limits: GitResourceLimits) {
    var offset = start
    while (offset < checksumOffset) {
        if (checksumOffset - offset < 8) {
            throw GitException(GitErrorCode.indexCorrupt, "index extension header is truncated")
        }
        val signatureFirst = bytes[offset].toInt() and 0xff
        val length = bytes.readUint32(offset + 4)
        if (length > limits.maxInflatedBytes || length > checksumOffset - offset - 8) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "index extension length is invalid")
        }
        if (signatureFirst < 0x41 || signatureFirst > 0x5a) {
            throw GitException(GitErrorCode.unsupportedRepositoryFormat, "unknown mandatory index extension")
        }
        offset += 8 + length.toInt()
    }
}

private fun validatePath(path: ByteArray) {
    if (path.isEmpty() || path.size > 4096 || path.indexOfByte(0) != -1) {
        throw GitException(GitErrorCode.pathEscapesWorktree, "invalid path")
    }
    val value = try {
        utf8Decode(path)
    } catch (error: DartFormatException) {
        throw GitException(GitErrorCode.pathEscapesWorktree, "path is not valid UTF-8")
    }
    if (value.startsWith("/") ||
        value.startsWith("\\") ||
        value.contains("\\") ||
        value.split("/").any { part -> part.isEmpty() || part == "." || part == ".." }
    ) {
        throw GitException(GitErrorCode.pathEscapesWorktree, "path escapes worktree")
    }
}

private fun comparePaths(left: GitIndexEntry, right: GitIndexEntry): Int {
    val leftPath = left.path
    val rightPath = right.path
    var index = 0
    while (index < leftPath.size && index < rightPath.size) {
        val comparison = (leftPath[index].toInt() and 0xff) - (rightPath[index].toInt() and 0xff)
        if (comparison != 0) return comparison
        index += 1
    }
    return leftPath.size - rightPath.size
}

private fun requireDigest(bytes: ByteArray, checksumOffset: Int) {
    val digest = ForkMeshSha1()
    digest.add(bytes, 0, checksumOffset)
    if (!constantTimeEquals(digest.close(), bytes.copyOfRange(checksumOffset, bytes.size))) {
        throw GitException(GitErrorCode.indexCorrupt, "index checksum does not match")
    }
}
