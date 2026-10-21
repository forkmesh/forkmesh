#if canImport(WebRTC)
import FMBase
import FMChat
import Foundation
import Observation
import WebRTC

struct GroupCallTile: Identifiable, Equatable {
    let peer: String
    var name: String
    var video: RTCVideoTrack?

    var id: String { peer }

    static func == (a: GroupCallTile, b: GroupCallTile) -> Bool {
        a.peer == b.peer && a.name == b.name && a.video === b.video
    }
}

@MainActor
@Observable
final class NativeGroupCall {
    static let shared = NativeGroupCall()
    static let reconnectTries = 5
    static let reconnectBase: TimeInterval = 1

    private(set) var conversation = ""
    private(set) var media: DirectCallKind = .audio
    private(set) var state = ""
    private(set) var tiles: [GroupCallTile] = []
    private(set) var muted = false
    private(set) var cameraOff = false
    private(set) var lastError = ""
    private(set) var localVideo: RTCVideoTrack?
    private(set) var rosterTick = 0

    @ObservationIgnored let roster = GroupCallRoster()
    @ObservationIgnored weak var office: OfficeChannelService?
    @ObservationIgnored private var sendKey = GroupCallSendKey()
    @ObservationIgnored private var peer = ""
    @ObservationIgnored private var connection: RTCPeerConnection?
    @ObservationIgnored private var relay: GroupPeerDelegate?
    @ObservationIgnored private var socket: URLSessionWebSocketTask?
    @ObservationIgnored private var audioTrack: RTCAudioTrack?
    @ObservationIgnored private var video: (RTCVideoTrack, NativeCameraPipeline)?
    @ObservationIgnored private var encryptor: RTCFmFrameEncryptor?
    @ObservationIgnored private var decryptors: [String: [RTCFmFrameDecryptor]] = [:]
    @ObservationIgnored private var timers: [Task<Void, Never>] = []
    @ObservationIgnored private var retries = 0
    @ObservationIgnored private var reconnecting: Task<Void, Never>?
    @ObservationIgnored private var generation = 0
    @ObservationIgnored private var audioStarted = false

    var active: Bool { !conversation.isEmpty }

    func inCall(_ scope: String) -> Int {
        _ = rosterTick
        return roster.roster(scope).count
    }

    func handleFrame(_ plain: JSONObject, conversation scope: String) {
        guard let event = roster.handle(plain, conversation: scope, selfPeer: peer) else { return }
        rosterTick += 1
        guard active, scope == conversation else { return }
        switch event {
        case let .joined(_, isNew):
            if isNew { shareKey() }
            relabel()
        case let .left(peer):
            forget(peer)
            rotateKey()
            relabel()
        case let .key(peer, key):
            for decryptor in decryptors[peer] ?? [] {
                decryptor.setKey(Data(key.raw), keyId: UInt8(key.keyId))
            }
        }
    }

    func join(_ scope: String, media kind: DirectCallKind) async -> String? {
        guard !active else { return "Leave the current call first." }
        guard office?.room(for: scope)?.isPrivate == true else { return "Group calls need a private channel." }
        guard await WebCallPeerFactory.requestAccess(for: kind) else {
            return kind == .video
                ? "Allow ForkMesh to use the camera and microphone in Settings to join the call."
                : "Allow ForkMesh to use the microphone in Settings to join the call."
        }
        conversation = scope
        media = kind
        state = "joining"
        lastError = ""
        muted = false
        cameraOff = false
        sendKey = GroupCallSendKey()
        encryptor = RTCFmFrameEncryptor()
        retries = 0
        audioTrack = NativeCallMedia.shared.makeAudioTrack()
        if kind == .video {
            let made = NativeCallMedia.shared.makeVideo()
            video = made
            localVideo = made.0
            _ = await made.1.start(
                width: CallMediaTuning.groupVideoWidth, height: CallMediaTuning.groupVideoHeight,
                fps: CallMediaTuning.groupVideoFrameRate)
        }
        startTimers()
        guard await connect() else {
            let error = "Group calls are not available right now."
            leave()
            lastError = error
            return error
        }
        return nil
    }

