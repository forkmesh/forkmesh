import CoreMotion
import FMBase
import FMChat
import SwiftUI
import UIKit

enum DirectLiveHaptics {
    @MainActor private static let knock: UIImpactFeedbackGenerator = UIImpactFeedbackGenerator(style: .heavy)
    @MainActor private static let echo: UIImpactFeedbackGenerator = UIImpactFeedbackGenerator(style: .rigid)
    @MainActor private static let brush: UIImpactFeedbackGenerator = UIImpactFeedbackGenerator(style: .soft)

    @MainActor static func tapped() {
        knock.impactOccurred(intensity: 1)
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 90_000_000)
            echo.impactOccurred(intensity: 0.55)
            knock.prepare()
        }
    }

    @MainActor static func drawn() {
        guard FmHaptics.enabled else { return }
        brush.impactOccurred(intensity: 0.6)
    }

    @MainActor static func sent() {
        brush.impactOccurred(intensity: 0.45)
    }
}

@MainActor
final class DirectLiveMotionFeed {
    static let shared: DirectLiveMotionFeed = DirectLiveMotionFeed()
    static let rate: Double = 20
    static let smoothing: Double = 0.1

    private let manager: CMMotionManager = CMMotionManager()
    private weak var engine: DirectLiveEngine?
    private var resting: CMAcceleration?
    private var lastTap: Int64 = 0

    var available: Bool { manager.isDeviceMotionAvailable || manager.isAccelerometerAvailable }

    func start(_ engine: DirectLiveEngine) {
        self.engine = engine
        if manager.isDeviceMotionActive || manager.isAccelerometerActive { return }
        if manager.isDeviceMotionAvailable {
            manager.deviceMotionUpdateInterval = 1.0 / DirectLiveMotionFeed.rate
            manager.startDeviceMotionUpdates(to: .main) { [weak self] (motion: CMDeviceMotion?, _: Error?) in
                guard let motion = motion else { return }
                let gravity: CMAcceleration = motion.gravity
                let push: CMAcceleration = motion.userAcceleration
                MainActor.assumeIsolated { self?.feed(gravity: gravity, push: push) }
            }
        } else if manager.isAccelerometerAvailable {
            resting = nil
            manager.accelerometerUpdateInterval = 1.0 / DirectLiveMotionFeed.rate
            manager.startAccelerometerUpdates(to: .main) { [weak self] (data: CMAccelerometerData?, _: Error?) in
                guard let reading = data?.acceleration else { return }
                MainActor.assumeIsolated { self?.feelRaw(reading) }
            }
        }
    }

    func stop() {
        if manager.isDeviceMotionActive { manager.stopDeviceMotionUpdates() }
        if manager.isAccelerometerActive { manager.stopAccelerometerUpdates() }
        resting = nil
        lastTap = 0
        engine = nil
    }

    private func feelRaw(_ reading: CMAcceleration) {
        var rest: CMAcceleration = resting ?? reading
        let k: Double = DirectLiveMotionFeed.smoothing
        rest.x += (reading.x - rest.x) * k
        rest.y += (reading.y - rest.y) * k
        rest.z += (reading.z - rest.z) * k
        resting = rest
        feed(gravity: rest, push: CMAcceleration(x: reading.x - rest.x, y: reading.y - rest.y, z: reading.z - rest.z))
    }

    private func feed(gravity: CMAcceleration, push: CMAcceleration) {
        guard let engine = engine, !engine.sharingMotion.isEmpty else {
            stop()
            return
        }
        sendTap(engine, push: push)
        engine.addMotion(
            DirectLiveMotionSample(
                at: engine.now, gx: gravity.x, gy: gravity.y, gz: gravity.z, ax: push.x, ay: push.y, az: push.z))
    }

    private func sendTap(_ engine: DirectLiveEngine, push: CMAcceleration) {
        let impact: Double = sqrt(push.x * push.x + push.y * push.y + push.z * push.z)
        let now: Int64 = engine.now
        guard impact >= 1.4, now - lastTap >= 500 else { return }
        lastTap = now
        for conversation in engine.sharingMotion {
            engine.tap(conversation, at: DirectLivePoint(x: 0.5, y: 0))
        }
        DirectLiveHaptics.sent()
    }
}

