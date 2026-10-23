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
    static let routeEvery: TimeInterval = 4
    static let stallAfter: TimeInterval = 4
    static let videoOnFrames: Double = 2

    var onIceCandidate: (@MainActor (JSONObject) -> Void)?
    var onLive: (@MainActor () -> Void)?
    var onClosed: (@MainActor () -> Void)?
    var onConnection: (@MainActor (DirectCallConnection) -> Void)?
    var onRemoteVideoActive: (@MainActor (Bool) -> Void)?
    var onRoute: (@MainActor (DirectCallRoute?) -> Void)?
    var onRemoteVideo: (@MainActor (RTCVideoTrack?) -> Void)?

    let media: DirectCallKind
    let connection: RTCPeerConnection
    let audioTrack: RTCAudioTrack
    let videoTrack: RTCVideoTrack?
    let camera: NativeCameraPipeline?
    private(set) var remoteVideo: RTCVideoTrack?
    weak var quality: CallQualityMonitor?
    private let probe = NativeQualityProbe()
    private var closed = false
    private var live = false
    private var packets: Double = -1
    private var packetsGrewAt = Date()
    private var stalled = false
    private var frameLog: [Double] = []
    private var videoSeen = false
    private var videoOn = false
    private var routeWatch: Task<Void, Never>?
    private var audioStarted = false
    private var cameraOff = false

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
        NativeCallMedia.preferRed(connection)
    }

    private static let streamIds = ["forkmesh"]

    private var delegateRelay: NativePeerDelegate?

    func startCapture() async {
        guard let camera, !cameraOff else { return }
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

    func restartOffer() async throws -> JSONObject? {
        guard !closed else { return nil }
        connection.restartIce()
        let created = try await describe { connection, done in
            connection.offer(for: Self.constraints, completionHandler: done)
        }
        return try await adopt(created)
    }

    func resumeMedia() {
        guard !closed, live else { return }
        NativeCallMedia.shared.resumeAudio()
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
        cameraOff = off
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
        routeWatch?.cancel()
        probe.stop()
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
            if !live {
                live = true
                if !audioStarted {
                    audioStarted = true
                    NativeCallMedia.shared.beginAudio()
                }
                watchRoute()
                watchQuality()
            } else {
                onConnection?(.connected)
            }
            NativeCallMedia.capSenders(connection, videoBitrate: CallMediaTuning.videoMaxBitrate)
            onLive?()
        case .closed:
            close()
        case .failed:
            onConnection?(.failed)
        case .disconnected:
            onConnection?(.disconnected)
        default:
            break
        }
    }

    private func watchMedia(_ reports: [[CallStatsEntry]]) {
        guard !closed else { return }
        var received: Double = 0
        var audio = false
        var frames: Double = 0
        var video = false
        for entries in reports {
            for entry in entries where entry.type == "inbound-rtp" {
                if entry.kind == "audio" {
                    audio = true
                    received += entry.number("packetsReceived") ?? 0
                } else if entry.kind == "video" {
                    video = true
                    frames += entry.number("framesDecoded") ?? 0
                }
            }
        }
        let now = Date()
        if audio {
            if received > packets {
                packets = received
                packetsGrewAt = now
                if stalled {
                    stalled = false
                    onConnection?(.connected)
                }
            } else if !stalled, connection.connectionState == .connected,
                now.timeIntervalSince(packetsGrewAt) >= Self.stallAfter
            {
                stalled = true
                onConnection?(.stalled)
            }
        }
        guard video else { return }
        frameLog.append(frames)
        if frameLog.count > 3 { frameLog.removeFirst(frameLog.count - 3) }
        let on: Bool
        if frames > 0 && !videoSeen {
            videoSeen = true
            on = true
        } else if frameLog.count >= 3 {
            on = frames - (frameLog.first ?? frames) >= Self.videoOnFrames
        } else {
            return
        }
        guard on != videoOn else { return }
        videoOn = on
        onRemoteVideoActive?(on)
    }

    fileprivate func receiverAdded(_ receiver: RTCRtpReceiver) {
        guard let track = receiver.track as? RTCVideoTrack else { return }
        remoteVideo = track
        onRemoteVideo?(track)
    }

    private func watchQuality() {
        guard let quality else { return }
        probe.start(
            monitor: quality,
            connections: { [weak self] in
                guard let self, !self.closed else { return [] }
                return [self.connection]
            },
            muted: { [weak self] in !(self?.audioTrack.isEnabled ?? false) },
            levels: { NativeQualityProbe.levelsShown() },
            full: { [weak self] (reports: [[CallStatsEntry]]) in self?.watchMedia(reports) })
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
