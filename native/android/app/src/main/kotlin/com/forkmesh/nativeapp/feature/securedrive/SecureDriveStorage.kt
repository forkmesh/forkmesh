package com.forkmesh.nativeapp.feature.securedrive

import android.content.ContentResolver
import android.content.Intent
import android.net.Uri
import android.os.ParcelFileDescriptor
import android.provider.DocumentsContract
import android.provider.OpenableColumns
import android.system.ErrnoException
import android.system.Os
import android.system.OsConstants
import com.forkmesh.nativeapp.securedrive.BlobSource
import com.forkmesh.nativeapp.securedrive.ContainerIo
import com.forkmesh.nativeapp.securedrive.ContainerOpener
import com.forkmesh.nativeapp.securedrive.SecureDriveException
import java.io.FileDescriptor
import java.io.InputStream

internal class DescriptorContainer(private val pfd: ParcelFileDescriptor) : ContainerIo {
    private val fd: FileDescriptor = pfd.fileDescriptor

    override fun size(): Long = try {
        Os.fstat(fd).st_size
    } catch (_: ErrnoException) {
        0L
    }

    override fun readAt(offset: Long, length: Int): ByteArray? {
        if (offset < 0 || length < 0) return null
        val out = ByteArray(length)
        var done = 0
        return try {
            while (done < length) {
                val n = Os.pread(fd, out, done, length - done, offset + done)
                if (n <= 0) return null
                done += n
            }
            out
        } catch (_: Exception) {
            null
        }
    }

    override fun writeAt(offset: Long, bytes: ByteArray): Boolean {
        if (offset < 0) return false
        var done = 0
        return try {
            while (done < bytes.size) {
                val n = Os.pwrite(fd, bytes, done, bytes.size - done, offset + done)
                if (n <= 0) return false
                done += n
            }
            true
        } catch (_: Exception) {
            false
        }
    }

    override fun sync(): Boolean = try {
        Os.fsync(fd)
        true
    } catch (error: ErrnoException) {
        error.errno == OsConstants.EINVAL || error.errno == OsConstants.EROFS
    }

    override fun close() {
        try {
            pfd.close()
        } catch (_: Exception) {
        }
    }
}

internal class SafContainerOpener(private val resolver: ContentResolver) : ContainerOpener {
    override fun open(container: String, write: Boolean): ContainerIo? {
        val pfd = try {
            resolver.openFileDescriptor(Uri.parse(container), if (write) "rw" else "r")
        } catch (_: Exception) {
            null
        } ?: return null
        if (!seekable(pfd)) {
            pfd.close()
            return null
        }
        return DescriptorContainer(pfd)
    }

    fun check(container: String) {
        val pfd = try {
            resolver.openFileDescriptor(Uri.parse(container), "rw")
        } catch (_: Exception) {
            null
        } ?: throw SecureDriveException("container_unavailable")
        pfd.use {
            if (!seekable(it)) throw SecureDriveException("not_seekable")
        }
    }

    fun discard(container: String) {
        try {
            DocumentsContract.deleteDocument(resolver, Uri.parse(container))
        } catch (_: Exception) {
        }
    }

    fun keepAccess(uri: Uri) {
        try {
            resolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        } catch (_: Exception) {
        }
    }

    fun displayName(uri: Uri): String = try {
        resolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0).orEmpty() else ""
        }.orEmpty()
    } catch (_: Exception) {
        ""
    }

    fun source(uri: Uri): BlobSource {
        val size = try {
            resolver.openFileDescriptor(uri, "r")?.use { it.statSize } ?: -1L
        } catch (_: Exception) {
            -1L
        }
        val length = if (size >= 0) size else querySize(uri)
        return object : BlobSource {
            override val length: Long = length
            override fun open(): InputStream = resolver.openInputStream(uri) ?: throw SecureDriveException("read_failed")
        }
    }

    private fun querySize(uri: Uri): Long = try {
        resolver.query(uri, arrayOf(OpenableColumns.SIZE), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst() && !cursor.isNull(0)) cursor.getLong(0) else -1L
        } ?: -1L
    } catch (_: Exception) {
        -1L
    }

    private fun seekable(pfd: ParcelFileDescriptor): Boolean = try {
        Os.lseek(pfd.fileDescriptor, 0, OsConstants.SEEK_CUR)
        val mode = Os.fstat(pfd.fileDescriptor).st_mode
        OsConstants.S_ISREG(mode)
    } catch (_: ErrnoException) {
        false
    }
}
