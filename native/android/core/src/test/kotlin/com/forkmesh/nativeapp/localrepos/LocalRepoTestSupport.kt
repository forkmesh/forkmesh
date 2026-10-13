package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.model.auth.AuthSession
import com.forkmesh.nativeapp.security.DeviceIdentity
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.nio.file.FileVisitResult
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.SimpleFileVisitor
import java.nio.file.attribute.BasicFileAttributes
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.SecureRandom
import java.security.Signature
import java.security.spec.NamedParameterSpec
import java.security.spec.X509EncodedKeySpec
import java.util.Base64
import java.util.concurrent.TimeUnit
import java.util.zip.CRC32
import java.util.zip.Deflater

object LocalScratch {
    val root: File by lazy {
        val projectDir = File(System.getProperty("user.dir"))
        val base = if (File(projectDir, "build.gradle.kts").isFile) {
            File(File(projectDir, "build"), "localrepos-test")
        } else {
            File(System.getProperty("java.io.tmpdir"), "forkmesh-localrepos-test")
        }
        Files.createDirectories(base.toPath())
        base.canonicalFile
    }

    fun create(prefix: String): File = Files.createTempDirectory(root.toPath(), prefix).toFile().canonicalFile

    fun delete(directory: File) {
        val target = directory.absoluteFile.toPath().normalize()
        require(target.startsWith(root.toPath()) && target != root.toPath()) {
            "refusing to delete $directory: it is not a scratch directory below $root"
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

inline fun <T> withScratch(prefix: String, block: (File) -> T): T {
    val directory = LocalScratch.create(prefix)
    try {
        return block(directory)
    } finally {
        LocalScratch.delete(directory)
    }
}

class SeededIdentity(seed: ByteArray = ByteArray(32) { (it + 1).toByte() }) : DeviceIdentity {
    private val seedBytes = seed.copyOf()
    private val pair = KeyPairGenerator.getInstance("Ed25519").apply {
        initialize(NamedParameterSpec.ED25519, FixedSeed(seedBytes))
    }.generateKeyPair()
    private val publicRaw = pair.public.encoded.takeLast(32).toByteArray()

    override val publicKeyBytes: ByteArray get() = publicRaw.copyOf()
    override val publicKeyB64url: String get() = b64url(publicRaw)
    override val nodeId: String get() = publicKeyB64url
    override val shortKey: String get() = publicKeyB64url.take(8)

    override fun sign(data: ByteArray): String {
        val signer = Signature.getInstance("Ed25519")
        signer.initSign(pair.private)
        signer.update(data)
        return b64url(signer.sign())
    }

    override fun exportSeed(): ByteArray = seedBytes.copyOf()

    override fun rotate() = throw UnsupportedOperationException("a seeded test identity does not rotate")

    private class FixedSeed(private val seed: ByteArray) : SecureRandom() {
        override fun nextBytes(bytes: ByteArray) {
            seed.copyInto(bytes, 0, 0, minOf(bytes.size, seed.size))
        }
    }

    companion object {
        private val SPKI_PREFIX = byteArrayOf(0x30, 0x2a, 0x30, 0x05, 0x06, 0x03, 0x2b, 0x65, 0x70, 0x03, 0x21, 0x00)

        fun b64url(bytes: ByteArray): String = Base64.getUrlEncoder().withoutPadding().encodeToString(bytes)

        fun verify(data: ByteArray, signatureB64url: String, publicKeyB64url: String): Boolean {
            val raw = Base64.getUrlDecoder().decode(publicKeyB64url)
            val key = KeyFactory.getInstance("Ed25519").generatePublic(X509EncodedKeySpec(SPKI_PREFIX + raw))
            val verifier = Signature.getInstance("Ed25519")
            verifier.initVerify(key)
            verifier.update(data)
            return verifier.verify(Base64.getUrlDecoder().decode(signatureB64url))
        }
    }
}

fun mobileNodeSession(
    nodeName: String = "alice",
    capabilities: List<String> = listOf("browse", "comment", "submit_issue", "submit_pr", "publish_repo", "owner_sign"),
): AuthSession = AuthSession(
    nodeName = nodeName,
    email = "alice@example.com",
    status = "active",
    pubkey = "",
    emailVerified = true,
    isAdmin = false,
    solana = "",
    hasPayoutAddress = false,
    avatarPng = "",
    avatarUpdatedAt = 0,
    createdAt = 1,
    savedAt = 1,
    sessionKind = "mobile_node",
    deviceKind = "mobile_node",
    capabilities = capabilities,
    sessionToken = "session-token",
)

object HostGit {
    val available: Boolean by lazy {
        try {
            run(File(System.getProperty("user.dir")), "--version").first == 0
        } catch (error: IOException) {
            false
        }
    }

    fun run(directory: File, vararg args: String): Pair<Int, String> {
        val builder = ProcessBuilder(listOf("git") + args).directory(directory).redirectErrorStream(true)
        builder.environment()["GIT_CONFIG_NOSYSTEM"] = "1"
        builder.environment()["GIT_CONFIG_GLOBAL"] = File(directory, ".no-global-gitconfig").path
        val process = builder.start()
        val output = process.inputStream.readBytes().toString(Charsets.UTF_8)
        if (!process.waitFor(120, TimeUnit.SECONDS)) {
            process.destroy()
            throw IOException("git ${args.joinToString(" ")} timed out")
        }
        return process.exitValue() to output
    }
}

class TestZipEntry(
    val name: ByteArray,
    val data: ByteArray = ByteArray(0),
    val deflate: Boolean = false,
    val versionMadeBy: Int = 20,
    val externalAttributes: Long = 0,
    val utf8Flag: Boolean = true,
    val declaredSize: Long? = null,
) {
    constructor(name: String, data: String = "", deflate: Boolean = false, versionMadeBy: Int = 20, externalAttributes: Long = 0) :
        this(name.toByteArray(Charsets.UTF_8), data.toByteArray(Charsets.UTF_8), deflate, versionMadeBy, externalAttributes)
}

fun writeTestZip(file: File, entries: List<TestZipEntry>) {
    val out = ByteArrayOutputStream()
    val central = ByteArrayOutputStream()
    fun u16(stream: ByteArrayOutputStream, value: Int) {
        stream.write(value and 0xff)
        stream.write((value shr 8) and 0xff)
    }
    fun u32(stream: ByteArrayOutputStream, value: Long) {
        u16(stream, (value and 0xffff).toInt())
        u16(stream, ((value shr 16) and 0xffff).toInt())
    }
    for (entry in entries) {
        val crc = CRC32().apply { update(entry.data) }.value
        val payload = if (entry.deflate) {
            val deflater = Deflater(Deflater.DEFAULT_COMPRESSION, true)
            deflater.setInput(entry.data)
            deflater.finish()
            val buffer = ByteArrayOutputStream()
            val chunk = ByteArray(8192)
            while (!deflater.finished()) buffer.write(chunk, 0, deflater.deflate(chunk))
            deflater.end()
            buffer.toByteArray()
        } else {
            entry.data
        }
        val method = if (entry.deflate) 8 else 0
        val flags = if (entry.utf8Flag) 0x800 else 0
        val size = entry.declaredSize ?: entry.data.size.toLong()
        val offset = out.size().toLong()
        u32(out, 0x04034b50)
        u16(out, 20)
        u16(out, flags)
        u16(out, method)
        u16(out, 0)
        u16(out, 0x21)
        u32(out, crc)
        u32(out, payload.size.toLong())
        u32(out, size)
        u16(out, entry.name.size)
        u16(out, 0)
        out.write(entry.name)
        out.write(payload)

        u32(central, 0x02014b50)
        u16(central, entry.versionMadeBy)
        u16(central, 20)
        u16(central, flags)
        u16(central, method)
        u16(central, 0)
        u16(central, 0x21)
        u32(central, crc)
        u32(central, payload.size.toLong())
        u32(central, size)
        u16(central, entry.name.size)
        u16(central, 0)
        u16(central, 0)
        u16(central, 0)
        u16(central, 0)
        u32(central, entry.externalAttributes)
        u32(central, offset)
        central.write(entry.name)
    }
    val centralOffset = out.size().toLong()
    val centralBytes = central.toByteArray()
    out.write(centralBytes)
    u32(out, 0x06054b50)
    u16(out, 0)
    u16(out, 0)
    u16(out, entries.size)
    u16(out, entries.size)
    u32(out, centralBytes.size.toLong())
    u32(out, centralOffset)
    u16(out, 0)
    file.writeBytes(out.toByteArray())
}
