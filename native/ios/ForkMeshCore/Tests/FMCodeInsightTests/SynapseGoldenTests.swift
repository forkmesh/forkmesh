import Foundation
import XCTest
@testable import FMCodeInsight

final class SynapseGoldenTests: XCTestCase {
    #if os(Linux)
    static let positionTolerance: Float = 1e-3
    #else
    static let positionTolerance: Float = 1000
    #endif
    static let anchorTolerance: Float = 1e-3
    static let seedTolerance: Float = 1e-3

    private static var cached: (snapshot: InsightSnapshot, expected: [String: Any])?

    func load() throws -> (snapshot: InsightSnapshot, expected: [String: Any]) {
        if let cached = Self.cached { return cached }
        let url = Fixture.root.appendingPathComponent("synapse.json")
        guard FileManager.default.fileExists(atPath: url.path) else { throw XCTSkip("native/fixtures/insight/synapse.json is missing") }
        let expected = try JSONSerialization.jsonObject(with: Data(contentsOf: url)) as? [String: Any] ?? [:]
        let now: Int64 = ((try? Fixture.expected())?["now"] as? NSNumber)?.int64Value ?? 1_792_000_000
        let source = MemorySource(files: try Fixture.repoFiles(), commits: try Fixture.history())
        var options = InsightOptions()
        options.history = false
        var snapshot = try Insight.analyze(source: source, options: options, now: now)
        Insight.applyHistory(&snapshot, try Fixture.history(), InsightOptions(), now: now)
        for i in snapshot.files.indices {
            let result = Insight.computeHealth(snapshot.files[i])
            snapshot.files[i].health = result.health
            snapshot.files[i].healthIssues = result.issues
        }
        Insight.computeHotspots(&snapshot)
        Self.cached = (snapshot, expected)
        return (snapshot, expected)
    }

    func paths(_ snapshot: InsightSnapshot, _ indexes: [Int]) -> [String] {
        indexes.map { snapshot.files[$0].path }
    }

    func vectors(_ value: Any?) -> [SynapseVector] {
        (value as? [[NSNumber]] ?? []).map { SynapseVector($0[0].floatValue, $0[1].floatValue, $0[2].floatValue) }
    }

    func deviation(_ want: [SynapseVector], _ got: [SynapseVector]) -> Float {
        XCTAssertEqual(want.count, got.count, "position count")
        var worst: Float = 0
        for (a, b) in zip(want, got) {
            worst = max(worst, abs(a.x - b.x), abs(a.y - b.y), abs(a.z - b.z))
        }
        return worst
    }

    func testFixtureMatchesTheSnapshot() throws {
        let (snapshot, expected) = try load()
        XCTAssertEqual((expected["files"] as? NSNumber)?.intValue, snapshot.files.count)
        XCTAssertEqual(expected["fixture"] as? String, "native/fixtures/insight")
    }

    func testStableHashMatchesTheDesktop() throws {
        let (_, expected) = try load()
        let rows = expected["hashes"] as? [[String: Any]] ?? []
        XCTAssertFalse(rows.isEmpty)
        for row in rows {
            let text = row["text"] as? String ?? ""
            XCTAssertEqual(UInt32((row["hash"] as? NSNumber)?.uint64Value ?? 0), Synapse.stableHash(text), "hash of \(text)")
        }
    }

    func testQueriesMatchTheDesktop() throws {
        let (snapshot, expected) = try load()
        XCTAssertEqual((expected["importCount"] as? NSNumber)?.intValue, Synapse.importCount(snapshot))
        XCTAssertEqual(expected["orphans"] as? [String], paths(snapshot, Synapse.orphans(snapshot)))
        XCTAssertEqual(expected["leaves"] as? [String], paths(snapshot, Synapse.leaves(snapshot)))
        XCTAssertEqual(expected["mostImported"] as? [String], paths(snapshot, Synapse.mostImported(snapshot, 10)))
        XCTAssertEqual(expected["mostImporting"] as? [String], paths(snapshot, Synapse.mostImporting(snapshot, 10)))
        XCTAssertEqual(expected["mostImportedAll"] as? [String], paths(snapshot, Synapse.mostImported(snapshot, -1)))
        XCTAssertEqual(expected["mostImportingAll"] as? [String], paths(snapshot, Synapse.mostImporting(snapshot, -1)))
        XCTAssertEqual(expected["mostConnected"] as? String, snapshot.files[Synapse.mostConnected(snapshot)].path)
        let orphans = (expected["orphans"] as? [String] ?? []).count
        let cycles = (expected["cycles"] as? [Any] ?? []).count
        let imports = (expected["importCount"] as? NSNumber)?.intValue ?? -1
        XCTAssertEqual("\(imports) imports · \(cycles) circular dependencies · \(orphans) orphans", Synapse.summary(snapshot))
    }

