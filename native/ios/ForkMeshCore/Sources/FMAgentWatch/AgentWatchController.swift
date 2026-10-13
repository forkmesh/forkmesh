// What the Watch screen follows and how each beat becomes a note, a mark and a
// touch. The phone's Watch screen and the watch app both drive one of these;
// neither polls (see `ring()`).
//
// Time comes from an `AgentWatchClock`, so tests (and the watch, which may
// want its own) can drive it by hand. Sound and touch go out through an
// `AgentWatchOutput`, which the platform implements.

import FMBase
import Foundation
import Observation

// MARK: - What is watched

/// What a run looks like at one reading: its status and every beat so far.
public struct AgentWatchSnapshot: Sendable {
    public let status: String
    public let beats: [WatchBeat]

    public init(status: String, beats: [WatchBeat]) {
        self.status = status
        self.beats = beats
    }
}

/// One agent run the Watch screen follows, and how to read it again.
public struct AgentWatchTarget: Sendable {
    /// Unique across everything being watched: "relay:owner/name#12".
    public let key: String
    public let id: Int

    /// The task it was given, as the agent list titles it.
    public let title: String

    /// Where it runs: a repository, a desktop.
    public let subtitle: String

    /// The project it works in ("forkmesh"): written over the top of its body,
    /// and what its voice is seeded from, as on the desktop.
    public let project: String

    /// Its runtime and model, which pick the face drawn in its body.
    public let provider: String
    public let model: String

    /// What it was asked.
    public let prompt: String

    /// Its usage when watching began, when the list knows it: turns climb its
    /// line up the scale, tokens swell its body and fill its sound out.
    public let turns: Int
    public let tokens: Int

    /// Reads the run afresh. Called once when watching starts and again on
    /// each `ring()` while the run is live.
    public let load: @Sendable () async throws -> AgentWatchSnapshot

    public init(
        key: String,
        id: Int,
        title: String,
        subtitle: String = "",
        project: String = "",
        provider: String = "",
        model: String = "",
        prompt: String = "",
        turns: Int = 0,
        tokens: Int = 0,
        load: @escaping @Sendable () async throws -> AgentWatchSnapshot
    ) {
        self.key = key
        self.id = id
        self.title = title
        self.subtitle = subtitle
        self.project = project
        self.provider = provider
        self.model = model
        self.prompt = prompt
        self.turns = turns
        self.tokens = tokens
        self.load = load
    }

    /// The project, or the last path part of the subtitle.
    public var projectName: String {
        if !project.isEmpty { return project }
        guard let slash = subtitle.range(of: "/", options: .backwards) else { return subtitle }
        return String(subtitle[slash.upperBound...])
    }
}

/// The only states a picture carries (the desktop's `State`, bar Idle).
public enum WatchState: Int, CaseIterable, Sendable {
    case starting, running, waiting, done, failed

    /// Running, waiting on you, or about to start.
    public var isLive: Bool { self == .running || self == .waiting || self == .starting }
}

/// A status string ("running", "waiting_for_input", "failed", ...) as a state.
public func watchStateFor(_ status: String) -> WatchState {
    let clean = status.lowercased().replacingOccurrences(of: "_", with: "-")
    switch clean {
    case "running": return .running
    case "queued", "": return .starting
    case "waiting", "waiting-for-input": return .waiting
    case "failed", "error": return .failed
    default: return .done
    }
}

/// The rows the field seats agents in, top to bottom (the desktop's `Band`).
public enum WatchBand: Int, CaseIterable, Sendable {
    case working, waiting, resting
}

public func bandFor(_ state: WatchState) -> WatchBand {
    switch state {
    case .running, .starting: return .working
    case .waiting: return .waiting
    case .done, .failed: return .resting
    }
}

public func bandLabel(_ band: WatchBand) -> String {
    switch band {
    case .working: return "working"
    case .waiting: return "waiting on you"
    case .resting: return "idle or finished"
    }
}

/// A body's glow decays with this half-life (`Scene::kEnergyHalfLife`).
public let kEnergyHalfLife: Double = 1.6

/// How long a mark stays in the ring round its body (`kHistorySeconds`).
public let kHistorySeconds: Double = 90.0

/// Roughly how late the speaker is behind a buffer handed to it: what a heard
/// mark waits on top of its note, so it lands when the note is heard.
public let kSpeakerLatency: Double = 0.06

/// Two marks of one kind off one body this close together sound as one chord
/// and are drawn as one (`kMergeWindow`).
private let mergeWindow: Double = 0.03

public let kTagSeconds: Double = 2.4

