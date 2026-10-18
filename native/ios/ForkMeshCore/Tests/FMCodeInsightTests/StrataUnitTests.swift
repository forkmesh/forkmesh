import Foundation
import XCTest
@testable import FMCodeInsight

final class StrataUnitTests: XCTestCase {
    func graphFor(_ snapshot: InsightSnapshot, _ modules: [String]) -> InsightModuleGraph {
        var graph = InsightModuleGraph()
        graph.depth = 1
        graph.moduleOfFile = [Int](repeating: -1, count: snapshot.files.count)
        for name in modules {
            var node = InsightModuleNode()
            node.path = name
            node.name = name
            graph.nodes.append(node)
        }
        for (f, file) in snapshot.files.enumerated() {
            let top = String(file.path.split(separator: "/").first ?? "")
            guard let m = modules.firstIndex(of: top) else { continue }
            graph.moduleOfFile[f] = m
            graph.nodes[m].files.append(f)
            graph.nodes[m].codeLines += file.codeLines
            graph.nodes[m].complexity += file.complexity
            graph.nodes[m].functions += file.functions.count
            graph.nodes[m].types += file.types
            graph.nodes[m].abstractTypes += file.abstractTypes
        }
        var edgeOf: [Int: Int] = [:]
        for (f, file) in snapshot.files.enumerated() {
            for d in file.dependsOn {
                let a = graph.moduleOfFile[f]
                let b = graph.moduleOfFile[d]
                if a < 0 || b < 0 { continue }
                if a == b {
                    graph.nodes[a].internalEdges += 1
                    continue
                }
                let key = a * 1000 + b
                if edgeOf[key] == nil {
                    edgeOf[key] = graph.edges.count
                    var edge = InsightModuleEdge()
                    edge.from = a
                    edge.to = b
                    graph.edges.append(edge)
                }
                let e = edgeOf[key] ?? 0
                graph.edges[e].weight += 1
                graph.edges[e].fileEdges.append(InsightFileEdge(from: f, to: d))
            }
        }
        var incoming = [Set<Int>](repeating: [], count: graph.nodes.count)
        var outgoing = [Set<Int>](repeating: [], count: graph.nodes.count)
        for edge in graph.edges {
            outgoing[edge.from].insert(edge.to)
            incoming[edge.to].insert(edge.from)
        }
        for i in graph.nodes.indices {
            graph.nodes[i].afferent = incoming[i].count
            graph.nodes[i].efferent = outgoing[i].count
            let sum = graph.nodes[i].afferent + graph.nodes[i].efferent
            graph.nodes[i].instability = sum > 0 ? Double(graph.nodes[i].efferent) / Double(sum) : 0
            graph.nodes[i].abstractness = graph.nodes[i].types > 0 ? Double(graph.nodes[i].abstractTypes) / Double(graph.nodes[i].types) : 0
            graph.nodes[i].distance = abs(graph.nodes[i].abstractness + graph.nodes[i].instability - 1.0)
            graph.nodes[i].health = 9
        }
        return graph
    }

    func richReport() -> (InsightSnapshot, StrataReport) {
        let snapshot = StrataGolden.rich()
        return (snapshot, Strata.buildReport(snapshot, graphFor(snapshot, ["app", "core", "util", "api"])))
    }

    func count(_ report: StrataReport, _ id: String) -> Int {
        let r = report.ruleIndex(id)
        return r < 0 ? -1 : report.rules[r].violations.count
    }

    func first(_ report: StrataReport, _ id: String) -> StrataViolation? {
        let r = report.ruleIndex(id)
        return r < 0 ? nil : report.rules[r].violations.first
    }

    func gate(_ report: StrataReport, _ name: String) -> StrataGate? {
        report.gates.first { $0.name == name }
    }

    func overlapping(_ layout: StrataGraphLayout) -> Bool {
        for i in layout.boxes.indices {
            for j in (i + 1)..<layout.boxes.count {
                let a = layout.boxes[i].adjusted(1, 1, -1, -1)
                let b = layout.boxes[j].adjusted(1, 1, -1, -1)
                if a.left < b.right && b.left < a.right && a.top < b.bottom && b.top < a.bottom { return true }
            }
        }
        return false
    }

