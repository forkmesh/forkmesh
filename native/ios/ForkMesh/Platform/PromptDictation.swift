// The composer's dictation over the Speech framework. It listens only while it
// is on, recognizes on the device when the device can, keeps nothing (the audio
// buffers go straight to the recognizer and nothing is written to disk), and
// stops after a stretch of quiet or two minutes in all.

import AVFoundation
import FMAccount
import FMBase
import Foundation
import Speech

enum PromptSpeechModel: Equatable {
    case olderSystem
    case unsupported
    case missing
    case installed
    case failed(String)
}

private enum PromptDictationError: LocalizedError {
    case noMicrophone

    var errorDescription: String? { "no microphone input is available" }
}

@MainActor
final class PromptDictation {
    private var engine: AVAudioEngine?
    private var request: SFSpeechAudioBufferRecognitionRequest?
    private var task: SFSpeechRecognitionTask?
    private var cancelAnalysis: (() -> Void)?
    private var silence: Task<Void, Never>?
    private var limit: Task<Void, Never>?
    private var onDone: ((Bool) -> Void)?
    private var generation: Int = 0
    private var heard: Bool = false
    private var settled: Bool = false
    private var metered: Int?

    private(set) var listening: Bool = false
    private(set) static var isListening: Bool = false
    private(set) static var isOverhearing: Bool = false
    private static weak var overhearing: PromptDictation?
    var pause: UInt64 = 4
    var ambient: Bool = false

    static var allowed: Bool {
        SFSpeechRecognizer.authorizationStatus() == .authorized && AVAudioApplication.shared.recordPermission == .granted
    }

    private(set) static var heldUntil: Date = .distantPast

    static func yieldOverhearing() {
        overhearing?.stop()
    }

    static func holdOverhearing(_ seconds: Double) {
        heldUntil = max(heldUntil, Date().addingTimeInterval(seconds))
        yieldOverhearing()
    }

    static var mayOverhear: Bool {
        !isListening && !MessagesVoiceRecorder.isRecording && !AppleSpeechEngine.talking && Date() >= heldUntil
    }

    static var refused: Bool {
        let speech: SFSpeechRecognizerAuthorizationStatus = SFSpeechRecognizer.authorizationStatus()
        return speech == .denied || speech == .restricted || AVAudioApplication.shared.recordPermission == .denied
    }

