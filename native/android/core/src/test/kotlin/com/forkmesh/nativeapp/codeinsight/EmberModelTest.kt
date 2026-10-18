package com.forkmesh.nativeapp.codeinsight

import com.forkmesh.nativeapp.testing.assertEq
import com.forkmesh.nativeapp.testing.expect
import java.time.ZoneOffset
import kotlin.math.abs
import kotlin.math.hypot

class EmberModelTest {
    private fun separate(layout: EmberLayout): Boolean = layout.nodes.all { node ->
        node.children.indices.all { i ->
            (i + 1 until node.children.size).all { j ->
                val a = layout.nodes[node.children[i]]
                val b = layout.nodes[node.children[j]]
                hypot(a.x - b.x, a.y - b.y) >= a.r + b.r - 1e-6
            }
        }
    }

    private fun inside(layout: EmberLayout): Boolean = (1 until layout.nodes.size).all {
        val c = layout.nodes[it]
        val p = layout.nodes[c.parent]
        hypot(c.x - p.x, c.y - p.y) + c.r <= p.r + 1e-6
    }

    fun testPackingNeverOverlaps() {
        var seed = 11L
        val circles = List(300) {
            seed = (seed * 1664525L + 1013904223L) and 0xffffffffL
            EmberCircle(0.0, 0.0, 1.0 + (seed shr 8) % 40)
        }
        val radius = EmberPack.packSiblings(circles)
        var area = 0.0
        for (i in circles.indices) {
            area += circles[i].r * circles[i].r
            expect(hypot(circles[i].x, circles[i].y) + circles[i].r <= radius + 1e-6, "circle $i outside")
            for (j in i + 1 until circles.size) {
                expect(hypot(circles[i].x - circles[j].x, circles[i].y - circles[j].y) >= circles[i].r + circles[j].r - 1e-6, "$i overlaps $j")
            }
        }
        expect(area / (radius * radius) > 0.6, "packing is dense")
    }

    fun testPackingSmallCases() {
        val two = listOf(EmberCircle(r = 3.0), EmberCircle(r = 1.0))
        assertEq(4.0, EmberPack.packSiblings(two))
        val e = EmberPack.encloseCircles(listOf(EmberCircle(-2.0, 0.0, 1.0), EmberCircle(2.0, 0.0, 1.0)))
        expect(abs(e.x) < 1e-9 && abs(e.y) < 1e-9 && abs(e.r - 3) < 1e-9, "enclosing circle $e")
        assertEq(0.0, EmberPack.packSiblings(emptyList()))
    }

    fun testLayoutStructure() {
        val snapshot = EmberFixtures.sample()
        val layout = EmberMap.buildLayout(snapshot)
        assertEq("sample", layout.nodes[0].name)
        expect(layout.nodeOfFile.all { it > 0 }, "every file has a node")
        expect(separate(layout), "siblings do not overlap")
        expect(inside(layout), "children inside parents")
        assertEq(snapshot.files.sumOf { it.codeLines.toLong() }, layout.nodes[0].codeLines)
        assertEq(snapshot.files.size, layout.nodes[0].files)
        expect(layout.nodes.any { it.name == "deep/a/b/c" }, "folder chain collapses")
        val readme = layout.nodes[layout.fileNode(7)]
        expect(abs(readme.r - kotlin.math.sqrt(20.0)) < 1e-9, "radius from lines")
        val leaf = layout.fileNode(8)
        assertEq(leaf, layout.nodeAt(layout.nodes[leaf].x, layout.nodes[leaf].y))
        assertEq(listOf("sample", "deep/a/b/c", "leaf.go"), layout.chain(leaf).map { layout.nodes[it].name })
        expect(layout.isAncestor(0, leaf) && !layout.isAncestor(leaf, 0), "ancestors")
        val again = EmberMap.buildLayout(snapshot)
        expect(layout.nodes.indices.all { layout.nodes[it].x == again.nodes[it].x && layout.nodes[it].r == again.nodes[it].r }, "deterministic")
    }

    fun testSyntheticLayoutIsSound() {
        val layout = EmberMap.buildLayout(EmberFixtures.synthetic(3000))
        expect(separate(layout), "synthetic siblings do not overlap")
        expect(inside(layout), "synthetic children inside parents")
    }

