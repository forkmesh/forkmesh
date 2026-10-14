import AVFoundation
import CoreImage
import FMBase
import FMChat
import Foundation
import ImageIO
import UniformTypeIdentifiers

final class CallDepthCamera: NSObject, AVCaptureDataOutputSynchronizerDelegate, @unchecked Sendable {
    static let side: CGFloat = 480
    static let quality: CGFloat = 0.75
    static let orientation: CallDepthOrientation = .right
    static let staleNanos: UInt64 = 2_000_000_000

    private let session = AVCaptureSession()
    private let queue = DispatchQueue(label: "com.forkmesh.call-depth-camera", qos: .userInteractive)
    private let video = AVCaptureVideoDataOutput()
    private let depth = AVCaptureDepthDataOutput()
    private let context = CIContext(options: [.cacheIntermediates: false])
    private let lock = NSLock()
    private var synchronizer: AVCaptureDataOutputSynchronizer?
    private var device: AVCaptureDevice?
    private var configured = false
    private var observers: [NSObjectProtocol] = []
    private var busySince: UInt64?
    private var wantLandmarks = false
    private var wantMask = false
    private var running = false

    var onFrame: (@Sendable (String) -> Void)?
    var onFailure: (@Sendable (String) -> Void)?

    static var trueDepth: AVCaptureDevice? {
        AVCaptureDevice.default(.builtInTrueDepthCamera, for: .video, position: .front)
    }

    static var available: Bool {
        guard trueDepth != nil else { return false }
        let status = AVCaptureDevice.authorizationStatus(for: .video)
        return status == .authorized || status == .notDetermined
    }

    deinit {
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        if session.isRunning { session.stopRunning() }
    }

    func start(landmarks: Bool, mask: Bool, _ done: @escaping @Sendable (String?) -> Void) {
        want(landmarks: landmarks, mask: mask)
        switch AVCaptureDevice.authorizationStatus(for: .video) {
        case .authorized:
            queue.async { done(self.run()) }
        case .notDetermined:
            AVCaptureDevice.requestAccess(for: .video) { granted in
                guard granted else {
                    done("denied")
                    return
                }
                self.queue.async { done(self.run()) }
            }
        default:
            done("denied")
        }
    }

    func stop(_ done: (@Sendable () -> Void)? = nil) {
        queue.async {
            self.setRunning(false)
            if self.session.isRunning { self.session.stopRunning() }
            done?()
        }
    }

    func frameDone(landmarks: Bool, mask: Bool) {
        lock.lock()
        busySince = nil
        wantLandmarks = landmarks
        wantMask = mask
        lock.unlock()
    }

    private func want(landmarks: Bool, mask: Bool) {
        lock.lock()
        wantLandmarks = landmarks
        wantMask = mask
        busySince = nil
        lock.unlock()
    }

    private func setRunning(_ on: Bool) {
        lock.lock()
        running = on
        busySince = nil
        lock.unlock()
    }

    private func claim() -> (landmarks: Bool, mask: Bool)? {
        lock.lock()
        defer { lock.unlock() }
        guard running else { return nil }
        let now = DispatchTime.now().uptimeNanoseconds
        if let since = busySince, now - since < CallDepthCamera.staleNanos { return nil }
        busySince = now
        return (wantLandmarks, wantMask)
    }

    private func release() {
        lock.lock()
        busySince = nil
        lock.unlock()
    }

    private func run() -> String? {
        if !configured {
            if let failure = configure() { return failure }
            configured = true
        }
        if !session.isRunning { session.startRunning() }
        guard session.isRunning else { return "failed" }
        setRunning(true)
        return nil
    }

