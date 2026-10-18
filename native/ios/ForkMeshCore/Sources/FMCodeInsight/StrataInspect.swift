import Foundation

public struct StrataRGB: Sendable, Equatable {
    public var red: Double
    public var green: Double
    public var blue: Double

    public init(red: Double, green: Double, blue: Double) {
        self.red = red
        self.green = green
        self.blue = blue
    }

    public init(hex: UInt32) {
        red = Double((hex >> 16) & 0xFF) / 255.0
        green = Double((hex >> 8) & 0xFF) / 255.0
        blue = Double(hex & 0xFF) / 255.0
    }

    public func mixed(_ other: StrataRGB, _ t: Double) -> StrataRGB {
        let k = max(0, min(1, t))
        return StrataRGB(red: red + (other.red - red) * k, green: green + (other.green - green) * k, blue: blue + (other.blue - blue) * k)
    }

    public var luminance: Double { 0.2126 * red + 0.7152 * green + 0.0722 * blue }

    public var prefersDarkText: Bool { luminance > 0.52 }
}

public enum StrataPalette {
    public static let healthy = StrataRGB(hex: 0x3FB98F)
    public static let problematic = StrataRGB(hex: 0xE3B341)
    public static let unhealthy = StrataRGB(hex: 0xE5534B)
    public static let ratings: [StrataRGB] = [StrataRGB(hex: 0x3FB98F), StrataRGB(hex: 0x8CC265), StrataRGB(hex: 0xE3B341), StrataRGB(hex: 0xF0883E), StrataRGB(hex: 0xE5534B)]
    public static let severities: [StrataRGB] = [StrataRGB(hex: 0xE5534B), StrataRGB(hex: 0xF0883E), StrataRGB(hex: 0xE3B341), StrataRGB(hex: 0x8B949E)]

    public static func scale(_ t: Double) -> StrataRGB {
        let k = max(0, min(1, t))
        return k < 0.5 ? healthy.mixed(problematic, k * 2.0) : problematic.mixed(unhealthy, (k - 0.5) * 2.0)
    }

    public static func health(_ health: Double) -> StrataRGB {
        scale((10.0 - max(1.0, min(10.0, health))) / 9.0)
    }

    public static func rating(_ rating: Int) -> StrataRGB { ratings[max(0, min(4, rating))] }

    public static func severity(_ severity: StrataSeverity) -> StrataRGB { severities[severity.rawValue] }

    public static func gate(_ status: StrataGateStatus) -> StrataRGB {
        status == .fail ? unhealthy : status == .warn ? problematic : healthy
    }

    public static func instability(_ instability: Double) -> StrataRGB {
        let stable = StrataRGB(hex: 0x4C8EDA)
        let middle = StrataRGB(hex: 0x9A7CE0)
        let volatile = StrataRGB(hex: 0xF09A4A)
        let t = max(0, min(1, instability))
        return t < 0.5 ? stable.mixed(middle, t * 2.0) : middle.mixed(volatile, (t - 0.5) * 2.0)
    }

    public static func change(_ t: Double, dark: Bool) -> StrataRGB {
        let warm = StrataRGB(hex: 0xD29922)
        let hot = StrataRGB(hex: 0xF85149)
        let cold = dark ? StrataRGB(hex: 0x2A3A4E) : StrataRGB(hex: 0xC8D3E0)
        return t < 0.5 ? cold.mixed(warm, t * 2.0) : warm.mixed(hot, (t - 0.5) * 2.0)
    }
}

public enum StrataGraphColor: Int, Sendable, CaseIterable {
    case rating = 0, health, instability, complexity

    public var label: String {
        switch self {
        case .rating: return "Debt rating"
        case .health: return "Health"
        case .instability: return "Instability"
        case .complexity: return "Complexity"
        }
    }
}

public enum StrataTreemapColor: Int, Sendable, CaseIterable {
    case health = 0, maxComplexity, debt, changeFrequency

