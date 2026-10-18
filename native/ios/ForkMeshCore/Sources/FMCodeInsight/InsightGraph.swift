import Foundation

extension Insight {
    static func clamp01(_ value: Double) -> Double { max(0, min(1, value)) }

    public static func resolveDependencies(_ snapshot: inout InsightSnapshot) {
        let resolver = Resolver(snapshot)
        for i in snapshot.files.indices {
            var file = snapshot.files[i]
            var targets = Set<Int>()
            var shown: [String] = []
            var external: [String] = []
            let family = Resolver.family(file.language)
            for spec in file.imports {
                let optional = spec.hasPrefix("?")
                let hits = resolver.resolve(i, spec)
                for hit in hits where hit != i { targets.insert(hit) }
                if optional { continue }
                shown.append(spec)
                if hits.isEmpty {
                    if file.language == "C#" && resolver.namespaces.contains(spec) { continue }
                    if family == "jvm" && (resolver.namespaces.contains(PathText.section(spec, ".", 0, -2))
                        || resolver.namespaces.contains(spec.replacingOccurrences(of: ".*", with: ""))) {
                        continue
                    }
                    external.append(spec)
                }
            }
            if let declaredTypes = resolver.types[family] {
                for reference in file.references {
                    guard let declared = declaredTypes[reference], declared.count <= 2 else { continue }
                    for hit in declared where hit != i { targets.insert(hit) }
                }
            }
            file.references = []
            file.imports = shown
            file.externalImports = external
            file.dependsOn = targets.sorted()
            file.dependedOnBy = []
            snapshot.files[i] = file
        }
        for i in snapshot.files.indices {
            for target in snapshot.files[i].dependsOn { snapshot.files[target].dependedOnBy.append(i) }
        }
    }

    public static func stronglyConnected(_ adjacency: [[Int]]) -> [[Int]] {
        let n = adjacency.count
        var index = [Int](repeating: -1, count: n)
        var low = [Int](repeating: 0, count: n)
        var onStack = [Bool](repeating: false, count: n)
        var stack: [Int] = []
        var components: [[Int]] = []
        var counter = 0
        for start in 0..<n where index[start] < 0 {
            var work: [(node: Int, edge: Int)] = [(start, 0)]
            index[start] = counter
            low[start] = counter
            counter += 1
            stack.append(start)
            onStack[start] = true
            while !work.isEmpty {
                let top = work.count - 1
                let node = work[top].node
                let edges = adjacency[node]
                if work[top].edge < edges.count {
                    let next = edges[work[top].edge]
                    work[top].edge += 1
                    if next < 0 || next >= n { continue }
                    if index[next] < 0 {
                        index[next] = counter
                        low[next] = counter
                        counter += 1
                        stack.append(next)
                        onStack[next] = true
                        work.append((next, 0))
                    } else if onStack[next] {
                        low[node] = min(low[node], index[next])
                    }
                    continue
                }
                work.removeLast()
                if let parent = work.last {
                    low[parent.node] = min(low[parent.node], low[node])
                }
                if low[node] == index[node] {
                    var component: [Int] = []
                    while true {
                        let member = stack.removeLast()
                        onStack[member] = false
                        component.append(member)
                        if member == node { break }
                    }
                    component.sort()
                    components.append(component)
                }
            }
        }
        return components
    }

    public static func markCycles(_ snapshot: inout InsightSnapshot) {
        let adjacency = snapshot.files.map(\.dependsOn)
        var cycles = stronglyConnected(adjacency).filter { $0.count > 1 }
        cycles.sort { a, b in
            if a.count != b.count { return a.count > b.count }
            return a[0] < b[0]
        }
        for i in snapshot.files.indices { snapshot.files[i].cycle = -1 }
        for (c, cycle) in cycles.enumerated() {
            for member in cycle { snapshot.files[member].cycle = c }
        }
        snapshot.cycles = cycles
    }

