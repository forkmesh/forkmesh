import FMAssistant
import SwiftUI

enum GenieMood: Equatable {
    case idle
    case greeting
    case listening
    case thinking
    case speaking

    init(_ phase: GeniePhase) {
        switch phase {
        case .greeting: self = .greeting
        case .listening, .confirming: self = .listening
        case .thinking, .working, .handingOff: self = .thinking
        case .speaking: self = .speaking
        case .idle, .finished: self = .idle
        }
    }
}

struct GenieStride: Equatable {
    var phase: Double = 0
    var walking: Double = 0
    var facing: Double = 0
    var air: Double = 0
    var land: Double = 0

    static let still: GenieStride = GenieStride()
}

enum GenieInk {
    static let glow: Color = Color(red: 0.25, green: 0.95, blue: 0.86)
    static let glowSoft: Color = Color(red: 0.42, green: 0.86, blue: 0.94)
    static let glowDeep: Color = Color(red: 0.05, green: 0.55, blue: 0.56)
    static let lilac: Color = Color(red: 0.56, green: 0.40, blue: 0.82)
    static let violet: Color = Color(red: 0.40, green: 0.24, blue: 0.66)
    static let plum: Color = Color(red: 0.21, green: 0.13, blue: 0.38)
    static let shell: Color = Color(red: 0.10, green: 0.10, blue: 0.20)
    static let shellLight: Color = Color(red: 0.22, green: 0.22, blue: 0.36)
    static let pitch: Color = Color(red: 0.03, green: 0.03, blue: 0.08)
    static let deepSea: Color = Color(red: 0.10, green: 0.30, blue: 0.40)
    static let silver: Color = Color(red: 0.80, green: 0.82, blue: 0.88)
    static let silverDark: Color = Color(red: 0.46, green: 0.48, blue: 0.56)
    static let sickly: Color = Color(red: 0.42, green: 0.62, blue: 0.36)
    static let gold: Color = Color(red: 1.0, green: 0.81, blue: 0.33)
    static let mist: Color = Color(red: 0.93, green: 0.9, blue: 1.0)
}

struct GenieAvatar: View {
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var mood: GenieMood = .idle
    var level: Double = 0
    var animated: Bool = true
    var frameRate: Double = 30
    var cue: GenieCue? = nil
    var ambient: Bool = false
    var lean: CGSize = .zero
    var vitals: GenieVitals? = nil
    var at: Date? = nil
    var stride: GenieStride = .still
    var show: GenieShow? = nil
    var gaze: CGPoint? = nil
    var alarm: Double = 0

    private var tilt: CGSize { reduceMotion ? .zero : lean }

    var body: some View {
        if let at = at {
            painted(at)
        } else if animated {
            TimelineView(.animation(minimumInterval: 1.0 / frameRate, paused: reduceMotion && mood != .speaking)) {
                (timeline: TimelineViewDefaultContext) in
                painted(timeline.date)
            }
            .accessibilityHidden(true)
        } else {
            Canvas { (context: inout GraphicsContext, size: CGSize) in
                GeniePainter(mood: mood, level: level, t: 0, speakingT: 0, lean: tilt, vitality: vitality, condition: condition)
                    .paint(&context, size: size)
            }
            .accessibilityHidden(true)
        }
    }

    private var vitality: Double { vitals?.health ?? 0.7 }

    private var condition: GenieVitalLevel { vitals?.level ?? .happy }

    private func painted(_ date: Date) -> some View {
        Canvas { (context: inout GraphicsContext, size: CGSize) in
            let t: Double = reduceMotion ? 0 : date.timeIntervalSinceReferenceDate
            let showing: GenieTrickFrame? = reduceMotion ? nil : show?.frame(at: date)
            let active: (GenieReaction, Double)? = reduceMotion || mood != .idle || showing != nil
                ? nil : GenieCues.current(cue, at: date, ambient: ambient)
            GeniePainter(
                mood: mood, level: level, t: t, speakingT: date.timeIntervalSinceReferenceDate,
                reaction: active?.0, reactionAge: active?.1 ?? 0, lean: tilt, stride: reduceMotion ? .still : stride,
                vitality: vitality, condition: condition, trick: showing, gaze: reduceMotion ? nil : gaze, alarm: reduceMotion ? 0 : alarm)
                .paint(&context, size: size)
        }
        .accessibilityHidden(true)
    }
}

struct GeniePainter {
    private struct Leg {
        var shoulder: CGPoint
        var knee: CGPoint
        var foot: CGPoint
    }

    private static let shoulders: [CGPoint] = [
        CGPoint(x: 35, y: 76), CGPoint(x: 30, y: 71), CGPoint(x: 28, y: 65), CGPoint(x: 29, y: 59),
    ]
    static let ground: CGFloat = 96.2
    private static let knees: [CGPoint] = [
        CGPoint(x: 27, y: 79), CGPoint(x: 20, y: 72), CGPoint(x: 15, y: 61), CGPoint(x: 13.5, y: 51),
    ]
    private static let feet: [CGPoint] = [
        CGPoint(x: 31, y: ground), CGPoint(x: 17.5, y: ground), CGPoint(x: 8.5, y: ground), CGPoint(x: 2.5, y: ground),
    ]

    let mood: GenieMood
    let level: Double
    let t: Double
    let speakingT: Double
    var reaction: GenieReaction? = nil
    var reactionAge: Double = 0
    var lean: CGSize = .zero
    var stride: GenieStride = .still
    var vitality: Double = 0.7
    var condition: GenieVitalLevel = .happy
    var trick: GenieTrickFrame? = nil
    var gaze: CGPoint? = nil
    var alarm: Double = 0

    private static let blush: Color = Color(red: 0.98, green: 0.36, blue: 0.62)
    private static let gold: Color = Color(red: 1.0, green: 0.84, blue: 0.3)

    private var showing: CGFloat { CGFloat(trick?.rise ?? 0) }

    private var trickEyes: GenieEyeMove { trick?.eyes ?? .normal }

    private var trickMouth: GenieMouthMove { trick?.mouth ?? .normal }

    private var trickAge: Double { trick?.age ?? 0 }

    private var fright: CGFloat { CGFloat(min(max(alarm, 0), 1)) }

    private var envelope: Double {
        guard let reaction = reaction else { return 0 }
        return sin(Double.pi * min(max(reactionAge / reaction.duration, 0), 1))
    }

    private var rise: Double {
        guard let reaction = reaction else { return 0 }
        let up: Double = min(max(reactionAge / 0.22, 0), 1)
        let down: Double = min(max((reaction.duration - reactionAge) / 0.26, 0), 1)
        return up * down
    }

    private var glance: CGFloat {
        if case .glance(let direction) = reaction { return CGFloat(direction * envelope) }
        return 0
    }

    private var surprise: CGFloat {
        max(reaction == .surprise ? CGFloat(envelope) : 0, fright * 0.8, trickEyes == .wide ? showing : 0, air * 0.7)
    }

    private var nod: CGFloat {
        guard reaction == .nod else { return 0 }
        return CGFloat(sin(reactionAge / GenieReaction.nod.duration * Double.pi * 2)) * 1.8
    }

