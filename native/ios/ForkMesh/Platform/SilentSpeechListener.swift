import AVFoundation
import CoreMotion
import FMBase
import FMTools
import Foundation
import Observation
import UIKit

@MainActor
@Observable
final class SilentSpeechListener: NSObject, CMHeadphoneMotionManagerDelegate {
    static let shared: SilentSpeechListener = SilentSpeechListener()
    static let calibrationSeconds: Double = 2
    static let holdPreRoll: Double = 0.1
    static let recentSeconds: Double = 0.4
    static let drawInterval: Double = 1.0 / 15
    static let pulseInterval: Double = 1
    static let saver: DispatchQueue = DispatchQueue(label: "silent-speech-save", qos: .utility)

    enum Hold: Equatable {
        case train(String)
        case decode
    }

    private(set) var listening: Bool = false
    private(set) var connected: Bool = false
    private(set) var problem: String?
    private(set) var denied: Bool = false
    private(set) var rate: Double?
    private(set) var scope: SilentScope = SilentScope()
    private(set) var hold: Hold?
    private(set) var dataset: SilentDataset = .starter
    private(set) var evaluation: SilentEvaluation?
    private(set) var lastMatch: SilentMatch?
    private(set) var transcript: SilentTranscript = SilentTranscript()
    private(set) var noise: SilentNoiseFloor?
    private(set) var calibrating: Bool = false
    private(set) var detecting: Bool = false
    private(set) var reading: SilentReading?
    private(set) var readingSummary: String?
    private(set) var lastSentence: [String] = []
    private(set) var pulse: SilentPulse?
    private(set) var diagnosis: SilentDiagnosis?
    private(set) var diagnosing: Bool = false
    let hasAcceleration: Bool = true
    var selectedWord: String? = nil
    var channels: Set<SilentChannel> = SilentChannel.defaults {
        didSet {
            if channels.isEmpty { channels = oldValue }
            prefs?.set(channels.map(\.rawValue).sorted(), forKey: SilentSpeechPrefs.channels)
            invalidate()
        }
    }
    var minMargin: Double = SilentSpeechPrefs.defaultMinMargin {
        didSet { prefs?.set(minMargin, forKey: SilentSpeechPrefs.minMargin) }
    }
    var maxDistance: Double? = nil {
        didSet {
            if let maxDistance { prefs?.set(maxDistance, forKey: SilentSpeechPrefs.maxDistance) } else { prefs?.remove(SilentSpeechPrefs.maxDistance) }
        }
    }
    var speakAloud: Bool = false {
        didSet { prefs?.set(speakAloud, forKey: SilentSpeechPrefs.speakAloud) }
    }
    var autoDetect: Bool = false {
        didSet {
            prefs?.set(autoDetect, forKey: SilentSpeechPrefs.autoDetect)
            segmenter?.reset()
            if autoDetect && noise == nil { calibrate() }
        }
    }

    @ObservationIgnored private let manager: CMHeadphoneMotionManager = CMHeadphoneMotionManager()
    @ObservationIgnored private let voice: AVSpeechSynthesizer = AVSpeechSynthesizer()
    @ObservationIgnored private var model: SilentModel?
    @ObservationIgnored private var recent: [SilentSample] = []
    @ObservationIgnored private var held: [SilentSample] = []
    @ObservationIgnored private var calibration: [Double] = []
    @ObservationIgnored private var calibrationStart: Double?
    @ObservationIgnored private var segmenter: SilentSegmenter?
    @ObservationIgnored private var rateWindow: [Double] = []
    @ObservationIgnored private var live: SilentScope = SilentScope()
    @ObservationIgnored private var drawn: Double = -.infinity
    @ObservationIgnored private var session: SilentReading?
    @ObservationIgnored private var still: [SilentSample] = []
    @ObservationIgnored private var pulseChecked: Double = -.infinity
    @ObservationIgnored private var generation: Int = 0

    private var prefs: KeyValueStore? { Services.get(KeyValueStore.self) }

