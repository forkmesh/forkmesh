import Foundation

public struct CallOutfitColor: Hashable, Sendable {
    public let r: Double
    public let g: Double
    public let b: Double
    public let a: Double

    public init(r: Double, g: Double, b: Double, a: Double = 1) {
        self.r = r
        self.g = g
        self.b = b
        self.a = a
    }

    public init(hex: String) {
        let value = Int(hex.dropFirst(), radix: 16) ?? 0
        self.init(r: Double((value >> 16) & 255), g: Double((value >> 8) & 255), b: Double(value & 255))
    }

    public init(css: String) {
        if css.hasPrefix("#") {
            self.init(hex: css)
            return
        }
        let inside = css.drop { $0 != "(" }.dropFirst().prefix { $0 != ")" }
        let parts = inside.split(separator: ",").map { Double($0.trimmingCharacters(in: .whitespaces)) ?? 0 }
        self.init(r: parts[0], g: parts[1], b: parts[2], a: parts.count > 3 ? parts[3] : 1)
    }
}

public enum CallOutfitFabricKind: String, Sendable {
    case wool, jersey, heather, rib, knit, denim, plaid
}

public struct CallOutfitFabric: Hashable, Sendable {
    public let kind: CallOutfitFabricKind
    public let colors: [String]

    public init(_ kind: CallOutfitFabricKind, _ colors: [String]) {
        self.kind = kind
        self.colors = colors
    }
}

public struct CallOutfitGradientStop: Equatable, Sendable {
    public let offset: Double
    public let color: CallOutfitColor
}

public struct CallOutfitGradient: Equatable, Sendable {
    public let x0: Double
    public let y0: Double
    public let x1: Double
    public let y1: Double
    public let stops: [CallOutfitGradientStop]
}

public enum CallOutfitPaint: Equatable, Sendable {
    case color(CallOutfitColor)
    case fabric(CallOutfitFabric)
    case gradient(CallOutfitGradient)
}

public enum CallOutfitCommand: Equatable, Sendable {
    case save
    case restore
    case beginPath
    case closePath
    case fill
    case stroke
    case clip
    case moveTo(Double, Double)
    case lineTo(Double, Double)
    case quadTo(Double, Double, Double, Double)
    case arc(Double, Double, Double, Double, Double)
    case fillRect(Double, Double, Double, Double)
    case translate(Double, Double)
    case rotate(Double)
    case fillStyle(CallOutfitPaint)
    case strokeStyle(CallOutfitPaint)
    case lineWidth(Double)
    case lineCap(String)
    case lineJoin(String)
    case lineDash([Double])
    case composite(String)
}

public enum CallOutfitDrawing {
    public static func commands(_ id: String) -> [CallOutfitCommand]? {
        let pen = Pen()
        switch id {
        case "suit": pen.suit()
        case "hoodie": pen.hoodie()
        case "sweater": pen.sweater()
        case "denim": pen.denim()
        case "flannel": pen.flannel()
        case "turtleneck": pen.turtleneck()
        default: return nil
        }
        return pen.ops
    }

    public static func fabricTile(_ fabric: CallOutfitFabric) -> [UInt8] {
        let side = CallOutfits.tile
        var data = [UInt8](repeating: 0, count: side * side * 4)
        let colors = fabric.colors.map(rgb)
        for y in 0..<side {
            for x in 0..<side {
                let shade = self.shade(fabric.kind, colors, x, y)
                let at = (y * side + x) * 4
                data[at] = clampByte(shade[0])
                data[at + 1] = clampByte(shade[1])
                data[at + 2] = clampByte(shade[2])
                data[at + 3] = 255
            }
        }
        return data
    }

    static func clampByte(_ value: Double) -> UInt8 {
        if !(value > 0) { return 0 }
        if value >= 255 { return 255 }
        return UInt8(value.rounded(.toNearestOrEven))
    }

    static func rgb(_ hex: String) -> [Double] {
        let value = Int(hex.dropFirst(), radix: 16) ?? 0
        return [Double((value >> 16) & 255), Double((value >> 8) & 255), Double(value & 255)]
    }

