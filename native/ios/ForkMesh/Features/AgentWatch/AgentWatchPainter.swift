// The drawing layer the Watch field's figures are written against: a QPainter
// stand-in (pen, painter, `watchInk`, `drawWatchGlow` and the easing helpers),
// over SwiftUI's GraphicsContext. The figures keep the desktop's QPainter shape on purpose,
// so a change to AgentActivityView.cpp can be carried here line for line.

import SwiftUI

// MARK: - Points

extension CGPoint {
    static func + (lhs: CGPoint, rhs: CGPoint) -> CGPoint { CGPoint(x: lhs.x + rhs.x, y: lhs.y + rhs.y) }
    static func - (lhs: CGPoint, rhs: CGPoint) -> CGPoint { CGPoint(x: lhs.x - rhs.x, y: lhs.y - rhs.y) }
    static func * (lhs: CGPoint, rhs: Double) -> CGPoint { CGPoint(x: lhs.x * rhs, y: lhs.y * rhs) }
    static func / (lhs: CGPoint, rhs: Double) -> CGPoint { CGPoint(x: lhs.x / rhs, y: lhs.y / rhs) }
    static prefix func - (point: CGPoint) -> CGPoint { CGPoint(x: -point.x, y: -point.y) }

    /// The length of the vector.
    var distance: Double { Double(hypot(x, y)) }

    /// `Offset(dx, dy)`.
    init(_ x: Double, _ y: Double) { self.init(x: x, y: y) }
}

/// `Rect.fromLTWH`.
func watchBox(_ x: Double, _ y: Double, _ w: Double, _ h: Double) -> CGRect {
    CGRect(x: x, y: y, width: w, height: h)
}

func watchRadians(_ degrees: Double) -> Double { degrees * Double.pi / 180.0 }

/// A point on a quadratic curve.
func watchOnQuad(_ from: CGPoint, _ control: CGPoint, _ to: CGPoint, _ t: Double) -> CGPoint {
    let u = 1 - t
    return from * (u * u) + control * (2 * u * t) + to * (t * t)
}

func watchEaseOut(_ t: Double) -> Double {
    let t = min(max(t, 0.0), 1.0)
    return 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t)
}

func watchEaseInOut(_ t: Double) -> Double {
    let t = min(max(t, 0.0), 1.0)
    return t * t * (3.0 - 2.0 * t)
}

func watchPolar(_ centre: CGPoint, _ radius: Double, _ angle: Double) -> CGPoint {
    centre + CGPoint(cos(angle), sin(angle)) * radius
}

/// a * b modulo 2^32 (quint32 arithmetic, as the desktop).
func watchMul32(_ a: Int, _ b: Int) -> Int {
    Int(UInt32(truncatingIfNeeded: a) &* UInt32(truncatingIfNeeded: b))
}

/// A stable pseudo-random number in 0..1 (the desktop's `scatter`).
func watchScatter(_ value: Int) -> Double {
    var seed = UInt32(truncatingIfNeeded: value)
    seed ^= seed >> 16
    seed = seed &* 0x7feb_352d
    seed ^= seed >> 15
    seed = seed &* 0x846c_a68b
    seed ^= seed >> 16
    return Double(seed % 100_000) / 100_000.0
}

// MARK: - Colour

/// A colour as straight (not premultiplied) sRGB
/// channels and alpha, 0..1, so alpha can be scaled and channels stepped.
struct WatchRGBA: Equatable {
    var r: Double
    var g: Double
    var b: Double
    var a: Double

    init(_ r: Double, _ g: Double, _ b: Double, _ a: Double = 1) {
        self.r = r
        self.g = g
        self.b = b
        self.a = a
    }

    /// `Color.fromARGB(a, r, g, b)`, 0..255 each.
    init(argb a: Int, _ r: Int, _ g: Int, _ b: Int) {
        self.init(Double(r) / 255, Double(g) / 255, Double(b) / 255, Double(a) / 255)
    }