    private func configure() -> String? {
        guard let device = CallDepthCamera.trueDepth else { return "no_camera" }
        guard let input = try? AVCaptureDeviceInput(device: device) else { return "failed" }
        session.beginConfiguration()
        defer { session.commitConfiguration() }
        session.automaticallyConfiguresApplicationAudioSession = false
        if session.canSetSessionPreset(.vga640x480) { session.sessionPreset = .vga640x480 }
        guard session.canAddInput(input) else { return "failed" }
        session.addInput(input)
        video.alwaysDiscardsLateVideoFrames = true
        video.videoSettings = [kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA]
        guard session.canAddOutput(video) else { return "failed" }
        session.addOutput(video)
        guard session.canAddOutput(depth) else { return "no_depth" }
        session.addOutput(depth)
        depth.isFilteringEnabled = true
        depth.alwaysDiscardsLateDepthData = true
        for connection in [video.connection(with: .video), depth.connection(with: .depthData)].compactMap({ $0 }) {
            connection.isEnabled = true
            guard connection.isVideoMirroringSupported else { continue }
            connection.automaticallyAdjustsVideoMirroring = false
            connection.isVideoMirrored = false
        }
        guard (try? device.lockForConfiguration()) != nil else { return "failed" }
        let formats = device.activeFormat.supportedDepthDataFormats.filter {
            CMFormatDescriptionGetMediaSubType($0.formatDescription) == kCVPixelFormatType_DepthFloat32
        }
        if let best = formats.max(by: {
            CMVideoFormatDescriptionGetDimensions($0.formatDescription).width
                < CMVideoFormatDescriptionGetDimensions($1.formatDescription).width
        }) {
            device.activeDepthDataFormat = best
        }
        let ranges = device.activeFormat.videoSupportedFrameRateRanges
        if ranges.contains(where: { $0.maxFrameRate >= 30 }) {
            device.activeVideoMinFrameDuration = CMTime(value: 1, timescale: 30)
        }
        if ranges.contains(where: { $0.minFrameRate <= 24 && $0.maxFrameRate >= 24 }) {
            device.activeVideoMaxFrameDuration = CMTime(value: 1, timescale: 24)
        }
        device.unlockForConfiguration()
        self.device = device
        let sync = AVCaptureDataOutputSynchronizer(dataOutputs: [video, depth])
        sync.setDelegate(self, queue: queue)
        synchronizer = sync
        observe()
        return nil
    }

    private func observe() {
        let center = NotificationCenter.default
        observers.append(
            center.addObserver(forName: .AVCaptureSessionRuntimeError, object: session, queue: nil) { [weak self] _ in
                self?.fail("session")
            })
        observers.append(
            center.addObserver(forName: .AVCaptureSessionWasInterrupted, object: session, queue: nil) { [weak self] note in
                let raw = (note.userInfo?[AVCaptureSessionInterruptionReasonKey] as? NSNumber)?.intValue ?? 0
                guard AVCaptureSession.InterruptionReason(rawValue: raw) != .videoDeviceNotAvailableInBackground else { return }
                self?.fail("interrupted")
            })
    }

    private func fail(_ reason: String) {
        lock.lock()
        let wasRunning = running
        running = false
        lock.unlock()
        guard wasRunning else { return }
        onFailure?(reason)
    }

    func dataOutputSynchronizer(
        _ synchronizer: AVCaptureDataOutputSynchronizer, didOutput synchronizedDataCollection: AVCaptureSynchronizedDataCollection
    ) {
        guard let frame = synchronizedDataCollection.synchronizedData(for: video) as? AVCaptureSynchronizedSampleBufferData,
            !frame.sampleBufferWasDropped, let pixels = CMSampleBufferGetImageBuffer(frame.sampleBuffer),
            let wanted = claim()
        else { return }
        var sensed: AVDepthData?
        if let data = synchronizedDataCollection.synchronizedData(for: depth) as? AVCaptureSynchronizedDepthData,
            !data.depthDataWasDropped
        {
            sensed = data.depthData
        }
        let time = CMSampleBufferGetPresentationTimeStamp(frame.sampleBuffer).seconds * 1000
        guard let payload = encode(pixels, depth: sensed, landmarks: wanted.landmarks, mask: wanted.mask, time: time) else {
            release()
            return
        }
        onFrame?(payload)
    }

