import AVFoundation
import CoreVideo
import FMAssistant
import Foundation
import ImageIO
import Vision

enum GenieCameraFailure: Equatable {
    case denied
    case noCamera
    case failed
}

struct GenieDepthField {
    let buffer: CVPixelBuffer

    init?(_ depth: AVDepthData) {
        let metres: AVDepthData = depth.depthDataType == kCVPixelFormatType_DepthFloat32
            ? depth : depth.converting(toDepthDataType: kCVPixelFormatType_DepthFloat32)
        buffer = metres.applyingExifOrientation(.leftMirrored).depthDataMap
    }

    func metres(at points: [CGPoint]) -> Double? {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { return nil }
        let width: Int = CVPixelBufferGetWidth(buffer)
        let height: Int = CVPixelBufferGetHeight(buffer)
        let row: Int = CVPixelBufferGetBytesPerRow(buffer)
        var found: [Float] = []
        for point in points {
            let cx: Int = Int(point.x * CGFloat(width))
            let cy: Int = Int((1 - point.y) * CGFloat(height))
            for dy in -1...1 {
                for dx in -1...1 {
                    let x: Int = cx + dx * 2
                    let y: Int = cy + dy * 2
                    guard x >= 0, y >= 0, x < width, y < height else { continue }
                    let value: Float = base.advanced(by: y * row).assumingMemoryBound(to: Float32.self)[x]
                    if value.isFinite, value > 0.05, value < 2 { found.append(value) }
                }
            }
        }
        guard !found.isEmpty else { return nil }
        return Double(found.sorted()[found.count / 2])
    }
}

