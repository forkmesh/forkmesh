import Foundation

public struct StrataDrillNode: Sendable, Equatable {
    public var module = false
    public var dependent = false
    public var index = -1
    public var weight = 0

    public init(module: Bool = false, dependent: Bool = false, index: Int = -1, weight: Int = 0) {
        self.module = module
        self.dependent = dependent
        self.index = index
        self.weight = weight
    }
}

public struct StrataDrillGraph: Sendable, Equatable {
    public var module = -1
    public var nodes: [StrataDrillNode] = []
    public var links: [StrataLayoutLink] = []
    public var layout = StrataGraphLayout()
    public var hiddenFiles = 0

    public init() {}
}

public enum StrataTreemapSize: Int, Sendable, CaseIterable {
    case lines = 0, complexity, functions

    public var label: String {
        switch self {
        case .lines: return "Lines of code"
        case .complexity: return "Complexity"
        case .functions: return "Functions"
        }
    }
}

public struct StrataTreemap: Sendable, Equatable {
    public var modules: [StrataRect] = []
    public var files: [StrataRect] = []
    public var headers: [Bool] = []

    public init() {}
}

extension Strata {
    public static func drillGraph(_ snapshot: InsightSnapshot, _ graph: InsightModuleGraph, module: Int, maxFiles: Int = 120) -> StrataDrillGraph {
        var out = StrataDrillGraph()
        out.module = module
        guard module >= 0 && module < graph.nodes.count else { return out }
        var files = stableSorted(graph.nodes[module].files) { a, b in
            let fa = snapshot.files[a]
            let fb = snapshot.files[b]
            let da = fa.dependsOn.count + fa.dependedOnBy.count
            let db = fb.dependsOn.count + fb.dependedOnBy.count
            if (da > 0) != (db > 0) { return da > 0 }
            return fa.codeLines + 40 * da > fb.codeLines + 40 * db
        }
        if files.count > maxFiles {
            out.hiddenFiles = files.count - maxFiles
            files = Array(files.prefix(maxFiles))
        }
        var nodeOfFile: [Int: Int] = [:]
        for f in files {
            nodeOfFile[f] = out.nodes.count
            out.nodes.append(StrataDrillNode(module: false, dependent: false, index: f, weight: snapshot.files[f].codeLines))
        }
        func moduleOf(_ file: Int) -> Int {
            file >= 0 && file < graph.moduleOfFile.count ? graph.moduleOfFile[file] : -1
        }
        let moduleCount = graph.nodes.count
        var usesWeight: [Int: Int] = [:]
        var usedByWeight: [Int: Int] = [:]
        var fileToModule: [Int: Int] = [:]
        var moduleToFile: [Int: Int] = [:]
        var internalLinks: [StrataLayoutLink] = []
        for f in files {
            let file = snapshot.files[f]
            for d in file.dependsOn {
                let m = moduleOf(d)
                if m == module {
                    if let to = nodeOfFile[d], d != f, let from = nodeOfFile[f] {
                        internalLinks.append(StrataLayoutLink(from, to, 1, false))
                    }
                } else if m >= 0 {
                    usesWeight[m, default: 0] += 1
                    fileToModule[f * moduleCount + m, default: 0] += 1
                }
            }
            for s in file.dependedOnBy {
                let m = moduleOf(s)
                if m >= 0 && m != module {
                    usedByWeight[m, default: 0] += 1
                    moduleToFile[f * moduleCount + m, default: 0] += 1
                }
            }
        }
        func strongest(_ weights: [Int: Int], _ limit: Int) -> [Int] {
            let list = weights.map { (-$0.value, $0.key) }.sorted { $0.0 != $1.0 ? $0.0 < $1.0 : $0.1 < $1.1 }
            return list.prefix(limit).map(\.1)
        }
        var usesNode: [Int: Int] = [:]
        for m in strongest(usesWeight, 24) {
            usesNode[m] = out.nodes.count
            out.nodes.append(StrataDrillNode(module: true, dependent: false, index: m, weight: usesWeight[m] ?? 0))
        }
        var usedByNode: [Int: Int] = [:]
        for m in strongest(usedByWeight, 24) {
            usedByNode[m] = out.nodes.count
            out.nodes.append(StrataDrillNode(module: true, dependent: true, index: m, weight: usedByWeight[m] ?? 0))
        }
        out.links = internalLinks
        for (key, value) in fileToModule {
            let f = key / moduleCount
            let m = key % moduleCount
            if let target = usesNode[m], let source = nodeOfFile[f] {
                out.links.append(StrataLayoutLink(source, target, value, false))
            }
        }
        for (key, value) in moduleToFile {
            let f = key / moduleCount
            let m = key % moduleCount
            if let source = usedByNode[m], let target = nodeOfFile[f] {
                out.links.append(StrataLayoutLink(source, target, value, false))
            }
        }
        out.links = stableSorted(out.links) { a, b in a.from != b.from ? a.from < b.from : a.to < b.to }
        do {
            let count = out.nodes.count
            var adjacency = [[Int]](repeating: [], count: count)
            for link in out.links { adjacency[link.from].append(link.to) }
            var cyclic = Set<Int>()
            for part in components(adjacency) where part.count >= 2 {
                let members = Set(part)
                for v in part {
                    for w in adjacency[v] where members.contains(w) { cyclic.insert(v * count + w) }
                }
            }
            for i in out.links.indices {
                out.links[i].cycle = cyclic.contains(out.links[i].from * count + out.links[i].to)
            }
        }
        var input = StrataLayoutInput()
        for node in out.nodes {
            if node.module {
                let name = graph.nodes[node.index].name
                input.sizes.append(StrataSize(clamp(60.0 + Double(name.utf16.count) * 7.0, 120.0, 230.0), 36))
            } else {
                let name = fileName(snapshot.files[node.index].path)
                let byName = 36.0 + Double(name.utf16.count) * 7.0
                let bySize = 80.0 + Double(max(0, node.weight)).squareRoot() * 1.6
                input.sizes.append(StrataSize(clamp(max(byName, bySize), 110.0, 260.0), 48))
            }
        }
        input.links = out.links
        input.layerGap = 84
        input.nodeGap = 28
        out.layout = layeredLayout(input)
        return out
    }

