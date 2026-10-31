package com.forkmesh.nativeapp.feature.djstudio

import android.app.Activity
import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.os.SystemClock
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableDoubleStateOf
import androidx.compose.runtime.mutableStateListOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.compose.ui.geometry.Rect
import androidx.compose.ui.graphics.ImageBitmap
import androidx.compose.ui.graphics.asImageBitmap
import com.forkmesh.nativeapp.api.PrefStore
import com.forkmesh.nativeapp.core.Services
import com.forkmesh.nativeapp.djstudio.DjAutomix
import com.forkmesh.nativeapp.djstudio.DjAutomixDeck
import com.forkmesh.nativeapp.djstudio.DjAutomixStep
import com.forkmesh.nativeapp.djstudio.DjRepeat
import com.forkmesh.nativeapp.djstudio.DjRepeatStep
import com.forkmesh.nativeapp.djstudio.DjBpm
import com.forkmesh.nativeapp.djstudio.DjCrate
import com.forkmesh.nativeapp.djstudio.DjDeckSide
import com.forkmesh.nativeapp.djstudio.DjDrumPad
import com.forkmesh.nativeapp.djstudio.DjFocus
import com.forkmesh.nativeapp.djstudio.DjHistoryItem
import com.forkmesh.nativeapp.djstudio.DjJam
import com.forkmesh.nativeapp.djstudio.DjJamPresence
import com.forkmesh.nativeapp.djstudio.DjJamSummary
import com.forkmesh.nativeapp.djstudio.DjLoadTarget
import com.forkmesh.nativeapp.djstudio.DjPlayedSong
import com.forkmesh.nativeapp.djstudio.DjQueueItem
import com.forkmesh.nativeapp.djstudio.DjReaction
import com.forkmesh.nativeapp.djstudio.DjSampleStep
import com.forkmesh.nativeapp.djstudio.DjSaved
import com.forkmesh.nativeapp.djstudio.DjSavedDeck
import com.forkmesh.nativeapp.djstudio.DjScratchAudio
import com.forkmesh.nativeapp.djstudio.DjSearchResult
import com.forkmesh.nativeapp.djstudio.DjSection
import com.forkmesh.nativeapp.djstudio.DjSmart
import com.forkmesh.nativeapp.djstudio.DjStudio
import com.forkmesh.nativeapp.djstudio.DjTapTempo
import com.forkmesh.nativeapp.djstudio.DjTrack
import com.forkmesh.nativeapp.djstudio.DjWaveAccumulator
import com.forkmesh.nativeapp.djstudio.DjWaveBar
import com.forkmesh.nativeapp.djstudio.DjWaveCache
import com.forkmesh.nativeapp.net.HttpClient
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import java.io.File
import java.util.UUID
import java.util.concurrent.Executors
import kotlin.math.abs
import kotlin.math.max
import kotlin.math.min
import kotlin.random.Random

sealed interface DjPrompt {
    data class NewPlaylist(val ids: List<String>) : DjPrompt
    data class NewTag(val id: String) : DjPrompt
    data class RenamePlaylist(val id: String) : DjPrompt
    data class RenameTag(val name: String) : DjPrompt
}

sealed interface DjSheet {
    data class Load(val target: DjLoadTarget) : DjSheet
    data class Deck(val side: DjDeckSide) : DjSheet
    data object Pads : DjSheet
    data object Info : DjSheet
    data object Jam : DjSheet
    data object Crate : DjSheet
    data class Song(val id: String) : DjSheet
    data class Similar(val side: DjDeckSide) : DjSheet
}

class DjDeck internal constructor(val side: DjDeckSide, internal val voice: DjVoice) {
    var videoId by mutableStateOf("")
    var title by mutableStateOf("")
    var author by mutableStateOf("")
    var artwork by mutableStateOf<ImageBitmap?>(null)
    var waveform by mutableStateOf<List<DjWaveBar>>(emptyList())
    var duration by mutableDoubleStateOf(0.0)
    var playing by mutableStateOf(false)
    var loading by mutableStateOf(false)
    var reading by mutableStateOf(false)
    var cue by mutableDoubleStateOf(0.0)
    var level by mutableDoubleStateOf(1.0)
    var rate by mutableDoubleStateOf(1.0)
    var bend by mutableDoubleStateOf(0.0)
    var bpm by mutableDoubleStateOf(0.0)
    var loop by mutableStateOf(false)
    var repeats by mutableStateOf(false)
    var scratching by mutableStateOf(false)
    var brake by mutableDoubleStateOf(0.0)
    var sample by mutableStateOf<DjSampleStep>(DjSampleStep.Idle)
    var showsRemaining by mutableStateOf(false)
    var stoppedAt by mutableDoubleStateOf(0.0)

    internal var keepsPlaying = false
    internal var counted = false
    internal var failed = false
    internal var tapTempo = DjTapTempo()
    internal var resumeAfterScratch = false
    internal var scratchAt = 0.0
    internal var scratchAudio: DjScratchAudio? = null
    internal var job: Job? = null
    internal var download: DjDownload? = null
    internal var decoder: DjDecoder? = null
    internal var pcmFile: File? = null
    internal var brakeJob: Job? = null
    internal var repeatJob: Job? = null
    internal var brakeStart = 0L
    internal var loadId = 0L

    val loaded: Boolean get() = videoId.isNotEmpty()
    val effectiveRate: Double get() = rate * (1 + bend) * (1 - brake)
    val shownBpm: Double get() = bpm * effectiveRate
    val moving: Boolean get() = playing && !scratching

    fun position(): Double = if (scratching) scratchAt else voice.position
    val buffering: Boolean get() = loading || voice.buffering
}

