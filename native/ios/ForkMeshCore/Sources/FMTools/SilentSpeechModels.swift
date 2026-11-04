import Foundation

public struct SilentVector: Equatable, Sendable {
    public var x: Double
    public var y: Double
    public var z: Double

    public init(_ x: Double, _ y: Double, _ z: Double) {
        self.x = x
        self.y = y
        self.z = z
    }

    public static let zero: SilentVector = SilentVector(0, 0, 0)

    public var length: Double { (x * x + y * y + z * z).squareRoot() }

    func lerp(_ other: SilentVector, _ f: Double) -> SilentVector {
        SilentVector(x + (other.x - x) * f, y + (other.y - y) * f, z + (other.z - z) * f)
    }
}

public struct SilentQuaternion: Equatable, Sendable {
    public var w: Double
    public var x: Double
    public var y: Double
    public var z: Double

    public init(w: Double, x: Double, y: Double, z: Double) {
        self.w = w
        self.x = x
        self.y = y
        self.z = z
    }

    public static let identity: SilentQuaternion = SilentQuaternion(w: 1, x: 0, y: 0, z: 0)

    public static func fromRotationVector(_ v: SilentVector) -> SilentQuaternion {
        let angle: Double = v.length
        guard angle > 1e-12 else { return .identity }
        let s: Double = sin(angle / 2) / angle
        return SilentQuaternion(w: cos(angle / 2), x: v.x * s, y: v.y * s, z: v.z * s)
    }

    public var conjugate: SilentQuaternion { SilentQuaternion(w: w, x: -x, y: -y, z: -z) }

    public var normalized: SilentQuaternion {
        let n: Double = (w * w + x * x + y * y + z * z).squareRoot()
        guard n > 1e-12 else { return .identity }
        return SilentQuaternion(w: w / n, x: x / n, y: y / n, z: z / n)
    }

    public static func * (a: SilentQuaternion, b: SilentQuaternion) -> SilentQuaternion {
        SilentQuaternion(
            w: a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            x: a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            y: a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            z: a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w)
    }

    public var rotationVector: SilentVector {
        let q: SilentQuaternion = w < 0 ? SilentQuaternion(w: -w, x: -x, y: -y, z: -z) : self
        let s: Double = (q.x * q.x + q.y * q.y + q.z * q.z).squareRoot()
        guard s > 1e-12 else { return SilentVector(2 * q.x, 2 * q.y, 2 * q.z) }
        let angle: Double = 2 * atan2(s, q.w)
        return SilentVector(q.x / s * angle, q.y / s * angle, q.z / s * angle)
    }

    func nlerp(_ other: SilentQuaternion, _ f: Double) -> SilentQuaternion {
        let dot: Double = w * other.w + x * other.x + y * other.y + z * other.z
        let o: SilentQuaternion = dot < 0 ? SilentQuaternion(w: -other.w, x: -other.x, y: -other.y, z: -other.z) : other
        return SilentQuaternion(w: w + (o.w - w) * f, x: x + (o.x - x) * f, y: y + (o.y - y) * f, z: z + (o.z - z) * f).normalized
    }
}

public struct SilentSample: Equatable, Sendable {
    public var t: Double
    public var rotationRate: SilentVector
    public var attitude: SilentQuaternion
    public var acceleration: SilentVector?

    public init(t: Double, rotationRate: SilentVector, attitude: SilentQuaternion, acceleration: SilentVector? = nil) {
        self.t = t
        self.rotationRate = rotationRate
        self.attitude = attitude
        self.acceleration = acceleration
    }

    public var energy: Double { rotationRate.length + (acceleration?.length ?? 0) }

    func lerp(_ other: SilentSample, at time: Double) -> SilentSample {
        let span: Double = other.t - t
        let f: Double = span > 0 ? min(max((time - t) / span, 0), 1) : 0
        let acceleration: SilentVector? = {
            guard let a = self.acceleration, let b = other.acceleration else { return self.acceleration ?? other.acceleration }
            return a.lerp(b, f)
        }()
        return SilentSample(t: time, rotationRate: rotationRate.lerp(other.rotationRate, f), attitude: attitude.nlerp(other.attitude, f), acceleration: acceleration)
    }
}

public enum SilentChannel: String, CaseIterable, Sendable {
    case acceleration
    case rotation
    case attitude

    public var label: String {
        switch self {
        case .acceleration: return "Movement"
        case .rotation: return "Turning speed"
        case .attitude: return "Head angle"
        }
    }

    public static let defaults: Set<SilentChannel> = Set(SilentChannel.allCases)
}

public enum SilentFeatures {
    public static let rate: Double = 25
    public static let trimFraction: Double = 0.12
    public static let minFrames: Int = 4

    public static func resampled(_ samples: [SilentSample], rate: Double = SilentFeatures.rate) -> [SilentSample] {
        guard let first = samples.first, let last = samples.last, samples.count > 1, last.t > first.t else { return samples }
        let step: Double = 1 / rate
        let count: Int = Int(((last.t - first.t) / step).rounded(.down)) + 1
        var out: [SilentSample] = []
        out.reserveCapacity(count)
        var index: Int = 0
        for k in 0..<count {
            let time: Double = first.t + Double(k) * step
            while index < samples.count - 2 && samples[index + 1].t < time { index += 1 }
            out.append(samples[index].lerp(samples[index + 1], at: time))
        }
        return out
    }