    public var label: String {
        switch self {
        case .health: return "Health"
        case .maxComplexity: return "Max complexity"
        case .debt: return "Debt ratio"
        case .changeFrequency: return "Change frequency"
        }
    }
}

public struct StrataFact: Sendable, Equatable, Identifiable {
    public var label: String
    public var value: String
    public var id: String { label }

    public init(_ label: String, _ value: String) {
        self.label = label
        self.value = value
    }
}

public struct StrataTile: Sendable, Equatable, Identifiable {
    public var label: String
    public var value: String
    public var note: String
    public var id: String { label }

    public init(_ label: String, _ value: String, _ note: String) {
        self.label = label
        self.value = value
        self.note = note
    }
}

public struct StrataNeighbour: Sendable, Equatable, Identifiable {
    public var module: Int
    public var weight: Int
    public var cycle: Bool
    public var id: Int { module }

    public init(module: Int, weight: Int, cycle: Bool) {
        self.module = module
        self.weight = weight
        self.cycle = cycle
    }
}

public struct StrataRuleRow: Sendable, Equatable, Identifiable {
    public var rule: Int
    public var matching: Int
    public var debt: Double
    public var all: Bool
    public var id: Int { rule }

    public init(rule: Int, matching: Int, debt: Double, all: Bool) {
        self.rule = rule
        self.matching = matching
        self.debt = debt
        self.all = all
    }
}

public struct StrataRuleGroup: Sendable, Equatable, Identifiable {
    public var category: StrataCategory
    public var rows: [StrataRuleRow]
    public var count: Int
    public var debt: Double
    public var id: Int { category.rawValue }

    public init(category: StrataCategory, rows: [StrataRuleRow], count: Int, debt: Double) {
        self.category = category
        self.rows = rows
        self.count = count
        self.debt = debt
    }
}

public struct StrataFileEdgeRow: Sendable, Equatable, Identifiable {
    public var from: Int
    public var to: Int
    public var id: Int { from &* 1_000_003 &+ to }

    public init(from: Int, to: Int) {
        self.from = from
        self.to = to
    }
}

public struct StrataCellDirection: Sendable, Equatable {
    public var from: Int
    public var to: Int
    public var weight: Int
    public var exists: Bool
    public var edges: [StrataFileEdgeRow]
    public var hidden: Int

    public init(from: Int, to: Int, weight: Int, exists: Bool, edges: [StrataFileEdgeRow], hidden: Int) {
        self.from = from
        self.to = to
        self.weight = weight
        self.exists = exists
        self.edges = edges
        self.hidden = hidden
    }
}

extension Strata {
    public static func matches(_ text: String, _ filter: String) -> Bool {
        filter.isEmpty || text.range(of: filter, options: .caseInsensitive) != nil
    }

    public static func moduleComplexityT(_ node: InsightModuleNode) -> Double {
        let average = node.functions > 0 ? Double(node.complexity) / Double(node.functions) : 1.0
        return clamp((average - 1.5) / 8.0, 0, 1)
    }

    public static func moduleColor(_ model: StrataModel, _ module: Int, by: StrataGraphColor) -> StrataRGB {
        let report = model.report
        let node = report.graph.nodes[module]
        switch by {
        case .rating: return StrataPalette.rating(module < report.modules.count ? report.modules[module].rating : 0)
        case .health: return StrataPalette.health(node.health)
        case .instability: return StrataPalette.instability(node.instability)
        case .complexity: return StrataPalette.scale(moduleComplexityT(node))
        }
    }

    public static func fileColor(_ model: StrataModel, _ file: Int, by: StrataGraphColor) -> StrataRGB {
        let info = model.snapshot.files[file]
        switch by {
        case .rating: return StrataPalette.rating(file < model.report.files.count ? model.report.files[file].rating : 0)
        case .health: return StrataPalette.health(info.health)
        case .instability:
            let sum = info.dependsOn.count + info.dependedOnBy.count
            return StrataPalette.instability(sum > 0 ? Double(info.dependsOn.count) / Double(sum) : 0)
        case .complexity: return StrataPalette.scale(clamp(Double(info.maxComplexity - 4) / 22.0, 0, 1))
        }
    }

