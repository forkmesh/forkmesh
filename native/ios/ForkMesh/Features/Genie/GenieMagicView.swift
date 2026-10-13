import FMAssistant
import SwiftUI

struct GenieMagicView: View {
    let glimmers: [GenieGlimmer]
    let front: Bool

    var body: some View {
        Canvas { (context: inout GraphicsContext, _: CGSize) in
            for glimmer in glimmers where glimmer.front == front {
                GenieMagicPainter.draw(glimmer, in: &context)
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

enum GenieMagicPainter {
    static let darkGold: Color = Color(red: 0.72, green: 0.52, blue: 0.16)

    static func ink(_ glimmer: GenieGlimmer) -> Color {
        switch glimmer.tone {
        case .glow: return GenieInk.glow
        case .lilac: return GenieInk.lilac
        case .violet: return GenieInk.violet
        case .plum: return GenieInk.plum
        case .gold: return GenieInk.gold
        case .mist: return GenieInk.mist
        case .spectrum:
            let hue: Double = glimmer.hue - glimmer.hue.rounded(.down)
            return Color(hue: hue, saturation: 0.72, brightness: 1)
        }
    }

    static func draw(_ glimmer: GenieGlimmer, in context: inout GraphicsContext) {
        guard glimmer.opacity > 0.01 else { return }
        var layer: GraphicsContext = context
        layer.opacity = min(max(glimmer.opacity, 0), 1)
        let at: CGPoint = CGPoint(x: glimmer.x, y: glimmer.y)
        let r: CGFloat = CGFloat(glimmer.size)
        let color: Color = ink(glimmer)
        switch glimmer.shape {
        case .puff: puff(&layer, at, r, color)
        case .orb: orb(&layer, at, r, color)
        case .spark: spark(&layer, at, r, color, angle: glimmer.angle)
        case .star: star(&layer, at, r, color)
        case .dust:
            soft(&layer, at, r * 2.6, color.opacity(0.4))
            layer.fill(circle(at, r), with: .color(color))
        case .ring: ring(&layer, at, r, color, glimmer)
        case .band: band(&layer, at, r, color, glimmer)
        case .beam: beam(&layer, at, CGPoint(x: glimmer.x2, y: glimmer.y2), CGFloat(glimmer.weight), color)
        case .arc, .line: curve(&layer, glimmer, color)
        case .lamp: lamp(&layer, at, r, glimmer)
        case .carpet: carpet(&layer, at, r, glimmer)
        case .crystal: crystal(&layer, at, r, glimmer)
        case .rune: rune(&layer, at, r, color, glimmer)
        case .bubble: bubble(&layer, at, r, color)
        case .coin: coin(&layer, at, r, glimmer)
        case .moon: moon(&layer, at, r, color, glimmer)
        case .bottle: bottle(&layer, at, r, glimmer)
        case .outline: outline(&layer, color, glimmer)
        case .wisp: wisp(&layer, at, r, color, glimmer)
        case .glow: glow(&layer, at, r, color, glimmer)
        }
    }

    static func circle(_ c: CGPoint, _ r: CGFloat) -> Path {
        Path(ellipseIn: CGRect(x: c.x - r, y: c.y - r, width: r * 2, height: r * 2))
    }

    static func soft(_ context: inout GraphicsContext, _ c: CGPoint, _ r: CGFloat, _ color: Color) {
        context.fill(
            circle(c, r), with: .radialGradient(Gradient(colors: [color, color.opacity(0)]), center: c, startRadius: 0, endRadius: r))
    }

    static func puff(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color) {
        let lobes: [(CGFloat, CGFloat, CGFloat)] = [(-0.45, 0.1, 0.75), (0.4, 0.15, 0.7), (0, -0.25, 0.85)]
        for lobe in lobes {
            let c: CGPoint = CGPoint(x: at.x + r * lobe.0, y: at.y + r * lobe.1)
            let size: CGFloat = r * lobe.2
            context.fill(
                circle(c, size),
                with: .radialGradient(
                    Gradient(colors: [color.opacity(0.85), color.opacity(0.4), color.opacity(0)]), center: c, startRadius: 0, endRadius: size))
        }
    }

    static func orb(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color) {
        soft(&context, at, r * 2.4, color.opacity(0.5))
        context.fill(
            circle(at, r),
            with: .radialGradient(
                Gradient(colors: [Color.white, color, color.opacity(0.7)]), center: CGPoint(x: at.x - r * 0.3, y: at.y - r * 0.3), startRadius: 0,
                endRadius: r * 1.3))
    }

    static func spark(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color, angle: Double) {
        var path: Path = Path()
        for k in 0..<8 {
            let a: Double = Double(k) / 8 * Double.pi * 2 + angle * Double.pi / 180
            let reach: CGFloat = k % 2 == 0 ? r : r * 0.28
            let point: CGPoint = CGPoint(x: at.x + CGFloat(cos(a)) * reach, y: at.y + CGFloat(sin(a)) * reach)
            if k == 0 { path.move(to: point) } else { path.addLine(to: point) }
        }
        path.closeSubpath()
        soft(&context, at, r * 1.7, color.opacity(0.4))
        context.fill(path, with: .color(color))
        context.fill(circle(at, r * 0.22), with: .color(Color.white.opacity(0.9)))
    }

    static func star(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color) {
        var path: Path = Path()
        for k in 0..<10 {
            let a: Double = Double(k) / 10 * Double.pi * 2 - Double.pi / 2
            let reach: CGFloat = k % 2 == 0 ? r : r * 0.45
            let point: CGPoint = CGPoint(x: at.x + CGFloat(cos(a)) * reach, y: at.y + CGFloat(sin(a)) * reach)
            if k == 0 { path.move(to: point) } else { path.addLine(to: point) }
        }
        path.closeSubpath()
        soft(&context, at, r * 2, color.opacity(0.35))
        context.fill(path, with: .color(color))
    }

    static func ring(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color, _ glimmer: GenieGlimmer) {
        var layer: GraphicsContext = context
        layer.translateBy(x: at.x, y: at.y)
        layer.rotate(by: .degrees(glimmer.angle))
        let squash: CGFloat = CGFloat(glimmer.squash)
        let oval: Path = Path(ellipseIn: CGRect(x: -r, y: -r * squash, width: r * 2, height: r * 2 * squash))
        let width: CGFloat = CGFloat(glimmer.weight)
        layer.stroke(oval, with: .color(color.opacity(0.3)), lineWidth: width * 2.6)
        layer.stroke(oval, with: .color(color), lineWidth: width)
    }

    static func band(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color, _ glimmer: GenieGlimmer) {
        var layer: GraphicsContext = context
        layer.clip(to: Path(roundedRect: glimmer.box, cornerRadius: 8))
        layer.translateBy(x: at.x, y: at.y)
        layer.rotate(by: .degrees(glimmer.angle))
        let height: CGFloat = glimmer.box.height * 2 + 40
        layer.fill(
            Path(CGRect(x: -r / 2, y: -height / 2, width: r, height: height)),
            with: .linearGradient(
                Gradient(colors: [color.opacity(0), color.opacity(0.55), Color.white.opacity(0.7), color.opacity(0.55), color.opacity(0)]),
                startPoint: CGPoint(x: -r / 2, y: 0), endPoint: CGPoint(x: r / 2, y: 0)))
    }

    static func beam(_ context: inout GraphicsContext, _ from: CGPoint, _ to: CGPoint, _ width: CGFloat, _ color: Color) {
        var path: Path = Path()
        path.move(to: from)
        path.addLine(to: to)
        context.stroke(path, with: .color(color.opacity(0.25)), style: StrokeStyle(lineWidth: width * 4, lineCap: .round))
        context.stroke(path, with: .color(color), style: StrokeStyle(lineWidth: width, lineCap: .round))
        context.stroke(path, with: .color(Color.white.opacity(0.8)), style: StrokeStyle(lineWidth: width * 0.35, lineCap: .round))
    }

    static func curve(_ context: inout GraphicsContext, _ glimmer: GenieGlimmer, _ color: Color) {
        let a: CGPoint = CGPoint(x: glimmer.x, y: glimmer.y)
        let b: CGPoint = CGPoint(x: glimmer.x2, y: glimmer.y2)
        var path: Path = Path()
        path.move(to: a)
        if glimmer.shape == .arc {
            let dx: CGFloat = b.x - a.x
            let dy: CGFloat = b.y - a.y
            let length: CGFloat = max(hypot(dx, dy), 0.001)
            let bend: CGFloat = CGFloat(glimmer.bend)
            let control: CGPoint = CGPoint(x: (a.x + b.x) / 2 - dy / length * bend, y: (a.y + b.y) / 2 + dx / length * bend)
            path.addQuadCurve(to: b, control: control)
        } else {
            path.addLine(to: b)
        }
        let shown: Path = glimmer.reveal < 1 ? path.trimmedPath(from: 0, to: CGFloat(max(glimmer.reveal, 0))) : path
        let width: CGFloat = CGFloat(glimmer.weight)
        context.stroke(shown, with: .color(color.opacity(0.3)), style: StrokeStyle(lineWidth: width * 2.6, lineCap: .round))
        context.stroke(shown, with: .color(color), style: StrokeStyle(lineWidth: width, lineCap: .round))
    }

    static func lamp(_ context: inout GraphicsContext, _ at: CGPoint, _ s: CGFloat, _ glimmer: GenieGlimmer) {
        var layer: GraphicsContext = context
        layer.translateBy(x: at.x, y: at.y)
        layer.rotate(by: .degrees(glimmer.angle))
        layer.scaleBy(x: glimmer.squash < 0 ? -1 : 1, y: 1)
        let metal: GraphicsContext.Shading = .linearGradient(
            Gradient(colors: [Color.white, GenieInk.gold, GenieMagicPainter.darkGold]), startPoint: CGPoint(x: -s * 0.3, y: -s * 0.6),
            endPoint: CGPoint(x: s * 0.3, y: s * 0.4))
        let line: GraphicsContext.Shading = .color(GenieMagicPainter.darkGold)
        soft(&layer, CGPoint(x: 0, y: 0), s * 1.6, GenieInk.gold.opacity(0.35))
        var handle: Path = Path()
        handle.addArc(center: CGPoint(x: -s * 0.82, y: -s * 0.05), radius: s * 0.22, startAngle: .degrees(60), endAngle: .degrees(300), clockwise: false)
        layer.stroke(handle, with: line, style: StrokeStyle(lineWidth: s * 0.13, lineCap: .round))
        layer.stroke(handle, with: .color(GenieInk.gold), style: StrokeStyle(lineWidth: s * 0.07, lineCap: .round))
        var spout: Path = Path()
        spout.move(to: CGPoint(x: s * 0.5, y: -s * 0.08))
        spout.addQuadCurve(to: CGPoint(x: s * 1.08, y: -s * 0.34), control: CGPoint(x: s * 0.86, y: s * 0.06))
        spout.addLine(to: CGPoint(x: s * 1.02, y: -s * 0.4))
        spout.addQuadCurve(to: CGPoint(x: s * 0.45, y: -s * 0.26), control: CGPoint(x: s * 0.74, y: -s * 0.18))
        spout.closeSubpath()
        layer.fill(spout, with: metal)
        layer.stroke(spout, with: line, lineWidth: s * 0.04)
        let foot: Path = Path(ellipseIn: CGRect(x: -s * 0.32, y: s * 0.26, width: s * 0.64, height: s * 0.18))
        layer.fill(foot, with: metal)
        layer.stroke(foot, with: line, lineWidth: s * 0.04)
        let lid: Path = Path(ellipseIn: CGRect(x: -s * 0.34, y: -s * 0.52, width: s * 0.68, height: s * 0.44))
        layer.fill(lid, with: metal)
        layer.stroke(lid, with: line, lineWidth: s * 0.04)
        let belly: Path = Path(ellipseIn: CGRect(x: -s * 0.75, y: -s * 0.3, width: s * 1.5, height: s * 0.62))
        layer.fill(belly, with: metal)
        layer.stroke(belly, with: line, lineWidth: s * 0.05)
        layer.fill(circle(CGPoint(x: 0, y: -s * 0.58), s * 0.09), with: metal)
        layer.fill(Path(ellipseIn: CGRect(x: -s * 0.45, y: -s * 0.22, width: s * 0.36, height: s * 0.12)), with: .color(Color.white.opacity(0.6)))
    }

    static func carpet(_ context: inout GraphicsContext, _ at: CGPoint, _ width: CGFloat, _ glimmer: GenieGlimmer) {
        var layer: GraphicsContext = context
        layer.translateBy(x: at.x, y: at.y)
        layer.rotate(by: .degrees(glimmer.angle))
        let height: CGFloat = width * 0.2
        let phase: Double = glimmer.bend
        let steps: Int = 16
        func edge(_ t: Double, _ y: CGFloat) -> CGPoint {
            let x: CGFloat = -width / 2 + width * CGFloat(t)
            return CGPoint(x: x, y: y + CGFloat(sin(t * Double.pi * 3 + phase)) * height * 0.28)
        }
        var cloth: Path = Path()
        cloth.move(to: edge(0, -height / 2))
        for k in 1...steps { cloth.addLine(to: edge(Double(k) / Double(steps), -height / 2)) }
        for k in stride(from: steps, through: 0, by: -1) { cloth.addLine(to: edge(Double(k) / Double(steps), height / 2)) }
        cloth.closeSubpath()
        layer.fill(
            cloth,
            with: .linearGradient(
                Gradient(colors: [GenieInk.lilac, GenieInk.violet, GenieInk.plum]), startPoint: CGPoint(x: 0, y: -height / 2),
                endPoint: CGPoint(x: 0, y: height / 2)))
        layer.stroke(cloth, with: .color(GenieInk.gold), lineWidth: 1.2)
        var stripe: Path = Path()
        stripe.move(to: edge(0.06, 0))
        for k in 1...steps { stripe.addLine(to: edge(0.06 + 0.88 * Double(k) / Double(steps), 0)) }
        layer.stroke(stripe, with: .color(GenieInk.gold.opacity(0.85)), style: StrokeStyle(lineWidth: 0.9, dash: [3, 2]))
        for end in [0.0, 1.0] {
            for k in 0..<3 {
                let base: CGPoint = edge(end, -height / 2 + height * CGFloat(k + 1) / 4)
                var tassel: Path = Path()
                tassel.move(to: base)
                tassel.addLine(to: CGPoint(x: base.x + (end == 0 ? -4 : 4), y: base.y + 1))
                layer.stroke(tassel, with: .color(GenieInk.gold), style: StrokeStyle(lineWidth: 1, lineCap: .round))
            }
        }
    }

    static func crystal(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ glimmer: GenieGlimmer) {
        let stand: Path = Path(ellipseIn: CGRect(x: at.x - r * 0.6, y: at.y + r * 0.78, width: r * 1.2, height: r * 0.36))
        context.fill(stand, with: .color(GenieInk.gold))
        context.stroke(stand, with: .color(GenieMagicPainter.darkGold), lineWidth: 0.8)
        soft(&context, at, r * 1.7, GenieInk.lilac.opacity(0.45))
        let ball: Path = circle(at, r)
        context.fill(
            ball,
            with: .radialGradient(
                Gradient(colors: [GenieInk.mist.opacity(0.75), GenieInk.lilac.opacity(0.6), GenieInk.violet.opacity(0.8)]),
                center: CGPoint(x: at.x - r * 0.2, y: at.y - r * 0.25), startRadius: 0, endRadius: r * 1.1))
        var mist: GraphicsContext = context
        mist.clip(to: ball)
        mist.translateBy(x: at.x, y: at.y)
        mist.rotate(by: .degrees(glimmer.angle))
        for k in 0..<3 {
            var swirl: Path = Path()
            swirl.addArc(
                center: .zero, radius: r * (0.3 + 0.2 * CGFloat(k)), startAngle: .degrees(Double(k) * 120), endAngle: .degrees(Double(k) * 120 + 200),
                clockwise: false)
            mist.stroke(swirl, with: .color((k == 1 ? GenieInk.glow : GenieInk.mist).opacity(0.6)), style: StrokeStyle(lineWidth: r * 0.14, lineCap: .round))
        }
        context.stroke(ball, with: .color(GenieInk.mist.opacity(0.9)), lineWidth: 1)
        context.fill(Path(ellipseIn: CGRect(x: at.x - r * 0.55, y: at.y - r * 0.6, width: r * 0.4, height: r * 0.26)), with: .color(Color.white.opacity(0.75)))
    }

    static func rune(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color, _ glimmer: GenieGlimmer) {
        var layer: GraphicsContext = context
        layer.translateBy(x: at.x, y: at.y)
        layer.rotate(by: .degrees(glimmer.angle))
        var path: Path = Path()
        switch glimmer.variant % 6 {
        case 0:
            path.move(to: CGPoint(x: 0, y: -r))
            path.addLine(to: CGPoint(x: r * 0.87, y: r * 0.5))
            path.addLine(to: CGPoint(x: -r * 0.87, y: r * 0.5))
            path.closeSubpath()
        case 1:
            path.addEllipse(in: CGRect(x: -r * 0.7, y: -r * 0.7, width: r * 1.4, height: r * 1.4))
            path.addEllipse(in: CGRect(x: -r * 0.15, y: -r * 0.15, width: r * 0.3, height: r * 0.3))
        case 2:
            path.move(to: CGPoint(x: -r, y: -r * 0.4))
            path.addLine(to: CGPoint(x: -r * 0.33, y: r * 0.4))
            path.addLine(to: CGPoint(x: r * 0.33, y: -r * 0.4))
            path.addLine(to: CGPoint(x: r, y: r * 0.4))
        case 3:
            path.move(to: CGPoint(x: 0, y: -r))
            path.addLine(to: CGPoint(x: r * 0.6, y: 0))
            path.addLine(to: CGPoint(x: 0, y: r))
            path.addLine(to: CGPoint(x: -r * 0.6, y: 0))
            path.closeSubpath()
            path.move(to: CGPoint(x: -r * 0.6, y: 0))
            path.addLine(to: CGPoint(x: r * 0.6, y: 0))
        case 4:
            path.addArc(center: .zero, radius: r * 0.8, startAngle: .degrees(-120), endAngle: .degrees(120), clockwise: false)
            path.addEllipse(in: CGRect(x: -r * 0.18, y: -r * 0.18, width: r * 0.36, height: r * 0.36))
        default:
            for k in 0...16 {
                let t: Double = Double(k) / 16
                let a: Double = t * Double.pi * 3.2
                let point: CGPoint = CGPoint(x: CGFloat(cos(a) * t) * r, y: CGFloat(sin(a) * t) * r)
                if k == 0 { path.move(to: point) } else { path.addLine(to: point) }
            }
        }
        let width: CGFloat = CGFloat(glimmer.weight)
        layer.stroke(path, with: .color(color.opacity(0.35)), style: StrokeStyle(lineWidth: width * 3, lineCap: .round, lineJoin: .round))
        layer.stroke(path, with: .color(color), style: StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round))
    }

    static func bubble(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color) {
        let disc: Path = circle(at, r)
        context.fill(disc, with: .radialGradient(Gradient(colors: [color.opacity(0), color.opacity(0.28)]), center: at, startRadius: r * 0.4, endRadius: r))
        context.stroke(disc, with: .color(color.opacity(0.9)), lineWidth: 0.9)
        context.fill(Path(ellipseIn: CGRect(x: at.x - r * 0.55, y: at.y - r * 0.6, width: r * 0.4, height: r * 0.28)), with: .color(Color.white.opacity(0.85)))
    }

    static func coin(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ glimmer: GenieGlimmer) {
        var layer: GraphicsContext = context
        layer.translateBy(x: at.x, y: at.y)
        layer.scaleBy(x: max(CGFloat(abs(glimmer.squash)), 0.08), y: 1)
        let disc: Path = circle(.zero, r)
        layer.fill(
            disc,
            with: .linearGradient(
                Gradient(colors: [Color.white, GenieInk.gold, GenieMagicPainter.darkGold]), startPoint: CGPoint(x: -r, y: -r), endPoint: CGPoint(x: r, y: r)))
        layer.stroke(disc, with: .color(GenieMagicPainter.darkGold), lineWidth: 1)
        layer.stroke(circle(.zero, r * 0.62), with: .color(GenieMagicPainter.darkGold.opacity(0.8)), lineWidth: 0.8)
    }

    static func moon(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color, _ glimmer: GenieGlimmer) {
        soft(&context, at, r * 1.9, color.opacity(0.35))
        var layer: GraphicsContext = context
        layer.translateBy(x: at.x, y: at.y)
        layer.rotate(by: .degrees(glimmer.angle))
        layer.clip(to: circle(CGPoint(x: r * 0.45, y: -r * 0.15), r * 0.85), options: .inverse)
        layer.fill(circle(.zero, r), with: .radialGradient(Gradient(colors: [Color.white, color]), center: CGPoint(x: -r * 0.4, y: 0), startRadius: 0, endRadius: r * 1.2))
    }

    static func bottle(_ context: inout GraphicsContext, _ at: CGPoint, _ h: CGFloat, _ glimmer: GenieGlimmer) {
        var layer: GraphicsContext = context
        layer.translateBy(x: at.x, y: at.y)
        layer.rotate(by: .degrees(glimmer.angle))
        soft(&layer, CGPoint(x: 0, y: h * 0.05), h * 0.7, GenieInk.glow.opacity(0.35))
        var glass: Path = Path()
        glass.addEllipse(in: CGRect(x: -h * 0.3, y: -h * 0.18, width: h * 0.6, height: h * 0.6))
        glass.addRoundedRect(in: CGRect(x: -h * 0.08, y: -h * 0.48, width: h * 0.16, height: h * 0.36), cornerSize: CGSize(width: h * 0.04, height: h * 0.04))
        layer.fill(
            glass,
            with: .linearGradient(
                Gradient(colors: [GenieInk.lilac.opacity(0.85), GenieInk.violet.opacity(0.9), GenieInk.plum.opacity(0.95)]),
                startPoint: CGPoint(x: 0, y: -h * 0.5), endPoint: CGPoint(x: 0, y: h * 0.42)))
        layer.stroke(glass, with: .color(GenieInk.glow.opacity(0.8)), lineWidth: 1)
        layer.fill(Path(CGRect(x: -h * 0.1, y: -h * 0.3, width: h * 0.2, height: h * 0.05)), with: .color(GenieInk.gold))
        layer.fill(Path(ellipseIn: CGRect(x: -h * 0.07, y: -h * 0.56, width: h * 0.14, height: h * 0.1)), with: .color(GenieInk.gold))
        layer.fill(Path(ellipseIn: CGRect(x: -h * 0.2, y: -h * 0.08, width: h * 0.1, height: h * 0.2)), with: .color(Color.white.opacity(0.45)))
    }

    static func outline(_ context: inout GraphicsContext, _ color: Color, _ glimmer: GenieGlimmer) {
        let box: CGRect = glimmer.box
        let corners: [CGPoint] = [
            CGPoint(x: box.minX, y: box.minY), CGPoint(x: box.maxX, y: box.minY), CGPoint(x: box.maxX, y: box.maxY), CGPoint(x: box.minX, y: box.maxY),
        ]
        let phase: Double = glimmer.angle
        let wave: CGFloat = CGFloat(glimmer.bend)
        var path: Path = Path()
        var along: Double = 0
        for side in 0..<4 {
            let a: CGPoint = corners[side]
            let b: CGPoint = corners[(side + 1) % 4]
            let length: CGFloat = max(hypot(b.x - a.x, b.y - a.y), 0.001)
            let nx: CGFloat = (b.y - a.y) / length
            let ny: CGFloat = -(b.x - a.x) / length
            let steps: Int = max(Int(length / 6), 2)
            for k in 0...steps {
                let t: CGFloat = CGFloat(k) / CGFloat(steps)
                let offset: CGFloat = CGFloat(sin(phase + (along + Double(length * t)) * 0.12)) * wave
                let point: CGPoint = CGPoint(x: a.x + (b.x - a.x) * t + nx * offset, y: a.y + (b.y - a.y) * t + ny * offset)
                if side == 0 && k == 0 { path.move(to: point) } else { path.addLine(to: point) }
            }
            along += Double(length)
        }
        path.closeSubpath()
        let width: CGFloat = CGFloat(glimmer.weight)
        context.stroke(path, with: .color(color.opacity(0.3)), style: StrokeStyle(lineWidth: width * 3, lineJoin: .round))
        context.stroke(path, with: .color(color), style: StrokeStyle(lineWidth: width, lineJoin: .round))
    }

    static func wisp(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color, _ glimmer: GenieGlimmer) {
        let sway: CGFloat = CGFloat(sin(glimmer.angle))
        let lean: CGFloat = CGFloat(cos(glimmer.angle))
        var path: Path = Path()
        path.move(to: at)
        path.addQuadCurve(to: CGPoint(x: at.x + sway * r * 0.18, y: at.y - r * 0.5), control: CGPoint(x: at.x + lean * r * 0.22, y: at.y - r * 0.25))
        path.addQuadCurve(to: CGPoint(x: at.x - sway * r * 0.12, y: at.y - r), control: CGPoint(x: at.x - lean * r * 0.22, y: at.y - r * 0.75))
        let width: CGFloat = CGFloat(glimmer.weight)
        context.stroke(path, with: .color(color.opacity(0.3)), style: StrokeStyle(lineWidth: width * 3.2, lineCap: .round))
        context.stroke(path, with: .color(color), style: StrokeStyle(lineWidth: width, lineCap: .round))
    }

    static func glow(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat, _ color: Color, _ glimmer: GenieGlimmer) {
        guard !glimmer.box.isEmpty else {
            soft(&context, at, r, color.opacity(0.8))
            return
        }
        let frame: Path = Path(roundedRect: glimmer.box, cornerRadius: 10)
        context.fill(frame, with: .color(color.opacity(0.16)))
        var halo: GraphicsContext = context
        halo.addFilter(.blur(radius: 5))
        halo.stroke(frame, with: .color(color.opacity(0.8)), lineWidth: 5)
        context.stroke(frame, with: .color(color.opacity(0.9)), lineWidth: 1.2)
    }
}
