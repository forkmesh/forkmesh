import CoreVideo
import FMChat
import Foundation
import Metal

struct NativeFaceDepth {
    let texture: MTLTexture
    let faceMetres: Double
    let toDepthUv: [Double]
}

enum NativeSegmentModels {
    static func hair() -> CallSegmentModel? {
        guard let url = Bundle.main.url(forResource: "hair-192", withExtension: "fmnn", subdirectory: "CallSegmentation")
            ?? Bundle.main.url(forResource: "hair-192", withExtension: "fmnn"),
            let data = try? Data(contentsOf: url)
        else { return nil }
        return try? CallSegmentModel.parse(data)
    }
}

final class NativeSegmentNetwork {
    let plan: CallSegmentPlan
    let arena: MTLBuffer
    private let weights: MTLBuffer
    private let pipeline: MTLComputePipelineState
    private let width: Int

    init?(device: MTLDevice, library: MTLLibrary, model: CallSegmentModel) {
        let plan = CallSegmentPlan(model)
        guard let function = library.makeFunction(name: "fmSegLayer"),
            let pipeline = try? device.makeComputePipelineState(function: function),
            let arena = device.makeBuffer(length: max(2, plan.arenaCount) * 2, options: .storageModePrivate),
            let weights = model.weights.withUnsafeBytes({ raw in raw.baseAddress.flatMap { device.makeBuffer(bytes: $0, length: max(2, raw.count), options: .storageModeShared) } })
        else { return nil }
        self.plan = plan
        self.arena = arena
        self.weights = weights
        self.pipeline = pipeline
        width = min(256, pipeline.maxTotalThreadsPerThreadgroup)
    }

    var inputShape: CallSegmentShape { plan.model.inputShape }
    var outputShape: CallSegmentShape { plan.model.outputShape }
    var inputBytes: Int { plan.inputOffset * 2 }
    var outputBytes: Int { plan.outputOffset * 2 }

    func encode(_ encoder: MTLComputeCommandEncoder) {
        encoder.setComputePipelineState(pipeline)
        encoder.setBuffer(weights, offset: 0, index: 3)
        for step in plan.steps {
            encoder.setBuffer(arena, offset: step.input * 2, index: 0)
            encoder.setBuffer(arena, offset: step.second * 2, index: 1)
            encoder.setBuffer(arena, offset: step.output * 2, index: 2)
            step.fields.withUnsafeBytes { raw in
                if let base = raw.baseAddress { encoder.setBytes(base, length: raw.count, index: 4) }
            }
            let groups = (step.count + width - 1) / width
            encoder.dispatchThreadgroups(MTLSize(width: groups, height: 1, depth: 1), threadsPerThreadgroup: MTLSize(width: width, height: 1, depth: 1))
        }
    }
}

enum NativeSegmentPass {
    static func render(_ commands: MTLCommandBuffer, into target: MTLTexture, _ pipeline: MTLRenderPipelineState, textures: [MTLTexture],
                       uniforms: [SIMD4<Float>], buffers: [(MTLBuffer, Int, Int)] = [], clear: Bool = false) -> Bool {
        let descriptor = MTLRenderPassDescriptor()
        descriptor.colorAttachments[0].texture = target
        descriptor.colorAttachments[0].loadAction = clear ? .clear : .dontCare
        descriptor.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
        descriptor.colorAttachments[0].storeAction = .store
        guard let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor) else { return false }
        encoder.setRenderPipelineState(pipeline)
        for (index, texture) in textures.enumerated() { encoder.setFragmentTexture(texture, index: index) }
        let values = uniforms.isEmpty ? [SIMD4<Float>(0, 0, 0, 0)] : uniforms
        values.withUnsafeBytes { raw in
            if let base = raw.baseAddress { encoder.setFragmentBytes(base, length: raw.count, index: 0) }
        }
        for (buffer, offset, index) in buffers { encoder.setFragmentBuffer(buffer, offset: offset, index: index) }
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
        return true
    }

    static func clear(_ commands: MTLCommandBuffer, _ textures: [MTLTexture]) -> Bool {
        for texture in textures {
            let descriptor = MTLRenderPassDescriptor()
            descriptor.colorAttachments[0].texture = texture
            descriptor.colorAttachments[0].loadAction = .clear
            descriptor.colorAttachments[0].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            descriptor.colorAttachments[0].storeAction = .store
            guard let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor) else { return false }
            encoder.endEncoding()
        }
        return true
    }

    static func pipeline(_ device: MTLDevice, _ library: MTLLibrary, _ fragment: String, _ format: MTLPixelFormat) -> MTLRenderPipelineState? {
        guard let vertex = library.makeFunction(name: "fmSegQuadVertex"), let function = library.makeFunction(name: fragment) else { return nil }
        let descriptor = MTLRenderPipelineDescriptor()
        descriptor.vertexFunction = vertex
        descriptor.fragmentFunction = function
        descriptor.colorAttachments[0].pixelFormat = format
        return try? device.makeRenderPipelineState(descriptor: descriptor)
    }

    static func texture(_ device: MTLDevice, _ format: MTLPixelFormat, _ size: CallFaceSize) -> MTLTexture? {
        let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: max(1, size.width), height: max(1, size.height), mipmapped: false)
        descriptor.usage = [.renderTarget, .shaderRead]
        descriptor.storageMode = .private
        return device.makeTexture(descriptor: descriptor)
    }

    static func rows(_ m: [Double], _ wx: Double, _ wy: Double) -> (SIMD4<Float>, SIMD4<Float>) {
        (SIMD4(Float(m[0]), Float(m[2]), Float(m[4]), Float(wx)), SIMD4(Float(m[1]), Float(m[3]), Float(m[5]), Float(wy)))
    }
}