    static func applyWindows(_ snapshot: inout InsightSnapshot, _ significant: [[Int]], _ windows: [[UInt64]]) {
        let totalWindows = windows.reduce(0) { $0 + $1.count }
        if totalWindows > 6_000_000 { return }
        var counts: [UInt64: Int] = [:]
        counts.reserveCapacity(totalWindows)
        for list in windows {
            for hash in list { counts[hash, default: 0] += 1 }
        }
        var i = 0
        while i < snapshot.files.count && i < windows.count {
            let list = windows[i]
            let lines = significant[i]
            var marked = Set<Int>()
            for k in 0..<list.count where (counts[list[k]] ?? 0) >= 2 {
                var j = 0
                while j < Measure.window && k + j < lines.count {
                    marked.insert(lines[k + j])
                    j += 1
                }
            }
            snapshot.files[i].duplicatedLines = marked.count
            i += 1
        }
    }

    public static func markDuplication(_ snapshot: inout InsightSnapshot, texts: [String: [UInt8]]) {
        var significant = [[Int]](repeating: [], count: snapshot.files.count)
        var windows = [[UInt64]](repeating: [], count: snapshot.files.count)
        for (i, file) in snapshot.files.enumerated() {
            guard let bytes = texts[file.path] else { continue }
            let measured = Measure.measure(file.path, bytes, windows: true)
            significant[i] = measured.significant
            windows[i] = measured.windows
        }
        applyWindows(&snapshot, significant, windows)
    }

    public static func computeHotspots(_ snapshot: inout InsightSnapshot) {
        var maxCommits = 0
        var maxLines = 1
        for file in snapshot.files {
            maxCommits = max(maxCommits, file.commits)
            maxLines = max(maxLines, file.codeLines)
        }
        var top = 0.0
        for i in snapshot.files.indices {
            let file = snapshot.files[i]
            if maxCommits <= 0 || file.commits <= 0 {
                snapshot.files[i].hotspot = 0
                continue
            }
            let change = Double(file.commits) / Double(maxCommits)
            let size = Double(max(0, file.codeLines)).squareRoot() / Double(maxLines).squareRoot()
            let hotspot = pow(change, 0.7) * pow(max(0.02, size), 0.3)
            snapshot.files[i].hotspot = hotspot
            top = max(top, hotspot)
        }
        if top > 0 {
            for i in snapshot.files.indices { snapshot.files[i].hotspot = clamp01(snapshot.files[i].hotspot / top) }
        }
    }

    public static func modulePathFor(_ filePath: String, depth: Int) -> String {
        let segments = filePath.components(separatedBy: "/")
        let dirs = segments.count - 1
        let take = min(max(1, depth), dirs)
        if take <= 0 { return "" }
        return segments[0..<take].joined(separator: "/")
    }

    public static func autoModuleDepth(_ snapshot: InsightSnapshot) -> Int {
        var previousCount = 0
        var bestDepth = 1
        var bestCount = 0
        for depth in 1...6 {
            var modules = Set<String>()
            for file in snapshot.files { modules.insert(modulePathFor(file.path, depth: depth)) }
            let count = modules.count
            if count >= 8 && count <= 60 { return depth }
            if count > 60 {
                return previousCount >= 3 ? depth - 1 : (count <= 140 ? depth : max(1, depth - 1))
            }
            if count > bestCount {
                bestCount = count
                bestDepth = depth
            }
            if count == previousCount && depth > 1 { break }
            previousCount = count
        }
        return max(1, bestDepth)
    }

