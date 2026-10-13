package com.forkmesh.nativeapp.git.pack

import com.forkmesh.nativeapp.git.bytes.GitBytesBuilder
import com.forkmesh.nativeapp.git.bytes.asciiDecodeAllowInvalid
import com.forkmesh.nativeapp.git.bytes.readUint32
import com.forkmesh.nativeapp.git.crypto.ForkMeshSha1
import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.crypto.GitSha1ObjectOrigin
import com.forkmesh.nativeapp.git.crypto.Sha1CollisionScreen
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.objects.GitObjectType
import com.forkmesh.nativeapp.git.security.GitCancellationToken
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import java.util.zip.DataFormatException
import java.util.zip.Inflater

class GitPackObject internal constructor(
    val offset: Long,
    val type: GitObjectType,
    val body: ByteArray,
    val objectId: GitObjectId,
    val crc32: Long,
)

class GitPack internal constructor(val objects: List<GitPackObject>, private val checksumBytes: ByteArray) {
    val checksum: ByteArray get() = checksumBytes.copyOf()

    private val byId: Map<GitObjectId, GitPackObject> by lazy {
        val map = HashMap<GitObjectId, GitPackObject>(objects.size * 2)
        for (item in objects) map.putIfAbsent(item.objectId, item)
        map
    }

    fun lookup(objectId: GitObjectId): GitPackObject? = byId[objectId]
}

object GitPackReader {
    fun parse(
        input: ByteArray,
        limits: GitResourceLimits,
        origin: GitSha1ObjectOrigin = GitSha1ObjectOrigin.trustedLocal,
        cancellation: GitCancellationToken? = null,
        detector: GitSha1CollisionDetector? = null,
        progress: ((resolved: Int, total: Int) -> Unit)? = null,
    ): GitPack {
        cancellation?.throwIfCancelled()
        val bytes = input
        if (bytes.size < 32) {
            throw GitException(GitErrorCode.packCorrupt, "pack is truncated")
        }
        val trailerOffset = bytes.size - 20
        verifyTrailer(bytes, trailerOffset)
        if (asciiDecodeAllowInvalid(bytes, 0, 4) != "PACK") {
            throw GitException(GitErrorCode.packCorrupt, "pack signature is invalid")
        }
        if (bytes.readUint32(4) != 2L) {
            throw GitException(GitErrorCode.unsupportedRepositoryFormat, "only pack version 2 is supported")
        }
        val count = bytes.readUint32(8)
        if (count > GitResourceLimits.hardMaxRefCount || count > limits.maxRefCount) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "pack object count exceeds configured limit")
        }
        var offset = 12
        val entries = ArrayList<PackEntry>(count.toInt())
        val inflater = Inflater()
        try {
            for (index in 0 until count.toInt()) {
                if (offset >= trailerOffset) {
                    throw GitException(GitErrorCode.packCorrupt, "pack object header is truncated")
                }
                val objectOffset = offset
                val header = readObjectHeader(bytes, offset, trailerOffset, limits)
                offset = header.nextOffset
                var baseOffset: Int? = null
                var baseId: GitObjectId? = null
                if (header.kind == PackObjectKind.ofsDelta) {
                    val parsed = readOffsetDeltaBase(bytes, offset, trailerOffset)
                    offset = parsed.nextOffset
                    val base = objectOffset.toLong() - parsed.distance
                    if (base < 12) {
                        throw GitException(GitErrorCode.packCorrupt, "offset delta base is before pack data")
                    }
                    baseOffset = base.toInt()
                } else if (header.kind == PackObjectKind.refDelta) {
                    if (trailerOffset - offset < 20) {
                        throw GitException(GitErrorCode.packCorrupt, "reference delta base is truncated")
                    }
                    baseId = GitObjectId.fromSha1Bytes(bytes, offset)
                    offset += 20
                }
                val inflated = inflateEntry(inflater, bytes, offset, trailerOffset, header.size, limits)
                entries.add(
                    PackEntry(
                        offset = objectOffset,
                        kind = header.kind,
                        declaredSize = header.size,
                        body = inflated.body,
                        crc32 = crc32(bytes, objectOffset, inflated.end),
                        baseOffset = baseOffset,
                        baseId = baseId,
                    ),
                )
                offset = inflated.end
            }
        } finally {
            inflater.end()
        }
        if (offset != trailerOffset) {
            throw GitException(GitErrorCode.packCorrupt, "pack has trailing object data")
        }
        val byOffset = HashMap<Int, PackEntry>(entries.size * 2)
        for (entry in entries) byOffset[entry.offset] = entry
        val state = ResolutionState(limits, byOffset, entries, origin, cancellation, detector)
        val objects = ArrayList<GitPackObject>(entries.size)
        progress?.invoke(0, entries.size)
        for ((index, entry) in entries.withIndex()) {
            objects.add(state.resolve(entry, 0))
            if (progress != null && ((index + 1) % 256 == 0 || index + 1 == entries.size)) {
                progress(index + 1, entries.size)
            }
        }
        return GitPack(objects, bytes.copyOfRange(trailerOffset, bytes.size))
    }
}

