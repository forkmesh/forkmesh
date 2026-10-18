import Foundation
import XCTest
@testable import FMCodeInsight

enum EmberSample {
    static func file(_ path: String, _ lines: Int, _ health: Double, commits: Int = 0, hotspot: Double = 0, author: String = "",
                     fixes: Int = 0, language: String = "C++") -> InsightFile {
        var f = InsightFile(path: path)
        f.language = language
        f.codeLines = lines
        f.lines = lines + lines / 5
        f.health = health
        f.commits = commits
        f.recentCommits = commits / 3
        f.hotspot = hotspot
        f.mainAuthor = author
        f.mainAuthorShare = author.isEmpty ? 0 : 0.6
        if !author.isEmpty { f.topAuthors.append(InsightAuthorCount(name: author, commits: commits)) }
        f.authors = author.isEmpty ? 0 : 1
        f.fixCommits = fixes
        f.complexity = lines / 10
        if health < 8 { f.healthIssues = ["Long function", "Deep nesting"] }
        f.functions = [InsightFunction(name: "run", line: 12, length: 40, complexity: 9)]
        return f
    }

    static func snapshot() -> InsightSnapshot {
        var s = InsightSnapshot(files: [
            file("src/core/engine.cpp", 4200, 2.5, commits: 66, hotspot: 1.0, author: "Ada", fixes: 14),
            file("src/core/engine.h", 300, 7.0, commits: 40, hotspot: 0.4, author: "Ada", fixes: 3),
            file("src/core/util.cpp", 800, 9.5, commits: 5, hotspot: 0.05, author: "Bo"),
            file("src/ui/window.cpp", 2500, 5.0, commits: 30, hotspot: 0.6, author: "Bo", fixes: 6),
            file("src/ui/window.h", 200, 9.0, commits: 12, hotspot: 0.1, author: "Cy"),
            file("src/ui/theme.qml", 150, 9.8, commits: 2, hotspot: 0.01, author: "Cy", language: "QML"),
            file("tools/build.py", 400, 3.5, language: "Python"),
            file("README.md", 20, 10, commits: 1, author: "Ada", language: "Markdown"),
            file("deep/a/b/c/leaf.go", 90, 8.5, commits: 3, hotspot: 0.02, author: "Dee", fixes: 1, language: "Go"),
        ])
        s.source = "/tmp/ember-sample"
        s.name = "sample"
        s.git = true
        s.historyDays = 365
        s.commitsScanned = 400
        s.files[6].formerAuthorShare = 0.9
        s.files[0].formerAuthorShare = 0.7
        s.formerAuthors = ["Dee"]
        s.authors = ["Ada", "Bo", "Cy", "Dee"]
        s.couplings = [
            InsightCoupling(a: 0, b: 1, shared: 30, degree: 0.8),
            InsightCoupling(a: 0, b: 3, shared: 10, degree: 0.35),
            InsightCoupling(a: 3, b: 4, shared: 9, degree: 0.5),
        ]
        return s
    }

    static func plain() -> InsightSnapshot {
        var s = snapshot()
        s.git = false
        s.commitsScanned = 0
        for i in s.files.indices { s.files[i].commits = 0 }
        return s
    }
}

final class EmberUnitTests: XCTestCase {
    func childrenInside(_ layout: EmberLayout, _ tolerance: Double) -> Bool {
        for node in layout.nodes where node.parent >= 0 {
            let p = layout.nodes[node.parent]
            if hypot(node.x - p.x, node.y - p.y) + node.r > p.r + tolerance { return false }
        }
        return true
    }

    func siblingsSeparate(_ layout: EmberLayout, _ tolerance: Double) -> Bool {
        for node in layout.nodes {
            let kids = node.children
            for i in kids.indices {
                for j in kids.indices where j > i {
                    let a = layout.nodes[kids[i]]
                    let b = layout.nodes[kids[j]]
                    if hypot(a.x - b.x, a.y - b.y) < a.r + b.r - tolerance { return false }
                }
            }
        }
        return true
    }

