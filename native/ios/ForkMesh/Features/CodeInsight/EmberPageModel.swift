import FMCodeInsight
import Foundation
import Observation
import SwiftUI

enum EmberMode: String, CaseIterable, Identifiable {
    case map = "Map"
    case list = "List"

    var id: String { rawValue }
}

struct EmberTransition {
    let from: EmberView
    let to: EmberView
    let start: Date
    let id: Int
}

struct EmberAccessibleNode: Identifiable {
    let node: Int
    let file: Int
    let x: CGFloat
    let y: CGFloat
    let diameter: CGFloat
    let label: String

    var id: Int { node }
}

@MainActor
@Observable
final class EmberPageModel {
    static let transitionSeconds: Double = 0.45
    static let linkSeconds: Double = 0.42

    private(set) var snapshot = InsightSnapshot()
    private(set) var layout: EmberLayout?
    private(set) var coupling: EmberCouplingIndex = []
    private(set) var coloring = EmberColoring()
    private(set) var summary = EmberSummary()
    private(set) var history = false
    private(set) var authorOptions: [EmberAuthorOption] = []
    private(set) var commitMax = 1
    private(set) var tab: EmberTab = .codeHealth
    private(set) var selected = -1
    private(set) var focus = 0
    private(set) var transition: EmberTransition?
    private(set) var linkStart: Date?
    var filters = EmberFilters()
    var view = EmberView()
    var interacting = false
    var reduceMotion = false
    var visible = true
    var trends: [String: [InsightTrendPoint]] = [:]

    @ObservationIgnored private var key = ""
    @ObservationIgnored private var tabChosen = false
    @ObservationIgnored private var size: CGSize = .zero
    @ObservationIgnored private var transitionCounter = 0
    @ObservationIgnored private var linkCounter = 0
    @ObservationIgnored private var recolorTask: Task<Void, Never>?

    var width: Double { Double(size.width) }
    var height: Double { Double(size.height) }
    var ready: Bool { layout != nil }

    var animating: Bool {
        if !visible { return false }
        if transition != nil || linkStart != nil { return true }
        return tab == .hotspots && coloring.glowing > 0 && !reduceMotion && !interacting
    }

    func load(_ snapshot: InsightSnapshot, key: String) async {
        guard key != self.key else { return }
        self.key = key
        let previous = selected >= 0 && selected < self.snapshot.files.count ? self.snapshot.files[selected].path : ""
        self.snapshot = snapshot
        layout = nil
        selected = -1
        linkStart = nil
        transition = nil
        trends = [:]
        let prepared: (EmberLayout, EmberCouplingIndex, EmberSummary) = await Task.detached(priority: .userInitiated) {
            (Ember.buildLayout(snapshot), Ember.buildCouplingIndex(snapshot), Ember.summarize(snapshot))
        }.value
        guard key == self.key else { return }
        coupling = prepared.1
        summary = prepared.2
        history = Ember.hasHistory(snapshot)
        authorOptions = Ember.authorOptions(snapshot)
        if !authorOptions.contains(where: { $0.author == filters.author }) || !history { filters.author = "" }
        commitMax = Ember.commitMax(snapshot)
        filters.minCommits = history ? min(filters.minCommits, commitMax) : 0
        tab = Ember.defaultTab(snapshot, chosen: tabChosen ? tab : nil)
        focus = 0
        layout = prepared.0
        if let root = prepared.0.nodes.first, size.width > 0 {
            view = Ember.fitTransform(root, width: width, height: height)
        }
        recolor()
        if !previous.isEmpty {
            let restore = snapshot.indexOf(previous)
            if restore >= 0 { select(restore, zoom: true) }
        }
    }

    func resize(_ newSize: CGSize) {
        guard newSize != size, newSize.width > 0, newSize.height > 0 else { return }
        size = newSize
        guard let layout, focus >= 0, focus < layout.nodes.count else { return }
        transition = nil
        view = Ember.fitTransform(layout.nodes[focus], width: width, height: height)
    }

    func setTab(_ newTab: EmberTab) {
        tabChosen = true
        guard newTab != tab else { return }
        tab = newTab
        recolor()
    }