    public static func maxCommits(_ snapshot: InsightSnapshot) -> Double {
        Double(max(1, snapshot.files.map(\.commits).max() ?? 1))
    }

    public static func treemapColorValue(_ model: StrataModel, _ file: Int, by: StrataTreemapColor, maxCommits: Double) -> Double {
        let info = model.snapshot.files[file]
        switch by {
        case .health: return (10.0 - max(1.0, min(10.0, info.health))) / 9.0
        case .maxComplexity: return clamp(Double(info.maxComplexity - 4) / 26.0, 0, 1)
        case .debt: return file < model.report.files.count ? clamp(model.report.files[file].ratio / 0.5, 0, 1) : 0
        case .changeFrequency: return (Double(info.commits) / max(1, maxCommits)).squareRoot()
        }
    }

    public static func treemapColor(_ model: StrataModel, _ file: Int, by: StrataTreemapColor, maxCommits: Double, dark: Bool) -> StrataRGB {
        let t = treemapColorValue(model, file, by: by, maxCommits: maxCommits)
        return by == .changeFrequency ? StrataPalette.change(t, dark: dark) : StrataPalette.scale(t)
    }

    public static func violationMatches(_ model: StrataModel, _ v: StrataViolation, text: String, severity: StrataSeverity? = nil) -> Bool {
        if let severity, v.severity != severity { return false }
        if text.isEmpty { return true }
        if matches(v.message, text) { return true }
        if v.file >= 0 && v.file < model.snapshot.files.count && matches(model.snapshot.files[v.file].path, text) { return true }
        if v.module >= 0 && v.module < model.report.graph.nodes.count && matches(model.report.graph.nodes[v.module].path, text) { return true }
        return false
    }

    public static func ruleGroups(_ model: StrataModel, text: String, severity: StrataSeverity? = nil) -> [StrataRuleGroup] {
        let report = model.report
        var groups: [StrataRuleGroup] = []
        for category in StrataCategory.allCases {
            var rows: [StrataRuleRow] = []
            var count = 0
            var debt = 0.0
            for (r, rule) in report.rules.enumerated() where rule.category == category {
                let ruleMatch = text.isEmpty || matches(rule.title, text) || matches(rule.id, text)
                var matching = rule.violations.count
                var matchingDebt = rule.debt
                if !ruleMatch || severity != nil {
                    matching = 0
                    matchingDebt = 0
                    for v in rule.violations where violationMatches(model, v, text: ruleMatch ? "" : text, severity: severity) {
                        matching += 1
                        matchingDebt += v.debt
                    }
                    if matching == 0 { continue }
                }
                rows.append(StrataRuleRow(rule: r, matching: matching, debt: matchingDebt, all: ruleMatch))
                count += matching
                debt += matchingDebt
            }
            if !rows.isEmpty { groups.append(StrataRuleGroup(category: category, rows: rows, count: count, debt: debt)) }
        }
        return groups
    }

    public static func ruleViolations(_ model: StrataModel, rule: Int, text: String, severity: StrataSeverity? = nil, all: Bool, limit: Int = 500) -> (indexes: [Int], hidden: Int) {
        var indexes: [Int] = []
        var hidden = 0
        for (i, v) in model.report.rules[rule].violations.enumerated() {
            if !violationMatches(model, v, text: all ? "" : text, severity: severity) { continue }
            if indexes.count >= limit {
                hidden += 1
                continue
            }
            indexes.append(i)
        }
        return (indexes, hidden)
    }

    public static func violationWhere(_ model: StrataModel, rule: Int, _ v: StrataViolation) -> String {
        let moduleScope = model.report.rules[rule].moduleScope
        if moduleScope && v.module >= 0 && v.module < model.report.graph.nodes.count { return model.report.graph.nodes[v.module].path }
        if v.file >= 0 && v.file < model.snapshot.files.count {
            return model.snapshot.files[v.file].path + (v.line > 0 ? ":\(v.line)" : "")
        }
        return ""
    }

