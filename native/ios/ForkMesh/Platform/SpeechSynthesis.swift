import AVFoundation
import FMTools
import Foundation

enum SpeechRenderError: LocalizedError {
    case silent

    var errorDescription: String? { "the voice produced no audio" }
}

@MainActor
final class AppleSpeechEngine: NSObject, SpeechEngine, AVSpeechSynthesizerDelegate {
    var onRange: ((Int, Int) -> Void)?
    var onFinish: (() -> Void)?

    private let synthesizer: AVSpeechSynthesizer = AVSpeechSynthesizer()
    private var current: ObjectIdentifier?
    private var job: SpeechRenderJob?

    override init() {
        super.init()
        synthesizer.delegate = self
    }

    var personalVoiceStatus: PersonalVoiceStatus {
        Self.status(AVSpeechSynthesizer.personalVoiceAuthorizationStatus)
    }

    static var language: String { AVSpeechSynthesisVoice.currentLanguageCode() }

    private static func status(_ status: AVSpeechSynthesizer.PersonalVoiceAuthorizationStatus) -> PersonalVoiceStatus {
        switch status {
        case .notDetermined: return .notDetermined
        case .denied: return .denied
        case .authorized: return .authorized
        case .unsupported: return .unsupported
        @unknown default: return .unsupported
        }
    }

    func voices() -> [SpeechVoice] {
        AVSpeechSynthesisVoice.speechVoices().map { (voice: AVSpeechSynthesisVoice) -> SpeechVoice in
            let quality: SpeechVoiceQuality
            if voice.voiceTraits.contains(.isPersonalVoice) {
                quality = .personal
            } else {
                switch voice.quality {
                case .premium: quality = .premium
                case .enhanced: quality = .enhanced
                default: quality = .standard
                }
            }
            return SpeechVoice(
                id: voice.identifier, name: voice.name, language: voice.language, quality: quality,
                novelty: voice.voiceTraits.contains(.isNoveltyVoice))
        }
    }

    static func utterance(_ text: String, voiceId: String, rate: Float, pitch: Float) -> AVSpeechUtterance {
        let utterance: AVSpeechUtterance = AVSpeechUtterance(string: text)
        utterance.voice = AVSpeechSynthesisVoice(identifier: voiceId)
        utterance.rate = min(max(rate, AVSpeechUtteranceMinimumSpeechRate), AVSpeechUtteranceMaximumSpeechRate)
        utterance.pitchMultiplier = min(max(pitch, 0.5), 2.0)
        utterance.prefersAssistiveTechnologySettings = false
        utterance.preUtteranceDelay = 0.05
        return utterance
    }

    func speak(_ text: String, voiceId: String, rate: Float, pitch: Float) {
        if synthesizer.isSpeaking {
            current = nil
            synthesizer.stopSpeaking(at: .immediate)
        }
        activateSession()
        let utterance: AVSpeechUtterance = Self.utterance(text, voiceId: voiceId, rate: rate, pitch: pitch)
        current = ObjectIdentifier(utterance)
        synthesizer.speak(utterance)
    }

    func pause() {
        synthesizer.pauseSpeaking(at: .word)
    }

    func resume() {
        activateSession()
        synthesizer.continueSpeaking()
    }

    func stop() {
        current = nil
        synthesizer.stopSpeaking(at: .immediate)
    }

    func requestPersonalVoice() async -> PersonalVoiceStatus {
        let granted: AVSpeechSynthesizer.PersonalVoiceAuthorizationStatus = await withCheckedContinuation {
            (continuation: CheckedContinuation<AVSpeechSynthesizer.PersonalVoiceAuthorizationStatus, Never>) in
            AVSpeechSynthesizer.requestPersonalVoiceAuthorization { (status: AVSpeechSynthesizer.PersonalVoiceAuthorizationStatus) in
                continuation.resume(returning: status)
            }
        }
        return Self.status(granted)
    }

