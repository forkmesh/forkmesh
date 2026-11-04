import FMTools
import Foundation
import XCTest

enum SilentSynth {
    static func pattern(_ word: String, _ p: Double, _ amp: Double) -> SilentVector {
        let one: Double = amp * sin(2 * Double.pi * p)
        let two: Double = amp * sin(4 * Double.pi * p)
        switch word {
        case "yes": return SilentVector(one, 0, 0)
        case "no": return SilentVector(0, one, 0)
        case "help": return SilentVector(two, 0, 0)
        default: return SilentVector(0, 0, one)
        }
    }

    static func take(_ word: String, _ random: inout SilentRandom, rate: Double = 25, acceleration: Bool = true, noise: Double = 0.05) -> [SilentSample] {
        let duration: Double = 0.8 + 0.3 * random.unit()
        let amp: Double = 1.5 + 0.5 * random.unit()
        let lead: Double = 0.3
        let count: Int = Int(((2 * lead + duration) * rate).rounded())
        var attitude: SilentQuaternion = .identity
        var samples: [SilentSample] = []
        for k in 0..<count {
            let t: Double = Double(k) / rate
            let p: Double = (t - lead) / duration
            let base: SilentVector = p >= 0 && p <= 1 ? pattern(word, p, amp) : .zero
            let rate3: SilentVector = SilentVector(
                base.x + noise * (random.unit() - 0.5), base.y + noise * (random.unit() - 0.5), base.z + noise * (random.unit() - 0.5))
            attitude = (attitude * SilentQuaternion.fromRotationVector(SilentVector(rate3.x / rate, rate3.y / rate, rate3.z / rate))).normalized
            let accel: SilentVector? = acceleration ? SilentVector(0.02 * rate3.x, 0.02 * rate3.y, 0.02 * rate3.z) : nil
            samples.append(SilentSample(t: t, rotationRate: rate3, attitude: attitude, acceleration: accel))
        }
        return samples
    }

    static func dataset(words: [String], takes: Int, seed: UInt64, acceleration: Bool = true) -> SilentDataset {
        var random: SilentRandom = SilentRandom(seed: seed)
        var dataset: SilentDataset = SilentDataset(words: words.map { SilentWord($0) })
        for n in 0..<takes {
            for word in words {
                dataset.addTake(SilentTake(id: "\(word)-\(n)", word: word, samples: take(word, &random, acceleration: acceleration)))
            }
        }
        return dataset
    }
}

final class SilentSpeechTests: XCTestCase {
    func testRandomMatchesSplitMix64() {
        var random: SilentRandom = SilentRandom(seed: 0x5EED_5EED)
        XCTAssertEqual(random.next(), 0x99a4_143d_3458_5f45)
        XCTAssertEqual(random.next(), 0xfc18_d87f_cc9c_a7a3)
        XCTAssertEqual(random.next(), 0x7220_ff96_60d1_3a72)
    }

    func testQuaternionRoundTripsARotationVector() {
        let v: SilentVector = SilentVector(0.3, -0.2, 0.9)
        let back: SilentVector = SilentQuaternion.fromRotationVector(v).rotationVector
        XCTAssertEqual(back.x, v.x, accuracy: 1e-9)
        XCTAssertEqual(back.y, v.y, accuracy: 1e-9)
        XCTAssertEqual(back.z, v.z, accuracy: 1e-9)
        let r: SilentVector = (SilentQuaternion.fromRotationVector(v).conjugate * SilentQuaternion.fromRotationVector(v)).rotationVector
        XCTAssertEqual(r.length, 0, accuracy: 1e-9)
    }

    func testResamplesToTwentyFiveHertz() {
        var random: SilentRandom = SilentRandom(seed: 1)
        let fast: [SilentSample] = SilentSynth.take("yes", &random, rate: 100)
        let resampled: [SilentSample] = SilentFeatures.resampled(fast)
        let span: Double = fast.last!.t - fast.first!.t
        XCTAssertEqual(resampled.count, Int((span * 25).rounded(.down)) + 1)
        XCTAssertEqual(resampled[1].t - resampled[0].t, 0.04, accuracy: 1e-9)
    }

