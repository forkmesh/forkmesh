package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import kotlin.math.abs
import kotlin.math.max

class SynapseGoldenTest {
    private val expected: Map<String, Any?> by lazy {
        @Suppress("UNCHECKED_CAST")
        Json.parse(File(InsightFixtures.fixture, "synapse.json").readText(Charsets.UTF_8)) as Map<String, Any?>
    }

    private val snapshot: InsightSnapshot by lazy {
        val base = Insight.analyze(MemorySource(InsightFixtures.repoFiles), InsightOptions(history = false), now = InsightFixtures.NOW)
        Insight.applyHistory(base, Insight.parseGitLog(InsightFixtures.historyLog), InsightOptions(), InsightFixtures.NOW)
        for (file in base.files) {
            val health = Insight.computeHealth(file)
            file.health = health.score
            file.healthIssues = health.issues
        }
        Insight.computeHotspots(base)
        base
    }

    private fun paths(indexes: List<Int>): List<String> = indexes.map { snapshot.files[it].path }

    @Suppress("UNCHECKED_CAST")
    private fun obj(value: Any?): Map<String, Any?> = value as Map<String, Any?>

    @Suppress("UNCHECKED_CAST")
    private fun list(value: Any?): List<Any?> = value as List<Any?>

    private fun floats(value: Any?): FloatArray {
        val out = ArrayList<Float>()
        for (point in list(value)) for (c in list(point)) out.add((c as Number).toFloat())
        return out.toFloatArray()
    }

    private fun maxDeviation(want: FloatArray, got: FloatArray): Float {
        assertEq(want.size, got.size, "position count")
        var worst = 0f
        for (i in want.indices) worst = max(worst, abs(want[i] - got[i]))
        return worst
    }

    private fun graph(includeIsolated: Boolean): Pair<SynapseGraph, Map<String, Any?>> {
        val key = if (includeIsolated) "withOrphans" else "connected"
        return Synapse.buildGraph(snapshot, includeIsolated) to obj(obj(expected["graphs"])[key])
    }

    fun testFixtureMatchesTheSnapshot() {
        assertEq((expected["files"] as Number).toInt(), snapshot.files.size, "file count")
        assertEq("native/fixtures/insight", expected["fixture"])
    }

    fun testStableHash() {
        for (row in list(expected["hashes"])) {
            val entry = obj(row)
            val text = entry["text"] as String
            assertEq((entry["hash"] as Number).toLong(), Synapse.stableHashUnsigned(text), "hash of '$text'")
        }
    }

    fun testQueries() {
        assertEq((expected["importCount"] as Number).toInt(), Synapse.importCount(snapshot), "importCount")
        assertEq(expected["orphans"], paths(Synapse.orphans(snapshot)), "orphans")
        assertEq(expected["leaves"], paths(Synapse.leaves(snapshot)), "leaves")
        assertEq(expected["mostImported"], paths(Synapse.mostImported(snapshot, 10)), "mostImported")
        assertEq(expected["mostImporting"], paths(Synapse.mostImporting(snapshot, 10)), "mostImporting")
        assertEq(expected["mostImportedAll"], paths(Synapse.mostImported(snapshot, -1)), "mostImported all")
        assertEq(expected["mostImportingAll"], paths(Synapse.mostImporting(snapshot, -1)), "mostImporting all")
        assertEq(expected["mostConnected"], snapshot.files[Synapse.mostConnected(snapshot)].path, "mostConnected")
    }

    fun testCyclePaths() {
        val cycles = list(expected["cycles"])
        assertEq(cycles.size, snapshot.cycles.size, "cycle count")
        for ((c, row) in cycles.withIndex()) {
            val entry = obj(row)
            val path = Synapse.cyclePath(snapshot, snapshot.cycles[c])
            assertEq(entry["files"], paths(snapshot.cycles[c]), "cycle $c files")
            assertEq(entry["path"], paths(path), "cycle $c path")
            assertEq(entry["text"], Synapse.cycleText(snapshot, path), "cycle $c text")
        }
    }

    fun testNeighbourhoods() {
        val rows = list(expected["neighbourhoods"])
        assertEq(snapshot.files.size * 3, rows.size, "neighbourhood rows")
        for (row in rows) {
            val entry = obj(row)
            val file = snapshot.indexOf(entry["file"] as String)
            val hops = (entry["hops"] as Number).toInt()
            val got = Synapse.neighbourhood(snapshot, file, hops, SynapseDirection.Both)
            val imports = got.imports.entries.associate { snapshot.files[it.key].path to it.value.toLong() }
            val importers = got.importers.entries.associate { snapshot.files[it.key].path to it.value.toLong() }
            val wantImports = obj(entry["imports"]).mapValues { (it.value as Number).toLong() }
            val wantImporters = obj(entry["importers"]).mapValues { (it.value as Number).toLong() }
            assertEq(wantImports, imports, "imports of ${entry["file"]} at $hops hops")
            assertEq(wantImporters, importers, "importers of ${entry["file"]} at $hops hops")
            val onlyImports = Synapse.neighbourhood(snapshot, file, hops, SynapseDirection.Imports)
            expect(onlyImports.importers.isEmpty() && onlyImports.imports == got.imports, "imports-only walk of ${entry["file"]}")
        }
    }

