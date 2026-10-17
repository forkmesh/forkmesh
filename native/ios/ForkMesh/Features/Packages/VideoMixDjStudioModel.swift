import AVFoundation
import FMApi
import FMPackages
import Foundation
import QuartzCore
import SwiftUI
import UIKit

enum DjSheet: Identifiable, Equatable {
    case load(DjLoadTarget)
    case deck(DjDeckSide)
    case pads
    case info
    case jam

    var id: String {
        switch self {
        case let .load(side): return "load-\(side.rawValue)"
        case let .deck(side): return "deck-\(side.rawValue)"
        case .pads: return "pads"
        case .info: return "info"
        case .jam: return "jam"
        }
    }
}

@MainActor
@Observable
final class DjDeck {
    let side: DjDeckSide
    var videoId = ""
    var title = ""
    var author = ""
    var artwork: UIImage?
    var waveform: [DjWaveBar] = []
    var time: Double = 0
    var duration: Double = 0
    var playing = false {
        didSet { if oldValue, !playing { coast() } }
    }
    var coasting = false
    var reading = false
    var advancing = false
    var cue: Double = 0
    var level: Double = 1
    var rate: Double = 1
    var bend: Double = 0
    var bpm: Double = 0
    var loop = false
    var scratching = false
    var brake: Double = 0
    var sample: DjSampleStep = .idle
    var showsRemaining = false

    @ObservationIgnored var tickDate = CACurrentMediaTime()
    @ObservationIgnored var keepsPlaying = false
    @ObservationIgnored var tapTempo = DjTapTempo()
    @ObservationIgnored var resumeAfterScratch = false
    @ObservationIgnored var scratchStamp: CFTimeInterval = 0
    @ObservationIgnored var scratchSent: CFTimeInterval = 0
    @ObservationIgnored var scratchSpeed: Double = 0
    @ObservationIgnored var scratchTask: Task<Void, Never>?
    @ObservationIgnored var scratchAudio: DjScratchAudio?
    @ObservationIgnored var audioFor = ""
    @ObservationIgnored var vinyl = false
    @ObservationIgnored var settleUntil: CFTimeInterval = 0
    @ObservationIgnored var rateTask: Task<Void, Never>?
    @ObservationIgnored var brakeTask: Task<Void, Never>?
    @ObservationIgnored var brakeStart: CFTimeInterval = 0
    @ObservationIgnored var spin = DjSpin()
    @ObservationIgnored var coastTask: Task<Void, Never>?
    @ObservationIgnored let player: DjDeckPlayer

    init(side: DjDeckSide, host: DjPlayerHost) {
        self.side = side
        player = DjDeckPlayer(side: side, host: host)
    }

    var loaded: Bool { !videoId.isEmpty }
    var effectiveRate: Double { rate * (1 + bend) * (1 - brake) }
    var shownBpm: Double { bpm * effectiveRate }
    var moving: Bool { (playing || coasting) && !scratching }

    func position(at now: CFTimeInterval = CACurrentMediaTime()) -> Double {
        guard advancing, !scratching else { return time }
        let moved = time + (now - tickDate) * effectiveRate
        return duration > 0 ? min(duration, moved) : moved
    }

    func shown(at now: CFTimeInterval = CACurrentMediaTime()) -> Double {
        let target = position(at: now)
        guard moving else {
            spin.hold(target, at: now)
            return target
        }
        let goal = advancing ? effectiveRate : playing ? DjSpin.waiting * effectiveRate : 0
        return spin.step(target: target, speed: goal, at: now)
    }

    private func coast() {
        coasting = true
        coastTask?.cancel()
        coastTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(1.5))
            guard !Task.isCancelled else { return }
            self?.coasting = false
        }
    }

    func rebase(_ now: CFTimeInterval = CACurrentMediaTime()) {
        time = position(at: now)
        tickDate = now
    }

    func settle(_ now: CFTimeInterval = CACurrentMediaTime()) {
        settleUntil = now + 0.6
    }
}

@MainActor
@Observable
final class DjStudioHub {
    static let shared = DjStudioHub()
    private(set) var model: DjStudioModel?
    @ObservationIgnored private var relaunched = false

    func studio() -> DjStudioModel {
        if let model { return model }
        let made = DjStudioModel()
        model = made
        return made
    }

    func relaunch() {
        guard !relaunched else { return }
        relaunched = true
        guard PackageGate.allows("video-mix-dj-studio"), DjSaved.decode(UserDefaults.standard.data(forKey: DjStudio.storageKey)).playing else { return }
        studio().resumePlayback()
    }
}

@MainActor
@Observable
final class DjStudioModel {
    let a: DjDeck
    let b: DjDeck
    var crossfader: Double = 0
    var samplerLevel: Double = 0.8
    var sections: [DjSection?] = Array(repeating: nil, count: DjStudio.sectionCount)
    var automixing = false
    var automixOn = false
    var onScreen = false
    var notice: String?
    var sheet: DjSheet?
    var searchTarget: DjLoadTarget = .a
    var searchText = ""
    var searchResults: [DjSearchResult] = []
    var searching = false
    var searchProblem = ""
    var history: [DjHistoryItem] = []
    var flashedPad: DjDrumPad?
    var leavingAfterSheet = false
    var confirmingReset = false
    var resettingAfterSheet = false
    var queue: [DjQueueItem] = []
    var jam: DjJam?
    var jams: [DjJamSummary] = []
    var jamBusy = false
    var jamProblem = ""
    var inviteText = ""
    var inviteSuggestions: [String] = []
    var crowd: [DjJamPresence] = []
    var effects: [DjEffect] = []
    var reacting = false
    var shareMotion = true
    var queueDrag: DjQueueDrag?
    var draggedId: String?
    var dropSide: DjDeckSide?

