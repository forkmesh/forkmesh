package com.forkmesh.nativeapp.feature.chat

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.MediaPlayer
import android.media.RingtoneManager
import android.media.ToneGenerator
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import com.forkmesh.nativeapp.chat.DirectCallEngine
import com.forkmesh.nativeapp.chat.DirectCallInvite
import com.forkmesh.nativeapp.chat.DirectCallPhase
import com.forkmesh.nativeapp.platform.AppLifecycle
import com.forkmesh.nativeapp.platform.ProjectLifecycle
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

data class CallWindow(val showWhenLocked: Boolean = false, val turnScreenOn: Boolean = false)

internal class CallRinger(private val context: Context, private val engine: DirectCallEngine) {
    companion object {
        const val RINGBACK_VOLUME = 80
        private val PATTERN = longArrayOf(0, 1000, 1000)
        private val AMPLITUDES = intArrayOf(0, 255, 0)
        private val windowState = MutableStateFlow(CallWindow())

        val window: StateFlow<CallWindow> = windowState.asStateFlow()
    }

    private val main = Handler(Looper.getMainLooper())
    private val audio = context.getSystemService(AudioManager::class.java)
    private val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= 31) {
        context.getSystemService(VibratorManager::class.java)?.defaultVibrator
    } else {
        @Suppress("DEPRECATION")
        context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
    }
    private val listener: () -> Unit = { main.post { sync() } }
    private var tone: ToneGenerator? = null
    private var ringingBack = false
    private var ringingCall = ""
    private var player: MediaPlayer? = null
    private var vibrating = false
    private var notifiedCall = ""
    private var watch: Job? = null

    @Volatile
    private var released = false

    fun start() {
        engine.addListener(listener)
        watch = ProjectLifecycle.scope.launch { AppLifecycle.foreground.collect { sync() } }
        main.post { sync() }
    }

    fun release() {
        if (released) return
        released = true
        engine.removeListener(listener)
        watch?.cancel()
        watch = null
        main.post {
            stopRingback()
            stopIncoming()
            runCatching { tone?.release() }
            tone = null
            windowState.value = CallWindow()
        }
    }

    private fun sync() {
        if (released) return
        val phase = engine.phase
        if (phase == DirectCallPhase.RINGING) startRingback() else stopRingback()
        val incoming = if (phase == null) engine.invite else null
        if (incoming == null) stopIncoming() else ringIncoming(incoming)
        windowState.value = CallWindow(showWhenLocked = incoming != null || phase != null, turnScreenOn = incoming != null)
    }

    private fun startRingback() {
        if (ringingBack) return
        val generator = tone ?: runCatching { ToneGenerator(AudioManager.STREAM_VOICE_CALL, RINGBACK_VOLUME) }.getOrNull() ?: return
        tone = generator
        ringingBack = runCatching { generator.startTone(ToneGenerator.TONE_SUP_RINGTONE) }.getOrDefault(false)
    }

    private fun stopRingback() {
        if (!ringingBack) return
        ringingBack = false
        runCatching { tone?.stopTone() }
    }

    private fun ringIncoming(invite: DirectCallInvite) {
        if (ringingCall != invite.callId) {
            stopIncoming()
            ringingCall = invite.callId
            when (audio?.ringerMode ?: AudioManager.RINGER_MODE_NORMAL) {
                AudioManager.RINGER_MODE_SILENT -> Unit
                AudioManager.RINGER_MODE_VIBRATE -> vibrate()
                else -> {
                    playRingtone()
                    vibrate()
                }
            }
        }
        if (AppLifecycle.foreground.value) {
            if (notifiedCall.isNotEmpty()) {
                IncomingCallNotifier.cancel(context)
                notifiedCall = ""
            }
        } else if (notifiedCall != invite.callId) {
            notifiedCall = invite.callId
            IncomingCallNotifier.post(context, invite)
        }
    }

    private fun stopIncoming() {
        if (ringingCall.isEmpty() && notifiedCall.isEmpty() && player == null && !vibrating) return
        ringingCall = ""
        player?.let { runCatching { it.stop() } }
        player?.let { runCatching { it.release() } }
        player = null
        if (vibrating) runCatching { vibrator?.cancel() }
        vibrating = false
        if (notifiedCall.isNotEmpty()) IncomingCallNotifier.cancel(context)
        notifiedCall = ""
    }

    private fun playRingtone() {
        val uri = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_RINGTONE) ?: return
        val made = MediaPlayer()
        val started = runCatching {
            made.setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build(),
            )
            made.setDataSource(context, uri)
            made.isLooping = true
            made.setOnPreparedListener { prepared -> if (player === prepared) runCatching { prepared.start() } }
            made.setOnErrorListener { _, _, _ -> true }
            made.prepareAsync()
        }.isSuccess
        if (started) player = made else runCatching { made.release() }
    }

    private fun vibrate() {
        val motor = vibrator ?: return
        if (!motor.hasVibrator()) return
        val effect = if (motor.hasAmplitudeControl()) {
            VibrationEffect.createWaveform(PATTERN, AMPLITUDES, 0)
        } else {
            VibrationEffect.createWaveform(PATTERN, 0)
        }
        vibrating = runCatching {
            if (Build.VERSION.SDK_INT >= 33) {
                motor.vibrate(effect, VibrationAttributes.createForUsage(VibrationAttributes.USAGE_RINGTONE))
            } else {
                @Suppress("DEPRECATION")
                motor.vibrate(effect, AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE).build())
            }
        }.isSuccess
    }
}