enum DirectLiveGeometry {
    static func point(_ location: CGPoint, in size: CGSize) -> DirectLivePoint {
        let width: CGFloat = max(size.width, 1)
        return DirectLivePoint(x: Double(location.x / width), y: Double((location.y - size.height / 2) / width))
    }

    static func location(_ point: DirectLivePoint, in size: CGSize) -> CGPoint {
        CGPoint(x: CGFloat(point.x) * size.width, y: size.height / 2 + CGFloat(point.y) * size.width)
    }

    static func color(_ rgb: UInt32) -> Color { Color(fmARGB: 0xFF00_0000 | (rgb & 0xFFFFFF)) }
}

struct MessagesLiveInkLayer: View {
    let engine: DirectLiveEngine
    let conversation: String

    var body: some View {
        let strokes: [DirectLiveStroke] = engine.strokes(in: conversation)
        let taps: [DirectLiveTap] = engine.taps(in: conversation)
        Group {
            if strokes.isEmpty && taps.isEmpty {
                Color.clear
            } else {
                TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { (context: TimelineViewDefaultContext) in
                    Canvas { (canvas: inout GraphicsContext, size: CGSize) in
                        let now: Int64 = context.date.epochMillis
                        for stroke in strokes { draw(stroke, now: now, canvas: &canvas, size: size) }
                        for tap in taps { ripple(tap, now: now, canvas: &canvas, size: size) }
                    }
                }
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func draw(_ stroke: DirectLiveStroke, now: Int64, canvas: inout GraphicsContext, size: CGSize) {
        let opacity: Double = stroke.opacity(at: now)
        guard opacity > 0, let first = stroke.points.first else { return }
        let color: Color = DirectLiveGeometry.color(stroke.color)
        let width: CGFloat = CGFloat(stroke.width)
        if stroke.points.count == 1 {
            let center: CGPoint = DirectLiveGeometry.location(first, in: size)
            let dot: Path = Path(ellipseIn: CGRect(x: center.x - width / 2, y: center.y - width / 2, width: width, height: width))
            canvas.fill(dot, with: .color(color.opacity(opacity)))
            return
        }
        var path: Path = Path()
        path.move(to: DirectLiveGeometry.location(first, in: size))
        var previous: CGPoint = DirectLiveGeometry.location(first, in: size)
        for point in stroke.points.dropFirst() {
            let next: CGPoint = DirectLiveGeometry.location(point, in: size)
            path.addQuadCurve(
                to: CGPoint(x: (previous.x + next.x) / 2, y: (previous.y + next.y) / 2), control: previous)
            previous = next
        }
        path.addLine(to: previous)
        canvas.stroke(
            path, with: .color(color.opacity(opacity * 0.35)),
            style: StrokeStyle(lineWidth: width * 2.6, lineCap: .round, lineJoin: .round))
        canvas.stroke(
            path, with: .color(color.opacity(opacity)), style: StrokeStyle(lineWidth: width, lineCap: .round, lineJoin: .round))
    }

    private func ripple(_ tap: DirectLiveTap, now: Int64, canvas: inout GraphicsContext, size: CGSize) {
        let t: Double = Double(now - tap.at) / Double(DirectLiveFrames.tapLifeMs)
        guard t >= 0, t < 1 else { return }
        let center: CGPoint = DirectLiveGeometry.location(tap.point, in: size)
        let tint: Color = tap.isSelf ? Color.white : Color(fmARGB: 0xFFFF375F)
        for ring in 0..<3 {
            let local: Double = t - Double(ring) * 0.14
            guard local > 0 else { continue }
            let radius: CGFloat = 14 + CGFloat(local) * 120
            let circle: Path = Path(
                ellipseIn: CGRect(x: center.x - radius, y: center.y - radius, width: radius * 2, height: radius * 2))
            canvas.stroke(circle, with: .color(tint.opacity((1 - local) * 0.8)), lineWidth: 3 - CGFloat(ring) * 0.7)
        }
        let core: CGFloat = 16 * CGFloat(1 - t)
        canvas.fill(
            Path(ellipseIn: CGRect(x: center.x - core, y: center.y - core, width: core * 2, height: core * 2)),
            with: .color(tint.opacity(1 - t)))
    }
}

struct MessagesLiveTouchSurface: View {
    @Environment(\.fmPalette) private var fm
    let engine: DirectLiveEngine
    let conversation: String
    let onDone: () -> Void
    @State private var color: UInt32 = DirectLiveFrames.colors[1]
    @State private var strokeId: String = ""
    @State private var touchStart: CGPoint?

    static let tapTravel: CGFloat = 8

    var body: some View {
        ZStack {
            GeometryReader { (geometry: GeometryProxy) in
                Color.white.opacity(0.001)
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture(minimumDistance: 0, coordinateSpace: .local)
                            .onChanged { (value: DragGesture.Value) in changed(value, size: geometry.size) }
                            .onEnded { (value: DragGesture.Value) in ended(value, size: geometry.size) })
                    .accessibilityElement()
                    .accessibilityLabel("Touch canvas")
                    .accessibilityHint("Tap to make their phone tap back. Drag to draw on the background.")
                    .accessibilityAddTraits(.allowsDirectInteraction)
                    .accessibilityAction(named: "Send a tap") {
                        engine.tap(conversation, at: DirectLivePoint(x: 0.5, y: 0))
                        DirectLiveHaptics.sent()
                    }
            }
            VStack {
                Text("Tap to buzz. Draw on the background.")
                    .font(.system(size: 13, weight: .medium))
                    .foregroundStyle(fm.textPrimary)
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .chatGlass(Capsule(), border: fm.border)
                    .padding(.top, 70)
                    .allowsHitTesting(false)
                Spacer()
                toolbar
                    .padding(.horizontal, FmSpace.x3)
                    .padding(.bottom, FmSpace.x4)
            }
        }
        .accessibilityIdentifier("chat-live-touch")
    }

    private var toolbar: some View {
        return HStack(spacing: 10) {
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 8) {
                    ForEach(DirectLiveFrames.colors, id: \.self) { (rgb: UInt32) in
                        Button(action: {
                            color = rgb
                            FmHaptics.selection()
                        }) {
                            Circle()
                                .fill(DirectLiveGeometry.color(rgb))
                                .frame(width: 24, height: 24)
                                .overlay(Circle().stroke(Color.gray.opacity(0.5), lineWidth: 0.5))
                                .padding(3)
                                .overlay(Circle().stroke(fm.textPrimary, lineWidth: color == rgb ? 2 : 0))
                                .frame(width: 44, height: 44)
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("Ink colour \(DirectLiveTouchNames.color(rgb))")
                        .accessibilityAddTraits(color == rgb ? .isSelected : [])
                    }
                }
            }
            toolButton("trash", label: "Clear drawing", on: false) {
                engine.clearInk(conversation)
                FmHaptics.tap()
            }
            toolButton("checkmark", label: "Done", on: false, action: onDone)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 4)
        .chatGlass(RoundedRectangle(cornerRadius: 26, style: .continuous), border: fm.border)
    }

    private func toolButton(_ symbol: String, label: String, on: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 16, weight: .semibold))
                .foregroundStyle(on ? Color.white : fm.textPrimary)
                .frame(width: 44, height: 44)
                .background(Circle().fill(on ? fm.accent : Color.clear))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }

    private func changed(_ value: DragGesture.Value, size: CGSize) {
        guard let start = touchStart else {
            touchStart = value.startLocation
            return
        }
        if strokeId.isEmpty {
            let travel: CGFloat = hypot(value.location.x - start.x, value.location.y - start.y)
            if travel < MessagesLiveTouchSurface.tapTravel { return }
            strokeId = engine.beginStroke(
                conversation, at: DirectLiveGeometry.point(start, in: size), color: color, width: 5)
        }
        engine.extendStroke(conversation, id: strokeId, to: DirectLiveGeometry.point(value.location, in: size))
    }

    private func ended(_ value: DragGesture.Value, size: CGSize) {
        if strokeId.isEmpty {
            engine.tap(conversation, at: DirectLiveGeometry.point(value.location, in: size))
            DirectLiveHaptics.sent()
        } else {
            engine.extendStroke(conversation, id: strokeId, to: DirectLiveGeometry.point(value.location, in: size))
            engine.endStroke(conversation, id: strokeId)
        }
        strokeId = ""
        touchStart = nil
    }
}