    func testFormatting() {
        XCTAssertEqual(Strata.formatDebt(0), "0 min")
        XCTAssertEqual(Strata.formatDebt(45), "45 min")
        XCTAssertEqual(Strata.formatDebt(200), "3 h 20 min")
        XCTAssertEqual(Strata.formatDebt(6 * 8 * 60 + 3 * 60), "6 d 3 h")
        XCTAssertEqual(Strata.formatDebt(16 * 60), "2 d")
        XCTAssertEqual(Strata.effortMinutes(1000), 18 * 8 * 60, accuracy: 1e-9)
        XCTAssertEqual(Strata.ratingFor(0.0), 0)
        XCTAssertEqual(Strata.ratingFor(0.05), 0)
        XCTAssertEqual(Strata.ratingFor(0.051), 1)
        XCTAssertEqual(Strata.ratingFor(0.10), 1)
        XCTAssertEqual(Strata.ratingFor(0.2), 2)
        XCTAssertEqual(Strata.ratingFor(0.5), 3)
        XCTAssertEqual(Strata.ratingFor(0.51), 4)
        XCTAssertEqual(Strata.ratingLetter(0), "A")
        XCTAssertEqual(Strata.ratingLetter(4), "E")
        XCTAssertEqual(Strata.ratingLetter(9), "E")
        XCTAssertEqual(Strata.ruleCatalog().count, 17)
    }

    func testFixedRoundsTiesUpLikeQt() {
        XCTAssertEqual(Strata.fixed(13.25, 1), "13.3")
        XCTAssertEqual(Strata.fixed(0.125, 2), "0.13")
        XCTAssertEqual(Strata.fixed(12.5, 0), "13")
        XCTAssertEqual(Strata.fixed(0.0, 2), "0.00")
        XCTAssertEqual(Strata.fixed(-0.001, 2), "0.00")
        XCTAssertEqual(Strata.fixed(-1.5, 0), "-2")
        XCTAssertEqual(Strata.fixed(9.96, 1), "10.0")
        XCTAssertEqual(Strata.localeFixed(1234567.891, 2), "1,234,567.89")
        XCTAssertEqual(Strata.grouped(1000), "1,000")
        XCTAssertEqual(Strata.grouped(999), "999")
        XCTAssertEqual(Strata.grouped(-1234), "-1,234")
        XCTAssertEqual(Strata.compact(123_456), "123k")
        XCTAssertEqual(Strata.compact(2_500_000), "2.50M")
        XCTAssertEqual(Strata.compact(99_999), "99,999")
        XCTAssertEqual(Strata.htmlEscaped("a<b>&\"c'"), "a&lt;b&gt;&amp;&quot;c'")
    }

    func testRulesFire() {
        let (_, report) = richReport()
        XCTAssertEqual(report.graph.cycles.count, 1)
        XCTAssertEqual(count(report, "ARC01"), 2)
        XCTAssertEqual(first(report, "ARC01")?.severity, .critical)
        XCTAssertGreaterThanOrEqual(first(report, "ARC01")?.debt ?? 0, 30)
        XCTAssertEqual(count(report, "ARC02"), 3)
        XCTAssertEqual(count(report, "ARC04"), 1)
        XCTAssertEqual(count(report, "ARC05"), 1)
        XCTAssertEqual(count(report, "CS01"), 2)
        XCTAssertEqual(first(report, "CS01")?.severity, .high)
        XCTAssertEqual(first(report, "CS01")?.line, 40)
        XCTAssertEqual(count(report, "CS02"), 1)
        XCTAssertEqual(count(report, "CS03"), 1)
        XCTAssertEqual(count(report, "CS04"), 1)
        XCTAssertEqual(count(report, "CS05"), 1)
        XCTAssertEqual(first(report, "MNT01")?.severity, .high)
        XCTAssertEqual(count(report, "MNT02"), 1)
        XCTAssertEqual(count(report, "MNT03"), 1)
        XCTAssertEqual(count(report, "MNT04"), 1)
        XCTAssertEqual(count(report, "MNT05"), 1)
        XCTAssertEqual(first(report, "MNT05")?.message, "90 % written by former contributors, hotspot 0.80")
        XCTAssertEqual(first(report, "MNT01")?.message, "2,500 lines of code")
    }

