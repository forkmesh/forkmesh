import Observation
import SwiftUI
import UIKit

@MainActor
@Observable
final class ShellLaunchFlight {
    static let shared = ShellLaunchFlight()

    struct Ghost {
        let image: UIImage
        let frame: CGRect
    }

    struct Flight: Identifiable {
        let id: UUID
        let from: CGPoint
        let to: CGPoint
        let seat: Bool
        let fromArrow: Bool
        let ghost: Ghost?
        let duration: Double
        let start: Date
        let seed: Double

        func landed(_ now: Date) -> Bool {
            now.timeIntervalSince(start) >= duration
        }
    }

    struct Spawn: Identifiable {
        let id: UUID
        let known: Set<Int>
    }

    static let duration: Double = 1.2
    static let ghostDuration: Double = 0.95
    static let shrink: Double = 0.32
    static let burst: Double = 0.45
    static let hold: Duration = .seconds(12)
    static let spawnHold: Duration = .seconds(45)

    @ObservationIgnored var source: CGRect = .zero
    @ObservationIgnored var card: CGRect = .zero
    @ObservationIgnored var row: CGRect = .zero
    @ObservationIgnored private(set) var seated: [Int] = []
    @ObservationIgnored private var ghost: Ghost?
    var target: CGRect = .zero
    private(set) var flights: [Flight] = []
    private(set) var spawns: [Spawn] = []
    private(set) var charging: Date?
    private(set) var floor: Int = 0
    private(set) var landings: Int = 0
    @ObservationIgnored private var reserved: Int = 0

    func badge(running: Int) -> Int {
        return max(running, floor)
    }

    var origin: CGPoint {
        CGPoint(x: source.midX, y: source.midY)
    }

    private var seat: CGPoint? {
        if row == .zero {
            return nil
        }
        let now: Date = Date()
        let flying: Int = flights.filter { $0.seat && !$0.landed(now) }.count
        return AgentDotsBar.seat(min(seated.count, AgentDotsBar.most) + spawns.count + flying, in: row)
    }

    func charge(reduceMotion: Bool) {
        if reduceMotion || (target == .zero && row == .zero) {
            return
        }
        if let image = PromptScreenCapture.region(card) {
            ghost = Ghost(image: image, frame: card)
        }
        if source != .zero {
            charging = Date()
        }
    }

    func discharge() {
        charging = nil
        ghost = nil
    }

    func launch(running: Int, reduceMotion: Bool) {
        reserved = max(reserved, badge(running: running)) + 1
        let count: Int = reserved
        let ghost: Ghost? = self.ghost
        let seat: CGPoint? = self.seat
        let known: Set<Int>? = seat == nil ? nil : Set(seated)
        charging = nil
        self.ghost = nil
        if reduceMotion || (target == .zero && seat == nil) {
            land(count, spawn: known)
            return
        }
        let to: CGPoint = seat ?? CGPoint(x: target.midX, y: target.midY)
        let flight: Flight = Flight(
            id: UUID(), from: start(toward: to, seat: seat != nil), to: to, seat: seat != nil,
            fromArrow: source != .zero, ghost: ghost,
            duration: ghost == nil ? ShellLaunchFlight.duration : ShellLaunchFlight.ghostDuration, start: Date(),
            seed: Double.random(in: 0..<(2 * Double.pi)))
        flights.append(flight)
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .seconds(flight.duration))
            self?.land(count, spawn: known)
            try? await Task.sleep(for: .seconds(ShellLaunchFlight.burst))
            self?.flights.removeAll { $0.id == flight.id }
        }
    }

    func claim(_ ids: [Int]) {
        seated = ids
        if spawns.isEmpty {
            return
        }
        var taken: Set<Int> = []
        let kept: [Spawn] = spawns.filter { (spawn: Spawn) -> Bool in
            guard let id = ids.first(where: { !spawn.known.contains($0) && !taken.contains($0) }) else { return true }
            taken.insert(id)
            return false
        }
        if kept.count != spawns.count {
            withAnimation(.easeOut(duration: 0.25)) {
                spawns = kept
            }
        }
    }

    private func start(toward to: CGPoint, seat: Bool) -> CGPoint {
        if source != .zero {
            return origin
        }
        return seat ? CGPoint(x: to.x + 160, y: to.y + 360) : CGPoint(x: to.x, y: to.y - 320)
    }

    private func land(_ count: Int, spawn known: Set<Int>?) {
        FmHaptics.selection()
        let spawn: Spawn? = known.map { Spawn(id: UUID(), known: $0) }
        withAnimation(.spring(duration: 0.4, bounce: 0.55)) {
            floor = max(floor, count)
            landings += 1
            if let spawn = spawn {
                spawns.append(spawn)
            }
        }
        let landing: Int = landings
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: ShellLaunchFlight.hold)
            guard let self = self, self.landings == landing, self.flights.isEmpty else { return }
            self.floor = 0
            self.reserved = 0
        }
        if let spawn = spawn {
            Task { @MainActor [weak self] in
                try? await Task.sleep(for: ShellLaunchFlight.spawnHold)
                guard let self = self, self.spawns.contains(where: { $0.id == spawn.id }) else { return }
                withAnimation(.easeOut(duration: 0.25)) {
                    self.spawns.removeAll { $0.id == spawn.id }
                }
            }
        }
    }
}

