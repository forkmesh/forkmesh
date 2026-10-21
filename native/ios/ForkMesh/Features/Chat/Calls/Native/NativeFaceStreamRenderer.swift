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
    let place: FaceStreamPlace?
    let colours: FaceStreamMouthColours
    let rgba: [UInt8]
}

struct NativeFaceStreamMouths {
    let atlas: FaceStreamAtlas
    let texture: MTLTexture
    let sheet: [UInt8]
    let sheetWidth: Int
    var gains: [[Double]]
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

    static func cgImage(rgba: [UInt8], width: Int, height: Int) -> CGImage? {
        guard let provider = CGDataProvider(data: Data(rgba) as CFData) else { return nil }
        return CGImage(
            width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 32, bytesPerRow: width * 4,
            space: CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB(),
            bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.noneSkipLast.rawValue),
            provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    static func grayImage(_ values: [Float], width: Int, height: Int) -> CGImage? {
        let bytes = values.map { UInt8(min(max(($0 * 255).rounded(), 0), 255)) }
        guard let provider = CGDataProvider(data: Data(bytes) as CFData) else { return nil }
        return CGImage(
            width: width, height: height, bitsPerComponent: 8, bitsPerPixel: 8, bytesPerRow: width,
            space: CGColorSpaceCreateDeviceGray(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.none.rawValue),
            provider: provider, decode: nil, shouldInterpolate: true, intent: .defaultIntent)
    }

    static func maskImage(_ reference: FaceStreamReference) -> CGImage? {
        guard let mask = reference.mask, reference.maskW > 0, reference.maskH > 0 else { return nil }
        return grayImage(mask.map { $0 != 0 ? 1 : 0 }, width: reference.maskW, height: reference.maskH)
    }

    static func texture(_ bytes: [UInt8], width: Int, height: Int, device: MTLDevice) -> MTLTexture? {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: width, height: height, mipmapped: false)
        descriptor.usage = .shaderRead
        descriptor.storageMode = .shared
        guard let texture = device.makeTexture(descriptor: descriptor) else { return nil }
        bytes.withUnsafeBytes { raw in
            if let base = raw.baseAddress { texture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: base, bytesPerRow: width * 4) }
        }
        return texture
    }

    static func floats(_ bytes: [UInt8]) -> [Float] { bytes.map { Float($0) / 255 } }

    static func make(_ reference: FaceStreamReference, device: MTLDevice, queue: MTLCommandQueue) -> NativeFaceStreamAsset? {
        guard let picture = image(reference.image), let pixels = rgba(picture, width: reference.width, height: reference.height) else { return nil }
        let w = reference.width, h = reference.height
        let colours = FaceStreamView.mouthColours(rgba: pixels, width: w, height: h, points: reference.points)
        if let alpha = FaceStreamView.matte(reference, rgba: pixels) {
            let layers = FaceStreamView.layers(reference, alpha: alpha)
            let plate = FaceStreamView.plate(rgba: pixels, width: w, height: h, removal: FaceStreamView.grow(layers.removal, w, h))
            let unmixed = FaceStreamView.unmix(rgba: pixels, plate: plate, alpha: alpha)
            let pad = Int((FaceStreamView.pad * Double(w)).rounded())
            let pw = w + 2 * pad, ph = h + 2 * pad
            let shifted = reference.points.map { CallFacePoint($0.x + Double(pad), $0.y + Double(pad)) }
            let matrix = FaceStreamView.photoTransform(shifted)
            let canvas = CGAffineTransform(a: matrix[0], b: matrix[1], c: matrix[2], d: matrix[3], tx: matrix[4], ty: matrix[5])
            guard let source = cgImage(rgba: FaceStreamView.padPixels(unmixed, w, h, pad), width: pw, height: ph),
                let personImage = grayImage(FaceStreamView.padFloats(alpha, w, h, pad), width: pw, height: ph),
                let headImage = grayImage(FaceStreamView.padFloats(layers.head, w, h, pad), width: pw, height: ph),
                let photo = NativeFaceAssets.placed(source, width: pw, height: ph, transform: canvas, gray: false),
                let person = NativeFaceAssets.placed(personImage, width: pw, height: ph, transform: canvas, gray: true),
                let head = NativeFaceAssets.placed(headImage, width: pw, height: ph, transform: canvas, gray: true)
            else { return nil }
            let built = FaceStreamView.face(photo: photo, person: floats(person), head: floats(head), points: shifted, matrix: matrix)
            guard let face = NativeFaceAssets.make(id: "face-stream", head: built.head, photo: photo, aux: built.aux, device: device, queue: queue),
                let backdrop = texture(plate, width: w, height: h, device: device)
            else { return nil }
            let place = FaceStreamPlace(points: reference.points, pose: FaceStreamSending.pose(reference.points).pose, matrix: matrix, pad: pad, width: w, height: h)
            return NativeFaceStreamAsset(reference: reference, face: face, backdrop: backdrop, place: place, colours: colours, rgba: pixels)
        }
        let points = reference.points
        let matrix = CallFaceCustom.transform(points).matrix
        let canvas = CGAffineTransform(a: matrix[0], b: matrix[1], c: matrix[2], d: matrix[3], tx: matrix[4], ty: matrix[5])
        guard let photo = NativeFaceAssets.placed(picture, width: w, height: h, transform: canvas, gray: false) else { return nil }
        let placedPoints = points.map { CallFaceCustom.apply(matrix, $0) }
        let polygons = CallFaceCustom.polygons(placedPoints)
        let built = CallFaceCustom.buildMatte(
            pixels: photo, size: NativeFaceAssets.side, points: placedPoints, person: nil,
            faceMask: CallFaceCustom.rasterize(polygons.face, size: NativeFaceAssets.side),
            innerMask: CallFaceCustom.rasterize(polygons.inner, size: NativeFaceAssets.side),
            neckMask: CallFaceCustom.rasterize(polygons.neck, size: NativeFaceAssets.side))
        let model = CallFaceCustom.face(placed: placedPoints, head: built.head)
        guard let face = NativeFaceAssets.make(id: "face-stream", head: CallFaces.head(model), photo: photo, aux: CallFaceCustom.matteRGBA(built), device: device, queue: queue),
            let backdrop = texture(FaceStreamView.backdrop(rgba: pixels, width: w, height: h, removal: [Float](repeating: 0, count: w * h)), width: w, height: h, device: device)
        else { return nil }
        let place = FaceStreamPlace(points: points, pose: FaceStreamSending.pose(points).pose, matrix: matrix, pad: 0, width: w, height: h)
        return NativeFaceStreamAsset(reference: reference, face: face, backdrop: backdrop, place: place, colours: colours, rgba: pixels)
    }

    static func mouths(_ atlas: FaceStreamAtlas, asset: NativeFaceStreamAsset?, device: MTLDevice) -> NativeFaceStreamMouths? {
        let width = atlas.tileW * atlas.tiles.count
        guard let picture = image(atlas.image), let sheet = rgba(picture, width: width, height: atlas.tileH),
            let texture = texture(sheet, width: width, height: atlas.tileH, device: device)
        else { return nil }
        var mouths = NativeFaceStreamMouths(atlas: atlas, texture: texture, sheet: sheet, sheetWidth: width, gains: atlas.tiles.map { _ in [1, 1, 1] })
        if let asset { mouths.gains = gains(mouths, asset: asset) }
        return mouths
    }

    static func gains(_ mouths: NativeFaceStreamMouths, asset: NativeFaceStreamAsset) -> [[Double]] {
        FaceStreamView.atlasGains(sheet: mouths.sheet, sheetWidth: mouths.sheetWidth, atlas: mouths.atlas, rgba: asset.rgba,
            width: asset.reference.width, height: asset.reference.height, points: asset.reference.points)
    }
}

