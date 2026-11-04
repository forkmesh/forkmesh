import FMBase
import Foundation

public struct NotePrintLayout: Equatable, Sendable {
    public enum Size: String, CaseIterable, Sendable {
        case a4, letter, legal

        public var label: String {
            switch self {
            case .a4: return "A4"
            case .letter: return "Letter"
            case .legal: return "Legal"
            }
        }

        public var millimetres: (width: Double, height: Double) {
            switch self {
            case .a4: return (210, 297)
            case .letter: return (215.9, 279.4)
            case .legal: return (215.9, 355.6)
            }
        }
    }

    public enum Orientation: String, CaseIterable, Sendable {
        case portrait, landscape

        public var label: String { self == .portrait ? "Portrait" : "Landscape" }
    }

    public enum Margins: String, CaseIterable, Sendable {
        case narrow, normal, wide

        public var label: String {
            switch self {
            case .narrow: return "Narrow"
            case .normal: return "Normal"
            case .wide: return "Wide"
            }
        }

        public var millimetres: Double {
            switch self {
            case .narrow: return 12.7
            case .normal: return 25.4
            case .wide: return 38.1
            }
        }
    }

    public var size: Size
    public var orientation: Orientation
    public var margins: Margins
    public var cover: Bool
    public var contents: Bool
    public var numbers: Bool

    public init(
        size: Size = .a4, orientation: Orientation = .portrait, margins: Margins = .normal,
        cover: Bool = false, contents: Bool = false, numbers: Bool = true
    ) {
        self.size = size
        self.orientation = orientation
        self.margins = margins
        self.cover = cover
        self.contents = contents
        self.numbers = numbers
    }

    public static let standard = NotePrintLayout()

    public var line: String {
        "<!-- page: size=\(size.rawValue); orientation=\(orientation.rawValue); margins=\(margins.rawValue); "
            + "cover=\(Self.word(cover)); contents=\(Self.word(contents)); numbers=\(Self.word(numbers)) -->"
    }

    public var paperMillimetres: (width: Double, height: Double) {
        let paper = size.millimetres
        return orientation == .landscape ? (paper.height, paper.width) : paper
    }

    public var paperPoints: (width: Double, height: Double) {
        let paper = paperMillimetres
        return (Self.points(paper.width), Self.points(paper.height))
    }

    public var marginPoints: Double { Self.points(margins.millimetres) }

    public static func points(_ millimetres: Double) -> Double { millimetres / 25.4 * 72 }

    static func word(_ flag: Bool) -> String { flag ? "yes" : "no" }

    static func flag(_ value: String) -> Bool? {
        switch value {
        case "yes": return true
        case "no": return false
        default: return nil
        }
    }

    public static func parse(_ settings: String) -> NotePrintLayout {
        var layout = NotePrintLayout()
        for pair in settings.components(separatedBy: ";") {
            let parts = pair.components(separatedBy: "=")
            let key = parts[0].trimmingCharacters(in: .whitespaces).lowercased()
            let value = parts.count > 1 ? parts[1].trimmingCharacters(in: .whitespaces).lowercased() : ""
            switch key {
            case "size": layout.size = Size(rawValue: value) ?? layout.size
            case "orientation": layout.orientation = Orientation(rawValue: value) ?? layout.orientation
            case "margins": layout.margins = Margins(rawValue: value) ?? layout.margins
            case "cover": layout.cover = flag(value) ?? layout.cover
            case "contents": layout.contents = flag(value) ?? layout.contents
            case "numbers": layout.numbers = flag(value) ?? layout.numbers
            default: break
            }
        }
        return layout
    }
}

public struct NoteFigure: Equatable, Sendable {
    public let svg: String
    public let caption: String

    public init(svg: String, caption: String) {
        self.svg = svg
        self.caption = caption
    }

    public var dataURL: String { "data:image/svg+xml;base64,\(Data(svg.utf8).base64EncodedString())" }

    public var label: String { caption.isEmpty ? "Diagram" : caption }

    public var size: NoteFigureSize { NoteFigureSize.of(svg) }

    public var page: String {
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
            + "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src data:; style-src 'unsafe-inline'\">"
            + "<style>html,body{margin:0;height:100%;background:transparent}"
            + "img{display:block;width:100%;height:100%;object-fit:contain}</style>"
            + "</head><body><img alt=\"\" src=\"\(dataURL)\"></body></html>"
    }
}

