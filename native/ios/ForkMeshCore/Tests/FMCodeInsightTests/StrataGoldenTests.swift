import Foundation
import XCTest
@testable import FMCodeInsight

final class StrataJsonDiff {
    var problems: [String] = []
    var checks = 0

    func compare(_ path: String, _ actual: Any, _ expected: Any?) {
        guard let expected else {
            problems.append("\(path): missing expected")
            return
        }
        if let a = actual as? [String: Any] {
            guard let e = expected as? [String: Any] else { return problems.append("\(path): expected \(type(of: expected)), got an object") }
            let keys = Set(a.keys).union(e.keys)
            for key in keys.sorted() {
                guard let av = a[key] else {
                    problems.append("\(path).\(key): missing in the port")
                    continue
                }
                compare("\(path).\(key)", av, e[key])
            }
            return
        }
        if let a = actual as? [Any] {
            guard let e = expected as? [Any] else { return problems.append("\(path): expected \(type(of: expected)), got an array") }
            checks += 1
            if a.count != e.count {
                problems.append("\(path): \(a.count) items, want \(e.count)")
                return
            }
            for i in a.indices { compare("\(path)[\(i)]", a[i], e[i]) }
            return
        }
        checks += 1
        if actual is NSNull {
            if !(expected is NSNull) { problems.append("\(path): got null, want \(expected)") }
            return
        }
        if let a = actual as? String {
            guard let e = expected as? String else { return problems.append("\(path): got \(a.debugDescription), want \(expected)") }
            if a != e { problems.append("\(path): got \(a.prefix(300).debugDescription), want \(e.prefix(300).debugDescription)\(firstDifference(a, e))") }
            return
        }
        if let a = actual as? Bool {
            guard let e = expected as? NSNumber, e.intValue == 0 || e.intValue == 1 else {
                return problems.append("\(path): got \(a), want \(expected)")
            }
            if a != e.boolValue { problems.append("\(path): got \(a), want \(e.boolValue)") }
            return
        }
        let value: Double
        if let a = actual as? Int { value = Double(a) } else if let a = actual as? Double { value = a } else {
            problems.append("\(path): unsupported \(type(of: actual))")
            return
        }
        guard let e = (expected as? NSNumber)?.doubleValue else { return problems.append("\(path): got \(value), want \(expected)") }
        if abs(value - e) > 1e-9 * max(1, abs(e)) { problems.append("\(path): got \(value), want \(e)") }
    }

    func firstDifference(_ a: String, _ b: String) -> String {
        let x = Array(a.utf16)
        let y = Array(b.utf16)
        var i = 0
        while i < min(x.count, y.count) && x[i] == y[i] { i += 1 }
        let lo = max(0, i - 80)
        let left = String(utf16CodeUnits: Array(x[lo..<min(x.count, i + 80)]), count: min(x.count, i + 80) - lo)
        let right = String(utf16CodeUnits: Array(y[lo..<min(y.count, i + 80)]), count: min(y.count, i + 80) - lo)
        return " (first difference at \(i): got …\(left)… want …\(right)…)"
    }
}

enum StrataGolden {
    static func file(_ path: String, _ codeLines: Int) -> InsightFile {
        var file = InsightFile(path: path)
        file.language = "C++"
        file.codeLines = codeLines
        file.lines = codeLines + 10
        file.commentLines = codeLines / 5
        file.health = 9
        return file
    }

    static func function(_ name: String, _ line: Int, _ length: Int, _ complexity: Int, _ nesting: Int = 1, _ parameters: Int = 1) -> InsightFunction {
        InsightFunction(name: name, line: line, length: length, complexity: complexity, nesting: nesting, parameters: parameters)
    }

    static func link(_ s: inout InsightSnapshot, _ from: Int, _ to: Int) {
        s.files[from].dependsOn.append(to)
        s.files[to].dependedOnBy.append(from)
    }

