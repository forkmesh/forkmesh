package com.forkmesh.nativeapp.git.refs

import com.forkmesh.nativeapp.git.bytes.asciiDecode
import com.forkmesh.nativeapp.git.bytes.dartSplitLines
import com.forkmesh.nativeapp.git.bytes.dartTrim
import com.forkmesh.nativeapp.git.bytes.utf8Encode
import com.forkmesh.nativeapp.git.errors.GitErrorCode
import com.forkmesh.nativeapp.git.errors.GitException
import com.forkmesh.nativeapp.git.objects.GitObjectId
import com.forkmesh.nativeapp.git.security.GitResourceLimits
import com.forkmesh.nativeapp.git.worktree.FileSystemEntityType
import com.forkmesh.nativeapp.git.worktree.createDirectories
import com.forkmesh.nativeapp.git.worktree.createFileExclusive
import com.forkmesh.nativeapp.git.worktree.directoryExists
import com.forkmesh.nativeapp.git.worktree.fileExists
import com.forkmesh.nativeapp.git.worktree.readString
import com.forkmesh.nativeapp.git.worktree.renameFile
import com.forkmesh.nativeapp.git.worktree.walkEntries
import com.forkmesh.nativeapp.git.worktree.writeStringFlushed
import java.io.File

class GitRefName private constructor(val value: String) {
    private fun validate() {
        if (utf8Encode(value).size > GitResourceLimits.hardMaxRefNameBytes ||
            value.endsWith("/") ||
            value.endsWith(".") ||
            value.endsWith(".lock") ||
            value.contains("..") ||
            value.contains("//") ||
            forbiddenCharacters.containsMatchIn(value)
        ) {
            throw GitException(GitErrorCode.invalidRef, "invalid ref spelling")
        }
        for (component in value.split("/")) {
            if (component.isEmpty() || component == "." || component == "..") {
                throw GitException(GitErrorCode.invalidRef, "invalid ref component")
            }
        }
    }

    override fun equals(other: Any?): Boolean = other is GitRefName && other.value == value

    override fun hashCode(): Int = value.hashCode()

    override fun toString(): String = value

    companion object {
        private val forbiddenCharacters = Regex("""[\x00-\x20~^:?*\\\[]""")

        fun branch(value: String): GitRefName {
            val ref = GitRefName(value)
            if (!value.startsWith("refs/heads/")) {
                throw GitException(GitErrorCode.invalidRef, "expected branch ref")
            }
            ref.validate()
            return ref
        }

        fun parse(value: String): GitRefName {
            val ref = GitRefName(value)
            if (!value.startsWith("refs/heads/") &&
                !value.startsWith("refs/tags/") &&
                !value.startsWith("refs/remotes/")
            ) {
                throw GitException(GitErrorCode.invalidRef, "unsupported ref namespace")
            }
            ref.validate()
            return ref
        }
    }
}

data class GitSymbolicRef(val target: String)

@Suppress("EnumEntryName")
enum class GitRefUpdateResult { updated, nonFastForward }

class RefStore(val gitDirectory: File) {
    private fun fileFor(ref: GitRefName): File = File(gitDirectory, ref.value)

    fun readDirect(ref: GitRefName): GitObjectId? = readOptional(fileFor(ref))

    fun listDirect(prefix: String, limits: GitResourceLimits): Map<String, GitObjectId> {
        if (!prefix.startsWith("refs/") || !prefix.endsWith("/")) {
            throw GitException(GitErrorCode.invalidRef, "ref prefix is invalid")
        }
        val result = LinkedHashMap<String, GitObjectId>()
        val packed = File(gitDirectory, "packed-refs")
        if (fileExists(packed)) {
            val bytes = packed.readBytes()
            if (bytes.size > 16 * 1024 * 1024) {
                throw GitException(GitErrorCode.resourceLimitExceeded, "packed-refs exceeds the configured limit")
            }
            val refs = PackedRefs.parse(bytes, limits)
            for ((name, ref) in refs.entries) {
                if (name.startsWith(prefix)) result[name] = ref.objectId
            }
        }
        val root = File(gitDirectory, prefix.substring(0, prefix.length - 1).replace('/', File.separatorChar))
        if (directoryExists(root)) {
            val rootPath = root.toPath()
            walkEntries(root) { entity, type ->
                if (type != FileSystemEntityType.file) return@walkEntries
                val relative = rootPath.relativize(entity.toPath()).toString().replace(File.separatorChar, '/')
                if (relative.endsWith(".lock")) return@walkEntries
                val name = "$prefix$relative"
                val ref = try {
                    GitRefName.parse(name)
                } catch (error: GitException) {
                    return@walkEntries
                }
                if (result.size >= limits.maxRefCount) {
                    throw GitException(GitErrorCode.resourceLimitExceeded, "local refs exceed the configured limit")
                }
                val text = readString(entity).dartTrim()
                if (text.startsWith("ref: ")) return@walkEntries
                result[ref.value] = GitObjectId.parseSha1(text)
            }
        }
        return result
    }

    fun update(ref: GitRefName, expectedOld: GitObjectId?, next: GitObjectId): GitRefUpdateResult {
        val destination = fileFor(ref)
        val lock = File("${destination.path}.lock")
        createDirectories(lock.parentFile)
        if (!createFileExclusive(lock)) {
            throw GitException(GitErrorCode.refLocked, "ref lock already exists")
        }
        try {
            val current = readOptional(destination)
            if (current != expectedOld) return GitRefUpdateResult.nonFastForward
            writeStringFlushed(lock, "${next.hex}\n")
            renameFile(lock, destination)
            return GitRefUpdateResult.updated
        } finally {
            if (fileExists(lock)) lock.delete()
        }
    }

    private fun readOptional(file: File): GitObjectId? {
        if (!fileExists(file)) return null
        val value = readString(file).dartTrim()
        if (value.startsWith("ref: ")) {
            throw GitException(GitErrorCode.invalidRef, "expected direct ref")
        }
        return GitObjectId.parseSha1(value)
    }
}

class PackedRef(val objectId: GitObjectId, val peeled: GitObjectId?)

class PackedRefs private constructor(internal val entries: Map<String, PackedRef>) {
    fun lookup(name: String): PackedRef? = entries[name]

    companion object {
        fun parse(bytes: ByteArray, limits: GitResourceLimits): PackedRefs {
            val text = asciiDecode(bytes)
            val entries = LinkedHashMap<String, PackedRef>()
            var previousName: String? = null
            for (line in dartSplitLines(text)) {
                if (line.isEmpty() || line.startsWith("#")) continue
                if (line.startsWith("^")) {
                    if (previousName == null || entries.getValue(previousName).peeled != null) {
                        throw GitException(GitErrorCode.invalidRef, "invalid peeled ref")
                    }
                    val current = entries.getValue(previousName)
                    entries[previousName] = PackedRef(current.objectId, GitObjectId.parseSha1(line.substring(1)))
                    continue
                }
                val separator = line.indexOf(' ')
                if (separator != 40 || separator == line.length - 1 || entries.size >= limits.maxRefCount) {
                    throw GitException(GitErrorCode.invalidRef, "packed ref record is invalid")
                }
                val name = line.substring(separator + 1)
                GitRefName.parse(name)
                if (previousName != null && previousName >= name) {
                    throw GitException(GitErrorCode.invalidRef, "packed refs are not sorted")
                }
                entries[name] = PackedRef(GitObjectId.parseSha1(line.substring(0, separator)), null)
                previousName = name
            }
            return PackedRefs(entries)
        }
    }
}
