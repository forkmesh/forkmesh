import Foundation

enum HeaderKind { case block, function, type, container }

struct HeaderInfo {
    var kind = HeaderKind.block
    var name = ""
    var parameters = 0
    var abstract = false
    var countedType = false
}

struct Measured {
    var info = InsightFile()
    var significant: [Int] = []
    var windows: [UInt64] = []
    var skip = false
}

enum Measure {
    static let window = 6

    static let countedTypeKeywords: Set<String> = ["class", "struct", "interface", "enum", "union", "trait", "protocol", "record", "object", "actor"]

    static let nonNameWords: Set<String> = [
        "if", "for", "while", "switch", "catch", "return", "new", "throw", "await", "sizeof", "typeof", "delete",
        "case", "function", "func", "fn", "fun", "def", "async", "decltype", "alignof", "static_assert", "foreach",
        "using", "lock", "when", "match", "elif", "assert", "super", "this", "sub",
    ]

    static let scriptLike: Set<String> = ["JavaScript", "TypeScript", "Vue", "Svelte"]
    static let wrappers: Set<String> = ["(anonymous)", "define", "describe", "suite", "context", "$", "jQuery", "ready", "addEventListener"]
    static let strictTernary: Set<String> = ["TypeScript", "Swift", "Kotlin", "C#", "Dart", "Vue", "Svelte", "Scala"]
    static let looseTernary: Set<String> = ["C", "C++", "Objective-C", "Java", "JavaScript", "PHP", "Groovy", "Perl", "QML"]
    static let typedLanguages: Set<String> = ["Swift", "C#", "Kotlin", "Java", "Scala", "Dart"]

    static let continuationEnds: [Units] = ["(", ",", "=", ":", "->", "=>", "<", "&", "|", "+", ".", "["].map(U.units)
    static let continuationStarts: [Units] = [",", ":", ".", "->", ")", "where", "throws", "const", "override", "noexcept", "final", "?", "&&", "||", "async", "->"].map(U.units)
    static let boilerplatePrefixes: [Units] = [
        "import ", "#include", "#import", "using ", "from ", "package ", "require", "use ", "@import", "export {",
        "export *", "} from", "#pragma", "#define", "#endif", "#if", "#else", "namespace ", "module ",
    ].map(U.units)

    static func lastSegment(_ qualified: String) -> String {
        let units = U.units(qualified)
        let colons = U.lastIndexOfPair(units, U.colon, U.colon)
        let dot = U.lastIndexOf(units, U.dot)
        let cut = max(colons, dot)
        if cut < 0 { return qualified }
        let skip = units[cut] == U.colon ? 2 : 1
        return U.string(Array(units[min(units.count, cut + skip)...]))
    }

    static func countParameters(_ inside: Units, python: Bool) -> Int {
        let trimmed = U.trimmed(inside)
        if trimmed.isEmpty || trimmed == U.units("void") { return 0 }
        var depth = 0
        var parts: [Units] = []
        var current: Units = []
        for c in trimmed {
            if c == U.openParen || c == U.less || c == U.openBracket || c == U.openBrace {
                depth += 1
            } else if c == U.closeParen || c == U.greater || c == U.closeBracket || c == U.closeBrace {
                depth = max(0, depth - 1)
            }
            if c == U.comma && depth == 0 {
                parts.append(U.trimmed(current))
                current = []
                continue
            }
            current.append(c)
        }
        parts.append(U.trimmed(current))
        var count = 0
        let selfWord = U.units("self")
        let clsWord = U.units("cls")
        for (i, part) in parts.enumerated() {
            if part.isEmpty { continue }
            if python && i == 0 && (part == selfWord || part == clsWord) { continue }
            if python && (part == [U.star] || part == [U.slash]) { continue }
            count += 1
        }
        return count
    }

    static func hasSuffix(_ s: Units, _ tail: Units) -> Bool {
        if tail.count > s.count { return false }
        let offset = s.count - tail.count
        for k in 0..<tail.count where s[offset + k] != tail[k] { return false }
        return true
    }

