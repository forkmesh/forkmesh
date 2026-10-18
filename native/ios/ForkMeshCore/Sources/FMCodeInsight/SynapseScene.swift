import Foundation

public enum SynapseColourBy: Int, CaseIterable, Sendable {
    case module, language, health, hotspot

    public var label: String {
        switch self {
        case .module: return "Module"
        case .language: return "Language"
        case .health: return "Health"
        case .hotspot: return "Hotspot"
        }
    }
}

public enum SynapseShowMode: Int, CaseIterable, Sendable {
    case everything, cycles, orphans, neighbourhood

    public var label: String {
        switch self {
        case .everything: return "Everything"
        case .cycles: return "Circular only"
        case .orphans: return "Orphans"
        case .neighbourhood: return "Neighbourhood"
        }
    }

    public var includesIsolated: Bool { self == .orphans }
}

public struct SynapseColour: Sendable, Equatable, Hashable {
    public var r: Float
    public var g: Float
    public var b: Float
    public var a: Float

    public init(_ r: Float, _ g: Float, _ b: Float, _ a: Float = 1) {
        self.r = r
        self.g = g
        self.b = b
        self.a = a
    }

    public init(rgb: UInt32, alpha: Float = 1) {
        r = Float((rgb >> 16) & 0xff) / 255
        g = Float((rgb >> 8) & 0xff) / 255
        b = Float(rgb & 0xff) / 255
        a = alpha
    }

    public init(bytes r: Int, _ g: Int, _ b: Int, _ a: Int = 255) {
        self.r = Float(r) / 255
        self.g = Float(g) / 255
        self.b = Float(b) / 255
        self.a = Float(a) / 255
    }

    public func mixed(_ other: SynapseColour, _ t: Float) -> SynapseColour {
        let u = 1 - t
        return SynapseColour(r * u + other.r * t, g * u + other.g * t, b * u + other.b * t, a * u + other.a * t)
    }

    public func withAlpha(_ alpha: Float) -> SynapseColour {
        SynapseColour(r, g, b, SynapseMath.clamp(alpha, 0, 1))
    }

    func hsv() -> (h: Float, s: Float, v: Float) {
        let high = max(r, g, b)
        let low = min(r, g, b)
        let delta = high - low
        var h: Float = 0
        if delta > 0 {
            if high == r {
                h = (g - b) / delta
            } else if high == g {
                h = (b - r) / delta + 2
            } else {
                h = (r - g) / delta + 4
            }
            h /= 6
            if h < 0 { h += 1 }
        }
        return (h, high > 0 ? delta / high : 0, high)
    }

    static func fromHsv(_ h: Float, _ s: Float, _ v: Float, _ a: Float) -> SynapseColour {
        if s <= 0 { return SynapseColour(v, v, v, a) }
        let sector = (h - h.rounded(.down)) * 6
        let i = Int(sector.rounded(.down)) % 6
        let f = sector - sector.rounded(.down)
        let p = v * (1 - s)
        let q = v * (1 - s * f)
        let t = v * (1 - s * (1 - f))
        switch i {
        case 0: return SynapseColour(v, t, p, a)
        case 1: return SynapseColour(q, v, p, a)
        case 2: return SynapseColour(p, v, t, a)
        case 3: return SynapseColour(p, q, v, a)
        case 4: return SynapseColour(t, p, v, a)
        default: return SynapseColour(v, p, q, a)
        }
    }

    public func lighter(_ factor: Int) -> SynapseColour {
        if factor <= 0 { return self }
        if factor < 100 { return darker(10000 / factor) }
        var (h, s, v) = hsv()
        v = v * Float(factor) / 100
        if v > 1 {
            s = max(0, s - (v - 1))
            v = 1
        }
        return Self.fromHsv(h, s, v, a)
    }

    public func darker(_ factor: Int) -> SynapseColour {
        if factor <= 0 { return self }
        if factor < 100 { return lighter(10000 / factor) }
        let (h, s, v) = hsv()
        return Self.fromHsv(h, s, v * 100 / Float(factor), a)
    }