    func testTrimsTheQuietLeadInAndLeadOut() {
        var random: SilentRandom = SilentRandom(seed: 2)
        let samples: [SilentSample] = SilentSynth.take("no", &random, noise: 0.01)
        let trimmed: [SilentSample] = SilentFeatures.trimmed(samples)
        XCTAssertLessThan(trimmed.count, samples.count - 10)
        XCTAssertGreaterThan(trimmed.first!.t, 0.25)
    }

    func testFramesHaveThreeValuesPerChannelAndAreCentred() {
        var random: SilentRandom = SilentRandom(seed: 3)
        let samples: [SilentSample] = SilentSynth.take("help", &random)
        XCTAssertEqual(SilentFeatures.frames(samples, channels: SilentChannel.defaults).first?.count, 9)
        let frames: [[Double]] = SilentFeatures.frames(samples, channels: [.rotation])
        XCTAssertEqual(frames.first?.count, 3)
        let mean: Double = frames.reduce(0) { $0 + $1[0] } / Double(frames.count)
        XCTAssertEqual(mean, 0, accuracy: 1e-9)
    }

    func testDTWIsZeroForItselfSymmetricAndForgivesTempo() {
        var random: SilentRandom = SilentRandom(seed: 4)
        let a: [[Double]] = SilentFeatures.frames(SilentSynth.take("yes", &random), channels: [.rotation])
        let slow: [[Double]] = SilentFeatures.frames(SilentSynth.take("yes", &random), channels: [.rotation])
        let other: [[Double]] = SilentFeatures.frames(SilentSynth.take("no", &random), channels: [.rotation])
        XCTAssertEqual(SilentDTW.distance(a, a), 0, accuracy: 1e-12)
        XCTAssertEqual(SilentDTW.distance(a, other), SilentDTW.distance(other, a), accuracy: 1e-12)
        XCTAssertLessThan(SilentDTW.distance(a, slow), SilentDTW.distance(a, other))
        XCTAssertEqual(SilentDTW.distance(a, other, cutoff: 1e-6), .infinity)
        XCTAssertEqual(SilentDTW.distance([], a), .infinity)
    }

    func testModelRecognisesEachWordAndRejectsFarOnes() {
        let train: SilentDataset = SilentSynth.dataset(words: ["yes", "no", "help", "stop"], takes: 3, seed: 10)
        let model: SilentModel = SilentModel(takes: train.takes, channels: SilentChannel.defaults)
        var random: SilentRandom = SilentRandom(seed: 99)
        for word in ["yes", "no", "help", "stop"] {
            let match: SilentMatch? = model.match(SilentSynth.take(word, &random), maxDistance: nil, minMargin: 0)
            XCTAssertEqual(match?.word, word)
            XCTAssertEqual(match?.accepted, true)
        }
        let strict: SilentMatch? = model.match(SilentSynth.take("yes", &random), maxDistance: 1e-6, minMargin: 0)
        XCTAssertEqual(strict?.accepted, false)
        XCTAssertTrue(strict?.summary.hasPrefix("Not sure") ?? false)
    }

    func testWorksWithoutAccelerationLikeAndroidHeadTrackers() {
        let train: SilentDataset = SilentSynth.dataset(words: ["yes", "no"], takes: 3, seed: 11, acceleration: false)
        let model: SilentModel = SilentModel(takes: train.takes, channels: SilentChannel.defaults)
        var random: SilentRandom = SilentRandom(seed: 12)
        XCTAssertEqual(model.match(SilentSynth.take("no", &random, acceleration: false), maxDistance: nil, minMargin: 0)?.word, "no")
    }

    func testEvaluationFindsSeparableWordsSignificant() throws {
        let dataset: SilentDataset = SilentSynth.dataset(words: ["yes", "no", "help", "stop"], takes: 4, seed: 42)
        let result: SilentEvaluation = try XCTUnwrap(SilentEvaluator.evaluate(dataset, channels: SilentChannel.defaults, permutations: 200))
        XCTAssertEqual(result.evaluated, 16)
        XCTAssertEqual(result.correct, 16)
        XCTAssertEqual(result.pValue, 1.0 / 201.0, accuracy: 1e-12)
        XCTAssertTrue(result.significant)
        XCTAssertEqual(result.confusion.map { $0.reduce(0, +) }, [4, 4, 4, 4])
        XCTAssertNotNil(result.suggestedMaxDistance)
        XCTAssertLessThan(result.nullMean, 0.5)
        XCTAssertEqual(result.headline, "Right 16 of 16 (100%), chance is about 25%")
        XCTAssertTrue(result.verdict.hasPrefix("p = 0.005"))
    }

