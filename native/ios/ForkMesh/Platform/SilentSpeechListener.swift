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

    enum Hold: Equatable {
        case train(String)
        case decode
    }

    private(set) var listening: Bool = false
    private(set) var connected: Bool = false
    private(set) var problem: String?
    private(set) var denied: Bool = false
    private(set) var level: Double = 0
    private(set) var rate: Double?
    private(set) var hold: Hold?
    private(set) var dataset: SilentDataset = .starter
    private(set) var evaluation: SilentEvaluation?
    private(set) var evaluating: Bool = false
    private(set) var lastMatch: SilentMatch?
    private(set) var transcript: SilentTranscript = SilentTranscript()
    private(set) var noise: SilentNoiseFloor?
    private(set) var calibrating: Bool = false
    private(set) var detecting: Bool = false
    private(set) var reading: SilentReading?
    private(set) var readingSummary: String?
    private(set) var lastSentence: [String] = []
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
        level = 0
        rate = nil
        hold = nil
        held.removeAll()
        recent.removeAll()
        rateWindow.removeAll()
        if reading != nil { finishReading() }
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
            self.level = 0
            self.rate = nil
            self.hold = nil
            self.held.removeAll()
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
        connected = true
        level = level * 0.6 + sample.energy * 0.4
        rateWindow.append(sample.t)
        rateWindow.removeAll { $0 < sample.t - 2 }
        if rateWindow.count > 5, let first = rateWindow.first, let last = rateWindow.last, last > first {
            rate = Double(rateWindow.count - 1) / (last - first)
        }
        recent.append(sample)
        recent.removeAll { $0.t < sample.t - Self.recentSeconds }
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
        guard autoDetect || reading != nil, var segmenter else { return }
        let segment: [SilentSample]? = segmenter.push(sample)
        detecting = segmenter.active
        self.segmenter = segmenter
        if reading != nil {
            read(sample.t, segment)
        } else if let segment {
            decode(segment)
        }
    }

    func startReading() {
        guard listening, reading == nil, hold == nil, dataset.words.count >= 2 else { return }
        reading = SilentReading(words: dataset.words.map(\.word), seed: UInt64(Date().timeIntervalSince1970 * 1000))
        readingSummary = nil
        lastSentence = []
        segmenter?.reset()
        calibrate()
    }

    private func read(_ t: Double, _ segment: [SilentSample]?) {
        guard var current = reading else { return }
        current.tick(t)
        if let segment {
            let takes: [SilentTake] = current.hear(segment).map { SilentTake(id: UUID().uuidString, word: $0.word, samples: $0.samples) }
            if !takes.isEmpty {
                takes.forEach { dataset.addTake($0) }
                lastSentence = takes.map(\.id)
                changed()
            }
        }
        reading = current
        if current.done { finishReading() }
    }

    func readAgain() {
        reading?.again()
        segmenter?.reset()
    }

    func skipSentence() {
        reading?.skip()
        segmenter?.reset()
    }

    func dropLastSentence() {
        guard !lastSentence.isEmpty else { return }
        dataset.removeTakes(Set(lastSentence))
        reading?.forget(lastSentence.count)
        lastSentence = []
        changed()
    }

    func finishReading() {
        guard let current = reading else { return }
        reading = nil
        readingSummary = current.summary
        lastSentence = []
        segmenter?.reset()
        detecting = false
    }

    func beginHold(_ kind: Hold) {
        guard listening, hold == nil, reading == nil else { return }
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
            dataset.addTake(SilentTake(id: UUID().uuidString, word: word, samples: samples))
            changed()
        case .decode:
            decode(samples)
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
    }

    private func decode(_ samples: [SilentSample]) {
        if model == nil { model = SilentModel(takes: dataset.takes, channels: channels, neighbors: SilentEvaluator.neighbors(for: dataset)) }
        guard let model, let match = model.match(samples, maxDistance: maxDistance, minMargin: minMargin) else {
            lastMatch = nil
            return
        }
        lastMatch = match
        guard match.accepted else { return }
        transcript.append(match.word)
        if speakAloud { speak(dataset.phrase(match.word)) }
    }

    func speak(_ text: String) {
        voice.stopSpeaking(at: .immediate)
        voice.speak(AVSpeechUtterance(string: text))
    }

    func evaluate() {
        guard !evaluating else { return }
        evaluating = true
        let snapshot: SilentDataset = dataset
        let channels: Set<SilentChannel> = channels
        Task.detached(priority: .userInitiated) {
            let result: SilentEvaluation? = SilentEvaluator.evaluate(snapshot, channels: channels, neighbors: SilentEvaluator.neighbors(for: snapshot))
            await MainActor.run {
                self.evaluation = result
                self.evaluating = false
                if let limit = result?.suggestedMaxDistance { self.maxDistance = limit }
            }
        }
    }

    func addWord(_ word: String, phrase: String) -> Bool {
        guard dataset.addWord(word, phrase: phrase) else { return false }
        selectedWord = SilentDataset.clean(word)
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
        let data: Data = dataset.encoded()
        try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
        try? data.write(to: url, options: [.atomic, .completeFileProtection])
    }
}