    static func rich() -> InsightSnapshot {
        var files = [
            file("app/main.cpp", 300), file("app/window.cpp", 400), file("core/engine.cpp", 2500),
            file("core/model.cpp", 200), file("util/strings.cpp", 400), file("api/interfaces.h", 120),
        ]
        files[0].functions = [function("main", 10, 30, 3)]
        files[2].functions = [function("Engine::run", 40, 140, 26, 6, 8), function("Engine::step", 300, 20, 12)]
        files[2].complexity = 60
        files[2].maxComplexity = 26
        files[2].duplicatedLines = 120
        files[2].commentLines = 2
        files[2].commits = 40
        files[2].mainAuthor = "Author One"
        files[2].mainAuthorShare = 0.96
        files[2].authors = 2
        files[1].commits = 12
        files[1].formerAuthorShare = 0.9
        files[1].hotspot = 0.8
        files[4].types = 5
        files[5].types = 10
        files[5].abstractTypes = 9
        for i in files.indices where files[i].maxComplexity == 0 {
            for fn in files[i].functions { files[i].maxComplexity = max(files[i].maxComplexity, fn.complexity) }
        }
        var s = InsightSnapshot(files: files)
        s.source = "/tmp/strata-synthetic"
        s.name = "synthetic"
        s.authors = ["Author One", "Author Two"]
        s.historyDays = 365
        for (a, b) in [(0, 1), (1, 0), (0, 2), (1, 3), (2, 1), (2, 4), (3, 4), (5, 3), (0, 4)] { link(&s, a, b) }
        return s
    }

    static func fixtureSnapshots() throws -> (plain: InsightSnapshot, history: InsightSnapshot) {
        let now = InsightGoldenTests.now
        let source = MemorySource(files: try Fixture.repoFiles(), commits: try Fixture.history())
        var options = InsightOptions()
        options.history = false
        let base = try Insight.analyze(source: source, options: options, now: now)
        var history = base
        Insight.applyHistory(&history, try Fixture.history(), InsightOptions(), now: now)
        for i in history.files.indices {
            let result = Insight.computeHealth(history.files[i])
            history.files[i].health = result.health
            history.files[i].healthIssues = result.issues
        }
        Insight.computeHotspots(&history)
        history.head = String(repeating: "0", count: 40)
        history.branch = "main"
        history.analyzedAt = 0
        var plain = base
        plain.head = history.head
        plain.branch = history.branch
        plain.analyzedAt = 0
        return (plain, history)
    }

    static func rect(_ r: StrataRect) -> [Any] { [r.x, r.y, r.width, r.height] }

    static func layout(_ layout: StrataGraphLayout) -> [String: Any] {
        [
            "boxes": layout.boxes.map(rect),
            "ranks": layout.ranks,
            "edges": layout.edges.map { edge -> [String: Any] in
                ["from": edge.from, "to": edge.to, "weight": edge.weight, "cycle": edge.cycle, "flat": edge.flat,
                 "points": edge.points.map { [$0.x, $0.y] }]
            },
            "bounds": rect(layout.bounds),
            "crossings": layout.crossings,
            "rankCount": layout.rankCount,
        ]
    }

    static func score(_ s: StrataScore) -> [String: Any] {
        ["debt": s.debt, "effort": s.effort, "ratio": s.ratio, "rating": s.rating, "interest": s.interest, "violations": s.violations, "worst": s.worst]
    }

    static func treemap(_ snapshot: InsightSnapshot, _ graph: InsightModuleGraph, _ sizeBy: StrataTreemapSize) -> [String: Any] {
        let map = Strata.treemap(snapshot, graph, sizeBy: sizeBy, area: StrataRect(14, 6, 972, 648))
        return ["modules": map.modules.map(rect), "files": map.files.map(rect)]
    }

