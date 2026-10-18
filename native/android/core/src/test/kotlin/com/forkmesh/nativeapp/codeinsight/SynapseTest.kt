package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.text.NumberFormat
import java.util.Locale
import kotlin.math.abs
import kotlin.math.hypot
import kotlin.math.sqrt

class SynapseTest {
    private class Spec(val path: String, vararg val imports: String)

    private fun makeSnapshot(specs: List<Spec>, cycles: List<List<String>> = emptyList()): InsightSnapshot {
        val files = specs.map { spec ->
            val hash = Synapse.stableHashUnsigned(spec.path)
            InsightFile(
                path = spec.path,
                language = if (spec.path.endsWith(".py")) "Python" else "C++",
                codeLines = 40 + (hash % 400L).toInt(),
                health = 2.0 + (hash % 80L).toDouble() / 10.0,
                hotspot = (hash % 100L).toDouble() / 100.0,
            ).also { it.lines = it.codeLines + 10 }
        }
        val snapshot = InsightSnapshot(name = "synapse-test", files = files)
        val dependsOn = List(files.size) { ArrayList<Int>() }
        val dependedOnBy = List(files.size) { ArrayList<Int>() }
        val external = List(files.size) { ArrayList<String>() }
        for ((i, spec) in specs.withIndex()) {
            for (target in spec.imports) {
                val to = snapshot.indexOf(target)
                if (to < 0) {
                    external[i].add(target)
                    continue
                }
                dependsOn[i].add(to)
                dependedOnBy[to].add(i)
            }
        }
        for (i in files.indices) {
            files[i].dependsOn = dependsOn[i]
            files[i].dependedOnBy = dependedOnBy[i]
            files[i].externalImports = external[i]
        }
        val members = cycles.mapIndexed { c, cycle ->
            cycle.map { path -> snapshot.indexOf(path).also { files[it].cycle = c } }
        }
        snapshot.cycles = members
        return snapshot
    }

    private fun smallSnapshot(): InsightSnapshot = makeSnapshot(
        listOf(
            Spec("core/a.cpp", "core/b.cpp"),
            Spec("core/b.cpp", "core/c.cpp"),
            Spec("core/c.cpp", "core/a.cpp", "util/log.cpp"),
            Spec("util/log.cpp"),
            Spec("util/str.cpp", "util/log.cpp", "fmt"),
            Spec("app/main.cpp", "core/a.cpp", "util/str.cpp"),
            Spec("app/view.cpp", "app/main.cpp"),
            Spec("docs/lonely.cpp"),
        ),
        listOf(listOf("core/a.cpp", "core/b.cpp", "core/c.cpp")),
    )

    private fun generatedSnapshot(modules: Int, perModule: Int, salt: Int): InsightSnapshot {
        val paths = ArrayList<String>()
        for (m in 0 until modules) for (f in 0 until perModule) paths.add("mod$m/sub/file$f.cpp")
        val imports = List(paths.size) { ArrayList<String>() }
        for (i in paths.indices) {
            val module = i / perModule
            val h = Synapse.stableHashUnsigned("$i-$salt")
            val local = (h % 3L).toInt() + 1
            for (k in 0 until local) {
                val target = module * perModule + ((h ushr (4 + k * 5)) % perModule.toLong()).toInt()
                if (target != i) imports[i].add(paths[target])
            }
            if (h % 7L == 0L && modules > 1) {
                val other = (module + 1 + (h % (modules - 1).toLong()).toInt()) % modules
                imports[i].add(paths[other * perModule + ((h ushr 9) % perModule.toLong()).toInt()])
            }
        }
        return makeSnapshot(paths.indices.map { Spec(paths[it], *imports[it].toTypedArray()) })
    }

    private fun near(a: Float, b: Float, eps: Float = 1e-4f) = abs(a - b) <= eps

    fun testQueries() {
        val s = smallSnapshot()
        val loop = Synapse.cyclePath(s, s.cycles.first())
        expect(loop.size == 4 && loop.first() == loop.last(), "cycle path closes on itself")
        assertEq("a.cpp → b.cpp → c.cpp → a.cpp", Synapse.cycleText(s, loop))
        assertEq(listOf("docs/lonely.cpp"), Synapse.orphans(s).map { s.files[it].path })
        assertEq(listOf("util/log.cpp"), Synapse.leaves(s).map { s.files[it].path })
        val imported = Synapse.mostImported(s, 2)
        expect(imported.size == 2 && s.files[imported.first()].dependedOnBy.size == 2, "most imported is ranked by importers")
        assertEq(2, s.files[Synapse.mostImporting(s, 1).first()].dependsOn.size)
        assertEq(8, Synapse.importCount(s))
        val main = s.indexOf("app/main.cpp")
        val one = Synapse.neighbourhood(s, main, 1, SynapseDirection.Both)
        expect(one.imports.size == 2 && one.importers.size == 1, "one hop: two imports and one importer")
        val two = Synapse.neighbourhood(s, main, 2, SynapseDirection.Imports)
        assertEq(2, two.imports[s.indexOf("core/b.cpp")])
        expect(two.importers.isEmpty(), "imports-only walk has no importers")
        val up = Synapse.neighbourhood(s, s.indexOf("util/log.cpp"), 3, SynapseDirection.Importers)
        expect(up.importers.containsKey(main) && up.imports.isEmpty(), "importers walk upward")
        expect(Synapse.mostConnected(s) >= 0, "most connected file found")
        assertEq(0, Synapse.neighbourhood(s, main, 0, SynapseDirection.Both).size)
    }

