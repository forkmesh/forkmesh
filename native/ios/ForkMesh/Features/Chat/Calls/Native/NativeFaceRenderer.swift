import CoreVideo
import FMChat
import Foundation
import ImageIO
import Metal
import QuartzCore
import Vision

enum NativeFaceBackdrop: Equatable {
    case none
    case follow
    case blur(Double)

    var mode: Int {
        switch self {
        case .none: return 0
        case .blur: return 1
        case .follow: return 2
        }
    }
}

struct NativeFaceTrackingFrame {
    let session: Int
    let vertices: [CallFacePoint]
    let triangles: [Int]
    let shapes: CallFaceBlendShapes?
    let focal: Double
    let faceMetres: Double
    let depth: CVPixelBuffer?
    let depthAgeMs: Double
}

final class NativeFaceRenderer: @unchecked Sendable {
    struct Output {
        let frame: CVPixelBuffer
        let keep: CVPixelBuffer
        let backgroundDone: Bool
    }

    static let syncBudgetMs = 16.0
    static let syncReturnMs = 10.0
    static let costShare = 0.2
    static let warmUpRuns = 2

    private let lock = NSLock()
    private let loader = DispatchQueue(label: "com.forkmesh.native-face-load", qos: .userInitiated)
    private let detector = DispatchQueue(label: "com.forkmesh.native-face-detect", qos: .userInteractive)
    private let learner = DispatchQueue(label: "com.forkmesh.native-face-learn", qos: .userInitiated)
    private var faceId = ""
    private var faceImage = ""
    private var faceAsset: NativeFaceAsset?
    private var faceToken = 0
    private var outfitId = ""
    private var outfitTexture: MTLTexture?
    private var outfitToken = 0
    private var gpu: NativeFaceGPU?
    private var gpuTried = false
    private var detectCost = -1.0
    private var detectRuns = 0
    private var detectAsync = false
    private var detectBusy = false
    private var pending: CallFaceStageInput?
    private var governor = CallFaceQualityGovernor()
    private var governed = false
    private var binding: CallFaceMeshBinding?
    private var bindingSession = -1
    private var learnBusy = false
    private var learned: (points: [CallFacePoint], vertices: [CallFacePoint], time: Double, session: Int)?

    private let stage = CallFaceStage()
    private var stageFaceToken = -1
    private var stageOutfitToken = -1
    private var stageIdle = true
    private var lastMap: CallFaceFrameMap?
    private var frameCount = 0
    private var cadence = 0
    private let syncRequest = NativeFaceRenderer.landmarkRequest()
    private let asyncRequest = NativeFaceRenderer.landmarkRequest()
    private let learnRequest = NativeFaceRenderer.landmarkRequest()

    var wantsFrames: Bool {
        lock.lock()
        defer { lock.unlock() }
        return !faceId.isEmpty || outfitTexture != nil
    }

    var wantsFaceTracking: Bool {
        lock.lock()
        defer { lock.unlock() }
        return !faceId.isEmpty
    }

    var fastSegmentation: Bool {
        lock.lock()
        defer { lock.unlock() }
        return governor.level.fastSegmentation
    }

    var qualityIndex: Int {
        lock.lock()
        defer { lock.unlock() }
        return governor.index
    }

    func setFace(_ id: String, customImage: String) {
        let known = id.isEmpty || CallFaces.byId(id) != nil || (id == CallFaceCustom.id && !customImage.isEmpty)
        let next = known ? id : ""
        let image = next == CallFaceCustom.id ? customImage : ""
        lock.lock()
        if next == faceId && image == faceImage {
            lock.unlock()
            return
        }
        faceId = next
        faceImage = image
        faceAsset = nil
        faceToken += 1
        let token = faceToken
        lock.unlock()
        guard !next.isEmpty else { return }
        loader.async { [weak self] in
            guard let self, let gpu = self.readyGPU() else { return }
            gpu.prepareHair()
            let asset = next == CallFaceCustom.id
                ? NativeFaceAssets.custom(dataURL: image, device: gpu.device, queue: gpu.queue)
                : NativeFaceAssets.builtIn(next, device: gpu.device, queue: gpu.queue)
            self.lock.lock()
            if token == self.faceToken { self.faceAsset = asset }
            self.lock.unlock()
        }
    }

    func setOutfit(_ id: String) {
        let next = CallOutfits.byId(id) == nil ? "" : id
        lock.lock()
        if next == outfitId {
            lock.unlock()
            return
        }
        outfitId = next
        outfitTexture = nil
        outfitToken += 1
        let token = outfitToken
        lock.unlock()
        guard !next.isEmpty else { return }
        loader.async { [weak self] in
            guard let self, let gpu = self.readyGPU() else { return }
            let texture = NativeOutfitPainter.texture(next, device: gpu.device, queue: gpu.queue)
            self.lock.lock()
            if token == self.outfitToken {
                self.outfitTexture = texture
                self.outfitToken += 1
            }
            self.lock.unlock()
        }
    }

    private func readyGPU() -> NativeFaceGPU? {
        lock.lock()
        if gpuTried {
            let made = gpu
            lock.unlock()
            return made
        }
        lock.unlock()
        let made = NativeFaceGPU()
        lock.lock()
        if !gpuTried {
            gpuTried = true
            gpu = made
            if let made, !governed {
                governor = CallFaceQualityGovernor(start: CallFaceQuality.startLevel(appleFamily: made.appleFamily))
                governed = true
            }
        }
        let current = gpu
        lock.unlock()
        return current
    }

    static func thermal() -> CallFaceThermal {
        CallFaceThermal(rawValue: ProcessInfo.processInfo.thermalState.rawValue) ?? .nominal
    }

