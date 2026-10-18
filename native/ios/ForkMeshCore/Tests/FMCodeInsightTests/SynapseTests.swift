import Foundation
import XCTest
@testable import FMCodeInsight

final class SynapseTests: XCTestCase {
    static func makeSnapshot(_ specs: [(String, [String])], cycles: [[String]] = []) -> InsightSnapshot {
        var files: [InsightFile] = []
        for (path, _) in specs {
            var file = InsightFile(path: path)
            file.language = path.hasSuffix(".py") ? "Python" : "C++"
            let hash = Synapse.stableHash(path)
            file.codeLines = 40 + Int(hash % 400)
            file.lines = file.codeLines + 10
            file.health = 2 + Double(hash % 80) / 10
            file.hotspot = Double(hash % 100) / 100
            files.append(file)
        }
        var snapshot = InsightSnapshot(files: files)
        for (i, spec) in specs.enumerated() {
            for target in spec.1 {
                let to = snapshot.indexOf(target)
                if to < 0 {
                    snapshot.files[i].externalImports.append(target)
                    continue
                }
                snapshot.files[i].dependsOn.append(to)
                snapshot.files[to].dependedOnBy.append(i)
            }
        }
        for cycle in cycles {
            var members: [Int] = []
            for path in cycle {
                let index = snapshot.indexOf(path)
                members.append(index)
                snapshot.files[index].cycle = snapshot.cycles.count
            }
            snapshot.cycles.append(members)
        }
        return snapshot
    }

    static func small() -> InsightSnapshot {
        makeSnapshot([
            ("core/a.cpp", ["core/b.cpp"]),
            ("core/b.cpp", ["core/c.cpp"]),
            ("core/c.cpp", ["core/a.cpp", "util/log.cpp"]),
            ("util/log.cpp", []),
            ("util/str.cpp", ["util/log.cpp", "fmt"]),
            ("app/main.cpp", ["core/a.cpp", "util/str.cpp"]),
            ("app/view.cpp", ["app/main.cpp"]),
            ("docs/lonely.cpp", []),
        ], cycles: [["core/a.cpp", "core/b.cpp", "core/c.cpp"]])
    }

    static func generated(modules: Int, perModule: Int, salt: Int) -> InsightSnapshot {
        var specs: [(String, [String])] = []
        for m in 0..<modules {
            for f in 0..<perModule { specs.append(("mod\(m)/sub/file\(f).cpp", [])) }
        }
        for i in specs.indices {
            let module = i / perModule
            let h = Synapse.stableHash("\(i)-\(salt)")
            let local = Int(h % 3) + 1
            for k in 0..<local {
                let target = module * perModule + Int((h >> UInt32(4 + k * 5)) % UInt32(perModule))
                if target != i { specs[i].1.append(specs[target].0) }
            }
            if h % 7 == 0 && modules > 1 {
                let other = (module + 1 + Int(h % UInt32(modules - 1))) % modules
                specs[i].1.append(specs[other * perModule + Int((h >> 9) % UInt32(perModule))].0)
            }
        }
        return makeSnapshot(specs)
    }

