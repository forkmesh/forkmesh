package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.json.Json
import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.io.File
import kotlin.math.abs
import kotlin.math.max

@Suppress("UNCHECKED_CAST")
class EmberGoldenTest {
    private val golden: Map<String, Any?> by lazy {
        Json.parse(File(InsightFixtures.fixture, "ember.json").readText(Charsets.UTF_8)) as Map<String, Any?>
    }

    private val base: InsightSnapshot by lazy {
        val snapshot = Insight.analyze(MemorySource(InsightFixtures.repoFiles), InsightOptions(history = false), now = InsightFixtures.NOW)
        assertEq("", snapshot.error, "base analysis error")
        snapshot.name = golden["name"] as String
        snapshot
    }

    private val snapshot: InsightSnapshot by lazy {
        val history = base.deepCopy()
        Insight.applyHistory(history, Insight.parseGitLog(InsightFixtures.historyLog), InsightOptions(), InsightFixtures.NOW)
        for (file in history.files) {
            val health = Insight.computeHealth(file)
            file.health = health.score
            file.healthIssues = health.issues
        }
        Insight.computeHotspots(history)
        history
    }

    private val coupling by lazy { Ember.buildCouplingIndex(snapshot) }

    private val layout by lazy { EmberMap.buildLayout(snapshot) }

    private fun obj(value: Any?): Map<String, Any?> = value as Map<String, Any?>

    private fun list(value: Any?): List<Any?> = value as List<Any?>

    private fun num(value: Any?): Double = (value as Number).toDouble()

    private fun int(value: Any?): Int = (value as Number).toInt()

    private fun near(expected: Double, actual: Double, message: String, tolerance: Double = 1e-9) {
        if (abs(expected - actual) > tolerance * max(1.0, max(abs(expected), abs(actual)))) {
            throw AssertionError("$message: expected <$expected> but was <$actual>")
        }
    }

    private fun color(expected: Any?, actual: EmberColor?, message: String) {
        if (expected == null) {
            assertEq(null, actual, message)
            return
        }
        expect(actual != null, "$message: missing colour")
        val e = list(expected).map(::num)
        val a = listOf(actual!!.r, actual.g, actual.b, actual.a)
        for (i in 0..3) near(e[i], a[i], "$message channel $i", 1e-4)
    }

    private fun path(index: Int): String = snapshot.files[index].path

    fun testFixtureMatchesSnapshot() {
        assertEq(list(golden["files"]), snapshot.files.map { it.path }, "file list")
        assertEq(golden["hasHistory"], Ember.hasHistory(snapshot), "history")
        assertEq(golden["hasHistoryBase"], Ember.hasHistory(base), "base history")
        val plain = base.deepCopy().also { it.git = false }
        assertEq(golden["hasHistoryPlain"], Ember.hasHistory(plain), "plain history")
    }

    fun testPacking() {
        for ((c, case) in list(golden["packing"]).withIndex()) {
            val o = obj(case)
            val circles = list(o["input"]).map { EmberCircle(0.0, 0.0, num(it)) }
            val radius = EmberPack.packSiblings(circles)
            near(num(o["radius"]), radius, "case $c radius")
            for ((i, want) in list(o["output"]).withIndex()) {
                val w = list(want).map(::num)
                near(w[0], circles[i].x, "case $c circle $i x")
                near(w[1], circles[i].y, "case $c circle $i y")
                near(w[2], circles[i].r, "case $c circle $i r")
            }
            val e = EmberPack.encloseCircles(circles)
            val we = list(o["enclose"]).map(::num)
            near(we[0], e.x, "case $c enclose x")
            near(we[1], e.y, "case $c enclose y")
            near(we[2], e.r, "case $c enclose r")
        }
    }

    fun testLayout() {
        val want = obj(golden["layout"])
        assertEq(int(want["maxDepth"]), layout.maxDepth, "max depth")
        val nodes = list(want["nodes"])
        assertEq(nodes.size, layout.nodes.size, "node count")
        for ((i, w) in nodes.withIndex()) {
            val n = obj(w)
            val got = layout.nodes[i]
            val label = "node $i ${n["path"]}"
            assertEq(n["name"], got.name, "$label name")
            assertEq(n["path"], got.path, "$label path")
            assertEq(int(n["parent"]), got.parent, "$label parent")
            assertEq(int(n["depth"]), got.depth, "$label depth")
            assertEq(n["file"], if (got.file >= 0) path(got.file) else "", "$label file")
            assertEq(list(n["children"]).map(::int), got.children.toList(), "$label children")
            near(num(n["x"]), got.x, "$label x")
            near(num(n["y"]), got.y, "$label y")
            near(num(n["r"]), got.r, "$label r")
            assertEq(num(n["codeLines"]).toLong(), got.codeLines, "$label lines")
            assertEq(int(n["files"]), got.files, "$label files")
            near(num(n["health"]), got.health, "$label health")
            assertEq(int(n["commits"]), got.commits, "$label commits")
        }
        assertEq(list(want["nodeOfFile"]).map(::int), layout.nodeOfFile.toList(), "node of file")
    }