    static func hash(_ x: Int, _ y: Int, _ seed: Int) -> Double {
        let sum = Int64(x) * 374_761_393 + Int64(y) * 668_265_263 + Int64(seed) * 2_147_483_647
        var h = UInt32(truncatingIfNeeded: sum)
        h = (h ^ (h >> 13)) &* 1_274_126_177
        return Double(h ^ (h >> 16)) / 4_294_967_295
    }

    static func wave(_ value: Int, _ period: Double) -> Double {
        sin((Double(value) * 2 * Double.pi) / period)
    }

    static func scaled(_ color: [Double], _ k: Double) -> [Double] {
        color.map { CallFaceJS.max(0, CallFaceJS.min(255, $0 * k)) }
    }

    static func band(_ v: Int) -> Int {
        let m = v % 128
        if m < 36 { return 2 }
        if m < 44 || (m >= 76 && m < 84) { return 1 }
        if m >= 100 && m < 104 { return 3 }
        return 0
    }

    static func shade(_ kind: CallOutfitFabricKind, _ colors: [[Double]], _ x: Int, _ y: Int) -> [Double] {
        let base = colors[0]
        switch kind {
        case .wool:
            return scaled(base, 0.96 + 0.05 * hash(x, y, 1) + 0.025 * wave(x + y, 4))
        case .jersey:
            return scaled(base, 0.97 + 0.045 * hash(x, y, 2))
        case .heather:
            let fleck = hash(x >> 1, y, 3)
            return scaled(base, 0.9 + 0.12 * hash(x, y, 4) + (fleck > 0.93 ? 0.16 : fleck < 0.05 ? -0.12 : 0))
        case .rib:
            return scaled(base, 0.9 + 0.1 * abs(wave(x, 8)) + 0.04 * hash(x, y, 5))
        case .knit:
            let cx = (x % 8) - 4
            let cy = Double((y + (abs(cx) > 0 ? abs(cx) : 0)) % 6) / 6
            return scaled(base, 0.82 + 0.16 * sin(cy * Double.pi) + 0.05 * hash(x, y, 6) - 0.06 * (abs(cx) < 1 ? 1 : 0))
        case .denim:
            let twill = 0.5 + 0.5 * wave(x + y * 2, 6)
            let weft = hash(x, y, 7) > 0.82 ? 1.22 : 1
            return scaled(base, (0.86 + 0.16 * twill + 0.06 * hash(x >> 2, y, 8)) * weft)
        case .plaid:
            let a = band(x)
            let b = band(y)
            let pick = (y + x) % 4 < 2 ? a : b
            let other = colors[a == b ? a : Swift.max(a, b)]
            let mixed = colors[pick].enumerated().map { i, c in (c + other[i]) / 2 }
            return scaled(mixed, 0.94 + 0.08 * hash(x, y, 9) + 0.03 * wave(x - y, 4))
        }
    }

    final class Pen {
        typealias P = (Double, Double)
        var ops: [CallOutfitCommand] = []
        let unit = CallOutfits.unit
        let size = CallOutfits.size

        func at(_ x: Double, _ y: Double) -> P {
            (CallOutfits.neck.x + x * unit, CallOutfits.neck.y + y * unit)
        }

        func color(_ css: String) -> CallOutfitPaint {
            .color(CallOutfitColor(css: css))
        }

        func shade(_ r: Double, _ g: Double, _ b: Double, _ a: Double) -> CallOutfitPaint {
            .color(CallOutfitColor(r: r, g: g, b: b, a: a))
        }

        func fabric(_ kind: CallOutfitFabricKind, _ colors: String...) -> CallOutfitPaint {
            .fabric(CallOutfitFabric(kind, colors))
        }

        func moveTo(_ p: P) { ops.append(.moveTo(p.0, p.1)) }
        func lineTo(_ p: P) { ops.append(.lineTo(p.0, p.1)) }
        func quadTo(_ c: P, _ p: P) { ops.append(.quadTo(c.0, c.1, p.0, p.1)) }

