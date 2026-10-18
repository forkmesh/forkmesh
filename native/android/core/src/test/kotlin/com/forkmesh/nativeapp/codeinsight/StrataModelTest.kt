package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import kotlin.math.abs
import kotlin.math.max

object StrataSynthetic {
    fun file(path: String, codeLines: Int) = InsightFile(
        path = path,
        language = "C++",
        codeLines = codeLines,
        lines = codeLines + 10,
        commentLines = codeLines / 5,
        health = 9.0,
    )

    fun function(name: String, line: Int, length: Int, complexity: Int, nesting: Int = 1, parameters: Int = 1) =
        InsightFunction(name, line, length, complexity, nesting, parameters)

    fun link(files: List<InsightFile>, from: Int, to: Int) {
        files[from].dependsOn = files[from].dependsOn + to
        files[to].dependedOnBy = files[to].dependedOnBy + from
    }

    fun rich(): InsightSnapshot {
        val files = listOf(
            file("app/main.cpp", 300),
            file("app/window.cpp", 400),
            file("core/engine.cpp", 2500),
            file("core/model.cpp", 200),
            file("util/strings.cpp", 400),
            file("api/interfaces.h", 120),
        )
        files[0].functions = listOf(function("main", 10, 30, 3))
        files[2].functions = listOf(function("Engine::run", 40, 140, 26, 6, 8), function("Engine::step", 300, 20, 12))
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
        for (f in files) {
            if (f.maxComplexity == 0) for (fn in f.functions) f.maxComplexity = max(f.maxComplexity, fn.complexity)
        }
        link(files, 0, 1)
        link(files, 1, 0)
        link(files, 0, 2)
        link(files, 1, 3)
        link(files, 2, 1)
        link(files, 2, 4)
        link(files, 3, 4)
        link(files, 5, 3)
        link(files, 0, 4)
        return InsightSnapshot(
            source = "/tmp/strata-synthetic",
            name = "synthetic",
            historyDays = 365,
            files = files,
            authors = listOf("Author One", "Author Two"),
        )
    }

    fun graphFor(snapshot: InsightSnapshot, modules: List<String>): InsightModuleGraph {
        val nodes = modules.map { InsightModuleNode(path = it, name = it) }
        val nodeFiles = List(modules.size) { ArrayList<Int>() }
        val moduleOfFile = IntArray(snapshot.files.size) { -1 }
        for ((f, file) in snapshot.files.withIndex()) {
            val m = modules.indexOf(file.path.substringBefore('/'))
            if (m < 0) continue
            moduleOfFile[f] = m
            nodeFiles[m].add(f)
            val node = nodes[m]
            node.codeLines += file.codeLines
            node.complexity += file.complexity
            node.functions += file.functions.size
            node.types += file.types
            node.abstractTypes += file.abstractTypes
        }
        val edges = ArrayList<InsightModuleEdge>()
        val edgeOf = HashMap<Int, Int>()
        val fileEdges = ArrayList<ArrayList<Pair<Int, Int>>>()
        for ((f, file) in snapshot.files.withIndex()) {
            for (d in file.dependsOn) {
                val a = moduleOfFile[f]
                val b = moduleOfFile[d]
                if (a < 0 || b < 0) continue
                if (a == b) {
                    nodes[a].internalEdges++
                    continue
                }
                val at = edgeOf.getOrPut(a * 1000 + b) {
                    edges.add(InsightModuleEdge(from = a, to = b))
                    fileEdges.add(ArrayList())
                    edges.size - 1
                }
                edges[at].weight++
                fileEdges[at].add(f to d)
            }
        }
        for ((i, edge) in edges.withIndex()) edge.fileEdges = fileEdges[i]
        for ((i, node) in nodes.withIndex()) {
            node.files = nodeFiles[i]
            val incoming = edges.filter { it.to == i }.map { it.from }.toSet()
            val outgoing = edges.filter { it.from == i }.map { it.to }.toSet()
            node.afferent = incoming.size
            node.efferent = outgoing.size
            val sum = node.afferent + node.efferent
            node.instability = if (sum != 0) node.efferent.toDouble() / sum else 0.0
            node.abstractness = if (node.types != 0) node.abstractTypes.toDouble() / node.types else 0.0
            node.distance = abs(node.abstractness + node.instability - 1.0)
            node.health = 9.0
        }
        return InsightModuleGraph(1, nodes, edges, emptyList(), moduleOfFile.toList())
    }