    /// `Color(0xAARRGGBB)`.
    init(hex: UInt32) {
        self.init(
            argb: Int((hex >> 24) & 0xff), Int((hex >> 16) & 0xff), Int((hex >> 8) & 0xff), Int(hex & 0xff))
    }

    /// `Color.fromRGBO`.
    init(rgbo r: Int, _ g: Int, _ b: Int, _ opacity: Double) {
        self.init(Double(r) / 255, Double(g) / 255, Double(b) / 255, opacity)
    }

    static let clear = WatchRGBA(0, 0, 0, 0)

    func withAlpha(_ alpha: Double) -> WatchRGBA { WatchRGBA(r, g, b, min(max(alpha, 0), 1)) }

    var color: Color { Color(.sRGB, red: r, green: g, blue: b, opacity: a) }
}

/// ink(): the desktop's colour on its dark field (m_dark is always true here):
/// QColor::fromHslF(hue clamped 0..0.9999, sat, light) with alpha.
func watchInk(_ hue: Double, _ saturation: Double, _ lightness: Double, _ alpha: Double = 1) -> WatchRGBA {
    let h = min(max(hue, 0.0), 0.9999) * 360.0
    let s = min(max(saturation, 0.0), 1.0)
    let l = min(max(lightness, 0.0), 1.0)
    let chroma = (1.0 - abs(2.0 * l - 1.0)) * s
    let secondary = chroma * (1.0 - abs((h / 60.0).truncatingRemainder(dividingBy: 2.0) - 1.0))
    let match = l - chroma / 2.0
    var red = 0.0
    var green = 0.0
    var blue = 0.0
    switch h {
    case ..<60: (red, green, blue) = (chroma, secondary, 0)
    case ..<120: (red, green, blue) = (secondary, chroma, 0)
    case ..<180: (red, green, blue) = (0, chroma, secondary)
    case ..<240: (red, green, blue) = (0, secondary, chroma)
    case ..<300: (red, green, blue) = (secondary, 0, chroma)
    default: (red, green, blue) = (chroma, 0, secondary)
    }
    // Each channel is rounded to a byte.
    func byte(_ v: Double) -> Double { ((v + match) * 255).rounded() / 255 }
    return WatchRGBA(byte(red), byte(green), byte(blue), min(max(alpha, 0.0), 1.0))
}

/// A linear or radial gradient shader.
enum WatchShader {
    case linear(from: CGPoint, to: CGPoint, colours: [WatchRGBA], stops: [Double]?)
    case radial(centre: CGPoint, radius: Double, colours: [WatchRGBA], stops: [Double]?)

    var shading: GraphicsContext.Shading {
        switch self {
        case let .linear(from, to, colours, stops):
            return .linearGradient(Self.gradient(colours, stops), startPoint: from, endPoint: to)
        case let .radial(centre, radius, colours, stops):
            return .radialGradient(Self.gradient(colours, stops), center: centre, startRadius: 0, endRadius: radius)
        }
    }

    private static func gradient(_ colours: [WatchRGBA], _ stops: [Double]?) -> Gradient {
        let locations: [Double] = stops ?? colours.indices.map {
            colours.count > 1 ? Double($0) / Double(colours.count - 1) : 0
        }
        return Gradient(stops: zip(colours, locations).map { Gradient.Stop(color: $0.color, location: $1) })
    }
}

/// drawGlow: a two-stop radial gradient, colour at centre to transparent at
/// the radius, drawn at the colour's alpha.
func drawWatchGlow(_ ctx: GraphicsContext, _ centre: CGPoint, _ radius: Double, _ colour: WatchRGBA) {
    if colour.a <= 0.004 || radius <= 0.2 { return }
    // The desktop's sprite cache shares one sprite between colours a step of
    // four apart in each channel; the same step here, so the glows match.
    func step(_ v: Double) -> Double { Double(Int((v * 255).rounded()) & 0xfc) / 255 }
    let solid = WatchRGBA(step(colour.r), step(colour.g), step(colour.b), (colour.a * 255).rounded() / 255)
    let rect = CGRect(x: centre.x - radius, y: centre.y - radius, width: radius * 2, height: radius * 2)
    ctx.fill(
        Path(ellipseIn: rect),
        with: WatchShader.radial(centre: centre, radius: radius, colours: [solid, solid.withAlpha(0)], stops: nil)
            .shading)
}