    func testEvaluationOfIdenticalMotionIsNotSignificant() throws {
        var random: SilentRandom = SilentRandom(seed: 7)
        var dataset: SilentDataset = SilentDataset(words: [SilentWord("a"), SilentWord("b"), SilentWord("c")])
        for n in 0..<12 {
            dataset.addTake(SilentTake(id: "\(n)", word: ["a", "b", "c"][n % 3], samples: SilentSynth.take("yes", &random, noise: 0.4)))
        }
        let result: SilentEvaluation = try XCTUnwrap(SilentEvaluator.evaluate(dataset, channels: SilentChannel.defaults, permutations: 200))
        XCTAssertFalse(result.significant)
        XCTAssertTrue(result.verdict.contains("not yet better than chance"))
    }

    func testEvaluationNeedsTwoWordsWithTwoTakes() {
        var dataset: SilentDataset = SilentSynth.dataset(words: ["yes", "no"], takes: 1, seed: 5)
        XCTAssertNotNil(SilentEvaluator.requirement(dataset))
        XCTAssertNil(SilentEvaluator.evaluate(dataset, channels: SilentChannel.defaults))
        var random: SilentRandom = SilentRandom(seed: 6)
        dataset.addTake(SilentTake(id: "y", word: "yes", samples: SilentSynth.take("yes", &random)))
        dataset.addTake(SilentTake(id: "n", word: "no", samples: SilentSynth.take("no", &random)))
        XCTAssertNil(SilentEvaluator.requirement(dataset))
    }

    func testLeaveOneOutUsesTheMatrixAndSkipsSingleTakeWords() {
        let d: [[Double]] = [[0, 1, 5, 5], [1, 0, 5, 5], [5, 5, 0, 2], [5, 5, 2, 0]]
        let results = SilentEvaluator.leaveOneOut(d, labels: [0, 0, 1, 1], wordCount: 2, neighbors: 1)
        XCTAssertEqual(results.map(\.guess), [0, 0, 1, 1])
        XCTAssertEqual(results.map(\.distance), [1, 1, 2, 2])
        XCTAssertEqual(SilentEvaluator.leaveOneOut(d, labels: [0, 1, 1, 1], wordCount: 2, neighbors: 1).count, 3)
    }

    func testLeaveOneOutOnThreeNeighboursMatchesSortingEveryDistance() {
        var random: SilentRandom = SilentRandom(seed: 21)
        let n: Int = 30
        var d: [[Double]] = Array(repeating: Array(repeating: 0, count: n), count: n)
        for i in 0..<n {
            for j in (i + 1)..<n {
                d[i][j] = (random.unit() * 8).rounded() / 4
                d[j][i] = d[i][j]
            }
        }
        let labels: [Int] = (0..<n).map { $0 % 3 }
        let results = SilentEvaluator.leaveOneOut(d, labels: labels, wordCount: 3, neighbors: 3)
        for (i, r) in results.enumerated() {
            let scores: [Double] = (0..<3).map { (w: Int) -> Double in
                let best: [Double] = Array(labels.indices.filter { $0 != i && labels[$0] == w }.map { d[i][$0] }.sorted().prefix(3))
                return best.reduce(0, +) / Double(best.count)
            }
            XCTAssertEqual(r.distance, scores.min()!)
            XCTAssertEqual(r.guess, scores.firstIndex(of: scores.min()!)!)
        }
        XCTAssertEqual(results.count, n)
    }

