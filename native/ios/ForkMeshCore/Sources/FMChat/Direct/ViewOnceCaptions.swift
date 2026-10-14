import Foundation

public struct CaptionWord: Equatable, Sendable {
    public let text: String
    public let start: Double
    public let duration: Double

    public init(text: String, start: Double, duration: Double) {
        self.text = text
        self.start = start
        self.duration = duration
    }

    public var end: Double { start + max(0, duration) }
}

public struct CaptionLine: Equatable, Sendable {
    public let words: [CaptionWord]
    public let start: Double
    public let end: Double

    public init(words: [CaptionWord], start: Double, end: Double) {
        self.words = words
        self.start = start
        self.end = end
    }

    public var text: String { words.map(\.text).joined(separator: " ") }
}

public enum CaptionWordState: Equatable, Sendable {
    case spoken
    case active
    case upcoming
}

public struct CaptionCue: Hashable, Sendable {
    public let line: Int
    public let word: Int

    public init(line: Int, word: Int) {
        self.line = line
        self.word = word
    }

    public func state(of index: Int) -> CaptionWordState {
        if index < word { return .spoken }
        return index == word ? .active : .upcoming
    }
}

public struct CaptionLayout: Equatable, Sendable {
    public static let minFontSize: Double = 12
    public static let activeScale: Double = 1.08

    public let width: Double
    public let height: Double
    public let fontSize: Double
    public let bottomInset: Double
    public let maxWidth: Double

    public init(width: Double, height: Double) {
        self.width = width
        self.height = height
        fontSize = max(CaptionLayout.minFontSize, (min(height, width * 1.4) * 0.042).rounded())
        bottomInset = (height * 0.08).rounded()
        maxWidth = (width * 0.9).rounded(.down)
    }

    public func horizontalPadding(_ size: Double) -> Double { (size * 0.55).rounded() }

    public func verticalPadding(_ size: Double) -> Double { (size * 0.3).rounded() }

    public func cornerRadius(_ size: Double) -> Double { (size * 0.6).rounded() }

    public func margin(_ size: Double) -> Double { (size * 0.3).rounded(.up) }

    public func fittedFontSize(textWidth: Double) -> Double {
        guard textWidth > 0 else { return fontSize }
        let needed: Double = textWidth + 2 * horizontalPadding(fontSize)
        guard needed > maxWidth else { return fontSize }
        let scaled: Double = (fontSize * maxWidth / (textWidth + 1.1 * fontSize)).rounded(.down)
        return max(CaptionLayout.minFontSize, min(fontSize, scaled))
    }
}

public enum ViewOnceCaptionsFailure: String, Error, Sendable, Equatable {
    case denied
    case unavailable
    case unsupported
    case noAudio
    case noWords
    case tooLarge
    case failed

    public var message: String {
        switch self {
        case .denied: return "Allow Speech Recognition for ForkMesh in Settings to add captions."
        case .unavailable:
            return "Captions need on-device speech recognition, which this iPhone does not have for your language."
        case .unsupported: return "Captions can only be added to MP4 recordings."
        case .noAudio: return "This video has no sound to caption."
        case .noWords: return "No words heard, so there are no captions."
        case .tooLarge: return "With captions the video is too large to send. You can send it without them."
        case .failed: return "Captions could not be added. You can still send the video without them."
        }
    }
}

public enum ViewOnceCaptions {
    public static let prefKey = "chat.videoCaptions"
    public static let maxWords = 6
    public static let maxCharacters = 28
    public static let clauseWords = 3
    public static let pauseBreak: Double = 0.6
    public static let hold: Double = 1.2
    public static let lastWordGrace: Double = 0.35
    public static let minWordDuration: Double = 0.12
    public static let audioBitRate = 64_000

    private static let sentenceEnds: Set<Character> = [".", "!", "?", "…", "。", "！", "？"]
    private static let clauseEnds: Set<Character> = [",", ";", ":", "—", "，", "、"]