public let kRankSeconds: Double = 4.0
public let kActivitySeconds: Double = 30.0

public let kTrafficSeconds: Double = 10.0

// MARK: - Output and time

/// Where sound and touch go. The phone's is AgentWatchOutput.swift in the app
/// (AVAudioEngine and Core Haptics); tests hand the controller a recorder.
@MainActor
public protocol AgentWatchOutput: AnyObject {
    /// Sounds `notes`, placed on the watcher's clock, where `now` is that
    /// clock's reading at the call: a note due at `now + 0.3` is heard 0.3 s
    /// from now. Overlapping calls mix. Render with `renderNotes(_:from:)`.
    func play(_ notes: [Note], now: Double)

    /// Plays a haptic pattern, cutting off any still playing.
    func feel(_ pulses: [Pulse])

    /// Silences everything at once: the screen closed, or the app went away.
    func stop()
}

/// An output that drops everything (a preview, or no speaker at all).
@MainActor
public final class SilentAgentWatchOutput: AgentWatchOutput {
    public init() {}
    public func play(_ notes: [Note], now: Double) {}
    public func feel(_ pulses: [Pulse]) {}
    public func stop() {}
}

/// A pending call on an `AgentWatchClock`.
@MainActor
public protocol AgentWatchTimer: AnyObject {
    func cancel()
}

/// The watcher's clock: seconds since watching began, and calls after a
/// delay. Everything is on the main actor.
@MainActor
public protocol AgentWatchClock: AnyObject {
    var now: Double { get }
    @discardableResult
    func after(_ seconds: Double, _ action: @escaping @MainActor () -> Void) -> AgentWatchTimer
}

/// The real clock: a monotonic reading and Task-based delays.
@MainActor
public final class SystemAgentWatchClock: AgentWatchClock {
    private let origin = DispatchTime.now().uptimeNanoseconds

    public init() {}

    public var now: Double { Double(DispatchTime.now().uptimeNanoseconds &- origin) / 1e9 }

    @discardableResult
    public func after(_ seconds: Double, _ action: @escaping @MainActor () -> Void) -> AgentWatchTimer {
        let timer = TaskTimer()
        let nanos = UInt64(max(0, seconds) * 1e9)
        timer.task = Task { @MainActor [weak timer] in
            try? await Task.sleep(nanoseconds: nanos)
            guard let timer, !timer.cancelled, !Task.isCancelled else { return }
            action()
        }
        return timer
    }

    private final class TaskTimer: AgentWatchTimer {
        var task: Task<Void, Never>?
        var cancelled = false

        func cancel() {
            cancelled = true
            task?.cancel()
        }
    }
}

/// A clock tests (and previews) advance by hand: nothing happens until
/// `advance(by:)`, which runs every call that falls due, in time order.
@MainActor
public final class ManualAgentWatchClock: AgentWatchClock {
    public private(set) var now: Double
    private var queue: [Entry] = []
    private var sequence = 0

    public init(now: Double = 0) { self.now = now }

    @discardableResult
    public func after(_ seconds: Double, _ action: @escaping @MainActor () -> Void) -> AgentWatchTimer {
        sequence += 1
        let entry = Entry(due: now + max(0, seconds), order: sequence, action: action)
        queue.append(entry)
        return entry
    }

    /// Moves time on by `seconds`, running what falls due on the way.
    public func advance(by seconds: Double) {
        let target = now + seconds
        while true {
            queue.removeAll { $0.cancelled }
            guard let next = queue.min(by: { ($0.due, $0.order) < ($1.due, $1.order) }), next.due <= target else {
                break
            }
            queue.removeAll { $0 === next }
            now = max(now, next.due)
            next.action()
        }
        now = target
    }

    /// Calls still waiting.
    public var pending: Int { queue.filter { !$0.cancelled }.count }

    private final class Entry: AgentWatchTimer {
        let due: Double
        let order: Int
        let action: @MainActor () -> Void
        var cancelled = false

        init(due: Double, order: Int, action: @escaping @MainActor () -> Void) {
            self.due = due
            self.order = order
            self.action = action
        }

        func cancel() { cancelled = true }
    }
}

// MARK: - The field's model

/// One agent on the field (the desktop's `Body`, with its voice).
@MainActor
public final class WatchedAgent {
    public let target: AgentWatchTarget

    /// Its seat in the mix; re-rolled when it would clash with a neighbour.
    public internal(set) var voice: Voice
    var melody = Melody()

    public internal(set) var state: WatchState = .starting
    public internal(set) var loaded = false
    public internal(set) var error = ""