    public static func trimmed(_ samples: [SilentSample], fraction: Double = SilentFeatures.trimFraction, keep: Int = SilentFeatures.minFrames) -> [SilentSample] {
        guard samples.count > keep else { return samples }
        let peak: Double = samples.map(\.energy).max() ?? 0
        guard peak > 0 else { return samples }
        let limit: Double = peak * fraction
        var start: Int = samples.firstIndex { $0.energy >= limit } ?? 0
        var end: Int = samples.lastIndex { $0.energy >= limit } ?? samples.count - 1
        while end - start + 1 < keep {
            if start > 0 { start -= 1 }
            if end - start + 1 < keep && end < samples.count - 1 { end += 1 }
        }
        return Array(samples[start...end])
    }

    public static func dimension(_ channels: Set<SilentChannel>) -> Int { channels.count * 3 }

    public static func frames(_ samples: [SilentSample], channels: Set<SilentChannel>) -> [[Double]] {
        let kept: [SilentSample] = trimmed(resampled(samples))
        guard let origin = kept.first?.attitude.conjugate else { return [] }
        var frames: [[Double]] = kept.map { (sample: SilentSample) -> [Double] in
            var row: [Double] = []
            row.reserveCapacity(dimension(channels))
            for channel in SilentChannel.allCases where channels.contains(channel) {
                let v: SilentVector
                switch channel {
                case .acceleration: v = sample.acceleration ?? .zero
                case .rotation: v = sample.rotationRate
                case .attitude: v = (origin * sample.attitude).rotationVector
                }
                row.append(contentsOf: [v.x, v.y, v.z])
            }
            return row
        }
        let width: Int = dimension(channels)
        guard width > 0 else { return frames }
        for c in 0..<width {
            let mean: Double = frames.reduce(0) { $0 + $1[c] } / Double(frames.count)
            for i in frames.indices { frames[i][c] -= mean }
        }
        return frames
    }
}

public struct SilentNormalizer: Equatable, Sendable {
    public let scale: [Double]

    public init(scale: [Double]) {
        self.scale = scale
    }

    public init(_ sets: [[[Double]]]) {
        let width: Int = sets.first { !$0.isEmpty }?.first?.count ?? 0
        var sum: [Double] = Array(repeating: 0, count: width)
        var squares: [Double] = Array(repeating: 0, count: width)
        var n: Double = 0
        for frames in sets {
            for row in frames where row.count == width {
                for c in 0..<width {
                    sum[c] += row[c]
                    squares[c] += row[c] * row[c]
                }
                n += 1
            }
        }
        scale = (0..<width).map { (c: Int) -> Double in
            guard n > 1 else { return 1 }
            let mean: Double = sum[c] / n
            let variance: Double = max(squares[c] / n - mean * mean, 0)
            let sd: Double = variance.squareRoot()
            return sd > 1e-9 ? 1 / sd : 1
        }
    }

    public func apply(_ frames: [[Double]]) -> [[Double]] {
        frames.map { (row: [Double]) -> [Double] in
            row.count == scale.count ? zip(row, scale).map { $0 * $1 } : row
        }
    }
}

public enum SilentDTW {
    public static let band: Double = 0.25

    public static func distance(_ a: [[Double]], _ b: [[Double]], band: Double = SilentDTW.band, cutoff: Double = .infinity) -> Double {
        let n: Int = a.count
        let m: Int = b.count
        guard n > 0, m > 0 else { return .infinity }
        let window: Int = max(Int((band * Double(max(n, m))).rounded(.up)), abs(n - m))
        let norm: Double = Double(n + m)
        var previous: [Double] = Array(repeating: .infinity, count: m + 1)
        var current: [Double] = Array(repeating: .infinity, count: m + 1)
        previous[0] = 0
        for i in 1...n {
            for j in 0...m { current[j] = .infinity }
            let lo: Int = max(1, i - window)
            let hi: Int = min(m, i + window)
            var rowBest: Double = .infinity
            if lo <= hi {
                for j in lo...hi {
                    let cost: Double = euclidean(a[i - 1], b[j - 1])
                    let step: Double = min(previous[j], current[j - 1], previous[j - 1])
                    current[j] = cost + step
                    rowBest = min(rowBest, current[j])
                }
            }
            if rowBest / norm > cutoff { return .infinity }
            swap(&previous, &current)
        }
        return previous[m] / norm
    }

    static func euclidean(_ a: [Double], _ b: [Double]) -> Double {
        var sum: Double = 0
        for k in 0..<min(a.count, b.count) {
            let d: Double = a[k] - b[k]
            sum += d * d
        }
        return sum.squareRoot()
    }
}

public struct SilentWord: Equatable, Sendable {
    public var word: String
    public var phrase: String

    public init(_ word: String, phrase: String = "") {
        self.word = word
        self.phrase = phrase
    }

    public var spoken: String { phrase.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty ? word : phrase }
}

public struct SilentTake: Equatable, Sendable {
    public var id: String
    public var word: String
    public var samples: [SilentSample]

    public init(id: String, word: String, samples: [SilentSample]) {
        self.id = id
        self.word = word
        self.samples = samples
    }
}

public struct SilentMatch: Equatable, Sendable {
    public let word: String
    public let distance: Double
    public let runnerUp: String?
    public let margin: Double
    public let accepted: Bool

    public init(word: String, distance: Double, runnerUp: String?, margin: Double, accepted: Bool) {
        self.word = word
        self.distance = distance
        self.runnerUp = runnerUp
        self.margin = margin
        self.accepted = accepted
    }

    public static func margin(_ first: Double, _ second: Double?) -> Double {
        guard let second, second.isFinite, second > 0 else { return 1 }
        return max(0, min(1, (second - first) / second))
    }

    public var summary: String {
        let sure: Int = Int((margin * 100).rounded())
        if accepted { return "\(word) · \(sure)% ahead of \(runnerUp ?? "nothing")" }
        return "Not sure · closest was \(word)"
    }
}

