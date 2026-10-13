// The result is platform neutral: tokens carrying a UTF-16 range and a
// `TokenKind`, and `TokenKind` names the theme colour the app resolves. The
// tokenizer is one alternation written out by hand over code units, so it
// needs no regex engine; its `\w`, `\d`, `\b` and `\s` mean what they mean
// in a non-unicode ECMAScript regex.

import Foundation

/// Language families the lightweight highlighter understands.
public enum CodeLang: String, Sendable, CaseIterable {
    case dart, typescript, javascript, python, go, rust, c, java, kotlin, swift
    case ruby, shell, yaml, toml, json, html, css, markdown, plain

    public var label: String {
        switch self {
        case .dart: return "Dart"
        case .typescript: return "TypeScript"
        case .javascript: return "JavaScript"
        case .python: return "Python"
        case .go: return "Go"
        case .rust: return "Rust"
        case .c: return "C / C++"
        case .java: return "Java"
        case .kotlin: return "Kotlin"
        case .swift: return "Swift"
        case .ruby: return "Ruby"
        case .shell: return "Shell"
        case .yaml: return "YAML"
        case .toml: return "TOML"
        case .json: return "JSON"
        case .html: return "HTML"
        case .css: return "CSS"
        case .markdown: return "Markdown"
        case .plain: return "Plain text"
        }
    }

    /// The language for a file path, by name and extension.
    public static func forPath(_ path: String) -> CodeLang {
        let lower = path.lowercased()
        let name = lower.split(separator: "/", omittingEmptySubsequences: false).last.map(String.init) ?? ""
        if name == "dockerfile" || name == "makefile" || name.hasSuffix(".mk") { return .shell }
        guard let dot = name.lastIndex(of: ".") else { return .plain }
        switch String(name[name.index(after: dot)...]) {
        case "dart": return .dart
        case "ts", "tsx": return .typescript
        case "js", "jsx", "mjs", "cjs": return .javascript
        case "py", "pyi": return .python
        case "go": return .go
        case "rs": return .rust
        case "c", "cc", "cpp", "cxx", "h", "hpp", "m", "mm": return .c
        case "java": return .java
        case "kt", "kts": return .kotlin
        case "swift": return .swift
        case "rb", "rake", "gemspec": return .ruby
        case "sh", "bash", "zsh", "fish": return .shell
        case "yaml", "yml": return .yaml
        case "toml", "ini", "cfg", "conf": return .toml
        case "json", "jsonc", "lock": return .json
        case "html", "htm", "xml", "svg", "vue", "svelte": return .html
        case "css", "scss", "less": return .css
        case "md", "mdx", "markdown": return .markdown
        default: return .plain
        }
    }
}

/// What a run of code is, and therefore how the theme paints it.
public enum TokenKind: String, Sendable, CaseIterable {
    /// Code no rule claims.
    case plain
    case keyword
    case type
    case string
    case comment
    case number
    case function
    /// A Markdown heading: the keyword colour at the emphasis weight.
    case heading

    /// The `FmTheme` colour for this kind. `codeBase` is
    /// `CodeHighlighter.baseColorDarkARGB` in dark mode and `textPrimary` in
    /// light mode.
    public var themeColorName: String {
        switch self {
        case .plain: return "codeBase"
        case .keyword, .heading: return "codeKeyword"
        case .type: return "codeType"
        case .string: return "codeString"
        case .comment: return "codeComment"
        case .number: return "codeNumber"
        case .function: return "codeFunction"
        }
    }

    /// Comments are set in italics.
    public var isItalic: Bool { self == .comment }

    /// `FmType.emphasis` weight.
    public var isEmphasized: Bool { self == .heading }
}

/// A coloured run inside one line: UTF-16 offsets `[start, end)` and a kind.
public struct HighlightToken: Equatable, Hashable, Sendable {
    public let start: Int
    public let end: Int
    public let kind: TokenKind

    public init(start: Int, end: Int, kind: TokenKind) {
        self.start = start
        self.end = end
        self.kind = kind
    }

    public var range: DiffRange { DiffRange(start, end) }
    public var nsRange: NSRange { NSRange(location: start, length: end - start) }

