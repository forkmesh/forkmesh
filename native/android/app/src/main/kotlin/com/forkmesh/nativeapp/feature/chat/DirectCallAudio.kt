package com.forkmesh.nativeapp.feature.chat

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioDeviceInfo
import android.media.AudioFocusRequest
import android.media.AudioManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import com.forkmesh.nativeapp.chat.DirectCallEngine
import com.forkmesh.nativeapp.chat.DirectCallKind
import com.forkmesh.nativeapp.chat.DirectCallPhase
import com.forkmesh.nativeapp.feature.facechanger.callHeadphonesConnected
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.OtherAppCall
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch

object CallSpeaker {
    private var holders = 0
    private var forced = false

    fun hold(context: Context): Boolean {
        val audio = context.getSystemService(AudioManager::class.java) ?: return false
        if (callHeadphonesConnected(context)) return false
        holders += 1
        if (!forced) forced = runCatching { route(audio, true) }.getOrDefault(false)
        return true
    }

    fun release(context: Context) {
        if (holders == 0) return
        holders -= 1
        if (holders > 0 || !forced) return
        forced = false
        val audio = context.getSystemService(AudioManager::class.java) ?: return
        runCatching { route(audio, false) }
    }

    private fun route(audio: AudioManager, on: Boolean): Boolean {
        if (Build.VERSION.SDK_INT >= 31) {
            if (!on) {
                audio.clearCommunicationDevice()
                return true
            }
            val speaker = audio.availableCommunicationDevices.firstOrNull { it.type == AudioDeviceInfo.TYPE_BUILTIN_SPEAKER }
                ?: return false
            return audio.setCommunicationDevice(speaker)
        }
        @Suppress("DEPRECATION")
        audio.isSpeakerphoneOn = on
        return true
    }
}

object DirectCallAudio {
    private val main = Handler(Looper.getMainLooper())
    private var context: Context? = null
    private var engine: DirectCallEngine? = null
    private var listener: (() -> Unit)? = null
    private var watch: Job? = null
    private var focus: AudioFocusRequest? = null
    private var interrupted = false
    private var speaker = false
    private var phase: DirectCallPhase? = null

    fun attach(context: Context, made: DirectCallEngine) {
        detach()
        val app = context.applicationContext
        this.context = app
        engine = made
        val heard: () -> Unit = { main.post { refresh() } }
        listener = heard
        made.addListener(heard)
        watch = AppLifecycle.scope.launch { AppLifecycle.foreground.collect { shown -> if (shown) resume() } }
        main.post { refresh() }
    }

    fun detach() {
        val made = engine
        listener?.let { made?.removeListener(it) }
        listener = null
        watch?.cancel()
        watch = null
        release()
        engine = null
        phase = null
    }

    private fun refresh() {
        val made = engine ?: return
        val now = made.phase
        val was = phase
        phase = now
        OtherAppCall.refresh()
        if (now != null && focus == null) hold()
        if (now == null) {
            release()
            if (was != null) CallDiagnosticsBook.finish()
            return
        }
        if (now == DirectCallPhase.LIVE && was != DirectCallPhase.LIVE) route(made)
    }

    private fun hold() {
        val audio = context?.getSystemService(AudioManager::class.java) ?: return
        val request = AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_VOICE_COMMUNICATION)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH)
                    .build(),
            )
            .setAcceptsDelayedFocusGain(true)
            .setOnAudioFocusChangeListener({ change -> focusChanged(change) }, main)
            .build()
        focus = request
        interrupted = false
        runCatching { audio.requestAudioFocus(request) }
    }

    private fun focusChanged(change: Int) {
        when (change) {
            AudioManager.AUDIOFOCUS_GAIN -> if (interrupted) {
                interrupted = false
                resume()
            }
            AudioManager.AUDIOFOCUS_LOSS, AudioManager.AUDIOFOCUS_LOSS_TRANSIENT,
            AudioManager.AUDIOFOCUS_LOSS_TRANSIENT_CAN_DUCK -> interrupted = true
        }
    }

    private fun resume() {
        val made = engine ?: return
        if (!made.active) return
        if (made.phase == DirectCallPhase.LIVE) route(made)
        DirectCallFeature.run { if (it === made) it.resumeMedia() }
    }

    private fun route(made: DirectCallEngine) {
        val app = context ?: return
        if (made.media != DirectCallKind.VIDEO || speaker) return
        speaker = CallSpeaker.hold(app)
    }

    private fun release() {
        val app = context
        val audio = app?.getSystemService(AudioManager::class.java)
        focus?.let { request -> runCatching { audio?.abandonAudioFocusRequest(request) } }
        focus = null
        interrupted = false
        if (speaker && app != null) CallSpeaker.release(app)
        speaker = false
    }
}
