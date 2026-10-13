package com.forkmesh.nativeapp.agentwatch

import kotlin.math.abs
import kotlin.math.ceil
import kotlin.math.floor
import kotlin.math.max
import kotlin.math.min
import kotlin.math.pow

class AgentWatchSnapshot(val status: String, val beats: List<WatchBeat>)

class AgentWatchTarget(
    val key: String,
    val id: Int,
    val title: String,
    val subtitle: String = "",
    val project: String = "",
    val provider: String = "",
    val model: String = "",
    val prompt: String = "",
    val turns: Int = 0,
    val tokens: Int = 0,
    val load: () -> AgentWatchSnapshot,
) {
    val projectName: String
        get() {
            if (project.isNotEmpty()) return project
            val slash = subtitle.lastIndexOf('/')
            return if (slash < 0) subtitle else subtitle.substring(slash + 1)
        }
}

enum class WatchState {
    starting, running, waiting, done, failed;

    val isLive: Boolean get() = this == running || this == waiting || this == starting
}

fun watchStateFor(status: String): WatchState = when (status.lowercase().replace("_", "-")) {
    "running" -> WatchState.running
    "queued", "" -> WatchState.starting
    "waiting", "waiting-for-input" -> WatchState.waiting
    "failed", "error" -> WatchState.failed
    else -> WatchState.done
}

enum class WatchBand { working, waiting, resting }

fun bandFor(state: WatchState): WatchBand = when (state) {
    WatchState.running, WatchState.starting -> WatchBand.working
    WatchState.waiting -> WatchBand.waiting
    WatchState.done, WatchState.failed -> WatchBand.resting
}

fun bandLabel(band: WatchBand): String = when (band) {
    WatchBand.working -> "working"
    WatchBand.waiting -> "waiting on you"
    WatchBand.resting -> "idle or finished"
}

const val kEnergyHalfLife: Double = 1.6
const val kHistorySeconds: Double = 90.0
const val kSpeakerLatency: Double = 0.06
private const val MERGE_WINDOW = 0.03

interface AgentWatchOutput {
    fun play(notes: List<Note>, now: Double)

    fun feel(pulses: List<Pulse>)

    fun stop()
}

class SilentAgentWatchOutput : AgentWatchOutput {
    override fun play(notes: List<Note>, now: Double) {}

    override fun feel(pulses: List<Pulse>) {}

    override fun stop() {}
}

fun interface AgentWatchTimer {
    fun cancel()
}

interface AgentWatchClock {
    val now: Double

    fun after(seconds: Double, action: () -> Unit): AgentWatchTimer
}

class ManualAgentWatchClock(now: Double = 0.0) : AgentWatchClock {
    private class Entry(val due: Double, val order: Int, val action: () -> Unit) : AgentWatchTimer {
        var cancelled = false

        override fun cancel() {
            cancelled = true
        }
    }

    override var now: Double = now
        private set
    private val queue = ArrayList<Entry>()
    private var sequence = 0

    override fun after(seconds: Double, action: () -> Unit): AgentWatchTimer {
        sequence += 1
        val entry = Entry(now + max(0.0, seconds), sequence, action)
        queue += entry
        return entry
    }

    fun advance(seconds: Double) {
        val target = now + seconds
        while (true) {
            queue.removeAll { it.cancelled }
            val next = queue.minWithOrNull(compareBy<Entry>({ it.due }, { it.order })) ?: break
            if (next.due > target) break
            queue.remove(next)
            now = max(now, next.due)
            next.action()
        }
        now = target
    }

    val pending: Int get() = queue.count { !it.cancelled }
}

fun interface AgentWatchLoader {
    fun load(target: AgentWatchTarget, done: (AgentWatchSnapshot?, Throwable?) -> Unit)
}

val immediateWatchLoader = AgentWatchLoader { target, done ->
    val snapshot = try {
        target.load()
    } catch (error: Throwable) {
        done(null, error)
        return@AgentWatchLoader
    }
    done(snapshot, null)
}

