import FMBase
import Foundation

public struct TextMatch: Equatable, Sendable, Identifiable {
    public var id: Int { offset }
    public let line: Int
    public let column: Int
    public let offset: Int
    public let length: Int
}

public struct FindOptions: Equatable, Sendable {
    public var caseSensitive: Bool
    public var wholeWord: Bool

    public init(caseSensitive: Bool = false, wholeWord: Bool = false) {
        self.caseSensitive = caseSensitive
        self.wholeWord = wholeWord
    }
}

public struct FindInText: Equatable, Sendable {
    public static let maxMatches = 5000

    public private(set) var query = ""
    public private(set) var matches: [TextMatch] = []
    public private(set) var current: Int?
    public var options = FindOptions()

    public init() {}

    static func isWord(_ unit: UInt16) -> Bool {
        guard let scalar = Unicode.Scalar(unit) else { return true }
        return CharacterSet.alphanumerics.contains(scalar) || unit == 0x5F
    }

    public mutating func search(_ query: String, in text: String) {
        self.query = query
        matches = []
        current = nil
        guard !query.isEmpty else { return }
        let hay = Array((options.caseSensitive ? text : text.lowercased()).utf16)
        let needle = Array((options.caseSensitive ? query : query.lowercased()).utf16)
        guard needle.count <= hay.count else { return }
        var line = 1
        var lineStart = 0
        var i = 0
        while i + needle.count <= hay.count {
            if hay[i] == 0x0A {
                line += 1
                lineStart = i + 1
                i += 1
                continue
            }
            if hay[i] == needle[0] && Array(hay[i..<(i + needle.count)]) == needle {
                let before = i == 0 ? true : !FindInText.isWord(hay[i - 1])
                let afterIndex = i + needle.count
                let after = afterIndex >= hay.count ? true : !FindInText.isWord(hay[afterIndex])
                if !options.wholeWord || (before && after) {
                    matches.append(TextMatch(line: line, column: i - lineStart + 1, offset: i, length: needle.count))
                    if matches.count >= FindInText.maxMatches { break }
                    i += max(1, needle.count)
                    continue
                }
            }
            i += 1
        }
        current = matches.isEmpty ? nil : 0
    }

    public mutating func next() {
        guard !matches.isEmpty else { return }
        current = ((current ?? -1) + 1) % matches.count
    }

    public mutating func previous() {
        guard !matches.isEmpty else { return }
        current = ((current ?? 0) - 1 + matches.count) % matches.count
    }

    public mutating func jump(toLine line: Int) {
        guard let index = matches.firstIndex(where: { $0.line >= line }) else { return }
        current = index
    }

    public var currentMatch: TextMatch? { current.map { matches[$0] } }

    public var status: String {
        if query.isEmpty { return "" }
        if matches.isEmpty { return "No results" }
        let more = matches.count >= FindInText.maxMatches ? "+" : ""
        return "\((current ?? 0) + 1) of \(matches.count)\(more)"
    }

    public static func replacing(_ text: String, query: String, with replacement: String, options: FindOptions = FindOptions()) -> (String, Int) {
        var finder = FindInText()
        finder.options = options
        finder.search(query, in: text)
        guard !finder.matches.isEmpty else { return (text, 0) }
        var units = Array(text.utf16)
        let insert = Array(replacement.utf16)
        for match in finder.matches.reversed() {
            units.replaceSubrange(match.offset..<(match.offset + match.length), with: insert)
        }
        return (String(decoding: units, as: UTF16.self), finder.matches.count)
    }
}

public enum GoToLine {
    public static func parse(_ text: String, lineCount: Int) -> Int? {
        let clean = text.dartTrimmed.split(separator: ":").first.map(String.init) ?? ""
        guard let line = Int(clean), line > 0, lineCount > 0 else { return nil }
        return min(line, lineCount)
    }
}

public struct CodeSymbol: Equatable, Sendable, Identifiable {
    public var id: String { "\(line):\(name)" }
    public let name: String
    public let kind: String
    public let line: Int
    public let depth: Int
}

public struct CodeReference: Equatable, Sendable, Identifiable {
    public var id: String { "\(symbol)#\(line)" }
    public let symbol: String
    public let line: Int
    public let text: String
    public let isCall: Bool
}

public enum CodeOutline {
    public static let maxSymbols = 2000

