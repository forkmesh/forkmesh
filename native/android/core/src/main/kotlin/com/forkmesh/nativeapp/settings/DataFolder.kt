package com.forkmesh.nativeapp.settings

import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.crypto.B64
import com.forkmesh.nativeapp.crypto.ChatDeviceKeys
import com.forkmesh.nativeapp.crypto.Ed25519
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.security.Identity
import com.forkmesh.nativeapp.security.SecretStore
import java.io.File
import java.io.InputStream
import java.io.OutputStream

enum class DataFolderNeed { ChooseFolder, ReconnectFolder }

data class DataFolderProject(
    val slug: String,
    val projectId: String,
    val namespace: String,
    val name: String,
    val publicKey: String,
)

data class DataFolderTree(val name: String, val local: File, val skippedTopLevel: Set<String> = emptySet())

data class DataFolderIndexEntry(val local: String, val signature: String)

data class DataFolderCopyReport(var copied: Int = 0, var unchanged: Int = 0, val failed: MutableList<String> = mutableListOf())

interface DataFolderIo {
    fun read(path: String): ByteArray?

    fun write(path: String, bytes: ByteArray): Boolean

    fun openOutput(path: String): OutputStream?

    fun openInput(path: String): InputStream?

    fun listNames(path: String): List<String>

    fun makeDirs(path: String): Boolean
}

class FileDataFolderIo(private val root: File) : DataFolderIo {
    private fun file(path: String): File = if (path.isEmpty()) root else File(root, path)

    override fun read(path: String): ByteArray? = file(path).takeIf { it.isFile }?.readBytes()

    override fun write(path: String, bytes: ByteArray): Boolean = try {
        val target = file(path)
        target.parentFile?.mkdirs()
        val partial = File(target.parentFile, target.name + DataFolder.PARTIAL_SUFFIX)
        partial.writeBytes(bytes)
        partial.renameTo(target)
    } catch (_: Exception) {
        false
    }

    override fun openOutput(path: String): OutputStream? = try {
        file(path).also { it.parentFile?.mkdirs() }.outputStream()
    } catch (_: Exception) {
        null
    }

    override fun openInput(path: String): InputStream? = try {
        file(path).takeIf { it.isFile }?.inputStream()
    } catch (_: Exception) {
        null
    }

    override fun listNames(path: String): List<String> = file(path).list()?.sorted() ?: emptyList()

    override fun makeDirs(path: String): Boolean = file(path).let { it.isDirectory || it.mkdirs() }
}

object DataFolder {
    const val DEFAULT_FOLDER_NAME = "ForkMesh projects"
    const val HOME_PROJECT = "forkmesh"
    const val MARKER_FILE = "forkmesh-data.json"
    const val MARKER_TYPE = "forkmesh.data-folder"
    const val PROJECT_FILE = "project.json"
    const val SETTINGS_PATH = "data/core/config/ForkMesh/settings.json"
    const val KIT_PATH = "data/core/identity/recovery-kit.json"
    const val INDEX_PATH = "data/core/state/sync-index.json"
    const val SETTINGS_TYPE = "forkmesh.android-settings"
    const val PARTIAL_SUFFIX = ".fm-partial"
    val ROOTS = listOf("data/core", "data/core/config", "data/core/cache", "data/core/state", "data/core/logs", "data/core/ssh")
    val PLUGIN_ENTRIES = mapOf(
        "chat-history" to "chat/history",
        "local_mesh_history.json" to "chat/local-mesh/local_mesh_history.json",
        "chat-people-pictures" to "chat/people-pictures",
        "diagnostics" to "core/logs/diagnostics",
        "NextTask" to "agents/next-task",
        "secure-drive" to "secure-drive",
        "rss-reader" to "rss-reader",
    )

    const val LOCATION_KEY = "dataFolder/location"
    const val SYNCED_AT_KEY = "dataFolder/syncedAt"
    const val KIT_KEY_PREFIX = "dataFolder/kitKey/"
    val DEVICE_PREFIXES = listOf("dataFolder/", "install/")
    val SKIPPED_NAMES = setOf("LocalModels", "CodeInsight", ".Trash", "tmp")

