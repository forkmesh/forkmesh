import Foundation

public struct EmberRow: Sendable, Equatable, Identifiable {
    public var id: Int
    public var title: String
    public var detail: String
    public var value: String
    public var fraction: Double
    public var color: EmberColor
    public var rank: Int
    public var notes: [String]

    public init(id: Int, title: String, detail: String, value: String, fraction: Double, color: EmberColor, rank: Int = 0, notes: [String] = []) {
        self.id = id
        self.title = title
        self.detail = detail
        self.value = value
        self.fraction = fraction
        self.color = color
        self.rank = rank
        self.notes = notes
    }
}

public struct EmberMetric: Sendable, Equatable, Identifiable {
    public var label: String
    public var value: String
    public var line: Int

    public init(label: String, value: String, line: Int = 0) {
        self.label = label
        self.value = value
        self.line = line
    }

    public var id: String { label }
}

public struct EmberCard: Sendable, Equatable {
    public var file: Int
    public var folder: String
    public var name: String
    public var health: Double
    public var band: HealthBand
    public var note: String
    public var metrics: [EmberMetric]
    public var issues: [String]
}

public struct EmberAuthorOption: Sendable, Equatable, Hashable, Identifiable {
    public var label: String
    public var author: String

    public init(label: String, author: String) {
        self.label = label
        self.author = author
    }

    public var id: String { author }
}

public struct EmberSearchHit: Sendable, Equatable, Identifiable {
    public var file: Int
    public var title: String
    public var detail: String

    public var id: Int { file }
}

extension Ember {
    public static let scoringHelp = "Each circle is a file, sized by lines of code; grey discs are folders.\n\nCode health runs from 10 (healthy) to 1 (unhealthy). It drops for long and deeply nested functions, high cyclomatic complexity, many parameters, large files, duplicated blocks and import cycles. Healthy is 8 or above, problematic 4 to 8, unhealthy below 4.\n\nHotspots are files that change often and are large: change frequency in the git history window times size. Refactoring targets are hotspots with low health, ranked by hotspot × (10 − health)."
    public static let targetsIntro = "Frequently changed files with low code health, ranked by hotspot × (10 − health). Improving these pays back on every future change."
    public static let couplingIntro = "Files committed together with this one. Degree is the share of their commits made together."
    public static let noHistory = "No git history"
    public static let trendPrompt = "Select a file to see how its size and complexity changed."
    public static let trendNeedsHistory = "Complexity trends need git history; this folder has none."
    public static let trendEmpty = "No earlier versions of this file in the history window."

    public static func trendLoading(_ path: String) -> String { "Reading the history of \(fileNameOf(path))…" }

    public static func couplingEmptyText(history: Bool) -> String {
        history ? "This file does not change together with other files often enough to count as coupled."
            : "Change coupling needs git history; this folder has none."
    }

    public static func hotspotsEmptyText(history: Bool) -> String {
        history ? "No file changed in the history window." : "Hotspots need git history; this folder has none."
    }

    public static func couplingTabTitle(_ count: Int) -> String { count == 0 ? "Coupling" : "Coupling (\(count))" }

    public static func commitThresholdText(_ value: Int) -> String { value == 0 ? "any" : "at least \(value) commits" }

    public static func pathCrumb(_ folder: String) -> String {
        folder.isEmpty ? "/" : folder.split(separator: "/", omittingEmptySubsequences: false).joined(separator: " / ")
    }

    public static func healthText(_ health: Double) -> String { decimal(health, digits: health >= 9.95 ? 0 : 1) }

    public static func healthShare(_ health: Double) -> Double { max(0.02, min(1, (health - 1) / 9)) }

    public static func defaultTab(_ snapshot: InsightSnapshot, chosen: EmberTab?) -> EmberTab {
        let history = hasHistory(snapshot)
        guard let chosen else { return history ? .hotspots : .codeHealth }
        return !history && chosen.needsHistory ? .codeHealth : chosen
    }