public enum SilentScores {
    public static func perWord(_ distances: [(word: String, distance: Double)], neighbors: Int) -> [(word: String, distance: Double)] {
        var byWord: [String: [Double]] = [:]
        var order: [String] = []
        for entry in distances where entry.distance.isFinite {
            if byWord[entry.word] == nil { order.append(entry.word) }
            byWord[entry.word, default: []].append(entry.distance)
        }
        let k: Int = max(1, neighbors)
        let scored: [(word: String, distance: Double)] = order.map { (word: String) -> (word: String, distance: Double) in
            let best: [Double] = Array(byWord[word, default: []].sorted().prefix(k))
            return (word, best.reduce(0, +) / Double(best.count))
        }
        return scored.enumerated().sorted { ($0.element.distance, $0.offset) < ($1.element.distance, $1.offset) }.map(\.element)
    }

    public static func ranked(_ scores: [(word: String, distance: Double)], language: SilentLanguage?, previous: String?) -> [(word: String, distance: Double, effective: Double)] {
        guard let language, scores.count > 1 else { return scores.map { ($0.word, $0.distance, $0.distance) } }
        let best: Double = scores[0].distance
        let middle: Double = scores[scores.count / 2].distance
        let tau: Double = max((middle - best) / 2, best * 0.05, 1e-6)
        let costs: [Double] = scores.map { language.cost($0.word, after: previous, vocabulary: scores.count) }
        let lowest: Double = costs.min() ?? 0
        let adjusted: [(word: String, distance: Double, effective: Double)] = scores.indices.map { (i: Int) -> (word: String, distance: Double, effective: Double) in
            (scores[i].word, scores[i].distance, scores[i].distance + SilentLanguage.weight * tau * (costs[i] - lowest))
        }
        return adjusted.enumerated().sorted { ($0.element.effective, $0.offset) < ($1.element.effective, $1.offset) }.map(\.element)
    }
}

public struct SilentLanguage: Equatable, Sendable {
    public static let weight: Double = 0.5
    public static let smoothing: Double = 2

    public private(set) var unigrams: [String: Int] = [:]
    public private(set) var bigrams: [String: [String: Int]] = [:]
    public private(set) var total: Int = 0

    public init(sentences: [[String]]) {
        for sentence in sentences where !sentence.isEmpty {
            var previous: String = ""
            for word in sentence {
                unigrams[word, default: 0] += 1
                bigrams[previous, default: [:]][word, default: 0] += 1
                total += 1
                previous = word
            }
        }
    }

    public func probability(_ word: String, after previous: String?, vocabulary: Int) -> Double {
        let size: Double = Double(max(vocabulary, unigrams.count, 1))
        let unigram: Double = (Double(unigrams[word] ?? 0) + 1) / (Double(total) + size)
        let context: [String: Int] = bigrams[previous ?? ""] ?? [:]
        let seen: Double = Double(context.values.reduce(0, +))
        return (Double(context[word] ?? 0) + SilentLanguage.smoothing * unigram) / (seen + SilentLanguage.smoothing)
    }

    public func cost(_ word: String, after previous: String?, vocabulary: Int) -> Double {
        -log(probability(word, after: previous, vocabulary: vocabulary))
    }
}

public struct SilentModel: Sendable {
    public let channels: Set<SilentChannel>
    public let neighbors: Int
    public let normalizer: SilentNormalizer
    public let templates: [(word: String, frames: [[Double]])]
    public let language: SilentLanguage?

    public init(takes: [SilentTake], channels: Set<SilentChannel>, neighbors: Int = 1, language: SilentLanguage? = nil) {
        self.channels = channels
        self.neighbors = neighbors
        self.language = language
        let raw: [(word: String, frames: [[Double]])] = takes.map { ($0.word, SilentFeatures.frames($0.samples, channels: channels)) }.filter { !$0.frames.isEmpty }
        let normalizer: SilentNormalizer = SilentNormalizer(raw.map(\.frames))
        self.normalizer = normalizer
        templates = raw.map { ($0.word, normalizer.apply($0.frames)) }
    }

    public var words: Set<String> { Set(templates.map(\.word)) }

    public func match(_ samples: [SilentSample], maxDistance: Double?, minMargin: Double, previous: String? = nil) -> SilentMatch? {
        let frames: [[Double]] = normalizer.apply(SilentFeatures.frames(samples, channels: channels))
        guard !frames.isEmpty, !templates.isEmpty else { return nil }
        let distances: [(word: String, distance: Double)] = templates.map { ($0.word, SilentDTW.distance(frames, $0.frames)) }
        let ranked = SilentScores.ranked(SilentScores.perWord(distances, neighbors: neighbors), language: language, previous: previous)
        guard let best = ranked.first else { return nil }
        let second = ranked.count > 1 ? ranked[1] : nil
        let margin: Double = SilentMatch.margin(best.effective, second?.effective)
        let accepted: Bool = (maxDistance.map { best.distance <= $0 } ?? true) && (second == nil || margin >= minMargin)
        return SilentMatch(word: best.word, distance: best.distance, runnerUp: second?.word, margin: margin, accepted: accepted)
    }
}

public struct SilentRandom: Sendable {
    private var state: UInt64

    public init(seed: UInt64) {
        state = seed
    }

    public mutating func next() -> UInt64 {
        state &+= 0x9E37_79B9_7F4A_7C15
        var z: UInt64 = state
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        return z ^ (z >> 31)
    }

    public mutating func below(_ n: Int) -> Int { Int(next() % UInt64(n)) }

    public mutating func unit() -> Double { Double(next() >> 11) / 9_007_199_254_740_992 }

    public mutating func shuffle(_ values: inout [Int]) {
        guard values.count > 1 else { return }
        for i in stride(from: values.count - 1, to: 0, by: -1) {
            values.swapAt(i, below(i + 1))
        }
    }
}

