import Foundation

public struct SynapseCamera: Sendable, Equatable {
    public static let fieldOfView: Float = 38
    public static let bow: Float = 0.26
    public static let starRadius: Float = 1500
    public static let minDistance: Float = 12
    public static let maxDistance: Float = 2400
    public static let maxPitch: Float = 1.45

    public var yaw: Float = 1.05
    public var pitch: Float = 0.32
    public var distance: Float = 420
    public var target = SynapseVector()

    public init(yaw: Float = 1.05, pitch: Float = 0.32, distance: Float = 420, target: SynapseVector = SynapseVector()) {
        self.yaw = yaw
        self.pitch = pitch
        self.distance = distance
        self.target = target
    }

    public var back: SynapseVector {
        let cp = SynapseMath.cos(pitch)
        return SynapseVector(cp * SynapseMath.sin(yaw), SynapseMath.sin(pitch), cp * SynapseMath.cos(yaw))
    }

    public var eye: SynapseVector { target + back * distance }

    public var basis: (right: SynapseVector, up: SynapseVector, forward: SynapseVector) {
        let forward = -back
        let right = SynapseVector.cross(forward, SynapseVector(0, 1, 0)).normalized
        let up = SynapseVector.cross(right, forward)
        return (right, up, forward)
    }

    public func clamped() -> SynapseCamera {
        var camera = self
        camera.pitch = SynapseMath.clamp(pitch, -Self.maxPitch, Self.maxPitch)
        camera.distance = SynapseMath.clamp(distance, Self.minDistance, Self.maxDistance)
        return camera
    }

    public static func tanHalfFov() -> Float { SynapseMath.tan(fieldOfView * 0.5 * Float.pi / 180) }

    public func orbited(dx: Float, dy: Float) -> SynapseCamera {
        var camera = self
        camera.yaw -= dx * 0.008
        camera.pitch = SynapseMath.clamp(pitch + dy * 0.008, -Self.maxPitch, Self.maxPitch)
        return camera
    }

    public func panned(dx: Float, dy: Float, height: Float) -> SynapseCamera {
        var camera = self
        let (right, up, _) = basis
        let scale = 2 * distance * Self.tanHalfFov() / max(1, height)
        camera.target -= right * dx * scale
        camera.target += up * dy * scale
        return camera
    }

    public func zoomed(by factor: Float, towardX x: Float, y: Float, width: Float, height: Float) -> SynapseCamera {
        var camera = self
        let newDistance = SynapseMath.clamp(distance * factor, Self.minDistance, Self.maxDistance)
        let applied = newDistance / distance
        let ndcX = x / max(1, width) * 2 - 1
        let ndcY = 1 - y / max(1, height) * 2
        let (right, up, _) = basis
        let tanV = Self.tanHalfFov()
        let tanH = tanV * max(1, width) / max(1, height)
        let under = target + (right * (ndcX * tanH) + up * (ndcY * tanV)) * distance
        camera.target += (under - target) * (1 - applied)
        camera.distance = newDistance
        return camera
    }

    public static func easeInOut(_ t: Float) -> Float {
        t < 0.5 ? 4 * t * t * t : 1 - SynapseMath.pow(-2 * t + 2, 3) / 2
    }

    public static func interpolate(_ from: SynapseCamera, _ to: SynapseCamera, _ e: Float) -> SynapseCamera {
        var camera = from
        camera.target = from.target + (to.target - from.target) * e
        camera.distance = from.distance + (to.distance - from.distance) * e
        camera.pitch = from.pitch + (to.pitch - from.pitch) * e
        return camera
    }

