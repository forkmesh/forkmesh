import Foundation

public enum RSSEntities {
    public static let named: [String: UInt32] = [
        "quot": 34, "amp": 38, "apos": 39, "lt": 60, "gt": 62, "nbsp": 160, "iexcl": 161, "cent": 162, "pound": 163,
        "curren": 164, "yen": 165, "brvbar": 166, "sect": 167, "uml": 168, "copy": 169, "ordf": 170, "laquo": 171,
        "not": 172, "shy": 173, "reg": 174, "macr": 175, "deg": 176, "plusmn": 177, "sup2": 178, "sup3": 179,
        "acute": 180, "micro": 181, "para": 182, "middot": 183, "cedil": 184, "sup1": 185, "ordm": 186, "raquo": 187,
        "frac14": 188, "frac12": 189, "frac34": 190, "iquest": 191, "Agrave": 192, "Aacute": 193, "Acirc": 194,
        "Atilde": 195, "Auml": 196, "Aring": 197, "AElig": 198, "Ccedil": 199, "Egrave": 200, "Eacute": 201,
        "Ecirc": 202, "Euml": 203, "Igrave": 204, "Iacute": 205, "Icirc": 206, "Iuml": 207, "ETH": 208, "Ntilde": 209,
        "Ograve": 210, "Oacute": 211, "Ocirc": 212, "Otilde": 213, "Ouml": 214, "times": 215, "Oslash": 216,
        "Ugrave": 217, "Uacute": 218, "Ucirc": 219, "Uuml": 220, "Yacute": 221, "THORN": 222, "szlig": 223,
        "agrave": 224, "aacute": 225, "acirc": 226, "atilde": 227, "auml": 228, "aring": 229, "aelig": 230,
        "ccedil": 231, "egrave": 232, "eacute": 233, "ecirc": 234, "euml": 235, "igrave": 236, "iacute": 237,
        "icirc": 238, "iuml": 239, "eth": 240, "ntilde": 241, "ograve": 242, "oacute": 243, "ocirc": 244,
        "otilde": 245, "ouml": 246, "divide": 247, "oslash": 248, "ugrave": 249, "uacute": 250, "ucirc": 251,
        "uuml": 252, "yacute": 253, "thorn": 254, "yuml": 255, "OElig": 338, "oelig": 339, "Scaron": 352,
        "scaron": 353, "Yuml": 376, "fnof": 402, "circ": 710, "tilde": 732, "Alpha": 913, "Beta": 914, "Gamma": 915,
        "Delta": 916, "Epsilon": 917, "Zeta": 918, "Eta": 919, "Theta": 920, "Iota": 921, "Kappa": 922,
        "Lambda": 923, "Mu": 924, "Nu": 925, "Xi": 926, "Omicron": 927, "Pi": 928, "Rho": 929, "Sigma": 931,
        "Tau": 932, "Upsilon": 933, "Phi": 934, "Chi": 935, "Psi": 936, "Omega": 937, "alpha": 945, "beta": 946,
        "gamma": 947, "delta": 948, "epsilon": 949, "zeta": 950, "eta": 951, "theta": 952, "iota": 953,
        "kappa": 954, "lambda": 955, "mu": 956, "nu": 957, "xi": 958, "omicron": 959, "pi": 960, "rho": 961,
        "sigmaf": 962, "sigma": 963, "tau": 964, "upsilon": 965, "phi": 966, "chi": 967, "psi": 968, "omega": 969,
        "thetasym": 977, "upsih": 978, "piv": 982, "ensp": 8194, "emsp": 8195, "thinsp": 8201, "zwnj": 8204,
        "zwj": 8205, "lrm": 8206, "rlm": 8207, "ndash": 8211, "mdash": 8212, "lsquo": 8216, "rsquo": 8217,
        "sbquo": 8218, "ldquo": 8220, "rdquo": 8221, "bdquo": 8222, "dagger": 8224, "Dagger": 8225, "bull": 8226,
        "hellip": 8230, "permil": 8240, "prime": 8242, "Prime": 8243, "lsaquo": 8249, "rsaquo": 8250,
        "oline": 8254, "frasl": 8260, "euro": 8364, "image": 8465, "weierp": 8472, "real": 8476, "trade": 8482,
        "alefsym": 8501, "larr": 8592, "uarr": 8593, "rarr": 8594, "darr": 8595, "harr": 8596, "crarr": 8629,
        "lArr": 8656, "uArr": 8657, "rArr": 8658, "dArr": 8659, "hArr": 8660, "forall": 8704, "part": 8706,
        "exist": 8707, "empty": 8709, "nabla": 8711, "isin": 8712, "notin": 8713, "ni": 8715, "prod": 8719,
        "sum": 8721, "minus": 8722, "lowast": 8727, "radic": 8730, "prop": 8733, "infin": 8734, "ang": 8736,
        "and": 8743, "or": 8744, "cap": 8745, "cup": 8746, "int": 8747, "there4": 8756, "sim": 8764, "cong": 8773,
        "asymp": 8776, "ne": 8800, "equiv": 8801, "le": 8804, "ge": 8805, "sub": 8834, "sup": 8835, "nsub": 8836,
        "sube": 8838, "supe": 8839, "oplus": 8853, "otimes": 8855, "perp": 8869, "sdot": 8901, "lceil": 8968,
        "rceil": 8969, "lfloor": 8970, "rfloor": 8971, "lang": 9001, "rang": 9002, "loz": 9674, "spades": 9824,
        "clubs": 9827, "hearts": 9829, "diams": 9830, "hyphen": 8208, "dash": 8208, "nbhy": 8209,
    ]