        func path(_ points: [P], close: Bool = true) {
            ops.append(.beginPath)
            for (i, point) in points.enumerated() {
                if i == 0 { moveTo(at(point.0, point.1)) } else { lineTo(at(point.0, point.1)) }
            }
            if close { ops.append(.closePath) }
        }

        func curve(_ from: P, _ steps: [[Double]]) {
            ops.append(.beginPath)
            moveTo(at(from.0, from.1))
            for step in steps {
                if step.count == 4 { quadTo(at(step[0], step[1]), at(step[2], step[3])) } else { lineTo(at(step[0], step[1])) }
            }
        }

        func fillWith(_ style: CallOutfitPaint, _ draw: () -> Void) {
            ops.append(.save)
            ops.append(.fillStyle(style))
            draw()
            ops.append(.fill)
            ops.append(.restore)
        }

        func stroke(_ style: CallOutfitPaint, _ widthUnits: Double, dash: [Double]? = nil, _ draw: () -> Void) {
            ops.append(.save)
            ops.append(.strokeStyle(style))
            ops.append(.lineWidth(widthUnits * unit))
            ops.append(.lineCap("round"))
            ops.append(.lineJoin("round"))
            if let dash { ops.append(.lineDash(dash.map { $0 * unit })) }
            draw()
            ops.append(.stroke)
            ops.append(.restore)
        }

        func shoulderLine(_ neck: Double) -> [P] {
            var line: [P] = []
            var x = -5.5
            while x <= -neck {
                line.append((x, -0.75 + 0.3 * (-x - 0.9)))
                x += 0.25
            }
            return line
        }

        func cutTop(_ neckHalf: Double, _ neckline: () -> Void) {
            ops.append(.save)
            ops.append(.composite("destination-out"))
            let left = shoulderLine(neckHalf)
            let right = left.map { (-$0.0, $0.1) }.reversed()
            ops.append(.beginPath)
            moveTo(at(-5.6, -2.2))
            for point in left { lineTo(at(point.0, point.1)) }
            neckline()
            for point in right { lineTo(at(point.0, point.1)) }
            lineTo(at(5.6, -2.2))
            ops.append(.closePath)
            ops.append(.fill)
            ops.append(.restore)
        }

        func roundNeck(_ half: Double, _ depth: Double, _ top: Double = -0.7) -> () -> Void {
            { [self] in
                lineTo(at(-half, top))
                quadTo(at(-half * 0.92, depth), at(0, depth))
                quadTo(at(half * 0.92, depth), at(half, top))
            }
        }

        func gradient(_ from: P, _ to: P, _ stops: [(Double, CallOutfitPaint)]) -> CallOutfitPaint {
            let list = stops.map { offset, paint -> CallOutfitGradientStop in
                guard case let .color(color) = paint else { return CallOutfitGradientStop(offset: offset, color: CallOutfitColor(r: 0, g: 0, b: 0, a: 0)) }
                return CallOutfitGradientStop(offset: offset, color: color)
            }
            return .gradient(CallOutfitGradient(x0: from.0, y0: from.1, x1: to.0, y1: to.1, stops: list))
        }

        func soften() {
            ops.append(.save)
            ops.append(.composite("source-atop"))
            let under = gradient(at(0, -0.6), at(0, 1.6), [(0, color("rgba(0,0,0,0.28)")), (1, color("rgba(0,0,0,0)"))])
            ops.append(.fillStyle(under))
            ops.append(.fillRect(0, 0, size, size))
            for side in [-1.0, 1.0] {
                let edge = gradient(at(side * 1.8, 0), at(side * 3.4, 0), [(0, color("rgba(0,0,0,0)")), (1, color("rgba(0,0,0,0.18)"))])
                ops.append(.fillStyle(edge))
                let x0 = at(side * 1.8, 0).0
                ops.append(.fillRect(side < 0 ? 0 : x0, 0, side < 0 ? x0 : size - x0, size))
            }
            ops.append(.restore)
        }

