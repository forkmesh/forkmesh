package com.forkmesh.nativeapp.localrepos

import com.forkmesh.nativeapp.git.bytes.dartTrim
import com.forkmesh.nativeapp.git.crypto.GitSha1CollisionDetector
import com.forkmesh.nativeapp.git.crypto.GitSha1DcDetector
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.model.Repository
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.nio.file.AtomicMoveNotSupportedException
import java.nio.file.FileVisitResult
import java.nio.file.Files
import java.nio.file.LinkOption
import java.nio.file.Path
import java.nio.file.SimpleFileVisitor
import java.nio.file.StandardCopyOption
import java.nio.file.attribute.BasicFileAttributes
import java.time.LocalDateTime
import java.time.ZoneId
import java.time.ZoneOffset
import java.util.concurrent.CopyOnWriteArrayList

@Suppress("EnumEntryName")
enum class LocalRepoOrigin {
    authored,

    cloned,
}

data class LocalRepoRecord(
    val name: String,
    val path: String,
    val createdAt: Long,
    val description: String = "",
    val branch: String = "main",
    val lastCommitSubject: String = "",
    val lastCommitAt: Long? = null,
    val publishedAs: String? = null,
    val origin: LocalRepoOrigin = LocalRepoOrigin.authored,
) {
    val isPublished: Boolean get() = !publishedAs.isNullOrEmpty()
    val isCloned: Boolean get() = origin == LocalRepoOrigin.cloned

    val meshRoute: Pair<String, String>?
        get() {
            val route = (publishedAs ?: "").dartTrim()
            val slash = route.indexOf('/')
            if (slash <= 0 || slash == route.length - 1) return null
            return route.substring(0, slash) to route.substring(slash + 1)
        }

    val syncLabel: String
        get() = when {
            !isPublished -> "Local only"
            isCloned -> "Cloned"
            else -> "Published"
        }

    fun copyWith(
        description: String? = null,
        branch: String? = null,
        lastCommitSubject: String? = null,
        lastCommitAt: Long? = null,
        clearLastCommit: Boolean = false,
        publishedAs: String? = null,
        clearPublishedAs: Boolean = false,
        origin: LocalRepoOrigin? = null,
    ): LocalRepoRecord = LocalRepoRecord(
        name = name,
        path = path,
        createdAt = createdAt,
        description = description ?: this.description,
        branch = branch ?: this.branch,
        lastCommitSubject = lastCommitSubject ?: this.lastCommitSubject,
        lastCommitAt = if (clearLastCommit) null else (lastCommitAt ?: this.lastCommitAt),
        publishedAs = if (clearPublishedAs) null else (publishedAs ?: this.publishedAs),
        origin = origin ?: this.origin,
    )

    fun toJson(): LinkedHashMap<String, Any?> = linkedMapOf(
        "name" to name,
        "path" to path,
        "description" to description,
        "createdAt" to DartDateTime.toIso8601Utc(createdAt),
        "branch" to branch,
        "lastCommit" to lastCommitSubject,
        "lastCommitAt" to lastCommitAt?.let(DartDateTime::toIso8601Utc),
        "publishedAs" to publishedAs,
        "origin" to origin.name,
    )

    companion object {
        fun fromJson(json: Map<String, Any?>): LocalRepoRecord? {
            val name = json["name"] as? String ?: return null
            val path = json["path"] as? String ?: return null
            if (!LocalRepoStore.isValidName(name)) return null
            return LocalRepoRecord(
                name = name,
                path = path,
                description = interpolate(json["description"] ?: ""),
                createdAt = DartDateTime.tryParse(interpolate(json["createdAt"] ?: "")) ?: 0L,
                branch = interpolate(json["branch"] ?: "main"),
                lastCommitSubject = interpolate(json["lastCommit"] ?: ""),
                lastCommitAt = DartDateTime.tryParse(interpolate(json["lastCommitAt"] ?: "")),
                publishedAs = json["publishedAs"] as? String,
                origin = if (json["origin"] == LocalRepoOrigin.cloned.name) LocalRepoOrigin.cloned else LocalRepoOrigin.authored,
            )
        }

        private fun interpolate(value: Any): String = when (value) {
            is Double -> Json.formatDouble(value)
            else -> value.toString()
        }
    }
}

