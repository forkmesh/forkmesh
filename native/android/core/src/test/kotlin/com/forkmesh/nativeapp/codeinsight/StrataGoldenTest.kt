package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import kotlin.math.abs
import kotlin.math.max

class StrataGoldenTest {
    private val expected: Map<String, Any?> by lazy {
        @Suppress("UNCHECKED_CAST")
        Json.parse(File(InsightFixtures.fixture, "strata.json").readText(Charsets.UTF_8)) as Map<String, Any?>
    }

    private val base: InsightSnapshot by lazy {
        val snapshot = Insight.analyze(
            MemorySource(InsightFixtures.repoFiles),
            InsightOptions(history = false),
            now = InsightFixtures.NOW,
        )
        assertEq("", snapshot.error, "base analysis error")
        snapshot.head = "0000000000000000000000000000000000000000"
        snapshot.branch = "main"
        snapshot.analyzedAt = 0
        snapshot
    }

    private val history: InsightSnapshot by lazy {
        val snapshot = base.deepCopy()
        Insight.applyHistory(snapshot, Insight.parseGitLog(InsightFixtures.historyLog), InsightOptions(), InsightFixtures.NOW)
        for (file in snapshot.files) {
            val health = Insight.computeHealth(file)
            file.health = health.score
            file.healthIssues = health.issues
        }
        Insight.computeHotspots(snapshot)
        snapshot
    }

    private fun pathOf(snapshot: InsightSnapshot, file: Int): String? =
        if (file >= 0 && file < snapshot.files.size) snapshot.files[file].path else null

    private fun moduleOf(graph: InsightModuleGraph, module: Int): String? =
        if (module >= 0 && module < graph.nodes.size) graph.nodes[module].path else null

    private fun scoreJson(score: StrataScore): MutableMap<String, Any?> = linkedMapOf(
        "debt" to score.debt,
        "effort" to score.effort,
        "ratio" to score.ratio,
        "rating" to score.rating,
        "interest" to score.interest,
        "violations" to score.violations,
        "worst" to score.worst,
    )

    private fun rectJson(rect: StrataRect) = listOf(rect.x, rect.y, rect.width, rect.height)

    private fun layoutJson(layout: StrataGraphLayout): Map<String, Any?> = mapOf(
        "boxes" to layout.boxes.map(::rectJson),
        "ranks" to layout.ranks,
        "edges" to layout.edges.map { edge ->
            mapOf(
                "from" to edge.from,
                "to" to edge.to,
                "weight" to edge.weight,
                "cycle" to edge.cycle,
                "flat" to edge.flat,
                "points" to edge.points.map { listOf(it.x, it.y) },
            )
        },
        "bounds" to rectJson(layout.bounds),
        "crossings" to layout.crossings,
        "rankCount" to layout.rankCount,
    )

    private fun treemapJson(snapshot: InsightSnapshot, graph: InsightModuleGraph, sizeBy: StrataTreemapSize): Map<String, Any?> {
        val map = StrataLayout.treemap(snapshot, graph, sizeBy, StrataRect(14.0, 6.0, 972.0, 648.0))
        return mapOf("modules" to map.modules.map(::rectJson), "files" to map.files.map(::rectJson))
    }

    private fun refsJson(report: StrataReport, refs: List<StrataRef>) = refs.map { listOf(report.rules[it.rule].id, it.index) }