    func testCyclePathsMatchTheDesktop() throws {
        let (snapshot, expected) = try load()
        let rows = expected["cycles"] as? [[String: Any]] ?? []
        XCTAssertEqual(rows.count, snapshot.cycles.count)
        for (c, row) in rows.enumerated() where c < snapshot.cycles.count {
            let path = Synapse.cyclePath(snapshot, snapshot.cycles[c])
            XCTAssertEqual(row["files"] as? [String], paths(snapshot, snapshot.cycles[c]), "cycle \(c) files")
            XCTAssertEqual(row["path"] as? [String], paths(snapshot, path), "cycle \(c) path")
            XCTAssertEqual(row["text"] as? String, Synapse.cycleText(snapshot, path), "cycle \(c) text")
        }
    }

    func testNeighbourhoodsMatchTheDesktop() throws {
        let (snapshot, expected) = try load()
        let rows = expected["neighbourhoods"] as? [[String: Any]] ?? []
        XCTAssertEqual(rows.count, snapshot.files.count * 3)
        func named(_ map: [Int: Int]) -> [String: Int] {
            Dictionary(uniqueKeysWithValues: map.map { (snapshot.files[$0.key].path, $0.value) })
        }
        func wanted(_ value: Any?) -> [String: Int] {
            (value as? [String: NSNumber] ?? [:]).mapValues(\.intValue)
        }
        for row in rows {
            let path = row["file"] as? String ?? ""
            let file = snapshot.indexOf(path)
            let hops = (row["hops"] as? NSNumber)?.intValue ?? 0
            let both = Synapse.neighbourhood(snapshot, file, hops: hops, direction: .both)
            XCTAssertEqual(wanted(row["imports"]), named(both.imports), "imports of \(path) at \(hops)")
            XCTAssertEqual(wanted(row["importers"]), named(both.importers), "importers of \(path) at \(hops)")
            let forward = Synapse.neighbourhood(snapshot, file, hops: hops, direction: .imports)
            XCTAssertTrue(forward.importers.isEmpty && forward.imports == both.imports)
            let backward = Synapse.neighbourhood(snapshot, file, hops: hops, direction: .importers)
            XCTAssertTrue(backward.imports.isEmpty && backward.importers == both.importers)
        }
    }

