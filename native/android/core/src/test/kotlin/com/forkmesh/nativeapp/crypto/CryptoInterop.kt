package com.forkmesh.nativeapp.crypto

import java.io.File
import java.io.IOException
import java.util.concurrent.TimeUnit

object CryptoInterop {
    val scratchDir: File by lazy {
        var dir: File? = File(CryptoInterop::class.java.protectionDomain.codeSource.location.toURI()).absoluteFile
        while (dir != null && dir.name != "build") dir = dir.parentFile
        val build = dir ?: error("test classes are not under a build directory")
        check(build.parentFile?.name == "core") { "unexpected build directory $build" }
        File(build, "crypto-interop").also { it.mkdirs() }
    }

    val repoRoot: File? by lazy {
        var dir: File? = scratchDir
        while (dir != null && !File(dir, "app/public/portable-identity.js").isFile) dir = dir.parentFile
        dir
    }

    class Result(val exit: Int, val stdout: ByteArray, val stderr: String) {
        val text: String get() = String(stdout, Charsets.UTF_8)
    }

    fun run(vararg command: String, timeoutSeconds: Long = 120): Result? = try {
        val process = ProcessBuilder(*command).directory(scratchDir).start()
        var stderr = ""
        val errors = Thread { stderr = String(process.errorStream.readBytes(), Charsets.UTF_8) }.also { it.start() }
        val stdout = process.inputStream.readBytes()
        if (!process.waitFor(timeoutSeconds, TimeUnit.SECONDS)) {
            process.destroyForcibly()
            null
        } else {
            errors.join()
            Result(process.exitValue(), stdout, stderr)
        }
    } catch (_: IOException) {
        null
    }

    fun <T> withFiles(vararg names: String, block: (List<File>) -> T): T {
        val files = names.map { File(scratchDir, it) }
        try {
            return block(files)
        } finally {
            files.forEach { it.delete() }
        }
    }
}