final class NativeHairMatte {
    private let device: MTLDevice
    private let network: NativeSegmentNetwork
    private let crop: MTLComputePipelineState
    private let frame: MTLRenderPipelineState
    private var ping: MTLTexture?
    private var pong: MTLTexture?
    private var size = CallFaceSize(0, 0)
    private var fresh = true
    private var cleared = false
    private(set) var ran = 0

    init?(device: MTLDevice, library: MTLLibrary, model: CallSegmentModel) {
        guard model.inputShape.c == 4, model.outputShape.c == 2,
            let network = NativeSegmentNetwork(device: device, library: library, model: model),
            let function = library.makeFunction(name: "fmHairCrop"),
            let crop = try? device.makeComputePipelineState(function: function),
            let frame = NativeSegmentPass.pipeline(device, library, "fmHairFrame", .r8Unorm)
        else { return nil }
        self.device = device
        self.network = network
        self.crop = crop
        self.frame = frame
    }

    var texture: MTLTexture? { ping }

    func reset() {
        fresh = true
    }

    func update(_ commands: MTLCommandBuffer, camera: MTLTexture, map: CallFaceFrameMap, region: CallFaceHairRegion, run: Bool, depth: NativeFaceDepth?) -> MTLTexture? {
        let wanted = CallFaceMatte.hairSize(map: map)
        if wanted != size || ping == nil || pong == nil {
            ping = NativeSegmentPass.texture(device, .r8Unorm, wanted)
            pong = NativeSegmentPass.texture(device, .r8Unorm, wanted)
            size = wanted
            fresh = true
            cleared = false
        }
        guard var current = ping, var spare = pong else { return nil }
        if !cleared {
            guard NativeSegmentPass.clear(commands, [current, spare]) else { return nil }
            cleared = true
        }
        let upright = map.uprightSize
        let toBuffer = NativeSegmentPass.rows(map.toBufferMatrix, Double(map.bufferWidth), Double(map.bufferHeight))
        let input = network.inputShape
        let output = network.outputShape
        let regionValue = SIMD4<Float>(Float(region.center.x), Float(region.center.y), Float(region.size.x), Float(region.size.y))
        let rounds = fresh ? CallFaceMatte.hairWarmRounds : 1
        for _ in 0..<rounds {
            if run || fresh {
                guard let encoder = commands.makeComputeCommandEncoder() else { return nil }
                encoder.setComputePipelineState(crop)
                encoder.setTexture(camera, index: 0)
                encoder.setTexture(current, index: 1)
                encoder.setBuffer(network.arena, offset: network.inputBytes, index: 0)
                func bits(_ value: Int) -> Float { Float(bitPattern: UInt32(truncatingIfNeeded: value)) }
                let uniforms: [SIMD4<Float>] = [
                    regionValue, SIMD4(Float(upright.width), Float(upright.height), 0, 0), toBuffer.0, toBuffer.1,
                    SIMD4(bits(input.w), bits(input.h), bits(input.c), 0),
                ]
                uniforms.withUnsafeBytes { raw in
                    if let base = raw.baseAddress { encoder.setBytes(base, length: raw.count, index: 1) }
                }
                let groups = MTLSize(width: (input.w + 15) / 16, height: (input.h + 15) / 16, depth: 1)
                encoder.dispatchThreadgroups(groups, threadsPerThreadgroup: MTLSize(width: 16, height: 16, depth: 1))
                network.encode(encoder)
                encoder.endEncoding()
                ran += 1
            }
            var depthRows = (SIMD4<Float>(0, 0, 0, 0), SIMD4<Float>(0, 0, 0, 0))
            var depthState = SIMD4<Float>(0, 0, 0, 0)
            if let depth {
                let m = CallFaceMatte.compose(depth.toDepthUv, CallFaceMatte.uprightToBufferUv(map))
                depthRows = (SIMD4(Float(m[0]), Float(m[2]), Float(m[4]), 0), SIMD4(Float(m[1]), Float(m[3]), Float(m[5]), 0))
                depthState = SIMD4(1, Float(depth.faceMetres), Float(CallFaceMatte.hairDepthNear), Float(CallFaceMatte.hairDepthFar))
            }
            let uniforms: [SIMD4<Float>] = [
                regionValue,
                SIMD4(Float(upright.width), Float(upright.height), Float(fresh ? 1 : CallFaceMatte.hairRate), 0),
                SIMD4(Float(output.w), Float(output.h), 0, 0),
                depthState, depthRows.0, depthRows.1,
            ]
            guard NativeSegmentPass.render(commands, into: spare, frame, textures: [current, depth?.texture ?? current], uniforms: uniforms,
                                           buffers: [(network.arena, network.outputBytes, 1)])
            else { return nil }
            swap(&current, &spare)
        }
        ping = current
        pong = spare
        fresh = false
        return current
    }
}