    static let legacy: Set<String> = ["amp", "lt", "gt", "quot", "nbsp", "copy", "reg"]

    static let windows1252: [UInt32: UInt32] = [
        0x80: 0x20AC, 0x82: 0x201A, 0x83: 0x0192, 0x84: 0x201E, 0x85: 0x2026, 0x86: 0x2020, 0x87: 0x2021,
        0x88: 0x02C6, 0x89: 0x2030, 0x8A: 0x0160, 0x8B: 0x2039, 0x8C: 0x0152, 0x8E: 0x017D, 0x91: 0x2018,
        0x92: 0x2019, 0x93: 0x201C, 0x94: 0x201D, 0x95: 0x2022, 0x96: 0x2013, 0x97: 0x2014, 0x98: 0x02DC,
        0x99: 0x2122, 0x9A: 0x0161, 0x9B: 0x203A, 0x9C: 0x0153, 0x9E: 0x017E, 0x9F: 0x0178,
    ]

    static func scalar(forNumber number: UInt32) -> Unicode.Scalar {
        if let mapped = windows1252[number], let scalar = Unicode.Scalar(mapped) { return scalar }
        if number == 0 || number > 0x10FFFF || (0xD800...0xDFFF).contains(number) { return "\u{FFFD}" }
        return Unicode.Scalar(number) ?? "\u{FFFD}"
    }

    public static func decode(_ text: String) -> String {
        guard text.contains("&") else { return text }
        let scalars = Array(text.unicodeScalars)
        var out = String.UnicodeScalarView()
        var index = 0
        let count = scalars.count
        while index < count {
            let scalar = scalars[index]
            guard scalar == "&" else {
                out.append(scalar)
                index += 1
                continue
            }
            var cursor = index + 1
            if cursor < count, scalars[cursor] == "#" {
                cursor += 1
                var hex = false
                if cursor < count, scalars[cursor] == "x" || scalars[cursor] == "X" {
                    hex = true
                    cursor += 1
                }
                let digitsStart = cursor
                while cursor < count {
                    let value = scalars[cursor]
                    let isDigit = hex ? value.properties.isASCIIHexDigit : ("0"..."9").contains(value)
                    if !isDigit { break }
                    cursor += 1
                }
                if cursor > digitsStart {
                    var digits = String.UnicodeScalarView()
                    digits.append(contentsOf: scalars[digitsStart..<cursor])
                    let number = UInt32(String(digits).prefix(8), radix: hex ? 16 : 10) ?? 0x110000
                    out.append(Self.scalar(forNumber: number))
                    if cursor < count, scalars[cursor] == ";" { cursor += 1 }
                    index = cursor
                    continue
                }
                out.append(scalar)
                index += 1
                continue
            }
            let nameStart = cursor
            while cursor < count, cursor - nameStart < 32, scalars[cursor].properties.isAlphabetic || ("0"..."9").contains(scalars[cursor]) {
                cursor += 1
            }
            var nameView = String.UnicodeScalarView()
            nameView.append(contentsOf: scalars[nameStart..<cursor])
            let name = String(nameView)
            if cursor < count, scalars[cursor] == ";", let code = named[name], let decoded = Unicode.Scalar(code) {
                out.append(decoded)
                index = cursor + 1
                continue
            }
            var matched = false
            var length = name.count
            while length >= 2 {
                let candidate = String(name.prefix(length))
                if legacy.contains(candidate), let code = named[candidate], let decoded = Unicode.Scalar(code) {
                    out.append(decoded)
                    index = nameStart + length
                    matched = true
                    break
                }
                length -= 1
            }
            if !matched {
                out.append(scalar)
                index += 1
            }
        }
        return String(out)
    }
}

public enum RSSHTMLToken: Equatable, Sendable {
    case start(name: String, attributes: [RSSHTMLAttribute], selfClosing: Bool)
    case end(name: String)
    case text(String)
}

public struct RSSHTMLAttribute: Equatable, Sendable {
    public var name: String
    public var value: String

    public init(_ name: String, _ value: String) {
        self.name = name
        self.value = value
    }
}