    fun simple(vararg files: InsightFile): InsightSnapshot = InsightSnapshot(files = files.toList())
}

class StrataModelTest {
    private val modules = listOf("app", "core", "util", "api")

    private fun count(report: StrataReport, id: String): Int {
        val r = report.ruleIndex(id)
        return if (r < 0) -1 else report.rules[r].violations.size
    }

    private fun first(report: StrataReport, id: String): StrataViolation? =
        report.rules.getOrNull(report.ruleIndex(id))?.violations?.firstOrNull()

    private fun gate(report: StrataReport, name: String): StrataGate? = report.gates.firstOrNull { it.name == name }

    private fun overlapping(layout: StrataGraphLayout): Boolean {
        for (i in layout.boxes.indices) for (j in i + 1 until layout.boxes.size) {
            if (layout.boxes[i].adjusted(1.0, 1.0, -1.0, -1.0).intersects(layout.boxes[j].adjusted(1.0, 1.0, -1.0, -1.0))) return true
        }
        return false
    }

    fun testFormatting() {
        assertEq("0 min", Strata.formatDebt(0.0))
        assertEq("45 min", Strata.formatDebt(45.0))
        assertEq("3 h 20 min", Strata.formatDebt(200.0))
        assertEq("6 d 3 h", Strata.formatDebt(6 * 8 * 60 + 3 * 60.0))
        assertEq("2 d", Strata.formatDebt(16 * 60.0))
        expect(abs(Strata.effortMinutes(1000) - 18 * 8 * 60) < 1e-9, "18 man-days per 1000 lines of code")
        expect(Strata.ratingFor(0.0) == 0 && Strata.ratingFor(0.05) == 0, "rating A up to 5 %")
        expect(Strata.ratingFor(0.051) == 1 && Strata.ratingFor(0.10) == 1, "rating B up to 10 %")
        assertEq(2, Strata.ratingFor(0.2))
        assertEq(3, Strata.ratingFor(0.5))
        assertEq(4, Strata.ratingFor(0.51))
        assertEq('A', Strata.ratingLetter(0))
        assertEq('E', Strata.ratingLetter(4))
        assertEq(17, Strata.ruleCatalog().size)
    }

    fun testRules() {
        val snapshot = StrataSynthetic.rich()
        val report = Strata.buildReport(snapshot, StrataSynthetic.graphFor(snapshot, modules))
        assertEq(1, report.graph.cycles.size, "module cycle")
        assertEq(2, count(report, "ARC01"))
        val cycle = first(report, "ARC01")
        expect(cycle != null && cycle.severity == StrataSeverity.Critical && cycle.debt >= 30, "module cycle is critical with debt")
        assertEq(3, count(report, "ARC02"))
        assertEq(1, count(report, "ARC04"))
        assertEq(1, count(report, "ARC05"))
        assertEq(2, count(report, "CS01"))
        val complex = first(report, "CS01")
        expect(complex != null && complex.severity == StrataSeverity.High && complex.line == 40, "complexity above 20 is high")
        assertEq(1, count(report, "CS02"))
        assertEq(1, count(report, "CS03"))
        assertEq(1, count(report, "CS04"))
        assertEq(1, count(report, "CS05"))
        assertEq(StrataSeverity.High, first(report, "MNT01")?.severity)
        assertEq(1, count(report, "MNT02"))
        assertEq(1, count(report, "MNT03"))
        assertEq(1, count(report, "MNT04"))
        assertEq(1, count(report, "MNT05"))

        val scaled = StrataSynthetic.simple(StrataSynthetic.file("a/x.cpp", 100), StrataSynthetic.file("a/y.cpp", 100))
        scaled.files[0].functions = listOf(StrataSynthetic.function("f", 1, 10, 12))
        scaled.files[1].functions = listOf(StrataSynthetic.function("g", 1, 10, 40))
        val scaledReport = Strata.buildReport(scaled, StrataSynthetic.graphFor(scaled, listOf("a")))
        val violations = scaledReport.rules[scaledReport.ruleIndex("CS01")].violations
        val small = violations.first { it.file == 0 }.debt
        val big = violations.first { it.file == 1 }.debt
        expect(big > small && small > 0, "debt grows with how far over the threshold")

        val names = listOf("s", "a", "b", "c", "v", "x", "y", "z")
        val sdp = StrataSynthetic.simple(*names.map { StrataSynthetic.file("$it/one.cpp", 100) }.toTypedArray())
        for ((a, b) in listOf(1 to 0, 2 to 0, 3 to 0, 0 to 4, 4 to 5, 4 to 6, 4 to 7)) StrataSynthetic.link(sdp.files, a, b)
        val sdpReport = Strata.buildReport(sdp, StrataSynthetic.graphFor(sdp, names))
        val sdpViolation = first(sdpReport, "ARC03")
        expect(
            count(sdpReport, "ARC03") == 1 && sdpViolation?.module == 0 && sdpViolation.peerModule == 4,
            "stable module depending on a volatile one breaks the stable dependencies principle",
        )

        val hub = StrataSynthetic.simple(*(0 until 25).map { StrataSynthetic.file("h/f$it.cpp", 50) }.toTypedArray())
        for (i in 1..12) StrataSynthetic.link(hub.files, 0, i)
        for (i in 13 until 25) StrataSynthetic.link(hub.files, i, 0)
        assertEq(1, count(Strata.buildReport(hub, StrataSynthetic.graphFor(hub, listOf("h"))), "DES01"))
    }

