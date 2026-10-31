import FMAgents
import FMApi
import FMAssistant
import FMBase
import Observation
import SwiftUI
import TipKit
import UIKit
import UniformTypeIdentifiers

@MainActor
@Observable
final class ShellCounts {
    static let shared = ShellCounts()

    var agents: Int = 0
    var agentsTotal: Int = 0
    var timers: Int = 0
    var runnerAgents: [String: Int] = [:]
    var swimmers: [AquariumSwimmer] = []
    var currentRepo: RepoRef?
    private(set) var repoCounts: [RepoSection: Int] = [:]
    private(set) var repoCountsSlug: String = ""
    var pulls: [PublishedPull] = []
    var pullsSlug: String = ""
    var pullsRevision: Int = 0
    var pullsRings: Int = 0
    var newPulls: Int = 0
    private(set) var loadingRepo: String = ""
    @ObservationIgnored private var freshSlugs: Set<String> = []

    init() {
        currentRepo = CodeCurrentRepo.memory.ref
        if let repo = currentRepo { restoreCounts(repo) }
    }

    static func countsKey(_ slug: String) -> String {
        "shell.repoCounts.\(slug)"
    }

    private static var store: KeyValueStore {
        Services.get(KeyValueStore.self) ?? UserDefaultsStore()
    }

    func restoreCounts(_ repo: RepoRef) {
        let slug: String = repo.slug.lowercased()
        guard slug != repoCountsSlug else { return }
        let saved: [String] = ShellCounts.store.stringList(forKey: ShellCounts.countsKey(slug)) ?? []
        guard !saved.isEmpty else { return }
        var counts: [RepoSection: Int] = [:]
        for line in saved {
            let parts: [Substring] = line.split(separator: ":", maxSplits: 1)
            guard parts.count == 2, let section = RepoSection(rawValue: String(parts[0])), let count = Int(parts[1])
            else { continue }
            counts[section] = count
        }
        repoCounts = counts
        repoCountsSlug = slug
    }

    func adopt(_ repo: RepoRef, counts: [RepoSection: Int]) {
        let slug: String = repo.slug.lowercased()
        let next: [RepoSection: Int] = slug == repoCountsSlug ? repoCounts.merging(counts) { $1 } : counts
        if repoCounts != next { repoCounts = next }
        if repoCountsSlug != slug { repoCountsSlug = slug }
        ShellCounts.store.set(
            next.map { "\($0.key.rawValue):\($0.value)" }.sorted(), forKey: ShellCounts.countsKey(slug))
    }

    func isFresh(_ repo: RepoRef) -> Bool {
        freshSlugs.contains(repo.slug.lowercased())
    }

    func markFresh(_ repo: RepoRef, _ fresh: Bool) {
        if fresh {
            freshSlugs.insert(repo.slug.lowercased())
        } else {
            freshSlugs.remove(repo.slug.lowercased())
        }
    }

    func setLoading(_ repo: RepoRef?) {
        let slug: String = repo?.slug.lowercased() ?? ""
        if loadingRepo != slug { loadingRepo = slug }
    }

    func isLoadingCounts(_ repo: RepoRef) -> Bool {
        loadingRepo == repo.slug.lowercased()
    }

    func repoCount(_ section: RepoSection) -> Int? {
        guard let repo = currentRepo, repo.slug.lowercased() == repoCountsSlug else { return nil }
        return repoCounts[section]
    }

    func knowsCounts(of repo: RepoRef) -> Bool {
        repo.slug.lowercased() == repoCountsSlug
    }

    var newPullsBadge: Int {
        guard let repo = currentRepo, repo.slug.lowercased() == pullsSlug else { return 0 }
        return newPulls
    }
}

@MainActor
@Observable
final class ShellChrome {
    static let shared = ShellChrome()

    private(set) var bare: [AppTab: Int] = [:]

    private(set) var returning: Set<AppTab> = []

    func isBare(_ tab: AppTab) -> Bool { (bare[tab] ?? 0) > 0 && !returning.contains(tab) }

    func hide(on tab: AppTab) {
        returning.remove(tab)
        bare[tab, default: 0] += 1
    }

    func show(on tab: AppTab) {
        let left: Int = (bare[tab] ?? 0) - 1
        bare[tab] = left > 0 ? left : nil
        if left <= 0 { returning.remove(tab) }
    }

    func returnToRoot(on tab: AppTab) {
        guard bare[tab] != nil else { return }
        returning.insert(tab)
    }

    func stayOff(_ tab: AppTab) { returning.remove(tab) }

    private(set) var stackReturns: [AppTab: Int] = [:]

    func leave(_ dismiss: DismissAction, stack: AppTab? = nil) {
        let router: AppRouter? = AppNavigator.shared?.router
        let tab: AppTab = router?.selectedTab ?? .agents
        var transaction: Transaction = Transaction()
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            bare[tab] = nil
            returning.remove(tab)
            if ShellDetailHost.shared.closes(tab) { return }
            if stack == .agents && tab == .agents, let router {
                let path: [Route] = router.path(for: .agents)
                let list: Int? = path.lastIndex { (route: Route) -> Bool in
                    if case .agents = route { return true }
                    return false
                }
                router.setPath(list.map { Array(path.prefix(through: $0)) } ?? [], for: .agents)
                stackReturns[tab, default: 0] += 1
            } else {
                dismiss()
            }
        }
    }

    private(set) var revealed: [AppTab: Int] = [:]

    func isRevealed(_ tab: AppTab) -> Bool { (revealed[tab] ?? 0) > 0 }

    func reveal(on tab: AppTab) { revealed[tab, default: 0] += 1 }

    func conceal(on tab: AppTab) {
        let left: Int = (revealed[tab] ?? 0) - 1
        revealed[tab] = left > 0 ? left : nil
    }
}

struct BareShellTop: ViewModifier {
    @State private var tab: AppTab?

    func body(content: Content) -> some View {
        content
            .onAppear {
                guard tab == nil else { return }
                let selected: AppTab = AppNavigator.shared?.router.selectedTab ?? .agents
                tab = selected
                ShellChrome.shared.hide(on: selected)
            }
            .onDisappear {
                guard let selected = tab else { return }
                tab = nil
                ShellChrome.shared.show(on: selected)
            }
    }
}

