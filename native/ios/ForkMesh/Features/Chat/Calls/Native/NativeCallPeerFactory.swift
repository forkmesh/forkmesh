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
    private(set) var remoteVideoActive = false
    private(set) var remoteVideoSeen = false
    private(set) var localVideo: RTCVideoTrack?
    let quality = CallQualityMonitor()
    private let micMeter = NativeMicMeter()
    @ObservationIgnored var filter: NativeFrameFilter? = NativeCallMedia.shared.lookFilter
    @ObservationIgnored private var preview: (RTCVideoTrack, NativeCameraPipeline)?
    @ObservationIgnored private var previewAudio: RTCAudioTrack?
    @ObservationIgnored private var meterVideo = false

    var busy: Bool { current != nil }

    func makePeer(media: DirectCallKind, iceServers: [String]) async throws -> DirectCallPeer {
        current?.close()
        micMeter.stop(deactivate: false)
        quality.reset()
        let video = media == .video ? (preview ?? NativeCallMedia.shared.makeVideo()) : nil
        if media != .video, let preview {
            preview.1.stop()
        }
        preview = nil
        let peer = try NativeCallPeer(media: media, iceServers: iceServers, audioTrack: previewAudio, video: video)
        previewAudio = nil
        peer.camera?.setFilter(filter)
        peer.quality = quality
        route = nil
        remoteVideo = peer.remoteVideo
        remoteVideoActive = false
        remoteVideoSeen = false
        localVideo = peer.videoTrack
        peer.onRoute = { [weak self, weak peer] (found: DirectCallRoute?) in
            guard let self, let peer, self.current === peer else { return }
            self.route = found
        }
        peer.onRemoteVideo = { [weak self, weak peer] (track: RTCVideoTrack?) in
            guard let self, let peer, self.current === peer else { return }
            self.remoteVideo = track
        }
        peer.onRemoteVideoActive = { [weak self, weak peer] (on: Bool) in
            guard let self, let peer, self.current === peer else { return }
            self.remoteVideoActive = on
            if on { self.remoteVideoSeen = true }
        }
        let previous = peer.onClosed
        peer.onClosed = { [weak self, weak peer] in
            previous?()
            guard let self, let peer, self.current === peer else { return }
            self.current = nil
            self.route = nil
            self.remoteVideo = nil
            self.remoteVideoActive = false
            self.remoteVideoSeen = false
            self.localVideo = nil
            self.quality.reset()
        }
        current = peer
        return peer
    }

    func startPreview(media: DirectCallKind) async -> Bool {
        meterVideo = media == .video
        if current == nil, !micMeter.running {
            quality.reset()
            _ = micMeter.start(into: quality, video: meterVideo)
        }
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

    func pauseMeter() {
        micMeter.stop(deactivate: false)
    }

    func resumeMeter() {
        guard current == nil, !micMeter.running else { return }
        _ = micMeter.start(into: quality, video: meterVideo)
    }

    func endPreview() {
        micMeter.stop(deactivate: current == nil && !DirectCallAudio.callBusy)
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
        ZStack {
            if media == .video {
                if factory.remoteVideoActive || factory.remoteVideoSeen {
                    NativeVideoView(track: factory.remoteVideo)
                        .opacity(factory.remoteVideoActive ? 1 : 0)
                        .ignoresSafeArea()
                    if let local = factory.localVideo {
                        DraggableCallPreview {
                            NativeVideoView(track: local, mirrored: true)
                        }
                    }
                } else if let local = factory.localVideo {
                    NativeVideoView(track: local, mirrored: true)
                        .ignoresSafeArea()
                        .accessibilityLabel("Your video")
                }
            }
        }
    }
}

struct NativeMiniVideo: View {
    let factory: NativeCallPeerFactory

    var body: some View {
        NativeVideoView(track: factory.remoteVideo)
            .opacity(factory.remoteVideoActive ? 1 : 0)
    }
}
#endif