    func filtersChanged(debounce: Bool) {
        recolorTask?.cancel()
        if !debounce {
            recolor()
            return
        }
        recolorTask = Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(90))
            guard !Task.isCancelled else { return }
            self?.recolor()
        }
    }

    func resetFilters() {
        filters = EmberFilters()
        recolor()
    }

    func toggleBand(_ band: HealthBand) {
        filters = Ember.toggleBand(filters, band)
        recolor()
    }

    private func recolor() {
        guard layout != nil else { return }
        coloring = Ember.colorize(snapshot, tab: tab, filters: filters, coupling: coupling)
    }

    func rank(of file: Int) -> Int {
        file >= 0 && file < coloring.files.count ? coloring.files[file].rank : 0
    }

    func currentView(at date: Date) -> EmberView {
        guard let transition else { return view }
        let t = min(1, date.timeIntervalSince(transition.start) / Self.transitionSeconds)
        return Ember.interpolateView(transition.from, transition.to, Ember.easeInOut(t), width: width, height: height)
    }

    func linkProgress(at date: Date) -> Double {
        guard let linkStart, !reduceMotion else { return 1 }
        return min(1, max(0, date.timeIntervalSince(linkStart) / Self.linkSeconds))
    }

    private func move(to target: EmberView, animate: Bool) {
        if !animate || reduceMotion || size == .zero {
            transition = nil
            view = target
            return
        }
        transitionCounter += 1
        let id = transitionCounter
        transition = EmberTransition(from: currentView(at: Date()), to: target, start: Date(), id: id)
        Task { @MainActor [weak self] in
            try? await Task.sleep(for: .milliseconds(Int(Self.transitionSeconds * 1000) + 10))
            guard let self, self.transition?.id == id else { return }
            self.view = target
            self.transition = nil
        }
    }

    func focusNode(_ node: Int, animate: Bool = true) {
        guard let layout, node >= 0, node < layout.nodes.count else { return }
        let target = Ember.focusTarget(layout, node)
        focus = target
        move(to: Ember.fitTransform(layout.nodes[target], width: width, height: height), animate: animate)
    }

    func fitAll() {
        focusNode(0)
    }

    func zoomOut() {
        guard let layout else { return }
        focusNode(Ember.zoomOutTarget(layout, focus: focus))
    }

    func zoomBy(_ factor: Double) {
        guard let layout else { return }
        var target = transition?.to ?? view
        target.k = max(Ember.minScale(layout, width: width, height: height),
                       min(Ember.maxScale(layout, width: width, height: height), target.k * factor))
        move(to: target, animate: true)
        focus = Ember.focusFromView(layout, target, width: width, height: height)
    }

    func select(_ file: Int, zoom: Bool) {
        let valid = file >= 0 && file < snapshot.files.count ? file : -1
        if valid != selected && valid >= 0 && !reduceMotion {
            linkCounter += 1
            let id = linkCounter
            linkStart = Date()
            Task { @MainActor [weak self] in
                try? await Task.sleep(for: .milliseconds(Int(Self.linkSeconds * 1000) + 20))
                guard let self, self.linkCounter == id else { return }
                self.linkStart = nil
            }
        }
        selected = valid
        if valid >= 0, zoom, let layout,
           let target = Ember.revealTarget(layout, view: currentView(at: Date()), file: valid, width: width, height: height) {
            focusNode(target)
        }
    }

    func selectBestMatch() {
        let best = Ember.bestMatch(snapshot, query: filters.search)
        if best >= 0 { select(best, zoom: true) }
    }

    func tap(at point: CGPoint) {
        guard let layout else { return }
        let current = currentView(at: Date())
        switch Ember.tapAt(layout, view: current, focus: focus, selected: selected, x: Double(point.x), y: Double(point.y),
                           width: width, height: height) {
        case .none:
            break
        case .selectFile(let file):
            select(file, zoom: false)
        case .focus(let node):
            focusNode(node)
        case .zoomOut:
            zoomOut()
        case .clearSelection:
            select(-1, zoom: false)
        }
    }

    func beginGesture() {
        if transition != nil {
            view = currentView(at: Date())
            transition = nil
        }
        interacting = true
    }

    func panBy(dx: Double, dy: Double) {
        view = Ember.pan(view, dx: dx, dy: dy)
    }

    func pinch(by factor: Double, at point: CGPoint) {
        guard let layout, factor.isFinite, factor > 0 else { return }
        view = Ember.zoomAround(view, scale: factor, x: Double(point.x), y: Double(point.y), width: width, height: height,
                                minK: Ember.minScale(layout, width: width, height: height),
                                maxK: Ember.maxScale(layout, width: width, height: height))
    }

    func settle() {
        interacting = false
        guard let layout else { return }
        focus = Ember.focusFromView(layout, view, width: width, height: height)
    }

    func crumbs() -> [EmberCrumb] {
        guard let layout else { return [] }
        return Ember.crumbs(layout, focus: focus, selectedFile: selected)
    }

    func accessibleNodes(limit: Int = 80) -> [EmberAccessibleNode] {
        guard let layout, size.width > 0 else { return [] }
        let current = view
        var files: [EmberAccessibleNode] = []
        var folders: [EmberAccessibleNode] = []
        for index in Ember.visibleNodes(layout, view: current, width: width, height: height, minRadius: 7) {
            let node = layout.nodes[index]
            let p = Ember.toScreen(current, x: node.x, y: node.y, width: width, height: height)
            let r = node.r * current.k
            if node.file >= 0 {
                files.append(EmberAccessibleNode(node: index, file: node.file, x: CGFloat(p.x), y: CGFloat(p.y), diameter: CGFloat(2 * r),
                                                 label: Ember.describeFile(snapshot, file: node.file, rank: rank(of: node.file))))
            } else if index != 0 && r >= 14 {
                folders.append(EmberAccessibleNode(node: index, file: -1, x: CGFloat(p.x), y: CGFloat(p.y), diameter: CGFloat(2 * r),
                                                   label: "Folder " + Ember.describeFolder(node)))
            }
        }
        files.sort { $0.diameter > $1.diameter }
        return Array(folders.prefix(limit / 4)) + Array(files.prefix(limit))
    }

    func activate(_ item: EmberAccessibleNode) {
        if item.file >= 0 {
            select(item.file, zoom: false)
        } else {
            focusNode(item.node)
        }
    }
}
