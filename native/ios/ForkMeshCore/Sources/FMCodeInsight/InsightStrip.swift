import Foundation

struct Stripped {
    var noComments: Units
    var code: Units
}

enum Strip {
    static func source(_ text: Units, _ spec: LanguageSpec) -> Stripped {
        var nc = text
        var cd = text
        let n = text.count
        let python = spec.syntax == .python
        let name = spec.name
        let backticks = name == "JavaScript" || name == "TypeScript" || name == "Vue" || name == "Svelte" || name == "Go" || name == "Shell"
        let rust = name == "Rust"
        let cFamily = name == "C" || name == "C++" || name == "Objective-C"
        let shell = name == "Shell" || name == "Perl"
        let elixir = spec.syntax == .keyword && name == "Elixir"
        @inline(__always) func blank(_ buffer: inout Units, _ at: Int) {
            if buffer[at] != U.newline { buffer[at] = U.space }
        }
        var i = 0
        while i < n {
            let c = text[i]
            let next: UInt16 = i + 1 < n ? text[i + 1] : 0
            var lineComment = false
            switch spec.comments {
            case .cLike:
                lineComment = c == U.slash && next == U.slash
            case .lua:
                lineComment = c == U.dash && next == U.dash
            case .hash:
                if c == U.hash {
                    var excluded = false
                    if shell && i > 0 {
                        let prev = text[i - 1]
                        excluded = prev == U.dollar || prev == U.openBrace || !(U.isSpace(prev) || prev == U.semicolon)
                    }
                    lineComment = !excluded
                }
            case .css:
                lineComment = false
            }
            if spec.comments == .lua && lineComment && i + 3 < n && text[i + 2] == U.openBracket && text[i + 3] == U.openBracket {
                let end = U.indexOf(text, [U.closeBracket, U.closeBracket], from: i + 4)
                let stop = end < 0 ? n : end + 2
                for k in i..<stop {
                    blank(&nc, k)
                    blank(&cd, k)
                }
                i = stop
                continue
            }
            if lineComment {
                while i < n && text[i] != U.newline {
                    blank(&nc, i)
                    blank(&cd, i)
                    i += 1
                }
                continue
            }
            if (spec.comments == .cLike || spec.comments == .css) && c == U.slash && next == U.star {
                let end = U.indexOf(text, [U.star, U.slash], from: i + 2)
                let stop = end < 0 ? n : end + 2
                for k in i..<stop {
                    blank(&nc, k)
                    blank(&cd, k)
                }
                i = stop
                continue
            }
            let quote = c == U.quote || c == U.apostrophe || (backticks && c == U.backtick)
            if quote {
                if c == U.apostrophe {
                    if rust && i + 2 < n && (U.isLetter(text[i + 1]) || text[i + 1] == U.underscore) && text[i + 2] != U.apostrophe {
                        i += 1
                        continue
                    }
                    if cFamily && i > 0 && U.isDigit(text[i - 1]) {
                        i += 1
                        continue
                    }
                    if elixir {
                        i += 1
                        continue
                    }
                }
                let triple = python && i + 2 < n && text[i + 1] == c && text[i + 2] == c
                let multiline = triple || c == U.backtick
                var k = i + (triple ? 3 : 1)
                while k < n {
                    let ch = text[k]
                    if ch == U.backslash {
                        k += 2
                        continue
                    }
                    if !multiline && ch == U.newline { break }
                    if ch == c {
                        if !triple { break }
                        if k + 2 < n && text[k + 1] == c && text[k + 2] == c { break }
                    }
                    k += 1
                }
                let contentStart = i + (triple ? 3 : 1)
                let contentEnd = min(k, n)
                if contentStart < contentEnd {
                    for m in contentStart..<contentEnd { blank(&cd, m) }
                }
                i = min(n, contentEnd + (k < n && text[k] == c ? (triple ? 3 : 1) : 0))
                continue
            }
            i += 1
        }
        if cFamily {
            var lineStart = 0
            while lineStart < n {
                var lineEnd = U.indexOf(cd, U.newline, from: lineStart)
                if lineEnd < 0 { lineEnd = n }
                var first = lineStart
                while first < lineEnd && U.isSpace(cd[first]) { first += 1 }
                if first < lineEnd && cd[first] == U.hash {
                    var end = lineEnd
                    while true {
                        var last = end - 1
                        while last > lineStart && U.isSpace(cd[last]) { last -= 1 }
                        if lineStart < end {
                            for m in lineStart..<end { blank(&cd, m) }
                        }
                        if last >= lineStart && cd[last] == U.backslash && end < n {
                            lineStart = end + 1
                            end = U.indexOf(cd, U.newline, from: lineStart)
                            if end < 0 { end = n }
                            continue
                        }
                        break
                    }
                    lineEnd = end
                }
                lineStart = lineEnd + 1
            }
        }
        return Stripped(noComments: nc, code: cd)
    }
}