    /// In the mix: heard as well as seen.
    public internal(set) var listening = true

    /// Every beat read so far, whether played yet or not.
    public internal(set) var seen: [WatchBeat] = []
    var pending: [WatchBeat] = []

    /// The beat played last: what it is doing now.
    public internal(set) var last: WatchBeat?

    public internal(set) var events = 0
    var turnsSeen = 0
    var bytesSeen = 0
    private var energy = 0.0
    private var energyAt = 0.0
    public internal(set) var lastEventSeconds = -1e9
    public internal(set) var lastNoteSeconds = -1e9

    /// What it last said to you, and when: the display under its body.
    public internal(set) var said = ""
    public internal(set) var saidSeconds = -1e9

    /// What it was asked, and the latest steer if that is something else.
    public internal(set) var prompt = ""
    public internal(set) var lastPrompt = ""

    /// Every mark it has made, by kind: what its card's key counts.
    public internal(set) var kinds: [AgentMotion: Int] = [:]
    var cadenceRung = false

    /// 0..1, how present it is on the screen, mid-fade included.
    public internal(set) var presence = 1.0

    public internal(set) var tag: AgentMotion?
    public internal(set) var tagSeconds = -1e9

    public internal(set) var rank = 0

    var drawn: (x: Double, y: Double, at: Double)?

    public init(_ target: AgentWatchTarget, _ voice: Voice) {
        self.target = target
        self.voice = voice
    }

    public var id: Int { target.id }
    public var hue: Double { voice.hue }
    public var turns: Int { max(target.turns, turnsSeen) }
    public var tokens: Int { max(target.tokens, bytesSeen / 4) }

    /// Beats read but not yet played.
    public var pendingCount: Int { pending.count }

    /// 0..1 from the tokens burned.
    public var size: Double { voice.bloom }

    /// 0..1: jumps on every note and decays.
    public func energyAt(_ now: Double) -> Double {
        energy * pow(0.5, max(0, now - energyAt) / kEnergyHalfLife)
    }

    func bump(_ now: Double, _ by: Double) {
        energy = min(1.0, energyAt(now) + by)
        energyAt = now
    }

    /// Worth showing with "working only" on: running, or it did something
    /// recently enough that its ripples are still settling.
    public func working(_ now: Double) -> Bool {
        state.isLive || now - lastEventSeconds < 30.0 || now - lastNoteSeconds < 30.0
    }

    /// Sets the state directly (tests and previews).
    public func setState(_ state: WatchState) { self.state = state }
}

/// A mark on the field: one note of one agent (the desktop's `Pulse`). Born
/// when its note is heard, which may be a moment after it was asked for.
@MainActor
public final class WatchPulse {
    public let agent: WatchedAgent
    public let motion: AgentMotion
    public let born: Double
    public let seconds: Double

    /// 0..1, from how loud its note is.
    public internal(set) var strength: Double

    /// 0..1 round its body, fanned so two never overlay.
    public let angle: Double
    public internal(set) var heard: Bool
    public let text: String
    public let seed: Int
    var struck = false

    init(
        agent: WatchedAgent, motion: AgentMotion, born: Double, seconds: Double, strength: Double, angle: Double,
        heard: Bool = false, text: String = "", seed: Int = 0
    ) {
        self.agent = agent
        self.motion = motion
        self.born = born
        self.seconds = seconds
        self.strength = strength
        self.angle = angle
        self.heard = heard
        self.text = text
        self.seed = seed
    }

    /// Not landed yet: its note is still to sound.
    public func pending(_ now: Double) -> Bool { now < born }

    /// 0..1 through its life.
    public func life(_ now: Double) -> Double {
        let t = seconds > 0 ? (now - born) / seconds : 1.0
        return min(max(t, 0.0), 1.0)
    }

    public func expired(_ now: Double) -> Bool { now - born >= seconds }
}

/// A mark that landed: what the ring round a body is drawn from.
public struct WatchMark {
    public let agent: WatchedAgent
    public let motion: AgentMotion
    public let seconds: Double
    public let strength: Double
}

// MARK: - The controller

/// Follows a set of agent runs and plays each thing they do as the desktop's
/// Watch window does: a note on the shared grid in the agent's own voice, the
/// figure for that kind of work off its body landing on the note, and a touch
/// of the motor in its rhythm.
///
/// Nothing here polls. Runs are read once by `start()` and again only when
/// `ring()` is called (the relay's "sessions changed" bell, the local
/// network's `agents.changed`, the phone's own agent service), and the beats
/// each new reading adds are played out at a walking pace, round-robin across
/// agents, so a burst of twenty tool calls is a phrase rather than a chord.
///
/// Observe `revision` (it moves on every change); call `advance()` each frame
/// the field is drawn.
@MainActor
@Observable
public final class AgentWatchController {
    /// Bumped on every change a view shows.
    public private(set) var revision = 0

