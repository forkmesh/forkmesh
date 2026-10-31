package com.forkmesh.nativeapp.feature.tools

import android.content.Context
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.speech.tts.TextToSpeech
import android.speech.tts.UtteranceProgressListener
import android.speech.tts.Voice
import com.forkmesh.nativeapp.tools.SpeechChunk
import com.forkmesh.nativeapp.tools.SpeechEngine
import com.forkmesh.nativeapp.tools.SpeechEngineStatus
import com.forkmesh.nativeapp.tools.SpeechText
import com.forkmesh.nativeapp.tools.SpeechUtterance
import com.forkmesh.nativeapp.tools.SpeechVoice
import com.forkmesh.nativeapp.tools.SpeechVoiceQuality
import com.forkmesh.nativeapp.tools.SpeechVoices
import com.forkmesh.nativeapp.tools.SpeechWav
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.RandomAccessFile
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicInteger

internal class AndroidSpeechEngine(context: Context) : SpeechEngine {
    override var onRange: ((Int, Int) -> Unit)? = null
    override var onFinish: (() -> Unit)? = null
    override var onStatus: (() -> Unit)? = null

    @Volatile
    override var status: SpeechEngineStatus = SpeechEngineStatus.STARTING
        private set

    private val app = context.applicationContext
    private val main = Handler(Looper.getMainLooper())
    private var tts: TextToSpeech? = null
    private var session = 0
    private var text = ""
    private var voiceId = ""
    private var rate = 1f
    private var pitch = 1f
    private var position = 0
    private var pieces: List<SpeechChunk> = emptyList()
    private val renderCount = AtomicInteger()
    private val renders = ConcurrentHashMap<String, CountDownLatch>()
    private val rendered = ConcurrentHashMap<String, Boolean>()

    private val listener = object : UtteranceProgressListener() {
        override fun onStart(utteranceId: String?) {}

        override fun onDone(utteranceId: String?) {
            if (renderFinished(utteranceId, true)) return
            main.post { done(utteranceId) }
        }

        @Deprecated("Deprecated in Java")
        override fun onError(utteranceId: String?) {
            if (renderFinished(utteranceId, false)) return
            main.post { failed(utteranceId) }
        }

        override fun onStop(utteranceId: String?, interrupted: Boolean) {
            renderFinished(utteranceId, false)
        }

        override fun onRangeStart(utteranceId: String?, start: Int, end: Int, frame: Int) {
            main.post { range(utteranceId, start, end) }
        }
    }

    init {
        tts = TextToSpeech(app) { result ->
            main.post {
                val engine = tts
                status = if (result == TextToSpeech.SUCCESS && engine != null) SpeechEngineStatus.READY else SpeechEngineStatus.FAILED
                engine?.takeIf { status == SpeechEngineStatus.READY }?.setOnUtteranceProgressListener(listener)
                onStatus?.invoke()
            }
        }
    }

    val language: String get() = Locale.getDefault().toLanguageTag()

    private fun ready(): TextToSpeech? = tts?.takeIf { status == SpeechEngineStatus.READY }

    private fun installed(engine: TextToSpeech): List<Voice> =
        runCatching { engine.voices?.toList() }.getOrNull().orEmpty()
            .filter { TextToSpeech.Engine.KEY_FEATURE_NOT_INSTALLED !in (it.features ?: emptySet<String>()) }

    override fun voices(): List<SpeechVoice> {
        val engine = ready() ?: return emptyList()
        return installed(engine).map { voice ->
            SpeechVoice(
                id = voice.name,
                name = SpeechVoices.displayName(voice.name),
                language = voice.locale.toLanguageTag(),
                quality = SpeechVoiceQuality.ofAndroid(voice.quality),
                online = voice.isNetworkConnectionRequired,
            )
        }.sortedBy { it.id }
    }

    private fun limit(): Int = runCatching { TextToSpeech.getMaxSpeechInputLength() }.getOrDefault(4000).coerceAtLeast(100)

    private fun configure(engine: TextToSpeech, voiceId: String, rate: Float, pitch: Float) {
        installed(engine).firstOrNull { it.name == voiceId }?.let { runCatching { engine.voice = it } }
        engine.setSpeechRate(rate)
        engine.setPitch(pitch)
    }

    override fun speak(text: String, voiceId: String, rate: Float, pitch: Float) {
        this.text = text
        this.voiceId = voiceId
        this.rate = rate
        this.pitch = pitch
        position = 0
        play(0)
    }