final class NativePersonMatte {
    private let device: MTLDevice
    private let fuse: MTLRenderPipelineState
    private let temporal: MTLRenderPipelineState
    private let seed: MTLRenderPipelineState
    private let grow: MTLRenderPipelineState
    private let clean: MTLRenderPipelineState
    private let moments: MTLRenderPipelineState
    private let box: MTLRenderPipelineState
    private let coefficients: MTLRenderPipelineState
    private var state: (size: CallFaceSize, ping: MTLTexture, pong: MTLTexture, clean: MTLTexture, fused: MTLTexture)?
    private var regions: (size: CallFaceSize, ping: MTLTexture, pong: MTLTexture, keep: MTLTexture)?
    private var guide: (size: CallFaceSize, a: MTLTexture, b: MTLTexture, coefficients: MTLTexture)?
    private var first = true
    private var regionsFresh = true
    private var regionMap: CallFaceFrameMap?
    var guideWidth = CallFaceMatte.guideWidth

    init?(device: MTLDevice, library: MTLLibrary) {
        func make(_ name: String, _ format: MTLPixelFormat) -> MTLRenderPipelineState? { NativeSegmentPass.pipeline(device, library, name, format) }
        guard let fuse = make("fmMatteFuse", .r16Float),
            let temporal = make("fmMatteTemporal", .r16Float),
            let seed = make("fmMatteSeed", .rg16Float),
            let grow = make("fmMatteGrow", .rg16Float),
            let clean = make("fmMatteClean", .r16Float),
            let moments = make("fmMatteMoments", .rgba16Float),
            let box = make("fmMatteBox", .rgba16Float),
            let coefficients = make("fmMatteCoefficients", .rgba16Float)
        else { return nil }
        self.device = device
        self.fuse = fuse
        self.temporal = temporal
        self.seed = seed
        self.grow = grow
        self.clean = clean
        self.moments = moments
        self.box = box
        self.coefficients = coefficients
    }

    var ready: Bool { state != nil && guide != nil && !first }
    var mask: MTLTexture? { state?.clean }
    var coefficientTexture: MTLTexture? { guide?.coefficients }

    func reset() {
        first = true
    }

