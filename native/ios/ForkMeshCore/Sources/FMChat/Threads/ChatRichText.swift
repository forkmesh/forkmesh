import FMBase
import Foundation

public struct ChatRichSpan: Equatable, Sendable {
    public var text: String
    public var bold = false
    public var italic = false
    public var strike = false
    public var code = false

    public init(text: String, bold: Bool = false, italic: Bool = false, strike: Bool = false, code: Bool = false) {
        self.text = text
        self.bold = bold
        self.italic = italic
        self.strike = strike
        self.code = code
    }

    public var plain: Bool { !bold && !italic && !strike && !code }
}

public struct ChatTextBlock: Equatable, Sendable {
    public let quote: Bool
    public let text: String
}

public enum ChatTextFormat: String, CaseIterable, Sendable {
    case bold, italic, strike, code, codeBlock, quote, bullets

    public var title: String {
        switch self {
        case .bold: return "Bold"
        case .italic: return "Italic"
        case .strike: return "Strikethrough"
        case .code: return "Code"
        case .codeBlock: return "Code block"
        case .quote: return "Quote"
        case .bullets: return "Bulleted list"
        }
    }

    public var symbol: String {
        switch self {
        case .bold: return "bold"
        case .italic: return "italic"
        case .strike: return "strikethrough"
        case .code: return "chevron.left.forwardslash.chevron.right"
        case .codeBlock: return "curlybraces.square"
        case .quote: return "text.quote"
        case .bullets: return "list.bullet"
        }
    }

    var marker: String {
        switch self {
        case .bold: return "*"
        case .italic: return "_"
        case .strike: return "~"
        case .code: return "`"
        case .codeBlock: return "```"
        case .quote: return "> "
        case .bullets: return "\u{2022} "
        }
    }
}

public enum ChatRichText {
    static let markers: Set<Character> = ["*", "_", "~", "`"]
    static let openers: Set<Character> = ["(", "[", "{", "\"", "'", "\u{201C}", "\u{2018}", "-"]
    static let closers: Set<Character> = [".", ",", ";", ":", "!", "?", ")", "]", "}", "\"", "'", "\u{201D}", "\u{2019}", "-"]

    public static func spans(_ text: String) -> [ChatRichSpan] {
        var out: [ChatRichSpan] = []
        scan(Array(text), base: ChatRichSpan(text: ""), into: &out)
        return out
    }

    public static func hasFormatting(_ text: String) -> Bool {
        spans(text).contains { !$0.plain }
    }

    static func scan(_ chars: [Character], base: ChatRichSpan, into out: inout [ChatRichSpan]) {
        var literal = ""
        var index = 0
        func flush() {
            if literal.isEmpty { return }
            var span = base
            span.text = literal
            append(span, to: &out)
            literal = ""
        }
        while index < chars.count {
            let character = chars[index]
            if markers.contains(character), opens(chars, at: index) {
                let width = character == "*" && index + 1 < chars.count && chars[index + 1] == "*" ? 2 : 1
                if let close = closing(chars, from: index, marker: character, width: width) {
                    flush()
                    let inner = Array(chars[(index + width)..<close])
                    var styled = base
                    switch character {
                    case "*": styled.bold = true
                    case "_": styled.italic = true
                    case "~": styled.strike = true
                    default: styled.code = true
                    }
                    if styled.code {
                        styled.text = String(inner)
                        append(styled, to: &out)
                    } else {
                        scan(inner, base: styled, into: &out)
                    }
                    index = close + width
                    continue
                }
            }
            literal.append(character)
            index += 1
        }
        flush()
    }

    static func append(_ span: ChatRichSpan, to out: inout [ChatRichSpan]) {
        if span.text.isEmpty { return }
        if var last = out.last, last.bold == span.bold, last.italic == span.italic, last.strike == span.strike,
            last.code == span.code
        {
            last.text += span.text
            out[out.count - 1] = last
            return
        }
        out.append(span)
    }

    static func opens(_ chars: [Character], at index: Int) -> Bool {
        if index > 0 {
            let previous = chars[index - 1]
            if !previous.isWhitespace && !openers.contains(previous) && !markers.contains(previous) { return false }
        }
        return true
    }