    /// Start listening. `onWords` receives the whole utterance so far each
    /// time the recognizer revises it; `onDone` fires once listening ends for
    /// any reason, with true when it ended on a quiet moment after words were
    /// heard. False when dictation is unavailable or refused, with a sentence
    /// for the person in `onError`.
    func start(
        onWords: @escaping (String) -> Void,
        onDone: @escaping (Bool) -> Void,
        onError: @escaping (String) -> Void
    ) async -> Bool {
        if listening {
            return true
        }
        if ambient {
            Self.overhearing = self
        } else {
            Self.yieldOverhearing()
        }
        generation += 1
        let attempt: Int = generation
        self.onDone = onDone
        heard = false
        settled = false
        let allowed: Bool = await Self.authorized()
        guard attempt == generation else {
            return false
        }
        guard allowed else {
            onError("Dictation is not available. Allow microphone and speech recognition for ForkMesh in Settings.")
            finish()
            return false
        }
        let identifier: String = Self.localeIdentifier()
        if #available(iOS 26.0, *), let analysis = await PromptSpeechAnalysis.ready(identifier: identifier) {
            guard attempt == generation else {
                return false
            }
            return beginAnalysis(analysis, onWords: onWords, onError: onError)
        }
        guard attempt == generation else {
            return false
        }
        return beginRecognizer(identifier: identifier, onWords: onWords, onError: onError)
    }

    func stop() {
        generation += 1
        request?.endAudio()
        finish()
    }

    static func modelStatus() async -> PromptSpeechModel {
        if #available(iOS 26.0, *) {
            return await PromptSpeechAnalysis.status(identifier: localeIdentifier())
        }
        return .olderSystem
    }

    static func installModel() async -> PromptSpeechModel {
        if #available(iOS 26.0, *) {
            return await PromptSpeechAnalysis.install(identifier: localeIdentifier())
        }
        return .olderSystem
    }

    @available(iOS 26.0, *)
    private func beginAnalysis(
        _ analysis: PromptSpeechAnalysis,
        onWords: @escaping (String) -> Void,
        onError: @escaping (String) -> Void
    ) -> Bool {
        do {
            try startEngine(PromptSpeechAnalysis.tap(analysis.feed))
        } catch {
            analysis.cancel()
            onError("Dictation could not start: \(error.localizedDescription)")
            finish()
            return false
        }
        markListening()
        let attempt: Int = generation
        cancelAnalysis = { analysis.cancel() }
        analysis.run(
            onWords: { [weak self] (words: String) in
                guard let self = self, self.listening, self.generation == attempt else {
                    return
                }
                onWords(words)
                self.heardWords()
            },
            onEnd: { [weak self] in
                guard let self = self, self.listening, self.generation == attempt else {
                    return
                }
                self.finish()
            },
            onError: { [weak self] (message: String) in
                guard let self = self, self.listening, self.generation == attempt else {
                    return
                }
                onError(message)
                self.finish()
            })
        return true
    }

    private func beginRecognizer(
        identifier: String,
        onWords: @escaping (String) -> Void,
        onError: @escaping (String) -> Void
    ) -> Bool {
        guard let recognizer = Self.makeRecognizer(identifier), recognizer.isAvailable else {
            onError("Speech recognition is not available right now. Try again in a moment.")
            finish()
            return false
        }
        if ambient && !recognizer.supportsOnDeviceRecognition {
            onError("Always listening needs on-device speech recognition, which this iPhone does not have for this language.")
            finish()
            return false
        }
        let request: SFSpeechAudioBufferRecognitionRequest = SFSpeechAudioBufferRecognitionRequest()
        request.shouldReportPartialResults = true
        request.taskHint = .dictation
        if recognizer.supportsOnDeviceRecognition {
            request.requiresOnDeviceRecognition = true
        }
        do {
            try startEngine(Self.recognizerTap(request))
        } catch {
            onError("Dictation could not start: \(error.localizedDescription)")
            finish()
            return false
        }
        self.request = request
        markListening()
        let attempt: Int = generation
        task = recognizer.recognitionTask(with: request) { [weak self] (result: SFSpeechRecognitionResult?, error: Error?) in
            let words: String? = result?.bestTranscription.formattedString
            let final: Bool = result?.isFinal ?? false
            let failed: Bool = error != nil
            Task { @MainActor in
                guard let self = self, self.listening, self.generation == attempt else {
                    return
                }
                if let words = words {
                    onWords(words)
                    self.heardWords()
                }
                if final || failed {
                    self.finish()
                }
            }
        }
        return true
    }

    private func startEngine(_ tap: @escaping AVAudioNodeTapBlock) throws {
        let session: AVAudioSession = AVAudioSession.sharedInstance()
        let options: AVAudioSession.CategoryOptions = ambient ? [.mixWithOthers, .defaultToSpeaker] : [.duckOthers, .defaultToSpeaker]
        try session.setCategory(.playAndRecord, mode: .measurement, options: options)
        try session.setActive(true, options: .notifyOthersOnDeactivation)
        guard session.isInputAvailable else {
            throw PromptDictationError.noMicrophone
        }
        let fresh: AVAudioEngine = AVAudioEngine()
        let input: AVAudioInputNode = fresh.inputNode
        let format: AVAudioFormat = input.inputFormat(forBus: 0)
        guard format.sampleRate > 0, format.channelCount > 0 else {
            throw PromptDictationError.noMicrophone
        }
        let meter: Int = DictationMeter.shared.begin()
        metered = meter
        input.installTap(onBus: 0, bufferSize: 1024, format: format, block: DictationMeter.tap(tap, session: meter))
        engine = fresh
        fresh.prepare()
        try fresh.start()
    }

    private func markListening() {
        listening = true
        if ambient {
            Self.isOverhearing = true
        } else {
            Self.isListening = true
        }
        armSilence(seconds: ambient ? 55 : 8)
        limit = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 120 * 1_000_000_000)
            if !Task.isCancelled {
                self?.stop()
            }
        }
    }

    private func heardWords() {
        heard = true
        armSilence(seconds: pause)
    }

    private func armSilence(seconds: UInt64) {
        silence?.cancel()
        silence = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: seconds * 1_000_000_000)
            if !Task.isCancelled, let self = self {
                self.settled = self.heard
                self.stop()
            }
        }
    }

    private func finish() {
        silence?.cancel()
        limit?.cancel()
        silence = nil
        limit = nil
        if let engine {
            if engine.isRunning {
                engine.stop()
            }
            engine.inputNode.removeTap(onBus: 0)
        }
        engine = nil
        if let meter = metered {
            DictationMeter.shared.end(meter)
        }
        metered = nil
        task?.cancel()
        task = nil
        request = nil
        let cancelAnalysis: (() -> Void)? = self.cancelAnalysis
        self.cancelAnalysis = nil
        cancelAnalysis?()
        try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
        if listening {
            if ambient {
                Self.isOverhearing = false
            } else {
                Self.isListening = false
            }
        }
        listening = false
        let quiet: Bool = settled
        settled = false
        heard = false
        let done: ((Bool) -> Void)? = onDone
        onDone = nil
        done?(quiet)
    }

    nonisolated private static func recognizerTap(_ request: SFSpeechAudioBufferRecognitionRequest) -> AVAudioNodeTapBlock {
        return { (buffer: AVAudioPCMBuffer, _: AVAudioTime) in
            request.append(buffer)
        }
    }

    private static func localeIdentifier() -> String {
        Services.get(KeyValueStore.self).map { (prefs: KeyValueStore) -> String in
            SettingsService.dictationLocale(prefs)
        } ?? ""
    }

    private static func makeRecognizer(_ identifier: String) -> SFSpeechRecognizer? {
        if !identifier.isEmpty, let chosen: SFSpeechRecognizer = SFSpeechRecognizer(locale: Locale(identifier: identifier)) {
            return chosen
        }
        return SFSpeechRecognizer()
    }

    /// Speech recognition and the microphone, asked for the first time only.
    private static func authorized() async -> Bool {
        let speech: SFSpeechRecognizerAuthorizationStatus = await withCheckedContinuation {
            (continuation: CheckedContinuation<SFSpeechRecognizerAuthorizationStatus, Never>) in
            SFSpeechRecognizer.requestAuthorization { (status: SFSpeechRecognizerAuthorizationStatus) in
                continuation.resume(returning: status)
            }
        }
        if speech != .authorized {
            return false
        }
        return await AVAudioApplication.requestRecordPermission()
    }
}