    override init() {
        super.init()
        manager.delegate = self
        if let prefs = Services.get(KeyValueStore.self) {
            let saved: Set<SilentChannel> = Set((prefs.stringList(forKey: SilentSpeechPrefs.channels) ?? []).compactMap(SilentChannel.init(rawValue:)))
            channels = saved.isEmpty ? SilentChannel.defaults : saved
            minMargin = prefs.double(forKey: SilentSpeechPrefs.minMargin) ?? SilentSpeechPrefs.defaultMinMargin
            maxDistance = prefs.double(forKey: SilentSpeechPrefs.maxDistance)
            speakAloud = prefs.bool(forKey: SilentSpeechPrefs.speakAloud) ?? false
            autoDetect = prefs.bool(forKey: SilentSpeechPrefs.autoDetect) ?? false
        }
        dataset = Self.load() ?? .starter
        selectedWord = dataset.words.first?.word
        rebuild()
    }

    var supported: Bool { manager.isDeviceMotionAvailable }

    func start() {
        guard !listening else { return }
        switch CMHeadphoneMotionManager.authorizationStatus() {
        case .denied, .restricted:
            denied = true
            problem = "Motion access is off for ForkMesh. Turn on Motion & Fitness in Settings to read your earbuds."
            return
        default:
            denied = false
        }
        guard manager.isDeviceMotionAvailable else {
            problem = "Head tracking is not available on this device."
            return
        }
        problem = nil
        listening = true
        manager.startDeviceMotionUpdates(to: .main) { [weak self] (motion: CMDeviceMotion?, error: Error?) in
            MainActor.assumeIsolated {
                guard let self else { return }
                if let motion { self.receive(motion) } else if error != nil { self.fail() }
            }
        }
        if autoDetect && noise == nil { calibrate() }
    }

    func stop() {
        guard listening else { return }
        manager.stopDeviceMotionUpdates()
        listening = false
        connected = false
        rate = nil
        hold = nil
        held.removeAll()
        recent.removeAll()
        live.clear()
        scope = live
        drawn = -.infinity
        still.removeAll()
        pulse = nil
        rateWindow.removeAll()
        if session != nil { finishReading() }
        segmenter?.reset()
        calibrating = false
        detecting = false
    }

    nonisolated func headphoneMotionManagerDidConnect(_ manager: CMHeadphoneMotionManager) {
        Task { @MainActor in self.connected = true }
    }

    nonisolated func headphoneMotionManagerDidDisconnect(_ manager: CMHeadphoneMotionManager) {
        Task { @MainActor in
            self.connected = false
            self.rate = nil
            self.hold = nil
            self.held.removeAll()
            self.still.removeAll()
            self.pulse = nil
        }
    }

    private func fail() {
        if CMHeadphoneMotionManager.authorizationStatus() == .denied {
            denied = true
            problem = "Motion access is off for ForkMesh. Turn on Motion & Fitness in Settings to read your earbuds."
            stop()
        }
    }

    private func receive(_ motion: CMDeviceMotion) {
        let q: CMQuaternion = motion.attitude.quaternion
        let sample: SilentSample = SilentSample(
            t: motion.timestamp,
            rotationRate: SilentVector(motion.rotationRate.x, motion.rotationRate.y, motion.rotationRate.z),
            attitude: SilentQuaternion(w: q.w, x: q.x, y: q.y, z: q.z),
            acceleration: SilentVector(motion.userAcceleration.x, motion.userAcceleration.y, motion.userAcceleration.z))
        if !connected { connected = true }
        live.push(sample)
        if sample.t - drawn >= Self.drawInterval { draw() }
        rateWindow.append(sample.t)
        if let first = rateWindow.firstIndex(where: { $0 >= sample.t - 2 }), first > 0 { rateWindow.removeFirst(first) }
        if rateWindow.count > 5, let first = rateWindow.first, let last = rateWindow.last, last > first {
            let measured: Double = (Double(rateWindow.count - 1) / (last - first)).rounded()
            if rate != measured { rate = measured }
        }
        recent.append(sample)
        if let first = recent.firstIndex(where: { $0.t >= sample.t - Self.recentSeconds }), first > 0 { recent.removeFirst(first) }
        listenForPulse(sample)
        if hold != nil {
            held.append(sample)
            return
        }
        if calibrating {
            calibration.append(sample.energy)
            if calibrationStart == nil { calibrationStart = sample.t }
            if let start = calibrationStart, sample.t - start >= Self.calibrationSeconds { finishCalibration() }
            return
        }
        guard autoDetect || session != nil, var segmenter else { return }
        let segment: [SilentSample]? = segmenter.push(sample)
        if detecting != segmenter.active { detecting = segmenter.active }
        self.segmenter = segmenter
        if session != nil {
            read(sample.t, segment, busy: segmenter.active)
        } else if let segment {
            decode(segment)
        }
    }

