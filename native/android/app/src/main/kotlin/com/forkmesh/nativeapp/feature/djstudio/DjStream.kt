package com.forkmesh.nativeapp.feature.djstudio

import android.media.MediaCodec
import android.media.MediaDataSource
import android.media.MediaExtractor
import android.media.MediaFormat
import com.forkmesh.nativeapp.djstudio.DjScratchAudio
import com.forkmesh.nativeapp.djstudio.DjWaveRanges
import com.forkmesh.nativeapp.net.HttpClient
import java.io.File
import java.io.IOException
import java.io.RandomAccessFile
import java.nio.ByteOrder
import java.nio.ShortBuffer
import java.nio.channels.FileChannel
import java.util.concurrent.atomic.AtomicBoolean
import kotlin.math.ceil
import kotlin.math.max
import kotlin.math.min

internal class DjPcm(file: File, capacityFrames: Long, val rate: Int) {
    private val channel = RandomAccessFile(file, "rw").channel
    val capacity: Long = max(1L, min(capacityFrames, Int.MAX_VALUE / 4L - 1))
    private val buffer: ShortBuffer = channel.map(FileChannel.MapMode.READ_WRITE, 0, capacity * 4).order(ByteOrder.nativeOrder()).asShortBuffer()

    @Volatile var frames: Long = 0
        private set
    @Volatile var finished = false
    @Volatile var failed = false
    @Volatile var total: Long = capacity

    val seconds: Double get() = total.toDouble() / rate

    fun write(left: Short, right: Short) {
        val at = frames
        if (at >= capacity) return
        val index = (at * 2).toInt()
        buffer.put(index, left)
        buffer.put(index + 1, right)
        frames = at + 1
    }

    fun left(frame: Int): Short = buffer.get(frame * 2)
    fun right(frame: Int): Short = buffer.get(frame * 2 + 1)

    fun close() {
        runCatching { channel.close() }
    }
}

internal class DjStreamFiles(private val dir: File) {
    init {
        dir.mkdirs()
    }

    fun complete(id: String): File? = File(dir, "$id.media").takeIf { it.isFile && it.length() > 0 }

    fun partial(id: String): File = File(dir, "$id.part")

    fun finish(id: String): File {
        val done = File(dir, "$id.media")
        partial(id).renameTo(done)
        done.setLastModified(System.currentTimeMillis())
        trim()
        return done
    }

    fun touch(id: String) {
        complete(id)?.setLastModified(System.currentTimeMillis())
    }

    private fun trim() {
        val files = dir.listFiles { file -> file.isFile && file.name.endsWith(".media") }?.sortedByDescending { it.lastModified() } ?: return
        files.drop(KEEP).forEach { it.delete() }
    }

    companion object {
        const val KEEP = 10
    }
}

internal class DjDownload(private val url: String, private val file: File, private val agent: String) {
    private val lock = Object()
    private var total = -1L
    private var ready = BooleanArray(0)
    private val wanted = ArrayDeque<Int>()
    private val stopped = AtomicBoolean(false)
    @Volatile var failed = false
        private set
    @Volatile var done = false
        private set

    fun start(onDone: () -> Unit, onFailed: () -> Unit) {
        Thread({
            try {
                val first = fetch(0L..(DjWaveRanges.CHUNK - 1)) ?: throw IOException("no first range")
                RandomAccessFile(file, "rw").use { raf ->
                    raf.setLength(total)
                    raf.seek(0)
                    raf.write(first)
                }
                val spans = DjWaveRanges.spans(total)
                synchronized(lock) {
                    ready = BooleanArray(spans.size)
                    ready[0] = true
                    for (index in 1 until spans.size) wanted.add(index)
                    lock.notifyAll()
                }
                val workers = (1..4).map {
                    Thread({ work(spans) }, "dj-download").apply { start() }
                }
                workers.forEach { it.join() }
                if (synchronized(lock) { ready.all { it } }) {
                    done = true
                    onDone()
                } else if (!stopped.get()) {
                    fail(onFailed)
                }
            } catch (_: Throwable) {
                fail(onFailed)
            }
        }, "dj-download").start()
    }