    static let patterns: [(String, NSRegularExpression)] = [
        ("func", #"^\s*(?:(?:public|private|internal|fileprivate|open|static|class|override|final|mutating|nonisolated|@\w+(?:\([^)]*\))?)\s+)*func\s+([A-Za-z_][A-Za-z0-9_]*)"#),
        ("type", #"^\s*(?:(?:public|private|internal|fileprivate|open|final|export|default|abstract|pub(?:\([^)]*\))?)\s+)*(?:class|struct|enum|protocol|actor|extension|interface|trait|union)\s+([A-Za-z_][A-Za-z0-9_.]*)"#),
        ("func", #"^\s*(?:async\s+)?def\s+([A-Za-z_][A-Za-z0-9_]*)"#),
        ("type", #"^\s*class\s+([A-Za-z_][A-Za-z0-9_]*)"#),
        ("func", #"^\s*(?:export\s+)?(?:default\s+)?(?:async\s+)?function\s*\*?\s*([A-Za-z_$][A-Za-z0-9_$]*)"#),
        ("func", #"^\s*(?:pub(?:\([^)]*\))?\s+)?(?:async\s+)?(?:unsafe\s+)?(?:const\s+)?fn\s+([A-Za-z_][A-Za-z0-9_]*)"#),
        ("func", #"^\s*func\s+(?:\([^)]*\)\s*)?([A-Za-z_][A-Za-z0-9_]*)"#),
        ("type", #"^\s*type\s+([A-Za-z_][A-Za-z0-9_]*)\s+(?:struct|interface)"#),
        ("func", #"^[A-Za-z_][\w:<>,\s\*&~]*?\b([A-Za-z_~][A-Za-z0-9_]*(?:::[A-Za-z_~][A-Za-z0-9_]*)*)\s*\([^;]*\)\s*(?:const\s*)?(?:noexcept\s*)?(?:override\s*)?\{?\s*$"#),
        ("const", #"^\s*(?:export\s+)?(?:const|let)\s+([A-Za-z_$][A-Za-z0-9_$]*)\s*=\s*(?:async\s*)?(?:\([^)]*\)|[A-Za-z_$][A-Za-z0-9_$]*)\s*=>"#),
    ].map { ($0.0, try! NSRegularExpression(pattern: $0.1)) }

    static let notNames: Set<String> = ["if", "for", "while", "switch", "return", "catch", "else", "sizeof", "do"]

    public static func symbols(_ text: String) -> [CodeSymbol] {
        var out: [CodeSymbol] = []
        for (index, line) in text.components(separatedBy: "\n").enumerated() {
            if line.count > 400 { continue }
            let trimmed = line.dartTrimmed
            if trimmed.hasPrefix("//") || trimmed.hasPrefix("#") && !trimmed.hasPrefix("#define") || trimmed.hasPrefix("*") { continue }
            let range = NSRange(line.startIndex..., in: line)
            for (kind, re) in patterns {
                guard let match = re.firstMatch(in: line, range: range), let name = Range(match.range(at: 1), in: line) else { continue }
                let symbol = String(line[name])
                if notNames.contains(symbol) { break }
                let indent = line.prefix { $0 == " " || $0 == "\t" }.reduce(0) { $0 + ($1 == "\t" ? 4 : 1) }
                out.append(CodeSymbol(name: symbol, kind: kind, line: index + 1, depth: min(indent / 4, 6)))
                break
            }
            if out.count >= maxSymbols { break }
        }
        return out
    }

    public static func references(to name: String, in text: String, excludingLine definition: Int? = nil) -> [CodeReference] {
        guard !name.isEmpty else { return [] }
        let short = name.split(separator: ":").last.map(String.init) ?? name
        var out: [CodeReference] = []
        var finder = FindInText()
        finder.options = FindOptions(caseSensitive: true, wholeWord: true)
        let lines = text.components(separatedBy: "\n")
        for (index, line) in lines.enumerated() where index + 1 != definition {
            finder.search(short, in: line)
            guard let match = finder.matches.first else { continue }
            let units = Array(line.utf16)
            var after = match.offset + match.length
            while after < units.count && units[after] == 0x20 { after += 1 }
            let isCall = after < units.count && units[after] == 0x28
            out.append(CodeReference(symbol: "\(definition ?? 0):\(name)", line: index + 1, text: String(line.dartTrimmed.prefix(160)), isCall: isCall))
            if out.count >= 500 { break }
        }
        return out
    }
}