    func testQueries() {
        let s = Self.small()
        let loop = Synapse.cyclePath(s, s.cycles[0])
        XCTAssertEqual(loop.count, 4)
        XCTAssertEqual(loop.first, loop.last)
        XCTAssertEqual(Synapse.cycleText(s, loop), "a.cpp → b.cpp → c.cpp → a.cpp")
        XCTAssertEqual(Synapse.orphans(s).map { s.files[$0].path }, ["docs/lonely.cpp"])
        XCTAssertEqual(Synapse.leaves(s).map { s.files[$0].path }, ["util/log.cpp"])
        let imported = Synapse.mostImported(s, 2)
        XCTAssertEqual(imported.count, 2)
        XCTAssertEqual(s.files[imported[0]].dependedOnBy.count, 2)
        XCTAssertEqual(s.files[Synapse.mostImporting(s, 1)[0]].dependsOn.count, 2)
        XCTAssertEqual(Synapse.importCount(s), 8)
        let main = s.indexOf("app/main.cpp")
        let one = Synapse.neighbourhood(s, main, hops: 1, direction: .both)
        XCTAssertEqual(one.imports.count, 2)
        XCTAssertEqual(one.importers.count, 1)
        let two = Synapse.neighbourhood(s, main, hops: 2, direction: .imports)
        XCTAssertEqual(two.imports[s.indexOf("core/b.cpp")], 2)
        XCTAssertTrue(two.importers.isEmpty)
        let up = Synapse.neighbourhood(s, s.indexOf("util/log.cpp"), hops: 3, direction: .importers)
        XCTAssertNotNil(up.importers[main])
        XCTAssertTrue(up.imports.isEmpty)
        XCTAssertEqual(Synapse.neighbourhood(s, main, hops: 0, direction: .both).count, 0)
        XCTAssertEqual(Synapse.neighbourhood(s, -1, hops: 2, direction: .both).count, 0)
        XCTAssertGreaterThanOrEqual(Synapse.mostConnected(s), 0)
        XCTAssertEqual(Synapse.summary(s), "8 imports · 1 circular dependency · 1 orphan")
        XCTAssertEqual(Synapse.plural(22481, "import", "imports"), "22,481 imports")
        XCTAssertEqual(Synapse.externalPackages(s.files[s.indexOf("util/str.cpp")]), ["fmt"])
        XCTAssertEqual(Synapse.fileName("a/b/c.swift"), "c.swift")
        XCTAssertEqual(Synapse.fileName("c.swift"), "c.swift")
        XCTAssertEqual(Synapse.matches(s, "main", limit: 5).map { s.files[$0].path }, ["app/main.cpp"])
        XCTAssertEqual(Synapse.matches(s, "util/", limit: 1).count, 1)
        XCTAssertTrue(Synapse.matches(s, "  ", limit: 5).isEmpty)
    }

    func testCyclePathIsTheShortestLoopThroughTheFirstFile() {
        let s = Self.makeSnapshot([
            ("a.ts", ["b.ts", "d.ts"]),
            ("b.ts", ["c.ts"]),
            ("c.ts", ["a.ts"]),
            ("d.ts", ["a.ts"]),
        ], cycles: [["c.ts", "b.ts", "a.ts", "d.ts"]])
        XCTAssertEqual(Synapse.cycleText(s, Synapse.cyclePath(s, s.cycles[0])), "a.ts → d.ts → a.ts")
        XCTAssertTrue(Synapse.cyclePath(s, []).isEmpty)
    }

    func testGraphBuilding() {
        let s = Self.small()
        let graph = Synapse.buildGraph(s, includeIsolated: false)
        XCTAssertEqual(graph.nodes.count, 7)
        XCTAssertEqual(graph.edges.count, 8)
        XCTAssertEqual(graph.imports, 8)
        XCTAssertEqual(graph.edges.filter(\.cycle).count, 3)
        XCTAssertEqual(graph.cycles.count, 1)
        XCTAssertEqual(graph.cycles[0].edges.count, 3)
        XCTAssertGreaterThanOrEqual(graph.lobes.count, 3)
        XCTAssertEqual(graph.nodeFor(s.indexOf("docs/lonely.cpp")), -1)
        XCTAssertEqual(graph.lobes.reduce(0) { $0 + $1.nodes.count }, graph.nodes.count)
        for (n, node) in graph.nodes.enumerated() {
            XCTAssertEqual(graph.nodeFor(node.file), n)
            XCTAssertTrue(node.size >= 0.5 && node.size <= 3.2)
            XCTAssertEqual(node.hemisphere, graph.lobes[node.lobe].hemisphere)
        }
        let withIsolated = Synapse.buildGraph(s, includeIsolated: true)
        XCTAssertEqual(withIsolated.nodes.count, 8)
        XCTAssertTrue(withIsolated.includesIsolated)
        XCTAssertEqual(Synapse.connectedCount(withIsolated), 7)
        XCTAssertEqual(Synapse.cycleOrder(graph), [0])
    }