    @ObservationIgnored public let output: AgentWatchOutput
    @ObservationIgnored public let clock: AgentWatchClock
    @ObservationIgnored public let debounce: Double
    @ObservationIgnored public private(set) var agents: [WatchedAgent] = []

    /// Marks in flight, oldest first.
    @ObservationIgnored public private(set) var pulses: [WatchPulse] = []

    /// Every mark that landed in the last `kHistorySeconds`, oldest first.
    @ObservationIgnored public private(set) var history: [WatchMark] = []

    /// When each kind last sounded, and last landed heard or not, on `now`'s
    /// clock: what lights the list of sounds.
    @ObservationIgnored public private(set) var lastSounded: [AgentMotion: Double] = [:]
    @ObservationIgnored public private(set) var lastLanded: [AgentMotion: Double] = [:]
    @ObservationIgnored public private(set) var landCount: [AgentMotion: Int] = [:]
    @ObservationIgnored public private(set) var lastText: [AgentMotion: String] = [:]
    @ObservationIgnored public private(set) var lastTextSeconds: [AgentMotion: Double] = [:]

    /// Kinds whose sound is switched off; nothing of them is played.
    @ObservationIgnored public private(set) var muted: Set<AgentMotion> = []

    @ObservationIgnored public private(set) var bytesIn = 0
    @ObservationIgnored public private(set) var readsDone = 0
    @ObservationIgnored public private(set) var lastReadSeconds = -1e9
    @ObservationIgnored private var arrivals: [(seconds: Double, bytes: Int)] = []
    @ObservationIgnored private var rankedAt = -1e9

    @ObservationIgnored private var touches: [ObjectIdentifier: AgentWatchTimer] = [:]
    @ObservationIgnored private var reads: [Task<Void, Never>] = []
    @ObservationIgnored private var recheck: Set<String> = []
    @ObservationIgnored private var debounceTimer: AgentWatchTimer?
    @ObservationIgnored private var beatTimer: AgentWatchTimer?
    @ObservationIgnored private var roundRobin = 0
    @ObservationIgnored private var paused = false
    @ObservationIgnored private var disposed = false
    @ObservationIgnored private var started = false
    @ObservationIgnored private var presenceClock = -1.0
    @ObservationIgnored private var soundOn: Bool
    @ObservationIgnored private var feelOn: Bool
    @ObservationIgnored private var workingOnlyOn = false

    /// How many catch-up beats a run already under way plays when watching
    /// starts: enough to hear who it is, not a replay of its afternoon.
    public static let catchUp = 3

    /// - Parameters:
    ///   - targets: the runs to follow, seated in this order.
    ///   - output: where sound and touch go.
    ///   - clock: the watcher's clock; the system one by default.
    ///   - debounce: how long a burst of `ring()`s is gathered into one read.
    public init(
        targets: [AgentWatchTarget],
        output: AgentWatchOutput,
        clock: AgentWatchClock? = nil,
        debounce: Double = 0.8,
        sound: Bool = true,
        feel: Bool = true
    ) {
        self.output = output
        self.clock = clock ?? SystemAgentWatchClock()
        self.debounce = debounce
        self.soundOn = sound
        self.feelOn = feel
        for target in targets {
            agents.append(WatchedAgent(target, seat(target)))
        }
    }

    /// Seconds on the watcher's clock: what every note and mark is placed on.
    public var now: Double { clock.now }

    private func changed() { revision &+= 1 }

    /// "Hear everything": every agent in the mix.
    public var sound: Bool {
        get { soundOn }
        set {
            if soundOn == newValue { return }
            soundOn = newValue
            for agent in agents { agent.listening = newValue }
            if !newValue {
                output.stop()
            } else {
                let t = now
                for agent in agents { announceSeat(agent, t) }
            }
            changed()
        }
    }

    /// Haptics on or off.
    public var feel: Bool {
        get { feelOn }
        set {
            if feelOn == newValue { return }
            feelOn = newValue
            changed()
        }
    }

    /// Only the agents that are working; the rest fade out.
    public var workingOnly: Bool {
        get { workingOnlyOn }
        set {
            if workingOnlyOn == newValue { return }
            workingOnlyOn = newValue
            changed()
        }
    }

    /// Beats are waiting to be played.
    public var busy: Bool { agents.contains { !$0.pending.isEmpty } }

