import Foundation
import XCTest
@testable import FMCodeInsight

final class EmberCheck {
    var problems: [String] = []
    var checks = 0

    func num(_ value: Any?) -> Double? { (value as? NSNumber)?.doubleValue }

    func close(_ label: String, _ actual: Double, _ expected: Any?, tolerance: Double = 1e-9) {
        checks += 1
        guard let want = num(expected) else { return problems.append("\(label): missing expected") }
        let scale = max(1, abs(want))
        if abs(actual - want) > tolerance * scale { problems.append("\(label): got \(actual), want \(want)") }
    }

    func int(_ label: String, _ actual: Int, _ expected: Any?) {
        checks += 1
        guard let want = num(expected) else { return problems.append("\(label): missing expected") }
        if Double(actual) != want { problems.append("\(label): got \(actual), want \(want)") }
    }

    func bool(_ label: String, _ actual: Bool, _ expected: Any?) {
        checks += 1
        guard let want = expected as? Bool else { return problems.append("\(label): missing expected") }
        if actual != want { problems.append("\(label): got \(actual), want \(want)") }
    }

    func text(_ label: String, _ actual: String, _ expected: Any?) {
        checks += 1
        guard let want = expected as? String else { return problems.append("\(label): missing expected") }
        if actual != want { problems.append("\(label): got \(actual.debugDescription), want \(want.debugDescription)") }
    }

    func texts(_ label: String, _ actual: [String], _ expected: Any?) {
        checks += 1
        guard let want = expected as? [String] else { return problems.append("\(label): missing expected") }
        if actual != want { problems.append("\(label): got \(actual), want \(want)") }
    }

    func color(_ label: String, _ actual: EmberColor?, _ expected: Any?) {
        checks += 1
        guard let want = expected as? [Any] else {
            if actual != nil && !(expected is NSNull) { problems.append("\(label): missing expected") }
            if actual != nil && expected is NSNull { problems.append("\(label): got \(actual!), want no colour") }
            return
        }
        guard let actual else { return problems.append("\(label): got no colour, want \(want)") }
        let channels = [actual.r, actual.g, actual.b, actual.a]
        for (k, value) in channels.enumerated() {
            guard let w = num(want[k]) else { return problems.append("\(label): bad expected") }
            if abs(value - w) > 1e-6 { return problems.append("\(label)[\(k)]: got \(value), want \(w)") }
        }
    }

    func count(_ label: String, _ actual: Int, _ expected: [Any]?) -> Bool {
        checks += 1
        guard let expected else {
            problems.append("\(label): missing expected")
            return false
        }
        if actual != expected.count {
            problems.append("\(label): count \(actual), want \(expected.count)")
            return false
        }
        return true
    }
}

final class EmberGoldenTests: XCTestCase {
    static let viewportWidth = 390.0
    static let viewportHeight = 640.0