    public static func bandRange(_ band: HealthBand) -> (Double, Double) {
        switch band {
        case .healthy: return (8, 10)
        case .problematic: return (4, 7.5)
        case .unhealthy: return (1, 3.5)
        }
    }

    public static func toggleBand(_ filters: EmberFilters, _ band: HealthBand) -> EmberFilters {
        let range = bandRange(band)
        var out = filters
        let same = filters.minHealth == range.0 && filters.maxHealth == range.1
        out.minHealth = same ? 1 : range.0
        out.maxHealth = same ? 10 : range.1
        return out
    }

    public static func commitMax(_ snapshot: InsightSnapshot) -> Int {
        max(1, min(snapshot.files.reduce(0) { max($0, $1.commits) }, 200))
    }

    public static func authorOptions(_ snapshot: InsightSnapshot) -> [EmberAuthorOption] {
        let ranked = topAuthors(snapshot, limit: 0)
        let former = Set(snapshot.formerAuthors)
        let rankedSet = Set(ranked)
        func label(_ name: String) -> String { former.contains(name) ? name + " (former)" : name }
        var options = [EmberAuthorOption(label: "All authors", author: "")]
        for name in ranked { options.append(EmberAuthorOption(label: label(name), author: name)) }
        var seen = rankedSet
        for name in snapshot.authors where !seen.contains(name) {
            seen.insert(name)
            options.append(EmberAuthorOption(label: label(name), author: name))
        }
        return options
    }

    public static func bandNote(_ file: InsightFile, rank: Int) -> String {
        if rank > 0 { return "Refactoring target #\(rank)" }
        if file.hotspot >= 0.5 { return "Hotspot: changes often and is large" }
        if file.test { return "Test file" }
        return ""
    }

    public static func bandRows(_ summary: EmberSummary) -> [EmberRow] {
        let total = max(Int64(1), summary.healthyLines + summary.problematicLines + summary.unhealthyLines)
        func row(_ id: Int, _ band: HealthBand, _ files: Int, _ lines: Int64) -> EmberRow {
            let share = Double(lines) / Double(total)
            return EmberRow(id: id, title: band.name, detail: "\(number(files)) files · \(number(lines)) lines of code",
                            value: percentText(share) + " of code", fraction: share, color: bandColor(band))
        }
        return [
            row(0, .healthy, summary.healthy, summary.healthyLines),
            row(1, .problematic, summary.problematic, summary.problematicLines),
            row(2, .unhealthy, summary.unhealthy, summary.unhealthyLines),
        ]
    }

    public static func hotspotRows(_ snapshot: InsightSnapshot, limit: Int = 8) -> [EmberRow] {
        hotspotOrder(snapshot).prefix(limit).map { i in
            let f = snapshot.files[i]
            return EmberRow(id: i, title: fileNameOf(f.path), detail: folderOf(f.path), value: "\(number(f.commits)) commits",
                            fraction: f.hotspot, color: healthColor(f.health))
        }
    }

    public static func targetRows(_ snapshot: InsightSnapshot, _ targets: [EmberTarget]) -> [EmberRow] {
        let top = targets.first?.score ?? 1
        return targets.map { t in
            let f = snapshot.files[t.file]
            let notes = Array(t.reasons.prefix(1)) + Array(t.reasons.dropFirst(2).prefix(2))
            return EmberRow(id: t.file, title: fileNameOf(f.path), detail: folderOf(f.path), value: "health \(decimal(f.health))",
                            fraction: t.score / max(1e-9, top), color: healthColor(f.health), rank: t.rank, notes: notes)
        }
    }

    public static func couplingRows(_ snapshot: InsightSnapshot, _ refs: [EmberCouplingRef]) -> [EmberRow] {
        refs.map { ref in
            let f = snapshot.files[ref.file]
            let folder = folderOf(f.path)
            return EmberRow(id: ref.file, title: fileNameOf(f.path), detail: "\(folder.isEmpty ? "/" : folder) · \(number(ref.shared)) shared commits",
                            value: percentText(ref.degree), fraction: ref.degree, color: couplingLink)
        }
    }

