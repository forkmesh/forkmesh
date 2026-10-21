import Foundation

public struct CallFaceFrameMap: Equatable, Sendable {
    public let bufferWidth: Int
    public let bufferHeight: Int
    public let degrees: Int

    public init(bufferWidth: Int, bufferHeight: Int, degrees: Int) {
        self.bufferWidth = bufferWidth
        self.bufferHeight = bufferHeight
        let turn = ((degrees % 360) + 360) % 360
        self.degrees = turn - turn % 90
    }

    public var sideways: Bool { degrees == 90 || degrees == 270 }
    public var uprightWidth: Int { sideways ? bufferHeight : bufferWidth }
    public var uprightHeight: Int { sideways ? bufferWidth : bufferHeight }

    public func toUpright(_ p: CallFacePoint) -> CallFacePoint {
        let w = Double(bufferWidth)
        let h = Double(bufferHeight)
        switch degrees {
        case 90: return CallFacePoint(h - p.y, p.x)
        case 180: return CallFacePoint(w - p.x, h - p.y)
        case 270: return CallFacePoint(p.y, w - p.x)
        default: return CallFacePoint(p.x, p.y)
        }
    }

    public func toBuffer(_ p: CallFacePoint) -> CallFacePoint {
        let w = Double(bufferWidth)
        let h = Double(bufferHeight)
        switch degrees {
        case 90: return CallFacePoint(p.y, h - p.x)
        case 180: return CallFacePoint(w - p.x, h - p.y)
        case 270: return CallFacePoint(w - p.y, p.x)
        default: return CallFacePoint(p.x, p.y)
        }
    }

    public var toBufferMatrix: [Double] { Self.matrix(toBuffer) }
    public var toUprightMatrix: [Double] { Self.matrix(toUpright) }

    public func bufferUv(_ upright: CallFacePoint) -> CallFacePoint {
        let b = toBuffer(upright)
        return CallFacePoint(b.x / Double(max(1, bufferWidth)), b.y / Double(max(1, bufferHeight)))
    }

    public func reducedSize(_ limit: Int) -> (width: Int, height: Int, scale: Double) {
        let w = max(1, uprightWidth)
        let h = max(1, uprightHeight)
        let lowWidth = min(limit, w)
        let lowHeight = max(1, Int(CallFaceJS.round(Double(lowWidth * h) / Double(w))))
        let scale = Double(lowWidth) / Double(w)
        return sideways ? (lowHeight, lowWidth, scale) : (lowWidth, lowHeight, scale)
    }

    static func matrix(_ map: (CallFacePoint) -> CallFacePoint) -> [Double] {
        let o = map(CallFacePoint(0, 0))
        let x = map(CallFacePoint(1, 0))
        let y = map(CallFacePoint(0, 1))
        return [x.x - o.x, x.y - o.y, y.x - o.x, y.y - o.y, o.x, o.y]
    }
}

public enum CallFaceStageInput {
    case found(points: [CallFacePoint], pupils: [CallFacePoint]?, probe: CallFaceProbeSample?, time: Double)
    case missing
    case stale
}

public struct CallFaceProbeSample {
    public let crop: CallFaceProbeCrop
    public let image: CallFaceImage

    public init(crop: CallFaceProbeCrop, image: CallFaceImage) {
        self.crop = crop
        self.image = image
    }
}

public struct CallFaceStageFrame: Equatable, Sendable {
    public let drawn: [CallFacePoint]?
    public let live: [CallFacePoint]?
    public let presence: Double
    public let hide: Double
    public let dressed: Double
    public let expression: CallFaceExpressionReading?
    public let outfit: CallOutfitFrame?
}

public final class CallFaceStage {
    public var hasFace = false
    public var faceReady = false
    public var hasOutfit = false
    public var blend: CallFaceBlendShapes?
    public private(set) var presence = 0.0
    public private(set) var hide = 1.0
    public private(set) var dressed = 0.0
    public private(set) var live: [CallFacePoint]?
    public private(set) var expression: CallFaceExpressionReading?
    private let smoother = CallFacePointSmoother(count: CallFaceGeometry.landmarkCount, minCutoff: 1.5, beta: 1.5)
    private let probe = CallFaceExpressionProbe()
    private let outfitSmoother = CallOutfitSmoother()
    private var seenAt = -Double.infinity
    private var found = false