    func testPackingNeverOverlapsAndEnclosesEverything() {
        var seed: UInt32 = 11
        var circles: [EmberCircle] = []
        for _ in 0..<300 {
            seed = seed &* 1664525 &+ 1013904223
            circles.append(EmberCircle(r: 1 + Double((seed >> 8) % 2000) / 100))
        }
        let r = Ember.packSiblings(&circles)
        var area = 0.0
        for (i, a) in circles.enumerated() {
            area += a.r * a.r
            XCTAssertLessThanOrEqual(hypot(a.x, a.y) + a.r, r + 1e-6)
            for b in circles[(i + 1)...] {
                XCTAssertGreaterThanOrEqual(hypot(a.x - b.x, a.y - b.y), a.r + b.r - 1e-6)
            }
        }
        XCTAssertGreaterThan(area / (r * r), 0.55)
        var two = [EmberCircle(r: 3), EmberCircle(r: 5)]
        XCTAssertEqual(Ember.packSiblings(&two), 8, accuracy: 1e-9)
        let e = Ember.encloseCircles([EmberCircle(x: -1, y: 0, r: 1), EmberCircle(x: 1, y: 0, r: 1), EmberCircle(x: 0, y: 0, r: 0.5)])
        XCTAssertEqual(e.r, 2, accuracy: 1e-6)
        XCTAssertEqual(e.x, 0, accuracy: 1e-6)
        XCTAssertEqual(Ember.encloseCircles([]), EmberCircle())
        var empty: [EmberCircle] = []
        XCTAssertEqual(Ember.packSiblings(&empty), 0)
    }

    func testLayoutCollapsesChainsAndAddsUp() {
        let s = EmberSample.snapshot()
        let layout = Ember.buildLayout(s)
        XCTAssertGreaterThan(layout.nodes.count, s.files.count)
        for i in s.files.indices {
            let node = layout.fileNode(i)
            XCTAssertGreaterThanOrEqual(node, 0)
            XCTAssertEqual(layout.nodes[node].file, i)
        }
        XCTAssertTrue(childrenInside(layout, 1e-6))
        XCTAssertTrue(siblingsSeparate(layout, 1e-6))
        let deep = layout.nodes[layout.nodes[layout.fileNode(8)].parent]
        XCTAssertEqual(deep.name, "deep/a/b/c")
        XCTAssertEqual(deep.path, "deep/a/b/c")
        XCTAssertEqual(layout.nodes[0].codeLines, 4200 + 300 + 800 + 2500 + 200 + 150 + 400 + 20 + 90)
        XCTAssertEqual(layout.nodes[0].name, "sample")
        let engine = layout.fileNode(0)
        XCTAssertGreaterThan(layout.nodes[engine].r, layout.nodes[layout.fileNode(2)].r)
        XCTAssertGreaterThanOrEqual(layout.nodes[layout.fileNode(7)].r, 12.0.squareRoot() - 1e-9)
        XCTAssertEqual(layout.nodeAt(x: layout.nodes[engine].x, y: layout.nodes[engine].y), engine)
        XCTAssertEqual(layout.nodeAt(x: 1e9, y: 1e9), -1)
        let chain = layout.chain(engine)
        XCTAssertEqual(chain.count, 4)
        XCTAssertEqual(chain.first, 0)
        XCTAssertEqual(layout.nodes[chain[1]].name, "src")
        XCTAssertEqual(chain.last, engine)
        XCTAssertTrue(layout.isAncestor(chain[1], engine))
        XCTAssertFalse(layout.isAncestor(engine, chain[1]))
        XCTAssertEqual(Ember.buildLayout(s), layout)
        let core = layout.nodes[chain[2]]
        XCTAssertGreaterThan(core.health, 2.5)
        XCTAssertLessThan(core.health, 4.5)
        var unnamed = s
        unnamed.name = ""
        XCTAssertEqual(Ember.buildLayout(unnamed).nodes[0].name, "Repository")
        var options = EmberLayoutOptions()
        options.collapseChains = false
        XCTAssertEqual(Ember.buildLayout(s, options: options).nodes.count, layout.nodes.count + 3)
    }