public struct NoteFigureSize: Equatable, Sendable {
    public let width: Double?
    public let aspect: Double

    public init(width: Double?, aspect: Double) {
        self.width = width
        self.aspect = aspect
    }

    public static func of(_ svg: String) -> NoteFigureSize {
        let tag = rootTag(svg)
        let width = attribute("width", in: tag).flatMap(length)
        let height = attribute("height", in: tag).flatMap(length)
        let box = attribute("viewBox", in: tag).flatMap(viewBox)
        let result: NoteFigureSize
        switch (width, height, box) {
        case let (w?, h?, _):
            result = NoteFigureSize(width: w, aspect: h / w)
        case let (w?, nil, b?):
            result = NoteFigureSize(width: w, aspect: b.height / b.width)
        case let (nil, h?, b?):
            result = NoteFigureSize(width: h * b.width / b.height, aspect: b.height / b.width)
        case let (nil, nil, b?):
            result = NoteFigureSize(width: nil, aspect: b.height / b.width)
        case let (w?, nil, nil):
            result = NoteFigureSize(width: w, aspect: 150 / w)
        case let (nil, h?, nil):
            result = NoteFigureSize(width: 300, aspect: h / 300)
        case (nil, nil, nil):
            result = NoteFigureSize(width: 300, aspect: 0.5)
        }
        return NoteFigureSize(width: result.width, aspect: min(20, max(0.05, result.aspect)))
    }

    static func rootTag(_ svg: String) -> String {
        let start = NoteDocument.afterProlog(svg)
        guard let end = start.firstIndex(of: ">") else { return String(start) }
        return String(start[..<end])
    }

    static func attribute(_ name: String, in tag: String) -> String? {
        let pattern = "[ \\t\\r\\n]\(name)[ \\t\\r\\n]*=[ \\t\\r\\n]*(?:\"([^\"]*)\"|'([^']*)')"
        guard let regex = try? NSRegularExpression(pattern: pattern) else { return nil }
        let range = NSRange(tag.startIndex..., in: tag)
        guard let match = regex.firstMatch(in: tag, range: range) else { return nil }
        for group in 1...2 {
            if let found = Range(match.range(at: group), in: tag) { return String(tag[found]) }
        }
        return nil
    }

    static func length(_ text: String) -> Double? {
        let trimmed = text.trimmingCharacters(in: .whitespaces).lowercased()
        let digits = trimmed.prefix { "0123456789.+-".contains($0) }
        guard let value = Double(digits), value > 0 else { return nil }
        let unit = trimmed.dropFirst(digits.count).trimmingCharacters(in: .whitespaces)
        let scale: Double
        switch unit {
        case "", "px": scale = 1
        case "pt": scale = 4.0 / 3.0
        case "pc": scale = 16
        case "mm": scale = 96 / 25.4
        case "cm": scale = 96 / 2.54
        case "in": scale = 96
        case "em": scale = 16
        default: return nil
        }
        return value * scale
    }

    static func viewBox(_ text: String) -> (width: Double, height: Double)? {
        let numbers = text.split(whereSeparator: { $0 == "," || $0.isWhitespace }).compactMap { Double($0) }
        guard numbers.count == 4, numbers[2] > 0, numbers[3] > 0 else { return nil }
        return (numbers[2], numbers[3])
    }
}

public enum NoteDocumentPart: Equatable, Sendable {
    case markdown(String)
    case figure(NoteFigure)
    case pageBreak
}

public struct NoteHeading: Equatable, Sendable {
    public let level: Int
    public let text: String
    public let anchor: String

    public init(level: Int, text: String, anchor: String) {
        self.level = level
        self.text = text
        self.anchor = anchor
    }
}

public enum NoteDocument {
    public static let svgMaxBytes = 262144
    public static let exportTitle = "Export PDF"
    public static let menuLabel = "Export PDF"
    public static let paperLabel = "Paper"
    public static let orientationLabel = "Orientation"
    public static let marginsLabel = "Margins"
    public static let coverLabel = "Cover page"
    public static let contentsLabel = "Contents"
    public static let numbersLabel = "Page numbers"
    public static let saveLayoutLabel = "Save as this note's page layout"
    public static let exportButton = "Export PDF"
    public static let pageBreakLabel = "Page break"
    public static let failedMessage = "The PDF could not be made."