extension View {
    func bareShellTop() -> some View { modifier(BareShellTop()) }
}

private struct ShellStackTabKey: EnvironmentKey {
    static let defaultValue: AppTab? = nil
}

private struct ShellTabHiddenKey: EnvironmentKey {
    static let defaultValue: Bool = false
}

extension EnvironmentValues {
    var shellStackTab: AppTab? {
        get { self[ShellStackTabKey.self] }
        set { self[ShellStackTabKey.self] = newValue }
    }

    var shellTabHidden: Bool {
        get { self[ShellTabHiddenKey.self] }
        set { self[ShellTabHiddenKey.self] = newValue }
    }
}

@MainActor
@Observable
final class ShellBarPrefs {
    static let shared = ShellBarPrefs(store: Services.get(KeyValueStore.self) ?? UserDefaultsStore())

    @ObservationIgnored private let order: ShellBarOrder
    @ObservationIgnored private let store: KeyValueStore
    private(set) var sort: ShellBarSort
    private(set) var recent: [String]
    private(set) var uses: [String: Int]
    private(set) var layout: ShellBarLayout
    private(set) var menu: ShellMenuLayout
    private(set) var home: ShellHomeStyle
    private(set) var barStyle: ShellBarStyle
    var homeGroups: [ShellHomeGroupInfo] = []
    var drag: ShellBarDrag?

    init(store: KeyValueStore) {
        self.store = store
        order = ShellBarOrder(store: store)
        sort = order.sort
        recent = order.recent
        uses = order.uses
        layout = ShellBarLayout.load(store)
        menu = ShellMenuLayout.load(store)
        home = ShellHomeStyle.load(store)
        barStyle = ShellBarStyle.load(store)
    }

    func styleBar(_ edit: (inout ShellBarStyle) -> Void) {
        var next: ShellBarStyle = barStyle
        edit(&next)
        next = next.clamped
        guard next != barStyle else { return }
        barStyle = next
        next.save(store)
    }

    private var limit: Int { barStyle.capacity }

    func styleHome(_ edit: (inout ShellHomeStyle) -> Void) {
        var next: ShellHomeStyle = home
        edit(&next)
        next.across = ShellHomeStyle.clampedAcross(next.across)
        guard next != home else { return }
        home = next
        next.save(store)
    }

    func arranged(_ groups: [ShellMenuGroup]) -> [ShellMenuGroup] {
        let placed: [[ShellBarItem]] = menu.arranged(
            groups, group: { (group: ShellMenuGroup) -> String in group.id },
            items: { (group: ShellMenuGroup) -> [ShellBarItem] in group.items },
            id: { (item: ShellBarItem) -> String in item.id })
        return zip(groups, placed).compactMap { (entry: (ShellMenuGroup, [ShellBarItem])) -> ShellMenuGroup? in
            entry.1.isEmpty ? nil : ShellMenuGroup(id: entry.0.id, title: entry.0.title, items: entry.1)
        }
    }

    @discardableResult
    func moveInMenu(_ itemId: String, to group: String, near target: String?, groups: [ShellMenuGroup]) -> Bool {
        var next: ShellMenuLayout = menu
        var current: [(String, [String])] = groups.map { ($0.id, $0.items.map(\.id)) }
        if !current.contains(where: { $0.0 == group }) {
            current.append((group, []))
        }
        next.move(itemId, to: group, near: target, current: current)
        guard next != menu else { return false }
        menu = next
        next.save(store)
        return true
    }

    func resetMenu() {
        guard !menu.isEmpty else { return }
        menu = ShellMenuLayout()
        menu.save(store)
    }

    var ranking: [String] {
        ShellBarOrder.ranking(sort, recent: recent, uses: uses)
    }

    func setSort(_ next: ShellBarSort) {
        order.sort = next
        sort = next
    }

    func use(_ id: String) {
        order.use(id)
        recent = order.recent
        uses = order.uses
    }

    func visit(_ ids: [String]) {
        guard order.visit(ids) else { return }
        recent = order.recent
        uses = order.uses
    }

    func clear() {
        order.clear()
        recent = order.recent
        uses = order.uses
    }

    static let keep: [String] = [ShellHome.homeId]

    func entries(_ items: [ShellBarItem]) -> [ShellBarEntry] {
        layout.barSlots(items, ranking: ranking, keep: ShellBarPrefs.keep, limit: limit, id: \.id).map {
            ShellBarEntry(slot: $0)
        }
    }

    func inBar(_ itemId: String, items: [ShellBarItem]) -> Bool {
        layout.inBar(itemId, items: items, ranking: ranking, keep: ShellBarPrefs.keep, limit: limit, id: \.id)
    }

    func barIds(_ items: [ShellBarItem]) -> Set<String> {
        var ids: Set<String> = []
        for slot in layout.barSlots(items, ranking: ranking, keep: ShellBarPrefs.keep, limit: limit, id: \.id) {
            switch slot {
            case let .item(item): ids.insert(item.id)
            case let .folder(_, members): ids.formUnion(members.map(\.id))
            }
        }
        return ids
    }

    func barIsFull(_ items: [ShellBarItem]) -> Bool {
        layout.barIsFull(items, ranking: ranking, keep: ShellBarPrefs.keep, limit: limit, id: \.id)
    }

    func removeFromBar(_ ids: [String], items: [ShellBarItem]) {
        let current: [String] = ranking
        let room: Int = limit
        arrange {
            $0.removeFromBar(ids, items: items, ranking: current, keep: ShellBarPrefs.keep, limit: room, id: \.id)
        }
    }

    @discardableResult
    func putInBar(_ itemId: String, at target: String? = nil, items: [ShellBarItem]) -> Bool {
        let current: [String] = ranking
        var next: ShellBarLayout = layout
        guard
            next.putInBar(
                itemId, at: target, items: items, ranking: current, keep: ShellBarPrefs.keep, limit: limit, id: \.id)
        else { return false }
        arrange { $0 = next }
        if target != nil && sort != .usual {
            setSort(.usual)
        }
        return true
    }