    func testLargeLayoutStaysSeparateAndDeterministic() {
        let big = EmberGoldenTests.synthetic(3000)
        let layout = Ember.buildLayout(big)
        XCTAssertTrue(childrenInside(layout, 1e-6))
        XCTAssertTrue(siblingsSeparate(layout, 1e-6))
        XCTAssertEqual(Ember.buildLayout(big).nodes.last?.x, layout.nodes.last?.x)
    }

    func testViewMathAndTouchActions() {
        let s = EmberSample.snapshot()
        let layout = Ember.buildLayout(s)
        let w = 400.0, h = 800.0
        let root = Ember.fitTransform(layout.nodes[0], width: w, height: h)
        XCTAssertEqual(layout.nodes[0].r * root.k, 182, accuracy: 1e-6)
        let engine = layout.fileNode(0)
        let src = layout.chain(engine)[1]
        let folder = Ember.fitTransform(layout.nodes[src], width: w, height: h)
        let start = Ember.interpolateView(root, folder, 0, width: w, height: h)
        let end = Ember.interpolateView(root, folder, 1, width: w, height: h)
        XCTAssertEqual(start.k, root.k, accuracy: 1e-9)
        XCTAssertEqual(end, folder)
        XCTAssertTrue(Ember.interpolateView(root, folder, 0.5, width: w, height: h).k > 0)
        XCTAssertEqual(Ember.easeInOut(0), 0)
        XCTAssertEqual(Ember.easeInOut(1), 1)
        XCTAssertEqual(Ember.easeInOut(0.5), 0.5, accuracy: 1e-9)
        let focus = Ember.readableFocus(layout, engine, width: w, height: h)
        XCTAssertTrue(layout.isFolder(focus) && layout.isAncestor(focus, engine))

        let centre = Ember.toScreen(root, x: layout.nodes[engine].x, y: layout.nodes[engine].y, width: w, height: h)
        XCTAssertEqual(Ember.tapAt(layout, view: root, focus: 0, selected: -1, x: centre.x, y: centre.y, width: w, height: h), .selectFile(0))
        XCTAssertEqual(Ember.tapAt(layout, view: root, focus: 0, selected: 3, x: 1, y: 1, width: w, height: h), .clearSelection)
        XCTAssertEqual(Ember.tapAt(layout, view: root, focus: 0, selected: -1, x: 1, y: 1, width: w, height: h), EmberTap.none)
        XCTAssertEqual(Ember.tapAt(layout, view: folder, focus: src, selected: -1, x: 1, y: 1, width: w, height: h), .zoomOut)
        let tiny = EmberView(cx: root.cx, cy: root.cy, k: 0.05)
        let core = layout.chain(engine)[2]
        let corePoint = Ember.toScreen(root, x: layout.nodes[core].x, y: layout.nodes[core].y, width: w, height: h)
        if case .focus(let target) = Ember.tapAt(layout, view: root, focus: 0, selected: -1, x: corePoint.x, y: corePoint.y,
                                                 width: w, height: h, fileRadius: 1e9) {
            XCTAssertTrue(layout.isFolder(target))
            XCTAssertTrue(layout.isAncestor(target, core))
        } else {
            XCTFail("tapping a folder zooms into it")
        }
        XCTAssertEqual(Ember.tapAt(layout, view: tiny, focus: 0, selected: -1, x: w / 2, y: h / 2, width: w, height: h), EmberTap.none)

        XCTAssertEqual(Ember.zoomOutTarget(layout, focus: src), 0)
        XCTAssertEqual(Ember.zoomOutTarget(layout, focus: 0), 0)
        XCTAssertEqual(Ember.focusTarget(layout, engine), core)
        XCTAssertEqual(Ember.focusFromView(layout, root, width: w, height: h), 0)
        XCTAssertEqual(Ember.focusFromView(layout, folder, width: w, height: h), src)

        let zoomed = Ember.zoomAround(root, scale: 2, x: 100, y: 300, width: w, height: h, minK: 0, maxK: 100)
        let before = Ember.toWorld(root, x: 100, y: 300, width: w, height: h)
        let after = Ember.toWorld(zoomed, x: 100, y: 300, width: w, height: h)
        XCTAssertEqual(zoomed.k, root.k * 2, accuracy: 1e-12)
        XCTAssertEqual(before.x, after.x, accuracy: 1e-9)
        XCTAssertEqual(before.y, after.y, accuracy: 1e-9)
        let clamped = Ember.zoomAround(root, scale: 1000, x: 0, y: 0, width: w, height: h,
                                       minK: Ember.minScale(layout, width: w, height: h), maxK: Ember.maxScale(layout, width: w, height: h))
        XCTAssertEqual(clamped.k, 90)
        let panned = Ember.pan(root, dx: 10, dy: -20)
        XCTAssertEqual(panned.cx, root.cx - 10 / root.k, accuracy: 1e-12)
        XCTAssertEqual(panned.cy, root.cy + 20 / root.k, accuracy: 1e-12)

        XCTAssertNil(Ember.revealTarget(layout, view: Ember.fitTransform(layout.nodes[core], width: w, height: h), file: 0, width: w, height: h))
        XCTAssertNotNil(Ember.revealTarget(layout, view: root, file: 7, width: w, height: h))

        let visible = Ember.visibleNodes(layout, view: root, width: w, height: h)
        XCTAssertEqual(visible.first, 0)
        XCTAssertEqual(Set(visible).count, layout.nodes.count)
        let close = Ember.visibleNodes(layout, view: EmberView(cx: layout.nodes[engine].x, cy: layout.nodes[engine].y, k: 60), width: w, height: h)
        XCTAssertFalse(close.contains(0))
        XCTAssertTrue(close.contains(engine))
        XCTAssertLessThan(close.count, layout.nodes.count)
    }

