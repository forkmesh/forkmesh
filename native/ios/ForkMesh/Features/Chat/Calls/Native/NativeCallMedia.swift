#if canImport(WebRTC)
import AVFoundation
import FMChat
import Foundation
import WebRTC

protocol NativeFrameFilter: AnyObject {
    func filter(_ buffer: CVPixelBuffer, rotation: RTCVideoRotation, timeNs: Int64) -> CVPixelBuffer
}

final class NativeCameraPipeline: NSObject, RTCVideoCapturerDelegate {
    let source: RTCVideoSource
    private(set) var capturer: RTCCameraVideoCapturer?
    weak var filter: NativeFrameFilter?
    private let lock = NSLock()
    private var running = false

    init(source: RTCVideoSource) {
        self.source = source
        super.init()
    }

    func capturer(_ capturer: RTCVideoCapturer, didCapture frame: RTCVideoFrame) {
        lock.lock()
        let current = filter
        lock.unlock()
        guard let current, let buffer = frame.buffer as? RTCCVPixelBuffer else {
            source.capturer(capturer, didCapture: frame)
            return
        }
        let filtered = current.filter(buffer.pixelBuffer, rotation: frame.rotation, timeNs: frame.timeStampNs)
        let out = RTCVideoFrame(buffer: RTCCVPixelBuffer(pixelBuffer: filtered), rotation: frame.rotation, timeStampNs: frame.timeStampNs)
        source.capturer(capturer, didCapture: out)
    }

    func setFilter(_ next: NativeFrameFilter?) {
        lock.lock()
        filter = next
        lock.unlock()
    }

    func start(width: Int, height: Int, fps: Int) async -> Bool {
        if running { return true }
        guard let device = RTCCameraVideoCapturer.captureDevices().first(where: { $0.position == .front })
            ?? RTCCameraVideoCapturer.captureDevices().first
        else { return false }
        let formats = RTCCameraVideoCapturer.supportedFormats(for: device)
        let format = formats.min { a, b in
            let da = CMVideoFormatDescriptionGetDimensions(a.formatDescription)
            let db = CMVideoFormatDescriptionGetDimensions(b.formatDescription)
            return abs(Int(da.width) - width) + abs(Int(da.height) - height)
                < abs(Int(db.width) - width) + abs(Int(db.height) - height)
        }
        guard let format else { return false }
        let maxRate = format.videoSupportedFrameRateRanges.map { $0.maxFrameRate }.max() ?? Double(fps)
        let made = RTCCameraVideoCapturer(delegate: self)
        capturer = made
        running = true
        return await withCheckedContinuation { (continuation: CheckedContinuation<Bool, Never>) in
            made.startCapture(with: device, format: format, fps: min(fps, Int(maxRate))) { error in
                continuation.resume(returning: error == nil)
            }
        }
    }

    func stop() {
        guard running, let capturer else { return }
        running = false
        capturer.stopCapture()
        self.capturer = nil
    }
}

@MainActor
final class NativeCallMedia {
    static let shared = NativeCallMedia()

    let factory: RTCPeerConnectionFactory
    let voice = NativeVoicePipeline()
    let lookFilter = NativeLookFilter()
    private var callKitOwnsAudio = false
    private var audioUsers = 0
    private var lookToken: UUID?

    private init() {
        RTCInitializeSSL()
        factory = RTCFmFactory.factory(withCaptureProcessor: voice.capture)
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
        lookToken = look.addListener { [weak self, weak look] (_: FaceChangerPart) in
            guard let self, let look else { return }
            self.voice.setVoice(look.effectiveVoice)
            self.lookFilter.apply(background: look.background, customImage: look.customBackgroundImage)
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
    }

    func callKitDeactivated(_ audioSession: AVAudioSession) {
        let session = RTCAudioSession.sharedInstance()
        session.audioSessionDidDeactivate(audioSession)
        session.isAudioEnabled = false
        callKitOwnsAudio = false
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