    const val ALERT_TITLE = "Keep your ForkMesh data safe"
    const val ALERT_MESSAGE =
        "ForkMesh keeps your projects in its own folder, and Android deletes that folder with the app. Choose a folder, for example Documents on this phone: each project keeps its data there, data/core for settings and the identity key and one folder per plugin, so you can reinstall and pick the same folder to get everything back."
    const val RECONNECT_MESSAGE =
        "The folder holding your ForkMesh projects can't be opened. Choose it again so ForkMesh can keep your data safe."
    const val FOLDER_NAME_HINT =
        "Suggested name: “$DEFAULT_FOLDER_NAME”. In the picker, make a new folder with that name and choose it."

    fun projectFolder(scope: ProjectScope): String = if (scope.isLegacy) HOME_PROJECT else scope.slug

    fun kitKey(slug: String): String = KIT_KEY_PREFIX + slug

    fun folderPath(tree: String, relative: String): String {
        val first = relative.substringBefore('/')
        val mapped = PLUGIN_ENTRIES[first] ?: return "data/core/$tree/$relative"
        return "data/$mapped" + relative.substring(first.length)
    }

    fun need(reachable: Boolean, insideApp: Boolean): DataFolderNeed? {
        if (!reachable) return DataFolderNeed.ReconnectFolder
        if (insideApp) return DataFolderNeed.ChooseFolder
        return null
    }

    fun identitySaved(prefs: PrefStore, slug: String, publicKey: String): Boolean =
        publicKey.isEmpty() || prefs.getString(kitKey(slug)) == publicKey

    fun message(need: DataFolderNeed): String = when (need) {
        DataFolderNeed.ChooseFolder -> ALERT_MESSAGE
        DataFolderNeed.ReconnectFolder -> RECONNECT_MESSAGE
    }

    fun isDeviceKey(key: String): Boolean = DEVICE_PREFIXES.any { key.startsWith(it) }

    fun exportSettings(values: Map<String, Any?>): Map<String, Any?> {
        val out = linkedMapOf<String, Any?>()
        for (key in values.keys.sorted()) {
            if (isDeviceKey(key)) continue
            val tagged = when (val value = values[key]) {
                is String -> linkedMapOf("t" to "string", "v" to value)
                is Boolean -> linkedMapOf("t" to "bool", "v" to value)
                is Long -> linkedMapOf("t" to "long", "v" to value.toString())
                is Int -> linkedMapOf("t" to "int", "v" to value.toLong())
                is Float -> linkedMapOf("t" to "float", "v" to value.toDouble())
                is Set<*> -> linkedMapOf("t" to "set", "v" to value.map { it.toString() }.sorted())
                else -> null
            } ?: continue
            out[key] = tagged
        }
        return linkedMapOf("type" to SETTINGS_TYPE, "schemaVersion" to 1L, "values" to out)
    }

    fun decodeSettings(document: Map<String, Any?>): Map<String, Any> {
        if (document["type"] != SETTINGS_TYPE) return emptyMap()
        val values = document["values"] as? Map<*, *> ?: return emptyMap()
        val out = linkedMapOf<String, Any>()
        for ((rawKey, rawValue) in values) {
            val key = rawKey as? String ?: continue
            if (isDeviceKey(key)) continue
            val tagged = rawValue as? Map<*, *> ?: continue
            val value: Any = when (tagged["t"]) {
                "string" -> tagged["v"] as? String
                "bool" -> tagged["v"] as? Boolean
                "long" -> (tagged["v"] as? String)?.toLongOrNull()
                "int" -> (tagged["v"] as? Number)?.toInt()
                "float" -> (tagged["v"] as? Number)?.toFloat()
                "set" -> (tagged["v"] as? List<*>)?.map { it.toString() }?.toSet()
                else -> null
            } ?: continue
            out[key] = value
        }
        return out
    }

