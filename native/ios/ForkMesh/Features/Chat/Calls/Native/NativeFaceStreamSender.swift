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
    private var mouthBusy = false

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

    func considerMouth(_ buffer: CVPixelBuffer, degrees: Int, timeMs: Double, points: [CallFacePoint], reading: CallFaceExpressionReading?,
                       send: @escaping @Sendable ([UInt8]) -> Void) {
        lock.lock()
        guard !mouthBusy, let candidate = sender.mouthCandidate(time: timeMs, points: points, reading: reading) else {
            lock.unlock()
            return
        }
        mouthBusy = true
        let policy = sender.atlasPolicy
        lock.unlock()
        let upright = CIImage(cvPixelBuffer: buffer).oriented(NativeFaceRenderer.orientation(degrees))
        pictures.async { [weak self] in
            guard let self else { return }
            defer {
                self.lock.lock()
                self.mouthBusy = false
                self.lock.unlock()
            }
            guard let tile = self.mouthTile(upright, crop: candidate.crop, policy: policy) else { return }
            self.lock.lock()
            self.sender.storeMouthTile(candidate, rgba: tile)
            let due = self.sender.atlasDue(time: timeMs)
            let sheet = due ? self.sender.atlasSheet() : nil
            self.lock.unlock()
            guard let sheet, !sheet.tiles.isEmpty, let image = NativeFaceStreamAssets.cgImage(rgba: sheet.rgba, width: sheet.width, height: sheet.height),
                let jpeg = Self.jpeg(image, quality: policy.quality)
            else { return }
            self.lock.lock()
            let packet = try? self.sender.atlas(time: timeMs, tiles: sheet.tiles, image: jpeg)
            self.lock.unlock()
            if let packet { send(packet) }
        }
    }

    private func mouthTile(_ upright: CIImage, crop: FaceStreamMouthCrop, policy: FaceStreamAtlasPolicy) -> [UInt8]? {
        let extent = upright.extent
        let image = upright.transformed(by: CGAffineTransform(translationX: -extent.minX, y: -extent.minY))
        let halfW = Double(policy.tileW) * crop.scale * 0.75
        let halfH = Double(policy.tileH) * crop.scale * 0.75
        let reach = (halfW * halfW + halfH * halfH).squareRoot()
        let box = CGRect(x: crop.center.x - reach, y: crop.center.y - reach, width: 2 * reach, height: 2 * reach)
            .intersection(CGRect(x: 0, y: 0, width: extent.width, height: extent.height)).integral
        guard box.width >= 2, box.height >= 2,
            let region = context.createCGImage(image, from: CGRect(x: box.minX, y: extent.height - box.maxY, width: box.width, height: box.height))
        else { return nil }
        let tw = policy.tileW, th = policy.tileH
        var out = [UInt8](repeating: 0, count: tw * th * 4)
        let drawn: Bool = out.withUnsafeMutableBytes { raw in
            guard let ctx = CGContext(
                data: raw.baseAddress, width: tw, height: th, bitsPerComponent: 8, bytesPerRow: tw * 4,
                space: NativeFaceAssets.rgbSpace(region), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return false }
            let s = crop.scale
            ctx.translateBy(x: 0, y: CGFloat(th))
            ctx.scaleBy(x: 1, y: -1)
            ctx.concatenate(CGAffineTransform(
                a: crop.ux.x / s, b: crop.uy.x / s, c: crop.ux.y / s, d: crop.uy.y / s,
                tx: Double(tw) / 2 - (crop.center.x * crop.ux.x + crop.center.y * crop.ux.y) / s,
                ty: Double(th) / 2 - (crop.center.x * crop.uy.x + crop.center.y * crop.uy.y) / s))
            ctx.translateBy(x: box.minX, y: box.maxY)
            ctx.scaleBy(x: 1, y: -1)
            ctx.interpolationQuality = .high
            ctx.draw(region, in: CGRect(x: 0, y: 0, width: box.width, height: box.height))
            return true
        }
        return drawn ? out : nil
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