    static func closing(_ chars: [Character], from start: Int, marker: Character, width: Int) -> Int? {
        let first = start + width
        guard first < chars.count, !chars[first].isWhitespace, chars[first] != marker else { return nil }
        var index = first + 1
        while index + width - 1 < chars.count {
            let character = chars[index]
            if character == "\n" { return nil }
            if character == marker && (width == 1 || chars[index + 1] == marker) {
                let after = index + width
                let previous = chars[index - 1]
                let boundary = after >= chars.count || chars[after].isWhitespace || closers.contains(chars[after])
                    || markers.contains(chars[after])
                if !previous.isWhitespace && boundary { return index }
            }
            index += 1
        }
        return nil
    }

    public static func blocks(_ text: String) -> [ChatTextBlock] {
        var out: [ChatTextBlock] = []
        var lines: [String] = []
        var quoting = false
        func flush() {
            if lines.isEmpty { return }
            out.append(ChatTextBlock(quote: quoting, text: lines.joined(separator: "\n")))
            lines = []
        }
        for raw in text.components(separatedBy: "\n") {
            let quoted = raw.hasPrefix(">")
            let line = quoted ? String(raw.dropFirst(raw.hasPrefix("> ") ? 2 : 1)) : raw
            if quoted != quoting {
                flush()
                quoting = quoted
            }
            lines.append(line)
        }
        flush()
        return out.filter { !$0.text.dartTrimmed.isEmpty }
    }

    public static func apply(_ format: ChatTextFormat, to draft: String) -> String {
        let body = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        if body.isEmpty { return draft }
        switch format {
        case .bold, .italic, .strike, .code:
            let marker = format.marker
            if body.count > marker.count * 2, body.hasPrefix(marker), body.hasSuffix(marker) {
                return String(body.dropFirst(marker.count).dropLast(marker.count))
            }
            return marker + body + marker
        case .codeBlock:
            if body.hasPrefix("```"), body.hasSuffix("```"), body.count > 6 {
                return String(body.dropFirst(3).dropLast(3)).trimmingCharacters(in: .newlines)
            }
            return "```\n" + body + "\n```"
        case .quote, .bullets:
            let marker = format.marker
            let lines = body.components(separatedBy: "\n")
            if lines.allSatisfy({ $0.hasPrefix(marker) }) {
                return lines.map { String($0.dropFirst(marker.count)) }.joined(separator: "\n")
            }
            return lines.map { $0.hasPrefix(marker) ? $0 : marker + $0 }.joined(separator: "\n")
        }
    }

    public static func forwardText(comment: String, sender: String, place: String, text: String, fileName: String = "")
        -> String
    {
        var quoted = text.dartTrimmed
        if quoted.isEmpty { quoted = fileName.isEmpty ? "(no text)" : "\u{1F4CE} \(fileName)" }
        let body = quoted.components(separatedBy: "\n").map { "> " + $0 }.joined(separator: "\n")
        let origin = place.isEmpty ? sender : "\(sender) in \(place)"
        let lead = comment.dartTrimmed
        let shared = "\(body)\n> \u{2014} \(origin)"
        return String((lead.isEmpty ? shared : "\(lead)\n\(shared)").prefix(ChatLimits.maxTextChars))
    }
}

public struct ChatEmojiShortcode: Equatable, Sendable {
    public let name: String
    public let emoji: String
}

