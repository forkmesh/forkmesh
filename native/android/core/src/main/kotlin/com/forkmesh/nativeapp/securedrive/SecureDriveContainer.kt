package com.forkmesh.nativeapp.securedrive

import java.io.Closeable
import java.io.File
import java.io.RandomAccessFile
import java.nio.ByteBuffer
import java.nio.channels.FileChannel

interface ContainerIo : Closeable {
    fun size(): Long
    fun readAt(offset: Long, length: Int): ByteArray?
    fun writeAt(offset: Long, bytes: ByteArray): Boolean
    fun sync(): Boolean

    fun pages(): Long = size() / SecureDriveLayout.PAGE_BYTES

    fun appendRandom(pages: Long): Boolean {
        var offset = size()
        var left = pages * SecureDriveLayout.PAGE_BYTES
        while (left > 0) {
            val block = SdCrypto.random(minOf(left, 1L shl 20).toInt())
            if (!writeAt(offset, block)) return false
            offset += block.size
            left -= block.size
        }
        return sync()
    }
}

class ChannelContainer(private val channel: FileChannel, private val onClose: () -> Unit = {}) : ContainerIo {
    override fun size(): Long = try {
        channel.size()
    } catch (_: Exception) {
        0L
    }

    override fun readAt(offset: Long, length: Int): ByteArray? {
        if (offset < 0 || length < 0) return null
        val buffer = ByteBuffer.allocate(length)
        return try {
            var position = offset
            while (buffer.hasRemaining()) {
                val read = channel.read(buffer, position)
                if (read <= 0) return null
                position += read
            }
            buffer.array()
        } catch (_: Exception) {
            null
        }
    }

    override fun writeAt(offset: Long, bytes: ByteArray): Boolean {
        if (offset < 0) return false
        val buffer = ByteBuffer.wrap(bytes)
        return try {
            var position = offset
            while (buffer.hasRemaining()) {
                val written = channel.write(buffer, position)
                if (written < 0) return false
                position += written
            }
            true
        } catch (_: Exception) {
            false
        }
    }

    override fun sync(): Boolean = try {
        channel.force(true)
        true
    } catch (_: Exception) {
        false
    }

    override fun close() {
        try {
            channel.close()
        } catch (_: Exception) {
        }
        onClose()
    }
}

fun interface ContainerOpener {
    fun open(container: String, write: Boolean): ContainerIo?
}

object FileContainerOpener : ContainerOpener {
    override fun open(container: String, write: Boolean): ContainerIo? {
        val file = File(container)
        if (!file.isFile) return null
        return try {
            val access = RandomAccessFile(file, if (write) "rw" else "r")
            ChannelContainer(access.channel) { runCatching { access.close() } }
        } catch (_: Exception) {
            null
        }
    }
}

object SecureDriveLayout {
    const val PAGE_BYTES = 16384L
    const val UNIT_OVERHEAD = 28L
    const val CHUNK_BYTES = PAGE_BYTES - UNIT_OVERHEAD
    const val LEASE_PAGE = 0L
    const val ROOT_PAGES = 4L
    const val ROOT_A_PAGE = 1L
    const val ROOT_B_PAGE = 5L
    const val MAILBOX_PAGE = 9L
    const val MAILBOX_SLOTS = 64
    const val SLOT_BYTES = 8192L
    const val DATA_PAGE = 41L
    const val INITIAL_DATA_PAGES = 32L

    fun mailboxOffset(mailbox: Int): Long = MAILBOX_PAGE * PAGE_BYTES + mailbox.toLong() * SLOT_BYTES
}