@Suppress("EnumEntryName")
internal enum class PackObjectKind { commit, tree, blob, tag, ofsDelta, refDelta }

internal class ObjectHeader(val kind: PackObjectKind, val size: Int, val nextOffset: Int)

internal fun readObjectHeader(bytes: ByteArray, start: Int, limit: Int, limits: GitResourceLimits): ObjectHeader {
    var offset = start
    var current = readByte(bytes, offset++, limit)
    val kind = when ((current shr 4) and 7) {
        1 -> PackObjectKind.commit
        2 -> PackObjectKind.tree
        3 -> PackObjectKind.blob
        4 -> PackObjectKind.tag
        6 -> PackObjectKind.ofsDelta
        7 -> PackObjectKind.refDelta
        else -> throw GitException(GitErrorCode.packCorrupt, "pack object type is invalid")
    }
    var size = (current and 0x0f).toLong()
    var shift = 4
    while (current and 0x80 != 0) {
        if (shift > 56 || offset >= limit) {
            throw GitException(GitErrorCode.packCorrupt, "pack object size is invalid")
        }
        current = readByte(bytes, offset++, limit)
        size = size or ((current and 0x7f).toLong() shl shift)
        shift += 7
    }
    if (size > limits.maxObjectBytes) {
        throw GitException(GitErrorCode.resourceLimitExceeded, "pack object exceeds configured limit")
    }
    return ObjectHeader(kind, size.toInt(), offset)
}

internal class OffsetDeltaBase(val distance: Long, val nextOffset: Int)

internal fun readOffsetDeltaBase(bytes: ByteArray, start: Int, limit: Int): OffsetDeltaBase {
    var offset = start
    var current = readByte(bytes, offset++, limit)
    var distance = (current and 0x7f).toLong()
    while (current and 0x80 != 0) {
        if (offset >= limit || distance > 0x00ffffffffffffffL) {
            throw GitException(GitErrorCode.packCorrupt, "offset delta distance is invalid")
        }
        current = readByte(bytes, offset++, limit)
        distance = ((distance + 1) shl 7) or (current and 0x7f).toLong()
    }
    if (distance == 0L) {
        throw GitException(GitErrorCode.packCorrupt, "offset delta distance is zero")
    }
    return OffsetDeltaBase(distance, offset)
}

private class PackEntry(
    val offset: Int,
    val kind: PackObjectKind,
    val declaredSize: Int,
    val body: ByteArray,
    val crc32: Long,
    val baseOffset: Int?,
    val baseId: GitObjectId?,
) {
    var resolved: GitPackObject? = null
    var resolving = false
}