    func update(_ commands: MTLCommandBuffer, camera: MTLTexture, map: CallFaceFrameMap, raw: MTLTexture?, depth: NativeFaceDepth?, face: [Double]) -> Bool {
        if let raw {
            let rawSize = CallFaceSize(raw.width, raw.height)
            if state?.size != rawSize {
                guard let ping = NativeSegmentPass.texture(device, .r16Float, rawSize), let pong = NativeSegmentPass.texture(device, .r16Float, rawSize),
                    let cleanTexture = NativeSegmentPass.texture(device, .r16Float, rawSize), let fused = NativeSegmentPass.texture(device, .r16Float, rawSize)
                else { return false }
                state = (rawSize, ping, pong, cleanTexture, fused)
                first = true
                regions = nil
            }
            guard var current = state else { return false }
            var source = raw
            if let depth {
                let m = depth.toDepthUv
                let uniforms: [SIMD4<Float>] = [
                    SIMD4(Float(depth.faceMetres), Float(CallFaceMatte.depthNear), Float(CallFaceMatte.depthFar), Float(CallFaceMatte.depthAgree)),
                    SIMD4(Float(m[0]), Float(m[2]), Float(m[4]), 0),
                    SIMD4(Float(m[1]), Float(m[3]), Float(m[5]), 0),
                ]
                guard NativeSegmentPass.render(commands, into: current.fused, fuse, textures: [raw, depth.texture], uniforms: uniforms) else { return false }
                source = current.fused
            }
            let uniforms: [SIMD4<Float>] = [SIMD4(first ? 1 : 0, Float(CallFaceMatte.hold), 0, 0)]
            guard NativeSegmentPass.render(commands, into: current.pong, temporal, textures: [source, current.ping], uniforms: uniforms) else { return false }
            swap(&current.ping, &current.pong)
            state = current
            first = false
        }
        guard let current = state else { return false }
        if regions == nil || regionMap != map {
            let size = CallFaceMatte.regionSize(rawWidth: current.size.width, rawHeight: current.size.height, map: map)
            guard let ping = NativeSegmentPass.texture(device, .rg16Float, size), let pong = NativeSegmentPass.texture(device, .rg16Float, size),
                let keep = NativeSegmentPass.texture(device, .rg16Float, size)
            else { return false }
            regions = (size, ping, pong, keep)
            regionMap = map
            regionsFresh = true
        }
        guard var region = regions else { return false }
        let rw = Float(region.size.width)
        let rh = Float(region.size.height)
        let up = NativeSegmentPass.rows(map.toUprightUvMatrix, 0, 0)
        let seedUniforms: [SIMD4<Float>] = [
            SIMD4(0.5 / rw, 0.5 / rh, regionsFresh ? 1 : 0, 0),
            SIMD4(Float(face[0]), Float(face[1]), Float(face[2]), 0),
            up.0, up.1,
        ]
        guard NativeSegmentPass.render(commands, into: region.ping, seed, textures: [current.ping, region.keep], uniforms: seedUniforms) else { return false }
        let step: [SIMD4<Float>] = [SIMD4(1 / rw, 1 / rh, 0, 0)]
        for _ in 0..<CallFaceMatte.growRounds {
            guard NativeSegmentPass.render(commands, into: region.pong, grow, textures: [region.ping], uniforms: step) else { return false }
            swap(&region.ping, &region.pong)
        }
        swap(&region.keep, &region.ping)
        regions = region
        regionsFresh = false
        guard NativeSegmentPass.render(commands, into: current.clean, clean, textures: [current.ping, region.keep], uniforms: step) else { return false }
        let size = CallFaceMatte.guideSize(max(CallFaceMatte.minimumGuide, guideWidth), map: map)
        if guide?.size != size {
            guard let a = NativeSegmentPass.texture(device, .rgba16Float, size), let b = NativeSegmentPass.texture(device, .rgba16Float, size),
                let made = NativeSegmentPass.texture(device, .rgba16Float, size)
            else { return false }
            guide = (size, a, b, made)
        }
        guard let g = guide else { return false }
        let gw = Float(g.size.width)
        let gh = Float(g.size.height)
        let horizontal: [SIMD4<Float>] = [SIMD4(1 / gw, 0, 0, 0)]
        let vertical: [SIMD4<Float>] = [SIMD4(0, 1 / gh, 0, 0)]
        guard NativeSegmentPass.render(commands, into: g.a, moments, textures: [camera, current.clean], uniforms: [SIMD4(0.25 / gw, 0.25 / gh, 0, 0)]),
            NativeSegmentPass.render(commands, into: g.b, box, textures: [g.a], uniforms: horizontal),
            NativeSegmentPass.render(commands, into: g.a, box, textures: [g.b], uniforms: vertical),
            NativeSegmentPass.render(commands, into: g.b, coefficients, textures: [g.a], uniforms: [SIMD4(Float(CallFaceMatte.eps), 0, 0, 0)]),
            NativeSegmentPass.render(commands, into: g.a, box, textures: [g.b], uniforms: horizontal),
            NativeSegmentPass.render(commands, into: g.coefficients, box, textures: [g.a], uniforms: vertical)
        else { return false }
        return true
    }
}
