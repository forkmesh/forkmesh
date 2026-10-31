import FMChat
import Observation
import SwiftUI
#if canImport(WebRTC)
import WebRTC
#endif

@MainActor
@Observable
final class DirectCallPresentation {
    static let shared = DirectCallPresentation()

    var minimized = false
    var returning = false
    var fullShown = false {
        didSet { if fullShown { returning = false } }
    }

    private init() {
        #if canImport(WebRTC)
        NativeQualityProbe.levelsShown = {
            !DirectCallPresentation.shared.minimized && AppLifecycle.shared.state == .resumed
        }
        #endif
    }

    func minimize() {
        withAnimation(.spring(response: 0.35, dampingFraction: 0.85)) { minimized = true }
    }

    func expand() {
        returning = true
        minimized = false
    }

    var hostsPage: Bool { minimized || returning }
}

enum CallMiniSide: String {
    case leading
    case trailing
}

struct DirectCallMiniWindow: View {
    let engine: DirectCallEngine
    let factory: WebCallPeerFactory?

    static let margin: CGFloat = 12
    static let videoTile = CGSize(width: 116, height: 164)
    static let voiceTile = CGSize(width: 156, height: 96)
    static let snapCorner: CGFloat = 0.12

    @AppStorage("fm.call.miniSide") private var side = CallMiniSide.trailing.rawValue
    @AppStorage("fm.call.miniY") private var fraction = 0.18
    @State private var drag: CGSize = .zero
    @State private var dragging = false

    private var presentation: DirectCallPresentation { DirectCallPresentation.shared }

    private var video: Bool { engine.media == .video }

    private var tile: CGSize { video ? Self.videoTile : Self.voiceTile }

    private var remoteVideoShowing: Bool {
        if let factory, factory.current != nil { return factory.remoteVideo }
        #if canImport(WebRTC)
        if let native = engine.peerFactory as? NativeCallPeerFactory { return native.remoteVideoActive }
        #endif
        return false
    }

    var body: some View {
        GeometryReader { (proxy: GeometryProxy) in
            let spot = Self.center(side: CallMiniSide(rawValue: side) ?? .trailing, fraction: fraction, tile: tile, in: proxy.size)
            content
                .frame(width: tile.width, height: tile.height)
                .clipShape(RoundedRectangle(cornerRadius: 14))
                .overlay(RoundedRectangle(cornerRadius: 14).stroke(Color.white.opacity(0.3), lineWidth: 1))
                .shadow(color: .black.opacity(0.45), radius: 10, y: 3)
                .scaleEffect(dragging ? 1.04 : 1)
                .position(x: spot.x + drag.width, y: spot.y + drag.height)
                .gesture(
                    DragGesture(minimumDistance: 6)
                        .onChanged { (value: DragGesture.Value) in
                            dragging = true
                            drag = value.translation
                        }
                        .onEnded { (value: DragGesture.Value) in
                            let end = CGPoint(
                                x: spot.x + value.predictedEndTranslation.width,
                                y: spot.y + value.predictedEndTranslation.height)
                            let next = Self.snap(end, tile: tile, in: proxy.size)
                            withAnimation(.spring(response: 0.35, dampingFraction: 0.8)) {
                                side = next.side.rawValue
                                fraction = next.fraction
                                drag = .zero
                                dragging = false
                            }
                        }
                )
        }
        .transition(.scale(scale: 0.6).combined(with: .opacity))
    }