private class ResolutionState(
    val limits: GitResourceLimits,
    val byOffset: Map<Int, PackEntry>,
    val entries: List<PackEntry>,
    val origin: GitSha1ObjectOrigin,
    val cancellation: GitCancellationToken?,
    val detector: GitSha1CollisionDetector?,
) {
    val byId = HashMap<GitObjectId, PackEntry>()
    var totalBytes = 0L

    fun resolve(entry: PackEntry, depth: Int): GitPackObject {
        cancellation?.throwIfCancelled()
        entry.resolved?.let { return it }
        if (depth >= 32) {
            throw GitException(GitErrorCode.deltaDepthExceeded, "pack delta chain exceeds depth limit")
        }
        if (entry.resolving) {
            throw GitException(GitErrorCode.packCorrupt, "pack delta cycle")
        }
        entry.resolving = true
        try {
            val type: GitObjectType
            val body: ByteArray
            when (entry.kind) {
                PackObjectKind.commit -> {
                    type = GitObjectType.commit
                    body = entry.body
                }
                PackObjectKind.tree -> {
                    type = GitObjectType.tree
                    body = entry.body
                }
                PackObjectKind.blob -> {
                    type = GitObjectType.blob
                    body = entry.body
                }
                PackObjectKind.tag -> {
                    type = GitObjectType.tag
                    body = entry.body
                }
                PackObjectKind.ofsDelta, PackObjectKind.refDelta -> {
                    val base = deltaBase(entry, depth)
                    type = base.type
                    body = applyDelta(base.body, entry.body, limits)
                }
            }
            val screening = Sha1CollisionScreen(detector = detector).screenObject(
                type = type,
                body = body,
                origin = origin,
                limits = limits,
                cancellation = cancellation,
            )
            if (!screening.approved) throw screening.toException()
            val result = GitPackObject(
                offset = entry.offset.toLong(),
                type = type,
                body = body,
                objectId = GitObjectId.fromSha1Bytes(screening.digest),
                crc32 = entry.crc32,
            )
            entry.resolved = result
            byId[result.objectId] = entry
            addToTotal(body.size)
            return result
        } finally {
            entry.resolving = false
        }
    }

    private fun deltaBase(entry: PackEntry, depth: Int): GitPackObject {
        var baseEntry: PackEntry? = null
        if (entry.baseOffset != null) {
            baseEntry = byOffset[entry.baseOffset]
        } else if (entry.baseId != null) {
            baseEntry = byId[entry.baseId] ?: findAndResolve(entry.baseId, depth + 1)
        }
        if (baseEntry == null) {
            throw GitException(GitErrorCode.unsupportedRepositoryFormat, "thin packs are unsupported")
        }
        return resolve(baseEntry, depth + 1)
    }

    private fun findAndResolve(id: GitObjectId, depth: Int): PackEntry? {
        for (candidate in entries) {
            val resolvedObject = resolve(candidate, depth)
            if (resolvedObject.objectId == id) return candidate
        }
        return null
    }

    private fun addToTotal(byteLength: Int) {
        totalBytes += byteLength
        if (totalBytes > limits.maxInflatedBytes) {
            throw GitException(GitErrorCode.resourceLimitExceeded, "resolved pack bytes exceed configured limit")
        }
    }
}

internal fun applyDelta(base: ByteArray, delta: ByteArray, limits: GitResourceLimits): ByteArray {
    val reader = DeltaReader(delta)
    val sourceSize = reader.readVariableInteger()
    val targetSize = reader.readVariableInteger()
    if (sourceSize != base.size.toLong() || targetSize > limits.maxObjectBytes) {
        throw GitException(GitErrorCode.packCorrupt, "delta header is invalid")
    }
    val output = GitBytesBuilder(minOf(targetSize, 1L shl 20).toInt())
    while (!reader.isAtEnd) {
        val instruction = reader.readByte()
        if (instruction == 0) {
            throw GitException(GitErrorCode.packCorrupt, "delta instruction is zero")
        }
        if (instruction and 0x80 == 0) {
            if (output.length.toLong() + instruction > targetSize) {
                reader.readBytes(instruction)
                throw GitException(GitErrorCode.packCorrupt, "delta insert exceeds target size")
            }
            reader.copyBytes(instruction, output)
            continue
        }
        var sourceOffset = 0L
        var size = 0
        if (instruction and 0x01 != 0) sourceOffset = sourceOffset or reader.readByte().toLong()
        if (instruction and 0x02 != 0) sourceOffset = sourceOffset or (reader.readByte().toLong() shl 8)
        if (instruction and 0x04 != 0) sourceOffset = sourceOffset or (reader.readByte().toLong() shl 16)
        if (instruction and 0x08 != 0) sourceOffset = sourceOffset or (reader.readByte().toLong() shl 24)
        if (instruction and 0x10 != 0) size = size or reader.readByte()
        if (instruction and 0x20 != 0) size = size or (reader.readByte() shl 8)
        if (instruction and 0x40 != 0) size = size or (reader.readByte() shl 16)
        if (size == 0) size = 0x10000
        if (sourceOffset < 0 ||
            size < 0 ||
            sourceOffset > base.size ||
            size > base.size - sourceOffset ||
            output.length.toLong() + size > targetSize
        ) {
            throw GitException(GitErrorCode.packCorrupt, "delta copy is invalid")
        }
        output.add(base, sourceOffset.toInt(), size)
    }
    if (output.length.toLong() != targetSize) {
        throw GitException(GitErrorCode.packCorrupt, "delta result size does not match header")
    }
    return output.takeBytes()
}

