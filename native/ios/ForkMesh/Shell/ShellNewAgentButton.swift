import CoreGraphics
import CoreMotion
import Observation
import SwiftUI
import TipKit

@MainActor
@Observable
final class ShellSparkleTilt {
    static let shared: ShellSparkleTilt = ShellSparkleTilt()
    static let smoothing: Double = 0.12
    static let kick: Double = 0.35

    private(set) var tilt: CGSize = .zero
    private(set) var energy: Double = 0

    @ObservationIgnored private let motion: CMMotionManager = CMMotionManager()
    @ObservationIgnored private var users: Int = 0
    @ObservationIgnored private var resting: CMAcceleration?

    func begin() {
        users += 1
        guard users == 1, motion.isAccelerometerAvailable, !motion.isAccelerometerActive else { return }
        resting = nil
        motion.accelerometerUpdateInterval = 1.0 / 30.0
        motion.startAccelerometerUpdates(to: .main) { [weak self] (data: CMAccelerometerData?, _: Error?) in
            guard let data = data else { return }
            let reading: CMAcceleration = data.acceleration
            MainActor.assumeIsolated { self?.feel(reading) }
        }
    }

    func end() {
        users = max(0, users - 1)
        guard users == 0 else { return }
        if motion.isAccelerometerActive { motion.stopAccelerometerUpdates() }
        resting = nil
        tilt = .zero
        energy = 0
    }

    private func feel(_ reading: CMAcceleration) {
        var rest: CMAcceleration = resting ?? reading
        let k: Double = ShellSparkleTilt.smoothing
        rest.x += (reading.x - rest.x) * k
        rest.y += (reading.y - rest.y) * k
        rest.z += (reading.z - rest.z) * k
        resting = rest
        let jolt: Double = ((reading.x - rest.x) * (reading.x - rest.x)
            + (reading.y - rest.y) * (reading.y - rest.y)
            + (reading.z - rest.z) * (reading.z - rest.z)).squareRoot()
        let next: CGSize = CGSize(width: ShellSparkleTilt.clamp(rest.x), height: ShellSparkleTilt.clamp(-rest.y - 0.6))
        if abs(next.width - tilt.width) + abs(next.height - tilt.height) > 0.01 {
            tilt = next
        }
        let raw: Double = min(1, max(energy * 0.85, jolt * ShellSparkleTilt.kick * 4))
        let charged: Double = raw < 0.05 ? 0 : raw
        if abs(charged - energy) > 0.005 {
            energy = charged
        }
    }

    nonisolated static func clamp(_ value: Double) -> CGFloat {
        CGFloat(min(max(value, -1), 1))
    }
}

struct ShellSparkleField: View {
    let tilt: CGSize
    let energy: Double
    let paused: Bool
    let stars: Int
    var bornAt: Double = 0
    var plain: Bool = false

    static let most: Int = 24
    static let goldenAngle: Double = 2.3999632