    @ObservationIgnored var crowdDate = Date()
    @ObservationIgnored var deckFrames: [DjDeckSide: CGRect] = [:]
    @ObservationIgnored var seenReactions: [String] = []
    @ObservationIgnored var sentReactions: [CFTimeInterval] = []
    @ObservationIgnored let motion = DjMotionFeed()
    @ObservationIgnored var automixLoop: Task<Void, Never>?

    @ObservationIgnored var jamTask: Task<Void, Never>?
    @ObservationIgnored var shareTasks: [DjDeckSide: Task<Void, Never>] = [:]
    @ObservationIgnored var suggestTask: Task<Void, Never>?
    @ObservationIgnored var applied: [DjDeckSide: Int64] = [:]
    @ObservationIgnored var applyingRemote = false
    @ObservationIgnored private var analysed: Set<String> = []

    @ObservationIgnored var onLeave: (() -> Void)?
    @ObservationIgnored let host: DjPlayerHost
    @ObservationIgnored let defaults: UserDefaults
    @ObservationIgnored private let sampler = DjSampler()
    @ObservationIgnored private var sectionCursor = 0
    @ObservationIgnored private var searchInstances: [URL] = []
    @ObservationIgnored private var automixTask: Task<Void, Never>?
    @ObservationIgnored private var noticeTask: Task<Void, Never>?
    @ObservationIgnored private var saveTask: Task<Void, Never>?
    @ObservationIgnored private var searchTask: Task<Void, Never>?
    @ObservationIgnored private var sectionTasks: [DjDeckSide: Task<Void, Never>] = [:]
    @ObservationIgnored private var previousCategory: AVAudioSession.Category?
    @ObservationIgnored private var previousOptions: AVAudioSession.CategoryOptions = []
    @ObservationIgnored private var previousIdle = false
    @ObservationIgnored private var audioClaimed = false
    @ObservationIgnored private var active = false
    @ObservationIgnored private var savedAt: CFTimeInterval = 0
    @ObservationIgnored private var resumeAutomix = false

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        let host = DjPlayerHost(origin: DjStudioModel.origin())
        self.host = host
        a = DjDeck(side: .a, host: host)
        b = DjDeck(side: .b, host: host)
        let saved = DjSaved.decode(defaults.data(forKey: DjStudio.storageKey))
        crossfader = saved.crossfader
        samplerLevel = saved.samplerLevel
        sampler.level = saved.samplerLevel
        sections = saved.sections
        sectionCursor = sections.firstIndex(where: { $0 == nil }) ?? 0
        history = saved.history
        queue = saved.queue
        shareMotion = defaults.object(forKey: Self.motionKey) as? Bool ?? true
        resumeAutomix = saved.automix
        host.onEvent = { [weak self] side, event in
            guard let self else { return }
            self.handle(event, self.deck(side))
        }
        host.onReload = { [weak self] in self?.revive() }
        for deck in [a, b] {
            let stored = saved[deck.side]
            deck.cue = stored.cue
            deck.level = stored.level
            deck.rate = stored.rate
            deck.bpm = stored.bpm
            if !stored.videoId.isEmpty {
                prepare(deck, id: stored.videoId, title: stored.title, author: stored.author, start: stored.position, keepSettings: true)
                deck.keepsPlaying = stored.playing
            }
        }
        let center = NotificationCenter.default
        for name in [UIApplication.didEnterBackgroundNotification, UIApplication.willTerminateNotification] {
            center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.save(now: true) }
            }
        }
        center.addObserver(forName: UIApplication.willEnterForegroundNotification, object: nil, queue: .main) { [weak self] _ in
            MainActor.assumeIsolated { self?.comeBack() }
        }
        center.addObserver(forName: AVAudioSession.interruptionNotification, object: nil, queue: .main) { [weak self] note in
            let type = (note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt).flatMap(AVAudioSession.InterruptionType.init(rawValue:))
            let options = AVAudioSession.InterruptionOptions(rawValue: note.userInfo?[AVAudioSessionInterruptionOptionKey] as? UInt ?? 0)
            guard type == .ended, options.contains(.shouldResume) else { return }
            MainActor.assumeIsolated { self?.comeBack() }
        }
    }

    func resumePlayback() {
        for deck in [a, b] where deck.keepsPlaying && deck.loaded && !deck.playing { play(deck) }
        let automix = resumeAutomix
        resumeAutomix = false
        guard a.playing || b.playing else { return }
        if automix, !automixOn { setAutomix(true) }
        if defaults.string(forKey: Self.jamKey)?.isEmpty == false { resumeJam() }
    }

    private func comeBack() {
        for deck in [a, b] where deck.keepsPlaying && deck.loaded && !deck.playing && !deck.scratching { play(deck) }
    }

    private func revive() {
        let now = CACurrentMediaTime()
        for deck in [a, b] where deck.loaded {
            let at = deck.position(at: now)
            deck.time = at
            deck.tickDate = now
            deck.advancing = false
            deck.playing = deck.keepsPlaying
            deck.player.load(deck.videoId, start: at, rate: deck.effectiveRate, volume: volume(for: deck))
        }
    }

    private static func origin() -> URL {
        if let url = AppServices.current?.api.url("/"), url.scheme == "https", let host = url.host {
            var components = URLComponents()
            components.scheme = "https"
            components.host = host
            components.port = url.port
            components.path = "/"
            if let origin = components.url { return origin }
        }
        return URL(string: "https://forkmesh.com/")!
    }

    func deck(_ side: DjDeckSide) -> DjDeck { side == .a ? a : b }

    var audibleDeck: DjDeck? {
        [a, b].filter(\.playing).max { left, right in
            DjStudio.gain(left.side, crossfader: crossfader) * left.level < DjStudio.gain(right.side, crossfader: crossfader) * right.level
        }
    }

    func appear() {
        guard !active else { return }
        active = true
        onScreen = true
        claimAudio()
        FmCuePlayer.shared.quieten(true)
        previousIdle = UIApplication.shared.isIdleTimerDisabled
        UIApplication.shared.isIdleTimerDisabled = true
        FmOrientationLock.allowLandscape(true)
        host.dock()
        applyMix()
        resumeJam()
    }

    func disappear() {
        guard active else { return }
        active = false
        onScreen = false
        for deck in [a, b] where deck.scratching { scratchEnded(deck.side) }
        for deck in [a, b] where deck.brake != 0 { releaseBrake(deck) }
        sampler.stop()
        save(now: true)
        FmCuePlayer.shared.quieten(false)
        UIApplication.shared.isIdleTimerDisabled = previousIdle
        FmOrientationLock.allowLandscape(false)
        if !a.playing, !b.playing {
            setAutomix(false)
            releaseAudio()
        }
    }

    func leave() {
        disappear()
        onLeave?()
    }

    func stopAll() {
        setAutomix(false)
        for deck in [a, b] where deck.playing || deck.keepsPlaying { pause(deck) }
        if !active { releaseAudio() }
    }

    func resetAll() {
        setAutomix(false)
        searchTask?.cancel()
        for task in sectionTasks.values { task.cancel() }
        sectionTasks = [:]
        sampler.scratch(level: 0, speed: 0)
        sampler.stop()
        host.reset()
        let fresh = DjSaved()
        let now = CACurrentMediaTime()
        for deck in [a, b] {
            deck.scratchTask?.cancel()
            deck.scratchTask = nil
            deck.rateTask?.cancel()
            deck.rateTask = nil
            deck.brakeTask?.cancel()
            deck.brakeTask = nil
            deck.brake = 0
            deck.scratching = false
            deck.resumeAfterScratch = false
            deck.scratchSpeed = 0
            deck.videoId = ""
            deck.title = ""
            deck.author = ""
            deck.artwork = nil
            deck.waveform = []
            deck.scratchAudio = nil
            deck.audioFor = ""
            deck.reading = false
            deck.time = 0
            deck.duration = 0
            deck.playing = false
            deck.keepsPlaying = false
            deck.advancing = false
            deck.cue = fresh[deck.side].cue
            deck.level = fresh[deck.side].level
            deck.rate = fresh[deck.side].rate
            deck.bend = 0
            deck.bpm = fresh[deck.side].bpm
            deck.loop = false
            deck.sample = .idle
            deck.showsRemaining = false
            deck.tapTempo = DjTapTempo()
            deck.tickDate = now
            deck.settleUntil = 0
        }
        crossfader = fresh.crossfader
        samplerLevel = fresh.samplerLevel
        sampler.level = fresh.samplerLevel
        sections = fresh.sections
        sectionCursor = 0
        flashedPad = nil
        searchText = ""
        searchResults = []
        searching = false
        searchProblem = ""
        sheet = nil
        save(now: true)
        if !active { releaseAudio() }
        show("Studio reset. Load a song on each deck to start fresh.")
    }

    private func claimAudio() {
        guard !audioClaimed else { return }
        audioClaimed = true
        let session = AVAudioSession.sharedInstance()
        previousCategory = session.category
        previousOptions = session.categoryOptions
        try? session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try? session.setActive(true)
    }

    private func releaseAudio() {
        guard audioClaimed else { return }
        audioClaimed = false
        if let previousCategory {
            try? AVAudioSession.sharedInstance().setCategory(previousCategory, options: previousOptions)
        }
    }

    func show(_ text: String) {
        notice = text
        noticeTask?.cancel()
        noticeTask = Task { [weak self] in
            try? await Task.sleep(for: .seconds(2.4))
            guard !Task.isCancelled else { return }
            self?.notice = nil
        }
    }

    func load(_ side: DjDeckSide, id: String, title: String = "", author: String = "") {
        let deck = deck(side)
        prepare(deck, id: id, title: title, author: author, start: 0, keepSettings: false)
        show(title.isEmpty ? "Deck \(side.label) loaded." : "Deck \(side.label): \(title)")
        save()
        share(side)
    }

    func loadLink(_ text: String) -> Bool {
        let id = DjStudio.videoId(text)
        guard !id.isEmpty else { return false }
        place(id: id)
        return true
    }

    func place(id: String, title: String = "", author: String = "") {
        if let side = searchTarget.side {
            load(side, id: id, title: title, author: author)
        } else {
            enqueue(id: id, title: title, author: author)
        }
    }

    func prepare(_ deck: DjDeck, id: String, title: String, author: String, start: Double, keepSettings: Bool) {
        sectionTasks[deck.side]?.cancel()
        deck.videoId = id
        deck.title = title
        deck.author = author
        deck.artwork = nil
        deck.reading = false
        deck.scratchAudio = nil
        deck.audioFor = ""
        if let measured = DjWaveSource.cached(id) {
            deck.waveform = measured
            analysed.insert(id)
        } else {
            deck.waveform = []
        }
        deck.time = start
        deck.tickDate = CACurrentMediaTime()
        deck.duration = 0
        deck.playing = false
        deck.keepsPlaying = false
        deck.advancing = false
        if !keepSettings {
            deck.cue = 0
            deck.bpm = 0
            deck.loop = false
            deck.tapTempo = DjTapTempo()
        }
        deck.player.load(id, start: start, rate: deck.effectiveRate, volume: volume(for: deck))
        Task { [weak deck] in
            let image = await DjArtwork.load(id)
            guard let deck, deck.videoId == id else { return }
            deck.artwork = image
        }
    }

    func eject(_ side: DjDeckSide) {
        let deck = deck(side)
        sectionTasks[side]?.cancel()
        deck.player.stop()
        deck.videoId = ""
        deck.title = ""
        deck.author = ""
        deck.artwork = nil
        deck.waveform = []
        deck.scratchAudio = nil
        deck.audioFor = ""
        deck.reading = false
        deck.time = 0
        deck.duration = 0
        deck.playing = false
        deck.keepsPlaying = false
        deck.advancing = false
        deck.cue = 0
        deck.bpm = 0
        deck.loop = false
        deck.sample = .idle
        sections = sections.map { $0?.deck == side ? nil : $0 }
        save()
        share(side)
    }

    private func handle(_ event: DjPlayerEvent, _ deck: DjDeck) {
        switch event {
        case .ready:
            applyMix()
            deck.player.setRate(deck.effectiveRate)
            if deck.playing { deck.player.play() }
        case let .source(id, url):
            analyse(deck, id: id, source: url)
        case let .tick(time, duration, state):
            if duration > 0, abs(duration - deck.duration) > 0.5 { deck.duration = duration }
            guard !deck.scratching else { return }
            let now = CACurrentMediaTime()
            if now < deck.settleUntil {
                advance(state == 1 && deck.playing, deck)
                return
            }
            apply(state, deck)
            let continuous = deck.advancing && state == 1
            let tracked = DjStudio.trackedPosition(predicted: deck.position(at: now), reported: time, continuous: continuous)
            if abs(deck.time - tracked) > 0.0005 { deck.time = tracked }
            deck.tickDate = now
            advance(state == 1, deck)
            if deck.loop, deck.playing, deck.time >= deck.cue + DjStudio.loopSeconds {
                deck.player.seek(deck.cue)
                deck.time = deck.cue
                deck.tickDate = now
                deck.settle(now)
            }
            if deck.playing, now - savedAt > 5 { save(now: true) }
        case let .state(state):
            apply(state, deck)
            if state == 1 { advance(true, deck) }
        case let .meta(title, author):
            if deck.title != title || deck.author != author {
                deck.title = title
                deck.author = author
                if history.contains(where: { $0.id == deck.videoId }) {
                    history = history.map { $0.id == deck.videoId ? DjHistoryItem(id: $0.id, title: title, author: author) : $0 }
                }
                save()
            }
        case let .failed(code):
            deck.playing = false
            deck.keepsPlaying = false
            advance(false, deck)
            switch code {
            case 2: show("Deck \(deck.side.label): that YouTube link is not valid.")
            case 100: show("Deck \(deck.side.label): that video was removed or is private.")
            case 101, 150, 152, 153: show("Deck \(deck.side.label): the owner does not allow this video to be embedded.")
            case -1: show("YouTube could not be reached. Check your connection.")
            default: show("Deck \(deck.side.label): YouTube could not play this video.")
            }
        }
    }

    private func advance(_ advancing: Bool, _ deck: DjDeck) {
        guard deck.advancing != advancing, !deck.scratching else { return }
        deck.rebase()
        deck.advancing = advancing
    }

    private func apply(_ state: Int, _ deck: DjDeck) {
        guard !deck.scratching else { return }
        let playing = state == 1 || state == 3
        if deck.playing != playing { deck.playing = playing }
        if state != 1 { advance(false, deck) }
        if state == 0 { deck.keepsPlaying = false }
        if state == 0, deck.duration > 0 { deck.time = deck.duration }
    }

    func start(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded else {
            openLoad(side)
            return
        }
        if !deck.playing { play(deck) }
    }

    func pauseDeck(_ side: DjDeckSide) {
        let deck = deck(side)
        if deck.playing || deck.keepsPlaying { pause(deck) }
        holdAutomix()
    }

    func stop(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded else { return }
        sectionTasks[side]?.cancel()
        if deck.playing || deck.keepsPlaying { pause(deck) }
        rewind(deck)
        holdAutomix()
    }

    func restart(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded else {
            openLoad(side)
            return
        }
        sectionTasks[side]?.cancel()
        rewind(deck)
        if !deck.playing { play(deck) }
    }

    private func rewind(_ deck: DjDeck) {
        deck.player.seek(0)
        deck.time = 0
        deck.tickDate = CACurrentMediaTime()
        deck.settle()
        share(deck.side)
    }

    func play(_ deck: DjDeck) {
        host.dock()
        claimAudio()
        deck.rebase()
        deck.settle()
        deck.playing = true
        deck.keepsPlaying = true
        deck.player.play()
        remember(deck)
        share(deck.side)
        save()
    }

    private func analyse(_ deck: DjDeck, id: String, source: URL) {
        guard deck.videoId == id, deck.audioFor != id else { return }
        deck.audioFor = id
        let wantsBars = !analysed.contains(id)
        if wantsBars {
            analysed.insert(id)
            deck.reading = true
        }
        Task { [weak self, weak deck] in
            let read = await DjWaveSource.read(for: id, source: source, bars: wantsBars)
            if wantsBars, read.bars == nil { self?.analysed.remove(id) }
            guard let deck, deck.videoId == id else { return }
            if wantsBars { deck.reading = false }
            if let bars = read.bars { deck.waveform = bars }
            deck.scratchAudio = read.audio
            if read.bars == nil, read.audio == nil { deck.audioFor = "" }
        }
    }

    private func remember(_ deck: DjDeck) {
        guard deck.loaded else { return }
        let updated = DjStudio.remember(history, id: deck.videoId, title: deck.title, author: deck.author)
        guard updated != history else { return }
        history = updated
        save()
    }

    func forget(_ offsets: IndexSet) {
        history.remove(atOffsets: offsets)
        save()
    }

    func clearHistory() {
        history = []
        save()
    }

    func pause(_ deck: DjDeck) {
        advance(false, deck)
        deck.settle()
        deck.playing = false
        deck.keepsPlaying = false
        deck.player.pause()
        share(deck.side)
        save()
    }

    func setCue(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded else { return }
        deck.cue = deck.position()
        show("Deck \(side.label) cue set at \(DjStudio.clock(deck.cue)).")
        save()
    }

    func jumpToCue(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded else { return }
        deck.player.seek(deck.cue)
        deck.time = deck.cue
        deck.tickDate = CACurrentMediaTime()
        deck.settle()
        share(side)
    }

    func cueStop(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded else { return }
        if deck.playing || deck.keepsPlaying { pause(deck) }
        jumpToCue(side)
        holdAutomix()
    }

    func seek(_ side: DjDeckSide, to seconds: Double) {
        let deck = deck(side)
        guard deck.loaded, deck.duration > 0 else { return }
        let target = min(deck.duration, max(0, seconds))
        deck.time = target
        deck.tickDate = CACurrentMediaTime()
        deck.settle()
        deck.player.seek(target)
        share(side)
    }

    func setRate(_ side: DjDeckSide, _ rate: Double) {
        let deck = deck(side)
        let clamped = DjStudio.clampRate(rate)
        guard abs(clamped - deck.rate) > 0.0001 else { return }
        deck.rebase()
        deck.rate = clamped
        sendRate(deck)
        save()
        share(side)
    }

    func touchBegan(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded, deck.playing, !deck.scratching else { return }
        deck.brakeTask?.cancel()
        deck.brakeTask = nil
        deck.brakeStart = CACurrentMediaTime()
        deck.rebase()
        deck.brake = DjStudio.touchBrake
        sendRate(deck)
    }

    func touchEnded(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.brake != 0 else { return }
        let left = DjStudio.touchBrakeSeconds - (CACurrentMediaTime() - deck.brakeStart)
        deck.brakeTask?.cancel()
        deck.brakeTask = Task { [weak self, weak deck] in
            if left > 0 { try? await Task.sleep(for: .seconds(left)) }
            guard !Task.isCancelled, let self, let deck else { return }
            self.releaseBrake(deck)
        }
    }

    private func releaseBrake(_ deck: DjDeck) {
        deck.brakeTask?.cancel()
        deck.brakeTask = nil
        guard deck.brake != 0 else { return }
        deck.rebase()
        deck.brake = 0
        sendRate(deck)
    }

    func bend(_ side: DjDeckSide, _ direction: Double) {
        let deck = deck(side)
        deck.rebase()
        deck.bend = direction * DjStudio.bendAmount
        sendRate(deck)
    }

    private func sendRate(_ deck: DjDeck) {
        guard deck.rateTask == nil else { return }
        deck.rateTask = Task { [weak deck] in
            try? await Task.sleep(for: .milliseconds(40))
            guard let deck else { return }
            deck.rateTask = nil
            deck.player.setRate(deck.effectiveRate)
        }
    }

    func sync(_ side: DjDeckSide) {
        let deck = deck(side)
        let other = self.deck(side.other)
        guard deck.loaded, other.loaded else {
            show("Load both decks to sync.")
            return
        }
        deck.rebase()
        deck.rate = DjStudio.syncedRate(ownBpm: deck.bpm, otherBpm: other.bpm, otherRate: other.effectiveRate)
        sendRate(deck)
        share(side)
        show(deck.bpm > 0 && other.bpm > 0
            ? "Deck \(side.label) matched to \(String(format: "%.1f", other.shownBpm)) BPM."
            : "Deck \(side.label) matched Deck \(side.other.label)’s tempo. Tap both BPM boxes to beat-match.")
        save()
    }

    func tapTempo(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded else { return }
        if let bpm = deck.tapTempo.tap(at: CACurrentMediaTime()) {
            deck.bpm = bpm / max(0.01, deck.effectiveRate)
            save()
        } else if deck.bpm == 0 {
            show("Keep tapping on the beat to set Deck \(side.label)’s BPM.")
        }
    }

    func clearBpm(_ side: DjDeckSide) {
        deck(side).bpm = 0
        deck(side).tapTempo = DjTapTempo()
        save()
    }

    func toggleLoop(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded else { return }
        deck.loop.toggle()
        if deck.loop, deck.position() > deck.cue + DjStudio.loopSeconds || deck.position() < deck.cue {
            deck.cue = deck.position()
        }
        show(deck.loop ? "Deck \(side.label) loops 8 seconds from \(DjStudio.clock(deck.cue))." : "Deck \(side.label) loop off.")
        save()
    }

    func setLevel(_ side: DjDeckSide, _ level: Double) {
        deck(side).level = min(1, max(0, level))
        applyMix()
        save()
    }

    func scratchBegan(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded, deck.duration > 0, !deck.scratching else { return }
        releaseBrake(deck)
        deck.rebase()
        deck.resumeAfterScratch = deck.playing
        deck.advancing = false
        deck.scratching = true
        let now = CACurrentMediaTime()
        deck.scratchStamp = now
        deck.scratchSent = now
        deck.scratchSpeed = 0
        deck.player.scratch(0, at: deck.time)
        deck.scratchTask?.cancel()
        deck.scratchTask = nil
        if let audio = deck.scratchAudio, abs(audio.duration - deck.duration) < 2 {
            deck.vinyl = true
            sampler.beginScratch(side, audio: audio, at: deck.time, gain: DjStudio.gain(side, crossfader: crossfader) * deck.level)
            return
        }
        deck.vinyl = false
        sampler.beginScratch()
        deck.scratchTask = Task { [weak self, weak deck] in
            while !Task.isCancelled {
                try? await Task.sleep(for: .milliseconds(45))
                guard let self, let deck, deck.scratching else { return }
                guard CACurrentMediaTime() - deck.scratchStamp > 0.06, deck.scratchSpeed != 0 else { continue }
                deck.scratchSpeed = abs(deck.scratchSpeed) < 0.05 ? 0 : deck.scratchSpeed * 0.35
                self.sendScratch(deck, force: true)
            }
        }
    }

    func scratchMoved(_ side: DjDeckSide, radians: Double) {
        let deck = deck(side)
        guard deck.scratching else { return }
        let seconds = DjStudio.scratchSeconds(fromRadians: radians)
        let now = CACurrentMediaTime()
        deck.time = min(deck.duration, max(0, deck.time + seconds))
        deck.scratchSpeed = DjStudio.scratchSpeed(previous: deck.scratchSpeed, seconds: seconds, elapsed: now - deck.scratchStamp)
        deck.scratchStamp = now
        sendScratch(deck, force: false)
    }

    private func sendScratch(_ deck: DjDeck, force: Bool) {
        let gain = DjStudio.gain(deck.side, crossfader: crossfader) * deck.level
        if deck.vinyl {
            sampler.scratch(deck.side, to: deck.time, gain: gain)
            let now = CACurrentMediaTime()
            guard now - deck.scratchSent >= 0.25 else { return }
            deck.scratchSent = now
            deck.player.scratch(0, at: deck.time)
            return
        }
        sampler.scratch(level: DjStudio.scratchLevel(deck.scratchSpeed) * gain, speed: abs(deck.scratchSpeed))
        let now = CACurrentMediaTime()
        guard force || now - deck.scratchSent >= 0.04 else { return }
        deck.scratchSent = now
        deck.player.scratch(deck.scratchSpeed, at: deck.time)
    }

    func scratchEnded(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.scratching else { return }
        deck.scratchTask?.cancel()
        deck.scratchTask = nil
        deck.scratchSpeed = 0
        if deck.vinyl { sampler.endScratch(side) } else { sampler.scratch(level: 0, speed: 0) }
        deck.vinyl = false
        deck.player.scratch(nil, at: deck.time)
        deck.player.seek(deck.time)
        deck.scratching = false
        deck.tickDate = CACurrentMediaTime()
        deck.settle()
        deck.playing = deck.resumeAfterScratch
        deck.keepsPlaying = deck.resumeAfterScratch
        if deck.resumeAfterScratch { deck.player.play() } else { deck.player.pause() }
        share(side)
    }

    func setCrossfader(_ value: Double, cancelAutomix: Bool = true) {
        if cancelAutomix, automixing {
            automixTask?.cancel()
            automixing = false
        }
        let clamped = min(1, max(-1, value))
        guard clamped != crossfader else { return }
        crossfader = clamped
        applyMix()
        save()
    }

    private func volume(for deck: DjDeck) -> DjVolume {
        let gain = DjStudio.gain(deck.side, crossfader: crossfader) * deck.level
        return DjVolume(level: Int((gain * 100).rounded()), muted: gain < 0.02, gain: (gain * 100).rounded() / 100)
    }

    private func applyMix() {
        for deck in [a, b] { deck.player.setVolume(volume(for: deck)) }
    }

    func slam(_ side: DjDeckSide) {
        automixing = false
        fade(to: side == .a ? -1 : 1, seconds: 0.18, then: nil)
    }

    func automix() {
        setAutomix(!automixOn)
        show(automixOn
            ? (shownQueue.isEmpty ? "AUTOMIX on: queue songs and they play one after another." : "AUTOMIX on: the queue plays one song after another.")
            : "AUTOMIX off.")
    }

    func setAutomix(_ on: Bool) {
        automixLoop?.cancel()
        automixLoop = nil
        if !on {
            automixTask?.cancel()
            automixing = false
        }
        automixOn = on
        guard on else { return }
        automixLoop = Task { [weak self] in
            while !Task.isCancelled, self != nil {
                self?.automixStep()
                try? await Task.sleep(for: .milliseconds(500))
            }
        }
    }

    func holdAutomix() {
        guard automixOn, !a.playing, !b.playing else { return }
        setAutomix(false)
        show("AUTOMIX off.")
    }

    private func automixStep() {
        guard automixOn, !automixing, !a.scratching, !b.scratching else { return }
        let favoured: DjDeckSide = crossfader <= 0 ? .a : .b
        switch DjAutomix.step(a: snapshot(a), b: snapshot(b), favoured: favoured, queued: !shownQueue.isEmpty) {
        case .wait:
            break
        case let .preload(side):
            guard let item = shownQueue.first else { return }
            playFromQueue(item, on: side)
        case let .start(side):
            play(deck(side))
            fade(to: side == .a ? -1 : 1, seconds: 0.4, then: nil)
        case let .mix(from, to, seconds):
            let outgoing = deck(from)
            automixing = true
            play(deck(to))
            fade(to: to == .b ? 1 : -1, seconds: seconds) { [weak self] in
                guard let self else { return }
                self.automixing = false
                if outgoing.playing { self.pause(outgoing) }
            }
        }
    }

    private func snapshot(_ deck: DjDeck) -> DjAutomixDeck {
        DjAutomixDeck(loaded: deck.loaded, playing: deck.playing, position: deck.position(), duration: deck.duration, rate: deck.effectiveRate)
    }

    func mixNow() {
        if automixing {
            automixTask?.cancel()
            automixing = false
            return
        }
        let target: DjDeckSide = crossfader <= 0 ? .b : .a
        let incoming = deck(target)
        let outgoing = deck(target.other)
        guard incoming.loaded else {
            show("Load Deck \(target.label) to mix into it.")
            openLoad(target)
            return
        }
        if !incoming.playing { play(incoming) }
        automixing = true
        fade(to: target == .b ? 1 : -1, seconds: DjStudio.automixSeconds) { [weak self] in
            guard let self else { return }
            self.automixing = false
            if outgoing.playing { self.pause(outgoing) }
        }
    }

    private func fade(to end: Double, seconds: Double, then: (() -> Void)?) {
        automixTask?.cancel()
        let start = crossfader
        let began = CACurrentMediaTime()
        automixTask = Task { [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                let progress = min(1, (CACurrentMediaTime() - began) / seconds)
                let eased = progress * progress * (3 - 2 * progress)
                self.setCrossfader(start + (end - start) * eased, cancelAutomix: false)
                if progress >= 1 {
                    then?()
                    return
                }
                try? await Task.sleep(for: .milliseconds(30))
            }
        }
    }

    func sample(_ side: DjDeckSide) {
        let deck = deck(side)
        switch deck.sample {
        case .idle:
            guard deck.loaded, deck.duration > 0 else {
                show("Load Deck \(side.label) before sampling it.")
                return
            }
            deck.sample = .recording(start: deck.position())
            show("Sampling Deck \(side.label). Tap SAMPLE again to stop.")
        case let .recording(start):
            guard let section = DjStudio.sampleSection(side, start: start, end: deck.position()) else {
                deck.sample = .idle
                show("Nothing was sampled. Play Deck \(side.label) while sampling.")
                return
            }
            let slot = sectionCursor
            sections[slot] = section
            sectionCursor = (slot + 1) % DjStudio.sectionCount
            deck.sample = .armed(slot: slot)
            show("Sample armed: \(String(format: "%.1f", section.duration)) s from \(DjStudio.clock(section.start)). Tap again to play it.")
            save()
        case let .armed(slot):
            deck.sample = .idle
            playSection(slot)
        }
    }

    func playSection(_ index: Int) {
        guard sections.indices.contains(index), let section = sections[index] else { return }
        let deck = deck(section.deck)
        guard deck.loaded else { return }
        let keepPlaying = deck.playing
        deck.player.seek(section.start)
        deck.time = section.start
        deck.tickDate = CACurrentMediaTime()
        play(deck)
        sectionTasks[section.deck]?.cancel()
        sectionTasks[section.deck] = Task { [weak self, weak deck] in
            try? await Task.sleep(for: .seconds(section.duration))
            guard !Task.isCancelled, !keepPlaying, let self, let deck, deck.playing,
                abs(deck.position() - section.start - section.duration) < 2
            else { return }
            self.pause(deck)
        }
    }

    func clearSection(_ index: Int) {
        guard sections.indices.contains(index) else { return }
        sections[index] = nil
        sectionCursor = index
        for deck in [a, b] where deck.sample == .armed(slot: index) { deck.sample = .idle }
        save()
    }

    func playPad(_ pad: DjDrumPad) {
        sampler.play(pad)
        flashedPad = pad
        Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(110))
            if self?.flashedPad == pad { self?.flashedPad = nil }
        }
    }

    func setSamplerLevel(_ level: Double) {
        samplerLevel = min(1, max(0, level))
        sampler.level = samplerLevel
        save()
    }

    func openLoad(_ side: DjDeckSide) {
        openLoader(DjLoadTarget(side))
    }

    func openLoader(_ target: DjLoadTarget) {
        searchTarget = target
        searchProblem = ""
        sheet = .load(target)
    }

    func search() {
        let query = searchText.trimmingCharacters(in: .whitespacesAndNewlines)
        if loadLink(query) {
            sheet = nil
            return
        }
        guard query.count >= 2 else {
            searchProblem = "Enter at least two characters, or paste a YouTube link."
            return
        }
        searchTask?.cancel()
        searching = true
        searchProblem = ""
        let target = searchTarget
        searchTask = Task { [weak self] in
            guard let self else { return }
            let results = await self.fetchResults(query)
            guard !Task.isCancelled else { return }
            self.searching = false
            guard let results else {
                self.searchProblem = "YouTube search is unavailable right now. Paste a YouTube link to load it directly."
                return
            }
            self.searchResults = results
            guard let top = results.first else {
                self.searchProblem = "No YouTube videos matched that search."
                return
            }
            self.searchTarget = target
            self.place(id: top.id, title: top.title, author: top.author)
            if self.sheet == .load(target) { self.sheet = nil }
        }
    }

    private func fetchResults(_ query: String) async -> [DjSearchResult]? {
        if searchInstances.isEmpty, let index = URL(string: "https://api.invidious.io/instances.json?sort_by=health"),
            let (data, response) = try? await URLSession.shared.data(from: index),
            (response as? HTTPURLResponse)?.statusCode == 200
        {
            searchInstances = DjStudio.searchInstances(data)
        }
        for instance in searchInstances {
            guard let url = DjStudio.searchURL(instance, query: query) else { continue }
            var request = URLRequest(url: url)
            request.timeoutInterval = 8
            if let (data, response) = try? await URLSession.shared.data(for: request),
                (response as? HTTPURLResponse)?.statusCode == 200
            {
                searchInstances.removeAll { $0 == instance }
                searchInstances.insert(instance, at: 0)
                return DjStudio.searchResults(data)
            }
            if Task.isCancelled { return nil }
        }
        searchInstances = []
        return nil
    }

    func save(now: Bool = false) {
        saveTask?.cancel()
        guard !now else {
            write()
            return
        }
        saveTask = Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(400))
            guard !Task.isCancelled else { return }
            self?.write()
        }
    }

    private func write() {
        savedAt = CACurrentMediaTime()
        var saved = DjSaved()
        saved.crossfader = crossfader
        saved.samplerLevel = samplerLevel
        saved.sections = sections
        saved.history = history
        saved.queue = queue
        saved.automix = automixOn
        for deck in [a, b] {
            var stored = DjSavedDeck()
            stored.videoId = deck.videoId
            stored.title = deck.title
            stored.author = deck.author
            stored.cue = deck.cue
            stored.level = deck.level
            stored.rate = deck.rate
            stored.bpm = deck.bpm
            stored.loop = deck.loop
            stored.position = deck.position()
            stored.playing = deck.keepsPlaying && deck.loaded
            saved[deck.side] = stored
        }
        if let data = saved.encoded() { defaults.set(data, forKey: DjStudio.storageKey) }
    }
}

