import Foundation

extension Synapse {
    struct Grouping {
        var groupOfFile: [Int] = []
        var paths: [String] = []
        var names: [String] = []
        var layers: [Int] = []
    }

    static func directoryKey(_ path: String, _ depth: Int) -> String {
        let parts = path.split(separator: "/", omittingEmptySubsequences: true)
        if parts.count <= 1 { return "" }
        let take = min(depth, parts.count - 1)
        return parts[0..<take].joined(separator: "/")
    }

    static func fallbackGrouping(_ snapshot: InsightSnapshot, _ files: [Int]) -> Grouping {
        let wanted = min(max(files.count / 40, 3), 14)
        var chosen = 1
        for depth in 1...4 {
            var keys = Set<String>()
            for file in files { keys.insert(directoryKey(snapshot.files[file].path, depth)) }
            chosen = depth
            if keys.count >= wanted { break }
        }
        var grouping = Grouping()
        grouping.groupOfFile = [Int](repeating: -1, count: snapshot.files.count)
        var index: [String: Int] = [:]
        for file in files.sorted(by: byPath(snapshot)) {
            let key = directoryKey(snapshot.files[file].path, chosen)
            let group: Int
            if let existing = index[key] {
                group = existing
            } else {
                group = grouping.paths.count
                index[key] = group
                grouping.paths.append(key)
                grouping.names.append(key.isEmpty ? "(root)" : key)
                grouping.layers.append(0)
            }
            grouping.groupOfFile[file] = group
        }
        return grouping
    }

    static func moduleGrouping(_ snapshot: InsightSnapshot, _ files: [Int]) -> Grouping {
        let modules = Insight.buildModuleGraph(snapshot, depth: 0)
        if modules.moduleOfFile.count != snapshot.files.count || modules.nodes.isEmpty {
            return fallbackGrouping(snapshot, files)
        }
        var grouping = Grouping()
        grouping.groupOfFile = [Int](repeating: -1, count: snapshot.files.count)
        var remap: [Int: Int] = [:]
        for file in files {
            let module = modules.moduleOfFile[file]
            if module < 0 || module >= modules.nodes.count { return fallbackGrouping(snapshot, files) }
            let group: Int
            if let existing = remap[module] {
                group = existing
            } else {
                group = grouping.paths.count
                remap[module] = group
                let node = modules.nodes[module]
                grouping.paths.append(node.path)
                let name = node.path.isEmpty ? node.name : node.path
                grouping.names.append(name.isEmpty ? "(root)" : name)
                grouping.layers.append(node.layer)
            }
            grouping.groupOfFile[file] = group
        }
        return grouping
    }

