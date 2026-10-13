import FMBase
import XCTest
@testable import FMTools

@MainActor
final class FakeSpeechEngine: SpeechEngine {
    var onRange: ((Int, Int) -> Void)?
    var onFinish: (() -> Void)?
    var personalVoiceStatus: PersonalVoiceStatus = .notDetermined
    var installed: [SpeechVoice] = []
    var grant: PersonalVoiceStatus = .authorized
    var personal: SpeechVoice?
    var spoken: [(text: String, voiceId: String, rate: Float, pitch: Float)] = []
    var calls: [String] = []
    var renderFails = false

    struct Failure: LocalizedError {
        var errorDescription: String? { "disk full" }
    }

    func voices() -> [SpeechVoice] { installed }

    func speak(_ text: String, voiceId: String, rate: Float, pitch: Float) {
        spoken.append((text, voiceId, rate, pitch))
        calls.append("speak")
    }

    func pause() { calls.append("pause") }
    func resume() { calls.append("resume") }
    func stop() { calls.append("stop") }

    func requestPersonalVoice() async -> PersonalVoiceStatus {
        personalVoiceStatus = grant
        if grant == .authorized, let personal { installed.append(personal) }
        return grant
    }

    func render(_ text: String, voiceId: String, rate: Float, pitch: Float, name: String) async throws -> URL {
        if renderFails { throw Failure() }
        return URL(fileURLWithPath: "/tmp/" + name)
    }
}

@MainActor
final class SpeechTests: XCTestCase {
    let samantha = SpeechVoice(id: "com.apple.voice.compact.en-US.Samantha", name: "Samantha", language: "en-US", quality: .standard)
    let ava = SpeechVoice(id: "com.apple.voice.premium.en-US.Ava", name: "Ava", language: "en-US", quality: .premium)
    let serena = SpeechVoice(id: "com.apple.voice.premium.en-GB.Serena", name: "Serena", language: "en-GB", quality: .premium)
    let zoe = SpeechVoice(id: "com.apple.voice.enhanced.en-US.Zoe", name: "Zoe", language: "en-US", quality: .enhanced)
    let thomas = SpeechVoice(id: "com.apple.voice.premium.fr-FR.Thomas", name: "Thomas", language: "fr-FR", quality: .premium)
    let zarvox = SpeechVoice(id: "com.apple.speech.synthesis.voice.Zarvox", name: "Zarvox", language: "en-US", quality: .standard, novelty: true)
    let eddy = SpeechVoice(id: "com.apple.eloquence.en-US.Eddy", name: "Eddy", language: "en-US", quality: .standard)
    let mine = SpeechVoice(id: "personal.1", name: "My Voice", language: "en-US", quality: .personal)

    func model(_ voices: [SpeechVoice], prefs: KeyValueStore = InMemoryKeyValueStore()) -> (SpeechPlaygroundModel, FakeSpeechEngine) {
        let engine = FakeSpeechEngine()
        engine.installed = voices
        let model = SpeechPlaygroundModel(engine: engine, prefs: prefs, language: "en-US")
        model.load()
        return (model, engine)
    }

    func testBestVoicePrefersNaturalQualityInTheLanguage() {
        XCTAssertEqual(SpeechVoices.best([samantha, zoe, ava, thomas], language: "en-US"), ava)
        XCTAssertEqual(SpeechVoices.best([samantha, serena, zoe], language: "en-US"), serena)
        XCTAssertEqual(SpeechVoices.best([samantha, thomas], language: "en-US"), samantha)
        XCTAssertEqual(SpeechVoices.best([eddy, zarvox, samantha], language: "en-US"), samantha)
        XCTAssertEqual(SpeechVoices.best([ava, mine], language: "en-US"), mine)
        XCTAssertEqual(SpeechVoices.best([samantha, thomas], language: "fr_FR"), thomas)
        XCTAssertNil(SpeechVoices.best([], language: "en-US"))
    }

    func testLanguageMatchAndNaturalHint() {
        XCTAssertEqual(SpeechVoices.languageMatch("en-US", "en_us"), 2)
        XCTAssertEqual(SpeechVoices.languageMatch("en-GB", "en-US"), 1)
        XCTAssertEqual(SpeechVoices.languageMatch("fr-FR", "en-US"), 0)
        XCTAssertFalse(SpeechVoices.hasNatural([samantha, eddy, thomas], language: "en-US"))
        XCTAssertTrue(SpeechVoices.hasNatural([samantha, serena], language: "en-US"))
    }