    func testCrumbs() {
        let s = EmberSample.snapshot()
        let layout = Ember.buildLayout(s)
        let engine = layout.fileNode(0)
        let src = layout.chain(engine)[1]
        XCTAssertEqual(Ember.crumbs(layout, focus: 0, selectedFile: -1).map(\.name), ["sample"])
        let selected = Ember.crumbs(layout, focus: src, selectedFile: 0)
        XCTAssertEqual(selected.map(\.name), ["sample", "src", "core", "engine.cpp"])
        XCTAssertTrue(selected.last?.current == true)
        let away = Ember.crumbs(layout, focus: src, selectedFile: 6)
        XCTAssertEqual(away.map(\.name), ["sample", "src", "…", "build.py"])
        XCTAssertEqual(away[2].node, -1)

        var deep = InsightSnapshot(files: [InsightFile(path: "a/b/c/d/e/f/g/x.c"), InsightFile(path: "a/b/c/d/e/f/g/y.c"),
                                           InsightFile(path: "a/1.c"), InsightFile(path: "a/b/2.c"), InsightFile(path: "a/b/c/3.c"),
                                           InsightFile(path: "a/b/c/d/4.c"), InsightFile(path: "a/b/c/d/e/5.c"), InsightFile(path: "a/b/c/d/e/f/6.c")])
        deep.name = "r"
        let deepLayout = Ember.buildLayout(deep)
        let g = deepLayout.nodes[deepLayout.fileNode(0)].parent
        let crumbs = Ember.crumbs(deepLayout, focus: g, selectedFile: -1)
        XCTAssertEqual(crumbs.map(\.name), ["r", "…", "d", "e", "f", "g"])
    }