    static func placeLobes(_ graph: inout SynapseGraph) {
        let count = graph.lobes.count
        if count == 0 { return }
        let shape = graph.shape
        if count == 1 {
            graph.lobes[0].anchor = SynapseVector()
            graph.lobes[0].hemisphere = 0
            return
        }
        let lobes = graph.lobes
        let order = Array(0..<count).sorted { a, b in
            let la = lobes[a]
            let lb = lobes[b]
            if la.layer != lb.layer { return la.layer < lb.layer }
            if la.nodes.count != lb.nodes.count { return la.nodes.count > lb.nodes.count }
            return U.less(la.path, lb.path)
        }
        var points = [SynapseVector](repeating: SynapseVector(), count: count)
        for i in 0..<count {
            let y: Float = 1 - 2 * (Float(i) + 0.5) / Float(count)
            let r = max(0, 1 - y * y).squareRoot()
            let phi = Float(i) * SynapseMath.goldenAngle
            points[i] = SynapseVector(r * SynapseMath.sin(phi), y * 0.8, r * SynapseMath.cos(phi))
        }
        points.sort { a, b in
            if a.z != b.z { return a.z < b.z }
            return a.x < b.x
        }
        var anchors = [SynapseVector](repeating: SynapseVector(), count: count)
        for i in 0..<count { anchors[order[i]] = points[i] }

        var weights: [UInt64: Int] = [:]
        for edge in graph.edges {
            let a = graph.nodes[edge.from].lobe
            let b = graph.nodes[edge.to].lobe
            if a == b { continue }
            let key = UInt64(min(a, b)) << 32 | UInt64(max(a, b))
            weights[key, default: 0] += 1
        }
        let springs: [(a: Int, b: Int, weight: Float)] = weights.map { key, value in
            (Int(key >> 32), Int(key & 0xffffffff), SynapseMath.log1p(Float(value)))
        }.sorted { x, y in
            if x.a != y.a { return x.a < y.a }
            if x.b != y.b { return x.b < y.b }
            return x.weight < y.weight
        }
        var mass = [Float](repeating: 0, count: count)
        var totalMass: Float = 0
        for i in 0..<count {
            mass[i] = (Float(graph.lobes[i].nodes.count) + 1).squareRoot()
            totalMass += mass[i]
        }
        let ideal: Float = 2 * (4 * 3.14159 / Float(count)).squareRoot()
        let average = totalMass / Float(count)
        var force = [SynapseVector](repeating: SynapseVector(), count: count)
        for iteration in 0..<160 {
            let temp: Float = 0.12 * (1 - Float(iteration) / 160) + 0.004
            for i in 0..<count { force[i] = SynapseVector() }
            for i in 0..<count {
                for j in (i + 1)..<max(i + 1, count) {
                    let d = anchors[i] - anchors[j]
                    let dist2 = max(d.lengthSquared, 1e-4)
                    let strength = ideal * ideal * mass[i] * mass[j] / average / average
                    let f = d * (strength / dist2) * 0.05
                    force[i] += f
                    force[j] -= f
                }
            }
            for spring in springs {
                let d = anchors[spring.b] - anchors[spring.a]
                let f = d * (0.04 * spring.weight)
                force[spring.a] += f
                force[spring.b] -= f
            }
            for i in 0..<count {
                var f = force[i]
                let len = f.length
                if len > temp { f *= temp / len }
                let p = anchors[i] + f
                let plen = p.length
                anchors[i] = plen > 1e-5 ? p / plen : SynapseVector(0, 1, 0)
            }
        }
        let middle: Float = 0.5 * (shape.innerShell + shape.outerShell)
        for i in 0..<count {
            var direction = anchors[i]
            if abs(direction.x) < 0.25 {
                let sign: Float = direction.x < 0 ? -1 : (direction.x > 0 ? 1 : (i % 2 == 0 ? 1 : -1))
                direction.x = 0.25 * sign
                direction.normalize()
            }
            if direction.y < -0.6 {
                direction.y = -0.6
                direction.normalize()
            }
            graph.lobes[i].anchor = shape.onShell(direction, middle)
            graph.lobes[i].hemisphere = graph.lobes[i].anchor.x < 0 ? -1 : 1
        }
    }

