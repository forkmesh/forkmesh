import AVFoundation
import FMAssistant
import FMBase
import FMTools
import Foundation

@MainActor
final class GenieSpeechVoice: GenieVoice {
    private let speech: AppleSpeechEngine = AppleSpeechEngine()
    private let prefs: KeyValueStore?
    private var dictation: PromptDictation?
    private var speaking: CheckedContinuation<Void, Never>?
    private var speakingTimeout: Task<Void, Never>?

    init(prefs: KeyValueStore? = Services.get(KeyValueStore.self)) {
        self.prefs = prefs
        speech.onFinish = { [weak self] in self?.finishSpeaking() }
    }

    func say(_ text: String) async {
        let line: String = text.dartTrimmed
        guard !line.isEmpty, !Task.isCancelled else { return }
        finishSpeaking()
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            speaking = continuation
            speech.speak(line, voiceId: voiceId, rate: rate, pitch: pitch)
            let seconds: Double = 4 + Double(line.count) * 0.09
            speakingTimeout = Task { @MainActor [weak self] in
                try? await Task.sleep(for: .seconds(seconds))
                if !Task.isCancelled { self?.finishSpeaking() }
            }
        }
    }

    func listen(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard {
        await hear(ambient: false, onWords: onWords)
    }

    func listenAmbient(onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard {
        guard PromptDictation.mayOverhear else { return .silence }
        return await hear(ambient: true, onWords: onWords)
    }

    private func hear(ambient: Bool, onWords: @escaping @MainActor (String) -> Void) async -> GenieHeard {
        guard !Task.isCancelled else { return .silence }
        stopListening()
        let listener: PromptDictation = PromptDictation()
        listener.pause = 2
        listener.ambient = ambient
        dictation = listener
        let hearing: GenieHearing = GenieHearing(onWords: onWords)
        let heard: GenieHeard = await withCheckedContinuation { (continuation: CheckedContinuation<GenieHeard, Never>) in
            hearing.continuation = continuation
            Task { @MainActor in
                _ = await listener.start(
                    onWords: { (words: String) in hearing.heard(words) },
                    onDone: { (_: Bool) in hearing.finish() },
                    onError: { (message: String) in hearing.problem = message })
            }
        }
        if dictation === listener { dictation = nil }
        return heard
    }

    func stopListening() {
        let current: PromptDictation? = dictation
        dictation = nil
        current?.stop()
    }

    func stopSpeaking() {
        speech.stop()
        finishSpeaking()
    }

    private func finishSpeaking() {
        speakingTimeout?.cancel()
        speakingTimeout = nil
        let waiter: CheckedContinuation<Void, Never>? = speaking
        speaking = nil
        waiter?.resume()
    }

    private var voiceId: String {
        if let chosen = prefs?.string(forKey: SpeechPrefs.voice), !chosen.isEmpty {
            return chosen
        }
        let language: String = AppleSpeechEngine.language
        let voices: [AVSpeechSynthesisVoice] = AVSpeechSynthesisVoice.speechVoices().filter { $0.language == language }
        let best: AVSpeechSynthesisVoice? = voices.first { $0.quality == .premium } ?? voices.first { $0.quality == .enhanced }
        return best?.identifier ?? AVSpeechSynthesisVoice(language: language)?.identifier ?? ""
    }

    private var rate: Float {
        SpeechTuning.utteranceRate(speed: prefs?.double(forKey: SpeechPrefs.speed) ?? SpeechTuning.defaultSpeed)
    }

    private var pitch: Float {
        let chosen: Double = prefs?.double(forKey: SpeechPrefs.pitch) ?? 0.92
        return Float(min(max(chosen, SpeechTuning.pitches.lowerBound), SpeechTuning.pitches.upperBound))
    }
}

@MainActor
final class GenieHearing {
    let onWords: @MainActor (String) -> Void
    var words: String = ""
    var problem: String?
    var continuation: CheckedContinuation<GenieHeard, Never>?

    init(onWords: @escaping @MainActor (String) -> Void) {
        self.onWords = onWords
    }

    func heard(_ text: String) {
        words = text
        onWords(text)
    }

    func finish() {
        let waiter: CheckedContinuation<GenieHeard, Never>? = continuation
        continuation = nil
        let said: String = words.dartTrimmed
        if said.isEmpty, let problem = problem {
            waiter?.resume(returning: .failed(problem))
        } else {
            waiter?.resume(returning: said.isEmpty ? .silence : .words(said))
        }
    }
}
