import FMBase
import Foundation
import Observation

public enum SpeechVoiceQuality: Int, Comparable, Sendable, CaseIterable {
    case standard = 1
    case enhanced = 2
    case premium = 3
    case personal = 4

    public static func < (lhs: SpeechVoiceQuality, rhs: SpeechVoiceQuality) -> Bool { lhs.rawValue < rhs.rawValue }

    public var label: String {
        switch self {
        case .standard: return "Standard"
        case .enhanced: return "Enhanced"
        case .premium: return "Premium"
        case .personal: return "Personal Voice"
        }
    }
}

public struct SpeechVoice: Equatable, Identifiable, Sendable {
    public let id: String
    public let name: String
    public let language: String
    public let quality: SpeechVoiceQuality
    public let novelty: Bool

    public init(id: String, name: String, language: String, quality: SpeechVoiceQuality, novelty: Bool = false) {
        self.id = id
        self.name = name
        self.language = language
        self.quality = quality
        self.novelty = novelty
    }

    public var robotic: Bool {
        id.hasPrefix("com.apple.eloquence") || id.hasPrefix("com.apple.speech.synthesis.voice")
    }

    public var natural: Bool { !novelty && !robotic }
}

public struct SpeechVoiceSection: Equatable, Identifiable, Sendable {
    public let id: String
    public let title: String
    public let voices: [SpeechVoice]
}

public enum PersonalVoiceStatus: Equatable, Sendable {
    case unsupported
    case notDetermined
    case denied
    case authorized
}

public struct SpeechSample: Equatable, Identifiable, Sendable {
    public var id: String { title }
    public let title: String
    public let text: String
}

public struct SpeechHighlight: Equatable, Sendable {
    public let before: String
    public let word: String
    public let after: String
}

public enum SpeechVoices {
    public static func languageMatch(_ voice: String, _ preferred: String) -> Int {
        let lhs = normalized(voice)
        let rhs = normalized(preferred)
        if lhs == rhs { return 2 }
        return base(lhs) == base(rhs) ? 1 : 0
    }

    public static func normalized(_ language: String) -> String {
        language.replacingOccurrences(of: "_", with: "-").lowercased()
    }

    public static func base(_ language: String) -> String {
        String(normalized(language).split(separator: "-").first ?? "")
    }

    public static func rank(_ voice: SpeechVoice, language: String) -> [Int] {
        [
            voice.natural ? 1 : 0,
            languageMatch(voice.language, language) > 0 ? 1 : 0,
            voice.quality.rawValue,
            languageMatch(voice.language, language),
        ]
    }

    public static func best(_ voices: [SpeechVoice], language: String) -> SpeechVoice? {
        voices.max { lhs, rhs in
            let left = rank(lhs, language: language)
            let right = rank(rhs, language: language)
            if left != right { return left.lexicographicallyPrecedes(right) }
            return lhs.name > rhs.name
        }
    }

    public static func hasNatural(_ voices: [SpeechVoice], language: String) -> Bool {
        voices.contains { $0.natural && $0.quality >= .enhanced && languageMatch($0.language, language) > 0 }
    }

    public static func sections(
        _ voices: [SpeechVoice], language: String, title: (String) -> String
    ) -> [SpeechVoiceSection] {
        let groups = Dictionary(grouping: voices.filter { !$0.novelty }) { normalized($0.language) }
        let ordered = groups.keys.sorted { lhs, rhs in
            let left = languageMatch(lhs, language)
            let right = languageMatch(rhs, language)
            if left != right { return left > right }
            return title(lhs).localizedCaseInsensitiveCompare(title(rhs)) == .orderedAscending
        }
        return ordered.map { key in
            let members = (groups[key] ?? []).sorted { lhs, rhs in
                if lhs.robotic != rhs.robotic { return !lhs.robotic }
                if lhs.quality != rhs.quality { return lhs.quality > rhs.quality }
                return lhs.name.localizedCaseInsensitiveCompare(rhs.name) == .orderedAscending
            }
            return SpeechVoiceSection(id: key, title: title(members.first?.language ?? key), voices: members)
        }
    }
}

public enum SpeechTuning {
    public static let speeds: ClosedRange<Double> = 0.5...2.0
    public static let pitches: ClosedRange<Double> = 0.5...2.0
    public static let defaultSpeed: Double = 1.0
    public static let defaultPitch: Double = 1.0

    public static func utteranceRate(speed: Double) -> Float {
        let clamped = min(max(speed, speeds.lowerBound), speeds.upperBound)
        if clamped <= 1 { return Float(0.25 + (clamped - 0.5) * 0.5) }
        return Float(0.5 + (clamped - 1) * 0.25)
    }

    public static func label(speed: Double) -> String {
        var digits = String(format: "%.2f", speed)
        while digits.hasSuffix("0") { digits.removeLast() }
        if digits.hasSuffix(".") { digits.removeLast() }
        return digits + "×"
    }

    public static func label(pitch: Double) -> String {
        if abs(pitch - 1) < 0.025 { return "Natural" }
        return pitch < 1 ? "Lower" : "Higher"
    }