    /// Some run has not been read yet.
    public var loading: Bool { agents.contains { !$0.loaded } }

    /// Whether the filter shows this agent at all (the fade is only its look).
    public func shown(_ agent: WatchedAgent) -> Bool { !workingOnlyOn || agent.working(now) }

    /// Whether anything on the field is still moving: while false, a view may
    /// stop drawing frames until `revision` moves.
    public var moving: Bool {
        let t = now
        if busy || !pulses.isEmpty { return true }
        if t - lastReadSeconds < kTrafficSeconds && bytesPerSecond(t) > 0 { return true }
        for agent in agents {
            if agent.energyAt(t) > 0.01 || t - agent.tagSeconds < kTagSeconds { return true }
            let target = shown(agent) ? 1.0 : 0.0
            if abs(agent.presence - target) > 0.01 { return true }
        }
        return false
    }

    public func setMuted(_ motion: AgentMotion, _ value: Bool) {
        let did = value ? muted.insert(motion).inserted : muted.remove(motion) != nil
        if did { changed() }
    }

    /// Reads every run for the first time. Finished runs replay from the
    /// start; running ones play their last few beats and then follow along.
    public func start() async {
        if started { return }
        started = true
        if soundOn {
            let t = now
            for agent in agents { announceSeat(agent, t) }
        }
        let initial = agents.map { agent in Task { await self.read(agent, first: true) } }
        for read in initial { await read.value }
    }

    @discardableResult
    public func add(_ targets: [AgentWatchTarget]) -> Int {
        if disposed { return 0 }
        var seated = 0
        for target in targets {
            if let known = agents.first(where: { $0.target.key == target.key }) {
                if known.loaded && !known.state.isLive { recheck.insert(target.key) }
                continue
            }
            let agent = WatchedAgent(target, seat(target))
            agents.append(agent)
            seated += 1
            if started {
                if soundOn { announceSeat(agent, now) }
                reads.append(Task { await self.read(agent, first: true) })
            }
        }
        if seated > 0 { changed() }
        return seated
    }

    /// The doorbell: the runs may have moved on. A burst is gathered over
    /// `debounce` into one read of every live (or unread) run.
    public func ring() {
        if disposed || debounceTimer != nil { return }
        debounceTimer = clock.after(debounce) { [weak self] in
            guard let self else { return }
            self.debounceTimer = nil
            guard !self.disposed, !self.paused else { return }
            self.reads.removeAll()
            let recheck = self.recheck
            self.recheck.removeAll()
            for agent in self.agents where !agent.loaded || agent.state.isLive || recheck.contains(agent.target.key) {
                self.reads.append(Task { await self.read(agent) })
            }
        }
    }

    /// Waits for the reads the last `ring()` started (tests; a pull to refresh).
    public func settle() async {
        let pending = reads
        for read in pending { await read.value }
    }

    /// Stops sound and pacing while the app is away; nothing is lost, the
    /// beats wait in their queues.
    public func pause() {
        if paused { return }
        paused = true
        beatTimer?.cancel()
        beatTimer = nil
        debounceTimer?.cancel()
        debounceTimer = nil
        output.stop()
    }

    /// Picks up where `pause()` left off, and reads the live runs again.
    public func resume() {
        if !paused { return }
        paused = false
        scheduleBeats()
        ring()
    }

    /// Picked out: felt in the hand in its own rhythm, so a tapped agent can be
    /// told apart without looking.
    public func greet(_ agent: WatchedAgent) {
        if feelOn { output.feel(feelFor(.start, agent.voice)) }
    }

    /// In or out of the mix, as a double click does on the desktop. A voice
    /// coming in announces its seat.
    public func toggleListening(_ agent: WatchedAgent) {
        agent.listening.toggle()
        if agent.listening { announceSeat(agent, now) }
        changed()
    }

    /// Eases the field towards now: marks strike when their notes sound, old
    /// ones leave, bodies fade in and out. Call every frame the field paints.
    public func advance() {
        let t = now
        for pulse in pulses where !pulse.struck && !pulse.pending(t) {
            strike(pulse)
        }
        pulses.removeAll { $0.expired(t) }
        var stale = 0
        while stale < history.count && t - history[stale].seconds > kHistorySeconds {
            stale += 1
        }
        if stale > 0 { history.removeFirst(stale) }
        if t - rankedAt >= kRankSeconds { rerank(t) }
        // Presence eases towards shown or hidden over about 0.9 s.
        let step = presenceClock < 0 ? 1.0 : (t - presenceClock) / 0.9
        presenceClock = t
        for agent in agents {
            let target = shown(agent) ? 1.0 : 0.0
            if agent.presence < target {
                agent.presence = min(target, agent.presence + step)
            } else if agent.presence > target {
                agent.presence = max(target, agent.presence - step)
            }
        }
    }