    private fun fail(onFailed: () -> Unit) {
        failed = true
        synchronized(lock) { lock.notifyAll() }
        if (!stopped.get()) onFailed()
    }

    private fun work(spans: List<LongRange>) {
        while (!stopped.get() && !failed) {
            val index = synchronized(lock) { wanted.removeFirstOrNull() } ?: return
            var bytes: ByteArray? = null
            for (attempt in 0 until 3) {
                bytes = runCatching { fetch(spans[index]) }.getOrNull()
                if (bytes != null || stopped.get()) break
            }
            if (bytes == null) {
                failed = true
                synchronized(lock) { lock.notifyAll() }
                return
            }
            synchronized(lock) {
                RandomAccessFile(file, "rw").use { raf ->
                    raf.seek(spans[index].first)
                    raf.write(bytes)
                }
                ready[index] = true
                lock.notifyAll()
            }
        }
    }

    private fun fetch(span: LongRange): ByteArray? {
        val response = HttpClient.Request.get(url)
            .header("Range", DjWaveRanges.header(span))
            .header("User-Agent", agent.ifEmpty { null })
            .timeouts(10_000, 30_000)
            .send()
        if (response.status != 206 && response.status != 200) return null
        if (total < 0) {
            total = DjWaveRanges.total(response.header("Content-Range")) ?: DjWaveRanges.declaredLength(url)
                ?: if (response.status == 200) response.body.size.toLong() else return null
        }
        return response.body
    }

    fun size(): Long = synchronized(lock) {
        while (total < 0 && !failed && !stopped.get()) lock.wait(200)
        total
    }

    fun await(start: Long, end: Long): Boolean {
        synchronized(lock) {
            while (true) {
                if (failed || stopped.get()) return false
                if (ready.isEmpty()) {
                    lock.wait(200)
                    continue
                }
                val first = (start / DjWaveRanges.CHUNK).toInt()
                val last = min(ready.size - 1, ((end - 1) / DjWaveRanges.CHUNK).toInt())
                var missing = -1
                for (index in first..last) if (!ready[index]) {
                    missing = index
                    break
                }
                if (missing < 0) return true
                if (wanted.remove(missing)) wanted.addFirst(missing)
                lock.wait(200)
            }
        }
    }

    fun stop() {
        stopped.set(true)
        synchronized(lock) { lock.notifyAll() }
    }

    fun source(): MediaDataSource = object : MediaDataSource() {
        private var raf: RandomAccessFile? = null

        override fun readAt(position: Long, buffer: ByteArray, offset: Int, size: Int): Int {
            val length = size()
            if (length <= 0 || position >= length) return -1
            val count = min(size.toLong(), length - position).toInt()
            if (!await(position, position + count)) throw IOException("download stopped")
            synchronized(this) {
                val reader = raf ?: RandomAccessFile(file, "r").also { raf = it }
                reader.seek(position)
                return reader.read(buffer, offset, count)
            }
        }

        override fun getSize(): Long = size()

        override fun close() {
            runCatching { raf?.close() }
        }
    }
}

