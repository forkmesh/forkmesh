import Foundation

public struct StrataPoint: Sendable, Equatable {
    public var x: Double
    public var y: Double

    public init(_ x: Double, _ y: Double) {
        self.x = x
        self.y = y
    }
}

public struct StrataSize: Sendable, Equatable {
    public var width: Double
    public var height: Double

    public init(_ width: Double, _ height: Double) {
        self.width = width
        self.height = height
    }
}

public struct StrataRect: Sendable, Equatable {
    public var x: Double
    public var y: Double
    public var width: Double
    public var height: Double

    public init(_ x: Double = 0, _ y: Double = 0, _ width: Double = 0, _ height: Double = 0) {
        self.x = x
        self.y = y
        self.width = width
        self.height = height
    }

    public init(origin: StrataPoint, size: StrataSize) {
        self.init(origin.x, origin.y, size.width, size.height)
    }

    public var left: Double { x }
    public var top: Double { y }
    public var right: Double { x + width }
    public var bottom: Double { y + height }
    public var centerX: Double { x + width / 2 }
    public var centerY: Double { y + height / 2 }
    public var isNull: Bool { width == 0 && height == 0 }
    public var isEmpty: Bool { width <= 0 || height <= 0 }

    public func adjusted(_ dx1: Double, _ dy1: Double, _ dx2: Double, _ dy2: Double) -> StrataRect {
        StrataRect(x + dx1, y + dy1, width - dx1 + dx2, height - dy1 + dy2)
    }

    public func contains(_ point: StrataPoint) -> Bool {
        point.x >= min(left, right) && point.x <= max(left, right) && point.y >= min(top, bottom) && point.y <= max(top, bottom)
    }

    public func united(_ other: StrataRect) -> StrataRect {
        if isNull { return other }
        if other.isNull { return self }
        let l1 = min(x, x + width), r1 = max(x, x + width)
        let t1 = min(y, y + height), b1 = max(y, y + height)
        let l2 = min(other.x, other.x + other.width), r2 = max(other.x, other.x + other.width)
        let t2 = min(other.y, other.y + other.height), b2 = max(other.y, other.y + other.height)
        let l = min(l1, l2), t = min(t1, t2)
        return StrataRect(l, t, max(r1, r2) - l, max(b1, b2) - t)
    }
}

public struct StrataLayoutLink: Sendable, Equatable {
    public var from: Int
    public var to: Int
    public var weight: Int
    public var cycle: Bool

    public init(_ from: Int, _ to: Int, _ weight: Int = 1, _ cycle: Bool = false) {
        self.from = from
        self.to = to
        self.weight = weight
        self.cycle = cycle
    }
}

public struct StrataLayoutInput: Sendable, Equatable {
    public var sizes: [StrataSize] = []
    public var layers: [Int] = []
    public var links: [StrataLayoutLink] = []
    public var layerGap: Double = 96
    public var nodeGap: Double = 40
    public var maxPerRank = 0

    public init() {}
}

public struct StrataRoutedEdge: Sendable, Equatable {
    public var from = -1
    public var to = -1
    public var weight = 1
    public var cycle = false
    public var flat = false
    public var points: [StrataPoint] = []

    public init() {}
}

public struct StrataGraphLayout: Sendable, Equatable {
    public var boxes: [StrataRect] = []
    public var ranks: [Int] = []
    public var edges: [StrataRoutedEdge] = []
    public var bounds = StrataRect()
    public var crossings = 0
    public var rankCount = 0

    public init() {}
}

extension Strata {
    static func isotonic(_ values: inout [Double], _ weights: [Double]) {
        var sums: [Double] = []
        var totals: [Double] = []
        var counts: [Int] = []
        for i in values.indices {
            let w = max(weights[i], 1e-3)
            sums.append(values[i] * w)
            totals.append(w)
            counts.append(1)
            while sums.count >= 2 {
                let b = sums.count - 1
                let a = b - 1
                if sums[a] / totals[a] <= sums[b] / totals[b] { break }
                sums[a] += sums[b]
                totals[a] += totals[b]
                counts[a] += counts[b]
                sums.removeLast()
                totals.removeLast()
                counts.removeLast()
            }
        }
        var i = 0
        for k in sums.indices {
            let mean = sums[k] / totals[k]
            for _ in 0..<counts[k] {
                values[i] = mean
                i += 1
            }
        }
    }