public enum RSSHTMLTokenizer {
    static let rawText: Set<String> = ["script", "style"]

    static func isSpace(_ byte: UInt8) -> Bool { byte == 0x20 || byte == 0x09 || byte == 0x0A || byte == 0x0D || byte == 0x0C }

    static func isLetter(_ byte: UInt8) -> Bool { (0x41...0x5A).contains(byte) || (0x61...0x7A).contains(byte) }

    static func lower(_ bytes: ArraySlice<UInt8>) -> String {
        String(decoding: bytes.map { (0x41...0x5A).contains($0) ? $0 + 32 : $0 }, as: UTF8.self)
    }

    static func find(_ needle: [UInt8], in bytes: [UInt8], from start: Int, caseInsensitive: Bool = false) -> Int? {
        guard !needle.isEmpty, bytes.count >= needle.count else { return nil }
        var index = start
        let last = bytes.count - needle.count
        while index <= last {
            var hit = true
            for offset in 0..<needle.count {
                var byte = bytes[index + offset]
                if caseInsensitive && (0x41...0x5A).contains(byte) { byte += 32 }
                if byte != needle[offset] {
                    hit = false
                    break
                }
            }
            if hit { return index }
            index += 1
        }
        return nil
    }

    public static func tokens(_ html: String) -> [RSSHTMLToken] {
        let bytes = Array(html.utf8)
        var tokens: [RSSHTMLToken] = []
        var text: [UInt8] = []
        var index = 0
        let count = bytes.count

        func flush() {
            if !text.isEmpty {
                tokens.append(.text(RSSEntities.decode(String(decoding: text, as: UTF8.self))))
                text.removeAll(keepingCapacity: true)
            }
        }

        while index < count {
            let byte = bytes[index]
            if byte != 0x3C {
                text.append(byte)
                index += 1
                continue
            }
            let next = index + 1 < count ? bytes[index + 1] : 0
            if next == 0x21 {
                if index + 3 < count, bytes[index + 2] == 0x2D, bytes[index + 3] == 0x2D {
                    flush()
                    if let close = find(Array("-->".utf8), in: bytes, from: index + 4) {
                        index = close + 3
                    } else {
                        index = count
                    }
                    continue
                }
                flush()
                if let close = bytes[index...].firstIndex(of: 0x3E) { index = close + 1 } else { index = count }
                continue
            }
            if next == 0x3F {
                flush()
                if let close = bytes[index...].firstIndex(of: 0x3E) { index = close + 1 } else { index = count }
                continue
            }
            if next == 0x2F {
                let nameStart = index + 2
                if nameStart < count, isLetter(bytes[nameStart]) {
                    var cursor = nameStart
                    while cursor < count, !isSpace(bytes[cursor]), bytes[cursor] != 0x3E, bytes[cursor] != 0x2F { cursor += 1 }
                    let name = lower(bytes[nameStart..<cursor])
                    guard let close = bytes[cursor...].firstIndex(of: 0x3E) else {
                        text.append(contentsOf: bytes[index...])
                        index = count
                        continue
                    }
                    flush()
                    tokens.append(.end(name: name))
                    index = close + 1
                    continue
                }
                if nameStart < count, bytes[nameStart] == 0x3E {
                    index = nameStart + 1
                    continue
                }
                text.append(byte)
                index += 1
                continue
            }
            guard isLetter(next) else {
                text.append(byte)
                index += 1
                continue
            }
            var cursor = index + 1
            while cursor < count, !isSpace(bytes[cursor]), bytes[cursor] != 0x3E, bytes[cursor] != 0x2F, bytes[cursor] != 0 {
                cursor += 1
            }
            let name = lower(bytes[(index + 1)..<cursor])
            var attributes: [RSSHTMLAttribute] = []
            var selfClosing = false
            var closed = false
            while cursor < count {
                while cursor < count, isSpace(bytes[cursor]) { cursor += 1 }
                guard cursor < count else { break }
                if bytes[cursor] == 0x3E {
                    closed = true
                    cursor += 1
                    break
                }
                if bytes[cursor] == 0x2F {
                    if cursor + 1 < count, bytes[cursor + 1] == 0x3E {
                        selfClosing = true
                        closed = true
                        cursor += 2
                        break
                    }
                    cursor += 1
                    continue
                }
                let attrStart = cursor
                cursor += 1
                while cursor < count, !isSpace(bytes[cursor]), bytes[cursor] != 0x3E, bytes[cursor] != 0x2F, bytes[cursor] != 0x3D {
                    cursor += 1
                }
                let attrName = lower(bytes[attrStart..<cursor])
                var probe = cursor
                while probe < count, isSpace(bytes[probe]) { probe += 1 }
                var value = ""
                if probe < count, bytes[probe] == 0x3D {
                    probe += 1
                    while probe < count, isSpace(bytes[probe]) { probe += 1 }
                    if probe < count, bytes[probe] == 0x22 || bytes[probe] == 0x27 {
                        let quote = bytes[probe]
                        let valueStart = probe + 1
                        var valueEnd = valueStart
                        while valueEnd < count, bytes[valueEnd] != quote { valueEnd += 1 }
                        value = RSSEntities.decode(String(decoding: bytes[valueStart..<min(valueEnd, count)], as: UTF8.self))
                        cursor = min(valueEnd + 1, count)
                    } else {
                        let valueStart = probe
                        var valueEnd = valueStart
                        while valueEnd < count, !isSpace(bytes[valueEnd]), bytes[valueEnd] != 0x3E { valueEnd += 1 }
                        value = RSSEntities.decode(String(decoding: bytes[valueStart..<valueEnd], as: UTF8.self))
                        cursor = valueEnd
                    }
                }
                if !attrName.isEmpty { attributes.append(RSSHTMLAttribute(attrName, value)) }
            }
            if !closed {
                text.append(contentsOf: bytes[index...])
                index = count
                continue
            }
            flush()
            tokens.append(.start(name: name, attributes: attributes, selfClosing: selfClosing))
            index = cursor
            if rawText.contains(name) && !selfClosing {
                let needle = Array("</\(name)".utf8)
                if let close = find(needle, in: bytes, from: index, caseInsensitive: true) {
                    if close > index {
                        tokens.append(.text(String(decoding: bytes[index..<close], as: UTF8.self)))
                    }
                    if let end = bytes[close...].firstIndex(of: 0x3E) {
                        tokens.append(.end(name: name))
                        index = end + 1
                    } else {
                        index = count
                    }
                } else {
                    tokens.append(.text(String(decoding: bytes[index...], as: UTF8.self)))
                    index = count
                }
            }
        }
        flush()
        return tokens
    }
}