enum DirectLiveTouchNames {
    static func color(_ rgb: UInt32) -> String {
        switch rgb {
        case 0xFFFFFF: return "white"
        case 0xFF453A: return "red"
        case 0xFF9F0A: return "orange"
        case 0xFFD60A: return "yellow"
        case 0x32D74B: return "green"
        case 0x64D2FF: return "blue"
        case 0xBF5AF2: return "purple"
        case 0x000000: return "black"
        default: return "custom"
        }
    }
}

struct MessagesLivePartnerAvatar<Avatar: View>: View {
    let engine: DirectLiveEngine
    let conversation: String
    let size: CGFloat
    @ViewBuilder let avatar: () -> Avatar

    var body: some View {
        let sharing: Bool = engine.partnerMotion(conversation) != nil
        Group {
            if sharing {
                TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { (context: TimelineViewDefaultContext) in
                    well(engine.motion[conversation]?.sample(at: context.date.epochMillis))
                }
            } else {
                avatar().frame(width: size, height: size)
            }
        }
        .frame(width: size, height: size)
        .animation(.spring(response: 0.35, dampingFraction: 0.8), value: sharing)
        .accessibilityIdentifier(sharing ? "chat-live-partner-motion" : "chat-live-partner-still")
    }

    private func well(_ sample: DirectLiveMotionSample?) -> some View {
        let roll: Double = sample?.steadyRoll ?? 0
        let pitch: Double = sample?.pitch ?? 0
        let depth: Double = min(max(pitch / (.pi / 2), -1), 1)
        let jolt: Double = min(sample?.jolt ?? 0, 2)
        let inner: CGFloat = size * 0.74
        let reach: CGFloat = (size - inner) / 2 + size * 0.08
        let shift: CGSize = CGSize(
            width: min(max(CGFloat(roll) * reach + CGFloat(sample?.ax ?? 0) * 3, -reach), reach),
            height: min(max(CGFloat(depth) * reach - CGFloat(sample?.ay ?? 0) * 3, -reach), reach))
        return ZStack {
            Circle().fill(Color.primary.opacity(0.08))
            avatar()
                .frame(width: inner, height: inner)
                .rotation3DEffect(.radians(-pitch * 0.6), axis: (x: 1, y: 0, z: 0), perspective: 0.5)
                .rotationEffect(.radians(roll))
                .scaleEffect(1 + CGFloat(depth) * 0.08)
                .offset(shift)
        }
        .frame(width: size, height: size)
        .clipShape(Circle())
        .overlay(Circle().stroke(Color(fmARGB: 0xFFFF375F).opacity(min(0.9, 0.25 + jolt * 0.6)), lineWidth: 1.5))
    }
}