    fun testViews() {
        val views = obj(golden["views"])
        val viewport = list(views["viewport"]).map(::num)
        val w = viewport[0]
        val h = viewport[1]
        for (f in list(views["fit"])) {
            val o = obj(f)
            val node = int(o["node"])
            val fit = EmberMap.fitTransform(layout.nodes[node], w, h)
            near(num(o["cx"]), fit.cx, "fit $node cx")
            near(num(o["cy"]), fit.cy, "fit $node cy")
            near(num(o["k"]), fit.k, "fit $node k")
        }
        for (r in list(views["readable"])) {
            val pair = list(r).map(::int)
            assertEq(pair[1], EmberMap.readableFocus(layout, pair[0], w, h), "readable focus of ${pair[0]}")
        }
        for (v in list(views["interpolate"])) {
            val o = obj(v)
            val from = EmberMap.fitTransform(layout.nodes[int(o["from"])], w, h)
            var to = EmberMap.fitTransform(layout.nodes[int(o["to"])], w, h)
            to = to.copy(k = to.k * int(o["zoom"]))
            val t = num(o["t"])
            val got = EmberMap.interpolateView(from, to, t, w, h)
            val label = "interpolate ${o["from"]}->${o["to"]} t=$t"
            near(num(o["cx"]), got.cx, "$label cx")
            near(num(o["cy"]), got.cy, "$label cy")
            near(num(o["k"]), got.k, "$label k")
        }
        for (e in list(views["ease"])) {
            val pair = list(e).map(::num)
            near(pair[1], EmberMap.easeInOut(pair[0]), "ease ${pair[0]}")
        }
    }

    fun testNodeAt() {
        for (p in list(golden["nodeAt"])) {
            val v = list(p)
            assertEq(int(v[2]), layout.nodeAt(num(v[0]), num(v[1])), "node at ${v[0]}, ${v[1]}")
        }
    }

    fun testCouplingIndex() {
        val want = list(golden["coupling"])
        assertEq(want.size, coupling.size, "coupling size")
        for ((i, refs) in want.withIndex()) {
            val got = coupling[i]
            val w = list(refs)
            assertEq(w.size, got.size, "coupling of ${path(i)}")
            for ((j, ref) in w.withIndex()) {
                val o = obj(ref)
                assertEq(o["path"], path(got[j].file), "coupling ${path(i)} #$j path")
                assertEq(int(o["shared"]), got[j].shared, "coupling ${path(i)} #$j shared")
                near(num(o["degree"]), got[j].degree, "coupling ${path(i)} #$j degree")
            }
        }
    }

    fun testTopNames() {
        assertEq(list(golden["topAuthors"]), Ember.topAuthors(snapshot, 0), "top authors")
        assertEq(list(golden["topLanguages"]), Ember.topLanguages(snapshot, 0), "top languages")
    }

    private fun checkTargets(want: Any?, got: List<EmberTarget>, label: String) {
        val w = list(want)
        assertEq(w.size, got.size, "$label count")
        for ((i, t) in w.withIndex()) {
            val o = obj(t)
            assertEq(o["path"], path(got[i].file), "$label #$i path")
            assertEq(int(o["rank"]), got[i].rank, "$label #$i rank")
            near(num(o["score"]), got[i].score, "$label #$i score")
            assertEq(list(o["reasons"]), got[i].reasons, "$label #$i reasons")
        }
    }

    fun testRefactoringTargets() {
        checkTargets(golden["targets"], Ember.refactoringTargets(snapshot, coupling, 0), "targets")
    }

    fun testTabColoring() {
        val tabs = list(golden["tabs"])
        assertEq(Ember.tabs.map { it.title }, tabs.map { obj(it)["tab"] }, "tab titles")
        for ((t, value) in tabs.withIndex()) {
            val want = obj(value)
            val tab = Ember.tabs[t]
            val got = Ember.colorize(snapshot, tab, EmberFilters(), coupling)
            val label = tab.title
            val files = list(want["files"])
            assertEq(files.size, got.files.size, "$label files")
            for ((i, f) in files.withIndex()) {
                val o = obj(f)
                val g = got.files[i]
                color(o["color"], g.color, "$label ${path(i)} colour")
                assertEq(o["dimmed"], g.dimmed, "$label ${path(i)} dimmed")
                near(num(o["glow"]), g.glow, "$label ${path(i)} glow")
                assertEq(int(o["rank"]), g.rank, "$label ${path(i)} rank")
                assertEq(o["marked"], g.marked, "$label ${path(i)} marked")
            }
            val legend = list(want["legend"])
            assertEq(legend.map { obj(it)["label"] }, got.legend.map { it.label }, "$label legend labels")
            for ((i, l) in legend.withIndex()) {
                color(obj(l)["color"], got.legend[i].color, "$label legend $i")
                assertEq(obj(l)["ring"], got.legend[i].ring, "$label legend $i ring")
            }
            assertEq(want["scale"], got.scale, "$label scale")
            color(want["scaleLow"], got.scaleLow, "$label scale low")
            color(want["scaleHigh"], got.scaleHigh, "$label scale high")
            assertEq(want["scaleLowLabel"], got.scaleLowLabel, "$label low label")
            assertEq(want["scaleHighLabel"], got.scaleHighLabel, "$label high label")
            assertEq(want["note"], got.note, "$label note")
            checkTargets(want["targets"], got.targets, "$label targets")
            assertEq(int(want["glowing"]), got.glowing, "$label glowing")
            assertEq(int(want["matching"]), got.matching, "$label matching")
        }
    }

