package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs
import kotlin.math.max

class InsightGoldenTest {
    private val expected: Map<String, Any?> by lazy {
        @Suppress("UNCHECKED_CAST")
        Json.parse(InsightFixtures.expectedText) as Map<String, Any?>
    }

    private fun baseSnapshot(): InsightSnapshot {
        val snapshot = Insight.analyze(
            MemorySource(InsightFixtures.repoFiles),
            InsightOptions(history = false),
            now = InsightFixtures.NOW,
        )
        assertEq("", snapshot.error, "base analysis error")
        return snapshot
    }

    private fun historySnapshot(base: InsightSnapshot): InsightSnapshot {
        val history = base.deepCopy()
        Insight.applyHistory(history, Insight.parseGitLog(InsightFixtures.historyLog), InsightOptions(), InsightFixtures.NOW)
        for (file in history.files) {
            val health = Insight.computeHealth(file)
            file.health = health.score
            file.healthIssues = health.issues
        }
        Insight.computeHotspots(history)
        return history
    }

    private fun pathList(snapshot: InsightSnapshot, indexes: List<Int>): List<String> =
        indexes.map { snapshot.files[it].path }.sorted()

    private fun snapshotJson(snapshot: InsightSnapshot): Map<String, Any?> {
        val files = snapshot.files.map { file ->
            mapOf(
                "path" to file.path,
                "language" to file.language,
                "bytes" to file.bytes,
                "lines" to file.lines,
                "codeLines" to file.codeLines,
                "commentLines" to file.commentLines,
                "blankLines" to file.blankLines,
                "types" to file.types,
                "abstractTypes" to file.abstractTypes,
                "complexity" to file.complexity,
                "maxComplexity" to file.maxComplexity,
                "maxNesting" to file.maxNesting,
                "duplicatedLines" to file.duplicatedLines,
                "test" to file.test,
                "functions" to file.functions.map {
                    mapOf(
                        "name" to it.name, "line" to it.line, "length" to it.length,
                        "complexity" to it.complexity, "nesting" to it.nesting, "parameters" to it.parameters,
                    )
                },
                "imports" to file.imports,
                "externalImports" to file.externalImports,
                "declares" to file.declares,
                "references" to file.references,
                "dependsOn" to pathList(snapshot, file.dependsOn),
                "dependedOnBy" to pathList(snapshot, file.dependedOnBy),
                "commits" to file.commits,
                "recentCommits" to file.recentCommits,
                "authors" to file.authors,
                "fixCommits" to file.fixCommits,
                "linesAdded" to file.linesAdded,
                "linesDeleted" to file.linesDeleted,
                "lastChanged" to file.lastChanged,
                "firstChanged" to file.firstChanged,
                "mainAuthor" to file.mainAuthor,
                "mainAuthorShare" to file.mainAuthorShare,
                "formerAuthorShare" to file.formerAuthorShare,
                "topAuthors" to file.topAuthors.map { listOf(it.name, it.commits) },
                "health" to file.health,
                "healthIssues" to file.healthIssues,
                "healthBand" to Insight.healthBand(file.health).label,
                "hotspot" to file.hotspot,
                "inCycle" to (file.cycle >= 0),
            )
        }
        val couplings = snapshot.couplings
            .sortedBy { snapshot.files[it.a].path + "\n" + snapshot.files[it.b].path }
            .map { mapOf("a" to snapshot.files[it.a].path, "b" to snapshot.files[it.b].path, "shared" to it.shared, "degree" to it.degree) }
        return mapOf(
            "files" to files,
            "cycles" to snapshot.cycles.map { pathList(snapshot, it) },
            "couplings" to couplings,
            "packageRoots" to snapshot.packageRoots,
            "authors" to snapshot.authors,
            "formerAuthors" to snapshot.formerAuthors,
            "commitsScanned" to snapshot.commitsScanned,
            "historyStart" to snapshot.historyStart,
            "historyEnd" to snapshot.historyEnd,
            "historyDays" to snapshot.historyDays,
            "skippedFiles" to snapshot.skippedFiles,
            "totalCodeLines" to snapshot.totalCodeLines,
            "totalComplexity" to snapshot.totalComplexity,
        )
    }