    public static func matrixOrder(_ graph: InsightModuleGraph) -> [Int] {
        let n = graph.nodes.count
        let layers = moduleLayers(graph)
        var cycleLayer: [Int: Int] = [:]
        var cycleName: [Int: String] = [:]
        for i in 0..<n {
            let c = graph.nodes[i].cycle
            if c < 0 { continue }
            cycleLayer[c] = max(cycleLayer[c] ?? 0, layers[i])
            let current = cycleName[c] ?? ""
            if current.isEmpty || U.less(graph.nodes[i].path, current) { cycleName[c] = graph.nodes[i].path }
        }
        return stableSorted(Array(0..<n)) { a, b in
            let na = graph.nodes[a]
            let nb = graph.nodes[b]
            let la = na.cycle >= 0 ? (cycleLayer[na.cycle] ?? 0) : layers[a]
            let lb = nb.cycle >= 0 ? (cycleLayer[nb.cycle] ?? 0) : layers[b]
            if la != lb { return la > lb }
            let ga = na.cycle >= 0 ? (cycleName[na.cycle] ?? "") : na.path
            let gb = nb.cycle >= 0 ? (cycleName[nb.cycle] ?? "") : nb.path
            if ga != gb { return U.less(ga, gb) }
            let ca = na.cycle >= 0 ? 0 : 1
            let cb = nb.cycle >= 0 ? 0 : 1
            if ca != cb { return ca < cb }
            return U.less(na.path, nb.path)
        }
    }

