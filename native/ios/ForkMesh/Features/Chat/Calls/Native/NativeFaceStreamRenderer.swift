import CoreGraphics
import CoreVideo
import FMChat
import Foundation
import ImageIO
import Metal
import SwiftUI

struct NativeFaceStreamAsset {
    let reference: FaceStreamReference
    let face: NativeFaceAsset
    let backdrop: MTLTexture
}

enum NativeFaceStreamAssets {
    static func image(_ bytes: [UInt8]) -> CGImage? {
        guard let source = CGImageSourceCreateWithData(Data(bytes) as CFData, nil) else { return nil }
        return CGImageSourceCreateImageAtIndex(source, 0, nil)
    }

    static func rgba(_ image: CGImage, width: Int, height: Int) -> [UInt8]? {
        var out = [UInt8](repeating: 0, count: width * height * 4)
        let drawn: Bool = out.withUnsafeMutableBytes { raw in
            guard let context = CGContext(
                data: raw.baseAddress, width: width, height: height, bitsPerComponent: 8, bytesPerRow: width * 4,
                space: NativeFaceAssets.rgbSpace(image), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
            else { return false }
            context.interpolationQuality = .high
            context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
            return true
        }
        return drawn ? out : nil
    }

    static func maskImage(_ reference: FaceStreamReference) -> CGImage? {
        guard let mask = reference.mask, reference.maskW > 0, reference.maskH > 0 else { return nil }
        let bytes = mask.map { $0 != 0 ? UInt8(255) : 0 }
        guard let provider = CGDataProvider(data: Data(bytes) as CFData) else { return nil }
        return CGImage(
            width: reference.maskW, height: reference.maskH, bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: reference.maskW,
            space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
            provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    static func make(_ reference: FaceStreamReference, device: MTLDevice, queue: MTLCommandQueue) -> NativeFaceStreamAsset? {
        guard let picture = image(reference.image), let pixels = rgba(picture, width: reference.width, height: reference.height) else { return nil }
        let side = NativeFaceAssets.side
        let points = reference.points
        let matrix = CallFaceCustom.transform(points).matrix
        let canvas = CGAffineTransform(a: matrix[0], b: matrix[1], c: matrix[2], d: matrix[3], tx: matrix[4], ty: matrix[5])
        guard let photo = NativeFaceAssets.placed(picture, width: reference.width, height: reference.height, transform: canvas, gray: false) else { return nil }
        var person: [Float]?
        if let mask = maskImage(reference), let drawn = NativeFaceAssets.placed(mask, width: reference.width, height: reference.height, transform: canvas, gray: true) {
            person = CallFaceCustom.personWeights(drawn)
        }
        let placedPoints = points.map { CallFaceCustom.apply(matrix, $0) }
        let polygons = CallFaceCustom.polygons(placedPoints)
        let built = CallFaceCustom.buildMatte(
            pixels: photo, size: side, points: placedPoints, person: person,
            faceMask: CallFaceCustom.rasterize(polygons.face, size: side),
            innerMask: CallFaceCustom.rasterize(polygons.inner, size: side),
            neckMask: CallFaceCustom.rasterize(polygons.neck, size: side))
        let model = CallFaceCustom.face(placed: placedPoints, head: built.head)
        guard let face = NativeFaceAssets.make(id: "face-stream", head: CallFaces.head(model), photo: photo, aux: CallFaceCustom.matteRGBA(built), device: device, queue: queue)
        else { return nil }
        let backdropBytes = FaceStreamView.backdrop(rgba: pixels, width: reference.width, height: reference.height, removal: FaceStreamView.removal(reference))
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: reference.width, height: reference.height, mipmapped: false)
        descriptor.usage = .shaderRead
        descriptor.storageMode = .shared
        guard let backdrop = device.makeTexture(descriptor: descriptor) else { return nil }
        backdropBytes.withUnsafeBytes { raw in
            if let base = raw.baseAddress {
                backdrop.replace(region: MTLRegionMake2D(0, 0, reference.width, reference.height), mipmapLevel: 0, withBytes: base, bytesPerRow: reference.width * 4)
            }
        }
        return NativeFaceStreamAsset(reference: reference, face: face, backdrop: backdrop)
    }
}

final class NativeFaceStreamRenderer: @unchecked Sendable {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let warp: MTLRenderPipelineState
    private let composite: MTLRenderPipelineState
    private let depthState: MTLDepthStencilState
    private let cache: CVMetalTextureCache
    private let blank: MTLTexture
    private let loader = DispatchQueue(label: "com.forkmesh.facestream.loader", qos: .userInitiated)
    private let lock = NSLock()
    private let receiver = FaceStreamReceiver()
    private var asset: NativeFaceStreamAsset?
    private var loading = 0
    private var vertexBuffer: MTLBuffer?
    private var warpTarget: MTLTexture?
    private var auxTarget: MTLTexture?
    private var depthTarget: MTLTexture?
    private var pool: CVPixelBufferPool?
    private var poolSize = (0, 0)
    private var count = 0

    init?() {
        guard let device = MTLCreateSystemDefaultDevice(), let queue = device.makeCommandQueue(), let library = device.makeDefaultLibrary() else { return nil }
        func pipeline(_ vertex: String, _ fragment: String, _ colors: [MTLPixelFormat], depth: MTLPixelFormat = .invalid) -> MTLRenderPipelineState? {
            guard let v = library.makeFunction(name: vertex), let f = library.makeFunction(name: fragment) else { return nil }
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = v
            descriptor.fragmentFunction = f
            for (index, format) in colors.enumerated() { descriptor.colorAttachments[index].pixelFormat = format }
            descriptor.depthAttachmentPixelFormat = depth
            return try? device.makeRenderPipelineState(descriptor: descriptor)
        }
        var made: CVMetalTextureCache?
        guard CVMetalTextureCacheCreate(nil, nil, device, nil, &made) == kCVReturnSuccess, let cache = made,
            let warp = pipeline("fmFaceWarpVertex", "fmFaceWarpFragment", [.rgba8Unorm, .rgba8Unorm], depth: .depth32Float),
            let composite = pipeline("fmFaceStreamQuadVertex", "fmFaceStreamComposite", [.bgra8Unorm])
        else { return nil }
        let depthDescriptor = MTLDepthStencilDescriptor()
        depthDescriptor.depthCompareFunction = .less
        depthDescriptor.isDepthWriteEnabled = true
        let blankDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 1, height: 1, mipmapped: false)
        blankDescriptor.usage = .shaderRead
        blankDescriptor.storageMode = .shared
        guard let depthState = device.makeDepthStencilState(descriptor: depthDescriptor), let blank = device.makeTexture(descriptor: blankDescriptor) else { return nil }
        let neutral: [UInt8] = [44, 46, 52, 255]
        neutral.withUnsafeBytes { raw in
            if let base = raw.baseAddress { blank.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0, withBytes: base, bytesPerRow: 4) }
        }
        self.device = device
        self.queue = queue
        self.warp = warp
        self.composite = composite
        self.depthState = depthState
        self.cache = cache
        self.blank = blank
    }

