package com.forkmesh.nativeapp.tools

import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.concurrent.ExecutorService
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException

class StallLogFile(
    private val directory: () -> File,
    val maxBytes: Long = 1024L * 1024L,
    private val build: AppBuild = AppBuild.UNKNOWN,
    private val now: () -> Long = { System.currentTimeMillis() },
) {
    companion object {
        const val FILE_NAME = "stalls.log"
    }

    private val writer: ExecutorService by lazy {
        Executors.newSingleThreadExecutor { runnable -> Thread(runnable, "forkmesh-stall-log").apply { isDaemon = true } }
    }

    @Volatile
    private var file: File? = null

    private var unavailable = false
    private var bytes = 0L

    val path: String? get() = file?.path

    fun append(record: String) {
        try {
            writer.execute {
                try {
                    write(record)
                } catch (_: Exception) {
                }
            }
        } catch (_: RejectedExecutionException) {
        }
    }

    fun flush() {
        try {
            writer.submit {}.get()
        } catch (_: Exception) {
        }
    }

    private fun write(record: String) {
        val file = open() ?: return
        val data = (if (record.endsWith("\n")) record else "$record\n").toByteArray(Charsets.UTF_8)
        if (bytes + data.size > maxBytes && bytes > 0) {
            try {
                Files.move(file.toPath(), File(file.path + ".1").toPath(), StandardCopyOption.REPLACE_EXISTING)
            } catch (_: IOException) {
            }
            bytes = 0
            writeBytes(file, sessionHeader("rotated").toByteArray(Charsets.UTF_8))
        }
        writeBytes(file, data)
    }

    private fun writeBytes(file: File, data: ByteArray) {
        FileOutputStream(file, true).use { out ->
            out.write(data)
            out.flush()
            try {
                out.fd.sync()
            } catch (_: IOException) {
            }
        }
        bytes += data.size
    }

    private fun open(): File? {
        file?.let { return it }
        if (unavailable) return null
        return try {
            val root = directory()
            val dir = File(root, "diagnostics")
            dir.mkdirs()
            if (!dir.isDirectory) throw IOException("cannot create ${dir.path}")
            val opened = File(dir, FILE_NAME)
            bytes = if (opened.exists()) opened.length() else 0
            file = opened
            writeBytes(opened, sessionHeader("session start").toByteArray(Charsets.UTF_8))
            opened
        } catch (_: Exception) {
            unavailable = true
            null
        }
    }

    private fun sessionHeader(label: String): String =
        "--- $label ${dartIsoLocal(now())} build=${build.version} mode=${build.mode} " +
            "platform=${build.platform} os=\"${build.osVersion}\" ---\n"
}