class WatchedAgent(val target: AgentWatchTarget, voice: Voice) {
    var voice: Voice = voice
        internal set
    internal var melody = Melody()
    var state: WatchState = WatchState.starting
        internal set
    var loaded = false
        internal set
    var error = ""
        internal set
    var listening = true
        internal set
    var seen: List<WatchBeat> = emptyList()
        internal set
    internal val pending = ArrayDeque<WatchBeat>()
    var last: WatchBeat? = null
        internal set
    var events = 0
        internal set
    internal var turnsSeen = 0
    internal var bytesSeen = 0
    private var energy = 0.0
    private var energyTime = 0.0
    var lastEventSeconds = -1e9
        internal set
    var lastNoteSeconds = -1e9
        internal set
    var said = ""
        internal set
    var saidSeconds = -1e9
        internal set
    var prompt = ""
        internal set
    var lastPrompt = ""
        internal set
    val kinds = HashMap<AgentMotion, Int>()
    internal var cadenceRung = false
    var presence = 1.0
        internal set

    val id: Int get() = target.id
    val hue: Double get() = voice.hue
    val turns: Int get() = max(target.turns, turnsSeen)
    val tokens: Int get() = max(target.tokens, bytesSeen / 4)
    val pendingCount: Int get() = pending.size
    val size: Double get() = voice.bloom

    fun energyAt(now: Double): Double = energy * 0.5.pow(max(0.0, now - energyTime) / kEnergyHalfLife)

    internal fun bump(now: Double, by: Double) {
        energy = min(1.0, energyAt(now) + by)
        energyTime = now
    }

    fun working(now: Double): Boolean = state.isLive || now - lastEventSeconds < 30.0 || now - lastNoteSeconds < 30.0

    fun setState(state: WatchState) {
        this.state = state
    }
}

class WatchPulse internal constructor(
    val agent: WatchedAgent,
    val motion: AgentMotion,
    val born: Double,
    val seconds: Double,
    strength: Double,
    val angle: Double,
    heard: Boolean = false,
    val text: String = "",
    val seed: Long = 0,
) {
    var strength: Double = strength
        internal set
    var heard: Boolean = heard
        internal set
    internal var struck = false

    fun pending(now: Double): Boolean = now < born

    fun life(now: Double): Double {
        val t = if (seconds > 0) (now - born) / seconds else 1.0
        return min(max(t, 0.0), 1.0)
    }

    fun expired(now: Double): Boolean = now - born >= seconds
}

class WatchMark(val agent: WatchedAgent, val motion: AgentMotion, val seconds: Double, val strength: Double)

