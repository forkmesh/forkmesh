import FMChat
import SwiftUI
import UIKit

enum DirectCallActions {
    @MainActor
    static func open(_ media: DirectCallKind, hub: ChatHub) {
        guard let engine = hub.calls, engine.available, !engine.active, engine.invite == nil,
            DirectCallPrecheck.shared.request == nil
        else { return }
        if let factory = engine.peerFactory as? WebCallPeerFactory, factory.busy {
            hub.post("A call is already using the camera and microphone.")
            return
        }
        #if canImport(WebRTC)
        if NativeCallPeerFactory.shared.busy {
            hub.post("A call is already using the camera and microphone.")
            return
        }
        #endif
        let conversation = hub.currentConversation
        let partner = engine.partnerName(for: conversation)
        guard !partner.isEmpty else {
            hub.post("The call could not start. Check that this conversation is connected.")
            return
        }
        Task { @MainActor in
            guard await WebCallPeerFactory.requestAccess(for: media) else {
                hub.post(
                    media == .video
                        ? "Allow ForkMesh to use the camera and microphone in Settings to make video calls."
                        : "Allow ForkMesh to use the microphone in Settings to make calls.")
                return
            }
            guard !engine.active, engine.invite == nil, DirectCallPrecheck.shared.request == nil else { return }
            if let factory = engine.peerFactory as? WebCallPeerFactory, factory.busy { return }
            DirectCallPrecheck.shared.request = DirectCallPrecheckRequest(
                conversation: conversation, partner: partner, media: media)
        }
    }

    @MainActor
    static func start(_ request: DirectCallPrecheckRequest, engine: DirectCallEngine) async -> Bool {
        guard engine.available, !engine.active else { return false }
        guard await WebCallPeerFactory.requestAccess(for: request.media) else { return false }
        return await engine.start(request.conversation, media: request.media)
    }

    @MainActor
    static func accept(_ engine: DirectCallEngine, hub: ChatHub) {
        guard let invite = engine.invite else { return }
        CallRinger.shared.hush()
        Task { @MainActor in
            guard await WebCallPeerFactory.requestAccess(for: invite.media) else {
                await engine.declineInvite()
                hub.post("Allow ForkMesh to use the camera and microphone in Settings to answer calls.")
                return
            }
            await engine.acceptInvite()
        }
    }
}

struct DirectCallHeaderButtons: View {
    @Environment(\.fmPalette) private var fm
    let hub: ChatHub
    let dark: Bool

    var body: some View {
        if let engine = hub.calls, engine.available, hub.inDirect {
            HStack(spacing: 6) {
                button("video.fill", label: "Video call", media: .video, engine: engine)
                button("phone.fill", label: "Voice call", media: .audio, engine: engine)
            }
        }
    }

    private func button(_ symbol: String, label: String, media: DirectCallKind, engine: DirectCallEngine) -> some View {
        Button(action: { DirectCallActions.open(media, hub: hub) }) {
            Image(systemName: symbol)
                .font(.system(size: 15, weight: .semibold))
                .foregroundStyle(dark ? Color.white : Color.black)
                .frame(width: 44, height: 44)
                .chatGlass(Circle(), border: fm.border)
        }
        .buttonStyle(.plain)
        .disabled(engine.active)
        .accessibilityLabel(label)
        .accessibilityIdentifier(media == .video ? "chat-call-video" : "chat-call-audio")
    }
}

struct DirectCallLayer: ViewModifier {
    let hub: ChatHub?
    @State private var handed: ObjectIdentifier?

    private var engine: DirectCallEngine? { hub?.calls }

    private var factory: WebCallPeerFactory? { engine?.peerFactory as? WebCallPeerFactory }

    func body(content: Content) -> some View {
        let shown: Bool = isShown
        return content
            .fullScreenCover(isPresented: shownBinding(shown)) {
                if let hub, let engine = hub.calls {
                    DirectCallCover(engine: engine, hub: hub, factory: engine.peerFactory as? WebCallPeerFactory)
                }
            }
            .onChange(of: engine?.active ?? false) { (_: Bool, now: Bool) in
                activeChanged(now)
            }
    }