    private var dizzy: CGFloat { max(reaction == .dizzy ? CGFloat(envelope) : 0, trickEyes == .spiral ? showing : 0) }

    private var eureka: CGFloat { reaction == .eureka ? CGFloat(rise) : 0 }

    private var giggle: CGFloat { reaction == .giggle ? CGFloat(rise) : 0 }

    private var cheer: CGFloat { reaction == .cheer ? CGFloat(rise) : 0 }

    private var waving: CGFloat {
        if mood == .greeting || reaction == .wave { return mood == .greeting ? 1 : CGFloat(rise) }
        return eureka
    }

    private var tiltX: CGFloat { min(max(lean.width, -1), 1) }

    private var tiltY: CGFloat { min(max(lean.height, -1), 1) }

    private var walk: CGFloat { CGFloat(min(max(stride.walking, 0), 1)) }

    private var air: CGFloat { CGFloat(min(max(stride.air, 0), 1)) }

    private var landing: CGFloat { CGFloat(min(max(stride.land, 0), 1)) }

    private var energy: Double { 0.45 + 0.55 * min(max(vitality, 0), 1) }

    private var bob: CGFloat {
        let idle: CGFloat = CGFloat(sin(t * 1.6 * energy)) * 1.4
        let step: CGFloat = CGFloat(abs(sin(stride.phase))) * 2.2 * walk
        let hop: CGFloat = giggle * CGFloat(abs(sin(reactionAge * 17))) * 2.2
        return idle - step - hop + nod
    }

    private var sway: Double {
        let idle: Double = sin(t * 0.8) * 1.2
        let wobble: Double = Double(dizzy) * sin(reactionAge * 9) * 9
        let heading: Double = stride.facing * 7 * Double(walk)
        return Double(tiltX) * 10 + idle + wobble + heading + Double(glance) * 5
    }

    private var look: CGPoint {
        var x: CGFloat = glance * 1.2 + tiltX * 0.9 + CGFloat(stride.facing) * 1.1 * walk
        var y: CGFloat = tiltY * 0.6
        if let gaze = gaze {
            x = x * 0.3 + gaze.x * 1.5
            y = y * 0.3 + gaze.y * 1.3
        }
        switch trickEyes {
        case .roll:
            x += CGFloat(cos(trickAge * 6)) * 1.4 * showing
            y += CGFloat(sin(trickAge * 6)) * 1.2 * showing
        case .up: y -= 1.4 * showing
        case .down: y += 1.3 * showing
        default: break
        }
        if mood == .thinking {
            x += CGFloat(sin(t * 1.3)) * 0.8
            y -= 1.1
        } else if mood == .idle && walk < 0.1 {
            x += CGFloat(sin(t * 0.37) * sin(t * 0.91)) * 0.9
            y += CGFloat(sin(t * 0.53)) * 0.4
        }
        return CGPoint(x: x, y: y)
    }

    private var lid: CGFloat {
        let cycle: Double = t.truncatingRemainder(dividingBy: 4.3)
        let blink: CGFloat = cycle < 0.14 ? CGFloat(sin(cycle / 0.14 * Double.pi)) : 0
        let tired: CGFloat
        switch condition {
        case .sick: tired = 0.45
        case .hungry: tired = 0.28
        case .peckish: tired = 0.12
        default: tired = 0
        }
        let squint: CGFloat = giggle * 0.6
        let trickLid: CGFloat
        switch trickEyes {
        case .closed: trickLid = 0.95 * showing
        case .sleepy: trickLid = 0.62 * showing
        case .squint: trickLid = 0.55 * showing
        default: trickLid = 0
        }
        return min(max(blink, tired - surprise * 0.5, squint, trickLid), 0.95)
    }

    private var tint: Color {
        condition == .sick ? GenieInk.sickly : GenieInk.glow
    }

    private var glowStrength: Double {
        let base: Double = 0.35 + 0.65 * min(max(vitality, 0), 1)
        let pulse: Double = mood == .thinking ? 0.2 * (0.5 + 0.5 * sin(t * 6)) : 0
        return min(base + pulse + Double(eureka) * 0.4, 1)
    }

    func paint(_ context: inout GraphicsContext, size: CGSize) {
        let side: CGFloat = min(size.width, size.height)
        guard side > 0 else { return }
        let unit: CGFloat = side / 100
        var world: GraphicsContext = context
        world.translateBy(x: (size.width - side) / 2, y: (size.height - side) / 2)
        world.scaleBy(x: unit, y: unit)
        world.translateBy(x: 0, y: 50 + GenieGravity.stand * 100 - GeniePainter.ground)
        aura(&world)
        var stage: GraphicsContext = world
        if let trick = trick {
            stage.opacity = trick.opacity
            stage.translateBy(x: 50 + CGFloat(trick.dx), y: 60 + CGFloat(trick.dy))
            stage.rotate(by: .degrees(trick.rotation))
            stage.scaleBy(x: CGFloat(trick.scaleX), y: CGFloat(trick.scaleY))
            stage.translateBy(x: -50, y: -60)
            if abs(trick.hue) > 0.5 { stage.addFilter(.hueRotation(.degrees(trick.hue))) }
        }
        if fright > 0.02 {
            stage.translateBy(x: 50, y: 92)
            stage.scaleBy(x: 1 + fright * 0.06, y: 1 - fright * 0.12)
            stage.translateBy(x: -50, y: -92)
        }
        if landing > 0.01 {
            stage.translateBy(x: 50, y: 93)
            stage.scaleBy(x: 1 + landing * 0.1, y: 1 - landing * 0.14)
            stage.translateBy(x: -50, y: -93)
        }
        shadow(&stage)
        var body: GraphicsContext = stage
        body.translateBy(x: 50, y: 90)
        body.rotate(by: .degrees(sway))
        body.translateBy(x: -50, y: -90 + bob)
        tail(&body)
        legs(&body, front: false)
        head(&body)
        turban(&body)
        eyes(&body)
        mouth(&body)
        legs(&body, front: true)
        burst(&body)
        if condition == .sick { sweat(&body) }
        effects(&world)
    }

    private func effects(_ context: inout GraphicsContext) {
        guard let trick = trick else { return }
        for particle in trick.particles where particle.opacity > 0.02 {
            var layer: GraphicsContext = context
            layer.opacity = particle.opacity
            layer.draw(Text(verbatim: particle.glyph).font(.system(size: CGFloat(particle.size) * 1.3)), at: CGPoint(x: particle.x, y: particle.y))
        }
    }

