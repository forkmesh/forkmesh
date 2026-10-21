#if canImport(WebRTC)
import AVFoundation
import FMChat
import Foundation
import WebRTC

protocol NativeFrameFilter: AnyObject {
    var active: Bool { get }
    var wantsFaceTracking: Bool { get }
    func filter(_ buffer: CVPixelBuffer, rotation: RTCVideoRotation, timeNs: Int64, tracking: NativeFaceTrackingFrame?) -> CVPixelBuffer
}

final class NativeCameraPipeline: NSObject, RTCVideoCapturerDelegate, @unchecked Sendable {
    enum Feed {
        case off
        case camera
        case tracking
    }

    let source: RTCVideoSource
    private(set) var capturer: RTCCameraVideoCapturer?
    weak var filter: NativeFrameFilter?
    private var tap: ((CVPixelBuffer, Int, Int64) -> Void)?
    private let lock = NSLock()
    private var running = false
    private var request = (width: 0, height: 0, fps: 0)
    private var feed = Feed.off
    private var switching = false
    private var trackingFailed = false
    private var trackerFrames = 0
    private var tracker: NativeFaceTrackingSource?
    private let gate = CallFaceFrameGate()
    private var scale = 1.0
    private let trackerCapturer: RTCVideoCapturer

    init(source: RTCVideoSource) {
        self.source = source
        trackerCapturer = RTCVideoCapturer(delegate: source)
        super.init()
    }

    func setTap(_ next: ((CVPixelBuffer, Int, Int64) -> Void)?) {
        lock.withLock { tap = next }
    }

    private func deliver(_ frame: RTCVideoFrame, from capturer: RTCVideoCapturer) {
        source.capturer(capturer, didCapture: frame)
        let sink = lock.withLock { tap }
        if let sink, let buffer = frame.buffer as? RTCCVPixelBuffer {
            sink(buffer.pixelBuffer, frame.rotation.rawValue, frame.timeStampNs)
        }
    }

    func capturer(_ capturer: RTCVideoCapturer, didCapture frame: RTCVideoFrame) {
        lock.lock()
        let current = filter
        lock.unlock()
        guard let current, let buffer = frame.buffer as? RTCCVPixelBuffer else {
            deliver(frame, from: capturer)
            return
        }
        guard admit(frame.timeStampNs, filter: current) else { return }
        if current.wantsFaceTracking { considerTracking() }
        let filtered = current.filter(buffer.pixelBuffer, rotation: frame.rotation, timeNs: frame.timeStampNs, tracking: nil)
        let out = RTCVideoFrame(buffer: RTCCVPixelBuffer(pixelBuffer: filtered), rotation: frame.rotation, timeStampNs: frame.timeStampNs)
        deliver(out, from: capturer)
    }

    private func tracked(_ buffer: CVPixelBuffer, rotation degrees: Int, timeNs: Int64, tracking: NativeFaceTrackingFrame) {
        lock.lock()
        trackerFrames += 1
        let current = filter
        let live = feed == .tracking && running
        lock.unlock()
        guard live else { return }
        let rotation = RTCVideoRotation(rawValue: degrees) ?? ._90
        guard let current, current.wantsFaceTracking else {
            considerCamera()
            deliver(RTCVideoFrame(buffer: RTCCVPixelBuffer(pixelBuffer: buffer), rotation: rotation, timeStampNs: timeNs), from: trackerCapturer)
            return
        }
        guard admit(timeNs, filter: current) else { return }
        let filtered = current.filter(buffer, rotation: rotation, timeNs: timeNs, tracking: tracking)
        deliver(RTCVideoFrame(buffer: RTCCVPixelBuffer(pixelBuffer: filtered), rotation: rotation, timeStampNs: timeNs), from: trackerCapturer)
    }

    private func admit(_ timeNs: Int64, filter: NativeFrameFilter) -> Bool {
        lock.lock()
        defer { lock.unlock() }
        let wanted = max(1, request.fps)
        guard filter.active else {
            if scale != 1 { adapt(1, fps: wanted) }
            return true
        }
        let thermal = CallFaceThermal(rawValue: ProcessInfo.processInfo.thermalState.rawValue) ?? .nominal
        let cap = CallFaceQuality.capture(thermal, fps: wanted)
        if cap.scale != scale { adapt(cap.scale, fps: cap.fps) }
        return gate.admit(timeNs: timeNs, fps: cap.fps)
    }

