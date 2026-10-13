package com.forkmesh.nativeapp.git.pack

import com.forkmesh.nativeapp.git.bytes.readUint32
import com.forkmesh.nativeapp.git.crypto.ForkMeshSha1
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import java.util.Collections

class GitPackIndexEntry(val objectId: GitObjectId, val crc32: Long, val offset: Long)

class GitPackIndexV2 private constructor(private val indexEntries: List<GitPackIndexEntry>, packChecksum: ByteArray) {
    val packChecksum: ByteArray = packChecksum.copyOf()

    val entries: List<GitPackIndexEntry> get() = Collections.unmodifiableList(indexEntries)

    fun lookup(objectId: GitObjectId): GitPackIndexEntry? {
        var low = 0
        var high = indexEntries.size - 1
        while (low <= high) {
            val middle = (low + high) / 2
            val comparison = indexEntries[middle].objectId.compareHex(objectId)
            if (comparison == 0) return indexEntries[middle]
            if (comparison < 0) {
                low = middle + 1
            } else {
                high = middle - 1
            }
        }
        return null
    }

    companion object {
        fun parse(input: ByteArray, limits: GitResourceLimits): GitPackIndexV2 {
            val bytes = input
            val headerLength = 8 + 256 * 4
            if (bytes.size < headerLength + 40) {
                throw GitException(GitErrorCode.packCorrupt, "pack index is truncated")
            }
            if (bytes[0] != 0xff.toByte() ||
                bytes[1] != 0x74.toByte() ||
                bytes[2] != 0x4f.toByte() ||
                bytes[3] != 0x63.toByte() ||
                bytes.readUint32(4) != 2L
            ) {
                throw GitException(GitErrorCode.unsupportedRepositoryFormat, "only pack index version 2 is supported")
            }
            verifyIndexChecksum(bytes)
            var previous = 0L
            for (index in 0 until 256) {
                val value = bytes.readUint32(8 + index * 4)
                if (value < previous) {
                    throw GitException(GitErrorCode.packCorrupt, "pack index fanout is not monotonic")
                }
                previous = value
            }
            if (previous > limits.maxRefCount || previous > GitResourceLimits.hardMaxRefCount) {
                throw GitException(
                    GitErrorCode.resourceLimitExceeded,
                    "pack index object count exceeds configured limit",
                )
            }
            val count = previous.toInt()
            val fixedLength = headerLength + count * 28 + 40
            if (fixedLength > bytes.size || (bytes.size - fixedLength) % 8 != 0) {
                throw GitException(GitErrorCode.packCorrupt, "pack index size is invalid")
            }
            val nameOffset = headerLength
            val crcOffset = nameOffset + count * 20
            val offsetOffset = crcOffset + count * 4
            val largeOffsetOffset = offsetOffset + count * 4
            val largeOffsetCount = (bytes.size - fixedLength) / 8
            val entries = ArrayList<GitPackIndexEntry>(count)
            var previousId: GitObjectId? = null
            for (index in 0 until count) {
                val id = GitObjectId.fromSha1Bytes(bytes, nameOffset + index * 20)
                if (previousId != null && previousId.compareHex(id) >= 0) {
                    throw GitException(GitErrorCode.packCorrupt, "pack index object IDs are not strictly sorted")
                }
                val packedOffset = bytes.readUint32(offsetOffset + index * 4)
                val offset = if (packedOffset and 0x80000000L == 0L) {
                    packedOffset
                } else {
                    readLargeOffset(bytes, largeOffsetOffset, (packedOffset and 0x7fffffffL).toInt(), largeOffsetCount)
                }
                entries.add(GitPackIndexEntry(id, bytes.readUint32(crcOffset + index * 4), offset))
                previousId = id
            }
            return GitPackIndexV2(entries, bytes.copyOfRange(bytes.size - 40, bytes.size - 20))
        }
    }
}

private fun readLargeOffset(bytes: ByteArray, start: Int, index: Int, count: Int): Long {
    if (index >= count) {
        throw GitException(GitErrorCode.packCorrupt, "pack index large offset is invalid")
    }
    val offset = start + index * 8
    return (bytes.readUint32(offset) shl 32) or bytes.readUint32(offset + 4)
}

private fun verifyIndexChecksum(bytes: ByteArray) {
    val checksumOffset = bytes.size - 20
    val digest = ForkMeshSha1()
    digest.add(bytes, 0, checksumOffset)
    val actual = digest.close()
    var different = 0
    for (index in 0 until 20) {
        different = different or (actual[index].toInt() xor bytes[checksumOffset + index].toInt())
    }
    if (different != 0) {
        throw GitException(GitErrorCode.packChecksumMismatch, "pack index checksum does not match")
    }
}
