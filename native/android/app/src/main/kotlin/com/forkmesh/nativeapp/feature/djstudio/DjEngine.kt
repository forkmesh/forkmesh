package com.forkmesh.nativeapp.feature.djstudio

import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioTrack
import com.forkmesh.nativeapp.djstudio.DjDeckSide
import com.forkmesh.nativeapp.djstudio.DjDrumPad
import com.forkmesh.nativeapp.djstudio.DjDrumSynth
import com.forkmesh.nativeapp.djstudio.DjScratchAudio
import com.forkmesh.nativeapp.djstudio.DjScratchHead
import java.util.concurrent.ConcurrentLinkedQueue
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min

internal class DjVoice(val side: DjDeckSide) {
    @Volatile var pcm: DjPcm? = null
    @Volatile var scratchAudio: DjScratchAudio? = null
    @Volatile var playing = false
    @Volatile var rate = 1.0
    @Volatile var gain = 1f
    @Volatile var scratching = false
    @Volatile var scratchGoal = 0.0
    @Volatile var position = 0.0
    @Volatile var buffering = false
    @Volatile var ended = false
    @Volatile var audible = false
    @Volatile var level = 0f
    @Volatile internal var seekTo = Double.NaN
    @Volatile internal var scratchStart = false
    @Volatile internal var scratchStop = false
    internal var frame = 0.0
    internal var shownGain = 0f
    internal val head = DjScratchHead()

    fun seek(seconds: Double) {
        seekTo = max(0.0, seconds)
        position = seekTo
        ended = false
    }

    fun beginScratch(at: Double) {
        scratchGoal = at
        scratchStart = true
        scratching = true
    }

    fun endScratch() {
        scratchStop = true
        scratching = false
    }

    val duration: Double get() = pcm?.seconds ?: 0.0
}

internal class DjEngine {
    companion object {
        const val BLOCK = 512
    }

    val a = DjVoice(DjDeckSide.A)
    val b = DjVoice(DjDeckSide.B)
    @Volatile var samplerLevel = 0.8f
    private val pads = ConcurrentLinkedQueue<FloatArray>()
    private val lock = Object()
    @Volatile private var running = false
    private var track: AudioTrack? = null
    private val outRate = 44_100
    private val padCache = HashMap<DjDrumPad, FloatArray>()

    fun voice(side: DjDeckSide): DjVoice = if (side == DjDeckSide.A) a else b

    fun pad(pad: DjDrumPad) {
        val samples = synchronized(padCache) { padCache.getOrPut(pad) { DjDrumSynth.samples(pad, outRate.toDouble()) } }
        pads.add(samples)
        wake()
    }

    fun wake() {
        if (!running) start()
        synchronized(lock) { lock.notifyAll() }
    }

    private fun busy(): Boolean = a.playing || b.playing || a.scratching || b.scratching || a.scratchStop || b.scratchStop || pads.isNotEmpty() || active.isNotEmpty()

    private val active = ArrayList<IntArrayCursor>()

    private class IntArrayCursor(val samples: FloatArray) {
        var at = 0
    }

    @Synchronized
    private fun start() {
        if (running) return
        running = true
        Thread({ loop() }, "dj-engine").apply { priority = Thread.MAX_PRIORITY }.start()
    }

    fun shutdown() {
        running = false
        wake()
    }