    public static func fitted(_ camera: SynapseCamera,
                              points input: [SynapseVector],
                              whole: Bool,
                              shape: SynapseBrainShape,
                              spacing: Float?,
                              aspect: Float,
                              autoRotate: Bool) -> SynapseCamera {
        var points = input
        var result = camera
        if whole || points.isEmpty {
            points.removeAll()
            for sx in [Float(-1), 1] {
                for sz in [Float(-1), 1] {
                    points.append(SynapseVector(sx * shape.halfWidth, shape.halfHeightTop, sz * shape.halfLength) * 0.8)
                    points.append(SynapseVector(sx * shape.halfWidth, -shape.halfHeightBottom, sz * shape.halfLength) * 0.8)
                }
            }
            points.append(SynapseVector(0, 0, shape.halfLength))
            points.append(SynapseVector(0, 0, -shape.halfLength))
            points.append(SynapseVector(shape.halfWidth, 0, 0))
            points.append(SynapseVector(-shape.halfWidth, 0, 0))
            points.append(SynapseVector(0, shape.halfHeightTop, 0))
            points.append(SynapseVector(0, -shape.halfHeightBottom, 0))
        }
        var lo = points[0]
        var hi = points[0]
        for p in points {
            lo = SynapseVector(min(lo.x, p.x), min(lo.y, p.y), min(lo.z, p.z))
            hi = SynapseVector(max(hi.x, p.x), max(hi.y, p.y), max(hi.z, p.z))
        }
        result.target = (lo + hi) * 0.5
        let back = camera.back
        let (right, up, _) = camera.basis
        let tanV = tanHalfFov() * 0.86
        let tanH = tanV * max(0.4, aspect)
        var distance: Float = 0
        for p in points {
            let q = p - result.target
            let x = autoRotate ? (q.x * q.x + q.z * q.z).squareRoot() : abs(SynapseVector.dot(q, right))
            let y = abs(SynapseVector.dot(q, up))
            let z = SynapseVector.dot(q, back)
            distance = max(distance, z + x / tanH, z + y / tanV)
        }
        let minimum = spacing.map { $0 * 4 } ?? 40
        result.distance = SynapseMath.clamp(distance, max(30, minimum), maxDistance)
        return result
    }
}

public struct SynapseProjection: Sendable {
    public let camera: SynapseCamera
    public let width: Float
    public let height: Float
    public let focal: Float
    public let eye: SynapseVector
    public let right: SynapseVector
    public let up: SynapseVector
    public let forward: SynapseVector
    public let nearPlane: Float
    public let fogNear: Float
    public let fogFar: Float

    public init(camera: SynapseCamera, width: Float, height: Float, boundingRadius: Float = 124) {
        self.camera = camera
        self.width = max(1, width)
        self.height = max(1, height)
        focal = self.height * 0.5 / SynapseCamera.tanHalfFov()
        eye = camera.eye
        let basis = camera.basis
        right = basis.right
        up = basis.up
        forward = basis.forward
        nearPlane = max(1, camera.distance * 0.05)
        let centreDepth = SynapseVector.dot(SynapseVector() - eye, forward)
        fogNear = centreDepth - boundingRadius * 0.55
        fogFar = centreDepth + boundingRadius * 1.1
    }

    @inline(__always) public func depth(_ p: SynapseVector) -> Float {
        SynapseVector.dot(p - eye, forward)
    }

    @inline(__always) public func project(_ p: SynapseVector) -> (x: Float, y: Float, depth: Float) {
        let v = p - eye
        let depth = SynapseVector.dot(v, forward)
        let scale = focal / max(depth, 1e-4)
        return (width * 0.5 + SynapseVector.dot(v, right) * scale, height * 0.5 - SynapseVector.dot(v, up) * scale, depth)
    }

    @inline(__always) public func fog(_ depth: Float, strength: Float = 0.72) -> Float {
        1 - strength * SynapseMath.clamp((depth - fogNear) / max(1, fogFar - fogNear), 0, 1)
    }
}

public final class SynapseFrame {
    public private(set) var count = 0
    public var x: [Float] = []
    public var y: [Float] = []
    public var depth: [Float] = []
    public var radius: [Float] = []
    public var fog: [Float] = []
    public var onScreen: [Bool] = []
    public var visible: [Bool] = []
    public private(set) var order: [Int] = []
    public private(set) var projection: SynapseProjection?

    public init() {}

