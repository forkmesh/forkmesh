import FMAccount
import FMAssistant
import FMBase
import SwiftUI
import UIKit

@MainActor
final class AquariumDirector {
    static let shared: AquariumDirector = AquariumDirector()
    nonisolated static let fps: Double = 30

    private var tank: AquariumTank?
    private var last: Date?
    private var roster: [AquariumSwimmer]?
    private var shape: (CGRect, CGFloat, CGFloat)?
    private var marks: [GenieLandmark] = []

    var spiderPlaying: Bool { tank?.spiderPlaying ?? false }

    func see(_ next: [GenieLandmark]) {
        marks = next
        tank?.see(next)
    }

    func pause() {
        last = nil
    }

    func reset() {
        tank = nil
        last = nil
        roster = nil
        shape = nil
    }

    func frame(at now: Date, screen: CGRect, top: CGFloat, floor: CGFloat, swimmers: [AquariumSwimmer], calm: Bool) -> AquariumTank {
        let random: () -> Double = { Double.random(in: 0..<1) }
        var next: AquariumTank
        if let known = tank {
            next = known
            if let shape = shape, shape.0 != screen || shape.1 != top || shape.2 != floor {
                next.resize(screen: screen, top: top, floor: floor)
            }
        } else {
            next = AquariumTank(screen: screen, top: top, floor: floor)
            next.see(marks)
        }
        shape = (screen, top, floor)
        if roster != swimmers {
            next.roster(swimmers, random: random)
            roster = swimmers
        }
        next.spider = AquariumDirector.spider(at: now)
        next.calm = calm
        let seconds: Double = last.map { now.timeIntervalSince($0) } ?? 0
        last = now
        let events: [AquariumEvent] = next.step(seconds, random: random)
        tank = next
        let asks: [AquariumSpiderAsk] = events.compactMap { (event: AquariumEvent) -> AquariumSpiderAsk? in
            if case .spider(let ask) = event { return ask }
            return nil
        }
        if !asks.isEmpty {
            Task { @MainActor in
                for ask in asks { AquariumDirector.apply(ask) }
            }
        }
        return next
    }

    static func spider(at now: Date) -> AquariumSpider? {
        let crawler: GenieCrawler = GenieCrawler.shared
        guard crawler.ready, !crawler.hidden else { return nil }
        let hub: GenieHub = GenieHub.shared
        let free: Bool = !crawler.held && !crawler.tumbling && !hub.holdsStill && !hub.busy && !GenieHandCoach.shared.on
            && !GenieScreenPlayer.shared.acting
        return AquariumSpider(point: crawler.point(at: now), side: crawler.side, free: free)
    }

    static func apply(_ ask: AquariumSpiderAsk) {
        let cues: GenieCues = GenieCues.shared
        switch ask {
        case .wave: cues.play(.wave)
        case .cheer: cues.play(.cheer)
        case .giggle: cues.play(.giggle)
        case .surprise: cues.play(.surprise)
        case .eureka: cues.play(.eureka)
        case .dizzy: cues.play(.dizzy)
        case .nod: cues.play(.nod)
        case .look(let point):
            GenieScreenPlayer.shared.look(at: point)
        case .walk(let point):
            let crawler: GenieCrawler = GenieCrawler.shared
            guard crawler.wanders, crawler.parked == nil, !GenieHub.shared.holdsStill, !GenieHandCoach.shared.on else { return }
            guard !UIAccessibility.isReduceMotionEnabled, !UIAccessibility.isVoiceOverRunning else { return }
            crawler.stroll(to: point)
        }
    }
}

struct AquariumLayer: View {
    var hidden: Bool = false
    var floor: CGFloat? = nil

    @Environment(\.accessibilityReduceMotion) private var reduceMotion
    @Environment(\.scenePhase) private var scenePhase
    @State private var settings: SettingsService? = Services.get(SettingsService.self)
    @State private var counts: ShellCounts = ShellCounts.shared

    private var on: Bool { settings?.aquariumOn ?? false }

    private var running: Bool { on && !hidden && scenePhase == .active }