    fun testCyclePathIsTheShortestLoopFromTheFirstPath() {
        val s = makeSnapshot(
            listOf(
                Spec("a.py", "b.py", "d.py"),
                Spec("b.py", "c.py"),
                Spec("c.py", "a.py"),
                Spec("d.py", "a.py"),
            ),
            listOf(listOf("a.py", "b.py", "c.py", "d.py")),
        )
        assertEq("a.py → d.py → a.py", Synapse.cycleText(s, Synapse.cyclePath(s, s.cycles.first())))
        assertEq(emptyList<Int>(), Synapse.cyclePath(s, emptyList()))
    }

    fun testSummary() {
        val format = NumberFormat.getIntegerInstance(Locale.US)
        assertEq("8 imports · 1 circular dependency · 1 orphan", Synapse.summary(smallSnapshot(), format))
        assertEq("22,481 imports", Synapse.plural(22481, "import", "imports", format))
    }

    fun testGraph() {
        val snapshot = smallSnapshot()
        val graph = Synapse.buildGraph(snapshot, false)
        assertEq(7, graph.nodes.size, "isolated files are left out")
        assertEq(8, graph.edges.size, "one edge per import")
        assertEq(3, graph.edges.count { it.cycle }, "three edges inside the cycle")
        expect(graph.cycles.size == 1 && graph.cycles.first().edges.size == 3, "cycle info lists its edges")
        expect(graph.lobes.size >= 3, "files grouped into lobes (${graph.lobes.size})")
        assertEq(8, Synapse.buildGraph(snapshot, true).nodes.size, "isolated files can be included")
        assertEq(-1, graph.nodeFor(snapshot.indexOf("docs/lonely.cpp")))
        for (lobe in graph.lobes) expect(lobe.hemisphere == 1 || lobe.hemisphere == -1, "multi-lobe anchors pick a hemisphere")
    }

    fun testSingleLobeSpreadsOverBothHemispheres() {
        val snapshot = makeSnapshot(listOf(Spec("a.cpp", "b.cpp"), Spec("b.cpp", "c.cpp"), Spec("c.cpp", "d.cpp"), Spec("d.cpp"), Spec("e.cpp", "a.cpp"), Spec("f.cpp", "a.cpp")))
        val graph = Synapse.buildGraph(snapshot, false)
        assertEq(1, graph.lobes.size)
        assertEq(0, graph.lobes.first().hemisphere)
        val positions = Synapse.runLayout(graph, SynapseLayoutOptions(iterations = 40))
        for ((i, node) in graph.nodes.withIndex()) expect(positions[i * 3] * node.hemisphere > 0, "node $i keeps its side")
    }

    fun testLayout() {
        val graph = Synapse.buildGraph(generatedSnapshot(6, 40, 1), false)
        val options = SynapseLayoutOptions(iterations = 160, publishIntervalMs = 0)
        var publishes = 0
        var last = -1.0
        val first = Synapse.runLayout(graph, options, publish = { _, progress ->
            publishes++
            last = progress
        })
        val second = Synapse.runLayout(graph, options)
        expect(first.contentEquals(second), "layout is deterministic")
        expect(publishes >= 2 && last == 1.0, "layout publishes progress and finishes at 1")
        for ((i, node) in graph.nodes.withIndex()) {
            expect(graph.shape.radiusOf(first[i * 3], first[i * 3 + 1], first[i * 3 + 2]) <= 1.0f, "node $i inside the brain")
            expect(first[i * 3] * node.hemisphere > 0, "node $i in its hemisphere")
        }
        val centres = Synapse.lobeCentres(graph, first)
        var spread = 0f
        for ((i, node) in graph.nodes.withIndex()) {
            spread += hypot(hypot(first[i * 3] - centres[node.lobe * 3], first[i * 3 + 1] - centres[node.lobe * 3 + 1]), first[i * 3 + 2] - centres[node.lobe * 3 + 2])
        }
        spread /= graph.nodes.size
        var apart = Float.MAX_VALUE
        for (a in graph.lobes.indices) for (b in a + 1 until graph.lobes.size) {
            val dx = centres[a * 3] - centres[b * 3]
            val dy = centres[a * 3 + 1] - centres[b * 3 + 1]
            val dz = centres[a * 3 + 2] - centres[b * 3 + 2]
            apart = minOf(apart, sqrt(dx * dx + dy * dy + dz * dz))
        }
        expect(apart > spread * 0.8f, "lobes separate (closest centres $apart, spread $spread)")
    }