    func testEmptyAndSingleLobeGraphs() {
        let empty = Synapse.buildGraph(InsightSnapshot(), includeIsolated: false)
        XCTAssertTrue(empty.nodes.isEmpty)
        var published: [Double] = []
        XCTAssertTrue(Synapse.runLayout(empty, publish: { _, progress in published.append(progress) }).isEmpty)
        XCTAssertEqual(published, [1])
        let flat = Self.makeSnapshot([("a.c", ["b.c"]), ("b.c", ["c.c"]), ("c.c", []), ("d.c", ["a.c"])])
        let graph = Synapse.buildGraph(flat, includeIsolated: false)
        XCTAssertEqual(graph.lobes.count, 1)
        XCTAssertEqual(graph.lobes[0].hemisphere, 0)
        let positions = Synapse.runLayout(graph, options: SynapseLayoutOptions(iterations: 40))
        for (i, p) in positions.enumerated() {
            XCTAssertGreaterThan(p.x * Float(graph.nodes[i].hemisphere), 0)
            XCTAssertLessThanOrEqual(graph.shape.radiusOf(p), 1)
        }
    }

    func testLayoutIsDeterministicBoundedAndSeparatesLobes() {
        let snapshot = Self.generated(modules: 6, perModule: 40, salt: 1)
        let graph = Synapse.buildGraph(snapshot, includeIsolated: false)
        let options = SynapseLayoutOptions(iterations: 160, publishInterval: 0)
        var publishes = 0
        var last = -1.0
        let first = Synapse.runLayout(graph, options: options, publish: { _, progress in
            publishes += 1
            last = progress
        })
        let second = Synapse.runLayout(graph, options: options)
        XCTAssertEqual(first, second)
        XCTAssertGreaterThanOrEqual(publishes, 2)
        XCTAssertEqual(last, 1)
        for (i, p) in first.enumerated() {
            XCTAssertLessThanOrEqual(graph.shape.radiusOf(p), 1)
            XCTAssertGreaterThan(p.x * Float(graph.nodes[i].hemisphere), 0)
        }
        let centres = Synapse.lobeCentres(graph, first)
        var spread: Float = 0
        for (i, p) in first.enumerated() { spread += (p - centres[graph.nodes[i].lobe]).length }
        spread /= Float(max(1, first.count))
        var apart: Float = 1e9
        for a in centres.indices {
            for b in centres.indices where b > a { apart = min(apart, (centres[a] - centres[b]).length) }
        }
        XCTAssertGreaterThan(apart, spread * 0.8, "closest centres \(apart), spread \(spread)")
        XCTAssertEqual(Synapse.lobeCentres(graph, []), graph.lobes.map(\.anchor))
    }

    func testBarnesHutLayoutAndCancel() {
        let snapshot = Self.generated(modules: 8, perModule: 220, salt: 2)
        let graph = Synapse.buildGraph(snapshot, includeIsolated: false)
        let options = SynapseLayoutOptions(iterations: 6)
        XCTAssertGreaterThan(graph.nodes.count, options.barnesHutAbove)
        let first = Synapse.runLayout(graph, options: options)
        let second = Synapse.runLayout(graph, options: options)
        XCTAssertEqual(first, second)
        for p in first {
            XCTAssertTrue(p.x.isFinite && p.y.isFinite && p.z.isFinite)
            XCTAssertLessThanOrEqual(graph.shape.radiusOf(p), 1)
        }
        var asked = false
        let stopped = Synapse.runLayout(graph, options: options, stop: {
            asked = true
            return true
        })
        XCTAssertTrue(asked)
        XCTAssertEqual(stopped, Synapse.seedPositions(graph))
        XCTAssertEqual(Synapse.layoutIterations(graph), 300)
        var big = SynapseGraph()
        big.nodes = [SynapseNode](repeating: SynapseNode(), count: 10000)
        XCTAssertEqual(Synapse.layoutIterations(big), 150)
        big.nodes = [SynapseNode](repeating: SynapseNode(), count: 40000)
        XCTAssertEqual(Synapse.layoutIterations(big), 120)
    }

