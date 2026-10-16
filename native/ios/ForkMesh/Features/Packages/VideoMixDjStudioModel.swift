import AVFoundation
import FMApi
import FMPackages
import Foundation
import QuartzCore
import SwiftUI
import UIKit

enum DjSheet: Identifiable, Equatable {
    case load(DjDeckSide)
    case deck(DjDeckSide)
    case pads
    case info

    var id: String {
        switch self {
        case let .load(side): return "load-\(side.rawValue)"
        case let .deck(side): return "deck-\(side.rawValue)"
        case .pads: return "pads"
        case .info: return "info"
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
    var playing = false
    var buffering = false
    var cue: Double = 0
    var level: Double = 1
    var rate: Double = 1
    var bend: Double = 0
    var bpm: Double = 0
    var loop = false
    var scratching = false
    var showsRemaining = false

    @ObservationIgnored var tickDate = CACurrentMediaTime()
    @ObservationIgnored var tapTempo = DjTapTempo()
    @ObservationIgnored var resumeAfterScratch = false
    @ObservationIgnored var lastScratchSeek: CFTimeInterval = 0
    @ObservationIgnored var settleUntil: CFTimeInterval = 0
    @ObservationIgnored var rateTask: Task<Void, Never>?
    @ObservationIgnored let player: DjYouTubePlayer

    init(side: DjDeckSide, origin: URL) {
        self.side = side
        player = DjYouTubePlayer(origin: origin)
    }

    var loaded: Bool { !videoId.isEmpty }
    var effectiveRate: Double { rate * (1 + bend) }
    var shownBpm: Double { bpm * effectiveRate }

    func position(at now: CFTimeInterval = CACurrentMediaTime()) -> Double {
        guard playing, !scratching, !buffering else { return time }
        let moved = time + (now - tickDate) * effectiveRate
        return duration > 0 ? min(duration, moved) : moved
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
final class DjStudioModel {
    let a: DjDeck
    let b: DjDeck
    var crossfader: Double = 0
    var samplerLevel: Double = 0.8
    var sections: [DjSection?] = Array(repeating: nil, count: DjStudio.sectionCount)
    var zoomed = false
    var automixing = false
    var recording = false
    var notice: String?
    var sheet: DjSheet?
    var searchTarget: DjDeckSide = .a
    var searchText = ""
    var searchResults: [DjSearchResult] = []
    var searching = false
    var searchProblem = ""
    var flashedPad: DjDrumPad?
    var leavingAfterSheet = false

    @ObservationIgnored var onLeave: (() -> Void)?
    @ObservationIgnored private let defaults: UserDefaults
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
    @ObservationIgnored private var active = false

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        let origin = DjStudioModel.origin()
        a = DjDeck(side: .a, origin: origin)
        b = DjDeck(side: .b, origin: origin)
        let saved = DjSaved.decode(defaults.data(forKey: DjStudio.storageKey))
        crossfader = saved.crossfader
        samplerLevel = saved.samplerLevel
        sampler.level = saved.samplerLevel
        sections = saved.sections
        sectionCursor = sections.firstIndex(where: { $0 == nil }) ?? 0
        zoomed = saved.zoomed
        for deck in [a, b] {
            deck.player.onEvent = { [weak self, weak deck] event in
                guard let self, let deck else { return }
                self.handle(event, deck)
            }
            let stored = saved[deck.side]
            deck.cue = stored.cue
            deck.level = stored.level
            deck.rate = stored.rate
            deck.bpm = stored.bpm
            deck.loop = stored.loop
            if !stored.videoId.isEmpty {
                prepare(deck, id: stored.videoId, title: stored.title, author: stored.author, start: stored.cue, keepSettings: true)
            }
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

    func appear() {
        guard !active else { return }
        active = true
        let session = AVAudioSession.sharedInstance()
        previousCategory = session.category
        previousOptions = session.categoryOptions
        try? session.setCategory(.playback, mode: .default, options: [.mixWithOthers])
        try? session.setActive(true)
        previousIdle = UIApplication.shared.isIdleTimerDisabled
        UIApplication.shared.isIdleTimerDisabled = true
        FmOrientationLock.allowLandscape(true)
        applyMix()
    }

    func disappear() {
        guard active else { return }
        active = false
        automixTask?.cancel()
        automixing = false
        for deck in [a, b] where deck.playing { pause(deck) }
        sampler.stop()
        save(now: true)
        UIApplication.shared.isIdleTimerDisabled = previousIdle
        if let previousCategory {
            try? AVAudioSession.sharedInstance().setCategory(previousCategory, options: previousOptions)
        }
        FmOrientationLock.allowLandscape(false)
    }

    func leave() {
        disappear()
        onLeave?()
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
        show("Deck \(side.label) loaded.")
        save()
    }

    func loadLink(_ text: String) -> Bool {
        let id = DjStudio.videoId(text)
        guard !id.isEmpty else { return false }
        load(searchTarget, id: id)
        return true
    }

    private func prepare(_ deck: DjDeck, id: String, title: String, author: String, start: Double, keepSettings: Bool) {
        sectionTasks[deck.side]?.cancel()
        deck.videoId = id
        deck.title = title
        deck.author = author
        deck.artwork = nil
        deck.waveform = DjStudio.waveform(id, count: 1200)
        deck.time = start
        deck.tickDate = CACurrentMediaTime()
        deck.duration = 0
        deck.playing = false
        deck.buffering = false
        if !keepSettings {
            deck.cue = 0
            deck.bpm = 0
            deck.loop = false
            deck.tapTempo = DjTapTempo()
        }
        let volume = volume(for: deck)
        deck.player.load(id, start: start, rate: deck.effectiveRate, volume: volume.level, muted: volume.muted)
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
        deck.time = 0
        deck.duration = 0
        deck.playing = false
        deck.cue = 0
        deck.bpm = 0
        deck.loop = false
        sections = sections.map { $0?.deck == side ? nil : $0 }
        save()
    }

    private func handle(_ event: DjPlayerEvent, _ deck: DjDeck) {
        switch event {
        case .ready:
            applyMix()
            deck.player.setRate(deck.effectiveRate)
        case let .tick(time, duration, state):
            if duration > 0, abs(duration - deck.duration) > 0.5 { deck.duration = duration }
            guard CACurrentMediaTime() >= deck.settleUntil else { return }
            apply(state, deck)
            if !deck.scratching {
                if abs(deck.time - time) > 0.001 { deck.time = time }
                deck.tickDate = CACurrentMediaTime()
            }
            if deck.loop, deck.playing, !deck.scratching, deck.time >= deck.cue + DjStudio.loopSeconds {
                deck.player.seek(deck.cue)
                deck.time = deck.cue
            }
        case let .state(state):
            apply(state, deck)
        case let .meta(title, author):
            if deck.title != title || deck.author != author {
                deck.title = title
                deck.author = author
                save()
            }
        case let .failed(code):
            deck.playing = false
            switch code {
            case 2: show("Deck \(deck.side.label): that YouTube link is not valid.")
            case 100: show("Deck \(deck.side.label): that video was removed or is private.")
            case 101, 150, 152, 153: show("Deck \(deck.side.label): the owner does not allow this video to be embedded.")
            case -1: show("YouTube could not be reached. Check your connection.")
            default: show("Deck \(deck.side.label): YouTube could not play this video.")
            }
        }
    }

    private func apply(_ state: Int, _ deck: DjDeck) {
        guard !deck.scratching else { return }
        let playing = state == 1 || state == 3
        let buffering = state == 3
        if deck.playing != playing { deck.playing = playing }
        if deck.buffering != buffering { deck.buffering = buffering }
        if state == 0, deck.duration > 0 { deck.time = deck.duration }
    }

    func togglePlay(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded else {
            openLoad(side)
            return
        }
        if deck.playing { pause(deck) } else { play(deck) }
    }

    private func play(_ deck: DjDeck) {
        deck.rebase()
        deck.settle()
        deck.playing = true
        deck.player.play()
    }

    private func pause(_ deck: DjDeck) {
        deck.rebase()
        deck.settle()
        deck.playing = false
        deck.player.pause()
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
    }

    func cueStop(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.loaded else { return }
        if deck.playing { pause(deck) }
        jumpToCue(side)
    }

    func seek(_ side: DjDeckSide, to seconds: Double) {
        let deck = deck(side)
        guard deck.loaded, deck.duration > 0 else { return }
        let target = min(deck.duration, max(0, seconds))
        deck.time = target
        deck.tickDate = CACurrentMediaTime()
        deck.settle()
        deck.player.seek(target)
    }

    func setRate(_ side: DjDeckSide, _ rate: Double) {
        let deck = deck(side)
        let clamped = DjStudio.clampRate(rate)
        guard abs(clamped - deck.rate) > 0.0001 else { return }
        deck.rebase()
        deck.rate = clamped
        sendRate(deck)
        save()
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
            try? await Task.sleep(for: .milliseconds(80))
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
        deck.rebase()
        deck.resumeAfterScratch = deck.playing
        deck.scratching = true
        deck.player.pause()
    }

    func scratchMoved(_ side: DjDeckSide, radians: Double) {
        let deck = deck(side)
        guard deck.scratching else { return }
        deck.time = min(deck.duration, max(0, deck.time + DjStudio.scratchSeconds(fromRadians: radians)))
        let now = CACurrentMediaTime()
        if now - deck.lastScratchSeek > 0.08 {
            deck.lastScratchSeek = now
            deck.player.seek(deck.time)
        }
    }

    func scratchEnded(_ side: DjDeckSide) {
        let deck = deck(side)
        guard deck.scratching else { return }
        deck.player.seek(deck.time)
        deck.scratching = false
        deck.tickDate = CACurrentMediaTime()
        deck.settle()
        deck.playing = deck.resumeAfterScratch
        if deck.resumeAfterScratch { deck.player.play() }
    }

    func setCrossfader(_ value: Double, cancelAutomix: Bool = true) {
        if cancelAutomix {
            automixTask?.cancel()
            automixing = false
        }
        let clamped = min(1, max(-1, value))
        guard clamped != crossfader else { return }
        crossfader = clamped
        applyMix()
        save()
    }

    private func volume(for deck: DjDeck) -> (level: Int, muted: Bool) {
        let gain = DjStudio.gain(deck.side, crossfader: crossfader) * deck.level
        return (Int((gain * 100).rounded()), gain < 0.02)
    }

    private func applyMix() {
        for deck in [a, b] {
            let volume = volume(for: deck)
            deck.player.setVolume(volume.level, muted: volume.muted)
        }
    }

    func slam(_ side: DjDeckSide) {
        automixing = false
        fade(to: side == .a ? -1 : 1, seconds: 0.18, then: nil)
    }

    func automix() {
        if automixing {
            automixTask?.cancel()
            automixing = false
            return
        }
        let target: DjDeckSide = crossfader <= 0 ? .b : .a
        let incoming = deck(target)
        let outgoing = deck(target.other)
        guard incoming.loaded else {
            show("Load Deck \(target.label) to automix into it.")
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

    func capture() {
        let candidates = [a, b].filter { $0.loaded && $0.duration > 0 }
        guard let deck = candidates.max(by: { left, right in
            let leftScore = (left.playing ? 2 : 0) + DjStudio.gain(left.side, crossfader: crossfader)
            let rightScore = (right.playing ? 2 : 0) + DjStudio.gain(right.side, crossfader: crossfader)
            return leftScore < rightScore
        }) else {
            show("Load and start a deck before capturing a section.")
            return
        }
        let start = deck.position()
        let slot = sectionCursor
        sections[slot] = DjSection(deck: deck.side, start: start, duration: min(DjStudio.sectionSeconds, max(1, deck.duration - start)))
        sectionCursor = (slot + 1) % DjStudio.sectionCount
        recording = true
        Task { [weak self] in
            try? await Task.sleep(for: .milliseconds(650))
            self?.recording = false
        }
        show("Captured Deck \(deck.side.label) at \(DjStudio.clock(start)) into Section \(slot + 1).")
        save()
    }

    func playSection(_ index: Int) {
        guard sections.indices.contains(index), let section = sections[index] else { return }
        let deck = deck(section.deck)
        guard deck.loaded else { return }
        deck.player.seek(section.start)
        deck.time = section.start
        deck.tickDate = CACurrentMediaTime()
        play(deck)
        sectionTasks[section.deck]?.cancel()
        sectionTasks[section.deck] = Task { [weak self, weak deck] in
            try? await Task.sleep(for: .seconds(section.duration))
            guard !Task.isCancelled, let self, let deck, deck.playing,
                abs(deck.position() - section.start - section.duration) < 2
            else { return }
            self.pause(deck)
        }
    }

    func clearSection(_ index: Int) {
        guard sections.indices.contains(index) else { return }
        sections[index] = nil
        sectionCursor = index
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

    func toggleZoom() {
        zoomed.toggle()
        save()
    }

    func openLoad(_ side: DjDeckSide) {
        searchTarget = side
        searchProblem = ""
        sheet = .load(side)
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
        searchTask = Task { [weak self] in
            guard let self else { return }
            let results = await self.fetchResults(query)
            guard !Task.isCancelled else { return }
            self.searching = false
            if let results {
                self.searchResults = results
                if results.isEmpty { self.searchProblem = "No YouTube videos matched that search." }
            } else {
                self.searchProblem = "YouTube search is unavailable right now. Paste a YouTube link to load it directly."
            }
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
        var saved = DjSaved()
        saved.crossfader = crossfader
        saved.samplerLevel = samplerLevel
        saved.sections = sections
        saved.zoomed = zoomed
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
        format.opaque = true
        return UIGraphicsImageRenderer(size: CGSize(width: target, height: target), format: format).image { _ in
            let scale = target / edge
            image.draw(in: CGRect(x: -crop.minX * scale, y: -crop.minY * scale, width: size.width * scale, height: size.height * scale))
        }
    }
}
