import Foundation
#if canImport(FoundationXML)
import FoundationXML
#endif

public struct SecureDrivePoint: Equatable, Hashable, Sendable {
    public var x: Double
    public var y: Double

    public init(_ x: Double, _ y: Double) {
        self.x = x
        self.y = y
    }
}

public struct SecureDriveStroke: Equatable, Sendable {
    public var color: String
    public var width: Double
    public var points: [SecureDrivePoint]

    public init(color: String = "#1f2328", width: Double = 3, points: [SecureDrivePoint] = []) {
        self.color = color
        self.width = width
        self.points = points
    }
}

public enum SecureDriveShape: String, CaseIterable, Sendable {
    case pen, line, box, oval, eraser
}

public struct SecureDriveDrawing: Equatable, Sendable {
    static let maxStrokes = 20000
    static let maxPoints = 200_000

    public var width: Int
    public var height: Int
    public var background: String
    public var strokes: [SecureDriveStroke]

    public init(width: Int = 1600, height: Int = 1000, background: String = "#ffffff", strokes: [SecureDriveStroke] = []) {
        self.width = width
        self.height = height
        self.background = background
        self.strokes = strokes
    }

    public static func validColor(_ color: String) -> Bool {
        let scalars = Array(color.unicodeScalars)
        guard scalars.count == 7, scalars[0] == "#" else { return false }
        return scalars.dropFirst().allSatisfy { ($0.value >= 48 && $0.value <= 57) || ($0.value >= 65 && $0.value <= 70) || ($0.value >= 97 && $0.value <= 102) }
    }

    static func fixed(_ value: Double) -> String { String(format: "%.1f", value) }

    static func escape(_ text: String) -> String {
        text.replacingOccurrences(of: "&", with: "&amp;").replacingOccurrences(of: "\"", with: "&quot;")
            .replacingOccurrences(of: "<", with: "&lt;").replacingOccurrences(of: ">", with: "&gt;")
    }

    public func toSvg() -> [UInt8] {
        var out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        out += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"\(width)\" height=\"\(height)\" viewBox=\"0 0 \(width) \(height)\" data-forkmesh-drawing=\"1\">\n"
        let fill = Self.validColor(background) ? background : "#ffffff"
        out += "    <rect width=\"\(width)\" height=\"\(height)\" fill=\"\(Self.escape(fill))\"/>\n"
        for stroke in strokes where !stroke.points.isEmpty {
            var d = [String]()
            for (i, p) in stroke.points.enumerated() {
                d.append("\(i == 0 ? "M" : "L")\(Self.fixed(p.x)) \(Self.fixed(p.y))")
            }
            if stroke.points.count == 1 {
                d.append("L\(Self.fixed(stroke.points[0].x)) \(Self.fixed(stroke.points[0].y))")
            }
            let color = Self.validColor(stroke.color) ? stroke.color : "#1f2328"
            out += "    <path d=\"\(d.joined(separator: " "))\" fill=\"none\" stroke=\"\(Self.escape(color))\" stroke-width=\"\(Self.fixed(stroke.width))\" stroke-linecap=\"round\" stroke-linejoin=\"round\"/>\n"
        }
        out += "</svg>\n"
        return Array(out.utf8)
    }

    public static func fromSvg(_ bytes: [UInt8]) -> SecureDriveDrawing? {
        let reader = SvgReader()
        let parser = XMLParser(data: Data(bytes))
        parser.shouldProcessNamespaces = true
        parser.shouldResolveExternalEntities = false
        parser.delegate = reader
        let ok = parser.parse()
        guard ok, !reader.failed, reader.marked else { return nil }
        return reader.drawing
    }

    public static func shape(_ tool: SecureDriveShape, from a: SecureDrivePoint, to b: SecureDrivePoint) -> [SecureDrivePoint] {
        switch tool {
        case .line: return [a, b]
        case .box: return [a, SecureDrivePoint(b.x, a.y), b, SecureDrivePoint(a.x, b.y), a]
        case .oval:
            let cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2
            let rx = abs(b.x - a.x) / 2, ry = abs(b.y - a.y) / 2
            return (0...72).map { i in
                let angle = Double(i) * Double.pi * 2 / 72
                return SecureDrivePoint(cx + rx * cos(angle), cy + ry * sin(angle))
            }
        case .pen, .eraser: return [a, b]
        }
    }

    public mutating func erase(at point: SecureDrivePoint, width: Double) -> Bool {
        let radius = max(8.0, width * 2)
        let before = strokes.count
        strokes.removeAll { stroke in
            stroke.points.contains { hypot($0.x - point.x, $0.y - point.y) <= radius + stroke.width / 2 }
        }
        return strokes.count != before
    }
}

final class SvgReader: NSObject, XMLParserDelegate {
    var drawing = SecureDriveDrawing()
    var marked = false
    var background = false
    var failed = false
    var points = 0
    static let command = try! NSRegularExpression(pattern: "([ML])\\s*(-?[0-9]+(?:\\.[0-9]+)?)[\\s,]+(-?[0-9]+(?:\\.[0-9]+)?)")

    func parser(_ parser: XMLParser, didStartElement elementName: String, namespaceURI: String?, qualifiedName qName: String?, attributes: [String: String] = [:]) {
        if failed { return }
        switch elementName {
        case "svg":
            marked = attributes["data-forkmesh-drawing"] == "1"
            drawing.width = min(max(Int(attributes["width"] ?? "") ?? 0, 1), 20000)
            drawing.height = min(max(Int(attributes["height"] ?? "") ?? 0, 1), 20000)
        case "rect" where !background:
            background = true
            if let fill = attributes["fill"], SecureDriveDrawing.validColor(fill) { drawing.background = fill }
        case "path":
            if drawing.strokes.count >= SecureDriveDrawing.maxStrokes {
                failed = true
                parser.abortParsing()
                return
            }
            var stroke = SecureDriveStroke()
            if let color = attributes["stroke"], SecureDriveDrawing.validColor(color) { stroke.color = color }
            if let text = attributes["stroke-width"], let width = Double(text.trimmingCharacters(in: .whitespaces)) {
                stroke.width = min(max(width, 0.5), 200)
            }
            let d = attributes["d"] ?? ""
            let ns = d as NSString
            for match in Self.command.matches(in: d, range: NSRange(location: 0, length: ns.length)) {
                let x = Double(ns.substring(with: match.range(at: 2))) ?? 0
                let y = Double(ns.substring(with: match.range(at: 3))) ?? 0
                stroke.points.append(SecureDrivePoint(x, y))
                points += 1
                if points > SecureDriveDrawing.maxPoints {
                    failed = true
                    parser.abortParsing()
                    return
                }
            }
            if stroke.points.count == 2 && stroke.points[0] == stroke.points[1] { stroke.points.removeLast() }
            if !stroke.points.isEmpty { drawing.strokes.append(stroke) }
        default:
            break
        }
    }

    func parser(_ parser: XMLParser, parseErrorOccurred parseError: Error) { failed = true }
}