    func testOctreeApproximatesExactRepulsion() {
        let snapshot = Self.generated(modules: 4, perModule: 60, salt: 3)
        let graph = Synapse.buildGraph(snapshot, includeIsolated: false)
        let points = Synapse.seedPositions(graph)
        let tree = SynapseOctree(points)
        let k2 = graph.spacing * graph.spacing
        let exact = tree.force(0, k2, 0)
        var direct = SynapseVector()
        for j in points.indices where j != 0 {
            let d = points[0] - points[j]
            direct += d * (k2 / max(d.lengthSquared, 0.01 * k2))
        }
        XCTAssertLessThan((exact - direct).length, 1e-2 * max(1, direct.length))
        let approximate = tree.force(0, k2, 0.85 * 0.85)
        XCTAssertLessThan((approximate - direct).length, 0.25 * max(1, direct.length))
    }

    func testStyleShowModes() {
        let s = Self.small()
        let graph = Synapse.buildGraph(s, includeIsolated: false)
        var input = SynapseStyleInput()
        input.selected = s.indexOf("app/main.cpp")
        input.showMode = .neighbourhood
        XCTAssertEqual(SynapseStyle.make(graph, s, input).visibleNodes, 4)
        input.depth = 3
        let deep = SynapseStyle.make(graph, s, input)
        XCTAssertEqual(deep.visibleNodes, 7)
        XCTAssertEqual(deep.nodeState[graph.nodeFor(input.selected)], .selected)
        XCTAssertEqual(deep.nodeState[graph.nodeFor(s.indexOf("app/view.cpp"))], .importers)
        XCTAssertEqual(deep.nodeState[graph.nodeFor(s.indexOf("core/a.cpp"))], .imports)
        XCTAssertTrue(deep.drawEdges.contains { deep.edgeState[$0] == .imports })
        XCTAssertTrue(deep.drawEdges.contains { deep.edgeState[$0] == .importers })
        input.selected = -1
        input.showMode = .cycles
        let cycles = SynapseStyle.make(graph, s, input)
        XCTAssertEqual(cycles.visibleNodes, 3)
        XCTAssertEqual(cycles.cycleEdges, 3)
        XCTAssertTrue(cycles.cycleNodesVisible)
        input.showMode = .everything
        input.isolatedCycle = 0
        XCTAssertEqual(SynapseStyle.make(graph, s, input).visibleNodes, 3)
        input.isolatedCycle = -1
        input.query = "util/"
        let search = SynapseStyle.make(graph, s, input)
        XCTAssertEqual(search.nodeState.filter { $0 == .match }.count, 2)
        XCTAssertEqual(search.nodeState.filter { $0 == .dim }.count, 5)
        input.query = ""
        let orphans = SynapseStyle.make(Synapse.buildGraph(s, includeIsolated: true), s, SynapseStyleInput())
        XCTAssertEqual(orphans.visibleNodes, 8)
        var orphanInput = SynapseStyleInput()
        orphanInput.showMode = .orphans
        XCTAssertEqual(SynapseStyle.make(Synapse.buildGraph(s, includeIsolated: true), s, orphanInput).visibleNodes, 1)
        let everything = SynapseStyle.make(graph, s, SynapseStyleInput())
        XCTAssertEqual(everything.drawEdges.count, 8)
        XCTAssertEqual(everything.pulseEdges, 8)
        XCTAssertTrue(everything.animating)
        XCTAssertFalse(everything.hubs.isEmpty)
        XCTAssertEqual(everything.nodeColour[graph.nodeFor(s.indexOf("core/a.cpp"))], SynapsePalette.cycle)
        var capped = SynapseStyleInput()
        capped.edgeCap = 3
        XCTAssertEqual(SynapseStyle.make(graph, s, capped).drawEdges.count, 3)
        for mode in SynapseColourBy.allCases {
            var colours = SynapseStyleInput()
            colours.colourBy = mode
            let style = SynapseStyle.make(graph, s, colours)
            XCTAssertFalse(style.legend(graph, s).isEmpty)
            XCTAssertEqual(style.legend(graph, s).last?.label, "Circular dependency")
        }
    }