    fun testBarnesHutLayout() {
        val graph = Synapse.buildGraph(generatedSnapshot(8, 220, 2), false)
        val fast = SynapseLayoutOptions(iterations = 24)
        expect(graph.nodes.size > fast.barnesHutAbove, "large graph uses Barnes-Hut (${graph.nodes.size} nodes)")
        val b1 = Synapse.runLayout(graph, fast)
        val b2 = Synapse.runLayout(graph, fast)
        expect(b1.contentEquals(b2), "Barnes-Hut layout is deterministic")
        for (i in graph.nodes.indices) {
            expect(b1[i * 3].isFinite() && graph.shape.radiusOf(b1[i * 3], b1[i * 3 + 1], b1[i * 3 + 2]) <= 1.0f, "node $i bounded")
        }
        var asked = false
        val stopped = Synapse.runLayout(graph, fast, stop = {
            asked = true
            true
        })
        expect(asked, "layout honours cancel")
        expect(stopped.contentEquals(Synapse.seedPositions(graph)), "a cancelled layout returns the seeds")
    }

    fun testIterationsShrinkForHugeGraphs() {
        val graph = Synapse.buildGraph(generatedSnapshot(2, 10, 3), false)
        assertEq(300, Synapse.layoutIterations(graph))
    }

    fun testShowModes() {
        val snapshot = smallSnapshot()
        val graph = Synapse.buildGraph(snapshot, false)
        val main = snapshot.indexOf("app/main.cpp")
        var style = SynapseScene.restyle(graph, SynapseSceneOptions(selected = main, showMode = SynapseShowMode.Neighbourhood))
        assertEq(4, style.visibleNodes, "neighbourhood shows the file and its neighbours")
        style = SynapseScene.restyle(graph, SynapseSceneOptions(selected = main, showMode = SynapseShowMode.Neighbourhood, depth = 3))
        assertEq(7, style.visibleNodes, "a deeper neighbourhood reaches further")
        style = SynapseScene.restyle(graph, SynapseSceneOptions(showMode = SynapseShowMode.Cycles))
        assertEq(3, style.visibleNodes, "cycle mode shows only the loop")
        style = SynapseScene.restyle(graph, SynapseSceneOptions(isolatedCycle = 0))
        assertEq(3, style.visibleNodes, "isolating a cycle shows its files")
        expect(style.drawEdges.all { style.edgeState[it] == SynapseScene.EDGE_CYCLE }, "isolated loop fibres are cycle fibres")
        val withOrphans = Synapse.buildGraph(snapshot, true)
        style = SynapseScene.restyle(withOrphans, SynapseSceneOptions(showMode = SynapseShowMode.Orphans))
        assertEq(1, style.visibleNodes, "orphan mode shows the isolated file")
        style = SynapseScene.restyle(graph, SynapseSceneOptions(query = "util/"))
        assertEq(2, (0 until graph.nodes.size).count { style.nodeState[it] == SynapseScene.MATCH }, "search marks matches")
    }

    fun testSelectionStates() {
        val snapshot = smallSnapshot()
        val graph = Synapse.buildGraph(snapshot, false)
        val main = snapshot.indexOf("app/main.cpp")
        val style = SynapseScene.restyle(graph, SynapseSceneOptions(selected = main))
        val state = { path: String -> style.nodeState[graph.nodeFor(snapshot.indexOf(path))] }
        assertEq(SynapseScene.SELECTED, state("app/main.cpp"))
        assertEq(SynapseScene.IMPORT, state("core/a.cpp"))
        assertEq(SynapseScene.IMPORTER, state("app/view.cpp"))
        assertEq(SynapseScene.DIM, state("core/b.cpp"))
        assertEq(SynapseColours.IMPORT, style.nodeColour[graph.nodeFor(snapshot.indexOf("core/a.cpp"))])
        val highlighted = style.drawEdges.filter { style.highlighted(it) }
        assertEq(3, highlighted.size, "fibres to direct neighbours light up")
        expect(style.drawEdges.takeLast(3).toSet() == highlighted.toSet(), "highlighted fibres are drawn last")
        expect(style.hubs.isNotEmpty(), "hub labels chosen")
    }