    func arrange(_ edit: (inout ShellBarLayout) -> Void) {
        var next: ShellBarLayout = layout
        edit(&next)
        guard next != layout else { return }
        layout = next
        next.save(store)
    }

    private func fitted(_ items: [ShellBarItem]) -> ShellBarLayout {
        var next: ShellBarLayout = layout
        next.fit(items, ranking: ranking, keep: ShellBarPrefs.keep, limit: limit, id: \.id)
        return next
    }

    func canStep(_ slotId: String, _ step: ShellBarStep, items: [ShellBarItem]) -> Bool {
        fitted(items).canStep(slotId, step, items: items, ranking: ranking, id: \.id)
    }

    func step(_ slotId: String, _ step: ShellBarStep, items: [ShellBarItem]) {
        let current: [String] = ranking
        var next: ShellBarLayout = fitted(items)
        next.step(slotId, step, items: items, ranking: current, id: \.id)
        arrange { $0 = next }
        if sort != .usual {
            setSort(.usual)
        }
    }
}

struct ShellBarSettingsItems: View {
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let prefs: ShellBarPrefs
    var includesHome: Bool = true

    var body: some View {
        Picker(
            selection: Binding(
                get: { prefs.sort },
                set: { (next: ShellBarSort) in
                    withAnimation(reduceMotion ? nil : .snappy) {
                        prefs.setSort(next)
                    }
                })
        ) {
            ForEach(ShellBarSort.allCases, id: \.self) { (sort: ShellBarSort) in
                SwiftUI.Label(ShellBarSettingsItems.title(sort), systemImage: ShellBarSettingsItems.symbol(sort))
                    .tag(sort)
                    .accessibilityIdentifier("shell-bar-sort-\(sort.rawValue)")
            }
        } label: {
            SwiftUI.Label("Sort", systemImage: "arrow.up.arrow.down")
        }
        .pickerStyle(.inline)
        .accessibilityIdentifier("shell-bar-sort")
        Button {
            withAnimation(reduceMotion ? nil : .snappy) {
                prefs.clear()
            }
        } label: {
            SwiftUI.Label("Clear usage history", systemImage: "arrow.counterclockwise")
        }
        .disabled(prefs.recent.isEmpty && prefs.uses.isEmpty)
        .accessibilityIdentifier("shell-bar-clear-recent")
        Button {
            withAnimation(reduceMotion ? nil : .snappy) {
                prefs.arrange { $0.showAll() }
            }
        } label: {
            SwiftUI.Label("Reset bar icons", systemImage: "eye")
        }
        .disabled(prefs.layout.hidden.isEmpty && prefs.layout.bar == nil)
        .accessibilityIdentifier("shell-bar-show-hidden")
        ShellBarResetButton(prefs: prefs)
        if includesHome {
            Button(role: .destructive) {
                withAnimation(reduceMotion ? nil : .snappy) {
                    prefs.resetMenu()
                }
            } label: {
                SwiftUI.Label("Reset Home order", systemImage: "square.grid.3x3")
            }
            .disabled(prefs.menu.isEmpty)
            .accessibilityIdentifier("shell-menu-reset-order")
        }
    }

    static func title(_ sort: ShellBarSort) -> String {
        switch sort {
        case .usual: return "Usual order"
        case .recent: return "Recently used"
        case .frequent: return "Most used"
        }
    }

    static func symbol(_ sort: ShellBarSort) -> String {
        switch sort {
        case .usual: return "list.bullet"
        case .recent: return "clock.arrow.circlepath"
        case .frequent: return "chart.bar"
        }
    }
}

struct ShellBarDrag: Equatable {
    let id: String
    let fromBar: Bool
}

struct ShellBarItem: Identifiable {
    enum Kind {
        case tab(position: Int, total: Int)
        case action
    }

    let id: String
    let title: String
    let symbol: String
    let badge: Int
    let selected: Bool
    let kind: Kind
    var pulse: Int = 0
    var flightTarget: Bool = false
    var running: Bool = false
    var active: Bool = false
    var count: Int?
    var live: Int = 0
    var progress: ShellBarProgress?
    var loading: Bool = false
    var action: () -> Void
}

struct ShellBarProgress: Equatable, Sendable {
    var startedAtMs: Int64
    var expectedMs: Int64

    var estimated: Bool { startedAtMs > 0 && expectedMs > 0 }

    func fraction(nowMs: Int64) -> Double? {
        guard estimated else { return nil }
        return min(0.99, max(0, Double(nowMs - startedAtMs) / Double(expectedMs)))
    }

    static func nowMs(_ date: Date = Date()) -> Int64 { Int64(date.timeIntervalSince1970 * 1000) }

    static func latest(_ runs: [ShellBarProgress]) -> ShellBarProgress? {
        guard !runs.isEmpty else { return nil }
        if let unknown = runs.first(where: { !$0.estimated }) { return unknown }
        return runs.max { $0.startedAtMs + $0.expectedMs < $1.startedAtMs + $1.expectedMs }
    }
}

enum ShellPage: Equatable, Sendable {
    case home
    case reportBug
}

struct ShellMenuGroup: Identifiable {
    let id: String
    let title: String
    let items: [ShellBarItem]
}

struct ShellBarLensSpot: Equatable {
    let column: Int
    let row: Int
}