    fun signature(size: Long, modifiedMillis: Long): String = "$size:$modifiedMillis"
}

class DataFolderStore(private val io: DataFolderIo) {
    val hasData: Boolean
        get() = io.read(DataFolder.MARKER_FILE)?.let { parse(it)["type"] == DataFolder.MARKER_TYPE } == true

    fun prepare(platform: String, nowMillis: Long): Boolean {
        val existing = io.read(DataFolder.MARKER_FILE)?.let { parse(it) }
        val marker = if (existing?.get("type") == DataFolder.MARKER_TYPE) {
            LinkedHashMap(existing)
        } else {
            linkedMapOf<String, Any?>("type" to DataFolder.MARKER_TYPE, "schemaVersion" to 1L, "createdAt" to nowMillis)
        }
        marker["updatedAt"] = nowMillis
        marker["updatedBy"] = platform
        return io.write(DataFolder.MARKER_FILE, Json.stringify(marker).toByteArray())
    }

    fun prepareProject(project: DataFolderProject): Boolean {
        val description = linkedMapOf<String, Any?>(
            "slug" to project.slug, "projectId" to project.projectId, "namespace" to project.namespace,
            "name" to project.name, "publicKey" to project.publicKey,
        )
        var ok = io.write("${project.slug}/${DataFolder.PROJECT_FILE}", Json.stringify(description).toByteArray())
        for (root in DataFolder.ROOTS) ok = io.makeDirs("${project.slug}/$root") && ok
        return ok
    }

    fun projects(): List<DataFolderProject> = io.listNames("").mapNotNull { name ->
        val object_ = io.read("$name/${DataFolder.PROJECT_FILE}")?.let { parse(it) } ?: return@mapNotNull null
        if (object_["slug"] != name) return@mapNotNull null
        DataFolderProject(
            name, object_["projectId"] as? String ?: "", object_["namespace"] as? String ?: "",
            object_["name"] as? String ?: "", object_["publicKey"] as? String ?: "",
        )
    }

    fun writeSettings(document: Map<String, Any?>, slug: String): Boolean =
        io.write("$slug/${DataFolder.SETTINGS_PATH}", Json.stringify(document).toByteArray())

    fun readSettings(slug: String): Map<String, Any?>? =
        io.read("$slug/${DataFolder.SETTINGS_PATH}")?.let { parse(it) }?.takeIf { it.isNotEmpty() }

    fun writeKit(kit: String, slug: String): Boolean = io.write("$slug/${DataFolder.KIT_PATH}", kit.toByteArray())

    fun readKit(slug: String): String? = io.read("$slug/${DataFolder.KIT_PATH}")?.toString(Charsets.UTF_8)

    fun readIndex(slug: String): MutableMap<String, DataFolderIndexEntry> {
        val parsed = io.read("$slug/${DataFolder.INDEX_PATH}")?.let { parse(it) } ?: return linkedMapOf()
        val index = linkedMapOf<String, DataFolderIndexEntry>()
        for ((key, value) in parsed) {
            val entry = value as? Map<*, *> ?: continue
            val local = entry["local"] as? String ?: continue
            index[key] = DataFolderIndexEntry(local, entry["sig"] as? String ?: "")
        }
        return index
    }

    private fun writeIndex(slug: String, index: Map<String, DataFolderIndexEntry>) {
        val out = sortedMapOf<String, Any?>()
        for ((key, entry) in index) out[key] = linkedMapOf("local" to entry.local, "sig" to entry.signature)
        io.write("$slug/${DataFolder.INDEX_PATH}", Json.stringify(out).toByteArray())
    }

