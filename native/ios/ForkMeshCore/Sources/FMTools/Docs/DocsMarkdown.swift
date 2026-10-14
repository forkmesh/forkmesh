import Foundation

public enum DocsBlock: Equatable, Sendable {
    case heading(level: Int, text: String, anchor: String)
    case paragraph(String)
    case listItem(ordered: Bool, number: Int, depth: Int, text: String)
    case quote(String)
    case code(language: String, text: String)
    case table(header: [String], rows: [[String]])
    case rule
}

public struct DocsRun: Equatable, Sendable {
    public var text: String
    public var bold: Bool
    public var italic: Bool
    public var code: Bool
    public var strike: Bool
    public var link: String?

    public init(text: String, bold: Bool = false, italic: Bool = false, code: Bool = false, strike: Bool = false, link: String? = nil) {
        self.text = text
        self.bold = bold
        self.italic = italic
        self.code = code
        self.strike = strike
        self.link = link
    }

    func sameStyle(_ other: DocsRun) -> Bool {
        bold == other.bold && italic == other.italic && code == other.code && strike == other.strike && link == other.link
    }
}

public enum DocsMarkdown {
    public static func anchor(_ heading: String) -> String {
        var out = ""
        for ch in heading.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() {
            if ch.isLetter || ch.isNumber || ch == "_" || ch == "-" {
                out.append(ch)
            } else if ch.isWhitespace {
                out.append("-")
            }
        }
        return out
    }

    public static func anchorMatching(_ fragment: String, in blocks: [DocsBlock]) -> String? {
        let anchors = blocks.compactMap { (block: DocsBlock) -> String? in
            if case let .heading(_, _, anchor) = block { return anchor }
            return nil
        }
        if anchors.contains(fragment) { return fragment }
        let lowered = fragment.lowercased()
        if anchors.contains(lowered) { return lowered }
        let normalised = anchor(fragment)
        return anchors.contains(normalised) ? normalised : nil
    }

    public static func blocks(_ markdown: String) -> [DocsBlock] {
        var parser = BlockParser(lines: markdown.replacingOccurrences(of: "\r\n", with: "\n").components(separatedBy: "\n"))
        parser.run()
        return parser.blocks
    }

    public static func plain(_ text: String) -> String {
        let joined = DocsInline.runs(text).map(\.text).joined()
        return joined.split(whereSeparator: { $0.isWhitespace }).joined(separator: " ")
    }
}

struct BlockParser {
    let lines: [String]
    var blocks: [DocsBlock] = []
    var paragraph: [String] = []
    var quote: [String] = []
    var listIndents: [Int] = []
    var anchors: [String: Int] = [:]
    var inComment = false
    var index = 0
    var lastBlank = true

    init(lines: [String]) {
        self.lines = lines
    }

    mutating func run() {
        while index < lines.count {
            let raw = lines[index]
            index += 1
            var line = raw.replacingOccurrences(of: "\t", with: "    ")
            if inComment {
                guard let end = line.range(of: "-->") else { continue }
                inComment = false
                line = String(line[end.upperBound...])
                if line.trimmingCharacters(in: .whitespaces).isEmpty { continue }
            }
            line = stripComments(line)
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            let indent = leadingSpaces(line)
            if let fence = fenceOpening(trimmed) {
                flushAll()
                readFence(fence: fence, info: String(trimmed.dropFirst(fence.count)), indent: indent)
                lastBlank = false
                continue
            }
            if trimmed.isEmpty {
                flushParagraph()
                flushQuote()
                lastBlank = true
                continue
            }
            defer { lastBlank = false }
            if trimmed.hasPrefix(">") {
                flushParagraph()
                listIndents.removeAll()
                quote.append(stripQuote(trimmed))
                continue
            }
            flushQuote()
            if let heading = atxHeading(trimmed) {
                flushParagraph()
                listIndents.removeAll()
                addHeading(level: heading.level, text: heading.text)
                continue
            }
            if !paragraph.isEmpty, listIndents.isEmpty, isSetext(trimmed) {
                let text = paragraph.joined(separator: " ")
                paragraph.removeAll()
                addHeading(level: trimmed.hasPrefix("=") ? 1 : 2, text: text)
                continue
            }
            if isRule(trimmed) {
                flushParagraph()
                listIndents.removeAll()
                blocks.append(.rule)
                continue
            }
            if trimmed.contains("|"), index < lines.count, isDelimiterRow(lines[index].trimmingCharacters(in: .whitespaces)) {
                flushParagraph()
                listIndents.removeAll()
                readTable(header: trimmed)
                continue
            }
            if let item = listItem(line) {
                flushParagraph()
                addListItem(indent: item.indent, ordered: item.ordered, number: item.number, text: item.text)
                continue
            }
            if isHTMLOnly(trimmed) {
                flushParagraph()
                continue
            }
            if !listIndents.isEmpty, case let .listItem(ordered, number, depth, text)? = blocks.last, paragraph.isEmpty {
                if !lastBlank || indent >= 2 {
                    let joiner = lastBlank ? "\n" : " "
                    blocks[blocks.count - 1] = .listItem(ordered: ordered, number: number, depth: depth, text: text + joiner + trimmed)
                    continue
                }
                listIndents.removeAll()
            }
            if indent >= 4, paragraph.isEmpty, listIndents.isEmpty, lastBlank {
                readIndentedCode(first: line)
                continue
            }
            listIndents.removeAll()
            paragraph.append(hardBreak(line) ? trimmed + "\n" : trimmed)
        }
        flushAll()
    }