    fun testFilters() {
        for (value in list(golden["filtered"])) {
            val o = obj(value)
            val f = obj(o["filters"])
            val filters = EmberFilters(num(f["minHealth"]), num(f["maxHealth"]), int(f["minCommits"]), f["author"] as String, f["search"] as String)
            val tab = Ember.tabs.first { it.title == o["tab"] }
            val got = Ember.colorize(snapshot, tab, filters, coupling)
            val label = "$filters on ${tab.title}"
            assertEq(o["active"], filters.active, "$label active")
            assertEq(list(o["dimmed"]), got.files.indices.filter { got.files[it].dimmed }.map(::path), "$label dimmed")
            assertEq(int(o["matching"]), got.matching, "$label matching")
        }
    }

    fun testSummary() {
        val want = obj(golden["summary"])
        val got = Ember.summarize(snapshot)
        assertEq(int(want["unhealthy"]), got.unhealthy, "unhealthy")
        assertEq(int(want["problematic"]), got.problematic, "problematic")
        assertEq(int(want["healthy"]), got.healthy, "healthy")
        assertEq(num(want["unhealthyLines"]).toLong(), got.unhealthyLines, "unhealthy lines")
        assertEq(num(want["problematicLines"]).toLong(), got.problematicLines, "problematic lines")
        assertEq(num(want["healthyLines"]).toLong(), got.healthyLines, "healthy lines")
        assertEq(want["topHotspot"], if (got.topHotspot >= 0) path(got.topHotspot) else "", "top hotspot")
        assertEq(want["text"], got.text, "summary text")
    }

    fun testNoHistory() {
        val plainCoupling = Ember.buildCouplingIndex(base)
        for ((t, value) in list(golden["noHistory"]).withIndex()) {
            val o = obj(value)
            val got = Ember.colorize(base, Ember.tabs[t], EmberFilters(), plainCoupling)
            assertEq(o["tab"], Ember.tabs[t].title, "no-history tab")
            assertEq(o["note"], got.note, "no-history ${o["tab"]} note")
            assertEq(int(o["matching"]), got.matching, "no-history ${o["tab"]} matching")
            assertEq(int(o["legend"]), got.legend.size, "no-history ${o["tab"]} legend")
        }
    }

    fun testText() {
        val text = obj(golden["text"])
        for (s in list(text["span"])) {
            val v = list(s)
            assertEq(v[1], Ember.spanText(int(v[0])), "span ${v[0]}")
        }
        for (a in list(text["ago"])) {
            val v = list(a)
            assertEq(v[2], Ember.agoText(num(v[0]).toLong(), num(v[1]).toLong()), "ago ${v[0]}")
        }
        for (p in list(text["percent"])) {
            val v = list(p)
            assertEq(v[1], Ember.percentText(num(v[0])), "percent ${v[0]}")
        }
        for (n in list(text["names"])) {
            val v = list(n)
            assertEq(v[1], Ember.fileNameOf(v[0] as String), "file name of ${v[0]}")
            assertEq(v[2], Ember.folderOf(v[0] as String), "folder of ${v[0]}")
        }
    }

    fun testSyntheticLayout() {
        val want = obj(golden["synthetic"])
        val big = EmberFixtures.synthetic(int(want["files"]))
        val got = EmberMap.buildLayout(big)
        assertEq(int(want["nodes"]), got.nodes.size, "synthetic nodes")
        assertEq(int(want["maxDepth"]), got.maxDepth, "synthetic depth")
        near(num(want["rootRadius"]), got.nodes[0].r, "synthetic root radius")
        near(num(want["sumX"]), got.nodes.sumOf { it.x }, "synthetic sum x", 1e-8)
        near(num(want["sumY"]), got.nodes.sumOf { it.y }, "synthetic sum y", 1e-8)
        near(num(want["sumR"]), got.nodes.sumOf { it.r }, "synthetic sum r", 1e-8)
    }
}