    func testDebtGrowsWithTheExcess() {
        var a = StrataGolden.file("a/x.cpp", 100)
        a.functions = [StrataGolden.function("f", 1, 10, 12)]
        var b = StrataGolden.file("a/y.cpp", 100)
        b.functions = [StrataGolden.function("g", 1, 10, 40)]
        let snapshot = InsightSnapshot(files: [a, b])
        let report = Strata.buildReport(snapshot, graphFor(snapshot, ["a"]))
        let rule = report.rules[report.ruleIndex("CS01")]
        let small = rule.violations.first { $0.file == 0 }?.debt ?? 0
        let big = rule.violations.first { $0.file == 1 }?.debt ?? 0
        XCTAssertGreaterThan(big, small)
        XCTAssertGreaterThan(small, 0)
        XCTAssertEqual(rule.violations.first?.file, 1)
    }

    func testStableDependenciesAndHubs() {
        var sdp = InsightSnapshot(files: ["s", "a", "b", "c", "v", "x", "y", "z"].map { StrataGolden.file("\($0)/one.cpp", 100) })
        for (from, to) in [(1, 0), (2, 0), (3, 0), (0, 4), (4, 5), (4, 6), (4, 7)] { StrataGolden.link(&sdp, from, to) }
        let sdpReport = Strata.buildReport(sdp, graphFor(sdp, ["s", "a", "b", "c", "v", "x", "y", "z"]))
        XCTAssertEqual(count(sdpReport, "ARC03"), 1)
        XCTAssertEqual(first(sdpReport, "ARC03")?.module, 0)
        XCTAssertEqual(first(sdpReport, "ARC03")?.peerModule, 4)
        XCTAssertEqual(first(sdpReport, "ARC03")?.message, "s (I = 0.25) depends on the less stable v (I = 0.75)")

        var hub = InsightSnapshot(files: (0..<25).map { StrataGolden.file("h/f\($0).cpp", 50) })
        for i in 1...12 { StrataGolden.link(&hub, 0, i) }
        for i in 13..<25 { StrataGolden.link(&hub, i, 0) }
        let hubReport = Strata.buildReport(hub, graphFor(hub, ["h"]))
        XCTAssertEqual(count(hubReport, "DES01"), 1)
        XCTAssertEqual(first(hubReport, "DES01")?.message, "Imported by 12 files and imports 12")
    }

    func testDebtAndGates() {
        let (snapshot, report) = richReport()
        let sum = report.rules.flatMap(\.violations).reduce(0) { $0 + $1.debt }
        XCTAssertEqual(report.total.debt, sum, accuracy: 1e-6)
        XCTAssertEqual(report.modules.reduce(0) { $0 + $1.debt }, sum, accuracy: 1e-6)
        let codeLines = 300 + 400 + 2500 + 200 + 400 + 120
        XCTAssertEqual(report.metrics.codeLines, codeLines)
        XCTAssertEqual(report.total.ratio, sum / Strata.effortMinutes(codeLines), accuracy: 1e-9)
        XCTAssertEqual(report.total.rating, Strata.ratingFor(report.total.ratio))
        XCTAssertGreaterThan(report.total.interest, 0)
        XCTAssertLessThanOrEqual(report.files[2].interest, 2 * report.files[2].debt)
        XCTAssertEqual(report.bySeverity[0], 2)
        XCTAssertEqual(gate(report, "No module dependency cycle")?.status, .fail)
        XCTAssertEqual(gate(report, "No critical rule violated")?.status, .fail)
        XCTAssertEqual(gate(report, "No critical rule violated")?.measured, "2 in 1 rule")
        XCTAssertEqual(gate(report, "No function above complexity 50")?.status, .pass)
        XCTAssertEqual(gate(report, "No function above complexity 50")?.measured, "26 (Engine::run)")
        let summary = Strata.summaryLine(report)
        XCTAssertTrue(summary.hasPrefix("Rating "))
        XCTAssertTrue(summary.contains("failing"))
        XCTAssertEqual(snapshot.files.count, report.files.count)

        var x = StrataGolden.file("a/x.cpp", 500)
        x.functions = [StrataGolden.function("f", 1, 10, 3)]
        var clean = InsightSnapshot(files: [x, StrataGolden.file("b/y.cpp", 500)])
        StrataGolden.link(&clean, 0, 1)
        let cleanReport = Strata.buildReport(clean, graphFor(clean, ["a", "b"]))
        XCTAssertEqual(cleanReport.total.debt, 0)
        XCTAssertEqual(cleanReport.total.rating, 0)
        XCTAssertEqual(cleanReport.gatesWith(.fail), 0)
        XCTAssertEqual(cleanReport.gatesWith(.warn), 0)
        XCTAssertEqual(cleanReport.total.interest, 0)
        XCTAssertTrue(Strata.summaryLine(cleanReport).contains("all gates pass"))
        XCTAssertEqual(gate(cleanReport, "Debt in the most-changed files")?.measured, "No history")
        XCTAssertFalse(cleanReport.history)

        var monster = StrataGolden.file("a/x.cpp", 40)
        monster.functions = [StrataGolden.function("monster", 1, 400, 70, 9, 12)]
        monster.commits = 20
        let bad = InsightSnapshot(files: [monster])
        let badReport = Strata.buildReport(bad, graphFor(bad, ["a"]))
        XCTAssertEqual(badReport.total.rating, 4)
        XCTAssertEqual(gate(badReport, "Technical debt rating")?.status, .fail)
        XCTAssertEqual(gate(badReport, "No function above complexity 50")?.status, .fail)
        XCTAssertEqual(gate(badReport, "Debt in the most-changed files")?.status, .fail)
        XCTAssertEqual(gate(badReport, "Debt in the most-changed files")?.measured.hasSuffix("in 1 files"), true)
    }

