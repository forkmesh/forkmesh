package com.forkmesh.nativeapp.feature.account.settings

import android.os.Build
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.feature.rssreader.RssReaderFeature
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.Prefs
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import com.forkmesh.nativeapp.rssreader.RssLibraryData
import com.forkmesh.nativeapp.settings.AccountEndpointCalling
import com.forkmesh.nativeapp.settings.AccountEndpointException
import com.forkmesh.nativeapp.settings.AccountSyncKeys
import com.forkmesh.nativeapp.settings.AccountSyncPrefs
import com.forkmesh.nativeapp.settings.AccountSyncService
import com.forkmesh.nativeapp.settings.AccountSyncSource
import com.forkmesh.nativeapp.settings.PreferenceSyncSource
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext

internal class PrefsSyncAdapter(private val prefs: Prefs) : AccountSyncPrefs {
    override fun keys(): Set<String> = prefs.keys()

    override fun raw(key: String): Any? = prefs.raw(key)

    override fun putRaw(key: String, value: Any?): Boolean = prefs.commitRaw(key, value)
}

internal class RssReaderSyncSource : AccountSyncSource {
    override val key = AccountSyncKeys.RSS
    override val kind = "subscriptions"
    override val label = "News reader subscriptions"

    private fun library() = runCatching { RssReaderFeature.service }.getOrNull()?.let { service ->
        service.library ?: runCatching { runBlocking { service.load() } }.getOrNull()
    }

    override fun snapshot(): Map<String, Any?>? {
        val data = library()?.syncData() ?: return null
        if (data.feeds.isEmpty() && data.folders.isEmpty()) return null
        return data.toJson()
    }

    override fun restore(value: Map<String, Any?>): Int {
        val service = runCatching { RssReaderFeature.service }.getOrNull() ?: return 0
        val library = library() ?: return 0
        val count = library.restoreSync(RssLibraryData.fromJson(value), System.currentTimeMillis())
        service.changed()
        return count
    }
}

internal object LazyAccountEndpoints : AccountEndpointCalling {
    override val serverOrigin: String get() = SettingsArea.endpoints?.serverOrigin.orEmpty()

    override fun accountCall(method: String, path: String, body: Map<String, Any?>?): Map<String, Any?> =
        (SettingsArea.endpoints ?: throw AccountEndpointException("offline")).accountCall(method, path, body)
}

internal object AccountSyncSetup {
    fun install(app: App) {
        val auth = Services.get<AuthService>()
        val prefs = PrefsSyncAdapter(Prefs(app))
        val service = AccountSyncService(
            client = LazyAccountEndpoints,
            prefs = prefs,
            sources = emptyList(),
            device = listOf(Build.MANUFACTURER, Build.MODEL).filter { it.isNotBlank() }.joinToString(" ").take(60),
        ) { if (auth?.isGuest?.value == true) "" else auth?.session?.value?.nodeName.orEmpty() }
        service.register(PreferenceSyncSource.home(prefs))
        service.register(RssReaderSyncSource())
        service.register(PreferenceSyncSource.settings(prefs))
        Services.put(service)
        ProjectLifecycle.scope.launch {
            AppLifecycle.foreground.drop(1).collect {
                if (service.enabled) withContext(Dispatchers.IO) { runCatching { service.saveIfChanged() } }
            }
        }
    }
}