    public static func buildGraph(_ snapshot: InsightSnapshot, includeIsolated: Bool) -> SynapseGraph {
        var graph = SynapseGraph()
        graph.includesIsolated = includeIsolated
        graph.nodeOfFile = [Int](repeating: -1, count: snapshot.files.count)
        var files: [Int] = []
        for (i, file) in snapshot.files.enumerated() where includeIsolated || importsOther(file, i) || importedByOther(file, i) {
            files.append(i)
        }
        let grouping = moduleGrouping(snapshot, files)
        for g in grouping.paths.indices {
            var lobe = SynapseLobe()
            lobe.path = grouping.paths[g]
            lobe.name = grouping.names[g]
            lobe.layer = grouping.layers[g]
            graph.lobes.append(lobe)
        }
        files.sort(by: byPath(snapshot))
        for file in files {
            var node = SynapseNode()
            node.file = file
            node.lobe = max(0, grouping.groupOfFile[file])
            node.seed = stableHash(snapshot.files[file].path)
            node.cycle = snapshot.files[file].cycle >= 0
            graph.nodeOfFile[file] = graph.nodes.count
            graph.lobes[node.lobe].nodes.append(graph.nodes.count)
            graph.nodes.append(node)
        }
        graph.outEdges = [[Int]](repeating: [], count: graph.nodes.count)
        graph.inEdges = [[Int]](repeating: [], count: graph.nodes.count)
        for n in graph.nodes.indices {
            let info = snapshot.files[graph.nodes[n].file]
            var previous = Int.min
            for target in info.dependsOn.sorted() {
                if target == previous { continue }
                previous = target
                let to = graph.nodeFor(target)
                if to < 0 || to == n { continue }
                let edge = SynapseEdge(
                    from: n, to: to,
                    cycle: info.cycle >= 0 && info.cycle == snapshot.files[target].cycle,
                    seed: SynapseMath.mixHash(graph.nodes[n].seed ^ (graph.nodes[to].seed &* 31)))
                graph.outEdges[n].append(graph.edges.count)
                graph.inEdges[to].append(graph.edges.count)
                graph.edges.append(edge)
            }
        }
        graph.imports = graph.edges.count
        for n in graph.nodes.indices {
            graph.nodes[n].fanIn = graph.inEdges[n].count
            graph.nodes[n].fanOut = graph.outEdges[n].count
            let info = snapshot.files[graph.nodes[n].file]
            let loc = Float(max(info.codeLines, info.lines / 2))
            let raw: Float = 0.55 + 0.1 * SynapseMath.log1p(loc) + 0.2 * Float(graph.nodes[n].fanIn).squareRoot()
            graph.nodes[n].size = SynapseMath.clamp(raw, 0.5, 3.2)
        }
        graph.cycles = cycleInfos(snapshot)
        var cycleOfInfo: [Int: Int] = [:]
        for c in graph.cycles.indices { cycleOfInfo[graph.cycles[c].index] = c }
        for e in graph.edges.indices where graph.edges[e].cycle {
            let fileCycle = snapshot.files[graph.nodes[graph.edges[e].from].file].cycle
            if let c = cycleOfInfo[fileCycle] { graph.cycles[c].edges.append(e) }
        }
        let shape = graph.shape
        let volume: Float = 4.18879 * shape.halfWidth * shape.halfLength
            * 0.5 * (shape.halfHeightTop + shape.halfHeightBottom)
            * (1 - SynapseMath.pow(shape.innerShell, 3))
        graph.spacing = SynapseMath.clamp(SynapseMath.cbrt(volume / Float(max(1, graph.nodes.count))), 2, 40) * 0.9
        placeLobes(&graph)
        for n in graph.nodes.indices {
            let lobe = graph.lobes[graph.nodes[n].lobe]
            let seed = graph.nodes[n].seed
            graph.nodes[n].hemisphere = lobe.hemisphere != 0 ? lobe.hemisphere : ((seed >> 7) & 1 != 0 ? 1 : -1)
        }
        return graph
    }

    public static func seedPositions(_ graph: SynapseGraph) -> [SynapseVector] {
        let shape = graph.shape
        var positions = [SynapseVector](repeating: SynapseVector(), count: graph.nodes.count)
        for n in graph.nodes.indices {
            let node = graph.nodes[n]
            let lobe = graph.lobes[node.lobe]
            let hemisphere = Float(node.hemisphere)
            let spread = graph.spacing * SynapseMath.cbrt(Float(lobe.nodes.count) + 1) * 0.7
            var p: SynapseVector
            if graph.lobes.count == 1 {
                var direction = SynapseMath.jitter(node.seed)
                if direction.lengthSquared < 1e-4 { direction = SynapseVector(0, 1, 0) }
                direction.x = abs(direction.x) * hemisphere + 0.15 * hemisphere
                p = shape.onShell(direction.normalized, 0.55 + 0.35 * SynapseMath.unitFromHash(SynapseMath.mixHash(node.seed)))
            } else {
                p = lobe.anchor + SynapseMath.jitter(node.seed) * spread
            }
            if p.x * hemisphere < shape.fissure {
                p.x = hemisphere * (shape.fissure + SynapseMath.unitFromHash(node.seed) * 2)
            }
            let r = shape.radiusOf(p)
            let outer = shape.outerAt(p)
            if r > outer { p *= outer / r }
            positions[n] = p
        }
        return positions
    }

    public static func layoutIterations(_ graph: SynapseGraph, _ options: SynapseLayoutOptions = SynapseLayoutOptions()) -> Int {
        let n = graph.nodes.count
        if n <= 5000 { return options.iterations }
        return max(options.iterations * 2 / 5, options.iterations * 5000 / n)
    }

    public static func lobeCentres(_ graph: SynapseGraph, _ positions: [SynapseVector]) -> [SynapseVector] {
        graph.lobes.map { lobe in
            if lobe.nodes.isEmpty || positions.count != graph.nodes.count { return lobe.anchor }
            var sum = SynapseVector()
            for n in lobe.nodes { sum += positions[n] }
            return sum / Float(lobe.nodes.count)
        }
    }