    func render(_ buffer: CVPixelBuffer, degrees: Int, timeNs: Int64, background: NativeFaceBackdrop, tracking: NativeFaceTrackingFrame? = nil,
                mask: () -> CVPixelBuffer?) -> Output? {
        let began = CACurrentMediaTime()
        lock.lock()
        let gpu = self.gpu
        let hasFace = !faceId.isEmpty
        let asset = faceAsset
        let token = faceToken
        let outfit = outfitTexture
        let outfitStamp = outfitToken
        let level = governor.level
        lock.unlock()
        guard let gpu, hasFace || outfit != nil else {
            if !stageIdle {
                stage.reset()
                stageIdle = true
                gpu?.resetMattes()
            }
            return nil
        }
        stageIdle = false
        if token != stageFaceToken {
            stageFaceToken = token
            stage.faceChanged()
        }
        if outfitStamp != stageOutfitToken {
            stageOutfitToken = outfitStamp
            stage.outfitChanged()
        }
        stage.hasFace = hasFace
        stage.faceReady = asset != nil
        stage.hasOutfit = outfit != nil
        let map = CallFaceFrameMap(bufferWidth: CVPixelBufferGetWidth(buffer), bufferHeight: CVPixelBufferGetHeight(buffer), degrees: degrees)
        if map != lastMap {
            if lastMap != nil { stage.sizeChanged() }
            lastMap = map
        }
        let time = Double(timeNs) / 1_000_000
        let frame = stage.step(locate(buffer, map: map, time: time, probe: hasFace, tracking: hasFace ? tracking : nil), time: time)
        let plan = CallFaceDrawPlan.make(head: asset?.head, frame: frame, hasOutfit: outfit != nil, width: Double(map.uprightWidth), height: Double(map.uprightHeight))
        guard plan.drawsAnything else { return nil }
        cadence = (cadence + 1) % 3600
        let wantsMatte = plan.showFace || plan.wearing || background != .none
        let fresh = wantsMatte && (level.runsSegment(frame: cadence) || !gpu.matteReady) ? mask() : nil
        frameCount = (frameCount + 1) % 4096
        let seed = (Double(frameCount) * 7.31).truncatingRemainder(dividingBy: 97)
        let depth = tracking.flatMap { frame -> NativeFaceGPU.DepthInput? in
            guard let map = frame.depth, frame.depthAgeMs <= CallFaceMatte.depthStaleMs, CallFaceDepth.usable(frame.faceMetres) else { return nil }
            return NativeFaceGPU.DepthInput(buffer: map, faceMetres: frame.faceMetres)
        }
        let output = gpu.draw(
            buffer, map: map, plan: plan, asset: asset, outfit: outfit, matte: fresh, wantsMatte: wantsMatte, background: background,
            runHair: level.runsHair(frame: cadence), guide: level.guide, depth: depth, seed: seed)
        let cost = (CACurrentMediaTime() - began) * 1000
        lock.lock()
        governor.note(costMs: cost, thermal: Self.thermal())
        lock.unlock()
        return output
    }

    private func bindingFor(_ tracking: NativeFaceTrackingFrame) -> CallFaceMeshBinding {
        if let binding, bindingSession == tracking.session, binding.triangles.count == tracking.triangles.count { return binding }
        let made = CallFaceMeshBinding(triangles: tracking.triangles)
        binding = made
        bindingSession = tracking.session
        lock.lock()
        learned = nil
        lock.unlock()
        return made
    }

    private func locate(_ buffer: CVPixelBuffer, map: CallFaceFrameMap, time: Double, probe: Bool, tracking: NativeFaceTrackingFrame?) -> CallFaceStageInput {
        guard let tracking, let shapes = tracking.shapes, !tracking.vertices.isEmpty, !tracking.triangles.isEmpty else {
            stage.blend = nil
            return locate(buffer, map: map, time: time, probe: probe, learnFrom: nil)
        }
        let binding = bindingFor(tracking)
        lock.lock()
        let fresh = learned
        learned = nil
        lock.unlock()
        if let fresh, fresh.session == tracking.session {
            binding.learn(landmarks: fresh.points, vertices: fresh.vertices, time: fresh.time)
        }
        stage.blend = shapes
        let upright = tracking.vertices.map { vertex -> CallFacePoint in
            let p = map.toUpright(vertex)
            return CallFacePoint(p.x, p.y, vertex.z)
        }
        guard binding.ready, let points = binding.place(upright, focal: tracking.focal) else {
            return locate(buffer, map: map, time: time, probe: probe, learnFrom: binding.unreliable ? nil : (upright, tracking.session))
        }
        if binding.due(time: time) { learn(buffer, map: map, vertices: upright, time: time, session: tracking.session) }
        let sample = probe ? Self.probeSample(buffer, map: map, points: points) : nil
        return .found(points: points, pupils: nil, probe: sample, time: time)
    }

    private func learn(_ buffer: CVPixelBuffer, map: CallFaceFrameMap, vertices: [CallFacePoint], time: Double, session: Int) {
        lock.lock()
        let start = !learnBusy
        if start { learnBusy = true }
        lock.unlock()
        guard start else { return }
        learner.async { [weak self] in
            guard let self else { return }
            let result = Self.detect(buffer, map: map, time: time, probe: false, request: self.learnRequest)
            self.lock.lock()
            if case let .found(points, _, _, _) = result { self.learned = (points, vertices, time, session) }
            self.learnBusy = false
            self.lock.unlock()
        }
    }

    private func locate(_ buffer: CVPixelBuffer, map: CallFaceFrameMap, time: Double, probe: Bool,
                        learnFrom: (vertices: [CallFacePoint], session: Int)?) -> CallFaceStageInput {
        lock.lock()
        let async = detectAsync
        let ready = pending
        pending = nil
        let start = async && !detectBusy
        if start { detectBusy = true }
        lock.unlock()
        if !async {
            let began = CACurrentMediaTime()
            let result = Self.detect(buffer, map: map, time: time, probe: probe, request: syncRequest)
            note(cost: (CACurrentMediaTime() - began) * 1000)
            keep(result, learnFrom: learnFrom, time: time)
            return result
        }
        if start {
            detector.async { [weak self] in
                guard let self else { return }
                let began = CACurrentMediaTime()
                let result = Self.detect(buffer, map: map, time: time, probe: probe, request: self.asyncRequest)
                let cost = (CACurrentMediaTime() - began) * 1000
                self.keep(result, learnFrom: learnFrom, time: time)
                self.lock.lock()
                self.pending = result
                self.detectBusy = false
                self.lock.unlock()
                self.note(cost: cost)
            }
        }
        return ready ?? .stale
    }