public enum RSSHTML {
    public static let maxContentChars = 200_000
    public static let maxSummaryChars = 300

    public static let voidTags: Set<String> = ["br", "hr", "img", "source", "wbr"]
    public static let allowedTags: Set<String> = [
        "a", "abbr", "b", "blockquote", "br", "caption", "cite", "code", "dd", "del",
        "details", "dfn", "div", "dl", "dt", "em", "figcaption", "figure", "h1", "h2",
        "h3", "h4", "h5", "h6", "hr", "i", "img", "ins", "kbd", "li", "mark", "ol", "p",
        "pre", "q", "s", "samp", "section", "small", "span", "strike", "strong", "sub",
        "summary", "sup", "table", "tbody", "td", "tfoot", "th", "thead", "time", "tr",
        "u", "ul", "audio", "video", "source", "wbr", "article", "aside", "header",
        "footer", "main",
    ]
    public static let droppedTags: Set<String> = [
        "script", "style", "template", "iframe", "object", "embed", "svg", "math",
        "head", "title", "textarea", "select", "button", "noscript", "canvas", "frame",
        "frameset", "applet", "noembed", "noframes", "dialog", "nav",
    ]
    public static let allowedAttributes: [String: [String]] = [
        "a": ["href", "title"],
        "img": ["src", "alt", "title", "width", "height"],
        "td": ["colspan", "rowspan"],
        "th": ["colspan", "rowspan", "scope"],
        "ol": ["start", "reversed"],
        "time": ["datetime"],
        "abbr": ["title"],
        "audio": ["src", "controls"],
        "video": ["src", "controls", "poster", "width", "height"],
        "source": ["src", "type"],
        "details": ["open"],
    ]
    static let urlAttributes: Set<String> = ["href", "src", "poster"]
    static let lazySources = ["data-src", "data-lazy-src", "data-original", "data-orig-file"]

    struct Sanitizer {
        let base: String
        let remoteMedia: Bool
        var upgradeMedia = false
        var altCaptions = false
        var captioned: Set<Int> = []
        var figures: [Int] = []
        var position = 0
        var out = ""
        var stack: [String] = []
        var drop: [String] = []
        var size = 0

        mutating func emit(_ text: String) {
            if size < maxContentChars {
                out += text
                size += RSSText.length(text)
            }
        }

