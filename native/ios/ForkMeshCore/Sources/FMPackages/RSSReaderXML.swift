import Foundation
#if canImport(FoundationXML)
import FoundationXML
#endif

public final class RSSXMLElement {
    public let tag: String
    public var attributes: [String: String]
    public var text: String = ""
    public var tail: String = ""
    public var children: [RSSXMLElement] = []

    init(tag: String, attributes: [String: String]) {
        self.tag = tag
        self.attributes = attributes
    }

    public func get(_ name: String) -> String? { attributes[name] }

    public func child(_ names: String...) -> RSSXMLElement? {
        for name in names {
            if let found = children.first(where: { $0.tag == name }) { return found }
        }
        return nil
    }

    public func all(_ name: String) -> [RSSXMLElement] { children.filter { $0.tag == name } }

    public var itertext: String {
        var out = text
        for child in children { out += child.itertext + child.tail }
        return out
    }

    public var plain: String { RSSText.strip(itertext) }

    static let htmlVoid: Set<String> = [
        "area", "base", "basefont", "br", "col", "frame", "hr", "img", "input", "isindex", "link", "meta", "param",
    ]

    public var serialized: String {
        var out = "<\(tag)"
        for key in attributes.keys.sorted() {
            out += " \(key)=\"\(RSSText.escape(attributes[key] ?? "", quote: true))\""
        }
        out += ">"
        if !RSSXMLElement.htmlVoid.contains(tag) {
            out += RSSText.escape(text, quote: false)
            for child in children { out += child.serialized }
            out += "</\(tag)>"
        }
        return out + RSSText.escape(tail, quote: false)
    }

    public var inner: String {
        var out = text
        for child in children { out += child.serialized }
        return RSSText.strip(out)
    }
}

public enum RSSXMLError: Error, Equatable, Sendable {
    case unreadable
}

public enum RSSXML {
    static func prefix(for namespace: String) -> String {
        if namespace.contains("purl.org/dc") { return "dc:" }
        if namespace.contains("purl.org/rss/1.0/modules/content") { return "content:" }
        if namespace.contains("search.yahoo.com/mrss") { return "media:" }
        if namespace.contains("itunes.com") { return "itunes:" }
        return ""
    }

    static func local(_ name: String) -> String {
        if let colon = name.lastIndex(of: ":") { return String(name[name.index(after: colon)...]).lowercased() }
        return name.lowercased()
    }

    final class Builder: NSObject, XMLParserDelegate {
        var root: RSSXMLElement?
        var stack: [RSSXMLElement] = []
        var failed = false

        func parser(
            _ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?, qualifiedName qName: String?,
            attributes attributeDict: [String: String] = [:]
        ) {
            let tag = RSSXML.prefix(for: namespaceURI ?? "") + RSSXML.local(elementName)
            var attributes: [String: String] = [:]
            for (key, value) in attributeDict {
                if key == "xmlns" || key.hasPrefix("xmlns:") { continue }
                attributes[RSSXML.local(key)] = value
            }
            let element = RSSXMLElement(tag: tag, attributes: attributes)
            if let parent = stack.last {
                parent.children.append(element)
            } else if root == nil {
                root = element
            }
            stack.append(element)
        }

        func parser(_ parser: XMLParser, didEndElement elementName: String, namespaceURI: String?, qualifiedName qName: String?) {
            if !stack.isEmpty { stack.removeLast() }
        }

        func append(_ string: String) {
            guard let current = stack.last else { return }
            if let last = current.children.last {
                last.tail += string
            } else {
                current.text += string
            }
        }

        func parser(_ parser: XMLParser, foundCharacters string: String) { append(string) }

        func parser(_ parser: XMLParser, foundCDATA CDATABlock: Data) {
            append(String(decoding: CDATABlock, as: UTF8.self))
        }

        func parser(_ parser: XMLParser, parseErrorOccurred parseError: Error) { failed = true }
    }

    static func run(_ text: String) -> RSSXMLElement? {
        let parser = XMLParser(data: Data(text.utf8))
        parser.shouldProcessNamespaces = true
        parser.shouldResolveExternalEntities = false
        let builder = Builder()
        parser.delegate = builder
        let ok = parser.parse()
        guard ok, !builder.failed, let root = builder.root else { return nil }
        return root
    }

    public static func stripDoctype(_ text: String) -> String {
        guard let start = text.range(of: "<!DOCTYPE", options: .caseInsensitive) else { return text }
        var index = start.upperBound
        var bracketed = false
        while index < text.endIndex {
            let character = text[index]
            if character == ">" { break }
            if character == "[" {
                bracketed = true
                break
            }
            index = text.index(after: index)
        }
        guard index < text.endIndex else { return text }
        if bracketed {
            guard let close = text.range(of: "]", range: index..<text.endIndex) else { return text }
            var after = close.upperBound
            while after < text.endIndex, text[after].isWhitespace { after = text.index(after: after) }
            guard after < text.endIndex, text[after] == ">" else { return text }
            return String(text[..<start.lowerBound]) + String(text[text.index(after: after)...])
        }
        return String(text[..<start.lowerBound]) + String(text[text.index(after: index)...])
    }