    func testWarningSummary() {
        var report = StrataReport()
        var gate = StrataGate()
        gate.status = .warn
        report.gates = [gate, StrataGate()]
        XCTAssertEqual(Strata.summaryLine(report), "Rating A · 0 min debt · 1 gate warning")
        XCTAssertEqual(Strata.gatesHeadline(report).text, "1 warning")
        report.gates.append(gate)
        XCTAssertEqual(Strata.summaryLine(report), "Rating A · 0 min debt · 2 gates warning")
    }

    func layoutMesh() -> StrataLayoutInput {
        var input = StrataLayoutInput()
        for i in 0..<30 { input.sizes.append(StrataSize(Double(100 + (i % 4) * 30), Double(70 + (i % 3) * 20))) }
        for i in 0..<30 {
            for k in 1...3 {
                let j = i + k * 3 + (i % 5)
                if j < 30 { input.links.append(StrataLayoutLink(i, j, 1 + (i + j) % 7, false)) }
            }
        }
        return input
    }

    func testLayout() {
        let input = layoutMesh()
        let layout = Strata.layeredLayout(input)
        XCTAssertFalse(overlapping(layout))
        for link in input.links {
            XCTAssertLessThan(layout.boxes[link.from].bottom, layout.boxes[link.to].top)
        }
        XCTAssertEqual(layout.edges.count, input.links.count)
        for edge in layout.edges {
            XCTAssertGreaterThanOrEqual(edge.points.count, 2)
            let start = edge.points[0]
            let box = layout.boxes[edge.from]
            XCTAssertLessThanOrEqual(hypot(start.x - box.centerX, start.y - box.bottom), box.width)
        }
        XCTAssertEqual(Strata.layeredLayout(input), layout)
        XCTAssertTrue(layout.bounds.contains(StrataPoint(layout.boxes[0].x, layout.boxes[0].y)))

        var hinted = StrataLayoutInput()
        hinted.sizes = Array(repeating: StrataSize(120, 80), count: 6)
        hinted.layers = [3, 2, 2, 1, 0, 0]
        hinted.links = [StrataLayoutLink(0, 1), StrataLayoutLink(0, 2), StrataLayoutLink(1, 3), StrataLayoutLink(2, 4), StrataLayoutLink(3, 5)]
        let layered = Strata.layeredLayout(hinted)
        for i in 0..<6 {
            for j in 0..<6 where hinted.layers[i] > hinted.layers[j] {
                XCTAssertLessThan(layered.boxes[i].top, layered.boxes[j].top)
            }
        }
        XCTAssertEqual(layered.ranks[1], layered.ranks[2])

        var cyclic = StrataLayoutInput()
        cyclic.sizes = Array(repeating: StrataSize(120, 80), count: 4)
        cyclic.links = [StrataLayoutLink(0, 1, 2, true), StrataLayoutLink(1, 2, 1, true), StrataLayoutLink(2, 0, 3, true), StrataLayoutLink(2, 3, 1, false)]
        let cycleLayout = Strata.layeredLayout(cyclic)
        let upward = cycleLayout.edges.filter { $0.points.count >= 2 && $0.points[$0.points.count - 1].y < $0.points[0].y }
        XCTAssertEqual(upward.count, 1)
        XCTAssertEqual(upward.first?.from, 1)
        XCTAssertGreaterThan(cycleLayout.ranks[3], max(cycleLayout.ranks[0], cycleLayout.ranks[1], cycleLayout.ranks[2]))
        XCTAssertFalse(overlapping(cycleLayout))

        var wide = StrataLayoutInput()
        wide.sizes.append(StrataSize(140, 80))
        for i in 1...40 {
            wide.sizes.append(StrataSize(120, 70))
            wide.links.append(StrataLayoutLink(0, i))
        }
        let wrapped = Strata.layeredLayout(wide)
        XCTAssertGreaterThan(wrapped.rankCount, 2)
        XCTAssertFalse(overlapping(wrapped))
        XCTAssertLessThan(wrapped.bounds.width, 40 * 160)

        var isolated = StrataLayoutInput()
        isolated.sizes = Array(repeating: StrataSize(100, 60), count: 5)
        isolated.links = [StrataLayoutLink(0, 1)]
        let islands = Strata.layeredLayout(isolated)
        XCTAssertFalse(overlapping(islands))
        XCTAssertTrue(islands.boxes[4].top > islands.boxes[1].bottom || islands.boxes[4].left > islands.boxes[1].right)

        XCTAssertEqual(Strata.layeredLayout(StrataLayoutInput()).boxes.count, 0)
        XCTAssertEqual(Strata.moduleBoxSize(0), StrataSize(128, 79))
        XCTAssertEqual(Strata.moduleBoxSize(100_000), StrataSize(340, 210))
    }

