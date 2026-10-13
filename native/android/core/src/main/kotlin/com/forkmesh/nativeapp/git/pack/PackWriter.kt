package com.forkmesh.nativeapp.git.pack

import com.forkmesh.nativeapp.git.bytes.GitBytesBuilder
import com.forkmesh.nativeapp.git.bytes.asciiEncode
import com.forkmesh.nativeapp.git.bytes.uint32Bytes
import com.forkmesh.nativeapp.git.bytes.zlibEncode
import com.forkmesh.nativeapp.git.crypto.ForkMeshSha1
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectFrame
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.objects.GitObjectType
import com.forkmesh.nativeapp.git.security.GitCancellationToken

class GitPackWriteObject(val type: GitObjectType, body: ByteArray) {
    val body: ByteArray = body.copyOf()
    val objectId: GitObjectId = GitObjectId.sha1Of(GitObjectFrame.encode(type, this.body))

    init {
        if (this.body.size > 256 * 1024 * 1024) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "pack object exceeds hard limit")
        }
    }
}

class GitWrittenPack internal constructor(val packBytes: ByteArray, val indexBytes: ByteArray)

object GitPackWriter {
    fun write(objects: Iterable<GitPackWriteObject>, cancellation: GitCancellationToken? = null): GitWrittenPack {
        cancellation?.throwIfCancelled()
        val values = objects.toList()
        val knownIds = HashSet<GitObjectId>()
        for (item in values) {
            if (!knownIds.add(item.objectId)) {
                throw GitException(GitErrorCode.packCorrupt, "pack write objects contain a duplicate object ID")
            }
        }
        val pack = GitBytesBuilder()
        pack.add(asciiEncode("PACK"))
        pack.add(uint32Bytes(2))
        pack.add(uint32Bytes(values.size.toLong()))
        val indexed = ArrayList<GitPackIndexWriteEntry>(values.size)
        for (item in values) {
            cancellation?.throwIfCancelled()
            val offset = pack.length
            val header = objectHeader(item.type, item.body.size)
            val compressed = zlibEncode(item.body)
            val objectBytes = GitBytesBuilder(header.size + compressed.size)
            objectBytes.add(header)
            objectBytes.add(compressed)
            val encoded = objectBytes.takeBytes()
            pack.add(encoded)
            indexed.add(GitPackIndexWriteEntry(item.objectId, offset = offset.toLong(), crc32 = crc32(encoded)))
        }
        cancellation?.throwIfCancelled()
        val beforeTrailer = pack.takeBytes()
        val digest = ForkMeshSha1()
        digest.add(beforeTrailer)
        val packChecksum = digest.close()
        val packBytes = beforeTrailer + packChecksum
        val indexBytes = GitPackIndexWriter.write(indexed, packChecksum)
        return GitWrittenPack(packBytes, indexBytes)
    }
}

class GitPackIndexWriteEntry(val objectId: GitObjectId, val offset: Long, val crc32: Long)

object GitPackIndexWriter {
    fun write(input: Iterable<GitPackIndexWriteEntry>, packChecksum: ByteArray): ByteArray {
        if (packChecksum.size != 20) {
            throw GitException(GitErrorCode.packCorrupt, "pack checksum must be 20 bytes")
        }
        val entries = input.sortedWith { left, right -> left.objectId.compareHex(right.objectId) }
        for (index in 1 until entries.size) {
            if (entries[index - 1].objectId == entries[index].objectId) {
                throw GitException(GitErrorCode.packCorrupt, "pack index entries contain a duplicate object ID")
            }
        }
        val fanout = LongArray(256)
        for (entry in entries) {
            fanout[entry.objectId.firstByte] += 1
        }
        for (index in 1 until fanout.size) {
            fanout[index] += fanout[index - 1]
        }
        val output = GitBytesBuilder(8 + 256 * 4 + entries.size * 28 + 40)
        output.add(byteArrayOf(0xff.toByte(), 0x74, 0x4f, 0x63))
        output.add(uint32Bytes(2))
        for (value in fanout) {
            output.add(uint32Bytes(value))
        }
        for (entry in entries) {
            output.add(entry.objectId.rawBytes)
        }
        for (entry in entries) {
            output.add(uint32Bytes(entry.crc32))
        }
        for (entry in entries) {
            if (entry.offset < 0 || entry.offset >= 0x80000000L) {
                throw GitException(
                    GitErrorCode.resourceLimitExceeded,
                    "large pack offsets are unsupported by the initial writer",
                )
            }
            output.add(uint32Bytes(entry.offset))
        }
        output.add(packChecksum)
        val beforeChecksum = output.takeBytes()
        val indexDigest = ForkMeshSha1()
        indexDigest.add(beforeChecksum)
        return beforeChecksum + indexDigest.close()
    }
}

private fun objectHeader(type: GitObjectType, size: Int): ByteArray {
    val typeCode = when (type) {
        GitObjectType.commit -> 1
        GitObjectType.tree -> 2
        GitObjectType.blob -> 3
        GitObjectType.tag -> 4
    }
    val output = GitBytesBuilder(10)
    var remaining = size.toLong()
    var first = (typeCode shl 4) or (remaining and 0x0f).toInt()
    remaining = remaining shr 4
    if (remaining != 0L) first = first or 0x80
    output.addByte(first)
    while (remaining != 0L) {
        var byte = (remaining and 0x7f).toInt()
        remaining = remaining shr 7
        if (remaining != 0L) byte = byte or 0x80
        output.addByte(byte)
    }
    return output.takeBytes()
}