    private fun modelJson(snapshot: InsightSnapshot, depth: Int): Map<String, Any?> {
        val model = Strata.buildModel(snapshot, depth)
        val report = model.report
        val graph = report.graph
        val m = report.metrics
        return mapOf(
            "depth" to graph.depth,
            "summary" to Strata.summaryLine(report),
            "history" to report.history,
            "total" to scoreJson(report.total),
            "metrics" to mapOf(
                "codeLines" to m.codeLines,
                "commentLines" to m.commentLines,
                "files" to m.files,
                "modules" to m.modules,
                "types" to m.types,
                "functions" to m.functions,
                "averageComplexity" to m.averageComplexity,
                "maxComplexity" to m.maxComplexity,
                "maxComplexityFunction" to m.maxComplexityFunction,
                "maxComplexityFile" to pathOf(snapshot, m.maxComplexityFile),
                "commentPercent" to m.commentPercent,
                "duplicatedPercent" to m.duplicatedPercent,
                "testFiles" to m.testFiles,
            ),
            "bySeverity" to report.bySeverity,
            "cycles" to graph.cycles.map { cycle -> cycle.map { graph.nodes[it].path } },
            "nodes" to graph.nodes.map {
                mapOf(
                    "path" to it.path,
                    "cycle" to it.cycle,
                    "afferent" to it.afferent,
                    "efferent" to it.efferent,
                    "instability" to it.instability,
                    "abstractness" to it.abstractness,
                    "distance" to it.distance,
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
            "rules" to report.rules.map { rule ->
                mapOf(
                    "id" to rule.id,
                    "title" to rule.title,
                    "category" to rule.category.label,
                    "severity" to rule.severity.label,
                    "moduleScope" to rule.moduleScope,
                    "debt" to rule.debt,
                    "worst" to rule.worst,
                    "violations" to rule.violations.map { v ->
                        mapOf(
                            "file" to pathOf(snapshot, v.file),
                            "module" to moduleOf(graph, v.module),
                            "peerModule" to moduleOf(graph, v.peerModule),
                            "line" to v.line,
                            "severity" to v.severity.label,
                            "message" to v.message,
                            "debt" to v.debt,
                        )
                    },
                )
            },
            "files" to snapshot.files.indices.map { f ->
                scoreJson(report.files[f]).apply {
                    put("path", snapshot.files[f].path)
                    put("refs", refsJson(report, report.fileViolations[f]))
                }
            },
            "modules" to graph.nodes.indices.map { i ->
                scoreJson(report.modules[i]).apply {
                    put("path", graph.nodes[i].path)
                    put("refs", refsJson(report, report.moduleViolations[i]))
                    put("worstRefs", refsJson(report, report.worstForModule(i, 6)))
                }
            },
            "gates" to report.gates.map {
                mapOf("name" to it.name, "status" to it.status.label, "measured" to it.measured, "detail" to it.detail)
            },
            "order" to model.order.map { graph.nodes[it].path },
            "layers" to StrataLayout.moduleLayers(graph),
            "layout" to layoutJson(model.layout),
            "drill" to graph.nodes.indices.map { module ->
                val drill = StrataLayout.drillGraph(snapshot, graph, module)
                mapOf(
                    "module" to graph.nodes[module].path,
                    "hiddenFiles" to drill.hiddenFiles,
                    "nodes" to drill.nodes.map {
                        mapOf(
                            "module" to it.module,
                            "dependent" to it.dependent,
                            "name" to if (it.module) moduleOf(graph, it.index) else pathOf(snapshot, it.index),
                            "weight" to it.weight,
                        )
                    },
                    "links" to drill.links.map { listOf(it.from, it.to, it.weight, it.cycle) },
                    "layout" to layoutJson(drill.layout),
                )
            },
            "boxSizes" to listOf(0, 1, 100, 1000, 5000, 20000, 100000).map {
                val size = StrataLayout.moduleBoxSize(it)
                listOf(it, size.width, size.height)
            },
            "treemapLines" to treemapJson(snapshot, graph, StrataTreemapSize.Lines),
            "treemapComplexity" to treemapJson(snapshot, graph, StrataTreemapSize.Complexity),
            "treemapFunctions" to treemapJson(snapshot, graph, StrataTreemapSize.Functions),
            "report" to StrataReportHtml.render(snapshot, report, "fixture <repo>"),
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
            want is String && got is String && want != got -> {
                val at = want.indices.firstOrNull { it >= got.length || want[it] != got[it] } ?: want.length
                out.add("$path: strings differ at $at: expected <${want.substring(maxOf(0, at - 60), minOf(want.length, at + 80))}> but was <${got.substring(maxOf(0, at - 60), minOf(got.length, at + 80))}>")
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

    fun testGoldenIsFound() {
        assertEq(InsightFixtures.NOW, (expected["now"] as Number).toLong())
        expect(expected.containsKey("synthetic"), "strata.json has the synthetic model")
    }

    fun testComparisonDetectsDifferences() {
        val got = modelJson(history, 0).toMutableMap()
        got["summary"] = "Rating A"
        got["order"] = (got["order"] as List<*>).reversed()
        @Suppress("UNCHECKED_CAST")
        val layout = (got["layout"] as Map<String, Any?>).toMutableMap()
        layout["crossings"] = (layout["crossings"] as Int) + 1
        got["layout"] = layout
        val mismatches = ArrayList<String>()
        compare("auto", expected["auto"], got, mismatches)
        expect(mismatches.size >= 3, mismatches.joinToString("\n"))
        expect(mismatches.any { it.startsWith("auto.summary") } && mismatches.any { it.startsWith("auto.layout.crossings") }, mismatches.joinToString("\n"))
    }

    fun testFormatting() {
        @Suppress("UNCHECKED_CAST")
        val debts = expected["formatDebt"] as List<List<Any?>>
        assertMatches("formatDebt", debts, debts.map { listOf(it[0], Strata.formatDebt((it[0] as Number).toDouble())) })
        @Suppress("UNCHECKED_CAST")
        val percents = expected["formatPercent"] as List<List<Any?>>
        assertMatches("formatPercent", percents, percents.map { listOf(it[0], Strata.formatPercent((it[0] as Number).toDouble())) })
    }

    fun testLayouts() {
        val mesh = StrataLayoutInput(
            sizes = (0 until 30).map { StrataSize(100.0 + (it % 4) * 30, 70.0 + (it % 3) * 20) },
            links = (0 until 30).flatMap { i ->
                (1..3).mapNotNull { k ->
                    val j = i + k * 3 + (i % 5)
                    if (j < 30) StrataLink(i, j, 1 + (i + j) % 7, false) else null
                }
            },
        )
        val hinted = StrataLayoutInput(
            sizes = List(6) { StrataSize(120.0, 80.0) },
            layers = listOf(3, 2, 2, 1, 0, 0),
            links = listOf(StrataLink(0, 1), StrataLink(0, 2), StrataLink(1, 3), StrataLink(2, 4), StrataLink(3, 5)),
        )
        val cyclic = StrataLayoutInput(
            sizes = List(7) { StrataSize(120.0 + it * 7, 80.0) },
            links = listOf(
                StrataLink(0, 1, 2, true), StrataLink(1, 2, 1, true), StrataLink(2, 0, 3, true), StrataLink(2, 3, 1, false),
                StrataLink(3, 4, 2, true), StrataLink(4, 5, 1, true), StrataLink(5, 3, 4, true), StrataLink(4, 3, 1, true),
                StrataLink(0, 6, 1, false),
            ),
        )
        val wide = StrataLayoutInput(
            sizes = listOf(StrataSize(140.0, 80.0)) + List(40) { StrataSize(120.0, 70.0) },
            links = (1..40).map { StrataLink(0, it) },
        )
        val isolated = StrataLayoutInput(sizes = List(5) { StrataSize(100.0, 60.0) }, links = listOf(StrataLink(0, 1)))
        val tall = StrataLayoutInput(sizes = List(12) { StrataSize(110.0, 70.0) }, links = (0 until 7).map { StrataLink(it, it + 1) })
        val lonely = StrataLayoutInput(sizes = List(9) { StrataSize(90.0 + it * 10, 60.0) })
        val squares = StrataLayout.squarify(listOf(50.0, 25.0, 12.5, 12.5, 0.0, 3.0, 7.25, 1.0), StrataRect(3.0, 5.0, 400.0, 200.0))
        val got = mapOf(
            "mesh" to layoutJson(StrataLayout.layeredLayout(mesh)),
            "hinted" to layoutJson(StrataLayout.layeredLayout(hinted)),
            "cyclic" to layoutJson(StrataLayout.layeredLayout(cyclic)),
            "wide" to layoutJson(StrataLayout.layeredLayout(wide)),
            "isolated" to layoutJson(StrataLayout.layeredLayout(isolated)),
            "tall" to layoutJson(StrataLayout.layeredLayout(tall)),
            "lonely" to layoutJson(StrataLayout.layeredLayout(lonely)),
            "squarify" to squares.map(::rectJson),
        )
        assertMatches("layouts", expected["layouts"], got)
    }

    fun testAutoDepthModel() {
        assertMatches("auto", expected["auto"], modelJson(history, 0))
    }

    fun testDepthTwoModel() {
        assertMatches("depth2", expected["depth2"], modelJson(history, 2))
    }

    fun testModelWithoutHistory() {
        assertMatches("plain", expected["plain"], modelJson(base, 0))
    }

    fun testSyntheticModel() {
        assertMatches("synthetic", expected["synthetic"], modelJson(StrataSynthetic.rich(), 1))
    }
}