    private func draw() {
        drawn = live.latest ?? drawn
        scope = live
    }

    private func listenForPulse(_ sample: SilentSample) {
        still.append(sample)
        if let first = still.firstIndex(where: { $0.t >= sample.t - SilentPulse.window }), first > 0 { still.removeFirst(first) }
        guard sample.t - pulseChecked >= Self.pulseInterval else { return }
        pulseChecked = sample.t
        let found: SilentPulse? = SilentPulse.estimate(still, stillness: max(noise?.on ?? 0, SilentPulse.stillness))
        if found?.label != pulse?.label { pulse = found }
    }

    func startReading() {
        guard listening, session == nil, hold == nil else { return }
        session = SilentReading(sentences: SilentReading.plan(dataset), seed: UInt64(Date().timeIntervalSince1970 * 1000))
        publishReading()
        readingSummary = nil
        lastSentence = []
        segmenter?.reset()
        calibrate()
    }

    private func read(_ t: Double, _ segment: [SilentSample]?, busy: Bool) {
        guard var current = session else { return }
        if let segment {
            let word: String? = current.position < current.sentence.count ? current.sentence[current.position] : nil
            live.mark(segment, label: word ?? "+")
            draw()
            current.hear(segment)
        }
        let takes: [SilentTake] = current.tick(t, busy: busy).map { SilentTake(id: UUID().uuidString, word: $0.word, samples: $0.samples) }
        if !takes.isEmpty {
            takes.forEach { dataset.learn($0) }
            lastSentence = takes.map(\.id)
            changed()
        }
        session = current
        publishReading()
        if current.done { finishReading() }
    }

    private func publishReading() {
        guard let current = session else {
            if reading != nil { reading = nil }
            return
        }
        if let shown = reading, shown.clock == current.clock, shown.position == current.position, shown.index == current.index,
           shown.collected == current.collected, shown.notice == current.notice, shown.sentencesRead == current.sentencesRead { return }
        reading = current
    }

    func readAgain() {
        session?.again()
        publishReading()
        segmenter?.reset()
    }

    func skipSentence() {
        session?.skip()
        publishReading()
        segmenter?.reset()
    }

    func dropLastSentence() {
        guard !lastSentence.isEmpty else { return }
        dataset.removeTakes(Set(lastSentence))
        session?.forget(lastSentence.count)
        publishReading()
        lastSentence = []
        changed()
    }

    func finishReading() {
        guard let current = session else { return }
        session = nil
        reading = nil
        readingSummary = current.summary
        lastSentence = []
        segmenter?.reset()
        detecting = false
        rebuild()
    }

    func beginHold(_ kind: Hold) {
        guard listening, hold == nil, session == nil else { return }
        if case .train(let word) = kind, !dataset.words.contains(where: { $0.word == word }) { return }
        let now: Double = recent.last?.t ?? 0
        held = recent.filter { $0.t >= now - Self.holdPreRoll }
        hold = kind
    }

    func endHold() {
        guard let kind = hold else { return }
        hold = nil
        let samples: [SilentSample] = held
        held.removeAll()
        segmenter?.reset()
        guard let first = samples.first, let last = samples.last, last.t - first.t >= 0.2 else { return }
        switch kind {
        case .train(let word):
            live.mark(samples, label: word)
            draw()
            dataset.addTake(SilentTake(id: UUID().uuidString, word: word, samples: samples))
            changed()
        case .decode:
            let words: [[SilentSample]] = noise.map { SilentSegmenter.split(samples, noise: $0) } ?? []
            if words.isEmpty {
                decode(samples)
            } else {
                words.forEach { decode($0) }
            }
        }
    }

    func calibrate() {
        guard listening else { return }
        calibration.removeAll()
        calibrationStart = nil
        calibrating = true
    }

    private func finishCalibration() {
        calibrating = false
        noise = SilentNoiseFloor.calibrate(calibration)
        segmenter = noise.map { SilentSegmenter(noise: $0) }
        calibration.removeAll()
        calibrationStart = nil
        live.recentre()
        draw()
    }

    func recentre() {
        live.recentre()
        draw()
    }

