package com.forkmesh.nativeapp.feature.poker

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.poker.PokerSound
import com.forkmesh.nativeapp.poker.PokerSynth
import java.util.EnumMap
import java.util.concurrent.Executors

internal class PokerSounds {
    private val worker = Executors.newSingleThreadExecutor { Thread(it, "poker-sound").apply { isDaemon = true } }
    private val synth = PokerSynth()
    private val pcm = EnumMap<PokerSound, ShortArray>(PokerSound::class.java)
    private val tracks = EnumMap<PokerSound, AudioTrack>(PokerSound::class.java)
    @Volatile
    private var released = false

    var muted: Boolean = Services.get<PrefStore>()?.getBool(MUTED_KEY) ?: false
        set(value) {
            field = value
            Services.get<PrefStore>()?.putBool(MUTED_KEY, value)
            if (value) worker.execute { tracks.values.forEach { runCatching { it.pause(); it.flush() } } }
        }

    fun warm() {
        worker.execute { PokerSound.entries.forEach(::samples) }
    }

    fun play(sound: PokerSound) {
        if (muted || released) return
        worker.execute {
            if (released) return@execute
            runCatching {
                val track = track(sound) ?: return@runCatching
                if (track.playState == AudioTrack.PLAYSTATE_PLAYING) track.stop()
                track.reloadStaticData()
                track.play()
            }
        }
    }

    fun release() {
        released = true
        worker.execute {
            tracks.values.forEach { runCatching { it.release() } }
            tracks.clear()
        }
        worker.shutdown()
    }

    private fun samples(sound: PokerSound): ShortArray = pcm.getOrPut(sound) { synth.render(sound) }

    private fun track(sound: PokerSound): AudioTrack? {
        tracks[sound]?.let { return it }
        val data = samples(sound)
        val bytes = data.size * 2
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_GAME)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .setSampleRate(synth.rate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                    .build(),
            )
            .setTransferMode(AudioTrack.MODE_STATIC)
            .setBufferSizeInBytes(bytes)
            .build()
        if (track.state == AudioTrack.STATE_UNINITIALIZED) {
            track.release()
            return null
        }
        track.write(data, 0, data.size)
        tracks[sound] = track
        return track
    }

    companion object {
        const val MUTED_KEY = "poker/muted"
    }
}

internal object PokerHaptics {
    fun turn(context: Context) {
        val vibrator = if (Build.VERSION.SDK_INT >= 31) {
            context.getSystemService(VibratorManager::class.java)?.defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Vibrator::class.java)
        } ?: return
        runCatching {
            val effect = if (Build.VERSION.SDK_INT >= 29) {
                VibrationEffect.createPredefined(VibrationEffect.EFFECT_DOUBLE_CLICK)
            } else {
                VibrationEffect.createOneShot(40, VibrationEffect.DEFAULT_AMPLITUDE)
            }
            vibrator.vibrate(effect)
        }
    }
}
