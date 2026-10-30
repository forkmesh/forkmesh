package com.forkmesh.nativeapp.platform

import android.content.Context
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.forkmesh.nativeapp.net.OtherAppCallState
import java.util.concurrent.CopyOnWriteArrayList
import kotlinx.coroutines.launch

object OtherAppCall {
    private const val POLL_MS = 2_000L

    private val main = Handler(Looper.getMainLooper())
    private val state = OtherAppCallState()
    private val listeners = CopyOnWriteArrayList<(Boolean) -> Unit>()
    private var audio: AudioManager? = null
    private var installed = false
    private var polling = false

    @Volatile
    var ownCall: () -> Boolean = { false }

    val inProgress: Boolean
        get() {
            refresh()
            return synchronized(state) { state.active }
        }

    fun install(context: Context) {
        if (installed) return
        installed = true
        val manager = context.applicationContext.getSystemService(AudioManager::class.java) ?: return
        audio = manager
        if (Build.VERSION.SDK_INT >= 31) {
            runCatching { manager.addOnModeChangedListener({ main.post(it) }) { refresh() } }
        } else {
            AppLifecycle.scope.launch { AppLifecycle.foreground.collect { shown -> poll(shown) } }
        }
        refresh()
    }

    fun onChange(listener: (Boolean) -> Unit) {
        listeners += listener
        listener(synchronized(state) { state.active })
    }

    fun refresh() {
        val mode = runCatching { audio?.mode }.getOrNull() ?: return
        val own = runCatching { ownCall() }.getOrDefault(false)
        val now = synchronized(state) { if (state.update(mode, own)) state.active else null } ?: return
        main.post { listeners.forEach { it(now) } }
    }

    private val poller = object : Runnable {
        override fun run() {
            if (!polling) return
            refresh()
            main.postDelayed(this, POLL_MS)
        }
    }

    private fun poll(on: Boolean) {
        if (on == polling) return
        polling = on
        main.removeCallbacks(poller)
        if (on) main.post(poller)
    }
}