class LocalRepoStoreException(override val message: String) : RuntimeException(message) {
    override fun toString(): String = message
}

open class LocalRepoStore(
    val root: File,
    private val performanceMonitor: GitPerformanceMonitor? = null,
    private val now: () -> Long = System::currentTimeMillis,
    val detector: GitSha1CollisionDetector? = GitSha1DcDetector(),
) {
    private val lock = Any()
    private val writeLock = Any()
    private val loadLock = Any()
    private val listeners = CopyOnWriteArrayList<() -> Unit>()

    @Volatile
    private var recordList: List<LocalRepoRecord> = emptyList()

    @Volatile
    private var loaded = false

    @Volatile
    private var activeWorkspace: GitWorkspace? = null

    val isBusy: Boolean get() = activeWorkspace != null

    fun cancelActive() {
        activeWorkspace?.cancel()
    }

    val records: List<LocalRepoRecord> get() = recordList

    val isLoaded: Boolean get() = loaded

    fun addListener(listener: () -> Unit) {
        listeners.add(listener)
    }

    fun removeListener(listener: () -> Unit) {
        listeners.remove(listener)
    }

    private fun notifyListeners() {
        for (listener in listeners) listener()
    }

    private val indexFile: File get() = File(root, indexFileName)

    fun directoryFor(name: String): File {
        if (!isValidName(name)) throw LocalRepoStoreException("Repository name is not valid.")
        ensureSafeRoot()
        val directory = File(root, name)
        if (typeOf(directory) == EntryType.link) {
            throw LocalRepoStoreException("Repository path is a symlink and was refused.")
        }
        return directory
    }

    private fun ensureSafeRoot() {
        var current: File = root.absoluteFile
        val missing = ArrayList<File>()
        while (true) {
            val type = typeOf(current)
            if (type == EntryType.directory) break
            if (type == EntryType.link) {
                throw LocalRepoStoreException("Local repository root is a symlink and was refused.")
            }
            if (type != EntryType.notFound) {
                throw LocalRepoStoreException("Local repository root is not a directory.")
            }
            missing.add(current)
            current = current.parentFile ?: break
        }
        for (directory in missing.asReversed()) {
            try {
                Files.createDirectory(directory.toPath())
            } catch (error: java.nio.file.FileAlreadyExistsException) {
                if (typeOf(directory) != EntryType.directory) throw error
            }
        }
    }

    fun ensureLoaded() {
        if (loaded) return
        synchronized(loadLock) {
            if (loaded) return
            load()
        }
    }

    private fun load() {
        ensureSafeRoot()
        val file = indexFile
        var parsed = ArrayList<LocalRepoRecord>()
        if (typeOf(file) == EntryType.file) {
            try {
                val json = Json.parse(file.readText(Charsets.UTF_8))
                val repos = (json as? Map<*, *>)?.get("repos")
                if (json is Map<*, *> && repos is List<*>) {
                    for (item in repos) {
                        @Suppress("UNCHECKED_CAST")
                        val map = item as? Map<String, Any?> ?: continue
                        LocalRepoRecord.fromJson(map)?.let(parsed::add)
                    }
                }
            } catch (error: Json.JsonException) {
                parsed = ArrayList()
            }
        }
        var changed = false
        val alive = ArrayList<LocalRepoRecord>()
        for (record in parsed) {
            val rebased = rebasedOnRoot(record)
            if (rebased !== record) changed = true
            if (typeOf(File(rebased.path)) == EntryType.directory) alive.add(rebased) else changed = true
        }
        if (adoptOrphans(alive)) changed = true
        val sorted = alive.sortedWith(byRecency)
        synchronized(lock) { recordList = sorted }
        loaded = true
        if (changed) persist()
        notifyListeners()
    }

    private fun rebasedOnRoot(record: LocalRepoRecord): LocalRepoRecord {
        val expected = File(root, record.name).path
        return if (expected == record.path) record else record.copy(path = expected)
    }

    private fun adoptOrphans(known: MutableList<LocalRepoRecord>): Boolean {
        val named = known.map { it.name }.toMutableSet()
        val entries = root.listFiles() ?: return false
        var adopted = false
        for (entry in entries.sortedBy { it.name }) {
            val name = entry.name
            if (!isValidName(name) || name in named) continue
            if (typeOf(entry) != EntryType.directory || typeOf(File(entry, ".git")) != EntryType.directory) continue
            known.add(LocalRepoRecord(name = name, path = entry.path, createdAt = entry.lastModified()))
            named.add(name)
            adopted = true
        }
        return adopted
    }

    private fun persist() {
        synchronized(writeLock) {
            ensureSafeRoot()
            val file = indexFile
            val tmp = File(root, "$indexFileName.tmp")
            val payload = DartJsonIndent.encode(
                linkedMapOf("version" to indexVersion, "repos" to records.map { it.toJson() }),
            )
            FileOutputStream(tmp).use { stream ->
                stream.write(payload.toByteArray(Charsets.UTF_8))
                stream.fd.sync()
            }
            try {
                Files.move(tmp.toPath(), file.toPath(), StandardCopyOption.ATOMIC_MOVE, StandardCopyOption.REPLACE_EXISTING)
            } catch (error: AtomicMoveNotSupportedException) {
                Files.move(tmp.toPath(), file.toPath(), StandardCopyOption.REPLACE_EXISTING)
            }
        }
    }

    fun list(): List<LocalRepoRecord> {
        synchronized(loadLock) {
            loaded = false
            load()
        }
        return records
    }

    fun find(name: String): LocalRepoRecord? {
        ensureLoaded()
        return records.firstOrNull { it.name == name }
    }

    fun record(meshIdentity: String): LocalRepoRecord? {
        val wanted = meshIdentity.dartTrim().lowercase()
        if (wanted.isEmpty()) return null
        return records.firstOrNull { (it.publishedAs ?: "").dartTrim().lowercase() == wanted }
    }

    fun recordFor(repository: Repository): LocalRepoRecord? =
        record(repository.fullName) ?: record("${repository.owner}/${repository.name}")

    fun sync(
        record: LocalRepoRecord,
        publisher: PublishService,
        onProgress: ((GitTransferProgress) -> Unit)? = null,
    ): LocalPullResult {
        val (owner, repo) = record.meshRoute
            ?: throw LocalRepoStoreException("${record.name} is only on this device. Publish it before syncing.")
        val workspace = workspace(record)
        activeWorkspace = workspace
        try {
            val branch = workspace.summary().branch
            if (branch == "HEAD") throw LocalRepoStoreException("Switch ${record.name} to a branch before syncing.")
            val result = publisher.pull(workspace, owner, repo, branch, onProgress = onProgress)
            try {
                updateSummary(record.name, workspace.summary())
            } catch (error: Exception) {
            }
            return result
        } finally {
            if (activeWorkspace === workspace) activeWorkspace = null
            workspace.dispose()
        }
    }

    open fun workspace(record: LocalRepoRecord): GitWorkspace =
        GitWorkspace(File(record.path), performanceMonitor = performanceMonitor, detector = detector)

    fun create(name: String, author: GitAuthor, description: String = "", branch: String = "main"): LocalRepoRecord {
        val directory = reserve(name)
        val workspace = workspace0(directory)
        activeWorkspace = workspace
        try {
            val readme = StringBuilder("# $name\n")
            if (description.dartTrim().isNotEmpty()) readme.append("\n${description.dartTrim()}\n")
            workspace.init(files = mapOf("README.md" to readme.toString()), commitMessage = "Initial commit\n", author = author, branch = branch)
            return register(name, directory, description, workspace)
        } catch (error: Throwable) {
            cleanupFailed(directory)
            throw error
        } finally {
            activeWorkspace = null
            workspace.dispose()
        }
    }

    fun importDirectory(name: String, source: File, author: GitAuthor, description: String = ""): LocalRepoRecord =
        importTree(name, FileImportDirectory(source), author, description)

    fun importTree(name: String, source: LocalImportDirectory, author: GitAuthor, description: String = ""): LocalRepoRecord {
        val directory = reserve(name)
        val workspace = workspace0(directory)
        activeWorkspace = workspace
        try {
            workspace.importFromTree(source, author = author, message = "Import from Files\n")
            return register(name, directory, description, workspace)
        } catch (error: Throwable) {
            cleanupFailed(directory)
            throw error
        } finally {
            activeWorkspace = null
            workspace.dispose()
        }
    }

    fun importZip(name: String, zipPath: String, author: GitAuthor, description: String = ""): LocalRepoRecord {
        val directory = reserve(name)
        val workspace = workspace0(directory)
        activeWorkspace = workspace
        try {
            workspace.importFromZip(zipPath, author = author, message = "Import from Files\n")
            return register(name, directory, description, workspace)
        } catch (error: Throwable) {
            cleanupFailed(directory)
            throw error
        } finally {
            activeWorkspace = null
            workspace.dispose()
        }
    }

    fun cloneFromMesh(
        name: String,
        credentials: ForkMeshRemoteCredentials,
        description: String = "",
        branch: String? = null,
        maxPackBytes: Int = GitWorkspace.maxCloneBytes,
        onProgress: ((GitTransferProgress) -> Unit)? = null,
        depth: Int? = null,
    ): LocalRepoRecord {
        ensureLoaded()
        val identity = "${credentials.owner}/${credentials.repo}"
        record(identity)?.let { throw LocalRepoStoreException(alreadyOnDevice(identity, it.name)) }
        val directory = reserve(name)
        val workspace = workspace0(directory)
        activeWorkspace = workspace
        try {
            workspace.clone(
                credentials = credentials,
                branch = branch,
                maxPackBytes = maxPackBytes,
                onProgress = onProgress,
                depth = depth,
            )
            val record = register(name, directory, description, workspace)
            val publishedAs = "${credentials.owner}/${credentials.repo}"
            mark(name) { it.copyWith(publishedAs = publishedAs, origin = LocalRepoOrigin.cloned) }
            return record.copyWith(publishedAs = publishedAs, origin = LocalRepoOrigin.cloned)
        } catch (error: Throwable) {
            cleanupFailed(directory)
            throw error
        } finally {
            activeWorkspace = null
            workspace.dispose()
        }
    }

    fun refresh(name: String): LocalRepoRecord? {
        val record = find(name) ?: return null
        val workspace = workspace(record)
        try {
            return updateSummary(name, workspace.summary())
        } finally {
            workspace.dispose()
        }
    }

    fun updateSummary(name: String, summary: LocalRepoSummary): LocalRepoRecord? {
        val record = find(name) ?: return null
        val updated = record.copyWith(
            branch = summary.branch,
            lastCommitSubject = summary.lastCommitSubject,
            lastCommitAt = summary.lastCommitAt,
            clearLastCommit = summary.lastCommitAt == null,
        )
        synchronized(lock) {
            recordList = recordList.map { if (it.name == name) updated else it }.sortedWith(byRecency)
        }
        persist()
        notifyListeners()
        return updated
    }

    fun markPublished(name: String, publishedAs: String) = mark(name) { it.copyWith(publishedAs = publishedAs) }

    private fun mark(name: String, update: (LocalRepoRecord) -> LocalRepoRecord) {
        ensureLoaded()
        synchronized(lock) {
            recordList = recordList.map { if (it.name == name) update(it) else it }
        }
        persist()
        notifyListeners()
    }

    fun delete(name: String) {
        ensureLoaded()
        val directory = directoryFor(name)
        if (typeOf(directory) == EntryType.directory) deleteRepositoryDirectory(directory)
        synchronized(lock) {
            recordList = recordList.filter { it.name != name }
        }
        persist()
        notifyListeners()
    }

    open fun workspace0(directory: File): GitWorkspace =
        GitWorkspace(directory, performanceMonitor = performanceMonitor, detector = detector)

    private fun reserve(name: String): File {
        validateName(name)?.let { throw LocalRepoStoreException(it) }
        ensureLoaded()
        if (records.any { it.name == name }) {
            throw LocalRepoStoreException("A local repository with that name already exists.")
        }
        val directory = directoryFor(name)
        if (typeOf(directory) != EntryType.notFound) {
            throw LocalRepoStoreException("A folder with that name already exists on this device.")
        }
        Files.createDirectory(directory.toPath())
        return directory
    }

    private fun register(name: String, directory: File, description: String, workspace: GitWorkspace): LocalRepoRecord {
        val summary = workspace.summary()
        val record = LocalRepoRecord(
            name = name,
            path = directory.path,
            description = description.dartTrim(),
            createdAt = now(),
            branch = summary.branch,
            lastCommitSubject = summary.lastCommitSubject,
            lastCommitAt = summary.lastCommitAt,
        )
        synchronized(lock) {
            recordList = (listOf(record) + recordList).sortedWith(byRecency)
        }
        persist()
        notifyListeners()
        return record
    }

    private fun cleanupFailed(directory: File) {
        try {
            if (typeOf(directory) == EntryType.directory) deleteRepositoryDirectory(directory)
        } catch (error: Exception) {
        }
    }

    private fun deleteRepositoryDirectory(directory: File) {
        val name = directory.name
        val rootPath = root.toPath().toRealPath()
        val target = directory.toPath()
        if (!isValidName(name) ||
            typeOf(directory) != EntryType.directory ||
            target.parent?.toRealPath() != rootPath ||
            target.toRealPath(LinkOption.NOFOLLOW_LINKS).parent != rootPath
        ) {
            throw LocalRepoStoreException("Repository path is outside the local repository root and was refused.")
        }
        Files.walkFileTree(
            target,
            object : SimpleFileVisitor<Path>() {
                override fun visitFile(file: Path, attrs: BasicFileAttributes): FileVisitResult {
                    Files.delete(file)
                    return FileVisitResult.CONTINUE
                }

                override fun visitFileFailed(file: Path, exc: IOException): FileVisitResult {
                    Files.deleteIfExists(file)
                    return FileVisitResult.CONTINUE
                }

                override fun postVisitDirectory(dir: Path, exc: IOException?): FileVisitResult {
                    if (exc != null) throw exc
                    Files.delete(dir)
                    return FileVisitResult.CONTINUE
                }
            },
        )
    }

    private enum class EntryType { file, directory, link, notFound, other }

    companion object {
        fun open(filesDir: File, performanceMonitor: GitPerformanceMonitor? = null): LocalRepoStore =
            LocalRepoStore(File(File(filesDir, "forkmesh"), "repos"), performanceMonitor = performanceMonitor)

        fun rootIn(documentsDir: File, projectId: String? = null): File {
            val base = if (projectId == null) documentsDir else File(File(documentsDir, "projects"), projectSlug(projectId))
            return File(File(base, "forkmesh"), "repos")
        }

        fun openInDocuments(
            documentsDir: File,
            projectId: String? = null,
            performanceMonitor: GitPerformanceMonitor? = null,
        ): LocalRepoStore = LocalRepoStore(rootIn(documentsDir, projectId), performanceMonitor = performanceMonitor)

        fun projectSlug(projectId: String): String = projectId.lowercase().replace(Regex("[^a-z0-9._-]"), "_")

        val namePattern = Regex("[A-Za-z0-9._-]{1,100}")
        const val indexFileName = "index.json"
        const val indexVersion = 1

        fun alreadyOnDevice(identity: String, name: String): String =
            "$identity is already on this device as $name. Open it and sync instead."

        fun isValidName(name: String): Boolean = namePattern.matches(name) && name != "." && name != ".."

        fun validateName(name: String): String? {
            if (name.isEmpty()) return "Enter a name."
            if (name.length > 100) return "Keep the name under 100 characters."
            if (!isValidName(name)) return "Use letters, digits, dots, dashes, or underscores only."
            return null
        }

        private val byRecency = Comparator<LocalRepoRecord> { a, b ->
            (b.lastCommitAt ?: b.createdAt).compareTo(a.lastCommitAt ?: a.createdAt)
        }

        private fun typeOf(file: File): EntryType {
            val attributes = try {
                Files.readAttributes(file.toPath(), BasicFileAttributes::class.java, LinkOption.NOFOLLOW_LINKS)
            } catch (error: IOException) {
                return EntryType.notFound
            }
            return when {
                attributes.isSymbolicLink -> EntryType.link
                attributes.isDirectory -> EntryType.directory
                attributes.isRegularFile -> EntryType.file
                else -> EntryType.other
            }
        }
    }
}

