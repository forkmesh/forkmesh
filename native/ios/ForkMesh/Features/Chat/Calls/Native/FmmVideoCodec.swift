#if canImport(Fmm)
import CoreImage
import CoreMedia
import CoreVideo
import FMChat
import Foundation
import VideoToolbox

struct FmmEncodedFrame {
    let data: Data
    let keyframe: Bool
    let layer: Int
    let ltrToken: Int64?
    let recovery: Int64?
}

struct FmmEncoderAbilities: Equatable {
    var lowLatency = false
    var temporalLayers = false
    var longTermRefs = false
}

final class FmmVideoEncoder: @unchecked Sendable {
    let width: Int
    let height: Int
    let fps: Int
    private(set) var abilities = FmmEncoderAbilities()
    private var session: VTCompressionSession?
    private let lock = NSLock()
    private var refs = GroupCallLongTermRefs()
    private var acked: [Int64] = []
    private var ackChanged = false
    private var wantKey = true
    private var recovering: (frame: Int64, token: Int64)?
    private var bitrate = 0
    private var frameIndex: Int64 = 0
    var onFrame: ((FmmEncodedFrame) -> Int64?)?

    init?(width: Int, height: Int, fps: Int, bitrate: Int, layers: Int) {
        self.width = width
        self.height = height
        self.fps = fps
        var made: VTCompressionSession?
        let lowLatency = [kVTVideoEncoderSpecification_EnableLowLatencyRateControl: kCFBooleanTrue] as CFDictionary
        var status = VTCompressionSessionCreate(
            allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_H264,
            encoderSpecification: lowLatency, imageBufferAttributes: nil, compressedDataAllocator: nil,
            outputCallback: nil, refcon: nil, compressionSessionOut: &made)
        abilities.lowLatency = status == noErr
        if status != noErr {
            status = VTCompressionSessionCreate(
                allocator: nil, width: Int32(width), height: Int32(height), codecType: kCMVideoCodecType_H264,
                encoderSpecification: nil, imageBufferAttributes: nil, compressedDataAllocator: nil,
                outputCallback: nil, refcon: nil, compressionSessionOut: &made)
        }
        guard status == noErr, let made else { return nil }
        session = made
        set(kVTCompressionPropertyKey_RealTime, kCFBooleanTrue)
        set(kVTCompressionPropertyKey_AllowFrameReordering, kCFBooleanFalse)
        set(kVTCompressionPropertyKey_ProfileLevel, kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel)
        set(kVTCompressionPropertyKey_ExpectedFrameRate, fps as CFNumber)
        set(kVTCompressionPropertyKey_MaxKeyFrameInterval, (fps * GroupCallVideoLayer.keyframeEverySeconds) as CFNumber)
        set(kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, GroupCallVideoLayer.keyframeEverySeconds as CFNumber)
        if layers > 1 {
            abilities.temporalLayers = set(kVTCompressionPropertyKey_BaseLayerFrameRateFraction, 0.5 as CFNumber)
        }
        abilities.longTermRefs = set(kVTCompressionPropertyKey_EnableLTR, kCFBooleanTrue)
        setBitrate(bitrate)
        VTCompressionSessionPrepareToEncodeFrames(made)
    }

    deinit { invalidate() }

    func invalidate() {
        lock.lock()
        let current = session
        session = nil
        lock.unlock()
        if let current {
            VTCompressionSessionCompleteFrames(current, untilPresentationTimeStamp: .invalid)
            VTCompressionSessionInvalidate(current)
        }
    }

    @discardableResult
    private func set(_ key: CFString, _ value: CFTypeRef) -> Bool {
        guard let session else { return false }
        return VTSessionSetProperty(session, key: key, value: value) == noErr
    }

    func setBitrate(_ next: Int) {
        lock.lock()
        defer { lock.unlock() }
        guard next > 0, abs(next - bitrate) * 10 > bitrate else { return }
        bitrate = next
        set(kVTCompressionPropertyKey_AverageBitRate, next as CFNumber)
        let bytesPerSecond = next / 8 * 3 / 2
        set(kVTCompressionPropertyKey_DataRateLimits, [bytesPerSecond, 1] as CFArray)
    }

    func requestKeyframe() {
        lock.withLock {
            wantKey = true
            recovering = nil
        }
    }

    func recover(from frame: Int64) {
        lock.withLock {
            if abilities.longTermRefs, let token = refs.token(for: frame) {
                recovering = (frame, token)
            } else {
                wantKey = true
            }
        }
    }

    func acknowledge(upTo frame: Int64) {
        lock.withLock {
            let tokens = refs.acknowledged(upTo: frame)
            if tokens != acked {
                acked = tokens
                ackChanged = true
            }
        }
    }