    public func bytesPerSecond(_ now: Double) -> Double {
        let since = now - kTrafficSeconds
        let bytes = arrivals.reduce(0) { $0 + ($1.seconds >= since ? $1.bytes : 0) }
        return Double(bytes) / kTrafficSeconds
    }

    public func eventsPerMinute(_ now: Double) -> Int {
        var count = 0
        for mark in history.reversed() {
            if now - mark.seconds > 60 { break }
            if !mark.motion.isFaint { count += 1 }
        }
        return count
    }

    /// The kinds of mark one agent has made, most first.
    public func kindsOf(_ agent: WatchedAgent) -> [(AgentMotion, Int)] {
        agent.kinds.filter { $0.value > 0 }
            .map { ($0.key, $0.value) }
            .sorted { $0.1 != $1.1 ? $0.1 > $1.1 : $0.0.rawValue < $1.0.rawValue }
    }

    /// Stops everything for good: timers, touches, sound.
    public func dispose() {
        if disposed { return }
        disposed = true
        debounceTimer?.cancel()
        beatTimer?.cancel()
        for touch in touches.values { touch.cancel() }
        touches.removeAll()
        output.stop()
    }

    // MARK: Internals

    /// The two notes a voice announces its seat with.
    private func announceSeat(_ agent: WatchedAgent, _ t: Double) {
        if !agent.listening || muted.contains(.start) { return }
        let notes = announce(agent.voice, &agent.melody, t)
        schedule(agent, .start, notes, t, heard: true, text: "")
    }

    /// The voice for a newcomer: its own hash, re-rolled while it would be
    /// confused with one already seated, then moved to the emptier side of the
    /// room (`AgentSonifier::listen`).
    private func seat(_ target: AgentWatchTarget) -> Voice {
        let title = target.projectName.isEmpty ? target.title : target.projectName
        var voice = Voice.forAgent(target.id, title)
        for salt in 1...8 {
            let clash = agents.contains { other in
                other.voice.instrument == voice.instrument
                    && (other.voice.octave == voice.octave || other.voice.pan == voice.pan)
            }
            if !clash { break }
            voice = Voice.forAgent(target.id, title, salt: salt)
        }
        if !agents.isEmpty {
            var weight = 0.0
            for other in agents { weight += other.voice.pan - 0.5 }
            if abs(weight) > 1e-9 && (weight > 0) == (voice.pan > 0.5) {
                voice.pan = 1.0 - voice.pan
            }
        }
        voice.applyUsage(turns: target.turns, tokens: target.tokens)
        return voice
    }

    private func rerank(_ t: Double) {
        rankedAt = t
        var score: [ObjectIdentifier: Double] = [:]
        for mark in history where !mark.motion.isFaint {
            score[ObjectIdentifier(mark.agent), default: 0] += exp((mark.seconds - t) / kActivitySeconds)
        }
        let count = Double(max(agents.count, 1))
        let keyed = agents.map { (agent: WatchedAgent) -> (WatchedAgent, Double) in
            let held = 0.6 * (1.0 - Double(agent.rank) / count)
            return (agent, (score[ObjectIdentifier(agent)] ?? 0) + held)
        }
        let ordered = keyed.sorted { a, b in
            if a.1 != b.1 { return a.1 > b.1 }
            if a.0.rank != b.0.rank { return a.0.rank < b.0.rank }
            let ax = min(max(a.0.voice.pan, 0.0), 1.0)
            let bx = min(max(b.0.voice.pan, 0.0), 1.0)
            return ax != bx ? ax < bx : a.0.id < b.0.id
        }
        for (place, entry) in ordered.enumerated() {
            entry.0.rank = place
        }
    }

