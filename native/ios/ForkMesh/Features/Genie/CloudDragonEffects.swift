import FMAssistant
import SwiftUI

extension CloudDragonPainter {
    private func seed(_ n: Int) -> Double { DragonMath.seed(n) }

    private func fract(_ v: Double) -> Double { v - v.rounded(.down) }

    private func arcOf(_ v: Double) -> Double { DragonMath.arc(v) }

    private func hold(_ from: Double = 0.08, _ to: Double = 0.92) -> Double { beat.hold(from, to) }

    func sparkle(_ c: inout GraphicsContext, _ x: Double, _ y: Double, _ r: Double, _ alpha: Double, _ color: Color) {
        guard alpha > 0.001, r > 0 else { return }
        var star: Path = Path()
        for i in 0..<8 {
            let a: Double = Double(i) * Double.pi / 4
            let q: Double = i % 2 == 1 ? r * 0.28 : r
            let p: CGPoint = CGPoint(x: x + cos(a) * q, y: y + sin(a) * q)
            if i == 0 { star.move(to: p) } else { star.addLine(to: p) }
        }
        star.closeSubpath()
        var s: GraphicsContext = c
        s.opacity *= alpha
        s.fill(star, with: .color(color))
    }

    func puff(_ c: inout GraphicsContext, _ x: Double, _ y: Double, _ r: Double, _ alpha: Double, _ tint: Color? = nil) {
        guard alpha > 0.001, r > 0 else { return }
        let inner: Color = tint ?? Color(red: 1, green: 1, blue: 1).opacity(0.95)
        let mid: Color = tint ?? Color(red: 0.965, green: 0.941, blue: 0.91).opacity(0.8)
        var s: GraphicsContext = c
        s.opacity *= alpha
        s.fill(
            Path(ellipseIn: CGRect(x: x - r, y: y - r, width: r * 2, height: r * 2)),
            with: .radialGradient(
                Gradient(stops: [
                    .init(color: inner, location: 0), .init(color: mid, location: 0.72),
                    .init(color: Color(red: 0.941, green: 0.91, blue: 0.863).opacity(0), location: 1),
                ]),
                center: CGPoint(x: x, y: y - r * 0.2), startRadius: 0, endRadius: r))
    }

    func note(_ c: inout GraphicsContext, _ x: Double, _ y: Double, _ r: Double, _ alpha: Double) {
        guard alpha > 0.001 else { return }
        var n: GraphicsContext = c
        n.opacity *= alpha
        n.translateBy(x: x, y: y)
        var head: GraphicsContext = n
        head.rotate(by: .radians(-0.3))
        head.fill(Path(ellipseIn: CGRect(x: -r * 0.55, y: -r * 0.42, width: r * 1.1, height: r * 0.84)), with: .color(DragonInk.leaf))
        var stem: Path = Path()
        stem.move(to: CGPoint(x: r * 0.45, y: 0))
        stem.addLine(to: CGPoint(x: r * 0.45, y: -r * 1.6))
        stem.addQuadCurve(to: CGPoint(x: r * 1.1, y: -r * 0.8), control: CGPoint(x: r * 1.1, y: -r * 1.3))
        n.stroke(stem, with: .color(DragonInk.leaf), style: StrokeStyle(lineWidth: r * 0.28, lineCap: .round))
    }

    func letter(_ c: inout GraphicsContext, _ text: String, _ x: Double, _ y: Double, _ size: Double, _ alpha: Double, _ color: Color) {
        guard alpha > 0.001 else { return }
        var l: GraphicsContext = c
        l.opacity *= alpha
        l.draw(Text(verbatim: text).font(.system(size: size, weight: .heavy, design: .rounded)).foregroundColor(color), at: CGPoint(x: x, y: y))
    }