    static func model(_ snapshot: InsightSnapshot, depth: Int) -> [String: Any] {
        let model = Strata.buildModel(snapshot, depth: depth)
        let report = model.report
        let graph = report.graph
        func pathOf(_ file: Int) -> Any { file >= 0 && file < snapshot.files.count ? snapshot.files[file].path : NSNull() }
        func moduleOf(_ module: Int) -> Any { module >= 0 && module < graph.nodes.count ? graph.nodes[module].path : NSNull() }
        func refs(_ list: [StrataViolationRef]) -> [Any] { list.map { [report.rules[$0.rule].id, $0.index] as [Any] } }
        let rules: [Any] = report.rules.map { rule -> [String: Any] in
            [
                "id": rule.id, "title": rule.title, "category": rule.category.name, "severity": rule.severity.name,
                "moduleScope": rule.moduleScope, "debt": rule.debt, "worst": rule.worst,
                "violations": rule.violations.map { v -> [String: Any] in
                    ["file": pathOf(v.file), "module": moduleOf(v.module), "peerModule": moduleOf(v.peerModule), "line": v.line,
                     "severity": v.severity.name, "message": v.message, "debt": v.debt]
                },
            ]
        }
        let files: [Any] = snapshot.files.indices.map { f -> [String: Any] in
            var s = score(report.files[f])
            s["path"] = snapshot.files[f].path
            s["refs"] = refs(report.fileViolations[f])
            return s
        }
        let modules: [Any] = graph.nodes.indices.map { m -> [String: Any] in
            var s = score(report.modules[m])
            s["path"] = graph.nodes[m].path
            s["refs"] = refs(report.moduleViolations[m])
            s["worstRefs"] = refs(report.worstForModule(m, limit: 6))
            return s
        }
        let m = report.metrics
        let metrics: [String: Any] = [
            "codeLines": m.codeLines, "commentLines": m.commentLines, "files": m.files, "modules": m.modules, "types": m.types,
            "functions": m.functions, "averageComplexity": m.averageComplexity, "maxComplexity": m.maxComplexity,
            "maxComplexityFunction": m.maxComplexityFunction, "maxComplexityFile": pathOf(m.maxComplexityFile),
            "commentPercent": m.commentPercent, "duplicatedPercent": m.duplicatedPercent, "testFiles": m.testFiles,
        ]
        let drills: [Any] = graph.nodes.indices.map { module -> [String: Any] in
            let drill = Strata.drillGraph(snapshot, graph, module: module)
            return [
                "module": graph.nodes[module].path,
                "hiddenFiles": drill.hiddenFiles,
                "nodes": drill.nodes.map { node -> [String: Any] in
                    ["module": node.module, "dependent": node.dependent, "name": node.module ? moduleOf(node.index) : pathOf(node.index), "weight": node.weight]
                },
                "links": drill.links.map { [$0.from, $0.to, $0.weight, $0.cycle] as [Any] },
                "layout": layout(drill.layout),
            ]
        }
        let boxSizes: [Any] = [0, 1, 100, 1000, 5000, 20000, 100000].map { lines -> [Any] in
            let size = Strata.moduleBoxSize(lines)
            return [lines, size.width, size.height]
        }
        return [
            "depth": graph.depth,
            "summary": Strata.summaryLine(report),
            "history": report.history,
            "total": score(report.total),
            "metrics": metrics,
            "bySeverity": report.bySeverity,
            "cycles": graph.cycles.map { $0.map { graph.nodes[$0].path } },
            "nodes": graph.nodes.map { node -> [String: Any] in
                ["path": node.path, "cycle": node.cycle, "afferent": node.afferent, "efferent": node.efferent,
                 "instability": node.instability, "abstractness": node.abstractness, "distance": node.distance]
            },
            "edges": graph.edges.map { edge -> [String: Any] in
                ["from": graph.nodes[edge.from].path, "to": graph.nodes[edge.to].path, "weight": edge.weight, "inCycle": edge.inCycle]
            },
            "rules": rules,
            "files": files,
            "modules": modules,
            "gates": report.gates.map { ["name": $0.name, "status": $0.status.name, "measured": $0.measured, "detail": $0.detail] },
            "order": model.order.map { graph.nodes[$0].path },
            "layers": Strata.moduleLayers(graph),
            "layout": layout(model.layout),
            "drill": drills,
            "boxSizes": boxSizes,
            "treemapLines": treemap(snapshot, graph, .lines),
            "treemapComplexity": treemap(snapshot, graph, .complexity),
            "treemapFunctions": treemap(snapshot, graph, .functions),
            "report": Strata.reportHtml(snapshot, report, title: "fixture <repo>"),
        ]
    }

