#if canImport(Fmm)
import CoreVideo
import FMChat
import Fmm
import Foundation
import Vision

final class FmmVideoSender: @unchecked Sendable {
    static let faceFps = 15
    static let retuneEvery: TimeInterval = 0.2

    struct Encoding {
        let layer: GroupCallVideoLayer
        var stream = 0
        var encoder: FmmVideoEncoder?
        var rep = Int(FmmmobileRepVideo)
        var lastNs: Int64 = 0
    }

    private weak var call: FmmmobileCall?
    private let queue = DispatchQueue(label: "com.forkmesh.fmm.video-send", qos: .userInitiated)
    private let faceQueue = DispatchQueue(label: "com.forkmesh.fmm.face-send", qos: .userInitiated)
    private let lock = NSLock()
    private let shaper = FmmFrameShaper()
    private let landmarks = NativeFaceRenderer.landmarkRequest()
    private var encodings: [Encoding]
    private(set) var track = 0
    private var faceStream = 0
    private var primaryRep = Int(FmmmobileRepVideo)
    private var faceRep = Int(FmmmobileRepOff)
    private var portrait = true
    private var busy = false
    private var faceBusy = false
    private var faceLastNs: Int64 = 0
    private var faceSender: NativeFaceStreamSender?
    private var faceSize = (0, 0)
    private var retunedAt = Date.distantPast
    private(set) var temporal = false
    var paused = false

    init?(call: FmmmobileCall) {
        self.call = call
        var made: [Encoding] = []
        for layer in GroupCallVideoLayer.simulcast {
            var encoding = Encoding(layer: layer)
            encoding.encoder = Self.encoder(layer, portrait: true, layers: GroupCallVideoLayer.temporalLayers)
            guard encoding.encoder != nil else { break }
            made.append(encoding)
        }
        guard let top = made.last?.encoder else { return nil }
        temporal = top.abilities.temporalLayers
        encodings = made
    }

    static func encoder(_ layer: GroupCallVideoLayer, portrait: Bool, layers: Int) -> FmmVideoEncoder? {
        FmmVideoEncoder(
            width: portrait ? layer.height : layer.width, height: portrait ? layer.width : layer.height, fps: layer.fps,
            bitrate: Int(Double(CallMediaTuning.groupVideoMaxBitrate) * layer.share / 2), layers: layers)
    }

    var abilities: FmmEncoderAbilities { encodings.last?.encoder?.abilities ?? FmmEncoderAbilities() }

    func publish() throws {
        guard let call else { return }
        var id = 0
        try call.publishVideo("camera", layers: temporal ? 2 : 1, encodings: encodings.count, face: true, ret0_: &id)
        lock.lock()
        track = id
        faceStream = call.faceStream(id)
        for index in encodings.indices {
            encodings[index].stream = call.encodingStream(id, index: index)
            wire(index)
        }
        lock.unlock()
    }

    private func wire(_ index: Int) {
        let stream = encodings[index].stream
        encodings[index].encoder?.onFrame = { [weak self] frame in self?.send(frame, stream: stream) }
    }

    private func send(_ frame: FmmEncodedFrame, stream: Int) -> Int64? {
        guard let call else { return nil }
        var flags = 0
        if frame.keyframe { flags |= Int(FmmmobileFlagKeyframe) }
        if frame.ltrToken != nil { flags |= Int(FmmmobileFlagLongTerm) }
        if frame.recovery != nil { flags |= Int(FmmmobileFlagRecover) }
        var id: Int64 = -1
        do {
            try call.sendVideo(stream, frame: frame.data, flags: flags, layer: frame.layer, refFrame: frame.recovery ?? -1, ret0_: &id)
        } catch {
            if frame.recovery != nil { request(stream) }
            return nil
        }
        return id
    }

    func submit(_ buffer: CVPixelBuffer, degrees: Int, timeNs: Int64) {
        lock.lock()
        let skip = busy || paused || track == 0
        if !skip { busy = true }
        let faceOn = primaryRep == Int(FmmmobileRepFace) || faceRep == Int(FmmmobileRepFace)
        let faceDue = faceOn && !faceBusy && timeNs - faceLastNs >= 1_000_000_000 / Int64(Self.faceFps)
        if faceDue {
            faceBusy = true
            faceLastNs = timeNs
        }
        lock.unlock()
        if faceDue { faceQueue.async { [weak self] in self?.face(buffer, degrees: degrees, timeNs: timeNs) } }
        guard !skip else { return }
        queue.async { [weak self] in
            guard let self else { return }
            self.encode(buffer, degrees: degrees, timeNs: timeNs)
            self.lock.withLock { self.busy = false }
        }
    }

    private func encode(_ buffer: CVPixelBuffer, degrees: Int, timeNs: Int64) {
        let upright = degrees == 90 || degrees == 270
        lock.lock()
        if upright != portrait {
            portrait = upright
            for index in encodings.indices {
                encodings[index].encoder?.invalidate()
                encodings[index].encoder = Self.encoder(encodings[index].layer, portrait: upright, layers: temporal ? 2 : 1)
                wire(index)
            }
        }
        let video = primaryRep == Int(FmmmobileRepVideo)
        let current = encodings
        lock.unlock()
        if Date().timeIntervalSince(retunedAt) >= Self.retuneEvery { retune() }
        guard video else { return }
        for (index, encoding) in current.enumerated() {
            guard encoding.rep == Int(FmmmobileRepVideo), let encoder = encoding.encoder,
                timeNs - encoding.lastNs >= 1_000_000_000 / Int64(encoding.layer.fps) - 5_000_000,
                let shaped = shaper.upright(buffer, degrees: degrees, width: encoder.width, height: encoder.height)
            else { continue }
            lock.withLock { encodings[index].lastNs = timeNs }
            encoder.encode(shaped, timeNs: timeNs)
        }
    }