    public init() {}

    public func faceChanged() {
        presence = 0
    }

    public func outfitChanged() {
        outfitSmoother.reset()
    }

    public func sizeChanged() {
        outfitSmoother.reset()
    }

    public func reset() {
        live = nil
        expression = nil
        seenAt = -Double.infinity
        found = false
        presence = 0
        hide = 1
        dressed = 0
        smoother.reset()
        probe.reset()
        outfitSmoother.reset()
    }

    public static func probeImage(crop: CallFaceProbeCrop, sample: (Double, Double) -> (Double, Double, Double)?) -> CallFaceImage {
        var rgba = [UInt8](repeating: 0, count: crop.width * crop.height * 4)
        for y in 0..<crop.height {
            for x in 0..<crop.width {
                let at = (y * crop.width + x) * 4
                rgba[at + 3] = 255
                let fx = crop.minX + (Double(x) + 0.5) / crop.scale
                let fy = crop.minY + (Double(y) + 0.5) / crop.scale
                guard let color = sample(fx, fy) else { continue }
                rgba[at] = CallOutfitDrawing.clampByte(color.0)
                rgba[at + 1] = CallOutfitDrawing.clampByte(color.1)
                rgba[at + 2] = CallOutfitDrawing.clampByte(color.2)
            }
        }
        return CallFaceImage(width: crop.width, height: crop.height, rgba: rgba)
    }

    public func step(_ input: CallFaceStageInput, time: Double) -> CallFaceStageFrame {
        var points: [CallFacePoint]?
        var fresh = false
        switch input {
        case let .found(raw, pupils, sample, seen):
            fresh = true
            let smoothed = smoother.smooth(raw, time: seen, scale: CallFaceGeometry.faceFrame(raw).span)
            points = smoothed
            live = smoothed
            seenAt = time
            if hasFace, let sample {
                expression = probe.measure(image: sample.image, crop: sample.crop, points: smoothed, time: seen, pupils: pupils, lids: true) ?? expression
            }
            if hasFace, let blend {
                expression = CallFaceBlendMapping.reading(blend, probe: expression)
            }
        case .missing:
            fresh = true
        case .stale:
            break
        }
        let held = live != nil && time - seenAt < CallFaces.holdMs
        if !held {
            live = nil
            expression = nil
            smoother.reset()
            probe.reset()
            outfitSmoother.reset()
        }
        if fresh { found = points != nil }
        let drawn = live.map { current in expression.map { CallFaceExpression.closeLids(current, $0.close) } ?? current }
        let seen = (fresh ? points != nil : found && held) && faceReady
        presence += ((held && faceReady ? 1 : 0) - presence) * (held ? CallFaces.fadeIn : CallFaces.fadeOut)
        let wearing = hasOutfit && held
        dressed += ((wearing ? 1 : 0) - dressed) * (wearing ? CallFaces.fadeIn : CallFaces.fadeOut)
        let body = live != nil && hasOutfit ? outfitSmoother.smooth(CallOutfits.frame(live)) : nil
        hide += ((seen ? 0 : 1) - hide) * (seen ? CallFaces.fadeIn : CallFaces.fadeOut)
        return CallFaceStageFrame(
            drawn: hasFace ? drawn : nil,
            live: live,
            presence: presence,
            hide: hasFace ? hide : 0,
            dressed: dressed,
            expression: expression,
            outfit: body
        )
    }
}

public struct CallFaceDrawPlan: Sendable {
    public static let toneSigma = 0.22
    public static let wideSigma = 0.35
    public static let widePasses = 3
    public static let hideSigma = 4.0
    public static let hidePasses = 4
    public static let haloStrength = 0.25
    public static let lowWidth = 160
    public static let fillWidth = 192
    public static let fillLevels = 5
    public static let defaultSkinRed = 0.12
    public static let gauss: [Float] = [0.2270, 0.1945, 0.1216, 0.0540, 0.0162]