    func render(_ text: String, voiceId: String, rate: Float, pitch: Float, name: String) async throws -> URL {
        let folder: URL = FileManager.default.temporaryDirectory
            .appendingPathComponent("speech-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let url: URL = folder.appendingPathComponent(name)
        let job: SpeechRenderJob = SpeechRenderJob(url: url)
        self.job = job
        defer { self.job = nil }
        try await job.run(Self.utterance(text, voiceId: voiceId, rate: rate, pitch: pitch))
        return url
    }

    private(set) static var talking: Bool = false

    private func activateSession() {
        Self.talking = true
        PromptDictation.yieldOverhearing()
        let session: AVAudioSession = AVAudioSession.sharedInstance()
        try? session.setCategory(.playback, mode: .spokenAudio, options: OtherAppCall.duckingOptions)
        try? session.setActive(true)
    }

    private func deactivateSession() {
        guard !PromptDictation.isListening else { return }
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
    }

    private func ended(_ utterance: ObjectIdentifier) {
        if !synthesizer.isSpeaking {
            Self.talking = false
            deactivateSession()
        }
        guard utterance == current else { return }
        current = nil
        onFinish?()
    }

    nonisolated func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, willSpeakRangeOfSpeechString characterRange: NSRange, utterance: AVSpeechUtterance) {
        let id: ObjectIdentifier = ObjectIdentifier(utterance)
        let location: Int = characterRange.location
        let length: Int = characterRange.length
        Task { @MainActor [weak self] in
            guard let self, id == self.current else { return }
            self.onRange?(location, length)
        }
    }

    nonisolated func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance) {
        let id: ObjectIdentifier = ObjectIdentifier(utterance)
        Task { @MainActor [weak self] in self?.ended(id) }
    }

    nonisolated func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didCancel utterance: AVSpeechUtterance) {
        let id: ObjectIdentifier = ObjectIdentifier(utterance)
        Task { @MainActor [weak self] in self?.ended(id) }
    }
}

final class SpeechRenderJob: NSObject, AVSpeechSynthesizerDelegate, @unchecked Sendable {
    private let synthesizer: AVSpeechSynthesizer = AVSpeechSynthesizer()
    private let url: URL
    private let lock: NSLock = NSLock()
    private var file: AVAudioFile?
    private var failure: Error?
    private var continuation: CheckedContinuation<Void, Error>?

    init(url: URL) {
        self.url = url
        super.init()
        synthesizer.delegate = self
    }

    func run(_ utterance: AVSpeechUtterance) async throws {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            lock.lock()
            self.continuation = continuation
            lock.unlock()
            synthesizer.write(utterance) { [weak self] (buffer: AVAudioBuffer) in
                self?.append(buffer)
            }
        }
    }

    private func append(_ buffer: AVAudioBuffer) {
        guard let pcm = buffer as? AVAudioPCMBuffer else { return }
        if pcm.frameLength == 0 {
            finish()
            return
        }
        lock.lock()
        defer { lock.unlock() }
        guard continuation != nil, failure == nil else { return }
        do {
            if file == nil {
                let settings: [String: Any] = [
                    AVFormatIDKey: kAudioFormatMPEG4AAC,
                    AVSampleRateKey: pcm.format.sampleRate,
                    AVNumberOfChannelsKey: pcm.format.channelCount,
                    AVEncoderAudioQualityKey: AVAudioQuality.high.rawValue,
                ]
                file = try AVAudioFile(
                    forWriting: url, settings: settings, commonFormat: pcm.format.commonFormat,
                    interleaved: pcm.format.isInterleaved)
            }
            try file?.write(from: pcm)
        } catch {
            failure = error
        }
        if failure != nil {
            DispatchQueue.main.async { [weak self] in
                self?.synthesizer.stopSpeaking(at: .immediate)
                self?.finish()
            }
        }
    }

    private func finish() {
        lock.lock()
        let continuation: CheckedContinuation<Void, Error>? = self.continuation
        self.continuation = nil
        let wrote: Bool = file != nil
        file = nil
        let failure: Error? = self.failure
        lock.unlock()
        guard let continuation else { return }
        if let failure {
            continuation.resume(throwing: failure)
        } else if !wrote {
            continuation.resume(throwing: SpeechRenderError.silent)
        } else {
            continuation.resume()
        }
    }

    func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didFinish utterance: AVSpeechUtterance) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in self?.finish() }
    }

    func speechSynthesizer(_ synthesizer: AVSpeechSynthesizer, didCancel utterance: AVSpeechUtterance) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.4) { [weak self] in self?.finish() }
    }
}