    private var isShown: Bool {
        guard let engine else { return false }
        return engine.active || engine.invite != nil || DirectCallPrecheck.shared.request != nil
    }

    private func shownBinding(_ shown: Bool) -> Binding<Bool> {
        Binding(
            get: { shown },
            set: { (now: Bool) in
                guard !now else { return }
                DirectCallPrecheck.shared.request = nil
                if let engine = hub?.calls, engine.active { Task { await engine.hangUp() } }
            })
    }

    private func activeChanged(_ now: Bool) {
        guard let factory else { return }
        if now {
            DirectCallPrecheck.shared.request = nil
            if let current = factory.current, current.previewing {
                handed = ObjectIdentifier(current)
            } else {
                handed = nil
            }
            return
        }
        if let handed, let current = factory.current, current.previewing, ObjectIdentifier(current) == handed {
            factory.endPreview()
        }
        handed = nil
    }
}

struct DirectCallRootLayer: ViewModifier {
    func body(content: Content) -> some View {
        content.modifier(DirectCallLayer(hub: ChatFeature.hub()))
    }
}

extension View {
    func directCallsRoot() -> some View { modifier(DirectCallRootLayer()) }
}

struct DirectCallCover: View {
    let engine: DirectCallEngine
    let hub: ChatHub
    let factory: WebCallPeerFactory?

    private var precheck: DirectCallPrecheck { DirectCallPrecheck.shared }

    private var nativeFactory: AnyObject? {
        #if canImport(WebRTC)
        return engine.peerFactory as? NativeCallPeerFactory
        #else
        return nil
        #endif
    }

    var body: some View {
        if engine.active {
            DirectCallScreen(engine: engine, factory: factory)
        } else if let invite = engine.invite {
            DirectCallIncomingScreen(invite: invite, engine: engine, hub: hub)
        } else if let request = precheck.request, let factory {
            DirectCallPreviewScreen(request: request, engine: engine, factory: factory)
        } else if let request = precheck.request, nativeFactory != nil {
            NativePreCallScreen(request: request, engine: engine)
        } else {
            Color.black.ignoresSafeArea()
        }
    }
}

struct DirectCallIncomingScreen: View {
    let invite: DirectCallInvite
    let engine: DirectCallEngine
    let hub: ChatHub

    var body: some View {
        ZStack {
            LinearGradient(
                colors: [Color(white: 0.16), Color.black], startPoint: .top, endPoint: .bottom
            )
            .ignoresSafeArea()
            VStack(spacing: FmSpace.x3) {
                Spacer()
                FmAvatar(name: invite.partner, size: 120, fontSize: 44)
                Text("@\(invite.partner)")
                    .font(.system(size: 30, weight: .semibold))
                    .lineLimit(1)
                    .minimumScaleFactor(0.6)
                Text(invite.media == .video ? "Video call" : "Voice call")
                    .font(.system(size: 17))
                    .opacity(0.85)
                Text("End-to-end encrypted")
                    .font(.system(size: 13))
                    .opacity(0.6)
                Spacer()
                HStack(spacing: FmSpace.x8) {
                    answer(
                        "Decline", symbol: "phone.down.fill", fill: Color.red, identifier: "chat-call-decline"
                    ) {
                        Task { await engine.declineInvite() }
                    }
                    answer(
                        "Accept", symbol: invite.media == .video ? "video.fill" : "phone.fill", fill: Color.green,
                        identifier: "chat-call-accept"
                    ) {
                        DirectCallActions.accept(engine, hub: hub)
                    }
                }
                .padding(.bottom, FmSpace.x7)
            }
            .foregroundStyle(.white)
            .padding(.horizontal, FmSpace.x5)
        }
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("chat-call-incoming")
    }

