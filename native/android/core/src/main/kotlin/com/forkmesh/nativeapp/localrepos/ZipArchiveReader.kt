package com.forkmesh.nativeapp.localrepos

import java.io.ByteArrayOutputStream
import java.io.Closeable
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction
import java.util.zip.DataFormatException
import java.util.zip.Inflater

class ZipFormatException(message: String) : IOException(message)

class ZipArchiveFile internal constructor(
    val name: String,
    val isFile: Boolean,
    val size: Long,
    internal val versionMadeBy: Int,
    internal val externalAttributes: Long,
    internal val flags: Int,
    internal val method: Int,
    internal val compressedSize: Long,
    internal val localHeaderOffset: Long,
) {
    var isSymbolicLink: Boolean = false
        internal set
}

class ZipArchive private constructor(private val file: RandomAccessFile, val files: List<ZipArchiveFile>) : Closeable {
    override fun close() = file.close()

    fun content(entry: ZipArchiveFile, limit: Long): ByteArray {
        if (entry.flags and 1 != 0) throw ZipFormatException("encrypted zip members are not supported")
        val header = ByteArray(30)
        file.seek(entry.localHeaderOffset)
        file.readFully(header)
        if (u32(header, 0) != LOCAL_SIGNATURE) throw ZipFormatException("invalid local file header")
        val dataStart = entry.localHeaderOffset + 30 + u16(header, 26) + u16(header, 28)
        if (dataStart + entry.compressedSize > file.length()) throw ZipFormatException("zip member is truncated")
        file.seek(dataStart)
        return when (entry.method) {
            0 -> {
                val take = minOf(entry.compressedSize, limit + 1).toInt()
                ByteArray(take).also { file.readFully(it) }
            }
            8 -> inflate(entry.compressedSize, limit)
            else -> throw ZipFormatException("unsupported compression method ${entry.method}")
        }
    }

    private fun inflate(compressedSize: Long, limit: Long): ByteArray {
        val inflater = Inflater(true)
        try {
            val out = ByteArrayOutputStream()
            val input = ByteArray(64 * 1024)
            val output = ByteArray(64 * 1024)
            var remaining = compressedSize
            var dummyFed = false
            while (!inflater.finished() && out.size() <= limit) {
                if (inflater.needsInput()) {
                    if (remaining > 0) {
                        val read = file.read(input, 0, minOf(input.size.toLong(), remaining).toInt())
                        if (read < 0) throw ZipFormatException("zip member is truncated")
                        remaining -= read
                        inflater.setInput(input, 0, read)
                    } else if (!dummyFed) {
                        dummyFed = true
                        inflater.setInput(ByteArray(1))
                    } else {
                        throw ZipFormatException("zip member is truncated")
                    }
                }
                val produced = try {
                    inflater.inflate(output)
                } catch (error: DataFormatException) {
                    throw ZipFormatException("Invalid Deflate data: ${error.message}")
                }
                if (produced == 0 && inflater.needsDictionary()) throw ZipFormatException("Invalid Deflate data")
                out.write(output, 0, minOf(produced.toLong(), limit + 1 - out.size()).toInt())
            }
            return out.toByteArray()
        } finally {
            inflater.end()
        }
    }