    func resize(_ n: Int) {
        if n == count { return }
        count = n
        x = [Float](repeating: 0, count: n)
        y = [Float](repeating: 0, count: n)
        depth = [Float](repeating: 0, count: n)
        radius = [Float](repeating: 0, count: n)
        fog = [Float](repeating: 1, count: n)
        onScreen = [Bool](repeating: false, count: n)
        visible = [Bool](repeating: false, count: n)
        order = Array(0..<n)
    }

    public func update(graph: SynapseGraph, positions: [SynapseVector], style: SynapseStyle, projection: SynapseProjection) {
        self.projection = projection
        let n = min(graph.nodes.count, positions.count)
        resize(n)
        let w = projection.width
        let h = projection.height
        let base = min(graph.spacing * 0.15, 3.4)
        for i in 0..<n {
            let state = i < style.nodeState.count ? style.nodeState[i] : .hidden
            let p = projection.project(positions[i])
            depth[i] = p.depth
            if state == .hidden || p.depth < projection.nearPlane {
                onScreen[i] = false
                visible[i] = false
                continue
            }
            visible[i] = true
            x[i] = p.x
            y[i] = p.y
            var r = base * graph.nodes[i].size
            switch state {
            case .selected: r *= 1.55
            case .imports, .importers, .match: r *= 1.15
            case .dim: r *= 0.85
            default: break
            }
            let rad = r * projection.focal / p.depth
            radius[i] = rad
            fog[i] = projection.fog(p.depth)
            let margin = rad * 3 + 2
            onScreen[i] = p.x > -margin && p.x < w + margin && p.y > -margin && p.y < h + margin
        }
        sortBackToFront()
    }

    func sortBackToFront() {
        let n = order.count
        if n < 2 { return }
        var shifts = 0
        let budget = n * 12
        var i = 1
        depth.withUnsafeBufferPointer { d in
            order.withUnsafeMutableBufferPointer { o in
                while i < n {
                    let item = o[i]
                    let key = d[item]
                    var j = i - 1
                    while j >= 0 && (d[o[j]] < key || (d[o[j]] == key && o[j] > item)) {
                        o[j + 1] = o[j]
                        j -= 1
                        shifts += 1
                    }
                    o[j + 1] = item
                    if shifts > budget { break }
                    i += 1
                }
            }
        }
        if i < n {
            let d = depth
            order.sort { a, b in d[a] != d[b] ? d[a] > d[b] : a < b }
        }
    }

    public func pick(x px: Float, y py: Float, nearRadius: Float = 9, slop: Float = 1.5) -> Int {
        var best = -1
        var bestDepth: Float = 0
        var near = -1
        var nearDistance = nearRadius
        for i in 0..<count where onScreen[i] && visible[i] {
            let dx = x[i] - px
            let dy = y[i] - py
            let d = (dx * dx + dy * dy).squareRoot()
            let reach = max(radius[i], 3) + slop
            if d <= reach && (best < 0 || depth[i] < bestDepth) {
                best = i
                bestDepth = depth[i]
            } else if best < 0 && d < nearDistance {
                near = i
                nearDistance = d
            }
        }
        return best >= 0 ? best : near
    }
}

extension Synapse {
    public static func stars() -> [(direction: SynapseVector, brightness: Float)] {
        (0..<420).map { i in
            let a = stableHash("star-a-\(i)")
            let b = stableHash("star-b-\(i)")
            let c = stableHash("star-c-\(i)")
            let z = Float(a & 0xffff) / 32767.5 - 1
            let theta = Float(b & 0xffff) / 65535 * 6.2831853
            let r = max(0, 1 - z * z).squareRoot()
            let bright = SynapseMath.pow(Float(c & 0xffff) / 65535, 3)
            return (SynapseVector(r * SynapseMath.cos(theta), z, r * SynapseMath.sin(theta)), bright)
        }
    }