    private func answer(
        _ title: String, symbol: String, fill: Color, identifier: String, action: @escaping () -> Void
    ) -> some View {
        Button(action: action) {
            VStack(spacing: FmSpace.x2) {
                Image(systemName: symbol)
                    .font(.system(size: 30, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(width: 76, height: 76)
                    .background(Circle().fill(fill))
                Text(title)
                    .font(.system(size: 15, weight: .medium))
                    .foregroundStyle(.white)
            }
        }
        .buttonStyle(.plain)
        .accessibilityLabel(title)
        .accessibilityIdentifier(identifier)
    }
}

struct DirectCallScreen: View {
    let engine: DirectCallEngine
    let factory: WebCallPeerFactory?
    @State private var notice = ""

    private var nativeRoute: DirectCallRoute? {
        #if canImport(WebRTC)
        return (engine.peerFactory as? NativeCallPeerFactory)?.route
        #else
        return nil
        #endif
    }

    private var hasNativeVideo: Bool {
        #if canImport(WebRTC)
        return (engine.peerFactory as? NativeCallPeerFactory)?.remoteVideo != nil
        #else
        return false
        #endif
    }

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if let peer = factory?.current {
                WebCallSurface(webView: peer.webView)
                    .ignoresSafeArea()
                    .opacity(engine.media == .video ? 1 : 0.01)
            }
            #if canImport(WebRTC)
            if let native = engine.peerFactory as? NativeCallPeerFactory {
                NativeCallSurface(factory: native, media: engine.media)
            }
            #endif
            if engine.media == .audio || (factory?.current == nil && !hasNativeVideo) {
                VStack(spacing: FmSpace.x3) {
                    FmAvatar(name: engine.partner, size: 110, fontSize: 40)
                    Text("@\(engine.partner)")
                        .font(.system(size: 26, weight: .semibold))
                        .foregroundStyle(.white)
                }
            }
            VStack {
                VStack(spacing: 4) {
                    Text("@\(engine.partner)")
                        .font(.system(size: 17, weight: .semibold))
                    Text(engine.stateLine)
                        .font(.system(size: 13))
                        .opacity(0.8)
                    if engine.phase == .live, let route = factory?.route ?? nativeRoute {
                        Text(route.note)
                            .font(.system(size: 12, weight: .medium))
                            .multilineTextAlignment(.center)
                            .opacity(0.8)
                            .accessibilityIdentifier("chat-call-route")
                    }
                    if !engine.voice.isEmpty {
                        Text("Voice: \(FaceChangerLook.voiceLabel(engine.voice))")
                            .font(.system(size: 12, weight: .medium))
                            .opacity(0.8)
                            .accessibilityIdentifier("chat-call-voice-state")
                    }
                }
                .foregroundStyle(.white)
                .shadow(radius: 4)
                .padding(.top, FmSpace.x4)
                .padding(.horizontal, FmSpace.x5)
                .accessibilityElement(children: .combine)
                if !notice.isEmpty {
                    Text(notice)
                        .font(.system(size: 13, weight: .semibold))
                        .multilineTextAlignment(.center)
                        .foregroundStyle(.white)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                        .background(Capsule().fill(Color.black.opacity(0.55)))
                        .padding(.top, 6)
                }
                Spacer()
                controls
                    .padding(.bottom, FmSpace.x5)
            }
        }
        .statusBarHidden(engine.media == .video)
    }

    private var controls: some View {
        HStack(spacing: FmSpace.x4) {
            control(engine.muted ? "mic.slash.fill" : "mic.fill", label: engine.muted ? "Unmute" : "Mute", on: engine.muted) {
                engine.toggleMute()
            }
            if engine.media == .video {
                control(
                    engine.cameraOff ? "video.slash.fill" : "video.fill",
                    label: engine.cameraOff ? "Turn camera on" : "Turn camera off", on: engine.cameraOff
                ) {
                    engine.toggleCamera()
                }
                FaceChangerFaceMenu(look: engine.look, peer: factory?.current, notice: $notice)
                FaceChangerBackgroundButton(look: engine.look, peer: factory?.current)
            }
            FaceChangerVoiceMenu(look: engine.look, shown: engine.voice)
            Button(action: { Task { await engine.hangUp() } }) {
                Image(systemName: "phone.down.fill")
                    .font(.system(size: 22, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(width: 64, height: 64)
                    .background(Circle().fill(Color.red))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Hang up")
            .accessibilityIdentifier("chat-call-hangup")
        }
    }

    private func control(_ symbol: String, label: String, on: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 20, weight: .semibold))
                .foregroundStyle(on ? Color.black : Color.white)
                .frame(width: 56, height: 56)
                .background(Circle().fill(on ? Color.white : Color.white.opacity(0.18)))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }
}