    private func encode(_ pixels: CVPixelBuffer, depth sensed: AVDepthData?, landmarks: Bool, mask: Bool, time: Double) -> String? {
        let oriented = CIImage(cvPixelBuffer: pixels).oriented(CGImagePropertyOrientation(rawValue: UInt32(CallDepthCamera.orientation.rawValue)) ?? .right)
        let scale = min(1, CallDepthCamera.side / max(oriented.extent.width, oriented.extent.height))
        let upright = oriented
            .transformed(by: CGAffineTransform(translationX: -oriented.extent.minX, y: -oriented.extent.minY))
            .transformed(by: CGAffineTransform(scaleX: scale, y: scale))
        let width = Int(upright.extent.width.rounded(.down))
        let height = Int(upright.extent.height.rounded(.down))
        guard width > 0, height > 0,
            let image = context.createCGImage(upright, from: CGRect(x: 0, y: 0, width: width, height: height)),
            let jpeg = CallDepthCamera.jpeg(image)
        else { return nil }
        var payload = JSONObject()
        payload["image"] = .string("data:image/jpeg;base64," + jpeg.base64EncodedString())
        payload["width"] = .int(Int64(width))
        payload["height"] = .int(Int64(height))
        payload["time"] = .double(time.isFinite ? time : 0)
        payload["regions"] = .null
        if let analysis = CallFaceLandmarks.analyze(image: image, landmarks: landmarks, mask: mask, accurate: false) {
            let found = analysis.regions
            let carved = found.flatMap { regions in sensed.flatMap { relief(regions, depth: $0, width: width, height: height) } }
            payload["regions"] = CallFaceLandmarks.json(carved ?? found)
            if let maskValue = analysis.mask { payload["mask"] = maskValue }
        }
        return JSON.encode(.object(payload))
    }

    private func relief(_ regions: [String: [[Double]]], depth sensed: AVDepthData, width: Int, height: Int) -> [String: [[Double]]]? {
        let metres = sensed.depthDataType == kCVPixelFormatType_DepthFloat32
            ? sensed : sensed.converting(toDepthDataType: kCVPixelFormatType_DepthFloat32)
        guard let field = CallDepthCamera.field(metres.depthDataMap) else { return nil }
        let sensorWidth = Double(CallDepthCamera.orientation.swapsAxes ? height : width)
        guard let focal = focal(metres, sensorWidth: sensorWidth) else { return nil }
        return CallFaceDepth.relief(regions, field: field, width: Double(width), height: Double(height), focal: focal)
    }

    private func focal(_ sensed: AVDepthData, sensorWidth: Double) -> Double? {
        if let calibration = sensed.cameraCalibrationData,
            let found = CallFaceDepth.focalPixels(
                fx: Double(calibration.intrinsicMatrix.columns.0.x),
                referenceWidth: Double(calibration.intrinsicMatrixReferenceDimensions.width), sensorWidth: sensorWidth)
        {
            return found
        }
        guard let device else { return nil }
        return CallFaceDepth.focalPixels(fieldOfViewDegrees: Double(device.activeFormat.videoFieldOfView), sensorWidth: sensorWidth)
    }

    static func field(_ buffer: CVPixelBuffer) -> CallDepthField? {
        guard CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_DepthFloat32 else { return nil }
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { return nil }
        let width = CVPixelBufferGetWidth(buffer)
        let height = CVPixelBufferGetHeight(buffer)
        let stride = CVPixelBufferGetBytesPerRow(buffer)
        var values = [Float](repeating: 0, count: width * height)
        values.withUnsafeMutableBufferPointer { out in
            for y in 0..<height {
                let row = base.advanced(by: y * stride).assumingMemoryBound(to: Float32.self)
                for x in 0..<width { out[y * width + x] = row[x] }
            }
        }
        return CallDepthField(width: width, height: height, values: values, orientation: orientation)
    }

    static func jpeg(_ image: CGImage) -> Data? {
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data as CFMutableData, UTType.jpeg.identifier as CFString, 1, nil)
        else { return nil }
        CGImageDestinationAddImage(destination, image, [kCGImageDestinationLossyCompressionQuality: quality] as CFDictionary)
        guard CGImageDestinationFinalize(destination) else { return nil }
        return data as Data
    }
}