struct ShellBottomBar: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let items: [ShellBarItem]
    var onPackage: (String) -> Void = { (_: String) in }
    var onCapsuleTop: (CGFloat) -> Void = { (_: CGFloat) in }

    @State private var prefs: ShellBarPrefs = ShellBarPrefs.shared
    @State private var naming: ShellBarNaming?
    @State private var targeted: Bool = false
    @State private var available: CGFloat = 0
    @State private var settingsShown: Bool = false
    @State private var lensSpot: ShellBarLensSpot?
    @State private var lensStretched: Bool = false

    @ScaledMetric(relativeTo: .body) private var iconSize: CGFloat = 20
    @ScaledMetric(relativeTo: .body) private var cellWidth: CGFloat = 64
    @ScaledMetric(relativeTo: .body) private var cellHeight: CGFloat = 52

    private var style: ShellBarStyle { prefs.barStyle }

    private var lensSlides: Bool { style.selection == .lens && !style.shape.separate }

    private var lensInset: CGFloat { lensSlides && style.width == .full ? 4 : 0 }

    private func sideInset(_ metrics: ShellBarMetrics) -> CGFloat {
        guard lensInset > 0, !metrics.scrolls else { return CGFloat(metrics.padding) }
        let rows: CGFloat = CGFloat(metrics.columns) * CGFloat(metrics.slot)
            + CGFloat(metrics.columns - 1) * CGFloat(metrics.gap)
        return max(CGFloat(metrics.padding), (available - rows) / 2)
    }

    private var glyphSize: CGFloat {
        (iconSize * CGFloat(style.size.scale) * (style.selection == .lens ? 1.1 : 1)).rounded()
    }

    var body: some View {
        let entries: [ShellBarEntry] = shown
        let metrics: ShellBarMetrics = style.metrics(
            available: max(0, Double(available) - 2 * Double(lensInset)), count: entries.count, cell: Double(cellWidth),
            height: Double(cellHeight) + (style.selection == .lens ? 2 : 0))
        let barShape: AnyShape = ShellBarShapes.bar(style, height: CGFloat(metrics.height), rows: metrics.rows)
        return container {
            VStack(spacing: 0) {
                carousel(entries, metrics: metrics)
                    .shellBarBacked(style, shape: barShape)
                    .overlay {
                        if targeted {
                            barShape.stroke(fm.accent, lineWidth: 1.5)
                                .allowsHitTesting(false)
                        }
                    }
                    .onDrop(of: [.text], delegate: drop(nil, isTargeted: $targeted))
                    .onGeometryChange(for: CGFloat.self, of: { $0.frame(in: .named(ShellDock.barSpace)).minY }) {
                        (top: CGFloat) in
                        onCapsuleTop(top)
                    }
            }
            .frame(maxWidth: .infinity)
            .onGeometryChange(for: CGFloat.self, of: { $0.size.width }) { (width: CGFloat) in
                available = width
            }
        }
        .padding(.horizontal, ShellBottomBar.sideMargin)
        .padding(.top, FmSpace.x1)
        .padding(.bottom, ShellBottomBar.floatHeight)
        .dynamicTypeSize(...DynamicTypeSize.accessibility2)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("shell-bottom-bar")
        .shellBarNaming($naming, prefs: prefs)
        .sheet(isPresented: $settingsShown) {
            ShellBarSettingsSheet(prefs: prefs)
        }
    }

    @MainActor static func run(_ item: ShellBarItem, prefs: ShellBarPrefs, reduceMotion: Bool) {
        withAnimation(reduceMotion ? nil : .spring(response: 0.45, dampingFraction: 0.8)) {
            prefs.use(item.id)
        }
        var transaction: Transaction = Transaction()
        transaction.disablesAnimations = true
        withTransaction(transaction) {
            item.action()
        }
    }

    private var landingItems: [ShellBarItem] {
        guard items.contains(where: { $0.flightTarget && !prefs.inBar($0.id, items: items) }) else { return items }
        return items.map { (item: ShellBarItem) -> ShellBarItem in
            var next: ShellBarItem = item
            if item.id == ShellHome.homeId {
                next.flightTarget = true
            }
            return next
        }
    }

    private var shown: [ShellBarEntry] {
        prefs.entries(landingItems)
    }

    private var heat: [String: Double] {
        guard style.highlightFrequent else { return [:] }
        let ids: [String] = items.map(\.id)
        let uses: [String: Int] = prefs.uses
        let most: Int = ShellBarOrder.mostUses(uses, among: ids)
        var result: [String: Double] = [:]
        for id in ids {
            let level: Double = ShellBarOrder.heat(id, uses: uses, most: most)
            if level > 0 {
                result[id] = level
            }
        }
        return result
    }

    private func carousel(_ entries: [ShellBarEntry], metrics: ShellBarMetrics) -> some View {
        let warmth: [String: Double] = heat
        let lines: [[ShellBarEntry]] = style.rowSplit(entries)
        let spot: ShellBarLensSpot? = lensTarget(lines)
        return ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView(.horizontal, showsIndicators: false) {
                VStack(alignment: .leading, spacing: CGFloat(metrics.rowGap)) {
                    ForEach(Array(lines.enumerated()), id: \.offset) {
                        (line: (offset: Int, element: [ShellBarEntry])) in
                        HStack(spacing: CGFloat(metrics.gap)) {
                            ForEach(line.element) { (entry: ShellBarEntry) in
                                slot(
                                    entry, metrics: metrics, first: entry.id == line.element.first?.id,
                                    last: entry.id == line.element.last?.id && line.element.count == metrics.columns,
                                    top: line.offset == 0,
                                    bottom: line.offset == lines.count - 1, heat: warmth
                                )
                                .id(entry.id)
                            }
                        }
                    }
                }
                .background(alignment: .topLeading) { slidingLens(metrics) }
                .padding(.vertical, CGFloat(metrics.padding))
                .padding(.horizontal, sideInset(metrics))
            }
            .scrollBounceBehavior(.basedOnSize, axes: .horizontal)
            .scrollClipDisabled(style.shape.separate && !metrics.scrolls)
            .frame(width: style.width == .fit ? CGFloat(metrics.width) : nil)
            .onChange(of: spot, initial: true) { (_: ShellBarLensSpot?, next: ShellBarLensSpot?) in
                let first: Bool = lensSpot == nil || next == nil
                withAnimation(first || reduceMotion ? nil : .spring(response: 0.42, dampingFraction: 0.72)) {
                    lensSpot = next
                }
                guard !first, !reduceMotion else { return }
                withAnimation(.easeOut(duration: 0.12)) { lensStretched = true }
                withAnimation(.spring(response: 0.4, dampingFraction: 0.5).delay(0.12)) { lensStretched = false }
            }
            .onChange(of: selectedId) { (_: String?, next: String?) in
                guard let next = next else { return }
                withAnimation(.snappy) {
                    proxy.scrollTo(next)
                }
            }
            .onChange(of: prefs.ranking.first) { (_: String?, _: String?) in
                guard prefs.sort != .usual, let first = shown.first?.id else { return }
                withAnimation(reduceMotion ? nil : .snappy) {
                    proxy.scrollTo(first, anchor: .leading)
                }
            }
        }
        .accessibilityIdentifier("shell-bottom-carousel")
    }

    private var selectedId: String? {
        guard let item = items.first(where: { $0.selected }) else { return nil }
        return shown.first { $0.contains(item.id) }?.id
    }

    private func lensTarget(_ lines: [[ShellBarEntry]]) -> ShellBarLensSpot? {
        guard lensSlides, let selectedId = selectedId else { return nil }
        for (row, line) in lines.enumerated() {
            if let column = line.firstIndex(where: { $0.id == selectedId }) {
                return ShellBarLensSpot(column: column, row: row)
            }
        }
        return nil
    }

    @ViewBuilder private func slidingLens(_ metrics: ShellBarMetrics) -> some View {
        if let lensSpot = lensSpot {
            let slot: CGFloat = CGFloat(metrics.slot)
            let wide: CGFloat = slot + 6
            let start: CGFloat = CGFloat(lensSpot.column) * (slot + CGFloat(metrics.gap)) - (wide - slot) / 2
            ShellBarLens()
                .frame(width: wide, height: CGFloat(metrics.height))
                .scaleEffect(x: lensStretched ? 1.1 : 1, y: lensStretched ? 0.92 : 1)
                .offset(
                    x: start,
                    y: CGFloat(lensSpot.row) * CGFloat(metrics.height + metrics.rowGap))
        }
    }

    @ViewBuilder private func slot(
        _ entry: ShellBarEntry, metrics: ShellBarMetrics, first: Bool, last: Bool, top: Bool, bottom: Bool,
        heat: [String: Double]
    ) -> some View {
        let side: CGFloat = CGFloat(metrics.height)
        let width: CGFloat = CGFloat(metrics.slot)
        let shape: AnyShape = ShellBarShapes.cell(style, side: side, first: first, last: last, top: top, bottom: bottom)
        switch entry.slot {
        case let .item(item):
            cell(item, shape: shape, width: width, height: side, heat: heat[item.id] ?? 0)
                .contextMenu {
                    ShellBarItemMenu(
                        item: item, items: items, prefs: prefs,
                        onName: { (next: ShellBarNaming) in naming = next }, onPackage: onPackage)
                    Section {
                        settingsButton
                    }
                }
                .onDrag {
                    prefs.drag = ShellBarDrag(id: item.id, fromBar: true)
                    return NSItemProvider(object: item.id as NSString)
                } preview: {
                    ShellBarCellLabel(item: item, iconSize: glyphSize, look: style)
                        .frame(width: width, height: side)
                        .background(Capsule().fill(fm.bgOverlay))
                }
                .onDrop(of: [.text], delegate: drop(entry.id, isTargeted: nil))
        case let .folder(folder, members):
            ShellBarFolderCell(
                folder: folder, members: members, items: items, prefs: prefs, iconSize: glyphSize,
                cellWidth: width, cellHeight: side, shape: shape, look: style,
                onName: { (next: ShellBarNaming) in naming = next }, onPackage: onPackage,
                onSettings: { settingsShown = true })
                .onDrop(of: [.text], delegate: drop(entry.id, isTargeted: nil))
        }
    }

    private var settingsButton: some View {
        Button {
            settingsShown = true
        } label: {
            SwiftUI.Label("Bottom bar settings…", systemImage: "slider.horizontal.3")
        }
        .accessibilityIdentifier("shell-bar-open-settings")
    }

    private func drop(_ target: String?, isTargeted: Binding<Bool>?) -> ShellBarDropDelegate {
        ShellBarDropDelegate(
            prefs: prefs, items: items, target: target, isTargeted: isTargeted,
            onDrop: { (drag: ShellBarDrag) -> Bool in
                let placed: Bool = withAnimation(reduceMotion ? nil : .snappy) {
                    prefs.putInBar(drag.id, at: target, items: items)
                }
                if placed {
                    FmHaptics.success()
                } else {
                    FmHaptics.warning()
                }
                return placed
            })
    }

    static func badgeText(_ count: Int) -> String {
        return count > 999 ? "999+" : "\(count)"
    }

    static func value(_ item: ShellBarItem) -> String {
        let new: String = item.badge > 0 ? "\(ShellBottomBar.badgeText(item.badge)) new" : ""
        if item.live > 0 {
            let live: String = "\(ShellBottomBar.badgeText(item.live)) running"
            return new.isEmpty ? live : live + ", " + new
        }
        guard item.active else { return new }
        var state: String = "Active"
        if let fraction = item.progress?.fraction(nowMs: ShellBarProgress.nowMs()) {
            state = "Running, \(Int(fraction * 100)) percent"
        }
        return new.isEmpty ? state : state + ", " + new
    }

    static func countText(_ count: Int) -> String {
        return count > 9999 ? "\(count / 1000)k" : count.formatted()
    }

    static let innerRadius: CGFloat = 10
    static let sideMargin: CGFloat = 10
    static let floatHeight: CGFloat = 24

    private func cell(_ item: ShellBarItem, shape: AnyShape, width: CGFloat, height: CGFloat, heat: Double)
        -> some View
    {
        Button {
            FmHaptics.selection()
            prefs.drag = nil
            ShellBottomBar.run(item, prefs: prefs, reduceMotion: reduceMotion)
        } label: {
            ShellBarCellLabel(item: item, iconSize: glyphSize, showsCounts: style.showCounts, look: style, heat: heat)
                .frame(width: width, height: height)
                .shellBarSlotChrome(shape, style: style, selected: item.selected, slides: lensSlides)
                .contentShape(shape)
        }
        .buttonStyle(ShellBarButtonStyle())
        .accessibilityLabel(Text(item.title))
        .accessibilityValue(Text(ShellBottomBar.value(item)))
        .accessibilityHint(hint(item))
        .accessibilityAddTraits(item.selected ? [.isSelected, .isButton] : [.isButton])
        .accessibilityShowsLargeContentViewer {
            SwiftUI.Label(item.title, systemImage: item.symbol)
        }
        .accessibilityIdentifier("bottom-bar-\(item.id)")
    }

    private func hint(_ item: ShellBarItem) -> Text {
        switch item.kind {
        case let .tab(position, total):
            return Text("Tab, \(position + 1) of \(total)")
        case .action:
            return Text("")
        }
    }

    @ViewBuilder private func container<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            GlassEffectContainer(spacing: style.shape.separate ? 0 : FmSpace.x3) {
                content()
            }
        } else {
            content()
        }
        #else
        content()
        #endif
    }
}

