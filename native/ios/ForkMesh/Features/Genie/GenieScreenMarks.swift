import FMAssistant
import SwiftUI

struct GenieScreenMarks: View {
    let effect: GenieScreenEffect
    let now: Date
    let place: CGPoint

    var body: some View {
        Canvas { (context: inout GraphicsContext, _: CGSize) in
            let fade: Double = effect.strength(at: now)
            guard fade > 0, let rect = effect.route.rects.first else { return }
            context.opacity = fade
            let u: Double = effect.progress(at: now)
            switch effect.route.mark {
            case .none:
                break
            case .outline:
                outline(&context, rect: rect, u: u)
            case .underline:
                let y: CGFloat = rect.maxY + 2
                context.stroke(
                    line(from: CGPoint(x: rect.minX, y: y), to: CGPoint(x: rect.minX + effect.reach(at: now), y: y)),
                    with: .color(GenieInk.silver.opacity(0.9)), style: StrokeStyle(lineWidth: 1.6, lineCap: .round))
            case .highlight:
                let band: CGRect = CGRect(x: rect.minX - 2, y: rect.minY + rect.height * 0.1, width: effect.reach(at: now) + 4, height: rect.height * 0.8)
                context.fill(Path(roundedRect: band, cornerRadius: 4), with: .color(GenieInk.glow.opacity(0.28)))
            case .rings:
                for ring in effect.rings(at: now) {
                    let grown: CGRect = ring.rect.insetBy(dx: -CGFloat(ring.age) * 10, dy: -CGFloat(ring.age) * 10)
                    context.stroke(
                        Path(roundedRect: grown, cornerRadius: min(grown.height / 2, 12)),
                        with: .color(GenieInk.glow.opacity(0.9 * (1 - ring.age))), lineWidth: 2.5 - 1.5 * CGFloat(ring.age))
                }
            case .tape:
                tape(&context, rect: rect, u: u)
            case .cobweb:
                cobweb(&context, rect: rect, u: u)
            case .lantern:
                let x: CGFloat = min(max(place.x, rect.minX), rect.maxX)
                let radius: CGFloat = max(rect.height * 1.4, 40)
                let light: CGRect = CGRect(x: x - radius, y: rect.midY - radius, width: radius * 2, height: radius * 2)
                context.fill(
                    Path(ellipseIn: light),
                    with: .radialGradient(
                        Gradient(colors: [GenieInk.glow.opacity(0.42), GenieInk.glow.opacity(0)]), center: CGPoint(x: x, y: rect.midY),
                        startRadius: 0, endRadius: radius))
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func line(from a: CGPoint, to b: CGPoint) -> Path {
        Path { (path: inout Path) in
            path.move(to: a)
            path.addLine(to: b)
        }
    }

    private func outline(_ context: inout GraphicsContext, rect: CGRect, u: Double) {
        let trail: [CGPoint] = effect.route.path.map { GenieCrawl.clamp($0, to: rect) }
        guard trail.count > 1 else { return }
        let reach: Double = u * Double(trail.count - 1)
        let whole: Int = min(Int(reach), trail.count - 1)
        var path: Path = Path()
        path.move(to: trail[0])
        if whole >= 1 {
            for index in 1...whole { path.addLine(to: trail[index]) }
        }
        if whole < trail.count - 1 {
            let a: CGPoint = trail[whole]
            let b: CGPoint = trail[whole + 1]
            let f: CGFloat = CGFloat(reach - Double(whole))
            path.addLine(to: CGPoint(x: a.x + (b.x - a.x) * f, y: a.y + (b.y - a.y) * f))
        }
        context.stroke(path, with: .color(GenieInk.silver.opacity(0.85)), style: StrokeStyle(lineWidth: 1.3, lineCap: .round, lineJoin: .round))
    }

    private func tape(_ context: inout GraphicsContext, rect: CGRect, u: Double) {
        let y: CGFloat = rect.minY - 3
        let end: CGFloat = rect.minX + rect.width * CGFloat(u)
        context.stroke(
            line(from: CGPoint(x: rect.minX, y: y), to: CGPoint(x: end, y: y)), with: .color(GenieInk.glow.opacity(0.9)),
            style: StrokeStyle(lineWidth: 1.5, lineCap: .round, dash: [5, 3]))
        var tick: CGFloat = rect.minX
        var count: Int = 0
        while tick <= end {
            let long: CGFloat = count % 5 == 0 ? 5 : 2.5
            context.stroke(line(from: CGPoint(x: tick, y: y), to: CGPoint(x: tick, y: y - long)), with: .color(GenieInk.glow.opacity(0.8)), lineWidth: 1)
            tick += 10
            count += 1
        }
        guard u >= 0.98, !effect.route.label.isEmpty else { return }
        let text: Text = Text(effect.route.label).font(.caption2.weight(.bold)).foregroundColor(Color.white)
        let at: CGPoint = CGPoint(x: rect.midX, y: y - 14)
        let chip: CGRect = CGRect(x: at.x - 26, y: at.y - 9, width: 52, height: 18)
        context.fill(Path(roundedRect: chip, cornerRadius: 9), with: .color(GenieSheet.night.opacity(0.92)))
        context.stroke(Path(roundedRect: chip, cornerRadius: 9), with: .color(GenieInk.glow.opacity(0.6)), lineWidth: 1)
        context.draw(text, at: at)
    }

    private func cobweb(_ context: inout GraphicsContext, rect: CGRect, u: Double) {
        guard let corner = effect.route.corner else { return }
        let inward: CGFloat = corner.x <= rect.midX ? 1 : -1
        let size: CGFloat = min(rect.width, rect.height, 64) * CGFloat(min(u * 1.3, 1))
        guard size > 2 else { return }
        let spokes: [CGPoint] = (0...4).map { (index: Int) -> CGPoint in
            let angle: Double = Double(index) * Double.pi / 8
            return CGPoint(x: inward * CGFloat(cos(angle)), y: CGFloat(sin(angle)))
        }
        var web: Path = Path()
        for spoke in spokes {
            web.move(to: corner)
            web.addLine(to: CGPoint(x: corner.x + spoke.x * size, y: corner.y + spoke.y * size))
        }
        for ring in [0.3, 0.6, 0.92] {
            let r: CGFloat = size * CGFloat(ring)
            for index in 0..<(spokes.count - 1) {
                let a: CGPoint = CGPoint(x: corner.x + spokes[index].x * r, y: corner.y + spokes[index].y * r)
                let b: CGPoint = CGPoint(x: corner.x + spokes[index + 1].x * r, y: corner.y + spokes[index + 1].y * r)
                let sag: CGPoint = CGPoint(x: (a.x + b.x) / 2 - (a.x + b.x - 2 * corner.x) * 0.06, y: (a.y + b.y) / 2 - (a.y + b.y - 2 * corner.y) * 0.06)
                if index == 0 { web.move(to: a) }
                web.addQuadCurve(to: b, control: sag)
            }
        }
        context.stroke(web, with: .color(GenieInk.silver.opacity(0.8)), style: StrokeStyle(lineWidth: 0.9, lineCap: .round, lineJoin: .round))
    }
}