    public static func metricLabel(_ snapshot: InsightSnapshot, tab: EmberTab, coupling: EmberCouplingIndex, file: Int) -> String {
        guard file >= 0 && file < snapshot.files.count else { return "" }
        let f = snapshot.files[file]
        switch tab {
        case .hotspots: return f.commits > 0 ? "\(number(f.commits)) commits" : ""
        case .refactoringTargets, .codeHealth: return "health \(decimal(f.health))"
        case .changeCoupling: return file < coupling.count && !coupling[file].isEmpty ? "\(coupling[file].count) coupled" : ""
        case .knowledge: return f.mainAuthor.isEmpty ? "" : "\(f.mainAuthor) \(percentText(f.mainAuthorShare))"
        case .defects: return f.fixCommits > 0 ? "\(f.fixCommits) fixes" : ""
        case .languages: return f.language
        }
    }

    public static func describeFile(_ snapshot: InsightSnapshot, file: Int, rank: Int) -> String {
        let f = snapshot.files[file]
        var parts = [
            fileNameOf(f.path),
            f.path,
            "Code health \(decimal(f.health)) · \(Insight.healthBand(f.health).name)",
            "\(number(f.codeLines)) lines of code · complexity \(number(f.complexity))",
        ]
        if hasHistory(snapshot) {
            parts.append("\(number(f.commits)) commits · \(number(f.authors)) authors · \(number(f.fixCommits)) fixes")
            if !f.mainAuthor.isEmpty { parts.append("Main author \(f.mainAuthor) (\(percentText(f.mainAuthorShare)))") }
        }
        if rank > 0 { parts.append("Refactoring target #\(rank)") }
        return parts.joined(separator: ". ")
    }

    public static func describeFolder(_ node: EmberNode) -> String {
        "\(node.name). \(number(node.files)) files · \(number(node.codeLines)) lines of code. Code health \(decimal(node.health)) (weighted by lines)"
    }

    public static func bestMatch(_ snapshot: InsightSnapshot, query text: String) -> Int {
        let query = trimmed(text).lowercased()
        if query.isEmpty { return -1 }
        var best = -1
        var bestRank = 1 << 30
        for (i, file) in snapshot.files.enumerated() {
            let path = file.path.lowercased()
            let name = fileNameOf(path)
            var rank: Int
            if name == query {
                rank = 0
            } else if name.hasPrefix(query) {
                rank = 1
            } else if name.contains(query) {
                rank = 2
            } else if path.contains(query) {
                rank = 3
            } else {
                continue
            }
            rank = rank * 100000 + file.path.utf16.count
            if rank < bestRank {
                bestRank = rank
                best = i
            }
        }
        return best
    }

    public static func searchHits(_ snapshot: InsightSnapshot, query text: String, limit: Int) -> [EmberSearchHit] {
        let query = trimmed(text).lowercased()
        if query.isEmpty || limit <= 0 { return [] }
        var ranked: [(Int, Int)] = []
        for (i, file) in snapshot.files.enumerated() {
            let path = file.path.lowercased()
            guard path.contains(query) else { continue }
            let rank = fileNameOf(path).contains(query) ? 0 : 1
            ranked.append((rank * 100000 + file.path.utf16.count, i))
        }
        ranked.sort { $0 < $1 }
        return ranked.prefix(limit).map { entry in
            let f = snapshot.files[entry.1]
            var detail = "\(f.path) · health \(decimal(f.health))"
            if f.commits > 0 { detail += " · \(number(f.commits)) commits" }
            return EmberSearchHit(file: entry.1, title: fileNameOf(f.path), detail: detail)
        }
    }

    static func dateFormatter(_ format: String, _ zone: TimeZone) -> DateFormatter {
        let formatter = DateFormatter()
        formatter.locale = Locale(identifier: "en_US_POSIX")
        formatter.timeZone = zone
        formatter.dateFormat = format
        return formatter
    }