    private func adapt(_ next: Double, fps: Int) {
        scale = next
        let width = Int32(Double(request.width) * next)
        let height = Int32(Double(request.height) * next)
        let source = self.source
        DispatchQueue.main.async { source.adaptOutputFormat(toWidth: width, height: height, fps: Int32(fps)) }
    }

    func setFilter(_ next: NativeFrameFilter?) {
        lock.lock()
        filter = next
        lock.unlock()
    }

    func start(width: Int, height: Int, fps: Int) async -> Bool {
        let begin: Bool? = lock.withLock {
            if running { return nil }
            request = (width, height, fps)
            running = true
            gate.reset()
            let wants = (filter?.wantsFaceTracking ?? false) && !trackingFailed
            if wants { switching = true }
            return wants
        }
        guard let tryTracking = begin else { return true }
        if tryTracking, await MainActor.run(body: { self.trackingAllowed() }) {
            await MainActor.run { self.beginTracking() }
            return true
        }
        lock.withLock { switching = false }
        return await startCamera()
    }

    private func startCamera() async -> Bool {
        let (wanted, live) = lock.withLock { (request, running) }
        guard live else { return false }
        guard let device = RTCCameraVideoCapturer.captureDevices().first(where: { $0.position == .front })
            ?? RTCCameraVideoCapturer.captureDevices().first
        else { return false }
        let formats = RTCCameraVideoCapturer.supportedFormats(for: device)
        let format = formats.min { a, b in
            let da = CMVideoFormatDescriptionGetDimensions(a.formatDescription)
            let db = CMVideoFormatDescriptionGetDimensions(b.formatDescription)
            return abs(Int(da.width) - wanted.width) + abs(Int(da.height) - wanted.height)
                < abs(Int(db.width) - wanted.width) + abs(Int(db.height) - wanted.height)
        }
        guard let format else { return false }
        let maxRate = format.videoSupportedFrameRateRanges.map { $0.maxFrameRate }.max() ?? Double(wanted.fps)
        let made = RTCCameraVideoCapturer(delegate: self)
        lock.withLock {
            capturer = made
            feed = .camera
        }
        return await withCheckedContinuation { (continuation: CheckedContinuation<Bool, Never>) in
            made.startCapture(with: device, format: format, fps: min(wanted.fps, Int(maxRate))) { error in
                continuation.resume(returning: error == nil)
            }
        }
    }

    @MainActor
    private func trackingAllowed() -> Bool {
        NativeFaceTrackingSource.supported && !EyeTracker.shared.running && AVCaptureDevice.authorizationStatus(for: .video) == .authorized
    }

    private func considerTracking() {
        lock.lock()
        let go = running && feed == .camera && !switching && !trackingFailed
        if go { switching = true }
        lock.unlock()
        guard go else { return }
        Task { @MainActor in self.switchToTracking() }
    }

    private func considerCamera() {
        lock.lock()
        let go = running && feed == .tracking && !switching
        if go { switching = true }
        lock.unlock()
        guard go else { return }
        Task { @MainActor in self.switchToCamera() }
    }

    @MainActor
    private func switchToTracking() {
        guard trackingAllowed() else {
            lock.lock()
            trackingFailed = true
            switching = false
            lock.unlock()
            return
        }
        lock.lock()
        let camera = capturer
        capturer = nil
        lock.unlock()
        guard let camera else {
            beginTracking()
            return
        }
        camera.stopCapture { [weak self] in
            DispatchQueue.main.async { MainActor.assumeIsolated { self?.beginTracking() } }
        }
    }