public struct SilentEvaluation: Equatable, Sendable {
    public let words: [String]
    public let evaluated: Int
    public let correct: Int
    public let nullMean: Double
    public let pValue: Double
    public let permutations: Int
    public let confusion: [[Int]]
    public let suggestedMaxDistance: Double?

    public init(words: [String], evaluated: Int, correct: Int, nullMean: Double, pValue: Double, permutations: Int, confusion: [[Int]], suggestedMaxDistance: Double?) {
        self.words = words
        self.evaluated = evaluated
        self.correct = correct
        self.nullMean = nullMean
        self.pValue = pValue
        self.permutations = permutations
        self.confusion = confusion
        self.suggestedMaxDistance = suggestedMaxDistance
    }

    public var accuracy: Double { evaluated > 0 ? Double(correct) / Double(evaluated) : 0 }
    public var chance: Double { words.isEmpty ? 0 : 1 / Double(words.count) }
    public var significant: Bool { pValue < 0.05 }

    public var headline: String {
        "Right \(correct) of \(evaluated) (\(SilentEvaluation.percent(accuracy))), chance is about \(SilentEvaluation.percent(max(chance, nullMean)))"
    }

    public var verdict: String {
        let p: String = pValue < 0.001 ? "p < 0.001" : String(format: "p = %.3f", pValue)
        if significant { return "\(p): your head motion tells these words apart better than chance." }
        return "\(p): not yet better than chance. Record more takes, or mouth the words more clearly."
    }

    public func mixups(_ limit: Int = 3) -> [(word: String, guess: String, count: Int)] {
        var out: [(word: String, guess: String, count: Int)] = []
        for row in words.indices {
            for column in words.indices where row != column && confusion[row][column] > 0 {
                out.append((words[row], words[column], confusion[row][column]))
            }
        }
        return Array(out.enumerated().sorted { (-$0.element.count, $0.offset) < (-$1.element.count, $1.offset) }.map(\.element).prefix(limit))
    }

    static func percent(_ value: Double) -> String { "\(Int((value * 100).rounded()))%" }
}

public enum SilentEvaluator {
    public static let permutations: Int = 500
    public static let seed: UInt64 = 0x5EED_5EED
    public static let minimumTakes: Int = 2
    public static let manyTakes: Int = 10
    public static let manyNeighbors: Int = 3
    public static let maxPerWord: Int = 12
    public static let maxTakes: Int = 600
    public static let minimumPermutations: Int = 100
    public static let budget: Int = 40_000_000

    public static func rounds(_ requested: Int, takes: Int) -> Int {
        min(requested, max(minimumPermutations, budget / max(1, takes * takes)))
    }

    public static func sample(_ takes: [SilentTake], perWord: Int = SilentEvaluator.maxPerWord, total: Int = SilentEvaluator.maxTakes) -> [SilentTake] {
        var seen: [String: Int] = [:]
        var kept: [Int] = []
        for i in takes.indices.reversed() where kept.count < total {
            let n: Int = seen[takes[i].word, default: 0]
            guard n < perWord else { continue }
            seen[takes[i].word] = n + 1
            kept.append(i)
        }
        return kept.reversed().map { takes[$0] }
    }

    public static func neighbors(for dataset: SilentDataset) -> Int {
        let counts: [Int] = dataset.words.map { dataset.count($0.word) }.filter { $0 > 0 }
        guard counts.count >= 2, let fewest = counts.min() else { return 1 }
        return fewest >= manyTakes ? manyNeighbors : 1
    }

    public static func requirement(_ dataset: SilentDataset) -> String? {
        let ready: Int = dataset.words.filter { dataset.count($0.word) >= minimumTakes }.count
        guard ready < 2 else { return nil }
        return "Record at least \(minimumTakes) takes of at least 2 words to check them. Five or more takes each gives a fairer score."
    }

    public static func matrix(_ frames: [[[Double]]]) -> [[Double]] {
        let n: Int = frames.count
        var d: [[Double]] = Array(repeating: Array(repeating: 0, count: n), count: n)
        for i in 0..<n {
            for j in (i + 1)..<max(i + 1, n) {
                let value: Double = SilentDTW.distance(frames[i], frames[j])
                d[i][j] = value
                d[j][i] = value
            }
        }
        return d
    }

    public static func leaveOneOut(_ d: [[Double]], labels: [Int], wordCount: Int, neighbors: Int) -> [(truth: Int, guess: Int, distance: Double)] {
        var counts: [Int] = Array(repeating: 0, count: wordCount)
        for label in labels { counts[label] += 1 }
        let k: Int = max(1, neighbors)
        var out: [(truth: Int, guess: Int, distance: Double)] = []
        var near: [[Double]] = Array(repeating: [], count: wordCount)
        for i in labels.indices where counts[labels[i]] >= minimumTakes {
            for w in 0..<wordCount { near[w].removeAll(keepingCapacity: true) }
            for j in labels.indices where j != i { keep(&near[labels[j]], d[i][j], k) }
            var guess: Int = -1
            var score: Double = .infinity
            for w in 0..<wordCount where !near[w].isEmpty {
                let value: Double = k == 1 ? near[w][0] : mean(near[w][...])
                if value < score {
                    score = value
                    guess = w
                }
            }
            if guess >= 0 { out.append((labels[i], guess, score)) }
        }
        return out
    }

    static func keep(_ best: inout [Double], _ value: Double, _ k: Int) {
        var i: Int
        if best.count < k {
            best.append(value)
            i = best.count - 1
        } else if value < best[k - 1] {
            best[k - 1] = value
            i = k - 1
        } else {
            return
        }
        while i > 0 && best[i - 1] > best[i] {
            best.swapAt(i - 1, i)
            i -= 1
        }
    }