        func folds(_ lines: [[Double]], _ strength: Double = 0.12) {
            for line in lines {
                let x0 = line[0], y0 = line[1], x1 = line[2], y1 = line[3]
                stroke(shade(0, 0, 0, strength), 0.09) { curve((x0, y0), [[(x0 + x1) / 2 + 0.15, (y0 + y1) / 2, x1, y1]]) }
                stroke(shade(255, 255, 255, strength * 0.5), 0.05) { curve((x0 + 0.08, y0), [[(x0 + x1) / 2 + 0.23, (y0 + y1) / 2, x1 + 0.08, y1]]) }
            }
        }

        func button(_ x: Double, _ y: Double, _ radius: Double, _ fill: String, _ rim: String = "rgba(0,0,0,0.45)") {
            ops.append(.save)
            ops.append(.fillStyle(color(fill)))
            ops.append(.strokeStyle(color(rim)))
            ops.append(.lineWidth(0.025 * unit))
            ops.append(.beginPath)
            let center = at(x, y)
            ops.append(.arc(center.0, center.1, radius * unit, 0, Double.pi * 2))
            ops.append(.fill)
            ops.append(.stroke)
            ops.append(.restore)
        }

        func suit() {
            ops.append(.fillStyle(fabric(.wool, "#33373e")))
            ops.append(.fillRect(0, 0, size, size))
            fillWith(fabric(.jersey, "#eceff3")) { path([(-1.15, -0.8), (0, 3.4), (1.15, -0.8)]) }
            fillWith(fabric(.wool, "#6a2234")) { path([(-0.2, 0.05), (0.2, 0.05), (0.13, 0.42), (-0.13, 0.42)]) }
            fillWith(fabric(.wool, "#5e1d2e")) { path([(-0.13, 0.42), (0.13, 0.42), (0.3, 3.0), (0, 3.35), (-0.3, 3.0)]) }
            ops.append(.save)
            ops.append(.beginPath)
            path([(-0.13, 0.42), (0.13, 0.42), (0.3, 3.0), (0, 3.35), (-0.3, 3.0)])
            ops.append(.clip)
            var y = 0.2
            while y < 3.6 {
                let row = y
                stroke(color("rgba(214,178,120,0.55)"), 0.05) { path([(-0.5, row), (0.5, row + 0.5)], close: false) }
                y += 0.42
            }
            ops.append(.restore)
            for side in [-1.0, 1.0] {
                fillWith(color("#f4f6f9")) { path([(side * 0.82, -0.78), (side * 0.18, 0.08), (side * 0.36, 0.58), (side * 1.0, -0.28)]) }
                stroke(color("rgba(0,0,0,0.25)"), 0.025) { path([(side * 0.18, 0.08), (side * 0.36, 0.58), (side * 1.0, -0.28)], close: false) }
            }
            for side in [-1.0, 1.0] {
                let lapel: [P] = [(side * 1.15, -0.8), (side * 1.62, 0.95), (side * 1.32, 1.12), (side * 1.5, 1.32), (side * 0.05, 3.42), (side * 0.02, 3.4)]
                fillWith(fabric(.wool, "#3a3f47")) { path(lapel) }
                stroke(color("rgba(0,0,0,0.35)"), 0.04) { path(Array(lapel[0..<5]), close: false) }
                stroke(color("rgba(0,0,0,0.3)"), 0.12) { path([(side * 1.17, -0.75), (side * 0.08, 3.35)], close: false) }
            }
            stroke(color("rgba(0,0,0,0.4)"), 0.03) { path([(0.02, 3.4), (0.12, 9.5)], close: false) }
            button(0.1, 3.85, 0.12, "#1d1f23")
            button(0.1, 5.05, 0.12, "#1d1f23")
            stroke(color("rgba(0,0,0,0.4)"), 0.05) { path([(1.35, 2.3), (2.35, 2.18)], close: false) }
            fillWith(color("#e9edf2")) { path([(1.55, 2.29), (1.75, 2.0), (1.92, 2.18), (2.1, 1.98), (2.2, 2.2)]) }
            folds([[-2.3, 1.6, -2.1, 4.6], [2.2, 1.7, 2.0, 4.5], [-0.9, 3.9, -0.7, 6.0]], 0.1)
            cutTop(0.82, roundNeck(0.74, -0.15, -0.82))
            soften()
        }

