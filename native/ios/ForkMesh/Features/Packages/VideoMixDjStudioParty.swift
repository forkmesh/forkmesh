import CoreMotion
import FMPackages
import SwiftUI
import UIKit

struct DjEffect: Identifiable, Equatable {
    let id = UUID()
    let reaction: DjReaction
    let sender: String
    let began = Date()
    let seed = UInt64.random(in: 1...UInt64.max)
}

struct DjQueueDrag: Equatable {
    var item: DjQueueItem
    var location: CGPoint
}

@MainActor
final class DjMotionFeed {
    private let manager = CMMotionManager()
    private var samples: [DjMotionSample] = []
    private var orientation: UIInterfaceOrientation = .portrait
    private var counted = 0
    private(set) var latest = DjMotionSample.still
    private(set) var energy: Double = 0

    var running: Bool { manager.isDeviceMotionActive }

    func start() {
        guard manager.isDeviceMotionAvailable, !manager.isDeviceMotionActive else { return }
        manager.deviceMotionUpdateInterval = 1.0 / 30.0
        manager.startDeviceMotionUpdates(to: .main) { [weak self] (data: CMDeviceMotion?, _: Error?) in
            guard let data else { return }
            let push = data.userAcceleration
            MainActor.assumeIsolated { self?.feel(push.x, push.y) }
        }
    }

    func stop() {
        if manager.isDeviceMotionActive { manager.stopDeviceMotionUpdates() }
        samples = []
        latest = .still
        energy = 0
    }

    func drain() -> [DjMotionSample] {
        defer { samples = [] }
        return samples
    }

    private func feel(_ x: Double, _ y: Double) {
        if counted % 30 == 0 {
            orientation = UIApplication.shared.connectedScenes.compactMap { $0 as? UIWindowScene }.first?.interfaceOrientation ?? .portrait
        }
        counted += 1
        let plane = GenieMotion.plane(x, y, orientation)
        let sample = DjMotionSample(x: DjMotion.clamp(Double(plane.width)), y: DjMotion.clamp(Double(plane.height)))
        latest = DjMotionSample(x: latest.x * 0.4 + sample.x * 0.6, y: latest.y * 0.4 + sample.y * 0.6)
        energy = energy * 0.94 + DjMotion.energy([sample]) * 0.06
        samples.append(sample)
        if samples.count > 240 { samples.removeFirst(samples.count - 240) }
    }
}

struct DjPeopleStrip: View {
    let model: DjStudioModel
    let size: CGFloat
    var vertical = false
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        if let jam = model.jam, jam.joined {
            let crowd = model.crowd
            let effects = model.effects
            let moving = !reduceMotion && (model.shareMotion || crowd.contains { $0.energy > 0.01 })
            TimelineView(.animation(minimumInterval: 1.0 / 24, paused: !moving)) { timeline in
                let progress = timeline.date.timeIntervalSince(model.crowdDate) / 1.5
                ScrollView(vertical ? .vertical : .horizontal, showsIndicators: false) {
                    let layout = vertical ? AnyLayout(VStackLayout(spacing: size * 0.35)) : AnyLayout(HStackLayout(spacing: size * 0.35))
                    layout {
                        ForEach(jam.members) { (member: DjJamMember) in
                            icon(member, mine: member.name == jam.me, seen: crowd.first { $0.name == member.name }, effects: effects, progress: progress)
                        }
                    }
                    .padding(.vertical, size * 0.2)
                    .padding(.horizontal, size * 0.3)
                }
                .scrollClipDisabled()
            }
        }
    }

    private func icon(_ member: DjJamMember, mine: Bool, seen: DjJamPresence?, effects: [DjEffect], progress: Double) -> DjPersonIcon {
        var shake = DjMotionSample.still
        var energy: Double = seen?.energy ?? 0
        if mine {
            energy = model.shareMotion ? model.motion.energy : 0
            if model.shareMotion { shake = model.motion.latest }
        } else if let seen {
            shake = DjMotion.replay(seen.motion, progress: progress)
        }
        if reduceMotion { shake = .still }
        let reaction: String? = effects.last { $0.sender == member.name }?.reaction.emoji
        return DjPersonIcon(name: member.name, status: model.status(of: member), host: member.status == "owner", shake: shake, energy: energy, reaction: reaction, size: size)
    }
}

struct DjPersonIcon: View {
    let name: String
    let status: DjPersonStatus
    let host: Bool
    let shake: DjMotionSample
    let energy: Double
    let reaction: String?
    let size: CGFloat