    mutating func flushAll() {
        flushParagraph()
        flushQuote()
    }

    mutating func flushParagraph() {
        guard !paragraph.isEmpty else { return }
        var text = ""
        for (offset, part) in paragraph.enumerated() {
            if offset > 0 && !text.hasSuffix("\n") { text += " " }
            text += part
        }
        blocks.append(.paragraph(text.trimmingCharacters(in: .whitespacesAndNewlines)))
        paragraph.removeAll()
    }

    mutating func flushQuote() {
        guard !quote.isEmpty else { return }
        var parts: [String] = []
        var current: [String] = []
        for line in quote {
            if line.isEmpty {
                if !current.isEmpty { parts.append(current.joined(separator: " ")) }
                current.removeAll()
            } else {
                current.append(line)
            }
        }
        if !current.isEmpty { parts.append(current.joined(separator: " ")) }
        if !parts.isEmpty { blocks.append(.quote(parts.joined(separator: "\n"))) }
        quote.removeAll()
    }

    mutating func addHeading(level: Int, text: String) {
        let plain = DocsMarkdown.plain(text)
        let base = DocsMarkdown.anchor(plain)
        let seen = anchors[base] ?? 0
        anchors[base] = seen + 1
        blocks.append(.heading(level: level, text: text, anchor: seen == 0 ? base : "\(base)-\(seen)"))
    }

    mutating func addListItem(indent: Int, ordered: Bool, number: Int, text: String) {
        while let last = listIndents.last, indent < last {
            listIndents.removeLast()
        }
        if let last = listIndents.last {
            if indent > last { listIndents.append(indent) }
        } else {
            listIndents.append(indent)
        }
        blocks.append(.listItem(ordered: ordered, number: number, depth: max(0, listIndents.count - 1), text: taskMarker(text)))
    }

    func taskMarker(_ text: String) -> String {
        if text.hasPrefix("[ ] ") { return "\u{2610} " + text.dropFirst(4) }
        if text.hasPrefix("[x] ") || text.hasPrefix("[X] ") { return "\u{2611} " + text.dropFirst(4) }
        return text
    }

    mutating func readFence(fence: String, info: String, indent: Int) {
        let marker = fence.first ?? "`"
        var body: [String] = []
        while index < lines.count {
            let line = lines[index]
            index += 1
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if trimmed.hasPrefix(fence), trimmed.allSatisfy({ $0 == marker }) {
                break
            }
            body.append(dropIndent(line, upTo: indent))
        }
        let language = info.trimmingCharacters(in: .whitespaces).split(separator: " ").first.map(String.init) ?? ""
        blocks.append(.code(language: language, text: body.joined(separator: "\n")))
    }

    mutating func readIndentedCode(first: String) {
        var body: [String] = [dropIndent(first, upTo: 4)]
        while index < lines.count {
            let line = lines[index].replacingOccurrences(of: "\t", with: "    ")
            if line.trimmingCharacters(in: .whitespaces).isEmpty {
                var ahead = index + 1
                while ahead < lines.count, lines[ahead].trimmingCharacters(in: .whitespaces).isEmpty { ahead += 1 }
                guard ahead < lines.count, leadingSpaces(lines[ahead]) >= 4 else { break }
                body.append("")
                index += 1
                continue
            }
            guard leadingSpaces(line) >= 4 else { break }
            body.append(dropIndent(line, upTo: 4))
            index += 1
        }
        while body.last?.isEmpty == true { body.removeLast() }
        blocks.append(.code(language: "", text: body.joined(separator: "\n")))
    }

