import Foundation

public struct TranscriptTool: Equatable, Sendable {
    public let name: String
    public let detail: String

    public init(_ name: String, _ detail: String) {
        self.name = name
        self.detail = detail
    }
}

public enum TranscriptAlign: Equatable, Sendable {
    case start
    case center
    case end
}

public enum TranscriptTaskTone: Equatable, Sendable {
    case done
    case failed
    case running
}

public struct TranscriptTable: Equatable, Sendable {
    public let header: [String]
    public let rows: [[String]]
    public let align: [TranscriptAlign]

    public init(header: [String], rows: [[String]], align: [TranscriptAlign]) {
        self.header = header
        self.rows = rows
        self.align = align
    }
}

public struct TranscriptTask: Equatable, Sendable {
    public let status: String
    public let summary: String
    public let body: String

    public init(status: String, summary: String, body: String) {
        self.status = status
        self.summary = summary
        self.body = body
    }

    public var tone: TranscriptTaskTone {
        switch status {
        case "completed", "done", "success", "succeeded": return .done
        case "failed", "killed", "error", "cancelled", "canceled", "stopped", "timeout": return .failed
        default: return .running
        }
    }
}

public enum TranscriptBlock: Equatable, Sendable {
    case status([String])
    case error(String)
    case prompt(String)
    case steer(from: String, text: String)
    case tools([TranscriptTool])
    case heading(level: Int, text: String)
    case bullet(marker: String, depth: Int, text: String)
    case code(language: String, text: String)
    case json(String)
    case table(TranscriptTable)
    case task(TranscriptTask)
    case rule
    case text(String)
}

public enum TranscriptInlineStyle: Equatable, Sendable {
    case plain
    case bold
    case italic
    case code
    case link
}

public struct TranscriptInline: Equatable, Sendable {
    public let text: String
    public let style: TranscriptInlineStyle
    public let url: String

    public init(_ text: String, _ style: TranscriptInlineStyle = .plain, url: String = "") {
        self.text = text
        self.style = style
        self.url = url
    }
}

public extension TranscriptBlock {
    static func steerFromYou(_ from: String) -> Bool { from.hasPrefix("ForkMesh") }
}

public struct TranscriptBlockCache: Sendable {
    private var parser = AgentTranscriptFormat.Parser()
    private var consumed = ""

    public init() {}

    public mutating func update(_ text: String) -> [TranscriptBlock] {
        if !text.utf8.starts(with: consumed.utf8) {
            parser = AgentTranscriptFormat.Parser()
            consumed = ""
        }
        let utf8 = Array(text.utf8)
        var end = utf8.count
        while end > 0, utf8[end - 1] != UInt8(ascii: "\n") { end -= 1 }
        let done = consumed.utf8.count
        if end > done {
            let fresh = String(decoding: utf8[done..<(end - 1)], as: UTF8.self)
            for raw in fresh.split(separator: "\n", omittingEmptySubsequences: false) {
                parser.feed(raw.hasSuffix("\r") ? String(raw.dropLast()) : String(raw))
            }
            consumed = String(decoding: utf8[0..<end], as: UTF8.self)
        }
        var tail = parser
        if end < utf8.count {
            let rest = String(decoding: utf8[end...], as: UTF8.self)
            tail.feed(rest.hasSuffix("\r") ? String(rest.dropLast()) : rest)
        }
        tail.finish()
        return tail.out
    }
}

public enum TranscriptJson {
    public static func readable(_ text: String) -> String? {
        guard pretty(text) != nil, let data = text.data(using: .utf8),
            let value = try? JSONSerialization.jsonObject(with: data) else { return nil }
        func lines(_ value: Any, _ depth: Int) -> [String] {
            let indent = String(repeating: "  ", count: depth)
            if let fields = value as? [String: Any] {
                if fields.isEmpty { return [indent + "(empty)"] }
                return fields.keys.sorted().flatMap { key -> [String] in
                    let label = key.replacingOccurrences(of: "_", with: " ")
                    return [indent + label + ":"] + lines(fields[key]!, depth + 1)
                }
            }
            if let items = value as? [Any] {
                if items.isEmpty { return [indent + "(empty)"] }
                return items.enumerated().flatMap { index, item in
                    [indent + "Item \(index + 1):"] + lines(item, depth + 1)
                }
            }
            if value is NSNull { return [indent + "null"] }
            let text = (value as? String) ?? String(describing: value)
            return text.components(separatedBy: "\n").map { indent + $0 }
        }
        return lines(value, 0).joined(separator: "\n")
    }