    fun testDebtAndGates() {
        val snapshot = StrataSynthetic.rich()
        val report = Strata.buildReport(snapshot, StrataSynthetic.graphFor(snapshot, modules))
        val sum = report.rules.sumOf { rule -> rule.violations.sumOf { it.debt } }
        expect(abs(report.total.debt - sum) < 1e-6, "total debt is the sum of violation debts")
        expect(abs(report.modules.sumOf { it.debt } - sum) < 1e-6, "module debts add up to the total")
        val codeLines = 300 + 400 + 2500 + 200 + 400 + 120
        assertEq(codeLines, report.metrics.codeLines)
        expect(abs(report.total.ratio - sum / Strata.effortMinutes(codeLines)) < 1e-9, "debt ratio is debt over effort")
        assertEq(Strata.ratingFor(report.total.ratio), report.total.rating)
        expect(report.total.interest > 0, "annual interest with history")
        expect(report.files[2].interest <= 2 * report.files[2].debt, "interest is capped")
        assertEq(2, report.bySeverity[0])
        assertEq(StrataGateStatus.Fail, gate(report, "No module dependency cycle")?.status)
        assertEq(StrataGateStatus.Fail, gate(report, "No critical rule violated")?.status)
        assertEq(StrataGateStatus.Pass, gate(report, "No function above complexity 50")?.status)
        val summary = Strata.summaryLine(report)
        expect(summary.startsWith("Rating ") && summary.contains("failing"), summary)

        val clean = StrataSynthetic.simple(StrataSynthetic.file("a/x.cpp", 500), StrataSynthetic.file("b/y.cpp", 500))
        clean.files[0].functions = listOf(StrataSynthetic.function("f", 1, 10, 3))
        StrataSynthetic.link(clean.files, 0, 1)
        val cleanReport = Strata.buildReport(clean, StrataSynthetic.graphFor(clean, listOf("a", "b")))
        expect(cleanReport.total.debt == 0.0 && cleanReport.total.rating == 0, "clean code is rated A with no debt")
        expect(cleanReport.gatesWith(StrataGateStatus.Fail) == 0 && cleanReport.gatesWith(StrataGateStatus.Warn) == 0, "clean passes")
        assertEq(0.0, cleanReport.total.interest)
        expect(Strata.summaryLine(cleanReport).contains("all gates pass"), "summary says all gates pass")

        val bad = StrataSynthetic.simple(StrataSynthetic.file("a/x.cpp", 40))
        bad.files[0].functions = listOf(StrataSynthetic.function("monster", 1, 400, 70, 9, 12))
        bad.files[0].commits = 20
        val badReport = Strata.buildReport(bad, StrataSynthetic.graphFor(bad, listOf("a")))
        assertEq(4, badReport.total.rating)
        assertEq(StrataGateStatus.Fail, gate(badReport, "Technical debt rating")?.status)
        assertEq(StrataGateStatus.Fail, gate(badReport, "No function above complexity 50")?.status)
        assertEq(StrataGateStatus.Fail, gate(badReport, "Debt in the most-changed files")?.status)
    }