    var body: some View {
        ZStack {
            if on {
                tank
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
        .onChange(of: on) { (_: Bool, now: Bool) in
            if !now { AquariumDirector.shared.reset() }
        }
    }

    private var tank: some View {
        GeometryReader { (proxy: GeometryProxy) in
            TimelineView(.animation(minimumInterval: 1.0 / AquariumDirector.fps, paused: !running)) { (timeline: TimelineViewDefaultContext) in
                let screen: CGRect = CGRect(origin: .zero, size: proxy.size)
                let insets: UIEdgeInsets = GenieKeepClear.keyWindow()?.safeAreaInsets ?? UIEdgeInsets(top: 47, left: 0, bottom: 34, right: 0)
                let ground: CGFloat = floor ?? proxy.size.height - insets.bottom
                let frame: AquariumTank = AquariumDirector.shared.frame(
                    at: timeline.date, screen: screen, top: insets.top, floor: ground, swimmers: counts.swimmers, calm: reduceMotion)
                Canvas { (context: inout GraphicsContext, _: CGSize) in
                    AquariumPainter(tank: frame).paint(&context)
                }
            }
        }
        .ignoresSafeArea()
        .opacity(hidden ? 0 : 1)
        .task(id: running) {
            guard running else {
                AquariumDirector.shared.pause()
                return
            }
            while !Task.isCancelled {
                AquariumDirector.shared.see(GenieScreenScan.shared.current())
                try? await Task.sleep(for: .seconds(GenieLandmarks.poll))
            }
        }
    }
}

extension AquariumColor {
    var color: Color { Color(red: red, green: green, blue: blue) }
}

struct AquariumPainter {
    let tank: AquariumTank

    static let water: Color = Color(red: 0.2, green: 0.55, blue: 0.85)
    static let ink: Color = Color(red: 0.08, green: 0.1, blue: 0.16)

    func paint(_ context: inout GraphicsContext) {
        context.fill(Path(tank.screen), with: .color(AquariumPainter.water.opacity(0.035)))
        for mark in tank.marks { draw(mark, in: &context) }
        for particle in tank.particles { draw(particle, in: &context) }
        for fish in tank.fish { draw(fish, in: &context) }
        for fish in tank.fish { speech(fish, in: &context) }
    }

    private func fade(_ progress: Double) -> Double {
        progress > 0.8 ? max(0, (1 - progress) / 0.2) : 1
    }

    private func draw(_ mark: AquariumMark, in context: inout GraphicsContext) {
        let tint: Color = mark.tint.color
        let alpha: Double = fade(mark.progress)
        let rect: CGRect = mark.rect
        switch mark.kind {
        case .ring:
            let ring: Path = Path(roundedRect: rect.insetBy(dx: -4, dy: -4), cornerRadius: 8)
            context.stroke(ring, with: .color(tint.opacity(0.8 * (1 - mark.progress))), lineWidth: 2)
        case .underline:
            let share: CGFloat = CGFloat(min(mark.progress * 2, 1))
            var line: Path = Path()
            line.move(to: CGPoint(x: rect.minX, y: rect.maxY + 3))
            line.addLine(to: CGPoint(x: rect.minX + rect.width * share, y: rect.maxY + 3))
            context.stroke(line, with: .color(tint.opacity(0.75 * alpha)), style: StrokeStyle(lineWidth: 2.5, lineCap: .round, dash: [0.5, 5]))
        case .highlight:
            let share: CGFloat = CGFloat(min(mark.progress * 2.5, 1))
            let band: CGRect = CGRect(x: rect.minX - 2, y: rect.minY, width: (rect.width + 4) * share, height: rect.height)
            context.fill(Path(roundedRect: band, cornerRadius: 4), with: .color(tint.opacity(0.18 * alpha)))
        case .trace:
            let outline: Path = Path(roundedRect: rect.insetBy(dx: -5, dy: -5), cornerRadius: 9).trimmedPath(from: 0, to: min(mark.progress * 1.4, 1))
            context.stroke(outline, with: .color(tint.opacity(0.7 * alpha)), style: StrokeStyle(lineWidth: 2.5, lineCap: .round, dash: [0.5, 6]))
        case .tape(let label):
            let y: CGFloat = rect.minY - 6
            let share: CGFloat = CGFloat(min(mark.progress * 2, 1))
            var tape: Path = Path()
            tape.move(to: CGPoint(x: rect.minX, y: y))
            tape.addLine(to: CGPoint(x: rect.minX + rect.width * share, y: y))
            tape.move(to: CGPoint(x: rect.minX, y: y - 4))
            tape.addLine(to: CGPoint(x: rect.minX, y: y + 4))
            if share >= 1 {
                tape.move(to: CGPoint(x: rect.maxX, y: y - 4))
                tape.addLine(to: CGPoint(x: rect.maxX, y: y + 4))
            }
            context.stroke(tape, with: .color(tint.opacity(0.85 * alpha)), style: StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
            if share >= 1 {
                let text: Text = Text(label).font(.system(size: 9, weight: .bold)).foregroundColor(tint.opacity(alpha))
                context.draw(text, at: CGPoint(x: rect.midX, y: y - 8))
            }
        }
    }

    private func draw(_ particle: AquariumParticle, in context: inout GraphicsContext) {
        let p: CGPoint = particle.point
        let size: CGFloat = CGFloat(particle.size)
        let alpha: Double = 1 - particle.progress * 0.7
        switch particle.bit {
        case .bubble, .splash:
            let circle: Path = Path(ellipseIn: CGRect(x: p.x - size, y: p.y - size, width: size * 2, height: size * 2))
            context.fill(circle, with: .color(AquariumPainter.water.opacity(0.12 * alpha)))
            context.stroke(circle, with: .color(Color.white.opacity(0.75 * alpha)), lineWidth: 0.8)
        case .sparkle:
            var star: Path = Path()
            star.move(to: CGPoint(x: p.x, y: p.y - size))
            star.addQuadCurve(to: CGPoint(x: p.x + size, y: p.y), control: p)
            star.addQuadCurve(to: CGPoint(x: p.x, y: p.y + size), control: p)
            star.addQuadCurve(to: CGPoint(x: p.x - size, y: p.y), control: p)
            star.addQuadCurve(to: CGPoint(x: p.x, y: p.y - size), control: p)
            context.fill(star, with: .color(AquariumColor.gold.color.opacity(alpha)))
        case .heart:
            glyph("♥", at: p, size: size, color: Color(red: 1, green: 0.35, blue: 0.6), alpha: alpha, in: &context)
        case .star:
            glyph("★", at: p, size: size, color: AquariumColor.gold.color, alpha: alpha, in: &context)
        case .note:
            glyph("♪", at: p, size: size, color: Color(red: 0.45, green: 0.3, blue: 0.85), alpha: alpha, in: &context)
        case .zzz:
            glyph("z", at: p, size: size, color: Color(red: 0.4, green: 0.55, blue: 0.85), alpha: alpha, in: &context)
        case .flash:
            let radius: CGFloat = size * 0.6
            context.fill(
                Path(ellipseIn: CGRect(x: p.x - radius, y: p.y - radius, width: radius * 2, height: radius * 2)),
                with: .color(Color.white.opacity(0.5 * (1 - particle.progress))))
        case .photo:
            let card: CGRect = CGRect(x: p.x - size / 2, y: p.y - size * 0.6, width: size, height: size * 1.2)
            context.fill(Path(roundedRect: card, cornerRadius: 2), with: .color(Color.white.opacity(0.92 * alpha)))
            context.fill(Path(card.insetBy(dx: 2.5, dy: 2.5).offsetBy(dx: 0, dy: -2).insetBy(dx: 0, dy: 1.5)), with: .color(GenieInk.violet.opacity(0.8 * alpha)))
        case .speed:
            var line: Path = Path()
            line.move(to: p)
            line.addLine(to: CGPoint(x: p.x - particle.velocity.width * 0.3, y: p.y - particle.velocity.height * 0.3))
            context.stroke(line, with: .color(Color.white.opacity(0.6 * alpha)), lineWidth: 1)
        case .glyph(let text):
            let radius: CGFloat = size * 0.8
            let circle: Path = Path(ellipseIn: CGRect(x: p.x - radius, y: p.y - radius, width: radius * 2, height: radius * 2))
            context.fill(circle, with: .color(AquariumPainter.water.opacity(0.14 * alpha)))
            context.stroke(circle, with: .color(Color.white.opacity(0.7 * alpha)), lineWidth: 0.8)
            glyph(text, at: p, size: size * 0.9, color: AquariumPainter.ink, alpha: alpha, in: &context)
        }
    }

    private func glyph(_ text: String, at point: CGPoint, size: CGFloat, color: Color, alpha: Double, in context: inout GraphicsContext) {
        context.draw(Text(text).font(.system(size: size, weight: .bold)).foregroundColor(color.opacity(alpha)), at: point)
    }

    private func draw(_ fish: AquariumFish, in context: inout GraphicsContext) {
        let at: CGPoint = fish.drawn
        let bob: CGFloat = CGFloat(sin(fish.swim * 0.7) * 1.2)
        var body: GraphicsContext = context
        body.opacity = fish.opacity * fish.appear
        body.translateBy(x: at.x, y: at.y + bob)
        if fish.facingLeft { body.scaleBy(x: -1, y: 1) }
        shape(fish, in: &body)
        var label: GraphicsContext = context
        label.opacity = fish.opacity * fish.appear
        let below: CGFloat = at.y + fish.height / 2 + 9
        let name: Text = Text(fish.label).font(.system(size: 9, weight: .semibold)).foregroundColor(Color.white)
        let resolved: GraphicsContext.ResolvedText = label.resolve(name)
        let measured: CGSize = resolved.measure(in: CGSize(width: 140, height: 20))
        let pill: CGRect = CGRect(x: at.x - measured.width / 2 - 4, y: below - measured.height / 2 - 1, width: measured.width + 8, height: measured.height + 2)
        label.fill(Path(roundedRect: pill, cornerRadius: pill.height / 2), with: .color(AquariumPainter.ink.opacity(0.45)))
        label.draw(resolved, at: CGPoint(x: at.x, y: below))
        if fish.waiting {
            let pulse: CGFloat = 1 + CGFloat(sin(tank.time * 4)) * 0.12
            let radius: CGFloat = 7 * pulse
            let center: CGPoint = CGPoint(x: at.x, y: at.y - fish.height / 2 - 10)
            label.fill(
                Path(ellipseIn: CGRect(x: center.x - radius, y: center.y - radius, width: radius * 2, height: radius * 2)),
                with: .color(Color(red: 1, green: 0.6, blue: 0.15).opacity(0.95)))
            label.draw(Text("?").font(.system(size: 10, weight: .heavy)).foregroundColor(Color.white), at: center)
        }
    }

    private func shape(_ fish: AquariumFish, in context: inout GraphicsContext) {
        let look: AquariumLook = fish.look
        let length: CGFloat = fish.length
        let height: CGFloat = fish.height
        let wag: CGFloat = CGFloat(sin(fish.swim * 6)) * height * 0.18
        let bodyColor: Color = look.body.color
        let finColor: Color = look.fin.color
        let outline: Color = AquariumPainter.ink.opacity(0.55)
        let tailSize: CGFloat = look.species == .betta || look.species == .goldfish ? 0.75 : 0.5
        var tail: Path = Path()
        tail.move(to: CGPoint(x: -length * 0.32, y: 0))
        tail.addQuadCurve(to: CGPoint(x: -length * 0.62, y: -height * tailSize + wag), control: CGPoint(x: -length * 0.45, y: -height * 0.1))
        tail.addQuadCurve(to: CGPoint(x: -length * 0.62, y: height * tailSize + wag), control: CGPoint(x: -length * 0.52, y: wag))
        tail.closeSubpath()
        context.fill(tail, with: .color(finColor.opacity(0.9)))
        var dorsal: Path = Path()
        let fin: CGFloat = look.species == .angelfish ? 0.9 : (look.species == .betta ? 0.6 : 0.3)
        dorsal.move(to: CGPoint(x: -length * 0.15, y: -height * 0.4))
        dorsal.addQuadCurve(to: CGPoint(x: length * 0.12, y: -height * 0.42), control: CGPoint(x: -length * 0.12, y: -height * (0.5 + fin)))
        dorsal.closeSubpath()
        context.fill(dorsal, with: .color(finColor.opacity(0.85)))
        if look.species == .angelfish || look.species == .betta {
            var anal: Path = Path()
            anal.move(to: CGPoint(x: -length * 0.15, y: height * 0.4))
            anal.addQuadCurve(to: CGPoint(x: length * 0.1, y: height * 0.42), control: CGPoint(x: -length * 0.12, y: height * (0.5 + fin)))
            anal.closeSubpath()
            context.fill(anal, with: .color(finColor.opacity(0.85)))
        }
        let hull: CGRect = look.species == .puffer
            ? CGRect(x: -height / 2, y: -height / 2, width: height, height: height)
            : CGRect(x: -length * 0.4, y: -height / 2, width: length * 0.8, height: height)
        let skin: Path = Path(ellipseIn: hull)
        context.fill(skin, with: .linearGradient(
            Gradient(colors: [bodyColor.opacity(1), bodyColor.opacity(0.78)]), startPoint: CGPoint(x: 0, y: hull.minY), endPoint: CGPoint(x: 0, y: hull.maxY)))
        var inside: GraphicsContext = context
        inside.clip(to: skin)
        switch look.species {
        case .clownfish:
            for x in [-0.08, 0.16] {
                let band: CGRect = CGRect(x: length * CGFloat(x) - length * 0.05, y: hull.minY, width: length * 0.1, height: height)
                inside.fill(Path(band), with: .color(Color.white.opacity(0.95)))
                inside.stroke(Path(band), with: .color(AquariumPainter.ink.opacity(0.6)), lineWidth: 0.8)
            }
        case .angelfish:
            for x in [-0.2, 0, 0.18] {
                inside.fill(Path(CGRect(x: length * CGFloat(x), y: hull.minY, width: length * 0.05, height: height)), with: .color(look.accent.color.opacity(0.7)))
            }
        case .tetra:
            inside.fill(Path(CGRect(x: hull.minX, y: -height * 0.12, width: hull.width, height: height * 0.18)), with: .color(look.accent.color.opacity(0.95)))
            inside.fill(Path(CGRect(x: hull.minX, y: height * 0.08, width: hull.width * 0.45, height: height * 0.35)), with: .color(look.fin.color.opacity(0.85)))
        case .puffer:
            for index in 0..<6 {
                let angle: Double = Double(index) * 1.05
                let spot: CGPoint = CGPoint(x: CGFloat(cos(angle)) * height * 0.25 - height * 0.05, y: CGFloat(sin(angle)) * height * 0.22 - height * 0.1)
                inside.fill(Path(ellipseIn: CGRect(x: spot.x - 1.6, y: spot.y - 1.6, width: 3.2, height: 3.2)), with: .color(look.accent.color.opacity(0.7)))
            }
        case .goldfish, .betta:
            inside.fill(Path(ellipseIn: CGRect(x: hull.minX + hull.width * 0.15, y: hull.minY + 1, width: hull.width * 0.6, height: height * 0.3)), with: .color(Color.white.opacity(0.2)))
        }
        context.stroke(skin, with: .color(outline), lineWidth: 0.9)
        let eyeRadius: CGFloat = max(2, height * 0.13)
        let eye: CGPoint = CGPoint(x: (look.species == .puffer ? height * 0.22 : length * 0.22), y: -height * 0.1)
        context.fill(Path(ellipseIn: CGRect(x: eye.x - eyeRadius, y: eye.y - eyeRadius, width: eyeRadius * 2, height: eyeRadius * 2)), with: .color(Color.white))
        let pupil: CGFloat = eyeRadius * 0.55
        context.fill(
            Path(ellipseIn: CGRect(x: eye.x - pupil + eyeRadius * 0.2, y: eye.y - pupil, width: pupil * 2, height: pupil * 2)),
            with: .color(AquariumPainter.ink))
    }

    private func speech(_ fish: AquariumFish, in context: inout GraphicsContext) {
        guard let text = fish.speech, let box = fish.speechBox else { return }
        var bubble: GraphicsContext = context
        bubble.opacity = fish.appear
        let shape: Path = Path(roundedRect: box, cornerRadius: 10)
        bubble.fill(shape, with: .color(Color.white.opacity(0.94)))
        bubble.stroke(shape, with: .color(fish.look.body.color.opacity(0.9)), lineWidth: 1.2)
        let words: Text = Text(text).font(.system(size: 11, weight: .semibold)).foregroundColor(AquariumPainter.ink)
        bubble.draw(words, at: CGPoint(x: box.midX, y: box.midY))
    }
}