    public static func pretty(_ text: String) -> String? {
        let source = Array(text.trimmingCharacters(in: .whitespacesAndNewlines).unicodeScalars)
        guard source.count >= 2, source.count <= 200_000, source[0] == "{" || source[0] == "[" else { return nil }
        var reader = Reader(s: source)
        var out = ""
        do {
            try reader.value(&out, 0, 0)
            reader.space()
            return reader.index == source.count ? out : nil
        } catch {
            return nil
        }
    }

    private struct Bad: Error {}

    private struct Reader {
        let s: [Unicode.Scalar]
        var index = 0

        mutating func space() {
            while index < s.count, s[index] == " " || s[index] == "\t" || s[index] == "\r" || s[index] == "\n" { index += 1 }
        }

        func indent(_ out: inout String, _ level: Int) {
            out += String(repeating: "  ", count: level)
        }

        mutating func value(_ out: inout String, _ level: Int, _ depth: Int) throws {
            if depth > 64 { throw Bad() }
            space()
            guard index < s.count else { throw Bad() }
            switch s[index] {
            case "{": try container(&out, level, depth, "}", true)
            case "[": try container(&out, level, depth, "]", false)
            case "\"": try string(&out)
            case "t": try literal(&out, "true")
            case "f": try literal(&out, "false")
            case "n": try literal(&out, "null")
            default: try number(&out)
            }
        }

        mutating func container(_ out: inout String, _ level: Int, _ depth: Int, _ close: Unicode.Scalar, _ keyed: Bool) throws {
            index += 1
            space()
            if index < s.count, s[index] == close {
                index += 1
                out += keyed ? "{}" : "[]"
                return
            }
            out += keyed ? "{\n" : "[\n"
            while true {
                indent(&out, level + 1)
                if keyed {
                    space()
                    guard index < s.count, s[index] == "\"" else { throw Bad() }
                    try string(&out)
                    space()
                    guard index < s.count, s[index] == ":" else { throw Bad() }
                    index += 1
                    out += ": "
                }
                try value(&out, level + 1, depth + 1)
                space()
                guard index < s.count else { throw Bad() }
                if s[index] == "," {
                    index += 1
                    out += ",\n"
                } else if s[index] == close {
                    index += 1
                    out += "\n"
                    indent(&out, level)
                    out.unicodeScalars.append(close)
                    return
                } else {
                    throw Bad()
                }
            }
        }

        mutating func string(_ out: inout String) throws {
            let start = index
            index += 1
            while index < s.count {
                let c = s[index]
                if c == "\\" {
                    index += 2
                    continue
                }
                if c == "\n" { throw Bad() }
                index += 1
                if c == "\"" {
                    out.unicodeScalars.append(contentsOf: s[start..<index])
                    return
                }
            }
            throw Bad()
        }

        mutating func literal(_ out: inout String, _ word: String) throws {
            let scalars = Array(word.unicodeScalars)
            guard index + scalars.count <= s.count, Array(s[index..<(index + scalars.count)]) == scalars else { throw Bad() }
            index += scalars.count
            out += word
        }

        mutating func number(_ out: inout String) throws {
            let start = index
            while index < s.count, ("0"..."9").contains(s[index]) || "+-.eE".unicodeScalars.contains(s[index]) { index += 1 }
            var text = ""
            text.unicodeScalars.append(contentsOf: s[start..<index])
            guard TranscriptJson.isNumber(text) else { throw Bad() }
            out += text
        }
    }

    static func isNumber(_ text: String) -> Bool {
        let c = Array(text)
        var i = 0
        func digits() -> Int {
            let from = i
            while i < c.count, c[i].isASCII, c[i].isNumber { i += 1 }
            return i - from
        }
        if i < c.count, c[i] == "-" { i += 1 }
        guard i < c.count else { return false }
        if c[i] == "0" {
            i += 1
        } else if digits() == 0 {
            return false
        }
        if i < c.count, c[i] == "." {
            i += 1
            if digits() == 0 { return false }
        }
        if i < c.count, c[i] == "e" || c[i] == "E" {
            i += 1
            if i < c.count, c[i] == "+" || c[i] == "-" { i += 1 }
            if digits() == 0 { return false }
        }
        return i == c.count
    }
}