    public static func violationTarget(_ model: StrataModel, rule: Int, _ v: StrataViolation) -> (path: String, line: Int)? {
        guard v.file >= 0 && v.file < model.snapshot.files.count else { return nil }
        return (model.snapshot.files[v.file].path, model.report.rules[rule].moduleScope ? 0 : v.line)
    }

    public static func compact(_ value: Int) -> String {
        if value >= 1_000_000 { return localeFixed(Double(value) / 1_000_000.0, 2) + "M" }
        if value >= 100_000 { return localeFixed(Double(value) / 1000.0, 0) + "k" }
        return grouped(value)
    }

    public static func dashboardTiles(_ model: StrataModel) -> [StrataTile] {
        let report = model.report
        let m = report.metrics
        let inCycles = report.graph.cycles.reduce(0) { $0 + $1.count }
        let rulesHit = report.rules.filter { !$0.violations.isEmpty }.count
        return [
            StrataTile("Lines of code", compact(m.codeLines), m.codeLines >= 100_000 ? grouped(m.codeLines) : ""),
            StrataTile("Files", grouped(m.files), m.testFiles > 0 ? "\(grouped(m.testFiles)) tests" : ""),
            StrataTile("Modules", grouped(m.modules), report.graph.cycles.isEmpty ? "no cycles" : "\(inCycles) in cycles"),
            StrataTile("Types", compact(m.types), ""),
            StrataTile("Functions", compact(m.functions), ""),
            StrataTile("Average complexity", localeFixed(m.averageComplexity, 1), "per function"),
            StrataTile("Max complexity", grouped(m.maxComplexity), m.maxComplexityFunction),
            StrataTile("Comments", "\(localeFixed(m.commentPercent, 1)) %", ""),
            StrataTile("Duplicated", "\(localeFixed(m.duplicatedPercent, 1)) %", ""),
            StrataTile("Rule violations", grouped(report.total.violations), "\(rulesHit) rules"),
        ]
    }

    public static func gatesHeadline(_ report: StrataReport) -> (text: String, status: StrataGateStatus) {
        let failing = report.gatesWith(.fail)
        let warning = report.gatesWith(.warn)
        if failing > 0 { return ("\(failing) failing", .fail) }
        if warning > 0 { return ("\(warning) warning", .warn) }
        return ("All pass", .pass)
    }

    public static func topRules(_ report: StrataReport, limit: Int = 6) -> [Int] {
        let hit = report.rules.indices.filter { !report.rules[$0].violations.isEmpty }
        return Array(stableSorted(hit) { report.rules[$0].debt > report.rules[$1].debt }.prefix(limit))
    }

    public static func topModules(_ report: StrataReport, limit: Int = 5) -> [Int] {
        let indebted = report.modules.indices.filter { report.modules[$0].debt > 0 }
        return Array(stableSorted(indebted) { report.modules[$0].debt > report.modules[$1].debt }.prefix(limit))
    }

    public static func topFiles(_ report: StrataReport, limit: Int = 10) -> [Int] {
        let indebted = report.files.indices.filter { report.files[$0].debt > 0 }
        return Array(stableSorted(indebted) { report.files[$0].debt > report.files[$1].debt }.prefix(limit))
    }

    public static func moduleFacts(_ model: StrataModel, _ module: Int) -> [StrataFact] {
        let report = model.report
        let node = report.graph.nodes[module]
        let score = report.modules[module]
        var facts = [
            StrataFact("Lines of code", grouped(node.codeLines)),
            StrataFact("Files", grouped(node.files.count)),
            StrataFact("Functions", grouped(node.functions)),
            StrataFact("Complexity", grouped(node.complexity)),
            StrataFact("Types", "\(node.types) (\(node.abstractTypes) abstract)"),
            StrataFact("Health", fixed(node.health, 1)),
            StrataFact("Afferent Ca", "\(node.afferent)"),
            StrataFact("Efferent Ce", "\(node.efferent)"),
            StrataFact("Instability I", fixed(node.instability, 2)),
            StrataFact("Abstractness A", fixed(node.abstractness, 2)),
            StrataFact("Distance D", fixed(node.distance, 2)),
            StrataFact("Cohesion", fixed(node.cohesion, 2)),
        ]
        if node.commits > 0 { facts.append(StrataFact("Commits", grouped(node.commits))) }
        if report.history { facts.append(StrataFact("Interest", formatDebt(score.interest) + " a year")) }
        return facts
    }