    static func layouts() -> [String: Any] {
        var mesh = StrataLayoutInput()
        for i in 0..<30 { mesh.sizes.append(StrataSize(Double(100 + (i % 4) * 30), Double(70 + (i % 3) * 20))) }
        for i in 0..<30 {
            for k in 1...3 {
                let j = i + k * 3 + (i % 5)
                if j < 30 { mesh.links.append(StrataLayoutLink(i, j, 1 + (i + j) % 7, false)) }
            }
        }
        var hinted = StrataLayoutInput()
        hinted.sizes = Array(repeating: StrataSize(120, 80), count: 6)
        hinted.layers = [3, 2, 2, 1, 0, 0]
        hinted.links = [StrataLayoutLink(0, 1), StrataLayoutLink(0, 2), StrataLayoutLink(1, 3), StrataLayoutLink(2, 4), StrataLayoutLink(3, 5)]
        var cyclic = StrataLayoutInput()
        cyclic.sizes = (0..<7).map { StrataSize(Double(120 + $0 * 7), 80) }
        cyclic.links = [StrataLayoutLink(0, 1, 2, true), StrataLayoutLink(1, 2, 1, true), StrataLayoutLink(2, 0, 3, true), StrataLayoutLink(2, 3, 1, false),
                        StrataLayoutLink(3, 4, 2, true), StrataLayoutLink(4, 5, 1, true), StrataLayoutLink(5, 3, 4, true), StrataLayoutLink(4, 3, 1, true),
                        StrataLayoutLink(0, 6, 1, false)]
        var wide = StrataLayoutInput()
        wide.sizes.append(StrataSize(140, 80))
        for i in 1...40 {
            wide.sizes.append(StrataSize(120, 70))
            wide.links.append(StrataLayoutLink(0, i))
        }
        var isolated = StrataLayoutInput()
        isolated.sizes = Array(repeating: StrataSize(100, 60), count: 5)
        isolated.links = [StrataLayoutLink(0, 1)]
        var tall = StrataLayoutInput()
        tall.sizes = Array(repeating: StrataSize(110, 70), count: 12)
        for i in 0..<7 { tall.links.append(StrataLayoutLink(i, i + 1)) }
        var lonely = StrataLayoutInput()
        lonely.sizes = (0..<9).map { StrataSize(Double(90 + $0 * 10), 60) }
        return [
            "mesh": layout(Strata.layeredLayout(mesh)),
            "hinted": layout(Strata.layeredLayout(hinted)),
            "cyclic": layout(Strata.layeredLayout(cyclic)),
            "wide": layout(Strata.layeredLayout(wide)),
            "isolated": layout(Strata.layeredLayout(isolated)),
            "tall": layout(Strata.layeredLayout(tall)),
            "lonely": layout(Strata.layeredLayout(lonely)),
            "squarify": Strata.squarify([50, 25, 12.5, 12.5, 0, 3, 7.25, 1], StrataRect(3, 5, 400, 200)).map(rect),
        ]
    }
}

final class StrataGoldenTests: XCTestCase {
    func golden() throws -> [String: Any] {
        let url = Fixture.root.appendingPathComponent("strata.json")
        guard FileManager.default.fileExists(atPath: url.path) else { throw XCTSkip("native/fixtures/insight/strata.json is missing") }
        return try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any] ?? [:]
    }

    func finish(_ diff: StrataJsonDiff, minimum: Int) {
        XCTAssertGreaterThanOrEqual(diff.checks, minimum)
        XCTAssert(diff.problems.isEmpty, "\(diff.problems.count) differences from the desktop:\n" + diff.problems.prefix(40).joined(separator: "\n"))
    }

    func testFormattingMatchesTheDesktop() throws {
        let golden = try golden()
        let diff = StrataJsonDiff()
        for pair in golden["formatDebt"] as? [[Any]] ?? [] {
            let minutes = (pair[0] as? NSNumber)?.doubleValue ?? 0
            diff.compare("formatDebt(\(minutes))", Strata.formatDebt(minutes), pair[1])
        }
        for pair in golden["formatPercent"] as? [[Any]] ?? [] {
            let ratio = (pair[0] as? NSNumber)?.doubleValue ?? 0
            diff.compare("formatPercent(\(ratio))", Strata.formatPercent(ratio), pair[1])
        }
        finish(diff, minimum: 25)
    }

    func testLayoutsMatchTheDesktop() throws {
        let golden = try golden()
        let diff = StrataJsonDiff()
        diff.compare("layouts", StrataGolden.layouts(), golden["layouts"])
        finish(diff, minimum: 300)
    }

    func testSyntheticModelMatchesTheDesktop() throws {
        let golden = try golden()
        let diff = StrataJsonDiff()
        diff.compare("synthetic", StrataGolden.model(StrataGolden.rich(), depth: 1), golden["synthetic"])
        finish(diff, minimum: 200)
    }

    func testFixtureModelWithHistoryMatchesTheDesktop() throws {
        let golden = try golden()
        let snapshots = try StrataGolden.fixtureSnapshots()
        let diff = StrataJsonDiff()
        diff.compare("auto", StrataGolden.model(snapshots.history, depth: 0), golden["auto"])
        finish(diff, minimum: 1000)
    }

    func testFixtureModelAtDepthTwoMatchesTheDesktop() throws {
        let golden = try golden()
        let snapshots = try StrataGolden.fixtureSnapshots()
        let diff = StrataJsonDiff()
        diff.compare("depth2", StrataGolden.model(snapshots.history, depth: 2), golden["depth2"])
        finish(diff, minimum: 1000)
    }

    func testFixtureModelWithoutHistoryMatchesTheDesktop() throws {
        let golden = try golden()
        let snapshots = try StrataGolden.fixtureSnapshots()
        let diff = StrataJsonDiff()
        diff.compare("plain", StrataGolden.model(snapshots.plain, depth: 0), golden["plain"])
        finish(diff, minimum: 1000)
    }
}