    func dust(_ c: inout GraphicsContext, _ x: Double, _ y: Double, _ u: Double, _ spread: Double, _ count: Int) {
        for i in 0..<count {
            let side: Double = i % 2 == 1 ? 1 : -1
            let r: Double = (3 + seed(i) * 4) * (0.6 + u)
            puff(&c, x + side * spread * (0.2 + 0.8 * seed(i + 4)) * DragonMath.smooth(u), y - seed(i + 9) * 3 - u * 4, r, 0.75 * (1 - u))
        }
    }

    func ticks(_ c: inout GraphicsContext, _ x: Double, _ y: Double, _ u: Double, _ color: Color) {
        let pop: Double = DragonMath.back(DragonMath.span(u, 0, 0.5))
        let fade: Double = 1 - DragonMath.span(u, 0.72, 1)
        var t: GraphicsContext = c
        t.opacity *= fade
        for i in 0..<3 {
            let angle: Double = -1.25 + Double(i) * 0.5
            let r: Double = 8 + pop * 7
            let length: Double = 9 * pop
            var lens: GraphicsContext = t
            lens.translateBy(x: x + cos(angle) * r, y: y + sin(angle) * r)
            lens.rotate(by: .radians(angle))
            var shape: Path = Path()
            shape.move(to: CGPoint(x: -length / 2, y: 0))
            shape.addQuadCurve(to: CGPoint(x: length / 2, y: 0), control: CGPoint(x: 0, y: -1.9))
            shape.addQuadCurve(to: CGPoint(x: -length / 2, y: 0), control: CGPoint(x: 0, y: 1.9))
            lens.fill(shape, with: .color(color))
        }
    }

    func headHook(_ c: inout GraphicsContext) {
        guard let act = frame.activity else { return }
        let age: Double = beat.age
        let u: Double = beat.u
        switch act {
        case .breatheFire:
            let blow: Double = DragonMath.smooth(DragonMath.span(u, 0.24, 0.32)) * (1 - DragonMath.smooth(DragonMath.span(u, 0.84, 0.96)))
            guard blow > 0 else { return }
            flame(&c, blow * (1 + DragonMath.wave(age, 31) * 0.05), 1 + DragonMath.wave(age, 23) * 0.1)
        case .magnify:
            let on: Double = hold()
            if on > 0 { prop(&c, "magnifier", 1190, 700, on * 1.04) }
        case .headphones:
            let on: Double = hold()
            guard on > 0 else { return }
            var band: GraphicsContext = c
            band.opacity *= on
            let curve: Path = arc(860, 470, 300, 300, -2.5, -0.2)
            band.stroke(curve, with: .color(Color(red: 0.173, green: 0.192, blue: 0.22)), style: StrokeStyle(lineWidth: 34, lineCap: .round))
            band.stroke(curve, with: .color(Color(red: 0.318, green: 0.345, blue: 0.373)), style: StrokeStyle(lineWidth: 7, lineCap: .round))
            prop(&c, "cup", 615, 460, on * 1.05)
        case .binoculars:
            let on: Double = hold()
            if on > 0 { prop(&c, "binoculars", 1150, 500, on) }
        default:
            break
        }
    }

    func underHook(_ c: inout GraphicsContext, _ act: DragonActivity) {
        let age: Double = beat.age
        switch act {
        case .codeFast:
            let on: Double = hold()
            for i in 0..<7 {
                let life: Double = fract(age * 2.3 + seed(i))
                let y: Double = -66 + Double(i) * 9
                var line: Path = Path()
                line.move(to: CGPoint(x: -46 - life * 26, y: y))
                line.addLine(to: CGPoint(x: -46 - life * 26 - 12 - seed(i + 5) * 12, y: y))
                c.stroke(
                    line, with: .color(Color(red: 0.055, green: 0.745, blue: 0.376).opacity(0.8 * (1 - life) * on)),
                    style: StrokeStyle(lineWidth: 1.3, lineCap: .round))
            }
        case .travel:
            let u: Double = fract(age * 1.43)
            puff(&c, -30 - u * 14, -3 - u * 3, 3 + u * 4, 0.7 * (1 - u) * hold())
        case .sleep:
            propAt(&c, "bed", 0, -19 + DragonMath.wave(age, 1.2) * 1.2, 0.86 * DragonMath.back(DragonMath.span(age, 0, 0.5)))
        default:
            break
        }
    }