    public var key: UInt32 {
        let q = { (x: Float, bits: Float) in UInt32(SynapseMath.clamp(x, 0, 1) * bits + 0.5) }
        return q(r, 31) << 15 | q(g, 31) << 10 | q(b, 31) << 5 | q(a, 31)
    }
}

public enum SynapsePalette {
    public static let background = SynapseColour(bytes: 7, 11, 22)
    public static let cycle = SynapseColour(bytes: 255, 84, 80)
    public static let imports = SynapseColour(bytes: 60, 200, 255)
    public static let importers = SynapseColour(bytes: 188, 140, 255)
    public static let selected = SynapseColour(bytes: 236, 246, 255)
    public static let text = SynapseColour(bytes: 230, 237, 243)
    public static let muted = SynapseColour(bytes: 139, 148, 158)
    public static let white = SynapseColour(1, 1, 1)

    static let lobes: [UInt32] = [
        0x6cb6ff, 0x56d4dd, 0xffa657, 0xd2a8ff, 0xe3c06d, 0x79e2c0,
        0xf2a7e0, 0xa5b4fc, 0x9ee7ff, 0xffc680, 0xb49cff, 0x8fd3b6,
    ]

    static let languages: [String: UInt32] = [
        "C++": 0xf34b7d, "C": 0x8fa3b8, "TypeScript": 0x4a9bf0, "JavaScript": 0xf1e05a,
        "Python": 0x5b8fd6, "Go": 0x00c0e8, "Rust": 0xdea584, "Java": 0xd08a3c,
        "Kotlin": 0xa97bff, "Swift": 0xf06a4a, "C#": 0x9b6fd6, "Ruby": 0xe04a5f,
        "PHP": 0x8c92d8, "Dart": 0x3dc5c5, "Objective-C": 0x649cf0, "Shell": 0x89e051,
        "QML": 0x44d08e, "Vue": 0x41d89a, "Svelte": 0xff6e40, "CSS": 0x9a7fe0,
        "SCSS": 0xd66fa8, "Lua": 0x607cf0, "Scala": 0xe05048,
    ]

    public static func lobe(_ index: Int) -> SynapseColour {
        let count = lobes.count
        return SynapseColour(rgb: lobes[((index % count) + count) % count])
    }

    public static func language(_ name: String) -> SynapseColour {
        if let known = languages[name] { return SynapseColour(rgb: known) }
        return lobe(Int(Synapse.stableHash(name) % 12))
    }

    public static func health(_ value: Double) -> SynapseColour {
        if value >= 8 { return SynapseColour(rgb: 0x3fb98f) }
        if value >= 4 { return SynapseColour(rgb: 0xe3b341) }
        return SynapseColour(rgb: 0xe5534b)
    }

    public static func hotspot(_ value: Double) -> SynapseColour {
        let t = Float(min(max(value, 0), 1))
        let cool = SynapseColour(rgb: 0x3a4d72)
        let warm = SynapseColour(rgb: 0xe3a23c)
        let hot = SynapseColour(rgb: 0xff6f3c)
        if t < 0.5 { return cool.mixed(warm, t * 2) }
        return warm.mixed(hot, (t - 0.5) * 2)
    }

    public static func shortLobeName(_ name: String) -> String {
        let parts = name.split(separator: "/", omittingEmptySubsequences: true)
        if parts.count <= 2 { return name }
        return parts.suffix(2).joined(separator: "/")
    }
}

public enum SynapseNodeState: UInt8, Sendable {
    case normal, dim, selected, imports, importers, match, hidden
}

public enum SynapseEdgeState: UInt8, Sendable {
    case normal, dim, imports, importers, cycle, hidden

    public var highlighted: Bool { self == .imports || self == .importers || self == .cycle }
}

public struct SynapseStyleInput: Sendable, Equatable {
    public var selected = -1
    public var isolatedCycle = -1
    public var showMode = SynapseShowMode.everything
    public var depth = 1
    public var query = ""
    public var colourBy = SynapseColourBy.module
    public var pulses = true
    public var edgeCap = 5000
    public var dimCap = 1200

    public init() {}
}

public struct SynapseLegendRow: Sendable, Equatable {
    public var colour: SynapseColour
    public var label: String
}

