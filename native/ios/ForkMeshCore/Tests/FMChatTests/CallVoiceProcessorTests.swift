import Foundation
import XCTest
@testable import FMChat

enum CallVoiceParity {
    static let sampleTolerance = 1e-6
    static let rmsTolerance = 1e-5

    static let fixture: [String: Any] = {
        var url = URL(fileURLWithPath: #filePath)
        url.deleteLastPathComponent()
        url.appendPathComponent("Fixtures/call-voice-parity.json")
        guard let data = try? Data(contentsOf: url),
              let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return [:] }
        return object
    }()

    static var cases: [[String: Any]] { fixture["cases"] as? [[String: Any]] ?? [] }

    static func numbers(_ value: Any?) -> [Double] {
        (value as? [Any] ?? []).map { ($0 as? NSNumber)?.doubleValue ?? .nan }
    }

    static func int(_ value: Any?) -> Int { (value as? NSNumber)?.intValue ?? 0 }

    private static func envelope(_ i: Int, _ a: Int, _ b: Int) -> Double {
        i < a || i >= b ? 0 : min(1, min(Double(i - a) / 480, Double(b - i) / 480))
    }

    static func input() -> [Float] {
        var x = [Float](repeating: 0, count: 48000)
        var seed: UInt32 = 0x1234567
        var phase = 0.0
        var lp = 0.0
        for i in 0..<48000 {
            seed ^= seed << 13
            seed ^= seed >> 17
            seed ^= seed << 5
            let noise = Double(seed) / 4294967296 - 0.5
            let glide = i < 1920 ? 0 : i >= 24960 ? 1 : Double(i - 1920) / 23040
            let f0 = i < 28000 ? 220 - 40 * glide : 125
            phase += f0 / 48000
            if phase >= 1 { phase -= 1 }
            let saw = 2 * phase - 1
            lp += 0.3 * (saw - lp)
            let voiced = 0.4 * lp * (envelope(i, 1920, 24960) + envelope(i, 31680, 46080))
            x[i] = Float(voiced + noise * (0.3 * envelope(i, 24960, 30720) + 0.006))
        }
        return x
    }

    static func run(_ spec: [String: Any], chunk: Int = 48000) -> ([Float], CallVoiceProcessor) {
        var samples = input()
        let processor = CallVoiceProcessor(sampleRate: 48000, userPitchHz: (spec["userPitchHz"] as? NSNumber)?.doubleValue ?? 0)
        processor.setVoice(spec["voice"] as? String ?? "")
        let switchAt = int(spec["switchAt"]) > 0 ? int(spec["switchAt"]) : samples.count
        let total = samples.count
        samples.withUnsafeMutableBufferPointer { buffer in
            var at = 0
            while at < total {
                if at == switchAt { processor.setVoice(spec["switchTo"] as? String ?? "") }
                let end = min(at + chunk, total, at < switchAt ? switchAt : total)
                processor.process(buffer.baseAddress! + at, count: end - at)
                at = end
            }
        }
        return (samples, processor)
    }

    static func rms(_ samples: [Float], block: Int) -> [Double] {
        (0..<(samples.count / block)).map { b in
            var sum = 0.0
            for i in (b * block)..<((b + 1) * block) { sum += Double(samples[i]) * Double(samples[i]) }
            return sqrt(sum / Double(block))
        }
    }

    static func hash(_ samples: [Float]) -> UInt32 {
        var h: UInt32 = 0x811c9dc5
        for v in samples { h = (h ^ v.bitPattern) &* 0x01000193 }
        return h
    }

    static func maxStep(_ samples: [Float], from: Int, to: Int) -> Double {
        var top = 0.0
        for i in max(1, from)..<min(samples.count, to) { top = max(top, abs(Double(samples[i]) - Double(samples[i - 1]))) }
        return top
    }
}

final class CallVoiceProcessorTests: XCTestCase {
    private typealias P = CallVoiceParity

    func testTheCatalogMatchesTheJsVoices() {
        let voices = P.fixture["voices"] as? [[String: Any]] ?? []
        XCTAssertEqual(voices.map { $0["id"] as? String ?? "" }, CallVoiceCatalog.voices.map(\.id))
        for (row, voice) in zip(voices, CallVoiceCatalog.voices) {
            XCTAssertEqual(row["label"] as? String, voice.label)
            let fields: [(String, Double)] = [("pitchHz", voice.pitchHz), ("tract", voice.tract), ("brightness", voice.brightness),
                                              ("breath", voice.breath), ("range", voice.range), ("jitter", voice.jitter),
                                              ("tremor", voice.tremor)]
            for (key, value) in fields {
                XCTAssertEqual((row[key] as? NSNumber)?.doubleValue, value, "\(voice.id).\(key)")
            }
        }
    }

