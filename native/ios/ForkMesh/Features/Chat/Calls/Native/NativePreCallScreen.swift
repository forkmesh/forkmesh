import FMChat
import SwiftUI

struct NativePreCallScreen: View {
    let request: DirectCallPrecheckRequest
    let engine: DirectCallEngine
    @State private var starting = false
    @State private var failed = ""

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()
            #if canImport(WebRTC)
            if request.media == .video, let local = NativeCallPeerFactory.shared.localVideo {
                NativeVideoView(track: local, mirrored: true)
                    .ignoresSafeArea()
            }
            #endif
            VStack(spacing: FmSpace.x3) {
                Text("Call @\(request.partner)")
                    .font(.system(size: 22, weight: .semibold))
                    .padding(.top, FmSpace.x6)
                Text(DirectCallRoute.hint)
                    .font(.system(size: 13))
                    .opacity(0.75)
                if request.media == .audio {
                    Spacer()
                    FmAvatar(name: request.partner, size: 110, fontSize: 40)
                }
                Spacer()
                if !failed.isEmpty {
                    Text(failed)
                        .font(.system(size: 13, weight: .semibold))
                        .multilineTextAlignment(.center)
                }
                HStack(spacing: FmSpace.x6) {
                    Button("Cancel") { close() }
                        .buttonStyle(.bordered)
                        .accessibilityIdentifier("chat-precall-cancel")
                    Button(request.media == .video ? "Video call" : "Voice call") { start() }
                        .buttonStyle(.borderedProminent)
                        .tint(.green)
                        .disabled(starting)
                        .accessibilityIdentifier("chat-precall-call")
                }
                .padding(.bottom, FmSpace.x7)
            }
            .foregroundStyle(.white)
            .shadow(radius: 4)
            .padding(.horizontal, FmSpace.x5)
        }
        .task { await open() }
    }

    private func open() async {
        #if canImport(WebRTC)
        if !(await NativeCallPeerFactory.shared.startPreview(media: request.media)) {
            failed = "The camera could not start."
        }
        #endif
    }

    private func start() {
        starting = true
        Task { @MainActor in
            let started = await DirectCallActions.start(request, engine: engine)
            starting = false
            if started {
                DirectCallPrecheck.shared.request = nil
            } else {
                failed = "The call could not start. Try again in a moment."
            }
        }
    }

    private func close() {
        #if canImport(WebRTC)
        if !engine.active { NativeCallPeerFactory.shared.endPreview() }
        #endif
        DirectCallPrecheck.shared.request = nil
    }
}