    private fun moduleJson(snapshot: InsightSnapshot, depth: Int): Map<String, Any?> {
        val graph = Insight.buildModuleGraph(snapshot, depth)
        return mapOf(
            "depth" to graph.depth,
            "nodes" to graph.nodes.map { node ->
                mapOf(
                    "path" to node.path,
                    "name" to node.name,
                    "files" to pathList(snapshot, node.files),
                    "codeLines" to node.codeLines,
                    "complexity" to node.complexity,
                    "functions" to node.functions,
                    "types" to node.types,
                    "abstractTypes" to node.abstractTypes,
                    "afferent" to node.afferent,
                    "efferent" to node.efferent,
                    "internalEdges" to node.internalEdges,
                    "instability" to node.instability,
                    "abstractness" to node.abstractness,
                    "distance" to node.distance,
                    "cohesion" to node.cohesion,
                    "health" to node.health,
                    "commits" to node.commits,
                    "inCycle" to (node.cycle >= 0),
                    "layer" to node.layer,
                )
            },
            "edges" to graph.edges.map {
                mapOf(
                    "from" to graph.nodes[it.from].path,
                    "to" to graph.nodes[it.to].path,
                    "weight" to it.weight,
                    "inCycle" to it.inCycle,
                )
            },
            "cycles" to graph.cycles.map { cycle -> cycle.map { graph.nodes[it].path }.sorted() },
        )
    }

    private fun compare(path: String, want: Any?, got: Any?, out: MutableList<String>) {
        when {
            want is Number && got is Number -> {
                val a = want.toDouble()
                val b = got.toDouble()
                if (abs(a - b) > 1e-9 * max(1.0, max(abs(a), abs(b)))) out.add("$path: expected $want but was $got")
            }
            want is Map<*, *> && got is Map<*, *> -> {
                if (want.keys != got.keys) out.add("$path: keys ${want.keys} vs ${got.keys}")
                for (key in want.keys) if (got.containsKey(key)) compare("$path.$key", want[key], got[key], out)
            }
            want is List<*> && got is List<*> -> {
                if (want.size != got.size) {
                    out.add("$path: expected ${want.size} items $want but was ${got.size} items $got")
                    return
                }
                for (i in want.indices) compare("$path[$i]", want[i], got[i], out)
            }
            else -> if (want != got) out.add("$path: expected <$want> but was <$got>")
        }
    }

    private fun assertMatches(section: String, want: Any?, got: Any?) {
        val mismatches = ArrayList<String>()
        compare(section, want, got, mismatches)
        if (mismatches.isNotEmpty()) {
            throw AssertionError("${mismatches.size} golden mismatch(es):\n" + mismatches.take(40).joinToString("\n"))
        }
    }

    fun testFixtureIsFound() {
        expect(InsightFixtures.repoFiles.size >= 50, "fixture repo has ${InsightFixtures.repoFiles.size} files")
        assertEq(InsightFixtures.NOW, (expected["now"] as Number).toLong())
    }

    fun testComparisonDetectsDifferences() {
        val base = baseSnapshot()
        val target = base.files.first { it.functions.isNotEmpty() }
        target.functions[0].complexity += 1
        target.mainAuthorShare = 0.5
        base.couplings = listOf(InsightCoupling(0, 1, 3, 1.0))
        val mismatches = ArrayList<String>()
        compare("base", expected["base"], snapshotJson(base), mismatches)
        assertEq(3, mismatches.size, mismatches.joinToString("\n"))
    }

    fun testProbes() {
        @Suppress("UNCHECKED_CAST")
        val probes = expected["probes"] as List<Map<String, Any?>>
        assertEq(45, probes.size)
        val got = probes.map {
            val path = it["path"] as String
            mapOf(
                "path" to path,
                "language" to Insight.languageForPath(path),
                "test" to Insight.isTestPath(path),
                "vendored" to Insight.isVendoredPath(path),
            )
        }
        assertMatches("probes", probes, got)
    }

    fun testBaseSnapshot() {
        assertMatches("base", expected["base"], snapshotJson(baseSnapshot()))
    }

    fun testHistorySnapshot() {
        assertMatches("history", expected["history"], snapshotJson(historySnapshot(baseSnapshot())))
    }

    fun testModuleGraphs() {
        val history = historySnapshot(baseSnapshot())
        val got = mapOf("auto" to moduleJson(history, 0), "depth1" to moduleJson(history, 1), "depth2" to moduleJson(history, 2))
        assertMatches("modules", expected["modules"], got)
    }

    fun testPipelineWithHistoryMatchesGolden() {
        val snapshot = Insight.analyze(
            MemorySource(InsightFixtures.repoFiles, log = Insight.parseGitLog(InsightFixtures.historyLog)),
            InsightOptions(),
            now = InsightFixtures.NOW,
        )
        assertMatches("history", expected["history"], snapshotJson(snapshot))
    }

    fun testHistoryDoesNotChangeTheBaseSnapshot() {
        val base = baseSnapshot()
        historySnapshot(base)
        assertMatches("base", expected["base"], snapshotJson(base))
    }
}
