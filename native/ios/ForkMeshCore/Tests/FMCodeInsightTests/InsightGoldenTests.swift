import Foundation
import XCTest
@testable import FMCodeInsight

struct MemorySource: InsightSource {
    var name = "repo"
    var description = "memory:repo"
    var head = ""
    var branch = ""
    var git = true
    var exists = true
    var files: [String: [UInt8]] = [:]
    var order: [String] = []
    var commits: [InsightCommit]?

    init(files: [(String, [UInt8])], commits: [InsightCommit]? = nil, git: Bool = true) {
        for (path, bytes) in files {
            self.files[path] = bytes
            order.append(path)
        }
        self.commits = commits
        self.git = git
    }

    func entries() throws -> [InsightEntry] {
        order.map { InsightEntry(path: $0, size: -1) }
    }

    func read(_ path: String, limit: Int64) throws -> [UInt8]? {
        guard let bytes = files[path] else { return nil }
        return Int64(bytes.count) > limit ? [] : bytes
    }

    func history(_ options: InsightOptions, cancel: @escaping () -> Bool) throws -> [InsightCommit]? {
        commits
    }
}

enum Fixture {
    static var root: URL {
        if let custom = ProcessInfo.processInfo.environment["FM_INSIGHT_FIXTURE"] { return URL(fileURLWithPath: custom) }
        var url = URL(fileURLWithPath: #filePath)
        while url.path != "/" {
            url.deleteLastPathComponent()
            let candidate = url.appendingPathComponent("fixtures/insight")
            if FileManager.default.fileExists(atPath: candidate.appendingPathComponent("expected.json").path) {
                return candidate
            }
        }
        return URL(fileURLWithPath: "/missing")
    }

    static func repoFiles() throws -> [(String, [UInt8])] {
        let repo = root.appendingPathComponent("repo")
        let base = repo.standardizedFileURL.path + "/"
        var files: [(String, [UInt8])] = []
        guard let walker = FileManager.default.enumerator(at: repo, includingPropertiesForKeys: [.isRegularFileKey]) else { return [] }
        for case let url as URL in walker {
            guard (try? url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile) == true else { continue }
            let path = String(url.standardizedFileURL.path.dropFirst(base.count))
            files.append((path, Array(try Data(contentsOf: url))))
        }
        return files
    }

    static func expected() throws -> [String: Any] {
        let data = try Data(contentsOf: root.appendingPathComponent("expected.json"))
        return try JSONSerialization.jsonObject(with: data) as? [String: Any] ?? [:]
    }

    static func history() throws -> [InsightCommit] {
        Insight.parseGitLog(Array(try Data(contentsOf: root.appendingPathComponent("history.log"))))
    }
}

final class Diff {
    var problems: [String] = []
    var checks = 0

    func int(_ value: Any?) -> Int? { (value as? NSNumber)?.intValue }

    func same(_ label: String, _ actual: Int, _ expected: Any?) {
        checks += 1
        guard let want = int(expected) else { return problems.append("\(label): missing expected") }
        if actual != want { problems.append("\(label): got \(actual), want \(want)") }
    }

    func same(_ label: String, _ actual: Int64, _ expected: Any?) {
        checks += 1
        guard let want = (expected as? NSNumber)?.int64Value else { return problems.append("\(label): missing expected") }
        if actual != want { problems.append("\(label): got \(actual), want \(want)") }
    }

    func same(_ label: String, _ actual: Double, _ expected: Any?) {
        checks += 1
        guard let want = (expected as? NSNumber)?.doubleValue else { return problems.append("\(label): missing expected") }
        if abs(actual - want) > 1e-9 * max(1, abs(want)) { problems.append("\(label): got \(actual), want \(want)") }
    }

    func same(_ label: String, _ actual: Bool, _ expected: Any?) {
        checks += 1
        guard let want = (expected as? NSNumber)?.boolValue else { return problems.append("\(label): missing expected") }
        if actual != want { problems.append("\(label): got \(actual), want \(want)") }
    }

    func same(_ label: String, _ actual: String, _ expected: Any?) {
        checks += 1
        guard let want = expected as? String else { return problems.append("\(label): missing expected") }
        if actual != want { problems.append("\(label): got \(actual.debugDescription), want \(want.debugDescription)") }
    }

    func same(_ label: String, _ actual: [String], _ expected: Any?) {
        checks += 1
        guard let want = expected as? [String] else { return problems.append("\(label): missing expected") }
        if actual != want { problems.append("\(label): got \(actual), want \(want)") }
    }
}

final class InsightGoldenTests: XCTestCase {
    static var now: Int64 { ((try? Fixture.expected())?["now"] as? NSNumber)?.int64Value ?? 1_792_000_000 }