    var body: some View {
        let reach = Double(size) * 0.3
        ZStack {
            Circle().fill(Self.tint(name).gradient)
            Text(name.first.map { String($0).uppercased() } ?? "?")
                .font(.system(size: size * 0.46, weight: .bold, design: .rounded))
                .foregroundStyle(.white)
        }
        .frame(width: size, height: size)
        .overlay(Circle().strokeBorder(ring, style: StrokeStyle(lineWidth: max(1.5, size * 0.08), dash: status == .invited ? [3, 2] : [])))
        .overlay(alignment: .bottomTrailing) { badge }
        .overlay(alignment: .topLeading) {
            if host {
                Image(systemName: "crown.fill")
                    .font(.system(size: size * 0.32))
                    .foregroundStyle(DjTheme.rgb(0xFFC83D))
                    .offset(x: -size * 0.12, y: -size * 0.14)
            }
        }
        .overlay(alignment: .top) {
            if let reaction {
                Text(reaction)
                    .font(.system(size: size * 0.62))
                    .offset(y: -size * 0.62)
                    .transition(.scale.combined(with: .opacity))
            }
        }
        .opacity(status.live ? 1 : 0.45)
        .scaleEffect(CGFloat(1 + min(1, energy) * 0.1))
        .rotationEffect(.degrees(max(-20, min(20, shake.x * 22))))
        .offset(x: CGFloat(max(-reach, min(reach, shake.x * reach))), y: CGFloat(max(-reach, min(reach, -shake.y * reach))))
        .animation(.spring(duration: 0.3), value: reaction)
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("\(name)\(host ? ", host" : ""): \(status.label)\(energy >= DjMotion.dancing ? ", dancing" : "")")
    }

    private var ring: Color {
        switch status {
        case .studio: return DjTheme.play
        case .listening: return DjTheme.active
        case .idle, .invited: return DjTheme.muted
        case .away: return Color.white.opacity(0.15)
        }
    }

    private var symbol: String? {
        switch status {
        case .listening: return "headphones"
        case .studio: return energy >= DjMotion.dancing ? "figure.dance" : nil
        case .away: return "moon.zzz.fill"
        case .idle, .invited: return nil
        }
    }

    @ViewBuilder private var badge: some View {
        if let symbol {
            Image(systemName: symbol)
                .font(.system(size: size * 0.26, weight: .bold))
                .foregroundStyle(.white)
                .frame(width: size * 0.42, height: size * 0.42)
                .background(Circle().fill(status == .away ? DjTheme.panel : ring))
                .offset(x: size * 0.1, y: size * 0.08)
        }
    }

    static func tint(_ name: String) -> Color {
        var hash: UInt64 = 0xcbf2_9ce4_8422_2325
        for byte in name.utf8 { hash = (hash ^ UInt64(byte)) &* 0x0000_0100_0000_01b3 }
        return Color(hue: Double(hash % 360) / 360, saturation: 0.55, brightness: 0.78)
    }
}

struct DjReactButton: View {
    let model: DjStudioModel
    let size: CGFloat

    var body: some View {
        DjRoundButton(size: size, lit: model.reacting, action: { model.reacting.toggle() }) {
            Image(systemName: "face.smiling")
        }
        .accessibilityLabel(model.reacting ? "Hide reactions" : "Reactions")
    }
}

struct DjReactionBar: View {
    let model: DjStudioModel

    var body: some View {
        HStack(spacing: 4) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 2) {
                    ForEach(DjReaction.all) { (reaction: DjReaction) in
                        Button {
                            DjTheme.tick()
                            model.react(reaction)
                        } label: {
                            Text(reaction.emoji)
                                .font(.system(size: 26))
                                .frame(width: 44, height: 44)
                                .contentShape(Rectangle())
                        }
                        .buttonStyle(DjPressStyle())
                        .accessibilityLabel(reaction.name)
                    }
                }
                .padding(.horizontal, 6)
            }
            Button {
                model.reacting = false
            } label: {
                Image(systemName: "xmark")
                    .font(.system(size: 13, weight: .bold))
                    .foregroundStyle(DjTheme.muted)
                    .frame(width: 40, height: 44)
                    .contentShape(Rectangle())
            }
            .buttonStyle(DjPressStyle())
            .accessibilityLabel("Hide reactions")
        }
        .frame(maxWidth: 560)
        .background(DjTheme.panel.opacity(0.94), in: Capsule())
        .overlay(Capsule().strokeBorder(DjTheme.edge, lineWidth: 1))
        .shadow(color: .black.opacity(0.5), radius: 8, y: 3)
        .padding(.horizontal, 12)
    }
}