    func testMatrixAndTreemap() {
        var graph = InsightModuleGraph()
        for name in ["ui", "net", "core", "base", "io"] {
            var node = InsightModuleNode()
            node.name = name
            node.path = name
            graph.nodes.append(node)
        }
        for (a, b) in [(0, 1), (0, 2), (1, 4), (4, 1), (1, 2), (2, 3), (4, 3)] {
            var edge = InsightModuleEdge()
            edge.from = a
            edge.to = b
            edge.weight = 1
            graph.edges.append(edge)
        }
        Strata.normalizeGraph(&graph)
        XCTAssertEqual(graph.cycles, [[1, 4]])
        XCTAssertEqual(graph.nodes[0].afferent, 0)
        XCTAssertEqual(graph.nodes[0].efferent, 2)
        let order = Strata.matrixOrder(graph)
        XCTAssertEqual(order.count, 5)
        XCTAssertEqual(order.first, 0)
        XCTAssertEqual(order.last, 3)
        let a = order.firstIndex(of: 1) ?? 0
        let b = order.firstIndex(of: 4) ?? 0
        XCTAssertEqual(abs(a - b), 1)
        let layers = Strata.moduleLayers(graph)
        XCTAssertEqual(layers[3], 0)
        XCTAssertGreaterThan(layers[0], layers[1])

        let weights: [Double] = [50, 25, 12.5, 12.5, 0]
        let area = StrataRect(0, 0, 400, 200)
        let rects = Strata.squarify(weights, area)
        for i in 0..<4 {
            XCTAssertEqual(rects[i].width * rects[i].height, weights[i] / 100.0 * 80_000, accuracy: 1.0)
            XCTAssertTrue(rects[i].left >= -0.01 && rects[i].top >= -0.01 && rects[i].right <= 400.01 && rects[i].bottom <= 200.01)
            for j in (i + 1)..<4 {
                let p = rects[i].adjusted(0.01, 0.01, -0.01, -0.01)
                let q = rects[j].adjusted(0.01, 0.01, -0.01, -0.01)
                XCTAssertFalse(p.left < q.right && q.left < p.right && p.top < q.bottom && q.top < p.bottom)
            }
        }
        XCTAssertTrue(rects[4].isEmpty)
        XCTAssertEqual(Strata.squarify([1, 2], StrataRect(0, 0, 0, 10)), [StrataRect(), StrataRect()])
    }