    mutating func readTable(header: String) {
        let head = cells(header)
        index += 1
        var rows: [[String]] = []
        while index < lines.count {
            let trimmed = stripComments(lines[index]).trimmingCharacters(in: .whitespaces)
            guard !trimmed.isEmpty, trimmed.contains("|") else { break }
            var row = cells(trimmed)
            if row.count < head.count { row += Array(repeating: "", count: head.count - row.count) }
            if row.count > head.count { row = Array(row.prefix(head.count)) }
            rows.append(row)
            index += 1
        }
        blocks.append(.table(header: head, rows: rows))
    }

    func cells(_ row: String) -> [String] {
        var text = row
        if text.hasPrefix("|") { text.removeFirst() }
        if text.hasSuffix("|") && !text.hasSuffix("\\|") { text.removeLast() }
        var out: [String] = []
        var current = ""
        var escaped = false
        var inCode = false
        for ch in text {
            if escaped {
                if ch != "|" { current.append("\\") }
                current.append(ch)
                escaped = false
                continue
            }
            if ch == "\\" {
                escaped = true
                continue
            }
            if ch == "`" { inCode.toggle() }
            if ch == "|" && !inCode {
                out.append(current.trimmingCharacters(in: .whitespaces))
                current = ""
                continue
            }
            current.append(ch)
        }
        if escaped { current.append("\\") }
        out.append(current.trimmingCharacters(in: .whitespaces))
        return out
    }

    func isDelimiterRow(_ trimmed: String) -> Bool {
        guard trimmed.contains("-") else { return false }
        let parts = cells(trimmed)
        guard !parts.isEmpty else { return false }
        for part in parts {
            var body = Substring(part)
            if body.hasPrefix(":") { body = body.dropFirst() }
            if body.hasSuffix(":") { body = body.dropLast() }
            if body.isEmpty || !body.allSatisfy({ $0 == "-" }) { return false }
        }
        return trimmed.contains("|") || parts.count > 1
    }

    func fenceOpening(_ trimmed: String) -> String? {
        for marker: Character in ["`", "~"] {
            let run = trimmed.prefix { $0 == marker }
            if run.count >= 3 {
                if marker == "`" && trimmed.dropFirst(run.count).contains("`") { return nil }
                return String(run)
            }
        }
        return nil
    }

    func atxHeading(_ trimmed: String) -> (level: Int, text: String)? {
        let hashes = trimmed.prefix { $0 == "#" }
        guard (1...6).contains(hashes.count) else { return nil }
        let rest = trimmed.dropFirst(hashes.count)
        guard rest.isEmpty || rest.first == " " else { return nil }
        var text = rest.trimmingCharacters(in: .whitespaces)
        while text.hasSuffix("#") { text.removeLast() }
        return (hashes.count, text.trimmingCharacters(in: .whitespaces))
    }

    func isSetext(_ trimmed: String) -> Bool {
        guard let first = trimmed.first, first == "=" || first == "-" else { return false }
        return trimmed.allSatisfy { $0 == first }
    }

    func isRule(_ trimmed: String) -> Bool {
        let compact = trimmed.filter { $0 != " " }
        guard compact.count >= 3, let first = compact.first, first == "-" || first == "*" || first == "_" else { return false }
        return compact.allSatisfy { $0 == first }
    }

    func listItem(_ line: String) -> (indent: Int, ordered: Bool, number: Int, text: String)? {
        let indent = leadingSpaces(line)
        let body = line.drop { $0 == " " }
        guard let first = body.first else { return nil }
        if first == "-" || first == "*" || first == "+" {
            let rest = body.dropFirst()
            guard rest.first == " " else { return nil }
            return (indent, false, 0, rest.trimmingCharacters(in: .whitespaces))
        }
        let digits = body.prefix { $0.isASCII && $0.isNumber }
        guard (1...9).contains(digits.count) else { return nil }
        let after = body.dropFirst(digits.count)
        guard let mark = after.first, mark == "." || mark == ")" else { return nil }
        let rest = after.dropFirst()
        guard rest.first == " " else { return nil }
        return (indent, true, Int(digits) ?? 1, rest.trimmingCharacters(in: .whitespaces))
    }