    private struct Chain {
        var link = -1
        var reversed = false
        var flat = false
        var nodes: [Int] = []
    }

    public static func layeredLayout(_ input: StrataLayoutInput) -> StrataGraphLayout {
        var out = StrataGraphLayout()
        let n = input.sizes.count
        out.boxes = [StrataRect](repeating: StrataRect(), count: n)
        out.ranks = [Int](repeating: -1, count: n)
        if n == 0 { return out }

        var links: [StrataLayoutLink] = []
        var seen = Set<Int>()
        var adjacency = [[Int]](repeating: [], count: n)
        var degree = [Int](repeating: 0, count: n)
        for link in input.links {
            if link.from < 0 || link.from >= n || link.to < 0 || link.to >= n || link.from == link.to { continue }
            let key = link.from * n + link.to
            if seen.contains(key) { continue }
            seen.insert(key)
            links.append(link)
            adjacency[link.from].append(link.to)
            degree[link.from] += 1
            degree[link.to] += 1
        }
        let parts = components(adjacency)
        var comp = [Int](repeating: -1, count: n)
        var compSize = [Int](repeating: 0, count: parts.count)
        for (c, part) in parts.enumerated() {
            compSize[c] = part.count
            for v in part { comp[v] = c }
        }
        var layer = [Int](repeating: 0, count: n)
        if input.layers.count == n {
            for i in 0..<n { layer[i] = max(0, input.layers[i]) }
        } else {
            var compLayer = [Int](repeating: 0, count: parts.count)
            for (c, part) in parts.enumerated() {
                for v in part {
                    for w in adjacency[v] where comp[w] != c {
                        compLayer[c] = max(compLayer[c], compLayer[comp[w]] + 1)
                    }
                }
            }
            for i in 0..<n { layer[i] = compLayer[comp[i]] }
        }

        var active: [Int] = []
        var isolated: [Int] = []
        for i in 0..<n {
            if degree[i] > 0 { active.append(i) } else { isolated.append(i) }
        }

        var sub = [Int](repeating: 0, count: n)
        do {
            var weightOf: [Int: Int] = [:]
            for link in links { weightOf[link.from * n + link.to] = max(1, link.weight) }
            func weight(_ a: Int, _ b: Int) -> Double { Double(weightOf[a * n + b] ?? 1) }
            for part in parts where part.count >= 2 {
                let members = Set(part)
                var remaining = members
                var outWeight: [Int: Double] = [:]
                var inWeight: [Int: Double] = [:]
                var inside: [Int: [Int]] = [:]
                for v in part {
                    for w in adjacency[v] where members.contains(w) {
                        let wt = weight(v, w)
                        outWeight[v, default: 0] += wt
                        inWeight[w, default: 0] += wt
                        inside[v, default: []].append(w)
                    }
                }
                var incoming: [Int: [Int]] = [:]
                for v in part {
                    for w in inside[v] ?? [] { incoming[w, default: []].append(v) }
                }
                func remove(_ v: Int) {
                    remaining.remove(v)
                    for w in inside[v] ?? [] where remaining.contains(w) {
                        inWeight[w, default: 0] -= weight(v, w)
                    }
                    for u in incoming[v] ?? [] where remaining.contains(u) {
                        outWeight[u, default: 0] -= weight(u, v)
                    }
                }
                var front: [Int] = []
                var back: [Int] = []
                while !remaining.isEmpty {
                    var changed = true
                    while changed {
                        changed = false
                        for v in part where remaining.contains(v) && (outWeight[v] ?? 0) <= 1e-9 {
                            back.insert(v, at: 0)
                            remove(v)
                            changed = true
                        }
                        for v in part where remaining.contains(v) && (inWeight[v] ?? 0) <= 1e-9 {
                            front.append(v)
                            remove(v)
                            changed = true
                        }
                    }
                    if remaining.isEmpty { break }
                    var best = -1
                    var bestDelta = -1e18
                    for v in part where remaining.contains(v) {
                        let delta = (outWeight[v] ?? 0) - (inWeight[v] ?? 0)
                        if delta > bestDelta {
                            bestDelta = delta
                            best = v
                        }
                    }
                    front.append(best)
                    remove(best)
                }
                let sequence = front + back
                var position: [Int: Int] = [:]
                for (i, v) in sequence.enumerated() { position[v] = i }
                for v in sequence {
                    for u in incoming[v] ?? [] where (position[u] ?? 0) < (position[v] ?? 0) {
                        sub[v] = max(sub[v], sub[u] + 1)
                    }
                }
            }
        }

        var rank = [Int](repeating: -1, count: n)
        do {
            var maxLayer = 0
            var maxSub = 0
            for v in active {
                maxLayer = max(maxLayer, layer[v])
                maxSub = max(maxSub, sub[v])
            }
            let band = maxSub + 1
            let distinct = Array(Set(active.map { (maxLayer - layer[$0]) * band + sub[$0] })).sorted()
            var rankOf: [Int: Int] = [:]
            for (i, key) in distinct.enumerated() { rankOf[key] = i }
            for v in active { rank[v] = rankOf[(maxLayer - layer[v]) * band + sub[v]] ?? 0 }
        }
        var rankCount = 0
        for v in active { rankCount = max(rankCount, rank[v] + 1) }

        var cap = input.maxPerRank
        if cap <= 0 {
            var widths = 0.0
            var heights = 0.0
            for v in active {
                widths += input.sizes[v].width + input.nodeGap
                heights += input.sizes[v].height + input.layerGap
            }
            let count = Double(max(1, active.count))
            let aspect = 1.5
            cap = max(4, Int((aspect * count * (heights / count) / max(1.0, widths / count)).squareRoot().rounded()))
        }
        var predecessors = [[Int]](repeating: [], count: n)
        for link in links { predecessors[link.to].append(link.from) }
        do {
            let maxRank = rankCount - 1
            let sequence = stableSorted(active) { a, b in
                let la = maxRank - rank[a]
                let lb = maxRank - rank[b]
                if la != lb { return la < lb }
                if predecessors[a].count != predecessors[b].count { return predecessors[a].count > predecessors[b].count }
                return a < b
            }
            var level = [Int](repeating: -1, count: n)
            var fill: [Int: Int] = [:]
            var top = 0
            for v in sequence {
                var l = maxRank - rank[v]
                for w in adjacency[v] where rank[w] > rank[v] && level[w] >= 0 {
                    l = max(l, level[w] + 1)
                }
                while (fill[l] ?? 0) >= cap { l += 1 }
                level[v] = l
                fill[l, default: 0] += 1
                top = max(top, l)
            }
            for v in active { rank[v] = top - level[v] }
            rankCount = top + 1
        }
        do {
            var used = [Bool](repeating: false, count: rankCount)
            for v in active { used[rank[v]] = true }
            var remap = [Int](repeating: -1, count: rankCount)
            var next = 0
            for r in 0..<rankCount where used[r] {
                remap[r] = next
                next += 1
            }
            for v in active { rank[v] = remap[rank[v]] }
            rankCount = next
        }

        var vrank = rank
        var vwidth = input.sizes.map(\.width)
        let dummyWidth = 6.0
        var down = [[Int]](repeating: [], count: n)
        var up = [[Int]](repeating: [], count: n)
        var chains: [Chain] = []
        var dummies = 0
        for (l, link) in links.enumerated() {
            var chain = Chain()
            chain.link = l
            let ru = rank[link.from]
            let rv = rank[link.to]
            if ru == rv {
                chain.flat = true
                chain.nodes = [link.from, link.to]
                chains.append(chain)
                continue
            }
            chain.reversed = ru > rv
            let top = chain.reversed ? link.to : link.from
            let bottom = chain.reversed ? link.from : link.to
            chain.nodes.append(top)
            let span = rank[bottom] - rank[top] - 1
            if dummies + span <= 30000 && rank[top] + 1 < rank[bottom] {
                for r in (rank[top] + 1)..<rank[bottom] {
                    let d = vrank.count
                    vrank.append(r)
                    vwidth.append(dummyWidth)
                    down.append([])
                    up.append([])
                    chain.nodes.append(d)
                    dummies += 1
                }
            }
            chain.nodes.append(bottom)
            for k in 0..<(chain.nodes.count - 1) {
                down[chain.nodes[k]].append(chain.nodes[k + 1])
                up[chain.nodes[k + 1]].append(chain.nodes[k])
            }
            chains.append(chain)
        }
        let total = vrank.count
        func isReal(_ v: Int) -> Bool { v < n }

        var order = [[Int]](repeating: [], count: rankCount)
        for v in 0..<total where vrank[v] >= 0 { order[vrank[v]].append(v) }
        var pos = [Double](repeating: 0, count: total)
        func refreshPositions() {
            for row in order {
                for (i, v) in row.enumerated() { pos[v] = Double(i) }
            }
        }
        func groupOf(_ v: Int) -> Int { isReal(v) && compSize[comp[v]] > 1 ? comp[v] : -1 }
        if !order.isEmpty {
            order[0] = stableSorted(order[0]) { a, b in
                let ca = a < n ? comp[a] : -1
                let cb = b < n ? comp[b] : -1
                if ca != cb { return ca < cb }
                return a < b
            }
        }
        refreshPositions()
        func sweep(_ r: Int, _ fromAbove: Bool) {
            let row = order[r]
            let count = row.count
            var own = [Double](repeating: 0, count: count)
            for i in 0..<count {
                let neighbours = fromAbove ? up[row[i]] : down[row[i]]
                if neighbours.isEmpty {
                    own[i] = pos[row[i]]
                    continue
                }
                var sum = 0.0
                for w in neighbours { sum += pos[w] }
                own[i] = sum / Double(neighbours.count)
            }
            var unitSum: [Int: Double] = [:]
            var unitCount: [Int: Int] = [:]
            for i in 0..<count {
                let g = groupOf(row[i])
                if g < 0 { continue }
                unitSum[g, default: 0] += own[i]
                unitCount[g, default: 0] += 1
            }
            struct Key {
                var unit: Double
                var tag: Int
                var own: Double
                var current: Double
                var v: Int
            }
            var keys: [Key] = []
            keys.reserveCapacity(count)
            for i in 0..<count {
                let v = row[i]
                let g = groupOf(v)
                var unitKey = own[i]
                if g >= 0 { unitKey = (unitSum[g] ?? 0) / Double(unitCount[g] ?? 1) }
                keys.append(Key(unit: unitKey, tag: g >= 0 ? g : n + v, own: own[i], current: pos[v], v: v))
            }
            keys = stableSorted(keys) { a, b in
                if a.unit != b.unit { return a.unit < b.unit }
                if a.tag != b.tag { return a.tag < b.tag }
                if a.own != b.own { return a.own < b.own }
                return a.current < b.current
            }
            for i in 0..<count {
                order[r][i] = keys[i].v
                pos[keys[i].v] = Double(i)
            }
        }
        func countCrossings() -> Int {
            var crossings = 0
            if rankCount < 2 { return 0 }
            for r in 0..<(rankCount - 1) {
                var edges: [(Int, Int)] = []
                for v in order[r] {
                    for w in down[v] where vrank[w] == r + 1 {
                        edges.append((Int(pos[v]), Int(pos[w])))
                    }
                }
                edges.sort { $0.0 != $1.0 ? $0.0 < $1.0 : $0.1 < $1.1 }
                let width = order[r + 1].count + 1
                var tree = [Int](repeating: 0, count: width + 1)
                var seenCount = 0
                for edge in edges {
                    var greater = seenCount
                    var i = edge.1 + 1
                    while i > 0 {
                        greater -= tree[i]
                        i -= i & -i
                    }
                    crossings += greater
                    i = edge.1 + 1
                    while i <= width {
                        tree[i] += 1
                        i += i & -i
                    }
                    seenCount += 1
                }
            }
            return crossings
        }
        if rankCount > 1 {
            for r in 1..<rankCount { sweep(r, true) }
        }
        var best = order
        var bestCrossings = countCrossings()
        var iteration = 0
        while iteration < 16 && bestCrossings > 0 {
            if iteration % 2 == 0 {
                if rankCount > 1 { for r in 1..<rankCount { sweep(r, true) } }
            } else {
                if rankCount > 1 { for r in stride(from: rankCount - 2, through: 0, by: -1) { sweep(r, false) } }
            }
            let crossings = countCrossings()
            if crossings < bestCrossings {
                bestCrossings = crossings
                best = order
            }
            iteration += 1
        }
        order = best
        refreshPositions()
        out.crossings = bestCrossings

        var x = [Double](repeating: 0, count: total)
        func separation(_ a: Int, _ b: Int) -> Double {
            let ra = isReal(a)
            let rb = isReal(b)
            let gap = ra && rb ? input.nodeGap : (ra || rb ? input.nodeGap * 0.45 : 10.0)
            return (vwidth[a] + vwidth[b]) / 2.0 + gap
        }
        for row in order {
            var cursor = 0.0
            for i in row.indices {
                if i > 0 { cursor += separation(row[i - 1], row[i]) }
                x[row[i]] = cursor
            }
        }
        func place(_ r: Int, _ mode: Int) {
            let row = order[r]
            let count = row.count
            if count == 0 { return }
            var offsets = [Double](repeating: 0, count: count)
            for i in 1..<max(1, count) { offsets[i] = offsets[i - 1] + separation(row[i - 1], row[i]) }
            var targets = [Double](repeating: 0, count: count)
            var weights = [Double](repeating: 0, count: count)
            for i in 0..<count {
                let v = row[i]
                var sum = 0.0
                var k = 0
                if mode != 1 {
                    for w in up[v] {
                        sum += x[w]
                        k += 1
                    }
                }
                if mode != 0 {
                    for w in down[v] {
                        sum += x[w]
                        k += 1
                    }
                }
                let desired = k > 0 ? sum / Double(k) : x[v]
                targets[i] = desired - offsets[i]
                weights[i] = k == 0 ? 0.05 : (isReal(v) ? 1.0 + 0.2 * Double(k) : 6.0)
            }
            isotonic(&targets, weights)
            for i in 0..<count { x[row[i]] = targets[i] + offsets[i] }
        }
        for _ in 0..<10 {
            if rankCount > 1 {
                for r in 1..<rankCount { place(r, 0) }
                for r in stride(from: rankCount - 2, through: 0, by: -1) { place(r, 1) }
            }
        }
        for r in 0..<rankCount { place(r, 2) }

        var rankTop = [Double](repeating: 0, count: rankCount)
        var rankHeight = [Double](repeating: 20, count: rankCount)
        for v in active { rankHeight[rank[v]] = max(rankHeight[rank[v]], input.sizes[v].height) }
        var cursorY = 0.0
        for r in 0..<rankCount {
            rankTop[r] = cursorY
            cursorY += rankHeight[r] + input.layerGap
        }
        var minX = Double.greatestFiniteMagnitude
        for v in 0..<total where vrank[v] >= 0 { minX = min(minX, x[v] - vwidth[v] / 2.0) }
        if minX == Double.greatestFiniteMagnitude { minX = 0 }
        for v in 0..<total { x[v] -= minX }
        for v in active {
            let size = input.sizes[v]
            let r = rank[v]
            out.boxes[v] = StrataRect(x[v] - size.width / 2.0, rankTop[r] + (rankHeight[r] - size.height) / 2.0, size.width, size.height)
            out.ranks[v] = r
        }

        var exitPort: [Int: Double] = [:]
        var entryPort: [Int: Double] = [:]
        do {
            var exits = [[(Double, Int)]](repeating: [], count: n)
            var entries = [[(Double, Int)]](repeating: [], count: n)
            for (c, chain) in chains.enumerated() where !chain.flat {
                let top = chain.nodes[0]
                let bottom = chain.nodes[chain.nodes.count - 1]
                exits[top].append((x[chain.nodes[1]], c))
                entries[bottom].append((x[chain.nodes[chain.nodes.count - 2]], c))
            }
            for v in 0..<n {
                let box = out.boxes[v]
                func assign(_ list: [(Double, Int)], _ ports: inout [Int: Double]) {
                    let sorted = stableSorted(list) { a, b in a.0 != b.0 ? a.0 < b.0 : a.1 < b.1 }
                    let m = sorted.count
                    for k in 0..<m {
                        let t = m == 1 ? 0.5 : 0.22 + 0.56 * Double(k) / Double(m - 1)
                        ports[sorted[k].1 * n + v] = box.left + box.width * t
                    }
                }
                assign(exits[v], &exitPort)
                assign(entries[v], &entryPort)
            }
        }
        for (c, chain) in chains.enumerated() {
            let link = links[chain.link]
            var edge = StrataRoutedEdge()
            edge.from = link.from
            edge.to = link.to
            edge.weight = link.weight
            edge.cycle = link.cycle
            edge.flat = chain.flat
            if chain.flat {
                let a = out.boxes[link.from]
                let b = out.boxes[link.to]
                let below = a.centerX <= b.centerX
                let dx = abs(b.centerX - a.centerX)
                let sx = a.centerX + (below ? 0.18 : -0.18) * a.width
                let ex = b.centerX + (below ? -0.18 : 0.18) * b.width
                let sy = below ? a.bottom : a.top
                let ey = below ? b.bottom : b.top
                let bulge = 26 + 0.12 * dx
                let my = below ? max(sy, ey) + bulge : min(sy, ey) - bulge
                edge.points = [StrataPoint(sx, sy), StrataPoint((sx + ex) / 2.0, my), StrataPoint(ex, ey)]
            } else {
                let top = chain.nodes[0]
                let bottom = chain.nodes[chain.nodes.count - 1]
                var points: [StrataPoint] = []
                points.append(StrataPoint(exitPort[c * n + top] ?? out.boxes[top].centerX, out.boxes[top].bottom))
                if chain.nodes.count > 2 {
                    for k in 1..<(chain.nodes.count - 1) {
                        let d = chain.nodes[k]
                        let r = vrank[d]
                        points.append(StrataPoint(x[d], rankTop[r]))
                        points.append(StrataPoint(x[d], rankTop[r] + rankHeight[r]))
                    }
                }
                points.append(StrataPoint(entryPort[c * n + bottom] ?? out.boxes[bottom].centerX, out.boxes[bottom].top))
                if chain.reversed { points.reverse() }
                edge.points = points
            }
            out.edges.append(edge)
        }

        var bounds = StrataRect()
        for v in active { bounds = bounds.united(out.boxes[v]) }
        for edge in out.edges {
            for p in edge.points { bounds = bounds.united(StrataRect(p.x, p.y, 1, 1)) }
        }
        if !isolated.isEmpty {
            var area = 0.0
            for v in isolated {
                area += (input.sizes[v].width + input.nodeGap * 0.6) * (input.sizes[v].height + input.nodeGap * 0.6)
            }
            let beside = !active.isEmpty && bounds.height > bounds.width * 1.1
            if beside {
                let startX = bounds.right + input.nodeGap * 2.0
                let limit = max(bounds.height, 400.0)
                var cx = startX
                var cy = bounds.top
                var columnWidth = 0.0
                for v in isolated {
                    let size = input.sizes[v]
                    if cy > bounds.top && cy + size.height > bounds.top + limit {
                        cy = bounds.top
                        cx += columnWidth + input.nodeGap * 0.6
                        columnWidth = 0
                    }
                    out.boxes[v] = StrataRect(cx, cy, size.width, size.height)
                    out.ranks[v] = rankCount
                    cy += size.height + input.nodeGap * 0.6
                    columnWidth = max(columnWidth, size.width)
                }
            } else {
                let startY = active.isEmpty ? 0 : bounds.bottom + input.layerGap * 0.7
                let upper = max(active.isEmpty ? 0.0 : bounds.width, 1100.0)
                let limit = clamp(area.squareRoot() * 1.9, 600.0, upper)
                var cx = active.isEmpty ? 0 : bounds.left
                let left = cx
                var cy = startY
                var rowHeight = 0.0
                for v in isolated {
                    let size = input.sizes[v]
                    if cx > left && cx + size.width > left + limit {
                        cx = left
                        cy += rowHeight + input.nodeGap * 0.6
                        rowHeight = 0
                    }
                    out.boxes[v] = StrataRect(cx, cy, size.width, size.height)
                    out.ranks[v] = rankCount
                    cx += size.width + input.nodeGap * 0.6
                    rowHeight = max(rowHeight, size.height)
                }
            }
            for v in isolated { bounds = bounds.united(out.boxes[v]) }
            rankCount += 1
        }
        out.bounds = bounds
        out.rankCount = rankCount
        return out
    }