    var stale: Bool {
        lock.lock()
        defer { lock.unlock() }
        return receiver.frame?.stale ?? false
    }

    var needsKey: Bool {
        lock.lock()
        defer { lock.unlock() }
        return receiver.needsKey
    }

    @discardableResult
    func receive(_ packet: [UInt8], ready: (@Sendable () -> Void)? = nil) -> FaceStreamResult {
        lock.lock()
        let result = receiver.receive(packet)
        if case .reference(let reference) = result {
            loading += 1
            let token = loading
            lock.unlock()
            loader.async { [weak self] in
                guard let self, let made = NativeFaceStreamAssets.make(reference, device: self.device, queue: self.queue) else { return }
                self.lock.lock()
                if token == self.loading { self.asset = made }
                self.lock.unlock()
                ready?()
            }
            return result
        }
        lock.unlock()
        return result
    }

    func render(width: Int, height: Int) -> CVPixelBuffer? {
        lock.lock()
        let asset = self.asset
        let frame = receiver.frame
        lock.unlock()
        guard let output = makeBuffer(width: width, height: height), let target = texture(output) else { return nil }
        guard let commands = queue.makeCommandBuffer() else { return nil }
        var mouth = FaceStreamMouthFrame(center: CallFacePoint(-1e5, -1e5), x: CallFacePoint(0, 0), y: CallFacePoint(0, 0))
        var showFace = false
        var tongue = 0.0
        if let asset, let frame, prepareTargets(width: width, height: height) {
            let live = FaceStreamView.livePoints(frame, reference: asset.reference, width: Double(width), height: Double(height))
            let close = [frame.expression.lidLeft, frame.expression.lidRight]
            let drawn = CallFaceExpression.closeLids(live, close)
            let head = CallFaceGeometry.liveHead(asset.face.head.model, drawn)
            let vertices = CallFaceWarp.warpVertices(
                mesh: asset.face.head.mesh, layout: asset.face.head.layout, live: head.points, photoSize: CallFaces.photoSize,
                depths: CallFaceGeometry.vertexDepths(asset.face.head.model, head.pose))
            let gaze = CallFacePoint(frame.expression.gazeX, frame.expression.gazeY)
            if drawWarp(commands, vertices: vertices, asset: asset.face, look: CallFaceWarp.lookUniform([gaze, gaze]), close: close, width: width, height: height) {
                mouth = FaceStreamView.mouthFrame(head.points)
                showFace = true
                tongue = min(max(frame.expression.tongue, 0), 1)
            }
        }
        count = (count + 1) % 4096
        let descriptor = MTLRenderPassDescriptor()
        descriptor.colorAttachments[0].texture = target.1
        descriptor.colorAttachments[0].loadAction = .dontCare
        descriptor.colorAttachments[0].storeAction = .store
        guard let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor) else { return nil }
        encoder.setRenderPipelineState(composite)
        encoder.setFragmentTexture(asset?.backdrop ?? blank, index: 0)
        encoder.setFragmentTexture(showFace ? warpTarget : blank, index: 1)
        encoder.setFragmentTexture(showFace ? auxTarget : blank, index: 2)
        let seed = Float((Double(count) * 7.31).truncatingRemainder(dividingBy: 97))
        let faceOn: Float = showFace ? 1 : 0
        let pixelsMouth = SIMD4<Float>(Float(width), Float(height), Float(mouth.center.x), Float(mouth.center.y))
        let mouthAxes = SIMD4<Float>(Float(mouth.x.x), Float(mouth.x.y), Float(mouth.y.x), Float(mouth.y.y))
        let flags = SIMD4<Float>(Float(tongue), faceOn, seed, 0)
        let uniforms: [SIMD4<Float>] = [pixelsMouth, mouthAxes, flags]
        uniforms.withUnsafeBytes { raw in
            if let base = raw.baseAddress { encoder.setFragmentBytes(base, length: raw.count, index: 0) }
        }
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
        commands.commit()
        commands.waitUntilCompleted()
        _ = target.0
        return commands.status == .completed ? output : nil
    }

    private func prepareTargets(width: Int, height: Int) -> Bool {
        if warpTarget?.width == width, warpTarget?.height == height, auxTarget != nil, depthTarget != nil { return true }
        func make(_ format: MTLPixelFormat, depth: Bool) -> MTLTexture? {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: max(1, width), height: max(1, height), mipmapped: false)
            descriptor.usage = depth ? .renderTarget : [.renderTarget, .shaderRead]
            descriptor.storageMode = depth && device.supportsFamily(.apple2) ? .memoryless : .private
            return device.makeTexture(descriptor: descriptor)
        }
        warpTarget = make(.rgba8Unorm, depth: false)
        auxTarget = make(.rgba8Unorm, depth: false)
        depthTarget = make(.depth32Float, depth: true)
        return warpTarget != nil && auxTarget != nil && depthTarget != nil
    }

    private func drawWarp(_ commands: MTLCommandBuffer, vertices: [Float], asset: NativeFaceAsset, look: [Double], close: [Double], width: Int, height: Int) -> Bool {
        guard let warpTarget, let auxTarget, let depthTarget, !vertices.isEmpty else { return false }
        let length = vertices.count * MemoryLayout<Float>.stride
        if (vertexBuffer?.length ?? 0) < length { vertexBuffer = device.makeBuffer(length: max(length, 64 * 1024), options: .storageModeShared) }
        guard let vertexBuffer else { return false }
        vertices.withUnsafeBytes { raw in
            if let base = raw.baseAddress { vertexBuffer.contents().copyMemory(from: base, byteCount: length) }
        }
        func eye(_ side: Int) -> SIMD4<Float> {
            let e = side < asset.eyes.count && asset.eyes[side].count >= 4 ? asset.eyes[side] : [0, 0, 1, 1]
            return SIMD4(Float(e[0]), Float(e[1]), Float(e[2]), Float(e[3]))
        }
        let toX = SIMD4<Float>(1, 0, 0, Float(width))
        let toY = SIMD4<Float>(0, 1, 0, Float(height))
        let gaze = SIMD4<Float>(Float(look[0]), Float(look[1]), Float(look[2]), Float(look[3]))
        let lids = SIMD4<Float>(Float(close[0]), Float(close[1]), 0, 0)
        let uniforms: [SIMD4<Float>] = [toX, toY, eye(0), eye(1), gaze, lids]
        let descriptor = MTLRenderPassDescriptor()
        for (index, texture) in [warpTarget, auxTarget].enumerated() {
            descriptor.colorAttachments[index].texture = texture
            descriptor.colorAttachments[index].loadAction = .clear
            descriptor.colorAttachments[index].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            descriptor.colorAttachments[index].storeAction = .store
        }
        descriptor.depthAttachment.texture = depthTarget
        descriptor.depthAttachment.loadAction = .clear
        descriptor.depthAttachment.clearDepth = 1
        descriptor.depthAttachment.storeAction = .dontCare
        guard let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor) else { return false }
        encoder.setRenderPipelineState(warp)
        encoder.setDepthStencilState(depthState)
        encoder.setVertexBuffer(vertexBuffer, offset: 0, index: 0)
        uniforms.withUnsafeBytes { raw in
            if let base = raw.baseAddress {
                encoder.setVertexBytes(base, length: raw.count, index: 1)
                encoder.setFragmentBytes(base, length: raw.count, index: 0)
            }
        }
        encoder.setFragmentTexture(asset.photo, index: 0)
        encoder.setFragmentTexture(asset.aux, index: 1)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertices.count / CallFaceWarp.vertexFloats)
        encoder.endEncoding()
        return true
    }

    private func makeBuffer(width: Int, height: Int) -> CVPixelBuffer? {
        if poolSize != (width, height) || pool == nil {
            var made: CVPixelBufferPool?
            let attributes: [String: Any] = [
                kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA,
                kCVPixelBufferWidthKey as String: width,
                kCVPixelBufferHeightKey as String: height,
                kCVPixelBufferMetalCompatibilityKey as String: true,
                kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
            ]
            CVPixelBufferPoolCreate(nil, nil, attributes as CFDictionary, &made)
            pool = made
            poolSize = (width, height)
        }
        guard let pool else { return nil }
        var out: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out)
        return out
    }

    private func texture(_ buffer: CVPixelBuffer) -> (CVMetalTexture, MTLTexture)? {
        var made: CVMetalTexture?
        let status = CVMetalTextureCacheCreateTextureFromImage(nil, cache, buffer, nil, .bgra8Unorm, CVPixelBufferGetWidth(buffer), CVPixelBufferGetHeight(buffer), 0, &made)
        guard status == kCVReturnSuccess, let made, let texture = CVMetalTextureGetTexture(made) else { return nil }
        return (made, texture)
    }
}

struct NativeFaceStreamBadge: View {
    let stale: Bool

    var body: some View {
        Text(stale ? FaceStream.staleDetail : FaceStream.label)
            .font(.caption.weight(.medium))
            .foregroundStyle(.white)
            .padding(.horizontal, 8)
            .padding(.vertical, 3)
            .background(.black.opacity(0.55), in: Capsule())
            .accessibilityLabel("\(FaceStream.label). \(FaceStream.detail)")
    }
}