public enum AgentTranscriptFormat {
    public static let steerMark = "⟵ "

    public static func blocks(_ transcript: String) -> [TranscriptBlock] {
        var parser = Parser()
        for raw in transcript.split(separator: "\n", omittingEmptySubsequences: false) {
            parser.feed(raw.hasSuffix("\r") ? String(raw.dropLast()) : String(raw))
        }
        parser.finish()
        return parser.out
    }

    public static func inline(_ text: String) -> [TranscriptInline] {
        let chars = Array(text)
        var out: [TranscriptInline] = []
        var plain = ""
        func flush() {
            if !plain.isEmpty { out.append(TranscriptInline(plain)) }
            plain = ""
        }
        func find(_ target: [Character], from start: Int) -> Int? {
            var i = start
            while i + target.count <= chars.count {
                if target.indices.allSatisfy({ chars[i + $0] == target[$0] }) { return i }
                i += 1
            }
            return nil
        }
        func wordChar(_ index: Int) -> Bool {
            index >= 0 && index < chars.count && (chars[index].isLetter || chars[index].isNumber)
        }
        func startsWith(_ word: String, at index: Int) -> Bool {
            let w = Array(word)
            return index + w.count <= chars.count && Array(chars[index..<(index + w.count)]) == w
        }
        var i = 0
        while i < chars.count {
            let c = chars[i]
            if c == "`", let end = find(["`"], from: i + 1), end > i + 1 {
                flush()
                out.append(TranscriptInline(String(chars[(i + 1)..<end]), .code))
                i = end + 1
                continue
            }
            if c == "*" || c == "_", i + 1 < chars.count, chars[i + 1] == c, c == "*" || !wordChar(i - 1),
                let end = find([c, c], from: i + 2), end > i + 2, c == "*" || !wordChar(end + 2)
            {
                flush()
                out.append(TranscriptInline(String(chars[(i + 2)..<end]), .bold))
                i = end + 2
                continue
            }
            if c == "*" || c == "_", i + 1 < chars.count, chars[i + 1] != " ", chars[i + 1] != c, !wordChar(i - 1),
                let end = find([c], from: i + 1), end > i + 1, chars[end - 1] != " ", !wordChar(end + 1),
                end + 1 >= chars.count || chars[end + 1] != c
            {
                flush()
                out.append(TranscriptInline(String(chars[(i + 1)..<end]), .italic))
                i = end + 1
                continue
            }
            if c == "[", let close = find(["]", "("], from: i + 1), close > i + 1,
                let end = find([")"], from: close + 2)
            {
                let url = String(chars[(close + 2)..<end])
                if url.hasPrefix("https://") || url.hasPrefix("http://") {
                    flush()
                    out.append(TranscriptInline(String(chars[(i + 1)..<close]), .link, url: url))
                    i = end + 1
                    continue
                }
            }
            if c == "h", i == 0 || chars[i - 1].isWhitespace || chars[i - 1] == "(",
                startsWith("https://", at: i) || startsWith("http://", at: i)
            {
                var end = i
                while end < chars.count, !chars[end].isWhitespace, !"<>\"'`".contains(chars[end]) { end += 1 }
                while end > i, ".,;:!?)".contains(chars[end - 1]) { end -= 1 }
                let url = String(chars[i..<end])
                if url.count > "https://".count {
                    flush()
                    out.append(TranscriptInline(url, .link, url: url))
                    i = end
                    continue
                }
            }
            plain.append(c)
            i += 1
        }
        flush()
        return out
    }

    static func tool(_ line: String) -> (String, String)? {
        if line.hasPrefix("\u{25B8} ") { return runnerTool(String(line.dropFirst(2))) }
        guard line.count > 3, line.hasPrefix("["), line.hasSuffix("]") else { return nil }
        let inner = String(line.dropFirst().dropLast())
        let name: String
        let detail: String
        if let space = inner.firstIndex(of: " ") {
            name = String(inner[..<space])
            detail = inner[inner.index(after: space)...].trimmingCharacters(in: .whitespaces)
        } else {
            name = inner
            detail = ""
        }
        return toolName(name) ? (name, detail) : nil
    }

