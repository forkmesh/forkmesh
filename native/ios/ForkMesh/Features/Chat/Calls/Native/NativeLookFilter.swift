#if canImport(WebRTC)
import CoreImage
import FMChat
import Foundation
import UIKit
import Vision
import WebRTC

final class NativeLookFilter: NativeFrameFilter, @unchecked Sendable {
    static let blurSigma: [String: Double] = ["blur": 8, "blur-strong": 20]
    static let portraitRatio: CGFloat = 1.05

    private let context = CIContext(options: [.cacheIntermediates: false])
    private let request: VNGeneratePersonSegmentationRequest = {
        let made = VNGeneratePersonSegmentationRequest()
        made.qualityLevel = .balanced
        made.outputPixelFormat = kCVPixelFormatType_OneComponent8
        return made
    }()
    private let lock = NSLock()
    private var background = ""
    private var picture: CIImage?
    private var portraitPicture: CIImage?
    private var pool: CVPixelBufferPool?
    private var poolSize: CGSize = .zero

    var active: Bool {
        lock.lock()
        defer { lock.unlock() }
        return !background.isEmpty
    }

    func apply(background id: String, customImage: String) {
        var landscape: CIImage?
        var portrait: CIImage?
        if id == FaceChangerLook.customBackground {
            if let image = CallBackgroundPhoto.image(fromDataURL: customImage), let cg = image.cgImage {
                landscape = CIImage(cgImage: cg)
                portrait = landscape
            }
        } else if Self.blurSigma[id] == nil, !id.isEmpty {
            landscape = Self.bundled(id)
            portrait = Self.bundled(id + "-portrait") ?? landscape
        }
        lock.lock()
        background = (id == FaceChangerLook.customBackground && landscape == nil) ? "" : id
        picture = landscape
        portraitPicture = portrait
        lock.unlock()
    }

    private static func bundled(_ name: String) -> CIImage? {
        guard let url = Bundle.main.url(forResource: name, withExtension: "jpg", subdirectory: "CallBackgrounds")
            ?? Bundle.main.url(forResource: name, withExtension: "jpg")
        else { return nil }
        return CIImage(contentsOf: url)
    }

    func filter(_ buffer: CVPixelBuffer, rotation: RTCVideoRotation, timeNs: Int64) -> CVPixelBuffer {
        lock.lock()
        let id = background
        let landscape = picture
        let portrait = portraitPicture
        lock.unlock()
        guard !id.isEmpty else { return buffer }
        let handler = VNImageRequestHandler(cvPixelBuffer: buffer, orientation: .up)
        guard (try? handler.perform([request])) != nil, let mask = request.results?.first?.pixelBuffer else {
            return buffer
        }
        let source = CIImage(cvPixelBuffer: buffer)
        let extent = source.extent
        var maskImage = CIImage(cvPixelBuffer: mask)
        maskImage = maskImage.transformed(by: CGAffineTransform(
            scaleX: extent.width / maskImage.extent.width, y: extent.height / maskImage.extent.height))
        let backdrop: CIImage
        if let sigma = Self.blurSigma[id] {
            backdrop = source.clampedToExtent().applyingGaussianBlur(sigma: sigma).cropped(to: extent)
        } else {
            let upright = rotation == ._90 || rotation == ._270
            let shownTall = upright ? extent.width > extent.height * Self.portraitRatio : extent.height > extent.width * Self.portraitRatio
            guard let chosen = (shownTall ? portrait : landscape) ?? landscape else { return buffer }
            backdrop = Self.fill(chosen, into: extent, rotation: rotation)
        }
        let composed = source.applyingFilter("CIBlendWithMask", parameters: [
            kCIInputBackgroundImageKey: backdrop, kCIInputMaskImageKey: maskImage,
        ])
        guard let output = makeBuffer(width: Int(extent.width), height: Int(extent.height)) else { return buffer }
        context.render(composed, to: output)
        return output
    }

    private static func fill(_ image: CIImage, into extent: CGRect, rotation: RTCVideoRotation) -> CIImage {
        var oriented = image
        switch rotation {
        case ._90: oriented = image.oriented(.left)
        case ._180: oriented = image.oriented(.down)
        case ._270: oriented = image.oriented(.right)
        default: break
        }
        let scale = max(extent.width / oriented.extent.width, extent.height / oriented.extent.height)
        let scaled = oriented.transformed(by: CGAffineTransform(scaleX: scale, y: scale))
        let dx = extent.midX - scaled.extent.midX
        let dy = extent.midY - scaled.extent.midY
        return scaled.transformed(by: CGAffineTransform(translationX: dx, y: dy)).cropped(to: extent)
    }

    private func makeBuffer(width: Int, height: Int) -> CVPixelBuffer? {
        let size = CGSize(width: width, height: height)
        if pool == nil || poolSize != size {
            var made: CVPixelBufferPool?
            let attributes: [String: Any] = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
                kCVPixelBufferWidthKey as String: width,
                kCVPixelBufferHeightKey as String: height,
                kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
            ]
            CVPixelBufferPoolCreate(nil, nil, attributes as CFDictionary, &made)
            pool = made
            poolSize = size
        }
        guard let pool else { return nil }
        var out: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out)
        return out
    }
}
#endif