    private func retune() {
        guard let call else { return }
        retunedAt = Date()
        let current = lock.withLock { encodings }
        for encoding in current where encoding.stream != 0 {
            let rate = call.videoBitrate(encoding.stream)
            if rate > 0 { encoding.encoder?.setBitrate(rate) }
            encoding.encoder?.acknowledge(upTo: call.ackedFrame(encoding.stream))
        }
    }

    private func face(_ buffer: CVPixelBuffer, degrees: Int, timeNs: Int64) {
        defer { lock.withLock { faceBusy = false } }
        guard let call else { return }
        let map = CallFaceFrameMap(bufferWidth: CVPixelBufferGetWidth(buffer), bufferHeight: CVPixelBufferGetHeight(buffer), degrees: degrees)
        let timeMs = Double(timeNs) / 1_000_000
        guard case let .found(points, _, _, _) = NativeFaceRenderer.detect(buffer, map: map, time: timeMs, probe: false, request: landmarks) else { return }
        lock.lock()
        if faceSender == nil || faceSize != (map.uprightWidth, map.uprightHeight) {
            faceSender = NativeFaceStreamSender(width: map.uprightWidth, height: map.uprightHeight, fps: Self.faceFps)
            faceSize = (map.uprightWidth, map.uprightHeight)
        }
        let sender = faceSender
        let target = primaryRep == Int(FmmmobileRepFace) ? track : (faceRep == Int(FmmmobileRepFace) ? faceStream : 0)
        lock.unlock()
        guard let sender, target != 0 else { return }
        if let packet = sender.frame(time: UInt32(truncatingIfNeeded: Int64(timeMs)), points: points, reading: nil) {
            var id: Int64 = 0
            try? call.sendVideo(target, frame: Data(packet), flags: Int(FmmmobileFlagFace), layer: 0, refFrame: -1, ret0_: &id)
        }
        let still: @Sendable ([UInt8]) -> Void = { [weak call] packet in
            var id: Int64 = 0
            try? call?.sendVideo(target, frame: Data(packet), flags: Int(FmmmobileFlagFace | FmmmobileFlagKeyframe), layer: 0, refFrame: -1, ret0_: &id)
        }
        sender.considerReference(buffer, degrees: degrees, timeMs: timeMs, points: points, reading: nil, send: still)
        sender.considerMouth(buffer, degrees: degrees, timeMs: timeMs, points: points, reading: nil, send: still)
    }

    func request(_ stream: Int) {
        lock.lock()
        let isFace = stream == faceStream || (stream == track && primaryRep == Int(FmmmobileRepFace))
        let encoder = encodings.first { $0.stream == stream }?.encoder
        let sender = faceSender
        lock.unlock()
        if isFace { sender?.requestKey() } else { encoder?.requestKeyframe() }
    }

    func recover(_ stream: Int, frame: Int64) {
        lock.withLock { encodings.first { $0.stream == stream }?.encoder }?.recover(from: frame)
    }

    func represent(_ stream: Int, rep: Int) {
        lock.lock()
        var restart: FmmVideoEncoder?
        if stream == faceStream {
            faceRep = rep
        } else if let index = encodings.firstIndex(where: { $0.stream == stream }) {
            let was = encodings[index].rep
            encodings[index].rep = rep
            if stream == track { primaryRep = rep }
            if rep == Int(FmmmobileRepVideo) && was != rep { restart = encodings[index].encoder }
        }
        let sender = faceSender
        lock.unlock()
        restart?.requestKeyframe()
        if rep == Int(FmmmobileRepFace) { sender?.requestKey() }
    }

    var sendingFace: Bool { lock.withLock { primaryRep == Int(FmmmobileRepFace) } }

    func stop() {
        lock.lock()
        let current = encodings
        track = 0
        lock.unlock()
        for encoding in current { encoding.encoder?.invalidate() }
    }
}

final class FmmRemoteVideo: @unchecked Sendable {
    static let faceWidth = 288
    static let faceHeight = 360

    let stream: Int
    let peer: String
    let feed: FmmTileFeed
    private let decoder = FmmVideoDecoder()
    private let lock = NSLock()
    private var face: NativeFaceStreamRenderer?
    private(set) var rep = Int(FmmmobileRepVideo)

    init(stream: Int, peer: String, feed: FmmTileFeed) {
        self.stream = stream
        self.peer = peer
        self.feed = feed
        decoder.onImage = { [weak feed] image in feed?.show(image) }
    }

    func setRep(_ next: Int) {
        lock.withLock { rep = next }
    }

    var showsFace: Bool { lock.withLock { rep == Int(FmmmobileRepFace) } }

    var faceStale: Bool { lock.withLock { face?.stale ?? false } }

    func handle(_ frame: FmmmobileVideoIn) {
        guard let data = frame.data else { return }
        if frame.face {
            lock.lock()
            if face == nil { face = NativeFaceStreamRenderer() }
            let renderer = face
            lock.unlock()
            renderer?.receive([UInt8](data))
        } else {
            _ = decoder.decode(data)
        }
    }

    func renderFace() {
        let renderer = lock.withLock { rep == Int(FmmmobileRepFace) ? face : nil }
        guard let renderer, let image = renderer.render(width: Self.faceWidth, height: Self.faceHeight) else { return }
        feed.show(image)
    }

    func close() {
        decoder.reset()
    }
}
#endif