    public static func dust(_ shape: SynapseBrainShape) -> [SynapseVector] {
        var points: [SynapseVector] = []
        let count = 1800
        points.reserveCapacity(count)
        for i in 0..<count {
            let y: Float = 1 - 2 * (Float(i) + 0.5) / Float(count)
            let r = max(0, 1 - y * y).squareRoot()
            let phi = Float(i) * SynapseMath.goldenAngle
            let direction = SynapseVector(r * SynapseMath.sin(phi), y, r * SynapseMath.cos(phi))
            let p = shape.onShell(direction, shape.outerAt(direction) + 0.025)
            if abs(p.x) < shape.fissure * 0.6 { continue }
            points.append(p)
        }
        return points
    }

    public static func curvePoint(_ a: SynapseVector, _ b: SynapseVector, _ t: Float) -> SynapseVector {
        let mid = (a + b) * 0.5
        let c = mid - mid * SynapseCamera.bow
        let u = 1 - t
        return a * (u * u) + c * (2 * u * t) + b * (t * t)
    }

    public static func flightToFile(_ camera: SynapseCamera, graph: SynapseGraph, positions: [SynapseVector], file: Int,
                                    aspect: Float, autoRotate: Bool) -> SynapseCamera? {
        let node = graph.nodeFor(file)
        if node < 0 || node >= positions.count { return nil }
        var nodes = [node]
        for e in graph.outEdges[node] { nodes.append(graph.edges[e].to) }
        for e in graph.inEdges[node] { nodes.append(graph.edges[e].from) }
        var fitted = SynapseCamera.fitted(camera, points: nodes.map { positions[$0] }, whole: false, shape: graph.shape,
                                          spacing: graph.spacing, aspect: aspect, autoRotate: autoRotate)
        fitted.target = positions[node] * 0.7 + fitted.target * 0.3
        fitted.distance = SynapseMath.clamp(fitted.distance * 1.1, graph.spacing * 6, max(graph.spacing * 6, camera.distance))
        fitted.yaw = camera.yaw
        return fitted
    }

    public static func flightToCycle(_ camera: SynapseCamera, graph: SynapseGraph, positions: [SynapseVector], cycle: Int,
                                     aspect: Float, autoRotate: Bool) -> SynapseCamera? {
        if cycle < 0 || cycle >= graph.cycles.count { return nil }
        let points = graph.cycles[cycle].files.map { graph.nodeFor($0) }.filter { $0 >= 0 && $0 < positions.count }.map { positions[$0] }
        var fitted = SynapseCamera.fitted(camera, points: points, whole: false, shape: graph.shape, spacing: graph.spacing,
                                          aspect: aspect, autoRotate: autoRotate)
        fitted.distance = max(fitted.distance * 1.15, graph.spacing * 6)
        fitted.yaw = camera.yaw
        return fitted
    }

    public static func fit(_ camera: SynapseCamera, graph: SynapseGraph, positions: [SynapseVector], style: SynapseStyle,
                           settled: Bool, aspect: Float, autoRotate: Bool) -> SynapseCamera {
        var points: [SynapseVector] = []
        let isolated = style.input.isolatedCycle
        if isolated >= 0 && isolated < graph.cycles.count {
            for file in graph.cycles[isolated].files {
                let node = graph.nodeFor(file)
                if node >= 0 && node < positions.count { points.append(positions[node]) }
            }
            var fitted = SynapseCamera.fitted(camera, points: points, whole: false, shape: graph.shape, spacing: graph.spacing,
                                              aspect: aspect, autoRotate: autoRotate)
            fitted.yaw = camera.yaw
            return fitted
        }
        if positions.count == graph.nodes.count {
            for i in graph.nodes.indices where i < style.nodeState.count && style.nodeState[i] != .hidden {
                points.append(positions[i])
            }
        }
        let whole = points.count == graph.nodes.count || !settled
        var fitted = SynapseCamera.fitted(camera, points: points, whole: whole, shape: graph.shape,
                                          spacing: graph.nodes.isEmpty ? nil : graph.spacing, aspect: aspect, autoRotate: autoRotate)
        fitted.yaw = camera.yaw
        return fitted
    }
}
