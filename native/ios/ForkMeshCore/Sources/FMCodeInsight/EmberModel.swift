import Foundation

public enum EmberTab: Int, Sendable, Equatable, CaseIterable, Identifiable {
    case hotspots, refactoringTargets, codeHealth, changeCoupling, knowledge, defects, languages

    public var id: Int { rawValue }

    public var title: String {
        switch self {
        case .hotspots: return "Hotspots"
        case .refactoringTargets: return "Refactoring targets"
        case .codeHealth: return "Code health"
        case .changeCoupling: return "Change coupling"
        case .knowledge: return "Knowledge"
        case .defects: return "Defects"
        case .languages: return "Languages"
        }
    }

    public var needsHistory: Bool { self != .codeHealth && self != .languages }
}

public struct EmberFilters: Sendable, Equatable {
    public var minHealth: Double = 1
    public var maxHealth: Double = 10
    public var minCommits = 0
    public var author = ""
    public var search = ""

    public init(minHealth: Double = 1, maxHealth: Double = 10, minCommits: Int = 0, author: String = "", search: String = "") {
        self.minHealth = minHealth
        self.maxHealth = maxHealth
        self.minCommits = minCommits
        self.author = author
        self.search = search
    }

    public var active: Bool {
        minHealth > 1 || maxHealth < 10 || minCommits > 0 || !author.isEmpty || !Ember.trimmed(search).isEmpty
    }
}

public struct EmberCouplingRef: Sendable, Equatable {
    public var file: Int
    public var shared: Int
    public var degree: Double

    public init(file: Int, shared: Int, degree: Double) {
        self.file = file
        self.shared = shared
        self.degree = degree
    }
}

public typealias EmberCouplingIndex = [[EmberCouplingRef]]

public struct EmberTarget: Sendable, Equatable {
    public var file: Int
    public var rank: Int
    public var score: Double
    public var reasons: [String]

    public init(file: Int, rank: Int = 0, score: Double, reasons: [String] = []) {
        self.file = file
        self.rank = rank
        self.score = score
        self.reasons = reasons
    }
}

public struct EmberColor: Sendable, Equatable, Hashable {
    public var r: Double
    public var g: Double
    public var b: Double
    public var a: Double

    public init(r: Double, g: Double, b: Double, a: Double = 1) {
        self.r = r
        self.g = g
        self.b = b
        self.a = a
    }

    public init(hex: UInt32) {
        r = Double((hex >> 16) & 0xFF) / 255
        g = Double((hex >> 8) & 0xFF) / 255
        b = Double(hex & 0xFF) / 255
        a = 1
    }

    public var hexText: String {
        func c(_ v: Double) -> Int { Int((max(0, min(1, v)) * 255).rounded()) }
        return String(format: "#%02x%02x%02x", c(r), c(g), c(b))
    }
}

public struct EmberLegendEntry: Sendable, Equatable, Hashable {
    public var label: String
    public var color: EmberColor?
    public var ring: Bool

    public init(label: String, color: EmberColor?, ring: Bool = false) {
        self.label = label
        self.color = color
        self.ring = ring
    }
}

public struct EmberFileStyle: Sendable, Equatable {
    public var color: EmberColor
    public var dimmed = false
    public var glow: Double = 0
    public var rank = 0
    public var marked = false

    public init(color: EmberColor, dimmed: Bool = false, glow: Double = 0, rank: Int = 0, marked: Bool = false) {
        self.color = color
        self.dimmed = dimmed
        self.glow = glow
        self.rank = rank
        self.marked = marked
    }
}

public struct EmberColoring: Sendable, Equatable {
    public var tab: EmberTab = .codeHealth
    public var files: [EmberFileStyle] = []
    public var legend: [EmberLegendEntry] = []
    public var scale = false
    public var scaleLow: EmberColor?
    public var scaleHigh: EmberColor?
    public var scaleLowLabel = ""
    public var scaleHighLabel = ""
    public var note = ""
    public var targets: [EmberTarget] = []
    public var glowing = 0
    public var matching = 0

    public init(tab: EmberTab = .codeHealth) {
        self.tab = tab
    }
}

public struct EmberSummary: Sendable, Equatable {
    public var unhealthy = 0
    public var problematic = 0
    public var healthy = 0
    public var unhealthyLines: Int64 = 0
    public var problematicLines: Int64 = 0
    public var healthyLines: Int64 = 0
    public var topHotspot = -1
    public var text = ""

    public init() {}
}

