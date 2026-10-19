package com.forkmesh.nativeapp.feature.account.settings

import android.content.ContentResolver
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.DocumentsContract
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.account.projects.ProjectScope
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.PrefsStore
import com.forkmesh.nativeapp.feature.account.projects.AppFiles
import com.forkmesh.nativeapp.feature.account.projects.ProjectsService
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.platform.SecureStore
import com.forkmesh.nativeapp.security.DeviceIdentity
import com.forkmesh.nativeapp.settings.DataFolder
import com.forkmesh.nativeapp.settings.DataFolderIdentity
import com.forkmesh.nativeapp.settings.DataFolderIo
import com.forkmesh.nativeapp.settings.DataFolderNeed
import com.forkmesh.nativeapp.settings.DataFolderProject
import com.forkmesh.nativeapp.settings.DataFolderStore
import com.forkmesh.nativeapp.settings.DataFolderTree
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import java.io.InputStream
import java.io.OutputStream

class SafDataFolderIo(private val resolver: ContentResolver, private val tree: Uri, private val rootId: String) : DataFolderIo {
    private val ids = HashMap<String, String>()

    private fun documentUri(id: String): Uri = DocumentsContract.buildDocumentUriUsingTree(tree, id)

    private fun childId(parentId: String, name: String): String? = try {
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId)
        resolver.query(children, arrayOf(DocumentsContract.Document.COLUMN_DOCUMENT_ID, DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { cursor ->
            var found: String? = null
            while (found == null && cursor.moveToNext()) {
                if (cursor.getString(1) == name) found = cursor.getString(0)
            }
            found
        }
    } catch (_: Exception) {
        null
    }

    private fun childNames(parentId: String): List<String> = try {
        val children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, parentId)
        resolver.query(children, arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)?.use { cursor ->
            val names = mutableListOf<String>()
            while (cursor.moveToNext()) cursor.getString(0)?.let(names::add)
            names.sorted()
        } ?: emptyList()
    } catch (_: Exception) {
        emptyList()
    }

    @Synchronized
    private fun resolve(path: String, createDirectories: Boolean): String? {
        if (path.isEmpty()) return rootId
        ids[path]?.let { return it }
        var current = rootId
        var walked = ""
        for (segment in path.split('/')) {
            if (segment.isEmpty() || segment == "." || segment == "..") return null
            walked = if (walked.isEmpty()) segment else "$walked/$segment"
            val known = ids[walked] ?: childId(current, segment) ?: if (createDirectories) {
                try {
                    DocumentsContract.createDocument(resolver, documentUri(current), DocumentsContract.Document.MIME_TYPE_DIR, segment)
                        ?.let(DocumentsContract::getDocumentId)
                } catch (_: Exception) {
                    null
                }
            } else {
                null
            }
            current = known ?: return null
            ids[walked] = current
        }
        return current
    }

    @Synchronized
    private fun fileId(path: String, create: Boolean): String? {
        ids[path]?.let { return it }
        val parent = path.substringBeforeLast('/', "")
        val name = path.substringAfterLast('/')
        val parentId = resolve(parent, create) ?: return null
        val existing = childId(parentId, name)
        val id = existing ?: if (create) {
            try {
                val mime = if (name.endsWith(".json")) "application/json" else "application/octet-stream"
                DocumentsContract.createDocument(resolver, documentUri(parentId), mime, name)?.let(DocumentsContract::getDocumentId)
            } catch (_: Exception) {
                null
            }
        } else {
            null
        }
        if (id != null) ids[path] = id
        return id
    }

    override fun read(path: String): ByteArray? = openInput(path)?.use { it.readBytes() }

    override fun write(path: String, bytes: ByteArray): Boolean = try {
        openOutput(path)?.use { it.write(bytes) } != null
    } catch (_: Exception) {
        false
    }

    override fun openOutput(path: String): OutputStream? = try {
        fileId(path, create = true)?.let { resolver.openOutputStream(documentUri(it), "wt") }
    } catch (_: Exception) {
        null
    }

    override fun openInput(path: String): InputStream? = try {
        fileId(path, create = false)?.let { resolver.openInputStream(documentUri(it)) }
    } catch (_: Exception) {
        null
    }

    override fun listNames(path: String): List<String> = resolve(path, createDirectories = false)?.let(::childNames) ?: emptyList()

    override fun makeDirs(path: String): Boolean = resolve(path, createDirectories = true) != null

