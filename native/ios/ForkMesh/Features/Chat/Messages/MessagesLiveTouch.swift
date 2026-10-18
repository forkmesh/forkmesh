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
    static let rate: Double = 30
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
    @State private var pickingColor: Bool = false

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
            VStack(spacing: FmSpace.x2) {
                VStack(alignment: .leading, spacing: FmSpace.x2) {
                    toolbar
                    if pickingColor {
                        colorDropDown
                            .transition(.move(edge: .top).combined(with: .opacity))
                    }
                }
                .padding(.top, 62)
                .padding(.horizontal, FmSpace.x3)
                if !pickingColor {
                    Text("Tap to buzz. Draw on the background.")
                        .font(.system(size: 13, weight: .medium))
                        .foregroundStyle(fm.textPrimary)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 8)
                        .chatGlass(Capsule(), border: fm.border)
                        .allowsHitTesting(false)
                }
                Spacer()
            }
        }
        .accessibilityIdentifier("chat-live-touch")
    }

    private var toolbar: some View {
        return HStack(spacing: 6) {
            Button(action: {
                withAnimation(.easeOut(duration: 0.18)) { pickingColor.toggle() }
                FmHaptics.selection()
            }) {
                HStack(spacing: 4) {
                    swatch(color, selected: false)
                    Image(systemName: "chevron.down")
                        .font(.system(size: 11, weight: .bold))
                        .foregroundStyle(fm.textPrimary)
                        .rotationEffect(.degrees(pickingColor ? 180 : 0))
                }
                .padding(.horizontal, 8)
                .frame(height: 44)
                .contentShape(Rectangle())
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Ink colour")
            .accessibilityValue(DirectLiveTouchNames.color(color))
            .accessibilityHint(pickingColor ? "Hides the colours." : "Shows the colours.")
            .accessibilityIdentifier("chat-live-color-picker")
            toolButton("trash", label: "Clear drawing", on: false) {
                engine.clearInk(conversation)
                FmHaptics.tap()
            }
            toolButton("checkmark", label: "Done", on: false, action: onDone)
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 2)
        .chatGlass(Capsule(), border: fm.border)
    }

    private var colorDropDown: some View {
        LazyVGrid(columns: Array(repeating: GridItem(.fixed(44), spacing: 4), count: 4), spacing: 4) {
            ForEach(DirectLiveFrames.colors, id: \.self) { (rgb: UInt32) in
                Button(action: {
                    color = rgb
                    FmHaptics.selection()
                    withAnimation(.easeOut(duration: 0.18)) { pickingColor = false }
                }) {
                    swatch(rgb, selected: color == rgb)
                        .frame(width: 44, height: 44)
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Ink colour \(DirectLiveTouchNames.color(rgb))")
                .accessibilityAddTraits(color == rgb ? .isSelected : [])
            }
        }
        .padding(6)
        .chatGlass(RoundedRectangle(cornerRadius: 22, style: .continuous), border: fm.border)
    }

    private func swatch(_ rgb: UInt32, selected: Bool) -> some View {
        Circle()
            .fill(DirectLiveGeometry.color(rgb))
            .frame(width: 24, height: 24)
            .overlay(Circle().stroke(Color.gray.opacity(0.5), lineWidth: 0.5))
            .padding(3)
            .overlay(Circle().stroke(fm.textPrimary, lineWidth: selected ? 2 : 0))
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
            if pickingColor { withAnimation(.easeOut(duration: 0.18)) { pickingColor = false } }
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

enum DirectLivePhone {
    static let floatingFaceKey: String = "chat/live/floatingFace"
    static let rim: Color = Color(fmARGB: 0xFFFF375F)
}

struct MessagesLivePartnerPhone<Avatar: View>: View {
    @Environment(\.fmPalette) private var fm
    @AppStorage(DirectLivePhone.floatingFaceKey) private var floatingFace: Bool = false
    let engine: DirectLiveEngine
    let conversation: String
    let name: String
    let dark: Bool
    @ViewBuilder let avatar: () -> Avatar

    var body: some View {
        let sharing: Bool = engine.partnerMotion(conversation) != nil
        Menu {
            Toggle(isOn: $floatingFace) {
                Label("Floating 3D Face", systemImage: "person.and.background.dotted")
            }
        } label: {
            Group {
                if sharing || floatingFace {
                    TimelineView(.animation(minimumInterval: sharing ? 1.0 / 60.0 : 1.0 / 30.0)) { (context: TimelineViewDefaultContext) in
                        phone(
                            sharing ? engine.motion[conversation]?.sample(at: context.date.epochMillis) : nil,
                            time: context.date.timeIntervalSinceReferenceDate)
                    }
                } else {
                    phone(nil, time: 0)
                }
            }
            .frame(width: 44, height: 44)
            .chatGlass(Circle(), border: fm.border)
        }
        .menuStyle(.button)
        .buttonStyle(.plain)
        .animation(.spring(response: 0.35, dampingFraction: 0.75), value: floatingFace)
        .accessibilityLabel("\(name)'s phone")
        .accessibilityValue(
            [sharing ? "Showing its direction" : "Not sharing its direction", floatingFace ? "floating face on" : ""]
                .filter { !$0.isEmpty }.joined(separator: ", "))
        .accessibilityHint("Turns the floating 3D face on or off.")
        .accessibilityAction(named: "Send a tap") {
            engine.tap(conversation, at: DirectLivePoint(x: 0.5, y: 0))
            DirectLiveHaptics.sent()
        }
        .accessibilityIdentifier(sharing ? "chat-live-partner-motion" : "chat-live-partner-still")
    }

    private func phone(_ sample: DirectLiveMotionSample?, time: Double) -> some View {
        let roll: Double = sample?.steadyRoll ?? 0
        let pitch: Double = sample?.pitch ?? 0
        let jolt: Double = min(sample?.jolt ?? 0, 2)
        let ink: Color = dark ? Color.white : Color.black.opacity(0.75)
        let rim: Color = sample == nil ? ink.opacity(0.6) : DirectLivePhone.rim.opacity(min(1, 0.6 + jolt * 0.4))
        let bob: CGFloat = CGFloat(sin(time * 2.4))
        let tilt: CGSize = CGSize(width: CGFloat(sin(roll)) * 7, height: CGFloat(sin(pitch)) * 4)
        return ZStack {
            RoundedRectangle(cornerRadius: 5, style: .continuous)
                .fill(Color.black.opacity(dark ? 0.55 : 0.1))
            RoundedRectangle(cornerRadius: 5, style: .continuous)
                .stroke(rim, lineWidth: 1.5)
            Capsule()
                .fill(rim)
                .frame(width: 6, height: 1.6)
                .frame(maxHeight: .infinity, alignment: .top)
                .padding(.top, 3)
            if floatingFace {
                Ellipse()
                    .fill(Color.black.opacity(0.3))
                    .frame(width: 12 - bob * 1.5, height: 4)
                    .blur(radius: 1.2)
                    .offset(y: 7)
            } else {
                avatar().frame(width: 15, height: 15)
            }
        }
        .frame(width: 20, height: 32)
        .rotation3DEffect(.radians(-pitch * 0.6), axis: (x: 1, y: 0, z: 0), perspective: 0.5)
        .rotationEffect(.radians(roll))
        .overlay {
            if floatingFace {
                avatar()
                    .frame(width: 22, height: 22)
                    .shadow(color: Color.black.opacity(0.35), radius: 2.5, y: 3 + bob)
                    .rotation3DEffect(.radians(roll * 0.6), axis: (x: 0, y: 1, z: 0), perspective: 0.6)
                    .rotation3DEffect(.radians(-pitch * 0.35), axis: (x: 1, y: 0, z: 0), perspective: 0.6)
                    .scaleEffect(1 + CGFloat(jolt) * 0.08)
                    .offset(x: tilt.width, y: -5 - bob * 2 + tilt.height)
                    .transition(.scale(scale: 0.4).combined(with: .opacity))
                    .allowsHitTesting(false)
            }
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
                    .frame(width: 44, height: 44)
                    .background(Circle().fill(drawing ? fm.accent : Color.clear))
                    .chatGlass(Circle(), border: fm.border)
            }
            .buttonStyle(.plain)
            .accessibilityLabel(drawing ? "Stop drawing" : "Draw")
            .accessibilityIdentifier("chat-live-touch-button")
        }
    }
}