    func testColouringPerTab() {
        let s = EmberSample.snapshot()
        let coupling = Ember.buildCouplingIndex(s)
        XCTAssertEqual(coupling[0].count, 2)
        XCTAssertEqual(coupling[0].first?.file, 1)
        XCTAssertEqual(coupling[0].first?.degree, 0.8)
        XCTAssertEqual(coupling[4].map(\.file), [3])

        let health = Ember.colorize(s, tab: .codeHealth, coupling: coupling)
        XCTAssertEqual(health.files[0].color, Ember.bandColor(.unhealthy))
        XCTAssertEqual(health.files[3].color, Ember.bandColor(.problematic))
        XCTAssertEqual(health.files[2].color, Ember.bandColor(.healthy))
        XCTAssertEqual(health.legend.count, 3)
        XCTAssertEqual(health.matching, s.files.count)

        let hot = Ember.colorize(s, tab: .hotspots, coupling: coupling)
        XCTAssertLessThan(hot.files[0].color.g, hot.files[3].color.g)
        XCTAssertLessThan(hot.files[3].color.g, hot.files[2].color.g)
        XCTAssertEqual(hot.files[6].color, Ember.neutral)
        XCTAssertGreaterThan(hot.files[0].glow, 0)
        XCTAssertEqual(hot.files[2].glow, 0)
        XCTAssertGreaterThanOrEqual(hot.glowing, 1)
        XCTAssertTrue(hot.scale)

        let targets = Ember.refactoringTargets(s, coupling)
        XCTAssertGreaterThanOrEqual(targets.count, 3)
        XCTAssertEqual(targets.prefix(3).map(\.file), [0, 3, 1])
        XCTAssertEqual(targets.map(\.rank), Array(1...targets.count))
        XCTAssertEqual(targets[0].reasons, [
            "Changed in 66 commits (22 in the last 90 days)", "Code health 2.5, unhealthy", "Long function", "Deep nesting",
            "14 bug-fix commits", "Changes together with 2 other files", "70 % written by former contributors",
        ])
        XCTAssertEqual(Ember.targetScore(s.files[7]), 0)
        XCTAssertFalse(targets.contains { s.files[$0.file].health >= 9.9 })

        let refactor = Ember.colorize(s, tab: .refactoringTargets, coupling: coupling)
        XCTAssertEqual(refactor.files[0].rank, 1)
        XCTAssertFalse(refactor.files[0].dimmed)
        XCTAssertTrue(refactor.files[6].dimmed)
        XCTAssertEqual(refactor.targets, targets)

        let couple = Ember.colorize(s, tab: .changeCoupling, coupling: coupling)
        XCTAssertNotEqual(couple.files[0].color, Ember.neutral)
        XCTAssertEqual(couple.files[2].color, Ember.neutral)
        XCTAssertGreaterThan(couple.files[0].color.g, couple.files[3].color.g - 1e-9)

        let knowledge = Ember.colorize(s, tab: .knowledge, coupling: coupling)
        XCTAssertEqual(knowledge.files[0].color, Ember.categoricalPalette[0])
        XCTAssertFalse(Ember.categoricalPalette.contains(knowledge.files[6].color))
        XCTAssertTrue(knowledge.files[6].marked)
        XCTAssertTrue(knowledge.files[0].marked)
        XCTAssertTrue(knowledge.legend.contains { $0.label == "Dee (former)" })
        XCTAssertEqual(knowledge.legend.last, EmberLegendEntry(label: "Mostly former contributors", color: nil, ring: true))

        let defects = Ember.colorize(s, tab: .defects, coupling: coupling)
        XCTAssertGreaterThan(defects.files[0].color.r, 0.7)
        XCTAssertEqual(defects.files[2].color, Ember.neutral)

        let languages = Ember.colorize(s, tab: .languages, coupling: coupling)
        XCTAssertEqual(languages.files[0].color, Ember.categoricalPalette[0])
        XCTAssertNotEqual(languages.files[6].color, languages.files[0].color)
        XCTAssertEqual(languages.legend.map(\.label), ["C++", "Python", "QML", "Go", "Markdown"])

        let plain = EmberSample.plain()
        XCTAssertFalse(Ember.hasHistory(plain))
        let blocked = Ember.colorize(plain, tab: .hotspots, coupling: Ember.buildCouplingIndex(plain))
        XCTAssertEqual(blocked.note, "Hotspots need git history. This folder has none, so only Code health and Languages can be shown.")
        XCTAssertEqual(blocked.matching, 0)
        XCTAssertEqual(Ember.colorize(plain, tab: .languages, coupling: []).matching, plain.files.count)
        XCTAssertEqual(Ember.defaultTab(plain, chosen: nil), .codeHealth)
        XCTAssertEqual(Ember.defaultTab(plain, chosen: .defects), .codeHealth)
        XCTAssertEqual(Ember.defaultTab(s, chosen: nil), .hotspots)
        XCTAssertEqual(Ember.defaultTab(s, chosen: .knowledge), .knowledge)

        XCTAssertEqual(Ember.summarize(s).text, "2 unhealthy · 2 problematic · top hotspot src/core/engine.cpp")
        XCTAssertEqual(Ember.mix(EmberColor(hex: 0x000000), EmberColor(hex: 0xffffff), 2), EmberColor(hex: 0xffffff))
        XCTAssertEqual(Ember.bandColor(.healthy).hexText, "#3fb98f")
        let dimmed = Ember.fileFill(EmberFileStyle(color: EmberColor(hex: 0xffffff), dimmed: true), background: EmberColor(hex: 0x000000), dark: true)
        XCTAssertEqual(dimmed.fill.r, 0.26, accuracy: 1e-4)
        XCTAssertGreaterThan(dimmed.light.r, dimmed.fill.r)
        XCTAssertLessThan(dimmed.shade.r, dimmed.fill.r)
        let lit = Ember.fileFill(EmberFileStyle(color: Ember.neutral), background: EmberColor(hex: 0xffffff), dark: false)
        XCTAssertEqual(lit.fill, Ember.neutral)
        let mid = Ember.bezier((0, 0), (1, 2), (2, 0), 0.5)
        XCTAssertEqual(mid.x, 1, accuracy: 1e-12)
        XCTAssertEqual(mid.y, 1, accuracy: 1e-12)
    }