        func attributes(_ tag: String, _ attrs: [RSSHTMLAttribute]) -> [(String, String)] {
            var values: [String: String] = [:]
            for attr in attrs { values[attr.name] = attr.value }
            if tag == "img" {
                let current = values["src"] ?? ""
                if current.isEmpty || current.hasPrefix("data:") {
                    for key in lazySources {
                        if let lazy = values[key], !lazy.isEmpty {
                            values["src"] = lazy
                            break
                        }
                    }
                }
            }
            var kept: [(String, String)] = []
            for name in allowedAttributes[tag] ?? [] {
                guard var value = values[name] else { continue }
                if urlAttributes.contains(name) {
                    if name == "href" && RSSText.strip(value).lowercased().hasPrefix("mailto:") {
                        value = RSSText.strip(value)
                    } else {
                        value = RSSURL.http(value, base: base)
                        if upgradeMedia && name != "href" { value = RSSURL.upgraded(value) }
                    }
                    if value.isEmpty { continue }
                } else if ["width", "height", "colspan", "rowspan", "start"].contains(name) {
                    let trimmed = RSSText.strip(value)
                    if trimmed.isEmpty || !trimmed.unicodeScalars.allSatisfy({ $0.properties.numericType == .decimal }) { continue }
                    value = RSSText.prefix(trimmed, 6)
                } else if ["controls", "reversed", "open"].contains(name) {
                    value = ""
                } else {
                    value = RSSText.clean(value, 300)
                }
                kept.append((name, value))
            }
            if tag == "a" {
                kept.append(("rel", "noopener noreferrer"))
                kept.append(("target", "_blank"))
            }
            if tag == "img" {
                kept.append(("loading", "lazy"))
                kept.append(("referrerpolicy", "no-referrer"))
            }
            if (tag == "audio" || tag == "video") && !kept.contains(where: { $0.0 == "controls" }) {
                kept.append(("controls", ""))
            }
            return kept
        }

        static func ignored(_ attrs: [RSSHTMLAttribute]) -> Bool {
            guard let value = attrs.first(where: { $0.name == "class" })?.value else { return false }
            return value.split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\n" || $0 == "\r" || $0 == "\u{0C}" })
                .contains { $0 == "instapaper_ignore" }
        }

        mutating func start(_ tag: String, _ attrs: [RSSHTMLAttribute]) {
            if !drop.isEmpty {
                if tag == drop.last && !voidTags.contains(tag) { drop.append(tag) }
                return
            }
            if Self.ignored(attrs) {
                if !voidTags.contains(tag) { drop.append(tag) }
                return
            }
            if droppedTags.contains(tag) {
                if tag == "iframe" {
                    let src = RSSURL.http(attrs.first { $0.name == "src" }?.value ?? "", base: base)
                    if !src.isEmpty {
                        emit("<p><a href=\"\(RSSText.escape(src, quote: true))\" rel=\"noopener noreferrer\" target=\"_blank\">Open embedded content</a></p>")
                    }
                }
                drop.append(tag)
                return
            }
            guard allowedTags.contains(tag) else { return }
            let kept = attributes(tag, attrs)
            if (tag == "img" || tag == "source") && !kept.contains(where: { $0.0 == "src" }) { return }
            if !remoteMedia && ["img", "audio", "video", "source"].contains(tag) {
                let src = kept.first { $0.0 == "src" }?.1 ?? ""
                if tag != "source" && !src.isEmpty {
                    let alt = kept.first { $0.0 == "alt" }?.1 ?? ""
                    let label = tag == "img" ? (alt.isEmpty ? "Image" : "Image: \(alt)") : "Open \(tag)"
                    emit("<p><a href=\"\(RSSText.escape(src, quote: true))\" rel=\"noopener noreferrer\" target=\"_blank\">\(RSSText.escape(label, quote: false))</a></p>")
                }
                if tag == "audio" || tag == "video" {
                    emit("<span>")
                    stack.append("span")
                    mediaSpans.append(tag)
                }
                return
            }
            var rendered = ""
            for (name, value) in kept {
                if !value.isEmpty || name == "alt" {
                    rendered += " \(name)=\"\(RSSText.escape(value, quote: true))\""
                } else {
                    rendered += " \(name)"
                }
            }
            emit("<\(tag)\(rendered)>")
            if !voidTags.contains(tag) { stack.append(tag) }
            if tag == "figure" { figures.append(position) }
            if tag == "img" && altCaptions && !figures.contains(where: captioned.contains) {
                let alt = kept.first { $0.0 == "alt" }?.1 ?? ""
                if !alt.isEmpty { emit("<span class=\"alt-caption\">\(RSSText.escape(alt, quote: false))</span>") }
            }
        }

        var mediaSpans: [String] = []

        mutating func end(_ rawTag: String) {
            var tag = rawTag
            if !drop.isEmpty {
                if tag == drop.last { drop.removeLast() }
                return
            }
            if !remoteMedia && (tag == "audio" || tag == "video") {
                guard mediaSpans.last == tag else { return }
                mediaSpans.removeLast()
                tag = "span"
            }
            guard stack.contains(tag) else { return }
            while let open = stack.popLast() {
                emit("</\(open)>")
                if open == "figure" { _ = figures.popLast() }
                if open == tag { break }
            }
        }

        mutating func text(_ data: String) {
            if drop.isEmpty { emit(RSSText.escape(data, quote: false)) }
        }

        mutating func result() -> String {
            while let open = stack.popLast() { emit("</\(open)>") }
            return RSSText.strip(out)
        }
    }

