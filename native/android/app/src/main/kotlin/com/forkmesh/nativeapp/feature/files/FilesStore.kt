package com.forkmesh.nativeapp.feature.files

import android.content.Context
import android.content.Intent
import android.net.Uri
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.feature.openwrt.OpenWrtState
import com.forkmesh.nativeapp.files.FileItem
import com.forkmesh.nativeapp.files.FileSort
import com.forkmesh.nativeapp.files.FileSource
import com.forkmesh.nativeapp.files.LocalFileSource
import com.forkmesh.nativeapp.files.SavedShare
import com.forkmesh.nativeapp.files.SmbClient
import com.forkmesh.nativeapp.files.SmbCredentials
import com.forkmesh.nativeapp.files.SmbFileSource
import com.forkmesh.nativeapp.files.SocketSmbTransport
import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.json.str
import com.forkmesh.nativeapp.platform.SecureStore
import java.io.File
import java.util.UUID

data class SavedFolder(val uri: String, val name: String)

data class FilesClip(val source: FileSource, val item: FileItem, val move: Boolean)

data class RouterShare(val host: String, val share: String, val guests: Boolean) {
    val title: String get() = "$share on router"
}

class FilesStore(private val app: App) {
    private val prefs = app.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
    private val secrets = SecureStore(app)
    private val open = mutableMapOf<String, FileSource>()

    var shares by mutableStateOf(loadShares())
        private set
    var folders by mutableStateOf(loadFolders())
        private set
    var clip by mutableStateOf<FilesClip?>(null)
    var sort by mutableStateOf(FileSort.entries.firstOrNull { it.name == prefs.getString("sort", "") } ?: FileSort.Name)
        private set
    var showHidden by mutableStateOf(prefs.getBoolean("showHidden", false))
        private set

    val phoneRoot: File get() = File(app.filesDir, "Files").also { it.mkdirs() }

    val phone: FileSource by lazy { LocalFileSource(phoneRoot, PHONE_TITLE) }

    val tempDir: File get() = File(app.cacheDir, "files-open").also { it.mkdirs() }

    init {
        val cutoff = System.currentTimeMillis() - 86_400_000L
        tempDir.walkBottomUp().filter { it != tempDir && it.lastModified() < cutoff }.forEach { it.delete() }
    }

    fun temp(): File = File.createTempFile("transfer", ".part", tempDir)

    fun updateSort(value: FileSort) {
        sort = value
        prefs.edit().putString("sort", value.name).apply()
    }

    fun updateShowHidden(value: Boolean) {
        showHidden = value
        prefs.edit().putBoolean("showHidden", value).apply()
    }

    private fun loadShares(): List<SavedShare> = runCatching {
        Json.parse(prefs.getString("shares", "[]") ?: "[]") as? List<*>
    }.getOrNull().orEmpty().mapNotNull { (it as? Map<*, *>)?.let { map -> SavedShare.fromJson(map.entries.associate { e -> e.key.toString() to e.value }) } }

    private fun loadFolders(): List<SavedFolder> = runCatching {
        Json.parse(prefs.getString("folders", "[]") ?: "[]") as? List<*>
    }.getOrNull().orEmpty().mapNotNull { entry ->
        val map = (entry as? Map<*, *>)?.entries?.associate { it.key.toString() to it.value }
        val uri = map.str("uri")
        if (uri.isEmpty()) null else SavedFolder(uri, map.str("name").ifEmpty { "Folder" })
    }

    private fun saveShares(next: List<SavedShare>) {
        shares = next
        prefs.edit().putString("shares", Json.stringify(next.map { it.toJson() })).apply()
    }

    private fun saveFolders(next: List<SavedFolder>) {
        folders = next
        prefs.edit().putString("folders", Json.stringify(next.map { mapOf("uri" to it.uri, "name" to it.name) })).apply()
    }

    fun addFolder(uri: Uri, name: String): SavedFolder {
        runCatching { app.contentResolver.takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION) }
        val folder = SavedFolder(uri.toString(), name)
        saveFolders(folders.filter { it.uri != folder.uri } + folder)
        return folder
    }

    fun removeFolder(folder: SavedFolder) {
        runCatching { app.contentResolver.releasePersistableUriPermission(Uri.parse(folder.uri), Intent.FLAG_GRANT_READ_URI_PERMISSION or Intent.FLAG_GRANT_WRITE_URI_PERMISSION) }
        open.remove("folder:${folder.uri}")
        saveFolders(folders.filter { it.uri != folder.uri })
    }

    fun folderSource(folder: SavedFolder): FileSource =
        open.getOrPut("folder:${folder.uri}") { SafFileSource(app.contentResolver, Uri.parse(folder.uri), folder.name) }

    fun newShare(host: String, share: String, user: String, guest: Boolean, remember: Boolean, port: Int): SavedShare {
        val existing = shares.firstOrNull { it.host.equals(host, true) && it.share.equals(share, true) && it.port == port }
        return SavedShare(existing?.id ?: UUID.randomUUID().toString(), host, share, if (guest) "" else user.trim(), guest, remember && !guest, port)
    }

    fun saveShare(share: SavedShare, password: String) {
        saveShares(shares.filter { it.id != share.id } + share)
        if (share.remember && !share.guest) secrets.write(secretKey(share), password) else secrets.delete(secretKey(share))
    }

    fun removeShare(share: SavedShare) {
        secrets.delete(secretKey(share))
        open.remove("share:${share.id}")?.let { runCatching { it.close() } }
        saveShares(shares.filter { it.id != share.id })
    }

    fun rememberedPassword(share: SavedShare): String? = if (share.remember) secrets.read(secretKey(share)) else null

    fun needsPassword(share: SavedShare): Boolean = !share.guest && open["share:${share.id}"] == null && rememberedPassword(share) == null

    fun shareSource(share: SavedShare, password: String? = null): FileSource {
        open["share:${share.id}"]?.let { return it }
        val credentials = if (share.guest) SmbCredentials.guest else SmbCredentials.parse(share.user, password ?: rememberedPassword(share) ?: "")
        val client = SmbClient(share.host, share.share, credentials, share.guest) { SocketSmbTransport(share.host, share.port) }
        val source = SmbFileSource(client, share.title)
        open["share:${share.id}"] = source
        return source
    }

    fun forget(share: SavedShare) {
        open.remove("share:${share.id}")?.let { runCatching { it.close() } }
    }

    fun findShares(host: String, port: Int, user: String, password: String, guest: Boolean): List<String> {
        val credentials = if (guest) SmbCredentials.guest else SmbCredentials.parse(user, password)
        return SmbClient.listShares(host, credentials, guest, SocketSmbTransport(host, port))
    }

    val routerShares: List<RouterShare>
        get() {
            val router = OpenWrtState.shared
            if (!router.signedIn || router.routerHost.isEmpty()) return emptyList()
            return router.snapshot.shares
                .filter { found -> shares.none { it.host.equals(router.routerHost, true) && it.share.equals(found.name, true) } }
                .map { RouterShare(router.routerHost, it.name, it.guests) }
        }

    private fun secretKey(share: SavedShare) = "files.smb.${share.id}"

    companion object {
        const val PREFS = "market.packages.files"
        const val PHONE_TITLE = "On this phone"
    }
}