    companion object {
        fun open(context: Context, tree: Uri, create: Boolean): SafDataFolderIo? {
            val resolver = context.contentResolver
            val treeId = try {
                DocumentsContract.getTreeDocumentId(tree)
            } catch (_: Exception) {
                return null
            }
            val picked = SafDataFolderIo(resolver, tree, treeId)
            val name = try {
                resolver.query(DocumentsContract.buildDocumentUriUsingTree(tree, treeId), arrayOf(DocumentsContract.Document.COLUMN_DISPLAY_NAME), null, null, null)
                    ?.use { if (it.moveToFirst()) it.getString(0) else null }
            } catch (_: Exception) {
                null
            } ?: return null
            if (name == DataFolder.FOLDER_NAME) return picked
            val rootId = picked.resolve(DataFolder.FOLDER_NAME, createDirectories = create) ?: return null
            return SafDataFolderIo(resolver, tree, rootId)
        }
    }
}

object DataFolderFeature {
    private val work = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private val lock = Mutex()

    private val revisionFlow = MutableStateFlow(0)
    val revision: StateFlow<Int> get() = revisionFlow

    var status: String = ""
        private set
    var statusIsError: Boolean = false
        private set
    var working: Boolean = false
        private set
    var restorable: List<DataFolderProject> = emptyList()
        private set
    var needsRestart: Boolean = false
        private set
    var dismissedThisLaunch: Boolean = false
    private var reachable: Boolean? = null

    private val app: App get() = App.instance
    private val rawPrefs: Prefs get() = Prefs(app)
    private val prefs: PrefsStore get() = PrefsStore(rawPrefs)
    private val scope: ProjectScope get() = Services.get<ProjectScope>() ?: ProjectScope.legacy
    private val projectName: String get() = Services.get<ProjectsService>()?.active?.value?.label.orEmpty().ifEmpty { "ForkMesh" }
    val slug: String get() = DataFolder.projectFolder(scope)
    private val publicKey: String get() = Services.get<DeviceIdentity>()?.publicKeyB64url.orEmpty()

    private fun bump() {
        revisionFlow.value = revisionFlow.value + 1
    }

    private fun report(text: String, error: Boolean = false) {
        status = text
        statusIsError = error
        bump()
    }

    private val location: Uri? get() = rawPrefs.getString(DataFolder.LOCATION_KEY)?.takeIf { it.isNotEmpty() }?.let(Uri::parse)

    private fun hasPermission(uri: Uri): Boolean =
        app.contentResolver.persistedUriPermissions.any { it.uri == uri && it.isReadPermission && it.isWritePermission }

    private fun store(create: Boolean = false): DataFolderStore? {
        val uri = location ?: return null
        if (!hasPermission(uri)) return null
        return SafDataFolderIo.open(app, uri, create)?.let(::DataFolderStore)
    }

    fun folderName(): String = location?.lastPathSegment?.substringAfterLast(':')?.substringAfterLast('/').orEmpty()

    fun need(): DataFolderNeed? {
        val reach = reachable ?: (location?.let(::hasPermission) == true).also { reachable = it }
        return DataFolder.need(prefs, slug, publicKey, reach)
    }

    fun showsAlert(): Boolean = need() != null && !dismissedThisLaunch && !needsRestart

    fun summary(): String {
        val name = folderName()
        return when {
            location == null -> "Not set up: your data is deleted with the app"
            reachable == false -> "Can't open “$name”"
            else -> "Saved in “$name/${DataFolder.FOLDER_NAME}”"
        }
    }

    private fun trees(scope: ProjectScope): List<DataFolderTree> {
        val skipped = if (scope.isLegacy) setOf("projects") else emptySet()
        return listOf(
            DataFolderTree("documents", AppFiles.projectDocumentsDir(app, scope), skipped),
            DataFolderTree("support", AppFiles.projectSupportDir(app, scope), skipped),
        )
    }

