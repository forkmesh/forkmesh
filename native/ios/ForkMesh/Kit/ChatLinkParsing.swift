// The parsing half of chat link text (ChatTextSegment, safeChatLink,
// parseChatText). Foundation only, so the rules can be exercised off-device;
// the view is in ChatLinkText.swift.

import Foundation

/// One run of a chat message: plain words, or words that point somewhere.
struct ChatTextSegment: Equatable, Hashable, CustomStringConvertible {
    /// What the reader sees.
    let text: String

    /// Where a tap goes, or nil for plain words.
    let url: String?

    init(_ text: String, _ url: String? = nil) {
        self.text = text
        self.url = url
    }

    var isLink: Bool {
        return url != nil
    }

    var description: String {
        guard let url = url else {
            return "text(\(text))"
        }
        return "link(\(text) -> \(url))"
    }
}

private let chatSafeSchemes: Set<String> = ["http", "https", "mailto"]

/// RFC 3986 scheme: a letter, then letters, digits, "+", "-" or ".".
private func isChatLinkScheme(_ text: String) -> Bool {
    var first: Bool = true
    for scalar in text.unicodeScalars {
        let value: UInt32 = scalar.value
        let letter: Bool = (value >= 0x41 && value <= 0x5a) || (value >= 0x61 && value <= 0x7a)
        let digit: Bool = value >= 0x30 && value <= 0x39
        let mark: Bool = value == 0x2b || value == 0x2d || value == 0x2e
        if first {
            if !letter {
                return false
            }
            first = false
        } else if !(letter || digit || mark) {
            return false
        }
    }
    return !first
}

/// The host of `//authority/path...`, without user info or port.
private func chatLinkHost(afterScheme rest: String) -> String {
    guard rest.hasPrefix("//") else {
        return ""
    }
    let authority: Substring = rest.dropFirst(2).prefix { (character: Character) -> Bool in
        return character != "/" && character != "?" && character != "#"
    }
    var host: String = String(authority)
    if let at = host.lastIndex(of: "@") {
        host = String(host[host.index(after: at)...])
    }
    if host.hasPrefix("[") {
        // An IPv6 literal keeps its colons; the port follows the bracket.
        if let close = host.firstIndex(of: "]") {
            host = String(host[...close])
        }
        return host
    }
    if let colon = host.lastIndex(of: ":") {
        host = String(host[..<colon])
    }
    return host
}

/// The address a chat message may hand to the browser, or an empty string.
/// Anything that could execute or read the device (`javascript:`, `file:`,
/// `data:`) is not a link; its label is shown as plain words instead, which is
/// what the web and desktop renderers do with it.
///
/// This returns the address as written with the scheme lower-cased and spaces
/// encoded; `URL(string:)` does the rest when it opens.
func safeChatLink(_ raw: String) -> String {
    var kept: String.UnicodeScalarView = String.UnicodeScalarView()
    for scalar in raw.unicodeScalars where scalar.value >= 0x20 && scalar.value != 0x7f {
        kept.append(scalar)
    }
    let candidate: String = String(kept).trimmingCharacters(in: .whitespacesAndNewlines)
    if candidate.isEmpty || candidate.utf16.count > 2048 {
        return ""
    }
    guard let colon = candidate.firstIndex(of: ":") else {
        return ""
    }
    let schemeText: String = String(candidate[..<colon])
    if !isChatLinkScheme(schemeText) {
        return ""
    }
    let scheme: String = schemeText.lowercased()
    if !chatSafeSchemes.contains(scheme) {
        return ""
    }
    let rest: String = String(candidate[candidate.index(after: colon)...])
    if scheme != "mailto" && chatLinkHost(afterScheme: rest).isEmpty {
        return ""
    }
    return (scheme + ":" + rest).replacingOccurrences(of: " ", with: "%20")
}

/// JavaScript's `\s`.
private func isChatWhitespace(_ scalar: Unicode.Scalar) -> Bool {
    let value: UInt32 = scalar.value
    if value == 0x20 || (value >= 0x09 && value <= 0x0d) {
        return true
    }
    if value == 0xa0 || value == 0x1680 || value == 0x2028 || value == 0x2029 {
        return true
    }
    if value >= 0x2000 && value <= 0x200a {
        return true
    }
    return value == 0x202f || value == 0x205f || value == 0x3000 || value == 0xfeff
}

private func chatScalarsMatch(_ chars: [Unicode.Scalar], at index: Int, lowercased word: String) -> Bool {
    var position: Int = index
    for expected in word.unicodeScalars {
        if position >= chars.count {
            return false
        }
        var value: UInt32 = chars[position].value
        if value >= 0x41 && value <= 0x5a {
            value += 0x20
        }
        if value != expected.value {
            return false
        }
        position += 1
    }
    return true
}