public enum ChatEmojiShortcodes {
    static let table: [(String, String)] = [
        ("smile", "\u{1F604}"), ("grinning", "\u{1F600}"), ("joy", "\u{1F602}"), ("rofl", "\u{1F923}"),
        ("slightly_smiling_face", "\u{1F642}"), ("blush", "\u{1F60A}"), ("heart_eyes", "\u{1F60D}"),
        ("sunglasses", "\u{1F60E}"), ("wink", "\u{1F609}"), ("thinking_face", "\u{1F914}"), ("sweat_smile", "\u{1F605}"),
        ("upside_down_face", "\u{1F643}"), ("cry", "\u{1F622}"), ("sob", "\u{1F62D}"), ("rage", "\u{1F621}"),
        ("scream", "\u{1F631}"), ("sleeping", "\u{1F634}"), ("exploding_head", "\u{1F92F}"), ("partying_face", "\u{1F973}"),
        ("face_palm", "\u{1F926}"), ("shrug", "\u{1F937}"), ("grimacing", "\u{1F62C}"), ("neutral_face", "\u{1F610}"),
        ("thumbsup", "\u{1F44D}"), ("+1", "\u{1F44D}"), ("thumbsdown", "\u{1F44E}"), ("-1", "\u{1F44E}"),
        ("clap", "\u{1F44F}"), ("pray", "\u{1F64F}"), ("muscle", "\u{1F4AA}"), ("raised_hands", "\u{1F64C}"),
        ("wave", "\u{1F44B}"), ("handshake", "\u{1F91D}"), ("ok_hand", "\u{1F44C}"), ("point_up", "\u{261D}\u{FE0F}"),
        ("eyes", "\u{1F440}"), ("brain", "\u{1F9E0}"), ("heart", "\u{2764}\u{FE0F}"), ("broken_heart", "\u{1F494}"),
        ("fire", "\u{1F525}"), ("sparkles", "\u{2728}"), ("tada", "\u{1F389}"), ("100", "\u{1F4AF}"),
        ("star", "\u{2B50}"), ("zap", "\u{26A1}"), ("boom", "\u{1F4A5}"), ("white_check_mark", "\u{2705}"),
        ("heavy_check_mark", "\u{2714}\u{FE0F}"), ("x", "\u{274C}"), ("warning", "\u{26A0}\u{FE0F}"),
        ("question", "\u{2753}"), ("exclamation", "\u{2757}"), ("rocket", "\u{1F680}"), ("bug", "\u{1F41B}"),
        ("bulb", "\u{1F4A1}"), ("pushpin", "\u{1F4CC}"), ("memo", "\u{1F4DD}"), ("calendar", "\u{1F4C5}"),
        ("lock", "\u{1F512}"), ("key", "\u{1F511}"), ("link", "\u{1F517}"), ("hourglass", "\u{231B}"),
        ("coffee", "\u{2615}"), ("pizza", "\u{1F355}"), ("beer", "\u{1F37A}"), ("cake", "\u{1F382}"),
        ("rainbow", "\u{1F308}"), ("sunny", "\u{2600}\u{FE0F}"), ("snowflake", "\u{2744}\u{FE0F}"),
        ("computer", "\u{1F4BB}"), ("iphone", "\u{1F4F1}"), ("wrench", "\u{1F527}"), ("hammer", "\u{1F528}"),
        ("gear", "\u{2699}\u{FE0F}"), ("chart_with_upwards_trend", "\u{1F4C8}"), ("moneybag", "\u{1F4B0}"),
        ("trophy", "\u{1F3C6}"), ("dart", "\u{1F3AF}"), ("ship", "\u{1F6A2}"), ("robot_face", "\u{1F916}"),
        ("speech_balloon", "\u{1F4AC}"), ("bell", "\u{1F514}"), ("no_bell", "\u{1F515}"), ("see_no_evil", "\u{1F648}"),
        ("thinking", "\u{1F914}"), ("party", "\u{1F973}"), ("ok", "\u{1F197}"), ("new", "\u{1F195}"),
    ]

    public static let all: [ChatEmojiShortcode] = table.map { ChatEmojiShortcode(name: $0.0, emoji: $0.1) }

    public static func emoji(named name: String) -> String? {
        let key = name.lowercased()
        return all.first { $0.name == key }?.emoji
    }

    public static func candidates(_ query: String, limit: Int = 8) -> [ChatEmojiShortcode] {
        let key = query.lowercased()
        if key.isEmpty { return [] }
        var seen = Set<String>()
        let prefixed = all.filter { $0.name.hasPrefix(key) }
        let inside = all.filter { !$0.name.hasPrefix(key) && $0.name.contains(key) }
        var out: [ChatEmojiShortcode] = []
        for entry in prefixed + inside where !seen.contains(entry.emoji) {
            seen.insert(entry.emoji)
            out.append(entry)
            if out.count >= limit { break }
        }
        return out
    }

    public static func expand(_ text: String) -> String {
        guard text.contains(":") else { return text }
        var out = ""
        var rest = Substring(text)
        while let open = rest.firstIndex(of: ":") {
            out += rest[rest.startIndex..<open]
            let after = rest.index(after: open)
            if let close = rest[after...].firstIndex(of: ":") {
                let name = rest[after..<close]
                if !name.isEmpty, !name.contains(where: { $0.isWhitespace }), let emoji = emoji(named: String(name)) {
                    out += emoji
                    rest = rest[rest.index(after: close)...]
                    continue
                }
            }
            out.append(":")
            rest = rest[after...]
        }
        out += rest
        return out
    }
}