    public static func canCaption(mime: String) -> Bool {
        let bare: String = mime.split(separator: ";").first.map(String.init)?.trimmingCharacters(in: .whitespaces).lowercased() ?? ""
        return bare == "video/mp4" || bare == "video/quicktime"
    }

    public static func cleaned(_ words: [CaptionWord]) -> [CaptionWord] {
        var out: [CaptionWord] = []
        let ordered: [CaptionWord] = words.enumerated()
            .sorted { (a: (offset: Int, element: CaptionWord), b: (offset: Int, element: CaptionWord)) -> Bool in
                a.element.start == b.element.start ? a.offset < b.offset : a.element.start < b.element.start
            }
            .map(\.element)
        for word in ordered {
            let text: String = word.text.trimmingCharacters(in: .whitespacesAndNewlines)
            guard !text.isEmpty, word.start.isFinite, word.duration.isFinite else { continue }
            let start: Double = max(0, word.start)
            if text.allSatisfy({ (character: Character) -> Bool in character.isPunctuation }), let last = out.last {
                out[out.count - 1] = CaptionWord(
                    text: last.text + text, start: last.start, duration: max(last.duration, start + word.duration - last.start))
                continue
            }
            out.append(CaptionWord(text: text, start: start, duration: max(minWordDuration, word.duration)))
        }
        return out
    }

    public static func lines(_ words: [CaptionWord]) -> [CaptionLine] {
        var groups: [[CaptionWord]] = []
        var current: [CaptionWord] = []
        var characters: Int = 0
        for word in cleaned(words) {
            if let last = current.last {
                let wouldBe: Int = characters + 1 + word.text.count
                let pause: Bool = word.start - last.end > pauseBreak
                let sentence: Bool = last.text.last.map { sentenceEnds.contains($0) } ?? false
                let clause: Bool = current.count >= clauseWords && (last.text.last.map { clauseEnds.contains($0) } ?? false)
                if current.count >= maxWords || wouldBe > maxCharacters || pause || sentence || clause {
                    groups.append(current)
                    current = []
                    characters = 0
                }
            }
            characters += (current.isEmpty ? 0 : 1) + word.text.count
            current.append(word)
        }
        if !current.isEmpty { groups.append(current) }
        var out: [CaptionLine] = []
        for (index, group) in groups.enumerated() {
            guard let first = group.first, let last = group.last else { continue }
            var end: Double = last.end + hold
            if index + 1 < groups.count, let next = groups[index + 1].first { end = min(end, next.start) }
            out.append(CaptionLine(words: group, start: first.start, end: max(end, first.start + minWordDuration)))
        }
        return out
    }

    public static func cue(at time: Double, in lines: [CaptionLine]) -> CaptionCue? {
        guard time.isFinite else { return nil }
        guard let index = lines.lastIndex(where: { (line: CaptionLine) -> Bool in line.start <= time }) else { return nil }
        let line: CaptionLine = lines[index]
        guard time < line.end else { return nil }
        let word: Int = line.words.lastIndex { (word: CaptionWord) -> Bool in word.start <= time } ?? 0
        if word == line.words.count - 1, time > line.words[word].end + lastWordGrace {
            return CaptionCue(line: index, word: line.words.count)
        }
        return CaptionCue(line: index, word: word)
    }

    public static func videoBitRate(durationSeconds: Double, sourceBitRate: Double, maxBytes: Int, audioBitRate: Int)
        -> Int
    {
        let seconds: Double = max(1, durationSeconds.isFinite ? durationSeconds : 60)
        let budget: Double = Double(maxBytes) * 0.85 * 8 / seconds - Double(audioBitRate)
        let wanted: Double = sourceBitRate.isFinite && sourceBitRate > 0 ? max(600_000, sourceBitRate * 1.2) : 1_200_000
        return Int(max(250_000, min(budget, wanted)).rounded())
    }
}