    public static func lines(_ markdown: String) -> [String] {
        markdown.replacingOccurrences(of: "\r\n", with: "\n").replacingOccurrences(of: "\r", with: "\n")
            .components(separatedBy: "\n")
    }

    static func commentBody(_ line: String) -> Substring? {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        guard trimmed.hasPrefix("<!--") else { return nil }
        let rest = trimmed.dropFirst(4)
        guard rest.hasSuffix("-->") else { return nil }
        return rest.dropLast(3)
    }

    public static func layoutSettings(_ line: String) -> String? {
        guard let body = commentBody(line) else { return nil }
        let lead = body.drop { $0.isWhitespace }
        guard lead.prefix(5).lowercased() == "page:" else { return nil }
        return String(lead.dropFirst(5))
    }

    public static func isLayoutLine(_ line: String) -> Bool { layoutSettings(line) != nil }

    public static func isImageOptions(_ line: String) -> Bool {
        guard let body = commentBody(line) else { return false }
        return body.drop { $0.isWhitespace }.prefix(6).lowercased() == "image:"
    }

    public static func isPageBreak(_ line: String) -> Bool {
        guard let body = commentBody(line) else { return false }
        return body.trimmingCharacters(in: .whitespaces).lowercased() == "pagebreak"
    }

    static func fence(_ line: String) -> (marker: String, info: String)? {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        for mark: Character in ["`", "~"] {
            let run = trimmed.prefix { $0 == mark }
            guard run.count >= 3 else { continue }
            let info = trimmed.dropFirst(run.count)
            if mark == "`" && info.contains("`") { return nil }
            return (String(run), String(info))
        }
        return nil
    }

    static func closes(_ line: String, _ marker: String) -> Bool {
        let trimmed = line.trimmingCharacters(in: .whitespaces)
        return trimmed.hasPrefix(marker) && trimmed.allSatisfy { $0 == marker.first }
    }

    static func fenceEnd(_ lines: [String], from start: Int, marker: String) -> Int {
        var index = start + 1
        while index < lines.count {
            if closes(lines[index], marker) { return index }
            index += 1
        }
        return lines.count - 1
    }

    static func layoutLineIndices(_ lines: [String]) -> [Int] {
        var found: [Int] = []
        var index = 0
        while index < lines.count {
            if let open = fence(lines[index]) {
                index = fenceEnd(lines, from: index, marker: open.marker) + 1
                continue
            }
            if isLayoutLine(lines[index]) { found.append(index) }
            index += 1
        }
        return found
    }

    public static func layout(_ markdown: String) -> NotePrintLayout {
        let lines = lines(markdown)
        guard let first = layoutLineIndices(lines).first, let settings = layoutSettings(lines[first]) else {
            return .standard
        }
        return NotePrintLayout.parse(settings)
    }

    public static func withLayout(_ markdown: String, _ layout: NotePrintLayout) -> String {
        var lines = lines(markdown)
        if let first = layoutLineIndices(lines).first { lines.remove(at: first) }
        while let head = lines.first, head.trimmingCharacters(in: .whitespaces).isEmpty {
            lines.removeFirst()
        }
        return "\(layout.line)\n\n\(lines.joined(separator: "\n"))"
    }

    public static func withoutLayout(_ markdown: String) -> String {
        var lines = lines(markdown)
        for index in layoutLineIndices(lines).reversed() { lines.remove(at: index) }
        return lines.joined(separator: "\n")
    }

    public static func figure(info: String, body: String) -> NoteFigure? {
        let words = info.trimmingCharacters(in: .whitespaces)
        let word = words.prefix { !$0.isWhitespace }
        guard word.lowercased() == "svg" else { return nil }
        guard body.utf8.count <= svgMaxBytes, startsWithSvg(body) else { return nil }
        let caption = words.dropFirst(word.count).trimmingCharacters(in: .whitespaces)
        return NoteFigure(svg: body, caption: caption)
    }