    private fun checkGraph(includeIsolated: Boolean) {
        val (graph, want) = graph(includeIsolated)
        val nodePath = { n: Int -> snapshot.files[graph.nodes[n].file].path }
        assertEq(want["includesIsolated"], graph.includesIsolated)
        assertEq((want["imports"] as Number).toInt(), graph.imports, "imports")
        assertEq((want["spacing"] as Number).toFloat(), graph.spacing, "spacing")
        assertEq((want["iterations"] as Number).toInt(), Synapse.layoutIterations(graph), "iterations")
        val lobes = list(want["lobes"])
        assertEq(lobes.size, graph.lobes.size, "lobe count")
        for ((l, row) in lobes.withIndex()) {
            val entry = obj(row)
            val lobe = graph.lobes[l]
            assertEq(entry["name"], lobe.name, "lobe $l name")
            assertEq(entry["path"], lobe.path, "lobe $l path")
            assertEq((entry["layer"] as Number).toInt(), lobe.layer, "lobe $l layer")
            assertEq((entry["hemisphere"] as Number).toInt(), lobe.hemisphere, "lobe $l hemisphere")
            assertEq(entry["nodes"], lobe.nodes.map(nodePath), "lobe $l nodes")
            val anchor = floats(listOf(entry["anchor"]))
            val got = floatArrayOf(lobe.anchorX, lobe.anchorY, lobe.anchorZ)
            expect(maxDeviation(anchor, got) <= ANCHOR_TOLERANCE, "lobe $l anchor ${anchor.toList()} vs ${got.toList()}")
        }
        val nodes = list(want["nodes"])
        assertEq(nodes.size, graph.nodes.size, "node count")
        for ((n, row) in nodes.withIndex()) {
            val entry = obj(row)
            val node = graph.nodes[n]
            assertEq(entry["path"], nodePath(n), "node $n path")
            assertEq((entry["lobe"] as Number).toInt(), node.lobe, "node $n lobe")
            assertEq((entry["hemisphere"] as Number).toInt(), node.hemisphere, "node $n hemisphere")
            assertEq((entry["size"] as Number).toFloat(), node.size, "node $n size")
            assertEq((entry["fanIn"] as Number).toInt(), node.fanIn, "node $n fanIn")
            assertEq((entry["fanOut"] as Number).toInt(), node.fanOut, "node $n fanOut")
            assertEq(entry["cycle"], node.cycle, "node $n cycle")
            assertEq((entry["seed"] as Number).toLong(), node.seed.toLong() and 0xffffffffL, "node $n seed")
        }
        val edges = list(want["edges"])
        assertEq(edges.size, graph.edges.size, "edge count")
        for ((e, row) in edges.withIndex()) {
            val entry = obj(row)
            val edge = graph.edges[e]
            assertEq(entry["from"], nodePath(edge.from), "edge $e from")
            assertEq(entry["to"], nodePath(edge.to), "edge $e to")
            assertEq(entry["cycle"], edge.cycle, "edge $e cycle")
            assertEq((entry["seed"] as Number).toLong(), edge.seed.toLong() and 0xffffffffL, "edge $e seed")
        }
        val cycles = list(want["cycles"])
        assertEq(cycles.size, graph.cycles.size, "graph cycle count")
        for ((c, row) in cycles.withIndex()) {
            val entry = obj(row)
            val info = graph.cycles[c]
            assertEq((entry["index"] as Number).toInt(), info.index, "graph cycle $c index")
            assertEq(entry["files"], paths(info.files), "graph cycle $c files")
            assertEq(entry["path"], paths(info.path), "graph cycle $c path")
            assertEq(entry["edges"], info.edges.map { listOf(nodePath(graph.edges[it].from), nodePath(graph.edges[it].to)) }, "graph cycle $c edges")
        }
        val seeds = floats(want["seeds"])
        val seedDeviation = maxDeviation(seeds, Synapse.seedPositions(graph))
        expect(seedDeviation <= SEED_TOLERANCE, "seed positions deviate by $seedDeviation")
        for (row in list(want["layouts"])) {
            val entry = obj(row)
            val iterations = (entry["iterations"] as Number).toInt()
            val positions = Synapse.runLayout(graph, SynapseLayoutOptions(iterations = iterations))
            val deviation = maxDeviation(floats(entry["positions"]), positions)
            val centres = maxDeviation(floats(entry["lobeCentres"]), Synapse.lobeCentres(graph, positions))
            val tolerance = if (iterations <= 10) SEED_TOLERANCE else LAYOUT_TOLERANCE
            expect(deviation <= tolerance, "layout of $iterations iterations deviates by $deviation")
            expect(centres <= tolerance, "lobe centres after $iterations iterations deviate by $centres")
        }
    }

    fun testConnectedGraphAndLayout() = checkGraph(false)

    fun testGraphWithOrphansAndLayout() = checkGraph(true)

    fun testSummary() {
        val text = Synapse.summary(snapshot)
        val orphans = list(expected["orphans"]).size
        val cycles = list(expected["cycles"]).size
        assertEq("${expected["importCount"]} imports · $cycles circular dependencies · $orphans orphans", text)
    }

    private companion object {
        const val ANCHOR_TOLERANCE = 1e-5f
        const val SEED_TOLERANCE = 1e-4f
        const val LAYOUT_TOLERANCE = 1e-2f
    }
}