    func testDrillAndReport() {
        let (snapshot, report) = richReport()
        let drill = Strata.drillGraph(snapshot, report.graph, module: 1)
        XCTAssertEqual(drill.nodes.filter { !$0.module }.count, 2)
        XCTAssertGreaterThanOrEqual(drill.nodes.filter(\.module).count, 2)
        XCTAssertFalse(overlapping(drill.layout))
        XCTAssertEqual(Strata.drillGraph(snapshot, report.graph, module: 99).nodes.count, 0)
        let capped = Strata.drillGraph(snapshot, report.graph, module: 1, maxFiles: 1)
        XCTAssertEqual(capped.hiddenFiles, 1)

        let html = Strata.reportHtml(snapshot, report, title: "synthetic")
        XCTAssertTrue(html.contains("<h2 id=\"dashboard\">Dashboard</h2>"))
        XCTAssertTrue(html.contains("Quality gates") && html.contains("No module dependency cycle"))
        XCTAssertTrue(html.contains("ARC01") && html.contains("Avoid module dependency cycles"))
        XCTAssertTrue(html.contains("<h2 id=\"modules\">Modules</h2>") && html.contains("util"))
        XCTAssertTrue(html.contains("core/engine.cpp:40"))
        XCTAssertFalse(html.contains("<script"))

        var dated = snapshot
        dated.analyzedAt = 1_792_000_000
        dated.branch = "main"
        dated.head = "0123456789abcdef"
        let stamped = Strata.reportHtml(dated, report, title: "x", analyzedText: "today")
        XCTAssertTrue(stamped.contains("<div class=\"muted\">branch main · commit 0123456789 · analyzed today</div>"))
    }

    func testBuildModelUsesTheEngineGraph() {
        let model = Strata.buildModel(StrataGolden.rich(), depth: 1)
        XCTAssertEqual(model.report.graph.nodes.map(\.path), ["api", "app", "core", "util"])
        XCTAssertEqual(model.order.count, 4)
        XCTAssertEqual(model.layout.boxes.count, 4)
        XCTAssertEqual(model.summary, Strata.summaryLine(model.report))
        XCTAssertEqual(model.depth, 1)
    }

    func testColours() {
        XCTAssertEqual(StrataPalette.scale(0), StrataPalette.healthy)
        XCTAssertEqual(StrataPalette.scale(0.5), StrataPalette.problematic)
        XCTAssertEqual(StrataPalette.scale(2), StrataPalette.unhealthy)
        XCTAssertEqual(StrataPalette.health(10), StrataPalette.healthy)
        XCTAssertEqual(StrataPalette.health(1), StrataPalette.unhealthy)
        XCTAssertEqual(StrataPalette.rating(7), StrataPalette.ratings[4])
        XCTAssertEqual(StrataPalette.severity(.low), StrataRGB(hex: 0x8B949E))
        XCTAssertEqual(StrataPalette.instability(0), StrataRGB(hex: 0x4C8EDA))
        XCTAssertEqual(StrataPalette.instability(1).blue, StrataRGB(hex: 0xF09A4A).blue, accuracy: 1e-12)
        XCTAssertEqual(StrataPalette.change(0, dark: true), StrataRGB(hex: 0x2A3A4E))
        XCTAssertEqual(StrataPalette.gate(.warn), StrataPalette.problematic)
        XCTAssertTrue(StrataPalette.healthy.prefersDarkText)
        XCTAssertFalse(StrataRGB(hex: 0x1F2328).prefersDarkText)
        let model = Strata.buildModel(StrataGolden.rich(), depth: 1)
        let engine = model.snapshot.indexOf("core/engine.cpp")
        XCTAssertEqual(Strata.fileColor(model, engine, by: .complexity), StrataPalette.scale(1))
        XCTAssertEqual(Strata.fileColor(model, engine, by: .rating), StrataPalette.rating(model.report.files[engine].rating))
        XCTAssertEqual(Strata.treemapColorValue(model, engine, by: .changeFrequency, maxCommits: Strata.maxCommits(model.snapshot)), 1, accuracy: 1e-12)
        XCTAssertEqual(Strata.treemapColorValue(model, engine, by: .maxComplexity, maxCommits: 1), 22.0 / 26.0, accuracy: 1e-12)
        let api = model.report.graph.nodes.firstIndex { $0.path == "api" } ?? 0
        XCTAssertEqual(Strata.moduleColor(model, api, by: .instability), StrataPalette.instability(model.report.graph.nodes[api].instability))
        XCTAssertEqual(Strata.edgeWidth(0), 1)
        XCTAssertEqual(Strata.edgeWidth(1_000_000), 7)
    }

