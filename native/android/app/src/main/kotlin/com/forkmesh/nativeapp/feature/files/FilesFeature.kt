package com.forkmesh.nativeapp.feature.files

import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.feature.navigation.NavigationFeature
import com.forkmesh.nativeapp.navigation.Route
import com.forkmesh.nativeapp.ui.navigation.Screen

object FilesFeature {
    private const val OWNER = "existing"

    @Volatile
    private var instance: FilesStore? = null

    internal val store: FilesStore get() = instance ?: error("Files is not installed")

    fun install(app: App) {
        if (instance == null) instance = FilesStore(app)
        NavigationFeature.register(Route.Kind.Files, OWNER) { FilesHomeScreen() }
    }

    fun browseShare(host: String, share: String, guests: Boolean): Screen {
        val saved = store.shares.firstOrNull { it.host.equals(host, true) && it.share.equals(share, true) }
        if (saved != null && !store.needsPassword(saved)) return FolderScreen(store.shareSource(saved), "", saved)
        if (guests) {
            val guest = store.newShare(host, share, "", guest = true, remember = false, port = 445)
            store.saveShare(guest, "")
            return FolderScreen(store.shareSource(guest), "", guest)
        }
        return AddShareScreen(saved?.host ?: host, saved?.share ?: share, saved)
    }
}