    func testCrossPlatformParity() throws {
        let dataset: SilentDataset = SilentSynth.dataset(words: ["yes", "no", "help"], takes: 3, seed: 2026, acceleration: false)
        let result: SilentEvaluation = try XCTUnwrap(SilentEvaluator.evaluate(dataset, channels: [.rotation, .attitude], permutations: 100))
        XCTAssertEqual(result.evaluated, 9)
        XCTAssertEqual(result.correct, 9)
        XCTAssertEqual(result.pValue, 1.0 / 101.0, accuracy: 1e-12)
        let frames: [[Double]] = SilentFeatures.frames(dataset.takes[0].samples, channels: [.rotation, .attitude])
        XCTAssertEqual(frames.count, 26)
        XCTAssertEqual(SilentDTW.distance(frames, SilentFeatures.frames(dataset.takes[1].samples, channels: [.rotation, .attitude])), 0.866728, accuracy: 1e-5)
    }

    func testSegmenterFindsAMouthedWordWithPreRoll() throws {
        var random: SilentRandom = SilentRandom(seed: 8)
        let quiet: [Double] = (0..<50).map { _ in 0.02 + 0.01 * random.unit() }
        let noise: SilentNoiseFloor = try XCTUnwrap(SilentNoiseFloor.calibrate(quiet))
        XCTAssertGreaterThan(noise.on, noise.off)
        var segmenter: SilentSegmenter = SilentSegmenter(noise: noise)
        var found: [[SilentSample]] = []
        let word: [SilentSample] = SilentSynth.take("yes", &random, noise: 0.01)
        for sample in word {
            if let segment = segmenter.push(sample) { found.append(segment) }
        }
        let tail: Double = word.last!.t
        for k in 1...20 {
            let t: Double = tail + Double(k) / 25
            if let segment = segmenter.push(SilentSample(t: t, rotationRate: .zero, attitude: .identity)) { found.append(segment) }
        }
        XCTAssertEqual(found.count, 1)
        let segment: [SilentSample] = try XCTUnwrap(found.first)
        XCTAssertLessThan(segment.first!.t, 0.3)
        XCTAssertGreaterThan(segment.last!.t - segment.first!.t, 0.6)
        XCTAssertFalse(segmenter.active)
    }

    func testSegmenterIgnoresAShortBump() throws {
        let noise: SilentNoiseFloor = SilentNoiseFloor(floor: 0.02, spread: 0.005)
        var segmenter: SilentSegmenter = SilentSegmenter(noise: noise)
        var found: Int = 0
        for k in 0..<40 {
            let bump: Double = (10...12).contains(k) ? 1 : 0
            if segmenter.push(SilentSample(t: Double(k) / 25, rotationRate: SilentVector(bump, 0, 0), attitude: .identity)) != nil { found += 1 }
        }
        XCTAssertEqual(found, 0)
    }

    func testDatasetCleansWordsAndRoundTripsJSON() throws {
        var dataset: SilentDataset = .starter
        XCTAssertFalse(dataset.addWord(" YES "))
        XCTAssertTrue(dataset.addWord(" Thirsty ", phrase: "Could I have some water, please? "))
        XCTAssertEqual(dataset.phrase("thirsty"), "Could I have some water, please?")
        XCTAssertEqual(dataset.phrase("yes"), "yes")
        var random: SilentRandom = SilentRandom(seed: 9)
        dataset.addTake(SilentTake(id: "t1", word: "thirsty", samples: SilentSynth.take("yes", &random)))
        dataset.addTake(SilentTake(id: "t2", word: "yes", samples: SilentSynth.take("yes", &random, acceleration: false)))
        dataset.addTake(SilentTake(id: "t3", word: "unknown", samples: SilentSynth.take("yes", &random)))
        XCTAssertEqual(dataset.takes.count, 2)
        let back: SilentDataset = try XCTUnwrap(SilentDataset.decode(dataset.encoded()))
        XCTAssertEqual(back.words, dataset.words)
        XCTAssertEqual(back.takes.map(\.id), ["t1", "t2"])
        XCTAssertNotNil(back.takes[0].samples[0].acceleration)
        XCTAssertNil(back.takes[1].samples[0].acceleration)
        XCTAssertEqual(back.takes[0].samples.count, dataset.takes[0].samples.count)
        XCTAssertEqual(back.takes[0].samples[5].rotationRate.x, dataset.takes[0].samples[5].rotationRate.x, accuracy: 1e-5)
        dataset.removeWord("thirsty")
        XCTAssertEqual(dataset.takes.map(\.id), ["t2"])
        dataset.removeLastTake("yes")
        XCTAssertEqual(dataset.count("yes"), 0)
        XCTAssertNil(SilentDataset.decode(Data("{\"version\":99,\"words\":[]}".utf8)))
    }