    static func captionedFigures(_ tokens: [RSSHTMLToken]) -> Set<Int> {
        var open: [Int] = []
        var found: Set<Int> = []
        for (index, token) in tokens.enumerated() {
            switch token {
            case let .start(name, _, selfClosing):
                if name == "figure" && !selfClosing { open.append(index) }
                if name == "figcaption", let figure = open.last { found.insert(figure) }
            case let .end(name):
                if name == "figure" { _ = open.popLast() }
            case .text:
                break
            }
        }
        return found
    }

    public static func sanitize(
        _ html: String, base: String = "", remoteMedia: Bool = true, upgradeMedia: Bool = false, altCaptions: Bool = false
    ) -> String {
        if RSSText.strip(html).isEmpty { return "" }
        var sanitizer = Sanitizer(base: base, remoteMedia: remoteMedia, upgradeMedia: upgradeMedia)
        let tokens = RSSHTMLTokenizer.tokens(RSSText.prefix(html, maxContentChars * 2))
        if altCaptions && remoteMedia {
            sanitizer.altCaptions = true
            sanitizer.captioned = captionedFigures(tokens)
        }
        for (index, token) in tokens.enumerated() {
            sanitizer.position = index
            switch token {
            case let .start(name, attributes, selfClosing):
                sanitizer.start(name, attributes)
                if selfClosing && sanitizer.drop.isEmpty && !voidTags.contains(name) && sanitizer.stack.last == name {
                    sanitizer.end(name)
                }
            case let .end(name):
                sanitizer.end(name)
            case let .text(data):
                sanitizer.text(data)
            }
        }
        return RSSText.prefix(sanitizer.result(), maxContentChars + 2000)
    }

    static let textBlocks: Set<String> = [
        "p", "div", "br", "li", "h1", "h2", "h3", "h4", "h5", "h6", "tr", "blockquote", "pre", "section", "article",
    ]

    public static func plainText(_ value: String, limit: Int = maxSummaryChars) -> String {
        var text = value
        if text.contains("<") || text.contains("&") {
            var parts: [String] = []
            var drop = 0
            for token in RSSHTMLTokenizer.tokens(RSSText.prefix(text, 400_000)) {
                switch token {
                case let .start(name, _, _):
                    if droppedTags.contains(name) {
                        drop += 1
                    } else if textBlocks.contains(name) {
                        parts.append(" ")
                    }
                case let .end(name):
                    if droppedTags.contains(name) && drop > 0 {
                        drop -= 1
                    } else if textBlocks.contains(name) {
                        parts.append(" ")
                    }
                case let .text(data):
                    if drop == 0 { parts.append(data) }
                }
            }
            text = parts.joined()
        }
        text = RSSText.clean(text, Int.max)
        if RSSText.length(text) <= limit { return text }
        let head = RSSText.prefix(text, limit)
        var cut = head
        if let space = head.lastIndex(of: " ") {
            let before = String(head[..<space])
            cut = before.isEmpty ? head : before
        }
        return cut + "\u{2026}"
    }

    public static func plainToHTML(_ text: String) -> String {
        let normalized = text.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
        var paragraphs: [[String]] = []
        var current: [String] = []
        for line in normalized.components(separatedBy: "\n") {
            if RSSText.strip(line).isEmpty {
                if !current.isEmpty { paragraphs.append(current) }
                current = []
            } else {
                current.append(RSSText.strip(line))
            }
        }
        if !current.isEmpty { paragraphs.append(current) }
        return paragraphs.map { "<p>" + $0.map(linkify).joined(separator: "<br>") + "</p>" }.joined()
    }

    static let trailingPunctuation: Set<Character> = [".", ",", ";", ":", "!", "?", ")", "]", "'", "\""]

    static func nextURL(_ text: Substring) -> Substring.Index? {
        let secure = text.range(of: "https://", options: .caseInsensitive)?.lowerBound
        let plain = text.range(of: "http://", options: .caseInsensitive)?.lowerBound
        switch (secure, plain) {
        case let (.some(left), .some(right)): return min(left, right)
        case let (.some(left), nil): return left
        case let (nil, .some(right)): return right
        default: return nil
        }
    }

    public static func linkify(_ line: String) -> String {
        var out = ""
        var rest = Substring(line)
        while let start = nextURL(rest) {
            var end = start
            while end < rest.endIndex, !rest[end].isWhitespace, rest[end] != "<", rest[end] != ">", rest[end] != "\"" {
                end = rest.index(after: end)
            }
            while end > start, trailingPunctuation.contains(rest[rest.index(before: end)]) { end = rest.index(before: end) }
            let candidate = String(rest[start..<end])
            let url = RSSURL.http(candidate)
            out += RSSText.escape(String(rest[..<start]), quote: false)
            if url.isEmpty {
                out += RSSText.escape(candidate, quote: false)
            } else {
                out += "<a href=\"\(RSSText.escape(url, quote: true))\">\(RSSText.escape(candidate, quote: false))</a>"
            }
            rest = rest[end...]
        }
        return out + RSSText.escape(String(rest), quote: false)
    }