enum DjArtwork {
    static func load(_ id: String) async -> UIImage? {
        for quality in ["maxresdefault", "hqdefault"] {
            guard let (data, response) = try? await URLSession.shared.data(from: DjStudio.thumbnailURL(id, quality: quality)),
                (response as? HTTPURLResponse)?.statusCode == 200, let image = UIImage(data: data), image.size.width > 200
            else { continue }
            return square(image, letterboxed: quality == "hqdefault")
        }
        return nil
    }

    private static func square(_ image: UIImage, letterboxed: Bool) -> UIImage {
        let size = image.size
        let content = letterboxed ? CGRect(x: 0, y: size.height * 0.125, width: size.width, height: size.height * 0.75) : CGRect(origin: .zero, size: size)
        let edge = min(content.width, content.height)
        let crop = CGRect(x: content.midX - edge / 2, y: content.midY - edge / 2, width: edge, height: edge)
        let target = min(edge, 640)
        let format = UIGraphicsImageRendererFormat()
        format.scale = 1
        format.opaque = false
        return UIGraphicsImageRenderer(size: CGSize(width: target, height: target), format: format).image { _ in
            UIBezierPath(ovalIn: CGRect(x: 0, y: 0, width: target, height: target)).addClip()
            let scale = target / edge
            image.draw(in: CGRect(x: -crop.minX * scale, y: -crop.minY * scale, width: size.width * scale, height: size.height * scale))
        }
    }
}