    func leave() {
        guard active else { return }
        reconnecting?.cancel()
        reconnecting = nil
        for timer in timers { timer.cancel() }
        timers.removeAll()
        dropConnection()
        encryptor = nil
        video?.1.stop()
        video = nil
        localVideo = nil
        audioTrack = nil
        if audioStarted {
            audioStarted = false
            NativeCallMedia.shared.endAudio()
        }
        conversation = ""
        state = ""
    }

    func toggleMute() {
        muted.toggle()
        audioTrack?.isEnabled = !muted
    }

    func toggleCamera() {
        guard media == .video else { return }
        cameraOff.toggle()
        video?.0.isEnabled = !cameraOff
        if cameraOff {
            video?.1.stop()
        } else if let pipeline = video?.1 {
            Task {
                _ = await pipeline.start(
                    width: CallMediaTuning.groupVideoWidth, height: CallMediaTuning.groupVideoHeight,
                    fps: CallMediaTuning.groupVideoFrameRate)
            }
        }
    }

    private func startTimers() {
        timers.append(Task { @MainActor [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(GroupCallFrames.heartbeat * 1_000_000_000))
                self?.announce("in")
            }
        })
        timers.append(Task { @MainActor [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: UInt64(GroupCallFrames.pingEvery * 1_000_000_000))
                self?.sendSignal(["type": "ping"])
            }
        })
    }

    private func announce(_ value: String) {
        guard !peer.isEmpty, let office else { return }
        let fields = GroupCallFrames.presence(peer: peer, state: value, media: media.rawValue)
        let scope = conversation
        Task { await office.sendLiveFrame(scope, type: GroupCallFrames.presenceType, fields: fields) }
    }

    private func shareKey() {
        guard !peer.isEmpty, let office else { return }
        let fields = GroupCallFrames.key(peer: peer, keyId: sendKey.keyId, raw: sendKey.raw)
        let scope = conversation
        Task { await office.sendLiveFrame(scope, type: GroupCallFrames.keyType, fields: fields) }
    }

    private func useKey() {
        encryptor?.setKey(Data(sendKey.raw), keyId: UInt8(sendKey.keyId))
    }

    private func rotateKey() {
        sendKey.rotate()
        shareKey()
        let keyId = sendKey.keyId
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(GroupCallFrames.rotateDelay * 1_000_000_000))
            guard let self, self.active, self.sendKey.keyId == keyId else { return }
            self.useKey()
        }
    }

    private func relabel() {
        tiles = tiles.map { tile in
            var copy = tile
            copy.name = roster.name(conversation, peer: tile.peer).isEmpty ? "Someone" : roster.name(conversation, peer: tile.peer)
            return copy
        }
    }

    private func forget(_ peer: String) {
        for decryptor in decryptors[peer] ?? [] { decryptor.clearKeys() }
        decryptors.removeValue(forKey: peer)
        tiles.removeAll { $0.peer == peer }
    }

    private func connect() async -> Bool {
        guard let office, let encryptor, let ticket = try? await office.callTicket(conversation),
            let url = URL(string: ticket.url)
        else { return false }
        generation += 1
        let current = generation
        let shared = NativeCallMedia.shared
        let delegate = GroupPeerDelegate()
        guard let made = shared.factory.peerConnection(
            with: shared.configuration(iceServers: []),
            constraints: RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: nil),
            delegate: delegate)
        else { return false }
        delegate.call = self
        delegate.generation = current
        relay = delegate
        connection = made
        let audioInit = RTCRtpTransceiverInit()
        audioInit.direction = .sendOnly
        audioInit.streamIds = ["forkmesh"]
        if let audioTrack, let transceiver = made.addTransceiver(with: audioTrack, init: audioInit) {
            encryptor.attach(to: transceiver.sender)
        }
        let videoInit = RTCRtpTransceiverInit()
        videoInit.direction = .sendOnly
        videoInit.streamIds = ["forkmesh"]
        let videoTransceiver = video.map { made.addTransceiver(with: $0.0, init: videoInit) }
            ?? made.addTransceiver(of: .video, init: videoInit)
        if let videoTransceiver {
            encryptor.attach(to: videoTransceiver.sender)
        }
        let task = URLSession.shared.webSocketTask(with: url)
        socket = task
        task.resume()
        sendSignal(["type": "join", "ticket": .string(ticket.ticket)])
        receive(task, generation: current)
        return true
    }

    private func dropConnection() {
        if !peer.isEmpty { announce("out") }
        peer = ""
        generation += 1
        if let socket {
            socket.send(.string(JSON.encode(.object(["type": .string("leave")])))) { _ in }
            socket.cancel(with: .normalClosure, reason: nil)
        }
        socket = nil
        connection?.close()
        connection = nil
        relay = nil
        for (peer, _) in decryptors { forget(peer) }
        decryptors = [:]
        tiles = []
    }

    private func connectionLost(_ reason: String) {
        guard active, reconnecting == nil else { return }
        dropConnection()
        guard retries < Self.reconnectTries else {
            lastError = reason
            leave()
            lastError = reason
            return
        }
        let wait = Self.reconnectBase * pow(2, Double(retries))
        retries += 1
        state = "reconnecting"
        reconnecting = Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: UInt64(wait * 1_000_000_000))
            guard let self, !Task.isCancelled else { return }
            self.reconnecting = nil
            guard self.active else { return }
            if !(await self.connect()) { self.connectionLost(reason) }
        }
    }

    private func sendSignal(_ message: JSONObject) {
        socket?.send(.string(JSON.encode(.object(message)))) { _ in }
    }

    private func receive(_ task: URLSessionWebSocketTask, generation current: Int) {
        task.receive { [weak self] result in
            Task { @MainActor [weak self] in
                guard let self, self.generation == current else { return }
                switch result {
                case .failure:
                    self.connectionLost("The call ended.")
                case let .success(message):
                    if case let .string(text) = message {
                        await self.handleSignal(JSON.parseObject(text))
                    }
                    if self.generation == current { self.receive(task, generation: current) }
                }
            }
        }
    }

    private func handleSignal(_ message: JSONObject) async {
        guard let connection else { return }
        switch message["type"]?.text ?? "" {
        case "joined":
            peer = message["peer"]?.text ?? ""
            announce("in")
            sendKey.rotate()
            useKey()
            shareKey()
            guard let offer = try? await describe({ connection.offer(for: Self.constraints, completionHandler: $0) }),
                (try? await setLocal(offer)) != nil
            else { return connectionLost("The call could not connect.") }
            sendSignal(["type": "offer", "sdp": .string(offer.sdp)])
        case "answer":
            try? await setRemote(RTCSessionDescription(type: .answer, sdp: message["sdp"]?.text ?? ""))
        case "offer":
            guard (try? await setRemote(RTCSessionDescription(type: .offer, sdp: message["sdp"]?.text ?? ""))) != nil,
                let answer = try? await describe({ connection.answer(for: Self.constraints, completionHandler: $0) }),
                (try? await setLocal(answer)) != nil
            else { return }
            sendSignal(["type": "answer", "sdp": .string(answer.sdp), "rev": message["rev"] ?? .int(0)])
        case "peer-left":
            let gone = message["peer"]?.text ?? ""
            forget(gone)
            rotateKey()
        case "error":
            if message["error"]?.text == "room_full" {
                leave()
                lastError = "This call is full."
            } else {
                connectionLost("The call could not connect.")
            }
        default:
            break
        }
    }

    fileprivate func stateChanged(_ newState: RTCPeerConnectionState, generation current: Int) {
        guard current == generation else { return }
        switch newState {
        case .connected:
            state = "live"
            retries = 0
            if !audioStarted {
                audioStarted = true
                NativeCallMedia.shared.beginAudio()
            }
            if let connection { NativeCallMedia.capSenders(connection, videoBitrate: CallMediaTuning.groupVideoMaxBitrate) }
        case .failed:
            connectionLost("The call connection failed.")
        default:
            break
        }
    }

    fileprivate func receiverAdded(_ receiver: RTCRtpReceiver, streams: [RTCMediaStream], generation current: Int) {
        guard current == generation, let stream = streams.first?.streamId, GroupCallFrames.validPeer(stream) else { return }
        let decryptor = RTCFmFrameDecryptor()
        if let key = roster.keys(conversation)[stream] {
            decryptor.setKey(Data(key.raw), keyId: UInt8(key.keyId))
        }
        decryptor.attach(to: receiver)
        decryptors[stream, default: []].append(decryptor)
        let name = roster.name(conversation, peer: stream)
        var tile = tiles.first { $0.peer == stream } ?? GroupCallTile(peer: stream, name: name.isEmpty ? "Someone" : name)
        if let track = receiver.track as? RTCVideoTrack { tile.video = track }
        if let index = tiles.firstIndex(where: { $0.peer == stream }) {
            tiles[index] = tile
        } else {
            tiles.append(tile)
        }
    }

    private static let constraints = RTCMediaConstraints(mandatoryConstraints: nil, optionalConstraints: nil)

    private func describe(
        _ create: (@escaping (RTCSessionDescription?, Error?) -> Void) -> Void
    ) async throws -> RTCSessionDescription {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<RTCSessionDescription, Error>) in
            create { description, error in
                if let description {
                    continuation.resume(returning: description)
                } else {
                    continuation.resume(throwing: error ?? NativeCallError.badDescription)
                }
            }
        }
    }

    private func setLocal(_ description: RTCSessionDescription) async throws {
        guard let connection else { throw NativeCallError.noConnection }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.setLocalDescription(description) { error in
                if let error { continuation.resume(throwing: error) } else { continuation.resume() }
            }
        }
    }

    private func setRemote(_ description: RTCSessionDescription) async throws {
        guard let connection else { throw NativeCallError.noConnection }
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Void, Error>) in
            connection.setRemoteDescription(description) { error in
                if let error { continuation.resume(throwing: error) } else { continuation.resume() }
            }
        }
    }
}