    func checkGraph(_ includeIsolated: Bool) throws {
        let (snapshot, expected) = try load()
        let key = includeIsolated ? "withOrphans" : "connected"
        let want = (expected["graphs"] as? [String: Any])?[key] as? [String: Any] ?? [:]
        let graph = Synapse.buildGraph(snapshot, includeIsolated: includeIsolated)
        let nodePath = { (n: Int) in snapshot.files[graph.nodes[n].file].path }
        XCTAssertEqual(want["includesIsolated"] as? Bool, graph.includesIsolated)
        XCTAssertEqual((want["imports"] as? NSNumber)?.intValue, graph.imports)
        XCTAssertEqual((want["spacing"] as? NSNumber)?.floatValue, graph.spacing)
        XCTAssertEqual((want["iterations"] as? NSNumber)?.intValue, Synapse.layoutIterations(graph))
        let lobes = want["lobes"] as? [[String: Any]] ?? []
        XCTAssertEqual(lobes.count, graph.lobes.count, "lobe count")
        for (l, row) in lobes.enumerated() where l < graph.lobes.count {
            let lobe = graph.lobes[l]
            XCTAssertEqual(row["name"] as? String, lobe.name, "lobe \(l) name")
            XCTAssertEqual(row["path"] as? String, lobe.path, "lobe \(l) path")
            XCTAssertEqual((row["layer"] as? NSNumber)?.intValue, lobe.layer, "lobe \(l) layer")
            XCTAssertEqual((row["hemisphere"] as? NSNumber)?.intValue, lobe.hemisphere, "lobe \(l) hemisphere")
            XCTAssertEqual(row["nodes"] as? [String], lobe.nodes.map(nodePath), "lobe \(l) nodes")
            let anchor = vectors([row["anchor"] as Any])
            XCTAssertLessThanOrEqual(deviation(anchor, [lobe.anchor]), Self.anchorTolerance, "lobe \(l) anchor")
        }
        let nodes = want["nodes"] as? [[String: Any]] ?? []
        XCTAssertEqual(nodes.count, graph.nodes.count, "node count")
        for (n, row) in nodes.enumerated() where n < graph.nodes.count {
            let node = graph.nodes[n]
            XCTAssertEqual(row["path"] as? String, nodePath(n), "node \(n) path")
            XCTAssertEqual((row["lobe"] as? NSNumber)?.intValue, node.lobe, "node \(n) lobe")
            XCTAssertEqual((row["hemisphere"] as? NSNumber)?.intValue, node.hemisphere, "node \(n) hemisphere")
            XCTAssertEqual((row["size"] as? NSNumber)?.floatValue, node.size, "node \(n) size")
            XCTAssertEqual((row["fanIn"] as? NSNumber)?.intValue, node.fanIn, "node \(n) fanIn")
            XCTAssertEqual((row["fanOut"] as? NSNumber)?.intValue, node.fanOut, "node \(n) fanOut")
            XCTAssertEqual(row["cycle"] as? Bool, node.cycle, "node \(n) cycle")
            XCTAssertEqual((row["seed"] as? NSNumber)?.uint32Value, node.seed, "node \(n) seed")
        }
        let edges = want["edges"] as? [[String: Any]] ?? []
        XCTAssertEqual(edges.count, graph.edges.count, "edge count")
        for (e, row) in edges.enumerated() where e < graph.edges.count {
            let edge = graph.edges[e]
            XCTAssertEqual(row["from"] as? String, nodePath(edge.from), "edge \(e) from")
            XCTAssertEqual(row["to"] as? String, nodePath(edge.to), "edge \(e) to")
            XCTAssertEqual(row["cycle"] as? Bool, edge.cycle, "edge \(e) cycle")
            XCTAssertEqual((row["seed"] as? NSNumber)?.uint32Value, edge.seed, "edge \(e) seed")
        }
        let cycles = want["cycles"] as? [[String: Any]] ?? []
        XCTAssertEqual(cycles.count, graph.cycles.count, "graph cycle count")
        for (c, row) in cycles.enumerated() where c < graph.cycles.count {
            let info = graph.cycles[c]
            XCTAssertEqual((row["index"] as? NSNumber)?.intValue, info.index)
            XCTAssertEqual(row["files"] as? [String], paths(snapshot, info.files))
            XCTAssertEqual(row["path"] as? [String], paths(snapshot, info.path))
            XCTAssertEqual(row["edges"] as? [[String]], info.edges.map { [nodePath(graph.edges[$0].from), nodePath(graph.edges[$0].to)] })
        }
        let seeds = deviation(vectors(want["seeds"]), Synapse.seedPositions(graph))
        XCTAssertLessThanOrEqual(seeds, Self.seedTolerance, "seed positions deviate by \(seeds)")
        for row in want["layouts"] as? [[String: Any]] ?? [] {
            let iterations = (row["iterations"] as? NSNumber)?.intValue ?? 0
            let positions = Synapse.runLayout(graph, options: SynapseLayoutOptions(iterations: iterations))
            let worst = deviation(vectors(row["positions"]), positions)
            let centres = deviation(vectors(row["lobeCentres"]), Synapse.lobeCentres(graph, positions))
            print("synapse \(key) \(iterations) iterations: positions within \(worst), lobe centres within \(centres)")
            XCTAssertLessThanOrEqual(worst, Self.positionTolerance, "\(key) layout of \(iterations) iterations deviates by \(worst)")
            XCTAssertLessThanOrEqual(centres, Self.positionTolerance, "\(key) lobe centres after \(iterations) iterations deviate by \(centres)")
        }
    }

    func testConnectedGraphAndLayoutMatchTheDesktop() throws {
        try checkGraph(false)
    }

    func testGraphWithOrphansAndLayoutMatchTheDesktop() throws {
        try checkGraph(true)
    }
}