    static func runnerTool(_ inner: String) -> (String, String)? {
        let name: String
        let detail: String
        if let colon = inner.range(of: ": ") {
            name = String(inner[..<colon.lowerBound])
            detail = inner[colon.upperBound...].trimmingCharacters(in: .whitespaces)
        } else {
            name = inner.trimmingCharacters(in: .whitespaces)
            detail = ""
        }
        return toolName(name) ? (name, detail) : nil
    }

    static func toolName(_ name: String) -> Bool {
        guard name.count >= 2, name.count <= 80, let first = name.first, first.isLetter else { return false }
        return name.allSatisfy { $0.isLetter || $0.isNumber || $0 == "_" || $0 == "." || $0 == ":" || $0 == "-" }
    }

    static let toolSpan = 300

    static func toolOpening(_ line: String) -> Bool {
        guard line.hasPrefix("["), !line.hasSuffix("]"), !line.contains("]("),
            let space = line.firstIndex(of: " ")
        else { return false }
        return tool(String(line[..<space]) + "]") != nil
    }

    static func heading(_ line: String) -> (Int, String)? {
        let hashes = line.prefix { $0 == "#" }.count
        guard hashes >= 1, hashes <= 6 else { return nil }
        let rest = line.dropFirst(hashes)
        guard rest.first == " " else { return nil }
        let text = rest.trimmingCharacters(in: .whitespaces)
        return text.isEmpty ? nil : (hashes, text)
    }

    static func bullet(_ line: String) -> (String, Int, String)? {
        var indent = 0
        var index = line.startIndex
        while index < line.endIndex, line[index] == " " || line[index] == "\t" {
            indent += line[index] == "\t" ? 4 : 1
            index = line.index(after: index)
        }
        let body = line[index...]
        if let first = body.first, "-*+".contains(first), body.dropFirst().first == " " {
            let text = body.dropFirst(2).trimmingCharacters(in: .whitespaces)
            return text.isEmpty ? nil : ("•", indent / 2, text)
        }
        let digits = body.prefix { $0.isASCII && $0.isNumber }
        guard !digits.isEmpty, digits.count <= 3 else { return nil }
        let after = body.dropFirst(digits.count)
        guard let mark = after.first, mark == "." || mark == ")", after.dropFirst().first == " " else { return nil }
        let text = after.dropFirst(2).trimmingCharacters(in: .whitespaces)
        return text.isEmpty ? nil : ("\(digits).", indent / 2, text)
    }

    static func isRule(_ line: String) -> Bool {
        let bare = line.filter { $0 != " " }
        guard bare.count >= 3, let first = bare.first, "-*_".contains(first) else { return false }
        return bare.allSatisfy { $0 == first }
    }

    static func task(_ text: String) -> (String, String)? {
        guard text.hasPrefix("Task ") else { return nil }
        let rest = text.dropFirst(5)
        guard let colon = rest.range(of: ": ") else { return nil }
        let status = String(rest[..<colon.lowerBound])
        guard !status.isEmpty, status.count <= 20, status != "cost",
            status.allSatisfy({ ("a"..."z").contains($0) || $0 == "_" })
        else { return nil }
        let summary = rest[colon.upperBound...].trimmingCharacters(in: .whitespaces)
        return summary.isEmpty ? nil : (status, summary)
    }

    static func cells(_ line: String) -> [String] {
        var body = line.trimmingCharacters(in: .whitespaces)
        if body.hasPrefix("|") { body.removeFirst() }
        if body.hasSuffix("|"), !body.hasSuffix("\\|") { body.removeLast() }
        var out: [String] = []
        var cell = ""
        var code = false
        let chars = Array(body)
        var i = 0
        while i < chars.count {
            let c = chars[i]
            if c == "\\", i + 1 < chars.count, chars[i + 1] == "|" {
                cell.append("|")
                i += 2
                continue
            }
            if c == "`" { code.toggle() }
            if c == "|", !code {
                out.append(cell.trimmingCharacters(in: .whitespaces))
                cell = ""
            } else {
                cell.append(c)
            }
            i += 1
        }
        out.append(cell.trimmingCharacters(in: .whitespaces))
        return out
    }