    companion object {
        private const val LOCAL_SIGNATURE = 0x04034b50L
        private const val CENTRAL_SIGNATURE = 0x02014b50L
        private const val END_SIGNATURE = 0x06054b50L
        private const val ZIP64_LOCATOR_SIGNATURE = 0x07064b50L
        private const val ZIP64_END_SIGNATURE = 0x06064b50L

        fun open(zip: File): ZipArchive {
            val raf = RandomAccessFile(zip, "r")
            try {
                return ZipArchive(raf, readDirectory(raf))
            } catch (error: Throwable) {
                raf.close()
                throw error
            }
        }

        private fun readDirectory(raf: RandomAccessFile): List<ZipArchiveFile> {
            val length = raf.length()
            if (length < 22) throw ZipFormatException("Could not find End of Central Directory Record")
            val tailLength = minOf(length, 22L + 0xffff).toInt()
            val tail = ByteArray(tailLength)
            raf.seek(length - tailLength)
            raf.readFully(tail)
            var eocd = -1
            for (index in tailLength - 22 downTo 0) {
                if (u32(tail, index) == END_SIGNATURE) {
                    eocd = index
                    break
                }
            }
            if (eocd < 0) throw ZipFormatException("Could not find End of Central Directory Record")
            val eocdPosition = length - tailLength + eocd
            var directorySize = u32(tail, eocd + 12)
            var directoryOffset = u32(tail, eocd + 16)

            if (eocdPosition >= 20) {
                val locator = ByteArray(20)
                raf.seek(eocdPosition - 20)
                raf.readFully(locator)
                if (u32(locator, 0) == ZIP64_LOCATOR_SIGNATURE) {
                    val recordOffset = u64(locator, 8)
                    if (recordOffset < 0 || recordOffset + 56 > length) throw ZipFormatException("invalid zip64 locator")
                    val record = ByteArray(56)
                    raf.seek(recordOffset)
                    raf.readFully(record)
                    if (u32(record, 0) != ZIP64_END_SIGNATURE) throw ZipFormatException("invalid zip64 end record")
                    directorySize = u64(record, 40)
                    directoryOffset = u64(record, 48)
                }
            }
            if (directoryOffset < 0 || directorySize < 0 || directoryOffset + directorySize > length) {
                throw ZipFormatException("central directory is out of range")
            }
            if (directorySize > Int.MAX_VALUE) throw ZipFormatException("central directory is too large")
            val directory = ByteArray(directorySize.toInt())
            raf.seek(directoryOffset)
            raf.readFully(directory)

            val byName = LinkedHashMap<String, ZipArchiveFile>()
            var offset = 0
            while (offset + 46 <= directory.size && u32(directory, offset) == CENTRAL_SIGNATURE) {
                val versionMadeBy = u16(directory, offset + 4)
                val flags = u16(directory, offset + 8)
                val method = u16(directory, offset + 10)
                var compressed = u32(directory, offset + 20)
                var uncompressed = u32(directory, offset + 24)
                val nameLength = u16(directory, offset + 28)
                val extraLength = u16(directory, offset + 30)
                val commentLength = u16(directory, offset + 32)
                val external = u32(directory, offset + 38)
                var localOffset = u32(directory, offset + 42)
                val nameStart = offset + 46
                val extraStart = nameStart + nameLength
                val end = extraStart + extraLength + commentLength
                if (end > directory.size) throw ZipFormatException("central directory entry is truncated")
                val name = decodeName(directory, nameStart, nameLength)

                var extra = extraStart
                while (extra + 4 <= extraStart + extraLength) {
                    val id = u16(directory, extra)
                    val size = u16(directory, extra + 2)
                    val dataEnd = extra + 4 + size
                    if (dataEnd > extraStart + extraLength) break
                    if (id == 0x0001) {
                        var cursor = extra + 4
                        if (uncompressed == 0xffffffffL && cursor + 8 <= dataEnd) {
                            uncompressed = u64(directory, cursor)
                            cursor += 8
                        }
                        if (compressed == 0xffffffffL && cursor + 8 <= dataEnd) {
                            compressed = u64(directory, cursor)
                            cursor += 8
                        }
                        if (localOffset == 0xffffffffL && cursor + 8 <= dataEnd) {
                            localOffset = u64(directory, cursor)
                        }
                    }
                    extra = dataEnd
                }
                if (localOffset < 0 || localOffset >= length || compressed < 0 || uncompressed < 0) {
                    throw ZipFormatException("zip member is out of range")
                }

                val isDirectory = name.endsWith("/") || name.endsWith("\\")
                val candidate = ZipArchiveFile(
                    name = name,
                    isFile = !isDirectory,
                    size = uncompressed,
                    versionMadeBy = versionMadeBy,
                    externalAttributes = external,
                    flags = flags,
                    method = method,
                    compressedSize = compressed,
                    localHeaderOffset = localOffset,
                )
                val entry = byName.getOrPut(name) { candidate }
                if (versionMadeBy shr 8 == 3 && ((external shr 16) and 0xf000L) == 0xa000L) {
                    val archive = ZipArchive(raf, emptyList())
                    val target = archive.content(candidate, MAX_LINK_BYTES)
                    if (target.size > MAX_LINK_BYTES) throw ZipFormatException("symbolic link target is too long")
                    val text = strictUtf8(target, 0, target.size)
                        ?: throw ZipFormatException("FormatException: Invalid UTF-8 byte")
                    if (text.isNotEmpty()) entry.isSymbolicLink = true
                }
                offset = end
            }
            return byName.values.toList()
        }

        private const val MAX_LINK_BYTES = 64L * 1024

        private fun decodeName(bytes: ByteArray, start: Int, length: Int): String =
            strictUtf8(bytes, start, length) ?: String(bytes, start, length, Charsets.ISO_8859_1)

        private fun strictUtf8(bytes: ByteArray, start: Int, length: Int): String? = try {
            Charsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes, start, length))
                .toString()
        } catch (error: java.nio.charset.CharacterCodingException) {
            null
        }

        private fun u16(bytes: ByteArray, offset: Int): Int =
            (bytes[offset].toInt() and 0xff) or ((bytes[offset + 1].toInt() and 0xff) shl 8)

        private fun u32(bytes: ByteArray, offset: Int): Long =
            u16(bytes, offset).toLong() or (u16(bytes, offset + 2).toLong() shl 16)

        private fun u64(bytes: ByteArray, offset: Int): Long = u32(bytes, offset) or (u32(bytes, offset + 4) shl 32)
    }
}
