import FMApi
import FMBase
import SwiftUI

struct ShellHomeSettingsSheet: View {
    @Environment(\.dismiss) private var dismiss

    let prefs: ShellBarPrefs

    var body: some View {
        NavigationStack {
            ShellHomeSettingsForm(prefs: prefs)
                .toolbar {
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Done") { dismiss() }
                            .accessibilityIdentifier("shell-home-settings-done")
                    }
                }
        }
        .presentationDetents([.medium, .large])
        .presentationDragIndicator(.visible)
    }
}

struct ShellHomeSettingsForm: View {
    @Environment(\.fmPalette) var fm
    @Environment(\.accessibilityReduceMotion) var reduceMotion

    let prefs: ShellBarPrefs

    @ScaledMetric(relativeTo: .body) private var iconSize: CGFloat = 22
    @ScaledMetric(relativeTo: .caption) private var labelSize: CGFloat = 12

    private var style: ShellHomeStyle { prefs.home }

    private var categories: [ShellHomeGroupInfo] {
        var known: [ShellHomeGroupInfo] = prefs.homeGroups
        if known.isEmpty {
            known = [ShellHomeGroupInfo(id: "main", title: "Go to", count: 0)]
            known += HubGroups.current(guest: false).map { (group: HubGroup) -> ShellHomeGroupInfo in
                ShellHomeGroupInfo(id: group.id, title: group.title, count: group.items.count)
            }
        }
        let ids: Set<String> = Set(known.map(\.id))
        let missing: [ShellHomeGroupInfo] = style.hidden.filter { !ids.contains($0) }.map {
            ShellHomeGroupInfo(id: $0, title: $0 == "repo" ? "Repository" : $0.capitalized, count: 0)
        }
        return known + missing
    }

    private func edit(_ change: @escaping (inout ShellHomeStyle) -> Void) {
        FmHaptics.selection()
        withAnimation(reduceMotion ? nil : .snappy) {
            prefs.styleHome(change)
        }
    }

    private func binding<Value>(_ path: WritableKeyPath<ShellHomeStyle, Value>) -> Binding<Value> {
        Binding<Value>(
            get: { prefs.home[keyPath: path] },
            set: { (next: Value) in edit { $0[keyPath: path] = next } })
    }

    var body: some View {
        Form {
            lookSection
            sortSection
            categorySection
            highlightSection
            barSection
            resetSection
        }
        .navigationTitle("Home screen")
        .navigationBarTitleDisplayMode(.inline)
        .tint(fm.accent)
        .accessibilityIdentifier("shell-home-settings")
    }

    private var lookSection: some View {
        Section {
            ShellHomeIconPreview(
                style: style.icons, iconSize: iconSize, labelSize: labelSize, showsCounts: style.showCounts,
                highlight: style.highlightFrequent)
                .listRowBackground(fm.bgBase)
            Picker("Icon style", selection: binding(\.icons)) {
                ForEach(ShellHomeIconStyle.allCases, id: \.self) { (icons: ShellHomeIconStyle) in
                    Text(icons.title).tag(icons)
                }
            }
            .pickerStyle(.segmented)
            .accessibilityIdentifier("shell-home-icon-style")
            Picker("Icons per row", selection: binding(\.across)) {
                ForEach(ShellHomeStyle.acrossChoices, id: \.self) { (across: Int) in
                    Text(verbatim: "\(across)").tag(across)
                }
            }
            .accessibilityIdentifier("shell-home-across")
        } header: {
            Text("Look")
        } footer: {
            Text("Outline and Filled keep the icons plain, Accent draws them in your accent colour and Tiles sets each one on a rounded tile.")
        }
    }

    private var sortSection: some View {
        Section {
            Picker(selection: binding(\.sort)) {
                ForEach(ShellHomeSort.allCases, id: \.self) { (sort: ShellHomeSort) in
                    SwiftUI.Label(sort.title, systemImage: sort.symbol)
                        .tag(sort)
                        .accessibilityIdentifier("shell-home-sort-\(sort.rawValue)")
                }
            } label: {
                Text("Sort icons")
            }
            .pickerStyle(.inline)
            .labelsHidden()
            .accessibilityIdentifier("shell-home-sort")
        } header: {
            Text("Sort icons")
        } footer: {
            Text(style.grouped
                ? "Icons are sorted inside each category. Dragging an icon on Home switches back to Custom order."
                : "Icons are sorted across all categories.")
        }
    }