    static func isSeparator(_ cell: String) -> Bool {
        var body = Substring(cell)
        if body.hasPrefix(":") { body = body.dropFirst() }
        if body.hasSuffix(":") { body = body.dropLast() }
        return !body.isEmpty && body.allSatisfy { $0 == "-" }
    }

    static func table(_ lines: [String]) -> TranscriptTable? {
        guard lines.count >= 2 else { return nil }
        let separator = cells(lines[1])
        guard !separator.isEmpty, separator.allSatisfy(isSeparator) else { return nil }
        let header = cells(lines[0])
        let width = header.count
        guard width > 0, separator.count == width else { return nil }
        let align: [TranscriptAlign] = separator.map { cell in
            if cell.hasPrefix(":") && cell.hasSuffix(":") { return .center }
            if cell.hasSuffix(":") { return .end }
            return .start
        }
        let rows = lines.dropFirst(2).map { line -> [String] in
            let row = cells(line)
            return (0..<width).map { $0 < row.count ? row[$0] : "" }
        }
        return TranscriptTable(header: header, rows: rows, align: align)
    }

    struct TaskDraft: Sendable {
        let status: String
        let summary: String
        var lines: [String] = []
        var blanks = 0
    }

    struct Parser: Sendable {
        var out: [TranscriptBlock] = []
        var status: [String] = []
        var paragraph: [String] = []
        var prompt: [String]?
        var fence: [String]?
        var fenceLanguage = ""
        var table: [String] = []
        var tools: [TranscriptTool] = []
        var task: TaskDraft?
        var openTool: [String]?
        var openToolClose: Int?

        mutating func flushParagraph() {
            if !paragraph.isEmpty {
                let text = paragraph.joined(separator: "\n")
                let head = text.drop { $0 == " " || $0 == "\t" }
                if head.hasPrefix("{") || head.hasPrefix("["), let json = TranscriptJson.pretty(text) {
                    out.append(.json(json))
                } else {
                    out.append(.text(text))
                }
            }
            paragraph = []
        }

        mutating func flushTable() {
            if !table.isEmpty {
                if let grid = AgentTranscriptFormat.table(table) {
                    out.append(.table(grid))
                } else {
                    out.append(.code(language: "", text: table.joined(separator: "\n")))
                }
            }
            table = []
        }

        mutating func flushText() {
            flushParagraph()
            flushTable()
            if let lines = prompt {
                let text = lines.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
                if text.hasPrefix(AgentTranscriptFormat.steerMark),
                    let colon = text.range(of: ": "),
                    text.distance(from: text.startIndex, to: colon.lowerBound) > AgentTranscriptFormat.steerMark.count
                {
                    let from = text[text.index(text.startIndex, offsetBy: AgentTranscriptFormat.steerMark.count)..<colon.lowerBound]
                    out.append(
                        .steer(
                            from: from.trimmingCharacters(in: .whitespaces),
                            text: text[colon.upperBound...].trimmingCharacters(in: .whitespacesAndNewlines)))
                } else if !text.isEmpty {
                    out.append(.prompt(text))
                }
            }
            prompt = nil
        }

        mutating func flushStatus() {
            if !status.isEmpty { out.append(.status(status)) }
            status = []
        }

        mutating func flushTools() {
            if !tools.isEmpty { out.append(.tools(tools)) }
            tools = []
        }

        mutating func flushTask() {
            if let draft = task {
                var body = draft.lines.joined(separator: "\n")
                while let last = body.last, last.isWhitespace { body.removeLast() }
                out.append(.task(TranscriptTask(status: draft.status, summary: draft.summary, body: body)))
            }
            task = nil
        }

        mutating func flushAll() {
            flushText()
            flushStatus()
            flushTools()
            flushTask()
        }

        mutating func emit(_ block: TranscriptBlock) {
            flushAll()
            out.append(block)
        }

        func code(_ language: String, _ lines: [String]) -> TranscriptBlock {
            let text = lines.joined(separator: "\n")
            let lower = language.lowercased()
            if lower == "json" || lower == "jsonc" || lower.isEmpty, let json = TranscriptJson.pretty(text) {
                return .json(json)
            }
            return .code(language: language, text: text)
        }