    static func afterProlog(_ body: String) -> Substring {
        var rest = Substring(body)
        while true {
            rest = rest.drop { $0.isWhitespace }
            if rest.hasPrefix("<?xml"), let end = rest.range(of: "?>", range: rest.index(rest.startIndex, offsetBy: 5)..<rest.endIndex) {
                rest = rest[end.upperBound...]
                continue
            }
            if rest.hasPrefix("<!--"), let end = rest.range(of: "-->", range: rest.index(rest.startIndex, offsetBy: 4)..<rest.endIndex) {
                rest = rest[end.upperBound...]
                continue
            }
            return rest
        }
    }

    static func startsWithSvg(_ body: String) -> Bool {
        let rest = afterProlog(body)
        guard rest.prefix(4).lowercased() == "<svg", let next = rest.dropFirst(4).first else { return false }
        return next.isWhitespace || next == ">" || next == "/"
    }

    public static func parts(_ markdown: String) -> [NoteDocumentPart] {
        let lines = lines(markdown)
        var out: [NoteDocumentPart] = []
        var buffer: [String] = []
        func flush() {
            let text = buffer.joined(separator: "\n")
            if !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty { out.append(.markdown(text)) }
            buffer.removeAll()
        }
        var index = 0
        while index < lines.count {
            let line = lines[index]
            if let open = fence(line) {
                let end = fenceEnd(lines, from: index, marker: open.marker)
                let closed = end > index && closes(lines[end], open.marker)
                let body = lines[min(index + 1, lines.count)..<(closed ? end : lines.count)].joined(separator: "\n")
                if let figure = figure(info: open.info, body: body) {
                    flush()
                    out.append(.figure(figure))
                } else {
                    buffer.append(contentsOf: lines[index...end])
                }
                index = end + 1
                continue
            }
            if isPageBreak(line) {
                flush()
                out.append(.pageBreak)
            } else if !isLayoutLine(line) && !isImageOptions(line) {
                buffer.append(line)
            }
            index += 1
        }
        flush()
        return out
    }

    public static func headings(_ markdown: String) -> [NoteHeading] {
        var html = NotePrintHTML()
        _ = html.body(parts(markdown))
        return html.headings.filter { $0.level <= 2 }
    }

    public static func pdfFileName(_ title: String) -> String { "\(slug(title)).pdf" }

    public static func slug(_ title: String) -> String {
        var out = ""
        var dash = false
        for scalar in title.unicodeScalars {
            if isSlugScalar(scalar) {
                out.unicodeScalars.append(scalar)
                dash = false
            } else if !dash {
                out.append("-")
                dash = true
            }
        }
        let trimmed = String(out.drop(while: { $0 == "-" }).reversed().drop(while: { $0 == "-" }).reversed()).lowercased()
        return trimmed.isEmpty ? "note" : trimmed
    }

    static func isSlugScalar(_ scalar: Unicode.Scalar) -> Bool {
        switch scalar.value {
        case 0x30...0x39, 0x41...0x5A, 0x61...0x7A: return true
        default: return scalar == "_" || scalar == "." || scalar == "-"
        }
    }

    public static func exportDate(_ date: Date, timeZone: TimeZone = .current) -> String {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = timeZone
        formatter.dateFormat = "MMMM d, yyyy"
        return formatter.string(from: date)
    }

    public static func pageNumber(_ page: Int, of total: Int) -> String { "\(page) / \(total)" }

    public static func showsNumber(onPage index: Int, layout: NotePrintLayout) -> Bool {
        layout.numbers && !(layout.cover && index == 0)
    }

    public static func printHTML(
        title: String, emoji: String, markdown: String, layout: NotePrintLayout,
        date: Date, timeZone: TimeZone = .current, nativeMargins: Bool = false
    ) -> String {
        let heading = title.dartTrimmed.isEmpty ? "Untitled note" : title.dartTrimmed
        var builder = NotePrintHTML()
        let body = builder.body(parts(markdown))
        let escape = NotePrintHTML.escape
        var cover = ""
        if layout.cover {
            let mark = emoji.isEmpty ? "" : "<div class=\"note-cover-emoji\">\(escape(emoji))</div>"
            cover = "<section class=\"note-cover\">\(mark)<h1>\(escape(heading))</h1>"
                + "<div class=\"note-cover-date\">\(escape(exportDate(date, timeZone: timeZone)))</div></section>"
        }
        var contents = ""
        if layout.contents {
            let items = builder.headings.filter { $0.level <= 2 }.map { (entry: NoteHeading) -> String in
                "<li class=\"note-contents-\(entry.level)\"><a href=\"#\(escape(entry.anchor))\">\(escape(entry.text))</a></li>"
            }
            contents = "<section class=\"note-contents\"><h1>Contents</h1><ol>\(items.joined())</ol></section>"
        }
        let lead = layout.cover ? "" : "<h1>\(escape(heading))</h1>"
        let name = escape(slug(title))
        return "<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            + "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src data: https: http:; style-src 'unsafe-inline'\">"
            + "<title>\(name)</title><style>\(printStyles(layout, nativeMargins: nativeMargins))</style></head>"
            + "<body>\(cover)\(contents)<article>\(lead)\(body)</article></body></html>"
    }

