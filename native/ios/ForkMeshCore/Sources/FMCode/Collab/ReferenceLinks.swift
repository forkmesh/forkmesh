import FMApi
import FMBase
import Foundation

public enum ReferenceKind: String, Equatable, Sendable {
    case issue, pull, commit, url
}

public struct Reference: Equatable, Sendable {
    public let kind: ReferenceKind
    public let label: String
    public let repo: RepoRef?
    public let number: Int
    public let sha: String
    public let url: String

    public func route(in current: RepoRef) -> Route? {
        let target = repo ?? current
        switch kind {
        case .issue: return .issue(target, number: number)
        case .pull: return .pull(target, number: number, tab: .conversation)
        case .commit: return .commit(target, sha: sha)
        case .url: return nil
        }
    }

    public func link(in current: RepoRef) -> String {
        let target = repo ?? current
        switch kind {
        case .issue: return "forkmesh://\(target.owner)/\(target.name)/issues/\(number)"
        case .pull: return "forkmesh://\(target.owner)/\(target.name)/pulls/\(number)"
        case .commit: return "forkmesh://\(target.owner)/\(target.name)/commit/\(sha)"
        case .url: return url
        }
    }
}

public enum ReferenceSegment: Equatable, Sendable {
    case text(String)
    case reference(Reference)
}

public struct ReferenceRun: Equatable, Sendable {
    public let text: String
    public let url: String?

    public init(text: String, url: String?) {
        self.text = text
        self.url = url
    }
}

public enum ReferenceLinks {
    public static let chatRepo = RepoRef(owner: "forkmesh", name: "forkmesh")
    static let trailingPunctuation: Set<Character> = [".", ",", ";", ":", "!", "?"]

    static func isBoundary(_ chars: [Character], _ index: Int) -> Bool {
        guard index >= 0, index < chars.count else { return true }
        let c = chars[index]
        return !(c.isLetter || c.isNumber) && c != "_"
    }

    static func isHex(_ c: Character) -> Bool { c.isASCII && c.isHexDigit }

    static func digits(_ chars: [Character], from start: Int) -> Int {
        var end = start
        while end < chars.count, chars[end].isASCII, chars[end].isNumber { end += 1 }
        return end
    }

    static func hasPrefix(_ chars: [Character], _ at: Int, _ prefix: String, caseInsensitive: Bool) -> Bool {
        let want = Array(prefix)
        guard at + want.count <= chars.count else { return false }
        for (k, c) in want.enumerated() {
            let have = chars[at + k]
            if caseInsensitive ? have.lowercased() != c.lowercased() : have != c { return false }
        }
        return true
    }

    static func repoPrefix(_ chars: [Character], before hash: Int) -> (RepoRef, Int)? {
        var start = hash
        var slash = -1
        while start > 0 {
            let c = chars[start - 1]
            if c == "/" {
                if slash >= 0 { break }
                slash = start - 1
            } else if !(c.isLetter || c.isNumber || c == "-" || c == "_" || c == ".") {
                break
            }
            start -= 1
        }
        guard slash > start, slash < hash - 1, isBoundary(chars, start - 1) else { return nil }
        let owner = String(chars[start..<slash])
        let name = String(chars[(slash + 1)..<hash])
        guard let ref = RepoRef(slug: "\(owner)/\(name)") else { return nil }
        return (ref, start)
    }