internal class DjDecoder(
    private val pcmFile: File,
    private val onReady: (DjPcm) -> Unit,
    private val onFinished: (DjPcm, DjScratchAudio?) -> Unit,
    private val onFailed: () -> Unit,
) {
    private val stopped = AtomicBoolean(false)

    fun stop() {
        stopped.set(true)
    }

    fun start(source: MediaDataSource?, path: String?) {
        Thread({ run(source, path) }, "dj-decode").start()
    }

    private fun run(source: MediaDataSource?, path: String?) {
        val extractor = MediaExtractor()
        var codec: MediaCodec? = null
        var pcm: DjPcm? = null
        try {
            if (source != null) extractor.setDataSource(source) else extractor.setDataSource(path!!)
            val track = (0 until extractor.trackCount).firstOrNull {
                extractor.getTrackFormat(it).getString(MediaFormat.KEY_MIME)?.startsWith("audio/") == true
            } ?: throw IOException("no audio track")
            extractor.selectTrack(track)
            val format = extractor.getTrackFormat(track)
            val mime = format.getString(MediaFormat.KEY_MIME)!!
            var rate = format.getInteger(MediaFormat.KEY_SAMPLE_RATE)
            var channels = format.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
            val durationUs = if (format.containsKey(MediaFormat.KEY_DURATION)) format.getLong(MediaFormat.KEY_DURATION) else 0L
            val seconds = durationUs / 1_000_000.0
            val capacity = if (seconds > 0) ceil(seconds * rate).toLong() + rate * 2L else rate * 60L * 20
            var store = DjPcm(pcmFile, capacity, rate)
            pcm = store
            if (seconds > 0) store.total = min(store.capacity, ceil(seconds * rate).toLong())
            val keepsMono = DjScratchAudio.fits(seconds)
            val mono = if (keepsMono) ShortArray(ceil(seconds * MONO_RATE).toInt() + MONO_RATE) else null
            var monoCount = 0
            var monoSum = 0.0
            var monoFrames = 0
            var monoPhase = 0L
            codec = MediaCodec.createDecoderByType(mime)
            codec.configure(format, null, null, 0)
            codec.start()
            onReady(store)
            val info = MediaCodec.BufferInfo()
            var inputDone = false
            while (!stopped.get()) {
                if (!inputDone) {
                    val index = codec.dequeueInputBuffer(10_000)
                    if (index >= 0) {
                        val input = codec.getInputBuffer(index)!!
                        val size = extractor.readSampleData(input, 0)
                        if (size < 0) {
                            codec.queueInputBuffer(index, 0, 0, 0, MediaCodec.BUFFER_FLAG_END_OF_STREAM)
                            inputDone = true
                        } else {
                            codec.queueInputBuffer(index, 0, size, extractor.sampleTime, 0)
                            extractor.advance()
                        }
                    }
                }
                val out = codec.dequeueOutputBuffer(info, 10_000)
                if (out == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    val changed = codec.outputFormat
                    channels = changed.getInteger(MediaFormat.KEY_CHANNEL_COUNT)
                    val fresh = changed.getInteger(MediaFormat.KEY_SAMPLE_RATE)
                    if (fresh != rate && store.frames == 0L) {
                        rate = fresh
                        store.close()
                        store = DjPcm(pcmFile, if (seconds > 0) ceil(seconds * rate).toLong() + rate * 2L else rate * 60L * 20, rate)
                        pcm = store
                        if (seconds > 0) store.total = min(store.capacity, ceil(seconds * rate).toLong())
                        onReady(store)
                    }
                    continue
                }
                if (out < 0) continue
                val buffer = codec.getOutputBuffer(out)!!.order(ByteOrder.nativeOrder()).asShortBuffer()
                val samples = info.size / 2
                val step = max(1, channels)
                var index = 0
                while (index + step - 1 < samples) {
                    val left = buffer.get(index)
                    val right = if (step > 1) buffer.get(index + 1) else left
                    store.write(left, right)
                    if (mono != null) {
                        monoSum += (left + right) / 2.0
                        monoCount += 1
                        monoPhase += MONO_RATE
                        if (monoPhase >= rate) {
                            monoPhase -= rate
                            if (monoFrames < mono.size) mono[monoFrames++] = (monoSum / monoCount).toInt().toShort()
                            monoSum = 0.0
                            monoCount = 0
                        }
                    }
                    index += step
                }
                codec.releaseOutputBuffer(out, false)
                if (info.flags and MediaCodec.BUFFER_FLAG_END_OF_STREAM != 0) break
            }
            if (stopped.get()) return
            store.total = store.frames
            store.finished = true
            val audio = mono?.let { DjScratchAudio(it.copyOf(monoFrames), MONO_RATE.toDouble()) }
            onFinished(store, audio)
        } catch (_: Throwable) {
            pcm?.failed = true
            pcm?.finished = true
            pcm?.let { it.total = it.frames }
            if (!stopped.get()) onFailed()
        } finally {
            runCatching { codec?.stop() }
            runCatching { codec?.release() }
            runCatching { extractor.release() }
        }
    }

    companion object {
        const val MONO_RATE = 22_050
    }
}