    public static func dateText(_ time: Int64, zone: TimeZone = .current) -> String {
        dateFormatter("d MMM yyyy", zone).string(from: Date(timeIntervalSince1970: TimeInterval(time)))
    }

    public static func monthText(_ time: Int64, zone: TimeZone = .current) -> String {
        dateFormatter("MMM yyyy", zone).string(from: Date(timeIntervalSince1970: TimeInterval(time)))
    }

    public static func trendInfo(_ point: InsightTrendPoint, zone: TimeZone = .current) -> String {
        "\(point.commit.prefix(8)) · \(point.author) · \(dateText(point.time, zone: zone))\nHealth \(decimal(point.health)) · worst function \(number(point.maxComplexity))"
    }

    public static func compact(_ value: Double) -> String {
        if abs(value) >= 10000 { return decimal(value / 1000, digits: 0) + "k" }
        if abs(value) >= 1000 { return decimal(value / 1000, digits: 1) + "k" }
        return number(Int(value.rounded()))
    }

    public static func card(_ snapshot: InsightSnapshot, file index: Int, rank: Int, now: Int64, zone: TimeZone = .current) -> EmberCard {
        let f = snapshot.files[index]
        let history = hasHistory(snapshot)
        let span = spanText(snapshot.historyDays > 0 ? snapshot.historyDays : 365)
        var metrics: [EmberMetric] = []
        metrics.append(EmberMetric(label: "Commits", value: history
            ? "\(number(f.commits)) commits / \(span) · \(number(f.recentCommits)) in the last 90 days" : noHistory))
        metrics.append(EmberMetric(label: "Size", value: "\(number(f.codeLines)) lines of code · \(number(f.lines)) lines in total"))
        metrics.append(EmberMetric(label: "Main author", value: history && !f.mainAuthor.isEmpty
            ? "\(f.mainAuthor) (\(percentText(f.mainAuthorShare))) · \(number(f.authors)) authors" : noHistory))
        metrics.append(EmberMetric(label: "Knowledge", value: history
            ? "\(percentText(f.formerAuthorShare)) code by former contributors" : noHistory))
        metrics.append(EmberMetric(label: "Defects", value: history ? "\(number(f.fixCommits)) fix commits / \(span)" : noHistory))
        metrics.append(EmberMetric(label: "Modified", value: history && f.lastChanged > 0
            ? "\(agoText(f.lastChanged, now: now)) · \(dateText(f.lastChanged, zone: zone))" : noHistory))
        var worst: InsightFunction?
        for fn in f.functions where fn.complexity > (worst?.complexity ?? Int.min) { worst = fn }
        var complexity = "\(number(f.complexity)) in total"
        if let worst { complexity += " · worst \(worst.name)() \(number(worst.complexity)) at line \(worst.line)" }
        metrics.append(EmberMetric(label: "Complexity", value: complexity, line: worst?.line ?? 0))
        let longest = f.functions.reduce(0) { max($0, $1.length) }
        metrics.append(EmberMetric(label: "Functions",
                                   value: "\(number(f.functions.count)) functions · longest \(number(longest)) lines · deepest nesting \(number(f.maxNesting))"))
        metrics.append(EmberMetric(label: "Duplication", value: f.duplicatedLines > 0
            ? "\(number(f.duplicatedLines)) lines in repeated blocks" : "No repeated blocks"))
        var imports = "imports \(number(f.dependsOn.count)) files · used by \(number(f.dependedOnBy.count)) files"
        if !f.externalImports.isEmpty { imports += " · \(number(f.externalImports.count)) packages" }
        if f.cycle >= 0 && f.cycle < snapshot.cycles.count { imports += " · in an import cycle of \(snapshot.cycles[f.cycle].count) files" }
        metrics.append(EmberMetric(label: "Imports", value: imports))
        metrics.append(EmberMetric(label: "Language", value: f.test ? f.language + " · test file" : f.language))
        let band = Insight.healthBand(f.health)
        return EmberCard(file: index, folder: pathCrumb(folderOf(f.path)), name: fileNameOf(f.path), health: f.health, band: band,
                         note: bandNote(f, rank: rank), metrics: metrics, issues: f.healthIssues)
    }