extension Ember {
    public static let neutral = EmberColor(hex: 0x6e7681)
    public static let couplingLink = EmberColor(hex: 0xf2cc60)
    public static let topHotspotGlow = EmberColor(hex: 0xff7b52)
    public static let hotspotMid = EmberColor(hex: 0xf0883e)
    public static let categoricalPalette: [EmberColor] = [
        EmberColor(hex: 0x3987e5), EmberColor(hex: 0xd95926), EmberColor(hex: 0x199e70), EmberColor(hex: 0xc98500),
        EmberColor(hex: 0xd55181), EmberColor(hex: 0x008300), EmberColor(hex: 0x9085e9), EmberColor(hex: 0xe66767),
    ]

    static func trimmed(_ text: String) -> String {
        text.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    static func containsIgnoringCase(_ text: String, _ query: String) -> Bool {
        query.isEmpty || text.lowercased().contains(query.lowercased())
    }

    static func logShare(_ value: Double, _ maxValue: Double) -> Double {
        if value <= 0 || maxValue <= 0 { return 0 }
        return max(0, min(1, log1p(value) / log1p(maxValue)))
    }

    public static func bandColor(_ band: HealthBand) -> EmberColor {
        switch band {
        case .healthy: return EmberColor(hex: 0x3fb98f)
        case .problematic: return EmberColor(hex: 0xe3b341)
        case .unhealthy: return EmberColor(hex: 0xe5534b)
        }
    }

    public static func healthColor(_ health: Double) -> EmberColor {
        bandColor(Insight.healthBand(health))
    }

    public static func mix(_ a: EmberColor, _ b: EmberColor, _ value: Double) -> EmberColor {
        let t = max(0, min(1, value))
        func channel(_ x: Double, _ y: Double) -> Double {
            let f = Float(x + (y - x) * t)
            return Double((f * 65535).rounded()) / 65535
        }
        return EmberColor(r: channel(a.r, b.r), g: channel(a.g, b.g), b: channel(a.b, b.b), a: channel(a.a, b.a))
    }

    public static func lighten(_ color: EmberColor, _ amount: Double) -> EmberColor {
        mix(color, EmberColor(r: 1, g: 1, b: 1, a: color.a), amount)
    }

    public static func darken(_ color: EmberColor, _ amount: Double) -> EmberColor {
        mix(color, EmberColor(r: 0, g: 0, b: 0, a: color.a), amount)
    }

    public static func fileFill(_ style: EmberFileStyle, background: EmberColor, dark: Bool) -> (fill: EmberColor, light: EmberColor, shade: EmberColor) {
        let fill = style.dimmed ? mix(style.color, background, dark ? 0.74 : 0.7) : style.color
        return (fill, lighten(fill, style.dimmed ? 0.06 : 0.3), darken(fill, style.dimmed ? 0.06 : 0.28))
    }

    public static func bezier(_ a: (x: Double, y: Double), _ c: (x: Double, y: Double), _ b: (x: Double, y: Double), _ t: Double) -> (x: Double, y: Double) {
        let u = 1 - t
        return (a.x * u * u + c.x * 2 * u * t + b.x * t * t, a.y * u * u + c.y * 2 * u * t + b.y * t * t)
    }

    public static func hasHistory(_ snapshot: InsightSnapshot) -> Bool {
        if !snapshot.git { return false }
        if snapshot.commitsScanned > 0 { return true }
        return snapshot.files.contains { $0.commits > 0 }
    }

    public static func matchesFilters(_ file: InsightFile, _ filters: EmberFilters) -> Bool {
        if file.health < filters.minHealth - 1e-9 || file.health > filters.maxHealth + 1e-9 { return false }
        if file.commits < filters.minCommits { return false }
        if !filters.author.isEmpty && file.mainAuthor != filters.author
            && !file.topAuthors.contains(where: { $0.name == filters.author }) {
            return false
        }
        let search = trimmed(filters.search)
        if !search.isEmpty && !containsIgnoringCase(file.path, search) { return false }
        return true
    }

    public static func buildCouplingIndex(_ snapshot: InsightSnapshot) -> EmberCouplingIndex {
        var index = EmberCouplingIndex(repeating: [], count: snapshot.files.count)
        for c in snapshot.couplings {
            if c.a < 0 || c.b < 0 || c.a >= index.count || c.b >= index.count || c.a == c.b { continue }
            index[c.a].append(EmberCouplingRef(file: c.b, shared: c.shared, degree: c.degree))
            index[c.b].append(EmberCouplingRef(file: c.a, shared: c.shared, degree: c.degree))
        }
        for i in index.indices where index[i].count > 1 {
            index[i].sort { a, b in
                if a.degree != b.degree { return a.degree > b.degree }
                if a.shared != b.shared { return a.shared > b.shared }
                return a.file < b.file
            }
        }
        return index
    }

    public static func targetScore(_ file: InsightFile) -> Double {
        max(0, file.hotspot) * max(0, 10 - file.health)
    }

    public static func refactoringTargets(_ snapshot: InsightSnapshot, _ coupling: EmberCouplingIndex, limit: Int = 15) -> [EmberTarget] {
        var targets: [EmberTarget] = []
        for (i, file) in snapshot.files.enumerated() {
            let s = targetScore(file)
            if s <= 0.05 || file.commits <= 0 { continue }
            targets.append(EmberTarget(file: i, score: s))
        }
        targets.sort { a, b in
            if a.score != b.score { return a.score > b.score }
            return U.less(snapshot.files[a.file].path, snapshot.files[b.file].path)
        }
        if limit > 0 && targets.count > limit { targets = Array(targets.prefix(limit)) }
        for r in targets.indices {
            let file = snapshot.files[targets[r].file]
            var reasons: [String] = []
            reasons.append("Changed in \(number(file.commits)) commits (\(number(file.recentCommits)) in the last 90 days)")
            reasons.append("Code health \(decimal(file.health)), \(Insight.healthBand(file.health).name.lowercased())")
            reasons.append(contentsOf: file.healthIssues.prefix(2))
            if file.fixCommits > 0 { reasons.append("\(number(file.fixCommits)) bug-fix commits") }
            let t = targets[r].file
            if t < coupling.count && !coupling[t].isEmpty {
                reasons.append("Changes together with \(number(coupling[t].count)) other files")
            }
            if file.formerAuthorShare >= 0.5 {
                reasons.append("\(percentText(file.formerAuthorShare)) written by former contributors")
            }
            targets[r].rank = r + 1
            targets[r].reasons = reasons
        }
        return targets
    }

    static func rankedNames(_ lines: [String: Int64], limit: Int) -> [String] {
        let names = lines.keys.sorted { a, b in
            let la = lines[a] ?? 0
            let lb = lines[b] ?? 0
            return la != lb ? la > lb : U.less(a, b)
        }
        return limit > 0 ? Array(names.prefix(limit)) : names
    }

    public static func topAuthors(_ snapshot: InsightSnapshot, limit: Int) -> [String] {
        var lines: [String: Int64] = [:]
        for file in snapshot.files where !file.mainAuthor.isEmpty {
            lines[file.mainAuthor, default: 0] += Int64(max(1, file.codeLines))
        }
        return rankedNames(lines, limit: limit)
    }

    public static func topLanguages(_ snapshot: InsightSnapshot, limit: Int) -> [String] {
        var lines: [String: Int64] = [:]
        for file in snapshot.files where !file.language.isEmpty {
            lines[file.language, default: 0] += Int64(max(1, file.codeLines))
        }
        return rankedNames(lines, limit: limit)
    }

    public static func colorize(_ snapshot: InsightSnapshot, tab: EmberTab, filters: EmberFilters = EmberFilters(),
                                coupling: EmberCouplingIndex) -> EmberColoring {
        var out = EmberColoring(tab: tab)
        let n = snapshot.files.count
        out.files = snapshot.files.map { EmberFileStyle(color: neutral, dimmed: !matchesFilters($0, filters)) }
        if tab.needsHistory && !hasHistory(snapshot) {
            out.note = "\(tab.title) need git history. This folder has none, so only Code health and Languages can be shown."
            for i in out.files.indices { out.files[i].dimmed = true }
            return out
        }

        switch tab {
        case .hotspots:
            let maxCommits = snapshot.files.reduce(0) { max($0, $1.commits) }
            let grey = EmberColor(hex: 0x6e7681)
            let orange = hotspotMid
            let red = EmberColor(hex: 0xda3633)
            for i in 0..<n {
                let t = pow(logShare(Double(snapshot.files[i].commits), Double(maxCommits)), 1.6)
                if snapshot.files[i].commits <= 0 {
                    out.files[i].color = neutral
                } else {
                    out.files[i].color = t < 0.5 ? mix(grey, orange, t / 0.5) : mix(orange, red, (t - 0.5) / 0.5)
                }
            }
            for index in hotspotOrder(snapshot).prefix(12) {
                let file = snapshot.files[index]
                if file.hotspot < 0.35 { break }
                out.files[index].glow = file.hotspot
                out.glowing += 1
            }
            out.scale = true
            out.scaleLow = grey
            out.scaleHigh = red
            out.scaleLowLabel = "Rarely changed"
            out.scaleHighLabel = "Changed often"
            out.legend.append(EmberLegendEntry(label: "Top hotspot", color: topHotspotGlow, ring: true))
        case .refactoringTargets:
            out.targets = refactoringTargets(snapshot, coupling, limit: 15)
            var rank = [Int](repeating: 0, count: n)
            for t in out.targets { rank[t.file] = t.rank }
            for i in 0..<n {
                out.files[i].color = healthColor(snapshot.files[i].health)
                out.files[i].rank = rank[i]
                if rank[i] == 0 { out.files[i].dimmed = true }
            }
            out.legend.append(EmberLegendEntry(label: "Unhealthy target", color: bandColor(.unhealthy)))
            out.legend.append(EmberLegendEntry(label: "Problematic target", color: bandColor(.problematic)))
            if out.targets.isEmpty { out.note = "No refactoring targets: every frequently changed file is healthy." }
        case .codeHealth:
            for i in 0..<n { out.files[i].color = healthColor(snapshot.files[i].health) }
            out.legend.append(EmberLegendEntry(label: "Healthy", color: bandColor(.healthy)))
            out.legend.append(EmberLegendEntry(label: "Problematic", color: bandColor(.problematic)))
            out.legend.append(EmberLegendEntry(label: "Unhealthy", color: bandColor(.unhealthy)))
        case .changeCoupling:
            let weak = EmberColor(hex: 0x7d6a3a)
            let strong = couplingLink
            var coupled = 0
            for i in 0..<n {
                guard i < coupling.count, let first = coupling[i].first else {
                    out.files[i].color = neutral
                    continue
                }
                coupled += 1
                out.files[i].color = mix(weak, strong, max(0, min(1, (first.degree - 0.2) / 0.8)))
            }
            out.scale = true
            out.scaleLow = weak
            out.scaleHigh = strong
            out.scaleLowLabel = "Weak"
            out.scaleHighLabel = "Strong coupling"
            if coupled == 0 { out.note = "No files change together often enough to count as coupled." }
        case .knowledge:
            let authors = topAuthors(snapshot, limit: categoricalPalette.count)
            var slot: [String: Int] = [:]
            for (a, name) in authors.enumerated() { slot[name] = a }
            let former = Set(snapshot.formerAuthors)
            let quiet = EmberColor(hex: 0x3d444d)
            let anyQuiet = snapshot.files.contains { $0.mainAuthor.isEmpty }
            for i in 0..<n {
                let file = snapshot.files[i]
                if let s = slot[file.mainAuthor] {
                    out.files[i].color = categoricalPalette[s]
                } else {
                    out.files[i].color = file.mainAuthor.isEmpty ? quiet : neutral
                }
                out.files[i].marked = file.formerAuthorShare >= 0.5
            }
            for (a, name) in authors.enumerated() {
                out.legend.append(EmberLegendEntry(label: former.contains(name) ? name + " (former)" : name, color: categoricalPalette[a]))
            }
            if topAuthors(snapshot, limit: 0).count > authors.count {
                out.legend.append(EmberLegendEntry(label: "Other authors", color: neutral))
            }
            if anyQuiet { out.legend.append(EmberLegendEntry(label: "No commits in the window", color: quiet)) }
            out.legend.append(EmberLegendEntry(label: "Mostly former contributors", color: nil, ring: true))
        case .defects:
            let maxFixes = snapshot.files.reduce(0) { max($0, $1.fixCommits) }
            let low = EmberColor(hex: 0xe3a8a2)
            let high = EmberColor(hex: 0xd1242f)
            for i in 0..<n {
                let fixes = snapshot.files[i].fixCommits
                out.files[i].color = fixes > 0 ? mix(low, high, logShare(Double(fixes), Double(maxFixes))) : neutral
            }
            out.scale = true
            out.scaleLow = low
            out.scaleHigh = high
            out.scaleLowLabel = "Few fixes"
            out.scaleHighLabel = "Many fixes"
            if maxFixes == 0 { out.note = "No bug-fix commits found in the history window." }
        case .languages:
            let languages = topLanguages(snapshot, limit: categoricalPalette.count)
            var slot: [String: Int] = [:]
            for (l, name) in languages.enumerated() { slot[name] = l }
            for i in 0..<n {
                if let s = slot[snapshot.files[i].language] { out.files[i].color = categoricalPalette[s] }
            }
            for (l, name) in languages.enumerated() {
                out.legend.append(EmberLegendEntry(label: name, color: categoricalPalette[l]))
            }
            if topLanguages(snapshot, limit: 0).count > languages.count {
                out.legend.append(EmberLegendEntry(label: "Other", color: neutral))
            }
        }
        out.matching = out.files.reduce(0) { $0 + ($1.dimmed ? 0 : 1) }
        return out
    }

    public static func hotspotOrder(_ snapshot: InsightSnapshot) -> [Int] {
        snapshot.files.indices.filter { snapshot.files[$0].hotspot > 0 }.sorted { a, b in
            let ha = snapshot.files[a].hotspot
            let hb = snapshot.files[b].hotspot
            return ha != hb ? ha > hb : U.less(snapshot.files[a].path, snapshot.files[b].path)
        }
    }

    public static func summarize(_ snapshot: InsightSnapshot) -> EmberSummary {
        var s = EmberSummary()
        var best = 0.0
        for (i, file) in snapshot.files.enumerated() {
            switch Insight.healthBand(file.health) {
            case .healthy:
                s.healthy += 1
                s.healthyLines += Int64(file.codeLines)
            case .problematic:
                s.problematic += 1
                s.problematicLines += Int64(file.codeLines)
            case .unhealthy:
                s.unhealthy += 1
                s.unhealthyLines += Int64(file.codeLines)
            }
            if file.hotspot > best {
                best = file.hotspot
                s.topHotspot = i
            }
        }
        var parts = ["\(number(s.unhealthy)) unhealthy", "\(number(s.problematic)) problematic"]
        if s.topHotspot >= 0 { parts.append("top hotspot \(snapshot.files[s.topHotspot].path)") }
        s.text = parts.joined(separator: " · ")
        return s
    }

    public static func number(_ value: Int) -> String { U.grouped(value) }

    public static func number(_ value: Int64) -> String { U.grouped(Int(value)) }

    public static func decimal(_ value: Double, digits: Int = 1) -> String {
        guard value.isFinite else { return String(value) }
        let exact = String(format: "%.\(digits + 30)f", abs(value))
        let parts = exact.split(separator: ".", maxSplits: 1, omittingEmptySubsequences: false)
        let fraction = parts.count > 1 ? Array(parts[1].utf8) : []
        var kept = Array(parts[0].utf8) + fraction.prefix(digits)
        if digits < fraction.count && fraction[digits] >= UInt8(ascii: "5") {
            var i = kept.count - 1
            while i >= 0 {
                if kept[i] == UInt8(ascii: "9") {
                    kept[i] = UInt8(ascii: "0")
                    i -= 1
                } else {
                    kept[i] += 1
                    break
                }
            }
            if i < 0 { kept.insert(UInt8(ascii: "1"), at: 0) }
        }
        let wholeCount = kept.count - digits
        let whole = Int(String(decoding: kept.prefix(wholeCount), as: UTF8.self)) ?? 0
        var out = U.grouped(whole)
        if digits > 0 { out += "." + String(decoding: kept.suffix(digits), as: UTF8.self) }
        return value < 0 ? "-" + out : out
    }

    public static func fileNameOf(_ path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return path }
        return String(path[path.index(after: slash)...])
    }