    static func hasPrefix(_ s: Units, _ head: Units) -> Bool {
        if head.count > s.count { return false }
        for k in 0..<head.count where s[k] != head[k] { return false }
        return true
    }

    static func parenBalance(_ s: Units) -> Int {
        var balance = 0
        for c in s {
            if c == U.openParen { balance += 1 } else if c == U.closeParen { balance -= 1 }
        }
        return balance
    }

    static func cleanHeader(_ raw: Units, _ range: Range<Int>) -> Units {
        var kept: [Units] = []
        var start = range.lowerBound
        var i = range.lowerBound
        while i <= range.upperBound {
            if i == range.upperBound || raw[i] == U.newline {
                let t = U.trim(raw, start..<i)
                if !t.isEmpty {
                    let first = raw[t.lowerBound]
                    let last = raw[t.upperBound - 1]
                    var skip = false
                    if first == U.hash {
                        skip = true
                    } else if first == U.at {
                        var hasSpace = false
                        var hasParen = false
                        for k in t {
                            if raw[k] == U.space { hasSpace = true }
                            if raw[k] == U.openParen { hasParen = true }
                        }
                        skip = !hasSpace && !hasParen
                    }
                    if !skip && first == U.openBracket && last == U.closeBracket { skip = true }
                    if !skip { kept.append(Array(raw[t])) }
                }
                start = i + 1
            }
            i += 1
        }
        if kept.count > 1 {
            var first = kept.count - 1
            var balance = parenBalance(kept[first])
            while first > 0 {
                let above = kept[first - 1]
                let current = kept[first]
                var join = balance < 0
                if !join {
                    for end in continuationEnds where hasSuffix(above, end) {
                        join = true
                        break
                    }
                }
                if !join {
                    for head in continuationStarts where hasPrefix(current, head) {
                        join = true
                        break
                    }
                }
                if !join { break }
                first -= 1
                balance += parenBalance(kept[first])
            }
            kept = Array(kept[first...])
        }
        var header: Units = []
        for (k, line) in kept.enumerated() {
            if k > 0 { header.append(U.space) }
            header.append(contentsOf: line)
        }
        while header.first == U.at {
            let space = U.indexOf(header, U.space)
            let paren = U.indexOf(header, U.openParen)
            if paren >= 0 && (space < 0 || paren < space) {
                let close = U.matchingParen(header, paren)
                if close < 0 { break }
                header = U.trimmed(Array(header[(close + 1)...]))
            } else if space > 0 {
                header = U.trimmed(Array(header[(space + 1)...]))
            } else {
                break
            }
        }
        return header
    }