    static let seeds: [(radius: CGFloat, angle: Double, depth: CGFloat, size: CGFloat, phase: Double)] = (0..<most).map {
        (index: Int) in
        let n: Double = Double(index)
        let radius: CGFloat = 0.5 + CGFloat((n * 0.6180339 + 0.31).truncatingRemainder(dividingBy: 1)) * 0.36
        let depth: CGFloat = 0.35 + CGFloat((n * 0.7548776).truncatingRemainder(dividingBy: 1)) * 0.65
        let size: CGFloat = 1.5 + CGFloat((n * 0.5698402).truncatingRemainder(dividingBy: 1)) * 1.6
        return (radius, n * goldenAngle - Double.pi / 3, depth, size, n * 1.37)
    }

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: paused)) { (context: TimelineViewDefaultContext) in
            Canvas { (canvas: inout GraphicsContext, size: CGSize) in
                draw(&canvas, size: size, time: context.date.timeIntervalSinceReferenceDate)
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func draw(_ canvas: inout GraphicsContext, size: CGSize, time: Double) {
        let center: CGPoint = CGPoint(x: size.width / 2, y: size.height / 2)
        let half: CGFloat = min(size.width, size.height) / 2
        let shown: Int = min(max(stars, 0), ShellSparkleField.most)
        if !plain {
            aurora(&canvas, size: size, center: center, time: time, lively: min(1, Double(shown) / 6))
            sheen(&canvas, size: size)
        }
        for index in 0..<shown {
            star(&canvas, index: index, newest: index == shown - 1, center: center, half: half, time: time)
        }
        if !plain && !paused && shown > 0 {
            comet(&canvas, size: size, time: time)
        }
        if !plain {
            rim(&canvas, size: size, center: center, time: time)
        }
    }

    private func aurora(_ canvas: inout GraphicsContext, size: CGSize, center: CGPoint, time: Double, lively: Double) {
        let disc: Path = Path(ellipseIn: CGRect(origin: .zero, size: size))
        let ice: Color = Color(red: 0.45, green: 0.95, blue: 1)
        let rose: Color = Color(red: 1, green: 0.55, blue: 0.88)
        var glow: GraphicsContext = canvas
        glow.blendMode = .screen
        glow.opacity = 0.3 + lively * 0.25 + energy * 0.2
        glow.fill(
            disc,
            with: .conicGradient(
                Gradient(colors: [ice.opacity(0), ice, rose.opacity(0), rose, ice.opacity(0)]),
                center: center, angle: .radians(paused ? 0 : time * 0.5)))
        glow.fill(
            disc,
            with: .radialGradient(
                Gradient(colors: [Color.white.opacity(0.4), Color.white.opacity(0)]),
                center: CGPoint(x: center.x - tilt.width * 6, y: size.height * 0.3 - tilt.height * 6),
                startRadius: 0, endRadius: size.width * 0.55))
    }

    private func sheen(_ canvas: inout GraphicsContext, size: CGSize) {
        let sheenX: CGFloat = size.width * (0.5 + tilt.width * 0.6)
        let band: Path = Path(CGRect(x: sheenX - size.height * 0.9, y: -size.height, width: size.height * 0.7, height: size.height * 3))
        var tilted: GraphicsContext = canvas
        tilted.translateBy(x: sheenX, y: size.height / 2)
        tilted.rotate(by: .degrees(Double(22 + tilt.height * 18)))
        tilted.translateBy(x: -sheenX, y: -size.height / 2)
        tilted.fill(
            band,
            with: .linearGradient(
                Gradient(colors: [Color.white.opacity(0), Color.white.opacity(0.22 + energy * 0.2), Color.white.opacity(0)]),
                startPoint: CGPoint(x: sheenX - size.height * 0.9, y: 0),
                endPoint: CGPoint(x: sheenX - size.height * 0.2, y: 0)))
    }

    private func star(_ canvas: inout GraphicsContext, index: Int, newest: Bool, center: CGPoint, half: CGFloat, time: Double) {
        let seed = ShellSparkleField.seeds[index]
        let orbit: Double = seed.angle + (paused ? 0 : time * (0.08 + Double(seed.depth) * 0.12))
        let reach: CGFloat = seed.radius * half
        let point: CGPoint = CGPoint(
            x: center.x + CGFloat(cos(orbit)) * reach + tilt.width * seed.depth * half * 0.3,
            y: center.y + CGFloat(sin(orbit)) * reach + tilt.height * seed.depth * half * 0.2)
        let twinkle: Double = paused
            ? 0.8 : 0.55 + 0.45 * sin(time * (2.2 + seed.phase.truncatingRemainder(dividingBy: 1.6)) + seed.phase)
        let age: Double = time - bornAt
        let bloom: Double = newest && !paused && age >= 0 && age < 1.2 ? 1 - age / 1.2 : 0
        let glow: Double = min(1, twinkle * (0.6 + Double(seed.depth) * 0.4) + energy * 0.5 + bloom)
        let radius: CGFloat = seed.size * (0.75 + CGFloat(glow) * 0.45 + CGFloat(energy) * 0.4 + CGFloat(bloom) * 1.8)
        let halo: CGFloat = radius * 2.4
        canvas.fill(
            Path(ellipseIn: CGRect(x: point.x - halo, y: point.y - halo, width: halo * 2, height: halo * 2)),
            with: .radialGradient(
                Gradient(colors: [Color.white.opacity(glow * 0.45), Color.white.opacity(0)]),
                center: point, startRadius: 0, endRadius: halo))
        canvas.fill(ShellSparkleField.star(at: point, radius: radius), with: .color(Color.white.opacity(max(0.4, glow))))
    }

    private func comet(_ canvas: inout GraphicsContext, size: CGSize, time: Double) {
        let period: Double = 6.5
        let phase: Double = time.truncatingRemainder(dividingBy: period) / period
        guard phase < 0.14 else { return }
        let run: CGFloat = CGFloat(phase / 0.14)
        let head: CGPoint = CGPoint(x: size.width * (-0.15 + run * 1.3), y: size.height * (0.15 + run * 0.55))
        let tail: CGPoint = CGPoint(x: head.x - size.width * 0.35, y: head.y - size.height * 0.15)
        var streak: Path = Path()
        streak.move(to: tail)
        streak.addLine(to: head)
        canvas.stroke(
            streak,
            with: .linearGradient(
                Gradient(colors: [Color.white.opacity(0), Color.white.opacity(0.9)]), startPoint: tail, endPoint: head),
            style: StrokeStyle(lineWidth: 1.4, lineCap: .round))
        canvas.fill(ShellSparkleField.star(at: head, radius: 2.2), with: .color(Color.white))
    }

    private func rim(_ canvas: inout GraphicsContext, size: CGSize, center: CGPoint, time: Double) {
        let ring: Path = Path(ellipseIn: CGRect(origin: .zero, size: size).insetBy(dx: 0.75, dy: 0.75))
        canvas.stroke(
            ring,
            with: .conicGradient(
                Gradient(colors: [
                    Color.white.opacity(0.85), Color.white.opacity(0.08),
                    Color(red: 0.6, green: 0.9, blue: 1).opacity(0.7), Color.white.opacity(0.08),
                    Color(red: 1, green: 0.7, blue: 0.95).opacity(0.6), Color.white.opacity(0.85),
                ]),
                center: center, angle: .radians(paused ? 0 : -time * 0.9)),
            lineWidth: 1.5)
    }

    static func star(at center: CGPoint, radius: CGFloat) -> Path {
        let waist: CGFloat = radius * 0.22
        var path: Path = Path()
        path.move(to: CGPoint(x: center.x, y: center.y - radius))
        path.addQuadCurve(to: CGPoint(x: center.x + radius, y: center.y), control: CGPoint(x: center.x + waist, y: center.y - waist))
        path.addQuadCurve(to: CGPoint(x: center.x, y: center.y + radius), control: CGPoint(x: center.x + waist, y: center.y + waist))
        path.addQuadCurve(to: CGPoint(x: center.x - radius, y: center.y), control: CGPoint(x: center.x - waist, y: center.y + waist))
        path.addQuadCurve(to: CGPoint(x: center.x, y: center.y - radius), control: CGPoint(x: center.x - waist, y: center.y - waist))
        path.closeSubpath()
        return path
    }
}

struct ShellNewAgentButton: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceTransparency) var reduceTransparency
    @Environment(\.accessibilityReduceMotion) var reduceMotion
    @Environment(\.scenePhase) var scenePhase
    @ScaledMetric(relativeTo: .body) private var diameter: CGFloat = 44
    let onNewAgent: () -> Void
    let onPalette: () -> Void
    var tint: Color? = nil
    let paletteTip = PaletteTip()

    @Environment(\.shellTabHidden) private var tabHidden
    @State private var tilt: ShellSparkleTilt = ShellSparkleTilt.shared
    @State private var feeling: Bool = false
    @State private var bornAt: Double = 0
    private var running: Int { ShellCounts.shared.agents }

    var body: some View {
        face
            .onTapGesture {
                FmHaptics.tap()
                onNewAgent()
            }
            .onLongPressGesture(minimumDuration: 0.35, maximumDistance: 10) {
                FmHaptics.palette()
                onPalette()
            }
            .hoverEffect(.lift)
            .accessibilityElement()
            .accessibilityAddTraits(.isButton)
            .accessibilityLabel(Text("New agent"))
            .accessibilityValue(Text(ShellNewAgentButton.runningLabel(running)))
            .accessibilityHint(Text("Opens the composer to start an agent. Each star is an agent running. Touch and hold for the command palette."))
            .accessibilityAction { onNewAgent() }
            .accessibilityAction(named: Text("Command palette")) { onPalette() }
            .accessibilityShowsLargeContentViewer {
                SwiftUI.Label("New agent", systemImage: "plus")
            }
            .accessibilityIdentifier("shell-new-agent-button")
            .popoverTip(paletteTip)
            .genieKeepsClear()
            .onAppear { feel(!tabHidden) }
            .onDisappear { feel(false) }
            .onChange(of: scenePhase) { _, phase in feel(phase == .active && !tabHidden) }
            .onChange(of: tabHidden) { _, hidden in feel(!hidden && scenePhase == .active) }
            .onChange(of: reduceMotion) { _, _ in
                feel(false)
                feel(scenePhase == .active && !tabHidden)
            }
            .onChange(of: running) { (old: Int, next: Int) in
                if next > old {
                    bornAt = Date().timeIntervalSinceReferenceDate
                }
            }
    }

    static func runningLabel(_ count: Int) -> String {
        switch count {
        case 0: return "No agents running"
        case 1: return "1 agent running"
        default: return "\(count) agents running"
        }
    }

    private var accent: Color { tint ?? fm.accent }

    private var still: Bool { reduceMotion || !feeling }

    private func feel(_ on: Bool) {
        let wanted: Bool = on && !reduceMotion
        guard wanted != feeling else { return }
        feeling = wanted
        if wanted { tilt.begin() } else { tilt.end() }
    }

    private var face: some View {
        let energy: Double = still ? 0 : tilt.energy
        let lively: Double = Double(min(running, 8)) / 8
        return ZStack {
            fill
            ShellSparkleField(
                tilt: still ? .zero : tilt.tilt, energy: energy, paused: still, stars: running, bornAt: bornAt,
                plain: reduceTransparency)
            Image(systemName: "plus")
                .font(.system(size: diameter * 0.42, weight: .bold))
                .foregroundStyle(Color.white)
                .shadow(color: Color.black.opacity(0.3), radius: 2, y: 1)
                .symbolEffect(.bounce, value: still ? 0 : Int(energy * 3))
            if reduceTransparency {
                rim
            }
        }
        .frame(width: diameter, height: diameter)
        .clipShape(Circle())
        .shadow(color: accent.opacity(0.35 + lively * 0.25 + energy * 0.3), radius: 8 + lively * 6, y: 3)
        .contentShape(Circle())
    }

    @ViewBuilder private var fill: some View {
        if reduceTransparency {
            Circle().fill(accent)
        } else {
            Circle().fill(
                LinearGradient(
                    colors: [accent.opacity(0.95), accent, accent.opacity(0.7)],
                    startPoint: UnitPoint(x: 0.5 - (still ? 0 : tilt.tilt.width) * 0.5, y: 0),
                    endPoint: UnitPoint(x: 0.5 + (still ? 0 : tilt.tilt.width) * 0.5, y: 1)))
        }
    }

    private var rim: some View {
        Circle()
            .strokeBorder(Color.white.opacity(0.6), lineWidth: 1)
            .allowsHitTesting(false)
    }
}