    fun testLayout() {
        val input = StrataLayoutInput(
            sizes = (0 until 30).map { StrataSize(100.0 + (it % 4) * 30, 70.0 + (it % 3) * 20) },
            links = (0 until 30).flatMap { i ->
                (1..3).mapNotNull { k ->
                    val j = i + k * 3 + (i % 5)
                    if (j < 30) StrataLink(i, j, 1 + (i + j) % 7) else null
                }
            },
        )
        val layout = StrataLayout.layeredLayout(input)
        expect(!overlapping(layout), "layered layout has no overlapping boxes")
        expect(input.links.all { layout.boxes[it.from].bottom < layout.boxes[it.to].top }, "every edge points down")
        assertEq(input.links.size, layout.edges.size)
        assertEq(layout.boxes, StrataLayout.layeredLayout(input).boxes)

        val cyclic = StrataLayoutInput(
            sizes = List(4) { StrataSize(120.0, 80.0) },
            links = listOf(StrataLink(0, 1, 2, true), StrataLink(1, 2, 1, true), StrataLink(2, 0, 3, true), StrataLink(2, 3, 1)),
        )
        val cycleLayout = StrataLayout.layeredLayout(cyclic)
        assertEq(1, cycleLayout.edges.count { it.points.size >= 2 && it.points.last().y < it.points.first().y }, "upward edges")
        expect(cycleLayout.ranks[3] > maxOf(cycleLayout.ranks[0], cycleLayout.ranks[1], cycleLayout.ranks[2]), "dependency below the cycle")
        expect(!overlapping(cycleLayout), "cycle layout has no overlaps")

        val wide = StrataLayout.layeredLayout(
            StrataLayoutInput(
                sizes = listOf(StrataSize(140.0, 80.0)) + List(40) { StrataSize(120.0, 70.0) },
                links = (1..40).map { StrataLink(0, it) },
            ),
        )
        expect(wide.rankCount > 2 && !overlapping(wide) && wide.bounds.width < 40 * 160, "wide rows fold")

        val islands = StrataLayout.layeredLayout(StrataLayoutInput(sizes = List(5) { StrataSize(100.0, 60.0) }, links = listOf(StrataLink(0, 1))))
        expect(
            !overlapping(islands) && (islands.boxes[4].top > islands.boxes[1].bottom || islands.boxes[4].left > islands.boxes[1].right),
            "isolated modules go below or beside the graph",
        )
    }

    fun testMatrixAndTreemap() {
        val names = listOf("ui", "net", "core", "base", "io")
        val edges = listOf(0 to 1, 0 to 2, 1 to 4, 4 to 1, 1 to 2, 2 to 3, 4 to 3).map { InsightModuleEdge(it.first, it.second, 1) }
        val graph = Strata.normalizeGraph(
            InsightModuleGraph(1, names.map { InsightModuleNode(path = it, name = it) }, edges, emptyList(), emptyList()),
        )
        val order = StrataLayout.matrixOrder(graph)
        expect(order.size == 5 && order.first() == 0 && order.last() == 3, "matrix orders from top layer to foundation: $order")
        assertEq(1, abs(order.indexOf(1) - order.indexOf(4)))
        val layers = StrataLayout.moduleLayers(graph)
        expect(layers[3] == 0 && layers[0] > layers[1], "module layers computed from edges")

        val weights = listOf(50.0, 25.0, 12.5, 12.5, 0.0)
        val area = StrataRect(0.0, 0.0, 400.0, 200.0)
        val rects = StrataLayout.squarify(weights, area)
        for (i in 0 until 4) {
            expect(abs(rects[i].width * rects[i].height - weights[i] / 100.0 * 80000) <= 1.0, "treemap areas are proportional")
        }
        expect(rects[4].isEmpty, "zero weight gets no cell")
    }

    fun testDrillAndReport() {
        val snapshot = StrataSynthetic.rich()
        val report = Strata.buildReport(snapshot, StrataSynthetic.graphFor(snapshot, modules))
        val drill = StrataLayout.drillGraph(snapshot, report.graph, 1)
        assertEq(2, drill.nodes.count { !it.module })
        expect(drill.nodes.count { it.module } >= 2, "drill-down collapses other modules")
        expect(!overlapping(drill.layout), "drill-down layout has no overlaps")
        val html = StrataReportHtml.render(snapshot, report, "synthetic")
        expect(html.contains("<h2 id=\"dashboard\">Dashboard</h2>"), "report has the dashboard")
        expect(html.contains("Quality gates") && html.contains("No module dependency cycle"), "report has the gates")
        expect(html.contains("ARC01") && html.contains("Avoid module dependency cycles"), "report has the rules")
        expect(html.contains("<h2 id=\"modules\">Modules</h2>") && html.contains("util"), "report has the module table")
        expect(html.contains("core/engine.cpp:40"), "report lists violations with lines")
        expect(!html.contains("<script"), "report has no scripts")
    }
}