    fun testTapAndFocus() {
        val snapshot = EmberFixtures.sample()
        val layout = EmberMap.buildLayout(snapshot)
        val w = 400.0
        val h = 700.0
        val view = EmberMap.fitTransform(layout.nodes[0], w, h)
        val engine = layout.fileNode(0)
        val ex = EmberMap.toScreenX(view, layout.nodes[engine].x, w)
        val ey = EmberMap.toScreenY(view, layout.nodes[engine].y, h)
        assertEq(EmberMap.Tap.SelectFile(0), EmberMap.tapAt(layout, view, 0, -1, ex, ey, w, h))
        assertEq(EmberMap.Tap.None, EmberMap.tapAt(layout, view, 0, -1, 1.0, 1.0, w, h))
        assertEq(EmberMap.Tap.ClearSelection, EmberMap.tapAt(layout, view, 0, 3, 1.0, 1.0, w, h))
        val deep = layout.nodes.indexOfFirst { it.name == "deep/a/b/c" }
        val core = layout.nodes.indexOfFirst { it.name == "core" }
        assertEq(EmberMap.Tap.Focus(core), EmberMap.tapAt(layout, view, 0, -1, ex, ey, w, h, fileRadius = 1e9, folderRadius = 0.0))
        val focused = EmberMap.fitTransform(layout.nodes[deep], w, h)
        assertEq(EmberMap.Tap.ZoomOut, EmberMap.tapAt(layout, focused, deep, -1, 1.0, 1.0, w, h))
        assertEq(deep, EmberMap.focusFromView(layout, focused, w, h))
        assertEq(0, EmberMap.focusFromView(layout, view, w, h))
        assertEq(core, EmberMap.readableFocus(layout, engine, w, h, 1e9))
        assertEq(0, EmberMap.readableFocus(layout, engine, w, h, 0.0))
        val zoomed = EmberMap.zoomAround(view, 2.0, 100.0, 200.0, w, h, 0.0, 1e9)
        expect(abs(EmberMap.toWorldX(view, 100.0, w) - EmberMap.toWorldX(zoomed, 100.0, w)) < 1e-9, "zoom keeps the anchor")
        assertEq(view.k * 2, zoomed.k)
    }

    fun testCrumbs() {
        val snapshot = EmberFixtures.sample()
        val layout = EmberMap.buildLayout(snapshot)
        val engine = layout.fileNode(0)
        assertEq(listOf("sample", "src", "core", "engine.cpp"), EmberMap.crumbs(layout, 0, engine).map { it.name })
        val crumbs = EmberMap.crumbs(layout, layout.nodes[engine].parent, engine)
        assertEq(listOf("sample", "src", "core", "engine.cpp"), crumbs.map { it.name })
        expect(crumbs.last().current, "file crumb is current")
        val leaf = layout.fileNode(8)
        assertEq(listOf("sample", "src", "core", "…", "leaf.go"), EmberMap.crumbs(layout, layout.nodes[engine].parent, leaf).map { it.name })
    }

    fun testColoringOnSample() {
        val snapshot = EmberFixtures.sample()
        val coupling = Ember.buildCouplingIndex(snapshot)
        assertEq(listOf(1, 3), coupling[0].map { it.file })
        val hot = Ember.colorize(snapshot, EmberTab.Hotspots, EmberFilters(), coupling)
        assertEq(Ember.neutral, hot.files[6].color)
        assertEq(3, hot.glowing)
        val targets = Ember.colorize(snapshot, EmberTab.RefactoringTargets, EmberFilters(), coupling)
        assertEq(listOf(0, 3, 1, 4), targets.targets.map { it.file })
        expect(targets.files[2].dimmed && !targets.files[0].dimmed, "non-targets are dimmed")
        assertEq("Changed in 66 commits (22 in the last 90 days)", targets.targets[0].reasons[0])
        assertEq("70 % written by former contributors", targets.targets[0].reasons.last())
        val knowledge = Ember.colorize(snapshot, EmberTab.Knowledge, EmberFilters(), coupling)
        expect(knowledge.legend.any { it.label == "Dee (former)" }, "former author marked")
        expect(knowledge.files[6].marked && knowledge.files[6].color == Ember.quiet, "quiet file")
        val plain = EmberFixtures.sample().also { it.git = false }
        val note = Ember.colorize(plain, EmberTab.Defects, EmberFilters(), coupling)
        expect(note.note.startsWith("Defects need git history"), note.note)
        assertEq(0, note.matching)
        assertEq("2 unhealthy · 2 problematic · top hotspot src/core/engine.cpp", Ember.summarize(snapshot).text)
    }