struct ShellBarCellLabel: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let item: ShellBarItem
    let iconSize: CGFloat
    var showsCounts: Bool = false
    var spacing: CGFloat = 3
    var minimumScale: CGFloat = 0.75
    var pillCounts: Bool = false
    var look: ShellBarStyle = ShellBarStyle()
    var heat: Double = 0

    private var badgeText: String {
        showsCounts ? ShellBottomBar.countText(item.badge) : ShellBottomBar.badgeText(item.badge)
    }

    private var tiled: Bool { look.icons == .tiles }

    private var tileSide: CGFloat { (iconSize * 1.5).rounded() }

    private var glyphHeight: CGFloat { tiled ? tileSide : iconSize + 4 }

    private var activity: Bool { look.showActivity }

    private var homeMarks: Bool { look.marks == .home }

    private var ringShown: Bool { item.progress != nil && activity }

    private var ringSize: CGFloat { tiled ? tileSide - 5 : iconSize + 10 }

    private var haloOutset: CGFloat { ringShown && !tiled ? 8 : 0 }

    var body: some View {
        VStack(spacing: spacing) {
            glyph
                .frame(width: homeMarks ? glyphHeight : nil, height: glyphHeight)
                .overlay {
                    if let progress = item.progress, activity {
                        ShellBarProgressRing(progress: progress, size: ringSize, lineWidth: tiled ? 1.75 : 2)
                            .transition(.scale.combined(with: .opacity))
                    }
                }
                .background {
                    if item.active && activity {
                        activityMark
                            .transition(.scale.combined(with: .opacity))
                    } else if heat > 0 {
                        Circle()
                            .fill(
                                RadialGradient(
                                    colors: [fm.accent.opacity(0.1 + 0.3 * heat), fm.accent.opacity(0)],
                                    center: .center, startRadius: 0, endRadius: iconSize * (0.8 + 0.3 * CGFloat(heat)))
                            )
                            .frame(width: iconSize * 2.2, height: iconSize * 2.2)
                            .allowsHitTesting(false)
                            .accessibilityHidden(true)
                    }
                }
                .onGeometryChange(for: CGRect.self, of: { $0.frame(in: .global) }) { (frame: CGRect) in
                    if item.flightTarget {
                        ShellLaunchFlight.shared.target = frame
                    }
                }
                .overlay(alignment: .topTrailing) {
                    if homeMarks {
                        homeCountAnchor
                    } else {
                        plainCount
                    }
                }
                .overlay(alignment: .topLeading) {
                    if homeMarks {
                        homeLiveAnchor
                    } else if item.live > 0 && activity {
                        ShellHeartbeatCount(count: item.live)
                            .alignmentGuide(.leading) { (d: ViewDimensions) in d[.trailing] }
                            .offset(x: 2, y: -6)
                            .transition(.scale.combined(with: .opacity))
                    }
                }
                .overlay(alignment: .bottomLeading) { desktopOfflineAnchor }
            if look.labels.shows(selected: item.selected) {
                Text(item.title)
                    .font(.system(size: labelSize, weight: item.selected ? .semibold : .medium))
                    .foregroundStyle(labelColor)
                    .lineLimit(1)
                    .minimumScaleFactor(minimumScale)
                    .transition(.opacity)
            }
        }
        .shellBarLift(!look.shape.backed, dark: fm.dark)
    }

    @ViewBuilder private var desktopOfflineAnchor: some View {
        if ShellDesktopOfflineDot.shows(item.id) {
            let drop: CGFloat = tiled ? tileSide * 0.09 : (glyphHeight - iconSize) / 2
            let inset: CGFloat = tiled || homeMarks ? drop : 0
            Color.clear
                .frame(width: 0, height: 0)
                .overlay { ShellDesktopOfflineDot() }
                .offset(x: inset, y: -drop)
                .transition(.scale.combined(with: .opacity))
        }
    }

    @ViewBuilder private var activityMark: some View {
        if homeMarks {
            ShellHomeActivityRing(
                size: (tiled ? tileSide + 5 : iconSize + 14) + haloOutset, corner: tiled ? tileSide * 0.3 + 2.5 : nil)
        } else {
            ShellActivityHalo(size: (tiled ? tileSide : iconSize) + 10 + haloOutset)
        }
    }

    @ViewBuilder private var plainCount: some View {
        if item.badge > 0 {
            countMark(badgeText, value: item.badge, unread: true)
                .keyframeAnimator(initialValue: 1.0, trigger: item.pulse) { badge, scale in
                    badge.scaleEffect(scale)
                } keyframes: { (_: Double) in
                    KeyframeTrack {
                        SpringKeyframe(1.6, duration: 0.18)
                        SpringKeyframe(0.9, duration: 0.16)
                        SpringKeyframe(1.0, duration: 0.2)
                    }
                }
                .alignmentGuide(.trailing) { (d: ViewDimensions) in d[.leading] }
                .offset(x: pillCounts ? -8 : -2, y: pillCounts ? -9 : -6)
                .transition(.scale.combined(with: .opacity))
                .accessibilityHidden(true)
        } else if showsCounts, let count = item.count, count > 0 {
            countMark(ShellBottomBar.countText(count), value: count, unread: false)
                .alignmentGuide(.trailing) { (d: ViewDimensions) in d[.leading] }
                .offset(x: pillCounts ? -8 : -2, y: pillCounts ? -9 : -6)
                .transition(.scale.combined(with: .opacity))
                .accessibilityHidden(true)
        }
    }

    private var anchorInset: CGFloat {
        tiled ? min(12, tileSide * 0.28) : (glyphHeight - iconSize) / 2 + iconSize * 0.3
    }

    private var anchorDrop: CGFloat {
        let drop: CGFloat = tiled ? min(10, tileSide * 0.22) : (glyphHeight - iconSize) / 2 + 6
        return look.shape.separate ? drop : max(drop, ShellBarCellLabel.markHeight - 6)
    }

    static let markHeight: CGFloat = 22

    private var homeCountAnchor: some View {
        Color.clear
            .frame(width: 0, height: 0)
            .overlay(alignment: .bottomLeading) {
                if item.badge > 0 {
                    ShellHomeCountMark(text: badgeText, value: item.badge, unread: true)
                        .keyframeAnimator(initialValue: 1.0, trigger: item.pulse) { badge, scale in
                            badge.scaleEffect(scale, anchor: .bottomLeading)
                        } keyframes: { (_: Double) in
                            KeyframeTrack {
                                SpringKeyframe(1.4, duration: 0.18)
                                SpringKeyframe(0.92, duration: 0.16)
                                SpringKeyframe(1.0, duration: 0.2)
                            }
                        }
                        .transition(.scale(scale: 0.4, anchor: .bottomLeading).combined(with: .opacity))
                } else if showsCounts, let count = item.count, count > 0 {
                    ShellHomeCountMark(text: ShellBottomBar.countText(count), value: count, unread: false)
                        .transition(.scale(scale: 0.4, anchor: .bottomLeading).combined(with: .opacity))
                }
            }
            .offset(x: -anchorInset, y: anchorDrop)
            .accessibilityHidden(true)
    }

    @ViewBuilder private var homeLiveAnchor: some View {
        if item.live > 0 && activity {
            Color.clear
                .frame(width: 0, height: 0)
                .overlay(alignment: .bottomTrailing) {
                    ShellHeartbeatCount(count: item.live)
                        .fixedSize()
                }
                .offset(x: anchorInset, y: anchorDrop)
                .transition(.scale.combined(with: .opacity))
        }
    }

    private var labelSize: CGFloat { ((look.selection == .lens ? 11 : 10) * CGFloat(look.size.scale)).rounded() }

    private var labelColor: Color {
        if look.selection == .lens {
            return item.selected ? fm.textPrimary : fm.textSecondary
        }
        return item.selected ? fm.accent : fm.textSecondary
    }

    private var glyphColor: Color {
        if look.selection == .lens {
            if item.selected { return fm.textPrimary }
            return item.active || look.icons == .accent ? fm.accent : fm.textSecondary
        }
        return item.selected || item.active || look.icons == .accent ? fm.accent : fm.textPrimary
    }

    private var symbol: some View {
        Image(systemName: item.symbol)
            .font(.system(size: tiled ? iconSize * 0.82 : iconSize, weight: item.selected ? .semibold : .regular))
            .symbolVariant(look.icons == .filled ? .fill : .none)
            .foregroundStyle(glyphColor)
            .scaleEffect(ringShown ? 0.78 : 1)
            .animation(reduceMotion ? nil : .snappy, value: ringShown)
            .symbolEffect(.bounce, value: item.pulse)
    }

    @ViewBuilder private var glyph: some View {
        if tiled {
            symbol
                .frame(width: tileSide, height: tileSide)
                .background(tile)
        } else {
            symbol
        }
    }

    private var tile: some View {
        let shape: RoundedRectangle = RoundedRectangle(cornerRadius: tileSide * 0.3, style: .continuous)
        let raised: Color = fm.dark ? fm.bgElevated : fm.bgRaised
        return shape
            .fill(LinearGradient(colors: [raised, fm.bgOverlay], startPoint: .top, endPoint: .bottom))
            .overlay {
                if item.selected {
                    shape.fill(fm.accent.opacity(0.16))
                }
            }
            .overlay {
                shape.strokeBorder(
                    LinearGradient(
                        colors: [Color.white.opacity(fm.dark ? 0.16 : 0.9), Color.white.opacity(fm.dark ? 0.03 : 0.2)],
                        startPoint: .top, endPoint: .bottom),
                    lineWidth: 0.75)
            }
            .shadow(color: Color.black.opacity(fm.dark ? 0.45 : 0.1), radius: 2.5, y: 1.5)
            .accessibilityHidden(true)
    }

    @ViewBuilder private func countMark(_ text: String, value: Int, unread: Bool) -> some View {
        if pillCounts {
            Text(verbatim: text)
                .font(.system(.caption2, design: .rounded, weight: .bold).monospacedDigit())
                .foregroundStyle(unread ? fm.onPrimary : fm.count)
                .contentTransition(.numericText(value: Double(value)))
                .lineLimit(1)
                .fixedSize()
                .padding(.horizontal, 5)
                .frame(minWidth: 18, minHeight: 18)
                .background(Capsule().fill(unread ? fm.accent : fm.count.opacity(0.16)))
                .padding(1.5)
                .background(Capsule().fill(fm.bgBase))
        } else {
            Text(verbatim: text)
                .fmText(.labelSmall, color: fm.count, weight: FmType.emphasis, tracking: 0, tabular: true)
                .contentTransition(.numericText(value: Double(value)))
                .lineLimit(1)
                .fixedSize()
                .frame(minWidth: 16, minHeight: 16)
        }
    }
}