    private fun open(): AudioTrack {
        val minimum = AudioTrack.getMinBufferSize(outRate, AudioFormat.CHANNEL_OUT_STEREO, AudioFormat.ENCODING_PCM_16BIT)
        return AudioTrack.Builder()
            .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_MUSIC).build())
            .setAudioFormat(AudioFormat.Builder().setSampleRate(outRate).setEncoding(AudioFormat.ENCODING_PCM_16BIT).setChannelMask(AudioFormat.CHANNEL_OUT_STEREO).build())
            .setBufferSizeInBytes(max(minimum, BLOCK * 4 * 4))
            .setTransferMode(AudioTrack.MODE_STREAM)
            .setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY)
            .build()
    }

    private fun loop() {
        val left = FloatArray(BLOCK)
        val right = FloatArray(BLOCK)
        val scratch = FloatArray(BLOCK)
        val out = ShortArray(BLOCK * 2)
        var idle = 0
        while (running) {
            if (!busy()) {
                idle += 1
                if (idle > 40) {
                    track?.pause()
                    track?.flush()
                    synchronized(lock) { if (!busy() && running) lock.wait(500) }
                    continue
                }
            } else {
                idle = 0
            }
            val current = track ?: open().also { track = it }
            if (current.playState != AudioTrack.PLAYSTATE_PLAYING) current.play()
            left.fill(0f)
            right.fill(0f)
            render(a, left, right, scratch)
            render(b, left, right, scratch)
            mixPads(left, right)
            for (index in 0 until BLOCK) {
                out[index * 2] = clip(left[index])
                out[index * 2 + 1] = clip(right[index])
            }
            current.write(out, 0, out.size, AudioTrack.WRITE_BLOCKING)
        }
        track?.run {
            runCatching { stop() }
            release()
        }
        track = null
    }

    private fun clip(value: Float): Short {
        val soft = if (value > 0.9f) 0.9f + (value - 0.9f) / (1 + (value - 0.9f) * 10) else if (value < -0.9f) -0.9f + (value + 0.9f) / (1 - (value + 0.9f) * 10) else value
        return (max(-1f, min(1f, soft)) * 32767).toInt().toShort()
    }

    private fun mixPads(left: FloatArray, right: FloatArray) {
        while (true) active.add(IntArrayCursor(pads.poll() ?: break))
        if (active.isEmpty()) return
        val level = samplerLevel
        val iterator = active.iterator()
        while (iterator.hasNext()) {
            val cursor = iterator.next()
            val count = min(BLOCK, cursor.samples.size - cursor.at)
            for (index in 0 until count) {
                val value = cursor.samples[cursor.at + index] * level
                left[index] += value
                right[index] += value
            }
            cursor.at += count
            if (cursor.at >= cursor.samples.size) iterator.remove()
        }
    }

    private fun render(voice: DjVoice, left: FloatArray, right: FloatArray, scratch: FloatArray) {
        val pcm = voice.pcm
        if (pcm == null) {
            voice.level = 0f
            voice.audible = false
            return
        }
        val rate = pcm.rate.toDouble()
        if (!voice.seekTo.isNaN()) {
            voice.frame = voice.seekTo * rate
            voice.seekTo = Double.NaN
        }
        val target = voice.gain
        val from = voice.shownGain
        voice.shownGain = target
        if (voice.scratchStart) {
            voice.scratchStart = false
            voice.head.place(voice.frame / rate)
        }
        val audio = voice.scratchAudio
        if (voice.scratching || voice.scratchStop) {
            if (audio != null) {
                voice.head.render(audio.samples, audio.rate, outRate.toDouble(), voice.scratchGoal, target, scratch, BLOCK)
                var peak = 0f
                for (index in 0 until BLOCK) {
                    left[index] += scratch[index]
                    right[index] += scratch[index]
                    peak = max(peak, kotlin.math.abs(scratch[index]))
                }
                voice.level = peak
                voice.frame = voice.head.position * rate
            } else {
                voice.frame = voice.scratchGoal * rate
                voice.level = 0f
            }
            voice.position = voice.frame / rate
            if (voice.scratchStop) voice.scratchStop = false
            voice.audible = audio != null
            return
        }
        if (!voice.playing) {
            voice.level = 0f
            voice.audible = false
            voice.buffering = false
            return
        }
        val step = voice.rate * rate / outRate
        var frame = voice.frame
        var peak = 0f
        val available = pcm.frames
        val total = pcm.total
        var stalled = false
        for (index in 0 until BLOCK) {
            val base = floor(frame).toLong()
            if (base + 1 >= available) {
                if (pcm.finished && base + 1 >= min(total, available)) {
                    voice.playing = false
                    voice.ended = true
                } else {
                    stalled = true
                }
                break
            }
            val fraction = (frame - base).toFloat()
            val at = base.toInt()
            val g = (from + (target - from) * index / BLOCK) / 32768f
            val l = (pcm.left(at) * (1 - fraction) + pcm.left(at + 1) * fraction) * g
            val r = (pcm.right(at) * (1 - fraction) + pcm.right(at + 1) * fraction) * g
            left[index] += l
            right[index] += r
            peak = max(peak, max(kotlin.math.abs(l), kotlin.math.abs(r)))
            frame += step
        }
        voice.buffering = stalled
        voice.frame = frame
        voice.position = frame / rate
        voice.level = peak
        voice.audible = !stalled && voice.playing
    }
}