    func golden() throws -> [String: Any] {
        let url = Fixture.root.appendingPathComponent("ember.json")
        guard FileManager.default.fileExists(atPath: url.path) else { throw XCTSkip("native/fixtures/insight/ember.json is missing") }
        return try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any] ?? [:]
    }

    func snapshots() throws -> (base: InsightSnapshot, history: InsightSnapshot) {
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
        return (base, history)
    }

    static func synthetic(_ count: Int) -> InsightSnapshot {
        var s = InsightSnapshot()
        s.name = "big"
        s.git = true
        s.commitsScanned = 1000
        var seed: UInt32 = 7
        func next() -> UInt32 {
            seed = seed &* 1664525 &+ 1013904223
            return seed >> 8
        }
        for i in 0..<count {
            let m = next() % 12
            let p = next() % 9
            let q = next() % 15
            var f = InsightFile(path: "m\(m)/p\(p)/q\(q)/f\(i).cpp")
            f.language = "C++"
            f.codeLines = Int(5 + next() % 3000)
            f.health = 1 + Double(next() % 90) / 10.0
            f.commits = Int(next() % 80)
            f.hotspot = Double(next() % 1000) / 1000.0
            s.byPath[f.path] = i
            s.files.append(f)
        }
        return s
    }

    func finish(_ check: EmberCheck, minimum: Int) {
        XCTAssertGreaterThanOrEqual(check.checks, minimum)
        if !check.problems.isEmpty {
            XCTFail("\(check.problems.count) of \(check.checks) Ember golden checks differ:\n" + check.problems.prefix(40).joined(separator: "\n"))
        }
    }

    func testSnapshotAndHistoryFlagsMatchTheDesktop() throws {
        let g = try golden()
        let (base, s) = try snapshots()
        let check = EmberCheck()
        check.text("name", s.name, g["name"])
        check.texts("files", s.files.map(\.path), g["files"])
        check.bool("hasHistory", Ember.hasHistory(s), g["hasHistory"])
        check.bool("hasHistoryBase", Ember.hasHistory(base), g["hasHistoryBase"])
        var plain = base
        plain.git = false
        check.bool("hasHistoryPlain", Ember.hasHistory(plain), g["hasHistoryPlain"])
        finish(check, minimum: 5)
    }

    func testPackingMatchesTheDesktop() throws {
        let g = try golden()
        let cases = g["packing"] as? [[String: Any]] ?? []
        let check = EmberCheck()
        var seed: UInt32 = 5
        for (n, count) in [1, 2, 3, 4, 7, 25, 120].enumerated() {
            var circles: [EmberCircle] = []
            for _ in 0..<count {
                seed = seed &* 1664525 &+ 1013904223
                circles.append(EmberCircle(r: 1 + Double((seed >> 8) % 4000) / 100))
            }
            guard n < cases.count else {
                check.problems.append("packing \(count): missing")
                continue
            }
            let item = cases[n]
            let input = item["input"] as? [Any] ?? []
            if check.count("packing \(count) input", circles.count, input) {
                for (i, c) in circles.enumerated() { check.close("packing \(count) input \(i)", c.r, input[i]) }
            }
            let radius = Ember.packSiblings(&circles)
            let e = Ember.encloseCircles(circles)
            check.close("packing \(count) radius", radius, item["radius"])
            let output = item["output"] as? [[Any]] ?? []
            if check.count("packing \(count) output", circles.count, output) {
                for (i, c) in circles.enumerated() {
                    check.close("packing \(count) x\(i)", c.x, output[i][0])
                    check.close("packing \(count) y\(i)", c.y, output[i][1])
                    check.close("packing \(count) r\(i)", c.r, output[i][2])
                }
            }
            let enclose = item["enclose"] as? [Any] ?? []
            if enclose.count == 3 {
                check.close("packing \(count) enclose x", e.x, enclose[0])
                check.close("packing \(count) enclose y", e.y, enclose[1])
                check.close("packing \(count) enclose r", e.r, enclose[2])
            } else {
                check.problems.append("packing \(count) enclose: missing")
            }
        }
        finish(check, minimum: 300)
    }

    func testLayoutMatchesTheDesktop() throws {
        let g = try golden()
        let (_, s) = try snapshots()
        let layout = Ember.buildLayout(s)
        let want = g["layout"] as? [String: Any] ?? [:]
        let check = EmberCheck()
        check.int("maxDepth", layout.maxDepth, want["maxDepth"])
        let nodeOfFile = want["nodeOfFile"] as? [Any] ?? []
        if check.count("nodeOfFile", layout.nodeOfFile.count, nodeOfFile) {
            for (i, n) in layout.nodeOfFile.enumerated() { check.int("nodeOfFile \(i)", n, nodeOfFile[i]) }
        }
        let nodes = want["nodes"] as? [[String: Any]] ?? []
        if check.count("nodes", layout.nodes.count, nodes) {
            for (i, n) in layout.nodes.enumerated() {
                let w = nodes[i]
                let label = "node \(i) \(n.path)"
                check.text("\(label) name", n.name, w["name"])
                check.text("\(label) path", n.path, w["path"])
                check.int("\(label) parent", n.parent, w["parent"])
                check.int("\(label) depth", n.depth, w["depth"])
                check.text("\(label) file", n.file >= 0 ? s.files[n.file].path : "", w["file"])
                let children = (w["children"] as? [Any] ?? []).compactMap { ($0 as? NSNumber)?.intValue }
                check.checks += 1
                if children != n.children { check.problems.append("\(label) children: got \(n.children), want \(children)") }
                check.close("\(label) x", n.x, w["x"])
                check.close("\(label) y", n.y, w["y"])
                check.close("\(label) r", n.r, w["r"])
                check.close("\(label) codeLines", Double(n.codeLines), w["codeLines"])
                check.int("\(label) files", n.files, w["files"])
                check.close("\(label) health", n.health, w["health"])
                check.int("\(label) commits", n.commits, w["commits"])
            }
        }
        finish(check, minimum: 900)
    }

    func testViewMathMatchesTheDesktop() throws {
        let g = try golden()
        let (_, s) = try snapshots()
        let layout = Ember.buildLayout(s)
        let views = g["views"] as? [String: Any] ?? [:]
        let w = Self.viewportWidth
        let h = Self.viewportHeight
        let check = EmberCheck()
        let viewport = views["viewport"] as? [Any] ?? []
        check.close("viewport width", w, viewport.first)
        check.close("viewport height", h, viewport.last)
        let fits = views["fit"] as? [[String: Any]] ?? []
        if check.count("fit", layout.nodes.count, fits) {
            for (i, node) in layout.nodes.enumerated() {
                let fit = Ember.fitTransform(node, width: w, height: h)
                check.int("fit \(i) node", i, fits[i]["node"])
                check.close("fit \(i) cx", fit.cx, fits[i]["cx"])
                check.close("fit \(i) cy", fit.cy, fits[i]["cy"])
                check.close("fit \(i) k", fit.k, fits[i]["k"])
            }
        }
        let readable = views["readable"] as? [[Any]] ?? []
        if check.count("readable", layout.nodes.count, readable) {
            for i in layout.nodes.indices {
                check.int("readable \(i)", Ember.readableFocus(layout, i, width: w, height: h), readable[i][1])
            }
        }
        let interpolate = views["interpolate"] as? [[String: Any]] ?? []
        for (i, item) in interpolate.enumerated() {
            guard let from = (item["from"] as? NSNumber)?.intValue, let to = (item["to"] as? NSNumber)?.intValue,
                  let zoom = (item["zoom"] as? NSNumber)?.doubleValue, let t = (item["t"] as? NSNumber)?.doubleValue else {
                check.problems.append("interpolate \(i): malformed")
                continue
            }
            let a = Ember.fitTransform(layout.nodes[from], width: w, height: h)
            var b = Ember.fitTransform(layout.nodes[to], width: w, height: h)
            b.k *= zoom == 3 ? 3 : 1
            let v = Ember.interpolateView(a, b, t, width: w, height: h)
            check.close("interpolate \(from)->\(to) t\(t) cx", v.cx, item["cx"])
            check.close("interpolate \(from)->\(to) t\(t) cy", v.cy, item["cy"])
            check.close("interpolate \(from)->\(to) t\(t) k", v.k, item["k"])
        }
        check.int("interpolate cases", interpolate.count, 28)
        for item in views["ease"] as? [[Any]] ?? [] {
            guard let t = check.num(item[0]) else { continue }
            check.close("ease \(t)", Ember.easeInOut(t), item[1])
        }
        let probes = g["nodeAt"] as? [[Any]] ?? []
        check.int("nodeAt probes", probes.count, layout.nodes.count * 3)
        for probe in probes {
            guard let x = check.num(probe[0]), let y = check.num(probe[1]) else { continue }
            check.int("nodeAt \(x),\(y)", layout.nodeAt(x: x, y: y), probe[2])
        }
        finish(check, minimum: 600)
    }

    func targetsMatch(_ check: EmberCheck, _ label: String, _ s: InsightSnapshot, _ targets: [EmberTarget], _ expected: Any?) {
        let want = expected as? [[String: Any]] ?? []
        guard check.count("\(label) targets", targets.count, expected as? [Any]) else { return }
        for (i, t) in targets.enumerated() {
            check.text("\(label) target \(i) path", s.files[t.file].path, want[i]["path"])
            check.int("\(label) target \(i) rank", t.rank, want[i]["rank"])
            check.close("\(label) target \(i) score", t.score, want[i]["score"])
            check.texts("\(label) target \(i) reasons", t.reasons, want[i]["reasons"])
        }
    }

    func testCouplingTargetsAndRankingsMatchTheDesktop() throws {
        let g = try golden()
        let (_, s) = try snapshots()
        let coupling = Ember.buildCouplingIndex(s)
        let check = EmberCheck()
        let want = g["coupling"] as? [[[String: Any]]] ?? []
        if check.count("coupling", coupling.count, want) {
            for (i, refs) in coupling.enumerated() {
                guard check.count("coupling \(i)", refs.count, want[i]) else { continue }
                for (k, ref) in refs.enumerated() {
                    check.text("coupling \(i).\(k) path", s.files[ref.file].path, want[i][k]["path"])
                    check.int("coupling \(i).\(k) shared", ref.shared, want[i][k]["shared"])
                    check.close("coupling \(i).\(k) degree", ref.degree, want[i][k]["degree"])
                }
            }
        }
        check.texts("topAuthors", Ember.topAuthors(s, limit: 0), g["topAuthors"])
        check.texts("topLanguages", Ember.topLanguages(s, limit: 0), g["topLanguages"])
        targetsMatch(check, "all", s, Ember.refactoringTargets(s, coupling, limit: 0), g["targets"])
        finish(check, minimum: 60)
    }

    func coloringMatches(_ check: EmberCheck, _ s: InsightSnapshot, _ c: EmberColoring, _ w: [String: Any]) {
        let label = c.tab.title
        check.text("\(label) tab", c.tab.title, w["tab"])
        let files = w["files"] as? [[String: Any]] ?? []
        if check.count("\(label) files", c.files.count, files) {
            for (i, f) in c.files.enumerated() {
                let fl = "\(label) \(s.files[i].path)"
                check.color("\(fl) color", f.color, files[i]["color"])
                check.bool("\(fl) dimmed", f.dimmed, files[i]["dimmed"])
                check.close("\(fl) glow", f.glow, files[i]["glow"])
                check.int("\(fl) rank", f.rank, files[i]["rank"])
                check.bool("\(fl) marked", f.marked, files[i]["marked"])
            }
        }
        let legend = w["legend"] as? [[String: Any]] ?? []
        if check.count("\(label) legend", c.legend.count, legend) {
            for (i, e) in c.legend.enumerated() {
                check.text("\(label) legend \(i) label", e.label, legend[i]["label"])
                check.color("\(label) legend \(i) color", e.color, legend[i]["color"])
                check.bool("\(label) legend \(i) ring", e.ring, legend[i]["ring"])
            }
        }
        check.bool("\(label) scale", c.scale, w["scale"])
        check.color("\(label) scaleLow", c.scaleLow, w["scaleLow"])
        check.color("\(label) scaleHigh", c.scaleHigh, w["scaleHigh"])
        check.text("\(label) scaleLowLabel", c.scaleLowLabel, w["scaleLowLabel"])
        check.text("\(label) scaleHighLabel", c.scaleHighLabel, w["scaleHighLabel"])
        check.text("\(label) note", c.note, w["note"])
        targetsMatch(check, label, s, c.targets, w["targets"])
        check.int("\(label) glowing", c.glowing, w["glowing"])
        check.int("\(label) matching", c.matching, w["matching"])
    }

    func testEveryTabColouringMatchesTheDesktop() throws {
        let g = try golden()
        let (base, s) = try snapshots()
        let coupling = Ember.buildCouplingIndex(s)
        let check = EmberCheck()
        let tabs = g["tabs"] as? [[String: Any]] ?? []
        if check.count("tabs", EmberTab.allCases.count, tabs) {
            for (i, tab) in EmberTab.allCases.enumerated() {
                coloringMatches(check, s, Ember.colorize(s, tab: tab, coupling: coupling), tabs[i])
            }
        }
        let plain = g["noHistory"] as? [[String: Any]] ?? []
        if check.count("noHistory", EmberTab.allCases.count, plain) {
            for (i, tab) in EmberTab.allCases.enumerated() {
                let c = Ember.colorize(base, tab: tab, coupling: Ember.buildCouplingIndex(base))
                check.text("noHistory \(i) tab", tab.title, plain[i]["tab"])
                check.text("noHistory \(tab.title) note", c.note, plain[i]["note"])
                check.int("noHistory \(tab.title) matching", c.matching, plain[i]["matching"])
                check.int("noHistory \(tab.title) legend", c.legend.count, plain[i]["legend"])
            }
        }
        finish(check, minimum: 1600)
    }

    func testFiltersMatchTheDesktop() throws {
        let g = try golden()
        let (_, s) = try snapshots()
        let coupling = Ember.buildCouplingIndex(s)
        let check = EmberCheck()
        let cases = g["filtered"] as? [[String: Any]] ?? []
        check.int("filter cases", cases.count, 14)
        for (i, item) in cases.enumerated() {
            let f = item["filters"] as? [String: Any] ?? [:]
            let filters = EmberFilters(minHealth: check.num(f["minHealth"]) ?? 1, maxHealth: check.num(f["maxHealth"]) ?? 10,
                                       minCommits: Int(check.num(f["minCommits"]) ?? 0), author: f["author"] as? String ?? "",
                                       search: f["search"] as? String ?? "")
            let tab = EmberTab.allCases.first { $0.title == item["tab"] as? String } ?? .codeHealth
            let c = Ember.colorize(s, tab: tab, filters: filters, coupling: coupling)
            let dimmed = s.files.indices.filter { c.files[$0].dimmed }.map { s.files[$0].path }
            check.bool("filter \(i) active", filters.active, item["active"])
            check.texts("filter \(i) \(tab.title) dimmed", dimmed, item["dimmed"])
            check.int("filter \(i) \(tab.title) matching", c.matching, item["matching"])
        }
        let authors = Ember.topAuthors(s, limit: 0)
        func author(_ index: Int) -> Any? { index < cases.count ? (cases[index]["filters"] as? [String: Any])?["author"] : nil }
        check.text("filter author 1", authors.first ?? "", author(6))
        check.text("filter author 3", authors.count > 2 ? authors[2] : "", author(8))
        finish(check, minimum: 40)
    }

    func testSummaryTextAndSyntheticLayoutMatchTheDesktop() throws {
        let g = try golden()
        let (_, s) = try snapshots()
        let check = EmberCheck()
        let summary = Ember.summarize(s)
        let want = g["summary"] as? [String: Any] ?? [:]
        check.int("summary unhealthy", summary.unhealthy, want["unhealthy"])
        check.int("summary problematic", summary.problematic, want["problematic"])
        check.int("summary healthy", summary.healthy, want["healthy"])
        check.close("summary unhealthyLines", Double(summary.unhealthyLines), want["unhealthyLines"])
        check.close("summary problematicLines", Double(summary.problematicLines), want["problematicLines"])
        check.close("summary healthyLines", Double(summary.healthyLines), want["healthyLines"])
        check.text("summary topHotspot", summary.topHotspot >= 0 ? s.files[summary.topHotspot].path : "", want["topHotspot"])
        check.text("summary text", summary.text, want["text"])

        let text = g["text"] as? [String: Any] ?? [:]
        for item in text["span"] as? [[Any]] ?? [] {
            let days = Int(check.num(item[0]) ?? 0)
            check.text("span \(days)", Ember.spanText(days), item[1])
        }
        for item in text["ago"] as? [[Any]] ?? [] {
            let when = Int64(check.num(item[0]) ?? 0)
            let now = Int64(check.num(item[1]) ?? 0)
            check.text("ago \(when)", Ember.agoText(when, now: now), item[2])
        }
        for item in text["percent"] as? [[Any]] ?? [] {
            let share = check.num(item[0]) ?? 0
            check.text("percent \(share)", Ember.percentText(share), item[1])
        }
        for item in text["names"] as? [[Any]] ?? [] {
            let path = item[0] as? String ?? ""
            check.text("fileName \(path)", Ember.fileNameOf(path), item[1])
            check.text("folder \(path)", Ember.folderOf(path), item[2])
        }

        let big = Self.synthetic(400)
        let layout = Ember.buildLayout(big)
        let synthetic = g["synthetic"] as? [String: Any] ?? [:]
        check.int("synthetic files", big.files.count, synthetic["files"])
        check.int("synthetic nodes", layout.nodes.count, synthetic["nodes"])
        check.int("synthetic maxDepth", layout.maxDepth, synthetic["maxDepth"])
        check.close("synthetic rootRadius", layout.nodes.first?.r ?? 0, synthetic["rootRadius"])
        check.close("synthetic sumX", layout.nodes.reduce(0) { $0 + $1.x }, synthetic["sumX"], tolerance: 1e-8)
        check.close("synthetic sumY", layout.nodes.reduce(0) { $0 + $1.y }, synthetic["sumY"], tolerance: 1e-8)
        check.close("synthetic sumR", layout.nodes.reduce(0) { $0 + $1.r }, synthetic["sumR"], tolerance: 1e-8)
        finish(check, minimum: 50)
    }
}