class DjStudioModel internal constructor(
    internal val context: Context,
    internal val engine: DjEngine,
    internal val resolver: DjResolver,
) {
    internal val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)
    internal val prefs: PrefStore? get() = Services.get<PrefStore>()
    private val saver = Executors.newSingleThreadExecutor { Thread(it, "dj-save").apply { isDaemon = true } }
    private val streams = DjStreamFiles(File(context.cacheDir, "dj-streams"))
    private val waves = File(context.cacheDir, "dj-waveforms").apply { mkdirs() }
    private val pcmDir = File(context.cacheDir, "dj-pcm").apply { mkdirs() }
    internal val agent: String = runCatching { android.webkit.WebSettings.getDefaultUserAgent(context) }.getOrDefault("")

    val a = DjDeck(DjDeckSide.A, engine.a)
    val b = DjDeck(DjDeckSide.B, engine.b)
    var crossfader by mutableDoubleStateOf(0.0)
    var samplerLevel by mutableDoubleStateOf(0.8)
    val sections = mutableStateListOf<DjSection?>().apply { repeat(DjStudio.SECTION_COUNT) { add(null) } }
    var automixing by mutableStateOf(false)
    var automixOn by mutableStateOf(false)
    var autoDjOn by mutableStateOf(false)
    var autoDjFinding by mutableStateOf(false)
    var onScreen by mutableStateOf(false)
    var notice by mutableStateOf<String?>(null)
    var sheet by mutableStateOf<DjSheet?>(null)
    var prompt by mutableStateOf<DjPrompt?>(null)
    var searchTarget by mutableStateOf(DjLoadTarget.A)
    var searchText by mutableStateOf("")
    var searchResults by mutableStateOf<List<DjSearchResult>>(emptyList())
    var searching by mutableStateOf(false)
    var searchProblem by mutableStateOf("")
    var similarSeed by mutableStateOf("")
    var similarResults by mutableStateOf<List<DjSearchResult>>(emptyList())
    var similarLoading by mutableStateOf(false)
    var similarProblem by mutableStateOf("")
    var history by mutableStateOf<List<DjHistoryItem>>(emptyList())
    var library by mutableStateOf<List<DjPlayedSong>>(emptyList())
    var crate by mutableStateOf(DjCrate())
    var flashedPad by mutableStateOf<DjDrumPad?>(null)
    var confirmingReset by mutableStateOf(false)
    var queue by mutableStateOf<List<DjQueueItem>>(emptyList())
    var jam by mutableStateOf<DjJam?>(null)
    var jams by mutableStateOf<List<DjJamSummary>>(emptyList())
    var jamBusy by mutableStateOf(false)
    var jamProblem by mutableStateOf("")
    var inviteText by mutableStateOf("")
    var inviteSuggestions by mutableStateOf<List<String>>(emptyList())
    var crowd by mutableStateOf<List<DjJamPresence>>(emptyList())
    val effects = mutableStateListOf<DjEffect>()
    var reacting by mutableStateOf(false)
    var shareMotion by mutableStateOf(true)
    var draggedId by mutableStateOf<String?>(null)
    var dragPoint by mutableStateOf<androidx.compose.ui.geometry.Offset?>(null)
    var dropSide by mutableStateOf<DjDeckSide?>(null)

    internal val deckFrames = HashMap<DjDeckSide, Rect>()
    internal var seenReactions = mutableListOf<String>()
    internal val sentReactions = mutableListOf<Long>()
    internal var jamJob: Job? = null
    internal val shareJobs = HashMap<DjDeckSide, Job>()
    internal var suggestJob: Job? = null
    internal val applied = HashMap<DjDeckSide, Long>()
    internal var applyingRemote = false
    internal val motion = DjMotionFeed(context)
    internal var onLeave: (() -> Unit)? = null
    internal val unplayable = mutableSetOf<String>()
    internal var autoDjJob: Job? = null
    internal var autoDjSeed = ""
    internal var autoDjChoices = listOf<com.forkmesh.nativeapp.djstudio.DjAutoDjCandidate>()
    internal var autoDjPlayed = listOf<String>()
    internal var autoDjOwnsAutomix = false
    internal var autoDjRefresh = false
    private var sectionCursor = 0
    private var searchInstances = listOf<String>()
    private var automixLoop: Job? = null
    private var automixJob: Job? = null
    private var noticeJob: Job? = null
    private var saveJob: Job? = null
    private var searchJob: Job? = null
    private var similarJob: Job? = null
    private val sectionJobs = HashMap<DjDeckSide, Job>()
    private var tickJob: Job? = null
    private var savedAt = 0L
    private var resumeAutomix = false
    private var resumeAutoDj = false
    private var active = false
    private var pcmCounter = 0L
    private var lastPlaying = false

    init {
        val saved = DjSaved.decode(prefs?.getString(DjStudio.STORAGE_KEY))
        crossfader = saved.crossfader
        samplerLevel = saved.samplerLevel
        engine.samplerLevel = saved.samplerLevel.toFloat()
        saved.sections.forEachIndexed { index, section -> sections[index] = section }
        sectionCursor = sections.indexOfFirst { it == null }.takeIf { it >= 0 } ?: 0
        history = saved.history
        library = saved.library
        queue = saved.queue
        crate = DjCrate.decode(prefs?.getString(DjCrate.STORAGE_KEY))
        shareMotion = prefs?.getBool(MOTION_KEY) ?: true
        resumeAutomix = saved.automix
        resumeAutoDj = saved.autoDj
        for (deck in listOf(a, b)) {
            val stored = saved[deck.side]
            deck.repeats = stored.repeats
            deck.cue = stored.cue
            deck.level = stored.level
            deck.rate = stored.rate
            deck.bpm = stored.bpm
            if (stored.videoId.isNotEmpty()) {
                prepare(deck, stored.videoId, stored.title, stored.author, stored.position, true)
                deck.keepsPlaying = stored.playing
                deck.counted = stored.playing || stored.position > stored.cue + 1
            }
        }
        applyMix()
        refreshSmart()
        startTicking()
    }

    fun deck(side: DjDeckSide): DjDeck = if (side == DjDeckSide.A) a else b

    val anyPlaying: Boolean get() = a.playing || b.playing

    val audibleDeck: DjDeck?
        get() = listOf(a, b).filter { it.playing }.maxByOrNull { DjStudio.gain(it.side, crossfader) * it.level }

    fun nowPlayingTitle(): String = (audibleDeck ?: listOf(a, b).firstOrNull { it.loaded })?.let { it.title.ifEmpty { "Deck ${it.side.label}" } }.orEmpty()

    fun nowPlayingDetail(): String {
        val deck = audibleDeck ?: return if (a.loaded || b.loaded) "Paused" else ""
        return listOf(deck.author, "Deck ${deck.side.label}").filter { it.isNotEmpty() }.joinToString(" · ")
    }

    fun resumePlayback() {
        for (deck in listOf(a, b)) if (deck.keepsPlaying && deck.loaded && !deck.playing) play(deck)
        val automix = resumeAutomix
        val autoDj = resumeAutoDj
        resumeAutomix = false
        resumeAutoDj = false
        if (!anyPlaying) return
        if (automix && !automixOn) setAutomix(true)
        if (autoDj && automixOn) setAutoDj(true, refresh = false)
        if (!prefs?.getString(JAM_KEY).isNullOrEmpty()) resumeJam()
    }

    fun appear(activity: Activity?) {
        if (active) return
        active = true
        onScreen = true
        activity?.let { resolver.dock(it) }
        activity?.window?.addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        motion.onShake = { shakeMix() }
        motion.listenForShakes(true)
        applyMix()
        resumeJam()
    }

    fun disappear(activity: Activity?) {
        if (!active) return
        active = false
        onScreen = false
        motion.listenForShakes(false)
        activity?.window?.clearFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        for (deck in listOf(a, b)) if (deck.scratching) scratchEnded(deck.side)
        for (deck in listOf(a, b)) if (deck.brake != 0.0) releaseBrake(deck)
        save(true)
        if (!anyPlaying) setAutomix(false)
    }

    fun leave() {
        onLeave?.invoke()
    }

    private var resumable = setOf<DjDeckSide>()

    fun pauseAll() {
        resumable = listOf(a, b).filter { it.playing }.map { it.side }.toSet()
        for (deck in listOf(a, b)) if (deck.playing) pause(deck)
        holdAutomix()
    }

    fun resumeAll() {
        for (side in resumable) deck(side).takeIf { it.loaded && !it.playing }?.let { play(it) }
        resumable = emptySet()
        if (!anyPlaying) listOf(a, b).filter { it.loaded }.maxByOrNull { DjStudio.gain(it.side, crossfader) * it.level }?.let { play(it) }
    }

    fun stopTheMusic() {
        setAutomix(false)
        for (deck in listOf(a, b)) if (deck.playing || deck.keepsPlaying) pause(deck)
        DjMediaService.end(context)
    }

    fun resetAll() {
        setAutomix(false)
        searchJob?.cancel()
        sectionJobs.values.forEach { it.cancel() }
        sectionJobs.clear()
        val fresh = DjSaved()
        for (deck in listOf(a, b)) {
            unload(deck)
            deck.cue = 0.0
            deck.level = fresh[deck.side].level
            deck.rate = 1.0
            deck.bend = 0.0
            deck.bpm = 0.0
            deck.loop = false
            deck.repeats = false
            deck.sample = DjSampleStep.Idle
            deck.showsRemaining = false
            deck.tapTempo = DjTapTempo()
            deck.brake = 0.0
        }
        crossfader = fresh.crossfader
        samplerLevel = fresh.samplerLevel
        engine.samplerLevel = fresh.samplerLevel.toFloat()
        for (index in sections.indices) sections[index] = null
        sectionCursor = 0
        searchText = ""
        searchResults = emptyList()
        searching = false
        searchProblem = ""
        sheet = null
        applyMix()
        save(true)
        DjMediaService.end(context)
        show("Studio reset. Load a song on each deck to start fresh.")
    }

    fun show(text: String) {
        notice = text
        noticeJob?.cancel()
        noticeJob = scope.launch {
            delay(2400)
            notice = null
        }
    }

    fun load(side: DjDeckSide, id: String, title: String = "", author: String = "") {
        val deck = deck(side)
        prepare(deck, id, title, author, 0.0, false)
        show(if (title.isEmpty()) "Deck ${side.label} loaded." else "Deck ${side.label}: $title")
        save()
        share(side)
    }

    fun loadLink(text: String): Boolean {
        val id = DjStudio.videoId(text)
        if (id.isEmpty()) return false
        place(id)
        return true
    }

    fun place(id: String, title: String = "", author: String = "") {
        val side = searchTarget.side
        if (side != null) load(side, id, title, author) else enqueue(id, title, author)
    }

    private fun unload(deck: DjDeck) {
        deck.job?.cancel()
        deck.repeatJob?.cancel()
        deck.repeatJob = null
        deck.download?.stop()
        deck.decoder?.stop()
        deck.download = null
        deck.decoder = null
        deck.voice.playing = false
        deck.voice.pcm = null
        deck.voice.scratchAudio = null
        deck.voice.scratching = false
        deck.pcmFile?.delete()
        deck.pcmFile = null
        deck.videoId = ""
        deck.title = ""
        deck.author = ""
        deck.artwork = null
        deck.waveform = emptyList()
        deck.scratchAudio = null
        deck.reading = false
        deck.loading = false
        deck.duration = 0.0
        deck.playing = false
        deck.keepsPlaying = false
        deck.scratching = false
        deck.stoppedAt = 0.0
        deck.voice.seek(0.0)
    }

    internal fun prepare(deck: DjDeck, id: String, title: String, author: String, start: Double, keepSettings: Boolean) {
        sectionJobs[deck.side]?.cancel()
        unload(deck)
        val ticket = ++deck.loadId
        deck.videoId = id
        deck.title = title
        deck.author = author
        deck.loading = true
        deck.counted = false
        deck.failed = false
        deck.voice.seek(start)
        deck.stoppedAt = start
        if (!keepSettings) {
            deck.cue = 0.0
            deck.bpm = 0.0
            deck.loop = false
            deck.tapTempo = DjTapTempo()
        }
        cachedWave(id)?.let { deck.waveform = it }
        deck.voice.rate = deck.effectiveRate
        applyMix()
        scope.launch {
            val image = withContext(Dispatchers.IO) { DjImages.artwork(id) }
            if (deck.videoId == id) deck.artwork = image
        }
        deck.job = scope.launch {
            val file = streams.complete(id)
            if (file != null) {
                streams.touch(id)
                decode(deck, ticket, id, null, file.path)
                return@launch
            }
            val found = resolver.resolve(id)
            if (deck.loadId != ticket) return@launch
            when (found) {
                is DjResolved.Failed -> failed(deck, found.code)
                is DjResolved.Found -> {
                    if (deck.title.isEmpty() && found.title.isNotEmpty()) retitle(deck, found.title, found.author)
                    if (found.duration > 0 && deck.duration <= 0) deck.duration = found.duration
                    val download = DjDownload(found.url, streams.partial(id), agent)
                    deck.download = download
                    download.start(
                        onDone = { scope.launch { if (deck.loadId == ticket) withContext(Dispatchers.IO) { streams.finish(id) } } },
                        onFailed = { scope.launch { if (deck.loadId == ticket) failed(deck, -1) } },
                    )
                    decode(deck, ticket, id, download.source(), null)
                }
            }
        }
    }

    private fun decode(deck: DjDeck, ticket: Long, id: String, source: android.media.MediaDataSource?, path: String?) {
        val file = File(pcmDir, "${deck.side.key}-${++pcmCounter}.pcm")
        deck.pcmFile = file
        val decoder = DjDecoder(
            file,
            onReady = { pcm ->
                scope.launch {
                    if (deck.loadId != ticket) return@launch
                    deck.voice.pcm = pcm
                    deck.duration = pcm.seconds
                    deck.loading = false
                    deck.voice.playing = deck.playing
                    if (deck.playing) engine.wake()
                }
            },
            onFinished = { pcm, audio ->
                scope.launch {
                    if (deck.loadId != ticket) return@launch
                    deck.duration = pcm.seconds
                    deck.scratchAudio = audio
                    deck.voice.scratchAudio = audio
                    analyse(deck, id, audio)
                }
            },
            onFailed = { scope.launch { if (deck.loadId == ticket) failed(deck, -3) } },
        )
        deck.decoder = decoder
        decoder.start(source, path)
    }

    private fun failed(deck: DjDeck, code: Int) {
        deck.loading = false
        deck.playing = false
        deck.voice.playing = false
        deck.keepsPlaying = false
        deck.failed = true
        if (deck.loaded) unplayable.add(deck.videoId)
        val label = deck.side.label
        show(
            when (code) {
                2 -> "Deck $label: that YouTube link is not valid."
                100 -> "Deck $label: that video was removed or is private."
                101, 150, 152, 153 -> "Deck $label: the owner does not allow this video to be embedded."
                -1 -> "YouTube could not be reached. Check your connection."
                -2 -> "Deck $label: YouTube did not offer a stream for this video."
                -3 -> "Deck $label: this video's audio could not be decoded."
                else -> "Deck $label: YouTube could not play this video."
            },
        )
    }

    private fun cachedWave(id: String): List<DjWaveBar>? =
        File(waves, "$id.bars").takeIf { it.isFile }?.let { runCatching { DjWaveCache.decode(it.readBytes()) }.getOrNull() }?.takeIf { it.isNotEmpty() }

    private fun analyse(deck: DjDeck, id: String, audio: DjScratchAudio?) {
        if (audio == null) return
        val wantsBars = deck.waveform.isEmpty()
        if (wantsBars) deck.reading = true
        scope.launch {
            val bars = if (wantsBars) withContext(Dispatchers.Default) { bars(audio) } else null
            if (bars != null && bars.isNotEmpty()) withContext(Dispatchers.IO) { runCatching { File(waves, "$id.bars").writeBytes(DjWaveCache.encode(bars)) } }
            if (deck.videoId != id) return@launch
            deck.reading = false
            if (bars != null && bars.isNotEmpty()) deck.waveform = bars
            if (deck.bpm == 0.0) detectBpm(deck.side, quietly = true)
        }
    }

    private fun bars(audio: DjScratchAudio): List<DjWaveBar> {
        val count = DjWaveAccumulator.bars(audio.duration)
        val accumulator = DjWaveAccumulator(audio.rate, audio.samples.size, count)
        val chunk = FloatArray(8192)
        var at = 0
        while (at < audio.samples.size) {
            val n = min(chunk.size, audio.samples.size - at)
            for (index in 0 until n) chunk[index] = audio.samples[at + index] / 32768f
            accumulator.add(chunk, n)
            at += n
        }
        return accumulator.finish(count)
    }

    fun detectBpm(side: DjDeckSide, quietly: Boolean = false) {
        val deck = deck(side)
        val id = deck.videoId
        val audio = deck.scratchAudio
        if (audio == null || id.isEmpty()) {
            if (!quietly) show("Deck ${side.label}'s audio is not read yet: tap the BPM box on the beat instead.")
            return
        }
        scope.launch {
            val found = withContext(Dispatchers.Default) { DjBpm.detect(audio) }
            if (deck.videoId != id || (quietly && deck.bpm != 0.0)) return@launch
            if (found == null) {
                if (!quietly) show("No steady beat found on Deck ${side.label}: tap the BPM box on the beat instead.")
                return@launch
            }
            deck.bpm = found
            deck.tapTempo = DjTapTempo()
            save()
        }
    }

    private fun retitle(deck: DjDeck, title: String, author: String) {
        if (deck.title == title && deck.author == author) return
        deck.title = title
        deck.author = author
        if (history.any { it.id == deck.videoId }) history = history.map { if (it.id == deck.videoId) DjHistoryItem.of(it.id, title, author) else it }
        if (library.any { it.id == deck.videoId }) library = DjStudio.retitled(library, deck.videoId, title, author)
        if (crate.track(deck.videoId) != null) {
            updateCrate { it.keep(deck.videoId, title, author) }
        } else {
            refreshSmart()
        }
        save()
    }

    fun eject(side: DjDeckSide) {
        val deck = deck(side)
        sectionJobs[side]?.cancel()
        unload(deck)
        deck.cue = 0.0
        deck.bpm = 0.0
        deck.loop = false
        deck.sample = DjSampleStep.Idle
        for (index in sections.indices) if (sections[index]?.deck == side) sections[index] = null
        save()
        share(side)
        notifyPlayback()
    }

    private fun startTicking() {
        tickJob = scope.launch {
            while (isActive) {
                delay(if (active || anyPlaying) 60 else 400)
                tick()
            }
        }
    }

    private fun tick() {
        val now = SystemClock.elapsedRealtime()
        for (deck in listOf(a, b)) {
            val voice = deck.voice
            if (deck.playing && voice.ended && replay(deck)) continue
            if (deck.playing && voice.ended) {
                voice.ended = false
                deck.playing = false
                deck.keepsPlaying = false
                deck.counted = false
                deck.stoppedAt = deck.duration
                notifyPlayback()
            }
            if (deck.loop && deck.playing && voice.position >= deck.cue + DjStudio.LOOP_SECONDS) voice.seek(deck.cue)
            aimRepeat(deck)
        }
        if (anyPlaying && now - savedAt > 5000) save(true)
        if (anyPlaying != lastPlaying) notifyPlayback()
    }

    internal fun notifyPlayback() {
        lastPlaying = anyPlaying
        DjMediaService.update(context, anyPlaying)
    }

    fun start(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded) {
            openLoad(side)
            return
        }
        if (deck.failed) {
            prepare(deck, deck.videoId, deck.title, deck.author, deck.position(), true)
        }
        if (!deck.playing) play(deck)
    }

    fun pauseDeck(side: DjDeckSide) {
        val deck = deck(side)
        if (deck.playing || deck.keepsPlaying) pause(deck)
        holdAutomix()
    }

    fun stop(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded) return
        sectionJobs[side]?.cancel()
        if (deck.playing || deck.keepsPlaying) pause(deck)
        rewind(deck)
        holdAutomix()
    }

    fun restart(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded) {
            openLoad(side)
            return
        }
        sectionJobs[side]?.cancel()
        rewind(deck)
        if (!deck.playing) play(deck)
    }

    private fun rewind(deck: DjDeck, to: Double = 0.0) {
        deck.repeatJob?.cancel()
        deck.repeatJob = null
        deck.counted = false
        deck.voice.seek(to)
        deck.stoppedAt = to
        share(deck.side)
    }

    private fun replay(deck: DjDeck): Boolean {
        if (!deck.repeats || !deck.keepsPlaying || !deck.loaded || deck.failed || deck.scratching) return false
        sectionJobs[deck.side]?.cancel()
        rewind(deck, DjRepeat.span(deck.waveform, deck.duration)?.start ?: 0.0)
        play(deck)
        return true
    }

    private fun aimRepeat(deck: DjDeck) {
        if (deck.repeatJob?.isActive == true) return
        if (!deck.repeats || deck.loop || !deck.playing || !deck.keepsPlaying || deck.scratching || deck.failed || deck.voice.pcm == null) return
        val span = DjRepeat.span(deck.waveform, deck.duration) ?: return
        when (val step = DjRepeat.step(deck.position(), span, deck.effectiveRate)) {
            DjRepeatStep.Wait -> Unit
            DjRepeatStep.Wrap -> wrap(deck, span.start)
            is DjRepeatStep.After -> {
                val id = deck.videoId
                deck.repeatJob = scope.launch {
                    delay((step.seconds * 1000).toLong())
                    deck.repeatJob = null
                    if (deck.videoId == id && deck.repeats && deck.playing && !deck.scratching) wrap(deck, span.start)
                }
            }
        }
    }

    private fun wrap(deck: DjDeck, start: Double) {
        sectionJobs[deck.side]?.cancel()
        rewind(deck, start)
        count(deck)
    }

    fun toggleRepeat(side: DjDeckSide) {
        val deck = deck(side)
        deck.repeats = !deck.repeats
        if (!deck.repeats) {
            deck.repeatJob?.cancel()
            deck.repeatJob = null
        }
        show(if (deck.repeats) "Deck ${side.label} repeats its song." else "Deck ${side.label} repeat off.")
        save()
    }

    fun play(deck: DjDeck) {
        if (!deck.loaded) return
        deck.voice.ended = false
        if (deck.duration > 0 && deck.voice.position >= deck.duration - 0.05) deck.voice.seek(0.0)
        deck.voice.rate = deck.effectiveRate
        deck.voice.playing = deck.voice.pcm != null
        engine.wake()
        deck.playing = true
        deck.keepsPlaying = true
        count(deck)
        remember(deck)
        share(deck.side)
        save()
        notifyPlayback()
    }

    fun pause(deck: DjDeck) {
        deck.voice.playing = false
        deck.playing = false
        deck.keepsPlaying = false
        deck.stoppedAt = deck.voice.position
        share(deck.side)
        save()
        notifyPlayback()
    }

    private fun remember(deck: DjDeck) {
        if (!deck.loaded) return
        val updated = DjStudio.remember(history, deck.videoId, deck.title, deck.author)
        if (updated == history) return
        history = updated
    }

    private fun count(deck: DjDeck) {
        if (!deck.loaded || deck.counted) return
        deck.counted = true
        library = DjStudio.played(library, deck.videoId, deck.title, deck.author, System.currentTimeMillis() / 1000.0)
        refreshSmart()
    }

    fun plays(id: String): Int = library.firstOrNull { it.id == id }?.plays ?: 0

    fun forgetPlayed(id: String) {
        history = history.filter { it.id != id }
        library = library.filter { it.id != id }
        refreshSmart()
        save()
    }

    fun clearHistory() {
        history = emptyList()
        library = emptyList()
        refreshSmart()
        save()
    }

    fun setCue(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded) return
        deck.cue = deck.position()
        show("Deck ${side.label} cue set at ${DjStudio.clock(deck.cue)}.")
        save()
    }

    fun jumpToCue(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded) return
        deck.voice.seek(deck.cue)
        deck.stoppedAt = deck.cue
        share(side)
    }

    fun cueStop(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded) return
        if (deck.playing || deck.keepsPlaying) pause(deck)
        jumpToCue(side)
        holdAutomix()
    }

    fun seek(side: DjDeckSide, seconds: Double) {
        val deck = deck(side)
        if (!deck.loaded || deck.duration <= 0) return
        val target = min(deck.duration, max(0.0, seconds))
        deck.voice.seek(target)
        deck.stoppedAt = target
        share(side)
    }

    fun setRate(side: DjDeckSide, rate: Double) {
        val deck = deck(side)
        val clamped = DjStudio.clampRate(rate)
        if (abs(clamped - deck.rate) <= 0.0001) return
        deck.rate = clamped
        deck.voice.rate = deck.effectiveRate
        save()
        share(side)
    }

    fun touchBegan(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded || !deck.playing || deck.scratching) return
        deck.brakeJob?.cancel()
        deck.brakeStart = SystemClock.elapsedRealtime()
        deck.brake = DjStudio.TOUCH_BRAKE
        deck.voice.rate = deck.effectiveRate
    }

    fun touchEnded(side: DjDeckSide) {
        val deck = deck(side)
        if (deck.brake == 0.0) return
        val left = (DjStudio.TOUCH_BRAKE_SECONDS * 1000).toLong() - (SystemClock.elapsedRealtime() - deck.brakeStart)
        deck.brakeJob?.cancel()
        deck.brakeJob = scope.launch {
            if (left > 0) delay(left)
            releaseBrake(deck)
        }
    }

    private fun releaseBrake(deck: DjDeck) {
        deck.brakeJob?.cancel()
        deck.brakeJob = null
        if (deck.brake == 0.0) return
        deck.brake = 0.0
        deck.voice.rate = deck.effectiveRate
    }

    fun bend(side: DjDeckSide, direction: Double) {
        val deck = deck(side)
        deck.bend = direction * DjStudio.BEND_AMOUNT
        deck.voice.rate = deck.effectiveRate
    }

    fun sync(side: DjDeckSide) {
        val deck = deck(side)
        val other = deck(side.other)
        if (!deck.loaded || !other.loaded) {
            show("Load both decks to sync.")
            return
        }
        deck.rate = DjStudio.syncedRate(deck.bpm, other.bpm, other.effectiveRate)
        deck.voice.rate = deck.effectiveRate
        share(side)
        show(
            if (deck.bpm > 0 && other.bpm > 0) "Deck ${side.label} matched to ${"%.1f".format(other.shownBpm)} BPM."
            else "Deck ${side.label} matched Deck ${side.other.label}'s tempo. Tap both BPM boxes to beat-match.",
        )
        save()
    }

    fun tapTempo(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded) return
        val bpm = deck.tapTempo.tap(SystemClock.elapsedRealtime() / 1000.0)
        if (bpm != null) {
            deck.bpm = bpm / max(0.01, deck.effectiveRate)
            save()
        } else if (deck.bpm == 0.0) {
            show("Keep tapping on the beat to set Deck ${side.label}'s BPM.")
        }
    }

    fun clearBpm(side: DjDeckSide) {
        deck(side).bpm = 0.0
        deck(side).tapTempo = DjTapTempo()
        save()
    }

    fun toggleLoop(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded) return
        deck.loop = !deck.loop
        val at = deck.position()
        if (deck.loop && (at > deck.cue + DjStudio.LOOP_SECONDS || at < deck.cue)) deck.cue = at
        show(if (deck.loop) "Deck ${side.label} loops 8 seconds from ${DjStudio.clock(deck.cue)}." else "Deck ${side.label} loop off.")
        save()
    }

    fun setLevel(side: DjDeckSide, level: Double) {
        deck(side).level = min(1.0, max(0.0, level))
        applyMix()
        save()
    }

    fun scratchBegan(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded || deck.duration <= 0 || deck.scratching) return
        releaseBrake(deck)
        deck.resumeAfterScratch = deck.playing
        deck.scratchAt = deck.voice.position
        deck.voice.playing = false
        deck.voice.beginScratch(deck.scratchAt)
        deck.scratching = true
        engine.wake()
    }

    fun scratchMoved(side: DjDeckSide, radians: Double) {
        val deck = deck(side)
        if (!deck.scratching) return
        val seconds = DjStudio.scratchSeconds(radians)
        deck.scratchAt = min(deck.duration, max(0.0, deck.scratchAt + seconds))
        deck.voice.scratchGoal = deck.scratchAt
    }

    fun scratchEnded(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.scratching) return
        deck.voice.endScratch()
        deck.voice.seek(deck.scratchAt)
        deck.scratching = false
        deck.stoppedAt = deck.scratchAt
        deck.playing = deck.resumeAfterScratch
        deck.keepsPlaying = deck.resumeAfterScratch
        deck.voice.playing = deck.resumeAfterScratch && deck.voice.pcm != null
        engine.wake()
        share(side)
    }

    fun setCrossfader(value: Double, cancelAutomix: Boolean = true, saving: Boolean = true) {
        if (cancelAutomix && automixing) {
            automixJob?.cancel()
            automixing = false
        }
        val clamped = min(1.0, max(-1.0, value))
        if (clamped == crossfader) return
        crossfader = clamped
        applyMix()
        if (saving) save()
    }

    internal fun applyMix() {
        for (deck in listOf(a, b)) deck.voice.gain = (DjStudio.gain(deck.side, crossfader) * deck.level).toFloat()
    }

    fun slam(side: DjDeckSide) {
        automixing = false
        fade(if (side == DjDeckSide.A) -1.0 else 1.0, 0.18, null)
    }

    fun automix() {
        val autoDj = autoDjOn
        setAutomix(!automixOn)
        val focus = crate.label(crate.focus) + if (crate.shuffle) ", shuffled" else " in order"
        show(
            when {
                !automixOn -> if (autoDj) "AUTOMIX and AUTO DJ off." else "AUTOMIX off."
                crate.focus != DjFocus.Everything -> if (shownQueue.isEmpty()) "AUTOMIX on: playing $focus." else "AUTOMIX on: the queue, then $focus."
                shownQueue.isNotEmpty() -> "AUTOMIX on: the queue plays one song after another."
                library.isEmpty() && crate.tracks.isEmpty() -> "AUTOMIX on: queue songs and they play one after another."
                else -> "AUTOMIX on: with the queue empty it shuffles the songs you have played."
            },
        )
    }

    fun setAutomix(on: Boolean) {
        automixLoop?.cancel()
        automixLoop = null
        if (!on) {
            automixJob?.cancel()
            automixing = false
            stopAutoDj()
        }
        automixOn = on
        if (!on) return
        automixLoop = scope.launch {
            while (isActive) {
                automixStep()
                delay(500)
            }
        }
    }

    fun holdAutomix() {
        if (!automixOn || anyPlaying) return
        val autoDj = autoDjOn
        setAutomix(false)
        show(if (autoDj) "AUTOMIX and AUTO DJ off." else "AUTOMIX off.")
    }

    private fun automixStep() {
        if (!automixOn || automixing || a.scratching || b.scratching) return
        val favoured = if (crossfader <= 0) DjDeckSide.A else DjDeckSide.B
        if (autoDjOn) primeAutoDj()
        val picked = autoDjOn && autoDjNext() != null
        val pooled = !(autoDjOn && autoDjFinding) && crate.pool(library, unplayable).isNotEmpty()
        val queued = shownQueue.isNotEmpty() || picked || pooled
        when (val step = DjAutomix.step(snapshot(a), snapshot(b), favoured, queued)) {
            DjAutomixStep.Wait -> Unit
            is DjAutomixStep.Preload -> preload(step.side)
            is DjAutomixStep.Start -> {
                play(deck(step.side))
                fade(if (step.side == DjDeckSide.A) -1.0 else 1.0, 0.4, null)
            }
            is DjAutomixStep.Mix -> mix(deck(step.from), deck(step.to), step.seconds)
        }
    }

    internal fun preload(side: DjDeckSide): Boolean {
        val item = shownQueue.firstOrNull()
        if (item != null) {
            playFromQueue(item, side)
            return true
        }
        if (autoDjOn) {
            val pick = takeAutoDj()
            if (pick != null) {
                load(side, pick.id, pick.title, pick.author)
                show("AUTO DJ: ${pick.title} is up next.")
                return true
            }
        }
        val song = replayed() ?: return false
        load(side, song.id, song.title, song.author)
        return true
    }

    fun shakeMix() {
        if (sheet != null || automixing || a.scratching || b.scratching) return
        val playing = listOf(a, b).filter { it.playing }
        val live = if (playing.size > 1) deck(if (crossfader <= 0) DjDeckSide.A else DjDeckSide.B) else playing.firstOrNull()
        if (live == null) {
            show("Start a deck, then shake to mix into the next song.")
            return
        }
        val incoming = deck(live.side.other)
        if (!incoming.playing && (autoDjOn && incoming.counted || !snapshot(incoming).fresh) && !preload(incoming.side) && !snapshot(incoming).fresh) {
            show("Queue a song to shake into it.")
            return
        }
        DjHaptics.tick(context)
        show(if (incoming.title.isEmpty()) "Mixing into Deck ${incoming.side.label}." else "Mixing into ${incoming.title}.")
        mix(live, incoming, DjStudio.AUTOMIX_SECONDS)
    }

    fun quickNext(side: DjDeckSide) {
        val deck = deck(side)
        if (deck.scratching) return
        var resume = deck.playing || deck.keepsPlaying
        val other = deck(side.other)
        if (automixing) {
            automixJob?.cancel()
            automixing = false
            if (other.playing) {
                resume = false
                fade(if (other.side == DjDeckSide.A) -1.0 else 1.0, 0.4, null)
            }
        }
        if (!preload(side)) {
            show(if (crate.focus == DjFocus.Everything) "Nothing to load next: queue a song or play a few first." else "Nothing else in ${crate.label(crate.focus)} to load next.")
            return
        }
        if (resume) play(deck)
    }

    fun deleteSong(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded) return
        val id = deck.videoId
        val name = deck.title.ifEmpty { "the song" }
        removeSong(id)
        if (!inJam && queue.any { it.videoId == id }) {
            queue = queue.filter { it.videoId != id }
            save()
        }
        if (automixOn && deck.playing && skipOut(deck)) {
            show("Deleted $name. Mixing into the next song.")
            return
        }
        val resume = deck.playing || deck.keepsPlaying
        if (preload(side)) {
            if (resume) play(deck)
            show("Deleted $name. Loaded the next song.")
        } else {
            eject(side)
            show("Deleted $name.")
        }
    }

    fun openSimilar(side: DjDeckSide) {
        val deck = deck(side)
        if (!deck.loaded) {
            openLoad(side)
            return
        }
        val other = deck(side.other)
        searchTarget = if (other.playing) DjLoadTarget.QUEUE else DjLoadTarget.of(side.other)
        sheet = DjSheet.Similar(side)
        findSimilar(song(deck.videoId))
    }

    fun findSimilar(seed: DjTrack) {
        if (similarSeed == seed.id && (similarResults.isNotEmpty() || similarLoading)) return
        similarJob?.cancel()
        similarSeed = seed.id
        similarResults = emptyList()
        similarProblem = ""
        similarLoading = true
        similarJob = scope.launch {
            var found = invidious({ DjStudio.similarUrl(it, seed.id) }, DjStudio::similarResults)
            if (found.isNullOrEmpty()) {
                val query = DjSmart.searchQuery(seed)
                if (query.length >= 2) fetchResults(query)?.let { found = it }
            }
            if (similarSeed != seed.id) return@launch
            similarLoading = false
            val results = found.orEmpty().filter { it.id != seed.id }
            similarResults = results
            similarProblem = when {
                found == null -> "YouTube suggestions are unavailable right now."
                results.isEmpty() -> "YouTube has no suggestions for this song."
                else -> ""
            }
        }
    }

    private fun replayed(): DjPlayedSong? {
        val decks = listOf(a, b).filter { it.loaded }
        var picked: DjPlayedSong? = null
        updateCrate(saving = crate.focus != DjFocus.Everything) {
            picked = it.next(library, decks.map { d -> d.videoId }.toSet(), decks.filter { d -> d.playing }.map { d -> d.videoId }.toSet(), unplayable, Random.nextLong())
        }
        return picked
    }

    fun refocusAutomix() {
        if (!automixOn || automixing || shownQueue.isNotEmpty() || crate.focus == DjFocus.Everything) return
        val pool = crate.pool(library, unplayable).map { it.id }.toSet()
        val favoured = if (crossfader <= 0) DjDeckSide.A else DjDeckSide.B
        val idle = listOf(deck(favoured), deck(favoured.other)).filter { !it.playing && !it.scratching && it.videoId !in pool }
        for (deck in idle) if (pool.size > 1 || deck.side == favoured || anyPlaying) preload(deck.side)
    }

    internal fun skipOut(deck: DjDeck): Boolean {
        if (automixing || deck.scratching) return false
        val other = deck(deck.side.other)
        if (!deck.playing) {
            if (!preload(deck.side)) eject(deck.side)
            return true
        }
        if (other.playing) {
            pause(deck)
            fade(if (other.side == DjDeckSide.A) -1.0 else 1.0, 0.4, null)
            if (!preload(deck.side)) eject(deck.side)
            return true
        }
        val pool = crate.pool(library, unplayable).map { it.id }.toSet()
        if (!(snapshot(other).fresh && other.videoId in pool) && !preload(other.side)) {
            pause(deck)
            return false
        }
        mix(deck, other, 2.0, replacing = true)
        return true
    }

    private fun mix(outgoing: DjDeck, incoming: DjDeck, seconds: Double, replacing: Boolean = false) {
        automixing = true
        if (!incoming.playing) play(incoming)
        automixJob?.cancel()
        val began = SystemClock.elapsedRealtime()
        automixJob = scope.launch {
            while (isActive && !incoming.voice.audible && SystemClock.elapsedRealtime() - began < DjAutomix.AUDIBLE * 1000) delay(20)
            val left = snapshot(outgoing).remaining ?: seconds
            runFade(if (incoming.side == DjDeckSide.B) 1.0 else -1.0, max(2.0, min(seconds, left)))
            automixing = false
            if (outgoing.playing) pause(outgoing)
            if (replacing && !preload(outgoing.side)) eject(outgoing.side)
        }
    }

    internal fun snapshot(deck: DjDeck): DjAutomixDeck =
        DjAutomixDeck(deck.loaded && !deck.failed, deck.playing, deck.position(), deck.duration, deck.effectiveRate, deck.repeats)

    fun mixNow() {
        if (automixing) {
            automixJob?.cancel()
            automixing = false
            return
        }
        val target = if (crossfader <= 0) DjDeckSide.B else DjDeckSide.A
        val incoming = deck(target)
        val outgoing = deck(target.other)
        if (!incoming.loaded) {
            show("Load Deck ${target.label} to mix into it.")
            openLoad(target)
            return
        }
        if (!incoming.playing) play(incoming)
        automixing = true
        fade(if (target == DjDeckSide.B) 1.0 else -1.0, DjStudio.AUTOMIX_SECONDS) {
            automixing = false
            if (outgoing.playing) pause(outgoing)
        }
    }

    private fun fade(end: Double, seconds: Double, then: (() -> Unit)?) {
        automixJob?.cancel()
        automixJob = scope.launch {
            runFade(end, seconds)
            then?.invoke()
        }
    }

    private suspend fun runFade(end: Double, seconds: Double) {
        val start = crossfader
        val began = SystemClock.elapsedRealtime()
        while (true) {
            val progress = min(1.0, (SystemClock.elapsedRealtime() - began) / (seconds * 1000))
            val eased = progress * progress * (3 - 2 * progress)
            setCrossfader(start + (end - start) * eased, cancelAutomix = false, saving = false)
            if (progress >= 1) {
                save()
                return
            }
            delay(16)
        }
    }

    fun sample(side: DjDeckSide) {
        val deck = deck(side)
        when (val step = deck.sample) {
            DjSampleStep.Idle -> {
                if (!deck.loaded || deck.duration <= 0) {
                    show("Load Deck ${side.label} before sampling it.")
                    return
                }
                deck.sample = DjSampleStep.Recording(deck.position())
                show("Sampling Deck ${side.label}. Tap SAMPLE again to stop.")
            }
            is DjSampleStep.Recording -> {
                val section = DjStudio.sampleSection(side, step.start, deck.position())
                if (section == null) {
                    deck.sample = DjSampleStep.Idle
                    show("Nothing was sampled. Play Deck ${side.label} while sampling.")
                    return
                }
                val slot = sectionCursor
                sections[slot] = section
                sectionCursor = (slot + 1) % DjStudio.SECTION_COUNT
                deck.sample = DjSampleStep.Armed(slot)
                show("Sample armed: ${"%.1f".format(section.duration)} s from ${DjStudio.clock(section.start)}. Tap again to play it.")
                save()
            }
            is DjSampleStep.Armed -> {
                deck.sample = DjSampleStep.Idle
                playSection(step.slot)
            }
        }
    }

    fun playSection(index: Int) {
        val section = sections.getOrNull(index) ?: return
        val deck = deck(section.deck)
        if (!deck.loaded) return
        val keepPlaying = deck.playing
        deck.voice.seek(section.start)
        play(deck)
        sectionJobs[section.deck]?.cancel()
        sectionJobs[section.deck] = scope.launch {
            delay((section.duration * 1000).toLong())
            if (keepPlaying || !deck.playing || abs(deck.position() - section.start - section.duration) >= 2) return@launch
            pause(deck)
        }
    }

    fun clearSection(index: Int) {
        if (index !in sections.indices) return
        sections[index] = null
        sectionCursor = index
        for (deck in listOf(a, b)) if (deck.sample == DjSampleStep.Armed(index)) deck.sample = DjSampleStep.Idle
        save()
    }

    fun playPad(pad: DjDrumPad) {
        engine.pad(pad)
        flashedPad = pad
        scope.launch {
            delay(110)
            if (flashedPad == pad) flashedPad = null
        }
    }

    fun changeSamplerLevel(level: Double) {
        samplerLevel = min(1.0, max(0.0, level))
        engine.samplerLevel = samplerLevel.toFloat()
        save()
    }

    fun openLoad(side: DjDeckSide) = openLoader(DjLoadTarget.of(side))

    fun openLoader(target: DjLoadTarget) {
        searchTarget = target
        searchProblem = ""
        sheet = DjSheet.Load(target)
    }

    fun search() {
        val query = searchText.trim()
        if (loadLink(query)) {
            sheet = null
            return
        }
        if (query.length < 2) {
            searchProblem = "Enter at least two characters, or paste a YouTube link."
            return
        }
        searchJob?.cancel()
        searching = true
        searchProblem = ""
        val target = searchTarget
        searchJob = scope.launch {
            val results = fetchResults(query)
            searching = false
            if (results == null) {
                searchProblem = "YouTube search is unavailable right now. Paste a YouTube link to load it directly."
                return@launch
            }
            searchResults = results
            val top = results.firstOrNull()
            if (top == null) {
                searchProblem = "No YouTube videos matched that search."
                return@launch
            }
            searchTarget = target
            place(top.id, top.title, top.author)
            if (sheet == DjSheet.Load(target)) sheet = null
        }
    }

    internal suspend fun fetchResults(query: String): List<DjSearchResult>? =
        invidious({ DjStudio.searchUrl(it, query) }, DjStudio::searchResults) ?: withContext(Dispatchers.IO) {
            runCatching {
                val response = HttpClient.Request.get(DjStudio.pageSearchUrl(query))
                    .header("User-Agent", DESKTOP_AGENT).header("Accept-Language", "en").timeouts(10_000, 15_000).send()
                if (response.ok) DjStudio.pageResults(response.text()) else null
            }.getOrNull()
        }

    private suspend fun invidious(address: (String) -> String?, parse: (String) -> List<DjSearchResult>): List<DjSearchResult>? =
        invidiousData(address)?.let { parse(it.first) }

    internal suspend fun invidiousData(address: (String) -> String?): Pair<String, String>? = withContext(Dispatchers.IO) {
        if (searchInstances.isEmpty()) {
            searchInstances = runCatching {
                val response = HttpClient.Request.get("https://api.invidious.io/instances.json?sort_by=health").timeouts(6_000, 8_000).send()
                if (response.ok) DjStudio.searchInstances(response.text()) else emptyList()
            }.getOrDefault(emptyList())
        }
        for (instance in searchInstances) {
            val url = address(instance) ?: continue
            val response = runCatching { HttpClient.Request.get(url).timeouts(6_000, 8_000).send() }.getOrNull()
            if (response != null && response.ok) {
                searchInstances = listOf(instance) + searchInstances.filter { it != instance }
                return@withContext response.text() to instance
            }
        }
        searchInstances = emptyList()
        null
    }

    internal fun updateCrate(saving: Boolean = true, change: (DjCrate) -> Unit) {
        val copy = crate.copy()
        change(copy)
        crate = copy
        if (saving) saveCrate()
    }

    fun saveCrate() {
        refreshSmart()
        val text = crate.encoded()
        saver.execute { prefs?.putString(DjCrate.STORAGE_KEY, text) }
    }

    fun refreshSmart() {
        val copy = crate.copy()
        copy.categorize(librarySongs, library, System.currentTimeMillis() / 1000.0)
        crate = copy
    }

    fun save(now: Boolean = false) {
        saveJob?.cancel()
        if (now) {
            write()
            return
        }
        saveJob = scope.launch {
            delay(400)
            write()
        }
    }

    private fun write() {
        savedAt = SystemClock.elapsedRealtime()
        var saved = DjSaved(
            crossfader = crossfader, samplerLevel = samplerLevel, sections = sections.toList(), history = history,
            queue = queue, automix = automixOn, library = library, autoDj = autoDjOn,
        )
        for (deck in listOf(a, b)) {
            saved = saved.with(
                deck.side,
                DjSavedDeck(
                    deck.videoId, deck.title, deck.author, deck.cue, deck.level, deck.rate, deck.bpm, deck.loop,
                    deck.position(), deck.keepsPlaying && deck.loaded, deck.repeats,
                ),
            )
        }
        val text = saved.encoded()
        saver.execute { prefs?.putString(DjStudio.STORAGE_KEY, text) }
    }

    fun flush() {
        save(true)
        runCatching { saver.submit {}.get() }
    }

    fun enqueueLocal(id: String, title: String, author: String) {
        if (queue.size >= DjStudio.QUEUE_LIMIT) {
            show("The queue is full. Play or remove a video first.")
            return
        }
        queue = queue + DjQueueItem.of(UUID.randomUUID().toString(), id, title, author, addedAt = System.currentTimeMillis())
        show(if (title.isEmpty()) "Added to the queue." else "Queued: $title")
        save()
    }

    companion object {
        const val JAM_KEY = "fm.videoMixDjStudio.jam"
        const val MOTION_KEY = "fm.videoMixDjStudio.shareMotion"
        const val PACKAGE = "video-mix-dj-studio"
        const val DESKTOP_AGENT = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/133.0.0.0 Safari/537.36"
    }
}