    private func decode(_ samples: [SilentSample]) {
        if model == nil { model = SilentModel(takes: dataset.takes, channels: channels, neighbors: SilentEvaluator.neighbors(for: dataset), language: dataset.language, perWord: SilentModel.maxPerWord) }
        guard let model, let match = model.match(samples, maxDistance: maxDistance, minMargin: minMargin, previous: transcript.words.last) else {
            lastMatch = nil
            return
        }
        lastMatch = match
        live.mark(samples, label: match.accepted ? match.word : "?")
        draw()
        guard match.accepted else { return }
        transcript.append(match.word)
        if speakAloud { speak(dataset.phrase(match.word)) }
    }

    func speak(_ text: String) {
        voice.stopSpeaking(at: .immediate)
        voice.speak(AVSpeechUtterance(string: text))
    }

    func diagnose() {
        guard !diagnosing else { return }
        diagnosing = true
        let input: SilentDoctorInput = SilentDoctorInput(
            dataset: dataset, channels: channels, available: hasAcceleration ? SilentChannel.defaults : SilentChannel.defaults.subtracting([.acceleration]),
            minMargin: minMargin, maxDistance: maxDistance, rate: rate, noise: noise, pulse: pulse)
        Task.detached(priority: .userInitiated) {
            let evaluation: SilentEvaluation? = SilentEvaluator.evaluate(input.dataset, channels: input.channels, neighbors: SilentEvaluator.neighbors(for: input.dataset))
            let diagnosis: SilentDiagnosis = SilentDoctor.diagnose(input)
            await MainActor.run {
                self.evaluation = evaluation
                self.diagnosis = diagnosis
                self.diagnosing = false
            }
        }
    }

    func applyFix(_ finding: SilentFinding) {
        guard let fix = finding.fix else { return }
        diagnosis = diagnosis?.without(finding)
        switch fix {
        case .dropTakes(let ids):
            dataset.removeTakes(Set(ids))
            maxDistance = nil
            changed()
            diagnose()
        case .channels(let chosen):
            channels = chosen
            diagnose()
        case .margin(let value):
            minMargin = value
        case .clearLimit:
            maxDistance = nil
        case .calibrate:
            calibrate()
        case .read:
            diagnosis = nil
            startReading()
        }
    }

    func addEntry(_ text: String) -> Bool {
        guard let word = dataset.addEntry(text) else { return false }
        selectedWord = word
        changed()
        return true
    }

    func setPhrase(_ phrase: String, for word: String) {
        dataset.setPhrase(phrase, for: word)
        save()
    }

    func removeWord(_ word: String) {
        dataset.removeWord(word)
        if selectedWord == word { selectedWord = dataset.words.first?.word }
        changed()
    }

    func undoTake(_ word: String) {
        dataset.removeLastTake(word)
        changed()
    }

    func resetLimit() { maxDistance = nil }

    func undoWord() { transcript.undo() }

    func clearTranscript() {
        transcript.clear()
        lastMatch = nil
    }

    func copyTranscript() { UIPasteboard.general.string = transcript.text }

    private func changed() {
        invalidate()
        save()
    }

    private func invalidate() {
        model = nil
        evaluation = nil
        rebuild()
    }

    private func rebuild() {
        generation += 1
        guard session == nil, !dataset.takes.isEmpty else { return }
        let ticket: Int = generation
        let takes: [SilentTake] = dataset.takes
        let channels: Set<SilentChannel> = channels
        let neighbors: Int = SilentEvaluator.neighbors(for: dataset)
        let language: SilentLanguage = dataset.language
        Task.detached(priority: .utility) {
            let built: SilentModel = SilentModel(takes: takes, channels: channels, neighbors: neighbors, language: language, perWord: SilentModel.maxPerWord)
            await MainActor.run {
                if ticket == self.generation && self.model == nil { self.model = built }
            }
        }
    }

    private static var file: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("SilentSpeech", isDirectory: true)
            .appendingPathComponent("dataset.json")
    }

    private static func load() -> SilentDataset? {
        guard let data = try? Data(contentsOf: file) else { return nil }
        return SilentDataset.decode(data)
    }

    private func save() {
        let url: URL = Self.file
        let snapshot: SilentDataset = dataset
        Self.saver.async {
            let data: Data = snapshot.encoded()
            try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
            try? data.write(to: url, options: [.atomic, .completeFileProtection])
        }
    }
}