    public static func rankedFiles(_ snapshot: InsightSnapshot, coloring: EmberColoring, coupling: EmberCouplingIndex) -> [EmberRow] {
        let files = snapshot.files
        let visible = files.indices.filter { $0 < coloring.files.count && !coloring.files[$0].dimmed }
        func color(_ i: Int) -> EmberColor { i < coloring.files.count ? coloring.files[i].color : neutral }
        func row(_ i: Int, _ value: String, _ fraction: Double, notes: [String] = []) -> EmberRow {
            EmberRow(id: i, title: fileNameOf(files[i].path), detail: folderOf(files[i].path), value: value,
                     fraction: max(0, min(1, fraction)), color: color(i), notes: notes)
        }
        func ordered(_ keys: (Int) -> [Double]) -> [Int] {
            visible.map { ($0, keys($0)) }.sorted { a, b in
                for (x, y) in zip(a.1, b.1) where x != y { return x < y }
                return U.less(files[a.0].path, files[b.0].path)
            }.map(\.0)
        }
        switch coloring.tab {
        case .hotspots:
            let maxCommits = Double(max(1, files.reduce(0) { max($0, $1.commits) }))
            return ordered { [-files[$0].hotspot, -Double(files[$0].commits)] }.map { i in
                row(i, files[i].commits > 0 ? "\(number(files[i].commits)) commits" : "no commits", Double(files[i].commits) / maxCommits)
            }
        case .refactoringTargets:
            let shown = Set(visible)
            return targetRows(snapshot, coloring.targets).filter { shown.contains($0.id) }
        case .codeHealth:
            return ordered { [files[$0].health, -Double(files[$0].codeLines)] }.map { i in
                row(i, "health \(decimal(files[i].health))", (files[i].health - 1) / 9)
            }
        case .changeCoupling:
            func strongest(_ i: Int) -> Double { i < coupling.count ? coupling[i].first?.degree ?? 0 : 0 }
            func count(_ i: Int) -> Int { i < coupling.count ? coupling[i].count : 0 }
            return ordered { [-strongest($0), -Double(count($0))] }.map { i in
                row(i, count(i) > 0 ? "\(count(i)) coupled · \(percentText(strongest(i)))" : "not coupled", strongest(i))
            }
        case .knowledge:
            return ordered { [-files[$0].formerAuthorShare, -Double(files[$0].codeLines)] }.map { i in
                let f = files[i]
                let value = f.mainAuthor.isEmpty ? "no commits in the window" : "\(f.mainAuthor) \(percentText(f.mainAuthorShare))"
                let notes = f.formerAuthorShare > 0 ? ["\(percentText(f.formerAuthorShare)) code by former contributors"] : []
                return row(i, value, f.formerAuthorShare, notes: notes)
            }
        case .defects:
            let maxFixes = Double(max(1, files.reduce(0) { max($0, $1.fixCommits) }))
            return ordered { [-Double(files[$0].fixCommits), -Double(files[$0].commits)] }.map { i in
                row(i, files[i].fixCommits > 0 ? "\(number(files[i].fixCommits)) fixes" : "no fixes", Double(files[i].fixCommits) / maxFixes)
            }
        case .languages:
            var order: [String: Int] = [:]
            for (k, name) in topLanguages(snapshot, limit: 0).enumerated() { order[name] = k }
            let maxLines = Double(max(1, files.reduce(0) { max($0, $1.codeLines) }))
            return ordered { [Double(order[files[$0].language] ?? Int.max), -Double(files[$0].codeLines)] }.map { i in
                row(i, files[i].language, Double(files[i].codeLines) / maxLines)
            }
        }
    }
}