    public static func buildModuleGraph(_ snapshot: InsightSnapshot, depth: Int = 0) -> InsightModuleGraph {
        var graph = InsightModuleGraph()
        graph.depth = depth > 0 ? depth : autoModuleDepth(snapshot)
        var byPath: [String: Int] = [:]
        var paths: [String] = []
        let modulePaths = snapshot.files.map { modulePathFor($0.path, depth: graph.depth) }
        for module in modulePaths where byPath[module] == nil {
            byPath[module] = -1
            paths.append(module)
        }
        paths.sort(by: U.less)
        for path in paths {
            var node = InsightModuleNode()
            node.path = path
            node.name = path.isEmpty ? "(root files)" : path
            byPath[path] = graph.nodes.count
            graph.nodes.append(node)
        }
        graph.moduleOfFile = [Int](repeating: 0, count: snapshot.files.count)
        var weightedHealth = [Double](repeating: 0, count: graph.nodes.count)
        var weights = [Double](repeating: 0, count: graph.nodes.count)
        for (i, file) in snapshot.files.enumerated() {
            let module = byPath[modulePaths[i]] ?? 0
            graph.moduleOfFile[i] = module
            graph.nodes[module].files.append(i)
            graph.nodes[module].codeLines += file.codeLines
            graph.nodes[module].complexity += file.complexity
            graph.nodes[module].functions += file.functions.count
            graph.nodes[module].types += file.types
            graph.nodes[module].abstractTypes += file.abstractTypes
            graph.nodes[module].commits += file.commits
            let weight = Double(max(1, file.codeLines))
            weightedHealth[module] += file.health * weight
            weights[module] += weight
        }
        var edgeIndex: [UInt64: Int] = [:]
        var incomingFiles = [Set<Int>](repeating: [], count: graph.nodes.count)
        var outgoingFiles = [Set<Int>](repeating: [], count: graph.nodes.count)
        for (i, file) in snapshot.files.enumerated() {
            let from = graph.moduleOfFile[i]
            for target in file.dependsOn {
                let to = graph.moduleOfFile[target]
                if from == to {
                    graph.nodes[from].internalEdges += 1
                    continue
                }
                let key = UInt64(UInt32(from)) << 32 | UInt64(UInt32(to))
                let position: Int
                if let existing = edgeIndex[key] {
                    position = existing
                } else {
                    var edge = InsightModuleEdge()
                    edge.from = from
                    edge.to = to
                    position = graph.edges.count
                    edgeIndex[key] = position
                    graph.edges.append(edge)
                }
                graph.edges[position].weight += 1
                graph.edges[position].fileEdges.append(InsightFileEdge(from: i, to: target))
                outgoingFiles[from].insert(target)
                incomingFiles[to].insert(i)
            }
        }
        var adjacency = [[Int]](repeating: [], count: graph.nodes.count)
        for edge in graph.edges { adjacency[edge.from].append(edge.to) }
        let components = stronglyConnected(adjacency)
        var componentOf = [Int](repeating: -1, count: graph.nodes.count)
        for (c, component) in components.enumerated() {
            for member in component { componentOf[member] = c }
        }
        let multi = components.filter { $0.count > 1 }
        graph.cycles = multi.enumerated().sorted { x, y in
            if x.element.count != y.element.count { return x.element.count > y.element.count }
            return x.offset < y.offset
        }.map(\.element)
        for (c, cycle) in graph.cycles.enumerated() {
            for member in cycle { graph.nodes[member].cycle = c }
        }
        for e in graph.edges.indices {
            graph.edges[e].inCycle = componentOf[graph.edges[e].from] == componentOf[graph.edges[e].to]
        }
        let componentCount = components.count
        var componentOut = [Set<Int>](repeating: [], count: componentCount)
        var componentIn = [Set<Int>](repeating: [], count: componentCount)
        for edge in graph.edges {
            let a = componentOf[edge.from]
            let b = componentOf[edge.to]
            if a != b {
                componentOut[a].insert(b)
                componentIn[b].insert(a)
            }
        }
        var remaining = [Int](repeating: 0, count: componentCount)
        var layer = [Int](repeating: 0, count: componentCount)
        var queue: [Int] = []
        for c in 0..<componentCount {
            remaining[c] = componentOut[c].count
            if remaining[c] == 0 { queue.append(c) }
        }
        var head = 0
        while head < queue.count {
            let c = queue[head]
            for predecessor in componentIn[c].sorted() {
                layer[predecessor] = max(layer[predecessor], layer[c] + 1)
                remaining[predecessor] -= 1
                if remaining[predecessor] == 0 { queue.append(predecessor) }
            }
            head += 1
        }
        for m in graph.nodes.indices {
            var node = graph.nodes[m]
            node.layer = layer[componentOf[m]]
            node.afferent = incomingFiles[m].count
            node.efferent = outgoingFiles[m].count
            let total = node.afferent + node.efferent
            node.instability = total > 0 ? Double(node.efferent) / Double(total) : 0
            node.abstractness = node.types > 0 ? Double(node.abstractTypes) / Double(node.types) : 0
            node.distance = abs(node.abstractness + node.instability - 1.0)
            node.cohesion = node.files.isEmpty ? 0 : (Double(node.internalEdges) + 1.0) / Double(node.files.count)
            node.health = weights[m] > 0 ? (weightedHealth[m] / weights[m] * 10.0).rounded() / 10.0 : 10.0
            graph.nodes[m] = node
        }
        return graph
    }
}
