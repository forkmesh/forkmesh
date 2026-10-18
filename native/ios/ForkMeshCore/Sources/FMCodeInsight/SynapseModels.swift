import Foundation

public enum SynapseDirection: Sendable, Equatable {
    case imports, importers, both
}

public struct SynapseBrainShape: Sendable, Equatable {
    public var halfWidth: Float = 96
    public var halfHeightTop: Float = 76
    public var halfHeightBottom: Float = 58
    public var halfLength: Float = 124
    public var fissure: Float = 15
    public var innerShell: Float = 0.4
    public var outerShell: Float = 0.96

    public init() {}

    public var boundingRadius: Float { halfLength }

    public func radiusOf(_ point: SynapseVector) -> Float {
        let length = point.length
        if length < 1e-6 { return 0 }
        let taper: Float = 1 - 0.1 * (point.z / length)
        let a = halfWidth * taper
        let b = point.y >= 0 ? halfHeightTop : halfHeightBottom
        let x = point.x / a
        let y = point.y / b
        let z = point.z / halfLength
        return (x * x + y * y + z * z).squareRoot()
    }

    public func outerAt(_ point: SynapseVector) -> Float {
        let length = point.length
        if length < 1e-6 { return outerShell }
        let d = point / length
        let theta = SynapseMath.atan2(d.y, d.z)
        let phi = SynapseMath.asin(SynapseMath.clamp(d.x, -1, 1))
        let groove = SynapseMath.sin(10 * theta + 2.6 * SynapseMath.sin(5 * phi + 1.3 * theta))
        return outerShell - 0.055 * (0.5 + 0.5 * groove)
    }

    public func onShell(_ direction: SynapseVector, _ radius: Float) -> SynapseVector {
        let r = radiusOf(direction)
        if r < 1e-6 { return SynapseVector() }
        return direction * (radius / r)
    }

    func shellNormal(_ p: SynapseVector) -> SynapseVector {
        let b = p.y >= 0 ? halfHeightTop : halfHeightBottom
        let n = SynapseVector(p.x / (halfWidth * halfWidth), p.y / (b * b), p.z / (halfLength * halfLength))
        let len = n.length
        return len > 1e-6 ? n / len : SynapseVector(0, 1, 0)
    }
}

public struct SynapseLobe: Sendable, Equatable {
    public var name = ""
    public var path = ""
    public var anchor = SynapseVector()
    public var hemisphere = 1
    public var layer = 0
    public var nodes: [Int] = []

    public init() {}
}

public struct SynapseNode: Sendable, Equatable {
    public var file = -1
    public var lobe = 0
    public var hemisphere = 1
    public var size: Float = 1
    public var fanIn = 0
    public var fanOut = 0
    public var cycle = false
    public var seed: UInt32 = 0

    public init() {}
}

public struct SynapseEdge: Sendable, Equatable {
    public var from = -1
    public var to = -1
    public var cycle = false
    public var seed: UInt32 = 0

    public init(from: Int = -1, to: Int = -1, cycle: Bool = false, seed: UInt32 = 0) {
        self.from = from
        self.to = to
        self.cycle = cycle
        self.seed = seed
    }
}

public struct SynapseCycle: Sendable, Equatable {
    public var index = -1
    public var files: [Int] = []
    public var edges: [Int] = []
    public var path: [Int] = []

    public init() {}
}

public struct SynapseGraph: Sendable, Equatable {
    public var includesIsolated = false
    public var shape = SynapseBrainShape()
    public var nodes: [SynapseNode] = []
    public var edges: [SynapseEdge] = []
    public var nodeOfFile: [Int] = []
    public var lobes: [SynapseLobe] = []
    public var outEdges: [[Int]] = []
    public var inEdges: [[Int]] = []
    public var cycles: [SynapseCycle] = []
    public var imports = 0
    public var spacing: Float = 10

    public init() {}

    public func nodeFor(_ file: Int) -> Int {
        file >= 0 && file < nodeOfFile.count ? nodeOfFile[file] : -1
    }
}

public struct SynapseLayoutOptions: Sendable, Equatable {
    public var iterations = 300
    public var barnesHutAbove = 1500
    public var theta: Float = 0.85
    public var publishInterval: Double = 0.045

    public init(iterations: Int = 300, barnesHutAbove: Int = 1500, theta: Float = 0.85, publishInterval: Double = 0.045) {
        self.iterations = iterations
        self.barnesHutAbove = barnesHutAbove
        self.theta = theta
        self.publishInterval = publishInterval
    }
}

public struct SynapseNeighbourhood: Sendable, Equatable {
    public var imports: [Int: Int] = [:]
    public var importers: [Int: Int] = [:]

    public init(imports: [Int: Int] = [:], importers: [Int: Int] = [:]) {
        self.imports = imports
        self.importers = importers
    }

    public func contains(_ file: Int) -> Bool { imports[file] != nil || importers[file] != nil }

    public var count: Int { imports.count + importers.count }
}