// MARK: - A QPainter stand-in

enum QtDash { case solid, dash, dot }

/// QPen(colour, width, style, cap, join), with Qt's defaults: width 1,
/// SolidLine, SquareCap, BevelJoin. `shader` is a gradient brush on the pen.
struct QtPen {
    var colour: WatchRGBA
    var width: Double = 1.0
    var dash: QtDash = .solid
    var cap: CGLineCap = .square
    var join: CGLineJoin = .bevel
    var shader: WatchShader?

    init(
        _ colour: WatchRGBA, _ width: Double = 1.0, _ dash: QtDash = .solid, _ cap: CGLineCap = .square,
        _ join: CGLineJoin = .bevel, _ shader: WatchShader? = nil
    ) {
        self.colour = colour
        self.width = width
        self.dash = dash
        self.cap = cap
        self.join = join
        self.shader = shader
    }

    // QPen::setColor replaces the brush, gradient and all.
    func withColour(_ value: WatchRGBA) -> QtPen { QtPen(value, width, dash, cap, join) }
    func withWidth(_ value: Double) -> QtPen { QtPen(colour, value, dash, cap, join, shader) }
    func withDash(_ value: QtDash) -> QtPen { QtPen(colour, width, value, cap, join, shader) }

    var style: StrokeStyle {
        // Qt's DashLine and DotLine patterns, in units of the pen's width;
        // Core Graphics caps every dash, as Qt does.
        let pattern: [CGFloat]
        switch dash {
        case .solid: pattern = []
        case .dash: pattern = [CGFloat(4 * width), CGFloat(2 * width)]
        case .dot: pattern = [CGFloat(width), CGFloat(2 * width)]
        }
        return StrokeStyle(lineWidth: CGFloat(width), lineCap: cap, lineJoin: join, miterLimit: 4, dash: pattern)
    }

    var shading: GraphicsContext.Shading { shader?.shading ?? .color(colour.color) }
}

/// QPainter over a GraphicsContext: the pen and brush as state, Qt's drawing
/// calls on top. Every path fills odd-even, as QPainterPath does.
struct QtPainter {
    private(set) var ctx: GraphicsContext
    private var pen: QtPen?
    private var brush: GraphicsContext.Shading?
    private var saved: [(GraphicsContext, QtPen?, GraphicsContext.Shading?)] = []

    init(_ ctx: GraphicsContext) { self.ctx = ctx }

    mutating func save() { saved.append((ctx, pen, brush)) }

    mutating func restore() {
        guard let state = saved.popLast() else { return }
        (ctx, pen, brush) = state
    }

    mutating func translate(_ by: CGPoint) { ctx.translateBy(x: by.x, y: by.y) }

    /// QPainter::rotate: degrees, clockwise on the y-down screen.
    mutating func rotate(_ degrees: Double) { ctx.rotate(by: .degrees(degrees)) }

    mutating func setPen(_ value: QtPen) { pen = value }
    mutating func noPen() { pen = nil }
    mutating func setBrush(_ colour: WatchRGBA) { brush = .color(colour.color) }
    mutating func setBrushShader(_ shader: WatchShader) { brush = shader.shading }
    mutating func noBrush() { brush = nil }

    private func fill(_ path: Path) {
        if let brush { ctx.fill(path, with: brush, style: FillStyle(eoFill: true)) }
    }

    private func stroke(_ path: Path) {
        if let pen { ctx.stroke(path, with: pen.shading, style: pen.style) }
    }