    static func classifyHeader(_ raw: Units, _ range: Range<Int>, language: String) -> HeaderInfo {
        var info = HeaderInfo()
        let header = cleanHeader(raw, range)
        if header.isEmpty { return info }
        if P.controlStart.matches(header) { return info }
        let firstParen = U.indexOf(header, U.openParen)
        let beforeEnd = firstParen < 0 ? header.count : firstParen
        let keywordFunction = P.functionKeyword.matches(header)
        if !keywordFunction, let type = P.typeHeader.firstMatch(in: header, range: 0..<beforeEnd) {
            let keyword = type.string(1, in: header)
            let counted = countedTypeKeywords.contains(keyword)
            info.kind = counted ? .type : .container
            info.countedType = counted
            info.abstract = keyword == "interface" || keyword == "protocol" || keyword == "trait" || U.contains(header, "abstract ")
            let after = U.trim(header, type.group(1)!.upperBound..<beforeEnd)
            var end = after.lowerBound
            while end < after.upperBound && CharSet.word.contains(header[end]) { end += 1 }
            info.name = U.string(header, after.lowerBound..<end)
            return info
        }
        var function = keywordFunction
        if !function && hasSuffix(header, [U.equals, U.greater]) { function = true }
        if !function && (language == "Swift" || language == "Kotlin") {
            if let computed = P.computedProperty.firstMatch(in: header) {
                info.kind = .function
                info.name = computed.string(1, in: header)
                return info
            }
        }
        if !function && firstParen > 0 {
            let lastClose = U.lastIndexOf(header, U.closeParen)
            if lastClose > firstParen {
                let tail = (lastClose + 1)..<header.count
                var depth = 0
                var assignment = false
                var operatorSeen = false
                let operatorWord = U.units("operator")
                for k in 0..<firstParen {
                    let c = header[k]
                    if !operatorSeen && k >= operatorWord.count {
                        operatorSeen = header[(k - operatorWord.count)..<k].elementsEqual(operatorWord)
                    }
                    if c == U.less || c == U.openBracket {
                        depth += 1
                    } else if c == U.greater || c == U.closeBracket {
                        depth = max(0, depth - 1)
                    } else if c == U.equals && depth == 0 {
                        let prev: UInt16 = k > 0 ? header[k - 1] : 0
                        let nxt: UInt16 = k + 1 < header.count ? header[k + 1] : 0
                        if nxt != U.equals && prev != U.equals && prev != U.bang && prev != U.less && prev != U.greater && !operatorSeen {
                            assignment = true
                        }
                    }
                }
                let trimmedTail = U.trim(header, tail)
                let tailOk = P.tailAfterParen.matches(header, range: tail) || (!trimmedTail.isEmpty && header[trimmedTail.lowerBound] == U.colon)
                if !assignment && tailOk {
                    if let call = P.callName.firstMatch(in: header, range: 0..<(firstParen + 1)),
                       !nonNameWords.contains(lastSegment(call.string(1, in: header))) {
                        function = true
                    }
                }
            }
        }
        if !function { return info }
        info.kind = .function
        var paramsOpen = -1
        P.callName.forEachMatch(in: header, range: 0..<header.count) { m in
            let candidate = m.string(1, in: header)
            if nonNameWords.contains(lastSegment(candidate)) || nonNameWords.contains(candidate) { return true }
            info.name = candidate
            paramsOpen = m.end - 1
            return false
        }
        if info.name.isEmpty, let arrow = P.arrowName.firstMatch(in: header) {
            info.name = arrow.string(1, in: header)
        }
        if paramsOpen < 0 { paramsOpen = firstParen }
        if paramsOpen >= 0 {
            let close = U.matchingParen(header, paramsOpen)
            if close > paramsOpen {
                info.parameters = countParameters(Array(header[(paramsOpen + 1)..<close]), python: false)
            }
        }
        if info.name.isEmpty { info.name = "(anonymous)" }
        if scriptLike.contains(language) && wrappers.contains(lastSegment(info.name)) {
            info.kind = .container
        }
        return info
    }

    static func decisionPositions(_ code: Units, language: String) -> [Int] {
        var positions: [Int] = []
        let all = 0..<code.count
        P.decisionsBase.forEachMatch(in: code, range: all) { m in
            positions.append(m.start)
            return true
        }
        if strictTernary.contains(language) {
            P.ternaryStrict.forEachMatch(in: code, range: all) { m in
                positions.append(m.start)
                return true
            }
        } else if looseTernary.contains(language) {
            let n = code.count
            for i in 0..<n where code[i] == U.question {
                let next: UInt16 = i + 1 < n ? code[i + 1] : 0
                let prev: UInt16 = i > 0 ? code[i - 1] : 0
                if next == U.question || next == U.dot || next == U.colon || prev == U.question || next == U.greater
                    || next == U.closeParen || next == U.comma {
                    continue
                }
                positions.append(i)
            }
        } else if language == "Rust" {
            P.rustArms.forEachMatch(in: code, range: all) { m in
                positions.append(m.start)
                return true
            }
        }
        positions.sort()
        return positions
    }

    struct LineStarts {
        var starts: [Int] = [0]

