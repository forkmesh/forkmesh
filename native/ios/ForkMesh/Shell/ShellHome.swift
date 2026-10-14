import FMApi
import FMBase
import SwiftUI
import UniformTypeIdentifiers

struct ShellHomeHold: Equatable {
    let tab: AppTab
    let path: [Route]
}

struct ShellHomeTile: Identifiable {
    let group: String
    let item: ShellBarItem

    var id: String { "\(group)/\(item.id)" }
}

struct ShellHomeLayout {
    let shown: [ShellMenuGroup]
    let barItems: [ShellBarItem]
    let docked: Set<String>
    let heat: [String: Double]
    let frequent: Set<String>
    let origins: [String: ShellMenuGroup]
    let tileHeight: CGFloat
    let tileScale: CGFloat
}

struct ShellHome: View {
    static let homeId: String = "home"

    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion
    @Environment(\.accessibilityVoiceOverEnabled) var voiceOver
    @Environment(\.displayScale) var displayScale

    let groups: [ShellMenuGroup]
    let prefs: ShellBarPrefs
    var onPackage: (String) -> Void = { (_: String) in }
    var onSearch: (String) -> Void = { (_: String) in }
    let onPick: (ShellBarItem) -> Void

    @State private var naming: ShellBarNaming?
    @State private var query: String = ""
    @FocusState private var searching: Bool
    @State private var viewHeight: CGFloat = 0
    @State private var searchBlockHeight: CGFloat = 0
    @State private var headerHeight: CGFloat = 0
    @State private var searchShown: Bool = false
    @State private var contentTop: CGFloat = 0
    @State private var settingsShown: Bool = false
    @State private var barSettingsShown: Bool = false

    @ScaledMetric(relativeTo: .body) private var iconSize: CGFloat = 22
    @ScaledMetric(relativeTo: .caption) private var labelSize: CGFloat = 12
    @ScaledMetric(relativeTo: .body) private var tileHeight: CGFloat = 36
    @ScaledMetric(relativeTo: .body) private var gridTileHeight: CGFloat = 66
    @ScaledMetric(relativeTo: .body) private var minimumTileHeight: CGFloat = 50
    @ScaledMetric(relativeTo: .caption) private var searchHeight: CGFloat = 26

    private var style: ShellHomeStyle { prefs.home }

    private var columns: [GridItem] {
        Array(repeating: GridItem(.flexible(), spacing: FmSpace.x0), count: style.across)
    }

    private var linked: [ShellMenuGroup] {
        ShellHome.linkGroups(groups)
    }

    private var shownGroups: [ShellMenuGroup] {
        shownGroups(linked)
    }

    private func shownGroups(_ linked: [ShellMenuGroup]) -> [ShellMenuGroup] {
        ShellHome.arranged(
            ShellHome.filtered(linked, query: query), style: style, uses: prefs.uses, recent: prefs.recent,
            searching: !trimmedQuery.isEmpty)
    }

    private var groupInfo: [ShellHomeGroupInfo] {
        linked.map { ShellHomeGroupInfo(id: $0.id, title: $0.title, count: $0.items.count) }
    }

    private var sectionLead: CGFloat {
        style.headed ? headerHeight : (style.grouped ? FmSpace.x2 : 0)
    }

    private var searchVisible: Bool {
        searchShown || voiceOver || !query.isEmpty
    }

    private func fittedTileHeight(_ shown: [ShellMenuGroup]) -> CGFloat {
        let across: Int = style.across
        let rows: Int = shown.reduce(0) { $0 + ($1.items.count + across - 1) / across }
        let fixed: CGFloat =
            (searchVisible ? searchBlockHeight : 0) + CGFloat(shown.count) * sectionLead + FmSpace.x3
        return CGFloat(
            ShellBarOrder.fittedTile(
                height: Double(viewHeight), fixed: Double(fixed), rows: rows, tile: Double(gridTileHeight),
                minimum: Double(minimumTileHeight)))
    }