struct DirectLiveTapButton: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let dark: Bool
    @State private var pulse: Int = 0

    var body: some View {
        if let engine = hub.live, hub.inDirect {
            Button(action: {
                engine.tap(hub.relay.currentConversation, at: DirectLivePoint(x: 0.5, y: 0))
                DirectLiveHaptics.sent()
                pulse += 1
            }) {
                Image(systemName: "hand.tap.fill")
                    .font(.system(size: 16, weight: .medium))
                    .foregroundStyle(dark ? Color.white : Color.black.opacity(0.7))
                    .symbolEffect(.bounce, value: pulse)
                    .frame(width: 38, height: 38)
                    .chatGlass(Circle(), border: fm.border)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Send a tap")
            .accessibilityHint("Their phone taps back with a vibration.")
            .accessibilityIdentifier("chat-live-tap-button")
        }
    }
}

struct DirectLiveTouchButton: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let dark: Bool
    @Binding var drawing: Bool
    @Binding var focused: Bool

    var body: some View {
        if hub.live != nil, hub.inDirect {
            Button(action: {
                focused = false
                withAnimation(.easeOut(duration: 0.2)) { drawing.toggle() }
                FmHaptics.selection()
            }) {
                Image(systemName: "pencil.tip")
                    .font(.system(size: 16, weight: .medium))
                    .foregroundStyle(drawing ? Color.white : (dark ? Color.white : Color.black.opacity(0.7)))
                    .frame(width: 38, height: 38)
                    .background(Circle().fill(drawing ? fm.accent : Color.clear))
                    .chatGlass(Circle(), border: fm.border)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(drawing ? "Stop drawing" : "Draw")
            .accessibilityIdentifier("chat-live-touch-button")
        }
    }
}