    func testSectionsPutTheLanguageFirstAndDropNovelty() {
        let sections = SpeechVoices.sections([thomas, samantha, eddy, zarvox, serena, ava, zoe], language: "en-US") { $0 }
        XCTAssertEqual(sections.map(\.id), ["en-us", "en-gb", "fr-fr"])
        XCTAssertEqual(sections[0].voices.map(\.name), ["Ava", "Zoe", "Samantha", "Eddy"])
        XCTAssertEqual(sections[0].title, "en-US")
    }

    func testTuning() {
        XCTAssertEqual(SpeechTuning.utteranceRate(speed: 1), 0.5, accuracy: 0.0001)
        XCTAssertEqual(SpeechTuning.utteranceRate(speed: 0.5), 0.25, accuracy: 0.0001)
        XCTAssertEqual(SpeechTuning.utteranceRate(speed: 2), 0.75, accuracy: 0.0001)
        XCTAssertEqual(SpeechTuning.utteranceRate(speed: 9), 0.75, accuracy: 0.0001)
        XCTAssertEqual(SpeechTuning.label(speed: 1), "1×")
        XCTAssertEqual(SpeechTuning.label(speed: 1.25), "1.25×")
        XCTAssertEqual(SpeechTuning.label(speed: 0.5), "0.5×")
        XCTAssertEqual(SpeechTuning.label(pitch: 1.01), "Natural")
        XCTAssertEqual(SpeechTuning.label(pitch: 0.8), "Lower")
        XCTAssertEqual(SpeechTuning.estimatedSeconds("", speed: 1), 0)
        XCTAssertEqual(SpeechTuning.estimatedSeconds(String(repeating: "word ", count: 165), speed: 1), 60)
        XCTAssertEqual(SpeechTuning.estimatedSeconds(String(repeating: "word ", count: 165), speed: 2), 30)
    }

    func testHighlightUsesUTF16Ranges() {
        let text = "Hi 👋 there friend"
        XCTAssertEqual(
            SpeechText.highlight(text, location: 6, length: 5),
            SpeechHighlight(before: "Hi 👋 ", word: "there", after: " friend"))
        XCTAssertNil(SpeechText.highlight(text, location: 4, length: 1))
        XCTAssertNil(SpeechText.highlight(text, location: 40, length: 2))
        XCTAssertNil(SpeechText.highlight(text, location: 0, length: 0))
    }

    func testFileNameAndDuration() {
        XCTAssertEqual(SpeechText.fileName("Hello, World! How are you today?"), "hello-world-how-are-you.m4a")
        XCTAssertEqual(SpeechText.fileName("!!!"), "speech.m4a")
        XCTAssertEqual(SpeechText.duration(42), "42 s")
        XCTAssertEqual(SpeechText.duration(125), "2 min 5 s")
    }

    func testLoadPicksTheBestVoiceUnlessOneWasSaved() {
        let (picked, _) = model([samantha, ava, zoe])
        XCTAssertEqual(picked.voice, ava)
        let prefs = InMemoryKeyValueStore()
        prefs.set(zoe.id, forKey: SpeechPrefs.voice)
        let (saved, _) = model([samantha, ava, zoe], prefs: prefs)
        XCTAssertEqual(saved.voice, zoe)
        prefs.set("gone", forKey: SpeechPrefs.voice)
        let (missing, _) = model([samantha, ava], prefs: prefs)
        XCTAssertEqual(missing.voice, ava)
    }

    func testSpeakPauseResumeAndFinish() {
        let (model, engine) = model([ava])
        model.text = "  Hello there  "
        model.speed = 2
        model.pitch = 1.2
        model.toggle()
        XCTAssertEqual(model.state, .speaking)
        XCTAssertEqual(engine.spoken.last?.text, "Hello there")
        XCTAssertEqual(engine.spoken.last?.voiceId, ava.id)
        XCTAssertEqual(engine.spoken.last?.rate ?? 0, 0.75, accuracy: 0.0001)
        XCTAssertEqual(engine.spoken.last?.pitch ?? 0, 1.2, accuracy: 0.0001)
        engine.onRange?(6, 5)
        XCTAssertEqual(model.highlight, SpeechHighlight(before: "Hello ", word: "there", after: ""))
        model.toggle()
        XCTAssertEqual(model.state, .paused)
        model.toggle()
        XCTAssertEqual(model.state, .speaking)
        engine.onFinish?()
        XCTAssertEqual(model.state, .idle)
        XCTAssertNil(model.highlight)
        XCTAssertEqual(engine.calls, ["speak", "pause", "resume"])
        XCTAssertEqual(model.prefs.string(forKey: SpeechPrefs.text), "  Hello there  ")
    }