struct ShellLaunchSparkles: View {
    @Environment(\.fmPalette) var fm
    let flight: ShellLaunchFlight

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            if !flight.flights.isEmpty || flight.charging != nil {
                let offset: CGPoint = proxy.frame(in: .global).origin
                TimelineView(.animation) { (timeline: TimelineViewDefaultContext) in
                    Canvas { (context: inout GraphicsContext, _: CGSize) in
                        if let since = flight.charging {
                            charge(since: since, at: timeline.date, offset: offset, in: &context)
                        }
                        for item in flight.flights {
                            draw(item, at: timeline.date, offset: offset, in: &context)
                        }
                    }
                }
            }
        }
        .ignoresSafeArea()
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private var palette: [Color] {
        [fm.accent, Color(red: 1, green: 0.84, blue: 0.3), Color(red: 1, green: 0.45, blue: 0.75), Color.white]
    }

    private static func bend(_ from: CGPoint, _ to: CGPoint, seat: Bool) -> CGPoint {
        if seat {
            return CGPoint(x: from.x + (to.x - from.x) * 0.15, y: from.y + (to.y - from.y) * 0.85)
        }
        return CGPoint(x: from.x + (to.x - from.x) * 0.85, y: from.y)
    }

    private static func curve(_ from: CGPoint, _ bend: CGPoint, _ to: CGPoint, _ u: CGFloat) -> CGPoint {
        CGPoint(
            x: (1 - u) * (1 - u) * from.x + 2 * (1 - u) * u * bend.x + u * u * to.x,
            y: (1 - u) * (1 - u) * from.y + 2 * (1 - u) * u * bend.y + u * u * to.y)
    }

    private func point(_ item: ShellLaunchFlight.Flight, _ t: Double, offset: CGPoint) -> CGPoint {
        let from: CGPoint = item.from
        let to: CGPoint = item.to
        if item.seat {
            let at: CGPoint = ShellLaunchSparkles.curve(from, ShellLaunchSparkles.bend(from, to, seat: true), to, CGFloat(t))
            return CGPoint(x: at.x - offset.x, y: at.y - offset.y)
        }
        let top: CGFloat = min(from.y, to.y)
        let lift: CGFloat = min(360, max(160, (top - 120) / 0.75))
        let lean: CGFloat = abs(to.x - from.x) < 60 ? (to.x < from.x ? 60 : -60) : 0
        let rise: CGPoint = CGPoint(x: from.x + lean, y: top - lift)
        let fall: CGPoint = CGPoint(x: to.x - lean, y: top - lift)
        let u: CGFloat = CGFloat(t)
        let a: CGFloat = (1 - u) * (1 - u) * (1 - u)
        let b: CGFloat = 3 * (1 - u) * (1 - u) * u
        let c: CGFloat = 3 * (1 - u) * u * u
        let d: CGFloat = u * u * u
        return CGPoint(
            x: a * from.x + b * rise.x + c * fall.x + d * to.x - offset.x,
            y: a * from.y + b * rise.y + c * fall.y + d * to.y - offset.y)
    }

    private static func eased(_ t: Double) -> Double {
        return t < 0.5 ? 4 * t * t * t : 1 - pow(-2 * t + 2, 3) / 2
    }

    private func charge(since: Date, at now: Date, offset: CGPoint, in context: inout GraphicsContext) {
        let elapsed: Double = now.timeIntervalSince(since)
        let grow: CGFloat = CGFloat(min(1, elapsed / 0.22))
        let colors: [Color] = palette
        let center: CGPoint = CGPoint(x: flight.origin.x - offset.x, y: flight.origin.y - offset.y)
        let wave: Double = min(1, elapsed / 0.5)
        let ring: CGFloat = CGFloat(18 + 26 * wave)
        context.stroke(
            Path(ellipseIn: CGRect(x: center.x - ring, y: center.y - ring, width: ring * 2, height: ring * 2)),
            with: .color(fm.accent.opacity(0.5 * (1 - wave))), lineWidth: 2)
        for index in 0..<6 {
            let angle: Double = elapsed * 5 + Double(index) * (2 * Double.pi / 6)
            let orbit: CGFloat = 22 * grow
            let at: CGPoint = CGPoint(x: center.x + orbit * CGFloat(cos(angle)), y: center.y + orbit * CGFloat(sin(angle)))
            context.fill(
                ShellLaunchSparkles.star(at: at, radius: 3 * grow, spin: angle),
                with: .color(colors[index % colors.count].opacity(0.85)))
        }
        let pulse: CGFloat = CGFloat(1 + 0.2 * sin(elapsed * 30)) * grow
        context.fill(
            Path(ellipseIn: CGRect(x: center.x - 14 * grow, y: center.y - 14 * grow, width: 28 * grow, height: 28 * grow)),
            with: .color(fm.accent.opacity(0.22)))
        context.fill(ShellLaunchSparkles.star(at: center, radius: 11 * pulse, spin: elapsed * 4), with: .color(colors[1]))
        context.fill(ShellLaunchSparkles.star(at: center, radius: 5 * pulse, spin: elapsed * 4), with: .color(Color.white))
    }

    private func ghostPlace(
        _ item: ShellLaunchFlight.Flight, _ ghost: ShellLaunchFlight.Ghost, _ elapsed: Double, offset: CGPoint
    ) -> (rect: CGRect, radius: CGFloat, travel: Double) {
        let frame: CGRect = ghost.frame
        let width: CGFloat = max(frame.width, 1)
        let small: CGFloat = min(0.3, 120 / width)
        let end: CGFloat = 14 / width
        let from: CGPoint = CGPoint(x: frame.midX, y: frame.midY)
        let to: CGPoint = item.to
        var scale: CGFloat = 1
        var center: CGPoint = from
        var travel: Double = 0
        var size: CGSize = frame.size
        if elapsed < ShellLaunchFlight.shrink {
            let u: CGFloat = CGFloat(elapsed / ShellLaunchFlight.shrink)
            let eased: CGFloat = 1 - (1 - u) * (1 - u) * (1 - u)
            scale = 1 + (small - 1) * eased
            size = CGSize(width: frame.width * scale, height: frame.height * scale)
        } else {
            travel = min(1, (elapsed - ShellLaunchFlight.shrink) / (ShellLaunchFlight.ghostDuration - ShellLaunchFlight.shrink))
            let u: CGFloat = CGFloat(travel * travel)
            center = ShellLaunchSparkles.curve(from, ShellLaunchSparkles.bend(from, to, seat: item.seat), to, u)
            if item.seat {
                let side: CGFloat = AgentDotsBar.dot - 5
                size = CGSize(
                    width: frame.width * small + (side - frame.width * small) * u,
                    height: frame.height * small + (side - frame.height * small) * u)
                scale = size.width / width
            } else {
                scale = small + (end - small) * u
                size = CGSize(width: frame.width * scale, height: frame.height * scale)
            }
        }
        let rect: CGRect = CGRect(
            x: center.x - size.width / 2 - offset.x, y: center.y - size.height / 2 - offset.y,
            width: size.width, height: size.height)
        let pill: CGFloat = CGFloat(travel)
        let radius: CGFloat = min(size.height / 2, 28 * scale + (size.height / 2 - 28 * scale) * pill)
        return (rect, radius, travel)
    }

    private func drawGhost(
        _ item: ShellLaunchFlight.Flight, _ ghost: ShellLaunchFlight.Ghost, elapsed: Double, offset: CGPoint,
        in context: inout GraphicsContext
    ) {
        let colors: [Color] = palette
        let place: (rect: CGRect, radius: CGFloat, travel: Double) = ghostPlace(item, ghost, elapsed, offset: offset)
        if place.travel > 0 {
            for step in stride(from: 12, through: 1, by: -1) {
                let lag: Double = elapsed - Double(step) * 0.03
                guard lag > ShellLaunchFlight.shrink else { continue }
                let p: CGRect = ghostPlace(item, ghost, lag, offset: offset).rect
                let fade: Double = 1 - Double(step) / 13
                let wobble: Double = sin(item.seed + Double(step) * 1.7 + elapsed * 18)
                let at: CGPoint = CGPoint(x: p.midX + CGFloat(wobble * 6 * (1 - fade)), y: p.midY + CGFloat(cos(item.seed + Double(step)) * 5 * (1 - fade)))
                context.fill(
                    ShellLaunchSparkles.star(at: at, radius: CGFloat(2 + 4 * fade), spin: elapsed * 6 + Double(step)),
                    with: .color(colors[step % colors.count].opacity(fade * 0.85)))
            }
        }
        let shape: Path = Path(roundedRect: place.rect, cornerRadius: place.radius, style: .continuous)
        let glow: CGFloat = CGFloat(6 + 10 * place.travel)
        context.fill(
            Path(roundedRect: place.rect.insetBy(dx: -glow / 2, dy: -glow / 2), cornerRadius: place.radius + glow / 2, style: .continuous),
            with: .color(fm.accent.opacity((0.18 + 0.2 * place.travel) * min(1, place.travel * 4))))
        context.drawLayer { (layer: inout GraphicsContext) in
            layer.opacity = 1 - 0.5 * max(0, (place.travel - 0.6) / 0.4)
            layer.clip(to: shape)
            layer.draw(Image(uiImage: ghost.image), in: place.rect)
        }
    }

    private func draw(_ item: ShellLaunchFlight.Flight, at now: Date, offset: CGPoint, in context: inout GraphicsContext) {
        let elapsed: Double = now.timeIntervalSince(item.start)
        let raw: Double = elapsed / item.duration
        let colors: [Color] = palette
        if let ghost = item.ghost {
            if raw < 1 {
                drawGhost(item, ghost, elapsed: elapsed, offset: offset, in: &context)
                return
            }
        } else if item.fromArrow && elapsed < 0.4 {
            let puff: Double = elapsed / 0.4
            let base: CGPoint = CGPoint(x: item.from.x - offset.x, y: item.from.y - offset.y)
            let spread: CGFloat = CGFloat(22 + 30 * puff)
            for index in 0..<6 {
                let angle: Double = item.seed + Double(index) * (2 * Double.pi / 6)
                let at: CGPoint = CGPoint(x: base.x + spread * CGFloat(cos(angle)), y: base.y + spread * CGFloat(sin(angle)))
                context.fill(
                    ShellLaunchSparkles.star(at: at, radius: CGFloat(3 * (1 - puff) + 0.5), spin: angle + puff * 4),
                    with: .color(colors[index % colors.count].opacity(0.85 * (1 - puff))))
            }
            context.stroke(
                Path(ellipseIn: CGRect(x: base.x - spread, y: base.y - spread, width: spread * 2, height: spread * 2)),
                with: .color(fm.accent.opacity(0.4 * (1 - puff))), lineWidth: 2)
        }
        if raw < 1 {
            for step in stride(from: 16, through: 1, by: -1) {
                let lag: Double = raw - Double(step) * 0.035
                guard lag > 0 else { continue }
                let p: CGPoint = point(item, ShellLaunchSparkles.eased(lag), offset: offset)
                let wobble: Double = sin(item.seed + Double(step) * 1.7 + elapsed * 18)
                let fade: Double = 1 - Double(step) / 17
                let jitter: CGPoint = CGPoint(x: p.x + CGFloat(wobble * 6 * (1 - fade)), y: p.y + CGFloat(cos(item.seed + Double(step)) * 5 * (1 - fade)))
                let radius: CGFloat = CGFloat(2 + 5 * fade)
                let spin: Double = elapsed * 6 + Double(step)
                context.fill(
                    ShellLaunchSparkles.star(at: jitter, radius: radius, spin: spin),
                    with: .color(colors[step % colors.count].opacity(fade * 0.9)))
            }
            let head: CGPoint = point(item, ShellLaunchSparkles.eased(raw), offset: offset)
            let pulse: CGFloat = CGFloat(1 + 0.2 * sin(elapsed * 30))
            context.fill(
                Path(ellipseIn: CGRect(x: head.x - 14, y: head.y - 14, width: 28, height: 28)),
                with: .color(fm.accent.opacity(0.22)))
            context.fill(ShellLaunchSparkles.star(at: head, radius: 11 * pulse, spin: elapsed * 4), with: .color(colors[1]))
            context.fill(ShellLaunchSparkles.star(at: head, radius: 5 * pulse, spin: elapsed * 4), with: .color(Color.white))
            return
        }
        let burst: Double = min(1, (elapsed - item.duration) / ShellLaunchFlight.burst)
        let center: CGPoint = CGPoint(x: item.to.x - offset.x, y: item.to.y - offset.y)
        let ring: CGFloat = CGFloat(10 + 34 * burst)
        for index in 0..<10 {
            let angle: Double = item.seed + Double(index) * (2 * Double.pi / 10)
            let at: CGPoint = CGPoint(x: center.x + ring * CGFloat(cos(angle)), y: center.y + ring * CGFloat(sin(angle)))
            context.fill(
                ShellLaunchSparkles.star(at: at, radius: CGFloat(5 * (1 - burst) + 1), spin: angle + burst * 3),
                with: .color(colors[index % colors.count].opacity(1 - burst)))
        }
        context.stroke(
            Path(ellipseIn: CGRect(x: center.x - ring, y: center.y - ring, width: ring * 2, height: ring * 2)),
            with: .color(fm.accent.opacity(0.35 * (1 - burst))), lineWidth: 2)
    }

    static func star(at center: CGPoint, radius: CGFloat, spin: Double) -> Path {
        var path: Path = Path()
        let inner: CGFloat = radius * 0.32
        for index in 0..<8 {
            let angle: Double = spin + Double(index) * Double.pi / 4
            let r: CGFloat = index % 2 == 0 ? radius : inner
            let p: CGPoint = CGPoint(x: center.x + r * CGFloat(cos(angle)), y: center.y + r * CGFloat(sin(angle)))
            if index == 0 {
                path.move(to: p)
            } else {
                path.addLine(to: p)
            }
        }
        path.closeSubpath()
        return path
    }
}