    func testRuleFilters() {
        let model = Strata.buildModel(StrataGolden.rich(), depth: 1)
        let all = Strata.ruleGroups(model, text: "")
        XCTAssertEqual(all.map(\.category), StrataCategory.allCases)
        XCTAssertEqual(all.flatMap(\.rows).count, 17)
        XCTAssertEqual(all.reduce(0) { $0 + $1.count }, model.report.total.violations)

        let engine = Strata.ruleGroups(model, text: "ENGINE.cpp")
        XCTAssertFalse(engine.isEmpty)
        for row in engine.flatMap(\.rows) {
            XCTAssertFalse(row.all)
            let shown = Strata.ruleViolations(model, rule: row.rule, text: "ENGINE.cpp", all: row.all)
            XCTAssertEqual(shown.indexes.count, row.matching)
            for i in shown.indexes {
                let v = model.report.rules[row.rule].violations[i]
                XCTAssertTrue(Strata.violationMatches(model, v, text: "engine.cpp"))
            }
        }
        let byId = Strata.ruleGroups(model, text: "cs05")
        XCTAssertEqual(byId.flatMap(\.rows).map { model.report.rules[$0.rule].id }, ["CS05"])
        XCTAssertTrue(byId.first?.rows.first?.all ?? false)

        let critical = Strata.ruleGroups(model, text: "", severity: .critical)
        XCTAssertEqual(critical.flatMap(\.rows).map { model.report.rules[$0.rule].id }, ["ARC01"])
        XCTAssertEqual(critical.first?.count, model.report.bySeverity[0])
        let lowNoise = Strata.ruleGroups(model, text: "nothing-matches-this", severity: nil)
        XCTAssertTrue(lowNoise.isEmpty)

        let r = model.report.ruleIndex("ARC01")
        let limited = Strata.ruleViolations(model, rule: r, text: "", all: true, limit: 1)
        XCTAssertEqual(limited.indexes, [0])
        XCTAssertEqual(limited.hidden, model.report.rules[r].violations.count - 1)
        let cycleViolation = model.report.rules[r].violations[0]
        XCTAssertEqual(Strata.violationWhere(model, rule: r, cycleViolation), model.report.graph.nodes[cycleViolation.module].path)
        XCTAssertEqual(Strata.violationTarget(model, rule: r, cycleViolation)?.line, 0)
        let cs = model.report.ruleIndex("CS05")
        let brain = model.report.rules[cs].violations[0]
        XCTAssertEqual(Strata.violationWhere(model, rule: cs, brain), "core/engine.cpp:40")
        XCTAssertEqual(Strata.violationTarget(model, rule: cs, brain)?.path, "core/engine.cpp")
        XCTAssertEqual(Strata.violationTarget(model, rule: cs, brain)?.line, 40)
    }