    /// The token's text. Fine for one token; to cut a whole line into pieces
    /// use `CodeHighlighter.pieces`, which converts the line once.
    public func text(in line: String) -> String { line.substring(utf16: start, end) }
}

/// Per-line syntax highlighter that tracks `/* */` blocks across lines. It
/// aims for a readable editor look, not a full parser.
public struct CodeHighlighter: Sendable {
    public let lang: CodeLang

    /// `codeBaseColor` in dark mode; light mode uses the theme's textPrimary.
    public static let baseColorDarkARGB: UInt32 = 0xFFD7_D9E0

    private let keywords: Set<String>
    private let hashComments: Bool
    private let slashComments: Bool
    private let blockComments: Bool
    private let isHTML: Bool

    public init(_ lang: CodeLang) {
        self.lang = lang
        keywords = CodeHighlighter.keywords(for: lang)
        switch lang {
        case .python, .ruby, .shell, .yaml, .toml:
            hashComments = true
        default:
            hashComments = false
        }
        switch lang {
        case .python, .ruby, .shell, .yaml, .toml, .markdown, .html, .plain:
            slashComments = false
        default:
            slashComments = true
        }
        switch lang {
        case .python, .ruby, .shell, .yaml, .toml, .markdown, .plain, .json:
            blockComments = false
        default:
            blockComments = true
        }
        isHTML = lang == .html
    }

    // MARK: - Block comments

    /// For each line, whether it begins inside an unterminated `/* … */`.
    public func blockCommentStarts(_ lines: [String]) -> [Bool] {
        var result = [Bool](repeating: false, count: lines.count)
        guard blockComments else { return result }
        var inBlock = false
        for i in lines.indices {
            result[i] = inBlock
            // Only ASCII markers matter here and no offsets leave this
            // function, so the cheaper UTF-8 view will do.
            inBlock = CodeHighlighter.scanBlockState(Array(lines[i].utf8), inBlock: inBlock)
        }
        return result
    }

    private static func scanBlockState(_ line: [UInt8], inBlock start: Bool) -> Bool {
        var inBlock = start
        var cursor = 0
        let count = line.count
        func find(_ a: UInt8, _ b: UInt8, from: Int) -> Int {
            var i = from
            while i + 1 < count {
                if line[i] == a && line[i + 1] == b { return i }
                i += 1
            }
            return -1
        }
        while cursor < count {
            if inBlock {
                let close = find(0x2A, 0x2F, from: cursor)
                if close < 0 { break }
                inBlock = false
                cursor = close + 2
            } else {
                let open = find(0x2F, 0x2A, from: cursor)
                if open < 0 { break }
                let lineComment = find(0x2F, 0x2F, from: cursor)
                if lineComment >= 0 && lineComment < open { break }
                inBlock = true
                cursor = open + 2
            }
        }
        return inBlock
    }

    // MARK: - Highlighting

    /// Tokens covering the whole line in order, adjacent runs of one kind
    /// merged. An empty line has no tokens.
    public func highlight(_ line: String, startsInBlockComment: Bool = false) -> [HighlightToken] {
        highlight(units: U16.units(line), startsInBlockComment: startsInBlockComment)
    }

    /// The line cut into `(text, kind)` pieces, converting it only once.
    public func pieces(_ line: String, startsInBlockComment: Bool = false) -> [(text: String, kind: TokenKind)] {
        let units = U16.units(line)
        return highlight(units: units, startsInBlockComment: startsInBlockComment).map {
            (U16.string(units[$0.start..<$0.end]), $0.kind)
        }
    }

    func highlight(units line: Units, startsInBlockComment: Bool) -> [HighlightToken] {
        var out = TokenSink()
        let count = line.count
        var cursor = 0
        if startsInBlockComment {
            let close = U16.indexOf(line, Marker.blockClose, from: 0)
            if close < 0 {
                out.add(0, count, .comment)
                return out.tokens
            }
            out.add(0, close + 2, .comment)
            cursor = close + 2
        }
        if lang == .markdown {
            markdown(line, from: cursor, into: &out)
            return out.tokens
        }

        var position = cursor
        while position < count {
            guard let end = matchEnd(line, at: position) else {
                position += 1
                continue
            }
            out.add(cursor, position, .plain)
            out.add(position, end, classify(line, position, end))
            cursor = end
            // Every alternative consumes at least one unit.
            position = end
        }
        out.add(cursor, count, .plain)
        return out.tokens
    }