    fun testEdgeCap() {
        val graph = Synapse.buildGraph(generatedSnapshot(4, 60, 5), false)
        val style = SynapseScene.restyle(graph, SynapseSceneOptions(edgeCap = 50))
        assertEq(50, style.drawEdges.size)
        val again = SynapseScene.restyle(graph, SynapseSceneOptions(edgeCap = 50))
        expect(style.drawEdges.contentEquals(again.drawEdges), "the fibre sample is stable")
    }

    fun testProjectionAndPicking() {
        val snapshot = smallSnapshot()
        val graph = Synapse.buildGraph(snapshot, false)
        val positions = Synapse.runLayout(graph)
        val style = SynapseScene.restyle(graph, SynapseSceneOptions())
        val camera = SynapseScene.fittedCamera(style, positions, SynapseCamera(), null, 800f / 600f, false, true)
        val projection = SynapseProjection()
        projection.update(style, positions, camera, 800f, 600f)
        var exact = 0
        for (i in graph.nodes.indices) {
            expect(projection.onScreen[i], "fit shows node $i")
            if (SynapseScene.pick(style, projection, projection.x[i], projection.y[i]) == graph.nodes[i].file) exact++
        }
        expect(exact >= graph.nodes.size - 1, "picking finds the node under the finger ($exact of ${graph.nodes.size})")
        assertEq(-1, SynapseScene.pick(style, projection, 2f, 2f))
        val target = graph.nodes.first().file
        val flown = SynapseScene.fileCamera(style, positions, camera, target, 800f / 600f, false)
        expect(flown != null && flown.distance <= camera.distance, "flying to a file moves closer")
        val cycleCamera = SynapseScene.cycleCamera(style, positions, camera, 0, 800f / 600f, false)
        expect(cycleCamera != null, "a loop can be framed")
    }

    fun testCameraGestures() {
        val camera = SynapseCamera()
        val orbited = SynapseScene.orbit(camera, 100f, 1000f)
        expect(near(orbited.yaw, camera.yaw - 0.8f) && orbited.pitch == 1.45f, "orbit turns and clamps the pitch")
        val zoomed = SynapseScene.zoomAt(camera, 0.5f, 400f, 300f, 800f, 600f)
        expect(near(zoomed.distance, 210f) && near(zoomed.targetX, 0f) && near(zoomed.targetZ, 0f), "zoom at the centre keeps the target")
        val panned = SynapseScene.pan(camera, 10f, 0f, 600f)
        expect(abs(panned.targetY) < 1e-4f && (panned.targetX != 0f || panned.targetZ != 0f), "horizontal pan stays level")
        val halfway = SynapseScene.interpolate(camera, camera.copy(distance = 100f), 0.5f)
        expect(near(halfway.distance, 260f), "flights ease between cameras")
        assertEq(2400f, camera.copy(distance = 1e6f).clamped().distance)
    }

    fun testColours() {
        assertEq(0xFF6CB6FF.toInt(), SynapseColours.lobe(0))
        assertEq(SynapseColours.lobe(1), SynapseColours.lobe(13))
        assertEq(0xFFF34B7D.toInt(), SynapseColours.language("C++"))
        assertEq(0xFF3FB98F.toInt(), SynapseColours.health(9.0))
        assertEq(0xFFE5534B.toInt(), SynapseColours.health(1.0))
        assertEq(0xFF3A4D72.toInt(), SynapseColours.hotspot(0.0))
        assertEq(0xFFFF6F3C.toInt(), SynapseColours.hotspot(1.0))
        assertEq(0x80FFFFFF.toInt(), SynapseColours.withAlpha(SynapseColours.WHITE, 0.5f))
        val lighter = SynapseColours.lighter(0xFF336699.toInt(), 150)
        expect((lighter and 0xff) > 0x99, "lighter brightens")
    }

    fun testSearchPrefersConnectedFiles() {
        val snapshot = smallSnapshot()
        val graph = Synapse.buildGraph(snapshot, false)
        val hits = SynapseScene.searchMatches(snapshot, ".cpp", 10, graph)
        assertEq("docs/lonely.cpp", snapshot.files[hits.last()].path)
        assertEq(listOf(snapshot.indexOf("util/log.cpp")), SynapseScene.searchMatches(snapshot, "util/log.cpp", 1, graph))
        assertEq(emptyList<Int>(), SynapseScene.searchMatches(snapshot, "  ", 5, graph))
    }

    fun testDustAndStars() {
        val shape = SynapseBrainShape()
        val dust = SynapseScene.dust(shape)
        expect(dust.size % 3 == 0 && dust.size / 3 in 1000..1800, "cortex dust points (${dust.size / 3})")
        for (i in 0 until dust.size / 3) expect(abs(dust[i * 3]) >= shape.fissure * 0.6f, "dust avoids the fissure")
        assertEq(420 * 4, SynapseScene.stars().size)
    }
}