    func testDetailsFacts() {
        let model = Strata.buildModel(StrataGolden.rich(), depth: 1)
        let core = model.report.graph.nodes.firstIndex { $0.path == "core" } ?? 0
        let facts = Strata.moduleFacts(model, core)
        XCTAssertEqual(facts.map(\.label), ["Lines of code", "Files", "Functions", "Complexity", "Types", "Health", "Afferent Ca", "Efferent Ce",
                                            "Instability I", "Abstractness A", "Distance D", "Cohesion", "Commits", "Interest"])
        XCTAssertEqual(facts[0].value, "2,700")
        XCTAssertEqual(facts[4].value, "0 (0 abstract)")
        XCTAssertEqual(Strata.moduleCycleNote(model, core), "In a dependency cycle with 1 other modules.")
        let neighbours = Strata.moduleNeighbours(model, core)
        XCTAssertEqual(neighbours.uses.map { model.report.graph.nodes[$0.module].path }, ["util", "app"])
        XCTAssertEqual(neighbours.uses.map(\.weight), [2, 1])
        XCTAssertEqual(neighbours.uses.map(\.cycle), [false, true])
        XCTAssertEqual(neighbours.usedBy.map { model.report.graph.nodes[$0.module].path }, ["app", "api"])
        let ranked = Strata.moduleFilesByDebt(model, core)
        XCTAssertEqual(ranked.first, model.snapshot.indexOf("core/engine.cpp"))

        let engine = model.snapshot.indexOf("core/engine.cpp")
        let fileFacts = Strata.fileFacts(model, engine)
        XCTAssertEqual(fileFacts.first { $0.label == "Lines of code" }?.value, "2,500 (2 comment lines)")
        XCTAssertEqual(fileFacts.first { $0.label == "Commits" }?.value, "40 (0 recent, 0 fixes)")
        XCTAssertEqual(fileFacts.first { $0.label == "Authors" }?.value, "2 · main Author One (96 %)")
        XCTAssertEqual(fileFacts.first { $0.label == "Duplicated" }?.value, "120 lines")
        let refs = Strata.fileViolations(model, engine)
        XCTAssertFalse(refs.isEmpty)
        for k in 1..<refs.count {
            XCTAssertLessThanOrEqual(model.report.violation(refs[k - 1]).severity, model.report.violation(refs[k]).severity)
        }

        let tiles = Strata.dashboardTiles(model)
        XCTAssertEqual(tiles.map(\.label), ["Lines of code", "Files", "Modules", "Types", "Functions", "Average complexity", "Max complexity",
                                            "Comments", "Duplicated", "Rule violations"])
        XCTAssertEqual(tiles[0].value, "3,920")
        XCTAssertEqual(tiles[2].note, "2 in cycles")
        XCTAssertEqual(tiles[6].note, "Engine::run")
        XCTAssertEqual(Strata.topRules(model.report).count, 6)
        XCTAssertEqual(Strata.topModules(model.report).count, 4)
        XCTAssertLessThanOrEqual(Strata.topFiles(model.report).count, 10)
    }

    func testMatrixHelpersAndSearch() {
        let model = Strata.buildModel(StrataGolden.rich(), depth: 1)
        let graph = model.report.graph
        let (weights, maxWeight) = Strata.matrixWeights(graph)
        XCTAssertEqual(weights.count, graph.edges.count)
        XCTAssertEqual(maxWeight, graph.edges.map(\.weight).max())
        let app = graph.nodes.firstIndex { $0.path == "app" } ?? 0
        let core = graph.nodes.firstIndex { $0.path == "core" } ?? 0
        let direction = Strata.cellDirection(model, from: app, to: core)
        XCTAssertTrue(direction.exists)
        XCTAssertEqual(direction.edges.count, direction.weight)
        XCTAssertEqual(direction.edges.first.map { model.snapshot.files[$0.from].path.hasPrefix("app/") }, true)
        let missing = Strata.cellDirection(model, from: app, to: app)
        XCTAssertFalse(missing.exists)
        XCTAssertEqual(Strata.matrixLayerBreaks(model).allSatisfy { $0 > 0 && $0 < model.order.count }, true)
        let hits = Strata.searchHits(model, "eng")
        XCTAssertEqual(hits.files, [model.snapshot.indexOf("core/engine.cpp")])
        XCTAssertTrue(hits.modules.isEmpty)
        XCTAssertTrue(Strata.moduleMatches(model, core, "engine"))
        XCTAssertFalse(Strata.moduleMatches(model, app, "engine"))
        XCTAssertEqual(Strata.moduleCaption(graph.nodes[core]), "2,700 LOC · 2 files")
        let labels = Strata.scatterLabels(model, selected: -1)
        XCTAssertEqual(Set(labels).count, graph.nodes.count)
    }
}