    public static func printStyles(_ layout: NotePrintLayout, nativeMargins: Bool) -> String {
        let paper = layout.paperMillimetres
        let margin = layout.margins.millimetres
        let page = nativeMargins
            ? "@page { margin: 0; }"
            : "@page { size: \(millimetres(paper.width)) \(millimetres(paper.height)); margin: \(millimetres(margin)); }"
        let coverTop = Int(((paper.height - 2 * margin) * 0.22).rounded())
        let sans = "-apple-system, \"Segoe UI\", system-ui, Roboto, \"Helvetica Neue\", Arial, sans-serif"
        let mono = "ui-monospace, SFMono-Regular, \"SF Mono\", Menlo, Consolas, monospace"
        return """
        \(page)
        html { print-color-adjust: exact; -webkit-print-color-adjust: exact; }
        body { margin: 0; background: #fff; color: #1f2328; font: 11pt/1.45 \(sans); overflow-wrap: anywhere; }
        h1, h2, h3, h4, h5, h6 { margin: 16pt 0 6pt; line-height: 1.25; font-weight: 600; break-after: avoid; page-break-after: avoid; }
        h1 { font-size: 20pt; } h2 { font-size: 15pt; } h3 { font-size: 12.5pt; } h4 { font-size: 11pt; } h5, h6 { font-size: 10pt; }
        body > article > :first-child { margin-top: 0; }
        p { margin: 0 0 8pt; }
        a { color: #0969da; text-decoration: none; }
        ul, ol { margin: 0 0 8pt; padding-left: 18pt; }
        li { margin: 2pt 0; }
        pre { margin: 0 0 10pt; padding: 8pt; border: .5pt solid #d0d7de; border-radius: 4pt; background: #f6f8fa; font: 9pt/1.4 \(mono); white-space: pre-wrap; break-inside: avoid; page-break-inside: avoid; }
        code { border-radius: 2pt; background: #f6f8fa; padding: 0 2pt; font: 9pt \(mono); }
        pre code { background: none; padding: 0; }
        blockquote { margin: 0 0 8pt; border-left: 2pt solid #d0d7de; padding-left: 10pt; color: #57606a; }
        table { width: 100%; margin: 0 0 10pt; border-collapse: collapse; font-size: 9.5pt; }
        th, td { border: .5pt solid #d0d7de; padding: 3pt 5pt; text-align: left; vertical-align: top; }
        th { background: #f6f8fa; }
        tr, img { break-inside: avoid; page-break-inside: avoid; }
        img { max-width: 100%; }
        hr { margin: 12pt 0; border: 0; border-top: .5pt solid #d0d7de; }
        .note-figure { margin: 10pt 0 12pt; text-align: center; break-inside: avoid; page-break-inside: avoid; }
        .note-figure img { display: block; max-width: 100%; height: auto; margin: 0 auto; }
        .note-figure figcaption { margin-top: 4pt; font-size: 9pt; color: #57606a; }
        .note-page-break { break-before: page; page-break-before: always; height: 0; margin: 0; border: 0; }
        .note-cover { break-after: page; page-break-after: always; padding-top: \(coverTop)mm; text-align: center; }
        .note-cover-emoji { font-size: 48pt; line-height: 1.2; }
        .note-cover h1 { margin: 10pt 0 8pt; font-size: 28pt; }
        .note-cover-date { font-size: 12pt; color: #57606a; }
        .note-contents { break-after: page; page-break-after: always; }
        .note-contents ol { list-style: none; margin: 0; padding: 0; }
        .note-contents li { margin: 0 0 5pt; }
        .note-contents .note-contents-2 { padding-left: 16pt; font-size: 10pt; }
        """
    }