        init(_ text: Units) {
            for i in 0..<text.count where text[i] == U.newline { starts.append(i + 1) }
        }

        func lineAt(_ position: Int) -> Int {
            var lo = 0
            var hi = starts.count
            while lo < hi {
                let mid = (lo + hi) / 2
                if starts[mid] <= position { lo = mid + 1 } else { hi = mid }
            }
            return lo
        }
    }

    static func finish(_ info: inout InsightFile, fileLevel: Int) {
        var total = fileLevel
        for fn in info.functions {
            total += fn.complexity
            info.maxComplexity = max(info.maxComplexity, fn.complexity)
            info.maxNesting = max(info.maxNesting, fn.nesting)
        }
        info.complexity = total
    }

    static func analyzeBraces(_ info: inout InsightFile, _ stripped: Stripped) {
        let code = stripped.code
        let lines = LineStarts(code)
        struct Frame {
            var kind: HeaderKind
            var function: Int
        }
        var stack: [Frame] = []
        var ranges: [(bodyStart: Int, bodyEnd: Int)] = []
        var headerStart = 0
        var functionDepth = -1
        var currentFunction = -1
        var parenDepth = 0
        let n = code.count
        var i = 0
        while i < n {
            let c = code[i]
            if c == U.openParen {
                parenDepth += 1
            } else if c == U.closeParen {
                parenDepth = max(0, parenDepth - 1)
            } else if c == U.semicolon && parenDepth == 0 {
                headerStart = i + 1
            } else if c == U.openBrace {
                if currentFunction >= 0 {
                    stack.append(Frame(kind: .block, function: currentFunction))
                    let nesting = stack.count - functionDepth - 1
                    info.functions[currentFunction].nesting = max(info.functions[currentFunction].nesting, nesting)
                } else {
                    let headerRange = headerStart..<i
                    let kind = classifyHeader(code, headerRange, language: info.language)
                    if kind.kind == .function {
                        var fn = InsightFunction()
                        fn.name = kind.name
                        var first = headerStart
                        while first < i && U.isSpace(code[first]) { first += 1 }
                        let cleaned = cleanHeader(code, headerRange)
                        if !cleaned.isEmpty {
                            let probe = Array(cleaned[0..<min(cleaned.count, 12)])
                            let at = U.indexOf(code, probe, from: first, startsBefore: i)
                            if at >= 0 && at < i { first = at }
                        }
                        fn.line = lines.lineAt(first)
                        fn.parameters = kind.parameters
                        info.functions.append(fn)
                        ranges.append((i + 1, i + 1))
                        currentFunction = info.functions.count - 1
                        stack.append(Frame(kind: .function, function: currentFunction))
                        functionDepth = stack.count - 1
                    } else {
                        if kind.kind == .type {
                            if kind.countedType { info.types += 1 }
                            if kind.abstract { info.abstractTypes += 1 }
                        }
                        stack.append(Frame(kind: kind.kind, function: -1))
                    }
                }
                headerStart = i + 1
                parenDepth = 0
            } else if c == U.closeBrace {
                if let frame = stack.popLast() {
                    if frame.kind == .function && stack.count == functionDepth {
                        ranges[frame.function].bodyEnd = i
                        info.functions[frame.function].length = max(1, lines.lineAt(i) - info.functions[frame.function].line + 1)
                        currentFunction = -1
                        functionDepth = -1
                    }
                }
                headerStart = i + 1
                parenDepth = 0
            }
            i += 1
        }
        if currentFunction >= 0 {
            ranges[currentFunction].bodyEnd = n
            info.functions[currentFunction].length = max(1, lines.lineAt(n - 1) - info.functions[currentFunction].line + 1)
        }
        var fileLevel = 0
        for position in decisionPositions(code, language: info.language) {
            var lo = 0
            var hi = ranges.count - 1
            var found = -1
            while lo <= hi {
                let mid = (lo + hi) / 2
                if ranges[mid].bodyStart <= position {
                    found = mid
                    lo = mid + 1
                } else {
                    hi = mid - 1
                }
            }
            if found >= 0 && position < ranges[found].bodyEnd {
                info.functions[found].complexity += 1
            } else {
                fileLevel += 1
            }
        }
        finish(&info, fileLevel: fileLevel)
        if info.types > 0 && info.abstractTypes == 0 && (info.language == "C++" || info.language == "C") && P.pureVirtual.matches(code) {
            info.abstractTypes = 1
        }
    }

