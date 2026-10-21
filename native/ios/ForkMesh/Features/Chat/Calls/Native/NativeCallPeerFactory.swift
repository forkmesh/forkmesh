#if canImport(WebRTC)
import FMChat
import Foundation
import Observation
import SwiftUI
import WebRTC

@MainActor
@Observable
final class NativeCallPeerFactory: DirectCallPeerMaking {
    static let shared = NativeCallPeerFactory()

    private(set) var current: NativeCallPeer?
    private(set) var route: DirectCallRoute?
    private(set) var remoteVideo: RTCVideoTrack?
    private(set) var localVideo: RTCVideoTrack?
    @ObservationIgnored var filter: NativeFrameFilter? = NativeCallMedia.shared.lookFilter
    @ObservationIgnored private var preview: (RTCVideoTrack, NativeCameraPipeline)?
    @ObservationIgnored private var previewAudio: RTCAudioTrack?

    var busy: Bool { current != nil }

    func makePeer(media: DirectCallKind, iceServers: [String]) async throws -> DirectCallPeer {
        current?.close()
        let video = media == .video ? (preview ?? NativeCallMedia.shared.makeVideo()) : nil
        if media != .video, let preview {
            preview.1.stop()
        }
        preview = nil
        let peer = try NativeCallPeer(media: media, iceServers: iceServers, audioTrack: previewAudio, video: video)
        previewAudio = nil
        peer.camera?.setFilter(filter)
        route = nil
        remoteVideo = nil
        localVideo = peer.videoTrack
        peer.onRoute = { [weak self, weak peer] (found: DirectCallRoute?) in
            guard let self, let peer, self.current === peer else { return }
            self.route = found
        }
        peer.onRemoteVideo = { [weak self, weak peer] (track: RTCVideoTrack?) in
            guard let self, let peer, self.current === peer else { return }
            self.remoteVideo = track
        }
        let previous = peer.onClosed
        peer.onClosed = { [weak self, weak peer] in
            previous?()
            guard let self, let peer, self.current === peer else { return }
            self.current = nil
            self.route = nil
            self.remoteVideo = nil
            self.localVideo = nil
        }
        current = peer
        return peer
    }

    func startPreview(media: DirectCallKind) async -> Bool {
        if media == .video {
            let made = preview ?? NativeCallMedia.shared.makeVideo()
            preview = made
            made.1.setFilter(filter)
            localVideo = made.0
            return await made.1.start(
                width: CallMediaTuning.videoWidth, height: CallMediaTuning.videoHeight,
                fps: CallMediaTuning.videoFrameRate)
        }
        if previewAudio == nil { previewAudio = NativeCallMedia.shared.makeAudioTrack() }
        return true
    }

    func endPreview() {
        preview?.1.stop()
        preview = nil
        previewAudio = nil
        if current == nil { localVideo = nil }
    }

    func setFilter(_ next: NativeFrameFilter?) {
        filter = next
        preview?.1.setFilter(next)
        current?.camera?.setFilter(next)
    }
}

struct NativeVideoView: UIViewRepresentable {
    let track: RTCVideoTrack?
    var mirrored = false
    var fill = true

    final class Holder {
        var track: RTCVideoTrack?
    }

    func makeCoordinator() -> Holder { Holder() }

    func makeUIView(context: Context) -> RTCMTLVideoView {
        let view = RTCMTLVideoView(frame: .zero)
        view.videoContentMode = fill ? .scaleAspectFill : .scaleAspectFit
        view.backgroundColor = .black
        return view
    }

    func updateUIView(_ view: RTCMTLVideoView, context: Context) {
        view.transform = mirrored ? CGAffineTransform(scaleX: -1, y: 1) : .identity
        guard context.coordinator.track !== track else { return }
        context.coordinator.track?.remove(view)
        context.coordinator.track = track
        track?.add(view)
    }

    static func dismantleUIView(_ view: RTCMTLVideoView, coordinator: Holder) {
        coordinator.track?.remove(view)
        coordinator.track = nil
    }
}

struct NativeCallSurface: View {
    let factory: NativeCallPeerFactory
    let media: DirectCallKind

    var body: some View {
        ZStack(alignment: .topTrailing) {
            if media == .video {
                NativeVideoView(track: factory.remoteVideo)
                    .ignoresSafeArea()
                if let local = factory.localVideo {
                    NativeVideoView(track: local, mirrored: true)
                        .frame(width: 110, height: 160)
                        .clipShape(RoundedRectangle(cornerRadius: 12))
                        .padding(.top, 90)
                        .padding(.trailing, 14)
                }
            }
        }
    }
}
#endif