    public static func firstImage(_ content: String) -> String {
        for token in RSSHTMLTokenizer.tokens(content) {
            if case let .start(name, attributes, _) = token, name == "img",
               let src = attributes.first(where: { $0.name == "src" })?.value, !src.isEmpty {
                return src
            }
        }
        return ""
    }
}

public struct RSSDiscoveredLink: Equatable, Sendable {
    public var url: String
    public var title: String
    public var type: String
}

public struct RSSDiscovery: Equatable, Sendable {
    public var feeds: [RSSDiscoveredLink]
    public var icon: String
    public var title: String
}

public enum RSSFeedTypes {
    public static let feedTypes: Set<String> = [
        "application/rss+xml", "application/atom+xml", "application/feed+json",
        "application/json", "application/rdf+xml", "application/xml", "text/xml",
    ]
    public static let accept =
        "application/rss+xml, application/atom+xml, application/feed+json, application/json;q=0.9, application/xml;q=0.9, text/xml;q=0.9, text/html;q=0.8, */*;q=0.5"
    public static let guessPaths = ["/feed", "/rss", "/feed.xml", "/atom.xml", "/rss/"]

    public static func discover(_ document: String, base: String) -> RSSDiscovery {
        var feeds: [(String, String, String)] = []
        var icons: [(Bool, String)] = []
        var title = ""
        var inTitle = false
        var baseHref = ""
        for token in RSSHTMLTokenizer.tokens(RSSText.prefix(document, 600_000)) {
            switch token {
            case let .start(name, attributes, _):
                var values: [String: String] = [:]
                for attr in attributes { values[attr.name] = attr.value }
                if name == "base", let href = values["href"], !href.isEmpty, baseHref.isEmpty { baseHref = href }
                if name == "title" && title.isEmpty { inTitle = true }
                guard name == "link" else { continue }
                let rel = (values["rel"] ?? "").lowercased().split(whereSeparator: { $0 == " " || $0 == "\t" || $0 == "\n" }).map(String.init)
                let kind = RSSText.strip(String((values["type"] ?? "").lowercased().split(separator: ";", omittingEmptySubsequences: false).first ?? ""))
                let href = values["href"] ?? ""
                if href.isEmpty { continue }
                if rel.contains("alternate") && feedTypes.contains(kind) {
                    feeds.append((kind, href, RSSText.clean(values["title"] ?? "", 200)))
                } else if rel.contains("icon") || rel.contains("apple-touch-icon") {
                    icons.append((rel.contains("apple-touch-icon"), href))
                }
            case let .end(name):
                if name == "title" { inTitle = false }
            case let .text(data):
                if inTitle { title = RSSText.clean(title + data, 200) }
            }
        }
        let root = RSSURL.http(baseHref, base: base).isEmpty ? base : RSSURL.http(baseHref, base: base)
        let order: [String: Int] = [
            "application/rss+xml": 0, "application/atom+xml": 1, "application/feed+json": 2,
            "application/json": 3, "application/rdf+xml": 4,
        ]
        var found: [RSSDiscoveredLink] = []
        let sorted = feeds.enumerated().sorted { lhs, rhs in
            let left = order[lhs.element.0] ?? 5
            let right = order[rhs.element.0] ?? 5
            return left == right ? lhs.offset < rhs.offset : left < right
        }
        for entry in sorted {
            let url = RSSURL.http(entry.element.1, base: root)
            if !url.isEmpty && !found.contains(where: { $0.url == url }) {
                found.append(RSSDiscoveredLink(url: url, title: entry.element.2, type: entry.element.0))
            }
        }
        let orderedIcons = icons.enumerated().sorted { lhs, rhs in
            lhs.element.0 == rhs.element.0 ? lhs.offset < rhs.offset : lhs.element.0 && !rhs.element.0
        }
        let icon = orderedIcons.lazy.map { RSSURL.http($0.element.1, base: root) }.first { !$0.isEmpty } ?? ""
        return RSSDiscovery(feeds: found, icon: icon, title: title)
    }
}

public enum RSSReadable {
    final class Node {
        let tag: String
        let attributes: [(String, String)]
        var children: [Child] = []
        weak var parent: Node?

        init(tag: String, attributes: [(String, String)], parent: Node?) {
            self.tag = tag
            self.attributes = attributes
            self.parent = parent
        }

        func attribute(_ name: String) -> String { attributes.first { $0.0 == name }?.1 ?? "" }
    }

    enum Child {
        case text(String)
        case node(Node)
    }

    static let skip: Set<String> = [
        "script", "style", "noscript", "template", "svg", "nav", "header", "footer", "aside", "form", "button",
        "select", "textarea", "iframe", "head",
    ]
    static let autoClose: [String: Set<String>] = [
        "p": ["p", "div", "ul", "ol", "table", "h1", "h2", "h3", "h4", "blockquote"],
        "li": ["li"],
    ]