    static func mean(_ values: ArraySlice<Double>) -> Double {
        values.isEmpty ? .infinity : values.reduce(0, +) / Double(values.count)
    }

    public static func evaluate(_ dataset: SilentDataset, channels: Set<SilentChannel>, neighbors: Int = 1, permutations: Int = SilentEvaluator.permutations, seed: UInt64 = SilentEvaluator.seed) -> SilentEvaluation? {
        guard requirement(dataset) == nil else { return nil }
        let sampled: [SilentTake] = sample(dataset.takes)
        var counts: [String: Int] = [:]
        for take in sampled { counts[take.word, default: 0] += 1 }
        let words: [String] = dataset.words.map(\.word).filter { counts[$0, default: 0] >= minimumTakes }
        guard words.count >= 2 else { return nil }
        let chosen: Set<String> = Set(words)
        let takes: [SilentTake] = sampled.filter { chosen.contains($0.word) }
        let model: SilentModel = SilentModel(takes: takes, channels: channels, neighbors: neighbors)
        guard !model.templates.isEmpty else { return nil }
        let index: [String: Int] = Dictionary(uniqueKeysWithValues: words.enumerated().map { ($1, $0) })
        let labels: [Int] = model.templates.map { index[$0.word] ?? 0 }
        let d: [[Double]] = matrix(model.templates.map(\.frames))
        let results = leaveOneOut(d, labels: labels, wordCount: words.count, neighbors: neighbors)
        guard !results.isEmpty else { return nil }
        let correct: Int = results.filter { $0.truth == $0.guess }.count
        var confusion: [[Int]] = Array(repeating: Array(repeating: 0, count: words.count), count: words.count)
        for r in results { confusion[r.truth][r.guess] += 1 }
        let rounds: Int = SilentEvaluator.rounds(permutations, takes: labels.count)
        var random: SilentRandom = SilentRandom(seed: seed)
        var shuffled: [Int] = labels
        var atLeast: Int = 0
        var nullTotal: Double = 0
        for _ in 0..<rounds {
            random.shuffle(&shuffled)
            let null = leaveOneOut(d, labels: shuffled, wordCount: words.count, neighbors: neighbors)
            let hits: Int = null.filter { $0.truth == $0.guess }.count
            if hits >= correct { atLeast += 1 }
            nullTotal += null.isEmpty ? 0 : Double(hits) / Double(null.count)
        }
        let good: [Double] = results.filter { $0.truth == $0.guess }.map(\.distance).sorted()
        let suggested: Double? = good.count >= 3 ? good[max(0, Int((0.9 * Double(good.count)).rounded(.up)) - 1)] * 1.25 : nil
        return SilentEvaluation(
            words: words, evaluated: results.count, correct: correct,
            nullMean: rounds > 0 ? nullTotal / Double(rounds) : 0,
            pValue: Double(1 + atLeast) / Double(1 + rounds), permutations: rounds,
            confusion: confusion, suggestedMaxDistance: suggested)
    }
}

public struct SilentNoiseFloor: Equatable, Sendable {
    public let floor: Double
    public let spread: Double

    public init(floor: Double, spread: Double) {
        self.floor = floor
        self.spread = spread
    }

    public static let minimumRise: Double = 0.05

    public var on: Double { floor + max(4 * spread, SilentNoiseFloor.minimumRise) }
    public var off: Double { floor + max(2 * spread, SilentNoiseFloor.minimumRise / 2) }

    public static func calibrate(_ energies: [Double]) -> SilentNoiseFloor? {
        guard energies.count >= 10 else { return nil }
        let middle: Double = median(energies)
        let mad: Double = median(energies.map { abs($0 - middle) })
        return SilentNoiseFloor(floor: middle, spread: 1.4826 * mad)
    }

    static func median(_ values: [Double]) -> Double {
        let sorted: [Double] = values.sorted()
        let m: Int = sorted.count / 2
        return sorted.count % 2 == 1 ? sorted[m] : (sorted[m - 1] + sorted[m]) / 2
    }
}

public struct SilentSegmenter: Sendable {
    public let noise: SilentNoiseFloor
    public let preRoll: Double
    public let hangover: Double
    public let minimum: Double
    public let maximum: Double
    public private(set) var active: Bool = false
    private var history: [SilentSample] = []
    private var segment: [SilentSample] = []
    private var rising: Int = 0
    private var onset: Double = 0
    private var quietSince: Double?

    public init(noise: SilentNoiseFloor, preRoll: Double = 0.2, hangover: Double = 0.35, minimum: Double = 0.3, maximum: Double = 3) {
        self.noise = noise
        self.preRoll = preRoll
        self.hangover = hangover
        self.minimum = minimum
        self.maximum = maximum
    }

    public mutating func reset() {
        active = false
        history.removeAll()
        segment.removeAll()
        rising = 0
        quietSince = nil
    }

    public mutating func push(_ sample: SilentSample) -> [SilentSample]? {
        let energy: Double = sample.energy
        guard active else {
            history.append(sample)
            history.removeAll { $0.t < sample.t - preRoll }
            rising = energy > noise.on ? rising + 1 : 0
            if rising == 1 { onset = sample.t }
            if rising >= 2 {
                active = true
                segment = history
                history.removeAll()
                quietSince = nil
            }
            return nil
        }
        segment.append(sample)
        if energy < noise.off {
            if quietSince == nil { quietSince = sample.t }
        } else {
            quietSince = nil
        }
        let start: Double = segment.first?.t ?? sample.t
        let ended: Bool = quietSince.map { sample.t - $0 >= hangover } ?? false
        guard ended || sample.t - start >= maximum else { return nil }
        let cut: Double = quietSince ?? sample.t
        let finished: [SilentSample] = segment.filter { $0.t <= cut + 0.08 }
        active = false
        segment.removeAll()
        rising = 0
        quietSince = nil
        guard !finished.isEmpty, cut - onset >= minimum else { return nil }
        return finished
    }
}