    func runGolden() throws -> (base: InsightSnapshot, history: InsightSnapshot) {
        let source = MemorySource(files: try Fixture.repoFiles(), commits: try Fixture.history())
        var options = InsightOptions()
        options.history = false
        let base = try Insight.analyze(source: source, options: options, now: Self.now)
        var history = base
        Insight.applyHistory(&history, try Fixture.history(), InsightOptions(), now: Self.now)
        for i in history.files.indices {
            let result = Insight.computeHealth(history.files[i])
            history.files[i].health = result.health
            history.files[i].healthIssues = result.issues
        }
        Insight.computeHotspots(&history)
        return (base, history)
    }

    func paths(_ snapshot: InsightSnapshot, _ indexes: [Int]) -> [String] {
        indexes.map { snapshot.files[$0].path }.sorted(by: U.less)
    }

    func compare(_ snapshot: InsightSnapshot, _ expected: [String: Any], _ label: String, _ diff: Diff) {
        let files = expected["files"] as? [[String: Any]] ?? []
        diff.same("\(label).files.count", snapshot.files.count, files.count as NSNumber)
        for (i, want) in files.enumerated() where i < snapshot.files.count {
            let file = snapshot.files[i]
            let p = "\(label).\(want["path"] as? String ?? "?")"
            diff.same("\(p).path", file.path, want["path"])
            diff.same("\(p).language", file.language, want["language"])
            diff.same("\(p).bytes", file.bytes, want["bytes"])
            diff.same("\(p).lines", file.lines, want["lines"])
            diff.same("\(p).codeLines", file.codeLines, want["codeLines"])
            diff.same("\(p).commentLines", file.commentLines, want["commentLines"])
            diff.same("\(p).blankLines", file.blankLines, want["blankLines"])
            diff.same("\(p).types", file.types, want["types"])
            diff.same("\(p).abstractTypes", file.abstractTypes, want["abstractTypes"])
            diff.same("\(p).complexity", file.complexity, want["complexity"])
            diff.same("\(p).maxComplexity", file.maxComplexity, want["maxComplexity"])
            diff.same("\(p).maxNesting", file.maxNesting, want["maxNesting"])
            diff.same("\(p).duplicatedLines", file.duplicatedLines, want["duplicatedLines"])
            diff.same("\(p).test", file.test, want["test"])
            let functions = want["functions"] as? [[String: Any]] ?? []
            diff.same("\(p).functions.count", file.functions.count, functions.count as NSNumber)
            for (k, fnWant) in functions.enumerated() where k < file.functions.count {
                let fn = file.functions[k]
                let f = "\(p).fn[\(k)]"
                diff.same("\(f).name", fn.name, fnWant["name"])
                diff.same("\(f).line", fn.line, fnWant["line"])
                diff.same("\(f).length", fn.length, fnWant["length"])
                diff.same("\(f).complexity", fn.complexity, fnWant["complexity"])
                diff.same("\(f).nesting", fn.nesting, fnWant["nesting"])
                diff.same("\(f).parameters", fn.parameters, fnWant["parameters"])
            }
            diff.same("\(p).imports", file.imports, want["imports"])
            diff.same("\(p).externalImports", file.externalImports, want["externalImports"])
            diff.same("\(p).declares", file.declares, want["declares"])
            diff.same("\(p).references", file.references, want["references"])
            diff.same("\(p).dependsOn", paths(snapshot, file.dependsOn), want["dependsOn"])
            diff.same("\(p).dependedOnBy", paths(snapshot, file.dependedOnBy), want["dependedOnBy"])
            diff.same("\(p).commits", file.commits, want["commits"])
            diff.same("\(p).recentCommits", file.recentCommits, want["recentCommits"])
            diff.same("\(p).authors", file.authors, want["authors"])
            diff.same("\(p).fixCommits", file.fixCommits, want["fixCommits"])
            diff.same("\(p).linesAdded", file.linesAdded, want["linesAdded"])
            diff.same("\(p).linesDeleted", file.linesDeleted, want["linesDeleted"])
            diff.same("\(p).lastChanged", file.lastChanged, want["lastChanged"])
            diff.same("\(p).firstChanged", file.firstChanged, want["firstChanged"])
            diff.same("\(p).mainAuthor", file.mainAuthor, want["mainAuthor"])
            diff.same("\(p).mainAuthorShare", file.mainAuthorShare, want["mainAuthorShare"])
            diff.same("\(p).formerAuthorShare", file.formerAuthorShare, want["formerAuthorShare"])
            let authors = want["topAuthors"] as? [[Any]] ?? []
            diff.same("\(p).topAuthors.count", file.topAuthors.count, authors.count as NSNumber)
            for (k, pair) in authors.enumerated() where k < file.topAuthors.count && pair.count == 2 {
                diff.same("\(p).topAuthors[\(k)].name", file.topAuthors[k].name, pair[0])
                diff.same("\(p).topAuthors[\(k)].commits", file.topAuthors[k].commits, pair[1])
            }
            diff.same("\(p).health", file.health, want["health"])
            diff.same("\(p).healthIssues", file.healthIssues, want["healthIssues"])
            diff.same("\(p).healthBand", Insight.healthBand(file.health).name, want["healthBand"])
            diff.same("\(p).hotspot", file.hotspot, want["hotspot"])
            diff.same("\(p).inCycle", file.cycle >= 0, want["inCycle"])
        }
        let cycles = snapshot.cycles.map { paths(snapshot, $0) }
        let wantCycles = expected["cycles"] as? [[String]] ?? []
        if cycles != wantCycles { diff.problems.append("\(label).cycles: got \(cycles), want \(wantCycles)") }
        for (k, cycle) in snapshot.cycles.enumerated() {
            for member in cycle where snapshot.files[member].cycle != k {
                diff.problems.append("\(label).cycle index of \(snapshot.files[member].path)")
            }
        }
        let couplings = Set(snapshot.couplings.map { "\(snapshot.files[$0.a].path)\n\(snapshot.files[$0.b].path)\n\($0.shared)\n\(String(format: "%.9f", $0.degree))" })
        let wantCouplings = Set((expected["couplings"] as? [[String: Any]] ?? []).map { c -> String in
            let degree = (c["degree"] as? NSNumber)?.doubleValue ?? -1
            return "\(c["a"] as? String ?? "")\n\(c["b"] as? String ?? "")\n\(diff.int(c["shared"]) ?? -1)\n\(String(format: "%.9f", degree))"
        })
        if couplings != wantCouplings { diff.problems.append("\(label).couplings: got \(couplings.sorted()), want \(wantCouplings.sorted())") }
        let roots = expected["packageRoots"] as? [String: String] ?? [:]
        if snapshot.packageRoots != roots { diff.problems.append("\(label).packageRoots: got \(snapshot.packageRoots), want \(roots)") }
        diff.same("\(label).authors", snapshot.authors, expected["authors"])
        diff.same("\(label).formerAuthors", snapshot.formerAuthors, expected["formerAuthors"])
        diff.same("\(label).commitsScanned", snapshot.commitsScanned, expected["commitsScanned"])
        diff.same("\(label).historyStart", snapshot.historyStart, expected["historyStart"])
        diff.same("\(label).historyEnd", snapshot.historyEnd, expected["historyEnd"])
        diff.same("\(label).historyDays", snapshot.historyDays, expected["historyDays"])
        diff.same("\(label).skippedFiles", snapshot.skippedFiles, expected["skippedFiles"])
        diff.same("\(label).totalCodeLines", snapshot.totalCodeLines, expected["totalCodeLines"])
        diff.same("\(label).totalComplexity", snapshot.totalComplexity, expected["totalComplexity"])
    }