    @MainActor
    private func beginTracking() {
        lock.lock()
        let live = running
        let wanted = request
        lock.unlock()
        guard live else {
            lock.lock()
            switching = false
            lock.unlock()
            return
        }
        let made = NativeFaceTrackingSource()
        made.onFrame = { [weak self] buffer, rotation, time, tracking in
            self?.tracked(buffer, rotation: rotation, timeNs: time, tracking: tracking)
        }
        made.onFailure = { [weak self, weak made] in
            DispatchQueue.main.async {
                MainActor.assumeIsolated {
                    if let made { self?.trackingBroke(made) }
                }
            }
        }
        lock.lock()
        tracker = made
        trackerFrames = 0
        feed = .tracking
        switching = false
        lock.unlock()
        guard made.start(width: wanted.width, height: wanted.height, fps: wanted.fps) else {
            trackingBroke(made)
            return
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + CallFaceCapture.startupMs / 1000) { [weak self] in
            MainActor.assumeIsolated {
                guard let self else { return }
                self.lock.lock()
                let silent = self.tracker === made && self.trackerFrames == 0
                self.lock.unlock()
                if silent { self.trackingBroke(made) }
            }
        }
    }

    @MainActor
    private func trackingBroke(_ broken: NativeFaceTrackingSource) {
        lock.lock()
        guard tracker === broken else {
            lock.unlock()
            return
        }
        trackingFailed = true
        switching = true
        lock.unlock()
        switchToCamera()
    }

    @MainActor
    private func switchToCamera() {
        lock.lock()
        let stopping = tracker
        tracker = nil
        feed = .off
        lock.unlock()
        stopping?.stop()
        DispatchQueue.main.asyncAfter(deadline: .now() + CallFaceCapture.restartDelayMs / 1000) { [weak self] in
            guard let self else { return }
            Task {
                _ = await self.startCamera()
                self.lock.withLock { self.switching = false }
            }
        }
    }

    func stop() {
        lock.lock()
        guard running else {
            lock.unlock()
            return
        }
        running = false
        feed = .off
        switching = false
        let camera = capturer
        capturer = nil
        let stopping = tracker
        tracker = nil
        lock.unlock()
        camera?.stopCapture()
        guard let stopping else { return }
        if Thread.isMainThread {
            MainActor.assumeIsolated { stopping.stop() }
        } else {
            DispatchQueue.main.async { MainActor.assumeIsolated { stopping.stop() } }
        }
    }
}

@MainActor
final class NativeCallMedia {
    static let shared = NativeCallMedia()

    let factory: RTCPeerConnectionFactory
    let voice = NativeVoicePipeline()
    let lookFilter = NativeLookFilter()
    private(set) var callKitOwnsAudio = false
    var onCallKitAudio: ((Bool) -> Void)?
    private var audioUsers = 0
    private var lookToken: UUID?

    private init() {
        RTCInitializeSSL()
        factory = RTCFmFactory.factory(with: voice.capture)
        let session = RTCAudioSession.sharedInstance()
        session.useManualAudio = true
        session.isAudioEnabled = false
        let configuration = RTCAudioSessionConfiguration.webRTC()
        configuration.category = AVAudioSession.Category.playAndRecord.rawValue
        configuration.mode = AVAudioSession.Mode.voiceChat.rawValue
        configuration.categoryOptions = [.allowBluetooth, .allowBluetoothA2DP, .duckOthers]
        RTCAudioSessionConfiguration.setWebRTC(configuration)
    }

    func follow(_ look: FaceChangerLook) {
        if let lookToken { look.removeListener(lookToken) }
        voice.setVoice(look.effectiveVoice)
        lookFilter.apply(background: look.background, customImage: look.customBackgroundImage)
        lookFilter.apply(face: look.face, customImage: look.customFaceImage)
        lookToken = look.addListener { [weak self, weak look] (_: FaceChangerPart) in
            guard let self, let look else { return }
            self.voice.setVoice(look.effectiveVoice)
            self.lookFilter.apply(background: look.background, customImage: look.customBackgroundImage)
            self.lookFilter.apply(face: look.face, customImage: look.customFaceImage)
        }
    }

    func makeAudioTrack() -> RTCAudioTrack {
        let constraints = RTCMediaConstraints(
            mandatoryConstraints: nil,
            optionalConstraints: [
                "googEchoCancellation": "true", "googAutoGainControl": "true", "googNoiseSuppression": "true",
                "googHighpassFilter": "true",
            ])
        let source = factory.audioSource(with: constraints)
        return factory.audioTrack(with: source, trackId: "audio-" + UUID().uuidString.lowercased())
    }