    func testFiltersDimWithoutRemoving() {
        let s = EmberSample.snapshot()
        let coupling = Ember.buildCouplingIndex(s)
        func dimmed(_ filters: EmberFilters) -> [Int] {
            let c = Ember.colorize(s, tab: .codeHealth, filters: filters, coupling: coupling)
            XCTAssertEqual(c.files.count, s.files.count)
            return s.files.indices.filter { c.files[$0].dimmed }
        }
        XCTAssertFalse(EmberFilters().active)
        XCTAssertFalse(EmberFilters(search: "   ").active)
        XCTAssertEqual(dimmed(EmberFilters(minHealth: 8)), [0, 1, 3, 6])
        XCTAssertEqual(dimmed(EmberFilters(minCommits: 10)), [2, 5, 6, 7, 8])
        XCTAssertEqual(dimmed(EmberFilters(author: "Bo")), [0, 1, 4, 5, 6, 7, 8])
        XCTAssertEqual(dimmed(EmberFilters(search: " WINDOW ")), [0, 1, 2, 5, 6, 7, 8])
        XCTAssertTrue(EmberFilters(maxHealth: 9.5).active)
        let toggled = Ember.toggleBand(EmberFilters(), .problematic)
        XCTAssertEqual(toggled.minHealth, 4)
        XCTAssertEqual(toggled.maxHealth, 7.5)
        let back = Ember.toggleBand(toggled, .problematic)
        XCTAssertEqual(back.minHealth, 1)
        XCTAssertEqual(back.maxHealth, 10)
        XCTAssertEqual(Ember.commitMax(s), 66)
        XCTAssertEqual(Ember.commitMax(EmberSample.plain()), 1)
        XCTAssertEqual(Ember.commitThresholdText(0), "any")
        XCTAssertEqual(Ember.commitThresholdText(4), "at least 4 commits")
    }