    fun backup(slug: String, trees: List<DataFolderTree>): DataFolderCopyReport {
        val report = DataFolderCopyReport()
        val index = readIndex(slug)
        for (tree in trees) {
            for ((relative, file) in walk(tree.local, tree.skippedTopLevel)) {
                val key = DataFolder.folderPath(tree.name, relative)
                val local = "${tree.name}/$relative"
                val signature = DataFolder.signature(file.length(), file.lastModified())
                if (index[key]?.let { it.signature == signature && it.local == local } == true) {
                    report.unchanged++
                    continue
                }
                val copied = try {
                    io.openOutput("$slug/$key")?.use { out -> file.inputStream().use { it.copyTo(out) } } != null
                } catch (_: Exception) {
                    false
                }
                if (copied) {
                    index[key] = DataFolderIndexEntry(local, signature)
                    report.copied++
                } else {
                    report.failed.add(key)
                }
            }
        }
        writeIndex(slug, index)
        return report
    }

    fun restore(slug: String, trees: List<DataFolderTree>): DataFolderCopyReport {
        val report = DataFolderCopyReport()
        val byName = trees.associateBy { it.name }
        for ((key, entry) in readIndex(slug).toSortedMap()) {
            val parts = entry.local.split('/')
            val tree = byName[parts.first()] ?: continue
            if (parts.size < 2 || parts.any { it == ".." || it == "." || it.isEmpty() }) continue
            if (key.split('/').any { it == ".." }) continue
            val target = File(tree.local, parts.drop(1).joinToString("/"))
            if (target.exists()) {
                report.unchanged++
                continue
            }
            val copied = try {
                target.parentFile?.mkdirs()
                val partial = File(target.parentFile, target.name + DataFolder.PARTIAL_SUFFIX)
                val read = io.openInput("$slug/$key")?.use { input -> partial.outputStream().use { input.copyTo(it) } } != null
                read && partial.renameTo(target)
            } catch (_: Exception) {
                false
            }
            if (copied) report.copied++ else report.failed.add(key)
        }
        return report
    }

    private fun walk(base: File, skippedTopLevel: Set<String>): List<Pair<String, File>> {
        if (!base.isDirectory) return emptyList()
        val found = mutableListOf<Pair<String, File>>()
        fun visit(dir: File, prefix: String) {
            val children = dir.listFiles()?.sortedBy { it.name } ?: return
            for (child in children) {
                if (child.name in DataFolder.SKIPPED_NAMES) continue
                if (prefix.isEmpty() && child.name in skippedTopLevel) continue
                val relative = if (prefix.isEmpty()) child.name else "$prefix/${child.name}"
                if (java.nio.file.Files.isSymbolicLink(child.toPath())) continue
                if (child.isDirectory) {
                    visit(child, relative)
                } else if (child.isFile && !child.name.endsWith(DataFolder.PARTIAL_SUFFIX)) {
                    found.add(relative to child)
                }
            }
        }
        visit(base, "")
        return found
    }

    private fun parse(bytes: ByteArray): Map<String, Any?> = try {
        Json.parseObject(bytes.toString(Charsets.UTF_8))
    } catch (_: Exception) {
        emptyMap()
    }
}

object DataFolderIdentity {
    fun saveKit(identity: DeviceIdentity, passphrase: String, confirm: String, slug: String, store: DataFolderStore, prefs: PrefStore, nowMillis: Long) {
        val kit = RecoveryKit.export(identity, passphrase, confirm, nowMillis)
        if (!store.writeKit(kit, slug)) throw RecoveryKitException("Could not write the identity key into the data folder.")
        prefs.putString(DataFolder.kitKey(slug), identity.publicKeyB64url)
    }

    fun restoreSeed(kit: String, passphrase: String, namespace: String, secrets: SecretStore): String {
        val seed = RecoveryKit.openSeed(kit, passphrase)
        try {
            val publicKey = B64.encodeUrlNoPadding(Ed25519.publicKey(seed))
            val current = ChatDeviceKeys.storedSeed(secrets.read(namespace + Identity.SEED_KEY))
            val changed = current == null || !current.contentEquals(seed)
            current?.fill(0)
            secrets.write(namespace + Identity.SEED_KEY, B64.encode(seed))
            if (changed) secrets.delete(namespace + ChatDeviceKeys.IDENTITY_STORE_KEY)
            return publicKey
        } finally {
            seed.fill(0)
        }
    }
}