    private func read(_ agent: WatchedAgent, first: Bool = false) async {
        let snapshot: AgentWatchSnapshot
        do {
            snapshot = try await agent.target.load()
        } catch {
            if disposed { return }
            agent.loaded = true
            agent.error = "\(error)"
            changed()
            return
        }
        if disposed { return }
        let wasLive = agent.state.isLive
        let state = watchStateFor(snapshot.status)
        let beats = snapshot.beats
        let fresh: [WatchBeat]
        if first {
            fresh = state.isLive ? Array(beats.suffix(Self.catchUp)) : beats
            // What it was asked and last said, even from before the catch-up.
            for beat in beats {
                if beat.motion == .prompt && agent.prompt.isEmpty { agent.prompt = beat.text }
                if !beat.said.isEmpty { agent.said = beat.said }
            }
            if agent.prompt.isEmpty { agent.prompt = agent.target.prompt }
        } else {
            fresh = newBeats(agent.seen, beats)
        }
        agent.seen = beats
        let bytes = fresh.reduce(0) { $0 + $1.bytes }
        let t = now
        bytesIn += bytes
        readsDone += 1
        lastReadSeconds = t
        arrivals.append((t, bytes))
        arrivals.removeAll { t - $0.seconds > kTrafficSeconds }
        agent.loaded = true
        agent.error = ""
        agent.state = state
        agent.pending.append(contentsOf: fresh)
        // A desktop's transcript does not record its own ending; the status does.
        let ended = !state.isLive && (first || wasLive)
        if ended {
            agent.pending.append(
                state == .failed ? WatchBeat(.fail, "failed") : WatchBeat(.finish, "finished"))
        }
        scheduleBeats()
        changed()
    }

    private func scheduleBeats() {
        if paused || disposed || beatTimer != nil { return }
        let waiting = agents.reduce(0) { $0 + $1.pending.count }
        if waiting == 0 { return }
        // A slot of the grid apiece when there is little to say; quicker when a
        // lot has piled up, which the melody takes as grace notes.
        let ms = waiting > 24 ? 110 : waiting > 8 ? 190 : Int((kSlotSeconds * 1000).rounded())
        beatTimer = clock.after(Double(ms) / 1000) { [weak self] in
            guard let self else { return }
            self.beatTimer = nil
            if self.paused || self.disposed { return }
            self.playNext()
            self.scheduleBeats()
        }
    }

    private func playNext() {
        for i in 0..<agents.count {
            let agent = agents[(roundRobin + i) % agents.count]
            if agent.pending.isEmpty { continue }
            roundRobin = (roundRobin + i + 1) % agents.count
            let beat = agent.pending.removeFirst()
            // The run's last beat, once it has ended, is its ending: the cadence.
            let ending = agent.pending.isEmpty && !agent.state.isLive
                && (beat.motion == .finish || beat.motion == .fail)
            if ending {
                finish(agent, beat, agent.state != .failed)
            } else {
                emit(agent, beat)
            }
            changed()
            return
        }
    }

    /// One beat of one agent (`AgentSonifier::handleEvent` + `Scene::ingest`).
    private func emit(_ agent: WatchedAgent, _ beat: WatchBeat) {
        let t = now
        let motion = beat.motion
        if !motion.isBookkeeping {
            agent.events += 1
            agent.lastEventSeconds = t
            agent.last = beat
            agent.cadenceRung = false
        }
        if !beat.said.isEmpty {
            agent.said = beat.said
            agent.saidSeconds = t
        }
        if motion == .prompt {
            if agent.prompt.isEmpty {
                agent.prompt = beat.text
            } else if beat.text != agent.prompt {
                agent.lastPrompt = beat.text
            }
        }
        // Usage, as far as the transcript shows it: each reply or call a turn,
        // each few characters a token. The list's own figures win when bigger.
        if motion == .speak { agent.turnsSeen += 1 }
        agent.bytesSeen += beat.bytes
        let drift = agent.voice.drift
        agent.voice.applyUsage(turns: agent.turns, tokens: agent.tokens)
        agent.melody.degree -= agent.voice.drift - drift

        let note = noteForBeat(motion, agent.voice, &agent.melody, t, bytes: beat.bytes, seed: beat.seed)
        if note.isRest { return }
        let heard = soundOn && agent.listening && !muted.contains(motion)
        schedule(agent, motion, [note], t, heard: heard, text: beat.text, seed: beat.seed)
    }

    /// The run reached its end (`AgentSonifier::handleFinished` +
    /// `Scene::finish`): a cadence, and one big slow mark.
    private func finish(_ agent: WatchedAgent, _ beat: WatchBeat, _ success: Bool) {
        let t = now
        agent.last = beat
        agent.lastEventSeconds = t
        agent.bump(t, 0.5)
        let motion: AgentMotion = success ? .finish : .notice
        let pulse = WatchPulse(
            agent: agent, motion: motion, born: t, seconds: motion.seconds * 1.6, strength: 1.0, angle: 0,
            text: success ? "run finished" : "run failed or stopped")
        pulse.struck = true
        addPulse(pulse)
        land(pulse)
        if agent.cadenceRung { return }
        agent.cadenceRung = true
        let start = ((t / kSlotSeconds).rounded(.up) + 1) * kSlotSeconds
        let heard = soundOn && agent.listening && !muted.contains(motion)
        schedule(agent, motion, doneCadence(agent.voice, success, start), t, heard: heard, text: pulse.text)
        // Nothing else from this voice lands on top of the ending.
        agent.melody.lastSlot = (start + 2.6) / kSlotSeconds
        agent.melody.degree = 0
    }