public struct SynapseStyle: Sendable {
    public var input = SynapseStyleInput()
    public var nodeState: [SynapseNodeState] = []
    public var nodeColour: [SynapseColour] = []
    public var baseColour: [SynapseColour] = []
    public var edgeState: [SynapseEdgeState] = []
    public var drawEdges: [Int] = []
    public var edgeMid: [SynapseColour] = []
    public var hubs: [Int] = []
    public var neighbours = SynapseNeighbourhood()
    public var cycleEdges = 0
    public var pulseEdges = 0
    public var visibleNodes = 0
    public var cycleNodesVisible = false

    public init() {}

    public static func base(_ graph: SynapseGraph, _ snapshot: InsightSnapshot, _ node: Int, _ mode: SynapseColourBy) -> SynapseColour {
        let n = graph.nodes[node]
        let file = snapshot.files[n.file]
        switch mode {
        case .module: return SynapsePalette.lobe(n.lobe)
        case .language: return SynapsePalette.language(file.language)
        case .health: return SynapsePalette.health(file.health)
        case .hotspot: return SynapsePalette.hotspot(file.hotspot)
        }
    }

    public static func make(_ graph: SynapseGraph, _ snapshot: InsightSnapshot, _ input: SynapseStyleInput) -> SynapseStyle {
        var style = SynapseStyle()
        style.input = input
        let n = graph.nodes.count
        let selected = input.selected >= 0 && input.selected < snapshot.files.count ? input.selected : -1
        var visible = [Bool](repeating: true, count: n)
        let isolated = input.isolatedCycle >= 0 && input.isolatedCycle < graph.cycles.count
        if isolated {
            visible = [Bool](repeating: false, count: n)
            for file in graph.cycles[input.isolatedCycle].files {
                let node = graph.nodeFor(file)
                if node >= 0 { visible[node] = true }
            }
        } else if input.showMode == .cycles {
            for i in 0..<n { visible[i] = graph.nodes[i].cycle }
        } else if input.showMode == .orphans {
            for i in 0..<n { visible[i] = graph.nodes[i].fanIn == 0 && graph.nodes[i].fanOut == 0 }
        }
        style.neighbours = selected >= 0
            ? Synapse.neighbourhood(snapshot, selected, hops: min(max(input.depth, 1), 3), direction: .both)
            : SynapseNeighbourhood()
        if input.showMode == .neighbourhood && selected >= 0 && !isolated {
            for i in 0..<n {
                let file = graph.nodes[i].file
                visible[i] = file == selected || style.neighbours.contains(file)
            }
        }
        let query = input.query.trimmingCharacters(in: .whitespacesAndNewlines)
        let searching = !query.isEmpty && selected < 0
        style.nodeState = [SynapseNodeState](repeating: .normal, count: n)
        style.nodeColour = [SynapseColour](repeating: SynapsePalette.muted, count: n)
        style.baseColour = [SynapseColour](repeating: SynapsePalette.muted, count: n)
        for i in 0..<n {
            let node = graph.nodes[i]
            var state = SynapseNodeState.normal
            if !visible[i] {
                state = .hidden
            } else if selected >= 0 {
                if node.file == selected {
                    state = .selected
                } else if style.neighbours.imports[node.file] != nil {
                    state = .imports
                } else if style.neighbours.importers[node.file] != nil {
                    state = .importers
                } else {
                    state = .dim
                }
            } else if searching {
                state = snapshot.files[node.file].path.range(of: query, options: .caseInsensitive) != nil ? .match : .dim
            }
            style.nodeState[i] = state
            if state != .hidden { style.visibleNodes += 1 }
            if state == .normal && node.cycle { style.cycleNodesVisible = true }
            let base = Self.base(graph, snapshot, i, input.colourBy)
            style.baseColour[i] = base
            let colour: SynapseColour
            switch state {
            case .selected: colour = SynapsePalette.selected
            case .imports: colour = SynapsePalette.imports
            case .importers: colour = SynapsePalette.importers
            case .match: colour = node.cycle ? SynapsePalette.cycle : base.lighter(125)
            case .dim: colour = (node.cycle ? SynapsePalette.cycle : base).mixed(SynapsePalette.background, 0.62)
            default: colour = node.cycle ? SynapsePalette.cycle : base
            }
            style.nodeColour[i] = colour
        }
        style.edgeState = [SynapseEdgeState](repeating: .normal, count: graph.edges.count)
        var highlighted: [Int] = []
        var normal: [Int] = []
        var dim: [Int] = []
        for e in graph.edges.indices {
            let edge = graph.edges[e]
            let a = style.nodeState[edge.from]
            let b = style.nodeState[edge.to]
            var state = SynapseEdgeState.normal
            if a == .hidden || b == .hidden {
                state = .hidden
            } else if selected >= 0 {
                let fa = graph.nodes[edge.from].file
                let fb = graph.nodes[edge.to].file
                let ia = fa == selected ? 0 : (style.neighbours.imports[fa] ?? -1)
                let ib = style.neighbours.imports[fb] ?? -1
                let ra = style.neighbours.importers[fa] ?? -1
                let rb = fb == selected ? 0 : (style.neighbours.importers[fb] ?? -1)
                if ia >= 0 && ib == ia + 1 {
                    state = .imports
                } else if rb >= 0 && ra == rb + 1 {
                    state = .importers
                } else {
                    state = .dim
                }
            } else if searching {
                state = a == .match && b == .match ? .normal : .dim
            } else if edge.cycle {
                let cycle = snapshot.files[graph.nodes[edge.from].file].cycle
                let small = cycle >= 0 && cycle < snapshot.cycles.count && snapshot.cycles[cycle].count <= 40
                if isolated || small { state = .cycle }
            }
            style.edgeState[e] = state
            if state.highlighted {
                highlighted.append(e)
            } else if state == .normal {
                normal.append(e)
            } else if state == .dim {
                dim.append(e)
            }
        }
        let cap = input.edgeCap
        let bySeed = { (x: Int, y: Int) -> Bool in
            let sx = graph.edges[x].seed
            let sy = graph.edges[y].seed
            return sx != sy ? sx < sy : x < y
        }
        if highlighted.count > cap {
            highlighted.sort(by: bySeed)
            highlighted.removeSubrange(cap...)
        }
        var room = cap - highlighted.count
        if normal.count > room {
            normal.sort(by: bySeed)
            normal.removeSubrange(max(0, room)...)
        }
        room -= normal.count
        let dimRoom = max(0, min(room, input.dimCap))
        if dim.count > dimRoom {
            dim.sort(by: bySeed)
            dim.removeSubrange(dimRoom...)
        }
        style.drawEdges = dim + normal + highlighted
        style.cycleEdges = highlighted.reduce(0) { $0 + (style.edgeState[$1] == .cycle ? 1 : 0) }
        style.edgeMid = [SynapseColour](repeating: SynapsePalette.muted, count: graph.edges.count)
        for e in style.drawEdges { style.edgeMid[e] = style.edgeColour(graph, e, 0.5) }
        style.pulseEdges = style.drawEdges.reduce(0) { $0 + (style.edgePulses(graph, $1) ? 1 : 0) }
        var hubs: [Int] = []
        for i in 0..<n where style.nodeState[i] != .hidden && style.nodeState[i] != .dim && graph.nodes[i].fanIn > 0 {
            hubs.append(i)
        }
        hubs.sort { x, y in
            let fx = graph.nodes[x].fanIn
            let fy = graph.nodes[y].fanIn
            return fx != fy ? fx > fy : x < y
        }
        style.hubs = Array(hubs.prefix(10))
        return style
    }