    private var categorySection: some View {
        Section {
            Toggle(isOn: binding(\.grouped)) {
                SwiftUI.Label("Group by category", systemImage: "square.grid.3x1.below.line.grid.1x2")
            }
            .accessibilityIdentifier("shell-home-grouped")
            Toggle(isOn: binding(\.showTitles)) {
                SwiftUI.Label("Show category titles", systemImage: "textformat.size.smaller")
            }
            .disabled(!style.grouped)
            .accessibilityIdentifier("shell-home-titles")
            Picker(selection: binding(\.groupOrder)) {
                ForEach(ShellHomeGroupOrder.allCases, id: \.self) { (order: ShellHomeGroupOrder) in
                    SwiftUI.Label(order.title, systemImage: order.symbol).tag(order)
                }
            } label: {
                SwiftUI.Label("Category order", systemImage: "arrow.up.arrow.down")
            }
            .pickerStyle(.menu)
            .accessibilityIdentifier("shell-home-group-order")
            ForEach(categories) { (group: ShellHomeGroupInfo) in
                Toggle(
                    isOn: Binding<Bool>(
                        get: { prefs.home.shows(group.id) },
                        set: { (on: Bool) in edit { $0.setShown(group.id, on) } })
                ) {
                    HStack(spacing: FmSpace.x3) {
                        Image(systemName: ShellHomeGroupInfo.symbol(group.id))
                            .symbolRenderingMode(.hierarchical)
                            .foregroundStyle(style.shows(group.id) ? fm.accent : fm.textTertiary)
                            .frame(width: 24)
                        Text(group.title)
                            .foregroundStyle(style.shows(group.id) ? fm.textPrimary : fm.textTertiary)
                        Spacer(minLength: FmSpace.x2)
                        if group.count > 0 {
                            Text(verbatim: "\(group.count)")
                                .font(.system(.footnote, design: .rounded).monospacedDigit())
                                .foregroundStyle(fm.textTertiary)
                        }
                    }
                }
                .accessibilityIdentifier("shell-home-category-\(group.id)")
            }
        } header: {
            Text("Categories")
        } footer: {
            Text("A hidden category leaves Home, but its icons still show when you search Home and stay in the bottom bar. Touch and hold a category title on Home to hide it from there.")
        }
    }

    private var highlightSection: some View {
        Section {
            Toggle(isOn: binding(\.highlightFrequent)) {
                SwiftUI.Label("Highlight frequently used", systemImage: "sparkle")
            }
            .accessibilityIdentifier("shell-home-highlight")
            Toggle(isOn: binding(\.showCounts)) {
                SwiftUI.Label("Show counts", systemImage: "number.circle")
            }
            .accessibilityIdentifier("shell-home-counts")
        } header: {
            Text("Highlights")
        } footer: {
            Text("Frequently used icons get a soft glow that grows with use. Unread badges always show, even with counts off.")
        }
    }

    private var barSection: some View {
        Section {
            NavigationLink {
                ShellBarSettingsForm(prefs: prefs)
                    .shellPageClearance()
            } label: {
                SwiftUI.Label("Bottom bar", systemImage: "dock.rectangle")
            }
            .accessibilityIdentifier("shell-home-bar-settings-link")
        } footer: {
            Text("Shape, glass, icon style and size, labels and how the selected icon is marked.")
        }
    }

    private var resetSection: some View {
        Section {
            Button {
                FmHaptics.selection()
                withAnimation(reduceMotion ? nil : .snappy) {
                    prefs.clear()
                }
            } label: {
                SwiftUI.Label("Clear usage history", systemImage: "arrow.counterclockwise")
            }
            .disabled(prefs.recent.isEmpty && prefs.uses.isEmpty)
            .accessibilityIdentifier("shell-home-clear-usage")
            Button(role: .destructive) {
                withAnimation(reduceMotion ? nil : .snappy) {
                    prefs.resetMenu()
                }
            } label: {
                SwiftUI.Label("Reset Home order", systemImage: "square.grid.3x3")
            }
            .disabled(prefs.menu.isEmpty)
            .accessibilityIdentifier("shell-home-reset-order")
            Button(role: .destructive) {
                edit { $0 = ShellHomeStyle() }
            } label: {
                SwiftUI.Label("Reset Home settings", systemImage: "arrow.uturn.backward")
            }
            .disabled(style.isDefault)
            .accessibilityIdentifier("shell-home-reset-style")
        }
    }
}

struct HomeScreenSettingsCard: View {
    @Environment(\.fmPalette) var fm

    var body: some View {
        NavigationLink {
            ShellHomeSettingsForm(prefs: ShellBarPrefs.shared)
                .shellPageClearance()
        } label: {
            SettingsLinkRow(
                icon: "grid_view_outlined", title: "Home screen", subtitle: "Icon style, sorting, categories and highlights")
        }
        .buttonStyle(FmPressStyle(highlight: fm.accentSubtle))
        .accessibilityIdentifier("settings-home-screen")
    }
}
