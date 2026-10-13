package com.forkmesh.nativeapp.ui.kit

import android.os.SystemClock
import android.view.KeyEvent
import com.forkmesh.nativeapp.api.ApiService
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.feature.account.AuthService
import com.forkmesh.nativeapp.insights.UserActivityLog
import com.forkmesh.nativeapp.insights.UserActivitySync
import com.forkmesh.nativeapp.platform.AppLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

object FmUserActivity {
    private const val KEY_ECHO_MS = 150L
    const val KIND = "android"

    val log: UserActivityLog by lazy { UserActivityLog(runCatching { Services.get<PrefStore>() }.getOrNull()) }

    private val sync: UserActivitySync by lazy {
        UserActivitySync(log, KIND, { payload -> Services.require<ApiService>().syncActivityToday(payload) })
    }

    private var lastKeyMs = Long.MIN_VALUE / 2
    private var syncing = false

    fun touchDown(uptimeMs: Long) = log.touchDown(uptimeMs)

    fun key(event: KeyEvent) {
        if (event.action != KeyEvent.ACTION_DOWN || event.repeatCount != 0) return
        lastKeyMs = SystemClock.uptimeMillis()
        log.record()
    }

    fun typed() {
        if (SystemClock.uptimeMillis() - lastKeyMs < KEY_ECHO_MS) return
        log.record()
    }

    fun <T> typing(change: (T) -> Unit): (T) -> Unit = { value ->
        typed()
        change(value)
    }

    fun flush() = log.flush()

    fun startSync() {
        if (syncing) return
        syncing = true
        AppLifecycle.scope.launch {
            AppLifecycle.foreground.collectLatest { foreground ->
                if (!foreground) {
                    syncNow(force = false)
                    return@collectLatest
                }
                while (true) {
                    syncNow(force = false)
                    delay(UserActivitySync.INTERVAL_MS)
                }
            }
        }
    }

    suspend fun syncNow(force: Boolean): Boolean = withContext(Dispatchers.IO) {
        runCatching { sync.sync(account(), force) }.getOrDefault(false)
    }

    private fun account(): String {
        val session = Services.get<AuthService>()?.session?.value ?: return ""
        return if (session.sessionKind == "preview") "" else session.nodeName
    }
}