final class GenieDepthCamera: NSObject, AVCaptureDataOutputSynchronizerDelegate, AVCaptureVideoDataOutputSampleBufferDelegate,
    @unchecked Sendable
{
    static let gain: CGFloat = 1.3

    private let session: AVCaptureSession = AVCaptureSession()
    private let queue: DispatchQueue = DispatchQueue(label: "com.forkmesh.genie.hands", qos: .userInitiated)
    private let video: AVCaptureVideoDataOutput = AVCaptureVideoDataOutput()
    private let depth: AVCaptureDepthDataOutput = AVCaptureDepthDataOutput()
    private var synchronizer: AVCaptureDataOutputSynchronizer?
    private var configured: Bool = false
    private let request: VNDetectHumanHandPoseRequest = {
        let made: VNDetectHumanHandPoseRequest = VNDetectHumanHandPoseRequest()
        made.maximumHandCount = 2
        return made
    }()

    var onHands: (@Sendable ([GenieHandPose], Date) -> Void)?

    static var trueDepth: AVCaptureDevice? {
        AVCaptureDevice.default(.builtInTrueDepthCamera, for: .video, position: .front)
    }

    static var available: Bool {
        trueDepth != nil || AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front) != nil
    }

    func start(_ done: @escaping @Sendable (GenieCameraFailure?) -> Void) {
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            queue.async { done(self.run()) }
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { (granted: Bool) in
                guard granted else {
                    done(.denied)
                    return
                }
                self.queue.async { done(self.run()) }
            }
        default:
            done(.denied)
        }
    }

    func stop() {
        queue.async {
            if self.session.isRunning { self.session.stopRunning() }
        }
    }

    private func run() -> GenieCameraFailure? {
        if !configured {
            if let failure = configure() { return failure }
            configured = true
        }
        if !session.isRunning { session.startRunning() }
        return session.isRunning ? nil : .failed
    }

    private func configure() -> GenieCameraFailure? {
        let sensor: AVCaptureDevice? = GenieDepthCamera.trueDepth
        guard let device = sensor ?? AVCaptureDevice.default(.builtInWideAngleCamera, for: .video, position: .front) else { return .noCamera }
        guard let input = try? AVCaptureDeviceInput(device: device) else { return .failed }
        session.beginConfiguration()
        defer { session.commitConfiguration() }
        if session.canSetSessionPreset(.vga640x480) { session.sessionPreset = .vga640x480 }
        guard session.canAddInput(input) else { return .failed }
        session.addInput(input)
        video.alwaysDiscardsLateVideoFrames = true
        video.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange]
        guard session.canAddOutput(video) else { return .failed }
        session.addOutput(video)
        if sensor != nil, session.canAddOutput(depth) {
            session.addOutput(depth)
            depth.isFilteringEnabled = true
            depth.alwaysDiscardsLateDepthData = true
            depth.connection(with: .depthData)?.isEnabled = true
            let formats: [AVCaptureDevice.Format] = device.activeFormat.supportedDepthDataFormats.filter {
                CMFormatDescriptionGetMediaSubType($0.formatDescription) == kCVPixelFormatType_DepthFloat32
            }
            if let best = formats.max(by: {
                CMVideoFormatDescriptionGetDimensions($0.formatDescription).width < CMVideoFormatDescriptionGetDimensions($1.formatDescription).width
            }), (try? device.lockForConfiguration()) != nil {
                device.activeDepthDataFormat = best
                device.unlockForConfiguration()
            }
            let sync: AVCaptureDataOutputSynchronizer = AVCaptureDataOutputSynchronizer(dataOutputs: [video, depth])
            sync.setDelegate(self, queue: queue)
            synchronizer = sync
        } else {
            video.setSampleBufferDelegate(self, queue: queue)
        }
        return nil
    }

    func dataOutputSynchronizer(
        _ synchronizer: AVCaptureDataOutputSynchronizer, didOutput synchronizedDataCollection: AVCaptureSynchronizedDataCollection
    ) {
        guard let frame = synchronizedDataCollection.synchronizedData(for: video) as? AVCaptureSynchronizedSampleBufferData,
              !frame.sampleBufferWasDropped, let pixels = CMSampleBufferGetImageBuffer(frame.sampleBuffer) else { return }
        var field: GenieDepthField?
        if let sensed = synchronizedDataCollection.synchronizedData(for: depth) as? AVCaptureSynchronizedDepthData, !sensed.depthDataWasDropped {
            field = GenieDepthField(sensed.depthData)
        }
        detect(pixels, depth: field)
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        guard let pixels = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }
        detect(pixels, depth: nil)
    }

    private func detect(_ pixels: CVPixelBuffer, depth field: GenieDepthField?) {
        let handler: VNImageRequestHandler = VNImageRequestHandler(cvPixelBuffer: pixels, orientation: .leftMirrored, options: [:])
        guard (try? handler.perform([request])) != nil else { return }
        let hands: [GenieHandPose] = (request.results ?? []).compactMap { GenieDepthCamera.pose($0, depth: field) }
        onHands?(hands, Date())
    }

    private static let joints: [(GenieJoint, VNHumanHandPoseObservation.JointName)] = [
        (.wrist, .wrist), (.thumbTip, .thumbTip), (.thumbKnuckle, .thumbMP), (.indexTip, .indexTip), (.indexKnuckle, .indexMCP),
        (.middleTip, .middleTip), (.middleKnuckle, .middleMCP), (.ringTip, .ringTip), (.ringKnuckle, .ringMCP),
        (.littleTip, .littleTip), (.littleKnuckle, .littleMCP),
    ]

    static func screen(_ location: CGPoint) -> CGPoint {
        CGPoint(x: 0.5 + (location.x - 0.5) * gain, y: 0.5 + (0.5 - location.y) * gain)
    }

    private static func pose(_ observation: VNHumanHandPoseObservation, depth field: GenieDepthField?) -> GenieHandPose? {
        guard let found = try? observation.recognizedPoints(.all) else { return nil }
        var mapped: [GenieJoint: CGPoint] = [:]
        var raw: [GenieJoint: CGPoint] = [:]
        var trust: Double = 0
        for (joint, name) in joints {
            guard let point = found[name], point.confidence > 0.3 else { continue }
            raw[joint] = point.location
            mapped[joint] = screen(point.location)
            trust += Double(point.confidence)
        }
        guard mapped[.wrist] != nil, mapped[.indexTip] != nil, mapped[.thumbTip] != nil, mapped[.middleKnuckle] != nil, mapped.count >= 8 else {
            return nil
        }
        let palm: [CGPoint] = [GenieJoint.wrist, .indexKnuckle, .middleKnuckle, .ringKnuckle].compactMap { raw[$0] }
        let metres: Double? = field?.metres(at: palm)
        return GenieHandPose(joints: mapped, depth: metres, confidence: trust / Double(joints.count))
    }
}