    suspend fun choose(tree: Uri) {
        val resolver = app.contentResolver
        try {
            resolver.takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION)
        } catch (_: Exception) {
            report("ForkMesh can't keep access to that folder. Pick another one.", error = true)
            return
        }
        val previous = location
        rawPrefs.putString(DataFolder.LOCATION_KEY, tree.toString())
        if (previous != null && previous != tree) {
            runCatching { resolver.releasePersistableUriPermission(previous, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION) }
        }
        reachable = true
        val found = withContext(Dispatchers.IO) {
            val store = store(create = true) ?: return@withContext null
            val mine = slug
            val current = publicKey
            store.projects().filter { store.readKit(it.slug) != null && !(it.slug == mine && it.publicKey == current) }
        }
        if (found == null) {
            reachable = false
            report(DataFolder.RECONNECT_MESSAGE, error = true)
            return
        }
        restorable = found
        if (found.isEmpty()) {
            report("ForkMesh keeps a copy of your data in “${folderName()}/${DataFolder.FOLDER_NAME}”.")
            sync()
        } else {
            report("This folder holds ForkMesh data from an earlier install. Enter its passphrase to restore it.")
        }
    }

    suspend fun startFresh() {
        restorable = emptyList()
        report("ForkMesh keeps a copy of this install's data here too.")
        sync()
    }

    suspend fun saveIdentity(passphrase: String, confirm: String) {
        val identity = Services.get<DeviceIdentity>() ?: return report("This device has no identity key yet.", error = true)
        working = true
        bump()
        val failure = withContext(Dispatchers.Default) {
            val store = store(create = true) ?: return@withContext DataFolder.RECONNECT_MESSAGE
            try {
                DataFolderIdentity.saveKit(identity, passphrase, confirm, slug, store, prefs, System.currentTimeMillis())
                null
            } catch (error: Exception) {
                error.message ?: "The identity key was not saved."
            }
        }
        working = false
        if (failure != null) {
            report(failure, error = true)
        } else {
            report("Your identity key is saved, encrypted, in the data folder.")
            sync()
        }
    }

    suspend fun restore(passphrase: String) {
        working = true
        bump()
        val projects = restorable
        val outcome = withContext(Dispatchers.IO) {
            val store = store() ?: return@withContext null
            val secrets = SecureStore(app)
            val restored = projects.filter { project ->
                val kit = store.readKit(project.slug) ?: return@filter false
                runCatching { DataFolderIdentity.restoreSeed(kit, passphrase, project.namespace, secrets) }
                    .onSuccess { prefs.putString(DataFolder.kitKey(project.slug), it) }
                    .isSuccess
            }
            if (restored.isEmpty()) return@withContext Triple(0, 0, 0)
            store.readSettings(DataFolder.HOME_PROJECT)?.let { document ->
                for ((key, value) in DataFolder.decodeSettings(document)) rawPrefs.commitRaw(key, value)
            }
            var files = 0
            var failed = 0
            for (project in restored) {
                val copy = store.restore(project.slug, trees(ProjectScope(project.projectId, project.namespace)))
                files += copy.copied
                failed += copy.failed.size
            }
            Triple(restored.size, files, failed)
        }
        working = false
        when {
            outcome == null -> report(DataFolder.RECONNECT_MESSAGE, error = true)
            outcome.first == 0 -> report("The passphrase didn't open the identity key in this folder.", error = true)
            else -> {
                restorable = emptyList()
                needsRestart = true
                val projectsText = if (outcome.first == 1) "1 project" else "${outcome.first} projects"
                val failedText = if (outcome.third > 0) " ${outcome.third} files could not be copied." else ""
                report("Restored $projectsText and ${outcome.second} files.$failedText Restart ForkMesh to finish.", error = outcome.third > 0)
            }
        }
    }

    suspend fun sync() {
        if (needsRestart || restorable.isNotEmpty() || location == null) return
        val project = DataFolderProject(slug, scope.projectId, scope.namespace, projectName, publicKey)
        val trees = trees(scope)
        val failed = lock.withLock {
            withContext(Dispatchers.IO) {
                val store = store(create = true) ?: return@withContext null
                val now = System.currentTimeMillis()
                if (!store.prepare("android", now) || !store.prepareProject(project)) return@withContext null
                val raw = rawPrefs
                val values = raw.keys().associateWith { raw.raw(it) }
                store.writeSettings(DataFolder.exportSettings(values), DataFolder.HOME_PROJECT)
                val copy = store.backup(project.slug, trees)
                raw.putLong(DataFolder.SYNCED_AT_KEY, now)
                copy.failed.size
            }
        }
        when {
            failed == null -> {
                reachable = false
                report(DataFolder.RECONNECT_MESSAGE, error = true)
            }
            failed > 0 -> report("$failed files could not be copied to the data folder.", error = true)
            else -> {
                reachable = true
                bump()
            }
        }
    }

    fun syncSoon() {
        work.launch { runCatching { sync() } }
    }

    fun restart(context: Context) {
        val intent = context.packageManager.getLaunchIntentForPackage(context.packageName)
            ?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK or Intent.FLAG_ACTIVITY_CLEAR_TASK)
        if (intent != null) context.startActivity(intent)
        Runtime.getRuntime().exit(0)
    }
}