class AgentWatchController(
    targets: List<AgentWatchTarget>,
    val output: AgentWatchOutput,
    val clock: AgentWatchClock,
    private val loader: AgentWatchLoader = immediateWatchLoader,
    val debounce: Double = 0.8,
    sound: Boolean = true,
    feel: Boolean = true,
) {
    var revision = 0
        private set
    private val listeners = ArrayList<() -> Unit>()
    val agents = ArrayList<WatchedAgent>()
    val pulses = ArrayList<WatchPulse>()
    val history = ArrayList<WatchMark>()
    val lastSounded = HashMap<AgentMotion, Double>()
    val lastLanded = HashMap<AgentMotion, Double>()
    val landCount = HashMap<AgentMotion, Int>()
    val lastText = HashMap<AgentMotion, String>()
    val lastTextSeconds = HashMap<AgentMotion, Double>()
    private val mutedSet = HashSet<AgentMotion>()
    val muted: Set<AgentMotion> get() = mutedSet

    private val touches = HashSet<AgentWatchTimer>()
    private var debounceTimer: AgentWatchTimer? = null
    private var beatTimer: AgentWatchTimer? = null
    private var roundRobin = 0
    private var paused = false
    private var disposed = false
    private var started = false
    private var presenceClock = -1.0
    private var soundOn = sound
    private var feelOn = feel
    private var workingOnlyOn = false

    init {
        for (target in targets) agents += WatchedAgent(target, seat(target))
    }

    val now: Double get() = clock.now

    fun addListener(listener: () -> Unit) {
        listeners += listener
    }

    fun removeListener(listener: () -> Unit) {
        listeners -= listener
    }

    private fun changed() {
        revision += 1
        for (listener in listeners.toList()) listener()
    }

    var sound: Boolean
        get() = soundOn
        set(value) {
            if (soundOn == value) return
            soundOn = value
            for (agent in agents) agent.listening = value
            if (!value) {
                output.stop()
            } else {
                val t = now
                for (agent in agents) announceSeat(agent, t)
            }
            changed()
        }

    var feel: Boolean
        get() = feelOn
        set(value) {
            if (feelOn == value) return
            feelOn = value
            changed()
        }

    var workingOnly: Boolean
        get() = workingOnlyOn
        set(value) {
            if (workingOnlyOn == value) return
            workingOnlyOn = value
            changed()
        }

    val busy: Boolean get() = agents.any { it.pending.isNotEmpty() }

    val loading: Boolean get() = agents.any { !it.loaded }

    fun shown(agent: WatchedAgent): Boolean = !workingOnlyOn || agent.working(now)

    val moving: Boolean
        get() {
            val t = now
            if (busy || pulses.isNotEmpty()) return true
            for (agent in agents) {
                if (agent.energyAt(t) > 0.01) return true
                val target = if (shown(agent)) 1.0 else 0.0
                if (abs(agent.presence - target) > 0.01) return true
            }
            return false
        }

    fun setMuted(motion: AgentMotion, value: Boolean) {
        val did = if (value) mutedSet.add(motion) else mutedSet.remove(motion)
        if (did) changed()
    }

    fun start() {
        if (started) return
        started = true
        if (soundOn) {
            val t = now
            for (agent in agents) announceSeat(agent, t)
        }
        for (agent in agents.toList()) read(agent, first = true)
    }

    fun add(targets: List<AgentWatchTarget>): Int {
        if (disposed) return 0
        var seated = 0
        for (target in targets) {
            if (agents.any { it.target.key == target.key }) continue
            val agent = WatchedAgent(target, seat(target))
            agents += agent
            seated++
            if (started) {
                if (soundOn) announceSeat(agent, now)
                read(agent, first = true)
            }
        }
        if (seated > 0) changed()
        return seated
    }

    fun ring() {
        debounceTimer?.cancel()
        debounceTimer = clock.after(debounce) {
            if (disposed || paused) return@after
            for (agent in agents.toList()) {
                if (!agent.loaded || agent.state.isLive) read(agent)
            }
        }
    }

    fun pause() {
        if (paused) return
        paused = true
        beatTimer?.cancel()
        beatTimer = null
        output.stop()
    }

    fun resume() {
        if (!paused) return
        paused = false
        scheduleBeats()
        ring()
    }

    fun greet(agent: WatchedAgent) {
        if (feelOn) output.feel(feelFor(AgentMotion.start, agent.voice))
    }

    fun toggleListening(agent: WatchedAgent) {
        agent.listening = !agent.listening
        if (agent.listening) announceSeat(agent, now)
        changed()
    }

    fun advance() {
        val t = now
        for (pulse in pulses.toList()) {
            if (!pulse.struck && !pulse.pending(t)) strike(pulse)
        }
        pulses.removeAll { it.expired(t) }
        var stale = 0
        while (stale < history.size && t - history[stale].seconds > kHistorySeconds) stale++
        if (stale > 0) history.subList(0, stale).clear()
        val step = if (presenceClock < 0) 1.0 else (t - presenceClock) / 0.9
        presenceClock = t
        for (agent in agents) {
            val target = if (shown(agent)) 1.0 else 0.0
            if (agent.presence < target) {
                agent.presence = min(target, agent.presence + step)
            } else if (agent.presence > target) {
                agent.presence = max(target, agent.presence - step)
            }
        }
    }

    fun kindsOf(agent: WatchedAgent): List<Pair<AgentMotion, Int>> =
        agent.kinds.filter { it.value > 0 }.map { it.key to it.value }
            .sortedWith(compareBy<Pair<AgentMotion, Int>>({ -it.second }, { it.first.ordinal }))

    fun dispose() {
        if (disposed) return
        disposed = true
        debounceTimer?.cancel()
        beatTimer?.cancel()
        for (touch in touches) touch.cancel()
        touches.clear()
        listeners.clear()
        output.stop()
    }

    private fun announceSeat(agent: WatchedAgent, t: Double) {
        if (!agent.listening || AgentMotion.start in mutedSet) return
        val notes = announce(agent.voice, agent.melody, t)
        schedule(agent, AgentMotion.start, notes, t, heard = true, text = "")
    }

    private fun seat(target: AgentWatchTarget): Voice {
        val title = target.projectName.ifEmpty { target.title }
        var voice = Voice.forAgent(target.id, title)
        for (salt in 1..8) {
            val clash = agents.any { other ->
                other.voice.instrument == voice.instrument &&
                    (other.voice.octave == voice.octave || other.voice.pan == voice.pan)
            }
            if (!clash) break
            voice = Voice.forAgent(target.id, title, salt)
        }
        if (agents.isNotEmpty()) {
            var weight = 0.0
            for (other in agents) weight += other.voice.pan - 0.5
            if (abs(weight) > 1e-9 && (weight > 0) == (voice.pan > 0.5)) voice.pan = 1.0 - voice.pan
        }
        voice.applyUsage(target.turns, target.tokens)
        return voice
    }

    private fun read(agent: WatchedAgent, first: Boolean = false) {
        loader.load(agent.target) { snapshot, error ->
            if (disposed) return@load
            if (snapshot == null) {
                agent.loaded = true
                agent.error = error?.message ?: error.toString()
                changed()
                return@load
            }
            apply(agent, snapshot, first)
        }
    }

    private fun apply(agent: WatchedAgent, snapshot: AgentWatchSnapshot, first: Boolean) {
        val wasLive = agent.state.isLive
        val state = watchStateFor(snapshot.status)
        val beats = snapshot.beats
        val fresh: List<WatchBeat>
        if (first) {
            fresh = if (state.isLive) beats.takeLast(CATCH_UP) else beats
            for (beat in beats) {
                if (beat.motion == AgentMotion.prompt && agent.prompt.isEmpty()) agent.prompt = beat.text
                if (beat.said.isNotEmpty()) agent.said = beat.said
            }
            if (agent.prompt.isEmpty()) agent.prompt = agent.target.prompt
        } else {
            fresh = newBeats(agent.seen, beats)
        }
        agent.seen = beats
        agent.loaded = true
        agent.error = ""
        agent.state = state
        agent.pending.addAll(fresh)
        val ended = !state.isLive && (first || wasLive)
        if (ended) {
            agent.pending.addLast(
                if (state == WatchState.failed) WatchBeat(AgentMotion.fail, "failed") else WatchBeat(AgentMotion.finish, "finished"),
            )
        }
        scheduleBeats()
        changed()
    }

    private fun scheduleBeats() {
        if (paused || disposed || beatTimer != null) return
        val waiting = agents.sumOf { it.pending.size }
        if (waiting == 0) return
        val ms = if (waiting > 24) 110 else if (waiting > 8) 190 else floor(kSlotSeconds * 1000 + 0.5).toInt()
        beatTimer = clock.after(ms / 1000.0) {
            beatTimer = null
            if (paused || disposed) return@after
            playNext()
            scheduleBeats()
        }
    }

    private fun playNext() {
        for (i in agents.indices) {
            val agent = agents[(roundRobin + i) % agents.size]
            if (agent.pending.isEmpty()) continue
            roundRobin = (roundRobin + i + 1) % agents.size
            val beat = agent.pending.removeFirst()
            val ending = agent.pending.isEmpty() && !agent.state.isLive &&
                (beat.motion == AgentMotion.finish || beat.motion == AgentMotion.fail)
            if (ending) finish(agent, beat, agent.state != WatchState.failed) else emit(agent, beat)
            changed()
            return
        }
    }

    private fun emit(agent: WatchedAgent, beat: WatchBeat) {
        val t = now
        val motion = beat.motion
        if (!motion.isBookkeeping) {
            agent.events += 1
            agent.lastEventSeconds = t
            agent.last = beat
            agent.cadenceRung = false
        }
        if (beat.said.isNotEmpty()) {
            agent.said = beat.said
            agent.saidSeconds = t
        }
        if (motion == AgentMotion.prompt) {
            if (agent.prompt.isEmpty()) agent.prompt = beat.text
            else if (beat.text != agent.prompt) agent.lastPrompt = beat.text
        }
        if (motion == AgentMotion.speak) agent.turnsSeen += 1
        agent.bytesSeen += beat.bytes
        val drift = agent.voice.drift
        agent.voice.applyUsage(agent.turns, agent.tokens)
        agent.melody.degree -= agent.voice.drift - drift
        val note = noteForBeat(motion, agent.voice, agent.melody, t, beat.bytes, beat.seed)
        if (note.isRest) return
        val heard = soundOn && agent.listening && motion !in mutedSet
        schedule(agent, motion, listOf(note), t, heard, beat.text, beat.seed)
    }

    private fun finish(agent: WatchedAgent, beat: WatchBeat, success: Boolean) {
        val t = now
        agent.last = beat
        agent.lastEventSeconds = t
        agent.bump(t, 0.5)
        val motion = if (success) AgentMotion.finish else AgentMotion.notice
        val pulse = WatchPulse(
            agent, motion, t, motion.seconds * 1.6, 1.0, 0.0,
            text = if (success) "run finished" else "run failed or stopped",
        )
        pulse.struck = true
        addPulse(pulse)
        land(pulse)
        if (agent.cadenceRung) return
        agent.cadenceRung = true
        val start = (ceil(t / kSlotSeconds) + 1) * kSlotSeconds
        val heard = soundOn && agent.listening && motion !in mutedSet
        schedule(agent, motion, doneCadence(agent.voice, success, start), t, heard, pulse.text)
        agent.melody.lastSlot = (start + 2.6) / kSlotSeconds
        agent.melody.degree = 0
    }

    private fun schedule(
        agent: WatchedAgent,
        motion: AgentMotion,
        notes: List<Note>,
        t: Double,
        heard: Boolean,
        text: String,
        seed: Long = 0,
    ) {
        val first = notes.firstOrNull() ?: return
        if (heard) output.play(notes, t)
        val latency = if (heard) kSpeakerLatency else 0.0
        for (note in notes) placeNote(agent, motion, note.start - t + latency, note.gain, heard, text, seed)
        if (feelOn && !motion.isFaint) {
            val delay = max(0.0, first.start - t + latency)
            val voice = agent.voice.copy()
            var timer: AgentWatchTimer? = null
            timer = clock.after(delay) {
                timer?.let { touches.remove(it) }
                if (!disposed && feelOn) output.feel(feelFor(motion, voice))
            }
            touches += timer
        }
    }

    private fun placeNote(
        agent: WatchedAgent,
        motion: AgentMotion,
        delay: Double,
        gain: Double,
        heard: Boolean,
        text: String,
        seed: Long,
    ) {
        val strength = min(max(gain / 0.3, 0.15), 1.0)
        val born = now + max(0.0, delay)
        for (pulse in pulses) {
            if (pulse.agent !== agent || pulse.motion != motion) continue
            if (abs(born - pulse.born) > MERGE_WINDOW) continue
            pulse.strength = min(1.0, pulse.strength + 0.25 * strength)
            if (heard) pulse.heard = true
            return
        }
        val angle = (agent.events * 0.381966 + jitter(agent.id, 3)).mod(1.0)
        val pulse = WatchPulse(agent, motion, born, motion.seconds, strength, angle, heard, text, seed)
        addPulse(pulse)
        if (!pulse.pending(now)) strike(pulse)
    }

    private fun addPulse(pulse: WatchPulse) {
        if (pulses.size >= 400) pulses.removeAt(0)
        pulses += pulse
    }

    private fun strike(pulse: WatchPulse) {
        pulse.struck = true
        land(pulse)
        val agent = pulse.agent
        if (pulse.motion.isFaint) {
            agent.bump(pulse.born, 0.04)
            return
        }
        agent.bump(pulse.born, 0.25 + 0.45 * pulse.strength)
        agent.lastNoteSeconds = pulse.born
    }

    private fun land(pulse: WatchPulse) {
        val motion = pulse.motion
        lastLanded[motion] = max(lastLanded[motion] ?: Double.NEGATIVE_INFINITY, pulse.born)
        landCount[motion] = (landCount[motion] ?: 0) + 1
        if (pulse.text.isNotEmpty()) {
            lastText[motion] = pulse.text
            lastTextSeconds[motion] = pulse.born
        }
        if (pulse.heard) lastSounded[motion] = max(lastSounded[motion] ?: Double.NEGATIVE_INFINITY, pulse.born)
        pulse.agent.kinds[motion] = (pulse.agent.kinds[motion] ?: 0) + 1
        history += WatchMark(pulse.agent, motion, pulse.born, pulse.strength)
        if (history.size > 900) history.subList(0, history.size - 900).clear()
    }

    companion object {
        const val CATCH_UP = 3

        fun jitter(id: Int, stir: Int): Double {
            var h = (id.toLong() * 2_654_435_761L + stir.toLong() * 0x9e3779b9L) and 0xffffffffL
            h = h xor (h ushr 15)
            h = (h * 0x2545f491L) and 0xffffffffL
            h = h xor (h ushr 13)
            return (h % 10000).toDouble() / 10000.0
        }
    }
}