    func testChoicesResolveLikeTheWeb() {
        XCTAssertEqual(CallVoiceCatalog.normalizeChoice(" OFF "), "off")
        XCTAssertEqual(CallVoiceCatalog.normalizeChoice("Kofi"), "kofi")
        XCTAssertEqual(CallVoiceCatalog.normalizeChoice("nobody"), "face")
        XCTAssertEqual(CallVoiceCatalog.normalizeChoice(nil), "face")
        XCTAssertEqual(CallVoiceCatalog.resolve("off", faceId: "maya"), "")
        XCTAssertEqual(CallVoiceCatalog.resolve("face", faceId: "maya"), "maya")
        XCTAssertEqual(CallVoiceCatalog.resolve("face", faceId: "custom"), "")
        XCTAssertEqual(CallVoiceCatalog.resolve("teen", faceId: "maya"), "teen")
        XCTAssertEqual(CallVoiceCatalog.label(""), "Your own")
        XCTAssertEqual(CallVoiceCatalog.label("deep"), "Deep man")
        XCTAssertEqual(CallVoiceCatalog.choices().count, 16)
        XCTAssertEqual(CallVoiceCatalog.choices().first?.id, "face")
        XCTAssertEqual(CallVoiceCatalog.choices().first?.label, "Match face")
    }

    func testTheInputMatchesTheJsGenerator() {
        let x = P.input()
        let expected = P.numbers(P.fixture["inputSamples"])
        let stride = P.int(P.fixture["stride"])
        XCTAssertFalse(expected.isEmpty)
        for (k, value) in expected.enumerated() {
            XCTAssertLessThanOrEqual(abs(Double(x[k * stride]) - value), 1e-8, "input sample \(k)")
        }
        XCTAssertEqual(CallVoiceProcessor().latencySamples, P.int(P.fixture["latency"]))
    }

    func testEveryVoiceMatchesTheJsEngine() {
        let stride = P.int(P.fixture["stride"])
        let block = P.int(P.fixture["block"])
        var worstSample = 0.0
        var worstRms = 0.0
        var differing: [String] = []
        XCTAssertEqual(P.cases.count, 19)
        for spec in P.cases {
            let name = spec["name"] as? String ?? ""
            let (out, processor) = P.run(spec)
            for (k, value) in P.numbers(spec["samples"]).enumerated() {
                let error = abs(Double(out[k * stride]) - value)
                worstSample = max(worstSample, error)
                XCTAssertLessThanOrEqual(error, P.sampleTolerance, "\(name) sample \(k * stride)")
            }
            let ours = P.rms(out, block: block)
            for (k, value) in P.numbers(spec["rms"]).enumerated() {
                let error = abs(ours[k] - value) / max(value, 1e-3)
                worstRms = max(worstRms, error)
                XCTAssertLessThanOrEqual(error, P.rmsTolerance, "\(name) rms block \(k)")
            }
            if UInt32(P.int(spec["hash"])) != P.hash(out) { differing.append(name) }
            let pitch = (spec["finalUserPitchHz"] as? NSNumber)?.doubleValue ?? 0
            XCTAssertLessThanOrEqual(abs(processor.userPitchHz - pitch), 1e-6, "\(name) caller pitch")
        }
        print("call voice parity: worst sample error \(worstSample), worst relative rms error \(worstRms), "
              + "not bit-identical \(differing.count) of \(P.cases.count) \(differing)")
    }

    func testOffIsTheInputDelayedByTheLatency() {
        let input = P.input()
        let (out, processor) = P.run(["voice": "off", "userPitchHz": 0, "switchAt": 0, "switchTo": ""])
        let latency = processor.latencySamples
        XCTAssertTrue(out[0..<latency].allSatisfy { $0 == 0 })
        XCTAssertTrue(Array(out[latency...]) == Array(input[0..<(input.count - latency)]))
    }

    func testSwitchingVoicesMidStreamDoesNotClick() {
        for (from, to) in [("maya", "kofi"), ("off", "teen"), ("robot", "off"), ("deep", "ingrid")] {
            let at = 24000
            let spec: [String: Any] = ["voice": from, "userPitchHz": 0, "switchAt": at, "switchTo": to]
            let (out, processor) = P.run(spec)
            var only = spec
            only["switchAt"] = 0
            let first = P.maxStep(P.run(only).0, from: 0, to: 48000)
            only["voice"] = to
            let second = P.maxStep(P.run(only).0, from: 0, to: 48000)
            let around = P.maxStep(out, from: at, to: at + processor.latencySamples + 4800)
            XCTAssertLessThanOrEqual(around, 1.2 * max(first, second), "\(from) to \(to)")
        }
    }

    func testBufferSizesMatchOneLongBuffer() {
        for name in ["maya-to-kofi", "older-woman"] {
            guard let spec = P.cases.first(where: { $0["name"] as? String == name }) else { return XCTFail(name) }
            let whole = P.run(spec).0
            for chunk in [128, 480, 960, 1024] {
                XCTAssertTrue(P.run(spec, chunk: chunk).0 == whole, "\(name) in \(chunk)-sample buffers")
            }
        }
    }

    func testANonFiniteSampleDoesNotPoisonTheStream() {
        var samples = P.input()
        samples[20000] = .nan
        samples[20001] = .infinity
        let processor = CallVoiceProcessor()
        processor.setVoice("maya")
        processor.process(&samples)
        XCTAssertTrue(samples.allSatisfy(\.isFinite))
        XCTAssertGreaterThan(P.rms(Array(samples[30000..<40000]), block: 10000)[0], 0.05)
    }
}