    public func nodeRadius(_ graph: SynapseGraph, _ node: Int) -> Float {
        let base = min(graph.spacing * 0.15, 3.4) * graph.nodes[node].size
        switch node < nodeState.count ? nodeState[node] : .normal {
        case .selected: return base * 1.55
        case .imports, .importers, .match: return base * 1.15
        case .dim: return base * 0.85
        default: return base
        }
    }

    public func edgeWidth(_ edge: Int) -> Float {
        switch edge < edgeState.count ? edgeState[edge] : .normal {
        case .imports, .importers: return 1.5
        case .cycle: return cycleEdges > 300 ? 0.95 : 1.35
        case .dim: return 0.6
        default: return 0.85
        }
    }

    public func edgePulses(_ graph: SynapseGraph, _ edge: Int) -> Bool {
        if !input.pulses { return false }
        let state = edge < edgeState.count ? edgeState[edge] : .hidden
        if state.highlighted { return true }
        return state == .normal && graph.edges.count <= 1500
    }

    public func edgeColour(_ graph: SynapseGraph, _ edge: Int, _ t: Float) -> SynapseColour {
        let e = graph.edges[edge]
        switch edge < edgeState.count ? edgeState[edge] : .normal {
        case .imports:
            return SynapsePalette.imports.mixed(SynapseColour(bytes: 150, 230, 255), t * 0.5).withAlpha(0.85)
        case .importers:
            return SynapseColour(bytes: 220, 190, 255).mixed(SynapsePalette.importers, t).withAlpha(0.85)
        case .cycle:
            return SynapsePalette.cycle.withAlpha(cycleEdges > 300 ? 0.3 : 0.8)
        case .dim:
            return nodeColour[e.from].mixed(nodeColour[e.to], t).withAlpha(0.05)
        default:
            break
        }
        let drawn = Float(max(1, drawEdges.count))
        let alpha = SynapseMath.clamp(0.5 / max(1, drawn / 120).squareRoot(), 0.08, 0.42)
        let a = baseColour[e.from]
        let b = baseColour[e.to]
        let tone = e.cycle ? a.mixed(b, t).mixed(SynapsePalette.cycle, 0.45) : a.mixed(b, t)
        return tone.mixed(SynapsePalette.white, 0.18).withAlpha(alpha)
    }