    func compareModules(_ snapshot: InsightSnapshot, depth: Int, _ expected: [String: Any], _ label: String, _ diff: Diff) {
        let graph = Insight.buildModuleGraph(snapshot, depth: depth)
        diff.same("\(label).depth", graph.depth, expected["depth"])
        let nodes = expected["nodes"] as? [[String: Any]] ?? []
        diff.same("\(label).nodes.count", graph.nodes.count, nodes.count as NSNumber)
        for (i, want) in nodes.enumerated() where i < graph.nodes.count {
            let node = graph.nodes[i]
            let p = "\(label).\(want["path"] as? String ?? "?")"
            diff.same("\(p).path", node.path, want["path"])
            diff.same("\(p).name", node.name, want["name"])
            diff.same("\(p).files", paths(snapshot, node.files), want["files"])
            diff.same("\(p).codeLines", node.codeLines, want["codeLines"])
            diff.same("\(p).complexity", node.complexity, want["complexity"])
            diff.same("\(p).functions", node.functions, want["functions"])
            diff.same("\(p).types", node.types, want["types"])
            diff.same("\(p).abstractTypes", node.abstractTypes, want["abstractTypes"])
            diff.same("\(p).afferent", node.afferent, want["afferent"])
            diff.same("\(p).efferent", node.efferent, want["efferent"])
            diff.same("\(p).internalEdges", node.internalEdges, want["internalEdges"])
            diff.same("\(p).instability", node.instability, want["instability"])
            diff.same("\(p).abstractness", node.abstractness, want["abstractness"])
            diff.same("\(p).distance", node.distance, want["distance"])
            diff.same("\(p).cohesion", node.cohesion, want["cohesion"])
            diff.same("\(p).health", node.health, want["health"])
            diff.same("\(p).commits", node.commits, want["commits"])
            diff.same("\(p).inCycle", node.cycle >= 0, want["inCycle"])
            diff.same("\(p).layer", node.layer, want["layer"])
        }
        let edges = expected["edges"] as? [[String: Any]] ?? []
        diff.same("\(label).edges.count", graph.edges.count, edges.count as NSNumber)
        for (i, want) in edges.enumerated() where i < graph.edges.count {
            let edge = graph.edges[i]
            let e = "\(label).edge[\(i)]"
            diff.same("\(e).from", graph.nodes[edge.from].path, want["from"])
            diff.same("\(e).to", graph.nodes[edge.to].path, want["to"])
            diff.same("\(e).weight", edge.weight, want["weight"])
            diff.same("\(e).inCycle", edge.inCycle, want["inCycle"])
            if edge.fileEdges.count != edge.weight { diff.problems.append("\(e).fileEdges count") }
        }
        let cycles = graph.cycles.map { $0.map { graph.nodes[$0].path }.sorted(by: U.less) }
        let wantCycles = expected["cycles"] as? [[String]] ?? []
        if cycles != wantCycles { diff.problems.append("\(label).cycles: got \(cycles), want \(wantCycles)") }
    }