    static func indentOf(_ s: Units, _ range: Range<Int>) -> Int {
        var indent = 0
        for i in range {
            let c = s[i]
            if c == U.space {
                indent += 1
            } else if c == U.tab {
                indent += 4
            } else {
                break
            }
        }
        return indent
    }

    static func analyzePython(_ info: inout InsightFile, _ stripped: Stripped) {
        let code = stripped.code
        let text = stripped.noComments
        let codeLines = U.lines(code)
        let textLines = U.lines(text)
        var current = -1
        var fnIndent = 0
        var lastCodeLine = 0
        var blocks: [Int] = []
        var fileLevel = 0
        func close(_ endLine: Int) {
            if current < 0 { return }
            info.functions[current].length = max(1, endLine - info.functions[current].line + 1)
            current = -1
            blocks.removeAll()
        }
        var continuation = 0
        for i in 0..<codeLines.count {
            let line = codeLines[i]
            let trimmed = U.trim(code, line)
            if trimmed.isEmpty { continue }
            let continued = continuation > 0
            for k in trimmed {
                let c = code[k]
                if c == U.openParen || c == U.openBracket || c == U.openBrace {
                    continuation += 1
                } else if c == U.closeParen || c == U.closeBracket || c == U.closeBrace {
                    continuation = max(0, continuation - 1)
                }
            }
            let lineNo = i + 1
            let indent = indentOf(code, line)
            if !continued && current >= 0 && indent <= fnIndent { close(lastCodeLine) }
            if !continued {
                if let def = P.pythonDef.firstMatch(in: code, range: trimmed), current < 0 {
                    var fn = InsightFunction()
                    fn.name = def.string(1, in: code)
                    fn.line = lineNo
                    if i < textLines.count {
                        let lastLine = min(textLines.count, i + 30) - 1
                        let source = Array(text[textLines[i].lowerBound..<textLines[lastLine].upperBound])
                        let open = U.indexOf(source, U.openParen)
                        if open >= 0 {
                            let closeAt = U.matchingParen(source, open)
                            if closeAt > open {
                                fn.parameters = countParameters(Array(source[(open + 1)..<closeAt]), python: true)
                            }
                        }
                    }
                    info.functions.append(fn)
                    current = info.functions.count - 1
                    fnIndent = indent
                    lastCodeLine = lineNo
                    continue
                }
                if P.pythonClass.matches(code, range: trimmed) {
                    info.types += 1
                    let slice = Array(code[trimmed])
                    if U.contains(slice, "ABC") || U.contains(slice, "Protocol") || U.contains(slice, "ABCMeta") {
                        info.abstractTypes += 1
                    }
                }
            }
            let decisions = P.pythonDecisions.count(in: code, range: trimmed)
            if current >= 0 {
                info.functions[current].complexity += decisions
                if !continued {
                    while let last = blocks.last, last >= indent { blocks.removeLast() }
                    info.functions[current].nesting = max(info.functions[current].nesting, blocks.count)
                    if P.pythonBlock.matches(code, range: trimmed) { blocks.append(indent) }
                }
            } else {
                fileLevel += decisions
            }
            lastCodeLine = lineNo
        }
        close(lastCodeLine)
        finish(&info, fileLevel: fileLevel)
    }