    public static func squarify(_ weights: [Double], _ rect: StrataRect) -> [StrataRect] {
        var out = [StrataRect](repeating: StrataRect(), count: weights.count)
        var order: [Int] = []
        var total = 0.0
        for i in weights.indices where weights[i] > 0 {
            order.append(i)
            total += weights[i]
        }
        if order.isEmpty || total <= 0 || rect.width <= 0 || rect.height <= 0 { return out }
        order = stableSorted(order) { weights[$0] > weights[$1] }
        let scale = rect.width * rect.height / total
        var free = rect
        var i = 0
        while i < order.count {
            let side = min(free.width, free.height)
            if side <= 0 {
                while i < order.count {
                    out[order[i]] = StrataRect(free.x, free.y, 0, 0)
                    i += 1
                }
                break
            }
            var rowSum = 0.0
            var minArea = Double.greatestFiniteMagnitude
            var maxArea = 0.0
            var worst = Double.greatestFiniteMagnitude
            var j = i
            while j < order.count {
                let area = weights[order[j]] * scale
                let sum = rowSum + area
                let lo = min(minArea, area)
                let hi = max(maxArea, area)
                let s2 = side * side
                let candidate = max(s2 * hi / (sum * sum), (sum * sum) / (s2 * lo))
                if j > i && candidate > worst { break }
                worst = candidate
                rowSum = sum
                minArea = lo
                maxArea = hi
                j += 1
            }
            let thickness = rowSum / side
            if free.width >= free.height {
                var y = free.top
                for k in i..<j {
                    let h = weights[order[k]] * scale / thickness
                    out[order[k]] = StrataRect(free.left, y, thickness, k + 1 == j ? free.bottom - y : h)
                    y += h
                }
                let left = min(free.right, free.left + thickness)
                free.width -= left - free.x
                free.x = left
            } else {
                var x = free.left
                for k in i..<j {
                    let w = weights[order[k]] * scale / thickness
                    out[order[k]] = StrataRect(x, free.top, k + 1 == j ? free.right - x : w, thickness)
                    x += w
                }
                let top = min(free.bottom, free.top + thickness)
                free.height -= top - free.y
                free.y = top
            }
            i = j
        }
        return out
    }

    public static func treemapSize(_ file: InsightFile, _ sizeBy: StrataTreemapSize) -> Double {
        switch sizeBy {
        case .lines: return Double(file.codeLines)
        case .complexity: return file.codeLines > 0 ? Double(max(1, file.complexity)) : 0
        case .functions: return file.codeLines > 0 ? Double(max(1, file.functions.count)) : 0
        }
    }

    public static func treemap(_ snapshot: InsightSnapshot, _ graph: InsightModuleGraph, sizeBy: StrataTreemapSize, area: StrataRect) -> StrataTreemap {
        var out = StrataTreemap()
        var moduleWeights = [Double](repeating: 0, count: graph.nodes.count)
        for m in graph.nodes.indices {
            for f in graph.nodes[m].files { moduleWeights[m] += treemapSize(snapshot.files[f], sizeBy) }
        }
        out.modules = squarify(moduleWeights, area)
        out.files = [StrataRect](repeating: StrataRect(), count: snapshot.files.count)
        out.headers = [Bool](repeating: false, count: graph.nodes.count)
        for m in graph.nodes.indices {
            let box = out.modules[m]
            if box.width < 2 || box.height < 2 { continue }
            let header = box.height > 44 && box.width > 70 ? 17.0 : 0
            out.headers[m] = header > 0
            let inner = box.adjusted(2, 2 + header, -2, -2)
            let weights = graph.nodes[m].files.map { treemapSize(snapshot.files[$0], sizeBy) }
            let cells = squarify(weights, inner)
            for (k, cell) in cells.enumerated() { out.files[graph.nodes[m].files[k]] = cell }
        }
        return out
    }
}