    func isHTMLOnly(_ trimmed: String) -> Bool {
        guard trimmed.hasPrefix("<"), !trimmed.hasPrefix("<http"), !trimmed.hasPrefix("<mailto:") else { return false }
        return DocsInline.stripTags(trimmed).trimmingCharacters(in: .whitespaces).isEmpty
    }

    func stripQuote(_ trimmed: String) -> String {
        var text = Substring(trimmed)
        while text.first == ">" {
            text = text.dropFirst()
            if text.first == " " { text = text.dropFirst() }
        }
        return text.trimmingCharacters(in: .whitespaces)
    }

    mutating func stripComments(_ line: String) -> String {
        var text = line
        while let start = text.range(of: "<!--") {
            if let end = text.range(of: "-->", range: start.upperBound..<text.endIndex) {
                text.removeSubrange(start.lowerBound..<end.upperBound)
            } else {
                text.removeSubrange(start.lowerBound..<text.endIndex)
                inComment = true
                break
            }
        }
        return text
    }

    func hardBreak(_ line: String) -> Bool {
        line.hasSuffix("  ") || line.hasSuffix("\\")
    }

    func leadingSpaces(_ line: String) -> Int {
        var count = 0
        for ch in line {
            if ch == " " { count += 1 } else if ch == "\t" { count += 4 } else { break }
        }
        return count
    }

    func dropIndent(_ line: String, upTo count: Int) -> String {
        var dropped = 0
        var rest = Substring(line)
        while dropped < count, rest.first == " " {
            rest = rest.dropFirst()
            dropped += 1
        }
        return String(rest)
    }
}

public enum DocsInline {
    static let entities: [String: String] = [
        "amp": "&", "lt": "<", "gt": ">", "quot": "\"", "apos": "'", "nbsp": "\u{00A0}", "mdash": "\u{2014}",
        "ndash": "\u{2013}", "hellip": "\u{2026}", "middot": "\u{00B7}", "rarr": "\u{2192}", "larr": "\u{2190}",
        "copy": "\u{00A9}", "times": "\u{00D7}",
    ]

    public static func runs(_ text: String) -> [DocsRun] {
        var scanner = InlineScanner(chars: Array(text))
        scanner.scan(until: nil)
        return merged(scanner.out)
    }

    static func merged(_ runs: [DocsRun]) -> [DocsRun] {
        var out: [DocsRun] = []
        for run in runs where !run.text.isEmpty {
            if let last = out.last, last.sameStyle(run) {
                out[out.count - 1].text += run.text
            } else {
                out.append(run)
            }
        }
        return out
    }

    public static func stripTags(_ text: String) -> String {
        var out = ""
        let chars = Array(text)
        var i = 0
        while i < chars.count {
            if chars[i] == "<", let end = tagEnd(chars, from: i) {
                i = end + 1
                continue
            }
            out.append(chars[i])
            i += 1
        }
        return out
    }

    static func tagEnd(_ chars: [Character], from start: Int) -> Int? {
        var i = start + 1
        if i < chars.count, chars[i] == "/" { i += 1 }
        guard i < chars.count, chars[i].isASCII, chars[i].isLetter else { return nil }
        while i < chars.count {
            if chars[i] == ">" { return i }
            if chars[i] == "<" || chars[i] == "\n" { return nil }
            i += 1
        }
        return nil
    }
}

struct InlineScanner {
    let chars: [Character]
    var i = 0
    var out: [DocsRun] = []
    var bold = false
    var italic = false
    var strike = false
    var link: String?

    init(chars: [Character]) {
        self.chars = chars
    }

    func run(_ text: String, code: Bool = false) -> DocsRun {
        DocsRun(text: text, bold: bold, italic: italic, code: code, strike: strike, link: link)
    }

    mutating func emit(_ text: String) {
        out.append(run(text))
    }

    func at(_ offset: Int) -> Character? {
        let p = i + offset
        return p >= 0 && p < chars.count ? chars[p] : nil
    }

    func starts(_ token: String, at position: Int) -> Bool {
        var p = position
        for ch in token {
            guard p < chars.count, chars[p] == ch else { return false }
            p += 1
        }
        return true
    }