struct DjReactionLayer: View {
    let model: DjStudioModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        let effects = model.effects
        let emojis = Array(Set(effects.map(\.reaction.emoji))).sorted()
        TimelineView(.animation(paused: effects.isEmpty)) { timeline in
            Canvas { context, size in
                for effect in effects {
                    DjEffectDrawing.draw(effect, context, size, now: timeline.date, reduced: reduceMotion)
                }
            } symbols: {
                ForEach(emojis, id: \.self) { emoji in
                    Text(emoji).font(.system(size: DjEffectDrawing.base)).tag(emoji)
                }
            }
        }
        .ignoresSafeArea()
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

enum DjEffectDrawing {
    static let base: CGFloat = 64

    static func draw(_ effect: DjEffect, _ context: GraphicsContext, _ size: CGSize, now: Date, reduced: Bool) {
        let reaction = effect.reaction
        let t = now.timeIntervalSince(effect.began) / reaction.seconds
        guard t >= 0, t <= 1, let symbol = context.resolveSymbol(id: reaction.emoji) else { return }
        let w = Double(size.width)
        let h = Double(size.height)
        let m = min(w, h)
        let centre = CGPoint(x: w / 2, y: h / 2)
        let r = { (index: Int, salt: Int) -> Double in random(effect.seed, index, salt) }
        let put = { (point: CGPoint, points: Double, angle: Double, opacity: Double) in
            guard opacity > 0.01 else { return }
            var layer = context
            layer.opacity = min(1, opacity)
            layer.translateBy(x: point.x, y: point.y)
            if angle != 0 { layer.rotate(by: .radians(angle)) }
            let scale = CGFloat(points) / base
            layer.scaleBy(x: scale, y: scale)
            layer.draw(symbol, at: .zero)
        }
        defer { label(effect, context, size, t) }
        if reduced {
            put(centre, m * 0.3, 0, t < 0.2 ? t / 0.2 : (1 - t) / 0.8)
            return
        }
        switch reaction.style {
        case .rise, .rain:
            let falling = reaction.style == .rain
            for index in 0..<16 {
                let delay = r(index, 0) * 0.35
                let p = (t - delay) / (1 - delay)
                guard p > 0 else { continue }
                let sway: Double = sin(p * 6 + r(index, 2) * 6) * m * 0.04
                let x: Double = r(index, 1) * w + sway
                let y: Double = falling ? -0.05 * h + p * 1.15 * h : 1.05 * h - p * 1.15 * h
                let fade: Double = p > 0.75 ? (1 - p) / 0.25 : min(1, p * 6)
                put(CGPoint(x: x, y: y), m * (0.08 + r(index, 3) * 0.07), falling ? (r(index, 4) - 0.5) * p * 2 : 0, fade)
            }
        case .burst:
            let p: Double = 1 - pow(1 - t, 3)
            for index in 0..<16 {
                let angle: Double = Double(index) / 16 * 2 * Double.pi + r(index, 0) * 0.3
                let distance: Double = p * m * (0.22 + r(index, 1) * 0.28)
                let point = CGPoint(x: w / 2 + cos(angle) * distance, y: h / 2 + sin(angle) * distance)
                let points: Double = m * (0.08 + r(index, 2) * 0.06) * (0.6 + 0.4 * p)
                put(point, points, (r(index, 3) - 0.5) * t * 3, 1 - t * t)
            }
        case .zoom:
            put(centre, m * 0.42 * pop(t), sin(t * 12) * 0.08 * (1 - t), t > 0.7 ? (1 - t) / 0.3 : 1)
        case .spin:
            var ring = context
            ring.opacity = (1 - t) * 0.6
            let radius: Double = m * 0.3 * (0.5 + t)
            ring.stroke(Path(ellipseIn: CGRect(x: w / 2 - radius, y: h / 2 - radius, width: radius * 2, height: radius * 2)),
                        with: .color(Color(hue: reaction.hue, saturation: 0.8, brightness: 1)), lineWidth: CGFloat(m * 0.02))
            put(centre, m * 0.36 * (0.4 + 0.6 * min(1, t * 3)), t * 4 * Double.pi, t > 0.75 ? (1 - t) / 0.25 : 1)
        case .orbit:
            let radius: Double = m * 0.28 * (0.6 + 0.4 * min(1, t * 2))
            let fade: Double = t < 0.15 ? t / 0.15 : t > 0.8 ? (1 - t) / 0.2 : 1
            for index in 0..<8 {
                let angle: Double = Double(index) / 8 * 2 * Double.pi + t * 2 * Double.pi * 1.2
                put(CGPoint(x: w / 2 + cos(angle) * radius, y: h / 2 + sin(angle) * radius), m * 0.1, 0, fade)
            }
        case .wave:
            for index in 0..<7 {
                let phase: Double = sin(t * 10 + Double(index))
                let x: Double = (t * 1.5 - Double(index) * 0.12 - 0.1) * w
                let y: Double = h * 0.5 + phase * m * 0.08
                put(CGPoint(x: x, y: y), m * 0.12, phase * 0.25, 1)
            }
        case .confetti:
            for index in 0..<70 {
                let delay = r(index, 0) * 0.3
                let p = (t - delay) / (1 - delay)
                guard p > 0 else { continue }
                let sway: Double = sin(p * 5 + r(index, 2) * 6) * m * 0.03
                let x: Double = r(index, 1) * w + sway
                let y: Double = -0.05 * h + p * (1 + r(index, 3) * 0.3) * h
                var piece = context
                piece.opacity = p > 0.85 ? (1 - p) / 0.15 : 1
                piece.translateBy(x: CGFloat(x), y: CGFloat(y))
                piece.rotate(by: .radians(p * 6 * Double.pi * r(index, 4)))
                piece.fill(Path(CGRect(x: -m * 0.009, y: -m * 0.015, width: m * 0.018, height: m * 0.03)),
                           with: .color(Color(hue: r(index, 5), saturation: 0.8, brightness: 1)))
            }
            put(centre, m * 0.3 * pop(min(1, t * 2.5)), 0, t < 0.45 ? 1 : max(0, (0.6 - t) / 0.15))
        case .fireworks:
            for burst in 0..<4 {
                let p: Double = (t - Double(burst) * 0.18) / 0.6
                guard p > 0, p < 1 else { continue }
                let ox: Double = w * (0.2 + 0.6 * r(burst, 10))
                let oy: Double = h * (0.2 + 0.4 * r(burst, 11))
                var dots = context
                dots.opacity = 1 - p
                let spread: Double = (1 - pow(1 - p, 3)) * m * 0.22
                let dot: Double = m * 0.009
                for index in 0..<22 {
                    let angle: Double = Double(index) / 22 * 2 * Double.pi
                    let x: Double = ox + cos(angle) * spread
                    let y: Double = oy + sin(angle) * spread + p * p * m * 0.08
                    let hue: Double = (reaction.hue + r(burst * 31 + index, 12) * 0.25).truncatingRemainder(dividingBy: 1)
                    dots.fill(Path(ellipseIn: CGRect(x: x - dot, y: y - dot, width: dot * 2, height: dot * 2)),
                              with: .color(Color(hue: hue, saturation: 0.7, brightness: 1)))
                }
            }
            let fade: Double = t < 0.5 ? 1 : max(0, (0.65 - t) / 0.15)
            put(CGPoint(x: w / 2, y: h * (0.9 - 0.5 * min(1, t * 2))), m * 0.16, 0, fade)
        case .flash:
            var glow = context
            glow.opacity = 0.26 * pow(max(0, sin(t * Double.pi * 3)), 2)
            glow.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Color(hue: reaction.hue, saturation: 0.85, brightness: 1)))
            put(centre, m * 0.42 * pop(t), 0, t > 0.7 ? (1 - t) / 0.3 : 1)
        }
    }

    private static func label(_ effect: DjEffect, _ context: GraphicsContext, _ size: CGSize, _ t: Double) {
        guard !effect.sender.isEmpty, t < 0.9 else { return }
        var layer = context
        layer.opacity = t > 0.7 ? (0.9 - t) / 0.2 : min(1, t * 8)
        var text = layer.resolve(Text("\(effect.sender) \(effect.reaction.emoji)").font(.system(size: 13, weight: .semibold)))
        text.shading = .color(.white)
        let measured = text.measure(in: size)
        let point = CGPoint(x: size.width / 2, y: size.height * 0.16)
        let box = CGRect(x: point.x - measured.width / 2 - 10, y: point.y - measured.height / 2 - 5, width: measured.width + 20, height: measured.height + 10)
        layer.fill(Path(roundedRect: box, cornerRadius: box.height / 2), with: .color(.black.opacity(0.6)))
        layer.draw(text, at: point)
    }

    private static func pop(_ t: Double) -> Double {
        guard t < 0.25 else { return 1 + 0.06 * sin((t - 0.25) * 20) * (1 - t) }
        let p = t / 0.25 - 1
        return 1 + 2.7 * p * p * p + 1.7 * p * p
    }

    private static func random(_ seed: UInt64, _ index: Int, _ salt: Int) -> Double {
        var z = seed &+ UInt64(truncatingIfNeeded: index) &* 0x9E37_79B9_7F4A_7C15 &+ UInt64(truncatingIfNeeded: salt) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 30)) &* 0xBF58_476D_1CE4_E5B9
        z = (z ^ (z >> 27)) &* 0x94D0_49BB_1331_11EB
        z ^= z >> 31
        return Double(z >> 11) / Double(1 << 53)
    }
}
