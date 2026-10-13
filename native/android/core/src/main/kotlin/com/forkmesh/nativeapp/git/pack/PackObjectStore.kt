package com.forkmesh.nativeapp.git.pack

import com.forkmesh.nativeapp.git.bytes.constantTimeEquals
import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.crypto.GitSha1ObjectOrigin
import com.forkmesh.nativeapp.git.crypto.Sha1CollisionScreen
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectFrame
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.objects.GitObjectType
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.git.worktree.FileSystemEntityType
import com.forkmesh.nativeapp.git.worktree.directoryExists
import com.forkmesh.nativeapp.git.worktree.entityType
import com.forkmesh.nativeapp.git.worktree.fileExists
import com.forkmesh.nativeapp.git.worktree.listEntries
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.channels.FileChannel
import java.util.zip.Inflater

class GitPackObjectStore(
    val gitDirectory: File,
    val limits: GitResourceLimits,
    val detector: GitSha1CollisionDetector? = null,
) {
    private var indexFileList: List<File>? = null
    private val indexes = HashMap<String, GitPackIndexV2>()
    private val packs = HashMap<String, MappedPack>()
    private val caches = HashMap<String, PackDeltaBaseCache>()

    private val packDirectory: File get() = File(File(gitDirectory, "objects"), "pack")

    @Synchronized
    fun invalidate() {
        indexFileList = null
        indexes.clear()
        packs.clear()
        caches.clear()
    }

    @Synchronized
    fun contains(objectId: GitObjectId): Boolean = locate(objectId) != null

    @Synchronized
    fun read(objectId: GitObjectId): GitObjectFrame {
        val found = locate(objectId) ?: throw GitException(GitErrorCode.invalidObject, "object is absent")
        val packPath = "${found.file.path.substring(0, found.file.path.length - 4)}.pack"
        val pack = packFor(File(packPath), found.index)
        val cache = caches.getOrPut(packPath) { PackDeltaBaseCache() }
        val inflater = Inflater()
        val resolved = try {
            resolve(pack, found.entry.offset, found.index, cache, inflater)
        } finally {
            inflater.end()
        }
        val screening = Sha1CollisionScreen(detector = detector).screenObject(
            type = resolved.type,
            body = resolved.body,
            origin = GitSha1ObjectOrigin.trustedLocal,
            limits = limits,
        )
        if (!screening.approved) throw screening.toException()
        if (GitObjectId.fromSha1Bytes(screening.digest) != objectId) {
            throw GitException(GitErrorCode.objectHashMismatch, "packed object does not match its index entry")
        }
        return GitObjectFrame(resolved.type, resolved.body)
    }

    private fun indexFiles(): List<File> {
        indexFileList?.let { return it }
        val directory = packDirectory
        val files = if (directoryExists(directory)) {
            listEntries(directory).filter { entity ->
                entity.path.endsWith(".idx") && entityType(entity) == FileSystemEntityType.file
            }
        } else {
            emptyList()
        }
        indexFileList = files
        return files
    }

    private fun locate(objectId: GitObjectId): Located? {
        val listedBefore = indexFileList != null
        search(objectId)?.let { return it }
        if (!listedBefore) return null
        indexFileList = null
        return search(objectId)
    }

    private fun search(objectId: GitObjectId): Located? {
        for (file in indexFiles()) {
            val index = indexFor(file)
            val entry = index.lookup(objectId) ?: continue
            return Located(file, index, entry)
        }
        return null
    }

    private fun indexFor(file: File): GitPackIndexV2 {
        indexes[file.path]?.let { return it }
        val index = GitPackIndexV2.parse(file.readBytes(), limits)
        indexes[file.path] = index
        return index
    }

    private fun packFor(packFile: File, index: GitPackIndexV2): MappedPack {
        packs[packFile.path]?.let { return it }
        if (!fileExists(packFile)) {
            throw GitException(GitErrorCode.packCorrupt, "pack file is missing")
        }
        val buffer = try {
            RandomAccessFile(packFile, "r").use { file ->
                if (file.length() > Int.MAX_VALUE) {
                    throw GitException(GitErrorCode.resourceLimitExceeded, "pack file exceeds configured limit")
                }
                file.channel.map(FileChannel.MapMode.READ_ONLY, 0, file.length())
            }
        } catch (error: IOException) {
            throw GitException(GitErrorCode.packCorrupt, "pack file cannot be read")
        }
        val size = buffer.capacity()
        if (size < 32 || !constantTimeEquals(index.packChecksum, copyRange(buffer, size - 20, size))) {
            throw GitException(GitErrorCode.packChecksumMismatch, "pack and index checksums do not match")
        }
        val header = copyRange(buffer, 0, 8)
        if (header[0] != 'P'.code.toByte() ||
            header[1] != 'A'.code.toByte() ||
            header[2] != 'C'.code.toByte() ||
            header[3] != 'K'.code.toByte()
        ) {
            throw GitException(GitErrorCode.packCorrupt, "pack signature is invalid")
        }
        if (header[4].toInt() != 0 || header[5].toInt() != 0 || header[6].toInt() != 0 || header[7].toInt() != 2) {
            throw GitException(GitErrorCode.unsupportedRepositoryFormat, "only pack version 2 is supported")
        }
        val entries = index.entries
        val offsets = LongArray(entries.size) { entries[it].offset }
        offsets.sort()
        val pack = MappedPack(buffer, size - 20, offsets)
        packs[packFile.path] = pack
        return pack
    }

    private fun resolve(
        pack: MappedPack,
        offset: Long,
        index: GitPackIndexV2,
        cache: PackDeltaBaseCache,
        inflater: Inflater,
    ): PackResolvedObject {
        val chain = ArrayList<Pair<Long, ByteArray>>()
        val visited = HashSet<Long>()
        var current = offset
        var base: PackResolvedObject
        while (true) {
            val cached = cache.lookup(current)
            if (cached != null) {
                base = cached
                break
            }
            if (!visited.add(current)) {
                throw GitException(GitErrorCode.packCorrupt, "pack delta cycle")
            }
            if (chain.size >= maxDeltaChain) {
                throw GitException(GitErrorCode.deltaDepthExceeded, "pack delta chain exceeds depth limit")
            }
            if (current < 12 || current >= pack.trailer) {
                throw GitException(GitErrorCode.packCorrupt, "pack object offset is invalid")
            }
            val bytes = pack.entryBytes(current.toInt())
            val header = readObjectHeader(bytes, 0, bytes.size, limits)
            var dataOffset = header.nextOffset
            var next: Long? = null
            var type = GitObjectType.blob
            when (header.kind) {
                PackObjectKind.commit -> type = GitObjectType.commit
                PackObjectKind.tree -> type = GitObjectType.tree
                PackObjectKind.blob -> type = GitObjectType.blob
                PackObjectKind.tag -> type = GitObjectType.tag
                PackObjectKind.ofsDelta -> {
                    val parsed = readOffsetDeltaBase(bytes, dataOffset, bytes.size)
                    dataOffset = parsed.nextOffset
                    val baseOffset = current - parsed.distance
                    if (baseOffset < 12) {
                        throw GitException(GitErrorCode.packCorrupt, "offset delta base is before pack data")
                    }
                    next = baseOffset
                }
                PackObjectKind.refDelta -> {
                    if (bytes.size - dataOffset < 20) {
                        throw GitException(GitErrorCode.packCorrupt, "reference delta base is truncated")
                    }
                    val baseId = GitObjectId.fromSha1Bytes(bytes, dataOffset)
                    dataOffset += 20
                    val baseEntry = index.lookup(baseId)
                        ?: throw GitException(GitErrorCode.unsupportedRepositoryFormat, "thin packs are unsupported")
                    next = baseEntry.offset
                }
            }
            val body = inflateEntry(inflater, bytes, dataOffset, bytes.size, header.size, limits).body
            if (next != null) {
                chain.add(current to body)
                current = next
                continue
            }
            base = PackResolvedObject(type, body)
            cache.store(current, base)
            break
        }
        for ((linkOffset, delta) in chain.asReversed()) {
            base = PackResolvedObject(base.type, applyDelta(base.body, delta, limits))
            cache.store(linkOffset, base)
        }
        return base
    }

    private class Located(val file: File, val index: GitPackIndexV2, val entry: GitPackIndexEntry)

    companion object {
        const val maxDeltaChain = 4096
    }
}