    func find(_ token: String, from position: Int, end: Int) -> Int? {
        var p = position
        while p < end {
            if chars[p] == "\\" {
                p += 2
                continue
            }
            if starts(token, at: p) { return p }
            p += 1
        }
        return nil
    }

    func closer(_ token: String, from position: Int, end: Int) -> Bool {
        var p = position
        while let found = find(token, from: p, end: end) {
            if found > position, !chars[found - 1].isWhitespace { return true }
            p = found + 1
        }
        return false
    }

    mutating func scan(until end: Int?) {
        let limit = end ?? chars.count
        while i < limit {
            let ch = chars[i]
            if ch == "\\", i + 1 < limit, let next = at(1), next.isPunctuation || next.isSymbol {
                emit(String(next))
                i += 2
                continue
            }
            if ch == "`" && codeSpan(limit: limit) { continue }
            if ch == "!" && at(1) == "[" && image(limit: limit) { continue }
            if ch == "[" && bracketLink(limit: limit) { continue }
            if ch == "<" && angle(limit: limit) { continue }
            if ch == "&" && entity(limit: limit) { continue }
            if (ch == "h" || ch == "H") && link == nil && bareURL(limit: limit) { continue }
            if (ch == "*" || ch == "_" || ch == "~") && emphasis(limit: limit) { continue }
            emit(String(ch))
            i += 1
        }
    }

    mutating func codeSpan(limit: Int) -> Bool {
        var n = 0
        while i + n < limit, chars[i + n] == "`" { n += 1 }
        var p = i + n
        while p < limit {
            if chars[p] == "`" {
                var m = 0
                while p + m < limit, chars[p + m] == "`" { m += 1 }
                if m == n {
                    var body = String(chars[(i + n)..<p])
                    if body.count >= 2, body.hasPrefix(" "), body.hasSuffix(" "), body.contains(where: { $0 != " " }) {
                        body = String(body.dropFirst().dropLast())
                    }
                    out.append(run(body.replacingOccurrences(of: "\n", with: " "), code: true))
                    i = p + m
                    return true
                }
                p += m
            } else {
                p += 1
            }
        }
        emit(String(repeating: "`", count: n))
        i += n
        return true
    }

    func closingBracket(from open: Int, limit: Int) -> Int? {
        var depth = 0
        var p = open
        while p < limit {
            let c = chars[p]
            if c == "\\" {
                p += 2
                continue
            }
            if c == "`" {
                var n = 0
                while p + n < limit, chars[p + n] == "`" { n += 1 }
                var q = p + n
                var closed = false
                while q < limit {
                    if chars[q] == "`" {
                        var m = 0
                        while q + m < limit, chars[q + m] == "`" { m += 1 }
                        if m == n {
                            closed = true
                            q += m
                            break
                        }
                        q += m
                    } else {
                        q += 1
                    }
                }
                p = closed ? q : p + n
                continue
            }
            if c == "[" { depth += 1 }
            if c == "]" {
                depth -= 1
                if depth == 0 { return p }
            }
            p += 1
        }
        return nil
    }

    func destination(from open: Int, limit: Int) -> (href: String, end: Int)? {
        guard open < limit, chars[open] == "(" else { return nil }
        var p = open + 1
        while p < limit, chars[p] == " " { p += 1 }
        var href = ""
        if p < limit, chars[p] == "<" {
            p += 1
            while p < limit, chars[p] != ">" {
                href.append(chars[p])
                p += 1
            }
            guard p < limit else { return nil }
            p += 1
        } else {
            var depth = 0
            while p < limit {
                let c = chars[p]
                if c == "\\", p + 1 < limit {
                    href.append(chars[p + 1])
                    p += 2
                    continue
                }
                if c == " " { break }
                if c == "(" { depth += 1 }
                if c == ")" {
                    if depth == 0 { break }
                    depth -= 1
                }
                href.append(c)
                p += 1
            }
        }
        while p < limit, chars[p] != ")" {
            if chars[p] == "\n" { return nil }
            p += 1
        }
        guard p < limit else { return nil }
        return (href, p)
    }

    mutating func image(limit: Int) -> Bool {
        guard let close = closingBracket(from: i + 1, limit: limit) else { return false }
        let alt = String(chars[(i + 2)..<close])
        if let target = destination(from: close + 1, limit: limit) {
            emit(DocsMarkdown.plain(alt))
            i = target.end + 1
            return true
        }
        return false
    }