    func testNothingToSayDoesNothing() {
        let (model, engine) = model([ava])
        model.text = "   \n"
        XCTAssertFalse(model.canSpeak)
        model.speak()
        XCTAssertEqual(model.state, .idle)
        XCTAssertTrue(engine.spoken.isEmpty)
        let (voiceless, _) = self.model([])
        XCTAssertFalse(voiceless.canSpeak)
    }

    func testPreviewTogglesAndHidesTheHighlight() {
        let (model, engine) = model([ava, zoe])
        model.preview(zoe)
        XCTAssertEqual(model.previewing, zoe.id)
        XCTAssertEqual(engine.spoken.last?.text, "Hi, I'm Zoe. This is how I sound reading your words aloud.")
        engine.onRange?(0, 2)
        XCTAssertNil(model.highlight)
        model.preview(zoe)
        XCTAssertEqual(model.state, .idle)
        XCTAssertNil(model.previewing)
        XCTAssertEqual(SpeechPlaygroundModel.previewLine(thomas), "Thomas")
    }

    func testSampleAndClearStopSpeech() {
        let (model, engine) = model([ava])
        model.speak()
        model.useSample(SpeechPlaygroundModel.samples[1])
        XCTAssertEqual(model.state, .idle)
        XCTAssertEqual(model.text, SpeechPlaygroundModel.samples[1].text)
        model.clear()
        XCTAssertEqual(model.text, "")
        XCTAssertEqual(model.summary, "")
        XCTAssertEqual(engine.calls, ["speak", "stop"])
    }

    func testTuningPersistsAndClamps() {
        let prefs = InMemoryKeyValueStore()
        let (model, _) = model([ava], prefs: prefs)
        model.speed = 1.5
        model.pitch = 0.75
        model.tuningChanged()
        let (reloaded, _) = self.model([ava], prefs: prefs)
        XCTAssertEqual(reloaded.speed, 1.5)
        XCTAssertEqual(reloaded.pitch, 0.75)
        prefs.set(7.0, forKey: SpeechPrefs.speed)
        XCTAssertEqual(self.model([ava], prefs: prefs).0.speed, 2)
        reloaded.resetTuning()
        XCTAssertEqual(reloaded.speed, 1)
        XCTAssertEqual(prefs.double(forKey: SpeechPrefs.pitch), 1)
    }

    func testHints() {
        let (plain, _) = model([samantha, eddy])
        XCTAssertTrue(plain.showsVoiceHint)
        XCTAssertTrue(plain.showsPersonalVoiceHint)
        let (good, engine) = model([ava])
        XCTAssertFalse(good.showsVoiceHint)
        engine.personalVoiceStatus = .unsupported
        good.load()
        XCTAssertFalse(good.showsPersonalVoiceHint)
    }

    func testPersonalVoiceIsSelectedOnceAllowed() async {
        let (model, engine) = model([ava])
        engine.personal = mine
        await model.requestPersonalVoice()
        XCTAssertEqual(model.voice, mine)
        XCTAssertEqual(model.notice?.message, "Personal Voice is ready.")
        XCTAssertFalse(model.showsPersonalVoiceHint)
        let (denied, refusing) = self.model([ava])
        refusing.grant = .denied
        await denied.requestPersonalVoice()
        XCTAssertEqual(denied.voice, ava)
        XCTAssertEqual(denied.notice?.isError, true)
    }

    func testRenderAudio() async {
        let (model, engine) = model([ava])
        model.text = "Save this line"
        let url = await model.renderAudio()
        XCTAssertEqual(url?.lastPathComponent, "save-this-line.m4a")
        XCTAssertFalse(model.rendering)
        engine.renderFails = true
        let failed = await model.renderAudio()
        XCTAssertNil(failed)
        XCTAssertEqual(model.notice?.message, "Could not save the audio: disk full")
    }
}