    public struct Result: Equatable, Sendable {
        public var title: String
        public var contentHtml: String
    }

    static func build(_ document: String) -> (root: Node, meta: [String: String], title: String) {
        let root = Node(tag: "root", attributes: [], parent: nil)
        var current = root
        var skipping: [String] = []
        var meta: [String: String] = [:]
        var title = ""
        var inTitle = false
        for token in RSSHTMLTokenizer.tokens(RSSText.prefix(document, 1_500_000)) {
            switch token {
            case let .start(tag, attrs, _):
                let values = attrs.map { ($0.name, $0.value) }
                if tag == "meta" {
                    let lookup = Dictionary(values, uniquingKeysWith: { first, _ in first })
                    let key = (lookup["property"].flatMap { $0.isEmpty ? nil : $0 } ?? lookup["name"] ?? "").lowercased()
                    if !key.isEmpty, let content = lookup["content"], !content.isEmpty, meta[key] == nil { meta[key] = content }
                    continue
                }
                if tag == "title" { inTitle = true }
                if !skipping.isEmpty {
                    if tag == skipping.last && !RSSHTML.voidTags.contains(tag) { skipping.append(tag) }
                    continue
                }
                if skip.contains(tag) {
                    skipping.append(tag)
                    continue
                }
                if (tag == "p" || tag == "li") && (current.tag == "p" || current.tag == "li")
                    && (autoClose[current.tag]?.contains(tag) ?? false) {
                    current = current.parent ?? root
                }
                let node = Node(tag: tag, attributes: values, parent: current)
                current.children.append(.node(node))
                if !RSSHTML.voidTags.contains(tag) { current = node }
            case let .end(tag):
                if tag == "title" { inTitle = false }
                if !skipping.isEmpty {
                    if tag == skipping.last { skipping.removeLast() }
                    continue
                }
                var node: Node? = current
                while let candidate = node, candidate.tag != tag { node = candidate.parent }
                if let found = node, let parent = found.parent { current = parent }
            case let .text(data):
                if inTitle {
                    title = RSSText.clean(title + data, 300)
                    continue
                }
                if skipping.isEmpty && !data.isEmpty { current.children.append(.text(data)) }
            }
        }
        return (root, meta, title)
    }

    static func text(_ node: Node) -> String {
        var parts: [String] = []
        func walk(_ item: Node) {
            for child in item.children {
                switch child {
                case let .text(value): parts.append(value)
                case let .node(inner): walk(inner)
                }
            }
        }
        walk(node)
        return RSSText.clean(parts.joined(), Int.max)
    }

    static func serialize(_ node: Node) -> String {
        var out = ""
        func walk(_ child: Child) {
            switch child {
            case let .text(value):
                out += RSSText.escape(value, quote: false)
            case let .node(item):
                out += "<\(item.tag)"
                for (key, value) in item.attributes { out += " \(key)=\"\(RSSText.escape(value, quote: true))\"" }
                out += ">"
                if RSSHTML.voidTags.contains(item.tag) { return }
                for inner in item.children { walk(inner) }
                out += "</\(item.tag)>"
            }
        }
        for child in node.children { walk(child) }
        return out
    }

    static func paragraphScore(_ node: Node) -> Int {
        var total = 0
        for child in node.children {
            guard case let .node(item) = child, ["p", "pre", "blockquote", "li", "h2", "h3", "figure"].contains(item.tag) else { continue }
            let length = RSSText.length(text(item))
            total += length + (item.tag == "p" && length > 80 ? 25 : 0)
        }
        return total
    }

    public static func extract(_ document: String, base: String) -> Result {
        let (root, meta, pageTitle) = build(document)
        var best: Node?
        var bestScore = 0
        var articles: [Node] = []
        var stack: [Node] = [root]
        while let node = stack.popLast() {
            if node.tag == "article" || node.tag == "main" || node.attribute("itemprop").lowercased().contains("articlebody") {
                articles.append(node)
            }
            let score = paragraphScore(node)
            if score > bestScore {
                best = node
                bestScore = score
            }
            for child in node.children {
                if case let .node(inner) = child { stack.append(inner) }
            }
        }
        if let richest = articles.max(by: { RSSText.length(text($0)) < RSSText.length(text($1)) }) {
            if Double(RSSText.length(text(richest))) >= Double(bestScore) * 0.6 { best = richest }
        }
        let title = RSSText.clean(RSSEntities.decode(meta["og:title"] ?? pageTitle), 300)
        guard let chosen = best, RSSText.length(text(chosen)) >= 140 else { return Result(title: title, contentHtml: "") }
        return Result(title: title, contentHtml: RSSHTML.sanitize(serialize(chosen), base: base))
    }
}
