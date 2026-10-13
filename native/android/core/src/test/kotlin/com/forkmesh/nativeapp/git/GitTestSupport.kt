package com.forkmesh.nativeapp.git

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.assertFails
import java.io.File
import java.io.IOException
import java.nio.file.FileVisitResult
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.SimpleFileVisitor
import java.nio.file.attribute.BasicFileAttributes
import java.util.zip.Deflater

object GitTestDirs {
    val root: File by lazy {
        val projectDir = File(System.getProperty("user.dir"))
        val base = if (File(projectDir, "build.gradle.kts").isFile) {
            File(File(projectDir, "build"), "git-test")
        } else {
            File(System.getProperty("java.io.tmpdir"), "forkmesh-git-test")
        }
        Files.createDirectories(base.toPath())
        base.canonicalFile
    }

    fun create(prefix: String): File = Files.createTempDirectory(root.toPath(), prefix).toFile().canonicalFile

    fun delete(directory: File) {
        val target = directory.absoluteFile.toPath().normalize()
        require(target.startsWith(root.toPath()) && target != root.toPath()) {
            "refusing to delete $directory: it is not a test directory below $root"
        }
        if (!Files.exists(target, LinkOption.NOFOLLOW_LINKS)) return
        Files.walkFileTree(
            target,
            object : SimpleFileVisitor<Path>() {
                override fun visitFile(file: Path, attrs: BasicFileAttributes): FileVisitResult {
                    Files.deleteIfExists(file)
                    return FileVisitResult.CONTINUE
                }

                override fun postVisitDirectory(dir: Path, exc: IOException?): FileVisitResult {
                    if (exc != null) throw exc
                    Files.deleteIfExists(dir)
                    return FileVisitResult.CONTINUE
                }
            },
        )
    }
}

inline fun <T> withTempDir(prefix: String, block: (File) -> T): T {
    val directory = GitTestDirs.create(prefix)
    try {
        return block(directory)
    } finally {
        GitTestDirs.delete(directory)
    }
}

inline fun assertGitError(code: GitErrorCode, block: () -> Unit): GitException {
    val error = assertFails<GitException>(block)
    assertEq(code, error.code, "GitException code (${error.message})")
    return error
}

fun fixtureId(byte: Int): GitObjectId = GitObjectId.parseSha1(byte.toString(16).padStart(2, '0').repeat(20))

fun utf8(text: String): ByteArray = text.toByteArray(Charsets.UTF_8)

fun utf8Text(bytes: ByteArray): String = String(bytes, Charsets.UTF_8)

fun latin1Text(bytes: ByteArray): String = String(bytes, Charsets.ISO_8859_1)

fun uint32(value: Long): ByteArray =
    byteArrayOf((value shr 24).toByte(), (value shr 16).toByte(), (value shr 8).toByte(), value.toByte())

fun zlib(bytes: ByteArray): ByteArray {
    val deflater = Deflater()
    try {
        deflater.setInput(bytes)
        deflater.finish()
        val output = java.io.ByteArrayOutputStream()
        val buffer = ByteArray(8192)
        while (!deflater.finished()) {
            output.write(buffer, 0, deflater.deflate(buffer))
        }
        return output.toByteArray()
    } finally {
        deflater.end()
    }
}

fun sha1(bytes: ByteArray): ByteArray {
    val digest = ForkMeshSha1()
    digest.add(bytes)
    return digest.close()
}

fun withSha1Trailer(bytes: ByteArray): ByteArray = bytes + sha1(bytes)

fun File.child(path: String): File = File(this, path)

class ClearDetector : GitSha1CollisionDetector {
    override fun start(): GitSha1CollisionDetectorSession = object : GitSha1CollisionDetectorSession {
        override fun add(bytes: ByteArray) {}

        override fun close(): GitSha1CollisionDetectorStatus = GitSha1CollisionDetectorStatus.clear

        override fun dispose() {}
    }
}
