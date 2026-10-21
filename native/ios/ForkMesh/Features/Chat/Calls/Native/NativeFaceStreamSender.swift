import CoreGraphics
import CoreImage
import CoreVideo
import FMChat
import Foundation
import ImageIO
import UniformTypeIdentifiers
import Vision

struct NativeFaceStreamPicture {
    let crop: FaceStreamCrop
    let image: CGImage
    let rgba: [UInt8]
}

final class NativeFaceStreamSender: @unchecked Sendable {
    let sender: FaceStreamSender
    private let context = CIContext(options: [.cacheIntermediates: false])
    private let pictures = DispatchQueue(label: "com.forkmesh.facestream.picture", qos: .utility)
    private let lock = NSLock()
    private var busy = false

    init?(width: Int, height: Int, fps: Int = FaceStream.defaultFPS) {
        guard let sender = try? FaceStreamSender(FaceStreamConfig(width: width, height: height, fps: fps)) else { return nil }
        self.sender = sender
    }

    func frame(time: UInt32, points: [CallFacePoint], reading: CallFaceExpressionReading?) -> [UInt8]? {
        lock.lock()
        defer { lock.unlock() }
        return try? sender.frame(time: time, points: points, reading: reading)
    }

    func requestKey() {
        lock.lock()
        sender.requestKey()
        lock.unlock()
    }

    func considerReference(_ buffer: CVPixelBuffer, degrees: Int, timeMs: Double, points: [CallFacePoint], reading: CallFaceExpressionReading?,
                           send: @escaping @Sendable ([UInt8]) -> Void) {
        lock.lock()
        let score = sender.score(points: points, reading: reading, light: nil)
        guard !busy, sender.due(time: timeMs, score: score) else {
            lock.unlock()
            return
        }
        busy = true
        let crop = sender.crop(points)
        let policy = sender.policy
        lock.unlock()
        let upright = CIImage(cvPixelBuffer: buffer).oriented(NativeFaceRenderer.orientation(degrees))
        pictures.async { [weak self] in
            guard let self else { return }
            defer {
                self.lock.lock()
                self.busy = false
                self.lock.unlock()
            }
            guard let picture = self.picture(upright, crop: crop, policy: policy) else { return }
            let local = FaceStreamSending.referencePoints(points, crop: crop, width: policy.width, height: policy.height)
            let light = FaceStreamSending.light(rgba: picture.rgba, width: policy.width, height: policy.height, points: local)
            guard let jpeg = Self.jpeg(picture.image, quality: policy.quality) else { return }
            let mask = Self.personMask(picture.image, width: policy.maskW, height: policy.maskH)
            self.lock.lock()
            let checked = self.sender.score(points: points, reading: reading, light: light)
            let packet = checked > 0 ? (try? self.sender.reference(time: timeMs, crop: crop, points: points, format: FaceStreamReference.jpeg, image: jpeg, mask: mask, score: checked)) : nil
            self.lock.unlock()
            if let packet { send(packet) }
        }
    }

    private func picture(_ upright: CIImage, crop: FaceStreamCrop, policy: FaceStreamPolicy) -> NativeFaceStreamPicture? {
        let extent = upright.extent
        let image = upright.transformed(by: CGAffineTransform(translationX: -extent.minX, y: -extent.minY))
        let rect = CGRect(x: Double(crop.x), y: extent.height - Double(crop.y + crop.h), width: Double(crop.w), height: Double(crop.h))
        let scaled = image.cropped(to: rect)
            .transformed(by: CGAffineTransform(translationX: -rect.minX, y: -rect.minY))
            .transformed(by: CGAffineTransform(scaleX: Double(policy.width) / rect.width, y: Double(policy.height) / rect.height))
        let target = CGRect(x: 0, y: 0, width: policy.width, height: policy.height)
        guard let cgImage = context.createCGImage(scaled, from: target),
            let rgba = NativeFaceStreamAssets.rgba(cgImage, width: policy.width, height: policy.height)
        else { return nil }
        return NativeFaceStreamPicture(crop: crop, image: cgImage, rgba: rgba)
    }

    static func jpeg(_ image: CGImage, quality: Double) -> [UInt8]? {
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data, UTType.jpeg.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(destination, image, [kCGImageDestinationLossyCompressionQuality: quality] as CFDictionary)
        guard CGImageDestinationFinalize(destination) else { return nil }
        return [UInt8](data as Data)
    }

    static func personMask(_ image: CGImage, width: Int, height: Int) -> [UInt8]? {
        let request = VNGeneratePersonSegmentationRequest()
        request.qualityLevel = .balanced
        request.outputPixelFormat = kCVPixelFormatType_OneComponent8
        let handler = VNImageRequestHandler(cgImage: image, orientation: .up, options: [:])
        guard (try? handler.perform([request])) != nil, let mask = request.results?.first?.pixelBuffer else { return nil }
        CVPixelBufferLockBaseAddress(mask, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(mask, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(mask) else { return nil }
        let mw = CVPixelBufferGetWidth(mask)
        let mh = CVPixelBufferGetHeight(mask)
        let stride = CVPixelBufferGetBytesPerRow(mask)
        let bytes = base.assumingMemoryBound(to: UInt8.self)
        var out = [UInt8](repeating: 0, count: width * height)
        for y in 0..<height {
            for x in 0..<width {
                let mx = min(mw - 1, (x * mw + mw / 2) / width)
                let my = min(mh - 1, (y * mh + mh / 2) / height)
                out[y * width + x] = bytes[my * stride + mx] >= 128 ? 1 : 0
            }
        }
        return out
    }
}