    private func makeLayout() -> ShellHomeLayout {
        let linked: [ShellMenuGroup] = self.linked
        let shown: [ShellMenuGroup] = shownGroups(linked)
        let items: [ShellBarItem] = barItems
        let ids: [String] = linked.flatMap { $0.items.map(\.id) }
        let uses: [String: Int] = prefs.uses
        let most: Int = ShellBarOrder.mostUses(uses, among: ids)
        var heat: [String: Double] = [:]
        var frequent: Set<String> = []
        for id in Set(ids) {
            if style.highlightFrequent {
                heat[id] = ShellBarOrder.heat(id, uses: uses, most: most)
            }
            if ShellBarOrder.frequentMark(id, uses: uses, most: most) != nil {
                frequent.insert(id)
            }
        }
        var origins: [String: ShellMenuGroup] = [:]
        for group in linked {
            for item in group.items where origins[item.id] == nil {
                origins[item.id] = group
            }
        }
        let height: CGFloat = fittedTileHeight(shown)
        return ShellHomeLayout(
            shown: shown, barItems: items, docked: prefs.barIds(items), heat: heat, frequent: frequent,
            origins: origins, tileHeight: height, tileScale: gridTileHeight > 0 ? height / gridTileHeight : 1)
    }

    private var trimmedQuery: String {
        query.trimmingCharacters(in: .whitespacesAndNewlines)
    }

    var body: some View {
        let layout: ShellHomeLayout = makeLayout()
        return ScrollView {
            if searchVisible {
                searchField
                    .transition(.move(edge: .top).combined(with: .opacity))
            }
            LazyVGrid(columns: columns, alignment: .leading, spacing: FmSpace.x0) {
                ForEach(layout.shown) { (group: ShellMenuGroup) in
                    if style.headed {
                        Section {
                            tiles(group, layout: layout)
                        } header: {
                            sectionHeader(group)
                                .onDrop(of: [.text], delegate: drop(group.id, near: nil))
                        }
                    } else if style.grouped {
                        Section {
                            tiles(group, layout: layout)
                        } header: {
                            Color.clear
                                .frame(height: FmSpace.x2)
                                .onDrop(of: [.text], delegate: drop(group.id, near: nil))
                                .accessibilityHidden(true)
                        }
                    } else {
                        Section {
                            tiles(group, layout: layout)
                        }
                    }
                }
            }
            .padding(.horizontal, FmSpace.x1)
            .padding(.top, style.headed ? 0 : FmSpace.x2)
            .padding(.bottom, FmSpace.x3)
            .background(alignment: .top) { pullSensor }
            if layout.shown.isEmpty && trimmedQuery.isEmpty {
                allHidden
            }
            if !trimmedQuery.isEmpty {
                searchEverywhere
            }
        }
        .scrollBounceBehavior(.always, axes: .vertical)
        .onGeometryChange(for: CGFloat.self, of: { $0.frame(in: .global).minY + $0.safeAreaInsets.top }) {
            (top: CGFloat) in
            contentTop = top
        }
        .onGeometryChange(for: CGFloat.self, of: { $0.size.height - $0.safeAreaInsets.top - $0.safeAreaInsets.bottom }) {
            (height: CGFloat) in
            if !searching {
                viewHeight = height
            }
        }
        .scrollDismissesKeyboard(.interactively)
        .onDrop(of: [.text], isTargeted: nil) { (_: [NSItemProvider]) -> Bool in
            dropOnHome()
        }
        .onChange(of: searching) { (_: Bool, focused: Bool) in
            if !focused, trimmedQuery.isEmpty {
                hideSearch()
            }
        }
        .onChange(of: groupInfo, initial: true) { (_: [ShellHomeGroupInfo], next: [ShellHomeGroupInfo]) in
            prefs.homeGroups = next
        }
        .sheet(isPresented: $settingsShown) {
            ShellHomeSettingsSheet(prefs: prefs)
        }
        .sheet(isPresented: $barSettingsShown) {
            ShellBarSettingsSheet(prefs: prefs)
        }
        .onAppear { prefs.drag = nil }
        .onDisappear {
            prefs.drag = nil
            if trimmedQuery.isEmpty {
                searchShown = false
            }
        }
        .dynamicTypeSize(...DynamicTypeSize.accessibility2)
        .accessibilityIdentifier("shell-home")
        .shellBarNaming($naming, prefs: prefs)
    }

    private func tiles(_ group: ShellMenuGroup, layout: ShellHomeLayout) -> some View {
        ForEach(ShellHome.tiles(group.items, in: group.id)) { (entry: ShellHomeTile) in
            tile(entry.item, group: entry.group, layout: layout)
        }
    }

    private var allHidden: some View {
        VStack(spacing: FmSpace.x3) {
            Image(systemName: "square.grid.3x3.square")
                .font(.system(size: 34, weight: .light))
                .symbolRenderingMode(.hierarchical)
                .foregroundStyle(fm.textTertiary)
            Text("Every category is hidden")
                .fmText(.titleSmall, color: fm.textSecondary)
            Button {
                FmHaptics.selection()
                settingsShown = true
            } label: {
                SwiftUI.Label("Home settings", systemImage: "slider.horizontal.3")
                    .fmText(.bodyMedium, color: fm.accent)
            }
            .buttonStyle(.plain)
            .accessibilityIdentifier("shell-home-all-hidden-settings")
        }
        .frame(maxWidth: .infinity)
        .padding(.vertical, FmSpace.x6)
    }