    public static func moduleCycleNote(_ model: StrataModel, _ module: Int) -> String? {
        let graph = model.report.graph
        let node = graph.nodes[module]
        guard node.cycle >= 0 else { return nil }
        let size = node.cycle < graph.cycles.count ? graph.cycles[node.cycle].count : 0
        return "In a dependency cycle with \(size - 1) other modules."
    }

    public static func moduleNeighbours(_ model: StrataModel, _ module: Int) -> (uses: [StrataNeighbour], usedBy: [StrataNeighbour]) {
        let graph = model.report.graph
        let node = graph.nodes[module]
        var uses: [StrataNeighbour] = []
        var usedBy: [StrataNeighbour] = []
        func inCycle(_ other: Int) -> Bool { graph.nodes[other].cycle >= 0 && graph.nodes[other].cycle == node.cycle }
        for edge in graph.edges {
            if edge.from == module && edge.to != module {
                uses.append(StrataNeighbour(module: edge.to, weight: edge.weight, cycle: inCycle(edge.to)))
            } else if edge.to == module && edge.from != module {
                usedBy.append(StrataNeighbour(module: edge.from, weight: edge.weight, cycle: inCycle(edge.from)))
            }
        }
        return (stableSorted(uses) { $0.weight > $1.weight }, stableSorted(usedBy) { $0.weight > $1.weight })
    }

    public static func moduleFilesByDebt(_ model: StrataModel, _ module: Int, limit: Int = 8) -> [Int] {
        let report = model.report
        let ranked = stableSorted(report.graph.nodes[module].files) { report.files[$0].debt > report.files[$1].debt }
        var out: [Int] = []
        for f in ranked {
            if out.count >= limit || report.files[f].debt <= 0 { break }
            out.append(f)
        }
        return out
    }

    public static func fileFacts(_ model: StrataModel, _ file: Int) -> [StrataFact] {
        let info = model.snapshot.files[file]
        let score = model.report.files[file]
        var facts = [
            StrataFact("Language", info.language),
            StrataFact("Lines of code", "\(grouped(info.codeLines)) (\(grouped(info.commentLines)) comment lines)"),
            StrataFact("Functions", grouped(info.functions.count)),
            StrataFact("Complexity", "\(info.complexity) · max \(info.maxComplexity)"),
            StrataFact("Max nesting", "\(info.maxNesting)"),
            StrataFact("Health", fixed(info.health, 1)),
            StrataFact("Imports", "\(info.dependsOn.count) files · \(info.externalImports.count) packages"),
            StrataFact("Imported by", "\(info.dependedOnBy.count) files"),
        ]
        if info.duplicatedLines > 0 { facts.append(StrataFact("Duplicated", "\(info.duplicatedLines) lines")) }
        if info.commits > 0 {
            facts.append(StrataFact("Commits", "\(info.commits) (\(info.recentCommits) recent, \(info.fixCommits) fixes)"))
            facts.append(StrataFact("Authors", "\(info.authors) · main \(info.mainAuthor) (\(formatPercent(info.mainAuthorShare)))"))
            facts.append(StrataFact("Interest", formatDebt(score.interest) + " a year"))
        }
        return facts
    }

    public static func fileViolations(_ model: StrataModel, _ file: Int) -> [StrataViolationRef] {
        guard file >= 0 && file < model.report.fileViolations.count else { return [] }
        return model.report.sortedBySeverity(model.report.fileViolations[file])
    }