    public static func folderOf(_ path: String) -> String {
        guard let slash = path.lastIndex(of: "/") else { return "" }
        return String(path[..<slash])
    }

    public static func spanText(_ days: Int) -> String {
        if days >= 330 {
            let years = max(1, Int((Double(days) / 365.0).rounded()))
            return years == 1 ? "1 year" : "\(years) years"
        }
        if days >= 45 {
            let months = max(1, Int((Double(days) / 30.4).rounded()))
            return "\(months) months"
        }
        return days == 1 ? "1 day" : "\(max(0, days)) days"
    }

    public static func agoText(_ when: Int64, now: Int64) -> String {
        if when <= 0 { return "never" }
        let days = max(Int64(0), (now - when) / 86400)
        if days == 0 { return "today" }
        if days == 1 { return "yesterday" }
        if days < 14 { return "\(days) days ago" }
        if days < 60 { return "\(days / 7) weeks ago" }
        if days < 365 { return "\(max(Int64(2), Int64((Double(days) / 30.4).rounded()))) months ago" }
        let years = Int64((Double(days) / 365.0).rounded())
        return years <= 1 ? "1 year ago" : "\(years) years ago"
    }

    public static func percentText(_ share: Double) -> String {
        "\(Int((max(0, min(1, share)) * 100).rounded())) %"
    }
}