public enum SilentText {
    public static let sentences: [String] = [
        "Can you help me please",
        "I would like some water",
        "Yes that sounds good to me",
        "No thank you I am fine",
        "Please stop for a moment",
        "Where are we going today",
        "What time is it now",
        "I need to go home soon",
        "Call me when you are ready",
        "Let us meet again tomorrow",
        "The weather is nice outside",
        "I am feeling tired tonight",
        "Turn the music down a little",
        "Open the door for me",
        "How was your day at work",
        "We should eat something first",
        "Send a message to my friend",
        "I do not know the answer",
        "Bring me a cup of coffee",
        "Wait here I will be back",
        "That is a really good idea",
        "Could you speak more slowly",
        "Read the next page out loud",
        "My phone battery is almost empty",
        "Remind me to buy some bread",
        "Good morning how did you sleep",
        "I love you very much",
        "Is there anything else you need",
        "Show me the way to the station",
        "Thank you for your help",
    ]

    public static func words(_ text: String) -> [String] {
        let cleaned: String = String(text.lowercased().map { (c: Character) -> Character in
            if c == "\u{2019}" { return "'" }
            return c.isLetter || c.isNumber || c == "'" ? c : " "
        })
        return cleaned.split(separator: " ").map { $0.trimmingCharacters(in: CharacterSet(charactersIn: "'")) }.filter { !$0.isEmpty }
    }
}

public struct SilentReading: Equatable, Sendable {
    public static let duration: Double = 300
    public static let scriptLength: Int = 100
    public static let sessionSentences: Int = 8
    public static let chunk: Int = 5

    public let sentences: [[String]]
    public let duration: Double
    public private(set) var index: Int = 0
    public private(set) var heard: [[SilentSample]] = []
    public private(set) var started: Double?
    public private(set) var elapsed: Double = 0
    public private(set) var sentencesRead: Int = 0
    public private(set) var collected: Int = 0

    public init(sentences: [[String]], seed: UInt64, duration: Double = SilentReading.duration) {
        self.sentences = SilentReading.script(sentences, seed: seed)
        self.duration = duration
    }

    public static func pool(_ dataset: SilentDataset) -> [[String]] {
        var out: [[String]] = []
        for text in dataset.sentences + SilentText.sentences {
            let words: [String] = SilentText.words(text)
            if !words.isEmpty && !out.contains(words) { out.append(words) }
        }
        let covered: Set<String> = Set(out.flatMap { $0 })
        let orphans: [String] = dataset.words.map(\.word).filter { !covered.contains($0) }
        for start in stride(from: 0, to: orphans.count, by: chunk) {
            out.append(Array(orphans[start..<min(orphans.count, start + chunk)]))
        }
        return out
    }

    public static func plan(_ dataset: SilentDataset, size: Int = SilentReading.sessionSentences) -> [[String]] {
        let counts: [String: Int] = dataset.counts()
        let pool: [[String]] = pool(dataset)
        let scored: [(sentence: [String], score: Double, order: Int)] = pool.enumerated().map { (item: (offset: Int, element: [String])) -> (sentence: [String], score: Double, order: Int) in
            let total: Int = item.element.reduce(0) { $0 + min(counts[$1, default: 0], 10) }
            return (item.element, Double(total) / Double(item.element.count), item.offset)
        }
        return scored.sorted { ($0.score, $0.order) < ($1.score, $1.order) }.prefix(max(1, size)).map(\.sentence)
    }

    public static func script(_ sentences: [[String]], seed: UInt64, count: Int = SilentReading.scriptLength) -> [[String]] {
        let usable: [[String]] = sentences.filter { !$0.isEmpty }
        guard !usable.isEmpty else { return [] }
        var random: SilentRandom = SilentRandom(seed: seed)
        var order: [Int] = []
        while order.count < count {
            var pass: [Int] = Array(usable.indices)
            random.shuffle(&pass)
            if let last = order.last, pass.count > 1, pass[0] == last { pass.swapAt(0, 1) }
            order.append(contentsOf: pass)
        }
        return order.prefix(count).map { usable[$0] }
    }

    public var sentence: [String] { sentences.isEmpty ? [] : sentences[index % sentences.count] }
    public var position: Int { heard.count }
    public var timeUp: Bool { elapsed >= duration }
    public var done: Bool { timeUp && heard.isEmpty }

    public var clock: String { "\(SilentReading.minutes(elapsed)) of \(SilentReading.minutes(duration))" }

    public var summary: String {
        let takes: String = collected == 1 ? "1 take" : "\(collected) takes"
        let read: String = sentencesRead == 1 ? "1 sentence" : "\(sentencesRead) sentences"
        return "Saved \(takes) from \(read)."
    }

    static func minutes(_ seconds: Double) -> String {
        let whole: Int = max(0, Int(seconds.rounded(.down)))
        return String(format: "%d:%02d", whole / 60, whole % 60)
    }

    public mutating func tick(_ t: Double) {
        if started == nil { started = t }
        elapsed = max(0, t - (started ?? t))
    }

    public mutating func hear(_ segment: [SilentSample]) -> [(word: String, samples: [SilentSample])] {
        guard !segment.isEmpty, !sentence.isEmpty else { return [] }
        heard.append(segment)
        guard heard.count >= sentence.count else { return [] }
        let labelled: [(word: String, samples: [SilentSample])] = zip(sentence, heard).map { ($0, $1) }
        heard.removeAll()
        index += 1
        sentencesRead += 1
        collected += labelled.count
        return labelled
    }