    public var animating: Bool {
        input.pulses && (pulseEdges > 0 || cycleNodesVisible)
    }

    public func legend(_ graph: SynapseGraph, _ snapshot: InsightSnapshot) -> [SynapseLegendRow] {
        var rows: [SynapseLegendRow] = []
        switch input.colourBy {
        case .module:
            let lobes = graph.lobes.indices.sorted { a, b in
                let x = graph.lobes[a].nodes.count
                let y = graph.lobes[b].nodes.count
                return x != y ? x > y : a < b
            }
            for l in lobes.prefix(6) {
                rows.append(SynapseLegendRow(colour: SynapsePalette.lobe(l), label: SynapsePalette.shortLobeName(graph.lobes[l].name)))
            }
        case .language:
            var counts: [String: Int] = [:]
            for node in graph.nodes { counts[snapshot.files[node.file].language, default: 0] += 1 }
            let sorted = counts.sorted { x, y in x.value != y.value ? x.value > y.value : U.less(x.key, y.key) }
            for entry in sorted.prefix(6) {
                rows.append(SynapseLegendRow(colour: SynapsePalette.language(entry.key), label: entry.key.isEmpty ? "Other" : entry.key))
            }
        case .health:
            rows.append(SynapseLegendRow(colour: SynapsePalette.health(10), label: "Healthy"))
            rows.append(SynapseLegendRow(colour: SynapsePalette.health(5), label: "Problematic"))
            rows.append(SynapseLegendRow(colour: SynapsePalette.health(1), label: "Unhealthy"))
        case .hotspot:
            rows.append(SynapseLegendRow(colour: SynapsePalette.hotspot(1), label: "Changes often"))
            rows.append(SynapseLegendRow(colour: SynapsePalette.hotspot(0.5), label: "Sometimes"))
            rows.append(SynapseLegendRow(colour: SynapsePalette.hotspot(0), label: "Rarely"))
        }
        if input.selected >= 0 {
            rows.append(SynapseLegendRow(colour: SynapsePalette.imports, label: "Imports"))
            rows.append(SynapseLegendRow(colour: SynapsePalette.importers, label: "Imported by"))
        }
        if !graph.cycles.isEmpty {
            rows.append(SynapseLegendRow(colour: SynapsePalette.cycle, label: "Circular dependency"))
        }
        return rows
    }
}