    func testReadingScriptIsBalancedWithoutRepeatsAndMatchesAndroid() {
        let words: [String] = ["yes", "no", "help", "stop", "water"]
        let script: [[String]] = SilentReading.script(words, seed: 7, count: 20)
        XCTAssertEqual(script.count, 20)
        XCTAssertTrue(script.allSatisfy { $0.count == 5 })
        let flat: [String] = script.flatMap { $0 }
        XCTAssertEqual(Set(words.map { w in flat.filter { $0 == w }.count }), [20])
        XCTAssertFalse(zip(flat, flat.dropFirst()).contains { $0 == $1 })
        XCTAssertEqual(script[0].joined(separator: " "), "water no stop yes help")
        XCTAssertEqual(script[1].joined(separator: " "), "yes help stop no water")
        XCTAssertEqual(SilentReading.script(["yes", "no"], seed: 1, count: 3).map { $0.count }, [4, 4, 4])
        XCTAssertEqual(SilentReading.length(9), 6)
        XCTAssertEqual(SilentReading.script([], seed: 1), [])
    }

    func testReadingLabelsEachHeardWordInOrder() {
        var reading: SilentReading = SilentReading(words: ["yes", "no", "help", "stop"], seed: 3, duration: 10)
        let first: [String] = reading.sentence
        var random: SilentRandom = SilentRandom(seed: 4)
        var labelled: [(word: String, samples: [SilentSample])] = []
        for word in first {
            XCTAssertTrue(labelled.isEmpty)
            labelled = reading.hear(SilentSynth.take(word, &random))
        }
        XCTAssertEqual(labelled.map(\.word), first)
        XCTAssertEqual(reading.sentencesRead, 1)
        XCTAssertEqual(reading.collected, 4)
        XCTAssertEqual(reading.index, 1)
        XCTAssertEqual(reading.position, 0)
        XCTAssertEqual(reading.summary, "Saved 4 takes from 1 sentence.")
        _ = reading.hear(SilentSynth.take("yes", &random))
        XCTAssertEqual(reading.position, 1)
        reading.again()
        XCTAssertEqual(reading.position, 0)
        XCTAssertEqual(reading.index, 1)
        reading.skip()
        XCTAssertEqual(reading.index, 2)
        reading.forget(4)
        XCTAssertEqual(reading.collected, 0)
        XCTAssertEqual(reading.sentencesRead, 0)
    }

    func testReadingRunsForItsDuration() {
        var reading: SilentReading = SilentReading(words: ["yes", "no"], seed: 1, duration: 300)
        reading.tick(1000)
        reading.tick(1072.6)
        XCTAssertEqual(reading.clock, "1:12 of 5:00")
        XCTAssertFalse(reading.timeUp)
        reading.tick(1300)
        XCTAssertTrue(reading.done)
        _ = reading.hear([SilentSample(t: 0, rotationRate: .zero, attitude: .identity)])
        XCTAssertFalse(reading.done)
    }

    func testManyTakesMatchOnThreeNeighbours() {
        XCTAssertEqual(SilentEvaluator.neighbors(for: SilentSynth.dataset(words: ["yes", "no"], takes: 9, seed: 1)), 1)
        var dataset: SilentDataset = SilentSynth.dataset(words: ["yes", "no"], takes: 10, seed: 1)
        XCTAssertEqual(SilentEvaluator.neighbors(for: dataset), 3)
        dataset.addWord("help")
        XCTAssertEqual(SilentEvaluator.neighbors(for: dataset), 3)
        dataset.removeTakes(["yes-0", "no-1"])
        XCTAssertEqual(dataset.count("yes"), 9)
        XCTAssertEqual(SilentEvaluator.neighbors(for: dataset), 1)
    }

    func testTranscriptUndoAndClear() {
        var transcript: SilentTranscript = SilentTranscript()
        transcript.append("help")
        transcript.append("water")
        XCTAssertEqual(transcript.text, "help water")
        transcript.undo()
        XCTAssertEqual(transcript.text, "help")
        transcript.clear()
        XCTAssertEqual(transcript.text, "")
    }
}