    private struct TokenSink {
        var tokens: [HighlightToken] = []

        mutating func add(_ start: Int, _ end: Int, _ kind: TokenKind) {
            guard end > start else { return }
            if let last = tokens.last, last.kind == kind, last.end == start {
                tokens[tokens.count - 1] = HighlightToken(start: last.start, end: end, kind: kind)
            } else {
                tokens.append(HighlightToken(start: start, end: end, kind: kind))
            }
        }
    }

    private enum Marker {
        static let blockClose = U16.units("*/")
        static let blockOpen = U16.units("/*")
        static let lineComment = U16.units("//")
        static let htmlOpen = U16.units("<!--")
        static let htmlClose = U16.units("-->")
        static let fence = U16.units("```")
    }

    /// The alternation, tried in order at one
    /// position. Returns where the first alternative that matches ends.
    private func matchEnd(_ line: Units, at p: Int) -> Int? {
        let count = line.count
        let unit = line[p]
        switch unit {
        case U16.less:
            if isHTML, U16.hasPrefix(line, Marker.htmlOpen, at: p),
               let end = CodeHighlighter.lazyUntil(line, from: p + 4, close: Marker.htmlClose) {
                return end
            }
            // `</?[A-Za-z][\w.:-]*`
            var i = p + 1
            if i < count, line[i] == U16.slash { i += 1 }
            guard i < count, U16.isAlpha(line[i]) else { return nil }
            i += 1
            while i < count, U16.isWord(line[i]) || line[i] == U16.dot || line[i] == U16.colon
                || line[i] == U16.minus {
                i += 1
            }
            return i
        case U16.quote, U16.apostrophe:
            // `"(?:[^"\\]|\\.)*"?`: an unterminated string runs to the end.
            var i = p + 1
            while i < count {
                let c = line[i]
                if c == unit { return i + 1 }
                if c == U16.backslash {
                    guard i + 1 < count, !U16.isLineTerminator(line[i + 1]) else { break }
                    i += 2
                } else {
                    i += 1
                }
            }
            return i
        case U16.backtick:
            var i = p + 1
            while i < count, line[i] != U16.backtick { i += 1 }
            return i < count ? i + 1 : i
        case U16.slash:
            guard slashComments, p + 1 < count else { return nil }
            if line[p + 1] == U16.slash { return CodeHighlighter.restOfLine(line, from: p + 2) }
            if line[p + 1] == U16.star {
                return CodeHighlighter.lazyUntil(line, from: p + 2, close: Marker.blockClose)
            }
            return nil
        case U16.hash:
            return hashComments ? CodeHighlighter.restOfLine(line, from: p + 1) : nil
        case U16.at:
            // `@[A-Za-z_][\w.]*`
            var i = p + 1
            guard i < count, U16.isAlpha(line[i]) || line[i] == U16.underscore else { return nil }
            i += 1
            while i < count, U16.isWord(line[i]) || line[i] == U16.dot { i += 1 }
            return i
        default:
            if U16.isDigit(unit) { return CodeHighlighter.numberEnd(line, at: p) }
            if U16.isAlpha(unit) || unit == U16.underscore || unit == U16.dollar {
                var i = p + 1
                while i < count, U16.isWord(line[i]) || line[i] == U16.dollar { i += 1 }
                return i
            }
            return nil
        }
    }

    /// `.*`: to the end, or to the first line terminator a line may still hold.
    private static func restOfLine(_ line: Units, from: Int) -> Int {
        var i = from
        while i < line.count, !U16.isLineTerminator(line[i]) { i += 1 }
        return i
    }

    /// `.*?(?:close|$)`: up to and including `close`, else the end of the
    /// input; a line terminator in between defeats both.
    private static func lazyUntil(_ line: Units, from: Int, close: Units) -> Int? {
        var i = from
        while i < line.count {
            if U16.hasPrefix(line, close, at: i) { return i + close.count }
            if U16.isLineTerminator(line[i]) { return nil }
            i += 1
        }
        return line.count
    }