    public static func moduleBoxSize(_ codeLines: Int) -> StrataSize {
        let width = clamp(96.0 + Double(max(0, codeLines)).squareRoot(), 128.0, 340.0)
        let height = clamp(width * 0.62, 76.0, 210.0)
        return StrataSize(width.rounded(), height.rounded())
    }

    public static func moduleLayers(_ graph: InsightModuleGraph) -> [Int] {
        let n = graph.nodes.count
        var layers = [Int](repeating: 0, count: n)
        if graph.nodes.contains(where: { $0.layer > 0 }) {
            for i in 0..<n { layers[i] = graph.nodes[i].layer }
            var consistent = true
            for edge in graph.edges where !edge.inCycle && edge.from >= 0 && edge.to >= 0 && layers[edge.from] <= layers[edge.to] {
                consistent = false
                break
            }
            if consistent { return layers }
        }
        var adjacency = [[Int]](repeating: [], count: n)
        for edge in graph.edges where edge.from >= 0 && edge.from < n && edge.to >= 0 && edge.to < n && edge.from != edge.to {
            adjacency[edge.from].append(edge.to)
        }
        let parts = components(adjacency)
        var comp = [Int](repeating: 0, count: n)
        for (c, part) in parts.enumerated() {
            for v in part { comp[v] = c }
        }
        var compLayer = [Int](repeating: 0, count: parts.count)
        for (c, part) in parts.enumerated() {
            for v in part {
                for w in adjacency[v] where comp[w] != c {
                    compLayer[c] = max(compLayer[c], compLayer[comp[w]] + 1)
                }
            }
        }
        for i in 0..<n { layers[i] = compLayer[comp[i]] }
        return layers
    }

    public static func moduleLayout(_ graph: InsightModuleGraph) -> StrataGraphLayout {
        var input = StrataLayoutInput()
        input.sizes = graph.nodes.map { moduleBoxSize($0.codeLines) }
        input.layers = moduleLayers(graph)
        input.links = graph.edges.map { StrataLayoutLink($0.from, $0.to, $0.weight, $0.inCycle) }
        input.layerGap = 80
        input.nodeGap = 44
        return layeredLayout(input)
    }
}