    private func trickLeg(_ move: GenieLegMove, index: Int, right: Bool, age: Double) -> (knee: CGPoint, foot: CGPoint)? {
        func p(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: x, y: y) }
        let s0: CGPoint = GeniePainter.shoulders[index]
        let k0: CGPoint = GeniePainter.knees[index]
        let f0: CGPoint = GeniePainter.feet[index]
        let kx: Double = Double(k0.x)
        let ky: Double = Double(k0.y)
        let fx: Double = Double(f0.x)
        let fy: Double = Double(f0.y)
        let group: Double = (index % 2 == 0) == !right ? 0 : Double.pi
        let sideShift: Double = right ? Double.pi : 0
        switch move {
        case .rest:
            return nil
        case .wave:
            guard index == 0, !right else { return nil }
            return (p(19, 52), p(10 + sin(age * 13) * 6, 36))
        case .cheer:
            guard index == 0 else { return nil }
            return (p(20, 50), p(10 + sin(age * 13 + (right ? 1.4 : 0)) * 6, 32))
        case .dance:
            let lift: Double = max(0, sin(age * 9 + group)) * 8
            return (p(kx, ky - lift * 0.6), p(fx + sin(age * 4.5 + group) * 3, fy - lift))
        case .march:
            let lift: Double = max(0, sin(age * 12 + group)) * 6
            return (p(kx, ky - lift * 0.55), p(fx, fy - lift))
        case .drum:
            guard index == 0 else { return nil }
            return (p(26, 60), p(36, 82 - max(0, sin(age * 22 + sideShift)) * 8))
        case .swim:
            let a: Double = age * 6 + Double(index) * 0.8 + sideShift
            return (k0, p(kx + cos(a) * 12, ky + 14 + sin(a) * 9))
        case .splay:
            let dx: Double = Double(s0.x) - 50
            let dy: Double = Double(s0.y) - 62
            let length: Double = max(hypot(dx, dy), 1)
            return (p(Double(s0.x) + dx / length * 16, Double(s0.y) + dy / length * 16 - 4), p(Double(s0.x) + dx / length * 36, Double(s0.y) + dy / length * 36))
        case .tuck:
            return (mix(k0, p(34, 64), 0.6), mix(f0, p(42 - Double(index) * 1.5, 76), 0.85))
        case .coverEyes:
            guard index <= 1 else { return nil }
            return index == 0 ? (p(28, 54), p(44, 58)) : (p(22, 50), p(35, 53))
        case .clap:
            guard index == 0 else { return nil }
            return (p(30, 50), p(47 - abs(sin(age * 8)) * 7, 52))
        case .flex:
            guard index == 0 else { return nil }
            return (p(14, 46), p(24, 30 + sin(age * 7) * 2))
        case .flap:
            guard index <= 1 else { return nil }
            let beat: Double = sin(age * 14 + Double(index)) * 14
            return (p(16, 52 + beat * 0.4), p(4, 50 + beat))
        case .reach:
            guard index == 0 else { return nil }
            return (p(26, 40), p(22, 16 + sin(age * 5) * 2))
        case .shrug:
            guard index == 0 else { return nil }
            let bob: Double = sin(age * 6) * 2
            return (p(20, 56 - bob), p(6, 46 - bob))
        case .typing:
            guard index == 0 else { return nil }
            return (p(28, 70), p(38, 86 - max(0, sin(age * 25 + (right ? 1.7 : 0))) * 4))
        case .guitar:
            guard index == 0 else { return nil }
            return right ? (p(22, 58), p(10, 50)) : (p(30, 66), p(44 + sin(age * 18) * 4, 74))
        case .conduct:
            guard index == 0, right else { return nil }
            return (p(22, 48), p(16 + sin(age * 5) * 8, 30 + sin(age * 10) * 6))
        case .salute:
            guard index == 0, right else { return nil }
            return (p(18, 44), p(34, 34))
        case .point:
            guard index == 0, right else { return nil }
            return (p(14, 58), p(1, 54))
        case .wiggle:
            let k: Double = Double(index)
            return (
                p(kx + sin(age * 13 + k) * 2, ky + cos(age * 11 + k) * 2),
                p(fx + sin(age * 16 + k * 1.7 + (right ? 1 : 0)) * 3, fy + cos(age * 13 + k) * 3))
        case .hug:
            guard index <= 1 else { return nil }
            return (p(24, 60), p(44, 74 - Double(index) * 6))
        }
    }

    private func ellipse(_ x: CGFloat, _ y: CGFloat, _ rx: CGFloat, _ ry: CGFloat) -> Path {
        Path(ellipseIn: CGRect(x: x - rx, y: y - ry, width: rx * 2, height: ry * 2))
    }

    private func glow(_ context: inout GraphicsContext, _ x: CGFloat, _ y: CGFloat, _ r: CGFloat, _ color: Color) {
        context.fill(
            ellipse(x, y, r, r),
            with: .radialGradient(Gradient(colors: [color, color.opacity(0)]), center: CGPoint(x: x, y: y), startRadius: 0, endRadius: r))
    }

    private func aura(_ context: inout GraphicsContext) {
        let pulse: CGFloat = CGFloat(0.5 + 0.5 * sin(t * 2.1))
        let boost: CGFloat = mood == .listening ? CGFloat(min(max(level, 0), 1)) * 8 : 0
        let radius: CGFloat = 38 + pulse * 3 + boost
        let color: Color = mood == .thinking || trick?.magic == .incense ? GenieInk.violet : tint
        glow(&context, 50, 60, radius, color.opacity(0.2 * glowStrength))
        guard mood == .listening else { return }
        for ring in 0..<3 {
            let phase: CGFloat = CGFloat((t * 0.8 + Double(ring) / 3).truncatingRemainder(dividingBy: 1))
            let r: CGFloat = 26 + phase * 24
            context.stroke(ellipse(50, 60, r, r), with: .color(tint.opacity(Double(1 - phase) * 0.5)), lineWidth: 0.8)
        }
    }

    private func shadow(_ context: inout GraphicsContext) {
        let grounded: Double = 1 - Double(air)
        guard grounded > 0.02 else { return }
        let y: CGFloat = GeniePainter.ground + 0.6
        var flat: GraphicsContext = context
        flat.translateBy(x: 50, y: y)
        flat.scaleBy(x: 1, y: 0.11)
        flat.translateBy(x: -50, y: -y)
        flat.fill(
            ellipse(50, y, 36, 36),
            with: .radialGradient(
                Gradient(stops: [
                    .init(color: GenieInk.pitch.opacity(0.42 * grounded), location: 0),
                    .init(color: GenieInk.pitch.opacity(0.18 * grounded), location: 0.55),
                    .init(color: GenieInk.pitch.opacity(0), location: 1),
                ]),
                center: CGPoint(x: 50, y: y), startRadius: 0, endRadius: 36))
    }

    private func toBody(_ ground: CGPoint) -> CGPoint {
        let angle: Double = sway * Double.pi / 180
        let c: CGFloat = CGFloat(cos(angle))
        let s: CGFloat = CGFloat(sin(angle))
        let dx: CGFloat = ground.x - 50
        let dy: CGFloat = ground.y - 90
        return CGPoint(x: dx * c + dy * s + 50, y: -dx * s + dy * c + 90 - bob)
    }

    private func tail(_ context: inout GraphicsContext) {
        let swish: CGFloat = CGFloat(sin(t * 1.9)) * 3 + CGFloat(stride.facing) * -4 * walk
        var path: Path = Path()
        path.move(to: CGPoint(x: 45, y: 79))
        path.addCurve(to: CGPoint(x: 42 + swish, y: 92), control1: CGPoint(x: 39, y: 84), control2: CGPoint(x: 49 + swish, y: 87))
        path.addCurve(to: CGPoint(x: 53 + swish * 1.3, y: 98.5), control1: CGPoint(x: 35 + swish, y: 97.5), control2: CGPoint(x: 47 + swish, y: 101))
        path.addCurve(to: CGPoint(x: 45.8 + swish, y: 93), control1: CGPoint(x: 49.5 + swish, y: 97), control2: CGPoint(x: 44.5 + swish, y: 96.2))
        path.addCurve(to: CGPoint(x: 56, y: 79), control1: CGPoint(x: 58.5 + swish, y: 87), control2: CGPoint(x: 55, y: 82))
        path.closeSubpath()
        context.fill(
            path,
            with: .linearGradient(
                Gradient(colors: [GenieInk.shellLight, GenieInk.shell, GenieInk.pitch]), startPoint: CGPoint(x: 42, y: 80),
                endPoint: CGPoint(x: 52, y: 99)))
        context.stroke(path, with: .color(tint.opacity(0.32 * glowStrength)), lineWidth: 0.45)
    }

    private func legs(_ context: inout GraphicsContext, front: Bool) {
        let order: [Int] = front ? [1, 0] : [3, 2]
        for index in order {
            for side in [CGFloat(-1), CGFloat(1)] {
                let leg: Leg = pose(index, side: side)
                drawLeg(&context, leg, back: !front)
            }
        }
    }

    private func pose(_ index: Int, side: CGFloat) -> Leg {
        func m(_ p: CGPoint) -> CGPoint { side < 0 ? p : CGPoint(x: 100 - p.x, y: p.y) }
        let shoulder: CGPoint = m(GeniePainter.shoulders[index])
        var knee: CGPoint = m(GeniePainter.knees[index])
        var foot: CGPoint = m(GeniePainter.feet[index])
        let group: Double = (index % 2 == 0) == (side < 0) ? 0 : Double.pi
        let swing: Double = stride.phase + group
        let lift: CGFloat = CGFloat(max(0, sin(swing))) * 6 * walk
        let reach: CGFloat = CGFloat(cos(swing)) * 3 * walk * CGFloat(stride.facing == 0 ? 1 : stride.facing)
        let breathe: CGFloat = CGFloat(sin(t * 1.6 * energy + Double(index))) * 0.5
        foot.y -= lift
        foot.x += reach
        foot = toBody(foot)
        knee.y -= lift * 0.55 + breathe
        knee.x += reach * 0.5
        let droop: CGFloat = CGFloat(1 - min(max(vitality, 0), 1)) * 3
        knee.y += droop
        if air > 0.01 {
            let dx: CGFloat = shoulder.x - 50
            let dy: CGFloat = shoulder.y - 62
            let length: CGFloat = max(hypot(dx, dy), 0.01)
            let flail: CGFloat = CGFloat(sin(stride.phase + Double(index) * 1.3 + (side < 0 ? 0 : 0.7)))
            let outKnee: CGPoint = CGPoint(x: shoulder.x + dx / length * 13, y: shoulder.y + dy / length * 13 - 4 + flail * 2)
            let outFoot: CGPoint = CGPoint(x: shoulder.x + dx / length * 27, y: shoulder.y + dy / length * 27 - 3 + flail * 6)
            knee = mix(knee, outKnee, air)
            foot = mix(foot, outFoot, air)
        }
        if index == 0 {
            let raised: CGFloat = side < 0 ? max(waving, cheer) : cheer
            if raised > 0.01 {
                let wiggle: CGFloat = CGFloat(sin(reactionAge * 13 + (side < 0 ? 0 : 1.4))) * 6 * raised
                let upFoot: CGPoint = m(CGPoint(x: 10 + wiggle * (side < 0 ? 1 : -1), y: 38))
                let upKnee: CGPoint = m(CGPoint(x: 21, y: 56))
                foot = mix(foot, upFoot, raised)
                knee = mix(knee, upKnee, raised)
            }
        }
        if let trick = trick, showing > 0.01, let target = trickLeg(trick.legs, index: index, right: side > 0, age: trick.age) {
            knee = mix(knee, m(target.knee), showing)
            foot = mix(foot, m(target.foot), showing)
        }
        return Leg(shoulder: shoulder, knee: knee, foot: foot)
    }

    private func mix(_ a: CGPoint, _ b: CGPoint, _ k: CGFloat) -> CGPoint {
        CGPoint(x: a.x + (b.x - a.x) * k, y: a.y + (b.y - a.y) * k)
    }

    private func drawLeg(_ context: inout GraphicsContext, _ leg: Leg, back: Bool) {
        let shade: Double = back ? 0.85 : 1
        let outward: CGFloat = leg.foot.x < 50 ? -1 : 1
        context.fill(
            ellipse(leg.shoulder.x, leg.shoulder.y, 3, 3),
            with: .radialGradient(
                Gradient(colors: [GenieInk.shellLight.opacity(shade), GenieInk.pitch.opacity(shade)]),
                center: CGPoint(x: leg.shoulder.x - 0.8, y: leg.shoulder.y - 0.9), startRadius: 0, endRadius: 3.4))
        var upper: Path = Path()
        upper.move(to: leg.shoulder)
        upper.addLine(to: leg.knee)
        context.stroke(upper, with: .color(GenieInk.pitch.opacity(shade)), style: StrokeStyle(lineWidth: 6.2, lineCap: .round))
        context.stroke(upper, with: .color(GenieInk.shell.opacity(shade)), style: StrokeStyle(lineWidth: 4.6, lineCap: .round))
        var sheen: Path = Path()
        sheen.move(to: CGPoint(x: leg.shoulder.x, y: leg.shoulder.y - 1.1))
        sheen.addLine(to: CGPoint(x: leg.knee.x, y: leg.knee.y - 1.1))
        context.stroke(sheen, with: .color(GenieInk.shellLight.opacity(0.75 * shade)), style: StrokeStyle(lineWidth: 1.1, lineCap: .round))
        let dx: CGFloat = leg.foot.x - leg.knee.x
        let dy: CGFloat = leg.foot.y - leg.knee.y
        let length: CGFloat = max(sqrt(dx * dx + dy * dy), 0.01)
        var nx: CGFloat = -dy / length
        var ny: CGFloat = dx / length
        if nx * outward < 0 || (abs(nx) < 0.05 && ny > 0) {
            nx = -nx
            ny = -ny
        }
        let mid: CGPoint = CGPoint(x: leg.knee.x + dx * 0.42, y: leg.knee.y + dy * 0.42)
        let bulge: CGFloat = min(length * 0.24, 5.5)
        var claw: Path = Path()
        claw.move(to: CGPoint(x: leg.knee.x + nx * 3.7, y: leg.knee.y + ny * 3.7))
        claw.addQuadCurve(to: leg.foot, control: CGPoint(x: mid.x + nx * (bulge + 3), y: mid.y + ny * (bulge + 3)))
        claw.addQuadCurve(
            to: CGPoint(x: leg.knee.x - nx * 3.1, y: leg.knee.y - ny * 3.1),
            control: CGPoint(x: mid.x + nx * (bulge * 0.5 - 0.6), y: mid.y + ny * (bulge * 0.5 - 0.6)))
        claw.closeSubpath()
        context.fill(
            claw,
            with: .linearGradient(
                Gradient(stops: [
                    Gradient.Stop(color: GenieInk.shellLight.opacity(shade), location: 0),
                    Gradient.Stop(color: GenieInk.shell.opacity(shade), location: 0.35),
                    Gradient.Stop(color: GenieInk.pitch.opacity(shade), location: 0.72),
                    Gradient.Stop(color: tint.opacity(glowStrength * shade), location: 1),
                ]),
                startPoint: leg.knee, endPoint: leg.foot))
        var rim: Path = Path()
        rim.move(to: CGPoint(x: leg.knee.x + nx * 2.4, y: leg.knee.y + ny * 2.4))
        rim.addQuadCurve(
            to: mix(leg.knee, leg.foot, 0.82), control: CGPoint(x: mid.x + nx * (bulge + 1.7), y: mid.y + ny * (bulge + 1.7)))
        context.stroke(rim, with: .color(GenieInk.silver.opacity(0.28 * shade)), style: StrokeStyle(lineWidth: 0.7, lineCap: .round))
        var vein: Path = Path()
        vein.move(to: mix(leg.knee, leg.foot, 0.12))
        vein.addQuadCurve(to: mix(leg.knee, leg.foot, 0.62), control: CGPoint(x: mid.x + nx * bulge * 0.8, y: mid.y + ny * bulge * 0.8))
        context.stroke(vein, with: .color(tint.opacity(0.7 * glowStrength * shade)), style: StrokeStyle(lineWidth: 0.75, lineCap: .round))
        glow(&context, leg.foot.x, leg.foot.y, 3.6, tint.opacity(0.6 * glowStrength * shade))
        glow(&context, leg.knee.x, leg.knee.y, 6, tint.opacity(0.42 * glowStrength * shade))
        context.fill(
            ellipse(leg.knee.x, leg.knee.y, 3.9, 3.9),
            with: .radialGradient(
                Gradient(colors: [GenieInk.shellLight.opacity(shade), GenieInk.pitch.opacity(shade)]),
                center: CGPoint(x: leg.knee.x - 1, y: leg.knee.y - 1.2), startRadius: 0, endRadius: 4.4))
        context.fill(
            ellipse(leg.knee.x, leg.knee.y, 2.2, 2.2),
            with: .radialGradient(
                Gradient(colors: [Color.white.opacity(0.9 * shade), tint.opacity(glowStrength * shade), GenieInk.glowDeep.opacity(shade)]),
                center: CGPoint(x: leg.knee.x - 0.5, y: leg.knee.y - 0.5), startRadius: 0, endRadius: 2.4))
        context.stroke(ellipse(leg.knee.x, leg.knee.y, 3.9, 3.9), with: .color(GenieInk.silverDark.opacity(0.5 * shade)), lineWidth: 0.35)
    }

    private func head(_ context: inout GraphicsContext) {
        let face: Path = ellipse(50, 61, 25.5, 25)
        context.fill(
            face,
            with: .radialGradient(
                Gradient(colors: [GenieInk.shellLight, GenieInk.shell, GenieInk.pitch]),
                center: CGPoint(x: 41, y: 54), startRadius: 1, endRadius: 34))
        if condition == .sick {
            context.fill(face, with: .color(GenieInk.sickly.opacity(0.16)))
        }
        var rim: Path = Path()
        rim.addArc(center: CGPoint(x: 50, y: 61), radius: 24.6, startAngle: .degrees(15), endAngle: .degrees(80), clockwise: false)
        context.stroke(rim, with: .color(tint.opacity(0.4 * glowStrength)), style: StrokeStyle(lineWidth: 0.8, lineCap: .round))
        var gloss: GraphicsContext = context
        gloss.clip(to: face)
        gloss.fill(ellipse(37, 77, 7, 4), with: .color(Color.white.opacity(0.05)))
        context.fill(ellipse(38, 73.5, 4, 2.2), with: .color(GenieInk.violet.opacity(0.24)))
        context.fill(ellipse(62, 73.5, 4, 2.2), with: .color(GenieInk.violet.opacity(0.24)))
    }

    private func feather(_ context: inout GraphicsContext) {
        let hunger: Double = 1 - min(max(vitality, 0), 1)
        var plume: GraphicsContext = context
        plume.translateBy(x: 50, y: 27)
        plume.rotate(by: .degrees(sin(t * 1.1) * 4 + hunger * 38 + Double(tiltX) * -6))
        plume.translateBy(x: -50, y: -27)
        let base: CGPoint = CGPoint(x: 50, y: 28)
        let bend: CGPoint = CGPoint(x: 44, y: 13)
        let tip: CGPoint = CGPoint(x: 55, y: 0.5)
        func spine(_ u: CGFloat) -> CGPoint {
            let a: CGFloat = (1 - u) * (1 - u)
            let b: CGFloat = 2 * (1 - u) * u
            let c: CGFloat = u * u
            return CGPoint(x: a * base.x + b * bend.x + c * tip.x, y: a * base.y + b * bend.y + c * tip.y)
        }
        func normal(_ u: CGFloat) -> CGPoint {
            let dx: CGFloat = 2 * (1 - u) * (bend.x - base.x) + 2 * u * (tip.x - bend.x)
            let dy: CGFloat = 2 * (1 - u) * (bend.y - base.y) + 2 * u * (tip.y - bend.y)
            let length: CGFloat = max(hypot(dx, dy), 0.01)
            return CGPoint(x: -dy / length, y: dx / length)
        }
        let steps: Int = 14
        var left: [CGPoint] = []
        var right: [CGPoint] = []
        for k in 0...steps {
            let u: CGFloat = CGFloat(k) / CGFloat(steps)
            let notch: CGFloat = k % 2 == 1 ? 0.74 : 1
            let width: CGFloat = 7.4 * CGFloat(pow(sin(Double.pi * Double(min(u * 1.05, 1))), 0.75)) * (1 - u * 0.3) * notch
            let p: CGPoint = spine(u)
            let n: CGPoint = normal(u)
            left.append(CGPoint(x: p.x + n.x * width, y: p.y + n.y * width))
            right.append(CGPoint(x: p.x - n.x * width * 0.92, y: p.y - n.y * width * 0.92))
        }
        var leaf: Path = Path()
        leaf.move(to: base)
        for point in left { leaf.addLine(to: point) }
        for point in right.reversed() { leaf.addLine(to: point) }
        leaf.closeSubpath()
        plume.fill(
            leaf,
            with: .linearGradient(
                Gradient(colors: [GenieInk.shellLight, GenieInk.shell, GenieInk.pitch]), startPoint: CGPoint(x: 42, y: 8),
                endPoint: CGPoint(x: 58, y: 26)))
        plume.stroke(leaf, with: .color(GenieInk.pitch.opacity(0.6)), lineWidth: 0.35)
        for k in Swift.stride(from: 2, to: steps - 1, by: 2) {
            var barb: Path = Path()
            barb.move(to: spine(CGFloat(k) / CGFloat(steps) - 0.03))
            barb.addLine(to: left[k + 1])
            barb.move(to: spine(CGFloat(k) / CGFloat(steps) - 0.03))
            barb.addLine(to: right[k + 1])
            plume.stroke(barb, with: .color(GenieInk.pitch.opacity(0.55)), lineWidth: 0.4)
        }
        var vein: Path = Path()
        vein.move(to: base)
        vein.addQuadCurve(to: spine(0.94), control: bend)
        plume.stroke(vein, with: .color(tint.opacity(0.3 * glowStrength)), style: StrokeStyle(lineWidth: 2.4, lineCap: .round))
        plume.stroke(vein, with: .color(tint.opacity(0.95 * glowStrength)), style: StrokeStyle(lineWidth: 0.8, lineCap: .round))
    }

    private var domePath: Path {
        var dome: Path = Path()
        dome.move(to: CGPoint(x: 21.5, y: 60))
        dome.addCurve(to: CGPoint(x: 50, y: 23), control1: CGPoint(x: 15, y: 39), control2: CGPoint(x: 29, y: 23))
        dome.addCurve(to: CGPoint(x: 78.5, y: 60), control1: CGPoint(x: 71, y: 23), control2: CGPoint(x: 85, y: 39))
        dome.addCurve(to: CGPoint(x: 50, y: 44.5), control1: CGPoint(x: 75.5, y: 51), control2: CGPoint(x: 64, y: 44.5))
        dome.addCurve(to: CGPoint(x: 21.5, y: 60), control1: CGPoint(x: 36, y: 44.5), control2: CGPoint(x: 24.5, y: 51))
        dome.closeSubpath()
        return dome
    }

    private var archPath: Path {
        var arch: Path = Path()
        arch.move(to: CGPoint(x: 22.4, y: 59.6))
        arch.addCurve(to: CGPoint(x: 50, y: 44.5), control1: CGPoint(x: 25.4, y: 51), control2: CGPoint(x: 36, y: 44.5))
        arch.addCurve(to: CGPoint(x: 77.6, y: 59.6), control1: CGPoint(x: 64, y: 44.5), control2: CGPoint(x: 74.6, y: 51))
        return arch
    }

    private func silver(_ context: inout GraphicsContext, _ path: Path, width: CGFloat) {
        context.stroke(path, with: .color(GenieInk.pitch.opacity(0.55)), style: StrokeStyle(lineWidth: width + 1, lineCap: .round, lineJoin: .round))
        context.stroke(path, with: .color(GenieInk.silverDark), style: StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round))
        context.stroke(path, with: .color(GenieInk.silver), style: StrokeStyle(lineWidth: width * 0.62, lineCap: .round, lineJoin: .round))
        var shine: GraphicsContext = context
        shine.translateBy(x: 0, y: -width * 0.18)
        shine.stroke(path, with: .color(Color.white.opacity(0.7)), style: StrokeStyle(lineWidth: width * 0.18, lineCap: .round, lineJoin: .round))
    }

    private func turban(_ context: inout GraphicsContext) {
        feather(&context)
        let dome: Path = domePath
        context.fill(
            dome,
            with: .linearGradient(
                Gradient(colors: [GenieInk.lilac, GenieInk.violet, GenieInk.plum, GenieInk.shell]), startPoint: CGPoint(x: 36, y: 22),
                endPoint: CGPoint(x: 58, y: 64)))
        var cloth: GraphicsContext = context
        cloth.clip(to: dome)
        for k in 0..<4 {
            let y: CGFloat = 58 - CGFloat(k) * 8.5
            let peak: CGFloat = 31 - CGFloat(k) * 6
            var fold: Path = Path()
            fold.move(to: CGPoint(x: 18, y: y))
            fold.addCurve(to: CGPoint(x: 82, y: y - 3), control1: CGPoint(x: 26, y: peak), control2: CGPoint(x: 70, y: peak - 4))
            cloth.stroke(fold, with: .color(GenieInk.pitch.opacity(0.42)), lineWidth: 2.2)
            var lift: GraphicsContext = cloth
            lift.translateBy(x: 0, y: 1.5)
            lift.stroke(fold, with: .color(GenieInk.lilac.opacity(0.4)), lineWidth: 0.9)
        }
        cloth.fill(ellipse(37, 31, 10, 6), with: .color(Color.white.opacity(0.1)))
        var trim: Path = Path()
        trim.move(to: CGPoint(x: 23, y: 55))
        trim.addQuadCurve(to: CGPoint(x: 32, y: 32), control: CGPoint(x: 20.5, y: 40))
        trim.move(to: CGPoint(x: 77, y: 55))
        trim.addQuadCurve(to: CGPoint(x: 68, y: 32), control: CGPoint(x: 79.5, y: 40))
        cloth.stroke(trim, with: .color(tint.opacity(0.32 * glowStrength)), style: StrokeStyle(lineWidth: 2, lineCap: .round))
        cloth.stroke(trim, with: .color(tint.opacity(0.85 * glowStrength)), style: StrokeStyle(lineWidth: 0.6, lineCap: .round))
        silver(&context, archPath, width: 3.4)
        var chevron: Path = Path()
        chevron.move(to: CGPoint(x: 33.5, y: 28.5))
        chevron.addQuadCurve(to: CGPoint(x: 50, y: 40), control: CGPoint(x: 43, y: 31))
        chevron.addQuadCurve(to: CGPoint(x: 66.5, y: 28.5), control: CGPoint(x: 57, y: 31))
        silver(&context, chevron, width: 2.6)
        let gem: CGPoint = CGPoint(x: 50, y: 40.5)
        glow(&context, gem.x, gem.y, 8 + eureka * 5, tint.opacity(0.5 * glowStrength))
        context.fill(
            ellipse(gem.x, gem.y, 5.4, 5.4),
            with: .radialGradient(
                Gradient(colors: [Color.white, GenieInk.silver, GenieInk.silverDark]), center: CGPoint(x: gem.x - 1.8, y: gem.y - 2),
                startRadius: 0, endRadius: 7))
        context.fill(ellipse(gem.x, gem.y, 3.9, 3.9), with: .color(GenieInk.pitch))
        context.fill(
            ellipse(gem.x, gem.y, 3, 3),
            with: .radialGradient(
                Gradient(colors: [Color.white, tint, GenieInk.glowDeep]), center: CGPoint(x: gem.x - 0.8, y: gem.y - 0.8), startRadius: 0,
                endRadius: 3.4))
        context.fill(ellipse(gem.x - 1, gem.y - 1.1, 0.8, 0.7), with: .color(Color.white.opacity(0.95)))
        var diamond: Path = Path()
        diamond.move(to: CGPoint(x: 50, y: 46.3))
        diamond.addLine(to: CGPoint(x: 52.6, y: 49.8))
        diamond.addLine(to: CGPoint(x: 50, y: 54))
        diamond.addLine(to: CGPoint(x: 47.4, y: 49.8))
        diamond.closeSubpath()
        context.fill(
            diamond,
            with: .linearGradient(Gradient(colors: [Color.white, GenieInk.silver, GenieInk.silverDark]), startPoint: CGPoint(x: 48, y: 46), endPoint: CGPoint(x: 52, y: 54)))
        context.stroke(diamond, with: .color(GenieInk.pitch.opacity(0.5)), lineWidth: 0.3)
        var inner: Path = Path()
        inner.move(to: CGPoint(x: 50, y: 48.2))
        inner.addLine(to: CGPoint(x: 51.2, y: 49.9))
        inner.addLine(to: CGPoint(x: 50, y: 51.8))
        inner.addLine(to: CGPoint(x: 48.8, y: 49.9))
        inner.closeSubpath()
        context.fill(inner, with: .color(tint.opacity(glowStrength)))
        if condition == .thriving {
            let cycle: Double = t.truncatingRemainder(dividingBy: 3.4)
            if cycle < 0.6 {
                let k: CGFloat = CGFloat(sin(cycle / 0.6 * Double.pi))
                glint(&context, CGPoint(x: 52.6, y: 38), 2.8 * k)
            }
        }
    }

    private func glint(_ context: inout GraphicsContext, _ at: CGPoint, _ r: CGFloat) {
        guard r > 0.05 else { return }
        var cross: Path = Path()
        cross.move(to: CGPoint(x: at.x - r, y: at.y))
        cross.addLine(to: CGPoint(x: at.x + r, y: at.y))
        cross.move(to: CGPoint(x: at.x, y: at.y - r))
        cross.addLine(to: CGPoint(x: at.x, y: at.y + r))
        context.stroke(cross, with: .color(Color.white.opacity(0.9)), style: StrokeStyle(lineWidth: 0.45, lineCap: .round))
    }

    private func eyes(_ context: inout GraphicsContext) {
        let wide: CGFloat = 1 + surprise * 0.2
        let spots: [(CGFloat, CGFloat, CGFloat)] = [
            (43.6, 66.8, 6), (56.4, 66.8, 6), (33.4, 60.4, 4.4), (66.6, 60.4, 4.4), (44.2, 55.6, 3.1), (55.8, 55.6, 3.1),
        ]
        for (index, spot) in spots.enumerated() {
            eye(&context, CGPoint(x: spot.0, y: spot.1), spot.2 * wide, spiral: dizzy > 0.2 && index < 2, index: index)
        }
    }

    private func heart(_ c: CGPoint, _ r: CGFloat) -> Path {
        var path: Path = Path()
        path.move(to: CGPoint(x: c.x, y: c.y + r * 0.8))
        path.addCurve(
            to: CGPoint(x: c.x, y: c.y - r * 0.35), control1: CGPoint(x: c.x - r * 1.3, y: c.y + r * 0.1),
            control2: CGPoint(x: c.x - r * 0.6, y: c.y - r * 1.1))
        path.addCurve(
            to: CGPoint(x: c.x, y: c.y + r * 0.8), control1: CGPoint(x: c.x + r * 0.6, y: c.y - r * 1.1),
            control2: CGPoint(x: c.x + r * 1.3, y: c.y + r * 0.1))
        path.closeSubpath()
        return path
    }

    private func star(_ c: CGPoint, _ r: CGFloat) -> Path {
        var path: Path = Path()
        for k in 0..<10 {
            let angle: Double = Double(k) / 10 * Double.pi * 2 - Double.pi / 2
            let reach: CGFloat = k % 2 == 0 ? r : r * 0.45
            let point: CGPoint = CGPoint(x: c.x + CGFloat(cos(angle)) * reach, y: c.y + CGFloat(sin(angle)) * reach)
            if k == 0 { path.move(to: point) } else { path.addLine(to: point) }
        }
        path.closeSubpath()
        return path
    }

    private func eye(_ context: inout GraphicsContext, _ c: CGPoint, _ r: CGFloat, spiral: Bool, index: Int) {
        glow(&context, c.x, c.y, r * 1.55, tint.opacity(0.3 * glowStrength))
        context.fill(ellipse(c.x, c.y, r * 1.13, r * 1.13), with: .color(GenieInk.pitch))
        context.stroke(ellipse(c.x, c.y, r, r), with: .color(tint.opacity(0.85 * glowStrength + 0.15)), lineWidth: max(r * 0.11, 0.35))
        var iris: GraphicsContext = context
        iris.clip(to: ellipse(c.x, c.y, r * 0.93, r * 0.93))
        let glyph: Bool = index < 2 && showing > 0.3 && [GenieEyeMove.hearts, .stars, .cross].contains(trickEyes)
        if glyph {
            iris.fill(ellipse(c.x, c.y, r, r), with: .color(GenieInk.pitch))
            switch trickEyes {
            case .hearts:
                iris.fill(heart(CGPoint(x: c.x, y: c.y + r * 0.05), r * 0.75), with: .color(GeniePainter.blush))
            case .stars:
                iris.fill(star(c, r * 0.8), with: .color(GeniePainter.gold))
            default:
                var cross: Path = Path()
                cross.move(to: CGPoint(x: c.x - r * 0.5, y: c.y - r * 0.5))
                cross.addLine(to: CGPoint(x: c.x + r * 0.5, y: c.y + r * 0.5))
                cross.move(to: CGPoint(x: c.x + r * 0.5, y: c.y - r * 0.5))
                cross.addLine(to: CGPoint(x: c.x - r * 0.5, y: c.y + r * 0.5))
                iris.stroke(cross, with: .color(tint), style: StrokeStyle(lineWidth: r * 0.22, lineCap: .round))
            }
        } else if spiral {
            iris.fill(ellipse(c.x, c.y, r, r), with: .color(GenieInk.pitch))
            var swirl: Path = Path()
            let turns: Int = 18
            for k in 0...turns {
                let u: CGFloat = CGFloat(k) / CGFloat(turns)
                let angle: Double = Double(u) * Double.pi * 4 + reactionAge * 8
                let p: CGPoint = CGPoint(x: c.x + CGFloat(cos(angle)) * r * 0.8 * u, y: c.y + CGFloat(sin(angle)) * r * 0.8 * u)
                if k == 0 { swirl.move(to: p) } else { swirl.addLine(to: p) }
            }
            iris.stroke(swirl, with: .color(tint), lineWidth: r * 0.14)
        } else {
            let shift: CGPoint = CGPoint(x: c.x + look.x * r * 0.16, y: c.y + look.y * r * 0.16)
            iris.fill(
                ellipse(c.x, c.y, r, r),
                with: .radialGradient(
                    Gradient(colors: [GenieInk.deepSea, GenieInk.pitch]), center: CGPoint(x: shift.x, y: shift.y + r * 0.45),
                    startRadius: 0, endRadius: r * 1.05))
            iris.fill(ellipse(shift.x, shift.y + r * 0.05, r * 0.58, r * 0.58), with: .color(GenieInk.pitch.opacity(0.9)))
            var shine: Path = Path()
            shine.addArc(center: CGPoint(x: shift.x, y: shift.y), radius: r * 0.72, startAngle: .degrees(25), endAngle: .degrees(155), clockwise: false)
            iris.stroke(shine, with: .color(tint.opacity(0.55 * glowStrength + 0.1)), style: StrokeStyle(lineWidth: r * 0.13, lineCap: .round))
            iris.fill(ellipse(shift.x - r * 0.3, shift.y - r * 0.33, r * 0.27, r * 0.23), with: .color(Color.white.opacity(0.96)))
            iris.fill(ellipse(shift.x + r * 0.32, shift.y + r * 0.22, r * 0.11, r * 0.11), with: .color(Color.white.opacity(0.75)))
        }
        let closed: CGFloat = trickEyes == .wink && index == 0 ? max(lid, showing * 0.95) : lid
        if closed > 0.02 {
            var lidContext: GraphicsContext = context
            lidContext.clip(to: ellipse(c.x, c.y, r * 1.02, r * 1.02))
            let drop: CGFloat = r * 2 * closed
            lidContext.fill(Path(CGRect(x: c.x - r * 1.1, y: c.y - r * 1.1, width: r * 2.2, height: drop)), with: .color(GenieInk.shell))
            var edge: Path = Path()
            edge.move(to: CGPoint(x: c.x - r, y: c.y - r + drop))
            edge.addLine(to: CGPoint(x: c.x + r, y: c.y - r + drop))
            lidContext.stroke(edge, with: .color(tint.opacity(0.4 * glowStrength)), lineWidth: max(r * 0.1, 0.3))
        }
    }

    private func mouth(_ context: inout GraphicsContext) {
        let c: CGPoint = CGPoint(x: 50, y: 76.4)
        if mood == .speaking || mood == .greeting {
            let open: CGFloat = CGFloat((0.5 + 0.5 * sin(speakingT * 17)) * (0.65 + 0.35 * sin(speakingT * 6.1)))
            context.fill(ellipse(c.x, c.y + 0.3, 2.6, 0.5 + 1.9 * open), with: .color(GenieInk.pitch))
            context.stroke(ellipse(c.x, c.y + 0.3, 2.6, 0.5 + 1.9 * open), with: .color(tint.opacity(0.8)), lineWidth: 0.45)
            return
        }
        if showing > 0.2, shapedMouth(&context, c) { return }
        if surprise > 0.05 {
            context.stroke(ellipse(c.x, c.y, 1.2 + surprise, 1.2 + surprise * 1.3), with: .color(tint.opacity(0.9)), lineWidth: 0.55)
            return
        }
        let smile: CGFloat
        switch condition {
        case .sick: smile = -1.2
        case .hungry: smile = -0.4
        case .peckish: smile = 0.6
        default: smile = 1.6
        }
        let grin: CGFloat = smile + giggle * 1.2 + cheer * 1 + (mood == .listening ? 0.4 : 0) - (mood == .thinking ? 1.2 : 0)
            + (trickMouth == .grin ? showing * 1.2 : 0)
        var arc: Path = Path()
        arc.move(to: CGPoint(x: c.x - 2.6, y: c.y))
        arc.addQuadCurve(to: CGPoint(x: c.x + 2.6, y: c.y), control: CGPoint(x: c.x, y: c.y + grin))
        context.stroke(arc, with: .color(tint.opacity(0.95)), style: StrokeStyle(lineWidth: 0.6, lineCap: .round))
    }

    private func shapedMouth(_ context: inout GraphicsContext, _ c: CGPoint) -> Bool {
        func hole(_ rx: CGFloat, _ ry: CGFloat, _ dx: CGFloat = 0) {
            context.fill(ellipse(c.x + dx, c.y + 0.3, rx, ry), with: .color(GenieInk.pitch))
            context.stroke(ellipse(c.x + dx, c.y + 0.3, rx, ry), with: .color(tint.opacity(0.85)), lineWidth: 0.45)
        }
        func arc(_ bend: CGFloat) {
            var path: Path = Path()
            path.move(to: CGPoint(x: c.x - 2.6, y: c.y))
            path.addQuadCurve(to: CGPoint(x: c.x + 2.6, y: c.y), control: CGPoint(x: c.x, y: c.y + bend))
            context.stroke(path, with: .color(tint.opacity(0.95)), style: StrokeStyle(lineWidth: 0.6, lineCap: .round))
        }
        switch trickMouth {
        case .normal, .grin:
            return false
        case .o:
            hole(1.5, 1.8)
        case .whistle:
            hole(0.8, 0.9, 1.2)
        case .frown:
            arc(-1.6)
        case .tongue:
            arc(2.2)
            context.fill(ellipse(c.x + 0.6, c.y + 1.9, 1.1, 1.5), with: .color(GeniePainter.blush))
        case .yawn:
            hole(2.3, 2.9)
        case .chatter:
            hole(2.4, 0.4 + CGFloat(abs(sin(trickAge * 24))) * 1.6)
        case .open:
            hole(2.6, 1.9)
        case .flat:
            var line: Path = Path()
            line.move(to: CGPoint(x: c.x - 2.4, y: c.y + 0.4))
            line.addLine(to: CGPoint(x: c.x + 2.4, y: c.y + 0.4))
            context.stroke(line, with: .color(tint.opacity(0.95)), style: StrokeStyle(lineWidth: 0.6, lineCap: .round))
        }
        return true
    }

    private func burst(_ context: inout GraphicsContext) {
        guard eureka > 0.05 else { return }
        let progress: CGFloat = CGFloat(min(max(reactionAge / GenieReaction.eureka.duration, 0), 1))
        for index in 0..<8 {
            let angle: Double = Double(index) / 8 * Double.pi * 2
            let inner: CGFloat = 7 + progress * 4
            let outer: CGFloat = inner + 4 * eureka
            var ray: Path = Path()
            ray.move(to: CGPoint(x: 50 + CGFloat(cos(angle)) * inner, y: 41 + CGFloat(sin(angle)) * inner))
            ray.addLine(to: CGPoint(x: 50 + CGFloat(cos(angle)) * outer, y: 41 + CGFloat(sin(angle)) * outer))
            context.stroke(ray, with: .color(tint.opacity(Double(eureka * (1 - progress * 0.7)))), style: StrokeStyle(lineWidth: 0.7, lineCap: .round))
        }
    }

    private func sweat(_ context: inout GraphicsContext) {
        let cycle: CGFloat = CGFloat(t.truncatingRemainder(dividingBy: 2.6) / 2.6)
        let y: CGFloat = 57 + cycle * 8
        var drop: Path = Path()
        drop.move(to: CGPoint(x: 75, y: y - 2.4))
        drop.addQuadCurve(to: CGPoint(x: 75, y: y + 1.4), control: CGPoint(x: 77.6, y: y + 1))
        drop.addQuadCurve(to: CGPoint(x: 75, y: y - 2.4), control: CGPoint(x: 72.4, y: y + 1))
        context.fill(drop, with: .color(GenieInk.glowSoft.opacity(Double(1 - cycle) * 0.85)))
    }
}