    public mutating func again() { heard.removeAll() }

    public mutating func skip() {
        heard.removeAll()
        index += 1
    }

    public mutating func forget(_ takes: Int) {
        guard takes > 0, sentencesRead > 0 else { return }
        sentencesRead -= 1
        collected = max(0, collected - takes)
    }
}

public struct SilentScopeFrame: Equatable, Sendable {
    public let t: Double
    public let rotation: SilentVector
    public let angle: SilentVector
    public let acceleration: SilentVector?
    public let energy: Double
}

public struct SilentMark: Equatable, Sendable {
    public let start: Double
    public let end: Double
    public let label: String
}

public struct SilentSpectrum: Equatable, Sendable {
    public let power: [Double]
    public let binHz: Double
    public let peakHz: Double?

    public static let empty: SilentSpectrum = SilentSpectrum(power: [], binHz: SilentFeatures.rate / Double(SilentScope.spectrumSize), peakHz: nil)
}

public struct SilentScope: Sendable {
    public static let window: Double = 6
    public static let spectrumSize: Int = 64
    public static let minimumSpectrum: Int = 16
    public static let lowestPeakHz: Double = 0.5
    public static let degrees: Double = 180 / Double.pi

    public private(set) var samples: [SilentSample] = []
    public private(set) var reference: SilentQuaternion?
    public private(set) var marks: [SilentMark] = []

    public init() {}

    public var latest: Double? { samples.last?.t }

    public mutating func push(_ sample: SilentSample) {
        if reference == nil { reference = sample.attitude }
        samples.append(sample)
        let start: Double = sample.t - SilentScope.window
        if let first = samples.firstIndex(where: { $0.t >= start }), first > 0 { samples.removeFirst(first) }
        marks.removeAll { $0.end < start }
    }

    public mutating func recentre() {
        reference = samples.last?.attitude ?? reference
    }

    public mutating func mark(_ segment: [SilentSample], label: String) {
        guard let first = segment.first, let last = segment.last else { return }
        marks.append(SilentMark(start: first.t, end: last.t, label: label))
    }

    public mutating func clear() {
        samples.removeAll()
        marks.removeAll()
        reference = nil
    }

    public var frames: [SilentScopeFrame] {
        let origin: SilentQuaternion = (reference ?? samples.first?.attitude ?? .identity).conjugate
        let d: Double = SilentScope.degrees
        return SilentFeatures.resampled(samples).map { (s: SilentSample) -> SilentScopeFrame in
            let r: SilentVector = s.rotationRate
            let a: SilentVector = (origin * s.attitude).rotationVector
            return SilentScopeFrame(t: s.t, rotation: SilentVector(r.x * d, r.y * d, r.z * d), angle: SilentVector(a.x * d, a.y * d, a.z * d), acceleration: s.acceleration, energy: s.energy)
        }
    }

    public var spectrum: SilentSpectrum { SilentScope.spectrum(frames.map(\.rotation)) }

    public static func spectrum(_ values: [SilentVector], size: Int = SilentScope.spectrumSize, rate: Double = SilentFeatures.rate) -> SilentSpectrum {
        let binHz: Double = rate / Double(size)
        let tail: [SilentVector] = Array(values.suffix(size))
        guard tail.count >= minimumSpectrum else { return SilentSpectrum(power: [], binHz: binHz, peakHz: nil) }
        let n: Int = tail.count
        let pad: Int = size - n
        var power: [Double] = Array(repeating: 0, count: size / 2 + 1)
        for axis in 0..<3 {
            let raw: [Double] = tail.map { axis == 0 ? $0.x : (axis == 1 ? $0.y : $0.z) }
            let mean: Double = raw.reduce(0, +) / Double(n)
            var signal: [Double] = Array(repeating: 0, count: size)
            for i in 0..<n {
                let hann: Double = n > 1 ? 0.5 - 0.5 * cos(2 * Double.pi * Double(i) / Double(n - 1)) : 1
                signal[pad + i] = (raw[i] - mean) * hann
            }
            for k in 0...(size / 2) {
                var re: Double = 0
                var im: Double = 0
                for i in 0..<size {
                    let phase: Double = 2 * Double.pi * Double(k * i) / Double(size)
                    re += signal[i] * cos(phase)
                    im -= signal[i] * sin(phase)
                }
                power[k] += (re * re + im * im) / Double(size)
            }
        }
        var peak: Int?
        for k in power.indices where Double(k) * binHz >= lowestPeakHz && power[k] > 1e-6 {
            if peak.map({ power[k] > power[$0] }) ?? true { peak = k }
        }
        return SilentSpectrum(power: power, binHz: binHz, peakHz: peak.map { Double($0) * binHz })
    }
}

public struct SilentDataset: Equatable, Sendable {
    public var words: [SilentWord]
    public var takes: [SilentTake]
    public var sentences: [String]

    public init(words: [SilentWord], takes: [SilentTake] = [], sentences: [String] = []) {
        self.words = words
        self.takes = takes
        self.sentences = sentences
    }

    public static let starter: SilentDataset = SilentDataset(words: ["yes", "no", "help", "stop", "water"].map { SilentWord($0) })

    public func count(_ word: String) -> Int { takes.filter { $0.word == word }.count }

    public func counts() -> [String: Int] {
        var out: [String: Int] = [:]
        for take in takes { out[take.word, default: 0] += 1 }
        return out
    }

    public var language: SilentLanguage {
        SilentLanguage(sentences: (sentences + SilentText.sentences).map(SilentText.words))
    }

    public static func clean(_ word: String) -> String {
        word.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
    }