    /// Plays `notes` (when `heard`) and draws a mark for each, landing when it
    /// sounds (`AgentSonifier::schedule` feeding `Scene::note`).
    private func schedule(
        _ agent: WatchedAgent, _ motion: AgentMotion, _ notes: [Note], _ t: Double, heard: Bool, text: String,
        seed: Int = 0
    ) {
        guard let first = notes.first else { return }
        if heard { output.play(notes, now: t) }
        let latency = heard ? kSpeakerLatency : 0.0
        for note in notes {
            placeNote(agent, motion, note.start - t + latency, note.gain, heard: heard, text: text, seed: seed)
        }
        if feelOn && !motion.isFaint {
            let delay = max(0.0, first.start - t + latency)
            let voice = agent.voice
            let holder = TouchKey()
            let key = ObjectIdentifier(holder)
            let timer = clock.after(delay) { [weak self] in
                _ = holder
                guard let self else { return }
                self.touches[key] = nil
                if !self.disposed && self.feelOn { self.output.feel(feelFor(motion, voice)) }
            }
            touches[key] = timer
        }
    }

    private final class TouchKey {}

    private func placeNote(
        _ agent: WatchedAgent, _ motion: AgentMotion, _ delay: Double, _ gain: Double, heard: Bool, text: String,
        seed: Int
    ) {
        let strength = min(max(gain / 0.3, 0.15), 1.0)
        let born = now + max(0.0, delay)
        for pulse in pulses {
            if pulse.agent !== agent || pulse.motion != motion { continue }
            if abs(born - pulse.born) > mergeWindow { continue }
            pulse.strength = min(1.0, pulse.strength + 0.25 * strength)
            if heard { pulse.heard = true }
            return
        }
        // The golden angle, so consecutive marks off one body fan out.
        let angle = (Double(agent.events) * 0.381966 + Self.jitter(agent.id, 3))
            .truncatingRemainder(dividingBy: 1.0)
        let pulse = WatchPulse(
            agent: agent, motion: motion, born: born, seconds: motion.seconds, strength: strength, angle: angle,
            heard: heard, text: text, seed: seed)
        addPulse(pulse)
        if !pulse.pending(now) { strike(pulse) }
    }

    private func addPulse(_ pulse: WatchPulse) {
        if pulses.count >= 400 { pulses.removeFirst() }
        pulses.append(pulse)
    }

    private func strike(_ pulse: WatchPulse) {
        pulse.struck = true
        land(pulse)
        let agent = pulse.agent
        if pulse.motion.isFaint {
            agent.bump(pulse.born, 0.04)
            return
        }
        agent.bump(pulse.born, 0.25 + 0.45 * pulse.strength)
        agent.lastNoteSeconds = pulse.born
    }

    private func land(_ pulse: WatchPulse) {
        let motion = pulse.motion
        lastLanded[motion] = max(lastLanded[motion] ?? -Double.infinity, pulse.born)
        landCount[motion, default: 0] += 1
        if !pulse.text.isEmpty {
            lastText[motion] = pulse.text
            lastTextSeconds[motion] = pulse.born
        }
        if pulse.heard {
            lastSounded[motion] = max(lastSounded[motion] ?? -Double.infinity, pulse.born)
        }
        pulse.agent.kinds[motion, default: 0] += 1
        if !motion.isFaint && pulse.born >= pulse.agent.tagSeconds {
            pulse.agent.tag = motion
            pulse.agent.tagSeconds = pulse.born
        }
        history.append(WatchMark(agent: pulse.agent, motion: motion, seconds: pulse.born, strength: pulse.strength))
        if history.count > 900 { history.removeFirst(history.count - 900) }
    }

    /// A stable 0..1 for an agent, to break ties the sound does not have to.
    static func jitter(_ id: Int, _ stir: Int) -> Double {
        var h = UInt64(bitPattern: Int64(id) &* 2_654_435_761 &+ Int64(stir) &* 0x9e37_79b9) & 0xffff_ffff
        h ^= h >> 15
        h = (h &* 0x2545_f491) & 0xffff_ffff
        h ^= h >> 13
        return Double(h % 10000) / 10000.0
    }
}