    private func keep(_ result: CallFaceStageInput, learnFrom: (vertices: [CallFacePoint], session: Int)?, time: Double) {
        guard let learnFrom, case let .found(points, _, _, _) = result else { return }
        lock.lock()
        learned = (points, learnFrom.vertices, time, learnFrom.session)
        lock.unlock()
    }

    private func note(cost: Double) {
        lock.lock()
        detectRuns += 1
        guard detectRuns > Self.warmUpRuns else {
            lock.unlock()
            return
        }
        detectCost = detectCost < 0 ? cost : detectCost + (cost - detectCost) * Self.costShare
        if !detectAsync && detectCost > Self.syncBudgetMs { detectAsync = true }
        if detectAsync && detectCost < Self.syncReturnMs { detectAsync = false }
        lock.unlock()
    }

    static func landmarkRequest() -> VNDetectFaceLandmarksRequest {
        let request = VNDetectFaceLandmarksRequest()
        request.constellation = .constellation76Points
        return request
    }

    static func orientation(_ degrees: Int) -> CGImagePropertyOrientation {
        switch degrees {
        case 90: return .right
        case 180: return .down
        case 270: return .left
        default: return .up
        }
    }

    static func detect(_ buffer: CVPixelBuffer, map: CallFaceFrameMap, time: Double, probe: Bool, request: VNDetectFaceLandmarksRequest) -> CallFaceStageInput {
        let handler = VNImageRequestHandler(cvPixelBuffer: buffer, orientation: orientation(map.degrees), options: [:])
        guard (try? handler.perform([request])) != nil else { return .missing }
        let size = CGSize(width: map.uprightWidth, height: map.uprightHeight)
        let located = CallFaces.located(CallFaceLandmarks.regions(request.results ?? [], size: size))
        guard let points = located.points else { return .missing }
        let sample = probe ? probeSample(buffer, map: map, points: points) : nil
        return .found(points: points, pupils: located.pupils, probe: sample, time: time)
    }

    static func probeSample(_ buffer: CVPixelBuffer, map: CallFaceFrameMap, points: [CallFacePoint]) -> CallFaceProbeSample? {
        let crop = CallFaceExpression.probeCrop(points)
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let reader = NativeCameraReader(buffer) else { return nil }
        let image = CallFaceStage.probeImage(crop: crop) { x, y in
            let at = map.toBuffer(CallFacePoint(x, y))
            return reader.rgb(at.x, at.y)
        }
        return CallFaceProbeSample(crop: crop, image: image)
    }
}

struct NativeCameraTone {
    let range: SIMD4<Float>
    let matrix: SIMD4<Float>

    init(_ buffer: CVPixelBuffer) {
        let video = CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        range = video ? SIMD4(16 / 255, 255 / 219, 128 / 255, 255 / 224) : SIMD4(0, 1, 128 / 255, 1)
        let attached = CVBufferCopyAttachment(buffer, kCVImageBufferYCbCrMatrixKey, nil) as? String
        let hd = attached == (kCVImageBufferYCbCrMatrix_ITU_R_709_2 as String)
        matrix = hd ? SIMD4(1.5748, 0.187324, 0.468124, 1.8556) : SIMD4(1.402, 0.344136, 0.714136, 1.772)
    }

    static func yuv(_ buffer: CVPixelBuffer) -> Bool? {
        switch CVPixelBufferGetPixelFormatType(buffer) {
        case kCVPixelFormatType_420YpCbCr8BiPlanarFullRange, kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange: return true
        case kCVPixelFormatType_32BGRA: return false
        default: return nil
        }
    }
}

struct NativeCameraReader {
    let width: Int
    let height: Int
    let yuv: Bool
    let first: UnsafePointer<UInt8>
    let firstStride: Int
    let second: UnsafePointer<UInt8>?
    let secondStride: Int
    let tone: NativeCameraTone

    init?(_ buffer: CVPixelBuffer) {
        guard let yuv = NativeCameraTone.yuv(buffer) else { return nil }
        width = CVPixelBufferGetWidth(buffer)
        height = CVPixelBufferGetHeight(buffer)
        self.yuv = yuv
        tone = NativeCameraTone(buffer)
        if yuv {
            guard CVPixelBufferGetPlaneCount(buffer) >= 2,
                let luma = CVPixelBufferGetBaseAddressOfPlane(buffer, 0),
                let chroma = CVPixelBufferGetBaseAddressOfPlane(buffer, 1)
            else { return nil }
            first = UnsafePointer(luma.assumingMemoryBound(to: UInt8.self))
            firstStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 0)
            second = UnsafePointer(chroma.assumingMemoryBound(to: UInt8.self))
            secondStride = CVPixelBufferGetBytesPerRowOfPlane(buffer, 1)
        } else {
            guard let base = CVPixelBufferGetBaseAddress(buffer) else { return nil }
            first = UnsafePointer(base.assumingMemoryBound(to: UInt8.self))
            firstStride = CVPixelBufferGetBytesPerRow(buffer)
            second = nil
            secondStride = 0
        }
    }

    func bilinear(_ plane: UnsafePointer<UInt8>, stride: Int, step: Int, channel: Int, width: Int, height: Int, _ x: Double, _ y: Double) -> Double {
        let fx = max(0, min(Double(width - 1), x - 0.5))
        let fy = max(0, min(Double(height - 1), y - 0.5))
        let x0 = Int(fx)
        let y0 = Int(fy)
        let x1 = min(width - 1, x0 + 1)
        let y1 = min(height - 1, y0 + 1)
        let tx = fx - Double(x0)
        let ty = fy - Double(y0)
        func at(_ px: Int, _ py: Int) -> Double { Double(plane[py * stride + px * step + channel]) }
        let top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * tx
        let bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * tx
        return top + (bottom - top) * ty
    }

    func rgb(_ x: Double, _ y: Double) -> (Double, Double, Double)? {
        guard x >= 0, y >= 0, x <= Double(width), y <= Double(height) else { return nil }
        if !yuv {
            return (
                bilinear(first, stride: firstStride, step: 4, channel: 2, width: width, height: height, x, y),
                bilinear(first, stride: firstStride, step: 4, channel: 1, width: width, height: height, x, y),
                bilinear(first, stride: firstStride, step: 4, channel: 0, width: width, height: height, x, y)
            )
        }
        guard let second else { return nil }
        let cw = max(1, (width + 1) / 2)
        let ch = max(1, (height + 1) / 2)
        let range = tone.range
        let matrix = tone.matrix
        let luma = (bilinear(first, stride: firstStride, step: 1, channel: 0, width: width, height: height, x, y) - Double(range.x) * 255) * Double(range.y)
        let cb = (bilinear(second, stride: secondStride, step: 2, channel: 0, width: cw, height: ch, x / 2, y / 2) - Double(range.z) * 255) * Double(range.w)
        let cr = (bilinear(second, stride: secondStride, step: 2, channel: 1, width: cw, height: ch, x / 2, y / 2) - Double(range.z) * 255) * Double(range.w)
        return (
            luma + Double(matrix.x) * cr,
            luma - Double(matrix.y) * cb - Double(matrix.z) * cr,
            luma + Double(matrix.w) * cb
        )
    }
}