        func hoodie() {
            ops.append(.fillStyle(fabric(.heather, "#8e9299")))
            ops.append(.fillRect(0, 0, size, size))
            ops.append(.save)
            curve((-1.85, -0.55), [[-1.5, 1.25, 0, 1.2], [1.5, 1.25, 1.85, -0.55]])
            lineTo(at(1.05, -0.6))
            quadTo(at(0.9, 0.45), at(0, 0.45))
            quadTo(at(-0.9, 0.45), at(-1.05, -0.6))
            ops.append(.closePath)
            ops.append(.fillStyle(fabric(.heather, "#9a9ea5")))
            ops.append(.fill)
            ops.append(.clip)
            let rim = gradient(at(0, -0.4), at(0, 1.25), [(0, color("rgba(255,255,255,0.12)")), (0.7, color("rgba(0,0,0,0)")), (1, color("rgba(0,0,0,0.3)"))])
            ops.append(.fillStyle(rim))
            ops.append(.fillRect(0, 0, size, size))
            ops.append(.restore)
            stroke(color("rgba(0,0,0,0.3)"), 0.07) { curve((-1.85, -0.55), [[-1.5, 1.25, 0, 1.2], [1.5, 1.25, 1.85, -0.55]]) }
            stroke(color("rgba(0,0,0,0.2)"), 0.04) { curve((-1.05, -0.6), [[-0.9, 0.45, 0, 0.45], [0.9, 0.45, 1.05, -0.6]]) }
            for (x0, x1, end) in [(-0.36, -0.44, 3.3), (0.36, 0.3, 3.55)] {
                button(x0, 1.0, 0.07, "#6f737a", "rgba(0,0,0,0.5)")
                stroke(color("#e6e2d8"), 0.085) { curve((x0, 1.0), [[x0 - 0.05, (1 + end) / 2, x1, end]]) }
                stroke(color("#2b2d31"), 0.11) { path([(x1, end), (x1, end + 0.3)], close: false) }
            }
            stroke(color("rgba(0,0,0,0.22)"), 0.05) { curve((-1.9, 6.4), [[0, 5.8, 1.9, 6.4]]) }
            folds([[-2.2, 1.4, -1.9, 4.8], [2.1, 1.5, 1.9, 4.6], [-0.9, 2.2, -1.1, 5.2], [1.0, 2.4, 0.8, 5.4]], 0.13)
            cutTop(1.05, roundNeck(0.92, 0.05, -0.6))
            soften()
        }

        func sweater() {
            ops.append(.fillStyle(fabric(.knit, "#2d3d5e")))
            ops.append(.fillRect(0, 0, size, size))
            for cx in [-1.15, 0, 1.15] {
                var y = 0.65
                while y < 10.5 {
                    let row = y
                    for dir in [-1.0, 1.0] {
                        stroke(color("rgba(0,0,0,0.32)"), 0.13) { curve((cx - dir * 0.2, row), [[cx + dir * 0.3, row + 0.35, cx + dir * 0.2, row + 0.7]]) }
                        stroke(color("rgba(150,170,210,0.22)"), 0.06) { curve((cx - dir * 0.2, row - 0.03), [[cx + dir * 0.3, row + 0.32, cx + dir * 0.2, row + 0.67]]) }
                    }
                    y += 0.7
                }
                for side in [-1.0, 1.0] {
                    stroke(color("rgba(0,0,0,0.28)"), 0.05) { path([(cx + side * 0.36, 0.6), (cx + side * 0.36, 10.5)], close: false) }
                }
            }
            ops.append(.save)
            curve((-1.25, -0.72), [[-1.15, 0.48, 0, 0.48], [1.15, 0.48, 1.25, -0.72]])
            lineTo(at(0.92, -0.72))
            quadTo(at(0.85, 0.1), at(0, 0.1))
            quadTo(at(-0.85, 0.1), at(-0.92, -0.72))
            ops.append(.closePath)
            ops.append(.fillStyle(fabric(.rib, "#34466b")))
            ops.append(.fill)
            ops.append(.clip)
            var k = -1.0
            while k <= 1.001 {
                let angle = k * 1.25
                let inner: P = (sin(angle) * 0.92, 0.1 - (1 - cos(angle)) * 0.75)
                let outer: P = (sin(angle) * 1.25, 0.48 - (1 - cos(angle)) * 1.0)
                stroke(color("rgba(0,0,0,0.22)"), 0.025) { path([inner, outer], close: false) }
                k += 0.06
            }
            ops.append(.restore)
            stroke(color("rgba(0,0,0,0.3)"), 0.04) { curve((-1.25, -0.72), [[-1.15, 0.48, 0, 0.48], [1.15, 0.48, 1.25, -0.72]]) }
            folds([[-2.3, 1.4, -2.0, 4.6], [2.2, 1.5, 2.0, 4.6]], 0.1)
            cutTop(0.92, roundNeck(0.86, 0.1, -0.72))
            soften()
        }