    private func paint(_ path: Path) {
        fill(path)
        stroke(path)
    }

    /// An ellipse as Qt draws it: from 3 o'clock round, which is where a dash
    /// pattern starts.
    private static func ellipse(_ rect: CGRect) -> Path {
        var path = Path()
        path.addRelativeArc(
            center: .zero, radius: 1, startAngle: .zero, delta: .radians(2 * Double.pi),
            transform: CGAffineTransform(a: rect.width / 2, b: 0, c: 0, d: rect.height / 2, tx: rect.midX, ty: rect.midY))
        path.closeSubpath()
        return path
    }

    func drawEllipse(_ centre: CGPoint, _ rx: Double, _ ry: Double) {
        // QPainter normalizes the rectangle.
        let rect = CGRect(x: centre.x - abs(rx), y: centre.y - abs(ry), width: 2 * abs(rx), height: 2 * abs(ry))
        paint(Self.ellipse(rect))
    }

    func drawRect(_ rect: CGRect) { paint(Path(rect)) }

    /// drawRoundedRect(rect, xRadius, yRadius) in Qt::AbsoluteSize.
    func drawRoundedRect(_ rect: CGRect, _ xRadius: Double, _ yRadius: Double) {
        paint(Path(roundedRect: rect, cornerSize: CGSize(width: xRadius, height: yRadius)))
    }

    func drawLine(_ from: CGPoint, _ to: CGPoint) {
        var path = Path()
        path.move(to: from)
        path.addLine(to: to)
        stroke(path)
    }

    /// A point is a dot the pen's width across, round or square by its cap.
    func drawPoint(_ at: CGPoint) {
        guard let pen else { return }
        let half = pen.width / 2
        let rect = CGRect(x: at.x - half, y: at.y - half, width: pen.width, height: pen.width)
        ctx.fill(pen.cap == .round ? Path(ellipseIn: rect) : Path(rect), with: pen.shading)
    }

    /// The arc of `rect`'s ellipse from `start16` for `span16`, in sixteenths
    /// of a degree anticlockwise from 3 o'clock, as Qt measures.
    static func arc(_ rect: CGRect, _ start16: Int, _ span16: Int, into path: inout Path) {
        path.addRelativeArc(
            center: .zero, radius: 1, startAngle: .radians(-watchRadians(Double(start16) / 16.0)),
            delta: .radians(-watchRadians(Double(span16) / 16.0)),
            transform: CGAffineTransform(a: rect.width / 2, b: 0, c: 0, d: rect.height / 2, tx: rect.midX, ty: rect.midY))
    }

    func drawArc(_ rect: CGRect, _ start16: Int, _ span16: Int) {
        var path = Path()
        Self.arc(rect, start16, span16, into: &path)
        stroke(path)
    }

    func drawPie(_ rect: CGRect, _ start16: Int, _ span16: Int) {
        var path = Path()
        path.move(to: CGPoint(x: rect.midX, y: rect.midY))
        Self.arc(rect, start16, span16, into: &path)
        path.closeSubpath()
        paint(path)
    }

    func drawPath(_ path: Path) { paint(path) }

    func drawPolygon(_ points: [CGPoint]) {
        var path = Path()
        path.addLines(points)
        path.closeSubpath()
        paint(path)
    }

    func drawPolyline(_ points: [CGPoint]) {
        var path = Path()
        path.addLines(points)
        stroke(path)
    }

    /// drawText(rect, Qt::AlignCenter, text) in a bold font `pixelSize` high,
    /// in the pen's colour.
    func drawText(_ rect: CGRect, _ text: String, _ pixelSize: Int) {
        guard let pen, pixelSize > 0 else { return }
        let resolved = ctx.resolve(
            Text(text).font(.system(size: CGFloat(pixelSize), weight: .bold)).foregroundColor(pen.colour.color))
        ctx.draw(resolved, at: CGPoint(x: rect.midX, y: rect.midY), anchor: .center)
    }
}