    fun testFiltersDimWithoutRemoving() {
        val snapshot = EmberFixtures.sample()
        val coupling = Ember.buildCouplingIndex(snapshot)
        val c = Ember.colorize(snapshot, EmberTab.CodeHealth, EmberFilters(author = "Bo"), coupling)
        assertEq(snapshot.files.size, c.files.size)
        assertEq(2, c.matching)
        val band = EmberPanel.toggleBand(EmberFilters(), 2)
        assertEq(1.0 to 3.5, band.minHealth to band.maxHealth)
        val back = EmberPanel.toggleBand(band, 2)
        assertEq(EmberFilters(), back)
        expect(!EmberFilters().active && EmberFilters(search = " x ").active, "active filters")
    }

    fun testPanelRows() {
        val snapshot = EmberFixtures.sample()
        val coupling = Ember.buildCouplingIndex(snapshot)
        val bands = EmberPanel.bandRows(snapshot)
        assertEq(listOf("Healthy", "Problematic", "Unhealthy"), bands.map { it.title })
        assertEq("5 files · 1,260 lines of code", bands[0].detail)
        val hot = EmberPanel.hotspotRows(snapshot)
        assertEq(listOf("engine.cpp", "window.cpp", "engine.h", "window.h", "util.cpp", "leaf.go", "theme.qml"), hot.map { it.title })
        assertEq("66 commits", hot[0].value)
        val targets = EmberPanel.targetRows(snapshot, Ember.refactoringTargets(snapshot, coupling))
        assertEq(1.0, targets[0].fraction)
        assertEq("health 2.5", targets[0].value)
        assertEq(3, targets[0].notes.size)
        val rows = EmberPanel.couplingRows(snapshot, coupling[0])
        assertEq("src/core · 30 shared commits", rows[0].detail)
        assertEq("80 %", rows[0].value)
        assertEq("14 fixes", EmberPanel.metricLabel(snapshot, EmberTab.Defects, coupling, 0))
        assertEq("Ada 60 %", EmberPanel.metricLabel(snapshot, EmberTab.Knowledge, coupling, 0))
        assertEq("2 coupled", EmberPanel.metricLabel(snapshot, EmberTab.ChangeCoupling, coupling, 0))
    }

    fun testRankedLists() {
        val snapshot = EmberFixtures.sample()
        val coupling = Ember.buildCouplingIndex(snapshot)
        fun ranked(tab: EmberTab, filters: EmberFilters = EmberFilters()) =
            EmberPanel.rankedFiles(snapshot, Ember.colorize(snapshot, tab, filters, coupling), coupling)
        assertEq(0, ranked(EmberTab.Hotspots).first().id)
        assertEq(listOf(0, 6, 3), ranked(EmberTab.CodeHealth).take(3).map { it.id })
        assertEq(listOf(0, 3, 1, 4), ranked(EmberTab.RefactoringTargets).map { it.id })
        assertEq(0, ranked(EmberTab.Defects).first().id)
        assertEq(listOf(0, 1, 3, 4), ranked(EmberTab.ChangeCoupling).take(4).map { it.id })
        assertEq(6, ranked(EmberTab.Knowledge).first().id)
        assertEq("C++", ranked(EmberTab.Languages).first().value)
        assertEq(2, ranked(EmberTab.CodeHealth, EmberFilters(author = "Bo")).size)
    }

    fun testSearchAndAuthors() {
        val snapshot = EmberFixtures.sample()
        assertEq(0, EmberPanel.bestMatch(snapshot, "engine.cpp"))
        assertEq(1, EmberPanel.bestMatch(snapshot, "ENGINE.H"))
        assertEq(4, EmberPanel.bestMatch(snapshot, "wind"))
        assertEq(8, EmberPanel.bestMatch(snapshot, "deep"))
        assertEq(-1, EmberPanel.bestMatch(snapshot, "   "))
        val authors = EmberPanel.authorOptions(snapshot)
        assertEq(listOf("All authors", "Ada", "Bo", "Cy", "Dee (former)"), authors.map { it.label })
        assertEq("Dee", authors.last().author)
        assertEq(66, EmberPanel.commitMax(snapshot))
        assertEq("any", EmberPanel.commitThresholdText(0))
        assertEq("at least 4 commits", EmberPanel.commitThresholdText(4))
        assertEq(EmberTab.Hotspots, EmberPanel.defaultTab(snapshot, null))
        val plain = EmberFixtures.sample().also { it.git = false }
        assertEq(EmberTab.CodeHealth, EmberPanel.defaultTab(plain, null))
        assertEq(EmberTab.CodeHealth, EmberPanel.defaultTab(plain, EmberTab.Knowledge))
        assertEq(EmberTab.Languages, EmberPanel.defaultTab(plain, EmberTab.Languages))
    }