    @discardableResult
    public mutating func addWord(_ word: String, phrase: String = "") -> Bool {
        let name: String = SilentDataset.clean(word)
        guard !name.isEmpty, !words.contains(where: { $0.word == name }) else { return false }
        words.append(SilentWord(name, phrase: phrase.trimmingCharacters(in: .whitespacesAndNewlines)))
        return true
    }

    @discardableResult
    public mutating func addSentence(_ text: String) -> Bool {
        let words: [String] = SilentText.words(text)
        let trimmed: String = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard words.count >= 2, !sentences.contains(where: { SilentText.words($0) == words }) else { return false }
        sentences.append(trimmed)
        words.forEach { addWord($0) }
        return true
    }

    public mutating func addEntry(_ text: String) -> String? {
        if let colon = text.firstIndex(of: ":") {
            let word: String = SilentDataset.clean(String(text[..<colon]))
            guard addWord(word, phrase: String(text[text.index(after: colon)...])) else { return nil }
            return word
        }
        let words: [String] = SilentText.words(text)
        if words.count >= 2 { return addSentence(text) ? words.first : nil }
        guard let word = words.first, addWord(word) else { return nil }
        return word
    }

    public mutating func removeSentence(_ text: String) {
        sentences.removeAll { $0 == text }
    }

    public mutating func learn(_ take: SilentTake) {
        addWord(take.word)
        addTake(take)
    }

    public mutating func setPhrase(_ phrase: String, for word: String) {
        guard let i = words.firstIndex(where: { $0.word == word }) else { return }
        words[i].phrase = phrase.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    public mutating func removeWord(_ word: String) {
        words.removeAll { $0.word == word }
        takes.removeAll { $0.word == word }
    }

    public mutating func addTake(_ take: SilentTake) {
        guard words.contains(where: { $0.word == take.word }), take.samples.count >= 2 else { return }
        takes.append(take)
    }

    public mutating func removeLastTake(_ word: String) {
        guard let i = takes.lastIndex(where: { $0.word == word }) else { return }
        takes.remove(at: i)
    }

    public mutating func removeTakes(_ ids: Set<String>) {
        takes.removeAll { ids.contains($0.id) }
    }

    public func phrase(_ word: String) -> String { words.first { $0.word == word }?.spoken ?? word }

    public static let version: Int = 1

    public func json() -> [String: Any] {
        [
            "sentences": sentences,
            "version": SilentDataset.version,
            "words": words.map { ["word": $0.word, "phrase": $0.phrase] },
            "takes": takes.map { (take: SilentTake) -> [String: Any] in
                ["id": take.id, "word": take.word, "samples": take.samples.map(SilentDataset.encode)]
            },
        ]
    }

    public func encoded() -> Data {
        (try? JSONSerialization.data(withJSONObject: json(), options: [.sortedKeys])) ?? Data()
    }

    public static func decode(_ data: Data) -> SilentDataset? {
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { return nil }
        return decode(object)
    }

    public static func decode(_ object: [String: Any]) -> SilentDataset? {
        guard (object["version"] as? Int ?? 0) <= version, let rawWords = object["words"] as? [[String: Any]] else { return nil }
        var dataset: SilentDataset = SilentDataset(words: [], sentences: (object["sentences"] as? [Any] ?? []).compactMap { $0 as? String })
        for raw in rawWords {
            if let word = raw["word"] as? String { dataset.addWord(word, phrase: raw["phrase"] as? String ?? "") }
        }
        for raw in object["takes"] as? [[String: Any]] ?? [] {
            guard let word = raw["word"] as? String, let rows = raw["samples"] as? [[Any]] else { continue }
            let samples: [SilentSample] = rows.compactMap(decodeSample)
            dataset.addTake(SilentTake(id: raw["id"] as? String ?? UUID().uuidString, word: word, samples: samples))
        }
        return dataset
    }

    static func round(_ value: Double) -> Double { (value * 100_000).rounded() / 100_000 }

    static func encode(_ s: SilentSample) -> [Double] {
        var row: [Double] = [s.t, s.rotationRate.x, s.rotationRate.y, s.rotationRate.z, s.attitude.w, s.attitude.x, s.attitude.y, s.attitude.z]
        if let a = s.acceleration { row.append(contentsOf: [a.x, a.y, a.z]) }
        return row.map(round)
    }

    static func decodeSample(_ raw: [Any]) -> SilentSample? {
        let v: [Double] = raw.compactMap { ($0 as? NSNumber)?.doubleValue }
        guard v.count == raw.count, v.count == 8 || v.count == 11 else { return nil }
        let acceleration: SilentVector? = v.count == 11 ? SilentVector(v[8], v[9], v[10]) : nil
        return SilentSample(t: v[0], rotationRate: SilentVector(v[1], v[2], v[3]), attitude: SilentQuaternion(w: v[4], x: v[5], y: v[6], z: v[7]).normalized, acceleration: acceleration)
    }
}

public struct SilentTranscript: Equatable, Sendable {
    public private(set) var words: [String] = []

    public init(words: [String] = []) {
        self.words = words
    }

    public var text: String { words.joined(separator: " ") }

    public mutating func append(_ word: String) { words.append(word) }

    public mutating func undo() {
        if !words.isEmpty { words.removeLast() }
    }

    public mutating func clear() { words.removeAll() }
}

public enum SilentSpeechPrefs {
    public static let channels: String = "silentSpeech.channels"
    public static let minMargin: String = "silentSpeech.minMargin"
    public static let maxDistance: String = "silentSpeech.maxDistance"
    public static let speakAloud: String = "silentSpeech.speakAloud"
    public static let autoDetect: String = "silentSpeech.autoDetect"
    public static let neighbors: String = "silentSpeech.neighbors"
    public static let defaultMinMargin: Double = 0.08
}