@available(iOS 26.0, *)
@MainActor
final class PromptSpeechAnalysis {
    let feed: PromptAnalyzerFeed
    private let transcriber: SpeechTranscriber
    private let analyzer: SpeechAnalyzer
    private let stream: AsyncStream<AnalyzerInput>
    private var tasks: [Task<Void, Never>] = []
    private static var installing: Task<PromptSpeechModel, Never>?

    private init(transcriber: SpeechTranscriber, format: AVAudioFormat) {
        self.transcriber = transcriber
        analyzer = SpeechAnalyzer(modules: [transcriber])
        let (stream, continuation) = AsyncStream<AnalyzerInput>.makeStream()
        self.stream = stream
        feed = PromptAnalyzerFeed(continuation: continuation, format: format)
    }

    static func ready(identifier: String) async -> PromptSpeechAnalysis? {
        guard let locale: Locale = await locale(identifier) else {
            return nil
        }
        guard await installed(locale) else {
            Task { @MainActor in
                _ = await install(identifier: identifier)
            }
            return nil
        }
        let transcriber: SpeechTranscriber = makeTranscriber(locale)
        guard let format: AVAudioFormat = try? await SpeechAnalyzer.bestAvailableAudioFormat(compatibleWith: [transcriber]) else {
            return nil
        }
        return PromptSpeechAnalysis(transcriber: transcriber, format: format)
    }

    static func status(identifier: String) async -> PromptSpeechModel {
        guard let locale: Locale = await locale(identifier) else {
            return .unsupported
        }
        return await installed(locale) ? .installed : .missing
    }

    static func install(identifier: String) async -> PromptSpeechModel {
        if let running: Task<PromptSpeechModel, Never> = installing {
            return await running.value
        }
        let job: Task<PromptSpeechModel, Never> = Task { @MainActor in
            guard let locale: Locale = await locale(identifier) else {
                return .unsupported
            }
            if await installed(locale) {
                return .installed
            }
            do {
                if let request = try await AssetInventory.assetInstallationRequest(supporting: [makeTranscriber(locale)]) {
                    try await request.downloadAndInstall()
                }
            } catch {
                return .failed(error.localizedDescription)
            }
            return await installed(locale) ? .installed : .missing
        }
        installing = job
        let result: PromptSpeechModel = await job.value
        installing = nil
        return result
    }

    nonisolated static func tap(_ feed: PromptAnalyzerFeed) -> AVAudioNodeTapBlock {
        return { (buffer: AVAudioPCMBuffer, _: AVAudioTime) in
            feed.append(buffer)
        }
    }

    nonisolated static func join(_ settled: String, _ words: String) -> String {
        if settled.isEmpty || words.isEmpty {
            return settled + words
        }
        if settled.last?.isWhitespace == true || words.first?.isWhitespace == true {
            return settled + words
        }
        return settled + " " + words
    }

