package com.forkmesh.nativeapp.feature.agentwatch

import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import android.os.Build
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import com.forkmesh.nativeapp.agentwatch.AgentWatchOutput
import com.forkmesh.nativeapp.agentwatch.Note
import com.forkmesh.nativeapp.agentwatch.Pulse
import com.forkmesh.nativeapp.agentwatch.kWatchSampleRate
import com.forkmesh.nativeapp.agentwatch.renderNotes
import com.forkmesh.nativeapp.platform.OtherAppCall
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicInteger
import java.util.concurrent.locks.ReentrantLock
import kotlin.concurrent.withLock
import kotlin.math.max
import kotlin.math.min

internal class WatchMixer {
    private class Voice(val pcm: ShortArray, var at: Int, val generation: Int)

    private val lock = ReentrantLock()
    private val wake = lock.newCondition()
    private val voices = ArrayList<Voice>()
    private var thread: Thread? = null
    private var running = false
    private val chunkFrames = 1024

    fun add(pcm: ShortArray, generation: Int) {
        if (pcm.isEmpty()) return
        lock.withLock {
            voices += Voice(pcm, 0, generation)
            if (voices.size > 24) voices.removeAt(0)
            if (thread == null) start()
            wake.signalAll()
        }
    }

    fun clear() {
        lock.withLock { voices.clear() }
    }

    fun release() {
        lock.withLock {
            running = false
            voices.clear()
            wake.signalAll()
        }
        thread = null
    }

    private fun start() {
        running = true
        thread = Thread({ loop() }, "AgentWatchMixer").apply {
            isDaemon = true
            priority = Thread.MAX_PRIORITY
            start()
        }
    }

    private fun loop() {
        val minBytes = AudioTrack.getMinBufferSize(kWatchSampleRate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        val track = AudioTrack.Builder()
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_MUSIC)
                    .build(),
            )
            .setAudioFormat(
                AudioFormat.Builder()
                    .setSampleRate(kWatchSampleRate)
                    .setChannelMask(AudioFormat.CHANNEL_OUT_STEREO)
                    .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                    .build(),
            )
            .setBufferSizeInBytes(max(minBytes, chunkFrames * 4 * 2))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .build()
        val mix = IntArray(chunkFrames * 2)
        val out = ShortArray(chunkFrames * 2)
        var playing = false
        try {
            while (true) {
                lock.withLock {
                    while (running && voices.isEmpty()) {
                        if (playing) {
                            track.pause()
                            track.flush()
                            playing = false
                        }
                        wake.await()
                    }
                    if (!running) return
                    mix.fill(0)
                    val iterator = voices.iterator()
                    while (iterator.hasNext()) {
                        val voice = iterator.next()
                        val count = min(mix.size, voice.pcm.size - voice.at)
                        for (i in 0 until count) mix[i] += voice.pcm[voice.at + i].toInt()
                        voice.at += count
                        if (voice.at >= voice.pcm.size) iterator.remove()
                    }
                }
                for (i in mix.indices) out[i] = mix[i].coerceIn(-32768, 32767).toShort()
                if (!playing) {
                    track.play()
                    playing = true
                }
                track.write(out, 0, out.size)
            }
        } catch (_: InterruptedException) {
        } finally {
            runCatching { track.stop() }
            track.release()
        }
    }
}

internal class AndroidAgentWatchOutput(context: Context) : AgentWatchOutput {
    private val mixer = WatchMixer()
    private val renderer = Executors.newSingleThreadExecutor { Thread(it, "AgentWatchRender").apply { isDaemon = true } }
    private val generation = AtomicInteger()
    private val vibrator: Vibrator? = if (Build.VERSION.SDK_INT >= 31) {
        (context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE) as? VibratorManager)?.defaultVibrator
    } else {
        @Suppress("DEPRECATION")
        context.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
    }

    override fun play(notes: List<Note>, now: Double) {
        if (OtherAppCall.inProgress) return
        val wanted = generation.get()
        val asked = System.nanoTime()
        renderer.execute {
            val late = (System.nanoTime() - asked) / 1e9
            val pcm = renderNotes(notes, now + late)
            if (generation.get() == wanted) mixer.add(pcm, wanted)
        }
    }

    override fun feel(pulses: List<Pulse>) {
        val motor = vibrator ?: return
        if (!motor.hasVibrator() || pulses.isEmpty()) return
        val timings = ArrayList<Long>()
        val amplitudes = ArrayList<Int>()
        var cursor = 0L
        for (raw in pulses.sortedBy { it.atMs }) {
            val pulse = raw.clamped
            val gap = pulse.atMs - cursor
            if (gap > 0) {
                timings += gap
                amplitudes += 0
            }
            val length = max(1, pulse.ms).toLong()
            timings += length
            amplitudes += max(1, (pulse.intensity * 255).toInt())
            cursor = pulse.atMs + length
        }
        val effect = if (motor.hasAmplitudeControl()) {
            VibrationEffect.createWaveform(timings.toLongArray(), amplitudes.toIntArray(), -1)
        } else {
            val pattern = arrayListOf(0L)
            timings.forEachIndexed { i, t ->
                val on = amplitudes[i] > 0
                val lastIsOn = pattern.size % 2 == 0
                if (on == lastIsOn) pattern[pattern.size - 1] = pattern.last() + t else pattern += t
            }
            VibrationEffect.createWaveform(pattern.toLongArray(), -1)
        }
        runCatching {
            motor.cancel()
            motor.vibrate(effect)
        }
    }

    override fun stop() {
        generation.incrementAndGet()
        mixer.clear()
        runCatching { vibrator?.cancel() }
    }

    fun release() {
        stop()
        mixer.release()
        renderer.shutdownNow()
    }
}