    private var content: some View {
        ZStack {
            Color.black
            if DirectCallPresentation.shared.hostsPage, let peer = factory?.current {
                WebCallSurface(webView: peer.webView)
                    .opacity(video && remoteVideoShowing ? 1 : 0.01)
                    .allowsHitTesting(false)
            }
            #if canImport(WebRTC)
            if video, let native = engine.peerFactory as? NativeCallPeerFactory {
                NativeMiniVideo(factory: native)
                    .allowsHitTesting(false)
            }
            #endif
            if !video || !remoteVideoShowing {
                LinearGradient(colors: [Color(white: 0.2), Color(white: 0.08)], startPoint: .top, endPoint: .bottom)
                VStack(spacing: 4) {
                    FmAvatar(name: engine.partner, size: video ? 44 : 34, fontSize: video ? 17 : 14)
                    if video {
                        Text("@\(engine.partner)")
                            .font(.system(size: 12, weight: .semibold))
                            .lineLimit(1)
                            .minimumScaleFactor(0.7)
                    }
                    status
                }
                .foregroundStyle(.white)
                .padding(.horizontal, 6)
                .padding(.bottom, video ? 30 : 34)
                .allowsHitTesting(false)
            }
            VStack {
                Spacer()
                HStack(spacing: 10) {
                    small(engine.muted ? "mic.slash.fill" : "mic.fill", fill: engine.muted ? Color.white : Color.black.opacity(0.55),
                          tint: engine.muted ? Color.black : Color.white, label: engine.muted ? "Unmute" : "Mute") {
                        engine.toggleMute()
                    }
                    .accessibilityIdentifier("chat-call-mini-mute")
                    small("phone.down.fill", fill: Color.red, tint: Color.white, label: "Hang up") {
                        Task { await engine.hangUp() }
                    }
                    .accessibilityIdentifier("chat-call-mini-hangup")
                }
                .padding(.bottom, 6)
            }
        }
        .contentShape(Rectangle())
        .onTapGesture { presentation.expand() }
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Call with @\(engine.partner), \(engine.stateLine)")
        .accessibilityHint("Double-tap to return to the call. Drag to move it.")
        .accessibilityAction(named: "Return to call") { presentation.expand() }
        .accessibilityIdentifier("chat-call-mini")
    }

    @ViewBuilder private var status: some View {
        if engine.phase == .live, !engine.reconnecting, let since = engine.liveSince {
            TimelineView(.periodic(from: since, by: 1)) { (context: TimelineViewDefaultContext) in
                Text(Self.duration(context.date.timeIntervalSince(since)))
                    .font(.system(size: 12, weight: .medium).monospacedDigit())
                    .opacity(0.85)
            }
        } else {
            Text(engine.reconnecting ? "Reconnecting…" : engine.phase == .ringing ? "Calling…" : "Connecting…")
                .font(.system(size: 11, weight: .medium))
                .lineLimit(1)
                .opacity(0.85)
        }
    }

    private func small(
        _ symbol: String, fill: Color, tint: Color, label: String, action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 13, weight: .semibold))
                .foregroundStyle(tint)
                .frame(width: 30, height: 30)
                .background(Circle().fill(fill))
                .frame(width: 40, height: 40)
                .contentShape(Circle())
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }

    static func duration(_ seconds: TimeInterval) -> String {
        let total = max(0, Int(seconds))
        let hours = total / 3600
        let minutes = (total % 3600) / 60
        let rest = total % 60
        if hours > 0 { return String(format: "%d:%02d:%02d", hours, minutes, rest) }
        return String(format: "%d:%02d", minutes, rest)
    }

    static func range(tile: CGSize, in size: CGSize) -> (left: CGFloat, right: CGFloat, top: CGFloat, bottom: CGFloat) {
        let left = margin + tile.width / 2
        let right = max(left, size.width - margin - tile.width / 2)
        let top = margin + tile.height / 2
        let bottom = max(top, size.height - margin - tile.height / 2)
        return (left, right, top, bottom)
    }

    static func center(side: CallMiniSide, fraction: Double, tile: CGSize, in size: CGSize) -> CGPoint {
        let box = range(tile: tile, in: size)
        let y = box.top + CGFloat(min(1, max(0, fraction))) * (box.bottom - box.top)
        return CGPoint(x: side == .leading ? box.left : box.right, y: y)
    }

    static func snap(_ point: CGPoint, tile: CGSize, in size: CGSize) -> (side: CallMiniSide, fraction: Double) {
        let box = range(tile: tile, in: size)
        let side: CallMiniSide = point.x < (box.left + box.right) / 2 ? .leading : .trailing
        let span = box.bottom - box.top
        var fraction = span > 0 ? Double((point.y - box.top) / span) : 0
        fraction = min(1, max(0, fraction))
        if fraction < snapCorner { fraction = 0 }
        if fraction > 1 - snapCorner { fraction = 1 }
        return (side, fraction)
    }
}