    /// `\b0[xX][0-9a-fA-F_]+\b` then `\b\d[\d_]*(?:\.\d+)?(?:[eE][+-]?\d+)?\b`.
    private static func numberEnd(_ line: Units, at p: Int) -> Int? {
        let count = line.count
        if p > 0, U16.isWord(line[p - 1]) { return nil }
        func boundary(_ i: Int) -> Bool { i >= count || !U16.isWord(line[i]) }
        func digitsEnd(_ from: Int) -> Int {
            var i = from
            while i < count, U16.isDigit(line[i]) { i += 1 }
            return i
        }

        if line[p] == 0x30, p + 2 < count, line[p + 1] | 0x20 == 0x78 {
            var i = p + 2
            while i < count, U16.isHexDigit(line[i]) || line[i] == U16.underscore { i += 1 }
            if i > p + 2, boundary(i) { return i }
        }

        var run = p + 1
        while run < count, U16.isDigit(line[run]) || line[run] == U16.underscore { run += 1 }
        func exponentEnd(_ from: Int) -> Int? {
            guard from < count, line[from] | 0x20 == 0x65 else { return nil }
            var i = from + 1
            if i < count, line[i] == U16.plus || line[i] == U16.minus { i += 1 }
            let end = digitsEnd(i)
            return end > i ? end : nil
        }
        var fraction: Int?
        if run + 1 < count, line[run] == U16.dot, U16.isDigit(line[run + 1]) {
            fraction = digitsEnd(run + 1)
        }
        // The regex engine's preference order: both optional groups, then
        // fewer. A shorter digit run can never end on a boundary.
        var candidates: [Int] = []
        if let fraction {
            if let exponent = exponentEnd(fraction) { candidates.append(exponent) }
            candidates.append(fraction)
        }
        if let exponent = exponentEnd(run) { candidates.append(exponent) }
        candidates.append(run)
        return candidates.first(where: boundary)
    }

    private func classify(_ line: Units, _ start: Int, _ end: Int) -> TokenKind {
        let first = line[start]
        if first == U16.slash || first == U16.hash { return .comment }
        if first == U16.less {
            return U16.hasPrefix(line, Marker.htmlOpen, at: start) && isHTML ? .comment : .type
        }
        if first == U16.quote || first == U16.apostrophe || first == U16.backtick { return .string }
        if first == U16.at { return .function }
        if U16.isDigit(first) { return .number }
        let token = U16.string(line[start..<end])
        if keywords.contains(token) { return .keyword }
        if CodeHighlighter.literals.contains(token) { return .number }
        if CodeHighlighter.types.contains(token) { return .type }
        var i = end
        while i < line.count, U16.isRegexSpace(line[i]) { i += 1 }
        if i < line.count, line[i] == U16.openParen {
            // Call or constructor: Capitalised names read as types.
            return U16.isUpper(first) ? .type : .function
        }
        return .plain
    }

    private func markdown(_ line: Units, from cursor: Int, into out: inout TokenSink) {
        let count = line.count
        let trimmed = U16.trimLeft(line[cursor..<count])
        if trimmed.first == U16.hash {
            out.add(cursor, count, .heading)
        } else if trimmed.first == U16.greater {
            out.add(cursor, count, .comment)
        } else if U16.hasPrefix(trimmed, Marker.fence) {
            out.add(cursor, count, .string)
        } else if let bullet = CodeHighlighter.listMarkerEnd(line, from: cursor) {
            out.add(cursor, bullet, .keyword)
            out.add(bullet, count, .plain)
        } else {
            out.add(cursor, count, .plain)
        }
    }

    /// `^(\s*)([-*+]|\d+\.)\s`
    private static func listMarkerEnd(_ line: Units, from: Int) -> Int? {
        let count = line.count
        var i = from
        while i < count, U16.isRegexSpace(line[i]) { i += 1 }
        guard i < count else { return nil }
        if line[i] == U16.minus || line[i] == U16.star || line[i] == U16.plus {
            i += 1
        } else if U16.isDigit(line[i]) {
            while i < count, U16.isDigit(line[i]) { i += 1 }
            guard i < count, line[i] == U16.dot else { return nil }
            i += 1
        } else {
            return nil
        }
        guard i < count, U16.isRegexSpace(line[i]) else { return nil }
        return i + 1
    }