    @ViewBuilder private func homeMenuItems(hiding group: ShellMenuGroup?) -> some View {
        if let group = group {
            Button {
                withAnimation(reduceMotion ? nil : .snappy) {
                    prefs.styleHome { $0.setShown(group.id, false) }
                }
            } label: {
                SwiftUI.Label("Hide \(group.title)", systemImage: "eye.slash")
            }
            .accessibilityIdentifier("shell-home-hide-category")
        }
        Button {
            settingsShown = true
        } label: {
            SwiftUI.Label("Home settings…", systemImage: "slider.horizontal.3")
        }
        .accessibilityIdentifier("shell-home-open-settings")
    }

    private func sectionHeader(_ group: ShellMenuGroup) -> some View {
        let title: String = group.title
        return HStack(spacing: FmSpace.x2) {
            Text(title.uppercased())
                .fmText(.labelSmall, color: fm.textTertiary, weight: FmType.emphasis)
                .lineLimit(1)
                .fixedSize()
            Capsule()
                .fill(fm.border)
                .frame(height: max(1, 1 / displayScale))
                .accessibilityHidden(true)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.top, FmSpace.x2)
        .padding(.bottom, FmSpace.x2)
        .padding(.horizontal, FmSpace.x2)
        .onGeometryChange(for: CGFloat.self, of: { $0.size.height }) { (height: CGFloat) in
            headerHeight = height
        }
        .contentShape(Rectangle())
        .contextMenu {
            homeMenuItems(hiding: group)
        }
        .accessibilityElement(children: .combine)
        .accessibilityAddTraits(.isHeader)
    }

    private var dragging: Binding<String?> {
        Binding(
            get: { prefs.drag?.fromBar == false ? prefs.drag?.id : nil },
            set: { (next: String?) in prefs.drag = next.map { ShellBarDrag(id: $0, fromBar: false) } })
    }

    private func dropOnHome() -> Bool {
        guard let drag = prefs.drag else { return true }
        prefs.drag = nil
        guard drag.fromBar, !ShellBarPrefs.keep.contains(drag.id) else { return true }
        withAnimation(reduceMotion ? nil : .snappy) {
            prefs.removeFromBar([drag.id], items: barItems)
        }
        FmHaptics.success()
        return true
    }

    private func drop(_ group: String, near target: String?) -> ShellMenuDropDelegate {
        ShellMenuDropDelegate(
            dragging: dragging,
            onOther: { dropOnHome() },
            onEnter: { (itemId: String) in
                guard itemId != target, prefs.home.reorders else { return }
                let moved: Bool = withAnimation(reduceMotion ? nil : .snappy) {
                    prefs.moveInMenu(itemId, to: group, near: target, groups: groups)
                }
                if moved {
                    if prefs.home.sort != .custom {
                        withAnimation(reduceMotion ? nil : .snappy) {
                            prefs.styleHome { $0.sort = .custom }
                        }
                    }
                    FmHaptics.selection()
                }
            })
    }

    private var barItems: [ShellBarItem] {
        groups.flatMap(\.items)
    }

    private var pullSensor: some View {
        Color.clear
            .frame(height: 0)
            .onGeometryChange(for: CGFloat.self, of: { $0.frame(in: .global).minY }) { (top: CGFloat) in
                if ShellBarOrder.revealsSearch(pulled: Double(top - contentTop), shown: searchVisible) {
                    showSearch()
                }
            }
            .accessibilityHidden(true)
    }

    private func showSearch() {
        guard !searchShown else {
            searching = true
            return
        }
        FmHaptics.selection()
        withAnimation(reduceMotion ? nil : .snappy) {
            searchShown = true
        }
        DispatchQueue.main.async {
            searching = true
        }
    }

    private func hideSearch() {
        guard searchShown else { return }
        withAnimation(reduceMotion ? nil : .snappy) {
            searchShown = false
        }
    }