    public static func runLayout(_ graph: SynapseGraph,
                                 options: SynapseLayoutOptions = SynapseLayoutOptions(),
                                 publish: (([SynapseVector], Double) -> Void)? = nil,
                                 stop: (() -> Bool)? = nil) -> [SynapseVector] {
        var positions = seedPositions(graph)
        let n = positions.count
        if n == 0 {
            publish?(positions, 1)
            return positions
        }
        let shape = graph.shape
        let k = graph.spacing
        let k2 = k * k
        let floor2 = 0.01 * k2
        let theta2 = options.theta * options.theta
        let iterations = max(1, layoutIterations(graph, options))
        let barnesHut = n > options.barnesHutAbove
        let lobePull = graph.lobes.count > 1
        let startTemp: Float = 0.06 * shape.halfLength + 1.5 * k
        let endTemp: Float = 0.02 * k
        let chunkSize = 256
        let chunks = (n + chunkSize - 1) / chunkSize
        var force = [SynapseVector](repeating: SynapseVector(), count: n)
        var lastPublish = DispatchTime.now().uptimeNanoseconds
        let interval = UInt64(max(0, options.publishInterval) * 1_000_000_000)
        publish?(positions, 0)
        for iteration in 0..<iterations {
            if let stop, stop() { return positions }
            let progress = Float(iteration) / Float(iterations)
            let temp = startTemp * SynapseMath.pow(1 - progress, 2) + endTemp
            let tree = barnesHut ? SynapseOctree(positions) : nil
            positions.withUnsafeBufferPointer { points in
                force.withUnsafeMutableBufferPointer { out in
                    let target = out
                    let repulse: (Int) -> Void = { chunk in
                        let begin = chunk * chunkSize
                        let end = min(n, begin + chunkSize)
                        for i in begin..<end {
                            if let tree {
                                target[i] = tree.force(i, k2, theta2)
                                continue
                            }
                            var total = SynapseVector()
                            let p = points[i]
                            for j in 0..<n where j != i {
                                let d = p - points[j]
                                let dist2 = max(d.lengthSquared, floor2)
                                total += d * (k2 / dist2)
                            }
                            target[i] = total
                        }
                    }
                    if n > 300 {
                        DispatchQueue.concurrentPerform(iterations: chunks, execute: repulse)
                    } else {
                        for chunk in 0..<chunks { repulse(chunk) }
                    }
                }
            }
            for edge in graph.edges {
                let d = positions[edge.to] - positions[edge.from]
                let dist = d.length
                if dist < 1e-4 { continue }
                let magnitude = 1.1 * k * SynapseMath.log(max(dist, 0.3 * k) / k)
                let f = d * (magnitude / dist)
                force[edge.from] += f
                force[edge.to] -= f
            }
            for i in 0..<n {
                let node = graph.nodes[i]
                let hemisphere = Float(node.hemisphere)
                var p = positions[i]
                var f = force[i]
                if lobePull { f += (graph.lobes[node.lobe].anchor - p) * 0.05 }
                let r = shape.radiusOf(p)
                let normal = shape.shellNormal(p)
                let outer = shape.outerAt(p)
                if r > outer {
                    f -= normal * ((r - outer) * shape.halfLength * 2.5)
                } else if r < shape.innerShell {
                    f += normal * ((shape.innerShell - r) * shape.halfLength * 2.0)
                }
                let side = p.x * hemisphere
                if side < shape.fissure { f.x = f.x + hemisphere * (shape.fissure - side) * 2.0 }
                let len = f.length
                if len > 1e-6 { p += f * (min(len, temp) / len) }
                let after = shape.radiusOf(p)
                if after > 0.99 { p *= 0.99 / after }
                if p.x * hemisphere < 0.5 * shape.fissure { p.x = hemisphere * 0.5 * shape.fissure }
                positions[i] = p
            }
            if let publish {
                let now = DispatchTime.now().uptimeNanoseconds
                if now &- lastPublish >= interval {
                    publish(positions, Double(iteration + 1) / Double(iterations))
                    lastPublish = now
                }
            }
        }
        publish?(positions, 1)
        return positions
    }
}

final class SynapseOctree {
    private let points: [SynapseVector]
    private var order: [Int]
    private var scratch: [Int]
    private var centre: [SynapseVector] = []
    private var mass: [Float] = []
    private var size: [Float] = []
    private var children: [Int32] = []
    private var begin: [Int32] = []
    private var end: [Int32] = []
    private var leaf: [Bool] = []