    private fun play(from: Int) {
        session += 1
        val engine = ready()
        val start = from.coerceIn(0, text.length)
        pieces = SpeechText.chunks(text.substring(start), limit()).map { SpeechChunk(start + it.offset, it.text) }
        if (engine == null || pieces.isEmpty()) {
            finishLater(session)
            return
        }
        configure(engine, voiceId, rate, pitch)
        pieces.forEachIndexed { index, piece ->
            val id = SpeechUtterance(session, piece.offset, index == pieces.lastIndex).id
            val queued = engine.speak(piece.text, if (index == 0) TextToSpeech.QUEUE_FLUSH else TextToSpeech.QUEUE_ADD, null, id)
            if (queued != TextToSpeech.SUCCESS) {
                engine.stop()
                finishLater(session)
                return
            }
        }
    }

    private fun finishLater(expected: Int) {
        main.post {
            if (expected != session) return@post
            session += 1
            onFinish?.invoke()
        }
    }

    private fun current(utteranceId: String?): SpeechUtterance? =
        SpeechUtterance.parse(utteranceId)?.takeIf { it.session == session }

    private fun range(utteranceId: String?, start: Int, end: Int) {
        val utterance = current(utteranceId) ?: return
        if (end <= start) return
        position = utterance.offset + start
        onRange?.invoke(position, end - start)
    }

    private fun done(utteranceId: String?) {
        val utterance = current(utteranceId) ?: return
        if (!utterance.last) {
            pieces.firstOrNull { it.offset > utterance.offset }?.let { position = maxOf(position, it.offset) }
            return
        }
        session += 1
        onFinish?.invoke()
    }

    private fun failed(utteranceId: String?) {
        current(utteranceId) ?: return
        session += 1
        onFinish?.invoke()
    }

    override fun pause() {
        session += 1
        tts?.stop()
    }

    override fun resume() {
        play(position)
    }

    override fun stop() {
        session += 1
        tts?.stop()
    }

    private fun renderFinished(utteranceId: String?, ok: Boolean): Boolean {
        val id = utteranceId ?: return false
        val latch = renders.remove(id) ?: return false
        rendered[id] = ok
        latch.countDown()
        return true
    }

    override fun render(text: String, voiceId: String, rate: Float, pitch: Float, name: String): File {
        val engine = ready() ?: throw IOException("speech is not ready")
        val folder = File(app.cacheDir, SHARE_DIR).apply { mkdirs() }
        val parts = ArrayList<File>()
        try {
            configure(engine, voiceId, rate, pitch)
            for (piece in SpeechText.chunks(text, limit())) {
                val id = "render-${renderCount.incrementAndGet()}"
                val part = File(folder, "$id.part")
                parts += part
                val latch = CountDownLatch(1)
                renders[id] = latch
                if (engine.synthesizeToFile(piece.text, Bundle(), part, id) != TextToSpeech.SUCCESS) {
                    renders.remove(id)
                    throw IOException("the speech engine refused the text")
                }
                if (!latch.await(RENDER_TIMEOUT_SECONDS, TimeUnit.SECONDS)) {
                    renders.remove(id)
                    throw IOException("the speech engine took too long")
                }
                if (rendered.remove(id) != true) throw IOException("the speech engine stopped")
            }
            val target = File(folder, name)
            join(parts, target)
            return target
        } finally {
            parts.forEach { it.delete() }
        }
    }

    private fun join(parts: List<File>, target: File) {
        val layouts = parts.map { part ->
            val head = FileInputStream(part).use { input -> input.readNBytesCompat(SpeechWav.HEAD_BYTES) }
            SpeechWav.layout(head, part.length())
        }
        SpeechWav.requireSameFormat(layouts)
        target.outputStream().buffered().use { out ->
            out.write(SpeechWav.header(layouts[0].format, layouts.sumOf { it.dataSize }))
            val buffer = ByteArray(64 * 1024)
            parts.forEachIndexed { index, part ->
                RandomAccessFile(part, "r").use { file ->
                    file.seek(layouts[index].dataOffset.toLong())
                    var left = layouts[index].dataSize
                    while (left > 0) {
                        val read = file.read(buffer, 0, minOf(buffer.size, left))
                        if (read <= 0) break
                        out.write(buffer, 0, read)
                        left -= read
                    }
                }
            }
        }
    }

    private fun FileInputStream.readNBytesCompat(count: Int): ByteArray {
        val buffer = ByteArray(count)
        var filled = 0
        while (filled < count) {
            val read = read(buffer, filled, count - filled)
            if (read <= 0) break
            filled += read
        }
        return buffer.copyOf(filled)
    }

    fun shutdown() {
        session += 1
        onRange = null
        onFinish = null
        onStatus = null
        renders.keys.toList().forEach { renderFinished(it, false) }
        tts?.stop()
        tts?.shutdown()
        tts = null
        status = SpeechEngineStatus.FAILED
    }

    companion object {
        const val SHARE_DIR = "speech-share"
        private const val RENDER_TIMEOUT_SECONDS = 300L
    }
}
