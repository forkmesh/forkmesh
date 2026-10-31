import CoreGraphics
import CoreVideo
import FMChat
import Foundation
import ImageIO
import Metal
import Vision

struct NativeFaceAsset {
    let id: String
    let head: CallFaceHead
    let eyes: [[Double]]
    let photo: MTLTexture
    let aux: MTLTexture
}

enum NativeFaceAssets {
    static let side = Int(CallFaces.photoSize)

    static func builtIn(_ id: String, device: MTLDevice, queue: MTLCommandQueue) -> NativeFaceAsset? {
        guard let face = CallFaces.byId(id),
            let photo = bundled(id, extension: "jpg"),
            let matte = bundled(id + "-matte", extension: "png"),
            let photoBytes = pixels(photo, size: side),
            let matteBytes = pixels(matte, size: side)
        else { return nil }
        return make(id: id, head: CallFaces.head(face), photo: photoBytes, aux: matteBytes, device: device, queue: queue)
    }

    static func custom(dataURL: String, device: MTLDevice, queue: MTLCommandQueue) -> NativeFaceAsset? {
        guard let image = decode(dataURL) else { return nil }
        let width = image.width
        let height = image.height
        let scale = min(1, CallFaceCustom.side / Double(max(width, height)))
        let sizedWidth = max(1, Int((Double(width) * scale).rounded()))
        let sizedHeight = max(1, Int((Double(height) * scale).rounded()))
        guard let sized = scale < 1 ? resized(image, width: sizedWidth, height: sizedHeight) : image else { return nil }
        let faceRequest = VNDetectFaceLandmarksRequest()
        faceRequest.constellation = .constellation76Points
        let personRequest = VNGeneratePersonSegmentationRequest()
        personRequest.qualityLevel = .accurate
        personRequest.outputPixelFormat = kCVPixelFormatType_OneComponent8
        let handler = VNImageRequestHandler(cgImage: sized, orientation: .up, options: [:])
        guard (try? handler.perform([faceRequest, personRequest])) != nil else { return nil }
        let regions = CallFaceLandmarks.regions(faceRequest.results ?? [], size: CGSize(width: sized.width, height: sized.height))
        let sizedScale = Double(sized.width) / Double(width)
        guard let points = try? CallFaceCustom.validate(CallFaces.located(regions, scale: sizedScale).points) else { return nil }
        let matrix = CallFaceCustom.transform(points).matrix
        let canvas = CGAffineTransform(a: matrix[0], b: matrix[1], c: matrix[2], d: matrix[3], tx: matrix[4], ty: matrix[5])
        guard let photo = placed(image, width: width, height: height, transform: canvas, gray: false) else { return nil }
        var person: [Float]?
        if let mask = personRequest.results?.first?.pixelBuffer, let maskImage = grayImage(mask),
            let drawn = placed(maskImage, width: width, height: height, transform: canvas, gray: true)
        {
            person = CallFaceCustom.personWeights(drawn)
        }
        let placedPoints = points.map { CallFaceCustom.apply(matrix, $0) }
        let polygons = CallFaceCustom.polygons(placedPoints)
        let built = CallFaceCustom.buildMatte(
            pixels: photo,
            size: side,
            points: placedPoints,
            person: person,
            faceMask: CallFaceCustom.rasterize(polygons.face, size: side),
            innerMask: CallFaceCustom.rasterize(polygons.inner, size: side),
            neckMask: CallFaceCustom.rasterize(polygons.neck, size: side)
        )
        let model = CallFaceCustom.face(placed: placedPoints, head: built.head)
        return make(id: CallFaceCustom.id, head: CallFaces.head(model), photo: photo, aux: CallFaceCustom.matteRGBA(built), device: device, queue: queue)
    }

    static func make(id: String, head: CallFaceHead, photo: [UInt8], aux: [UInt8], device: MTLDevice, queue: MTLCommandQueue) -> NativeFaceAsset? {
        guard let photoTexture = texture(photo, size: side, device: device, queue: queue),
            let auxTexture = texture(aux, size: side, device: device, queue: queue)
        else { return nil }
        return NativeFaceAsset(id: id, head: head, eyes: CallFaceWarp.photoEyes(head.layout, photoSize: CallFaces.photoSize), photo: photoTexture, aux: auxTexture)
    }