    init(_ points: [SynapseVector]) {
        self.points = points
        let n = points.count
        order = Array(0..<n)
        scratch = [Int](repeating: 0, count: n)
        if n == 0 { return }
        var lo = points[0]
        var hi = points[0]
        for p in points {
            lo = SynapseVector(min(lo.x, p.x), min(lo.y, p.y), min(lo.z, p.z))
            hi = SynapseVector(max(hi.x, p.x), max(hi.y, p.y), max(hi.z, p.z))
        }
        let span = hi - lo
        let extent = max(max(max(span.x, span.y), span.z), 1e-3) * 1.001
        centre.reserveCapacity(n * 2)
        mass.reserveCapacity(n * 2)
        size.reserveCapacity(n * 2)
        children.reserveCapacity(n * 16)
        begin.reserveCapacity(n * 2)
        end.reserveCapacity(n * 2)
        leaf.reserveCapacity(n * 2)
        _ = build(0, n, (lo + hi) * 0.5, extent, 0)
    }

    private static func octant(_ p: SynapseVector, _ c: SynapseVector) -> Int {
        (p.x >= c.x ? 1 : 0) | (p.y >= c.y ? 2 : 0) | (p.z >= c.z ? 4 : 0)
    }

    private func build(_ first: Int, _ last: Int, _ boxCentre: SynapseVector, _ extent: Float, _ depth: Int) -> Int {
        let index = centre.count
        var sum = SynapseVector()
        for i in first..<last { sum += points[order[i]] }
        let cellMass = Float(last - first)
        centre.append(sum / max(1, cellMass))
        mass.append(cellMass)
        size.append(extent)
        begin.append(Int32(first))
        end.append(Int32(last))
        for _ in 0..<8 { children.append(-1) }
        if last - first <= 4 || depth >= 24 {
            leaf.append(true)
            return index
        }
        leaf.append(false)
        var counts = [Int](repeating: 0, count: 8)
        for i in first..<last { counts[Self.octant(points[order[i]], boxCentre)] += 1 }
        var starts = [Int](repeating: first, count: 9)
        for o in 0..<8 { starts[o + 1] = starts[o] + counts[o] }
        var cursor = Array(starts[0..<8])
        for i in first..<last {
            let body = order[i]
            let o = Self.octant(points[body], boxCentre)
            scratch[cursor[o]] = body
            cursor[o] += 1
        }
        for i in first..<last { order[i] = scratch[i] }
        let half = extent * 0.5
        let quarter = extent * 0.25
        for o in 0..<8 where counts[o] > 0 {
            let childCentre = boxCentre + SynapseVector((o & 1) != 0 ? quarter : -quarter,
                                                        (o & 2) != 0 ? quarter : -quarter,
                                                        (o & 4) != 0 ? quarter : -quarter)
            let child = build(starts[o], starts[o + 1], childCentre, half, depth + 1)
            children[index * 8 + o] = Int32(child)
        }
        return index
    }

    func force(_ body: Int, _ k2: Float, _ theta2: Float) -> SynapseVector {
        var total = SynapseVector()
        if centre.isEmpty { return total }
        let p = points[body]
        let floor2 = 0.01 * k2
        return withUnsafeTemporaryAllocation(of: Int.self, capacity: 512) { stack -> SynapseVector in
            var top = 0
            stack[top] = 0
            top += 1
            while top > 0 {
                top -= 1
                let cell = stack[top]
                if leaf[cell] {
                    for i in Int(begin[cell])..<Int(end[cell]) {
                        let other = order[i]
                        if other == body { continue }
                        let d = p - points[other]
                        let dist2 = max(d.lengthSquared, floor2)
                        total += d * (k2 / dist2)
                    }
                    continue
                }
                let d = p - centre[cell]
                let dist2 = d.lengthSquared
                let extent = size[cell]
                if extent * extent < theta2 * dist2 {
                    total += d * (mass[cell] * k2 / max(dist2, floor2))
                    continue
                }
                for o in 0..<8 {
                    let child = Int(children[cell * 8 + o])
                    if child >= 0 && top < 511 {
                        stack[top] = child
                        top += 1
                    }
                }
            }
            return total
        }
    }
}