    func testCameraFitAndPicking() {
        let s = Self.small()
        let graph = Synapse.buildGraph(s, includeIsolated: false)
        let positions = Synapse.runLayout(graph)
        var input = SynapseStyleInput()
        let style = SynapseStyle.make(graph, s, input)
        let camera = Synapse.fit(SynapseCamera(), graph: graph, positions: positions, style: style, settled: true,
                                 aspect: 800 / 600, autoRotate: false)
        let frame = SynapseFrame()
        frame.update(graph: graph, positions: positions, style: style,
                     projection: SynapseProjection(camera: camera, width: 800, height: 600, boundingRadius: graph.shape.boundingRadius))
        XCTAssertEqual(frame.onScreen.filter { $0 }.count, graph.nodes.count)
        var exact = 0
        for n in graph.nodes.indices {
            let hit = frame.pick(x: frame.x[n], y: frame.y[n])
            if hit == n {
                exact += 1
            } else {
                XCTAssertGreaterThanOrEqual(hit, 0)
                let dx = frame.x[hit] - frame.x[n]
                let dy = frame.y[hit] - frame.y[n]
                XCTAssertLessThan((dx * dx + dy * dy).squareRoot(), 40)
                XCTAssertLessThan(frame.depth[hit], frame.depth[n])
            }
        }
        XCTAssertGreaterThanOrEqual(exact, graph.nodes.count - 1)
        XCTAssertEqual(frame.pick(x: 2, y: 2), -1)
        for k in 1..<frame.order.count {
            XCTAssertGreaterThanOrEqual(frame.depth[frame.order[k - 1]], frame.depth[frame.order[k]])
        }
        input.selected = s.indexOf("app/main.cpp")
        let flight = Synapse.flightToFile(camera, graph: graph, positions: positions, file: input.selected, aspect: 1, autoRotate: true)
        XCTAssertNotNil(flight)
        XCTAssertLessThanOrEqual(flight?.distance ?? .infinity, max(camera.distance, graph.spacing * 6))
        XCTAssertNil(Synapse.flightToFile(camera, graph: graph, positions: positions, file: s.indexOf("docs/lonely.cpp"), aspect: 1, autoRotate: true))
        XCTAssertNotNil(Synapse.flightToCycle(camera, graph: graph, positions: positions, cycle: 0, aspect: 1, autoRotate: true))
        let whole = Synapse.fit(SynapseCamera(), graph: graph, positions: positions, style: style, settled: false, aspect: 0.5, autoRotate: true)
        XCTAssertEqual(whole.target.x, 0, accuracy: 1e-4)
        XCTAssertGreaterThan(whole.distance, 200)
        let zoomed = camera.zoomed(by: 0.5, towardX: 400, y: 300, width: 800, height: 600)
        XCTAssertEqual(zoomed.distance, camera.distance * 0.5, accuracy: 1e-3)
        XCTAssertEqual(SynapseCamera(distance: 5).clamped().distance, SynapseCamera.minDistance)
        XCTAssertEqual(camera.orbited(dx: 0, dy: 10000).pitch, SynapseCamera.maxPitch)
        XCTAssertEqual(SynapseCamera.easeInOut(0), 0)
        XCTAssertEqual(SynapseCamera.easeInOut(1), 1)
        XCTAssertEqual(SynapseCamera.easeInOut(0.5), 0.5, accuracy: 1e-6)
    }