    static func millimetres(_ value: Double) -> String {
        let rounded = (value * 10).rounded() / 10
        return rounded == rounded.rounded() ? "\(Int(rounded))mm" : "\(rounded)mm"
    }

    public static func write(_ data: Data, fileName: String, in directory: URL) throws -> URL {
        let target = directory.appendingPathComponent(fileName)
        try data.write(to: target, options: .atomic)
        return target
    }
}

struct NotePrintHTML {
    var headings: [NoteHeading] = []
    private var anchors: [String: Int] = [:]

    static func escape(_ text: String) -> String {
        var out = ""
        out.reserveCapacity(text.utf8.count)
        for ch in text {
            switch ch {
            case "&": out += "&amp;"
            case "<": out += "&lt;"
            case ">": out += "&gt;"
            case "\"": out += "&quot;"
            case "'": out += "&#39;"
            default: out.append(ch)
            }
        }
        return out
    }

    static func safeLink(_ href: String) -> String? {
        let trimmed = href.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return nil }
        guard let colon = trimmed.firstIndex(of: ":") else { return trimmed }
        let scheme = trimmed[..<colon]
        if scheme.contains(where: { $0 == "/" || $0 == "?" || $0 == "#" }) { return trimmed }
        return ["http", "https", "mailto"].contains(scheme.lowercased()) ? trimmed : nil
    }

    static func safeImage(_ src: String) -> String? {
        let trimmed = src.trimmingCharacters(in: .whitespaces)
        let lowered = trimmed.lowercased()
        return lowered.hasPrefix("https://") || lowered.hasPrefix("http://") || lowered.hasPrefix("data:image/") ? trimmed : nil
    }

    mutating func body(_ parts: [NoteDocumentPart]) -> String {
        var out = ""
        for part in parts {
            switch part {
            case let .markdown(text):
                out += blocks(DocsMarkdown.blocks(text))
            case let .figure(figure):
                let caption = figure.caption.isEmpty ? "" : "<figcaption>\(Self.escape(figure.caption))</figcaption>"
                out += "<figure class=\"note-figure\"><img src=\"\(figure.dataURL)\" alt=\"\(Self.escape(figure.label))\">\(caption)</figure>"
            case .pageBreak:
                out += "<div class=\"note-page-break\"></div>"
            }
        }
        return out
    }

    mutating func anchor(_ text: String) -> String {
        let plain = DocsMarkdown.anchor(DocsMarkdown.plain(text))
        let base = plain.isEmpty ? "section" : plain
        let seen = anchors[base] ?? 0
        anchors[base] = seen + 1
        return "note-" + (seen == 0 ? base : "\(base)-\(seen)")
    }

    mutating func blocks(_ blocks: [DocsBlock]) -> String {
        var out = ""
        var lists: [Bool] = []
        func closeLists(to depth: Int) {
            while lists.count > depth {
                out += lists.removeLast() ? "</li></ol>" : "</li></ul>"
            }
        }
        for block in blocks {
            if case let .listItem(ordered, number, depth, text) = block {
                let level = depth + 1
                closeLists(to: level)
                if lists.count == level, lists[level - 1] != ordered { closeLists(to: level - 1) }
                if lists.count == level {
                    out += "</li>"
                }
                while lists.count < level {
                    let start = ordered && lists.count == level - 1 && number != 1 ? " start=\"\(number)\"" : ""
                    out += ordered ? "<ol\(start)>" : "<ul>"
                    lists.append(ordered)
                }
                out += "<li>\(Self.inline(text))"
                continue
            }
            closeLists(to: 0)
            switch block {
            case let .heading(level, text, _):
                let id = anchor(text)
                headings.append(NoteHeading(level: level, text: DocsMarkdown.plain(text), anchor: id))
                out += "<h\(level) id=\"\(Self.escape(id))\">\(Self.inline(text))</h\(level)>"
            case let .paragraph(text):
                out += "<p>\(Self.inline(text))</p>"
            case let .quote(text):
                let paragraphs = text.components(separatedBy: "\n").map { "<p>\(Self.inline($0))</p>" }
                out += "<blockquote>\(paragraphs.joined())</blockquote>"
            case let .code(_, text):
                out += "<pre><code>\(Self.escape(text))</code></pre>"
            case let .table(header, rows):
                let head = header.map { "<th>\(Self.inline($0))</th>" }.joined()
                let body = rows.map { (row: [String]) -> String in
                    "<tr>" + row.map { "<td>\(Self.inline($0))</td>" }.joined() + "</tr>"
                }.joined()
                out += "<table><thead><tr>\(head)</tr></thead><tbody>\(body)</tbody></table>"
            case .rule:
                out += "<hr>"
            case .listItem:
                break
            }
        }
        closeLists(to: 0)
        return out
    }

    static func runs(_ runs: [DocsRun]) -> String {
        runs.map { (run: DocsRun) -> String in
            var html = escape(run.text).replacingOccurrences(of: "\n", with: "<br>")
            if run.code { html = "<code>\(html)</code>" }
            if run.strike { html = "<del>\(html)</del>" }
            if run.italic { html = "<em>\(html)</em>" }
            if run.bold { html = "<strong>\(html)</strong>" }
            if let link = run.link, let href = safeLink(link) { html = "<a href=\"\(escape(href))\">\(html)</a>" }
            return html
        }.joined()
    }

    static func image(_ chars: [Character], at start: Int, scanner: InlineScanner) -> (html: String, next: Int)? {
        guard start + 1 < chars.count, chars[start] == "!", chars[start + 1] == "[",
            let close = scanner.closingBracket(from: start + 1, limit: chars.count),
            let target = scanner.destination(from: close + 1, limit: chars.count)
        else { return nil }
        let alt = escape(DocsMarkdown.plain(String(chars[(start + 2)..<close])))
        guard let src = safeImage(target.href) else { return (alt, target.end + 1) }
        return ("<img src=\"\(escape(src))\" alt=\"\(alt)\">", target.end + 1)
    }

    static func inline(_ text: String) -> String {
        let chars = Array(text)
        let scanner = InlineScanner(chars: chars)
        var out = ""
        var pending = ""
        func flush() {
            if !pending.isEmpty { out += runs(DocsInline.runs(pending)) }
            pending = ""
        }
        var index = 0
        while index < chars.count {
            let ch = chars[index]
            if ch == "\\", index + 1 < chars.count {
                pending.append(ch)
                pending.append(chars[index + 1])
                index += 2
                continue
            }
            if ch == "`" {
                var count = 0
                while index + count < chars.count, chars[index + count] == "`" { count += 1 }
                var probe = index + count
                var end: Int?
                while probe < chars.count {
                    guard chars[probe] == "`" else {
                        probe += 1
                        continue
                    }
                    var run = 0
                    while probe + run < chars.count, chars[probe + run] == "`" { run += 1 }
                    if run == count {
                        end = probe + run
                        break
                    }
                    probe += run
                }
                let stop = end ?? index + count
                pending += String(chars[index..<stop])
                index = stop
                continue
            }
            if ch == "!", let found = image(chars, at: index, scanner: scanner) {
                flush()
                out += found.html
                index = found.next
                continue
            }
            if ch == "[", index + 1 < chars.count, chars[index + 1] == "!",
                let close = scanner.closingBracket(from: index, limit: chars.count),
                let inner = image(chars, at: index + 1, scanner: scanner), inner.next == close,
                let target = scanner.destination(from: close + 1, limit: chars.count)
            {
                flush()
                if let href = safeLink(target.href) {
                    out += "<a href=\"\(escape(href))\">\(inner.html)</a>"
                } else {
                    out += inner.html
                }
                index = target.end + 1
                continue
            }
            pending.append(ch)
            index += 1
        }
        flush()
        return out
    }
}

extension NoteEditorModel {
    public var pageLayout: NotePrintLayout { NoteDocument.layout(markdown) }

    public var pdfFileName: String { NoteDocument.pdfFileName(title) }

    public func savePageLayout(_ layout: NotePrintLayout) {
        guard canEdit else { return }
        let next = NoteDocument.withLayout(markdown, layout)
        guard next != markdown else { return }
        markdown = next
        edited()
    }

    public func printHTML(_ layout: NotePrintLayout, date: Date, timeZone: TimeZone = .current) -> String {
        NoteDocument.printHTML(
            title: title, emoji: emoji, markdown: markdown, layout: layout, date: date, timeZone: timeZone,
            nativeMargins: true)
    }
}