    public static func estimatedSeconds(_ text: String, speed: Double) -> Int {
        let words = SpeechText.wordCount(text)
        guard words > 0 else { return 0 }
        return max(1, Int((Double(words) / (165 * max(speed, 0.1)) * 60).rounded()))
    }
}

public enum SpeechText {
    public static let maxLength = 20_000

    public static func wordCount(_ text: String) -> Int {
        text.split { $0.isWhitespace || $0.isNewline }.count
    }

    public static func highlight(_ text: String, location: Int, length: Int) -> SpeechHighlight? {
        let utf16 = text.utf16
        guard location >= 0, length > 0, location + length <= utf16.count else { return nil }
        let start = utf16.index(utf16.startIndex, offsetBy: location)
        let end = utf16.index(start, offsetBy: length)
        guard let lower = start.samePosition(in: text), let upper = end.samePosition(in: text) else { return nil }
        return SpeechHighlight(
            before: String(text[text.startIndex..<lower]), word: String(text[lower..<upper]),
            after: String(text[upper...]))
    }

    public static func duration(_ seconds: Int) -> String {
        if seconds < 60 { return "\(seconds) s" }
        return "\(seconds / 60) min \(seconds % 60) s"
    }

    public static func fileName(_ text: String) -> String {
        let words = text.split { !$0.isLetter && !$0.isNumber }.prefix(5).map { $0.lowercased() }
        let stem = words.joined(separator: "-")
        return (stem.isEmpty ? "speech" : String(stem.prefix(48))) + ".m4a"
    }
}

public enum SpeechPrefs {
    public static let voice = "speech.voice"
    public static let speed = "speech.speed"
    public static let pitch = "speech.pitch"
    public static let text = "speech.text"
}

@MainActor
public protocol SpeechEngine: AnyObject {
    var onRange: ((Int, Int) -> Void)? { get set }
    var onFinish: (() -> Void)? { get set }
    var personalVoiceStatus: PersonalVoiceStatus { get }

    func voices() -> [SpeechVoice]
    func speak(_ text: String, voiceId: String, rate: Float, pitch: Float)
    func pause()
    func resume()
    func stop()
    func requestPersonalVoice() async -> PersonalVoiceStatus
    func render(_ text: String, voiceId: String, rate: Float, pitch: Float, name: String) async throws -> URL
}

@MainActor
@Observable
public final class SpeechPlaygroundModel {
    public enum State: Equatable, Sendable {
        case idle
        case speaking
        case paused
    }

    public static let title = "Speech"
    public static let unavailableTitle = "Speech is unavailable"
    public static let placeholder = "Type or paste anything, then tap Speak."
    public static let noNaturalVoice =
        "For the most natural sound, download a Premium or Enhanced voice: Settings › Accessibility › Read & Speak › Voices. It appears here as soon as the download finishes."
    public static let personalVoiceHint =
        "Speak in your own voice: set up Personal Voice in Settings › Accessibility › Personal Voice, then allow ForkMesh to use it."
    public static let samples: [SpeechSample] = [
        SpeechSample(
            title: "Hello",
            text: "Hi there! This is ForkMesh speaking. Type anything you like into the box, pick a voice, and I'll read it back to you."),
        SpeechSample(
            title: "Story",
            text: "The lighthouse keeper climbed the spiral stairs one last time. Below, the sea was calm and silver, and far out on the water a single boat was turning for home."),
        SpeechSample(
            title: "Update",
            text: "Good morning. Three agents finished overnight: two pull requests are ready for review, and one test run needs your attention. Your next meeting starts at 10:30."),
        SpeechSample(
            title: "Poem",
            text: "Shall I compare thee to a summer's day? Thou art more lovely and more temperate. Rough winds do shake the darling buds of May, and summer's lease hath all too short a date."),
    ]

    @ObservationIgnored public let engine: SpeechEngine
    @ObservationIgnored public let prefs: KeyValueStore
    @ObservationIgnored public let language: String

    public var text: String
    public private(set) var voices: [SpeechVoice] = []
    public private(set) var voiceId: String
    public var speed: Double
    public var pitch: Double
    public private(set) var state: State = .idle
    public private(set) var spoken: (location: Int, length: Int)?
    public private(set) var spokenText = ""
    public private(set) var previewing: String?
    public private(set) var personalVoiceStatus: PersonalVoiceStatus = .unsupported
    public private(set) var rendering = false
    public var notice: ToolsNotice?

    public init(engine: SpeechEngine, prefs: KeyValueStore = InMemoryKeyValueStore(), language: String = "en-US") {
        self.engine = engine
        self.prefs = prefs
        self.language = language
        text = prefs.string(forKey: SpeechPrefs.text) ?? SpeechPlaygroundModel.samples[0].text
        voiceId = prefs.string(forKey: SpeechPrefs.voice) ?? ""
        speed = min(max(prefs.double(forKey: SpeechPrefs.speed) ?? SpeechTuning.defaultSpeed, SpeechTuning.speeds.lowerBound), SpeechTuning.speeds.upperBound)
        pitch = min(max(prefs.double(forKey: SpeechPrefs.pitch) ?? SpeechTuning.defaultPitch, SpeechTuning.pitches.lowerBound), SpeechTuning.pitches.upperBound)
        engine.onRange = { [weak self] location, length in self?.spokenRange(location, length) }
        engine.onFinish = { [weak self] in self?.finished() }
    }