    func report(_ diff: Diff, atLeast: Int = 1) {
        XCTAssertGreaterThanOrEqual(diff.checks, atLeast)
        if !diff.problems.isEmpty {
            XCTFail("\(diff.problems.count) golden mismatches:\n" + diff.problems.prefix(80).joined(separator: "\n"))
        }
    }

    func testProbesMatchTheDesktop() throws {
        let diff = Diff()
        let probes = try Fixture.expected()["probes"] as? [[String: Any]] ?? []
        XCTAssertEqual(probes.count, 45)
        for probe in probes {
            let path = probe["path"] as? String ?? ""
            diff.same("probe \(path) language", Insight.languageForPath(path), probe["language"])
            diff.same("probe \(path) test", Insight.isTestPath(path), probe["test"])
            diff.same("probe \(path) vendored", Insight.isVendoredPath(path), probe["vendored"])
        }
        report(diff, atLeast: 135)
    }

    func testBaseSnapshotMatchesTheDesktop() throws {
        let expected = try Fixture.expected()
        let result = try runGolden()
        let diff = Diff()
        XCTAssertEqual(result.base.error, "")
        compare(result.base, expected["base"] as? [String: Any] ?? [:], "base", diff)
        report(diff, atLeast: 2400)
    }

    func testHistorySnapshotMatchesTheDesktop() throws {
        let expected = try Fixture.expected()
        let result = try runGolden()
        let diff = Diff()
        compare(result.history, expected["history"] as? [String: Any] ?? [:], "history", diff)
        report(diff, atLeast: 2400)
    }

    func testModuleGraphsMatchTheDesktop() throws {
        let expected = try Fixture.expected()
        let modules = expected["modules"] as? [String: Any] ?? [:]
        let history = try runGolden().history
        let diff = Diff()
        compareModules(history, depth: 0, modules["auto"] as? [String: Any] ?? [:], "auto", diff)
        compareModules(history, depth: 1, modules["depth1"] as? [String: Any] ?? [:], "depth1", diff)
        compareModules(history, depth: 2, modules["depth2"] as? [String: Any] ?? [:], "depth2", diff)
        report(diff, atLeast: 600)
    }

    func testAnalyzeWithHistoryEqualsApplyingHistoryAfterwards() throws {
        let source = MemorySource(files: try Fixture.repoFiles(), commits: try Fixture.history())
        let full = try Insight.analyze(source: source, now: Self.now)
        let golden = try runGolden().history
        XCTAssertEqual(full.files, golden.files)
        XCTAssertEqual(full.authors, golden.authors)
        XCTAssertEqual(full.commitsScanned, golden.commitsScanned)
    }
}