private class DeltaReader(val bytes: ByteArray) {
    var offset = 0

    val isAtEnd: Boolean get() = offset == bytes.size

    fun readByte(): Int {
        if (isAtEnd) {
            throw GitException(GitErrorCode.packCorrupt, "delta is truncated")
        }
        return bytes[offset++].toInt() and 0xff
    }

    fun readBytes(length: Int): ByteArray {
        if (length < 0 || length > bytes.size - offset) {
            throw GitException(GitErrorCode.packCorrupt, "delta is truncated")
        }
        val result = bytes.copyOfRange(offset, offset + length)
        offset += length
        return result
    }

    fun copyBytes(length: Int, output: GitBytesBuilder) {
        if (length < 0 || length > bytes.size - offset) {
            throw GitException(GitErrorCode.packCorrupt, "delta is truncated")
        }
        output.add(bytes, offset, length)
        offset += length
    }

    fun readVariableInteger(): Long {
        var value = 0L
        var shift = 0
        while (true) {
            if (shift > 56) {
                throw GitException(GitErrorCode.packCorrupt, "delta variable integer is invalid")
            }
            val byte = readByte()
            value = value or ((byte and 0x7f).toLong() shl shift)
            if (byte and 0x80 == 0) return value
            shift += 7
        }
    }
}

internal class InflatedEntry(val body: ByteArray, val end: Int)

internal fun inflateEntry(
    inflater: Inflater,
    bytes: ByteArray,
    start: Int,
    limit: Int,
    declaredSize: Int,
    limits: GitResourceLimits,
): InflatedEntry {
    if (limit - start < 6) {
        throw GitException(GitErrorCode.packCorrupt, "zlib stream is truncated")
    }
    val cmf = bytes[start].toInt() and 0xff
    val flg = bytes[start + 1].toInt() and 0xff
    if (cmf and 0x0f != 8 || cmf shr 4 > 7 || ((cmf shl 8) or flg) % 31 != 0 || flg and 0x20 != 0) {
        throw GitException(GitErrorCode.packCorrupt, "zlib header is invalid")
    }
    inflater.reset()
    inflater.setInput(bytes, start, limit - start)
    val capacityLimit = declaredSize + 1
    var output = ByteArray(minOf(capacityLimit, maxOf(64, (limit - start).toLong().times(4).coerceAtMost(1L shl 24).toInt())))
    var produced = 0
    while (true) {
        val count = try {
            inflater.inflate(output, produced, output.size - produced)
        } catch (error: DataFormatException) {
            throw GitException(GitErrorCode.packCorrupt, "pack zlib stream is invalid")
        }
        produced += count
        if (inflater.finished()) break
        if (produced == output.size) {
            if (output.size >= capacityLimit) {
                throw GitException(GitErrorCode.packCorrupt, "pack object size does not match header")
            }
            output = output.copyOf(minOf(capacityLimit.toLong(), output.size.toLong() * 2).toInt())
            continue
        }
        if (count == 0) {
            if (inflater.needsInput()) {
                throw GitException(GitErrorCode.packCorrupt, "deflate stream is truncated")
            }
            throw GitException(GitErrorCode.packCorrupt, "pack zlib stream is invalid")
        }
    }
    if (produced != declaredSize || produced > limits.maxObjectBytes) {
        throw GitException(GitErrorCode.packCorrupt, "pack object size does not match header")
    }
    val end = limit - inflater.remaining
    return InflatedEntry(if (output.size == produced) output else output.copyOf(produced), end)
}

private fun verifyTrailer(bytes: ByteArray, trailerOffset: Int) {
    val digest = ForkMeshSha1()
    digest.add(bytes, 0, trailerOffset)
    val actual = digest.close()
    var different = 0
    for (index in 0 until 20) {
        different = different or (actual[index].toInt() xor bytes[trailerOffset + index].toInt())
    }
    if (different != 0) {
        throw GitException(GitErrorCode.packChecksumMismatch, "pack trailer checksum does not match")
    }
}

private fun readByte(bytes: ByteArray, offset: Int, limit: Int): Int {
    if (offset >= limit) {
        throw GitException(GitErrorCode.packCorrupt, "pack is truncated")
    }
    return bytes[offset].toInt() and 0xff
}