    public static func matrixWeights(_ graph: InsightModuleGraph) -> (weights: [Int: Int], maxWeight: Int) {
        let n = graph.nodes.count
        var weights: [Int: Int] = [:]
        var maxWeight = 1
        for edge in graph.edges where edge.from >= 0 && edge.to >= 0 {
            let key = edge.from * n + edge.to
            weights[key, default: 0] += max(1, edge.weight)
            maxWeight = max(maxWeight, weights[key] ?? 0)
        }
        return (weights, maxWeight)
    }

    public static func matrixLayerBreaks(_ model: StrataModel) -> [Int] {
        let layers = moduleLayers(model.report.graph)
        let order = model.order
        guard order.count > 1 else { return [] }
        return (1..<order.count).filter { layers[order[$0]] != layers[order[$0 - 1]] }
    }

    public static func cellDirection(_ model: StrataModel, from: Int, to: Int, limit: Int = 60) -> StrataCellDirection {
        let graph = model.report.graph
        let index = graph.edgeIndex(from, to)
        guard index >= 0 else { return StrataCellDirection(from: from, to: to, weight: 0, exists: false, edges: [], hidden: 0) }
        let edge = graph.edges[index]
        var rows: [StrataFileEdgeRow] = []
        let fileCount = model.snapshot.files.count
        for fileEdge in edge.fileEdges.prefix(limit) where fileEdge.from >= 0 && fileEdge.to >= 0 && fileEdge.from < fileCount && fileEdge.to < fileCount {
            rows.append(StrataFileEdgeRow(from: fileEdge.from, to: fileEdge.to))
        }
        return StrataCellDirection(from: from, to: to, weight: edge.weight, exists: true, edges: rows, hidden: max(0, edge.fileEdges.count - limit))
    }

    public static func moduleMatches(_ model: StrataModel, _ module: Int, _ filter: String) -> Bool {
        if filter.isEmpty { return true }
        let node = model.report.graph.nodes[module]
        if matches(node.path, filter) || matches(node.name, filter) { return true }
        return node.files.contains { matches(model.snapshot.files[$0].path, filter) }
    }

    public static func moduleCaption(_ node: InsightModuleNode) -> String {
        "\(grouped(node.codeLines)) LOC · \(node.files.count) \(node.files.count == 1 ? "file" : "files")"
    }

    public static func drillCaption(_ model: StrataModel, _ node: StrataDrillNode) -> String {
        if node.module {
            return node.dependent ? "uses these files · \(node.weight)" : "used · \(node.weight)"
        }
        let file = model.snapshot.files[node.index]
        return "\(grouped(file.codeLines)) LOC · max cc \(file.maxComplexity)"
    }

    public static func edgeWidth(_ weight: Int) -> Double {
        clamp(1.0 + 1.25 * log2(1.0 + Double(weight)), 1.0, 7.0)
    }

    public static func scatterLabels(_ model: StrataModel, selected: Int) -> [Int] {
        let nodes = model.report.graph.nodes
        let drawOrder = stableSorted(Array(nodes.indices)) { nodes[$0].codeLines > nodes[$1].codeLines }
        var labelled = Array(drawOrder.prefix(10))
        for m in drawOrder where nodes[m].distance > 0.7 && labelled.count < 18 && !labelled.contains(m) {
            labelled.append(m)
        }
        if selected >= 0 && !labelled.contains(selected) { labelled.insert(selected, at: 0) }
        return labelled
    }

    public static func searchHits(_ model: StrataModel, _ text: String, limit: Int = 40) -> (modules: [Int], files: [Int]) {
        guard !text.isEmpty else { return ([], []) }
        let modules = model.report.graph.nodes.indices.filter { matches(model.report.graph.nodes[$0].path, text) || matches(model.report.graph.nodes[$0].name, text) }
        let files = model.snapshot.files.indices.filter { matches(model.snapshot.files[$0].path, text) }
        return (Array(modules.prefix(limit)), Array(files.prefix(limit)))
    }
}