struct ShellHeartbeatCount: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    static let crossing: Double = 225

    let count: Int

    static func sinceCrossing(_ t: Double) -> Double {
        let turn: Double = ShellActivityHalo.turn
        let head: Double = 360 * Double(ShellActivityHalo.arc)
        let spin: Double = t.truncatingRemainder(dividingBy: turn) / turn
        let lag: Double = (spin - (crossing - head) / 360).truncatingRemainder(dividingBy: 1)
        return (lag < 0 ? lag + 1 : lag) * turn
    }

    static func beat(_ since: Double) -> Double {
        let turn: Double = ShellActivityHalo.turn
        func bump(_ center: Double, _ width: Double) -> Double {
            let gap: Double = abs(since - center)
            let x: Double = max(0, 1 - min(gap, turn - gap) / width)
            return x * x * (3 - 2 * x)
        }
        return bump(0, 0.09) + 0.6 * bump(0.22, 0.09)
    }

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: reduceMotion)) { (context: TimelineViewDefaultContext) in
            let since: Double = ShellHeartbeatCount.sinceCrossing(context.date.timeIntervalSinceReferenceDate)
            let beat: Double = reduceMotion ? 0 : ShellHeartbeatCount.beat(since)
            let ripple: Double = reduceMotion ? 1 : min(1, since / 0.6)
            Text(verbatim: ShellBottomBar.badgeText(count))
                .fmText(.labelSmall, color: fm.onPrimary, weight: FmType.emphasis, tracking: 0, tabular: true)
                .contentTransition(.numericText(value: Double(count)))
                .lineLimit(1)
                .fixedSize()
                .padding(.horizontal, 4)
                .frame(minWidth: 18, minHeight: 18)
                .background {
                    ZStack {
                        Capsule()
                            .fill(fm.accent)
                            .shadow(color: fm.accent.opacity((fm.dark ? 0.45 : 0.25) + 0.4 * beat), radius: 3 + 3 * beat, y: 1)
                        Capsule()
                            .stroke(fm.accent.opacity(reduceMotion ? 0 : 0.6 * (1 - ripple)), lineWidth: 1.25)
                            .scaleEffect(1 + 0.7 * ripple)
                    }
                }
                .padding(1.5)
                .background(Capsule().fill(fm.bgBase))
                .scaleEffect(1 + 0.22 * beat)
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

struct ShellBarProgressRing: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    static let sweep: CGFloat = 0.28

    let progress: ShellBarProgress
    let size: CGFloat
    var lineWidth: CGFloat = 2

    var body: some View {
        TimelineView(
            .animation(minimumInterval: progress.estimated ? 0.5 : 1.0 / 30.0, paused: reduceMotion && !progress.estimated)
        ) { (context: TimelineViewDefaultContext) in
            let fraction: Double? = progress.fraction(nowMs: ShellBarProgress.nowMs(context.date))
            let t: Double = context.date.timeIntervalSinceReferenceDate
            let spin: Double = reduceMotion ? 0 : t.truncatingRemainder(dividingBy: ShellActivityHalo.turn) / ShellActivityHalo.turn
            ZStack {
                Circle()
                    .stroke(fm.accent.opacity(0.18), lineWidth: lineWidth)
                Circle()
                    .trim(from: 0, to: fraction.map { CGFloat($0) } ?? ShellBarProgressRing.sweep)
                    .stroke(fm.accent, style: StrokeStyle(lineWidth: lineWidth, lineCap: .round))
                    .rotationEffect(.degrees(fraction == nil ? 360 * spin - 90 : -90))
                    .animation(fraction == nil || reduceMotion ? nil : .linear(duration: 0.5), value: fraction)
            }
        }
        .frame(width: size, height: size)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

struct ShellActivityHalo: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    static let turn: Double = 1.6
    static let breath: Double = 2.4
    static let arc: CGFloat = 0.7

    let size: CGFloat

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: reduceMotion)) { (context: TimelineViewDefaultContext) in
            let t: Double = context.date.timeIntervalSinceReferenceDate
            let spin: Double = reduceMotion ? 0 : t.truncatingRemainder(dividingBy: ShellActivityHalo.turn) / ShellActivityHalo.turn
            let breath: Double = reduceMotion ? 1 : 0.5 + 0.5 * sin(t * 2 * .pi / ShellActivityHalo.breath)
            ZStack {
                Circle()
                    .fill(
                        RadialGradient(
                            colors: [fm.accent.opacity(0.32), fm.accent.opacity(0)], center: .center, startRadius: 0,
                            endRadius: size / 2)
                    )
                    .scaleEffect(0.85 + 0.2 * breath)
                    .opacity(0.6 + 0.4 * breath)
                Circle()
                    .stroke(fm.accent.opacity(0.14), lineWidth: 1.5)
                Circle()
                    .trim(from: 0, to: reduceMotion ? 1 : ShellActivityHalo.arc)
                    .stroke(
                        AngularGradient(
                            colors: [fm.accent.opacity(reduceMotion ? 1 : 0), fm.accent], center: .center, startAngle: .degrees(0),
                            endAngle: .degrees(360 * Double(ShellActivityHalo.arc))),
                        style: StrokeStyle(lineWidth: 1.75, lineCap: .round)
                    )
                    .rotationEffect(.degrees(360 * spin))
            }
        }
        .frame(width: size, height: size)
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }
}