    private var searchField: some View {
        HStack(spacing: FmSpace.x1) {
            HStack(spacing: FmSpace.x1) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 11, weight: .medium))
                    .foregroundStyle(fm.textTertiary)
                    .accessibilityHidden(true)
                TextField("", text: $query, prompt: Text("Search").foregroundColor(fm.textTertiary))
                    .fmText(.bodySmall)
                    .textInputAutocapitalization(.never)
                    .autocorrectionDisabled()
                    .submitLabel(.search)
                    .focused($searching)
                    .onSubmit { submit() }
                    .accessibilityLabel(Text("Search"))
                    .accessibilityIdentifier("shell-home-search")
            }
            .padding(.leading, FmSpace.x2)
            .frame(maxHeight: .infinity)
            .contentShape(Rectangle())
            .onTapGesture { searching = true }
            if query.isEmpty {
                Menu {
                    Section {
                        Button {
                            settingsShown = true
                        } label: {
                            SwiftUI.Label("Home settings…", systemImage: "square.grid.3x3")
                        }
                        .accessibilityIdentifier("shell-home-settings-sheet")
                    }
                    Section("Bottom bar") {
                        Button {
                            barSettingsShown = true
                        } label: {
                            SwiftUI.Label("Bottom bar settings…", systemImage: "dock.rectangle")
                        }
                        .accessibilityIdentifier("shell-home-bar-settings")
                        ShellBarSettingsItems(prefs: prefs)
                    }
                } label: {
                    Image(systemName: "slider.horizontal.3")
                        .font(.system(size: 11, weight: .medium))
                        .foregroundStyle(fm.textTertiary)
                        .frame(width: 32, height: searchHeight)
                        .contentShape(Rectangle())
                }
                .accessibilityLabel(Text("Icon settings"))
                .accessibilityIdentifier("shell-home-settings")
            } else {
                Button {
                    query = ""
                    if !searching {
                        hideSearch()
                    }
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.system(size: 12))
                        .foregroundStyle(fm.textTertiary)
                        .frame(width: 32, height: searchHeight)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel(Text("Clear search"))
                .accessibilityIdentifier("shell-home-search-clear")
            }
        }
        .frame(height: searchHeight)
        .shellGlass(Capsule(), interactive: false)
        .genieKeepsClear()
        .padding(.horizontal, FmSpace.x2)
        .padding(.top, FmSpace.x1)
        .padding(.bottom, FmSpace.x1)
        .onGeometryChange(for: CGFloat.self, of: { $0.size.height }) { (height: CGFloat) in
            searchBlockHeight = height
        }
    }

    private var searchEverywhere: some View {
        Button {
            FmHaptics.selection()
            searching = false
            onSearch(trimmedQuery)
        } label: {
            HStack(spacing: FmSpace.x2) {
                Image(systemName: "magnifyingglass")
                    .font(.system(size: 12, weight: .medium))
                Text("Search everywhere for \u{201C}\(trimmedQuery)\u{201D}")
                    .fmText(.bodySmall, color: fm.accent)
                    .lineLimit(1)
                Spacer(minLength: 0)
            }
            .foregroundStyle(fm.accent)
            .padding(.horizontal, FmSpace.x3)
            .frame(height: tileHeight)
            .background(RoundedRectangle(cornerRadius: FmRadius.md).fill(fm.bgOverlay))
            .contentShape(RoundedRectangle(cornerRadius: FmRadius.md))
        }
        .buttonStyle(ShellBarButtonStyle())
        .padding(.horizontal, FmSpace.x2)
        .padding(.bottom, FmSpace.x3)
        .accessibilityIdentifier("shell-home-search-everywhere")
    }

    private func submit() {
        guard !trimmedQuery.isEmpty else { return }
        searching = false
        if let first = shownGroups.first?.items.first {
            FmHaptics.selection()
            onPick(first)
        } else {
            onSearch(trimmedQuery)
        }
    }

    static func tiles(_ items: [ShellBarItem], in group: String) -> [ShellHomeTile] {
        items.map { ShellHomeTile(group: group, item: $0) }
    }

    static func arranged(
        _ groups: [ShellMenuGroup], style: ShellHomeStyle, uses: [String: Int], recent: [String], searching: Bool
    ) -> [ShellMenuGroup] {
        var byId: [String: ShellBarItem] = [:]
        for group in groups {
            for item in group.items where byId[item.id] == nil {
                byId[item.id] = item
            }
        }
        let sections: [ShellHomeSection] = groups.map { (group: ShellMenuGroup) -> ShellHomeSection in
            ShellHomeSection(
                id: group.id, title: group.title,
                entries: group.items.map { (item: ShellBarItem) -> ShellHomeEntry in
                    ShellHomeEntry(id: item.id, title: item.title, badge: item.badge, count: item.count, active: item.active)
                })
        }
        return style.arranged(sections, uses: uses, recent: recent, searching: searching).map {
            (section: ShellHomeArranged) -> ShellMenuGroup in
            ShellMenuGroup(id: section.id, title: section.title, items: section.ids.compactMap { byId[$0] })
        }
    }

    static func linkGroups(_ groups: [ShellMenuGroup]) -> [ShellMenuGroup] {
        groups.compactMap { (group: ShellMenuGroup) -> ShellMenuGroup? in
            let items: [ShellBarItem] = group.items.filter { $0.id != ShellHome.homeId }
            return items.isEmpty ? nil : ShellMenuGroup(id: group.id, title: group.title, items: items)
        }
    }

    static func filtered(_ groups: [ShellMenuGroup], query: String) -> [ShellMenuGroup] {
        let needle: String = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !needle.isEmpty else { return groups }
        return groups.compactMap { (group: ShellMenuGroup) -> ShellMenuGroup? in
            let items: [ShellBarItem] =
                group.title.localizedStandardContains(needle)
                ? group.items : group.items.filter { $0.title.localizedStandardContains(needle) }
            return items.isEmpty ? nil : ShellMenuGroup(id: group.id, title: group.title, items: items)
        }
    }

    private func tile(_ item: ShellBarItem, group: String, layout: ShellHomeLayout) -> some View {
        var shown: ShellBarItem = item
        shown.flightTarget = false
        let docked: Bool = layout.docked.contains(item.id)
        let uses: Int = prefs.uses[item.id] ?? 0
        let heat: Double = layout.heat[item.id] ?? 0
        let frequent: Bool = layout.frequent.contains(item.id)
        let icon: CGFloat = iconSize * min(1, layout.tileScale)
        return Button {
            FmHaptics.selection()
            prefs.drag = nil
            onPick(item)
        } label: {
            ShellHomeIcon(
                item: shown, style: style.icons, iconSize: icon, labelSize: labelSize, showsCounts: style.showCounts,
                heat: heat
            )
            .padding(.horizontal, 2)
            .frame(maxWidth: .infinity)
            .frame(height: layout.tileHeight)
            .contentShape(RoundedRectangle(cornerRadius: FmRadius.md))
        }
        .buttonStyle(ShellHomeButtonStyle())
        .opacity(prefs.drag == ShellBarDrag(id: item.id, fromBar: false) ? 0.35 : 1)
        .contextMenu {
            ShellBarItemMenu(
                item: item, items: layout.barItems, prefs: prefs, inBar: false,
                onName: { (next: ShellBarNaming) in naming = next }, onPackage: onPackage)
            Section {
                homeMenuItems(hiding: layout.origins[item.id])
            }
        }
        .onDrag {
            prefs.drag = ShellBarDrag(id: item.id, fromBar: false)
            return NSItemProvider(object: item.id as NSString)
        } preview: {
            ShellHomeIcon(
                item: shown, style: style.icons, iconSize: iconSize, labelSize: labelSize, showsCounts: style.showCounts)
                .padding(FmSpace.x3)
                .background(RoundedRectangle(cornerRadius: FmRadius.lg, style: .continuous).fill(fm.bgOverlay))
        }
        .onDrop(of: [.text], delegate: drop(group, near: item.id))
        .accessibilityLabel(Text(item.title))
        .accessibilityValue(Text(ShellHome.value(item, docked: docked, uses: uses, frequent: frequent)))
        .accessibilityAddTraits(item.selected ? [.isSelected, .isButton] : [.isButton])
        .accessibilityIdentifier("shell-home-\(group)-\(item.id)")
        .genieKeepsClear()
    }

    static func value(_ item: ShellBarItem, docked: Bool, uses: Int = 0, frequent: Bool = false) -> String {
        let parts: [String] = [
            docked ? "In the bar" : "",
            item.active ? "Active" : "",
            item.badge > 0
                ? "\(ShellBottomBar.countText(item.badge)) new"
                : item.count.flatMap { $0 > 0 ? ShellBottomBar.countText($0) : nil } ?? "",
            uses > 1 ? "Opened \(uses) times" : (uses == 1 ? "Opened once" : ""),
            frequent ? "Used often" : "",
        ]
        return parts.filter { !$0.isEmpty }.joined(separator: ", ")
    }
}

struct ShellHomeButtonStyle: ButtonStyle {
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .opacity(configuration.isPressed ? 0.75 : 1)
            .scaleEffect(configuration.isPressed ? 0.9 : 1)
            .animation(reduceMotion ? nil : .spring(response: 0.25, dampingFraction: 0.6), value: configuration.isPressed)
    }
}