internal object DjImages {
    private val cache = object : android.util.LruCache<String, ImageBitmap>(24 * 1024 * 1024) {
        override fun sizeOf(key: String, value: ImageBitmap): Int = value.width * value.height * 4
    }

    fun cached(url: String): ImageBitmap? = cache.get(url)

    fun load(url: String): ImageBitmap? {
        cache.get(url)?.let { return it }
        val bytes = runCatching { HttpClient.Request.get(url).timeouts(8_000, 15_000).send() }.getOrNull()?.takeIf { it.ok }?.body ?: return null
        val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: return null
        return bitmap.asImageBitmap().also { cache.put(url, it) }
    }

    fun artwork(id: String): ImageBitmap? {
        val key = "art:$id"
        cache.get(key)?.let { return it }
        for (quality in listOf("maxresdefault", "hqdefault")) {
            val bytes = runCatching { HttpClient.Request.get(DjStudio.thumbnailUrl(id, quality)).timeouts(8_000, 15_000).send() }
                .getOrNull()?.takeIf { it.ok }?.body ?: continue
            val bitmap = BitmapFactory.decodeByteArray(bytes, 0, bytes.size) ?: continue
            if (bitmap.width <= 200) continue
            val top = if (quality == "hqdefault") bitmap.height * 0.125f else 0f
            val height = if (quality == "hqdefault") bitmap.height * 0.75f else bitmap.height.toFloat()
            val edge = min(bitmap.width.toFloat(), height).toInt()
            val x = ((bitmap.width - edge) / 2f).toInt()
            val y = (top + (height - edge) / 2f).toInt()
            val square = Bitmap.createBitmap(bitmap, x, y, edge, edge)
            val scaled = if (edge > 640) Bitmap.createScaledBitmap(square, 640, 640, true) else square
            return scaled.asImageBitmap().also { cache.put(key, it) }
        }
        return null
    }
}