    static func analyzeKeyword(_ info: inout InsightFile, _ stripped: Stripped) {
        let code = stripped.code
        let codeLines = U.lines(code)
        var current = -1
        var baseDepth = 0
        var depth = 0
        var fileLevel = 0
        var lastCodeLine = 0
        let ifWord = U.units(" if ")
        for i in 0..<codeLines.count {
            let trimmed = U.trim(code, codeLines[i])
            if trimmed.isEmpty { continue }
            if let def = P.keywordDef.firstMatch(in: code, range: trimmed) {
                if current >= 0 {
                    info.functions[current].length = max(1, lastCodeLine - info.functions[current].line + 1)
                }
                var fn = InsightFunction()
                fn.name = def.string(1, in: code)
                fn.line = i + 1
                let slice = Array(code[trimmed])
                let open = U.indexOf(slice, U.openParen)
                if open >= 0 {
                    let closeAt = U.matchingParen(slice, open)
                    if closeAt > open {
                        fn.parameters = countParameters(Array(slice[(open + 1)..<closeAt]), python: false)
                    }
                }
                info.functions.append(fn)
                current = info.functions.count - 1
                baseDepth = depth
            }
            if U.hasPrefix(code, trimmed, "class ") || U.hasPrefix(code, trimmed, "module ") || U.hasPrefix(code, trimmed, "defmodule ") {
                info.types += 1
            }
            let slice = Array(code[trimmed])
            let inlineIf = U.indexOf(slice, ifWord) >= 0 && !U.hasPrefix(code, trimmed, "if") ? 1 : 0
            let opens = P.keywordOpen.count(in: code, range: trimmed) - inlineIf
            let ends = P.keywordEnd.count(in: code, range: trimmed)
            depth = max(0, depth + opens - ends)
            let decisions = P.keywordDecisions.count(in: code, range: trimmed)
            if current >= 0 {
                info.functions[current].complexity += decisions
                info.functions[current].nesting = max(info.functions[current].nesting, max(0, depth - baseDepth - 1))
            } else {
                fileLevel += decisions
            }
            lastCodeLine = i + 1
        }
        if current >= 0 {
            info.functions[current].length = max(1, lastCodeLine - info.functions[current].line + 1)
        }
        finish(&info, fileLevel: fileLevel)
    }

    static func boilerplate(_ line: Units) -> Bool {
        for prefix in boilerplatePrefixes where hasPrefix(line, prefix) { return true }
        return false
    }

    static func lineHash(_ line: Units) -> UInt64 {
        var h: UInt64 = 0x51ed_270b_cbf2_9ce4
        for c in line {
            h ^= UInt64(c)
            h = h &* 0x100_0000_01b3
        }
        return h &* 0x9e37_79b9_7f4a_7c15
    }

    static func computeWindows(_ measured: inout Measured, _ noComments: Units) {
        var hashes: [UInt64] = []
        let lines = U.lines(noComments)
        for (i, line) in lines.enumerated() {
            let normalized = U.simplified(noComments, line)
            if normalized.count < 4 || boilerplate(normalized) { continue }
            var punctuation = true
            for c in normalized where U.isLetterOrNumber(c) {
                punctuation = false
                break
            }
            if punctuation { continue }
            measured.significant.append(i + 1)
            hashes.append(lineHash(normalized))
        }
        if hashes.count < window { return }
        for k in 0...(hashes.count - window) {
            var h: UInt64 = 0xcbf2_9ce4_8422_2325
            for j in 0..<window {
                h ^= hashes[k + j] &+ 0x9e37_79b9_7f4a_7c15 &+ (h << 6) &+ (h >> 2)
                h = h &* 0x100_0000_01b3
            }
            measured.windows.append(h)
        }
    }

    static func collectSymbols(_ info: inout InsightFile, _ stripped: Stripped) {
        guard typedLanguages.contains(info.language) else { return }
        let code = stripped.code
        let all = 0..<code.count
        var declared = Set<String>()
        P.typeDeclaration.forEachMatch(in: code, range: all) { m in
            let name = m.string(2, in: code)
            if declared.insert(name).inserted { info.declares.append(name) }
            return true
        }
        if let package = P.packageDeclaration.firstMatch(in: code) {
            info.declares.append("package:" + package.string(1, in: code))
        }
        var referenced = Set<String>()
        P.capitalized.forEachMatch(in: code, range: all) { m in
            if referenced.count >= 800 { return false }
            let name = U.string(code, m.range)
            if !declared.contains(name) { referenced.insert(name) }
            return true
        }
        info.references = referenced.sorted(by: U.less)
    }