    func encode(_ buffer: CVPixelBuffer, timeNs: Int64) {
        lock.lock()
        guard let session else {
            lock.unlock()
            return
        }
        var options: [CFString: Any] = [:]
        var recovery: Int64?
        if wantKey {
            options[kVTEncodeFrameOptionKey_ForceKeyFrame] = kCFBooleanTrue
            wantKey = false
            refs.reset()
            acked = []
        } else if let pending = recovering {
            options[kVTEncodeFrameOptionKey_ForceLTRRefresh] = kCFBooleanTrue
            options[kVTEncodeFrameOptionKey_AcknowledgedLTRTokens] = [NSNumber(value: pending.token)]
            recovery = pending.frame
            recovering = nil
        } else if ackChanged, abilities.longTermRefs {
            options[kVTEncodeFrameOptionKey_AcknowledgedLTRTokens] = acked.map { NSNumber(value: $0) }
            ackChanged = false
        }
        frameIndex += 1
        lock.unlock()
        let pts = CMTime(value: timeNs / 1000, timescale: 1_000_000)
        VTCompressionSessionEncodeFrame(
            session, imageBuffer: buffer, presentationTimeStamp: pts, duration: .invalid,
            frameProperties: options.isEmpty ? nil : options as CFDictionary, infoFlagsOut: nil
        ) { [weak self] status, _, sample in
            guard status == noErr, let sample, let self else { return }
            self.output(sample, recovery: recovery)
        }
    }

    private func output(_ sample: CMSampleBuffer, recovery: Int64?) {
        let attachments = (CMSampleBufferGetSampleAttachmentsArray(sample, createIfNecessary: false) as? [[CFString: Any]])?.first ?? [:]
        let keyframe = !((attachments[kCMSampleAttachmentKey_NotSync] as? Bool) ?? false)
        let token = (attachments[kVTSampleAttachmentKey_RequireLTRAcknowledgementToken] as? NSNumber)?.int64Value
        guard let block = CMSampleBufferGetDataBuffer(sample) else { return }
        var length = 0
        var pointer: UnsafeMutablePointer<CChar>?
        guard CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &length, dataPointerOut: &pointer) == noErr,
            let pointer
        else { return }
        let avcc = [UInt8](UnsafeRawBufferPointer(start: pointer, count: length))
        var sets: [[UInt8]] = []
        if keyframe, let format = CMSampleBufferGetFormatDescription(sample) { sets = Self.parameterSets(format) }
        let annexB = H264Bitstream.annexB(avcc: avcc, parameterSets: sets)
        let units = H264Bitstream.units(annexB: annexB)
        let slice = units.first { [1, 5].contains(H264Bitstream.type($0)) }
        let referenced = ((slice?.first ?? 0x60) >> 5) & 3 != 0
        let layer = keyframe || referenced || !abilities.temporalLayers ? 0 : 1
        let frame = FmmEncodedFrame(
            data: Data(annexB), keyframe: keyframe, layer: layer, ltrToken: token, recovery: keyframe ? nil : recovery)
        guard let id = onFrame?(frame) else { return }
        if let token {
            lock.withLock { refs.note(frame: id, token: token) }
        }
    }

    static func parameterSets(_ format: CMFormatDescription) -> [[UInt8]] {
        var count = 0
        guard CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
            format, parameterSetIndex: 0, parameterSetPointerOut: nil, parameterSetSizeOut: nil,
            parameterSetCountOut: &count, nalUnitHeaderLengthOut: nil) == noErr
        else { return [] }
        var out: [[UInt8]] = []
        for index in 0..<count {
            var pointer: UnsafePointer<UInt8>?
            var size = 0
            if CMVideoFormatDescriptionGetH264ParameterSetAtIndex(
                format, parameterSetIndex: index, parameterSetPointerOut: &pointer, parameterSetSizeOut: &size,
                parameterSetCountOut: nil, nalUnitHeaderLengthOut: nil) == noErr, let pointer {
                out.append([UInt8](UnsafeBufferPointer(start: pointer, count: size)))
            }
        }
        return out
    }
}

final class FmmVideoDecoder: @unchecked Sendable {
    private var session: VTDecompressionSession?
    private var format: CMVideoFormatDescription?
    private var sets: [[UInt8]] = []
    private let lock = NSLock()
    var onImage: ((CVPixelBuffer) -> Void)?

    deinit { reset() }

    func reset() {
        lock.lock()
        let current = session
        session = nil
        format = nil
        sets = []
        lock.unlock()
        if let current { VTDecompressionSessionInvalidate(current) }
    }