    static func utf8Declaration(_ text: String) -> String {
        guard text.hasPrefix("<?xml"), let end = text.range(of: "?>") else { return text }
        let declaration = text[..<end.upperBound]
        guard declaration.range(of: "encoding", options: .caseInsensitive) != nil else { return text }
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" + text[end.upperBound...]
    }

    static let xmlEntities: Set<String> = ["amp", "lt", "gt", "quot", "apos"]

    public static func repair(_ text: String) -> String {
        var out = String.UnicodeScalarView()
        let scalars = Array(text.unicodeScalars)
        var index = 0
        while index < scalars.count {
            let scalar = scalars[index]
            let value = scalar.value
            if value <= 0x08 || value == 0x0B || value == 0x0C || (0x0E...0x1F).contains(value) {
                index += 1
                continue
            }
            guard scalar == "&" else {
                out.append(scalar)
                index += 1
                continue
            }
            var cursor = index + 1
            if cursor < scalars.count, scalars[cursor] == "#" {
                cursor += 1
                var hex = false
                if cursor < scalars.count, scalars[cursor] == "x" || scalars[cursor] == "X" {
                    hex = true
                    cursor += 1
                }
                let start = cursor
                while cursor < scalars.count,
                      hex ? scalars[cursor].properties.isASCIIHexDigit : ("0"..."9").contains(scalars[cursor]) {
                    cursor += 1
                }
                if cursor > start, cursor < scalars.count, scalars[cursor] == ";" {
                    out.append(contentsOf: scalars[index...cursor])
                    index = cursor + 1
                } else {
                    out.append(contentsOf: "&amp;".unicodeScalars)
                    index += 1
                }
                continue
            }
            if cursor < scalars.count, scalars[cursor].isASCII, scalars[cursor].properties.isAlphabetic {
                let start = cursor
                while cursor < scalars.count, scalars[cursor].isASCII,
                      scalars[cursor].properties.isAlphabetic || ("0"..."9").contains(scalars[cursor]) {
                    cursor += 1
                }
                if cursor < scalars.count, scalars[cursor] == ";" {
                    var nameView = String.UnicodeScalarView()
                    nameView.append(contentsOf: scalars[start..<cursor])
                    let name = String(nameView)
                    if xmlEntities.contains(name) {
                        out.append(contentsOf: "&\(name);".unicodeScalars)
                    } else if let code = RSSEntities.named[name] {
                        out.append(contentsOf: "&#\(code);".unicodeScalars)
                    } else {
                        out.append(contentsOf: "&amp;\(name);".unicodeScalars)
                    }
                    index = cursor + 1
                    continue
                }
            }
            out.append(contentsOf: "&amp;".unicodeScalars)
            index += 1
        }
        return String(out)
    }

    public static func parse(_ document: String) throws -> RSSXMLElement {
        var source = document
        while let first = source.unicodeScalars.first, first == "\u{FEFF}" || first == " " || first == "\t" || first == "\r" || first == "\n" {
            source.unicodeScalars.removeFirst()
        }
        source = utf8Declaration(stripDoctype(source))
        if let root = run(source) { return root }
        if let root = run(repair(source)) { return root }
        throw RSSXMLError.unreadable
    }

    static func charset(_ name: String) -> String.Encoding? {
        switch name.lowercased().trimmingCharacters(in: .whitespaces).trimmingCharacters(in: CharacterSet(charactersIn: "\"'")) {
        case "utf-8", "utf8": return .utf8
        case "iso-8859-1", "latin1", "latin-1", "iso8859-1", "l1": return .isoLatin1
        case "windows-1252", "cp1252": return .windowsCP1252
        case "us-ascii", "ascii": return .isoLatin1
        case "utf-16": return .utf16
        case "utf-16le": return .utf16LittleEndian
        case "utf-16be": return .utf16BigEndian
        case "iso-8859-2", "latin2": return .isoLatin2
        case "shift_jis", "shift-jis", "sjis": return .shiftJIS
        case "euc-jp": return .japaneseEUC
        case "windows-1251", "cp1251": return .windowsCP1251
        default: return nil
        }
    }

    public static func decode(_ data: Data, contentType: String = "") -> String {
        let bytes = [UInt8](data.prefix(4))
        if bytes.count >= 3, bytes[0] == 0xEF, bytes[1] == 0xBB, bytes[2] == 0xBF {
            return String(decoding: data.dropFirst(3), as: UTF8.self)
        }
        if bytes.count >= 2, (bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF) {
            if let text = String(data: data, encoding: .utf16) { return text }
        }
        let head = String(decoding: data.prefix(300), as: UTF8.self)
        var declared = ""
        if head.hasPrefix("<?xml"), let end = head.range(of: "?>") {
            let declaration = String(head[..<end.lowerBound])
            if let range = declaration.range(of: "encoding", options: .caseInsensitive) {
                let rest = declaration[range.upperBound...].drop { $0 == " " || $0 == "=" }
                if let quote = rest.first, quote == "\"" || quote == "'" {
                    declared = String(rest.dropFirst().prefix { $0 != quote })
                }
            }
        }
        if declared.isEmpty, let range = contentType.range(of: "charset=", options: .caseInsensitive) {
            declared = String(contentType[range.upperBound...].prefix { $0 != ";" })
        }
        if let encoding = charset(declared), encoding != .utf8, let text = String(data: data, encoding: encoding) {
            return text
        }
        return String(decoding: data, as: UTF8.self)
    }
}
