package com.forkmesh.nativeapp.ui.kit

import android.media.AudioAttributes
import android.media.SoundPool
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import androidx.compose.foundation.gestures.awaitEachGesture
import androidx.compose.foundation.gestures.awaitFirstDown
import androidx.compose.foundation.interaction.DragInteraction
import androidx.compose.foundation.pager.PagerState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.snapshotFlow
import androidx.compose.ui.Modifier
import androidx.compose.ui.input.pointer.pointerInput
import com.forkmesh.nativeapp.App
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.ui.RatchetBurst
import com.forkmesh.nativeapp.ui.RatchetGate
import com.forkmesh.nativeapp.ui.RatchetStroke
import com.forkmesh.nativeapp.ui.SoundCue
import com.forkmesh.nativeapp.ui.SoundCueBatch
import com.forkmesh.nativeapp.ui.SoundCueFallback
import com.forkmesh.nativeapp.ui.SoundCueGate
import com.forkmesh.nativeapp.ui.SoundCuePrefs
import com.forkmesh.nativeapp.ui.SoundCueSynth
import java.io.File
import kotlinx.coroutines.flow.collectLatest
import kotlinx.coroutines.flow.dropWhile
import kotlinx.coroutines.flow.filterIsInstance
import kotlinx.coroutines.flow.first

object FmSoundCues {
    private val main = Handler(Looper.getMainLooper())
    private val batch = SoundCueBatch()
    private val gate = SoundCueGate()
    private val fallback = SoundCueFallback()
    private val clips = HashMap<SoundCue, IntArray>()
    private val ready = HashSet<Int>()
    private var pool: SoundPool? = null
    private var forced = false
    private var loading = false
    private var strikes = 0
    private val ratchetGate = RatchetGate()
    private val ratchetClips = HashMap<RatchetBurst, IntArray>()
    private var catchClips = IntArray(0)
    private val ratchetReady = HashSet<Int>()
    private var ratchetPool: SoundPool? = null
    private var ratchetTurn = 0
    private var quiet = 0

    val enabled: Boolean get() = SoundCuePrefs.enabled(Services.get<PrefStore>())

    fun play(cue: SoundCue) {
        fallback.heard(SystemClock.uptimeMillis())
        main.post { hold(cue, always = false) }
    }

    fun preview(cue: SoundCue) {
        fallback.heard(SystemClock.uptimeMillis())
        main.post { hold(cue, always = true) }
    }

    fun touched(downAtMs: Long, cue: SoundCue = SoundCue.CLICK) {
        main.postDelayed({ if (fallback.silentSince(downAtMs)) play(cue) }, SoundCueFallback.DELAY_MS)
    }

    fun ratchet(lines: Int, changed: Int, flagged: Int) {
        if (lines <= 0 || !enabled) return
        main.post {
            ratchetGate.pass(lines, changed, flagged)?.let { stroke ->
                strike(stroke)
                main.postDelayed(::closeRatchet, RatchetBurst.BATCH_MS)
            }
        }
    }

    fun warmUp() {
        main.post { if (enabled) load() }
    }

    fun quieten(on: Boolean) {
        quiet = (quiet + if (on) 1 else -1).coerceAtLeast(0)
    }

    private fun hold(cue: SoundCue, always: Boolean) {
        if (!always && (quiet > 0 || !enabled)) return
        forced = forced || always
        if (batch.hold(cue)) main.post(::flush)
    }

    private fun flush() {
        val always = forced
        forced = false
        val cue = batch.take() ?: return
        if (!always && (quiet > 0 || !enabled)) return
        if (!gate.admit(cue, SystemClock.uptimeMillis())) return
        val sounds = pool ?: return load()
        val ids = clips[cue] ?: return
        val id = if (cue == SoundCue.KEYSTROKE) ids[strikes++ % ids.size] else ids[0]
        if (id in ready) sounds.play(id, 1f, 1f, 1, 0, 1f)
    }

    private fun closeRatchet() {
        val stroke = ratchetGate.close() ?: return
        strike(stroke)
        main.postDelayed(::closeRatchet, RatchetBurst.BATCH_MS)
    }

    private fun strike(stroke: RatchetStroke) {
        if (!enabled) return
        val sounds = ratchetPool ?: return load()
        val ids = ratchetClips[stroke.burst] ?: return
        ratchetTurn = (ratchetTurn + 1) and Int.MAX_VALUE
        val id = ids[ratchetTurn % ids.size]
        if (id in ratchetReady) sounds.play(id, 1f, 1f, 0, 0, 1f)
        if (stroke.caught && catchClips.isNotEmpty()) {
            val caught = catchClips[ratchetTurn % catchClips.size]
            if (caught in ratchetReady) sounds.play(caught, 1f, 1f, 1, 0, 1f)
        }
    }

