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

    init() {
        currentRepo = CodeCurrentRepo.memory.ref
    }

    func adopt(_ repo: RepoRef, counts: [RepoSection: Int]) {
        let slug: String = repo.slug.lowercased()
        repoCounts = slug == repoCountsSlug ? repoCounts.merging(counts) { $1 } : counts
        repoCountsSlug = slug
    }

    func repoCount(_ section: RepoSection) -> Int? {
        guard let repo = currentRepo, repo.slug.lowercased() == repoCountsSlug else { return nil }
        return repoCounts[section]
    }

    func knowsCounts(of repo: RepoRef) -> Bool {
        repo.slug.lowercased() == repoCountsSlug
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

extension EnvironmentValues {
    var shellStackTab: AppTab? {
        get { self[ShellStackTabKey.self] }
        set { self[ShellStackTabKey.self] = newValue }
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
    }

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
        layout.barSlots(items, ranking: ranking, keep: ShellBarPrefs.keep, id: \.id).map { ShellBarEntry(slot: $0) }
    }

    func inBar(_ itemId: String, items: [ShellBarItem]) -> Bool {
        layout.inBar(itemId, items: items, ranking: ranking, keep: ShellBarPrefs.keep, id: \.id)
    }

    func barIsFull(_ items: [ShellBarItem]) -> Bool {
        layout.barIsFull(items, ranking: ranking, keep: ShellBarPrefs.keep, id: \.id)
    }

    func removeFromBar(_ ids: [String], items: [ShellBarItem]) {
        let current: [String] = ranking
        arrange { $0.removeFromBar(ids, items: items, ranking: current, keep: ShellBarPrefs.keep, id: \.id) }
    }

    @discardableResult
    func putInBar(_ itemId: String, at target: String? = nil, items: [ShellBarItem]) -> Bool {
        let current: [String] = ranking
        var next: ShellBarLayout = layout
        guard next.putInBar(itemId, at: target, items: items, ranking: current, keep: ShellBarPrefs.keep, id: \.id)
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
        next.fit(items, ranking: ranking, keep: ShellBarPrefs.keep, id: \.id)
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
    var action: () -> Void
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

struct ShellBottomBar: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let items: [ShellBarItem]
    var onPackage: (String) -> Void = { (_: String) in }
    var onCapsuleTop: (CGFloat) -> Void = { (_: CGFloat) in }

    @State private var prefs: ShellBarPrefs = ShellBarPrefs.shared
    @State private var naming: ShellBarNaming?
    @State private var targeted: Bool = false
    @State private var barWidth: CGFloat = 0

    @ScaledMetric(relativeTo: .body) private var iconSize: CGFloat = 20
    @ScaledMetric(relativeTo: .body) private var cellWidth: CGFloat = 64
    @ScaledMetric(relativeTo: .body) private var cellHeight: CGFloat = 52

    var body: some View {
        container {
            VStack(spacing: 0) {
                glassCapsule(
                    carousel
                        .clipShape(Capsule())
                )
                .overlay {
                    if targeted {
                        Capsule().strokeBorder(fm.accent, lineWidth: 1.5)
                            .allowsHitTesting(false)
                    }
                }
                .onDrop(of: [.text], delegate: drop(nil, isTargeted: $targeted))
                .frame(maxWidth: .infinity)
                .onGeometryChange(for: CGFloat.self, of: { $0.frame(in: .named(ShellDock.barSpace)).minY }) {
                    (top: CGFloat) in
                    onCapsuleTop(top)
                }
            }
        }
        .padding(.horizontal, FmSpace.x3)
        .padding(.top, FmSpace.x1)
        .dynamicTypeSize(...DynamicTypeSize.accessibility2)
        .accessibilityElement(children: .contain)
        .accessibilityIdentifier("shell-bottom-bar")
        .shellBarNaming($naming, prefs: prefs)
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

    private var carousel: some View {
        ScrollViewReader { (proxy: ScrollViewProxy) in
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 2) {
                    ForEach(shown) { (entry: ShellBarEntry) in
                        slot(entry)
                            .id(entry.id)
                    }
                }
                .padding(4)
            }
            .onGeometryChange(for: CGFloat.self, of: { $0.size.width }) { (width: CGFloat) in
                barWidth = width
            }
            .scrollBounceBehavior(.basedOnSize, axes: .horizontal)
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

    private var slotWidth: CGFloat {
        let count: CGFloat = CGFloat(max(1, shown.count))
        guard barWidth > 0 else { return cellWidth }
        return max(44, min(cellWidth, (barWidth - 8 - 2 * (count - 1)) / count))
    }

    private var selectedId: String? {
        guard let item = items.first(where: { $0.selected }) else { return nil }
        return shown.first { $0.contains(item.id) }?.id
    }

    @ViewBuilder private func slot(_ entry: ShellBarEntry) -> some View {
        switch entry.slot {
        case let .item(item):
            cell(item, shape: selectionShape(entry.id))
                .contextMenu {
                    ShellBarItemMenu(
                        item: item, items: items, prefs: prefs,
                        onName: { (next: ShellBarNaming) in naming = next }, onPackage: onPackage)
                }
                .onDrag {
                    prefs.drag = ShellBarDrag(id: item.id, fromBar: true)
                    return NSItemProvider(object: item.id as NSString)
                } preview: {
                    ShellBarCellLabel(item: item, iconSize: iconSize)
                        .frame(width: slotWidth, height: cellHeight)
                        .background(Capsule().fill(fm.bgOverlay))
                }
                .onDrop(of: [.text], delegate: drop(entry.id, isTargeted: nil))
        case let .folder(folder, members):
            ShellBarFolderCell(
                folder: folder, members: members, items: items, prefs: prefs, iconSize: iconSize,
                cellWidth: slotWidth, cellHeight: cellHeight, shape: selectionShape(entry.id),
                onName: { (next: ShellBarNaming) in naming = next }, onPackage: onPackage)
                .onDrop(of: [.text], delegate: drop(entry.id, isTargeted: nil))
        }
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
        return new.isEmpty ? "Active" : "Active, " + new
    }

    static func countText(_ count: Int) -> String {
        return count > 9999 ? "\(count / 1000)k" : count.formatted()
    }

    static let innerRadius: CGFloat = 10

    private func selectionShape(_ id: String) -> UnevenRoundedRectangle {
        let outer: CGFloat = cellHeight / 2
        let inner: CGFloat = ShellBottomBar.innerRadius
        let leading: CGFloat = shown.first?.id == id ? outer : inner
        let trailing: CGFloat = shown.last?.id == id ? outer : inner
        return UnevenRoundedRectangle(
            topLeadingRadius: leading, bottomLeadingRadius: leading, bottomTrailingRadius: trailing,
            topTrailingRadius: trailing, style: .continuous)
    }

    private func cell(_ item: ShellBarItem, shape: UnevenRoundedRectangle) -> some View {
        Button {
            FmHaptics.selection()
            prefs.drag = nil
            ShellBottomBar.run(item, prefs: prefs, reduceMotion: reduceMotion)
        } label: {
            ShellBarCellLabel(item: item, iconSize: iconSize)
                .frame(width: slotWidth, height: cellHeight)
                .background {
                    if item.selected {
                        shape.fill(fm.accent.opacity(0.14))
                    }
                }
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
            GlassEffectContainer(spacing: FmSpace.x3) {
                content()
            }
        } else {
            content()
        }
        #else
        content()
        #endif
    }

    @ViewBuilder private func glassCapsule<Content: View>(_ content: Content) -> some View {
        #if compiler(>=6.2)
        if #available(iOS 26.0, *) {
            content.glassEffect(.regular.interactive(), in: Capsule())
        } else {
            materialCapsule(content)
        }
        #else
        materialCapsule(content)
        #endif
    }

    private func materialCapsule<Content: View>(_ content: Content) -> some View {
        content
            .background(Capsule().fill(.ultraThinMaterial))
            .overlay(Capsule().strokeBorder(fm.border.opacity(0.6), lineWidth: 0.5))
            .shadow(color: Color.black.opacity(0.12), radius: 12, y: 4)
    }
}

struct ShellBarCellLabel: View {
    @Environment(\.fmPalette) var fm

    let item: ShellBarItem
    let iconSize: CGFloat
    var showsCounts: Bool = false
    var spacing: CGFloat = 3
    var minimumScale: CGFloat = 0.75
    var pillCounts: Bool = false

    private var badgeText: String {
        showsCounts ? ShellBottomBar.countText(item.badge) : ShellBottomBar.badgeText(item.badge)
    }

    var body: some View {
        VStack(spacing: spacing) {
            Image(systemName: item.symbol)
                .font(.system(size: iconSize, weight: item.selected ? .semibold : .regular))
                .foregroundStyle(item.selected || item.active ? fm.accent : fm.textPrimary)
                .symbolEffect(.bounce, value: item.pulse)
                .frame(height: iconSize + 4)
                .background {
                    if item.active {
                        ShellActivityHalo(size: iconSize + 10)
                            .transition(.scale.combined(with: .opacity))
                    }
                }
                .onGeometryChange(for: CGRect.self, of: { $0.frame(in: .global) }) { (frame: CGRect) in
                    if item.flightTarget {
                        ShellLaunchFlight.shared.target = frame
                    }
                }
                .overlay(alignment: .topTrailing) {
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
                .overlay(alignment: .topLeading) {
                    if item.live > 0 {
                        ShellHeartbeatCount(count: item.live)
                            .alignmentGuide(.leading) { (d: ViewDimensions) in d[.trailing] }
                            .offset(x: 2, y: -6)
                            .transition(.scale.combined(with: .opacity))
                    }
                }
            Text(item.title)
                .font(.system(size: 10, weight: item.selected ? .semibold : .medium))
                .foregroundStyle(item.selected ? fm.accent : fm.textSecondary)
                .lineLimit(1)
                .minimumScaleFactor(minimumScale)
        }
    }

    @ViewBuilder private func countMark(_ text: String, value: Int, unread: Bool) -> some View {
        if pillCounts {
            Text(verbatim: text)
                .font(.system(.caption2, design: .rounded, weight: .bold).monospacedDigit())
                .foregroundStyle(unread ? Color.white : fm.count)
                .contentTransition(.numericText(value: Double(value)))
                .lineLimit(1)
                .fixedSize()
                .padding(.horizontal, 5)
                .frame(minWidth: 18, minHeight: 18)
                .background(Capsule().fill(unread ? fm.count : fm.count.opacity(0.16)))
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

    static let period: Double = 1.1

    let count: Int

    static func beat(_ phase: Double) -> Double {
        func bump(_ center: Double, _ width: Double) -> Double {
            let x: Double = max(0, 1 - abs(phase - center) / width)
            return x * x * (3 - 2 * x)
        }
        return bump(0.07, 0.07) + 0.6 * bump(0.25, 0.08)
    }

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0, paused: reduceMotion)) { (context: TimelineViewDefaultContext) in
            let t: Double = context.date.timeIntervalSinceReferenceDate
            let phase: Double = t.truncatingRemainder(dividingBy: ShellHeartbeatCount.period) / ShellHeartbeatCount.period
            let beat: Double = reduceMotion ? 0 : ShellHeartbeatCount.beat(phase)
            let ripple: Double = reduceMotion ? 1 : min(1, phase / 0.6)
            Text(verbatim: ShellBottomBar.badgeText(count))
                .fmText(.labelSmall, color: fm.count, weight: FmType.emphasis, tracking: 0, tabular: true)
                .contentTransition(.numericText(value: Double(count)))
                .lineLimit(1)
                .fixedSize()
                .frame(minWidth: 16, minHeight: 16)
                .background {
                    ZStack {
                        Circle()
                            .fill(fm.count.opacity(0.12 + 0.14 * beat))
                        Circle()
                            .stroke(fm.count.opacity(reduceMotion ? 0 : 0.5 * (1 - ripple)), lineWidth: 1.25)
                            .scaleEffect(1 + 0.7 * ripple)
                    }
                    .frame(width: 18, height: 18)
                }
                .scaleEffect(1 + 0.22 * beat)
        }
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