final class GroupPeerDelegate: NSObject, RTCPeerConnectionDelegate {
    weak var call: NativeGroupCall?
    var generation = 0

    func peerConnection(_ peerConnection: RTCPeerConnection, didChange stateChanged: RTCSignalingState) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didAdd stream: RTCMediaStream) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove stream: RTCMediaStream) {}
    func peerConnectionShouldNegotiate(_ peerConnection: RTCPeerConnection) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCIceConnectionState) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCIceGatheringState) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didGenerate candidate: RTCIceCandidate) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didRemove candidates: [RTCIceCandidate]) {}
    func peerConnection(_ peerConnection: RTCPeerConnection, didOpen dataChannel: RTCDataChannel) {}

    func peerConnection(_ peerConnection: RTCPeerConnection, didChange newState: RTCPeerConnectionState) {
        let current = generation
        Task { @MainActor [weak self] in self?.call?.stateChanged(newState, generation: current) }
    }

    func peerConnection(
        _ peerConnection: RTCPeerConnection, didAdd rtpReceiver: RTCRtpReceiver, streams mediaStreams: [RTCMediaStream]
    ) {
        let current = generation
        Task { @MainActor [weak self] in self?.call?.receiverAdded(rtpReceiver, streams: mediaStreams, generation: current) }
    }
}
#endif