    func overHook(_ c: inout GraphicsContext, _ act: DragonActivity) {
        let age: Double = beat.age
        let u: Double = beat.u
        func smooth(_ v: Double) -> Double { DragonMath.smooth(v) }
        func span(_ a: Double, _ b: Double) -> Double { DragonMath.span(u, a, b) }
        switch act {
        case .breatheFire:
            let blow: Double = smooth(span(0.3, 0.4)) * (1 - smooth(span(0.8, 0.92)))
            for i in 0..<9 {
                let life: Double = fract(age * 1.5 + seed(i))
                sparkle(
                    &c, 58 + life * 36, -48 + (seed(i + 3) - 0.5) * 22 - life * 12, 1.8 * (1 - life), blow * (1 - life),
                    i % 2 == 1 ? DragonInk.gold : DragonInk.ember)
            }
        case .land:
            if age > 0.9 { dust(&c, 0, -2, DragonMath.clamp01((age - 0.9) / 0.9), 48, 14) }
        case .terminal:
            let on: Double = hold()
            for i in 0..<5 {
                let life: Double = fract(age * 0.55 + Double(i) / 5)
                letter(
                    &c, i % 2 == 1 ? "</>" : "{ }", 42 + seed(i) * 14 + life * 5, -40 - life * 34, 6.4, arcOf(life) * 0.95 * on, DragonInk.leaf)
            }
        case .coffee:
            let on: Double = hold()
            let p: CGPoint = local(DragonPoint(frame.pawN.x + 88, frame.pawN.y - 130))
            for i in 0..<3 {
                let life: Double = fract(age * 0.42 + Double(i) / 3)
                var steam: Path = Path()
                for k in 0...8 {
                    let v: Double = Double(k) / 8
                    let point: CGPoint = CGPoint(x: p.x + Double(i - 1) * 3 + sin(v * 6 + age * 3 + Double(i)) * 2.4, y: p.y - (life * 14 + v * 12))
                    if k == 0 { steam.move(to: point) } else { steam.addLine(to: point) }
                }
                c.stroke(
                    steam, with: .color(DragonInk.steam.opacity(0.75 * arcOf(life) * on)), style: StrokeStyle(lineWidth: 1.4, lineCap: .round))
            }
        case .celebrate:
            let colors: [Color] = [DragonInk.mint, DragonInk.gold, DragonInk.pink, Color(red: 0.416, green: 0.643, blue: 1), Color(red: 0.71, green: 0.549, blue: 1)]
            for i in 0..<34 {
                let local: Double = age - seed(i) * 0.7
                if local < 0 { continue }
                let side: Double = seed(i + 40) - 0.5
                var piece: GraphicsContext = c
                piece.opacity *= 1 - DragonMath.span(local, 1.8, 2.3)
                piece.translateBy(
                    x: side * (72 + local * 14) + sin(local * 5 + Double(i)) * 3,
                    y: -104 - sin(DragonMath.clamp01(local / 0.5) * 1.57) * 26 + local * local * 22)
                piece.rotate(by: .radians(local * (3 + seed(i + 7) * 6)))
                piece.scaleBy(x: 1, y: cos(local * 7 + Double(i)))
                piece.fill(Path(roundedRect: CGRect(x: -1.7, y: -0.9, width: 3.4, height: 1.8), cornerRadius: 0.6), with: .color(colors[i % 5]))
            }
        case .headphones:
            for i in 0..<4 {
                let life: Double = fract(age * 0.5 + Double(i) / 4)
                note(&c, (i % 2 == 1 ? 42 : -36) + sin(life * 7 + Double(i)) * 3, -66 - life * 32, 3.2, arcOf(life) * hold())
            }
        case .upload:
            let life: Double = fract(age * 0.8)
            propAt(&c, "arrow", 14, -15 - life * 12, 0.72, alpha: arcOf(life) * hold())
        case .protect:
            let band: Double = (age.truncatingRemainder(dividingBy: 1.6)) / 1.6
            sparkle(&c, 6 + band * 28, -40 + band * 8, 3.4, arcOf(band) * hold(), DragonInk.white)
        case .build:
            let cycle: Double = age * 6 / DragonMath.tau
            let v0: Double = cycle - cycle.rounded(.down)
            if v0 > 0.5 {
                let v: Double = (v0 - 0.5) * 2
                sparkle(&c, 42 + seed(Int(cycle.rounded(.down))) * 18, -46 - v * 5, 2.6 * (1 - v), (1 - v) * hold(), DragonInk.mint)
            }
        case .share:
            share(&c)
        case .binoculars:
            let pulse: Double = arcOf((age.truncatingRemainder(dividingBy: 1.7)) / 1.7)
            sparkle(&c, 62, -50, 3.6 * pulse, pulse * hold(), DragonInk.white)
        case .sleep:
            for i in 0..<3 {
                let life: Double = fract(age * 0.33 + Double(i) / 3)
                propAt(
                    &c, "zee", 46 + life * 18 + sin(life * 6) * 2, -44 - life * 32, 0.4 + life * 0.6,
                    alpha: arcOf(life) * DragonMath.span(age, 0.6, 1))
            }
        case .stretch:
            ticks(&c, 42, -88, span(0.2, 0.9), DragonInk.gold)
        case .star:
            for i in 0..<5 {
                let life: Double = fract(age * 0.7 + Double(i) / 5)
                propAt(
                    &c, "sparkle", 30 + cos(Double(i) * 2.4) * 34, -46 + sin(Double(i) * 2.4) * 30, 0.5 * arcOf(life) + 0.1,
                    alpha: arcOf(life) * hold())
            }
        case .globe:
            for i in 0..<4 {
                let a: Double = age * 2.3 + Double(i) * DragonMath.tau / 4
                let front: Double = sin(a)
                puff(
                    &c, cos(a) * 58, -34 + front * 11, 1.8 + front * 0.6, (0.55 + front * 0.45) * hold(0.16, 0.86),
                    Color(red: 0.667, green: 1, blue: 0.922).opacity(0.95))
            }
        case .heart:
            for i in 0..<4 {
                let life: Double = fract(age * 0.55 + Double(i) / 4)
                propAt(
                    &c, "loveheart", 34 + Double(i) * 8 + sin(life * 6 + Double(i)) * 3, -48 - life * 40, 0.3 + Double(i) * 0.1,
                    alpha: arcOf(life) * hold(), rot: sin(life * 6 + Double(i)) * 0.2)
            }
        default:
            break
        }
    }