    private fun load() {
        if (pool != null || loading) return
        val context = runCatching { App.instance.applicationContext }.getOrNull() ?: return
        loading = true
        Thread({
            val dir = File(context.cacheDir, "sound-cues")
            runCatching { dir.mkdirs() }
            fun write(name: String, pcm: ShortArray): File {
                val wav = SoundCueSynth.wav(pcm)
                return File(dir, name).also { file ->
                    if (!file.exists() || !file.readBytes().contentEquals(wav)) file.writeBytes(wav)
                }
            }
            val files = runCatching {
                SoundCue.entries.associateWith { cue ->
                    val seeds = if (cue == SoundCue.KEYSTROKE) (1..SoundCueSynth.KEYSTROKE_VARIANTS).map(SoundCueSynth::keystrokeSeed) else listOf(0L)
                    seeds.mapIndexed { index, seed -> write("${cue.slug}-$index.wav", SoundCueSynth.render(cue, seed = seed)) }
                }
            }.getOrNull()
            val bursts = runCatching {
                RatchetBurst.all.associateWith { burst ->
                    (1..RatchetBurst.VARIANTS).map { index ->
                        write("${burst.slug}-$index.wav", burst.render(seed = SoundCueSynth.keystrokeSeed(index)))
                    }
                }
            }.getOrNull()
            val catches = runCatching {
                (1..RatchetBurst.VARIANTS).map { index ->
                    write("ratchet-catch-$index.wav", RatchetBurst.renderCatch(seed = SoundCueSynth.keystrokeSeed(index)))
                }
            }.getOrNull()
            main.post {
                install(files)
                installRatchet(bursts, catches)
            }
        }, "SoundCueRender").apply {
            isDaemon = true
            start()
        }
    }

    private fun install(files: Map<SoundCue, List<File>>?) {
        loading = false
        if (files == null) return
        val sounds = SoundPool.Builder()
            .setMaxStreams(6)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build(),
            )
            .build()
        sounds.setOnLoadCompleteListener { _, id, status ->
            if (status == 0) main.post { ready += id }
        }
        files.forEach { (cue, list) -> clips[cue] = list.map { sounds.load(it.path, 1) }.toIntArray() }
        pool = sounds
    }

    private fun installRatchet(files: Map<RatchetBurst, List<File>>?, catches: List<File>?) {
        if (files == null) return
        val sounds = SoundPool.Builder()
            .setMaxStreams(6)
            .setAudioAttributes(
                AudioAttributes.Builder()
                    .setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
                    .build(),
            )
            .build()
        sounds.setOnLoadCompleteListener { _, id, status ->
            if (status == 0) main.post { ratchetReady += id }
        }
        files.forEach { (burst, list) -> ratchetClips[burst] = list.map { sounds.load(it.path, 1) }.toIntArray() }
        catchClips = catches.orEmpty().map { sounds.load(it.path, 1) }.toIntArray()
        ratchetPool = sounds
    }
}

@Composable
fun FmSoundOnShow(open: SoundCue = SoundCue.OPEN, close: SoundCue = SoundCue.CLOSE) {
    DisposableEffect(Unit) {
        FmSoundCues.play(open)
        onDispose { FmSoundCues.play(close) }
    }
}

@Composable
fun FmQuietSounds() {
    DisposableEffect(Unit) {
        FmSoundCues.quieten(true)
        onDispose { FmSoundCues.quieten(false) }
    }
}

fun Modifier.fmTapSounds(): Modifier = pointerInput(Unit) {
    awaitEachGesture {
        val down = awaitFirstDown(requireUnconsumed = false)
        FmUserActivity.touchDown(down.uptimeMillis)
        val slop = viewConfiguration.touchSlop
        var moved = false
        while (true) {
            val change = awaitPointerEvent().changes.firstOrNull { it.id == down.id } ?: break
            if ((change.position - down.position).getDistance() > slop) moved = true
            if (!change.pressed) {
                if (!moved && change.isConsumed) FmSoundCues.touched(down.uptimeMillis)
                break
            }
        }
    }
}

@Composable
fun FmPagerSounds(state: PagerState) {
    LaunchedEffect(state) {
        state.interactionSource.interactions.filterIsInstance<DragInteraction.Start>().collectLatest {
            val from = state.settledPage
            snapshotFlow { state.isScrollInProgress }.dropWhile { !it }.first { !it }
            if (state.settledPage != from) FmSoundCues.play(SoundCue.SELECT)
        }
    }
}
