import FMChat
import SwiftUI
#if canImport(WebRTC)
import WebRTC
#endif

struct GroupCallHeaderButtons: View {
    let hub: ChatHub
    let dark: Bool

    var body: some View {
        #if canImport(WebRTC)
        let call = NativeGroupCall.shared
        let conversation = hub.currentConversation
        if !hub.inDirect, hub.office?.room(for: conversation)?.isPrivate == true, !call.active {
            let count = call.inCall(conversation)
            HStack(spacing: 6) {
                if count > 0 {
                    Button { join(.audio) } label: {
                        Label("Join · \(count)", systemImage: "phone.fill")
                            .font(.system(size: 13, weight: .semibold))
                            .padding(.horizontal, 10)
                            .padding(.vertical, 5)
                            .background(Capsule().fill(Color.green))
                            .foregroundStyle(.white)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel("Join the call in progress, \(count) in call")
                    .accessibilityIdentifier("chat-group-call-join")
                }
                button("video.fill", label: "Video group call", media: .video)
                button("phone.fill", label: "Voice group call", media: .audio)
            }
        }
        #endif
    }

    private func button(_ symbol: String, label: String, media: DirectCallKind) -> some View {
        Button { join(media) } label: {
            Image(systemName: symbol)
                .font(.system(size: 15, weight: .semibold))
                .frame(width: 32, height: 32)
                .foregroundStyle(dark ? Color.white : Color.primary)
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }

    private func join(_ media: DirectCallKind) {
        #if canImport(WebRTC)
        let conversation = hub.currentConversation
        Task { @MainActor in
            if let error = await NativeGroupCall.shared.join(conversation, media: media) { hub.post(error) }
        }
        #endif
    }
}

struct GroupCallLayer: ViewModifier {
    func body(content: Content) -> some View {
        #if canImport(WebRTC)
        content.fullScreenCover(isPresented: Binding(
            get: { NativeGroupCall.shared.active },
            set: { if !$0 { NativeGroupCall.shared.leave() } }
        )) {
            GroupCallPanel(call: NativeGroupCall.shared)
        }
        #else
        content
        #endif
    }
}

extension View {
    func groupCallsRoot() -> some View { modifier(GroupCallLayer()) }
}

#if canImport(WebRTC)
struct GroupCallPanel: View {
    let call: NativeGroupCall

    private let columns = [GridItem(.adaptive(minimum: 150), spacing: 6)]

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            VStack(spacing: FmSpace.x3) {
                VStack(spacing: 4) {
                    Text(call.conversation)
                        .font(.system(size: 17, weight: .semibold))
                    Text(stateLine)
                        .font(.system(size: 13))
                        .opacity(0.8)
                        .accessibilityIdentifier("chat-group-call-state")
                }
                .foregroundStyle(.white)
                .padding(.top, FmSpace.x4)
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 6) {
                        tile(name: "You", track: call.localVideo, mirrored: true)
                        ForEach(call.tiles) { member in
                            tile(name: member.name, track: member.video, mirrored: false)
                        }
                    }
                    .padding(.horizontal, FmSpace.x3)
                }
                Text("End-to-end encrypted. ForkMesh's call server forwards everyone's audio and video without being able to see or hear them.")
                    .font(.system(size: 11))
                    .multilineTextAlignment(.center)
                    .foregroundStyle(.white.opacity(0.6))
                    .padding(.horizontal, FmSpace.x5)
                HStack(spacing: FmSpace.x5) {
                    control(call.muted ? "mic.slash.fill" : "mic.fill", label: call.muted ? "Unmute" : "Mute") {
                        call.toggleMute()
                    }
                    if call.media == .video {
                        control(call.cameraOff ? "video.slash.fill" : "video.fill", label: call.cameraOff ? "Camera on" : "Camera off") {
                            call.toggleCamera()
                        }
                    }
                    control("phone.down.fill", label: "Leave", fill: .red) { call.leave() }
                        .accessibilityIdentifier("chat-group-call-leave")
                }
                .padding(.bottom, FmSpace.x5)
            }
        }
    }

    private var stateLine: String {
        switch call.state {
        case "live": return "Connected · end-to-end encrypted · \(call.tiles.count + 1) in call"
        case "reconnecting": return "Reconnecting…"
        default: return "Connecting…"
        }
    }

    private func tile(name: String, track: RTCVideoTrack?, mirrored: Bool) -> some View {
        ZStack(alignment: .bottomLeading) {
            RoundedRectangle(cornerRadius: 10).fill(Color(white: 0.12))
            if let track {
                NativeVideoView(track: track, mirrored: mirrored)
                    .clipShape(RoundedRectangle(cornerRadius: 10))
            } else {
                FmAvatar(name: name, size: 56, fontSize: 22)
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            Text(name)
                .font(.system(size: 12, weight: .medium))
                .foregroundStyle(.white)
                .padding(.horizontal, 6)
                .padding(.vertical, 2)
                .background(Capsule().fill(Color.black.opacity(0.55)))
                .padding(6)
        }
        .aspectRatio(3 / 4, contentMode: .fit)
    }

    private func control(_ symbol: String, label: String, fill: Color = Color(white: 0.25), action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol)
                .font(.system(size: 22, weight: .semibold))
                .foregroundStyle(.white)
                .frame(width: 60, height: 60)
                .background(Circle().fill(fill))
        }
        .buttonStyle(.plain)
        .accessibilityLabel(label)
    }
}
#endif