private class MappedPack(val buffer: ByteBuffer, val trailer: Int, val offsets: LongArray) {
    fun entryBytes(start: Int): ByteArray {
        var low = 0
        var high = offsets.size
        while (low < high) {
            val middle = (low + high) ushr 1
            if (offsets[middle] <= start) low = middle + 1 else high = middle
        }
        val end = if (low < offsets.size && offsets[low] < trailer) offsets[low].toInt() else trailer
        return copyRange(buffer, start, end)
    }
}

private fun copyRange(buffer: ByteBuffer, start: Int, end: Int): ByteArray {
    val view = buffer.duplicate()
    view.position(start)
    val bytes = ByteArray(end - start)
    view.get(bytes)
    return bytes
}

internal class PackResolvedObject(val type: GitObjectType, val body: ByteArray)

internal class PackDeltaBaseCache {
    private val entries = HashMap<Long, PackResolvedObject>()
    private val order = ArrayDeque<Long>()
    private var bytes = 0L

    fun lookup(offset: Long): PackResolvedObject? = entries[offset]

    fun store(offset: Long, value: PackResolvedObject) {
        if (value.body.size > maxEntryBytes || entries.containsKey(offset)) return
        entries[offset] = value
        order.addLast(offset)
        bytes += value.body.size
        while (bytes > maxBytes && order.isNotEmpty()) {
            val evicted = entries.remove(order.removeFirst())
            if (evicted != null) bytes -= evicted.body.size
        }
    }

    companion object {
        const val maxBytes = 32 * 1024 * 1024
        const val maxEntryBytes = 8 * 1024 * 1024
    }
}
