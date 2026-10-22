#if canImport(WebRTC)
import FMBase
import FMChat
import Foundation
import WebRTC

enum NativeCallError: Error {
    case noConnection
    case badDescription
    case failed(String)
}

@MainActor
final class NativeCallPeer: NSObject, DirectCallPeer {
    static let disconnectGrace: TimeInterval = 8
    static let routeEvery: TimeInterval = 4

    var onIceCandidate: (@MainActor (JSONObject) -> Void)?
    var onLive: (@MainActor () -> Void)?
    var onClosed: (@MainActor () -> Void)?
    var onRoute: (@MainActor (DirectCallRoute?) -> Void)?
    var onRemoteVideo: (@MainActor (RTCVideoTrack?) -> Void)?

    let media: DirectCallKind
    let connection: RTCPeerConnection
    let audioTrack: RTCAudioTrack
    let videoTrack: RTCVideoTrack?
    let camera: NativeCameraPipeline?
    private(set) var remoteVideo: RTCVideoTrack?
    private var closed = false
    private var live = false
    private var grace: Task<Void, Never>?
    private var routeWatch: Task<Void, Never>?
    private var audioStarted = false

    init(media: DirectCallKind, iceServers: [String], audioTrack: RTCAudioTrack? = nil, video: (RTCVideoTrack, NativeCameraPipeline)? = nil) throws {
        let shared = NativeCallMedia.shared
        self.media = media
        self.audioTrack = audioTrack ?? shared.makeAudioTrack()
        if media == .video {
            let made = video ?? shared.makeVideo()
            videoTrack = made.0
            camera = made.1
        } else {
            videoTrack = nil
            camera = nil
        }
        let relay = NativePeerDelegate()
        guard let made = shared.factory.peerConnection(
            with: shared.configuration(iceServers: iceServers),
            constraints: RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: nil),
            delegate: relay)
        else { throw NativeCallError.noConnection }
        connection = made
        super.init()
        relay.peer = self
        delegateRelay = relay
        connection.add(self.audioTrack, streamIds: Self.streamIds)
        if let videoTrack {
            connection.add(videoTrack, streamIds: Self.streamIds)
        } else {
            let videoInit = RTCRtpTransceiverInit()
            videoInit.direction = .sendRecv
            videoInit.streamIds = Self.streamIds
            connection.addTransceiver(of: .video, init: videoInit)
        }
    }

    private static let streamIds = ["forkmesh"]

    private var delegateRelay: NativePeerDelegate?

    func startCapture() async {
        guard let camera else { return }
        _ = await camera.start(
            width: CallMediaTuning.videoWidth, height: CallMediaTuning.videoHeight, fps: CallMediaTuning.videoFrameRate)
    }

    func offer() async throws -> JSONObject {
        await startCapture()
        let created = try await describe { connection, done in
            connection.offer(for: Self.constraints, completionHandler: done)
        }
        return try await adopt(created)
    }

    func answer(offer: JSONObject) async throws -> JSONObject {
        await startCapture()
        try await setRemote(offer, type: .offer)
        let created = try await describe { connection, done in
            connection.answer(for: Self.constraints, completionHandler: done)
        }
        return try await adopt(created)
    }

    func acceptAnswer(_ answer: JSONObject) async throws {
        try await setRemote(answer, type: .answer)
    }

    func add(candidate: JSONObject) async {
        guard !closed, let line = candidate["candidate"]?.text, !line.isEmpty else { return }
        let index = Int32(candidate["sdpMLineIndex"]?.intValue ?? 0)
        let mid = candidate["sdpMid"]?.text
        let made = RTCIceCandidate(sdp: line, sdpMLineIndex: index, sdpMid: mid)
        await withCheckedContinuation { (continuation: CheckedContinuation<Void, Never>) in
            connection.add(made) { _ in continuation.resume() }
        }
    }

    func setMuted(_ muted: Bool) {
        audioTrack.isEnabled = !muted
    }

    func setCameraOff(_ off: Bool) {
        videoTrack?.isEnabled = !off
        if off {
            camera?.stop()
        } else {
            Task { await startCapture() }
        }
    }

    func close() {
        guard !closed else { return }
        closed = true
        grace?.cancel()
        routeWatch?.cancel()
        camera?.stop()
        connection.close()
        if audioStarted {
            audioStarted = false
            NativeCallMedia.shared.endAudio()
        }
        onRoute?(nil)
        onClosed?()
    }

    private static let constraints = RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: nil)

    private func describe(
        _ create: (RTCPeerConnection, @escaping (RTCSessionDescription?, Error?) -> Void) -> Void
    ) async throws -> RTCSessionDescription {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<RTCSessionDescription, Error>) in
            create(connection) { description, error in
                if let description {
                    continuation.resume(returning: description)
                } else {
                    continuation.resume(throwing: error ?? NativeCallError.badDescription)
                }
            }
        }
    }

    private func adopt(_ created: RTCSessionDescription) async throws -> JSONObject {
        let tuned = RTCSessionDescription(type: created.type, sdp: CallMediaTuning.tune(created.sdp))
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.setLocalDescription(tuned) { error in
                if let error { continuation.resume(throwing: error) } else { continuation.resume() }
            }
        }
        return ["type": .string(RTCSessionDescription.string(for: tuned.type)), "sdp": .string(tuned.sdp)]
    }

    private func setRemote(_ description: JSONObject, type: RTCSdpType) async throws {
        guard let sdp = description["sdp"]?.text, !sdp.isEmpty else { throw NativeCallError.badDescription }
        let remote = RTCSessionDescription(type: type, sdp: sdp)
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.setRemoteDescription(remote) { error in
                if let error { continuation.resume(throwing: error) } else { continuation.resume() }
            }
        }
    }

    fileprivate func generated(_ candidate: RTCIceCandidate) {
        guard !closed else { return }
        var json: JSONObject = ["candidate": .string(candidate.sdp), "sdpMLineIndex": .int(Int64(candidate.sdpMLineIndex))]
        if let mid = candidate.sdpMid { json["sdpMid"] = .string(mid) }
        onIceCandidate?(json)
    }

    fileprivate func stateChanged(_ state: RTCPeerConnectionState) {
        guard !closed else { return }
        switch state {
        case .connected:
            grace?.cancel()
            grace = nil
            if !live {
                live = true
                if !audioStarted {
                    audioStarted = true
                    NativeCallMedia.shared.beginAudio()
                }
                NativeCallMedia.capSenders(connection, videoBitrate: CallMediaTuning.videoMaxBitrate)
                watchRoute()
                onLive?()
            }
        case .failed, .closed:
            close()
        case .disconnected:
            grace?.cancel()
            grace = Task { @MainActor [weak self] in
                try? await Task.sleep(nanoseconds: UInt64(NativeCallPeer.disconnectGrace * 1_000_000_000))
                guard let self, !Task.isCancelled, self.connection.connectionState == .disconnected else { return }
                self.close()
            }
        default:
            break
        }
    }

    fileprivate func receiverAdded(_ receiver: RTCRtpReceiver) {
        guard let track = receiver.track as? RTCVideoTrack else { return }
        remoteVideo = track
        onRemoteVideo?(track)
    }

    private func watchRoute() {
        routeWatch?.cancel()
        routeWatch = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                guard let self, !self.closed else { return }
                self.onRoute?(await NativeCallMedia.readRoute(self.connection))
                try? await Task.sleep(nanoseconds: UInt64(NativeCallPeer.routeEvery * 1_000_000_000))
            }
        }
    }
}

final class NativePeerDelegate: NSObject, RTCPeerConnectionDelegate {
    weak var peer: NativeCallPeer?

    func peerConnection(_ peerConnection: RTCPeerConnection, didChange stateChanged: RTCSignalingState) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didAdd stream: RTCMediaStream) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove stream: RTCMediaStream) {}
    func peerConnectionShouldNegotiate(_ peerConnection: RTCPeerConnection) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCIceConnectionState) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCIceGatheringState) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove candidates: [RTCIceCandidate]) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didOpen dataChannel: RTCDataChannel) {}

    func peerConnection(_ peerConnection: RTCPeerConnection, didGenerate candidate: RTCIceCandidate) {
        Task { @MainActor [weak self] in self?.peer?.generated(candidate) }
    }

    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCPeerConnectionState) {
        Task { @MainActor [weak self] in self?.peer?.stateChanged(newState) }
    }

    func peerConnection(
        _ peerConnection: RTCPeerConnection, didAdd rtpReceiver: RTCRtpReceiver, streams mediaStreams: [RTCMediaStream]
    ) {
        Task { @MainActor [weak self] in self?.peer?.receiverAdded(rtpReceiver) }
    }
}
#endif