    static func texture(_ bytes: [UInt8], size: Int, device: MTLDevice, queue: MTLCommandQueue) -> MTLTexture? {
        guard bytes.count >= size * size * 4 else { return nil }
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: size, height: size, mipmapped: true)
        descriptor.usage = .shaderRead
        descriptor.storageMode = .shared
        guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
        bytes.withUnsafeBytes { raw in
            guard let base = raw.baseAddress else { return }
            texture.replace(region: MTLRegionMake2D(0, 0, size, size), mipmapLevel: 0, withBytes: base, bytesPerRow: size * 4)
        }
        guard let commands = queue.makeCommandBuffer(), let blit = commands.makeBlitCommandEncoder() else { return nil }
        blit.generateMipmaps(for: texture)
        blit.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()
        return commands.status == .completed ? texture : nil
    }

    static func bundled(_ name: String, extension type: String) -> CGImage? {
        guard let url = Bundle.main.url(forResource: name, withExtension: type, subdirectory: "CallFaces")
            ?? Bundle.main.url(forResource: name, withExtension: type),
            let source = CGImageSourceCreateWithURL(url as CFURL, nil)
        else { return nil }
        return CGImageSourceCreateImageAtIndex(source, 0, nil)
    }

    static func decode(_ dataURL: String) -> CGImage? {
        guard let comma = dataURL.firstIndex(of: ","),
            let data = Data(base64Encoded: String(dataURL[dataURL.index(after: comma)...])),
            let source = CGImageSourceCreateWithData(data as CFData, nil)
        else { return nil }
        let options: CFDictionary = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceThumbnailMaxPixelSize: 4096,
        ] as CFDictionary
        return CGImageSourceCreateThumbnailAtIndex(source, 0, options) ?? CGImageSourceCreateImageAtIndex(source, 0, nil)
    }

    static func rgbSpace(_ image: CGImage) -> CGColorSpace {
        if let space = image.colorSpace, space.model == .rgb { return space }
        return CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB()
    }

    static func pixels(_ image: CGImage, size: Int) -> [UInt8]? {
        var out = [UInt8](repeating: 0, count: size * size * 4)
        let drawn: Bool = out.withUnsafeMutableBytes { raw in
            guard let context = CGContext(
                data: raw.baseAddress, width: size, height: size, bitsPerComponent: 8, bytesPerRow: size * 4,
                space: rgbSpace(image), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return false }
            context.setFillColor(red: 0, green: 0, blue: 0, alpha: 1)
            context.fill(CGRect(x: 0, y: 0, width: size, height: size))
            context.interpolationQuality = image.width == size && image.height == size ? .none : .high
            context.draw(image, in: CGRect(x: 0, y: 0, width: size, height: size))
            return true
        }
        return drawn ? out : nil
    }

    static func resized(_ image: CGImage, width: Int, height: Int) -> CGImage? {
        guard let context = CGContext(
            data: nil, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
            space: rgbSpace(image), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        else { return nil }
        context.interpolationQuality = .high
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        return context.makeImage()
    }

    static func placed(_ image: CGImage, width: Int, height: Int, transform: CGAffineTransform, gray: Bool) -> [UInt8]? {
        let channels = gray ? 1 : 4
        var out = [UInt8](repeating: 0, count: side * side * channels)
        let drawn: Bool = out.withUnsafeMutableBytes { raw in
            let space = gray ? CGColorSpaceCreateDeviceGray() : rgbSpace(image)
            let info = gray ? CGImageAlphaInfo.none.rawValue : CGImageAlphaInfo.premultipliedLast.rawValue
            guard let context = CGContext(
                data: raw.baseAddress, width: side, height: side, bitsPerComponent: 8, bytesPerRow: side * channels,
                space: space, bitmapInfo: info)
            else { return false }
            context.setFillColor(gray: 0, alpha: 1)
            context.fill(CGRect(x: 0, y: 0, width: side, height: side))
            context.translateBy(x: 0, y: CGFloat(side))
            context.scaleBy(x: 1, y: -1)
            context.concatenate(transform)
            context.translateBy(x: 0, y: CGFloat(height))
            context.scaleBy(x: 1, y: -1)
            context.interpolationQuality = .high
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }
        return drawn ? out : nil
    }

    static func grayImage(_ buffer: CVPixelBuffer) -> CGImage? {
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { return nil }
        let width = CVPixelBufferGetWidth(buffer)
        let height = CVPixelBufferGetHeight(buffer)
        let stride = CVPixelBufferGetBytesPerRow(buffer)
        let data = Data(bytes: base, count: stride * height)
        guard let provider = CGDataProvider(data: data as CFData) else { return nil }
        return CGImage(
            width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: stride,
            space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
            provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }
}

enum NativeOutfitPainter {
    struct Pen {
        var transform = CGAffineTransform.identity
        var fill = CallOutfitPaint.color(CallOutfitColor(r: 0, g: 0, b: 0))
        var stroke = CallOutfitPaint.color(CallOutfitColor(r: 0, g: 0, b: 0))
        var lineWidth: CGFloat = 1
        var cap = CGLineCap.butt
        var join = CGLineJoin.miter
        var dash: [CGFloat] = []
        var blend = CGBlendMode.normal
    }

    final class Replay {
        let context: CGContext
        var pen = Pen()
        var saved: [Pen] = []
        var path = CGMutablePath()
        var tiles: [CallOutfitFabric: CGImage] = [:]
        let space: CGColorSpace

        init(context: CGContext, space: CGColorSpace) {
            self.context = context
            self.space = space
        }

        func run(_ command: CallOutfitCommand) {
            switch command {
            case .save:
                saved.append(pen)
                context.saveGState()
            case .restore:
                guard let last = saved.popLast() else { return }
                pen = last
                context.restoreGState()
            case .beginPath:
                path = CGMutablePath()
            case .closePath:
                if !path.isEmpty { path.closeSubpath() }
            case let .moveTo(x, y):
                path.move(to: CGPoint(x: x, y: y), transform: pen.transform)
            case let .lineTo(x, y):
                if path.isEmpty {
                    path.move(to: CGPoint(x: x, y: y), transform: pen.transform)
                } else {
                    path.addLine(to: CGPoint(x: x, y: y), transform: pen.transform)
                }
            case let .quadTo(cx, cy, x, y):
                if path.isEmpty { path.move(to: CGPoint(x: cx, y: cy), transform: pen.transform) }
                path.addQuadCurve(to: CGPoint(x: x, y: y), control: CGPoint(x: cx, y: cy), transform: pen.transform)
            case let .arc(x, y, radius, start, end):
                path.addArc(center: CGPoint(x: x, y: y), radius: radius, startAngle: start, endAngle: end, clockwise: false, transform: pen.transform)
            case .fill:
                fill(path)
            case .stroke:
                stroke(path)
            case .clip:
                context.addPath(path)
                context.clip(using: .winding)
            case let .fillRect(x, y, w, h):
                var placed = pen.transform
                fill(CGPath(rect: CGRect(x: x, y: y, width: w, height: h), transform: &placed))
            case let .translate(x, y):
                pen.transform = pen.transform.translatedBy(x: x, y: y)
            case let .rotate(angle):
                pen.transform = pen.transform.rotated(by: angle)
            case let .fillStyle(paint):
                pen.fill = paint
            case let .strokeStyle(paint):
                pen.stroke = paint
            case let .lineWidth(width):
                if width > 0, width.isFinite { pen.lineWidth = width }
            case let .lineCap(name):
                pen.cap = name == "round" ? .round : name == "square" ? .square : .butt
            case let .lineJoin(name):
                pen.join = name == "round" ? .round : name == "bevel" ? .bevel : .miter
            case let .lineDash(lengths):
                let valid = lengths.allSatisfy { $0 >= 0 && $0.isFinite } && lengths.contains { $0 > 0 }
                let doubled = lengths.count % 2 == 1 ? lengths + lengths : lengths
                pen.dash = valid ? doubled.map { CGFloat($0) } : []
            case let .composite(name):
                switch name {
                case "destination-out": pen.blend = .destinationOut
                case "source-atop": pen.blend = .sourceAtop
                case "destination-over": pen.blend = .destinationOver
                case "multiply": pen.blend = .multiply
                default: pen.blend = .normal
                }
            }
        }

        func color(_ value: CallOutfitColor) -> CGColor {
            CGColor(srgbRed: value.r / 255, green: value.g / 255, blue: value.b / 255, alpha: value.a)
        }

        func fill(_ shape: CGPath) {
            guard !shape.isEmpty else { return }
            context.saveGState()
            context.setBlendMode(pen.blend)
            context.addPath(shape)
            if case let .color(value) = pen.fill {
                context.setFillColor(color(value))
                context.fillPath(using: .winding)
            } else {
                context.clip(using: .winding)
                paint(pen.fill)
            }
            context.restoreGState()
        }

        func stroke(_ shape: CGPath) {
            guard !shape.isEmpty, pen.transform.a * pen.transform.d - pen.transform.b * pen.transform.c != 0 else { return }
            var inverse = pen.transform.inverted()
            guard let local = shape.copy(using: &inverse) else { return }
            context.saveGState()
            context.setBlendMode(pen.blend)
            context.concatenate(pen.transform)
            context.addPath(local)
            context.setLineWidth(pen.lineWidth)
            context.setLineCap(pen.cap)
            context.setLineJoin(pen.join)
            context.setLineDash(phase: 0, lengths: pen.dash)
            if case let .color(value) = pen.stroke {
                context.setStrokeColor(color(value))
                context.strokePath()
            } else {
                context.replacePathWithStrokedPath()
                context.clip(using: .winding)
                context.concatenate(pen.transform.inverted())
                paint(pen.stroke)
            }
            context.restoreGState()
        }

        func paint(_ style: CallOutfitPaint) {
            context.concatenate(pen.transform)
            switch style {
            case let .color(value):
                context.setFillColor(color(value))
                context.fill(CGRect(x: -1e5, y: -1e5, width: 2e5, height: 2e5))
            case let .fabric(fabric):
                guard let tile = tile(fabric) else { return }
                let side = CGFloat(CallOutfits.tile)
                context.translateBy(x: 0, y: side)
                context.scaleBy(x: 1, y: -1)
                context.draw(tile, in: CGRect(x: 0, y: 0, width: side, height: side), byTiling: true)
            case let .gradient(gradient):
                let colors = gradient.stops.map { color($0.color) } as CFArray
                let locations = gradient.stops.map { CGFloat($0.offset) }
                guard let made = CGGradient(colorsSpace: space, colors: colors, locations: locations) else { return }
                context.drawLinearGradient(
                    made, start: CGPoint(x: gradient.x0, y: gradient.y0), end: CGPoint(x: gradient.x1, y: gradient.y1),
                    options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
            }
        }

        func tile(_ fabric: CallOutfitFabric) -> CGImage? {
            if let cached = tiles[fabric] { return cached }
            let side = CallOutfits.tile
            let bytes = CallOutfitDrawing.fabricTile(fabric)
            guard let provider = CGDataProvider(data: Data(bytes) as CFData),
                let image = CGImage(
                    width: side, height: side, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: side * 4, space: space,
                    bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue), provider: provider,
                    decode: nil, shouldInterpolate: false, intent: .defaultIntent)
            else { return nil }
            tiles[fabric] = image
            return image
        }
    }

    static func pixels(_ id: String) -> [UInt8]? {
        guard let commands = CallOutfitDrawing.commands(id) else { return nil }
        let size = Int(CallOutfits.size)
        let space = CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB()
        var out = [UInt8](repeating: 0, count: size * size * 4)
        let drawn: Bool = out.withUnsafeMutableBytes { raw in
            guard let context = CGContext(
                data: raw.baseAddress, width: size, height: size, bitsPerComponent: 8, bytesPerRow: size * 4,
                space: space, bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return false }
            context.translateBy(x: 0, y: CGFloat(size))
            context.scaleBy(x: 1, y: -1)
            context.interpolationQuality = .none
            let replay = Replay(context: context, space: space)
            for command in commands { replay.run(command) }
            return true
        }
        return drawn ? out : nil
    }

    static func texture(_ id: String, device: MTLDevice, queue: MTLCommandQueue) -> MTLTexture? {
        guard let bytes = pixels(id) else { return nil }
        return NativeFaceAssets.texture(bytes, size: Int(CallOutfits.size), device: device, queue: queue)
    }
}