final class NativeFaceStreamRenderer: @unchecked Sendable {
    let device: MTLDevice
    private let queue: MTLCommandQueue
    private let warp: MTLRenderPipelineState
    private let mouthPipeline: MTLRenderPipelineState
    private let composite: MTLRenderPipelineState
    private let depthState: MTLDepthStencilState
    private let cache: CVMetalTextureCache
    private let blank: MTLTexture
    private let loader = DispatchQueue(label: "com.forkmesh.facestream.loader", qos: .userInitiated)
    private let lock = NSLock()
    private let receiver = FaceStreamReceiver()
    private var asset: NativeFaceStreamAsset?
    private var mouths: NativeFaceStreamMouths?
    private var anchor: FaceStreamAnchor?
    private var loading = 0
    private var atlasLoading = 0
    private var vertexBuffer: MTLBuffer?
    private var mouthBuffer: MTLBuffer?
    private var warpTarget: MTLTexture?
    private var auxTarget: MTLTexture?
    private var mouthTarget: MTLTexture?
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
            let warp = pipeline("fmFaceStreamWarpVertex", "fmFaceStreamWarpFragment", [.rgba8Unorm, .rgba8Unorm], depth: .depth32Float),
            let mouthPipeline = pipeline("fmFaceStreamMouthVertex", "fmFaceStreamMouthFragment", [.rgba8Unorm]),
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
        self.mouthPipeline = mouthPipeline
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
        switch result {
        case .reference(let reference):
            loading += 1
            let token = loading
            lock.unlock()
            loader.async { [weak self] in
                guard let self, let made = NativeFaceStreamAssets.make(reference, device: self.device, queue: self.queue) else { return }
                self.lock.lock()
                if token == self.loading {
                    self.asset = made
                    self.anchor = nil
                    if var mouths = self.mouths {
                        mouths.gains = NativeFaceStreamAssets.gains(mouths, asset: made)
                        self.mouths = mouths
                    }
                }
                self.lock.unlock()
                ready?()
            }
        case .atlas(let atlas):
            atlasLoading += 1
            let token = atlasLoading
            let current = asset
            lock.unlock()
            loader.async { [weak self] in
                guard let self, let made = NativeFaceStreamAssets.mouths(atlas, asset: current, device: self.device) else { return }
                self.lock.lock()
                if token == self.atlasLoading { self.mouths = made }
                self.lock.unlock()
                ready?()
            }
        default:
            lock.unlock()
        }
        return result
    }

    func render(width: Int, height: Int) -> CVPixelBuffer? {
        lock.lock()
        let asset = self.asset
        let mouths = self.mouths
        let frame = receiver.frame
        if let asset, let place = asset.place, anchor == nil || anchor?.width != width || anchor?.height != height {
            anchor = FaceStreamView.anchor(head: asset.face.head, place: place, width: width, height: height)
        }
        let anchor = self.anchor
        lock.unlock()
        guard let output = makeBuffer(width: width, height: height), let target = texture(output) else { return nil }
        guard let commands = queue.makeCommandBuffer() else { return nil }
        var shape = FaceStreamMouthShape(center: CallFacePoint(-1e5, -1e5), axis: CallFacePoint(1, 0), halfWidth: 1, halfGap: 0, span: 1)
        var showFace = false
        var tongue = 0.0
        var mouthOn = 0.0
        if let asset, let frame, prepareTargets(width: width, height: height) {
            let live = FaceStreamView.livePoints(frame, reference: asset.reference, width: Double(width), height: Double(height))
            let close = [frame.expression.lidLeft, frame.expression.lidRight]
            let drawn = CallFaceExpression.closeLids(live, close)
            let head = CallFaceGeometry.liveHead(asset.face.head.model, drawn)
            let points = FaceStreamView.warpPoints(head.points, drawn: drawn, mouthStart: asset.face.head.mesh.mouthStart, anchor: anchor)
            let vertices = CallFaceWarp.warpVertices(
                mesh: asset.face.head.mesh, layout: asset.face.head.layout, live: points, photoSize: CallFaces.photoSize,
                depths: CallFaceGeometry.vertexDepths(asset.face.head.model, head.pose))
            let gaze = CallFacePoint(frame.expression.gazeX, frame.expression.gazeY)
            if drawWarp(commands, vertices: vertices, asset: asset.face, look: CallFaceWarp.lookUniform([gaze, gaze]), close: close, width: width, height: height) {
                shape = FaceStreamView.mouthShape(points)
                showFace = true
                tongue = min(max(frame.expression.tongue, 0), 1)
                if let mouths, let blend = FaceStreamView.mouthBlend(mouths.atlas.tiles, expression: frame.expression, points: drawn), blend.opacity > 0.001,
                    drawMouth(commands, points: points, mouths: mouths, blend: blend, width: width, height: height)
                {
                    mouthOn = blend.opacity
                }
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
        encoder.setFragmentTexture(mouthOn > 0 ? mouthTarget : blank, index: 3)
        let seed = Float((Double(count) * 7.31).truncatingRemainder(dividingBy: 97))
        let lip = asset?.colours.lip ?? [0.55, 0.3, 0.3]
        let teeth = asset?.colours.teeth ?? [0.86, 0.83, 0.76]
        let faceOn: Float = showFace ? 1 : 0
        let centre = SIMD4<Float>(Float(shape.center.x), Float(shape.center.y), Float(width), Float(height))
        let axes = SIMD4<Float>(Float(shape.axis.x), Float(shape.axis.y), Float(shape.halfWidth), Float(shape.halfGap))
        let flags = SIMD4<Float>(Float(shape.span), Float(tongue), faceOn, seed)
        let lipColour = SIMD4<Float>(Float(lip[0]), Float(lip[1]), Float(lip[2]), Float(mouthOn))
        let teethColour = SIMD4<Float>(Float(teeth[0]), Float(teeth[1]), Float(teeth[2]), 0)
        let uniforms: [SIMD4<Float>] = [centre, axes, flags, lipColour, teethColour]
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
        if warpTarget?.width == width, warpTarget?.height == height, auxTarget != nil, mouthTarget != nil, depthTarget != nil { return true }
        func make(_ format: MTLPixelFormat, depth: Bool) -> MTLTexture? {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: max(1, width), height: max(1, height), mipmapped: false)
            descriptor.usage = depth ? .renderTarget : [.renderTarget, .shaderRead]
            descriptor.storageMode = depth && device.supportsFamily(.apple2) ? .memoryless : .private
            return device.makeTexture(descriptor: descriptor)
        }
        warpTarget = make(.rgba8Unorm, depth: false)
        auxTarget = make(.rgba8Unorm, depth: false)
        mouthTarget = make(.rgba8Unorm, depth: false)
        depthTarget = make(.depth32Float, depth: true)
        return warpTarget != nil && auxTarget != nil && mouthTarget != nil && depthTarget != nil
    }

    private func drawMouth(_ commands: MTLCommandBuffer, points: [CallFacePoint], mouths: NativeFaceStreamMouths, blend: FaceStreamMouthBlend, width: Int, height: Int) -> Bool {
        guard let mouthTarget else { return false }
        let atlas = mouths.atlas
        let vertices = FaceStreamView.mouthMesh(points, tileA: atlas.tiles[blend.a], tileB: atlas.tiles[blend.b], tileW: atlas.tileW, tileH: atlas.tileH)
        let length = vertices.count * MemoryLayout<Float>.stride
        if (mouthBuffer?.length ?? 0) < length { mouthBuffer = device.makeBuffer(length: max(length, 16 * 1024), options: .storageModeShared) }
        guard let mouthBuffer, !vertices.isEmpty else { return false }
        vertices.withUnsafeBytes { raw in
            if let base = raw.baseAddress { mouthBuffer.contents().copyMemory(from: base, byteCount: length) }
        }
        func gain(_ k: Int) -> SIMD4<Float> {
            let g = k < mouths.gains.count ? mouths.gains[k] : [1, 1, 1]
            return SIMD4(Float(g[0]), Float(g[1]), Float(g[2]), 1)
        }
        let size = SIMD4<Float>(Float(width), Float(height), 0.5 / Float(atlas.tileW), 0.5 / Float(atlas.tileH))
        let tiles = SIMD4<Float>(Float(atlas.tiles.count), Float(blend.a), Float(blend.b), Float(blend.mix))
        let uniforms: [SIMD4<Float>] = [size, tiles, gain(blend.a), gain(blend.b)]
        let descriptor = MTLRenderPassDescriptor()
        descriptor.colorAttachments[0].texture = mouthTarget
        descriptor.colorAttachments[0].loadAction = .clear
        descriptor.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        descriptor.colorAttachments[0].storeAction = .store
        guard let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor) else { return false }
        encoder.setRenderPipelineState(mouthPipeline)
        encoder.setVertexBuffer(mouthBuffer, offset: 0, index: 0)
        uniforms.withUnsafeBytes { raw in
            if let base = raw.baseAddress {
                encoder.setVertexBytes(base, length: raw.count, index: 1)
                encoder.setFragmentBytes(base, length: raw.count, index: 0)
            }
        }
        encoder.setFragmentTexture(mouths.texture, index: 0)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: vertices.count / FaceStreamView.mouthVertexFloats)
        encoder.endEncoding()
        return true
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