        mutating func settleTool() {
            guard let lines = openTool else { return }
            let close = openToolClose
            openTool = nil
            openToolClose = nil
            if let close,
                let (name, detail) = AgentTranscriptFormat.tool(
                    lines[0...close].joined(separator: "\n").trimmingCharacters(in: .whitespaces))
            {
                tools.append(TranscriptTool(name, detail))
                for rest in lines[(close + 1)...] { feed(rest) }
                return
            }
            feed(lines[0], opening: false)
            for rest in lines.dropFirst() { feed(rest) }
        }

        mutating func continueTool(_ line: String, _ trimmed: String) {
            guard var lines = openTool else { return }
            if trimmed.isEmpty, openToolClose != nil {
                settleTool()
                feed(line)
                return
            }
            lines.append(line)
            openTool = lines
            if trimmed.hasSuffix("]") {
                openToolClose = lines.count - 1
                if trimmed.hasSuffix("…]") {
                    settleTool()
                    return
                }
            }
            if lines.reduce(0, { $0 + $1.count + 1 }) > AgentTranscriptFormat.toolSpan { settleTool() }
        }

        mutating func feed(_ line: String, opening: Bool = true) {
            let trimmed = line.trimmingCharacters(in: .whitespaces)
            if openTool != nil {
                continueTool(line, trimmed)
                return
            }
            if var lines = fence {
                if trimmed.hasPrefix("```") {
                    fence = nil
                    out.append(code(fenceLanguage, lines))
                } else {
                    lines.append(line)
                    fence = lines
                }
                return
            }
            if var draft = task {
                if line.hasPrefix("    ") {
                    draft.lines.append(contentsOf: Array(repeating: "", count: draft.blanks))
                    draft.blanks = 0
                    draft.lines.append(String(line.dropFirst(4)))
                    task = draft
                    return
                }
                if trimmed.isEmpty {
                    draft.blanks += 1
                    task = draft
                    return
                }
                flushTask()
            }
            if trimmed.isEmpty {
                flushText()
                return
            }
            if prompt != nil {
                if !line.hasPrefix("==>"), !line.hasPrefix("!!"), AgentTranscriptFormat.tool(trimmed) == nil {
                    prompt?.append(line)
                    return
                }
                flushText()
            }
            if trimmed.hasPrefix("```") {
                flushAll()
                fence = []
                fenceLanguage = String(trimmed.dropFirst(3)).trimmingCharacters(in: .whitespaces)
                return
            }
            if line.hasPrefix("==>") {
                let text = String(line.dropFirst(3)).trimmingCharacters(in: .whitespaces)
                flushText()
                flushTools()
                if let (status, summary) = AgentTranscriptFormat.task(text) {
                    flushAll()
                    task = TaskDraft(status: status, summary: summary)
                } else if text == "User steering prompt" {
                    flushStatus()
                    prompt = []
                } else if !text.isEmpty {
                    status.append(text)
                }
                return
            }
            if line.hasPrefix("!!") {
                emit(.error(String(line.dropFirst(2)).trimmingCharacters(in: .whitespaces)))
                return
            }
            if line.hasPrefix(">"), paragraph.isEmpty, table.isEmpty {
                flushAll()
                prompt = [String(line.dropFirst()).trimmingCharacters(in: .whitespaces)]
                return
            }
            if let (name, detail) = AgentTranscriptFormat.tool(trimmed) {
                flushText()
                flushStatus()
                tools.append(TranscriptTool(name, detail))
                return
            }
            if opening, paragraph.isEmpty, table.isEmpty, AgentTranscriptFormat.toolOpening(trimmed) {
                flushText()
                flushStatus()
                openTool = [line]
                return
            }
            if trimmed.hasPrefix("|") {
                flushParagraph()
                flushStatus()
                flushTools()
                table.append(trimmed)
                return
            }
            if !table.isEmpty { flushText() }
            if let (level, text) = AgentTranscriptFormat.heading(trimmed) {
                emit(.heading(level: level, text: text))
                return
            }
            if AgentTranscriptFormat.isRule(trimmed) {
                emit(.rule)
                return
            }
            if let (marker, depth, text) = AgentTranscriptFormat.bullet(line) {
                emit(.bullet(marker: marker, depth: depth, text: text))
                return
            }
            flushStatus()
            flushTools()
            paragraph.append(line)
        }

        mutating func finish() {
            while openTool != nil { settleTool() }
            if let lines = fence {
                out.append(code(fenceLanguage, lines))
                fence = nil
            }
            flushAll()
        }
    }
}
