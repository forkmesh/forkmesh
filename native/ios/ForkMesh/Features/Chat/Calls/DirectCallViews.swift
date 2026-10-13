import FMChat
import SwiftUI
import UIKit
import WebKit

enum DirectCallActions {
    @MainActor
    static func start(_ media: DirectCallKind, hub: ChatHub) {
        guard let engine = hub.calls, engine.available, !engine.active else { return }
        let conversation = hub.currentConversation
        Task { @MainActor in
            guard await WebCallPeerFactory.requestAccess(for: media) else {
                hub.post(
                    media == .video
                        ? "Allow ForkMesh to use the camera and microphone in Settings to make video calls."
                        : "Allow ForkMesh to use the microphone in Settings to make calls.")
                return
            }
            if !(await engine.start(conversation, media: media)) {
                hub.post("The call could not start. Check that this conversation is connected.")
            }
        }
    }

    @MainActor
    static func accept(_ engine: DirectCallEngine, hub: ChatHub) {
        guard let invite = engine.invite else { return }
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
        Button(action: { DirectCallActions.start(media, hub: hub) }) {
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
    let hub: ChatHub

    func body(content: Content) -> some View {
        content
            .overlay(alignment: .top) {
                if let engine = hub.calls, let invite = engine.invite, !engine.active {
                    DirectCallIncomingBanner(invite: invite, engine: engine, hub: hub)
                        .padding(.horizontal, FmSpace.x3)
                        .padding(.top, FmSpace.x2)
                        .transition(.move(edge: .top).combined(with: .opacity))
                }
            }
            .fullScreenCover(isPresented: activeBinding) {
                if let engine = hub.calls {
                    DirectCallScreen(engine: engine, factory: engine.peerFactory as? WebCallPeerFactory)
                }
            }
    }

    private var activeBinding: Binding<Bool> {
        Binding(
            get: { hub.calls?.active ?? false },
            set: { shown in
                guard !shown, let engine = hub.calls, engine.active else { return }
                Task { await engine.hangUp() }
            })
    }
}

extension View {
    func directCalls(_ hub: ChatHub) -> some View { modifier(DirectCallLayer(hub: hub)) }
}

struct DirectCallIncomingBanner: View {
    @Environment(\.fmPalette) private var fm
    let invite: DirectCallInvite
    let engine: DirectCallEngine
    let hub: ChatHub

    var body: some View {
        HStack(spacing: FmSpace.x3) {
            FmAvatar(name: invite.partner, size: 40, fontSize: FmType.body)
            VStack(alignment: .leading, spacing: 2) {
                Text("@\(invite.partner)")
                    .font(.system(size: 15, weight: .semibold))
                Text(invite.media == .video ? "Encrypted video call" : "Encrypted voice call")
                    .font(.system(size: 12))
                    .foregroundStyle(fm.textSecondary)
            }
            Spacer(minLength: 0)
            Button(action: { Task { await engine.declineInvite() } }) {
                Image(systemName: "phone.down.fill")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(width: 44, height: 44)
                    .background(Circle().fill(Color.red))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Decline")
            Button(action: { DirectCallActions.accept(engine, hub: hub) }) {
                Image(systemName: invite.media == .video ? "video.fill" : "phone.fill")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(.white)
                    .frame(width: 44, height: 44)
                    .background(Circle().fill(Color.green))
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Accept")
        }
        .padding(FmSpace.x3)
        .background(RoundedRectangle(cornerRadius: 22, style: .continuous).fill(.regularMaterial))
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("chat-call-incoming")
    }
}

struct DirectCallScreen: View {
    let engine: DirectCallEngine
    let factory: WebCallPeerFactory?

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            if let peer = factory?.current {
                WebCallSurface(webView: peer.webView)
                    .ignoresSafeArea()
                    .opacity(engine.media == .video ? 1 : 0.01)
            }
            if engine.media == .audio || factory?.current == nil {
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
                }
                .foregroundStyle(.white)
                .shadow(radius: 4)
                .padding(.top, FmSpace.x4)
                .accessibilityElement(children: .combine)
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
                faceMenu
            }
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

    private var faceMenu: some View {
        Menu {
            Picker("Face", selection: Binding(get: { engine.face }, set: { engine.setFace($0) })) {
                Text("Your own face").tag("")
                ForEach(DirectCallEngine.faces) { (face: DirectCallFace) in
                    Text(face.label).tag(face.id)
                }
            }
        } label: {
            Image(systemName: "theatermasks.fill")
                .font(.system(size: 20, weight: .semibold))
                .foregroundStyle(engine.face.isEmpty ? Color.white : Color.black)
                .frame(width: 56, height: 56)
                .background(Circle().fill(engine.face.isEmpty ? Color.white.opacity(0.18) : Color.white))
        }
        .accessibilityLabel("Face")
        .accessibilityValue(DirectCallEngine.faces.first { $0.id == engine.face }?.label ?? "Your own face")
        .accessibilityHint("Shows the other person a different face that follows your head and lips")
        .accessibilityIdentifier("chat-call-face")
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

struct WebCallSurface: UIViewRepresentable {
    let webView: WKWebView

    func makeUIView(context: Context) -> WKWebView { webView }

    func updateUIView(_ view: WKWebView, context: Context) {}
}