    func makeVideo() -> (RTCVideoTrack, NativeCameraPipeline) {
        let source = factory.videoSource()
        let track = factory.videoTrack(with: source, trackId: "video-" + UUID().uuidString.lowercased())
        let pipeline = NativeCameraPipeline(source: source)
        pipeline.setFilter(lookFilter)
        return (track, pipeline)
    }

    func beginAudio() {
        audioUsers += 1
        guard !callKitOwnsAudio else { return }
        let session = RTCAudioSession.sharedInstance()
        session.lockForConfiguration()
        try? session.setConfiguration(RTCAudioSessionConfiguration.webRTC(), active: true)
        session.unlockForConfiguration()
        session.isAudioEnabled = true
    }

    func endAudio() {
        audioUsers = max(0, audioUsers - 1)
        guard audioUsers == 0, !callKitOwnsAudio else { return }
        let session = RTCAudioSession.sharedInstance()
        session.isAudioEnabled = false
        session.lockForConfiguration()
        try? session.setActive(false)
        session.unlockForConfiguration()
    }

    func callKitActivated(_ audioSession: AVAudioSession) {
        callKitOwnsAudio = true
        let session = RTCAudioSession.sharedInstance()
        session.audioSessionDidActivate(audioSession)
        session.isAudioEnabled = true
        onCallKitAudio?(true)
    }

    func callKitDeactivated(_ audioSession: AVAudioSession) {
        let session = RTCAudioSession.sharedInstance()
        session.audioSessionDidDeactivate(audioSession)
        session.isAudioEnabled = false
        callKitOwnsAudio = false
        onCallKitAudio?(false)
    }

    static func iceServers(_ urls: [String]) -> [RTCIceServer] {
        urls.isEmpty ? [] : [RTCIceServer(urlStrings: urls)]
    }

    func configuration(iceServers: [String]) -> RTCConfiguration {
        let configuration = RTCConfiguration()
        configuration.iceServers = Self.iceServers(iceServers)
        configuration.sdpSemantics = .unifiedPlan
        configuration.bundlePolicy = .maxBundle
        configuration.rtcpMuxPolicy = .require
        configuration.continualGatheringPolicy = .gatherContinually
        return configuration
    }

    static func capSenders(_ connection: RTCPeerConnection, videoBitrate: Int) {
        for sender in connection.senders {
            guard let kind = sender.track?.kind else { continue }
            let parameters = sender.parameters
            for encoding in parameters.encodings {
                encoding.maxBitrateBps = NSNumber(
                    value: kind == kRTCMediaStreamTrackKindVideo ? videoBitrate : CallMediaTuning.audioMaxBitrate)
                encoding.networkPriority = .high
            }
            sender.parameters = parameters
        }
    }

    static func readRoute(_ connection: RTCPeerConnection) async -> DirectCallRoute? {
        await withCheckedContinuation { (continuation: CheckedContinuation<DirectCallRoute?, Never>) in
            connection.statistics { report in
                let stats = report.statistics
                var pair: RTCStatistics?
                for entry in stats.values where entry.type == "transport" {
                    if let id = entry.values["selectedCandidatePairId"] as? String { pair = stats[id] }
                }
                if pair == nil {
                    pair = stats.values.first { entry in
                        entry.type == "candidate-pair" && (entry.values["state"] as? String) == "succeeded"
                            && ((entry.values["nominated"] as? NSNumber)?.boolValue ?? false)
                    }
                }
                guard let pair else {
                    continuation.resume(returning: nil)
                    return
                }
                let local = (pair.values["localCandidateId"] as? String).flatMap { stats[$0] }
                let remote = (pair.values["remoteCandidateId"] as? String).flatMap { stats[$0] }
                let localType = local?.values["candidateType"] as? String ?? ""
                let remoteType = remote?.values["candidateType"] as? String ?? ""
                continuation.resume(returning: localType == "relay" || remoteType == "relay" ? .relay : .direct)
            }
        }
    }
}
#endif