        func denim() {
            ops.append(.fillStyle(fabric(.denim, "#4b6f9e")))
            ops.append(.fillRect(0, 0, size, size))
            let tee: [P] = [(-0.95, -0.9), (0.95, -0.9), (0.8, 10.5), (-0.8, 10.5)]
            fillWith(fabric(.jersey, "#efece5")) { path(tee) }
            stroke(color("rgba(0,0,0,0.18)"), 0.06) { curve((-0.78, -0.75), [[-0.7, 0.3, 0, 0.3], [0.7, 0.3, 0.78, -0.75]]) }
            for side in [-1.0, 1.0] {
                let edge: [P] = [(side * 0.92, -0.2), (side * 0.8, 10.5)]
                stroke(color("rgba(0,0,0,0.35)"), 0.14) { path(edge.map { ($0.0 - side * 0.02, $0.1) }, close: false) }
                stroke(color("#c8873a"), 0.025, dash: [0.09, 0.07]) { path(edge.map { ($0.0 + side * 0.12, $0.1) }, close: false) }
                stroke(color("#c8873a"), 0.025, dash: [0.09, 0.07]) { path([(side * 0.92, 1.45), (side * 3.2, 1.25)], close: false) }
                stroke(color("#c8873a"), 0.025, dash: [0.09, 0.07]) { path([(side * 0.92, 1.58), (side * 3.2, 1.38)], close: false) }
                let flap: [P] = [(side * 1.35, 1.85), (side * 2.45, 1.85), (side * 2.45, 2.25), (side * 1.9, 2.5), (side * 1.35, 2.25)]
                fillWith(fabric(.denim, "#45679a")) { path(flap) }
                stroke(color("rgba(0,0,0,0.35)"), 0.035) { path(flap) }
                stroke(color("#c8873a"), 0.022, dash: [0.08, 0.06]) { path(flap.map { ($0.0 - side * 0.0, $0.1 + 0.07) }, close: true) }
                stroke(color("rgba(0,0,0,0.25)"), 0.03) { path([(side * 1.4, 2.3), (side * 1.4, 3.6), (side * 2.4, 3.6), (side * 2.4, 2.3)], close: false) }
                button(side * 1.9, 2.3, 0.08, "#b07a3c")
                let collar: [P] = [(side * 0.95, -0.95), (side * 1.85, -0.4), (side * 1.5, 1.25), (side * 0.92, 0.45)]
                fillWith(fabric(.denim, "#557aab")) { path(collar) }
                stroke(color("rgba(0,0,0,0.38)"), 0.04) { path(collar) }
                stroke(color("#c8873a"), 0.022, dash: [0.08, 0.06]) { path([(side * 1.75, -0.35), (side * 1.43, 1.08), (side * 1.0, 0.45)], close: false) }
            }
            var y = 1.9
            while y < 10 {
                button(-1.05, y, 0.09, "#b07a3c")
                y += 1.25
            }
            folds([[-2.3, 2.2, -2.0, 5.0], [2.3, 2.2, 2.0, 5.0]], 0.12)
            cutTop(0.95, roundNeck(0.72, 0.1, -0.9))
            soften()
        }