internal object DartDateTime {
    fun toIso8601Utc(millis: Long): String {
        val time = LocalDateTime.ofEpochSecond(Math.floorDiv(millis, 1000L), 0, ZoneOffset.UTC)
        val year = time.year
        val yearText = if (year in 0..9999) {
            year.toString().padStart(4, '0')
        } else {
            (if (year < 0) "-" else "+") + Math.abs(year).toString().padStart(6, '0')
        }
        fun two(value: Int) = value.toString().padStart(2, '0')
        val ms = Math.floorMod(millis, 1000L).toString().padStart(3, '0')
        return "$yearText-${two(time.monthValue)}-${two(time.dayOfMonth)}T" +
            "${two(time.hour)}:${two(time.minute)}:${two(time.second)}.${ms}Z"
    }

    private val format = Regex(
        "([+-]?\\d{4,6})-?(\\d\\d)-?(\\d\\d)" +
            "(?:[ T](\\d\\d)(?::?(\\d\\d)(?::?(\\d\\d)(?:[.,](\\d+))?)?)?" +
            "( ?[zZ]| ?([-+])(\\d\\d)(?::?(\\d\\d))?)?)?",
    )

    fun tryParse(text: String): Long? {
        val match = format.matchEntire(text) ?: return null
        val groups = match.groupValues
        fun number(index: Int): Int = if (groups[index].isEmpty()) 0 else groups[index].toInt()
        val year = groups[1].toInt()
        val fraction = groups[7]
        val millis = if (fraction.isEmpty()) 0 else fraction.padEnd(3, '0').substring(0, 3).toInt()
        val local = try {
            LocalDateTime.of(year, 1, 1, 0, 0)
                .plusMonths((number(2) - 1).toLong())
                .plusDays((number(3) - 1).toLong())
                .plusHours(number(4).toLong())
                .plusMinutes(number(5).toLong())
                .plusSeconds(number(6).toLong())
        } catch (error: Exception) {
            return null
        }
        val epochSecond = if (groups[8].isEmpty()) {
            local.atZone(ZoneId.systemDefault()).toEpochSecond()
        } else {
            var seconds = local.toEpochSecond(ZoneOffset.UTC)
            if (groups[9].isNotEmpty()) {
                val offset = number(10) * 3600 + number(11) * 60
                seconds -= if (groups[9] == "-") -offset else offset
            }
            seconds
        }
        return epochSecond * 1000 + millis
    }
}

internal object DartJsonIndent {
    fun encode(value: Any?): String = StringBuilder().also { write(it, value, 0) }.toString()

    private fun write(out: StringBuilder, value: Any?, depth: Int) {
        when (value) {
            is Map<*, *> -> {
                if (value.isEmpty()) {
                    out.append("{}")
                    return
                }
                out.append("{\n")
                var first = true
                for ((key, item) in value) {
                    if (!first) out.append(",\n")
                    first = false
                    indent(out, depth + 1)
                    out.append(Json.stringify(key.toString())).append(": ")
                    write(out, item, depth + 1)
                }
                out.append('\n')
                indent(out, depth)
                out.append('}')
            }
            is List<*> -> {
                if (value.isEmpty()) {
                    out.append("[]")
                    return
                }
                out.append("[\n")
                value.forEachIndexed { index, item ->
                    if (index > 0) out.append(",\n")
                    indent(out, depth + 1)
                    write(out, item, depth + 1)
                }
                out.append('\n')
                indent(out, depth)
                out.append(']')
            }
            else -> out.append(Json.stringify(value))
        }
    }

    private fun indent(out: StringBuilder, depth: Int) {
        repeat(depth) { out.append("  ") }
    }
}