    static func generated(_ bytes: [UInt8]) -> Bool {
        let head = bytes.prefix(2048).map { $0 >= 65 && $0 <= 90 ? $0 + 32 : $0 }
        let needles = ["@generated", "do not edit", "auto-generated", "autogenerated", "generated by the protocol buffer", "code generated by"]
        for needle in needles where containsBytes(head, Array(needle.utf8)) { return true }
        return false
    }

    static func containsBytes(_ hay: [UInt8], _ needle: [UInt8]) -> Bool {
        if needle.count > hay.count { return false }
        let head = needle[0]
        var i = 0
        while i <= hay.count - needle.count {
            if hay[i] == head {
                var k = 1
                while k < needle.count && hay[i + k] == needle[k] { k += 1 }
                if k == needle.count { return true }
            }
            i += 1
        }
        return false
    }

    static func binary(_ bytes: [UInt8]) -> Bool {
        bytes.prefix(8000).contains(0)
    }

    static func shouldSkip(_ bytes: [UInt8]) -> Bool {
        if bytes.isEmpty || binary(bytes) || generated(bytes) { return true }
        if bytes.count > 20000 {
            var newlines = 0
            for b in bytes where b == 10 { newlines += 1 }
            if newlines < bytes.count / 400 { return true }
        }
        return false
    }

    static func measure(_ path: String, _ bytes: [UInt8], windows withWindows: Bool) -> Measured {
        var measured = Measured()
        var info = InsightFile(path: path)
        let spec = Languages.spec(for: path)
        info.language = spec.name
        info.bytes = Int64(bytes.count)
        info.test = Languages.isTest(path)
        let text = U.decode(bytes)
        let stripped = Strip.source(text, spec)
        let original = U.lines(text)
        let withoutComments = U.lines(stripped.noComments)
        let codeOnly = U.lines(stripped.code)
        var count = original.count
        if count > 0 && original[count - 1].isEmpty { count -= 1 }
        info.lines = count
        for i in 0..<count {
            if U.trim(text, original[i]).isEmpty {
                info.blankLines += 1
                continue
            }
            let nc = i < withoutComments.count ? U.trim(stripped.noComments, withoutComments[i]) : 0..<0
            if nc.isEmpty {
                info.commentLines += 1
                continue
            }
            if spec.syntax == .python && i < codeOnly.count {
                let codeRange = U.trim(stripped.code, codeOnly[i])
                var quotesOnly = !codeRange.isEmpty
                for k in codeRange {
                    let c = stripped.code[k]
                    if c != U.quote && c != U.apostrophe && !U.isSpace(c) {
                        quotesOnly = false
                        break
                    }
                }
                if quotesOnly {
                    info.commentLines += 1
                    continue
                }
            }
            info.codeLines += 1
        }
        switch spec.syntax {
        case .braces: analyzeBraces(&info, stripped)
        case .python: analyzePython(&info, stripped)
        case .keyword: analyzeKeyword(&info, stripped)
        case .css: info.complexity = 0
        }
        info.imports = Imports.fromStripped(spec.name, stripped.noComments)
        collectSymbols(&info, stripped)
        measured.info = info
        if withWindows && spec.syntax != .css { computeWindows(&measured, stripped.noComments) }
        return measured
    }
}

extension U {
    static func lastIndexOfPair(_ s: Units, _ a: UInt16, _ b: UInt16) -> Int {
        var i = s.count - 2
        while i >= 0 {
            if s[i] == a && s[i + 1] == b { return i }
            i -= 1
        }
        return -1
    }
}