final class NativeFaceGPU {
    struct Targets {
        let map: CallFaceFrameMap
        let camera: MTLTexture
        let warp: MTLTexture
        let aux: MTLTexture
        let depth: MTLTexture
        let lowCam: MTLTexture
        let lowFace: MTLTexture
        let plain: MTLTexture
        let wideCam: MTLTexture
        let wideFace: MTLTexture
        let spare: MTLTexture
        let lowScale: Double
        let pyramid: [MTLTexture]
        let pulled: [MTLTexture]
        let blurA: MTLTexture
        let blurB: MTLTexture
        let backdrop: MTLTexture
    }

    struct DepthInput {
        let buffer: CVPixelBuffer
        let faceMetres: Double
    }

    let device: MTLDevice
    let queue: MTLCommandQueue
    let appleFamily: Int
    private let library: MTLLibrary
    private let person: NativePersonMatte?
    private let hairLock = NSLock()
    private var hair: NativeHairMatte?
    private var hairTried = false
    private let cache: CVMetalTextureCache
    private let cameraYuv: MTLRenderPipelineState
    private let cameraRgb: MTLRenderPipelineState
    private let warp: MTLRenderPipelineState
    private let reduce: MTLRenderPipelineState
    private let blur: MTLRenderPipelineState
    private let weigh: MTLRenderPipelineState
    private let push: MTLRenderPipelineState
    private let pull: MTLRenderPipelineState
    private let backdrop: MTLRenderPipelineState
    private let composite: MTLRenderPipelineState
    private let depthState: MTLDepthStencilState
    private let blank: MTLTexture
    private var vertexBuffer: MTLBuffer?
    private var targets: Targets?
    private var maskTexture: MTLTexture?
    private var depthCopy: MTLTexture?
    private var outputPool: CVPixelBufferPool?
    private var keepPool: CVPixelBufferPool?
    private var poolWidth = 0
    private var poolHeight = 0
    private var failed = false