    func decode(_ data: Data) -> Bool {
        let units = H264Bitstream.units(annexB: [UInt8](data))
        let parameters = units.filter { [H264Bitstream.sps, H264Bitstream.pps].contains(H264Bitstream.type($0)) }.map { Array($0) }
        let slices = units.filter { ![H264Bitstream.sps, H264Bitstream.pps, 6, 9].contains(H264Bitstream.type($0)) }
        lock.lock()
        defer { lock.unlock() }
        if parameters.count >= 2, parameters != sets {
            guard makeSession(parameters) else { return false }
        }
        guard let session, let format, !slices.isEmpty else { return false }
        let avcc = H264Bitstream.avcc(slices)
        var block: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault, memoryBlock: nil, blockLength: avcc.count, blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil, offsetToData: 0, dataLength: avcc.count, flags: kCMBlockBufferAssureMemoryNowFlag,
            blockBufferOut: &block) == noErr, let block
        else { return false }
        _ = avcc.withUnsafeBytes { CMBlockBufferReplaceDataBytes(with: $0.baseAddress!, blockBuffer: block, offsetIntoDestination: 0, dataLength: avcc.count) }
        var sample: CMSampleBuffer?
        var size = avcc.count
        guard CMSampleBufferCreateReady(
            allocator: kCFAllocatorDefault, dataBuffer: block, formatDescription: format, sampleCount: 1, sampleTimingEntryCount: 0,
            sampleTimingArray: nil, sampleSizeEntryCount: 1, sampleSizeArray: &size, sampleBufferOut: &sample) == noErr, let sample
        else { return false }
        let status = VTDecompressionSessionDecodeFrame(session, sampleBuffer: sample, flags: [._EnableAsynchronousDecompression], infoFlagsOut: nil) {
            [weak self] status, _, image, _, _ in
            guard status == noErr, let image else { return }
            self?.onImage?(image)
        }
        return status == noErr
    }

    private func makeSession(_ parameters: [[UInt8]]) -> Bool {
        if let session { VTDecompressionSessionInvalidate(session) }
        session = nil
        var made: CMFormatDescription?
        let pointers = parameters.map { set -> UnsafePointer<UInt8> in
            let copy = UnsafeMutablePointer<UInt8>.allocate(capacity: set.count)
            copy.initialize(from: set, count: set.count)
            return UnsafePointer(copy)
        }
        defer { pointers.forEach { UnsafeMutablePointer(mutating: $0).deallocate() } }
        let sizes = parameters.map(\.count)
        guard CMVideoFormatDescriptionCreateFromH264ParameterSets(
            allocator: kCFAllocatorDefault, parameterSetCount: parameters.count, parameterSetPointers: pointers,
            parameterSetSizes: sizes, nalUnitHeaderLength: 4, formatDescriptionOut: &made) == noErr, let made
        else { return false }
        let attributes = [
            kCVPixelBufferPixelFormatTypeKey: kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
            kCVPixelBufferIOSurfacePropertiesKey: [:] as CFDictionary,
        ] as [CFString: Any] as CFDictionary
        var created: VTDecompressionSession?
        guard VTDecompressionSessionCreate(
            allocator: kCFAllocatorDefault, formatDescription: made, decoderSpecification: nil, imageBufferAttributes: attributes,
            outputCallback: nil, decompressionSessionOut: &created) == noErr, let created
        else { return false }
        VTSessionSetProperty(created, key: kVTDecompressionPropertyKey_RealTime, value: kCFBooleanTrue)
        session = created
        format = made
        sets = parameters
        return true
    }
}

final class FmmFrameShaper: @unchecked Sendable {
    private let context = CIContext(options: [.cacheIntermediates: false])
    private var pools: [String: CVPixelBufferPool] = [:]

    func upright(_ buffer: CVPixelBuffer, degrees: Int, width: Int, height: Int) -> CVPixelBuffer? {
        let oriented = CIImage(cvPixelBuffer: buffer).oriented(NativeFaceRenderer.orientation(degrees))
        let extent = oriented.extent
        let scale = max(Double(width) / extent.width, Double(height) / extent.height)
        let scaled = oriented.transformed(by: CGAffineTransform(translationX: -extent.minX, y: -extent.minY))
            .transformed(by: CGAffineTransform(scaleX: scale, y: scale))
        let dx = (Double(width) - scaled.extent.width) / 2
        let dy = (Double(height) - scaled.extent.height) / 2
        let placed = scaled.transformed(by: CGAffineTransform(translationX: dx, y: dy))
        guard let out = make(width: width, height: height) else { return nil }
        context.render(placed, to: out, bounds: CGRect(x: 0, y: 0, width: width, height: height), colorSpace: nil)
        return out
    }

    private func make(width: Int, height: Int) -> CVPixelBuffer? {
        let key = "\(width)x\(height)"
        if pools[key] == nil {
            var made: CVPixelBufferPool?
            let attributes: [String: Any] = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
                kCVPixelBufferWidthKey as String: width,
                kCVPixelBufferHeightKey as String: height,
                kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
            ]
            CVPixelBufferPoolCreate(nil, nil, attributes as CFDictionary, &made)
            pools[key] = made
        }
        guard let pool = pools[key] else { return nil }
        var out: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out)
        return out
    }
}
#endif