    func testPanelRowsSearchAndCard() {
        let s = EmberSample.snapshot()
        let coupling = Ember.buildCouplingIndex(s)
        let options = Ember.authorOptions(s)
        XCTAssertEqual(options.map(\.label), ["All authors", "Ada", "Bo", "Cy", "Dee (former)"])
        XCTAssertEqual(options.first?.author, "")

        let bands = Ember.bandRows(Ember.summarize(s))
        XCTAssertEqual(bands.map(\.title), ["Healthy", "Problematic", "Unhealthy"])
        XCTAssertEqual(bands[0].detail, "5 files · 1,260 lines of code")
        XCTAssertEqual(bands[2].value, "53 % of code")
        let hot = Ember.hotspotRows(s)
        XCTAssertEqual(hot.map(\.id), [0, 3, 1, 4, 2, 8, 5])
        XCTAssertEqual(hot[0].value, "66 commits")
        XCTAssertEqual(Ember.hotspotsEmptyText(history: false), "Hotspots need git history; this folder has none.")
        let targetRows = Ember.targetRows(s, Ember.refactoringTargets(s, coupling))
        XCTAssertEqual(targetRows[0].fraction, 1)
        XCTAssertEqual(targetRows[0].notes, ["Changed in 66 commits (22 in the last 90 days)", "Long function", "Deep nesting"])
        let coupled = Ember.couplingRows(s, coupling[0])
        XCTAssertEqual(coupled.map(\.detail), ["src/core · 30 shared commits", "src/ui · 10 shared commits"])
        XCTAssertEqual(coupled.map(\.value), ["80 %", "35 %"])
        XCTAssertEqual(Ember.couplingTabTitle(2), "Coupling (2)")

        XCTAssertEqual(Ember.metricLabel(s, tab: .knowledge, coupling: coupling, file: 0), "Ada 60 %")
        XCTAssertEqual(Ember.metricLabel(s, tab: .changeCoupling, coupling: coupling, file: 0), "2 coupled")
        XCTAssertEqual(Ember.metricLabel(s, tab: .codeHealth, coupling: coupling, file: 0), "health 2.5")
        XCTAssertEqual(Ember.metricLabel(s, tab: .hotspots, coupling: coupling, file: 0), "66 commits")
        XCTAssertEqual(Ember.metricLabel(s, tab: .defects, coupling: coupling, file: 2), "")
        XCTAssertTrue(Ember.describeFile(s, file: 0, rank: 1).hasSuffix("Refactoring target #1"))

        XCTAssertEqual(Ember.bestMatch(s, query: "window"), 4)
        XCTAssertEqual(Ember.bestMatch(s, query: "WINDOW.CPP"), 3)
        XCTAssertEqual(Ember.bestMatch(s, query: "core/"), 1)
        XCTAssertEqual(Ember.bestMatch(s, query: "zzz"), -1)
        let hits = Ember.searchHits(s, query: "window", limit: 10)
        XCTAssertEqual(hits.map(\.file), [4, 3])
        XCTAssertEqual(hits[0].detail, "src/ui/window.h · health 9.0 · 12 commits")

        let card = Ember.card(s, file: 0, rank: 1, now: 1_792_000_000, zone: TimeZone(identifier: "UTC")!)
        XCTAssertEqual(card.folder, "src / core")
        XCTAssertEqual(card.name, "engine.cpp")
        XCTAssertEqual(card.note, "Refactoring target #1")
        XCTAssertEqual(card.band, .unhealthy)
        let metrics = Dictionary(uniqueKeysWithValues: card.metrics.map { ($0.label, $0) })
        XCTAssertEqual(card.metrics.map(\.label), ["Commits", "Size", "Main author", "Knowledge", "Defects", "Modified", "Complexity",
                                                   "Functions", "Duplication", "Imports", "Language"])
        XCTAssertEqual(metrics["Commits"]?.value, "66 commits / 1 year · 22 in the last 90 days")
        XCTAssertEqual(metrics["Size"]?.value, "4,200 lines of code · 5,040 lines in total")
        XCTAssertEqual(metrics["Main author"]?.value, "Ada (60 %) · 1 authors")
        XCTAssertEqual(metrics["Knowledge"]?.value, "70 % code by former contributors")
        XCTAssertEqual(metrics["Modified"]?.value, "No git history")
        XCTAssertEqual(metrics["Complexity"]?.value, "420 in total · worst run() 9 at line 12")
        XCTAssertEqual(metrics["Complexity"]?.line, 12)
        XCTAssertEqual(metrics["Functions"]?.value, "1 functions · longest 40 lines · deepest nesting 0")
        XCTAssertEqual(metrics["Duplication"]?.value, "No repeated blocks")
        XCTAssertEqual(metrics["Imports"]?.value, "imports 0 files · used by 0 files")
        var changed = s
        changed.files[0].lastChanged = 1_792_000_000 - 86400 * 3
        let dated = Ember.card(changed, file: 0, rank: 0, now: 1_792_000_000, zone: TimeZone(identifier: "UTC")!)
        XCTAssertEqual(dated.metrics[5].value, "3 days ago · 11 Oct 2026")
        XCTAssertEqual(dated.note, "Hotspot: changes often and is large")
        let plainCard = Ember.card(EmberSample.plain(), file: 6, rank: 0, now: 0)
        XCTAssertEqual(plainCard.metrics[0].value, "No git history")
        XCTAssertEqual(plainCard.metrics[1].value, "400 lines of code · 480 lines in total")

        XCTAssertEqual(Ember.trendInfo(InsightTrendPoint(time: 1_792_000_000, commit: "0123456789abcdef", author: "Ada", codeLines: 1,
                                                         complexity: 2, maxComplexity: 1234, health: 7.25),
                                       zone: TimeZone(identifier: "UTC")!),
                       "01234567 · Ada · 14 Oct 2026\nHealth 7.3 · worst function 1,234")
        XCTAssertEqual(Ember.compact(12_500), "13k")
        XCTAssertEqual(Ember.compact(1_250), "1.3k")
        XCTAssertEqual(Ember.compact(999.6), "1,000")
        XCTAssertEqual(Ember.healthText(10), "10")
        XCTAssertEqual(Ember.healthText(9.94), "9.9")
        XCTAssertEqual(Ember.decimal(12345.678), "12,345.7")
        XCTAssertEqual(Ember.decimal(-0.04), "-0.0")
        XCTAssertEqual(Ember.decimal(7.25), "7.3")
        XCTAssertEqual(Ember.decimal(7.35), "7.3")
        XCTAssertEqual(Ember.decimal(9.96), "10.0")
        XCTAssertEqual(Ember.decimal(999.95), "1,000.0")
        XCTAssertEqual(Ember.decimal(0.5, digits: 0), "1")
        XCTAssertEqual(Ember.decimal(2, digits: 0), "2")
        XCTAssertEqual(Ember.number(1_234_567), "1,234,567")
        XCTAssertEqual(Ember.pathCrumb(""), "/")
    }