    init?() {
        guard let device = MTLCreateSystemDefaultDevice(),
            let queue = device.makeCommandQueue(),
            let library = device.makeDefaultLibrary()
        else { return nil }
        var made: CVMetalTextureCache?
        guard CVMetalTextureCacheCreate(nil, nil, device, nil, &made) == kCVReturnSuccess, let cache = made else { return nil }
        func pipeline(_ vertex: String, _ fragment: String, _ colors: [MTLPixelFormat], depth: MTLPixelFormat = .invalid) -> MTLRenderPipelineState? {
            guard let vertexFunction = library.makeFunction(name: vertex), let fragmentFunction = library.makeFunction(name: fragment) else { return nil }
            let descriptor = MTLRenderPipelineDescriptor()
            descriptor.vertexFunction = vertexFunction
            descriptor.fragmentFunction = fragmentFunction
            for (index, format) in colors.enumerated() { descriptor.colorAttachments[index].pixelFormat = format }
            descriptor.depthAttachmentPixelFormat = depth
            return try? device.makeRenderPipelineState(descriptor: descriptor)
        }
        let half: [MTLPixelFormat] = [.rgba16Float]
        guard let cameraYuv = pipeline("fmFaceQuadVertex", "fmFaceCameraYuv", [.rgba8Unorm]),
            let cameraRgb = pipeline("fmFaceQuadVertex", "fmFaceCameraRgb", [.rgba8Unorm]),
            let warp = pipeline("fmFaceWarpVertex", "fmFaceWarpFragment", [.rgba8Unorm, .rgba8Unorm], depth: .depth32Float),
            let reduce = pipeline("fmFaceQuadVertex", "fmFaceReduce", half),
            let blur = pipeline("fmFaceQuadVertex", "fmFaceBlur", half),
            let weigh = pipeline("fmFaceQuadVertex", "fmFaceWeigh", half),
            let push = pipeline("fmFaceQuadVertex", "fmFacePush", half),
            let pull = pipeline("fmFaceQuadVertex", "fmFacePull", half),
            let backdrop = pipeline("fmFaceQuadVertex", "fmFaceBackdrop", half),
            let composite = pipeline("fmFaceQuadVertex", "fmFaceComposite", [.bgra8Unorm, .r8Unorm])
        else { return nil }
        let depthDescriptor = MTLDepthStencilDescriptor()
        depthDescriptor.depthCompareFunction = .less
        depthDescriptor.isDepthWriteEnabled = true
        let blankDescriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .rgba8Unorm, width: 1, height: 1, mipmapped: false)
        blankDescriptor.usage = .shaderRead
        blankDescriptor.storageMode = .shared
        guard let depthState = device.makeDepthStencilState(descriptor: depthDescriptor),
            let blank = device.makeTexture(descriptor: blankDescriptor)
        else { return nil }
        let zero: [UInt8] = [0, 0, 0, 0]
        zero.withUnsafeBytes { raw in
            if let base = raw.baseAddress { blank.replace(region: MTLRegionMake2D(0, 0, 1, 1), mipmapLevel: 0, withBytes: base, bytesPerRow: 4) }
        }
        self.device = device
        self.queue = queue
        self.library = library
        person = NativePersonMatte(device: device, library: library)
        let families: [(MTLGPUFamily, Int)] = [(.apple9, 9), (.apple8, 8), (.apple7, 7), (.apple6, 6), (.apple5, 5), (.apple4, 4)]
        appleFamily = families.first { device.supportsFamily($0.0) }?.1 ?? 0
        self.cache = cache
        self.cameraYuv = cameraYuv
        self.cameraRgb = cameraRgb
        self.warp = warp
        self.reduce = reduce
        self.blur = blur
        self.weigh = weigh
        self.push = push
        self.pull = pull
        self.backdrop = backdrop
        self.composite = composite
        self.depthState = depthState
        self.blank = blank
    }

    var matteReady: Bool { person?.ready ?? false }

    func prepareHair() {
        hairLock.lock()
        let tried = hairTried
        hairTried = true
        hairLock.unlock()
        guard !tried, let model = NativeSegmentModels.hair() else { return }
        let made = NativeHairMatte(device: device, library: library, model: model)
        hairLock.lock()
        hair = made
        hairLock.unlock()
    }

    private func currentHair() -> NativeHairMatte? {
        hairLock.lock()
        defer { hairLock.unlock() }
        return hair
    }

    func resetMattes() {
        person?.reset()
        currentHair()?.reset()
    }

    func draw(
        _ buffer: CVPixelBuffer, map: CallFaceFrameMap, plan: CallFaceDrawPlan, asset: NativeFaceAsset?, outfit: MTLTexture?,
        matte: CVPixelBuffer?, wantsMatte: Bool, background: NativeFaceBackdrop, runHair: Bool, guide: Int, depth: DepthInput?, seed: Double
    ) -> NativeFaceRenderer.Output? {
        guard let yuv = NativeCameraTone.yuv(buffer),
            let targets = targets(for: map),
            let output = makeBuffer(width: map.bufferWidth, height: map.bufferHeight, format: kCVPixelFormatType_32BGRA, pool: \.outputPool),
            let keep = makeBuffer(width: map.bufferWidth, height: map.bufferHeight, format: kCVPixelFormatType_OneComponent8, pool: \.keepPool),
            let outputTexture = texture(output, plane: 0, format: .bgra8Unorm),
            let keepTexture = texture(keep, plane: 0, format: .r8Unorm),
            let commands = queue.makeCommandBuffer()
        else { return nil }
        failed = false
        var held: [CVMetalTexture] = [outputTexture.0, keepTexture.0]
        if yuv {
            guard let luma = texture(buffer, plane: 0, format: .r8Unorm), let chroma = texture(buffer, plane: 1, format: .rg8Unorm) else { return nil }
            held.append(luma.0)
            held.append(chroma.0)
            let tone = NativeCameraTone(buffer)
            pass(commands, into: targets.camera, cameraYuv, [luma.1, chroma.1], [tone.range, tone.matrix])
        } else {
            guard let source = texture(buffer, plane: -1, format: .bgra8Unorm) else { return nil }
            held.append(source.0)
            pass(commands, into: targets.camera, cameraRgb, [source.1], [])
        }
        var depthInfo: NativeFaceDepth?
        if let depth, let made = depthTexture(depth.buffer, held: &held) {
            depthInfo = NativeFaceDepth(
                texture: made, faceMetres: depth.faceMetres,
                toDepthUv: CallFaceMatte.depthUv(
                    imageWidth: map.bufferWidth, imageHeight: map.bufferHeight,
                    depthWidth: CVPixelBufferGetWidth(depth.buffer), depthHeight: CVPixelBufferGetHeight(depth.buffer)))
        }
        var hasMatte = false
        if wantsMatte, let person {
            person.guideWidth = guide
            let raw = matte.flatMap(upload)
            hasMatte = person.update(commands, camera: targets.camera, map: map, raw: raw, depth: raw == nil ? nil : depthInfo, face: plan.seed) && person.ready
        }
        let coef = hasMatte ? person?.coefficientTexture ?? blank : blank
        let clean = hasMatte ? person?.mask ?? blank : blank
        let showFace = plan.showFace && asset != nil && !plan.vertices.isEmpty
        if showFace, let asset {
            drawWarp(commands, targets: targets, plan: plan, asset: asset)
            let sigma = CallFaceDrawPlan.toneSigma(span: plan.span, scale: targets.lowScale)
            reduceInto(commands, targets.lowCam, mode: 0, targets: targets)
            blurInto(commands, targets.lowCam, sigma: sigma, spare: targets.spare)
            reduceInto(commands, targets.lowFace, mode: 1, targets: targets)
            blurInto(commands, targets.lowFace, sigma: sigma, spare: targets.spare)
            let wide = CallFaceDrawPlan.wideSigma(span: plan.span, scale: targets.lowScale)
            reduceInto(commands, targets.wideCam, mode: 0, targets: targets)
            reduceInto(commands, targets.wideFace, mode: 1, targets: targets)
            for _ in 0..<CallFaceDrawPlan.widePasses {
                blurInto(commands, targets.wideCam, sigma: wide, spare: targets.spare)
                blurInto(commands, targets.wideFace, sigma: wide, spare: targets.spare)
            }
        }
        var hairTexture: MTLTexture?
        if showFace && hasMatte, let region = plan.hairRegion, let hair = currentHair() {
            hairTexture = hair.update(commands, camera: targets.camera, map: map, region: region, run: runHair, depth: depthInfo)
        }
        let wearing = plan.wearing && hasMatte && outfit != nil
        var blurStrength: Double?
        if case let .blur(strength) = background, hasMatte { blurStrength = strength }
        var fill = targets.camera
        var behind = targets.camera
        if hasMatte && (showFace || wearing || blurStrength != nil) { fill = buildFill(commands, targets: targets, coef: coef, clean: clean) }
        if let blurStrength {
            behind = buildBackdrop(commands, targets: targets, strength: blurStrength)
        } else if hasMatte && showFace && background == .none {
            behind = buildBackdrop(commands, targets: targets, strength: CallFaceDrawPlan.haloStrength)
        }
        if plan.hide > 0.001 {
            reduceInto(commands, targets.plain, mode: 2, targets: targets)
            for _ in 0..<CallFaceDrawPlan.hidePasses {
                blurInto(commands, targets.plain, sigma: CallFaceDrawPlan.hideSigma, spare: targets.spare)
            }
        }
        let uniforms = compositeUniforms(
            map: map, plan: plan, showFace: showFace, wearing: wearing, hasMatte: hasMatte, follow: background == .follow, seed: seed,
            hair: hairTexture != nil, blur: blurStrength != nil)
        let descriptor = MTLRenderPassDescriptor()
        descriptor.colorAttachments[0].texture = outputTexture.1
        descriptor.colorAttachments[0].loadAction = .dontCare
        descriptor.colorAttachments[0].storeAction = .store
        descriptor.colorAttachments[1].texture = keepTexture.1
        descriptor.colorAttachments[1].loadAction = .dontCare
        descriptor.colorAttachments[1].storeAction = .store
        if let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor) {
            encoder.setRenderPipelineState(composite)
            let textures: [MTLTexture] = [
                targets.camera, coef, clean, targets.warp, targets.aux, targets.lowCam, targets.lowFace, targets.wideCam, targets.wideFace,
                fill, behind, targets.plain, outfit ?? blank, hairTexture ?? blank,
            ]
            for (index, texture) in textures.enumerated() { encoder.setFragmentTexture(texture, index: index) }
            bytes(encoder, uniforms)
            encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
            encoder.endEncoding()
        } else {
            failed = true
        }
        guard !failed else { return nil }
        commands.commit()
        commands.waitUntilCompleted()
        withExtendedLifetime(held) {}
        guard commands.status == .completed else { return nil }
        return NativeFaceRenderer.Output(frame: output, keep: keep, backgroundDone: blurStrength != nil)
    }

    private func compositeUniforms(
        map: CallFaceFrameMap, plan: CallFaceDrawPlan, showFace: Bool, wearing: Bool, hasMatte: Bool, follow: Bool, seed: Double, hair: Bool, blur: Bool
    ) -> [SIMD4<Float>] {
        let up = map.toUprightMatrix
        let width = Float(map.bufferWidth)
        let height = Float(map.bufferHeight)
        func f(_ value: Double) -> Float { Float(value) }
        let faceUv = showFace ? map.bufferUv(plan.faceCenter) : CallFacePoint(0.5, 0.5)
        let outfitFace = plan.outfitFace.map(map.bufferUv) ?? CallFacePoint(0.5, 0.5)
        let zone = plan.zone
        let tongue = plan.tongueFrame
        let outfit = wearing ? plan.outfit : nil
        let reach = (outfit?.unit ?? 1) * CallOutfits.reach
        return [
            SIMD4(f(up[0]), f(up[2]), f(up[4]), width),
            SIMD4(f(up[1]), f(up[3]), f(up[5]), height),
            SIMD4(hasMatte ? 1 : 0, showFace ? 1 : 0, f(showFace ? plan.presence : 0), f(plan.hide)),
            SIMD4(follow ? 1 : 0, f(plan.tongue), f(plan.skinRed), f(seed)),
            SIMD4(f(faceUv.x), f(faceUv.y), f(outfitFace.x), f(outfitFace.y)),
            SIMD4(f(zone?.center.x ?? -1e5), f(zone?.center.y ?? -1e5), f(zone?.x.x ?? 0), f(zone?.x.y ?? 0)),
            SIMD4(f(zone?.y.x ?? 0), f(zone?.y.y ?? 0), f(outfit == nil ? 0 : plan.dressed), 0),
            SIMD4(f(tongue?.center.x ?? -1e5), f(tongue?.center.y ?? -1e5), f(tongue?.x.x ?? 0), f(tongue?.x.y ?? 0)),
            SIMD4(f(tongue?.y.x ?? 0), f(tongue?.y.y ?? 0), f(reach) / width, f(reach) / height),
            SIMD4(f(outfit?.center.x ?? -1e5), f(outfit?.center.y ?? -1e5), f(outfit?.x.x ?? 0), f(outfit?.x.y ?? 0)),
            SIMD4(f(outfit?.y.x ?? 0), f(outfit?.y.y ?? 0), 0, 0),
            SIMD4(hair ? 1 : 0, blur ? 1 : 0, Float(map.uprightWidth), Float(map.uprightHeight)),
        ]
    }

    private func drawWarp(_ commands: MTLCommandBuffer, targets: Targets, plan: CallFaceDrawPlan, asset: NativeFaceAsset) {
        let length = plan.vertices.count * MemoryLayout<Float>.stride
        if (vertexBuffer?.length ?? 0) < length {
            vertexBuffer = device.makeBuffer(length: max(length, 64 * 1024), options: .storageModeShared)
        }
        guard let vertexBuffer else {
            failed = true
            return
        }
        plan.vertices.withUnsafeBytes { raw in
            if let base = raw.baseAddress { vertexBuffer.contents().copyMemory(from: base, byteCount: length) }
        }
        let toBuffer = targets.map.toBufferMatrix
        func f(_ value: Double) -> Float { Float(value) }
        func eye(_ side: Int) -> SIMD4<Float> {
            let e = side < asset.eyes.count && asset.eyes[side].count >= 4 ? asset.eyes[side] : [0, 0, 1, 1]
            return SIMD4(f(e[0]), f(e[1]), f(e[2]), f(e[3]))
        }
        let uniforms: [SIMD4<Float>] = [
            SIMD4(f(toBuffer[0]), f(toBuffer[2]), f(toBuffer[4]), Float(targets.map.bufferWidth)),
            SIMD4(f(toBuffer[1]), f(toBuffer[3]), f(toBuffer[5]), Float(targets.map.bufferHeight)),
            eye(0),
            eye(1),
            SIMD4(f(plan.look[0]), f(plan.look[1]), f(plan.look[2]), f(plan.look[3])),
            SIMD4(f(plan.close[0]), f(plan.close[1]), 0, 0),
        ]
        let descriptor = MTLRenderPassDescriptor()
        for (index, texture) in [targets.warp, targets.aux].enumerated() {
            descriptor.colorAttachments[index].texture = texture
            descriptor.colorAttachments[index].loadAction = .clear
            descriptor.colorAttachments[index].clearColor = MTLClearColor(red: 0, green: 0, blue: 0, alpha: 0)
            descriptor.colorAttachments[index].storeAction = .store
        }
        descriptor.depthAttachment.texture = targets.depth
        descriptor.depthAttachment.loadAction = .clear
        descriptor.depthAttachment.clearDepth = 1
        descriptor.depthAttachment.storeAction = .dontCare
        guard let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor) else {
            failed = true
            return
        }
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
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: plan.vertices.count / CallFaceWarp.vertexFloats)
        encoder.endEncoding()
    }

    private func reduceInto(_ commands: MTLCommandBuffer, _ out: MTLTexture, mode: Float, targets: Targets) {
        pass(commands, into: out, reduce, [targets.camera, targets.warp, targets.aux], [SIMD4(0.25 / Float(out.width), 0.25 / Float(out.height), mode, 0)])
    }

    private func blurInto(_ commands: MTLCommandBuffer, _ out: MTLTexture, sigma: Double, spare: MTLTexture) {
        let step = Float(CallFaceDrawPlan.blurStep(sigma))
        pass(commands, into: spare, blur, [out], [SIMD4(step / Float(out.width), 0, 0, 0)])
        pass(commands, into: out, blur, [spare], [SIMD4(0, step / Float(out.height), 0, 0)])
    }

    private func buildFill(_ commands: MTLCommandBuffer, targets: Targets, coef: MTLTexture, clean: MTLTexture) -> MTLTexture {
        let levels = targets.pyramid
        let first = levels[0]
        pass(commands, into: first, weigh, [targets.camera, coef, clean], [SIMD4(0.25 / Float(first.width), 0.25 / Float(first.height), 1, 0)])
        for level in 1..<levels.count {
            let from = levels[level - 1]
            pass(commands, into: levels[level], push, [from], [SIMD4(0.5 / Float(from.width), 0.5 / Float(from.height), 0, 0)])
        }
        for level in stride(from: levels.count - 1, through: 0, by: -1) {
            let top = level == levels.count - 1
            let coarse = top ? levels[level] : targets.pulled[level + 1]
            pass(commands, into: targets.pulled[level], pull, [levels[level], coarse], [SIMD4(0, 0, top ? 1 : 0, 0)])
        }
        return targets.pulled[0]
    }

    private func buildBackdrop(_ commands: MTLCommandBuffer, targets: Targets, strength: Double) -> MTLTexture {
        pass(commands, into: targets.blurA, push, [targets.pyramid[1]], [SIMD4(0, 0, 0, 0)])
        let halo = CallFaceDrawPlan.haloSigma(strength)
        for _ in 0..<halo.passes { blurInto(commands, targets.blurA, sigma: halo.sigma, spare: targets.blurB) }
        pass(commands, into: targets.backdrop, backdrop, [targets.blurA, targets.pulled[1]], [])
        return targets.backdrop
    }

    private func pass(_ commands: MTLCommandBuffer, into target: MTLTexture, _ pipeline: MTLRenderPipelineState, _ textures: [MTLTexture], _ uniforms: [SIMD4<Float>]) {
        let descriptor = MTLRenderPassDescriptor()
        descriptor.colorAttachments[0].texture = target
        descriptor.colorAttachments[0].loadAction = .dontCare
        descriptor.colorAttachments[0].storeAction = .store
        guard let encoder = commands.makeRenderCommandEncoder(descriptor: descriptor) else {
            failed = true
            return
        }
        encoder.setRenderPipelineState(pipeline)
        for (index, texture) in textures.enumerated() { encoder.setFragmentTexture(texture, index: index) }
        bytes(encoder, uniforms.isEmpty ? [SIMD4<Float>(0, 0, 0, 0)] : uniforms)
        encoder.drawPrimitives(type: .triangle, vertexStart: 0, vertexCount: 3)
        encoder.endEncoding()
    }

    private func bytes(_ encoder: MTLRenderCommandEncoder, _ uniforms: [SIMD4<Float>]) {
        uniforms.withUnsafeBytes { raw in
            if let base = raw.baseAddress { encoder.setFragmentBytes(base, length: raw.count, index: 0) }
        }
    }

    private func upload(_ matte: CVPixelBuffer) -> MTLTexture? {
        guard CVPixelBufferGetPixelFormatType(matte) == kCVPixelFormatType_OneComponent8 else { return nil }
        let width = CVPixelBufferGetWidth(matte)
        let height = CVPixelBufferGetHeight(matte)
        if maskTexture?.width != width || maskTexture?.height != height {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r8Unorm, width: width, height: height, mipmapped: false)
            descriptor.usage = .shaderRead
            descriptor.storageMode = .shared
            maskTexture = device.makeTexture(descriptor: descriptor)
        }
        guard let maskTexture else { return nil }
        CVPixelBufferLockBaseAddress(matte, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(matte, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(matte) else { return nil }
        maskTexture.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: base, bytesPerRow: CVPixelBufferGetBytesPerRow(matte))
        return maskTexture
    }

    private func depthTexture(_ buffer: CVPixelBuffer, held: inout [CVMetalTexture]) -> MTLTexture? {
        guard CVPixelBufferGetPixelFormatType(buffer) == kCVPixelFormatType_DepthFloat32 else { return nil }
        if CVPixelBufferGetIOSurface(buffer) != nil, let made = texture(buffer, plane: -1, format: .r32Float) {
            held.append(made.0)
            return made.1
        }
        let width = CVPixelBufferGetWidth(buffer)
        let height = CVPixelBufferGetHeight(buffer)
        if depthCopy?.width != width || depthCopy?.height != height {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: .r32Float, width: width, height: height, mipmapped: false)
            descriptor.usage = .shaderRead
            descriptor.storageMode = .shared
            depthCopy = device.makeTexture(descriptor: descriptor)
        }
        guard let depthCopy else { return nil }
        CVPixelBufferLockBaseAddress(buffer, .readOnly)
        defer { CVPixelBufferUnlockBaseAddress(buffer, .readOnly) }
        guard let base = CVPixelBufferGetBaseAddress(buffer) else { return nil }
        depthCopy.replace(region: MTLRegionMake2D(0, 0, width, height), mipmapLevel: 0, withBytes: base, bytesPerRow: CVPixelBufferGetBytesPerRow(buffer))
        return depthCopy
    }

    private func texture(_ buffer: CVPixelBuffer, plane: Int, format: MTLPixelFormat) -> (CVMetalTexture, MTLTexture)? {
        let planar = plane >= 0 && CVPixelBufferIsPlanar(buffer)
        let index = max(0, plane)
        let width = planar ? CVPixelBufferGetWidthOfPlane(buffer, index) : CVPixelBufferGetWidth(buffer)
        let height = planar ? CVPixelBufferGetHeightOfPlane(buffer, index) : CVPixelBufferGetHeight(buffer)
        var made: CVMetalTexture?
        let status = CVMetalTextureCacheCreateTextureFromImage(nil, cache, buffer, nil, format, width, height, index, &made)
        guard status == kCVReturnSuccess, let made, let texture = CVMetalTextureGetTexture(made) else { return nil }
        return (made, texture)
    }

    private func makeBuffer(width: Int, height: Int, format: OSType, pool path: ReferenceWritableKeyPath<NativeFaceGPU, CVPixelBufferPool?>) -> CVPixelBuffer? {
        if poolWidth != width || poolHeight != height {
            outputPool = nil
            keepPool = nil
            poolWidth = width
            poolHeight = height
        }
        if self[keyPath: path] == nil {
            var made: CVPixelBufferPool?
            let attributes: [String: Any] = [
                kCVPixelBufferPixelFormatTypeKey as String: format,
                kCVPixelBufferWidthKey as String: width,
                kCVPixelBufferHeightKey as String: height,
                kCVPixelBufferMetalCompatibilityKey as String: true,
                kCVPixelBufferIOSurfacePropertiesKey as String: [:] as [String: Any],
            ]
            CVPixelBufferPoolCreate(nil, nil, attributes as CFDictionary, &made)
            self[keyPath: path] = made
        }
        guard let pool = self[keyPath: path] else { return nil }
        var out: CVPixelBuffer?
        CVPixelBufferPoolCreatePixelBuffer(nil, pool, &out)
        return out
    }

    private func targets(for map: CallFaceFrameMap) -> Targets? {
        if let targets, targets.map == map { return targets }
        targets = nil
        let width = map.bufferWidth
        let height = map.bufferHeight
        let depthStorage: MTLStorageMode = device.supportsFamily(.apple2) ? .memoryless : .private
        func make(_ format: MTLPixelFormat, _ w: Int, _ h: Int, storage: MTLStorageMode = .private) -> MTLTexture? {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(pixelFormat: format, width: max(1, w), height: max(1, h), mipmapped: false)
            descriptor.usage = storage == .memoryless ? .renderTarget : [.renderTarget, .shaderRead]
            descriptor.storageMode = storage
            return device.makeTexture(descriptor: descriptor)
        }
        let low = map.reducedSize(CallFaceDrawPlan.lowWidth)
        let fillSize = map.reducedSize(CallFaceDrawPlan.fillWidth)
        let levels = CallFaceDrawPlan.levelSizes(width: fillSize.width, height: fillSize.height)
        let pyramid = levels.compactMap { make(.rgba16Float, $0.0, $0.1) }
        let pulled = levels.compactMap { make(.rgba16Float, $0.0, $0.1) }
        let blurWidth = max(1, (fillSize.width + 1) / 2)
        let blurHeight = max(1, (fillSize.height + 1) / 2)
        guard pyramid.count == levels.count, pulled.count == levels.count,
            let camera = make(.rgba8Unorm, width, height),
            let warp = make(.rgba8Unorm, width, height),
            let aux = make(.rgba8Unorm, width, height),
            let depth = make(.depth32Float, width, height, storage: depthStorage),
            let lowCam = make(.rgba16Float, low.width, low.height),
            let lowFace = make(.rgba16Float, low.width, low.height),
            let plain = make(.rgba16Float, low.width, low.height),
            let wideCam = make(.rgba16Float, low.width, low.height),
            let wideFace = make(.rgba16Float, low.width, low.height),
            let spare = make(.rgba16Float, low.width, low.height),
            let blurA = make(.rgba16Float, blurWidth, blurHeight),
            let blurB = make(.rgba16Float, blurWidth, blurHeight),
            let backdrop = make(.rgba16Float, blurWidth, blurHeight)
        else { return nil }
        let made = Targets(
            map: map, camera: camera, warp: warp, aux: aux, depth: depth, lowCam: lowCam, lowFace: lowFace, plain: plain,
            wideCam: wideCam, wideFace: wideFace, spare: spare, lowScale: low.scale, pyramid: pyramid, pulled: pulled,
            blurA: blurA, blurB: blurB, backdrop: backdrop)
        targets = made
        return made
    }
}