    public let showFace: Bool
    public let wearing: Bool
    public let presence: Double
    public let hide: Double
    public let dressed: Double
    public let vertices: [Float]
    public let zone: CallFaceAxisFrame?
    public let faceCenter: CallFacePoint
    public let span: Double
    public let look: [Double]
    public let close: [Double]
    public let tongue: Double
    public let tongueFrame: CallFaceAxisFrame?
    public let skinRed: Double
    public let outfit: CallOutfitFrame?
    public let outfitFace: CallFacePoint?
    public let hairRegion: CallFaceHairRegion?
    public let seed: [Double]

    public var drawsAnything: Bool { showFace || wearing || hide > 0.001 }

    public static func make(head: CallFaceHead?, frame: CallFaceStageFrame, hasOutfit: Bool, photoSize: Double = CallFaces.photoSize,
                            width: Double = 0, height: Double = 0) -> CallFaceDrawPlan {
        let drawn = frame.drawn
        let showFace = head != nil && drawn != nil && frame.presence > 0.001
        let wearing = hasOutfit && frame.outfit != nil && frame.live != nil && frame.dressed > 0.001
        var vertices: [Float] = []
        var zone: CallFaceAxisFrame?
        var center = CallFacePoint(0, 0)
        var span = 1.0
        var hairRegion: CallFaceHairRegion?
        if showFace, let head, let drawn {
            let live = CallFaceGeometry.liveHead(head.model, drawn)
            if width > 0, height > 0 { hairRegion = CallFaceWarp.hairRegion(live.pose, width: width, height: height) }
            zone = CallFaceWarp.zoneFrame(drawn, live.pose)
            let face = CallFaceGeometry.faceFrame(drawn)
            center = face.mid
            span = face.span
            vertices = CallFaceWarp.warpVertices(mesh: head.mesh, layout: head.layout, live: live.points, photoSize: photoSize, depths: CallFaceGeometry.vertexDepths(head.model, live.pose))
        }
        let expression = frame.expression
        let closed = expression?.close ?? []
        let tongue = showFace && expression != nil ? CallFaceGeometry.clampValue(expression?.tongue ?? 0, 0, 1) : 0
        let tongueFrame = tongue > 0 ? drawn.map { CallFaceWarp.axisFrame(CallFaceExpression.tongueWindow($0)) } : nil
        var outfitFace: CallFacePoint?
        if wearing, let live = frame.live {
            let face = CallFaceGeometry.faceFrame(live)
            outfitFace = CallFacePoint(face.mid.x, face.mid.y + 0.6 * face.span)
        }
        return CallFaceDrawPlan(
            showFace: showFace,
            wearing: wearing,
            presence: showFace ? frame.presence : 0,
            hide: frame.hide,
            dressed: wearing ? CallFaceJS.min(1, frame.dressed) : 0,
            vertices: vertices,
            zone: zone,
            faceCenter: center,
            span: span,
            look: CallFaceWarp.lookUniform(expression?.look.map { Optional($0) }),
            close: [closed.count > 0 && closed[0].isFinite ? closed[0] : 0, closed.count > 1 && closed[1].isFinite ? closed[1] : 0],
            tongue: tongue,
            tongueFrame: tongueFrame,
            skinRed: expression?.skinRed ?? defaultSkinRed,
            outfit: wearing ? frame.outfit : nil,
            outfitFace: outfitFace,
            hairRegion: hairRegion,
            seed: CallFaceMatte.faceSeed(drawn ?? (hasOutfit ? frame.live : nil), width: width, height: height)
        )
    }

    public static func blurStep(_ sigma: Double) -> Double {
        CallFaceJS.max(0.5, sigma / 2)
    }

    public static func toneSigma(span: Double, scale: Double) -> Double {
        toneSigma * span * scale
    }

    public static func wideSigma(span: Double, scale: Double) -> Double {
        wideSigma * span * scale
    }

    public static func levelSizes(width: Int, height: Int, levels: Int = fillLevels) -> [(Int, Int)] {
        var out: [(Int, Int)] = []
        var w = width
        var h = height
        for _ in 0...levels {
            out.append((w, h))
            w = max(1, (w + 1) / 2)
            h = max(1, (h + 1) / 2)
        }
        return out
    }

    public static func haloSigma(_ strength: Double) -> (sigma: Double, passes: Int) {
        (1.5 + 5 * CallFaceJS.max(0, CallFaceJS.min(1, strength)), strength > 0.6 ? 3 : 2)
    }
}