    func testRankedListFollowsTheTabMeasure() {
        let s = EmberSample.snapshot()
        let coupling = Ember.buildCouplingIndex(s)
        func ids(_ tab: EmberTab, _ filters: EmberFilters = EmberFilters()) -> [Int] {
            Ember.rankedFiles(s, coloring: Ember.colorize(s, tab: tab, filters: filters, coupling: coupling), coupling: coupling).map(\.id)
        }
        XCTAssertEqual(ids(.hotspots).prefix(3), [0, 3, 1])
        XCTAssertEqual(ids(.codeHealth).prefix(3), [0, 6, 3])
        XCTAssertEqual(ids(.changeCoupling).prefix(4), [0, 1, 3, 4])
        XCTAssertEqual(ids(.defects).prefix(3), [0, 3, 1])
        XCTAssertEqual(ids(.knowledge).prefix(2), [6, 0])
        XCTAssertEqual(ids(.languages).prefix(5), [0, 3, 2, 1, 4])
        XCTAssertEqual(ids(.refactoringTargets), Ember.refactoringTargets(s, coupling).map(\.file))
        XCTAssertEqual(ids(.codeHealth, EmberFilters(search: "ui/")), [3, 4, 5])
        XCTAssertEqual(ids(.hotspots).count, s.files.count)
    }
}