    private func share(_ c: inout GraphicsContext) {
        let u: Double = beat.u
        let v: Double = DragonMath.span(u, 0.24, 0.94)
        if v <= 0 {
            if u > 0.04 { propAt(&c, "plane", 24 - 16 * DragonMath.bump(u, 0, 0.24), -74, 0.42, rot: -0.1) }
            return
        }
        func path(_ k: Double) -> CGPoint {
            CGPoint(x: 34 + k * 104 - sin(k * 7) * 11, y: -52 - k * 58 + cos(k * 7) * 11 - 11)
        }
        var s: GraphicsContext = c
        s.opacity *= 1 - DragonMath.span(u, 0.82, 1)
        var trail: Path = Path()
        for k in 0...34 {
            let p: CGPoint = path(Double(k) / 34 * v)
            if k == 0 { trail.move(to: p) } else { trail.addLine(to: p) }
        }
        s.stroke(
            trail, with: .color(DragonInk.flight.opacity(0.85)), style: StrokeStyle(lineWidth: 1, lineCap: .round, dash: [2.2, 3]))
        let p: CGPoint = path(v)
        let q: CGPoint = path(min(v + 0.01, 1.01))
        propAt(&s, "plane", p.x, p.y, 0.6, rot: atan2(q.y - p.y, q.x - p.x) + 0.42)
    }
}