        func flannel() {
            let plaid = fabric(.plaid, "#a8323a", "#2a2a2e", "#5a1c22", "#e6d9b8")
            ops.append(.fillStyle(plaid))
            ops.append(.fillRect(0, 0, size, size))
            stroke(color("rgba(0,0,0,0.3)"), 0.03) { path([(-0.2, 0.4), (-0.2, 10.5)], close: false) }
            stroke(color("rgba(0,0,0,0.3)"), 0.03) { path([(0.2, 0.4), (0.2, 10.5)], close: false) }
            var y = 0.9
            while y < 10 {
                button(0, y, 0.085, "#ece6d6")
                y += 1.05
            }
            for side in [-1.0, 1.0] {
                let pocket: [P] = [(side * 1.25, 1.75), (side * 2.3, 1.75), (side * 2.3, 3.15), (side * 1.78, 3.4), (side * 1.25, 3.15)]
                ops.append(.save)
                path(pocket)
                ops.append(.clip)
                let corner = at(side * 1.78, 2.5)
                ops.append(.translate(corner.0, corner.1))
                ops.append(.rotate(Double.pi / 4))
                ops.append(.fillStyle(plaid))
                ops.append(.fillRect(-200, -200, 400, 400))
                ops.append(.restore)
                stroke(color("rgba(0,0,0,0.38)"), 0.035) { path(pocket) }
                stroke(color("rgba(0,0,0,0.3)"), 0.03) { path([(side * 1.25, 2.1), (side * 2.3, 2.1)], close: false) }
                let collar: [P] = [(side * 0.86, -0.85), (side * 1.6, -0.35), (side * 1.0, 1.0), (side * 0.15, 0.45)]
                fillWith(fabric(.plaid, "#b23a42", "#303034", "#62202a", "#ecdfbf")) { path(collar) }
                stroke(color("rgba(0,0,0,0.4)"), 0.04) { path(collar) }
            }
            folds([[-2.3, 1.8, -2.0, 4.8], [2.2, 1.8, 2.0, 4.8]], 0.13)
            cutTop(0.86, roundNeck(0.78, 0.25, -0.85))
            soften()
        }

        func turtleneck() {
            ops.append(.fillStyle(fabric(.rib, "#1e1f23")))
            ops.append(.fillRect(0, 0, size, size))
            ops.append(.save)
            curve((-0.86, -1.18), [[0, -1.0, 0.86, -1.18], [0.92, -0.72], [0, -0.55, -0.92, -0.72]])
            ops.append(.closePath)
            ops.append(.fillStyle(color("rgba(255,255,255,0.07)")))
            ops.append(.fill)
            ops.append(.restore)
            stroke(color("rgba(0,0,0,0.5)"), 0.06) { curve((-0.92, -0.72), [[0, -0.55, 0.92, -0.72]]) }
            for side in [-1.0, 1.0] {
                stroke(color("rgba(0,0,0,0.35)"), 0.12) { curve((side * 0.9, -0.7), [[side * 1.05, -0.2, side * 1.6, -0.05]]) }
            }
            folds([[-2.2, 1.5, -2.0, 4.8], [2.2, 1.5, 2.0, 4.8], [-0.6, -0.3, -0.5, 0.2], [0.55, -0.35, 0.5, 0.15]], 0.1)
            ops.append(.save)
            ops.append(.composite("destination-out"))
            ops.append(.beginPath)
            moveTo(at(-5.6, -2.2))
            for point in shoulderLine(0.86) { lineTo(at(point.0, point.1)) }
            lineTo(at(-0.86, -1.18))
            quadTo(at(0, -1.0), at(0.86, -1.18))
            for point in shoulderLine(0.86).map({ (-$0.0, $0.1) }).reversed() { lineTo(at(point.0, point.1)) }
            lineTo(at(5.6, -2.2))
            ops.append(.closePath)
            ops.append(.fill)
            ops.append(.restore)
            soften()
        }
    }
}