struct ShellMenuDropDelegate: DropDelegate {
    @Binding var dragging: String?
    var onOther: (() -> Bool)? = nil
    let onEnter: (String) -> Void

    func validateDrop(info: DropInfo) -> Bool { dragging != nil || onOther != nil }

    func dropEntered(info: DropInfo) {
        guard let itemId = dragging else { return }
        onEnter(itemId)
    }

    func dropUpdated(info: DropInfo) -> DropProposal? {
        DropProposal(operation: .move)
    }

    func performDrop(info: DropInfo) -> Bool {
        if dragging == nil, let onOther = onOther {
            return onOther()
        }
        dragging = nil
        return true
    }
}

struct ShellBarDropDelegate: DropDelegate {
    let prefs: ShellBarPrefs
    let items: [ShellBarItem]
    let target: String?
    let isTargeted: Binding<Bool>?
    let onDrop: (ShellBarDrag) -> Bool

    func validateDrop(info: DropInfo) -> Bool { prefs.drag != nil }

    func dropEntered(info: DropInfo) {
        isTargeted?.wrappedValue = true
    }

    func dropExited(info: DropInfo) {
        isTargeted?.wrappedValue = false
    }

    func dropUpdated(info: DropInfo) -> DropProposal? {
        guard let drag = prefs.drag else { return DropProposal(operation: .cancel) }
        let fits: Bool = target != nil || drag.fromBar || prefs.inBar(drag.id, items: items) || !prefs.barIsFull(items)
        return DropProposal(operation: fits ? .move : .forbidden)
    }

    func performDrop(info: DropInfo) -> Bool {
        isTargeted?.wrappedValue = false
        guard let drag = prefs.drag else { return false }
        prefs.drag = nil
        return onDrop(drag)
    }
}

struct ShellBarButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .opacity(configuration.isPressed ? 0.7 : 1)
            .scaleEffect(configuration.isPressed ? 0.94 : 1)
    }
}