    fun testCardText() {
        val snapshot = EmberFixtures.sample()
        snapshot.files[0].lastChanged = 1_791_000_000
        val card = EmberPanel.card(snapshot, 0, 1, 1_792_000_000, ZoneOffset.UTC)
        assertEq("src / core", card.crumb)
        assertEq("engine.cpp", card.name)
        assertEq("Refactoring target #1", card.note)
        val values = card.metrics.associate { it.label to it.value }
        assertEq("66 commits / 1 year · 22 in the last 90 days", values["Commits"])
        assertEq("4,200 lines of code · 5,040 lines in total", values["Size"])
        assertEq("Ada (60 %) · 1 authors", values["Main author"])
        assertEq("70 % code by former contributors", values["Knowledge"])
        assertEq("14 fix commits / 1 year", values["Defects"])
        assertEq("11 days ago · 3 Oct 2026", values["Modified"])
        assertEq("420 in total · worst run() 9 at line 12", values["Complexity"])
        assertEq(12, card.metrics.first { it.label == "Complexity" }.line)
        assertEq("1 functions · longest 40 lines · deepest nesting 0", values["Functions"])
        assertEq("No repeated blocks", values["Duplication"])
        assertEq("imports 0 files · used by 0 files", values["Imports"])
        assertEq("C++", values["Language"])
        assertEq(listOf("Long function", "Deep nesting"), card.issues)
        val plain = EmberFixtures.sample().also { it.git = false }
        assertEq("No git history", EmberPanel.card(plain, 0, 0, 0).metrics[0].value)
        assertEq("Hotspot: changes often and is large", EmberPanel.card(snapshot, 3, 0, 0).note)
        assertEq("Coupling (2)", EmberPanel.couplingTabTitle(2))
        assertEq("10", EmberPanel.healthText(10.0))
        assertEq("9.9", EmberPanel.healthText(9.9))
    }

    fun testTrendHelpers() {
        assertEq(1.0, EmberPanel.niceMax(0.3))
        assertEq(250.0, EmberPanel.niceMax(201.0))
        assertEq(1000.0, EmberPanel.niceMax(501.0))
        assertEq("12k", EmberPanel.compact(12345.0))
        assertEq("1.5k", EmberPanel.compact(1500.0))
        assertEq("750", EmberPanel.compact(750.0))
        val points = listOf(
            InsightTrendPoint(100, "aaaaaaaaaaaa", "Ada", 10, 2, 1, 9.0),
            InsightTrendPoint(200, "bbbbbbbbbbbb", "Bo", 20, 4, 2, 8.0),
            InsightTrendPoint(500, "cccccccccccc", "Cy", 30, 6, 3, 7.0),
        )
        assertEq(0.25, EmberPanel.trendX(points, 1))
        assertEq(1, EmberPanel.nearestTrendPoint(points, 0.3))
        assertEq(2, EmberPanel.nearestTrendPoint(points, 0.9))
        assertEq("bbbbbbbb · Bo · 1 Jan 1970\nHealth 8.0 · worst function 2", EmberPanel.trendInfo(points[1], ZoneOffset.UTC))
    }

    fun testColorMixing() {
        val mid = Ember.mix(EmberColor.hex("#000000"), EmberColor.hex("#ffffff"), 0.5)
        expect(abs(mid.r - 0.5) < 1e-4, "mid grey ${mid.r}")
        assertEq("#3fb98f", Ember.healthyColor.hex)
        assertEq(Ember.unhealthyColor, Ember.healthColor(3.9))
        assertEq(Ember.problematicColor, Ember.healthColor(4.0))
        assertEq(Ember.healthyColor, Ember.healthColor(8.0))
    }
}