    mutating func bracketLink(limit: Int) -> Bool {
        guard let close = closingBracket(from: i, limit: limit) else { return false }
        if let target = destination(from: close + 1, limit: limit) {
            let saved = link
            if link == nil { link = target.href }
            let resume = target.end + 1
            i += 1
            scan(until: close)
            link = saved
            i = resume
            return true
        }
        if close + 1 < limit, chars[close + 1] == "[", let refEnd = closingBracket(from: close + 1, limit: limit) {
            i += 1
            scan(until: close)
            i = refEnd + 1
            return true
        }
        return false
    }

    mutating func angle(limit: Int) -> Bool {
        var p = i + 1
        var body = ""
        while p < limit, chars[p] != ">", chars[p] != " ", chars[p] != "<" {
            body.append(chars[p])
            p += 1
        }
        if p < limit, chars[p] == ">" {
            let lowered = body.lowercased()
            if lowered.hasPrefix("http://") || lowered.hasPrefix("https://") || lowered.hasPrefix("mailto:") {
                let saved = link
                if link == nil { link = body }
                emit(lowered.hasPrefix("mailto:") ? String(body.dropFirst(7)) : body)
                link = saved
                i = p + 1
                return true
            }
        }
        guard let end = DocsInline.tagEnd(chars, from: i), end < limit else { return false }
        let tag = String(chars[(i + 1)..<end]).lowercased()
        if tag.hasPrefix("br") { emit("\n") }
        i = end + 1
        return true
    }

    mutating func entity(limit: Int) -> Bool {
        var p = i + 1
        var name = ""
        while p < limit, p - i <= 10, chars[p] != ";" {
            name.append(chars[p])
            p += 1
        }
        guard p < limit, chars[p] == ";", !name.isEmpty else { return false }
        var value: String?
        if name.hasPrefix("#x") || name.hasPrefix("#X") {
            value = UInt32(name.dropFirst(2), radix: 16).flatMap(Unicode.Scalar.init).map { String(Character($0)) }
        } else if name.hasPrefix("#") {
            value = UInt32(name.dropFirst()).flatMap(Unicode.Scalar.init).map { String(Character($0)) }
        } else {
            value = DocsInline.entities[name]
        }
        guard let value else { return false }
        emit(value)
        i = p + 1
        return true
    }

    mutating func bareURL(limit: Int) -> Bool {
        if let previous = at(-1), previous.isLetter || previous.isNumber || previous == "/" {
            return false
        }
        guard starts("https://", at: i) || starts("http://", at: i) else { return false }
        var p = i
        while p < limit, !chars[p].isWhitespace, chars[p] != "<", chars[p] != ">", chars[p] != "`" { p += 1 }
        while p > i, ".,;:!?*_'\"".contains(chars[p - 1]) { p -= 1 }
        if p > i, chars[p - 1] == ")" {
            let body = chars[i..<p]
            if body.filter({ $0 == "(" }).count < body.filter({ $0 == ")" }).count { p -= 1 }
        }
        let url = String(chars[i..<p])
        guard url.count > 8 else { return false }
        link = url
        emit(url)
        link = nil
        i = p
        return true
    }

    mutating func emphasis(limit: Int) -> Bool {
        let ch = chars[i]
        let double = at(1) == ch
        if ch == "~" {
            guard double else { return false }
            if strike {
                strike = false
                i += 2
                return true
            }
            guard closer("~~", from: i + 2, end: limit), let next = at(2), !next.isWhitespace else { return false }
            strike = true
            i += 2
            return true
        }
        let token = double ? String([ch, ch]) : String(ch)
        let width = token.count
        let active = double ? bold : italic
        let before = at(-1)
        let after = at(width)
        if ch == "_" {
            let wordBefore = before.map { $0.isLetter || $0.isNumber } ?? false
            let wordAfter = after.map { $0.isLetter || $0.isNumber } ?? false
            if active ? wordAfter : wordBefore { return false }
        }
        if active {
            guard let before, !before.isWhitespace else { return false }
            if double { bold = false } else { italic = false }
            i += width
            return true
        }
        guard let after, !after.isWhitespace else { return false }
        guard closer(token, from: i + width, end: limit) else { return false }
        if double { bold = true } else { italic = true }
        i += width
        return true
    }
}