    public static func segments(_ text: String, local: Bool = true, commits: Bool = true) -> [ReferenceSegment] {
        let chars = Array(text)
        var out: [ReferenceSegment] = []
        var plain = ""
        var fenced = false
        var lineStart = true
        var i = 0
        func flush() {
            if !plain.isEmpty { out.append(.text(plain)); plain = "" }
        }
        func emit(_ reference: Reference) {
            flush()
            out.append(.reference(reference))
        }
        while i < chars.count {
            let c = chars[i]
            if lineStart && hasPrefix(chars, i, "```", caseInsensitive: false) {
                fenced.toggle()
                plain += "```"
                i += 3
                lineStart = false
                continue
            }
            if c == "\n" {
                plain.append(c)
                i += 1
                lineStart = true
                continue
            }
            lineStart = false
            if c == "`" {
                var ticks = 0
                while i + ticks < chars.count && chars[i + ticks] == "`" { ticks += 1 }
                let delimiter = String(repeating: "`", count: ticks)
                let rest = String(chars[(i + ticks)...])
                if let close = rest.range(of: delimiter) {
                    let inner = rest.distance(from: rest.startIndex, to: close.lowerBound)
                    plain += String(chars[i..<(i + ticks + inner + ticks)])
                    i += ticks + inner + ticks
                } else {
                    plain += delimiter
                    i += ticks
                }
                continue
            }
            if fenced {
                plain.append(c)
                i += 1
                continue
            }
            if hasPrefix(chars, i, "https://", caseInsensitive: false) || hasPrefix(chars, i, "http://", caseInsensitive: false)
                || hasPrefix(chars, i, "forkmesh://", caseInsensitive: false)
            {
                var end = i
                while end < chars.count && !chars[end].isWhitespace { end += 1 }
                var linkEnd = end
                while linkEnd > i && trailingPunctuation.contains(chars[linkEnd - 1]) { linkEnd -= 1 }
                let url = String(chars[i..<linkEnd])
                emit(Reference(kind: .url, label: url, repo: nil, number: 0, sha: "", url: url))
                plain += String(chars[linkEnd..<end])
                i = end
                continue
            }
            if local && isBoundary(chars, i - 1) {
                var matched = false
                for (prefix, kind) in [("pull request #", ReferenceKind.pull), ("pull #", .pull), ("PR #", .pull), ("issue #", .issue)] {
                    guard hasPrefix(chars, i, prefix, caseInsensitive: true) else { continue }
                    let start = i + prefix.count
                    let end = digits(chars, from: start)
                    guard end > start, let number = Int(String(chars[start..<end])) else { continue }
                    emit(Reference(kind: kind, label: String(chars[i..<end]), repo: nil, number: number, sha: "", url: ""))
                    i = end
                    matched = true
                    break
                }
                if matched { continue }
            }
            if c == "#" {
                let end = digits(chars, from: i + 1)
                if end > i + 1, let number = Int(String(chars[(i + 1)..<end])) {
                    if let (repo, start) = repoPrefix(chars, before: i), plain.count >= i - start {
                        plain.removeLast(i - start)
                        emit(Reference(kind: .issue, label: String(chars[start..<end]), repo: repo, number: number, sha: "", url: ""))
                        i = end
                        continue
                    }
                    if local && isBoundary(chars, i - 1) {
                        emit(Reference(kind: .issue, label: String(chars[i..<end]), repo: nil, number: number, sha: "", url: ""))
                        i = end
                        continue
                    }
                }
            }
            if commits && local && isHex(c) && isBoundary(chars, i - 1) {
                var end = i
                while end < chars.count && isHex(chars[end]) { end += 1 }
                let length = end - i
                if length >= 7 && length <= 40 && isBoundary(chars, end) {
                    let sha = String(chars[i..<end])
                    emit(Reference(kind: .commit, label: sha, repo: nil, number: 0, sha: sha, url: ""))
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

    public static func references(_ text: String) -> [Reference] {
        segments(text).compactMap { if case .reference(let r) = $0 { return r } else { return nil } }
    }

    public static func runs(_ text: String, repo: RepoRef?, commits: Bool = true) -> [ReferenceRun] {
        var out: [ReferenceRun] = []
        func add(_ text: String, _ url: String?) {
            if url == nil, let last = out.last, last.url == nil {
                out[out.count - 1] = ReferenceRun(text: last.text + text, url: nil)
            } else {
                out.append(ReferenceRun(text: text, url: url))
            }
        }
        for segment in segments(text, local: repo != nil, commits: commits) {
            switch segment {
            case .text(let plain): add(plain, nil)
            case .reference(let ref):
                if ref.kind == .url {
                    add(ref.label, ref.url)
                } else if let target = ref.repo ?? repo {
                    add(ref.label, ref.link(in: target))
                } else {
                    add(ref.label, nil)
                }
            }
        }
        return out
    }

    public static func markdown(_ text: String, repo: RepoRef) -> String {
        segments(text).map { segment in
            switch segment {
            case .text(let plain): return plain
            case .reference(let ref):
                if ref.kind == .url && ref.label.hasPrefix("http") { return ref.label }
                return "[\(ref.label)](\(ref.link(in: repo)))"
            }
        }.joined()
    }
}