/// The bare URL starting at `index` (`https?://[^\s<>]+`, case-insensitive),
/// without the sentence punctuation trailing it. A closing parenthesis is part
/// of the address only when the address opened one: ".../Fork_(software)" is a
/// page, "(see .../status)" is a sentence wrapped around a link.
private func chatRawUrl(_ chars: [Unicode.Scalar], at index: Int) -> [Unicode.Scalar] {
    var prefix: Int = 0
    if chatScalarsMatch(chars, at: index, lowercased: "https://") {
        prefix = 8
    } else if chatScalarsMatch(chars, at: index, lowercased: "http://") {
        prefix = 7
    } else {
        return []
    }
    var end: Int = index + prefix
    while end < chars.count {
        let scalar: Unicode.Scalar = chars[end]
        if isChatWhitespace(scalar) || scalar == "<" || scalar == ">" {
            break
        }
        end += 1
    }
    if end == index + prefix {
        return []
    }
    var url: [Unicode.Scalar] = Array(chars[index..<end])
    let trailing: Set<Unicode.Scalar> = [",", ".", "!", "?", ";", ":"]
    while let last = url.last {
        if trailing.contains(last) {
            url.removeLast()
            continue
        }
        if last == ")" {
            let closes: Int = url.filter { $0 == ")" }.count
            let opens: Int = url.filter { $0 == "(" }.count
            if closes > opens {
                url.removeLast()
                continue
            }
        }
        break
    }
    return url
}

/// Index of the ")" closing a `[label](target)` target opening at `start`,
/// counting nested parentheses, or -1 when the link never closes.
private func chatLinkTargetEnd(_ chars: [Unicode.Scalar], from start: Int) -> Int {
    var depth: Int = 0
    var index: Int = start
    while index < chars.count {
        let scalar: Unicode.Scalar = chars[index]
        if scalar == "\n" {
            return -1 // a link never spans lines
        }
        if scalar == "(" {
            depth += 1
        } else if scalar == ")" {
            if depth == 0 {
                return index
            }
            depth -= 1
        }
        index += 1
    }
    return -1
}

/// Index of the first "](" at or after `start`, or -1.
private func chatLabelEnd(_ chars: [Unicode.Scalar], from start: Int) -> Int {
    var index: Int = start
    while index + 1 < chars.count {
        if chars[index] == "]" && chars[index + 1] == "(" {
            return index
        }
        index += 1
    }
    return -1
}

private func chatString(_ chars: [Unicode.Scalar], _ range: Range<Int>) -> String {
    var view: String.UnicodeScalarView = String.UnicodeScalarView()
    view.append(contentsOf: chars[range])
    return String(view)
}

/// Splits a chat message into the runs a transcript should paint.
///
/// Both shapes people write are resolved: a pasted http(s) URL and markdown's
/// `[label](url)`. The rules match the web renderer (chat-rich-text.js) and the
/// desktop one (ChatLinkText.h) so one message reads the same on every client.
func parseChatText(_ text: String) -> [ChatTextSegment] {
    let chars: [Unicode.Scalar] = Array(text.unicodeScalars)
    var segments: [ChatTextSegment] = []
    var buffer: String.UnicodeScalarView = String.UnicodeScalarView()

    func flush() {
        if buffer.isEmpty {
            return
        }
        segments.append(ChatTextSegment(String(buffer)))
        buffer = String.UnicodeScalarView()
    }

    var index: Int = 0
    while index < chars.count {
        if chars[index] == "[" {
            let labelEnd: Int = chatLabelEnd(chars, from: index + 1)
            var targetEnd: Int = -1
            if labelEnd > index + 1 && !chars[index..<labelEnd].contains("\n") {
                targetEnd = chatLinkTargetEnd(chars, from: labelEnd + 2)
            }
            if targetEnd > labelEnd + 1 {
                let label: String = chatString(chars, (index + 1)..<labelEnd)
                let href: String = safeChatLink(chatString(chars, (labelEnd + 2)..<targetEnd))
                if href.isEmpty {
                    // A target we will not open: show the words, not a dead link.
                    buffer.append(contentsOf: label.unicodeScalars)
                } else {
                    flush()
                    segments.append(ChatTextSegment(label, href))
                }
                index = targetEnd + 1
                continue
            }
        }
        let rawUrl: [Unicode.Scalar] = chatRawUrl(chars, at: index)
        if !rawUrl.isEmpty {
            let rawText: String = chatString(rawUrl, 0..<rawUrl.count)
            let rawHref: String = safeChatLink(rawText)
            if !rawHref.isEmpty {
                flush()
                segments.append(ChatTextSegment(rawText, rawHref))
                index += rawUrl.count
                continue
            }
        }
        buffer.append(chars[index])
        index += 1
    }
    flush()
    return segments
}