    func testColoursAndAtmosphere() {
        XCTAssertEqual(SynapsePalette.lobe(12), SynapsePalette.lobe(0))
        XCTAssertEqual(SynapsePalette.lobe(-1), SynapsePalette.lobe(11))
        XCTAssertEqual(SynapsePalette.language("Swift"), SynapseColour(rgb: 0xf06a4a))
        XCTAssertEqual(SynapsePalette.language("Brainfuck"), SynapsePalette.language("Brainfuck"))
        XCTAssertEqual(SynapsePalette.health(8), SynapseColour(rgb: 0x3fb98f))
        XCTAssertEqual(SynapsePalette.health(3.9), SynapseColour(rgb: 0xe5534b))
        XCTAssertEqual(SynapsePalette.hotspot(0), SynapseColour(rgb: 0x3a4d72))
        XCTAssertEqual(SynapsePalette.hotspot(1), SynapseColour(rgb: 0xff6f3c))
        let lighter = SynapseColour(rgb: 0x405060).lighter(125)
        XCTAssertGreaterThan(lighter.b, SynapseColour(rgb: 0x405060).b)
        XCTAssertEqual(SynapseColour(rgb: 0xffffff).lighter(150), SynapseColour(rgb: 0xffffff))
        XCTAssertLessThan(SynapseColour(rgb: 0x80a0c0).darker(200).r, SynapseColour(rgb: 0x80a0c0).r)
        XCTAssertEqual(SynapsePalette.shortLobeName("a/b/c/d"), "c/d")
        XCTAssertEqual(SynapsePalette.shortLobeName("a/b"), "a/b")
        XCTAssertEqual(Synapse.stars().count, 420)
        let dust = Synapse.dust(SynapseBrainShape())
        XCTAssertGreaterThan(dust.count, 1500)
        XCTAssertTrue(dust.allSatisfy { abs($0.x) >= 15 * 0.6 })
        let a = SynapseVector(0, 0, 0)
        let b = SynapseVector(10, 0, 0)
        XCTAssertEqual(Synapse.curvePoint(a, b, 0), a)
        XCTAssertEqual(Synapse.curvePoint(a, b, 1), b)
    }

    func testVectorMathMatchesQt() {
        XCTAssertEqual(SynapseVector(3, 4, 12).length, 13)
        XCTAssertEqual(SynapseVector(0, 0, 0).length, 0)
        XCTAssertEqual(SynapseVector(1, 0, 0).normalized, SynapseVector(1, 0, 0))
        XCTAssertEqual(SynapseVector(0.000001, 0, 0).normalized, SynapseVector())
        var v = SynapseVector(0, 3, 4)
        v.normalize()
        XCTAssertEqual(v.y, 0.6, accuracy: 1e-7)
        XCTAssertEqual(SynapseMath.mixHash(0), 0)
        XCTAssertEqual(Synapse.stableHash(""), SynapseMath.mixHash(2166136261))
        let shape = SynapseBrainShape()
        XCTAssertEqual(shape.radiusOf(SynapseVector(0, 76, 0)), 1, accuracy: 1e-6)
        XCTAssertEqual(shape.radiusOf(SynapseVector(0, -58, 0)), 1, accuracy: 1e-6)
        XCTAssertEqual(shape.radiusOf(shape.onShell(SynapseVector(1, 1, 1), 0.7)), 0.7, accuracy: 1e-5)
        let outer = shape.outerAt(SynapseVector(10, 20, 30))
        XCTAssertTrue(outer <= shape.outerShell && outer >= shape.outerShell - 0.055)
    }
}