    // MARK: - Word lists

    static let literals: Set<String> = [
        "true", "false", "null", "nil", "None", "True", "False", "undefined", "NaN", "self", "this", "super",
    ]

    static func keywords(for lang: CodeLang) -> Set<String> {
        switch lang {
        case .python: return pythonKeywords
        case .ruby: return rubyKeywords
        case .shell: return shellKeywords
        case .css: return cssKeywords
        case .yaml, .toml, .json, .markdown, .plain: return []
        default: return cLikeKeywords
        }
    }

    static let cLikeKeywords: Set<String> = [
        "if", "else", "for", "while", "do", "return", "break", "continue", "switch", "case", "default",
        "try", "catch", "finally", "throw", "throws", "new", "class", "extends", "implements", "import",
        "export", "from", "as", "in", "of", "is", "async", "await", "yield", "static", "const", "let",
        "var", "function", "void", "enum", "interface", "type", "namespace", "with", "mixin", "typedef",
        "final", "late", "required", "get", "set", "abstract", "override", "operator", "extension",
        "part", "library", "show", "hide", "external", "factory", "covariant", "sealed", "base",
        "public", "private", "protected", "package", "struct", "union", "func", "fn", "pub", "mut",
        "impl", "trait", "use", "mod", "crate", "where", "match", "loop", "move", "ref", "dyn", "unsafe",
        "defer", "select", "range", "go", "chan", "map", "fallthrough", "goto", "instanceof", "typeof",
        "delete", "fun", "val", "when", "object", "companion", "data", "open", "internal", "lateinit",
        "init", "suspend", "guard", "protocol", "inout", "sizeof", "unsigned", "signed", "volatile",
        "register", "inline", "virtual", "template", "typename", "using", "constexpr", "nullptr",
        "declare", "readonly", "keyof", "satisfies", "debugger", "then", "on",
    ]

    static let pythonKeywords: Set<String> = [
        "def", "lambda", "pass", "raise", "except", "elif", "not", "and", "or", "global", "nonlocal",
        "del", "assert", "with", "yield", "from", "import", "as", "class", "return", "if", "else",
        "for", "while", "try", "finally", "in", "is", "async", "await", "break", "continue", "match",
        "case",
    ]

    static let rubyKeywords: Set<String> = [
        "def", "end", "do", "module", "class", "if", "elsif", "else", "unless", "while", "until", "for",
        "in", "return", "yield", "begin", "rescue", "ensure", "raise", "require", "require_relative",
        "include", "extend", "attr_reader", "attr_writer", "attr_accessor", "puts", "then", "case",
        "when", "and", "or", "not", "lambda", "proc", "private", "public",
    ]

    static let shellKeywords: Set<String> = [
        "if", "then", "fi", "else", "elif", "for", "in", "do", "done", "while", "until", "case", "esac",
        "function", "export", "local", "return", "echo", "exit", "source", "set", "unset", "readonly",
        "shift", "cd", "FROM", "RUN", "CMD", "COPY", "ADD", "ENV", "ARG", "WORKDIR", "EXPOSE",
        "ENTRYPOINT", "VOLUME", "USER", "LABEL",
    ]

    static let cssKeywords: Set<String> = [
        "import", "media", "keyframes", "font-face", "supports", "important", "from", "to",
    ]

    static let types: Set<String> = [
        "String", "int", "double", "bool", "num", "List", "Map", "Set", "Future", "Stream", "Widget",
        "State", "BuildContext", "Object", "dynamic", "Never", "Iterable", "Duration", "DateTime",
        "Uint8List", "number", "string", "boolean", "any", "unknown", "never", "Promise", "Array",
        "Record", "Partial", "Readonly", "Date", "Error", "float", "str", "bytes", "dict", "list",
        "tuple", "float32", "float64", "int32", "int64", "uint", "uint8", "byte", "rune", "error",
        "i32", "i64", "u8", "u32", "u64", "usize", "isize", "f32", "f64", "char", "Vec", "Option",
        "Result", "Box", "Rc", "Arc", "HashMap", "size_t", "long", "short", "Int", "Double", "Float",
        "Boolean", "Unit", "Any", "Character",
    ]
}
