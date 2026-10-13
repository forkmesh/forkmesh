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
        guard FmHaptics.enabled else { return }
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
        guard FmHaptics.enabled else { return }
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
        engine.addMotion(
            DirectLiveMotionSample(
                at: engine.now, gx: gravity.x, gy: gravity.y, gz: gravity.z, ax: push.x, ay: push.y, az: push.z))
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
    let partner: String
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
                    .accessibilityHint("Tap to make \(partner)'s phone tap back. Drag to draw on the background.")
                    .accessibilityAddTraits(.allowsDirectInteraction)
                    .accessibilityAction(named: "Send a tap") {
                        engine.tap(conversation, at: DirectLivePoint(x: 0.5, y: 0))
                    }
            }
            .ignoresSafeArea()
            VStack {
                Text("Tap to buzz \(partner). Draw on the background.")
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
        let sharing: Bool = engine.isSharingMotion(conversation)
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
            toolButton(
                sharing ? "figure.walk.motion" : "iphone.gen3.radiowaves.left.and.right",
                label: sharing ? "Stop sharing movement" : "Share my movement", on: sharing
            ) {
                let next: Bool = !sharing
                engine.setSharingMotion(conversation, next)
                if next { DirectLiveMotionFeed.shared.start(engine) }
                FmHaptics.selection()
            }
            .disabled(!DirectLiveMotionFeed.shared.available)
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

struct MessagesLivePartnerMotion: View {
    @Environment(\.fmPalette) private var fm
    let engine: DirectLiveEngine
    let conversation: String
    let partner: String

    var body: some View {
        if engine.partnerMotion(conversation) != nil {
            TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { (context: TimelineViewDefaultContext) in
                let sample: DirectLiveMotionSample? = engine.motion[conversation]?.sample(at: context.date.epochMillis)
                card(sample)
            }
            .transition(.scale(scale: 0.8).combined(with: .opacity))
            .accessibilityElement(children: .ignore)
            .accessibilityLabel("\(partner) is sharing their movement")
            .accessibilityIdentifier("chat-live-partner-motion")
        }
    }

    private func card(_ sample: DirectLiveMotionSample?) -> some View {
        let roll: Double = sample?.steadyRoll ?? 0
        let pitch: Double = (sample?.pitch ?? 0) * 0.6
        let jolt: Double = min(sample?.jolt ?? 0, 2)
        let shake: CGSize = CGSize(width: CGFloat(sample?.ax ?? 0) * 10, height: CGFloat(-(sample?.ay ?? 0)) * 10)
        return VStack(spacing: 6) {
            ZStack {
                Circle()
                    .stroke(Color(fmARGB: 0xFFFF375F).opacity(min(0.9, jolt * 0.8)), lineWidth: 2)
                    .frame(width: 58 + CGFloat(jolt) * 18, height: 58 + CGFloat(jolt) * 18)
                RoundedRectangle(cornerRadius: 7, style: .continuous)
                    .fill(
                        LinearGradient(
                            colors: [Color(fmARGB: 0xFF64D2FF), Color(fmARGB: 0xFFBF5AF2)], startPoint: .top,
                            endPoint: .bottom)
                    )
                    .overlay(
                        RoundedRectangle(cornerRadius: 7, style: .continuous).stroke(Color.white.opacity(0.9), lineWidth: 2)
                    )
                    .overlay(alignment: .top) {
                        Capsule().fill(Color.black.opacity(0.6)).frame(width: 10, height: 3).padding(.top, 4)
                    }
                    .frame(width: 30, height: 56)
                    .rotation3DEffect(.radians(pitch), axis: (x: 1, y: 0, z: 0), perspective: 0.5)
                    .rotationEffect(.radians(roll))
                    .offset(shake)
            }
            .frame(width: 84, height: 84)
            Text(partner)
                .font(.system(size: 11, weight: .semibold))
                .foregroundStyle(fm.textPrimary)
                .lineLimit(1)
        }
        .padding(8)
        .frame(width: 100)
        .chatGlass(RoundedRectangle(cornerRadius: 20, style: .continuous), border: fm.border)
    }
}

struct DirectLiveTouchButton: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let dark: Bool
    @Binding var touching: Bool
    @Binding var focused: Bool

    var body: some View {
        if let engine = hub.live, hub.inDirect {
            let on: Bool = touching || engine.isSharingMotion(hub.relay.currentConversation)
            Button(action: {
                focused = false
                withAnimation(.easeOut(duration: 0.2)) { touching.toggle() }
                FmHaptics.selection()
            }) {
                Image(systemName: "hand.tap.fill")
                    .font(.system(size: 16, weight: .medium))
                    .foregroundStyle(on ? Color.white : (dark ? Color.white : Color.black.opacity(0.7)))
                    .frame(width: 38, height: 38)
                    .background(Circle().fill(on ? fm.accent : Color.clear))
                    .chatGlass(Circle(), border: fm.border)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(touching ? "Close touch" : "Touch: tap, draw and share movement")
            .accessibilityIdentifier("chat-live-touch-button")
        }
    }
}