    public func load() {
        voices = engine.voices()
        personalVoiceStatus = engine.personalVoiceStatus
        if voice == nil, let best = SpeechVoices.best(voices, language: language) {
            voiceId = best.id
        }
    }

    public var voice: SpeechVoice? { voices.first { $0.id == voiceId } }

    public var showsVoiceHint: Bool { !voices.isEmpty && !SpeechVoices.hasNatural(voices, language: language) }

    public var showsPersonalVoiceHint: Bool {
        personalVoiceStatus == .notDetermined
            || (personalVoiceStatus == .authorized && !voices.contains { $0.quality == .personal })
    }

    public var trimmed: String { String(text.trimmingCharacters(in: .whitespacesAndNewlines).prefix(SpeechText.maxLength)) }

    public var canSpeak: Bool { !trimmed.isEmpty && voice != nil }

    public var highlight: SpeechHighlight? {
        guard previewing == nil, state != .idle, let spoken else { return nil }
        return SpeechText.highlight(spokenText, location: spoken.location, length: spoken.length)
    }

    public var summary: String {
        let words = SpeechText.wordCount(text)
        guard words > 0 else { return "" }
        let seconds = SpeechTuning.estimatedSeconds(text, speed: speed)
        return "\(words) word\(words == 1 ? "" : "s") · about \(SpeechText.duration(seconds))"
    }

    public func select(_ voice: SpeechVoice) {
        voiceId = voice.id
        prefs.set(voice.id, forKey: SpeechPrefs.voice)
    }

    public func tuningChanged() {
        prefs.set(speed, forKey: SpeechPrefs.speed)
        prefs.set(pitch, forKey: SpeechPrefs.pitch)
    }

    public func resetTuning() {
        speed = SpeechTuning.defaultSpeed
        pitch = SpeechTuning.defaultPitch
        tuningChanged()
    }

    public func useSample(_ sample: SpeechSample) {
        stop()
        text = sample.text
        prefs.set(text, forKey: SpeechPrefs.text)
    }

    public func clear() {
        stop()
        text = ""
        prefs.set(text, forKey: SpeechPrefs.text)
    }

    public func toggle() {
        switch state {
        case .idle: speak()
        case .speaking: pause()
        case .paused: resume()
        }
    }

    public func speak() {
        guard let voice, !trimmed.isEmpty else { return }
        if state != .idle { engine.stop() }
        prefs.set(text, forKey: SpeechPrefs.text)
        previewing = nil
        start(trimmed, voice: voice)
    }

    public func preview(_ voice: SpeechVoice) {
        if previewing == voice.id && state != .idle {
            stop()
            return
        }
        if state != .idle { engine.stop() }
        previewing = voice.id
        start(Self.previewLine(voice), voice: voice)
    }

    public static func previewLine(_ voice: SpeechVoice) -> String {
        guard SpeechVoices.base(voice.language) == "en" else { return voice.name }
        return "Hi, I'm \(voice.name). This is how I sound reading your words aloud."
    }

    private func start(_ line: String, voice: SpeechVoice) {
        spokenText = line
        spoken = nil
        state = .speaking
        engine.speak(line, voiceId: voice.id, rate: SpeechTuning.utteranceRate(speed: speed), pitch: Float(pitch))
    }

    public func pause() {
        guard state == .speaking else { return }
        engine.pause()
        state = .paused
    }

    public func resume() {
        guard state == .paused else { return }
        engine.resume()
        state = .speaking
    }

    public func stop() {
        guard state != .idle else { return }
        engine.stop()
        reset()
    }

    private func reset() {
        state = .idle
        spoken = nil
        previewing = nil
    }

    func spokenRange(_ location: Int, _ length: Int) {
        guard state != .idle else { return }
        spoken = (location, length)
    }

    func finished() {
        reset()
    }

    public func requestPersonalVoice() async {
        personalVoiceStatus = await engine.requestPersonalVoice()
        voices = engine.voices()
        if personalVoiceStatus == .authorized, let personal = voices.first(where: { $0.quality == .personal }) {
            select(personal)
            notice = ToolsNotice("Personal Voice is ready.")
        } else if personalVoiceStatus == .denied {
            notice = ToolsNotice("Personal Voice is off for ForkMesh. Turn it on in Settings › Accessibility › Personal Voice.", isError: true)
        }
    }

    public func renderAudio() async -> URL? {
        guard let voice, !trimmed.isEmpty, !rendering else { return nil }
        rendering = true
        defer { rendering = false }
        do {
            return try await engine.render(
                trimmed, voiceId: voice.id, rate: SpeechTuning.utteranceRate(speed: speed), pitch: Float(pitch),
                name: SpeechText.fileName(trimmed))
        } catch {
            notice = ToolsNotice("Could not save the audio: \(error.localizedDescription)", isError: true)
            return nil
        }
    }
}