    func run(
        onWords: @escaping (String) -> Void,
        onEnd: @escaping () -> Void,
        onError: @escaping (String) -> Void
    ) {
        let transcriber: SpeechTranscriber = self.transcriber
        let analyzer: SpeechAnalyzer = self.analyzer
        let stream: AsyncStream<AnalyzerInput> = self.stream
        tasks.append(Task { @MainActor in
            var settled: String = ""
            do {
                for try await result in transcriber.results {
                    let joined: String = PromptSpeechAnalysis.join(settled, String(result.text.characters))
                    if result.isFinal {
                        settled = joined
                    }
                    onWords(joined.trimmingCharacters(in: .whitespacesAndNewlines))
                }
            } catch {
                if !Task.isCancelled {
                    onError("Dictation stopped: \(error.localizedDescription)")
                    return
                }
            }
            onEnd()
        })
        tasks.append(Task { @MainActor in
            do {
                try await analyzer.start(inputSequence: stream)
            } catch {
                if !Task.isCancelled {
                    onError("Dictation could not start: \(error.localizedDescription)")
                }
            }
        })
    }

    func cancel() {
        feed.finish()
        for task in tasks {
            task.cancel()
        }
        tasks = []
        let analyzer: SpeechAnalyzer = self.analyzer
        Task {
            try? await analyzer.cancelAndFinishNow()
        }
    }

    private static func makeTranscriber(_ locale: Locale) -> SpeechTranscriber {
        SpeechTranscriber(locale: locale, transcriptionOptions: [], reportingOptions: [.volatileResults], attributeOptions: [])
    }

    private static func locale(_ identifier: String) async -> Locale? {
        let wanted: Locale = identifier.isEmpty ? Locale.current : Locale(identifier: identifier)
        let supported: [Locale] = await SpeechTranscriber.supportedLocales
        let tag: String = wanted.identifier(.bcp47)
        if let exact: Locale = supported.first(where: { $0.identifier(.bcp47) == tag }) {
            return exact
        }
        let language: Locale.LanguageCode? = wanted.language.languageCode
        return supported.first { (candidate: Locale) -> Bool in
            candidate.language.languageCode == language
        }
    }

    private static func installed(_ locale: Locale) async -> Bool {
        let tag: String = locale.identifier(.bcp47)
        return await SpeechTranscriber.installedLocales.contains { (candidate: Locale) -> Bool in
            candidate.identifier(.bcp47) == tag
        }
    }
}

@available(iOS 26.0, *)
final class PromptAnalyzerFeed: @unchecked Sendable {
    private let continuation: AsyncStream<AnalyzerInput>.Continuation
    private let format: AVAudioFormat
    private var converter: AVAudioConverter?

    init(continuation: AsyncStream<AnalyzerInput>.Continuation, format: AVAudioFormat) {
        self.continuation = continuation
        self.format = format
    }

    func append(_ buffer: AVAudioPCMBuffer) {
        guard let converted: AVAudioPCMBuffer = convert(buffer) else {
            return
        }
        continuation.yield(AnalyzerInput(buffer: converted))
    }

    func finish() {
        continuation.finish()
    }

    private func convert(_ buffer: AVAudioPCMBuffer) -> AVAudioPCMBuffer? {
        if buffer.format == format {
            return buffer
        }
        if converter == nil || converter?.inputFormat != buffer.format {
            converter = AVAudioConverter(from: buffer.format, to: format)
            converter?.primeMethod = .none
        }
        guard let converter: AVAudioConverter = converter, buffer.format.sampleRate > 0 else {
            return nil
        }
        let ratio: Double = format.sampleRate / buffer.format.sampleRate
        let capacity: AVAudioFrameCount = AVAudioFrameCount((Double(buffer.frameLength) * ratio).rounded(.up)) + 1
        guard let output: AVAudioPCMBuffer = AVAudioPCMBuffer(pcmFormat: format, frameCapacity: capacity) else {
            return nil
        }
        var supplied: Bool = false
        var failure: NSError?
        let status: AVAudioConverterOutputStatus = converter.convert(to: output, error: &failure) {
            (_: AVAudioPacketCount, state: UnsafeMutablePointer<AVAudioConverterInputStatus>) -> AVAudioBuffer? in
            if supplied {
                state.pointee = .noDataNow
                return nil
            }
            supplied = true
            state.pointee = .haveData
            return buffer
        }
        if status == .error || output.frameLength == 0 {
            return nil
        }
        return output
    }
}
